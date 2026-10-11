/*
 * nxray: a ray tracer in tiles, every worker core tracing its share of them
 * (docs/raytrace.md, docs/multicore.md). The work this machine's small cores are for: a pixel
 * is a few thousand instructions of float arithmetic and four bytes stored.
 *
 * Core 0 traces nothing. It gives every worker its share of a picture at once (trace_share),
 * shows the picture as it fills, and reads the keys:
 *   a   all the workers the geometry has      1   one worker      0   core 0 alone
 *   s   the next geometry (15 small, 3 large and 12 small, 7 large, 15 of the smallest)
 *   w   each tile framed in its core's colour  space  stop and go   q, Escape  leave
 * For tests: RAY_FRAMES=N leaves after N pictures, RAY_WORKERS=N and RAY_SHAPE=4,4,... set
 * the start, RAY_SIZE=WxH the picture (320x240), RAY_STILL=1 keeps the scene where it is.
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
#include "nano-X.h"
#include "mcw.h"

#define ONE 65536
#define SPHERES 6
#define DEPTH 3

struct vec { float x, y, z; };
struct sphere { struct vec centre; float radius, radius2; struct vec colour; float mirror; };

/* The scene and the camera of the picture being traced: core 0 writes them between two
 * pictures, the workers only read. */
static struct {
	struct sphere spheres[SPHERES];
	struct vec eye, right, up, forward, light;
	int width, height, owners;
	uint32_t *pixels;
	/* A tile is 32 pixels square, or as much smaller as gives a worker two dozen of them (never
	 * narrower than eight pixels, which are two of the machine's texels). */
	int tile_w, tile_h, tiles;
	uint32_t stride, step;	/* the order the tiles are traced in: below */
	int stop;		/* the picture is given up: a share ends at its next tile */
} scene __attribute__((aligned(16)));

static inline struct vec v(float x, float y, float z) { struct vec r = {x, y, z}; return r; }
static inline struct vec add(struct vec a, struct vec b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline struct vec sub(struct vec a, struct vec b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline struct vec scale(struct vec a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline struct vec unit(struct vec a) { return scale(a, 1.0f / __builtin_sqrtf(dot(a, a))); }

/* The nearest thing a ray meets: a sphere's number, SPHERES for the ground, -1 for the sky. */
static int
nearest(struct vec from, struct vec way, float *distance)
{
	float best = 1e30f;
	int what = -1, i;

	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &scene.spheres[i];
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
shadowed(struct vec from, struct vec way)
{
	int i;

	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &scene.spheres[i];
		struct vec to = sub(s->centre, from);
		float along = dot(to, way);

		if (along > 0.0f && dot(to, to) - along * along < s->radius2)
			return 1;
	}
	return 0;
}

/* The colour a ray brings back; *rays counts every ray cast for it. */
static struct vec
trace(struct vec from, struct vec way, uint32_t *rays)
{
	struct vec seen = v(0, 0, 0);
	float weight = 1.0f;
	int bounce;

	for (bounce = 0; bounce < DEPTH; bounce++) {
		struct vec at, normal, colour;
		float distance, mirror, lit;
		int what = nearest(from, way, &distance);

		(*rays)++;
		if (what < 0) {
			/* the sky: paler towards the horizon */
			float high = way.y > 0.0f ? way.y : 0.0f;

			seen = add(seen, scale(v(0.75f - 0.45f * high, 0.85f - 0.35f * high, 1.0f), weight));
			break;
		}
		at = add(from, scale(way, distance));
		if (what == SPHERES) {
			/* the ground: squares of two greys */
			int cx = (int)(at.x + 1000.0f), cz = (int)(at.z + 1000.0f);

			normal = v(0, 1, 0);
			colour = (cx ^ cz) & 1 ? v(0.85f, 0.85f, 0.8f) : v(0.25f, 0.27f, 0.3f);
			mirror = 0.25f;
		} else {
			const struct sphere *s = &scene.spheres[what];

			normal = scale(sub(at, s->centre), 1.0f / s->radius);
			colour = s->colour;
			mirror = s->mirror;
		}
		lit = dot(normal, scene.light);
		(*rays)++;
		if (lit < 0.0f || shadowed(at, scene.light))
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
	return c <= 0.0f ? 0 : c >= 1.0f ? 255 : (uint32_t)(c * 255.0f);
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
trace_tile(uint32_t tile, uint32_t core)
{
	int tw = scene.tile_w, th = scene.tile_h;
	int across = (scene.width + tw - 1) / tw, x0 = (int)(tile % (uint32_t)across) * tw, y0 = (int)(tile / (uint32_t)across) * th;
	int x1 = x0 + tw < scene.width ? x0 + tw : scene.width, y1 = y0 + th < scene.height ? y0 + th : scene.height, x, y;
	float half_w = (float)scene.width * 0.5f, half_h = (float)scene.height * 0.5f, zoom = 1.0f / half_h;
	uint32_t rays = 0;

	for (y = y0; y < y1; y++) {
		uint32_t *out = scene.pixels + y * scene.width + x0;

		for (x = x0; x < x1; x++) {
			float sx = ((float)x + 0.5f - half_w) * zoom, sy = (half_h - (float)y - 0.5f) * zoom;
			struct vec way = unit(add(add(scale(scene.right, sx), scale(scene.up, sy)), scale(scene.forward, 1.6f)));
			struct vec c = trace(scene.eye, way, &rays);

			if (scene.owners && (x == x0 || y == y0))
				*out++ = owner_colour[core & 15];
			else
				*out++ = byte_of(c.x) << 16 | byte_of(c.y) << 8 | byte_of(c.z);
		}
	}
	return rays;
}

/*
 * A worker's job: its share of a picture, all of it, so that it is never without work while
 * the picture is traced (handed a tile at a time, a worker waited some passes for its next:
 * 255 of them worked a fifth of the time). The tiles are taken in an order that scatters
 * them, tile j being number j * stride of the picture, and worker w of n has every n-th: as
 * many as any other, from all over the picture. which: w, and n from bit 16.
 */
static uint32_t
trace_share(uint32_t which, uint32_t core)
{
	uint32_t n = which >> 16, tiles = (uint32_t)scene.tiles, j = which & 0xffff, at = j * scene.stride % tiles, rays = 0;

	for (; j < tiles && !scene.stop; j += n) {
		rays += trace_tile(at, core);
		if ((at += scene.step) >= tiles)
			at -= tiles;
	}
	return rays;
}

/* ---- core 0 ---- */

static GR_WINDOW_ID window;
static int workers, wanted = MC_MAX_CORES, stopped, frames_left = -1, still;
/* The geometries offered, as many workers of each as this machine has room for (15 on a
 * machine of 16 cores, 63 on one of 64): small, three large and the rest small, large, the
 * smallest. */
static unsigned
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

/* The workers given back, and as many taken as wanted. */
static void
take_workers(void)
{
	extern char __DATA_BEGIN__[], _end[];

	mcw_close();
	workers = wanted > 0 ? mcw_open(wanted) : 0;
	if (workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
}

static void
set_scene(float t)
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

		scene.spheres[i].centre = v(made[i].ring * cosf(a), made[i].height + (i ? 0.25f * sinf(2.0f * a) + 0.25f : 0.0f), made[i].ring * sinf(a));
		scene.spheres[i].radius = made[i].radius;
		scene.spheres[i].radius2 = made[i].radius * made[i].radius;
		scene.spheres[i].colour = v(made[i].r, made[i].g, made[i].b);
		scene.spheres[i].mirror = made[i].mirror;
	}
	scene.eye = v(7.0f * cosf(turn), 2.6f, 7.0f * sinf(turn));
	scene.forward = unit(sub(target, scene.eye));
	scene.right = unit(v(-scene.forward.z, 0, scene.forward.x));
	scene.up = v(scene.right.y * scene.forward.z - scene.right.z * scene.forward.y, scene.right.z * scene.forward.x - scene.right.x * scene.forward.z,
		scene.right.x * scene.forward.y - scene.right.y * scene.forward.x);
	scene.light = unit(v(0.5f, 0.9f, -0.3f));
}

/* Keys; true when the picture being traced is to be given up for a new one. */
static int
keys(void)
{
	GR_EVENT e;
	int again = 0;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return again;
		if (e.type == GR_EVENT_TYPE_CLOSE_REQ) {
			scene.stop = 1;	/* (the shares end at their next tiles: the workers are waited for) */
			exit(0);
		}
		if (e.type == GR_EVENT_TYPE_UPDATE && e.update.utype == GR_UPDATE_SIZE)
			seglWindowChanged();
		if (e.type != GR_EVENT_TYPE_KEY_DOWN)
			continue;
		switch (e.keystroke.ch) {
		case 'q': case MWKEY_ESCAPE: scene.stop = 1; exit(0);
		case ' ': stopped = !stopped; break;
		case 'w': scene.owners = !scene.owners; again = 1; break;
		case 'a': wanted = MC_MAX_CORES; again = 2; break;
		case '1': wanted = 1; again = 2; break;
		case '0': wanted = 0; again = 2; break;
		}
	}
}

static void
pass(void)
{
	__asm__ volatile(".word 0x0100000f");	/* pause: the machine's pass ends */
}

int
main(void)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	static const GLfixed corners[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	const char *text;
	GR_SCREEN_INFO info;
	GLuint texture;
	int width = 320, height = 240, scale_by, frame = 0, i, view_w, view_h;
	unsigned started = 0;
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
	if (GrOpen() < 0) {
		fprintf(stderr, "nxray: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	GrGetScreenInfo(&info);
	for (scale_by = 3; scale_by > 1 && (width * scale_by > info.cols - 20 || height * scale_by > info.rows - 60); scale_by--)
		;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Rays", GR_ROOT_WINDOW_ID, -1, -1, width * scale_by, height * scale_by, 0);
	GrSelectEvents(window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	GrMapWindow(window);
	GrSetFocus(window);
	if (seglInit(window) < 0) {
		fprintf(stderr, "nxray: this machine has no GPU\n");
		return 1;
	}
	scene.width = width;
	scene.height = height;
	scene.pixels = seglMemory((unsigned)(width * height * 4));
	if (!scene.pixels) {
		fprintf(stderr, "nxray: no GPU memory left\n");
		return 1;
	}
	memset(scene.pixels, 0, (size_t)(width * height * 4));
	/* the picture is a texture where it lies: one rectangle over the window, drawn again as it fills */
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	seglTexturePointer(scene.pixels, width, height, GL_RGBA);
	seglSize(&view_w, &view_h);
	glViewport(0, 0, view_w, view_h);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(corners, uv, texture, 255, 255, 0);
	seglSwap();

	atexit(mcw_close);
	take_workers();
	while (frames_left != 0) {
		static uint8_t busy[MC_MAX_CORES];
		/* when the keys were last read and the picture's figures last said, and what was traced since */
		static unsigned keys_at, said_at, pictures_since, rays_since, clock_from;
		int next = 0, out = 0, k, again = 0, s, tiles;
		unsigned began = now_ms(), shown = began, rays = 0, sum = 0, took;
		char title[120];

		if (!clock_from)
			clock_from = said_at = began;
		/* the scene moves by the clock, so it moves as fast whatever a picture takes (a test's
		 * pictures are a fixed step apart, to be the same pictures every time) */
		if (!still && frames_left < 0)
			t = (float)(began - clock_from) * 0.0006f;

		set_scene(t);
		for (s = 32; s > 1 && ((width + (s < 8 ? 8 : s) - 1) / (s < 8 ? 8 : s)) * ((height + s - 1) / s) < 24 * workers; s >>= 1)
			;
		scene.tile_h = s;
		scene.tile_w = s < 8 ? 8 : s;
		scene.tiles = tiles = ((width + scene.tile_w - 1) / scene.tile_w) * ((height + scene.tile_h - 1) / scene.tile_h);
		/* a stride with no factor in common with the number of tiles visits every one of them */
		for (scene.stride = (uint32_t)tiles * 5 / 8 + 1;; scene.stride++) {
			uint32_t a = scene.stride, b = (uint32_t)tiles;

			while (b) { uint32_t r = a % b; a = b; b = r; }
			if (a == 1)
				break;
		}
		scene.step = (uint32_t)(workers ? workers : 1) * scene.stride % (uint32_t)tiles;
		scene.stop = 0;
		/* every worker its share of the picture at once: they trace, this core shows */
		for (k = 1; k <= workers; k++) {
			mcw_post(k, trace_share, (uint32_t)(k - 1) | (uint32_t)workers << 16, (uint32_t)k);
			busy[k] = 1;
			out++;
		}
		while ((out || (workers == 0 && next < tiles)) && !(again && workers == 0)) {
			if (workers == 0)	/* core 0 alone: a tile, then a look at the window */
				rays += trace_tile((uint32_t)next++ * scene.stride % (uint32_t)tiles, 0);
			for (k = 1; k <= workers; k++)
				if (busy[k] && mcw_done(k)) {
					rays += mcw_result(k);
					busy[k] = 0;
					out--;
				}
			if (now_ms() - shown >= 200 && now_ms() - began >= 1500) {
				/* a picture is shown when it is whole; one that takes long, as it fills (the
				 * workers' in a third of a second came out half old and half new) */
				seglSwapAgain();
				shown = now_ms();
			}
			if (now_ms() - keys_at >= 200) {
				keys_at = now_ms();
				/* (a picture given up: the shares end at their next tiles, and are waited for) */
				if ((again = keys()))
					scene.stop = 1;
			} else if (workers) {
				pass();
			}
		}
		took = now_ms() - began;
		seglSwapAgain();
		if (again >= 2) {
			take_workers();
			continue;
		}
		if (again)
			continue;
		/* (the sum is a test's: 300 thousand pixels are half a second of this core, with every worker waiting) */
		for (i = 0; frames_left > 0 && i < width * height; i++)
			sum = sum * 31 + scene.pixels[i];
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
			GrSetWindowTitle(window, title);
			GrFlush();
			fprintf(stderr, "raystat: %d x %d with %d workers: %u.%u pictures a second (%u ms the last), %u thousand rays a second\n", width, height,
				workers, tenths / 10, tenths % 10, took, rays_since / span);
			said_at = now_ms();
			pictures_since = rays_since = 0;
		}
		frame++;
		if (frames_left > 0)
			frames_left--;
		if (!still && frames_left >= 0)
			t += 0.35f;
		(void)started;
		while (stopped && frames_left != 0) {
			pass();
			if (keys())
				break;
		}
	}
	return 0;
}
