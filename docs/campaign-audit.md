# Whole-campaign co-op: what still breaks, per level, and the test plan

Static pass over the 24 campaign levels (2026-10-25) for co-op with 2-4 players on the game's own server: the host
runs the story (`storysync.c`, multiplayer.md 9b), clients' players are `player_mp` bodies there. Nothing in this
document was run in a game; the new native code (`native/campaign.c`) is **not yet run**. Tags as in the other
documents: **[V]** read in the data or the executable, **[L]** likely, **[G]** needs a runtime test.

Inputs: `player-audit.md` (rules C1-C11 and the per-level counts; this document does not repeat them),
`multiplayer.md` 8-9d, `native/storysync.c`, `timesync.c`, the level scripts of `out.s3darc` (all `#ssl` texts with
their owners, 6 135 object texts, the same count as the earlier audit), class data of the `ps` block.

## 0. Short answers

| Question | Answer |
|---|---|
| Biggest blocker | **Time reversal is refused in co-op** (timesync: the reverse key does nothing; the server never starts the time recorder). At least 10 levels have reverse puzzles (section 1, G1); where a puzzle has no slow/stop way through, the **whole team** is stuck, not only the clients. Needs shared reversal (multiplayer.md 9a "Reversal"). |
| What storysync already fixes | first player starts the story, the host's events are replayed on clients (their `$player` locks, teleports, loadouts, hints, FMVs), level exits through the server, checkpoints move respawns, `FailMission` = team restart. So most `Transport`/`DisableMove` sequences (train buttons, glider, checkpoint restores) already carry every client. |
| What still fails after storysync | reverse puzzles (G1); seats (`StartUse` is host-only: zeppelin turrets 8/23, armoured car 19) and quads (9/12) (G2); gates that start a lift or train with the first player inside (7, 17, 18, 22, 23) (G3); death scenes kill the host when a client falls in (14, 21) (G4); quad scanners fail the mission when a client walks into them (9, 12) (G5); story objects used by a client (buttons, pickups, a mounted gun) [G] (G6); scripted AI/turret/boss targets see the host only (G7); late joiners keep the level-start locks (G8). |
| Implemented now (`campaign.c`, `TS_CAMPAIGN=1`, default off, not yet run) | "all players" gates for the 5 lift/train domains with a gather of the missing players after 20 s; host-only quad scanners; per-player death domains; host/client `regroup` and `gather` commands. Section 3. |
| Effort to a whole-campaign run | shared reversal L (largest, decides 10 levels); seats/vehicles L; AI nearest-player M; late-join state M; button/pickup forwarding S-M if the live test shows it is needed; spread teleports S; player-facing regroup S. Section 5. |

## 1. Generic gaps after storysync.c

| Id | Gap | Where | Fix design | Effort | Status |
|---|---|---|---|---|---|
| G1 | **Time reversal refused in co-op** ([V] timesync.c: "local reverse refused"; the recorder `0x5846b0` starts only in game mode 1). Puzzles built on reverse: force fields that flip direction only while time runs backwards (`OnStartTReverse`/`OnStopTReverse` -> `SetSpeed`), a collision that hides only during a reverse (`ts2_skyscraper_part2` `Shaft_coll_actor`, `Shaft_coll_actor1`: `Hide()` on reverse start) [V], kill domains locked only during a reverse (`ts2_guardfactory_part04` `PROPELLERS_1` -> `dom_propeller_kill`; `ts2_city_part03` `dom_kill1/2`) [V], and objects rewound by the recorder (cranes, bridges, lifts, propellers; the levels' `$tc.SetMode("reverse"/"puzzle_reverse*")` zones and "Reverse" hints) [L] | 2?, 3, 5, 9, 10, 14, 16, 17, 19, 21, 23 (table per level) | (a) **shared reversal** as planned in 9a: the server records and rewinds its world, clients see the result through replication, every machine shows the effect, players are not rewound. (b) Cheaper partial step: a co-op "script reverse" that fires the world time events `OnStartTReverse`/`OnStopTReverse` on every machine while a player holds reverse (no rewind): makes the force-field, hidden-collision and kill-domain puzzles passable, not the rewound-object ones. Both belong to timesync.c. | (a) L, (b) M | design; **blocker** |
| G2 | **Seats and vehicles**: `$player.StartUse(...)` is wrapped (host-only) on clients [V storysync `host_calls`], `DisableUse` too; the zeppelin (`vhc_zeppelin_cruiser`), armoured car (`vhc_bronik`) and quads (`vhc_bike`) derive from `vehicle` -> `dyn_destroy` -> `dynamic`, and `dynamic.cls` has `isSyncMP = Yes`, so vehicles are probably replicated [L] (the earlier audit said "not synced": `vehicle.cls` itself has no `isSyncMP` line, the parent has). Whether a `player_mp` can drive a quad or man a turret that sits on a moving vehicle is [G]; stock MP maps have usable `sob_turret_hmg`/`sob_turret_lom` (maps 1, 4, 7) and `vhc_bronik` props (7, 8) [V], so remote players manning a fixed turret is [L] | 8, 23 (zeppelin turret rides), 19 (armoured-car gun), 9, 12 (quads), 9 (truck ride at the start) | Fallback for every seat sequence: on a client, the replayed seat start puts the player into the stock spectator view of the host (`$mp_rules_sys.Spectate()`, observed to follow another player [V multiplayer.md]) until the sequence's end event, then the end's `Transport` replay (already there, e.g. `BR_pl_3` -> `Bronik_Exitpos`) or a regroup. Better for the zeppelins: per-level seat table (host `tur02`/`tur05`, clients the next free `tur0x`, the AI gunner of that seat removed on the host). Quads: one `bike*` per player (`$farm.Create` copies) if remote driving works. Hook point: the client's replay of the seat handler (storysync event hook) or a table in campaign.c keyed by object + event | spectate M, seats L, quads L | design |
| G3 | **"All players" gates**: storysync starts a story domain with the first player, so a lift or train that leaves "when the player is inside" leaves with that player only [V] | lifts: 7 `dom_lift__move_up`, 17/18 `dom_act_lift` (+ `lift_door` `IsBelong($player)` on `dom_act_lift1`); trains: 22, 23 `dom_train_start` | **campaign.c rule ALL**: the domain counts as entered only while the host and every joined client are inside; after 20 s of waiting the missing clients are moved in (gather). Trains started by a button (10 `train_button`, 13 `ref_button`) already teleport every client through the replayed `$player.Transport` [V], but onto one point (G11) | done S | **campaign.c, not yet run** |
| G4 | **Death scenes**: `dom_RIP_cine_start*` -> `RIP_cine` (camera, forced stop/slow, `$player.Kill()`): a client falling in sets the story domain off on the host, the host's player is killed and every machine plays the death camera; the client who fell survives (its `Kill` is wrapped) [V] | 14, 21 | **campaign.c rule KILL**: the domain never fires its story in co-op; any player inside it dies (host: `$player.Kill()`, client: message 71), respawn at the last checkpoint | done S | **campaign.c, not yet run** |
| G5 | **Quad scanners**: `dom_scanner*` start `actor_checker*`, whose frame fails the mission when no quad is in the zone [V]; with "any player" a client on foot in the zone restarts the level for everybody, possibly in a loop | 9, 12 | **campaign.c rule HOST**: only the host's player sets the scanners off (the single-player test) | done S | **campaign.c, not yet run** |
| G6 | **Story objects used by a client**: buttons (`OnPushed/OnPoped`, 162 object handlers, e.g. train buttons 10/13/22/23, lift buttons 19/20/22, `prison_button` 11, `but_ceilgun*` 14), pickups (`magnet_mine_item.OnPicked` 9/11/20) and mounted guns (`final_room_turret_hmg.OnOccupied` 2 -> unlocks the rockets) [V]. storysync forwards only domain entries and `OnEye` from clients; other client-detected story events are held back. Buttons and items are `isSyncMP` [V], so the host probably runs the event itself when a client uses them [L/G] | every level with story buttons (section 2) | Live test T-B first. If the host does not see it: storysync forwards held-back `OnPushed/OnPoped/OnPicked/OnOccupied/OnReleased` of `isSyncMP` objects like `OnEye` (sub 22 style), the host fires them once (state check: a button already pushed on the host is not pushed again) | S-M | test first |
| G7 | **AI and turrets that name `"player"`** (audit C10, 235 calls): on the host the name is the host's player. Normal AI perception already shoots client players [V multiplayer.md 8a], so only scripted aims/targets are host-only. Matters for: `Metro_Tank` (3), `helicopter_actor\|tur01` (7), `ceilgun1..4` (14), `ai_swat1..5` in the lift (18), `STRIDER\|tur_shoulder1` boss (24) | 3, 7, 14, 18, 24 (others cosmetic) | Name remote players for scripts (host: `player_mp<slot>`, the naming path aisync.c uses for objects created in play), then hook `SetTarget` (`0x4b2ba0`, `0x4cb6d0`, `0x51a3d0`) and `SetAim` (`0x83d3f0`): `"player"` -> the nearest living player's name | M | design |
| G8 | **Late joiners**: `OnInitLevel` is local, so a client that joins mid-level runs the level-start locks again, and the host's events that released them are not repeated (multiplayer.md 9b "Late joiners"). Level-start locks: 14 `DisableMove(1)` (released by `Start_cine`), 9 `Aimable(0)` (truck end), 1/2/24 `SetupWeapon(0,0,0)` (weapons later from the story or pickups), 1 all powers off, 2/3/14/16 `DisableTC("time_reverse",1)` [V] | 1, 2, 3, 9, 14, 16, 24 | On a client's join the host sends its own player's lock state (input, movement, aim, time-power disables, weapon slots) and the client applies it; or the rule "join at the level start only" with a menu message. Needs the RE of the lock fields | M | design |
| G9 | **Locks from story domains reach every player**: a replayed `DisableMove()` locks players wherever they are (13 `dom_save` until `camp_rebel_1` ends its walk, 1 `dom_act_weapon` 3 s, 22/23 `dom_train_start`) [V]; released by the host's replay | 1, 13, 22, 23, 10/13 trains | Acceptable while the release replay arrives. Safety net: lock watchdog on clients (audit C11): a lock older than N s that the host's player does not have is cleared | M | design |
| G10 | **Level exits need the host's player in the reload domain** (the reload runs from the host's own point test, multiplayer.md 9b) [V] | all 23 exits | Acceptable (the host leads). Optional: a client's report of a reload domain -> the host calls its `Reload` | S | design |
| G11 | **Teleports put every player on one point**: replayed `Transport(<locator>)` (trains 10 `train_player_wp`, 13 `Train_actor/Swift_place`; checkpoint restores 3/18/19/20/22/24; finales 7 `fin_pos`, 18 `anc_ZCS_p1`) | 3, 7, 10, 13, 18-20, 22, 24 | On clients after a replayed `Transport`, offset the player by its slot (0.7 m ring, as campaign.c's gather does) - only after the live test shows whether stacked bodies push apart by themselves and whether an offset drops a player off a train | S | design (test first) |
| G12 | **Stranded players** (any reason: missed seat, fell behind a quad, lift, train) | all | **campaign.c `!cp regroup`** (host: every client to the host; client: itself to the host). Needs a player-facing trigger (pause-menu entry or a key) | console done S; menu S | **campaign.c, not yet run** |
| G13 | **Container traps** `*\|dom_mis_fail` (27): `OnStopTReverse` + `IsBelong($player)` -> `FailMission` [V]; `OnStopTReverse` is a world time event, local on every machine; with reverse refused they never fire | 11, 15, 18, 20 | Inactive today. With shared reversal: per player (the trapped player dies or is moved out; no team restart): add them to campaign.c's KILL-style rules | S (with G1) | design |
| G14 | **Cinematics and FMVs**: `cinema` actors are not `isSyncMP` [V]; replayed `Unlock`/`Start` start them on each client, their `SetCamFollow` and `DisableInput` pairs come with the host's replays [V]; `fmv_15-17 "noSkip"` + credits end the campaign on every machine | all | Covered by storysync; check that a client's camera returns after each cinematic (T-C) | - | test |
| G15 | **Campaign end**: `ts2_cityfinal_part01` `FMV_Manager` -> credits -> the host returns to the menu, the server closes; clients see "server closed" [L] | 24 | Clients: show the credits and go to the main menu without the error (coopsession) | S | design |

## 2. Per level

Round 2 (section 3a) changed several "design" entries below: the quick reversal covers the force-field, hidden
collision and kill-zone puzzles (3, 5 partly, 14, 16, 17, 19 partly, 21), seats/rides are written for 8, 19 and
23, late-join state (G8) and nearest-player AI targets (G7) are written - all not yet run.

Columns: Risk | Fix | Status. "storysync" = covered by the shipped story sync (checked live on part02/part03 only);
"campaign.c" = new rule, not yet run; "design" = section 1 / 5. Exit = the reload domain (target in brackets).
Every level also has the generic items G6 (buttons), G7 (cosmetic AI aims), G8 (late joiners), G12 (regroup).

#### 1. `ts2_city_part01` - exit `dom_reload` (locked) -> part02

| Risk | Fix | Status |
|---|---|---|
| Start: FMV, `SetupWeapon(0,0,0)`, all powers off in `OnInitLevel`; `start_cine` wake-up `Transport` + `AutoSave(1)` | local on each machine + replay | storysync |
| Pistol hand-over: `Rebel_weaponer.OnBhvEnd("GIVE")` (host AI event) -> `SetupWeapon(4,0,0)` | replayed to every client | storysync [G: check the client gets the pistol] |
| `dom_act_weapon` locks movement 3 s for everybody | G9 | acceptable |
| `STRIDER\|Spider` forced `tslow` in a story event | replayed on every machine; timesync re-sync | storysync [G] |
| `reb_revive_a` VO only if the host is in `dom_revive` | cosmetic | - |
| 8 `OnEye` look triggers | sub 22 | storysync (T9 on part02) |
| Late joiner: unarmed, powers off | G8 | design |

#### 2. `ts2_city_part02` - exits `dom_reload`, `dom_reload1` (locked) -> part03

| Risk | Fix | Status |
|---|---|---|
| Time-power tutorial `cine_start` (`StartTControl("tstop"/"tslow")`) | replayed; timesync | storysync |
| `tc_domain_bridge` auto mode `reverse`, `building_site_rocket_6` reverse handling: a bridge to rewind? | G1 | **[L] blocker if reverse is required** - check live |
| `final_room_turret_hmg.OnOccupied` -> `$rockets_unlocker.Unlock()`, `SetHealthRate` for the user | host mans the gun; G6 test with a client | test |
| 30 scripted AI aims | G7 cosmetic | design |
| Natural exit | checked live (T8) | storysync |

#### 3. `ts2_city_part03` - exit `dom_reload` -> skyscraper_part1

| Risk | Fix | Status |
|---|---|---|
| `Metro_Tank.SetTarget("player",0/1)`: the tank turret fires at the host only | G7 | design |
| Valve puzzle `valveLocator` (`OnPushed/OnPoped`) | G6 test | test |
| `dom_kill1/2` locked only during a reverse (the way through a kill zone?) | G1 | **[L] blocker** - check |
| `Check_Point_1..3` restores (`Transport` + far-AI kill) | team restart checked live (T7) | storysync |

#### 4. `ts2_skyscraper_part1` - exit `dom_reload` -> skyscraper_part2

| Risk | Fix | Status |
|---|---|---|
| Garage and force-field buttons (`botton_enter_garage`, `but_force_field*`, `ref_button_garage`) | G6 test | test |
| `opendoor8` light zones by `IsBelong($player)` | local, cosmetic | - |
| 2 `OnEye` | sub 22 | storysync |

#### 5. `ts2_skyscraper_part2` - exit `Reload_domen` (locked) -> skyscraper_part3

| Risk | Fix | Status |
|---|---|---|
| **Reverse puzzles**: `Shaft_coll_actor`/`Shaft_coll_actor1` hide only during a reverse [V]; `force_field1/2` flip only during a reverse [V]; crane `Kran_actor`/`dom_kran_tc`, propellers `dom_hint_propeller1/2`, trap `dom_hint_trap1`, conveyor `dom_hint_conveyer`, `act_revers` [L] | G1 (b) covers the shaft and force fields; the crane/propellers need (a) | **blocker** |
| FMV `fmv_04` with `DisableInput(1/0)` pair | replayed | storysync |
| Alarm buttons, laser fence, prison gate (`but_alarm*`, `but_laser_fence`, `Button_prison_gate`) | G6 test | test |
| Crawl zones `dom_holster*` (`SelectHolster`) | local per player | storysync |

#### 6. `ts2_skyscraper_part3` - exit `Reload_domen` -> skyscraper_part4

| Risk | Fix | Status |
|---|---|---|
| `Fall_domain*` `SetDeathFallHeight` | local per player | storysync |
| Falling cabin `cabin1` (`FALL` frames, `$tc.SetMode`) | presentation | - |
| Story consoles `data_console_1\|button`, `s3d_refLocator8`, `__renamed123` (autosaves) | G6 test | test |
| 4 `OnEye` | sub 22 | storysync |

#### 7. `ts2_skyscraper_part4` - exit `dom_reload` (locked) -> zepride1

| Risk | Fix | Status |
|---|---|---|
| **Lift**: `dom_lift__move_up` (act once) closes the door and sends `lift_cabin` up with the first player; `lift_door.OnAnimEnd("CLOSE")` checks `dom_act_lift1.IsBelong($player)` | rule ALL on `dom_lift__move_up`, gather after 20 s | **campaign.c, not yet run** |
| `helicopt_plc` -> `helicopter_actor\|tur01.SetTarget("player")` | G7 | design |
| Finale `Cine_FIN_Actor`: zeppelin route, `$player.Transport("fin_pos")` (all to one point), `fmv_05 "end"`, unlocks the exit | replay carries every client; G11 | storysync [G] |

#### 8. `ts2_zepride1_part01` - exit `dom_reload` (locked) -> zepcrashsite_part01

| Risk | Fix | Status |
|---|---|---|
| **The whole level is a turret ride**: `Cine1_ACTOR` and the restores `Check_Point_1..8` do `$player.StartUse("Zeppelin\|Zeppelin_cruiser_ACTOR\|tur02/tur05")` - host only; clients stand on the deck [V]. The zeppelin moves by `StartRoute` (host) and is replicated if `vehicle` inherits `isSyncMP` [L]; a client body on its deck [G] | G2: spectate the host (M) or a turret per player (L) | **design, blocker for clients** |
| Intro `PreStart_cine` puts everyone on `Zeppelin_cruiser_Intro_plc/player_place`, locks movement and powers | replayed | storysync [G: bodies on a moving actor] |
| `Zepp_Destr_Cine_ACTOR` -> `FailMission("GAME_ZEP_DESTROYED")` | team restart from `Check_Point_n` (restores seat the host only) | storysync + G2 |
| `Zeppelin\|Zeppelin_cruiser_ACTOR.OnOccupied/OnReleased` (radar range for `user = $player`) | object argument travels as null | cosmetic |
| Deck elevator buttons `Zeppelin\|elev_*` | G6 test | test |

#### 9. `ts2_zepcrashsite_part01` - exit `Reload_domen` -> zepcrashsite_part02

| Risk | Fix | Status |
|---|---|---|
| Truck ride at the start: `OnInitLevel` `Aimable(0)` + `Transport("start_place")` on each machine, truck by host `StartRoute`; `truck_place.OnAnimEnd("MOVE")` gives the aim back | replay; bodies on a moving vehicle [G] | storysync [G] |
| **Quads**: `bike1..12` created on the host; remote driving [G] | G2 | design |
| Quad scanners `dom_scanner`, `dom_scanner_1`, `dom_scanner_2` -> `actor_checker*` `FailMission("UI_OBJECTIVES_NO_TRIKE")` | rule HOST | **campaign.c, not yet run** |
| Reverse time zones `dom_time_zone3/4/6/14/15` (`puzzle_reverse_once`), `bridge_puzle_actor` | G1 | **[L] blocker** |
| `dom_hint_boost*` `InBike()` hints | each machine's own (replay evaluates the client's player) | storysync |
| `magnet_mine_item.OnPicked` (autosave), `Tower_Button`, `nt_ref_button` | G6 test | test |

#### 10. `ts2_zepcrashsite_part02` - exit `Reload_domen` (locked) -> zepcrashsite_part03

| Risk | Fix | Status |
|---|---|---|
| **Train**: `train_button.OnPushed` -> `Transport("train_player_wp")` + `DisableMove(1)`, train starts; `Reload_domen.OnEnter` releases | replay teleports and locks every client onto one point (G11); bodies riding the train [G D6]; the exit moves everybody anyway | storysync [G] |
| Reverse lift puzzle (`dom_hint_lift*` "Reverse", `Lift_actor`, `acktor_revers*`, `time_domain` reverse) | G1 | **[L] blocker** |
| Valves `Valve_button`, `lift_button`, `bridge_button`, `rope_m`, `destr_train_button` | G6 test | test |
| FMVs `fmv_07`, `fmv_08` with input locks | replayed | storysync |

#### 11. `ts2_zepcrashsite_part03` - exit `Reload_domen` -> summeroutdoor_part01

| Risk | Fix | Status |
|---|---|---|
| Prison escape: `prison_button` -> `Prison_Cine` (gates open, `AutoSave(3)`) | G6 test (client presses) | test |
| Containers `clonecontainer_3/4/6/7\|dom_mis_fail` | G13 (inactive without reverse) | - |
| `magnet_mine_item.OnPicked` | G6 | test |

#### 12. `ts2_summeroutdoor_part01` - exit `dom_reload` -> summeroutdoor_part02

| Risk | Fix | Status |
|---|---|---|
| **Quads** | G2 | design |
| Scanners `dom_scanner_1..3` -> `actor_checker_1..3` `FailMission` | rule HOST | **campaign.c, not yet run** |
| `STRIDER\|Spider` walk (28 camera shakes) | presentation | - |
| `Bridge_button` | G6 test | test |

#### 13. `ts2_summeroutdoor_part02` - exit `dom_reload` (locked) -> guardfactory_part01

| Risk | Fix | Status |
|---|---|---|
| Dialogue lock: `dom_save` `DisableMove()` for everybody, released by `camp_rebel_1.OnBhvEnd("WALK")` (host AI) | replay releases; G9 | storysync |
| **Train**: `ref_button` -> `Transport("Train_actor/Swift_place")` + `DisableMove(1)` | as level 10 | storysync [G] |
| FMV `fmv_10` | replayed | storysync |

#### 14. `ts2_guardfactory_part01` - exit `Reload_domen` -> guardfactory_part02

| Risk | Fix | Status |
|---|---|---|
| Start lock `DisableMove(1)` in `OnInitLevel`, released by `Start_cine.OnAnimEnd("CLIP_1")` | replay; late joiner stays locked (G8) | storysync / design |
| **Death scene** `dom_RIP_cine_start` -> `RIP_cine` (`$player.Kill()`) | rule KILL | **campaign.c, not yet run** |
| **Fan shaft**: `dom_fan_tc` reverse, `dom_force_field`, `dom_force_field1/2` lift the player only while time runs backwards, `dom_force_field3` locked during a reverse [V]; hint `dom_hint_shaft` "Reverse" | G1 (b) | **blocker** |
| Ceiling guns `ceilgun1..4.SetTarget("player")` (host only), `but_ceilgun1..4` | G7, G6 | design / test |
| `dom_timemeter` `SetFixedTimeTR` | presentation | - |

#### 15. `ts2_guardfactory_part02` - exit `Reload_domen` -> guardfactory_part03

| Risk | Fix | Status |
|---|---|---|
| 14 containers `s3d_Container_*\|dom_mis_fail`; `start_room_door2` (`OnStopTReverse`) | G13 / G1 | inactive |
| `QGS_event_dom` (time-stop soldier demo) | story domain | storysync |
| `s3d_Planer_button` | G6 test | test |

#### 16. `ts2_guardfactory_part03` - exit `Reload_domen` -> guardfactory_part04

| Risk | Fix | Status |
|---|---|---|
| Conveyor force field `dom_ffield_conveyer` flips only during a reverse [V] (the stop zone `dom_conveyer_tc` may be the intended way) | G1 (b) | [L] check |
| Lift valve `lift_valve01`, consoles (`E3_blue_console`, `console01`, `ref_button_press`) | G6 test | test |
| 27 scripted AI aims | G7 cosmetic | design |

#### 17. `ts2_guardfactory_part04` - exit `dom_reload` -> guardfactory_part05

| Risk | Fix | Status |
|---|---|---|
| **Lift**: `dom_act_lift.OnEnter` closes the door; `lift_door` goes up when the host is in `dom_act_lift1`, else reopens | rule ALL on `dom_act_lift`, gather | **campaign.c, not yet run** |
| Conveyor `dom_conveyer_tc1` (reverse), `dom_force_field1/2`, `PROPELLERS_1` (kill domain locked only during a reverse) [V] | G1 | **blocker** |
| Cranes `but_crane1/2`, `interface\|button` | G6 test | test |

#### 18. `ts2_guardfactory_part05` - exit `dom_reload` (locked) -> zep_factory_part1

| Risk | Fix | Status |
|---|---|---|
| **Lift**: `dom_act_lift` (locked until the cabin came down) -> door -> `dom_act_lift1.IsBelong($player)` -> up + `$dom_cine_dirigible.Unlock()`; `ai_swat1..5.SetAim("player")` on arrival | rule ALL on `dom_act_lift`, gather; G7 | **campaign.c, not yet run** / design |
| 5 containers `clonecontainer_*\|dom_mis_fail` | G13 | inactive |
| Glider escape `planer_cine` (`Transport("anc_ZCS_p1")`, `fmv_11`) | replay; G11 | storysync |
| Tram buttons `but_tram*`, gates `but_gate01/02`, `but_planer_out` | G6 test | test |
| `ai_rocketeer2` auto mode reverse (combat hint) | not required | - |

#### 19. `ts2_zep_factory_part1` - exit `Reload_domen` -> zep_factory_part2

| Risk | Fix | Status |
|---|---|---|
| **Armoured-car chase**: `actor_start_chase` `$player.StartUse("Bronevik")`, `DisableUse()` - host only; clients stay at the start while the car leaves; the end `BR_pl_3` replays `Transport("Bronik_Exitpos")` to everybody [V] | G2 spectate (M); now: clients wait, then are teleported at the end; `!cp regroup` | design |
| Start FMV `actor_start_scene` (`fmv_12`, input lock) from `OnInitLevel` | local + replay | storysync |
| Conveyor `dom_hint_conveyor` (`puzzle_reverse`), `dom_train_hint` (`puzzle_reverse_once`), `force_field` flips on reverse [V] | G1 | **[L] blocker** |
| `Check_Point_1..3` restores (`Transport`, AI deaths, `Bronevik.StartRoute`) | team restart | storysync [G] |
| Lifts with call buttons (`Lift_*_button_3/4`), `semaphoreButton`, `dot_button`, `Hoisting_FB_pult1`, `Alarm_button` | G6 test (call buttons bring a lift back: no gate needed) | test |

#### 20. `ts2_zep_factory_part2` - exit `Reload_domen` -> zep_factory_part3

| Risk | Fix | Status |
|---|---|---|
| 4 containers `clone1..4\|dom_mis_fail` | G13 | inactive |
| `magnet_mine_item.OnPicked` (clears AI, autosave) | G6 test | test |
| Lifts with call buttons `s3d_Lift_button_1/2`, gates `s3d_Gate_button_Left/Right` | G6 test | test |
| `Check_Point_1/2` restores | team restart | storysync [G] |

#### 21. `ts2_zep_factory_part3` - exit `Reload_domen` (locked) -> zep_factory_part4

| Risk | Fix | Status |
|---|---|---|
| **Death scenes** `dom_RIP_cine_start`, `_1`, `_2` (locked until `Propellers_mortal_Cine` CLIP_5) -> `RIP_cine` | rule KILL | **campaign.c, not yet run** |
| Turbines `dom_hint_turbines` (`puzzle_reverse`), `Propellers_dom_WIND` flips on reverse | G1 | **[L] blocker** |
| Gate consoles `s3d_Gate_pult_Left/Right`, `s3d_Propellers_button`, `s3d_Planer_button` | G6 test | test |

#### 22. `ts2_zep_factory_part4` - exit `Reload_domen` (locked) -> zepride2

| Risk | Fix | Status |
|---|---|---|
| **Train fight**: `train_button` unlocks `dom_train_start` (act once), whose `OnEnter` starts `Train_actor` and locks movement; whoever is not aboard stays behind | rule ALL on `dom_train_start`, gather | **campaign.c, not yet run** |
| `Check_Point_1` restore `Transport("Train_actor/tr_cabin")` (on the moving train) | replay; G11; [G] | storysync |
| `Fin_cine` scripted `tslow` | replay | storysync |
| Consoles, zeppelin deck switches, lift `s3d_Lift_button_1` | G6 test | test |

#### 23. `ts2_zepride2_part01` - exit `dom_reload` (locked) -> cityfinal

| Risk | Fix | Status |
|---|---|---|
| **Turret ride intro**: `Zep_Intro_actor` / `Zeppelin_cruiser_plc_iga` put everybody on `player_place`, then `StartUse(...\|tur01)` host only | G2 | **design** |
| Train to the citadel: `dom_train_start` (act once, unlocked by `train_button`) locks movement and starts `train_lorry` | rule ALL, gather | **campaign.c, not yet run** |
| `rope_btn1/2` set the auto mode to reverse; `dom_hint_lifts` "Reverse" | G1 | **[L] blocker** |
| `Loading_cine` hides/shows the player (`hide()`/`show()`) | replay pairs | storysync |
| `Zepp_Destr_Cine_ACTOR` `FailMission` | team restart (no `Check_Point_n`: from the level start) | storysync |
| FMVs `fmv_13`, `fmv_14 "end"` | replay | storysync |

#### 24. `ts2_cityfinal_part01` - no exit (credits)

| Risk | Fix | Status |
|---|---|---|
| Boss `STRIDER\|tur_shoulder1.SetTarget("player")` on damage (the host draws its fire) | G7 | design |
| `SetupWeapon(0,0,0)` at the level start | G8 for late joiners | design |
| `Check_Point_1` restore (`Transport` np_4 -> np_5 -> `Check_Point_1`) | team restart | storysync [G] |
| Ending `FMV_Manager` `fmv_15..17 "noSkip"` with `DisableInput` pairs, credits | replay; G15 | storysync / design |
| 3 `OnEye` | sub 22 | storysync |

## 3. `native/campaign.c` (implemented, **not yet run**)

Off unless `TS_CAMPAIGN=1`; acts only in a co-op session whose host runs the story. Build checked (`build.sh`, no
warnings with `-Wall -Wextra`); the data archives are unchanged (no recipe needed: level scripts are not edited).

- **Domain rules** (host). `storysync.c`'s domain test hook (`ss_dom_count`, `0x6c2e3d`) asks
  `campaign_dom_verdict(dom, host_inside)` first (one clearly marked block in storysync.c); -1 = no rule.

  | Level | Domain | Rule |
  |---|---|---|
  | `ts2_skyscraper_part4` | `dom_lift__move_up` | ALL: entered only while the host and every joined client are inside |
  | `ts2_guardfactory_part04`, `ts2_guardfactory_part05` | `dom_act_lift` | ALL |
  | `ts2_zep_factory_part4`, `ts2_zepride2_part01` | `dom_train_start` | ALL |
  | `ts2_zepcrashsite_part01` | `dom_scanner`, `dom_scanner_1`, `dom_scanner_2` | HOST: only the host's player (the single-player test) |
  | `ts2_summeroutdoor_part01` | `dom_scanner_1..3` | HOST |
  | `ts2_guardfactory_part01` | `dom_RIP_cine_start` | KILL: never fires its story; a player inside dies |
  | `ts2_zep_factory_part3` | `dom_RIP_cine_start`, `_1`, `_2` | KILL |

- **Gather** (host, ALL): while the domain is unlocked, not yet entered (nor spent: an `isActOnce` domain that
  fired), the host's player inside and somebody missing, the missing clients are moved into it after
  `TS_CAMPAIGN_GATHER` seconds (default 20; repeated every 20 s while somebody is missing). The client places its
  own player (message 70) at the domain's floor middle, 0.7 m apart per slot, clamped into the domain's box; the
  same player placement call as storysync's `!ss goto` (vtable `+0x210`, used live in the lab).
- **KILL** (host): every 250 ms, for a KILL domain that is not locked: the host's player inside -> `$player.Kill()`;
  a client's body inside -> message 71, the client runs `$player.Kill()`; at most once per 8 s per player. Acts only
  for domains whose verdict storysync has asked for in this level (proof that its hook is live; a host alone keeps
  the stock cinematic).
- **Console** (`TS_CONSOLE`): `!cp state` (rules of the level with domain state, lock count, host in/out, clients
  missing), `!cp regroup` (host: every client to the host; client: itself to the host's body), `!cp gather <domain>`
  (host).
- Switches: `TS_CAMPAIGN=1` on; `TS_CAMPAIGN_RULES=0` no domain rules; `TS_CAMPAIGN_GATHER=<s>` (0 = never);
  `TS_CAMPAIGN_KILL=0` death domains as storysync; `TS_CAMPAIGN_LOG=1` verbose log (`campaign: ...` lines).
- Messages: subtypes 70-79 (70 gather/regroup, 71 kill; 72-74 in section 3a). console.c's per-frame table grew
  from 8 to 16 entries for campaign.c and coopreverse.c.
- Known limits: a dead client counts as "missing" for an ALL gate until it respawns, then the gather pulls it in
  (gathering a dead player is [G]); a client that steps out while a lift door closes can still be left behind (the
  `lift_door` check uses the host's player); the rules are by object name, checked against the 1.2 data only.

## 3a. Round 2: reversal, seats, late joiners, AI targets (live results in 3b)

Everything below builds (`build.sh`, `-Wall -Wextra` clean) and is off unless switched on; the per-level tables of
section 2 still say "design" for these items: read them with this section.

### Reversal, quick variant (`native/coopreverse.c`, `TS_COOP_REVERSE=1`)

How the engine reverses (static, patch 1.2):

| Part | Where | Notes |
|---|---|---|
| Reverse key | player `+0x39c` bit `0x1000000`, power input `0x40d2c0` | needs the power controller idle (`+0x280` = -1) and energy (`+0x2ac`) above the reverse threshold, then asks the recorder: `call 0x5847c0` at `0x40d491` (`[0xfc8400]` recorder, at least 5 frames, `+0x700ad0`) -> 0 = refusal path `0x40d449` -> `0x40d469` (key flag cleared, `ret 4`). A reverse is a timed run, not held: a second press while it runs is ignored by the engine |
| Recorder | `0x5846b0` records only in game mode 1 (`cmp [gm+0x3c],1`) [V] | so in co-op the key always reaches the refusal |
| World messages | `0x3ff` reverse start (playback `0x584720`), `0x400` end (`0x584870`); the entity message switch `0x643fe0` (table `0x644264`/`0x644298`) sends `0x3fd/0x3fe` slow -> vtable `+0x100/+0x104`, `0x3ff/0x400` reverse -> `+0xf0/+0xf4`, `0x401/0x402` stop -> `+0xf8/+0xfc` [V] | base entity: all six do nothing; the script events `OnStartTReverse`/`OnStopTReverse` are what the puzzles listen to |

The quick variant: the call at `0x40d491` goes through a thunk; in a co-op session with timesync on, a reverse
key press becomes a request to the host (message 80; the host's own press directly), which accepts it when no other
world power runs (timesync's slow/stop, or another reverse: one power at a time, both ways: timesync refuses
slow/stop starts while a co-op reverse runs). Every machine then fires `OnStartTReverse` on the level's
reverse-puzzle objects and shows the reverse screen effect (slot 5); after `TS_COOP_REVERSE_SECONDS` (default 5,
less when the owner's energy does not pay for it) every machine fires `OnStopTReverse`. Only the owner's energy is
spent (`TS_COOP_REVERSE_COST`, default 0.12 per second; energy out ends it), a second press ends one's own reverse.
The host repeats the state every second (late joiners), ends it when the owner leaves, a level change drops it.
storysync treats these events as local (each machine runs its own handlers), and campaign.c keeps `SetSpeed` in
the clients' scripts (storysync had wrapped it as an AI call: the force fields and the wind would not have
flipped, or even started, on clients) - so `TS_CAMPAIGN=1` belongs with `TS_COOP_REVERSE=1`.

What it covers, per reverse puzzle:

| Level | Puzzle | Mechanism in the scripts | Quick variant |
|---|---|---|---|
| 2 `ts2_city_part02` | `tc_domain_bridge` (auto mode reverse), `building_site_rocket_6` | an object to rewind [L] (no reverse handler) | **needs rewind** (or not needed at all: check live) |
| 3 `ts2_city_part03` | `dom_kill1/2` | kill domains `Lock()` on reverse start, `Unlock()` on stop [V] | **yes** |
| 5 `ts2_skyscraper_part2` | `Shaft_coll_actor`, `Shaft_coll_actor1` | collision `Hide()` during a reverse [V] | **yes** |
| 5 | `force_field1/2` | `SetSpeed` flips [V] | **yes** (with `TS_CAMPAIGN=1` on clients) |
| 5 | crane `Kran_actor` (`s3d_Kran_button` FORTH/BACK, `dom_kran_tc`), conveyor `Conveyer_actor` (`s3d_refLocator18`), propellers `dom_hint_propeller1/2`, traps `dom_hint_trap1`, `act_revers` | animated `dynamic` actors (`isSyncTR`) put back by the recorder [L] | **needs rewind** |
| 9 `ts2_zepcrashsite_part01` | `dom_time_zone3/4/6/14/15` (`puzzle_reverse_once`), `bridge_puzle_actor` (UP/DOWN) | rewind of the bridge actor [L] | **needs rewind** |
| 10 `ts2_zepcrashsite_part02` | lift `Lift_actor` (DOWN, `acktor_revers*`, `dom_hint_lift*`) | rewind of the lift [L] | **needs rewind** |
| 14 `ts2_guardfactory_part01` | fan shaft `dom_force_field`, `_1`, `_2` (`SetSpeed` up during a reverse), `dom_force_field3` locked during a reverse, `dom_hint_guns` hint | [V] | **yes** |
| 16 `ts2_guardfactory_part03` | `dom_ffield_conveyer` `SetSpeed` flips | [V] | **yes** |
| 17 `ts2_guardfactory_part04` | `dom_force_field1/2` flip, `PROPELLERS_1` locks `dom_propeller_kill` during a reverse | [V] (the blades keep turning forwards: cosmetic) | **yes** |
| 19 `ts2_zep_factory_part1` | `force_field` flips | [V] | **yes** |
| 19 | `dom_hint_conveyor` (`puzzle_reverse`), `dom_train_hint` (`puzzle_reverse_once`; `Actor_train`) | rewind [L] | **needs rewind** |
| 21 `ts2_zep_factory_part3` | turbines: `Propellers_dom_WIND`, `_WIND2` `SetSpeed` -5/-7.5 instead of 25 | [V] | **yes** |
| 23 `ts2_zepride2_part01` | `rope_btn1/2` drop `cage1_actor`/`cage2_actor` (`FALL`), reverse brings them back | rewind [L] | **needs rewind** |
| 11, 15, 18, 20 | container traps `*\|dom_mis_fail`, `start_room_door2` | `OnStopTReverse` + `IsBelong($player)` -> `FailMission` | **left out on purpose** (without a real rewind they would fail the mission for nothing) |
| 8, 23 | `hide_dom`, `hide_dom_planer1` | show zeppelin parts for a real rewind | left out |

Per-object rewind (design for the "needs rewind" rows; now the fallback, see "Real rewind" below): record only the objects a per-level table names (the actors
above: `Kran_actor`, `Conveyer_actor`, `bridge_puzle_actor`, `Lift_actor`, `Actor_train`, `cage1_actor`,
`cage2_actor`, the level 2 and 5 propeller/trap objects once identified live), on the host, 10 times a second for
the last 10 s: animation clip name and time, position and orientation (`dynamic` actors play animations, so the
clip and its time are the state; `Start(clip, time, ...)` with a start time is used by the shipped scripts, e.g.
`$Train_actor.Start("MOVE_A",23)`). During a co-op reverse the host steps that ring buffer backwards: each step
starts the recorded clip at the recorded time and holds it (RE item: the native behind `Start`'s third argument and
an animation pause/speed field of `dynamic`), and stops at the oldest record or when the reverse ends; `dynamic` is
`isSyncMP`, so clients follow the host's actor. Script events fired by the forward animation (`OnAnimEnd`,
`OnActionFrame`) must not fire while it runs backwards (a guard in storysync's event hook during a reverse). Not
covered by it: physics objects and AI (the engine's own recorder does those; enabling it on the server instead -
the game-mode test at `0x5846b0` - is the full variant, L, with unknown effects on replicated players). Estimate
for the per-object rewind: RE of the animation time API 0.5 day, recorder + playback + event guard 1-2 days, live
tuning per puzzle 1 day: **M (3-4 days)**.

### Real rewind: the engine's own recorder on the host (`coopreverse.c`, `TS_COOP_REWIND=1`; live results in 3b)

Checked first, as asked: the stock recorder can be switched on for the host, so no new recorder was written.

How the single-player reverse works (static, patch 1.2):

| Part | Where | Notes |
|---|---|---|
| Objects | `[0x1095f94]` TR serializer (a save-manager subclass, vtable `0xd9cfac`) and `[0xfc8400]` recorder task (7 MB ring), both made by `0x583d50` from a level-system init (`0x4fd5a0`, vtable `0xd8cf9c`) [V]; whether they exist in a network game is [L] (the mod checks both pointers before it uses them) |
| Recording | every 0.1 s (`0x584960`, period `[0xdc90ec]`) `0x5849c0` serializes the world through the save path with `[0xfcace8]` flag `0x800000`; the serializer's virtuals (`0x583900` save, `0x583940` load, `0x583980`, `0x5839b0`) send each object to the TR ring instead of a save file. Recorded: every class with `isSyncTR` (57 classes set it: `dynamic`, `phys_rb`, doors, buttons, items, `ai_brain`, vehicles, turrets, `domain` ...) [V]. Players are not (`plr_comm` and `player_ghost` have no `isSyncTR`): nobody's body is rewound, as in the campaign |
| Mode tests | `cmp [gm+0x3c],1` in nine places of the recorder code: `0x583912`, `0x583952`, `0x58398c`, `0x5839d6`, `0x583a61`, `0x5840b0`, `0x584679`, `0x5846b0` (record start), `0x585b97` [V]; in a network game each takes the "do not record" way |
| Playback | key path `0x40d4d1`: controller mode 2, duration `+0x2b0`, `0x584170(player)` asks for playback (recorder flag 8, at least 5 snapshots); at the next snapshot tick `0x584720` broadcasts `0x3ff` and `0x584bf0` sets `[0xfcace8]` `0x400000` (playback runs) and broadcasts the global state change (message 6, `[0x1095f90]` bit `0x40`); snapshots are loaded back through the serializer's load path (flag `0x1000000`); the end `0x584870` broadcasts `0x400` and restarts recording. World part of a power: start `0x4417f0` (eax = 2, the player), end `0x441a70` (player; mode 2; 0) -> `0x5847f0` |
| Entities during playback | the entity frame `0x6440ed`: with `0x400000` set, an entity whose `+0x98` bit 6 is clear returns before its network update (`0x6931e0`) [V] - so replicated puzzle objects would not send their positions while they run backwards |

What the mod does on a co-op host with `TS_COOP_REWIND=1` (and `TS_COOP_REVERSE=1`):
- the nine mode reads go through a thunk that hands the recorder a fake game-mode object with mode 1 while this
  machine is the server of a co-op session (clients never record);
- the host's own reverse key runs the engine's reverse unchanged (energy, duration, effect, the scripts' own
  `OnStartTReverse`/`OnStopTReverse`); a client's request starts the same playback through `0x4417f0` and ends it
  through `0x441a70` after the client's affordable time (the client pays its energy, as in the quick variant);
  one power at a time with timesync as before;
- clients are told (message 81) and fire their own copies of the puzzle table's reverse events (force fields etc.)
  with the reverse effect: their players are pushed by their own domains;
- every frame of a playback the host runs the network update of the level's rewind actors itself (`0x6931e0`):
  `Kran_actor`, `Conveyer_actor` (5), `bridge_puzle_actor` (9), `Lift_actor` (10), `Actor_train`,
  `Actor_ghost_train` (19), `cage1_actor`, `cage2_actor` (23);
- container traps: the host's own check is the level's script (as in the campaign: trapped = mission failed = team
  restart); for 1.5 s after a rewind a client inside a closed trap (`*|dom_mis_fail` with a lock, levels 11, 15, 18,
  20) dies (message 82) and respawns at the checkpoint.

Coverage with the real rewind (everything that the campaign's recorder covers, on the host):

| Level | Puzzle | Real rewind | Open [G] |
|---|---|---|---|
| 2 | bridge zone `tc_domain_bridge` | the recorder rewinds whatever the zone is about | which object; whether needed at all |
| 5 | crane, conveyor, propellers, traps, shaft collision, force fields | yes (crane/conveyor pushed every frame; force fields and the collision by the scripts' events: host by the engine, clients by the table) | multi-part actors: does the base network update carry the animation pose, or only the root transform (232 classes use the base write `0x645720`, which serializes the entity's motion component `+0x34`) |
| 9 | bridge `bridge_puzle_actor` | yes (pushed) | as 5 |
| 10 | lift `Lift_actor` | yes (pushed) | a client standing on the lift while it rewinds |
| 11, 15, 18, 20 | container traps | doors and containers rewind on the host (doors, buttons `isSyncMP`), client traps handled | the trap domains' lock state is local per machine (domains are not `isSyncMP`): only the host's counts, which is what the mod checks |
| 14, 16, 17, 19, 21 | force fields, kill zones, wind | yes (scripts' events) | - |
| 19 | conveyor / train hints | yes (`Actor_train` pushed) | which conveyor object |
| 23 | rope cages | yes (pushed) | - |

What can go wrong (live test): the TR system may not exist on a server (then `stock_ready` stays false and the
quick variant runs - logged as "stock rewind ... (n snapshots)"); serializing the whole world every 0.1 s on the host
costs frame time (the campaign does the same on one PC); loading snapshots back may delete or recreate objects
(projectiles, corpses, AI killed during the window) without the network create/remove messages -> objects that
clients no longer see or see twice; AI rewound on the host jump on clients (aisync follows positions); the
reverse playback's own effects (camera, sound `0x81e300`) play on the host's view also when a client owns the
reverse. Fallback if the stock recorder misbehaves in a network game: the per-object recorder of the design above
(only the rewind actors, clip/time + transform, M).

Realistic live-test need (N100 pair; the main PC as client for one round):
- 1 session: does the host record (`snapshots` count in `!rv state` grows), host's own reverse on level 14 and 5
  (crane), frame time with recording;
- 1 session: client-owned reverse on 5, 9, 10, 23 with frames on both machines (does the client see the actor move
  back during the playback, or only a jump at the end; does the client's player on the lift/bridge survive);
- 1 session: containers on 15 (host trapped -> team restart; client trapped -> client dies), AI killed during the
  window (resurrected on the host: seen on the client?), level change right after a reverse;
- expected fixes after that: 1-2 days (object create/remove after a rewind, per-actor pose if only transforms
  replicate). Total about 2 days of tests and fixes before reversal can be called done.

### Seats and vehicles (`campaign.c`, `TS_CAMPAIGN_SEATS`)

Static check of replication:
- Class properties are inherited along `parentDesc` (the team `idTeam` of `plr_comm.cls` reaches `player_mp`,
  observed); `isSyncMP` is a class property (entity `+0x98` bit 1). The chains: `vhc_zeppelin_cruiser`, `vhc_bike`,
  `vhc_bronik` -> `vehicle` -> `dyn_destroy` -> `dynamic` (`isSyncMP = Yes`); `vhc_truck` -> `dyn_destroy`;
  zeppelin and armoured-car guns `sob_turret_hmg_vhc_zep` / `_bronik` -> `sob_turret_hmg_vhc` -> `sob_turret_hmg`
  -> `sob_turret_usable` -> `sob_turret_basic` (`isSyncMP = Yes`). No class in the `ps` block sets `isSyncMP = No`
  on a vehicle or turret [V]. So vehicles and their guns are networked objects; their own net code (regular update)
  was not traced [L].
- A player's network update (`0x420120`) appends the state of the object its vtable `+0x248` returns (the weapon or
  the used object) [V], and the stock MP maps have usable mounted guns (`sob_turret_hmg`, `sob_turret_lom` on maps 1,
  4, 7) [V]: a remote player manning a fixed gun is stock MP behaviour [L]. A remote player driving (quad) is not
  (no MP map has quads) [G].

Implemented (host story events, through one callback in storysync's host event hook):

| Level | Event (host) | Clients |
|---|---|---|
| `ts2_zepride1_part01` | `Cine1_ACTOR.OnAnimEnd("CLIP_3")` (host -> `tur02`), every `Check_Point_n.OnUnlocked` (restores: host -> `tur05`/`tur02`) | client 1 -> `tur04` (host removes `rebel_hmg1`), client 2 -> `tur03` (`rebel_tur1`), client 3 -> `tur06` (`rebel25`); the gunners from their AI blocks' `turret =` [V] |
| `ts2_zepride2_part01` | `Zep_Intro_actor` / `Zeppelin_cruiser_plc_iga` `.OnAnimEnd("MOVE")` (host -> `tur01`) | `tur04` (`rebel_hmg1`), `tur03` (`rebel_tur1`), `tur02` (`rebel_hmg2`) |
| `ts2_zep_factory_part1` | `actor_start_chase.OnUnlocked` (host -> the gun of `Bronevik`) ... `BR_pl_3.OnAnimEnd("MOVE")` | **ride**: each frame the client's player is placed 2.2 m above the car's origin, 0.8 m steps to the sides, view kept; at the end the replayed `Transport("Bronik_Exitpos")` puts everybody down |

The client runs `$player.StartUse("<seat>")` 0.5 s after the host's message (after the replay of the same event).
Open [G]: whether a client's `StartUse` on a turret of the moving zeppelin works, whether the removed gunner's
seat is free at once, the ride height on the armoured car (`TS_CAMPAIGN_RIDE_HEIGHT`), jitter of a per-frame
placement. Quads (9, 12): design only - one `bike*` copy per player when T-V shows that a `player_mp` can drive;
otherwise the host-only scanner rule (section 3) plus `!cp regroup` at the checkpoints. Effort left: quads M,
spectator fallback M (only if the seats fail).

### Late joiners (`campaign.c`, `TS_CAMPAIGN_LATEJOIN`)

The host records what its level scripts set on its own player in this level run - `DisableMove`, `DisableInput`,
`Aimable`, `DisableTC(mode)` (each of the three modes) and `SetupWeapon` - by hooking those natives (`0x438e90`,
`0x438d40`, `0x666d90`, `0x439040`, `0x43a140`; all start `push -1; push <handler>`, replaced by a jump, the
trampoline replays the 7 bytes). A client whose slot starts running the level (coopnet's in-game flag) more than 20
s after the host's level start gets that state (message 72) 3, 10 and 25 s later (its own `OnInitLevel` may run
after the first one); the loadout only with the first. Covers 14 (`DisableMove(1)` until `Start_cine`), 9
(`Aimable(0)` until the truck stops), 1 (powers off, pistol from `Rebel_weaponer`), 2/3/14/16 (`DisableTC`). Not
covered: weapons picked up (not scripted), story progress itself (objects the host already unlocked replicate;
local-only presentation does not).

### AI targets (`campaign.c`, `TS_CAMPAIGN_AI`)

On the host `SetAim` (`0x83d3f0`) and the three `SetTarget` natives (`0x4b2ba0`, `0x4cb6d0`, `0x51a3d0`: turrets,
vehicles, the boss) are hooked: an argument `"player"` becomes the name of the nearest player to the caller (the
host's player keeps `"player"`). Remote players get a script name for that, `coop_player<slot>`, inserted into the
script manager's named list as aisync.c names objects created in play. Covers `Metro_Tank` (3),
`helicopter_actor|tur01` (7), `ceilgun1..4` (14), `ai_swat1..5` in the lift (18), `STRIDER|tur_shoulder1` (24) and
the ~230 scripted aims of ordinary soldiers. The choice is made once per call (as the script calls it); dead players
are not skipped [G].

## 3b. Live pass on the N100 pair (2026-10-26)

Host `D:\Games\TimeShift-test`, client `TimeShift-test2` on the N100 (`lab\n100_pair.ps1`, DLL and paks built from a
clean worktree of HEAD, paks = `work\coop_test_pak`). Runs in `work\runs\n100pair-<stamp>` (frames `host\shotNNN.png`,
`client\shotNNN.png`, logs `ts_coop.log`). Since commit 20d8c4b `TS_CAMPAIGN`, `TS_COOP_REVERSE` and `TS_COOP_REWIND`
are **on by default in co-op sessions** (coopsession.c; `=0` switches each off); stock maps are untouched
(n100pair-20261026-014450: `multiplayer2`, modules idle, no recording, the reverse key keeps its stock use).

| Test | Level | Result | Run(s) |
|---|---|---|---|
| Death domain (KILL) | 14 | client put into `dom_RIP_cine_start` (lab `!cp putin`): host `client in slot 1 ... killed`, no death cinematic, host alive. Fixed on the way: a client killed by `$player.Kill()` stayed "waiting for respawn" for good -> clients now die by `FailMission` (multiplayer death, respawned at 9.83 0.46 -200.57); a dead body left in the domain was killed again every 8 s -> once per entry | -225807, -230530, -231150 |
| Reverse, quick variant on a client | 14 | client presses B (`lab\n100_key.ps1`) in the fan shaft after `cine1` started the fans: host runs its playback for the client (`client's reverse key: reverse for 5.0 s`), the client fires the shaft's 5 reverse objects and **rises up the shaft** (y -6.28 -> 19.5 -> 111), falls back when the reverse ends; reverse tint on both screens (client frame shot074). Fixed on the way: the key never reached the mod on a client (the engine offers reverse only with recorded frames, 0x43edd0: a fake frame count now), and the power start path 0x43e1b0 now asks the mod too | -232319 (also -231744) |
| Real rewind on the host | 14, 5, 9 | the host records (`snapshots 216` = 21.6 s), its own B key runs the engine's reverse (sepia frame host shot127), a client's B key or `!rv start` runs the same playback for the client and ends after the client's time (`reverse ended (time up)`). Fixed on the way: the mod's frame (script tick) does not run during the playback -> the end and the network pushes now run in the recorder's own update (0x584960) | -224356, -225807, -011719, -010451 |
| Client crash with the host recording | 5, 7 | clients crashed at the level start (AV 0x447a5c, player weapons) only when the host recorded (bisected: n100pair-20261026-000101 host-only rewind crashed, -235552 without rewind did not) -> the host pauses recording while a client loads; levels 7 and 5 load cleanly since | -000630, -011719 |
| "All players" lift | 7 | host in `dom_lift__move_up`: `waits for 1 player(s)`, 20 s later `1 client(s) sent to dom_lift__move_up`, the lift goes up with both (both at y 54.33) | -001120 |
| "All players" train | 22 | first the gate never opened (the host stood on the train with its body but no test point inside) -> the host now counts by its body too; then the client was gathered and rode the train away (229 -> 254 x) | -012515, -013926 |
| Quad scanner, host only | 9 | client on foot in `dom_scanner_1`, host elsewhere: no mission failure (`restart 0/0`) | -010451 |
| Zeppelin seats | 8 | host `!ss fire Check_Point_1 OnUnlocked`: `rebel_hmg1` removed, client `seat ...tur04 taken -> Ok`, client frame shot089 on the turret over the moving deck. Fixed on the way: the zeppelin has a net id but no motion data -> stood still on the client; driverless vehicles (zeppelin, dirigibles) now run their `StartRoute` on clients too (client within ~20 m of the host's zeppelin); a seat sent to a loading client is sent again when it runs the level | -002536, -003512 |
| Armoured-car ride | 19 | client rides from `actor_start_chase` to `BR_pl_3` and ends next to the host at the exit (client -101.92 -10.78 -22.32, host -102.87 -9.29 -22.60; frames client shot099 / host shot119). Fixed on the way: clients follow the host's car position (sub 75, the client's copy of an AI-driven car does not move), local routes only for driverless vehicles (a client's `StartRoute` on AI vehicles crashed it) | -005950 |
| Stock client crash, level 19 | 19 | also without the new modules (n100pair-20261026-005301): AV 0x49bddd when an AI biker died on the client (vehicle method 0x49bd70 without a physics body on clients) -> guarded in campaign.c (`TS_CAMPAIGN_VEHGUARD=0` off); no crash since | -005950 |
| AI nearest player | 14 | client next to `ceilgun1`, host far: `ceilgun1..4 targets coop_player1 (nearest player)` | -224356 |
| Late-join state | 14 | clients that started more than 20 s after the host got `player state (mask 21)` and applied it (`-> Ok`) | -224356 and later |

Not proven live (open):
- **Rewound actors seen on clients during a playback** (crane level 5, bridge 9, lift 10, train 19, cages 23): the
  pushes run, but no frame shows a puzzle actor moving back on the client yet (the client died near the level 9
  truck in two tries, level 5 had no visual set-up). Needs the per-level puzzle set-up of the test plan.
- Container traps after a rewind (15), level 2's reverse zone, the force fields of 5, 16, 17, 19, 21 (only 14 proven).
- Zeppelin 23 seats and cages, the level 18 lift, quads (remote driving), the truck ride at the start of level 9.
- The host's own seat view on the zeppelin (host frames dark at `tur05`: check in a play pass).

## 3c. Whole-campaign bot pass (N100 pair, 2026-10-26)

**Resume here (2026-10-26, stopped for the owner's shutdown).** Working through the gap list below in order.
- (1) Rewound objects on clients: done in coopreverse.c (message 83). During the host's playback it sends each rewind
  actor's clip and time every 0.1 s; the client sets them with the actor's own `Start(clip, time[, cycle])` and `Stop()`;
  three more sends after the end. Live on level 5 (run `n100pair-20261026-044632`): the client's crane clip time went
  5.5 -> 4.71 -> 2.10 during the host's rewind and ran on to 5.5 with the host's afterwards. Proven by the clip times
  on both sides; the frames came from a client standing on the crane, so they do not show the movement clearly.
  Next: a frame pair with the client watching the crane from the floor; then levels 9, 10, 19, 23 (bridge, lift, train,
  cages).
- **Update 2026-10-27 (vehicle stream, commits 4563c63, 0a192c9; built, level 9 probed, stream not yet seen working).**
  `campaign.c` message 76 streams vehicle transforms (host: every vehicle it moves; client: the vehicle its own player
  uses, `+0x9c0 == $player`); `TS_CAMPAIGN_VEHSYNC=0` switches it off. First probe (n100pair-20261026-215624, level 9):
  `!cp vehs` listed only the 7 `truck_barrel_*` and never a bike: the stream matched one vtable (0xd7ebb0). Lab probe
  `!cp vts <text>` (run -220609) showed the classes: bikes/quads `0xd91610`, trucks/armoured cars `0xd91970`, the
  vehicle family is nine vtables; a vtable scan of the exe found the base method `0x51c160` in slot `+0x78` of exactly
  those nine, so vehicles are now recognised by that slot (`is_vehicle`). Also `!cp at x y z [object]` (lab: put this
  machine's player at a point, facing an object, for the "watch from the floor" frame pairs). To do on the N100 (it was
  lent to the split agent): level 9 `!cp vehs` on both sides (bike/bronik/truck `recv` on the client), a client
  `StartUse("bike1")` + drive (host sees it), level 7 helicopters/interceptors, level 19 armoured car; then the frame
  pairs of goal 2 (use `!cp at` next to `Kran_actor` etc. on the client). Level 16 chatter filter
  (`campaign_frame_local`) and level 8 Cooke wait (`campaign_replay_wait`) are built but not run either.
- Earlier plan for (2) AI vehicles on clients. `!cp nodeset <obj> 68 x y z` writes on a client persist (the crane probe), so try
  a host position stream (node +0x38/+0x48/+0x58 rows and +0x68, maybe +0x14) for route-driven vehicles. Then (3)-(6) as
  listed in the coordinator's order (16 chatter, Cooke's dialogue on 8, the replay flood on 23, the unverified items),
  then re-run the bot on the touched levels.

`lab\campaign_bot.ps1` (private lab, Windows PowerShell 5.1): for each level it starts the pair on that level as a
co-op session (story, time powers, campaign rules and rewind on by default), waits until the client runs it, checks
both (session role, same level, story replays), takes frames, idles 25 s, then takes the level's exit on the host
(unlock the reload domain, put the host's player into it; after 25 s without a level change the server's level
change `!ss exit <next>`) and waits until both run the next level. Summaries:
`work\runs\campaign-bot-20261026-015102.txt` (1-2), `-021430.txt` (3-24), `-033819.txt` (20-22 again with longer
waits for first-time client loads, up to ~6 min while the second copy builds its shader cache).

**24/24 levels traversed by host + client, no crash on either side, the client still joined after every level
change.**

| # | Level | Exit | Client on next level | Replays on client | Client "unknown object" | Run |
|---|---|---|---|---|---|---|
| 1 | city_part01 | natural | yes | - | 22 | n100pair-20261026-015102 |
| 2 | city_part02 | natural | yes | - | 20 | -015852 |
| 3 | city_part03 | natural | yes | 112 | 31 | -021430 |
| 4 | skyscraper_part1 | natural | yes | 5 | 2 | -021654 |
| 5 | skyscraper_part2 | natural | yes | 5 | 4 | -022044 |
| 6 | skyscraper_part3 | natural | yes | 6 | 2 | -022403 |
| 7 | skyscraper_part4 | natural | yes | 14 | 53 | -022631 |
| 8 | zepride1_part01 | natural | yes | 28 | 2 | -022846 |
| 9 | zepcrashsite_part01 | server | yes | 17 | 6 | -023112 |
| 10 | zepcrashsite_part02 | server | yes | 0 | 3 | -023500 |
| 11 | zepcrashsite_part03 | server | yes | 0 | 2 | -023927 |
| 12 | summeroutdoor_part01 | natural | yes | 1 | 2 | -024308 |
| 13 | summeroutdoor_part02 | natural | yes | 14 | 2 | -024641 |
| 14 | guardfactory_part01 | natural | yes | 0 | 2 | -024905 |
| 15 | guardfactory_part02 | natural | yes | 10 | 2 | -025208 |
| 16 | guardfactory_part03 | server | yes | 762 | 3 | -025438 |
| 17 | guardfactory_part04 | server | yes | 19 | 2 | -025859 |
| 18 | guardfactory_part05 | natural | yes | 14 | 3 | -030250 |
| 19 | zep_factory_part1 | server | yes | 102 | 31 | -030716 |
| 20 | zep_factory_part2 | natural | yes | 0 | 2 | -033819 |
| 21 | zep_factory_part3 | server | yes | 14 | 3 | -034105 |
| 22 | zep_factory_part4 | natural | yes | 16 | 195 | -034349 |
| 23 | zepride2_part01 | server | yes | 51 | 28 | -033113 |
| 24 | cityfinal_part01 | (final) | - | 4 | 2 | -033551 |

"server" = the bot's placement did not set the exit off, the server's level change did (same result for the
session). Those exits are boxes above the floor (`!ss goto` snaps to the floor below them); the bot now uses
`!cp putin <exit> 0` (no snap). Replays: story events the client ran for the host in the ~2 minutes of the run
(verbose story log from level 3 on).

Open items checked after the bot pass:

| Item | Result | Run |
|---|---|---|
| Container trap (15) | the trap check finds a client inside a closed container and kills it (respawn); the check at the end of the engine's playback did not fire in the first try (client placed mid-air) - diagnostics added, `!rv trapcheck` (lab) | -034647, -035241 |
| Crane (5), client view of a rewind | the client stood on the crane boom and was carried when the crane moved (frames client shot054 -> shot065); **during and after the host's rewind the client's crane did not move back** (shot070, shot081) | -040223 |
| Lift (18) | gate waits, gathers the client, both ride up (y -104.8 -> -69.5) | -040836 |
| Truck ride (9) | the client that joins after the truck stopped stands at the same place as the host; a client in time gets the ride (queued for loading clients, keeps its place on the bed) - not seen live | -041353 |
| Quads (9) | a client can `StartUse` a quad and drive its **own copy** (moved 3.4 m on the client, not on the host): other players see only its body; scanners stay host-only, so no failures | -041353 |
| Zeppelin seats (23) | client seated at the intro; the intro actor ends its animation every minute and re-seated the client each time -> seat rules now act once per level run (not yet re-run) | -041920 |
| Host's seat view (8) | host frames on the bridge are fine; the dark view earlier was the `tur05` seat of the checkpoint restore (looks at the hull) | -022846 |
| Wind (21) | not reproduced: after `$Propellers_mortal_Cine.Unlock()` neither host nor client was pushed (set-up, not co-op) | -042621 |

Remaining gameplay gaps (the list for the next round):
1. **Rewound puzzle actors on clients** (cranes, conveyors, bridges, lifts, cages, trains): `dynamic` actors animate
   locally on every machine from the replayed `Start()` calls; the host's rewind restores its own copy only, and
   these actors have no motion component the network update could carry (net write `0x645720` needs entity `+0x34`).
   Fix: during a playback the host sends each rewind actor's animation id and time (`0x951660`, `0x951500` read them),
   clients set them (setter still to find in the node's animation channel, `[[node+0x84]+0xc]...`) - M.
2. Route-driven AI vehicles (helicopters, interceptors, bikes, the armoured car) do not move on clients (no motion
   data; a client's own `StartRoute` on them crashed). Riders follow the host's position (armoured car, truck); others
   are scenery on clients. Fix: mirror their transforms like the ride position (sub 75) - M.
3. Quads: clients drive unsynced local copies - ride-along or a synced vehicle transform as in 2.
4. Natural exits that are boxes above the floor need the host's player to fall/jump into them (fine in play).
5. Level 16: cyclic machinery (`fire_kill_actor`, presses `PRESS6`, `FL*`) sends ~760 story replays in 2 minutes:
   classify their animation-frame handlers as local - S.
6. Clients miss the dialogue lines of actors the host has just created (`Intro_Cooke`, `Intro_rebel_1` on 8; the
   name arrives after the replay) - delay replays naming not-yet-named objects a little longer, S.
7. A replay waiting for an object that the client has destroyed (`s3d_refLocator480`, 23) logs an engine error every
   frame for 5 s - stop waiting when the object is known to be gone, S.
8. Unverified: the trap check at the end of a real playback, seat-once on 23, force fields of 5/16/17/19/21 (14 proven),
   level 2's reverse zone.

## 4. Test plan for the live pass

Set-up (N100 pair, `lab\n100_pair.ps1`; later the same steps with the main PC as client when the owner allows):

```
lab\n100_pair.ps1 -Seconds 600 -HostIni 'AutoTestMap="<level>"' `
  -HostEnv 'TS_CAMPAIGN=1;TS_CAMPAIGN_LOG=1;TS_COOP_REVERSE=1;TS_COOP_REVERSE_LOG=1;TS_STORYSYNC_LOG=1' `
  -ClientEnv 'TS_CAMPAIGN=1;TS_CAMPAIGN_LOG=1;TS_COOP_REVERSE=1;TS_COOP_REVERSE_LOG=1' `
  -Steps { At 90; OnHost '!cp state'; ... }
```

Fast movement: `!ss goto <domain>` (this machine's player into a domain), `$player.Transport("<object>")` (to a
placed object, AI or locator; not to a domain), `!ss fire <object> <event> [0|1]`, `$<object>.Unlock()` (open a
locked domain or start a cinematic), `!ss exit <level>` (host: the whole session to the next level),
`!ss restart` (team restart), `!cp state|regroup|gather`. A whole-campaign smoke run is 23 x (`$<exit>.Unlock()`,
`!ss goto <exit>` on the host) with the exits of section 2, checking after each change that the client is still
joined (`return $ui.GetGameInfo("IsServer")` = `No` on the client) and runs the new level.

Generic tests (run once, on the level named):

| Id | Level | Steps | Expected |
|---|---|---|---|
| T-C1 rule ALL | 7 | host `!ss goto dom_lift__move_up`; `!cp state` | "clients missing 1", lift stays; ~20 s later the client is moved into the cabin, the lift goes up with both; `campaign: 1 client(s) sent to dom_lift__move_up` |
| T-C2 ALL, both walk in | 17 | host `!ss goto dom_act_lift`, client `!ss goto dom_act_lift` within 20 s | door closes once both are in; both ride up (client body stays in the cabin: [G] D6) |
| T-C3 ALL train | 22 | host `$dom_train_start.Unlock()`, `!ss goto dom_train_start`; wait | train starts only after the gather; both locked, both released at `MOVE_A` frame 4 + 6 s |
| T-C4 HOST scanner | 9 | `$dom_scanner_1.Unlock()`; client `!ss goto dom_scanner_1`, host elsewhere | no mission failure; then host `!ss goto dom_scanner_1` on foot -> team restart (single-player rule) |
| T-C5 KILL | 14 | client `!ss goto dom_RIP_cine_start` | the client dies and respawns at the checkpoint; the host lives; no death camera anywhere; `campaign: client in slot 1 inside ...: killed` |
| T-C6 KILL host | 21 | `$dom_RIP_cine_start.Unlock()`; host `!ss goto dom_RIP_cine_start` | the host dies once, respawns; the client sees no cinematic |
| T-C7 regroup | any | client walks/teleports far away; host `!cp regroup`; then client `!cp regroup` | client next to the host each time, standing on the floor |
| T-C8 off | 7 | without `TS_CAMPAIGN` | `!cp state` says off; the lift leaves with the first player (storysync behaviour) |
| T-B buttons/pickups (G6) | 10, 11, 2 | client uses `train_button` (10), `prison_button` (11), picks `magnet_mine_item` (11), mans `final_room_turret_hmg` (2) | host log `storysync: sent train_button.OnPushed` etc.; if the host never runs it: G6 forwarding is needed |
| T-R reverse (G1) | 5, 14 | try the shaft (`Shaft_coll_actor`) and the fan shaft with slow/stop only | confirms which levels are blocked without reversal |
| T-V vehicles (G2, D6) | 8, 19, 9 | watch the client during the zeppelin intro, the armoured-car start and the truck ride; try a client on a quad and on a free zeppelin turret | does the client's body ride a moving vehicle; can `player_mp` drive or man it |
| T-A AI (G7) | 3, 14, 24 | client in the open near `Metro_Tank`, `ceilgun*`, the boss | they fire at the host only (expected today) |
| T-L late join (G8) | 14 | client joins after `Start_cine` ended | client locked (expected today) |
| T-X exits | all | the smoke run above | 23 level changes, client joined after each |
| T-R1 quick reverse | 14 | `!ss goto dom_force_field` on the client, client presses reverse (or `!rv start`) | host log `coopreverse: client's reverse key: reverse for 5.0 s`; both `reverse on (5 puzzle object(s))`, reverse effect on both; the client is lifted up the fan shaft; `reverse off` after 5 s, the field pushes down again |
| T-R2 one power | 14 | during a reverse press slow on the host; during a host slow press reverse on the client | `timesync: local slow refused, a co-op reverse runs`; `coopreverse: ... refused (a slow/stop power runs)` |
| T-R3 energy, early end | 21 | client reverses, presses again after 2 s | client energy down ~0.24 then regenerating; host's energy unchanged; wind back to 25 at once (`Propellers_dom_WIND`) |
| T-R4 force fields without reverse | 21 | `$Propellers_mortal_Cine.Unlock()` on the host, client stands in the wind domain | the client is pushed as the host is (SetSpeed now runs on clients) |
| T-W1 host records | 5 | `TS_COOP_REWIND=1` on the host; after 10 s `!rv state` | `stock rewind 1 (ready 1 ... snapshots` growing; no frame-time drop that players feel |
| T-W2 host's own rewind | 5 | host presses the crane button, then reverse | the crane moves back on the host and on the client (during the playback, not only at the end); `coopreverse: the host's reverse (engine playback ...)`; client `reverse on` |
| T-W3 client's rewind | 9, 10, 23 | the bridge / lift / cages, then the client presses reverse | host `client's reverse key: reverse for ...`, the host's world plays back, the client sees the actor return; the client's player on the lift is not thrown off |
| T-W4 traps | 15 | host and client each stand in a container while a reverse ends | host trapped -> team restart; client trapped -> `trapped in s3d_Container_n ... it dies`, respawn at the checkpoint |
| T-W5 side effects | 14 | kill a soldier, reverse over it; fire during a reverse | the soldier is back on both machines (or the mismatch is logged); no duplicate or missing projectiles on the client; level change after a reverse works |
| T-S1 zeppelin seats | 8 | host `!ss fire Check_Point_1 OnUnlocked` | host on `tur05`, `rebel_hmg1` gone, client on `tur04` (`campaign: seat ... taken`), both turrets fire; zeppelin moving on both |
| T-S2 armoured-car ride | 19 | host `!ss fire actor_start_chase OnUnlocked`, wait for the route, then the end | the client stands on the car's roof while it drives and can shoot; set down at `Bronik_Exitpos` at the end |
| T-L2 late join state | 14 | host waits 60 s after the level start (past `Start_cine`), then the client joins | host `client 1 joined 6x s into the level`; the client can move ~3 s after it runs the level |
| T-A2 nearest player | 14 | client stands near `ceilgun1`, host far; host console `$ceilgun1.SetTarget("player")` | `campaign: remote player in slot 1 named coop_player1`; the gun shoots the client |

Per level, after the generic tests: start each level with `AutoTestMap`, walk the risky places of section 2 with
`!ss goto`/`Transport`, and record per level: client joined, story replays without errors
(`storysync: ... not replayed` / `dropped` lines), what the client sees in cinematics (frames), whether any client
lock outlives the host's.

## 5. Order and effort

| Step | Work | Effort | Unblocks |
|---|---|---|---|
| 1 | Live pass of campaign.c (T-C1..T-C8) and the probes T-B, T-R, T-V, T-A, T-L | 1 day | decides G2, G6, G11 |
| 2 | Time reversal: quick variant and the real rewind (stock recorder on the host) written (section 3a); per-object rewind only as fallback | ~2 days of live tests and fixes (T-W1..T-W5) | 2?, 3, 5, 9, 10, 11, 14-21, 23 |
| 3 | Seats written (zeppelin turrets per player, armoured-car ride, section 3a); quads and a spectator fallback if T-S fails | live test + M | 8, 9, 12, 19, 23 |
| 4 | G6 forwarding if T-B fails | S-M | many levels |
| 5 | AI `"player"` -> nearest player (G7): written (section 3a) | live test | 3, 7, 14, 18, 24 |
| 6 | Late-join state (G8): written (section 3a); lock watchdog (G9), spread teleports (G11), player-facing regroup (G12), campaign end (G15) | M + S | polish |
