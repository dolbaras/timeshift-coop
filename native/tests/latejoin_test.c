/*
 * Offline check of the host's player state for clients (latejoin.h, campaign.c message 72), on the host:  sh tests/run.sh
 * Timeline of the live runs n100pair-20261106-231030 / -234222 (level 2): the client starts running the level 16 s into
 * it; the host's story re-allows time reversal (DisableTC("time_reverse", 0): bit 32) about 7 s after that, from a
 * replay the client never got. Before the fix only clients more than 20 s late got anything, so its reverse stayed locked.
 */
#include <stdio.h>

#include "../latejoin.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    /* late joiners as before */
    CHECK(latejoin_sends(25000) == 3, "25 s into the level: 3 full sends");
    CHECK(latejoin_sends(16000) == 0 && latejoin_sends(20000) == 0, "16 s / 20 s: no full sends");
    CHECK(latejoin_bits(32 | 64, 1) == (32 | 64) && latejoin_bits(32 | 64, 0) == 32, "the loadout only in the first full send");

    /* the live case: the client runs the level (in_level counts from then); the host's reverse permission changes at 7 s */
    unsigned seq = 0, sent = 0;
    CHECK(!tc_mirror_due(0, seq, sent, 100000, 5000), "nothing changed yet: nothing to send");
    seq = 1;   /* DisableTC("time_reverse", 0) at in_level 7.0 s */
    CHECK(!tc_mirror_due(32, seq, sent, 400, 7400), "0.4 s after the change: wait until it settles");
    CHECK(tc_mirror_due(32, seq, sent, 1000, 8000), "1 s after the change: send");
    sent = seq;
    CHECK(!tc_mirror_due(32, seq, sent, 5000, 12000), "sent once: not again while it stays");

    /* a change before the client ran the level: sent 3 s after it starts running, not earlier */
    seq = 1, sent = 0;
    CHECK(!tc_mirror_due(32, seq, sent, 4000, 2000), "client runs the level for 2 s: not yet");
    CHECK(tc_mirror_due(32, seq, sent, 5000, 3000), "client runs the level for 3 s: send");

    /* a lock and its release within a second (03840: DisableTC 1, then 0 one second later): only the settled state goes */
    seq = 2, sent = 1;
    CHECK(!tc_mirror_due(32, seq, sent, 300, 60000), "a quick lock/release: wait");
    seq = 3;
    CHECK(tc_mirror_due(32, seq, sent, 1000, 61300), "settled: send the final state");

    /* other bits alone (loadout, move/input/aim) are not mirrored */
    CHECK(!tc_mirror_due(64 | 1 | 2 | 4, 5, 0, 9000, 9000), "no time-power bits: nothing");
    CHECK((LJ_TC & (1 | 2 | 4 | 64)) == 0 && LJ_TC == 56, "LJ_TC is the three DisableTC bits");

    printf("latejoin_test: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
