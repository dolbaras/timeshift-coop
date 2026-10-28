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
#include "ts.h"

#define LEVEL_NAME ((const char *)0xfcabc8)
#define PLAYER (*(unsigned char **)0xfc8180)

typedef struct {
    int valid, cp;
    char level[0x80];
    float pos[3], ammo_total;
    int slots[3], magazine;
} Rec;

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

/* <game folder>\coop\continue.txt (the folder above the exe's own) */
static void record_path(char *out, int size, int make_dir)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\');
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
    snprintf(out, size, "%s\\coop\\continue.txt", exe);
}

static int read_record(Rec *r)
{
    char path[MAX_PATH], line[256];
    memset(r, 0, sizeof *r);
    record_path(path, sizeof path, 0);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "level=", 6))
            snprintf(r->level, sizeof r->level, "%s", line + 6);
        else if (!strncmp(line, "checkpoint=", 11))
            r->cp = atoi(line + 11);
        else if (!strncmp(line, "pos=", 4))
            sscanf(line + 4, "%f %f %f", &r->pos[0], &r->pos[1], &r->pos[2]);
        else if (!strncmp(line, "slots=", 6))
            sscanf(line + 6, "%d %d %d", &r->slots[0], &r->slots[1], &r->slots[2]);
        else if (!strncmp(line, "ammo=", 5))
            sscanf(line + 5, "%f %d", &r->ammo_total, &r->magazine);
    }
    fclose(f);
    r->valid = r->level[0] != 0 && r->cp > 0;
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
    char path[MAX_PATH];
    record_path(path, sizeof path, 1);
    FILE *f = fopen(path, "w");
    if (!f) {
        ts_log("coopsave: cannot write %s", path);
        return;
    }
    fprintf(f, "level=%s\ncheckpoint=%d\npos=%.2f %.2f %.2f\nslots=%d %d %d\nammo=%.1f %d\n", r.level, id, pos[0], pos[1], pos[2],
            r.slots[0], r.slots[1], r.slots[2], r.ammo_total, r.magazine);
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
    ts_log("coopsave: the host's weapons %d %d %d and ammunition %.0f/%d put back (%s)", pending.slots[0], pending.slots[1],
           pending.slots[2], pending.ammo_total, pending.magazine, ok ? "ok" : "no weapon record");
}
