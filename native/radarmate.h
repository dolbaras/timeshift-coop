/*
 * radarmate.h - the co-op partner on the HUD radar: the decision part (no game access), checked offline by tests/radarmate_test.c.
 *
 * How the stock game puts players on the radar (TimeShift.Exe patch 1.2, static): the multiplayer player list ([0xfcab18]: +0xfc entries of 0x84
 * bytes, +0x100 count; entry +0x40 -> a cell holding the player object, +0x54 team, +0x68 flags, +0x80 the radar record) gets a record per player
 * from FUN_005F8820(list, i) (stdcall, ret 8), called when a player joins a team (FUN_005F84E0: for the LOCAL player's join it refreshes every
 * entry). FUN_005F8820 reads the game option RadarType (game_options.ini 1 = ShowFriendly; gs_mp_modifier.cls: DM / CTF "Disable", KOT "ShowAll")
 * at 0x5F8900 (FUN_00402E30, thiscall(option, a1, int *value), the value written through its 2nd argument and tested at 0x5f893c): 0 -> no record
 * at all; 1 -> no record for an "enemy" player (FUN_006002E0); 2 -> a record for everyone. Never one for the local player ([0xfc8180], 0x5f8999)
 * nor for an entry with flags bit 2 (0x5f89a5). The record (0x5c bytes, ctor FUN_006AEF70: refcount +4, the tracked object at +0x2C as a plain
 * pointer, sprite of elem_hud_main_512x512, colour by team) is kept in entry +0x80 and given to the radar: R = [0x1095ec8]->vf 0x1c (the radar's
 * record interface, radar +0x108), R->vf0 (FUN_005B07E0) adds it to the list at R +0x140 (nodes {record, next}; a record whose object is already
 * listed is not added twice), R->vf4 (FUN_005B0950) takes the one of an object out (FUN_005F8D50, a player leaving). The blip draw FUN_005B09A0
 * walks that list each HUD update: a record whose object is NULL is skipped, the others are drawn at their object's place relative to [0xfc8180].
 * Live (2026-11-08, mm_ts_pair.ps1, n100pair-20261108-234915): the host's radar never showed the client at 6-20 m (frames 100-140) while AI
 * blips appeared later: no record for the partner in a co-op session.
 *
 * The fix, network (coopsession.c): in a co-op session (1) the RadarType read inside FUN_005F8820 answers 2 (ShowAll: the partner is never an
 * "enemy" there); (2) every 2 s, an entry of the list that is a player other than the local one, not skipped by the stock rule and without a
 * record gets FUN_005F8820(list, i) (in case no team join ran for it in this mode), at most RM_MAX_TRIES times per object. The record's lifetime
 * stays the game's (entry +0x80, taken out with the entry).
 *
 * Both fixes paint the partner's record blue (rm_partner_colour).
 *
 * The fix, split screen (split.c): player 2 is a second local `player`, in no list. One record is made by the stock FUN_005F8820 on a list of
 * the mod's own (one entry whose cell names player 2, RadarType answered 2 for that call); its object is set only while a view is drawn (view 0,
 * player 1's: player 2; view 1, player 2's: player 1 - each half's radar is drawn relative to its own player, [0xfc8180] being swapped) and is
 * NULL at any other moment, so the radar never holds a pointer to a player that is gone. The mod keeps one reference; when the radar has dropped
 * its own (refcount < 2: a new HUD), a new record is made, the old one is left as it is (never released: its object is NULL).
 */
#ifndef RADARMATE_H
#define RADARMATE_H

#define RM_LIST_CELL   0xfcab18u   /* the multiplayer player list */
#define RM_LOCAL_CELL  0xfc8180u   /* the local player (HUD) */
#define RM_ENTRY_SIZE  0x84
#define RM_ENTRY_OBJ   0x40        /* -> a cell holding the player object */
#define RM_ENTRY_FLAGS 0x68        /* bit 2: no record (0x5f89a5); bit 5: not in play (FUN_006002E0 skips it) */
#define RM_ENTRY_REC   0x80        /* the radar record (smart pointer: the record's address), 0 = none */
#define RM_SKIP_FLAGS  0x24u
#define RM_REGISTER    0x5f8820u   /* FUN_005F8820(list, index), stdcall, ret 8 */
#define RM_OPT_SITE    0x5f8900u   /* call FUN_00402E30 (thiscall(option, a1, int *value), ret 8): the RadarType read of FUN_005F8820 */
#define RM_OPT_GET     0x402e30u
#define RM_SHOW_ALL    2
#define RM_MAX_TRIES   3
#define RM_HUD_CELL    0x1095ec8u  /* the HUD: vtable +0x1c -> the radar's record interface R */
#define RM_HUD_RADAR   0x1c
#define RM_R_LIST      0x140       /* R +0x140: the first node {record, next} */
#define RM_REC_REFS    4
#define RM_REC_OBJ     0x2c
#define RM_PLAYER_VTBL 0x00d72f78u /* the campaign class `player` */
#define RM_REC_COLOUR  0x34        /* 4 floats r g b a (0..255): the ctor copies its colour argument there, the blip draw reads it (0x5b0ab8) */
/* FUN_005F8820's colours (0x5f89bb..): red (255,64,64,255) = an enemy player (every player in the co-op session's game type, where no team sets the
 * friendly ones), green (70,211,70,255) = the friendly team, blue (78,93,255,255) = entry +0x54 == 1. Live (n100pair-20261109-034448): the partner's
 * blip was RED, the colour of the enemy soldiers in the campaign. The co-op partner is drawn blue: neither an enemy (red) nor a rebel (green). */
static const float rm_partner_colour[4] = {78.0f, 93.0f, 255.0f, 255.0f};
/* does a record need the partner colour? (its colour differs from it) */
static inline int rm_colour_differs(const float c[4])
{
    for (int k = 0; k < 4; k++)
        if (c[k] != rm_partner_colour[k])
            return 1;
    return 0;
}

typedef struct { unsigned obj, flags, rec, tries; } rm_entry;

/* the stock rule of FUN_005F8820: does the entry get a record? (type = RadarType, enemy = FUN_006002E0's answer) */
static inline int rm_stock_registers(int type, int is_local, int enemy)
{
    if (type == 0 || is_local)
        return 0;
    if (type == 1 && enemy)
        return 0;
    return 1;
}

/* the RadarType FUN_005F8820 sees: ShowAll for the mod's own call (force), or in a co-op session (unless switched off); else the stock value */
static inline int rm_radar_type(int stock, int coop, int on, int force)
{
    return force || (coop && on) ? RM_SHOW_ALL : stock;
}

/* the next entry that needs a record: a player object, not the local one, not skipped by the stock rule, without a record, not given up; -1 = none */
static inline int rm_next(const rm_entry *e, int n, unsigned local_obj)
{
    for (int i = 0; i < n; i++)
        if (e[i].obj && e[i].obj != local_obj && !(e[i].flags & RM_SKIP_FLAGS) && !e[i].rec && e[i].tries < RM_MAX_TRIES)
            return i;
    return -1;
}

/* split screen: the object of the shared record while view v is drawn (0 = player 1's view, 1 = player 2's); 0 = none (NULL) */
static inline unsigned rm_view_obj(int v, unsigned p1, unsigned p2)
{
    if (!p1 || !p2 || p1 == p2)
        return 0;
    return v == 0 ? p2 : v == 1 ? p1 : 0;
}

/* split screen: is a (new) record needed? (none yet, or the radar holds no reference to ours any more) */
static inline int rm_split_need(unsigned rec, int refs)
{
    return !rec || refs < 2;
}

#endif
