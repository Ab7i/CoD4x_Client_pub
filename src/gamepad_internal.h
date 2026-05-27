/*
 * gamepad_internal.h -- internal types, constants, and iw3mp engine
 * addresses shared by every gamepad_*.c translation unit.
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
 * Stage 3-A: infrastructure only -- types + addresses, no behavior.
 *
 * NOTE on key codes: the GAME_K_* constants below are the keynum_t
 * values used internally by iw3mp.exe (the running engine). They are
 * DELIBERATELY different from CoD4x_Client_pub's keycodes.h (K_JOY1..32)
 * because we will be calling engine functions (Key_SetBinding, CL_KeyEvent)
 * at raw iw3mp addresses -- those functions expect engine-ABI keynums,
 * not CoD4x-internal keynums. Do not include keycodes.h with this file.
 */

#ifndef __GAMEPAD_INTERNAL_H__
#define __GAMEPAD_INTERNAL_H__

#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration so we can declare `extern cvar_t *...` below
 * without pulling all of q_shared.h into every TU that wants the
 * gamepad types. The full struct lives in cvar.h / q_shared.h and
 * is included by gamepad.c and gamepad_move.c via q_shared.h. */
struct cvar_s;
typedef struct cvar_s cvar_t;

/* ===================================================================
 * 1. Counts
 * =================================================================== */

#define GP_MAX_GPAD_COUNT       1   /* iw3mp supports exactly one local player */
#define GP_PHYSAXIS_COUNT       6   /* RSX, RSY, LSX, LSY, RTRIG, LTRIG    */
#define GP_VIRTAXIS_COUNT       6   /* SIDE, FORWARD, UP, YAW, PITCH, ATTACK */
#define GP_STICK_DIR_COUNT      4   /* up, down, left, right (per stick)   */
#define GP_MAP_COUNT            2   /* MAP_LINEAR, MAP_SQUARED             */

/* ===================================================================
 * 2. GamePadButton / GamePadStick bit-mask layout (matches engine ABI)
 *
 *   GamePadButton  = DIGITAL or ANALOG bit | XInput-style mask
 *   GamePadStick   = STICK   bit | small index 0..3
 *   GPAD_VALUE_MASK isolates the low 28 bits.
 * =================================================================== */

#define GP_VALUE_MASK           0x0FFFFFFFu
#define GP_DPAD_MASK            (XINPUT_GAMEPAD_DPAD_UP    \
                               | XINPUT_GAMEPAD_DPAD_DOWN  \
                               | XINPUT_GAMEPAD_DPAD_LEFT  \
                               | XINPUT_GAMEPAD_DPAD_RIGHT)
#define GP_DIGITAL_MASK         (1u << 28)
#define GP_ANALOG_MASK          (1u << 29)
#define GP_STICK_MASK           (1u << 30)

typedef enum {
    GPAD_NONE     = 0,

    GPAD_UP       = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_DPAD_UP        & GP_VALUE_MASK),
    GPAD_DOWN     = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_DPAD_DOWN      & GP_VALUE_MASK),
    GPAD_LEFT     = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_DPAD_LEFT      & GP_VALUE_MASK),
    GPAD_RIGHT    = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_DPAD_RIGHT     & GP_VALUE_MASK),
    GPAD_START    = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_START          & GP_VALUE_MASK),
    GPAD_BACK     = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_BACK           & GP_VALUE_MASK),
    GPAD_L3       = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_LEFT_THUMB     & GP_VALUE_MASK),
    GPAD_R3       = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_RIGHT_THUMB    & GP_VALUE_MASK),
    GPAD_L_SHLDR  = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_LEFT_SHOULDER  & GP_VALUE_MASK),
    GPAD_R_SHLDR  = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_RIGHT_SHOULDER & GP_VALUE_MASK),
    GPAD_A        = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_A              & GP_VALUE_MASK),
    GPAD_B        = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_B              & GP_VALUE_MASK),
    GPAD_X        = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_X              & GP_VALUE_MASK),
    GPAD_Y        = GP_DIGITAL_MASK | (XINPUT_GAMEPAD_Y              & GP_VALUE_MASK),

    GPAD_L_TRIG   = GP_ANALOG_MASK  | 0,
    GPAD_R_TRIG   = GP_ANALOG_MASK  | 1
} gp_button_e;

typedef enum {
    GPAD_INVALID = 0,
    GPAD_LX = GP_STICK_MASK | 0,
    GPAD_LY = GP_STICK_MASK | 1,
    GPAD_RX = GP_STICK_MASK | 2,
    GPAD_RY = GP_STICK_MASK | 3
} gp_stick_e;

typedef enum {
    GPAD_BUTTON_RELEASED = 0,
    GPAD_BUTTON_PRESSED  = 1,
    GPAD_BUTTON_UPDATE   = 2
} gp_button_event_e;

typedef enum {
    GPAD_PHYSAXIS_NONE     = -1,
    GPAD_PHYSAXIS_RSTICK_X =  0,
    GPAD_PHYSAXIS_RSTICK_Y =  1,
    GPAD_PHYSAXIS_LSTICK_X =  2,
    GPAD_PHYSAXIS_LSTICK_Y =  3,
    GPAD_PHYSAXIS_RTRIGGER =  4,
    GPAD_PHYSAXIS_LTRIGGER =  5
} gp_phys_axis_e;

typedef enum {
    GPAD_VIRTAXIS_NONE    = -1,
    GPAD_VIRTAXIS_SIDE    =  0,
    GPAD_VIRTAXIS_FORWARD =  1,
    GPAD_VIRTAXIS_UP      =  2,
    GPAD_VIRTAXIS_YAW     =  3,
    GPAD_VIRTAXIS_PITCH   =  4,
    GPAD_VIRTAXIS_ATTACK  =  5
} gp_virt_axis_e;

typedef enum {
    GPAD_MAP_NONE    = -1,
    GPAD_MAP_LINEAR  =  0,
    GPAD_MAP_SQUARED =  1
} gp_map_e;

typedef enum {
    GPAD_STICK_POS = 0,
    GPAD_STICK_NEG = 1
} gp_stick_dir_e;

/* ===================================================================
 * 3. Engine ABI keynum_t (values internal to iw3mp.exe)
 *
 *   Used only when interacting with engine functions at raw iw3mp
 *   addresses. See big NOTE in the file header.
 * =================================================================== */

#define GAME_K_NONE                          0x00
#define GAME_K_FIRSTGAMEPADBUTTON_RANGE_1    0x01
#define GAME_K_BUTTON_A                      0x01
#define GAME_K_BUTTON_B                      0x02
#define GAME_K_BUTTON_X                      0x03
#define GAME_K_BUTTON_Y                      0x04
#define GAME_K_BUTTON_LSHLDR                 0x05
#define GAME_K_BUTTON_RSHLDR                 0x06
#define GAME_K_LASTGAMEPADBUTTON_RANGE_1     0x06

#define GAME_K_FIRSTGAMEPADBUTTON_RANGE_2    0x0E
#define GAME_K_BUTTON_START                  0x0E
#define GAME_K_BUTTON_BACK                   0x0F
#define GAME_K_BUTTON_LSTICK                 0x10
#define GAME_K_BUTTON_RSTICK                 0x11
#define GAME_K_BUTTON_LTRIG                  0x12
#define GAME_K_BUTTON_RTRIG                  0x13
#define GAME_K_DPAD_UP                       0x14
#define GAME_K_FIRSTDPAD                     0x14
#define GAME_K_DPAD_DOWN                     0x15
#define GAME_K_DPAD_LEFT                     0x16
#define GAME_K_DPAD_RIGHT                    0x17
#define GAME_K_LASTDPAD                      0x17
#define GAME_K_LASTGAMEPADBUTTON_RANGE_2     0x17

#define GAME_K_FIRSTGAMEPADBUTTON_RANGE_3    0x1C
#define GAME_K_APAD_UP                       0x1C
#define GAME_K_FIRSTAPAD                     0x1C
#define GAME_K_APAD_DOWN                     0x1D
#define GAME_K_APAD_LEFT                     0x1E
#define GAME_K_APAD_RIGHT                    0x1F
#define GAME_K_LASTAPAD                      0x1F
#define GAME_K_LASTGAMEPADBUTTON_RANGE_3     0x1F

/* ===================================================================
 * 4. POD structs (engine ABI -- field order matters)
 * =================================================================== */

/* Matches iw3mp's keyname_t (Structs.hpp L5563 in iw3sp_mod ref). */
typedef struct {
    const char *name;
    int         keynum;
} gp_keyname_t;

/* Matches Game::ButtonToCodeMap_t (Structs.hpp L5842). */
typedef struct {
    gp_button_e padButton;
    int         code;       /* engine keynum (GAME_K_BUTTON_*) */
} gp_button_to_code_t;

/* Matches Game::StickToCodeMap_t (Structs.hpp L5794). */
typedef struct {
    gp_stick_e  stick;
    int         posCode;    /* engine keynum (GAME_K_APAD_*) */
    int         negCode;
} gp_stick_to_code_t;

/* GpadAxesGlob -- physical/virtual axis binding tables.
 * Field layout intentionally matches iw3mp memory layout because the
 * engine's Key_WriteBindings reads this struct directly at a known
 * address. See Phase 3-E when we wire this up. */
typedef struct {
    gp_phys_axis_e  physicalAxis;
    gp_map_e        mapType;
} gp_virtual_axis_t;

typedef struct {
    /* Polled physical-axis deflections in [-1..1], populated each frame
     * by gp_populate_axes() (defined in gamepad_move.c). Field order
     * matches iw3sp_mod's Game::GpadAxesGlob::axesValues -- index with
     * gp_phys_axis_e values (RSTICK_X..LTRIGGER). */
    float axesValues[GP_PHYSAXIS_COUNT];
    gp_virtual_axis_t virtualAxes[GP_VIRTAXIS_COUNT];
} gp_axes_glob_t;

/* ===================================================================
 * 5. Internal state (gamepad_*.c owns these)
 * =================================================================== */

typedef struct {
    bool                 enabled;
    bool                 inUse;
    int                  portIndex;
    unsigned short       digitals;
    unsigned short       lastDigitals;
    float                analogs[2];        /* triggers L,R (0..1) */
    float                lastAnalogs[2];
    float                sticks[4];         /* LX, LY, RX, RY      */
    float                lastSticks[4];
    bool                 stickDown[4][GP_STICK_DIR_COUNT];
    bool                 stickDownLast[4][GP_STICK_DIR_COUNT];
    float                lowRumble;
    float                highRumble;
    XINPUT_VIBRATION     rumble;
    XINPUT_CAPABILITIES  caps;
    bool                 previousState;     /* for setup hints */
} gp_state_t;

typedef struct {
    gp_axes_glob_t axes;
    unsigned       nextScrollTime;
} gp_globals_t;

/* ===================================================================
 * 5b. Engine usercmd_t -- mirror of iw3sp_mod's Game::usercmd_s
 *
 *   CoD4x's q_shared.h declares an incomplete `usercmd_t` (it names
 *   only field_16/17/18/1C as placeholders for the movement bytes).
 *   The engine's actual struct is the layout below; Stage 2 verified
 *   byte-identity vs iw3sp_mod's CL_MouseMove.
 *
 *   Phase 3-C casts CoD4x's `usercmd_t*` to `gp_usercmd_t*` inside
 *   our hook target so we can write the named fields directly without
 *   touching q_shared.h (which is shared by many TUs).
 *
 *   The pragma pack(1) matches CoD4x's q_shared.h pack; the iw3sp_mod
 *   layout naturally lacks padding between the char block and gunPitch
 *   because pitchmove+yawmove sit on a 4-byte boundary anyway, so the
 *   two layouts are byte-identical.
 * =================================================================== */

#pragma pack(push, 1)
typedef struct {
    int   serverTime;
    int   buttons;
    int   angles[3];
    char  weapon;
    char  offHandIndex;
    char  forwardmove;
    char  rightmove;
    char  upmove;
    char  pitchmove;
    char  yawmove;
    float gunPitch;
    float gunYaw;
    float gunXOfs;
    float gunYOfs;
    float gunZOfs;
    float meleeChargeYaw;
    char  meleeChargeDist;
    char  selectedLocation[2];
} gp_usercmd_t;
#pragma pack(pop)

typedef struct {
    const char *buttonA;
    const char *buttonB;
    const char *buttonX;
    const char *buttonY;
    const char *buttonBlack;
    const char *buttonWhite;
    const char *buttonDown;     /* D-pad */
    const char *buttonLTrig;
    const char *buttonRTrig;
    const char *buttonStart;
    const char *buttonBack;
    const char *buttonLStick;
    const char *buttonRStick;
} gp_button_mappings_t;

/* ===================================================================
 * 6. iw3mp hook sites (from stage3d-address-map.json -- 17 accepted)
 *
 *   _SITE_  = patch target (place where we install jmp/call)
 *   _FN_    = function entry (for direct calls / Set<T*>)
 *
 *   Comments cross-reference the iw3sp_mod source line in Gamepad.cpp
 *   so future port-PRs can audit each hook against the original.
 * =================================================================== */

/* --- direct CALLs to engine entries (we just need the function VA) --- */
#define IW3MP_CL_MOUSEMOVE_FN          0x463490u   /* Gamepad.cpp:963   */
#define IW3MP_CL_KEYEVENT_FN           0x467EB0u   /* Gamepad.cpp:1112  */

/* --- Set<T*> sites (overwrite a pointer literal in the .text/.data) --- */
#define IW3MP_KEYNAME_TABLE_PTR_1      (0x4676F0u + 0x8Du)   /* Gamepad.cpp:2105 */
#define IW3MP_KEYNAME_TABLE_PTR_2      (0x4676F0u + 0x95u)   /* Gamepad.cpp:2106 */
#define IW3MP_KEYNAME_TABLE_PTR_3      (0x4677C0u + 0x77u)   /* Gamepad.cpp:2107 */

/* --- HOOK_CALL sites (5-byte CALL rel32) --- */
#define IW3MP_GETLOCKEYNAME_CALL_1     (0x4677C0u + 0x6Fu)   /* Gamepad.cpp:2108 */
#define IW3MP_GETLOCKEYNAME_CALL_2     (0x475DC0u + 0x91u)   /* Gamepad.cpp:2109 */
#define IW3MP_CL_KEYEVENT_HK_CALL      0x4FDCBFu             /* Gamepad.cpp:2308 */

/* SUPERSEDED: iw3sp_mod installs IN_Frame_Hk here (Gamepad.cpp:2305) as a
 * HOOK_CALL replacing the engine's CALL to IN_MouseMove. In CoD4x this
 * hook is REDUNDANT and MUST NOT be installed: CoD4x already redirects
 * iw3mp's entire IN_Frame entry point via
 *     SetCall(0x452A44, IN_Frame);                    [sys_patch.c:1152]
 * and CoD4x's own IN_Frame in win_input.c:438 already calls our
 * IN_GamepadsMove() each frame. Kept here purely for cross-reference
 * against the iw3sp_mod source -- DO NOT install. */
#define IW3MP_IN_FRAME_CALL_LEGACY     0x576193u             /* Gamepad.cpp:2305 -- documented only */

/* --- HOOK_JUMP sites (5-byte JMP rel32) --- */
#define IW3MP_KEY_GETCMDASSIGN_JMP     0x4678E0u             /* Gamepad.cpp:2322 */
#define IW3MP_KEY_WRITEBIND_JMP        0x4FFB0Fu             /* Gamepad.cpp:2281 */
#define IW3MP_KEY_SETBIND_JMP_1        0x5529B8u             /* Gamepad.cpp:2325 */
#define IW3MP_KEY_SETBIND_JMP_2        0x5529CBu             /* Gamepad.cpp:2326 */
#define IW3MP_KEY_SETBIND_JMP_3        0x5529E3u             /* Gamepad.cpp:2327 */
#define IW3MP_CL_MOUSEMOVE_STUB_JMP    0x463D70u             /* Gamepad.cpp:2330 -- call-CL_MouseMove site inside its host 0x463D10 */

/* --- not yet resolved (Stage 3.5) ---
 *   IW3MP_PLAYER_USEENTITY_JMP      <Player_UseEntity_Stub host>
 *   IW3MP_UI_REFRESH_JMP            <UI_RefreshStub host>
 *   IW3MP_CL_MOUSEEVENT_STUB_JMP    <CL_MouseEvent_Stub host -- needs 0x43F920 twin>
 *   IW3MP_RELOAD_HINT_PTR           <0x4237B0 + byte-pattern-found offset>
 */

/* ===================================================================
 * 7. Phase 3-B polling API (defined in gamepad_poll.c)
 *
 *   Path A: state machine lives in our DLL; engine ABI is NOT touched.
 *   The dispatch layer (gamepad_buttons.c) and the existing stick code
 *   (gamepad.c) read from these globals.
 * =================================================================== */

/* Canonical owner: gamepad_poll.c. Index = port (only port 0 used today). */
extern gp_state_t      gp_state[GP_MAX_GPAD_COUNT];

/* Raw XInput state cached by the poller for re-use by the stick code in
 * gamepad.c (avoids a second XInputGetState per frame). Valid only when
 * gp_state[port].enabled is true. */
extern XINPUT_GAMEPAD  gp_raw[GP_MAX_GPAD_COUNT];

/* Per-frame poll: refresh device connectivity (throttled), pump
 * XInputGetState, update digitals / analogs / sticks edge cache.
 * Safe to call when cl_gamepad is disabled (returns early). */
void gp_poll_all(void);

/* Edge-detect helpers (port = 0..GP_MAX_GPAD_COUNT-1). */
float gp_get_button       (int port, gp_button_e button);
int   gp_is_button_pressed (int port, gp_button_e button);
int   gp_is_button_released(int port, gp_button_e button);
int   gp_button_requires_updates(int port, gp_button_e button);
float gp_get_stick        (int port, gp_stick_e  stick);

/* ===================================================================
 * 8. Phase 3-B dispatch API (defined in gamepad_buttons.c)
 *
 *   Path A delivery via Com_QueueEvent(SE_KEY, ...). The button table
 *   here maps gp_button_e -> CoD4x keynum (K_JOY1..K_JOY16) to keep
 *   compatibility with config_mp.cfg bindings shipped in Stage 3A.
 * =================================================================== */

/* Drive Com_QueueEvent based on per-frame edges in gp_state[port]. */
void gp_dispatch_buttons(int port);

/* Force-release every input still cached as held; clear lastDigitals/
 * lastAnalogs. Called on disconnect transition so the engine doesn't
 * see a frozen "down" key. */
void gp_release_all(int port);

/* ===================================================================
 * 9. Phase 3-C movement API (defined in gamepad_move.c)
 *
 *   `gp_globals[0].axes.axesValues[]` is the canonical per-frame
 *   deflection table that CL_GamepadAxisValue reads through. It is
 *   populated by gp_populate_axes() before each CL_GamepadMove call.
 *
 *   `gp_cl_mousemove` is the hook target installed at iw3mp
 *   0x463D70 in Phase 3-C.4 (NOT installed yet -- this file declares
 *   the function so gamepad_move.c can compile without callers).
 * =================================================================== */

extern gp_globals_t gp_globals[GP_MAX_GPAD_COUNT];

/* Copy gp_state[port].sticks[] + analogs[] into gp_globals[port]
 * .axes.axesValues[], applying the standard stick deadzone from
 * cl_gamepad_deadzone_{left,right}. Caller: gp_cl_gamepadmove. */
void gp_populate_axes(int port);

/* Phase 3-C.4 hook target -- replaces the engine's CALL CL_MouseMove
 * at iw3mp 0x463D70 (HOOK_JUMP). Either falls through to the original
 * CL_MouseMove (mouse path) or routes to gp_cl_gamepadmove
 * (gamepad path). __cdecl: pre-flight DumpCallSite verified the
 * engine adds ESP,4 after the call. */
void __cdecl gp_cl_mousemove(gp_usercmd_t *cmd);

/* Phase 3-C movement writer: reads gp_globals[0].axes.axesValues[]
 * via CL_GamepadAxisValue, applies sensitivity + move scale, writes
 * cmd->{forwardmove,rightmove,pitchmove,yawmove}. No aim assist
 * (deferred to Stage 3-D). */
void gp_cl_gamepadmove(gp_usercmd_t *cmd);

/* ===================================================================
 * 10. Phase 3-C cvars shared with gamepad_move.c
 *
 *   Defined in gamepad.c. Made non-static so gamepad_move.c can read
 *   them without going through Cvar_FindVar each frame.
 * =================================================================== */

extern cvar_t *cl_gamepad;
extern cvar_t *cl_gamepad_sens_look;
extern cvar_t *cl_gamepad_deadzone_left;
extern cvar_t *cl_gamepad_deadzone_right;
extern cvar_t *cl_gamepad_legacy_sticks;   /* new in 3-C */
extern cvar_t *cl_gamepad_invert_pitch;    /* new in 3-C */

#endif /* __GAMEPAD_INTERNAL_H__ */
