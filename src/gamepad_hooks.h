/*
 * gamepad_hooks.h -- ergonomic macros around CoD4x's sys_patch primitives,
 * shaped to look like iw3sp_mod's Utils::Hook API so ported code reads
 * almost line-for-line with the original C++.
 *
 * Derived from iw3sp_mod by JerryALT
 *   https://gitea.com/JerryALT/iw3sp_mod   (GPL-3.0)
 * Ported to CoD4x multiplayer by Ab7i
 *   https://github.com/Ab7i                (AGPL-3.0)
 *
 * Stage 3-A: infrastructure only. These macros expand to existing
 * CoD4x calls (SetJump, SetCall, Patch_Memset, Patch_SetPtr); they
 * install no behavior on their own.
 *
 * Translation table iw3sp_mod -> CoD4x
 *   Utils::Hook(A, fn, HOOK_JUMP).install()->quick();   GP_HOOK_JUMP(A, fn);
 *   Utils::Hook(A, fn, HOOK_CALL).install()->quick();   GP_HOOK_CALL(A, fn);
 *   Utils::Hook::Set<T>(A, value);                      GP_HOOK_SET_PTR(A, value);
 *   Utils::Hook::Nop(A, n);                             GP_HOOK_NOP(A, n);
 *   Utils::Hook::Call<T>(A)(args...);                   ((T)A)(args...)
 *
 * All four macros are SELF-UNPROTECTING via the Patch_* wrappers in
 * sys_patch.c (each VirtualProtects its patch site for the duration of
 * the write). This is required because the gamepad hooks install from
 * IN_StartupGamepads -- OUTSIDE CoD4x's Patch_MainModule unprotect
 * window -- so the raw SetCall/SetJump (which write .text directly and
 * assume an already-unprotected page) would fault. The earlier raw
 * mapping caused a startup crash in the first Phase 3-C.4 attempt;
 * routing through Patch_SetCall/Patch_SetJump fixes it.
 *
 *   Utils::Hook(A, fn, HOOK_JUMP) -> GP_HOOK_JUMP(A, fn) -> Patch_SetJump
 *   Utils::Hook(A, fn, HOOK_CALL) -> GP_HOOK_CALL(A, fn) -> Patch_SetCall
 *   Utils::Hook::Set<T>(A, value) -> GP_HOOK_SET_PTR(A, value) -> Patch_SetPtr
 *   Utils::Hook::Nop(A, n)        -> GP_HOOK_NOP(A, n)
 *   Utils::Hook::Call<T>(A)(...)  -> ((T)A)(...)
 *
 * NOTE: GP_HOOK_NOP still uses the raw Patch_Memset (no self-unprotect).
 * It is currently UNUSED. Before its first use (Stage 3-E, the
 * 0x5947A8 NOP-then-jump), add a self-unprotecting Patch_Nop in
 * sys_patch.c and point this macro at it.
 */

#ifndef __GAMEPAD_HOOKS_H__
#define __GAMEPAD_HOOKS_H__

#include "sys_patch.h"   /* Patch_SetCall, Patch_SetJump, Patch_SetPtr, Patch_Memset */

/* Install a 5-byte JMP rel32 at iw3mp address `addr` to function `fn`.
 * Self-unprotecting (Patch_SetJump). `fn` may be any function pointer. */
#define GP_HOOK_JUMP(addr, fn) \
    Patch_SetJump((DWORD)(addr), (void *)(fn))

/* Install a 5-byte CALL rel32 at iw3mp address `addr` to function `fn`.
 * Self-unprotecting (Patch_SetCall). */
#define GP_HOOK_CALL(addr, fn) \
    Patch_SetCall((DWORD)(addr), (void *)(fn))

/* Overwrite a pointer-sized value at iw3mp address `addr` with `value`.
 * Self-unprotects (Patch_SetPtr). Equivalent to iw3sp_mod's
 * Utils::Hook::Set<T*>. */
#define GP_HOOK_SET_PTR(addr, value) \
    Patch_SetPtr((DWORD)(addr), (void *)(value))

/* NOP-fill `n` bytes at `addr`. RAW (not self-unprotecting) -- unused
 * today; see the NOTE above before first use. */
#define GP_HOOK_NOP(addr, n) \
    Patch_Memset((void *)(DWORD)(addr), 0x90, (size_t)(n))

#endif /* __GAMEPAD_HOOKS_H__ */
