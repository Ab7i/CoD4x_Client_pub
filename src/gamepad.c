/*
 * gamepad.c -- native game controller support (XInput).
 *
 * Stage 3A: analog sticks. Right stick drives the view through the same
 * CL_MouseEvent path the mouse uses; left stick drives 8-way digital
 * movement via synthetic arrow-key events. Buttons + triggers as Commit 2.
 * On disconnect, every held input is released (Gamepad_ReleaseAll) so
 * nothing stays stuck "down" in the engine. Includes temporary
 * cl_gamepad_debug logging for the triggers and both sticks.
 */

#include "q_shared.h"
#include "win_sys.h"
#include "qcommon.h"
#include "keycodes.h"
#include "cl_input.h"
#include "gamepad.h"

#include <windows.h>
#include <xinput.h>
#include <math.h>

// Left/right trigger analog value (0-255) at/above which it counts down.
#define TRIGGER_THRESHOLD  30

// XInput thumbstick full-scale value.
#define THUMB_MAX          32767.0f

// Converts a normalized stick magnitude (0..1, after sensitivity) into
// mouse-like per-frame counts for CL_MouseEvent. Primary look-feel knob.
#define GAMEPAD_LOOK_SCALE 12.0f

// Controller settings. All CVAR_ARCHIVE so they persist to config_mp.cfg.
static cvar_t *cl_gamepad;
static cvar_t *cl_gamepad_sens_look;
static cvar_t *cl_gamepad_sens_ads;
static cvar_t *cl_gamepad_deadzone_left;
static cvar_t *cl_gamepad_deadzone_right;
static cvar_t *cl_gamepad_invert_y;
static cvar_t *cl_gamepad_accel_curve;

// Temporary diagnostic cvar (flags 0 -- not archived).
static cvar_t *cl_gamepad_debug;

// Options for the cl_gamepad_accel_curve enum cvar (NULL-terminated).
static const char *gamepad_accel_curve_names[] =
{
    "linear",
    "exponential",
    NULL
};

// Per-frame controller tracking for edge detection and transitions.
typedef struct
{
    qboolean connected;     // slot 0 device connected as of the last poll
    WORD     prev_buttons;  // wButtons from the previous polled frame
    qboolean prev_lt_down;  // left trigger digital state, previous frame
    qboolean prev_rt_down;  // right trigger digital state, previous frame
    BYTE     prev_lt_raw;   // raw left-trigger value, previous frame (debug)
    BYTE     prev_rt_raw;   // raw right-trigger value, previous frame (debug)
    qboolean prev_move_fwd;   // left-stick forward state, previous frame
    qboolean prev_move_back;  // left-stick backward state, previous frame
    qboolean prev_move_left;  // left-stick strafe-left state, previous frame
    qboolean prev_move_right; // left-stick strafe-right state, previous frame
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

    cl_gamepad_debug = Cvar_RegisterBool(
        "cl_gamepad_debug", qfalse, 0,
        "Log raw controller trigger/stick values (temporary diagnostic)");

    s_gamepad.connected = qfalse;
    s_gamepad.prev_buttons = 0;
    s_gamepad.prev_lt_down = qfalse;
    s_gamepad.prev_rt_down = qfalse;
    s_gamepad.prev_lt_raw = 0;
    s_gamepad.prev_rt_raw = 0;
    s_gamepad.prev_move_fwd = qfalse;
    s_gamepad.prev_move_back = qfalse;
    s_gamepad.prev_move_left = qfalse;
    s_gamepad.prev_move_right = qfalse;

    Com_Printf(CON_CHANNEL_SYSTEM, "XInput initialized\n");
}

/*
===========
Gamepad_ReleaseMovement

Sends a release event for any held left-stick movement direction and
clears the cached states.
===========
*/
static void Gamepad_ReleaseMovement(void)
{
    if ( s_gamepad.prev_move_fwd )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_UPARROW, qfalse, 0, NULL );
    if ( s_gamepad.prev_move_back )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_DOWNARROW, qfalse, 0, NULL );
    if ( s_gamepad.prev_move_left )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_LEFTARROW, qfalse, 0, NULL );
    if ( s_gamepad.prev_move_right )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_RIGHTARROW, qfalse, 0, NULL );

    s_gamepad.prev_move_fwd = qfalse;
    s_gamepad.prev_move_back = qfalse;
    s_gamepad.prev_move_left = qfalse;
    s_gamepad.prev_move_right = qfalse;
}

/*
===========
Gamepad_ReleaseAll

Sends a release event for every input currently held -- digital buttons,
both triggers, and left-stick movement -- then clears all cached states.
Used on controller disconnect so nothing stays stuck "down" in the engine
when the pad is unplugged mid-input.
===========
*/
static void Gamepad_ReleaseAll(void)
{
    unsigned int i;

    // Digital buttons held in the last polled frame.
    for ( i = 0; i < GAMEPAD_BUTTON_COUNT; ++i )
    {
        if ( s_gamepad.prev_buttons & gamepad_buttons[i].mask )
            Com_QueueEvent( g_wv.sysMsgTime, SE_KEY,
                gamepad_buttons[i].keycode, qfalse, 0, NULL );
    }
    s_gamepad.prev_buttons = 0;

    // Triggers.
    if ( s_gamepad.prev_lt_down )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_JOY15, qfalse, 0, NULL );
    if ( s_gamepad.prev_rt_down )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_JOY16, qfalse, 0, NULL );
    s_gamepad.prev_lt_down = qfalse;
    s_gamepad.prev_rt_down = qfalse;

    // Left-stick movement directions.
    Gamepad_ReleaseMovement();
}

/*
===========
Gamepad_ApplyRightStick

Right stick -> view. Feeds the same CL_MouseEvent accumulator the mouse
uses, after a radial deadzone, magnitude re-normalization, acceleration
curve, sensitivity scaling and optional Y inversion.
===========
*/
static void Gamepad_ApplyRightStick(SHORT thumb_rx, SHORT thumb_ry)
{
    static SHORT dbg_prev_rx = 0;   // last raw rx that was debug-logged
    static SHORT dbg_prev_ry = 0;   // last raw ry that was debug-logged

    float rx, ry, magnitude, dz, norm_mag, curve_mag, scale;
    float dir_x, dir_y;
    int   dx, dy;
    qboolean dbg;

    rx = (float)thumb_rx / THUMB_MAX;
    ry = (float)thumb_ry / THUMB_MAX;

    magnitude = sqrtf( rx * rx + ry * ry );
    dz = cl_gamepad_deadzone_right->floatval;

    // TEMP debug: log only when a raw axis moved noticeably (> 1000),
    // gated behind cl_gamepad_debug, to avoid per-frame spam.
    dbg = qfalse;
    if ( cl_gamepad_debug->boolean )
    {
        int drx = (int)thumb_rx - (int)dbg_prev_rx;
        int dry = (int)thumb_ry - (int)dbg_prev_ry;
        if ( drx < 0 ) drx = -drx;
        if ( dry < 0 ) dry = -dry;
        if ( drx > 1000 || dry > 1000 )
        {
            dbg = qtrue;
            dbg_prev_rx = thumb_rx;
            dbg_prev_ry = thumb_ry;
        }
    }

    // Radial deadzone: ignore the stick entirely inside the dead circle.
    if ( magnitude <= dz || magnitude <= 0.0f )
    {
        if ( dbg )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick R: rx=%d ry=%d mag=%.3f (in deadzone)\n",
                thumb_rx, thumb_ry, magnitude);
        return;
    }

    // Re-normalize the post-deadzone magnitude to 0..1.
    norm_mag = ( magnitude - dz ) / ( 1.0f - dz );
    if ( norm_mag > 1.0f )
        norm_mag = 1.0f;

    // Acceleration curve applied to the magnitude (direction preserved).
    if ( cl_gamepad_accel_curve->integer == 1 )   // "exponential"
        curve_mag = norm_mag * norm_mag;
    else                                          // "linear"
        curve_mag = norm_mag;

    dir_x = rx / magnitude;
    dir_y = ry / magnitude;

    scale = cl_gamepad_sens_look->floatval * GAMEPAD_LOOK_SCALE;

    dx = (int)( dir_x * curve_mag * scale );
    dy = (int)( dir_y * curve_mag * scale );

    // Stick up (positive ry) should look up. Mouse "look up" is negative
    // dy, so negate by default; cl_gamepad_invert_y keeps it positive.
    if ( !cl_gamepad_invert_y->boolean )
        dy = -dy;

    if ( dbg )
        Com_Printf(CON_CHANNEL_SYSTEM,
            "Stick R: rx=%d ry=%d mag=%.3f -> dx=%d dy=%d (CL_MouseEvent)\n",
            thumb_rx, thumb_ry, magnitude, dx, dy);

    if ( dx != 0 || dy != 0 )
        CL_MouseEvent( 0, 0, dx, dy );
}

/*
===========
Gamepad_ApplyLeftStick

Left stick -> 8-way digital movement. Each axis direction is an on/off
state past an axial deadzone; edge changes are sent as arrow-key events.
===========
*/
static void Gamepad_ApplyLeftStick(SHORT thumb_lx, SHORT thumb_ly)
{
    float    lx, ly, dz;
    qboolean fwd, back, left, right;

    lx = (float)thumb_lx / THUMB_MAX;
    ly = (float)thumb_ly / THUMB_MAX;
    dz = cl_gamepad_deadzone_left->floatval;

    fwd   = ( ly >  dz );
    back  = ( ly < -dz );
    left  = ( lx < -dz );
    right = ( lx >  dz );

    if ( fwd != s_gamepad.prev_move_fwd )
    {
        if ( cl_gamepad_debug->boolean )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick L: lx=%d ly=%d fwd=%d (was %d) -> K_UPARROW %s\n",
                thumb_lx, thumb_ly, fwd, s_gamepad.prev_move_fwd,
                fwd ? "pressed" : "released");
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_UPARROW, fwd, 0, NULL );
        s_gamepad.prev_move_fwd = fwd;
    }
    if ( back != s_gamepad.prev_move_back )
    {
        if ( cl_gamepad_debug->boolean )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick L: lx=%d ly=%d back=%d (was %d) -> K_DOWNARROW %s\n",
                thumb_lx, thumb_ly, back, s_gamepad.prev_move_back,
                back ? "pressed" : "released");
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_DOWNARROW, back, 0, NULL );
        s_gamepad.prev_move_back = back;
    }
    if ( left != s_gamepad.prev_move_left )
    {
        if ( cl_gamepad_debug->boolean )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick L: lx=%d ly=%d left=%d (was %d) -> K_LEFTARROW %s\n",
                thumb_lx, thumb_ly, left, s_gamepad.prev_move_left,
                left ? "pressed" : "released");
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_LEFTARROW, left, 0, NULL );
        s_gamepad.prev_move_left = left;
    }
    if ( right != s_gamepad.prev_move_right )
    {
        if ( cl_gamepad_debug->boolean )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick L: lx=%d ly=%d right=%d (was %d) -> K_RIGHTARROW %s\n",
                thumb_lx, thumb_ly, right, s_gamepad.prev_move_right,
                right ? "pressed" : "released");
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_RIGHTARROW, right, 0, NULL );
        s_gamepad.prev_move_right = right;
    }
}

/*
===========
IN_GamepadsMove

Per-frame controller poll. Called from IN_Frame().
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
        // Release every held input so nothing stays stuck down.
        if ( s_gamepad.connected )
        {
            s_gamepad.connected = qfalse;
            Gamepad_ReleaseAll();
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
        s_gamepad.prev_move_fwd = qfalse;
        s_gamepad.prev_move_back = qfalse;
        s_gamepad.prev_move_left = qfalse;
        s_gamepad.prev_move_right = qfalse;
        Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad: connected (slot 0)\n");
    }

    // TEMPORARY drift diagnostic: raw trigger values on change, gated.
    if ( cl_gamepad_debug->boolean &&
         ( state.Gamepad.bLeftTrigger  != s_gamepad.prev_lt_raw ||
           state.Gamepad.bRightTrigger != s_gamepad.prev_rt_raw ) )
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "Gamepad debug: LT=%d RT=%d\n",
            state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger);
    }
    s_gamepad.prev_lt_raw = state.Gamepad.bLeftTrigger;
    s_gamepad.prev_rt_raw = state.Gamepad.bRightTrigger;

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

    // Analog sticks.
    Gamepad_ApplyRightStick( state.Gamepad.sThumbRX, state.Gamepad.sThumbRY );
    Gamepad_ApplyLeftStick(  state.Gamepad.sThumbLX, state.Gamepad.sThumbLY );
}
