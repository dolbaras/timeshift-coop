/*
 * Offline check of the rewind followers (rewindfollow.h, coopreverse.c message 84), on the host:  sh tests/run.sh
 * Values from the live run n100pair-20261106-231030 (level 2): the host's bridge wreck clip ANIM_GROUP index 1 at
 * 4.65 s after its rewind, running; before the wreck the actor shows clip 0 at 0 s.
 */
#include <stdio.h>

#include "../rewindfollow.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    const char *const *l = rf_level(rf_table, "ts2_city_part02");
    CHECK(l && l[0] && !strcmp(l[0], "ANIM_GROUP") && !l[1], "level 2 followers: want exactly ANIM_GROUP");
    l = rf_level(rf_table, "TS2_CITY_PART02");
    CHECK(l && !strcmp(l[0], "ANIM_GROUP"), "level name case does not matter");
    CHECK(!rf_level(rf_table, "ts2_city_part03"), "level 3 has no followers");
    CHECK(!rf_level(rf_table, "ts2_city_part0"), "a prefix of a level name is no match");
    CHECK(!rf_level(rf_table, ""), "an empty level name is no match");
    int n = 0;
    for (const char *const *t = rf_table; *t; t++, n++) {
        const char *const *f = rf_level(rf_table, *t);
        int k = 0;
        while (f && f[k])
            k++;
        CHECK(f && k > 0 && k <= RF_MAX, "%s: %d followers (1..%d)", *t, k, RF_MAX);
        while (*t)
            t++;
    }
    CHECK(n == 1, "%d levels in the table (want 1)", n);

    unsigned char b[RF_LEN];
    int a, c, fl;
    float t;
    CHECK(rf_pack(b, 0, 1, RF_RUNS, 4.65f) == RF_LEN, "packed length");
    CHECK(rf_unpack(b, RF_LEN, &a, &c, &fl, &t) && a == 0 && c == 1 && fl == RF_RUNS && t == 4.65f,
          "round trip clip 1 at 4.65 s running: %d %d %d %.2f", a, c, fl, (double)t);
    rf_pack(b, 0, 0, RF_HOLD, 0.f);
    CHECK(rf_unpack(b, RF_LEN, &a, &c, &fl, &t) && c == 0 && fl == RF_HOLD && t == 0.f, "clip 0 at 0 s held (before the wreck)");
    CHECK(!rf_unpack(b, RF_LEN - 1, &a, &c, &fl, &t), "a short message is refused");
    rf_pack(b, RF_MAX, 1, 0, 1.f);
    CHECK(!rf_unpack(b, RF_LEN, &a, &c, &fl, &t), "an actor index beyond the list is refused");
    rf_pack(b, 0, -1, 0, 1.f);
    CHECK(!rf_unpack(b, RF_LEN, &a, &c, &fl, &t), "no clip (-1) is refused");
    rf_pack(b, 0, 1, 0, -2.f);
    CHECK(!rf_unpack(b, RF_LEN, &a, &c, &fl, &t), "a negative time is refused");
    rf_pack(b, 0, 1, 0, 1.f);
    b[0] = 2;
    CHECK(!rf_unpack(b, RF_LEN, &a, &c, &fl, &t), "another protocol version is refused");

    /* companions (message 85): the level 2 list, and the live end state of run n100pair-20261106-235732 after the host's
     * rewind: Bridge_plrc_actor hidden again, Bridge_dom_kill_1 locked again (host) */
    const char *const *s = rf_level(rs_table, "ts2_city_part02");
    int ns = 0;
    while (s && s[ns])
        ns++;
    CHECK(ns == 12 && !strcmp(s[0], "Bridge_plrc_actor") && !strcmp(s[5], "physc_bridge_actor") && !strcmp(s[6], "Bridge_dom_kill_1")
          && !strcmp(s[11], "Bridge_dom_kill_6"), "level 2 companions: 6 actors and 6 kill domains (got %d)", ns);
    CHECK(ns <= RS_MAX, "within RS_MAX");
    CHECK(!rf_level(rs_table, "ts2_city_part03"), "level 3 has no companions");
    int hid[RS_MAX], lk[RS_MAX], h2[RS_MAX], l2[RS_MAX];
    for (int i = 0; i < 12; i++) {
        hid[i] = i < 6 ? 1 : -1;      /* actors hidden, domains: no hidden state */
        lk[i] = i < 6 ? -1 : 1;       /* kill domains locked */
    }
    hid[5] = -1;                      /* an object the host did not find: nothing to say */
    unsigned char sb[2 + 3 * RS_MAX];
    int len = rs_pack(sb, sizeof sb, 12, hid, lk);
    CHECK(len == 38, "12 companions: 38 bytes (got %d)", len);
    CHECK(rs_unpack(sb, len, h2, l2) == 12, "unpacked 12");
    int same = 1;
    for (int i = 0; i < 12; i++)
        same &= h2[i] == hid[i] && l2[i] == lk[i];
    CHECK(same, "round trip of hidden/locks");
    CHECK(h2[12] == -1 && l2[12] == -1 && h2[RS_MAX - 1] == -1, "indices beyond n: nothing to say");
    CHECK(rs_unpack(sb, len - 1, h2, l2) < 0, "a short states message is refused");
    sb[1] = RS_MAX + 1;
    CHECK(rs_unpack(sb, sizeof sb, h2, l2) < 0, "too many entries are refused");
    CHECK(rs_pack(sb, sizeof sb, RS_MAX + 1, hid, lk) == 0, "packing too many is refused");
    CHECK(rs_pack(sb, 10, 12, hid, lk) == 0, "packing into a short buffer is refused");
    /* the lock steps: the client's kill_1 unlocked (0), the host's locked again (1): one Lock() */
    CHECK(rs_lock_steps(0, 1) == 1 && rs_lock_steps(1, 0) == -1 && rs_lock_steps(1, 1) == 0, "lock steps +1 / -1 / 0");
    CHECK(rs_lock_steps(0, 200) == 8 && rs_lock_steps(200, 0) == -8, "lock steps capped at 8");

    printf("rewindfollow_test: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
