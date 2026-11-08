/*
 * Offline check of the co-op partner on the HUD radar (radarmate.h, coopsession.c):  sh tests/run.sh
 * The stock rule (FUN_005F8820) with the co-op session's RadarType 0 never gives the partner a record (the defect seen live on the N100 pair,
 * n100pair-20261108-234915: no partner blip at 6-20 m); the co-op answer ShowAll gives every other player one, never the local player; the
 * 2-second refresh picks only players without a record, in play, not the local one. When the executable is at hand (TS_EXE, default
 * ../../../game/bin/TimeShift.Exe.orig) the patched call site and the stock function prologues are compared with it.
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
    CHECK(rm_radar_type(0, 1, 1) == RM_SHOW_ALL && rm_radar_type(1, 1, 1) == RM_SHOW_ALL, "co-op session: ShowAll");
    CHECK(rm_radar_type(0, 0, 1) == 0 && rm_radar_type(1, 0, 1) == 1, "outside a co-op session: the stock value");
    CHECK(rm_radar_type(0, 1, 0) == 0, "TS_COOP_RADAR=0: the stock value");
    CHECK(rm_stock_registers(rm_radar_type(0, 1, 1), 0, 1), "with the co-op answer the partner gets a record even when FUN_006002E0 calls it an enemy");
    CHECK(!rm_stock_registers(rm_radar_type(0, 1, 1), 1, 0), "never a record for the local player");

    rm_entry e[4] = {{0x100, 0, 0x900}, {0x200, 0, 0}, {0x300, 0x20, 0}, {0, 0, 0}};
    CHECK(rm_next(e, 4, 0x200) == -1, "the local player (0x200) and an entry out of play (flags bit 5) and an empty one: nothing");
    CHECK(rm_next(e, 4, 0x100) == 1, "the partner without a record: entry 1");
    e[1].rec = 0x901;
    CHECK(rm_next(e, 4, 0x100) == -1, "once registered: not again");
    e[0].rec = 0;
    CHECK(rm_next(e, 4, 0x200) == 0, "a record lost (e.g. after a level change): registered again");

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
        } else {
            CHECK(0, "exe read");
        }
        free(d);
        fclose(f);
    }
    printf("radarmate: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
