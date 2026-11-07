/*
 * Rewind followers (coopreverse.c, message 84): animated actors that the host's stock recorder rewinds by itself, but
 * whose clip and clip time do not reach the clients (they are not in an entity's network update; a client's copy runs
 * the clips the replayed story starts). During a host's reverse playback the host sends each follower's raw clip index,
 * time and flags at 10 Hz (held), and three times after the playback (with the real running state); a client sets them
 * through the engine's own clip start (as campaign.c's digest does) and Stop().
 * Unlike coopreverse.c's rewind actors (cranes, lifts: the stock recorder left them at their end pose, the host steps
 * them back from its own record), a follower is never set on the host: its recorder already does it right.
 * Live 2026-11-06 (N100 pair, ts2_city_part02, run n100pair-20261106-231030): the bridge wreck ANIM_GROUP ("ACTION",
 * 11 s); the host's reverse took the host's clip from 9.37 s back to 4.65 s, the client's copy stayed at its end
 * (11.0 s): the host saw the bridge standing again, the client the wreck.
 * Kept in a header so tests/rewindfollow_test.c checks the very same code on the host.
 */
#ifndef TS_REWINDFOLLOW_H
#define TS_REWINDFOLLOW_H

#include <string.h>

#define RF_PROTO 1
#define RF_LEN 9                             /* {u8 proto, u8 actor, i16 clip, u8 flags, f32 time} */
#define RF_RUNS 1
#define RF_CYCLE 2
#define RF_HOLD 4
#define RF_MAX 4                             /* followers per level */

/* level base name (no folder, no .lg) -> its followers; NULL-separated groups, the table ends with an empty group */
static const char *const rf_table[] = {
    "ts2_city_part02", "ANIM_GROUP", NULL,
    NULL};

/* The followers of a level (a NULL-terminated list), or NULL. */
static inline const char *const *rf_level(const char *const *t, const char *level)
{
    while (*t) {
        int same = 1;
        const char *a = *t, *b = level;
        for (; *a || *b; a++, b++) {
            char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
            if (ca != cb) {
                same = 0;
                break;
            }
        }
        if (same)
            return t + 1;
        while (*t)
            t++;
        t++;
    }
    return NULL;
}

static inline int rf_pack(unsigned char *b, int actor, int clip, int flags, float time)
{
    short c = (short)clip;
    b[0] = RF_PROTO;
    b[1] = (unsigned char)actor;
    memcpy(b + 2, &c, 2);
    b[4] = (unsigned char)flags;
    memcpy(b + 5, &time, 4);
    return RF_LEN;
}

/*
 * Their companions (message 85): local objects (not isSyncMP) whose shown/hidden state or lock count the wreck's story
 * events set on every machine, and the host's recorder puts back by itself. Live 2026-11-06 (run n100pair-20261106-235732):
 * the host's rewind 2 s into the wreck hid Bridge_plrc_actor again (+0x228 0x800 -> 0x1800) and locked Bridge_dom_kill_1
 * again (0 -> 1); the client kept the collision shown and the kill domain unlocked, while its clip ran to the end and the
 * kill domains 2-6 stayed locked (the host never ran their frames): a collapsed bridge without its kill zones there, an
 * intact one on the host. The host sends each object's hidden bit and lock count (255: not a domain) with the clips; a
 * client puts only what differs right: Show()/Hide(), Lock()/Unlock() (rs_lock_steps).
 */
#define RS_MAX 16
/* (a name with '#' in front: its lock count is mirrored, else its shown/hidden state; the kind is named here because the
 * kill domains (DOM_KILL) are not told by the domain vtable test: live 2026-11-07, run n100pair-20261107-002022) */
static const char *const rs_table[] = {
    "ts2_city_part02", "Bridge_plrc_actor", "Bridge_plrc_actor1", "Bridge_plrc_actor2", "Bridge_plrc_actor3",
    "Bridge_plrc_actor4", "physc_bridge_actor", "#Bridge_dom_kill_1", "#Bridge_dom_kill_2", "#Bridge_dom_kill_3",
    "#Bridge_dom_kill_4", "#Bridge_dom_kill_5", "#Bridge_dom_kill_6", NULL,
    NULL};

static inline int rs_is_lock(const char *entry)
{
    return entry[0] == '#';
}

static inline const char *rs_name(const char *entry)
{
    return entry[0] == '#' ? entry + 1 : entry;
}

/* {u8 proto, u8 n, n x {u8 index, u8 hidden (2: not known / not shown-hidden), u8 locks (255: not a domain)}}; hidden[i]
 * < 0 and locks[i] < 0 mean "nothing to say"; returns the length (0: n too big) */
static inline int rs_pack(unsigned char *b, int size, int n, const int *hidden, const int *locks)
{
    if (n < 0 || n > RS_MAX || size < 2 + 3 * n)
        return 0;
    b[0] = RF_PROTO;
    b[1] = (unsigned char)n;
    for (int i = 0; i < n; i++) {
        b[2 + 3 * i] = (unsigned char)i;
        b[3 + 3 * i] = (unsigned char)(hidden[i] < 0 ? 2 : hidden[i] ? 1 : 0);
        b[4 + 3 * i] = (unsigned char)(locks[i] < 0 ? 255 : locks[i] > 254 ? 254 : locks[i]);
    }
    return 2 + 3 * n;
}

/* fills hidden[] / locks[] (-1: nothing to say) by index; returns n, or -1 for a bad message */
static inline int rs_unpack(const unsigned char *b, int len, int *hidden, int *locks)
{
    if (len < 2 || b[0] != RF_PROTO || b[1] > RS_MAX || len < 2 + 3 * b[1])
        return -1;
    int n = b[1];
    for (int i = 0; i < RS_MAX; i++)
        hidden[i] = -1, locks[i] = -1;
    for (int i = 0; i < n; i++) {
        int k = b[2 + 3 * i];
        if (k >= RS_MAX)
            return -1;
        hidden[k] = b[3 + 3 * i] == 2 ? -1 : b[3 + 3 * i] ? 1 : 0;
        locks[k] = b[4 + 3 * i] == 255 ? -1 : b[4 + 3 * i];
    }
    return n;
}

/* Lock() (> 0) or Unlock() (< 0) calls that take a domain's lock count from `mine` to `host` (at most 8 either way). */
static inline int rs_lock_steps(int mine, int host)
{
    int d = host - mine;
    return d > 8 ? 8 : d < -8 ? -8 : d;
}

/* 1 = a valid message (actor below RF_MAX, a clip index 0 or above, a time 0..1000 s) */
static inline int rf_unpack(const unsigned char *b, int len, int *actor, int *clip, int *flags, float *time)
{
    short c;
    if (len < RF_LEN || b[0] != RF_PROTO || b[1] >= RF_MAX)
        return 0;
    memcpy(&c, b + 2, 2);
    memcpy(time, b + 5, 4);
    if (c < 0 || !(*time >= 0.f && *time <= 1000.f))
        return 0;
    *actor = b[1];
    *clip = c;
    *flags = b[4] & (RF_RUNS | RF_CYCLE | RF_HOLD);
    return 1;
}

#endif
