/* nxphys: rigid boxes, every worker core moving its share of them (docs/physics.md).
 * Keys, and the bar's buttons: 1 2 3 a wall, towers, a heap; space the heavy box thrown; p stop;
 * w each box in its core's colour; a / 0 all the workers, none; r again; q leave. The pointer
 * carries the box it is pressed on, or turns the view; the wheel (+ -) brings it or the eye nearer. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES/segl.h>
#include "ui.h"
#include "mcw.h"
#include "physics.h"

#define MAX_BODIES 2048
#define ROOM 12.0f		/* the walls are this far from the middle */
#define ROOM_HIGH 8		/* cells of the grid upwards */
#define TILES 12		/* the floor's squares each way */
#define FLOOR_QUADS (TILES * TILES)
#define SHADES 64
#define COLOURS 16

/* A share's own two texels: the substep it is to make next, the picture it last made, what had
 * no room, and (PHYS_COUNT) the instructions of its grids, its substeps with a look and without. */
struct share { volatile uint32_t phase, shown; uint32_t trouble, pad; uint32_t spent[3], most; };

/* Core 0's: the workers only read it. */
static struct {
	struct phys w;
	struct share *shares;
	uint32_t *quads[2];	/* a picture's quads, for the GPU: one drawn while the other is made */
	uint8_t *colours;
	/* the shares: `nbody` of them have the bodies (body i is share i mod nbody's), and the
	 * `nservice` after those the grid and the pictures; with none of those every share has
	 * its bodies, its cells of the grid and its bodies' part of a picture */
	int nshares, nbody, nservice;
	uint32_t *costs;	/* PHYS_COUNT: what each substep cost each share, `cost_row` words a share */
	uint32_t cost_row;
	uint32_t base;		/* substeps made before the shares' first */
	volatile uint32_t limit, show_step, show;	/* picture `show` is of substep show_step, or the first after */
	volatile int stop, owners;
	int count;
	int ahead;		/* PHYS_ORDER=ahead: the look for a pair of substeps is made after the pair before, not just before it */
	int spin;		/* PHYS_SPIN: a share with nothing to do turns in place, as it did */
} sim __attribute__((aligned(16)));
static char sim_end[16] __attribute__((aligned(16), used));	/* (nothing else in sim's last 16 bytes) */

static inline uint32_t
cycles(void)
{
	uint32_t count;

	__asm__ volatile("rdcycle %0" : "=r"(count));
	return count;
}

/* Share s's part of the picture being made: boxes as quads. */
static void
show_share(int s, int from, int into)
{
	uint32_t *quads = sim.quads[into] + 16 * FLOOR_QUADS;
	int owners = sim.owners, every = sim.nservice ? sim.nservice : sim.nshares, i;

	for (i = sim.nservice ? s - sim.nbody : s; i < sim.w.bodies; i += every)
		phys_quads(&sim.w, i, from, owners ? (uint32_t)(i % sim.nbody) % COLOURS : sim.colours[i], quads + PHYS_QUAD_WORDS * i);
}

/* The first share from `at` on that has not made the substep before `phase`, or n. (The share
 * that is last to make a substep looks at every other each time: four a turn.) */
static inline int
behind(int at, int n, uint32_t phase)
{
	const struct share *shares = sim.shares;

	for (; at + 4 <= n; at += 4) {
		if (shares[at].phase < phase) return at;
		if (shares[at + 1].phase < phase) return at + 1;
		if (shares[at + 2].phase < phase) return at + 2;
		if (shares[at + 3].phase < phase) return at + 3;
	}
	for (; at < n; at++)
		if (shares[at].phase < phase)
			break;
	return at;
}

/* What share s can do now, done, once every share has made the substep before (`seen` is how
 * far that was found so, the number of shares when it is): its part of the picture asked for,
 * or its next substep. A share of the grid and the pictures has no bodies. */
static int
share_advance(int s, int *seen)
{
	struct share *st = &sim.shares[s];
	uint32_t phase = st->phase, show = sim.show, kind = phase % PHYS_EVERY;
	int from = (int)((sim.base + phase) % PHYS_BUFFERS), trouble = 0, n = sim.nshares, bodied = s < sim.nbody, serves = !sim.nservice || !bodied;
	int wanted = serves && st->shown != show && phase >= sim.show_step;
	/* (a look's number is that of the pair of substeps it is for. Every PHYS_EVERY-th substep
	 * looks for contacts first; or, looking ahead, the substep before does when it has said
	 * its boxes are made: the longest look then holds no share up. docs/physics.md) */
	int turn = (int)((sim.base + phase) / PHYS_EVERY) + 1, ahead = sim.ahead;
	uint32_t began = 0, took = 0;

	if (!wanted && phase >= sim.limit)
		return 0;
	if (*seen < n && (*seen = behind(*seen, n, phase)) < n)
		return 0;
	if (wanted) {
		/* (the bodies are all at this substep, and none goes on until this share does) */
		show_share(s, from, (int)(show & 1));
		st->shown = show;
		return 1;
	}
	if (phase >= sim.limit)
		return 0;
	if (sim.count)
		began = cycles();
	/* (looking ahead, the first substep of all has its look first too: there was none before it) */
	if (bodied)
		trouble = phys_step(&sim.w, s, sim.nbody, from, (ahead ? phase == 0 : kind == 0) ? turn : 0);
	if (sim.count) {
		took = cycles() - began;
		st->spent[kind == 0 && !ahead ? 1 : 2] += took;
		if ((kind || ahead) && took > st->most)
			st->most = took;
		began = cycles();
	}
	/* the grid the next look reads: made a substep before that look */
	if (kind == (ahead ? 0 : PHYS_EVERY - 1) && serves)
		trouble += phys_grid(&sim.w, s, from, ahead ? turn + 1 : (int)((sim.base + phase + 1) / PHYS_EVERY) + 1);
	if (sim.count) {
		st->spent[0] += cycles() - began;
		took += cycles() - began;
	}
	*seen = 0;
	st->phase = phase + 1;
	if (ahead && kind == PHYS_EVERY - 1 && bodied) {
		/* the next pair's look, where the bodies were before this substep: nobody waits for it */
		if (sim.count)
			began = cycles();
		trouble += phys_look(&sim.w, s, sim.nbody, from, (int)((sim.base + phase + 1) / PHYS_EVERY) + 1);
		if (sim.count) {
			st->spent[1] += cycles() - began;
			took += cycles() - began;
		}
	}
	if (sim.costs && phase < sim.cost_row)
		sim.costs[(uint32_t)s * sim.cost_row + phase] = took;
	if (trouble)
		st->trouble += (uint32_t)trouble;
	return 1;
}

/* A worker's job: its share, for as long as the boxes move. (Run by core 0, it does nothing:
 * that core has its own share and those of the workers that have no core, in its own loop.) */
static uint32_t
share_job(uint32_t s, uint32_t worker)
{
	int seen = 0;

	if (!mcw_on_worker())
		return 0;
	/* (with nothing to do its pass ends there and it looks again in the next: turning in
	 * place it cost the machine the pass's instructions, 8,192 of them, for nothing) */
	while (!sim.stop && !mcw_recalled((int)worker))
		if (!share_advance((int)s, &seen) && !sim.spin)
			mc_end_pass();
	return 1;
}

/* ---- core 0 ---- */

#define BAR 54			/* the bar under the picture: a row of buttons and a line of figures */
#define BUTTONS 7
#define LENS 0.6f		/* how far the view opens up and down at one unit ahead */
static const struct { const char *label; int key; } buttons[BUTTONS] = {
	{" Wall", '1'}, {" Towers", '2'}, {" Heap", '3'}, {" Throw", ' '}, {" Stop", 'p'}, {" Cores", 'w'}, {" Workers", 'W'},
};
static GR_WINDOW_ID window, picture;
static int bar_y, wide, button_step = 90;	/* (the buttons share the bar's width) */
static char status[120] = "";
static float eye[3], ahead[3], right[3], up[3];
/* the button held in the picture: where the window is on the display, where the pointer was
 * last, and how far ahead of the eye the held box's point was taken */
static int talk;	/* PHYS_SAY: lines on the console as it runs (a line is a tenth of a second of this core) */
static int holding, held_for_good, origin_x, origin_y, last_x, last_y;
static float held_far;
static int view_moved, shown_buffer = -1;	/* the view is not the shown picture's; which buffer that picture is */
static GLuint texture;
/* The workers asked for: all of them. With more than the bodies need, some have the grid and
 * the pictures (service_of); the wall's 240 substeps took 1,740 ms with 207 and 1,704 to 1,718
 * with 223 to 255, the heap's 120 took 1,181 and 1,133 (docs/physics.md). */
#define WORKERS_ASKED 255
static int workers, wanted = WORKERS_ASKED, stopped, scene = 1, bodies_wanted = 400, total_quads;
#define OWN_UP_TO 7		/* this core has a share of its own with so many workers or fewer: with more it costs passes */
#define SERVICE_FROM 64		/* from so many shares on some of them have no bodies, but the grid and the pictures; */
#define SERVICE_LEAST 16	/* never fewer of those than so many: the grid is every body's key looked at by each */
static int own;			/* 1: share 0 is this core's and worker k has share k; 0: worker k has share k - 1 */
static uint8_t running[MC_MAX_CORES];	/* worker k has its share's job out */
static int seen_by[MC_MAX_CORES];
static int first_own, jobs_changed = 1;	/* its own apart, no share before first_own is this core's (found again as jobs begin and end) */
static int shown_so_far;	/* shares found to have made the picture asked for */
static float view_turn = 1.15f, view_high = 11.0f, view_far = 22.0f;	/* along the wall and from above: both its sides show */

static const volatile uint32_t *
control(int offset)
{
	return (const volatile uint32_t *)((const char *)seglPalette() - 0x400 + offset);
}

static unsigned now_ms(void) { return *control(0x34); }

static uint32_t waited, waited_before;	/* passes this core ended with nothing to do; and when the shares began */

static void
pass(void)
{
	waited++;
	mcw_rest();	/* the kernel has the core for what is left of the pass (the pause hint with no workers) */
}

static inline uint32_t bits(float f) { union { float f; uint32_t u; } c = {f}; return c.u; }

/* The colours' table: a row a colour, 64 shades of it across; and the floor's squares. */
static void
make_table(uint32_t *texels)
{
	static const uint8_t colours[COLOURS][3] = {
		{225, 80, 60}, {240, 150, 50}, {235, 205, 70}, {100, 195, 90}, {70, 185, 200}, {80, 125, 230}, {150, 100, 220}, {225, 110, 175},
		{150, 152, 158}, {105, 108, 116}, {250, 250, 250}, {140, 255, 60}, {60, 255, 190}, {255, 120, 120}, {120, 170, 255}, {255, 200, 140},
	};
	int c, s, x, z, corner;

	for (c = 0; c < COLOURS; c++)
		for (s = 0; s < SHADES; s++)
			texels[c * SHADES + s] = (uint32_t)(colours[c][0] * s / (SHADES - 1)) << 16 | (uint32_t)(colours[c][1] * s / (SHADES - 1)) << 8 |
				(uint32_t)(colours[c][2] * s / (SHADES - 1));
	for (z = 0; z < TILES; z++)
		for (x = 0; x < TILES; x++) {
			float side = 2.0f * ROOM / TILES, x0 = -ROOM + side * (float)x, z0 = -ROOM + side * (float)z;
			uint32_t texel = (56u * 16 + 8) | (uint32_t)(((x ^ z) & 1 ? 8 : 9) * 64 + 32) << 16, q[16];

			for (corner = 0; corner < 4; corner++) {
				q[4 * corner] = bits(x0 + (corner == 1 || corner == 2 ? side : 0.0f));
				q[4 * corner + 1] = 0;
				q[4 * corner + 2] = bits(z0 + (corner >= 2 ? side : 0.0f));
				q[4 * corner + 3] = texel;
			}
			memcpy(sim.quads[0] + 16 * (z * TILES + x), q, sizeof q);
			memcpy(sim.quads[1] + 16 * (z * TILES + x), q, sizeof q);
		}
}

static uint32_t chance = 12345;
static float
random_unit(void)
{
	chance = chance * 1103515245u + 12345u;
	return (float)(chance >> 8 & 0xffff) * (1.0f / 65536.0f);
}

/* A box put in the room at rest, upright; where the next goes. */
static int
put(int at, float hx, float hy, float hz, float density, float x, float y, float z, int colour)
{
	struct body_state *s = &sim.w.states[0][at];

	if (at >= MAX_BODIES)
		return at;
	memset(s, 0, sizeof *s);
	phys_box(&sim.w, at, hx, hy, hz, density);
	s->pos[0] = x, s->pos[1] = y, s->pos[2] = z;
	s->q[3] = 1.0f;
	sim.colours[at] = (uint8_t)colour;
	return at + 1;
}

/* The heavy box, the last body, sent from one end of the room at the middle. */
static void
throw_box(int buffer)
{
	struct body_state *s = &sim.w.states[buffer][sim.w.bodies - 1];

	memset(s, 0, sizeof *s);
	s->pos[0] = 0.0f, s->pos[1] = 2.2f, s->pos[2] = ROOM - 2.0f;
	s->q[3] = 1.0f;
	s->vel[1] = 2.0f, s->vel[2] = -14.0f;
	s->spin[0] = 1.5f, s->spin[1] = 0.7f;
	s->key = phys_key(&sim.w, s->pos);
}

/* Memory nobody has written yet (zeros, from mcw_alloc()) is there for the workers: a store
 * of a zero into each of its pages. A worker stops at every page that is not. */
static void
there(void *memory, unsigned bytes)
{
	volatile uint32_t *at = memory, *end = (volatile uint32_t *)((char *)memory + bytes);

	for (; at < end; at = (volatile uint32_t *)(((uintptr_t)at | 4095) + 1))
		*at = 0;
}

/* What the workers read and write of the first n bodies is there (the rest is this core's to write). */
static void
bodies_there(int n)
{
	static int so_far;
	int k;

	if (n <= so_far)
		return;
	for (k = 0; k < PHYS_BUFFERS; k++)
		there(&sim.w.poses[k][so_far], (unsigned)(n - so_far) * sizeof(struct body_pose));
	for (k = 1; k < PHYS_BUFFERS; k++)
		there(&sim.w.states[k][so_far], (unsigned)(n - so_far) * sizeof(struct body_state));
	there(&sim.w.info[so_far], (unsigned)(n - so_far) * sizeof(struct body_info));
	there(&sim.w.manifolds[so_far * PHYS_MANIFOLDS], (unsigned)(n - so_far) * PHYS_MANIFOLDS * sizeof(struct manifold));
	/* (and their quads of both pictures: GPU memory is a worker's to write after a store of this core's) */
	for (k = 0; k < 2; k++)
		there(sim.quads[k] + 16 * FLOOR_QUADS + PHYS_QUAD_WORDS * so_far, (unsigned)(n - so_far) * PHYS_QUAD_WORDS * 4);
	so_far = n;
}

/* A scene's boxes, into states[0]: a wall of bricks, towers of cubes, or boxes let fall. */
static void
set_scene(void)
{
	int n = 0, i, j, k;

	chance = 12345;
	if (scene == 1) {
		/* every other row half a brick along, with a half brick at each end: nothing hangs over */
		for (j = 0; j < (getenv("PHYS_ROWS") ? atoi(getenv("PHYS_ROWS")) : 10); j++)
			for (i = 0; i < (j & 1 ? 21 : 20); i++) {
				int half = j & 1 && (i == 0 || i == 20);
				float x = j & 1 ? (i == 0 ? -10.0f : i == 20 ? 9.5195f : -9.75f + 0.5005f + 1.001f * (float)(i - 1)) : -9.75f + 1.001f * (float)i;

				n = put(n, half ? 0.25f : 0.5f, 0.25f, 0.25f, 1.0f, x, 0.25f + 0.501f * (float)j, 0.0f, (i + j / 2) % 8);
			}
	} else if (scene == 2) {
		for (k = 0; k < 9; k++)
			for (j = 0; j < 12; j++)
				n = put(n, 0.3f, 0.3f, 0.3f, 1.0f, -3.0f + 3.0f * (float)(k % 3), 0.3f + 0.601f * (float)j, -3.0f + 3.0f * (float)(k / 3), (k + j) % 8);
	} else if (scene == 4) {
		/* (a test's: one tower) */
		for (j = 0; j < bodies_wanted - 1; j++)
			n = put(n, 0.3f, 0.3f, 0.3f, 1.0f, 0.0f, 0.3f + 0.601f * (float)j, 0.0f, j % 8);
	} else {
		for (i = 0; i < bodies_wanted - 1; i++) {
			struct body_state *s = &sim.w.states[0][n];
			float q[4] = {random_unit() - 0.5f, random_unit() - 0.5f, random_unit() - 0.5f, random_unit() + 0.2f}, len;

			n = put(n, 0.25f + 0.25f * random_unit(), 0.2f + 0.15f * random_unit(), 0.25f + 0.2f * random_unit(), 1.0f,
				-5.2f + 1.3f * (float)(i % 9), 1.5f + 1.3f * (float)(i / 81), -5.2f + 1.3f * (float)(i / 9 % 9), i % 8);
			len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			for (k = 0; k < 4; k++)
				s->q[k] = q[k] / len;
		}
	}
	n = put(n, 0.4f, 0.4f, 0.4f, 6.0f, 0, 0, 0, 10);
	sim.w.bodies = n;
	bodies_there(n);
	sim.base = 0;
	if (scene < 3 && !getenv("PHYS_STILL"))
		throw_box(0);
	else
		sim.w.states[0][n - 1].pos[1] = 0.4f, sim.w.states[0][n - 1].pos[0] = ROOM - 1.0f;
}

/* How many of n shares have no bodies, but the grid and the pictures: none of a few, three
 * in sixteen of many, and no more than leaves the fullest share with as many bodies as it
 * would have (a share is as long as its bodies: one more of them costs more than the grid). */
static int
service_of(int n)
{
	const char *text = getenv("PHYS_SERVICE");
	int most = n * 3 / 16, each = (sim.w.bodies + n - 1) / n, spare;

	if (text)
		return atoi(text) > 0 && atoi(text) < n ? atoi(text) : 0;
	if (n < SERVICE_FROM)
		return 0;
	spare = n - (sim.w.bodies + (each > 0 ? each : 1) - 1) / (each > 0 ? each : 1);
	if (spare < most)
		most = spare;
	return most >= SERVICE_LEAST ? most : 0;
}

/* The bodies shared out among `n` shares, all to begin a round of phases. */
static void
set_shares(int n)
{
	int s, i, buffer = (int)(sim.base % PHYS_BUFFERS);

	sim.nshares = n;
	sim.nservice = service_of(n);
	sim.nbody = n - sim.nservice;
	sim.w.cell_shares = sim.nservice ? sim.nservice : n, sim.w.cell_first = sim.nservice ? sim.nbody : 0;
	jobs_changed = 1, shown_so_far = 0;
	for (s = 0; s < n; s++) {
		memset(&sim.shares[s], 0, sizeof sim.shares[s]);
		sim.shares[s].shown = sim.show;
		seen_by[s] = 0;
	}
	/* (a body's key names its cell's share: other shares, other keys) */
	for (i = 0; i < sim.w.bodies; i++)
		sim.w.states[buffer][i].key = phys_key(&sim.w, sim.w.states[buffer][i].pos);
	/* (and the grid the first look needs: the shares make every one after) */
	phys_grid_all(&sim.w, buffer, (int)(sim.base / PHYS_EVERY) + 1);
}

static uint32_t
phase_least(void)
{
	uint32_t least = ~0u;
	int s;

	for (s = 0; s < sim.nshares; s++)
		if (sim.shares[s].phase < least)
			least = sim.shares[s].phase;
	return least;
}

static uint32_t
phase_most(void)
{
	uint32_t most = 0;
	int s;

	for (s = 0; s < sim.nshares; s++)
		if (sim.shares[s].phase > most)
			most = sim.shares[s].phase;
	return most;
}

/* PHYS_COUNT: how many of this core's passes each substep took, with a look and without (it
 * sees a substep made a pass late, every one alike). */
static uint32_t passes_of[2][8], noted_phase, noted_at;

static void
note_substeps(void)
{
	uint32_t least = phase_least();

	for (; noted_phase < least; noted_phase++, noted_at = waited) {
		uint32_t took = waited - noted_at;

		passes_of[noted_phase % PHYS_EVERY != 0][took < 7 ? took : 7]++;
	}
}

/* This core's own share, when it has one, and those of workers that have no core or no job out. */
static int
own_shares(void)
{
	int s, did = 0;

	/* (not every share looked at every turn: this core's turns are a share's time too) */
	if (jobs_changed) {
		for (first_own = own; first_own < sim.nshares && running[first_own + 1 - own]; first_own++)
			;
		jobs_changed = 0;
	}
	if (own)
		did = share_advance(0, &seen_by[0]);
	for (s = first_own; s < sim.nshares; s++)
		if (!running[s + 1 - own])
			did |= share_advance(s, &seen_by[s]);
	return did;
}

/* Which jobs have ended (a core the kernel wanted back, or a job this core was left to run). */
static void
look_at_jobs(void)
{
	int k;

	for (k = 1; k <= workers; k++)
		if (running[k] && mcw_done(k))
			running[k] = 0, jobs_changed = 1;
}

static void
start_jobs(void)
{
	int k;

	sim.stop = 0;
	for (k = 1; k <= workers && k - 1 + own < sim.nshares; k++)
		if (!running[k]) {
			running[k] = 1, jobs_changed = 1;
			mcw_post(k, share_job, (uint32_t)(k - 1 + own), (uint32_t)k);
		}
}

/* Every share brought to one phase and the jobs ended: the bodies are then one state. */
static void
settle(void)
{
	uint32_t limit = sim.limit, most = phase_most();
	int k;

	if (most < limit)
		sim.limit = most;
	while (phase_least() < sim.limit) {
		look_at_jobs();
		if (!own_shares())
			pass();
	}
	sim.stop = 1;
	for (k = 1; k <= workers; k++)
		if (running[k]) {
			mcw_wait(k);
			running[k] = 0, jobs_changed = 1;
		}
	sim.limit = limit;
}

/* After settle(): the shares begin again from where the bodies are, with a look. */
static void
begin_again(void)
{
	sim.base += sim.nshares ? sim.shares[0].phase : 0;
}

static void share_out(void);

/* The workers given back and as many taken as wanted, and a share for each. */
static void
take_workers(void)
{
	extern char __DATA_BEGIN__[], _end[];
	const char *owned = getenv("PHYS_OWN");

	mcw_close();
	workers = wanted > 0 ? mcw_open(wanted) : 0;
	if (workers) {
		extern char __executable_start[], etext[];
		const volatile char *code;

		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
		/* and the program's code: a page of it that no core has run yet stops every worker that comes to it */
		for (code = __executable_start; code < etext; code += 4096)
			(void)*code;
	}
	/* (a share a worker, and with few of them one for this core, which would only wait:
	 * with many, every share waiting for this core's as well costs more passes than it saves) */
	own = workers > 0 && (owned ? atoi(owned) != 0 : workers <= OWN_UP_TO);
	share_out();
}

/* The scene's bodies shared out among the workers there are. */
static void
share_out(void)
{
	const char *text = getenv("PHYS_SHARES");
	int n = text && atoi(text) > workers ? atoi(text) : workers ? workers + own : 1;

	set_shares(n < MC_MAX_CORES - 1 + own ? n : MC_MAX_CORES - 1 + own);
}

/* The picture the shares have made, shown; or (again) the one shown last, from where the view
 * is now: turning the view asks nothing of the shares and waits for no substep. */
static void
show_picture(int again)
{
	if (!again)
		shown_buffer = (int)(sim.show & 1);
	if (shown_buffer < 0)
		return;
	view_moved = 0;
	seglKeptQuads(0, sim.quads[shown_buffer], 0, FLOOR_QUADS + 6 * sim.w.bodies, texture, 255, 0);
	seglKeptBlend(0, 0, 1);
	seglKeptMatrices();
	glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	seglSwapEarly();	/* (no waiting for it: ask_picture() settles before that buffer is written again) */
}

static void
ask_picture(void)
{
	uint32_t at = phase_most() + 1, last = sim.limit;

	/* (the list being drawn may read the buffer the shares are about to write) */
	seglSettle();
	sim.show_step = at < last ? at : last;
	sim.show++;
	shown_so_far = 0;
}

static int
picture_made(void)
{
	/* (with shares of the pictures, those: the ones before them make none) */
	if (shown_so_far < sim.nbody && sim.nservice)
		shown_so_far = sim.nbody;
	for (; shown_so_far < sim.nshares; shown_so_far++)
		if (sim.shares[shown_so_far].shown != sim.show)
			return 0;
	return 1;
}

/* The view: from a circle round the room's middle, looking at the middle. */
static void
set_view(void)
{
	float to[3] = {0.0f, 2.5f, 0.0f}, len, m[16];
	int width, height, k;

	eye[0] = view_far * sinf(view_turn), eye[1] = view_high * view_far * (1.0f / 22.0f), eye[2] = view_far * cosf(view_turn);
	for (len = 0.0f, k = 0; k < 3; k++)
		ahead[k] = to[k] - eye[k], len += ahead[k] * ahead[k];
	for (len = sqrtf(len), k = 0; k < 3; k++)
		ahead[k] /= len;
	right[0] = -ahead[2], right[1] = 0.0f, right[2] = ahead[0];	/* ahead x up */
	len = sqrtf(right[0] * right[0] + right[2] * right[2]);
	right[0] /= len, right[2] /= len;
	up[0] = right[1] * ahead[2] - right[2] * ahead[1], up[1] = right[2] * ahead[0] - right[0] * ahead[2], up[2] = right[0] * ahead[1] - right[1] * ahead[0];
	for (k = 0; k < 3; k++)
		m[4 * k] = right[k], m[4 * k + 1] = up[k], m[4 * k + 2] = -ahead[k], m[4 * k + 3] = 0.0f;
	m[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
	m[13] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
	m[14] = ahead[0] * eye[0] + ahead[1] * eye[1] + ahead[2] * eye[2];
	m[15] = 1.0f;
	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustumf(-LENS * 0.5f * (float)width / (float)(height > 0 ? height : 1), LENS * 0.5f * (float)width / (float)(height > 0 ? height : 1), -LENS * 0.5f,
		LENS * 0.5f, 0.5f, 100.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(m);
}

/* The way from the eye through a pixel of the picture, at unit length. */
static void
ray(int px, int py, float *way)
{
	int width, height, k;
	float x, y, len = 0.0f;

	seglSize(&width, &height);
	x = (2.0f * ((float)px + 0.5f) / (float)(width > 0 ? width : 1) - 1.0f) * LENS * (float)width / (float)(height > 0 ? height : 1);
	y = (1.0f - 2.0f * ((float)py + 0.5f) / (float)(height > 0 ? height : 1)) * LENS;
	for (k = 0; k < 3; k++)
		way[k] = ahead[k] + right[k] * x + up[k] * y, len += way[k] * way[k];
	for (len = sqrtf(len), k = 0; k < 3; k++)
		way[k] /= len;
}

/* The window is so large now: the picture has all of it but the bar, the buttons its width. */
static void
set_size(int width, int height)
{
	if (width == wide && height == bar_y + BAR)
		return;
	wide = width;
	bar_y = height > BAR + 40 ? height - BAR : 40;
	button_step = wide > 10 + 20 * BUTTONS ? (wide - 10) / BUTTONS : 20;
	GrResizeWindow(picture, wide, bar_y);
	seglWindowChanged();
	set_view();
	view_moved = 1;
}

/* Nearer or further: the held box along the pointer's ray, or with none held the eye. */
static void
zoom(int out)
{
	if (sim.w.grab >= 0) {
		held_far *= out ? 1.1f : 0.9f;
		if (held_far < 2.0f) held_far = 2.0f;
		if (held_far > 60.0f) held_far = 60.0f;
		last_x = -1;	/* (the box's place is worked out again) */
		return;
	}
	view_far *= out ? 1.1f : 0.9f;
	if (view_far < 6.0f) view_far = 6.0f;
	if (view_far > 45.0f) view_far = 45.0f;
	set_view();
	view_moved = 1;
}

/* The bar under the picture: a button for what the keys do, and a line of figures. */
static void
draw_bar(void)
{
	int k;

	ui_fill(window, 0, bar_y, wide, BAR, UI_FACE);
	for (k = 0; k < BUTTONS; k++) {
		int key = buttons[k].key, down = (key == 'p' && stopped) || (key == 'w' && sim.owners) || (key == 'W' && workers > 0);

		ui_button(window, 5 + button_step * k, bar_y + 5, button_step - 4, 22, key == 'p' && stopped ? " Go" : buttons[k].label, down);
	}
	ui_text(window, 8, bar_y + 33, status, -1, BLACK, 0);
	GrFlush();
}

/* What a key or its button does. 1: the scene again; 2: other workers; 3: the heavy box thrown. */
static int
act(int key)
{
	switch (key) {
	case 'q': case MWKEY_ESCAPE: sim.stop = 1; exit(0);
	case 'p': stopped = !stopped; break;
	case 'w': sim.owners = !sim.owners; break;
	case 'r': return 1;
	case '1': case '2': case '3': scene = key - '0'; return 1;
	case 'a': wanted = MC_MAX_CORES; return 2;
	case '0': wanted = 0; return 2;
	case 'W': wanted = workers ? 0 : WORKERS_ASKED; return 2;
	case ' ': return 3;
	case '+': case '=': zoom(0); break;
	case '-': zoom(1); break;
	case MWKEY_LEFT: view_turn -= 0.15f; set_view(); view_moved = 1; break;
	case MWKEY_RIGHT: view_turn += 0.15f; set_view(); view_moved = 1; break;
	case MWKEY_UP: if (view_high < 20.0f) view_high += 1.0f; set_view(); view_moved = 1; break;
	case MWKEY_DOWN: if (view_high > 2.0f) view_high -= 1.0f; set_view(); view_moved = 1; break;
	}
	return 0;
}

/* The button went down at that pixel of the picture: the box under it is held by that point,
 * or with no box there the view is. */
static void
press(int px, int py)
{
	float way[3], local[3], distance;
	int buffer = (int)((sim.base + phase_least()) % PHYS_BUFFERS), body, k;

	ray(px, py, way);
	body = phys_pick(&sim.w, buffer, eye, way, local, &distance);
	last_x = px, last_y = py;
	holding = 1;
	if (body < 0) {
		if (talk)
			fprintf(stderr, "nxphys: no box at %d, %d: the view is held\n", px, py);
		return;
	}
	held_far = distance * (way[0] * ahead[0] + way[1] * ahead[1] + way[2] * ahead[2]);
	for (k = 0; k < 3; k++)
		sim.w.grab_at[k] = local[k], sim.w.grab_to[k] = eye[k] + way[k] * distance;
	sim.w.grab = body;
	if (talk)
		fprintf(stderr, "nxphys: box %d held, %d mm away\n", body, (int)(distance * 1000.0f));
}

/* While the button is down: the held box's point goes where the pointer is, as far from the
 * eye as it was taken; or the view turns. The pointer is the machine's words (docs/input.md). */
static void
pointer(void)
{
	int px = (int)*control(0x20) - origin_x, py = (int)*control(0x24) - origin_y, k;
	float way[3], far;

	if (!holding || held_for_good)
		return;
	if (!(*control(0x28) & 1)) {
		if (sim.w.grab >= 0 && talk)
			fprintf(stderr, "nxphys: box %d let go\n", sim.w.grab);
		sim.w.grab = -1;
		holding = 0;
		return;
	}
	if (px == last_x && py == last_y)
		return;
	if (sim.w.grab >= 0) {
		ray(px, py, way);
		far = held_far / (way[0] * ahead[0] + way[1] * ahead[1] + way[2] * ahead[2]);
		for (k = 0; k < 3; k++) {
			float to = eye[k] + way[k] * far, most = k == 1 ? 2.0f * ROOM_HIGH - 1.0f : ROOM - 0.5f;

			sim.w.grab_to[k] = to > most ? most : to < (k == 1 ? 0.3f : -most) ? (k == 1 ? 0.3f : -most) : to;
		}
	} else {
		view_turn -= 0.01f * (float)(px - last_x);
		view_high += 0.05f * (float)(py - last_y);
		if (view_high > 20.0f) view_high = 20.0f;
		if (view_high < 2.0f) view_high = 2.0f;
		set_view();
		view_moved = 1;
	}
	last_x = px, last_y = py;
}

/* Keys and the button. What act() said, of the last thing that said anything. */
static int
events(void)
{
	GR_EVENT e;
	int again = 0, k;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return again;
		ui_origin_note(&e);	/* (the window's place is remembered: a click says it, an update forgets it) */
		if (e.type == GR_EVENT_TYPE_CLOSE_REQ) {
			sim.stop = 1;
			exit(0);
		}
		if (e.type == GR_EVENT_TYPE_EXPOSURE && e.exposure.wid == window)
			draw_bar();
		if (e.type == GR_EVENT_TYPE_KEY_DOWN && (k = act(e.keystroke.ch)) != 0)
			again = k;
		if (e.type == GR_EVENT_TYPE_UPDATE && e.update.utype == GR_UPDATE_SIZE && e.update.wid == window) {
			set_size(e.update.width, e.update.height);
			draw_bar();
		}
		if (ui_wheel(&e)) {
			zoom(ui_wheel(&e) > 0);
			continue;
		}
		if (e.type != GR_EVENT_TYPE_BUTTON_DOWN || !(e.button.changebuttons & GR_BUTTON_L))
			continue;
		if (e.button.y < bar_y) {
			/* (where the pointer is now, not where the event says it was: the box is taken
			 * under the hand; and the window where this press says it is: the picture is its
			 * top left corner) */
			ui_window_origin(window, &origin_x, &origin_y);
			press((int)*control(0x20) - origin_x, (int)*control(0x24) - origin_y);
		} else if (e.button.y >= bar_y + 5 && e.button.y < bar_y + 27 && e.button.x >= 5 && (e.button.x - 5) % button_step < button_step - 4 &&
			   (e.button.x - 5) / button_step < BUTTONS) {
			if ((k = act(buttons[(e.button.x - 5) / button_step].key)) != 0)
				again = k;
		}
	}
}

/* A test's last lines: a sum of every body's state, and where and how fast the bodies are. */
static void
say_done(unsigned took, int print)
{
	int buffer = (int)((sim.base + sim.limit) % PHYS_BUFFERS), trouble = 0, i, k, s;
	const uint32_t *words = (const uint32_t *)sim.w.states[buffer];
	uint32_t sum = 0;
	float deepest, fastest, top = 0.0f;

	for (i = 0; i < sim.w.bodies; i++) {
		/* (not the key: it names a share, and the sum is the same for any number of them) */
		for (k = 0; k < 16; k++)
			if (k != 3)
				sum = sum * 31 + words[16 * i + k];
		if (sim.w.states[buffer][i].pos[1] > top)
			top = sim.w.states[buffer][i].pos[1];
		if (print) {
			const struct body_state *b = &sim.w.states[buffer][i];

			/* (its turn in ten-thousandths, its half sides in millimetres, its spin in thousandths a second) */
			fprintf(stderr, "nxphys| %d: at %d %d %d going %d %d %d turned %d %d %d %d half %d %d %d spinning %d %d %d\n", i, (int)(b->pos[0] * 1000.0f),
				(int)(b->pos[1] * 1000.0f), (int)(b->pos[2] * 1000.0f), (int)(b->vel[0] * 1000.0f), (int)(b->vel[1] * 1000.0f), (int)(b->vel[2] * 1000.0f),
				(int)(b->q[0] * 10000.0f), (int)(b->q[1] * 10000.0f), (int)(b->q[2] * 10000.0f), (int)(b->q[3] * 10000.0f),
				(int)(sim.w.shapes[i].half[0] * 1000.0f), (int)(sim.w.shapes[i].half[1] * 1000.0f), (int)(sim.w.shapes[i].half[2] * 1000.0f),
				(int)(b->spin[0] * 1000.0f), (int)(b->spin[1] * 1000.0f), (int)(b->spin[2] * 1000.0f));
		}
	}
	for (s = 0; s < sim.nshares; s++)
		trouble += (int)sim.shares[s].trouble;
	phys_figures(&sim.w, buffer, &deepest, &fastest);
	if (getenv("PHYS_QSUM") && shown_buffer >= 0) {
		/* (a test's: the last picture's quads as the GPU has them, summed) */
		uint32_t quads = 0;

		for (i = 0; i < 16 * (FLOOR_QUADS + 6 * sim.w.bodies); i++)
			quads = quads * 31 + sim.quads[shown_buffer][i];
		fprintf(stderr, "physstat: quads %08x\n", (unsigned)quads);
	}
	if (sim.w.grab >= 0) {
		float at[3];

		phys_point(&sim.w, buffer, sim.w.grab, sim.w.grab_at, at);
		fprintf(stderr, "nxphys: box %d's held point is %d mm from where it is wanted\n", sim.w.grab, (int)(1000.0f * sqrtf(
			(at[0] - sim.w.grab_to[0]) * (at[0] - sim.w.grab_to[0]) + (at[1] - sim.w.grab_to[1]) * (at[1] - sim.w.grab_to[1]) +
			(at[2] - sim.w.grab_to[2]) * (at[2] - sim.w.grab_to[2]))));
	}
	if (sim.count) {
		extern unsigned mcw_faults;	/* (pages a worker stopped at, each brought by this core) */

		fprintf(stderr, "physstat: the first core ended %u passes with nothing to do, in %u substeps; %u pages brought for workers\n",
			(unsigned)(waited - waited_before), (unsigned)sim.limit, mcw_faults);
	}
	if (sim.count && sim.costs) {
		/* what the time is made of: a substep is as long as its dearest share */
		uint32_t dearest[2] = {0, 0}, step;

		for (step = 0; step < sim.limit && step < sim.cost_row; step++) {
			uint32_t most = 0;

			for (s = 0; s < sim.nshares; s++)
				if (sim.costs[(uint32_t)s * sim.cost_row + step] > most)
					most = sim.costs[(uint32_t)s * sim.cost_row + step];
			dearest[step % PHYS_EVERY != 0] += most;
		}
		fprintf(stderr, "physstat: the dearest share's instructions, substep by substep: %u in all with a look, %u without; %d shares have the bodies, %d the grid and the pictures\n",
			(unsigned)dearest[0], (unsigned)dearest[1], sim.nbody, sim.nservice);
		for (k = 0; k < 2; k++)
			fprintf(stderr, "physstat: substeps %s a look by the passes they took, 0 to 7 and over: %u %u %u %u %u %u %u %u\n", k ? "without" : "with",
				(unsigned)passes_of[k][0], (unsigned)passes_of[k][1], (unsigned)passes_of[k][2], (unsigned)passes_of[k][3], (unsigned)passes_of[k][4],
				(unsigned)passes_of[k][5], (unsigned)passes_of[k][6], (unsigned)passes_of[k][7]);
	}
	if (sim.count) {
		uint32_t spent[3] = {0, 0, 0}, most = 0, looks = (sim.limit + PHYS_EVERY - 1) / PHYS_EVERY, plain = sim.limit - looks;

		for (s = 0; s < sim.nshares; s++)
			for (k = 0; k < 3; k++)
				spent[k] += sim.shares[s].spent[k];
		for (s = 0; s < sim.nshares; s++)
			if (sim.shares[s].most > most)
				most = sim.shares[s].most;
		fprintf(stderr, "physstat: a box costs %u instructions a grid, %u a substep with a look for contacts, %u a substep without (a share's longest: %u)\n",
			spent[0] / (looks ? looks : 1) / (uint32_t)sim.w.bodies, spent[1] / (looks ? looks : 1) / (uint32_t)sim.w.bodies,
			spent[2] / (plain ? plain : 1) / (uint32_t)sim.w.bodies, most);
	}
	fprintf(stderr, "physstat: done: %d boxes, %u substeps in %u ms, %d shares on %d workers: highest %d mm, deepest %d mm, fastest %d mm/s, "
		"%d without room, sum %08x\n", sim.w.bodies, (unsigned)sim.limit, took, sim.nshares, workers, (int)(top * 1000.0f),
		(int)(deepest * 1000.0f), (int)(fastest * 1000.0f), trouble, (unsigned)sum);
}

int
main(void)
{
	const char *text;
	GR_SCREEN_INFO info;
	uint32_t *table, steps = 0;
	int width = 640, height = 400, hold, print, frames = 0, asked = 0, cells, k;
	int pick_x = -1, pick_y = -1;
	uint32_t input = 0;
	unsigned hot_at = 0, drawn_at = 0;
	unsigned began, said_at, keys_at = 0, jobs_at = 0, asked_at = 0, retry_at = 0, made_said = 0, paced_at = 0;

	if ((text = getenv("PHYS_BODIES")) != NULL)
		bodies_wanted = atoi(text);
	if (bodies_wanted < 2 || bodies_wanted > MAX_BODIES)
		bodies_wanted = 400;
	if ((text = getenv("PHYS_SCENE")) != NULL && atoi(text) >= 1 && atoi(text) <= 4)
		scene = atoi(text);
	if ((text = getenv("PHYS_WORKERS")) != NULL)
		wanted = atoi(text);
	if ((text = getenv("PHYS_STEPS")) != NULL)
		steps = (uint32_t)atoi(text);
	sim.limit = steps ? steps : ~0u;
	hold = getenv("PHYS_HOLD") != NULL;
	print = getenv("PHYS_PRINT") != NULL;
	sim.count = getenv("PHYS_COUNT") != NULL;
	sim.spin = getenv("PHYS_SPIN") != NULL;
	sim.ahead = (text = getenv("PHYS_ORDER")) != NULL && !strcmp(text, "ahead");
	/* a contact found by a look has to do until the last substep before the next look's is
	 * made: two substeps and one to spare, or with the look one ahead three and none */
	sim.w.look_ahead = (PHYS_EVERY + 1) * PHYS_DT;
	sim.owners = getenv("PHYS_OWNERS") != NULL;
	if ((text = getenv("PHYS_PICK")) != NULL)
		sscanf(text, "%d,%d", &pick_x, &pick_y);
	talk = getenv("PHYS_SAY") != NULL || pick_x >= 0;
	sim.w.grab = -1;
	if (GrOpen() < 0) {
		fprintf(stderr, "nxphys: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	GrGetScreenInfo(&info);
	if (width > info.cols - 20) width = (info.cols - 20) & ~3;
	if ((text = getenv("PHYS_SIZE")) != NULL)
		sscanf(text, "%dx%d", &width, &height);
	if (width < 160 || height < 100) width = 640, height = 400;
	if (height > info.rows - 60 - BAR) height = info.rows - 60 - BAR;
	ui_init();
	bar_y = height, wide = width;
	button_step = (wide - 10) / BUTTONS;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Physics", GR_ROOT_WINDOW_ID, -1, -1, width, height + BAR, UI_FACE);
	/* (not the pointer's moves: while the button is down its place is read from the machine's words) */
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	picture = GrNewWindow(window, 0, 0, width, height, 0, BLACK, BLACK);
	GrMapWindow(picture);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	draw_bar();
	if (seglInit(picture) < 0) {
		fprintf(stderr, "nxphys: this machine has no GPU\n");
		return 1;
	}
	sim.w.half_x = sim.w.half_z = ROOM;
	sim.w.gravity = 9.81f;
	sim.w.gx = sim.w.gz = (int)(2.0f * ROOM / PHYS_CELL);
	sim.w.gy = ROOM_HIGH;
	sim.w.cell_shares = 1;
	cells = sim.w.gx * sim.w.gy * sim.w.gz;
	total_quads = FLOOR_QUADS + 6 * MAX_BODIES;
	for (k = 0; k < PHYS_BUFFERS; k++) {
		sim.w.states[k] = mcw_alloc(MAX_BODIES * sizeof(struct body_state));
		sim.w.poses[k] = mcw_alloc(MAX_BODIES * sizeof(struct body_pose));
	}
	sim.w.shapes = mcw_alloc(MAX_BODIES * sizeof(struct body_shape));
	sim.w.info = mcw_alloc(MAX_BODIES * sizeof(struct body_info));
	sim.w.manifolds = mcw_alloc(MAX_BODIES * PHYS_MANIFOLDS * sizeof(struct manifold));
	sim.w.grids[0] = mcw_alloc((unsigned)cells * sizeof(struct gcell));
	sim.w.grids[1] = mcw_alloc((unsigned)cells * sizeof(struct gcell));
	sim.shares = mcw_alloc(MC_MAX_CORES * sizeof(struct share));
	sim.colours = mcw_alloc(MAX_BODIES);
	sim.quads[0] = seglMemory((unsigned)total_quads * 64);
	sim.quads[1] = seglMemory((unsigned)total_quads * 64);
	table = seglMemory(COLOURS * SHADES * 4);
	if (!sim.w.states[0] || !sim.w.states[1] || !sim.w.states[2] || !sim.w.poses[0] || !sim.w.poses[1] || !sim.w.poses[2] || !sim.w.shapes || !sim.w.info || !sim.w.manifolds ||
	    !sim.w.grids[0] || !sim.w.grids[1] || !sim.shares || !sim.colours ||
	    !sim.quads[0] || !sim.quads[1] || !table || seglKeep(1, total_quads) < 0) {
		fprintf(stderr, "nxphys: no memory\n");
		return 1;
	}
	/* (the bodies' own memory when a scene says how many: bodies_there(); a picture's quads
	 * are written before they are drawn, and none past the bodies is) */
	there(sim.w.grids[0], (unsigned)cells * sizeof(struct gcell));
	there(sim.w.grids[1], (unsigned)cells * sizeof(struct gcell));
	there(sim.shares, MC_MAX_CORES * sizeof(struct share));
	if (sim.count && steps && steps <= 4096) {
		/* (a test's: every share's every substep, a row of whole texels a share) */
		sim.cost_row = (steps + 3) & ~3u;
		sim.costs = mcw_alloc(MC_MAX_CORES * sim.cost_row * 4);
		if (sim.costs)
			there(sim.costs, MC_MAX_CORES * sim.cost_row * 4);
	}
	make_table(table);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	seglTexturePointer(table, SHADES, COLOURS, GL_RGBA);
	seglWorldUp(2);
	set_view();

	atexit(mcw_close);
	set_scene();
	take_workers();
	fprintf(stderr, "nxphys: %d boxes, %d shares on %d workers\n", sim.w.bodies, sim.nshares, workers);
	start_jobs();
	began = said_at = now_ms();
	waited_before = noted_at = waited;
	if (pick_x >= 0) {
		/* a test: the box at that pixel held from the start, its point wanted three metres up */
		press(pick_x, pick_y);
		sim.w.grab_to[1] += 3.0f;
		held_for_good = 1;
	}
	for (;;) {
		unsigned now = now_ms();
		int did = stopped ? 0 : own_shares(), again = 0;

		if (sim.costs)
			note_substeps();

		/* as fast as time goes and no faster; what was not made in its time is not owed */
		if (!steps && !stopped) {
			unsigned due = (now - paced_at) * PHYS_HZ / 1000u;

			if (sim.limit == ~0u) {
				sim.limit = phase_least();
				paced_at = now;
			} else if (due) {
				uint32_t least = phase_least(), limit = sim.limit + due;

				sim.limit = limit > least + 4 ? least + 4 : limit;
				paced_at += due * 1000u / PHYS_HZ;
			}
		}
		if (asked && picture_made()) {
			show_picture(0);
			frames++;
			asked = 0;
		}
		/* a picture is asked for when the last has been shown, and no more than 25 a second */
		if (!asked && !stopped && now - asked_at >= 40 && phase_least() < sim.limit) {
			ask_picture();
			asked = 1;
			asked_at = now;
		}
		/* (the window system is asked when the button or key words change and for a while
		 * after, the event being on its way; else a few times a second: docs/input.md) */
		if (input != (*control(0x28) ^ *control(0x2c) << 8)) {
			input = *control(0x28) ^ *control(0x2c) << 8;
			hot_at = now;
		}
		if (now - keys_at >= 150 || now - hot_at < 120) {
			int was = stopped;

			keys_at = now;
			again = events();
			if (stopped != was && !again) {
				if (stopped)
					settle();
				else
					start_jobs(), paced_at = now;
			}
			if (stopped != was || again)
				draw_bar();
		}
		pointer();
		if (view_moved && now - drawn_at >= 30) {
			show_picture(1);
			drawn_at = now;
		}
		if (now - jobs_at >= 50) {
			jobs_at = now;
			look_at_jobs();
		}
		/* a worker that lost its core is asked for again now and then */
		if (!stopped && now - retry_at >= 1000) {
			retry_at = now;
			start_jobs();
		}
		if (again) {
			settle();
			begin_again();
			sim.w.grab = -1;
			if (again == 1)
				shown_buffer = -1;
			if (again == 1)
				set_scene();
			if (again == 3)
				throw_box((int)(sim.base % PHYS_BUFFERS));
			if (again == 2)
				take_workers();
			else if (again == 1)
				share_out();
			else
				set_shares(sim.nshares);
			draw_bar();
			asked = 0;
			stopped = 0;
			if (!steps)
				sim.limit = ~0u;
			start_jobs();
			began = said_at = now_ms();
			made_said = 0;
			frames = 0;
			continue;
		}
		if (steps && !asked && sim.shares[0].phase >= sim.limit && phase_least() >= sim.limit) {
			/* a test: so many substeps, the last picture, and what the boxes come to */
			unsigned took = now_ms() - began;

			ask_picture();
			while (!picture_made()) {
				look_at_jobs();
				if (!own_shares())
					pass();
			}
			show_picture(0);
			settle();
			say_done(took, print);
			if (!hold)
				return 0;
			fprintf(stderr, "nxphys: holding\n");
			steps = 0;
			stopped = 1;
		}
		if (now - said_at >= 1000 && !stopped) {
			uint32_t made = phase_least();
			unsigned span = now - said_at, rate = (made - made_said) * 1000u / span;

			snprintf(status, sizeof status, "%d boxes on %d workers: %u substeps a second, %u.%02u of real time, %u pictures a second", sim.w.bodies,
				workers, rate, rate / PHYS_HZ, rate % PHYS_HZ * 100 / PHYS_HZ, (unsigned)frames * 1000u / span);
			draw_bar();
			if (talk)
				fprintf(stderr, "physstat: %d boxes, %d shares on %d workers: %u substeps a second, %u pictures a second\n", sim.w.bodies, sim.nshares,
				workers, rate, (unsigned)frames * 1000u / span);
			said_at = now;
			made_said = made;
			frames = 0;
		}
		if (!did)
			pass();
	}
}
