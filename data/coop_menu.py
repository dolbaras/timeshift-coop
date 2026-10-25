"""The Co-op menu.

Main menu -> "Co-op" (new frame CoopMenu): Host / Join LAN / Join Internet / Direct connection / Back.
Host -> new frame CoopHost: the 24 campaign levels in campaign order (names: the game's own GAME_<level>
strings, preview: the chapter's loading picture), difficulty, max players 2-4, LAN or Internet, optional
password. "Create" hands the choices to the game's own server set-up: CreateGame, CommSettings and
ServerModifiers run their "Next"/"Create" step by themselves while the UI string CoopStage says so, and
CommSettings then points the server at the campaign level (profile ServerLocation; ServerMapId = the
level's singleplayer id) with co-op limits (1 player is enough, no time or score limit). The player's own
multiplayer settings are set aside first and put back by the main menu and the Multiplayer frame
(profile keys CoopKeep_*), so the stock multiplayer keeps them. The stock map list is not changed.
Joining uses the stock LAN list, Internet list and direct connection; co-op servers are named
"[COOP]<player name>". The version check runs in the DLL (native/coopsession.c); a client that has to
leave finds the reason in the UI string CoopError, which the main menu shows.

UI strings shared with the DLL and the lab's test recipe: CoopSession ("host" / "client" / ""),
CoopDifficulty, CoopMaxPlayers, CoopLevel, CoopStage, CoopAuto, CoopError, CoopDataVersion.
"""
import re

# profile keys the co-op server changes; the main menu and the Multiplayer frame restore them
KEPT_KEYS = ["ServerMinPlayers", "ServerMaxPlayers", "ServerTimeLimit", "ServerFragLimit",
             "ServerMaxSpectators", "ServerName", "NetworkMode"]

RESTORE_FUNCS = '''   // co-op mod: a co-op server set the player's own multiplayer settings aside; put them back
   func CoopRestoreProfile()
      var keep : string
      keep := $ui.GetUserVar("CoopKeep")
      if keep == "1"
%RESTORE%
         $ui.SetUserVar("CoopKeep", "0")
         $ui.FlushCfg()
      end
   end

   func CoopRestoreKey(key : string)
      var v : string
      v := $ui.GetUserVar("CoopKeep_" + key)
      if v != "null" and v != ""
         $ui.SetUserVar(key, $ui.GetUserVar("CoopKeep_" + key))
      end
   end

'''

KEEP_FUNCS = '''   // co-op mod: the co-op menu's server set-up runs this frame's "Next" by itself (CoopStage)
   override OnUpdate
      if $ui.GetStr("CoopStage") == "host_comm"
         $ui.SetStr("CoopStage", "host_modifiers")
         if $ui.GetStr("CoopLevel") != ""
            CoopKeepProfile()
            CoopValues()
         end
         SaveValues()
         if $ui.GetStr("CoopLevel") != ""
            CoopLimits()
         end
         $ui.SwitchFrame("ServerModifiers")
      end
   end

   // the player's own values, put back by the main menu (CoopRestoreProfile)
   func CoopKeepProfile()
      var keep : string
      keep := $ui.GetUserVar("CoopKeep")
      if keep != "1"
%KEEP%
         $ui.SetUserVar("CoopKeep", "1")
      end
   end

   func CoopKeepKey(key : string)
      var v : string
      v := $ui.GetUserVar(key)
      if v != "null"
         $ui.SetUserVar("CoopKeep_" + key, $ui.GetUserVar(key))
      end
   end

   func CoopValues()
      Element("ServerName").Element("InputText").SetText($ui.GetStr("CoopServerName"))
      Element("NetworkMode").SetVal($ui.GetStr("CoopNetwork"))
      Element("ServerPassword").SetVal($ui.GetStr("CoopPassword"))
      Element("ServerMaxSpectators").SetVal(0)
      Element("ServerTimeLimit").SetVal(0)
      Element("ServerFragLimit").SetVal(0)
      if $ui.GetStr("CoopMaxPlayers") == "2"
         Element("ServerMaxPlayers").SetVal(2)
      else if $ui.GetStr("CoopMaxPlayers") == "3"
         Element("ServerMaxPlayers").SetVal(3)
      else
         Element("ServerMaxPlayers").SetVal(4)
      end
   end

   // co-op limits and the campaign level (after the map select of CreateGame wrote its own)
   func CoopLimits()
      $ui.SetUserVar("ServerMinPlayers", 1)
      $ui.SetUserVar("ServerTimeLimit", 0)
      $ui.SetUserVar("ServerFragLimit", 0)
      $ui.SetUserVar("ServerLocation", $ui.GetStr("CoopLevel"))
      $ui.SetVariable("Multiplayer", "ServerMapId", "int", $ui.GetStr("CoopMapId"))
      $ui.FlushCfg()
   end

'''


def sp_levels(text):
    """[(level, id, loading texture)] of the singleplayer list of gs_map_list.cls, in file order."""
    t = text.replace("\r\n", "\n")
    i = t.find("singleplayer {")
    if i < 0:
        raise ValueError("gs_map_list.cls: no singleplayer list")
    out = []
    for m in re.finditer(r"\n[ \t]+(ts2_\w+)[ \t]*\{([^}]*)\}", t[i:]):
        body = m.group(2)
        lid = re.search(r"\bid\s*=\s*(\d+)", body)
        tex = re.search(r"\bLoadingTexture\s*=\s*(\w+)", body)
        if not lid or not tex:
            raise ValueError("gs_map_list.cls: %s has no id or LoadingTexture" % m.group(1))
        out.append((m.group(1), lid.group(1), tex.group(1)))
    return out


def build(ctx):
    v = ctx.version
    levels = sp_levels(ctx.text("CacheBlock/ps/gs_map_list.cls"))
    if len(levels) != 24:
        raise ValueError("gs_map_list.cls: %d campaign levels, expected 24 (another game version?)" % len(levels))
    tex_names = {e.name.lower() for e in ctx.archive.entries if e.type == 6}
    if ctx.archive.base:
        tex_names |= {e.name.lower() for e in ctx.archive.base.entries if e.type == 6}
    lines, ids = [], []
    for lvl, lid, tex in levels:
        small = tex + "_small"
        if small.lower() not in tex_names:
            small = tex
        lines.append('      AddLine(win, "GAME_%s", "%s", "%s")' % (lvl, lvl, small))
        ids.append('      if level == "%s"\n         return "%s"\n      end' % (lvl, lid))

    host = ctx.data_file("ui/coophost.ui")
    host = host.replace("%LEVEL_LINES%", "\n".join(lines)).replace("%MAPID_LINES%", "\n".join(ids))
    ctx.add("CacheBlock/ps/coophost.ui", host)
    ctx.add("CacheBlock/ps/coopmenu.ui", ctx.data_file("ui/coopmenu.ui").replace("%VERSION%", v))

    restore = RESTORE_FUNCS.replace("%RESTORE%", "\n".join(
        '         CoopRestoreKey("%s")' % k for k in KEPT_KEYS))
    keep = KEEP_FUNCS.replace("%KEEP%", "\n".join('         CoopKeepKey("%s")' % k for k in KEPT_KEYS))

    # ---- main menu: the Co-op entry (after Multiplayer; the entries below move down one row) ----
    shifts = [("Options", "#ifndef _DEMO\n               posY        = 130, 30", "160"),
              ("Profiles", "               posY        = 160, 30", "190"),
              ("Content", "               posY        = 190, 30", "220"),
              ("Update", "               posY        = 220, 30", "250"),
              ("Credits", "               posY        = 250, 30", "280")]
    edits = [('''               text        = UI_BTN_MULTIPLAYER
               //textColor               = 255 192 192 192
               //textColorHighlighted    = 255 149 238 242
            }
''', '''               text        = UI_BTN_MULTIPLAYER
               //textColor               = 255 192 192 192
               //textColorHighlighted    = 255 149 238 242
            }
#ifndef _DEMO
            {
               type        = button
               name        = Coop
               posX        = center, 100%
               posY        = 130, 30
               text        = COOP_BTN_COOP
            }
#endif //_DEMO
''')]
    for name, posy, new in shifts:
        anchor = "               name        = %s\n               posX        = center, 100%%\n%s" % (name, posy)
        edits.append((anchor, re.sub(r"= \d+, 30$", "= %s, 30" % new, anchor)))
    edits += [
        ('''               text        = UI_BTN_STOP_GAME
#ifdef _DEMO
               posY        = 190, 30
#else
               posY        = 280, 30''', '''               text        = UI_BTN_STOP_GAME
#ifdef _DEMO
               posY        = 190, 30
#else
               posY        = 310, 30'''),
        ('''      if $ui.GetStr("KICKED") == "Yes"''', '''      // co-op mod: data version for the DLL, the player's own multiplayer settings back, why a
      // co-op session ended (set by the DLL: another mod version)
      $ui.SetStr("CoopDataVersion", "%s")
      CoopRestoreProfile()
      $ui.SetStr("CoopSession", "")
      $ui.SetStr("CoopStage", "")
      if $ui.GetStr("CoopError") != ""
         CoopShowError($ui.ShowFrame("ErrorMsg"))
      end

      if $ui.GetStr("KICKED") == "Yes"''' % v),
        ('''   override OnButtonPressed("MultiPlayer")''', '''   override OnButtonPressed("Coop")
      $ui.SwitchFrame("CoopMenu")
   end

''' + restore + '''   func CoopShowError(frm : object)
      frm.Element("MessageCaption").SetText("COOP_FRAME_COOP")
      frm.Element("Message").SetText($ui.GetStr("CoopError"))
      $ui.SetStr("CoopError", "")
   end

   override OnButtonPressed("MultiPlayer")'''),
    ]
    ctx.edit("CacheBlock/ps/mainmenu.ui", edits)

    # ---- Multiplayer: restore too (the stock server set-up starts here) ----
    ctx.edit("CacheBlock/ps/multiplayer.ui", [
        ('''   override OnInit
      if $ui.GetGameInfo("Rating", "ESRB") == "Yes"''', '''   override OnInit
      CoopRestoreProfile()
      $ui.SetStr("CoopStage", "")
      if $ui.GetGameInfo("Rating", "ESRB") == "Yes"'''),
        ('''   override OnButtonPressed("CreateGame")''', restore + '''   override OnButtonPressed("CreateGame")'''),
    ])

    # ---- the stock server set-up, driven by CoopStage ----
    ctx.edit("CacheBlock/ps/creategame.ui", [
        ('''   override OnButtonPressed("Back")''', '''   // co-op mod: the co-op menu's server set-up runs this frame's "Next" by itself (CoopStage)
   override OnUpdate
      if $ui.GetStr("CoopStage") == "host_create"
         $ui.SetStr("CoopStage", "host_comm")
         if !AcceptVal()
            $ui.SetStr("CoopStage", "")
            $ui.ShowErrorMsg("UI_SERVER_NO_MAP_SELECTED")
            return
         end
         AcceptMap()
         AcceptModifier()
         Element("ServerLocation").AcceptVal()
         AcceptValues()
         $ui.SwitchFrame("CommSettings")
      end
   end

   override OnButtonPressed("Back")'''),
    ])
    ctx.edit("CacheBlock/ps/commsettings_pc.ui", [
        ('''   override OnButtonPressed("Back")''', keep + '''   override OnButtonPressed("Back")'''),
    ])
    ctx.edit("CacheBlock/ps/servermodifiers.ui", [
        ('''   override OnScrollSelection("LeftList")''', '''   // co-op mod: the co-op menu's server set-up presses "Create" by itself (CoopStage)
   override OnUpdate
      if $ui.GetStr("CoopStage") == "host_modifiers"
         $ui.SetStr("CoopStage", "host_wait")
         StartCreating()
      end
   end

   override OnScrollSelection("LeftList")'''),
    ])
