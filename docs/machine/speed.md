# Every machine's speed (19 October 2026)

The same loads on every processor the harness has, in one session: an RTX 5070 laptop (D3D11,
fxc2) and its i7-13650HX. Each load was run once on each machine in turn, so the cards' drift
from hour to hour falls on all of them alike; a number moves a few percent from one session to
the next (compare `experiments/rv32_min/README.md`, "Measured", the same loads a day before).

| | runs as | what it is |
|---|---|---|
| CPU | `rvc_harness --cpu --image linux-net` | the machine on the host's processor (`cpu-harness.md`), real time |
| fragment | `--rvc experiments\rvc_opt --image linux-net` | the shader machine: the tick and the commit as fragment shaders, what the world runs |
| fragment, lean | `--rvc experiments\rvc_lean --image linux-net` | the same written again, a shader a draw (`experiments/rvc_lean/README.md`) |
| compute | `--compute fast` | the processor as a compute shader, 255 workers (`experiments/rv32_min/README.md`) |
| compute, plain | `--compute plain` | `rv32_min.hlsl`: core 0 alone, no fast path |
| old rvc | `--d3d11 --payload rvc\_Nix\rvc\data-net` | upstream rvc's shader as it was, one core, upstream's Linux with the MMU |

Every one but the old rvc runs our Linux without the MMU (`nommu.md`), the same image, the
programs as built; the old rvc has upstream's kernel and busybox, so only the shell's loads are
its. Each line through `tools\gpu_queue.ps1`, the shell's line sent at its prompt (`--expect`,
`--send`), `--stats 1`; the machine's defaults otherwise (8,192 instructions a pass, 16,384 a frame for
compute).

## The shell

Seconds by the guest's clock (`/proc/uptime` at the prompt, `time`), and millions of
instructions a second over the same window: all cores (core 0; the workers' are the rest).

| | CPU | fragment | fragment, lean | compute | compute, plain | old rvc |
|---|---|---|---|---|---|---|
| Linux to its prompt | 1.1 s | 5.0 s (201 / 3.5) | 5.8 s (185 / 3.0) | 1.6 s (4.1) | 2.0 s (2.6) | 107.6 s (0.31) |
| `time factor 999999999989`, twice | 0.70 s (111) | 21.45 s (7.05) | 21.26 s (7.18) | 12.52 s (12.03) | 34.73 s (4.48) | 51.85 s (0.35) |
| `time sh -c 'seq 1 20000 \| gzip -c >/dev/null'`, twice | 1.25 s (48.7) | 22.54 s (5.35) | 25.20 s (4.81) | 13.37 s (8.75) | 31.38 s (3.88) | 448.9 s, once (0.33) |

(The shader machine's boot has geotune's passes in it, which lay out the workers on all of them:
the 200 million a second of its workers. The compute machine has its geometry from the host.)

## Programs

A game's frame in ms of guest time and frames a second, nxray's picture; then the millions of
instructions a second over the window the game measures, all cores / core 0 / the workers.

| | CPU | fragment | fragment, lean | compute | compute, plain |
|---|---|---|---|---|---|
| Quake, `quake +timedemo demo1` | 6.84 ms, 146.1 fps (24.2 / 12.5 / 11.7) | 11.71 ms, 85.4 (9.3 / 3.7 / 5.7) | 13.07 ms, 76.5 (8.5 / 3.3 / 5.2) | 7.96 ms, 125.7 (15.7 / 5.6 / 10.1) | 36.23 ms, 27.6 (3.4) |
| Quake III, `+set timedemo 1 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3`, frames 300-500 | 6.39 ms, 156.5 (27.2 / 4.0 / 23.2) | 13.06 ms, 76.6 (12.9 / 1.9 / 11.0) | 14.16 ms, 70.6 (11.8 / 1.7 / 10.1) | 8.86 ms, 112.9 (20.1 / 3.2 / 16.9) | 57.93 ms, 17.3 (3.4) |
| Red Alert, `TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_HUNT=50 RALERT_FRAMES=900 ralert`, frames 500-900 | 6.14 ms, 162.8 (76.6 / 60.0 / 16.6) | 21.68 ms, 46.1 (7.2 / 4.8 / 2.4) | 24.39 ms, 41.0 (6.7 / 4.4 / 2.3) | 14.09 ms, 71.0 (12.4 / 7.4 / 5.0) | 40.52 ms, 24.7 (3.7) |
| nxray, `RAY_FRAMES=60 nxray` | 60.2 ms, 16.6 a second (267 / 7.3 / 260) | 44.2 ms, 22.6 (183 / 4.6 / 178) | 46.1 ms, 21.7 (180 / 4.5 / 175) | 12.2 ms, 81.7 (337 / 8.2 / 329) | 6,567 ms, 0.15 (3.3) |
| Doom, `DOOM_VIEW=told doom -1 -timedemo demo1` (5,026 tics) | 3.39 ms, 295.2 fps | 16.67 ms, 60.0 | 18.69 ms, 53.5 | 9.90 ms, 101.0 | 21.81 ms, 45.8 |

Every program under Nano-X (`nano-X -p & sleep 2;` first). Doom's from its realtics, as `-timedemo`
prints them (the counters' window there falls after the demo). The compute machine's first nxray run
stopped a worker at its start (`experiments/rv32_min/README.md`, "Cores"); its number is from the next.
The CPU column is the host's processor, not a card: core 0 there is 10 to 60 million a second.

What they say:

- The compute machine is 1.5 to 1.7 times the fragment one on the games and on the shell, and 3.6
  times on nxray, whose core 0 waits for its workers: its passes go on while core 0 rests.
- `rvc_lean` is `rvc_opt`'s speed to 13% (`factor` level, Quake, Red Alert, Doom and `gzip` 8 to 13%
  slower, nxray 4%), with two fifths of its tick's bytecode and a sixth of its commit's.
- The plain compute shader, one core and no fast path, is 2.3 to 2.8 times slower than the fast one
  on the shell and 2.2 to 6.5 times on the games, which lose their workers (nxray 540 times).
- Upstream's shader runs its own Linux at 0.31 to 0.35 million instructions a second: the fragment
  machine is 20 times that on one core, the compute one 35.

## Bytecode

The fxc2 bytecode of each draw and dispatch, as the harness compiled it (its log's `pass ... bytes`):

| | the processor | the commit |
|---|---|---|
| fragment (`rvc_opt`) | tick 301,848 (vertex 5,448, geometry 12,108) | 238,120, the points' 81,528 + 24,428, the copy's 29,452 + 46,756 |
| fragment, lean | tick 116,344 (vertex 484, geometry 9,100) | 22,596, the points' 33,052 + 18,228, the copy's 640 + 18,228 |
| compute | `rv32_min_fast.hlsl` 72,396 | the shader machine's commit, 60,424 |
| compute, plain | `rv32_min.hlsl` 35,536 | the same |
| old rvc | tick 279,276 | 22,920 |

The devices are the same on all but the old rvc: the GPU device's draw 77,476 and its control
88,032, the sound card's mix 46,764.
