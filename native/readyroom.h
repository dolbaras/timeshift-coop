/*
 * The co-op ready room (coopsession.c; ready-ts, 2026-11-09). The owner: "Ready" buttons as in Jericho, where the host's
 * Start waits until every player is ready. TimeShift has no lobby screen (the co-op host's server loads the campaign level
 * at once, clients join it running), so the FIRST level of a hosted session starts in the game's own warm-up with its own
 * ready system: the mission starts when every player has pressed READY, nobody can walk off before.
 * The decisions are kept in this header so tests/readyroom_test.c checks the very same code:
 * - rr_warmup: bWarmUp of a server level start in a co-op session: 1 only for the session's first level and only on the
 *   server (later levels: level exits and team restarts start at once, everybody is there already);
 * - rr_ready_opt: bReadySystem (the game's ReadyType and its HUD hint): on while the warm-up runs;
 * - rr_step: this machine's hold of its own player while the room lasts. The engine's DisableMove / DisableInput may be a
 *   flag or a counter (and the level's own scripts may set or clear them meanwhile), so the hold sets once, re-asserts every
 *   RR_REASSERT_MS as a release + set pair (a flag ends set, a counter stays balanced), releases once at the end, and never
 *   touches a player entity it no longer sees (a respawn: the new one is held, the old pointer is dropped).
 */
#ifndef TS_READYROOM_H
#define TS_READYROOM_H

#define RR_REASSERT_MS 1000u

/* bWarmUp: ready_mode = the room is wanted (TS_COOP_READY), levels_before = server level starts already seen in this
 * session (back at the menu: 0), server = this machine is the server. */
static inline int rr_warmup(int ready_mode, int levels_before, int server)
{
    return ready_mode && levels_before == 0 && server;
}

/* bReadySystem in a co-op session: the room's ready system while its warm-up runs (the same on every machine). */
static inline int rr_ready_opt(int warmup_running)
{
    return warmup_running != 0;
}

/* The hold. rr_step tells the caller what to call on the player entity this tick (in this order):
 * RR_SET = DisableMove(1) + DisableInput(1); RR_PAIR = (0) then (1) for both; RR_RELEASE = (0) for both. */
enum { RR_NONE = 0, RR_SET = 1, RR_PAIR = 2, RR_RELEASE = 3 };
enum { RR_EV_NONE = 0, RR_EV_ENTER = 1, RR_EV_LEAVE = 2, RR_EV_HELD = 4 };
typedef struct {
    int on;                 /* the room is running here */
    const void *held;       /* the entity this machine holds (NULL = none) */
    unsigned held_at, since;
} rr_hold;

/* in_room = the room's warm-up runs in a co-op session; player = this machine's player entity now (NULL = none); now = ms.
 * *act = what to do with `player` this tick (RR_*), *ev = what happened (RR_EV_* bits, for the log). */
static inline void rr_step(rr_hold *h, int in_room, const void *player, unsigned now, int *act, int *ev)
{
    *act = RR_NONE, *ev = RR_EV_NONE;
    if (in_room && !h->on) {
        h->on = 1, h->since = now, h->held = 0;
        *ev |= RR_EV_ENTER;
    }
    if (h->on && in_room) {
        if (player && player != h->held) {
            h->held = player, h->held_at = now;
            *act = RR_SET, *ev |= RR_EV_HELD;
        } else if (player && now - h->held_at >= RR_REASSERT_MS) {
            h->held_at = now;
            *act = RR_PAIR;
        }
    }
    if (h->on && !in_room) {
        h->on = 0;
        if (h->held && h->held == player)
            *act = RR_RELEASE;
        h->held = 0;
        *ev |= RR_EV_LEAVE;
    }
}

#endif
