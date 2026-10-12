# What an instruction costs (October 2026)

Measurements for whoever next tries to make the tick faster, taken on the D3D11 harness's
machine as it is (eight targets, 256 cores, 384 texels of write cache, 8,192 instructions a
pass) on an RTX 5070 laptop card. None of what was tried on the way was kept: nothing gained.

## Measure with the card's clock high

The card runs at 1,455 MHz (state P4) for the first two to ten seconds of a load and at
2,835 MHz (P2) after, and the emulator's speed is the clock's: the same snapshot ran at
1.70M instructions a second and at 3.41M. A short benchmark after a pause measures the
governor. Warm the card first and watch `nvidia-smi --query-gpu=clocks.gr` while it runs;
compare two builds by running them in turn (A B A B), long runs, and expect two runs of one
build to differ by 2%. A cold boot is inside those first seconds: it takes twice as long on a
card that was idle as on one that was busy.

## Kinds of instruction

`programs/classes` (`--image classes`): loops of one kind each, a mark between them, timed
with `--stats-from P2 --until P3`. Bare metal, no paging. Nanoseconds an instruction, a pass's
own cost (below) taken off:

| | 8,192 a pass |
|---|---|
| register arithmetic (15 and a branch back) | 135 |
| a two-instruction loop within one instruction texel | 120 |
| loads from one page | 215 |
| stores to texels already in the write cache | 290 |
| a jump or a branch taken to another texel | about 235 |

Under Linux a load costs more than that: nine in ten pass the stored-addresses filter and
search the cache's tags (`--profile`: `ram_read_bloom_pass`).

## A pass besides its instructions

Two instructions a pass (`--ticks 2`), milliseconds a pass:

| | |
|---|---|
| D3D11 as it is | 0.134 |
| without the GPU device | 0.102 |
| one target, one core (`--no-mrt`) | 0.087 |
| one target, one core, no GPU device | 0.067 |
| D3D12 | 0.088 |

A shell loop is 2.37 ms a pass of 8,192, so this is 5.6% of it, and a seventh of a pass of
2,000 (a desktop's, a file being read). Instructions a second by pass length, the same loop:
2.81M at 2,048, 3.21M at 4,096, 3.45M at 8,192, 3.72M at 16,384, 3.93M at 32,768.

## One workload, the builds there are

A shell loop, passes of 8,192, long runs in turn:

| | Instructions a second |
|---|---|
| D3D11, eight targets, 256 cores (the default) | 3.43 to 3.48M |
| D3D11, eight targets, 16 cores | 3.52M |
| D3D11, one target, one core | 3.54M |
| D3D12, DXC's bytecode, one target, one core | 3.43M |
| D3D12, fxc2's bytecode (`RVC12_DXBC=1`), one target, one core | 3.67M |

The eight targets no longer make core 0 faster by themselves (they did while its state was
1,485 texels): what they cost is the pass's own 0.05 ms.

## Tried, and not kept

- The run's end looked for only after a store or a jump taken, in place of after every
  instruction: one branch fewer for two instructions in three, and slower (arithmetic 153 ns
  against 150; as a value returned from the step, much slower).
- The step without its debug word; the write cache's tags zeroed at a pass's start in place
  of an occupancy bit looked up at every load and store (stores 292 ns against 305 alone,
  nothing on Linux); a second-level TLB of 512 entries (its two arrays as one, so that the
  shader's arrays stay under the 4,096 elements the driver takes): nothing, each.
- Core 0's pixels drawn by a build of the pass without `CORES`, the workers' by the build
  that has them: the same state, and nothing.
- One buffer kept as the one the tick reads, each commit copied back into it (in case a
  texel read after the other buffer's turn were a slow one): nothing.
- The GPU device's first pass drawn only when a list is submitted: 0.008 ms a pass.

A pass at 8,192 and at 32,768 differs in its events too when timed with `--fixed-dt 0.004`:
that is 4 ms of guest time a pass whatever its length, so the kernel's timer interrupt (some
60 page walks and 65 general-path instructions each) comes four times as often an instruction
in the shorter pass. Real time has it a hundred times a second either way.
