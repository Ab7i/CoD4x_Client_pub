/*
 * gamepad_buttons.c -- Path A dispatch: turn polled gamepad edges into
 * CoD4x SE_KEY events on the engine's input queue.
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
 * Phase 3-B notes
 * ---------------
 * iw3sp_mod's CL_GamepadButtonEvent (Gamepad.cpp:1295) writes engine
 * playerKeys[] directly + calls Cbuf_AddText by raw VA. That is "Path
 * B" and is deferred to Stage 3-D when we will also touch aim-assist
 * structs at the engine ABI level.
 *
 * For Phase 3-B we stay on "Path A": every dispatch lowers to a
 *
 *     Com_QueueEvent(g_wv.sysMsgTime, SE_KEY, keycode, down, 0, NULL);
 *
 * which is the same delivery mechanism Stage 3A used. This means
 *   (a) no new engine-ABI addresses to verify,
 *   (b) existing `bind JOY1 "+attack"` lines in config_mp.cfg keep
 *       working exactly as they did before Stage 3-B landed.
 *
 * The button table below is the SAME ordering as Stage 3A's
 * gamepad_buttons[] in the original gamepad.c, so no user-visible
 * binding ever moves.
 */

#include "q_shared.h"
#include "qcommon.h"
#include "win_sys.h"
#include "keycodes.h"
#include "cl_input.h"
#include "gamepad_internal.h"

#include <windows.h>
#include <xinput.h>

/* ===================================================================
 * Button table: gp_button_e -> CoD4x keynum (K_JOY1..K_JOY16)
 *
 *   Order chosen to match Stage 3A so persisted bindings do not move.
 *   The triggers participate in the same loop: gp_is_button_pressed
 *   handles both digital and analog button kinds uniformly.
 * =================================================================== */

static const gp_button_to_code_t s_gp_button_list[] =
{
    { GPAD_A,        K_JOY1  },
    { GPAD_B,        K_JOY2  },
    { GPAD_X,        K_JOY3  },
    { GPAD_Y,        K_JOY4  },
    { GPAD_L_SHLDR,  K_JOY5  },
    { GPAD_R_SHLDR,  K_JOY6  },
    { GPAD_BACK,     K_JOY7  },
    { GPAD_START,    K_JOY8  },
    { GPAD_L3,       K_JOY9  },
    { GPAD_R3,       K_JOY10 },
    { GPAD_UP,       K_JOY11 },
    { GPAD_DOWN,     K_JOY12 },
    { GPAD_LEFT,     K_JOY13 },
    { GPAD_RIGHT,    K_JOY14 },
    { GPAD_L_TRIG,   K_JOY15 },
    { GPAD_R_TRIG,   K_JOY16 },
};

#define GP_BUTTON_LIST_COUNT \
    ( sizeof(s_gp_button_list) / sizeof(s_gp_button_list[0]) )

/* ===================================================================
 * Public API (declared in gamepad_internal.h)
 * =================================================================== */

void gp_dispatch_buttons(int port)
{
    unsigned int i;

    if (port < 0 || port >= GP_MAX_GPAD_COUNT)
        return;
    if (!gp_state[port].enabled)
        return;

    /* In Path A we deliver only edge events. Com_QueueEvent +
     * the engine's Key_Event handler take care of repeat handling
     * (no need for the GPAD_BUTTON_UPDATE path iw3sp_mod uses). */
    for (i = 0; i < GP_BUTTON_LIST_COUNT; ++i)
    {
        gp_button_e b       = s_gp_button_list[i].padButton;
        int         keycode = s_gp_button_list[i].code;

        if (gp_is_button_pressed(port, b))
        {
            gp_state[port].inUse = true;
            Com_QueueEvent(g_wv.sysMsgTime, SE_KEY, keycode, qtrue, 0, NULL);
        }
        else if (gp_is_button_released(port, b))
        {
            Com_QueueEvent(g_wv.sysMsgTime, SE_KEY, keycode, qfalse, 0, NULL);
        }
    }
}

void gp_release_all(int port)
{
    unsigned int i;

    if (port < 0 || port >= GP_MAX_GPAD_COUNT)
        return;

    /* Use the LAST KNOWN held state -- the poller may have already
     * zeroed `digitals`/`analogs` to denote "no signal" before
     * calling us, so we drive releases from lastDigitals/lastAnalogs. */
    for (i = 0; i < GP_BUTTON_LIST_COUNT; ++i)
    {
        gp_button_e  b       = s_gp_button_list[i].padButton;
        int          keycode = s_gp_button_list[i].code;
        unsigned int bm      = (unsigned int)b;

        if (bm & GP_DIGITAL_MASK)
        {
            if (gp_state[port].lastDigitals & (unsigned short)bm)
                Com_QueueEvent(g_wv.sysMsgTime, SE_KEY, keycode, qfalse, 0, NULL);
        }
        else if (bm & GP_ANALOG_MASK)
        {
            unsigned int idx = bm & GP_VALUE_MASK;
            if (idx < 2 && gp_state[port].lastAnalogs[idx] > 0.0f)
                Com_QueueEvent(g_wv.sysMsgTime, SE_KEY, keycode, qfalse, 0, NULL);
        }
    }

    /* Flush the cache so the next connect starts clean. */
    gp_state[port].digitals     = 0;
    gp_state[port].lastDigitals = 0;
    gp_state[port].analogs[0]   = 0.0f;
    gp_state[port].analogs[1]   = 0.0f;
    gp_state[port].lastAnalogs[0] = 0.0f;
    gp_state[port].lastAnalogs[1] = 0.0f;
    gp_state[port].inUse        = false;

    /* Stick directional cache, too, for completeness. */
    {
        int s, d;
        for (s = 0; s < 4; ++s)
        {
            gp_state[port].sticks[s]      = 0.0f;
            gp_state[port].lastSticks[s]  = 0.0f;
            for (d = 0; d < GP_STICK_DIR_COUNT; ++d)
            {
                gp_state[port].stickDown[s][d]     = false;
                gp_state[port].stickDownLast[s][d] = false;
            }
        }
    }
}
