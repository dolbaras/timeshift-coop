/*
 * Offline check of the co-op partner on the HUD radar (radarmate.h, coopsession.c):  sh tests/run.sh
 * The stock rule (FUN_005F8820) with the co-op session's RadarType 0 never gives the partner a record (the defect seen live on the N100 pair,
 * n100pair-20261108-234915: no partner blip at 6-20 m); the co-op answer ShowAll gives every other player one, never the local player; the
 * 2-second refresh picks only players without a record, not skipped by the stock rule, not the local one, at most 3 tries per object. Split
 * screen: the shared record names the other player in each view and nothing between views; a new one when the radar dropped it. When the
 * executable is at hand (TS_EXE, default ../../../game/bin/TimeShift.Exe.orig) the patched call site and the stock code the fix relies on are
 * compared with it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../radarmate.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* PE: file offset of a VA */
static long va_off(const unsigned char *d, long n, unsigned va)
{
    unsigned pe = *(const unsigned *)(d + 0x3c), base = *(const unsigned *)(d + pe + 24 + 28);
    unsigned short ns = *(const unsigned short *)(d + pe + 6), opt = *(const unsigned short *)(d + pe + 20);
    for (int i = 0; i < ns; i++) {
        const unsigned char *s = d + pe + 24 + opt + 40 * i;
        unsigned vsz = *(const unsigned *)(s + 8), sva = *(const unsigned *)(s + 12), rsz = *(const unsigned *)(s + 16), raw = *(const unsigned *)(s + 20);
        (void)vsz;
        if (va >= base + sva && va < base + sva + rsz)
            return (long)(raw + (va - base - sva)) < n ? (long)(raw + (va - base - sva)) : -1;
    }
    return -1;
}

int main(void)
{
    /* the defect: the co-op session's game type gives RadarType 0 (or 1 with the partner an "enemy"): no record */
    int stock_partner = rm_stock_registers(0, 0, 0) || rm_stock_registers(1, 0, 1);
    CHECK(!stock_partner, "model of the stock rule: RadarType 0 / ShowFriendly with an enemy: no record");
    printf("     (stock rule, RadarType 0: partner record %d = the missing blip)\n", rm_stock_registers(0, 0, 0));
    /* the fix */
    CHECK(rm_radar_type(0, 1, 1, 0) == RM_SHOW_ALL && rm_radar_type(1, 1, 1, 0) == RM_SHOW_ALL, "co-op session: ShowAll");
    CHECK(rm_radar_type(0, 0, 1, 0) == 0 && rm_radar_type(1, 0, 1, 0) == 1, "outside a co-op session: the stock value");
    CHECK(rm_radar_type(0, 1, 0, 0) == 0, "TS_COOP_RADAR=0: the stock value");
    CHECK(rm_radar_type(0, 0, 0, 1) == RM_SHOW_ALL && rm_radar_type(1, 0, 1, 1) == RM_SHOW_ALL, "the split screen's own call: ShowAll");
    CHECK(rm_stock_registers(rm_radar_type(0, 1, 1, 0), 0, 1), "with the co-op answer the partner gets a record even when FUN_006002E0 calls it an enemy");
    CHECK(!rm_stock_registers(rm_radar_type(0, 1, 1, 0), 1, 0), "never a record for the local player");

    rm_entry e[5] = {{0x100, 0, 0x900, 0}, {0x200, 0, 0, 0}, {0x300, 0x20, 0, 0}, {0x400, 0x4, 0, 0}, {0, 0, 0, 0}};
    CHECK(rm_next(e, 5, 0x200) == -1, "the local player (0x200), an entry out of play (flags bit 5), one the stock skips (bit 2), an empty one: nothing");
    CHECK(rm_next(e, 5, 0x100) == 1, "the partner without a record: entry 1");
    e[1].tries = RM_MAX_TRIES;
    CHECK(rm_next(e, 5, 0x100) == -1, "an object that got no record after 3 tries: left alone");
    e[1].tries = 0;
    e[1].rec = 0x901;
    CHECK(rm_next(e, 5, 0x100) == -1, "once registered: not again");
    e[0].rec = 0;
    CHECK(rm_next(e, 5, 0x200) == 0, "a record lost (e.g. after a level change): registered again");

    /* the co-op colour */
    float red[4] = {255, 64, 64, 255}, blue[4] = {78, 93, 255, 255};
    CHECK(rm_colour_differs(red) && !rm_colour_differs(blue), "a red (enemy) blip is repainted, a blue one is left alone");

    /* split screen */
    CHECK(rm_view_obj(0, 0x1000, 0x2000) == 0x2000 && rm_view_obj(1, 0x1000, 0x2000) == 0x1000, "player 1's view: player 2; player 2's view: player 1");
    CHECK(!rm_view_obj(-1, 0x1000, 0x2000) && !rm_view_obj(0, 0x1000, 0) && !rm_view_obj(1, 0, 0x2000) && !rm_view_obj(0, 0x1000, 0x1000),
          "no view, no player 2, no player 1, the same object: NULL");
    CHECK(rm_split_need(0, 0) && rm_split_need(0x5000, 1) && !rm_split_need(0x5000, 2) && !rm_split_need(0x5000, 3),
          "a record is made when there is none or the radar dropped its reference (refs < 2)");

    const char *path = getenv("TS_EXE");
    if (!path || !*path) path = "../../../game/bin/TimeShift.Exe.orig";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("(no executable at %s: the build facts are not compared)\n", path);
    } else {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *d = malloc((size_t)n);
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n) {
            long o = va_off(d, n, RM_OPT_SITE);
            CHECK(o >= 0 && d[o] == 0xe8 && RM_OPT_SITE + 5 + *(int *)(d + o + 1) == RM_OPT_GET, "exe: 0x5f8900 calls 0x402e30 (the RadarType read)");
            long r = va_off(d, n, RM_OPT_GET + 0x17);   /* FUN_00402E30's early return: ret 8 */
            CHECK(r >= 0 && d[r] == 0xc2 && d[r + 1] == 8, "exe: FUN_00402E30 returns with ret 8 (thiscall, two arguments)");
            long g = va_off(d, n, RM_REGISTER + 0x524);  /* FUN_005F8820's epilogue: ret 8 */
            CHECK(g >= 0 && d[g] == 0xc2 && d[g + 1] == 8, "exe: FUN_005F8820 returns with ret 8 (stdcall (list, index))");
            /* FUN_00402E30 writes the value through its 2nd argument (cached path: mov ecx,[esp+0x14]; mov [ecx],eax at 0x402e82) */
            long w = va_off(d, n, 0x402e82);
            CHECK(w >= 0 && !memcmp(d + w, "\x8b\x4c\x24\x14\x89\x01", 6), "exe: FUN_00402E30 writes the value through its 2nd argument");
            /* the 2nd argument is &[esp+0x20] (lea edx,[esp+0x20]; push edx at 0x5f88f0), tested after the call: mov eax,[esp+0x20] at 0x5f893c */
            long a = va_off(d, n, 0x5f88f0), t = va_off(d, n, 0x5f893c);
            CHECK(a >= 0 && !memcmp(d + a, "\x8d\x54\x24\x20\x52", 5), "exe: the value's address is the 2nd argument of the read");
            CHECK(t >= 0 && !memcmp(d + t, "\x8b\x44\x24\x20\x85\xc0", 6), "exe: FUN_005F8820 tests the value it read (0 = no record)");
            long c = va_off(d, n, 0x5f8948);   /* cmp eax,1: ShowFriendly asks FUN_006002E0 (enemy) */
            CHECK(c >= 0 && d[c] == 0x83 && d[c + 1] == 0xf8 && d[c + 2] == 1, "exe: value 1 asks the enemy test, other values register");
            struct { unsigned va; const char *b; int n; const char *what; } f[] = {
                {0x5f8999, "\x39\x05\x80\x81\xfc\x00\x0f\x84", 8, "FUN_005F8820: no record for the local player ([0xfc8180])"},
                {0x5f89a5, "\x8b\x55\x68\xc1\xea\x02\xf6\xc2\x01", 9, "FUN_005F8820: no record for an entry with flags +0x68 bit 2"},
                {0x5f8b78, "\xb8\x5c\x00\x00\x00", 5, "FUN_005F8820: the record is 0x5c bytes"},
                {0x5f8cf1, "\x8b\x0d\xc8\x5e\x09\x01", 6, "FUN_005F8820: the radar interface from [0x1095ec8]"},
                {0x5f8d04, "\x8b\x01\x8b\x50\x1c\xff\xd2", 7, "... its vtable +0x1c"},
                {0x6aefa8, "\xc7\x46\x04\x01\x00\x00\x00", 7, "record ctor FUN_006AEF70: refcount +4 starts at 1"},
                {0x6aefdc, "\x89\x4e\x2c", 3, "record ctor: the object (ecx) at +0x2c, a plain pointer"},
                {0x5b0802, "\x8b\x81\x40\x01\x00\x00", 6, "radar add FUN_005B07E0: the list at +0x140"},
                {0x5b0810, "\x8b\x77\x2c\x8b\x10\x39\x72\x2c\x74", 9, "radar add: a record of an object already listed is not added"},
                {0x5b0a08, "\x8b\xbe\x48\x02\x00\x00", 6, "blip draw FUN_005B09A0: the list at radar +0x248 (= interface +0x140)"},
                {0x5b0a52, "\x8b\x4b\x2c\x85\xc9\x0f\x84", 7, "blip draw: a record whose object is NULL is skipped"},
                {0x5b09d4, "\x8b\x0d\x80\x81\xfc\x00", 6, "blip draw: relative to the local player [0xfc8180]"},
                {0x5b0962, "\x39\x56\x2c", 3, "radar remove FUN_005B0950: by object"},
                {0x6aefe1, "\x89\x56\x34", 3, "record ctor: the colour argument goes to +0x34 (+0x38, +0x3c, +0x40 follow)"},
                {0x5b0ab8, "\xf3\x0f\x10\x43\x34", 5, "blip draw: the colour read from the record +0x34"},
            };
            for (unsigned k = 0; k < sizeof f / sizeof f[0]; k++) {
                long q = va_off(d, n, f[k].va);
                CHECK(q >= 0 && !memcmp(d + q, f[k].b, (size_t)f[k].n), "exe: %s (%08x)", f[k].what, f[k].va);
            }
            /* FUN_005F8820's colour constants: the blue set (0xdc97f8 78, 0xdc97f4 93, 0xdc8e3c 255) is the one the co-op partner gets */
            unsigned cv[3] = {0xdc97f8u, 0xdc97f4u, 0xdc8e3cu};
            float want[3] = {78.0f, 93.0f, 255.0f};
            int cok = 1;
            for (int k = 0; k < 3; k++) {
                long q = va_off(d, n, cv[k]);
                float v = 0;
                if (q >= 0) memcpy(&v, d + q, 4);
                if (q < 0 || v != want[k]) cok = 0;
            }
            CHECK(cok && rm_partner_colour[0] == 78.0f && rm_partner_colour[1] == 93.0f && rm_partner_colour[2] == 255.0f,
                  "exe: the co-op colour is the game's own blue of FUN_005F8820 (78, 93, 255)");
        } else {
            CHECK(0, "exe read");
        }
        free(d);
        fclose(f);
    }
    printf("radarmate: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
