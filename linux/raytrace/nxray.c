/*
 * nxray: a ray tracer in tiles, every worker core tracing its share of them
 * (docs/apps/raytrace.md, docs/machine/multicore.md). The work this machine's small cores are for: a pixel
 * is a few thousand instructions of float arithmetic and four bytes stored.
 *
 * Core 0 traces nothing. It makes each picture's scene and shares it out; every worker has a
 * job for all the pictures (trace_pictures) and goes from one to the next. The bar under the
 * picture has a button for each of these keys but the last:
 *   r   the next size of picture     m   how often a ray is mirrored (0 to 2)     h   shadows
 *   space  stop and go     w   each tile framed in its core's colour
 *   W   all the workers, one, none (a, 1, 0: each by itself)     q, Escape  leave
 * For tests: RAY_FRAMES=N leaves after N pictures, RAY_WORKERS=N sets the start,
 * RAY_SIZE=WxH the picture (320x240), RAY_STILL=1 keeps the scene where it is.
 * Each picture's line (raystat:) has its time, its rays and a sum of its pixels, which is the
 * same whichever cores traced it.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES/segl.h>
#include "ui.h"
#include "mcw.h"

#define ONE 65536
#define SPHERES 6
#define DEPTH 3
#define SHADE 16

struct vec { float x, y, z; };
struct sphere { struct vec centre; float radius, radius2; struct vec colour; float mirror; };

/* A picture's scene and camera, and how it is shared out: core 0 writes them before the
 * picture is published, the workers only read. Picture f is pictures[f & 1]: two are out at
 * once, and a worker goes from one to the next without the first core. */
static struct picture {
	struct sphere spheres[SPHERES];
	struct vec eye, right, up, forward, light;
	int width, height, owners;
	int depth, shadows;	/* how many times a ray turns at most (DEPTH), and whether a point asks if it is lit */
	uint32_t *pixels;
	/* A tile is 32 pixels square, or as much smaller as gives a worker two dozen of them (never
	 * narrower than eight pixels, which are two of the machine's texels). */
	int tile_w, tile_h, tiles;
	uint32_t stride, step;	/* the order the tiles are traced in: below */
	int shares;		/* how many shares the picture is in: one a worker that has a core */
	uint32_t jobs;		/* which workers had jobs out when it was shared out (jobs_now then) */
	int traced_by;		/* how many workers trace it: none, and the first core does */
	/* Worker k's share (NO_SHARE: none), how many of its last tiles another traces, and whose last tiles it traces after its own (and how many). */
	struct { uint16_t share, given, helps, takes; } plan[MC_MAX_CORES];
	/* For the eye's own rays: a sphere as the eye has it, and the pixels it can be in. */
	struct { struct vec to; float to2; int x0, x1, y0, y1; } seen[SPHERES];
	/* And the pixels its picture in the ground can be in: it as the eye would see it under the ground. */
	struct { int x0, x1, y0, y1; } mirrored[SPHERES];
	/* For the ground's shadows: the spheres whose shadow a square of the ground may be in,
	 * a bit each. The squares are SHADE each way from shade_x, shade_z, shade_by to a unit. */
	float shade_x, shade_z, shade_by;
	uint8_t shade[SHADE * SHADE];
} pictures[2] __attribute__((aligned(16)));
#define NO_SHARE 0xffff

/* What the workers go by, core 0's: the last picture published, the last one shown (the
 * picture two after it has its pixels), a picture given up (a share ends at its next tile),
 * the jobs to end, and RAY_SPIN (a worker with no picture to trace turns in place). */
static struct { volatile uint32_t frame, shown; volatile int stop, leave; int spin, unused[3]; } go __attribute__((aligned(16)));

static inline struct vec v(float x, float y, float z) { struct vec r = {x, y, z}; return r; }
static inline struct vec add(struct vec a, struct vec b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline struct vec sub(struct vec a, struct vec b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline struct vec scale(struct vec a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline struct vec unit(struct vec a) { return scale(a, 1.0f / __builtin_sqrtf(dot(a, a))); }

/* The nearest thing a ray meets: a sphere's number, SPHERES for the ground, -1 for the sky. */
static int
nearest(const struct picture *sc, struct vec from, struct vec way, float *distance)
{
	float best = 1e30f;
	int what = -1, i;

	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &sc->spheres[i];
		struct vec to = sub(s->centre, from);
		float along = dot(to, way), off2 = dot(to, to) - along * along;

		if (along > 0.0f && off2 < s->radius2) {
			float t = along - __builtin_sqrtf(s->radius2 - off2);

			if (t > 0.001f && t < best)
				best = t, what = i;
		}
	}
	if (way.y < -0.0001f) {
		float t = -from.y / way.y;

		if (t > 0.001f && t < best)
			best = t, what = SPHERES;
	}
	*distance = best;
	return what;
}

/* Whether anything stands between a point and the light. */
static int
shadowed(const struct picture *sc, struct vec from, struct vec way)
{
	int i;

	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &sc->spheres[i];
		struct vec to = sub(s->centre, from);
		float along = dot(to, way);

		if (along > 0.0f && dot(to, to) - along * along < s->radius2)
			return 1;
	}
	return 0;
}

/* nearest() for a ray from the eye through pixel x: only the row's spheres (row, rows) that
 * can be in that pixel are tried, with what sc->seen has of each. The same arithmetic. */
static inline int
nearest_seen(const struct picture *sc, struct vec way, int x, const uint8_t *row, int rows, float *distance)
{
	float best = 1e30f;
	int what = -1, k;

	for (k = 0; k < rows; k++) {
		int i = row[k];
		float along, off2, radius2 = sc->spheres[i].radius2;

		if (x < sc->seen[i].x0 || x > sc->seen[i].x1)
			continue;
		along = dot(sc->seen[i].to, way), off2 = sc->seen[i].to2 - along * along;
		if (along > 0.0f && off2 < radius2) {
			float t = along - __builtin_sqrtf(radius2 - off2);

			if (t > 0.001f && t < best)
				best = t, what = i;
		}
	}
	if (way.y < -0.0001f) {
		float t = -sc->eye.y / way.y;

		if (t > 0.001f && t < best)
			best = t, what = SPHERES;
	}
	*distance = best;
	return what;
}

/* nearest() for the eye's ray through pixel x thrown back by the ground: only the row's
 * spheres (row, rows) whose picture in the ground can be in that pixel are tried. */
static inline int
nearest_mirrored(const struct picture *sc, struct vec from, struct vec way, int x, const uint8_t *row, int rows, float *distance)
{
	float best = 1e30f;
	int what = -1, k;

	for (k = 0; k < rows; k++) {
		int i = row[k];
		const struct sphere *s = &sc->spheres[i];
		struct vec to;
		float along, off2;

		if (x < sc->mirrored[i].x0 || x > sc->mirrored[i].x1)
			continue;
		to = sub(s->centre, from);
		along = dot(to, way), off2 = dot(to, to) - along * along;
		if (along > 0.0f && off2 < s->radius2) {
			float t = along - __builtin_sqrtf(s->radius2 - off2);

			if (t > 0.001f && t < best)
				best = t, what = i;
		}
	}
	*distance = best;
	return what;
}

/* shadowed() for a point of the ground and the scene's light: only the spheres whose shadow
 * its square of the ground can be in are tried. */
static inline int
shadowed_ground(const struct picture *sc, struct vec from)
{
	float gx = (from.x - sc->shade_x) * sc->shade_by, gz = (from.z - sc->shade_z) * sc->shade_by;
	unsigned mask;
	int i;

	if (!(gx >= 0.0f && gx < (float)SHADE && gz >= 0.0f && gz < (float)SHADE))
		return 0;
	for (i = 0, mask = sc->shade[(int)gz * SHADE + (int)gx]; mask; i++, mask >>= 1)
		if (mask & 1) {
			const struct sphere *s = &sc->spheres[i];
			struct vec to = sub(s->centre, from);
			float along = dot(to, sc->light);

			if (along > 0.0f && dot(to, to) - along * along < s->radius2)
				return 1;
		}
	return 0;
}

/* The colour the eye's ray through pixel x brings back; *rays counts every ray cast for it.
 * row, rows: the spheres nearest_seen() tries, and after them those nearest_mirrored() does. */
static inline struct vec
trace(const struct picture *sc, struct vec from, struct vec way, uint32_t *rays, int x, const uint8_t *row, int rows, int rows_mirrored)
{
	struct vec seen = v(0, 0, 0);
	float weight = 1.0f;
	int bounce, ground = 0;

	for (bounce = 0; bounce < sc->depth; bounce++) {
		struct vec at, normal, colour;
		float distance, mirror, lit;
		int what = bounce == 0 ? nearest_seen(sc, way, x, row, rows, &distance)
		           : ground ? nearest_mirrored(sc, from, way, x, row + rows, rows_mirrored, &distance) : nearest(sc, from, way, &distance);

		(*rays)++;
		if (what < 0) {
			/* the sky: paler towards the horizon */
			float high = way.y > 0.0f ? way.y : 0.0f;

			seen = add(seen, scale(v(0.75f - 0.45f * high, 0.85f - 0.35f * high, 1.0f), weight));
			break;
		}
		at = add(from, scale(way, distance));
		ground = bounce == 0 && what == SPHERES;
		if (what == SPHERES) {
			/* the ground: squares of two greys */
			int cx = (int)(at.x + 1000.0f), cz = (int)(at.z + 1000.0f);

			normal = v(0, 1, 0);
			colour = (cx ^ cz) & 1 ? v(0.85f, 0.85f, 0.8f) : v(0.25f, 0.27f, 0.3f);
			mirror = 0.25f;
		} else {
			const struct sphere *s = &sc->spheres[what];

			normal = scale(sub(at, s->centre), 1.0f / s->radius);
			colour = s->colour;
			mirror = s->mirror;
		}
		lit = dot(normal, sc->light);
		*rays += (uint32_t)sc->shadows;
		if (lit < 0.0f || (sc->shadows && (what == SPHERES ? shadowed_ground(sc, at) : shadowed(sc, at, sc->light))))
			lit = 0.0f;
		seen = add(seen, scale(colour, weight * (1.0f - mirror) * (0.2f + 0.8f * lit)));
		weight *= mirror;
		if (weight < 0.03f)
			break;
		way = sub(way, scale(normal, 2.0f * dot(way, normal)));
		from = at;
	}
	return seen;
}

static inline uint32_t
byte_of(float c)
{
	/* (0 below none and 255 above one, as two instructions) */
	return (uint32_t)(__builtin_fminf(__builtin_fmaxf(c, 0.0f), 1.0f) * 255.0f);
}

static const uint32_t owner_colour[16] = {
	0xffffff, 0xff4040, 0x40ff40, 0x4080ff, 0xffff40, 0xff40ff, 0x40ffff, 0xff8000,
	0x80ff00, 0x0080ff, 0xff0080, 0x8000ff, 0x00ff80, 0xc0c0c0, 0xff8080, 0x80ff80,
};

/*
 * One tile. Its rows of the picture begin and end on 16 bytes (a tile is a multiple of eight
 * pixels of four bytes across, and the picture's width is a multiple of four), so no two
 * cores store to the same 16 bytes; everything else it writes is its own stack.
 */
static uint32_t
trace_tile(const struct picture *sc, uint32_t tile, uint32_t core)
{
	int tw = sc->tile_w, th = sc->tile_h;
	int across = (sc->width + tw - 1) / tw, x0 = (int)(tile % (uint32_t)across) * tw, y0 = (int)(tile / (uint32_t)across) * th;
	int x1 = x0 + tw < sc->width ? x0 + tw : sc->width, y1 = y0 + th < sc->height ? y0 + th : sc->height, x, y;
	float half_w = (float)sc->width * 0.5f, half_h = (float)sc->height * 0.5f, zoom = 1.0f / half_h;
	uint32_t rays = 0;

	for (y = y0; y < y1; y++) {
		uint32_t *out = sc->pixels + y * sc->width + x0;
		uint8_t row[2 * SPHERES];
		int rows = 0, rows_mirrored = 0, i;

		/* the spheres that can be in these pixels of the row, in their order, then those
		 * whose pictures in the ground can */
		for (i = 0; i < SPHERES; i++)
			if (y >= sc->seen[i].y0 && y <= sc->seen[i].y1 && x1 > sc->seen[i].x0 && x0 <= sc->seen[i].x1)
				row[rows++] = (uint8_t)i;
		for (i = 0; i < SPHERES; i++)
			if (y >= sc->mirrored[i].y0 && y <= sc->mirrored[i].y1 && x1 > sc->mirrored[i].x0 && x0 <= sc->mirrored[i].x1)
				row[rows + rows_mirrored++] = (uint8_t)i;
		for (x = x0; x < x1; x++) {
			float sx = ((float)x + 0.5f - half_w) * zoom, sy = (half_h - (float)y - 0.5f) * zoom;
			struct vec way = unit(add(add(scale(sc->right, sx), scale(sc->up, sy)), scale(sc->forward, 1.6f)));
			struct vec c = trace(sc, sc->eye, way, &rays, x, row, rows, rows_mirrored);

			if (sc->owners && (x == x0 || y == y0))
				*out++ = owner_colour[core & 15];
			else
				*out++ = byte_of(c.x) << 16 | byte_of(c.y) << 8 | byte_of(c.z);
		}
	}
	return rays;
}

/* How far a worker is with picture f and what the picture took it, in instructions: its own
 * tiles, and those of another's share. slots[f & 1][k]: its own 16 bytes. */
struct slot { volatile uint32_t frame, rays, spent, lent; };
static struct slot slots[2][MC_MAX_CORES] __attribute__((aligned(16)));

static inline uint32_t
cycles(void)
{
	uint32_t count;

	__asm__ volatile("rdcycle %0" : "=r"(count));
	return count;
}

/* The tiles are taken in an order that scatters them, tile j being number j * stride of the
 * picture, and share w of n has every n-th of them from the w-th: as many as any other,
 * from all over the picture. How many that is. */
static inline uint32_t
share_length(const struct picture *sc, uint32_t share)
{
	uint32_t tiles = (uint32_t)sc->tiles, n = (uint32_t)sc->shares;

	return share < tiles ? (tiles - share + n - 1) / n : 0;
}

/* A share's tiles `from` to `to` of its own, traced. */
static uint32_t
share_tiles(const struct picture *sc, uint32_t share, uint32_t from, uint32_t to, uint32_t core)
{
	uint32_t tiles = (uint32_t)sc->tiles, rays = 0;
	uint32_t at = (uint32_t)((uint64_t)(share + from * (uint32_t)sc->shares) * sc->stride % tiles);

	for (; from < to && !go.stop; from++) {
		rays += trace_tile(sc, at, core);
		if ((at += sc->step) >= tiles)
			at -= tiles;
	}
	return rays;
}

/* Worker k's part of picture `frame`: its share but the tiles given to another, then what it
 * was given of another's (the picture's plan). */
static void
worker_picture(const struct picture *sc, uint32_t frame, uint32_t k)
{
	struct slot *slot = &slots[frame & 1][k];
	uint32_t share = sc->plan[k].share, length = share_length(sc, share), given = sc->plan[k].given, takes = sc->plan[k].takes;
	uint32_t began = cycles(), between, rays;

	rays = share_tiles(sc, share, 0, length - given, k);
	between = cycles();
	if (takes) {
		uint32_t theirs = share_length(sc, sc->plan[k].helps);

		rays += share_tiles(sc, sc->plan[k].helps, theirs - takes, theirs, k);
	}
	slot->spent = between - began;
	slot->lent = cycles() - between;
	slot->rays = rays;
	slot->frame = frame;
}

/* A worker's job: its part of every picture after `seen`, for as long as pictures are traced.
 * Handed a share a picture, the 255 jobs took the first core four passes to give out and one
 * to take back, of a picture's ten. (Run by the first core, it does nothing: that core then
 * traces the part itself.) */
static uint32_t
trace_pictures(uint32_t k, uint32_t seen)
{
	if (!mcw_on_worker())
		return 0;
	while (!go.leave && !mcw_recalled((int)k)) {
		/* the next picture, once it is published and the one two before it, whose pixels it has, is shown */
		if (go.frame == seen || go.shown + 1 < seen) {
			/* (its pass ends here: turning in place cost the machine the pass's instructions) */
			if (!go.spin)
				mc_end_pass();
			continue;
		}
		seen++;
		worker_picture(&pictures[seen & 1], seen, k);
	}
	return 1;
}

/* ---- core 0 ---- */

#define BAR 54		/* the bar under the picture: a row of buttons and a line of figures */
#define BUTTONS 6
#define SIZES 5
static const int sizes[SIZES][2] = {{160, 120}, {240, 180}, {320, 240}, {480, 360}, {640, 480}};
static const int button_keys[BUTTONS] = {'r', 'm', 'h', ' ', 'w', 'W'};
static GR_WINDOW_ID window, picture;
static GLuint texture[2];
static int bar_y, wide, button_step = 90;
static char status[120] = "";
static int width = 320, height = 240, room;	/* the picture, and how many pixels its memory holds */
static int depth = DEPTH, shadows = 1;
static int workers, wanted = MC_MAX_CORES, stopped, frames_left = -1, still, owners, even;
static uint8_t running[MC_MAX_CORES];	/* worker k has its job out */
static uint32_t jobs_now = 1;		/* goes up when a job begins or ends: a picture shared out before has other shares */

static unsigned
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

/* A job for every worker that has a core and none out: it begins with the next picture published. */
static void
start_jobs(void)
{
	int k;

	for (k = 1; k <= workers; k++) {
		if (running[k])
			continue;
		mcw_done(k);	/* (a worker without a core is asked about: the library asks for one again) */
		if (!mcw_bound(k))
			continue;
		running[k] = 1, jobs_now++;
		mcw_post(k, trace_pictures, (uint32_t)k, go.frame);
	}
}

/* The jobs that have ended (a core the kernel wanted back): what such a worker had of the
 * pictures after `done` and did not trace is traced here. */
static void
look_at_jobs(uint32_t done)
{
	uint32_t f;
	int k;

	for (k = 1; k <= workers; k++)
		if (running[k] && mcw_done(k)) {
			running[k] = 0, jobs_now++;
			for (f = done + 1; f - done <= go.frame - done; f++)
				if (pictures[f & 1].plan[k].share != NO_SHARE && slots[f & 1][k].frame != f)
					worker_picture(&pictures[f & 1], f, (uint32_t)k);
		}
}

/* Whether every worker's part of picture f is in (*from: the first worker not known to be through). */
static int
all_in(uint32_t f, int *from)
{
	const struct picture *sc = &pictures[f & 1];

	for (; *from <= workers; (*from)++)
		if (sc->plan[*from].share != NO_SHARE && slots[f & 1][*from].frame != f)
			return 0;
	return 1;
}

static void
end_jobs(void)
{
	int k;

	go.stop = go.leave = 1;
	for (k = 1; k <= workers; k++)
		if (running[k]) {
			mcw_wait(k);
			running[k] = 0;
		}
	go.leave = 0;
	jobs_now++;
}

/* The workers given back, and as many taken as wanted. */
static void
take_workers(void)
{
	extern char __DATA_BEGIN__[], _end[], __executable_start[], etext[];
	const volatile char *code;

	end_jobs();
	go.stop = 0;
	mcw_close();
	workers = wanted > 0 ? mcw_open(wanted) : 0;
	if (!workers)
		return;
	mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	/* (and the program's code: a page of it that no core has run or read stops every worker that comes to it) */
	for (code = __executable_start; code < etext; code += 4096)
		(void)*code;
	go.spin = getenv("RAY_SPIN") != NULL;
	start_jobs();
}

/* A picture's shares: one for each worker that has a job out. How many. When they are the
 * workers picture `done` was shared among, one whose share took long there has some of its
 * last tiles traced by one whose share was short: the dearest with the cheapest and so on
 * inwards, each pair to end together if the tiles cost what they did. */
static int
give_shares(struct picture *sc, uint32_t done)
{
	static uint32_t cost[MC_MAX_CORES];
	static uint8_t worker_of[MC_MAX_CORES], order[MC_MAX_CORES];
	const struct picture *old = &pictures[done & 1];
	const struct slot *was = slots[done & 1];
	uint16_t starts[66];
	uint32_t least = ~0u, most = 0, total = 0, step;
	int n = 0, k, i, same = done != 0 && old->jobs == jobs_now && old->traced_by > 0;

	/* what each share of that picture took, whoever traced its tiles */
	for (k = 1; k <= workers && same; k++)
		if (old->plan[k].share != NO_SHARE)
			cost[old->plan[k].share] = was[k].spent, worker_of[old->plan[k].share] = (uint8_t)k;
	for (k = 1; k <= workers && same; k++)
		if (old->plan[k].share != NO_SHARE && old->plan[k].takes)
			cost[old->plan[k].helps] += was[k].lent;
	for (k = 1; k <= workers; k++) {
		uint16_t share = same ? old->plan[k].share : running[k] ? (uint16_t)n : NO_SHARE;

		n += share != NO_SHARE;
		if (sc->plan[k].share != share)
			sc->plan[k].share = share;
		if (sc->plan[k].given || sc->plan[k].takes)
			sc->plan[k].given = sc->plan[k].takes = 0;
	}
	sc->shares = n ? n : 1;
	sc->traced_by = n;
	sc->jobs = jobs_now;
	if (!same || even || !n)
		return n;
	for (i = 0; i < n; i++) {
		total += cost[i] >> 4;
		if (cost[i] < least) least = cost[i];
		if (cost[i] > most) most = cost[i];
	}
	/* (nothing to gain under a pass and a half between the dearest and the mean) */
	if (most - (total / (uint32_t)n << 4) < 12288)
		return n;
	/* the shares by cost, in 64 steps from the least to the most */
	step = (most - least) / 64 + 1;
	memset(starts, 0, sizeof starts);
	for (i = 0; i < n; i++)
		starts[(cost[i] - least) / step + 2]++;
	for (i = 2; i < 66; i++)
		starts[i] = (uint16_t)(starts[i] + starts[i - 1]);
	for (i = 0; i < n; i++)
		order[starts[(cost[i] - least) / step + 1]++] = (uint8_t)i;
	for (i = 0; i < n / 2; i++) {
		uint32_t cheap = order[i], dear = order[n - 1 - i], length = share_length(old, dear);	/* (as that picture was shared out: so is this one) */
		uint32_t give = (uint32_t)((uint64_t)(cost[dear] - cost[cheap]) * length / (2 * (uint64_t)cost[dear] + 1));

		if (give > length / 2)
			give = length / 2;
		sc->plan[worker_of[dear]].given = (uint16_t)give;
		sc->plan[worker_of[cheap]].helps = (uint16_t)dear;
		sc->plan[worker_of[cheap]].takes = (uint16_t)give;
	}
	return n;
}

static void
set_scene(struct picture *sc, float t)
{
	static const struct { float ring, height, radius, speed, phase, r, g, b, mirror; } made[SPHERES] = {
		{0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.9f, 0.9f, 0.95f, 0.75f},
		{2.3f, 0.6f, 0.6f, 0.9f, 0.0f, 0.95f, 0.25f, 0.2f, 0.3f},
		{2.3f, 0.6f, 0.6f, 0.9f, 2.1f, 0.2f, 0.8f, 0.3f, 0.3f},
		{2.3f, 0.6f, 0.6f, 0.9f, 4.2f, 0.25f, 0.4f, 0.95f, 0.3f},
		{3.8f, 0.4f, 0.4f, -0.6f, 1.0f, 0.95f, 0.8f, 0.2f, 0.5f},
		{3.8f, 0.4f, 0.4f, -0.6f, 4.1f, 0.8f, 0.3f, 0.9f, 0.5f},
	};
	struct vec target = v(0, 0.8f, 0);
	float turn = 0.25f * t;
	int i;

	for (i = 0; i < SPHERES; i++) {
		float a = made[i].speed * t + made[i].phase;

		sc->spheres[i].centre = v(made[i].ring * cosf(a), made[i].height + (i ? 0.25f * sinf(2.0f * a) + 0.25f : 0.0f), made[i].ring * sinf(a));
		sc->spheres[i].radius = made[i].radius;
		sc->spheres[i].radius2 = made[i].radius * made[i].radius;
		sc->spheres[i].colour = v(made[i].r, made[i].g, made[i].b);
		sc->spheres[i].mirror = made[i].mirror;
	}
	sc->eye = v(7.0f * cosf(turn), 2.6f, 7.0f * sinf(turn));
	sc->forward = unit(sub(target, sc->eye));
	sc->right = unit(v(-sc->forward.z, 0, sc->forward.x));
	sc->up = v(sc->right.y * sc->forward.z - sc->right.z * sc->forward.y, sc->right.z * sc->forward.x - sc->right.x * sc->forward.z,
		sc->right.x * sc->forward.y - sc->right.y * sc->forward.x);
	sc->light = unit(v(0.5f, 0.9f, -0.3f));
}

/* The pixels a sphere can be in along one way of the screen: between its two tangents from
 * the eye, a few pixels more. c, z: its centre across (or up) and ahead; per: pixels to a
 * unit of the screen, less than none where they run the other way. */
static void
span(float c, float z, float r, float per, float middle, int most, int *low, int *high)
{
	float den = z * z - r * r, root, a, b;

	*low = 0, *high = most;
	if (z < r * 1.01f + 0.01f)
		return;	/* (round the eye or behind it: anywhere) */
	root = r * __builtin_sqrtf(c * c + den);
	a = middle + per * 1.6f * (c * z - root) / den, b = middle + per * 1.6f * (c * z + root) / den;
	if (a > b) {
		float swap = a;

		a = b, b = swap;
	}
	*low = a > (float)most + 8.0f ? most + 1 : a > -8.0f ? (int)a - 4 : 0;
	*high = b < -8.0f ? -1 : b < (float)most + 8.0f ? (int)b + 4 : most;
}

/* What the eye's rays and the ground's shadows are spared (sc->seen, sc->shade): where
 * each sphere can be seen, and which squares of the ground its shadow can reach. */
static void
set_spared(struct picture *sc)
{
	float half_w = (float)sc->width * 0.5f, half_h = (float)sc->height * 0.5f, low_x = 1e30f, low_z = 1e30f, high_x = -1e30f, high_z = -1e30f;
	float by = 1.0f / sc->light.y, spot[SPHERES][3];
	int i, x, z;

	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &sc->spheres[i];
		struct vec to = sub(s->centre, sc->eye), under = sub(v(s->centre.x, -s->centre.y, s->centre.z), sc->eye);
		float ahead = dot(to, sc->forward), under_ahead = dot(under, sc->forward);

		sc->seen[i].to = to;
		sc->seen[i].to2 = dot(to, to);
		span(dot(to, sc->right), ahead, s->radius, half_h, half_w - 0.5f, sc->width - 1, &sc->seen[i].x0, &sc->seen[i].x1);
		span(dot(to, sc->up), ahead, s->radius, -half_h, half_h - 0.5f, sc->height - 1, &sc->seen[i].y0, &sc->seen[i].y1);
		span(dot(under, sc->right), under_ahead, s->radius, half_h, half_w - 0.5f, sc->width - 1, &sc->mirrored[i].x0, &sc->mirrored[i].x1);
		span(dot(under, sc->up), under_ahead, s->radius, -half_h, half_h - 0.5f, sc->height - 1, &sc->mirrored[i].y0, &sc->mirrored[i].y1);
		/* its shadow is within radius / light.y of where the light through its centre lands */
		spot[i][0] = s->centre.x - sc->light.x * s->centre.y * by;
		spot[i][1] = s->centre.z - sc->light.z * s->centre.y * by;
		spot[i][2] = s->radius * by * 1.01f + 0.01f;
		if (spot[i][0] - spot[i][2] < low_x) low_x = spot[i][0] - spot[i][2];
		if (spot[i][1] - spot[i][2] < low_z) low_z = spot[i][1] - spot[i][2];
		if (spot[i][0] + spot[i][2] > high_x) high_x = spot[i][0] + spot[i][2];
		if (spot[i][1] + spot[i][2] > high_z) high_z = spot[i][1] + spot[i][2];
	}
	sc->shade_x = low_x;
	sc->shade_z = low_z;
	sc->shade_by = (float)SHADE / (high_x - low_x > high_z - low_z ? high_x - low_x : high_z - low_z);
	memset(sc->shade, 0, sizeof sc->shade);
	for (i = 0; i < SPHERES; i++) {
		int x0 = (int)((spot[i][0] - spot[i][2] - low_x) * sc->shade_by), x1 = (int)((spot[i][0] + spot[i][2] - low_x) * sc->shade_by);
		int z0 = (int)((spot[i][1] - spot[i][2] - low_z) * sc->shade_by), z1 = (int)((spot[i][1] + spot[i][2] - low_z) * sc->shade_by);

		for (z = z0 < 0 ? 0 : z0; z <= z1 && z < SHADE; z++)
			for (x = x0 < 0 ? 0 : x0; x <= x1 && x < SHADE; x++)
				sc->shade[z * SHADE + x] |= (uint8_t)(1 << i);
	}
}

/* The next picture made, shared out and published. The workers that had the last of the
 * picture two before it (`done` or the one before: all are through with it) begin it when
 * they are through with the one between. */
static void
publish(float t, uint32_t done)
{
	uint32_t frame = go.frame + 1;
	struct picture *sc = &pictures[frame & 1];
	int width = sc->width, height = sc->height, shares, s;

	sc->owners = owners;
	sc->depth = depth;
	sc->shadows = shadows;
	set_scene(sc, t);
	set_spared(sc);
	shares = give_shares(sc, done);
	for (s = 32; s > 1 && ((width + (s < 8 ? 8 : s) - 1) / (s < 8 ? 8 : s)) * ((height + s - 1) / s) < 24 * shares; s >>= 1)
		;
	sc->tile_h = s;
	sc->tile_w = s < 8 ? 8 : s;
	sc->tiles = ((width + sc->tile_w - 1) / sc->tile_w) * ((height + sc->tile_h - 1) / sc->tile_h);
	/* a stride with no factor in common with the number of tiles visits every one of them */
	for (sc->stride = (uint32_t)sc->tiles * 5 / 8 + 1;; sc->stride++) {
		uint32_t a = sc->stride, b = (uint32_t)sc->tiles;

		while (b) { uint32_t r = a % b; a = b; b = r; }
		if (a == 1)
			break;
	}
	sc->step = (uint32_t)((uint64_t)sc->shares * sc->stride % (uint32_t)sc->tiles);
	/* (its pixels were picture `done`'s or the one's before: the list that shows that one has
	 * been drawn while the scene was made, mostly) */
	seglSettle();
	go.shown = done;
	go.frame = frame;
}

/* The bar under the picture: a button for what the keys do, saying how each stands, and a line of figures. */
static void
draw_bar(void)
{
	char label[BUTTONS][24];
	int k;

	snprintf(label[0], sizeof label[0], " %d x %d", width, height);
	snprintf(label[1], sizeof label[1], " Mirrors: %d", depth - 1);
	snprintf(label[2], sizeof label[2], " Shadows");
	snprintf(label[3], sizeof label[3], stopped ? " Go" : " Stop");
	snprintf(label[4], sizeof label[4], " Cores");
	snprintf(label[5], sizeof label[5], wanted > 1 ? " All workers" : wanted ? " One worker" : " No workers");
	ui_fill(window, 0, bar_y, wide, BAR, UI_FACE);
	for (k = 0; k < BUTTONS; k++)
		ui_button(window, 5 + button_step * k, bar_y + 5, button_step - 4, 22, label[k], (k == 2 && shadows) || (k == 3 && stopped) || (k == 4 && owners));
	ui_text(window, 8, bar_y + 33, status, -1, BLACK, 0);
	GrFlush();
}

/* The window is so large now: the picture has all of it but the bar, the buttons its width. */
static void
set_window(int w, int h)
{
	int view_w, view_h;

	wide = w;
	bar_y = h > BAR + 40 ? h - BAR : 40;
	button_step = wide > 10 + 20 * BUTTONS ? (wide - 10) / BUTTONS : 20;
	GrResizeWindow(picture, wide, bar_y);
	seglWindowChanged();
	seglSize(&view_w, &view_h);
	glViewport(0, 0, view_w, view_h);
}

/* The two pictures at another size, in the memory they have: called with no picture out. */
static void
set_size(int w, int h)
{
	int i;

	if (w * h > room)
		return;
	width = w, height = h;
	for (i = 0; i < 2; i++) {
		pictures[i].width = w, pictures[i].height = h;
		memset(pictures[i].pixels, 0, (size_t)(w * h * 4));
		glBindTexture(GL_TEXTURE_2D, texture[i]);
		seglTexturePointer(pictures[i].pixels, w, h, GL_RGBA);
	}
}

/* The sizes in turn, from the one that is nearest above the picture's own. */
static void
next_size(int *w, int *h)
{
	int k;

	for (k = 0; k < SIZES && (sizes[k][0] * sizes[k][1] <= width * height || sizes[k][0] * sizes[k][1] > room); k++)
		continue;
	k = k < SIZES ? k : 0;
	*w = sizes[k][0], *h = sizes[k][1];
}

static int want_w, want_h;	/* the size asked for with the picture given up */

/* What a key or its button does; what keys() is to return for it: 1 the picture given up
 * for a new one, with 2 other workers taken, with 4 another size. */
static int
act(int key)
{
	switch (key) {
	case 'q': case MWKEY_ESCAPE: go.stop = go.leave = 1; exit(0);
	case ' ': stopped = !stopped; return 0;
	case 'w': owners = !owners; return 1;
	case 'm': depth = depth % DEPTH + 1; reg_set_int("rays.turns", depth); return 1;
	case 'h': shadows = !shadows; reg_set_int("rays.shadows", shadows); return 1;
	case 'r':
		next_size(&want_w, &want_h);
		reg_set_int("rays.width", want_w), reg_set_int("rays.height", want_h);
		return 5;
	case 'W': wanted = wanted > 1 ? 1 : wanted ? 0 : MC_MAX_CORES; return 3;
	case 'a': wanted = MC_MAX_CORES; return 3;
	case '1': wanted = 1; return 3;
	case '0': wanted = 0; return 3;
	}
	return -1;
}

/* Keys and buttons: what they ask for together, as act() says it. */
static int
keys(void)
{
	GR_EVENT e;
	int again = 0, k;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return again;
		if (e.type == GR_EVENT_TYPE_CLOSE_REQ) {
			go.stop = go.leave = 1;	/* (the shares end at their next tiles and the jobs with them: the workers are waited for) */
			exit(0);
		}
		if (e.type == GR_EVENT_TYPE_UPDATE && e.update.utype == GR_UPDATE_SIZE && e.update.wid == window) {
			set_window(e.update.width, e.update.height);
			draw_bar();
		}
		if (e.type == GR_EVENT_TYPE_EXPOSURE && e.exposure.wid == window)
			draw_bar();
		k = -1;
		if (e.type == GR_EVENT_TYPE_KEY_DOWN)
			k = act(e.keystroke.ch);
		else if (e.type == GR_EVENT_TYPE_BUTTON_DOWN && (e.button.changebuttons & GR_BUTTON_L) && e.button.y >= bar_y + 5 &&
			 e.button.y < bar_y + 27 && e.button.x >= 5 && (e.button.x - 5) % button_step < button_step - 4 &&
			 (e.button.x - 5) / button_step < BUTTONS)
			k = act(button_keys[(e.button.x - 5) / button_step]);
		if (k < 0)
			continue;
		again |= k;
		draw_bar();
	}
}

static void
pass(void)
{
	mcw_rest();	/* nothing to do: the kernel has the core until the next pass (the pause hint with no workers) */
}

int
main(void)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	static const GLfixed corners[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	const char *text;
	GR_SCREEN_INFO info;
	int scale_by, frame = 0, i;
	uint32_t done = 0;	/* the last picture that is whole and has been seen to */
	float t = 0.0f;

	if ((text = getenv("RAY_SIZE")) != NULL && sscanf(text, "%dx%d", &width, &height) == 2)
		width = (width + 3) & ~3;
	if (width < 32 || height < 32 || width > 1024 || height > 768)
		width = 320, height = 240;
	if ((text = getenv("RAY_FRAMES")) != NULL)
		frames_left = atoi(text);
	if ((text = getenv("RAY_WORKERS")) != NULL)
		wanted = atoi(text);
	still = getenv("RAY_STILL") != NULL;
	if (frames_left < 0 && !getenv("RAY_SIZE")) {
		/* as it was left (registry.h); a test has what it says and the rest as it comes */
		int w = reg_int("rays.width", width), h = reg_int("rays.height", height);

		if (w >= 32 && h >= 32 && w <= 640 && h <= 480 && !(w & 3))
			width = w, height = h;
		depth = reg_int("rays.turns", DEPTH);
		depth = depth < 1 || depth > DEPTH ? DEPTH : depth;
		shadows = reg_int("rays.shadows", 1) != 0;
	}
	if (GrOpen() < 0) {
		fprintf(stderr, "nxray: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	GrGetScreenInfo(&info);
	for (scale_by = 3; scale_by > 1 && (width * scale_by > info.cols - 20 || height * scale_by > info.rows - 60 - BAR); scale_by--)
		;
	ui_init();
	wide = width * scale_by, bar_y = height * scale_by;
	button_step = (wide - 10) / BUTTONS;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Rays", GR_ROOT_WINDOW_ID, -1, -1, wide, bar_y + BAR, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	picture = GrNewWindow(window, 0, 0, wide, bar_y, 0, BLACK, BLACK);
	GrMapWindow(picture);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	draw_bar();
	if (seglInit(picture) < 0) {
		fprintf(stderr, "nxray: this machine has no GPU\n");
		return 1;
	}
	/* a picture is a texture where it lies, one rectangle over the window; two, one shown
	 * while the other is traced */
	/* (their memory is for the largest size the button gives, or this one's if it is larger) */
	glGenTextures(2, texture);
	room = width * height > sizes[SIZES - 1][0] * sizes[SIZES - 1][1] ? width * height : sizes[SIZES - 1][0] * sizes[SIZES - 1][1];
	for (i = 0; i < 2; i++) {
		pictures[i].pixels = seglMemory((unsigned)(room * 4));
		if (!pictures[i].pixels) {
			fprintf(stderr, "nxray: no GPU memory left\n");
			return 1;
		}
	}
	set_size(width, height);
	set_window(wide, bar_y + BAR);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(corners, uv, texture[0], 255, 255, 0);
	seglSwap();

	atexit(mcw_close);
	atexit(end_jobs);	/* (first: the workers' jobs end only when told) */
	even = getenv("RAY_EVEN") != NULL;
	take_workers();
	while (frames_left != 0) {
		/* when the keys were last read and the picture's figures last said, and what was traced since */
		static unsigned keys_at, said_at, pictures_since, rays_since, clock_from, jobs_at, whole_at, began_at[2];
		struct picture *sc;
		int next = 0, k, again = 0, ahead = 1, from = 1, tiles;
		unsigned now = now_ms(), shown = now, rays = 0, sum = 0, took, looked = now;
		uint32_t at;
		char title[120];

		if (!clock_from)
			clock_from = said_at = jobs_at = whole_at = now;
		/* a worker that lost its core is asked for again now and then */
		if (workers && now - jobs_at >= 1000) {
			jobs_at = now;
			start_jobs();
		}
		/* Two pictures are out while they follow each other freely: a worker goes from one to
		 * the next, and this core shows one and makes the next but one meanwhile. One for a
		 * test, whose pictures are timed each by itself, and with no workers. */
		for (k = 1; k <= workers && frames_left < 0 && !getenv("RAY_ONE"); k++)
			if (running[k]) {
				ahead = 2;
				break;
			}
		while (!stopped && go.frame - done < (uint32_t)ahead) {
			/* the scene moves by the clock, so it moves as fast whatever a picture takes (a test's
			 * pictures are a fixed step apart, to be the same pictures every time) */
			began_at[(go.frame + 1) & 1] = now = now_ms();
			if (!still && frames_left < 0)
				t = (float)(now - clock_from) * 0.0006f;
			publish(t, done);
			if (!still && frames_left >= 0)
				t += 0.35f;
		}
		if (go.frame == done) {
			/* stopped, and nothing out */
			pass();
			if (now - keys_at >= 200) {
				keys_at = now;
				k = keys();
				if (k & 4) {
					seglSettle();
					set_size(want_w, want_h);
					draw_bar();
				}
				if (k & 2)
					take_workers();
			}
			continue;
		}
		at = done + 1;
		sc = &pictures[at & 1];
		tiles = sc->tiles;
		while (sc->traced_by && !all_in(at, &from)) {
			pass();
			now = now_ms();
			/* (asking every worker is a pass's worth: now and then, for one that stopped or lost its core) */
			if (now - looked >= 50) {
				looked = now;
				look_at_jobs(done);
			}
			if (now - shown >= 200 && now - began_at[at & 1] >= 1500) {
				/* a picture is shown when it is whole; one that takes long, as it fills */
				seglQuad(corners, uv, texture[at & 1], 255, 255, 0);
				seglSwap();
				shown = now;
			}
			if (now - keys_at >= 200) {
				keys_at = now;
				/* (a picture given up: the shares end at their next tiles) */
				if ((k = keys()) != 0)
					again |= k, go.stop = 1;
			}
		}
		while (!sc->traced_by && next < tiles && !again) {
			/* this core alone: a tile, then a look at the window */
			rays += trace_tile(sc, (uint32_t)next++ * sc->stride % (uint32_t)tiles, 0);
			now = now_ms();
			if (now - shown >= 200 && now - began_at[at & 1] >= 1500) {
				seglQuad(corners, uv, texture[at & 1], 255, 255, 0);
				seglSwap();
				shown = now;
			}
			if (now - keys_at >= 200) {
				keys_at = now;
				again |= keys();
			}
		}
		/* (a picture that was whole when it was asked about has had no look at the keys) */
		if (!again && now_ms() - keys_at >= 200) {
			keys_at = now_ms();
			again = keys();
		}
		if (again) {
			/* the pictures that are out are given up, and are waited for: then all begins again */
			go.stop = 1;
			for (from = 1; pictures[go.frame & 1].traced_by && !all_in(go.frame, &from);) {
				pass();
				look_at_jobs(done);
			}
			done = go.frame;
			if (again & 4) {
				/* (what shows the last picture has been drawn before its texture is another size) */
				seglSettle();
				set_size(want_w, want_h);
				draw_bar();
			}
			if (again & 2)
				take_workers();
			go.stop = 0;
			continue;
		}
		now = now_ms();
		took = ahead > 1 ? now - whole_at : now - began_at[at & 1];
		whole_at = now;
		for (k = 1; k <= workers && sc->traced_by; k++)
			if (sc->plan[k].share != NO_SHARE)
				rays += slots[at & 1][k].rays;
		/* (shown, and not waited for: its pixels are written again by the picture after the next, which publish() holds until this is drawn) */
		seglQuad(corners, uv, texture[at & 1], 255, 255, 0);
		seglSwapEarly();
		done = at;
		/* (the sum is a test's: 300 thousand pixels are half a second of this core, with every worker waiting) */
		for (i = 0; frames_left > 0 && i < width * height; i++)
			sum = sum * 31 + sc->pixels[i];
		if (sc->traced_by && getenv("RAY_SHARES")) {
			/* (for a test: what the workers' parts of the picture took, in instructions) */
			uint32_t least = ~0u, most = 0, all = 0, lent = 0;

			for (k = 1; k <= workers; k++)
				if (sc->plan[k].share != NO_SHARE) {
					uint32_t took_k = slots[at & 1][k].spent + slots[at & 1][k].lent;

					all += took_k >> 4, lent += sc->plan[k].takes;
					if (took_k < least) least = took_k;
					if (took_k > most) most = took_k;
				}
			fprintf(stderr, "raystat: %d shares of %d tiles: %u to %u instructions, %u the mean; %u tiles traced for another\n", sc->traced_by, tiles,
				(unsigned)least, (unsigned)most, (unsigned)(all / (uint32_t)sc->traced_by << 4), (unsigned)lent);
		}
		pictures_since++;
		rays_since += rays;
		if (frames_left > 0) {
			/* a test: every picture's line */
			fprintf(stderr, "raystat: picture %d, %d x %d: %u ms with %d workers (%s), %u rays, %u thousand rays a second, sum %08x\n", frame, width,
				height, took, workers, "of the machine's", rays, took ? rays / took : 0, sum);
		} else if (now_ms() - said_at >= 1000) {
			/* once a second: a line on the console and a new title are a tenth of a second of this
			 * core each, and said after every picture they were most of the time between two */
			unsigned span = now_ms() - said_at, tenths = pictures_since * 10000 / span;

			snprintf(title, sizeof title, "Rays - %d workers: %u.%u pictures a second, %u thousand rays a second", workers, tenths / 10, tenths % 10,
				rays_since / span);
			snprintf(status, sizeof status, "%s", title + 7);
			draw_bar();
			fprintf(stderr, "raystat: %d x %d with %d workers: %u.%u pictures a second (%u ms the last), %u thousand rays a second\n", width, height,
				workers, tenths / 10, tenths % 10, took, rays_since / span);
			said_at = now_ms();
			pictures_since = rays_since = 0;
		}
		frame++;
		if (frames_left > 0)
			frames_left--;
	}
	seglSettle();
	return 0;
}
