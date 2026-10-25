TimeShift Co-op for TimeShift (2007, PC)
==============================================================================

Play the TimeShift story campaign together: 2-4 players over a local network or the internet,
on the game's own network code. The mod also makes the game start on today's PCs, adds gamepad
support and brings the game's own multiplayer back online without GameSpy (closed in 2014).

What the mod does
- Co-op: a "Co-op" entry in the main menu. The host picks a campaign level, the difficulty, the
  number of players (2-4), LAN or Internet and an optional password; the others join. Co-op
  servers are named [COOP]<host's name> and show the campaign level as their map.
- Joining: the game's LAN list, its Internet list or a direct connection by IP address.
  The Internet list and Internet hosting go through OpenSpy (a community replacement for the
  closed GameSpy servers); nothing to set up.
- In a co-op session: no friendly fire, no warm-up or ready step; the enemies move, fight and die
  the same on every machine. Every player needs the same version of the mod (a player with
  another version is told so and leaves).
- The game's own multiplayer works again: LAN list on every network adapter, Internet servers
  through OpenSpy.
- Today's PCs: the game starts with many CPU cores, with current graphics drivers and on GPUs
  that are neither NVIDIA nor ATI (Intel), with today's video memory sizes.
- Gamepads: Xbox-style (XInput) pads in the game and in its menus.
- Local split screen (experimental, for testers): two players on one PC, player 2 on the second
  pad. Off unless the environment variable TS_SPLIT=1 is set when the game starts.

Not finished in this version
- Time powers and story events in co-op are still being tested.
- Saves and "continue" for a co-op campaign, vehicles and turret levels with several players.

Requirements
- Your own copy of TimeShift with the official patch 1.2 (the main menu shows "Ver 1.02").
  The mod is made for bin\TimeShift.exe with SHA-256
  689c63da36cd5902edcad2abe1111d4697a92b12c4e17fe0659b856752641be4. The installer checks this and
  stops, changing nothing, when your exe is another version. Other editions (Steam) are not
  supported yet.
- Windows 10 or 11.
- Every player needs the same version of the mod.

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
there before is kept and put back), coop.s3darc, coop_ati.s3darc and coop_nv.s3darc in
preload\paks (built from your archives), [Preload] arcName in bin\steamwars.ini (coop.s3darc
instead of patch.s3darc), the coop folder, coop-installer.ps1 and "Uninstall Co-op.bat".
Uninstall: "Uninstall Co-op.bat" in the game folder - removes the mod, puts the replaced files
and the arcName setting back. Profiles and saves (UserConfigs) are not touched.

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
