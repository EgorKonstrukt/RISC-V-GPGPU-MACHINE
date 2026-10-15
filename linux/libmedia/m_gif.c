/*
 * GIF pictures and animations (docs/media.md). A frame's codes stand by themselves, so every
 * frame is unpacked (LZW, into a byte a pixel) by a core of its own, all of them ahead; a frame
 * is then laid on the canvas by every core, rows each, in order (a frame lies on the one before).
 */
#include <stdlib.h>
#include "m_int.h"

struct gif_frame {
	uint32_t data;		/* where its first sub-block's length is */
	int x, y, w, h, interlaced, delay, disposal, clear, min_code;
	uint32_t palette;	/* where its colours are in the file (0: the global ones) */
	int colours;
	uint8_t *indices;	/* w * h bytes, made by a worker */
	uint32_t indices_bytes;
	int bad;
};

struct media_anim {
	const uint8_t *file;
	uint32_t size;
	int w, h, frames, loops, background, global_colours, workers;
	uint32_t global;	/* where the global colours are (0: none) */
	struct gif_frame *frame;
	uint8_t *canvas, *before;	/* the canvas, and what a "back to the previous" frame restores */
	uint32_t canvas_bytes;
	int shown;			/* the frame the canvas has (-1: none) */
	struct lzw *tables;		/* a table a core, made here before the jobs */
	uint32_t tables_bytes;
};

/* ---------------------------------------------------------------- the frames' places */

static const char *
gif_scan(struct media_anim *a, int whole)
{
	const uint8_t *f = a->file;
	uint32_t at = 13, size = a->size, room = 16;
	int delay = 0, disposal = 0, clear = -1;

	if (size < 13)
		return "a GIF picture that is cut short";
	a->w = (int)m_le16(f + 6), a->h = (int)m_le16(f + 8);
	a->background = f[11];
	if (f[10] & 0x80) {
		a->global = at;
		a->global_colours = 2 << (f[10] & 7);
		at += 3 * (uint32_t)a->global_colours;
	}
	a->loops = 1;
	a->frames = 0;
	if (whole && !(a->frame = calloc(room, sizeof *a->frame)))
		return "not enough memory for the animation";
	while (at < size) {
		int kind = f[at++];

		if (kind == 0x3b)
			break;
		if (kind == 0x21) {
			int label;

			if (at >= size)
				break;
			label = f[at++];
			if (label == 0xf9 && at + 5 < size && f[at] >= 4) {
				disposal = f[at + 1] >> 2 & 7;
				delay = (int)m_le16(f + at + 2) * 10;
				clear = f[at + 1] & 1 ? f[at + 4] : -1;
			} else if (label == 0xff && at + 14 < size && f[at] == 11 && !memcmp(f + at + 1, "NETSCAPE2.0", 11) && f[at + 12] >= 3) {
				a->loops = (int)m_le16(f + at + 14);
			}
			while (at < size && f[at])
				at += f[at] + 1u;
			at++;
			continue;
		}
		if (kind != 0x2c || at + 9 > size)
			break;	/* (what follows is not read: the frames so far are the picture) */
		{
			struct gif_frame g;
			int packed = f[at + 8];

			memset(&g, 0, sizeof g);
			g.x = (int)m_le16(f + at), g.y = (int)m_le16(f + at + 2);
			g.w = (int)m_le16(f + at + 4), g.h = (int)m_le16(f + at + 6);
			g.interlaced = (packed & 0x40) != 0;
			at += 9;
			if (packed & 0x80) {
				g.palette = at;
				g.colours = 2 << (packed & 7);
				at += 3 * (uint32_t)g.colours;
			}
			if (at >= size)
				break;
			g.min_code = f[at++];
			g.data = at;
			g.delay = delay, g.disposal = disposal, g.clear = clear;
			delay = disposal = 0, clear = -1;
			while (at < size && f[at])
				at += f[at] + 1u;
			at++;
			if (at > size)
				break;
			if (whole) {
				if (a->frames == (int)room) {
					struct gif_frame *more = realloc(a->frame, 2 * room * sizeof *more);

					if (!more)
						break;
					a->frame = more, room *= 2;
				}
				a->frame[a->frames] = g;
			}
			if (!a->w || !a->h)
				a->w = g.x + g.w, a->h = g.y + g.h;
			a->frames++;
		}
	}
	if (!a->frames)
		return "a GIF picture with nothing in it";
	if (a->w < 1 || a->h < 1 || a->w > M_MOST_SIDE || a->h > M_MOST_SIDE)
		return "a GIF picture of a size this does not read";
	return NULL;
}

/* ---------------------------------------------------------------- LZW */

struct lzw { uint16_t prefix[4096]; uint8_t suffix[4096], first[4096]; uint16_t length[4096]; };

/* A frame's codes into its indices (w * h; what is missing stays 0). */
static void
gif_unpack(const struct media_anim *a, struct gif_frame *g, struct lzw *t)
{
	const uint8_t *f = a->file, *end = f + a->size;
	uint32_t at = g->data, block = 0, total = (uint32_t)g->w * (uint32_t)g->h, n = 0, hold = 0;
	int bits = 0, min = g->min_code, size, next, clear, eoi, prev = -1, i;
	uint8_t *out = g->indices;

	if (min < 2 || min > 11) {
		g->bad = 1;
		return;
	}
	clear = 1 << min, eoi = clear + 1;
	size = min + 1, next = clear + 2;
	for (i = 0; i < clear; i++)
		t->prefix[i] = 0xffff, t->suffix[i] = t->first[i] = (uint8_t)i, t->length[i] = 1;
	while (n < total) {
		int code, len;

		while (bits < size) {
			if (!block) {
				if (f + at >= end || !f[at])
					goto done;
				block = f[at++];
			}
			if (f + at >= end)
				goto done;
			hold |= (uint32_t)f[at++] << bits;
			bits += 8;
			block--;
		}
		code = (int)(hold & ((1u << size) - 1));
		hold >>= size, bits -= size;
		if (code == clear) {
			size = min + 1, next = clear + 2, prev = -1;
			continue;
		}
		if (code == eoi)
			break;
		if (prev < 0) {
			if (code >= clear)
				break;
			out[n++] = (uint8_t)code;
			prev = code;
			continue;
		}
		if (code < next) {
			len = t->length[code];
			if (next < 4096) {
				t->prefix[next] = (uint16_t)prev, t->suffix[next] = t->first[code];
				t->first[next] = t->first[prev], t->length[next] = (uint16_t)(t->length[prev] + 1);
			}
		} else if (code == next && next < 4096) {
			t->prefix[next] = (uint16_t)prev, t->suffix[next] = t->first[prev];
			t->first[next] = t->first[prev], t->length[next] = (uint16_t)(t->length[prev] + 1);
			len = t->length[next];
		} else {
			g->bad = 1;
			break;
		}
		/* the string from its end back, written where it goes */
		{
			int c = code, k = len;

			if (n + (uint32_t)len > total)
				k = len, len = (int)(total - n);
			while (k > len)
				c = t->prefix[c], k--;
			for (k = len - 1; k >= 0; k--, c = t->prefix[c])
				out[n + (uint32_t)k] = t->suffix[c];
			n += (uint32_t)len;
		}
		if (next < 4096) {
			next++;
			if (next == 1 << size && size < 12)
				size++;
		}
		prev = code;
	}
done:
	return;
}

static void
unpack_jobs(void *ctx, uint32_t from, uint32_t to)
{
	struct media_anim *a = ctx;
	struct lzw *t = a->tables + mcw_self();
	uint32_t k;

	for (k = from; k < to; k++)
		gif_unpack(a, &a->frame[k], t);
}

/* ---------------------------------------------------------------- laying a frame on the canvas */

struct lay { struct media_anim *a; const struct gif_frame *g; struct media_image *out; uint8_t pal[256][4]; };

/* The row of a frame's indices that is row y of the frame (interlaced: in four passes). */
static int
frame_row(const struct gif_frame *g, int y)
{
	int h = g->h, before;

	if (!g->interlaced)
		return y;
	if (y % 8 == 0)
		return y / 8;
	before = (h + 7) / 8;
	if (y % 8 == 4)
		return before + y / 8;
	before += (h + 3) / 8;
	if (y % 4 == 2)
		return before + y / 4;
	before += (h + 1) / 4;
	return before + y / 2;
}

static void
lay_rows(void *ctx, uint32_t from, uint32_t to)
{
	struct lay *l = ctx;
	const struct gif_frame *g = l->g;
	struct media_anim *a = l->a;
	uint32_t y;
	int x;

	for (y = from; y < to; y++) {
		int fy = (int)y - g->y;
		uint8_t *row = a->canvas + y * (uint32_t)a->w * 4;
		const uint8_t *in;

		if (fy < 0 || fy >= g->h)
			continue;
		in = g->indices + (uint32_t)frame_row(g, fy) * (uint32_t)g->w;
		for (x = 0; x < g->w && g->x + x < a->w; x++) {
			int v = in[x];

			if (v != g->clear)
				memcpy(row + (g->x + x) * 4, l->pal[v], 4);
		}
	}
}

static void
clear_rect(struct media_anim *a, const struct gif_frame *g)
{
	int y, x;

	for (y = g->y; y < g->y + g->h && y < a->h; y++)
		for (x = g->x; x < g->x + g->w && x < a->w; x++)
			memset(a->canvas + ((uint32_t)y * (uint32_t)a->w + (uint32_t)x) * 4, 0, 4);
}

static void
palette_of(const struct media_anim *a, const struct gif_frame *g, uint8_t pal[256][4])
{
	const uint8_t *p = a->file + (g->palette ? g->palette : a->global);
	int n = g->palette ? g->colours : a->global_colours, i;

	for (i = 0; i < 256; i++) {
		if (i < n && (g->palette || a->global))
			pal[i][0] = p[3 * i], pal[i][1] = p[3 * i + 1], pal[i][2] = p[3 * i + 2];
		else
			pal[i][0] = pal[i][1] = pal[i][2] = 0;
		pal[i][3] = 255;
	}
}

static void
lay(struct media_anim *a, int n, int workers)
{
	static struct lay l;
	const struct gif_frame *g = &a->frame[n];

	/* what the frame before asked to be done when it went */
	if (n > 0) {
		const struct gif_frame *p = &a->frame[n - 1];

		if (p->disposal == 2)
			clear_rect(a, p);
		else if (p->disposal == 3 && a->before)
			memcpy(a->canvas, a->before, a->canvas_bytes);
	} else {
		memset(a->canvas, 0, a->canvas_bytes);
	}
	if (g->disposal == 3 && a->before)
		memcpy(a->before, a->canvas, a->canvas_bytes);
	if (!g->indices)
		return;
	l.a = a, l.g = g;
	palette_of(a, g, l.pal);
	m_for((uint32_t)a->h, lay_rows, &l, (uint32_t)a->w * 4, workers);
}

/* ---------------------------------------------------------------- the calls */

static struct media_anim *
anim_open(const void *data, uint32_t size, int workers, int only_first, const char **why)
{
	struct media_anim *a = calloc(1, sizeof *a);
	int k, got, any_back = 0, frames;

	if (!a) {
		*why = "not enough memory for the animation";
		return NULL;
	}
	a->file = data, a->size = size;
	if ((*why = gif_scan(a, 1)) != NULL) {
		media_anim_close(a);
		return NULL;
	}
	frames = only_first ? 1 : a->frames;
	for (k = 0; k < frames; k++) {
		struct gif_frame *g = &a->frame[k];

		g->indices_bytes = (uint32_t)g->w * (uint32_t)g->h;
		if (g->indices_bytes && !(g->indices = m_map(g->indices_bytes))) {
			*why = "not enough memory for the animation";
			media_anim_close(a);
			return NULL;
		}
		any_back |= g->disposal == 3;
	}
	a->canvas_bytes = (uint32_t)a->w * (uint32_t)a->h * 4;
	if (!(a->canvas = m_map(a->canvas_bytes)) || (any_back && !(a->before = m_map(a->canvas_bytes)))) {
		*why = "not enough memory for the animation";
		media_anim_close(a);
		return NULL;
	}
	a->shown = -1;
	got = m_workers_begin(workers);
	a->tables_bytes = (uint32_t)(mcw_count() + 1) * sizeof(struct lzw);
	if (!(a->tables = m_map(a->tables_bytes))) {
		m_workers_end();
		*why = "not enough memory for the animation";
		media_anim_close(a);
		return NULL;
	}
	m_for((uint32_t)frames, unpack_jobs, a, 0, got);
	a->workers = got;
	m_workers_end();
	return a;
}

struct media_anim *
media_anim_open(const void *data, uint32_t size, int workers, const char **why)
{
	return anim_open(data, size, workers, 0, why);
}

int media_anim_frames(const struct media_anim *a) { return a->frames; }
int media_anim_width(const struct media_anim *a) { return a->w; }
int media_anim_height(const struct media_anim *a) { return a->h; }
int media_anim_loops(const struct media_anim *a) { return a->loops; }
int m_anim_delay(const struct media_anim *a, int n) { return a->frame[n].delay ? a->frame[n].delay : 100; }

const uint8_t *
media_anim_frame(struct media_anim *a, int n, int *delay_ms)
{
	int k, got;

	if (n < 0 || n >= a->frames)
		return NULL;
	got = m_workers_begin(a->workers);
	if (n <= a->shown)
		a->shown = -1;
	for (k = a->shown + 1; k <= n; k++)
		lay(a, k, got);
	m_workers_end();
	a->shown = n;
	if (delay_ms)
		*delay_ms = a->frame[n].delay ? a->frame[n].delay : 100;
	return a->canvas;
}

void
media_anim_close(struct media_anim *a)
{
	int k;

	if (!a)
		return;
	for (k = 0; a->frame && k < a->frames; k++)
		m_unmap(a->frame[k].indices, a->frame[k].indices_bytes);
	free(a->frame);
	m_unmap(a->canvas, a->canvas_bytes);
	m_unmap(a->before, a->canvas_bytes);
	m_unmap(a->tables, a->tables_bytes);
	free(a);
}

const char *
m_gif_probe(const uint8_t *f, uint32_t size, struct media_info *info)
{
	struct media_anim a;
	const char *why;

	memset(&a, 0, sizeof a);
	a.file = f, a.size = size;
	if ((why = gif_scan(&a, 0)) != NULL)
		return why;
	info->kind = MEDIA_GIF;
	info->width = a.w, info->height = a.h;
	info->frames = a.frames;
	info->alpha = 1;
	return NULL;
}

struct gif_out { const struct media_anim *a; struct media_image *out; };

static void
out_rows(void *ctx, uint32_t from, uint32_t to)
{
	struct gif_out *g = ctx;
	uint32_t y;
	int x;

	for (y = from; y < to; y++) {
		const uint8_t *in = g->a->canvas + y * (uint32_t)g->a->w * 4;
		uint8_t *o = g->out->pixels + y * (uint32_t)g->out->stride;

		for (x = 0; x < g->a->w; x++, in += 4, o += g->out->channels)
			m_put(g->out, o, in[0], in[1], in[2], in[3]);
	}
}

const char *
m_gif_decode(const uint8_t *f, uint32_t size, struct media_image *out, int workers)
{
	const char *why;
	struct media_anim *a = anim_open(f, size, workers, 1, &why);
	struct gif_out g;
	int got;

	if (!a)
		return why;
	a->frames = 1;
	media_anim_frame(a, 0, NULL);
	g.a = a, g.out = out;
	got = m_workers_begin(workers);
	m_for((uint32_t)a->h, out_rows, &g, (uint32_t)out->stride, got);
	m_workers_end();
	why = a->frame[0].bad ? "the picture is damaged" : NULL;
	media_anim_close(a);
	return why;
}
