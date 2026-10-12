# What an instruction costs (October 2026)

Measurements for whoever next tries to make the tick faster, taken on the D3D11 harness's
machine as it is (eight targets, 256 cores, 384 texels of write cache, 8,192 instructions a
pass) on an RTX 5070 laptop card. Most of what was tried on the way gained nothing and was not
kept; what did gain is the first section.

## Where the time was

Against the machine as it was before all of this, both builds on one snapshot, in turn, the
card's clock high, the same instructions a pass: a shell loop 3.51M to 3.90M instructions a
second (11.2%), Doom's demo 3.15M to 3.52M (11.8%). None of it is from fewer operations in an
instruction's own arithmetic, which is where it was looked for first (below: 12% fewer bytecode
instructions were worth under 1%). It is from three things a pass did that it need not have:

- **The TLBs were empty at the start of every pass.** They are kept in the state's texels
  from pass to pass (`mmu.h`), and the tick's last loop, since it went to eight targets, wrote
  zeros there: `if (!have && !_Init && fp_state_texel(at, texel))` asks for the float
  registers' texel whether or not the TLB's was had already (`&&` runs both its sides), and
  what that function puts in its `out` argument for a texel that is not its own is zeros. So
  each pass walked the page table for every page it touched: 52 walks a pass of a shell loop,
  112 of Doom's demo, each an instruction on the general path (some 2.5 microseconds). Fixed:
  2.4 walks a pass, and 4.4% (a shell loop), 4.6% (Doom), 5.8% with passes of 2,048. Nothing
  showed it: a walk is not an emulated instruction, so every count and checksum was the same.
  (`--profile` now says why a page was walked: `w_empty` was every one of them.)
- **The write cache was searched for the texel it had just been searched for.** A search is
  a bucket's three tags read and compared, and the next table's when the bucket is full; a
  register save, a structure, a copy go to the same texel four times running. The two texels
  found or made last are remembered with their place, and so is the texel looked for last
  and not found (`types.h`, `L1_KNOWN`): of a shell loop's loads that pass the filter of
  stored addresses a third are answered by that, and 57% of its stores. 5% (a shell loop),
  3% (Doom).
- **Half the loads that searched found nothing.** The filter of stored addresses or-ed
  together lets nine loads in ten through. A bit for each texel the cache was given, by a
  hash of its number (1,024 bits, `L1_HAS`), stops nearly all of those: 1% on Doom.

With these a shell loop's loads search the cache 0.043 times an instruction where it was
0.226, its stores 0.062 where it was 0.144.

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

## What things cost

A way to time one thing: `RVC_FREEZE=1` in the environment and the D3D11 harness draws the
tick and nothing after it, so the state is never written back and every frame runs the same
pass (give it `--fixed-dt 0.000000001` for guest time to stand still too, or every frame
takes the timer's interrupt). `--bench` then prints `best_ms`, the fastest 1,024 frames'
time a frame, which two runs give within half a percent. Work added to every fast instruction
(`--define X_P_ALU=8`, `X_P_READ=7`, `X_P_TEX=1`: `emu.h`) is then so many nanoseconds an
instruction (`RVC_TICK_SCISSOR="x0 y0 x1 y1"` draws only those pixels of the tick):

| Added to every instruction | Nanoseconds |
|---|---|
| 24 operations, each on the last one's result | 31 (1.3 each) |
| 48 of them | 51 (1.1 each) |
| four reads of an array, among 32 of its elements | 8 |
| ... among 64, 128 | 12, 18 |
| ... among 256, 512 | 50, 100 |
| all four words of a `uint4` element chosen at random among 256 | 49 |
| one word of it | 10 |
| a write of an array's element; of a `uint4` one | 2; 4 |
| a texel of RAM read and used, the same one every time | 36 |
| ... a different one every time; two | 86; 211 |

So: an operation is a nanosecond and a bit, whether or not anything waits for it. An array
element that was read lately costs two or three; about a hundred elements stay that way, and
past that every read is 12 to 25 ns, one after another (a `uint4` element is four of them,
and so is one of its words chosen by a number: the four are read and one kept). A write
costs next to nothing. A texel of RAM is 36 ns when the texture's cache has it and a hundred
when it does not.

And what does not matter: **how many pixels run the tick.** The same pass drawn for one
pixel takes 2.07 ms, for one block of 32 pixels 2.03, for all 128 of core 0's 2.02. So there
is nothing to be had from fewer pixels, from one invocation in a vertex or geometry shader
with the state sent on as points, or from where the pixels are. Nor does the harness's wait
for its readback matter (spinning on `Map`, a longer ring, sixteen frames a `Map`: nothing),
and the card's clock under this load is the highest it reaches (2,835 MHz, P0).

## What a pass is made of

`tools\pass_costs.ps1` times one pass of each of seven kinds of work under Linux at three
places (a shell loop, Doom's demo, gzip, sha256sum, find, dd, grep) on a frozen state and
counts the same pass with `--profile`; `python tools\pass_regress.py logs\pass_costs.csv` fits
a cost to each kind of event. Twenty-one passes cannot tell apart counts that rise and fall
together, so the kinds are few and the figures rough (the passes are fitted within 5%):

| | Nanoseconds each | Of the time |
|---|---|---|
| an instruction that is no load or store | 115 | 33% |
| a load | 450 | 35% |
| a store (and 180 more when it searches the cache) | 370 | 29% |
| an instruction on the general path | 2,700 | 2.5% |

Loads and stores are 39% of a shell loop's instructions and two thirds of its time; a pass of
sha256sum, which is 85% arithmetic, takes 1.2 ms where the others take 1.9 to 2.7. What is
left in a load is mostly the texture: 0.09 (a shell loop) to 0.17 (Doom) data texels read an
instruction. Sixteen texels remembered would have had a third to a half of those
(`--profile` counted it), at two array reads for every load that gets that far: not tried.

The timer's interrupt is 2.5% of a shell loop at `--fixed-dt 0.004` (which has it every 2.5
passes; real time every five): 4.07M against 3.97M with 0.04 ms a pass.

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
- `add` and `addi` (most of the arithmetic) taken by a branch of their own in place of the
  eight results computed and one selected: nothing (the driver makes a select of a small
  branch anyway). The instruction's two ends, "not taken by this path" and "the budget is
  spent", as one test: 2% slower.
- The write cache's texels as an array of words in place of one of `uint4`, so that a load
  reads one element and not four: 0.5% by itself, 3% slower with the remembered texels.
- Four texels of RAM remembered in place of one for data reads: the counts say a third fewer
  texture reads, which choosing among four costs back.

A pass at 8,192 and at 32,768 differs in its events too when timed with `--fixed-dt 0.004`:
that is 4 ms of guest time a pass whatever its length, so the kernel's timer interrupt (some
60 page walks and 65 general-path instructions each) comes four times as often an instruction
in the shorter pass. Real time has it a hundred times a second either way.
