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
  leaves).
- Campaign rules for several players: lifts, trains and similar "everybody goes together" places
  wait for the other players and gather them; the zeppelin turrets and the armoured car ride of
  the campaign give every player a seat (or a place on the car); enemies, turrets and cannons
  that aim "at the player" aim at the nearest player; a death scene kills only the player who
  walked into it; the scanners of the quad levels count the host only.
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
- Co-op "Continue" and chapter saves of a co-op campaign are not there yet: the host starts any of the
  24 chapters from the Co-op menu; the host's own checkpoints and autosaves work.
- Quads on the quad levels (9 and 12): the enemies' quads follow the host's positions on the clients'
  screens (checked by coordinates on level 9); a client driving a quad is not tested.
- The lines Cooke and the rebels speak in the zeppelin intros (levels 8 and 23) are not heard on the
  clients' machines.
- Split screen: cutscenes, the pause menu and the loading screens are full screen for both; only
  player 1 uses turrets in local split screen; the scripts and the enemies treat player 1 as "the
  player"; each half is drawn every second frame, so each half runs at half of the game's frame rate;
  the shot sounds of player 2 are untested; player 2's time power on a client's split screen is not synced.
- Only the PC version of the game with patch 1.2 is supported; the Steam edition is not.
- Joining over the Internet (through OpenSpy and a forwarded port) is untested; LAN and a virtual
  LAN are what was tested. A whole campaign played by people from the first to the last level has
  not been done yet: the tests are automatic runs of the 24 levels with a host and a client.
- Please report anything that goes wrong (see Troubleshooting).

Requirements
- Your own copy of TimeShift with the official patch 1.2 (the main menu shows "Ver 1.02").
  The mod is made for bin\TimeShift.exe with SHA-256
  689c63da36cd5902edcad2abe1111d4697a92b12c4e17fe0659b856752641be4. The installer checks this and
  stops, changing nothing, when your exe is another version. Other editions (Steam) are not
  supported.
- Windows 10 or 11.
- Every player needs the same version of the mod.
- A second XInput gamepad for split screen.

Install
1. Extract the archive anywhere and run "Install Co-op.bat". It finds the game folder (or asks
   for it), builds the mod's data from your own game files (a few seconds; the mod contains no
   game files), keeps a copy of every file it replaces (in <game>\coop-backup) and installs the
   mod. Its window shows each step. Running it again reinstalls, updates or removes the mod.
   Its language is Windows' (English or Russian).
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
coop-installer.ps1 and "Uninstall Co-op.bat".
Uninstall: "Uninstall Co-op.bat" in the game folder - removes the mod, puts the replaced files
(the exe too) and the arcName setting back. Profiles and saves (UserConfigs) are not touched.

Network
- The host needs UDP port 7777 reachable by the other players: allow TimeShift in Windows
  Firewall (Windows asks the first time). Over the internet, forward UDP 7777 on the host's
  router to the host's PC, or play over a virtual LAN (Radmin VPN, ZeroTier...) and use the LAN
  list or the host's address there.
- Two players behind the same router join each other through the LAN list or the local address.

Troubleshooting
- The installer stops: its log is <game>\coop\install-log.txt, or
  %TEMP%\timeshift-coop-install.log when it stopped before changing anything.
- The game: the mod's log is bin\ts_coop.log.
- Please report problems with these logs on the mod's GitHub page (Issues).

Credits and license
- The mod: MIT license (LICENSE.txt), TimeShift Co-op contributors.
- The installer builds the mod's data with Python: python.org's embeddable CPython 3.12
  (coop\python, Python Software Foundation License, coop\python\LICENSE.txt).
- OpenSpy (openspy.net) keeps the GameSpy-era games online.
- TimeShift is a game by Saber Interactive, published by Sierra/Activision. This mod is not
  made or endorsed by them; you need your own copy of the game.
