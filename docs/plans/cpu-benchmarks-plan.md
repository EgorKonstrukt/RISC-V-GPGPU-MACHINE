# Plan: the benchmarks of the era, on the machine

Not built (20 October 2026). The machine's speed is said in its own terms now: instructions a
second, passes a frame, a game's frames (`docs/machine/speed.md`). This plan puts it on the scales
the 1980s and 1990s used, with their own programs, so that "a core is a 486 of N MHz" is a
measurement and not a guess.

## Which

All open, all plain C, none needing more than the C library and a clock:

| Benchmark | Says | Compares with |
|---|---|---|
| Dhrystone 2.1 | integer and strings: Dhrystones a second, DMIPS (over the VAX 11/780's 1,757) | every CPU of the era, and RISC-V cores' published DMIPS/MHz |
| Whetstone | floats: MWIPS | the same years; the FPU (`docs/machine/fpu.md`) |
| BYTEmark / nbench (BYTE, 1995; its Linux port) | ten kernels (sorting, bitfields, FP emulation, Fourier, assignment, IDEA, Huffman, neural net, LU) as indices against a Pentium 90 and an AMD K6/233 | a "this is a Pentium N" figure |
| CoreMark (2009, Apache) | the modern Dhrystone: lists, a matrix, a state machine, a CRC | embedded RISC-V cores' CoreMark/MHz |
| STREAM | memory bandwidth: copy, scale, add, triad | the write cache and RAM in a texture |
| lmbench (a few of its tests) | system call, context switch, pipe and fork latencies | the kernel's work in AGENTS.md (vfork, the zero pool, the ROM's windows) |

LINPACK and UnixBench are left out at first: LINPACK says what Whetstone and nbench's LU say,
UnixBench is mostly the shell and file system, which lmbench measures more directly.

## How

- **Built** by the userland's toolchain (`linux/userland/toolchain.sh`, `rv32-cc -O2`) into flat
  programs, in a folder of their own (`linux/bench`, a `build.sh` as `linux/apps/build.sh`), the
  sources fetched once by the script as `linux/apps/build.sh` fetches zlib. Not on the desktop.
- **Two numbers each**, because the machine has two clocks:
  - on `rvc_cpu`, the benchmark's score per million instructions: what the compiler and RV32IMAF
    make of it, the same on any speed of core (DMIPS and CoreMark a MIPS are the per-MHz figures
    real cores publish);
  - on the card, through `tools\gpu_queue.ps1`, the score in real time: what a core of the machine
    is, at the instructions a second it runs there.
- **One core and then the workers.** CoreMark and nbench run on core 0 first. CoreMark's copies on
  every worker through `mcwx.h` (its threads are not workers) give the machine's whole figure, the
  one the lane plan's table is set against (`docs/plans/simd-lanes-plan.md`).
- A benchmark that times itself by `time()` or `clock()` reads the machine's clock, which runs at
  the host's speed: on `rvc_cpu` that is not the card's. The per-instruction figure is taken from
  the instructions counted, never from the program's own seconds there.

## What comes out

A section of `docs/machine/speed.md`: each benchmark's score per million instructions, in real time
on the RTX 5070 laptop (and the 5090 where it is at hand), and beside each the machines of the era
that scored the same. A case of `tools/test_all.py` for one short run of each, checked by the
benchmark's own result check (Dhrystone's and CoreMark's validation, nbench's) and not by its score.

## Steps

1. Dhrystone and CoreMark on `rvc_cpu`: the first per-instruction figures, and a look at what
   the compiler made of them with `tools/prof.py`.
2. The same on the card, one core; then CoreMark on every worker.
3. Whetstone, nbench, STREAM; lmbench's latencies last (they want the kernel as it is used, with
   the desktop off and on).
4. The section of `speed.md` and the `test_all.py` case.
