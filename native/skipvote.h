/*
 * The co-op skip vote's tally (parity-016 round 4, sync-parity top gap 7): pure logic, no game state, so that
 * tests/skipvote_test.c can check it offline. skipvote.c feeds it.
 *
 * Every machine of the session is one entry: does it show a cinematic now ("watching"), how many players sit at it (1, or 2
 * with split screen), which of them pressed Skip (bits). The host decides:
 *   - a machine that watches needs a vote from each of its players;
 *   - a machine that does not watch has nothing to skip and does not hold the others back, EXCEPT in the first
 *     SV_GRACE_MS of the host's own cinematic: a client's report of its scene may still be on the way then, so every
 *     client counts as watching (otherwise the host could skip alone before the client's scene started, the old fault);
 *   - the skip goes when at least one vote exists and every watching player voted.
 * Votes of an older round (before the last skip order) do not count: a client reports the round it last heard of.
 */
#ifndef SKIPVOTE_H
#define SKIPVOTE_H

#define SV_MAX 16
#define SV_GRACE_MS 3000u

typedef struct {
    int id;              /* client id (0 = the host itself) */
    int watching;        /* shows a cinematic now (its last report) */
    int players;         /* 1, or 2 with split screen */
    unsigned votes;      /* bit 0 player 1, bit 1 player 2 */
    unsigned round;      /* the skip round the report belongs to */
} sv_machine;

static int sv_bits(unsigned v, int players)
{
    int n = 0;
    for (int i = 0; i < players && i < 2; i++)
        n += (v >> i) & 1;
    return n;
}

/* host: m[0] = the host itself, m[1..n-1] the clients that run the level. host_cine_ms = how long the host's own cinematic
 * runs (0 = none). Fills *votes / *need (watching players) and returns 1 when the skip goes. */
static int sv_decide(const sv_machine *m, int n, unsigned round, unsigned host_cine_ms, int *votes, int *need)
{
    int v = 0, w = 0;
    for (int i = 0; i < n && i < SV_MAX; i++) {
        int players = m[i].players < 1 ? 1 : m[i].players > 2 ? 2 : m[i].players;
        int grace = i > 0 && host_cine_ms > 0 && host_cine_ms < SV_GRACE_MS;
        if (!m[i].watching && !grace)
            continue;
        w += players;
        if (m[i].watching && m[i].round == round)
            v += sv_bits(m[i].votes, players);
    }
    if (votes)
        *votes = v;
    if (need)
        *need = w;
    return v > 0 && w > 0 && v >= w;
}

/* The vote applies at all (else Skip is the stock one): a network co-op session with at least one other machine (a client is
 * always with its host), or split screen on one machine. */
static int sv_gate(int coop_session, int is_client, int clients_in_game, int local_players)
{
    if (coop_session)
        return is_client || clients_in_game > 0 || local_players > 1;
    return local_players > 1;
}

#endif
