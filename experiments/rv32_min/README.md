# The machine in compute shaders (an experiment)

Three files, standalone (no includes):

- `rv32_min.hlsl` (334 lines): the no-MMU machine's processor (`docs/system/nommu.md`) as one
  thread of a compute shader, plain: a reference for what the emulation is without the
  optimisations, and a place to try ideas on. Core 0 alone.
- `rv32_min_fast.hlsl` (719 lines): the same, fast, with 255 worker cores (below).
- `sync.hlsl`: what `rvc_harness --compute` needs to run either as the whole machine, with the
  display, the GPU device, sound and the rest of the shader machine's devices.

The plain one: RV32IMAF, entered in supervisor mode, SBI answered by the shader, the CSRs as
fields (`CSR_FIELDS`), RAM a byte address buffer read and written directly. No fast path, no
write cache, no TLB, no devices.

## What it is

- One loop: an interrupt check, a fetch, a decode by `if`s in order of how common a kind is,
  the execution, one indexed write of the result. `[numthreads(1, 1, 1)]`, entry `tick`.
- `r[65]`: x0 to x31, then f0 to f31 as their bits, and a slot for writes that must not land
  (x0, no result), so every instruction ends in one indexed store and there is no float path.
- `csr[16]`: a CSR's slot is `(n >> 3 & 8) | (n & 7)`, which puts sstatus and mstatus, sie and
  mie, sip and mip in one slot each; a mask gives the supervisor's view. The machine's own
  CSRs read 0, as in the tick. The interrupt check runs only after what can change it (a CSR,
  a trap, `sret`, an SBI call).
- RAM from `0x80000000` (`Ram`, a `RWByteAddressBuffer`), the ROM from `0x40000000` (`Rom`),
  the RTC's two words at `0x030007f8`. Anything else reads 0 and drops writes: no GPU, sound
  or network, so a kernel and its console, not the desktop.
- The kernel's copy and fill (CSRs `0x0b0` to `0x0b3`, `linux/kernel/memcpy_hook.py`) run in
  the loop where they are asked for, not in a commit pass.
- The console needs nothing: it is memory (`docs/machine/console.md`, "Words in memory").

The host keeps the hart in `State` (words: `r` 0 to 63, `csr` 64 to 79, then pc, privilege,
fcsr, the reservation, the timer's compare low and high, the copy's source, destination and
length) and starts it with every word 0 but pc `0x80400000`, privilege 1, x11 `0x82200000`
(the device tree) and the reservation `~0`. A pass is a dispatch of `(1, 1, 1)` with its time,
the RTC, the instructions to run at most, RAM's size and a seed in the constant buffer; it
ends early on `wfi`.

Not done: `fflags`, the single rounding of `fmadd` (it rounds twice), denormals (the GPU
flushes them), NaNs made canonical, and a trap on a jump to an address not on a word.

## The fast shader: `rv32_min_fast.hlsl`

`rv32_min_fast.hlsl` (719 lines, no includes) is the same machine, fast, with worker cores.
Rewritten on 18 October 2026; what it was before (a fast loop in front of the general step,
376 lines) is in the history.

### What a single thread on the card costs

Measured on the RTX 5070 laptop (a loop of one thread, nothing else): a dependent load of RAM
45 to 57 ns, a load that misses to the far end of memory 320 ns, a groupshared read 25 to 35, a
local array (`static uint r[65]`) 50, a branch on a loaded value 20 more than a loaded address,
a dependent arithmetic operation about 3. And the card runs one thread's work at 1.14 to 1.33
GHz (P2), not the 3.09 it can: no other load raises it. So an instruction of the old fast loop,
about 340 ns, was some 100 operations in a row: the fetch, the tests of its kind, the
register reads, the writes, the loop. What made it faster is fewer operations in that row
and fewer branches; what did not, below.

### How it is fast

- **Decoded instructions** (`Dec`, 8 bytes a word of RAM, 264 MB): kind, rd, rs1, rs2, rs3 in
  one word, the immediate in the other, a branch's or jump's target and `lui`/`auipc`'s value
  worked out. An immediate operand reads x0 as rs2, so `addi` and `add` are one kind. Made the
  first time a word is run (`decode`); a store clears its word's entry, so an entry is always
  the word in RAM. Branches and jumps out of RAM are the general path's.
- **The kinds tested most common first** (counted on the three loads: `add`/`addi` 28 to 33%,
  `lw` 11 to 14, the other arithmetic 9 to 50, branches 15, `sw` 6 to 12, `lbu` and its kin 7):
  one `[branch] if` each, but `add` and the other nine kinds of arithmetic share the first, the
  result chosen by selects with no branch: 10% faster on a shell loop and 10 to 25% on
  `sha256sum` and `gzip` than `add` alone first (18 October 2026). `lui`/`auipc` or `jal` in it
  too, or the loads in one test, or the kinds renumbered so the test is one compare: slower.
- **Read ahead**: a turn reads the entry of pc + 4 and of a branch's target while it runs the
  one of pc, and takes the right one at its end with a select. A jump by register reads its
  target's entry when it knows it.
- **Two turns a time round the loop** (`turn`, called twice): the loop's test and branch half
  as often. Four times was slower (the card's instruction cache).
- **The registers in groupshared** (`r[65]`), not a local array: 8% on two of the loads.
- **The commonest floats in the inner loop** (`K_FA`, 18 October 2026): fadd, fsub, fmul, the sign
  injections (`fmv.s`, `fabs`, `fneg`), `fmv.x.w`/`fmv.w.x` and the four fused multiply-adds, one test
  with the result chosen by selects, as the integer arithmetic. Before, every float instruction
  failed the inner loop's eight tests, left it, and went through the outer loop and `op_fp`'s row
  of tests. Quake's timedemo 7 to 15% faster; integer loads the same. `mul` in the integer group
  as well was mixed (`sha256sum` 3% slower) and is not in.
- M, the other floats, an entry to make: the outer loop's. CSRs, `ecall`, atomics, traps: the general
  step's, as before. The general path is 0.5 to 0.7% of the instructions.
- Fixed on the way: `fcvt.w.s` of a negative number gave 0 and `fdiv` of two large numbers 0
  (fxc's conversions, the card's 1 / b; done as the tick does them). The guest's `fptest`: 0 of
  15 instructions wrong (3 before, also with the old shader).

### What did not pay (each timed on the three loads)

- A `switch` on the kind (an indirect branch): 10 to 20% slower than the tests.
- An entry with a tag and a check word (16 bytes, a hashed table): no faster than the raw word.
- The next instruction's registers read a turn ahead, with forwarding: 25% slower (more
  operations in the row than the read it hid).
- The word of a load read before its kind was known: 5 to 10% slower.
- Pairs fused at decode (`addi` + `addi`, `sw` + `sw` and `lw` + `lw` on one base, `lbu` and a
  branch on it: 18 to 25% of the pairs): as fast as without; the tests they add cost what they
  save. A turn of a pair costs as much as one of a single instruction.
- The body four times a round: 10% slower than twice.

### Cores

A core is a thread group of one thread: `Dispatch(cores)`, core 0 the kernel's, the others
workers on the mailboxes of `src/mc.h` (`docs/machine/multicore.md`), with the tick's rules:
while a core's start texel says "MCSU" it starts there (pc, stack) whatever it was doing, in
user mode with its number in a0; "MCST" parks it; `pause` puts it to sleep on its job word
as the pass found it, `ebreak` parks it, `fence.i` ends its pass; a trap or `ecall` writes
the fault record (the count last) and stops it until core 0's resume word changes, a call's
answer then in a0. A worker that is parked or asleep costs a thread group that reads a few
words and ends.

**A fault this machine has and the shader machine has not**: nxray (60 pictures) stops a
 worker at its start now and then (`mcw: a worker core stopped, cause 2, at pc 0`), 2 runs in 6
 with this shader as committed before 18 October and more with the ones after, which change
 only the timing. Caught with the jump's source recorded: a worker's `ret` read a return address
 of another call of the same loop from its stack (`lw ra` gave an older word of that slot).
 Workers here read and write one image together during a pass, which a GPU's memory gives no
 order to (below); on the shader machine a core reads RAM as the last commit left it and its own
 stores. Not fixed: a worker reading the third image but for the pages it stored to cost Quake
 III 7% and did not stop it, so the cause is more than which image a worker reads.

What a core stores, the others see from the next pass on, all at once, as on the shader
machine. A GPU's memory gives no such thing within a dispatch (a store can be seen before an
earlier one, a cache can hold an old line), and programs made for the machine count on it:
nxray with 255 workers had a worker call address 0 when it read core 0's half-written words.
So RAM is three images in one buffer: core 0's, the workers', and both as the last pass left
them. A core reads and writes its own image and marks the 4 KB page of each store (once in a
row: a register keeps the last page). After every pass `merge_list` (one thread group of
1,024) lists the marked pages and writes the dispatch for them, and `merge`, a thread group a
listed page (`DispatchIndirect`), takes each word as core 0 left it if core 0 changed it, else
as the workers left it, into all three images, and clears the entries of decoded words that
changed. Workers see each other's stores at once (on the shader machine a pass later): no
program tried so far minds. What the merge costs core 0 is nothing that the three loads show.
A first store to a page also marks a word for its 32 pages, and `merge_list` reads a page's
word only under a marked one: 0.03 ms a pass where reading all 32,768 was 0.29.

A core is one thread, not a warp's worth: the instructions are a chain, and the 31 other lanes
could take only the kernel's copies and fills and decoding (an instruction's 32 neighbours at
once). A build that made every copy twice ran 1% slower, so that is all they could save; and
lanes that copied for lane 0 left it reading stale words even after
`AllMemoryBarrierWithGroupSync` (fxc2 writes `sync_uglobal_g_t`): Linux could not run
`/emuinit`. What lanes give each other has to go through groupshared.

## Running it

- `bin\rvc_compute_harness.exe` (`harness/apps/rvc_compute_harness.cpp`): either shader with the
  image's `linux_payload.bin` in RAM and `rootfs.bin` as the ROM, the console typed into and
  printed, `rvc_harness`'s flags where they mean the same (`--image`, `--ticks`, `--input`,
  `--expect`/`--send`, `--until`, `--stats-from`, `--seconds`, `--frames`, `--fixed-dt`), and
  `--shader FILE`, `--cores N` (workers: there is no window system here, so nothing starts
  them) and `--all-run` (every core runs the image from the start in supervisor mode with its
  number in a0, up to 8,192 of them: a measurement). It takes the GPU lock.
- `bin\rvc_harness.exe --compute fast` (or `plain`, or a file): the whole machine, below.
  `--compute-cores N` (256: core 0 and 255 workers; plain has core 0 alone) and
  `--compute-passes N` (fast: 4) and `--compute-split F` (fast: 2): the first F passes of a frame always
  run, each core's at most `--ticks` / F of them (`--ticks` is 16,384 unless given), and the rest only
  while core 0 rests (`pause`, the kernel's `REST`): `merge_list` writes the next pass's dispatch,
  `DispatchIndirect` of none when core 0 ran its whole pass (`Args[6..8]`, State words 102-103 of
  core 0: its instructions in this frame and whether it ended early). A frame of the devices'
  passes costs some 0.2 ms beside a pass of 8,192 of about 1.1 ms (Quake's timedemo): two passes a
  frame were Quake 18 to 19% faster and Red Alert 7%, and the passes while core 0 waits for its
  workers Quake III 17% to 23%. `plain` keeps a pass a frame.

## The whole machine: `rvc_harness --compute`

    bin\rvc_harness.exe --compute fast --desktop --viz        (plain: rv32_min.hlsl, or a file)

The harness as it is, with this shader as its processor in place of the tick: its window,
the display, the GPU device, the sound card, the keys and the pointer, the console, `--expect`,
`--send`, `--until`, `--stats-from`, the fetch and the network, and 255 workers. It is `--cpu`
(`docs/machine/cpu-harness.md`) with the processor on the card instead of on the host:

- The init frames set the state texture up as ever; then the backend loads the image into the
  three images of RAM and `rootfs.bin` as the ROM, and a frame is `--compute-passes` passes of
  the cores, each with its merge, then the devices' passes.
- `sync.hlsl` puts the pages of the GPU's memory (0x86000000 on, bands 24 to 31) the merges
  listed in this frame into the state texture, a thread group a page (`export_pages`,
  `DispatchIndirect`), and Linux's own bands whole only when they are wanted: at first and,
  with the memory view open, every 60th frame, as `--cpu` does (nothing else reads them from
  the texture). It writes core 0's state rows as the commit and the host read them: the
  instructions so far (28,0).g, the pc (36,0).a, and in (41,0) the bands the commit carries
  over into the other buffer. No write cache, no stall.
- The commit, the GPU's draw, the sound card's mix and the control pass are the harness's own.
  After them `sync.hlsl` takes back into all three images what they wrote: the control words,
  the picture the commit copied back for the list drawn last (GPU_COPY), and when the host
  delivered them in this frame the console's input word, the network's ring, a fetch's answer
  and the counters' totals. Then `sync_cores` (a thread a core) writes what the control pass writes on the
  shader machine with its cores (`MC_STATS` in `src/gpu.h`, rvc_cpu's `control_pass`): how many
  cores the geometry has, which run and which sleep, every core's count, how many there could
  be; a worker the geometry does not have is parked. All of it is on the card, in order:
  nothing waits for a readback.
- The shader passes are built without `CORES`: the cores are the compute shader's.
- `pause` ends the pass (the kernel's `REST`, a program waiting for the next pass). `cycle` and
  `instret` count the core's instructions so far: a program's own counts (Quake III's
  `q3stat:`) work.

`RVC_COMPUTE_PROF=1` times one frame in 16 on the GPU at every step of this and prints the
means every 200: on a shell loop with a pass of 8,192 a frame, the cores 2.5 to 2.8 ms, the
merges 0.04, the export 0.01, the commit and the devices 0.35 to 0.48, what comes back 0.06.
Before the page lists the merges' list was 0.29 ms, the bands' export 0.2 and `sync_cores` on
one thread 0.26 to 0.31.

Not done: `--save-state` and `--load-state` (the processors' state is in their buffers, not in
the snapshot: the snapshot's texture has the GPU's memory, so `tools/snapshot_screen.py` shows
the screen), D3D12.

## Measured

On an RTX 5070 laptop on 18 October 2026, D3D11 + fxc2, no memory view (the view costs some
1.5 ms a frame). The card's clock moved between 1.14 and 1.33 GHz from run to run; what is
compared was run in turn.

**One core, `rvc_compute_harness`, passes of 32,768**, millions of instructions a second from
`START`, the shader before this rewrite (commit 786fb12) against this one, three rounds in turn,
the best of each:

| | before | now | |
|---|---|---|---|
| a shell loop | 2.93 | 3.94 | 1.34 |
| `sha256sum /bin/busybox` in a loop | 3.24 | 5.01 | 1.55 |
| `gzip -c /bin/busybox` in a loop | 2.93 | 4.00 | 1.37 |

Per round the ratios were the same within 3%. Without the workers' code (the three images,
the page marks) the shell loop was 3 to 6% faster. The sums of `sha256sum /bin/busybox` and of
`gzip -c /bin/busybox | sha256sum` are `rvc_cpu`'s.

Then `add` in the arithmetic's test, in turn with the shader above at 1.14 GHz (best of two):
a shell loop 3.21 -> 3.61, `sha256sum` 4.52 -> 5.07, `gzip` 3.26 -> 3.76.

**The whole machine against the shader machine**, 18 October 2026, after the page lists and
the arithmetic in one test: `rvc_harness --compute fast` against `rvc_harness --rvc
experimentsvc_opt` (the tick in a fragment shader, its commit, 255 workers), the same
`--ticks` (8,192 a frame), each load typed at the shell and measured from 14 s on for 20 s
(`--stats-after`), two rounds in turn, both at 1.14 GHz; millions of instructions a second:

| | the tick | compute | |
|---|---|---|---|
| a shell loop | 1.77, 1.79 | 2.98, 2.93 | 1.64 to 1.68 |
| `sha256sum /bin/busybox` in a loop | 2.33, 2.32 | 4.09, 4.06 | 1.75 |
| `gzip -c /bin/busybox` in a loop | 1.93, 1.94 | 3.10, 3.06 | 1.58 to 1.61 |

A frame of the shell loop: the tick 4.6 ms (4.2 its pass), compute 2.75 (2.5 its pass and
merge). With 255 workers, Quake III (`nano-X -p & sleep 2; QUAKE3_STATS_FRAMES=100 quake3
+devmap q3dm17`, standing still, `q3stat:` frames 101 to 400) took 2.58 to 2.62 s for 100 frames
on compute and 3.89 to 4.01 on the tick (1.5 times), and showed 38 frames a second in its
title; nxray (`RAY_FRAMES=4`) 73 to 86 ms a picture against 126 to 133, pictures 1 to 3 the
tick's sums. The card's clock is the driver's choice from run to run (0.88 GHz, 1.14, 1.33):
on another day the tick's Quake III was 3.29 to 3.35 s, so compare runs made in turn.

Before (17 October 2026, the same harness, other clocks): a shell loop 2.12 million
instructions a second with a pass a frame, 2.46 with two, 2.79 with four (`--compute-passes`);
the tick 2.03. Quake III, seconds for 100 frames:

| | seconds | thousands of instructions a frame (core 0) |
|---|---|---|
| the tick, 255 workers | 3.29 to 3.35 | 29 |
| the tick, everything on core 0 (`QUAKE3_SERVER=inline` `CLIENT` `SCENE` `SIDE` `inline`, no picture workers) | 15.1 to 15.4 | 267 to 273 |
| `--compute fast`, 255 workers | 3.29 to 3.41 | 29 to 30 |
| `--compute fast`, one core, this shader | 8.0 (at 1.33 GHz) to 11.5 (at 1.14) | 189 to 231 |
| `--compute fast`, one core, the shader before | 10.7 to 10.8 | 218 to 219 |

With workers the game's frame was 30 a second on both machines: the client's frame on core 0
(29 thousand instructions) plus waiting. Doom's demo1 with the view on workers: its 50 state
sums' digest `0ab4df93`, `tools/test_all.py`'s (again after the page lists, with
`DOOM_VIEW=told DOOM_VIEW_CHECK=1`: 0 not as told). `mctest 3` and `mctest 255 30000`: PASS, 0
pages brought for their start. `fptest`: 0 of 15 wrong. nxray's picture 0 differs from run to
run on the compute machine (rays 199,920 to 201,477, the tick's 201,460): nxray gives a picture
up when a key or a resize comes while it is traced and publishes the next a step of its clock
on, and when that comes is the machine's timing.

**The floats in the loop and the passes a frame** (19 October 2026, the same card and harness,
`--compute fast` as now against the shader before both, which had a pass of 8,192 a frame), ms a
game's frame or nxray's picture, and all cores / core 0 / workers in millions of instructions a
second (the workers' counted since `sync_cores` lists the counts' page: before, the host read
only workers 1 to 15's), against the fragment tick (`--rvc experiments\rvc_opt`):

| | the tick | compute before | compute now | |
|---|---|---|---|---|
| Quake, timedemo demo1 | 13.35 (8.3 / 3.2 / 5.0) | 9.20 (13.4 / 4.9 / 8.5) | 7.33 (16.0 / 6.2 / 9.8) | 1.26 |
| Quake III, `timedemo 1`, q3dm17 | 14.66 (12.0 / 1.7 / 10.2) | 9.81 (17.2 / 2.5 / 14.7) | 8.12 (20.7 / 2.9 / 17.8) | 1.21 |
| Red Alert, `RALERT_HUNT=50`, frames 500-900 | 24.55 (6.5 / 4.3 / 2.2) | 13.74 (12.9 / 7.5 / 5.4) | 12.69 (13.6 / 8.1 / 5.6) | 1.08 |
| nxray, 60 pictures | 45.3 (183 / 4.5 / 178) | 29.9 (296 / 7.2 / 288) | 11.5 (354 / 8.7 / 345) | 2.6 |

Over three rounds each the same: Quake 18 to 19% faster, Quake III 17 to 23%, Red Alert 2 to 7%.
nxray's pictures 1 to 7 are the tick's sums; its time is of a run the start race did not meet.

Measured again with every machine later on 19 October 2026 (`docs/machine/speed.md`, the same loads and
card), the tick and compute now: Quake 11.71 and 7.96 ms, Quake III 13.06 and 8.86, Red Alert
21.68 and 14.09, nxray 44.2 and 12.2 (its first run met the start race), Doom's `-timedemo demo1`
16.67 and 9.90; `factor 999999999989` 21.45 and 12.52 s; the plain shader 36.23, 57.93, 40.52,
6,567, 21.81 ms and 34.73 s.
**How far cores scale** (`--all-run`, a mix of `addi`, `lw`, `sw`, `add`, `bne`, `xor`, `lbu` on
every core, each on its own stack, passes of 32,768, the clock sampled through each run):

| cores | millions of instructions a second, all | per core | per core per GHz |
|---|---|---|---|
| 1 | 3.8 | 3.79 | 3.32 |
| 16 | 59 | 3.69 | 3.24 |
| 64 | 237 | 3.70 | 3.27 |
| 128 | 479 | 3.75 | 3.29 |
| 256 | 884 | 3.45 | 3.03 |
| 384 | 1,187 | 3.09 | 2.73 |
| 640 | 1,541 | 2.41 | 2.13 |
| 768 | 1,597 | 2.08 | 1.83 |

Linear to about 256 cores (92% of one core's speed each), 83% at 384, 65% at 640; the total
still grows to 768 and then little. The card has 36 SMs of four schedulers: at 256 cores that
is under two threads a scheduler, and a core issues an operation every few cycles, so the
schedulers fill from about 3.5 a scheduler (500 cores); this shader's registers allow some
16 to 20 thread groups an SM resident (576 to 720). A sweep before the merge was in gave the
same shape. The machine has 256 cores (its mailboxes).

## How it was checked

- fxc2 (`tools\fxc2\d3dcompiler_47.dll`, `cs_5_0`) compiles it in 0.04 s with no warnings:
  1,298 bytecode instructions, 145 of them flow control, 32,692 bytes.
- It was written from a first version of 491 lines that did the same with switches, a
  separate float array and an interrupt check before every instruction (1,911 bytecode
  instructions, 388 flow control; not kept). Both were made C++ with a shim for HLSL's types
  and buffers and run from the same random states on random valid instructions, pending
  interrupts and all; registers, CSRs, pc and RAM compared to the word. 0 differ in 294,473
  runs of up to 32 instructions (3.4 million instructions) and in 500,000 single ones. The
  shim makes every NaN canonical (clang's operand order otherwise chose which NaN's bits
  came through), and a run that fetched an encoding that does not exist or a pc not on a
  word was left out: the two versions were not meant to agree there.
- It has not run on a card. Nothing in the repo dispatches it; the bytecode counts say
  nothing of the time an instruction takes (`docs/machine/tick-costs.md`).

## Ideas talked over (17 October 2026)

None of these was built. What the repo had measured already decides most of them.

### More registers

RISC-V has 32 integer registers in every encoding (5-bit fields; the compressed ones name
x8 to x15 only), and no extension adds more. What does exist: D and Q widen the float
registers, RV64 widens the integer ones (another base ISA, kernel and toolchain), V adds 32
vector registers and a large instruction set, P is SIMD inside the integer registers. A
custom extension could reach a bank of more, but no compiler would allocate them. And the
registers are not where the time is: with 31 and 32 of them compiled code seldom spills.

### The registers in group shared memory

`experiments/rvc_compute` has it (`XR_SHARED`, with `L1_SHARED`): 2.61 to 2.70 ms a pass
against 2.70 to 2.76 on the raytracer, which its README calls nothing that can be told from
noise. One thread a group shares nothing, and the array is in fast memory either way. Its
FPU build does not use `XR_STORAGE` at all (`static uint xr[XR_F + 32]` in `types.h`).

### Instructions fused into bigger ones

A pass that decodes code into larger operations (an address and a load, a constant's `lui`
and `addi`, `auipc` and `jalr`, `slli` and `add`, a counter's `addi` and its branch, two
loads off one base) runs two guest instructions a turn. What it can take is the dispatch:
an instruction absorbed skips its fetch, its walk through the kinds' tests and a turn of
the loop, some 40 of about 120 bytecode operations (`docs/machine/tick-costs.md`, "Where an
instruction's operations are"). Work published on RISC-V macro-op fusion has single-digit to
low-teens percent fewer instructions; 5 to 15% here is the guess.

- A predecoded form alone is worth about ten operations, and four of them taken out was
  1.0 to 1.4% slower: the shape of the tests matters more than their number.
- Every kind added deepens the tree of tests, and a test an instruction passes is 16 to 19
  ns: only a few very common fusions can pay.
- When to throw decoded code away is already defined: RISC-V lets a fetch see old code until
  `fence.i`, which the tick stalls on (`STALL_FENCE`); without an MMU the key is the address.
  The decoding could be a pass of its own over whole pages, on thousands of threads.
- Finding a decoded entry by pc is a read that waits, 90 ns when the texture has to be read:
  a third of an instruction. The raw fetch hides this with its read-ahead; a decoded stream
  would want the same, laid out by basic block.
- A VRChat world compiles no shader while it runs: an interpreter of larger operations is as
  far as it goes there.

(Tried on 18 October 2026 on the fast shader: the pairs counted, the four commonest fused; no
gain. "What did not pay", above.)

Next, if it is taken up: count pairs and triples on the three loads (a shell loop, Doom's
timedemo, Quake e1m1) with counters in a private `rvc_cpu`, for how much the first four to
eight idioms cover; then fuse in the decoder as it is (the next word is usually in the
fetched texel), with no cache, and time it on the card; a decoded cache only after that.
Not on this shader: with no fast path, fusion would look far better here than in the tick.

### Several fast loops, one for each kind of work

An integer-only loop, a float one, and the tests for what a loop does not run taken out of
it. What it can take:

- The whole FPU costs an integer-only guest 1.1% (`docs/machine/fpu.md`). The fast step's
  tree of tests already keeps an integer instruction from passing the float ones.
- An arithmetic instruction is about 115 ns, a load 450, a store 370; loads and stores are two
  thirds of a shell loop's time (`docs/machine/tick-costs.md`, "What a pass is made of"). A
  cheaper loop changes the first figure only.
- A loop of register arithmetic is 135 ns an instruction, a two-instruction loop within one
  texel 120: the fetch, decode and dispatch every variant still pays. Below that is fewer
  dispatches (fusion), not cheaper ones.
- A wrong guess costs: an instruction on the general path is 2,700 ns. Quake has a float in
  every seven instructions, so the loops would have to go to one another, and every place
  they meet merges the state, which is what made the single loop slow before (`cpu.h`).

If kept in mind at all: by how memory is used rather than by kind (a pass that has written
nothing could have loads that skip the write cache's search, which `L1_HAS` already does in
part), and chosen by the host for a whole pass among compiled kernels, which costs the
shader nothing. A count of where loads and stores go (the cache's search, the texel read
last, the texture) says first whether there is anything there.
