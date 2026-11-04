/*
 * Offline check of the split screen's walk smoothing swap (p2axes.h), on the host:
 *   sh tests/run.sh
 * Models the engine's per-update walk code of patch 1.2 (Player update 0x40e860):
 *   smoothing 0x42f1a0: the value moves towards the target by rate * dt and stops there (rate 6.0 per second),
 *   speed 0x42f250: smoothstep on (v + 1) / 2, mapped back to -1..1, times MovementSpeed (here 1.0).
 * and runs two players in one frame the way the split screen does: player 1's update, then player 2's update
 * inside the swap. Without the swap the pair of values is shared. The test fails when:
 *   - player 1's speed while it pushes forward depends on what player 2 does (idle, forward, back),
 *   - player 1's speed differs from a single-player game,
 *   - the unswapped model does NOT show the reported effect (a one-step speed when player 2 is idle, full speed when
 *     both push forward), which would mean the model is wrong.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../p2axes.h"

static volatile float g_fb, g_lr;   /* stand-ins for 0xec6e44 and 0xec6e4c */
static const float RATE = 6.0f;

static float approach(float cur, float target, float dt)   /* 0x42f1a0 */
{
    if (cur < -1) cur = -1;
    if (cur > 1) cur = 1;
    if (cur < target) {
        cur += RATE * dt;
        if (cur > target) cur = target;
    } else if (cur > target) {
        cur -= RATE * dt;
        if (cur < target) cur = target;
    }
    return cur;
}

static float speed_of(float v)   /* 0x42f250 */
{
    float t = (v + 1) * 0.5f;
    if (t < 0) return -1;
    if (t > 1) t = 1;
    float f = (3.0f - t * 2.0f) * t * t;
    return f + f - 1;
}

/* one update of one player: smoothing towards the target, then the speed it walks with */
static float update(float target_fb, float dt)
{
    g_fb = approach(g_fb, target_fb, dt);
    return speed_of(g_fb);
}

/* the frames of a run: returns player 1's mean speed over the last second */
static float run(int swap, float t1, float t2, float fps, int with_p2)
{
    float p2_store[2] = {0, 0};
    float dt = 1.0f / fps, sum = 0;
    int n = (int)(fps * 3), counted = 0;
    g_fb = g_lr = 0;
    for (int i = 0; i < n; i++) {
        float s1 = update(t1, dt);
        if (with_p2) {
            if (swap) p2_axes_exchange(&g_fb, &g_lr, p2_store);
            update(t2, dt);
            if (swap) p2_axes_exchange(&g_fb, &g_lr, p2_store);
        }
        if (i >= n - (int)fps) { sum += s1; counted++; }
    }
    return sum / counted;
}

static int fails;
static void check(int ok, const char *what, float a, float b)
{
    printf("%s  %s (%.3f, %.3f)\n", ok ? "ok  " : "FAIL", what, a, b);
    if (!ok) fails++;
}

int main(void)
{
    const float rates[] = {35, 60, 70};
    for (int i = 0; i < 3; i++) {
        float fps = rates[i];
        printf("-- %g updates per second\n", fps);
        float solo = run(1, 1, 0, fps, 0);
        float idle_sw = run(1, 1, 0, fps, 1), fwd_sw = run(1, 1, 1, fps, 1), back_sw = run(1, 1, -1, fps, 1);
        float idle_sh = run(0, 1, 0, fps, 1), fwd_sh = run(0, 1, 1, fps, 1);
        check(fabsf(solo - 1) < 0.001f, "single player reaches full speed", solo, 1);
        check(fabsf(idle_sw - solo) < 0.001f, "swap: player 1 full speed while player 2 stands", idle_sw, solo);
        check(fabsf(fwd_sw - solo) < 0.001f, "swap: player 1 full speed while player 2 walks the same way", fwd_sw, solo);
        check(fabsf(back_sw - solo) < 0.001f, "swap: player 1 full speed while player 2 walks back", back_sw, solo);
        check(idle_sh < 0.35f, "model without swap: one step of speed while player 2 stands (the report)", idle_sh, 0.35f);
        check(fabsf(fwd_sh - 1) < 0.001f, "model without swap: full speed when both push forward (the report)", fwd_sh, 1);
    }
    {   /* player 2's own values survive between frames (it is not reset by the swap) */
        float store[2] = {0.25f, -0.5f};
        g_fb = 0.75f; g_lr = 0.1f;
        p2_axes_exchange(&g_fb, &g_lr, store);
        check(g_fb == 0.25f && g_lr == -0.5f && store[0] == 0.75f && store[1] == 0.1f, "exchange puts the stored pair live", g_fb, g_lr);
        p2_axes_exchange(&g_fb, &g_lr, store);
        check(g_fb == 0.75f && g_lr == 0.1f && store[0] == 0.25f && store[1] == -0.5f, "second exchange restores both", g_fb, g_lr);
    }
    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails != 0;
}
