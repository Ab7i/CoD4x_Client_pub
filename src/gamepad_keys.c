/*
 * gamepad_keys.c -- Phase 3-E.2: extend the engine keyName table so the
 * controller buttons have real names (BUTTON_A, DPAD_UP, ...) for
 * binding + display, instead of "0x01"-style hex.
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
 * Two consumers of the keyName data, two owners (see
 * notes/stage3e2-keyname-table.md):
 *   1. ENGINE name->keynum (Key_StringToKeynum @ iw3mp 0x4676F0/0x4677C0)
 *      -- used by the `bind <name>` parser. We repoint it to our
 *      combined table via 3 Set<keyname_t*> operand patches
 *      (Patch_SetPtr). Pre-flight DumpKeynameSites confirmed all three
 *      operands hold 0x726F48.
 *   2. CoD4x keynum->name (Key_KeynumToString in cl_keys.c) -- CoD4x
 *      REIMPLEMENTS this, reading the stock table at a hardcoded
 *      0x726F48, so the Set patches do NOT reach it. cl_keys.c calls
 *      gp_keynum_to_name() (below) as an additive fallback.
 *
 * Localized / glyph table (keynames_translated @ 0x727248) is Phase
 * 3-E.6 (naked GetLocalizedKeyName stubs); NOT handled here.
 */

#include "q_shared.h"
#include "qcommon.h"
#include "keys.h"               /* playerKeys (0x8F1CA0), qkey_t.binding */
#include "gamepad_internal.h"   /* gp_keyname_t, GAME_K_*, IW3MP_KEY_GETCMDASSIGN_JMP */
#include "gamepad_hooks.h"      /* GP_HOOK_SET_PTR, GP_HOOK_JUMP */

#include <string.h>             /* strcmp */

/* Stock english keyName table in iw3mp (.data). 95 entries + null,
 * confirmed by DumpKeynameSites.java. */
#define GP_STOCK_KEYNAMES   ((const gp_keyname_t *)0x726F48u)

/* The 3 engine operand sites that load the keyName table pointer
 * (Key_StringToKeynum / Key_KeynumToString in the engine). */
#define GP_KEYNAME_SITE_1   0x46777Du
#define GP_KEYNAME_SITE_2   0x467785u
#define GP_KEYNAME_SITE_3   0x467837u

/* Gamepad button names appended to the table. Names + keynums match
 * iw3sp_mod's extendedKeyNames (Gamepad.cpp:92) and the engine's
 * keynum_t (GAME_K_* in gamepad_internal.h). These are the strings the
 * `bind` command + the controls UI will use. */
static const gp_keyname_t gp_extended_keynames[] =
{
    { "BUTTON_A",       GAME_K_BUTTON_A      },
    { "BUTTON_B",       GAME_K_BUTTON_B      },
    { "BUTTON_X",       GAME_K_BUTTON_X      },
    { "BUTTON_Y",       GAME_K_BUTTON_Y      },
    { "BUTTON_LSHLDR",  GAME_K_BUTTON_LSHLDR },
    { "BUTTON_RSHLDR",  GAME_K_BUTTON_RSHLDR },
    { "BUTTON_START",   GAME_K_BUTTON_START  },
    { "BUTTON_BACK",    GAME_K_BUTTON_BACK   },
    { "BUTTON_LSTICK",  GAME_K_BUTTON_LSTICK },
    { "BUTTON_RSTICK",  GAME_K_BUTTON_RSTICK },
    { "BUTTON_LTRIG",   GAME_K_BUTTON_LTRIG  },
    { "BUTTON_RTRIG",   GAME_K_BUTTON_RTRIG  },
    { "DPAD_UP",        GAME_K_DPAD_UP       },
    { "DPAD_DOWN",      GAME_K_DPAD_DOWN     },
    { "DPAD_LEFT",      GAME_K_DPAD_LEFT     },
    { "DPAD_RIGHT",     GAME_K_DPAD_RIGHT    },
};

#define GP_EXTENDED_COUNT \
    ( (int)(sizeof(gp_extended_keynames) / sizeof(gp_extended_keynames[0])) )

/* Combined table: stock entries (copied at runtime) + extended + null.
 * Sized generously; the engine holds the pointer indefinitely, so this
 * MUST be static/global (never freed). */
#define GP_COMBINED_MAX  160
static gp_keyname_t gp_combined_keynames[GP_COMBINED_MAX];
static qboolean     gp_keynames_installed = qfalse;

/*
===========
gp_install_keynames

Build the combined keyName table (stock copied until null + the 16
gamepad names + null) and repoint the engine's name->keynum lookup at
it via the 3 Set operand patches. Idempotent (safe across in_restart).
Call once from IN_StartupGamepads.
===========
*/
void gp_install_keynames(void)
{
    const gp_keyname_t *src;
    int n = 0;
    int i;

    if ( gp_keynames_installed )
        return;

    /* Copy the stock english table until its null terminator, leaving
     * room for the extended block + null. */
    src = GP_STOCK_KEYNAMES;
    while ( src->name && n < (GP_COMBINED_MAX - GP_EXTENDED_COUNT - 1) )
    {
        gp_combined_keynames[n].name   = src->name;
        gp_combined_keynames[n].keynum = src->keynum;
        n++;
        src++;
    }

    /* Append the gamepad button names. */
    for ( i = 0; i < GP_EXTENDED_COUNT; ++i )
    {
        gp_combined_keynames[n].name   = gp_extended_keynames[i].name;
        gp_combined_keynames[n].keynum = gp_extended_keynames[i].keynum;
        n++;
    }

    /* Null-terminate. */
    gp_combined_keynames[n].name   = NULL;
    gp_combined_keynames[n].keynum = 0;

    /* Repoint the engine's 3 keyName-table loads at our combined table.
     * Self-unprotecting (Patch_SetPtr). */
    GP_HOOK_SET_PTR( GP_KEYNAME_SITE_1, gp_combined_keynames );
    GP_HOOK_SET_PTR( GP_KEYNAME_SITE_2, gp_combined_keynames );
    GP_HOOK_SET_PTR( GP_KEYNAME_SITE_3, gp_combined_keynames );

    gp_keynames_installed = qtrue;

    Com_Printf(CON_CHANNEL_SYSTEM,
        "Gamepad keyNames: %d stock + %d gamepad = %d entries installed\n",
        n - GP_EXTENDED_COUNT, GP_EXTENDED_COUNT, n);
}

/*
===========
gp_keynum_to_name

Additive fallback for CoD4x's Key_KeynumToString (cl_keys.c), which
reimplements keynum->name against the STOCK table and so never sees our
extended entries. Returns the gamepad button name for the gamepad
keynums (BUTTON_* and DPAD_*), or NULL if not a gamepad keynum (caller
keeps its existing behavior).
===========
*/
const char *gp_keynum_to_name(int keynum)
{
    int i;
    for ( i = 0; i < GP_EXTENDED_COUNT; ++i )
    {
        if ( gp_extended_keynames[i].keynum == keynum )
            return gp_extended_keynames[i].name;
    }
    return NULL;
}

/* ===================================================================
 * Phase 3-E.3: Key_GetCommandAssignmentInternal (engine 0x4678E0)
 *
 * The engine's "which key is bound to command X" lookup, made
 * gamepad-aware (iw3sp_mod Gamepad.cpp:1003). Installed as a full
 * entry-replacement via a naked trampoline (gamepad_stubs.asm) ->
 * __usercall arg1 (localClientNum) marshalled to this __cdecl fn.
 *
 * Reads the engine playerKeys exposed by CoD4x keys.h
 * (#define playerKeys (*((PlayerKeyState_t*)0x8F1CA0))), so NO new
 * engine-ABI reverse-engineering. MAX_LOCAL_CLIENTS==1 -> localClientNum
 * is always 0; the single playerKeys instance suffices.
 *
 * NOTE (keynum space): this operates on ENGINE keynums (BUTTON_A=0x1 ..
 * DPAD_RIGHT=0x17), matching the 3-E.2 keyName table and iw3sp_mod --
 * NOT the K_JOY1..16 that Path A currently emits. Until the input path
 * migrates from K_JOY to engine gamepad keynums, bindings made on
 * BUTTON_* won't fire from physical presses; this function is the
 * lookup foundation for that migration.
 * =================================================================== */

#define GP_K_LAST_KEY  0xDF   /* iw3sp_mod K_LAST_KEY */

/* iw3sp_mod GetGamePadCommand (Gamepad.cpp:988): remap a few shared
 * commands to their gamepad-combined equivalents. */
const char *gp_get_gamepad_command(const char *command)
{
    if ( strcmp(command, "+activate") == 0 || strcmp(command, "+reload") == 0 )
        return "+usereload";
    if ( strcmp(command, "+melee_breath") == 0 )
        return "+holdbreath";
    return command;
}

/* iw3sp_mod Key_IsValidGamePadChar (Gamepad.cpp:1473): true for the
 * engine gamepad keynum ranges (BUTTON_* and DPAD_*). */
qboolean gp_key_is_valid_gamepad_char(int key)
{
    return ( (key >= GAME_K_FIRSTGAMEPADBUTTON_RANGE_1 && key <= GAME_K_LASTGAMEPADBUTTON_RANGE_1)
          || (key >= GAME_K_FIRSTGAMEPADBUTTON_RANGE_2 && key <= GAME_K_LASTGAMEPADBUTTON_RANGE_2)
          || (key >= GAME_K_FIRSTGAMEPADBUTTON_RANGE_3 && key <= GAME_K_LASTGAMEPADBUTTON_RANGE_3) )
        ? qtrue : qfalse;
}

/* Engine entry replacement (called via gamepad_stubs.asm). __cdecl, 3
 * args. `keys` points to int[2]; fills with up to 2 matching keynums
 * (or -1), returns the count. When the pad is in use, look up gamepad
 * keynums for the remapped command; otherwise look up non-gamepad
 * keynums for the raw command. */
int __cdecl gp_key_getcmdassign(int localClientNum, const char *command, int *keys)
{
    int count = 0;
    int k;

    (void)localClientNum;   /* MAX_LOCAL_CLIENTS==1 -> always 0 */

    keys[0] = -1;
    keys[1] = -1;

    if ( !command )
        return 0;

    if ( gp_state[0].inUse )
    {
        const char *gpCmd = gp_get_gamepad_command(command);
        for ( k = 0; k < GP_K_LAST_KEY; ++k )
        {
            if ( !gp_key_is_valid_gamepad_char(k) )
                continue;
            if ( playerKeys.keys[k].binding
              && strcmp(playerKeys.keys[k].binding, gpCmd) == 0 )
            {
                keys[count++] = k;
                if ( count >= 2 )
                    return count;
            }
        }
    }
    else
    {
        for ( k = 0; k < GP_K_LAST_KEY; ++k )
        {
            if ( gp_key_is_valid_gamepad_char(k) )
                continue;
            if ( playerKeys.keys[k].binding
              && strcmp(playerKeys.keys[k].binding, command) == 0 )
            {
                keys[count++] = k;
                if ( count >= 2 )
                    return count;
            }
        }
    }

    return count;
}

/*
===========
gp_install_bindhooks

Phase 3-E.3: install the binding-related engine hooks. For now just the
Key_GetCommandAssignmentInternal entry replacement (naked trampoline).
Idempotent; called from IN_StartupGamepads after gp_install_keynames.
===========
*/
extern void gp_key_getcmdassign_stub(void);   /* gamepad_stubs.asm */

void gp_install_bindhooks(void)
{
    static qboolean installed = qfalse;
    if ( installed )
        return;

    GP_HOOK_JUMP( IW3MP_KEY_GETCMDASSIGN_JMP, gp_key_getcmdassign_stub );

    installed = qtrue;
    Com_Printf(CON_CHANNEL_SYSTEM,
        "Gamepad bind hooks installed (Key_GetCommandAssignment @ 0x%X)\n",
        (unsigned)IW3MP_KEY_GETCMDASSIGN_JMP);
}

/* ===================================================================
 * Phase 3-E.4a: Key_SetBinding hooks (3 CALL-site interceptions)
 *
 * The engine's Key_SetBinding (host iw3mp 0x552920) calls the inner
 * binder (0x4678b0) three times. We intercept ALL three with HOOK_CALL
 * (Patch_SetCall) so any binding that touches a gamepad keynum flips
 * gpad_buttonConfig to "custom". The original binding write is then
 * forwarded by gp_call_engine_keysetbinding (gamepad_stubs.asm) to the
 * inner binder (0x4678b0) with the original __usercall convention.
 *
 * Path A note: only the BIND API is hooked here -- physical button
 * presses still emit K_JOY1..16 (Path A). Migration of input emission
 * to engine keynums is the separate 3-E.4b sub-phase.
 * =================================================================== */

extern void gp_key_setbinding_stub01(void);   /* gamepad_stubs.asm */
extern void gp_key_setbinding_stub02(void);
extern void gp_key_setbinding_stub03(void);
extern void __cdecl gp_call_engine_keysetbinding(int lc, int keyNum,
                                                 const char *binding);

void __cdecl gp_key_setbinding_hk(int localClientNum, int keyNum, const char *binding)
{
    /* One-shot diagnostic so the first BUTTON_* bind in a session lands
     * in the console (gated by cl_gamepad_debug). */
    static int once = 0;
    qboolean isGamepad = gp_key_is_valid_gamepad_char(keyNum);

    if ( !once && cl_gamepad_debug && cl_gamepad_debug->boolean )
    {
        once = 1;
        Com_Printf(CON_CHANNEL_SYSTEM,
            "[gp] Key_SetBinding_Hk: lc=%d keyNum=%d binding=\"%s\" isGamepad=%d\n",
            localClientNum, keyNum,
            binding ? binding : "(null)",
            (int)isGamepad);
    }

    if ( isGamepad && gpad_buttonConfig )
    {
        Cvar_SetString( gpad_buttonConfig, "custom" );
    }

    /* Forward to the engine's inner binder so the binding is actually
     * stored. Preserves the original __usercall convention via the asm
     * helper. */
    gp_call_engine_keysetbinding( localClientNum, keyNum, binding );
}

void gp_install_keysetbinding_hooks(void)
{
    static qboolean installed = qfalse;
    if ( installed )
        return;

    /* HOOK_CALL (Patch_SetCall): the 3 sites are CALL instructions; we
     * only swap the call target. The engine's CALL still pushes the
     * return address, so each stub ends with a plain `ret`. */
    GP_HOOK_CALL( IW3MP_KEY_SETBIND_JMP_1, gp_key_setbinding_stub01 );
    GP_HOOK_CALL( IW3MP_KEY_SETBIND_JMP_2, gp_key_setbinding_stub02 );
    GP_HOOK_CALL( IW3MP_KEY_SETBIND_JMP_3, gp_key_setbinding_stub03 );

    installed = qtrue;
    Com_Printf(CON_CHANNEL_SYSTEM,
        "Gamepad Key_SetBinding hooks installed (3 sites: 0x%X, 0x%X, 0x%X)\n",
        (unsigned)IW3MP_KEY_SETBIND_JMP_1,
        (unsigned)IW3MP_KEY_SETBIND_JMP_2,
        (unsigned)IW3MP_KEY_SETBIND_JMP_3);
}
