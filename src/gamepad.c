/*
 * gamepad.c -- native game controller support (XInput).
 *
 * Commit 2: button input enabled. Digital buttons and the two analog
 * triggers (treated as digital via a threshold) are delivered to the
 * engine as SE_KEY events through Com_QueueEvent, so they bind exactly
 * like keyboard/mouse keys. Connect/disconnect logging is kept.
 */

#include "q_shared.h"
#include "win_sys.h"
#include "qcommon.h"
#include "keycodes.h"
#include "gamepad.h"

#include <windows.h>
#include <xinput.h>

// Left/right trigger analog value (0-255) at or above which the trigger
// counts as a pressed digital button.
#define TRIGGER_THRESHOLD 30

// Controller settings. All CVAR_ARCHIVE so they persist to config_mp.cfg.
static cvar_t *cl_gamepad;
static cvar_t *cl_gamepad_sens_look;
static cvar_t *cl_gamepad_sens_ads;
static cvar_t *cl_gamepad_deadzone_left;
static cvar_t *cl_gamepad_deadzone_right;
static cvar_t *cl_gamepad_invert_y;
static cvar_t *cl_gamepad_accel_curve;

// Options for the cl_gamepad_accel_curve enum cvar (NULL-terminated).
static const char *gamepad_accel_curve_names[] =
{
    "linear",
    "exponential",
    NULL
};

// Per-frame controller tracking. XInput needs no explicit device init;
// XInputGetState is polled directly, so this only holds what we need for
// edge detection and connect/disconnect transitions.
typedef struct
{
    qboolean connected;     // slot 0 device connected as of the last poll
    WORD     prev_buttons;  // wButtons from the previous polled frame
    qboolean prev_lt_down;  // left trigger digital state, previous frame
    qboolean prev_rt_down;  // right trigger digital state, previous frame
} gamepadState_t;

static gamepadState_t s_gamepad;

// XInput digital button bit -> engine keycode.
typedef struct
{
    WORD mask;
    int  keycode;
} gamepadButton_t;

static const gamepadButton_t gamepad_buttons[] =
{
    { XINPUT_GAMEPAD_A,              K_JOY1  },
    { XINPUT_GAMEPAD_B,              K_JOY2  },
    { XINPUT_GAMEPAD_X,              K_JOY3  },
    { XINPUT_GAMEPAD_Y,              K_JOY4  },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  K_JOY5  },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, K_JOY6  },
    { XINPUT_GAMEPAD_BACK,           K_JOY7  },
    { XINPUT_GAMEPAD_START,          K_JOY8  },
    { XINPUT_GAMEPAD_LEFT_THUMB,     K_JOY9  },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    K_JOY10 },
    { XINPUT_GAMEPAD_DPAD_UP,        K_JOY11 },
    { XINPUT_GAMEPAD_DPAD_DOWN,      K_JOY12 },
    { XINPUT_GAMEPAD_DPAD_LEFT,      K_JOY13 },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     K_JOY14 },
};

#define GAMEPAD_BUTTON_COUNT ( sizeof(gamepad_buttons) / sizeof(gamepad_buttons[0]) )

/*
===========
IN_StartupGamepads

Registers the controller cvars and resets controller state tracking.
Called once from IN_Startup().
===========
*/
void IN_StartupGamepads(void)
{
    cl_gamepad = Cvar_RegisterBool(
        "cl_gamepad", qfalse, CVAR_ARCHIVE,
        "Enable native game controller support");

    cl_gamepad_sens_look = Cvar_RegisterFloat(
        "cl_gamepad_sens_look", 2.0f, 0.0f, 20.0f, CVAR_ARCHIVE,
        "Controller look (right stick) sensitivity");

    cl_gamepad_sens_ads = Cvar_RegisterFloat(
        "cl_gamepad_sens_ads", 0.5f, 0.0f, 20.0f, CVAR_ARCHIVE,
        "Controller aim-down-sights sensitivity multiplier");

    cl_gamepad_deadzone_left = Cvar_RegisterFloat(
        "cl_gamepad_deadzone_left", 0.15f, 0.0f, 1.0f, CVAR_ARCHIVE,
        "Left stick (movement) deadzone");

    cl_gamepad_deadzone_right = Cvar_RegisterFloat(
        "cl_gamepad_deadzone_right", 0.10f, 0.0f, 1.0f, CVAR_ARCHIVE,
        "Right stick (look) deadzone");

    cl_gamepad_invert_y = Cvar_RegisterBool(
        "cl_gamepad_invert_y", qfalse, CVAR_ARCHIVE,
        "Invert the controller look Y axis");

    cl_gamepad_accel_curve = Cvar_RegisterEnum(
        "cl_gamepad_accel_curve", gamepad_accel_curve_names, 0, CVAR_ARCHIVE,
        "Controller look acceleration curve");

    s_gamepad.connected = qfalse;
    s_gamepad.prev_buttons = 0;
    s_gamepad.prev_lt_down = qfalse;
    s_gamepad.prev_rt_down = qfalse;

    Com_Printf(CON_CHANNEL_SYSTEM, "XInput initialized\n");
}

/*
===========
IN_GamepadsMove

Per-frame controller poll. Called from IN_Frame().
Commit 2: queues SE_KEY events for digital buttons and for the two
triggers (analog, gated by TRIGGER_THRESHOLD). Edge-detected so each
press/release is sent exactly once.
===========
*/
void IN_GamepadsMove(void)
{
    XINPUT_STATE state;
    DWORD        result;
    WORD         changed;
    unsigned int i;
    qboolean     lt_down;
    qboolean     rt_down;

    // Feature guard: zero cost / zero XInput calls when disabled.
    if ( !cl_gamepad->boolean )
        return;

    result = XInputGetState(0, &state);

    if ( result != ERROR_SUCCESS )
    {
        // Slot 0 empty or device unplugged -- handle silently, no spam.
        // Log only the connected -> disconnected transition.
        if ( s_gamepad.connected )
        {
            s_gamepad.connected = qfalse;
            s_gamepad.prev_buttons = 0;
            s_gamepad.prev_lt_down = qfalse;
            s_gamepad.prev_rt_down = qfalse;
            Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: disconnected (slot 0)\n");
        }
        return;
    }

    // Log the disconnected -> connected transition once.
    if ( !s_gamepad.connected )
    {
        s_gamepad.connected = qtrue;
        s_gamepad.prev_buttons = 0;
        s_gamepad.prev_lt_down = qfalse;
        s_gamepad.prev_rt_down = qfalse;
        Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: connected (slot 0)\n");
    }

    // Digital buttons: queue a key event for every bit that changed.
    changed = state.Gamepad.wButtons ^ s_gamepad.prev_buttons;

    if ( changed )
    {
        for ( i = 0; i < GAMEPAD_BUTTON_COUNT; ++i )
        {
            if ( changed & gamepad_buttons[i].mask )
            {
                qboolean down =
                    ( state.Gamepad.wButtons & gamepad_buttons[i].mask ) != 0;

                Com_QueueEvent( g_wv.sysMsgTime, SE_KEY,
                    gamepad_buttons[i].keycode, down, 0, NULL );
            }
        }
    }

    s_gamepad.prev_buttons = state.Gamepad.wButtons;

    // Triggers: analog 0-255, treated as digital buttons via a threshold.
    // Edge-detected against the cached state so each crossing fires once.
    lt_down = ( state.Gamepad.bLeftTrigger  > TRIGGER_THRESHOLD );
    rt_down = ( state.Gamepad.bRightTrigger > TRIGGER_THRESHOLD );

    if ( lt_down != s_gamepad.prev_lt_down )
    {
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_JOY15, lt_down, 0, NULL );
        s_gamepad.prev_lt_down = lt_down;
    }

    if ( rt_down != s_gamepad.prev_rt_down )
    {
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_JOY16, rt_down, 0, NULL );
        s_gamepad.prev_rt_down = rt_down;
    }
}
