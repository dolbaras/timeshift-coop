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

typedef struct {
    int valid, cp;
    char level[0x80];
    float pos[3], ammo_total, health, energy;
    int slots[3], magazine;
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
    int format = 0, lines = 0;
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
    }
    fclose(f);
    /* a record of another format, an unknown chapter or values out of range is not used (a damaged or old file) */
    r->valid = format == FORMAT && known_chapter(r->level) && r->cp >= 1 && r->cp <= 99 && sane(r->ammo_total, 0, 99999) &&
               r->magazine >= 0 && r->magazine <= 9999 && sane(r->health, 0, 1000) && sane(r->energy, 0, 10);
    for (int i = 0; i < 3; i++)
        if (r->slots[i] < 0 || r->slots[i] > 40)
            r->valid = 0;
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
    char path[MAX_PATH];
    record_path(path, sizeof path, 1);
    FILE *f = fopen(path, "w");
    if (!f) {
        ts_log("coopsave: cannot write %s", path);
        return;
    }
    fprintf(f, "format=%d\nlevel=%s\ncheckpoint=%d\npos=%.2f %.2f %.2f\nslots=%d %d %d\nammo=%.1f %d\nhealth=%.1f\nenergy=%.2f\n", FORMAT,
            r.level, id, pos[0], pos[1], pos[2], r.slots[0], r.slots[1], r.slots[2], r.ammo_total, r.magazine, r.health, r.energy);
    fclose(f);
    ts_log("coopsave: checkpoint %d of %s recorded (weapons %d %d %d, ammunition %.0f/%d)", id, r.level, r.slots[0], r.slots[1], r.slots[2],
           r.ammo_total, r.magazine);
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
    snprintf(ssl, sizeof ssl, "$ui.SetStr(\"CoopHasSave\", \"%d\")", have);
    ts_script_run(ssl, out, sizeof out);
    if (have) {
        snprintf(ssl, sizeof ssl, "$ui.SetStr(\"CoopContLevel\", \"%s\")", r.level);
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
    apply_state = 1;
    apply_at = GetTickCount() + 14000;
    ts_log("coopsave: continuing %s at checkpoint %d", r.level, r.cp);
    return r.cp;
}

/* From the script tick: a few seconds into the continued level, the host's weapons and ammunition. */
void coopsave_tick(void)
{
    if (apply_state != 1 || (int)(GetTickCount() - apply_at) < 0)
        return;
    unsigned char *pl = PLAYER;
    if (!pl)
        return;
    apply_state = 2;
    char ssl[128], out[96];
    if (pending.slots[0] | pending.slots[1] | pending.slots[2]) {
        snprintf(ssl, sizeof ssl, "$player.SetupWeapon(%d, %d, %d)", pending.slots[0], pending.slots[1], pending.slots[2]);
        ts_script_run(ssl, out, sizeof out);
    }
    int ok = split_ammo_set(pl, pending.ammo_total, pending.magazine);
    /* life (never below 1: a record taken at 0 would be a dead host) and time energy */
    if (!IsBadWritePtr(pl + 0x234, 4) && pending.health > 0)
        *(float *)(pl + 0x234) = pending.health < 1.0f ? 1.0f : pending.health;
    if (!IsBadWritePtr(pl + 0x2ac, 4))
        *(float *)(pl + 0x2ac) = pending.energy;
    ts_log("coopsave: the host's weapons %d %d %d, ammunition %.0f/%d, life %.0f and time energy %.2f put back (%s)", pending.slots[0],
           pending.slots[1], pending.slots[2], pending.ammo_total, pending.magazine, pending.health, pending.energy,
           ok ? "ok" : "no weapon record");
}
