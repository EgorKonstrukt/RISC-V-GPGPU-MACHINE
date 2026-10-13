# Red Alert

Command & Conquer: Red Alert (1996) runs in the Linux image, from the same source tree as
Tiberian Dawn ([Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer)) and
on the same layer under it (`docs/tdawn.md`, `linux/tdawn`): a Nano-X window, the map drawn by
the GPU, the sound card. Its screen is 640 x 400, shown one to one.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh     (first: it makes the C++ driver)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/ralert/build.sh    (two minutes; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & ralert` at the console, or Red Alert in the Start menu's Games (not
tried from there). The data is the 1996 demo's: one Soviet and one Allied mission and
skirmish games, 24 MB, which the build fetches and the repository does not keep. Effects,
speech and the demo's two tunes play on the sound card.

## What the build is made of (`linux/ralert`)

- `build.sh` puts the tree at Tiberian Dawn's commit, applies Tiberian Dawn's patch (the
  shared files) and then `vanilla-conquer.patch` (Red Alert's own files, and two shared
  ones), copies `linux/tdawn`'s `host.c`, `shaderemu.cpp`, `soundio_shaderemu.cpp` and
  `cxxrt.cpp` in, and builds `vanillara` with `-DSHADEREMU_RA`.
- `gl.cpp` is Tiberian Dawn's map renderer with Red Alert's names. What differs: a cell is
  "mapped" once any of it shows and "visible" when all of it does (the other way round there);
  a map may be 128 x 128 cells; a shadow has as many levels of darkness as its pixels ask for.
- The sound pack (`ralert-sound.pak`, 12.8 MB, 294 sounds) is made by Tiberian Dawn's tool.
  The sounds are in MIX files inside the demo's two, behind an encrypted index: the build
  compiles the source's own `vanillamix` for the host and unpacks them with it.

## What was changed for this machine

Everything under "What was changed" in `docs/tdawn.md` that is in the shared files applies.
In Red Alert's own:

- **No hash of the data.** A MIX file is hashed (SHA-1) as it is read into memory, to
  compare with the sum it carries: 24 MB, some 500M instructions before the title.
- **Fading tables** (the palette's nearest colour to each faded colour, 65,000 distances a
  table, forty tables before the first mission) are made by a search that looks only at
  colours whose green is near enough to matter. The same tables.
- **The vortex's tables**, sixteen more for each theatre, are made when a vortex first
  appears. (The GPU's scene does not draw the vortex at all yet.)
- **Music that is not there.** A mission names its tune; the demo has two (Hell March and
  Crush). A tune that did not start was asked for again every frame, and each asking lists
  the folder, a million instructions: now another tune is picked, and after four failures
  none.
- **The radar's cover plate**, 160 x 141 pixels and three buttons, was drawn again whenever a
  unit moved (the unit asks for its dot on a radar that is not there): 160k a frame.
- **An object that moved does not change the cells it left and came to**, and a unit that
  looks again at a cell it has seen does not either. The game asks all of those cells to be
  drawn again, a hundred a frame; on the GPU only the scene is made again, and the renderer
  keeps a mark on every cell that has nothing of its own to draw.
- **The first run** goes past the menu straight into the Soviet mission, as after an
  install: the starter writes the setting that says the game has been run.
- The logic that repeats itself is Tiberian Dawn's, with the state sums unchanged: threat of
  a unit that steps, houses found from a table, the layer's sort, and the computer's count of
  units no team has (a unit is asked whether it could join only when one is wanted).

## The map's cells, a frame

Going through the cells in view was 35 thousand instructions of a frame early in a mission,
when most of the map is under the shroud, and is 19 now (`linux/ralert/gl.cpp`):

- a run of settled cells in a row, or of cells never seen, is passed over or made one black
  rectangle without asking the game about each cell;
- a cell with ore, a wall or a scorch mark keeps the rectangles it drew, as an object does,
  and they are put again until the game says the cell changed;
- a plain cell at the shroud's edge keeps its edge and is asked nothing unless the game has
  flagged it.

The state sums are the same, and a held frame is the same picture as with everything drawn
anew (`RALERT_REFRESH=1`) but for trees below the map's last row, which differed before. The
first Soviet mission: 16 to 18 frames a second in its first 300 frames where it was 15 to
17, 24 to 32 later; its logic is 105 to 130 thousand instructions of a frame's 180 to 200.

## A battle

`RALERT_HUNT=N` sends everything the player has after the enemy at frame N, which is the only
way a mission left alone comes to a battle. The Allied first mission with `RALERT_HUNT=50`:
a frame is 220 to 265 thousand instructions, of which the game's logic is 155 to 190 (it is
105 to 130 in the quiet mission), 14 to 17 frames a second in the harness. The profile of
it has no piece over 3%: threat scans, path finding, the houses' and each object's own turn.
A played mission in the world was measured at 551 thousand a frame (logic 359, drawing 127,
the rest 65), 5 frames a second at the world's 2.9 million instructions a second: the logic
alone would be 8 a second there. That was with the pointer in use, and a pointer that moves
was dear in every part of the frame, not only "the rest": the window system woke for each of
100 reports a second, and the game asked it for the moves wherever it looked for input. The
quiet mission with the pointer going round was 10 to 11 frames a second where it is 19 to 20
with the pointer at rest. The game reads the pointer from the machine's input words now, the
kernel reports a moving pointer 10 times a second while a program runs, and the pointer
itself is the display's cursor: 17 frames a second with the pointer going round
(`docs/input.md`, `docs/tdawn.md`).

A shape the game turns (an aircraft, a missile: `CC_Draw_Shape` with a rotation) was not the
GPU's: the game cleared a buffer, turned the shape into it at twice the size and drew that
on the page, at every drawing. The bomber that flies over that mission was 250 thousand
instructions a frame by itself while it flew, 6 frames a second where the frames around it
were 14. A turned shape is a shape of the atlas like any other now, turned once for each
turn of each frame (`turned()` in `linux/ralert/gl.cpp`): those frames are 255 thousand
instructions and 14 a second, and the state sum at frame 640 is the same (`52fa1019`). Its
picture was not compared with the game's own: the aircraft does not cross the view in that
run. `ralogic`-style counters put round an object's turn said where the logic goes there:
40 soldiers 100 thousand, 5 vehicles 35, 45 buildings 20, the houses 17. A soldier's 2,500
are its ordinary stages (the base class's turn 1,000 to 1,300, moving 600 to 800, firing
350): nothing in them is waste to be taken out in one place. A scan for a target was 25
thousand instructions, some hundreds of cells in rings at two calls each; an empty cell is
passed over without them now (`SHADEREMU_OCCUPIED` in `TechnoClass::Greatest_Threat`), 5 to
8% of the battle's logic, with the sums as they were. Building the game with link-time
optimisation (`-flto`) made a program that ends with "out of memory" at its start: its
globals are made in the order of its source files, which the optimiser's default partitions
do not keep. With a partition a file and nothing reordered it runs, a fifth fewer
instructions a frame, and is how both games are built now (`docs/tdawn.md`, "The whole
program at once").

What a frame costs outside its logic and its drawing ("rest", 25 thousand there) was half
the ground layer's sort, which asked every object for its place twice, by a virtual call:
once now, as in Tiberian Dawn (`LayerClass::Sort`), and the rest is 18.

## Movies, and the worker cores

The demo's opening movie (ENGLISH.VQA, 640 x 400, 156 frames of it before `TDAWN_AUTO`'s Return
ends it) was 41 of the 100 seconds from the command to a mission's first frame: a million
instructions a frame, and more than that in stores. A frame was decoded into a buffer and
copied to the screen, half a megabyte of stores, and the machine's core keeps 6 KB of stores
a pass (its write cache: `docs/multicore.md`).

- **The frame is a picture the GPU shows** (`host_picture`, `host_show_picture` in
  `linux/tdawn/host.c`): decoded once, into GPU memory, and drawn from there, stretched over
  the window if it is one of the small movies. No copy and no doubling by the game.
- **The worker cores decode it** when the machine has any (the D3D11 harness has;
  `linux/ralert/workers.c` is the game's jobs, functions a worker calls in the game's own
  memory, over the library every program has for them, `programs/mc/mcw.c`). A frame's block rows are shared between the
  cores, each writing its own rows of the picture, and the last worker unpacks the next
  frame's block pointers (LCW) while this one is drawn: the player loads a frame ahead for
  that. The movie's codebooks and pointers are allocated in whole 16 bytes with their pages
  already there (`workers_alloc`), so that no core stores beside another's and no worker
  stops at a new page; the picture is GPU memory, which a worker has as the game has it.
- A frame's palette starts on the 16 byte boundary after its pointers: a worker stores whole
  16 bytes to the pointers' very end. (The palette used to follow the pointers as they lay
  before they were moved to a boundary, and its first colours were pointers: a second of
  blue at the movie's start, on four cores only. `RALERT_MOVIE_CHECK` did not see it: it
  compares pictures, and both decodings were right.)
- The workers are the game's while a movie plays and no longer: it asks for them when a
  movie's buffers are made and gives them back when it ends, so that another program (Quake,
  whose server is on one) has them during a mission, where this game has no use for them.
- More workers do not make a movie faster. On the machine of 64 cores it has the kernel's
  fifteen: 156 frames in 14.2 s (its own pace is 10.4 s). Laid out for it (47 workers of 3 KB
  a pass for the rows and a large one to unpack) it was 13.7 s, with the same pictures, and
  that layout was not kept. What a frame takes is this core's own 190 to 214 thousand
  instructions (reading the file, the sound, the frame's bookkeeping), not the rows.
- `RALERT_MOVIE_SOFT=1` is the game's own way, `RALERT_WORKERS=N` uses no more than N workers,
  and `RALERT_MOVIE_CHECK=1` decodes every shared frame on core 0 as well and compares: 0 of
  156 differ. Each movie prints a line (`ralert: movie ...`).

| The opening movie, 156 frames | Time | Core 0's instructions a frame |
|---|---|---|
| As the game does it | 41.4 s | 1,033k |
| The frame shown by the GPU | 30.5 s | 821k |
| and three workers decoding | 19.3 s | 443k |
| and one of them unpacking a frame ahead | 18.6 s | 234k |

From the command to the mission's first frame: 101.9 s, 87.8 s with the picture, 80 s with
the workers; the whole 300-frame check 123 s to 106 s, with the same state sum. What limits
a frame now is stores, not instructions: 16,000 texels of picture and 2,000 of pointers a
frame, 384 a pass a core. In the mission itself the workers do nothing yet, and their being
there costs it 4 to 8% (14.1 game frames a second where one core has 14.5).

## The mission's frame

Where a mission's logic goes (a profile of the Soviet demo mission, frames 550 to 850: 94 to
161 thousand instructions a frame, 75 to 86 of them logic, 18 to 30 game frames a second):
nothing stands out. `HouseClass::AI` and `TechnoClass::AI` are 3.4% each,
`Recalc_Attributes` 2.4%, `BuildingClass::AI` 2.3%, `Greatest_Threat` with its cell
evaluations 5%, and the rest is some hundreds of functions under 1.5% each: 63 objects at
about a thousand instructions an object a frame. A job given to a worker and waited for
costs two passes (16 thousand instructions' time), so an object's turn cannot go to another
core for a gain, and the turns are in an order the state sums depend on. What could go is a
whole part that may be a game frame behind (the computer player's planning, about a tenth),
which changes those sums. A battle with hundreds of units was not profiled.

Found with a profile of the mission alone (a snapshot at `ralert: begun`, resumed with
`--pc-log` and `--ra-log`; `tools/pc_profile.py`, `tools/pc_callers.py` for who calls a leaf
function, `tools/pc_hot_in.py` for where in a function):

- **A house whose teams can want nothing does not count its units for them.** Every house,
  twenty of them, counted its infantry, units and vessels against what its teams want, every
  frame: a thousand instructions a kind a house. What the team types that are built ahead
  ask for is found once a scenario (they do not change), and a house with none of those and
  no team in play skips the counting. 600 frames of the Soviet mission were 512M
  instructions from the command and are 493M: 30k a game frame, a tenth of it. The state
  sums are the same (`01e0dd60` and `ea72a5d1` at frame 300, `fdfd5447` for the Soviet
  mission at frame 600).

What the profile shows after that, of a frame of some 275k: the map's scene a quarter (the
cells' loop 10%, a rectangle's way into the list another 10%), shapes drawn into the atlas
the first time they are seen 7% (it falls as the atlas fills), the kernel's clock tick 7%,
looking for targets 5%, and nothing else above 2%.

## At two million instructions a second

The figures and what the two games share are in `docs/tdawn.md` under the same heading: the
Soviet mission went from 9.7 to 23.6 game frames a second over its first 300 frames (12.6
to 30.9 over 1,200), the Allied one from 9.0 to 20.6 (8.7 to 21.7). Red Alert's own part:

- **A house that takes no part** (`HouseClass::ShaderEmu_Takes_No_Part`). Twenty houses have a
  turn every frame, 900 instructions each, and all but three or four have no object, no
  trigger, no special weapon and nothing asked of them: for such a house the turn comes to
  its timers, which are kept running in their order, and nothing else. A house with team
  types counts as one while none is built ahead for, no team is in play and its team timer
  has not run out. 18 to 13 thousand a frame; a house with only a civilian building or two
  still has its whole turn.
- **A scan for walls asks cells that have an overlay.** A computer's unit that scans for
  targets also asked every cell of the scan whether it is a wall worth shooting
  (`Evaluate_Just_Cell`, a hundred instructions to say no): only a cell with an overlay can be.
- **A trigger that ends** was looked for in each of the map's 16,384 cells by a call that
  found the trigger again every time: 1.6 million instructions, most of a second, when a
  mission's first triggers go. Found once, and only cells that have a trigger are compared.
- **`Shape_Dimensions`**, the smallest rectangle round a shape's pixels, looked at a pixel at a
  time four times over the first time each frame of each shape is asked for, 20 thousand
  instructions for a soldier. A row's first and last pixel are found a word at a time
  (`host_first_pixel`, which the atlas uses too): the same rectangle.
- The credits' tab is drawn at every count of the credits: `BF_Trans` by runs
  (`docs/tdawn.md`) took it from 13 to 9 thousand.
- **The houses' scan bits are made when something changed.** `HouseClass::Recalc_Attributes`
  went through every unit, soldier, aircraft, building and vessel every frame to say which
  kinds each house has, 4 thousand instructions. What it reads changes at a dozen places (an
  object made or gone, into or out of limbo, locked onto the map, discovered, captured, and
  where the bits are written directly): each says so (`ShaderEmu_Scans_Stale`), and a frame
  in which none did keeps the bits. `RALERT_CHECK_SCANS=1` makes them every frame anyway and
  compares: kept in 1,012 of 1,024 frames of a mission and 670 of 768 of a battle, never
  wrongly. A new place that changes `IsLocked`, `IsInLimbo`, `IsDiscoveredByPlayer`, an
  object's house or a scan word must say so too.
- **An anim or a bullet that ends is not looked for in everything's targets**
  (`ObjectClass::Detach_All`): that look asks every object, team, house and trigger, 15
  thousand instructions, for each puff of smoke that ends. Nothing can hold the target value
  of one whose value was never made, and a value is made in two places
  (`AbstractClass::As_Target`, `TargetClass`'s constructor from a pointer), which note it by
  the object's place in its heap (`ShaderEmu_Taken`), for good. Only these two kinds: what
  holds a soldier or a building may hold a pointer (a bullet's `Payback`, a radio contact),
  which no value says. `RALERT_CHECK_DETACH=1` makes the look anyway and sums every byte of
  every object before and after: `the look changed something for 0 of them` (134 in a
  battle of 800 frames). A saved game's values were made elsewhere: loading one notes all.
- **A `CCPtr` is one load.** Every object's `Class->` and `House->` went through the heap's
  buffer and object size (eight instructions, fifteen hundred times a frame: 9% of a battle
  in the profile by line). Each heap that has such pointers has a table of its objects'
  places now, entry 0 empty for the ID that is none (`ShaderEmu_Heap_Table` in `heap.cpp`,
  made again when the heap's buffer is another): 3% of every frame.
- **A unit's path searches** keep what `Can_Enter_Cell` said, as Tiberian Dawn's do
  (`docs/tdawn.md`, "Not done"; `RALERT_CHECK_ENTER=1`).
- **A scan for targets turns most objects away first for the least**
  (`TechnoClass::Evaluate_Object`): whether the object's kind is asked for and whose side it
  is on are tested before its cloak, its zone and its health. Every test only says no, so
  their order does not change an answer.
- **The scan passes empty cells a word at a time**, by Tiberian Dawn's bits of who is
  occupied (`cell.h`), where it has no walls to look for (the player's units, and a computer
  that does not shoot walls). `RALERT_CHECK_OCCUPY=1` compares the bits with the cells every
  frame: `0 cells' occupancy bits were wrong`. Worth 1%: most scans are short.
- **A new object is put into the drawing order with one question of its own**
  (`LayerClass::Sorted_Add`, both games): it was asked for its place at every comparison,
  two virtual calls for each of the 130 objects before it, 4 thousand instructions for every
  puff of smoke that appears.
- **An anim or a bullet does not make the houses' scan bits stale** by coming or going (it is
  in none of the scans): the bits are kept in 986 frames of 1,024 where they were in 928.
- **The view's own half of `Coord_To_Pixel`** is worked out when the view moves, not for
  every object of every frame.
- **A soldier with one weapon** is not asked which of its two is better
  (`What_Weapon_Should_I_Use`), and **a shape that goes to the scene** is not measured
  (`Shape_Dimensions` in `Techno_Draw_Object` is for the game's own drawing).
- **What moves is out of the cells' overlap lists** once the scene is live (both games;
  `SHADEREMU_NO_OVERLAP` in `map.cpp`): the lists say what to draw again when a cell is
  painted over, which the scene never does. Buildings and trees stay in them; an object not
  seen yet still reveals itself as a list would have made it (`ShaderEmu_Overlap_Seen`).
  Anything new that reads `Overlapper` for more than drawing finds no units there.

What is left in the Allied mission, of 89 thousand a frame, is about: 39 soldiers 26, 47
buildings 12, the houses 9, scans for targets 7, the scene 20, the kernel's tick 8. Nothing
in it is waste of the kinds above, and a battle is the same at one and a half times the
size: 15.2 frames a second with everything sent to hunt, where it was 5.6, and 9.1 while
reinforcements land (`docs/tdawn.md` lists what is still under 15). Not done: Tiberian
Dawn's occupancy bits for Red Alert's scan (2 thousand), a unit's scan or path on a worker
core while this one waits (a waited job is two passes: a paired path search was built for
Tiberian Dawn and gained 1%), a soldier that only moved shown from its kept rectangles
(what its `Draw_It` would draw is not known without calling it), and the logic itself on
more than one core, which is what a battle at 15 frames a second would take.

On 13 October 2026 the two missions were measured again with Tiberian Dawn's (the table in
`docs/tdawn.md`, "the long frames, and why a frame is not half"): 29.6 and 21.4 frames a
second on average before, 29.9 and 21.5 after, the lowest windows 17.7 and 10.0 to 18.4 and
9.9. Nothing of that day's work is in Red Alert's own code: its path searches are 2% of a
battle and were left as they are, and what holds its lowest windows is the frame below
and the landing (frames 500 to 560: logic 87 thousand a frame and drawing 54, 11.5 frames
a second). Tried and not kept: the shared code counted for the compiler too, with
only the files of the key arithmetic left out (`-fprofile-exclude-files=/common/[bimprs]`:
the counting build then starts and plays with the right sums); the missions were 29.8 and
21.4.

## The frame in which the sidebar comes

Mended in that day's second round (below, "and what was done"): 1,727 thousand
instructions to 521. As it was found: the longest frame of the Allied mission is frame
254, 1.76 million instructions (most of a second at two million a second, where the frames
round it are 80 to 130 thousand). It is the frame in which the sidebar and the radar's
frame are first drawn whole: pieces of 64 x 192, 160 x 124, 160 x 100 and 160 x 160
pixels, the tabs, two cameos, 165 KB of pixels unpacked, measured for their rows' spans and
copied to the page in one frame. Of frames 252 to 256 (2.72 million instructions):
`Build_Frame` 21% (all of it `LCW_Uncompress`), `BF_Trans` 18% (16 of them
`ShaderEmu_Spans_Of`), `memcpy` 9%, `memset` 3%, and the kernel 28% (pages given and
taken back). It is not the scene's refresh (`RALERT_REFRESH=0` has it too), and the 1.5 s
window it falls in is the mission's lowest (10 frames a second).

Done for it, in the shared code, and worth nothing to that frame (1.78 million after):
`LCW_Uncompress` copies a run of 24 bytes or more a word at a time (`LCW_Back` in
`common/lcw.cpp`: bytes from behind that are closer than their count are a pattern that
repeats, copied in pieces no longer than the distance), which took its part from 20% to
14% and gave as much to `memcpy`; and a row's span is tested for holes a word at a time.
Both give the same bytes: a build that also unpacked everything a byte at a time found 0
of 819 and 1,161 calls different in 600 frames of the two missions (185 more, the
movie's, write into the buffer they read and were not compared), and 2 million random
rows have the same spans. What would mend it: those shapes built before they are wanted,
one a frame from the mission's start or by the worker that unpacks shape files ("Frames
made ready" in `docs/tdawn.md`, which was tried for all of the sidebar's files at once and
lost by their turn in its queue), and the page's part under the sidebar written by rows on
several cores.

**And what was done** (the same day's second round; the figures and what is left are in
`docs/tdawn.md`, "second round: what the long frames are"):

- The sidebar's files are told of when they are found (`ShaderEmu_GL_Ahead_File` in
  `sidebar.cpp`, `power.cpp`, `tab.cpp`; a cameo when its strip gets it; the radar's two
  by name, `ShaderEmu_Radar_Ahead`, because the game finds them only when the radar is
  first drawn), the worker cores unpack them as the mission begins, and the game's own
  drawing copies them from there (`docs/tdawn.md`, "Frames made ready"). Of that frame's
  521 thousand that are left: the copies to the page 240, the page's holes made again 80
  (the sidebar's coming forces everything), the scene from nothing 190.
- **A trigger that ends is looked for in the cells that have one, by a list**
  (`MapClass::Detach`): the 16,384 cells were still gone through for each, 66 thousand
  instructions. The list is made when a trigger first ends and made again after a cell was
  given one (`ShaderEmu_Cell_Triggers_Stale`: the scenario's cell triggers, a saved game's
  cells, the editor). `RALERT_CHECK_TRIGGERS=1` asks every cell as well: `0 cells missed`.
- **A cell that changed is noted once a scene** (`told` in `gl.cpp`): a cell first seen
  tells of itself and its eight neighbours, so a unit's first sight of new land told of
  each cell nine times, 141 thousand instructions of the Soviet mission's frame 130.
- **A turned shape is turned from a ready frame** (`turned()` in `gl.cpp`): the aircraft
  that flies over the Allied mission's start had each frame unpacked by the game first,
  11 to 18 thousand each. The turning itself is still this core's.
- A message that expires, the scene's refresh in turns: `docs/tdawn.md`.

## Speed

On D3D11 with fxc2 (3.5M to 4.1M instructions a second), no delay between frames, nobody
playing, in thousands of instructions a game frame:

| | Objects | Logic | Drawing | A frame | Game frames/s |
|---|---|---|---|---|---|
| Soviet mission, the game's own drawing | 64 | 235 to 300 | 390 to 520 | 660 to 810 | 5 to 6 |
| Soviet mission | 64 | 132 to 175 | 69 to 93 | 222 to 288 | 12 to 18 |
| Allied mission | 92 to 98 | 159 to 173 | 91 to 134 | 289 to 336 | 12 to 14 |

The map's scene is 66k to 106k of the drawing: the view is 20 x 16 cells, four times Tiberian
Dawn's. From the command to the menu is about a minute (190M instructions with the desktop's
own start), and as long again to a mission's first frame.

## Checking it

    TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=300 ralert      (wait for "ralert: done")

- `TDAWN_AUTO=1` presses Return through the menus; `RALERT_SIDE=allies` or `soviet` answers
  the question of sides; `RALERT_SEED`, `RALERT_FRAMES`, `RALERT_STOP_FRAME` (with
  `RALERT_NO_DELAY=1`), `RALERT_STATS_MS`, `RALERT_RENDER=soft` and `RALERT_REFRESH` are
  Tiberian Dawn's switches (a scene from nothing every 256th time). `rastat:` lines say
  where a frame went. `RALERT_HUNT=N` sends everything after the enemy at frame N (the sums at
  frame 800 with `RALERT_HUNT=50`: `f5b3dbae` Soviet, `dd61e2fd` Allied), and
  `RALERT_CHECK_SCANS=1` must end with `0 of them wrongly`. `RALERT_FRAME_LOG`, `RALERT_MARK`,
  `RALERT_MESSAGE_GONE`, `RALERT_READY_DRAW`, `RALERT_CHECK_READY` and `RALERT_CHECK_TRIGGERS`
  are as Tiberian Dawn's of those names (`docs/tdawn.md`); the 1,200-frame sums are
  `9b84ad73` and `709e53d6`.
- A change to the game must leave its `state` sum the same: `01e0dd60` for the Soviet mission
  (SCU01EA) and `ea72a5d1` for the Allied one (SCG02EA) at frame 300 with seed 7.
- The picture against the game's own drawing (`RALERT_RENDER=soft`, both held at one frame):
  5% of pixels differ, by more than 16 levels 0.1% to 0.9%. Those are shadows: a true blend
  where the game's table picks the palette's nearest dark colour, which on snow is 27 levels off.
- Sound: `--fixed-dt 0.004 --sound-capture` and `tools\sound_reference.py ... --rom`; a mission's
  first 300 frames, 83 seconds of sound, are the model's in every sample. `TDAWN_SOUND_LOG=1` says what
  the game asked for.
- Palette cycling goes by the clock (Tiberian Dawn's goes by frames in a run with no delay):
  a few dozen pixels of lamps differ between two runs of one build.

## Not done

- Playing it by hand: the pointer and keys are Tiberian Dawn's code, untried here. So are
  the map's drag by the right button, which is the only way the map moves (`docs/tdawn.md`,
  "The map is dragged"), tested here as there.
- Skirmish, the Allied mission beyond 300 frames, saving and loading.
- The vortex in the GPU's scene; ground units sorted among walls as the game sorts them
  (the scene draws overlays first, then every object).
- Scroll steps sized by time, and frames left undrawn when late (`docs/tdawn.md` has both).
- Movies. The start: a minute to the menu.
