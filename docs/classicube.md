# ClassiCube

ClassiCube, the open client for Minecraft Classic, runs in the Linux image as a Nano-X client:
a 64 x 64 x 64 world it generates at start, to walk, dig and build in. Everything on the screen
is drawn by the machine's GPU. It was chosen because a frame of it is almost all drawing: the
world is chunks of quads that stay in GPU memory, and the CPU works when a block changes.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh        (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/classicube/build.sh   (6 s; the first time it also fetches)
    python tools\make_linux_image.py                                     (then a new shell snapshot)

In the guest: `nano-X -p & classicube` at the console, or ClassiCube in the Start menu's Games
(not tried from there). It starts in single player. W, A, S and D walk, Space jumps, the left
button digs and the right one builds; the pointer's travel over the window turns the view, and
so do the arrow keys, since the pointer is not held in the window. Escape is the game's menu.
Its textures are the game's own (`misc/cc_textures.zip` of its repository), so the build
fetches nothing but the source.

Lines typed at the terminal it was started from drive it, which is how a test reaches it
through the machine's serial console:

    down KEY / up KEY     a key or button, by the game's names (W, Space, Right, LeftMouse, RightMouse)
    turn DX DY            the pointer's travel, in pixels
    point X Y             the pointer's place in the window
    hold                  the frame after next stays ("classicube: holding frame N")
    anything else         said in chat (the game's own commands start with /)

## What the build is made of (`linux/classicube`)

- `build.sh` fetches the source at a fixed commit, applies `classicube.patch` and compiles every
  file of the game with `PLAT_SHADEREMU`: each leaves itself out by the platform's defines.
- `Graphics_ShaderEmu.c` is the game's graphics backend, written on the GPU device itself
  (`docs/gpu.md`), not on the OpenGL library: command lists, matrix blocks and GPU memory are
  its own.
- `Window_ShaderEmu.c` is the Nano-X window, the keys and the pointer, and the terminal's lines.
- `Math_ShaderEmu.c` is sine and cosine in floats: the game's own work in doubles.
- `store_zip.py` rewrites the texture pack with nothing compressed, at build time: inflating
  the zip and its PNGs was 7 seconds of every start.
- `classicube.patch` (`git diff` of the tree at `build.sh`'s commit): the platform in `Core.h`
  (low-memory build, no launcher, no FreeType, no sound), a vertex colour as the device has
  them, the map generator's noise in floats, one chunk built a frame, the chunk builder's
  short cuts (below), water's and lava's animation at 5 and 2.5 pictures a second, random
  block ticks at a quarter of the rate, the map's borders in quads of 16 blocks, a view
  distance of 128 and a seed from `CLASSICUBE_SEED`.


## Meshes from a worker core

A chunk's mesh is some 100 to 200 thousand instructions, and the game builds one a frame
while it has any to build: 15 seconds at 4 frames a second after the world is made. On a
machine with worker cores (`docs/multicore.md`) a second builder runs on one:

- `Builder.c` is compiled twice (`build.sh`: `-DSE_SECOND`, its three public names renamed).
  The second copy has every buffer of the first as its own, takes eight chunks at a time
  that have never had a mesh, and builds them into memory of its own; it makes no vertex
  buffer and writes nothing of the chunks'.
- The first core goes on building the nearest chunk each frame, and when the worker's eight
  are done makes them the chunks' own (`SE_Mesh_Collect` in `MapRenderer.c`): the parts'
  counts, and the vertices into GPU memory as any chunk's are (`Gfx_SE_UnlockPacked`).
- The two copies share no variable they write. They did share one, the game's `Drawer` (a
  block's corners, written for every block drawn): a chunk of one core now and then had
  vertices of the other's block, 6 of 52 meshes in one run. The builder's own faces (below)
  do not use it.
- A chunk that is made again (a block changed) stays the first core's, which draws it by its
  old mesh until the new one is there. Anything that makes chunks stale while the worker has
  some (`se_mesh_serial`) throws its eight away, and whatever frees the chunks or the blocks
  waits for it first (`MapRenderer_SE_Settle`).
- The build gives every variable its own 16 bytes, as Quake's does, and the game touches all
  of its data once before the first job (`mcw_touch`), or the worker would stop at each page
  of its own buffers until the next frame.

The world's making has two steps that are noise by the column, a third of it: the height
map and the strata. Both are shared out by rows of z between the first core and up to fifteen
workers (three until the machine had 64 cores: the command to the steady world went from
35.4 s to 34.6 s, so these two steps were near their end already) (`SE_GenShare` in `Generator.c`; the game asks for the workers on its own thread
before the generator's begins). The rest of the making draws from one run of random numbers
and stays in order.

From the command to the whole world drawn: 38.0 s on four cores (40.3 s on two, where only
the meshes have a worker), 52 s with `CLASSICUBE_MESH=inline` (the first core alone); the
finished world is the same 83 commands and 6,980 quads.

## How it is drawn

- **A chunk is packed vertices** (`VERTEX_PACKED`): a word a vertex, bytes of x, y and z in
  eighths of a block from the chunk's corner, with the texture coordinates a word beside it:
  32 bytes a quad. Each chunk has a matrix block that puts it in its place. The colour of a
  vertex (a face's shade, in sun or in shadow) is a tag into one table that all chunks share.
- **The builder writes those vertices itself** (`SE_RenderBlock` in the patch's `Builder.c`,
  in place of the game's `NormalBuilder_RenderBlock` and `Drawer.c`): no float is made to be
  converted. What a kind of block's faces come to (its corners in eighths, its texture
  coordinates in 1,024ths, with the game's own float arithmetic and the backend's rounding)
  is worked out once for each chunk that has the kind; a block is then integers added to
  them, and a run of blocks whole tiles added to u. `Gfx_SE_UnlockPacked` moves the bytes to
  count from the mesh's origin and puts each quad's tag in: 8 instructions a vertex, where
  packing floats was 72. The game's builder for smooth lighting still writes floats.
- **The terrain's textures are four across.** A packed vertex has 12 bits of u in 1,024ths,
  four repeats, and a run of 16 equal blocks is one quad that repeats its tile 16 times. So
  the atlas the chunks read is the game's (a strip 16 texels wide) written four times side
  by side, and u is a quarter of the game's.
- **Other buffers are compact vertices**, a texel each: with the command's colour when all of a
  buffer's vertices have one, with tags and a table of the buffer's own colours otherwise. A
  quad's texture coordinates are moved to start in the texture's first repeat.
- **A buffer filled and drawn again and again in one frame lives in the frame's set** (the 2D
  screen's quads, the models); one drawn frame after frame as it is gets GPU memory of its
  own on its second frame, and costs nothing after.
- **Passes**: what tests depth is pass 0, or pass 1 when it blends (water, which the game
  draws twice, depth first: here that first draw is left out, since pass 1 writes no depth).
  Everything drawn without depth (the 2D screen, the block in the hand) is pass 5, blended,
  so that it keeps its order. The alpha test is a key: a texel with no alpha becomes wholly
  transparent black when its texture is copied, and pass 0 leaves those out.
- **GPU memory** is the 3.3 MB a GL program has and, while the desktop is layers, the
  display's own framebuffer (3.9 MB), handed out in spans; a span given back is free two
  frames later, so that the list being shown never reads freed memory.

Not as the game draws elsewhere: no fog (the far chunks end at a line), clouds that stand
still, no lines (the block outline is the game's quads, so it is there), nearest texels, and
no smooth lighting.

## Speed

Instructions a frame in a 854 x 480 window, standing where the game starts with seed 7. The
machine runs 3.9 million a second on D3D11 with fxc2:

| | A frame | Frames/s |
|---|---|---|
| the first build that drew | 235k | 10 |
| the generator's noise and sine in floats, a view of 128 blocks | 110k | 20 |
| texels copied in runs, no frame rate line, animations and random ticks slower | 48k | 70 |
| walking | 58k to 62k | 55 to 60 |
| digging without pause | 148k | 25 |
| building without pause at the top of a shaft | 1,200k | 3 |

- A frame standing still is about 85 draws and 7,000 quads. A draw is some 130 instructions.
- **No copy by a call in a draw.** `DrawQuads` kept its six words of state with `Mem_Copy` and
  `MatrixBlock` copied its sixteen with it: a call of the C library's `memcpy` for 24 and 64
  bytes, 16,000 and 5,000 times before the world stood. Both are word copies in place now: a
  frame standing still (block physics off, under `rvc_cpu`) went from 58.6k to 53.6k, with
  the same meshes and the same picture.
- **A changed block builds its chunk again**, one chunk a frame. That was 1,000k instructions,
  then 744k for the chunk under the player at the start, and is 416k (below). So a block dug
  is one frame of 0.1 s. A block put at the top of a deep shaft changes the light of every
  chunk under it, and each of those is such a frame: that is the last row of the table.
- **What a chunk costs**, in thousands of instructions, for that chunk (`-DSE_COUNT`):

  | | read | light | faces | blocks | vertices | all |
  |---|---|---|---|---|---|---|
  | rows noted after the read, floats packed after the build | 58 | 87 | 282 | 164 | 153 | 744 |
  | rows read by the word with their masks, vertices packed | 70 | 12 | 216 | 100 | 18 | 416 |

  - A row of 18 blocks along x is read as four words of the world and two bytes, written
    into the chunk as words, and becomes two masks of a bit a block as it is read: the full
    opaque blocks and those not drawn (`Builder_ReadRows`; four blocks of one kind are one
    look at the tables). That was two passes over 18 x 18 x 18 blocks, 13 and 10 instructions a
    block, and is 170 to 210 a row, half of it the loop's own. "Light" had the second pass.
  - From the masks of a row and the four rows round it: which blocks to look at at all (not
    air, not a full opaque block among six), and for each of those which faces a full opaque
    neighbour hides, without the tables (`Builder_RowTodo`, `Builder_Covered`). Outside the
    world counts as full opaque where the game shows no face towards it (under the world,
    beside it below the sides' level): without that every buried block at the world's border
    is looked at, in 56 chunks of 64. The blocks with a face to draw are noted for the
    drawing loop, which asks nothing of the others.
  - The bookkeeping walks the four atlases a map uses instead of all 512.
  - The start's 64 chunks, the first core alone: 55.6M instructions from the first mesh to
    the last, now 32.3M (16.8 s and 9.8 s). Of those, the builder's and the backend's own
    functions were 41.7M and are 22.1M: the light's height map is 4.0M of that (untouched),
    the rows' read 3.3M, looking at blocks 5.8M, writing vertices 2.6M and 0.6M. From the
    game's first stat line to frames coming steadily, by `rvc_cpu`'s clock: 21.8 s and
    14.7 s, with the worker 15.2 s and 10.6 s.
  - Not done: a block whose only open face an earlier block's run has taken is still looked
    at (some 18,000 of the 30,000 blocks looked at at the start, about 100 instructions
    each); a mask of what runs have taken would pass over them.
- Water flowing after a dig goes on changing blocks, a chunk each time.
- From the command to the world standing is about 30 seconds: half of it before the first
  frame (the game's own start, the texture pack's pictures), 3 the generator and 12 the 64
  chunks, at five to ten frames a second. It was 100.

- **The frame's list is not waited for.** `Gfx_EndFrame` waited, with `pause`, until the GPU
  had drawn the list, and what was left of that pass of the machine was not run: half a pass
  a frame, of a frame of seven (`docs/boot.md`, "Passes that end early"). It submits and
  goes on now. A frame writes its own set of commands, matrices and arena, which the list
  being drawn never reads; the two things that are written in place settle for it first
  (`Settle`): a buffer's own span when the frame before drew from it (`SpanAlloc`; a dynamic
  buffer goes into the frame's arena instead while the list is still out), and a texture's
  texels. An animated tile's new texels (water, lava: some twelve updates a second, at the
  start of a frame, when the wait would be longest) are kept and written when the frame
  ends, before its own list is submitted (`Later_Flush`): that list is the first that is to
  show them. Memory given back was free two frames later already.

  | the shader machine, `tools\tick_util.ps1` | utilization, waiting (`CLASSICUBE_SWAP=wait`) | not waiting |
  |---|---|---|
  | the world being made and meshed | 95.5% | 98.1% |
  | standing | 93.3% (the wait 6.4%) | 99.7% |
  | walking (`down W`) | 93.7% | 99.7% |
  | digging (`turn 0 300`, `down LeftMouse`) | 94.1% | 99.7% |

  What is left standing is the animated tiles' texels: a tile is 1 KB and its four copies
  across 4 KB more, most of what a pass keeps of new stores.

`ccstat:` lines, every five seconds, give frames, instructions a frame, draws, quads, and
the texels and vertices made for the device.

    CLASSICUBE_SEED=N        the same world every time
    CLASSICUBE_HOLD=N        keep frame N on the screen and stop there
    CLASSICUBE_HOLD_LATE=1   with CLASSICUBE_HOLD: the frame stays while the next is made ready, then the game stops
    CLASSICUBE_SWAP=wait     every list waited for, as it was
    CLASSICUBE_STATS_MS=N    how often the ccstat: lines come (5000; 0: never)
    CLASSICUBE_SIZE=WxH      the window's size (854x480)
    CLASSICUBE_MESH=inline   every mesh by the first core, in one order every time
    CLASSICUBE_MESH_SUM=1    a ccmesh: line a chunk mesh made: origin, vertices, two sums
    CLASSICUBE_DEFINES=-DSE_COUNT   (to build.sh) a chunk's instructions by phase, in the ccstat: lines

## A quarter to a half fewer (12 October 2026)

Under `rvc_cpu`, `CLASSICUBE_SEED=7`, block physics off: a frame standing 55.0 thousand
instructions to 38.9, walking 54.6 to 40.9, digging straight down 1,176 to 629; from the
command to `classicube: steady` 66.5 million on the first core to 41.5 and 33.1 on the
workers to 28.7.

- **A draw's state is worked out when it changes** (`state_serial`, `DeriveState`, `Emit` in
  `Graphics_ShaderEmu.c`): `DrawQuads` was a third of a standing frame, 170 instructions
  for each of some 120 calls. A new kind of state must bump `state_serial`, and anything
  that makes a mesh again goes through `MeshMade`.
- **A chunk's matrix block** is rows made once a matrix (`ChunkRows`) and three words a chunk.
- **Loops over entities start at `Entities_SE_First`**: alone, the player is slot 255 of
  256, and six loops walked all of them every frame or tick. The translucent depth-only
  walk is left out (the device draws nothing in it), `ResetPartFlags` resets the atlases
  in use, and the terminal is asked for a line 20 times a second, not every frame.
- **Particles are removed in one pass** (`Terrain_Tick`, `Terrain_RemoveFirst`): one
  `memmove` a particle was 18% of digging.
- **The texture pack's pictures are decoded at build time** (`store_zip.py` writes
  `SEPIXELS`, width, height, then the game's own pixel words on a word of the file;
  `Png_Decode` reads them in one piece): decoding was a fifth of the start. An older
  `classicube.bin` cannot read the new pack; `build.sh` writes both each time.
- **The world's generator**: `NotchyGen_FillOblateSpheroid` in whole numbers by rows, the
  random numbers inline (the same numbers), one cosine of phi a cave step, the surface
  layer shared out by rows like the height map (`SE_SurfaceRows`). The same world.
- **The light's heights are made a column at a time**, and on the first core for the
  worker's chunks (`SE_Mesh_Post`): both builder copies called `LightHint`, so two cores
  could store into the same 16 bytes.
- `build.sh` compiles the 16 files a profile is full of, and the worker's `Builder.c`, with
  `HOT` (`-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`): 3% of a frame, a fifth
  of digging.

Not built: the matrix blocks kept while the view stands still (4 thousand of a standing
frame; writing them in place would break the two sets); `Stopwatch_Measure` from the clock
word (1.5 thousand a frame, and frame times in whole milliseconds).

`CLASSICUBE_TEXTURE_SUM=1` prints a `cctex:` line a texture made (42 of them, the same
sums from the PNG pack and the decoded one). A held frame's window is the top left 854 x
480 of a `--gpu-capture`: outside it two builds may differ.

## Checking it

- The GPU against its model: hold a frame, pass `--save-state` and `--gpu-capture`, and run
  `python tools\gpu_reference.py SNAP BMP --size 854x480`. At the start 99.87% of pixels away
  from triangle edges are identical. The model cuts triangles at the near plane now, as the
  card does: before, it left out every triangle that crossed it, here the sky and the borders.
- A change to how it builds chunks: the meshes' bytes must stay what they were. Under
  `rvc_cpu`, with `CLASSICUBE_MESH_SUM=1 CLASSICUBE_SEED=7`, the `ccmesh:` lines of the build
  before and after are compared: with `CLASSICUBE_MESH=inline` line for line (52 meshes at
  the start, 69,828 vertices; then a block dug by `down LeftMouse` and `up LeftMouse` typed
  together, which makes two or three chunks again), with the worker as a set by the second
  sum, which has each vertex's colour for its tag (the tags go by the order chunks are made
  in). Switch block physics off for it (`echo singleplayerphysics=false >>
  /tmp/classicube/options.txt` before the game): dirt turns to grass by the clock, and two
  builds' worlds part after a dig.
- A change to how it draws: hold the same frame of the same seed before and after and compare
  the GPU's targets. Two runs of one build give the same picture (`rvc_harness --cpu` makes
  one in 12 s). Digging held down is by the clock too: dig by single presses.
- A change to what the backend writes when: `CLASSICUBE_HOLD=1500 CLASSICUBE_HOLD_LATE=1` with
  `--gpu-capture`, once with `CLASSICUBE_SWAP=wait` and once without. The pictures were the
  same pixel for pixel (854 x 480). A plain hold stops the game before it writes anything
  for the next frame, and cannot show a store made too early.
- To see where instructions go, snapshot a run at `classicube: steady` and resume it with
  `--pc-log` (`tools\pc_profile.py`, `classicube=classicube.nm`).

## Not done

- Playing it by hand: the window's keys and pointer were written as Quake's are and never
  pressed; only the terminal's lines were tried. The world's beams are a pointer, not a
  mouse that is held: turning by them wants a look.
- Sound (the game's WAVs are in its repository; the sound card could play them from the ROM).
- Fog, and clouds that move.
- A world larger than 64 x 64 x 64 (the low-memory build's), and one kept from run to run:
  a saved map goes to `/tmp`.
- The VRChat world: "Import boot images" after this image, and the board's list of programs.
- `linux\prebuilt` does not have it.
