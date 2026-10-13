/* Rigid boxes for many cores with no locks (docs/physics.md). Each phase reads what the phase
 * before left and writes only what its share owns: body i and cell c are share i mod n's and
 * c mod n's, and everything a share writes is whole texels of its own. */
#ifndef PHYSICS_H
#define PHYSICS_H

#include <stdint.h>

#define PHYS_HZ 60		/* substeps in a second of the boxes' time */
#define PHYS_DT (1.0f / PHYS_HZ)
#define PHYS_EVERY 2		/* substeps between two looks for contacts */
#define PHYS_CELL 2.0f		/* a grid cell's side: no less than two bodies' radii and their margins */
#define PHYS_SLOTS 39
#define PHYS_MANIFOLDS 12
#define PHYS_QUAD_WORDS 96	/* a box as the GPU's quads: six of four compact vertices */
#define PHYS_BUFFERS 3		/* states kept: a substep's, the one it makes, and the one a look may still read */

/* A body as a substep leaves it: four texels. `key` is its cell, from bit 20 the cell's share
 * and PHYS_POSED when its body_pose is this state's; `held` is how many of its contact points
 * pushed it. */
#define PHYS_POSED 0x80000000u
#define PHYS_KEY_SHARE 0x7ff00000u
struct body_state { float pos[3]; uint32_t key; float q[4]; float vel[3]; uint32_t held; float spin[3]; uint32_t pad; };
/* What every neighbour would work out from a state, left with it by the body's owner: its
 * axes, where it is taken to be a substep on and its axes there, how far round itself it
 * looks for others, that with its radius, and how large its numbers are. Eight texels, two
 * of them nothing: a neighbour is found by a shift. */
struct body_pose { float r[9], next_at[3], next_r[9], margin, reach, about, pad[8]; };
/* What a body is, which nothing changes: two texels. */
struct body_shape { float half[3], inv_mass, inv_inertia[3], radius; };
/* How many pairs it has contacts with, whether a corner may have a push from the floor, and
 * the push each corner has from it: three texels. */
struct body_info { uint32_t manifolds, floored, pad[2]; float floor[8]; };
/* A pair's contact, in both bodies' lists: the points on each body in its own frame, and the
 * direction from the lower-numbered to the other in the lower one's frame. A point's a[3] is
 * the list's owner's alone: the push the point has, kept from substep to substep; its b[3] is
 * how far along the normal `a` is from its body's middle. Ten texels. */
struct manifold {
	uint32_t partner, points, pad[2];
	float normal[4];
	struct { float a[4], b[4]; } at[4];
};
struct gcell { uint32_t n, body[PHYS_SLOTS]; };

/* A box somebody holds (`body` -1: nobody): its point `at`, in its own frame, is pulled to `to`.
 * The first core's, two texels: a share only reads them, and `to` changes in one store's texel. */
#define PHYS_HOLDS 3		/* the pointer's, the left hand's, the right hand's */
struct phys_hold { int body; float at[3]; float to[3]; uint32_t pad; } __attribute__((aligned(16)));

struct phys {
	struct body_state *states[PHYS_BUFFERS];	/* a substep reads one and writes the next, round and round */
	struct body_pose *poses[PHYS_BUFFERS];	/* as the states */
	struct body_shape *shapes;
	struct body_info *info;
	struct manifold *manifolds;	/* PHYS_MANIFOLDS a body */
	struct gcell *grids[2];		/* look number t reads grids[t & 1], while the next look's is made */
	float look_ahead;		/* how long a contact found by a look has to do for, in seconds */
	int bodies, gx, gy, gz;
	/* the shares that make the grid: cell c is share cell_first + c mod cell_shares's */
	int cell_shares, cell_first;
	float half_x, half_z, gravity;	/* the room: a floor at 0, walls at +-half */
	int held;			/* how many of the holds have a box */
	struct phys_hold holds[PHYS_HOLDS];
};

/* A share's cells of the grid of what is where, for look number `turn` (a share of those
 * that make it); and for one share of the `shares` that have the bodies: a look for contacts
 * (its number, from 1), and a substep, with that look first when `looking` is its number.
 * Each returns what it had no room for. */
int phys_grid(const struct phys *w, int share, int from, int turn);
void phys_grid_all(const struct phys *w, int from, int turn);	/* every share's grid, by one core */
int phys_look(const struct phys *w, int share, int shares, int from, int turn);
int phys_step(const struct phys *w, int share, int shares, int from, int looking);

uint32_t phys_key(const struct phys *w, const float *pos);
void phys_box(struct phys *w, int body, float hx, float hy, float hz, float density);
/* A body's six faces as quads, each face one texel of a table 64 shades across, row `colour` of 16. */
void phys_quads(const struct phys *w, int body, int from, uint32_t colour, uint32_t *to);
/* How deep the deepest corner is under the floor or in a wall, and the fastest body's speed. */
/* The box a ray from `eye` along `way` (unit) meets first, or -1: where on it, in its own frame, and how far. */
int phys_pick(const struct phys *w, int from, const float *eye, const float *way, float *local, float *distance);
/* The box whose surface is nearest a point and no further than `reach`, or -1: the point of it, in its own frame. */
int phys_near(const struct phys *w, int from, const float *point, float reach, float *local);
/* Where in the room a point of a box is. */
void phys_point(const struct phys *w, int from, int body, const float *local, float *at);
void phys_figures(const struct phys *w, int from, float *deepest, float *fastest);

#endif
