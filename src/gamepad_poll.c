/*
 * gamepad_poll.c -- XInput polling + edge-detection state machine.
 *
 * Derived from iw3sp_mod by JerryALT
 *   https://gitea.com/JerryALT/iw3sp_mod
 *   Licensed under GPL-3.0
 *
 * Ported to CoD4x multiplayer by Ab7i
 *   https://github.com/Ab7i
 *   Licensed under AGPL-3.0 (compatible with GPL-3.0)
 *
 * iw3sp_mod itself is derived from IW4x Client.
 * Original Gamepad subsystem code patterns (c) IW4x team.
 *
 * Phase 3-B: Path A (CoD4x-native delivery). No engine ABI calls here;
 * this is a pure polling/state-machine layer. It reads XInput, populates
 * gp_state[] + gp_raw[] (defined in gamepad_internal.h), and exposes the
 * edge-detect helpers (gp_is_button_pressed/released/...) that the
 * dispatch layer in gamepad_buttons.c consumes.
 *
 * Mirrored from iw3sp_mod's Gamepad.cpp:
 *   - GPad_Check                (1232)
 *   - GPad_RefreshAll           (1249)
 *   - GPad_UpdateAll            (1796)
 *   - GPad_UpdateSticks         (1712)  [populated but not consumed in
 *                                        Phase 3-B; the stick handling
 *                                        in gamepad.c uses gp_raw[]
 *                                        directly until Phase 3-C]
 *   - GPad_UpdateDigitals       (1745)
 *   - GPad_UpdateAnalogs        (1770)
 *   - GPad_UpdateSticksDown     (1677)
 *   - GPad_GetStick             (1577)
 *   - GPad_GetButton            (1586)
 *   - GPad_IsButtonPressed      (1610)
 *   - GPad_IsButtonReleased     (1646)
 *   - GPad_ButtonRequiresUpdates(1641)
 */

#include "q_shared.h"
#include "qcommon.h"
#include "win_sys.h"
#include "gamepad_internal.h"

#include <windows.h>
#include <xinput.h>

/* ===================================================================
 * Tunables
 * =================================================================== */

/* How often we probe for plug/unplug. Matches iw3sp_mod's interval. */
#define GP_REFRESH_INTERVAL_MS  500

/* Trigger threshold (normalized 0..1) for treating a trigger as
 * "pressed". 0.12 matches Stage 3A's hardcoded 30/255 ~= 0.118 -- so
 * existing config_mp.cfg bindings on K_JOY15/K_JOY16 behave the same
 * as before this refactor. */
#define GP_TRIGGER_DEADZONE     0.12f

/* Stick deadzone for the edge-detect bookkeeping in
 * gp_update_sticks_down(). The actual stick math used by view/movement
 * still lives in gamepad.c and uses its own cl_gamepad_deadzone_* cvars;
 * this constant just keeps gp_state.stickDown[][] honest for any future
 * consumer that wants stick-as-keynum events. */
#define GP_STICK_PRESSED_THRESHOLD  0.40f

/* ===================================================================
 * Module-owned globals
 *
 *   Declared `extern` in gamepad_internal.h. We are the sole writer.
 * =================================================================== */

gp_state_t      gp_state[GP_MAX_GPAD_COUNT];
XINPUT_GAMEPAD  gp_raw[GP_MAX_GPAD_COUNT];

static int      s_last_refresh_time = -100000; /* force first-call refresh */

/* ===================================================================
 * Helpers used internally
 * =================================================================== */

/* Mirrors iw3sp_mod GPad_Check (Gamepad.cpp:1232).
 *
 *   XInputGetCapabilities returns ERROR_SUCCESS (0) when a pad is
 *   connected at `portIndex`; we mirror that into gp_state.enabled.
 *   Transitions (false->true / true->false) are logged so the user
 *   sees the same connect/disconnect notifications Stage 3A had. */
static qboolean gp_check(int port)
{
    qboolean previously_enabled = gp_state[port].enabled ? qtrue : qfalse;
    DWORD result;

    result = XInputGetCapabilities((DWORD)port, XINPUT_FLAG_GAMEPAD, &gp_state[port].caps);
    gp_state[port].enabled = (result == ERROR_SUCCESS);

    if (!previously_enabled && gp_state[port].enabled)
    {
        gp_state[port].portIndex = port;
        Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: connected (slot %d)\n", port);
    }
    else if (previously_enabled && !gp_state[port].enabled)
    {
        gp_release_all(port);
        Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: disconnected (slot %d)\n", port);
    }

    return gp_state[port].enabled ? qtrue : qfalse;
}

/* Mirrors iw3sp_mod GPad_RefreshAll (Gamepad.cpp:1249).
 *
 *   With GP_MAX_GPAD_COUNT == 1 this collapses to a single check on
 *   slot 0. Kept loop-shaped for symmetry with the reference. */
static void gp_refresh_all(void)
{
    int port_index = 0;
    int local;

    for (local = 0; local < GP_MAX_GPAD_COUNT; ++local)
    {
        while (port_index < XUSER_MAX_COUNT)
        {
            if (gp_check(port_index))
            {
                port_index++;
                break;
            }
            port_index++;
        }
    }
}

/* Mirrors iw3sp_mod GPad_UpdateDigitals (Gamepad.cpp:1745). */
static void gp_update_digitals(int port, const XINPUT_GAMEPAD *state)
{
    gp_state[port].lastDigitals = gp_state[port].digitals;
    gp_state[port].digitals     = state->wButtons;
}

/* Mirrors iw3sp_mod GPad_UpdateAnalogs (Gamepad.cpp:1770).
 *
 *   Normalizes the 0..255 trigger values to 0..1 and floors anything
 *   below GP_TRIGGER_DEADZONE to zero (so a noisy at-rest trigger
 *   never registers as held). */
static void gp_update_analogs(int port, const XINPUT_GAMEPAD *state)
{
    gp_state[port].lastAnalogs[0] = gp_state[port].analogs[0];
    gp_state[port].lastAnalogs[1] = gp_state[port].analogs[1];

    gp_state[port].analogs[0] = (float)state->bLeftTrigger  / 255.0f;
    gp_state[port].analogs[1] = (float)state->bRightTrigger / 255.0f;

    if (gp_state[port].analogs[0] < GP_TRIGGER_DEADZONE) gp_state[port].analogs[0] = 0.0f;
    if (gp_state[port].analogs[1] < GP_TRIGGER_DEADZONE) gp_state[port].analogs[1] = 0.0f;
}

/* Mirrors iw3sp_mod GPad_UpdateSticks (Gamepad.cpp:1712).
 *
 *   We populate sticks[] for completeness, but Phase 3-B does not
 *   consume them -- the existing Stage 3A view/movement code in
 *   gamepad.c reads gp_raw[port] directly. */
static void gp_update_sticks(int port, const XINPUT_GAMEPAD *state)
{
    int i;
    for (i = 0; i < 4; ++i)
        gp_state[port].lastSticks[i] = gp_state[port].sticks[i];

    gp_state[port].sticks[0] = (float)state->sThumbLX / 32767.0f;
    gp_state[port].sticks[1] = (float)state->sThumbLY / 32767.0f;
    gp_state[port].sticks[2] = (float)state->sThumbRX / 32767.0f;
    gp_state[port].sticks[3] = (float)state->sThumbRY / 32767.0f;
}

/* Mirrors iw3sp_mod GPad_UpdateSticksDown (Gamepad.cpp:1677).
 *
 *   Per-stick directional booleans (pos/neg on each axis). We do not
 *   dispatch stick-as-key events in Phase 3-B; this is bookkeeping
 *   for any later consumer (Phase 3-E menu scroll, etc.). */
static void gp_update_sticks_down(int port)
{
    int stick_idx;
    for (stick_idx = 0; stick_idx < 4; ++stick_idx)
    {
        float v = gp_state[port].sticks[stick_idx];

        gp_state[port].stickDownLast[stick_idx][GPAD_STICK_POS]
            = gp_state[port].stickDown[stick_idx][GPAD_STICK_POS];
        gp_state[port].stickDownLast[stick_idx][GPAD_STICK_NEG]
            = gp_state[port].stickDown[stick_idx][GPAD_STICK_NEG];

        gp_state[port].stickDown[stick_idx][GPAD_STICK_POS]
            = ( v >=  GP_STICK_PRESSED_THRESHOLD);
        gp_state[port].stickDown[stick_idx][GPAD_STICK_NEG]
            = ( v <= -GP_STICK_PRESSED_THRESHOLD);
    }
}

/* ===================================================================
 * Public API (declared in gamepad_internal.h)
 * =================================================================== */

void gp_poll_all(void)
{
    int time = Sys_Milliseconds();
    int port;

    if (time - s_last_refresh_time >= GP_REFRESH_INTERVAL_MS)
    {
        gp_refresh_all();
        s_last_refresh_time = time;
    }

    for (port = 0; port < GP_MAX_GPAD_COUNT; ++port)
    {
        XINPUT_STATE state;
        DWORD        result;

        if (!gp_state[port].enabled)
            continue;

        result = XInputGetState((DWORD)gp_state[port].portIndex, &state);
        if (result != ERROR_SUCCESS)
        {
            /* Hot-unplug detected between refresh ticks: drop the
             * enabled flag, release everything, log once. */
            gp_state[port].enabled = false;
            gp_release_all(port);
            Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: disconnected (slot %d)\n",
                       gp_state[port].portIndex);
            continue;
        }

        /* Cache raw state for the stick code in gamepad.c. */
        gp_raw[port] = state.Gamepad;

        gp_update_sticks  (port, &state.Gamepad);
        gp_update_digitals(port, &state.Gamepad);
        gp_update_analogs (port, &state.Gamepad);
        gp_update_sticks_down(port);
    }
}

/* Mirrors iw3sp_mod GPad_GetButton (Gamepad.cpp:1586).
 *
 *   Returns 1.0 for a fully-down digital button, the 0..1 analog
 *   trigger value for an analog "button", 0.0 otherwise. */
float gp_get_button(int port, gp_button_e button)
{
    unsigned int b = (unsigned int)button;

    if (port < 0 || port >= GP_MAX_GPAD_COUNT)
        return 0.0f;

    if (b & GP_DIGITAL_MASK)
    {
        return (gp_state[port].digitals & (unsigned short)b) ? 1.0f : 0.0f;
    }
    if (b & GP_ANALOG_MASK)
    {
        unsigned int idx = b & GP_VALUE_MASK;
        if (idx >= 2) return 0.0f;
        return gp_state[port].analogs[idx];
    }
    return 0.0f;
}

/* Mirrors iw3sp_mod GPad_IsButtonPressed (Gamepad.cpp:1610).
 *
 *   Edge: was-not-down AND is-down. Returns 1 / 0 (qtrue / qfalse). */
int gp_is_button_pressed(int port, gp_button_e button)
{
    unsigned int b = (unsigned int)button;

    if (port < 0 || port >= GP_MAX_GPAD_COUNT)
        return 0;

    if (b & GP_DIGITAL_MASK)
    {
        unsigned short mask = (unsigned short)b;
        int was_down = (gp_state[port].lastDigitals & mask) != 0;
        int is_down  = (gp_state[port].digitals     & mask) != 0;
        return (!was_down && is_down);
    }
    if (b & GP_ANALOG_MASK)
    {
        unsigned int idx = b & GP_VALUE_MASK;
        if (idx >= 2) return 0;
        return (gp_state[port].lastAnalogs[idx] <  GP_TRIGGER_DEADZONE)
            && (gp_state[port].analogs[idx]     >= GP_TRIGGER_DEADZONE);
    }
    return 0;
}

/* Mirrors iw3sp_mod GPad_IsButtonReleased (Gamepad.cpp:1646). */
int gp_is_button_released(int port, gp_button_e button)
{
    unsigned int b = (unsigned int)button;

    if (port < 0 || port >= GP_MAX_GPAD_COUNT)
        return 0;

    if (b & GP_DIGITAL_MASK)
    {
        unsigned short mask = (unsigned short)b;
        int was_down = (gp_state[port].lastDigitals & mask) != 0;
        int is_down  = (gp_state[port].digitals     & mask) != 0;
        return (was_down && !is_down);
    }
    if (b & GP_ANALOG_MASK)
    {
        unsigned int idx = b & GP_VALUE_MASK;
        if (idx >= 2) return 0;
        return (gp_state[port].lastAnalogs[idx] >= GP_TRIGGER_DEADZONE)
            && (gp_state[port].analogs[idx]     <  GP_TRIGGER_DEADZONE);
    }
    return 0;
}

/* Mirrors iw3sp_mod GPad_ButtonRequiresUpdates (Gamepad.cpp:1641).
 *
 *   Unused in Path A (Com_QueueEvent is edge-only), kept for symmetry
 *   with the reference and for any future Path-B consumer that needs
 *   continuous-update events. */
int gp_button_requires_updates(int port, gp_button_e button)
{
    unsigned int b = (unsigned int)button;

    if (port < 0 || port >= GP_MAX_GPAD_COUNT)
        return 0;

    if (b & GP_DIGITAL_MASK)
    {
        unsigned short mask = (unsigned short)b;
        int was_down = (gp_state[port].lastDigitals & mask) != 0;
        int is_down  = (gp_state[port].digitals     & mask) != 0;
        return (was_down && is_down);
    }
    /* For analog "buttons" the in-band PRESSED/RELEASED edges cover it. */
    return 0;
}

/* Mirrors iw3sp_mod GPad_GetStick (Gamepad.cpp:1577). */
float gp_get_stick(int port, gp_stick_e stick)
{
    unsigned int s = (unsigned int)stick;
    unsigned int idx;

    if (port < 0 || port >= GP_MAX_GPAD_COUNT)
        return 0.0f;
    if (!(s & GP_STICK_MASK))
        return 0.0f;

    idx = s & GP_VALUE_MASK;
    if (idx >= 4)
        return 0.0f;

    return gp_state[port].sticks[idx];
}
