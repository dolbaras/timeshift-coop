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

    printf("rewindfollow_test: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
