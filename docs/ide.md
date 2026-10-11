# The IDE

`nxide` is a place to write C in the machine, build it with the machine's own compiler
(`docs/cc.md`) and run it, with a debugger. Start menu, Programming; a `.c` or `.h` file
opened anywhere is its (`/usr/share/nxopen.15-ide`, `docs/open.md`), and so is the explorer's
Edit of one.

    / # nxide                   # the hello example
    / # nxide FILE.c

    wsl -- bash /mnt/c/.../linux/ide/build.sh      # 6 s, after Nano-X's; then the image

## What it is made of (`linux/ide`)

- `nxide.c`: the window. A menu bar (File, Edit, Selection, View, Run, Help: `ui_menu.h`'s
  menus), a row of keys, the text with its line numbers, a panel below with four leaves
  (Output, Problems, Variables, Call stack) and a status line that also asks (Find, Go to
  line, Save as).
- `text.h`: the text and its history. Every change is put in or taken out through two
  functions, which keep what they did: Ctrl+Z takes a command back and Ctrl+Y makes it
  again, 1,024 changes deep; letters typed one after another are one change. A tab is
  spaces to the next fourth column, in the file too.
- `syntax.h`: the colours. A lexer, a line at a time: keywords, types, numbers, strings,
  comments, the preprocessor's lines, and a name before a bracket. The one thing a line
  needs from those above it is whether it begins inside a comment, which is kept for every
  line and worked out again from the line that changed.
- `debug.h`: the debugger (below).

## Colours, and why no language server

A language server is a compiler that never stops running, and a compiler here is the first
core's whole time: compiling the smallest program is 0.85 s, the `home` example 5.5 s (most of
it the OpenGL and Nano-X headers). So the two halves are apart:

- **Colours are the lexer's**, a few hundred instructions for a line, done as a line is
  drawn. A key redraws its line; the lines below it are drawn again only when one was added
  or taken out, or a comment was opened or closed.
- **What is wrong is the compiler's.** A second and a half after the last key, the text as
  it is goes to `tcc -c` beside the program (never while something is built or run), and
  what it says is marked: a red bar before a line with an error, an orange one for a
  warning, and the lines in Problems (a click goes there). View, "Check while typing" turns
  it off; a build always does it.

## Build and run

Build (F7) saves the file and runs `cc -g FILE -o /tmp/ide-NAME`: always with `-g`, since
TinyCC does not optimise either way. Its output is in Output and its errors in Problems,
with the first one's line shown. Run (F6) builds and then starts the program, whose output
comes to Output as it is written (a program with a window opens it as any does). Stop
(Shift+F5) ends whatever runs.

## The debugger

It is feasible, and it is line by line. Two things make it so:

- TinyCC's `-g` writes **stabs**: every line's address, the functions, and each variable
  with its type and where it is (a place from the frame's pointer, or an address).
  `dbg_load` reads them from the program's file; types are followed through pointers,
  arrays and structures.
- The kernel has **ptrace**. The machine has no single step, so a stop is an instruction that
  traps (a word of zeros: below, why not `ebreak`) written over a line's first instruction
  and taken out again when the program is held. To
  go on from a line, the instruction there is looked at (`dbg_one`): it can go to the next
  one or, a branch or a jump, to one other place, and an `ebreak` at each is one
  instruction's step.

What it does: F9 or a click before a line's number marks the line; F5 builds and runs to
the first marked line (with none, to the first line of `main`), and goes on from a stop;
F10 steps over a line, F11 into a call, F12 out of the function. At a stop the line is
yellow, Variables has the function's and then the program's (numbers, floats, characters,
pointers with the text they point at, the first eight of an array, a structure's fields),
and Call stack the functions it is in (TinyCC keeps the return address just under the
frame's pointer and the caller's frame under that). A signal stops it too, at its line.

- A step's stops are in the one function being stepped and where it returns to (the
  return address is just under the frame's pointer), never anywhere else. Over: that
  function's lines. Out: the return address alone. Into: an instruction at a time until
  the line is another; at a call of one of the program's own functions, on to that
  function's first line, and a call of anything else is stepped over.
- **The worker cores run the same memory.** A stop is an instruction changed, for every
  core: the first version put one on every line of the program for a step, and a step
  taken in `main` while the workers shaded stopped them in `shade` (the debugger then
  showed a line of `shade` with the first core's registers: nonsense). The debugger holds
  the first core's thread alone. A mark on a line the workers run cannot be a stop for them, and it
  ends the program cleanly: the stop is an instruction that is none (a word of zeros), which
  is SIGILL for the first core's thread and a trap for a worker, whose library then ends
  the program with a line saying which core and where; the IDE says a worker came to a
  marked line. (It was `ebreak` at first, which on a worker core means "park": the worker
  went to sleep in the middle of its job and the program waited for it for ever.)
- A function's first line (its name) and its closing brace are not places to stop: at the
  first the frame is not made, and the brace's address is not an instruction's. A mark
  there is the next line's.
- Not there: watching a variable, changing one, stopping a program that is running (Stop
  ends it), and anything about code the compiler's libraries are made of (they have no
  stabs: a step into `printf` is a step over it).

## The examples

File, Examples lists `/usr/share/example-*.c` (`linux/tcc/examples`). One is opened as the
user's own copy, `/root/NAME.c`, which is written at the first Save or Build.

| | |
|---|---|
| `hello` | prints, with a function to step into |
| `cube` | the smallest OpenGL program |
| `home` | a place to walk round |
| `shader` | a picture every pixel of which is one function's, on the worker cores |

`shader` shares a 160 x 120 picture out in 240 pieces of half a row, about one a worker
(`mcw.h`, which `cc` links now: `docs/multicore.md`), and draws it as one texture: 20
pictures a second with 255 workers on the shader machine. Its waves are its own few floats
and not `sinf`: the C library works a sine out in doubles, each a call of hundreds of
instructions here, and with it a picture took seconds.

## Checking it

`NXIDE_PRINT=1` prints what happens (`nxide: ...`, the program's output after `nxide|`,
and at a stop every variable and the call stack), and `NXIDE_SCRIPT="..."` does commands
one after another, each when nothing is being built or run: `build run debug go over into
out stop undo redo save quit comment all indent`, `mark:LINE`, `line:LINE`, `type:TEXT`
(`_` a space, `|` Enter), `text`, `problems`, `wait`. Under `rvc_cpu`, with `nano-X -p &`:

    NXIDE_PRINT=1 NXIDE_SCRIPT="mark:9 debug over over out into go quit quit" nxide

must stop at line 9 in `square` with `n = 1`, then line 11 with `result = 1`, then line 21
in `main`; and `NXIDE_SCRIPT="line:16 type:int_broken_=_; build problems undo text run quit
quit" nxide /root/hello.c` must name an error at line 16, give the text back as it was, and
print the program's six lines. Two seconds for both.

More of the script, for what those two do not reach: `example:NAME`, `open:PATH`, `new`,
`find:TEXT`, `key:NAME` (`up`, `s-down` with Shift, `c-a` with Ctrl, `enter`, `f9`...),
`menu:N` (opens and closes a menu), `cut copy paste word unindent panel numbers`, `where`,
and `storm:N`: N keys and commands by a fixed sequence of chances, through the functions a
hand's go through, with the cursor and the selection checked after each; then every change
taken back must leave the text as it was and every one made again as the storm left it. A
change to `text.h` or to the keys is tested with

    NXIDE_SCRIPT="example:home storm:600 example:shader storm:500 example:cube storm:500 quit quit" nxide

(20 s under `rvc_cpu`), which also opens one file after another: the first build crashed at
the second file opened, its text put in at the first one's cursor. With `NXIDE_PRINT` every
command prints where it left the cursor and the history, and at its start the IDE prints
where its keys and menus are across the window, for a test that clicks (`nxkey X,Y`: the
window's client begins 3, 23 inside its frame). A program that opens a window covers the
IDE's keys where it lands: a test that clicks Stop runs a program that has none.

The IDE is also tested started as the desktop starts it, its children's ends ignored
(`sh -c 'trap "" CHLD; NXIDE_PRINT=1 NXIDE_SCRIPT="build quit quit" exec nxide'`): the bar
ignores SIGCHLD, a started program inherits that, and the first IDE never saw its build end. The window itself is a snapshot's layer:
the stopped line is `ffff96`, a mark `c81414`, an error's bar `dc0000`.
