/* Westwood's VQA films (docs/system/media.md, "Films"): version 2, 4x4 or 4x2 blocks, SND0/1/2 sound.
 * Every codebook's pieces are found at the open, so any frame can be made once its codebook
 * and pointers are unpacked; a frame's block rows go to every core but those that unpack the
 * next frames' pointers and codebooks ahead. */
#include <stdlib.h>
#include <sys/mman.h>
#include "m_video.h"

#define PTRS 4		/* frames whose pointers are unpacked ahead */
#define BOOKS 3		/* codebooks kept unpacked */
#define OWN_PLACES 4	/* pictures kept when the program gives no places */

struct piece { uint32_t at, bytes; };
struct vframe { uint32_t vpt, vpt_bytes; int packed, book, palette; };
struct vbook { int first, pieces, packed; uint32_t bytes; };	/* its pieces, and their bytes in all */

/* An unpacking for a worker: from the file's pieces (one, or a codebook's several) into `to`. */
struct unpack {
	const uint8_t *file;
	const struct piece *pieces;
	int count, packed;
	uint8_t *join;		/* where several packed pieces are put end to end first */
	uint8_t *to;
	uint32_t length;
};
struct slot { uint8_t *at; int which, state; mcw_handle h; struct unpack job; };	/* state 0 free, 1 out, 2 there */

struct vqa {
	const uint8_t *file;
	uint32_t size;
	int w, h, tall, bpr, rows, frames, fps, version, rate;
	uint32_t cb_bytes, ptr_bytes, join_bytes;
	struct vframe *fr;
	struct vbook *books;
	struct piece *pieces, *sounds;
	int nbooks, npieces, nsounds, npals, sound_kind, number;
	uint32_t (*pals)[256];
	/* the sound: decoded in order by one job, which counts the frames it has made */
	int16_t *pcm;
	volatile uint32_t *made;
	uint32_t pcm_frames, pcm_bytes;
	int channels, bits;
	mcw_handle sound_call;
	volatile int stop;
	/* the pictures */
	uint8_t **places;
	int slots, own_places;
	int done, asked, rowing, workers, row_workers;
	struct slot ptr[PTRS], cb[BOOKS];
	uint32_t cb_virtual;
	struct m_vq job;
};

/* The open films, for the jobs: a job's words are 32 bits, an address on this computer may not be. */
#define OPEN_MOST 8
static struct vqa *films[OPEN_MOST];

static const int16_t ima_step[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
	118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
	6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const int8_t zap2[4] = { -2, -1, 0, 1 };
static const int8_t zap4[16] = { -9, -8, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 8 };

static inline int clip16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* ---------------------------------------------------------------- the sound */

/* One channel's 4-bit IMA codes of an SND2 chunk (sosCODECDecompressData): a byte is two
 * samples, low half first; a stereo chunk's bytes are left and right in turn. */
static void
ima(const uint8_t *in, uint32_t bytes, int step_in, int16_t *out, int step_out, int *predicted, int *index)
{
	int sample = *predicted, at = *index;
	uint32_t i;

	for (i = 0; i < bytes; i++, in += step_in) {
		int half, code = *in;

		for (half = 0; half < 2; half++, code >>= 4) {
			int n = code & 15, step = ima_step[at], diff = step >> 3;

			if (n & 4) diff += step;
			if (n & 2) diff += step >> 1;
			if (n & 1) diff += step >> 2;
			if (n & 8) diff = -diff;
			sample = clip16(sample + diff);
			at += ima_index[n & 7];
			at = at < 0 ? 0 : at > 88 ? 88 : at;
			*out = (int16_t)sample;
			out += step_out;
		}
	}
	*predicted = sample, *index = at;
}

/* Westwood's own 8-bit ADPCM (Audio_Unzap): bytes 0 to 255 out. */
static uint32_t
unzap(const uint8_t *in, const uint8_t *in_end, uint8_t *out, uint32_t size)
{
	int sample = 0x80, count, code, i;
	uint32_t left = size;

	while (left > 0 && in < in_end) {
		code = *in >> 6;
		count = *in++ & 0x3f;
		if (code == 2) {
			if (count & 0x20) {
				sample += (int8_t)(count << 3) >> 3;
				*out++ = (uint8_t)(sample < 0 ? 0 : sample > 255 ? 255 : sample);
				left--;
			} else {
				for (count++; count > 0 && left && in < in_end; count--, left--)
					*out++ = *in++;
				sample = in[-1];
			}
		} else if (code == 1) {
			for (count++; count > 0 && left >= 2 && in < in_end; count--, left -= 2) {
				sample += zap4[*in & 15];
				*out++ = (uint8_t)(sample < 0 ? 0 : sample > 255 ? 255 : sample);
				sample += zap4[*in++ >> 4];
				*out++ = (uint8_t)(sample < 0 ? 0 : sample > 255 ? 255 : sample);
			}
		} else if (code == 0) {
			for (count++; count > 0 && left >= 4 && in < in_end; count--, left -= 4) {
				for (i = 0; i < 8; i += 2) {
					sample += zap2[*in >> i & 3];
					*out++ = (uint8_t)(sample < 0 ? 0 : sample > 255 ? 255 : sample);
				}
				in++;
			}
		} else {
			for (count++; count > 0 && left; count--, left--)
				*out++ = (uint8_t)(sample < 0 ? 0 : sample > 255 ? 255 : sample);
		}
	}
	return size - left;
}

/* Sound frames a chunk makes. */
static uint32_t
chunk_frames(const struct vqa *q, const struct piece *p)
{
	const uint8_t *c = q->file + p->at;

	switch (q->sound_kind) {
	case 2: return q->channels == 2 ? p->bytes / 2 * 2 : p->bytes * 2;
	case 1: return p->bytes >= 4 ? m_le16(c) / (uint32_t)q->channels : 0;
	default: return p->bytes / (uint32_t)(q->bits / 8) / (uint32_t)q->channels;
	}
}

/* Every chunk in order (a chunk's codes go on from the last's), counting what is made. */
static uint32_t
sound_job(uint32_t film, uint32_t unused)
{
	struct vqa *q = films[film];
	int predicted[2] = { 0, 0 }, index[2] = { 0, 0 }, c, k;
	uint32_t at = 0, i, n;

	(void)unused;
	for (k = 0; k < q->nsounds && !q->stop; k++) {
		const struct piece *p = &q->sounds[k];
		const uint8_t *in = q->file + p->at;
		int16_t *out = q->pcm + at * (uint32_t)q->channels;

		n = chunk_frames(q, p);
		if (at + n > q->pcm_frames)
			n = q->pcm_frames - at;
		if (q->sound_kind == 2) {
			if (q->channels == 2)
				for (c = 0; c < 2; c++)
					ima(in + c, n / 2, 2, out + c, 2, &predicted[c], &index[c]);
			else
				ima(in, n / 2, 1, out, 1, &predicted[0], &index[0]);
		} else if (q->sound_kind == 1) {
			uint8_t *bytes = (uint8_t *)out + n * (uint32_t)q->channels;	/* (the back half of its own room) */
			uint32_t samples = n * (uint32_t)q->channels;

			if (m_le16(in) == m_le16(in + 2))
				memcpy(bytes, in + 4, samples = samples < p->bytes - 4 ? samples : p->bytes - 4);
			else
				samples = unzap(in + 4, in + p->bytes, bytes, samples);
			for (i = 0; i < samples; i++)
				out[i] = (int16_t)((bytes[i] - 128) << 8);
		} else if (q->bits == 16) {
			memcpy(out, in, n * (uint32_t)q->channels * 2);
		} else {
			for (i = 0; i < n * (uint32_t)q->channels; i++)
				out[i] = (int16_t)((in[i] - 128) << 8);
		}
		at += n;
		*q->made = at;
	}
	return at;
}

static uint32_t
vqa_sound(struct media_video *v, uint32_t first, int16_t *to, uint32_t most)
{
	struct vqa *q = v->own;
	uint32_t made = *q->made;

	/* (asking helps its worker on at a page it stopped at: nothing else here looks at it) */
	if (q->sound_call && mcw_ready(q->sound_call)) {
		mcw_join(q->sound_call);
		q->sound_call = 0;
		made = *q->made;
	}
	if (first >= made)
		return 0;
	if (most > made - first)
		most = made - first;
	memcpy(to, q->pcm + first * (uint32_t)q->channels, (size_t)most * (uint32_t)q->channels * 2);
	return most;
}

/* ---------------------------------------------------------------- codebooks and pointers */

/* Slot (which & 255) of film (which >> 8)'s pointers (`book` 0) or codebooks (1). */
static uint32_t
unpack_job(uint32_t which, uint32_t book)
{
	const struct vqa *q = films[which >> 8];
	const struct unpack *u = book ? &q->cb[which & 255].job : &q->ptr[which & 255].job;
	uint32_t at = 0;
	int k;

	if (u->count == 1 && u->packed)
		return m_lcw(u->file + u->pieces[0].at, u->pieces[0].bytes, u->to, u->length);
	/* several pieces (a codebook's parts) end to end: into the codebook, or unpacked from `join` */
	for (k = 0; k < u->count; k++) {
		uint32_t n = u->pieces[k].bytes;
		uint8_t *into = u->packed ? u->join : u->to;

		if (at + n > (u->packed ? 0xffffffffu : u->length))
			n = u->length - at;
		memcpy(into + at, u->file + u->pieces[k].at, n);
		at += n;
	}
	return u->packed ? m_lcw(u->join, at, u->to, u->length) : at;
}

static int next_frame(const struct vqa *q) { return (q->done > q->asked ? q->done : q->asked) + 1; }

/* Starts an unpacking on a worker with nothing to do, or makes it here. */
static void
start(struct vqa *q, struct slot *s)
{
	int book = s >= q->cb && s < q->cb + BOOKS;
	uint32_t which = (uint32_t)(q->number << 8) | (uint32_t)(book ? s - q->cb : s - q->ptr);

	s->h = q->workers ? mcw_try_call(unpack_job, which, (uint32_t)book) : 0;
	if (s->h) {
		s->state = 1;
		return;
	}
	unpack_job(which, (uint32_t)book);
	s->state = 2;
}

static void
collect(struct slot *s, int n)
{
	int k;

	for (k = 0; k < n; k++)
		if (s[k].state == 1 && mcw_ready(s[k].h)) {
			mcw_join(s[k].h);
			s[k].state = 2;
		}
}

/* The slot with codebook b unpacked or being unpacked (begun now if it was neither), or -1. */
static int
book(struct vqa *q, int b, int keep)
{
	int k, pick = -1;
	struct vbook *x = &q->books[b];

	for (k = 0; k < BOOKS; k++)
		if (q->cb[k].which == b && q->cb[k].state)
			return k;
	/* (not the one the frame in the cores has: an empty one, else the oldest before b, else the newest) */
	for (k = 0; k < BOOKS; k++) {
		struct slot *c = &q->cb[k];

		if (c->state == 1 || k == keep)
			continue;
		if (pick < 0 || c->state == 0 ||
		    (q->cb[pick].state && (c->which < b ? q->cb[pick].which > b || c->which < q->cb[pick].which : q->cb[pick].which > b && c->which > q->cb[pick].which)))
			pick = k;
		if (c->state == 0)
			break;
	}
	if (pick < 0)
		return -1;
	q->cb[pick].which = b;
	q->cb[pick].job.file = q->file;
	q->cb[pick].job.pieces = q->pieces + x->first;
	q->cb[pick].job.count = x->pieces;
	q->cb[pick].job.packed = x->packed;
	q->cb[pick].job.to = q->cb[pick].at;
	q->cb[pick].job.length = q->cb_bytes;
	start(q, &q->cb[pick]);
	return pick;
}

/* The same for frame n's pointers (a frame whose pointers are not packed needs no slot: -2). */
static int
pointers(struct vqa *q, int n)
{
	int k, pick = -1, next = next_frame(q);

	if (!q->fr[n].packed)
		return -2;
	for (k = 0; k < PTRS; k++)
		if (q->ptr[k].which == n && q->ptr[k].state)
			return k;
	/* (a frame's that is made or let go, or too far on) */
	for (k = 0; k < PTRS && pick < 0; k++)
		if (q->ptr[k].state == 0 || (q->ptr[k].state == 2 && q->ptr[k].which != q->rowing &&
					     (q->ptr[k].which < next || q->ptr[k].which >= next + PTRS)))
			pick = k;
	if (pick < 0)
		return -1;
	q->ptr[pick].which = n;
	q->ptr[pick].job.file = q->file;
	q->ptr[pick].job.pieces = (const struct piece *)&q->fr[n];	/* (vpt, vpt_bytes: a piece) */
	q->ptr[pick].job.count = 1;
	q->ptr[pick].job.packed = 1;
	q->ptr[pick].job.to = q->ptr[pick].at;
	q->ptr[pick].job.length = q->ptr_bytes;
	start(q, &q->ptr[pick]);
	return pick;
}

/* ---------------------------------------------------------------- the pictures */

static uint8_t *place(const struct vqa *q, int n) { return q->places[n % q->slots]; }

static void
vqa_pump(struct media_video *v)
{
	struct vqa *q = v->own;
	int n, k, cb, p, turns;

	if (!q->places) {
		if (v->places) {
			q->places = v->places, q->slots = v->slots;
		} else {
			if (!(q->places = calloc(OWN_PLACES, sizeof *q->places)))
				return;
			q->slots = OWN_PLACES, q->own_places = 1;
			for (k = 0; k < OWN_PLACES; k++)
				if (!(q->places[k] = m_map(v->info.frame_bytes)))
					return;
		}
	}
	if (q->sound_call && mcw_ready(q->sound_call)) {
		mcw_join(q->sound_call);
		q->sound_call = 0;
	}
	for (turns = 0; turns < 2; turns++) {
		collect(q->ptr, PTRS);
		collect(q->cb, BOOKS);
		if (q->rowing >= 0) {
			if (!mcw_for_ready())
				break;
			mcw_for_wait();
			if (q->rowing > q->done)
				q->done = q->rowing;
			q->rowing = -1;
		}
		n = next_frame(q);
		if (n >= q->frames || n > q->asked + q->slots - 1)
			break;
		cb = q->fr[n].book >= 0 ? book(q, q->fr[n].book, -1) : -2;
		p = pointers(q, n);
		if (cb == -1 || p == -1 || (cb >= 0 && q->cb[cb].state != 2) || (p >= 0 && q->ptr[p].state != 2))
			break;
		/* every core's rows of it but this one's and the unpacking workers' */
		q->job.codebook = cb >= 0 ? q->cb[cb].at : q->cb[0].at;
		q->job.pointers = p >= 0 ? q->ptr[p].at : q->file + q->fr[n].vpt;
		q->job.out = place(q, n);
		q->rowing = n;
		if (q->row_workers && !(q->w & 15)) {
			mcw_for_opts o;

			memset(&o, 0, sizeof o);
			o.workers = q->row_workers;
			o.out_bytes = (uint32_t)(q->tall * q->w);
			o.first_idle = 1;
			mcw_for_begin((uint32_t)q->rows, m_vq_rows, &q->job, &o);
		} else {
			m_vq_rows(&q->job, 0, (uint32_t)q->rows);
		}
		/* and the next frames' pointers and codebooks, ahead */
		for (k = n + 1; k < q->frames && k < n + PTRS; k++) {
			if (q->fr[k].book >= 0 && book(q, q->fr[k].book, cb) == -1)
				break;
			if (pointers(q, k) == -1)
				break;
		}
	}
}

static int
vqa_ready(struct media_video *v, int *in_time)
{
	struct vqa *q = v->own;

	*in_time = 1;
	return q->done > q->asked ? q->done : q->asked;
}

static int
vqa_seek(struct media_video *v, int n)
{
	struct vqa *q = v->own;

	/* (frames stand by themselves: from n itself) */
	if (q->rowing >= 0) {
		mcw_for_wait();
		q->rowing = -1;
	}
	q->done = q->asked = n - 1;
	return n;
}

static const uint8_t *
vqa_frame(struct media_video *v, int n, const uint32_t **palette)
{
	struct vqa *q = v->own;

	if (n < q->asked || (n > q->done && n - 1 < q->asked))
		vqa_seek(v, n);
	if (n - 1 > q->asked)
		q->asked = n - 1;
	if (n > q->done) {
		vqa_pump(v);
		if (n > q->done)
			return NULL;
	}
	q->asked = n;
	*palette = q->npals ? q->pals[q->fr[n].palette] : NULL;
	return place(q, n);
}

static void
vqa_close(struct media_video *v)
{
	struct vqa *q = v->own;
	int k;

	q->stop = 1;
	if (q->rowing >= 0)
		mcw_for_wait();
	if (q->sound_call)
		mcw_join(q->sound_call);
	for (k = 0; k < PTRS; k++) {
		if (q->ptr[k].state == 1)
			mcw_join(q->ptr[k].h);
		m_unmap(q->ptr[k].at, q->ptr_bytes);
	}
	for (k = 0; k < BOOKS; k++) {
		if (q->cb[k].state == 1)
			mcw_join(q->cb[k].h);
		if (q->cb[k].at)
			munmap(q->cb[k].at, q->cb_virtual);
		m_unmap(q->cb[k].job.join, q->join_bytes);
	}
	if (q->own_places)
		for (k = 0; k < q->slots; k++)
			m_unmap(q->places[k], v->info.frame_bytes);
	if (q->own_places)
		free(q->places);
	m_unmap((void *)q->made, q->pcm_bytes);
	free(q->fr);
	free(q->books);
	free(q->pieces);
	free(q->sounds);
	free(q->pals);
	films[q->number] = NULL;
	m_unmap(q, sizeof *q);
}

static const struct m_film vqa_film = { vqa_frame, vqa_ready, vqa_seek, vqa_sound, vqa_pump, vqa_close, NULL };

/* ---------------------------------------------------------------- the file taken apart */

static int
grow(void *array, int count, size_t each)
{
	void **a = array;

	if (count & 63)
		return 1;
	return (*a = realloc(*a, (size_t)(count + 64) * each)) != NULL;
}

/* A palette of 6-bit colours (packed or not) as 0x00RRGGBB words. */
static int
palette(struct vqa *q, const uint8_t *c, uint32_t bytes, int packed)
{
	uint8_t raw[1792];
	uint32_t n = packed ? m_lcw(c, bytes, raw, sizeof raw) : bytes > sizeof raw ? sizeof raw : bytes, k;

	if (!packed)
		memcpy(raw, c, n);
	if (!grow(&q->pals, q->npals, sizeof *q->pals))
		return 0;
	for (k = 0; k < 256; k++) {
		uint32_t r = 3 * k + 2 < n ? raw[3 * k] & 63 : 0, g = 3 * k + 2 < n ? raw[3 * k + 1] & 63 : 0, b = 3 * k + 2 < n ? raw[3 * k + 2] & 63 : 0;

		q->pals[q->npals][k] = (r << 2 | r >> 4) << 16 | (g << 2 | g >> 4) << 8 | (b << 2 | b >> 4);
	}
	q->npals++;
	return 1;
}

static const char *
scan(struct vqa *q, int groupsize)
{
	const uint8_t *f = q->file;
	uint32_t at = 12, end = 8 + m_be32(f + 4) < q->size ? 8 + m_be32(f + 4) : q->size;
	int parts = 0, building = -1, books_done = 0, pal = -1, frame_open = 0, frames = 0;
	int book_now = -1;

	/* a chunk, or a frame's chunks one after another (the loader's order: VQA_LoadFrame) */
	while (at + 8 <= end) {
		const uint8_t *c = f + at;
		uint32_t bytes = m_be32(c + 4), inner = at + 8, inner_end;
		int container = !memcmp(c, "VQFR", 4) || !memcmp(c, "VQFK", 4) || !memcmp(c, "VQFL", 4);

		if (bytes > end - at - 8)
			bytes = end - at - 8;
		inner_end = container ? inner + bytes : at + 8 + bytes;
		if (!frame_open) {
			/* a frame uses the codebook the last one left whole (the first one's own when none is) */
			book_now = books_done ? books_done - 1 : 0;
			frame_open = 1;
		}
		for (;;) {
			const uint8_t *s = container ? f + inner : c;
			uint32_t size = container ? m_be32(s + 4) : bytes, data = (container ? inner : at) + 8;
			int ends = 0;

			if (container && (inner + 8 > inner_end || size > inner_end - inner - 8))
				break;
			if (!memcmp(s, "CBF0", 4) || !memcmp(s, "CBFZ", 4) || !memcmp(s, "CBP0", 4) || !memcmp(s, "CBPZ", 4)) {
				int whole = s[2] == 'F', packed = s[3] == 'Z';

				if (whole || building < 0 || parts == 0) {
					if (!grow(&q->books, q->nbooks, sizeof *q->books))
						return "not enough memory for the film";
					building = q->nbooks++;
					q->books[building].first = q->npieces;
					q->books[building].pieces = 0;
					q->books[building].bytes = 0;
					q->books[building].packed = packed;
					parts = 0;
				}
				if (!grow(&q->pieces, q->npieces, sizeof *q->pieces))
					return "not enough memory for the film";
				q->pieces[q->npieces].at = data;
				q->pieces[q->npieces++].bytes = size;
				q->books[building].pieces++;
				q->books[building].bytes += size;
				if (whole || ++parts == groupsize) {
					books_done = building + 1;
					parts = 0;
					building = -1;
				}
			} else if (!memcmp(s, "CPL0", 4) || !memcmp(s, "CPLZ", 4)) {
				if (!palette(q, f + data, size, s[3] == 'Z'))
					return "not enough memory for the film";
				pal = q->npals - 1;
			} else if (!memcmp(s, "VPT", 3) || !memcmp(s, "VPRZ", 4)) {
				if (!grow(&q->fr, frames, sizeof *q->fr))
					return "not enough memory for the film";
				q->fr[frames].vpt = data;
				q->fr[frames].vpt_bytes = size;
				q->fr[frames].packed = s[3] != '0' && s[3] != 'R';
				q->fr[frames].book = q->nbooks ? book_now : -1;
				q->fr[frames].palette = pal;
				ends = !container;
				frames++;
			} else if (!container && (!memcmp(s, "SND0", 4) || !memcmp(s, "SND1", 4) || !memcmp(s, "SND2", 4))) {
				if (!grow(&q->sounds, q->nsounds, sizeof *q->sounds))
					return "not enough memory for the film";
				q->sound_kind = s[3] - '0';
				q->sounds[q->nsounds].at = data;
				q->sounds[q->nsounds++].bytes = size;
			}
			if (!container) {
				if (ends)
					frame_open = 0;
				break;
			}
			inner += 8 + ((size + 1) & ~1u);
		}
		if (container)
			frame_open = 0;
		at += 8 + ((bytes + 1) & ~1u);
	}
	if (!frames)
		return "a VQA film with no frames";
	q->frames = frames;
	/* (frames before any codebook was whole wait for the first) */
	for (frames = 0; frames < q->frames; frames++) {
		if (q->fr[frames].book < 0 && q->nbooks)
			q->fr[frames].book = 0;
		if (q->fr[frames].book >= books_done)
			q->fr[frames].book = books_done ? books_done - 1 : -1;
		if (q->fr[frames].palette < 0 && q->npals)
			q->fr[frames].palette = 0;
	}
	return NULL;
}

static const char *
refuse(struct media_video *v, const char *why)
{
	vqa_close(v);
	v->own = NULL;
	return why;
}

const char *
m_vqa_open(struct media_video *v, const uint8_t *data, uint32_t size, int workers)
{
	struct vqa *q;
	const uint8_t *h;
	const char *why;
	uint32_t at, join = 0;
	volatile uint32_t sum = 0;
	int k, bw, bh, group;

	if (size < 12 + 8 + 42 || memcmp(data + 12, "VQHD", 4) || m_be32(data + 16) < 42)
		return "a VQA film without its header";
	h = data + 20;
	bw = h[10], bh = h[11];
	if (m_le16(h) < 1 || m_le16(h) > 2 || bw != 4 || (bh != 4 && bh != 2))
		return "a VQA film this does not play (it plays version 2, 4x4 or 4x2 blocks)";
	for (k = 0; k < OPEN_MOST && films[k]; k++)
		;
	if (k == OPEN_MOST)
		return "too many VQA films open";
	if (!(q = m_map(sizeof *q)))
		return "not enough memory for the film";
	films[k] = q;
	q->number = k;
	v->own = q;
	q->done = q->asked = q->rowing = -1;
	q->file = data, q->size = size;
	/* its pages here, so that no worker stops at one */
	for (at = 0; at < size; at += 4096)
		sum += data[at];
	(void)sum;
	q->version = (int)m_le16(h);
	q->w = (int)m_le16(h + 6), q->h = (int)m_le16(h + 8);
	q->tall = bh;
	q->fps = h[12] ? h[12] : 15;
	group = h[13] ? h[13] : 8;
	q->bpr = q->w / 4, q->rows = q->h / bh;
	q->cb_bytes = ((uint32_t)bw * bh * m_le16(h + 16) + 250) & ~3u;
	q->ptr_bytes = 2u * (uint32_t)q->bpr * (uint32_t)q->rows;
	q->rate = q->version >= 2 && m_le16(h + 24) ? (int)m_le16(h + 24) : 22050;
	q->channels = q->version >= 2 && h[26] == 2 ? 2 : 1;
	q->bits = q->version >= 2 && h[27] == 16 ? 16 : 8;
	if (q->w < 4 || q->h < bh || q->w > 2048 || q->h > 2048 || (q->w & 3) || q->h % bh)
		return refuse(v, "a VQA film of a size this does not play");
	if ((why = scan(q, group)) != NULL)
		return refuse(v, why);
	if (!(h[2] & 1))
		q->nsounds = 0;	/* (its header says it has none) */

	/* codebooks in room for any pointer (a block's index is two bytes): what is never written is never there */
	q->cb_virtual = ((bh == 4 ? 0xff00u : 0x0f00u) * (uint32_t)(bw * bh) + 4095) & ~4095u;
	if (q->cb_virtual < q->cb_bytes)
		q->cb_virtual = (q->cb_bytes + 4095) & ~4095u;
	for (k = 0; k < q->nbooks; k++)
		if (q->books[k].pieces > 1 && q->books[k].packed && q->books[k].bytes > join)
			join = q->books[k].bytes;
	q->join_bytes = join;
	for (k = 0; k < BOOKS; k++) {
		void *m = mmap(NULL, q->cb_virtual, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (m == MAP_FAILED)
			return refuse(v, "not enough memory for the film");
		q->cb[k].at = m;
		q->cb[k].which = -1;
		mcw_touch(m, q->cb_bytes);
		if (join && !(q->cb[k].job.join = m_map(join)))
			return refuse(v, "not enough memory for the film");
	}
	for (k = 0; k < PTRS; k++) {
		q->ptr[k].which = -1;
		if (!(q->ptr[k].at = m_map(q->ptr_bytes)))
			return refuse(v, "not enough memory for the film");
	}
	q->job.blocks_per_row = (uint32_t)q->bpr, q->job.num_rows = (uint32_t)q->rows;
	q->job.out_width = (uint32_t)q->w, q->job.tall = q->tall;

	/* the sound: its frames in all, made by a worker of its own from the start */
	for (k = 0; k < q->nsounds; k++)
		q->pcm_frames += chunk_frames(q, &q->sounds[k]);
	q->pcm_bytes = 16 + q->pcm_frames * (uint32_t)q->channels * 2;
	if (!(q->made = m_map(q->pcm_bytes)))
		return refuse(v, "not enough memory for the film");
	q->pcm = (int16_t *)((uint8_t *)q->made + 16);

	/* rows on all but some workers, which unpack (and one makes the sound at first) */
	q->workers = m_film_workers(v, workers);
	q->row_workers = q->workers - (q->workers >= 8 ? 3 : q->workers >= 4 ? 2 : q->workers >= 2 ? 1 : 0);
	mcw_touch_code((const void *)m_vq_rows, 1024);
	mcw_touch_code((const void *)unpack_job, 4096);
	mcw_touch_code((const void *)sound_job, 4096);
	if (q->pcm_frames && !(q->sound_call = q->workers ? mcw_try_call(sound_job, (uint32_t)q->number, 0) : 0))
		sound_job((uint32_t)q->number, 0);

	v->film = &vqa_film;
	v->info.kind = MEDIA_VQA;
	v->info.width = q->w;
	v->info.height = q->h;
	v->info.fps_milli = q->fps * 1000;
	v->info.frames = q->frames;
	v->info.format = MEDIA_FRAME_PAL8;
	v->info.frame_bytes = ((uint32_t)q->w * (uint32_t)q->h + 15) & ~15u;
	v->info.rate = q->pcm_frames ? q->rate : 0;
	v->info.channels = q->pcm_frames ? q->channels : 0;
	v->info.sound_frames = q->pcm_frames;
	v->info.workers = q->workers;
	v->info.least_places = 2;
	return NULL;
}
