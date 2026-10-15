/* Motion JPEG in AVI (docs/media.md, "MJPEG in AVI"): every picture is a JPEG by itself, decoded
 * ahead into its place a few at a time, their restart intervals over every core, and made the
 * GPU's opaque words there. PCM sound is copied out of the file's chunks as it is asked for. */
#include <stdio.h>
#include <stdlib.h>
#include "m_video.h"

#define PLACES_MOST 65	/* pictures kept ahead at most */
#define ROUND 8		/* pictures a round */
#define PLACES_BYTES (24u << 20)

struct avi_chunk { uint32_t at, bytes, first; };	/* in the file; a sound chunk's first frame */

struct avi {
	const uint8_t *file;
	uint32_t size;
	int w, h, frames, workers;
	struct avi_chunk *pic, *snd;	/* a picture's JPEG (an empty chunk: the one before again) */
	int npic, nsnd, video, audio;	/* the streams' numbers */
	int bits, channels, rate, align;
	uint32_t pcm_frames;
	uint8_t **places;
	int slots, own_places, done, asked;
	uint8_t *rooms;			/* a round's pictures': its intervals' count, the decoder's state, it with tables */
	uint32_t room, pic_most, rooms_bytes;
	int first, out, phase;		/* the round's first picture, how many, which half is out (1, 2) */
	uint32_t part_first[ROUND + 1];	/* each picture's first job of the second half */
	int own_want;			/* places of its own when the program gives none */
	char other[5];			/* the pictures' codec when it is not MJPEG */
};

static int
grow(void *array, int count, size_t each)
{
	void **a = array;
	void *more;

	if (count & 63)
		return 1;
	if (!(more = realloc(*a, (size_t)(count + 64) * each)))
		return 0;
	*a = more;
	return 1;
}

/* ---------------------------------------------------------------- the standard tables (JPEG's K.3) */

static const uint8_t dht_bits[4][16] = {
	{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
	{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
};
static const uint8_t ac_luma[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t ac_chroma[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
#define DHT_BYTES (2 + 2 + 4 * 17 + 2 * 12 + 2 * 162)

static void
dht_put(uint8_t *p)
{
	static const uint8_t classes[4] = { 0x00, 0x10, 0x01, 0x11 };
	int t, k, n;

	*p++ = 0xff, *p++ = 0xc4, *p++ = (DHT_BYTES - 2) >> 8, *p++ = (DHT_BYTES - 2) & 255;
	for (t = 0; t < 4; t++) {
		*p++ = classes[t];
		for (k = n = 0; k < 16; k++)
			n += *p++ = dht_bits[t][k];
		for (k = 0; k < n; k++)
			*p++ = t & 1 ? (t == 1 ? ac_luma : ac_chroma)[k] : (uint8_t)k;
	}
}

/* A picture as the decoder takes it: an MJPEG camera's leaves the standard tables out (AVI1). */
static const uint8_t *
with_tables(const struct avi *a, const uint8_t *f, uint32_t *bytes, uint8_t *tabled)
{
	uint32_t at = 2, n = *bytes;

	while (at + 4 <= n && f[at] == 0xff) {
		int marker = f[at + 1];

		if (marker == 0xc4)
			return f;
		if (marker == 0xda)
			break;
		at += 2 + ((uint32_t)f[at + 2] << 8 | f[at + 3]);
	}
	if (at + 4 > n || f[at + 1] != 0xda || n > a->pic_most)
		return f;
	memcpy(tabled, f, at);
	dht_put(tabled + at);
	memcpy(tabled + at + DHT_BYTES, f + at, n - at);
	*bytes = n + DHT_BYTES;
	return tabled;
}

/* ---------------------------------------------------------------- the pictures */

static uint8_t *place(const struct avi *a, int n) { return a->places[n % a->slots]; }

/* The decoder's words are 0xffRRGGBB; the GPU's top byte is transparency. */
static void
opaque_rows(void *ctx, uint32_t from, uint32_t to)
{
	struct media_image *im = ctx;
	uint32_t y;
	int x;

	for (y = from; y < to; y++) {
		uint32_t *row = (uint32_t *)(im->pixels + y * (uint32_t)im->stride);

		for (x = 0; x < im->width; x++)
			row[x] &= 0xffffffu;
	}
}

/* A round's picture k: its intervals' count, the picture as the decoder writes it (it keeps
 * pointing there), the decoder's state, the file with tables put in. */
#define HEAD 64
static uint8_t *room_of(const struct avi *a, int k) { return a->rooms + (uint32_t)k * (HEAD + a->room + a->pic_most + DHT_BYTES + 16); }

/* The round's first half: each picture's tables read and its restart intervals found, on a core
 * of its own (its count in its room); one without them is decoded there whole. */
static void
picture_heads(void *ctx, uint32_t from, uint32_t to)
{
	struct avi *a = ctx;

	for (; from < to; from++) {
		uint8_t *room = room_of(a, (int)from);
		const struct avi_chunk *c = &a->pic[a->first + (int)from];
		uint32_t bytes = c->bytes, *count = (uint32_t *)room;
		const uint8_t *f = a->file + c->at;
		struct media_image *im = (struct media_image *)(room + 16);

		memset(im, 0, sizeof *im);
		im->width = a->w, im->height = a->h, im->channels = 4, im->bgr = 1, im->stride = a->w * 4;
		im->pixels = place(a, a->first + (int)from);
		*count = 0;
		if (bytes >= 4 && f[0] == 0xff && f[1] == 0xd8)
			f = with_tables(a, f, &bytes, room + HEAD + a->room);
		if (bytes < 4 || f[0] != 0xff || f[1] != 0xd8)
			memset(im->pixels, 0, (uint32_t)a->w * (uint32_t)a->h * 4);	/* (damaged: black) */
		else if (!(*count = m_jpeg_parts(f, bytes, im, room + HEAD))) {
			if (m_jpeg_decode_alone(f, bytes, im, room + HEAD))
				memset(im->pixels, 0, (uint32_t)a->w * (uint32_t)a->h * 4);	/* (or another size) */
			else
				opaque_rows(im, 0, (uint32_t)a->h);
		}
	}
}

/* The second half: jobs from..to-1 of all the round's intervals, each picture's in turn. */
static void
picture_parts(void *ctx, uint32_t from, uint32_t to)
{
	struct avi *a = ctx;
	int k = 0;

	while (from < to) {
		uint32_t end;

		while (a->part_first[k + 1] <= from)
			k++;
		end = to < a->part_first[k + 1] ? to : a->part_first[k + 1];
		m_jpeg_part(room_of(a, k) + HEAD, from - a->part_first[k], end - a->part_first[k], 1);
		from = end;
	}
}

/* Where the first half left the counts: the second half's jobs. */
static uint32_t
parts_counted(struct avi *a)
{
	int k;

	a->part_first[0] = 0;
	for (k = 0; k < a->out; k++)
		a->part_first[k + 1] = a->part_first[k] + *(uint32_t *)room_of(a, k);
	return a->part_first[a->out];
}

static void
round_begin(struct avi *a, uint32_t n, mcw_body fn)
{
	mcw_for_opts o;

	memset(&o, 0, sizeof o);
	o.workers = a->workers, o.first_idle = 1, o.grain = fn == picture_heads ? 1 : 0;
	mcw_for_begin(n, fn, a, &o);
}

static void
avi_pump(struct media_video *v)
{
	struct avi *a = v->own;
	int n, k, turns;

	if (!a->places) {
		if (v->places) {
			a->places = v->places, a->slots = v->slots < PLACES_MOST ? v->slots : PLACES_MOST;
		} else {
			int own = a->own_want;

			if (!(a->places = calloc((size_t)own, sizeof *a->places)))
				return;
			a->slots = own, a->own_places = 1;
			for (k = 0; k < own; k++)
				if (!(a->places[k] = m_map(v->info.frame_bytes)))
					return;
		}
	}
	/* a round out goes on when its half is made: the player shows on meanwhile */
	if (a->phase) {
		uint32_t jobs;

		if (!mcw_for_ready())
			return;
		mcw_for_wait();
		if (a->phase == 1 && (jobs = parts_counted(a)) != 0) {
			round_begin(a, jobs, picture_parts);
			a->phase = 2;
			return;
		}
		a->done = a->first + a->out - 1;
		a->phase = 0;
	}
	/* the next pictures that have places free (shown ones') */
	n = (a->done > a->asked ? a->done : a->asked) + 1;
	turns = a->asked + a->slots - 1 - n + 1;
	if (turns > a->frames - n)
		turns = a->frames - n;
	if (turns > ROUND)
		turns = ROUND;
	if (turns <= 0)
		return;
	a->first = n, a->out = turns;
	if (!a->workers) {
		picture_heads(a, 0, (uint32_t)turns);
		picture_parts(a, 0, parts_counted(a));
		a->done = n + turns - 1;
		return;
	}
	round_begin(a, (uint32_t)turns, picture_heads);
	a->phase = 1;
}

/* The round out, waited for: its places are written no more. */
static void
round_back(struct avi *a)
{
	if (!a->phase)
		return;
	mcw_for_wait();
	if (a->phase == 1 && a->workers) {
		uint32_t jobs = parts_counted(a);

		if (jobs)
			m_for(jobs, picture_parts, a, 0, a->workers);
	}
	a->done = a->first + a->out - 1;
	a->phase = 0;
}

static int
avi_ready(struct media_video *v, int *in_time)
{
	struct avi *a = v->own;

	*in_time = 1;
	return a->done > a->asked ? a->done : a->asked;
}

static int
avi_seek(struct media_video *v, int n)
{
	struct avi *a = v->own;

	/* (every picture stands by itself: from n itself) */
	round_back(a);
	a->done = a->asked = n - 1;
	return n;
}

static const uint8_t *
avi_frame(struct media_video *v, int n, const uint32_t **palette)
{
	struct avi *a = v->own;

	if (n < a->asked || (n > a->done && n - 1 < a->asked))
		avi_seek(v, n);
	if (n - 1 > a->asked)
		a->asked = n - 1;
	if (n > a->done) {
		avi_pump(v);
		if (n > a->done)
			return NULL;
	}
	a->asked = n;
	*palette = NULL;
	return place(a, n);
}

/* ---------------------------------------------------------------- the sound */

static uint32_t
avi_sound(struct media_video *v, uint32_t first, int16_t *to, uint32_t most)
{
	struct avi *a = v->own;
	int low = 0, high = a->nsnd - 1, middle, ch = a->channels;
	uint32_t got = 0;

	if (first >= a->pcm_frames)
		return 0;
	while (low < high) {
		middle = (low + high + 1) / 2;
		if (a->snd[middle].first <= first)
			low = middle;
		else
			high = middle - 1;
	}
	for (; low < a->nsnd && got < most; low++) {
		const struct avi_chunk *c = &a->snd[low];
		uint32_t skip = first + got - c->first, n = c->bytes / (uint32_t)a->align - skip, i;
		const uint8_t *in = a->file + c->at + skip * (uint32_t)a->align;

		if (n > most - got)
			n = most - got;
		if (a->bits == 16)
			memcpy(to + got * (uint32_t)ch, in, (size_t)n * (uint32_t)ch * 2);
		else
			for (i = 0; i < n * (uint32_t)ch; i++)
				to[got * (uint32_t)ch + i] = (int16_t)((in[i] - 128) << 8);
		got += n;
	}
	return got;
}

static void
avi_close(struct media_video *v)
{
	struct avi *a = v->own;
	int k;

	if (!a)
		return;
	round_back(a);
	if (a->own_places) {
		for (k = 0; k < a->slots; k++)
			m_unmap(a->places[k], v->info.frame_bytes);
		free(a->places);
	}
	m_unmap(a->rooms, a->rooms_bytes);
	free(a->pic);
	free(a->snd);
	free(a);
	v->own = NULL;
}

static const struct m_film avi_film = { avi_frame, avi_ready, avi_seek, avi_sound, avi_pump, avi_close, NULL };

/* ---------------------------------------------------------------- the file taken apart */

static int
four(const uint8_t *p, const char *what)
{
	return !memcmp(p, what, 4);
}

/* A stream's header and format (LIST strl): which is the MJPEG pictures, which the PCM sound. */
static void
stream(struct avi *a, const uint8_t *p, uint32_t bytes, int number, uint32_t *scale, uint32_t *rate)
{
	const uint8_t *strh = NULL, *strf = NULL, *end = p + bytes;
	uint32_t strf_bytes = 0;

	while (p + 8 <= end) {
		uint32_t n = m_le32(p + 4);

		if (n > (uint32_t)(end - p - 8))
			break;
		if (four(p, "strh") && n >= 36)
			strh = p + 8;
		else if (four(p, "strf"))
			strf = p + 8, strf_bytes = n;
		p += 8 + ((n + 1) & ~1u);
	}
	if (!strh || !strf)
		return;
	if (four(strh, "vids") && a->video < 0 && strf_bytes >= 20) {
		const uint8_t *c = strf + 16;	/* biCompression */

		if ((c[0] | 32) == 'm' && (c[1] | 32) == 'j' && (c[2] | 32) == 'p' && (c[3] | 32) == 'g')
			a->video = number;
		else if (four(c, "JPEG") || four(c, "jpeg") || four(c, "AVRn") || four(c, "dmb1"))
			a->video = number;
		else {
			if (!a->other[0])
				memcpy(a->other, c, 4);
			return;
		}
		a->w = (int)m_le32(strf + 4);
		a->h = (int)m_le32(strf + 8);
		if (a->h < 0)
			a->h = -a->h;
		*scale = m_le32(strh + 20), *rate = m_le32(strh + 24);
	} else if (four(strh, "auds") && a->audio < 0 && strf_bytes >= 16 && m_le16(strf) == 1) {
		a->channels = (int)m_le16(strf + 2);
		a->rate = (int)m_le32(strf + 4);
		a->align = (int)m_le16(strf + 12);
		a->bits = (int)m_le16(strf + 14);
		if ((a->bits == 8 || a->bits == 16) && (a->channels == 1 || a->channels == 2) && a->align == a->channels * a->bits / 8 && a->rate > 0)
			a->audio = number;
	}
}

/* The chunks of a LIST movi (and of its LIST rec), in the file's order. */
static int
movi(struct avi *a, const uint8_t *p, const uint8_t *end)
{
	while (p + 8 <= end) {
		uint32_t n = m_le32(p + 4);
		int number = p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' ? (p[0] - '0') * 10 + p[1] - '0' : -1;

		if (n > (uint32_t)(end - p - 8))
			n = (uint32_t)(end - p - 8);	/* (a file cut short: what is there) */
		if (four(p, "LIST") && n >= 4) {
			if (!movi(a, p + 12, p + 8 + n))
				return 0;
		} else if (number >= 0 && number == a->video && (p[2] == 'd' || p[2] == 'D')) {
			if (!grow(&a->pic, a->npic, sizeof *a->pic))
				return 0;
			a->pic[a->npic].at = (uint32_t)(p + 8 - a->file), a->pic[a->npic].bytes = n;
			/* an empty chunk is the picture before shown again */
			if (!n && a->npic)
				a->pic[a->npic] = a->pic[a->npic - 1];
			if (n || a->npic)
				a->npic++;
		} else if (number >= 0 && number == a->audio && p[2] == 'w' && p[3] == 'b' && n >= (uint32_t)a->align) {
			if (!grow(&a->snd, a->nsnd, sizeof *a->snd))
				return 0;
			a->snd[a->nsnd].at = (uint32_t)(p + 8 - a->file);
			a->snd[a->nsnd].bytes = n - n % (uint32_t)a->align;
			a->snd[a->nsnd].first = a->pcm_frames;
			a->pcm_frames += n / (uint32_t)a->align;
			a->nsnd++;
		}
		p += 8 + ((n + 1) & ~1u);
	}
	return 1;
}

/* Why a film's pictures are not played: their codec by its four letters. */
static const char *
not_mjpeg(const struct avi *a)
{
	static const char *const kinds[][2] = { { "FMP4 XVID DIVX DX50 MP4V M4S2 3IV2 XVIX", "MPEG-4 Part 2" },
						{ "H264 X264 AVC1 DAVC", "H.264" }, { "HEVC H265 HVC1 HEV1", "H.265" } };
	static char why[96];
	char up[5] = { 0 };
	const char *kind = NULL;
	int i;

	if (!a->other[0])
		return "an AVI film without MJPEG pictures (this plays MJPEG with PCM sound)";
	for (i = 0; i < 4; i++)
		up[i] = a->other[i] >= 'a' && a->other[i] <= 'z' ? (char)(a->other[i] - 32) : a->other[i] < ' ' ? '?' : a->other[i];
	for (i = 0; i < 3 && !kind; i++)
		if (strstr(kinds[i][0], up))
			kind = kinds[i][1];
	snprintf(why, sizeof why, "%s (%s) pictures: not played (this plays MJPEG in AVI)", kind ? kind : "a codec this does not know", up);
	return why;
}

/* Every RIFF of the file (an OpenDML file goes on in RIFF AVIX): its headers, its LIST movi. */
static const char *
scan(struct avi *a, uint32_t *scale, uint32_t *rate)
{
	const uint8_t *f = a->file, *end = f + a->size, *riff = f;

	while (riff + 12 <= end && four(riff, "RIFF")) {
		uint32_t rn = m_le32(riff + 4);
		const uint8_t *p = riff + 12, *stop;

		if (rn > (uint32_t)(end - riff - 8))
			rn = (uint32_t)(end - riff - 8);
		stop = riff + 8 + rn;
		while (p + 12 <= stop) {
			uint32_t n = m_le32(p + 4);

			if (n > (uint32_t)(stop - p - 8))
				n = (uint32_t)(stop - p - 8);
			if (four(p, "LIST") && four(p + 8, "hdrl")) {
				const uint8_t *q = p + 12, *qend = p + 8 + n;
				int number = 0;

				while (q + 12 <= qend) {
					uint32_t m = m_le32(q + 4);

					if (m > (uint32_t)(qend - q - 8))
						break;
					if (four(q, "LIST") && m >= 4 && four(q + 8, "strl"))
						stream(a, q + 12, m - 4, number++, scale, rate);
					q += 8 + ((m + 1) & ~1u);
				}
				if (a->video < 0)
					return not_mjpeg(a);
			} else if (four(p, "LIST") && four(p + 8, "movi")) {
				if (!movi(a, p + 12, p + 8 + n))
					return "not enough memory for the film";
			}
			p += 8 + ((n + 1) & ~1u);
		}
		riff = stop + (rn & 1);
	}
	if (a->video < 0)
		return "an AVI film without its headers";
	if (!a->npic)
		return "an AVI film with no pictures";
	return NULL;
}

const char *
m_avi_open(struct media_video *v, const uint8_t *data, uint32_t size, int workers)
{
	struct avi *a = calloc(1, sizeof *a);
	const char *why;
	uint32_t scale = 0, rate = 0, at;
	volatile uint32_t sum = 0;

	if (!a)
		return "not enough memory for the film";
	v->own = a;
	a->file = data, a->size = size;
	a->video = a->audio = -1;
	a->done = a->asked = -1;
	if ((why = scan(a, &scale, &rate)) != NULL) {
		avi_close(v);
		return why;
	}
	if (a->w < 8 || a->h < 8 || a->w > M_MOST_SIDE || a->h > M_MOST_SIDE) {
		avi_close(v);
		return "an AVI film of a size this does not play";
	}
	/* its pages here, so that no worker stops at one */
	for (at = 0; at < size; at += 4096)
		sum += data[at];
	(void)sum;
	a->frames = a->npic;
	a->workers = m_film_workers(v, workers);
	/* each core's room, made here (a worker's first store to a new page waits for this core) */
	for (at = 0; at < (uint32_t)a->npic; at++)
		if (a->pic[at].bytes > a->pic_most)
			a->pic_most = (a->pic[at].bytes + 15) & ~15u;
	a->room = m_jpeg_parts_room(a->pic_most + DHT_BYTES);
	a->rooms_bytes = ROUND * (HEAD + a->room + a->pic_most + DHT_BYTES + 16);
	if (!(a->rooms = m_map(a->rooms_bytes))) {
		avi_close(v);
		return "not enough memory for the film";
	}
	v->film = &avi_film;
	v->info.kind = MEDIA_AVI;
	v->info.width = a->w;
	v->info.height = a->h;
	v->info.frames = a->frames;
	v->info.fps_milli = scale && rate ? (int)((uint64_t)rate * 1000u / scale) : 25000;
	if (v->info.fps_milli < 1)
		v->info.fps_milli = 1;
	v->info.format = MEDIA_FRAME_WORDS;
	v->info.frame_bytes = ((uint32_t)a->w * (uint32_t)a->h * 4 + 15) & ~15u;
	v->info.rate = a->pcm_frames ? a->rate : 0;
	v->info.channels = a->pcm_frames ? a->channels : 0;
	v->info.sound_frames = a->pcm_frames;
	v->info.workers = a->workers;
	/* (it plays in two places; it decodes rounds ahead in as many as it is given, and keeps three
	 * rounds' of its own as far as 24 MB go) */
	v->info.least_places = 2;
	a->own_want = 3 * ROUND + 1;
	while (a->own_want > 2 && (uint32_t)a->own_want * v->info.frame_bytes > PLACES_BYTES)
		a->own_want--;
	return NULL;
}
