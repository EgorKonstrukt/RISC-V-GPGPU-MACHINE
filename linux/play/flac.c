/*
 * dr_flac (https://github.com/mackron/dr_libs, public domain or MIT No Attribution) for
 * nxplay: a FLAC file in memory, read as 16-bit samples. build.sh fetches the header.
 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#include "dr_flac.h"
#include "flac.h"

static drflac *flac;

int
flac_open(const void *file, unsigned size, int *rate, int *channels, unsigned *frames)
{
	flac = drflac_open_memory(file, size, NULL);
	if (!flac)
		return 0;
	*rate = (int)flac->sampleRate;
	*channels = flac->channels;
	*frames = flac->totalPCMFrameCount > 0xfffffffful ? 0 : (unsigned)flac->totalPCMFrameCount;
	return 1;
}

int
flac_read(short *to, int frames)
{
	return (int)drflac_read_pcm_frames_s16(flac, (drflac_uint64)frames, to);
}

unsigned
flac_seek(unsigned frame)
{
	if (drflac_seek_to_pcm_frame(flac, frame))
		return frame;
	drflac_seek_to_pcm_frame(flac, 0);
	return 0;
}

/* ---- its blocks, each decoded by itself: for the worker cores (docs/play.md) ---- */

#include <sys/mman.h>
#include "mcw.h"

#define DECODERS 64
static const unsigned char *bytes;
static unsigned size, block_frames, block_count, total_frames;
static unsigned *block_at;	/* where each block's frame begins; 0: not looked for yet */
static unsigned known, known_to;	/* the block the search goes on from, and the byte it has got to */
static drflac *pool[DECODERS];

/* A decoder is memory of its own, whole pages: a worker stores to it, and nobody else. */
static void *
pages(size_t n, void *unused)
{
	unsigned *memory = mmap(NULL, n + 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (memory == MAP_FAILED)
		return NULL;
	memory[0] = (unsigned)n + 16;
	return memory + 4;
}

static void
pages_free(void *p, void *unused)
{
	if (p)
		munmap((unsigned *)p - 4, ((unsigned *)p)[-4]);
}

static void *
pages_again(void *p, size_t n, void *unused)
{
	void *to = pages(n, unused);
	size_t had = p ? ((unsigned *)p)[-4] - 16 : 0;

	if (to && p)
		memcpy(to, p, had < n ? had : n);
	pages_free(p, unused);
	return to;
}

static const drflac_allocation_callbacks in_pages = { NULL, pages, pages_again, pages_free };

/* The frame header at p, if one is there: its block's number and frames, and the header's
 * own length (0 for none). A stream of blocks all as long numbers them in a UTF-8 code. */
static unsigned
header(const unsigned char *p, const unsigned char *end, unsigned *number, unsigned *frames)
{
	unsigned n, more, length, code, rate, crc = 0, i, k;

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
static unsigned
next_header(unsigned at, unsigned end, unsigned *number)
{
	unsigned frames, word;

	if (end > size)
		end = size;
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
		if (bytes[at] == 0xff && bytes[at + 1] == 0xf8 && header(bytes + at, bytes + size, number, &frames) && *number < block_count &&
		    (frames == block_frames || (*number == block_count - 1 && frames == total_frames - *number * block_frames)))
			return at;
		at++;
	}
	return 0;
}

int
flac_blocks(const void *file, unsigned length, unsigned *frames_a_block)
{
	unsigned number, first;

	bytes = file;
	size = length;
	free(block_at);
	block_at = NULL;
	/* blocks all as long, a length the file says, and a first block where it should be */
	if (!flac || !flac->maxBlockSizeInPCMFrames || !flac->totalPCMFrameCount || flac->totalPCMFrameCount > 0xfffffffful ||
	    flac->container != drflac_container_native)
		return 0;
	block_frames = flac->maxBlockSizeInPCMFrames;
	total_frames = (unsigned)flac->totalPCMFrameCount;
	first = (unsigned)flac->firstFLACFramePosInBytes;
	block_count = (total_frames + block_frames - 1) / block_frames;
	if (!first || next_header(first, first + 16, &number) != first || number != 0 ||
	    (block_at = calloc(block_count + 1, sizeof block_at[0])) == NULL)
		return 0;
	block_at[0] = first;
	known = 0;
	known_to = first + 1;
	*frames_a_block = block_frames;
	return 1;
}

unsigned
flac_block_at(unsigned block, unsigned most)
{
	unsigned at, number, after, tries, from, end;

	if (block >= block_count)
		return 0;
	if (block_at[block])
		return block_at[block];
	if (block < known || block - known > 24) {
		/* elsewhere in the tune: from the nearest block known before it, and when that is far, by
		 * guesses at where it lies (a header found there says which block it is) */
		for (known = block; !block_at[known]; known--)
			;
		known_to = block_at[known] + 1;
		for (tries = 0; block - known > 24 && tries < 12; tries++) {
			from = (unsigned)((unsigned long long)(size - block_at[known]) * (block - known) / (block_count - known));
			from = block_at[known] + from - (from >> (tries < 3 ? 4 - tries : 1));
			at = next_header(from, size, &number);
			/* (it counts when the block after it follows it) */
			if (!at || number > block || number <= known)
				continue;
			if (number + 1 < block_count && (!next_header(at + 1, at + (1u << 20), &after) || after != number + 1))
				break;
			block_at[number] = at;
			known = number;
			known_to = at + 1;
		}
	}
	end = known_to + most;
	while (known < block) {
		at = next_header(known_to, end, &number);
		if (!at) {
			known_to = end >= size ? size : end - 16;
			return 0;
		}
		known_to = at + 1;
		if (number == known + 1)
			block_at[++known] = at;
	}
	return block_at[block];
}

int
flac_decoder(int n)
{
	if (n < 0 || n >= DECODERS)
		return 0;
	if (!pool[n] && (pool[n] = drflac_open_memory(bytes, size, &in_pages)) != NULL)
		mcw_touch((unsigned *)pool[n] - 4, ((unsigned *)pool[n])[-4]);
	return pool[n] != NULL;
}

void
flac_from(int n, unsigned at)
{
	drflac *f = pool[n];

	drflac__seek_to_byte(&f->bs, at);
	memset(&f->currentFLACFrame, 0, sizeof f->currentFLACFrame);
}

int
flac_next(int n, short *to, int frames)
{
	return (int)drflac_read_pcm_frames_s16(pool[n], (drflac_uint64)frames, to);
}

void
flac_close(void)
{
	int n;

	if (flac)
		drflac_close(flac);
	flac = NULL;
	for (n = 0; n < DECODERS; n++) {
		if (pool[n])
			drflac_close(pool[n]);
		pool[n] = NULL;
	}
}
