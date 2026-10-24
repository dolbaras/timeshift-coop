# TimeShift data format

How TimeShift (Saber Interactive, 2007, Saber3D engine, PC patch 1.2) stores its game data, and what the
data contains, as far as it matters for modding. Everything here was worked out from the archives and the
executable without running the game; statements that still need a runtime test are marked **(verify)**.

The extractor is `tools/s3darc.py` (Python 3, standard library only).

## 1. Files

The data lives in `preload/paks/*.s3darc`. `bin/steamwars.ini` selects them:

```ini
[Preload]
useArc     = 1
arcPath    = ..\preload\paks
arcName    = patch.s3darc        ; main archive (a patch on top of out.s3darc)
arcNameLoc = patch_en_us.s3darc  ; language archive (patch on top of out_en_us.s3darc)

[Level]
pathPS     = ..\PS               ; see section 6
```

| Archive | Contents |
|---|---|
| `out.s3darc` (1.1 GB) | base data: levels, textures, templates, sounds, shaders, scripts |
| `patch.s3darc` | patch 1.2 on top of `out`: new script block, preload lists, strings, 1 template, 2 SceneData |
| `out_ati` / `out_nv` | GPU-vendor data: 1129 textures (normal, height, "br" maps) encoded per vendor, and precompiled shader pairs (`pc_sm20_ati*` or `pc_sm30_nv*` in 4 variants) |
| `patch_ati` / `patch_nv` | only the 44 preload lists |
| `out_en_us` / `out_ru` | language data: loading screen, FMOD voice event projects, `.psbin` voice tables, the `vo_splines` block (lip-sync curves) |
| `patch_en_us` / `patch_ru` | no data of their own: directory only, every entry points into the base |

The vendor archive name is derived from `arcName` by the executable: it inserts `_ati` or `_nv` before
the extension according to the detected GPU vendor (function at `0x6d2b40`).

## 2. Archive format

All integers are little-endian. A *record* is `u32 usize, u32 csize, csize bytes of zlib` and inflates
to exactly `usize` bytes.

```
record   version        -> u32 1
record   base archive   -> u32 length + name ("" in a base archive, e.g. "out.s3darc" in a patch)
u32      dir_offset     absolute offset of the directory record
...      entry data     one raw zlib stream per entry stored in this file, back to back
record   directory      at dir_offset, up to the end of the file
```

Directory (inflated):

```
u32 count
count x {
    u32  name_length; char name[name_length]   flat name, no path, lower case
    u32  type                                  resource category, table below
    i32  offset                                absolute offset of the zlib stream, or -1
    u32  crc                                   zlib CRC-32 of the inflated data;
                                               if offset == -1: index of the entry in the base directory
    u32  usize                                 inflated size
    u32  csize                                 stored size
}
```

Entries are stored in directory order; there is no padding and no encryption.

**Patches.** A patch archive names its base in the header and carries a complete directory of the
combined set. An entry with offset `-1` is unchanged and read from the base archive at the given
directory index; an entry with a real offset replaces the base copy or is new (`patch.s3darc`: 3136
references, 55 replaced, 2 new). A file-level override is therefore done by writing a new patch archive
whose directory points to the base for everything else.

**Types.** The type id indexes the engine's preload category table (strings at `0xd705c0`); the same
names are the keys of the preload lists.

| id | Category | Content |
|---|---|---|
| 0 | (preload list) | per level: text `Key = [ "name", ... ]` with a u32 length prefix |
| 1 | SceneData | `<level>.data`, binary per-level character data (purpose not yet known) |
| 2 | CacheBlock | sub-archive of small text files, see below |
| 3 | Shader | HLSL source `.fx/.psh/.vsh` (packed) |
| 4 | ShaderCache | not used in the shipped archives |
| 5 | ShaderPairs | compiled shader permutations (vendor archives) |
| 6 | Textures | Saber texture container (`TCIP` chunk header) |
| 7 | Cubemaps | same container |
| 8 | SoundData | FMOD Designer `.fev` projects, `.psbin` voice tables |
| 9 | Sounds | names only (event names in preload lists) |
| 10 | WaveBanks_mem | FMOD sample banks (`FSB3`) |
| 11 | WaveBanks_strm_file | streamed banks, loose files under `sounds/` |
| 12 | Templates | binary object templates and animations (`LPTA` tag) |
| 13 | VoiceSplines | lip-sync curves, inside the `vo_splines` block (`.spl`) |
| 14 | Strings | UTF-16LE localisation tables (9 languages: cs, en, fr, de, hu, it, pl, ru, es) |
| 15 | Ragdolls | Havok `.hkx` (packed) |
| 16 | Scene | `<level>.lg`: the level's objects with their properties and scripts |
| 17-24 | SceneGrs, SceneScr, SceneTrc, SceneRain, SceneCDT, SceneSM, SceneSLO, SceneVis | per-level `.grs .scr .trc .rain .cdt .sm .slo .vis` |

**Packed entries.** Types 3, 15 and 18 have a second compression layer inside the stored data:
`u32 usize, u32 csize, u16 0, zlib[csize]`.

**Cache blocks** (type 2: `ps`, `ini`, `mp_stuff` in `out`/`patch`; `vo_splines` in `out_<lang>`):

```
u32 n_groups; u32 n_files; u32 data_size
n_groups x { u32 group_csize; u32 group_nfiles;
             group_nfiles x { u64 key; u32 usize } }
n_groups zlib streams of group_csize bytes; group g inflates to its files concatenated
```

Keys are sorted. File names are not stored: `key` is CRC-64 with the ISO polynomial in reflected form
(`0xD800000000000000`), initial value 0, no final xor, over the **lower-cased bare file name**, e.g.
`player.cls`, `materials.ps`, `<texture>.td`. The executable builds the CRC table at run time (function
at `0xc52440`). Names can be recovered by hashing candidate words; `s3darc.py names` harvests them from
the archives and the executable and identifies 5599 of 7738 `ps` members and 98 % of the voice splines.
The unnamed rest is mostly generated texture descriptors of textures that are not shipped, plus some
dialog and UI files.

## 3. What is in the data

Totals after extraction: 39 998 files, 3.34 GB. Counts per archive (local entries; cache block
members counted individually):

| Category | out | patch | out_ati / out_nv | out_en_us |
|---|---|---|---|---|
| Scene `.lg` | 39 (916 MB) | | | |
| SceneCDT / SceneSM | 39 / 39 | | | |
| SceneGrs / Trc / Rain / SLO / Vis / Scr | 15 / 12 / 4 / 8 / 21 / 7 | | | |
| SceneData `.data` | | 2 | | |
| Textures `.tex` | 1997 (739 MB) | | 1129 each | 1 |
| Cubemaps | 19 | | 19 each | |
| Templates `.tpl` | 756 (205 MB) | 1 | | |
| WaveBanks `.fsb` | 75 | | | |
| SoundData | 3 `.fev` | | | 2 `.fev`, 2 `.psbin` |
| Ragdolls `.hkx` | 46 | | | |
| Shader | 23 `.fx`, 15 `.psh`, 17 `.vsh` | | 2340 pairs each | |
| Strings | 9 | 9 | | |
| Preload lists | 44 | 44 | 44 each | |
| `ps` block | 7738 | 7738 (replaces) | | |
| `ini` block | `game_options.ini`, `steamwars.ini` | | | |
| `mp_stuff` block | `dictionary.str` | | | |
| `vo_splines` block | | | | 7055 `.spl` |

The `ps` block by extension (named members): 3953 `.td` texture descriptors, 1017 `.cls` object
classes, 466 `.sfx` effect definitions, 94 `.ui` interface frames, 68 `.ps` property sets, 1 `.clsdb`
class database; 2139 unnamed.

### Levels

39 levels: `menu`, 24 campaign parts (`ts2_city_part01` ... `ts2_cityfinal_part01`, grouped by mission:
city, skyscraper, zepride, zepcrashsite, guardfactory, summeroutdoor, zep_factory, cityfinal) and 14
multiplayer maps (`multiplayer1` ... `multiplayer14`).

A level is the set of entries named in its preload list (type 0, same name as the level):

```
SceneData   = [ "<level>.data" ]
CacheBlock  = [ "vo_splines" ]          (campaign levels)
ShaderPairs = [ ... ]   Textures = [ ... ]   Cubemaps = [ ... ]
Sounds = [ ... ]   WaveBanks_mem = [ ... ]   WaveBanks_strm_file = [ ... ]
Templates = [ ... ]   VoiceSplines = [ ... ]   Ragdolls = [ ... ]
Scene = [ "<level>.lg" ]   SceneScr / SceneTrc / SceneRain / SceneCDT / SceneSM / SceneSLO / SceneVis = [ ... ]
```

`initial_ps`, `initial_textures`, `initial_sounddata`, `initial_sounds` and `initial_tpls` list what is
loaded at start-up for every level (the `ps`, `ini` and `mp_stuff` blocks, shared shaders, strings,
170 common templates). The `.lg`, `.cdt`, `.sm`, `.rain`, `.vis`, `.grs` and `.trc` files of one level
share a 16-byte level GUID at their start. Only 2 of the 39 SceneData files listed are shipped (in the
patch); the rest are absent and evidently optional.

`.lg` is a binary object list in which each object carries its property text (the same syntax as the
`ps` files) and, optionally, script code (section 4). The other binary formats (`.tex`, `.tpl`, `.lg`
records, `.cdt` collision, `.sm` static meshes, `.data`) are not documented here yet.

## 4. Property text and scripts

The `ps` block files (`.cls .sfx .ui .td .ps .clsdb`) and the object properties inside `.lg` share one
text syntax: `key = value`, nested `name { ... }` blocks, lists `[ a b c ]`, `//` comments, inheritance
through `parentDesc = <class>` (and `#parent <block>` inside a file), preprocessor lines (`#ifdef _PS3`)
and verbatim code blocks between `<<<` and `>>>`. Values often have an MP variant next to the
single-player one (`speed` / `speedMP`, `fallDamage` / `fallDamageMP`, `def_weapons` / `def_weapons_mp`).

Two script languages are embedded in it (class names in the executable: `sslLEX`, `smlLEX`, ...):

**SSL** (Saber Script Language): typed, event-driven, `end`-terminated blocks. It appears after a
`#ssl` line in an object's properties inside `.lg` (the mission logic) and as `ssl = <<< ... >>>` in
UI frames (`.ui`). Syntax (illustrative, not game code):

```
override OnEnter
   $door_a.Unlock()
   SetBHV("RUN")   :1.5          // the ":seconds" suffix delays the statement
end

func Check(id : string, level : int = -1) : bool
   var s : string
   if id == ""
      return false
   end
   return true
end
```

Objects are addressed by name with `$name` (`$player`, `$ui`, `$saves_mng`, `$tc`, any level object).
In the 24 campaign `.lg` files: about 6 100 script sections (1.3 MB of SSL code). Most used handlers:
`OnActionFrame, OnBhvEnd, OnInit, OnDestroyed, OnEnter, OnInitLevel, OnComplete, OnUnlocked, OnAlert,
OnLeave, OnHit`; time-power handlers `OnStartTStop / OnStopTStop`, `OnStartTSlow / OnStopTSlow`,
`OnStartTReverse / OnStopTReverse`. Most used calls: `SetBHV, Unlock/Lock, PlaySound, SetPar,
Hide/Show, Create, SetState, Start, ShowMsg, SetSenses, Die, SetAim, SetFollow, ObjectiveAdd/Complete,
AutoSave`. The 14 multiplayer maps carry 310 small sections (31 KB), mostly `Lock/Unlock`,
`EnableZone`, `SetState`: doors, zones, moving parts.

**SML** (state machine language): condition lists in `sml_program { name = <<< ... >>> }` blocks of
`.cls` files (273 of them) and level objects, used for sound and effect reactions to object states:

```
sml_program {
   on_state = <<<
      if State(OPEN) and Health(1,100)
         StartSfx(light)
      else if State(CLOSE)
         StopSfx(light)
      end
   >>>
}
```

There is no compiled script format: all scripts ship as source text and are compiled at load.

### `.psbin`

`timeshift.psbin` and `timeshift_demo.psbin` (type SoundData, language archives) are loaded by
`snd::SYSTEM_FMOD::LoadPSBin`: binary FMOD voice tables mapping voice-line names (`ex_...`) to sound bank
entries. They are not compiled scripts. (`timeshift.psbin` in the executable's string list is this file.)

## 5. Where the co-op relevant parts are

- **Player**: `player.cls` (single player, `parentDesc = plr_comm`), `player_mp.cls`,
  `player_ghost.cls`, `plr_comm.cls` (shared: sounds, state reactions incl. time-power end sounds),
  `plr_model.cls`. Weapons: `wpn_plr_*` classes, `weapon.ps`, `dmg_types.ps`.
- **Level scripts and the player**: campaign scripts address the player as the single global
  `$player` (about 700 references), e.g. `$player.DisableTC`, `StartTControl / StopTControl`,
  `SetTStopLength`, `ClearTimeReverse`, `SetReverseAvailThreshold`, `Transport`, `DisableMove`,
  `DisableInput`, `FailMission`, `ShakeCamera`, `SelectHolster`, `Kill`. Saving: `$saves_mng.AutoSave`
  (131 calls), `saves_mng.cls`. Time-power HUD hints: `$ui.ShowTimeHint`. Time mode switches:
  `$tc.SetMode` (197 calls).
- **Time powers**: the mechanics are native code; data exposes switches and tuning only.
  `game_options.ini` (in the `ini` block): `StartEnergy`, energy regeneration, `ReverseStrength`,
  `DamageOnSlowedOnly`, `bTimeControl`, `bPlayersTimeEnergy`. Objects opt out of reversal with
  `isNoTReverse`. Chrono grenades: `pjl_chrono_slow/stop/reverse`, `ee_grenade_chrono_*`,
  `dsp_grenade_chrono_*`. Multiplayer time shield: `time_shield_sys`, `sfx_time_shield.cls`.
- **Multiplayer**: rules and options in `mp_rules_sys.cls`, `sw_game_options.cls`, `gs_mp_modifier.cls`,
  `gs_map_list.cls` and the `game_options.ini` match settings (respawn, warm-up, time limit, item
  respawn times, overtime). Native systems named in the network dictionary: `time_shield_sys,
  mp_rules_sys, sw_game_options, game_session, mp_str_sync_sys, mp_spectator_sys, mp_voting,
  mp_server_control`.
- **Networking**: the transport is GameSpy-based (`net::PEER_NETWORK`, `net::CHANNEL`,
  `net::GAMESPY_TRANSPORT`, `netDRV_GAMESPY`). `mp_stuff/dictionary.str` is a list of 1302 strings
  (u16 length + text): the native systems above plus the names of synchronised level objects, so the
  network code can send strings as indices (`mp_str_sync_sys`). Classes mark replicated objects with
  flags such as `isSyncSL`.
- **AI**: behaviour presets in `behav.ps` (`IDLE_TENSE_*`, `TALK_*`, ...); per-object `AI { ... }` blocks
  in level objects with `BEHAVIORS` (types such as `CINE`, `CINE_CYCLE`, `RUN`, `URGENT_WALK`,
  `ANIM_CYCLE`, `XNS_HO`; paths of named way points); classes `ai_brain.cls`, `ai_inf.cls`,
  `dom_ai.cls`; scripts drive AI with `SetBHV`, `SetSenses`, `SetFollow`, `SetAim`, `SetTarget`,
  `$AI.NavLink`. Enemy and NPC classes: `soldier_*`, `police_*`, `rebel_*`, `engineer_*`, `vhc_*`
  (vehicles), `tur_*` (turrets).
- **Dialogs and cut-scenes**: `dlg_*.ps` (`dialog { bhv {...} sentences {...} }`), `fmv_*.ps`.

## 6. Overriding scripts without repacking: `pathPS`

The `ps`-family loader (function at `0x98f770`) reads `[Level] pathPS` (default `.`), takes the
extension from the requested name (default `ps`) and opens the file through the generic loader
(`0x980620`), which tries in this order:

1. the cache block whose name equals the **last directory component** of the path: `..\PS` selects
   the block `ps`; the member is found by the CRC-64 of the lower-cased file name (`0x980300`);
2. the archive directories, by file name (`0x97fd60`);
3. the loose file `<pathPS>\<name>.<ext>`, also via the directory cache of registered search folders
   (`0x97ff90`).

Consequences:

- With the shipped setting (`pathPS = ..\PS`), the archived block wins: a loose `..\PS\player.cls` does
  **not** override the archived one. Loose files are used only for names the block does not contain
  (new classes, new UI frames).
- Pointing `pathPS` to a folder with another last component (for example `..\PSMOD`) makes step 1 miss,
  so every `ps`-family file is read loose from that folder. The folder then has to hold the complete set
  under the right names, which the recovered names cover only partly (the unnamed members, mostly
  texture descriptors, would be missing). **(verify** at run time which missing files are fatal.**)**
- The robust route is a new patch archive: copy the `ps` block, replace members by key (no names
  needed), and point every other entry at the base. Level scripts inside `.lg` can only be changed this
  way (or by native hooks), since they are part of the binary scene file.

## 7. Open questions

- Formats of `.lg` object records, `.tpl` templates, `.tex`, `.cdt`, `.sm`, `.data`.
- Whether `useArc = 0` with a full loose tree (the developers' `..\DATABASE`, `..\SCENES`,
  `..\preload_info` layout named in the default `steamwars.ini` of the `ini` block) still works.
- Names of the 2139 unnamed `ps` members (2-4 character extensions other than the nine known ones were
  not found for the sampled ones).
- What `SceneData` holds and why 37 of 39 are missing without visible effect.
