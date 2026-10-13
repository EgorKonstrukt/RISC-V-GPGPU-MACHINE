# The browser

`nxweb` (`linux/apps/nxweb.c`) shows pages of the web as they are written: HTML read into a
tree, style sheets, text in the fonts of the ROM (`docs/fonts.md`), tables, boxes side by
side, floats, forms that search. It runs no scripts. Where its pages come from is in
`docs/fetch.md`; this is what it does with them.

    nxweb [ADDRESS or FILE]

Back, Home, Reload, an address to type. Keys: arrows and the paging keys scroll, Tab goes
from link to link (its address in the status line) and Enter follows, Backspace goes back, F5
reloads, Ctrl+L opens the address. A click in a text field types into it and Enter sends its
form; a click on a `<summary>` opens and closes what it folds away.

## The parts

| File | What |
|---|---|
| `css.h` | style sheets: rules read from text, kept by the last step of their selector, found for an element of the tree |
| `web.h` | the page: HTML into a tree, each element's style, addresses made whole, the fonts' advances, the sheets a page names, the copies the image keeps |
| `web_layout.h` | the tree laid out for a width: a list of things to draw at places of the page |
| `nxweb.c` | the window, the host, the pixmap the page is drawn in, the keys and the pointer |
| `tools/web_test.c` | the first three on the host, with no window system and no machine |

The first three have nothing of the window system in them. `tools/web_test.c` builds them
natively (`gcc -fsanitize=address -Ilinux/apps tools/web_test.c`) against the image's own
fonts and the cache of `web/preload.py`: it lays a page out in milliseconds, says how many
texts, boxes and pictures came of it, how many texts lie past the right edge or over another
text (both are mistakes of the layout's), lists every item (`--items`), says what an item is
inside and with what style (`--why X Y`), and draws the page into a PPM file. A change to the
engine is tried there on every cached page first: the machine then gives the same numbers.

## The tree and the styles

- **The tree** (`web_parse`): elements and texts, with what HTML leaves unsaid (an `li` ends
  the one before it, a `td` its neighbour, a block a paragraph left open). Text is kept as
  16-bit characters (UTF-8, or Latin-1 when the page says so), white space as written: the
  style says later whether a run of it is one space or a listing's lines and columns.
  `<noscript>` in the body is read as what it is for a reader without scripts.
- **A tag's attributes are walked once** (`web_note_names`): a bit for each name (by its first
  letter and its length) answers most later questions about an attribute at once, and the id
  and the classes are taken in the same walk.
- **Styles** (`web_style_all`): parents before what is inside them. What the tag says of old
  (`web_plain_style`: headings, lists, `bgcolor`, `width`, `align`, `cellpadding`, `<font>`),
  then the sheets' rules, the weakest first, then the element's own `style`. A font's size is
  applied before the rest, since an `em` of the rest is that.
- **A rule is not looked at when what it must be inside is not there**: the names (tags, ids,
  classes) of the elements round the one being styled are counted as the walk goes down and
  up (`css_inside`), and a rule with an ancestor step that names none of them ends there.
  Hacker News' sheet has a dozen rules that end in `a`: each was tried on each of 230 links,
  climbing to the root.

| Understood | |
|---|---|
| Selectors | tag, `.class`, `#id`, `*`, `[attr]` and `[attr=v]` (also `^= $= *= ~= |=`), descendants, `>` children, `+` and `~` siblings, `:root`, `:link`, `:first-child`, `:last-child`, `:only-child`, `:nth-child()`, `:empty`, `:not()` of one step |
| Left out, with their whole rule | states (`:hover`, `:focus`, `:checked`), `::before` and `::after`, `:is()`, `:has()`, nesting |
| Text | `color`, `font-size`, `font-family` (to one of three faces), `font-weight`, `font-style`, `font`, `line-height`, `text-align`, `text-decoration`, `text-transform`, `white-space`, `vertical-align` (sub, super, and a cell's) |
| Boxes | `display` (block, inline, inline-block, flex, grid, table and its parts, list-item, none), margins, padding, borders a side (width, colour), `background` (a colour), `width`, `height`, `min-` and `max-width`, `min-height`, `box-sizing`, `float`, `clear`, `visibility` |
| Rows and grids | `flex-direction`, `flex-wrap`, `flex`, `flex-grow`, `flex-basis`, `justify-content`, `align-items`, `gap`; `grid-template-columns` with `fr`, lengths, `repeat()` and `minmax()` |
| Values | px, em, rem, pt, %, vw, vh, `calc()` of a sum, `min()`/`max()`/`clamp()` (their first), `auto`; `#rgb`, `rgb()`, `hsl()`, names, `light-dark()`, `currentcolor`; `var()` with fallbacks; `!important` |
| At-rules | `@media` with widths (also `width <= N`), `screen`, `print`, `prefers-color-scheme` (light), `scripting` (none), `hover`; `@supports` and `@layer` are read as if true |

- Lengths are 13/16 of the page's: text of 16 pixels is 13 here, and a page is as if 704
  pixels wide in a window of 600, which is what media queries are told. A picture is that
  much smaller too, except a sign of up to 32 pixels, which stays pixel for pixel.
- A font's family is one of three faces: the first name of the list that is known says
  which (Arial, Verdana, system-ui: without serifs; Georgia, Times: with; anything "mono",
  Courier, Consolas: fixed). A page that names none is in the face with serifs, as in any
  browser.

## Layout

`web_layout()` gives items (`struct web_item`): a text in one face and colour, a filled or
framed box, a picture, a form's field. Their order is the order to draw them in.

- **Boxes one under the other**, margins between them collapsing (`web_gap`), a width when the
  style gives one, centred by `margin: auto` or `<center>`.
- **Lines** (`struct web_flow`): words, pictures and boxes in a line stand on one baseline
  (`web_line_break`); a word's characters that its face lacks are in the face of every script
  (`web_char_face`); a word no line holds is broken where it must be. Words that follow each
  other in the page's text, in one face and colour, are one item: a line of plain text is one
  request to the window system.
- **Tables** (`web_table`): each column's narrowest and widest from its cells, the table as
  wide as they like up to its room or as its `width` says, the columns sharing what there is
  between narrowest and widest; `colspan`, `rowspan` (a cell keeps its columns in the rows
  below), a row as high as its highest cell, a cell's content at its top, middle or bottom.
- **Rows** (`web_flex_row`): each box its `flex-basis`, its `width` or its content's width;
  what is left over shared by `flex-grow` or placed by `justify-content`; shrunk towards
  their narrowest when too wide, and on further rows when even that is too wide or the row
  may wrap. A column is boxes one under the other with the gap between. **Grids** are the
  columns their template names, filled row by row; a template whose columns would be under
  40 pixels is one column.
- **Floats** (`web_float_box`): a box at the left or right edge of its column; the lines
  after it are shorter beside it, a table or a row stands beside it, `clear` goes under it.
  A box of nothing but floats holds them (a bar of floated items).
- **Nothing is cut off and nothing is placed by coordinates.** There is no clipping and no
  scrolling sideways: text that may not be broken is broken all the same where its box ends,
  a picture wider than its column is made smaller, a negative margin is none (a side column
  pulled into place by one comes under the content). `position: absolute` and `fixed` are
  boxes where they are written, except one with a clip, which is text for screen readers and
  not shown. A listing wider than the page (`white-space: pre`) is the one thing that runs
  off the right edge.
- **Forms**: text fields, buttons, check boxes, selects (a click takes the next option). A
  form whose method is GET is sent as an address with the fields' values after it
  (`submit` in `nxweb.c`): a search field works. POST is not done.
- A page that sends its reader on at once (`<meta http-equiv=refresh>`, often inside
  `<noscript>`) is followed, three times in a row at most.

## Drawn once, scrolled as a copy

The page is not drawn into the window. `nxweb` asks for a pixmap as wide as its view and as
high as 3.2 MB of GPU memory allow (1,365 rows at the usual width, three and a half views),
draws the page's rows into it once, and the view is a copy of a part of it
(`show_view`): one textured rectangle of the GPU's, whatever is on the page.

- **A pixmap in the GPU's memory** (`pixmap_map` in `scr_shaderemu.c`, `docs/nanox.md`): a
  pixmap of the screen's kind gets its pixels from the pool the windows' buffers come from,
  and is drawn on by the GPU as a window is; a copy from it to a window is a rectangle with
  it as its texture. The driver counts such pixmaps in a word a program can read (`REG_PIXMAPS`):
  if the pool had no room the browser draws into its window as before.
- **A scroll is steps**, a third of what is left each 15 ms (`scroll_step`): a notch of the
  wheel or a page key moves the page smoothly, and a step asks the window system for a copy
  and the scroll bar, nothing else.
- When the view leaves the rows the pixmap holds, the pixmap is drawn again round the view,
  further the way the scroll is going.
- A field typed into is drawn again where it stands in the pixmap, and the view copied.

## What a page costs

Instructions of the first core (`NXWEB_TIME=1`; `rdcycle` round each step), a page from the
cache in the ROM, the window 600 x 420:

| Thousands of instructions | Hacker News | Lobsters | Wikipedia's main page | danluu.com | Gutenberg's top 100 |
|---|---|---|---|---|---|
| bytes of HTML, of sheets | 35 KB, 7 KB | 59 KB, 59 KB | 75 KB, in the page | 23 KB, none | 60 KB, 190 KB |
| elements and texts, rules | 1,299, 88 | 2,005, 276 | 1,274, 84 | 1,048, 4 | 2,267, 518 |
| the sheets found and read from the ROM | 361 | 533 | 484 | 224 | 721 |
| the sheets' rules | 733 | 3,357 | 919 | 84 | 7,762 |
| the tree | 1,819 | 2,573 | 2,222 | 1,159 | 2,780 |
| the styles | 1,665 | 2,224 | 1,439 | 605 | 4,970 |
| the layout | 1,540 | 1,816 | 3,780 | 1,760 | 3,987 |
| the pixmap's rows asked of the window system | 354 | 582 | 671 | 73 | 361 |
| all, and at 3 million a second | 6,472: 2.2 s | 11,085: 3.7 s | 9,515: 3.2 s | 3,905: 1.3 s | 20,581: 6.9 s |

- A site's next page has the same sheets: they are not read again (`web_read_sheets` keeps a
  sum of them), so Lobsters' second page is 54 thousand where the first was 3,357.
- Where it went, and what was taken off (Hacker News, 13.3 million at first): every question
  about an attribute walked the tag (a bit a name now: 15% of everything); a rule ending in
  `a` was tried on every link up to the root (the names round an element are counted);
  `font-family` looked for 29 names anywhere in its value (one walk over the list); text was
  measured a call a character (a table of the face's first 256 advances, and a word the face
  has whole is put with one walk).
  A table's advances are looked up in the font when a text first has the character
  (`WEB_ADVANCE_LATER`, `web_advance_found` in `web.h`): filled whole, each face's table was
  26 thousand instructions for the sixty characters a page has; the start 1,940 thousand to
  1,873, the same pictures.
- **The start** with the home page (3.7 KB, 122 nodes, 77 texts) is 1,859 thousand, where the
  browser of before was 1,400: the layout 296, the tree 175, the sheets 116, the styles 75,
  four font files opened and mapped 143, 120 pages first touched 184, and in the window
  system the texts' glyphs looked up (`rom_glyph`, 130 instructions a character) 240. Taken
  off: the font tried at the start is the plain face's (not made twice), and the window
  system's widths are asked once (1,866 to 1,859). Not done: the home page laid out at the
  build, the pixmap's rows below the view drawn when the browser is idle.
- **Scrolling** (`build/web/cost.sh`'s way: the same `nxkey` line, less the same waits with no
  events): a notch of the wheel 251 thousand instructions in five steps where the browser of
  before drew the view again once for 336; a page key 182 thousand in ten steps where it was
  252 in one. A step is under 20 thousand: 6 ms of the first core.

- **On the shader machine** (13 October 2026, RTX 5090, D3D11 with fxc2, through the card's
  queue, 2.2 to 2.4 million instructions a second in the run; `NXWEB_TIME=1` has a scroll say
  how its steps went, by the host's clock and the GPU's count of copies): a notch of the wheel
  on Hacker News is 5 steps in 89 to 96 ms, the longest gap between two 28 to 36 ms; a page
  key 10 steps in 183 to 185 ms, the longest gap 21 to 23 ms; every step's picture was in the
  window before the next step was asked for. The screen at the end (six notches, two pages
  down to the page's end, one up: row 378) is the host test's drawing of those rows, every
  pixel within 8 levels. Hacker News from the ROM's copy was 6.4 million instructions there
  too. Not measured there: the browser of before, and a page larger than these.

The pictures in the machine against the same rows drawn by `tools/web_test.c` on the host
(13 October 2026, the harness with `--cpu --warp`, the view's 586 x 376 pixels): the layout
page 99.96% of pixels within 8 levels, Hacker News 99.99%, scrolled eight notches 100%,
Lobsters 99.6%, text.npr.org 99.9%, kernel.org 99.9%, Wikipedia's main page 97.5% (its
pictures are scaled by the GPU and by the test differently). What differs by more is slanted
text, which the test only approximates.

## Scripts

Not run, and measured before deciding (13 October 2026; QuickJS, quickjs-ng, MuJS, Duktape
and mquickjs built for the machine and counted with `rdcycle` under `rvc_cpu`):

- **What sites ship.** Of the 24 sites of `web/sites.txt`, 12 send no script at all and all 24
  have their text in the HTML. Of 18 mainstream sites tried beside them, eight answered a
  client without scripts with a challenge or a wall (Google's results, Reddit, Amazon, the
  New York Times, Stack Overflow), three are empty shells (YouTube, X, Instagram), the rest
  have their text in the page and use scripts for menus, search suggestions, collapsing and
  advertising.
- **What the language is.** 303 script files of those sites (10 MB): QuickJS parsed every one
  that was not a module with imports; the ES5 engines parsed 10 to 17%. Hacker News' 5 KB
  fails on them at one arrow function. So the engine would be QuickJS: 1.1 MB of program,
  five times this browser, 0.44 MB of memory at its start.
- **What parsing costs.** 701 instructions a byte of source over those files: 4.3 KB of script
  a second at 3 million instructions a second. Hacker News' script 0.8 s, jQuery 22 s, React
  with ReactDOM 38 s before anything is drawn, a site's own bundle three to eight minutes.
  Bytecode made ahead reads 17 times faster (jQuery 1.2 s) but is 3.5 times the source.
- **What running costs.** A class toggled on 50 elements 0.07 s, a list of 200 rows built as
  text 0.5 s, 50 KB of JSON parsed 2.3 s, 100 thousand additions of whole numbers 7 s and of
  fractions 12 s (a double is about 150 instructions of the machine, which has single floats
  only), 100 thousand function calls 15 s.
- **So**: a page may spend about ten thousand bytes of script at its load and a few hundred
  simple steps a click. That is Hacker News' comment folding and Lobsters' "3 hours ago", and
  nothing with jQuery or a framework. In the VRChat world it is less still: every script is
  one more address of a kind the world must have been built with, their names carry the
  version's hash, and there are no cookies and no POST for what the small scripts mostly do
  (vote, log in, load more).
- What was taken from it without an engine: `<noscript>` is shown, `<details>` opens and
  closes, a redirect for readers without scripts is followed. The next step, if one is wanted:
  QuickJS in a process of its own with a budget of instructions (`JS_SetInterruptHandler`),
  scripts of the page's own site under 10 KB, and a tree of the page it may change, after
  which the page is laid out again. The tree is there now (`web_nodes`); a binding to it is
  about forty calls (`querySelector`, `classList`, `addEventListener`, `getAttribute`,
  `innerHTML`, `style.display` lead the count in the scripts looked at).

Not done: scripts, POST, cookies, pictures as backgrounds, SVG drawn (a page's inline `<svg>`
is passed over, which is Google's own logo), positioned boxes, clipping and scrolling inside
a box, text selection, a find in the page.
