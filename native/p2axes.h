/*
 * The player's walk smoothing is two engine globals, not part of the player object (patch 1.2, Player update 0x40e860):
 *   0xec6e44 = forward/back value (command ids MOVE_FORWARD 0 / MOVE_BACKWARD 1), rate 6.0 per second at 0xec6e48
 *   0xec6e4c = left/right value (MOVE_LEFT 8 / MOVE_RIGHT 9), rate at 0xec6e50
 * Every update moves them towards the keys' target by rate * dt (0x42f1a0, called at 0x40eb15 / 0x40ec83), and the
 * velocity is smoothstep(value) * MovementSpeed (0x42f250, called at 0x40f91b / 0x40f921). Two local players share
 * them: player 1 raises the value by one step, player 2 (idle) takes it back, so player 1 walks at one step of speed
 * (about 15-25 % of the full speed) until player 2 presses the same direction as well. The split screen therefore
 * gives player 2 its own pair of values: they are exchanged with the live globals while player 2's code runs.
 *
 * Kept in a header so tests/axes_test.c checks the very same code on the host.
 */
#ifndef TS_P2AXES_H
#define TS_P2AXES_H

/* Swaps the live pair (l0, l1) with the stored pair; called on entering and again on leaving player 2's swap. */
static inline void p2_axes_exchange(volatile float *l0, volatile float *l1, float *store)
{
    float a = *l0, b = *l1;
    *l0 = store[0];
    *l1 = store[1];
    store[0] = a;
    store[1] = b;
}

#endif
