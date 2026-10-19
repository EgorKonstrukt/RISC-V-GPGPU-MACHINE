# RISC-V translated to Udon at build time (an experiment)

Udon cannot make or load a program at runtime: no extern assembles, loads or swaps one, and
there is no reflection or delegate. So instead of a JIT, `rv2udon.py` translates a bare-metal
RV32IMA image into Udon assembly once, in the editor, the way UdonSharp's output is made.
An interpreter in Udon runs at about 0.6 MIPS, below one of the shader machine's cores; the
translation is what might beat it.

    python experiments/udon_aot/rv2udon.py programs/bin/raytrace.bin programs/build/raytrace.elf OUT
    python experiments/udon_aot/rv2udon.py programs/bin/classes.bin - OUT      (no ELF: all of it as code)

`OUT/prog.uasm` is the program, `prog.jt` the jump table, `prog.externs` the externs it calls.
`editor_runner.cs` is a method body for `unity_execute_code`: it runs a C# RV32IMA interpreter
as the reference, then the translation in the editor's Udon VM, with the same console input,
and compares the console and every word of RAM.

## How it translates

- x1 to x31 are heap variables, x0 one never written; every immediate is a constant of its own
  (hex literals: Int32 arithmetic in Udon wraps, and the literal parser takes `0x80000000`).
  An add is `PUSH, PUSH, PUSH, EXTERN`; `li`, `lui`, `auipc`, `mv` and `jal`'s link are a
  `COPY`; a write to x0 (a `nop`) is nothing.
- RAM is one `int[]` from 0x80000000. A word load is three externs: `rs1 + (imm + 2^31)`, `>> 2`,
  the array's get. Bytes and halves shift and mask the word; a byte store reads, masks, writes.
- Branches and `jal` are direct `JUMP`s; `jalr` looks the target up in the jump table (one
  entry for every word of the text) and takes `JUMP_INDIRECT`.
- Where a backward branch lands and in every `jalr`, a budget is counted down; at 0 the slice
  ends and the next starts there. A console write ends the slice too, and a second status
  poll with no input and no write between it is a wait.
- Devices are found by constant propagation over the program: a load or store whose base is
  a known constant outside RAM is the UART (the transmit, receive and status registers) or the
  timer. Any other base is taken to be RAM, unchecked.
- `mulh*` is `Math.BigMul` and a shift; division is in 64 bits, where `MIN / -1` cannot throw.
- The heap is sized to the declarations through UdonSharp's `CompilerUdonInterface.Assemble`:
  the default heap is 512 variables, the most 1,048,576. Int64 constants and the RAM image are
  set in the heap after assembly, as an editor script would before the asset is saved.

## Measured (19 October 2026, editor, Mono, Ryzen 7 5800X3D)

All of them give the reference's console and RAM to the word (`optest.c` is built with `programs/build.bat`'s flags).

| program | guest instructions | Udon | guest MIPS |
|---|---|---|---|
| raytrace 320 x 200, 3 bounces, shadows | 73,572,594 | 12.1 s | 6.08 |
| raytrace 64 x 40 | 2,957,114 | 0.49 s | 6.04 |
| op test (every translated operation over edge values) | 39,392 | | same RAM and sum |
| classes, all seven loops | 46,500,442 | 9.0 s | 5.17 |

raytrace: 1,393 instructions become 12,449 Udon instructions (96 KB) and 457 heap variables;
its mix is 64% arithmetic, 17% multiplies (16.16 fixed point), 9% loads, 4% stores.

`classes`, per guest instruction: arithmetic 128 ns, `lw` 267, `sw` 263, a two-instruction loop
206 (its budget check costs what its body does), call and return 267, loads striding through
4 MB 156, jumps between `nop`s 93.

## What it would take

- Code known at build time only: anything loaded or written at runtime (Linux's programs,
  which a kernel without an MMU places where memory is free) needs relocation or a fallback.
- No traps, CSRs, interrupts or SBI yet; an unknown pointer to a device would throw in Udon.
- The VRChat client runs the VM under IL2CPP, not measured here; and Udon's time is the main
  thread's, so a frame's few milliseconds bound it, where the shader machine's cores are not.
