/* The path tracer's scene and its job for a worker core (tracer.c, docs/raytrace.md). */
#ifndef PATHTRACE_TRACER_H
#define PATHTRACE_TRACER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { PT_DIFFUSE, PT_MIRROR, PT_GLASS };

/* A model of triangles and the tree of boxes over them, made by make_meshes.py where the
 * model stands in the box. A box holds the triangles from `a`, `b` of them, or (b 0) the
 * two boxes from `a`. A triangle is a corner, its two edges from there, and the corners'
 * normals, which a point between them has a mean of. */
union pt_word { uint32_t u; float f; };
struct pt_node { float low[3]; uint32_t a; float high[3]; uint32_t b; };
struct pt_triangle { float corner[3], edge1[3], edge2[3], normal[3][3]; };
struct pt_mesh {
	const char *name;
	const struct pt_node *nodes;
	const struct pt_triangle *triangles;
	int node_count, triangle_count;
	float colour[3];
};
extern struct pt_mesh pt_meshes[];
extern int pt_mesh_count;

/* How far a worker is with its share: its own 16 bytes, which the core that shows reads. */
struct pt_slot { volatile uint32_t samples, rays, unused[2]; };

/* What a picture is traced with: written between two pictures by the core that hands the
 * tiles out, only read by the ones that trace. */
struct pt_scene {
	int width, height;	/* the width a multiple of four */
	int tile_w, tile_h;	/* a tile: 32 pixels square, or as much smaller as gives a worker two dozen (eight wide at least) */
	int tiles;
	uint32_t stride, step;	/* the order the tiles are traced in: pt_share */
	int samples;		/* how many of every pixel the picture is to have */
	int stop;		/* the picture is given up: a share ends with the sample it is at */
	struct pt_slot *slots;	/* one a worker, by its number */
	int bounces;		/* how many times a path may turn */
	int material[2];	/* the two balls, or the model and its ball: PT_DIFFUSE, PT_MIRROR or PT_GLASS */
	int scene;		/* 0: two balls and a block; from 1, a model of pt_meshes and one ball */
	int owners;		/* each tile framed in its core's colour */
	int depth;		/* a test's picture: how far the eye's ray goes, in 4,000,000ths (tools/pathtrace_reference.py) */
	float light;		/* how bright the lamp is */
	uint32_t seed;
	float *sums;		/* three floats a pixel: the light of the samples so far */
	uint32_t *pixels;	/* the picture, 0x00RRGGBB */
};
extern struct pt_scene pt;

/* One more sample of every pixel of a tile: `how` is the sample's number (from 0), with the
 * core's number in its top byte. Returns the rays cast. */
uint32_t pt_tile(uint32_t tile, uint32_t how);

/* A worker's job: its share of the picture, sample after sample from where its slot says it
 * is, to the picture's last. which: the worker's place w (from 0) among n, n from bit 16; it
 * has every n-th tile of an order that scatters them. how: its number from bit 24, and below
 * that how many samples at most in this call (0: all). It ends early when the kernel wants
 * its core back, to be posted again. */
uint32_t pt_share(uint32_t which, uint32_t how);

/* For the core that hands the shares out, before it does: the tracer's code read, so that no
 * worker stops at a page of it. */
void pt_read_code(void);

/* The same three with the arithmetic of before 13 October 2026 (tracer.c built with PT_OLD):
 * another sequence of numbers, a quotient for every wall and side. NXPATH_OLD=1; its sums
 * are docs/raytrace.md's old ones. */
uint32_t pt_tile_old(uint32_t tile, uint32_t how);
uint32_t pt_share_old(uint32_t which, uint32_t how);
void pt_read_code_old(void);

#ifdef __cplusplus
}
#endif
#endif
