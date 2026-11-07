/*
 * The host's player state for clients (campaign.c, message 72). The host records what the level scripts set on its own
 * player since the level's run started (bits: 1 DisableMove, 2 DisableInput, 4 Aimable, 8/16/32 DisableTC
 * slow/stop/reverse, 64 SetupWeapon).
 * - A client that starts running the level more than 20 s after it started gets all of it, 3, 10 and 25 s later, the
 *   loadout only in the first send (as before).
 * - The time-power permissions (DisableTC) go to every client in the level: whenever they changed on the host and have
 *   stayed so for 1 s, and the client runs the level for 3 s or more. A client runs the host's story events as replays,
 *   but those that ran while it still loaded the level reached nobody, and so did their delayed statements, which run
 *   on the host seconds later. Live 2026-11-06 (N100 pair, level 2): the arrest scene's OnUnlocked was "sent ... to 0
 *   client(s)" 13 s into the level; its `$player.DisableTC("time_reverse",0) :delay + 2` ran on the host after a voice
 *   line, after the client had started running the level (16 s); the client kept the level start's reverse lock
 *   (player +0x2a8 = 0, game flags 0xfcace0 bit 0x4000) and its reverse key never worked on that level. A client that got
 *   the replay has the same permissions already: the send changes nothing there.
 * Kept in a header so tests/latejoin_test.c checks the very same code on the host.
 */
#ifndef TS_LATEJOIN_H
#define TS_LATEJOIN_H

#define LJ_LATE_MS 20000u
#define LJ_LOADOUT 64
#define LJ_TC (8 | 16 | 32)
#define LJ_TC_SETTLE_MS 1000u
#define LJ_TC_AFTER_MS 3000u

/* Full sends for a client seen running the level `since_start` ms after the level's run started. */
static inline int latejoin_sends(unsigned since_start)
{
    return since_start > LJ_LATE_MS ? 3 : 0;
}

/* The bits of a late joiner's send (`first`: its first send carries the loadout). */
static inline int latejoin_bits(int mask_now, int first)
{
    return first ? mask_now : mask_now & ~LJ_LOADOUT;
}

/* 1 = send the host's time-power permissions to this client now: the host's permissions (`mask_now` & LJ_TC) changed to
 * a state (`seq`) the client has not been sent (`sent_seq`), they have stayed so for LJ_TC_SETTLE_MS (`since_change`),
 * and the client has run the level for LJ_TC_AFTER_MS (`in_level`). */
static inline int tc_mirror_due(int mask_now, unsigned seq, unsigned sent_seq, unsigned since_change, unsigned in_level)
{
    return (mask_now & LJ_TC) && seq != sent_seq && since_change >= LJ_TC_SETTLE_MS && in_level >= LJ_TC_AFTER_MS;
}

#endif
