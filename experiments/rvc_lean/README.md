# The machine's processor, a shader a draw (an experiment)

`experiments/rvc_opt`'s processor written again, for Linux without an MMU only, in the shape of
the compute shader `experiments/rv32_min/rv32_min_fast.hlsl`: every draw of it one file with no
includes.

- `tick.hlsl` (818 lines): the tick, every core of the machine. Its vertex, geometry and fragment
  shaders, and with `TICK_UNPACK` the pass that copies its eight targets into the state.
- `commit.hlsl` (345 lines): the commit. The state rows, the bands of RAM something other than a
  core changed (the GPU device's picture copied back, its control words, the host's zap), the
  cores' stores as points (`COMMIT_POINTS`, `commit_geom`), and the copy of those into the other
  buffer (`COMMIT_COPY`); at `_Init` RAM from the image.
- `main.shader`: the ShaderLab wrapper, a pass for each. `HLSLPROGRAM`, not `CGPROGRAM`: the
  harness, as Unity does, puts `HLSLSupport.cginc` and `UnityShaderVariables.cginc` in front of
  a `CGPROGRAM`, and those declare what the tick declares itself (`_Time`).

The GPU device and the sound card are `experiments/rvc_opt`'s (`gpu.shader`, `sound.shader`): the
harness takes them from there when the processor's folder has none.

    bin\rvc_harness.exe --rvc experiments\rvc_lean --image linux-net

The harness treats a folder whose name begins with `rvc_lean` as it does `rvc_opt`
(`isOpt`): the no-MMU machine's defines (`NO_PAGING`, `SBI_HLE`, `FPU`, `CORES=256`, `TICK_MRT`,
`COMMIT_BANDS`, `GPU_DEVICE`), so a copy of the folder elsewhere (`rvc_lean_try`) runs as well.

## What it is

The state texture is the machine's (docs/machine/multicore.md): RAM from row 64, core 0's block
and the workers' strip in rows 0-63, the worker geometry geotune lays out, the write cache of
three tables of buckets of three texels, the mailboxes of `src/mc.h`. A core's texels are where
`rvc_opt` has them (`tick.hlsl`, the comment at `L1_AT`), so the control pass, the host's reads
and the GPU device's passes are as they were.

What is not there: the MMU and its TLBs, the UART, the CSR cache and table (the CSRs Linux uses
are fields), `LAZY_MEMOP`, profiling and the experiments' switches (`X_P_*`, `TICK_LOAD`, `ZB`), the
tick drawn without eight targets, and upstream's switches of instructions: the general step is
one function of tests in the order of how common a kind is (`step`).

The tick:

- A fast loop (`fast`) for what cannot trap or change what interrupts see: the kinds told apart by
  `rvc_opt`'s tree of tests (arithmetic, loads, stores and floats one group, in it arithmetic and loads
  one; then branches, the jumps and `lui`/`auipc`, M), the arithmetic every result made and one chosen, one indexed register write
  (`xr[65]`: x0-x31, f0-f31 as bits, a slot for writes that must not land). Every float of opcode
  0x53 is the loop's (a sum, a difference, a product by themselves, `fp_light`'s signs, the
  lesser and greater, comparisons and moves, `fp_all` for the rest).
- The fetch from the texture (never the write cache), two texels at a time, and after an
  instruction that stayed in its texel the next as the texel turned (no texel compared, no word
  chosen).
- Loads and stores within one word of RAM in the loop, through one function each (`ram_word`,
  `ram_store`); the write cache searched only when the bloom word and a bit a texel (`has`) say it
  may hold the texel, and the two texels found or made last remembered.
- The general step (`step`): any instruction, the loads put together from two words, stores a
  byte at a time when not one whole word, the CSRs, SBI, `sret`, the traps and interrupts, and a
  worker's fault record.

## Measured

On the RTX 5070 laptop, 19 October 2026, D3D11 and fxc2, the four loads of
`experiments/rv32_min/README.md` ("Measured"): fps or ms, and millions of instructions a second
over the window each measures (all cores, core 0, the workers).

Bytecode (fxc2):

| | `rvc_opt` | `rvc_lean` |
|---|---|---|
| the tick's fragment shader | 301,848 bytes | about 107,000 |
| the commit's (the bands; the points) | 394,892 | 21,640; 32,992 |

Checked: Linux to its shell, geotune's layout of 255 workers, `fptest` 0 of 15 wrong, `mctest
255 30000` PASS with 0 pages brought, Doom's demo1 told and checked (0 of 5,000 views not as told,
digest `0ab4df93`), nxray's pictures 1-7 the tick's sums.

How it was made faster, one change at a time, two rounds each where they were close (Quake's
timedemo; nxray, 60 pictures, ms a picture):

| | Quake | nxray |
|---|---|---|
| as first written | 15.03 ms | 64.2 |
| the cache's three tables unrolled | 15.20 | 64.2 |
| the fetch's next word as the texel turned | 13.90 | 61.2 |
| every float the loop's | 13.50 | 49.2 |
| the kinds as `rvc_opt`'s tree | 13.10 | 46.8 |
| `rvc_opt` | 12.29 | 45.5 |

Tried and not kept, each on the same two loads: two instructions a turn of the loop (a function
called twice: nxray 47.5 to 49.0 ms), a whole word into a texel the cache has made without
reading it and a texel looked for and not there remembered (the same to the noise), M and the
floats that are not the loop's own in an outer loop as the compute shader has them (nxray 47.3 to
51.5: a division or a root then leaves the inner loop and comes back), and a smaller cache for
core 0 (`CORE0_BITS`, 5: the same; 4: Quake 7% slower, the cache full and the pass over sooner).
Those measured on one card: how large a loop, an array or a cache may be before it costs is the
card's, so where two shapes are equal here the smaller is kept.

And after those, each against the tick as it is, in turn (`factor 999999999989` and `gzip -c
/bin/busybox` twice each by the guest's `time`; Quake's timedemo; nxray's 60 pictures):

| | factor | gzip | Quake | nxray |
|---|---|---|---|---|
| the tick | 21.1 to 21.4 s | 107.7 to 108.7 s | 12.8 to 13.3 ms | 45.7 to 47.2 ms |
| arithmetic tested first by itself, then a group of the loads, stores and floats | 19.02 | 111.57 | 13.00 | 46.5 |
| arithmetic, loads and branches a test each, then stores or floats | 19.58 | 108.25 | 13.14 | 47.2 |
| a branch's or `jal`'s target texel read with the instruction, as the compute shader reads its entries | 25.23 | 122.07 | 15.11 | 52.6 |
| the second register read only when the instruction has one | 22.08 | 108.45 | 13.50 | 46.8 |

A loop of arithmetic gains from its own test, and nothing else does; one more texture read an
instruction costs more than the wait it saves. Nor does the frame's length: 16,384 instructions a
frame (`--ticks`), two rounds both always run and three with two (`--tick-rounds 3 --tick-split 2`)
left the machine at the same 8.0 to 8.4 million instructions a second in Quake (13.85, 12.95 and
13.37 ms); only nxray, whose first core waits for its workers, was faster (39.5, 39.2 and 28.4 ms).

## Rounds a frame (`--tick-rounds`, `--tick-split`)

`--tick-rounds N` draws the tick and its commit N times a frame before the devices' passes, the
clock a tick on for each (`_TimePass`) and the host's zap in the first only. `--tick-split F`: the
first F always run; a later one gives core 0 what is left of its F x `--ticks` (texel 39.y, its
instructions in this frame) and draws a worker only when core 0 ended the round before early
(39.z): what `rvc_harness --compute` does with its passes (`experiments/rv32_min/README.md`), with
no readback, the tick's geometry shader leaving the workers out. Off by default: four rounds with
one always run made nxray 2.4 times as fast (47.3 to 19.5 ms a picture) and Quake, Quake III and
Red Alert 5 to 15% slower, a round costing its draws whether anything runs in it or not.


All five in one session (19 October 2026), each load once, ms (a game's frame, nxray's picture) and
millions of instructions a second, all cores / core 0 / the workers, over the window measured:

| | `rvc_opt` | `rvc_lean` | `rvc_lean`, 4 rounds, 1 always | compute before | compute now |
|---|---|---|---|---|---|
| Quake, timedemo demo1 | 13.35 (8.3 / 3.2 / 5.0) | 14.20 (7.8 / 3.0 / 4.8) | 15.75 (6.9 / 2.8 / 4.2) | 9.20 (13.4 / 4.9 / 8.5) | 7.33 (16.0 / 6.2 / 9.8) |
| Quake III, timedemo q3dm17 | 14.66 (12.0 / 1.7 / 10.2) | 15.38 (10.9 / 1.6 / 9.3) | 15.28 (11.1 / 1.6 / 9.5) | 9.81 (17.2 / 2.5 / 14.7) | 8.12 (20.7 / 2.9 / 17.8) |
| Red Alert, a battle, frames 500-900 | 24.55 (6.5 / 4.3 / 2.2) | 25.15 (6.4 / 4.2 / 2.2) | 27.62 (5.7 / 3.8 / 1.9) | 13.74 (12.9 / 7.5 / 5.4) | 12.69 (13.6 / 8.1 / 5.6) |
| nxray, 60 pictures | 45.3 (183 / 4.5 / 178) | 47.0 (177 / 4.5 / 172) | 20.2 (150 / 3.9 / 146) | 29.9 (296 / 7.2 / 288) | 11.5 (354 / 8.7 / 345) |

(compute now's nxray from a run of the day before that did not meet the compute machine's start
race: experiments/rv32_min/README.md, "Cores".) `rvc_lean` is `rvc_opt`'s speed to 2 to 6% in this
round (in the A/B rounds before it, Quake 12.6 to 13.1 against 12.3, Red Alert's 300 frames 19.9
against 20.5), with a third of the tick's bytecode and a fifteenth of the commit's. The compute
machine is 1.8 to 2 times either on what core 0 does: what it has that a fragment shader cannot
is RAM read and written in place (no write cache, no commit), its registers in group shared
memory, and decoded instructions kept from pass to pass.