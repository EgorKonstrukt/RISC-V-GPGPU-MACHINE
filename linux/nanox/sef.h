/*
 * A font file of ours (docs/fonts.md, tools/make_fonts.py): glyphs the GPU draws from where the
 * file lies in the ROM. The window system's driver and the programs that measure text read it.
 */
#ifndef SHADEREMU_SEF_H
#define SHADEREMU_SEF_H

#include <stdint.h>

struct sef_head {
	char magic[4];			/* "SEF1" */
	uint16_t height, ascent;	/* of a line, in pixels */
	uint16_t bpp, em;		/* 1: a bit a pixel; 4: sixteen levels of coverage */
	uint32_t glyphs;
	uint32_t ranges, ranges_at;	/* runs of code points, rising: struct sef_range */
	uint32_t advances_at;		/* a byte a glyph */
	uint32_t table_at;		/* a 16-byte texel a glyph: bitmap, width, height, dx | dy << 8 */
	uint32_t bits_at, size;
	uint32_t missing;		/* the glyph for a code point the font lacks, or ~0 */
};

struct sef_range { uint32_t first, count, glyph; };

static inline int
sef_ok(const void *file, uint32_t size)
{
	const struct sef_head *h = file;

	return size >= sizeof *h && !__builtin_memcmp(h->magic, "SEF1", 4) && h->size <= size &&
	       h->table_at + 16 * h->glyphs <= h->size && !(h->table_at & 15);
}

/* The glyph of a code point, or -1 when the font has none. */
static inline int
sef_glyph(const void *file, uint32_t code)
{
	const struct sef_head *h = file;
	const struct sef_range *r = (const struct sef_range *)((const char *)file + h->ranges_at);
	uint32_t lo = 0, hi = h->ranges;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;

		if (code < r[mid].first)
			hi = mid;
		else if (code - r[mid].first >= r[mid].count)
			lo = mid + 1;
		else
			return (int)(r[mid].glyph + code - r[mid].first);
	}
	return -1;
}

static inline int
sef_advance(const void *file, int glyph)
{
	const struct sef_head *h = file;

	return ((const unsigned char *)file + h->advances_at)[glyph];
}

#endif
