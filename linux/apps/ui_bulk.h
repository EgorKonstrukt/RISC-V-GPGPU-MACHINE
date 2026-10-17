/* ui_bulk.h: megabytes copied or filled by the worker cores (docs/system/nanox.md, "Paint"): a core
 * keeps a few kilobytes of new stores a pass, so a picture moved by one core is a hundred
 * passes that end early. The workers are borrowed (mcw_lend): with another program at work
 * on them this core does it alone. The program links programs/mc/mcw.c. */
#ifndef SHADEREMU_APPS_UI_BULK_H
#define SHADEREMU_APPS_UI_BULK_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mcw.h"

#define BULK_WORKERS 96
#define BULK_LEAST 16384	/* bytes: less, and this core is done sooner by itself */
#define BULK_HELD_LEAST 4096	/* the same for a job that is held: its hand-over is not waited for */
#define BULK_QUEUE 6		/* jobs held to be given out together */

/* Rows of `bytes` each, `to_step` and `from_step` apart; from NULL: filled with `word`. After
 * its bytes a row has `tail` more, filled with `word`. With `two`, each row is then copied
 * (or filled) a second time, to2 from from2: a copy of what the first is about to replace.
 * unit: a worker's part is a multiple of so many rows (rows that touch end on 16 bytes so). */
struct bulk_job {
	uint8_t *to; const uint8_t *from; uint32_t bytes, rows, to_step, from_step, word, unit, tail;
	int two; uint8_t *to2; const uint8_t *from2; uint32_t to2_step, from2_step;
};

static struct bulk_job bulk_queue[BULK_QUEUE];	/* read by the workers once posted: not written until they are done */
static int bulk_queued, bulk_held, bulk_out;	/* jobs waiting, bulk_hold()s open, workers with a job out */
static int bulk_count = -1;

static void
bulk_words(uint8_t *to, uint32_t bytes, uint32_t value)
{
	uint32_t *word = (uint32_t *)to, n;

	for (n = bytes / 4; n; n--)
		*word++ = value;
}

static void
bulk_plain(const struct bulk_job *j)
{
	uint32_t row;

	for (row = 0; row < j->rows; row++) {
		uint8_t *to = j->to + row * j->to_step;

		if (j->from)
			memcpy(to, j->from + row * j->from_step, j->bytes);
		else
			bulk_words(to, j->bytes, j->word);
		if (j->tail)
			bulk_words(to + j->bytes, j->tail, j->word);
		if (!j->two)
			continue;
		if (j->from2)
			memcpy(j->to2 + row * j->to2_step, j->from2 + row * j->from2_step, j->bytes);
		else
			bulk_words(j->to2 + row * j->to2_step, j->bytes, j->word);
	}
}

/* A worker's part of a queued job: `part` is its first unit and how many (16 bits each). */
static uint32_t
bulk_work(uint32_t index, uint32_t part)
{
	struct bulk_job j = bulk_queue[index];
	uint32_t first = (part >> 16) * j.unit;

	j.to += first * j.to_step;
	if (j.from)
		j.from += first * j.from_step;
	if (j.two) {
		j.to2 += first * j.to2_step;
		if (j.from2)
			j.from2 += first * j.from2_step;
	}
	j.rows = (part & 0xffff) * j.unit;
	bulk_plain(&j);
	return 0;
}

static int
bulk_open(void)
{
	extern char __DATA_BEGIN__[], _end[];

	if (bulk_count < 0) {
		mcw_lend();
		bulk_count = mcw_open(BULK_WORKERS);
		if (bulk_count > 0) {
			mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
			atexit(mcw_close);
		} else {
			bulk_count = 0;
		}
	}
	return bulk_count;
}

/* A worker stops at a page that is not there yet, and waits for this core to look: a byte of
 * each page stored as it is (of the job's own bytes: never one before them). */
static void
bulk_pages(uint8_t *to, uint32_t step, uint32_t bytes, uint32_t rows)
{
	volatile uint8_t *at = to, *end = to + (rows - 1) * step + bytes;

	for (; at < end; at = (volatile uint8_t *)(((uintptr_t)at | 4095) + 1))
		*at = *at;
}

static uint32_t
bulk_weight(const struct bulk_job *j)
{
	return ((j->bytes + j->tail) >> 4) * j->rows * (j->two ? 2u : 1u) + 1;
}

/* The queued jobs given out: each its share of the workers by its bytes, in parts of whole
 * units; what is left over of a job's rows is this core's. Not waited for. */
static void
bulk_post(void)
{
	uint32_t total = 0, weight, units, each, more, unit, first, count, rows;
	int n = bulk_queued ? bulk_open() : 0, left, q, k = 0, share;

	for (q = 0; q < bulk_queued; q++)
		total += bulk_weight(&bulk_queue[q]);
	/* as many workers as are through in the passes it takes to post to them: a post is some
	 * hundred instructions and a worker keeps 2 KB a pass, so the square root of bytes / 128 */
	for (left = 4; left < n && (uint32_t)(left * left) < total / 8; left++)
		continue;
	n = left = left < n ? left : n;
	for (q = 0; q < bulk_queued; q++) {
		struct bulk_job *j = &bulk_queue[q], rest;

		unit = j->unit, units = j->rows / unit;
		weight = bulk_weight(j);
		share = (int)((uint64_t)n * weight / total);
		share = share < 1 ? 1 : share;
		share = share > left - (bulk_queued - 1 - q) ? left - (bulk_queued - 1 - q) : share;
		/* (a worker's part is two units or more: one is not worth its job) */
		if ((uint32_t)share > units / 2)
			share = (int)(units / 2);
		if (share < 1) {
			bulk_plain(j);
			continue;
		}
		left -= share;
		bulk_pages(j->to, j->to_step, j->bytes + j->tail, j->rows);
		if (j->two)
			bulk_pages(j->to2, j->to2_step, j->bytes, j->rows);
		each = units / (uint32_t)share, more = units % (uint32_t)share;
		for (first = 0; share > 0; share--, first += count) {
			count = each + (more ? 1 : 0), more -= more ? 1 : 0;
			mcw_post(++k, bulk_work, (uint32_t)q, first << 16 | count);
		}
		rows = units * unit;
		if (rows < j->rows) {
			rest = *j;
			rest.to += rows * j->to_step, rest.rows = j->rows - rows;
			if (rest.from)
				rest.from += rows * j->from_step;
			if (rest.two)
				rest.to2 += rows * j->to2_step;
			if (rest.two && rest.from2)
				rest.from2 += rows * j->from2_step;
			bulk_plain(&rest);
		}
	}
	bulk_queued = 0;
	bulk_out = k;
}

/* Everything asked for so far is done: the memory is this core's to read and write again. */
static void
bulk_wait(void)
{
	int k;

	if (bulk_queued)
		bulk_post();
	for (k = 1; k <= bulk_out; k++)
		mcw_wait(k);
	bulk_out = 0;
}

/* From here to bulk_go() large jobs are held and then given out together: they must be of
 * memory apart from each other's (no 16 bytes of two), and nothing between may need them done. */
static void
bulk_hold(void)
{
	if (!bulk_held++)
		bulk_wait();
}

/* later: the jobs are left with the workers, and bulk_wait() comes before their memory is
 * next used (any job asked for waits first by itself). */
static void
bulk_go(int later)
{
	bulk_held--;
	if (!later)
		bulk_wait();
	else if (!bulk_held && bulk_queued)
		bulk_post();
}

/* The job done now, or held. What it writes must be the caller's alone meanwhile, rows must
 * not overlap, and two workers' rows must not meet in the same 16 bytes (the callers see to
 * that). */
static void
bulk_do(struct bulk_job all)
{
	all.unit = all.unit > 1 ? all.unit : 1;
	while (all.rows / all.unit > 0xffff)
		all.unit *= 2;
	if (!bulk_held)
		bulk_wait();
	if ((all.bytes + all.tail) * all.rows * (all.two ? 2u : 1u) < (bulk_held ? BULK_HELD_LEAST : BULK_LEAST) || all.rows / all.unit < 4 ||
	    bulk_open() <= 0) {
		bulk_plain(&all);
		return;
	}
	if (bulk_queued == BULK_QUEUE)
		bulk_wait();
	bulk_queue[bulk_queued++] = all;
	if (!bulk_held)
		bulk_wait();
}

/* Whether rows as `all` has them may be shared out, and with what unit: rows that touch must
 * begin on 16 bytes and a part of them end on 16. */
static int
bulk_shared(uint8_t *to, uint32_t to_step, uint32_t bytes, uint32_t *unit)
{
	if (to_step >= bytes + 16)
		return 1;
	if (to_step != bytes || ((uintptr_t)to & 15) || (bytes & 3))
		return 0;
	while ((bytes * *unit) & 15)
		*unit *= 2;
	return 1;
}

/* Rows of a picture copied into another: `bytes` a row, the rows `to_step` and `from_step`
 * apart; each row's next `tail` bytes are filled with `word`. */
static void
bulk_rows_tail(void *to, uint32_t to_step, const void *from, uint32_t from_step, uint32_t bytes, uint32_t rows, uint32_t tail, uint32_t word)
{
	struct bulk_job all = { to, from, bytes, rows, to_step, from_step, word, 1, tail };

	if (bulk_shared(to, to_step, bytes + tail, &all.unit)) {
		bulk_do(all);
	} else {
		if (!bulk_held)
			bulk_wait();
		bulk_plain(&all);
	}
}

static void
bulk_rows(void *to, uint32_t to_step, const void *from, uint32_t from_step, uint32_t bytes, uint32_t rows)
{
	bulk_rows_tail(to, to_step, from, from_step, bytes, rows, 0, 0);
}

/* Rows copied twice: first into `kept` from `was`, then into `was` from `now` (what a change
 * replaced is kept, and the change taken for what is). Rows of `kept` touch. */
static void
bulk_rows_kept(void *kept, void *was, const void *now, uint32_t step, uint32_t bytes, uint32_t rows)
{
	struct bulk_job all = { kept, was, bytes, rows, bytes, step, 0, 1, 0, 1, was, now, step, step };

	if (bulk_shared(kept, bytes, bytes, &all.unit) && (step >= bytes + 16 || (step == bytes && !((uintptr_t)was & 15)))) {
		bulk_do(all);
	} else {
		bulk_rows(kept, bytes, was, step, bytes, rows);
		bulk_rows(was, step, now, step, bytes, rows);
	}
}

/* Rows filled with a word: `bytes` a row (whole words), the rows `to_step` apart. */
static void
bulk_fill_rows(void *to, uint32_t to_step, uint32_t bytes, uint32_t rows, uint32_t word)
{
	struct bulk_job all = { to, NULL, bytes, rows, to_step, 0, word, 1 };

	if (to_step < bytes + 16) {
		if (!bulk_held)
			bulk_wait();
		bulk_plain(&all);
	} else
		bulk_do(all);
}

/* One piece of memory copied, or (from NULL) filled with a word: in pieces of 1 KB. With
 * `also`, a second piece of the same size and place in its 16 bytes gets the same. */
static void
bulk_flat(void *to, void *also, const void *from, uint32_t bytes, uint32_t word)
{
	uint8_t *at = to, *at2 = also;
	const uint8_t *src = from;
	uint32_t head = (uint32_t)(-(uintptr_t)to & 15), rows;
	struct bulk_job all = { 0 };

	if (also && (((uintptr_t)to ^ (uintptr_t)also) & 15)) {
		bulk_flat(to, NULL, from, bytes, word);
		bulk_flat(also, NULL, from, bytes, word);
		return;
	}
	if (bytes < BULK_LEAST)
		head = bytes;
	/* what is before the first whole 16 bytes, and after the last whole piece, is this core's */
	all.to = at, all.from = src, all.bytes = head, all.rows = 1, all.word = word, all.unit = 1;
	all.two = also != NULL, all.to2 = at2, all.from2 = src;
	bulk_do(all);
	at += head, bytes -= head;
	if (src)
		src += head;
	if (at2)
		at2 += head;
	rows = bytes / 1024;
	if (rows) {
		all.to = at, all.from = all.from2 = src, all.to2 = at2, all.bytes = 1024, all.rows = rows;
		all.to_step = all.from_step = all.to2_step = all.from2_step = 1024;
		bulk_do(all);
	}
	all.to = at + rows * 1024, all.from = all.from2 = src ? src + rows * 1024 : NULL, all.to2 = at2 ? at2 + rows * 1024 : NULL;
	all.bytes = bytes - rows * 1024, all.rows = 1;
	bulk_do(all);
}

#endif
