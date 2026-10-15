/* Rigid boxes: physics.h, docs/physics.md. Positions are corrected, not velocities, and each
 * body corrects only itself: both bodies of a pair work the pair out, each for its own part. */
#include <stddef.h>
#include <string.h>
#include "physics.h"

#define MARGIN 0.03f			/* contacts are kept from this far apart, and further for fast bodies */
#define SINK (2.0f * PHYS_DT)		/* how fast a body leaves what it was in already */
#define FRICTION 0.6f
#define SLOW 0.2f			/* a body under this speed is taken to stay where it is, */
#define FAST 1.0f			/* and one over this to go on as it went */
#define MAX_SPEED 40.0f
#define MAX_SPIN 30.0f
#define MOST_POINTS 32
/* A point's depth guessed from the bodies' own frames is the real one to within this and a
 * hundred-thousandth of the bodies' places and sizes: some ten times what rounding can come to. */
#define SURE 1e-4f
#define SURE_OF 1e-5f
/* A body slower than SLEEP_SPEED and turning slower than SLEEP_SPIN for PHYS_SLEEP_AFTER
 * substeps sleeps: it stays where it is until a body faster than WAKE (or turning faster
 * than WAKE_SPIN, or held) comes within its reach. */
#define SLEEP_SPEED 0.05f
#define SLEEP_SPIN 0.1f
#define WAKE 0.2f
#define WAKE_SPIN 1.0f

typedef struct { float x, y, z; } v3;
typedef struct { v3 c[3]; } m3;		/* a rotation's columns: the body's axes in the room */
struct pose { v3 x; m3 r; float q[4]; };
struct place { const v3 *x; const m3 *r; };	/* where a body is and its axes, wherever those are kept */
/* A contact point as its body sees it for a substep: the point in the body's frame, the place
 * on the other it was `depth` past along n, how much of that is the body's to undo (`mine` of
 * what the substep brought, `later` of the rest), and the push it has had since it began. */
struct point { v3 local, on, n, was, turn; float depth, mine, later, share, had, over, over_new, push, gives, *kept; };

static inline v3 v(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static inline v3 add(v3 a, v3 b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 sub(v3 a, v3 b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 scale(v3 a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 cross(v3 a, v3 b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline v3 of(const float *f) { return v(f[0], f[1], f[2]); }
static inline float size(float f) { return f < 0.0f ? -f : f; }
static inline v3 rot(const m3 *m, v3 a) { return add(add(scale(m->c[0], a.x), scale(m->c[1], a.y)), scale(m->c[2], a.z)); }
static inline v3 unrot(const m3 *m, v3 a) { return v(dot(m->c[0], a), dot(m->c[1], a), dot(m->c[2], a)); }
static inline uint32_t bits(float f) { union { float f; uint32_t u; } c = {f}; return c.u; }

static inline m3
matrix_of(const float *q)
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	m3 m;

	m.c[0] = v(1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z), 2.0f * (x * z - w * y));
	m.c[1] = v(2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x));
	m.c[2] = v(2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y));
	return m;
}

/* q turned by the small rotation `turn` (an axis times an angle), at unit length again. */
static inline void
turned(const float *q, v3 turn, float *to)
{
	float x = q[0] + 0.5f * (turn.x * q[3] + turn.y * q[2] - turn.z * q[1]);
	float y = q[1] + 0.5f * (turn.y * q[3] + turn.z * q[0] - turn.x * q[2]);
	float z = q[2] + 0.5f * (turn.z * q[3] + turn.x * q[1] - turn.y * q[0]);
	float w = q[3] - 0.5f * (turn.x * q[0] + turn.y * q[1] + turn.z * q[2]);
	float inv = 1.0f / __builtin_sqrtf(x * x + y * y + z * z + w * w);

	to[0] = x * inv, to[1] = y * inv, to[2] = z * inv, to[3] = w * inv;
}

static inline void
pose_now(const struct body_state *s, struct pose *p)
{
	p->x = of(s->pos);
	p->q[0] = s->q[0], p->q[1] = s->q[1], p->q[2] = s->q[2], p->q[3] = s->q[3];
	p->r = matrix_of(p->q);
}

/* Where a body would be a substep on with nothing in its way. Whoever asks gets the same bits. */
static inline void
pose_next(const struct body_state *s, float gravity, struct pose *p)
{
	p->x = v(s->pos[0] + s->vel[0] * PHYS_DT, s->pos[1] + (s->vel[1] - gravity * PHYS_DT) * PHYS_DT, s->pos[2] + s->vel[2] * PHYS_DT);
	turned(s->q, scale(of(s->spin), PHYS_DT), p->q);
	p->r = matrix_of(p->q);
}

/* Where another body is taken to be a substep on. A slow one where it is: guessed on by its
 * last move, two bodies at rest each answer the other's answer, and a wall of them shakes.
 * A fast one goes on as it went (a thrown box's speed is the brick's it hits), and one that
 * nothing holds falls as well. Whoever asks gets the same bits. */
static inline void
pose_guess(const struct body_state *s, float gravity, struct pose *p)
{
	v3 vel = of(s->vel);
	float speed2 = dot(vel, vel), part = speed2 <= SLOW * SLOW ? 0.0f : speed2 >= FAST * FAST ? 1.0f : (__builtin_sqrtf(speed2) - SLOW) * (1.0f / (FAST - SLOW));
	float step = part * PHYS_DT;

	p->x = v(s->pos[0] + vel.x * step, s->pos[1] + vel.y * step - (s->held ? 0.0f : gravity * PHYS_DT * PHYS_DT), s->pos[2] + vel.z * step);
	turned(s->q, scale(of(s->spin), step), p->q);
	p->r = matrix_of(p->q);
}

static inline void
shifted(struct pose *p, v3 move, v3 turn)
{
	p->x = add(p->x, move);
	turned(p->q, turn, p->q);
	p->r = matrix_of(p->q);
}

/* What a push of one unit at `arm` from a body's middle along `way` moves that point by. */
static inline float
give(const m3 *r, v3 inv_inertia, float inv_mass, v3 arm, v3 way, v3 *turn)
{
	v3 c = unrot(r, cross(arm, way));
	v3 t = v(c.x * inv_inertia.x, c.y * inv_inertia.y, c.z * inv_inertia.z);

	*turn = rot(r, t);
	return inv_mass + dot(c, t);
}

/* A polygon cut to between two planes, |(p - origin) . axis| <= limit: first the side along
 * the axis, then the other. Its corners are in *from and *to is room for as many and two;
 * the two change places for each side that cuts. How many corners are left. */
static int
clip(v3 **from, v3 **to, int n, v3 origin, v3 axis, float limit)
{
	float along[10], past[10];
	int side, i, cuts, m;

	for (i = 0; i < n; i++)
		along[i] = dot(sub((*from)[i], origin), axis);
	for (side = 0; side < 2; side++) {
		const v3 *in = *from;
		v3 *out = *to;

		for (cuts = 0, i = 0; i < n; i++) {
			past[i] = (side ? -along[i] : along[i]) - limit;
			cuts |= past[i] > 0.0f;
		}
		if (!cuts)
			continue;
		for (m = 0, i = 0; i < n; i++) {
			v3 p = in[i], q = in[i + 1 == n ? 0 : i + 1];
			float dp = past[i], dq = past[i + 1 == n ? 0 : i + 1];

			if (dp <= 0.0f)
				out[m++] = p;
			if ((dp < 0.0f && dq > 0.0f) || (dp > 0.0f && dq < 0.0f))
				out[m++] = add(p, scale(sub(q, p), dp / (dp - dq)));
		}
		*from = out, *to = (v3 *)in, n = m;
		for (i = 0; !side && i < n; i++)
			along[i] = dot(sub(out[i], origin), axis);
	}
	return n;
}

/* Two boxes' contact, a the lower-numbered: the axis they overlap least along (a face's of
 * either, or the cross of an edge of each), and up to four points. For a face, the other
 * box's facing side is cut to the face's outline. How many points; none when they are apart. */
static int
box_box(const struct place *A, const float *ha, const struct place *B, const float *hb, float margin, struct manifold *m)
{
	const v3 *ax = A->r->c, *bx = B->r->c;
	float r[3][3], ar[3][3], ta[3], tb[3], best = -1e30f, edge_best = -1e30f;
	v3 t = sub(*B->x, *A->x), n, at_a[4], at_b[4];
	int i, j, k, axis = 0, edge = -1, count = 0;

	/* (a row of the axes' products and its face at a time: boxes apart are mostly known so early) */
	for (i = 0; i < 3; i++) {
		float apart;

		ta[i] = dot(t, ax[i]);
		for (j = 0; j < 3; j++) {
			r[i][j] = dot(ax[i], bx[j]);
			ar[i][j] = size(r[i][j]) + 1e-5f;
		}
		apart = size(ta[i]) - (ha[i] + hb[0] * ar[i][0] + hb[1] * ar[i][1] + hb[2] * ar[i][2]);
		if (apart > margin)
			return 0;
		if (apart > best)
			best = apart, axis = i;
	}
	for (j = 0; j < 3; j++) {
		float apart;

		tb[j] = dot(t, bx[j]);
		apart = size(tb[j]) - (hb[j] + ha[0] * ar[0][j] + ha[1] * ar[1][j] + ha[2] * ar[2][j]);
		if (apart > margin)
			return 0;
		if (apart > best)
			best = apart, axis = 3 + j;
	}
#define EDGES(i, j, i1, i2, j1, j2) \
	do { \
		float len2 = 1.0f - r[i][j] * r[i][j], apart; \
		\
		if (len2 < 1e-4f) \
			break; \
		apart = (size(ta[i2] * r[i1][j] - ta[i1] * r[i2][j]) - (ha[i1] * ar[i2][j] + ha[i2] * ar[i1][j] + hb[j1] * ar[i][j2] + hb[j2] * ar[i][j1])) / \
			__builtin_sqrtf(len2); \
		if (apart > margin) \
			return 0; \
		if (apart > edge_best) \
			edge_best = apart, edge = i * 3 + j; \
	} while (0)
	EDGES(0, 0, 1, 2, 1, 2); EDGES(0, 1, 1, 2, 2, 0); EDGES(0, 2, 1, 2, 0, 1);
	EDGES(1, 0, 2, 0, 1, 2); EDGES(1, 1, 2, 0, 2, 0); EDGES(1, 2, 2, 0, 0, 1);
	EDGES(2, 0, 0, 1, 1, 2); EDGES(2, 1, 0, 1, 2, 0); EDGES(2, 2, 0, 1, 0, 1);
#undef EDGES
	/* (a face unless an edge pair is clearly the better: faces give the points that hold a stack) */
	if (edge >= 0 && edge_best > best + 0.01f)
		best = edge_best;
	else
		edge = -1;

	if (edge >= 0) {
		/* the two edges' nearest points */
		v3 ea = ax[edge / 3], eb = bx[edge % 3], pa = *A->x, pb = *B->x, d;
		float ab, da, db, denom, s, u;

		i = edge / 3, j = edge % 3;
		n = cross(ea, eb);
		n = scale(n, 1.0f / __builtin_sqrtf(dot(n, n)));
		if (dot(n, t) < 0.0f)
			n = scale(n, -1.0f);
		for (k = 0; k < 3; k++) {
			if (k != i)
				pa = add(pa, scale(ax[k], dot(n, ax[k]) > 0.0f ? ha[k] : -ha[k]));
			if (k != j)
				pb = sub(pb, scale(bx[k], dot(n, bx[k]) > 0.0f ? hb[k] : -hb[k]));
		}
		d = sub(pb, pa);
		ab = r[i][j], da = dot(d, ea), db = dot(d, eb), denom = 1.0f - ab * ab;
		s = (da - ab * db) / denom;
		u = (ab * da - db) / denom;
		if (s > ha[i]) s = ha[i];
		if (s < -ha[i]) s = -ha[i];
		if (u > hb[j]) u = hb[j];
		if (u < -hb[j]) u = -hb[j];
		at_a[0] = add(pa, scale(ea, s));
		at_b[0] = add(pb, scale(eb, u));
		count = 1;
	} else {
		/* a face of one box (the reference) and the side of the other that faces it */
		int b_ref = axis >= 3, ref = b_ref ? axis - 3 : axis, inc = 0, r1 = (ref + 1) % 3, r2 = (ref + 2) % 3, i1, i2, corners = 4, pick[4], picked = 0;
		const struct place *R = b_ref ? B : A, *I = b_ref ? A : B;
		const v3 origin = *R->x;
		const float *hr = b_ref ? hb : ha, *hi = b_ref ? ha : hb;
		const v3 *rx = R->r->c, *ix = I->r->c;
		v3 out, centre, room[2][10], *poly = room[0], *spare = room[1];
		float most = -1.0f, low[2] = {1e30f, 1e30f}, high[2] = {-1e30f, -1e30f}, deep[10];

		n = b_ref ? scale(bx[ref], tb[ref] < 0.0f ? -1.0f : 1.0f) : scale(ax[ref], ta[ref] < 0.0f ? -1.0f : 1.0f);
		out = b_ref ? scale(n, -1.0f) : n;	/* the reference face looks this way */
		for (k = 0; k < 3; k++)
			if (size(dot(out, ix[k])) > most)
				most = size(dot(out, ix[k])), inc = k;
		i1 = (inc + 1) % 3, i2 = (inc + 2) % 3;
		centre = add(*I->x, scale(ix[inc], dot(out, ix[inc]) > 0.0f ? -hi[inc] : hi[inc]));
		poly[0] = add(add(centre, scale(ix[i1], hi[i1])), scale(ix[i2], hi[i2]));
		poly[1] = add(sub(centre, scale(ix[i1], hi[i1])), scale(ix[i2], hi[i2]));
		poly[2] = sub(sub(centre, scale(ix[i1], hi[i1])), scale(ix[i2], hi[i2]));
		poly[3] = sub(add(centre, scale(ix[i1], hi[i1])), scale(ix[i2], hi[i2]));
		corners = clip(&poly, &spare, corners, origin, rx[r1], hr[r1]);
		corners = clip(&poly, &spare, corners, origin, rx[r2], hr[r2]);
		/* of more than four, the ones furthest out each way along the face */
		for (k = 0; k < 4; k++)
			pick[k] = -1;
		for (k = 0; k < corners; k++) {
			v3 rel = sub(poly[k], origin);
			float u = dot(rel, rx[r1]), w = dot(rel, rx[r2]);

			deep[k] = hr[ref] - dot(rel, out);
			if (deep[k] <= -margin)
				continue;
			/* (along the diagonals: along the sides two of a rectangle's corners tie, and one is left out) */
			if (u + w < low[0]) low[0] = u + w, pick[0] = k;
			if (u + w > high[0]) high[0] = u + w, pick[1] = k;
			if (u - w < low[1]) low[1] = u - w, pick[2] = k;
			if (u - w > high[1]) high[1] = u - w, pick[3] = k;
		}
		for (k = 0; k < 4; k++) {
			v3 p, on;
			float depth;

			if (pick[k] < 0 || (picked >> pick[k] & 1))
				continue;
			picked |= 1 << pick[k];
			p = poly[pick[k]];
			depth = deep[pick[k]];
			on = add(p, scale(out, depth));	/* the point on the reference face over it */
			at_a[count] = b_ref ? p : on;
			at_b[count] = b_ref ? on : p;
			count++;
		}
	}
	if (count) {
		v3 nl = unrot(A->r, n);

		m->points = (uint32_t)count;
		m->normal[0] = nl.x, m->normal[1] = nl.y, m->normal[2] = nl.z, m->normal[3] = 0.0f;
		for (k = 0; k < count; k++) {
			v3 la = unrot(A->r, sub(at_a[k], *A->x)), lb = unrot(B->r, sub(at_b[k], *B->x));

			m->at[k].a[0] = la.x, m->at[k].a[1] = la.y, m->at[k].a[2] = la.z, m->at[k].a[3] = 0.0f;	/* (no push yet) */
			m->at[k].b[0] = lb.x, m->at[k].b[1] = lb.y, m->at[k].b[2] = lb.z, m->at[k].b[3] = dot(la, nl);
		}
	}
	return count;
}

static inline int
cell_of(const struct phys *w, v3 x)
{
	int cx = (int)((x.x + w->half_x) * (1.0f / PHYS_CELL)), cy = (int)(x.y * (1.0f / PHYS_CELL)), cz = (int)((x.z + w->half_z) * (1.0f / PHYS_CELL));

	if (cx < 0) cx = 0;
	if (cx >= w->gx) cx = w->gx - 1;
	if (cy < 0) cy = 0;
	if (cy >= w->gy) cy = w->gy - 1;
	if (cz < 0) cz = 0;
	if (cz >= w->gz) cz = w->gz - 1;
	return (cy * w->gz + cz) * w->gx + cx;
}

uint32_t
phys_key(const struct phys *w, const float *pos)
{
	uint32_t cell = (uint32_t)cell_of(w, of(pos));

	return cell | ((uint32_t)w->cell_first + cell % (uint32_t)w->cell_shares) << 20;
}

void
phys_box(struct phys *w, int body, float hx, float hy, float hz, float density)
{
	struct body_shape *s = &w->shapes[body];
	float mass = 8.0f * hx * hy * hz * density;

	s->half[0] = hx, s->half[1] = hy, s->half[2] = hz;
	s->inv_mass = 1.0f / mass;
	s->inv_inertia[0] = 3.0f / (mass * (hy * hy + hz * hz));
	s->inv_inertia[1] = 3.0f / (mass * (hx * hx + hz * hz));
	s->inv_inertia[2] = 3.0f / (mass * (hx * hx + hy * hy));
	s->radius = __builtin_sqrtf(hx * hx + hy * hy + hz * hz);
}

/* A body into its cell, if the cell is the share's whose key bits are `mine`; 1 when the
 * cell has no room. */
static inline int
grid_take(struct gcell *grid, uint32_t key, uint32_t mine, int body)
{
	struct gcell *cell = &grid[key & 0xfffff];
	uint32_t n;

	if (__builtin_expect((key & PHYS_KEY_SHARE) != mine, 1))
		return 0;
	n = cell->n;
	if (n >= PHYS_SLOTS)
		return 1;
	cell->body[n] = (uint32_t)body;
	cell->n = n + 1;
	return 0;
}

/* The grid: this share's cells take the bodies that are in them, in the bodies' order. (Every
 * body's key is looked at, most of them another share's: four a turn, three instructions each.) */
int
phys_grid(const struct phys *w, int share, int from, int turn)
{
	const struct body_state *s = w->states[from];
	struct gcell *grid = w->grids[turn & 1];
	uint32_t mine = (uint32_t)share << 20;
	int cells = w->gx * w->gy * w->gz, bodies = w->bodies, c, i, missed = 0;

	for (c = share - w->cell_first; c < cells; c += w->cell_shares)
		if (grid[c].n)
			grid[c].n = 0;
	for (i = 0; i + 4 <= bodies; i += 4) {
		missed += grid_take(grid, s[i].key, mine, i);
		missed += grid_take(grid, s[i + 1].key, mine, i + 1);
		missed += grid_take(grid, s[i + 2].key, mine, i + 2);
		missed += grid_take(grid, s[i + 3].key, mine, i + 3);
	}
	for (; i < bodies; i++)
		missed += grid_take(grid, s[i].key, mine, i);
	return missed;
}

/* The whole grid by one core: what every share's phys_grid() together leaves. */
void
phys_grid_all(const struct phys *w, int from, int turn)
{
	const struct body_state *s = w->states[from];
	struct gcell *grid = w->grids[turn & 1];
	int cells = w->gx * w->gy * w->gz, c, i;

	for (c = 0; c < cells; c++)
		if (grid[c].n)
			grid[c].n = 0;
	for (i = 0; i < w->bodies; i++) {
		struct gcell *cell = &grid[s[i].key & 0xfffff];

		if (cell->n < PHYS_SLOTS)
			cell->body[cell->n++] = (uint32_t)i;
	}
}

/* How far round itself a body looks for others: it is not looked for again for a while, and
 * the grid is a substep old when it is (w->look_ahead: how long that comes to). */
static inline float
margin_of(const struct phys *w, const struct body_state *s)
{
	v3 vel = of(s->vel);

	return MARGIN + __builtin_sqrtf(dot(vel, vel)) * w->look_ahead;
}

static inline uint32_t
moving(v3 vel, v3 spin)
{
	return dot(vel, vel) > WAKE * WAKE || dot(spin, spin) > WAKE_SPIN * WAKE_SPIN;
}

static inline int
asleep(const struct body_state *s)
{
	return s->quiet >= PHYS_SLEEP_AFTER && (s->key & PHYS_POSED);
}

void
phys_near_of(const struct phys *w, int from, int i)
{
	const struct body_state *s = &w->states[from][i];
	struct body_near *n = &w->nears[from][i];

	n->pos[0] = s->pos[0], n->pos[1] = s->pos[1], n->pos[2] = s->pos[2];
	n->reach = w->shapes[i].radius + margin_of(w, s);
	n->moving = moving(of(s->vel), of(s->spin));
}

void
phys_nears(const struct phys *w, int from)
{
	int i;

	for (i = 0; i < w->bodies; i++)
		phys_near_of(w, from, i);
}

/* Whether sleeping body i wakes: held, or a fast body is within reach of it, in the cells
 * round it of grid `turn` (whose cells are those of a substep or two ago). */
static __attribute__((noinline)) int
woken(const struct phys *w, const struct body_state *st, const struct body_pose *kept, const struct body_near *nb, int i, int turn)
{
	const struct phys_hold *hold;
	uint32_t k;

	const struct gcell *grid = w->grids[turn & 1];
	uint32_t cell = st[i].key & 0xfffff;
	int cx = (int)cell % w->gx, cz = (int)cell / w->gx % w->gz, cy = (int)cell / (w->gx * w->gz), x, y, z;
	float mine = kept[i].reach;
	v3 here = of(st[i].pos);

	if (w->held)
		for (hold = w->holds; hold < w->holds + PHYS_HOLDS; hold++)
			if (hold->body == i)
				return 1;
	{
		for (y = cy > 0 ? cy - 1 : 0; y <= cy + 1 && y < w->gy; y++)
			for (z = cz > 0 ? cz - 1 : 0; z <= cz + 1 && z < w->gz; z++)
				for (x = cx > 0 ? cx - 1 : 0; x <= cx + 1 && x < w->gx; x++) {
					const struct gcell *c = &grid[(y * w->gz + z) * w->gx + x];

					for (k = 0; k < c->n; k++) {
						const struct body_near *t = &nb[c->body[k]];
						float reach = mine + t->reach;
						v3 d;

						if (!t->moving)
							continue;
						d = sub(of(t->pos), here);
						if (dot(d, d) < reach * reach)
							return 1;
					}
				}
		return 0;
	}
}

/* (not in line: it is the rare way, and its floats would be in the common one's) */
static __attribute__((noinline)) void
poses_made(const struct body_state *s, float gravity, struct pose *mine)
{
	pose_now(s, &mine[0]);
	pose_guess(s, gravity, &mine[1]);
}

/* A neighbour's axes now, and where it is taken to be a substep on: as its owner left them
 * with its state, or worked out here (into `mine`) when the state is nobody's substep. */
static inline void
poses_of(const struct body_state *s, const struct body_pose *kept, float gravity, struct pose *mine, const m3 **r, const v3 **next_at, const m3 **next_r)
{
	if (s->key & PHYS_POSED) {
		*r = (const m3 *)kept->r, *next_at = (const v3 *)kept->next_at, *next_r = (const m3 *)kept->next_r;
	} else {
		poses_made(s, gravity, mine);
		*r = &mine[0].r, *next_at = &mine[1].x, *next_r = &mine[1].r;
	}
}

/* How large the numbers of a body's contacts are: its place's and its size. */
static inline float
about_of(const v3 *at, const struct body_shape *shape)
{
	return size(at->x) + size(at->y) + size(at->z) + shape->radius;
}

static inline void
poses_kept(const struct phys *w, const struct body_state *s, const struct body_shape *shape, const m3 *r, float gravity, struct body_pose *kept)
{
	struct pose next;

	pose_guess(s, gravity, &next);
	*(m3 *)kept->r = *r, *(v3 *)kept->next_at = next.x, *(m3 *)kept->next_r = next.r;
	kept->margin = margin_of(w, s);
	kept->reach = shape->radius + kept->margin;
	kept->about = about_of(&next.x, shape);
}

/* Body i's contacts, at look number `turn`: against what is in the 27 cells round it. A point
 * that was there at the last look keeps the push it had (the nearest old point's, of the
 * same pair). How many pairs had no room. */
static int
look(const struct phys *w, const struct body_state *st, const struct body_pose *kept, const struct body_near *nb, int i, int turn)
{
	const struct body_state *s = &st[i];
	const struct body_shape *shape = &w->shapes[i];
	/* the list is left as it was until the end: an entry of the new one is an old one (its
	 * number in `source`) or one made here (16 and its number) */
	struct manifold *list = &w->manifolds[i * PHYS_MANIFOLDS], *made = __builtin_alloca((PHYS_MANIFOLDS + 1) * sizeof *made);
	uint8_t source[PHYS_MANIFOLDS];
	struct pose made_me, made_other;
	struct place me, other;
	float margin = s->key & PHYS_POSED ? kept[i].margin : margin_of(w, s), mine = shape->radius + margin;
	int slow = dot(of(s->vel), of(s->vel)) < SLOW * SLOW;
	int cell = (int)(s->key & 0xfffff), cx = cell % w->gx, cz = cell / w->gx % w->gz, cy = cell / (w->gx * w->gz), x, y, z, dropped = 0;
	int x0 = cx > 0 ? cx - 1 : 0, x1 = cx + 1 < w->gx ? cx + 1 : w->gx - 1, y1 = cy + 1 < w->gy ? cy + 1 : w->gy - 1, z1 = cz + 1 < w->gz ? cz + 1 : w->gz - 1;
	uint32_t pairs = 0, had = w->info[i].manifolds, fresh = 0, nears = 0, k, m, o;
	int *near = __builtin_alloca(27 * PHYS_SLOTS * sizeof *near);
	const struct gcell *grid = w->grids[turn & 1];
	v3 here;

	me.x = (const v3 *)s->pos, me.r = &made_me.r;
	if (s->key & PHYS_POSED)
		me.r = (const m3 *)kept[i].r;
	else
		pose_now(s, &made_me);
	here = *me.x;
	/* which bodies of the cells are within reach, in the cells' order; then each of those */
	for (y = cy > 0 ? cy - 1 : 0; y <= y1; y++)
		for (z = cz > 0 ? cz - 1 : 0; z <= z1; z++)
			for (x = x0; x <= x1; x++) {
				const struct gcell *c = &grid[(y * w->gz + z) * w->gx + x];
				uint32_t in_cell = c->n;

				for (k = 0; k < in_cell; k++) {
					int j = (int)c->body[k];
					const struct body_near *t = &nb[j];
					float reach = mine + t->reach, along;
					v3 d;

					/* (too far along x alone is too far: most of a cell's are) */
					along = t->pos[0] - here.x;
					if (size(along) >= reach)
						continue;
					d = sub(of(t->pos), here);
					if (j != i && dot(d, d) < reach * reach)
						near[nears++] = j;
				}
			}
	for (k = 0; k < nears; k++) {
		int j = near[k], n, p;
		const struct body_state *t = &st[j];
		float theirs = t->key & PHYS_POSED ? kept[j].margin : margin_of(w, t);

		for (m = 0; m < had && list[m].partner != (uint32_t)j; m++)
			;
		/* (two slow bodies keep the contact they have, seven looks of eight: it has
		 * hardly changed, and finding it again is most of a look) */
		if (m < had && m >= pairs && slow && dot(of(t->vel), of(t->vel)) < SLOW * SLOW && ((turn + i + j) & 7)) {
			source[pairs++] = (uint8_t)m;
			continue;
		}
		other.x = (const v3 *)t->pos, other.r = &made_other.r;
		if (t->key & PHYS_POSED)
			other.r = (const m3 *)kept[j].r;
		else
			pose_now(t, &made_other);
		n = i < j ? box_box(&me, shape->half, &other, w->shapes[j].half, margin + theirs, &made[fresh])
			  : box_box(&other, w->shapes[j].half, &me, shape->half, margin + theirs, &made[fresh]);
		if (!n)
			continue;
		if (pairs == PHYS_MANIFOLDS) {
			dropped++;
			continue;
		}
		for (p = 0; p < n; p++) {
			const float *at = i < j ? made[fresh].at[p].a : made[fresh].at[p].b;
			float nearest = 0.01f;

			for (o = 0; m < had && o < list[m].points; o++) {
				v3 off = sub(of(at), of(i < j ? list[m].at[o].a : list[m].at[o].b));

				if (dot(off, off) < nearest)
					nearest = dot(off, off), made[fresh].at[p].a[3] = list[m].at[o].a[3];
			}
		}
		made[fresh].partner = (uint32_t)j;
		source[pairs++] = (uint8_t)(16 + fresh++);
	}
	/* (an old entry only ever moves down the list, so none is written over before it is taken) */
	for (m = 0; m < pairs; m++)
		if (source[m] >= 16)
			list[m] = made[source[m] - 16];
		else if (source[m] != m)
			list[m] = list[source[m]];
	if (w->info[i].manifolds != pairs)
		w->info[i].manifolds = pairs;
	return dropped;
}

/* How far a point is past what its body is to leave of the contact, counting only what the
 * substep brought (*new) and all of it. Pushed in deeper by its other contacts, the body
 * answers for its share of that too, and the other body for the rest. */
static inline float
over_at(const struct pose *p, const struct point *pt, float *new)
{
	float out = pt->depth - dot(sub(add(p->x, rot(&p->r, pt->local)), pt->on), pt->n);

	*new = out >= 0.0f ? pt->mine - out : pt->mine - pt->share * out;
	return *new + pt->later;
}

/* The body moved for its points, from where the pushes they had last substep leave it: all at
 * once, each as if alone, then as much of the sum as serves them best, so their order is
 * nothing. `speed` is it out of what the substep brought and held against sliding (its
 * velocity is taken from that), `cur` out of what it was in before as well. docs/physics.md. */
static void
settle_points(struct pose *cur, struct pose *speed, struct point *points, int n_points, v3 inv_inertia, float inv_mass)
{
	v3 move = v(0, 0, 0), spun = v(0, 0, 0), held = v(0, 0, 0), held_spun = v(0, 0, 0), turn;
	struct pose trial;
	struct point *pt;
	float num = 0.0f, num_new = 0.0f, den = 0.0f, all = 0.0f, new = 0.0f, len;

	for (pt = points; pt < points + n_points; pt++) {
		move = sub(move, scale(pt->n, inv_mass * pt->had));
		spun = sub(spun, scale(pt->turn, pt->had));
	}
	shifted(cur, move, spun);
	move = spun = v(0, 0, 0);
	for (pt = points; pt < points + n_points; pt++) {
		/* (its part is of what is left once the pushes it has are in: `mine` came as the depth before) */
		float before = pt->mine;

		pt->depth = dot(sub(add(cur->x, rot(&cur->r, pt->local)), pt->on), pt->n);
		if (before > pt->depth) before = pt->depth;
		if (before < 0.0f) before = 0.0f;
		pt->mine = pt->share * (pt->depth - before);
		pt->later = pt->share * (before < SINK ? before : SINK);
		/* (over_at() of the pose the depth was just taken at) */
		pt->over_new = pt->mine, pt->over = pt->mine + pt->later;
		/* (as far as the push it has can be given back) */
		if (pt->over < -pt->had * pt->gives)
			pt->over = -pt->had * pt->gives;
		if (pt->over_new < -pt->had * pt->gives)
			pt->over_new = -pt->had * pt->gives;
		pt->push = pt->over / pt->gives;
		move = sub(move, scale(pt->n, inv_mass * pt->push));
		spun = sub(spun, scale(pt->turn, pt->push));
	}
	trial = *cur;
	shifted(&trial, move, spun);
	for (pt = points; pt < points + n_points; pt++)
		if (pt->push != 0.0f) {
			float did = (pt->mine + pt->later) - over_at(&trial, pt, &len);

			num += pt->over * did;
			num_new += pt->over_new * did;
			den += did * did;
		}
	if (den >= 1e-20f) {
		all = num / den;
		if (all > 1.0f) all = 1.0f;
		if (all < 0.0f) all = 0.0f;
		new = num_new / den;
		if (new > all) new = all;
		if (new < 0.0f) new = 0.0f;
	}
	*speed = *cur;
	shifted(speed, scale(move, new), scale(spun, new));
	/* against sliding: each point's own part, by no more than friction gives what pushes it */
	for (pt = points; pt < points + n_points; pt++) {
		v3 arm, slid;
		float hold;

		pt->had += new * pt->push;
		pt->over = 0.0f;
		if (pt->had <= 0.0f) {
			pt->had = 0.0f;
			continue;
		}
		/* (`on` is done with: from here it is where the point is, for the sum below) */
		arm = rot(&speed->r, pt->local), pt->on = add(speed->x, arm), slid = sub(pt->on, pt->was);
		slid = sub(slid, scale(pt->n, dot(slid, pt->n)));
		len = dot(slid, slid);
		if (len < 1e-12f)
			continue;
		len = __builtin_sqrtf(len);
		pt->turn = slid = scale(slid, 1.0f / len);	/* (kept in the place of what its push turned: that is spent) */
		pt->over = pt->share * len;
		hold = pt->over / give(&speed->r, inv_inertia, inv_mass, arm, slid, &turn);
		if (hold > FRICTION * pt->had)
			hold = FRICTION * pt->had;
		held = sub(held, scale(slid, inv_mass * hold));
		held_spun = sub(held_spun, scale(turn, hold));
	}
	/* (as much of the sum as serves: four corners that each stop a spin alone stop it three times) */
	trial = *speed;
	shifted(&trial, held, held_spun);
	for (num = den = 0.0f, pt = points; pt < points + n_points; pt++)
		if (pt->over > 0.0f) {
			float did = dot(sub(pt->on, add(trial.x, rot(&trial.r, pt->local))), pt->turn);

			num += pt->over * did;
			den += did * did;
		}
	len = den < 1e-20f ? 0.0f : num / den;
	if (len > 1.0f) len = 1.0f;
	if (len > 0.0f)
		shifted(speed, scale(held, len), scale(held_spun, len));
	*cur = *speed;
	shifted(cur, scale(move, all - new), scale(spun, all - new));
}

static int
held_now(const struct phys *w, int i)
{
	return w->holds[0].body == i || w->holds[1].body == i || w->holds[2].body == i;
}

/* A held body: its point a quarter of the way to where each hold wants it, 30 cm at most.
 * (Not in line: the rare way.) */
static __attribute__((noinline)) void
pulled(const struct phys *w, int i, struct pose *cur, v3 inv_inertia, float inv_mass)
{
	const struct phys_hold *hold;

	for (hold = w->holds; hold < w->holds + PHYS_HOLDS; hold++)
		if (hold->body == i) {
			v3 arm = rot(&cur->r, of(hold->at)), pull = sub(of(hold->to), add(cur->x, arm)), turn;
			float len = __builtin_sqrtf(dot(pull, pull)), go = len * 0.25f > 0.3f ? 0.3f : len * 0.25f;

			if (len > 1e-4f) {
				pull = scale(pull, 1.0f / len);
				go /= give(&cur->r, inv_inertia, inv_mass, arm, pull, &turn);
				shifted(cur, scale(pull, inv_mass * go), scale(turn, go));
			}
		}
}

/* A look for the contacts of this share's bodies, where they are in buffer `from`. */
int
phys_look(const struct phys *w, int share, int shares, int from, int turn)
{
	const struct body_state *st = w->states[from];
	const struct body_pose *kept = w->poses[from];
	int i, dropped = 0;

	for (i = share; i < w->bodies; i += shares)
		dropped += look(w, st, kept, w->nears[from], i, turn);
	return dropped;
}

/* A substep of this share's bodies, from one buffer into the next; with `looking` (which
 * look it is, from 1), each body's contacts are found first. A body goes where it would go,
 * the others where pose_guess() says; then it takes its mass's part of every contact, and
 * the other body of each pair takes the rest for itself, from the same numbers. */
int
phys_step(const struct phys *w, int share, int shares, int from, int looking, int turn)
{
	const struct body_state *st = w->states[from];
	int next = from + 1 == PHYS_BUFFERS ? 0 : from + 1;
	struct body_state *to = w->states[next];
	const struct body_pose *kept = w->poses[from];
	int i, dropped = 0;

	for (i = share; i < w->bodies; i += shares) {
		const struct body_state *s = &st[i];
		const struct body_shape *shape = &w->shapes[i];
		struct manifold *list = &w->manifolds[i * PHYS_MANIFOLDS];
		struct pose now, cur, speed, theirs_made[2];
		struct point points[MOST_POINTS], *pt;
		struct body_state *out = &to[i];
		uint32_t pairs, m, k;
		v3 inv_inertia = of(shape->inv_inertia), vel, spin;
		float inv_mass = shape->inv_mass, fast, spin2, sx, sy, sz, w0, about;
		int n_points = 0, plane, corner = 0;
		uint32_t held = 0, quiet;

		if (asleep(s) && !woken(w, st, kept, w->nears[from], i, turn)) {
			to[i] = *s;
			memcpy(&w->poses[next][i], &kept[i], offsetof(struct body_pose, pad));
			w->nears[next][i] = w->nears[from][i];
			continue;
		}
		if (looking)
			dropped += look(w, st, kept, w->nears[from], i, looking);
		pairs = w->info[i].manifolds;
		if (s->key & PHYS_POSED) {
			now.x = of(s->pos), now.r = *(const m3 *)kept[i].r;
			now.q[0] = s->q[0], now.q[1] = s->q[1], now.q[2] = s->q[2], now.q[3] = s->q[3];
		} else {
			pose_now(s, &now);
		}
		pose_next(s, w->gravity, &cur);
		if (__builtin_expect(w->held != 0, 0)) {
			/* (by a copy: the pose itself stays in registers) */
			struct pose held_at = cur;

			pulled(w, i, &held_at, inv_inertia, inv_mass);
			cur = held_at;
		}
		about = about_of(&cur.x, shape);
		for (m = 0; m < pairs; m++) {
			struct manifold *c = &list[m];
			int j = (int)c->partner, self_b = i > j;
			const struct body_shape *theirs = &w->shapes[j];
			const m3 *their_r, *their_next_r;
			const v3 *their_next_at;
			v3 their_at = of(st[j].pos), n, n_b;
			float share = inv_mass / (inv_mass + theirs->inv_mass), gap, sure;

			poses_of(&st[j], &kept[j], w->gravity, theirs_made, &their_r, &their_next_at, &their_next_r);
			n = self_b ? scale(rot(their_next_r, of(c->normal)), -1.0f) : rot(&cur.r, of(c->normal));
			/* a point's depth from the bodies' own frames, nearly: one clearly apart is left there */
			sure = SURE + SURE_OF * (about + (st[j].key & PHYS_POSED ? kept[j].about : about_of(their_next_at, theirs)));
			gap = dot(sub(cur.x, *their_next_at), n) + sure;
			n_b = self_b ? unrot(&cur.r, n) : scale(unrot(their_next_r, n), -1.0f);
			for (k = 0; k < c->points && n_points < MOST_POINTS; k++) {
				float had = c->at[k].a[3], depth, before;
				v3 mine, other, arm, p, t, p_old, t_old;

				if (gap + c->at[k].b[3] + dot(of(c->at[k].b), n_b) < 0.0f) {
					if (had != 0.0f)
						c->at[k].a[3] = 0.0f;
					continue;
				}
				mine = of(self_b ? c->at[k].b : c->at[k].a), other = of(self_b ? c->at[k].a : c->at[k].b);
				arm = rot(&cur.r, mine), p = add(cur.x, arm), t = add(*their_next_at, rot(their_next_r, other));
				depth = dot(sub(p, t), n);
				if (depth <= 0.0f) {
					if (had != 0.0f)
						c->at[k].a[3] = 0.0f;
					continue;
				}
				p_old = add(now.x, rot(&now.r, mine)), t_old = add(their_at, rot(their_r, other));
				before = dot(sub(p_old, t_old), n);
				pt = &points[n_points++];
				pt->gives = give(&cur.r, inv_inertia, inv_mass, arm, n, &pt->turn);
				pt->local = mine, pt->on = t, pt->n = n, pt->was = add(p_old, sub(t, t_old));
				pt->share = share;
				pt->mine = before;
				pt->had = had;
				pt->kept = &c->at[k].a[3];
			}
		}
		/* the floor and the walls, a corner at a time: they take no part of a contact */
		if (cur.x.y > shape->radius + 0.1f && size(cur.x.x) < w->half_x - shape->radius && size(cur.x.z) < w->half_z - shape->radius) {
			if (w->info[i].floored) {
				for (corner = 0; corner < 8; corner++)
					if (w->info[i].floor[corner] != 0.0f)
						w->info[i].floor[corner] = 0.0f;
				w->info[i].floored = 0;
			}
			sx = 2.0f;	/* (and none of the loop below) */
		} else {
			sx = -1.0f;
		}
		for (corner = 0; sx <= 1.0f; sx += 2.0f)
			for (sy = -1.0f; sy <= 1.0f; sy += 2.0f)
				for (sz = -1.0f; sz <= 1.0f; sz += 2.0f, corner++) {
					v3 l = v(sx * shape->half[0], sy * shape->half[1], sz * shape->half[2]), arm = rot(&cur.r, l), p = add(cur.x, arm);

					for (plane = 0; plane < 3 && n_points < MOST_POINTS; plane++) {
						v3 n = plane == 0 ? v(0, -1.0f, 0) : plane == 1 ? v(p.x > 0.0f ? 1.0f : -1.0f, 0, 0) : v(0, 0, p.z > 0.0f ? 1.0f : -1.0f);
						v3 on = plane == 0 ? v(p.x, 0, p.z) : plane == 1 ? v(n.x * w->half_x, p.y, p.z) : v(p.x, p.y, n.z * w->half_z), old;
						float depth = dot(sub(p, on), n), had = plane == 0 ? w->info[i].floor[corner] : 0.0f, before;

						if (depth <= 0.0f) {
							if (had != 0.0f)
								w->info[i].floor[corner] = 0.0f;
							continue;
						}
						old = add(now.x, rot(&now.r, l));
						before = dot(sub(old, on), n);
						pt = &points[n_points++];
						pt->gives = give(&cur.r, inv_inertia, inv_mass, arm, n, &pt->turn);
						pt->local = l, pt->on = on, pt->n = n, pt->was = old;
						pt->share = 1.0f;
						pt->mine = before;
						pt->had = had;
						pt->kept = plane == 0 ? &w->info[i].floor[corner] : 0;
						if (plane == 0 && !w->info[i].floored)
							w->info[i].floored = 1;
					}
				}
		speed = cur;
		if (n_points) {
			settle_points(&cur, &speed, points, n_points, inv_inertia, inv_mass);
			for (pt = points; pt < points + n_points; pt++) {
				if (pt->kept && *pt->kept != pt->had)
					*pt->kept = pt->had;
				held += pt->had > 0.0f;
			}
		}
		/* the velocity is what the body did; getting out of what it was in already is no speed */
		vel = scale(sub(speed.x, now.x), 0.9995f / PHYS_DT);
		w0 = speed.q[3] * now.q[3] + speed.q[0] * now.q[0] + speed.q[1] * now.q[1] + speed.q[2] * now.q[2];
		spin = sub(sub(scale(of(speed.q), now.q[3]), scale(of(now.q), speed.q[3])), cross(of(speed.q), of(now.q)));
		spin = scale(spin, (w0 < 0.0f ? -2.0f : 2.0f) * 0.999f / PHYS_DT);
		fast = dot(vel, vel);
		if (fast > MAX_SPEED * MAX_SPEED)
			vel = scale(vel, MAX_SPEED / __builtin_sqrtf(fast));
		spin2 = dot(spin, spin);
		if (spin2 > MAX_SPIN * MAX_SPIN)
			spin = scale(spin, MAX_SPIN / __builtin_sqrtf(spin2));
		quiet = fast < SLEEP_SPEED * SLEEP_SPEED && spin2 < SLEEP_SPIN * SLEEP_SPIN && !(w->held && held_now(w, i)) ? s->quiet + 1 : 0;
		if (quiet >= PHYS_SLEEP_AFTER)
			quiet = PHYS_SLEEP_AFTER, vel = spin = v(0, 0, 0);
		out->pos[0] = cur.x.x, out->pos[1] = cur.x.y, out->pos[2] = cur.x.z;
		out->key = phys_key(w, out->pos);
		out->held = held;
		out->q[0] = cur.q[0], out->q[1] = cur.q[1], out->q[2] = cur.q[2], out->q[3] = cur.q[3];
		out->vel[0] = vel.x, out->vel[1] = vel.y, out->vel[2] = vel.z;
		out->spin[0] = spin.x, out->spin[1] = spin.y, out->spin[2] = spin.z;
		out->quiet = quiet;
		poses_kept(w, out, shape, &cur.r, w->gravity, &w->poses[next][i]);
		out->key |= PHYS_POSED;
		w->nears[next][i].pos[0] = cur.x.x, w->nears[next][i].pos[1] = cur.x.y, w->nears[next][i].pos[2] = cur.x.z;
		w->nears[next][i].reach = w->poses[next][i].reach;
		/* (a held box wakes what it is carried into, however slowly) */
		w->nears[next][i].moving = moving(vel, spin) || (w->held && held_now(w, i));
	}
	return dropped;
}

/* A face as a quad: its middle and each of four sums of two half sides, and its shade's texel. */
static inline uint32_t *
face(uint32_t *to, v3 middle, const v3 *corner, float lit, uint32_t colour)
{
	float shade = 0.42f + 0.58f * (lit > 0.0f ? lit : 0.0f);
	uint32_t texel = ((uint32_t)(shade * 63.0f) * 16 + 8) | (colour * 64 + 32) << 16;
	v3 at;

	at = add(middle, corner[0]), to[0] = bits(at.x), to[1] = bits(at.y), to[2] = bits(at.z), to[3] = texel;
	at = add(middle, corner[1]), to[4] = bits(at.x), to[5] = bits(at.y), to[6] = bits(at.z), to[7] = texel;
	at = add(middle, corner[2]), to[8] = bits(at.x), to[9] = bits(at.y), to[10] = bits(at.z), to[11] = texel;
	at = add(middle, corner[3]), to[12] = bits(at.x), to[13] = bits(at.y), to[14] = bits(at.z), to[15] = texel;
	return to + 16;
}

/* The two faces across an axis: they share the four sums, and one's light is the other's dark. */
static inline uint32_t *
faces(uint32_t *to, v3 x, v3 axis, v3 half, v3 u, v3 up, uint32_t colour)
{
	static const v3 light = {0.37f, 0.82f, 0.43f};
	v3 away = v(-u.x, -u.y, -u.z), corner[4] = {sub(away, up), sub(u, up), add(u, up), sub(up, u)};
	float lit = dot(axis, light);

	to = face(to, sub(x, half), corner, -lit, colour);
	return face(to, add(x, half), corner, lit, colour);
}

void
phys_quads(const struct phys *w, int body, int from, uint32_t colour, uint32_t *to)
{
	const struct body_state *s = &w->states[from][body];
	const struct body_shape *shape = &w->shapes[body];
	v3 x = of(s->pos), e0, e1, e2;
	m3 r;

	if (s->key & PHYS_POSED)
		r = *(const m3 *)w->poses[from][body].r;
	else
		r = matrix_of(s->q);
	e0 = scale(r.c[0], shape->half[0]), e1 = scale(r.c[1], shape->half[1]), e2 = scale(r.c[2], shape->half[2]);
	to = faces(to, x, r.c[0], e0, e1, e2, colour);
	to = faces(to, x, r.c[1], e1, e2, e0, colour);
	faces(to, x, r.c[2], e2, e0, e1, colour);
}

int
phys_pick(const struct phys *w, int from, const float *eye, const float *way, float *local, float *distance)
{
	float nearest = 1e30f;
	int i, k, best = -1;

	for (i = 0; i < w->bodies; i++) {
		struct pose p;
		v3 o, d;
		float in = 0.0f, out = nearest;

		pose_now(&w->states[from][i], &p);
		o = unrot(&p.r, sub(of(eye), p.x)), d = unrot(&p.r, of(way));
		/* the ray in the box's frame, against its three pairs of sides */
		for (k = 0; k < 3 && in <= out; k++) {
			float from_k = k == 0 ? o.x : k == 1 ? o.y : o.z, way_k = k == 0 ? d.x : k == 1 ? d.y : d.z, half = w->shapes[i].half[k];

			if (size(way_k) < 1e-6f) {
				if (size(from_k) > half)
					in = 1e30f;
			} else {
				float a = (-half - from_k) / way_k, b = (half - from_k) / way_k;

				if (a > b) { float c = a; a = b; b = c; }
				if (a > in) in = a;
				if (b < out) out = b;
			}
		}
		if (in <= out) {
			v3 at = add(o, scale(d, in));

			nearest = in, best = i;
			local[0] = at.x, local[1] = at.y, local[2] = at.z;
		}
	}
	*distance = nearest;
	return best;
}

int
phys_near(const struct phys *w, int from, const float *point, float reach, float *local)
{
	float nearest = reach * reach;
	int i, best = -1;

	for (i = 0; i < w->bodies; i++) {
		const float *half = w->shapes[i].half;
		struct pose p;
		v3 o, on, off;

		pose_now(&w->states[from][i], &p);
		o = unrot(&p.r, sub(of(point), p.x));
		on = v(o.x > half[0] ? half[0] : o.x < -half[0] ? -half[0] : o.x, o.y > half[1] ? half[1] : o.y < -half[1] ? -half[1] : o.y,
			o.z > half[2] ? half[2] : o.z < -half[2] ? -half[2] : o.z);
		off = sub(o, on);
		if (dot(off, off) <= nearest && (dot(off, off) < nearest || best < 0)) {
			nearest = dot(off, off), best = i;
			local[0] = on.x, local[1] = on.y, local[2] = on.z;
		}
	}
	return best;
}

void
phys_point(const struct phys *w, int from, int body, const float *local, float *at)
{
	struct pose p;
	v3 x;

	pose_now(&w->states[from][body], &p);
	x = add(p.x, rot(&p.r, of(local)));
	at[0] = x.x, at[1] = x.y, at[2] = x.z;
}

void
phys_figures(const struct phys *w, int from, float *deepest, float *fastest)
{
	float sx, sy, sz;
	int i;

	*deepest = *fastest = 0.0f;
	for (i = 0; i < w->bodies; i++) {
		const struct body_state *s = &w->states[from][i];
		const struct body_shape *shape = &w->shapes[i];
		struct pose p;
		float speed = __builtin_sqrtf(dot(of(s->vel), of(s->vel)));

		pose_now(s, &p);
		if (speed > *fastest)
			*fastest = speed;
		for (sx = -1.0f; sx <= 1.0f; sx += 2.0f)
			for (sy = -1.0f; sy <= 1.0f; sy += 2.0f)
				for (sz = -1.0f; sz <= 1.0f; sz += 2.0f) {
					v3 c = add(p.x, rot(&p.r, v(sx * shape->half[0], sy * shape->half[1], sz * shape->half[2])));
					float in = -c.y;

					if (size(c.x) - w->half_x > in) in = size(c.x) - w->half_x;
					if (size(c.z) - w->half_z > in) in = size(c.z) - w->half_z;
					if (in > *deepest)
						*deepest = in;
				}
	}
}
