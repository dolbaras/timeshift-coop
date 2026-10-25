# Gamepad support

TimeShift on PC (patch 1.2) cannot use a gamepad. The mod's `dinput8.dll` adds XInput pads (Xbox
360/One/Series pads, and other pads through Steam Input or DS4Windows). It feeds them into the game's
own input and binding system, so the game handles the pad much like the console versions did.

## What the PC build lacks

The engine was built for PC and consoles. Its input layer (`_inpINPUT`, vtable `0xdb7034`) still has a
joystick in its design:

- One float per input code: keys `0x00-0xff`, mouse buttons `0x100-0x107`, joystick buttons
  `0x108-0x127`, mouse axes `0x12c-0x131`, joystick axes `0x132-0x141` and POV hats `0x142-0x151`. An axis has
  one code per direction: the even code is the positive half, the odd code the negative half.
- A joystick poll (`0x7bcce0`) reads a `DIJOYSTATE` into those codes. There is also an "open pad n" function
  (`0x7bca50`: CreateDevice, data format, axis ranges) and a DirectInput enumeration callback (`0x7bcee0`).
- Command bindings (`controls.ini`, `Command` / `DefaultCommand`): five slots per command. The key binding
  screen keeps the joystick in slots 4 and 5 and has a hidden "Joystick" page for them.
- Joystick axes count as analog rate input (command flag `0x10`). The look code then uses the console
  `gamepadSensSplineX/Y` curves of the player class instead of mouse deltas.
- The menus have their own command context (`UI`, ids 1000-1021: `UP`, `DOWN`, `LEFT`, `RIGHT`, `ENTER`, `ESC`,
  `PG_UP`, `PG_DOWN`, ...). Its bindings sit in a table in the exe (`0xec1b80`), with keyboard and mouse only.

The device itself is never opened. The "open joystick" virtual (`vtable+0x78` = `0x7bc9f0`) is a stub,
`xor eax,eax; ret`. Nothing calls the opener or the enumeration callback, so the poll returns at once. The
game loads XInput only to check pad 0's type and to send rumble to pad 0.

## What the mod does

`native/gamepad.c` patches no game code. It replaces three entries of the input class's vtable:

| Slot | Original | Mod |
|---|---|---|
| `+0x60` keyboard poll (every frame, right before the joystick poll) | `0x7bc140` | calls the original, then writes player 1's XInput pad into the joystick codes |
| `+0x2c` rumble (called every frame by the game's vibration mixer, which already honours `JoyVibrOff`) | `0x7bc6d0` | `XInputSetState` on player 1's pad |
| `+0x54` key name for the controls screen | `0x786700` | names such as `PAD_A` and `PAD_LSTICK (Forw)` for the pad codes (the engine would ask a DirectInput device, and there is none) |

It also adds default pad bindings to the game's command tables:

- In-game (contexts `GAME` and `GAME_DEF`): bindings go into the joystick slots 4/5, and only while no
  command has a pad binding there yet. So the player's own pad bindings, once the game has saved them to
  `controls.ini`, are never overwritten. After "restore defaults" the pad defaults come back.
- Menus (context `UI`): bindings are added next to the built-in keys.

The pad is read only while the game window is in front. When the pad is idle, disconnected or the game
is in the background, all pad codes read 0, so a button is never stuck.

Code layout. It follows DirectInput's report of an Xbox pad, so names would match a DirectInput device:

| Pad | Code | Pad | Code |
|---|---|---|---|
| A, B, X, Y | `0x108-0x10b` | left stick right / left / back / forward | `0x132` / `0x133` / `0x134` / `0x135` |
| LB, RB | `0x10c`, `0x10d` | LT, RT analog | `0x136`, `0x137` |
| Back, Start | `0x10e`, `0x10f` | right stick right / left / down / up | `0x138` / `0x139` / `0x13a` / `0x13b` |
| LS, RS click | `0x110`, `0x111` | d-pad up / down / right / left | `0x142` / `0x143` / `0x144` / `0x145` |
| LT, RT as buttons (past XInput's threshold of 30) | `0x112`, `0x113` | | |

The sticks use XInput's recommended radial dead zones (left 7849, right 8689). Outside the dead zone the
range is rescaled to 0..1.

## Default bindings

| Pad | In game | In menus |
|---|---|---|
| Left stick | move (also the bike) | up / down / left / right (4-way, repeats when held) |
| Right stick | look | |
| RT | fire (`ATTACK_1`) | |
| LT | alternative fire / aim (`ATTACK_2`) | |
| RB | time power, automatic choice (`ACTIVATE_TIME_AUTO`) | page down |
| LB | throw grenade (`ACTIVATE_GREN_AUTO`) | page up |
| A | jump, bike boost | confirm (`ENTER`) |
| B | crouch | back (`ESC`) |
| X | use and reload (`USE`, `FORCE_RECHARGE`) | |
| Y | next weapon | |
| D-pad up / left / right | time stop / slow motion / time reversal | up / left / right (repeat when held) |
| D-pad down | next grenade type | down (repeats when held) |
| RS click | melee (`ATTACK_3`) | |
| LS click | zoom (`MINIZOOM`) | |
| Start | pause menu (`UI`), skip cutscene (`STOP_CINEMATIC`) | back (`ESC`: closes the pause menu) |
| Back | objectives / score (`SHOWFRAGS`) | |

## Menus

The menus read the `UI` command context like the keyboard does; no other input path is involved. After the
command evaluation (`IM` vtable `+0x34`, `0x4f3190` -> `0x6595e0`), every command of the active context with a
"pressed" (flag 4) or "released" edge is passed to the element key handlers of the open frames: vtable slot
`+0xa8`, `OnKey(id, down, ...)`, thiscall, `ret 0xc`, called from `0x700776`. The frame (`0xdafca8`, handler
`0x70ad90`) moves the highlight on `LEFT`/`RIGHT`/`UP`/`DOWN` (ids 0x3ed-0x3f0); buttons (`0xdb2a48`, handler
`0x73b410`) act on `ENTER` (0x3f5); lists (`0xdafff0`, `0x70d670`) also take `PG_UP`/`PG_DOWN`. The window
procedure (`0x4ce8c0`) gives the UI only typed characters (`WM_KEYDOWN` -> `ToUnicode`), not navigation.

So the pad's `UI` bindings are all the menus need. Two things are added for the pad while the `UI` context is
active:
- The engine counts any value above 0.0001 as pressed, so the left stick would press a direction right at the
  edge of its dead zone and two directions when held at an angle. In the menus the left stick is a 4-way digital
  direction instead: the dominant axis, once pushed past half way. In game it stays analog.
- Nothing in the engine repeats a held key. A held direction (d-pad, left stick, LB/RB) repeats after 400 ms,
  every 110 ms: its code reads 0 for one poll, which gives the engine a new "pressed" edge.

Note for logs: the `cmd ... on` lines are written in the rumble virtual, one frame after the evaluation. By
then the edge flags are often cleared again, so a menu command shows `flags=0x11` or `0x41` even though its
"pressed" edge was seen and handled. This misled the first live test into "the menus ignore the pad".

## Settings

The settings live in `ts_coop.ini` next to `TimeShift.exe`, section `[Gamepad]`. Every key is optional:

```
[Gamepad]
Enabled=1        ; 0 turns the pad off
Pad=auto         ; auto: player 1 uses the first connected pad; 1-4: that XInput pad
LookSpeed=1.0    ; right stick look speed (on top of the game's mouse sensitivity option)
InvertY=0        ; 1: right stick up looks down
Vibration=1      ; 0: no rumble (the game's own vibration option also applies)
```

Local player 2 (for the planned split screen) gets the next connected pad. `gamepad_slot(1)` returns it;
nothing uses it yet.

Test variables: `TS_PAD=1-4` picks that pad and reads it even while the window is not in front (test
windows never are). `TS_PADLOG=1` writes pad changes, command changes of the active context, rumble and
the camera to the log.

## Test results (2026-10-25, main PC, virtual Xbox 360 pad through ViGEmBus)

Checked in a run (`TS_PAD=1`, `TS_PADLOG=1`):
- The DLL loads, finds the input class (vtable entries as in patch 1.2) and hooks it.
- At the start the bindings were added: 29 to `GAME`, 29 to `GAME_DEF` and 13 to `UI`. All command names were
  found.
- The game read the virtual pad. D-pad presses and the left stick showed up in the log (`buttons=0x0002`,
  `L=0.00,-0.89`, ...).
- The active command context switched from `GAME` to `UI` when the main menu loaded. The game reached its
  menu and loading screens with the hooks in place.

Still to do: the controls screen (how the pad names look in its second column, and what happens to a pad
binding when a key is assigned there); whether `InvertMouse` applies to the right stick.

## Live test (2026-10-25, virtual Xbox 360 pad, main PC)

In a level (`ts2_city_part02`): left stick forward gave `MOVE_FORWARD` and the player walked; right stick
gave `ROTATE_RIGHT` and the view turned; RT gave `ATTACK_1`; d-pad left and up started slow motion and time
stop (effect and energy use visible in the frames).

Menus (2026-10-25, main PC, frame shots on request; runs `work\runs\20261025-062529`, `-062803`, `-063734`):
- main menu: d-pad down/up and the left stick move the highlight; A on "Single player" opens it, B returns; a
  small diagonal push of the stick does nothing; the stick held down repeats to the last item;
- "Single player" -> "New game" -> difficulty, all with the pad; A on "Novice" starts the campaign;
- the opening scenes of `ts2_city_part01`: Start skips the cutscene (gameplay with HUD and "game saved" after it);
- in game: Start opens the pause menu, d-pad moves in it, B and Start close it; Back opens the objectives;
- pause menu -> Options -> Mouse: up/down between rows, left/right change the slider (held: 25 -> 35 in 1.5 s),
  down reaches the bottom row (Back / Defaults / Accept) and left/right move along it; B leaves without saving;
- in game after the menus the left stick is analog again (`MOVE_FORWARD value=0.488` at a half push).

The earlier "menus ignore the pad" was a misreading: the `cmd` log shows the flags one frame late, after the
menus have consumed the edge (see "Menus"), and the frames of that run did not cover the moment after A.
