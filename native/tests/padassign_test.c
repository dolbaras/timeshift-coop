/*
 * Offline check of the pad assignment (padassign.h) on the host:  sh tests/run.sh
 * The two-pad behaviour of 0.1.7 (player 1 the first pad, player 2 the next, a fixed pad of player 1 first) must not change; one pad goes
 * to player 2 once the screen is split and player 1 plays on keyboard and mouse; explicit choices are honoured.
 */
#include <stdio.h>

#include "../padassign.h"

static int fails, checks;

static void chk(const char *what, const int con[4], int fixed, int d1, int d2, int split, int e1, int e2)
{
    int dev[2] = {d1, d2}, o1 = 99, o2 = 99;
    pad_assign(con, fixed, dev, split, &o1, &o2);
    checks++;
    if (o1 != e1 || (split && o2 != e2)) {
        fails++;
        printf("FAIL %s: got %d/%d, expected %d/%d\n", what, o1, o2, e1, e2);
    }
}

int main(void)
{
    const int none[4] = {0, 0, 0, 0}, one[4] = {1, 0, 0, 0}, one_b[4] = {0, 0, 1, 0}, two[4] = {1, 1, 0, 0}, two_gap[4] = {0, 1, 0, 1},
              four[4] = {1, 1, 1, 1};
    /* 0.1.7, two pads, nothing chosen: unchanged in both states */
    chk("two pads, single", two, -1, -2, -2, 0, 0, -1);
    chk("two pads, split", two, -1, -2, -2, 1, 0, 1);
    chk("two pads in slots 2 and 4, split", two_gap, -1, -2, -2, 1, 1, 3);
    chk("four pads, split", four, -1, -2, -2, 1, 0, 1);
    /* 0.1.7 with a fixed pad of player 1 (Pad=2 / TS_PAD=2) */
    chk("fixed pad 2, two pads, split", two, 1, -2, -2, 1, 1, 0);
    chk("fixed pad 1, two pads, split", two, 0, -2, -2, 1, 0, 1);
    chk("fixed pad 1, one pad, single", one, 0, -2, -2, 0, 0, -1);
    /* one pad */
    chk("one pad, single: player 1 uses it", one, -1, -2, -2, 0, 0, -1);
    chk("one pad, split: player 2 gets it", one, -1, -2, -2, 1, -1, 0);
    chk("one pad in slot 3, split", one_b, -1, -2, -2, 1, -1, 2);
    chk("one pad, split, player 1 keyboard", one, -1, -1, -2, 1, -1, 0);
    chk("one pad, split, player 2 pad 1 chosen", one, -1, -2, 0, 1, -1, 0);
    chk("one pad, split, both choose it: player 2 wins", one, -1, 0, 0, 1, -1, 0);
    chk("one pad, single, keyboard chosen", one, -1, -1, -2, 0, -1, -1);
    /* no pad */
    chk("no pad, single", none, -1, -2, -2, 0, -1, -1);
    chk("no pad, split", none, -1, -2, -2, 1, -1, -1);
    /* explicit choices with two pads */
    chk("two pads, split, keyboard for player 1: player 2 the first pad", two, -1, -1, -2, 1, -1, 0);
    chk("two pads, split, swapped", two, -1, 1, 0, 1, 1, 0);
    chk("two pads, split, player 2 pad 2: player 1 the other", two, -1, -2, 1, 1, 0, 1);
    chk("two pads, split, player 1 pad 2: player 2 the other", two, -1, 1, -2, 1, 1, 0);
    chk("two pads, single, player 1 pad 2", two, -1, 1, -2, 0, 1, -1);
    /* a choice of a pad that is not there is automatic */
    chk("one pad, split, player 2 chose pad 3 (gone)", one, -1, -2, 2, 1, -1, 0);
    chk("one pad, single, player 1 chose pad 4 (gone)", one, -1, 3, -2, 0, 0, -1);
    printf("padassign: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
