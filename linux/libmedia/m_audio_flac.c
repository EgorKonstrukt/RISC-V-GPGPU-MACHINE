/*
 * FLAC (docs/media.md) over dr_flac (https://github.com/mackron/dr_libs, public domain or MIT
 * No Attribution; build.sh fetches it and applies flac_bound.py). A stream of blocks all as long
 * numbers them, and each stands by itself: a piece is whole blocks, any core's. A block's place
 * in the file is not written anywhere: a decoder finds it by itself by its header (below).
 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#include <stdlib.h>
#include "dr_flac.h"
#include "m_audio.h"

struct flac {
	drflac *reader;
	uint32_t block, blocks, total, first;	/* frames a block, blocks, frames, where block 0 begins */
};

struct flac_dec {
	drflac *f;
	uint32_t next;		/* the block it would decode next without looking for it (~0: none) */
};

/* A decoder's memory is pages of its own: a worker stores to it, and nobody else. */
static void *
pages(size_t n, void *unused)
{
	(void)unused;
	return m_audio_pages((uint32_t)n);
}

static void
pages_free(void *p, void *unused)
{
	(void)unused;
	m_audio_pages_free(p);
}

static void *
pages_again(void *p, size_t n, void *unused)
{
	void *to = pages(n, unused);
	size_t had = p ? ((uint32_t *)p)[-4] - 16 : 0;

	if (to && p)
		memcpy(to, p, had < n ? had : n);
	pages_free(p, unused);
	return to;
}

static const drflac_allocation_callbacks in_pages = { NULL, pages, pages_again, pages_free };

/* The frame header at p, if one is there: its block's number and frames, and the header's
 * own length (0 for none). A stream of blocks all as long numbers them in a UTF-8 code. */
static uint32_t
header(const uint8_t *p, const uint8_t *end, uint32_t *number, uint32_t *frames)
{
	uint32_t n, more, length, code, rate, crc = 0, i, k;

	if (end - p < 16 || p[0] != 0xff || p[1] != 0xf8)
		return 0;
	code = p[2] >> 4;
	rate = p[2] & 15;
	n = p[4];
	if (!code || rate == 15 || p[3] >> 4 > 10 || (p[3] & 1) || (p[3] & 6) == 6 || (n >= 0x80 && n < 0xc0) || n >= 0xfc)
		return 0;
	more = n < 0x80 ? 0 : n < 0xe0 ? 1 : n < 0xf0 ? 2 : n < 0xf8 ? 3 : 4;
	if (more)
		n &= 0x3fu >> more;
	for (i = 0; i < more; i++) {
		if ((p[5 + i] & 0xc0) != 0x80)
			return 0;
		n = n << 6 | (p[5 + i] & 0x3f);
	}
	length = 5 + more;
	if (code == 6)
		*frames = p[length] + 1u, length += 1;
	else if (code == 7)
		*frames = (p[length] << 8 | p[length + 1]) + 1u, length += 2;
	else
		*frames = code == 1 ? 192 : code < 6 ? 576u << (code - 2) : 256u << (code - 8);
	length += rate == 12 ? 1 : rate > 12 ? 2 : 0;
	for (i = 0; i < length; i++)
		for (crc ^= p[i], k = 0; k < 8; k++)
			crc = crc & 0x80 ? (crc << 1 ^ 7) & 0xff : crc << 1;
	if (crc != p[length])
		return 0;
	*number = n;
	return length + 1;
}

/* The next frame header from `at` to `end` that is a block of this stream: where, or 0. */
static uint32_t
next_header(const struct media_audio *a, uint32_t at, uint32_t end, uint32_t *number)
{
	const struct flac *fl = a->own;
	const uint8_t *bytes = a->data;
	uint32_t frames, word;

	if (end > a->size)
		end = a->size;
	while (at + 16 <= end) {
		/* a word at a time while none of its bytes is ff */
		if (!(at & 3)) {
			memcpy(&word, bytes + at, 4);
			word = ~word;
			if (!((word - 0x01010101u) & ~word & 0x80808080u)) {
				at += 4;
				continue;
			}
		}
		if (bytes[at] == 0xff && bytes[at + 1] == 0xf8 && header(bytes + at, bytes + a->size, number, &frames) &&
		    *number < fl->blocks && (frames == fl->block || (*number == fl->blocks - 1 && frames == fl->total - *number * fl->block)))
			return at;
		at++;
	}
	return 0;
}

/* A header found at a guess counts when the block after it follows it. */
static uint32_t
sure_header(const struct media_audio *a, uint32_t at, uint32_t end, uint32_t *number)
{
	const struct flac *fl = a->own;
	uint32_t after;

	while ((at = next_header(a, at, end, number)) != 0) {
		if (*number + 1 == fl->blocks || (next_header(a, at + 1, at + (1u << 20), &after) && after == *number + 1))
			return at;
		at++;
	}
	return 0;
}

/*
 * Where block b begins, found by guesses between two places known (a header found says which
 * block it is) and then header by header: on any core, storing to nothing but its stack.
 */
static uint32_t
locate(const struct media_audio *a, uint32_t b)
{
	const struct flac *fl = a->own;
	uint32_t lo = fl->first, lo_n = 0, hi = a->size, hi_n = fl->blocks, at, n, guess, tries;

	for (tries = 0; tries < 48 && b - lo_n > 2 && hi - lo > 65536; tries++) {
		uint32_t per = (hi - lo) / (hi_n - lo_n);

		/* (a block early, so that the header found is this one's or one before) */
		guess = lo + (uint32_t)((uint64_t)(hi - lo) * (b - lo_n) / (hi_n - lo_n));
		guess = guess > lo + per ? guess - per : lo + 1;
		at = sure_header(a, guess, hi, &n);
		if (!at || n >= hi_n) {
			hi = guess;	/* nothing there: every block wanted is before it */
		} else if (n == b) {
			return at;
		} else if (n < b && n > lo_n) {
			lo = at, lo_n = n;
		} else if (n > b) {
			hi = at, hi_n = n;
		} else {
			break;
		}
	}
	for (at = lo, n = lo_n; n < b;) {
		uint32_t number;

		if (!(at = next_header(a, at + 1, a->size, &number)))
			return 0;
		if (number == n + 1)
			n = number;
	}
	return at;
}

const char *
m_flac_open(struct media_audio *a)
{
	struct flac *fl = calloc(1, sizeof *fl);
	drflac *f;
	uint32_t number;

	if (!fl)
		return "not enough memory";
	if (!(f = drflac_open_memory(a->data, a->size, NULL))) {
		free(fl);
		return "a FLAC file this cannot read";
	}
	if (f->channels > 2) {
		drflac_close(f);
		free(fl);
		return "a FLAC file of more than two channels";
	}
	fl->reader = f;
	a->own = fl;
	a->info.rate = (int)f->sampleRate;
	a->info.channels = f->channels;
	a->info.frames = f->totalPCMFrameCount > 0xfffffffful ? 0 : (uint32_t)f->totalPCMFrameCount;
	/* in pieces: blocks all as long, a length the file says, and a first block where it should be */
	if (!f->maxBlockSizeInPCMFrames || !a->info.frames || f->container != drflac_container_native)
		return NULL;
	fl->block = f->maxBlockSizeInPCMFrames;
	fl->total = a->info.frames;
	fl->first = (uint32_t)f->firstFLACFramePosInBytes;
	fl->blocks = (fl->total + fl->block - 1) / fl->block;
	if (fl->first && next_header(a, fl->first, fl->first + 16, &number) == fl->first && number == 0)
		a->unit = fl->block;
	return NULL;
}

int
m_flac_read(struct media_audio *a, int16_t *to, int frames)
{
	const struct flac *fl = a->own;

	return (int)drflac_read_pcm_frames_s16(fl->reader, (drflac_uint64)frames, to);
}

uint32_t
m_flac_seek(struct media_audio *a, uint32_t frame)
{
	const struct flac *fl = a->own;

	if (drflac_seek_to_pcm_frame(fl->reader, frame))
		return frame;
	drflac_seek_to_pcm_frame(fl->reader, 0);
	return 0;
}

int
m_flac_decoder(struct media_audio *a, int d)
{
	struct flac_dec *x = m_audio_pages(sizeof *x);

	if (!x)
		return 0;
	if (!(x->f = drflac_open_memory(a->data, a->size, &in_pages))) {
		m_audio_pages_free(x);
		return 0;
	}
	x->next = ~0u;
	a->dec[d] = x;
	return 1;
}

uint32_t
m_flac_step(struct media_audio *a, int d, uint32_t piece, int16_t *to, uint32_t done)
{
	const struct flac *fl = a->own;
	struct flac_dec *x = a->dec[d];
	uint32_t ch = (uint32_t)a->info.channels, want = media_audio_piece_frames(a, piece);
	uint32_t block = (media_audio_piece_first(a, piece) + done) / fl->block, n = want - done, at, got;

	if (n > fl->block)
		n = fl->block;
	if (x->next != block) {
		if (!(at = locate(a, block)))
			goto silence;
		drflac__seek_to_byte(&x->f->bs, at);
		memset(&x->f->currentFLACFrame, 0, sizeof x->f->currentFLACFrame);
	}
	got = (uint32_t)drflac_read_pcm_frames_s16(x->f, n, to + done * ch);
	if (got < n) {
		done += got;
		goto silence;
	}
	x->next = block + 1;
	return done + n;
silence:
	/* (a block that is not there or not whole) */
	memset(to + done * ch, 0, (want - done) * ch * 2);
	x->next = ~0u;
	return want;
}

void
m_flac_close(struct media_audio *a)
{
	struct flac *fl = a->own;
	int d;

	for (d = 0; d < a->decoders; d++) {
		struct flac_dec *x = a->dec[d];

		drflac_close(x->f);
		m_audio_pages_free(x);
	}
	drflac_close(fl->reader);
	free(fl);
}
