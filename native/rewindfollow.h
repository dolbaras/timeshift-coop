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
