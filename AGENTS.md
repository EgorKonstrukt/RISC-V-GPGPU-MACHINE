- `tools\make_image.ps1` passes its arguments on to `make_linux_image.py` (at first it did not: `$args` inside a `Measure-Command` block is the block's own): `pwsh tools\make_image.ps1 --save-prebuilt` saves the prebuilt folder under the lock.
- Tiberian Dawn makes a unit's second path search on a worker core while the first core makes
  the first, and does not run a search that must come out as one already made (`docs/tdawn.md`,
  "A unit's path searches"). Code a search reaches (`Can_Enter_Cell` of units and infantry,
  `Find_Path` and what it calls) must store nothing outside `ShaderEmu_Core[who]`, and a new
  use of the threshold other than `Passable_Cell`'s comparison breaks the reuse:
  `TDAWN_CHECK_PATHS=1` must say `0 of them wrong`. A counting build runs with
  `TDAWN_SCENE=inline`, which has no workers.
- A worker stops at every page of the program's own code that no core has run yet, and every
  worker that comes to it stops too (cause 12): nxphys had 921 such stops in its first substep
  with 255 workers, and a third stack page each. A program that starts many workers on code
  the first core has not run reads its text first (`take_workers` in `nxphys.c`), and
  `STACK_TOUCHED` in `mcw.c` is three pages. `PHYS_COUNT=1` prints how many pages were brought
  for workers: it must say 0.
- nxphys's shares are its grid's cells by remainder: fewer shares than workers with the same
  fullest share were tried (the fewest with the least full one) and were slower, 9.1 ms a
  substep against 6.1; how full a share is does not say how long its substep is.
- The two Command & Conquer games decode a mission's shape files ahead on a worker in the
  first few hundred frames: a frame summed over all cores is measured from frame 301 on.
- An idle machine ran the whole tick path about 48 times a second only to poll its input:
  the idle loop re-armed the driver's timer at the next tick. It now does so for a second
  after any key, button or pointer move, and every fourth tick on a machine nobody has
  touched (`IDLE_POLL`, `touched_at` in `shaderemu_input.c`): 209 thousand instructions a
  second idle became 125, an idle desktop 222 to 144, and a desktop under a moving pointer is
  as it was (577). Polling every other tick always was a third fewer samples of a moving
  pointer: not taken.
- nxphys asks for 207 workers (`WORKERS_ASKED`): a substep is as long as with 255 in its three
  scenes on `rvc_cpu` and on the shader machine (120 substeps of the wall 1,439 ms against
  1,442), and 48 cores are not turning in place. After a change to how bodies are shared
  out, measure 191 to 255 again (`PHYS_WORKERS`).

- Tick utilization is instructions run over busy passes times 8,192 (`pwsh tools\tick_util.ps1
  -Name N -Cmd "..." [-From TEXT]`, `docs/boot.md` "Passes that end early"): it prints what
  ended passes early by reason and place, the kernel's idle loop apart, and a WORKERS line.
  In the harness an early end costs 0.2 ms, not the rest of the pass (a pass is 0.2 ms + 0.24
  us an instruction); in the world's fixed mode it costs the instructions.
- The write cache is three tables (64 + 32 + 32 buckets of three). Its stalls were collisions
  at 50 to 80% full: another hash, or one that changes with a seed, did next to nothing in
  simulation, another bucket to try did (Doom 1,840 full-cache passes to 93). Four tables
  measured 0.6% slower.
- A port ends its frame with `seglSwapEarly()` and calls `seglSettle()` before it next writes,
  through a pointer, memory the list in flight reads (`docs/gpu.md`); each keeps a switch for
  the old wait (`QUAKE_SWAP`, `NES_SWAP`, `CLASSICUBE_SWAP`, `IMDEMO_SWAP`, `GL_SWAP` `=wait`),
  which gives before and after from one binary. A plain hold cannot show a store made too
  early: hold late (`QUAKE_HOLD_LATE=1` and the like) and compare the two pictures.
- One core keeps about 6 KB of distinct 16-byte texels a pass. A structure stored to every
  frame is laid out in texels by what is written together (Quake's `particle_t`); a table is
  not cleared whole at every page or frame (`css_reset` was 168 KB a page); a decoded piece is
  made where it will lie, not copied there (`piece_place` in `nxplay.c`); megabytes go to
  borrowed workers and are not waited for until the memory is next used (`ui_bulk.h`).
- The kernel's part (`linux/kernel/*_hook.py`, in `build.sh`'s order): cleared memory comes
  from a pool the idle loop fills (`zeropool_hook.py`; `zerotest` and `tailtest` must say
  PASS), the ROM is read ahead a window a copy and a whole ROM page needs no `fence.i`
  (`stall_hook.py`), romfs looks names up where they lie (`romfs_hook.py`), the console's
  driver stops at 62 characters a pass (`console_hook.py`), input is polled from the tick
  (`tick_input_hook.py`). A hook that replaces another hook's text must keep that hook's
  mark: run `build.sh` twice after adding one. A hook skips a file that has its marker: after
  editing a hook, `git checkout` the files it patches in the kernel tree first.
- `GrGetWindowInfo` does not wait for the GPU any more. A program that writes its window's
  buffer reads a pixel first (`GrReadArea`), as `nxmon` and `nxpaint` do.
- `rvc_harness --lazy` (`LAZY_MEMOP`): a fill or a copy, from the ROM too, is left to the
  commit and the pass goes on; its destination's pages are in no TLB until then. Off by
  default: it costs 1% of a shell loop and 3% of Doom though the fast loop is instruction for
  instruction the same (`tools\cso_loops.py`; README, "The same loop, compiled slower"). With
  it undefined the tick is the same blob. Its test: `programs\lazy`, 20 lines ok, sum
  `1a4a1226`, on `rvc_cpu` and on the shader machine off and on.
- A change to the tick is measured whole, alternating with the tree, on the shell loop AND on
  Doom's timedemo: they disagree (two SBI calls in the general path: level, and 1.9% under).
  Nearly every change to the tick's function, however far from the fast loop, has cost 1 to
  3% of every instruction; the tree sits at a level most changes leave.
- The kernel tick at 20 Hz with high-resolution timers is worth 5.5% of a game's frame at 2
  MIPS and was not installed: input is polled from the tick, so a key or the pointer would
  wait up to 50 ms; with the input poll kept on a timer of its own about 1% is left.
- The compiler's libraries are laid out for the ROM by `linux/tcc/pack.py` (`docs/cc.md`):
  what the examples take comes first in each archive and `mapped.c` asks for it in windows.
  After a change to an example, `gles.c`, `mcw.c`, Nano-X's library or musl run
  `linux/tcc/build.sh`. Reorder an archive's members, never its index, or every linked
  program's bytes change; `cc`'s outputs must stay the same bytes.
- `rvc_cpu` flushes a worker's TLB when it wakes for a new job: before, a program that freed
  memory its workers wrote gave wrong sums there and right ones on the shader machine.
- Every run on the card whose seconds or passes are measured goes through
  `tools\gpu_queue.ps1` (one at a time for every session); the shared image is made through
  `tools\make_image.ps1` (one writer at a time).
- Whether the window system lets other programs run is measured by another program's frames:
  glxgears on the desktop under `rvc_cpu`, its 5 s lines while a load runs (menus in Files by
  `nxkey`, letters into the editor, nxbench, programs starting, `--pointer-sweep 0`), as a
  share of its lines with nothing else. On 12 October 2026: 52 to 95% before, 81 to 97% after
  (menus 52 to 89, three programs starting 56 to 86).

- A program's first core with nothing to do until the next pass calls `mcw_rest()`
  (`docs/multicore.md`, "A core that rests"), not the pause hint: it is the `REST` ioctl of
  `/dev/gpu` (`_IO('G', 11)`, `shaderemu_gpu_rest`): the kernel yields, or does the idle
  loop's work and ends the pass. 322 instructions a call. Never from a job on a worker; with
  no workers open or on an older kernel it is the pause hint. `tools\tick_util.py` counts a
  pass that ended there apart (the `REST` line); the function must stay noinline with the
  pause inside it. `shaderemu_gpu.c` calls `zeroed_idle()`: it needs `zeropool_hook.py`.
- A page wanted of a mapped or executed ROM file brings the aligned 32 pages round it, a file
  of up to 32 pages comes whole, a file's last partial page rides in the same copy, and a lone
  piece under a page is copied by the processor (`linux/kernel/romwindow_hook.py`): the
  editor's first start went from 13 parallel copies to 3. `tailtest`'s sum covers
  `/usr/share/rom-dates`, which every image build writes anew: compare two kernels on one
  `rootfs.bin`.
- A breakpoint poked into another program sets a flag, and the kernel makes one `fence.i`
  when that program next runs (`linux/kernel/poke_hook.py`); a program poking itself still
  flushes at once.
- The shell vforks `command &` too, when every word of the line is plain text
  (`linux/userland/ash_vfork_hook.py`): a `$VAR`, a pipe or a redirection in the line is a
  fork again, with its copied pages.
- Before any timing on the card look for an `rvc_harness.exe` with no arguments (the
  developer's own): it holds the card at 100% outside the queue. A kernel build on all cores
  beside a run spoils a timing too.
- In WSL `$NAME` is the computer's name: a build script must not take an image's name from a
  variable called `NAME`.
- The display stream between players is judged while the picture changes, not by bytes or by
  the time to a settled picture (`docs/share.md`, "Judged while it moves"): record 300 frames
  with `rvc_harness --cpu --screen-log`, play them with `python tools/share_motion.py`, and
  hold two real clients' logs together with `python tools/share_clients.py` (`netTest`)
  (the tiles of before against the units, a packet every 0.15 s and every second), and a change to
  `ShareUnits.shader` must still give `tools/share_units.py`'s bytes (`bin\share_test.exe`,
  then `--against`), under fxc2 and under Windows' FXC: a `continue` in a loop, or a vector's
  component written by index in one, compiled under one and not the other.
- The pointer has a worker core of the window system's (`docs/nanox.md`, "The pointer on a core of its
  own"): the cursor and a dragged window follow it every pass, and the kernel reports no move
  inside the server's quiet rectangle. The core is lent (job, answer and "taken" words kept
  equal: the kernel takes it at once for a program that keeps workers). The test is a drag
  with the first core busy: where the layer is against the pointer, pass by pass (9 moves a
  second and 19 pixels behind before, every pixel and 1 behind now), and nxray must still say
  255 workers. A run's instructions shift half a percent with its timing: compare means of
  several timings (`sleep 0.3; ...`), never one run. What changes the window tree must raise
  `gd_scene_gen`, or the layer table and that rectangle are stale.
