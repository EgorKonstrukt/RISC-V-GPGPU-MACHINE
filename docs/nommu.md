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
- `bash linux/nommu/kernel.sh`: the MMU kernel's tree copied with its hooks, three patches
  (`patch_signal.py`, `patch_drivers.py`, `patch_ovl.py`) and its configuration without the MMU,
  into `build/nommu/Image`.
- `bash linux/nommu/build.sh [script ...]`: the sources copied to `~/nm/repo` again and the
  repository's build scripts run there with the flat drivers, a line each.
- `bash linux/nommu/root.sh`: mctest and the machine's `elf2flt`, then every program of the root
  made flat (`flat.py`, `elf2bflt.py`) into `build/nommu/flatroot`.
- `python linux/nommu/image.py`: `build/images/nommu`, the shared image with that root as its
  overlay, the kernel, and RAM in the device tree from the kernel's address (0x80400000) on.
- `python linux/nommu/test.py`: `tools/test_all.py` on that image, its workers bare.

`pwsh linux/nommu/all.ps1 [-Kernel]` is the whole round. The machine's own `cc` links a flat
program as a library and makes it flat with `elf2flt` (`linux/nommu/cc`). `linux/nommu/card.ps1`
times the games on the card, a configuration a run (an image and the shader's defines); a Zb
image is `RV32_MARCH=rv32imaf_zba_zbb` through `build.sh` and `root.sh`, then `image.py nommu-zb`.

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

## Memory

RAM is what the MMU kernel has (to 0x86000000; 88.7 MB for the kernel, 68 free on the desktop),
and a program's memory is taken whole and in runs of pages: a mapping wants one run as long as
itself. Quake III is the largest, 46 MB in four runs (its data and bss 22, the hunk 8, the
server's zone 6, the zone 4). `python linux/nommu/memory.py` runs each large program and then
Quake III, and all of them one after another in order and reversed, a machine each under
`rvc_cpu`, in about two and a half minutes: on 16 October 2026 all twelve fit, Quake III with 15.6
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
