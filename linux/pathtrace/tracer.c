/*
 * The path tracer's own part (docs/raytrace.md): a Cornell box, and one sample of every
 * pixel of a tile as a job for a worker core (docs/multicore.md). A pixel is a few thousand
 * instructions of float arithmetic and sixteen bytes stored, which is the work the machine's
 * small cores are for.
 *
 * What a job writes: its tile's part of the sums (three floats a pixel) and of the picture
 * (a word a pixel), and its own stack. A tile is 32 pixels across and the picture's width a
 * multiple of four, so a tile's rows begin and end on 16 bytes of both and no two cores store
 * to the same 16. The scene is only read.
 */
#include "tracer.h"
#include "mcw.h"

struct pt_scene pt;

struct vec { float x, y, z; };
/* What a ray meets: nothing (the box's open side), a wall, the lamp in the ceiling, a ball
 * (and which), a side of the block (two an axis, the low one first), the model. */
enum { MET_NONE, MET_LEFT, MET_RIGHT, MET_FLOOR, MET_CEILING, MET_BACK, MET_LAMP, MET_BALL, MET_BLOCK = MET_BALL + 3, MET_MODEL = MET_BLOCK + 6 };
/* Where in a model: the triangle, and how far along its two edges. */
struct met { const struct pt_triangle *triangle; float u, w; };

static inline struct vec v(float x, float y, float z) { struct vec r = {x, y, z}; return r; }
static inline struct vec add(struct vec a, struct vec b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline struct vec sub(struct vec a, struct vec b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline struct vec mul(struct vec a, struct vec b) { return v(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline struct vec scale(struct vec a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline struct vec unit(struct vec a) { return scale(a, 1.0f / __builtin_sqrtf(dot(a, a))); }

/* The box is the unit cube, open towards the eye (z = 0); the lamp a square of the ceiling. */
#define PT_MOST 2.5f
#define LAMP_X0 0.35f
#define LAMP_X1 0.65f
#define LAMP_Z0 0.35f
#define LAMP_Z1 0.65f
static const struct vec white = {0.75f, 0.75f, 0.75f}, red = {0.70f, 0.12f, 0.10f}, green = {0.14f, 0.55f, 0.16f};
/* two balls and a block; beside a model there is one ball, in the near corner */
static const struct { struct vec centre; float radius; } balls[3] = {{{0.30f, 0.18f, 0.62f}, 0.18f}, {{0.72f, 0.14f, 0.35f}, 0.14f},
                                                                    {{0.14f, 0.10f, 0.16f}, 0.10f}};
static const struct vec block_low = {0.55f, 0.0f, 0.60f}, block_high = {0.82f, 0.50f, 0.86f};

static inline uint32_t
next(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13, x ^= x >> 17, x ^= x << 5;
	return *state = x;
}

static inline float
chance(uint32_t *state)
{
	return (float)(next(state) >> 8) * (1.0f / 16777216.0f);
}

/* A point of the unit ball, by throwing away the cube's corners. */
static struct vec
in_ball(uint32_t *state)
{
	for (;;) {
		struct vec p = v(2.0f * chance(state) - 1.0f, 2.0f * chance(state) - 1.0f, 2.0f * chance(state) - 1.0f);
		float d = dot(p, p);

		if (d < 1.0f && d > 0.0001f)
			return scale(p, 1.0f / __builtin_sqrtf(d));
	}
}

static inline struct vec cross(struct vec a, struct vec b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline struct vec of(const float *p) { return v(p[0], p[1], p[2]); }

/* Where a ray is inside a box, if it is before `far`: the distance it comes in at (0 from inside). */
static inline int
box_met(const struct pt_node *node, struct vec from, struct vec over, float far, float *at)
{
	float a = (node->low[0] - from.x) * over.x, b = (node->high[0] - from.x) * over.x, low, high;

	/* (the lesser and the greater as an instruction each: no number here is not one) */
	low = __builtin_fminf(a, b), high = __builtin_fmaxf(a, b);
	a = (node->low[1] - from.y) * over.y, b = (node->high[1] - from.y) * over.y;
	low = __builtin_fmaxf(low, __builtin_fminf(a, b)), high = __builtin_fminf(high, __builtin_fmaxf(a, b));
	a = (node->low[2] - from.z) * over.z, b = (node->high[2] - from.z) * over.z;
	low = __builtin_fmaxf(low, __builtin_fminf(a, b)), high = __builtin_fminf(high, __builtin_fmaxf(a, b));
	*at = low;
	return (high >= low) & (high > 0.0f) & (low < far);
}

/* One over a ray's direction, for the boxes; a ray along an axis is given a very steep one. */
static inline struct vec
over_way(struct vec way)
{
	return v(1.0f / (way.x > -1e-9f && way.x < 1e-9f ? 1e-9f : way.x), 1.0f / (way.y > -1e-9f && way.y < 1e-9f ? 1e-9f : way.y),
	         1.0f / (way.z > -1e-9f && way.z < 1e-9f ? 1e-9f : way.z));
}

/* A ray and a triangle (Moeller and Trumbore's test): how far, and where in it. */
static inline int
triangle_met(const struct pt_triangle *tri, struct vec from, struct vec way, float *t, float *u, float *w)
{
	struct vec e1 = of(tri->edge1), e2 = of(tri->edge2), p = cross(way, e2), s, q;
	float det = dot(e1, p), inv;

	if (det > -1e-12f && det < 1e-12f)
		return 0;
	inv = 1.0f / det;
	s = sub(from, of(tri->corner));
	*u = dot(s, p) * inv;
	if (*u < 0.0f || *u > 1.0f)
		return 0;
	q = cross(s, e1);
	*w = dot(way, q) * inv;
	if (*w < 0.0f || *u + *w > 1.0f)
		return 0;
	*t = dot(e2, q) * inv;
	return 1;
}

/* The nearest triangle of a model a ray meets nearer than *nearest: down the tree of boxes,
 * the nearer of two first, so that the farther is mostly not looked into. */
static inline int
mesh_nearest(const struct pt_mesh *mesh, struct vec from, struct vec way, float *nearest, struct met *met)
{
	uint32_t stack[64], at = 0;
	float entered[64];	/* where the ray comes into each box put by */
	int held = 0;
	struct vec over = over_way(way);
	const struct pt_triangle *found = 0;
	float fu = 0.0f, fw = 0.0f, enter, best = *nearest;

	if (!box_met(&mesh->nodes[0], from, over, best, &enter))
		return 0;
	for (;;) {
		const struct pt_node *node = &mesh->nodes[at];

		if (node->b) {
			const struct pt_triangle *tri = mesh->triangles + node->a, *end = tri + node->b;

			for (; tri < end; tri++) {
				float t, u, w;

				if (triangle_met(tri, from, way, &t, &u, &w) && t > 0.0005f && t < best)
					best = t, found = tri, fu = u, fw = w;
			}
		} else {
			float t0, t1;
			int in0 = box_met(&mesh->nodes[node->a], from, over, best, &t0), in1 = box_met(&mesh->nodes[node->a + 1], from, over, best, &t1);

			if (in0 && in1) {
				entered[held] = t0 <= t1 ? t1 : t0;
				stack[held++] = t0 <= t1 ? node->a + 1 : node->a;
				at = t0 <= t1 ? node->a : node->a + 1;
				continue;
			}
			if (in0 || in1) {
				at = in0 ? node->a : node->a + 1;
				continue;
			}
		}
		/* (a box put by may be behind what was found since: only one the ray enters nearer is taken) */
		do {
			if (!held)
				goto done;
			at = stack[--held];
		} while (!(entered[held] < best));
	}
done:
	if (!found)
		return 0;
	*nearest = best;
	met->triangle = found, met->u = fu, met->w = fw;
	return 1;
}

/* A model's surface where a ray met it: towards the mean of the triangle's corners' normals. */
static inline struct vec
mesh_normal(const struct met *met, struct vec way)
{
	float k = 1.0f - met->u - met->w;
	struct vec normal = unit(add(add(scale(of(met->triangle->normal[0]), k), scale(of(met->triangle->normal[1]), met->u)), scale(of(met->triangle->normal[2]), met->w)));

	/* (seen from behind, as the inside of a spout is: matt and mirror have one side) */
	if (pt.material[0] != PT_GLASS && dot(normal, way) > 0.0f)
		normal = scale(normal, -1.0f);
	return normal;
}

/* Whether any triangle of a model is in a ray's way before `far`: every box the ray is in
 * before then is gone into, a box's two tried together as mesh_nearest() does. */
static int
mesh_between(const struct pt_mesh *mesh, struct vec from, struct vec way, float far)
{
	uint32_t stack[64], at = 0;
	int held = 0;
	struct vec over = over_way(way);
	float enter;

	if (!box_met(&mesh->nodes[0], from, over, far, &enter))
		return 0;
	for (;;) {
		const struct pt_node *node = &mesh->nodes[at];

		if (node->b) {
			const struct pt_triangle *tri = mesh->triangles + node->a, *end = tri + node->b;

			for (; tri < end; tri++) {
				float t, u, w;

				if (triangle_met(tri, from, way, &t, &u, &w) && t > 0.0005f && t < far)
					return 1;
			}
		} else {
			int in0 = box_met(&mesh->nodes[node->a], from, over, far, &enter), in1 = box_met(&mesh->nodes[node->a + 1], from, over, far, &enter);

			if (in0 && in1) {
				stack[held++] = node->a + 1;
				at = node->a;
				continue;
			}
			if (in0 || in1) {
				at = in0 ? node->a : node->a + 1;
				continue;
			}
		}
		if (!held)
			return 0;
		at = stack[--held];
	}
}

/* One pair of the block's sides, a to b along an axis: narrows where a ray (from o, going d
 * along it) is between them, and notes the side it comes in by. False: it never is. */
static inline int
block_pair(float o, float d, float a, float b, int axis, float *low, float *high, int *side)
{
	float t0, t1;
	int far_side = 0;

	if (d > -1e-6f && d < 1e-6f)
		return !(o < a || o > b);
	t0 = (a - o) / d, t1 = (b - o) / d;
	if (t0 > t1) {
		float swap = t0;

		t0 = t1, t1 = swap, far_side = 1;
	}
	if (t0 > *low)
		*low = t0, *side = axis * 2 + far_side;
	if (t1 < *high)
		*high = t1;
	return 1;
}

/* Whether a ray can do without the balls and the block (or, beside a model, the one ball):
 * it starts beyond them on some side and goes away. They are within these, with room. */
static inline int
clear_of_things(struct vec from, struct vec way)
{
	if (pt.scene)
		return (from.y >= 0.21f && way.y >= 0.0f) || (from.x >= 0.25f && way.x >= 0.0f) || (from.z >= 0.27f && way.z >= 0.0f);
	/* (the block's top is at 0.5 itself: a ray along it is the block's to answer) */
	return (from.y >= 0.5f && way.y > 1e-6f) || (from.x >= 0.87f && way.x >= 0.0f) || (from.x <= 0.11f && way.x <= 0.0f) ||
	       (from.z >= 0.87f && way.z >= 0.0f) || (from.z <= 0.20f && way.z <= 0.0f);
}

/* The nearest thing a ray meets (MET_NONE: the open side) and how far; *met: where in a model. */
static inline __attribute__((always_inline)) int
nearest(struct vec from, struct vec way, float *distance, struct met *met)
{
	int i, first = pt.scene ? 2 : 0, last = pt.scene ? 3 : 2, what = MET_NONE, side = 0;
	float best = 1e30f, low = -1e30f, high = 1e30f, t;

	/* the room: a ray inside it leaves by one wall of each pair */
	if (way.x < 0.0f) {
		t = -from.x / way.x;
		if (t > 0.0005f && t < best) best = t, what = MET_LEFT;
	} else if (way.x > 0.0f) {
		t = (1.0f - from.x) / way.x;
		if (t > 0.0005f && t < best) best = t, what = MET_RIGHT;
	}
	if (way.y < 0.0f) {
		t = -from.y / way.y;
		if (t > 0.0005f && t < best) best = t, what = MET_FLOOR;
	} else if (way.y > 0.0f) {
		t = (1.0f - from.y) / way.y;
		if (t > 0.0005f && t < best) best = t, what = MET_CEILING;
	}
	if (way.z > 0.0f) {
		t = (1.0f - from.z) / way.z;
		if (t > 0.0005f && t < best) best = t, what = MET_BACK;
	}
	if (what == MET_CEILING && best < 1e29f) {
		/* the ceiling: the lamp is a square of it */
		float x = from.x + way.x * best, z = from.z + way.z * best;

		if (x > LAMP_X0 && x < LAMP_X1 && z > LAMP_Z0 && z < LAMP_Z1)
			what = MET_LAMP;
	}
	if (!clear_of_things(from, way)) {
		for (i = first; i < last; i++) {
			struct vec to = sub(balls[i].centre, from);
			float along = dot(to, way), off2 = dot(to, to) - along * along, r2 = balls[i].radius * balls[i].radius;

			if (off2 < r2) {
				float half = __builtin_sqrtf(r2 - off2);

				t = along - half;
				if (t < 0.0005f)
					t = along + half;	/* from inside it (glass) */
				if (t > 0.0005f && t < best)
					best = t, what = MET_BALL + i;
			}
		}
		/* the block: where the ray is inside all three pairs of its sides */
		if (!pt.scene && block_pair(from.x, way.x, block_low.x, block_high.x, 0, &low, &high, &side) &&
		    block_pair(from.y, way.y, block_low.y, block_high.y, 1, &low, &high, &side) &&
		    block_pair(from.z, way.z, block_low.z, block_high.z, 2, &low, &high, &side) && low < high && low > 0.0005f && low < best)
			best = low, what = MET_BLOCK + side;
	}
	/* a model in the block's place */
	if (pt.scene && mesh_nearest(&pt_meshes[pt.scene - 1], from, way, &best, met))
		what = MET_MODEL;
	*distance = best;
	return best < 1e29f ? what : MET_NONE;
}

/* nearest() for the picture of distances (pt.depth), which is in no hurry. */
static __attribute__((noinline)) int
nearest_once(struct vec from, struct vec way, float *distance)
{
	struct met met;

	return nearest(from, way, distance, &met);
}

/* Whether a ball or the block stands between a point and a point of the lamp, `far` away. */
static inline int
shadowed(struct vec from, struct vec way, float far)
{
	int i, first = pt.scene ? 2 : 0, last = pt.scene ? 3 : 2, side;
	float low = 0.0005f, high = far;

	if (!clear_of_things(from, way)) {
		for (i = first; i < last; i++) {
			struct vec to = sub(balls[i].centre, from);
			float along = dot(to, way), off2 = dot(to, to) - along * along, r2 = balls[i].radius * balls[i].radius;

			if (pt.material[i == 2 ? 1 : i] != PT_GLASS && along > 0.0f && off2 < r2 && along - __builtin_sqrtf(r2 - off2) < far && along > 0.001f)
				return 1;
		}
	} else if (!pt.scene) {
		return 0;
	}
	if (pt.scene)
		return pt.material[0] != PT_GLASS && mesh_between(&pt_meshes[pt.scene - 1], from, way, far);
	return block_pair(from.x, way.x, block_low.x, block_high.x, 0, &low, &high, &side) && block_pair(from.y, way.y, block_low.y, block_high.y, 1, &low, &high, &side) &&
	       block_pair(from.z, way.z, block_low.z, block_high.z, 2, &low, &high, &side) && low < high;
}

/* The light one path brings back; *rays counts its rays. */
static struct vec
path(struct vec from, struct vec way, uint32_t *state, uint32_t *rays)
{
	struct vec seen = v(0, 0, 0), weight = v(1, 1, 1);
	int bounce, straight = 1;	/* the last turn was a mirror's or the eye's: a lamp met now is seen */

	for (bounce = 0; bounce < pt.bounces; bounce++) {
		struct met met;
		struct vec at, normal, colour = white;
		float t;
		int material = PT_DIFFUSE, what;

		(*rays)++;
		what = nearest(from, way, &t, &met);
		if (what == MET_NONE)
			break;
		if (what == MET_LAMP) {
			if (pt.light > 0.0f) {
				if (straight)
					seen = add(seen, scale(weight, pt.light));
				break;
			}
			what = MET_CEILING;
		}
		at = add(from, scale(way, t));
		/* what was met, as a surface */
		if (what >= MET_MODEL) {
			normal = mesh_normal(&met, way);
			colour = of(pt_meshes[pt.scene - 1].colour);
			material = pt.material[0];
		} else if (what >= MET_BLOCK) {
			float out = (what - MET_BLOCK) & 1 ? 1.0f : -1.0f;

			normal = what < MET_BLOCK + 2 ? v(out, 0, 0) : what < MET_BLOCK + 4 ? v(0, out, 0) : v(0, 0, out);
			colour = v(0.72f, 0.68f, 0.50f);
		} else if (what >= MET_BALL) {
			int i = what - MET_BALL;

			normal = scale(sub(at, balls[i].centre), 1.0f / balls[i].radius);
			colour = v(0.92f, 0.92f, 0.92f);
			material = pt.material[i == 2 ? 1 : i];
		} else if (what == MET_LEFT) {
			normal = v(1, 0, 0), colour = red;
		} else if (what == MET_RIGHT) {
			normal = v(-1, 0, 0), colour = green;
		} else {
			normal = what == MET_FLOOR ? v(0, 1, 0) : what == MET_CEILING ? v(0, -1, 0) : v(0, 0, -1);
		}
		if (material == PT_MIRROR) {
			way = sub(way, scale(normal, 2.0f * dot(way, normal)));
			weight = mul(weight, colour);
			from = at;
			straight = 1;
			continue;
		}
		if (material == PT_GLASS) {
			/* in or out through the surface, or back off it: more of the latter the flatter the ray */
			float c = -dot(way, normal), ratio = 1.0f / 1.5f, k, mirror;

			if (c < 0.0f)
				c = -c, normal = scale(normal, -1.0f), ratio = 1.5f;
			k = 1.0f - ratio * ratio * (1.0f - c * c);
			mirror = 0.04f + 0.96f * (1.0f - c) * (1.0f - c) * (1.0f - c) * (1.0f - c) * (1.0f - c);
			if (k < 0.0f || chance(state) < mirror)
				way = add(way, scale(normal, 2.0f * c));
			else
				way = unit(add(scale(way, ratio), scale(normal, ratio * c - __builtin_sqrtf(k))));
			from = at;
			straight = 1;
			continue;
		}
		/* a matt surface: the lamp's light on it, by a ray to a point of the lamp */
		weight = mul(weight, colour);
		{
			struct vec lamp = v(LAMP_X0 + (LAMP_X1 - LAMP_X0) * chance(state), 0.999f, LAMP_Z0 + (LAMP_Z1 - LAMP_Z0) * chance(state));
			struct vec to = sub(lamp, at);
			float d2 = dot(to, to), d = __builtin_sqrtf(d2), facing, down;

			to = scale(to, 1.0f / d);
			facing = dot(normal, to);
			down = to.y;	/* the lamp faces down: how squarely this ray leaves it */
			(*rays)++;
			if (facing > 0.0f && down > 0.0f && !shadowed(at, to, d - 0.002f)) {
				float area = (LAMP_X1 - LAMP_X0) * (LAMP_Z1 - LAMP_Z0);

				seen = add(seen, scale(weight, pt.light * facing * down * area / (3.14159265f * d2)));
			}
		}
		/* and on in a direction that favours the surface's own */
		way = unit(add(normal, in_ball(state)));
		from = at;
		straight = 0;
		if (bounce >= 2) {
			/* a path that carries little ends by chance, and the others count for it */
			float keep = weight.x > weight.y ? (weight.x > weight.z ? weight.x : weight.z) : (weight.y > weight.z ? weight.y : weight.z);

			if (chance(state) >= keep)
				break;
			weight = scale(weight, 1.0f / keep);
		}
	}
	return seen;
}

static inline uint32_t
shown(float sum, float samples)
{
	float c = sum / samples;

	c = c <= 0.0f ? 0.0f : c >= 1.0f ? 1.0f : __builtin_sqrtf(c);	/* (the screen's curve, near enough) */
	return (uint32_t)(c * 255.0f);
}

static const uint32_t owner_colour[16] = {
	0xffffff, 0xff4040, 0x40ff40, 0x4080ff, 0xffff40, 0xff40ff, 0x40ffff, 0xff8000,
	0x80ff00, 0x0080ff, 0xff0080, 0x8000ff, 0x00ff80, 0xc0c0c0, 0xff8080, 0x80ff80,
};

/* One more sample of every pixel of a tile. how: the sample's number, and the core's in the top byte. */
uint32_t
pt_tile(uint32_t tile, uint32_t how)
{
	uint32_t sample = how & 0xffffff, core = how >> 24, rays = 0;
	int tw = pt.tile_w, th = pt.tile_h;
	int across = (pt.width + tw - 1) / tw, x0 = (int)(tile % (uint32_t)across) * tw, y0 = (int)(tile / (uint32_t)across) * th;
	int x1 = x0 + tw < pt.width ? x0 + tw : pt.width, y1 = y0 + th < pt.height ? y0 + th : pt.height, x, y;
	float half_w = (float)pt.width * 0.5f, half_h = (float)pt.height * 0.5f, zoom = 0.5f / half_h, samples = (float)(sample + 1);
	const struct vec eye = v(0.5f, 0.5f, -1.35f);

	for (y = y0; y < y1; y++) {
		float *sum = pt.sums + (y * pt.width + x0) * 3;
		uint32_t *out = pt.pixels + y * pt.width + x0;

		for (x = x0; x < x1; x++, sum += 3) {
			uint32_t state = ((uint32_t)(y * pt.width + x) * 9781u + sample * 6271u + pt.seed) * 2654435761u | 1u;
			float sx, sy;
			struct vec c;

			if (pt.depth) {
				/* (through the pixel's middle, and no light: what the tree of boxes finds) */
				struct vec way = unit(v(((float)x + 0.5f - half_w) * zoom, (half_h - (float)y - 0.5f) * zoom, 1.35f));
				float t;

				rays++;
				*out++ = nearest_once(eye, way, &t) ? (uint32_t)(t * 4000000.0f) & 0xffffff : 0;
				continue;
			}
			next(&state);
			/* through a point of the pixel, not always its middle: edges come out smooth */
			sx = ((float)x + chance(&state) - half_w) * zoom;
			sy = (half_h - (float)y - chance(&state)) * zoom;
			c = path(eye, unit(v(sx, sy, 1.35f)), &state, &rays);
			/* one path in thousands finds the lamp by a mirror or through glass and is worth a
			 * hundred others: held to what a bright wall is, or it is a white speck for good */
			if (c.x > PT_MOST) c.x = PT_MOST;
			if (c.y > PT_MOST) c.y = PT_MOST;
			if (c.z > PT_MOST) c.z = PT_MOST;
			if (sample == 0)
				sum[0] = c.x, sum[1] = c.y, sum[2] = c.z;
			else
				sum[0] += c.x, sum[1] += c.y, sum[2] += c.z;
			if (pt.owners && (x == x0 || y == y0))
				*out++ = owner_colour[core & 15];
			else
				*out++ = shown(sum[0], samples) << 16 | shown(sum[1], samples) << 8 | shown(sum[2], samples);
		}
	}
	return rays;
}

/* A worker's share of the picture (tracer.h): it is never without work while the picture is
 * traced. Handed a tile at a time, a worker waited some passes for its next. */
uint32_t
pt_share(uint32_t which, uint32_t how)
{
	uint32_t k = how >> 24, limit = how & 0xffffff, n = which >> 16, w = which & 0xffff, tiles = (uint32_t)pt.tiles, did = 0;
	struct pt_slot *slot = &pt.slots[k];
	uint32_t sample;

	for (sample = slot->samples; sample < (uint32_t)pt.samples && !pt.stop; sample++) {
		uint32_t j, at = w * pt.stride % tiles, rays = 0;

		for (j = w; j < tiles; j += n) {
			rays += pt_tile(at, sample | k << 24);
			if ((at += pt.step) >= tiles)
				at -= tiles;
		}
		slot->rays += rays;
		slot->samples = sample + 1;
		if (++did == limit || mcw_recalled((int)k))
			break;
	}
	return did;
}
