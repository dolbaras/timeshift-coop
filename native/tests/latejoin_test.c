/*
 * Offline check of the late joiner rule (latejoin.h, campaign.c message 72), on the host:  sh tests/run.sh
 * Cases from the live run n100pair-20261106-231030 (level 2): the host's story re-allowed time reversal
 * (DisableTC("time_reverse", 0): bit 32) 13 s into the level while the client still loaded; the client ran the level
 * at 16.5 s. Before the fix such a client got nothing (the rule was "more than 20 s"), so its reverse stayed locked.
 */
#include <stdio.h>

#include "../latejoin.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    int early, n;

    /* the live case: 16.5 s in, the host's reverse re-allowed (32) and its weapons put back (64) */
    n = latejoin_plan(16500, 32 | 64, &early);
    CHECK(n == 3 && early, "16.5 s, mask 0x60: %d sends, early %d (want 3, 1)", n, early);
    CHECK(latejoin_bits(32 | 64, early, 1) == 32, "early first send: bits %x (want 0x20, no loadout)", latejoin_bits(32 | 64, early, 1));
    CHECK(latejoin_bits(32 | 64, early, 0) == 32, "early later send: bits %x (want 0x20)", latejoin_bits(32 | 64, early, 0));

    /* a client there from the level start: nothing changed yet, or only the loadout: nothing to send */
    n = latejoin_plan(4000, 0, &early);
    CHECK(n == 0, "4 s, mask 0: %d sends (want 0)", n);
    n = latejoin_plan(4000, 64, &early);
    CHECK(n == 0, "4 s, loadout only: %d sends (want 0)", n);

    /* locks set during the load (a cinematic's DisableMove / DisableInput) count as well */
    n = latejoin_plan(12000, 1 | 2, &early);
    CHECK(n == 3 && latejoin_bits(3, early, 1) == 3, "12 s, move/input locks: %d sends, bits %x (want 3, 3)", n, latejoin_bits(3, early, 1));

    /* late joiners as before: 3 sends whatever the mask is now (it is checked at each send), the loadout in the first only */
    n = latejoin_plan(25000, 0, &early);
    CHECK(n == 3 && !early, "25 s, mask 0: %d sends, early %d (want 3, 0)", n, early);
    CHECK(latejoin_bits(32 | 64, early, 1) == (32 | 64), "late first send: bits %x (want 0x60)", latejoin_bits(32 | 64, early, 1));
    CHECK(latejoin_bits(32 | 64, early, 0) == 32, "late later send: bits %x (want 0x20)", latejoin_bits(32 | 64, early, 0));

    /* the 20 s boundary itself is still "early" (the old rule sent only above 20 s) */
    n = latejoin_plan(20000, 8, &early);
    CHECK(early && n == 3, "20.0 s, mask 8: early %d, %d sends (want 1, 3)", early, n);

    printf("latejoin_test: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
