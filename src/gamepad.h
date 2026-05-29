#ifndef __GAMEPAD_H__
#define __GAMEPAD_H__

// Native game controller support (Stage A).

// Registers the cl_gamepad_* cvars. Called once from IN_Startup().
void IN_StartupGamepads(void);

// Per-frame controller poll. Called from IN_Frame().
void IN_GamepadsMove(void);

// Phase 3-E.1: SE_KEY dispatch wrapper. Called from Com_EventLoop in
// place of the engine CL_KeyEvent. Clears the controller in-use flag
// for keyboard/mouse keys (so the game leaves controller mode when the
// keyboard is touched), then forwards to the engine CL_KeyEvent. Gamepad
// keys (K_JOY1..K_JOY16) pass through with inUse untouched -- they are
// what SET inUse, so they must not clear it (Path A nuance: our gamepad
// buttons travel the same Com_QueueEvent path as keyboard keys).
void __cdecl gp_cl_keyevent(int localClientNum, int key, int down, unsigned time);

// Phase 3-E.2: build the combined keyName table (stock + gamepad names)
// and repoint the engine's name->keynum lookup at it. Called once from
// IN_StartupGamepads. Idempotent.
void gp_install_keynames(void);

// Phase 3-E.2: keynum->name fallback for CoD4x's reimplemented
// Key_KeynumToString (cl_keys.c). Returns "BUTTON_A".. for gamepad
// keynums, or NULL if `keynum` is not a gamepad button.
const char *gp_keynum_to_name(int keynum);

#endif // __GAMEPAD_H__
