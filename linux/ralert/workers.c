/*
 * Red Alert's side of the machine's worker cores (workers.h): its jobs, which are functions
 * here that a worker calls in the game's own memory, over the library every program that
 * uses the workers has and its second layer (programs/mc/mcwx.h: the next frame's pointers by
 * mcw_try_call), and libmedia's Westwood pieces (a frame's rows over every core, LCW).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcwx.h"
#include "media.h"	/* linux/libmedia: a movie's rows and pointers */
#include "workers.h"
#include "host.h"	/* linux/tdawn: the scene has a worker core between movies */

unsigned workers_frames;

static int count = -1, at_exit;
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

/* LCW_Uncompress (common/lcw.cpp) as libmedia has it, which may unpack into the front of the buffer it reads the back of. */
static uint32_t lcw_job(uint32_t job, uint32_t unused)
{
	const struct workers_lcw *j = (const struct workers_lcw *)job;

	(void)unused;
	media_lcw(j->source, j->dest, j->length);
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

	/* rows of whole texels, or two cores would store to one */
	if (workers_count() == 0 || (buff_width & 15) != 0 || blocks_per_row * 4 > buff_width || num_rows < 2 || !target
	    || buffer < target || buffer + bytes > target + target_bytes || ((uintptr_t)buffer & 15) != 0)
		return 0;
	/* every core rows of it, this one too (the worker that unpacks the next frame is left to it) */
	media_vq4_rows(codebook, pointers, buffer, blocks_per_row, num_rows, buff_width, workers_count());
	workers_frames++;
	return 1;
}
