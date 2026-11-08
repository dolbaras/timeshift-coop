TimeShift Co-op for TimeShift (2007, PC)
==============================================================================

Play the TimeShift story campaign together: 2-4 players over a local network or the internet,
on the game's own network code, or two players on one PC with split screen. The mod also makes
the game start on today's PCs, adds gamepad support and screen modes, and brings the game's own
multiplayer back online without GameSpy (closed in 2014).

What the mod does
- Co-op: a "Co-op" entry in the main menu. The host picks a campaign level (any of the 24
  chapters), the difficulty, the number of players (2-4), LAN or Internet and an optional
  password; the others join. Co-op servers are named [COOP]<host's name> and show the campaign
  level as their map.
- Joining: the game's LAN list, its Internet list or a direct connection by IP address.
  The Internet list and Internet hosting go through OpenSpy (a community replacement for the
  closed GameSpy servers); nothing to set up.
- In a co-op session: no friendly fire, no warm-up or ready step; the enemies move, fight and die
  the same on every machine; slow, stop and reverse time work for the players; a player who joins
  a level that is already running gets the state the host's player is in (locks, weapons).
  Every player needs the same version of the mod (a player with another version is told so and
  leaves). A player who got stranded (stuck behind a door, left behind at a lift) opens the pause
  menu (Esc) and picks "Regroup": on a client it moves that player to the host's player, on the
  host it brings every living client to the host.
- Campaign rules for several players: lifts, trains and similar "everybody goes together" places
  wait for the other players and gather them (a line on the screen says how many are missing); the
  zeppelin turrets and the armoured car ride of the campaign give every player a seat (or a place
  on the car); enemies, turrets and cannons that aim "at the player" aim at the nearest player; a
  death scene kills only the player who walked into it; the scanners of the quad levels count the
  host only.
- Continue: after you reach a checkpoint as the host of a co-op game, Co-op -> Continue hosts that
  chapter again from the checkpoint with your chosen weapons, every weapon's ammunition, grenades, health
  and time energy; friends join as usual (a client who rejoins the same session gets its own loadout
  back). The save is per profile, is kept when you update the mod and removed when you uninstall it.
  Where a chapter has no checkpoint marker to restore (only 7 of the 24 have one), it restarts from the
  start of that chapter with your loadout, and the Co-op menu says so; a damaged save is greyed out with
  a reason.
- Real rewind: the reverse power rewinds the world (the host keeps the history): the cranes, lifts,
  bridges, trains, cages and force fields of the puzzles go back on every machine, the reverse
  puzzles work for a client's own reverse key, and a player trapped in a closed container after a
  rewind dies and comes back at the checkpoint, as the mission would fail in the single player game.
- The game's own multiplayer works again: LAN list on every network adapter, Internet servers
  through OpenSpy.
- Today's PCs: the game starts with many CPU cores, with current graphics drivers (Direct3D 9Ex)
  and on GPUs that are neither NVIDIA nor ATI (Intel), with today's video memory sizes.
- Screen modes: in the game's own Video options, "Screen mode": Full screen, Borderless (full screen
  without a frame, quick switching to other programs) or Window. It changes at the next start.
- Gamepads: Xbox-style (XInput) pads in the game and in its menus.
- Split screen: two players on one PC, player 1 on keyboard and mouse or pad 1, player 2 on pad 2.
  You need a second XInput gamepad. Main menu -> Co-op -> SPLIT SCREEN: pick the chapter and the
  difficulty, Start (without a second pad the menu says so and starts nothing). Going back to the
  main menu ends it. Each player has an own view, weapons, HUD and pick-ups; player 2 comes back
  beside player 1 when it dies.
- Split screen in a network game: in the Co-op menu set "Second player on pad 2" to On, then host or
  join as usual. On a host both halves of its screen play in the session and the other players see
  both bodies. On a client the second player is supported in basic form (experimental).

New in 0.1.3
- Level 2 (City, part 2), the covered walkway the robot wrecks, in a network game: when a player
  reversed time during the collapse, the host's walkway came back but the other players' games let
  the collapse run on (one player saw the walkway and another did not; most of its kill zones never
  opened there). The clients now take the collapse, its collision pieces and its kill zones from the
  host during and after the rewind. A client that was still loading when the story allowed the
  reverse kept it locked for the whole level; the host now sends its time-power permissions to every
  client whenever they change. Checked with a host and a client on one test PC.
- Split screen: player 2 now has the same time-power locks as player 1 (before, it could use a
  power the story had not given yet, such as the reverse at the start of level 2). A player who dies
  again right after coming back beside the partner (on the level 2 walkway the comeback spot was
  inside the same kill zone, then a spot without floor) now gets the game's own restart from the
  checkpoint, as in the single-player game; before, both players could end alive in the water below
  the walkway.
- Bug reports: Collect logs.bat (in <game>\coop) now also packs the launcher's and the installer's
  logs and removes more personal data (see "Troubleshooting and reports").
- 0.1.2 fixed the slow walk of player 1 in split screen while player 2 stood still.

Not finished / known limits
- Quads on the quad levels (9 and 12): the enemies' quads follow the host's positions on the clients'
  screens (checked by coordinates on level 9); a client driving a quad was run on level 12 only, and
  how a third machine sees it is not tested. The truck ride at the start of level 9 works: a client
  that is already in the game rides along (checked by log and coordinates).
- Voices: the lines of Cooke and the rebels in the zeppelin intros (levels 8 and 23) and other spoken
  lines are started on the clients from the host's messages (checked in the logs); the sound itself
  could not be judged in the automatic tests (muted windows). The enemies' weapon shots are heard on
  the clients, but the soldiers' own voices and the gibbing sounds are still played on the host
  only. Tell us if a client misses a voice, shots or footsteps.
- A bad connection: joining a game over a link that loses about a third of its packets can fail once
  (the game sends its connect packets a few times and gives up); press Join again. Playing, changing
  levels and Continue were tested with 5 %, 15 % and 30 % lost packets.
- Split screen: cutscenes, the pause menu and the loading screens are full screen for both; each half
  is drawn every second frame, so each half runs at half of the game's frame rate (ts_coop.ini ->
  [Split] -> BothViews=1 draws both halves every frame; it only helps when the frame rate is capped,
  off by default, tried on an Intel card only, not on NVIDIA or AMD); the scripts treat player 1 as "the player" (the enemies and the all-players gates
  count player 2); the shot sounds of player 2 are untested. Split screen on a client is basic: player
  2's pick-ups were checked in a live game, its time power and kills over the network were not.
- Most checks of the rewind, force fields, vehicles and container traps were made by reading clip and
  time values, coordinates and the logs of both machines, not by watching the screens: the results are
  in the numbers, but how it looks in play has had little human testing.
- After the host's rewind, the clients take the rewound machinery's state from the host on level 2
  (the walkway) only. On other levels the kill zones, force fields and pieces that a rewound machine's
  animation switches may stay on a client as they were before the rewind (not seen in the tests; the
  places are listed for a later version).
- Level 2, the walkway the robot wrecks, works as the level is made: reverse time in the first
  seconds of the collapse and run across at once; a few seconds after the rewind the walkway
  collapses again, and a player still on it dies (the game restarts from the checkpoint it saved on
  the walkway). In split screen the walkway's trigger and its time-power zone follow player 1 only:
  RB (F on the keyboard), the automatic power the level's hint names, reverses only while player 1
  is on the walkway (otherwise it gives slow motion); the reverse button itself (D-pad right, B on
  the keyboard) works for both players. Checked with virtual pads in the single-player game and in
  split screen, not yet in a network game.
- Only the PC version of the game with patch 1.2 is supported. A Steam copy (version 1.0) works after
  it is brought to 1.2 with the community-made 1.2 update package that you supply yourself (see Requirements);
  that path was tested on a simulated Steam folder and with the package's files, not on a real Steam
  installation.
- Joining over the Internet (through OpenSpy and a forwarded port) is untested; LAN and a virtual
  LAN are what was tested. A whole campaign played by people from the first to the last level has
  not been done yet: the tests are automatic runs of the 24 levels with a host and a client.
- Please report anything that goes wrong (see "Troubleshooting and reports").

Requirements
- Your own copy of TimeShift with patch 1.2 (the main menu shows "Ver 1.02"). The mod is made for the
  code of the 1.2 bin\TimeShift.exe (SHA-256
  689c63da36cd5902edcad2abe1111d4697a92b12c4e17fe0659b856752641be4). The installer also accepts
  other builds of the same 1.2 code (for example community-made ones with a CPU fix): the mod does its
  own start-up fixes in memory, so such an exe is not needed. An exe of another version is refused,
  changing nothing, and the installer says which exe it found (its SHA-256).
- Steam edition (read this before you install): the Steam copy is patch 1.0, and Steam never
  received the 1.2 update the mod needs. The installer recognises such a copy and, before it changes
  anything, shows a warning and asks you to tick a box (in quiet mode: the switch -AcceptSteamUpdate).
  Installing then REPLACES bin\TimeShift.exe, bin\steamwars.ini and the data archives
  preload\paks\patch.s3darc, patch_ati.s3darc, patch_nv.s3darc and patch_en_us.s3darc with the files
  of the 1.2 update package for the Steam edition (community-made, a zip of about 11.7 MB) that YOU
  supply: you pick the zip, or the folder you extracted it to, in the installer's window (it also looks in
  your Downloads folder). The installer shows the package's TimeShift.exe SHA-256 and the list of files
  before you agree. The mod's own archive does not contain the package or any game file, and the
  mod does not download it. Your originals are backed up in <game>\coop-backup\steam10, and "Uninstall
  Co-op.bat" puts them back exactly (the game is patch 1.0 again; the final message says so). The
  package is third-party and unofficial. Steam's "Verify integrity of game files" also undoes it:
  run the installer again then. The game starts through Steam or through the mod's shortcut.
- Windows 10 or 11.
- Every player needs the same version of the mod.
- A second XInput gamepad for split screen.

Install
1. Run "timeshift-coop-setup-<version>.exe" (one file, nothing to extract: it unpacks itself into a
   temporary folder, starts the installer and deletes the folder afterwards), or extract the .zip
   anywhere and run "Install Co-op.bat" (the same installer). It finds the game folder (or asks
   for it; the exe also looks in the folder it lies in), builds the mod's data from your own game
   files (a few seconds; the mod contains no game files), keeps a copy of every file it replaces
   (in <game>\coop-backup) and installs the mod. Its window shows each step. Running it again
   reinstalls, updates or removes the mod (so does "Uninstall Co-op.bat" in the game folder, which
   the installer leaves either way). Its language is Windows' (English, Russian, German, French,
   Spanish, Italian, Polish, Czech or Hungarian: the languages of the game).
2. Start the game with the "TimeShift Co-op" shortcut the installer puts on the desktop and in
   the Start menu, or as usual (bin\TimeShift.Exe).
Updates: the shortcut first asks GitHub for the newest release of the mod (a second or two;
offline, the game simply starts). When there is a newer one, it downloads it (checked against
the release's SHA-256), runs its installer for your game folder and then starts the game.
A log is in <game>\coop\launcher-log.txt. Starting the game any other way skips this check.
What the installer changes in the game folder: bin\dinput8.dll (the mod; a dinput8.dll that was
there before is kept and put back), bin\TimeShift.exe (the installer sets the large-address-aware
flag so the game can use up to 4 GB of address space on 64-bit Windows: the 24 levels in one
session need it; the original exe is kept in <game>\coop-backup and put back when you uninstall),
coop.s3darc, coop_ati.s3darc and coop_nv.s3darc in preload\paks (built from your archives),
[Preload] arcName in bin\steamwars.ini (coop.s3darc instead of patch.s3darc), the coop folder,
coop-installer.ps1 and "Uninstall Co-op.bat". On a Steam copy also the files of the 1.2 update
package (see Requirements). Nothing outside the game folder is changed except the two shortcuts.
Uninstall: "Uninstall Co-op.bat" in the game folder - removes the mod, puts the replaced files
(the exe too, and on a Steam copy the files the 1.2 update replaced) and the arcName setting back.
Profiles and saves (UserConfigs) are not touched.
Good to know about the installer:
- A game folder under C:\Program Files (x86) makes Windows ask for administrator rights (once for the
  installer; the shortcut's update asks each time it installs a new version).
- If the game folder or one of its files is read-only or locked, the installer says so and changes
  nothing (a read-only flag on the files it rewrites is cleared by itself). The game must be closed.
- With several copies of the game, the installer window shows the first one it found and names the
  others: Browse picks another. Every copy is installed and removed on its own.
- Paths with spaces, other drives and non-English letters (also in the Windows user name) are fine.

Network
- The host needs UDP port 7777 reachable by the other players: allow TimeShift in Windows
  Firewall (Windows asks the first time; allow it on private AND public networks if your network is
  marked public). Over the internet, forward UDP 7777 on the host's router to the host's PC, or play
  over a virtual LAN (Radmin VPN, ZeroTier...) and use the LAN list or the host's address there.
- Two players behind the same router join each other through the LAN list or the local address.

FAQ
- The server list is empty, nobody can join: the host has to allow TimeShift through the firewall (UDP
  7777, see Network); on the internet the host's router has to forward UDP 7777; the LAN list only
  shows servers of your own network. Joining by IP address (Co-op -> Direct connection) works whenever
  UDP 7777 reaches the host.
- I use a VPN or virtual LAN (Radmin, Hamachi, ZeroTier): the mod sends its LAN search on every network
  adapter, so the host appears in the LAN list on the VPN. If it does not, use the host's address on
  that VPN (Direct connection). A host with two adapters (VPN + home network) is fine.
- "The host plays with co-op mod version X": every player needs exactly the same version. Start the
  game with the "TimeShift Co-op" shortcut: it updates the mod before the game starts.
- Where are my saves? In the game folder: <game>\UserConfigs\<profile>\ (the game's own saves and
  settings; the profile name is in bin\steamwars.ini). Continue's record is
  <game>\coop\continue-<profile>.txt. Both survive an update of the mod; the uninstall removes only the
  mod's coop folder (the Continue record with it), never UserConfigs.
- What does Continue not restore? It restores the host's last checkpoint of the last co-op session:
  the chapter, the host's weapons, health and time energy; at a chapter
  that has a restore marker also the place and the story state. Also restored: every weapon's
  ammunition and the grenades, and a client's own loadout when it rejoins the same session. Not
  restored: the enemies' state after the checkpoint, the loadout of a player who dies after the
  Continue (he gets the level's start loadout); a chapter without a marker starts from its
  beginning with your loadout. Only the last checkpoint of the last session is kept.
- A friend got stuck or fell behind: pause menu (Esc) -> Regroup.
- The game does not start after the installer, or shows a black screen: bin\ts_coop.log says which
  start-up fixes were applied; send it with a report (below). Compatibility modes are not needed.
- Nothing happens when I click the shortcut: start bin\TimeShift.exe by hand; the mod is loaded
  either way, only the update check is skipped. <game>\coop\launcher-log.txt says what the shortcut did.
- Does the mod use the internet? Only for the update check (GitHub, anonymous: when GitHub's request
  limit is used up or you are offline, the game simply starts without checking) and, in Internet mode,
  for the server list through OpenSpy.
- Which controllers work? Xbox-style (XInput) pads. Other pads work through Steam Input or DS4Windows.

Windows and antivirus
- The archive holds a PowerShell installer ("Install Co-op.bat" starts it), a DLL without a digital
  signature (bin\dinput8.dll: this is how the mod loads into the game) and a bundled Python. Windows
  SmartScreen may say "Windows protected your PC" for the downloaded zip or the .bat: "More info" ->
  "Run anyway". Some antivirus programs dislike a game mod that hooks the game's memory and may
  quarantine bin\dinput8.dll or stop PowerShell: restore the file and add the game folder to the
  antivirus exclusions (Windows Security -> Virus & threat protection -> Manage settings ->
  Exclusions). The source is public (the mod's GitHub page); the release page shows the zip's SHA-256.
- If no window appears within about 25 seconds, "Install Co-op.bat" says so in a console window; the
  log is %TEMP%\timeshift-coop-install.log.
- The setup exe (timeshift-coop-setup-<version>.exe) is NOT digitally signed (the project has no
  code-signing certificate), so SmartScreen will probably say "Windows protected your PC" ("More
  info" -> "Run anyway") and an antivirus may complain about a self-unpacking program. It contains
  exactly the same .zip as the release (the zip's SHA-256 is on the release page; nothing is
  downloaded, no game files are inside) and does nothing but unpack it into %TEMP% and start the
  PowerShell installer from there. To look inside first, run it from a command prompt as
  "timeshift-coop-setup-<version>.exe --extract <empty folder>": it only unpacks, nothing is
  started. If the exe is blocked, the .zip + "Install Co-op.bat" does the same. A message box
  appears when it cannot unpack, when PowerShell cannot be started or when the installer window
  did not start; the installer's own log is as above.

Troubleshooting and reports
- The installer stops: its log is <game>\coop\install-log.txt, or
  %TEMP%\timeshift-coop-install.log when it stopped before changing anything. To report it, double-click
  Collect logs.bat in the mod's folder (<game>\coop) and attach the zip from your desktop: it holds
  both logs (on a first install that stopped early there is no coop folder yet: attach the %TEMP% log).
- The game: the mod's log is bin\ts_coop.log; the logs of the four runs before it are
  ts_coop.1.log ... ts_coop.4.log (a run that never left the main menu is not kept).
- To report a problem: open the mod's GitHub page -> Issues -> New issue (a form asks for the version,
  the game edition, what happened and what you did) and attach the zip that
  <game>\coop\Collect logs.bat makes on your desktop (double-click it): the logs, the install record,
  hashes, and the system's graphics and Windows version. It removes your Windows user name, computer
  name, folder paths, player names, Steam names and IDs, e-mail and MAC addresses and public IP
  addresses, and uploads nothing; open the zip to check it before you attach it. A report from the
  host AND a client of the same session helps most.

Credits and license
- The mod: MIT license (LICENSE.txt), TimeShift Co-op contributors.
- The installer builds the mod's data with Python: python.org's embeddable CPython 3.12
  (coop\python, Python Software Foundation License, coop\python\LICENSE.txt).
- OpenSpy (openspy.net) keeps the GameSpy-era games online.
- TimeShift is a game by Saber Interactive, published by Sierra/Activision. This mod is not
  made or endorsed by them; you need your own copy of the game.
