# The game controllers

The display's keyboard and the beams are a poor way to play: a shooter wants two sticks, and a
strategy game a pointer that does not have to be aimed with a whole arm. In the VRChat world
whoever sits in a holodeck's seat in a headset (`docs/holodeck.md`) plays with their own
sticks, triggers and grips from the moment they sit (`EmuGamepad.cs`, one behaviour a
client). Nothing is taken or chosen: the controls are those of the program whose window has
the guest's keyboard.

A controller presses the keys and moves the pointer the games already read
(`docs/input.md`); the guest knows nothing of it. What the guest does say is which program
has the keyboard: the desktop's bar writes a number for it once a second to the control word
at `0x87000068` (`watch_focus` in `linux/nanox/nxbar.c`, the numbers and the names they go
by in `linux/apps/gameid.h`; a word nothing else used), `EmuMachine` reads it from the
readback (`gameId`), and `EmuGamepad.SchemeOf` picks the mapping: Doom and Quake the
shooter's, ClassiCube the blocks', the NES the pad's, anything else (the desktop, Red Alert,
Command & Conquer) the pointer's. A new game with controls of its own needs a number in
`gameid.h`, the same in `EmuGamepad.cs`, and a branch in `Map`. What the controls do is on
the board on the den's left wall (`DoText` in `ShaderEmuGamepad.cs`).

There are none at the den's desk or in the classroom, and no seats there: a controller that
anybody could take anywhere held its player in an unsynced station, which others could not
see and which went wrong when two took it.

## Sitting down and standing up

- Sitting down in a headset is taking it (`EmuHolodeck.Entered`, `EmuGamepad.Take`). The
  beams answer only the seat's console then, and the seat keeps its sitter
  (`disableStationExit`), so that the left stick is the game's and does not stand them up.
  Until 12 October 2026 the console had Shooter, Strategy and Put back keys: nobody could
  press them from the seat, and nobody should have to know which a game wants.
- There is no model of a controller. That a visitor has it is a synced number in their own
  `EmuShare` (`holding`), and the console's line says what the controls are.
- A notice hangs over the right hand, for the sitter alone, for ten seconds after sitting
  down or a change of the controls, and whenever a grip is held: what the controls are, and
  that both grips held for a second stand them up. A bar fills while they are held.
- The console's Exit key does the same. A desktop has no controller (`docs/holodeck.md`,
  "Without a headset"): End stands a sitter up there.
- A hand whose beam is on the console presses the console with its trigger, not the game.

## Shooters (Quake, Doom, by their own default keys)

| Control | Key | In the games |
|---|---|---|
| left stick, up and down | Up, Down | walk |
| left stick, sideways | `,` `.` | step aside |
| right stick, sideways | Left, Right | turn |
| right stick, up and down | `a` `z` | look up and down (Quake) |
| right trigger | Ctrl | fire |
| left trigger | Space | jump (Quake), open (Doom) |
| right grip | `/` | next weapon (Quake) |
| left grip | Esc | menu |
| jump button | Enter | accept |

A stick pushed a little holds its turn key for part of every tenth of a second, more the
further it is pushed: a slow turn from keys that are only up or down. Quake's own mouse look
is not used: the guest's pointer is a place on the display, not a movement, and would stop at
the display's edge.

## The pointer (the desktop, Red Alert, Command & Conquer)

| Control | Does |
|---|---|
| right stick | moves the pointer, faster the further it is pushed (`pointerSpeed`, 700 pixels a second at the end) |
| right trigger, left trigger | the left and the right button |
| left stick | the arrow keys, by which both games scroll their maps |
| right grip held, right stick up and down | the wheel |
| left grip, jump button | Esc, Enter |

On a desktop the same events would come from the keyboard and mouse VRChat walks with, which
is why a controller is not given out there: the visitor's own keyboard plays instead.

## The pad (the NES)

Left stick the pad (the arrow keys), right trigger A (`x`), left trigger B (`z`), the jump
button Start (Enter), right grip Select (Tab), left grip Esc.

## Blocks (ClassiCube)

Left stick walks (`w` `a` `s` `d`), right stick turns and looks (the arrow keys, pulsed as
the shooter's), the jump button jumps (Space), the right trigger digs and the left one
builds (the pointer's buttons, where the pointer was left), left grip Esc, right grip Enter.

## Checking it

In play mode the editor has no headset, so sitting down takes nothing: call `Take` by hand
(`SendCustomEvent` cannot pass the seat; set `held` and `pointer.away` with
`SetProgramVariable`, or test on a build). Then `RunInputEvent("_inputMoveVertical", new
UdonInputEventArgs(0.9f, HandType.LEFT))` and the like on "Gamepad", and read the display
keyboard's `queue` (a key code, plus 65536 while pressed) and the machine's `pointerX`.
Set the machine's `gameId` (1 Doom, 2 Quake, 3 ClassiCube, 6 the NES) and the keys held
must change with it, the old ones let go first. In the guest: `nx; sleep 8; doom & sleep
20`, then the word at `0x87000068` in a snapshot is 1, and 0 again once a terminal has the
keyboard.

## Not known yet

- The automatic controls and the beams on the console have not been in a headset. Whether a
  seat stops the right stick from turning the visitor in VR is untried; if it does not, the
  view turns with the game.
- ClassiCube's and the NES's mappings were written from their key tables and never played.
- Doom has no key for looking up, and neither game a second stick's analogue turn.
