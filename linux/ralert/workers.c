/*
 * Red Alert's side of the machine's worker cores (workers.h): its jobs, which are functions
 * here that a worker calls in the game's own memory, over the library every program that
 * uses the workers has and its second layer (programs/mc/mcwx.h: a frame's rows by mcw_for,
 * the next frame's pointers by mcw_try_call).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcwx.h"
#include "workers.h"
#include "host.h"	/* linux/tdawn: the scene has a worker core between movies */

unsigned workers_frames;

static int count = -1, at_exit;
static struct workers_unvq shared __attribute__((aligned(16)));
static struct workers_lcw unpacking __attribute__((aligned(16)));
static mcw_handle unpacker;
/* the memory the workers are to write a frame into */
static uint8_t *target;
static uint32_t target_bytes;
/* what workers_alloc() gave: whole 16 bytes each, so no core stores beside another's */
#define HELD_MOST 64
static uint8_t *held[HELD_MOST];
static uint32_t held_bytes[HELD_MOST];

/* ---- the jobs ---- */

/* UnVQ_4x4 (common/unvqbuff.cpp) for the block rows from `first` up to `end`. */
static void unvq_rows(const struct workers_unvq *job, uint32_t first, uint32_t end)
{
	const uint32_t *codebook = (const uint32_t *)job->codebook;
	const uint8_t *low = job->pointers + first * job->blocks_per_row;
	const uint8_t *high = low + job->blocks_per_row * job->num_rows;
	uint32_t width = job->buff_width;
	uint8_t *row = job->buffer + first * 4 * width;

	for (uint32_t r = first; r < end; r++, row += 4 * width) {
		uint8_t *out = row;
		for (uint32_t b = 0; b < job->blocks_per_row; b++, out += 4) {
			uint32_t p1 = *low++, p2 = *high++;
			if (p2 == 255) {
				uint32_t colour = p1 * 0x01010101u;
				*(uint32_t *)out = colour;
				*(uint32_t *)(out + width) = colour;
				*(uint32_t *)(out + 2 * width) = colour;
				*(uint32_t *)(out + 3 * width) = colour;
			} else {
				const uint32_t *block = codebook + 4 * ((p2 << 8) | p1);
				*(uint32_t *)out = block[0];
				*(uint32_t *)(out + width) = block[1];
				*(uint32_t *)(out + 2 * width) = block[2];
				*(uint32_t *)(out + 3 * width) = block[3];
			}
		}
	}
}

/* LCW_Uncompress (common/lcw.cpp), which may unpack into the front of the buffer it reads the back of. */
static void lcw(const struct workers_lcw *job)
{
	const uint8_t *source = job->source;
	uint8_t *dest = job->dest, *to = dest, *end = dest + job->length;
	const uint8_t *from;
	uint32_t count, op;

	while (to < end) {
		op = *source++;
		if (!(op & 0x80)) {
			count = (op >> 4) + 3;
			from = to - (*source++ + ((op & 0x0f) << 8));
		} else if (!(op & 0x40)) {
			if (op == 0x80)
				return;
			count = op & 0x3f;
			if (count > (uint32_t)(end - to))
				count = end - to;
			while (count--)
				*to++ = *source++;
			continue;
		} else if (op == 0xfe) {
			uint8_t value;
			count = source[0] + (source[1] << 8);
			value = source[2];
			source += 3;
			if (count > (uint32_t)(end - to))
				count = end - to;
			while (count--)
				*to++ = value;
			continue;
		} else if (op == 0xff) {
			count = source[0] + (source[1] << 8);
			from = dest + source[2] + (source[3] << 8);
			source += 4;
		} else {
			count = (op & 0x3f) + 3;
			from = dest + source[0] + (source[1] << 8);
			source += 2;
		}
		if (count > (uint32_t)(end - to))
			count = end - to;
		while (count--)
			*to++ = *from++;
	}
}

static void unvq_piece(void *job, uint32_t from, uint32_t to)
{
	unvq_rows((const struct workers_unvq *)job, from, to);
}

static uint32_t lcw_job(uint32_t job, uint32_t unused)
{
	(void)unused;
	lcw((const struct workers_lcw *)job);
	return 0;
}

/* ---- the game's side ---- */

int workers_count(void)
{
	const char *limit = getenv("RALERT_WORKERS");

	if (count >= 0) return count;
	host_workers_close();	/* the mission's scene had one: a movie has them all */
	/* fifteen: more share a frame's rows no faster (14.2 s of the opening movie with 15, 14.9 with 63) */
	count = mcw_open(limit ? atoi(limit) : 15);
	if (count > 0 && !at_exit) {
		at_exit = 1;
		atexit(mcw_close);
	}
	if (getenv("RALERT_STATS_MS") || getenv("RALERT_FRAMES")) fprintf(stderr, "ralert: %d worker cores\n", count);
	return count;
}

void *workers_alloc(unsigned bytes)
{
	void *memory;
	unsigned i, at;

	if (workers_count() == 0) return NULL;
	for (i = 0; i < HELD_MOST && held[i]; i++) {}
	bytes = (bytes + 15) & ~15u;
	if (i == HELD_MOST || posix_memalign(&memory, 16, bytes) != 0) return NULL;
	/* its pages here now, or a worker stops at each until this core looks */
	for (at = 0; at < bytes; at += 4096) ((volatile uint8_t *)memory)[at] = 0;
	((volatile uint8_t *)memory)[bytes - 1] = 0;
	held[i] = memory;
	held_bytes[i] = bytes;
	return memory;
}

int workers_owns(const void *memory)
{
	unsigned i;

	for (i = 0; i < HELD_MOST; i++)
		if (held[i] && (const uint8_t *)memory >= held[i] && (const uint8_t *)memory < held[i] + held_bytes[i]) return 1;
	return 0;
}

void workers_release(void *memory)
{
	unsigned i;

	for (i = 0; i < HELD_MOST; i++)
		if (memory && held[i] == memory) {
			workers_lcw_wait();
			held[i] = NULL;
			free(memory);
		}
}

/* The last worker unpacks; the others, and it when it has nothing to unpack, share frames. */
void workers_lcw_wait(void)
{
	if (!unpacker) return;
	mcw_join(unpacker);
	unpacker = 0;
}

int workers_lcw(const uint8_t *source, uint8_t *dest, unsigned length)
{
	if (workers_count() < 2 || !workers_owns(source) || !workers_owns(dest) || ((uintptr_t)dest & 15) != 0) return 0;
	workers_lcw_wait();
	unpacking.source = source;
	unpacking.dest = dest;
	unpacking.length = length;
	unpacker = mcw_try_call(lcw_job, (uint32_t)&unpacking, 0);
	return unpacker != 0;
}

void workers_target(void *memory, unsigned physical, unsigned bytes)
{
	(void)physical;
	target = memory;
	target_bytes = memory ? bytes : 0;
	/* the movie is over: the cores are another program's to have until the next one */
	if (!memory && count > 0) {
		workers_lcw_wait();
		mcw_for_end();
		mcw_close();
		count = -1;
	}
}

int workers_unvq(uint8_t *codebook, uint8_t *pointers, uint8_t *buffer, unsigned blocks_per_row, unsigned num_rows,
		 unsigned buff_width)
{
	unsigned bytes = num_rows * 4 * buff_width;
	mcw_for_opts rows = {0};

	/* rows of whole texels, or two cores would store to one */
	if (workers_count() == 0 || (buff_width & 15) != 0 || blocks_per_row * 4 > buff_width || num_rows < 2 || !target
	    || buffer < target || buffer + bytes > target + target_bytes || ((uintptr_t)buffer & 15) != 0)
		return 0;
	shared.codebook = codebook;
	shared.pointers = pointers;
	shared.buffer = buffer;
	shared.blocks_per_row = blocks_per_row;
	shared.num_rows = num_rows;
	shared.buff_width = buff_width;

	/* every core rows of it, this one too (the worker that unpacks the next frame is left to it) */
	rows.out_bytes = 4 * buff_width;
	mcw_for_opt(num_rows, unvq_piece, &shared, &rows);
	workers_frames++;
	return 1;
}
