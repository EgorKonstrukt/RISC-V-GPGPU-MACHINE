# Quake

Quake (1996) runs in the Linux image: id Software's released source, GLQuake, as a Nano-X
client. Everything on the screen is drawn by the machine's GPU; there is no software renderer
in the build. The machine's float instructions (`docs/fpu.md`) were added for it.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/quake/build.sh     (3 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & quake` at the console, or Quake in the Start menu's Games (not
tried from there by hand). With no arguments it plays its three demos in turn, as Quake does,
and Escape brings its menu up, where New Game is; the Start menu starts it with the menu up
already (`quake +menu_main`). `quake +map e1m1` starts the first level. The data is the
shareware episode's `pak0.pak` (version 1.06), which the build fetches and the repository
does not keep.

Lines typed at the terminal it was started from are console commands (`map e1m2`,
`timedemo demo1`, `hold`), which is also how a test drives it through the machine's serial
console. The window's keys and buttons go to the game as Quake's own (`vid_shaderemu.c`), and
`in_mouse 1` has the pointer's travel over the window turn the view; neither has been tried
by hand yet, only the terminal's commands.

## What the build is made of (`linux/quake`)

- `build.sh` fetches the source at a fixed commit, applies `quake.patch`, adds the files
  below and compiles GLQuake's file list without its x86 assembly, with the null sound, CD
  and network drivers. Floats are the machine's instructions, constants are single precision.
- `qgl.c` is the OpenGL GLQuake calls (`include/GL/gl.h` renames each call to `qgl...`): the
  1.1 calls it uses, glBegin and floats, turned into what `programs/linux/gles.c` (its float
  build) draws. It also keeps the textures' pool, the models' poses and their light tables.
- `world_shaderemu.c` is the level and the models (below).
- `mesh_shaderemu.c` stands in for `gl_mesh.c`: a model's triangles as they are, where GLQuake
  searched each model for long strips.
- `fmath.h`, `fmath_shaderemu.c`: sine, cosine, tangent and arctangent in floats. The source
  calls C's double functions, and the C library's float ones work in doubles inside.
- `vid_shaderemu.c` is the window, the frame and the input; `sys_shaderemu.c` the system.
- `snd_shaderemu.c` is the sound (below), in place of the game's mixer.

## Sound

The machine's sound card plays it (`docs/sound.md`): a sound is a voice pointed at its WAV
where the pak file lies in the ROM, with the WAV's own loop point, and nothing is loaded or
mixed by the CPU. The game's rules stay: which of eight channels a thing's sound takes, and
how loud it is in each ear by distance and side, worked out once a frame.

- A level's own sounds (torches, hums) are loops with a place. The 18 loudest that are heard
  have a voice each; one that is out of hearing gives its voice up.
- Water and sky are two more voices, as loud as the viewer's leaf says.
- Its level is half the game's own: a fight's mix ran into the limit in 1.5% of its samples
  at the game's level, and does in 0.1% at this. `volume` is the game's setting, as before.
- No music: the game's was a CD's tracks.
- To check it: `--fixed-dt 0.004 --sound-capture F.wav --save-state F.snap` on a timedemo and
  `python tools\sound_reference.py F.wav F.snap --rom build\images\linux\rootfs.bin`. A minute
  of demo1 is 3,072,768 samples, every one the model's.

## How it is drawn

- **Textures stay palette indices.** GLQuake's 3dfx path for 8-bit textures is the one used,
  without its rescaling to powers of two and without mipmaps: the GPU looks each texel up in
  the palette, and index 255 is a hole where the texture has any.
- **The level is kept in GPU memory** (`seglKeep`, `docs/gpu.md`), as Doom's is: its polygons
  are made into quads when it loads, sorted by texture, and once more sorted by light map,
  and a frame says which runs of them to draw. The light maps are a second draw that
  multiplies (pass 3) with the light map as a texture of grey words.
- **What is in view** comes from the level's own table of which leaves see which (its PVS):
  the leaves the viewer's leaf sees are listed when the viewer enters a leaf and tested
  against the view's four planes in integers. GLQuake walked the whole BSP tree every frame.
- **The sky** is two kept commands over the same vertices, their texture laid on the picture
  (`FRAGMENT_LAID`), and water is drawn a polygon at a time with the game's clock.
- **A model's pose is the file's own bytes.** A vertex of a model is four bytes (x, y, z and
  which of 162 normals); a pose's are copied into GPU memory as they are and drawn as packed
  vertices, with the model's texture coordinates in a buffer beside them. Its shading is the
  same vertices drawn again, multiplying, each coloured from a table of the light at every
  normal (`VERTEX_TABLE`): a table a light and a turn, 128 of them kept.
- **A particle is one vertex** (`VERTEX_POINTS`): the GPU makes GLQuake's triangle of it, and
  its size. Its colour is a palette index: a 16 x 16 texture holds every index once.
- **A flash** is the glow GLQuake draws by default (`gl_flashblend 1`), a square of a round
  picture, added. Lighting the walls with it instead made their light maps again each frame.
- **The 2D screen** (status bar, console, menus) is rectangles of textures in the one blended
  pass with no depth (`seglSprite`), so that it keeps the order it is drawn in.
- **GPU memory is a cache.** Every texture is kept in ordinary memory as the GPU will read
  it; GPU memory holds the ones being drawn, the level, and the poses and tables, and what
  was drawn longest ago makes room (a pose first). It is 7.2 MB: the 3.3 MB a GL program has
  and the display's own framebuffer, which nothing uses while the desktop is layers
  (`seglMemorySpare`). A level and a fight use 2 to 4.5 MB of it.

Not as GLQuake draws: no model shadows, no wobble of the world seen from under water, no
depth trick for the weapon (it can poke into a wall), nearest texels everywhere, and a
model's light in steps of 1/32.

## Smooth textures

The game asks for `GL_LINEAR` of the level's textures, its light maps, the models' skins, the
sky and its pictures, and for `GL_NEAREST` of the console's letters, and is given both: the GPU
weighs the four texels round a point for a texture so marked (`docs/gpu.md`, fragment flag
`0x400`; `qglTexParameterf` in `qgl.c`). The light is what shows most: a light map's texel is
16 units of wall, and was a square of one brightness. There are no smaller copies of a
texture for far walls (mipmaps), so those shimmer as they did. It costs the machine nothing
that can be measured (the same frames a second on e1m5), the picture agrees with
`tools/gpu_reference.py` within 2 levels at every pixel, and `QUAKE_FILTER=nearest` is the
picture as it was.

## Speed

A frame standing still at the start of the first level, in instructions:

| | A frame |
|---|---|
| GLQuake as it is, floats in software | 3,430k |
| the level and models in integers | 1,810k |
| the machine's float instructions | 752k |
| the GL library in floats, our own sine | 576k |
| the level kept in GPU memory | 390k |
| the game's clocks as floats (the game's time; the program's clock came later, below) | 263k |
| poses and particles as the GPU takes them, 2D by `seglSprite` | 195k |

The machine runs 3.7 to 3.9 million instructions a second (D3D11, fxc2), so 250k a frame is
15 frames a second. `quakestat:` lines, every five seconds, say where a frame went:

| | A frame | Server | In view | Level | Models | Particles, weapon | 2D, swap | Frames/s |
|---|---|---|---|---|---|---|---|---|
| e1m1, at the start | 195k | 111k | 40k | 1k | 4k | 3k | 19k | 19 |
| the start map | 221k | 64k | 43k | 40k | 30k | 8k | 19k | 17 |
| demo1 (e1m3), walking | 205k | 0 | 26k | 1k | 59k | 21k | 45k | 18 |
| demo1, a fight | 296k | 0 | 30k | 1k | 93k | 73k | 26k | 13 |
| e1m5, at the start | 509k | 356k | 35k | 34k | 35k | 5k | 18k | 7.5 |

- A fight has about 11 models and 400 to 700 particles in view. A particle is some 70
  instructions, most of them the game moving it.
- e1m5 is the game's server: its monsters' moves and looks are traces through the level
  (point tests 11%, the QuakeC interpreter 11%, line traces 8%). Nothing there is drawing.
- "Level" is light maps made again: surfaces whose light flickers, ten times a second.
- The game slows down rather than skipping when a frame is longer than a tenth of a second,
  as Quake always has.

From the command to a level's first frame is about 30 seconds, and of a level's own loading
half is its light maps being placed (`AllocBlock`, a third) and the kept level being built.

    QUAKE_HOLD=N        keep frame N on the screen and stop there ("quake: holding frame N")
    QUAKE_FILTER=nearest  every texture's nearest texel, as before the GPU could weigh four
    QUAKE_STATS_MS=N    how often the quakestat: lines come (5000; 0: never)
    hold                the console command: the frame after next stays
    -nokeep             draw the level a polygon at a time, as before it was kept
    QUAKE_DEFINES=-DSE_COUNT    (to build.sh) counts of the server's work in the quakestat: lines

### A frame gone through again (12 October 2026)

`timedemo demo1` went from 182 to 119 thousand instructions a frame (its worst and best
windows of 40 frames by 31 to 40%), a fight's frames from 338 to 230, and the time from the
command to a level's first picture from 108 to 65 million (124 to 76 for e1m1), with the
same sums and the same pictures. In thousands, a frame's parts as `quakestat:` has them
(in view / level / models / particles and weapon / 2D and swap / rest): 17.4, 29.3, 45.9,
22.1, 31.9, 33.2 before; 12.4, 16.7, 35.3, 16.1, 11.6, 24.3 now.

- The 2D screen is runs of sprites the program writes itself (`qglRect`, `qglText`,
  `qglPicture`; `seglSpriteSpace`): a letter was a glBegin, four vertices and a glEnd, about
  350 instructions, and is about 30. The status bar's four `sprintf` a frame are digits.
- Polygons drawn one after another (a door's faces and their light maps, water) are written
  straight into the frame as the two triangles the GPU makes of a quad (`qglQuadsRoom`):
  room for 64 is taken at once and the rest given back when anything is set. A draw is 200
  instructions before its first vertex, and `seglCompact`'s copy was 80 a quad.
- An entity's matrix is made of the world's in one go (`qglEntity`), sum for sum what
  gles.c's translate, rotates and scale make (its sine of 90 degrees is not 1: a rotation
  cannot be left out or simplified loosely).
- Particles are written where the GPU reads them, and `rand()` is the program's own copy
  of the library's (the same numbers, `sys_shaderemu.c`), inline in `r_part.c`: an explosion
  asks for seven thousand.
- What nobody read is not made: the nodes' marks (`R_MarkLeaves`), the palette's ramps
  (`V_UpdatePalette`), the view model's own light, `bitcounts`.
- The client looks only at entities that had an update (`se_entity_live`), brush models
  only at the light maps they used, and gles.c is told only state that changes.
- Loading: light maps are placed by looking only where a block's height changes and never
  in a block that failed for a smaller size (the same places: a sum over `light_s`,
  `light_t` and the kept vertices says so); the kept level's surfaces are sorted once;
  nothing is byte-swapped; the Hunk clears memory when it is taken, not also when it is
  given back, and not at all under a file read into it (`Hunk_AllocUnset`); the palette's
  gamma 0.7 is a table (768 `pow` in doubles before).
- The build is `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`: 6% fewer
  instructions a frame than `-O2`.

`QUAKE_STATS_FRAMES=N` gives a `quakestat:` line every N frames instead of by time (the same
frames each run: two builds compare line by line), and a level's first line says how many
instructions it took from the start.

Tried and not kept: water's corners shared between polygons (more: only two polygons share
a corner, and each then needs a conversion). Not attempted: `RecursiveLightPoint` (4% of a
frame; a shorter ray would not be the same bits), the poses' `memcpy` (2%).

Checking such a change: a held frame of a timedemo is the same picture each run only with
`+host_framerate 0.05` (without it the flash over the picture fades by real time, and every
pixel can differ by a level or three); `+map` needs `+host_framerate 0.05 +con_notifytime 0
+scr_conspeed 100000` and `QUAKE_SERVER=inline`. Frame 150 of demo1 is a fight, frame 420
has models, text and the status bar. `+forward` typed after "track 30:" lands in whichever
server frame is running: two builds of different speed walk the same way a frame or two
apart and stop at the same place. A test that types console commands runs `quake` in the
foreground.

## No doubles in a frame

The machine has float instructions and no doubles: a double addition is a call of some 100
instructions, a division of over 200. What the game still did in doubles every frame is gone:

- **The clock is whole milliseconds.** `realtime` was a double of seconds; it is
  `realtime_ms`, an unsigned count (`Sys_Milliseconds`, the machine's clock word), and the
  main loop hands `Host_Frame` the milliseconds that passed. A stamp is a count, a time
  since one a float (`REALTIME_SINCE`), a thing that only turns takes the count's remainder
  (`REALTIME_TURN`: the cursor's blink, the sky of `-nokeep`). A float of seconds would have
  been too coarse after some hours; the count wraps after 49 days and differences are right
  across that. The old `Sys_FloatTime` had that fault already: under the build's single
  constants its sum was a float's, in steps of 2 ms after 4.6 hours of the machine.
- **An entity's time of its last message, the server's time of its last look for a client
  and the view's time of its last stop** were doubles compared with the game's time, which
  is a float: floats now, and the same comparisons.
- **A variable's value** asked for by name (QuakeC's `cvar`, a few times a monster's frame)
  was read from its text again, in doubles; it is the value kept beside the text.
- **The projection**: OpenGL's doubles are floats (`include/GL/gl.h`); the tangent of the
  view's angle is still worked out in doubles, once for an angle, so the picture's numbers
  are the same to the bit.
- **The network keeps its doubles** (`net_time`, a connection's stamps: the timeouts of
  `net_dgrm.c`), and so do the timers nobody leaves on (`host_speeds`, `r_speeds`,
  `timerefresh`). `Sys_FloatTime` works its double out only when the clock has moved, and
  the network's poll asks only when something is scheduled. In a single-player game that
  is two or three conversions a frame.
- `RotatePointAroundVector` had no prototype: its float angle went as a double and arrived
  as nothing sensible. Only a `fov` other than 90 gets there.

| | The double routines, of the game's instructions | A frame |
|---|---|---|
| `timedemo demo1`, before | 3.0% | 186.6k |
| after | 0.00% | 180.0k |
| e1m5 with the server on the first core, before | 2.9% | 472.7k |
| after | 0.15% | 456.0k |

(`rvc_cpu`; the share from `--pc-log --ticks 256 --pass-ms 0.0078`, after loading.) The sums
of e1m5 are the ones from before in all three server modes, on `rvc_cpu` and on the shader
machine; with `+host_framerate` a frame's length never came from the clock. Without it a
frame's length is now the milliseconds times 0.001 in a float, where it was a difference of
doubles: the last bit may differ, and a typed `+forward` lands a frame earlier or later,
because every frame is shorter. The play checks below and the two machines' game of
`docs/lan.md` pass as before. To find such a routine's callers: `objdump -d` of
`~/shaderemu-linux/src/quake-build/quake`, every `jal` to a `__...df...` by the function it
is in, beside that function's share of a profile.

## A pass's instructions

A pass of the machine is up to 8,192 instructions, and what ends one early loses the rest
(`docs/boot.md`, "Passes that end early"; `tools\tick_util.ps1` measures it). Quake ended
one a frame at its swap and some more at its particles:

- **The swap is not waited for** (`seglSwapEarly`, `docs/gpu.md`): `qglSwap` submits the list
  and the next frame's sound, input, server and demo run while it is drawn. What the list
  may still read is settled for where it is written: a texture, a pose or a light table
  going into the pool (`resident`, `qglModel`, `qglLights`, the texture uploads, `qglLight`,
  `qglBlock`) and the palette. Everything else a frame writes is its own set. The kept
  level's commands settle in the library, which is where a frame's first wait now is: after
  the leaves are marked.
- **A particle is three texels of 16 bytes**, and the array is aligned to them: its place
  and colour, its speed and ramp, the rest. A frame stores to the first, or the first two.
  They were 44 bytes at any alignment: three or four texels a particle of the write cache's
  384, which a fight's 400 to 700 particles filled several times a frame.

- **A frame is refused under 10 ms, not under 13.9** (`Host_FilterTime`, 100 a second where the
  game has 72). On e1m1 a frame is 45 thousand instructions, 13 ms: it was refused, and the main
  loop, which waits for the clock by ending the pass, ended one to three passes a frame doing
  so. e1m1 from where it starts, on the harness: 89.7% to 98.2% of the passes' instructions,
  3.28M to 3.46M instructions a second on the first core; the game draws 68 frames a second.

| `timedemo demo1`, the shader machine | utilization | passes a full write cache ended | 969 frames |
|---|---|---|---|
| before | 96.5% (the swap 2.4%, the write cache 0.8%) | 1,318, of them 947 in the particles | 64.4 s |
| after | 99.2% (the write cache 0.5%) | 615, 224 in the particles | 59.1 s |

e1m5 with the server on a worker, from the command to frame 260: 96.7% to 98.0%. What is
left there and in the demo is a level's start: its textures copied into the pool, 6 KB a
pass, which is all of new stores a pass keeps (`memcpy` from `resident`).

    QUAKE_SWAP=wait       every list waited for, as it was
    QUAKE_HOLD_LATE=1     with QUAKE_HOLD=N: frame N stays while the game makes frame N + 1
                          ready, and stops before it is shown

To check a change to what is written when: `QUAKE_HOLD=420 QUAKE_HOLD_LATE=1 quake +timedemo
demo1` with `--gpu-capture`, once with `QUAKE_SWAP=wait` and once without: the two pictures
of frame 420 must be the same pixel for pixel. A plain hold cannot show a store that came
too early, since the game stops before it makes one.

## The server on a worker core

A frame of a single-player game is the server's (physics, the monsters' QuakeC) and then the
client's (the picture). On e1m5 the server's part was 355 of a frame's 515 thousand
instructions. On a machine with worker cores (`docs/multicore.md`; the D3D11 harness has them)
the server's physics runs on one, as a function of the game in the game's own memory
(`linux/quake/server_shaderemu.c`), and the two go at their own pace:

- **The client does not wait.** When the server's frame is still out, the client's frame goes
  on without one: it draws from the server's last two answers, moving things between them,
  which is what Quake's client does in a network game (the game only switched that off for a
  server in the same program). When the server's frame has ended its messages are sent, and
  the next is begun with the moves the client made meanwhile: at once, from a few places in
  the client's frame (`SE_ServerPoll`), not at the client's next frame.
- **The frame's messages are made on the worker too** (what the client is told: the entities
  it can see, 40 thousand instructions). They are kept there, and the first core puts them
  into the connection when it takes the frame, because the client reads its end meanwhile.
- **The server's frame has its own length** (`sv_frametime`: the time since its last one began,
  0.1 s of the game at most, as a frame of the game always was). Where that limit holds, the
  game's time goes slower than the clock, as it did before, and the client's time goes at
  that rate between the server's frames: at the slower of the last two frames' rates and a
  tenth slower still. At the rate of the last frame alone the client was at the newer
  frame's time early whenever the next took longer, and in one frame of eight nothing it
  drew moved, the player's own view included, which in play looked like being stuck for a
  moment. Now no frame stands still (the quakestat: line counts them); the client is a
  little behind when the next frame comes instead, and is taken up to it.
- **The server keeps to its own data.** Console lines, commands and cvar changes that QuakeC
  makes on the worker are kept and done by the first core afterwards; an error there ends
  the job and is raised on the first core; the server's random numbers are its own (the C
  library's have one state, which the client uses too). A console command that may be the
  server's (anything but the keys' and the like) waits for the server's frame to end.
  Loading, leaving and demos are on the first core, as they were.
- **Every variable has its own 16 bytes** (`-fno-common -fdata-sections`, and the sections
  aligned by `objcopy` in `build.sh`): two cores must not store to the same 16 bytes in one
  pass of the machine, and a variable of the server's beside one of the client's would be
  that.

On e1m5, standing where the level starts, in real time (RTX 5090, harness, D3D11):

| | Frames a second | Server frames a second | The first core's frame |
|---|---|---|---|
| server on the first core (`QUAKE_SERVER=inline`, or no worker cores) | 4.85 | 4.85 | 515 thousand instructions |
| server on a worker, the client waiting for it (`QUAKE_SERVER=wait`) | 6.3 | 6.3 | 211 thousand, and 9 passes of waiting |
| server on a worker, each at its own pace (the default) | 13.3 | 5.9 | 153 thousand |
| the same after the client's frame was gone through (water, sky, leaves, models' light: 117 thousand) and the machine's float loads and stores were put on its fast step | 22.2 | 7.5 | 115 thousand |

Of 54 frames drawn, 47 are between two of the server's. The server's frame is the 330
thousand instructions it was and decides how fast the game's time goes (0.1 s a frame at
most: 0.59 of the clock's rate here, 0.49 with the server on the first core). A pass of the
machine takes 6.5 ms while Quake runs, against 5 for integer code: floats, loads and stores. A pass of the machine with a busy
worker costs about 5% more than without, which those figures include.

Measured again on 13 October 2026 (the same card, one run each of 15 to 20 s standing where
the level starts, the build of the day before and that day's, both on that day's machine):
on e1m5 the server makes 15.9 and 16.8 frames a second (224 and 217 thousand instructions
each; a frame is 58 ms, 28 passes, the 90th of a hundred 32 and the longest 39; a pass of it
runs 8,045 instructions of 8,192) while the client draws 50.6 and 94.2; on e1m1 49.3 and 54.2
(62 and 60 thousand; 16 ms, 8 passes, the longest 18) to the client's 87.9 and 93.7. Above
ten a second the game's time is the clock's, and a frame is shorter than a monster's tenth of
a second, so fewer think in it. The twelve other workers took nothing from the server's: with
`QUAKE_WORKERS=1` it makes 16.0 on e1m5. Its worker is what limits it: 3.6 million
instructions a second of the 4.1 a core has at 2.0 ms a pass.

    QUAKE_SERVER=inline   the server on the first core
    QUAKE_SERVER=late     on the first core, its messages sent at the end of the client's frame
    QUAKE_SERVER=wait     on a worker, the client's frame waiting for it at its end
    QUAKE_SUM=N           a sum over every entity's fields each N frames of the server
                          (but the world's model and the players' names, whose places depend on the heap)
    QUAKE_NET=1           what a packet costs, every 200 sent (the game has its UDP driver: docs/lan.md)
    QUAKE_TRACK=N         where the player is and how fast, each N frames of the server
    QUAKE_TEST_EXIT=F     (with QUAKE_TRACK) at frame F the player is put into the level's exit

`late` and `wait` do the same things in the same order, one on a core and one on two. With
`+host_framerate 0.05` (a frame's length is then not the machine's speed) and `--fixed-dt
0.004`, `QUAKE_SUM=10` must print the same sums with `inline`, `late` and `wait`: that is the
check that the server on a worker computes what it computed on the first core. The default
cannot be checked so (how many frames the client draws to one of the server's is the
machine's speed); it runs the same job.

Play is checked with keys typed as console commands: `QUAKE_TRACK=5` and `+forward` after
"track 30:" must take the player the same way in every mode, and `QUAKE_TRACK=5
QUAKE_TEST_EXIT=40 quake +map e1m1` with `+attack` after "track 80:" must end the level, load
the next and go on printing where the player is. It did not at first: with the server's
messages sent late one was still unread when the level changed, the connection takes one
reliable message at a time, the "reconnect" could not be sent, and the client read the new
level in the old one's state (`i >= cl.maxclients`). The client now reads what it was sent
before the server is replaced (`SE_ServerDrop`). And a button counts as down for a frame of
the server's if any of the client's moves since the last had it down: the server took the
last move only, and a tap between two of its frames (a jump, a shot) was lost.

## The other workers, and draws made again (13 October 2026)

The first core's frame was gone through once more, this time for what workers could have
of it a frame behind and for what a frame draws that it drew before. The program asks for
thirteen workers (`SE_Workers` in `server_shaderemu.c`; `QUAKE_WORKERS=N`): the server's, and
four each for the light maps, the water and the particles.

| `rvc_cpu`, passes of 8,192, 3 million instructions a second | A frame before | now | Frames a second before | now |
|---|---|---|---|---|
| e1m5, where the level starts | 75.0k | 32.6k | 36.5 | 81 |
| e1m1, where the level starts | 44.4k | 20.4k | 61.7 | 86.6 |
| `timedemo demo1`, 969 frames | 41.6 s | 24.8 s | 23.3 | 39.0 |

A frame under four passes waits for the fourth (`Host_FilterTime`'s 10 ms is 3.4 passes): 84 to
87 frames a second is as fast as the game draws at this speed, and both levels are there.
On the shader machine (RTX 5090, D3D11, before and after in turn in one turn of the card): e1m5
42.5 to 89.8 frames a second (213 and 450 frames in 5 s, twice each; 77.8 to 35.6 thousand
instructions a frame), `timedemo demo1` 36.4 to 22.5 s (26.6 to 43.0 frames a second, once
each), e1m1 76 to between 77 and 91 (once each; 44.6 to 22.0 thousand a frame: its 5 s counts
fell from 455 to 384 during the run, which was not looked into). From the command to a
level's first frame is 1.5 to 2 million instructions more (2%): the kept brush models.

- **Light maps that flicker are the workers'** (`light_share` in `world_shaderemu.c`). When a
  style's brightness changes, ten times a second, four workers make again every light map of
  the level that has that style, into the light map's own texels in GPU memory. A page of
  light maps has one worker (two surfaces side by side share 16 bytes). Before, the first core
  made those in view again, in the frame (`R_BuildLightMap`): 4 thousand instructions a frame
  on e1m1, up to 30 in demo1. A brush model's are the workers' too.
- **Water is kept in GPU memory as the level is**, a run a texture, and its ripple is written
  there by workers: the texture coordinates of the water the viewer's leaf can see, as often as
  they get round (`water_share`; one worker to 160 quads). It was 10 thousand a frame on e1m5.
- **With both, and the sky kept, a frame asks nothing of a leaf**: no surface goes on a chain,
  and the torches and the like that stand in the leaves in sight are a list made when the
  viewer changes leaf, each tested against the view by its own box (`static_add`). The loop
  over the leaves in sight was 5 to 9 thousand a frame.
- **Brush models are kept too** (`brush_build`): a door's or a box's triangles lie in GPU
  memory, sorted by texture and again by light map, and a frame is its matrix and a draw a
  run. All of its faces are drawn, not those that face the viewer: the others are behind them.
  A brush model was 5 thousand instructions, and is 2.
- **A draw made again is its command copied** (`gpu_shaderemu.c`, which is `gles.c` and three
  calls after it; `qglModel`, `qglTrianglesAt`). A model's pose drawn with the skin and the
  pose where they were in GPU memory is the sixteen words it was drawn with before, with the
  frame's matrices and the light's table. An entity's matrix is written as rows where the
  draws read it (`qgpuRows`), and nothing is pushed or popped: the next entity's replaces it,
  and the world's is put back when something else is drawn (`qglWorldAgain`).
- **The status bar is the frame's first draws when it can be** (`SE_SbarHead` in
  `gl_screen.c`, `qglHead`): the bar of a game being played, the same for three frames, with
  no tint over the picture. A frame is built in one of two sets of memory in turn; when this
  set's last frame began with the same bar, the commands, vertices and matrices are still
  there and are counted, not written. It was 8 thousand instructions a frame.
- **Particles are four workers'** (`se_particle_share` in `r_part.c`): each has a quarter of
  the array, moves its particles and writes a vertex each into a buffer of its own; the first
  core gives a new particle a free place, and copies the vertices the workers last made into
  the frame, 16 bytes a particle where moving one was 70 instructions and two or three texels
  of the write cache. A particle is seen a frame after it is made.
- Smaller: the view's matrix in one go (`qglView`: the same sums as the five calls), the
  projections kept while the view's size is the same, `R_AnimateLight` when the tenth of a
  second or a style changes, and the terminal asked for a typed line four times a second and
  not every pass (a `select` was 800 instructions).

    QUAKE_LIGHT=frame     light maps made by this core, in view, as before (and brush models a polygon at a time)
    QUAKE_WATER=frame     water drawn into the frame by this core
    QUAKE_PARTICLES=frame particles moved and drawn by this core
    QUAKE_LIGHT=wait, QUAKE_WATER=wait, QUAKE_PARTICLES=wait   the workers, waited for in the frame
    QUAKE_MARK=leaves     the loop over the leaves in sight
    QUAKE_BRUSH=frame     brush models a polygon at a time
    QUAKE_SBAR=frame      the status bar drawn where it was, every frame
    QUAKE_WORKERS=N       so many workers in all (1: the server's only; 0: none)

With `wait` a frame's picture is what the first core's own work made of it; without, a light
map is some passes behind its style, the water's ripple a frame behind the clock and a
particle a frame behind its place.

Checked: the server's sums of e1m5 are the ones from before with `inline`, `late` and `wait`;
`+forward` stops where it did and the exit test loads the next level; with every switch above
set to the old way the game runs as it did. Held frames compared with the build before, pixel
by pixel (`+host_framerate 0.05`; under `rvc_harness --cpu`, and on the shader machine for
frame 60 of e1m5 and frames 150 and 300 of demo1): frames 150, 300 and 420 of demo1 and frame
60 of e1m5 are the same picture, with `wait` and without; frame 60 of e1m1 differs in one
pixel by 8 levels with `wait` (not found out which draw; not the brush models), and without
in 2% of its pixels, where a flickering light is a tenth of a second behind.

Tried and not kept: nothing of this round. Not attempted: the light at a thing that moves
(`RecursiveLightPoint`, 6% of e1m5's frame and of demo1's: a shorter ray is not the same
bits, and a worker's answer comes a pass too late for a frame of four); the status bar in
pieces, for a fight in which it changes every few frames; a demo's messages read ahead.

## Calls of the second layer, and the frame's jobs (14 October 2026)

Every job of the program is now a call of the workers' second layer (`programs/mc/mcwx.h`,
`docs/multicore.md`; `build.sh` compiles `mcwx.c` beside `mcw.c`): `mcw_call` gives it to a worker
that has nothing to do, `mcw_ready` asks after it and `mcw_join` takes it. No worker is the
server's or the particles' any more: the server's frame, a loader's job, the light maps and the
frame's jobs below each go to whichever is idle (`SE_GroupAdd` and the like in
`server_shaderemu.c`). The program still asks for thirteen, and each part is this core's with
fewer, as before. A call costs this core some 600 to 700 instructions from `mcw_call` to
`mcw_join` (a worker is looked at when it is chosen, when it is asked after and when it is
joined), where a post and a look at the mailbox were about 150; the first port, a call a piece,
was 2,800 instructions a frame dearer than the tree. So the pieces of a frame are as few calls
as they can be:

- **The frame's jobs** (`SE_FrameJobs` in `world_shaderemu.c`, after `CL_RelinkEntities`) are one
  call: the light under the models that moved, the water's ripple and the particles. A fight's
  particles (more than 512 a call) and much water take more calls. Water is rippled for the
  leaves the viewer saw the frame before; particles a call moved before the frame draws them are
  drawn in that frame (they were a frame behind).
- **The light under a model that moved** is walked down the level by the worker
  (`SE_LightAheadPrepare`, `SE_LightAheadJob` in `gl_rlight.c`) while this core goes on to the
  picture, some 11 to 13 thousand instructions. The walk is all that is kept; the sample's light
  is worked out again when the model is drawn, by the styles as they are then, so the light is
  the bits it was. A model out of view the frame before is left to this core, and an answer not
  there yet in time is worked out here as before (the worker says it has made them by a word, a
  pass later). The weapon is placed later (`V_CalcRefdef`) and drawn after the rest of the view:
  its walk is a call of its own (`SE_GunAhead`). The cache of points already walked has 256
  places (64: torches and moving models put each other out). In demo1's windows of five
  seconds 240 to 570 walks came from a worker, up to 22 were late and 40 to 200 were this core's. `QUAKE_LIGHTPOINT=check` makes every walk a worker made here too and counts those not
  the same (the `quakestat:` line about the light): 0 in demo1, with `+host_framerate 0.05`
  and without, and on e1m5 with `RVC_RACES`.
- **An explosion's 1,024 particles are made by their shares' workers** (`se_explode` in
  `r_part.c`, and `R_RunParticleEffect` of 1,024). This core gives each a free place as before
  and writes its number in the explosion beside it; the worker makes it from the explosion's
  place and the seed `rand()` had, stepped on by seven numbers a particle at once (a step taken
  2^b times is a multiply and an add: `se_leap`), and this core's `rand()` goes on as if the
  7,168 numbers had been drawn. When each share has a quarter free, the places are written a
  share at a time. An explosion was 170 thousand instructions of this core; a particle is still
  seen a frame after it is made.
- **The particles' vertices are in GPU memory** (`qglParticleMemory`, 64 KB taken from the pool,
  `qgpuPointsAt` in `gpu_shaderemu.c`), and each share is a draw from where its worker wrote them:
  they were copied into the frame, 16 bytes a particle. A share's buffer is written again only when
  the list that may still read it has been drawn (`seglSettle` when the frame before drew it).
- **The light maps are one call** a change of the styles (four calls before, each going through
  every surface for its pages).
- **The server's end is a word.** Its frame writes its number when it has ended, and
  `SE_ServerPoll`, a few times a frame, asks the library only when the word says so, or once a
  frame (a page the worker stopped at is served then).
- Smaller, on this core: the message's readers inline in `cl_parse.c` and `cl_tent.c`
  (`SE_MSG_INLINE` in `common.h`, the same arithmetic), a notify line drawn up to its trailing
  spaces (`se_line_length` in `console.c`).

Calls a frame of demo1 went from 4.8 to 2.5. Nothing a worker does is waiting: they have calls
only, never a team that lingers, and a worker without a call sleeps (with every part on this
core the thirteen ran no instruction in the window).

| `python tools/game_par.py quake --runs 3` (`rvc_cpu`, demo1's 969 frames) | this core a frame | all cores | factor |
|---|---|---|---|
| before (the tree of 13 October) | 68.8k | 99.9k | 1.45 |
| every job a call, a call a piece | 71.6k | 103.1k | 1.44 |
| now | 61.6k | 94.3k | 1.53 |
| now, with every part on this core (`QUAKE_WATER`, `_LIGHT`, `_PARTICLES`, `_LIGHTPOINT=frame`) | 105.2k | 105.2k | 1.00 |

What each new part is worth, by its switch: an explosion on this core 2.8k more of this core's
a frame (all cores 0.5k), the light walks here 2.35k more (all cores 1.1k fewer: a worker also
walks for models that leave the view), the particles copied 0.9k more. Of the workers' 32.7k a
frame, the particles are 12.6k, the water 11.0k, the light walks 3.5k and the light maps 3.4k.

On e1m5 and e1m1 where they start (`rvc_cpu`, the `quakestat:` lines, the frame at its 10 ms
floor in both builds): 31.5k to 30.7k and 20.2k to 20.3k instructions a frame of this core, the
server's frames as many.

    QUAKE_EXPLODE=frame        an explosion's particles made by this core, as before
    QUAKE_PARTICLES=copy       the workers' particles copied into the frame, as before
    QUAKE_PARTICLE_SUM=N       a sum of the particles drawn every N frames, and rand()'s seed
    QUAKE_LIGHTPOINT=frame     every walk for a model's light made by this core, as before
    QUAKE_LIGHTPOINT=check     and made here too when a worker made it, and compared

Checked, on `rvc_cpu`: `QUAKE_PARTICLES=wait QUAKE_PARTICLE_SUM=1 +host_framerate 0.05` prints the
same sum at each of the 969 frames with explosions made by workers and by this core (and as the
first build of the day, before the frame's jobs); without `+host_framerate` a sum is the machine's
timing, which a word more in the command line changes. The server's sums of e1m5 (`QUAKE_SUM=10`,
`+host_framerate 0.05`) are the tree's with `inline`, `late` and `wait`; `+forward` after "track
30:" stops at 480 687 24 in every mode in both builds; the exit test loads e1m2 and goes on.
`RVC_RACES` on demo1 and on e1m5 (the server on a worker): nothing undone and nothing read as it
was. `QUAKE_WORKERS=0`, 1 and 3 run the demo to its end. Pictures: in the next section. Not
checked: the shader machine and the world.

Tried and not kept: water in one call up to 480 quads (demo1's water is one call already). The
first core's frame was still the client's and its drawing, all into one GPU list: the next
section gives the drawing to a worker whole.

## The picture on a worker core (14 October 2026)

A frame's drawing is now a worker's, a frame late (`scene_shaderemu.c`, `SE_SceneFrame` in place
of `SCR_UpdateScreen` in `_Host_Frame`): everything that writes the GPU's list and swaps it, the
level, the models, the particles, the 2D screen (`SE_ScreenWorker` in `gl_screen.c`). The first
core gives frame N out with `mcw_call` and goes on to frame N + 1: the input, the server's part,
the demo's or the server's messages, the entities placed. It joins frame N's call when frame N +
1 is to be given out, so there is one writer of the list at a time, and the picture is a frame
later than it was.

- **What the worker reads that the first core goes on changing, it reads from a copy** made when
  the frame is given out: the client's state (`cl`, but its lists of models and sounds, which only
  a new level changes), the entities drawn (from their place on: not the messages' and baselines'
  fields, about 100 bytes of each), the dynamic lights, the frame's count, length and time, and the
  view's blend. The files that draw see those through `include/se_scene.h`: `cl`, `cl_entities`,
  `cl_visedicts`, `cl_dlights`, `host_framecount`, `host_frametime`, `realtime_ms` and `v_blend` are
  pointers there, at the copy while a worker draws and at the client's own otherwise.
- **What only the first core changes, it changes between the two frames**, when the worker has
  none: the view (`V_CalcRefdef`, `SE_ViewFront` in `view.c`), what `R_SetupFrame` made of the
  client's state (the light styles, the view's leaf, its contents' colour and blend:
  `SE_SceneFront` in `gl_rmain.c`, the same sums in the same order), the light maps' call
  (`SE_LightFrame`), the frame's jobs (water, particles, the light under models that moved and the
  weapon, whose place is known then), and the palette's decay (`V_UpdatePalette`). The light maps'
  call and the palette are made after the frame is given out: the frame does not read them, and
  the time between two frames is the frame's.
- **Anything else of the first core's that changes what the frame draws waits for it first**
  (`SE_SceneOwn`): a line printed or centred, a key, a command, a setting, a skin made for a player,
  a window resized, a level loaded, cleared or left, an error, and every message of the server's but
  a frame's everyday ones (`cl_parse.c`). A line the worker prints is written when it is joined
  (`mcw_print`); an error there ends the program (`mcw_fail`). The server's own lines and errors
  go where they went: `SE_ServerOnWorker` is now true on the server's worker only.
- **A frame is drawn here**, as before, in a menu, the console, a level's first frame, an
  intermission, a pause, a holodeck's seat, with mirrors or `lcd_x`, a held frame drawn late, a
  setting of the view's size changed, and with `QUAKE_SCENE=inline` or without workers.
- The first core waits for the frame by the word its call writes last, and asks the library only
  every fourth pass: `mcw_join`'s wait looks at every worker each pass, which was 2 to 3 thousand
  instructions a frame.

`python tools/game_par.py quake --runs 3` (demo1's 969 frames, `rvc_cpu`):

| | the first core a frame | all cores | factor | guest seconds for the demo |
|---|---|---|---|---|
| the tree of 13 October | 68.8k | 99.9k | 1.45 | 20.1 |
| the frame's jobs (above), `QUAKE_SCENE=inline` | 62.0k | 94.7k | 1.53 | 18.1 |
| the picture on a worker | 28.4k | 98.7k | 3.47 | 13.9 |

The demo is 31% shorter: 70 frames a second of the machine's time where it was 48. The
frame is as long as the worker's drawing and a pass or so to give it out and take it back: the
first core waits for it in nearly every frame (`quakestat: N of these frames drawn by a worker
core, K thousand instructions each, W waited for`: 34 to 46 thousand). The first core's 28.4k are
its own frame and the waiting. All cores run 3.6k more than with the picture here: the copy, the
waits, and light walked for models the worker does not draw. On e1m5 and e1m1 at their start the
first core's frame went from 30.7k to 16.4k and from 20.3k to 13.9k; both draw at the game's
floor of 10 ms a frame, as before.

    QUAKE_SCENE=inline     every frame drawn by the first core, as before

Checked on `rvc_cpu`, as for the section above: the particles' sums at all 969 frames of demo1 the
same as the first build's (`QUAKE_PARTICLES=wait`, `+host_framerate 0.05`); the server's sums of
e1m5 the tree's with `inline`, `late` and `wait`; `+forward` stops at 480 687 24 in every mode;
the exit test loads e1m2; `QUAKE_LIGHTPOINT=check` 0 not the same; `RVC_RACES` on demo1 and
e1m5 nothing undone, and reads as they were only of the walks' word, which the drawing worker
looks at (and of the client's time by `R_DrawParticles`, before it read it later).

Pictures, by `rvc_harness --cpu --warp` (the software rasteriser, no card): frames 150 (a fight),
300 and 420 of demo1 held (`QUAKE_HOLD`, `+host_framerate 0.05`), the tree's build against this
one, the GPU's whole target compared pixel by pixel. With the particles, light maps and water
waited for, the picture on a worker is the tree's at 150 and 420, and with `QUAKE_SCENE=inline` at
all three; at 300 1,912 pixels differ, all in the console's notify lines at the top, which go after
three seconds of the clock, and this build is faster: with `+con_notifytime 0` none differs. With
everything at its own pace (the default), the three are the tree's too. The particles there are
drawn from GPU memory in this build and copied into the frame in the tree's.

## Drawn without a gap, the 2D screen on a worker of its own (15 October 2026)

With the picture on a worker the frame was as long as three things one after another: the first
core's part between two frames (the view, the copy, the frame's jobs: 10 thousand instructions),
the pass or so it took to give the frame out and take it back, and the drawing (40 to 45
thousand). Now the drawing is the frame:

- **The drawing worker stays** (`scene_job`): one call that draws frame after frame, each from one
  of two copies in turn, and goes on to the next as soon as it has swapped one if the first core
  has given it. The first core fills a copy while the worker draws from the other, waiting only
  until the frame two before has been swapped (that copy's last reader), and says "no more"
  before anything that changes what drawing reads (`SE_SceneOwn`, as before). A frame is so shown
  up to two frames of the first core's after it was made. Between frames a worker with nothing to
  draw ends its passes (`mc_end_pass`), which costs it a few instructions a pass.
- **The view is in the copy too**: the refdef, the view's place and axes, its leaf and the light
  styles' values (`se_scene.h`), with which the first core makes the next frame's view while the
  worker draws the last. Code of the first core's in the files that draw reads its own state again
  between `se_real.h` and `se_scene.h` (`SE_SceneFront`, `R_AnimateLight`, the walks, the frame's
  jobs, the light maps). The particles' vertices are in four buffers: one being written, one
  shown, and two that a frame given out or a list being drawn may still read; a job writes into
  one of none of those, and no frame waits for the GPU for it. The walks' tables are two, a copy's
  each, and one is taken for walks only when none is out.
- **A worker waiting for the GPU ends its pass** (`rest` in `programs/linux/gles.c`): its pause was
  a sleep until its job changed, which a call that draws frame after frame never has. And the sky's
  place on the picture, set every frame, is written in the set being built only
  (`seglKeptLaidHere`, new in `gles.c`): written in both, it waited for the list being drawn every
  frame, 0.7 of a pass, the drawing worker's only wait.
- **The 2D screen is recorded by a third worker** (`record_screen`, `SE_ScreenRecord` in
  `gl_screen.c`) from the same copy, while the drawing worker draws the view: the status bar, the
  notify lines, the centre print, the crosshair, the icons, recorded as rectangles into GPU memory
  (one of three buffers) and runs of one texture and tint (`qglRecordBegin` .. `qglRecordEnd` in
  `qgl.c`: on the core that records, the texture, the colour and the rectangles go into the
  recording, anything else spoils it). The drawing worker draws the runs after the view, a command
  each pointing at the rectangles where they lie (`qglRecordDraw`, `seglSpritesAt` new in `gles.c`),
  waiting for the recording if it is not done (it always was). `gl_screen.c` and `sbar.c` read the
  client's state through `se_scene2d.h`, the copy of the frame being recorded. A frame with the
  console down, the scores, a menu or a smaller view is drawn by the first core, as before;
  `quakestat:` counts frames whose screen could not be recorded (0).

`python tools/game_par.py quake --runs 3` (demo1's 969 frames):

| | the first core a frame | all cores | factor | guest seconds for the demo | frames a second |
|---|---|---|---|---|---|
| the tree of 13 October | 68.8k | 99.9k | 1.45 | 20.1 | 48 |
| the picture on a worker (above) | 28.4k | 98.7k | 3.47 | 13.9 | 70 |
| drawn without a gap | 28.2k | 100.0k | 3.54 | 12.3 | 79 |
| and the sky laid in one set | 28.2k | 99.0k | 3.51 | 12.1 | 80 |
| and the 2D screen on a worker of its own | 29.3k | 102.2k | 3.49 | 10.5 | 92 |
| the same, `QUAKE_SCENE=inline` | 62.7k | 95.4k | 1.52 | 18.3 | 53 |

The drawing worker's frame is 31 to 37 thousand instructions, and is the frame: the first core
waits for it now and then (`quakestat:`, "waited for"). All cores run 2% more than the tree: the
copies, the walks made for models that are not drawn, and the 2D screen's runs. On e1m5 and e1m1 at
their start the first core's frame is 16.2k and 13.9k; both draw at the game's floor of 10 ms.

Checked on `rvc_cpu` as before: the particles' sums at all 969 frames as the tree's
(`QUAKE_PARTICLES=wait`, which now waits for every one of the frame's jobs, the walks too: with the
walks out the particles were given a frame later, by the walks' timing); the server's sums of e1m5
with `inline`, `late` and `wait`; `+forward` to 480 687 24; the exit test; `QUAKE_LIGHTPOINT=check`
0; `RVC_RACES` on demo1 and e1m5 nothing undone. Reads as they were: the drawing worker's look at
the words it is given (`published`, `stop`) and at the 2D worker's word, the walks' word, the
water's list as the frame's jobs left it (a frame's ripple may be of the list before), and every
`qgl` call's look at whether it records (`rec_on`, which the recording core writes; a call on
another core goes on as it would).

Pictures, as for the section above (`rvc_harness --cpu --warp`, frames 150, 300 and 420 of demo1
held, `+host_framerate 0.05`, against the tree's build): with the particles, light maps and water
waited for and with everything at its own pace, all six are the tree's to the pixel, the 2D screen
recorded and the frames drawn by the staying worker; frame 300 with `+con_notifytime 0` too.

## The server's frame, and what could be taken from it

Not done: what was measured on 13 October 2026, and what it suggests. e1m5 standing where
the level starts, `QUAKE_SERVER=inline +host_framerate 0.1`, 341 frames on `rvc_cpu`,
sampled every 256 instructions with the call chain of each sample (a private `rvc_cpu` that
wrote the stack's return addresses beside `--pc-log`, unwound by each function's frame;
99.9% of samples reached `main`; it is not in the tree). A frame is 405 thousand
instructions, the server 311 of them. In thousands a frame:

| The server by who asked | | Of the server |
|---|---|---|
| the monsters' `movetogoal` (a builtin: C) | 127 | 41% |
| the interpreter itself (`PR_ExecuteProgram`, entering and leaving a function) | 68 | 22% |
| other builtins: `checkclient` 15, `traceline` 12, `cvar` 5 | 35 | 11% |
| not under QuakeC | 81 | 26% |

Not under QuakeC: the messages 31 (16 of them `SV_SetIdealPitch`'s traces), doors and
platforms 19 (15 in `SV_PushMove`'s own loop over every entity), the player 13, a monster's
water check after its think 12 (`SV_CheckWaterTransition`). So 74% of the server is under
QuakeC and 22% is interpreting it.

| The server by where the instructions are | | Of the server |
|---|---|---|
| the interpreter | 70 | 22.5% |
| the level's clip tree walked (`SV_RecursiveHullCheck`) | 58 | 18.7% |
| point tests (`SV_PointContents`) | 40 | 13.0% |
| links (`SV_LinkEdict`, `SV_FindTouchedLeafs`, `SV_TouchLinks`) | 29 | 9.4% |
| a move clipped against the entities (`SV_Move`, `SV_ClipToLinks`, `SV_ClipMoveToEntity`) | 27 | 8.7% |
| `SV_PushMove` | 15 | 4.7% |

A monster's step is `SV_StepDirection` (125 with what it calls), `SV_movestep` in it 90,
and the floor looked for under the step 52 (`SV_CheckBottom`: 30 of the 40 of point tests;
the water check is the other 10).

What could be taken, none of it tried:

- **On one core, about 73 (23%).** The ideal pitch's traces every frame (16: the view's
  tilt on a slope), the pushers' loop (15), the water check of a monster that did not move
  (12), the floor check's point tests (30). Whether each can be left out or kept from one
  frame to the next with the same sums is not looked at.
- **The level's answers ahead, on workers: up to 62 (20%).** The clip tree walk and the
  point tests (98, 32%) read what does not change in a frame and are the same in any order.
  What has an order is what is asked and what is done with the answer: a step is tried,
  traced, its floor checked, and another direction only then; a later monster is clipped
  against where an earlier one went; `movetogoal` and the search for a direction draw from
  the one sequence of random numbers; QuakeC's globals are one set. So the frame stays as it
  is on its core, and workers answer the questions a monster will probably ask into a table
  kept by the question itself (start, end, box): an answer found is the same bits, one not
  found is worked out as now. It is what Tiberian Dawn's path searches do (`docs/tdawn.md`).
  The 20% supposes two questions in three guessed: a step's direction is the monster's state
  and a random draw, its length the animation frame's QuakeC. Measure that first: write each
  step's start, direction and length for some hundred frames and count what a guess from the
  frame's beginning would have hit.
- **QuakeC as machine code: about 47 (15%).** The interpreter is 22%, and what its
  statements did is still to be done. The most work for the least certain gain.
- **Not: thinks side by side on a copy of the frame's beginning**, put in order afterwards.
  It would gain more and give other results: random numbers a monster, two monsters wanting
  one place. The sums would not be the one core's.

The first three touch different instructions, the floor check apart, which the first two
share. The second and third together are about 35%: a frame of the server's of 200 thousand
where it is 311. The server makes 7.5 frames a second on its worker and the game's time
goes at 0.59 of the clock's, a frame being 0.1 s of the game at most; at 200 thousand it
would make about 11.5 and the game go at the clock's rate. Above ten a second a monster
no longer thinks in every frame, which is less again.

## Checking it

- The GPU against its model: hold a frame, pass `--save-state` and `--gpu-capture`, and run
  `python tools\gpu_reference.py SNAP BMP --size 640x480` (the picture is the window's size,
  not the display's). On the start map 99.996% of pixels away from triangle edges agree. In a
  demo's fight it is 96.6%: the model drops triangles that cross the near plane and the card
  clips them. Of the pixels the models and particles draw there, none and 9 of 2,022 differ.
- A change to how it draws: hold the same frame before and after and compare the window's
  pixels, which are the display's second layer in the snapshot. `+host_framerate 0.05` makes
  a level's frame N the same game time each run; `timedemo` does for a demo.
- To see where instructions go, snapshot a run at a `quakestat:` line and resume it with
  `--pc-log` (`tools\pc_profile.py`, `quake=quake.nm`).
- `linux/quake/quake.patch` is `git diff -- WinQuake` of the tree at `build.sh`'s commit.

## Not done

- Music, and hearing the sound by ear: it was checked against the card's model only.
- Playing it: keys and the pointer by hand, in the harness's window and in the VRChat world.
- The server: a busy level is 350k instructions a frame before anything is drawn ("The
  server's frame, and what could be taken from it").
- Loading: the light maps' placement is a search of every block for every surface.
