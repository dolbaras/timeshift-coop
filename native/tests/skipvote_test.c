/*
 * Offline check of the co-op skip vote's tally (skipvote.h, used by skipvote.c):  sh tests/run.sh
 * Before parity-016 round 4 a press of Start / Esc skipped the scene on that machine alone; the host's skip ended its scene and
 * its end events reached a client that was still watching. The "old" rule below (any vote skips) fails these checks.
 */
#include <stdio.h>

#include "../skipvote.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int old_rule(const sv_machine *m, int n)
{
    for (int i = 0; i < n; i++)
        if (m[i].votes)
            return 1;
    return 0;
}

int main(void)
{
    int v, w;
    /* gate: who votes at all */
    CHECK(!sv_gate(0, 0, 0, 1), "single player: stock skip");
    CHECK(sv_gate(0, 0, 0, 2), "split screen in single player: vote");
    CHECK(!sv_gate(1, 0, 0, 1), "host alone in its session: stock skip");
    CHECK(sv_gate(1, 0, 1, 1), "host with a client: vote");
    CHECK(sv_gate(1, 1, 0, 1), "a client: vote (the host is there)");
    CHECK(sv_gate(1, 0, 0, 2), "split host without clients: vote");

    /* two PCs, both watching */
    sv_machine m[3] = {{0, 1, 1, 0, 0}, {5, 1, 1, 0, 0}, {0}};
    CHECK(!sv_decide(m, 2, 0, 10000, &v, &w) && v == 0 && w == 2, "nobody voted: no skip (%d/%d)", v, w);
    m[1].votes = 1;
    CHECK(!sv_decide(m, 2, 0, 10000, &v, &w) && v == 1 && w == 2, "the client voted: 1/2, no skip (%d/%d)", v, w);
    CHECK(old_rule(m, 2), "(the old rule skipped here: the check above is what changed)");
    m[0].votes = 1;
    CHECK(sv_decide(m, 2, 0, 10000, &v, &w) && v == 2 && w == 2, "both voted: skip (%d/%d)", v, w);

    /* the host voted alone, the client does not watch (its scene is over or never started): after the grace time the host skips */
    sv_machine a[2] = {{0, 1, 1, 1, 0}, {5, 0, 1, 0, 0}};
    CHECK(!sv_decide(a, 2, 0, 1000, &v, &w) && w == 2, "client's report not in yet (1 s into the host's scene): wait (%d/%d)", v, w);
    CHECK(sv_decide(a, 2, 0, 5000, &v, &w) && v == 1 && w == 1, "client not watching (5 s in): the host's vote is enough (%d/%d)", v, w);

    /* a scene on a client only (the host shows none): the client's vote is enough */
    sv_machine c[2] = {{0, 0, 1, 0, 0}, {5, 1, 1, 1, 0}};
    CHECK(sv_decide(c, 2, 0, 0, &v, &w) && v == 1 && w == 1, "client-only scene: its own vote skips (%d/%d)", v, w);

    /* split screen on the host: both of its players must vote */
    sv_machine s[2] = {{0, 1, 2, 1, 0}, {5, 1, 1, 1, 0}};
    CHECK(!sv_decide(s, 2, 0, 9000, &v, &w) && v == 2 && w == 3, "split host's player 2 has not voted: no skip (%d/%d)", v, w);
    s[0].votes = 3;
    CHECK(sv_decide(s, 2, 0, 9000, &v, &w) && v == 3 && w == 3, "all three voted: skip (%d/%d)", v, w);
    sv_machine one[1] = {{0, 1, 2, 2, 0}};
    CHECK(!sv_decide(one, 1, 0, 9000, &v, &w) && v == 1 && w == 2, "split in single player, player 2 voted: 1/2 (%d/%d)", v, w);
    one[0].votes = 3;
    CHECK(sv_decide(one, 1, 0, 9000, &v, &w), "split in single player, both voted: skip");

    /* an old round's votes do not count (the client has not heard of the last skip order yet) */
    sv_machine r[2] = {{0, 1, 1, 1, 4}, {5, 1, 1, 1, 3}};
    CHECK(!sv_decide(r, 2, 4, 9000, &v, &w) && v == 1, "a vote of round 3 in round 4: not counted (%d/%d)", v, w);

    /* three clients, one of them watching nothing (left the scene), two voted with the host */
    sv_machine q[4] = {{0, 1, 1, 1, 0}, {2, 1, 1, 1, 0}, {3, 0, 1, 0, 0}, {4, 1, 1, 0, 0}};
    CHECK(!sv_decide(q, 4, 0, 9000, &v, &w) && v == 2 && w == 3, "4 machines, one not watching, one not voted: 2/3 (%d/%d)", v, w);
    q[3].votes = 1;
    CHECK(sv_decide(q, 4, 0, 9000, &v, &w) && v == 3 && w == 3, "... the last watcher voted: skip (%d/%d)", v, w);

    printf("skipvote: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
