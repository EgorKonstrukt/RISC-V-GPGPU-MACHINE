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
  `server_shaderemu.c` the server's frame on a worker core, `sys_shaderemu.c` the system.
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
    QUAKE3_SIZE=WxH       the window (640x480)
    QUAKE3_WANTED=1       a line for every file, model, picture and sound asked for; nothing is "missing"
    QUAKE3_DEFINES=-DSE_MEMPROF QUAKE3_LINK=-Wl,--wrap=memset,--wrap=memcpy   (to build.sh) who fills and copies how much

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

What two cores would both write there is two of (`quake3.patch`): a command's words, the
marks of what a trace has tested and the box a thing is clipped as, the buffers `va()` and
`Info_ValueForKey` return, a zone for the server's allocations. Console lines, commands and
cvar changes made on the worker are kept and done by the first core; an error ends the job
and is raised there. Each module calls the engine by its own way in, not by which was called last.

Not looked at: `COM_Parse`'s one token buffer, and `rand()`'s one state.

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
- The menu's picture was not captured (the card was in use): it was driven by `nxkey` on
  `rvc_cpu`, 93 thousand instructions a frame, and a game started from it.
