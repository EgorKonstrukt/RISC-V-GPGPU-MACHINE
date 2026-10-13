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


## Meshes from worker cores

A chunk's mesh is some 100 to 400 thousand instructions, and the game built one a frame
while it had any to build: 15 seconds at 4 frames a second after the world is made, and a
frame of 0.14 s for every block dug. On a machine with worker cores (`docs/multicore.md`)
the meshes are all theirs, and the first core builds none:

- `Builder.c` is compiled again for each of seven workers (`build.sh`: `-DSE_SECOND`, its
  four public names renamed; `SE_COPIES` in `MapRenderer.h`). A copy has every buffer of the
  first as its own, takes up to two chunks at a time and builds them into memory of its
  own; it makes no vertex buffer and writes nothing of the chunks'.
- Each frame the first core gives the nearest chunks that wait for a mesh to the workers
  that have none (`SE_Mesh_Post` in `MapRenderer.c`), and makes what a worker has finished
  the chunks' own (`SE_Mesh_Take`): the parts' counts, and the vertices into GPU memory as
  any chunk's are (`Gfx_SE_UnlockPacked`). Two chunks a job, so that the nearest are drawn
  soon and every worker has some.
- The copies share no variable they write. Two did share one, the game's `Drawer` (a
  block's corners, written for every block drawn): a chunk of one core now and then had
  vertices of the other's block, 6 of 52 meshes in one run. The builder's own faces (below)
  do not use it.
- A chunk that is made again (a block changed) is a worker's too, and is drawn by its old
  mesh until the new one is there: the old is let go and the new put in its place in one
  frame. A chunk has a count of the times it was asked to be made again (`made`): a mesh
  begun before the last of them is thrown away and begun again, and whatever frees the
  chunks or the blocks waits for the workers first (`MapRenderer_SE_Settle`). (One count
  for all chunks threw every worker's job away at each block changed: with water flowing
  no mesh would have been finished.)
- The light's heights of the whole map are made by the workers before the first mesh, rows
  of z each (`SE_Light`, `ClassicLighting_SE_Rows`): a row is whole 16 bytes. After that a
  changed column is the first core's, as it was.
- A chunk too large for a worker's buffer (16,384 vertices) is the first core's (`se_own`),
  and with no workers (`CLASSICUBE_MESH=inline`) all are, one a frame, as before.
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
finished world is the same 83 commands and 6,980 quads. With seven workers for the meshes
(13 October 2026, below) the meshes' part of it is 2 s where it was 7.

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
    CLASSICUBE_MESH_WORKERS=N   at most N workers for the meshes (7)
    CLASSICUBE_WALK=every    the chunks' commands made anew every frame, as they were
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

Not built then, and built the day after (below): the matrix blocks kept while the view
stands still, and `Stopwatch_Measure` from the clock word.

`CLASSICUBE_TEXTURE_SUM=1` prints a `cctex:` line a texture made (42 of them, the same
sums from the PNG pack and the decoded one). A held frame's window is the top left 854 x
480 of a `--gpu-capture`: outside it two builds may differ.

## The chunks' commands kept, and the meshes all the workers' (13 October 2026)

Under `rvc_cpu` at 3 million instructions a second and passes of 8,192, `CLASSICUBE_SEED=7`,
block physics off (frames in two seconds of the `ccstat:` lines, and instructions a frame):

| | before | after | |
|---|---|---|---|
| standing where the game starts | 134 frames, 40.8k | 276 frames, 19.9k | 2.06 times |
| jumping there (`down Space`: the camera moves every frame) | 121, 45.5k | 186, 29.5k | 1.54 |
| walking and jumping into a hill (`turn 1200 0`, `down W`, `down Space`) | 123, 44.5k | 169, 32.3k | 1.37 |
| digging down a shaft (`turn 0 300`, `down LeftMouse`), its two seconds | 65, 84k | 189, 29k | 2.9 |
| from the command to the whole world drawn | 16.7 s | 12.8 s | the meshes 7 s to 2 |

On the shader machine (D3D11, fxc2; one binary of each build from power-on, in turn, the
`ccstat:` lines of six seconds each):

| | before | after | |
|---|---|---|---|
| standing | 91.0 and 91.1 frames a second | 160.7 and 166.1 | 1.8 times |
| jumping | 79.9 and 80.3 | 108.4 and 115.0 | 1.4 |
| from the game's first stat line to the whole world | 7.7 s and 7.7 s | 5.4 s and 5.5 s | |

The machine ran 3.5 million instructions a second before and 3.1 after: what went was
cheap (a draw's words copied), what stayed is loads, stores and floats. Utilization 98.9%
and 98.7%, the loss in both the animated tiles' texels filling the write cache
(`CopyTexels`). The picture held after standing, jumping, looking down and two blocks dug
is the same in both builds, pixel for pixel (854 x 480), and the start's 52 meshes made by
the seven workers there are the old build's set by the second sum.

- **A walk of the chunks leaves its commands in the set, and they stay.** The game's two
  walks (`MapRenderer_RenderNormal`, `MapRenderer_RenderTranslucent`) came to the same 70 to
  80 draws frame after frame, each 110 instructions of the walk and the draw. The map's
  renderer counts what would change them (`se_walk_serial`: the list of chunks in view or
  its order, the faces towards the camera, a mesh made or let go, the atlases, the camera
  under water or not) and the backend notes a walk as it is made (`Gfx_SE_SegmentBegin`,
  `Gfx_SE_SegmentEnd`): where its commands are in the set, what they come to, the buffers
  drawn. While the count stands, no buffer or texture was made again (`kept_epoch`) and the
  walk would begin at the same command and vertex, the commands are left where they are in
  that set and the walk is not made. Each of the two sets has its own note, and one that a
  frame did not use is forgotten. `CLASSICUBE_WALK=every` is the old way; the `ccstat:`
  lines say how many walks were made and how many kept.
- **A chunk's matrix block has a place of its own in each set** (`KEPT_BLOCKS`, a slot a
  packed mesh), and the matrices have a number that stays while they are what they were
  (`MatricesLoaded`: the camera's, compared word for word when it is loaded; any other
  load has a number never used before). A block written for the camera's number is right
  until the camera moves: standing, nothing of the chunks is written at all. The 2D
  screen's and the held block's blocks are the frame's, as before.
- **Which chunks are in view is asked without going through them** when no chunk waits for
  a mesh: a plane's products with the grid's x, y and z are made once (`SE_Frustum`), a
  chunk's test is three sums a plane in the order `Frustum_TestSphere` makes them (the same
  floats), in the map's own order, and when every chunk is in or out as it was the list of
  those drawn stands (`SE_SameView`). With the camera where it was nothing is asked.
- **The frame's time is the machine's clock word** (`Stopwatch_Measure` in
  `Platform_Posix.c`): whole milliseconds, no system call and no division of doubles a
  frame (1.5 thousand instructions of a frame of 23).
- **The meshes are the workers'** (above): a block dug is not a frame of 0.14 s any more,
  and the world's 52 meshes are there 2 s after the map where they took 7. What is left of
  those 2 s is the first core's: the vertices into GPU memory (8 instructions each), the
  borders' buffers, the atlas four times across at its first draw.

Tried and not kept: every chunk in every list, with no test of what is in view (the list
then changes only when the camera leaves a chunk). 170 commands and 12,764 quads a frame
where 83 and 6,980 are in view at the start; jumping 30.4 thousand instructions a frame
against 29.5 with the test above, since every chunk's block is then written when the
camera moves, and walking into a hill 37.3 against 32.3.

Not done: a chunk's block is 28 words written whenever the camera moves, 25 of them the
same for every chunk (2 to 3 thousand instructions of a moving frame, and on the shader
machine, where a load or a store is four times an addition, an eighth of its time: it is
most of why jumping gained less than standing). The device reads a
block where the command says, so one block for all chunks wants an origin in the command
(a change to the GPU's shader, its model and Unity's copy). The held block and the 2D
screen are made anew every frame (some 5 thousand of a standing frame's 20). The
vertices of a worker's mesh could be the device's when the first core gets them. The
seven copies of the builder are 3.3 MB of variables that the first core touches at the
start (a million instructions).

## Checking it

- The GPU against its model: hold a frame, pass `--save-state` and `--gpu-capture`, and run
  `python tools\gpu_reference.py SNAP BMP --size 854x480`. At the start 99.87% of pixels away
  from triangle edges are identical. The model cuts triangles at the near plane now, as the
  card does: before, it left out every triangle that crossed it, here the sky and the borders.
- A change to how it builds chunks: the meshes' bytes must stay what they were. Under
  `rvc_cpu`, with `CLASSICUBE_MESH_SUM=1 CLASSICUBE_SEED=7`, the `ccmesh:` lines of the build
  before and after are compared: with `CLASSICUBE_MESH=inline` line for line (52 meshes at
  the start, 69,828 vertices; then `turn 0 300`, and at a later `ccstat:` line a block dug
  by `down LeftMouse` and `up LeftMouse` typed together, which makes two or three chunks
  again: the turn must have had a frame), with the workers as a set by the second sum,
  which has each vertex's colour for its tag (the tags go by the order chunks are made
  in). On 13 October 2026: 57 meshes with two blocks dug, the same lines and the same set. Switch block physics off for it (`echo singleplayerphysics=false >>
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
