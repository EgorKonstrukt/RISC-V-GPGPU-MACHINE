# Rays on every core

Two programs made for the machine's worker cores (`multicore.md`), and for the small ones
above all: a pixel of a traced picture is a few thousand instructions of float arithmetic
and a handful of bytes stored, so a core needs next to no write cache for it, and the
machine's geometry lets a program have fifteen such cores in place of three large ones.

Both work the same way. Core 0 traces nothing: it lays the cores out (`mcw_shape`), as many
workers of a size as the machine has room for (`mcw_fit`: 15 of the smaller sizes and 7 of
the largest on a machine of 16 cores, 63 and 27 on one of 64), hands a tile to every worker
that has none (`mcw_post`, `mcw_done`), and shows the picture as it fills. A tile is 32 x 32
pixels, or 16 x 16 when that would leave a worker fewer than four tiles of the picture: with
63 workers a picture of 320 x 240 in tiles of 32 is 80 tiles, hardly more than one each
(0.57 s; 0.48 s in tiles of 16), and in tiles of 16 where 32 would do, the handing out
costs (640 x 480: 1.76 s against 1.40 s). The sum is the same either way. The picture is words in GPU memory that the display draws where they
lie (`seglTexturePointer`), and a tile's rows begin and end on 16 bytes of it (a tile is 32
pixels of four bytes across, the picture's width a multiple of four), so no two cores store
to the same 16 bytes. Everything else a job writes is its own stack; the scene is only read.

## `nxray`: a ray tracer

`linux/raytrace` (`build.sh` after Nano-X's; Start menu: Other, "Rays (every core)"). Six
balls over a chequered ground, one light, shadows and three mirrorings, turning; 320 x 240.

    a   all the workers the geometry has      1   one worker      0   core 0 alone
    s   the next geometry (small, 3 large and the rest small, large, the smallest: as many as fit)
    w   each tile framed in its core's colour  space  stop and go   q, Escape  leave

`RAY_FRAMES=N` leaves after N pictures, `RAY_WORKERS=N` sets the start (every worker has its share of a picture in one job: `docs/multicore.md`, "A share, not a tile"),
`RAY_SIZE=WxH` the picture, `RAY_STILL=1` keeps the scene where it is. Each picture's line
(`raystat:`) has its time, its rays and a sum of its pixels.

One picture (201,460 rays) on the shader machine with 16 cores (it had `--cores 16` then), the same sum every time:

| The cores | A picture | Against core 0 alone |
|---|---|---|
| core 0 alone | 16.9 s | |
| 3 large | 7.8 s | 2.2 times |
| 7 large | 4.2 s | 4.0 |
| 12 of size 5 | 2.6 s | 6.5 |
| 15 small (size 4) | 2.2 s | 7.7 |
| 3 large and 12 small (the kernel's default) | 2.2 s | 7.7 |
| 15 of the smallest (size 3) | 1.3 to 1.5 s | 12 |

(Those are the tick in one target. In eight, each worker's pixels a block of its own, a
large worker costs no more than a small one: `multicore.md`, "Eight texels a pixel".)

On the machine's 64 cores, the program's own choices:

| | 16 cores | 64 cores |
|---|---|---|
| `nxray`, 320 x 240 | 1.21 s with 15 workers | 0.48 s with 63 |
| `nxray`, 640 x 480 (`RAY_SIZE`) | 4.72 s | 1.62 s (1.35 s with 63 of the smallest, `RAY_SHAPE`) |
| `nxpath`, 160 x 120, 8 samples | 11.4 s, 89 thousand rays a second | 6.9 s, 148 thousand |
| `nxpath`, 320 x 240, 4 samples | 18.7 s, 109 thousand | 9.4 s, 217 thousand |

The path tracer gains less: core 0 draws its panel seven times a second and hands the tiles
out in between.

## `nxpath`: a path tracer, with a panel

`linux/pathtrace` (`build.sh` after Dear ImGui's, `linux/imgui`; Start menu: Other, "Path
tracer (every core)"). A Cornell box: a red and a green wall, a lamp in the ceiling, a block
and two balls that are matt, a mirror or glass. `tracer.c` is the tracer, plain C, and its
job is one more sample of every pixel of a tile: a path from the eye that turns up to so
many times, the lamp's light on every matt surface it meets by a ray to a point of the lamp,
a path that carries little ended by chance. The samples' sums are three floats a pixel in
memory from `mcw_alloc`; the picture is made from them as each sample comes in, so it clears
while one watches. A sample is held to 2.5 times white: one path in thousands reaches the
lamp by a mirror or through glass and would be a white speck for good.

`nxpath.cpp` is the panel, Dear ImGui (`imgui.md`): the picture's size (160 x 120 to 640 x 480, which is 4.2 s a sample with 63 workers), samples a pixel,
turns of a path, the lamp, the two balls' kinds, which core traced what, and how the cores
are laid out (the smallest, small or large workers, as many as the machine has room for, or
this core alone); Render, Stop, how
far it is and how many rays a second. The panel is drawn seven times a second while a
picture is traced (it is some 200 thousand instructions of core 0) and the tiles are handed
out in between.

`NXPATH_AUTO=1` presses Render at the start and `NXPATH_EXIT=1` leaves when the picture is
done; `NXPATH_SAMPLES`, `NXPATH_BOUNCES`, `NXPATH_SIZE=WxH` and `NXPATH_CORES=N` (0 to 3, the
panel's four layouts) set the panel. A picture's line (`pathstat:`) has its time, rays and
sum.

160 x 120, 8 samples a pixel (1,023,465 rays), the same sum whichever cores:

| The cores | The picture | Rays a second |
|---|---|---|
| core 0 alone | 2 minutes (15.2 s a sample) | 8 thousand |
| 7 large | 32 s | 31 thousand |
| 15 small | 21 s | 48 thousand |
| 15 of the smallest | 13.6 s | 75 thousand, 9.4 times core 0 |

### Meshes

What stands in the box is the panel's choice ("in the box", `NXPATH_SCENE=N`): the two balls
and the block (0), or a model of triangles with one ball in the near corner: the Utah teapot
(1; 6,320 triangles), the Stanford rabbit at its third size (2; 3,851) or a torus knot made
by arithmetic (3; 5,040). The model's material is the first of the panel's two (matt, mirror
or glass; `NXPATH_FIRST=N`), the ball's the second.

- `linux/pathtrace/make_meshes.py` makes them at the build, on the host: it reads the teapot
  (an OBJ file of Stanford's CS148 course) and the rabbit (the scanning repository's
  `bun_zipper_res3.ply`), which `build.sh` fetches once, turns and sizes each to where it
  stands in the unit box, winds every triangle outward, gives every corner the mean normal
  of the faces round it, and writes `meshes.c`: a triangle as a corner, its two edges and
  three normals (18 floats, written as their bits), and a tree of boxes over them.
- The tree is split where the two halves' surfaces times their triangles are least (twelve
  places an axis tried), down to four triangles a leaf: 3,835 boxes for the teapot, 15 deep.
  A box is 32 bytes: its corners, and either where its triangles are and how many, or
  where its two boxes are.
- `tracer.c` goes down it (`mesh_nearest`): both of a box's two are tested, the nearer is
  gone into first and the farther kept on a stack of 64, and a kept box is tested again
  when it is taken, against what has been found since. A triangle is Moeller and
  Trumbore's test on the kept edges. A shadow's ray stops at the first triangle
  (`mesh_between`). A ray is 1.5 to 2 times what it is among the balls: 240 x 180 with 12
  samples is 1.7 s with the balls and 3.4 to 4.5 s with a model, 255 workers.
- The models are the program's own data (not `const`): a worker stops at a page nobody has
  touched, and `mcw_touch` of the data, which `nxpath` does when it takes its workers,
  brings them.
- To check the tree: `NXPATH_DEPTH=1 NXPATH_PRINT=1 NXPATH_SAMPLES=1 NXPATH_SCENE=N nxpath`
  prints how far every pixel's ray goes, and `python tools\pathtrace_reference.py meshes.c
  teapot LOG` tests every ray against every triangle with no tree: every ray that meets the
  model must have the guest's distance (1,638 for the teapot, 1,686 the rabbit, 1,698 the
  knot, at 160 x 120). `NXPATH_PRINT=1` alone prints the picture.
- The sums (`rvc_cpu`, 160 x 120, 2 samples): `84d4eafd` the balls, `40792470` the teapot,
  `46a882a3` the rabbit, `d29e8be5` the knot; with `NXPATH_CORES=3` the same. The shader
  machine's differ (its divisions and square roots are the card's).

## Checking them

A picture's sum does not depend on which cores traced it, or how many: run one with
`RAY_WORKERS=0` (or `NXPATH_CORES=3`) and one with all of them. `rvc_harness --cpu` runs both
at once the speed for looking at them (`cpu-harness.md`); the times above are the shader
machine's (`rvc_harness`).
