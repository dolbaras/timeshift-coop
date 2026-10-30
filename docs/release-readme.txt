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
  chapter again from the checkpoint with your weapons, ammunition, health and time energy; friends join
  as usual. The save is per profile, is kept when you update the mod and removed when you uninstall it.
  Grenades are not saved. Where a chapter has no checkpoint marker to restore, it restarts from the
  start of that chapter.
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

Not finished / known limits
- Quads on the quad levels (9 and 12): the enemies' quads follow the host's positions on the clients'
  screens (checked by coordinates on level 9); a client driving a quad is not tested. The truck ride at
  the start of level 9 works: a client that is already in the game rides along (checked by log and
  coordinates).
- The lines Cooke and the rebels speak in the zeppelin intros (levels 8 and 23) are not heard on the
  clients' machines.
- Split screen: cutscenes, the pause menu and the loading screens are full screen for both; only
  player 1 uses turrets in local split screen; the scripts and the enemies treat player 1 as "the
  player"; each half is drawn every second frame, so each half runs at half of the game's frame rate;
  the shot sounds of player 2 are untested. Split screen on a client is experimental: its copy of
  player 2 was run in basic form only; player 2's time power, kills and pick-ups over the network were
  not checked in a live game yet.
- Most checks of the rewind, force fields, vehicles and container traps were made by reading clip and
  time values, coordinates and the logs of both machines, not by watching the screens: the results are
  in the numbers, but how it looks in play has had little human testing.
- Only the PC version of the game with patch 1.2 is supported. A Steam copy (version 1.0) works after
  it is brought to 1.2 with a community update package that you download yourself (see Requirements);
  that path was tested on a simulated Steam folder and with the package's files, not on a real Steam
  installation.
- Joining over the Internet (through OpenSpy and a forwarded port) is untested; LAN and a virtual
  LAN are what was tested. A whole campaign played by people from the first to the last level has
  not been done yet: the tests are automatic runs of the 24 levels with a host and a client.
- Please report anything that goes wrong (see "Troubleshooting and reports").

Requirements
- Your own copy of TimeShift with patch 1.2 (the main menu shows "Ver 1.02"). The mod is made for the
  code of the 1.2 bin\TimeShift.exe (SHA-256
  689c63da36cd5902edcad2abe1111d4697a92b12c4e17fe0659b856752641be4). The installer also accepts the
  community's pre-patched build of it (the "CPU fix" exe) and the exe of the community's Steam
  update: they carry the same code, and the mod does its own start-up fixes in memory, so those
  exes are not needed. An exe of another version is refused, changing nothing, and the installer
  says which exe it found (its SHA-256).
- Steam edition: Steam never received the 1.2 update, its game is version 1.0. The installer
  recognises a Steam folder (steamapps\common\TimeShift) whose exe is not 1.2 and asks for the community
  package "TimeShift Update 1.2 (Steam)" (a zip of about 11.7 MB). It is on PCGamingWiki (community
  files, "TimeShift Update 1.2 (Steam)"), on ModDB (TimeShift -> Downloads -> "Timeshift Steam Update
  1.2") and was first published as a Steam guide of the same name. The mod neither contains nor
  downloads it: download it yourself, then pick the zip (or the folder you extracted it to) in the
  installer's window; it also looks in your Downloads folder. The installer checks the package, keeps
  every file it replaces in <game>\coop-backup\steam10 and puts them back when you uninstall (the
  game is version 1.0 again). The package is third-party and unofficial: it replaces bin\TimeShift.exe,
  bin\steamwars.ini and the patch archives in preload\paks. Steam's "Verify integrity of game files"
  undoes it; run the installer again then. The game starts through Steam or through the mod's shortcut.
- Windows 10 or 11.
- Every player needs the same version of the mod.
- A second XInput gamepad for split screen.

Install
1. Extract the archive anywhere and run "Install Co-op.bat". It finds the game folder (or asks
   for it), builds the mod's data from your own game files (a few seconds; the mod contains no
   game files), keeps a copy of every file it replaces (in <game>\coop-backup) and installs the
   mod. Its window shows each step. Running it again reinstalls, updates or removes the mod.
   Its language is Windows' (English, Russian, German, French, Spanish or Italian).
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
  the chapter, the host's weapons, the main weapon's ammunition, health and time energy; at a chapter
  that has a restore marker also the place and the story state. Not restored: grenades and other
  weapons' ammunition, the clients' own loadouts (a client joins as a new player), the enemies' state
  after the checkpoint; a chapter without a marker starts from its beginning. Only the last
  checkpoint of the last session is kept.
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

Troubleshooting and reports
- The installer stops: its log is <game>\coop\install-log.txt, or
  %TEMP%\timeshift-coop-install.log when it stopped before changing anything.
- The game: the mod's log is bin\ts_coop.log; the logs of the four runs before it are
  ts_coop.1.log ... ts_coop.4.log (a run that never left the main menu is not kept).
- To report a problem: open the mod's GitHub page -> Issues -> New issue (a form asks for the version,
  the game edition, what happened and what you did) and attach the zip that
  <game>\coop\Collect logs.bat makes on your desktop: the logs, the install record, hashes, and the
  system's graphics and Windows version. It removes your Windows user name, computer name, player name
  and public IP addresses; open the zip to check it before you attach it. A report from the host AND
  a client of the same session helps most.

Credits and license
- The mod: MIT license (LICENSE.txt), TimeShift Co-op contributors.
- The installer builds the mod's data with Python: python.org's embeddable CPython 3.12
  (coop\python, Python Software Foundation License, coop\python\LICENSE.txt).
- OpenSpy (openspy.net) keeps the GameSpy-era games online.
- TimeShift is a game by Saber Interactive, published by Sierra/Activision. This mod is not
  made or endorsed by them; you need your own copy of the game.
