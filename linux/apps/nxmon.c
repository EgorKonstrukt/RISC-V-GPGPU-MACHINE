/* nxmon: the monitor, a task manager (docs/system/nanox.md). Processes: each program's share of the first
 * core, its workers, GPU lists and memory. Performance: CPU (overall, or a 16 x 16 grid of every
 * core's load), memory and GPU over the last minute; the plots are shaded by worker cores straight
 * into the window's buffer (docs/machine/multicore.md), or by this core once a second without them. */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include "machine.h"
#include "registry.h"
#include "ui.h"
#include "ui_ask.h"
#include "mcw.h"

#define WIDTH 600
#define HEIGHT 470
#define TAB_Y 6
#define TOP 34			/* below the tabs */
#define SIDE_W 128
#define SIDE_ROW 56
#define PANE_X 148
#define GRAPH_X (PANE_X + 8)
#define GRAPH_Y 96
#define HISTORY 60		/* samples, a second each */
#define STEP 7			/* pixels a sample */
#define PLOT_W (HISTORY * STEP)
#define PLOT_H 160
#define STATS_Y (GRAPH_Y + PLOT_H + 12)
#define GRID 16			/* cores a row of the grid */
#define CELL_W 26
#define CELL_H 10
#define CELL_SAMPLES (CELL_W - 1)	/* a pixel a second */
#define LIST_Y (TOP + 24)
#define LIST_H (HEIGHT - LIST_Y - 40)
#define END_W 90
#define MAX_CORES 256
#define MAX_PROGRAMS 64
#define TAGS 4			/* worker tags a program may have: one an open of /dev/gpu that asked */

#define CORE0_COLOUR MWRGB(240, 150, 40)
#define WORKER_COLOUR MWRGB(60, 200, 220)
#define MEMORY_COLOUR MWRGB(200, 110, 230)
#define FILES_COLOUR MWRGB(240, 220, 90)
#define GPU_COLOUR MWRGB(90, 210, 110)

enum { PROCESSES, PERFORMANCE };
enum { CPU, MEMORY, GPU, PANES };
enum { BY_NAME, BY_PID, BY_CPU, BY_WORKERS, BY_GPU, BY_MEMORY, COLUMNS };

/* A plot: two values a sample, the second stacked on the first, `top` at the plot's top. */
struct plot {
	unsigned low[HISTORY], high[HISTORY], top;
	unsigned short low_px[HISTORY], high_px[HISTORY];	/* the same as heights, in 256ths of a pixel */
};
static struct plot plots[PANES] = { { {0}, {0}, 1000000 }, { {0}, {0}, 1 }, { {0}, {0}, 16 } };
static const uint32_t shades[PANES][2] = { {0xf09628, 0x3cc8dc}, {0xc86ee6, 0xf0dc5a}, {0x5ad26e, 0x2e8c46} };
static unsigned char core_load[MAX_CORES][CELL_SAMPLES];	/* each core's last seconds, 0 to 100 */
static unsigned char core_running[MAX_CORES];

static GR_WINDOW_ID window;
static int tab = PERFORMANCE, pane = CPU, per_core;	/* what is shown */
static int showing = -1;	/* what the workers shade: a pane, or PANES for the grid; -1 nothing */

/* The plots are in the window's own buffer (docs/system/nanox.md: every window has one in GPU memory,
 * which GrGetWindowInfo locates). Workers given different rows of it never store to the same
 * 16 bytes: a row is far longer than that. */
#define SHADERS 8
#define FRAME_MS 60
static int gpu = -1;
static uint32_t *surface;	/* the window's first pixel; 0 until it is known */
static int surface_row;		/* pixels from one row of it to the next */
static uint32_t *checked;
static int workers;		/* lent to this program: another that asks has them (mcw_asked) */
static int wait_samples;	/* without them: samples to go before they are asked for again */
static int shading, shade_shift;	/* they are at a frame; how far between two samples it is, in 256ths */
static int whole = 1;		/* without workers: the plot has to be shaded whole */
static int no_workers, check, print;
static unsigned frames_shaded;

/* The machine's control words (docs/machine/multicore.md, "Counters"): what each core ran, cores 16 on in
 * the network's row (0x876ba000); and a word a core with its program's tag (WORKERS_OWNER). */
static const volatile unsigned *core_words, *core_more, *owners;
static unsigned core_count = 1, core_before[MAX_CORES], core_rate[MAX_CORES];
static unsigned cores_working, cores_idle, cores_asleep, core_busiest;
static unsigned capacity;	/* instructions a second a core runs when it is never idle */
static unsigned speed, workers_rate, busy, kernel_busy;	/* the last sample: core 0, the workers, Linux's % */
static unsigned lists_rate, copies_rate;
static long memory_total, memory_used, memory_files, memory_cached, memory_slab, memory_free_block;	/* kB */
static long switches, interrupts;	/* a second */
static int gpu_users;

static struct program {
	int pid, seen, share, workers, draws, kernel;	/* kernel: a kernel thread, no memory of its own */
	unsigned long ticks;
	unsigned worker_rate, lists, lists_before, lists_rate, tags[TAGS];
	long memory;		/* kB */
	char name[16];
} programs[MAX_PROGRAMS];
static int program_count;
static struct ui_list list = { 8, LIST_Y, WIDTH - 16, LIST_H, 0, 0, -1 };
static int listed[MAX_PROGRAMS];	/* the list's rows: programs, in the chosen order */
static int sort_by = BY_CPU;
static int chosen_pid, asked_pid;
static const char *said = "";

/* The processor's count of instructions run (its low 32 bits; it wraps every few minutes). */
static unsigned
instructions(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

/* A whole small file of /proc; the number of bytes read. */
static int
read_file(const char *name, char *text, int size)
{
	int file = open(name, O_RDONLY), n = 0, got;

	if (file < 0)
		return text[0] = 0;
	while (n < size - 1 && (got = read(file, text + n, size - 1 - n)) > 0)
		n += got;
	close(file);
	text[n] = 0;
	return n;
}

/* A process's /proc/PID file that comes whole with one read (a second one only said so). */
static int
read_once(const char *name, char *text, int size)
{
	int file = open(name, O_RDONLY), n = file < 0 ? 0 : (int)read(file, text, (size_t)size - 1);

	if (file >= 0)
		close(file);
	text[n > 0 ? n : 0] = 0;
	return n > 0 ? n : 0;
}

/* The number that follows `name` in a text, or 0; `from`, when given, is where to look from
 * and is left after the name (the files' fields come in one order). */
static long
field(const char *text, const char *name, const char **from)
{
	const char *at = strstr(from && *from ? *from : text, name);

	if (at && from)
		*from = at + strlen(name);
	return at ? atol(at + strlen(name)) : 0;
}

/* "3.07": a count of millions with two places. */
static const char *
millions(char *to, unsigned n)
{
	sprintf(to, "%u.%02u", n / 1000000, n % 1000000 / 10000);
	return to;
}

/* Linux's time since the last sample by what it went to, and its counts of events: /proc/stat. */
static void
sample_linux(long ms)
{
	static long before[8], before_switches, before_interrupts;
	static char text[4096];
	long v[8] = {0}, d[8], all = 0, now_switches, now_interrupts;
	const char *at = NULL;
	char *next;
	int i;

	read_file("/proc/stat", text, sizeof text);
	/* the first line: "cpu" and the times */
	for (i = 0, next = text + 3; i < 8 && !strncmp(text, "cpu ", 4); i++) {
		char *number = next;

		v[i] = strtol(number, &next, 10);
		if (next == number)
			break;
	}
	for (i = 0; i < 8; i++) {
		d[i] = v[i] - before[i];
		all += d[i];
		before[i] = v[i];
	}
	/* user and nice are programs; system and the two interrupt times the kernel */
	kernel_busy = all > 0 ? (unsigned)(100 * (d[2] + d[5] + d[6]) / all) : 0;
	busy = all > 0 ? (unsigned)(100 * (d[0] + d[1]) / all) + kernel_busy : 0;
	now_interrupts = field(text, "\nintr ", &at);
	now_switches = field(text, "\nctxt ", &at);
	switches = (now_switches - before_switches) * 1000 / ms;
	interrupts = (now_interrupts - before_interrupts) * 1000 / ms;
	before_switches = now_switches;
	before_interrupts = now_interrupts;
}

/* What each worker core ran since the last sample, a second's worth; the sum of them, and
 * how many of them work (10,000 instructions a second or more), wait or sleep. */
static unsigned
sample_cores(long ms)
{
	unsigned c, now, sum = 0;

	if (!core_words)
		return 0;
	core_count = core_words[0x3c0 / 4] < 1 ? 1 : core_words[0x3c0 / 4] > MAX_CORES ? MAX_CORES : core_words[0x3c0 / 4];
	if (core_count > 16 && !core_more)
		core_count = 16;
	cores_working = cores_idle = cores_asleep = core_busiest = 0;
	for (c = 1; c < core_count; c++) {
		unsigned runs = c < 32 ? core_words[0x3c4 / 4] >> c : core_more[0x400 / 4 + 4 * (c / 32)] >> (c % 32);
		unsigned sleeps = c < 32 ? core_words[0x3c8 / 4] >> c : core_more[0x404 / 4 + 4 * (c / 32)] >> (c % 32);

		now = c < 16 ? core_words[0x380 / 4 + c] : core_more[c];
		/* (a geometry laid out anew starts the counts again: one that went down is all new) */
		unsigned ran = now < core_before[c] ? now : now - core_before[c];

		/* (most cores ran nothing, and a small count needs no division of 64 bits) */
		core_rate[c] = !ran ? 0 : ran < 4000000u ? ran * 1000u / (unsigned)ms : (unsigned)((unsigned long long)ran * 1000 / ms);
		core_before[c] = now;
		core_running[c] = runs & 1;
		sum += core_rate[c];
		if (core_rate[c] > core_busiest)
			core_busiest = core_rate[c];
		if (core_rate[c] >= 10000)
			cores_working++;
		else if (sleeps & 1)
			cores_asleep++;
		else if (runs & 1)
			cores_idle++;
	}
	return sum;
}

/* The largest run of free memory the kernel has as one block: the highest order /proc/buddyinfo
 * counts any of, in kB. Without an MMU a program's memory must be such a run. */
static long
largest_free_block(void)
{
	static char text[1024];
	long most = 0;
	char *line, *at;

	read_file("/proc/buddyinfo", text, sizeof text);
	for (line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
		int order = 0;

		at = strstr(line, "zone");
		if (!at || (strchr(line, '\n') && at > strchr(line, '\n')))
			continue;
		for (at += 4; *at == ' '; at++)
			;
		while (*at && *at != ' ')
			at++;	/* the zone's name */
		for (;;) {
			char *number = at;
			long count = strtol(number, &at, 10);

			if (at == number)
				break;
			if (count > 0 && (4L << order) > most)
				most = 4L << order;
			order++;
		}
	}
	return most;
}

static void
sample_memory(void)
{
	static char text[2048];
	const char *at = NULL;

	read_file("/proc/meminfo", text, sizeof text);
	memory_total = field(text, "MemTotal:", &at);
	memory_used = memory_total - field(text, "MemAvailable:", &at);
	memory_cached = field(text, "\nCached:", &at);
	memory_files = field(text, "Shmem:", &at);	/* files written since the start live in memory */
	at = NULL;
	memory_slab = field(text, "\nSlab:", &at);
	memory_free_block = largest_free_block();
	plots[MEMORY].top = memory_total > 0 ? memory_total : 1;
	plots[MEMORY].low[HISTORY - 1] = memory_used > memory_files ? memory_used - memory_files : 0;
	plots[MEMORY].high[HISTORY - 1] = memory_files;
}

static struct program *
program_of(int pid)
{
	int i;

	for (i = 0; i < program_count; i++)
		if (programs[i].pid == pid)
			return &programs[i];
	return NULL;
}

/* Who has the GPU open (/proc/gpu, a line an open): workers held, their tags, the GPU's memory
 * for programs, lists drawn; then what each program's cores ran, by the cores' owner words. */
static void
sample_gpu_users(long ms)
{
	static char text[2048];
	char *line;
	unsigned c;
	int i, pids = 0;

	for (i = 0; i < program_count; i++) {
		programs[i].workers = programs[i].draws = 0;
		programs[i].worker_rate = programs[i].lists = 0;
		memset(programs[i].tags, 0, sizeof programs[i].tags);
	}
	read_file("/proc/gpu", text, sizeof text);
	for (line = strchr(text, '\n'); line && line[1]; line = strchr(line + 1, '\n')) {
		unsigned pid, held, wanted, lent, tag, draws, lists;
		struct program *p;

		if (sscanf(line + 1, "%u %u %u %u %u %u %u", &pid, &held, &wanted, &lent, &tag, &draws, &lists) != 7)
			continue;
		p = program_of((int)pid);
		if (!p)
			continue;
		pids += !p->lists && !p->workers && !p->draws;
		p->workers += (int)held;
		p->draws |= (int)draws;
		p->lists += lists + 1;	/* (+1: an open with no list yet still counts as the GPU's) */
		for (i = 0; i < TAGS; i++)
			if (!p->tags[i] || p->tags[i] == tag) {
				p->tags[i] = tag;
				break;
			}
	}
	gpu_users = pids;
	for (c = 1; owners && c < core_count; c++) {
		unsigned tag = owners[0x400 / 4 + c];

		for (i = 0; tag && i < program_count; i++)
			if (programs[i].tags[0] == tag || programs[i].tags[1] == tag || programs[i].tags[2] == tag || programs[i].tags[3] == tag) {
				programs[i].worker_rate += core_rate[c];
				break;
			}
	}
	for (i = 0; i < program_count; i++) {
		struct program *p = &programs[i];

		p->lists_rate = p->lists_before && p->lists > p->lists_before ? (unsigned)((p->lists - p->lists_before) * 1000 / ms) : 0;
		p->lists_before = p->lists;
	}
}

/* Every process's share of the processor since the last look, from its /proc/PID/stat, and
 * its memory from /proc/PID/statm (pages it has). */
static void
sample_programs(long ms)
{
	DIR *folder = opendir("/proc");
	struct dirent *item;
	char name[40], text[400], *open_at, *close_at;
	unsigned long user, kernel;
	int i, n;

	for (i = 0; i < program_count; i++)
		programs[i].seen = 0;
	while (folder && (item = readdir(folder)) != NULL) {
		int pid = atoi(item->d_name), skip;
		char *at;

		if (pid <= 0)
			continue;
		snprintf(name, sizeof name, "/proc/%d/stat", pid);
		read_once(name, text, sizeof text);
		open_at = strchr(text, '(');
		close_at = strrchr(text, ')');
		if (!open_at || !close_at)
			continue;
		/* after the name: the state and ten numbers, then the time as a program and in the kernel */
		for (at = close_at + 1, skip = 0; skip < 11 && *at; skip++) {
			while (*at == ' ')
				at++;
			while (*at && *at != ' ')
				at++;
		}
		if (skip < 11 || at[0] != ' ' || at[1] < '0' || at[1] > '9')
			continue;
		user = strtoul(at, &at, 10);
		if (at[0] != ' ' || at[1] < '0' || at[1] > '9')
			continue;
		kernel = strtoul(at, NULL, 10);
		for (i = 0; i < program_count && programs[i].pid != pid; i++)
			;
		if (i == MAX_PROGRAMS)
			continue;
		if (i == program_count) {
			program_count++;
			memset(&programs[i], 0, sizeof programs[i]);
			programs[i].pid = pid;
			programs[i].ticks = user + kernel;	/* new: nothing to compare with yet */
			n = (int)(close_at - open_at - 1);
			n = n < (int)sizeof programs[i].name - 1 ? n : (int)sizeof programs[i].name - 1;
			memcpy(programs[i].name, open_at + 1, (size_t)n);
			programs[i].name[n] = 0;
		}
		/* a tick is a hundredth of a second */
		programs[i].share = (int)((user + kernel - programs[i].ticks) * 1000 / ms);
		programs[i].ticks = user + kernel;
		programs[i].seen = 1;
		snprintf(name, sizeof name, "/proc/%d/statm", pid);
		read_once(name, text, sizeof text);
		at = strchr(text, ' ');
		programs[i].memory = at ? atol(at + 1) * 4 : 0;
		programs[i].kernel = atol(text) == 0;
	}
	if (folder)
		closedir(folder);
	for (i = n = 0; i < program_count; i++)
		if (programs[i].seen)
			programs[n++] = programs[i];
	program_count = n;
	sample_gpu_users(ms);
}

/* Sample i's two values as heights: the first from the bottom, the second on top of it. */
static void
scale_sample(int which, int i)
{
	struct plot *p = &plots[which];
	unsigned low = !p->low[i] ? 0 : (unsigned)((unsigned long long)p->low[i] * (PLOT_H * 256) / p->top);
	unsigned high = !p->high[i] ? 0 : (unsigned)((unsigned long long)p->high[i] * (PLOT_H * 256) / p->top);

	low = low > PLOT_H * 256 ? PLOT_H * 256 : low;
	high = low + high > PLOT_H * 256 ? PLOT_H * 256 - low : high;
	p->low_px[i] = (unsigned short)low;
	p->high_px[i] = (unsigned short)high;
}

/* Colour b over colour a, k 256ths of it. */
static uint32_t
blend(uint32_t a, uint32_t b, unsigned k)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - k) + (b & 0xff00ff) * k) >> 8 & 0xff00ff;
	uint32_t g = ((a & 0x00ff00) * (256 - k) + (b & 0x00ff00) * k) >> 8 & 0x00ff00;

	return 0xff000000u | rb | g;
}

/* Where the plot's first pixel is in the window's buffer. */
static uint32_t *
plot_at(void)
{
	return surface + (GRAPH_Y + 1) * surface_row + GRAPH_X + 1;
}

/* The pixel shader: rows y0 to y1 of a plot from its samples, into rows `stride` pixels apart.
 * The heights run straight from one sample to the next, the plot `shift` 256ths of the way
 * from the last sample to the one before it sliding in; the edge of an area covers its last
 * pixel in part, the areas are brighter towards the top, and behind them are lines. */
static void
shade(int which, int x0, int x1, int y0, int y1, int shift, uint32_t *to, int stride)
{
	const struct plot *p = &plots[which];
	unsigned short low_at[PLOT_W], all_at[PLOT_W];
	int x, y;

	for (x = x0; x < x1; x++) {
		int u = (x + 1) * 256 / STEP + shift - 512, i, f;

		u = u < 0 ? 0 : u > (HISTORY - 1) * 256 ? (HISTORY - 1) * 256 : u;
		i = u >> 8;
		f = u & 255;
		if (i > HISTORY - 2) {
			i = HISTORY - 2;
			f = 256;
		}
		low_at[x] = (unsigned short)((p->low_px[i] * (256 - f) + p->low_px[i + 1] * f) >> 8);
		all_at[x] = (unsigned short)(low_at[x] + ((p->high_px[i] * (256 - f) + p->high_px[i + 1] * f) >> 8));
	}
	for (y = y0; y < y1; y++) {
		uint32_t *row = to + y * stride;
		int base = (PLOT_H - 1 - y) * 256;
		unsigned light = 150 + 105 * (PLOT_H - y) / PLOT_H;
		uint32_t low = blend(0, shades[which][0], light), high = blend(0, shades[which][1], light);
		uint32_t behind = y % 16 == 15 ? 0xff242424u : 0xff000000u;

		for (x = x0; x < x1; x++) {
			int below = low_at[x] - base, under = all_at[x] - base;
			uint32_t c = x % (STEP * 10) == STEP * 10 - 1 ? 0xff242424u : behind;

			if (under >= 256)
				c = high;
			else if (under > 0)
				c = blend(c, high, (unsigned)under);
			if (below >= 256)
				c = low;
			else if (below > 0)
				c = blend(c, low, (unsigned)below);
			row[x] = c;
		}
	}
}

/* Rows y0 to y1 of the grid: a cell a core, its last seconds' load as an area, the first core's
 * in Linux's colour; a core the machine has not laid out is darker, one it lacks darker still. */
static void
shade_grid(int y0, int y1, uint32_t *to, int stride)
{
	int x, y;

	for (y = y0; y < y1; y++) {
		uint32_t *row = to + y * stride;
		int cell_y = y % CELL_H, base = (CELL_H - 2 - cell_y) * 256;
		unsigned light = 150 + 105 * (CELL_H - 1 - cell_y) / (CELL_H - 1);
		uint32_t worker = blend(0, 0x3cc8dc, light), first = blend(0, 0xf09628, light);

		for (x = 0; x < PLOT_W; x++) {
			unsigned core = (unsigned)(y / CELL_H * GRID + x / CELL_W), cell_x = (unsigned)(x % CELL_W);
			uint32_t c;
			int height;

			if (cell_x == CELL_W - 1 || cell_y == CELL_H - 1 || x >= GRID * CELL_W) {
				row[x] = 0xff303030u;
				continue;
			}
			if (core >= core_count) {
				row[x] = 0xff080808u;
				continue;
			}
			c = core && !core_running[core] ? 0xff141414u : 0xff000000u;
			height = core_load[core][cell_x] * (CELL_H - 1) * 256 / 100 - base;
			if (height >= 256)
				c = core ? worker : first;
			else if (height > 0)
				c = blend(c, core ? worker : first, (unsigned)height);
			row[x] = c;
		}
	}
}

/* What the plot area shows now, rows y0 to y1 of it. */
static void
shade_showing(int y0, int y1, int shift, uint32_t *to, int stride)
{
	if (showing == PANES)
		shade_grid(y0, y1, to, stride);
	else if (showing >= 0)
		shade(showing, 0, PLOT_W, y0, y1, shift, to, stride);
}

/* A worker's job: rows `first` up to `last` (two bytes of `rows`) of the plot. */
static uint32_t
shade_rows(uint32_t rows, uint32_t shift)
{
	shade_showing((int)(rows & 255), (int)(rows >> 8), (int)shift, plot_at(), surface_row);
	return (rows >> 8) - (rows & 255);
}

/* Finds the window's buffer and maps it, and takes the workers: once the window is there. */
static void
locate(void)
{
	GR_WINDOW_INFO info;
	uint32_t offset, bytes;
	void *memory;

	if (surface || gpu < 0)
		return;
	GrGetWindowInfo(window, &info);
	if (!info.surface_address)
		return;
	surface_row = info.surface_row ? (int)info.surface_row : (int)info.width;
	offset = info.surface_address - 0x86000000u;
	bytes = (offset & 4095) + (uint32_t)surface_row * (GRAPH_Y + PLOT_H + 2) * 4;
	memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, gpu, offset & ~4095u);
	if (memory == MAP_FAILED)
		return;
	surface = (uint32_t *)((char *)memory + (offset & 4095));
	mcw_touch(memory, bytes);
	mcw_lend();
	workers = no_workers ? 0 : mcw_open(SHADERS);
}

/* The workers' frame, if they are at one, finished: before what they read changes. */
static void
finish_frame(void)
{
	int k;

	for (k = 1; k <= workers; k++)
		mcw_wait(k);
	shading = 0;
}

/* Another program is waiting for the workers: it has them, and this core shades meanwhile. */
static void
give_workers(void)
{
	finish_frame();
	mcw_close();
	workers = 0;
	whole = 1;
	wait_samples = 5;
	if (print)
		puts("nxmon: the workers are given to a program that asked");
}

/* Without workers, at a sample: they are taken again once some have been nobody's for a while. */
static void
take_workers(void)
{
	unsigned nobodys = core_count - 1 - cores_working - cores_asleep - cores_idle;

	if (no_workers || !surface)
		return;
	if (!nobodys)
		wait_samples = 5;
	else if (--wait_samples <= 0)
		workers = mcw_open(SHADERS);
}

/* Whether the workers are between two frames. */
static int
frame_done(void)
{
	int k;

	for (k = 1; k <= workers; k++)
		if (!mcw_done(k))
			return 0;
	return 1;
}

/* What the workers have just shaded, shaded again on this core and compared (a test). */
static void
check_frame(void)
{
	int x, y, differ = 0;

	shade_showing(0, PLOT_H, shade_shift, checked, PLOT_W);
	for (y = 0; y < PLOT_H; y++)
		for (x = 0; x < PLOT_W; x++)
			differ += checked[y * PLOT_W + x] != plot_at()[y * surface_row + x];
	printf("nxmon: %d of %d pixels differ from this core's\n", differ, PLOT_H * PLOT_W);
}

/* The window system draws by taking the window's buffer to the GPU and copying it back whole
 * (docs/system/nanox.md), which would undo what the workers wrote meanwhile: after asking it to draw,
 * a pixel is read, which is answered when all of that is done. */
static void
settle(void)
{
	GR_PIXELVAL pixel;

	GrReadArea(window, 0, 0, 1, 1, &pixel);
}

/* A frame for the workers: each some rows of the plot, `shift` 256ths of a sample on. */
static void
post_frame(int shift)
{
	int k;

	shade_shift = shift > 256 ? 256 : shift;
	for (k = 1; k <= workers; k++)
		mcw_post(k, shade_rows, (uint32_t)(PLOT_H * (k - 1) / workers) | (uint32_t)(PLOT_H * k / workers) << 8, (uint32_t)shade_shift);
	shading = 1;
}

/* Without workers, after a sample: the plot moved a sample to the left and the newest columns
 * shaded by this core; all of it when its scale changed, it is the grid or the window was cleared. */
static void
shade_here(int rescaled)
{
	int y;

	if (!surface || showing < 0)
		return;
	if (whole || rescaled || showing == PANES) {
		shade_showing(0, PLOT_H, 256, plot_at(), surface_row);
	} else {
		for (y = 0; y < PLOT_H; y++)
			memmove(plot_at() + y * surface_row, plot_at() + y * surface_row + STEP, (PLOT_W - STEP) * sizeof surface[0]);
		shade(showing, PLOT_W - STEP, PLOT_W, 0, PLOT_H, 256, plot_at(), surface_row);
	}
	whole = 0;
}

/* A line of text on the window's face, drawn again only when it reads otherwise than it did
 * (`again`: or the window is being drawn whole). `slot` names the place. */
static void
text_at(int slot, int x, int y, int width, const char *text, GR_COLOR colour, int again)
{
	static char shown[48][96];

	if (!again && !strcmp(shown[slot], text))
		return;
	snprintf(shown[slot], sizeof shown[slot], "%s", text);
	ui_fill(window, x, y, width, 15, UI_FACE);
	ui_text(window, x, y, text, -1, colour, 0);
}

/* A small square of a plot's colour and what it stands for. */
static void
legend(int x, int y, GR_COLOR colour, const char *text)
{
	ui_fill(window, x, y + 2, 8, 8, colour);
	ui_text(window, x + 12, y, text, -1, BLACK, 0);
}

static void
draw_tabs(void)
{
	static const char *const names[2] = { "Processes", "Performance" };
	int i, x = 8;

	ui_fill(window, 0, 0, WIDTH, TOP - 2, UI_FACE);
	for (i = 0; i < 2; i++) {
		int w = i ? 104 : 90;

		ui_fill(window, x, TAB_Y - (i == tab ? 2 : 0), w, 22 + (i == tab ? 2 : 0), i == tab ? WHITE : UI_FACE);
		ui_bevel(window, x, TAB_Y - (i == tab ? 2 : 0), w, 24, 0);
		ui_text(window, x + 10, TAB_Y + 4, names[i], -1, BLACK, 0);
		x += w + 2;
	}
	GrSetGCForeground(ui_gc, UI_SHADOW);
	GrLine(window, ui_gc, 0, TOP - 4, WIDTH - 1, TOP - 4);
}

/* What the CPU's, memory's and GPU's entries on the left say now. */
static void
side_values(char text[PANES][40])
{
	unsigned all = speed + workers_rate, use = capacity ? (unsigned)((unsigned long long)all * 100 / ((unsigned long long)capacity * core_count)) : 0;
	char m[16];

	snprintf(text[CPU], 40, "%u%%  %s M/s", use > 100 ? 100 : use, millions(m, all));
	snprintf(text[MEMORY], 40, "%ld / %ld MB", memory_used / 1024, memory_total / 1024);
	snprintf(text[GPU], 40, "%u lists/s", lists_rate);
}

static void
draw_side(int again)
{
	static const char *const names[PANES] = { "CPU", "Memory", "GPU" };
	static const GR_COLOR colours[PANES] = { CORE0_COLOUR, MEMORY_COLOUR, GPU_COLOUR };
	char text[PANES][40];
	int i;

	side_values(text);
	for (i = 0; i < PANES; i++) {
		int y = TOP + 4 + i * SIDE_ROW;
		GR_COLOR ground = i == pane ? MWRGB(204, 228, 247) : UI_FACE;

		if (again) {
			ui_fill(window, 8, y, SIDE_W, SIDE_ROW - 6, ground);
			if (i == pane)
				ui_bevel(window, 8, y, SIDE_W, SIDE_ROW - 6, 1);
			ui_fill(window, 14, y + 8, 4, SIDE_ROW - 22, colours[i]);
			ui_text(window, 26, y + 8, names[i], -1, BLACK, 0);
		}
		if (again || i != pane) {
			ui_fill(window, 26, y + 26, SIDE_W - 22, 15, ground);
			ui_text(window, 26, y + 26, text[i], -1, UI_SHADOW, 0);
		} else {
			ui_fill(window, 26, y + 26, SIDE_W - 22, 15, ground);
			ui_text(window, 26, y + 26, text[i], -1, BLACK, 0);
		}
	}
}

/* The CPU's two views, Overall and Per core: their buttons right of the pane's title. */
#define VIEW_X (WIDTH - 8 - 2 * 76)
#define VIEW_Y (TOP + 4)

static void
draw_pane_head(void)
{
	static const char *const titles[PANES] = { "CPU", "Memory", "GPU" };
	char line[120];

	ui_fill(window, PANE_X, TOP, WIDTH - PANE_X, GRAPH_Y - TOP - 1, UI_FACE);
	ui_text(window, PANE_X + 8, TOP + 6, titles[pane], -1, BLACK, 0);
	switch (pane) {
	case CPU:
		snprintf(line, sizeof line, "RISC-V %s, %u core%s, %s", machine_isa(), core_count, core_count == 1 ? "" : "s",
			 machine_mmu() ? "Sv32 paging" : "no MMU");
		ui_text(window, PANE_X + 8, TOP + 24, line, -1, UI_SHADOW, 0);
		ui_button(window, VIEW_X, VIEW_Y, 74, 20, "Overall", !per_core);
		ui_button(window, VIEW_X + 76, VIEW_Y, 74, 20, "Per core", per_core);
		if (per_core) {
			legend(PANE_X + 8, TOP + 44, CORE0_COLOUR, "core 0 (Linux)");
			legend(PANE_X + 130, TOP + 44, WORKER_COLOUR, "workers: a cell each, its last 25 s");
		} else {
			legend(PANE_X + 8, TOP + 44, CORE0_COLOUR, "core 0 (Linux)");
			legend(PANE_X + 130, TOP + 44, WORKER_COLOUR, "workers");
		}
		break;
	case MEMORY:
		snprintf(line, sizeof line, "%ld MB of RAM, %s", memory_total / 1024,
			 machine_mmu() ? "paged (Sv32)" : "no MMU: a program's memory is taken whole");
		ui_text(window, PANE_X + 8, TOP + 24, line, -1, UI_SHADOW, 0);
		legend(PANE_X + 8, TOP + 44, MEMORY_COLOUR, "in use");
		legend(PANE_X + 90, TOP + 44, FILES_COLOUR, "files kept in memory");
		break;
	case GPU:
		ui_text(window, PANE_X + 8, TOP + 24, "/dev/gpu: the machine's GPU, 27 MB at 0x86000000", -1, UI_SHADOW, 0);
		legend(PANE_X + 8, TOP + 44, GPU_COLOUR, "lists drawn a second");
		break;
	}
	if (!(pane == CPU && per_core))
		ui_text(window, GRAPH_X + PLOT_W - 64, GRAPH_Y - 16, "60 seconds", -1, UI_SHADOW, 0);
}

/* The numbers under the plot: two columns of a name and a value. */
static void
draw_stats(int again)
{
	char v[16][48];
	const char *names[16];
	int n = 0, i;

	memset(names, 0, sizeof names);
	switch (pane) {
	case CPU: {
		struct sysinfo info;
		char a[16], b[16];
		unsigned all = speed + workers_rate;
		unsigned use = capacity ? (unsigned)((unsigned long long)all * 100 / ((unsigned long long)capacity * core_count)) : 0;

		sysinfo(&info);
		names[n] = "Utilization"; snprintf(v[n++], 48, "%u%% of %u cores", use > 100 ? 100 : use, core_count);
		names[n] = "All cores"; snprintf(v[n++], 48, "%s million/s", millions(a, all));
		names[n] = "Core 0"; snprintf(v[n++], 48, "%s million/s", millions(b, speed));
		names[n] = "Linux busy"; snprintf(v[n++], 48, "%u%% (kernel %u%%)", busy, kernel_busy);
		names[n] = "A core's most"; snprintf(v[n++], 48, "%s million/s", millions(a, capacity));
		names[n] = "Workers busy"; snprintf(v[n++], 48, "%u", cores_working);
		names[n] = "Asleep, idle"; snprintf(v[n++], 48, "%u, %u", cores_asleep, cores_idle);
		names[n] = "Parked"; snprintf(v[n++], 48, "%u", core_count - 1 - cores_working - cores_asleep - cores_idle);
		names[n] = "Processes"; snprintf(v[n++], 48, "%d", program_count);
		names[n] = "Up time"; snprintf(v[n++], 48, "%ld:%02ld:%02ld", (long)info.uptime / 3600, (long)info.uptime / 60 % 60, (long)info.uptime % 60);
		names[n] = "Task switches"; snprintf(v[n++], 48, "%ld a second", switches);
		names[n] = "Interrupts"; snprintf(v[n++], 48, "%ld a second", interrupts);
		names[n] = "Load"; snprintf(v[n++], 48, "%lu.%02lu, %lu.%02lu", info.loads[0] >> 16, (info.loads[0] & 0xffff) * 100 >> 16,
					      info.loads[1] >> 16, (info.loads[1] & 0xffff) * 100 >> 16);
		if (print && core_count > 1)	/* (for a test: the workers' line) */
			printf("%u working, %u asleep, %u idle, %u parked; a core's most %u a second\n", cores_working, cores_asleep,
			       cores_idle, core_count - 1 - cores_working - cores_asleep - cores_idle, capacity);
		break;
	}
	case MEMORY:
		names[n] = "In use"; snprintf(v[n++], 48, "%ld MB", memory_used / 1024);
		names[n] = "Available"; snprintf(v[n++], 48, "%ld MB", (memory_total - memory_used) / 1024);
		names[n] = "Total"; snprintf(v[n++], 48, "%ld MB", memory_total / 1024);
		names[n] = "Files read"; snprintf(v[n++], 48, "%ld MB kept", (memory_cached - memory_files) / 1024);
		names[n] = "Files written"; snprintf(v[n++], 48, "%ld.%ld MB", memory_files / 1024, memory_files % 1024 * 10 / 1024);
		names[n] = "Kernel (slab)"; snprintf(v[n++], 48, "%ld KB", memory_slab);
		names[n] = "Largest free"; snprintf(v[n++], 48, "%ld KB, one piece", memory_free_block);
		names[n] = "Processes"; snprintf(v[n++], 48, "%d", program_count);
		break;
	case GPU: {
		static const char *const modes[] = { "off", "32-bit colour", "8-bit indexed", "the GPU's picture", "layers (the desktop)" };
		unsigned mode = core_words ? core_words[0] : 0;
		int i2, owner = -1;

		for (i2 = 0; i2 < program_count; i2++)
			if (programs[i2].draws)
				owner = i2;
		names[n] = "Lists drawn"; snprintf(v[n++], 48, "%u a second", lists_rate);
		names[n] = "Copies back"; snprintf(v[n++], 48, "%u a second", copies_rate);
		names[n] = "Display"; snprintf(v[n++], 48, "%s", mode < 5 ? modes[mode] : "?");
		names[n] = "Screen"; snprintf(v[n++], 48, "%u x %u", core_words ? core_words[1] : 0, core_words ? core_words[2] : 0);
		names[n] = "Programs on it"; snprintf(v[n++], 48, "%d", gpu_users);
		names[n] = "Its memory for"; snprintf(v[n++], 48, "%s", owner >= 0 ? programs[owner].name : "nobody");
		break;
	}
	}
	for (i = 0; i < 16; i++) {
		int x = PANE_X + 8 + (i % 2) * 222, y = STATS_Y + (i / 2) * 18;

		text_at(i * 2, x, y, 96, names[i] ? names[i] : "", UI_SHADOW, again);
		text_at(i * 2 + 1, x + 96, y, 124, names[i] ? v[i] : "", BLACK, again);
	}
}

/* A row of the process list: its columns, a fixed place each. */
static const int column_x[COLUMNS] = { 6, 150, 206, 256, 400, 492 };
static const char *const column_names[COLUMNS] = { "Name", "PID", "CPU", "Workers (busy)", "GPU", "Memory" };

static void
draw_row(int row, int y, GR_COLOR colour)
{
	const struct program *p = &programs[listed[row]];
	char text[COLUMNS][40], m[16];
	int c;

	snprintf(text[BY_NAME], 40, "%s", p->name);
	snprintf(text[BY_PID], 40, "%d", p->pid);
	snprintf(text[BY_CPU], 40, "%d%%", p->share);
	if (p->workers)
		snprintf(text[BY_WORKERS], 40, "%d (%u%%, %s M/s)", p->workers,
			 capacity ? (unsigned)((unsigned long long)p->worker_rate * 100 / ((unsigned long long)capacity * (unsigned)p->workers)) : 0,
			 millions(m, p->worker_rate));
	else
		snprintf(text[BY_WORKERS], 40, "-");
	if (p->lists)
		snprintf(text[BY_GPU], 40, "%u lists/s%s", p->lists_rate, p->draws ? " *" : "");
	else
		snprintf(text[BY_GPU], 40, "-");
	snprintf(text[BY_MEMORY], 40, "%ld.%ld MB", p->memory / 1024, p->memory % 1024 * 10 / 1024);
	for (c = 0; c < COLUMNS; c++)
		ui_text(window, list.x + column_x[c], y, text[c], -1, colour, 0);
}

static void
draw_list(void)
{
	int rows = ui_list_rows(&list), bar = list.count > rows ? UI_SCROLL_W : 0, i, c;
	char summary[120];
	unsigned held = 0, lists = 0;

	ui_fill(window, 8, TOP, WIDTH - 16, LIST_Y - TOP, UI_FACE);
	for (c = 0; c < COLUMNS; c++)
		ui_text(window, list.x + column_x[c], TOP + 4, column_names[c], -1, c == sort_by ? BLACK : UI_SHADOW, 0);
	GrSetGCForeground(ui_gc, BLACK);
	GrLine(window, ui_gc, list.x + column_x[sort_by], TOP + 18, list.x + column_x[sort_by] + 30, TOP + 18);
	ui_fill(window, list.x, list.y, list.width, list.height, WHITE);
	for (i = 0; i < rows && list.top + i < list.count; i++) {
		int n = list.top + i, y = list.y + 2 + i * UI_ROW;

		if (n == list.selected)
			ui_fill(window, list.x + 2, y, list.width - 4 - bar, UI_ROW, UI_SELECTED);
		draw_row(n, y + 3, n == list.selected ? WHITE : BLACK);
	}
	if (bar)
		ui_scrollbar(window, list.x + list.width - 2 - UI_SCROLL_W, list.y + 2, list.height - 4, list.count, rows, list.top);
	ui_bevel(window, list.x, list.y, list.width, list.height, 1);
	for (i = 0; i < program_count; i++) {
		held += (unsigned)programs[i].workers;
		lists += programs[i].lists_rate;
	}
	ui_fill(window, 0, HEIGHT - 34, WIDTH, 34, UI_FACE);
	ui_button(window, 8, HEIGHT - 30, END_W, 22, chosen_pid && chosen_pid == asked_pid ? "End it now" : "End task", 0);
	ui_text(window, END_W + 16, HEIGHT - 25, said, -1, BLACK, 0);
	snprintf(summary, sizeof summary, "%d processes, Linux %u%%, %u of %u workers held, %u lists/s", program_count, busy, held,
		 core_count > 1 ? core_count - 1 : 0, lists);
	if (!said[0])
		ui_text(window, END_W + 16, HEIGHT - 25, summary, -1, UI_SHADOW, 0);
}

/* Whether program a comes before b in the chosen order (the busiest first; names A to Z), the
   kernel's threads after the programs unless by number. */
static int
before(const struct program *a, const struct program *b)
{
	long x = 0, y = 0;

	if (a->kernel != b->kernel && sort_by != BY_PID)
		return b->kernel;
	switch (sort_by) {
	case BY_NAME: return strcmp(a->name, b->name) < 0;
	case BY_PID: return a->pid < b->pid;
	case BY_CPU: x = a->share, y = b->share; break;
	case BY_WORKERS: x = (long)a->workers * 1000000 + a->worker_rate / 1000, y = (long)b->workers * 1000000 + b->worker_rate / 1000; break;
	case BY_GPU: x = (long)a->lists_rate * 2 + (a->lists != 0), y = (long)b->lists_rate * 2 + (b->lists != 0); break;
	case BY_MEMORY: x = a->memory, y = b->memory; break;
	}
	return x != y ? x > y : a->pid < b->pid;
}

/* The programs in the list's order, the selected one still selected if it is still there. */
static void
list_programs(void)
{
	int i, k;

	list.count = 0;
	for (i = 0; i < program_count; i++) {
		if (programs[i].pid == 1 || programs[i].pid == getpid())
			continue;	/* (not the first program, without which there is no machine; not this one) */
		for (k = list.count++; k > 0 && before(&programs[i], &programs[listed[k - 1]]); k--)
			listed[k] = listed[k - 1];
		listed[k] = i;
	}
	list.selected = -1;
	for (k = 0; k < list.count; k++)
		if (programs[listed[k]].pid == chosen_pid)
			list.selected = k;
	if (list.selected < 0)
		chosen_pid = 0;
	if (list.top > list.count - ui_list_rows(&list))
		list.top = list.count > ui_list_rows(&list) ? list.count - ui_list_rows(&list) : 0;
}

static void draw(void);

static void
while_asking(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw();
}

/* True if a program may be ended: one the desktop is made of is asked about first. */
static int
may_end(const char *name)
{
	static const char *const answers[] = { "End it", "Cancel" };
	char text[200];

	if (strcmp(name, "nano-X") && strcmp(name, "nxbar") && strcmp(name, "emuinit"))
		return 1;
	snprintf(text, sizeof text, "%s is part of the desktop itself.\n%s", name,
		 !strcmp(name, "nxbar") ? "Without it there is no Start menu and no bar." : "Ending it closes every open program.");
	return ui_ask("Monitor", text, answers, 2, while_asking) == 0;
}

/* The button: the selected program is asked to go, and made to if it is pressed again. */
static void
end_program(void)
{
	if (chosen_pid && chosen_pid != asked_pid && list.selected >= 0 && !may_end(programs[listed[list.selected]].name)) {
		said = "Left running.";
		draw();
		return;
	}
	if (!chosen_pid) {
		said = "Choose a program in the list first.";
	} else if (chosen_pid == asked_pid) {
		kill(chosen_pid, SIGKILL);
		said = "Ended.";
		asked_pid = 0;
	} else if (kill(chosen_pid, SIGTERM) == 0) {
		asked_pid = chosen_pid;
		said = "Asked to end. Still there? Press again.";
	} else {
		said = "That one cannot be ended.";
	}
	draw_list();
}

static void
draw(void)
{
	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	draw_tabs();
	if (tab == PROCESSES) {
		showing = -1;
		draw_list();
		GrFlush();
		return;
	}
	showing = pane == CPU && per_core ? PANES : pane;
	draw_side(1);
	draw_pane_head();
	ui_fill(window, GRAPH_X, GRAPH_Y, PLOT_W + 2, PLOT_H + 2, BLACK);
	ui_bevel(window, GRAPH_X - 1, GRAPH_Y - 1, PLOT_W + 4, PLOT_H + 4, 1);
	/* (the plot itself is not the window system's to draw: the next frame has it again) */
	GrFlush();
	locate();
	whole = 1;	/* (without workers: at the next sample, when the window system has cleared it) */
	draw_stats(1);
}

/* A second's samples taken and shown: the plots move left, the text is written again. */
static void
sample(unsigned speed_now, long ms)
{
	static unsigned lists_before, copies_before, passes_before;
	unsigned passes;
	unsigned top = 1000000, c;
	int i, k, rescaled = 0;

	for (i = 0; i < PANES; i++) {
		memmove(plots[i].low, plots[i].low + 1, (HISTORY - 1) * sizeof plots[i].low[0]);
		memmove(plots[i].high, plots[i].high + 1, (HISTORY - 1) * sizeof plots[i].high[0]);
		memmove(plots[i].low_px, plots[i].low_px + 1, (HISTORY - 1) * sizeof plots[i].low_px[0]);
		memmove(plots[i].high_px, plots[i].high_px + 1, (HISTORY - 1) * sizeof plots[i].high_px[0]);
	}
	speed = speed_now;
	workers_rate = sample_cores(ms);
	sample_linux(ms);
	sample_memory();
	/* a core's most: 8,192 instructions a pass the machine made; without that word, core 0's
	   speed over how busy Linux kept it */
	passes = core_words ? core_words[0x32c / 4] : 0;
	if (passes && passes_before && passes != passes_before)
		capacity = (unsigned)((unsigned long long)(passes - passes_before) * 8192u * 1000u / (unsigned)ms);
	else if (busy >= 20) {
		unsigned estimate = (unsigned)((unsigned long long)speed * 100 / busy);

		capacity = capacity ? (capacity * 3 + estimate) / 4 : estimate;
	}
	passes_before = passes;
	if (capacity < speed)
		capacity = speed;
	if (capacity < core_busiest)
		capacity = core_busiest;
	plots[CPU].low[HISTORY - 1] = speed;
	plots[CPU].high[HISTORY - 1] = workers_rate;
	if (core_words) {
		unsigned lists = core_words[0x1c / 4], copies = core_words[0x38 / 4];

		lists_rate = lists_before ? (unsigned)((lists - lists_before) * 1000u / (unsigned)ms) : 0;
		copies_rate = copies_before ? (unsigned)((copies - copies_before) * 1000u / (unsigned)ms) : 0;
		lists_before = lists, copies_before = copies;
	}
	plots[GPU].low[HISTORY - 1] = lists_rate;
	for (i = 0; i < 2; i++) {
		int which = i ? GPU : CPU;
		unsigned least = which == CPU ? 1000000 : 16;

		/* the plot is as tall as its busiest second needs, in doublings */
		for (top = least, k = 0; k < HISTORY; k++)
			while (plots[which].low[k] + plots[which].high[k] > top)
				top *= 2;
		if (top != plots[which].top) {
			plots[which].top = top;
			for (k = 0; k < HISTORY - 1; k++)
				scale_sample(which, k);
			rescaled |= which == showing;
		}
	}
	for (i = 0; i < PANES; i++)
		scale_sample(i, HISTORY - 1);
	/* every core's load: core 0's is Linux's */
	for (c = 0; c < MAX_CORES; c++) {
		unsigned load = c == 0 ? busy : capacity ? (unsigned)((unsigned long long)core_rate[c] * 100 / capacity) : 0;

		memmove(core_load[c], core_load[c] + 1, CELL_SAMPLES - 1);
		core_load[c][CELL_SAMPLES - 1] = (unsigned char)(load > 100 ? 100 : load);
	}
	if (tab == PERFORMANCE) {
		draw_side(0);
		draw_stats(0);
	}
	if (!workers)
		take_workers();
	if (!workers && tab == PERFORMANCE)
		shade_here(rescaled);
	if (print && workers)
		printf("nxmon: %u frames of the plot from %d workers\n", frames_shaded, workers);
	frames_shaded = 0;
}

/* Milliseconds: the machine's clock word when it is mapped (a load), else the kernel's. */
static unsigned
ms_now(void)
{
	struct timeval now;

	if (core_words)
		return core_words[0x34 / 4];
	gettimeofday(&now, NULL);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_usec / 1000);
}

static GR_EVENT taken;
static int took;

static void
take(GR_EVENT *event)
{
	taken = *event;
	took = 1;
}

/* The next event, or a timeout after `ms`. GrGetNextEventTimeout asks the window system anew
 * at every call, which wakes it sixteen times a second for the frames' sake: here it is
 * asked once (GrPrepareSelect) and the wait is this program's, on the socket that told. */
static void
next_event(GR_EVENT *event, int ms)
{
	static int asked, fd = -1;
	struct timeval limit;
	fd_set set;
	int most = 0;

	FD_ZERO(&set);
	took = 0;
	GrServiceSelect(&set, take);	/* one that came with an answer to something else */
	if (took)
		asked = 0;		/* (and was the answer to the asking, too) */
	if (!took && !asked) {
		GrPrepareSelect(&most, &set);
		for (fd = most; fd > 0 && !FD_ISSET(fd, &set); fd--)
			continue;
		asked = 1;
	}
	if (!took) {
		FD_ZERO(&set);
		FD_SET(fd, &set);
		limit.tv_sec = ms / 1000, limit.tv_usec = ms % 1000 * 1000;
		if (select(fd + 1, &set, NULL, NULL, &limit) > 0) {
			GrServiceSelect(&set, take);
			asked = 0;
		}
	}
	if (took)
		*event = taken;
	else
		event->type = GR_EVENT_TYPE_TIMEOUT;
}

/* A click or a key that changes what is shown: the tab, the pane, the CPU's view, the list's
 * order. The workers' frame is finished first (they read what is shown); true if it was one. */
static int
choose(int new_tab, int new_pane, int new_per_core, int new_sort)
{
	if (new_tab == tab && new_pane == pane && new_per_core == per_core && new_sort == sort_by)
		return 0;
	finish_frame();
	tab = new_tab, pane = new_pane, per_core = new_per_core, sort_by = new_sort;
	if (!check) {
		reg_set_int("nxmon.tab", tab);
		reg_set_int("nxmon.pane", pane);
		reg_set_int("nxmon.cores", per_core);
	}
	list_programs();
	draw();
	return 1;
}

/* What a click does: the tabs, the panes on the left, the CPU's views, a column's title. */
static int
clicked(int x, int y)
{
	int i;

	if (ui_inside(x, y, 8, TAB_Y - 2, 90, 24))
		return choose(PROCESSES, pane, per_core, sort_by);
	if (ui_inside(x, y, 100, TAB_Y - 2, 104, 24))
		return choose(PERFORMANCE, pane, per_core, sort_by);
	if (tab == PERFORMANCE) {
		for (i = 0; i < PANES; i++)
			if (ui_inside(x, y, 8, TOP + 4 + i * SIDE_ROW, SIDE_W, SIDE_ROW - 6))
				return choose(tab, i, per_core, sort_by);
		if (pane == CPU && ui_inside(x, y, VIEW_X, VIEW_Y, 150, 20))
			return choose(tab, pane, x >= VIEW_X + 76, sort_by);
		return 0;
	}
	if (y >= TOP && y < LIST_Y)
		for (i = COLUMNS - 1; i >= 0; i--)
			if (x >= list.x + column_x[i])
				return choose(tab, pane, per_core, i);
	return 0;
}

#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_BUTTON_DOWN)

int
main(void)
{
	GR_EVENT event;
	unsigned then, now, looked;
	unsigned count_then = instructions();
	void *words = (gpu = open("/dev/gpu", O_RDWR)) < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000);

	if (words != MAP_FAILED)
		core_words = words;
	words = gpu < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000 + 0x6ba000);
	if (words != MAP_FAILED)
		core_more = words;
	/* the workers' owner words (WORKERS_OWNER, 0x86c01400) */
	words = gpu < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x00c01000);
	if (words != MAP_FAILED)
		owners = words;
	if (GrOpen() < 0)
		return 1;
	ui_init();
	{
		extern char __DATA_BEGIN__[], _end[];

		/* (NXMON_WORKERS=0: this core shades; NXMON_CHECK=1: it shades what the workers did again and
		 * compares, with the first view whatever the registry says) */
		no_workers = getenv("NXMON_WORKERS") && atoi(getenv("NXMON_WORKERS")) == 0;
		check = getenv("NXMON_CHECK") != NULL;
		print = getenv("NXMON_PRINT") != NULL;
		checked = check ? malloc(PLOT_H * PLOT_W * sizeof checked[0]) : NULL;
		if (!check) {
			tab = reg_int("nxmon.tab", PERFORMANCE) ? PERFORMANCE : PROCESSES;
			pane = reg_int("nxmon.pane", CPU);
			pane = pane < 0 || pane >= PANES ? CPU : pane;
			per_core = reg_int("nxmon.cores", 0) != 0;
		}
		/* (NXMON_SHOW=processes, cpu, cores, memory or gpu: that view first) */
		if (getenv("NXMON_SHOW")) {
			const char *show = getenv("NXMON_SHOW");

			tab = strcmp(show, "processes") ? PERFORMANCE : PROCESSES;
			pane = !strcmp(show, "memory") ? MEMORY : !strcmp(show, "gpu") ? GPU : CPU;
			per_core = !strcmp(show, "cores");
		}
		/* what a worker reads, there before it steps on it */
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
		atexit(mcw_close);
	}
	sample_cores(1000);
	then = looked = ms_now();
	sample_linux(1000);
	sample_memory();
	sample_programs(1000);
	window = GrNewWindowEx(UI_FIXED, "Monitor", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, EVENTS);
	list_programs();
	GrMapWindow(window);
	for (;;) {
		long ms;

		/* (with workers, a frame of the plot at every look; this core sleeps between) */
		next_event(&event, workers && tab == PERFORMANCE ? FRAME_MS : 1000);
		/* Escape and q close it */
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ || (event.type == GR_EVENT_TYPE_KEY_DOWN &&
		    (event.keystroke.ch == MWKEY_ESCAPE || event.keystroke.ch == 'q'))) {
			GrClose();
			return 0;
		}
		if (event.type == GR_EVENT_TYPE_EXPOSURE) {
			finish_frame();
			draw();
		}
		if (event.type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(&event) && clicked(event.button.x, event.button.y))
			continue;
		/* Ctrl+Tab, or 1 and 2: the other tab */
		if (event.type == GR_EVENT_TYPE_KEY_DOWN && (event.keystroke.ch == '1' || event.keystroke.ch == '2' ||
		    (event.keystroke.ch == MWKEY_TAB && (event.keystroke.modifiers & MWKMOD_CTRL)))) {
			choose(event.keystroke.ch == '1' ? PROCESSES : event.keystroke.ch == '2' ? PERFORMANCE : !tab, pane, per_core, sort_by);
			continue;
		}
		/* the list: a click or the arrows choose a program, the button or Delete ends it */
		if (tab == PROCESSES && (event.type == GR_EVENT_TYPE_BUTTON_DOWN || event.type == GR_EVENT_TYPE_MOUSE_MOTION ||
		    event.type == GR_EVENT_TYPE_KEY_DOWN)) {
			int changed;

			/* the pointer's moves while a button is held (the list's bar), and not otherwise */
			if (event.type == GR_EVENT_TYPE_BUTTON_DOWN ? !ui_wheel(&event) : event.type == GR_EVENT_TYPE_MOUSE_MOTION && !event.mouse.buttons)
				ui_moves(window, EVENTS, event.type == GR_EVENT_TYPE_BUTTON_DOWN);
			changed = event.type == GR_EVENT_TYPE_KEY_DOWN ? ui_list_key(&list, event.keystroke.ch) : ui_list_event(&list, &event);

			if (changed) {
				chosen_pid = list.selected >= 0 ? programs[listed[list.selected]].pid : 0;
				said = "";
				draw_list();
			}
			if ((event.type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(&event) &&
			     ui_inside(event.button.x, event.button.y, 8, HEIGHT - 30, END_W, 22)) ||
			    (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == MWKEY_DELETE))
				end_program();
		}
		now = ms_now();
		ms = (long)(now - then);
		if (workers && mcw_asked())
			give_workers();
		/* the workers read the samples: a new one is taken between two of their frames */
		if (workers && !frame_done())
			continue;
		if (shading) {
			shading = 0;
			frames_shaded++;
			if (check)
				check_frame();
		}
		if (ms < 1000) {
			if (workers && tab == PERFORMANCE) {
				if (event.type != GR_EVENT_TYPE_TIMEOUT)
					settle();
				post_frame((int)(ms * 256 / 1000));
			}
			continue;
		}
		{
			unsigned count_now = instructions();

			sample((unsigned)((unsigned long long)(count_now - count_then) * 1000 / ms), ms);
			count_then = count_now;
			then = now;
		}
		/* the processes are looked at every other second: each is two files to read */
		ms = (long)(now - looked);
		if (ms >= 2000) {
			sample_programs(ms);
			looked = now;
			list_programs();
			if (tab == PROCESSES)
				draw_list();
		}
		if (workers && tab == PERFORMANCE) {
			settle();
			post_frame(0);
		} else
			GrFlush();	/* (without workers nothing else sends what was drawn until an event comes) */
	}
}
