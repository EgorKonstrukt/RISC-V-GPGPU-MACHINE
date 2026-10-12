# Rigid boxes on every core

`nxphys` (`linux/physics`; Start menu: Other, "Physics (every core)") is a room of rigid
boxes: a wall of bricks with a heavy box thrown at it, towers, a heap let fall. Every worker
core (`multicore.md`) moves its share of the boxes, and nothing in it needs a lock: the
machine has none to give.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/physics/build.sh     (7 s; after Nano-X's build)
    python tools\make_linux_image.py
    / # nano-X -p & nxphys

    arrows  turn the view          space  throw the heavy box      1 2 3  a wall, towers, a heap
    r  the scene again             w  each box in its core's colour     p  stop and go
    a  all the workers             0  core 0 alone                      q, Escape  leave

The bar under the picture has a button for each of those but the arrows, and a line of
figures. In the picture the pointer takes a box by where it is pressed and carries it; pressed
where no box is, it turns the view. The wheel (or `+` and `-`) brings the eye nearer or takes
it further, and a held box instead while one is held. The window may be any size: the
picture has all of it but the bar, and the buttons share the bar's width.

## What the workers allow

A core sees another's stores one pass later, and 16 bytes have one writer a pass. So a
solver that applies a pair's push to both bodies, or goes through the contacts one after
another, or sorts bodies into shared bins, cannot be shared out. What can is anything
written as a gather: a core reads what the last phase left and writes only what is its own.

And every stage that must follow another costs a pass, whatever it computes, because that is
how long a core waits to see another's stores. So a step is made of as few stages as will do:
one.

## Who owns what

- **Body i is share `i mod n`'s**, and grid cell c is share `c mod n`'s, n being the number
  of shares (one a worker). Every record is whole 16 bytes (`physics.h`).
- **A body's state** (place, turn, velocity, spin: four texels) is in two buffers: a substep
  reads one and writes the other.
- **What its neighbours need of it** (`struct body_pose`, six texels in each buffer): its axes,
  and where `pose_guess` takes it to be next. Its owner leaves them with the state and sets
  `PHYS_POSED` in the state's key; a neighbour reads them there, where it worked them out
  again for every contact (a tenth of a substep). A state the program writes itself has the
  bit clear and is worked out as before.
- **A pair's contact** (`struct manifold`) is in both bodies' lists, each written by its own
  body. The points and the normal are the same bits in both, because the pair is always
  worked out with the lower-numbered body first.
- **The grid** is cells of 2 m with room for 39 bodies each.

## A substep

One phase, a sixtieth of a second of the boxes' time. For each of its bodies a share:

1. **Looks for contacts**, every second substep (`look`): the 27 cells round the body, a
   sphere test, then box against box: the axis of least overlap (a face of either, or the
   cross of an edge of each), and for a face the other box's facing side cut to the face's
   outline, up to four points. Two slow bodies keep the contact they have, seven looks of
   eight: finding it again was most of a look.
2. **Moves the body as it would go**, and takes every other body to be where `pose_guess`
   says.
3. **Takes its part of every contact** (`settle_points`): its mass's part of what the pair
   has to undo. The other body takes the rest for itself, from the same numbers.
4. **Takes its velocity from where it ended.**

The last substep before a look also leaves the grid for it (`phys_grid`): each share scans
every body's cell number and keeps the bodies of its own cells. That is a counting sort made
as a gather. A radix sort is three stages (histogram, prefix, scatter).

A share begins substep p when every share has published p-1: it reads the others' counters
and spins until they are there (a worker's `pause` is a sleep on its job word). One pass of
waiting, and no core is the conductor.

## What a body does about its contacts, and why

Each line here is there because the thing without it was measured to fail.

- **All its points at once, then as much of the sum as serves them best.** Each point says
  what push would settle it alone; the pushes are added, tried, and scaled by a least-squares
  fit to what the points wanted. The order of the points is then nothing. Gone through one
  after another, the first point always got the most and a resting stack walked sideways.
- **Friction the same way.** Four corners that each stop a spin alone stop it three times
  when added: a box set down on the floor span itself into the air.
- **The push a point had is kept and given again first** (`a[3]` of the list's own copy; the
  floor's in `body_info`). Without it a stack sinks: a substep only moves a load one layer.
- **A point that has come apart has no push any more.** Carried into the air it threw boxes.
- **What a substep brought a body into is its velocity; what it was in already only moves
  it** (`speed` and `cur`). Otherwise getting out of a wall is a kick.
- **A slow neighbour is taken to stay where it is** (`pose_guess`). Guessed on by its last
  move, two resting bodies each answer the other's answer, and a wall of bricks shook itself
  apart; a tower, where every push is straight down, never showed it. A fast neighbour goes
  on as it went, or a thrown box would give the brick it hits no speed.

Tried and not kept:

- **A body's mass shared among its contact points.** Each corner then mostly turns its share
  of the body, and four corners undo a quarter of what they should.
- **The lower of two bodies counting as the heavier.** A stack stands, and a brick that
  hangs over lifts the bricks on it: no load goes down.
- **A body's inverse inertia turned into the room once a pose**, so that a push's effect
  needs no turning into the body's frame and back: 1.6% fewer instructions, another sum, and
  the still wall's fastest brick at 3 mm a second where it was 2.
- **One job a substep a worker.** Giving 255 jobs out and waiting for them is some six passes
  of core 0; the workers have one job each for as long as the boxes move.

What it is not: a solver that converges. A load takes a substep a layer to arrive, so a tall
stack sinks a few millimetres before it holds, and a body resting on another does not press
on it as it should. Piles, walls and things knocked over look right; a see-saw would not.

## The bar, and a box in the hand

The window is a plain Nano-X window with the picture a window inside it, as the video
player's is: the bar is `ui.h`'s buttons (`linux/apps`), drawn by the window system, and the
GPU's picture never meets it. A button does what its key does (`act`).

- **Which box.** A press in the picture is a ray from the eye through that pixel
  (`ray`, from the view's own three directions), tried against every box in the box's own
  frame (`phys_pick`): three pairs of sides, on core 0, once a press.
- **Carrying it.** Core 0 writes which box, the point on it in the box's frame and where
  that point is wanted (`grab`, `grab_at`, `grab_to` of `struct phys`, which the workers only
  read). The box's own share moves that point a quarter of the way there each substep, 30 cm
  at most, before the contacts are settled: so a held box pushes what it is carried into and
  swings from where it was taken. Nothing but that share writes the box.
- **Where the pointer is** comes from the machine's words while the button is down
  (`docs/input.md`), not from events: the press says where the window is on the display.
  The wanted place is the pointer's ray at the distance ahead of the eye the box was taken at.
  The press itself is where the pointer is when the event is read, in a window looked up
  then (`ui_window_origin`): by the event's own place the box was taken beside the hand.
- **No waiting for the window system or for a substep.** Events are asked for when the
  machine's button or key words change and for a tenth of a second after (the event is on
  its way), else seven times a second; asked for ten times a second, a press was that late.
  A view that moved is drawn at once from the picture shown last (`show_picture(1)`): the
  shares are asked nothing, so turning does not wait for the boxes, and works while they stand.
- **Nothing is said on the console as it runs** unless `PHYS_SAY=1`: a line there is a tenth
  of a second of core 0, and one a second was a stutter a second.

## The picture

When core 0 asks (25 times a second at most) each share writes its boxes as quads straight
into GPU memory, 24 compact float vertices a box, at the first substep at or after the one
named: the picture is of one state. A face's colour is a texel of a table of 16 colours by
64 shades, and the worker picks the shade from the face's normal. Two buffers alternate, one
drawn while the other is made. Core 0 draws them with one command that stays (`seglKeptQuads`,
`docs/gpu.md`) and never touches a vertex. It is 3D with `seglWorldUp(2)`, so a holodeck
shows it.

## What it costs

Instructions a box (`PHYS_COUNT=1`, on `rvc_cpu`):

| | a substep | a substep with a look |
|---|---|---|
| a tower's cube, 8 points | 5 thousand | 12 thousand |
| a wall's brick, 16 points | 9 thousand | 17 to 18 thousand |

Where a wall's instructions go (a line profile of `physics.c` over an `rvc_cpu --pc-log`):
finding and making the contacts a quarter, building the points from them a quarter,
`settle_points` a fifth, the picture's quads 6%. A worker's passes are full ones (95% of
8,192 instructions on the shader machine): the stores of a substep fit its 3 KB a pass.

A substep takes as many passes as its longest share and one more, so with a box a worker the
slowest box sets the pace: some four passes a substep.

On the shader machine (D3D11, fxc2, RTX 5090, 255 workers), with nothing else on the card and
with another program taking 37 to 53% of it:

| | substeps a second | of real time | with the card shared |
|---|---|---|---|
| the wall, 206 boxes | 60 to 62 | all of it (it goes no faster) | 40 to 45, 0.7 |
| the heap, 400 boxes | 47 to 56 | 0.8 to 0.9 | 27 to 35, 0.5 |
| a heap of 1,000 as it lands | 11 to 18 | 0.2 to 0.3 | |

Forty-eight substeps of a wall of two rows (42 boxes): 16.6 s on core 0 alone, 2.2 s with 7
workers, 0.63 s with 255, the same sum each time. It runs no faster than time goes: core 0
lets the shares go a substep on for every sixtieth of a second.

### A fifth fewer (12 October 2026)

A wall's substep on the first core alone went from 2,903 thousand instructions to 2,320, and
with seven workers the workers' from 2,895 to 2,303; a brick's substep with a look is 13.6
thousand where it was 17.5, without one 7.8 where it was 8.7. To the first substep: 4.4
million instructions became 1.4 with 7 workers (2.3 with 255, of which 0.9 is `mcw_open`).

- **The look** first collects the bodies within reach (too far along x alone is too far; a
  neighbour's margin and radius together are in its pose, `reach`), then goes through those;
  the old list is left as it is until the new one is known. `struct body_pose` is eight
  texels, two of them nothing, so that a neighbour's is found by a shift.
- **A point that is clearly apart is left without working it out**: its depth is first
  guessed from the bodies' own frames (the gap between the middles along the normal, the
  point's `b[3]`, and the other point along the normal in its body's frame), which is the
  real depth to within rounding; `SURE` and a hundred-thousandth of the bodies' places and
  sizes is some ten times that. Three points of five are apart (the bricks beside and the
  one above). It assumes a state's `q` is a unit quaternion, as the rest does.
- `box_box` tests a row at a time and leaves early for boxes apart; `clip` cuts both planes
  of an axis in one call; `phys_quads` shares corner sums between the faces across an axis.
- **Nothing is cleared at the start**: what `mcw_alloc()` gives is zeros, a page is there
  for the workers by one store (`there()`), the bodies' arrays only as far as the scene's
  bodies (`bodies_there()`): 6.5 MB of `memset` gone.
- Built `-O3 -fsched-pressure`: 5% fewer than `-O2`. A function whose frame passes 2 KB pays
  two or three instructions for every local it touches (`look` with its list of manifolds
  as a local array): the list is on `alloca`.

- **The first core has a share of the bodies only with 7 workers or fewer** (`OWN_UP_TO`;
  `PHYS_OWN=0/1` says otherwise). Share 0 is then core 0's, run in its own loop by the same
  `share_advance()` as a worker's job, and worker k has share k; with more workers the
  shares are the workers', worker k with share k - 1. Every share waits for core 0's
  counter when it has one, and core 0 has the kernel's clock and the window to see to:
  passes for 119 substeps of the wall, with the share against without, were 8,790 for
  10,596 with 3 workers, 4,437 for 4,780 with 7, 2,346 for 2,278 with 15 and 356 for 342
  with 255. On the shader machine with 255 workers core 0 therefore runs half of its
  passes' instructions (it only waits; 57% of its passes end in its loop's `pause`); with a
  share it ran 76% of them and the wall took 4% more passes.
- **The frame ends with `seglSwapEarly()`**: the list reads one of the two quad buffers, the
  colour table (written once) and the set's own matrices, and `ask_picture()` settles
  (`seglSettle()`) before the shares write a quad buffer again. Core 0's loop no longer
  looks at every share each turn (`first_own`, `shown_so_far`), and `PHYS_COUNT=1` also
  says how many passes the first core ended with nothing to do.

Tried and not kept: `settle_points` out of line (3% more); the points built in two loops,
one for each pair of matrices (no fewer); the cuts' distances carried into the choice of
points (more, for the keeping). Still there: the workers spin for the other shares'
counters (5% of their instructions with seven, most of them with 255).

More sums a change must leave, `PHYS_WORKERS=0`: `PHYS_SCENE=4 PHYS_BODIES=13 PHYS_STEPS=300`
`e373ce02`, `PHYS_SCENE=3 PHYS_BODIES=150 PHYS_STEPS=90` `a1b597fa`, `PHYS_SCENE=2
PHYS_STEPS=60` `d15836d6`, the still wall `3a16b24c`; and `PHYS_QSUM=1` (with `PHYS_STEPS`)
prints a sum of the last picture's quads, `bc7f3663` for the wall.

## Switches, for tests

`PHYS_STEPS=N` ends after N substeps with a `physstat: done` line (the boxes' highest, the
deepest corner in the floor or a wall, the fastest box, a sum of every box's state);
`PHYS_HOLD=1` stays there and says `nxphys: holding`. `PHYS_SCENE=N` (4: one tower),
`PHYS_BODIES=N` (the heap's and the tower's), `PHYS_ROWS=N` (the wall's), `PHYS_STILL=1`
(nothing thrown), `PHYS_WORKERS=N`, `PHYS_SHARES=N` (more shares than workers: core 0 has
the rest), `PHYS_OWNERS=1` (the `w` key), `PHYS_SIZE=WxH` (the picture's size at the start), `PHYS_SAY=1` (a
`physstat:` line a second, and a line for a box held or let go), `PHYS_PICK=X,Y` (the box at that pixel held from the start, its
point wanted three metres up), `PHYS_PRINT=1` (every box's place and velocity),
`PHYS_COUNT=1` (instructions a box; with `PHYS_WORKERS=0`).

## Checking it

- **Whoever computes it, the same.** `PHYS_SCENE=1 PHYS_STEPS=120 nxphys` prints one sum
  with `PHYS_WORKERS=0`, with `PHYS_WORKERS=7` and with `PHYS_WORKERS=0 PHYS_SHARES=255`
  (`e444ba29` on `rvc_cpu`; the shader machine's differs, its divisions being the card's,
  and is one sum there too).
- **What stands, stands.** `PHYS_SCENE=1 PHYS_STILL=1 PHYS_STEPS=120 PHYS_PRINT=1 nxphys`:
  no brick more than a centimetre from where the first substep left it, none over a
  millimetre a second. `PHYS_SCENE=4 PHYS_BODIES=13 PHYS_STEPS=300 nxphys`: highest 6900 mm.
- **What is thrown, hits.** `PHYS_SCENE=1 PHYS_STEPS=180 PHYS_PRINT=1 nxphys`: several bricks
  over a metre out of the wall's plane (z), none in the floor (`deepest` a few millimetres).
- **The workers afterwards.** `nxphys & sleep 5; killall -9 nxphys; sleep 1; mctest 15 2000`
  says PASS; `nxphys & sleep 6; nxkey q; sleep 2; pidof nxphys` finds none.
- **The bar and the hand.** `PHYS_SCENE=1 PHYS_STILL=1 PHYS_PICK=350,222 PHYS_STEPS=120 nxphys`
  says `box 117 held` and at its end that the held point is some millimetres from where it is
  wanted; `PHYS_PICK=20,20` holds none. `PHYS_SAY=1 nxphys & sleep 6; nxkey 181,462` (the Towers button,
  the window where the window system first puts it) and the next `physstat:` line has 109
  boxes. The whole way from the pointer: `rvc_cpu --pointer-sweep 0 --pointer-circle
  400,300,12 --pointer-button 1,6,20` with `PHYS_SAY=1 PHYS_STILL=1 nxphys` says a box is held,
  then let go. On the shader machine, the screen of a snapshot (`tools\snapshot_screen.py`):
  `nxkey - - - -` leaves the boxes 0.018 of the picture where they were 0.046, and
  `nxkey drag:684,501:560,420` (the frame's corner) leaves a picture of 516 x 319 with the bar
  under it and seven buttons 72 apart.

A change to the solver is tried on a wall and on a tower both: every bug in it so far showed
in one and not the other.

## Workers that stopped at pages (12 October 2026)

With 255 workers every worker came, in the first substep, to pages of the program's own code
that no core had run yet, and each stopped there until the first core had read the page: 921
stops, and a third stack page each on top. `take_workers` now reads the program's text before
the jobs go out and the library has three stack pages there for a worker (`STACK_TOUCHED`).
`PHYS_COUNT=1` prints the pages brought for workers: 1,126 before, none now. The first
substep went from 92 ms to 41, a substep from 7.5 ms to 6.2, 120 substeps from 1,061 ms to
843; the sums are the same.

A worker whose share is waiting for the others cannot end its pass (the pause hint is a sleep
until its job word changes), so the instructions of all cores are the cores times the passes
whatever the work: 6.35 million a substep before, 5.26 now, of which 2.2 are work
(`PHYS_WORKERS=0`: 2.83 to 2.18). Fewer shares than workers with the same fullest share were
tried and are slower (9.1 ms a substep against 6.1 on the wall).

It asks for 207 workers, not all 255 (`WORKERS_ASKED`; `a` at its window and `PHYS_WORKERS`
give any number): with 207 a substep is as long as with 255 in every scene (the wall 6.2 ms
against 6.1, the towers 3.7 against 3.8, the heap 7.4 against 7.6; 191 is slower), and 48
cores are not turning in place. All cores a substep, the morning's build against this one:
the wall 6.35 million to 4.22, the towers 4.12 to 2.46, the heap 8.72 to 5.09; the start
59.3 million to 38.9, 50.7 to 37.9, 50.3 to 39.9.
