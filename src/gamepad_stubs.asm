; gamepad_stubs.asm -- naked-asm trampolines for the controller engine hooks.
;
; Derived from iw3sp_mod by JerryALT
;   https://gitea.com/JerryALT/iw3sp_mod   (GPL-3.0)
; Ported to CoD4x multiplayer by Ab7i
;   https://github.com/Ab7i                (AGPL-3.0)
; iw3sp_mod itself is derived from IW4x Client.
;
; Built with NASM -f win32 --prefix _ -Ox (see CMakeLists.txt NASM_FLAGS),
; so labels here are written WITHOUT the leading underscore; NASM adds it
; to match MinGW's cdecl C symbol decoration (gp_key_getcmdassign ->
; _gp_key_getcmdassign). Same convention as callbacks.asm /
; client_callbacks.asm.

; ---------------------------------------------------------------------
; gp_key_getcmdassign_stub -- Phase 3-E.3 entry trampoline for iw3mp
; Key_GetCommandAssignmentInternal @ 0x4678E0 (installed via HOOK_JUMP).
;
; The engine function is __usercall: arg1 (localClientNum) in EAX, arg2
; (command) + arg3 (keys[2]) on the stack. We FULLY re-implement it in C
; (gp_key_getcmdassign, __cdecl) and never resume the original body, so
; the trampoline ends with `ret` (not a jump-back) -- the 5-byte JMP that
; overwrote the entry's PUSH EBX + partial IMUL is irrelevant.
;
; 1:1 port of iw3sp_mod's Key_GetCommandAssignmentInternal_Stub
; (Gamepad.cpp:1056). Entry state (reached via JMP from 0x4678E0, so the
; engine CALLER's frame is intact):
;   EAX       = localClientNum (arg1, register)
;   [esp]     = caller return address
;   [esp+4]   = arg2 (command)
;   [esp+8]   = arg3 (keys)
;
; After `push eax` (+4) then `pushad` (+0x20 = 8 dwords), the literal
; offset 0x2C reaches arg3; after the first push drops esp by 4, the same
; 0x2C literal reaches arg2 (the classic esp-shift trick). EAX still holds
; localClientNum across pushad (pushad saves it but the register keeps its
; value), so the third push forwards arg1. The C return value (eax) is
; written into the saved-eax slot [esp+0x20] so it survives popad, then
; pop eax restores it as the function result before ret.
; ---------------------------------------------------------------------

SECTION .text
    extern gp_key_getcmdassign        ; int __cdecl(int lc, const char* cmd, int* keys)
    global gp_key_getcmdassign_stub

gp_key_getcmdassign_stub:
    push    eax                       ; save localClientNum (eax) -> slot at [esp+0x20] later
    pushad                            ; save all GPRs (0x20 bytes)

    push    dword [esp + 0x2C]        ; arg3 = keys      ([esp+0x20+0x4+0x8])
    push    dword [esp + 0x2C]        ; arg2 = command   (esp shifted -4, same literal)
    push    eax                       ; arg1 = localClientNum (still in eax)
    call    gp_key_getcmdassign       ; int __cdecl(lc, cmd, keys)
    add     esp, 0xC                  ; cdecl: caller cleans 3 args

    mov     dword [esp + 0x20], eax   ; overwrite the saved-eax slot with the return value

    popad                             ; restore GPRs
    pop     eax                       ; eax = return value (from the slot above)
    ret                               ; return to the engine caller
