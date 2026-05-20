/*
 * gamepad.c -- native game controller support (XInput).
 *
 * Commit 1: XInput probe + button edge logging. Polls controller slot 0
 * each frame and prints button press/release to the console. No events
 * are sent to the engine yet -- this verifies the controller is read
 * correctly before wiring input through.
 */

#include "q_shared.h"
#include "qcommon.h"
#include "gamepad.h"

#include <windows.h>
#include <xinput.h>

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
    qboolean connected;    // slot 0 device connected as of the last poll
    WORD     prev_buttons; // wButtons from the previous polled frame
} gamepadState_t;

static gamepadState_t s_gamepad;

// XInput digital button bit -> human-readable name, for Commit 1 logging.
typedef struct
{
    WORD        mask;
    const char *name;
} gamepadButton_t;

static const gamepadButton_t gamepad_buttons[] =
{
    { XINPUT_GAMEPAD_DPAD_UP,        "DPad-Up"       },
    { XINPUT_GAMEPAD_DPAD_DOWN,      "DPad-Down"     },
    { XINPUT_GAMEPAD_DPAD_LEFT,      "DPad-Left"     },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     "DPad-Right"    },
    { XINPUT_GAMEPAD_START,          "Start"         },
    { XINPUT_GAMEPAD_BACK,           "Back"          },
    { XINPUT_GAMEPAD_LEFT_THUMB,     "LeftThumb"     },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    "RightThumb"    },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  "LeftShoulder"  },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RightShoulder" },
    { XINPUT_GAMEPAD_A,              "A"             },
    { XINPUT_GAMEPAD_B,              "B"             },
    { XINPUT_GAMEPAD_X,              "X"             },
    { XINPUT_GAMEPAD_Y,              "Y"             },
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

    Com_Printf(CON_CHANNEL_SYSTEM, "XInput initialized\n");
}

/*
===========
IN_GamepadsMove

Per-frame controller poll. Called from IN_Frame().
Commit 1: detects button edges on slot 0 and logs them. No engine
events are generated yet.
===========
*/
void IN_GamepadsMove(void)
{
    XINPUT_STATE state;
    DWORD        result;
    WORD         changed;
    unsigned int i;

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
            Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: disconnected (slot 0)\n");
        }
        return;
    }

    // Log the disconnected -> connected transition once.
    if ( !s_gamepad.connected )
    {
        s_gamepad.connected = qtrue;
        s_gamepad.prev_buttons = 0;
        Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: connected (slot 0)\n");
    }

    // Edge detection: bits that differ between this frame and the last.
    changed = state.Gamepad.wButtons ^ s_gamepad.prev_buttons;

    if ( changed )
    {
        for ( i = 0; i < GAMEPAD_BUTTON_COUNT; ++i )
        {
            if ( changed & gamepad_buttons[i].mask )
            {
                qboolean down =
                    ( state.Gamepad.wButtons & gamepad_buttons[i].mask ) != 0;

                Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: %s %s\n",
                    gamepad_buttons[i].name,
                    down ? "pressed" : "released");
            }
        }
    }

    s_gamepad.prev_buttons = state.Gamepad.wButtons;
}
