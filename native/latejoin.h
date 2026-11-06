/*
 * Late joiners (campaign.c, message 72): which clients get the host's player state and which parts of it.
 * The host records what the level scripts set on its own player since the level's run started (bits: 1 DisableMove,
 * 2 DisableInput, 4 Aimable, 8/16/32 DisableTC slow/stop/reverse, 64 SetupWeapon). A client that is seen running the
 * level for the first time gets it:
 * - more than 20 s into the level: everything, 3, 10 and 25 s later, the loadout only in the first send (as before);
 * - earlier: only when the scripts already changed the host's player state at that moment, without the loadout. The
 *   story events that did it ran on the host while the client still loaded the level, so their replays reached no client
 *   (live 2026-11-06, level 2: the story's DisableTC("time_reverse", 0) 13 s into the level, "sent ... to 0 client(s)";
 *   the client ran the level at 16.5 s and kept its level start's reverse lock: its reverse key never worked there).
 *   The loadout stays with the client's own level start and the co-op loadout carry-over (coopsave.c).
 * Kept in a header so tests/latejoin_test.c checks the very same code on the host.
 */
#ifndef TS_LATEJOIN_H
#define TS_LATEJOIN_H

#define LJ_LATE_MS 20000u
#define LJ_LOADOUT 64

/* How many sends a client gets that is seen running the level `since_start` ms after the level's run started, with
 * `mask_now` = the host's recorded bits at that moment; *early = 1 for a client within the first 20 s. */
static inline int latejoin_plan(unsigned since_start, int mask_now, int *early)
{
    *early = since_start <= LJ_LATE_MS;
    if (!*early)
        return 3;
    return (mask_now & ~LJ_LOADOUT) ? 3 : 0;
}

/* The bits of one send: `mask_now` the host's bits now, `first` = this is the client's first send. */
static inline int latejoin_bits(int mask_now, int early, int first)
{
    return early || !first ? mask_now & ~LJ_LOADOUT : mask_now;
}

#endif
