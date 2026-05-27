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
 * All four macros assume the iw3mp.exe .text section has already been
 * unprotected by the caller (this matches how CoD4x's existing
 * SetCall/SetJump are used), OR Patch_SetPtr/GP_HOOK_SET_PTR will
 * self-unprotect via VirtualProtect (matching Utils::Hook::Set<T>
 * semantics). See sys_patch.c for which primitives self-unprotect.
 */

#ifndef __GAMEPAD_HOOKS_H__
#define __GAMEPAD_HOOKS_H__

#include "sys_patch.h"   /* SetCall, SetJump, Patch_Memset, Patch_SetPtr */

/* Install a 5-byte JMP rel32 at iw3mp address `addr` to function `fn`.
 * `fn` may be any function pointer; cast is implicit via (void*). */
#define GP_HOOK_JUMP(addr, fn) \
    SetJump((DWORD)(addr), (void *)(fn))

/* Install a 5-byte CALL rel32 at iw3mp address `addr` to function `fn`. */
#define GP_HOOK_CALL(addr, fn) \
    SetCall((DWORD)(addr), (void *)(fn))

/* Overwrite a pointer-sized value at iw3mp address `addr` with `value`.
 * Self-unprotects (calls VirtualProtect with PAGE_EXECUTE_READWRITE
 * around the write). Equivalent to iw3sp_mod's Utils::Hook::Set<T*>. */
#define GP_HOOK_SET_PTR(addr, value) \
    Patch_SetPtr((DWORD)(addr), (void *)(value))

/* NOP-fill `n` bytes starting at iw3mp address `addr`. Used to clear
 * the tail of instructions we replaced with a 5-byte jmp. */
#define GP_HOOK_NOP(addr, n) \
    Patch_Memset((void *)(DWORD)(addr), 0x90, (size_t)(n))

#endif /* __GAMEPAD_HOOKS_H__ */
