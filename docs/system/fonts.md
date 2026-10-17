# Fonts

The window system has two fonts of its own, a bit a pixel, Latin letters only. A page of the
web wants more: other scripts, faces with and without serifs, a fixed one, bold, italic, and
sizes from a footnote's to a heading's. Those are files in the ROM that the GPU draws glyphs
from where they lie; the processor's part of a string is still a word a character.

    python tools\make_fonts.py            # into build\images\linux\root\usr\share, before the image is made
    python tools\make_fonts.py --check build\images\linux\root\usr\share\font-sans-13.sef

| File | What |
|---|---|
| `font-sans-N.sef`, `font-sans-b-N.sef` | Liberation Sans, regular and bold |
| `font-serif-N.sef`, `font-serif-b-N.sef` | Liberation Serif |
| `font-mono-N.sef`, `font-mono-b-N.sef` | Liberation Mono |
| `font-uni-16.sef` | GNU Unifont: every character of the basic plane, 8 or 16 pixels wide |

- N is the size in pixels: 10, 11, 13, 15, 17, 20, 24, 28, 32. A text takes the nearest.
  The outline faces have Latin (with its extensions and Vietnamese), Greek, Cyrillic,
  punctuation, arrows, mathematical signs and the box-drawing characters: about 1,450 glyphs
  each, rasterised by FreeType with the faces' own hinting (through Pillow), kept as four bits
  of coverage a pixel. Unifont is a bit a pixel and has the rest: Chinese, Japanese, Korean,
  Arabic, Hebrew, the Indian scripts, 57 thousand glyphs. 55 files, 11.5 MB of ROM.
- The fonts are fetched once into `build\fetch\fonts` (Liberation 2.1.5, SIL Open Font
  License; Unifont 16.0.04, GPL 2+ with the font exception and OFL) and are not kept in this
  repository, like the desktop's pictures.
- Italic is the upright face slanted by the GPU (a fifth of a pixel a row), not a face of its
  own. A size that is not in the list is not scaled: the nearest is used. Unifont alone is
  drawn at twice its size, for text of 26 pixels and more.

## The file

`linux/nanox/sef.h`, little-endian, every part at a multiple of 16 bytes so that the GPU's
texels fall on it wherever the file lies in the ROM (romfs keeps a file's data at a multiple
of 16):

| Part | Contents |
|---|---|
| header, 64 bytes | `SEF1`; a line's height and ascent; bits a pixel (1 or 4); glyphs; where the parts are; the glyph for a character the font lacks |
| ranges | runs of code points, rising: first, count, first glyph (12 bytes each) |
| advances | a byte a glyph |
| table | a 16-byte texel a glyph: where its bitmap is in the file, its width, its height, and how far its corner is from the character's (a signed byte each way) |
| bitmaps | rows of whole bytes, the left pixel in the high bits; each glyph begins at a multiple of 4 |

A glyph is its ink's rectangle only: the table says where that lies in the character's cell.

## Drawing

`docs/machine/gpu.md`, the text command: with the flag `0x100` in its fragment word it has three more
words, a scale (16.16), a slant, and what the table's bitmap addresses are counted from (the
file's own place in the ROM, which is not known when the file is made). Fragment mode 7 is
coverage: sixteen levels, which are the colour's alpha, so such text is drawn in a pass that
blends (pass 5). The table and the bitmaps are read from the ROM by the vertex and the
fragment shader as textures and models are.

- **The window system** (`romfont_create`, `romfont_draw` in `scr_shaderemu.c`): a font whose
  name ends in `.sef` is such a file, found in the ROM as a picture is (`rom_address`) and
  mapped for its ranges and advances. `GrCreateFontEx(PATH, height, slant, NULL)`: a height
  other than the file's scales it, the width is the slant in 256ths of a pixel a row.
  Its text is 16-bit characters (`GR_TFUC16`); a string is one command however long.
- **What is drawn after blended text in a list is drawn in the same pass** (`blended` in the
  driver): the passes are drawn one after another, and a fill queued after a line of text
  would otherwise come out under it. The list's first command, the surface's own picture,
  stays in the opaque pass.
- A font not in the ROM (a file in memory), or a server that draws in software
  (`NANOX_SOFTWARE=1`), has none of this: the name is no font, and the program gets the
  built-in one. Text of these fonts is not clipped to a window's children: a program draws
  it where it is all its own (the browser draws into a pixmap).
- **A program measures by itself** (`web_advance` in `linux/apps/web.h`): it maps the same
  file and adds the advances up; asking the server would be a question a word.

Not done: kerning, ligatures, the joining of Arabic letters and the reordering of text
written from the right (such text is shown, each letter in its own form, from the left),
characters outside the basic plane (emoji), a real italic.
