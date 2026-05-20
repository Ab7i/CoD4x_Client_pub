#ifndef __GAMEPAD_H__
#define __GAMEPAD_H__

// Native game controller support (Stage A).

// Registers the cl_gamepad_* cvars. Called once from IN_Startup().
void IN_StartupGamepads(void);

// Per-frame controller poll. Called from IN_Frame().
void IN_GamepadsMove(void);

#endif // __GAMEPAD_H__
