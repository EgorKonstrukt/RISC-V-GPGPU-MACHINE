/*
 * nxpath [MESH]: a mesh viewer with a path tracer (docs/raytrace.md). Meshes are read from .obj and
 * .ply files and put into a scene with lights; the picture on the left is OpenGL's until
 * Render, then the path tracer's, traced by the worker cores under a tree of boxes they built.
 * The panel on the right is the window system's own; Dear ImGui draws only the handles.
 * For tests: NXPATH_AUTO=1 renders at the start, NXPATH_EXIT=1 leaves when the picture is done
 * (its line is "pathstat:"), NXPATH_SIZE=WxH, NXPATH_SAMPLES=N, NXPATH_CORES=0 (no workers),
 * NXPATH_SAVE=FILE (the picture written when done), NXPATH_PRINT=1 (a line for every change),
 * NXPATH_CHECK=N (N of the eye's rays through the tree and without it: the objects must agree).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES/segl.h>
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_files.h"
#include "mcw.h"
#include "scene.h"

#define PANEL 250		/* the panel's width, right of the picture */
#define STATUS 18		/* the line of figures along the bottom */
#define ROWS 7			/* of the scene's list */
#define MOST_W 640		/* the largest picture traced */
#define MOST_H 480
#define ONE 65536
#define SIZES 5
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE)

static const int sizes[SIZES][2] = {{160, 120}, {240, 180}, {320, 240}, {480, 360}, {640, 480}};
static const int sample_steps[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
#define SAMPLE_STEPS 10
static const char *const material_names[3] = {" Matt", " Mirror", " Glass"};
static const char *const what_names[3] = {" Move", " Turn", " Size"};

static GR_WINDOW_ID window, picture;
static int wide = 770, high = 480, view_w, view_h;	/* the window, and the picture in it */
static struct ui_list tree = {0, 30, PANEL - 8, ROWS * UI_ROW + 4, 0, 0, -1};
static int tab;			/* the panel's lower part: 0 what is selected, 1 the picture's settings */
static int what = GIZMO_MOVE;	/* what the handles do */
static int size_at = 2, samples_at = 4, bounces = 5;
static char status[160] = "";
static int view_dirty = 1, held;	/* held: a slider of the panel, by its number + 1 */
static int on_handle;		/* the pointer is over a handle, or holds one: gizmo_frame() said so */
static int drag, drag_x, drag_y;	/* a button held in the picture (1 turns the view, 2 moves it), and where the pointer was */
static int have_gizmo;

/* the two kinds of thing a row of the list is: the objects first, then the lights */
static int chosen_object(void) { return tree.selected >= 0 && tree.selected < sc.object_count ? tree.selected : -1; }
static int chosen_light(void) { return tree.selected >= sc.object_count ? tree.selected - sc.object_count : -1; }

/* ---- the worker cores ---- */

static int workers;
static uint8_t busy[MC_MAX_CORES];

static unsigned
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

static void
take_workers(int most)
{
	extern char __DATA_BEGIN__[], _end[];
	extern void mcw_entry(void);

	mcw_close();
	workers = 0;
	if (most <= 0)
		return;
	/* (where a worker begins is read first: on a page of code this core has not run, every one stopped there) */
	(void)*(const volatile char *)mcw_entry;
	workers = mcw_open(most);
	if (workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
}

/* ---- the scene as OpenGL draws it ---- */

/* A mesh for the GPU: three vertices a triangle where they stay (seglTagged), a vertex's tag
 * the one of 256 directions its normal is nearest. An object's table has a colour for each. */
struct shown_mesh { unsigned int *vertices; int count; };
static unsigned int *tables;	/* 256 words an object, then a light: SC_OBJECTS + SC_LIGHTS of them */
static int ball_mesh = -1;	/* what a light is drawn as */

/* The direction a tag stands for: sixteen by sixteen places of an octahedron unfolded. */
static void
tag_way(int tag, float *n)
{
	float u = ((float)(tag & 15) + 0.5f) / 8.0f - 1.0f, w = ((float)(tag >> 4) + 0.5f) / 8.0f - 1.0f;
	float y = 1.0f - fabsf(u) - fabsf(w), len;

	if (y < 0.0f) {
		float fu = u, fw = w;

		u = fu >= 0.0f ? 1.0f - fabsf(fw) : fabsf(fw) - 1.0f, w = fw >= 0.0f ? 1.0f - fabsf(fu) : fabsf(fu) - 1.0f;
	}
	len = sqrtf(u * u + y * y + w * w);
	n[0] = u / len, n[1] = y / len, n[2] = w / len;
}

static int
way_tag(const float *n)
{
	float sum = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]), u, w;
	int iu, iw;

	if (sum < 1e-12f)
		return 0;
	u = n[0] / sum, w = n[2] / sum;
	if (n[1] < 0.0f) {
		float fu = u, fw = w;

		u = fu >= 0.0f ? 1.0f - fabsf(fw) : fabsf(fw) - 1.0f, w = fw >= 0.0f ? 1.0f - fabsf(fu) : fabsf(fu) - 1.0f;
	}
	iu = (int)((u + 1.0f) * 8.0f), iw = (int)((w + 1.0f) * 8.0f);
	return (iu > 15 ? 15 : iu < 0 ? 0 : iu) | (iw > 15 ? 15 : iw < 0 ? 0 : iw) << 4;
}

/* A mesh's vertices for the GPU, made once; false when the GPU's memory is used up. */
static int
show_mesh(int m)
{
	struct sc_mesh *mesh = &sc.meshes[m];
	struct shown_mesh *shown;
	int i, count = mesh->triangles * 3;

	if (mesh->shown)
		return 1;
	shown = malloc(sizeof *shown);
	shown->count = count;
	shown->vertices = seglMemory((unsigned)count * 16);
	if (!shown->vertices) {
		free(shown);
		return 0;
	}
	for (i = 0; i < count; i++) {
		memcpy(&shown->vertices[i * 4], &mesh->positions[i * 3], 12);
		shown->vertices[i * 4 + 3] = (unsigned)way_tag(&mesh->normals[i * 3]) << 24;
	}
	mesh->shown = shown;
	return 1;
}

/* A table's colours: the colour lit from above and a little from the eye's side, by each
 * direction as the matrix turns it. `glow`: its own light (a light, or what is selected). */
static void
fill_table(unsigned int *table, const float *matrix, const float *colour, float glow)
{
	static const float light[3] = {0.35f, 0.85f, -0.4f};
	int tag, k;

	seglSettle();	/* (the list in flight reads the table) */
	for (tag = 0; tag < 256; tag++) {
		float n[3], w[3], len, lit, c[3];

		tag_way(tag, n);
		for (k = 0; k < 3; k++)
			w[k] = matrix[k] * n[0] + matrix[4 + k] * n[1] + matrix[8 + k] * n[2];
		len = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
		lit = len > 0.0f ? (w[0] * light[0] + w[1] * light[1] + w[2] * light[2]) / len : 0.0f;
		lit = 0.35f + 0.65f * (lit > 0.0f ? lit : -0.25f * lit) + glow;
		for (k = 0; k < 3; k++)
			c[k] = colour[k] * lit > 1.0f ? 1.0f : colour[k] * lit;
		table[tag] = (unsigned)(c[0] * 255.0f) << 16 | (unsigned)(c[1] * 255.0f) << 8 | (unsigned)(c[2] * 255.0f);
	}
}

static void
light_matrix(const struct sc_light *l, float *m)
{
	memset(m, 0, 16 * sizeof *m);
	m[0] = m[5] = m[10] = l->radius * 2.0f, m[15] = 1.0f;	/* (the ball is of radius one half) */
	m[12] = l->place[0], m[13] = l->place[1], m[14] = l->place[2];
}

/* Every table again: an object turned, coloured or picked out otherwise than it was. */
static void
fill_tables(void)
{
	int i;

	for (i = 0; i < sc.object_count; i++) {
		struct sc_object *o = &sc.objects[i];
		float c[3] = {o->colour[0], o->colour[1], o->colour[2]};

		if (o->material == SC_MIRROR)
			c[0] = 0.5f + 0.5f * c[0], c[1] = 0.5f + 0.5f * c[1], c[2] = 0.55f + 0.45f * c[2];
		if (o->material == SC_GLASS)
			c[0] = 0.7f * c[0], c[1] = 0.85f * c[1], c[2] = 0.6f + 0.4f * c[2];
		fill_table(tables + 256 * i, o->matrix, c, i == chosen_object() ? 0.25f : 0.0f);
	}
	for (i = 0; i < sc.light_count; i++) {
		float m[16];

		light_matrix(&sc.lights[i], m);
		fill_table(tables + 256 * (SC_OBJECTS + i), m, sc.lights[i].colour, i == chosen_light() ? 1.2f : 0.9f);
	}
	view_dirty = 1;
}

static void
view_matrices(float *view, float *projection)
{
	const struct sc_camera *c = &sc.camera;
	float x = c->lens * 0.5f * (float)view_w / (float)(view_h > 0 ? view_h : 1), y = c->lens * 0.5f, near = 0.5f, far = 100.0f;
	int k;

	for (k = 0; k < 3; k++)
		view[4 * k] = c->right[k], view[4 * k + 1] = c->up[k], view[4 * k + 2] = -c->ahead[k], view[4 * k + 3] = 0.0f;
	view[12] = -(c->right[0] * c->eye[0] + c->right[1] * c->eye[1] + c->right[2] * c->eye[2]);
	view[13] = -(c->up[0] * c->eye[0] + c->up[1] * c->eye[1] + c->up[2] * c->eye[2]);
	view[14] = c->ahead[0] * c->eye[0] + c->ahead[1] * c->eye[1] + c->ahead[2] * c->eye[2];
	view[15] = 1.0f;
	memset(projection, 0, 16 * sizeof *projection);
	projection[0] = near / x, projection[5] = near / y;
	projection[10] = -(far + near) / (far - near), projection[11] = -1.0f, projection[14] = -2.0f * far * near / (far - near);
}

/* ---- the path tracer's picture ---- */

static GLuint texture;
static int tracing, traced, showing;	/* a picture is being traced; one is there; it is what the window shows */
static int next_tile, sample, out;	/* this core alone: where it is; with workers: how many jobs are out */
static unsigned began, shown_at, rays;
static int leave_when_done;

static void draw_panel(void);

static void
say(const char *text)
{
	snprintf(status, sizeof status, "%s", text);
	ui_fill(window, 0, high - STATUS, wide, STATUS, UI_FACE);
	ui_text_fit(window, 6, high - STATUS + 3, wide - 12, status, BLACK);
	GrFlush();
}

/* The picture over the window's left part, as large as fits with its own shape. */
static void
show_traced(void)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	static const GLfixed corners[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	int w = view_w, h = view_w * pt.height / pt.width;

	if (h > view_h)
		h = view_h, w = view_h * pt.width / pt.height;
	glViewport(0, 0, view_w, view_h);
	glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glViewport((view_w - w) / 2, (view_h - h) / 2, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(corners, uv, texture, 255, 255, 0);
	seglSwap();
	shown_at = now_ms();
}

/* For a test: the object each of `count` of the eye's rays meets by the tree, and by trying
 * every triangle. A ray that meets a light first is not compared. */
static void
check_tree(int count)
{
	const struct sc_camera *c = &sc.camera;
	int i, k, wrong = 0, w = sizes[size_at][0], h = sizes[size_at][1];

	for (i = 0; i < count; i++) {
		int x = (int)((unsigned)i * 7919u % (unsigned)w), y = (int)((unsigned)i * 104729u % (unsigned)h), by_tree = pt_pick((float)x + 0.5f, (float)y + 0.5f, w, h), slow;
		float sx = (2.0f * ((float)x + 0.5f) / (float)w - 1.0f) * c->lens * (float)w / (float)h, sy = (1.0f - 2.0f * ((float)y + 0.5f) / (float)h) * c->lens;
		float way[3], len = 0.0f, far;

		for (k = 0; k < 3; k++)
			way[k] = c->ahead[k] + c->right[k] * sx + c->up[k] * sy, len += way[k] * way[k];
		for (len = sqrtf(len), k = 0; k < 3; k++)
			way[k] /= len;
		slow = sc_world_nearest_slow(c->eye, way, &far);
		wrong += by_tree >= 0 && by_tree != (slow < 0 ? SC_OBJECTS : (int)sc_world.triangles[slow].object);
	}
	fprintf(stderr, "nxpath: %d of %d rays met another object by the tree than without it\n", wrong, count);
}

static void
begin_render(void)
{
	char text[120];
	int s, tiles;

	if (tracing)
		return;
	say("Building the tree of boxes...");
	if (!sc_world_build(workers)) {
		say("No memory for the scene's triangles.");
		return;
	}
	seglSettle();
	sc_camera_set(&sc.camera);
	pt.width = sizes[size_at][0], pt.height = sizes[size_at][1];
	pt.bounces = bounces;
	pt.seed = 0x9e3779b9u;
	pt.owners = 0;
	glBindTexture(GL_TEXTURE_2D, texture);
	seglTexturePointer(pt.pixels, pt.width, pt.height, GL_RGBA);
	memset(pt.pixels, 0, (size_t)(pt.width * pt.height * 4));
	if (workers)
		mcw_touch(pt.sums, (unsigned)(pt.width * pt.height) * 3 * sizeof(float));
	pt_read_all();
	if (getenv("NXPATH_CHECK"))
		check_tree(atoi(getenv("NXPATH_CHECK")));
	for (s = 32; s > 1 && ((pt.width + (s < 8 ? 8 : s) - 1) / (s < 8 ? 8 : s)) * ((pt.height + s - 1) / s) < 24 * workers; s >>= 1)
		continue;
	pt.tile_h = s;
	pt.tile_w = s < 8 ? 8 : s;
	pt.tiles = tiles = ((pt.width + pt.tile_w - 1) / pt.tile_w) * ((pt.height + pt.tile_h - 1) / pt.tile_h);
	/* a stride with no factor in common with the number of tiles visits every one of them */
	for (pt.stride = (uint32_t)tiles * 5 / 8 + 1;; pt.stride++) {
		uint32_t a = pt.stride, b = (uint32_t)tiles;

		while (b) { uint32_t r = a % b; a = b; b = r; }
		if (a == 1)
			break;
	}
	pt.step = (uint32_t)(workers ? workers : 1) * pt.stride % (uint32_t)tiles;
	pt.samples = sample_steps[samples_at];
	pt.stop = 0;
	memset(pt.slots, 0, MC_MAX_CORES * sizeof(struct pt_slot));
	memset(busy, 0, sizeof busy);
	next_tile = sample = out = 0, rays = 0;
	tracing = traced = showing = 1;
	began = now_ms();
	snprintf(text, sizeof text, "%d triangles under %d boxes, built in %u ms by %u workers. Tracing...", sc_world.triangle_count,
		 sc_world.node_count, (unsigned)sc_world.build_ms, (unsigned)sc_world.build_workers);
	say(text);
	draw_panel();
}

static void
write_picture(const char *path)
{
	FILE *file = fopen(path, "wb");
	int i;

	if (!file) {
		say("The picture could not be written there.");
		return;
	}
	fprintf(file, "P6\n%d %d\n255\n", pt.width, pt.height);
	for (i = 0; i < pt.width * pt.height; i++) {
		fputc(pt.pixels[i] >> 16 & 255, file);
		fputc(pt.pixels[i] >> 8 & 255, file);
		fputc(pt.pixels[i] & 255, file);
	}
	fclose(file);
}

/* The picture is whole (or was given up): its figures, and what a test asked of it. */
static void
end_render(void)
{
	unsigned took = now_ms() - began, sum = 0;
	char text[160];
	int i;

	tracing = 0;
	for (i = 0; i < pt.width * pt.height; i++)
		sum = sum * 31 + pt.pixels[i];
	fprintf(stderr, "pathstat: %d x %d, %d samples, %d turns, %d triangles: tree %u ms by %u workers, picture %u ms with %d workers, %u rays, sum %08x\n",
		pt.width, pt.height, pt.samples, pt.bounces, sc_world.triangle_count, (unsigned)sc_world.build_ms, (unsigned)sc_world.build_workers, took,
		workers, rays, sum);
	snprintf(text, sizeof text, "%d x %d, %d samples: %u.%u s with %d workers, %u thousand rays a second (the tree %u ms).", pt.width, pt.height,
		 pt.samples, took / 1000, took % 1000 / 100, workers, took ? rays / took : 0, (unsigned)sc_world.build_ms);
	show_traced();
	say(text);
	if (getenv("NXPATH_SAVE"))
		write_picture(getenv("NXPATH_SAVE"));
	draw_panel();
	if (leave_when_done)
		exit(0);
}

/* The shares: every worker has all of its own at once and is asked how far it is; with no
 * workers this core traces a tile a call. */
static void
trace_on(void)
{
	static unsigned polls;
	int k, least = pt.samples;
	unsigned all = 0;

	if (!workers) {
		if (pt.stop || sample >= pt.samples) {
			if (pt.stop && sample > 0)
				pt.samples = sample;
			end_render();
			return;
		}
		rays += pt_tile((uint32_t)next_tile * pt.stride % (uint32_t)pt.tiles, (uint32_t)sample);
		if (++next_tile >= pt.tiles)
			next_tile = 0, sample++;
		return;
	}
	for (k = 1; k <= workers; k++) {
		int at = (int)pt.slots[k].samples;

		/* (asked when its slot says it is through, and now and then: its core may have been wanted back) */
		if (busy[k] && ((polls & 3) == 0 || at >= pt.samples || pt.stop) && mcw_done(k))
			busy[k] = 0, out--;
		if (!busy[k] && at < pt.samples && !pt.stop) {
			busy[k] = 1, out++;
			mcw_post(k, pt_share, (uint32_t)(k - 1) | (uint32_t)workers << 16, (uint32_t)k << 24 | (mcw_bound(k) ? 0u : 1u));
		}
		all += pt.slots[k].rays;
		if (at < least)
			least = at;
	}
	polls++;
	rays = all, sample = least;
	if (out == 0 && (least >= pt.samples || pt.stop)) {
		if (pt.stop)
			pt.samples = least > 0 ? least : 1;
		end_render();
	}
}

/* The picture's settings are the registry's (registry.h): the next start has them. */
static void
keep_settings(void)
{
	reg_set_int("pathtrace.size", size_at);
	reg_set_int("pathtrace.samples", samples_at);
	reg_set_int("pathtrace.turns", bounces);
	reg_set_int("pathtrace.sky", (int)(sc.sky_power * 100.0f + 0.5f));
}

/* ---- the scene's things ---- */

static void
name_free(char *name, const char *wanted)
{
	int n, i, taken;

	for (n = 1;; n++) {
		snprintf(name, SC_NAME, n == 1 ? "%.24s" : "%.24s %d", wanted, n);
		for (taken = 0, i = 0; i < sc.object_count; i++)
			taken |= !strcmp(sc.objects[i].name, name);
		for (i = 0; i < sc.light_count; i++)
			taken |= !strcmp(sc.lights[i].name, name);
		if (!taken)
			return;
	}
}

/* An object of a mesh, standing on the ground at the middle; `across`: how large its longest side is made. */
static int
add_object(int mesh, float across, int material, float r, float g, float b)
{
	struct sc_object *o;
	const struct sc_mesh *m;
	float longest;
	int k;

	if (mesh < 0 || sc.object_count == SC_OBJECTS || !show_mesh(mesh))
		return -1;
	m = &sc.meshes[mesh];
	o = &sc.objects[sc.object_count];
	memset(o, 0, sizeof *o);
	name_free(o->name, m->name);
	o->mesh = mesh;
	longest = m->high[0] - m->low[0];
	for (k = 1; k < 3; k++)
		if (m->high[k] - m->low[k] > longest)
			longest = m->high[k] - m->low[k];
	for (k = 0; k < 3; k++) {
		o->size[k] = longest > 0.0f ? across / longest : 1.0f;
		o->place[k] = -(m->low[k] + m->high[k]) * 0.5f * o->size[k];
	}
	o->place[1] = -m->low[1] * o->size[1];
	o->material = material;
	o->colour[0] = r, o->colour[1] = g, o->colour[2] = b;
	sc_object_matrix(o);
	return sc.object_count++;
}

static int
add_light(float x, float y, float z, float radius, float power)
{
	struct sc_light *l;

	if (sc.light_count == SC_LIGHTS)
		return -1;
	l = &sc.lights[sc.light_count];
	memset(l, 0, sizeof *l);
	name_free(l->name, "Light");
	l->place[0] = x, l->place[1] = y, l->place[2] = z;
	l->radius = radius, l->power = power;
	l->colour[0] = l->colour[1] = 1.0f, l->colour[2] = 0.95f;
	return sc.light_count++;
}

/* A mesh from a file, or the one of that file read before; -1 with the status line saying why not. */
static int
import_mesh(const char *path)
{
	const char *leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	char why[120], name[SC_NAME];
	int m;

	snprintf(name, sizeof name, "%.*s", (int)(strcspn(leaf, ".") < SC_NAME - 1 ? strcspn(leaf, ".") : SC_NAME - 1), leaf);
	for (m = 0; m < sc.mesh_count; m++)
		if (!strcmp(sc.meshes[m].name, name))
			return m;
	say("Reading the mesh...");
	m = sc_mesh_load(path, why, sizeof why);
	if (m < 0) {
		say(why);
		return -1;
	}
	snprintf(sc.meshes[m].name, SC_NAME, "%s", name);
	return m;
}

static void
choose(int row)
{
	tree.count = sc.object_count + sc.light_count;
	tree.selected = row < tree.count ? row : -1;
	ui_list_show(&tree);
	fill_tables();
	draw_panel();
}

static void
delete_chosen(void)
{
	int o = chosen_object(), l = chosen_light();

	if (o >= 0) {
		memmove(&sc.objects[o], &sc.objects[o + 1], (size_t)(sc.object_count - o - 1) * sizeof sc.objects[0]);
		sc.object_count--;
	} else if (l >= 0) {
		memmove(&sc.lights[l], &sc.lights[l + 1], (size_t)(sc.light_count - l - 1) * sizeof sc.lights[0]);
		sc.light_count--;
	}
	choose(-1);
}

/* An object's place, turn and size out of its matrix, as the handles left it. */
static void
from_matrix(struct sc_object *o)
{
	float len[3], r20, r21, r22, r10, r00;
	int k;

	for (k = 0; k < 3; k++) {
		const float *c = &o->matrix[4 * k];

		len[k] = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		o->size[k] = len[k] > 1e-6f ? len[k] : 1e-6f;
		o->place[k] = o->matrix[12 + k];
	}
	r00 = o->matrix[0] / o->size[0], r10 = o->matrix[1] / o->size[0], r20 = o->matrix[2] / o->size[0];
	r21 = o->matrix[6] / o->size[1], r22 = o->matrix[10] / o->size[2];
	o->turn[1] = asinf(r20 < -1.0f ? 1.0f : r20 > 1.0f ? -1.0f : -r20) * 57.29578f;
	o->turn[0] = atan2f(r21, r22) * 57.29578f;
	o->turn[2] = atan2f(r10, r00) * 57.29578f;
}

/* ---- the panel ---- */

static const char *
row_label(int row)
{
	static char text[SC_NAME + 8];

	if (row < sc.object_count)
		return sc.objects[row].name;
	snprintf(text, sizeof text, "* %s", sc.lights[row - sc.object_count].name);
	return text;
}

/* The values a line of three shows and its two small buttons change: an object's place, turn
 * or size, or a light's place. */
static float *
line_values(int line, float *step)
{
	int o = chosen_object(), l = chosen_light();

	*step = line == 0 ? 0.1f : line == 1 ? 15.0f : 0.0f;
	if (o >= 0)
		return line == 0 ? sc.objects[o].place : line == 1 ? sc.objects[o].turn : sc.objects[o].size;
	return l >= 0 && line == 0 ? sc.lights[l].place : NULL;
}

/* The sliders of what is shown, by number: a value from 0 to its most, and where it is kept. */
static int
slider_value(int n, int set, int value)
{
	int o = chosen_object(), l = chosen_light();
	float *f = NULL, most = 1.0f;

	if (tab == 1) {
		if (n == 0) { if (set) samples_at = value; return samples_at; }
		if (n == 1) { if (set) bounces = value + 1; return bounces - 1; }
		if (n == 2) f = &sc.sky_power, most = 2.0f;
	} else if (o >= 0 && n < 3)
		f = &sc.objects[o].colour[n];
	else if (l >= 0 && n < 3)
		f = &sc.lights[l].colour[n];
	else if (l >= 0 && n == 3)
		f = &sc.lights[l].radius, most = 1.0f;
	else if (l >= 0 && n == 4)
		f = &sc.lights[l].power, most = 100.0f;
	if (!f)
		return -1;
	if (set)
		*f = (float)value * most / 255.0f, *f = n == 3 && *f < 0.02f ? 0.02f : *f;
	return (int)(*f * 255.0f / most + 0.5f);
}

static int
slider_most(int n)
{
	return tab == 1 && n == 0 ? SAMPLE_STEPS - 1 : tab == 1 && n == 1 ? 7 : 255;
}

#define PX (wide - PANEL + 4)	/* the panel's left edge */
#define LOW (tree.y + tree.height + 6)	/* under the list: the two tabs, then what they show */
#define SLIDER_Y(n) (tab == 1 ? LOW + 54 + (n) * 38 : LOW + 30 + (chosen_object() >= 0 ? 124 : 28) + (n) * 22)

static void
draw_number(int x, int y, float v)
{
	char text[16];
	int whole = (int)(v < 0.0f ? -v : v), part = (int)(((v < 0.0f ? -v : v) - (float)whole) * 100.0f + 0.5f);

	if (part == 100)
		whole++, part = 0;
	snprintf(text, sizeof text, "%s%d.%02d", v < 0.0f ? "-" : "", whole, part);
	ui_text(window, x, y, text, -1, BLACK, 0);
}

static void
draw_panel(void)
{
	static const char *const lines[3] = {"Place", "Turn", "Size"}, *const colours[3] = {"Red", "Green", "Blue"};
	int o = chosen_object(), l = chosen_light(), i, k, y = LOW;
	char text[64];

	tree.x = PX, tree.count = sc.object_count + sc.light_count;
	ui_fill(window, wide - PANEL, 0, PANEL, high - STATUS, UI_FACE);
	ui_button(window, PX, 4, 78, 22, " Import...", 0);
	ui_button(window, PX + 82, 4, 78, 22, " Add...", 0);
	ui_button(window, PX + 164, 4, 78, 22, " Delete", 0);
	ui_list_draw(window, &tree, row_label);
	ui_button(window, PX, y, 119, 22, " Selected", tab == 0);
	ui_button(window, PX + 123, y, 119, 22, " Picture", tab == 1);
	y += 30;
	if (tab == 1) {
		snprintf(text, sizeof text, " %d x %d", sizes[size_at][0], sizes[size_at][1]);
		ui_button(window, PX, y, 119, 22, text, 0);
		ui_button(window, PX + 123, y, 119, 22, tracing ? " Stop" : " Render", tracing);
		snprintf(text, sizeof text, "Samples a pixel: %d", sample_steps[samples_at]);
		ui_text(window, PX, SLIDER_Y(0) - 14, text, -1, BLACK, 0);
		snprintf(text, sizeof text, "Turns of a path: %d", bounces);
		ui_text(window, PX, SLIDER_Y(1) - 14, text, -1, BLACK, 0);
		ui_text(window, PX, SLIDER_Y(2) - 14, "The sky's light", -1, BLACK, 0);
		for (i = 0; i < 3; i++)
			ui_slider(window, PX, SLIDER_Y(i), PANEL - 12, slider_value(i, 0, 0), slider_most(i));
		y = SLIDER_Y(2) + 28;
		ui_button(window, PX, y, 119, 22, showing ? " Edit the scene" : " Show the picture", 0);
		ui_button(window, PX + 123, y, 119, 22, " Export...", 0);
	} else if (o < 0 && l < 0) {
		ui_text(window, PX, y + 4, "Nothing is selected: click a thing", -1, UI_SHADOW, 0);
		ui_text(window, PX, y + 20, "in the picture or in the list.", -1, UI_SHADOW, 0);
	} else {
		if (o >= 0)
			for (i = 0; i < 3; i++)
				ui_button(window, PX + i * 82, y, 78, 22, what_names[i], what == i);
		y += o >= 0 ? 28 : 0;
		for (i = 0; i < (o >= 0 ? 3 : 1); i++, y += 22) {
			float step, *v = line_values(i, &step);

			ui_text(window, PX, y + 3, lines[i], -1, BLACK, 0);
			for (k = 0; k < 3; k++) {
				draw_number(PX + 36 + k * 70, y + 3, v[k]);
				ui_button(window, PX + 78 + k * 70, y, 12, 18, "-", 0);
				ui_button(window, PX + 91 + k * 70, y, 12, 18, "+", 0);
			}
		}
		if (o >= 0) {
			for (i = 0; i < 3; i++)
				ui_button(window, PX + i * 82, y + 2, 78, 22, material_names[i], sc.objects[o].material == i);
		}
		for (i = 0; i < (o >= 0 ? 3 : 5); i++) {
			ui_text(window, PX, SLIDER_Y(i) + 2, i < 3 ? colours[i] : i == 3 ? "Radius" : "Power", -1, BLACK, 0);
			ui_slider(window, PX + 44, SLIDER_Y(i), PANEL - 56, slider_value(i, 0, 0), slider_most(i));
		}
	}
	ui_fill(window, 0, high - STATUS, wide, STATUS, UI_FACE);
	ui_text_fit(window, 6, high - STATUS + 3, wide - 12, status, BLACK);
	GrFlush();
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_panel();
}

/* For a test without a screen: what the scene has and what is selected, on the console. */
static void
tell(void)
{
	int o = chosen_object(), l = chosen_light();

	if (!getenv("NXPATH_PRINT"))
		return;
	fprintf(stderr, "nxpath: %d objects, %d lights", sc.object_count, sc.light_count);
	if (o >= 0)
		fprintf(stderr, "; %s is%s at %d %d %d (hundredths), size %d, colour %d %d %d", sc.objects[o].name, material_names[sc.objects[o].material],
			(int)(sc.objects[o].place[0] * 100.0f), (int)(sc.objects[o].place[1] * 100.0f), (int)(sc.objects[o].place[2] * 100.0f),
			(int)(sc.objects[o].size[0] * 100.0f), (int)(sc.objects[o].colour[0] * 255.0f), (int)(sc.objects[o].colour[1] * 255.0f),
			(int)(sc.objects[o].colour[2] * 255.0f));
	if (l >= 0)
		fprintf(stderr, "; %s, power %d", sc.lights[l].name, (int)sc.lights[l].power);
	fprintf(stderr, "\n");
}

/* The scene was changed: the tables for it, and the traced picture is of another scene. */
static void
changed(void)
{
	int i;

	tell();
	for (i = 0; i < sc.object_count; i++)
		sc_object_matrix(&sc.objects[i]);
	if (!tracing)
		showing = 0;
	fill_tables();
	draw_panel();
}

static void
import_dialog(void)
{
	char path[512];
	int m;

	if (!ui_choose_file("Import a mesh", "/root/Models", ".obj .ply", path, sizeof path, others))
		return;
	if ((m = import_mesh(path)) < 0)
		return;
	if ((m = add_object(m, 1.0f, SC_MATT, 0.8f, 0.8f, 0.8f)) < 0) {
		say("No room for it: the scene or the GPU's memory is full.");
		return;
	}
	say("");
	choose(m);
	changed();
}

static void
export_dialog(void)
{
	char path[512];

	if (!traced) {
		say("There is no picture yet: Render first.");
		return;
	}
	if (!ui_save_file("Export the picture", "/root/Pictures", ".ppm", "render.ppm", path, sizeof path, others))
		return;
	ui_files_ending(path, sizeof path, ".ppm");
	write_picture(path);
}

enum { ADD_PLANE = 1, ADD_BOX, ADD_BALL, ADD_LIGHT };

static void
add_menu(void)
{
	static struct ui_menu_item menu[4];
	static const char *const labels[4] = {"Plane", "Box", "Ball", "Light"};
	int i, x, y;

	for (i = 0; i < 4; i++)
		menu[i].label = labels[i], menu[i].id = ADD_PLANE + i, menu[i].icon = -1;
	ui_window_origin(window, &x, &y);
	ui_menu_open(menu, 4, x + PX + 82, y + 26, 0, 0);
}

static void
add_thing(int id)
{
	int made = -1;

	if (id == ADD_PLANE)
		made = add_object(sc_mesh_plane(), 4.0f, SC_MATT, 0.7f, 0.7f, 0.7f);
	else if (id == ADD_BOX)
		made = add_object(sc_mesh_box(), 0.6f, SC_MATT, 0.8f, 0.6f, 0.3f);
	else if (id == ADD_BALL)
		made = add_object(sc_mesh_ball(16), 0.6f, SC_MIRROR, 0.95f, 0.95f, 0.95f);
	else if (id == ADD_LIGHT && (made = add_light(0.0f, 2.0f, 0.0f, 0.25f, 30.0f)) >= 0)
		made += sc.object_count;
	if (made < 0) {
		say("No room for another.");
		return;
	}
	choose(made);
	changed();
}

/* A click in the panel (not on its list); `x`, `y` in the window. */
static void
press_panel(int x, int y)
{
	int o = chosen_object(), l = chosen_light(), i, k, by = LOW + 30;

	if (y < 26) {
		if (x < PX + 80)
			import_dialog();
		else if (x < PX + 162)
			add_menu();
		else
			delete_chosen(), changed();
		return;
	}
	if (ui_inside(x, y, PX, LOW, 242, 22)) {
		tab = x >= PX + 121;
		draw_panel();
		return;
	}
	for (i = 0; i < 5; i++)
		if (slider_value(i, 0, 0) >= 0 && ui_inside(x, y, tab == 1 ? PX : PX + 44, SLIDER_Y(i) - 4, tab == 1 ? PANEL - 12 : PANEL - 56, 26)) {
			held = i + 1;
			ui_moves(window, EVENTS, 1);
			slider_value(i, 1, ui_slider_value(x, tab == 1 ? PX : PX + 44, tab == 1 ? PANEL - 12 : PANEL - 56, slider_most(i)));
			changed();
			return;
		}
	if (tab == 1) {
		if (ui_inside(x, y, PX, by, 119, 22)) {
			size_at = (size_at + 1) % SIZES;
			keep_settings();
			draw_panel();
		} else if (ui_inside(x, y, PX + 123, by, 119, 22)) {
			if (tracing)
				pt.stop = 1;
			else
				begin_render();
		} else if (ui_inside(x, y, PX, SLIDER_Y(2) + 28, 119, 22) && traced) {
			showing = !showing, view_dirty = 1;
			if (showing)
				show_traced();
			draw_panel();
		} else if (ui_inside(x, y, PX + 123, SLIDER_Y(2) + 28, 119, 22))
			export_dialog();
		return;
	}
	if (o < 0 && l < 0)
		return;
	if (o >= 0 && y >= by && y < by + 22) {
		what = (x - PX) / 82 > 2 ? 2 : (x - PX) / 82;
		view_dirty = 1;
		draw_panel();
		return;
	}
	by += o >= 0 ? 28 : 0;
	for (i = 0; i < (o >= 0 ? 3 : 1); i++, by += 22)
		for (k = 0; k < 3 && y >= by && y < by + 18; k++) {
			float step, *v = line_values(i, &step);
			int up = ui_inside(x, y, PX + 91 + k * 70, by, 12, 18);

			if (!up && !ui_inside(x, y, PX + 78 + k * 70, by, 12, 18))
				continue;
			if (i == 2)	/* (a size grows by a tenth of itself, all three ways at once) */
				v[0] *= up ? 1.1f : 1.0f / 1.1f, v[1] *= up ? 1.1f : 1.0f / 1.1f, v[2] *= up ? 1.1f : 1.0f / 1.1f;
			else
				v[k] += up ? step : -step;
			changed();
			return;
		}
	if (o >= 0 && y >= by + 2 && y < by + 24) {
		sc.objects[o].material = (x - PX) / 82 > 2 ? 2 : (x - PX) / 82;
		changed();
	}
}

/* ---- the picture: the view, and what is clicked in it ---- */

static const volatile uint32_t *
control(int offset)
{
	return (const volatile uint32_t *)((const char *)seglPalette() - 0x400 + offset);
}

/* Where the pointer is in the picture, and its buttons, from the machine's own words. */
static void
pointer_now(int *x, int *y, int *buttons)
{
	int ox, oy;

	ui_window_origin(window, &ox, &oy);
	*x = (int)*control(0x20) - ox, *y = (int)*control(0x24) - oy, *buttons = (int)*control(0x28);
}

/* The thing an eye's ray through a point of the picture meets first, by each thing's ball:
 * its row of the list, or -1. */
static int
thing_at(int px, int py)
{
	const struct sc_camera *c = &sc.camera;
	float sx = (2.0f * ((float)px + 0.5f) / (float)view_w - 1.0f) * c->lens * (float)view_w / (float)view_h;
	float sy = (1.0f - 2.0f * ((float)py + 0.5f) / (float)view_h) * c->lens, way[3], len = 0.0f, best = 1e30f;
	int i, k, found = -1;

	for (k = 0; k < 3; k++)
		way[k] = c->ahead[k] + c->right[k] * sx + c->up[k] * sy, len += way[k] * way[k];
	for (len = sqrtf(len), k = 0; k < 3; k++)
		way[k] /= len;
	for (i = 0; i < sc.object_count + sc.light_count; i++) {
		float centre[3], radius = 0.0f, to[3], along, off2;

		if (i < sc.object_count) {
			const struct sc_object *o = &sc.objects[i];
			const struct sc_mesh *m = &sc.meshes[o->mesh];
			float mid[3], half[3];

			for (k = 0; k < 3; k++)
				mid[k] = (m->low[k] + m->high[k]) * 0.5f, half[k] = (m->high[k] - m->low[k]) * 0.5f * o->size[k];
			for (k = 0; k < 3; k++)
				centre[k] = o->matrix[k] * mid[0] + o->matrix[4 + k] * mid[1] + o->matrix[8 + k] * mid[2] + o->matrix[12 + k];
			/* (a flat thing, the ground, is picked by the others' misses only: its ball is everywhere) */
			radius = half[0] > half[2] ? half[0] : half[2];
			radius = half[1] * 8.0f < radius ? 0.0f : sqrtf(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]) * 0.8f;
		} else {
			const struct sc_light *l = &sc.lights[i - sc.object_count];

			for (k = 0; k < 3; k++)
				centre[k] = l->place[k];
			radius = l->radius < 0.08f ? 0.08f : l->radius;
		}
		for (k = 0; k < 3; k++)
			to[k] = centre[k] - c->eye[k];
		along = to[0] * way[0] + to[1] * way[1] + to[2] * way[2];
		off2 = to[0] * to[0] + to[1] * to[1] + to[2] * to[2] - along * along;
		if (along > 0.0f && off2 < radius * radius && along < best)
			best = along, found = i;
	}
	return found;
}

/* The scene by OpenGL, and the handles of what is selected over it. */
static void
draw_view(void)
{
	float view[16], projection[16], m[16];
	int i, o = chosen_object(), l = chosen_light(), px, py, buttons, moved = 0;

	sc_camera_set(&sc.camera);
	view_matrices(view, projection);
	glViewport(0, 0, view_w, view_h);
	glClearColor(0.16f, 0.18f, 0.22f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(projection);
	glMatrixMode(GL_MODELVIEW);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
	for (i = 0; i < sc.object_count; i++) {
		const struct shown_mesh *shown = sc.meshes[sc.objects[i].mesh].shown;

		glLoadMatrixf(view);
		glMultMatrixf(sc.objects[i].matrix);
		seglTagged(shown->vertices, shown->count, tables + 256 * i);
	}
	for (i = 0; i < sc.light_count && ball_mesh >= 0; i++) {
		const struct shown_mesh *shown = sc.meshes[ball_mesh].shown;

		light_matrix(&sc.lights[i], m);
		glLoadMatrixf(view);
		glMultMatrixf(m);
		seglTagged(shown->vertices, shown->count, tables + 256 * (SC_OBJECTS + i));
	}
	if (have_gizmo && (o >= 0 || l >= 0)) {
		pointer_now(&px, &py, &buttons);
		if (o >= 0)
			memcpy(m, sc.objects[o].matrix, sizeof m);
		else
			light_matrix(&sc.lights[l], m), m[0] = m[5] = m[10] = 1.0f;
		moved = gizmo_frame(view, projection, m, o >= 0 ? what : GIZMO_MOVE, view_w, view_h, px, py, drag ? 0 : buttons & 1);
		if (moved == 1 && o >= 0) {
			memcpy(sc.objects[o].matrix, m, sizeof m);
			from_matrix(&sc.objects[o]);
		} else if (moved == 1)
			sc.lights[l].place[0] = m[12], sc.lights[l].place[1] = m[13], sc.lights[l].place[2] = m[14];
	}
	seglSwap();
	on_handle = moved;
	view_dirty = moved == 1;
	if (moved == 1) {
		showing = 0;
		fill_tables();
		draw_panel();
	}
}

static void
set_window(int w, int h)
{
	wide = w < PANEL + 80 ? PANEL + 80 : w, high = h < 200 ? 200 : h;
	view_w = wide - PANEL, view_h = high - STATUS;
	GrResizeWindow(picture, view_w, view_h);
	seglWindowChanged();
	view_dirty = 1;
}

/* A button went down in the picture: on a handle it is the handle's; else the left one
 * picks what is there and turns the view while held, the right one moves the view. */
static void
press_view(int x, int y, int button)
{
	if (showing) {
		showing = 0, view_dirty = 1;
		draw_panel();
		return;
	}
	if (button == 1) {
		int row = thing_at(x, y);

		if (row != tree.selected)
			choose(row);
	}
	drag = button, drag_x = x, drag_y = y;
}

/* The view follows the pointer while a button is held in the picture. */
static void
drag_view(void)
{
	struct sc_camera *c = &sc.camera;
	int x, y, buttons, k;

	pointer_now(&x, &y, &buttons);
	if (!(buttons & (drag == 1 ? 1 : 2))) {
		drag = 0;
		return;
	}
	if (x == drag_x && y == drag_y)
		return;
	if (drag == 1) {
		c->turn += (float)(x - drag_x) * 0.4f;
		c->tilt += (float)(y - drag_y) * 0.4f;
		c->tilt = c->tilt > 89.0f ? 89.0f : c->tilt < -89.0f ? -89.0f : c->tilt;
	} else
		for (k = 0; k < 3; k++)
			c->target[k] += (c->up[k] * (float)(y - drag_y) - c->right[k] * (float)(x - drag_x)) * c->far * 0.002f;
	drag_x = x, drag_y = y;
	view_dirty = 1;
}

static void
events(void)
{
	GR_EVENT e;
	int id, x, y;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return;
		ui_origin_note(&e);
		switch (ui_menu_event(&e, &id)) {
		case UI_MENU_CHOSEN:
			add_thing(id);
			/* fall through */
		case UI_MENU_CLOSED:
		case UI_MENU_TOOK:
			continue;
		}
		switch (e.type) {
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (e.general.wid != window)
				break;
			pt.stop = 1;
			exit(0);
		case GR_EVENT_TYPE_EXPOSURE:
			if (e.exposure.wid == window)
				draw_panel();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (e.update.utype == GR_UPDATE_SIZE && e.update.wid == window) {
				set_window(e.update.width, e.update.height);
				draw_panel();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (held && tab == 1)
				keep_settings();
			if (held)
				held = 0, ui_moves(window, EVENTS, 0);
			view_dirty = 1;	/* (a handle let go: its library is told by the next frame) */
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (held && (e.mouse.buttons & GR_BUTTON_L)) {
				int n = held - 1, at = tab == 1 ? PX : PX + 44, across = tab == 1 ? PANEL - 12 : PANEL - 56;

				slider_value(n, 1, ui_slider_value(e.mouse.x, at, across, slider_most(n)));
				changed();
			} else if (ui_list_event(&tree, &e))
				draw_panel();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (e.button.wid != window)
				break;
			x = e.button.x, y = e.button.y;
			if (x < view_w && y < view_h) {
				if (ui_wheel(&e)) {
					sc.camera.far *= ui_wheel(&e) > 0 ? 1.1f : 0.9f;
					sc.camera.far = sc.camera.far < 0.5f ? 0.5f : sc.camera.far > 60.0f ? 60.0f : sc.camera.far;
					showing = 0, view_dirty = 1;
				} else if (e.button.changebuttons & (GR_BUTTON_L | GR_BUTTON_R)) {
					/* (over a handle the press is the handle's: draw_view() sees the button) */
					if (!(have_gizmo && !showing && on_handle && (e.button.changebuttons & GR_BUTTON_L)))
						press_view(x, y, e.button.changebuttons & GR_BUTTON_L ? 1 : 2);
					view_dirty = 1;
				}
			} else if (y >= high - STATUS) {
				break;
			} else if (ui_inside(x, y, tree.x, tree.y, tree.width, tree.height)) {
				if (ui_list_event(&tree, &e) && !ui_wheel(&e))
					choose(tree.selected);
				else
					draw_panel();
			} else if (!ui_wheel(&e) && (e.button.changebuttons & GR_BUTTON_L))
				press_panel(x, y);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			switch (e.keystroke.ch) {
			case MWKEY_DELETE: delete_chosen(), changed(); break;
			case 'g': what = GIZMO_MOVE, view_dirty = 1, draw_panel(); break;
			case 'r': what = GIZMO_TURN, view_dirty = 1, draw_panel(); break;
			case 's': what = GIZMO_SIZE, view_dirty = 1, draw_panel(); break;
			case MWKEY_F5: begin_render(); break;
			case MWKEY_ESCAPE:
				if (tracing)
					pt.stop = 1;
				else
					showing = 0, view_dirty = 1, draw_panel();
				break;
			}
			break;
		}
	}
}

/* What the program starts with: a ground, the teapot (or a box, without its file), a mirror
 * ball, a light and a pale sky; and beside them the mesh it was started with. */
static void
first_scene(const char *more)
{
	int teapot = access("/root/Models/teapot.obj", R_OK) == 0 ? import_mesh("/root/Models/teapot.obj") : -1, made;

	add_object(sc_mesh_plane(), 6.0f, SC_MATT, 0.62f, 0.6f, 0.56f);
	snprintf(sc.objects[0].name, SC_NAME, "Ground");
	made = add_object(teapot >= 0 ? teapot : sc_mesh_box(), 1.4f, SC_MATT, 0.85f, 0.45f, 0.25f);
	ball_mesh = sc_mesh_ball(16);
	show_mesh(ball_mesh);
	made = add_object(ball_mesh, 0.6f, SC_MIRROR, 0.95f, 0.95f, 0.95f);
	if (made >= 0) {
		sc.objects[made].place[0] = 1.1f, sc.objects[made].place[2] = -0.5f;
		snprintf(sc.objects[made].name, SC_NAME, "Ball");
	}
	add_light(-1.2f, 2.4f, -1.4f, 0.3f, 40.0f);
	sc.sky[0] = 0.6f, sc.sky[1] = 0.75f, sc.sky[2] = 1.0f, sc.sky_power = (float)reg_int("pathtrace.sky", 35) * 0.01f;
	sc.camera.target[1] = 0.45f, sc.camera.turn = 35.0f, sc.camera.tilt = 22.0f, sc.camera.far = 4.2f, sc.camera.lens = 0.6f;
	say("");
	if (more && (made = import_mesh(more)) >= 0 && (made = add_object(made, 1.0f, SC_MATT, 0.8f, 0.8f, 0.8f)) >= 0)
		sc.objects[made].place[0] = -1.2f;
}

int
main(int argc, char **argv)
{
	GR_SCREEN_INFO info;
	const char *text;
	int w, h, i;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxpath: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	GrGetScreenInfo(&info);
	wide = wide > info.cols - 20 ? info.cols - 20 : wide, high = high > info.rows - 70 ? info.rows - 70 : high;
	ui_init();
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Path tracer", GR_ROOT_WINDOW_ID, -1, -1, wide, high, UI_FACE);
	view_w = wide - PANEL, view_h = high - STATUS;
	picture = GrNewWindow(window, 0, 0, view_w, view_h, 0, BLACK, BLACK);
	GrMapWindow(picture);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	if (seglInit(picture) < 0) {
		fprintf(stderr, "nxpath: this machine has no GPU\n");
		return 1;
	}
	have_gizmo = gizmo_init(picture);
	GrSelectEvents(picture, 0);	/* (the handles' library asked for the picture's events: they are the window's) */
	GrSelectEvents(window, EVENTS);
	pt.pixels = seglMemory(MOST_W * MOST_H * 4);
	tables = seglMemory(256 * 4 * (SC_OBJECTS + SC_LIGHTS));
	pt.sums = mcw_alloc(MOST_W * MOST_H * 3 * sizeof(float));
	pt.slots = mcw_alloc(MC_MAX_CORES * sizeof(struct pt_slot));
	if (!pt.pixels || !tables || !pt.sums || !pt.slots) {
		fprintf(stderr, "nxpath: no memory for the picture\n");
		return 1;
	}
	memset(pt.pixels, 0, MOST_W * MOST_H * 4);	/* (this core's first store there: a worker's would stop it) */
	glGenTextures(1, &texture);
	atexit(mcw_close);
	size_at = reg_int("pathtrace.size", size_at) % SIZES;
	samples_at = reg_int("pathtrace.samples", samples_at) % SAMPLE_STEPS;
	bounces = reg_int("pathtrace.turns", bounces);
	bounces = bounces < 1 || bounces > 8 ? 5 : bounces;
	if ((text = getenv("NXPATH_SIZE")) != NULL && sscanf(text, "%dx%d", &w, &h) == 2)
		for (i = 0; i < SIZES; i++)
			if (sizes[i][0] == w && sizes[i][1] == h)
				size_at = i;
	if ((text = getenv("NXPATH_SAMPLES")) != NULL)
		for (i = 0; i < SAMPLE_STEPS; i++)
			if (sample_steps[i] == atoi(text))
				samples_at = i;
	leave_when_done = getenv("NXPATH_EXIT") != NULL;
	take_workers((text = getenv("NXPATH_CORES")) != NULL ? atoi(text) : MC_MAX_CORES);
	first_scene(argc > 1 ? argv[1] : NULL);
	choose(argc > 1 && sc.object_count > 3 ? sc.object_count - 1 : 1);
	changed();
	if (getenv("NXPATH_AUTO"))
		begin_render();
	for (;;) {
		static int last_x, last_y;
		int x, y, buttons;

		events();
		if (tracing) {
			trace_on();
			if (tracing && now_ms() - shown_at >= 400)
				show_traced();
			if (workers)
				mcw_rest();
			continue;
		}
		if (drag)
			drag_view();
		/* (the handles light up under the pointer, and follow it: a frame when it has moved in the picture) */
		pointer_now(&x, &y, &buttons);
		if (!showing && have_gizmo && tree.selected >= 0 && (x != last_x || y != last_y) && x >= 0 && y >= 0 && x < view_w && y < view_h)
			view_dirty = view_dirty ? view_dirty : 1;
		last_x = x, last_y = y;
		if (view_dirty && !showing)
			draw_view();
		else
			mcw_rest();
	}
}
