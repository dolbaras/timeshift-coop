# Campaign scripts and "the one player": co-op audit

Scope: every place in the 24 campaign levels' scripts that assumes a single player - the global
`$player`, AI calls that name the player (`SetAim("player")` ...), the per-player systems the scripts
drive (`$tc` auto time mode, `$saves_mng`, level exits) - classified for a campaign co-op that runs on
the game's own multiplayer server.

Static analysis of the shipped data only (`out.s3darc` levels; `patch.s3darc` has no level files, so
the `out` versions are the final ones). Nothing was run. Tags as in the other documents:
**[V]** read in the data or the executable, **[L]** likely (consistent with the data, not traced to the
end), **[G]** hypothesis that needs a runtime test (section 8 lists the tests).

Model: `games/jericho/mod/docs/usrentity-audit.md` (same idea for Jericho's `Game.UsrEntity(0)`).

## 0. Short answers

| Question | Answer |
|---|---|
| How many single-player assumptions? | **630** script statements use `$player` in the 24 levels (after removing duplicates and demo-only code), plus **175** `$tc.SetMode` (auto time mode of the one player), **126** `AutoSave`, **235** AI calls that target the player by the name `"player"`, **23** level exits (`reload { nameScene }` domains). |
| Most common | `ShakeCamera` (262, cosmetic), `DisableTC` / time powers (108), `SetAim/SetInterest("player")` (235), input/movement locks and teleports (85). |
| Story-critical kinds | level exits, checkpoints/autosaves, mission failure, scripted kills, vehicle and seat sequences (zeppelin turrets, quads, armoured car, trains, lifts), input locks whose unlock comes from a host-only AI event. |
| Biggest gaps that need native code | (1) level change through the server instead of the single-player loader; (2) checkpoints and respawn near the team (the server writes no save and respawns at `start_pos`); (3) vehicles: the `vehicle` class is not network-synced and a remote player cannot drive; (4) mirroring host-side `$player` calls to each client's own `$player`; (5) AI and turrets that target `"player"` only see the host. |
| Pure script patches suffice for | "is any / are all players inside" tests (`IsBelong($player)` -> loop over `$player` and `@player_mp`, [G] that the loop sees remote players), per-player hazards (kill or teleport the player inside instead of failing the mission), skipping single-player-only staging on clients. |

Most important rules (details in section 5):

1. **Story scripts are host-authoritative.** On a client, handlers that change the world (unlock, spawn,
   AI behaviour, saves, exits, failure) must not run; the host runs them. Presentation (camera shake,
   light zones, sound, hints) may run locally on every machine.
2. **Every `$player` call made by a host story script is applied to every player** (mirrored to each
   client's own `$player`), except per-zone calls (holster in vents, fall height, container traps), which
   apply only to the player inside, and seat calls (`StartUse`), which only the host (or one player)
   takes.
3. **Triggers: the first player to arrive starts the story; gates that move the team (lifts, trains,
   level exits, vehicle boarding) wait for all living players or teleport the stragglers.**
4. **Mission failure is a team failure (restart from the last checkpoint); player death is not**
   (multiplayer respawn, moved to the last checkpoint or next to a living teammate).
5. **Seats and vehicles: one player drives or mans the scripted seat; the others take free seats
   (zeppelin turrets) or ride along as spectators** until the sequence ends, then are teleported.

## 1. What co-op changes

The mod runs the campaign level on the game's own server (`mod/docs/multiplayer.md` section 8; netcode in
`work/re/netcode.md`):

- **Host** (mode 2): `$player` is the host's own campaign `player` object. Remote players are `player_mp`
  objects created by the server, owned and moved by their clients [V]. The host has authority: AI
  decisions, triggers that depend on authority, deaths and physics run here [V for the authority bit,
  L for "everything"].
- **Client** (mode 3): `$player` is that client's own `player`; every other entity is a proxy without
  authority [V]. **Level scripts run on every machine; no script call is sent over the network** [V: none
  of the 406 natives sends]. A client therefore runs the same `OnInitLevel`, domain and cinematic
  handlers locally, against its own `$player`, while AI behaviour events (`OnBhvEnd`, `OnAlert`, ...)
  that need authority fire on the host only [L].
- **Name `"player"`**: AI natives take an entity name. On the host, `"player"` names the host's own
  `player`; remote players are `player_mp` entities with other names [L]. So `SetTarget("player")` aims
  at the host only.
- **Domains (trigger volumes)**: a domain's enter/leave test (`0x6c2dd0`) checks the points of one object
  stored in the domain (`+0xf8`, set when the domain is created, `0x6c3720`) [V]; `IsBelong(ent)`
  (`0x6c3d60`) tests the position of any entity [V]. Which entities set off `OnEnter` (only the local
  `player` of each machine, or every character, or every player) was not traced [G]. Hints both ways:
  the multiplayer maps use `OnEnter` only for per-viewer effects (light zones, EAX reverb, ambient
  sound), which fits "local player only"; some campaign `OnEnter` handlers test
  `IsBelong($player)` again, which fits "anyone" (`QGS_event_dom`). The rules below hold in both cases;
  test D1 (section 8) decides how much native help triggers need.
- **Saves**: on the server `$saves_mng.AutoSave(n)` writes nothing [V, observed]. **Death** follows the
  multiplayer rules: respawn after a few seconds at a `start_pos*` object [V, observed]; every campaign
  level has only the one `start_pos` at the level start [V].
- **Level change**: `$ui.LoadLevel` on a running server ends the session [V, observed]; the campaign
  exits (`dom_reload` domains) use the single-player level change [L].
- **Dedicated server** (mode 4) has no local `player`; every `$player` statement would fail there. The
  campaign needs a listen server (mode 2) [L].

## 2. Method

- Tool: `tools/s3darc.py lg-ssl extract` on the 24 levels (all object texts with a `#ssl` or
  `#ssl_begin` section), then a classifier over every statement (comments, `#ifdef _DEMO` branches and
  `#ifdef _LOW_VIOLENCE` branches dropped; the same statement in the same handler of an object and its
  template instance counted once).
- **Script owners** come in three chunk kinds of the `.lg` file; `lg-ssl list` names only the first, the
  other two show as `?` (their names were read here from the chunk headers):

  | Chunk | Owner | Name stored in | Scripts |
  |---|---|---|---|
  | `0xF0` > `0xFD` > `0x1BA` | level object (domains, buttons, doors, ...) | `0xF4` child | 1 582 |
  | `0x1EA` > `0x2E4` > `0x1BA` | animated actor / cinematic (`cinema`, `dynamic`) | `0x2E5` child: `"LPTA"` + name + type | 960 |
  | `0x1B8` > `0x1B9` > `0x1BA` | placed character or entity (AI, `start_pos`, `Check_Point_N`, buttons) | `0x1B9` header: `"SNIA"` + name + class | 3 593 |

- **Kinds** (the task's groups; one statement counts once, in the first matching kind):

  | Kind | Statements |
  |---|---|
  | a | triggers/zones: `IsBelong($player)`, `user = $player` (the domain `OnEnter` handlers themselves are counted separately) |
  | b | input/movement/staging: `DisableInput`, `DisableMove`, `Transport`, `StartUse` (non-vehicle), `DisableUse/EnableUse`, `Aimable`, `hide/show` |
  | c | time powers: `DisableTC`, `StartTControl/StopTControl`, `SetTStopLength`, `ClearTimeReverse`, `SetReverseAvailThreshold`, `SetFixedTimeTR`, `$tc.SetMode` |
  | d | checkpoints: `$saves_mng.AutoSave(n)`, `$ui.AutoSave(n)` |
  | e | failure/death: `FailMission`, `Kill`, `SetDeathFallHeight`, `SetGOD`, `SetHealthRate` |
  | f | camera/effects on the player: `ShakeCamera`, `StartFastPhysics` |
  | g | weapons: `SetupWeapon`, `SelectHolster`, `ClearGrenades`, NPC `SetWpnByPlayer` |
  | h | vehicles/seats: `InBike`, `Transport`/`StartUse` into a vehicle or turret |
  | i | AI on the player: `SetAim/SetInterest/SetTarget/EndAim("player")` |
  | j | other: `ShowSAMMsg`, `ShowMsg` (hints and SAM messages), one `ShakeCamera4` |

  Not in the kinds but related (they matter for the rules): `$ui.ShowTimeHint` 104 (time-power hints),
  objectives 43 through `$ui.Objective*` and about 217 through `$sw_objective_sys.Objective*`, screen
  effects `$effect.*` and cinematic `SetCamFollow` 69, FMV `$fmv.Play` 21, end credits 1.

## 3. Counts per level (campaign order)

Order = `gs_map_list.cls` `singleplayer` ids 30-53. "Scripts" = object texts with SSL; "OnEnter" = domain
`OnEnter` handlers (implicit player triggers); "OnEye" = look triggers (`look_trigger` objects that fire
when the player looks at a point); "exit" = `reload { nameScene }` domains.

| # | Level | Scripts | OnEnter | OnEye | a | b | c | d | e | f | g | h | i | j | exit |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `ts2_city_part01` | 544 | 87 | 7 | 9 | 4 | 5 | 7 | 0 | 85 | 4 | 0 | 24 | 12 | 1 |
| 2 | `ts2_city_part02` | 571 | 92 | 5 | 0 | 2 | 29 | 8 | 1 | 45 | 1 | 0 | 30 | 11 | 2 |
| 3 | `ts2_city_part03` | 171 | 42 | 0 | 0 | 4 | 9 | 6 | 0 | 0 | 1 | 0 | 15 | 0 | 1 |
| 4 | `ts2_skyscraper_part1` | 286 | 55 | 2 | 2 | 1 | 4 | 5 | 0 | 1 | 0 | 0 | 6 | 0 | 1 |
| 5 | `ts2_skyscraper_part2` | 250 | 81 | 0 | 0 | 3 | 24 | 5 | 0 | 0 | 6 | 0 | 21 | 1 | 1 |
| 6 | `ts2_skyscraper_part3` | 215 | 55 | 2 | 0 | 0 | 3 | 5 | 3 | 3 | 0 | 0 | 3 | 0 | 1 |
| 7 | `ts2_skyscraper_part4` | 230 | 40 | 1 | 1 | 2 | 12 | 6 | 0 | 2 | 2 | 0 | 6 | 0 | 1 |
| 8 | `ts2_zepride1_part01` | 128 | 6 | 0 | 2 | 2 | 6 | 5 | 1 | 1 | 3 | 10 | 0 | 0 | 1 |
| 9 | `ts2_zepcrashsite_part01` | 298 | 57 | 0 | 0 | 3 | 18 | 9 | 3 | 4 | 0 | 2 | 6 | 0 | 1 |
| 10 | `ts2_zepcrashsite_part02` | 272 | 77 | 0 | 0 | 5 | 10 | 7 | 0 | 3 | 0 | 0 | 11 | 3 | 1 |
| 11 | `ts2_zepcrashsite_part03` | 139 | 33 | 0 | 6 | 1 | 11 | 3 | 4 | 1 | 0 | 0 | 1 | 0 | 1 |
| 12 | `ts2_summeroutdoor_part01` | 329 | 36 | 0 | 0 | 2 | 6 | 5 | 3 | 28 | 0 | 0 | 0 | 0 | 1 |
| 13 | `ts2_summeroutdoor_part02` | 129 | 26 | 0 | 0 | 7 | 10 | 4 | 0 | 0 | 0 | 1 | 4 | 1 | 1 |
| 14 | `ts2_guardfactory_part01` | 154 | 41 | 0 | 3 | 3 | 22 | 6 | 1 | 0 | 1 | 0 | 7 | 0 | 1 |
| 15 | `ts2_guardfactory_part02` | 236 | 54 | 0 | 17 | 0 | 6 | 3 | 14 | 2 | 0 | 0 | 5 | 0 | 1 |
| 16 | `ts2_guardfactory_part03` | 279 | 53 | 0 | 0 | 0 | 12 | 4 | 0 | 0 | 2 | 0 | 27 | 0 | 1 |
| 17 | `ts2_guardfactory_part04` | 233 | 49 | 0 | 1 | 1 | 11 | 5 | 0 | 6 | 2 | 0 | 5 | 0 | 1 |
| 18 | `ts2_guardfactory_part05` | 333 | 52 | 0 | 7 | 3 | 11 | 5 | 5 | 21 | 0 | 0 | 6 | 0 | 1 |
| 19 | `ts2_zep_factory_part1` | 220 | 33 | 0 | 1 | 11 | 16 | 6 | 0 | 1 | 4 | 2 | 7 | 0 | 1 |
| 20 | `ts2_zep_factory_part2` | 175 | 17 | 1 | 4 | 2 | 13 | 4 | 4 | 2 | 3 | 0 | 8 | 0 | 1 |
| 21 | `ts2_zep_factory_part3` | 176 | 37 | 0 | 0 | 1 | 10 | 4 | 1 | 5 | 1 | 0 | 17 | 0 | 1 |
| 22 | `ts2_zep_factory_part4` | 215 | 33 | 0 | 0 | 4 | 5 | 3 | 0 | 0 | 3 | 1 | 5 | 0 | 1 |
| 23 | `ts2_zepride2_part01` | 223 | 45 | 0 | 0 | 14 | 22 | 6 | 1 | 0 | 7 | 2 | 14 | 0 | 1 |
| 24 | `ts2_cityfinal_part01` | 329 | 44 | 3 | 0 | 10 | 8 | 5 | 0 | 52 | 2 | 0 | 7 | 0 | 0 |
| | **Total** | 6 135 | 1 145 | 21 | **53** | **85** | **283** | **126** | **41** | **262** | **42** | **18** | **235** | **28** | 23 |

Of kind c, 175 are `$tc.SetMode` and 108 are `$player` calls. The final level has no exit domain: the
campaign ends with `FMV_Manager` (FMVs 15-17, then `$ui.ShowCredits(true)`).

## 4. Per-level tables

One row per kind; each cell lists the script owners (object, cinematic actor or placed character), the
handler in brackets and the calls (`$player.` omitted; `xN` = N statements). Long lists are cut after a
few owners ("... N more owners"); the counts in brackets are complete.

#### 1. `ts2_city_part01`

| Kind | Script owners (handler: calls) |
|---|---|
| a (9) | `door_start` (OnClose: IsBelong SM_dom_StartRoom)<br>`dom_KRO` (TRAX: IsBelong dom_KRO)<br>`reb_revive_a` (VO1: IsBelong dom_revive; VO2: IsBelong dom_revive; VO3: IsBelong dom_revive; VO4: IsBelong dom_revive; VO5: IsBelong dom_revive; VO6: IsBelong dom_revive; VO7: IsBelong dom_revive) |
| b (4) | `dom_act_weapon` (OnEnter: DisableMove(), DisableMove(0))<br>`dom_act_begin` (OnEnter: Aimable)<br>`start_cine` (OnAnimEnd(CLIP_1): Transport(start_cine_player_place)) |
| c (5) | `STRIDER\|Spider` (OnAnimEnd(CHARGE_CITY1): StartTControl(tslow), StopTControl(tslow))<br>`start_pos` (OnInitLevel: DisableTC(time_slow,1), DisableTC(time_stop,1), DisableTC(time_reverse,1)) |
| d (7) | `dom_in_room` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_act_Zasada` (OnEnter: $saves_mng.AutoSave(4))<br>`dom_roof_heli` (OnEnter: $saves_mng.AutoSave(5))<br>`dom_rooms_farm` (OnEnter: $saves_mng.AutoSave(6))<br>`dom_farm_kanava` (OnEnter: $saves_mng.AutoSave(7))<br>`start_cine` (OnAnimEnd(CLIP_1): $saves_mng.AutoSave(1))<br>`Rebel_hole_2` (OnBhvEnd(WAIVE): $saves_mng.AutoSave(3)) |
| f (85) | `dom_start` (Shoot: ShakeCamera x5)<br>`dom_act_hole` (OnEnter: ShakeCamera x3)<br>`dom_act_Zasada` (Explode: ShakeCamera)<br>`domHEL_SHUTING` (SHUT: ShakeCamera x34)<br>... 12 more owners |
| g (4) | `Rebel_weaponer` (OnBhvEnd(GIVE): SetupWeapon(4,0,0), SelectHolster)<br>`start_pos` (OnInitLevel: SetupWeapon(0,0,0), SelectHolster) |
| i (24) | `dom_REB_hole1` (OnEnter: $REB_hole1.SetInterest)<br>`dom_keepdown` (OnEnter: $Rebel_hole_2.SetInterest)<br>`dom_act_weapon` (OnEnter: $Rebel_weaponer.SetInterest)<br>`dom_act_Zasada` (Explode: $House_Cop1.SetAim, $House_Cop2.SetAim; OnEnter: $Rebel_ladder.SetInterest)<br>... 16 more owners |
| j (12) | `dom_cw_cop1` (OnEnter: ShowSAMMsg)<br>`dom_violence_start` (OnEnter: ShowSAMMsg)<br>`dom_farm_kanava` (OnEnter: ShowSAMMsg)<br>... 8 more owners |

#### 2. `ts2_city_part02`

| Kind | Script owners (handler: calls) |
|---|---|
| b (2) | `cine_start` (OnInterrupted: Transport(start_pos))<br>`start_pos` (OnInitLevel: Transport(start_pos)) |
| c (29) | `zaval_show_dom` (OnEnter: DisableTC(time_reverse,1))<br>`dom_phrase_trench_rebel` (OnEnter: ClearTimeReverse)<br>`dom_act_tstop_water` (OnEnter: SetTStopLength(150); OnLeave: SetTStopLength(100))<br>`tc_domain_bridge` (OnEnter: $tc.SetMode(reverse); OnLeave: $tc.SetMode(slow))<br>`tc_domain_water` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`dom_tc_after_bridge` (OnEnter: $tc.SetMode(slow))<br>`Hint_pipe1` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`building_site_rocket_6` (OnActionFrame("FLY",1): ClearTimeReverse, DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>... 6 more owners |
| d (8) | `dom_act_bridge_autosave` (OnEnter: $saves_mng.AutoSave(3))<br>`WOW_barrels_dom` (OnEnter: $saves_mng.AutoSave(6))<br>`dom_start_fake_building_battle` (OnEnter: $saves_mng.AutoSave(5))<br>`dom_autosave_2` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_kill_mech_and_street_guys1` (OnEnter: $saves_mng.AutoSave(4))<br>`autosave_domian_after_tube_puzzle` (OnEnter: $saves_mng.AutoSave(7))<br>`dom_act_voice_overs` (OnEnter: $saves_mng.AutoSave(8))<br>`Save_unlocker` (OnUnlocked: $saves_mng.AutoSave(1)) |
| e (1) | `final_room_turret_hmg` (OnOccupied: SetHealthRate) |
| f (45) | `dom_act_unlocker` (OnEnter: StartFastPhysics)<br>`ANIM_GROUP` (OnActionFrame("ACTION",1): ShakeCamera x4; OnActionFrame("ACTION",2): ShakeCamera x3; OnActionFrame("ACTION",3): ShakeCamera; OnActionFrame("ACTION",4): ShakeCamera; OnActionFrame("ACTION",5): ShakeCamera; OnActionFrame("ACTION",6): ShakeCamera; OnActionFrame("ACTION",7): ShakeCamera; OnActionFrame("ACTION",8): ShakeCamera)<br>`bronik_crashing_the_wall_plc` (OnActionFrame("MOVE",1): ShakeCamera x2)<br>`ANIM_UGOL` (OnActionFrame("ANIM",1): ShakeCamera; OnActionFrame("ANIM",2): ShakeCamera)<br>... 5 more owners |
| g (1) | `start_pos` (OnInitLevel: SetupWeapon(0,0,0)) |
| i (30) | `dom_ai_forth_room_end_aim` (OnEnter: $ai_house_forth_room_1.SetAim, $ai_house_forth_room_2.SetAim, $ai_house_forth_room_3.SetAim)<br>`dom_farm_assault_guy` (OnEnter: $ai_building_site_last_4.SetAim)<br>`dom_ai_first` (OnInit: SetAim)<br>`dom_ai_first1` (OnInit: SetAim)<br>... 22 more owners |
| j (11) | `dom_farm_fourth_room1` (OnEnter: ShowSAMMsg)<br>`zaval_show_dom` (OnEnter: ShowSAMMsg)<br>`dom_start_firs_interceptors_group1` (OnEnter: ShowSAMMsg)<br>... 8 more owners |

#### 3. `ts2_city_part03`

| Kind | Script owners (handler: calls) |
|---|---|
| b (4) | `Check_Point_3` (OnUnlocked: Transport(Check_Point_3))<br>`Check_Point_2` (OnUnlocked: Transport(Check_Point_2))<br>`Check_Point_1` (OnUnlocked: Transport(Check_Point_1))<br>`start_pos` (OnInitLevel: Transport(start_pos)) |
| c (9) | `first_puzzle_domain` (OnEnter: $tc.SetMode(stop))<br>`first_puzzle_ending_domain` (OnEnter: $tc.SetMode(slow))<br>`second_puzzle_domain` (OnEnter: $tc.SetMode(stop))<br>`second_puzzle_domain1` (OnEnter: $tc.SetMode(slow))<br>`third_puzzle_domain` (OnEnter: $tc.SetMode(stop))<br>`ai_square_rebel_1` (OnBhvEnd(RUN_TO_FIGHT): DisableTC(time_reverse,0))<br>`metro_tank_actor` (OnUnlocked: DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`start_pos` (OnInitLevel: DisableTC(time_reverse,1)) |
| d (6) | `dom_autosave` (OnEnter: $saves_mng.AutoSave(3))<br>`dom_dlg` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_act_rebels_start` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_SSAM_Fire` (OnEnter: $saves_mng.AutoSave(3))<br>`ai_square_rebel_1` (OnBhvEnd(RUN_TO_FIGHT): $saves_mng.AutoSave(1))<br>`Desc_Opp_back_right` (OnInit: $saves_mng.AutoSave(4)) |
| g (1) | `start_pos` (OnInitLevel: SelectHolster) |
| i (15) | `Metro_Tank_Zone` (OnEnter: $Metro_Tank.SetTarget(player,0), $Metro_Tank.SetTarget(player,1))<br>`Metro_Tank_PLACE_OLD` (OnActionFrame("MOVE",10): $Metro_Tank.SetTarget(player,0), $Metro_Tank.SetTarget(player,1))<br>`Metro_Tank_PLACE` (OnActionFrame("MOVE",10): $Metro_Tank.SetTarget(player,0), $Metro_Tank.SetTarget(player,1))<br>`metro_soldier_3` (OnInit: SetAim)<br>... 8 more owners |

#### 4. `ts2_skyscraper_part1`

| Kind | Script owners (handler: calls) |
|---|---|
| a (2) | `opendoor8` (OnClose: IsBelong Stroyka_1_domain; OnOpen: IsBelong Stroyka_1_domain) |
| b (1) | `start_pos` (OnInitLevel: Transport(player_transport_wp)) |
| c (4) | `time_control_domain_first_puzzle` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`time_control_domain_second_puzzle` (OnEnter: $tc.SetMode(stop))<br>`time_control_domain_second_puzzle_ending` (OnEnter: $tc.SetMode(slow)) |
| d (5) | `dom_pusk_broneviki_vo_dvore1` (OnEnter: $saves_mng.AutoSave(5))<br>`dom_cookes_transmission` (OnEnter: $saves_mng.AutoSave(3))<br>`dom_autosave1` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_act_rebel_start` (OnEnter: $saves_mng.AutoSave(4))<br>`dom_dialog_rebel_mech` (OnEnter: $saves_mng.AutoSave(2)) |
| f (1) | `falling_plate` (OnActionFrame("FALL",1): ShakeCamera) |
| i (6) | `dom_dialog_rebel_mech` (OnEnter: $ai_rebel_mech_scene.SetInterest)<br>`s3d_refLocator28\|ai_sniper` (OnInit: SetAim)<br>`ai_rebel_mech_scene` (OnInit: SetInterest)<br>`ai_building_site_enter_7` (OnInit: SetAim)<br>... 2 more owners |

#### 5. `ts2_skyscraper_part2`

| Kind | Script owners (handler: calls) |
|---|---|
| b (3) | `fmv_actor` (OnActionFrame("FMV",1): DisableInput(1); OnActionFrame("FMV",3): DisableInput(0))<br>`start_pos` (OnInitLevel: Transport(start_pos)) |
| c (24) | `s3d_refLocator18` (OnPushed: $tc.SetMode(stop), $tc.SetMode(slow); OnPoped: $tc.SetMode(stop), $tc.SetMode(slow))<br>`but_laser_fence` (OnPushed: DisableTC(time_reverse,0))<br>`dom_worker_alert` (OnEnter: DisableTC(time_reverse,1))<br>`dom_hint_swing` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`dom_tc` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`dom_hint_propeller1` (OnEnter: $tc.SetMode(reverse); OnLeave: $tc.SetMode(slow))<br>`dom_hint_trap1` (OnEnter: $tc.SetMode(puzzle_reverse_once); OnLeave: $tc.SetMode(slow))<br>`dom_hint_trap2` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>... 4 more owners |
| d (5) | `dom_avtosave0` (OnEnter: $saves_mng.AutoSave(0))<br>`dom_autosave1` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_avtosave4` (OnEnter: $saves_mng.AutoSave(4))<br>`dom_obj2` (OnEnter: $saves_mng.AutoSave(3))<br>`dom_objective_3` (OnEnter: $saves_mng.AutoSave(1)) |
| g (6) | `dom_holster` (OnEnter: SelectHolster; OnLeave: SelectHolster)<br>`dom_holster1` (OnEnter: SelectHolster; OnLeave: SelectHolster)<br>`dom_take_give_gu2` (OnEnter: SelectHolster; OnLeave: SelectHolster) |
| i (21) | `ai_guard2` (OnInit: SetInterest; OnBhvEnd(stand): EndInterest, SetInterest)<br>`act_alarm1` (OnUnlocked: $ai_guard1.SetInterest)<br>`ai_passage3` (OnAlert: $ai_passage2.SetAim)<br>`ai_passage2` (OnAlert: $ai_passage1.SetAim)<br>... 9 more owners |
| j (1) | `dom_worker_alert` (OnEnter: ShowSAMMsg) |

#### 6. `ts2_skyscraper_part3`

| Kind | Script owners (handler: calls) |
|---|---|
| c (3) | `cabin1` (OnActionFrame("FALL",3): $tc.SetMode(stop))<br>`start_pos` (OnInitLevel: $tc.SetMode(slow))<br>`data_console_1\|button` (OnPushed: ClearTimeReverse) |
| d (5) | `s3d_refLocator8` (OnPushed: $saves_mng.AutoSave(4))<br>`hall_fight` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_cooke_line` (OnEnter: $saves_mng.AutoSave(3))<br>`__renamed123` (OnPushed: $saves_mng.AutoSave(4))<br>`Door_opener_actor` (OnUnlocked: $saves_mng.AutoSave(2)) |
| e (3) | `Fall_domain` (OnEnter: SetDeathFallHeight(3))<br>`Fall_domain1` (OnEnter: SetDeathFallHeight(30))<br>`Fall_domain2` (OnEnter: SetDeathFallHeight(30)) |
| f (3) | `cabin1` (OnActionFrame("FALL",10): ShakeCamera; OnActionFrame("FALL",1): ShakeCamera)<br>`explosion` (OnActionFrame("Anim",2): ShakeCamera) |
| i (3) | `ai_guard64` (OnInit: SetAim)<br>`ai_guard46` (OnInit: SetAim)<br>`ai_comp_room_3` (OnInit: SetAim) |

#### 7. `ts2_skyscraper_part4`

| Kind | Script owners (handler: calls) |
|---|---|
| a (1) | `lift_door` (OnAnimEnd(CLOSE): IsBelong dom_act_lift1) |
| b (2) | `Cine_FIN_Actor` (OnAnimBegin(CLIP_1): Transport(fin_pos))<br>`start_pos` (OnInitLevel: Transport(Init_point_actor)) |
| c (12) | `tc_domain_1` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`tc_domain_2` (OnEnter: $tc.SetMode(slow))<br>`ai_gunner5` (OnComplete: DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`ai_gunner4` (OnComplete: $tc.SetMode(slow), DisableTC(time_reverse,1) x2)<br>`ai_gunner3` (OnComplete: DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`ai_gunner2` (OnComplete: DisableTC(time_reverse,1))<br>`ai_troop1_7` (OnComplete: DisableTC(time_reverse,0)) |
| d (6) | `dom_lift__move_up` (OnEnter: $saves_mng.AutoSave(6))<br>`dom_reach_obj1` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_reach_obj2` (OnEnter: $saves_mng.AutoSave(3))<br>`ai_gunner5` (OnComplete: $saves_mng.AutoSave(4))<br>`ai_gunner4` (OnComplete: $saves_mng.AutoSave(5))<br>`ai_gunner2` (OnComplete: $saves_mng.AutoSave(2)) |
| f (2) | `Fly_anim_actor_3` (OnActionFrame("FLY",2): ShakeCamera)<br>`Fly_anim_actor_4` (OnActionFrame("FLY",2): ShakeCamera) |
| g (2) | `Cine_FIN_Actor` (OnUnlocked: SetWpnByPlayer)<br>`ai_swift` (OnInit: SetWpnByPlayer) |
| i (6) | `helicopt_plc` (OnActionFrame("FLY",1): $helicopter_actor\|tur01.SetTarget(player))<br>`policeman5` (OnBhvEnd(RUN): EndAim)<br>`policeman4` (OnBhvEnd(RUN): EndAim)<br>`policeman3` (OnBhvEnd(RUN): EndAim)<br>... 2 more owners |

#### 8. `ts2_zepride1_part01`

| Kind | Script owners (handler: calls) |
|---|---|
| a (2) | `Zeppelin\|Zeppelin_cruiser_ACTOR` (OnOccupied: user = $player; OnReleased: user = $player) |
| b (2) | `PreStart_cine` (OnAnimEnd(CLIP_1): DisableMove(1))<br>`Zeppelin_cruiser_Intro_plc` (OnActionFrame("MOVE",10): DisableMove(0)) |
| c (6) | `PreStart_cine` (OnAnimEnd(CLIP_1): DisableTC(time_slow,1), DisableTC(time_stop,1), DisableTC(time_reverse,1))<br>`Zeppelin_cruiser_Intro_plc` (OnAnimEnd(MOVE): DisableTC(time_slow,0), DisableTC(time_stop,0), DisableTC(time_reverse,0)) |
| d (5) | `Zeppelin_cruiser_plc2` (OnActionFrame("MOVE2",4): $saves_mng.AutoSave(3))<br>`Cine1_ACTOR` (OnAnimEnd(CLIP_3): $saves_mng.AutoSave(1))<br>`Cine2_ACTOR` (OnAnimEnd(CLIP_5): $saves_mng.AutoSave(2))<br>`Cine3_ACTOR` (OnAnimEnd(CLIP_2): $ui.AutoSave(2))<br>`Zeppelin\|complete_puzzle_actor` (OnUnlocked: $ui.AutoSave(2)) |
| e (1) | `Zepp_Destr_Cine_ACTOR` (OnActionFrame("CLIP_1",9): FailMission(GAME_ZEP_DESTROYED)) |
| f (1) | `Zeppelin_cruiser_Intro_plc` (OnActionFrame("MOVE",8): ShakeCamera) |
| g (3) | `PreStart_cine` (OnAnimEnd(CLIP_1): SelectHolster)<br>`Zeppelin_cruiser_Intro_plc` (OnActionFrame("MOVE",10): SelectHolster)<br>`start_pos` (OnInitLevel: ClearGrenades) |
| h (10) | `PreStart_cine` (OnAnimEnd(CLIP_1): Transport(Zeppelin_cruiser_Intro_plc/player_place))<br>`Cine1_ACTOR` (OnAnimEnd(CLIP_3): StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur02))<br>`Check_Point_8` (OnUnlocked: StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur05))<br>`Check_Point_7` (OnUnlocked: StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur05))<br>`Check_Point_6` (OnUnlocked: StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur05))<br>`Check_Point_5` (OnUnlocked: StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur02))<br>`Check_Point_4` (OnUnlocked: StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur02))<br>`Check_Point_3` (OnUnlocked: StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur05))<br>`Check_Point_2` (OnUnlocked: StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur05))<br>`Check_Point_1` (OnUnlocked: StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur05)) |

#### 9. `ts2_zepcrashsite_part01`

| Kind | Script owners (handler: calls) |
|---|---|
| b (3) | `truck_place` (OnAnimEnd(MOVE): Aimable)<br>`start_pos` (OnInitLevel: Aimable, Transport(start_place)) |
| c (18) | `dom_farm_forest` (OnEnter: $tc.SetMode(stop))<br>`dom_clear` (OnEnter: $tc.SetMode(stop))<br>`dom_reach_np2` (OnEnter: $tc.SetMode(slow))<br>`dom_reach_np7` (OnEnter: $tc.SetMode(slow))<br>`dom_time_zone3` (OnEnter: $tc.SetMode(puzzle_reverse_once))<br>`dom_time_zone4` (OnEnter: $tc.SetMode(puzzle_reverse_once))<br>`dom_time_zone5` (OnEnter: $tc.SetMode(slow))<br>`dom_time_zone6` (OnEnter: $tc.SetMode(puzzle_reverse_once))<br>... 9 more owners |
| d (9) | `dom_reach_np1` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_reach_np2` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_reach_np5` (OnEnter: $saves_mng.AutoSave(5))<br>`dom_music_save` (OnEnter: $saves_mng.AutoSave(8))<br>`dom_save1` (OnEnter: $saves_mng.AutoSave(7))<br>`dom_time_zone10` (OnEnter: $saves_mng.AutoSave(9))<br>`dom_AutoSave1` (OnEnter: $saves_mng.AutoSave(6))<br>`dom_AutoSave` (OnEnter: $saves_mng.AutoSave(3))<br>... 1 more owners |
| e (3) | `actor_checker_1` (OnActionFrame("CHECK",1): FailMission(UI_OBJECTIVES_NO_TRIKE))<br>`actor_checker_2` (OnActionFrame("CHECK",1): FailMission(UI_OBJECTIVES_NO_TRIKE))<br>`actor_checker` (OnActionFrame("CHECK",1): FailMission(UI_OBJECTIVES_NO_TRIKE)) |
| f (4) | `jet3_place` (OnAnimEnd(FLY): ShakeCamera)<br>`jet1_place` (OnActionFrame("FLY",1): ShakeCamera)<br>`jet1_place2` (OnActionFrame("FLY",1): ShakeCamera)<br>`s3d_gate_destr\|Actor` (OnDestroyed: ShakeCamera) |
| h (2) | `dom_hint_boost` (OnEnter: InBike)<br>`dom_hint_boost1` (OnEnter: InBike) |
| i (6) | `rebel_2` (OnInit: SetInterest)<br>`rebel_1` (OnInit: SetInterest)<br>`first_grp_wake_up` (OnUnlocked: $first_grp_soldier_1.SetAim, $first_grp_soldier_2.SetAim, $first_grp_soldier_3.SetAim)<br>`forest_camp_alarm` (OnUnlocked: $forest_soldier_10.SetAim) |

#### 10. `ts2_zepcrashsite_part02`

| Kind | Script owners (handler: calls) |
|---|---|
| b (5) | `Reload_domen` (OnEnter: DisableMove(0))<br>`actor_fmv` (OnActionFrame("FMV",1): DisableInput(1); OnActionFrame("FMV",3): DisableInput(0))<br>`train_button` (OnPushed: Transport(train_player_wp), DisableMove(1)) |
| c (10) | `bridge_button` (OnPushed: DisableTC(time_reverse,1))<br>`dom_unlock_lift1` (OnEnter: $tc.SetMode(slow))<br>`time_domain1` (OnEnter: $tc.SetMode(slow))<br>`time_domain` (OnEnter: $tc.SetMode(reverse))<br>`time_domain2` (OnEnter: $tc.SetMode(stop))<br>`time_domain3` (OnEnter: $tc.SetMode(slow))<br>`acktor_revers_1` (OnUnlocked: $tc.SetMode(puzzle_reverse_once))<br>`acktor_revers` (OnUnlocked: $tc.SetMode(reverse), DisableTC(time_reverse,0))<br>... 1 more owners |
| d (7) | `dom_hint_propeller` (OnEnter: $saves_mng.AutoSave(5))<br>`dom_AutoSave1` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_AutoSave2` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_AutoSave4` (OnEnter: $saves_mng.AutoSave(4))<br>`dom_AutoSave7` (OnEnter: $saves_mng.AutoSave(7))<br>`train_grp_wakeup` (OnUnlocked: $saves_mng.AutoSave(6))<br>`idiot_2` (OnBhvEnd(run): $saves_mng.AutoSave(3)) |
| f (3) | `FallingCornice_actor` (OnAnimBegin(FALLING): ShakeCamera)<br>`Actor_TELEGA` (OnActionFrame("MOVE",1): ShakeCamera)<br>`Fly_actor1` (OnActionFrame("FLY",1): ShakeCamera) |
| i (11) | `dom_stend_worker` (OnEnter: $snipet_tower\|ai_sniper.SetAim)<br>`booth_guard` (OnInit: SetInterest)<br>`snipet_tower\|ai_sniper` (OnInit: SetAim)<br>`sniper_3` (OnInit: SetAim; OnAlert: EndAim)<br>... 4 more owners |
| j (3) | `cooke_voice_domain` (OnEnter: ShowSAMMsg)<br>`Actor_TELEGA` (OnActionFrame("MOVE",1): ShowSAMMsg)<br>`start_pos` (OnInitLevel: ShowSAMMsg) |

#### 11. `ts2_zepcrashsite_part03`

| Kind | Script owners (handler: calls) |
|---|---|
| a (6) | `group816` (OnClose: IsBelong sm_lamp2_domain; OnOpen: IsBelong sm_lamp2_domain)<br>`clonecontainer_3\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_3\|dom_mis_fail)<br>`clonecontainer_4\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_4\|dom_mis_fail)<br>`clonecontainer_6\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_6\|dom_mis_fail)<br>`clonecontainer_7\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_7\|dom_mis_fail) |
| b (1) | `start_pos` (OnInitLevel: Transport(start_place)) |
| c (11) | `prison_button` (OnPushed: $tc.SetMode(stop))<br>`dom_st` (OnEnter: DisableTC(time_reverse,1))<br>`dom_obj3` (OnEnter: $tc.SetMode(stop))<br>`time_domain` (OnEnter: $tc.SetMode(stop))<br>`time_domain1` (OnEnter: $tc.SetMode(slow))<br>`dom_farm_prison` (OnEnter: $tc.SetMode(slow))<br>`dom_hint_water` (OnEnter: $tc.SetMode(stop))<br>`act_wake_up` (OnUnlocked: $tc.SetMode(slow))<br>... 3 more owners |
| d (3) | `dom_obj2` (OnEnter: $saves_mng.AutoSave(2))<br>`Prison_Cine` (OnAnimEnd(CLIP_3): $saves_mng.AutoSave(3))<br>`magnet_mine_item` (OnPicked: $saves_mng.AutoSave(1)) |
| e (4) | `clonecontainer_3\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clonecontainer_4\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clonecontainer_6\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clonecontainer_7\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH)) |
| f (1) | `s3d_gate_destr\|Actor` (OnDestroyed: ShakeCamera) |
| i (1) | `kassir` (OnInit: SetAim) |

#### 12. `ts2_summeroutdoor_part01`

| Kind | Script owners (handler: calls) |
|---|---|
| b (2) | `act_fmv_09_timeline` (OnAnimBegin(FMV_09): DisableInput(1); OnAnimEnd(FMV_09): DisableInput(0)) |
| c (6) | `dom_save_farm` (OnEnter: $tc.SetMode(slow))<br>`dom_farm_bridge` (OnEnter: $tc.SetMode(stop))<br>`dom_nav2` (OnEnter: $tc.SetMode(slow))<br>`time_domain` (OnEnter: $tc.SetMode(stop))<br>`time_domain1` (OnEnter: $tc.SetMode(stop))<br>`start_pos` (OnInitLevel: $tc.SetMode(slow)) |
| d (5) | `dom_act_farm_soldier_near_the_truck` (OnEnter: $saves_mng.AutoSave(4))<br>`dom_save1` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_save` (OnEnter: $saves_mng.AutoSave(3))<br>`dom_save2` (OnEnter: $saves_mng.AutoSave(5))<br>`dom_save_farm` (OnEnter: $saves_mng.AutoSave(1)) |
| e (3) | `actor_checker_1` (OnActionFrame("CHECK",1): FailMission(UI_OBJECTIVES_NO_TRIKE))<br>`actor_checker_2` (OnActionFrame("CHECK",1): FailMission(UI_OBJECTIVES_NO_TRIKE))<br>`actor_checker_3` (OnActionFrame("CHECK",1): FailMission(UI_OBJECTIVES_NO_TRIKE)) |
| f (28) | `STRIDER\|Spider` (OnAnimBegin(WALK): ShakeCamera x28) |

#### 13. `ts2_summeroutdoor_part02`

| Kind | Script owners (handler: calls) |
|---|---|
| b (7) | `dom_reload` (OnEnter: DisableMove(0))<br>`dom_save` (OnEnter: DisableMove())<br>`actor_fmv10` (OnAnimBegin(PLAY_FMV): DisableInput(1); OnAnimEnd(PLAY_FMV): DisableInput(0))<br>`camp_rebel_1` (OnBhvEnd(WALK): DisableMove(0))<br>`ref_button` (OnPushed: DisableMove(1))<br>`start_pos` (OnInitLevel: Transport(start_place)) |
| c (10) | `dom_save4` (OnEnter: $tc.SetMode(slow))<br>`Dom_FortressGuard1` (OnEnter: $tc.SetMode(stop))<br>`dom_act_Station_farm` (OnEnter: $tc.SetMode(slow))<br>`dom_save1` (OnEnter: $tc.SetMode(stop))<br>`dom_no_reverse` (OnEnter: DisableTC(time_reverse,1))<br>`TRENCH_ALARMER` (OnUnlocked: DisableTC(time_reverse,0))<br>`forest_alarm` (OnUnlocked: $tc.SetMode(stop))<br>`ai_swat6` (OnInit: $tc.SetMode(stop), $tc.SetMode(slow))<br>... 1 more owners |
| d (4) | `dom_save4` (OnEnter: $saves_mng.AutoSave(1))<br>`Dom_FortressGuard1` (OnEnter: $saves_mng.AutoSave(4))<br>`progression_actor` (OnUnlocked: $saves_mng.AutoSave(3))<br>`camp_rebel_1` (OnBhvEnd(WALK): $saves_mng.AutoSave(2)) |
| h (1) | `ref_button` (OnPushed: Transport(Train_actor/Swift_place)) |
| i (4) | `dom_save` (OnEnter: $camp_rebel_1.SetInterest, $camp_rebel_2.SetInterest, $camp_rebel_3.SetInterest)<br>`FortressGuard3` (OnInit: SetAim) |
| j (1) | `start_pos` (OnInitLevel: ShowMsg) |

#### 14. `ts2_guardfactory_part01`

| Kind | Script owners (handler: calls) |
|---|---|
| a (3) | `dom_hint_guns` (OnStopTReverse: IsBelong dom_hint_guns)<br>`monitor_1\|rebel` (OnAlert: IsBelong monitor_1_dom_alert)<br>`controlpanel_3\|scientist_1` (OnBhvEnd(STAND): IsBelong monitor_1_dom_alert) |
| b (3) | `Start_cine` (OnAnimEnd(CLIP_1): DisableMove(0))<br>`start_pos` (OnInitLevel: DisableMove(1), Transport(start_pos)) |
| c (22) | `dom_hint_swing` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`dom_reach_np_3` (OnEnter: SetReverseAvailThreshold(0))<br>`dom_fan_tc` (OnEnter: $tc.SetMode(reverse), SetReverseAvailThreshold(10); OnLeave: $tc.SetMode(slow), SetReverseAvailThreshold(0))<br>`dom_timemeter` (OnEnter: SetFixedTimeTR(1); OnLeave: SetFixedTimeTR(0))<br>`dom_fire_tc` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`vent_gates` (OnActionFrame("OPEN",7): $tc.SetMode(slow))<br>`Start_cine` (OnAnimEnd(CLIP_1): DisableTC(time_reverse,0))<br>`RIP_cine` (OnActionFrame("CLIP_1",3): StopTControl(tslow), StartTControl(tstop); OnActionFrame("CLIP_1",4): StopTControl(tstop), StartTControl(tslow); OnActionFrame("CLIP_1",5): StopTControl(tslow))<br>... 3 more owners |
| d (6) | `dom_reach_obj1_sub2` (OnEnter: $saves_mng.AutoSave(6))<br>`dom_ssam_ballistic` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_farm_entrance2` (OnEnter: $saves_mng.AutoSave(3))<br>`dom_create_machinery` (OnEnter: $saves_mng.AutoSave(5))<br>`cine1` (OnAnimEnd(CLIP_1): $saves_mng.AutoSave(4))<br>`act_alarm_off` (OnUnlocked: $saves_mng.AutoSave(2)) |
| e (1) | `RIP_cine` (OnActionFrame("CLIP_1",6): Kill) |
| g (1) | `RIP_cine` (OnAnimBegin(CLIP_1): SetWpnByPlayer) |
| i (7) | `dom_act_ceilguns` (OnEnter: $ceilgun1.SetTarget(player), $ceilgun2.SetTarget(player), $ceilgun3.SetTarget(player), $ceilgun4.SetTarget(player))<br>`monitor_1\|rebel` (OnBhvEnd(TURN_R): SetAim)<br>`room4_console1\|scientist` (OnBhvEnd(STAND): SetAim)<br>`room3_console1\|scientist` (OnBhvEnd(STAND): SetAim) |

#### 15. `ts2_guardfactory_part02`

| Kind | Script owners (handler: calls) |
|---|---|
| a (17) | `start_room_door2` (OnStopTReverse: IsBelong dom_start_room01, IsBelong dom_start_room02)<br>`QGS_event_dom` (OnEnter: IsBelong QGS_event_dom)<br>`s3d_Container_1\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_1\|dom_mis_fail)<br>`s3d_Container_2\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_2\|dom_mis_fail)<br>`s3d_Container_3\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_3\|dom_mis_fail)<br>`s3d_Container_4\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_4\|dom_mis_fail)<br>`s3d_Container_5\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_5\|dom_mis_fail)<br>`s3d_Container_6\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_6\|dom_mis_fail)<br>`s3d_Container_7\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_7\|dom_mis_fail)<br>`s3d_Container_8\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_8\|dom_mis_fail)<br>`s3d_Container_10\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_10\|dom_mis_fail)<br>`s3d_Container_11\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_11\|dom_mis_fail)<br>`s3d_Container_12\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_12\|dom_mis_fail)<br>`s3d_Container_16\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_16\|dom_mis_fail)<br>`s3d_Container_9\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_9\|dom_mis_fail)<br>`s3d_Container_18\|dom_mis_fail` (OnStopTReverse: IsBelong s3d_Container_18\|dom_mis_fail) |
| c (6) | `dom_ladder_tc_tr` (OnEnter: $tc.SetMode(qg_slow))<br>`dom_ladder_tc_sl` (OnEnter: $tc.SetMode(slow))<br>`dom_create_10` (OnEnter: $tc.SetMode(stop))<br>`dom_tc_abc1` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(qg_slow))<br>`start_pos` (OnInitLevel: $tc.SetMode(slow)) |
| d (3) | `dom_autosave1` (OnEnter: $saves_mng.AutoSave(1))<br>`Tank_cine_actor` (OnAnimEnd(CLIP_3): $saves_mng.AutoSave(3))<br>`AutoSave4` (OnUnlocked: $saves_mng.AutoSave(4)) |
| e (14) | `s3d_Container_1\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_2\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_3\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_4\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_5\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_6\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_7\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_8\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_10\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_11\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_12\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_16\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_9\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`s3d_Container_18\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH)) |
| f (2) | `dom_create_02` (OnEnter: ShakeCamera)<br>`QGS_event_dom` (OnEnter: ShakeCamera) |
| i (5) | `dom_start_room02` (OnEnter: $controlpanel_1\|scientist_5.SetInterest)<br>`QGS_event_dom` (OnEnter: $ai_qg_stop07.SetAim)<br>`ai_qg_stop03` (OnInit: SetAim)<br>`alert_actor1` (OnUnlocked: $ai_patrol01.SetInterest)<br>... 1 more owners |

#### 16. `ts2_guardfactory_part03`

| Kind | Script owners (handler: calls) |
|---|---|
| c (12) | `dom_conveyer_tc` (OnEnter: SetTStopLength(140), $tc.SetMode(stop); OnLeave: SetTStopLength(100), $tc.SetMode(slow))<br>`dom_act_bridge_9` (OnEnter: DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`dom_cooke_vo` (OnEnter: DisableTC(time_reverse,0))<br>`dom_TC` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`Lift_actor` (OnActionFrame("UP",1): $tc.SetMode(stop); OnActionFrame("DOWN",2): $tc.SetMode(slow))<br>`start_pos` (OnInitLevel: DisableTC(time_reverse,1)) |
| d (4) | `dom_autosave1` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_autosave2` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_autosave3` (OnEnter: $saves_mng.AutoSave(3))<br>`ladder_cine` (OnAnimEnd(CLIP_1): $ui.AutoSave(3)) |
| g (2) | `dom_holster` (OnEnter: SelectHolster; OnLeave: SelectHolster) |
| i (27) | `dom_ai_bridge_2_WR_1` (OnInit: SetAim)<br>`ai_bridge7_3` (OnAlert: SetInterest)<br>`ai_bridge7_2` (OnAlert: SetInterest)<br>`ai_bridge7_1` (OnAlert: SetInterest)<br>... 13 more owners |

#### 17. `ts2_guardfactory_part04`

| Kind | Script owners (handler: calls) |
|---|---|
| a (1) | `lift_door` (OnAnimEnd(CLOSE): IsBelong dom_act_lift1) |
| b (1) | `start_pos` (OnInitLevel: Transport(start_pos)) |
| c (11) | `dom_conveyer_tc1` (OnEnter: $tc.SetMode(reverse); OnLeave: $tc.SetMode(slow))<br>`dom_propeller_kill2_tc` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow))<br>`dom_tc_Zone3_QGS` (OnEnter: $tc.SetMode(stop); OnLeave: $tc.SetMode(slow); OnLocked: $tc.SetMode(slow))<br>`ai_Z3_bfs_1` (OnInit: $tc.SetMode(slow))<br>`ai_Cross_02` (OnAlert: $tc.SetMode(stop))<br>`Spec_worker\|rebel` (OnAlert: $tc.SetMode(stop); OnComplete: $tc.SetMode(slow)) |
| d (5) | `dom_farm_cross_01` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_autosave03` (OnEnter: $saves_mng.AutoSave(3))<br>`dom_autosave04` (OnEnter: $saves_mng.AutoSave(4))<br>`dom_door_unlock` (OnEnter: $saves_mng.AutoSave(1))<br>`interface\|button` (OnPushed: $saves_mng.AutoSave(5)) |
| f (6) | `dom_act_Stones_01` (OnEnter: ShakeCamera)<br>`dom_act_Stones_02` (OnEnter: ShakeCamera)<br>`dom_shake02` (OnEnter: ShakeCamera)<br>`dom_shake01` (OnEnter: ShakeCamera)<br>... 2 more owners |
| g (2) | `dom_Holster1` (OnLeave: SelectHolster)<br>`start_pos` (OnInitLevel: SelectHolster) |
| i (5) | `sleeping\|scientist` (OnBhvEnd(STAND): SetInterest, SetAim)<br>`Sientist_programmer\|scientist_1` (OnBhvEnd(STAND): SetInterest, SetAim)<br>`ai_Cross_05` (OnInit: SetAim) |

#### 18. `ts2_guardfactory_part05`

| Kind | Script owners (handler: calls) |
|---|---|
| a (7) | `clonecontainer_1\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_1\|dom_mis_fail)<br>`clonecontainer_2\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_2\|dom_mis_fail)<br>`clonecontainer_3\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_3\|dom_mis_fail)<br>`clonecontainer_4\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_4\|dom_mis_fail)<br>`clonecontainer_5\|dom_mis_fail` (OnStopTReverse: IsBelong clonecontainer_5\|dom_mis_fail)<br>`lift_door` (OnAnimEnd(CLOSE): IsBelong dom_act_lift1)<br>`box_worker_laser\|worker` (OnInit: IsBelong dom_worker_WR) |
| b (3) | `planer_cine` (OnAnimEnd(CLIP_4): Transport(anc_ZCS_p1))<br>`Check_Point_1` (OnUnlocked: Transport(Check_Point_1))<br>`start_pos` (OnInitLevel: Transport(start_pos)) |
| c (11) | `tc_dom_lasers` (OnEnter: $tc.SetMode(slow))<br>`dom_copter_cine_start` (OnEnter: DisableTC(time_reverse,1), DisableTC(time_reverse,0), $tc.SetMode(slow))<br>`QG_actor` (OnActionFrame("START",2): $tc.SetMode(qg_slow))<br>`FlashGuardTC` (OnUnlocked: $tc.SetMode(slow))<br>`box1\|rebel` (OnAlert: $tc.SetMode(stop); OnComplete: $tc.SetMode(slow))<br>`ai_rocketeer2` (OnBhvEnd(APPEAR): $tc.SetMode(reverse); OnComplete: $tc.SetMode(slow))<br>`start_pos` (OnInitLevel: $tc.SetMode(slow)) |
| d (5) | `AutoSave5` (OnUnlocked: $saves_mng.AutoSave(5))<br>`AutoSave4` (OnUnlocked: $saves_mng.AutoSave(4))<br>`AutoSave3` (OnUnlocked: $saves_mng.AutoSave(3))<br>`AutoSave2` (OnUnlocked: $saves_mng.AutoSave(2))<br>`AutoSave1` (OnUnlocked: $saves_mng.AutoSave(1)) |
| e (5) | `clonecontainer_1\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clonecontainer_2\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clonecontainer_3\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clonecontainer_4\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clonecontainer_5\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH)) |
| f (21) | `DOM_act_02` (OnEnter: ShakeCamera)<br>`DOM_act_03` (OnEnter: ShakeCamera x2)<br>`DOM_act_04` (OnEnter: ShakeCamera)<br>`DOM_act_05` (OnEnter: ShakeCamera)<br>... 10 more owners |
| i (6) | `lift_cabin` (OnActionFrame("MOVE_UP",1): $ai_swat1.SetAim, $ai_swat2.SetAim, $ai_swat3.SetAim, $ai_swat4.SetAim, $ai_swat5.SetAim)<br>`ai_floor1_1` (OnAlert: SetAim) |

#### 19. `ts2_zep_factory_part1`

| Kind | Script owners (handler: calls) |
|---|---|
| a (1) | `ref_train_gate_2` (OnAnimEnd(CLOSE): IsBelong Hangar_domain) |
| b (11) | `BR_pl_3` (OnActionFrame("MOVE",2): Aimable; OnAnimEnd(MOVE): Aimable, EnableUse)<br>`actor_start_scene` (StartFmv: DisableInput(1); EndFmv: DisableInput(0); OnUnlocked: DisableMove())<br>`Check_Point_3` (OnUnlocked: Transport(Check_Point_3))<br>`Check_Point_2` (OnUnlocked: Transport(Check_Point_2))<br>`start_pos` (OnInitLevel: Transport(start_pos))<br>`actor_start_chase` (OnUnlocked: DisableMove(0), DisableUse) |
| c (16) | `dot_button` (OnPushed: DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`Train_stopper_2` (OnEnter: $tc.SetMode(slow))<br>`semaphoreButton` (OnPushed: $tc.SetMode(slow))<br>`dom_train_hint` (OnEnter: $tc.SetMode(puzzle_reverse_once))<br>`dom_act_alert_Conveyer` (OnEnter: $tc.SetMode(battle_stop))<br>`dom_reach_obj2` (OnEnter: $tc.SetMode(battle_slow))<br>`dom_hint_mines` (OnEnter: $tc.SetMode(mines))<br>`dom_hint_conveyor` (OnEnter: $tc.SetMode(puzzle_reverse); OnLeave: $tc.SetMode(battle_slow))<br>... 5 more owners |
| d (6) | `Train_stopper_2` (OnEnter: $saves_mng.AutoSave(5))<br>`dom_create_train_guard` (OnEnter: $saves_mng.AutoSave(4))<br>`dom_act_alert_Conveyer` (OnEnter: $saves_mng.AutoSave(6))<br>`Gate_Battle_actor1_4` (OnUnlocked: $saves_mng.AutoSave(3))<br>`Bridge_complete_actor` (OnUnlocked: $saves_mng.AutoSave(2))<br>`actor_start_chase` (OnUnlocked: $saves_mng.AutoSave(1)) |
| f (1) | `Opp_Bike_plcnew01` (OnActionFrame("MOVE",10): ShakeCamera) |
| g (4) | `dom_vent` (OnEnter: SelectHolster; OnLeave: SelectHolster)<br>`actor_start_scene` (OnUnlocked: SelectHolster)<br>`actor_start_chase` (OnUnlocked: SelectHolster) |
| h (2) | `BR_pl_3` (OnAnimEnd(MOVE): Transport(Bronik_Exitpos))<br>`actor_start_chase` (OnUnlocked: StartUse(Bronevik)) |
| i (7) | `actor_start_scene` (OnUnlocked: $Reb_2.SetInterest x3, $Reb_1.SetInterest x3)<br>`actor_start_chase` (OnUnlocked: $Opp_5.SetAim) |

#### 20. `ts2_zep_factory_part2`

| Kind | Script owners (handler: calls) |
|---|---|
| a (4) | `clone2\|dom_mis_fail` (OnStopTReverse: IsBelong clone2\|dom_mis_fail)<br>`clone1\|dom_mis_fail` (OnStopTReverse: IsBelong clone1\|dom_mis_fail)<br>`clone3\|dom_mis_fail` (OnStopTReverse: IsBelong clone3\|dom_mis_fail)<br>`clone4\|dom_mis_fail` (OnStopTReverse: IsBelong clone4\|dom_mis_fail) |
| b (2) | `Check_Point_2` (OnUnlocked: Transport(Check_Point_2))<br>`start_pos` (OnInitLevel: Transport(start_pos)) |
| c (13) | `yard_cops_dom_first` (OnInit: $tc.SetMode(qg_shield))<br>`dom_act_tetki` (OnEnter: $tc.SetMode(qg_slow))<br>`dom_act_house_swat` (OnEnter: DisableTC(time_reverse,1), DisableTC(time_reverse,0), $tc.SetMode(qg_shield))<br>`dom_act_lifters_workers` (OnEnter: $tc.SetMode(battle_slow))<br>`dom_act_Swat` (OnEnter: $tc.SetMode(qg_shield))<br>`dom_hint_gates` (OnEnter: $tc.SetMode(slow))<br>`dom_hint_bomb` (OnEnter: $tc.SetMode(stop))<br>`helicopter_plc2` (OnActionFrame("MOVE",2): DisableTC(time_reverse,0))<br>... 3 more owners |
| d (4) | `dom_act_house_swat` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_act_Swat` (OnEnter: $saves_mng.AutoSave(2))<br>`magnet_mine_item` (OnPicked: $saves_mng.AutoSave(4))<br>`Gate_Locker` (OnUnlocked: $saves_mng.AutoSave(3)) |
| e (4) | `clone2\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clone1\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clone3\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH))<br>`clone4\|dom_mis_fail` (OnStopTReverse: FailMission(UI_OBJECTIVES_BREACH)) |
| f (2) | `Door_expl\|warehouse_gates` (DoorExplode: ShakeCamera x2) |
| g (3) | `dom_vent` (OnEnter: SelectHolster; OnLeave: SelectHolster)<br>`start_pos` (OnInitLevel: SelectHolster) |
| i (8) | `reb_idle_actor` (OnUnlocked: $Mitchell.SetInterest, $Reb_2.SetInterest)<br>`Reb_3` (OnInit: SetInterest)<br>`Reb_2` (OnInit: SetInterest; OnBhvEnd(fin_run): SetInterest)<br>`Reb_1` (OnBhvEnd(fin_run): SetInterest)<br>... 2 more owners |

#### 21. `ts2_zep_factory_part3`

| Kind | Script owners (handler: calls) |
|---|---|
| b (1) | `start_pos` (OnInitLevel: Transport(start_pos)) |
| c (10) | `dom_act_worker1` (OnEnter: $tc.SetMode(battle_stop))<br>`dom_hint_turbines` (OnEnter: $tc.SetMode(puzzle_reverse))<br>`dom_act_console1` (OnEnter: $tc.SetMode(battle_stop))<br>`Propellers_mortal_Cine` (OnActionFrame("CLIP_4",1): StartTControl(tslow); OnActionFrame("CLIP_5",4): StopTControl(tslow))<br>`RIP_cine` (OnActionFrame("CLIP_1",3): StartTControl(tslow); OnActionFrame("CLIP_1",5): StopTControl(tslow))<br>`s3d_Propellers_button\|button` (OnPushed: DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`catwalk_alarm_actor` (OnUnlocked: $tc.SetMode(battle_slow)) |
| d (4) | `dom_act_worker1` (OnEnter: $saves_mng.AutoSave(2))<br>`Propellers_mortal_Cine` (OnAnimEnd(CLIP_5): $saves_mng.AutoSave(1))<br>`actor_Planer_StartLoading` (OnUnlocked: $saves_mng.AutoSave(4))<br>`s3d_Propellers_button\|button` (OnPushed: $saves_mng.AutoSave(3)) |
| e (1) | `RIP_cine` (OnAnimEnd(CLIP_1): Kill) |
| f (5) | `exploder2` (OnDestroyed: ShakeCamera)<br>`exploder1` (OnDestroyed: ShakeCamera)<br>`exploder_first` (OnDestroyed: ShakeCamera)<br>`DYN_catwalk2` (OnActionFrame("FALL2",1): ShakeCamera)<br>... 1 more owners |
| g (1) | `RIP_Swift` (OnInit: SetWpnByPlayer) |
| i (17) | `start_dom_act` (OnEnter: $Reb_1.SetInterest, $Reb_2.SetInterest, $Reb_3.SetInterest)<br>`Gate_Locker` (OnUnlocked: $Reb_1.SetInterest, $Reb_2.SetInterest, $Reb_3.SetInterest)<br>`catwalk_Opp2_3` (OnBhvEnd(move): SetAim)<br>`catwalk_Opp2_2` (OnBhvEnd(move): SetAim)<br>... 5 more owners |

#### 22. `ts2_zep_factory_part4`

| Kind | Script owners (handler: calls) |
|---|---|
| b (4) | `dom_train_start` (OnEnter: DisableMove(true))<br>`Train_actor` (OnActionFrame("MOVE_A",4): DisableMove(false))<br>`Check_Point_3` (OnUnlocked: Transport(Check_Point_1))<br>`Check_Point_2` (OnUnlocked: Transport(Check_Point_2)) |
| c (5) | `Train_actor` (OnActionFrame("MOVE_A",4): DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`Fin_cine` (OnAnimBegin(CLIP_6): StartTControl(tslow); OnAnimEnd(CLIP_6): StopTControl(tslow))<br>`FirstWave_actor` (Begin: $tc.SetMode(battle_slow)) |
| d (3) | `s3d_Pult_1` (OnPushed: $saves_mng.AutoSave(2))<br>`PlanerLoading_cine` (OnAnimEnd(CLIP_7): $saves_mng.AutoSave(3))<br>`FirstWave_Complete_Actor` (OnUnlocked: $saves_mng.AutoSave(1)) |
| g (3) | `PlanerLoading_cine` (OnUnlocked: SetWpnByPlayer)<br>`Cabin_Swift` (OnInit: SetWpnByPlayer)<br>`Train_Swift` (OnInit: SetWpnByPlayer) |
| h (1) | `Check_Point_1` (OnUnlocked: Transport(Train_actor/tr_cabin)) |
| i (5) | `Reb_3` (OnInit: SetInterest)<br>`Reb_1` (OnBhvEnd(move_in_train): SetInterest)<br>`Yard_Opp_4` (OnComplete: $Reb_1.SetInterest, $Reb_2.SetInterest, $Reb_3.SetInterest) |

#### 23. `ts2_zepride2_part01`

| Kind | Script owners (handler: calls) |
|---|---|
| b (14) | `dom_reload` (OnEnter: DisableMove(0))<br>`dom_train_start` (OnEnter: DisableMove())<br>`Zeppelin_cruiser_plc_iga` (OnAnimBegin(MOVE): Transport(Zep_Intro_actor/player_place), DisableMove(1); OnActionFrame("MOVE",10): DisableMove(0))<br>`act_timeline_fmv13` (OnActionFrame("FMV",2): DisableInput(1); OnActionFrame("FMV",3): DisableInput(0))<br>`Zep_Intro_actor` (OnAnimBegin(MOVE): Transport(Zep_Intro_actor/player_place), DisableMove(1); OnActionFrame("MOVE",10): DisableMove(0))<br>`Loading_cine` (onanimbegin(clip_1): aimable, hide; onanimend(clip_1): show, aimable) |
| c (22) | `resh_barrel1` (OnDestroyed: DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`dom_bronik_starter` (OnEnter: DisableTC(time_reverse,1), DisableTC(time_reverse,0))<br>`dom_on_floor` (OnEnter: $tc.SetMode(stop))<br>`dom_vent_spawn` (OnEnter: $tc.SetMode(stop))<br>`dom_opp_explode` (OnEnter: $tc.SetMode(stop))<br>`dom_hint_platform` (OnEnter: $tc.SetMode(stop))<br>`rope_btn1` (OnPushed: $tc.SetMode(reverse), $tc.SetMode(slow))<br>`rope_btn2` (OnPushed: $tc.SetMode(reverse), $tc.SetMode(slow))<br>... 5 more owners |
| d (6) | `dom_vent` (OnEnter: $saves_mng.AutoSave(3); OnLeave: $saves_mng.AutoSave(4))<br>`Zeppelin_cruiser_plc2_FC` (OnActionFrame("MOVE",1): $saves_mng.AutoSave(2))<br>`Zep_Intro_actor` (OnAnimEnd(MOVE): $saves_mng.AutoSave(1))<br>`s3d_fuel_loading_button\|button` (OnPushed: $saves_mng.AutoSave(5))<br>`Zeppelin\|complete_puzzle_actor` (OnUnlocked: $ui.AutoSave(2)) |
| e (1) | `Zepp_Destr_Cine_ACTOR` (OnActionFrame("CLIP_1",9): FailMission(GAME_ZEP_DESTROYED)) |
| g (7) | `dom_vent` (OnEnter: SelectHolster; OnLeave: SelectHolster)<br>`Zeppelin_cruiser_plc_iga` (OnAnimBegin(MOVE): SelectHolster; OnActionFrame("MOVE",10): SelectHolster)<br>`Zep_Intro_actor` (OnAnimBegin(MOVE): SelectHolster; OnActionFrame("MOVE",10): SelectHolster)<br>`start_pos` (OnInitLevel: ClearGrenades) |
| h (2) | `Zeppelin_cruiser_plc_iga` (OnAnimEnd(MOVE): StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur01))<br>`Zep_Intro_actor` (OnAnimEnd(MOVE): StartUse(Zeppelin\|Zeppelin_cruiser_ACTOR\|tur01)) |
| i (14) | `ai_soldier07` (OnInit: SetAim, SetInterest)<br>`ai_soldier06` (OnInit: SetAim, SetInterest)<br>`ai_soldier05` (OnInit: SetAim, SetInterest)<br>`ai_soldier04` (OnInit: SetAim, SetInterest)<br>... 3 more owners |

#### 24. `ts2_cityfinal_part01`

| Kind | Script owners (handler: calls) |
|---|---|
| b (10) | `FMV_Manager` (OnAnimBegin(FMV_15): DisableInput(1); OnAnimEnd(FMV_15): DisableInput(0); OnAnimBegin(FMV_16): DisableInput(1); OnAnimEnd(FMV_16): DisableInput(0); OnAnimBegin(FMV_17): DisableInput(1); OnAnimEnd(FMV_17): DisableInput(0))<br>`Check_Point_1` (OnUnlocked: Transport(np_4), Transport(np_5), Transport(Check_Point_1))<br>`start_pos` (OnInitLevel: Transport(start_place)) |
| c (8) | `dom_np1` (OnEnter: $tc.SetMode(stop))<br>`dom_start_spidar` (OnEnter: DisableTC(time_reverse,1))<br>`dom_start_copter` (OnEnter: DisableTC(time_reverse,1))<br>`dom_misc1` (OnEnter: $tc.SetMode(slow))<br>`dom_misc5` (OnEnter: $tc.SetMode(stop))<br>`roof_heli_plc` (OnAnimEnd(FLY): DisableTC(time_reverse,0))<br>`start_pos` (OnInitLevel: $tc.SetMode(slow))<br>`actor_set_slow` (OnUnlocked: $tc.SetMode(slow)) |
| d (5) | `dom_np1` (OnEnter: $saves_mng.AutoSave(1))<br>`dom_np4` (OnEnter: $saves_mng.AutoSave(3))<br>`dom_np5` (OnEnter: $saves_mng.AutoSave(4))<br>`dom_music2` (OnEnter: $saves_mng.AutoSave(2))<br>`dom_save` (OnEnter: $saves_mng.AutoSave(3)) |
| f (52) | `dom_np4` (OnEnter: ShakeCamera x4)<br>`roof_heli_plc` (OnAnimEnd(FLY): ShakeCamera)<br>`STRIDER\|Spider` (OnAnimBegin(BOK_K_NAM): ShakeCamera x4; OnAnimEnd(BOK_K_NAM): ShakeCamera x22; OnAnimEnd(RILO_K_NAM): ShakeCamera; OnAnimBegin(WIN): ShakeCamera; OnAnimBegin(LOSE): ShakeCamera; OnActionFrame("MOVE",7): ShakeCamera)<br>`STRIDER\|Carrot` (OnDestroyed: ShakeCamera)<br>... 3 more owners |
| g (2) | `STRIDER\|Spider` (OnAnimEnd(LOSE): SelectHolster)<br>`start_pos` (OnInitLevel: SetupWeapon(0,0,0)) |
| i (7) | `STRIDER\|tur_shoulder1` (OnHealthChanged: SetTarget(player))<br>`enemy_soldier_11` (OnInit: SetAim)<br>`enemy_soldier_9` (OnInit: SetAim)<br>`enemy_soldier_7` (OnInit: SetAim)<br>... 3 more owners |

## 5. Co-op rules per kind

### 5.1 Generic mechanisms

Named so the per-kind rules and the risk list can refer to them. "Native" = needs code in the mod's DLL
(hooks, new messages); "script" = a level-script patch (`lg-ssl replace` + `pack-patch`); "data" = a
class or list change in the `ps` block.

| Id | Mechanism | How | Kind of work |
|---|---|---|---|
| C1 | **Host-only story handlers.** On a client, level-script handlers that change the world (unlock/lock, `$farm.Create`, AI behaviour, door and actor starts, saves, exits, failure) do nothing; the host runs them and the world reaches the client through replication (`isSyncMP` objects, AI sync) or mod messages. Presentation-only handlers (light zones, sound and EAX domains, music, camera shake) keep running on every machine for its own player. | A filter in the script event dispatch when the mode is 3, with a per-level allow list generated from this audit (owner + handler). The script alternative is a guard at the top of each story handler, but it needs a "this machine is a client" test that the natives do not offer cleanly (`$ui.GetGameInfo("IsServer")` returns text, and single player must keep running everything). | native (preferred) or script |
| C2 | **Mirror the player calls.** When a host story script calls one of the `$player` natives in the mirror list (`DisableInput`, `DisableMove`, `Transport`, `Aimable`, `hide/show`, `DisableTC`, `StartTControl/StopTControl`, `SetTStopLength`, `ClearTimeReverse`, `SetReverseAvailThreshold`, `SetFixedTimeTR`, `SetupWeapon`, `SelectHolster`, `ClearGrenades`, `ShakeCamera`, `ShowSAMMsg/ShowMsg`) and `$tc.SetMode`, `$ui.ShowTimeHint`, objectives, music, `$effect.*`, `$fmv.Play`, the DLL also sends the call to every client (mod message subtype 1, "run this SSL"), which applies it to its own `$player`. | Hook the native functions listed in `work/re/script_api.tsv` (e.g. `DisableInput` `0x438d40`, `Transport` `0x438a40`) on the host; only calls made from level scripts are forwarded, not calls that arrive from the network or the console. | native |
| C3 | **Any / all players in a zone.** `$dom.IsBelong($player)` becomes "any player inside" (story checks) or "all living players inside" (gates that move the team). | Script: `if $dom.IsBelong($player) ... foreach p : @player_mp ($dom.IsBelong(p)) ...` (the `foreach x : @class (cond)` form is used by the shipped scripts, e.g. `foreach bike : @vhc_bike (IsBelong(bike))`). [G] that `@player_mp` enumerates the remote players on the host and that `IsBelong` works on them (it tests the entity position, `0x6c3d60`). Fallback: a native helper. | script (native fallback) |
| C4 | **Remote players set off triggers.** If a domain's `OnEnter` fires only for the local `player` (test D1), a client that enters a story domain sends "entered `<domain>`" to the host, which fires the same event there; the first player to arrive starts the story. | Client: send on the local enter event of domains whose handler is host-only (C1 list). Host: call the object's script event (`OnEnter` = event 0x200 registered in `0x6c2190`). If D1 shows the host already sees `player_mp`, nothing is needed. | native |
| C5 | **Gather and teleport.** Every `$player.Transport(<locator>)` of the host is applied to all players (C2), spread over a small ring around the locator so bodies do not overlap; gates of kind "all players" teleport stragglers after a timeout. | A client moves its own body (movement is client-owned), so the teleport is a C2 message to the owner. The spread needs either per-slot offsets in the DLL or extra locators in a level patch. | native (+ script for locators) |
| C6 | **Team failure vs. player death.** `FailMission` (story failure) -> everybody restarts from the last checkpoint (C7). A player's death (damage, `Kill`) -> multiplayer respawn, but at the current checkpoint or next to a living teammate, never at the level start. Clients never run `FailMission` themselves (C1). | Move the server's spawn point (`mp_rules_sys` spawn table, `[0xfcab18]+0x114`) on every checkpoint; team restart via C7. | native |
| C7 | **Checkpoints.** The host keeps the last `AutoSave(n)` id. Restart = reload the level through the server's own map change (message 0x445, not `$ui.LoadLevel`) and replay the level's `Check_Point_n` restore handler on the host. A full restore would need real saves in mode 2 (the server writes none today). | The `Check_Point_n` objects (placed entities with a `CP { comment }` block and one lock) carry the restore step of `AutoSave(n)`: their `OnUnlocked` teleports the player (`Transport("Check_Point_n")`) and fixes the world (kills far AI, unlocks the next fight) [L from the matching ids, e.g. `ts2_city_part03` `AutoSave(1..4)` / `Check_Point_1..3`]. | native |
| C8 | **Level exits through the server.** A `reload { nameScene, nameAnchor }` domain entered on the host starts the server's map change to `nameScene` for everyone; on clients the exit does nothing. `nameAnchor` (relative placement across levels) applies to the host; the others are gathered at the host (C5) after the load. | Hook the reload domain's action (`dom_reload` component, vtable `0xdc4d3c`; its script method `Reload` `0x8a9cc0` calls the entity's vtable `+0x13c`). Optional "all players inside" (C3) before leaving, with a countdown. | native |
| C9 | **Seats and vehicles.** The scripted seat goes to one player (the host or whoever triggered it); others take free seats of the same vehicle where the vehicle has them (zeppelin turrets), otherwise they spectate (`mp_spectator_sys`, `$mp_rules_sys.Spectate()` already follows another player's view [V, observed]) until the sequence ends, then are teleported (C5). | Vehicles (`vehicle.cls` and all `vhc_*`) are not `isSyncMP` [V]; a remote player cannot drive (no vehicle state in `player_mp`) [L]. Riding along needs native work; spectating needs only the existing system plus a script/DLL switch. | native |
| C10 | **AI that names the player.** `"player"` in AI and turret calls resolves to the nearest living player (`SetTarget`, `SetAim`), or stays the host for cosmetic look-at calls (`SetInterest`). | Hook the entity-name lookup used by these natives when the name is `"player"`. | native |
| C11 | **Lock watchdog on clients.** A mirrored lock (`DisableInput/DisableMove`, `hide`) that the host has released, or that is older than the host's own lock, is cleared on the client; a client never keeps a lock the host does not have. | Same as Jericho's M4. | native |

### 5.2 Rules per kind

| Kind | Vanilla behaviour on a server (no mod) | Co-op rule | Mechanisms | Native? |
|---|---|---|---|---|
| a. triggers/zones | `IsBelong($player)` tests only the machine's own player: on the host only the host counts. Domain `OnEnter` fires on the machine where it is detected [G: which players set it off]. Story progress therefore follows the host's position, and a client may run world changes locally that never reach the host. | **Host decides. "First player" for story triggers (dialogue, ambushes, objectives); "all living players" for gates that move or separate the team (lifts, trains, vehicle boarding, level exits) with teleport of stragglers after a timeout; "the player inside" for hazards and per-zone tuning.** Look triggers (`OnEye`): any player looking. | C1, C3, C4, C5 | C3 script; C4 native if D1 says so |
| b. input/movement/staging | Runs on each machine for its own player when the handler runs there; host-only handlers (AI events, cinematics started by host triggers) lock or move only the host. | **Locks and teleports of a cinematic apply to every player, as a pair** (lock and unlock mirrored together); clients never keep a lock the host released. `Transport` -> all players, spread. `StartUse` of a seat -> one player (C9). `Aimable(0/1)` (truck, armoured car) -> all. | C2, C5, C9, C11 | native |
| c. time powers | Campaign powers are global and local to the machine that runs them [V]; a host power changes the server's world, clients see the effect only through replication and miss the local flags and screen effects [L]. Scripted `StartTControl(..., effect)` slow-motion moments run per machine. | **Scripted slow/stop moments: host runs them (world), clients mirror the local flags and effects.** `DisableTC`, `SetTStopLength`, `ClearTimeReverse`, `SetReverseAvailThreshold`, `SetFixedTimeTR` -> all players (powers are world-global anyway). `$tc.SetMode` (auto-power choice) -> each player from its own position (zone-local, harmless to mirror). **Players' own powers: a client's request goes to the host, which runs it as the one global power** (the engine refuses a second power while one runs, `0x43c640`). | C2, mod message | native |
| d. checkpoints | `AutoSave` writes nothing on the server [V]; a client running it locally would write a single-player save of a client view [G]. | **Host only. Every autosave moves the respawn point (C6) and records the restart point (C7).** Clients ignore autosaves. | C1, C6, C7 | native |
| e. failure/death | `FailMission` on the host: behaviour in mode 2 unknown [G]; on a client it would open the single-player failure menu and its "load" path leaves the session [G]. `Kill()` kills the host even when another player set off the death scene. | **`FailMission` -> team restart (C7), decided on the host. `Kill`, fall height, container traps -> only the player concerned, then normal respawn (C6).** `SetHealthRate` on a turret seat -> the occupant. | C1, C6, C7, C3 | native |
| f. camera/effects | Per machine. | **Presentation: every machine, for its own player** (run locally where the handler runs, mirrored when the handler is host-only). Optional distance fade for explosions far from a player. The death camera of `RIP_cine` (`SetCamFollow`) only for the dying player. | C2 | native for mirroring only |
| g. weapons | Per machine. | **Story loadouts (`SetupWeapon` at level start and `Rebel_weaponer`'s hand-over, `ClearGrenades`) -> all players. `SelectHolster` of crawl zones (`dom_holster*`, `dom_vent`) -> the player inside (each machine for its own player). Holster during a cinematic -> with the lock (C2).** `SetWpnByPlayer` (NPC copies the player's weapon) -> the host's or the triggering player's weapon, cosmetic. | C2 | native for mirroring |
| h. vehicles/seats | Vehicles are not replicated; the seat is taken by the machine that runs the script. | **One player takes the scripted seat; the others take free seats or spectate (section 6).** `InBike()` checks -> "this player" for hints, "every living player" for the quad gates. | C3, C9 | native |
| i. AI and the player | On the host, `"player"` is the host only: scripted aims and turret targets ignore the other players. | **`SetTarget`/`SetAim("player")` -> nearest living player; `SetInterest`/`EndAim` (looking) -> nearest or host, cosmetic.** Boss and turret targets (`Metro_Tank`, `ceilgun1..4`, `helicopter_actor\|tur01`, `STRIDER\|tur_shoulder1`) matter most. | C10 | native |
| j. hints/messages | Per machine. | **Every player** (mirrored when host-only); zone hints only to the player inside. | C2 | native for mirroring |

### 5.3 Which calls need what

| Call | Rule | Where it may run |
|---|---|---|
| `DisableInput`, `DisableMove`, `hide/show`, `Aimable` | all players, paired | host story -> mirrored |
| `Transport(locator)` | all players, spread | host -> owners |
| `StartUse(seat)`, `DisableUse/EnableUse` | one player; others free seat / spectate | host |
| `DisableTC`, `SetTStopLength`, `ClearTimeReverse`, `SetReverseAvailThreshold`, `SetFixedTimeTR` | all players | host -> mirrored |
| `StartTControl/StopTControl` (scripted) | host runs the global effect; clients mirror flags/FX | host -> mirrored |
| `$tc.SetMode` | each player | local or mirrored |
| `AutoSave` | host only, moves respawn and restart point | host |
| `FailMission` | team restart | host |
| `Kill`, `SetDeathFallHeight` | the player concerned | owner of that player |
| `ShakeCamera`, `$effect.*`, `ShowSAMMsg/ShowMsg`, `ShowTimeHint`, objectives, music, `$fmv.Play` | every machine | local or mirrored |
| `SetupWeapon`, `ClearGrenades` | all players | host -> mirrored |
| `SelectHolster` | player in zone; all players during a cinematic | local / mirrored |
| `InBike` | per player | host evaluates per player |
| AI `SetTarget/SetAim("player")` | nearest player | host |

## 6. Sequences that separate the player or force a unique situation

FMVs: `$fmv.Play(name)` runs on the machine that runs the handler. Rule for all of them: **the host's
handler starts the FMV on every machine (C2); the world pauses for nobody** (the server keeps running), so
FMVs that do not end a level should be short or skippable for everyone at once: the host's skip ends it
for all; `"noSkip"` (final level) stays unskippable. Level-ending FMVs (`"end"` argument) are followed by
the exit (C8).

| # | Level | Objects | What happens | Proposal for the other players |
|---|---|---|---|---|
| 1 | `ts2_city_part01` | `start_pos` (`OnInitLevel`: FMV `fmv_01`, weapons removed `SetupWeapon(0,0,0)`, all powers disabled), `start_cine` (wake-up cinematic, `Transport("start_cine_player_place")`, `AutoSave(1)`) | intro, player placed for the cinematic | all players placed (C5); all disarmed; FMV on all |
| 1 | `ts2_city_part01` | `dom_act_weapon` (`DisableMove` for 3 s) + `Rebel_weaponer` (`OnBhvEnd(GIVE)`: `SetupWeapon(4,0,0)`) | the rebel hands the pistol to the player; the hand-over is an AI event (host only) | `SetupWeapon` to all players when the host's AI event fires; lock all for the 3 s or nobody |
| 1 | `ts2_city_part01` | `STRIDER\|Spider` (`CHARGE_CITY1`: forced `tslow`), FMV `city1_end_video` (`STRIDER\|Spider` `FMV`), `cine1` (`fmv_02`) | scripted slow motion, level-end FMV | slow-motion on the host (world) + mirrored flags; FMVs on all |
| 2 | `ts2_city_part02` | `cine_start` (tutorial cinematic: `StartTControl("tstop",1)` / `("tslow",1)`, `OnInterrupted` -> `Transport("start_pos")`), `Fmv_unlocker` (`fmv_03a`) | time-power introduction | mirrored to all; interruption by the host ends it for all |
| 2 | `ts2_city_part02` | `final_room_turret_hmg` (`OnOccupied`: `SetHealthRate(120,80)`, `$rockets_unlocker.Unlock()`) | mounted gun; health rate for its user | the occupant gets the rate; the unlock is host-side and fires for any occupant |
| 3 | `ts2_city_part03` | `Metro_Tank_Zone`, `Metro_Tank_PLACE` (`$Metro_Tank.SetTarget("player",0/1)`) | tank turret aims at the player | C10: nearest player |
| 5 | `ts2_skyscraper_part2` | `fmv_actor` (`fmv_04`, `DisableInput(1/0)`) | FMV with input lock | FMV + lock on all |
| 7 | `ts2_skyscraper_part4` | `Cine_FIN_Actor` (`Transport("fin_pos")`, zeppelin routes, `$ai_swift.SetWpnByPlayer()`, `fmv_05 "end"`, `$dom_reload.Unlock()`), `helicopt_plc` (`$helicopter_actor\|tur01.SetTarget("player")`), `lift_door` + `dom_act_lift1` | finale: the player boards the zeppelin; lift ride | everybody to `fin_pos` (spread); FMV on all; exit C8; lift: all players (C3) |
| 8 | `ts2_zepride1_part01` | `PreStart_cine` (`Transport("Zeppelin_cruiser_Intro_plc/player_place")`, `DisableMove(1)`, all powers off, holster), `Zeppelin_cruiser_Intro_plc` (`MOVE` frame 10: release), `Cine1_ACTOR` (`StartUse(... \|tur02)`), `Check_Point_1..8` (restore: `StartUse(... \|tur05 / tur02)`), `Zepp_Destr_Cine_ACTOR` (`FailMission("GAME_ZEP_DESTROYED")`), `Zeppelin\|Zeppelin_cruiser_ACTOR` (`OnOccupied/OnReleased`: radar range for `user = $player`), `Cine_Fin_ACTOR` (`fmv_06 "end"`) | **the whole level is a turret ride on the moving zeppelin** | the cruiser has several turret seats (the scripts use `tur01`-`tur06` and `tur08`; the class `vhc_zeppelin_cruiser` lists seven: `tur_charging`, two `sob_turret_dirigible_*`, four `sob_turret_hmg_vhc_zep`); AI rebels man `tur02`-`tur06`, `tur08` (`turret = ...` in their AI blocks). **Each player takes a turret; the AI gunner of that seat is removed.** Needs the zeppelin and its turrets replicated and remote players seated (native). Radar range per occupant. Failure -> team restart. Fallback: others spectate the host. |
| 9 | `ts2_zepcrashsite_part01` | `start_pos` (`Aimable(0)`, `Transport("start_place")`) + `truck_place` (`Aimable(1)` at the end of the truck ride), quads `bike1`-`bike12` (`$farm.Create`), `dom_scanner`, `dom_scanner_1`, `dom_scanner_2` (`IsNoBikes`) + `actor_checker`, `actor_checker_1`, `actor_checker_2` (`FailMission("UI_OBJECTIVES_NO_TRIKE")`), `dom_hint_boost*` (`InBike`) | truck ride, then **quad driving**; the checkers fail the mission when a scanner zone holds no quad | all ride the truck (C5 at `start_place`). Quads: one per player (spawn extra copies of the `bike*` objects per player); the fail test becomes "a living player is in the zone without a quad" -> teleport that player onto a quad or past the gap instead of failing. Needs remote quad driving (native); fallback: one driver, others spectate and are teleported at the next checkpoint. |
| 10 | `ts2_zepcrashsite_part02` | `start_pos` (`fmv_07`), `train_button` (`OnPushed`: `Transport("train_player_wp")`, `DisableMove(1)`, train starts), `Reload_domen` (`OnEnter`: `DisableMove(0)` + exit), `actor_fmv` / `Fmv_domen1` (`fmv_08`, `DisableInput`) | **train ride to the level exit**, the player locked on the train | the button press gathers everybody on the train (`train_player_wp`, spread) and locks all; the exit C8 releases all. A player who is dead when the button is pressed respawns on the train. |
| 11 | `ts2_zepcrashsite_part03` | `Prison_Cine`, `clonecontainer_3/4/6/7\|dom_mis_fail` (`OnStopTReverse`: player inside a locked container -> `FailMission("UI_OBJECTIVES_BREACH")`) | reverse-time container puzzle; player trapped = fail | per player: a trapped player is killed (respawn) or teleported out; no team failure |
| 12 | `ts2_summeroutdoor_part01` | `dom_scanner_1..3` + `actor_checker_1..3` (`FailMission("UI_OBJECTIVES_NO_TRIKE")`), `act_fmv_09_timeline` (`fmv_09`, `DisableInput`) | quad section again | as #9 |
| 13 | `ts2_summeroutdoor_part02` | `dom_save` (`OnEnter`: `DisableMove()`, rebel walks up) -> `camp_rebel_1` (`OnBhvEnd(WALK)`: `DisableMove(0)`, `AutoSave(2)`), `actor_fmv10` (`fmv_10`), `ref_button` (`Transport("Train_actor/Swift_place")`, `DisableMove(1)`, train), `dom_reload` (`DisableMove(0)`) | dialogue lock, then **train ride** to the exit | lock all while the rebel talks, unlock all on the host's AI event; train as #10 |
| 14 | `ts2_guardfactory_part01` | `start_pos` (`DisableMove(1)`, `Transport("start_pos")`) -> `Start_cine` (`DisableMove(0)`), `dom_RIP_cine_start` -> `RIP_cine` (death cinematic: forced stop/slow, `$player.Kill()`), `MeatMashine` (`tslow`), `dom_timemeter` (`SetFixedTimeTR`) | start lock; **death scene when the player falls into the machine** | start: all locked and released together; `RIP_cine`: only for the player who fell (camera, death); the others keep playing |
| 15 | `ts2_guardfactory_part02` | `s3d_Container_1..18\|dom_mis_fail` (14, as #11), `start_room_door2` (`IsBelong` on `dom_start_room01/02` after reverse), `QGS_event_dom`, `Tank_cine_actor` | container puzzle | as #11 |
| 18 | `ts2_guardfactory_part05` | `planer_cine` (glider escape: `Transport("anc_ZCS_p1")`, `fmv_11`), `clonecontainer_1..5\|dom_mis_fail`, `lift_door` + `dom_act_lift1` | **glider flight out of the factory** (cinematic), containers, lift | the cinematic + FMV for all; all players to `anc_ZCS_p1` (spread); exit C8 |
| 19 | `ts2_zep_factory_part1` | `actor_start_scene` (`fmv_12`, `DisableInput`, `DisableMove`, holster), `actor_start_chase` (`DisableMove(0)`, `StartUse("Bronevik")`, `DisableUse()`, `AutoSave(1)`), `BR_pl_3` (end: `Transport("Bronik_Exitpos")`, `EnableUse()`, `Aimable`) | **armoured-car chase: the player mans the gun of `Bronevik`** while it drives a route; NPCs ride on its sides (`Bronevik/L4_JUMP`, `R4_JUMP`) | one player on the gun; the others ride on the free outside places like the NPCs (attach to the `Bronevik` follow places, native) or spectate; all teleported to `Bronik_Exitpos` at the end |
| 20 | `ts2_zep_factory_part2` | `clone1..4\|dom_mis_fail` | container puzzle | as #11 |
| 21 | `ts2_zep_factory_part3` | `dom_RIP_cine_start`, `dom_RIP_cine_start1`, `dom_RIP_cine_start2` -> `RIP_cine` (`Kill`), `Propellers_mortal_Cine` (`tslow`) | death scene in the propellers | as #14 |
| 22 | `ts2_zep_factory_part4` | `dom_train_start` (`DisableMove(true)`), `Train_actor` (`MOVE_A` frame 4: `DisableMove(false)` after 6 s), `Check_Point_1` (restore: `Transport("Train_actor/tr_cabin")`), `Fin_cine` (`tslow`), `PlanerLoading_cine` (`$Train_Swift.SetWpnByPlayer()`) | **train fight** | all players gathered on the train when it starts (C3 "all" at `dom_train_start`, teleport stragglers to `Train_actor/tr_cabin`) |
| 23 | `ts2_zepride2_part01` | `Zep_Intro_actor` / `Zeppelin_cruiser_plc_iga` (`Transport("Zep_Intro_actor/player_place")`, `DisableMove`, powers off, `StartUse(... \|tur01)`), `Loading_cine` (`hide()/show()`, `aimable`), `act_timeline_fmv13` (`fmv_13`), `end_cinema` (`fmv_14 "end"`), `Zepp_Destr_Cine_ACTOR` (`FailMission`), `dom_train_start`, `dom_bronik_starter` | **second zeppelin turret ride**, then the citadel on foot | as #8 |
| 24 | `ts2_cityfinal_part01` | `STRIDER\|Spider` boss (`LOSE` -> holster) and `STRIDER\|tur_shoulder1` (`SetTarget("player")`), `actor_start_shoot`, `Check_Point_1` (restore: `Transport("np_4")`, `("np_5")`, `("Check_Point_1")`), `roof_heli_plc`, `FMV_Manager` (`fmv_15`-`fmv_17` `"noSkip"`, `DisableInput` around each, `$ui.ShowCredits(true)`) | final boss; ending FMVs and credits | boss targets the nearest player (C10); ending FMVs and credits on all machines, then back to the menu for everyone (end of session) |

Smaller unique moments (no separation): `ts2_city_part02` `building_destroy_scene_actor` (scripted
explosion, 17 camera shakes; `SetGOD` only in the demo build), `ts2_skyscraper_part3` `Fall_domain*`
(`SetDeathFallHeight(3/30)` per zone -> per player), crawl zones `dom_holster*`, `dom_vent`
(`SelectHolster` per player).

## 7. Riskiest places (likely soft-locks in co-op)

Ordered by impact. "Stall" = the story cannot go on; "strand" = a player cannot rejoin.

1. **Level exits: 23 `reload { nameScene }` domains** (`dom_reload`, `dom_reload1`, `Reload_domen`; e.g.
   `ts2_city_part02` `dom_reload`/`dom_reload1`, `ts2_zepcrashsite_part02` `Reload_domen`). The exit uses
   the single-player level change: on the host it would end the session or leave the clients behind; a
   client reaching an exit first may load the next level alone [G]. **Stall/session loss.** Fix: C8 (+ C3
   "all players" before leaving). Native.
2. **Death and checkpoints in every level.** No save is written on the server and the respawn point is the
   level's only `start_pos`; scripts lock the way back behind the player (e.g. `Check_Point_*` restore
   handlers `Lock()` earlier checkpoints, doors close). A player who dies late respawns at the level start
   and may be **stranded**; `Check_Point_n` restore scripts never run. Fix: C6 + C7. Native.
3. **Quad sections** `ts2_zepcrashsite_part01` (`dom_scanner*`, `actor_checker*`, `bike1`-`bike12`) and
   `ts2_summeroutdoor_part01` (`dom_scanner_1..3`, `actor_checker_1..3`): `FailMission("UI_OBJECTIVES_NO_TRIKE")`
   when a scanner zone has no quad; quads are not replicated and remote players cannot drive.
   **Repeated team failure or strand.** Fix: section 6 #9. Native.
4. **Zeppelin turret levels** `ts2_zepride1_part01` (`PreStart_cine`, `Cine1_ACTOR`, `Check_Point_1..8`,
   `Zepp_Destr_Cine_ACTOR`) and `ts2_zepride2_part01` (`Zep_Intro_actor`, `Zeppelin_cruiser_plc_iga`):
   the players stand on a moving vehicle that is not replicated; remote bodies (client-moved) fall off or
   lag, and the restore handlers seat only the host. **Strand / team failure.** Fix: section 6 #8. Native.
5. **Armoured-car chase** `ts2_zep_factory_part1` `actor_start_chase` -> `BR_pl_3`: only the host is put
   on `Bronevik`; the others stay at the start while the car leaves (`$Bronevik.DisableUse()` blocks
   them from using it). **Strand.** Fix: section 6 #19. Native.
6. **Trains** `ts2_zepcrashsite_part02` `train_button`, `ts2_summeroutdoor_part02` `ref_button`,
   `ts2_zep_factory_part4` `dom_train_start` / `Check_Point_1`: the host is teleported onto the train and
   locked; anyone not on board is left behind, and the train ends in the exit domain. **Strand.** Fix: C5 +
   C3 ("all players"), C8.
7. **Locks released by host-only AI events, set by a client's local trigger**: `ts2_summeroutdoor_part02`
   `dom_save` (`DisableMove()`) released only by `camp_rebel_1` `OnBhvEnd("WALK")`; a client that enters
   `dom_save` first locks itself, while the rebel's walk only finishes on the host [L]. Same pattern:
   `ts2_guardfactory_part01` `start_pos` `DisableMove(1)` -> `Start_cine` `OnAnimEnd(CLIP_1)`,
   `ts2_zep_factory_part1` `actor_start_scene` `DisableMove()` -> `actor_start_chase`. **Client locked
   forever.** Fix: C1 + C2 + C11.
8. **Lifts** `lift_door` + `dom_act_lift1` (`ts2_skyscraper_part4`, `ts2_guardfactory_part04`,
   `ts2_guardfactory_part05`): the cabin goes up when the host is inside, otherwise the door reopens. Other
   players are left below (**strand**), or a client inside keeps the lift waiting. Fix: C3 "all living
   players", teleport stragglers after a timeout.
9. **Container traps** `*|dom_mis_fail` (`ts2_guardfactory_part02` 14, `ts2_guardfactory_part05` 5,
   `ts2_zepcrashsite_part03` 4, `ts2_zep_factory_part2` 4): `OnStopTReverse` fails the mission only when
   the host is trapped; a trapped remote player stays shut in a container (**strand**), and a host trap
   fails everybody. Fix: per-player (C3 per player + kill or teleport, no `FailMission`).
10. **Death scenes** `RIP_cine` (`ts2_guardfactory_part01` via `dom_RIP_cine_start`;
    `ts2_zep_factory_part3` via `dom_RIP_cine_start`, `dom_RIP_cine_start1`, `dom_RIP_cine_start2`): the
    cinematic ends with `$player.Kill()` - the host dies when a client fell in (if the host sees the
    trigger), or the fallen client survives inside the machine (if only the client runs it). Fix: the
    player who set it off is the one killed and filmed.
11. **Mission failure paths** (`FailMission` 41 statements, e.g. `Zepp_Destr_Cine_ACTOR`): behaviour in
    server mode unknown [G]; on a client the single-player failure menu offers "load", which would leave
    the session. Fix: C1 + C6/C7.
12. **Final boss** `ts2_cityfinal_part01` `STRIDER|tur_shoulder1` `SetTarget("player")` (+ `Metro_Tank`,
    `ceilgun1..4`, `helicopter_actor|tur01` elsewhere): the scripted weapons ignore the other players -
    not a stall, but the encounter changes (the host draws all fire). Fix: C10.

Not risky: `ShakeCamera` (262), hints, light-zone checks such as `opendoor8` / `group816`
(`IsBelong($player)` deciding which light zone to switch on), `SetInterest("player")` look-at calls.

## 8. Runtime checks this audit needs

| Id | Question | How |
|---|---|---|
| D1 | Which players set off a domain's `OnEnter` on the host (host only, every character, every player)? | Server with one client: client walks into `ts2_city_part01` `hint_jump` (hint domain); watch the host's hint and the client's; repeat with a story domain (`dom_act_weapon`). |
| D2 | Do level scripts run on the client (`OnInitLevel`, domains, cinematics)? | Client console: `return $player.IsName("player")`, then watch `start_pos` effects (`DisableTC`) on the client after joining. |
| D3 | Does `foreach p : @player_mp (...)` on the host list the remote players, and does `IsBelong(p)` work on them? | Host console with a test domain; count matches. |
| D4 | What `FailMission` and `AutoSave` do in mode 2 and mode 3. | Console on host and client. |
| D5 | What a `reload` domain does on the host and on a client of a running server. | Walk into `ts2_city_part01` `dom_reload` with `TS_NETLOG=1`. |
| D6 | Can a `player_mp` stand on a moving `dynamic` platform and on the zeppelin without falling off? | `ts2_zepcrashsite_part02` train after `train_button` with a client teleported on board. |
| D7 | Does `"player"` in `SetTarget` resolve to the host's `player` on the server? | Console: `$ceilgun1.SetTarget("player")` in `ts2_guardfactory_part01` with a client in view. |
