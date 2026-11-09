/*
 * Offline check of the co-op ready room (readyroom.h, used by coopsession.c):  sh tests/run.sh
 * Jericho's lobby is the model: nobody starts before every player is ready, a player can take the ready back, the host's
 * start waits for the others. Here the first level of a hosted session is the room (the game's warm-up + ready system,
 * which starts the mission when every player is ready); the checks below are what coopsession.c decides around it:
 * the warm-up only on the session's first level and only on the server, the ready system while the warm-up runs, and the
 * hold of the own player, simulated against an engine whose DisableMove/DisableInput is a FLAG and one where it is a
 * COUNTER, with the level's own scripts setting and clearing them in the middle. Before ready-ts the co-op rules read
 * bWarmUp = 0 and bReadySystem = 0 always (no room) and nothing held the players: the "old" model below fails.
 */
#include <stdio.h>

#include "../readyroom.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* a fake player entity: the engine's lock as a flag or as a counter (DisableMove and DisableInput behave alike here) */
typedef struct { int counter_kind, value; } ent;
static void lock(ent *e, int on)
{
    if (e->counter_kind)
        e->value += on ? 1 : (e->value > 0 ? -1 : 0);
    else
        e->value = on ? 1 : 0;
}
static int locked(const ent *e) { return e->value > 0; }
static void apply(ent *e, int act)
{
    if (!e) return;
    if (act == RR_SET) lock(e, 1);
    else if (act == RR_PAIR) lock(e, 0), lock(e, 1);
    else if (act == RR_RELEASE) lock(e, 0);
}

/* the old co-op rules (0.1.3): no room at all */
static int old_warmup(int ready_mode, int levels_before, int server) { (void)ready_mode, (void)levels_before, (void)server; return 0; }

static void room_run(int counter_kind)
{
    const char *k = counter_kind ? "counter" : "flag";
    ent p = {counter_kind, 0}, p2 = {counter_kind, 0};
    rr_hold h = {0};
    int act, ev, holds = 0, enters = 0, leaves = 0;
    unsigned t = 1000;
    /* the level's intro locks the player before the room's first tick (the start camera) */
    lock(&p, 1);
    for (int tick = 0; tick < 400; tick++, t += 50) {   /* 20 s of room at 20 ticks a second */
        rr_step(&h, 1, &p, t, &act, &ev);
        apply(&p, act);
        holds += (ev & RR_EV_HELD) != 0, enters += (ev & RR_EV_ENTER) != 0;
        if (tick == 100) lock(&p, 0);   /* the intro ends at 5 s and releases its lock */
        if (tick > 120) CHECK(locked(&p), "%s: the player is held through the room (tick %d: value %d)", k, tick, p.value);
    }
    CHECK(enters == 1 && holds == 1, "%s: one enter, one hold (enters %d holds %d)", k, enters, holds);
    /* a flag cleared by a level script in the middle of the room: held again within RR_REASSERT_MS */
    if (!counter_kind) {
        lock(&p, 0);
        int back = -1;
        for (int i = 0; i < 40 && back < 0; i++, t += 50) {
            rr_step(&h, 1, &p, t, &act, &ev);
            apply(&p, act);
            if (locked(&p)) back = i * 50;
        }
        CHECK(back >= 0 && back <= (int)RR_REASSERT_MS, "flag: a script's release is undone within %u ms (%d)", RR_REASSERT_MS, back);
    }
    /* the player dies in the room and respawns: the new entity is held, the old one is never touched again */
    int old_value = p.value;
    rr_step(&h, 1, &p2, t, &act, &ev);
    apply(&p2, act);
    t += 50;
    CHECK(act == RR_SET && (ev & RR_EV_HELD) && locked(&p2), "%s: a respawned player is held", k);
    for (int i = 0; i < 60; i++, t += 50) {
        rr_step(&h, 1, &p2, t, &act, &ev);
        apply(&p2, act);
    }
    CHECK(p.value == old_value, "%s: the old entity is never touched after the respawn", k);
    /* every player is ready: the warm-up ends, the hold goes once; afterwards nothing more is done */
    rr_step(&h, 0, &p2, t, &act, &ev);
    apply(&p2, act);
    leaves += (ev & RR_EV_LEAVE) != 0;
    CHECK(act == RR_RELEASE && leaves == 1 && !locked(&p2) && p2.value == 0, "%s: released once at the start (value %d)", k, p2.value);
    for (int i = 0; i < 100; i++, t += 50) {
        rr_step(&h, 0, &p2, t, &act, &ev);
        CHECK(act == RR_NONE && ev == RR_EV_NONE, "%s: nothing after the room", k);
        if (act != RR_NONE) break;
    }
    /* a level script's own lock after the start is the script's business */
    lock(&p2, 1);
    rr_step(&h, 0, &p2, t, &act, &ev);
    CHECK(act == RR_NONE && locked(&p2), "%s: a later script lock is left alone", k);
}

int main(void)
{
    /* bWarmUp: the session's first level on the server only */
    CHECK(rr_warmup(1, 0, 1) == 1, "the first level of a hosted session is the ready room");
    CHECK(rr_warmup(1, 1, 1) == 0 && rr_warmup(1, 5, 1) == 0, "later levels of the session (exits, team restarts) start at once");
    CHECK(rr_warmup(1, 0, 0) == 0, "a client never starts a warm-up (the server's rules decide)");
    CHECK(rr_warmup(0, 0, 1) == 0, "TS_COOP_READY=0 / a lab copy: no room");
    CHECK(old_warmup(1, 0, 1) != rr_warmup(1, 0, 1), "the 0.1.3 rules (no room) fail the first check");
    /* bReadySystem: on while the room's warm-up runs, on every machine */
    CHECK(rr_ready_opt(1) == 1 && rr_ready_opt(0) == 0, "the ready system follows the warm-up");
    /* the hold against both engine kinds */
    room_run(0);
    room_run(1);
    /* no player entity yet (a client still loading): nothing is called, the room still counts */
    rr_hold h = {0};
    int act, ev;
    rr_step(&h, 1, 0, 100, &act, &ev);
    CHECK(act == RR_NONE && (ev & RR_EV_ENTER) && !(ev & RR_EV_HELD), "no player yet: no call");
    rr_step(&h, 0, 0, 200, &act, &ev);
    CHECK(act == RR_NONE && (ev & RR_EV_LEAVE), "the room ends without a player: nothing to release");
    printf("readyroom: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;   /* (the 0.1.3 rules' check above passes only when the room differs from them) */
}
