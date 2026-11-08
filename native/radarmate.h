/*
 * radarmate.h - the co-op partner on the HUD radar: the decision part (no game access), checked offline by tests/radarmate_test.c.
 *
 * How the stock game puts players on the radar (TimeShift.Exe patch 1.2, static): the multiplayer player list ([0xfcab18]: +0xfc entries of 0x84
 * bytes, +0x100 count; entry +0x40 -> the player object, +0x68 flags, +0x80 the radar record) gets a record per player from FUN_005F8820(list, i)
 * (stdcall, ret 8), called when a player joins a team (FUN_005F84E0: for the LOCAL player's join it refreshes every entry). FUN_005F8820 reads the
 * game option RadarType (game_options.ini 1 = ShowFriendly; gs_mp_modifier.cls: DM / CTF "Disable", KOT "ShowAll") at 0x5F8900 (FUN_00402E30,
 * the value in its 2nd argument): 0 -> no record at all; 1 -> no record for an "enemy" player (FUN_006002E0); 2 -> a record for everyone; never one
 * for the local player ([0xfc8180]). The record (FUN_006AEF70: object at +0x2C, sprite of elem_hud_main_512x512, colour by team) goes to the radar
 * (vtable +0 of [[0x1095ec8]]->vf 0x1c), and the radar's blip draw FUN_005B09A0 places it at the object's position.
 * Live (2026-11-08, mm_ts_pair.ps1, n100pair-20261108-234915): the host's radar never showed the client at 6-20 m (frames 100-140) while AI
 * blips appeared later: no record for the partner in a co-op session.
 *
 * The fix (coopsession.c): in a co-op session (1) the RadarType read inside FUN_005F8820 answers 2 (ShowAll: the partner is never an "enemy" there);
 * (2) every 2 s, an entry of the list that is a player other than the local one and has no record gets FUN_005F8820(list, i) (in case no team join
 * ran for it in this mode). The record's lifetime stays the game's (the entry's +0x80, removed with the entry).
 */
#ifndef RADARMATE_H
#define RADARMATE_H

#define RM_LIST_CELL   0xfcab18u   /* the multiplayer player list */
#define RM_LOCAL_CELL  0xfc8180u   /* the local player (HUD) */
#define RM_ENTRY_SIZE  0x84
#define RM_ENTRY_OBJ   0x40        /* -> pointer to the player object */
#define RM_ENTRY_FLAGS 0x68        /* bit 5: the entry is not in play (FUN_006002E0 skips it) */
#define RM_ENTRY_REC   0x80        /* the radar record (smart pointer), 0 = none */
#define RM_REGISTER    0x5f8820u   /* FUN_005F8820(list, index), stdcall, ret 8 */
#define RM_OPT_SITE    0x5f8900u   /* call FUN_00402E30 (thiscall(option, a1, int *value), ret 8): the RadarType read of FUN_005F8820 */
#define RM_OPT_GET     0x402e30u
#define RM_SHOW_ALL    2

typedef struct { unsigned obj, flags, rec; } rm_entry;

/* the stock rule of FUN_005F8820: does the entry get a record? (type = RadarType, enemy = FUN_006002E0's answer) */
static inline int rm_stock_registers(int type, int is_local, int enemy)
{
    if (type == 0 || is_local)
        return 0;
    if (type == 1 && enemy)
        return 0;
    return 1;
}

/* the RadarType FUN_005F8820 sees: the stock value outside a co-op session, ShowAll in one (unless switched off) */
static inline int rm_radar_type(int stock, int coop, int on) { return coop && on ? RM_SHOW_ALL : stock; }

/* the next entry that needs a record: a player object, not the local one, in play, without a record; -1 = none */
static inline int rm_next(const rm_entry *e, int n, unsigned local_obj)
{
    for (int i = 0; i < n; i++)
        if (e[i].obj && e[i].obj != local_obj && !(e[i].flags & 0x20) && !e[i].rec)
            return i;
    return -1;
}

#endif
