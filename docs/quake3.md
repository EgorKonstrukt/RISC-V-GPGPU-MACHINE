# Quake III Arena

One level of Quake III Arena's free demo (q3dm17, The Longest Yard) runs in the Linux image
with a bot: ioquake3's source with the game, the client's game and the menus linked in, and a
renderer and a sound layer of our own. Everything on the screen is drawn by the machine's
GPU, and most of what it draws it reads where a file lies in the ROM.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/quake3/build.sh    (17 s; the first time it also fetches)
    python tools\make_linux_image.py

In the guest: `nano-X -p & quake3 +map q3dm17 +addbot sarge 3`, or Quake III Arena in the
Start menu's Games, which opens the main menu. Lines typed at the terminal it was started
from are console commands. The demo's 47 MB pak is fetched by the build and not kept; what
goes into the image is 15.8 MB.

The main menu is cut down to what works here (`q3_ui/ui_menu.c` in the patch): the level
against Sarge at three skills (I CAN WIN, HURT ME PLENTY, HARDCORE: `addbot sarge` 1, 3, 4)
and EXIT, which asks nothing. Its icon is ioquake3's (`linux/apps/icons/quake3.png`).
A menu's cursor is the desktop's pointer: `SE_Input` tells the menus where it is in the
window, not how far it went (`UI_MouseEvent` in the patch), and their own cursor is not drawn.

The game's picture keeps the size the window had when it started and is stretched to the
window as it is now (`picture_viewport` in `tr_shaderemu.c`); the pointer's place in a menu
and the sky's repeats go by the same two ratios. Before, a resized window showed the first
size's picture in its corner.

Not tried by hand: the keys, the pointer, the menus, the sound by ear. On 13 October 2026
it was run by typed commands only, and its pictures were compared with the GPU's model.

## What the build is made of (`linux/quake3`)

- `build.sh` fetches ioquake3 at a fixed commit, applies `quake3.patch`, and compiles the
  engine, the bots' library and the three modules with Quake's flags (`linux/quake/build.sh`).
  The modules are not libraries: each is linked to one object whose names stay inside it, and
  its variables are put back as they began each time it is "loaded" (`sys_shaderemu.c`).
- `bake.py` makes the two files the game runs from (below); `jpgraw.c` reads the demo's JPEG
  pictures for it; `wanted.txt` is what the game asked for in a run with `QUAKE3_WANTED=1`.
- `tr_shaderemu.c` is the renderer, `gpu_shaderemu.c` the GPU library (`programs/linux/gles.c`
  as it is, and draws whose every word the renderer says), `snd_shaderemu.c` the sound,
  `server_shaderemu.c` the server's frame on a worker core, `client_shaderemu.c` the client's
  game on another, `sys_shaderemu.c` the system.
- `quake3.patch` is `git diff -- code` of the tree at `build.sh`'s commit.

## The two files

`bake.py` reads the demo's pak and writes only what `wanted.txt` names:

| | |
|---|---|
| `quake3-pak0.pk3` | 2.5 MB, stored and not compressed: what the game reads itself (the level for collision without its light maps, the bots' navigation, their scripts) |
| `quake3-se.dat` | 13.3 MB: pictures 6.5 (0.2 of it the menu's banner), sounds 3.0 (halved to 11 kHz: `--sound-rate`), models 1.6, the level 1.4 (1 MB of it the light grid), light maps 0.4 |

- **Pictures** are decoded and halved to 128 texels a side (256 for the flat screen's) when
  baked: three bytes a texel, or words where a picture's alpha is used.
- **Shaders** are the scripts' stages reduced to what the GPU has: one of four passes, a
  colour that may be a wave, a texture that may move. A reflection (`tcGen environment`) is
  left out, and what is blended over one is laid on its mean colour.
- **The level** is triangles sorted by shader stage, three vertices each, patches cut into
  4 x 4 pieces; a light map and the texture it lights are one draw. A texture with no script
  is lit by its surface's light map too, as the game does (until 13 October 2026 it was not:
  2,631 of the level's 23,550 vertices were lit, the walls and floors not among them; 17,052
  now), and on a surface with no light map it has the surface's own colours.
- **A model's pose** is a word a vertex (three bytes of place and which of 256 ways it
  faces), every frame of it, and its texture coordinates a word a vertex.
- **One character** (Sarge, for the player and the bot) and one level: `--bots`, `--map`.

## How it is drawn

- **The level is kept in GPU memory** (442 KB, 23,550 vertices in 75 draws that stay in every
  frame's list). A frame gives them its matrices and says what changed: a wave's colour, a
  texture's turn. Nothing of the level is looked for or sorted in a frame; the card's depth
  buffer hides what is behind.
- **Textures, light maps and models stay in the ROM.** The GPU reads them where the file
  lies; nothing is loaded into RAM or GPU memory, at a level's start or after.
- **A model is lit by the GPU**: three texels a thing (where the light comes from, its
  colour, the light from everywhere), from the level's light grid, kept by where the thing is.
- **Sprites, beams and the game's polygons** are quads written into the frame; the flat
  screen is quads in the one pass that blends without depth, in the order they come.
- **A model is drawn in the nearer of its two frames**, and its tags are that frame's.

What the GPU was given for it (`docs/gpu.md`): word textures and packed vertices in the ROM,
smooth three-byte textures, a draw lit by a light map (`0x800`), a clamped texture
(`0x1000`), a texture moved by two rows of numbers (`0x10000`), vertices lit by how they face
(`0x20000`). `rvc_backend11.cpp` bound the state texture to every texture of the vertex
shader; it now leaves the ROM's there. A world built before this draws the game wrongly.

Not as Quake III draws: no dynamic lights, no marks on walls or shadows, no fog, portals or
flares; no reflections; `tcMod turb` and vertex deforms stand still; rail rings are missing;
no mipmaps; the weapon can poke into a wall; a 2D shader's first stage only; no music or videos.

## Speed

Instructions a frame of the first core, on `rvc_cpu`, the player standing with 25 models in view:

| | A frame |
|---|---|
| everything on one core, two bots | 1,250k to 1,500k |
| the server's frame on a worker core | 370k to 400k |
| the client's moves not made again each frame, hot calls direct, no shadow traces | 210k to 250k |
| a pad the player is not near not traced | 172k to 188k |
| light kept by place, a thing's matrix written as rows | 168k to 185k |

With one bot, at 3 million instructions a second (computed from `rvc_cpu --ips 3000000`, not
measured on the card): a frame is 146 to 169 thousand, 18 to 20 frames a second; a frame of
the game on the worker is about 215 thousand (the bot 90 to 120, the game 33 to 51, telling
the clients 70), ten a second, 72% of that core. On the card on 13 October 2026, an earlier
build with two bots: 13 to 16.5 frames a second, passes 99.9% used, the worker's 93%.

- **Two bots are more than the worker has**: 335 to 380 thousand a frame of the game. The
  game then runs at about three quarters of the clock's rate (a job is 200 ms of it at most).
- `sv_fps` is 10 and `bot_thinktime` 200 here. The bot falls off the level once in 39 s
  of the game so; at 20 and 100 once in 62 s. Both were counted with the player standing still.
- From the command to the first frame: 127 million instructions, where it was 1,800. The
  network coder's table is not built (messages go as their bytes), the bots' script reader
  copies a word and not a kilobyte, freed memory is not filled, a string is not padded, the
  pak is not summed, the bots' answers to chat are not read.

`q3stat:` lines every 200 frames (`QUAKE3_STATS_FRAMES=N`; 0: never) say where a frame went.

    QUAKE3_HOLD=N         keep frame N on the screen and stop there ("quake3: holding frame N")
    hold                  the console command: the frame after next stays
    QUAKE3_SERVER=inline  the server on the first core
    QUAKE3_SERVER=frame   on its worker, taken and given out only where the first core's frame calls it,
                          and every frame of the game told to the clients (as until 13 October 2026)
    QUAKE3_ROUTES=late    the bots' routes made when a bot first asks, not while the level loads
    QUAKE3_TRACES=all     the server asks the level every trace again
    QUAKE3_SERVER_LOG=1   a q3srv: line for every job of the server's worker ("The server's pace")
    QUAKE3_CLIENT=inline  the client's game on the first core, drawn as it is made
    QUAKE3_SUM=1          a sum over what each frame of the client's game asked to have drawn, with its time
    QUAKE3_SIZE=WxH       the window (640x480)
    QUAKE3_WANTED=1       a line for every file, model, picture and sound asked for; nothing is "missing"
    QUAKE3_DEFINES=-DSE_MEMPROF QUAKE3_LINK=-Wl,--wrap=memset,--wrap=memcpy   (to build.sh) who fills and copies how much
    QUAKE3_DEFINES=-DSE_PARTS            (to build.sh) a q3parts: line: the first core's frame by part (with QUAKE3_CLIENT=inline)
    QUAKE3_DEFINES=-DSE_CHECK_VERSIONS   (to build.sh) a state left where it is is compared with what would have been copied

## In a holodeck

The room draws the frame's list itself (`docs/holodeck.md`), so its pass must read what the
GPU's reads: `GpuVolumePass.cginc` has the ROM's four textures (the materials get them from
"ShaderEmu/Give the holodecks' world the ROM"), and `volume_vertex` in `src/gpu.h` moves a
texture and places a light map as `gpu_vertex_sized` does. Before, nothing of the game had
a texture or a shape there. The level is sent whole whatever the camera sees, up is z
(`seglWorldUp`), and the right hand aims and holds the weapon (`docs/holodeck.md`, "Played
from the seat"; `SE_SeatMove`, `SE_SeatView`).
On 13 October 2026 the three shaders compiled in the editor; nobody had sat in it yet.

## The server on a worker core

As Quake's (`docs/quake.md`), and easier in that the two are a server and a client already:
the packets a client sends wait for the server's next frame, and the ones the server sends go
when the first core takes the frame. A frame of the game is run at a time, so the bots look
before each. It is on the first core while a level loads or anybody is not yet in the game.

What two cores would both write there is two of, three with the client's game's core (`quake3.patch`): a command's words, the
marks of what a trace has tested and the box a thing is clipped as, the buffers `va()` and
`Info_ValueForKey` return, a zone for the server's allocations. Console lines, commands and
cvar changes made on the worker are kept and done by the first core; an error ends the job
and is raised there. Each module calls the engine by its own way in, not by which was called last.

Not looked at: `COM_Parse`'s one token buffer, and `rand()`'s one state.

## The client's game a frame ahead

A frame of the client's was 154 thousand instructions of the first core with nobody but the
player in the level: the game's part 95 (the server's word taken in 27, the player's move
made ahead 18, every thing placed 35, the status 15) and the picture's 41. The game's part
is now a second worker's (`client_shaderemu.c`, 13 October 2026). It writes what it would
have drawn into a list (`frame_list_t` in `tr_shaderemu.c`: entities and polygons as they
were added, scenes and flat pictures in their order), and the first core makes the GPU's
commands of the list one frame later, while the worker is at the next.

- **A frame is asked for before the last is taken.** The first core asks for frame n + 1 with
  the time, the latest move and the latest word of the server's as they are then, waits for
  frame n and draws it; the worker goes from n to n + 1 without being told. Two lists in
  turn: a frame's list was last drawn from two frames before.
- **What it reads of the first core's is as it was when the frame was asked for** (which move
  and which snapshot are the latest: `SE_ClientPinned`). **What it would write there is
  kept** and done when the frame is taken: sounds (`snd_shaderemu.c`), console lines,
  commands, cvars and commands to the server (`server_shaderemu.c`), its two words for the
  next move. An error ends the frame and is raised on the first core.
- **The first core's it still is** while a level loads, for the first four frames of a game,
  and in any frame that has a command of the server's to run (a print, a config string): the
  worker's frames are waited for and taken first, and what the first core calls of the
  client's game (a console command, a key) waits for them too (`SE_ClientSettle`).
- **Three cores have one each** of what two had: a command's words, a trace's marks and box
  (two cores' boxes no longer share 16 bytes), the buffers `va()` and `Info_ValueForKey`
  return. `SE_OnWorker()` says which core by where the stack is: 0, 1 the server's, 2 this.

And each side's part was gone through:

- **A state that is where it would be copied to is not copied.** An entity's state went from
  the packet to the client's ring, to the game's snapshot, to the entity's next state and to
  its current one: 208 bytes four times for every thing in every snapshot, and most things of
  a level never change. Each state kept has a number (`se_entity_versions` in `cl_parse.c`;
  one that came unchanged has the number of what it came from), and the last three copies are
  left out where the number is the one already there. Not players' or events', which the
  game changes in place. `-DSE_CHECK_VERSIONS` compares instead of trusting the number.
- An item out of view is not made ready and added (`SE_OutOfView` in `cg_ents.c`, the
  renderer's four sides and 128 units to spare); a thing that lies still is where it is
  without the two general calls; `AnglesToAxis` of no angles is the identity (an item's second
  model asked six sines and cosines for it); a trace through the level asked for again is
  answered as before (`SE_LevelTrace`: a player who stands makes the same ones every frame;
  one who moves gains nothing by it); the status bar's numbers are not made by `sprintf`.
- **The renderer**: a stage's state is made when the file is opened and a model's surfaces and
  stages are found once (`model_way`), a thing's matrix is written where the GPU reads it
  (`gpuModelviewBlock`), a field of view's tangent, sine and cosine are kept, the level's
  draws that change are a list, `getenv` is not asked every frame (it was, twice).

Instructions a frame on `rvc_cpu` (8,192 a pass, 3 million a second), `devmap q3dm17`, the
player put at 488 -968 378 looking north (`setviewpos`), 22 models in 38 draws:

| | The first core | The client's worker | Frames a second |
|---|---|---|---|
| before, the level alone | 152k to 155k | | 17.7 to 18.1 |
| after, `QUAKE3_CLIENT=inline` | 100k to 103k | | 26.7 to 27.3 |
| after | 51k | 45k | 52.3 to 52.6 |
| before, with the bot (`addbot sarge 3`) | 163k to 174k | | 15.8 to 16.9 |
| after, with the bot | 53k to 58k | 50k to 54k | 43 to 46 |

On the shader machine (an RTX 5090, D3D11, through the card queue, before and after in turn,
frames 400 to 700 of the same scene). The level alone: 100 frames in 4.33 to 4.36 s before,
23 frames a second; 2.02 to 2.12 s after, 47 to 50. With the bot: 4.9 to 5.5 s before, 18 to
20 frames a second; 2.29 to 2.39 s after, 42 to 44, and 3.1 to 3.9 s in the windows the
bot's part is long in (below): the 300 frames in 15.2 and 15.4 s before, 8.1 and 8.6 after.
The first core's passes are 99.9% used and it rests in a third of them, the worker's
frame being the longer of the two there; a full write cache ended 5 and 7 passes of 3,300
where it ended 96 and 111 of 6,200.

With the bot the server's worker had a frame of the game of one to nine million instructions
now and then (the bot's part, as before): the level stood still for a second or more, and
the client's frames of that time were longer (a moved player's moves are made again). What
it was, and the server's pace since: "The server's pace" below.

On `rvc_cpu` a worker sees a store at once, and a worker that goes to sleep in the pass a job
is posted in has not seen it and sleeps on (the shader machine's sleeps on the job word of
the pass before, and wakes): `client_shaderemu.c` posts again after eight passes of nothing.
The server's worker has the same there, rarely: its jobs are not posted at once.

**Checking it.** `QUAKE3_SUM=1 QUAKE3_SERVER=inline quake3 +set fixedtime 33 +devmap ...`
prints a sum of what every frame asked to have drawn, with the frame's time; run once with
`QUAKE3_CLIENT=inline` and once without, the sums of the same times must be the same. On
`rvc_cpu`: 522 of 522 standing, 598 of 598 jumping and turning (`+set go "+moveup " ... +vstr
go`). Which frame the first snapshot comes in depends on the machine's speed, and a run that
began the other way has other times for a while and a few frames of other sums: run it
again. The shader machine's floats are not `rvc_cpu`'s, so its sums are compared with its
own: 572 of 573 there, the one being the first frame the worker makes, which the first core
has made already (a model's pose goes a step on when a time is made twice).

Pictures, held with `QUAKE3_HOLD` under `rvc_harness --cpu` (`QUAKE3_SERVER=inline`, `+set
fixedtime 33`, two views): the build before at frame 700 and this one at frame 702 are the
same in every pixel of the GPU's target (the worker's frame is shown a frame later, and
starting the workers takes a frame more), and so is this one with `QUAKE3_CLIENT=inline` at
700. Against the GPU's model: 100.000% within 2 levels away from triangle edges, 128
commands as before.

Tried and not kept: a job a frame, posted when the last was taken (the worker waits two
passes of every frame for the first core to see and to post: 200 frames in 4.37 s on
`rvc_cpu`, which has one of the two passes, against 4.32 asked for ahead; on the card that
build, without the numbered states, made 100 frames in 2.41 to 2.57 s). The first core
still waits 2.3 passes a frame on the card: not looked into. Not tried: the picture's part on a worker too (a worker keeps
half the first core's stores a pass, and the commands are all stores), the server's
snapshot handed over without the packet, the bot's part on a worker of its own.

## The server's pace

What moves in the level moves at the server's pace: the bot, the items, a hit, a door. The
game wants ten frames of it a second (`sv_fps`), and a job of the worker's makes up to 200 ms
of them, so a job that takes longer loses the rest and the game goes slower than the clock.
`QUAKE3_SERVER_LOG=1` prints a `q3srv:` line a job: when it was posted, begun, ended and
taken (the machine's clock), the game's time it was given, its frames and instructions, and
the parts' (the bots, the game, telling the clients, the client's moves and how many).

On the shader machine (an RTX 5090, D3D11, through the card queue, a pass 2.1 to 2.2 ms), the
bot's scene of above, frames 200 to 1,000, on 13 October 2026. "Before" is the build of 12
October and "that morning" this day's commit, one run each; the others four runs each, in turn:

| | Frames a second | Jobs a second | Frames of the game a second | The game's time, of the clock's | A job, posted to taken: median, 90th, longest |
|---|---|---|---|---|---|
| before | 22.9 | 7.4 | 8.4 | | |
| that morning | 46.3 | 7.5 | 8.7 | | |
| the same by this build's switches | 40.1 to 44.0 | 5.7 to 7.2 | 8.8 to 9.1 | 88 to 91% | 100 to 152 ms, 180 to 217, 2,300 to 2,530 |
| that, `QUAKE3_CLIENT=inline` (one run) | 31.1 | 7.8 | 9.0 | 90% | 98, 160, 2,077 |
| now | 43.3 to 46.9 | 9.8 to 9.9 | 10.0 to 10.1 | 100% | 70 to 74 ms, 92 to 118, 243 to 274 |

The level alone was at ten a second before and is (a job 171 thousand instructions, 130 now).
The worker runs 3.3 to 3.7 million instructions a second inside its jobs, 92 to 95% of its
passes: it is not held by its 1.5 KB of stores a pass, it has no more instructions. On
`rvc_cpu` at 3 million a second: 8.2 to 8.7 frames of the game a second and 82% of the clock
with the switches, 10.0 and 100% without; the longest job 3,234 ms and 322.

What held it, and what was done (`server_shaderemu.c`, `quake3.patch`):

- **The long frame was the bots' routes.** A bot that chooses an item asks the way to every
  item of the level at once, and the way to a place is worked out for the whole level the
  first time anybody asks (`AAS_UpdateAreaRoutingCache`: 280 to 450 thousand instructions a
  place of q3dm17's 570 areas, 21 places for 29 items). That was 9 million instructions in
  one frame of the bots': 2.1 to 2.5 s in which nothing of the level moved (3.2 s at 3
  million a second), once when the bot came and once more when it first had the rocket
  launcher and the health for a rocket jump, which is another set of ways. Both sets are now
  made by the server's worker while the client loads the level (`SE_ServerWarm`, from
  `CL_InitCGame`: 17 million instructions, 69 KB), when a bot is there by then, as it is from
  the menu and from `+addbot` on the command line. The time to the 100th frame is the same
  (26.7 to 27.0 s against 26.6 to 28.4): the first set was made in those frames before.
- **A frame is taken and the next given out where the first core finds it ended**, in every
  pass it waits for the client's worker and at a frame's parts (`SE_ServerPoll`), not once a
  frame of the client's: 15 ms a job to 4 or 5. Not when the frame kept a line, a command or
  an error for the first core: that waits for the frame's own call, as before.
- **Of several frames of the game in one job only the last tells the clients**
  (`se_server_quiet`): they got the others in the same moment, and telling is 70 thousand
  instructions of a frame's 200. It is what a client that asks for fewer snapshots gets.
- **The level's answer to a trace straight down is kept** (`SV_Trace`, 16 of them, by the
  trace's own numbers): a player who stands asks the same two with every move, 4,450
  instructions each, and the client sends a move every frame of its own. A move of a
  standing player was 16 to 18 thousand instructions of the server's, 6.2 to 7.6 of them a
  job since the client's frames doubled where `QUAKE3_CLIENT=inline` has 3.7: it is 11.2
  thousand now. A player who moves gains nothing by it.

So the client's worker did slow the server, by the moves: twice the frames are twice the
moves, 100 to 121 thousand instructions a job against 65. Nothing else the two workers share
was found to cost: a pass is as long with the client's worker (2.11 to 2.14 ms against 2.11).
The server's worker's page stops and system calls were not counted; its passes being 92 to
95% used leaves little for them.

A key's way to the screen, counted from the code and the times above and not measured: a turn
or a step is the client's own and is on the screen two of its frames after the key (the frame
that reads it asks the worker for the next, which is drawn a frame later), 43 ms at 46 frames
a second, where it was one frame of 44 ms. What the server answers (a hit, an item taken)
waits for the job in flight, then a job of its own and the frame that shows it: about 100
to 190 ms now, 160 to 350 that morning.

**Checking it.** `QUAKE3_SUM=1 QUAKE3_SERVER=inline ... +set fixedtime 33` as above, on
`rvc_cpu`: the sums are this day's commit's (574 of 574), with `QUAKE3_TRACES=all` and
without (574 of 574), on the client's worker and not (573 of 573). The server on its worker
has no such sum: which frame a move lands in is the machine's speed.

Not done, with what was measured for it:

- **A player who moves.** A move is then the game's whole step, several traces of the level
  at 4 to 5 thousand instructions each: not measured here, by that count 30 thousand or more
  a move, and at 45 moves a second about a third of the server's core. With it the server is
  short of its ten frames again. `pmove_fixed 1` with `pmove_msec 33` is the game's own way
  to at most 30 steps a second on both sides; the player's own motion then goes in steps of
  33 ms under frames of 22. Not tried.
- **A route to a place nobody went to before** is still 280 to 450 thousand instructions in
  the frame that asks, a job of twice the length: a bot that follows a player asks for one
  for each area the player comes to. All of them made ahead are 1,140 routes, 320 million
  instructions and 1.4 MB: for `bake.py`, as the file the library reads (`AAS_ReadRouteCache`).
- **A bot added by hand later** (`addbot` at the console) asks for its routes itself, as before.
- Two bots; telling the clients (70 thousand a frame: every entity's 208 bytes copied and
  compared for each client); the bots' part (85 thousand a frame of the game).
- Nothing of this was played by hand: the figures are of a player who stands.

## Checking it

Hold a frame under `rvc_harness --cpu` with `--save-state` and `--gpu-capture`, then

    python tools\gpu_reference.py SNAP BMP --size 640x480 --rom build\images\NAME\rootfs.bin

Four views of the level (`devmap`, `setviewpos`, `wait 90; hold`): away from triangle edges
99.4 to 100% of pixels within 2 levels of the model. What differed in one was a moving
texture, the capture being a frame older than the list; `hold` now waits for the GPU.

## Not done

- Playing it by hand, and hearing it. The pointer turns the view while the left button drags it
  (`docs/input.md`).
- A second character, the other three levels of the demo, the menu's setup pages.
- Marks and dynamic lights; the server's frame for two bots.
- The server's frame with a player who moves, a second bot, and a bot's route to a place
  nobody went to before ("The server's pace"). The client's game on its worker was not
  played by hand or in the world, and its sounds were not heard.
- The menu's picture was not captured (the card was in use): it was driven by `nxkey` on
  `rvc_cpu`, 93 thousand instructions a frame, and a game started from it.
