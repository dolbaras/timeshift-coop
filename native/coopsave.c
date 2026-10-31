/*
 * Co-op "Continue": the host's progress record and its use at the start of the next co-op session.
 *
 * The game's own saves cannot be loaded on a server (docs/multiplayer.md, "Saves and checkpoints on the server":
 * LoadGame starts a single-player game). The mod keeps its own small record instead, written at every checkpoint
 * the host reaches (storysync.c calls coopsave_checkpoint): the chapter (level), the checkpoint number and its
 * position, the host's weapon slots and the ammunition of its primary weapon. It is the text file coop\continue.txt
 * in the game folder (the installer's folder for the mod's files; removed with it).
 *
 * The Co-op menu's "Continue" button (data/ui/coopmenu.ui) is enabled while a record exists (UI string
 * CoopHasSave = "1", published once a second from split.c's menu poll) and hosts a co-op session on the recorded
 * chapter, with the host frame's own launch (CoopAuto = "host"; UI strings CoopLevel, CoopDifficulty, ...) and
 * CoopContinue = "1". When the level comes up (storysync.c level_changed asks coopsave_resume), the session
 * is put at the recorded checkpoint by the same restore a team restart uses (the level's Check_Point_<n> object,
 * storysync.c restore_step), and a few seconds later the host gets its weapons and ammunition back. Clients join
 * as usual and are put at the checkpoint by the session's respawn points.
 * TS_COOPSAVE=0 switches the record off.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ts.h"

#define LEVEL_NAME ((const char *)0xfcabc8)
#define PLAYER (*(unsigned char **)0xfc8180)
#define TSCALL __attribute__((thiscall))

/* ---- the player's real weapon inventory ----
 * The player holds its weapons in a manager embedded at player+0x328: +0x32c an array of weapon objects indexed by weapon
 * number (10 entries, +0x330 the count), +0x338/+0x33c/+0x340 the three weapons chosen by SetupWeapon (pointers into that array),
 * +0x34c the current one. Each weapon (virtual base, as split.c's ammo_rec) answers vtable +0x4c(which) with an ammunition
 * record (+8 total, +0xc rounds in the magazine) and +0x14 with its type number. The grenades sit in the list at player+0x36c. */
#define NWPN 10
typedef struct {
    int have;                 /* the record carries an inventory (optional lines of the file) */
    int set[3];               /* weapon numbers of the three chosen weapons, -1 unknown */
    float tot[NWPN][2];       /* per weapon number: ammunition total of record 0 / 1 */
    int mag[NWPN][2];
    int known[NWPN];
    int ngren, gsel, gren[8];   /* the grenade list at player+0x36c: counts per type, the selected type */
} Inv;

static BYTE *obj_vb(BYTE *w)
{
    if (!w || ((DWORD)w & 3) || IsBadReadPtr(w, 8) || IsBadReadPtr(*(BYTE **)(w + 4), 8))
        return NULL;
    BYTE *vb = w + 4 + *(int *)(*(BYTE **)(w + 4) + 4);
    if (IsBadReadPtr(vb, 4) || IsBadReadPtr(*(void **)vb, 0x50))
        return NULL;
    return vb;
}

static BYTE *obj_rec(BYTE *w, int which)
{
    BYTE *vb = obj_vb(w);
    if (!vb)
        return NULL;
    BYTE *rec = ((BYTE * (TSCALL *)(void *, int))(*(void ***)vb)[0x4c / 4])(vb, which);
    return rec && !IsBadReadPtr(rec, 0x24) ? rec : NULL;
}

static int obj_type(BYTE *w)
{
    BYTE *vb = obj_vb(w);
    return vb ? ((int(TSCALL *)(void *))(*(void ***)vb)[0x14 / 4])(vb) : -1;
}

static BYTE *wpn_at(BYTE *pl, int i)
{
    if (!pl || i < 0 || i >= NWPN || IsBadReadPtr(pl + 0x328, 0x28))
        return NULL;
    BYTE *arr = *(BYTE **)(pl + 0x32c);
    int cnt = *(int *)(pl + 0x330);
    if (!arr || i >= cnt || IsBadReadPtr(arr, NWPN * 4))
        return NULL;
    return *(BYTE **)(arr + i * 4);
}

/* what the player holds now */
static void inv_get(BYTE *pl, Inv *v)
{
    memset(v, 0, sizeof *v);
    v->set[0] = v->set[1] = v->set[2] = -1;
    if (!pl || IsBadReadPtr(pl + 0x328, 0x28))
        return;
    for (int i = 0; i < NWPN; i++) {
        BYTE *w = wpn_at(pl, i);
        if (!w)
            continue;
        for (int k = 0; k < 2; k++) {
            BYTE *rec = obj_rec(w, k);
            if (!rec)
                continue;
            v->tot[i][k] = *(float *)(rec + 8);
            v->mag[i][k] = *(int *)(rec + 0xc);
            v->known[i] = 1;
        }
        for (int s = 0; s < 3; s++)
            if (*(BYTE **)(pl + 0x338 + s * 4) == w)
                v->set[s] = i;
    }
    BYTE *g = *(BYTE **)(pl + 0x36c);
    if (g && !IsBadReadPtr(g, 0x14)) {
        BYTE *arr = *(BYTE **)(g + 4);
        int cnt = *(int *)(g + 8);
        if (arr && cnt > 0 && cnt <= 8 && !IsBadReadPtr(arr, cnt * 4)) {
            v->ngren = cnt;
            memcpy(v->gren, arr, cnt * 4);
            v->gsel = *(int *)(g + 0x10);
        }
    }
    v->have = v->set[0] >= 0;
}

/* put an inventory back: the weapon records, the grenade counts (the chosen weapons are set by the caller through SetupWeapon).
 * Returns the number of weapon records written. */
static int inv_set(BYTE *pl, const Inv *v)
{
    int n = 0;
    if (!pl || !v->have)
        return 0;
    for (int i = 0; i < NWPN; i++) {
        BYTE *w = wpn_at(pl, i);
        if (!w || !v->known[i])
            continue;
        for (int k = 0; k < 2; k++) {
            BYTE *rec = obj_rec(w, k);
            if (!rec || IsBadWritePtr(rec + 8, 8))
                continue;
            *(float *)(rec + 8) = v->tot[i][k];
            *(int *)(rec + 0xc) = v->mag[i][k];
            n++;
        }
    }
    BYTE *g = *(BYTE **)(pl + 0x36c);
    if (g && v->ngren > 0 && !IsBadReadPtr(g, 0x14)) {
        BYTE *arr = *(BYTE **)(g + 4);
        int cnt = *(int *)(g + 8);
        if (arr && cnt == v->ngren && !IsBadWritePtr(arr, cnt * 4)) {
            memcpy(arr, v->gren, cnt * 4);
            if (v->gsel >= 0 && v->gsel < cnt && !IsBadWritePtr(g + 0x10, 4))
                *(int *)(g + 0x10) = v->gsel;
        }
    }
    return n;
}

/* The chosen weapons (SetupWeapon; the real set when the record has the inventory, else the slot numbers of an older record), the
 * weapon records, the grenades, life (never below 1: a record taken at 0 would be a dead player) and time energy. */
static int apply_loadout(BYTE *pl, const Inv *v, const int *slots, float health, float energy, int *nrec)
{
    char ssl[128], out[96];
    const int *ws = v->have ? v->set : slots;
    if (ws[0] | ws[1] | ws[2]) {
        snprintf(ssl, sizeof ssl, "$player.SetupWeapon(%d, %d, %d)", ws[0], ws[1], ws[2]);
        ts_script_run(ssl, out, sizeof out);
    }
    *nrec = inv_set(pl, v);
    if (!IsBadWritePtr(pl + 0x234, 4) && health > 0)
        *(float *)(pl + 0x234) = health < 1.0f ? 1.0f : health;
    if (!IsBadWritePtr(pl + 0x2ac, 4) && energy >= 0)
        *(float *)(pl + 0x2ac) = energy;
    return *nrec > 0;
}

#define SUB_CS_ASK 90                /* server -> clients: {u8 1} report your loadout now */
#define SUB_CS_REPORT 91             /* client -> server: {u8 1, u32 id, u8 len, blob[len]} */
#define SUB_CS_HELLO 92              /* client -> server: {u8 1, u32 id} */
#define SUB_CS_RESTORE 93            /* server -> client: {u8 1, u8 len, blob[len]}; len 0 = nothing stored */
#define CS_FIRST 90
#define CS_LAST 99
#define BLOB_MAX 160
#define MAX_CLIENTS 8
#define GAME_MODE_OBJ (*(char **)0xfcab34)

static int cs_listening;
static int net_mode(void);
static void cs_message(int peer, unsigned short sub, const unsigned char *d, int len);
static void client_tick(void);
static char cont_level[0x80];            /* host: the level a Continue put back (clients of this level get their loadout) */

typedef struct {
    int valid, exists, cp;
    char level[0x80];
    float pos[3], ammo_total, health, energy;
    int slots[3], magazine;
    Inv inv;                  /* the real inventory (optional lines; inv.have = 0 in a record of an older mod) */
} Rec;

#define FORMAT 1   /* the record's own version: another number (an older or newer mod) is not used */

/* the campaign's chapters (the host frame's list): a record of any other level is not offered */
static const char *const CHAPTERS[] = {
    "ts2_city_part01", "ts2_city_part02", "ts2_city_part03", "ts2_skyscraper_part1", "ts2_skyscraper_part2",
    "ts2_skyscraper_part3", "ts2_skyscraper_part4", "ts2_zepride1_part01", "ts2_zepcrashsite_part01",
    "ts2_zepcrashsite_part02", "ts2_zepcrashsite_part03", "ts2_summeroutdoor_part01", "ts2_summeroutdoor_part02",
    "ts2_guardfactory_part01", "ts2_guardfactory_part02", "ts2_guardfactory_part03", "ts2_guardfactory_part04",
    "ts2_guardfactory_part05", "ts2_zep_factory_part1", "ts2_zep_factory_part2", "ts2_zep_factory_part3",
    "ts2_zep_factory_part4", "ts2_zepride2_part01", "ts2_cityfinal_part01"};

/* The chapters that have restore objects "Check_Point_<n>" (the level's own restore script of a team restart; found in the level files):
 * the bit n-1 of the mask is set when Check_Point_<n> exists. The other 17 chapters only have AutoSave(n) calls, which the mod cannot
 * rebuild on a server: Continue starts them from their beginning (with the weapons of the record). */
static const struct {
    const char *level;
    unsigned mask;
} MARKERS[] = {{"ts2_city_part03", 0x7},         {"ts2_cityfinal_part01", 0x1},     {"ts2_guardfactory_part05", 0x1},
               {"ts2_zepride1_part01", 0xff},    {"ts2_zep_factory_part1", 0x7},    {"ts2_zep_factory_part2", 0x3},
               {"ts2_zep_factory_part4", 0x7}};

static int has_marker(const char *level, int cp)
{
    for (size_t i = 0; i < sizeof MARKERS / sizeof *MARKERS; i++)
        if (!_stricmp(level, MARKERS[i].level))
            return cp >= 1 && cp <= 32 && (MARKERS[i].mask >> (cp - 1) & 1);
    return 0;
}

static int known_chapter(const char *l)
{
    for (size_t i = 0; i < sizeof CHAPTERS / sizeof *CHAPTERS; i++)
        if (!_stricmp(l, CHAPTERS[i]))
            return 1;
    return 0;
}

static int off = -1;
static Rec pending;           /* the record being applied to the running session */
static int apply_state;       /* 0 none, 1 waiting for the level's restore, 2 applied */
static DWORD apply_at;

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

static int disabled(void)
{
    char v[8];
    if (off < 0)
        off = ts_env("TS_COOPSAVE", v, sizeof v) && v[0] == '0';
    return off;
}

/* <game folder>\coop\continue-<profile>.txt: per game folder (the folder above the exe's own) and per game profile
 * (bin\steamwars.ini [Profile] Name). A reinstall keeps it, the uninstall removes it with coop\. */
static void record_path(char *out, int size, int make_dir)
{
    char exe[MAX_PATH], ini[MAX_PATH], prof[64], clean[64];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(ini, sizeof ini, "%s", exe);
    char *s = strrchr(ini, '\\');
    if (s)
        snprintf(s, sizeof ini - (size_t)(s - ini), "\\steamwars.ini");
    GetPrivateProfileStringA("Profile", "Name", "Default", prof, sizeof prof, ini);
    int n = 0;
    for (const char *c = prof; *c && n < 40; c++)
        clean[n++] = (isalnum((unsigned char)*c) || *c == '_' || *c == '-') ? *c : '_';
    clean[n] = 0;
    s = strrchr(exe, '\\');
    if (s)
        *s = 0;
    s = strrchr(exe, '\\');
    if (s)
        *s = 0;
    if (make_dir) {
        char d[MAX_PATH];
        snprintf(d, sizeof d, "%s\\coop", exe);
        CreateDirectoryA(d, NULL);
    }
    snprintf(out, size, "%s\\coop\\continue-%s.txt", exe, clean);
}

static int sane(float v, float lo, float hi)
{
    return v == v && v >= lo && v <= hi;   /* (not NaN) */
}

static int read_record(Rec *r)
{
    char path[MAX_PATH], line[256];
    memset(r, 0, sizeof *r);
    record_path(path, sizeof path, 0);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    r->exists = 1;
    int format = 0, lines = 0, have_set = 0;
    while (fgets(line, sizeof line, f) && lines++ < 40) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "format=", 7))
            format = atoi(line + 7);
        else if (!strncmp(line, "level=", 6))
            snprintf(r->level, sizeof r->level, "%s", line + 6);
        else if (!strncmp(line, "checkpoint=", 11))
            r->cp = atoi(line + 11);
        else if (!strncmp(line, "pos=", 4))
            sscanf(line + 4, "%f %f %f", &r->pos[0], &r->pos[1], &r->pos[2]);
        else if (!strncmp(line, "slots=", 6))
            sscanf(line + 6, "%d %d %d", &r->slots[0], &r->slots[1], &r->slots[2]);
        else if (!strncmp(line, "ammo=", 5))
            sscanf(line + 5, "%f %d", &r->ammo_total, &r->magazine);
        else if (!strncmp(line, "health=", 7))
            sscanf(line + 7, "%f", &r->health);
        else if (!strncmp(line, "energy=", 7))
            sscanf(line + 7, "%f", &r->energy);
        else if (!strncmp(line, "set=", 4)) {
            if (sscanf(line + 4, "%d %d %d", &r->inv.set[0], &r->inv.set[1], &r->inv.set[2]) == 3)
                have_set = 1;
        } else if (!strncmp(line, "wpn", 3) && line[3] >= '0' && line[3] <= '9' && line[4] == '=') {
            int i = line[3] - '0';
            if (sscanf(line + 5, "%f %d %f %d", &r->inv.tot[i][0], &r->inv.mag[i][0], &r->inv.tot[i][1], &r->inv.mag[i][1]) == 4)
                r->inv.known[i] = 1;
        } else if (!strncmp(line, "gren=", 5)) {
            int *g = r->inv.gren, c = sscanf(line + 5, "%d %d %d %d %d %d %d %d %d", &r->inv.gsel, &g[0], &g[1], &g[2], &g[3], &g[4], &g[5], &g[6], &g[7]);
            r->inv.ngren = c > 1 ? c - 1 : 0;
        }
    }
    fclose(f);
    /* a record of another format, an unknown chapter or values out of range is not used (a damaged or old file) */
    r->valid = format == FORMAT && known_chapter(r->level) && r->cp >= 1 && r->cp <= 99 && sane(r->ammo_total, 0, 99999) &&
               r->magazine >= 0 && r->magazine <= 9999 && sane(r->health, 0, 1000) && sane(r->energy, 0, 10);
    for (int i = 0; i < 3; i++)
        if (r->slots[i] < 0 || r->slots[i] > 40)
            r->valid = 0;
    /* the inventory lines are optional and checked on their own: a bad one is dropped, the rest of the record stays usable */
    Inv *v = &r->inv;
    int ok = have_set;
    for (int i = 0; i < 3; i++)
        ok = ok && v->set[i] >= 0 && v->set[i] < NWPN;
    for (int i = 0; i < NWPN; i++)
        if (v->known[i])
            for (int k = 0; k < 2; k++)
                ok = ok && sane(v->tot[i][k], 0, 99999) && v->mag[i][k] >= 0 && v->mag[i][k] <= 9999;
    for (int i = 0; i < v->ngren; i++)
        ok = ok && v->gren[i] >= 0 && v->gren[i] <= 99;
    ok = ok && v->gsel >= 0 && v->gsel < 8;
    v->have = ok;
    if (!ok)
        memset(v, 0, sizeof *v);
    return r->valid;
}

/* storysync.c: the host reached checkpoint `id` (at pos). */
void coopsave_checkpoint(int id, const float *pos)
{
    if (disabled() || id <= 0)
        return;
    Rec r;
    memset(&r, 0, sizeof r);
    level_base(LEVEL_NAME, r.level, sizeof r.level);
    /* (a continued level's own early checkpoints, below the recorded one, must not replace the record) */
    if (apply_state && pending.valid && _stricmp(pending.level, r.level) == 0 && id < pending.cp) {
        ts_log("coopsave: checkpoint %d of %s not recorded (continuing from %d)", id, r.level, pending.cp);
        return;
    }
    unsigned char *pl = PLAYER;
    if (pl && !IsBadReadPtr(pl + 0x3a8, 12))
        memcpy(r.slots, pl + 0x3a8, sizeof r.slots);
    if (pl)
        split_ammo_get(pl, &r.ammo_total, &r.magazine);
    if (pl && !IsBadReadPtr(pl + 0x234, 4))
        r.health = *(float *)(pl + 0x234);
    if (pl && !IsBadReadPtr(pl + 0x2ac, 4))
        r.energy = *(float *)(pl + 0x2ac);
    inv_get(pl, &r.inv);
    char path[MAX_PATH];
    record_path(path, sizeof path, 1);
    FILE *f = fopen(path, "w");
    if (!f) {
        ts_log("coopsave: cannot write %s", path);
        return;
    }
    fprintf(f, "format=%d\nlevel=%s\ncheckpoint=%d\npos=%.2f %.2f %.2f\nslots=%d %d %d\nammo=%.1f %d\nhealth=%.1f\nenergy=%.2f\n", FORMAT,
            r.level, id, pos[0], pos[1], pos[2], r.slots[0], r.slots[1], r.slots[2], r.ammo_total, r.magazine, r.health, r.energy);
    if (r.inv.have) {
        fprintf(f, "set=%d %d %d\n", r.inv.set[0], r.inv.set[1], r.inv.set[2]);
        for (int i = 0; i < NWPN; i++)
            if (r.inv.known[i])
                fprintf(f, "wpn%d=%.1f %d %.1f %d\n", i, r.inv.tot[i][0], r.inv.mag[i][0], r.inv.tot[i][1], r.inv.mag[i][1]);
        if (r.inv.ngren > 0) {
            fprintf(f, "gren=%d", r.inv.gsel);
            for (int i = 0; i < r.inv.ngren; i++)
                fprintf(f, " %d", r.inv.gren[i]);
            fprintf(f, "\n");
        }
    }
    fclose(f);
    ts_log("coopsave: checkpoint %d of %s recorded (weapons %d %d %d, ammunition %.0f/%d; inventory %s: chosen %d %d %d, grenades %d %d %d %d %d %d %d)", id,
           r.level, r.slots[0], r.slots[1], r.slots[2], r.ammo_total, r.magazine, r.inv.have ? "yes" : "no", r.inv.set[0], r.inv.set[1], r.inv.set[2],
           r.inv.gren[0], r.inv.gren[1], r.inv.gren[2], r.inv.gren[3], r.inv.gren[4], r.inv.gren[5], r.inv.gren[6]);
    {   /* the clients' loadouts (they answer with message 91) */
        unsigned char ask[1] = {1};
        if (net_mode() == 2)
            coopnet_send(-1, SUB_CS_ASK, ask, 1);
    }
}

/* The menu: whether Continue has something (once a second, from the menu poll). */
void coopsave_publish(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 1000)
        return;
    last = now;
    Rec r;
    char ssl[256], out[64];
    int have = !disabled() && read_record(&r);
    /* "1" a usable record, "2" a file that cannot be used (damaged, or of another mod version): the frame says so, "0" none */
    snprintf(ssl, sizeof ssl, "$ui.SetStr(\"CoopHasSave\", \"%d\")", have ? 1 : (!disabled() && r.exists) ? 2 : 0);
    ts_script_run(ssl, out, sizeof out);
    if (have) {
        snprintf(ssl, sizeof ssl, "$ui.SetStr(\"CoopContLevel\", \"%s\")", r.level);
        ts_script_run(ssl, out, sizeof out);
        /* "1": the chapter has the checkpoint's restore object, "0": Continue starts the chapter from its beginning */
        snprintf(ssl, sizeof ssl, "$ui.SetStr(\"CoopContMarker\", \"%d\")", has_marker(r.level, r.cp));
        ts_script_run(ssl, out, sizeof out);
    }
}

/* storysync.c at a level change on the host: the checkpoint to restore when this level is the one a "Continue"
 * asked for (the UI string CoopContinue is taken back at once), else 0. */
int coopsave_resume(const char *base)
{
    if (disabled())
        return 0;
    char out[64];
    ts_script_run("return $ui.GetStr(\"CoopContinue\")", out, sizeof out);
    if (strcmp(out, "1") != 0)
        return 0;
    Rec r;
    if (!read_record(&r) || _stricmp(r.level, base) != 0) {
        ts_log("coopsave: continue asked for, but the record is for %s, not %s", r.valid ? r.level : "(none)", base);
        return 0;
    }
    ts_script_run("$ui.SetStr(\"CoopContinue\", \"0\")", out, sizeof out);
    pending = r;
    snprintf(cont_level, sizeof cont_level, "%s", r.level);
    apply_state = 1;
    apply_at = GetTickCount() + 14000;
    ts_log("coopsave: continuing %s at checkpoint %d", r.level, r.cp);
    return r.cp;
}

/* From the script tick: a few seconds into the continued level, the host's weapons and ammunition. */
void coopsave_tick(void)
{
    if (!cs_listening && !disabled()) {
        cs_listening = 1;
        if (!coopnet_listen(CS_FIRST, CS_LAST, cs_message))
            ts_log("coopsave: no mod messages (the clients' loadouts are not carried)");
    }
    if (net_mode() == 3 && !disabled())
        client_tick();
    if (apply_state != 1 || (int)(GetTickCount() - apply_at) < 0)
        return;
    unsigned char *pl = PLAYER;
    if (!pl)
        return;
    apply_state = 2;
    int nrec = 0;
    int ok = apply_loadout(pl, &pending.inv, pending.slots, pending.health, pending.energy, &nrec);
    if (!nrec)
        ok = split_ammo_set(pl, pending.ammo_total, pending.magazine);
    const int *ws = pending.inv.have ? pending.inv.set : pending.slots;
    ts_log("coopsave: the host's weapons %d %d %d, ammunition %.0f/%d, life %.0f and time energy %.2f put back (%s; inventory: %d records, %d grenade types)",
           ws[0], ws[1], ws[2], pending.ammo_total, pending.magazine, pending.health, pending.energy, ok ? "ok" : "no weapon record", nrec,
           pending.inv.ngren);
}

/* ---- the clients' loadouts ----
 * A client's weapons live on its own machine, so the host asks every client for them at each checkpoint it records (message 90), a
 * client answers with its inventory (91: its id, a packed inventory, life and time energy), and the host keeps the last answer of each
 * client in coop\continue-<profile>-clients.txt (one line "<client id> <hex>"). In a continued level a client that has run its
 * level for a few seconds says hello (92); the host answers with that client's stored inventory (93, empty = nothing stored) and
 * the client puts it back, once per level. A client id is a hash of the PlayerGUID of its profile (each copy has its own). */
static struct {
    unsigned id;
    int len;
    unsigned char b[BLOB_MAX];
} cl_ent[MAX_CLIENTS];
static int cl_n, cl_loaded;
static volatile int cs_restore_got;      /* client: 0 nothing yet, 1 a loadout came, 2 the host has none */
static unsigned char cs_restore[BLOB_MAX];
static int cs_restore_len;

static int net_mode(void)
{
    char *gm = GAME_MODE_OBJ;
    return gm && !IsBadReadPtr(gm + 0x3c, 4) ? *(int *)(gm + 0x3c) : 0;   /* 2 server, 3 client */
}

/* this copy's id: a hash of the PlayerGUID line of its profile's user.ini */
static unsigned my_client_id(void)
{
    static unsigned id;
    if (id)
        return id;
    char exe[MAX_PATH], ini[MAX_PATH], prof[64], folder[MAX_PATH], guid[96] = "", line[256];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *sl = strrchr(exe, '\\');
    if (sl)
        *sl = 0;
    snprintf(ini, sizeof ini, "%s\\steamwars.ini", exe);
    GetPrivateProfileStringA("Profile", "Name", "Default", prof, sizeof prof, ini);
    GetPrivateProfileStringA("Profile", "Folder", "..\\UserConfigs\\", folder, sizeof folder, ini);
    snprintf(ini, sizeof ini, "%s\\%s%s\\user.ini", exe, folder, prof);
    FILE *f = fopen(ini, "r");
    while (f && fgets(line, sizeof line, f))
        if (!_strnicmp(line, "PlayerGUID", 10)) {
            char *q = strchr(line, '"');
            if (q)
                snprintf(guid, sizeof guid, "%s", q + 1);
            break;
        }
    if (f)
        fclose(f);
    unsigned h = 2166136261u;
    for (const char *c = guid; *c && *c != '"'; c++)
        h = (h ^ (unsigned char)*c) * 16777619u;
    id = h ? h : 1;
    return id;
}

/* inventory + life + energy as bytes: set[3], mask u16, per known weapon {f32 t0, u16 m0, f32 t1, u16 m1}, u8 ngren, u8 gsel, gren[], f32 life, f32 energy */
static int blob_pack(BYTE *pl, unsigned char *b)
{
    Inv v;
    inv_get(pl, &v);
    if (!v.have)
        return 0;
    int n = 0;
    for (int i = 0; i < 3; i++)
        b[n++] = (unsigned char)v.set[i];
    unsigned short mask = 0;
    for (int i = 0; i < NWPN; i++)
        if (v.known[i])
            mask |= (unsigned short)(1u << i);
    memcpy(b + n, &mask, 2), n += 2;
    for (int i = 0; i < NWPN; i++)
        if (v.known[i])
            for (int k = 0; k < 2; k++) {
                unsigned short m = (unsigned short)(v.mag[i][k] < 0 ? 0 : v.mag[i][k] > 9999 ? 9999 : v.mag[i][k]);
                memcpy(b + n, &v.tot[i][k], 4), n += 4;
                memcpy(b + n, &m, 2), n += 2;
            }
    b[n++] = (unsigned char)v.ngren;
    b[n++] = (unsigned char)(v.gsel < 0 || v.gsel > 7 ? 0 : v.gsel);
    for (int i = 0; i < v.ngren; i++)
        b[n++] = (unsigned char)(v.gren[i] < 0 ? 0 : v.gren[i] > 99 ? 99 : v.gren[i]);
    float life = IsBadReadPtr(pl + 0x234, 4) ? 0.f : *(float *)(pl + 0x234), en = IsBadReadPtr(pl + 0x2ac, 4) ? 0.f : *(float *)(pl + 0x2ac);
    memcpy(b + n, &life, 4), n += 4;
    memcpy(b + n, &en, 4), n += 4;
    return n;
}

static int blob_unpack(const unsigned char *b, int len, Inv *v, float *life, float *en)
{
    memset(v, 0, sizeof *v);
    int n = 0;
    if (len < 7)
        return 0;
    for (int i = 0; i < 3; i++)
        v->set[i] = b[n++];
    unsigned short mask = 0;
    memcpy(&mask, b + n, 2), n += 2;
    for (int i = 0; i < NWPN; i++)
        if (mask & (1u << i)) {
            if (n + 12 > len)
                return 0;
            for (int k = 0; k < 2; k++) {
                unsigned short m;
                memcpy(&v->tot[i][k], b + n, 4), n += 4;
                memcpy(&m, b + n, 2), n += 2;
                v->mag[i][k] = m;
                if (!sane(v->tot[i][k], 0, 99999))
                    return 0;
            }
            v->known[i] = 1;
        }
    if (n + 2 > len)
        return 0;
    v->ngren = b[n++];
    v->gsel = b[n++];
    if (v->ngren > 8 || v->gsel > 7 || n + v->ngren + 8 > len)
        return 0;
    for (int i = 0; i < v->ngren; i++)
        v->gren[i] = b[n++];
    memcpy(life, b + n, 4), n += 4;
    memcpy(en, b + n, 4);
    for (int i = 0; i < 3; i++)
        if (v->set[i] >= NWPN)
            return 0;
    if (!sane(*life, 0, 1000) || !sane(*en, 0, 10))
        return 0;
    v->have = 1;
    return 1;
}

static void clients_path(char *out, int size, int make_dir)
{
    record_path(out, size, make_dir);
    char *dot = strrchr(out, '.');
    if (dot)
        snprintf(dot, size - (int)(dot - out), "-clients.txt");
}

static void clients_load(void)
{
    if (cl_loaded)
        return;
    cl_loaded = 1;
    cl_n = 0;
    char path[MAX_PATH], line[2 * BLOB_MAX + 40];
    clients_path(path, sizeof path, 0);
    FILE *f = fopen(path, "r");
    for (int lines = 0; f && fgets(line, sizeof line, f) && lines < 40 && cl_n < MAX_CLIENTS; lines++) {
        unsigned id = 0;
        int pos = 0;
        if (sscanf(line, "%x %n", &id, &pos) < 1 || !id || pos <= 0)
            continue;
        int len = 0;
        for (const char *h = line + pos; isxdigit((unsigned char)h[0]) && isxdigit((unsigned char)h[1]) && len < BLOB_MAX; h += 2, len++) {
            unsigned x;
            sscanf(h, "%2x", &x);
            cl_ent[cl_n].b[len] = (unsigned char)x;
        }
        Inv v;
        float l, e;
        if (!blob_unpack(cl_ent[cl_n].b, len, &v, &l, &e))
            continue;   /* (a damaged line is dropped) */
        cl_ent[cl_n].id = id;
        cl_ent[cl_n].len = len;
        cl_n++;
    }
    if (f)
        fclose(f);
}

static void clients_save(void)
{
    char path[MAX_PATH];
    clients_path(path, sizeof path, 1);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < cl_n; i++) {
        fprintf(f, "%08x ", cl_ent[i].id);
        for (int k = 0; k < cl_ent[i].len; k++)
            fprintf(f, "%02x", cl_ent[i].b[k]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static int slot_of_peer(int peer)
{
    char *gm = GAME_MODE_OBJ;
    int n = gm ? *(int *)(gm + 0x2c) : 0;
    for (int i = 0; i < n; i++)
        if (*(int *)(*(char **)(gm + 0x28) + i * 0xa4) == peer)
            return i;
    return -1;
}

static void cs_message(int peer, unsigned short sub, const unsigned char *d, int len)
{
    if (disabled() || len < 1 || d[0] != 1)
        return;
    if (sub == SUB_CS_ASK && net_mode() == 3) {
        unsigned char m[8 + BLOB_MAX];
        unsigned id = my_client_id();
        int n = blob_pack(PLAYER, m + 6);
        if (n <= 0 || n > BLOB_MAX)
            return;
        m[0] = 1;
        memcpy(m + 1, &id, 4);
        m[5] = (unsigned char)n;
        coopnet_send(-1, SUB_CS_REPORT, m, 6 + n);
    } else if (sub == SUB_CS_REPORT && net_mode() == 2 && len >= 6 && d[5] <= BLOB_MAX && len >= 6 + d[5]) {
        unsigned id;
        memcpy(&id, d + 1, 4);
        Inv v;
        float l, e;
        if (!id || !blob_unpack(d + 6, d[5], &v, &l, &e)) {
            ts_log("coopsave: a client's loadout report is not usable (%d bytes)", len);
            return;
        }
        clients_load();
        int k = 0;
        while (k < cl_n && cl_ent[k].id != id)
            k++;
        if (k == cl_n && cl_n >= MAX_CLIENTS)
            k = 0;   /* (more than eight clients over time: the first entry goes) */
        else if (k == cl_n)
            cl_n++;
        cl_ent[k].id = id;
        cl_ent[k].len = d[5];
        memcpy(cl_ent[k].b, d + 6, d[5]);
        clients_save();
        ts_log("coopsave: client %08x's loadout recorded (weapons %d %d %d, life %.0f, %d bytes)", id, v.set[0], v.set[1], v.set[2], (double)l, d[5]);
    } else if (sub == SUB_CS_HELLO && net_mode() == 2 && len >= 5) {
        unsigned id;
        memcpy(&id, d + 1, 4);
        unsigned char m[8 + BLOB_MAX] = {1, 0};
        int n = 0, slot = slot_of_peer(peer);
        char base[0x80];
        level_base(LEVEL_NAME, base, sizeof base);
        if (cont_level[0] && _stricmp(cont_level, base) == 0) {
            clients_load();
            for (int k = 0; k < cl_n; k++)
                if (cl_ent[k].id == id) {
                    n = cl_ent[k].len;
                    memcpy(m + 2, cl_ent[k].b, n);
                }
        }
        m[1] = (unsigned char)n;
        if (slot >= 0)
            coopnet_send(slot, SUB_CS_RESTORE, m, 2 + n);
        ts_log("coopsave: client %08x says hello in %s: %s", id, base, n ? "its stored loadout sent" : "nothing stored (or not a continued level)");
    } else if (sub == SUB_CS_RESTORE && net_mode() == 3 && len >= 2) {
        if (d[1] && d[1] <= BLOB_MAX && len >= 2 + d[1]) {
            cs_restore_len = d[1];
            memcpy(cs_restore, d + 2, d[1]);
            cs_restore_got = 1;   /* a loadout follows */
        } else
            cs_restore_got = 2;   /* nothing stored */
    }
}

/* client side, from the script tick: say hello some seconds into each level, put the host's answer back */
static void client_tick(void)
{
    static char level[0x80];
    static DWORD next_hello;
    static int tries, done;
    char base[0x80];
    BYTE *pl = PLAYER;
    level_base(LEVEL_NAME, base, sizeof base);
    if (strcmp(base, level) != 0) {
        snprintf(level, sizeof level, "%s", base);
        tries = done = 0;
        cs_restore_got = 0;
        next_hello = GetTickCount() + 8000;
    }
    if (!pl || done || (int)(GetTickCount() - next_hello) < 0)
        return;
    if (cs_restore_got == 1) {
        Inv v;
        float life, en;
        int nrec = 0;
        done = 1;
        if (blob_unpack(cs_restore, cs_restore_len, &v, &life, &en)) {
            int ok = apply_loadout(pl, &v, v.set, life, en, &nrec);
            ts_log("coopsave: this client's loadout put back (weapons %d %d %d, life %.0f, %d records, %d grenade types, %s)", v.set[0], v.set[1], v.set[2],
                   (double)life, nrec, v.ngren, ok ? "ok" : "no weapon record");
        } else
            ts_log("coopsave: the host's answer for this client is not usable");
        return;
    }
    if (cs_restore_got == 2 || tries >= 8) {
        done = 1;
        return;
    }
    unsigned char m[8] = {1};
    unsigned id = my_client_id();
    memcpy(m + 1, &id, 4);
    coopnet_send(-1, SUB_CS_HELLO, m, 5);
    tries++;
    next_hello = GetTickCount() + 4000;
}

/* "!cs inv": raw look at the manager and the grenade list (lab) */
static int dump_inv(char *out, int size)
{
    BYTE *pl = PLAYER;
    if (!pl || IsBadReadPtr(pl + 0x328, 0x28))
        return snprintf(out, size, "no player");
    int n = snprintf(out, size, "mgr: arr %p cnt %d cur %d chosen %p %p %p;", *(void **)(pl + 0x32c), *(int *)(pl + 0x330),
                     *(int *)(pl + 0x34c), *(void **)(pl + 0x338), *(void **)(pl + 0x33c), *(void **)(pl + 0x340));
    for (int i = 0; i < NWPN && n < size - 200; i++) {
        BYTE *w = wpn_at(pl, i);
        n += snprintf(out + n, size - n, " [%d] %p", i, (void *)w);
        if (!w)
            continue;
        n += snprintf(out + n, size - n, " type %d", obj_type(w));
        for (int k = 0; k < 2; k++) {
            BYTE *rec = obj_rec(w, k);
            if (rec)
                n += snprintf(out + n, size - n, " r%d(%.0f %d | %x %x %x)", k, (double)*(float *)(rec + 8), *(int *)(rec + 0xc), *(unsigned *)(rec + 0x10),
                              *(unsigned *)(rec + 0x14), *(unsigned *)(rec + 0x18));
        }
        n += snprintf(out + n, size - n, ";");
    }
    BYTE *g = *(BYTE **)(pl + 0x36c);
    if (g && !IsBadReadPtr(g, 0x28)) {
        n += snprintf(out + n, size - n, " grenades %p:", (void *)g);
        for (int k = 0; k < 10; k++)
            n += snprintf(out + n, size - n, " %x", *(unsigned *)(g + k * 4));
        BYTE *arr = *(BYTE **)(g + 4);
        int cnt = *(int *)(g + 8);
        for (int i = 0; arr && i < cnt && i < 8 && !IsBadReadPtr(arr, 4 * (i + 1)) && n < size - 200; i++) {
            BYTE *e = *(BYTE **)(arr + i * 4);
            n += snprintf(out + n, size - n, " {%d %p", i, (void *)e);
            if (e && !IsBadReadPtr(e, 0x40)) {
                n += snprintf(out + n, size - n, " type %d", obj_type(e));
                for (int k = 0; k < 2; k++) {
                    BYTE *rec = obj_rec(e, k);
                    if (rec)
                        n += snprintf(out + n, size - n, " r%d(%.0f %d)", k, (double)*(float *)(rec + 8), *(int *)(rec + 0xc));
                }
            }
            n += snprintf(out + n, size - n, "}");
        }
    }
    return n;
}

int coopsave_console(const char *text, char *out, int size)
{
    if (strncmp(text, "!cs", 3) != 0)
        return 0;
    char cmd[32] = "";
    sscanf(text + 3, "%31s", cmd);
    BYTE *pl = PLAYER;
    if (!strcmp(cmd, "inv"))
        return dump_inv(out, size), 1;
    if (!strcmp(cmd, "rec")) {   /* "!cs rec": the inventory in the record's own terms, one short line (lab) */
        Inv v;
        inv_get(pl, &v);
        int n = snprintf(out, size, "set %d %d %d;", v.set[0], v.set[1], v.set[2]);
        for (int i = 0; i < NWPN && n < size - 80; i++)
            if (v.known[i])
                n += snprintf(out + n, size - n, " w%d %.0f/%d %.0f/%d;", i, (double)v.tot[i][0], v.mag[i][0], (double)v.tot[i][1], v.mag[i][1]);
        n += snprintf(out + n, size - n, " gren sel %d:", v.gsel);
        for (int i = 0; i < v.ngren; i++)
            n += snprintf(out + n, size - n, " %d", v.gren[i]);
        if (pl)
            n += snprintf(out + n, size - n, "; life %.0f energy %.2f", (double)*(float *)(pl + 0x234), (double)*(float *)(pl + 0x2ac));
        (void)n;
        return 1;
    }
    if (!strcmp(cmd, "ammo")) {   /* "!cs ammo <weapon> <record 0|1> <total> <magazine>" (lab) */
        int i, k, m;
        float t;
        BYTE *rec;
        if (sscanf(text + 3, "%*s %d %d %f %d", &i, &k, &t, &m) != 4 || k < 0 || k > 1 || !(rec = obj_rec(wpn_at(pl, i), k)))
            return snprintf(out, size, "usage: !cs ammo <weapon 0-9> <record 0|1> <total> <magazine>"), 1;
        *(float *)(rec + 8) = t;
        *(int *)(rec + 0xc) = m;
        return snprintf(out, size, "weapon %d record %d = %.0f/%d", i, k, (double)t, m), 1;
    }
    if (!strcmp(cmd, "gren")) {   /* "!cs gren <type> <count>" (lab) */
        int i, c;
        BYTE *g = pl ? *(BYTE **)(pl + 0x36c) : NULL;
        if (sscanf(text + 3, "%*s %d %d", &i, &c) != 2 || !g || IsBadReadPtr(g, 0x14) || i < 0 || i >= *(int *)(g + 8))
            return snprintf(out, size, "usage: !cs gren <type> <count>"), 1;
        ((int *)*(BYTE **)(g + 4))[i] = c;
        return snprintf(out, size, "grenade type %d = %d", i, c), 1;
    }
    if (!strcmp(cmd, "hp") || !strcmp(cmd, "en")) {   /* "!cs hp <life>", "!cs en <time energy>" (lab) */
        float v;
        if (!pl || sscanf(text + 3, "%*s %f", &v) != 1)
            return snprintf(out, size, "usage: !cs hp|en <value>"), 1;
        *(float *)(pl + (cmd[0] == 'h' ? 0x234 : 0x2ac)) = v;
        return snprintf(out, size, "%s = %.2f", cmd, (double)v), 1;
    }
    return snprintf(out, size, "!cs inv | ammo | gren | hp | en"), 1;
}
