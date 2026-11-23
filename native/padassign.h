/*
 * Which XInput pad each local player uses (gamepad.c; kept in a header so tests/padassign_test.c checks the very same code).
 *
 * The choice per player (dev[0] = player 1, dev[1] = player 2; set by the pause menu's / Co-op menu's input rows through pausemenu.c):
 *   -2 automatic, -1 keyboard and mouse only (player 1), 0-3 that XInput pad.
 * Automatic: two or more pads = player 1 the first, player 2 the next (as it always was); ONE pad = player 1 while the screen is not split,
 * player 2 once it is split (player 1 then plays on keyboard and mouse: split screen with one gamepad). A pad serves one player; where the
 * choices name the same pad twice, player 2 gets it. A choice of a pad that is not connected counts as automatic. A fixed pad of player 1
 * (ts_coop.ini Pad=, TS_PAD) is player 1's choice while it is automatic.
 * With split = 0 only player 1 is assigned (o2 = -1).
 */
#ifndef TS_PADASSIGN_H
#define TS_PADASSIGN_H

static inline void pad_assign(const int connected[4], int fixed_pad, const int dev[2], int split, int *o1, int *o2)
{
    int n = 0, pads[4];
    for (int i = 0; i < 4; i++)
        if (connected[i])
            pads[n++] = i;
    int d1 = dev[0], d2 = dev[1];
    if (d1 == -2 && fixed_pad >= 0)
        d1 = fixed_pad;
    if (d1 >= 0 && !connected[d1])
        d1 = -2;
    if (d2 >= 0 && !connected[d2])
        d2 = -2;
    int p1 = -1, p2 = -1;
    if (!split) {
        *o1 = d1 == -1 ? -1 : d1 >= 0 ? d1 : n > 0 ? pads[0] : -1;
        *o2 = -1;
        return;
    }
    if (d2 >= 0)
        p2 = d2;
    if (d1 >= 0)
        p1 = d1;
    if (d1 == -2 && d2 == -2) {
        if (n >= 2)
            p1 = pads[0], p2 = pads[1];
        else if (n == 1)
            p2 = pads[0];
    } else if (d1 == -2) {
        for (int i = 0; i < n && p1 < 0; i++)
            if (pads[i] != p2)
                p1 = pads[i];
    } else if (d2 == -2) {
        for (int i = 0; i < n && p2 < 0; i++)
            if (pads[i] != p1)
                p2 = pads[i];
    }
    if (p1 >= 0 && p1 == p2)
        p1 = -1;
    *o1 = p1;
    *o2 = p2;
}

#endif
