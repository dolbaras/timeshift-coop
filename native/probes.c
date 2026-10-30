/*
 * Dev probes (tests only, OFF by default): numbers instead of eyes for what moves on the other machine.
 *
 *   TS_PROBES=<seconds>  on from the start (the report period, default 10); or `!pr on [seconds]` in the test console
 *                        (TS_CONSOLE); `!pr off`. Nothing runs and nothing is hooked while it is off.
 *
 * Every frame (the script tick, main thread) the bodies of three kinds are sampled:
 *   player   remote players (slot entities that are not this machine's player or its split-screen player 2)
 *   ai       AI soldiers that are PROXIES here (role bit 1: the client's copies, moved by the net motion component
 *            that aisync.c feeds)
 *   aiown    AI soldiers this machine moves itself (the host's, single player): the reference
 *   self     this machine's own player: the other reference
 *
 * 1. Drawn-step smoothness ("pm:" lines, like Jericho's): per kind every report period
 *      pm:<kind> bodies N frames F triplets T stall S spike P jumps J irr 0.xxx ydev x.x mm vmax x.x m/s | dt avg x.x max x.x ms
 *    A body's horizontal speed per frame is step / real frame time. For three frames in a row with a mean speed over
 *    0.5 m/s (no jump among them): irr = |v2 - (v1+v3)/2| / mean(v1,v2,v3) (0 = even; a stall in the middle with
 *    steady neighbours gives 1.5), stall = the middle step is under 1 mm while both neighbours move (> 0.3 m/s),
 *    spike = the middle speed is over twice the larger neighbour (and over 1 m/s), ydev = mean |y2 - (y1+y3)/2|
 *    (vertical bounce of a body that walks, mean per triplet), jumps = steps over 2 m (counted apart, left out of the rest).
 *    pm:worst names the body with the highest irr of the period (kind, net id, name, samples).
 *    `!pr pm` = the figures since the last report (no reset); `!pr reset`.
 *    `!pr trace <self|player|net:<id>|<script name>> <frames>` logs the next frames (<= 600) of one body:
 *    "pt: <label> f<n> t<ms> x y z" (10 frames a line): for comparing a client's drawn place with the host's.
 * 2. Foot height against the floor (`!pr feet`, and each report with TS_PROBES_FEET=1): the engine's own floor ray
 *    (the one the player placement uses, filter 0x100100) under every sampled body, from three start heights:
 *      fl: <kind> <label> y <entity y> d18 <y - floor, ray from 1.8 m above> d05 <y - floor, ray from 0.5 m above> d00 <ray from the foot point>
 *    The entity position is the foot point. Which ray to read depends on the body (live check 2026-10-30, N100 pair): PLAYERS (self,
 *    player): d00 (0.000 standing on the ground; d05 is exactly -0.500 because that ray starts inside the player's own capsule and
 *    d18 hits something 1.4 m above); AI soldiers: d05 or d18 (they stand 0-3 cm below the ray's floor on both machines, so d00
 *    starts under the floor and reads positive or "none"). A body that stands on the ground has the same value on both machines;
 *    one that differs by more than ~5 cm is sunk in or floating. d18 also hits ceilings in low rooms. Moving platforms are not hit.
 * 3. Two-body collision (`!pr pairs`, always collected while on): for every pair of sampled bodies that came within
 *    3 m horizontally (and 1.5 m vertically): the smallest horizontal centre distance since the last reset and the
 *    frames under `!pr bumpr <m>` (default 0.5 m, about a body's width). Two solid bodies walked into each other stay
 *    about 0.6-0.8 m apart; a distance near 0 means they went through each other. Drive them with the console
 *    (`!cp putin`, `!ss goto`, `Transport`) or pads; this only measures.
 *
 * Addresses: patch 1.2 exe (the entity table and fields as in aisync.c / campaign.c).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

#define GAME_MODE_OBJ (*(char **)0xfcab34)
#define MODE_OFF 0x3c
#define SLOTS_OFF 0x28
#define NSLOTS_OFF 0x2c
#define SLOT_SIZE 0xa4
#define PLAYER (*(char **)0x1095b30)
#define ENT_BUCKETS (*(char **)0xecb7fc)
#define ENT_NBUCKETS (*(int *)0xecb800)
#define E_NETID 0x18
#define E_ROLE 0x2c
#define E_INST 0x80
#define E_NODE 0x230
#define NODE_POS 0x68
#define WORLD_OBJ (*(void **)0xfcacdc)       /* the physics scene: `this` of the ray 0x95bf30 */
#define RAY_FN 0x95bf30                      /* thiscall (pos*, float maxdist, out*, float up, float outoff, filter*, hit*, 0), ret 0x20 */
#define RAY_FILTER_VT 0xd72ee0

static const DWORD ai_vtables[] = {0xd764d8, 0xd771e0, 0xd77870, 0xd788a8, 0xd78d28, 0xd79510, 0xd7a008, 0xdbfa68};

enum { K_PLAYER, K_AI, K_AIOWN, K_SELF, K_N };
static const char *kind_name[K_N] = {"player", "ai", "aiown", "self"};

#define MAXB 256
typedef struct {
    char *ent;
    int net, kind, seen;
    char label[32];
    double t[3];       /* the last three sample times (s) */
    float p[3][3];     /* ... and positions */
    int have;
    /* per-period results */
    int frames, stall, spike, jumps, nirr;
    double irr_sum, ydev_sum, vmax;
} Body;

typedef struct { double v[3], dt[3]; float step[3]; int n; } Speeds;
static Speeds spd[MAXB];
static Body bodies[MAXB];
static int n_bodies;
static int on, period_ms = 10000, feet_in_report;
static double t_start, t_report, last_t;
static unsigned long frame_no;
static double dt_sum, dt_max;
static unsigned long dt_n;
static LARGE_INTEGER qpf;

typedef struct { char *a, *b; char la[32], lb[32]; double min_d; int under, near_frames; unsigned long seen; } Pair;
#define MAXP 64
static Pair pairs[MAXP];
static int n_pairs;
static float bump_r = 0.5f;

typedef struct { char *ent; int active; int left; unsigned long frame0; char label[32]; char line[420]; int in_line; } Trace;
static Trace trace;

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)qpf.QuadPart;
}

static int is_ai(const char *o)
{
    if (!o)
        return 0;
    DWORD vt = *(const DWORD *)o;
    for (unsigned i = 0; i < sizeof ai_vtables / sizeof ai_vtables[0]; i++)
        if (vt == ai_vtables[i])
            return 1;
    return 0;
}

static const char *ent_name(const char *ent)
{
    const char *inst = *(char *const *)(ent + E_INST);
    if (!inst || IsBadReadPtr(inst, 0x10) || *(const char *const *)(inst + 0xc) != ent)
        return "";
    const char *s = *(const char *const *)(inst + 4);
    return s && !IsBadReadPtr(s, 0x20) ? s + 0xc : "";
}

static float *ent_posp(const char *ent)
{
    if (!ent || IsBadReadPtr(ent, E_NODE + 4))
        return NULL;
    char *node = *(char **)(ent + E_NODE);
    if (!node || IsBadReadPtr(node, NODE_POS + 12))
        return NULL;
    return (float *)(node + NODE_POS);
}

static Body *body_for(char *ent, int kind, const char *label)
{
    int net = *(int *)(ent + E_NETID);
    for (int i = 0; i < n_bodies; i++)
        if (bodies[i].ent == ent && bodies[i].net == net && bodies[i].kind == kind) {
            bodies[i].seen = 1;
            return &bodies[i];
        }
    if (n_bodies >= MAXB)
        return NULL;
    Body *b = &bodies[n_bodies++];
    memset(b, 0, sizeof *b);
    spd[n_bodies - 1].n = 0;
    b->ent = ent;
    b->net = net;
    b->kind = kind;
    b->seen = 1;
    snprintf(b->label, sizeof b->label, "%s", label);
    return b;
}

static void sample(Body *b, double t)
{
    float *p = ent_posp(b->ent);
    if (!p)
        return;
    b->t[0] = b->t[1], b->t[1] = b->t[2], b->t[2] = t;
    memcpy(b->p[0], b->p[1], 12), memcpy(b->p[1], b->p[2], 12), memcpy(b->p[2], p, 12);
    b->have++;
    b->frames++;
    if (b->have < 3)
        return;
    double dt1 = b->t[1] - b->t[0], dt2 = b->t[2] - b->t[1];
    if (dt1 <= 0 || dt2 <= 0 || dt1 > 0.5 || dt2 > 0.5)
        return;
    float dx = b->p[2][0] - b->p[1][0], dz = b->p[2][2] - b->p[1][2], dy = b->p[2][1] - b->p[1][1];
    double st = sqrt(dx * dx + dz * dz);
    if (st > 2.0 || fabs(dy) > 2.0) {
        b->jumps++;
        return;
    }
    if (st / dt2 > b->vmax)
        b->vmax = st / dt2;
    float dy1 = b->p[1][1] - b->p[0][1];
    if (fabs(dy1) <= 2.0 && st / dt2 > 0.5)
        b->ydev_sum += fabs(b->p[2][1] - (b->p[1][1] + b->p[0][1]) / 2);
}

/* The speed triplets: the speeds of the last three frame steps (a run breaks at a pause, a load or a jump). */

static void steps(int idx, Body *b)
{
    Speeds *s = &spd[idx];
    if (b->have < 2) {
        s->n = 0;
        return;
    }
    double dt = b->t[2] - b->t[1];
    float dx = b->p[2][0] - b->p[1][0], dz = b->p[2][2] - b->p[1][2];
    double st = sqrt(dx * dx + dz * dz);
    if (dt <= 0 || dt > 0.5 || st > 2.0 || fabs(b->p[2][1] - b->p[1][1]) > 2.0) {
        s->n = 0;     /* a pause, a load or a jump breaks the run */
        return;
    }
    s->v[0] = s->v[1], s->v[1] = s->v[2], s->v[2] = st / dt;
    s->step[0] = s->step[1], s->step[1] = s->step[2], s->step[2] = (float)st;
    s->n++;
    if (s->n < 3)
        return;
    double va = s->v[0], vb = s->v[1], vc = s->v[2];
    double mean = (va + vb + vc) / 3;
    if (mean <= 0.5)
        return;
    b->nirr++;
    b->irr_sum += fabs(vb - (va + vc) / 2) / mean;
    if (s->step[1] < 0.001f && va > 0.3 && vc > 0.3)
        b->stall++;
    if (vb > 2 * (va > vc ? va : vc) && vb > 1.0)
        b->spike++;
}

static void pair_track(Body *a, Body *b)
{
    float *pa = ent_posp(a->ent), *pb = ent_posp(b->ent);
    if (!pa || !pb)
        return;
    float dx = pa[0] - pb[0], dz = pa[2] - pb[2];
    float d = sqrtf(dx * dx + dz * dz);
    if (d > 3.0f || fabsf(pa[1] - pb[1]) > 1.5f)
        return;
    char *x = a->ent < b->ent ? a->ent : b->ent, *y = a->ent < b->ent ? b->ent : a->ent;
    Pair *p = NULL;
    for (int i = 0; i < n_pairs; i++)
        if (pairs[i].a == x && pairs[i].b == y)
            p = &pairs[i];
    if (!p) {
        if (n_pairs >= MAXP)
            return;
        p = &pairs[n_pairs++];
        memset(p, 0, sizeof *p);
        p->a = x, p->b = y, p->min_d = 1e9;
        snprintf(p->la, sizeof p->la, "%s", x == a->ent ? a->label : b->label);
        snprintf(p->lb, sizeof p->lb, "%s", x == a->ent ? b->label : a->label);
    }
    p->near_frames++;
    if (d < p->min_d)
        p->min_d = d;
    if (d < bump_r)
        p->under++;
}

static void collect(double t)
{
    char *gm = GAME_MODE_OBJ;
    for (int i = 0; i < n_bodies; i++)
        bodies[i].seen = 0;
    char *self = PLAYER;
    char *p2 = (char *)split_player2();
    if (self && !IsBadReadPtr(self, E_NODE + 4)) {
        Body *b = body_for(self, K_SELF, "self");
        if (b)
            sample(b, t), steps((int)(b - bodies), b);
    }
    int mode = gm ? *(int *)(gm + MODE_OFF) : 0;
    int ns = gm ? *(int *)(gm + NSLOTS_OFF) : 0;
    if (gm && (mode == 2 || mode == 3) && ns > 0 && ns < 64)
        for (int i = 0; i < ns; i++) {
            char *slot = *(char **)(gm + SLOTS_OFF) + i * SLOT_SIZE;
            char *e = *(char **)(slot + 4);
            if (!e || e == self || e == p2 || IsBadReadPtr(e, E_NODE + 4))
                continue;
            char label[32];
            snprintf(label, sizeof label, "slot%d", i);
            Body *b = body_for(e, K_PLAYER, label);
            if (b)
                sample(b, t), steps((int)(b - bodies), b);
        }
    char *bk = ENT_BUCKETS;
    int nb = ENT_NBUCKETS;
    for (int i = 0; bk && i < nb; i++)
        for (char *o = *(char **)(bk + i * 12); o; ) {
            if (is_ai(o) && !IsBadReadPtr(o, E_NODE + 4) && *(int *)(o + E_NETID) >= 0 && *(void **)(o + E_NODE)) {
                unsigned role = *(unsigned *)(o + E_ROLE);
                const char *nm = ent_name(o);
                char label[32];
                snprintf(label, sizeof label, "%.20s/%d", nm[0] ? nm : "ai", *(int *)(o + E_NETID));
                Body *b = body_for(o, (role & 1) ? K_AI : K_AIOWN, label);
                if (b)
                    sample(b, t), steps((int)(b - bodies), b);
            }
            char *next = *(char **)(o + 8);
            o = next ? next - 4 : NULL;
        }
    /* bodies gone this frame leave the table (their speeds restart if the same one comes back) */
    for (int i = 0; i < n_bodies; ) {
        if (!bodies[i].seen) {
            bodies[i] = bodies[n_bodies - 1];
            spd[i] = spd[n_bodies - 1];
            n_bodies--;
        } else
            i++;
    }
    for (int i = 0; i < n_bodies; i++)
        for (int j = i + 1; j < n_bodies; j++)
            pair_track(&bodies[i], &bodies[j]);
}

/* ---- the floor ray ---- */
typedef int (TSCALL *ray_t)(void *world, float *pos, float maxdist, float *out, float up, float outoff, void *filter, void *hit, int zero);
static int floor_under(char *ent, float up, float *y_out)
{
    float *p = ent_posp(ent);
    if (!p || !WORLD_OBJ)
        return 0;
    float pos[3] = {p[0], p[1], p[2]}, out[3] = {0, 0, 0};
    DWORD filter[8] = {RAY_FILTER_VT, 2, 0x100100, 0, 0, 0, (DWORD) * (void **)(ent + E_NODE), 0};
    unsigned char hit[128];
    memset(hit, 0, sizeof hit);
    int ok = ((ray_t)RAY_FN)(WORLD_OBJ, pos, 2000000.0f, out, up, 0.0f, filter, hit, 0) & 0xff;
    if (ok)
        *y_out = out[1];
    return ok;
}

/* "<y> d18 <y - floor from 1.8 m above> d05 <... 0.5 m above> d00 <... from the entity's own height>": a body that stands on the ground has one of the two near 0
 * (d18 is the engine's own start height; it can hit a ceiling in a low room, d05 can start inside a player's own capsule and then reads
 * -0.5 exactly, d00 starts at the foot point: take the one near 0) */
static int feet_one(char *out, int size, char *ent, const char *who)
{
    float *p = ent_posp(ent), f18, f05, f00;
    if (!p)
        return 0;
    int a = floor_under(ent, 1.8f, &f18), b = floor_under(ent, 0.5f, &f05), c = floor_under(ent, 0.0f, &f00);
    int n = snprintf(out, size, "%s y %.3f", who, p[1]);
    if (a)
        n += snprintf(out + n, size - n, " d18 %.3f", p[1] - f18);
    else
        n += snprintf(out + n, size - n, " d18 none");
    if (b)
        n += snprintf(out + n, size - n, " d05 %.3f", p[1] - f05);
    else
        n += snprintf(out + n, size - n, " d05 none");
    if (c)
        n += snprintf(out + n, size - n, " d00 %.3f", p[1] - f00);
    else
        n += snprintf(out + n, size - n, " d00 none");
    return n;
}

static int feet_lines(char *out, int size)
{
    int n = 0;
    char *self = PLAYER;
    if (self && ent_posp(self))
        n += feet_one(out + n, size - n, self, "self");
    else
        n += snprintf(out + n, size - n, "self: none");
    for (int i = 0; i < n_bodies && n < size - 160; i++) {
        Body *b = &bodies[i];
        char who[64];
        if (b->kind == K_SELF || !ent_posp(b->ent))
            continue;
        snprintf(who, sizeof who, "; %s %s", kind_name[b->kind], b->label);
        n += feet_one(out + n, size - n, b->ent, who);
    }
    return n;
}

/* ---- reports ---- */
static int pm_text(char *out, int size, int reset)
{
    int n = 0;
    double avg_dt = dt_n ? dt_sum / dt_n * 1000 : 0;
    for (int k = 0; k < K_N; k++) {
        int nb = 0, frames = 0, stall = 0, spike = 0, jumps = 0, nirr = 0;
        double irr = 0, ydev = 0, vmax = 0;
        Body *worst = NULL;
        double worst_irr = -1;
        for (int i = 0; i < n_bodies; i++) {
            Body *b = &bodies[i];
            if (b->kind != k)
                continue;
            nb++;
            frames += b->frames, stall += b->stall, spike += b->spike, jumps += b->jumps, nirr += b->nirr;
            irr += b->irr_sum, ydev += b->ydev_sum;
            if (b->vmax > vmax)
                vmax = b->vmax;
            if (b->nirr >= 20 && b->irr_sum / b->nirr > worst_irr)
                worst_irr = b->irr_sum / b->nirr, worst = b;
        }
        if (!nb)
            continue;
        n += snprintf(out + n, size - n,
                      "pm:%s bodies %d frames %d triplets %d stall %d spike %d jumps %d irr %.3f ydev %.1f mm vmax %.1f m/s | dt avg %.1f max %.1f ms\n",
                      kind_name[k], nb, frames, nirr, stall, spike, jumps, nirr ? irr / nirr : 0.0, nirr ? ydev / nirr * 1000 : 0.0, vmax,
                      avg_dt, dt_max * 1000);
        if (worst)
            n += snprintf(out + n, size - n, "pm:worst %s %s net %d irr %.3f samples %d stall %d\n", kind_name[k], worst->label, worst->net,
                          worst->irr_sum / worst->nirr, worst->nirr, worst->stall);
    }
    if (!n)
        n += snprintf(out + n, size - n, "pm: no bodies\n");
    if (reset) {
        for (int i = 0; i < n_bodies; i++) {
            Body *b = &bodies[i];
            b->frames = b->stall = b->spike = b->jumps = b->nirr = 0;
            b->irr_sum = b->ydev_sum = b->vmax = 0;
        }
        dt_sum = dt_max = 0;
        dt_n = 0;
    }
    return n;
}

static int pairs_text(char *out, int size)
{
    int n = 0;
    for (int i = 0; i < n_pairs && n < size - 120; i++) {
        Pair *p = &pairs[i];
        n += snprintf(out + n, size - n, "bp: %s <-> %s min %.3f m, under %.2f m: %d of %d near frames\n", p->la, p->lb, p->min_d, bump_r,
                      p->under, p->near_frames);
    }
    if (!n)
        n += snprintf(out + n, size - n, "bp: no pair came within 3 m\n");
    return n;
}

static void log_lines(const char *text)
{
    char line[512];
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len > 500)
            len = 500;
        memcpy(line, p, len);
        line[len] = 0;
        if (len)
            ts_log("%s", line);
        if (!e)
            break;
        p = e + 1;
    }
}

static void trace_flush(void)
{
    if (trace.in_line)
        ts_log("pt: %s%s", trace.label, trace.line);
    trace.in_line = 0;
    trace.line[0] = 0;
}

static void trace_frame(double t)
{
    if (!trace.active)
        return;
    int alive = 0;
    for (int i = 0; i < n_bodies; i++)
        alive |= bodies[i].ent == trace.ent;
    float *p = alive ? ent_posp(trace.ent) : NULL;
    if (!p || trace.left <= 0) {
        trace_flush();
        trace.active = 0;
        ts_log("pt: %s trace ended", trace.label);
        return;
    }
    int n = (int)strlen(trace.line);
    snprintf(trace.line + n, sizeof trace.line - n, " [f%lu t%.0f %.3f %.3f %.3f]", frame_no, (t - t_start) * 1000, p[0], p[1], p[2]);
    trace.left--;
    if (++trace.in_line >= 8)
        trace_flush();
}

static void probes_frame(float dt)
{
    (void)dt;
    if (!on)
        return;
    double t = now_s();
    frame_no++;
    if (last_t > 0) {
        double d = t - last_t;
        if (d < 0.5) {
            dt_sum += d, dt_n++;
            if (d > dt_max)
                dt_max = d;
        }
    }
    last_t = t;
    collect(t);
    trace_frame(t);
    if ((t - t_report) * 1000 >= period_ms) {
        static char text[4096];
        t_report = t;
        pm_text(text, sizeof text, 1);
        log_lines(text);
        if (feet_in_report) {
            static char f[2048];
            feet_lines(f, sizeof f);
            char buf[2400] = "fl: ";
            int bn = 4;
            for (char *s = f; *s && bn < (int)sizeof buf - 8; s++) {
                if (*s == ';') {
                    memcpy(buf + bn, "\nfl:", 4);
                    bn += 4;
                } else
                    buf[bn++] = *s;
            }
            buf[bn] = 0;
            log_lines(buf);
        }
    }
}

static void probes_on(int secs)
{
    if (!qpf.QuadPart)
        QueryPerformanceFrequency(&qpf);
    if (secs > 0)
        period_ms = secs * 1000;
    if (!on) {
        if (!ts_frame_add(probes_frame)) {
            ts_log("probes: the script tick is not available");
            return;
        }
        n_bodies = n_pairs = 0;
        t_start = t_report = now_s();
        last_t = 0;
        frame_no = 0;
        dt_sum = dt_max = 0;
        dt_n = 0;
    }
    on = 1;
    ts_log("probes: on, report every %d s", period_ms / 1000);
}

/* frames are added once and stay in the engine's list: off = the callback returns at once */
void probes_install(void)
{
    char v[32];
    if (!ts_env("TS_PROBES", v, sizeof v) || !atoi(v))
        return;
    feet_in_report = ts_env("TS_PROBES_FEET", v, sizeof v) && v[0] == '1';
    ts_env("TS_PROBES", v, sizeof v);
    probes_on(atoi(v) > 1 ? atoi(v) : 10);
}

int probes_console(const char *text, char *out, int size)
{
    if (strncmp(text, "!pr", 3) != 0 || (text[3] && text[3] != ' '))
        return 0;
    char cmd[16] = "", a1[64] = "";
    int a2 = 0;
    sscanf(text + 3, "%15s %63s %d", cmd, a1, &a2);
    if (!strcmp(cmd, "on")) {
        probes_on(atoi(a1));
        return snprintf(out, size, "probes on (report every %d s)", period_ms / 1000), 1;
    }
    if (!strcmp(cmd, "off")) {
        on = 0;
        return snprintf(out, size, "probes off"), 1;
    }
    if (!on)
        return snprintf(out, size, "probes are off: `!pr on [seconds]` or TS_PROBES=<seconds>"), 1;
    if (!strcmp(cmd, "pm")) {
        pm_text(out, size, 0);
        return 1;
    }
    if (!strcmp(cmd, "reset")) {
        pm_text(out, size, 1);
        n_pairs = 0;
        return snprintf(out + strlen(out), size - (int)strlen(out), "reset"), 1;
    }
    if (!strcmp(cmd, "feet")) {
        feet_lines(out, size);
        ts_log("fl: %.1500s", out);
        return 1;
    }
    if (!strcmp(cmd, "pairs")) {
        pairs_text(out, size);
        log_lines(out);
        return 1;
    }
    if (!strcmp(cmd, "bumpr")) {
        float r = (float)atof(a1);
        if (r > 0.05f && r < 5.f)
            bump_r = r;
        return snprintf(out, size, "pair distance threshold %.2f m", bump_r), 1;
    }
    if (!strcmp(cmd, "trace")) {
        int frames = a2 > 0 && a2 <= 600 ? a2 : 120;
        char *e = NULL;
        for (int i = 0; i < n_bodies && !e; i++) {
            Body *b = &bodies[i];
            char netsel[32];
            snprintf(netsel, sizeof netsel, "net:%d", b->net);
            if ((!strcmp(a1, "self") && b->kind == K_SELF) || (!strcmp(a1, "player") && b->kind == K_PLAYER) || !strcmp(a1, netsel)
                || (b->kind >= K_AI && b->kind <= K_AIOWN && !strncmp(b->label, a1, strlen(a1)) && a1[0]))
                e = b->ent, snprintf(trace.label, sizeof trace.label, "%s(%s)", kind_name[b->kind], b->label);
        }
        if (!e)
            return snprintf(out, size, "no such body: self | player | net:<id> | <script name prefix>"), 1;
        trace.ent = e;
        trace.left = frames;
        trace.active = 1;
        trace.in_line = 0;
        trace.line[0] = 0;
        return snprintf(out, size, "tracing %s for %d frames into the log (pt: lines)", trace.label, frames), 1;
    }
    return snprintf(out, size, "!pr on [s] | off | pm | reset | feet | pairs | bumpr <m> | trace <self|player|net:<id>|name> [frames]"), 1;
}
