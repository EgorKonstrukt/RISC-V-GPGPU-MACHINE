# A C compiler in the machine

The image has a C compiler that runs in the guest and makes programs for it: TinyCC, from
jrrk2's `riscv32` branch (32-bit RISC-V). `linux/tcc/build.sh` builds it, after
Nano-X's build.

    / # cc hello.c -o hello                 # the C library, Nano-X and OpenGL ES are linked in
    / # example cube                        # compiles /usr/share/example-cube.c and runs it
    / # example home
    / # nxide                               # write, build, run and debug it in a window (docs/ide.md)

- `cc` is a script round `tcc -static`: programs here are static, as everything else in the
  image is. Its headers and libraries are files of the ROM, in `/usr/include` and `/usr/lib` (241 of
  them, 4.9 MB). They were one archive unpacked over the root at the first build, which was
  half of that build: 110 million instructions against 55 for the shader example in the
  world, where every page copied is a round.
- What a program gets: musl's headers and `libc.a`, `libnano-X.a` with `nano-X.h`,
  `libgles.a` with `GLES/gl.h` and `GLES/segl.h` (`programs/linux/gles.c` in its float build:
  OpenGL ES 1 on the machine's GPU, `docs/gpu.md`), and `libmcw.a` with `mcw.h`: the worker
  cores (`docs/multicore.md`, "Using the workers from a program"), for a program that shares
  its work out as `example shader` does.
- `cc -g` writes stabs, which the IDE's debugger reads (`docs/ide.md`).
- The C library's `sinf`, `cosf` and their like work in doubles, which are library calls
  here: a program that calls them for every pixel is seconds a picture. Write the few floats
  yourself (`wave` in the shader example).
- **Floats are instructions** (`docs/fpu.md`): `cc` passes `-mfpu`, and `linux/tcc/fpu_hook.py`
  limits that option to single precision, which is all the machine has. A `double` is still a
  library call each, and a constant written without an `f` is a double.
- TinyCC links its own runtime (`libtcc1.a`) last, after the C library, so compiler-rt's
  builtins (64-bit division, soft float, which the C library needs too) are put into that
  archive.
- The examples are `linux/tcc/examples/*.c`, in the image as `/usr/share/example-NAME.c`, in
  the Start menu's Programming and in the IDE's File menu. `hello` prints; `cube` is the
  smallest OpenGL program; `home` is a small place to walk round, a list of boxes drawn
  again only when the viewer moves; `shader` is a picture worked out a pixel at a time by
  255 worker cores. The OpenGL ones are floats throughout: `GL_FLOAT` arrays, `glRotatef`.
- Compiling takes about 7 seconds for either example (4 to 6 s of the processor: most of it
  is reading the libraries), the first time as any other.
- TinyCC does not optimise. A program that needs speed is still better built on the host
  with the cross compiler (`linux/userland/toolchain.sh`).

Not checked: programs that use `double` heavily, `long double`, and anything the branch's own
test suite skips (21 of 151 tests).
