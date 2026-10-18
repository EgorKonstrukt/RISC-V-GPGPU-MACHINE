# The machine in one small compute shader (an experiment)

`rv32_min.hlsl` is the no-MMU machine's processor (`docs/system/nommu.md`) as one thread of a
compute shader, in 301 lines: RV32IMAF, entered in supervisor mode, SBI answered by the
shader, the CSRs as fields (`CSR_FIELDS`), RAM a byte address buffer read and written
directly. No fast path, no write cache, no TLB, no devices. It is a reference for what the
emulation is without the optimisations, and a place to try ideas on; nothing runs it yet.

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

## With a fast path

`rv32_min_fast.hlsl` (376 lines) is the same machine with a loop in front of the general step,
as the tick has (`fast_run` in `cpu.h`): arithmetic and M, `lui`, `auipc`, the jumps, the
branches, the floats whose encoding exists, and loads and stores on their own alignment
inside RAM, run back to back with nothing carried but the registers and pc, the RAM word read
and written directly. Anything else (a CSR, a system instruction, an atomic, a fetch or an
access outside RAM, a misaligned one, an encoding that does not exist) leaves the loop where
it is found, and the general step runs that instruction. None of what the loop runs can
trap or change what interrupts see, so the interrupt check stays where it was: only after
the general step says something changed. The arithmetic, a branch's test, the two jump
offsets and `fmadd` are functions both paths call.

fxc2: 1,791 bytecode instructions (207 flow control), 44,724 bytes. Against `rv32_min.hlsl`
by the same test as below, with the pass length up to 32 and up to 256: 0 differ in 177,141
and 42,720 runs (2.1 and 3.8 million instructions) and in 300,000 single instructions. The
test sees the fast path: the loads' sign extension broken in it alone, 213 of 11,790 runs
differ. Not timed: nothing dispatches either shader.

## Running it: rvc_compute_harness

`bin\rvc_compute_harness.exe` (`harness/apps/rvc_compute_harness.cpp`) runs either shader on the
card: the image's `linux_payload.bin` in RAM and `rootfs.bin` as the ROM, a dispatch of `(1, 1,
1)` a pass, the state and the console's words read back after each, the console typed into
and printed. The flags are `rvc_harness`'s where they mean the same (`--image`, `--ticks`,
`--input`, `--expect`/`--send`, `--until`, `--stats-from`, `--seconds`, `--frames`,
`--fixed-dt`), and `--shader FILE`. It takes the GPU lock: through `tools\gpu_queue.ps1`.

The fast shader boots the no-MMU image to its shell in 1.6 s (9,782,704 instructions, 1,241
passes of 8,192). No GPU, sound or network device and no worker cores, so nothing past the
console.

On 18 October 2026 (RTX 5090, D3D11 + fxc2, passes of 8,192, the clock 2,865 to 2,910
MHz throughout), millions of instructions a second from where the command printed `START`,
about 12 s each, one run each, not in turn:

| | `rvc_harness` (the tick) | plain | fast |
|---|---|---|---|
| a shell loop | 5.37 | 4.60 | 6.65 |
| `sha256sum /bin/busybox` in a loop | 7.50 | 5.33 | 6.85 |
| `gzip -c /bin/busybox` in a loop | 3.40 | 1.26 | 2.07 |

Not yet known: whether the programs computed right on the mini machine (the sums of
`sha256sum` and of `gzip | sha256sum` against `rvc_cpu`'s were the next check), and how
much of the difference is the tick's whole machine (256 cores' state, the devices) against
the processor alone. gzip's loads and stores go to the buffer every time, where the tick has
its write cache and remembered texels.

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
