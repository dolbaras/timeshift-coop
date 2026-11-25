/*
 * The pause menu's Co-op frame (data/ui/coopinfo.ui, data/coop_menu.py) and the Co-op menu's input rows: the DLL's half.
 *
 * The frames are stock UI script frames; they cannot call the DLL. The two sides talk through UI strings (the way CoopRegroup /
 * CoopPad2 / CoopReadyRoom always did), polled from the input poll (gamepad.c -> split.c publish_pad2), which also runs while a
 * menu is up (the script tick stops there).
 *
 *   DLL -> frame (written about twice a second):
 *     CoopPads        connected XInput pads (0-4)
 *     CoopPadNow1/2   pad number (1-4) the local player 1 / 2 uses now (0 = none: player 1 keyboard and mouse; player 2 none), the
 *                     assignment WITH split screen (gamepad.c resolve_pads)
 *     CoopSplitOn     1 = split screen is on
 *     CoopSafe        1 = a cinematic-free moment in play: the second player may be switched
 *     CoopRoleNow     "" / "host" / "client" (the network session, coopsession.c)
 *     CoopPlayers     "Name, Name, ..." (host: itself and the joined clients; client: itself)
 *     CoopPeerN       N = 0..2: the host's joined clients' names ("" = none: the "player control" button needs one)
 *   frame -> DLL (read about twice a second, cleared when handled):
 *     CoopDev1 / CoopDev2   the input devices: a automatic, k keyboard and mouse (player 1 only), 1-4 gamepad number
 *     CoopSplitReq          "1" / "0": second player on / off
 *
 * Why the DLL does the work: split screen on / off is the existing CoopSplit (single player game) / CoopSplitJoin (network game)
 * choice that split.c polls once a second, and a drop-in waits for a safe moment there (split_choice); the removal of a player is the
 * game's own player control frame (ControlPlayer: KICK / BAN), opened by the frame's button; a direct call of the network layer's Kick
 * (vtable 0xdb5754 + 0x20 -> 0x76a060 -> transport 0x77fcf0) crashed the host (live 2026-11-25: null connection record).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ts.h"

int coopsession_role(void);
int coopsession_clients(int *ids, char names[][28], int max);
void gamepad_set_devices(int p1, int p2);
int gamepad_count(void);
int split_session_on(void);
int split_safe_moment(void);
void split_dropin_request(int on);
int ts_profile_value(const char *key, char *out, int size);

#define MAXC 3

static int snap_ids[MAXC], snap_n;

static int dev_of(const char *s, int p2)
{
    if (s[0] == 'a' || s[0] == 0)
        return -2;
    if (s[0] == 'k')
        return p2 ? -2 : -1;
    if (s[0] >= '1' && s[0] <= '4')
        return s[0] - '1';
    return -2;
}

void pausemenu_poll(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (last && now - last < 500)
        return;
    last = now;
    char out[160], ssl[1200];

    /* frame -> DLL (one script call; a missing string is "") */
    ts_script_run("return $ui.GetStr(\"CoopDev1\") + \"|\" + $ui.GetStr(\"CoopDev2\") + \"|\" + $ui.GetStr(\"CoopSplitReq\") + \"|\" + "
                  "\"\"",
                  out, sizeof out);
    char *f[4] = {"", "", "", ""};
    int nf = 0;
    for (char *p = out; nf < 4;) {
        f[nf++] = p;
        char *bar = strchr(p, '|');
        if (!bar)
            break;
        *bar = 0;
        p = bar + 1;
    }
    if (nf == 4) {
        gamepad_set_devices(dev_of(f[0], 0), dev_of(f[1], 1));
        int role = coopsession_role();
        if (f[2][0] == '0' || f[2][0] == '1') {
            int on = f[2][0] == '1';
            ts_log("pausemenu: second player %s requested (%s)", on ? "on" : "off", role ? "network session" : "single player game");
            if (on && !split_safe_moment())
                ts_log("pausemenu: not a safe moment (cinematic or not in play): the request waits in split.c");
            split_dropin_request(on);
            snprintf(ssl, sizeof ssl, "$ui.SetStr(\"%s\", \"%d\")\n$ui.SetStr(\"CoopSplitReq\", \"\")", role ? "CoopSplitJoin" : "CoopSplit", on);
            ts_script_run(ssl, out, sizeof out);
        }
    }

    /* DLL -> frame */
    int role = coopsession_role();
    int ids[8];
    char names[8][28];
    int n = coopsession_clients(ids, names, 8);
    if (n > MAXC)
        n = MAXC;
    snap_n = n;
    memcpy(snap_ids, ids, n * sizeof ids[0]);
    char me[64] = "";
    ts_profile_value("PlayerName", me, sizeof me);
    char clean[64];
    int cn = 0;
    for (int i = 0; me[i] && cn < 24; i++)
        if ((unsigned char)me[i] >= 32 && me[i] != ',' && me[i] != '"' && me[i] != '\\')
            clean[cn++] = me[i];
    clean[cn] = 0;
    char list[400];
    snprintf(list, sizeof list, "%s", cn ? clean : "Player");
    for (int i = 0; i < n; i++) {
        size_t u = strlen(list);
        snprintf(list + u, sizeof list - u, " / %s", names[i]);
    }
    int pad1 = gamepad_slot(0), pad2 = gamepad_slot(1);
    int split = split_session_on();
    int p1 = pad1;   /* (the NOW state: while the screen is split, player 1's pad or none) */
    int off = snprintf(ssl, sizeof ssl,
                       "$ui.SetStr(\"CoopPads\", \"%d\")\n$ui.SetStr(\"CoopPadNow1\", \"%d\")\n$ui.SetStr(\"CoopPadNow2\", \"%d\")\n"
                       "$ui.SetStr(\"CoopSplitOn\", \"%d\")\n$ui.SetStr(\"CoopSafe\", \"%d\")\n$ui.SetStr(\"CoopRoleNow\", \"%s\")\n"
                       "$ui.SetStr(\"CoopPlayers\", \"%s\")",
                       gamepad_count(), p1 + 1, pad2 + 1, split, split_safe_moment(), role == 1 ? "host" : role == 2 ? "client" : "", list);
    for (int i = 0; i < MAXC; i++)
        off += snprintf(ssl + off, sizeof ssl - off, "\n$ui.SetStr(\"CoopPeer%d\", \"%s\")", i, i < n ? names[i] : "");
    ts_script_run(ssl, out, sizeof out);
}
