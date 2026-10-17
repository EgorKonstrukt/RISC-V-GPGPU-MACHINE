# Plan: Micropolis, and Half-Life: Uplink on Xash3D FWGS

Micropolis has since been built (`linux/micropolis`, `nxmicropolis`; `docs/micropolis.md`);
Half-Life has not. What follows is the plan as it was written.

Written 15 October 2026. Nothing here is built. It comes from reading the upstream trees at
the commits below, from compiling and running Micropolis's engine on the build computer (WSL,
x86-64), from unpacking Uplink's installer there, and from reading `linux/quake`,
`linux/quake3`, `linux/tdawn`, `linux/apps/build.sh`, `docs/gpu.md` and `docs/quake3.md`.
What the reading did not settle is under "To find out first" in each part.

| | upstream | commit | licence |
|---|---|---|---|
| Micropolis | `github.com/SimHacker/MicropolisCore` | `9d5b400` (14 Oct 2026) | GPLv3 with EA's added terms; the name under the Micropolis Public Name License |
| Xash3D FWGS | `github.com/FWGS/xash3d-fwgs` | `141dfef` (15 Oct 2026) | GPLv3 (the files' headers) |
| hlsdk-portable | `github.com/FWGS/hlsdk-portable` | `6c168fc` (28 Sep 2026) | Valve's Half-Life 1 SDK licence: free distribution only |
| Uplink | `archive.org/download/hluplink/hluplink.exe` | md5 `498afa6af130a64ff5966774654b0f18` | Sierra's EULA: one person's use, **no redistribution** (below) |

## Micropolis

### What upstream is now

MicropolisCore has become a monorepo for the web: a SvelteKit app, renderers for Canvas, WebGL
and WebGPU, and an Electron overlay. The part we need is in two folders:

- `packages/micropolis-engine/src`: the simulation, 27 C++ files with 18,233 lines in total. It
  builds with Emscripten through a makefile. `emscripten.cpp` holds the Embind bindings.
- `content/micropolis`: the game's data.
  - `cities/`: 32 cities and scenarios of 27 KB each (0.9 MB in all).
  - `images/`: `tiles.png`, the 960 tiles of 16 x 16 pixels as a 256 x 960 sheet. It has
    **14 colours**. The sprites (`sprite_N_M.png`) have 20 colours between them, and there
    are the tool icons (`ic*.png`).
  - `sounds/`: 52 MP3s, 0.87 MB.
  - `data/`: strings and scenario texts.
  - `tilesets/`: seven other looks. They take 5.8 MB and are not needed.

The engine is tied to Emscripten in one place only. The callback interface (`callback.h`)
passes an `emscripten::val`, and `micropolis.h` already declares a stand-in class for that
type when `__EMSCRIPTEN__` is not defined. Built on the build computer with g++ and
everything but `emscripten.cpp` and `callback.cpp` (the console callback, which needs the real
Emscripten), it compiles to 137 KB of code. With a callback of our own that does nothing, it
ran Haight-Ashbury for 2,000 ticks in 0.03 s.

That run needed two fixes. Each is a line of a patch:

- `Micropolis::Micropolis()` leaves `callback` unset, and `setCallback` deletes whatever the
  pointer holds. That crashes unless the object's memory was zeroed.
- `newSprite` (`sprite.cpp:114`) gets a `SimSprite` from `malloc`, but the struct holds a
  `std::string`. Assigning to it crashes. `new SimSprite()` fixes it.

The engine is not deterministic as it stands. `seedRandom(1)` after loading a city still
gave a different map sum on each of two runs (`e6e8921a` and `704addae`). The time is read in
two places, `randomlySeedRandom` and `tickCount` (which sets the blink flag), and there may be
other causes.

### The licence, as it bears on what we ship

- **Code:** GPLv3 with EA's added terms. These forbid using "SimCity", forbid claiming any tie
  to EA, and require a modified version to be marked as modified.
- **Name:** "Micropolis" is a trademark that may be used for non-commercial work. The
  attribution must appear on the title page or the About page, and in the credits.
- **Data:** the cities, tiles and sounds are in the same GPL repository. The repository names
  no separate licence for the assets.

So the program is called Micropolis everywhere, including the Start menu, its caption and its
docs. Its About text carries EA's notice and the trademark line. Nothing in it says
"SimCity".

### How it fits this machine

- **Building.** The C++ is compiled as Tiberian Dawn's is: the `rv32-c++` driver with
  libstdc++'s headers, over musl (`linux/tdawn/build.sh`). Tiberian Dawn's `cxxrt.cpp` already
  provides allocation, `std::string` and the `__throw_*` functions. `tool.h` also uses a
  `std::map`, which needs libstdc++'s red-black tree functions (`_Rb_tree_insert_and_rebalance`
  and its siblings, from `tree.cc`). Either they are added to the runtime, or the map, which
  holds a tool's changes before they are applied, is replaced. The engine uses no threads,
  exceptions or RTTI.
- **The map is the case "commands that stay" (`seglKeep`, `docs/gpu.md`) was made for.** A
  window shows 40 x 25 tiles or so of the 120 x 100 map. Each tile is a kept quad on one
  paletted texture: 14 colours, so one byte a texel, 245 KB in GPU memory. A frame costs only
  the tiles that changed. Each pass the program compares the engine's `map[x][y]` with what
  it drew last, and rewrites only those quads' texture coordinates. Tile animation
  (`animateTiles`, `ANIMBIT`) is the only steady source of changes. Scrolling moves the kept
  matrices (`seglKeptMatrices`), as Doom's level does (`docs/doom.md`). The sprites (train,
  helicopter, aeroplane, ship, monster, tornado, explosion) are a few `seglQuad`s a frame from
  a second atlas.
- **The rest of the window** comes from the desktop's own parts (`ui.h`, `ui_menu.h`,
  `ui_ask.h`):
  - the tool palette, from the `ic*.png` icons;
  - funds, date, and the residential, commercial and industrial demand bars;
  - the message line, from `data/strings_en-US.xml` and `stri.*.txt`;
  - the budget, evaluation and graph windows;
  - the overview map, one pixel a tile, coloured by kind of tile.
- **Sound.** The 52 MP3s go through libmedia (`docs/media.md`) and the sound card
  (`docs/sound.md`). Alternatively they are decoded at build time into a pack the card plays
  from the ROM, as `tdawn-sound.pak` is.
- **Saving.**
  - Cities are written under `/persistence/micropolis` (`docs/persist.md`). A city is 27 KB
    raw and the folder has 96,000 packed bytes in all, so measure how small a city packs
    before deciding how many save slots to offer.
  - Speed, the tile overlays and auto-budget are registry keys (`micropolis.speed` and so on),
    not a file.
- **Opening.** Its line in the Start menu is `Games/Micropolis`. An `nxopen.*` line opens
  `.cty` files with it.
- **The image.** One tileset, the cities, the sounds and the strings come to about 2 MB.

### Steps

1. **`linux/micropolis/build.sh`.** Fetch `9d5b400`, apply `micropolis.patch` (the two fixes
   above and the determinism fix), and compile `packages/micropolis-engine/src` without
   `emscripten.cpp` and `callback.cpp`. First build a headless driver, `micropolis-sum`. It
   loads a city, runs N ticks with a fixed seed and prints population, funds, year and a sum
   of the map. Run it on `rvc_cpu` and on the build computer. The sums must agree, and
   `RVC_CALLS` gives the instructions a tick costs. Add it to `tools/test_all.py`.
2. **`nxmicropolis`.** A Nano-X window with the map as kept quads, scrolling, and the tool
   palette. Build, bulldoze and query from the pointer. The demand bars, funds and date.
   Pause and the three speeds. Judge it by its frames, on a page.
3. **The rest of the game.** Messages, the budget window, evaluation and graphs, the overview
   map with its overlays (power, traffic, pollution, crime, land value), disasters, the
   scenarios' win and loss, sound, saving and loading, `.cty` in `nxopen`, and the About text.
4. **Measure, then use the workers.** If a tick at the fastest speed costs a noticeable share
   of core 0's frame at 2 MIPS, the scans are the candidates. Pollution, crime, land value,
   population density and fire and police coverage are each a pass over the grid that writes
   only its own map, so they can be split into rows or blocks across worker cores
   (`mcwx.h`'s loops, as nxphys splits its grid). The power flood fill and traffic's random
   walks are serial and stay on core 0. Run once with `RVC_RACES`. The headless driver's sum
   must not change with workers or without them.

### To find out first

- **The cost of a tick.** The 2,000 ticks in 0.03 s on the build computer suggest tens of
  thousands of instructions a tick, but that is a guess until `rvc_cpu` counts it.
- **What else makes two seeded runs differ.** Look first at `tickCount` and at memory that is
  read before it is written (`newPtr` is `malloc`).
- **How small a saved city packs** under `nxpersist`.

## Half-Life: Uplink on Xash3D FWGS

### The data is not ours to ship

Uplink's installer carries Sierra's EULA (`TEMP/EULAFILE` inside it). It grants "a limited,
non-exclusive license ... to install and use one (1) copy of the Program for your personal
use". It forbids anyone to "copy ... reproduce ... the Program" except as the licence allows,
and nothing in it allows redistribution. The README adds nothing to that.

This is unlike Quake's shareware episode, which may be passed on. I said otherwise in chat
before reading the EULA, and I was wrong.

So the build can fetch it, as `linux/quake/build.sh` fetches Quake, but an image with
Uplink in it is for one person's machine. It must not go into `linux/prebuilt`, the shared
image or the VRChat world. The world's visitors cannot bring their own copy either:
`/persistence` holds 96,000 bytes. **Decide this before step 1.** If the answer is "only in a
private image", the work below still holds. Its documentation then says so, and the image
builder takes Uplink only when asked.

A second licence question: hlsdk-portable is Valve's SDK, which may be distributed "only for
free", and the engine is GPLv3. Upstream keeps them in separate libraries loaded at run time.
This machine's programs are static (Quake III's modules are linked into one binary), which
makes one combined work. Whether that is acceptable is the developer's call.

### What upstream has that we need

- **RISC-V, 32-bit, our float ABI.** The architecture detection (`3rdparty/library_suffix`,
  `include/build.h`) knows `__riscv_xlen == 32`. It maps `__riscv_float_abi_soft` to
  `XASH_RISCV_SOFTFP`, which is our `-march=rv32imaf -mabi=ilp32`. musl works
  (`Documentation/musl.md`).
- **Static linking.** Waf's `--static-linking` defines `XASH_STATIC_LIBS` and builds
  `engine/platform/misc/lib_static.c`. In that file `COM_LoadLibrary` finds a library by name
  in a generated table (`generated_library_tables.h`), and `COM_GetProcAddress` finds a
  function by name in the library's own table. Six libraries, and freevgui (below), become
  one binary: the engine, the filesystem, the renderer (`ref/gl`), the menus
  (`3rdparty/mainui`, C++), the client (`cl_dll`) and the server (`dlls`, `hl`). The client and the server share code
  (`pm_shared`) and names, so each is linked to one object whose names stay inside it, as
  Quake III's modules are (`docs/quake3.md`, `sys_shaderemu.c`).
- **A platform layer that is not SDL.** `common/backends.h` picks video, sound, input and
  timer backends. There are SDL, fbdev, DOS and PSP ones. `engine/platform/linux/vid_fbdev.c`
  (253 lines, software rendering only) is the smallest model. SDL2's input (364 lines) and
  sound (337) show what a backend must provide. Ours is
  `engine/platform/shaderemu/{vid,in,s,sys}_shaderemu.c`, which needs a Nano-X window, input
  as Quake's `vid_shaderemu.c` reads it, and the sound card. `GL_GetProcAddress` hands the
  renderer our GL functions by name.
- **A renderer that comes from GLQuake.** `ref/gl` has `gl_rsurf.c`, `gl_warp.c`, `gl_rmain.c`
  and `gl_draw.c`, as GLQuake does, plus `gl_studio.c`, `gl_beams.c`, `gl_decals.c` and
  `gl_triapi.c`. Quake's shim (`linux/quake/qgl.c`, 58 GL calls turned into `gles.c`'s compact
  vertices) is the model. Half-Life differs from Quake in three ways:
  - every texture has its own 256-colour palette, where `gles.c` has one shared palette, so
    textures become RGBA or get palettes of their own;
  - light maps are coloured (Quake III's are too: `tr_shaderemu.c`);
  - models are skeletal (`gl_studio.c`): every vertex is transformed by its bone on the CPU
    each frame.

  The software renderer (`ref/soft`) is not considered. Quake's software renderer at 2 MIPS
  is out of reach.
- **Uplink is a known game to it.** `gameinfo.txt` has a `demomap` key for "the demo chapter
  map (Half-Life Uplink)" (`Documentation/gameinfo.md`).
- **VGUI without Valve's binary.** hlsdk-portable's client always links VGUI, but through
  freevgui, an open reimplementation (`3rdparty/freevgui`). It can be linked in statically
  (`USE_STATIC_FREEVGUI`, `INTERNAL_VGUI_SUPPORT` in `cl_dll/wscript`). That is a seventh
  library in the binary.
- **Memory.** The guest has 128 MB. Waf's `--low-memory-mode N` exists for small machines.

### The data, as unpacked

`hluplink.exe` (50,872,079 bytes, the same file in the archive.org items `hluplink` and
`Half-lifeUplink`) is a **Wise** installer. REWise (`codeberg.org/CYBERDEV/REWise`, GPLv3, C,
made for game installers of 1999 to 2003) builds with CMake in WSL. It listed and unpacked the
installer in under 5 s without running it: 75 files, 89 MiB. What the game reads is
`MAINDIR/valve`:

| | |
|---|---|
| `pak0.PAK` | 79.2 MB, 1,952 files |
| of which `sound/*.wav` | 29.8 MB, 1,477 files |
| `maps/*.bsp` | 26.5 MB: `hldemo1`, `hldemo2` and `hldemo3` (7.9 MB together) and the seven hazard-course maps `t0a0*` (18.6) |
| `models/*.mdl` | 7.8 MB, 220 files |
| `sprites/*.spr` | 6.7 MB |
| `gfx/` | 7.9 MB |
| outside the pak | `decals.wad` 0.9 MB, `gfx.wad`, `cached.wad`, `liblist.gam`, configs |

Its `dlls/hl.dll` and `cl_dlls/client.dll` are Windows code and are not used. hlsdk-portable
builds both. The build fetches the installer (pinned by md5), builds REWise once, as Quake's
build does lhasa, unpacks it, and then bakes it, as Quake III's `bake.py` does:

- keep only the three Uplink maps (the hazard course optional);
- keep only what a run with the game logging its file opens asked for (`wanted.txt`);
- sounds at 11 kHz.

My guess for what reaches the image is 15 to 25 MB. Measure it.

### Steps

0. **The licence decision above.**
1. **The engine on `rvc_cpu`, with the null renderer and no window.** Write `build.sh` with
   the source lists written out, as Quake's is, not waf cross-compiling. Waf is run once on
   the build computer to print the lists and the generated table. Static libraries with
   localised modules, the `shaderemu` platform with sound and input stubs. `+map hldemo1`
   runs headless (Xash's `-dedicated` for the server alone, then the client with `ref/null`),
   and its frames and instructions are counted. This shows whether the game and the server
   fit in 128 MB, and what a server frame costs next to Quake's 195k instructions.
2. **`ref/gl` on `gles.c` through a shim like `qgl.c`.** RGBA textures first; per-texture
   palettes only if GPU memory or the time to load a level says so. Coloured light maps, then
   studio models with the CPU skinning as it is. The frames are compared with the GPU's model
   and judged on a page.
3. **The window, input, sound and menus.** A Nano-X window, keys and the pointer as Quake's
   (`vid_shaderemu.c`), the sound card from a baked pack (`snd_shaderemu.c` in `linux/quake3`),
   and mainui's menus cut down to what works here (`ui_menu.c` in Quake III's patch). The
   Start menu's `Games/Half-Life: Uplink`. Settings and saved games in `/persistence/uplink`.
   A saved game may not fit in 96,000 bytes: measure it.
4. **Speed, as for the other games** (`docs/multicore.md`, "A game's speed: passes a
   frame"). The server's frame on a worker core, as Quake's and Quake III's are; then the
   client's studio models (bones and skinning) on workers, or their poses read by the GPU as
   Quake III's are. Measured on a live level with its server (not a timedemo), on `rvc_cpu`
   and then once on the card. `RVC_RACES` for every worker part.

### To find out first

- The **licence decision** (step 0).
- Whether the engine, the game, a level and the menus **fit in 128 MB** with
  `--low-memory-mode`.
- **What a server frame and a client frame cost** on `hldemo1` at 2 MIPS, before any work on
  speed. Half-Life's monsters and skeletal models are heavier than Quake's.
- Whether hlsdk-portable's server and client **link into one binary** with only their names
  localised, or whether there is more global state that both write.
- **The size of the baked data** in the image.

## Order

Micropolis first. It is small, its data can ship, it needs no new renderer, and its first
step is a test that can be run today. Uplink waits on its licence decision. After that its
first step, the engine headless on `rvc_cpu`, is the cheapest way to learn whether it fits
this machine at all.
