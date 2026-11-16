/*
 * The co-op skip vote for cinematics (parity-016 round 4, sync-parity top gap 7; patch 1.2 exe only).
 *
 * The stock skip: the command STOP_CINEMATIC (id 0x30; Esc in controls.ini, Start on the pad through gamepad.c) is read as
 * "pressed" (flags +0x64 bit 2 of its entry in the active command context [IM+0x40]) by exactly two places:
 *   - the player's update 0x40c020 while a scene runs ([0x1095f90] & 0x10) calls 0x40c830: pressed, the player alive
 *     (vtable +0x214 = 0) and the scene not "noSkip" (0x811d30: [0x109649c] +0x2c current item, +0x78 description, +0x104
 *     bit 0) -> 0x816480 + 0x811c80, the stop of the two cinematic managers ([0xfcabac] and [0x109649c]: their +0x28 bits
 *     3 / 0 "skipping", a fade, "vo_empty" on the message line; the scene then runs to its end with its end events);
 *   - a game object's update 0x4fbf00 (vtable 0xd8d090) while a film runs ([0x1095f90] & 0x20): pressed and not
 *     [0xfcace0] & 0x100 (the film's noSkip; the trailer is always skippable) -> 0x6cc870, the film's stop.
 * So a skip from code is safe in the stock way: give the game ONE frame with STOP_CINEMATIC pressed, and its own checks and
 * stops run where they always run. That is how the vote ends a scene; nothing else is called.
 *
 * In co-op (a network co-op session with another machine, or split screen on one machine) every press of STOP_CINEMATIC is
 * taken out of the command context right after the engine's command evaluation (split.c's hook of IM vtable +0x34 calls
 * skipvote_after_eval after both players' passes; without split.c this file hooks it itself) and becomes a vote of that
 * player: player 1 = the machine's own keyboard and pad 1, player 2 = split screen's pad 2 (its own command context).
 * Clients send their state to the host (watching a skippable cinematic, players, votes); the host counts (skipvote.h) and,
 * when every watching player voted, tells every client "skip now" and presses STOP_CINEMATIC for one frame itself; each
 * client does the same when the order arrives. Split screen in single player counts its two players alone.
 * HUD (the game's message line, 9 languages): on a new vote every machine shows "<name> wants to skip the scene (1/2)"
 * (split screen: "Player 2 wants ..."), and every 3 s while the vote is open "Skip the scene: 1 of 2 players. Press Start or
 * Esc to skip as well." where a player has not voted yet, "... Waiting for the others." where everybody did.
 * Single player without split screen, the host alone in its session and stock multiplayer maps: the stock skip.
 *
 * Messages (coopnet.c, reliable, subtypes 110-119):
 *   110 client -> host  {u8 1, u8 watching, u8 players, u8 vote bits, u16 round, u8 n, name[n]}  (on change, every 2 s while watching)
 *   111 host -> clients {u8 1, u8 kind (0 the round only, 1 a new vote, 2 reminder), u8 votes, u8 needed, u16 round, u8 n, name[n]}
 *                       (kind 0 goes to a client whose report names another round: a late joiner learns the host's round)
 *   112 host -> clients {u8 1, u16 round}: skip now (votes of earlier rounds no longer count)
 * TS_SKIPVOTE=0: the stock skip everywhere. TS_SKIPVOTE_LOG=1: every press logged.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"
#include "skipvote.h"

#define TSCALL __attribute__((thiscall))

#define IM (*(BYTE **)0xfcabb4)             /* input manager: +0x40 active command context */
#define IM_VTBL 0x00d8be18
#define SLOT_EVAL (0x34 / 4)                /* 0x4f3190 */
#define CMD_STOP_CINEMATIC 0x30
#define CMD_PRESSED 4                       /* entry +0x64 flags: bit 2 */
#define CINE_FLAGS (*(volatile DWORD *)0x1095f90)   /* 0x10 a scene runs, 0x20 a film runs */
#define FILM_FLAGS (*(volatile DWORD *)0xfcace0)    /* 0x100 the film may not be skipped */
#define MGR_A (*(BYTE **)0xfcabac)          /* cinematic managers: +0x2c current list, +0x28 flags (A bit 3, B bit 0: skipping) */
#define MGR_B (*(BYTE **)0x109649c)
#define GAME_MODE_OBJ (*(char **)0xfcab34)  /* +0x3c mode, +0x28 slots, +0x2c count */
#define SLOT_SIZE 0xa4
#define SLOT_INGAME 1
#define SLOT_READY 0x80
#define SLOT_JOINED 0x100
#define MODE_SP 1
#define MODE_SERVER 2
#define MODE_CLIENT 3

#define SUB_STATE 110
#define SUB_SHOW 111
#define SUB_GO 112
#define SUB_LAST 119
#define PROTO 1
#define MAX_CLIENTS 15
#define NAME_MAX 24

static int enabled, verbose;
static LONG n_logged;
#define SLOG(...) do { if (InterlockedIncrement(&n_logged) <= 600) ts_log(__VA_ARGS__); } while (0)

typedef void (TSCALL *eval_t)(void *);
static eval_t real_eval;

/* ---- this machine ---- */
static volatile LONG press_bits;            /* (eval -> tick) players whose press was taken this frame, bit 0 / 1 */
static volatile LONG inject;                /* (tick -> eval) give the game one frame with STOP_CINEMATIC pressed */
static volatile LONG gating;                /* (tick -> eval) the vote applies now: presses are taken */
static unsigned local_votes, round_no;
static int was_watching, local_players = 1;
static DWORD cine_since, injected_at;
static char my_name[NAME_MAX + 1];

/* ---- host: the clients' reports ---- */
static struct { int id, watching, players; unsigned votes, round; DWORD at, synced_at; char name[NAME_MAX + 1]; } rep[MAX_CLIENTS];
static int n_rep;
static int last_votes;
static DWORD reminder_at, go_at;
/* ---- client: the host's last word ---- */
static volatile LONG got_go = -1;           /* (net pump -> tick) the round of a skip order */
static volatile LONG host_round = -1;       /* (net pump -> tick) the host's round, from its last message */
static struct { int kind, votes, need; char name[NAME_MAX + 1]; } show_in;
static volatile LONG show_seq;
static LONG show_done;
static DWORD sent_at;
static int sent_watching = -1, sent_players, sent_round = -1;
static unsigned sent_votes;

static int game_mode(void)
{
    char *gm = GAME_MODE_OBJ;
    return gm ? *(int *)(gm + 0x3c) : 0;
}

static char *slot_at(int i)
{
    char *gm = GAME_MODE_OBJ;
    return gm && i >= 0 && i < *(int *)(gm + 0x2c) ? *(char **)(gm + 0x28) + i * SLOT_SIZE : NULL;
}

static int client_in_game(char *s)
{
    int fl = s ? *(int *)(s + 8) : 0;
    return s && *(int *)s != 0 && (fl & SLOT_INGAME) && (fl & (SLOT_JOINED | SLOT_READY)) == (SLOT_JOINED | SLOT_READY);
}

static int clients_in_game(void)
{
    int n = 0;
    for (int i = 0; slot_at(i) && i < 32; i++)
        n += client_in_game(slot_at(i));
    return n;
}

static int any_item(BYTE *mgr)
{
    int **l = mgr ? *(int ***)(mgr + 0x2c) : NULL;
    return l && *l;
}

/* a cinematic this machine shows and the stock skip would end (see the top) */
static int watching_now(void)
{
    DWORD f = CINE_FLAGS;
    if (f & 0x10) {
        BYTE *b = MGR_B;
        if (any_item(b)) {
            BYTE *item = (BYTE *)**(int **)(b + 0x2c), *desc = item ? *(BYTE **)(item + 0x78) : NULL;
            if (desc && (desc[0x104] & 1))
                return 0;   /* (noSkip) */
        }
        if (any_item(MGR_A) || any_item(b))
            return 1;
    }
    return (f & 0x20) && !(FILM_FLAGS & 0x100);
}

static int skip_started(void)
{
    BYTE *a = MGR_A, *b = MGR_B;
    return (a && (*(DWORD *)(a + 0x28) & 8)) || (b && (*(DWORD *)(b + 0x28) & 1));
}

/* ---- the command context (main thread, after the engine's evaluation) ---- */
static DWORD *stop_flags(BYTE *ctx)
{
    if (!ctx || IsBadReadPtr(ctx, 0x10))
        return NULL;
    int first = *(int *)(ctx + 8), end = *(int *)(ctx + 0xc);
    BYTE *e = *(BYTE **)(ctx + 4);
    if (!e || first > CMD_STOP_CINEMATIC || end <= CMD_STOP_CINEMATIC)
        return NULL;
    return (DWORD *)(e + (CMD_STOP_CINEMATIC - first) * 0x68 + 0x64);
}

void skipvote_after_eval(void *im, void *ctx2)
{
    if (!enabled || !im)
        return;
    DWORD *f1 = stop_flags(*(BYTE **)((BYTE *)im + 0x40)), *f2 = ctx2 ? stop_flags(ctx2) : NULL;
    if (InterlockedExchange(&inject, 0)) {   /* the vote is through: the stock skip gets its press */
        if (f1)
            *f1 |= CMD_PRESSED;
        if (f2)
            *f2 &= ~(DWORD)CMD_PRESSED;
        injected_at = GetTickCount();
        return;
    }
    if (!gating)
        return;
    LONG bits = 0;
    if (f1 && (*f1 & CMD_PRESSED)) {
        *f1 &= ~(DWORD)CMD_PRESSED;
        bits |= 1;
    }
    if (f2 && (*f2 & CMD_PRESSED)) {
        *f2 &= ~(DWORD)CMD_PRESSED;
        bits |= 2;
    }
    if (bits)
        InterlockedOr(&press_bits, bits);
}

static void TSCALL hook_eval(void *im)
{
    real_eval(im);
    skipvote_after_eval(im, NULL);
}

/* ---- HUD line ---- */
static void show(const char *fmt, ...)
{
    char id[160], ssl[220], out[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(id, sizeof id, fmt, ap);
    va_end(ap);
    snprintf(ssl, sizeof ssl, "$player.ShowMsg(\"%s\", 3)", id);
    ts_script_run(ssl, out, sizeof out);
    if (verbose)
        SLOG("skipvote: HUD %s", id);
}

/* the reminder on this machine: "press" while one of its players has not voted, else "waiting" */
static void remind(int votes, int need)
{
    int mine = sv_bits(local_votes, local_players);
    show(mine < local_players ? "COOP_SKIP_PRESS,%d,%d" : "COOP_SKIP_WAIT,%d,%d", votes, need);
}

static void voter_name(char *out, int size, const char *name, int bit, int split_single)
{
    if (split_single)
        snprintf(out, size, "%d", bit + 1);
    else
        snprintf(out, size, bit ? "%.24s (2)" : "%.24s", name[0] ? name : "?");
}

static void clean_name(char *out, const char *in)
{
    int n = 0;
    for (int i = 0; in[i] && n < NAME_MAX; i++)
        if ((unsigned char)in[i] >= 32 && in[i] != ',' && in[i] != '"' && in[i] != '\\')
            out[n++] = in[i];
    out[n] = 0;
}

static void send_show_to(int slot, int kind, int votes, int need, const char *name)
{
    unsigned char b[8 + NAME_MAX + 8] = {PROTO, (unsigned char)kind, (unsigned char)votes, (unsigned char)need,
                                         (unsigned char)(round_no & 0xff), (unsigned char)(round_no >> 8)};
    int nl = (int)strlen(name);
    if (nl > NAME_MAX + 4)
        nl = NAME_MAX + 4;
    b[6] = (unsigned char)nl;
    memcpy(b + 7, name, nl);
    for (int i = slot < 0 ? 0 : slot; slot_at(i) && i < 32; i++) {
        if (client_in_game(slot_at(i)))
            coopnet_send(i, SUB_SHOW, b, 7 + nl);
        if (slot >= 0)
            break;
    }
}

static void send_show(int kind, int votes, int need, const char *name)
{
    send_show_to(-1, kind, votes, need, name);
}

static void go_skip(const char *why)
{
    InterlockedExchange(&inject, 1);
    go_at = GetTickCount();
    SLOG("skipvote: skip (%s): the game's own skip gets one press of STOP_CINEMATIC here (flags %#lx)", why, CINE_FLAGS);
}

/* ---- net pump (main thread): copy, the tick acts ---- */
static void on_message(int peer, unsigned short sub, const unsigned char *d, int len)
{
    if (!enabled || len < 2 || d[0] != PROTO)
        return;
    int mode = game_mode();
    if (sub == SUB_STATE && mode == MODE_SERVER && len >= 7) {
        int k = 0;
        while (k < n_rep && rep[k].id != peer)
            k++;
        if (k == n_rep) {
            if (n_rep >= MAX_CLIENTS)
                return;
            memset(&rep[k], 0, sizeof rep[k]);
            rep[k].id = peer;
            n_rep++;
        }
        unsigned before = rep[k].votes;
        rep[k].watching = d[1] != 0;
        rep[k].players = d[2] >= 2 ? 2 : 1;
        rep[k].votes = d[3] & 3;
        rep[k].round = (unsigned)(d[4] | (d[5] << 8));
        rep[k].at = GetTickCount();
        int nl = d[6];
        if (nl > NAME_MAX)
            nl = NAME_MAX;
        if (len >= 7 + nl) {
            char nm[NAME_MAX + 1];
            memcpy(nm, d + 7, nl);
            nm[nl] = 0;
            clean_name(rep[k].name, nm);
        }
        if (verbose || rep[k].votes != before)
            SLOG("skipvote: client %d: watching %d, players %d, votes %u (round %u)", peer, rep[k].watching, rep[k].players,
                 rep[k].votes, rep[k].round);
    } else if (sub == SUB_SHOW && mode == MODE_CLIENT && len >= 7) {
        int nl = d[6] > NAME_MAX + 4 ? NAME_MAX + 4 : d[6];
        if (len < 7 + nl)
            return;
        InterlockedExchange(&host_round, (LONG)(d[4] | (d[5] << 8)));
        if (d[1] == 0)
            return;   /* (the round only) */
        show_in.kind = d[1], show_in.votes = d[2], show_in.need = d[3];
        char nm[NAME_MAX + 8];
        memcpy(nm, d + 7, nl);
        nm[nl] = 0;
        snprintf(show_in.name, sizeof show_in.name, "%s", nm);
        InterlockedIncrement(&show_seq);
    } else if (sub == SUB_GO && mode == MODE_CLIENT && len >= 3)
        InterlockedExchange(&got_go, (LONG)(d[1] | (d[2] << 8)));
}

/* ---- the tick (main thread, every frame) ---- */
static int new_votes_name(unsigned before, unsigned after, const char *name, int split_single, char *out, int size)
{
    unsigned fresh = after & ~before;
    if (!fresh)
        return 0;
    voter_name(out, size, name, (fresh & 1) ? 0 : 1, split_single);
    return 1;
}

static void frame(float dt)
{
    (void)dt;
    DWORD now = GetTickCount();
    int mode = game_mode(), coop = ts_coop_active();
    int client = coop && mode == MODE_CLIENT, host = coop && mode == MODE_SERVER;
    local_players = split_p2_local() ? 2 : 1;
    int watching = watching_now();
    int on = sv_gate(coop, client, host ? clients_in_game() : 0, local_players) && (coop || mode == MODE_SP);
    InterlockedExchange(&gating, on && watching);
    if (watching && !was_watching) {
        cine_since = now;
        local_votes = 0;
        last_votes = 0;
        go_at = 0;
        if (on)
            SLOG("skipvote: a cinematic starts here (flags %#lx, %d player(s) here)", CINE_FLAGS, local_players);
    } else if (!watching && was_watching) {
        local_votes = 0;
        last_votes = 0;
        if (go_at)
            SLOG("skipvote: the cinematic is over here (%lu ms after the skip)", now - go_at);
        else if (on)
            SLOG("skipvote: the cinematic is over here (its own end, no skip)");
        go_at = 0;
    }
    was_watching = watching;
    if (injected_at && now - injected_at > 1500) {
        if (watching && !skip_started() && (CINE_FLAGS & 0x10))
            SLOG("skipvote: the scene still runs 1.5 s after the press (the game refused the skip: player dead or noSkip?)");
        injected_at = 0;
    }
    LONG bits = InterlockedExchange(&press_bits, 0);
    if (!on) {
        n_rep = 0;
        return;
    }
    unsigned before = local_votes;
    if (bits && watching) {
        local_votes |= (unsigned)bits & ((1u << local_players) - 1);
        if (verbose || local_votes != before)
            SLOG("skipvote: skip pressed by player%s %s here (votes %u)", bits == 3 ? "s 1 and 2" : "", bits == 2 ? "2" : bits == 1 ? "1" : "",
                 local_votes);
    }
    if (!my_name[0]) {
        char nm[64] = "";
        ts_profile_value("PlayerName", nm, sizeof nm);
        clean_name(my_name, nm);
        if (!my_name[0])
            snprintf(my_name, sizeof my_name, "Player");
    }

    if (client) {
        LONG hr = InterlockedExchange(&host_round, -1);
        if (hr >= 0 && (unsigned)hr != round_no) {
            SLOG("skipvote: the host's round is %ld (here %u)", hr, round_no);
            round_no = (unsigned)hr;
        }
        LONG go = InterlockedExchange(&got_go, -1);
        if (go >= 0) {
            round_no = (unsigned)go;
            local_votes = 0;
            if (watching)
                go_skip("every player voted: the host's order");
            else
                SLOG("skipvote: the host's skip order (round %ld): no cinematic here", go);
        }
        LONG seq = show_seq;
        if (seq != show_done) {
            show_done = seq;
            if (watching) {
                if (show_in.kind == 1)
                    show("COOP_SKIP_WANTS,%s,%d,%d", show_in.name, show_in.votes, show_in.need);
                else
                    remind(show_in.votes, show_in.need);
            }
        }
        if (watching != sent_watching || local_votes != sent_votes || local_players != sent_players || (int)round_no != sent_round
            || (watching && now - sent_at >= 2000)) {
            unsigned char b[7 + NAME_MAX] = {PROTO, (unsigned char)watching, (unsigned char)local_players, (unsigned char)local_votes,
                                             (unsigned char)(round_no & 0xff), (unsigned char)(round_no >> 8)};
            int nl = (int)strlen(my_name);
            b[6] = (unsigned char)nl;
            memcpy(b + 7, my_name, nl);
            if (coopnet_send(-1, SUB_STATE, b, 7 + nl) > 0) {
                sent_watching = watching, sent_votes = local_votes, sent_players = local_players, sent_round = (int)round_no;
                sent_at = now;
            }
        }
        return;
    }

    /* host or split screen in single player: the tally */
    sv_machine m[1 + MAX_CLIENTS];
    int n = 1, split_single = !coop;
    m[0].id = 0, m[0].watching = watching, m[0].players = local_players, m[0].votes = local_votes, m[0].round = round_no;
    char voter[NAME_MAX + 8] = "";
    new_votes_name(before, local_votes, my_name, split_single, voter, sizeof voter);
    if (host) {
        for (int k = 0; k < n_rep; k++) {   /* (reports of clients that are no longer in the level are dropped) */
            int here = 0;
            for (int i = 0; slot_at(i) && i < 32; i++)
                here |= client_in_game(slot_at(i)) && *(int *)slot_at(i) == rep[k].id;
            if (!here) {
                SLOG("skipvote: client %d left: its report is dropped", rep[k].id);
                rep[k--] = rep[--n_rep];
            }
        }
        static unsigned seen[MAX_CLIENTS];
        static int seen_id[MAX_CLIENTS];
        for (int i = 0; slot_at(i) && i < 32 && n < 1 + MAX_CLIENTS; i++) {
            char *s = slot_at(i);
            if (!client_in_game(s))
                continue;
            int id = *(int *)s, k = 0;
            while (k < n_rep && rep[k].id != id)
                k++;
            m[n].id = id;
            if (k < n_rep && rep[k].round != round_no && now - rep[k].synced_at >= 2000) {
                rep[k].synced_at = now;
                send_show_to(i, 0, 0, 0, "");   /* (its report names another round: tell it ours) */
            }
            if (k < n_rep && now - rep[k].at < 6000) {
                m[n].watching = rep[k].watching, m[n].players = rep[k].players, m[n].votes = rep[k].votes, m[n].round = rep[k].round;
                if (seen_id[n - 1] != id)
                    seen_id[n - 1] = id, seen[n - 1] = 0;
                unsigned cur = rep[k].round == round_no && rep[k].watching ? rep[k].votes : 0;
                if (!voter[0])
                    new_votes_name(seen[n - 1], cur, rep[k].name, 0, voter, sizeof voter);
                seen[n - 1] = cur;
            } else
                m[n].watching = 0, m[n].players = 1, m[n].votes = 0, m[n].round = round_no;
            n++;
        }
    }
    int votes = 0, need = 0;
    int go = sv_decide(m, n, round_no, watching ? now - cine_since + 1 : 0, &votes, &need);
    if (voter[0] && votes > last_votes && need > 0) {
        if (watching)
            show(split_single ? "COOP_SKIP_WANTS_P,%s,%d,%d" : "COOP_SKIP_WANTS,%s,%d,%d", voter, votes, need);
        if (host)
            send_show(1, votes, need, voter);
        SLOG("skipvote: %s%s wants to skip (%d/%d)", split_single ? "player " : "", voter, votes, need);
        reminder_at = now;
    }
    last_votes = votes;
    if (go && (!go_at || now - go_at > 2000)) {
        round_no++;
        local_votes = 0;
        last_votes = 0;
        SLOG("skipvote: every watching player voted (%d/%d): skip on every machine (round %u)", votes, need, round_no);
        if (host) {
            unsigned char b[3] = {PROTO, (unsigned char)(round_no & 0xff), (unsigned char)(round_no >> 8)};
            for (int i = 0; slot_at(i) && i < 32; i++)
                if (client_in_game(slot_at(i)))
                    coopnet_send(i, SUB_GO, b, 3);
        }
        if (watching)
            go_skip(host ? "every player voted" : "both players voted");
        else
            go_at = now;
        return;
    }
    if (votes > 0 && votes < need && now - reminder_at >= 3000) {
        reminder_at = now;
        if (watching)
            remind(votes, need);
        if (host)
            send_show(2, votes, need, "");
    }
}

void skipvote_install(void)
{
    char v[16];
    if (ts_env("TS_SKIPVOTE", v, sizeof v) && v[0] == '0') {
        ts_log("skipvote: off (TS_SKIPVOTE=0): the stock skip everywhere");
        return;
    }
    verbose = ts_env("TS_SKIPVOTE_LOG", v, sizeof v) && v[0] == '1';
    /* (the eval hook: split.c's, which calls skipvote_after_eval, or this file's when split.c is off) */
    void **iv = (void **)IM_VTBL;
    int own = 0;
    if ((DWORD)iv[SLOT_EVAL] == 0x4f3190) {
        void *vt = (void *)IM_VTBL;
        void *o = ts_patch_vtable(&vt, SLOT_EVAL, (void *)hook_eval);
        if (!o) {
            ts_log("skipvote: the command evaluation could not be hooked: the stock skip everywhere");
            return;
        }
        real_eval = (eval_t)o;
        own = 1;
    }
    int ok_tick = ts_frame_add(frame), ok_net = coopnet_listen(SUB_STATE, SUB_LAST, on_message);
    enabled = ok_tick;
    ts_log("skipvote: %s (co-op: every player's skip is a vote; command hook %s, messages %s)", enabled ? "on" : "OFF (no script tick)",
           own ? "own" : "split screen's", ok_net ? "on" : "off");
}
