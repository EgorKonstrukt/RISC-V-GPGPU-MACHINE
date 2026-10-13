/*
 * Parts of Doom's game code written again for this machine (docs/doom.md): the same answers
 * in fewer instructions, and without stores that fill the machine's write cache. A demo must
 * play the same: DOOM_HASH_TICS=N prints a sum of the game's state every N tics.
 *
 *   DOOM_PORT=sight,look,think,find,names,lines,group   the port's own code for any of
 *                                those (find: a point's subsector; names: of lumps and textures;
 *                                lines: what is in a blockmap cell; group: a sector's lines)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "doomdef.h"
#include "doomstat.h"
#include "m_bbox.h"
#include "m_random.h"
#include "m_swap.h"
#include "p_local.h"
#include "r_main.h"
#include "z_zone.h"
#include "r_state.h"
#include "w_wad.h"

boolean P_CheckSight_port(mobj_t *t1, mobj_t *t2);
fixed_t P_InterceptVector2(divline_t *v2, divline_t *v1);
extern fixed_t sightzstart, topslope, bottomslope;
extern int sightcounts[2];
extern int rndindex, prndindex;

/*
 * A line that an iterator has visited is marked here, not in the line: Doom's lines are 64
 * bytes apart, and stores that far apart share a few sets of the write cache.
 */
int doom_line_mark[65536];
static int sight_mark[65536], sight_pass;

static int use_port_sight = -1, use_port_look, use_port_think, use_port_find, use_port_names, use_port_lines, use_port_group, use_port_rest;

boolean P_LookForPlayers_port(mobj_t *actor, boolean allaround);
void P_RunThinkers_port(void);

/* A thing's `validcount`, which the game has and never uses, is ours: bit 31 says it is asleep
 * (P_RunThinkers), the rest that it is among the changed: the view's number and where. */
#define ASLEEP		0x80000000u
static int stamps_stale = 1;		/* every thing's validcount is to be 0 again: another level, or the numbers went round */

static void
which_port(void)
{
	const char *port = getenv("DOOM_PORT");

	use_port_sight = port && strstr(port, "sight");
	use_port_look = port && strstr(port, "look");
	use_port_think = port && strstr(port, "think");
	use_port_find = port && strstr(port, "find");
	use_port_names = port && strstr(port, "names");
	use_port_lines = port && strstr(port, "lines");
	use_port_group = port && strstr(port, "group");
	use_port_rest = port && strstr(port, "rest");
}

struct trace { fixed_t x, y, dx, dy, x2, y2; };

/* Which side of a line a point is on: 0 front, 1 back, 2 on it. As the port's P_DivlineSide. */
static inline int
side_of(fixed_t x, fixed_t y, fixed_t nx, fixed_t ny, fixed_t ndx, fixed_t ndy)
{
	fixed_t left, right;

	if (!ndx) {
		if (x == nx)
			return 2;
		if (x <= nx)
			return ndy > 0;
		return ndy < 0;
	}
	if (!ndy) {
		if (x == ny)	/* the port compares x here, and so do we */
			return 2;
		if (y <= ny)
			return ndx < 0;
		return ndx > 0;
	}
	left = (ndy >> FRACBITS) * ((x - nx) >> FRACBITS);
	right = ((y - ny) >> FRACBITS) * (ndx >> FRACBITS);
	if (right < left)
		return 0;
	if (left == right)
		return 2;
	return 1;
}

/*
 * Whether the line from t1 to t2 crosses nothing that blocks sight. The port's walk over the
 * BSP tree, nearer side first, without its calls: the far sides wait on a stack of their own.
 */
static int
sight_walk(const struct trace *trace)
{
	const fixed_t tx = trace->x, ty = trace->y, tdx = trace->dx, tdy = trace->dy, x2 = trace->x2, y2 = trace->y2;
	const node_t *const all = nodes;
	const int pass = sight_pass;
	int waiting[128], depth = 0, bspnum = numnodes - 1;

	for (;;) {
		const subsector_t *sub;
		const seg_t *seg;
		int count;

		while (!(bspnum & NF_SUBSECTOR)) {
			const node_t *bsp = &all[bspnum];
			int side = side_of(tx, ty, bsp->x, bsp->y, bsp->dx, bsp->dy);

			if (side == 2)
				side = 0;	/* on the line: both sides are crossed */
			if (side != side_of(x2, y2, bsp->x, bsp->y, bsp->dx, bsp->dy)) {
				if (depth == 128)
					return -1;	/* a tree deeper than any we know: the port's way */
				waiting[depth++] = bsp->children[side ^ 1];
			}
			bspnum = bsp->children[side];
		}
		sub = &subsectors[bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR];
		seg = &segs[sub->firstline];
		for (count = sub->numlines; count; seg++, count--) {
			const line_t *line = seg->linedef;
			int *mark = &sight_mark[line - lines];
			fixed_t x1, y1, ldx, ldy, opentop, openbottom, frac, slope;
			const sector_t *front, *back;
			divline_t divl, strace;

			if (*mark == pass)
				continue;	/* seen from its other side */
			*mark = pass;
			x1 = line->v1->x;
			y1 = line->v1->y;
			if (side_of(x1, y1, tx, ty, tdx, tdy) == side_of(line->v2->x, line->v2->y, tx, ty, tdx, tdy))
				continue;
			ldx = line->dx;
			ldy = line->dy;
			if (side_of(tx, ty, x1, y1, ldx, ldy) == side_of(x2, y2, x1, y1, ldx, ldy))
				continue;
			if (!(line->flags & ML_TWOSIDED))
				return 0;
			front = seg->frontsector;
			back = seg->backsector;
			if (front->floorheight == back->floorheight && front->ceilingheight == back->ceilingheight)
				continue;
			opentop = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;
			openbottom = front->floorheight > back->floorheight ? front->floorheight : back->floorheight;
			if (openbottom >= opentop)
				return 0;	/* a closed door */
			divl.x = x1;
			divl.y = y1;
			divl.dx = ldx;
			divl.dy = ldy;
			strace.x = tx;
			strace.y = ty;
			strace.dx = tdx;
			strace.dy = tdy;
			frac = P_InterceptVector2(&strace, &divl);
			if (front->floorheight != back->floorheight) {
				slope = FixedDiv(openbottom - sightzstart, frac);
				if (slope > bottomslope)
					bottomslope = slope;
			}
			if (front->ceilingheight != back->ceilingheight) {
				slope = FixedDiv(opentop - sightzstart, frac);
				if (slope < topslope)
					topslope = slope;
			}
			if (topslope <= bottomslope)
				return 0;
		}
		if (!depth)
			return 1;
		bspnum = waiting[--depth];
	}
}

boolean
P_CheckSight(mobj_t *t1, mobj_t *t2)
{
	struct trace t;
	int pnum, seen;

	if (use_port_sight < 0)
		which_port();
	if (use_port_sight)
		return P_CheckSight_port(t1, t2);
	pnum = (t1->subsector->sector - sectors) * numsectors + (t2->subsector->sector - sectors);
	if (rejectmatrix[pnum >> 3] & (1 << (pnum & 7))) {
		sightcounts[0]++;
		return false;
	}
	sightcounts[1]++;
	validcount++;
	sight_pass++;
	sightzstart = t1->z + t1->height - (t1->height >> 2);
	topslope = (t2->z + t2->height) - sightzstart;
	bottomslope = t2->z - sightzstart;
	t.x = t1->x;
	t.y = t1->y;
	t.x2 = t2->x;
	t.y2 = t2->y;
	t.dx = t2->x - t1->x;
	t.dy = t2->y - t1->y;
	seen = sight_walk(&t);
	return seen < 0 ? P_CheckSight_port(t1, t2) : seen;
}

/*
 * A monster looks for a player to wake up for. As the port's, but whether the player is in
 * front of it is asked before whether it can be seen, and a player not seen is not looked
 * at again in the same search (with one player the port looks at him twice): the same answer.
 */
boolean
P_LookForPlayers(mobj_t *actor, boolean allaround)
{
	int c = 0, stop = (actor->lastlook - 1) & 3, unseen = 0;

	if (use_port_sight < 0)
		which_port();
	if (use_port_look)
		return P_LookForPlayers_port(actor, allaround);
	for (;; actor->lastlook = (actor->lastlook + 1) & 3) {
		player_t *player;

		if (!playeringame[actor->lastlook])
			continue;
		if (c++ == 2 || actor->lastlook == stop)
			return false;
		if (unseen & (1 << actor->lastlook))
			continue;
		unseen |= 1 << actor->lastlook;
		player = &players[actor->lastlook];
		if (player->health <= 0)
			continue;
		if (!allaround) {
			angle_t an = R_PointToAngle2(actor->x, actor->y, player->mo->x, player->mo->y) - actor->angle;

			if (an > ANG90 && an < ANG270 &&
			    P_AproxDistance(player->mo->x - actor->x, player->mo->y - actor->y) > MELEERANGE)
				continue;	/* behind its back */
		}
		if (!P_CheckSight(actor, player->mo))
			continue;
		actor->target = player->mo;
		return true;
	}
}

/*
 * Every thinker's turn. As the port's, but a thing at rest is seen to here: its state counted
 * down without the call, or passed by. Found at rest it is asleep, and is not asked again
 * whether it rests until the game wakes it (doom_thing_changed). DOOM_PORT=rest: none sleeps.
 */
void
P_RunThinkers(void)
{
	thinker_t *th = thinkercap.next;
	const int stale = stamps_stale;

	if (use_port_sight < 0)
		which_port();
	if (use_port_think) {
		P_RunThinkers_port();
		return;
	}
	stamps_stale = 0;
	while (th != &thinkercap) {
		if (th->function.acv == (actionf_v)(-1)) {
			th->next->prev = th->prev;
			th->prev->next = th->next;
			Z_Free(th);
		} else if (th->function.acp1 == (actionf_p1)P_MobjThinker) {
			mobj_t *m = (mobj_t *)th;

			if (stale)
				m->validcount = 0;	/* (a saved game's marks, or a level's of long ago) */
			if (m->validcount < 0) {
				if (m->tics != -1 && !--m->tics)
					P_SetMobjState(m, m->state->nextstate);
			} else if (m->momx | m->momy | m->momz | (m->z ^ m->floorz) | (m->flags & MF_SKULLFLY)) {
				P_MobjThinker(m);
			} else if (m->tics != -1) {
				/* at rest: its thinker would count its state down and no more */
				if (!--m->tics)
					P_SetMobjState(m, m->state->nextstate);
				else if (!m->player && !use_port_rest)
					m->validcount |= ASLEEP;
			} else if ((m->flags & MF_COUNTKILL) && respawnmonsters) {
				P_MobjThinker(m);
			} else if (!m->player && !use_port_rest) {
				m->validcount |= ASLEEP;
			}
		} else if (th->function.acp1) {
			th->function.acp1(th);
		}
		th = th->next;
	}
}

/* A sum over what a demo must reproduce: the random numbers used and every thing's state. */
unsigned
doom_state_hash(void)
{
	unsigned h = 2166136261u;
	thinker_t *th;

#define MIX(v) (h = (h ^ (unsigned)(v)) * 16777619u)
	MIX(prndindex);
	MIX(rndindex);
	MIX(leveltime);
	for (th = thinkercap.next; th != &thinkercap; th = th->next) {
		const mobj_t *m = (const mobj_t *)th;

		if (th->function.acp1 != (actionf_p1)P_MobjThinker)
			continue;
		MIX(m->x), MIX(m->y), MIX(m->z), MIX(m->angle), MIX(m->momx), MIX(m->momy), MIX(m->momz);
		MIX(m->type), MIX(m->tics), MIX(m->health), MIX(m->flags), MIX(m->state - states);
	}
#undef MIX
	return h;
}

/* ---- what the view is told (doom_gl.c; docs/doom.md, "The view on worker cores") ---- */

/*
 * The game says which things it changed (doom.sh puts the calls in), took out or put into a
 * sector's list, in that order, and which sectors it wrote; a view takes them between tics.
 */
#define TOLD_MOST	600
#define TOLD_LINKS	1024
#define TOLD_GONE	256

int doom_told_all = 1;			/* more than fits, or another level: every thing again */
mobj_t *doom_told_changed[TOLD_MOST];	/* the things changed since the last view, each once; NULL: taken out since */
mobj_t *doom_told_linked[TOLD_LINKS];
mobj_t *doom_told_gone[TOLD_GONE];
int doom_told_changes, doom_told_links, doom_told_goings;
static unsigned told_view = 1;		/* this view's number: no thing has it before it is told of */
short *doom_told_sectors;		/* the sectors written since the last view, each once */
int doom_told_sector_count;
static unsigned char *sector_told;
static const sector_t *sectors_told;
static int sectors_told_count;
int doom_walls_changed;
int doom_level_loads;			/* the levels and saved games loaded so far */

/* The game is about to change what a view shows of a thing, or what P_RunThinkers goes by
 * (its state, its place, its heights, its speed): it is awake, and the next view is told. */
void
doom_thing_changed(mobj_t *thing)
{
	unsigned mark = thing->validcount;

	if (mark & ASLEEP)
		thing->validcount = mark &= ~ASLEEP;
	if (mark >> 12 == told_view || doom_told_all)
		return;
	if (doom_told_changes == TOLD_MOST) {
		doom_told_all = 1;
		return;
	}
	thing->validcount = told_view << 12 | doom_told_changes;
	doom_told_changed[doom_told_changes++] = thing;
}

void
doom_thing_linked(mobj_t *thing)
{
	doom_thing_changed(thing);
	if (doom_told_links == TOLD_LINKS)
		doom_told_all = 1;
	if (!doom_told_all)
		doom_told_linked[doom_told_links++] = thing;
}

void
doom_thing_gone(mobj_t *thing)
{
	const unsigned mark = thing->validcount & ~ASLEEP;

	if (doom_told_all)
		return;
	if (mark >> 12 == told_view)
		doom_told_changed[mark & 0xfff] = NULL;
	if (doom_told_goings == TOLD_GONE)
		doom_told_all = 1;
	else
		doom_told_gone[doom_told_goings++] = thing;
}

/* A view has taken the sectors, and with `things` the things: of those nothing is told any
 * more. (A view that reads the game's own things takes none, and none is noted.) */
void
doom_told_taken(int things)
{
	int i;

	if (things) {
		doom_told_changes = doom_told_links = doom_told_goings = doom_told_all = 0;
		if (++told_view == 1u << 19) {
			/* the numbers have gone round: the marks of long ago go, and every thing is told again */
			told_view = 1;
			stamps_stale = doom_told_all = 1;
		}
	}
	for (i = 0; i < doom_told_sector_count; i++)
		sector_told[doom_told_sectors[i]] = 0;
	doom_told_sector_count = 0;
}

/* The level the view keeps is this one: its sectors are told of from now on. */
void
doom_told_level(void)
{
	free(sector_told);
	free(doom_told_sectors);
	sector_told = calloc(numsectors + 1, 1);
	doom_told_sectors = malloc((numsectors + 1) * sizeof *doom_told_sectors);
	sectors_told = sectors;
	sectors_told_count = numsectors;
	doom_told_sector_count = 0;
	doom_walls_changed = 0;
}

void
doom_sector_changed(void *of)
{
	const sector_t *sector = of;
	int s = sector - sectors_told;

	if (sector < sectors_told || (unsigned)s >= (unsigned)sectors_told_count || sector_told[s])
		return;
	sector_told[s] = 1;
	doom_told_sectors[doom_told_sector_count++] = s;
}

result_e T_MovePlane_port(sector_t *sector, fixed_t speed, fixed_t dest, boolean crush, int floorOrCeiling, int direction);
void P_InitThinkers_port(void);

/* Every floor, ceiling, door and lift moves through here. */
result_e
T_MovePlane(sector_t *sector, fixed_t speed, fixed_t dest, boolean crush, int floorOrCeiling, int direction)
{
	doom_sector_changed(sector);
	return T_MovePlane_port(sector, speed, dest, crush, floorOrCeiling, direction);
}

/* The game empties its thinkers when it sets a level up or loads a saved game: the kept level
 * is made anew, and the things told of so far are no more. */
void
P_InitThinkers(void)
{
	P_InitThinkers_port();
	doom_level_loads++;
	doom_told_all = stamps_stale = 1;
	sectors_told = NULL;
	sectors_told_count = 0;
}

/*
 * a / b in 16.16, for |a| >> 14 < |b| (FixedDiv has seen to that), rounded toward zero: the
 * whole part by one division, the sixteen bits after it as many at a time as fit. The
 * remainder is below b, so it can move up by the bits b leaves free at the top.
 */
fixed_t
FixedDiv2(fixed_t a, fixed_t b)
{
	unsigned ua = a < 0 ? -(unsigned)a : (unsigned)a, ub = b < 0 ? -(unsigned)b : (unsigned)b;
	unsigned q = ua / ub, r = ua % ub, top = ub;
	int i = 0, room = 0;

	if (!(top >> 16))
		room = 16;
	else {
		if (!(top >> 24))
			room += 8, top <<= 8;
		if (!(top >> 28))
			room += 4, top <<= 4;
		if (!(top >> 30))
			room += 2, top <<= 2;
		if (!(top >> 31))
			room++;
	}
	while (room && i < 16) {
		int step = 16 - i < room ? 16 - i : room;

		r <<= step;
		q = q << step | r / ub;
		r %= ub;
		i += step;
	}
	for (; i < 16; i++) {
		unsigned carry = r >> 31;

		r <<= 1;
		q <<= 1;
		if (carry || r >= ub) {
			r -= ub;
			q |= 1;
		}
	}
	return (a ^ b) < 0 ? -(fixed_t)q : (fixed_t)q;
}

/* ---- which subsector a point is in ---- */

subsector_t *R_PointInSubsector_port(fixed_t x, fixed_t y);

/* As the port's R_PointOnSide. */
static inline int
point_side(fixed_t x, fixed_t y, const node_t *node)
{
	fixed_t dx, dy, left, right;

	if (!node->dx)
		return x <= node->x ? node->dy > 0 : node->dy < 0;
	if (!node->dy)
		return y <= node->y ? node->dx < 0 : node->dx > 0;
	dx = x - node->x;
	dy = y - node->y;
	if ((node->dy ^ node->dx ^ dx ^ dy) & 0x80000000)
		return ((node->dy ^ dx) & 0x80000000) != 0;
	left = (fixed_t)(((long long)(node->dy >> FRACBITS) * dx) >> FRACBITS);
	right = (fixed_t)(((long long)dy * (node->dx >> FRACBITS)) >> FRACBITS);
	return right >= left;
}

/*
 * The deepest node every point of a blockmap cell comes to. The side is one for the whole
 * cell when it is one at the corners of each part the node's own x and y cut the cell into:
 * inside such a part the answer only goes one way with x and one way with y.
 */
static int
cell_search(int cell)
{
	const fixed_t x0 = bmaporgx + ((cell % bmapwidth) << MAPBLOCKSHIFT), x1 = x0 + (1 << MAPBLOCKSHIFT) - 1;
	const fixed_t y0 = bmaporgy + ((cell / bmapwidth) << MAPBLOCKSHIFT), y1 = y0 + (1 << MAPBLOCKSHIFT) - 1;
	int nodenum = numnodes - 1;

	while (!(nodenum & NF_SUBSECTOR)) {
		const node_t *node = &nodes[nodenum];
		fixed_t xs[4] = { x0, x1 }, ys[4] = { y0, y1 };
		int nx = 2, ny = 2, i, k, side;

		if (node->x > x0 && node->x <= x1) {
			xs[2] = node->x - 1;
			xs[3] = node->x;
			nx = 4;
		}
		if (node->y > y0 && node->y <= y1) {
			ys[2] = node->y - 1;
			ys[3] = node->y;
			ny = 4;
		}
		side = point_side(x0, y0, node);
		for (i = 0; i < nx; i++)
			for (k = 0; k < ny; k++)
				if (point_side(xs[i], ys[k], node) != side)
					return nodenum;
		nodenum = node->children[side];
	}
	return nodenum;
}

/*
 * As the port's, which walks the tree from its root for every move a thing tries: the walk
 * starts at the node the point's blockmap cell is all inside of (found when a cell is first
 * asked about), and the point asked about last is remembered (a move asks twice).
 */
subsector_t *
R_PointInSubsector(fixed_t x, fixed_t y)
{
	static int *cell_node, loads = -1;	/* per cell: that node's number plus one, 0 for not known */
	static fixed_t last_x, last_y;
	static subsector_t *last;
	int cx, cy, nodenum;

	if (use_port_sight < 0)
		which_port();
	if (!numnodes || use_port_find)
		return R_PointInSubsector_port(x, y);
	if (loads != doom_level_loads) {
		loads = doom_level_loads;
		free(cell_node);
		cell_node = calloc(bmapwidth * bmapheight + 1, sizeof *cell_node);
		last = NULL;
	}
	if (x == last_x && y == last_y && last)
		return last;
	cx = (x - bmaporgx) >> MAPBLOCKSHIFT;
	cy = (y - bmaporgy) >> MAPBLOCKSHIFT;
	nodenum = numnodes - 1;
	if ((unsigned)cx < (unsigned)bmapwidth && (unsigned)cy < (unsigned)bmapheight && cell_node) {
		int *known = &cell_node[cy * bmapwidth + cx];

		if (!*known)
			*known = cell_search(cy * bmapwidth + cx) + 1;
		nodenum = *known - 1;
	}
	while (!(nodenum & NF_SUBSECTOR)) {
		const node_t *node = &nodes[nodenum];

		nodenum = node->children[point_side(x, y, node)];
	}
	last_x = x;
	last_y = y;
	return last = &subsectors[nodenum & ~NF_SUBSECTOR];
}

/* ---- names ---- */

int W_CheckNumForName_port(char *name);
int R_CheckTextureNumForName_port(char *name);
void R_InitSpriteLumps_port(void);
ssize_t pread(int, void *, size_t, off_t);	/* (unistd.h and p_spec.h both have a `close`) */
extern char *reloadname;
extern int numtextures;
extern struct { char name[8]; } **textures;	/* r_data.c's texture_t begins with its name */

struct named { unsigned a, b; int number; };	/* up to eight letters as two words; number < 0: free */

static unsigned
name_hash(unsigned a, unsigned b)
{
	return (a * 2654435761u ^ b * 2246822519u) >> 12;
}

/* The first `length` letters of a name, capitals, as two words. */
static void
name_words(const char *name, int length, unsigned *a, unsigned *b)
{
	union { char s[8]; unsigned x[2]; } u = { { 0 } };
	int i;

	for (i = 0; i < length; i++)
		u.s[i] = name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i];
	*a = u.x[0];
	*b = u.x[1];
}

static int
name_find(const struct named *table, unsigned mask, unsigned a, unsigned b)
{
	unsigned h = name_hash(a, b) & mask;

	for (; table[h].number >= 0; h = (h + 1) & mask)
		if (table[h].a == a && table[h].b == b)
			return table[h].number;
	return -1;
}

/* Puts a name in unless it is there: the first number given for a name stays. */
static void
name_add(struct named *table, unsigned mask, unsigned a, unsigned b, int number)
{
	unsigned h = name_hash(a, b) & mask;

	for (; table[h].number >= 0; h = (h + 1) & mask)
		if (table[h].a == a && table[h].b == b)
			return;
	table[h].a = a;
	table[h].b = b;
	table[h].number = number;
}

static struct named *
name_table(unsigned names, unsigned *mask)
{
	struct named *table;
	unsigned size = 64, i;

	while (size < names * 3)
		size *= 2;
	table = malloc(size * sizeof *table);
	for (i = 0; table && i < size; i++)
		table[i].number = -1;
	*mask = size - 1;
	return table;
}

/*
 * A lump's number by its name, the last lump of that name. The port compares the name with
 * every lump's, some 3,000 instructions a search; here a table of the names answers, made
 * again whenever lumps have been added.
 */
int
W_CheckNumForName(char *name)
{
	static struct named *table;
	static lumpinfo_t *of;
	static int count = -1;
	static unsigned mask;
	unsigned a, b;
	int i, length = 0;

	if (use_port_sight < 0)
		which_port();
	if (use_port_names || reloadname)
		return W_CheckNumForName_port(name);
	if (of != lumpinfo || count != numlumps) {
		free(table);
		table = name_table(numlumps, &mask);
		if (!table)
			return W_CheckNumForName_port(name);
		for (i = numlumps - 1; i >= 0; i--)
			name_add(table, mask, *(unsigned *)lumpinfo[i].name, *(unsigned *)&lumpinfo[i].name[4], i);
		of = lumpinfo;
		count = numlumps;
	}
	while (length < 8 && name[length])
		length++;
	name_words(name, length, &a, &b);
	return name_find(table, mask, a, b);
}

/*
 * A texture's number by its name, the first that fits. The port's comparison stops at the
 * end of the shorter name, so a name fits every texture it begins, and every texture whose
 * name begins it: `begins` has all the beginnings of every texture's name, `whole` the names.
 */
int
R_CheckTextureNumForName(char *name)
{
	static struct named *whole, *begins;
	static void *of;
	static int count = -1, nameless;
	static unsigned whole_mask, begins_mask;
	unsigned a, b;
	int i, k, length = 0, found, other;

	if (name[0] == '-')
		return 0;
	if (use_port_sight < 0)
		which_port();
	if (use_port_names)
		return R_CheckTextureNumForName_port(name);
	if (of != (void *)textures || count != numtextures) {
		free(whole);
		free(begins);
		whole = name_table(numtextures, &whole_mask);
		begins = name_table(numtextures * 8, &begins_mask);
		if (!whole || !begins) {
			free(whole);
			free(begins);
			whole = begins = NULL;
			count = -1;
			return R_CheckTextureNumForName_port(name);
		}
		nameless = -1;
		for (i = 0; i < numtextures; i++) {
			for (k = 0; k < 8 && textures[i]->name[k]; k++) {
				name_words(textures[i]->name, k + 1, &a, &b);
				name_add(begins, begins_mask, a, b, i);
			}
			if (!k && nameless < 0)
				nameless = i;
			name_words(textures[i]->name, k, &a, &b);
			name_add(whole, whole_mask, a, b, i);
		}
		of = textures;
		count = numtextures;
	}
	while (length < 8 && name[length])
		length++;
	if (!length)
		return numtextures > 0 ? 0 : -1;	/* no name at all fits the first texture */
	name_words(name, length, &a, &b);
	found = name_find(begins, begins_mask, a, b);
	if (nameless >= 0 && (found < 0 || nameless < found))
		found = nameless;
	for (k = 1; k < length; k++) {
		name_words(name, k, &a, &b);
		other = name_find(whole, whole_mask, a, b);
		if (other >= 0 && (found < 0 || other < found))
			found = other;
	}
	return found;
}

/*
 * Each sprite's width and offsets. The port reads every sprite whole for the eight bytes at
 * its head, 1.4 MB of the shareware game's data; here those eight bytes are read.
 */
void
R_InitSpriteLumps(void)
{
	int i;

	if (use_port_sight < 0)
		which_port();
	if (use_port_names || reloadname) {
		R_InitSpriteLumps_port();
		return;
	}
	firstspritelump = W_GetNumForName("S_START") + 1;
	lastspritelump = W_GetNumForName("S_END") - 1;
	numspritelumps = lastspritelump - firstspritelump + 1;
	spritewidth = Z_Malloc(numspritelumps * sizeof(void *), PU_STATIC, 0);
	spriteoffset = Z_Malloc(numspritelumps * sizeof(void *), PU_STATIC, 0);
	spritetopoffset = Z_Malloc(numspritelumps * sizeof(void *), PU_STATIC, 0);
	for (i = 0; i < numspritelumps; i++) {
		const lumpinfo_t *lump = &lumpinfo[firstspritelump + i];
		short head[4];
		const patch_t *patch = (const patch_t *)head;

		if (lump->size < 8 || !lump->handle || pread(fileno(lump->handle), head, 8, lump->position) != 8)
			patch = W_CacheLumpNum(firstspritelump + i, PU_CACHE);
		spritewidth[i] = SHORT(patch->width) << FRACBITS;
		spriteoffset[i] = SHORT(patch->leftoffset) << FRACBITS;
		spritetopoffset[i] = SHORT(patch->topoffset) << FRACBITS;
	}
}

/* ---- the lines of a blockmap cell ---- */

boolean P_BlockLinesIterator_port(int x, int y, boolean (*func)(line_t *));
boolean PIT_CheckLine(line_t *ld);
extern fixed_t tmbbox[4];

/*
 * As the port's. For a move's check, which is nearly every call, a line whose box the moving
 * thing's box is clear of is passed by here: the check would begin with that and return.
 */
boolean
P_BlockLinesIterator(int x, int y, boolean (*func)(line_t *))
{
	const fixed_t left = tmbbox[BOXLEFT], right = tmbbox[BOXRIGHT], top = tmbbox[BOXTOP], bottom = tmbbox[BOXBOTTOM];
	const int count = validcount;
	const short *list;

	if (func != PIT_CheckLine || use_port_lines)
		return P_BlockLinesIterator_port(x, y, func);
	if (x < 0 || y < 0 || x >= bmapwidth || y >= bmapheight)
		return true;
	for (list = blockmaplump + blockmap[y * bmapwidth + x]; *list != -1; list++) {
		line_t *ld = &lines[*list];

		if (doom_line_mark[*list] == count)
			continue;
		doom_line_mark[*list] = count;
		if (right <= ld->bbox[BOXLEFT] || left >= ld->bbox[BOXRIGHT] || top <= ld->bbox[BOXBOTTOM] || bottom >= ld->bbox[BOXTOP])
			continue;
		if (!PIT_CheckLine(ld))
			return false;
	}
	return true;
}

/* ---- the things of a blockmap cell ---- */

boolean P_BlockThingsIterator_port(int x, int y, boolean (*func)(mobj_t *));
boolean PIT_CheckThing(mobj_t *thing);
extern mobj_t *tmthing;
extern fixed_t tmx, tmy;

/*
 * As the port's. For a move's check a thing that cannot be in the way (nothing solid, or out
 * of reach, or the mover itself) is passed by here: the check would begin with that and return.
 */
boolean
P_BlockThingsIterator(int x, int y, boolean (*func)(mobj_t *))
{
	mobj_t *mobj;

	if (func != PIT_CheckThing || use_port_lines)
		return P_BlockThingsIterator_port(x, y, func);
	if (x < 0 || y < 0 || x >= bmapwidth || y >= bmapheight)
		return true;
	for (mobj = blocklinks[y * bmapwidth + x]; mobj; mobj = mobj->bnext) {
		fixed_t reach;

		if (!(mobj->flags & (MF_SOLID | MF_SPECIAL | MF_SHOOTABLE)))
			continue;
		reach = mobj->radius + tmthing->radius;
		if (abs(mobj->x - tmx) >= reach || abs(mobj->y - tmy) >= reach || mobj == tmthing)
			continue;
		if (!PIT_CheckThing(mobj))
			return false;
	}
	return true;
}

/* ---- a level's lines by sector ---- */

void P_GroupLines_port(void);

/*
 * As the port's, which looks at every line of the level for every sector (1.2M instructions
 * for the shareware game's first levels): here each line goes to its one or two sectors, in
 * the order of the lines, which the boxes depend on (M_AddToBox is not the same in any order).
 */
void
P_GroupLines(void)
{
	line_t **linebuffer, *li;
	fixed_t (*box)[4];
	sector_t *sector;
	int i, k, total = 0;

	if (use_port_sight < 0)
		which_port();
	if (use_port_group) {
		P_GroupLines_port();
		return;
	}
	for (i = 0; i < numsubsectors; i++)
		subsectors[i].sector = segs[subsectors[i].firstline].sidedef->sector;
	for (i = 0, li = lines; i < numlines; i++, li++) {
		total++;
		li->frontsector->linecount++;
		if (li->backsector && li->backsector != li->frontsector) {
			li->backsector->linecount++;
			total++;
		}
	}
	linebuffer = Z_Malloc(total * sizeof(void *), PU_LEVEL, 0);
	box = malloc((numsectors + 1) * sizeof *box);
	for (i = 0, sector = sectors; i < numsectors; i++, sector++) {
		M_ClearBox(box[i]);
		sector->lines = linebuffer;	/* (where its next line goes, until all are in) */
		linebuffer += sector->linecount;
	}
	for (i = 0, li = lines; i < numlines; i++, li++)
		for (k = 0; k < 2; k++) {
			sector = k ? li->backsector : li->frontsector;
			if (k && (!sector || sector == li->frontsector))
				break;
			*sector->lines++ = li;
			M_AddToBox(box[sector - sectors], li->v1->x, li->v1->y);
			M_AddToBox(box[sector - sectors], li->v2->x, li->v2->y);
		}
	for (i = 0, sector = sectors; i < numsectors; i++, sector++) {
		const fixed_t *bbox = box[i];
		int block;

		sector->lines -= sector->linecount;
		sector->soundorg.x = (bbox[BOXRIGHT] + bbox[BOXLEFT]) / 2;
		sector->soundorg.y = (bbox[BOXTOP] + bbox[BOXBOTTOM]) / 2;
		block = (bbox[BOXTOP] - bmaporgy + MAXRADIUS) >> MAPBLOCKSHIFT;
		sector->blockbox[BOXTOP] = block >= bmapheight ? bmapheight - 1 : block;
		block = (bbox[BOXBOTTOM] - bmaporgy - MAXRADIUS) >> MAPBLOCKSHIFT;
		sector->blockbox[BOXBOTTOM] = block < 0 ? 0 : block;
		block = (bbox[BOXRIGHT] - bmaporgx + MAXRADIUS) >> MAPBLOCKSHIFT;
		sector->blockbox[BOXRIGHT] = block >= bmapwidth ? bmapwidth - 1 : block;
		block = (bbox[BOXLEFT] - bmaporgx - MAXRADIUS) >> MAPBLOCKSHIFT;
		sector->blockbox[BOXLEFT] = block < 0 ? 0 : block;
	}
	free(box);
}
