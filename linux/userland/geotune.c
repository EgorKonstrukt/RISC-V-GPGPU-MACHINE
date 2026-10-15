/*
 * geotune: how the worker cores are laid out on the card this machine runs on (docs/multicore.md,
 * "Packing the small workers"). Run by the init (emuinit.c) before any program asks for workers,
 * since the layout may only change while every worker is parked; the kernel keeps a layout that
 * is set when the first program asks (workers_take() in shaderemu_gpu.c).
 *
 * The geometry is always the hybrid one: workers 1 to 16 big (caches of 2^5 buckets), which is
 * what keeps a game's few workers as fast as before, and the rest small (2^3). What depends on
 * the card is how the small ones' blocks lie in the tick's pixels: a card that runs every one of
 * them side by side wants each in a block of eight columns of its own, a card that does not
 * wants them packed two columns apart, so that fewer warps carry them. Which it is, is measured:
 * every small worker in a block of eight, the same job on an eighth of them and on all of them,
 * in turn, three times. A card that runs them all at once takes as long for all as for the
 * eighth. The choice is kept in the registry with the time the eighth took, and on a later boot
 * only that time is measured again: a choice made on another card, or one whose speed has
 * changed since, is made again.
 *
 *   geotune          what the init runs: nothing when a layout is set already
 *   geotune again    measures and chooses though the registry has a choice (no program may have
 *                    workers: run it before the desktop)
 *
 * Registry keys (registry.h): geotune.width (the small workers' blocks: 2 or 8 columns),
 * geotune.workers, geotune.iterations and geotune.us (the eighth's job and its microseconds),
 * geotune.ratio (all against the eighth, in hundredths), geotune.force (2, 4 or 8: that width,
 * nothing measured).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "mcw.h"
#include "registry.h"

#define CONTROL_OFFSET 0x01000000	/* the control words in /dev/gpu (docs/gpu.md) */
#define HOST_FLAGS (0x3c / 4)		/* bit 0: the host starts the desktop at boot */
#define CORES_NOW (0x3c0 / 4)		/* how many cores the geometry has now */
#define CORES_MOST (0x3cc / 4)		/* how many the machine could have */
#define SHAPE (0x3d0 / 4)		/* the geometry: bit 0 while laid out anew, workers, bits, the hybrid's word */
#define BIG_COUNT 16
#define BIG_BITS 5
#define SMALL_BITS 3
#define DENSE 2				/* columns of a small worker's block on a card that cannot run them all */
#define WIDE 8				/* and on one that can: a block of eight to itself */
#define ALL_AT_ONCE 150			/* all the small workers' job against an eighth's, in hundredths: less on a card that runs them all */
#define DRIFT 25			/* a later boot's time against the kept one, in hundredths, that is the same card still */
#define EARLY "/tmp/nxpersist.early"	/* the kept folder was loaded here (the nx script then does not) */

static volatile uint32_t *control, *boxes;

/* (whole microseconds: the program has no floating point, which the C library's would bring) */
static uint32_t now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)t.tv_sec * 1000000u + (uint32_t)t.tv_nsec / 1000u;
}

/* the hybrid's word: how many big workers and their bits, the small and the big blocks' widths */
static uint32_t hybrid(uint32_t big, uint32_t small_width, uint32_t big_width)
{
	return (big ? BIG_COUNT | BIG_BITS << 8 : 0) | small_width << 16 | big_width << 24;
}

/* Lays the workers out anew, as the kernel does (workers_layout()): every core parked, the strip
 * nothing but zeros for some passes, then the new geometry. How many cores it has then. */
static uint32_t lay_out(uint32_t workers, uint32_t bits, uint32_t word)
{
	uint32_t k, cores = 1;
	int waited;

	for (k = 1; k < MC_MAX_CORES; k++)
		boxes[MC_START_AT(k) / 4] = MC_STOP;
	usleep(60000);
	control[SHAPE] = 1;
	usleep(60000);
	control[SHAPE + 1] = workers;
	control[SHAPE + 2] = bits;
	control[SHAPE + 3] = word;
	usleep(60000);
	control[SHAPE] = 0;
	/* the control pass says how many there are now (one while the strip is laid out) */
	for (waited = 0; workers && waited < 100 && (cores = control[CORES_NOW]) <= 1; waited++)
		usleep(20000);
	return cores;
}

/* A worker's job: branches that go by its own numbers, so that no two workers are at the same
 * instruction for long, as with real work. */
static uint32_t spin(uint32_t seed, uint32_t n)
{
	uint32_t x = seed * 2654435761u | 1, sum = 0, i;

	for (i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		if (x & 1)
			sum += x >> 3;
		else if (x & 2)
			sum ^= x;
		else
			sum -= i;
	}
	return sum;
}

static int bound[MC_MAX_CORES], bound_count;

/* The job on the first k workers that have cores; how many microseconds until all were done. */
static uint32_t run(int k, uint32_t n)
{
	uint32_t start = now_us();
	int i;

	for (i = 0; i < k; i++)
		mcw_post(bound[i], spin, (uint32_t)bound[i], n);
	for (i = 0; i < k; i++)
		mcw_wait(bound[i]);
	return now_us() - start;
}

static uint32_t median3(const uint32_t *t)
{
	uint32_t a = t[0], b = t[1], c = t[2];

	return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
}

/* how far apart they are, in hundredths of the middle one */
static uint32_t spread3(const uint32_t *t)
{
	uint32_t low = t[0], high = t[0];
	int i;

	for (i = 1; i < 3; i++) {
		low = t[i] < low ? t[i] : low;
		high = t[i] > high ? t[i] : high;
	}
	return (high - low) * 100u / (median3(t) | 1);
}

/* The kept folder, from the host, before the registry is read: what the nx script does when the
 * desktop starts, done here so that a choice kept by an earlier session is there to be read. */
static void load_kept(void)
{
	pid_t pid;

	if (!(control[HOST_FLAGS] & 1) || access(EARLY, F_OK) == 0 || access("/usr/bin/nxpersist", X_OK) != 0)
		return;
	close(open(EARLY, O_CREAT | O_WRONLY, 0644));
	pid = fork();
	if (pid == 0) {
		execl("/usr/bin/nxpersist", "nxpersist", "load", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

int main(int argc, char **argv)
{
	int again = argc > 1 && !strcmp(argv[1], "again"), fd = open("/dev/gpu", O_RDWR), forced, i, lo, workers;
	uint32_t most, width, n, kept_n, cores, t_lo[3], t_all[3], ratio, us, kept_us;
	char text[64];

	if (fd < 0 || (control = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CONTROL_OFFSET)) == MAP_FAILED ||
	    (boxes = mmap(NULL, MC_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MC_PAGE_GPU_OFFSET)) == MAP_FAILED)
		return 0;	/* (a machine without the GPU device) */
	most = control[CORES_MOST];
	if (most > MC_MAX_CORES)
		most = MC_MAX_CORES;
	if (most < 2 + BIG_COUNT)
		return 0;
	if (control[SHAPE + 1] != 0 && !again)
		return 0;	/* laid out already: by an earlier run, or by the kernel for a program */
	/* Whether the tick knows the hybrid's word (MC_HYBRID): with the small blocks 64 columns
	 * wide, its targets have room for 16 small workers a band and 14 after the big ones (47
	 * cores of 256), and a tick that does not has every core. */
	cores = lay_out(most - 1, SMALL_BITS, hybrid(1, 64, WIDE));
	if (cores <= 1 + BIG_COUNT || cores > 1 + BIG_COUNT + 16 * ((most + 126) / 128) + 16) {
		lay_out(0, 0, 0);
		printf("geotune: this machine's workers are all alike: laid out by the kernel\n");
		return 0;
	}
	load_kept();
	forced = reg_int("geotune.force", 0);
	width = (uint32_t)reg_int("geotune.width", 0);
	workers = reg_int("geotune.workers", 0);
	kept_n = (uint32_t)reg_int("geotune.iterations", 0);
	kept_us = (uint32_t)reg_int("geotune.us", 0);
	if (forced == 2 || forced == 4 || forced == 8) {
		width = (uint32_t)forced;
		printf("geotune: %u-column blocks, as geotune.force says\n", width);
		goto done;
	}
	/* every worker small, each in eight columns of its own */
	lay_out(most - 1, SMALL_BITS, hybrid(0, WIDE, 0));
	spin(0, 64);	/* (the job's code there before a worker comes to it) */
	if (mcw_open((int)most - 1) <= 0)
		goto done;
	for (i = 1, bound_count = 0; i <= mcw_count(); i++)
		if (mcw_bound(i))
			bound[bound_count++] = i;
	lo = bound_count / 8;
	if (lo < 2 || bound_count < (int)most - 1 - 8) {
		mcw_close();
		goto done;
	}
	run(bound_count, 2000);	/* (the workers' stacks and pages, the first time) */
	/* a later boot: the eighth's time again, and the kept choice if it is the same */
	if (width && workers == bound_count && kept_n && kept_us && !again) {
		for (i = 0; i < 3; i++)
			t_lo[i] = run(lo, kept_n);
		us = median3(t_lo);
		if (us > kept_us / 100u * (100u - DRIFT) && us < kept_us / 100u * (100u + DRIFT)) {
			mcw_close();
			printf("geotune: %u-column blocks, as before (%u ms against %u)\n", width, us / 1000u, kept_us / 1000u);
			goto done;
		}
		printf("geotune: %u ms where it was %u: measured again\n", us / 1000u, kept_us / 1000u);
	}
	/* a job of some 200 ms for the eighth */
	n = 20000;
	us = run(lo, n);
	n = us > 1000 ? n / 100u * 200000u / us * 100u : 400000;
	n = n < 5000 ? 5000 : n > 400000 ? 400000 : n;
	for (i = 0; i < 3; i++) {
		t_lo[i] = run(lo, n);
		t_all[i] = run(bound_count, n);
	}
	mcw_close();
	us = median3(t_lo);
	ratio = median3(t_all) * 100u / (us | 1);
	width = ratio < ALL_AT_ONCE ? WIDE : DENSE;
	if (spread3(t_lo) > 30 || spread3(t_all) > 30)
		width = DENSE;	/* (too unsteady to say) */
	printf("geotune: %d workers took %u.%02u times as long as %d (%u ms, spread %u%% and %u%%): about %u side by side; %u-column blocks\n",
	       bound_count, ratio / 100, ratio % 100, lo, us / 1000u, spread3(t_lo), spread3(t_all), bound_count * 100u / (ratio > 100 ? ratio : 100), width);
	snprintf(text, sizeof text, "%u", width);
	reg_set("geotune.width", text);
	snprintf(text, sizeof text, "%d", bound_count);
	reg_set("geotune.workers", text);
	snprintf(text, sizeof text, "%u", n);
	reg_set("geotune.iterations", text);
	snprintf(text, sizeof text, "%u", us);
	reg_set("geotune.us", text);
	snprintf(text, sizeof text, "%u", ratio);
	reg_set("geotune.ratio", text);
done:
	usleep(60000);	/* (the kernel parks the cores of a program that closes the device) */
	if (!width)
		width = DENSE;
	lay_out(most - 1, SMALL_BITS, hybrid(1, width, WIDE));
	return 0;
}
