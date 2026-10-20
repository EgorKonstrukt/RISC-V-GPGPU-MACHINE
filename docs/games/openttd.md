# OpenTTD

`openttd` is [OpenTTD](https://www.openttd.org/) 15.3, the open remake of Transport Tycoon
Deluxe, on the desktop, with the open base sets in place of the original game's files:
OpenGFX 8.0 (pictures, GPL-2.0), OpenSFX 1.0.3 (sounds, CC BY-SA 3.0) and OpenMSX 0.4.2 (music,
GPL-2.0). Nothing of the 1995 game is in the image. The engine is upstream's, compiled with
the patch below; the window, the sound, the music and the mixer are new (`linux/openttd/shaderemu_v.cpp`,
`shaderemu_s.cpp`, `shaderemu_m.cpp`, `mixer_shaderemu.cpp`). The picture is drawn by the 8-bit blitter into a
paletted texture that the GPU draws as one quad; sounds are voices of the sound card, and the
music is its FM voices.

    / # openttd                                         # the title screen
    / # openttd -g                                      # straight into a new game
    / # openttd -g /persistence/openttd/save/NAME.sav   # a saved game
    / # OPENTTD_RATE=1 openttd                          # every 10 s: pictures drawn, game ticks run
    / # OPENTTD_SCROLL=1 OPENTTD_RATE=1 openttd -g      # the map scrolled to and fro, 2 s each way

    wsl -- bash /mnt/c/.../linux/openttd/build.sh   # after Nano-X's build; the first time builds GCC as well

For Linux without an MMU, which the world runs:
`bash linux/nommu/build.sh openttd/build.sh`, then `root.sh` and `image.py`
(`docs/system/nommu.md`, "Building it").

The Start menu has it under Games, the desktop has its icon (upstream's `media/openttd.128.png`,
`linux/apps/icons/openttd.png`), and a `.sav` file opens in it (`docs/system/open.md`). Its
settings and saved games are in `/persistence/openttd` (`docs/system/persist.md`), or in `/tmp`
when there is no such folder.

## The compiler

OpenTTD is C++20 and wants the whole of libstdc++: exceptions, run-time types,
`std::filesystem`, `std::regex`, threads' types. `linux/openttd/toolchain.sh` builds GCC 13.3
for `riscv32-linux-musl` once (about eight minutes) over the toolchain's binutils and musl,
with libstdc++ and libatomic (RV32 has no 64-bit atomics). It is position-independent by default
and has emulated thread-local storage, since a flat program's loader sets up none. Its drivers
`ottd-cc` and `ottd-c++` link a flat program where the work folder is no-MMU Linux's, a static
one otherwise, and put `ehframe.c`'s two marks round `.eh_frame` with a constructor that
registers it: without auxv there is no `PT_GNU_EH_FRAME` for the unwinder to find. The
binary is 14.8 MB, its data 21 MB (13 MB of it OpenSFX).

## What the patch changes

`openttd.patch` is applied to the release by `build.sh`, which writes only the files whose text
changes (a patched header otherwise rebuilt everything: 210 s against 10). With
`-DSHADEREMU=ON` the build defines `WITH_SHADEREMU`, and the changes are under it but for the
industry list's sort and seeks inside `RandomAccessFile`'s buffer, which `host_sum.sh`'s build has
too:

- The build takes the video and sound drivers and `mixer_shaderemu.cpp` in place of `mixer.cpp`,
  needs neither SDL nor Allegro, leaves out `extmidi` (it forks a player), and links Nano-X and
  `gles.o`. `StartNewThread` says no: the game runs on the first core alone.
- `null_v.cpp`: with `OPENTTD_SUM` set, a sum over every tile, the random state, the date and
  the companies' money, at the end of a `-v null:ticks=N` run.
- Defaults: the 8-bit blitter (`support8bpp` hardware), no zoom closer than normal (its sprites
  are 16 times the pixels), and a map of 64 by 64 for a new game (256 takes some sixteen minutes
  to make at 2 MIPS).
- Starting, measured with a one-tick run (`-v null:ticks=1 -g`) from 1.39 billion instructions
  to 350 million:
  - The base sets' MD5 sums are not checked (a whole pass over 20 MB).
  - A sprite drawn only at normal zoom, with no closer zoom shown, makes its zoomed-out levels
    from normal and the closest level is only its size; a font's glyph is encoded from its font
    zoom alone. Making the 4x level of every sprite was a third of a start.
  - `RandomAccessFile` reads 8 KB at a time (each read from the ROM costs some 8,000
    instructions however small), seeks inside what it holds without reading, reads from an
    8-byte boundary (the kernel copies a misaligned read a byte at a time: 100 million of a
    start) and has stdio unbuffered under it.
  - Each base set's sprite offset table is read once, not at every load of the sprites.
  - The industry list is sorted on names made once, not made at every comparison.
  - A river's search marks tiles in a bit a tile, not a hash set (most of making a river).
  - The title screen's game is `make_title.sh`'s, 64 by 64 with two towns (13.7 KB):
    upstream's took 226 million instructions to load.
- Without a game thread `GameLoopPause` draws a world being made, once a second: a picture is
  some 400,000 instructions, and four a second were a third of the making.

- A viewport's blits are made by worker cores (below), and a scroll as wide as the screen is
  one move by the machine's copy (CSRs `0xb0` to `0xb3`, `docs/machine/gpu.md`) in place of a
  `memmove` a row: what wraps round a row's end lands in the strips drawn anew.
  `OPENTTD_MEMOP=0` moves it row by row, `=check` compares the two at every scroll.

## Music

`shaderemu_m.cpp` is a music driver ("shaderemu") that plays OpenMSX's MIDI files on the sound
card's FM voices, as Doom's music (`linux/nanox/doom_sound.c`): a song is turned into notes,
twenty voices of tracks, and the card plays it with no more work from the processor. Its
instruments are DMXOPL's General MIDI bank for OPL (`genmidi.op2`, MIT, fetched by `build.sh`),
double-voice instruments as two notes, the drums from channel 10. Program, volume,
expression, pan and all-notes-off are read; pitch bend is not. A song's start and end as the
music set gives them are kept, and the volume slider sets the voices' volume. OpenMSX is
unpacked into the base set folder: music sets are not looked for in tars.

## The viewport on worker cores

A scroll draws the strips it exposed, about 400,000 instructions a picture at keyboard speed:
some 40% the blits, the rest finding and sorting the sprites. The finding loads sprites into
the cache (allocating and reading files), so it stays the first core's; the blits only read.
Between `ViewportBlitsBegin()` and `ViewportBlitsEnd()` (`gfx.cpp`, round the tile and parent
sprites of `ViewportDoDraw`) the first core keeps each blit (the sprite, its place, clip, mode,
and its remap, a text remap copied), and then every core blits the whole list into a band of
the part's rows (`mcw_for`), a sprite skipped at once when its rows miss the band. Bands meet
where no 16 bytes are in both. Strings and the cargo overlay are drawn after, by the first core.
`OPENTTD_WORKERS=N` asks for N workers (7 by default, 0 draws as upstream).

Scrolling a 640 by 480 window (`OPENTTD_SCROLL=1`, `rvc_cpu` at 2 MIPS), pictures in 10 s:

| | Pictures |
|---|---|
| upstream's `memmove` a row, no workers (`rvc_harness --cpu`) | 22 |
| the machine's copy, no workers | 34 to 36 |
| and 3 workers | 42 to 49 |
| and 7 workers | 49 to 51 |

`RVC_RACES` with 7 workers: 0 undone; the reads as they were are the team's round word.
A per-line table in the sprites (to skip a band's first rows) was tried on the first core
alone and did nothing measurable.

## Is it the same game

`host_sum.sh` builds the same tree for the build computer, without a window and without
`WITH_SHADEREMU`, and runs `test.cfg` (64 by 64, 1950, no competitors). The machine must give
its sums (`tools/test_all.py`, case `openttd`):

| Run | Date | Sum |
|---|---|---|
| seed 1, 500 ticks | 712229 | `9d6fc0a0` |
| seed 2, 2,000 ticks | 712250 | `66880c87` |

## Speed

At 2 MIPS on the desktop (`rvc_harness --cpu --cpu-ips 2000000`): the window says "Loading
OpenTTD..." until the title screen is up, some 85 seconds after the program starts, and Generate
makes a world of 64 by 64 in about 50 seconds, its progress drawn once a second (its Abort
button is not read meanwhile). A game on that map then runs at full speed, every game tick it
asks for, with 28 pictures a second; zoomed all the
way out about 20. Making a world of 128 by 128 takes about four times as long, 256 by 256 sixteen.

## Not done

- Pitch bend and other controllers in the music.
- Finding the viewport's sprites on workers: it loads sprites and writes the drawer's globals.
- Online content, the network game and screenshots in PNG (no libpng) are not built.
