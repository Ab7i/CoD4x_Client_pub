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
#include "gamepad_internal.h"   /* gp_keyname_t, GAME_K_* */
#include "gamepad_hooks.h"      /* GP_HOOK_SET_PTR -> Patch_SetPtr */

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
