/*
 * gamepad.c -- native game controller support (XInput).
 *
 * Phase 3-B (2026-05-27): button + trigger event delivery is now driven
 * by the iw3sp_mod-style state machine in gamepad_poll.c / gamepad_buttons.c.
 * The top-level entry points (IN_StartupGamepads, IN_GamepadsMove) keep
 * their C signatures so CoD4x's IN_Frame (win_input.c) is unchanged. The
 * Stage 3A analog-stick handlers (right stick -> view via CL_MouseEvent,
 * left stick -> 8-way arrow keys) stay intact in this file and will be
 * replaced by the proper usercmd_s path in Phase 3-C.
 *
 * Hook strategy
 * -------------
 * iw3sp_mod installs HOOK_CALL at iw3mp 0x576193 to splice IN_Frame_Hk
 * into the engine's per-frame input pump. We DO NOT install that hook:
 * CoD4x already redirects iw3mp's entire IN_Frame at 0x452A44
 * (sys_patch.c) and calls our IN_GamepadsMove() from win_input.c:438.
 * See gamepad_internal.h, IW3MP_IN_FRAME_CALL_LEGACY for the
 * cross-reference.
 *
 * The original Stage 3A header (Commit 2 + Stage 3A sticks) is preserved
 * in git history at tag stage3a-complete.
 *
 * Path A vs Path B: Phase 3-B is "Path A" -- all button delivery still
 * lowers to Com_QueueEvent(SE_KEY, ...) just like Stage 3A, so config_mp.cfg
 * bindings on K_JOY1..K_JOY16 remain valid. Path B (engine playerKeys[]
 * + Cbuf_AddText by iw3mp VA) lands in Stage 3-D when aim assist forces
 * us to touch engine internals anyway.
 *
 * iw3sp_mod attribution: this file does not itself port code from
 * iw3sp_mod (the iw3sp_mod logic lives in the sister files
 * gamepad_poll.c and gamepad_buttons.c). The Stage 3A code below is
 * original.
 */

#include "q_shared.h"
#include "win_sys.h"
#include "qcommon.h"
#include "keycodes.h"
#include "cl_input.h"
#include "gamepad.h"
#include "gamepad_internal.h"   /* gp_state, gp_raw, gp_poll_all, dispatch */
#include "gamepad_hooks.h"      /* GP_HOOK_CALL */

#include <windows.h>
#include <xinput.h>
#include <math.h>

// XInput thumbstick full-scale value.
#define THUMB_MAX          32767.0f

// Converts a normalized stick magnitude (0..1, after sensitivity) into
// mouse-like per-frame counts for CL_MouseEvent. Primary look-feel knob.
#define GAMEPAD_LOOK_SCALE 12.0f

// Controller settings. All CVAR_ARCHIVE so they persist to config_mp.cfg.
// Four cvars are shared with gamepad_move.c (Phase 3-C) and thus
// non-static; their `extern` declarations live in gamepad_internal.h.
cvar_t *cl_gamepad;
cvar_t *cl_gamepad_sens_look;
cvar_t *cl_gamepad_deadzone_left;
cvar_t *cl_gamepad_deadzone_right;
cvar_t *cl_gamepad_legacy_sticks;   // Phase 3-C: 1 = Stage 3A path, 0 = new usercmd path
cvar_t *cl_gamepad_invert_pitch;    // Phase 3-C: inverts pitchmove in the usercmd path

// gamepad.c-internal cvars (only read in this TU).
static cvar_t *cl_gamepad_sens_ads;
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

// Stage 3A stick-only bookkeeping. The button/trigger fields used in
// the old gamepadState_t are gone -- gp_state[0] (gamepad_poll.c) owns
// them now. Only the left-stick directional cache survives, used by
// Gamepad_ApplyLeftStick below for edge-event generation. This whole
// block goes away in Phase 3-C when sticks move to usercmd_s.
typedef struct
{
    qboolean was_enabled;     // gp_state[0].enabled from the previous frame
    qboolean prev_move_fwd;
    qboolean prev_move_back;
    qboolean prev_move_left;
    qboolean prev_move_right;
} gamepadStickState_t;

static gamepadStickState_t s_stick;

/*
===========
IN_StartupGamepads

Registers the controller cvars and resets per-stick state tracking.
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

    // Phase 3-C cvars (NOT YET active -- the usercmd hook is installed
    // in Phase 3-C.4 in a follow-up commit. Defaults are picked so the
    // Stage 3A behavior remains the user-visible path until the new
    // path is live + smoke-tested).
    cl_gamepad_legacy_sticks = Cvar_RegisterBool(
        "cl_gamepad_legacy_sticks", qtrue, CVAR_ARCHIVE,
        "Use Stage 3A stick handling (CL_MouseEvent + arrow keys). "
        "Set to 0 to enable the Phase 3-C analog usercmd path "
        "(requires the IN_GamepadsMove hook to be installed)");

    cl_gamepad_invert_pitch = Cvar_RegisterBool(
        "cl_gamepad_invert_pitch", qfalse, CVAR_ARCHIVE,
        "Invert the controller pitch axis in the Phase 3-C usercmd path "
        "(independent of cl_gamepad_invert_y which is the Stage 3A "
        "CL_MouseEvent-layer toggle)");

    s_stick.was_enabled    = qfalse;
    s_stick.prev_move_fwd  = qfalse;
    s_stick.prev_move_back = qfalse;
    s_stick.prev_move_left = qfalse;
    s_stick.prev_move_right = qfalse;

    /* Phase 3-C.4: redirect engine's CL_MouseMove call site (0x463D70,
     * originally CALL 0x463490) to our dispatcher. We use GP_HOOK_CALL
     * (not JMP) because the original instruction is a CALL -- the engine
     * pushes `cmd` and expects cdecl return semantics. gp_cl_mousemove
     * stays dormant until cl_gamepad_legacy_sticks==0 (default==1). */
    GP_HOOK_CALL(IW3MP_CL_MOUSEMOVE_STUB_JMP, gp_cl_mousemove);

    Com_Printf(CON_CHANNEL_SYSTEM, "XInput initialized\n");
}

/*
===========
Gamepad_ReleaseMovement

Sends a release event for any held left-stick movement direction and
clears the cached states. Called on controller disconnect transitions
to make sure no arrow key stays stuck "down" in the engine.

Note: button/trigger releases are handled by gp_release_all (see
gamepad_buttons.c). This function covers only the stick-driven movement
that still lives in this file until Phase 3-C.
===========
*/
static void Gamepad_ReleaseMovement(void)
{
    if ( s_stick.prev_move_fwd )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_UPARROW, qfalse, 0, NULL );
    if ( s_stick.prev_move_back )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_DOWNARROW, qfalse, 0, NULL );
    if ( s_stick.prev_move_left )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_LEFTARROW, qfalse, 0, NULL );
    if ( s_stick.prev_move_right )
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_RIGHTARROW, qfalse, 0, NULL );

    s_stick.prev_move_fwd   = qfalse;
    s_stick.prev_move_back  = qfalse;
    s_stick.prev_move_left  = qfalse;
    s_stick.prev_move_right = qfalse;
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

    if ( fwd != s_stick.prev_move_fwd )
    {
        if ( cl_gamepad_debug->boolean )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick L: lx=%d ly=%d fwd=%d (was %d) -> K_UPARROW %s\n",
                thumb_lx, thumb_ly, fwd, s_stick.prev_move_fwd,
                fwd ? "pressed" : "released");
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_UPARROW, fwd, 0, NULL );
        s_stick.prev_move_fwd = fwd;
    }
    if ( back != s_stick.prev_move_back )
    {
        if ( cl_gamepad_debug->boolean )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick L: lx=%d ly=%d back=%d (was %d) -> K_DOWNARROW %s\n",
                thumb_lx, thumb_ly, back, s_stick.prev_move_back,
                back ? "pressed" : "released");
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_DOWNARROW, back, 0, NULL );
        s_stick.prev_move_back = back;
    }
    if ( left != s_stick.prev_move_left )
    {
        if ( cl_gamepad_debug->boolean )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick L: lx=%d ly=%d left=%d (was %d) -> K_LEFTARROW %s\n",
                thumb_lx, thumb_ly, left, s_stick.prev_move_left,
                left ? "pressed" : "released");
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_LEFTARROW, left, 0, NULL );
        s_stick.prev_move_left = left;
    }
    if ( right != s_stick.prev_move_right )
    {
        if ( cl_gamepad_debug->boolean )
            Com_Printf(CON_CHANNEL_SYSTEM,
                "Stick L: lx=%d ly=%d right=%d (was %d) -> K_RIGHTARROW %s\n",
                thumb_lx, thumb_ly, right, s_stick.prev_move_right,
                right ? "pressed" : "released");
        Com_QueueEvent( g_wv.sysMsgTime, SE_KEY, K_RIGHTARROW, right, 0, NULL );
        s_stick.prev_move_right = right;
    }
}

/*
===========
UpdateTheButtonAHint

iw3sp_mod walks the menu list every frame and re-aligns the "BUTTON A"
hint glyph between PC and console offsets depending on whether a
gamepad is currently in use. We have no menu integration yet, so this
is intentionally a no-op for Phase 3-B.

TODO Phase 4 (menu integration): port the alignment loop from
iw3sp_mod Gamepad.cpp:292-323 once we have a way to walk the
CoD4x menu tree (uiInfo->uiDC.menuCount).
===========
*/
static void UpdateTheButtonAHint(void)
{
    /* intentionally empty -- Phase 4 */
}

/*
===========
IN_GamepadsMove

Per-frame controller poll. Called from IN_Frame() (win_input.c). The
button/trigger pipeline lives in the gp_* state machine
(gamepad_poll.c + gamepad_buttons.c); the stick pipeline is the
Stage 3A code above (replaced in Phase 3-C).
===========
*/
void IN_GamepadsMove(void)
{
    qboolean enabled_now;

    UpdateTheButtonAHint();

    // Feature guard: zero cost / zero XInput calls when disabled.
    if ( !cl_gamepad->boolean )
        return;

    gp_poll_all();

    enabled_now = gp_state[0].enabled ? qtrue : qfalse;

    if ( !enabled_now )
    {
        // Pad just unplugged this frame: gp_poll_all already released
        // the held buttons/triggers via gp_release_all. Release any
        // held stick movement too so no arrow key stays stuck down.
        if ( s_stick.was_enabled )
        {
            Gamepad_ReleaseMovement();
            s_stick.was_enabled = qfalse;
        }
        return;
    }

    s_stick.was_enabled = qtrue;

    // Buttons + triggers: edge-detected by the state machine, delivered
    // via Com_QueueEvent (Path A).
    gp_dispatch_buttons(0);

    // Right stick -> view. ALWAYS active: it feeds the CL_MouseEvent
    // accumulator that the engine CL_MouseMove consumes. In Phase 3-C's
    // hybrid (Option A) this is the look source in BOTH modes -- at
    // legacy=0 the gamepad dispatcher (gp_cl_mousemove) still calls the
    // original CL_MouseMove, so this accumulator is what rotates the view.
    Gamepad_ApplyRightStick( gp_raw[0].sThumbRX, gp_raw[0].sThumbRY );

    // Left stick -> movement. At legacy=1 (Stage 3A) it fires arrow-key
    // events here. At legacy=0 the Phase 3-C usercmd path
    // (gp_cl_gamepadmove) writes forwardmove/rightmove instead, so we
    // must NOT also fire arrow keys -- that would double-apply movement.
    if ( cl_gamepad_legacy_sticks->boolean )
        Gamepad_ApplyLeftStick( gp_raw[0].sThumbLX, gp_raw[0].sThumbLY );
}

/*
===========
gp_cl_keyevent

Phase 3-E.1: SE_KEY dispatch wrapper, installed at Com_EventLoop's
SE_KEY case (common.c) in place of the engine CL_KeyEvent. iw3sp_mod
hooks the engine key router (CL_KeyEvent_Hk @ iw3mp 0x4FDCBF) for this,
but that router is SUPERSEDED in CoD4x -- Com_EventLoop owns the key
loop and calls the engine CL_KeyEvent directly. So we wrap it here
instead (same pattern as IN_Frame in Phase 3-C).

Purpose: when a keyboard/mouse key is pressed, clear the controller
in-use flag so the game leaves controller mode. Our own gamepad buttons
travel the SAME Com_QueueEvent path as K_JOY1..K_JOY16 (Path A), so we
must NOT clear inUse for them -- they are what SET it in
gp_dispatch_buttons.

CL_KeyEvent is __cdecl (verified: iw3mp 0x467EB0 reads all args from
the stack; call site 0x4FDCBF cleans 16 bytes for 4 args).
===========
*/
void __cdecl gp_cl_keyevent(int localClientNum, int key, int down, unsigned time)
{
    /* Optional one-shot diagnostics, gated behind cl_gamepad_debug, kept
     * minimal for future bring-up (no per-event spam). */
    static int once_kb = 0;
    static int once_pad = 0;

    if ( key < K_JOY1 || key > K_JOY16 )
    {
        // Keyboard / mouse / other key -> leave controller mode.
        gp_state[0].inUse = false;

        if ( !once_kb && cl_gamepad_debug && cl_gamepad_debug->boolean )
        {
            once_kb = 1;
            Com_Printf(CON_CHANNEL_SYSTEM,
                "[gp] inUse=false (keyboard/mouse, key=%d)\n", key);
        }
    }
    else
    {
        // Gamepad button (K_JOY1..16) -> pass through, inUse unchanged.
        if ( !once_pad && cl_gamepad_debug && cl_gamepad_debug->boolean )
        {
            once_pad = 1;
            Com_Printf(CON_CHANNEL_SYSTEM,
                "[gp] gamepad key passthrough (key=%d, inUse unchanged)\n", key);
        }
    }

    CL_KeyEvent( localClientNum, key, down, time );
}
