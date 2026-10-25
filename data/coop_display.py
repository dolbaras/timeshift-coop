"""Screen mode in the game's own Video options (PC frame optionsvideo_pc.ui, group "Video", under depth of field).

Full screen / Borderless / Window, stored as the profile key COOP_DisplayMode ("0" / "1" / "2"); the mod's DLL
reads it at the start (native/display.c), so a change takes effect at the next start (the hint says so).
Without the key the choice shown follows the game's own VIDEO_FullScr. Texts: coop_strings.py COOP_VIDEO_*.
"""

EDITS = [
    ("CacheBlock/ps/optionsvideo_pc.ui", [
        ('''                  name        = VIDEO_UseDOF
                  posX        = 3%, 96%
                  posY        = 51%, 9%
                  text        = UI_VIDEO_OPT_DOF
                  hint        = UI_VIDEO_OPT_DOF_HINT
                  textAlign   = left centerY
                  section     = Video
                  typeVal     = Bool
               }''',
         '''                  name        = VIDEO_UseDOF
                  posX        = 3%, 96%
                  posY        = 51%, 9%
                  text        = UI_VIDEO_OPT_DOF
                  hint        = UI_VIDEO_OPT_DOF_HINT
                  textAlign   = left centerY
                  section     = Video
                  typeVal     = Bool
               }
               {
                  // co-op mod: screen mode (profile key COOP_DisplayMode, read by the mod at the next start)
                  type        = select
                  name        = CoopDisplayMode
                  posX        = 3%, 96%
                  posY        = 61%, 9%
                  text        = COOP_VIDEO_DISPLAY
                  hint        = COOP_VIDEO_DISPLAY_HINT
                  textAlign   = left centerY
                  typeVal     = String
               }'''),
        ('''      Element("VIDEO_MaterialQuality").AddItem("UI_S_BEST", "1")
   end''',
         '''      Element("VIDEO_MaterialQuality").AddItem("UI_S_BEST", "1")

      // co-op mod: screen mode
      Element("CoopDisplayMode").AddItem("COOP_VIDEO_FULL", "0")
      Element("CoopDisplayMode").AddItem("COOP_VIDEO_BORDERLESS", "1")
      Element("CoopDisplayMode").AddItem("COOP_VIDEO_WINDOW", "2")
   end'''),
        ('''   func ReadValues()
      Manipulate("Container", "RESET")
      FillElements()
      Manipulate("Container", "LOAD")
   end''',
         '''   func ReadValues()
      Manipulate("Container", "RESET")
      FillElements()
      Manipulate("Container", "LOAD")
      CoopReadDisplayMode()
   end

   // co-op mod: the screen mode chosen before, or the game's own full screen / window setting
   func CoopReadDisplayMode()
      var mode : string
      mode := $ui.GetUserVar("COOP_DisplayMode")
      if mode != "0" and mode != "1" and mode != "2"
         mode := "0"
         if $ui.GetUserVar("VIDEO_FullScr") == "no"
            mode := "2"
         end
      end
      Element("CoopDisplayMode").SetVal(mode)
   end'''),
        ('''      Manipulate("Container", "SAVE")
''',
         '''      Manipulate("Container", "SAVE")
      $ui.SetUserVar("COOP_DisplayMode", Element("CoopDisplayMode").GetVal())   // co-op mod
'''),
    ]),
]
