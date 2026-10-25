"""Co-op rules that are data: they act only in network sessions on campaign levels.

- contwosaves.ui ("continue without saves?"): the engine opens it when a save fails. On a server or a
  client every campaign autosave fails (the save handler runs only in single-player mode), and the frame
  then shows as an empty menu over the game. In a multiplayer session it now takes the "continue without
  saves" answer at once. Stock multiplayer maps never save, so they never open it.
- ai_brain.cls: isSyncMP = Yes. The AI classes carry network code the shipped data leaves off; with it the
  server gives every soldier a net id and the mod's aisync.c replicates movement, animation and deaths.
  Only levels with AI are affected (the stock multiplayer maps have none).

The multiplayer rules a co-op server must not have (warm-up, ready system, friendly fire) are switched off
by the mod's DLL in co-op sessions only (native/coopsession.c); the stock defaults stay as they are.
"""

EDITS = [
    ("CacheBlock/ps/contwosaves.ui", [
        ('''   override OnUpdate
      if GetStr("Inited") == "No"
''',
         '''   override OnUpdate
      if $ui.GetGameInfo("IsMultiplayer") == "Yes"
         // co-op mod: on a co-op server or client a failed autosave must not open a menu
         $saves_mng.ResetDevice()
         $ui.SetVariable("CONFIG", "WorkWOSaves", "bool", "Yes")
         $ui.CloseMenu()
         return
      end
      if GetStr("Inited") == "No"
'''),
    ]),
    ("CacheBlock/ps/ai_brain.cls", [
        ('''ai_brain {
   parentDesc = entity
''',
         '''ai_brain {
   parentDesc = entity
   isSyncMP = Yes
'''),
    ]),
]
