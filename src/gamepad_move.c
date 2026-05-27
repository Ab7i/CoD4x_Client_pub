/*
 * gamepad_move.c -- Phase 3-C analog movement via usercmd_s.
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
 * Mirrored from iw3sp_mod's Gamepad.cpp:
 *   - CL_GamepadAxisValue   (855) -- per-virtual-axis read with squared mode
 *   - ClampChar             (889) -- clamp int to signed char range
 *   - CL_GamepadMove        (894) -- writes forwardmove/rightmove/pitchmove/yawmove
 *   - CL_MouseMove          (958) -- dispatcher (mouse path vs gamepad path)
 *
 * Phase 3-C scope
 * ---------------
 * (a) Implement the four functions above + a small axesValues populate
 *     helper.
 * (b) Provide gp_cl_mousemove as the future HOOK_JUMP target at iw3mp
 *     0x463D70. The function is built, compiled, and unit-callable in
 *     this commit -- but NO HOOK IS INSTALLED YET. That happens in a
 *     separate Phase 3-C.4 commit after this code is reviewed.
 * (c) The aim-assist branch from iw3sp_mod's CL_GamepadMove (lines
 *     931-954) is DELIBERATELY SKIPPED. It reads engine clients[0] /
 *     viewangles / cgameMaxPitchSpeed / AimInput / AimOutput, which
 *     is engine-ABI Path B territory and lands in Stage 3-D.
 *
 * Default axis mapping (hardcoded for 3-C; configurable via bindaxis
 * in Phase 3-E):
 *   VA_SIDE    <- PHYSAXIS_LSTICK_X     (left stick X)
 *   VA_FORWARD <- PHYSAXIS_LSTICK_Y     (left stick Y)
 *   VA_YAW     <- PHYSAXIS_RSTICK_X     (right stick X)
 *   VA_PITCH   <- PHYSAXIS_RSTICK_Y     (right stick Y)
 *   VA_ATTACK  <- PHYSAXIS_RTRIGGER
 *   VA_UP      <- (none -- jump stays on the bound digital button)
 *
 * Pre-flight: DumpCallSite.java (Stage 3-C.3) verified iw3mp 0x463D70
 * is `CALL 0x463490` (cdecl) -- our gp_cl_mousemove can be installed
 * as a 5-byte JMP rel32 without trampoline.
 */

#include "q_shared.h"
#include "qcommon.h"
#include "win_sys.h"
#include "client.h"    /* Key_IsCatcherActive */
#include "gamepad_internal.h"

#include <math.h>

/* ===================================================================
 * Storage owned by this TU
 * =================================================================== */

gp_globals_t gp_globals[GP_MAX_GPAD_COUNT];

/* Initialized lazily on first gp_populate_axes call. We can't do it
 * at IN_StartupGamepads time without adding yet another call site;
 * lazy-init costs one branch per frame and keeps gamepad.c untouched
 * apart from the cvar registrations. */
static qboolean s_globals_inited = qfalse;

static void gp_init_globals(void)
{
    int p, v;
    for (p = 0; p < GP_MAX_GPAD_COUNT; ++p)
    {
        int i;
        for (i = 0; i < GP_PHYSAXIS_COUNT; ++i)
            gp_globals[p].axes.axesValues[i] = 0.0f;
        for (v = 0; v < GP_VIRTAXIS_COUNT; ++v)
        {
            gp_globals[p].axes.virtualAxes[v].physicalAxis = GPAD_PHYSAXIS_NONE;
            gp_globals[p].axes.virtualAxes[v].mapType      = GPAD_MAP_NONE;
        }
        /* Default binding (console-style FPS layout). */
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_SIDE   ].physicalAxis = GPAD_PHYSAXIS_LSTICK_X;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_SIDE   ].mapType      = GPAD_MAP_LINEAR;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_FORWARD].physicalAxis = GPAD_PHYSAXIS_LSTICK_Y;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_FORWARD].mapType      = GPAD_MAP_LINEAR;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_YAW    ].physicalAxis = GPAD_PHYSAXIS_RSTICK_X;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_YAW    ].mapType      = GPAD_MAP_LINEAR;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_PITCH  ].physicalAxis = GPAD_PHYSAXIS_RSTICK_Y;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_PITCH  ].mapType      = GPAD_MAP_LINEAR;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_ATTACK ].physicalAxis = GPAD_PHYSAXIS_RTRIGGER;
        gp_globals[p].axes.virtualAxes[GPAD_VIRTAXIS_ATTACK ].mapType      = GPAD_MAP_LINEAR;
        gp_globals[p].nextScrollTime = 0;
    }
    s_globals_inited = qtrue;
}

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Mirrors iw3sp_mod ClampChar (Gamepad.cpp:889). */
static char gp_clamp_char(int v)
{
    if (v < -128) return (char)-128;
    if (v >  127) return (char) 127;
    return (char)v;
}

/* Apply a radial deadzone to the *raw* stick magnitude before it
 * feeds CL_GamepadAxisValue. We re-normalize so the post-deadzone
 * value still spans -1..+1; that matches Stage 3A's right-stick
 * radial deadzone behaviour. Triggers use a simple per-axis floor. */
static float gp_apply_stick_deadzone(float v, float dz)
{
    float av;
    if (dz <= 0.0f) return v;
    av = (v < 0.0f) ? -v : v;
    if (av <= dz) return 0.0f;
    /* Re-normalize so v == sign * (av - dz) / (1 - dz). */
    {
        float n = (av - dz) / (1.0f - dz);
        if (n > 1.0f) n = 1.0f;
        return (v < 0.0f) ? -n : n;
    }
}

/* Copy gp_state[port].sticks[] + analogs[] into the canonical
 * axesValues[] indexed by gp_phys_axis_e, applying the deadzones
 * the user already configured for Stage 3A. Triggers fold their
 * 0..1 magnitude into a "positive deflection only" axis -- iw3sp_mod
 * does the same. */
void gp_populate_axes(int port)
{
    float lx, ly, rx, ry, lt, rt;
    float dzL, dzR;

    if (!s_globals_inited)
        gp_init_globals();

    if (port < 0 || port >= GP_MAX_GPAD_COUNT)
        return;

    dzL = cl_gamepad_deadzone_left  ? cl_gamepad_deadzone_left->floatval  : 0.0f;
    dzR = cl_gamepad_deadzone_right ? cl_gamepad_deadzone_right->floatval : 0.0f;

    lx = gp_apply_stick_deadzone(gp_state[port].sticks[0], dzL);
    ly = gp_apply_stick_deadzone(gp_state[port].sticks[1], dzL);
    rx = gp_apply_stick_deadzone(gp_state[port].sticks[2], dzR);
    ry = gp_apply_stick_deadzone(gp_state[port].sticks[3], dzR);
    lt = gp_state[port].analogs[0]; /* already deadzone-floored in gp_poll_all */
    rt = gp_state[port].analogs[1];

    gp_globals[port].axes.axesValues[GPAD_PHYSAXIS_LSTICK_X] = lx;
    gp_globals[port].axes.axesValues[GPAD_PHYSAXIS_LSTICK_Y] = ly;
    gp_globals[port].axes.axesValues[GPAD_PHYSAXIS_RSTICK_X] = rx;
    gp_globals[port].axes.axesValues[GPAD_PHYSAXIS_RSTICK_Y] = ry;
    gp_globals[port].axes.axesValues[GPAD_PHYSAXIS_LTRIGGER] = lt;
    gp_globals[port].axes.axesValues[GPAD_PHYSAXIS_RTRIGGER] = rt;
}

/* Mirrors iw3sp_mod CL_GamepadAxisValue (Gamepad.cpp:855).
 *
 *   Looks up the physical axis bound to `virt`, returns its raw
 *   deflection in [-1..1]. SQUARED map mode combines the deflection
 *   with its sibling axis on the same stick (sqrt(a*a + b*b)*a).
 *
 *   axisSameStick[] (iw3sp_mod Gamepad.cpp:44): for each physical
 *   axis, the OTHER axis on the same stick. We inline it here to
 *   avoid making it a public symbol. */
static gp_phys_axis_e gp_axis_same_stick(gp_phys_axis_e ax)
{
    switch (ax)
    {
        case GPAD_PHYSAXIS_RSTICK_X: return GPAD_PHYSAXIS_RSTICK_Y;
        case GPAD_PHYSAXIS_RSTICK_Y: return GPAD_PHYSAXIS_RSTICK_X;
        case GPAD_PHYSAXIS_LSTICK_X: return GPAD_PHYSAXIS_LSTICK_Y;
        case GPAD_PHYSAXIS_LSTICK_Y: return GPAD_PHYSAXIS_LSTICK_X;
        default:                     return GPAD_PHYSAXIS_NONE;
    }
}

static float gp_axis_value(int port, gp_virt_axis_e virt)
{
    gp_virtual_axis_t binding;
    float v;

    if (virt <= GPAD_VIRTAXIS_NONE || virt >= GP_VIRTAXIS_COUNT)
        return 0.0f;

    binding = gp_globals[port].axes.virtualAxes[virt];
    if (binding.physicalAxis <= GPAD_PHYSAXIS_NONE
        || binding.physicalAxis >= GP_PHYSAXIS_COUNT)
        return 0.0f;

    v = gp_globals[port].axes.axesValues[binding.physicalAxis];

    if (binding.mapType == GPAD_MAP_SQUARED)
    {
        gp_phys_axis_e other = gp_axis_same_stick(binding.physicalAxis);
        float ov = 0.0f;
        if (other > GPAD_PHYSAXIS_NONE && other < GP_PHYSAXIS_COUNT)
            ov = gp_globals[port].axes.axesValues[other];
        v = sqrtf(v * v + ov * ov) * v;
    }
    return v;
}

/* ===================================================================
 * Movement writer (the heart of Phase 3-C)
 * =================================================================== */

/* Mirrors iw3sp_mod CL_GamepadMove (Gamepad.cpp:894), **WITHOUT** the
 * aim-assist branch (lines 931-954). That branch reads engine
 * clients[0] + AimInput/AimOutput, which is Stage 3-D Path-B work.
 *
 *   Phase 3-C scope: write the four signed-char fields on `cmd`.
 *   Engine `PM_*` does the rest. */
void gp_cl_gamepadmove(gp_usercmd_t *cmd)
{
    float pitch, yaw, forward, side;
    float move_scale;
    int   forward_move, right_move, pitch_move, yaw_move;
    float sens_scale;

    if (!cmd)
        return;
    if (!s_globals_inited)
        gp_init_globals();

    /* Refresh the axesValues[] table from gp_state for THIS frame. */
    gp_populate_axes(0);

    pitch   = gp_axis_value(0, GPAD_VIRTAXIS_PITCH);
    if (!(cl_gamepad_invert_pitch && cl_gamepad_invert_pitch->boolean))
        pitch *= -1.0f;            /* stick-up -> negative pitch (look up) */

    yaw     = -gp_axis_value(0, GPAD_VIRTAXIS_YAW);
    forward =  gp_axis_value(0, GPAD_VIRTAXIS_FORWARD);
    side    =  gp_axis_value(0, GPAD_VIRTAXIS_SIDE);

    /* Sensitivity multiplier applied to the look axes only. Movement
     * doesn't get scaled -- the engine's PM_* already speeds up the
     * player based on forwardmove magnitude. */
    sens_scale = (cl_gamepad_sens_look ? cl_gamepad_sens_look->floatval : 1.0f);
    pitch *= sens_scale;
    yaw   *= sens_scale;

    /* iw3sp_mod's diagonal-normalize trick (Gamepad.cpp:912-918). When
     * both movement axes are deflected, scale up so the combined
     * deflection still maps to CHAR_MAX = 127 (not 90 from sqrt(2)/2). */
    move_scale = 127.0f;
    if (fabsf(side) > 0.0f || fabsf(forward) > 0.0f)
    {
        float length = (fabsf(side) <= fabsf(forward))
                       ? (side / forward)
                       : (forward / side);
        move_scale = sqrtf(length * length + 1.0f) * 127.0f;
    }

    forward_move = (int)floorf(forward * move_scale);
    right_move   = (int)floorf(side    * move_scale);
    pitch_move   = (int)floorf(pitch   * move_scale);
    yaw_move     = (int)floorf(yaw     * move_scale);

    cmd->rightmove   = gp_clamp_char((int)cmd->rightmove   + right_move);
    cmd->forwardmove = gp_clamp_char((int)cmd->forwardmove + forward_move);
    cmd->pitchmove   = gp_clamp_char((int)cmd->pitchmove   + pitch_move);
    cmd->yawmove     = gp_clamp_char((int)cmd->yawmove     + yaw_move);

    /* Aim-assist block from iw3sp_mod Gamepad.cpp:931-954 is
     * intentionally omitted -- deferred to Stage 3-D. */
}

/* ===================================================================
 * Hook target (NOT installed yet -- Phase 3-C.4)
 *
 *   When installed at IW3MP_CL_MOUSEMOVE_STUB_JMP (0x463D70), this
 *   replaces the engine's CALL CL_MouseMove. We branch:
 *     - mouse path:  forward to original CL_MouseMove (0x463490).
 *     - gamepad path: call gp_cl_gamepadmove(cmd).
 *
 *   The engine pushes `cmd` and adds ESP,4 after the call (cdecl --
 *   verified by DumpCallSite.java pre-flight, Phase 3-C.3).
 * =================================================================== */

typedef void (__cdecl *gp_cl_mousemove_engine_fn)(gp_usercmd_t *cmd);

void __cdecl gp_cl_mousemove(gp_usercmd_t *cmd)
{
    /* When cl_gamepad is OFF, OR the pad isn't currently in use, OR
     * the legacy stick path is selected: defer to engine mouse code.
     * This keeps the mouse path intact for keyboard+mouse users and
     * keeps Stage 3A's CL_MouseEvent path working unchanged when
     * cl_gamepad_legacy_sticks == 1. */
    qboolean want_gamepad =
        (cl_gamepad && cl_gamepad->boolean) &&
        gp_state[0].enabled &&
        gp_state[0].inUse &&
        !(cl_gamepad_legacy_sticks && cl_gamepad_legacy_sticks->boolean);

    if (!want_gamepad)
    {
        ((gp_cl_mousemove_engine_fn)IW3MP_CL_MOUSEMOVE_FN)(cmd);
        return;
    }

    /* Skip when the in-game console catcher is active -- otherwise
     * stick input keeps moving the player while the user is typing.
     * Matches iw3sp_mod's Key_IsCatcherActive(KEYCATCH_CONSOLE) guard. */
    if (Key_IsCatcherActive(0, KEYCATCH_CONSOLE))
        return;

    gp_cl_gamepadmove(cmd);
}
