# What an instruction costs (October 2026)

Measurements for whoever next tries to make the tick faster, taken on the D3D11 harness's
machine as it is (eight targets, 256 cores, 384 texels of write cache, 8,192 instructions a
pass) on an RTX 5070 laptop card. None of what was tried on the way was kept: nothing gained.

## Measure with the card's clock high

The card runs at 1,455 MHz (state P4) for the first two to ten seconds of a load and at
2,835 MHz (P2) after, and the emulator's speed is the clock's: the same snapshot ran at
1.70M instructions a second and at 3.41M. A short benchmark after a pause measures the
governor. Warm the card first and watch `nvidia-smi --query-gpu=clocks.gr` while it runs;
compare two builds by running them in turn (A B A B), long runs, from one and the same
snapshot (two snapshots of "the same" work differ by more than most changes are worth), and
expect two runs of one build to differ by half a percent. A cold boot is inside those first seconds: it takes twice as long on a
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

## Bytecode instructions an emulated one costs

Measured, by running the bytecode: `tools/dxbc_run.py TICK.asm SNAPSHOT CONSTANTS` is an
interpreter of the tick's pixel shader as `dxbc_disasm` prints it (`docs/linux.md`). It runs
one pixel of one pass (the pixel whose texels have the instruction clock) from a snapshot
taken before the pass, with the pass's constant buffer (`RVC_DUMP_CB=FILE` while the harness
runs it), and counts every instruction executed. Its eight output texels are the harness's
after the same pass, to the word (the commit's count, which the commit adds, aside): that is
the check that what it counts is what ran.

Before and after the fast step was rewritten for it (October 2026), the same four passes of
8,192 emulated instructions each, both builds giving the same texels:

| The pass | Bytecode instructions, before | After | For an emulated one | Emulated for a bytecode one |
|---|---|---|---|---|
| a shell loop, pass 40 | 1,318,187 | 1,183,065 | 160.9 to 144.4 | 11.4% more |
| a shell loop, pass 200 | 1,298,979 | 1,163,140 | 158.6 to 142.0 | 11.7% |
| Doom's demo, pass 40 | 1,236,392 | 1,106,049 | 150.9 to 135.0 | 11.8% |
| Doom's demo, pass 200 | 1,215,510 | 1,077,099 | 148.4 to 131.5 | 12.9% |
| all four | | | 154.7 to 138.2 | 11.9% |

(About nine of those are the pass's own beginning and end, not the fast loop's.)

`tools/tick_path.py TICK.asm [TICK2.asm]` is the same count without running anything: the
fast loop walked along the path each kind of instruction takes, weighed as a shell loop runs
them (`--profile` has the shares: `x_alu`, `x_load`, `x_store`, `x_branch`, `x_jump`,
`x_refetch`). It says where a change takes instructions out, by kind: arithmetic 112 to 89,
a load from the write cache 160 to 146, a store to a texel in the cache 173 to 162, a branch
92 to 82, a jump 106 to 97 (and 27 more for any of them when the next instruction texel
has to be taken up).

In time the gain is far less, the driver having done most of it already. Both builds on one
and the same snapshot, in turn, the card's clock high, the same state hash at the end: a
shell loop 3.908M to 3.983M instructions a second (1.9%), Doom's demo 3.676M to 3.740M
(1.7%), and of that 1.3% is the harness no longer drawing or copying anything of the
workers' while the machine has none. On WARP (the shader as x86 code): 1.6%.

(Figures of 4 to 7% in this change's first commit messages were taken with a snapshot made
by each build for itself: the two were then at different places in the guest's work, which
is worth more than the change. One snapshot for both builds, always.)

What was taken out, all of it on the path of every instruction or of every arithmetic one:

- whether an arithmetic encoding exists was 17 instructions of comparisons; it is a bit of a
  small table chosen by funct7, shifted by funct3 (8);
- a sign is carried by an arithmetic shift (`asint(w) >> 20`, one instruction) where a test,
  a select and a mask were four: the immediates, `sra`, a load's byte or half;
- a signed comparison is the unsigned one with both signs turned over;
- which funct3 is a load is a bit of a constant, and "in RAM" one comparison after the
  address's top bit is turned over;
- the two low bits of an instruction are in each kind's own test, not tested for all;
- the loop's end is one comparison with a budget that a store which filled the write cache
  sets to nothing, where it was the budget or the stall;
- the fetched word is kept for the general path only when the step declined it.

And what the driver does not forgive, each tried by itself on the same path: the "next
instruction is in this texel" flag worked out by each kind in place of once at the end
(3.6% slower, though three instructions fewer), register 0 turned into the scratch register
at decode (2% slower), the old four-instruction immediate (3% slower than the shift). A
count is a guide; only the two builds run in turn say what a change is worth.

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
