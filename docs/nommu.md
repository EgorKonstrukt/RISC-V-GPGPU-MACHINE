# Linux without an MMU

The same system and programs on a kernel built without the MMU (`CONFIG_MMU` not set): every
program is a flat (bFLT) one loaded whole into RAM, and the shader machine can run with no page
walk at all (`NO_PAGING`). `linux/nommu/` builds it beside the MMU build, which it does not change.

## Building it

In WSL, after the MMU build (`linux/kernel/build.sh` and the programs' build scripts):

- `bash linux/nommu/setup.sh` once: `~/nm/work`, a build folder of the MMU one's sources with
  nothing compiled, a copy of the built root in `~/nm/repo`, and compiler drivers (`rv32-cc`,
  `rv32-c++`) that compile every program position-independent and link it as a flat program
  (`flatcrt.c` is its start). `fork` is `vfork` there (`nommu_fork.h`, included in every file).
  `RV32_MARCH=rv32imaf_zba_zbb` builds with Zba and Zbb.
- `bash linux/nommu/kernel.sh`: the MMU kernel's tree copied with its hooks, five patches
  (`patch_kconfig.py`: RV32 without the MMU, still in supervisor mode under the machine's SBI and
  linked at 0x80400000; `patch_signal.py`, `patch_drivers.py`, which also keeps `/proc/cpuinfo`'s
  "mmu" line out, `patch_ovl.py`, `patch_console.py`: the console in memory) and its
  configuration without the MMU, into `build/nommu/Image`.
- `bash linux/nommu/build.sh [script ...]`: the sources copied to `~/nm/repo` again and the
  repository's build scripts run there with the flat drivers, a line each.
- `bash linux/nommu/root.sh`: mctest and the machine's `elf2flt`, then every program of the root
  made flat (`flat.py`, `elf2bflt.py`) into `build/nommu/flatroot`.
- `python linux/nommu/image.py`: `build/images/nommu`, the shared image with that root as its
  overlay, the kernel, and RAM in the device tree from the kernel's address (0x80400000) on.
- `python linux/nommu/test.py`: `tools/test_all.py` on that image, its workers bare.

`rvc_harness` with no arguments boots this image once it has been made, on the machine without
paging (`NO_PAGING`), with its window, desktop and sound. `--kernel mmu|nommu` and `--isa base|zb`
choose the kernel and the instruction set (`zb` takes `build/images/nommu-zb` and the shader's
`ZB`), as `k` and `i` do in the start menu; `--image linux-net` alone is Linux with the MMU.

`pwsh linux/nommu/all.ps1 [-Kernel]` is the whole round. The machine's own `cc` links a flat
program as a library and makes it flat with `elf2flt` (`linux/nommu/cc`). `linux/nommu/card.ps1`
times the games on the card, a configuration a run (an image and the shader's defines); a Zb
image is `RV32_MARCH=rv32imaf_zba_zbb` through `build.sh` and `root.sh`, then `image.py nommu-zb`.

## In the world

The VRChat world runs this Linux and no other: `MachineTick.shader` defines `NO_PAGING` (with
`SBI_HLE` and `FPU`, no `ZB`), and "ShaderEmu/Import boot images from the repository" takes the
"linux" image from `build/images/nommu`. After a no-MMU build: `python linux/nommu/image.py`,
then "Sync shader sources from the repository" and "Import boot images from the repository".

## What differs

- The kernel: the GPU's and the sound card's memory are mapped where they lie; a large mapping
  takes the first run of free pages long enough, not a buddy block, and drops the page cache
  before it fails; the kernel's early allocations come from the bottom of RAM; a ROM file is
  mapped from the ROM through overlayfs; a signal's trampoline on the stack is made visible to
  the instruction fetch; a program in the ROM is copied into RAM by one parallel copy.
- A program's whole memory is taken at its start: its data, bss and stack, and every anonymous
  mapping, cleared. A worker program gives its workers' stacks back when it closes them
  (`mcw_close`); a large pool is mapped at its size, not `calloc`'d (Quake III's zone and hunk).
  Quake III's pools are what q3dm17 takes with headroom (`build.sh`): the hunk 8 MB (5.3 at
  most), the zone 4 (42 KB), the server's 6 (3.2 with seven bots); `meminfo` says what is used.
- The shader machine: `BARE_WORKERS` starts a worker whose page table root is 0 with no
  translation; `NO_PAGING` takes every address as physical. `ZB` runs Zba and Zbb;
  `X_NO_FAST_AMO` sends atomics to the general path (for measuring).
- `rvc_cpu`: `RVC_BARE_WORKERS=1` as `BARE_WORKERS`, `RVC_ZB=1` as `ZB`, `RVC_GPU_COPY=1` marks
  every pixel of a drawn list's copy into RAM and names each new target.
- Programs: libmedia makes a picture in memory and writes it where a file cannot be mapped for
  writing; nxfiles knows a flat program; tcc copies a section that cannot grow in place; the
  NES reads its leave word PC-relative.

## The state

With `NO_PAGING` and `SBI_HLE` (the world's machine, and `rvc_harness`'s for this kernel) the
CSRs Linux uses are fields of core 0's first three texels: mstatus, mie, mip, stvec; sscratch,
sepc, scause, stval; scounteren and fcsr (`CSR_FIELDS` in `types.h`, `csr.h`). There is no CSR
table under the state rows and no CSR cache, so a CSR write never stalls; the machine's own
CSRs read as 0 and a write to one is dropped. `NO_PAGING` also leaves the TLBs' 409 texels
out: the float registers follow the write cache, and the tick draws 8 of core 0's 16 rows
(`CORE0_ROWS`), 500 texels, a worker's layout with a cache of 2^6 (8 x 8 pixels of the tick's
targets; 9 rows and 564 texels before the cache's third table was a quarter, `docs/multicore.md`). `CSR_TABLE` and
`CORE0_TLB_ROWS` compile the old ones. On the card (D3D11, fxc2, 17 October 2026, two runs
each, alternating): a shell loop 4,889k instructions a second before and 5,025k after (+2.8%),
Doom's timedemo 1,176 realtics before and 1,165 after (+1%); mctest with 255 workers, fptest and
Doom's state sums as before. Unity's commit (`MachineCommit.cginc`) has the tick's defines.

`CSR_FIELDS` also leaves out what only the MMU machine has:

- A trap or interrupt goes to supervisor mode only: no delegation, no machine-mode entry.
  Interrupts are SEIP, SSIP and STIP.
- No UART and no CLINT. The console is memory the kernel reads and writes itself
  (`docs/console.md`, "Words in memory"). The timer is the host's time for the pass against
  SBI `set_timer`'s compare, looked at once a pass and at the call.
- The RTC is read from the host's uniforms.
- No `mmu` words, no single step, no debug words but the last instruction and stall.
- The copy's destination for the commit's geometry comes from texel 37 (no `memop_*_p`).

The tick's device reads and writes are the MMU machine's still (`mem.h`), but for the RTC: the
paths cut down were 2 to 4% slower a pass under fxc. On the card, against the tree before
(17 October 2026): 3,000 full passes from one Quake III snapshot (q3dm17 with three bots,
frame 100) took 5.32 s against 5.56, means of two. Whole runs of a game vary 4% from run to
run and did not tell the two apart. mctest with 255 workers, fptest, doom-1, nxray's sums and
the boot's clock were as before.

## Memory

RAM is what the MMU kernel has (to 0x86000000; 88.7 MB for the kernel, 68 free on the desktop),
and a program's memory is taken whole and in runs of pages: a mapping wants one run as long as
itself. Quake III is the largest, 46 MB in four runs (its data and bss 22, the hunk 8, the
server's zone 6, the zone 4). `python linux/nommu/memory.py` runs each large program and then
Quake III, and all of them one after another in order and reversed, a machine each under
`rvc_cpu`, in about two and a half minutes: on 16 October 2026 all twelve fit, Quake III with 21.4
MB free at the least. Over 26 programs one after another the shell grew 25 KB, Nano-X 29 KB and
the slab 156 KB. What a program leaves in a tmpfs file stays where it lies until the file goes:
Red Alert and Tiberian Dawn run from a folder of their own in the ROM, no copy in `/tmp`.

## Speed on the card

D3D11, fxc2, 16 October 2026; one run a cell, guest time (repeated runs varied by 1 to 3%):

| Program | MMU | no MMU, `NO_PAGING` | and Zba/Zbb |
|---|---|---|---|
| Doom (timedemo demo1) | 129.1 fps | +11.2% | +13.2% |
| Quake (timedemo demo1) | 80.7 fps | +14.6% | +17.2% |
| Quake III (q3dm17, 3 bots, frames 101 to 300) | 20.9 fps | +15.6% | +17.9% |
| nxray (8 pictures 320 x 240) | 26.1 a second | +16.4% | +14.1% |
| Red Alert (game frames 300 to 900) | 75.6 a second | +15.9% | +14.0% |

Zba and Zbb take 1 to 2.5% of the instructions away (the programs built with them, musl not);
over `NO_PAGING` alone they are within the noise. Without `NO_PAGING` the no-MMU kernel gave
Doom 5.5% over the MMU one. On the card the float programs' sums are the card's (its division),
the same with either kernel.
