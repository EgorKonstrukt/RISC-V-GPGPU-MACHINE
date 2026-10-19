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

## Linux: the kernel and its programs translated

`linux2udon.py` translates the no-MMU kernel's text (0x80400000 to `_einittext`, 743,675 words)
and the text of programs in the ROM into Udon bytecode, and adds an interpreter written in Udon for
the rest: the system instructions, code it did not translate, and programs not named.
`editor/UdonLinux.cs` loads it into the editor's VM and runs it slice by slice with
`editor/RvRef.cs`'s console and clock; `RvRef.cs` is also the reference (`rv32_min.hlsl` in C#).

    python experiments/udon_aot/linux2udon.py IMG/linux_payload.bin OUT [IMG/rootfs.bin /bin/busybox ...]
    UdonAot.UdonLinux.Run(OUT, IMG, "/ # ", 40000, 120.0, "/ # ", "uname -a
")   (editor code)

- Registers are heap variables in translated code and an `int[]` in the interpreter; they are
  copied over when one hands to the other.
- Code is translated a block at a time: values known within a block are folded to constants
  (106,494 instructions in the kernel), writes overwritten before a read are dropped (29,311),
  and an address is checked to be RAM once a block for its base register. The kernel alone is
  50 MB and 181,391 heap slots, made in 10 s.
- A load or store below RAM goes to a routine for the ROM, the clock and the devices; a store
  there is dropped. `jalr` looks its target up in a table of the text; outside it, the interpreter.
- CSRs, `ecall`, `sret`, `wfi`, traps, interrupts and the SBI are the interpreter's. The copy CSR
  is `Array.Copy` or `Array.Clear`.
- The F extension as the machine has it (`docs/machine/fpu.md`) is the interpreter's too: registers
  as bits, each operation a `BitConverter.Int32BitsToSingle`, the `Single` operator and
  `SingleToInt32Bits`; conversions to an integer rounded by `Mathf` as the instruction's mode says
  and clamped. Translated code hands a float instruction to it.
- A program (a FLAT binary, whose text has no relocations) is translated at its file offsets and
  runs at a base held in a variable, wherever the kernel put it. At `sret` to user mode the
  interpreter finds which program and base by four words of its text, checked again each time.
  Programs that were not translated are interpreted.

Measured on 19 October 2026 (editor, Mono, Ryzen 7 5800X3D): with `uname -a`, `ls /`, an
arithmetic `echo`, `cat /proc/cpuinfo`, `head /etc/passwd` and `md5sum /bin/busybox`, the
console is the reference's to the byte, the boot timestamps aside: 8.2 s with busybox translated,
62.6 s with it interpreted (0.4 MIPS). First prompt at 3.0 s. With the kernel, busybox, nano-X
and the desktop's nine programs the build is 116 MB, 402,985 heap slots, 27.6 s.

## The C++ translator (`l2u/`)

`l2u/l2u.exe` takes `linux2udon.py`'s arguments and writes the same kind of output, a tenth to an
eighth of the bytes, in a second or two (`l2u/build.cmd` compiles it, 7 s):

    set L2U_HOT=build\udon_profile
    experiments\udon_aot\l2u\l2u.exe IMG\linux_payload.bin build\udon_linux IMG\rootfs.bin /bin/busybox ...

- Two tiers. Hot words are translated inline as before, in superblocks. Every other word is
  threaded: the segment's jump table (`jt`) points it at a routine shared by every word of the same
  operation and registers (about 42,000 of them), whose immediate is in the segment's `tim` table.
  Any pc can be entered, so the interpreter is seldom reached (7,500 handovers in the shell check,
  51,500 before). Threaded code runs about 2.5 times slower than inline.
- Hot is what the profile `L2U_HOT` names (words run 100 times, `L2U_HOTN`), and innermost loops
  of up to 64 words in programs (`L2U_INNER`; the kernel's `L2U_INNERK`, 0). A profile is made by
  a build with `L2U_PROF=1`, a run, and the `prof5` array read back, by program name: a program
  rebuilt into other words has none. `build/udon_profile` is the shell check's training script,
  the desktop, nxterm, nxsettings, nxnet, glxgears, a fetch and nxview, and each game's start and
  play run headless, summed.
- Floats, the CSRs, `fcsr` and the clock reads are translated in both tiers (fpu()'s semantics, f
  registers as `Int32` bits: a `Single` heap variable quietens a signalling NaN). 2,208 generated
  tests (`gen_ftest.py`) give the interpreter's results; `fptest 2000` the reference's console.
- A program's code that was not translated is threaded when it first runs, into a window of 1 M
  words with the same routines (`sec_dyn.inc`; `fence.i` empties it): `fptest 2000` 1.2 s, 6.5
  interpreted. Loads from the ROM (mapped files) stay in translated code.
- Library routines known by a hash of their words run natively (`sec_native.inc`): musl's and the
  kernel's `memcpy`, `memmove` and `memset` as `Buffer.BlockCopy` and `Array.Clear`, libgcc's
  64-bit division as `Int64`'s, musl's `strcmp` and `memcmp` a word a step; other cases go on in
  the translated body. Switches for comparisons: `L2U_DYN`, `L2U_NATIVE`, `L2U_UDREL`, `L2U_THR`.
- A halfword is moved by `Buffer.BlockCopy` of 2 bytes through a small `int[]` (`lhu` an add, the
  copy and a read; `sh` a write and the copy). Not right: a halfword across a word boundary and a misaligned
  word load or store.
- `build/udon_profile` is not in the repository: it is remade by the steps above.

Measured (19 October 2026, editor): kernel alone 4.8 bytes a text word (75.6 from the Python one);
kernel and 13 programs 22.4 MB and 65,232 heap slots (135 MB), translated in 1.7 s (24 to 31 s);
kernel and Red Alert 31.2 MB and 84,237 slots (252 MB, 854,733), booting to the prompt. The shell
check 7.4 s (8.1), the console the reference's. In the world against the Python build: prompt 4.0 s
(5.0), the desktop idle 16 s after `nx` (20.5), glxgears in a window counting 881 to 965 frames a
second (606 to 652); `dd` from `/dev/urandom` to `/dev/fb0` 3.0 s (1.6), the kernel's random
generator being in no profile. Without the desktop in the profile its start took 28 s.

With the games (19 October 2026, alternating runs in the translator's own runner, against the
build and profile above): Doom's timedemo from its mark 2.30 s (5.9; 2.65 with the old translator
and the games' profile), the shell check 5.8 s (6.7), boot 2.0 s (2.65), md5sum of 4 MB 9.0 s
(9.9), fptest 1.2 s (6.5). Hot inline code runs at 1.6 to 1.9 externs a guest instruction (an
add is one, 76 ns), so the translator's own share of that is 1.04 to 1.13 times; the rest is the
interpreter left behind and the profile. The world's build, the kernel, the 13 programs and the
nine games, is 123.4 MB, 14.0 bytes a word, 377,713 heap slots, translated in 8.7 s; the menu
imports it in 16 to 36 s.

The games in the world (editor, GPU mode, `nano-X -p & sleep 2; GAME &`, no worker cores, which
each game does without): Doom plays its demo at 21 frames a second, Quake e1m1 at 23, ClassiCube
at 28, the NES at 90 to 97% of its speed, Quake III q3dm17 at 11 after 90 s of loading; Red
Alert's menu, Tiberian Dawn's demo text and Micropolis's map are drawn and wait for the player.
Half-Life's hldemo1 runs at 1 frame a second after 190 s and its picture is black but for the HUD.

## Next: the GPU's upload (a plan, not done)

A drawing game loses about half of each frame to the driver's GPU path. `EmuUdonCpu` times a
frame's parts (`cpuMs`, `displayMs`, `gpuMs` with `markMs`, `uploadMs`, `renderMs`, `soundMs`,
`backMs`). Doom in the world (editor, 44 Unity frames and 22 lists a second): the guest 8.8 ms a
frame, `Gpu()` 6.0, display 1.0, sound 0.9, readback 0.6. A list costs 12.2 ms: 10.1 deciding
which 32 KB rows it reads (`Command`, `Texture`, `Mark`, an Udon walk of every command and glyph),
1.4 uploading them (39 rows, 1.25 MB), 0.6 drawing. The upload itself is cheap: 2 MB is 0.13 ms
of `Array.Copy`, 0.08 of `BlockCopy`, 0.15 of `LoadRawTextureData` and 0.25 of GPU time. Doom's
data in the GPU window is 154 rows (4.8 MB), spread from 0x86000000 to 0x87b40000.

1. Drop the walk: upload the used part of the window with every list, its extent from the GPU
   allocator's high-water mark (never a scan of `mem` in Udon). 4.8 MB is about 1.5 ms against
   10.1. `Upload` copies with one `Buffer.BlockCopy` from `mem` into the stage's bytes.
2. Lay the window out by how often it changes: the allocator (`/dev/gpu`'s mmap) hands memory
   out from the bottom, textures, models, fonts and glyph tables in a static region and the
   lists, vertices and uniforms of a frame in a streaming arena at a fixed place.
3. Upload the streaming arena with every list and a static row only when written: the native
   `memcpy`, `memset` and `memmove` mark the rows they write, and a translated store marks its row
   only when it falls in the static region. Doom's list would then be a few hundred KB.
4. Measure with the frame's parts above: Doom's device work is to go from about 7.9 ms a frame to
   about 2.5, giving the guest about 1.6 times the time at the same frame rate.

## In the world (`ShaderEmuWorldUdon.unity`)

A copy of the world has the Udon machine beside the shader one (menu "ShaderEmu/Add the Udon
machine to the open scene", `unity/ShaderEmu/Editor/ShaderEmuUdonCpu.cs`; the driver is
`Scripts/EmuUdonCpu.cs`). It uses the shader machine's screen, terminal, keyboards and sound card:

- The display, input and sound words lie in the Udon program's RAM at 0x87000000 as in the
  shader machine's. Each frame the driver gathers what the display shader reads (the control
  words, the pixels of modes 1 and 2, mode 4's layer table and layers) into a staging texture,
  packs it into a state texture of its own and draws it with the display shader at `_StateRow`.
- Sound: the voices are gathered the same way and mixed by the sound card's shader; its cursor
  is read back with `Material.GetInt`.
- The GPU (`docs/machine/gpu.md`): when a list is submitted the driver marks the rows of 0x86000000
  to 0x88000000 it reads (the list, the control words, each command's vertices, uniforms,
  textures, tiles and glyphs), uploads those 32 KB rows into a state texture of its own
  (`UdonAotPack` with `_DstRow`, the other rows kept), and has the world's GPU camera draw it with
  `GpuDraw` at `_StateRow` -3072. A copy the submit asks for is the picture through
  `UdonAotGpuBack` (bytes that read as `0x00RRGGBB`) and `VRCAsyncGPUReadback` into RAM a row an
  `Array.Copy`; then the submit word is taken back and the counters move. Mode 3 shows the target.
- The pointer, the keys and the host's clock are written into their words.
- Time: the guest's timer (5000 Hz) is real time since power on, read from a stopwatch at every
  slice; a `wfi` ends the frame unless the timer is due. The clock chip's two words are the host's
  local time in BCD (the harness's DS1742), set every frame. `rdcycle` and `rdinstret` are the
  instructions run: a counter each translated block adds its length to, and the interpreter's
  (4% of the shell check's time: 8.25 s against 7.91). The bar shows them (0.1 M idle, 5.4 M busy).
- Fetches (`docs/system/fetch.md`): `host:name`, `power:off`, `persist:` from `EmuPersist`'s
  PlayerData (the same kept folder as the shader machine's), https pages through
  `VRCStringDownloader` from the addresses the machine has (its fields and the world's sites), and
  pictures through `VRCImageDownloader`, turned into the answer's words by `UdonAotFetch.shader`
  (pixels over white, or a file carried in them) and read back 256 KB a part.

The prompt comes 4.3 to 4.7 s after power in the editor and 6.2 to 7.0 s in the VRChat client
(Build & Test). In the client, `dd` to `/dev/fb0` was shown at 640 x 480, `cat` to `/dev/sound`
played (5,117 mixes, 4 late), and nano-X with a window came up 4.1 s after it was started. In the
editor `NANOX_SOFTWARE=1 nx` draws the whole desktop; nxview decodes the 1623 x 1080 wallpaper in
768 ms on its one core.

With the GPU (editor): glxgears on the whole display draws 50 lists a second (one a frame), lit
and in its colours; nano-X without `NANOX_SOFTWARE` says "GPU drawing", and demo-hello's window
is the software one's to the pixel (0 of 26,574 differ). The whole desktop draws so, its icons'
labels in the image's own font (software mode had none). glxgears in a window on it: 23 lists
drawn and copied back a second, two Unity frames a list (its caption: 23 fps, 42.9 ms). In the client
(Build & Test): the window system in GPU mode 1.2 s after it was started, demo-hello's window
copied back, glxgears in a window 25.7 copies a second.

Devices, editor: `date` is the host's time and `sleep 5` five seconds; `nxpersist save`, then
`load` brought a deleted file back (187 bytes kept); `open --dry https://example.com/` 577 bytes,
a page; an 800 x 600 PNG of Wikipedia's came whole through the readback (19,291 of 480,000 pixels
a channel 1 away from Python's PIL over white, none more), and the guest's file has the md5 of
the same shader's output made in the editor. Client: the date and `sleep 5` right, the player's
name, `persist:state`, a page of creators.vrchat.com and a PNG of the world's sites.

The desktop's start (GPU mode, editor), sampled each second: the bar 7.0 s after `nx`, then the
machine busy without an idle frame until 19 s (61.5 M instructions, 3.2 MIPS, most of them
nxfiles drawing its icons), then idle at 0.17 M a second. It is the programs' own work. With the
C++ translator and its profile the desktop is idle at 16 s.

## What it would take

- Worker cores are not there: Udon runs on the main thread only, so a core more would be the same
  thread's time shared, no faster; programs ask for none and run on the first.
  The GPU sees only 0x86000000 to 0x88000000 and the ROM: a texture elsewhere in RAM draws as
  zeros, and glyphs of distance fields from RAM are not uploaded.
- `rv2udon.py` has no traps, CSRs or interrupts; an unknown pointer to a device throws there.
- Udon's time is the main thread's, so a frame's few milliseconds bound it, where the shader
  machine's cores are not.
