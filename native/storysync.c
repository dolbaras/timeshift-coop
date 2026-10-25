/*
 * The campaign's story in co-op on the game's own server (patch 1.2 exe only; addresses from the static
 * analysis in the private notes). On by default (coopsession.c sets TS_STORYSYNC=1 when it is not set); it acts
 * only in a co-op session whose host runs the story (COOP_F_STORYSYNC). TS_STORYSYNC=0 switches it off (on the
 * host: for the whole session); nothing changes in single player (game mode 1) or on stock multiplayer maps.
 *
 * Engine facts this builds on:
 * - Level objects with script text get their own script class: the entity's script set-up (0x644e50) creates
 *   a class and compiles the text with 0x8f9150 (call at 0x644fc4; the hint argument is the entity's name).
 * - Every script event of every object goes through one function, 0x903940 (thiscall: class, Status *out,
 *   instance, member index, argc, argv; ret 0x14), called from three places: by index (0x8f82bf), by name
 *   (0x8f97c6) and the per-frame mask events of an entity (0x905462: OnEnter 0x200, OnLeave 0x400,
 *   OnUnlocked 2, ... from entity+0x84 in 0x6442f0). Named events with arguments (OnBhvEnd, OnActionFrame,
 *   OnEye, ...) are fired by name with argument values.
 * - Domains (trigger volumes) test the points of one watched object (+0xf8: an object the level's domains share
 *   for the local player, not the $player entity) in 0x6c2dd0 when it moves, and fire OnEnter/OnLeave from
 *   the inside/outside state. A domain's IsBelong (0x6c3d60) tests the
 *   position of any entity, remote players included (measured: the host saw a client's body inside).
 * - A reload domain (level exit) needs the authority bit (clients never have it) and sends the single-player
 *   LOAD_LEVEL (0x3ec) to the world object (call at 0x8aa0e3): the host alone would load the next level. The
 *   server's own map change posts 0x43e to the level loader (as the rounds of a match do, 0x606960); the
 *   loader (0x6e3690) tells every client and loads the level everywhere.
 * - Checkpoints: $saves_mng.AutoSave(id) (0x6ce030). Respawn points: mp_rules_sys [0xfcab18] +0x114
 *   (stride 0x1c: position, angles, team flags), built from the level's start_pos objects.
 *
 * What this file does in a network game:
 * - Classification (both sides, when a level object's script is compiled): each event handler of the object
 *   is "local" when every statement only does presentation for the machine's own player (light zones, camera
 *   shake, hints, $tc.SetMode, sound, holster and fall-height zones), else "story". OnInitLevel and OnInit are
 *   always local (each machine sets up its own copy of the level).
 * - Client: the script text is compiled with the statements that only the host may run (AI control, $farm
 *   creations, deaths, saves, failure, level loads, seats) wrapped in "if false ... end" (the first
 *   never-true condition a test compile accepts). Story events the
 *   client detects itself do not run; domain entries and exits are sent to the host (20), look triggers too
 *   (22). Story events the host ran arrive as replays (21) and run here, at the same moment, with the
 *   wrapped text: the presentation and the client's own $player calls (locks, teleports, time-power
 *   settings, loadouts, hints) happen for the client's player, the world changes come from the host.
 * - Host: runs every handler as before. A story domain counts as entered while any player is inside (the
 *   host's player, a client's body by IsBelong, or a client's report), so the first player to arrive starts
 *   the story and OnLeave fires when the last one leaves. Every story event it runs is sent to the clients
 *   (21: object name, event name, arguments) before it runs.
 * - Level exits: on the host the single-player LOAD_LEVEL of a reload domain (and $ui.LoadLevel /
 *   RestartLevel of a campaign level) becomes the server's map change: everybody loads the next level.
 *   On a client a campaign level load is refused (the host decides).
 * - Checkpoints and deaths: AutoSave(n) on the host moves every respawn point to the checkpoint
 *   (Check_Point_<n> if the level has it, else the player who set it off, else the host's player).
 *   FailMission on the host restarts the level for everybody (TS_STORYSYNC_FAIL=death keeps the game's
 *   multiplayer death instead); after the reload, a level with a Check_Point_<n> object is put back at the
 *   last checkpoint by unlocking that object (its own restore script, replayed to the clients).
 *
 * Mod messages (coopnet.c, subtypes 20-39, reliable):
 *   20 client -> server  {u8 1, u8 inside, name}                    a story domain entered / left
 *   21 server -> clients {u8 1, u8 argc, u8 n, name, u8 n, event, args}  run this story event here
 *   22 client -> server  {u8 1, u8 on, name}                        a story look trigger (OnEye) fired
 *   args: u8 type (0 null, 1 int, 2 bool, 3 float, 4 string) + value (int/float 4 bytes, bool 1, string u8 len + bytes)
 *
 * Test console (TS_CONSOLE): "!ss state", "!ss class <object>", "!ss enter <domain>" / "!ss leave <domain>"
 * (client: report as if the player entered/left), "!ss fire <object> <event>" (host: run an event, replayed),
 * "!ss exit <level>" (host: server level change), "!ss restart" (host: team restart).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

/* ---- engine state ---- */
#define GAME_MODE_OBJ (*(char **)0xfcab34)   /* +0x3c mode, +0x28 client slots, +0x2c slot count */
#define MODE_OFF 0x3c
#define SLOTS_OFF 0x28
#define NSLOTS_OFF 0x2c
#define SLOT_SIZE 0xa4                       /* +0 client id (0 = the server), +4 player entity, +8 flags */
#define SLOT_READY 0x80
#define SLOT_JOINED 0x100
#define MODE_SERVER 2
#define MODE_CLIENT 3
#define PLAYER (*(char **)0x1095b30)         /* $player */
#define PEER_NETWORK (*(void **)0x1096148)   /* net::PEER_NETWORK; NULL outside network games */
#define LEVEL_NAME ((const char *)0xfcabc8)  /* the running level's file name */
#define LEVEL_NAME_MAX 0x104
#define GAME_FLAGS (*(volatile unsigned *)0xfcace8)
#define FLAG_LEVEL_RUNNING 0x80000000u       /* (cleared by the server's map change, set when loaded) */
#define MP_RULES (*(char **)0xfcab18)        /* +0x114 respawn points, +0x118 count */
#define SPAWN_STRIDE 0x1c
#define LEVEL_LOADER (*(void **)0x1033980)
#define SCHEDULER ((void *)0xecb7f8)
#define MGR (*(void **)0xebedec)
#define ENT_INST 0x80                        /* entity: its script instance */
#define ENT_NODE 0x230                       /* entity: scene node (position) */
#define DOM_WATCHED 0xf8                     /* domain: the object whose points it tests */
#define INST_NAME 4                          /* script instance: name (String) */
#define INST_NATIVE 0xc                      /* script instance: the C++ object */
#define INST_CLASS 0x10                      /* script instance: class */
#define CLASS_PARENT 8                       /* class: parent class */
#define CLASS_MEMBERS 0x20                   /* class: members, 12 bytes each, +0 symbol */

/* ---- script runtime (see console.c) ---- */
typedef struct { int refs, len, cap; char s[1]; } StrHdr;
typedef StrHdr *String;
typedef struct { int code; String msg; } Status;
typedef struct { void **type; unsigned data; } Variant;
typedef void *ObjPtr;

static void (__cdecl *eng_free)(void *) = (void *)0x7e4700;
static String *(TSCALL *str_ctor)(String *, const char *, int, int) = (void *)0x402490;
static String *(TSCALL *str_empty)(String *) = (void *)0x402a90;
static ObjPtr *(TSCALL *mgr_create_class)(void *, ObjPtr *, ObjPtr) = (void *)0x8fc2d0;
static ObjPtr *(TSCALL *mgr_find_obj)(void *, ObjPtr *, const String *) = (void *)0x8fc720;
static void (TSCALL *cls_release)(ObjPtr *) = (void *)0x8f7f10;
static void (TSCALL *inst_release)(ObjPtr *) = (void *)0x8f8050;
static int *(TSCALL *sym_ctor)(int *, const char *, char) = (void *)0x9c7530;
static const char *(TSCALL *sym_name)(const int *) = (void *)0x9c6ec0;
/* (events take their arguments as an array of POINTERS to values: the dispatcher reads [argv + i*4], 0x90e0c3) */
static Status *(TSCALL *fire_by_name)(ObjPtr *, Status *, int, int, Variant **) = (void *)0x8f9680;
static Variant *(TSCALL *var_ctor)(Variant *) = (void *)0x9c9260;
static void (TSCALL *var_dtor)(Variant *) = (void *)0x9c8690;
static void *(*type_int)(void) = (void *)0x9c8c50;
static void *(*type_bool)(void) = (void *)0x9c8d00;
static void *(*type_float)(void) = (void *)0x9c8e10;
static void *(*type_str)(void) = (void *)0x9c8ec0;
static int (TSCALL *dom_is_belong)(void *, void *) = (void *)0x6c3d60;          /* returns a bool in al */
static void (TSCALL *node_pos)(void *, float *) = (void *)0x94c8e0;
static int (TSCALL *post_delayed)(void *, void *, int, void *, float, int) = (void *)0x947020;
static void (TSCALL *broadcast)(void *, int, void *, int) = (void *)0x948480;   /* in-process, every task */

typedef Status *(TSCALL *parse_t)(ObjPtr *, Status *, const char *, const char *);
typedef Status *(TSCALL *event_t)(void *, Status *, void *, int, int, Variant **);
typedef int (TSCALL *post_t)(void *, void *, int, void *, int);
typedef void (TSCALL *native_t)(void *, int, Variant *, Variant *, void *);
static parse_t orig_parse = (parse_t)0x8f9150;
static event_t orig_event = (event_t)0x903940;
static post_t orig_post = (post_t)0x946fc0;

/* hook sites (the original bytes are checked before patching) */
#define SITE_PARSE 0x644fc4                  /* call 0x8f9150 in the entity's script set-up */
static const DWORD event_sites[3] = {0x8f82bf, 0x8f97c6, 0x905462};   /* calls 0x903940 */
#define SITE_DOMTEST 0x6c2e3d                /* 16 bytes: the count of the watched object's points */
#define DOMTEST_BACK 0x6c2e4d
#define SITE_RELOAD 0x8aa0e3                 /* call 0x946fc0: the reload domain's LOAD_LEVEL */
#define FN_AUTOSAVE 0x6ce030                 /* push -1; push 0xc69168 */
#define FN_FAILMISSION 0x43b550              /* push -1; push 0xc78fe0 */
#define FN_SPLOAD 0x675370                   /* the single-player "load level <name>" (stdcall, String*) */

#define MSG_LOAD_LEVEL 0x3ec
#define MSG_SERVER_MAP 0x43e
#define MSG_LEVEL_ENDING 0x3f6                /* (sent before a level is left, by the round change too) */

#define SUB_OCC 20
#define SUB_EVENT 21
#define SUB_EYE 22
#define PROTO 1
#define MAX_PAYLOAD 250

/* ---- settings and logging ---- */
static int enabled, any_level, fail_restart = 1, verbose;
/* team restart (host) */
static int restart_pending, restore_pending, restore_seen_off, restore_cp;
static int restore_check;                    /* (host) the checkpoint whose restore is checked once more */
static DWORD restore_check_at;
static float restore_check_pos[3];
static DWORD restart_at, restore_at, restore_since;
static char restore_level[0x104];
static char pending_ssl[300];                /* (host) SSL for the next script tick */
static LONG n_logged;
#define LOG_MAX 2000
#define SLOG(...) do { if (InterlockedIncrement(&n_logged) <= LOG_MAX) ts_log(__VA_ARGS__); } while (0)
#define VLOG(...) do { if (verbose) SLOG(__VA_ARGS__); } while (0)

static int game_mode(void)
{
    char *gm = GAME_MODE_OBJ;
    return gm ? *(int *)(gm + MODE_OFF) : 0;
}

/* MODE_SERVER / MODE_CLIENT in a co-op session whose host runs the story for everyone (coopsession.c: network
 * game on a campaign level, COOP_F_STORYSYNC), else 0. Multiplayer maps keep the game's own rules
 * (TS_STORYSYNC=all: every network map, for tests). */
static int net_role(void)
{
    int m = game_mode();
    if (!enabled || (m != MODE_SERVER && m != MODE_CLIENT) || !PEER_NETWORK)
        return 0;
    return any_level || ts_coop_feature(COOP_F_STORYSYNC) ? m : 0;
}

/* The same while a level's scripts are compiled: the session's settings reach a client (and are set on the host)
 * only after the level is built, so a level of a co-op session (network game, campaign level) is prepared
 * whenever this machine has TS_STORYSYNC; whether events are held back, sent and replayed is decided later by
 * net_role(). (A client whose host does not run the story keeps the prepared scripts: its own triggers then run
 * only their presentation; coopsession.c logs the mismatch.) */
static int compile_role(void)
{
    int m = game_mode();
    if (!enabled || (m != MODE_SERVER && m != MODE_CLIENT) || !PEER_NETWORK)
        return 0;
    return any_level || ts_coop_active() ? m : 0;
}

static char *slot_at(int i)
{
    char *gm = GAME_MODE_OBJ;
    return gm && i >= 0 && i < *(int *)(gm + NSLOTS_OFF) ? *(char **)(gm + SLOTS_OFF) + i * SLOT_SIZE : NULL;
}

static int slot_joined(char *s)
{
    return (*(int *)(s + 8) & (SLOT_JOINED | SLOT_READY)) == (SLOT_JOINED | SLOT_READY);
}

static void str_release(String s)
{
    if (s && --s->refs == 0)
        eng_free(s);
}

static ObjPtr addref(ObjPtr p)
{
    if (p)
        InterlockedIncrement((LONG *)p);
    return p;
}

static const char *inst_name(void *inst)
{
    String s = inst ? *(String *)((char *)inst + INST_NAME) : NULL;
    return s ? s->s : "";
}

/* The level's base name ("ts2_city_part02"), without a folder or ".lg". */
static void level_base(const char *in, char *out, int size)
{
    const char *p = in, *slash;
    while ((slash = strpbrk(p, "\\/")) != NULL)
        p = slash + 1;
    snprintf(out, size, "%s", p);
    char *dot = strrchr(out, '.');
    if (dot && _stricmp(dot, ".lg") == 0)
        *dot = 0;
}

static int campaign_name(const char *s)
{
    for (; *s; s++)
        if ((s[0] | 0x20) == 't' && (s[1] | 0x20) == 's' && s[2] == '2' && s[3] == '_')
            return 1;
    return 0;
}

static int sym_of(const char *name)
{
    int s = 0;
    sym_ctor(&s, name, 1);
    return s;
}

/* ---- classification of level object scripts ----------------------------------------------------- */
#define MAX_EV 24
#define EVNAME 28
typedef struct {
    void *cls;
    char name[64];                           /* the entity's name */
    unsigned char n_ev, rewritten, authority, dom_story, retest_logged;
    int ev_sym[MAX_EV];
    unsigned char ev_story[MAX_EV];
    char ev_name[MAX_EV][EVNAME];
} ClassRec;

#define TABLE_SIZE 8192                      /* power of two */
static ClassRec *table;
static int n_rec, n_rewritten;
static char rec_level[LEVEL_NAME_MAX];

static int sym_enter, sym_leave, sym_eye;

static unsigned hash_ptr(void *p)
{
    return ((unsigned)p >> 4) * 2654435761u >> 19 & (TABLE_SIZE - 1);
}

static ClassRec *rec_slot(void *cls, int create)
{
    unsigned h = hash_ptr(cls);
    for (int i = 0; i < TABLE_SIZE; i++, h = (h + 1) & (TABLE_SIZE - 1)) {
        ClassRec *r = &table[h];
        if (r->cls == cls)
            return r;
        if (!r->cls) {
            if (!create)
                return NULL;
            memset(r, 0, sizeof *r);
            r->cls = cls;
            n_rec++;
            return r;
        }
    }
    return NULL;
}

static void table_clear(void)
{
    if (table)
        memset(table, 0, sizeof(ClassRec) * TABLE_SIZE);
    n_rec = n_rewritten = 0;
}

/* The record of an instance's class, when the instance is the level object it was made for. */
static ClassRec *rec_of_inst(void *inst)
{
    if (!table || !inst)
        return NULL;
    void *cls = *(void **)((char *)inst + INST_CLASS);
    ClassRec *r = cls ? rec_slot(cls, 0) : NULL;
    return r && strcmp(r->name, inst_name(inst)) == 0 ? r : NULL;
}

static int rec_event(ClassRec *r, int sym)
{
    for (int i = 0; i < r->n_ev; i++)
        if (r->ev_sym[i] == sym)
            return i;
    return -1;
}

/* Statements that only change what this machine shows its own player. */
static const char *const local_calls[] = {
    "$Light_zones.", "ShakeCamera", "$tc.SetMode(", "ShowTimeHint(", "ShowSAMMsg(", "ShowMsg(", "PlaySound(",
    "SelectHolster(", "SetDeathFallHeight(", "StartFastPhysics", "$effect.", "EnableZone(",
    "$snd", "$music.", NULL};   /* (sound actors and music: each machine plays its own ambience) */

/* Calls only the host may make: on a client they are wrapped in "if false ... end". */
static const char *const host_calls[] = {
    "Create", "SetBHV", "PushBHV", "PopBHV", "SetPar", "SetSenses", "SetFollow", "EndFollow", "SetAim", "EndAim",
    "SetInterest", "EndInterest", "SetTarget", "EndTarget", "ShootNow", "ShootBegin", "ShootEnd", "ShootOnce",
    "StartRoute", "StopRoute", "SetApproach", "EndApproach", "SetNoCombat", "DisableStrike", "EnableTurns",
    "DisableTurns", "SetShootRandAngles", "SetShootRandDist", "SetContourShooting", "TurretKeepShooting",
    "DoAlert", "Exclaim", "SetSpeed", "SetWpnByPlayer", "ToggleFireable", "NavLink", "NavUnLink",
    "ExcludeFromNav", "IncludeToNav", "SetHealthGod", "SetHealth", "SetRegenerationSpeed", "Die",
    "DelayedDieRemove", "Kill", "FailMission", "AutoSave", "SaveGame", "LoadGame", "LoadLevel", "RestartLevel",
    "Reload", "StartUse", "DisableUse", "EnableUse", "SetGOD", NULL};

/* Events every machine runs for its own copy of the level, never sent: set-up, and the world time events
 * (each machine's own world slows/stops; timesync.c keeps the worlds in step). */
static const char *const always_local[] = {"OnInitLevel", "OnInit", "OnStartTStop", "OnStopTStop", "OnStartTSlow",
                                           "OnStopTSlow", "OnStartTReverse", "OnStopTReverse", NULL};

static int in_list(const char *const *list, const char *s, int len)
{
    for (int i = 0; list[i]; i++)
        if ((int)strlen(list[i]) == len && strncmp(list[i], s, len) == 0)
            return 1;
    return 0;
}

/* The never-true condition the client wraps host-only statements in (chosen once by a test compile). */
static const char *wrap_open;

static int is_ident(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* A line without its comment and trailing blanks; returns the length (0 = nothing); *open = '(' minus ')'. */
static int code_of(const char *p, const char *e, char *buf, int cap, int *open)
{
    int n = 0, q = 0;
    *open = 0;
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    for (; p < e && n < cap - 1; p++) {
        if (*p == '"')
            q = !q;
        else if (!q && p[0] == '/' && p + 1 < e && p[1] == '/')
            break;
        else if (!q && *p == '(')
            (*open)++;
        else if (!q && *p == ')')
            (*open)--;
        buf[n++] = *p;
    }
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r'))
        n--;
    buf[n] = 0;
    return n;
}

static int word_is(const char *code, const char *w)
{
    size_t n = strlen(w);
    return strncmp(code, w, n) == 0 && !is_ident(code[n]);
}

/* The method a statement calls: the identifier before its first '(' (empty when none). */
static int callee_of(const char *code, const char **start)
{
    const char *par = strchr(code, '(');
    if (!par)
        return 0;
    const char *b = par;
    while (b > code && is_ident(b[-1]))
        b--;
    *start = b;
    return (int)(par - b);
}

/* Classify `src` into `r`; with `out` (a client), also write the text with the host-only statements wrapped.
 * Returns 0 when the text has a statement over several lines (then it is not rewritten). */
static int classify(const char *src, ClassRec *r, char *out, size_t cap, int *n_wrapped)
{
    int depth = 0, cur = -1, safe = 1;
    size_t o = 0;
    *n_wrapped = 0;
#define EMIT(s, n) do { if (out && o + (n) < cap) { memcpy(out + o, (s), (n)); o += (n); } } while (0)
    for (const char *p = src; *p;) {
        const char *e = p;
        while (*e && *e != '\n')
            e++;
        const char *next = *e ? e + 1 : e;
        char code[512];
        int open, n = code_of(p, e, code, sizeof code, &open), wrap = 0;
        if (n && code[0] != '#') {
            if (depth == 0) {
                if (word_is(code, "override") || word_is(code, "func")) {
                    depth = 1;
                    cur = -1;
                    if (word_is(code, "override")) {
                        const char *s = code + 8;
                        while (*s == ' ' || *s == '\t')
                            s++;
                        int len = 0;
                        while (is_ident(s[len]))
                            len++;
                        if (len && len < EVNAME) {
                            char ev[EVNAME];
                            memcpy(ev, s, len);
                            ev[len] = 0;
                            int sym = sym_of(ev), k = -1;
                            for (int i = 0; i < r->n_ev; i++)
                                if (strcmp(r->ev_name[i], ev) == 0)
                                    k = i;
                            if (k < 0 && r->n_ev < MAX_EV && sym) {
                                k = r->n_ev++;
                                r->ev_sym[k] = sym;
                                r->ev_story[k] = 0;
                                strcpy(r->ev_name[k], ev);
                            }
                            cur = in_list(always_local, ev, len) ? -1 : k;
                        }
                    }
                }
            } else if (word_is(code, "end")) {
                if (--depth == 0)
                    cur = -1;
            } else if (word_is(code, "if") || word_is(code, "foreach")) {
                depth++;
            } else if (!word_is(code, "else") && !word_is(code, "return") && !word_is(code, "break")) {
                /* a statement */
                if (open)
                    safe = 0;
                int local = 0;
                for (int i = 0; local_calls[i]; i++)
                    if (strstr(code, local_calls[i]))
                        local = 1;
                if (cur >= 0 && !local)
                    r->ev_story[cur] = 1;
                const char *c;
                int len = callee_of(code, &c);
                /* campaign.c (TS_CAMPAIGN, not yet run): SetSpeed is the force-field/wind domains' only setter in the
                 * levels; it then runs on clients too (their players are pushed by their own copy of the domain) */
                if (len && in_list(host_calls, c, len)
                    && !campaign_local_call(c, len)) {
                    r->authority = 1;
                    wrap = out != NULL;
                }
            }
        }
        if (wrap) {
            EMIT(wrap_open, strlen(wrap_open));
            EMIT(p, (size_t)(e - p));
            EMIT("\nend\n", 5);
            (*n_wrapped)++;
        } else
            EMIT(p, (size_t)(next - p));
        p = next;
    }
    if (out)
        out[o < cap ? o : cap - 1] = 0;
#undef EMIT
    for (int i = 0; i < r->n_ev; i++)
        if (r->ev_story[i] && (r->ev_sym[i] == sym_enter || r->ev_sym[i] == sym_leave))
            r->dom_story = 1;
    return safe;
}

/* Compile `text` into a throwaway class with the same parent: 1 = it compiles. */
static int compiles(void *cls, const char *text, const char *hint)
{
    ObjPtr tmp = 0;
    Status st;
    void *parent = *(void **)((char *)cls + CLASS_PARENT);
    mgr_create_class(MGR, &tmp, addref(parent));   /* (the callee releases the by-value parent) */
    if (!tmp)
        return 0;
    orig_parse(&tmp, &st, text, hint);
    int ok = st.code == 0;
    if (!ok)
        SLOG("storysync: rewritten script of %s does not compile (%s); kept as it was", hint, st.msg ? st.msg->s : "");
    str_release(st.msg);
    cls_release(&tmp);
    return ok;
}

static void level_changed(void);
static int level_ran;                        /* the level of the table has run (its scripts are compiled) */
static int named_seen;                       /* named script objects while the level ran (a level torn down for a
                                                rebuild drops them; a client keeps its "running" flag) */
static int named_objects(void)
{
    return MGR ? *(int *)((char *)MGR + 0x18) : 0;   /* (script manager: +0x14 named objects, +0x18 count) */
}

/* The first wrapper that the engine compiles, or "" (then client scripts stay as they are). */
static const char *pick_wrapper(void)
{
    static const char *const cands[] = {"if false\n", "if 0\n", "if 1 == 0\n", NULL};
    for (int i = 0; cands[i]; i++) {
        char text[160];
        ObjPtr tmp = 0;
        Status st;
        snprintf(text, sizeof text, "func __ss_probe : *\n%sreturn 1\nend\nreturn 0\nend\n", cands[i]);
        mgr_create_class(MGR, &tmp, NULL);
        if (!tmp)
            break;
        orig_parse(&tmp, &st, text, "");
        int ok = st.code == 0;
        str_release(st.msg);
        cls_release(&tmp);
        if (ok) {
            SLOG("storysync: client scripts wrap host-only statements in \"%.*s\"", (int)strlen(cands[i]) - 1, cands[i]);
            return cands[i];
        }
    }
    SLOG("storysync: no wrapper compiles; client scripts stay as they are (no replays of host-only scripts)");
    return "";
}

static Status *TSCALL hook_parse(ObjPtr *clsp, Status *out, const char *src, const char *hint)
{
    int role = compile_role();
    if (!role || !table || !src || !clsp || !*clsp)
        return orig_parse(clsp, out, src, hint);
    if (!sym_enter) {
        sym_enter = sym_of("OnEnter");
        sym_leave = sym_of("OnLeave");
        sym_eye = sym_of("OnEye");
    }
    /* (a new level, or the same level built again after it ran: a team restart) */
    if (strncmp(rec_level, LEVEL_NAME, sizeof rec_level) != 0 || n_rec > TABLE_SIZE * 3 / 4
        || (level_ran && (!(GAME_FLAGS & FLAG_LEVEL_RUNNING) || named_objects() < named_seen / 2))) {
        snprintf(rec_level, sizeof rec_level, "%s", LEVEL_NAME);
        level_ran = 0;
        level_changed();
    }
    if (restore_pending)
        restore_seen_off = 1;   /* (the restarted level is being built) */
    void *cls = *clsp;
    ClassRec *r = rec_slot(cls, 1);
    if (!r)
        return orig_parse(clsp, out, src, hint);
    memset(r, 0, sizeof *r);
    r->cls = cls;
    snprintf(r->name, sizeof r->name, "%s", hint ? hint : "");
    if (role != MODE_CLIENT) {
        int w;
        classify(src, r, NULL, 0, &w);
        return orig_parse(clsp, out, src, hint);
    }
    if (!wrap_open)
        wrap_open = pick_wrapper();
    if (!wrap_open[0]) {
        int w;
        classify(src, r, NULL, 0, &w);
        return orig_parse(clsp, out, src, hint);
    }
    size_t len = strlen(src), lines = 1;
    for (const char *c = src; *c; c++)
        lines += *c == '\n';
    size_t cap = len + lines * 16 + 64;
    char *text = malloc(cap);
    int w = 0, safe = text ? classify(src, r, text, cap, &w) : 0;
    if (text && w && safe && compiles(cls, text, hint ? hint : "")) {
        orig_parse(clsp, out, text, hint);
        if (out->code == 0) {
            r->rewritten = 1;
            n_rewritten++;
            VLOG("storysync: %s: %d host-only statement(s) wrapped", r->name, w);
        }
    } else {
        if (w && !safe)
            SLOG("storysync: %s: a statement over several lines, script kept as it was", r->name);
        orig_parse(clsp, out, src, hint);
    }
    free(text);
    return out;
}

/* ---- occupancy of story domains on the host ---------------------------------------------------------- */
#define OCC_MAX 64
static struct { char name[64]; int client; } occ[OCC_MAX];
static int n_occ;

static void occ_set(const char *name, int client, int inside)
{
    for (int i = 0; i < n_occ; i++)
        if (occ[i].client == client && strcmp(occ[i].name, name) == 0) {
            if (!inside)
                occ[i] = occ[--n_occ];
            return;
        }
    if (inside && n_occ < OCC_MAX) {
        snprintf(occ[n_occ].name, sizeof occ[n_occ].name, "%s", name);
        occ[n_occ++].client = client;
    }
}

static int occ_has(const char *name)
{
    for (int i = 0; i < n_occ; i++)
        if (strcmp(occ[i].name, name) == 0)
            return 1;
    return 0;
}

static int ent_pos(char *ent, float *pos)
{
    if (!ent || !*(void **)(ent + ENT_NODE))
        return 0;
    node_pos(*(void **)(ent + ENT_NODE), pos);
    return 1;
}

/* Domain classes keep the engine's domain test as their update (vtable +0x134). */
#define DOM_UPDATE 0x6c2dd0
/* The object a player trigger domain watches (+0xf8): one object shared by the level's domains (class vtable
 * 0xdad650), not the $player entity. Learnt on the host as the one most story domains watch. */
static void *local_watch;
static int is_domain(char *ent)
{
    return ent && *(void ***)ent && (*(void ***)ent)[0x134 / 4] == (void *)DOM_UPDATE;
}

/* A remote player inside this story domain (host). */
static char *remote_inside(char *dom)
{
    if (!is_domain(dom))
        return NULL;
    void *inst = *(void **)(dom + ENT_INST);
    ClassRec *r = rec_of_inst(inst);
    if (!r || !r->dom_story)
        return NULL;
    for (int i = 0;; i++) {
        char *s = slot_at(i);
        if (!s)
            break;
        char *ent = *(char **)(s + 4);
        if (*(int *)s != 0 && ent && slot_joined(s) && (dom_is_belong(dom, ent) & 0xff))
            return ent;
    }
    return n_occ && occ_has(inst_name(inst)) ? PLAYER : NULL;   /* (a client's report) */
}

/* Called from the domain test (0x6c2e3d) instead of "count of the watched object's points": returns the count,
 * or more than `idx` (= "a point is inside") when only a remote player is inside a story domain. */
__attribute__((used)) int ss_dom_count(char *dom, int idx) __asm__("ss_dom_count");
__attribute__((used)) int ss_dom_count(char *dom, int idx)
{
    void *w = *(void **)(dom + DOM_WATCHED);
    int n = ((int (TSCALL *)(void *))(*(void ***)w)[0x11c / 4])(w);
    /* campaign.c (TS_CAMPAIGN, not yet run): per-level co-op rules ("all players" gates, host-only and death
     * domains) decide first; -1 = no rule for this domain. (A result > idx = inside, <= idx = outside.) */
    if (w == local_watch && net_role() == MODE_SERVER) {
        int v = campaign_dom_verdict(dom, idx < n);
        if (v >= 0)
            return v ? (idx < n ? n : idx + 1) : idx;
    }
    if (idx < n || w != local_watch || net_role() != MODE_SERVER)
        return n;
    return remote_inside(dom) ? idx + 1 : n;
}

/* The engine tests a domain only when its watched object (the host's own player) moves, so a client inside a
 * story domain would set it off only once the host's player moves. The host therefore re-runs the test of every
 * story domain that a client's body or report is inside, or that still counts as entered, 4 times a second. */
#define DOM_STATE 0xe8                       /* domain: bit 0 = entered */
#define DOM_LOCKS 0x88                       /* lock count (> 0: locked, no enter/leave) */
static void retest_story_domains(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 250 || !table)
        return;
    last = now;
    int clients = 0;
    for (int i = 0; slot_at(i); i++)
        if (*(int *)slot_at(i) != 0 && slot_joined(slot_at(i)))
            clients++;
    if (!clients)
        return;
    static int n_diag;
    int n_story = 0, n_found = 0, n_dom = 0, n_watch = 0, n_in = 0, votes = 0;
    void *watched = NULL, *cand = NULL;
    for (int i = 0; i < TABLE_SIZE; i++) {
        ClassRec *r = &table[i];
        if (!r->cls || !r->dom_story || !r->name[0])
            continue;
        n_story++;
        String nm = 0;
        ObjPtr inst = 0;
        str_ctor(&nm, r->name, -1, 0);
        mgr_find_obj(MGR, &inst, &nm);
        str_release(nm);
        if (!inst)
            continue;
        char *ent = *(char **)((char *)inst + INST_NATIVE);
        int found = rec_of_inst(inst) == r, dom = found && is_domain(ent);
        n_found += found;
        n_dom += dom;
        if (dom) {
            watched = *(void **)(ent + DOM_WATCHED);
            n_watch += watched == local_watch;
            if (!votes)
                cand = watched, votes = 1;
            else
                votes += cand == watched ? 1 : -1;
        }
        int in = dom && remote_inside(ent) != NULL;
        n_in += in;
        if (dom && local_watch && watched == local_watch && ((*(unsigned *)(ent + DOM_STATE) & 1) || in)) {
            unsigned before = *(unsigned *)(ent + DOM_STATE);
            ((void (TSCALL *)(void *))(*(void ***)ent)[0x134 / 4])(ent);
            if (*(unsigned *)(ent + DOM_STATE) != before || !(r->retest_logged++))
                VLOG("storysync: %s tested for a client: state %x -> %x, locks %d", r->name, before,
                     *(unsigned *)(ent + DOM_STATE), *(int *)(ent + DOM_LOCKS));
        }
        inst_release(&inst);
    }
    if (cand && cand != local_watch && votes > 0) {
        VLOG("storysync: story domains watch %p (vtable %p)", cand, *(void **)cand);
        local_watch = cand;
    }
    if (n_occ && n_diag < 4) {
        n_diag++;
        VLOG("storysync: re-test pass: %d client(s), %d story domain records, %d found, %d domains, %d watch %p,"
             " %d with a client inside, %d report(s)", clients, n_story, n_found, n_dom, n_watch, local_watch, n_in,
             n_occ);
    }
}

extern void ss_dom_stub(void) __asm__("ss_dom_stub");
__asm__(".text\n"
        ".globl ss_dom_stub\n"
        "ss_dom_stub:\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  call ss_dom_count\n"
        "  addl $8, %esp\n"
        "  ret\n");

/* ---- events: suppress on clients, send from the host, replay on clients ------------------------------- */
static void *replay_inst;
static int replay_sym, replay_armed;
static char *cur_entity;                     /* (host) the entity whose event runs now */
#define EYE_MAX 64
static char eye_done[EYE_MAX][64];
static int n_eye_done;

static void put_u8(unsigned char *b, int *n, int v)
{
    if (*n < MAX_PAYLOAD)
        b[(*n)++] = (unsigned char)v;
}

static void put_str(unsigned char *b, int *n, const char *s)
{
    int len = (int)strlen(s);
    if (len > 80)
        len = 80;
    put_u8(b, n, len);
    if (*n + len <= MAX_PAYLOAD) {
        memcpy(b + *n, s, len);
        *n += len;
    }
}

static void send_event(void *inst, int sym, int argc, Variant **argv)
{
    const char *name = inst_name(inst), *ev = sym_name(&sym);
    if (!name[0] || !ev)
        return;
    unsigned char b[MAX_PAYLOAD];
    int n = 0;
    if (argc < 0 || argc > 8 || (argc && !argv))
        argc = 0;
    put_u8(b, &n, PROTO);
    put_u8(b, &n, argc);
    put_str(b, &n, name);
    put_str(b, &n, ev);
    for (int i = 0; i < argc; i++) {
        Variant *a = argv[i];
        void *t = a ? a->type : NULL;
        if (t == type_int() || t == type_float()) {
            put_u8(b, &n, t == type_int() ? 1 : 3);
            for (int k = 0; k < 4; k++)
                put_u8(b, &n, a->data >> (8 * k));
        } else if (t == type_bool()) {
            put_u8(b, &n, 2);
            put_u8(b, &n, a->data & 0xff ? 1 : 0);
        } else if (t == type_str()) {
            String s = (String)a->data;
            put_u8(b, &n, 4);
            put_str(b, &n, s ? s->s : "");
        } else
            put_u8(b, &n, 0);   /* (objects and anything else: null) */
    }
    if (n >= MAX_PAYLOAD) {
        SLOG("storysync: event %s.%s too long to send", name, ev);
        return;
    }
    int sent = coopnet_send(-1, SUB_EVENT, b, n);
    VLOG("storysync: sent %s.%s (%d args) to %d client(s)", name, ev, argc, sent);
}

static void send_occ(const char *name, int inside, unsigned short sub)
{
    unsigned char b[MAX_PAYLOAD];
    int n = 0;
    put_u8(b, &n, PROTO);
    put_u8(b, &n, inside);
    int len = (int)strlen(name);
    if (len > 200)
        return;
    memcpy(b + n, name, len);
    n += len;
    coopnet_send(-1, sub, b, n);
    VLOG("storysync: told the host %s %s %s", name, sub == SUB_EYE ? "OnEye" : "occupancy", inside ? "on" : "off");
}

static Status *ok_status(Status *out)
{
    out->code = 0;
    out->msg = NULL;
    str_empty(&out->msg);
    return out;
}

static Status *TSCALL hook_event(void *cls, Status *out, void *inst, int idx, int argc, Variant **argv)
{
    int role = net_role();
    ClassRec *r = role && inst ? rec_of_inst(inst) : NULL;
    if (!r)
        return orig_event(cls, out, inst, idx, argc, argv);
    if (idx < 0 || idx >= *(int *)((char *)cls + CLASS_MEMBERS + 4))
        return orig_event(cls, out, inst, idx, argc, argv);
    int sym = *(int *)(*(char **)((char *)cls + CLASS_MEMBERS) + idx * 12);
    int k = rec_event(r, sym);
    if (k < 0 || !r->ev_story[k])
        return orig_event(cls, out, inst, idx, argc, argv);   /* (no handler here, or a local one) */
    if (role == MODE_CLIENT) {
        if (replay_armed && inst == replay_inst && sym == replay_sym) {
            replay_armed = 0;
            return orig_event(cls, out, inst, idx, argc, argv);
        }
        /* a story event this machine detected itself: the host decides */
        if ((sym == sym_enter || sym == sym_leave) && is_domain(*(char **)((char *)inst + INST_NATIVE)))
            send_occ(r->name, sym == sym_enter, SUB_OCC);
        else if (sym == sym_eye)
            send_occ(r->name, argc > 0 && argv && argv[0] && (argv[0]->data & 0xff), SUB_EYE);
        VLOG("storysync: %s.%s held back (host decides)", r->name, r->ev_name[k]);
        return ok_status(out);
    }
    /* host: a look trigger fired by two players runs once */
    if (sym == sym_eye && argc > 0 && argv && argv[0] && (argv[0]->data & 0xff)) {
        for (int i = 0; i < n_eye_done; i++)
            if (strcmp(eye_done[i], r->name) == 0)
                return ok_status(out);
        if (n_eye_done < EYE_MAX)
            snprintf(eye_done[n_eye_done++], sizeof eye_done[0], "%s", r->name);
    }
    send_event(inst, sym, argc, argv);   /* (before it runs: nested events keep their order) */
    char *prev = cur_entity;
    cur_entity = *(char **)((char *)inst + INST_NATIVE);
    Status *res = orig_event(cls, out, inst, idx, argc, argv);
    cur_entity = prev;
    /* campaign.c (TS_CAMPAIGN, not yet run): seats and rides of the host's story events */
    campaign_story_event(r->name, r->ev_name[k], argc > 0 && argv && argv[0] && argv[0]->type == type_str()
                         && argv[0]->data ? ((String)argv[0]->data)->s : "");
    return res;
}

/* Run an event of a named object here. replay: a client running the host's event. -1 = no such object yet. */
typedef struct { unsigned char type; int i; char s[84]; } Arg;

static int fire_named(const char *obj, const char *ev, int argc, const Arg *args, int replay)
{
    String nm = 0;
    ObjPtr inst = 0;
    str_ctor(&nm, obj, -1, 0);
    mgr_find_obj(MGR, &inst, &nm);
    str_release(nm);
    if (!inst)
        return -1;
    int sym = sym_of(ev), rc = 0;
    ClassRec *r = rec_of_inst(inst);
    if (!sym)
        SLOG("storysync: unknown event %s", ev);
    else if (replay && r && r->authority && !r->rewritten)
        SLOG("storysync: %s.%s not replayed (its script could not be prepared for a client)", obj, ev);
    else {
        Variant av[8], *avp[8];
        for (int i = 0; i < argc; i++) {
            avp[i] = &av[i];
            var_ctor(&av[i]);
            switch (args[i].type) {
            case 1: av[i].type = type_int(); av[i].data = (unsigned)args[i].i; break;
            case 2: av[i].type = type_bool(); av[i].data = args[i].i ? 1 : 0; break;
            case 3: av[i].type = type_float(); av[i].data = (unsigned)args[i].i; break;
            case 4: av[i].type = type_str(); av[i].data = 0; str_ctor((String *)&av[i].data, args[i].s, -1, 0); break;
            default: break;
            }
        }
        Status st;
        if (replay) {
            replay_inst = inst;
            replay_sym = sym;
            replay_armed = 1;
        }
        fire_by_name(&inst, &st, sym, argc, avp);   /* (never NULL) */
        replay_armed = 0;
        if (st.code)
            SLOG("storysync: %s.%s: %s", obj, ev, st.msg ? st.msg->s : "error");
        else
            VLOG("storysync: ran %s.%s%s", obj, ev, replay ? " (host's)" : "");
        str_release(st.msg);
        for (int i = 0; i < argc; i++)
            var_dtor(&av[i]);
        rc = 1;
    }
    inst_release(&inst);
    return rc;
}

/* ---- replays waiting for the script tick ---- */
#define QMAX 128
typedef struct { char obj[84], ev[EVNAME]; int argc, replay; Arg args[8]; DWORD until; } Replay;
static Replay queue[QMAX];
static int q_head, q_count;

static int get_str(const unsigned char *b, int len, int *n, char *out, int size)
{
    if (*n >= len)
        return 0;
    int l = b[(*n)++];
    if (*n + l > len || l >= size)
        return 0;
    memcpy(out, b + *n, l);
    out[l] = 0;
    *n += l;
    return 1;
}

static void got_event(const unsigned char *b, int len)
{
    if (len < 2 || b[0] != PROTO || q_count >= QMAX) {
        SLOG("storysync: replay dropped (%s)", q_count >= QMAX ? "queue full" : "bad message");
        return;
    }
    Replay *q = &queue[(q_head + q_count) % QMAX];
    int n = 2;
    q->argc = b[1] > 8 ? 8 : b[1];
    if (!get_str(b, len, &n, q->obj, sizeof q->obj) || !get_str(b, len, &n, q->ev, sizeof q->ev))
        return;
    for (int i = 0; i < q->argc; i++) {
        Arg *a = &q->args[i];
        if (n >= len)
            return;
        a->type = b[n++];
        a->i = 0;
        a->s[0] = 0;
        if (a->type == 1 || a->type == 3) {
            if (n + 4 > len)
                return;
            memcpy(&a->i, b + n, 4);
            n += 4;
        } else if (a->type == 2) {
            if (n >= len)
                return;
            a->i = b[n++];
        } else if (a->type == 4) {
            if (!get_str(b, len, &n, a->s, sizeof a->s))
                return;
        }
    }
    q->replay = 1;
    q->until = GetTickCount() + 5000;   /* (an object the host just created may arrive a moment later) */
    q_count++;
}

/* (host) a client's look trigger: run it in the next script tick */
static void got_eye(const char *name, int on)
{
    if (q_count >= QMAX)
        return;
    Replay *q = &queue[(q_head + q_count) % QMAX];
    snprintf(q->obj, sizeof q->obj, "%s", name);
    strcpy(q->ev, "OnEye");
    q->argc = 1;
    q->replay = 0;
    q->args[0].type = 2;
    q->args[0].i = on;
    q->args[0].s[0] = 0;
    q->until = GetTickCount();
    q_count++;
}

static void run_queue(void)
{
    int n = q_count;
    /* (a client replays only once its own player exists: the handlers' $player calls, e.g. a checkpoint's restore
     * teleport after a team restart, went nowhere while the restarted level was still spawning it) */
    if (n && !PLAYER && game_mode() == MODE_CLIENT && (int)(GetTickCount() - queue[q_head].until) < 25000)
        return;
    for (int i = 0; i < n && q_count; i++) {
        Replay *q = &queue[q_head];
        int rc = fire_named(q->obj, q->ev, q->argc, q->args, q->replay);
        if (rc < 0 && (int)(GetTickCount() - q->until) < 0)
            break;   /* (keep the order: wait for this one) */
        if (rc < 0)
            SLOG("storysync: replay %s.%s dropped: no such object here", q->obj, q->ev);
        q_head = (q_head + 1) % QMAX;
        q_count--;
    }
}

/* ---- messages ---- */
static void on_message(int peer, unsigned short sub, const unsigned char *data, int len)
{
    int role = net_role();
    if (sub == SUB_EVENT) {   /* (the host sends them only when it runs the story: also before its settings arrived) */
        if (compile_role() == MODE_CLIENT)
            got_event(data, len);
        return;
    }
    if (role != MODE_SERVER || len < 3 || data[0] != PROTO)
        return;
    char name[208];
    int l = len - 2 < (int)sizeof name - 1 ? len - 2 : (int)sizeof name - 1;
    memcpy(name, data + 2, l);
    name[l] = 0;
    if (sub == SUB_OCC) {
        occ_set(name, peer, data[1]);
        VLOG("storysync: client %d %s %s", peer, data[1] ? "in" : "out of", name);
    } else if (sub == SUB_EYE)
        got_eye(name, data[1]);
}

/* ---- level changes through the server --------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct { unsigned char a, b; unsigned short size; char name[260]; int kind, x, y; } MapMsg;   /* 0x114 */
#pragma pack(pop)
static MapMsg map_msg;
static char pending_level[LEVEL_NAME_MAX];

/* Asked from inside the world's update (a reload domain) or a script call: done in the next script tick. */
static void server_change_level(const char *level)
{
    level_base(level, pending_level, sizeof pending_level);
    SLOG("storysync: level change to %s requested", pending_level);
}

static void do_change_level(void)
{
    char base[LEVEL_NAME_MAX];
    snprintf(base, sizeof base, "%s", pending_level);
    pending_level[0] = 0;
    memset(&map_msg, 0, sizeof map_msg);
    map_msg.b = 0x40;
    map_msg.size = sizeof map_msg;
    snprintf(map_msg.name, sizeof map_msg.name, "%s", base);
    map_msg.kind = 3;   /* (as the match rounds' change, 0x606960, which also sends 0x3f6 first) */
    broadcast(SCHEDULER, MSG_LEVEL_ENDING, NULL, 0);
    post_delayed(SCHEDULER, LEVEL_LOADER, MSG_SERVER_MAP, &map_msg, 0.5f, 0);
    /* (a host started later continues here; run in the next script tick) */
    snprintf(pending_ssl, sizeof pending_ssl, "$ui.SetUserVar(\"ServerLocation\", \"%s\")", base);
    SLOG("storysync: server level change to %s for everybody", base);
}

static int TSCALL hook_reload_post(void *sched, void *target, int msg, void *data, int flags)
{
    if (msg == MSG_LOAD_LEVEL && data && net_role() == MODE_SERVER) {
        server_change_level((const char *)data + 4);
        return 1;
    }
    return orig_post(sched, target, msg, data, flags);
}

extern void __stdcall ss_spload_tramp(String *) __asm__("ss_spload_tramp");
__asm__(".text\n"
        ".globl ss_spload_tramp\n"
        "ss_spload_tramp:\n"
        "  pushl %ecx\n"
        "  pushl %ebx\n"
        "  xorl %ebx, %ebx\n"
        "  testb $1, 0x10d545c\n"
        "  pushl $0x67537b\n"            /* (push/ret keep the flags of the test) */
        "  ret\n");

static void __stdcall hook_spload(String *name)
{
    const char *s = name && *name ? (*name)->s : "";
    int role = net_role();
    if (role && campaign_name(s)) {
        if (role == MODE_SERVER) {
            server_change_level(s);
            return;
        }
        SLOG("storysync: client level load %s refused (the host changes levels)", s);
        return;
    }
    ss_spload_tramp(name);
}

/* ---- checkpoints, respawn points, team restart ---------------------------------------------------------- */
static int last_cp;
static float cp_pos[3];
static int cp_pos_set;

static int named_pos(const char *name, float *pos)
{
    String nm = 0;
    ObjPtr inst = 0;
    str_ctor(&nm, name, -1, 0);
    mgr_find_obj(MGR, &inst, &nm);
    str_release(nm);
    int ok = inst && ent_pos(*(char **)((char *)inst + INST_NATIVE), pos);
    if (inst)
        inst_release(&inst);
    return ok;
}

static int set_spawns(const float *pos)
{
    char *mp = MP_RULES;
    if (!mp)
        return 0;
    char *arr = *(char **)(mp + 0x114);
    int n = *(int *)(mp + 0x118);
    for (int i = 0; arr && i < n; i++) {
        float *p = (float *)(arr + i * SPAWN_STRIDE);
        p[0] = pos[0];
        p[1] = pos[1] + 0.5f;
        p[2] = pos[2];
    }
    return n;
}

static void checkpoint(int id)
{
    float pos[3];
    char cpn[32];
    const char *from = "the host's player";
    if (id < last_cp) {   /* (a restarted level's own early checkpoint must not undo the restore) */
        SLOG("storysync: checkpoint %d ignored (the session is at checkpoint %d)", id, last_cp);
        return;
    }
    snprintf(cpn, sizeof cpn, "Check_Point_%d", id);
    if (named_pos(cpn, pos))
        from = cpn;
    else {
        char *who = NULL;
        if (is_domain(cur_entity)) {
            if (PLAYER && (dom_is_belong(cur_entity, PLAYER) & 0xff))
                who = PLAYER;
            else
                who = remote_inside(cur_entity);
        }
        if (!(who && ent_pos(who, pos)) && !ent_pos(PLAYER, pos))
            return;
        if (who && who != PLAYER)
            from = "the client who reached it";
    }
    last_cp = id;
    memcpy(cp_pos, pos, sizeof pos);
    cp_pos_set = 1;
    int n = set_spawns(pos);
    SLOG("storysync: checkpoint %d at %.1f %.1f %.1f (%s), %d respawn point(s) moved", id, pos[0], pos[1], pos[2],
         from, n);
}

extern void TSCALL ss_autosave_tramp(void *, int, Variant *, Variant *, void *) __asm__("ss_autosave_tramp");
extern void TSCALL ss_fail_tramp(void *, int, Variant *, Variant *, void *) __asm__("ss_fail_tramp");
__asm__(".text\n"
        ".globl ss_autosave_tramp\n"
        "ss_autosave_tramp:\n"
        "  pushl $-1\n"
        "  pushl $0xc69168\n"
        "  pushl $0x6ce037\n"
        "  ret\n"
        ".globl ss_fail_tramp\n"
        "ss_fail_tramp:\n"
        "  pushl $-1\n"
        "  pushl $0xc78fe0\n"
        "  pushl $0x43b557\n"
        "  ret\n");

static void TSCALL hook_autosave(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (net_role() == MODE_SERVER && argc > 0 && argv && argv[0].type == type_int())
        checkpoint((int)argv[0].data);
    ss_autosave_tramp(obj, argc, argv, res, self);
}

static void TSCALL hook_fail(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (net_role() == MODE_SERVER && fail_restart && obj == PLAYER) {
        const char *why = argc > 0 && argv && argv[0].type == type_str() && argv[0].data ? ((String)argv[0].data)->s : "";
        if (!restart_pending && !restore_pending) {
            restart_pending = 1;
            restart_at = GetTickCount() + 3000;
            SLOG("storysync: mission failed (%s): team restart%s", why,
                 last_cp ? " from the last checkpoint" : " of the level");
        }
        if (self)
            inst_release(&self);   /* (the native would have released its by-value caller) */
        return;
    }
    ss_fail_tramp(obj, argc, argv, res, self);
}

static void team_restart(void)
{
    level_base(LEVEL_NAME, restore_level, sizeof restore_level);
    restore_cp = last_cp;
    restore_pending = 1;
    restore_seen_off = 0;
    restore_at = 0;
    restore_since = GetTickCount();
    n_occ = 0;
    n_eye_done = 0;
    server_change_level(restore_level);
}

/* After the restarted level runs: back to the checkpoint where the level has its restore object. */
static void restore_step(void)
{
    int running = (GAME_FLAGS & FLAG_LEVEL_RUNNING) != 0;
    if (!running) {
        restore_seen_off = 1;
        return;
    }
    if (!restore_seen_off)
        return;
    if (!restore_at) {   /* (2 s after the host's player exists again: its restore teleport needs it) */
        if (!PLAYER)
            return;
        restore_at = GetTickCount() + 2000;
        return;
    }
    if ((int)(GetTickCount() - restore_at) < 0)
        return;
    restore_pending = 0;
    char cpn[32], ssl[96], res[200];
    float pos[3];
    snprintf(cpn, sizeof cpn, "Check_Point_%d", restore_cp);
    if (restore_cp && named_pos(cpn, pos)) {
        last_cp = restore_cp;
        set_spawns(pos);
        snprintf(ssl, sizeof ssl, "$%s.Unlock()", cpn);
        ts_script_run(ssl, res, sizeof res);
        SLOG("storysync: restarted at checkpoint %d (%s -> %s)", restore_cp, ssl, res);
        restore_check = restore_cp;
        restore_check_at = GetTickCount() + 4000;
        memcpy(restore_check_pos, pos, sizeof pos);
    } else
        SLOG("storysync: restarted %s from its start (no restore object for checkpoint %d)", restore_level,
             restore_cp);
}

/* 4 s after the restore: a host's player that is not near the checkpoint (it spawned at the level start after the
 * restore script had run, seen with a client in the session) is put there, and the respawn points again. */
static void restore_verify(void)
{
    if (!restore_check || (int)(GetTickCount() - restore_check_at) < 0)
        return;
    int cp = restore_check;
    restore_check = 0;
    float pos[3];
    if (!ent_pos(PLAYER, pos))
        return;
    float dx = pos[0] - restore_check_pos[0], dz = pos[2] - restore_check_pos[2];
    if (dx * dx + dz * dz < 100.f)
        return;
    char *node = *(char **)(PLAYER + ENT_NODE);
    float at[3] = {restore_check_pos[0], restore_check_pos[1] + 0.5f, restore_check_pos[2]};
    float dir[3] = {*(float *)(node + 0x58), *(float *)(node + 0x5c), *(float *)(node + 0x60)};
    ((void (TSCALL *)(void *, float *, float *, int))(*(void ***)PLAYER)[0x210 / 4])(PLAYER, at, dir, 1);
    set_spawns(restore_check_pos);
    SLOG("storysync: the host's player was %.0f m from checkpoint %d after the restore: put there", sqrtf(dx * dx + dz * dz),
         cp);
}

/* ---- per level ---- */
static void level_changed(void)
{
    table_clear();
    if (!restore_pending)
        restore_check = 0;
    n_occ = 0;
    n_eye_done = 0;
    q_head = q_count = 0;
    if (!restore_pending) {
        last_cp = 0;
        cp_pos_set = 0;
    }
}

static void frame(float dt)
{
    (void)dt;
    /* (a client never sets the "level running" flag: there its player stands for it) */
    if ((GAME_FLAGS & FLAG_LEVEL_RUNNING) || (game_mode() == MODE_CLIENT && PLAYER)) {
        level_ran = 1;
        named_seen = named_objects();
    }
    int role = net_role();
    if (!role) {
        if (q_count && compile_role() == MODE_CLIENT)   /* (replays that came before the host's settings) */
            run_queue();
        return;
    }
    if (q_count)
        run_queue();
    if (role == MODE_CLIENT)
        return;
    retest_story_domains();
    if (restart_pending && (int)(GetTickCount() - restart_at) >= 0) {
        restart_pending = 0;
        team_restart();
    }
    if (restore_pending && GetTickCount() - restore_since > 180000) {
        restore_pending = 0;
        SLOG("storysync: the restarted level did not come up in 3 minutes; checkpoint restore dropped");
    }
    if (restore_pending)
        restore_step();
    restore_verify();
    if (pending_level[0])
        do_change_level();
    if (pending_ssl[0]) {
        char res[200];
        ts_script_run(pending_ssl, res, sizeof res);
        VLOG("storysync: %s -> %s", pending_ssl, res);
        pending_ssl[0] = 0;
    }
    static DWORD last_purge;
    if (n_occ && GetTickCount() - last_purge > 1000) {   /* reports of clients that left */
        last_purge = GetTickCount();
        for (int i = n_occ - 1; i >= 0; i--) {
            int found = 0;
            for (int k = 0;; k++) {
                char *s = slot_at(k);
                if (!s)
                    break;
                if (*(int *)s == occ[i].client && slot_joined(s))
                    found = 1;
            }
            if (!found)
                occ[i] = occ[--n_occ];
        }
    }
}

/* ---- test console: "!ss ..." ---- */
int storysync_console(const char *text, char *out, int size)
{
    if (strncmp(text, "!ss", 3) != 0)
        return 0;
    char cmd[32] = "", a1[128] = "", a2[64] = "";
    int a3 = -1;
    sscanf(text + 3, "%31s %127s %63s %d", cmd, a1, a2, &a3);
    int role = net_role();
    if (!strcmp(cmd, "state") || !cmd[0]) {
        snprintf(out, size, "storysync %s, role %d, level %s, classes %d (rewritten %d), occupancy %d, replays waiting %d,"
                 " checkpoint %d%s, restart %d/%d", enabled ? "on" : "off", role, LEVEL_NAME, n_rec, n_rewritten, n_occ,
                 q_count, last_cp, cp_pos_set ? " (respawn moved)" : "", restart_pending, restore_pending);
        return 1;
    }
    if (!strcmp(cmd, "class")) {
        String nm = 0;
        ObjPtr inst = 0;
        str_ctor(&nm, a1, -1, 0);
        mgr_find_obj(MGR, &inst, &nm);
        str_release(nm);
        ClassRec *r = rec_of_inst(inst);
        int n = 0;
        if (!r)
            n = snprintf(out, size, "%s: no record", a1);
        else {
            n = snprintf(out, size, "%s: rewritten %d, host-only calls %d, story domain %d:", r->name, r->rewritten,
                         r->authority, r->dom_story);
            for (int i = 0; i < r->n_ev && n < size - 40; i++)
                n += snprintf(out + n, size - n, " %s=%s", r->ev_name[i], r->ev_story[i] ? "story" : "local");
            char *ent = *(char **)((char *)inst + INST_NATIVE);
            if (is_domain(ent) && n < size - 60)
                snprintf(out + n, size - n, "; domain state %x, locks %d", *(unsigned *)(ent + 0xe8),
                         *(int *)(ent + 0x88));
        }
        if (inst)
            inst_release(&inst);
        return 1;
    }
    if (!strcmp(cmd, "find")) {   /* "!ss find <event>": the level objects with a handler of that event */
        int n = snprintf(out, size, "%s:", a1), k = 0, sym = sym_of(a1);
        for (int i = 0; table && i < TABLE_SIZE && n < size - 80; i++) {
            ClassRec *r = &table[i];
            if (r->cls && r->name[0] && rec_event(r, sym) >= 0) {
                n += snprintf(out + n, size - n, " %s", r->name);
                k++;
            }
        }
        snprintf(out + n, size - n, " (%d)", k);
        return 1;
    }
    if (!strcmp(cmd, "enter") || !strcmp(cmd, "leave")) {
        if (role != MODE_CLIENT)
            return snprintf(out, size, "client only"), 1;
        send_occ(a1, cmd[0] == 'e', SUB_OCC);
        return snprintf(out, size, "told the host: %s %s", cmd, a1), 1;
    }
    if (!strcmp(cmd, "fire")) {   /* "!ss fire <object> <event> [0|1]": with a third value, one bool argument */
        Arg arg = {2, a3 ? 1 : 0, ""};
        int rc = fire_named(a1, a2, a3 >= 0 ? 1 : 0, &arg, 0);
        return snprintf(out, size, rc < 0 ? "no object %s" : "ran %s.%s", a1, a2), 1;
    }
    if (!strcmp(cmd, "goto")) {   /* "!ss goto <domain>": this machine's player into the middle of a domain (lab) */
        String nm = 0;
        ObjPtr inst = 0;
        str_ctor(&nm, a1, -1, 0);
        mgr_find_obj(MGR, &inst, &nm);
        str_release(nm);
        char *dom = inst ? *(char **)((char *)inst + INST_NATIVE) : NULL;
        if (inst)
            inst_release(&inst);
        char *node = PLAYER ? *(char **)(PLAYER + ENT_NODE) : NULL;
        if (!is_domain(dom) || !node)
            return snprintf(out, size, "%s is not a domain (or no player)", a1), 1;
        /* the domain's bounds: +0xc8 min x y z, +0xd4 max x y z (from memory, 2026-10-25) */
        float *mn = (float *)(dom + 0xc8), *mx = (float *)(dom + 0xd4);
        float pos[3] = {(mn[0] + mx[0]) / 2, mn[1] + 0.1f, (mn[2] + mx[2]) / 2};
        float dir[3] = {*(float *)(node + 0x58), *(float *)(node + 0x5c), *(float *)(node + 0x60)};
        ((void (TSCALL *)(void *, float *, float *, int))(*(void ***)PLAYER)[0x210 / 4])(PLAYER, pos, dir, 1);
        return snprintf(out, size, "player to %.2f %.2f %.2f (bounds %.2f %.2f %.2f .. %.2f %.2f %.2f, state %x, locks %d)",
                        pos[0], pos[1], pos[2], mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], *(unsigned *)(dom + DOM_STATE),
                        *(int *)(dom + DOM_LOCKS)), 1;
    }
    if (!strcmp(cmd, "exit") && role == MODE_SERVER) {
        server_change_level(a1);
        return snprintf(out, size, "level change to %s", a1), 1;
    }
    if (!strcmp(cmd, "restart") && role == MODE_SERVER) {
        restart_pending = 1;
        restart_at = GetTickCount();
        return snprintf(out, size, "team restart (checkpoint %d)", last_cp), 1;
    }
    snprintf(out, size, "!ss state | class <obj> | find <event> | enter/leave <domain> | fire <obj> <event> [bool] |"
                        " goto <domain> | exit <level> | restart");
    return 1;
}

/* ---- install ---- */
static int patch_call(DWORD site, DWORD expect_target, void *hook)
{
    unsigned char want[5] = {0xe8}, rep[5] = {0xe8};
    *(DWORD *)(want + 1) = expect_target - (site + 5);
    *(DWORD *)(rep + 1) = (DWORD)hook - (site + 5);
    return ts_patch_bytes(site, want, rep, 5);
}

static int patch_jmp(DWORD at, const unsigned char *expect, int len, void *hook)
{
    unsigned char rep[16];
    memset(rep, 0x90, sizeof rep);
    rep[0] = 0xe9;
    *(DWORD *)(rep + 1) = (DWORD)hook - (at + 5);
    return ts_patch_bytes(at, expect, rep, len);
}

void storysync_install(void)
{
    char v[16];
    if (!ts_env("TS_STORYSYNC", v, sizeof v) || (v[0] != '1' && _stricmp(v, "all") != 0))
        return;
    any_level = _stricmp(v, "all") == 0;
    if (ts_env("TS_STORYSYNC_FAIL", v, sizeof v) && _stricmp(v, "death") == 0)
        fail_restart = 0;
    verbose = ts_env("TS_STORYSYNC_LOG", v, sizeof v) && v[0] == '1';
    table = calloc(TABLE_SIZE, sizeof(ClassRec));
    if (!table || !ts_tick_install() || !ts_frame_add(frame) || !coopnet_listen(SUB_OCC, 39, on_message)) {
        ts_log("storysync: off (no script tick or mod messages)");
        free(table);
        table = NULL;
        return;
    }
    static const unsigned char domtest[16] = {0x8b, 0x8e, 0xf8, 0x00, 0x00, 0x00, 0x8b, 0x01,
                                              0x8b, 0x90, 0x1c, 0x01, 0x00, 0x00, 0xff, 0xd2};
    static const unsigned char autosave[7] = {0x6a, 0xff, 0x68, 0x68, 0x91, 0xc6, 0x00};
    static const unsigned char fail[7] = {0x6a, 0xff, 0x68, 0xe0, 0x8f, 0xc7, 0x00};
    static const unsigned char spload[11] = {0x51, 0x53, 0x33, 0xdb, 0xf6, 0x05, 0x5c, 0x54, 0x0d, 0x01, 0x01};
    unsigned char domrep[16];
    memset(domrep, 0x90, sizeof domrep);
    domrep[0] = 0xe8;
    *(DWORD *)(domrep + 1) = (DWORD)ss_dom_stub - (SITE_DOMTEST + 5);
    int ok_parse = patch_call(SITE_PARSE, 0x8f9150, (void *)hook_parse), ok_ev = 0;
    for (int i = 0; i < 3; i++)
        ok_ev += patch_call(event_sites[i], 0x903940, (void *)hook_event);
    int ok_dom = ts_patch_bytes(SITE_DOMTEST, domtest, domrep, sizeof domrep);
    int ok_reload = patch_call(SITE_RELOAD, 0x946fc0, (void *)hook_reload_post);
    int ok_save = patch_jmp(FN_AUTOSAVE, autosave, sizeof autosave, (void *)hook_autosave);
    int ok_fail = patch_jmp(FN_FAILMISSION, fail, sizeof fail, (void *)hook_fail);
    int ok_load = patch_jmp(FN_SPLOAD, spload, sizeof spload, (void *)hook_spload);
    enabled = ok_parse && ok_ev == 3;
    ts_log("storysync: %s (scripts %d, events %d/3, domains %d, exits %d, checkpoints %d, failure %d%s, level loads %d)",
           enabled ? "on" : "off: script hooks not found (other exe)", ok_parse, ok_ev, ok_dom, ok_reload, ok_save,
           ok_fail, fail_restart ? " = team restart" : " = death", ok_load);
}
