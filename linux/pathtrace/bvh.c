/* The world the path tracer traces (scene.h): every object's triangles where they stand, and
 * one tree of boxes over them, made by the worker cores (docs/machine/multicore.md). A job writes its
 * own triangles, boxes, numbers and nodes, each on 16 bytes of its own, and its stack. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "scene.h"
#include "mcw.h"

struct sc_world sc_world;

#define BINS 8			/* the places a box is tried cut at, less one, along an axis */
#define LEAF 4			/* a leaf's triangles at most */
#define NARROW 32		/* so few triangles are tried along their widest axis alone, */
#define BINS_NARROW 4		/* and at fewer places */
#define LEVELS 36		/* a part's tree at its deepest, and the tree over the parts: */
#define TOP_LEVELS 24		/* a ray's path down both, and a box more, is within the tracer's stack of 64 */
#define GRID_LEVELS 10		/* halvings of the world for a triangle's place in it */
#define GRID_FEW 64		/* an object of so few triangles does not make the grid larger */
#define PART_LEAST 256		/* a part has at most so many triangles, */
#define PART_SHARE 128		/* or this share of the world's when that is more */
#define JOB_LEAST 32		/* triangles that are worth a job of their own */

/* An object for the jobs: its mesh, where its triangles begin in the world, its matrix's three
 * rows and the matrix for its normals. */
struct placed {
	const float *positions, *normals;
	uint32_t first, count, object;
	float matrix[12], normal[9];
};

/* A part of the world with a tree of its own: `count` triangles from `first` of the order,
 * its numbers from `work` and its nodes from `nodes` while it grows, from `base` after. */
struct part { uint32_t first, count, work, nodes, node_count, base, unused[2]; };

/* Triangles a box is made round along an axis, for trying it cut between two of them. */
struct bin { float low[3], high[3]; uint32_t count; };
/* A box still to be cut: its node, its numbers, how deep it may go, and the box of its
 * triangles' middles (twice them: a low corner and a high one added). */
struct span { uint32_t node, first, count, room; float low[3], high[3]; };

/* What a build has, kept for the next. Written by the first core between two rounds of jobs. */
static struct {
	struct placed placed[SC_OBJECTS];
	float low[3], scale;		/* the grid: where it begins, and cells a unit of twice a middle */
	struct sc_triangle *made;	/* the triangles in the scene's order */
	struct sc_node *boxes;		/* each one's box; `a` is its place in the grid, `b` the halvings it is within */
	struct sc_node *grown;		/* the parts' trees before they are put together */
	struct sc_node *tops;		/* the parts' boxes */
	struct part *parts;
	uint32_t *order, *work, *top_order;
	uint32_t made_room, boxes_room, grown_room, tops_room, parts_room, order_room, work_room, top_order_room;
	uint32_t triangles_room, nodes_room, part_count;
} build;
static uint32_t cell_first[4099];

static inline float
least(float a, float b)
{
	return __builtin_fminf(a, b);
}

static inline float
most(float a, float b)
{
	return __builtin_fmaxf(a, b);
}

/* Memory for `wanted` things of `each` bytes, on 16 bytes and touched for the workers: what
 * there was when it is enough, or new of twice the size, with the old's contents if `keep`. */
static int
enough(void *memory, uint32_t *room, uint32_t wanted, uint32_t each, int keep)
{
	void *held, *fresh;
	uint32_t larger = *room * 2 > wanted ? *room * 2 : wanted;

	if (wanted <= *room)
		return 1;
	larger = larger < 64 ? 64 : larger;
	if (larger > 0x7fffffffu / each || posix_memalign(&fresh, 16, ((size_t)larger * each + 15) & ~(size_t)15))
		return 0;
	memcpy(&held, memory, sizeof held);
	if (keep && *room)
		memcpy(fresh, held, (size_t)*room * each);
	free(held);
	mcw_touch(fresh, (larger * each + 15) & ~15u);
	memcpy(memory, &fresh, sizeof fresh);
	*room = larger;
	return 1;
}

/* A number's ten bits, two zeros after each. */
static inline uint32_t
spread(uint32_t x)
{
	x = (x | x << 16) & 0x030000ffu;
	x = (x | x << 8) & 0x0300f00fu;
	x = (x | x << 4) & 0x030c30c3u;
	return (x | x << 2) & 0x09249249u;
}

static inline void
placed_point(const float *m, const float *p, float *out)
{
	out[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
	out[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
	out[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

/* A job: the world's triangles from `first` to `end` made of their objects' meshes, each with
 * its box, its place in the grid (a cell's three numbers, their bits in turn) and how many
 * times the grid is halved before its cells are smaller than the triangle. */
static __attribute__((noinline)) uint32_t
place_job(uint32_t first, uint32_t end)
{
	const struct placed *from = build.placed;
	uint32_t i;
	int k, n;

	for (i = first; i < end; i++) {
		struct sc_triangle *tri = &build.made[i];
		struct sc_node *box = &build.boxes[i];
		const float *p, *normals;
		float a[3], b[3], c[3], face[3], wide = 0.0f, size;
		uint32_t cell[3], halvings = 0;

		while (i >= from->first + from->count)
			from++;
		p = from->positions + (i - from->first) * 9, normals = from->normals + (i - from->first) * 9;
		placed_point(from->matrix, p, a), placed_point(from->matrix, p + 3, b), placed_point(from->matrix, p + 6, c);
		for (k = 0; k < 3; k++) {
			float low = least(a[k], least(b[k], c[k])), high = most(a[k], most(b[k], c[k])), at = (low + high - build.low[k]) * build.scale;

			tri->corner[k] = a[k], tri->edge1[k] = b[k] - a[k], tri->edge2[k] = c[k] - a[k];
			box->low[k] = low, box->high[k] = high;
			cell[k] = (uint32_t)(int)least(most(at, 0.0f), 1023.0f);
			/* (one outside the grid is as one too large for it) */
			wide = most(wide, at < 0.0f || at >= 1024.0f ? 1024.0f : (high - low) * 2.0f * build.scale);
		}
		for (size = 512.0f; halvings < GRID_LEVELS && wide <= size; size *= 0.5f)
			halvings++;
		box->a = spread(cell[0]) << 2 | spread(cell[1]) << 1 | spread(cell[2]);
		box->b = halvings;
		face[0] = tri->edge1[1] * tri->edge2[2] - tri->edge1[2] * tri->edge2[1];
		face[1] = tri->edge1[2] * tri->edge2[0] - tri->edge1[0] * tri->edge2[2];
		face[2] = tri->edge1[0] * tri->edge2[1] - tri->edge1[1] * tri->edge2[0];
		for (n = 0; n < 3; n++) {
			const float *m = from->normal, *q = normals + n * 3;
			float x = m[0] * q[0] + m[1] * q[1] + m[2] * q[2], y = m[3] * q[0] + m[4] * q[1] + m[5] * q[2], z = m[6] * q[0] + m[7] * q[1] + m[8] * q[2];
			float d = x * x + y * y + z * z;

			/* (a normal of no length: the face's own, or straight up) */
			if (!(d > 0.0f)) {
				x = face[0], y = face[1], z = face[2];
				d = x * x + y * y + z * z;
			}
			if (!(d > 0.0f))
				x = 0.0f, y = 1.0f, z = 0.0f, d = 1.0f;
			d = 1.0f / __builtin_sqrtf(d);
			tri->normal[n][0] = x * d, tri->normal[n][1] = y * d, tri->normal[n][2] = z * d;
		}
		tri->object = from->object, tri->unused = 0;
	}
	return 0;
}

static inline float
half_area(const float *low, const float *high)
{
	float x = high[0] - low[0], y = high[1] - low[1], z = high[2] - low[2];

	return x * y + y * z + z * x;
}

static inline void
bin_add(struct bin *bin, const struct sc_node *box)
{
	bin->count++;
	bin->low[0] = least(bin->low[0], box->low[0]), bin->low[1] = least(bin->low[1], box->low[1]), bin->low[2] = least(bin->low[2], box->low[2]);
	bin->high[0] = most(bin->high[0], box->high[0]), bin->high[1] = most(bin->high[1], box->high[1]), bin->high[2] = most(bin->high[2], box->high[2]);
}

/* The box of some boxes, by their numbers, and the box of their middles (twice them). */
static void
around(const struct sc_node *boxes, const uint32_t *ids, uint32_t count, struct sc_node *all, struct span *middles)
{
	uint32_t i;
	int k;

	for (k = 0; k < 3; k++)
		all->low[k] = middles->low[k] = 1e30f, all->high[k] = middles->high[k] = -1e30f;
	for (i = 0; i < count; i++) {
		const struct sc_node *box = &boxes[ids[i]];

		for (k = 0; k < 3; k++) {
			float middle = box->low[k] + box->high[k];

			all->low[k] = least(all->low[k], box->low[k]), all->high[k] = most(all->high[k], box->high[k]);
			middles->low[k] = least(middles->low[k], middle), middles->high[k] = most(middles->high[k], middle);
		}
	}
}

/* A tree over `count` boxes, named by ids[]: nodes from nodes[0] on, a box's two together, at
 * most `leaf` boxes a leaf and `room` deep; ids[] is put in the leaves' order and a leaf's `a`
 * is where its numbers begin there. Returns the nodes made. A box is cut where its two parts'
 * areas times their numbers are least (the binned SAH); a tree of one box a leaf tries all axes. */
static __attribute__((noinline)) uint32_t
grow(struct sc_node *nodes, uint32_t *ids, const struct sc_node *boxes, uint32_t count, uint32_t leaf, uint32_t room)
{
	struct span stack[LEVELS + 2], now = {0, 0, count, room, {0, 0, 0}, {0, 0, 0}}, left, right;
	struct bin bins[3][BINS];
	uint32_t made = 1;
	int held = 0, k, j;

	around(boxes, ids, count, &nodes[0], &now);
	for (;;) {
		struct sc_node *node = &nodes[now.node], *below;
		uint32_t *mine = ids + now.first, i, end, left_count = 0, limit;
		float scale[3], best = 1e30f, wide = -1.0f, right_area[BINS];
		uint32_t right_count[BINS];
		int first_axis = 0, last_axis = 2, axis = -1, cut = 0, narrow = leaf > 1 && now.count <= NARROW, places = narrow ? BINS_NARROW : BINS;

		if (now.count <= leaf) {
			node->a = now.first, node->b = now.count;
			if (!held)
				return made;
			now = stack[--held];
			continue;
		}
		for (k = 0; k < 3; k++) {
			float width = now.high[k] - now.low[k];

			scale[k] = width > 0.0f ? (float)places * 0.999f / width : 0.0f;
			if (narrow && width > wide)
				wide = width, first_axis = last_axis = k;
		}
		for (k = first_axis; k <= last_axis; k++)
			for (j = 0; j < places; j++) {
				struct bin *bin = &bins[k][j];

				bin->count = 0;
				bin->low[0] = bin->low[1] = bin->low[2] = 1e30f, bin->high[0] = bin->high[1] = bin->high[2] = -1e30f;
			}
		if (first_axis == last_axis) {
			k = first_axis;
			for (i = 0; i < now.count; i++) {
				const struct sc_node *box = &boxes[mine[i]];

				bin_add(&bins[k][(int)((box->low[k] + box->high[k] - now.low[k]) * scale[k])], box);
			}
		} else {
			for (i = 0; i < now.count; i++) {
				const struct sc_node *box = &boxes[mine[i]];

				bin_add(&bins[0][(int)((box->low[0] + box->high[0] - now.low[0]) * scale[0])], box);
				bin_add(&bins[1][(int)((box->low[1] + box->high[1] - now.low[1]) * scale[1])], box);
				bin_add(&bins[2][(int)((box->low[2] + box->high[2] - now.low[2]) * scale[2])], box);
			}
		}
		/* every cut of every axis: what is right of it from the far end, then what is left */
		for (k = first_axis; k <= last_axis; k++) {
			struct bin all = {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, 0};

			for (j = places - 1; j > 0; j--) {
				const struct bin *bin = &bins[k][j];

				all.count += bin->count;
				all.low[0] = least(all.low[0], bin->low[0]), all.low[1] = least(all.low[1], bin->low[1]), all.low[2] = least(all.low[2], bin->low[2]);
				all.high[0] = most(all.high[0], bin->high[0]), all.high[1] = most(all.high[1], bin->high[1]), all.high[2] = most(all.high[2], bin->high[2]);
				right_count[j] = all.count, right_area[j] = half_area(all.low, all.high);
			}
			all.count = 0;
			all.low[0] = all.low[1] = all.low[2] = 1e30f, all.high[0] = all.high[1] = all.high[2] = -1e30f;
			for (j = 1; j < places; j++) {
				const struct bin *bin = &bins[k][j - 1];
				float cost;

				all.count += bin->count;
				all.low[0] = least(all.low[0], bin->low[0]), all.low[1] = least(all.low[1], bin->low[1]), all.low[2] = least(all.low[2], bin->low[2]);
				all.high[0] = most(all.high[0], bin->high[0]), all.high[1] = most(all.high[1], bin->high[1]), all.high[2] = most(all.high[2], bin->high[2]);
				if (!all.count || !right_count[j])
					continue;
				cost = half_area(all.low, all.high) * (float)all.count + right_area[j] * (float)right_count[j];
				if (cost < best)
					best = cost, axis = k, cut = j, left_count = all.count;
			}
		}
		node->a = made, node->b = 0;
		below = &nodes[made];
		left.node = made, right.node = made + 1, left.room = right.room = now.room - 1;
		made += 2;
		/* (a part too large for the levels left under it would never fit: halves then) */
		limit = now.room - 1 < 24 ? leaf << (now.room - 1) : ~0u;
		if (axis >= 0 && left_count <= limit && now.count - left_count <= limit) {
			float from = now.low[axis], by = scale[axis];
			float lx0 = 1e30f, ly0 = 1e30f, lz0 = 1e30f, lx1 = -1e30f, ly1 = -1e30f, lz1 = -1e30f;
			float rx0 = 1e30f, ry0 = 1e30f, rz0 = 1e30f, rx1 = -1e30f, ry1 = -1e30f, rz1 = -1e30f;

			for (k = 0; k < 3; k++)
				below[0].low[k] = below[1].low[k] = 1e30f, below[0].high[k] = below[1].high[k] = -1e30f;
			for (j = 0; j < places; j++) {
				const struct bin *bin = &bins[axis][j];
				struct sc_node *side = &below[j >= cut];

				for (k = 0; k < 3; k++)
					side->low[k] = least(side->low[k], bin->low[k]), side->high[k] = most(side->high[k], bin->high[k]);
			}
			/* the numbers of the left go first, and each side's middles get their box */
			for (i = 0, end = now.count; i < end;) {
				uint32_t id = mine[i];
				const struct sc_node *box = &boxes[id];
				float x = box->low[0] + box->high[0], y = box->low[1] + box->high[1], z = box->low[2] + box->high[2];

				if ((int)(((axis == 0 ? x : axis == 1 ? y : z) - from) * by) >= cut) {
					mine[i] = mine[--end], mine[end] = id;
					rx0 = least(rx0, x), ry0 = least(ry0, y), rz0 = least(rz0, z);
					rx1 = most(rx1, x), ry1 = most(ry1, y), rz1 = most(rz1, z);
				} else {
					i++;
					lx0 = least(lx0, x), ly0 = least(ly0, y), lz0 = least(lz0, z);
					lx1 = most(lx1, x), ly1 = most(ly1, y), lz1 = most(lz1, z);
				}
			}
			left.low[0] = lx0, left.low[1] = ly0, left.low[2] = lz0, left.high[0] = lx1, left.high[1] = ly1, left.high[2] = lz1;
			right.low[0] = rx0, right.low[1] = ry0, right.low[2] = rz0, right.high[0] = rx1, right.high[1] = ry1, right.high[2] = rz1;
		} else {
			left_count = now.count - now.count / 2;
			around(boxes, mine, left_count, &below[0], &left);
			around(boxes, mine + left_count, now.count - left_count, &below[1], &right);
		}
		left.first = now.first, left.count = left_count;
		right.first = now.first + left_count, right.count = now.count - left_count;
		stack[held++] = right;
		now = left;
	}
}

/* A job: the trees of the parts from `first` to `end`, each over its own numbers. */
static __attribute__((noinline)) uint32_t
grow_job(uint32_t first, uint32_t end)
{
	uint32_t at, i;

	for (at = first; at < end; at++) {
		struct part *part = &build.parts[at];
		uint32_t *ids = build.work + part->work;
		const uint32_t *from = build.order + part->first;

		for (i = 0; i < part->count; i++)
			ids[i] = from[i];
		part->node_count = grow(build.grown + part->nodes, ids, build.boxes, part->count, LEAF, LEVELS);
	}
	return 0;
}

/* A node of a part's tree as the world has it: its triangles, or its two boxes, where they are there. */
static inline void
node_put(struct sc_node *to, const struct sc_node *from, const struct part *part)
{
	int k;

	for (k = 0; k < 3; k++)
		to->low[k] = from->low[k], to->high[k] = from->high[k];
	to->a = from->b ? part->first + from->a : part->base + from->a - 1;
	to->b = from->b;
}

/* A job: the parts from `first` to `end` put into the world, their triangles in the leaves'
 * order and their nodes but each one's first, which is a leaf of the tree over the parts. */
static __attribute__((noinline)) uint32_t
put_job(uint32_t first, uint32_t end)
{
	uint32_t at, i;

	for (at = first; at < end; at++) {
		const struct part *part = &build.parts[at];
		const uint32_t *ids = build.work + part->work;
		const struct sc_node *from = build.grown + part->nodes;
		struct sc_triangle *to = sc_world.triangles + part->first;

		for (i = 0; i < part->count; i++)
			to[i] = build.made[ids[i]];
		for (i = 1; i < part->node_count; i++)
			node_put(&sc_world.nodes[part->base + i - 1], &from[i], part);
	}
	return 0;
}

/* One more part: the triangles of the order from `first` to `end`. */
static int
part_add(uint32_t first, uint32_t end)
{
	struct part *part;

	if (first == end)
		return 1;
	if (!enough(&build.parts, &build.parts_room, build.part_count + 1, sizeof(struct part), 1))
		return 0;
	part = &build.parts[build.part_count++];
	part->first = first, part->count = end - first;
	return 1;
}

/* Which of a cube's nine lots a triangle is of: the cube's own when the cubes inside it are
 * smaller than the triangle, else one of those eight's. */
static inline uint32_t
lot_of(const struct sc_node *box, int level, int shift)
{
	return box->b <= (uint32_t)level ? 0 : 1 + (box->a >> shift & 7);
}

/* The triangles of a cube of the grid, the order's from `first` to `end`, as parts of at most
 * `limit`: its own a part (a part's box is then little more than its cube), its eight cubes
 * each a part, small ones side by side together, a large one cut again. `cell` is the cube's
 * number among those of its size (`level` halvings). */
static int
parts_of(uint32_t first, uint32_t end, uint32_t cell, int level, uint32_t limit)
{
	uint32_t starts[10], count[9] = {0}, i, held, at;
	int shift = 27 - 3 * level, j;

	if (level == GRID_LEVELS)
		return part_add(first, end);
	if (level < 4) {
		/* (the order is by the first four halvings already, and has none of the cube's own) */
		for (j = 0; j <= 8; j++)
			starts[j + 1] = cell_first[1 + ((cell * 8 + (uint32_t)j) << (9 - 3 * level))];
		starts[0] = starts[1];
	} else {
		for (i = first; i < end; i++)
			count[lot_of(&build.boxes[build.order[i]], level, shift)]++;
		for (j = 0, at = first; j < 9; at += count[j++])
			starts[j] = at;
		starts[9] = end;
		for (i = first; i < end; i++)
			build.work[i] = build.order[i];
		for (j = 0; j < 9; j++)
			count[j] = starts[j];
		for (i = first; i < end; i++)
			build.order[count[lot_of(&build.boxes[build.work[i]], level, shift)]++] = build.work[i];
	}
	if (!part_add(starts[0], starts[1]))
		return 0;
	held = starts[1];
	for (j = 1; j <= 8; j++) {
		if (starts[j + 1] - starts[j] > limit) {
			if (!part_add(held, starts[j]) || !parts_of(starts[j], starts[j + 1], cell * 8 + (uint32_t)j - 1, level + 1, limit))
				return 0;
			held = starts[j + 1];
		} else if (starts[j + 1] - held > limit) {
			if (!part_add(held, starts[j]))
				return 0;
			held = starts[j];
		}
	}
	return part_add(held, end);
}

/* The scene's objects as the jobs want them, and the grid. Returns the triangles. */
static uint32_t
place_objects(void)
{
	float all[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f}, many[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f}, wide = 0.0f;
	struct placed *placed = build.placed;
	uint32_t count = 0;
	int i, k, corner;

	for (i = 0; i < sc.object_count && i < SC_OBJECTS; i++) {
		const struct sc_object *object = &sc.objects[i];
		const struct sc_mesh *mesh;
		const float *m = object->matrix;
		float det, sign, *n = placed->normal, box[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};

		if (object->mesh < 0 || object->mesh >= SC_MESHES)
			continue;
		mesh = &sc.meshes[object->mesh];
		if (!mesh->positions || mesh->triangles <= 0)
			continue;
		placed->positions = mesh->positions, placed->normals = mesh->normals;
		placed->first = count, placed->count = (uint32_t)mesh->triangles, placed->object = (uint32_t)i;
		for (k = 0; k < 3; k++) {
			placed->matrix[k * 4] = m[k], placed->matrix[k * 4 + 1] = m[4 + k];
			placed->matrix[k * 4 + 2] = m[8 + k], placed->matrix[k * 4 + 3] = m[12 + k];
		}
		/* a normal goes by the matrix turned inside out and over: these, but for their size */
		n[0] = m[5] * m[10] - m[6] * m[9], n[1] = m[2] * m[9] - m[1] * m[10], n[2] = m[1] * m[6] - m[2] * m[5];
		n[3] = m[6] * m[8] - m[4] * m[10], n[4] = m[0] * m[10] - m[2] * m[8], n[5] = m[2] * m[4] - m[0] * m[6];
		n[6] = m[4] * m[9] - m[5] * m[8], n[7] = m[1] * m[8] - m[0] * m[9], n[8] = m[0] * m[5] - m[1] * m[4];
		det = m[0] * n[0] + m[4] * n[1] + m[8] * n[2];
		sign = det < 0.0f ? -1.0f : 1.0f;
		for (k = 0; k < 9; k++)
			n[k] *= sign;
		for (corner = 0; corner < 8; corner++) {
			float p[3] = {corner & 1 ? mesh->high[0] : mesh->low[0], corner & 2 ? mesh->high[1] : mesh->low[1], corner & 4 ? mesh->high[2] : mesh->low[2]}, at[3];

			placed_point(placed->matrix, p, at);
			for (k = 0; k < 3; k++)
				box[k] = least(box[k], at[k]), box[3 + k] = most(box[3 + k], at[k]);
		}
		/* (the grid is round the objects of many triangles: a floor of two would make it coarse) */
		for (k = 0; k < 3; k++) {
			all[k] = least(all[k], box[k]), all[3 + k] = most(all[3 + k], box[3 + k]);
			if (placed->count > GRID_FEW)
				many[k] = least(many[k], box[k]), many[3 + k] = most(many[3 + k], box[3 + k]);
		}
		count += placed->count;
		placed++;
	}
	/* (cells as long as they are wide: the grid is as large as its longest side) */
	for (k = 0; k < 3; k++) {
		const float *round = many[0] <= many[3] ? many : all;

		build.low[k] = 2.0f * round[k];
		wide = most(wide, round[3 + k] - round[k]);
	}
	build.scale = wide > 0.0f ? 1023.0f / (2.0f * wide) : 0.0f;
	return count;
}

/* Where a triangle is in the first order: before the cells when one is smaller than it. */
static inline uint32_t
cell_of(const struct sc_node *box)
{
	return box->b < 4 ? 0 : 1 + (box->a >> 18);
}

/* The order: the triangles' numbers by the cell of the grid's first four halvings each is in,
 * as they come within one, the large ones first; cell_first[c + 1] is where cell c's begin. */
static void
order_by_cell(uint32_t count)
{
	uint32_t i, sum = 0;

	memset(cell_first, 0, sizeof cell_first);
	for (i = 0; i < count; i++)
		cell_first[cell_of(&build.boxes[i]) + 2]++;
	for (i = 2; i < 4099; i++) {
		uint32_t here = cell_first[i];

		cell_first[i - 1] = sum, sum += here;
	}
	for (i = 0; i < count; i++)
		build.order[cell_first[cell_of(&build.boxes[i]) + 1]++] = i;
}

/* The code of the jobs is read: a worker stops at a page of it that no core has run or read. */
static void
read_code(void)
{
	extern char __executable_start[], etext[];
	uintptr_t at[4] = {(uintptr_t)place_job, (uintptr_t)grow_job, (uintptr_t)put_job, (uintptr_t)grow}, low = at[0], high = at[0], page;
	int k;

	for (k = 1; k < 4; k++) {
		low = at[k] < low ? at[k] : low;
		high = at[k] > high ? at[k] : high;
	}
	low &= ~(uintptr_t)4095, high += 0x3000;
	low = low - (uintptr_t)__executable_start > 0x2000 ? low - 0x2000 : (uintptr_t)__executable_start;
	if (high > (uintptr_t)etext)
		high = (uintptr_t)etext;
	for (page = low; page < high; page += 4096)
		(void)*(const volatile char *)page;
}

/* A round of jobs: job j has from cut[j] to cut[j + 1], the first is this core's own and the
 * others the workers' from 1. given(): they are handed out; taken(): they are all done. */
static void
given(mcw_fn job, const uint32_t *cut, int jobs)
{
	int j;

	for (j = 1; j < jobs; j++)
		if (cut[j] < cut[j + 1])
			mcw_post(j, job, cut[j], cut[j + 1]);
	if (cut[0] < cut[1])
		job(cut[0], cut[1]);
}

static void
taken(const uint32_t *cut, int jobs)
{
	int j;

	for (j = 1; j < jobs; j++)
		if (cut[j] < cut[j + 1])
			mcw_wait(j);
}

/* The parts shared out between jobs, as even in triangles as whole parts allow; none for this
 * core when `spare`. */
static void
parts_cut(uint32_t *cut, int jobs, uint32_t triangles, int spare)
{
	uint32_t at = 0, sum = 0, takers = (uint32_t)(jobs - spare), each = triangles / takers, over = triangles % takers;
	int j;

	cut[0] = 0;
	for (j = 1; j <= jobs; j++) {
		uint32_t taker = (uint32_t)(j > spare ? j - spare : 0), until = each * taker + (taker < over ? taker : over);

		for (; at < build.part_count && (sum < until || j == jobs); at++)
			sum += build.parts[at].count;
		cut[j] = at;
	}
}

/* The tree over the parts, in the world's first nodes: a leaf of it is a part's first node.
 * With `apart` the first part, the large triangles', is one of the first box's two and the
 * tree over the others the second: by the parts' middles it would go far down among them. */
static void
tree_over_parts(uint32_t apart)
{
	uint32_t others = build.part_count - apart, first = 2 * apart, top = build.part_count * 2 - 1, i;
	struct sc_node *nodes = sc_world.nodes;
	int k;

	for (i = 0; i < others; i++)
		build.top_order[i] = i + apart;
	grow(nodes + first, build.top_order, build.tops, others, 1, TOP_LEVELS);
	for (i = first; i < top; i++) {
		if (nodes[i].b) {
			const struct part *part = &build.parts[build.top_order[nodes[i].a]];

			node_put(&nodes[i], &build.grown[part->nodes], part);
		} else {
			nodes[i].a += first;
		}
	}
	if (!apart)
		return;
	node_put(&nodes[1], &build.grown[build.parts[0].nodes], &build.parts[0]);
	for (k = 0; k < 3; k++)
		nodes[0].low[k] = least(nodes[1].low[k], nodes[2].low[k]), nodes[0].high[k] = most(nodes[1].high[k], nodes[2].high[k]);
	nodes[0].a = 1, nodes[0].b = 0;
}

/* A world of nothing: a leaf of one triangle that has no size, which no ray meets. */
static int
world_of_nothing(int made)
{
	sc_world.triangle_count = 0, sc_world.node_count = 1;
	sc_world.build_ms = sc_world.build_workers = 0;
	if (!sc_world.nodes || !sc_world.triangles)
		return 0;
	memset(&sc_world.triangles[0], 0, sizeof sc_world.triangles[0]);
	memset(&sc_world.nodes[0], 0, sizeof sc_world.nodes[0]);
	sc_world.nodes[0].b = 1;
	return made;
}

int
sc_world_build(int workers)
{
	struct timespec began, ended;
	uint32_t cut[258], count, limit, large, at, work = 0, nodes = 0;
	int jobs, j, used = 0;

	clock_gettime(CLOCK_MONOTONIC, &began);
	workers = workers < mcw_count() ? workers : mcw_count();
	workers = workers < 0 ? 0 : workers > 255 ? 255 : workers;
	if (workers)
		read_code();
	count = place_objects();
	if (!enough(&build.made, &build.made_room, count, sizeof(struct sc_triangle), 0) || !enough(&build.boxes, &build.boxes_room, count, sizeof(struct sc_node), 0) ||
	    !enough(&build.order, &build.order_room, count, sizeof(uint32_t), 0) || !enough(&build.work, &build.work_room, count, sizeof(uint32_t), 0) ||
	    !enough(&build.grown, &build.grown_room, count * 2, sizeof(struct sc_node), 0) || !enough(&sc_world.triangles, &build.triangles_room, count ? count : 1, sizeof(struct sc_triangle), 0) ||
	    !enough(&sc_world.nodes, &build.nodes_room, 1, sizeof(struct sc_node), 0))
		return world_of_nothing(0);
	if (!count)
		return world_of_nothing(1);
	/* the triangles, in even shares */
	jobs = (int)(count / JOB_LEAST) < workers ? (int)(count / JOB_LEAST) + 1 : workers + 1;
	for (j = 0; j <= jobs; j++)
		cut[j] = count / (uint32_t)jobs * (uint32_t)j + ((uint32_t)j < count % (uint32_t)jobs ? (uint32_t)j : count % (uint32_t)jobs);
	given(place_job, cut, jobs);
	taken(cut, jobs);
	used = jobs - 1;
	/* the parts, and each one's tree */
	order_by_cell(count);
	limit = count / PART_SHARE > PART_LEAST ? count / PART_SHARE : PART_LEAST;
	build.part_count = 0;
	large = count <= limit ? count : cell_first[1];
	if (!part_add(0, large) || !(count - large <= limit ? part_add(large, count) : parts_of(large, count, 0, 0, limit)) ||
	    !enough(&build.work, &build.work_room, count + 3 * build.part_count, sizeof(uint32_t), 0) ||
	    !enough(&build.tops, &build.tops_room, build.part_count, sizeof(struct sc_node), 0) ||
	    !enough(&build.top_order, &build.top_order_room, build.part_count, sizeof(uint32_t), 0))
		return world_of_nothing(0);
	for (at = 0; at < build.part_count; at++) {
		struct part *part = &build.parts[at];

		part->work = work, part->nodes = nodes;
		work += (part->count + 3) & ~3u, nodes += part->count * 2 - 1;
	}
	jobs = (int)build.part_count < workers + 1 ? (int)build.part_count : workers + 1;
	parts_cut(cut, jobs, count, 0);
	given(grow_job, cut, jobs);
	taken(cut, jobs);
	used = jobs - 1 > used ? jobs - 1 : used;
	/* the world: the parts put into it by the workers, and the tree over them made here */
	for (at = 0, nodes = build.part_count * 2 - 1; at < build.part_count; at++) {
		build.parts[at].base = nodes;
		nodes += build.parts[at].node_count - 1;
		build.tops[at] = build.grown[build.parts[at].nodes];
	}
	if (!enough(&sc_world.nodes, &build.nodes_room, nodes, sizeof(struct sc_node), 0))
		return world_of_nothing(0);
	sc_world.triangle_count = (int)count, sc_world.node_count = (int)nodes;
	jobs = (int)build.part_count < workers ? (int)build.part_count + 1 : workers + 1;
	parts_cut(cut, jobs, count, workers > 0);
	given(put_job, cut, jobs);
	tree_over_parts(large && large < count);
	taken(cut, jobs);
	clock_gettime(CLOCK_MONOTONIC, &ended);
	sc_world.build_ms = (uint32_t)(ended.tv_sec - began.tv_sec) * 1000u + (uint32_t)((ended.tv_nsec - began.tv_nsec) / 1000000);
	sc_world.build_workers = (uint32_t)used;
	return 1;
}

int
sc_world_nearest_slow(const float *from, const float *way, float *distance)
{
	float best = 1e30f;
	int i, found = -1;

	for (i = 0; i < sc_world.triangle_count; i++) {
		const struct sc_triangle *tri = &sc_world.triangles[i];
		const float *e1 = tri->edge1, *e2 = tri->edge2;
		float p[3] = {way[1] * e2[2] - way[2] * e2[1], way[2] * e2[0] - way[0] * e2[2], way[0] * e2[1] - way[1] * e2[0]};
		float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2], inv, s[3], q[3], u, w, t;

		if (det > -1e-12f && det < 1e-12f)
			continue;
		inv = 1.0f / det;
		s[0] = from[0] - tri->corner[0], s[1] = from[1] - tri->corner[1], s[2] = from[2] - tri->corner[2];
		u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
		if (u < 0.0f || u > 1.0f)
			continue;
		q[0] = s[1] * e1[2] - s[2] * e1[1], q[1] = s[2] * e1[0] - s[0] * e1[2], q[2] = s[0] * e1[1] - s[1] * e1[0];
		w = (way[0] * q[0] + way[1] * q[1] + way[2] * q[2]) * inv;
		if (w < 0.0f || u + w > 1.0f)
			continue;
		t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
		if (t > 0.0005f && t < best)
			best = t, found = i;
	}
	if (found >= 0)
		*distance = best;
	return found;
}
