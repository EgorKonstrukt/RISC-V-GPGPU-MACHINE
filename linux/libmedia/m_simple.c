/* PNM (P1-P6), BMP and TGA: headers here, rows converted over every core (docs/media.md). */
#include <stdlib.h>
#include "m_int.h"

struct simple {
	enum media_kind kind;
	const uint8_t *f, *pixels, *end;
	struct media_image *out;
	int w, h, bpp, row_bytes, top_down, maxval, pnm, gray;
	uint32_t masks[4];
	int shift[4], bits[4];
	uint8_t palette[256][4];
	int colours;
	uint32_t unpacked_bytes;
	uint8_t *unpacked;	/* RLE pictures, unpacked first on this core (8 bits or 4 a pixel, rows in order) */
};

/* ---------------------------------------------------------------- headers */

static const uint8_t *
pnm_number(const uint8_t *p, const uint8_t *end, int *v)
{
	for (;;) {
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
			p++;
		if (p < end && *p == '#') {
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		break;
	}
	if (p >= end || *p < '0' || *p > '9')
		return NULL;
	for (*v = 0; p < end && *p >= '0' && *p <= '9'; p++)
		*v = *v * 10 + (*p - '0');
	return p;
}

static void
mask_shift(uint32_t mask, int *shift, int *bits)
{
	*shift = *bits = 0;
	if (!mask)
		return;
	while (!(mask & 1))
		mask >>= 1, ++*shift;
	while (mask & 1)
		mask >>= 1, ++*bits;
}

static const char *
simple_header(const uint8_t *f, uint32_t size, struct simple *s)
{
	const uint8_t *end = f + size;

	memset(s, 0, sizeof *s);
	s->f = f, s->end = end;
	if (f[0] == 'P' && f[1] >= '1' && f[1] <= '6') {
		const uint8_t *p = f + 2;

		s->kind = MEDIA_PNM;
		s->pnm = f[1] - '0';
		s->maxval = 1;
		if (!(p = pnm_number(p, end, &s->w)) || !(p = pnm_number(p, end, &s->h)) ||
		    (s->pnm != 1 && s->pnm != 4 && !(p = pnm_number(p, end, &s->maxval))))
			return "a PNM picture that is damaged";
		if (s->maxval < 1 || s->maxval > 65535)
			return "a PNM picture of a kind this does not read";
		s->pixels = p + 1;	/* one white space ends the header */
		s->gray = s->pnm != 3 && s->pnm != 6;
		if (s->pnm >= 4) {
			int wide = s->maxval > 255 ? 2 : 1;

			s->row_bytes = s->pnm == 4 ? (s->w + 7) / 8 : s->w * (s->pnm == 6 ? 3 : 1) * wide;
			if (s->pixels + (uint64_t)s->row_bytes * s->h > end)
				return "a PNM picture that is cut short";
		}
		s->top_down = 1;
		return NULL;
	}
	if (size >= 54 && f[0] == 'B' && f[1] == 'M') {
		uint32_t off = m_le32(f + 10), hsize = m_le32(f + 14), comp, i;
		int32_t h;

		s->kind = MEDIA_BMP;
		if (hsize == 12) {
			s->w = (int)m_le16(f + 18), h = (int16_t)m_le16(f + 20), s->bpp = (int)m_le16(f + 24), comp = 0;
		} else {
			s->w = (int32_t)m_le32(f + 18), h = (int32_t)m_le32(f + 22), s->bpp = (int)m_le16(f + 28), comp = m_le32(f + 30);
		}
		s->top_down = h < 0;
		s->h = h < 0 ? -h : h;
		if (off >= size)
			return "a BMP picture that is damaged";
		s->pixels = f + off;
		if (s->bpp <= 8) {
			uint32_t entry = hsize == 12 ? 3 : 4, n = hsize >= 40 && m_le32(f + 46) ? m_le32(f + 46) : 1u << s->bpp;
			const uint8_t *pal = f + 14 + hsize;

			for (i = 0; i < n && i < 256 && pal + (i + 1) * entry <= f + off; i++) {
				s->palette[i][0] = pal[i * entry + 2], s->palette[i][1] = pal[i * entry + 1];
				s->palette[i][2] = pal[i * entry], s->palette[i][3] = 255;
			}
			s->colours = (int)i;
		}
		if (comp == 3 || comp == 6) {
			const uint8_t *m = hsize >= 52 ? f + 54 : f + 14 + hsize;

			s->masks[0] = m_le32(m), s->masks[1] = m_le32(m + 4), s->masks[2] = m_le32(m + 8);
			s->masks[3] = hsize >= 56 || comp == 6 ? m_le32(m + 12) : 0;
		} else if (s->bpp == 16) {
			s->masks[0] = 0x7c00, s->masks[1] = 0x03e0, s->masks[2] = 0x001f;
		} else if (s->bpp == 32) {
			s->masks[0] = 0xff0000, s->masks[1] = 0xff00, s->masks[2] = 0xff;
			/* alpha only when a v4/v5 header says so */
			s->masks[3] = hsize >= 56 ? m_le32(f + 14 + 52) : 0;
		}
		for (i = 0; i < 4; i++)
			mask_shift(s->masks[i], &s->shift[i], &s->bits[i]);
		if (comp == 1 || comp == 2) {
			s->bpp = comp == 1 ? 8 : 4;
			s->kind = MEDIA_BMP;
			s->row_bytes = -1;	/* RLE */
			return s->w > 0 && s->h > 0 ? NULL : "a BMP picture that is damaged";
		}
		if (comp != 0 && comp != 3 && comp != 6)
			return "a BMP picture of a kind this does not read";
		if (s->bpp != 1 && s->bpp != 4 && s->bpp != 8 && s->bpp != 16 && s->bpp != 24 && s->bpp != 32)
			return "a BMP picture of a kind this does not read";
		s->row_bytes = ((s->w * s->bpp + 31) / 32) * 4;
		if (s->w < 1 || s->h < 1 || s->pixels + (uint64_t)s->row_bytes * s->h > end)
			return "a BMP picture that is cut short";
		return NULL;
	}
	/* TGA has no mark at its start: its header must make sense */
	if (size >= 18 && f[1] <= 1 && (f[2] == 1 || f[2] == 2 || f[2] == 3 || f[2] == 9 || f[2] == 10 || f[2] == 11) &&
	    (f[16] == 8 || f[16] == 15 || f[16] == 16 || f[16] == 24 || f[16] == 32)) {
		int type = f[2], cmap_first = (int)m_le16(f + 3), cmap_len = (int)m_le16(f + 5), cmap_bits = f[7], i;
		const uint8_t *cmap = f + 18 + f[0];

		s->kind = MEDIA_TGA;
		s->w = (int)m_le16(f + 12), s->h = (int)m_le16(f + 14), s->bpp = f[16];
		s->top_down = (f[17] & 0x20) != 0;
		s->gray = type == 3 || type == 11;
		s->pixels = cmap + (f[1] ? cmap_len * ((cmap_bits + 7) / 8) : 0);
		if ((f[17] & 0x10) || s->w < 1 || s->h < 1 || s->pixels > end)
			return "a TGA picture of a kind this does not read";
		if (f[1]) {
			int eb = (cmap_bits + 7) / 8;

			for (i = 0; i < cmap_len && cmap_first + i < 256; i++) {
				const uint8_t *e = cmap + i * eb;
				uint8_t *p = s->palette[cmap_first + i];

				if (eb >= 3)
					p[0] = e[2], p[1] = e[1], p[2] = e[0], p[3] = eb == 4 ? e[3] : 255;
				else {
					uint32_t v = m_le16(e);

					p[0] = (uint8_t)((v >> 10 & 31) * 255 / 31), p[1] = (uint8_t)((v >> 5 & 31) * 255 / 31);
					p[2] = (uint8_t)((v & 31) * 255 / 31), p[3] = 255;
				}
			}
			s->colours = 256;
		}
		s->row_bytes = type >= 9 ? -1 : s->w * ((s->bpp + 7) / 8);
		if (s->row_bytes > 0 && s->pixels + (uint64_t)s->row_bytes * s->h > end)
			return "a TGA picture that is cut short";
		return NULL;
	}
	return "not a picture this knows";
}

const char *
m_simple_probe(const uint8_t *f, uint32_t size, struct media_info *info)
{
	struct simple s;
	const char *why = simple_header(f, size, &s);

	if (why)
		return why;
	info->kind = s.kind;
	info->width = s.w, info->height = s.h;
	info->alpha = s.masks[3] != 0 || (s.kind == MEDIA_TGA && s.bpp == 32);
	info->parallel = s.row_bytes > 0;
	return NULL;
}

/* ---------------------------------------------------------------- unpacking runs (this core) */

static int
unpack_bmp_rle(struct simple *s)
{
	const uint8_t *p = s->pixels, *end = s->end;
	int x = 0, y = 0, four = s->bpp == 4;

	s->unpacked_bytes = (uint32_t)s->w * (uint32_t)s->h;
	if (!(s->unpacked = m_map(s->unpacked_bytes)))
		return 0;
	while (p + 1 < end && y < s->h) {
		int n = p[0], v = p[1];

		p += 2;
		if (n) {
			int k;

			for (k = 0; k < n && x < s->w && y < s->h; k++)
				s->unpacked[(uint32_t)y * s->w + x++] = (uint8_t)(four ? (k & 1 ? v & 15 : v >> 4) : v);
			continue;
		}
		if (v == 0) {
			x = 0, y++;
		} else if (v == 1) {
			break;
		} else if (v == 2) {
			if (p + 2 > end)
				break;
			x += p[0], y += p[1], p += 2;
		} else {
			int k;

			for (k = 0; k < v && p < end; k++) {
				int c = four ? (k & 1 ? p[k / 2] & 15 : p[k / 2] >> 4) : p[k];

				if (x < s->w && y < s->h)
					s->unpacked[(uint32_t)y * s->w + x++] = (uint8_t)c;
			}
			p += four ? ((v + 1) / 2 + 1) / 2 * 2 : (v + 1) / 2 * 2;
		}
	}
	s->bpp = 8;
	return 1;
}

static int
unpack_tga_rle(struct simple *s)
{
	int bytes = (s->bpp + 7) / 8;
	uint32_t total = (uint32_t)s->w * (uint32_t)s->h * (uint32_t)bytes, n = 0;
	const uint8_t *p = s->pixels;

	s->unpacked_bytes = total;
	if (!(s->unpacked = m_map(total)))
		return 0;
	while (n < total && p < s->end) {
		int head = *p++, count = (head & 127) + 1;

		if (head & 128) {
			if (p + bytes > s->end)
				break;
			while (count-- && n < total)
				memcpy(s->unpacked + n, p, bytes), n += bytes;
			p += bytes;
		} else {
			uint32_t run = (uint32_t)count * bytes;

			if (run > total - n) run = total - n;
			if (p + run > s->end)
				break;
			memcpy(s->unpacked + n, p, run);
			n += run, p += run;
		}
	}
	s->row_bytes = s->w * bytes;
	s->pixels = s->unpacked;
	return 1;
}

/* ---------------------------------------------------------------- rows */

static void
simple_rows(void *ctx, uint32_t from, uint32_t to)
{
	struct simple *s = ctx;
	struct media_image *o = s->out;
	int x, y, ch = o->channels;

	for (y = (int)from; y < (int)to; y++) {
		int sy = s->top_down ? y : s->h - 1 - y;
		uint8_t *out = o->pixels + (uint32_t)y * (uint32_t)o->stride;
		const uint8_t *in = s->kind == MEDIA_BMP && s->unpacked ? s->unpacked + (uint32_t)sy * (uint32_t)s->w
				    : s->pixels + (uint32_t)sy * (uint32_t)s->row_bytes;

		for (x = 0; x < s->w; x++, out += ch) {
			int r, g, b, a = 255;

			if (s->kind == MEDIA_PNM) {
				if (s->pnm == 4) {
					r = g = b = in[x >> 3] >> (7 - (x & 7)) & 1 ? 0 : 255;
				} else if (s->maxval > 255) {
					const uint8_t *p = in + x * (s->gray ? 2 : 6);

					r = (p[0] << 8 | p[1]) * 255 / s->maxval;
					g = s->gray ? r : (p[2] << 8 | p[3]) * 255 / s->maxval;
					b = s->gray ? r : (p[4] << 8 | p[5]) * 255 / s->maxval;
				} else if (s->gray) {
					r = g = b = s->maxval == 255 ? in[x] : in[x] * 255 / s->maxval;
				} else {
					const uint8_t *p = in + x * 3;

					r = p[0], g = p[1], b = p[2];
					if (s->maxval != 255)
						r = r * 255 / s->maxval, g = g * 255 / s->maxval, b = b * 255 / s->maxval;
				}
			} else if (s->bpp <= 8 && !(s->kind == MEDIA_TGA && s->gray)) {
				int v = s->bpp == 8 ? in[x] : s->bpp == 4 ? in[x >> 1] >> (x & 1 ? 0 : 4) & 15 : in[x >> 3] >> (7 - (x & 7)) & 1;

				r = s->palette[v][0], g = s->palette[v][1], b = s->palette[v][2];
				a = s->kind == MEDIA_TGA ? s->palette[v][3] : 255;
			} else if (s->kind == MEDIA_TGA) {
				const uint8_t *p = in + x * ((s->bpp + 7) / 8);

				if (s->gray) {
					r = g = b = p[0];
					a = s->bpp == 16 ? p[1] : 255;
				} else if (s->bpp >= 24) {
					r = p[2], g = p[1], b = p[0];
					a = s->bpp == 32 ? p[3] : 255;
				} else {
					uint32_t v = m_le16(p);

					r = (v >> 10 & 31) * 255 / 31, g = (v >> 5 & 31) * 255 / 31, b = (v & 31) * 255 / 31;
				}
			} else if (s->bpp == 24) {
				r = in[3 * x + 2], g = in[3 * x + 1], b = in[3 * x];
			} else {
				uint32_t v = s->bpp == 16 ? m_le16(in + 2 * x) : m_le32(in + 4 * x);
				int c[4] = { 0, 0, 0, 255 }, k;

				for (k = 0; k < 4; k++)
					if (s->bits[k]) {
						uint32_t part = (v & s->masks[k]) >> s->shift[k], most = (1u << s->bits[k]) - 1;

						c[k] = s->bits[k] == 8 ? (int)part : (int)(part * 255 / most);
					}
				r = c[0], g = c[1], b = c[2], a = c[3];
			}
			m_put(o, out, r, g, b, a);
		}
	}
}

static const char *
pnm_ascii(struct simple *s)
{
	struct media_image *o = s->out;
	const uint8_t *p = s->pixels - 1;
	int x, y, k, v;

	for (y = 0; y < s->h; y++)
		for (x = 0; x < s->w; x++) {
			int c[3];

			for (k = 0; k < (s->pnm == 3 ? 3 : 1); k++) {
				if (s->pnm == 1) {
					while (p < s->end && *p != '0' && *p != '1')
						p++;
					if (p >= s->end)
						return "a PNM picture that is cut short";
					c[k] = *p++ == '1' ? 0 : 255;
					continue;
				}
				if (!(p = pnm_number(p, s->end, &v)))
					return "a PNM picture that is cut short";
				c[k] = v * 255 / s->maxval;
			}
			if (s->pnm != 3)
				c[1] = c[2] = c[0];
			m_put(o, o->pixels + (uint32_t)y * (uint32_t)o->stride + x * o->channels, c[0], c[1], c[2], 255);
		}
	return NULL;
}

const char *
m_simple_decode(const uint8_t *f, uint32_t size, struct media_image *out, int workers)
{
	static struct simple job;
	struct simple *s = &job;
	const char *why = simple_header(f, size, s);
	int got;

	if (why)
		return why;
	s->out = out;
	if (s->kind == MEDIA_PNM && s->pnm <= 3)
		return pnm_ascii(s);
	if (s->row_bytes < 0 && !(s->kind == MEDIA_BMP ? unpack_bmp_rle(s) : unpack_tga_rle(s)))
		return "not enough memory for the picture";
	got = m_workers_begin(workers);
	m_for((uint32_t)s->h, simple_rows, s, (uint32_t)out->stride, got);
	m_workers_end();
	if (s->unpacked)
		m_unmap(s->unpacked, s->unpacked_bytes);
	s->unpacked = NULL;
	return NULL;
}
