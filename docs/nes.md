# A NES

An emulator inside the emulated machine: Nofrendo (Matthew Conte's, as Espressif keep it for
the ESP32) as a Nano-X client, with two games anyone may pass on. On the four-core machine it
runs at a NES's speed or near it; on one core at a third.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/nes/build.sh       (2 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & nes thwaite` or `nes nova` (the Start menu's Games has both), or
`nes FILE.nes` for any other cartridge of a mapper Nofrendo has. Arrows are the pad, X is A,
Z is B, Enter is Start, Tab is Select and Escape ends. Its sound is the sound card's (below).

The games are Thwaite by Damian Yerrick (mapper 0) and Nova the Squirrel by NovaSquirrel
(mapper 1), both GPL-3; the build fetches their release files.

## What the build is made of (`linux/nes`)

- `build.sh` fetches esp32-nesemu at a fixed commit, applies `nofrendo.patch` and compiles
  Nofrendo with the files below, `programs/linux/gles.c` and `programs/mc/mcw.c`.
- `osd_shaderemu.c` is the machine's side: the window, the pad, the frame and its pace. The
  NES's picture is 256 x 240 bytes of GPU memory, each a place in the display's palette, which
  holds the NES's colours; the GPU stretches it over the window.
- `rc6502.h` is the 6502 recompiled (below); nofrendo's `nes6502.c` includes it.
- `nofrendo.patch` is ours (edit a clean clone at the commit `build.sh` names and save
  `git diff`); the build puts the tree back each time.
- `apu_shaderemu.c` is the sound (below).

## Speed

Instructions of the first core for a frame of the NES (`nesstat:` lines), and the speed that
is of a NES's 60 frames a second at about 4M instructions a second:

| | Thwaite's title | Thwaite, a game | Nova's title |
|---|---|---|---|
| Nofrendo as it came, one core, a frame in three drawn | 704k (9%) | | |
| idle loops counted, the background by table | 292k (22%) | 380k (17%) | 274k (24%) |
| the picture by three worker cores | 162k (39%) | 185k to 215k (26 to 31%) | 153k (42%) |
| the 6502 recompiled, asleep in its idle loops | 49k (116%) | 70k (80%) | 31k (172%) |
| the GPU draws: every frame that differs is shown | 51k (134%) | 87k (80%) | 34k (212%) |
| straight runs unchecked, lines crossed inside translated code, the lists a worker's | 47k (142%) | 63k (106%) | 37k (180%) |
| the line's step with no registers saved, the build optimised as one program | 46k (145%) | 59k (113%) | 36k (185%) |

With the limiter on, the two titles hold 60 frames in 1,000 ms and a Thwaite game 60 in
1,035 to 1,070 ms with 30 of them drawn. The GPU needs no worker cores, so this is what
one core does too. Nofrendo's own drawing on one core (`NES_PPU=here`, a frame in three)
is 205k to 215k, a third of a NES. A game is held to 60 frames a second (`frame_paced`; a test with
`NES_FRAMES`, or `NES_FAST=1`, runs as fast as it goes).

What each step is:

- **A game waits in a loop**, most of every frame: for its interrupt (`cmp zp; beq`, or a
  `jmp` to itself, which is how Super Mario Bros. waits), or for
  the PPU's status to say the picture has reached sprite 0 (`bit $2002; bmi; bvc`). A taken
  branch a few bytes back, or a `jmp` up to 16 bytes back, has one turn of its loop tried on a copy of the registers
  (`idle_loop`, `idle_step` in `nes6502.c`): when the loop only looks (loads, compares,
  tests, branches) and comes round to the registers and flags it began with, the turns left
  in the slice are counted, not run. The status counts as the same only until the cycle
  sprite 0 is hit (`ppu_status_steady`).
- **A slice that ends inside such a loop leaves the 6502 asleep** (`nes6502_idle`): the
  slices after it are not entered at all, only counted from the cycles of the loop's
  instructions, until an interrupt, cycles to burn or a change of the status wakes it; the
  registers are then brought to where the count has got to by doing the instructions
  between to them once more. Scanlines that pass so, with nothing for the PPU to find
  (sprite 0 not in the line, no frame interrupt due, no mapper counting lines), are one loop
  (`ppu_quiet_lines`): about 60 instructions a line where a line was 700.
- **A background tile's row is two words looked up**, not eight bytes each through the
  palette (`bg_words` in `nes_ppu.c`). A line that starts inside a tile is drawn beside the
  picture and copied in: as it came, such a line wrote up to seven pixels before its start.
- **The picture is drawn by the worker cores** (`docs/multicore.md`; `frame_elsewhere` in
  `osd_shaderemu.c`, `ppu_capture` and `ppu_draw` in `nes_ppu.c`). The first core draws
  nothing: it emulates every frame and, for the frames the workers are free for, the PPU
  keeps a record: its memory as the frame began (the name tables and pattern RAM copied only
  when written since) and each line's registers. The workers draw the lines from the
  record, each a share of them, into a texture not shown, and the window turns to it when
  they are done. Sprite 0's hit stays the first core's to find (as nofrendo does for a frame
  it skips). What is lost: a write to the name tables in mid-frame shows a frame late.
  A cartridge whose pattern pages turn as they are drawn (MMC2) is drawn by the first core.
- **As many workers as fit, in teams.** The NES asks the machine for twelve workers of size 5
  (`docs/multicore.md`: they are the machine's, 1.5 KB of new stores a pass, some six lines
  of the picture; `NES_WORKERS` for another number), and takes the workers
  there are where it is refused (three on a machine of four cores). A frame given out and
  heard done costs two passes whoever draws, so one frame at a time is at most a picture
  every five passes: the workers are in teams, a frame a team, as many teams as the game's
  pace allows (`frame_paced`: one more while it is ahead of its sixtieth of a second, one
  fewer when it falls behind; `NES_TEAMS` in a test). It has to be so because busy workers
  make every pass of the machine longer: of 60 frames of Thwaite's title, twelve workers as
  one team showed 22 with the game at 99% of its speed, three teams 43 at 57% and six 55 at
  44% (the graphics card 39% busy with other work meanwhile). The pictures shown a second
  stay near 22 to 25 however they are shared out: what bounds them is the machine's
  instructions a second on all its cores together, not the number of cores.
- **The GPU draws the picture** (`gd_...` in `osd_shaderemu.c`; `docs/gpu.md`, "A layer of
  tiles"), unless `NES_PPU` says workers, inline or here. The background is a layer of
  tiles whose cells are the four name tables two by two (and, under them, each pair's rows
  30 and 31, the attribute bytes a game shows when it scrolls up past the top); the sprites
  are another, a cell or two a sprite; the pattern tables are kept a byte a pixel in GPU
  memory, in pieces of a page so that a cartridge may turn them; a palette is a bank of 16
  for each of the NES's eight of four. The record of a frame is runs of lines: a new run
  only where the game wrote a register of the PPU, so a frame is a few quads a run (the
  colour behind, the sprites behind the background, the background, the sprites in front)
  and a game that scrolls every line is 240 short runs. The cells follow what the game
  writes to the name tables (a log of the addresses), the tiles what it writes to pattern
  RAM. Every frame is drawn, but one like the last is not (twice a second all the same),
  and one in two to four when the game is behind its time. Eight sprites a line are kept to,
  as the PPU does. What differs from nofrendo's own drawing: a sprite in front is not hidden
  by an earlier sprite that is behind the background (8 pixels of a Thwaite game frame),
  and a write to the name tables in mid-frame shows in the whole frame.
  A Thwaite game frame is 30 quads and 14 thousand instructions (`nesgpu:` lines say).
- **The 6502 is recompiled on the machine** (`rc6502.h`): the cartridge's code is translated
  to RISC-V instructions when it is first reached (everything jumps reach from there:
  Thwaite's 3,950 instructions in one go, 140 KB of code), with the 6502's registers and
  flags in registers of the machine, and nofrendo's interpreter runs only what is not
  translated (the flags i and d, `php`, `plp`, `brk`, `rti`, the undocumented, and `jmp ()`
  through a pointer outside RAM).
  A translated instruction does what the interpreter's macro for it does and takes the same
  cycles, and after each the cycles left are tested, so a slice ends at the instruction the
  interpreter's would. A read or write of a device goes to nofrendo's own handler at the
  cycle count the interpreter would have; a write that is not RAM leaves translated code
  after its instruction (it may have turned pages or ended the slice). A jump is straight
  to its target's code inside a 4 KB page, or anywhere when the cartridge turns no pages;
  else it looks the target up as the pages are now. New code is fetched after `fence.i`,
  which ends the machine's pass.

- **A straight run of translated instructions is there twice** (`rc_run_length`, `rc_leaders`
  in `rc6502.h`): as it was, the cycles left tested after every instruction, and a copy with
  no test in it, entered when the cycles left cover the whole run (one test at its top). A
  run begins wherever a jump comes to, so a loop's body is a run from its top, and the
  branch, `jmp`, `jsr` or `rts` that ends it is in the copy and takes the run's cycles with
  its own. Reads of the cartridge are a load through the page table in line, and a write to
  a register of the PPU, the sound or the pads no longer leaves translated code.
  `NES_RC=plain` is without the copies and the next step, to compare.
- **A slice that ends in translated code goes on into the next line from there**
  (`rc_line`, and `nes_between_lines` in `nes.c`): what the frame's loop does between two
  ordinary lines (the PPU's end of one line and start of the next, `ppu_line_turn`, and the
  cycles' book-keeping) is done from inside, and the code never leaves. Not at the line the
  picture ends, the two the blanking begins and ends at, when an interrupt waits that the
  6502 would take, or for a cartridge that counts lines. An awake line was 350 instructions
  round its slice and is about 110.
- **The GPU's list is a worker core's to make** (`gd_draw` as a job, `gd_show`): turning a
  frame's record into quads was 13 thousand instructions of a frame that is shown, on the
  core that has none to spare. The first core now only follows the game's writes and swaps:
  it gives the record to one worker (two records, the worker's and the one the PPU writes),
  goes on with the next frame, and shows the list when it finds it made, which it asks in
  the middle of the next frame and at its end. A frame whose turn comes while the worker
  is still at the one before is not shown (`nesgpu:` counts them: one or two at a level's
  start), and none is left out because the game is late, which was for when showing a
  frame cost this core 13 thousand. The picture is a frame later than it was.
  `NES_LIST=inline` is without the worker; a test (`NES_FRAMES`, `NES_FAST`) has it only
  with `NES_LIST=worker`. The sprites are written straight into the list
  (`seglSpriteSpace`, `seglSpritesUsed`), 50 instructions a sprite where the library's call
  for each was 250, and the eight-a-line count is made only when more than eight sprites
  begin within a sprite's height.

- **The line's step saves no registers** (`ppu_line_plain` in `nes_ppu.c`, called from
  `rc_line`): the line that needs nothing called (no run of a record to begin, sprite 0 not
  on it, no cartridge counting lines) is done by a function that calls nothing, and the
  build is `-flto`, which puts it into `rc_line` whole. About 80 instructions a line where
  the three functions were 150; any other line goes to `ppu_line_between` as before.
- **A cartridge that counts lines goes the same way** (MMC3: `ppu_lines.hblank`): its count is
  made from inside, and when it asks for an interrupt the translated code is left with the
  next line begun and the interpreter takes it first (`rc_leave`). An interrupt asked for
  while translated code has the registers always waits so (`rc_running` in
  `nes6502_irq`), which it did not before. Mega Man 3: 213k to 138k a frame.
- **The background's cells are kept by the memory they are of** (`gd_tables`, `gd_follow`):
  the PPU marks the bytes of its own four kilobytes of name tables a game writes, a bit
  each, and the cells are worked out once for that memory; a table of the picture that
  moves to other memory is copied. Before, every cell of all four tables was worked out
  again whenever the cartridge set its tables, which some do at every write to their
  register with nothing changed: half the frame of Battletoads, Metroid, R.C. Pro-Am and
  Marble Madness (520k, 447k, 391k and 361k a frame; now 211k, 149k, 105k and 76k).

Super Mario Bros. (mapper 0, a visitor's own cartridge: not in the image), in play, with all
of that: 113k to 57k instructions a frame on the first core while the picture stands and
63k to 72k while it scrolls, and with the limiter on 60 of 60 frames shown a second where
26 to 46 were, at 98 to 103% of a NES's speed standing and 92 to 103% scrolling.

Thirty more cartridges were run for this (a visitor's own again: none is in the image).
All give the interpreter's RAM sums. A frame, in thousands of instructions, with the GPU
drawing (`NES_FAST`, the pad's Start pressed four times and Right held), against 65 for
a NES's speed: Balloon Fight 38, Zelda 60, Mega Man 2 65, Tetris 70, Bubble Bobble 73,
Ninja Gaiden 76, Marble Madness 76, Dr. Mario 87, Gradius 89, DuckTales 98, Pac-Man 102,
Double Dragon 104, Excitebike 105, R.C. Pro-Am 105, Ice Climber 114, Mega Man 122, Donkey
Kong 131, Contra 134, Mega Man 3 138, Metroid 149, Kirby's Adventure 180, Castlevania
188, Battletoads 211, Punch-Out!! 325. What the slow ones have:

- Donkey Kong, Ice Climber and their kind wait by turning their random numbers, a `jsr`
  and eight `ror` a turn: work all frame, 1.4 instructions a cycle of the 6502, and a
  line's step at every line. Nothing sleeps.
- Castlevania reads the PPU's status in a loop that is not one that only looks: a fifth of
  its frame is `rc_rt_read`, `rc_slow_read`, `mem_readbyte` and `ppu_read`.
- Battletoads writes pattern RAM and the PPU's registers all frame (`ppu_write` and its way
  there 15%, `gd_tile` 2%).
- Punch-Out!! (MMC2) turns pattern pages as they are drawn, so the first core draws it.
- An `rts` that kept where it went last was tried (a key and an address at the
  instruction, a turn counted when pages are set): 4% off Donkey Kong, and 5% on to a
  cartridge whose returns go to many places. Not kept.

Where the first core's 70k of a Thwaite game frame go (measured at 85k, before the last cuts): the quiet lines 17%, the interpreter
for what is not translated and the way in and out of translated code 15%, the lines' records
9%, translated code 10%, the kernel 13%.

Another cartridge, measured (Super Mario Bros. + Duck Hunt, mapper 66; not in the image):
its three programs wait in three ways, and one of them was not known.

| | a frame, before | now |
|---|---|---|
| Super Mario Bros., its title | 175k (31%) | 98k (56%) |
| the cartridge's menu | 192k (28%) | 192k (28%) |

- Super Mario Bros. waits in `jmp` to itself: no branch, so no loop was looked for and every
  line of every frame ran it. That is the row above. What is left of its frame is the game
  (it is long in its interrupt) and what a line costs.
- The menu passes time in `lda $2002; inx; bne` four times 256 to a frame: X and Y change, so
  it is not a loop that only looks, and each read of the status is the device's handler
  from translated code (a profile of it is a third `ppu_read`, `mem_readbyte`,
  `rc_slow_read` and `rc_rt_read`).
- Duck Hunt's main loop turns its random numbers (`jsr` to a shift of four bytes of RAM)
  until the interrupt sets a flag: work whose every turn shows in memory.
- `jmp ()` through a pointer in RAM is translated now (the game's table jumps are all that):
  it took 2% off the times translated code is entered and nothing off the frame.

Not done: a read of the PPU's status from translated code without the handler, when it
changes nothing; a second pad, saved games; the first core's own drawing (one core) from
the record, which would let it skip more frames.

## Sound

No sample is computed. The NES's five channels are five voices of the sound card
(`docs/sound.md`), each playing a wave that lies in sound memory from the start, and
`linux/nes/apu_shaderemu.c` only tells the card how fast and how loud: the card reads a
voice's step and volume all the time. Nofrendo's own synthesis (`apu_process`) is never
called; its register handlers call `se_apu_write` first and are otherwise as they were, so
what a game reads back at `$4015` is what it read before.

| Channel | The voice plays | Pitch | Loudness |
|---|---|---|---|
| two pulses | a turn of 256 samples, one for each of the four duties | step from the 11-bit period; the sweep moves it | the envelope or the fixed level, 0 to 15 |
| triangle | its 32 steps | the period; a step of 0 when the length or linear counter stops it, so it stands where it was, as the NES's does | fixed |
| noise | the shift register's 32,767 bits, or the 93 of its short mode | one of 16 rates | envelope or fixed level |
| DMC | a recording decoded to bytes, in one of six slots | one of 16 rates | fixed |

- **The frame counter** is four calls a frame, at lines 65, 130, 196 and 261 of the picture
  (`se_apu_quarter`; `nes.c` compares the line with one number): envelopes and the
  triangle's linear counter at each, length counters and sweeps at every other. Each is a
  few stores to the voices' words. A game's frame did not move: 60 thousand instructions.
- **Workers.** With the channels on the card there is nothing to give a worker every frame.
  Two things are work, and both are a worker's when the program has one (the second of the
  two it opens; the first makes the GPU's lists): the long noise table at the start, and a
  DMC recording the first time a game plays it (up to 32,648 samples from one bit each).
  The first core copies the recording's bytes out of the cartridge, posts the job, and
  starts the voice at the quarter of a frame after the worker is done; the same recording
  again is found in its slot (by address, length, start level and first bytes).
- **A game that runs slower than a NES plays its tune slower**, with no gaps: nothing is
  queued that could run dry.
- Loudness is the NES's own mix in its linear form (a pulse 0.00752 a level, the triangle
  0.00851, noise 0.00494, DMC 0.00335), 1.2 of it to the card's full scale.
- Without a sound card (`rvc_cpu`, `NES_SOUND=0`) the channels are followed all the same.

Not done: writes to `$4011` as a way to play samples (the level is only where the next
recording starts), the DMC's interrupt, the frame counter's interrupt and its five-step
timing (five steps are run at the four-step pace), and the exact `$4015` (it is Nofrendo's:
a channel counts as sounding from its first note on).

To check it:

- The sums below hold with it, on a machine with a card and without.
- `NES_APU_PRINT=1` prints every frame's channels (`nesapu: FRAME p1 PERIOD LEVEL DUTY p2 ...
  tri PERIOD RUNS noise RATE LEVEL SHORT dmc PLAYING`): under `rvc_cpu`, a Thwaite game
  (`NES_PRESS="20 start 50 start 80 a 100 left 110 a"`) has pulse period 253 (440 Hz).
- The card itself, as any program's sound: `--fixed-dt 0.004 --sound-capture F.wav
  --save-state F.snap` with `nes thwaite & sleep 14`, then `tools\sound_reference.py F.wav
  F.snap --rom build\images\linux\rootfs.bin` must say MATCH.
- `NES_APU_TEST=1 nes thwaite` on the shader machine plays a recording made of the
  cartridge's first bytes (neither game of the image has one) and must print `the same as
  this core's` for what the worker decoded; `mctest 255 20000` passes beside it.

## Testing

- `NES_FRAMES=N` ends after N frames with `nes: done, picture SUM`; `NES_PRESS="20 start
  80 a"` presses buttons at frames, so a run repeats; `NES_SUM=1` adds a sum of the 6502's
  RAM after every frame, which is the one to trust: the last picture was the same through a
  mistake that the RAM's sum showed (a loop taken for idle at sight: a branch can be come
  to from elsewhere than the instruction before it).
- The sums a change to the emulator must leave, with `NES_SUM=1 NES_PPU=inline`:
  `NES_FRAMES=120 nes thwaite` RAM `31a18295`, picture `afcb4a98`; with `NES_FRAMES=180
  NES_PRESS="20 start 50 start 80 a 100 left 110 a"` RAM `2f58587d`, picture `39c90e07`;
  `NES_FRAMES=120 nes nova` RAM `3a2e5c47`, picture `d8a04406`. A cartridge that turns pages
  and waits in `jmp`, if one is at hand (Super Mario Bros. + Duck Hunt, `NES_FRAMES=600
  NES_PRESS="60 start 200 start 400 right"`): RAM `3b4dae3d`, picture `8f460adf`.
  Super Mario Bros. alone (mapper 0), `NES_FRAMES=600 NES_PRESS="60 start 200 right 260 a
  300 right 400 a"`: RAM `b8a863d7`, picture `208ec03b`. In `NES_PRESS`, `+right` holds a
  button down from its frame and `-right` lets it go.
- `NES_MARK=N` prints `nesmark` at frame N: `rvc_cpu --pc-log FILE --pc-from nesmark --ticks 256
  --pass-ms 0.0078` then samples the first core from there, for `tools\pc_profile.py` with
  `nes.nm` (a profile of a cartridge in seconds; the pass's milliseconds go down with its
  instructions, or the kernel's timer is a quarter of the profile).
- Each of these must give the same: `NES_RC=plain`, `NES_LIST=worker`, `NES_CPU=interp` (no recompiler), `NES_PLAIN=1` (every
  turn of an idle loop run), `NES_PLAIN=2` (loops passed over, nothing asleep), `NES_PLAIN=3`
  (asleep, no quiet lines), `NES_PPU=workers`, `NES_PPU=inline`. `NES_PPU=here` is nofrendo's own drawing, a frame in three: its
  frames are counted otherwise, so it has sums of its own (`b6f2e9ee`, `451f1f1d`,
  `70810528`), the same with each switch.
- All of that is instructions, so `rvc_cpu` does it (`docs/cpu-harness.md`), workers and
  all: the three runs in two seconds, with the same instructions a frame as `rvc_harness`.
  Speed in real seconds and pictures are `rvc_harness`'s.
- The GPU's drawing is checked at a held frame, where the first core draws the same record
  too: the window against that picture (`NES_HOLD`, below: no pixel different in the two
  titles and in Nova scrolled; the 8 above in a Thwaite game), and the GPU's target against
  its software model (`--gpu-capture`, `tools\gpu_reference.py SNAP BMP --size 512x480`:
  every pixel away from a triangle's edge the same).
- `NES_HOLD=N` stops at frame N with the window up and prints where the picture is in GPU
  memory (bytes past the palette at 0x87000400): in a snapshot from `rvc_harness`,
  the window's layer must be that picture through the palette, each pixel twice, and the
  workers' picture the same as `NES_PPU=inline`'s, with no pixel different.
- `mctest 3 120000` must print `PASS` before and after `nes` in one boot, and after one
  killed while its worker made lists (`nes nova & sleep 8; killall -9 nes.bin`).
- A key let go between two of the emulator's looks at the keyboard (every fourth frame)
  stays down until the next: a tap would otherwise never reach the game.
