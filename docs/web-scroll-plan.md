# Plan: the browser's scroll without stutters

Since built, the ring of strips made by worker cores (`strips_work` in `nxweb.c`; `docs/web.md`,
"Strips"). What follows is the plan as it was written.

Written 13 October 2026. Nothing here was built or measured then; it was a plan from reading
`nxweb.c`, `scr_shaderemu.c`, `docs/gpu.md`, `docs/nanox.md` and `docs/multicore.md`. What
the reading did not settle is under "To find out first".

## What stutters now

A scroll step is cheap: one copy of the view out of the page's pixmap and the scroll bar.
The stutter is `draw_rows()`, which runs when the view leaves the rows the pixmap holds
(`show_view` in `nxweb.c`):

- the whole pixmap is drawn again, though in the Unity world about two thirds of its rows
  were already there (720 rows, a view of 376, so a redraw every 260 rows or so);
- every item of the page is walked to find those in the pixmap;
- each text is three requests over the socket, and the server looks up a glyph for every
  character (`rom_glyph`, about 130 instructions) before it writes the GPU's word for it;
- all of it is on the first core, in the loop that also makes the scroll's steps.

## The idea

A page's drawing is not pixels the processor makes. It is a GPU command list: a rectangle a
box, one text command a string, a word a character (`docs/gpu.md`). Today the server writes
that list from the browser's requests. The browser can write it itself, and that writing is
plain computing and stores, which is what a worker core is for.

Three parts:

1. **The pixmap is a ring of strips.** The page is cut into strips of 64 rows. Strip `k` of
   the page lives in slot `k mod S` of the pixmap. A scroll never draws the pixmap again; it
   only needs the few strips that come into reach, and the rows already drawn stay. The view
   is one copy, or two where the ring wraps.
2. **A strip is its own list, in its own coordinates.** A submit can copy the drawn picture
   to any address (`0x87000050`, bit 2 of the submit word). So a strip's list has its items
   at `y - 64k`, vertices counted from 0, and it is copied to wherever the slot lies.
   Nothing in a list depends on the scroll's position, so nothing is patched during a scroll.
3. **Workers make the strips ahead of the view.** A job is: find the strip's items, write
   their commands and strings into GPU memory, submit. The first core only says which strips
   are wanted and copies the view.

The server stops being part of drawing a page. Its requests, its glyph lookups and the
socket leave the scroll altogether.

## A strip job

Input: a strip number and a slot. All of it runs on a worker, or on the first core when
there is none (`mcw_post` already runs a job on the posting core then, so it is one code path).

- **Its items** come from an index made once after the layout: for each strip, the numbers
  of the items that touch it, in drawing order. An item that spans strips is in each.
- **A box** is a rectangle command; a framed box four more; underline and strike-through
  one each.
- **A text** is a text command and a word a character: the glyph's number and how far right
  it starts. The browser already maps the font files for their advances (`web_advance`), so
  the glyph is `sef_glyph` on the same bytes. A table of each face's first 256 glyphs, like
  the one for advances, takes the lookup from about 130 instructions to a load.
- **Italic** is the slant word; **Find's highlight** and the **picked link's frame** are a
  rectangle and a text more.
- **The first command** fills the strip with the page's colour, in the opaque pass. Text of
  coverage goes in the blended pass with everything after it, as `romfont_draw` does.
- **Submit** as `gles.c` does with no system call: the lock word, the list's registers, the
  copy's rectangle (the slot's address in the pixmap). The job then ends its passes with
  `mc_end_pass()` until the drawn count moves, and marks the strip as there.

Memory is bounded and does not grow with the page: each slot has a fixed piece of GPU
memory for its list and strings (about 24 KB for 256 commands and 2,000 characters; eleven
slots are 264 KB). A strip that needs more is drawn the old way, by requests.

Stores bound a job, not instructions: a text of 40 characters is 224 bytes, and a worker
keeps 1.5 to 3 KB a pass. A dense strip is a few passes of one worker, so several strips are
given to several workers at once. The GPU draws one list a pass, so strips land one a pass.

## What stays with the server, at first

- **Pictures that were fetched** (not in the ROM) and **form fields**: drawn by requests on
  the strip once its copy has landed (the copies' count at `0x87000038`).
- **Pictures in the ROM** can be a rectangle of the list (fragment mode 4) once the browser
  finds their address as `rom_address` does. Later.
- **The window's own parts**: menus, the bar, the status line, the scroll bar.
- **No ROM fonts, no GPU pixmap, `NXWEB_DIRECT`**: the whole old path, unchanged.

## Steps

Each step leaves a working browser and is checked before the next.

| # | What | Checked by |
|---|---|---|
| 0 | Measure the stutter as it is: a long page, `NXWEB_TIME=1`, the longest gap between two steps across a redraw; the server's share counted apart | numbers to beat |
| 1 | The ring of strips, still drawn by requests: a strip that comes into reach is drawn alone, ahead of the view, between events | same pixels as today at every scroll position; the longest gap |
| 2 | The strip job in the browser, run by the first core: its own list, submitted directly | strips pixel for pixel against step 1 (`NXWEB_STRIPS=requests` keeps step 1 in the same binary) |
| 3 | The job on borrowed workers (`mcw_lend`, as `ui_bulk.h`) | same pixels; pages brought for the workers' start 0; a run with no workers and one beside a program that holds them all |
| 4 | The scroll's steps on a worker too: it reads the goal, moves a third of what is left by the host's clock, and copies the view into the window's buffer by a list of its own | a scroll while the first core is busy (a page loading): steps keep coming |
| 5 | Loading on workers: the sheets' rules beside the tree, styles by subtrees | instructions on the first core to a page's first picture |

Steps 1 to 3 are the cure for the stutter. Step 1 alone already cuts a redraw to one strip
and moves it off the scroll's path, but on the first core. Step 4 matters only if the first
core's own work still shows in the scroll after step 3. Step 5 is about how long a page
takes to appear and does nothing for scrolling.

## Rules a job must keep (`docs/multicore.md`)

- **One writer for sixteen bytes.** Each slot's memory, and each job's flags, are whole
  texels of their own (`aligned(16)`).
- **Memory a worker stores to is touched by the first core first**, GPU memory too, and the
  job's code is read first. The count of pages brought for workers must be 0.
- **No `malloc`, no `stdio` in a job.** Everything it writes is laid out before it starts.
- **A job reads the layout's items and the page's text and writes neither.** A new layout
  (a resize, a picture's size arriving, a `<details>` opened) waits for the jobs out, then
  marks every strip as not there.
- **A list in flight is not written.** A slot is given to a new strip only after its last
  list was drawn.
- **A job that waits ends its pass** with `mc_end_pass()`, never the pause hint.
- **Borrowed, and maybe none.** A game keeps its workers; the browser takes what is left.

## To find out first

- **Where the lists' GPU memory comes from.** The last 4 MiB are one OpenGL program's
  (`seglMemory`), and the pool belongs to the server. The likely answer is a second pixmap
  asked of the server and used as plain memory, which needs its address.
- **Whether `GrGetWindowInfo` answers for a pixmap** with the address of its pixels, as it
  does for a window (`surface_address`). If not, the server needs that one addition.
- **How a second program's direct lists sit with the server's.** The server draws on the
  same pixmap by lists that begin with the buffer as a texture and are copied back; a strip
  must not be submitted between such a list and its copy. `gles.c`'s way of taking its turn
  (`SEGL_DRAW_CLAIM`, the lock word) is to be read before step 2.
- **Whether a copy may land past the GPU's target height.** If a strip's list is only 64
  rows of the picture, the pixmap need not be held under 720 rows in the world any more;
  today `pixmap_map` refuses a taller one. More rows in the ring is more room ahead.
- **Strip height.** 64 rows gives the world eleven slots, seven under a view at worst and
  four ahead. To be chosen by step 1's numbers, not here.

## How it is checked at the end

- **Pixels**: every strip against the old path's drawing, on Hacker News, Lobsters,
  Wikipedia's main page, `web-long.html` and `web-layout.html`, scrolled by each means
  (wheel, page keys, End, the bar's thumb), with the harness's target and the world's
  (`docs/web.md`, "A scrolled page is checked with the world's target").
- **Motion**: on the card through `tools\gpu_queue.ps1`, a long scroll on a long page: the
  longest gap between two steps before and after, and whether every step's picture was in
  the window before the next.
- **Others**: glxgears' frame lines beside a scrolling browser, as `AGENTS.md` measures the
  window system; nxray still gets 255 workers when it asks.
- **By eye**: the developer, in the world. The page with the captured frames comes with the
  report.

## What was built (15 October 2026)

| # | | What came of it |
|---|---|---|
| 0 | measured | `NXWEB_TIME=1` says, for each scroll, the longest gap between two steps in the guest's ms and in the first core's instructions, and how much a step was the browser's own; `NXWEB_ROWS=720` holds the pixmap to the world's rows. A redraw was 112 to 364 thousand instructions between two steps (`docs/web.md`, "Strips") |
| 1 | built | the ring of strips, drawn by requests on a pixmap of 64 rows and copied into a slot, so a strip's text reaches no other slot; `NXWEB_STRIPS=requests` |
| 2 | built | a strip's own list, submitted by the browser; `NXWEB_STRIPS=here`. Pictures the image keeps are in it too (mode 4 from the ROM), which this plan had for later: Wikipedia's main page needed it |
| 3 | built | the lists made by up to four lent workers. Every list strip the same pixels as by requests (`NXWEB_CHECK=1`, on four pages), races and workers' faults 0, `nxray` beside it still has 255 workers |
| 4 | not built | its condition is not met: a step's own work is a thousand instructions, and what is left between two steps is the window system's copy. And a worker cannot submit a list: the lock word does not keep a worker and the first core apart in one pass (an atomic swap is no exclusion between cores, `docs/multicore.md`, "Races"); it would want a submit word of its own |
| 5 | half built | styles by subtrees on workers (`docs/web.md`, "Styles on the worker cores"): a page's first picture 14 to 21% fewer instructions of the first core. The sheets beside the tree are not: both write one table of atoms |

What "To find out first" found:

- **The lists' memory** is a second pixmap of the server's, used as plain memory (24 KB a slot).
- **`GrGetWindowInfo` on a pixmap** answered no address: the server has that addition now
  (`microwindows.patch`, `surface_locate` answers 0 for a pixmap in the server's memory).
- **A second program's lists beside the server's**: any order is right, since every list reads
  the pixmap when it is drawn, after the copies of those before it, and the copies are made in
  the order the lists were. The one wrong order is the server's drawing on a slot still in its
  queue when the browser submits a list for the same slot: before that, the browser reads a
  pixel of the pixmap, which the server answers once its drawing is in.
- **A copy past the target's rows**: a strip's list is drawn into the target's top 64 rows and
  may be copied anywhere, but the server still draws on the pixmap whole (a strip by requests,
  a new pixmap's black), so the pixmap stays under the target.
- **Strip height** stayed 64: 11 slots in the world, the view's six or seven and four ahead
  (256 rows). A page key moves 360, and the rest are made between the scroll's first steps:
  after a page's first view no measured scroll but a jump waited for a strip of the view.
