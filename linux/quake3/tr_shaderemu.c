/*
 * Quake III Arena's renderer on the machine's GPU (docs/quake3.md), in place of the game's own:
 * nothing of a frame is worked out here that the GPU can read for itself. The level's
 * triangles are in GPU memory from when it loads, sorted by what they are drawn with, and
 * are commands that stay in every frame's list; a texture, a light map and a model's pose
 * are read where bake.py's file lies in the ROM; a model is lit by a table of colours, one
 * for each way a vertex can face. A frame says where the eye is and what moved.
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "client/client.h"
#include "renderercommon/tr_public.h"
#include <GLES/segl.h>
#include "nano-X.h"
#include "gpu_shaderemu.h"
#include "se_data.h"
#include "se_q3.h"

#define INLINE_MODEL	0x4000		/* a handle from here on is one of the level's own models */
#define MAX_SCENE	1024		/* entities of a scene */
#define MAX_POLYS	1024
#define MAX_POLY_VERTS	6000
#define Z_NEAR		4.0f
#define Z_FAR		16384.0f

static refimport_t	ri;
static glconfig_t	config;
static GR_WINDOW_ID	window;
static int		opened, width = 640, height = 480;

/* ---- the file ---- */

static const unsigned char *dat;
static unsigned		dat_rom;	/* its first byte as the GPU addresses it */
static const se_texture_t *textures;
static const se_shader_t *shaders, **shader_of;
static const se_stage_t	*stages;
static const se_model_t	*models;
static const se_surface_t *surfaces;
static const se_skin_t	*skins;
static const se_skin_entry_t *skin_entries;
static const se_world_t	*world;
static const se_run_t	*runs;
static const se_bmodel_t *bmodels;
static const float	*normals;
static int		shader_count, model_count, skin_count;

static const void *section (const char *tag, unsigned each, int *count)
{
	const void	*at = SE_DataSection (tag, each, count);

	if (!at)
		ri.Error (ERR_FATAL, "%s has no %s", SE_DATA_FILE, tag);
	return at;
}

static void data_open (void)
{
	int	i, count;

	dat = SE_Data (&dat_rom);
	if (!dat)
		ri.Error (ERR_FATAL, "no %s of bake.py's", SE_DATA_FILE);
	if (!dat_rom)
		ri.Error (ERR_FATAL, "%s is not a file of the ROM: the GPU reads it there", SE_DATA_FILE);
	textures = section ("TEXS", sizeof (se_texture_t), NULL);
	shaders = section ("SHAD", sizeof (se_shader_t), &shader_count);
	stages = section ("STAG", sizeof (se_stage_t), NULL);
	models = section ("MODL", sizeof (se_model_t), &model_count);
	surfaces = section ("SURF", sizeof (se_surface_t), NULL);
	skins = section ("SKIN", sizeof (se_skin_t), &skin_count);
	skin_entries = section ("SKNE", sizeof (se_skin_entry_t), NULL);
	world = section ("WHDR", sizeof (se_world_t), NULL);
	runs = section ("WRUN", sizeof (se_run_t), NULL);
	bmodels = section ("WBMD", sizeof (se_bmodel_t), NULL);
	normals = section ("NRML", 12, &count);
	shader_of = malloc (shader_count * sizeof *shader_of);
	for (i = 0; i < shader_count; i++)
		shader_of[shaders[i].number] = &shaders[i];
}

/* A game path as the file's tables have it: lower case, forward slashes, and with `strip` no extension. */
static void clean_name (char *out, const char *name, int strip)
{
	int	i, dot = -1;

	for (i = 0; i < 63 && name[i]; i++) {
		char c = name[i];

		out[i] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? c + 32 : c;
		if (c == '.')
			dot = i;
		else if (c == '/' || c == '\\')
			dot = -1;
	}
	out[strip && dot >= 0 ? dot : i] = 0;
}

/* The entry of that name in a table sorted by name (each begins with its name), or -1. */
static int find (const void *table, int count, int each, const char *name)
{
	int	low = 0, high = count - 1;

	while (low <= high) {
		int	mid = (low + high) >> 1;
		int	c = strcmp (name, (const char *)table + mid * each);

		if (!c)
			return mid;
		if (c < 0)
			high = mid - 1;
		else
			low = mid + 1;
	}
	return -1;
}

/* ---- the window and the input ---- */

unsigned SE_WindowClock (void)
{
	const volatile unsigned	*clock = opened ? gpuControl (REG_CLOCK) : NULL;

	return clock ? *clock : 0;
}

void SE_Settle (void)
{
	if (opened)
		seglSettle ();
}

int SE_ClientPlaying (void)
{
	return clc.state == CA_ACTIVE && !clc.demoplaying;
}

static int		mouse_x, mouse_y, mouse_known, mouse_inside = 1;

static int game_key (int ch)
{
	static const struct { int ch, key; } other[] = {
		{MWKEY_LEFT, K_LEFTARROW}, {MWKEY_RIGHT, K_RIGHTARROW}, {MWKEY_UP, K_UPARROW}, {MWKEY_DOWN, K_DOWNARROW},
		{MWKEY_ENTER, K_ENTER}, {'\n', K_ENTER}, {'\r', K_ENTER}, {MWKEY_KP_ENTER, K_KP_ENTER}, {MWKEY_ESCAPE, K_ESCAPE},
		{MWKEY_BACKSPACE, K_BACKSPACE}, {MWKEY_TAB, K_TAB}, {MWKEY_INSERT, K_INS}, {MWKEY_DELETE, K_DEL},
		{MWKEY_HOME, K_HOME}, {MWKEY_END, K_END}, {MWKEY_PAGEUP, K_PGUP}, {MWKEY_PAGEDOWN, K_PGDN},
		{MWKEY_LSHIFT, K_SHIFT}, {MWKEY_RSHIFT, K_SHIFT}, {MWKEY_LCTRL, K_CTRL}, {MWKEY_RCTRL, K_CTRL},
		{MWKEY_LALT, K_ALT}, {MWKEY_RALT, K_ALT}, {MWKEY_PAUSE, K_PAUSE}, {'`', K_CONSOLE}, {'~', K_CONSOLE},
	};
	unsigned	i;

	if (ch >= 'A' && ch <= 'Z')
		return ch - 'A' + 'a';
	if (ch >= MWKEY_F1 && ch <= MWKEY_F12)
		return K_F1 + ch - MWKEY_F1;
	for (i = 0; i < ARRAY_LEN (other); i++)
		if (other[i].ch == ch)
			return other[i].key;
	return ch >= 32 && ch < 127 ? ch : 0;
}

/*
 * The window's keys and buttons as the game's events, and the pointer's travel over the
 * window as its mouse. Asking the window system costs system calls: asked when the machine's
 * own input counters have moved, and a few times a second (linux/quake/vid_shaderemu.c).
 */
void SE_Input (void)
{
	static unsigned	seen[4], asked_ms;
	static int	ask = 8;
	const volatile unsigned *input;
	GR_EVENT	e;
	int		i, key, down;

	if (!opened)
		return;
	input = gpuControl (REG_INPUT);
	if (mouse_inside) {
		int x = (int)input[0], y = (int)input[1];

		if (mouse_known && (x != mouse_x || y != mouse_y))
			Com_QueueEvent (0, SE_MOUSE, x - mouse_x, y - mouse_y, 0, NULL);
		mouse_x = x, mouse_y = y, mouse_known = 1;
	}
	for (i = 2; i < 4; i++)		/* the buttons, and the count of keys */
		if (seen[i] != input[i]) {
			seen[i] = input[i];
			ask = 8;
		}
	if (!ask && SE_WindowClock () - asked_ms >= 250)
		ask = 1;
	while (ask) {
		GrCheckNextEvent (&e);
		switch (e.type) {
		case GR_EVENT_TYPE_NONE:
			ask--;
			asked_ms = SE_WindowClock ();
			return;
		case GR_EVENT_TYPE_MOUSE_ENTER:
			mouse_inside = 1;
			mouse_known = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			mouse_inside = 0;
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (e.update.utype == GR_UPDATE_SIZE)
				seglWindowChanged ();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			down = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
			if (e.button.changebuttons & GR_BUTTON_L)
				Com_QueueEvent (0, SE_KEY, K_MOUSE1, down, 0, NULL);
			if (e.button.changebuttons & GR_BUTTON_R)
				Com_QueueEvent (0, SE_KEY, K_MOUSE2, down, 0, NULL);
			if (e.button.changebuttons & GR_BUTTON_M)
				Com_QueueEvent (0, SE_KEY, K_MOUSE3, down, 0, NULL);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			down = e.type == GR_EVENT_TYPE_KEY_DOWN;
			key = game_key (e.keystroke.ch);
			if (key)
				Com_QueueEvent (0, SE_KEY, key, down, 0, NULL);
			if (down && key == K_BACKSPACE)
				Com_QueueEvent (0, SE_CHAR, 'h' - 'a' + 1, 0, 0, NULL);
			else if (down && key != K_CONSOLE && e.keystroke.ch >= 32 && e.keystroke.ch < 127)
				Com_QueueEvent (0, SE_CHAR, e.keystroke.ch, 0, 0, NULL);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			Cbuf_ExecuteText (EXEC_APPEND, "quit\n");
			break;
		}
	}
}

void IN_Init (void *windowData) {}
void IN_Frame (void) {}
void IN_Shutdown (void) {}
void IN_Restart (void) {}

static void window_open (void)
{
	GR_SCREEN_INFO	info;
	const char	*s;
	int		bare;

	if (GrOpen () < 0)
		ri.Error (ERR_FATAL, "no Nano-X server (start one: nano-X -p &)");
	GrGetScreenInfo (&info);
	if ((s = getenv ("QUAKE3_SIZE")) != NULL)
		sscanf (s, "%dx%d", &width, &height);
	if (width > info.cols)
		width = info.cols;
	if (height > info.rows)
		height = info.rows;
	/* a window that leaves no room for a frame and caption has none, and sits in the corner */
	bare = width + 8 > info.cols || height + 26 > info.rows;
	window = GrNewWindowEx (bare ? GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOAUTOMOVE : GR_WM_PROPS_APPWINDOW,
		"Quake III Arena", GR_ROOT_WINDOW_ID, bare ? 0 : -1, bare ? 0 : -1, width, height, 0);
	GrSelectEvents (window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_CLOSE_REQ |
		GR_EVENT_MASK_UPDATE);
	GrMapWindow (window);
	GrSetFocus (window);
	if (seglInit (window) < 0)
		ri.Error (ERR_FATAL, "this machine has no GPU");
	seglWorldUp (3);
	opened = 1;
}

/* ---- numbers ---- */

static inline unsigned float_bits (float f)
{
	union { float f; unsigned u; } c = {f};

	return c.u;
}

static inline float floor_of (float f)
{
	float	whole = (float)(int)f;

	return whole > f ? whole - 1 : whole;
}

static inline unsigned level (float f)
{
	return f <= 0 ? 0 : f >= 255 ? 255 : (unsigned)f;
}

/* A colour as the GPU has it: the top byte is how transparent. */
static inline unsigned colour_word (unsigned r, unsigned g, unsigned b, unsigned a)
{
	return (255 - a) << 24 | r << 16 | g << 8 | b;
}

static float	sines[256];	/* of 256ths of a turn */

static float wave (const se_wave_t *w, float time)
{
	float	x = w->phase + time * w->frequency, v;

	x -= floor_of (x);
	switch (w->function) {
	case SE_TRIANGLE:	v = x < 0.25f ? 4 * x : x < 0.75f ? 2 - 4 * x : 4 * x - 4; break;
	case SE_SQUARE:		v = x < 0.5f ? 1 : -1; break;
	case SE_SAWTOOTH:	v = x; break;
	case SE_INVERSE_SAWTOOTH: v = 1 - x; break;
	case SE_NOISE:		v = 0; break;
	default:		v = sines[(int)(x * 256) & 255]; break;
	}
	return w->base + w->amplitude * v;
}

/* ---- a stage: what the GPU is told of it ---- */

static float		scene_time;	/* the scene's clock, in seconds */

/* Its texture now (one of several, by the clock), how its texels are read, and its pass. */
static void stage_state (gpu_state *s, const se_stage_t *st, int flat)
{
	const se_texture_t	*t;
	int			which = 0;

	memset (s, 0, sizeof *s);
	s->colour = 0x00ffffff;
	s->fragment = GPU_PASS (flat ? (st->pass == SE_ADD ? 6 : 5) : st->pass);
	if (st->texture == 0xffff)
		return;
	if (st->frames > 1)
		which = (int)(scene_time * st->fps) % st->frames;
	t = &textures[st->texture + which];
	s->texture = dat_rom + t->at;
	s->width = t->width, s->height = t->height;
	s->fragment |= (t->format == SE_WORDS ? GPU_WORDS : GPU_BYTES) | GPU_SMOOTH;
	if (st->flags & SE_CLAMP)
		s->fragment |= GPU_CLAMPED;
	if (st->flags & SE_KEYED)
		s->fragment |= GPU_KEYED, s->key = t->key;
}

/* Its colour: `entity` is the colour of what is drawn, `vertex` of its first corner (either may be NULL). */
static unsigned stage_colour (const se_stage_t *st, const byte *entity, const byte *vertex)
{
	unsigned	r = 255, g = 255, b = 255, a = 255;

	switch (st->rgb) {
	case SE_RGB_WAVE:
		r = g = b = level (wave (&st->rgb_wave, scene_time) * 255.0f);
		break;
	case SE_RGB_CONST:
		r = st->constant[0], g = st->constant[1], b = st->constant[2];
		break;
	case SE_RGB_ENTITY:
		if (entity)
			r = entity[0], g = entity[1], b = entity[2];
		break;
	case SE_RGB_ONE_MINUS_ENTITY:
		if (entity)
			r = 255 - entity[0], g = 255 - entity[1], b = 255 - entity[2];
		break;
	case SE_RGB_VERTEX:
	case SE_RGB_DIFFUSE:
		if (vertex)
			r = vertex[0], g = vertex[1], b = vertex[2];
		break;
	}
	switch (st->alpha) {
	case SE_A_WAVE:		a = level (wave (&st->alpha_wave, scene_time) * 255.0f); break;
	case SE_A_CONST:	a = st->constant[3]; break;
	case SE_A_ENTITY:	if (entity) a = entity[3]; break;
	case SE_A_ONE_MINUS_ENTITY: if (entity) a = 255 - entity[3]; break;
	case SE_A_VERTEX:	if (vertex) a = vertex[3]; break;
	default:		if (vertex && (st->flags & SE_IMPLICIT)) a = vertex[3]; break;
	}
	return colour_word (r, g, b, a);
}

/*
 * How its texture slides, turns and grows now (tcMod), for the GPU to move it by: two rows of
 * three, in the frame's memory, and their address. 0 when the frame has no room. `whole` is
 * what the vertices' own coordinates had taken off.
 */
static unsigned stage_moved (const se_stage_t *st, const float *whole)
{
	float	m[6] = {1, 0, 0, 0, 1, 0}, a[6], n[6], *out;
	int	i;

	for (i = 0; i < st->tcmods; i++) {
		const se_tcmod_t *t = &st->tcmod[i];
		float	p, sn, cs, turn;

		a[0] = 1, a[1] = 0, a[2] = 0, a[3] = 0, a[4] = 1, a[5] = 0;
		switch (t->kind) {
		case SE_SCROLL:
			a[2] = t->p[0] * scene_time, a[5] = t->p[1] * scene_time;
			a[2] -= floor_of (a[2]), a[5] -= floor_of (a[5]);
			break;
		case SE_SCALE:
			a[0] = t->p[0], a[4] = t->p[1];
			break;
		case SE_ROTATE:
			turn = -t->p[0] * scene_time * (1.0f / 360);
			turn -= floor_of (turn);
			sn = sines[(int)(turn * 256) & 255], cs = sines[((int)(turn * 256) + 64) & 255];
			a[0] = cs, a[1] = -sn, a[2] = 0.5f - 0.5f * cs + 0.5f * sn;
			a[3] = sn, a[4] = cs, a[5] = 0.5f - 0.5f * sn - 0.5f * cs;
			break;
		case SE_STRETCH: {
			se_wave_t w = {t->function, t->p[0], t->p[1], t->p[2], t->p[3]};

			p = wave (&w, scene_time);
			p = p ? 1.0f / p : 1.0f;
			a[0] = a[4] = p, a[2] = a[5] = 0.5f - 0.5f * p;
			break;
		}
		default:
			continue;
		}
		/* this one after those before it */
		n[0] = a[0] * m[0] + a[1] * m[3], n[1] = a[0] * m[1] + a[1] * m[4], n[2] = a[0] * m[2] + a[1] * m[5] + a[2];
		n[3] = a[3] * m[0] + a[4] * m[3], n[4] = a[3] * m[1] + a[4] * m[4], n[5] = a[3] * m[2] + a[4] * m[5] + a[5];
		memcpy (m, n, sizeof m);
	}
	if (whole) {
		m[2] += m[0] * whole[0] + m[1] * whole[1];
		m[5] += m[3] * whole[0] + m[4] * whole[1];
	}
	if (!(st->flags & SE_CLAMP))
		m[2] -= floor_of (m[2]), m[5] -= floor_of (m[5]);
	out = gpuFrameMemory (32);
	if (!out)
		return 0;
	out[0] = m[0], out[1] = m[1], out[2] = m[2], out[3] = 0;
	out[4] = m[3], out[5] = m[4], out[6] = m[5], out[7] = 0;
	return gpuAddress (out);
}

/* ---- the view ---- */

/* (of the scene's description: the whole of it is 8 KB, which a copy a scene is not worth) */
static struct { vec3_t vieworg, viewaxis[3]; int rdflags; } view;
static float		view_matrix[16], view_planes[4][4];
static int		matrix_is;	/* what the GPU's matrices are now: 0 unknown, 1 the scene's own, 2 the flat screen's */
static unsigned		frame_number = 2;
unsigned		se_counts[6];	/* since the last q3stat: line: models, their draws, how many were lit, sprites and beams, polygons, flat pictures */

static void view_set (const refdef_t *fd)
{
	float		p[16], xmax, ymax, ang, sn, cs;
	const float	*o = fd->vieworg, *f = fd->viewaxis[0], *l = fd->viewaxis[1], *u = fd->viewaxis[2];
	float		*m = view_matrix;
	int		i;

	VectorCopy (fd->vieworg, view.vieworg);
	AxisCopy (fd->viewaxis, view.viewaxis);
	view.rdflags = fd->rdflags;
	scene_time = fd->time * 0.001f;
	xmax = Z_NEAR * tan (fd->fov_x * (M_PI / 360.0f));
	ymax = Z_NEAR * tan (fd->fov_y * (M_PI / 360.0f));
	memset (p, 0, sizeof p);
	p[0] = Z_NEAR / xmax, p[5] = Z_NEAR / ymax;
	p[10] = -(Z_FAR + Z_NEAR) / (Z_FAR - Z_NEAR), p[11] = -1, p[14] = -2 * Z_FAR * Z_NEAR / (Z_FAR - Z_NEAR);
	/* the game's forward, left and up are the GPU's -z, -x and y */
	m[0] = -l[0], m[4] = -l[1], m[8] = -l[2], m[12] = DotProduct (l, o);
	m[1] = u[0], m[5] = u[1], m[9] = u[2], m[13] = -DotProduct (u, o);
	m[2] = -f[0], m[6] = -f[1], m[10] = -f[2], m[14] = DotProduct (f, o);
	m[3] = m[7] = m[11] = 0, m[15] = 1;
	glViewport (fd->x, config.vidHeight - (fd->y + fd->height), fd->width, fd->height);
	gpuProjection (p);
	gpuModelview (m);
	matrix_is = 1;
	/* the view's four sides, facing in: what is wholly behind one is not drawn */
	for (i = 0; i < 4; i++) {
		const float	*side = i < 2 ? l : u;
		float		half = (i < 2 ? fd->fov_x : fd->fov_y) * (M_PI / 360.0f), sign = i & 1 ? -1 : 1;
		float		*n = view_planes[i];

		ang = half;
		sn = sin (ang), cs = cos (ang);
		n[0] = f[0] * sn + sign * side[0] * cs;
		n[1] = f[1] * sn + sign * side[1] * cs;
		n[2] = f[2] * sn + sign * side[2] * cs;
		n[3] = DotProduct (o, n);
	}
}

static int culled (const float *centre, float radius)
{
	int	i;

	for (i = 0; i < 4; i++)
		if (DotProduct (centre, view_planes[i]) - view_planes[i][3] < -radius)
			return 1;
	return 0;
}

/* The GPU's matrix for a thing at `origin` turned to `axis`, its own units `scale` and `move`d (or NULL). */
static void entity_matrix (const float *origin, const vec3_t *axis, const float *scale, const float *move)
{
	float		rows[12], col[4][3];
	const float	*v = view_matrix;
	int		i, j;

	for (j = 0; j < 3; j++)
		for (i = 0; i < 3; i++)
			col[j][i] = scale ? axis[j][i] * scale[j] : axis[j][i];
	for (i = 0; i < 3; i++)
		col[3][i] = origin[i] + (move ? axis[0][i] * move[0] + axis[1][i] * move[1] + axis[2][i] * move[2] : 0);
	for (j = 0; j < 4; j++)
		for (i = 0; i < 3; i++)
			rows[i * 4 + j] = v[i] * col[j][0] + v[4 + i] * col[j][1] + v[8 + i] * col[j][2] + (j == 3 ? v[12 + i] : 0);
	gpuModelviewRows (rows);
	matrix_is = 0;
}

static void view_matrix_again (void)
{
	if (matrix_is != 1) {
		gpuModelview (view_matrix);
		matrix_is = 1;
	}
}

/* ---- the level ---- */

/* the light on things, as it was last asked for (light_at) */
#define LIGHT_KEPT	64
static struct { int at[3], valid; float light[9]; } light_kept[LIGHT_KEPT];	/* by where, in steps of 16 units */

typedef struct {
	gpu_state	state;
	unsigned	vertices, first, count;
	int		changes;	/* a frame says something of it: its colour, its picture, how it moves */
} run_t;

static run_t		*world_runs;
static int		world_count, world_kept, world_hidden[2], world_loaded;
static unsigned		world_mark;
static int		world_seen;	/* this frame drew a scene with the level in it */

static void *gpu_copy (unsigned at, unsigned bytes)
{
	void	*to = seglMemory (bytes);

	if (!to)
		ri.Error (ERR_DROP, "no GPU memory for the level (%u bytes more)", bytes);
	memcpy (to, dat + at, bytes);
	return to;
}

static void world_free (void)
{
	if (!world_loaded)
		return;
	gpuKeep (0, 0);
	gpuMemoryRelease (world_mark);
	free (world_runs);
	world_runs = NULL;
	world_loaded = world_count = world_kept = 0;
}

static void RE_LoadWorld (const char *name)
{
	const unsigned	*vertices;
	int		i, set, mesh = 0;

	world_free ();
	memset (light_kept, 0, sizeof light_kept);
	world_mark = gpuMemoryMark ();
	world_count = (int)world->runs;
	world_runs = calloc (world_count ? world_count : 1, sizeof (run_t));
	vertices = gpu_copy (world->vertices_at, world->vertices * 16);
	for (i = 0; i < world_count; i++) {
		const se_run_t		*r = &runs[i];
		const se_stage_t	*st = &stages[r->stage];
		run_t			*w = &world_runs[i];

		stage_state (&w->state, st, 0);
		w->state.fragment = (w->state.fragment & 0xffff) | GPU_PASS (r->pass);
		w->state.colour = stage_colour (st, NULL, NULL);
		w->vertices = gpuAddress (vertices + 4 * r->first);
		w->first = r->first, w->count = r->count;
		if (r->flags & SE_RUN_LIT) {
			const se_texture_t *page = &textures[r->light];

			w->state.fragment |= GPU_LIT;
			w->state.more[0] = gpuAddress (gpu_copy (r->light_at, r->count * 4));
			w->state.more[1] = dat_rom + page->at;
			w->state.more[2] = page->width | (unsigned)page->height << 16;
		}
		if (r->flags & SE_RUN_TAGGED) {
			w->state.vertex |= GPU_TAGGED | GPU_TABLE;
			w->state.colour = gpuAddress (gpu_copy (r->table_at, 1024));
		}
		if (r->flags & SE_RUN_SKY)
			w->state.fragment |= GPU_LAID;
		if (r->flags & SE_RUN_MOVING)
			w->state.vertex |= GPU_MOVED;
		w->changes = (r->flags & (SE_RUN_SKY | SE_RUN_MOVING)) || st->frames > 1 || ((st->rgb == SE_RGB_WAVE || st->alpha == SE_A_WAVE) && !(r->flags & SE_RUN_TAGGED));
		if (r->model == 0)
			world_kept = i + 1, mesh += r->count;
	}
	if (gpuKeep (world_kept, mesh) < 0)
		ri.Error (ERR_DROP, "the level is too large to keep in GPU memory (%i vertices in %i draws)", mesh, world_kept);
	for (set = 0; set < 2; set++)
		for (i = 0; i < world_kept; i++)
			gpuKept (set, i, &world_runs[i].state, world_runs[i].vertices, world_runs[i].first, world_runs[i].count);
	world_hidden[0] = world_hidden[1] = 0;
	world_loaded = 1;
	ri.Printf (PRINT_ALL, "the level is kept in GPU memory: %i vertices in %i draws, %u KB\n", mesh, world_kept,
		(gpuMemoryMark () - world_mark) >> 10);
}

/* What a frame says of a run that changes, in the words of a command of it. */
static void run_now (const se_run_t *r, const run_t *w, unsigned *c)
{
	const se_stage_t	*st = &stages[r->stage];
	gpu_state		s;

	if (st->frames > 1) {
		stage_state (&s, st, 0);
		c[7] = s.texture, c[8] = s.width, c[9] = s.height;
	}
	if (!(r->flags & SE_RUN_TAGGED) && (st->rgb == SE_RGB_WAVE || st->alpha == SE_A_WAVE))
		c[11] = stage_colour (st, NULL, NULL);
	if (r->flags & SE_RUN_MOVING)
		c[15] = stage_moved (st, r->whole);
	if (r->flags & SE_RUN_SKY) {
		/* the sky lies on the picture, four repeats to 1,024 pixels, and slides as the eye turns */
		const float	*f = view.viewaxis[0];
		float		yaw = atan2 (f[1], f[0]) * (1.0f / (2 * M_PI)), pitch = f[2];
		float		u = -yaw * 4 * config.vidWidth * (4.0f / 1024), v = pitch * config.vidHeight * (4.0f / 1024);

		c[12] = float_bits (u - floor_of (u)), c[13] = float_bits (v - floor_of (v));
		c[14] = c[15] = float_bits (4.0f);
	}
}

/* The level in this frame: the kept draws get the scene's matrices, and what changes is said. */
static void world_frame (void)
{
	int	i, set = seglSet ();

	if (world_seen)
		return;		/* (a frame's second scene of the level: the kept draws have the first's matrices) */
	seglKeptMatrices ();
	for (i = 0; i < world_kept; i++) {
		const run_t	*w = &world_runs[i];
		unsigned	*c;

		if (!w->changes && !world_hidden[set])
			continue;
		c = gpuKeptCommand (i);
		c[2] = w->count;
		if (w->changes)
			run_now (&runs[i], w, c);
	}
	world_hidden[set] = 0;
	world_seen = 1;
}

/* A frame with no scene of the level (a menu, the console over everything) must not show the kept one. */
static void world_hide (void)
{
	int	i, set = seglSet ();

	if (!world_kept || world_hidden[set])
		return;
	for (i = 0; i < world_kept; i++)
		gpuKeptCommand (i)[2] = 0;
	world_hidden[set] = 1;
}

/* One of the level's own models that moves (a door, a platform): its runs, where it is now. */
static void draw_inline (const refEntity_t *e)
{
	const se_bmodel_t	*b;
	unsigned		i, n = (unsigned)e->hModel - INLINE_MODEL;

	if (!world_loaded || n >= world->models)
		return;
	b = &bmodels[n];
	if (!b->runs)
		return;
	entity_matrix (e->origin, e->axis, NULL, NULL);
	for (i = b->first_run; i < b->first_run + b->runs && i < (unsigned)world_count; i++) {
		run_t		*w = &world_runs[i];
		gpu_state	s = w->state;
		unsigned	c[16];

		if (w->changes) {
			c[7] = s.texture, c[8] = s.width, c[9] = s.height, c[11] = s.colour;
			c[12] = s.more[0], c[13] = s.more[1], c[14] = s.more[2], c[15] = s.more[3];
			run_now (&runs[i], w, c);
			s.texture = c[7], s.width = c[8], s.height = c[9], s.colour = c[11];
			s.more[0] = c[12], s.more[1] = c[13], s.more[2] = c[14], s.more[3] = c[15];
		}
		gpuDrawAt (&s, w->vertices, (int)w->count);
	}
}

/* ---- the light on what moves ---- */

static float		*facings;	/* in GPU memory: the 256 ways a model's vertex can face */

/* The light at a place, from the level's grid of it: ambient, directed (0 to 255) and from where. */
static int light_at (const float *place, float *ambient, float *directed, float *from)
{
	const unsigned char	*grid;
	float			frac[3], total = 0;
	int			pos[3], step[3], i, j, at[3], kept;

	VectorClear (ambient);
	VectorClear (directed);
	VectorClear (from);
	if (!world_loaded || !world->grid_points || (view.rdflags & RDF_NOWORLDMODEL))
		return 0;
	/* (a thing at rest, or bobbing where it lies, was asked about last frame: the grid's points are 64 units apart) */
	for (i = 0; i < 3; i++)
		at[i] = (int)floor_of (place[i] * (1.0f / 16));
	kept = (int)(((unsigned)at[0] * 73856093u ^ (unsigned)at[1] * 19349663u ^ (unsigned)at[2] * 83492791u) >> 8) & (LIGHT_KEPT - 1);
	if (light_kept[kept].valid && light_kept[kept].at[0] == at[0] && light_kept[kept].at[1] == at[1] && light_kept[kept].at[2] == at[2]) {
		const float *l = light_kept[kept].light;

		VectorCopy (l, ambient);
		VectorCopy (l + 3, directed);
		VectorCopy (l + 6, from);
		return 1;
	}
	for (i = 0; i < 3; i++) {
		float v = (place[i] - world->grid_origin[i]) / world->grid_size[i];

		pos[i] = (int)floor_of (v);
		frac[i] = v - pos[i];
		if (pos[i] < 0)
			pos[i] = 0;
		else if (pos[i] > world->grid_bounds[i] - 1)
			pos[i] = world->grid_bounds[i] - 1;
	}
	step[0] = 8, step[1] = 8 * world->grid_bounds[0], step[2] = 8 * world->grid_bounds[0] * world->grid_bounds[1];
	grid = dat + world->grid_at + pos[0] * step[0] + pos[1] * step[1] + pos[2] * step[2];
	for (i = 0; i < 8; i++) {
		const unsigned char	*d = grid;
		float			factor = 1, sl, cl, sg, cg;

		for (j = 0; j < 3; j++) {
			if (i & (1 << j)) {
				if (pos[j] + 1 > world->grid_bounds[j] - 1)
					break;
				factor *= frac[j];
				d += step[j];
			} else {
				factor *= 1 - frac[j];
			}
		}
		if (j != 3 || !(d[0] + d[1] + d[2]))
			continue;	/* outside the level, or in a wall */
		total += factor;
		ambient[0] += factor * d[0], ambient[1] += factor * d[1], ambient[2] += factor * d[2];
		directed[0] += factor * d[3], directed[1] += factor * d[4], directed[2] += factor * d[5];
		sl = sines[d[7]], cl = sines[(d[7] + 64) & 255], sg = sines[d[6]], cg = sines[(d[6] + 64) & 255];
		from[0] += factor * cl * sg, from[1] += factor * sl * sg, from[2] += factor * cg;
	}
	if (total > 0 && total < 0.99f) {
		total = 1 / total;
		VectorScale (ambient, total, ambient);
		VectorScale (directed, total, directed);
	}
	VectorNormalize (from);
	VectorCopy (at, light_kept[kept].at);
	VectorCopy (ambient, light_kept[kept].light);
	VectorCopy (directed, light_kept[kept].light + 3);
	VectorCopy (from, light_kept[kept].light + 6);
	light_kept[kept].valid = 1;
	return 1;
}

/*
 * The light on a thing, for the GPU to light its vertices by (docs/gpu.md, vertices that face):
 * from where in the thing's own space, its colour, and the colour of the light from
 * everywhere, three texels in the frame's memory. 0 when the frame has no room.
 */
static unsigned entity_light (const refEntity_t *e)
{
	float	ambient[3], directed[3], from[3], *out = gpuFrameMemory (48);
	int	i;

	if (!out)
		return 0;
	if (!light_at (e->renderfx & RF_LIGHTING_ORIGIN ? e->lightingOrigin : e->origin, ambient, directed, from)) {
		ambient[0] = ambient[1] = ambient[2] = directed[0] = directed[1] = directed[2] = 150;
		from[0] = 0.57735f, from[1] = 0.57735f, from[2] = 0.57735f;
	}
	for (i = 0; i < 3; i++) {
		out[i] = DotProduct (from, e->axis[i]);
		out[4 + i] = directed[i] * (1.0f / 255);
		out[8 + i] = (ambient[i] + 32) * (1.0f / 255);	/* (the game's own least light on everything) */
	}
	out[3] = out[7] = out[11] = 0;
	se_counts[2]++;
	return gpuAddress (out);
}

/* ---- models ---- */

static const se_shader_t *shader_handle (qhandle_t h)
{
	return h > 0 && h <= shader_count ? shader_of[h - 1] : NULL;
}

static void draw_model (const refEntity_t *e)
{
	const se_model_t	*m;
	const se_surface_t	*surf;
	const se_shader_t	*custom = shader_handle (e->customShader);
	const se_skin_t		*skin = e->customSkin > 0 && e->customSkin <= skin_count ? &skins[e->customSkin - 1] : NULL;
	unsigned		light = 0;
	const float		*info;
	float			centre[3], radius;
	int			frame, i, j, k, lit = 0;

	if (e->hModel >= INLINE_MODEL) {
		draw_inline (e);
		return;
	}
	if (e->hModel <= 0 || e->hModel > model_count)
		return;
	m = &models[e->hModel - 1];
	frame = e->backlerp > 0.5f ? e->oldframe : e->frame;
	if (frame < 0 || frame >= m->frames)
		frame = 0;
	info = (const float *)(dat + m->frames_at) + 4 * frame;
	for (i = 0; i < 3; i++)
		centre[i] = e->origin[i] + e->axis[0][i] * info[0] + e->axis[1][i] * info[1] + e->axis[2][i] * info[2];
	radius = e->nonNormalizedAxes ? info[3] * 2 : info[3];
	if (!(e->renderfx & RF_FIRST_PERSON) && culled (centre, radius))
		return;
	entity_matrix (e->origin, e->axis, m->scale, m->translate);
	se_counts[0]++;
	for (i = 0, surf = &surfaces[m->first_surface]; i < m->surfaces; i++, surf++) {
		const se_shader_t	*sh = custom;
		unsigned		vertices = dat_rom + surf->vertices_at + (unsigned)frame * surf->vertices * 4;

		if (!sh && skin) {
			/* (which of a skin's shaders a surface has, by its name: found once) */
			static struct { const se_skin_t *skin; const se_surface_t *surface; const se_shader_t *shader; } found[64], *f;

			f = &found[((unsigned)(skin - skins) * 31 + (unsigned)(surf - surfaces)) & 63];
			if (f->skin != skin || f->surface != surf) {
				f->skin = skin, f->surface = surf, f->shader = NULL;
				for (j = 0; j < skin->count; j++)
					if (!strcmp (skin_entries[skin->first + j].surface, surf->name)) {
						int n = skin_entries[skin->first + j].shader;

						f->shader = n >= 0 ? shader_of[n] : NULL;
						break;
					}
			}
			sh = f->shader;
		} else if (!sh && surf->shader >= 0) {
			sh = shader_of[surf->shader];
		}
		if (!sh || (sh->flags & SE_NODRAW))
			continue;
		for (k = 0; k < sh->stages; k++) {
			const se_stage_t	*st = &stages[sh->first_stage + k];
			gpu_state		s;

			if (st->flags & SE_LIGHTMAP)
				continue;
			stage_state (&s, st, 0);
			s.vertex = GPU_PACKED | GPU_TAGGED;
			s.more[0] = dat_rom + surf->coords_at;
			if (st->rgb == SE_RGB_DIFFUSE) {
				if (!lit)
					light = entity_light (e), lit = 1;
				if (light)
					s.vertex |= GPU_FACING, s.colour = gpuAddress (facings), s.more[1] = light;
			} else {
				s.colour = stage_colour (st, e->shaderRGBA, NULL);
			}
			if (st->tcmods && (s.more[3] = stage_moved (st, NULL)) != 0)
				s.vertex |= GPU_MOVED;
			gpuDrawAt (&s, vertices, (int)surf->vertices);
			se_counts[1]++;
		}
	}
}

/* ---- sprites, beams and the game's own polygons ---- */

static inline void corner (unsigned *v, const float *p, float s, float t)
{
	v[0] = float_bits (p[0]), v[1] = float_bits (p[1]), v[2] = float_bits (p[2]);
	v[3] = ((unsigned)(int)(s * 1024.0f) & 0xffff) | (unsigned)(int)(t * 1024.0f) << 16;
}

/* Four corners in order round a quad, with the whole of each stage's texture on it (`along` times, the long way). */
static void draw_quad (const se_shader_t *sh, const float corners[4][3], const byte *colour, float along)
{
	int	k;

	if (!sh)
		return;
	view_matrix_again ();
	se_counts[3]++;
	for (k = 0; k < sh->stages; k++) {
		const se_stage_t	*st = &stages[sh->first_stage + k];
		gpu_state		s;
		unsigned		*v;

		if (st->flags & SE_LIGHTMAP)
			continue;
		stage_state (&s, st, 0);
		s.vertex = GPU_QUADS;
		s.colour = stage_colour (st, colour, colour);
		if (st->tcmods && (s.more[3] = stage_moved (st, NULL)) != 0)
			s.vertex |= GPU_MOVED;
		if ((v = gpuDrawRoom (&s, 4, 6)) == NULL)
			return;
		corner (v, corners[0], 0, 0), corner (v + 4, corners[1], along, 0);
		corner (v + 8, corners[2], along, 1), corner (v + 12, corners[3], 0, 1);
	}
}

static void draw_sprite (const refEntity_t *e)
{
	float	left[3], up[3], c[4][3], r = e->radius;
	int	i;

	if (culled (e->origin, r * 1.5f))
		return;
	if (e->rotation == 0) {
		VectorScale (view.viewaxis[1], r, left);
		VectorScale (view.viewaxis[2], r, up);
	} else {
		float	turn = e->rotation * (1.0f / 360), sn, cs;

		turn -= floor_of (turn);
		sn = sines[(int)(turn * 256) & 255] * r, cs = sines[((int)(turn * 256) + 64) & 255] * r;
		for (i = 0; i < 3; i++) {
			left[i] = cs * view.viewaxis[1][i] - sn * view.viewaxis[2][i];
			up[i] = cs * view.viewaxis[2][i] + sn * view.viewaxis[1][i];
		}
	}
	for (i = 0; i < 3; i++) {
		c[0][i] = e->origin[i] + left[i] + up[i], c[1][i] = e->origin[i] - left[i] + up[i];
		c[2][i] = e->origin[i] - left[i] - up[i], c[3][i] = e->origin[i] + left[i] - up[i];
	}
	draw_quad (shader_handle (e->customShader), c, e->shaderRGBA, 1);
}

/* A beam from one place to another (a rail's core, a bolt of lightning): a strip turned to face the eye. */
static void draw_beam (const refEntity_t *e, float half, int crossed)
{
	float	along[3], to_eye[3], right[3], c[4][3], length;
	int	i, turn;

	VectorSubtract (e->oldorigin, e->origin, along);
	length = VectorNormalize (along);
	VectorSubtract (view.vieworg, e->origin, to_eye);
	CrossProduct (along, to_eye, right);
	if (VectorNormalize (right) == 0)
		return;
	for (turn = 0; turn <= crossed; turn++) {
		for (i = 0; i < 3; i++) {
			c[0][i] = e->origin[i] + right[i] * half, c[1][i] = e->oldorigin[i] + right[i] * half;
			c[2][i] = e->oldorigin[i] - right[i] * half, c[3][i] = e->origin[i] - right[i] * half;
		}
		draw_quad (shader_handle (e->customShader), c, e->shaderRGBA, length * (1.0f / 256));
		CrossProduct (along, right, to_eye);
		VectorCopy (to_eye, right);
	}
}

typedef struct { const se_shader_t *shader; int first, count; } scene_poly_t;
static refEntity_t	scene_entities[MAX_SCENE];
static scene_poly_t		scene_polys[MAX_POLYS];
static polyVert_t	scene_verts[MAX_POLY_VERTS];
static int		entity_count, poly_count, vert_count;

static void draw_poly (const scene_poly_t *p)
{
	const polyVert_t	*pv = &scene_verts[p->first];
	const se_shader_t	*sh = p->shader;
	float			nudge[3] = {0, 0, 0};
	int			k, i, n = p->count, quad = n == 4;

	if (sh->flags & SE_OFFSET)	/* a mark on a wall: a little off it, towards the eye */
		VectorScale (view.viewaxis[0], -0.5f, nudge);
	se_counts[4]++;
	for (k = 0; k < sh->stages; k++) {
		const se_stage_t	*st = &stages[sh->first_stage + k];
		gpu_state		s;
		unsigned		*v;
		float			at[3];

		if (st->flags & SE_LIGHTMAP)
			continue;
		stage_state (&s, st, 0);
		s.vertex = quad ? GPU_QUADS : 0;
		s.colour = stage_colour (st, pv[0].modulate, pv[0].modulate);
		if (st->tcmods && (s.more[3] = stage_moved (st, NULL)) != 0)
			s.vertex |= GPU_MOVED;
		if ((v = gpuDrawRoom (&s, quad ? 4 : 3 * (n - 2), quad ? 6 : 3 * (n - 2))) == NULL)
			return;
		if (quad) {
			for (i = 0; i < 4; i++, v += 4) {
				VectorAdd (pv[i].xyz, nudge, at);
				corner (v, at, pv[i].st[0], pv[i].st[1]);
			}
		} else {
			for (i = 1; i + 1 < n; i++, v += 12) {
				VectorAdd (pv[0].xyz, nudge, at), corner (v, at, pv[0].st[0], pv[0].st[1]);
				VectorAdd (pv[i].xyz, nudge, at), corner (v + 4, at, pv[i].st[0], pv[i].st[1]);
				VectorAdd (pv[i + 1].xyz, nudge, at), corner (v + 8, at, pv[i + 1].st[0], pv[i + 1].st[1]);
			}
		}
	}
}

/* ---- a scene ---- */

static void RE_ClearScene (void)
{
	entity_count = poly_count = vert_count = 0;
}

static void RE_AddRefEntityToScene (const refEntity_t *e)
{
	if (entity_count < MAX_SCENE && e->reType >= 0 && e->reType < RT_MAX_REF_ENTITY_TYPE)
		scene_entities[entity_count++] = *e;
}

static void RE_AddPolyToScene (qhandle_t hShader, int numVerts, const polyVert_t *verts, int num)
{
	const se_shader_t	*sh = shader_handle (hShader);
	int			i;

	if (!sh)
		return;
	for (i = 0; i < num; i++, verts += numVerts) {
		if (numVerts < 3 || poly_count == MAX_POLYS || vert_count + numVerts > MAX_POLY_VERTS)
			return;
		scene_polys[poly_count].shader = sh, scene_polys[poly_count].first = vert_count, scene_polys[poly_count].count = numVerts;
		memcpy (&scene_verts[vert_count], verts, numVerts * sizeof (polyVert_t));
		vert_count += numVerts, poly_count++;
	}
}

static int RE_LightForPoint (vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir)
{
	return light_at (point, ambientLight, directedLight, lightDir);
}

static void RE_AddLightToScene (const vec3_t org, float intensity, float r, float g, float b) {}

static void RE_RenderScene (const refdef_t *fd)
{
	int	i, was = SE_Phase (3);

	view_set (fd);
	if (!(fd->rdflags & RDF_NOWORLDMODEL) && world_loaded)
		world_frame ();
	for (i = 0; i < entity_count; i++) {
		const refEntity_t *e = &scene_entities[i];

		switch (e->reType) {
		case RT_MODEL:
			if (!(e->renderfx & RF_THIRD_PERSON))
				draw_model (e);
			break;
		case RT_SPRITE:
			draw_sprite (e);
			break;
		case RT_RAIL_CORE:
			draw_beam (e, 3, 0);
			break;
		case RT_LIGHTNING:
			draw_beam (e, 8, 1);
			break;
		default:
			break;
		}
	}
	view_matrix_again ();
	for (i = 0; i < poly_count; i++)
		draw_poly (&scene_polys[i]);
	/* (as the game's own renderer: a scene's entities and polygons are drawn once) */
	entity_count = poly_count = vert_count = 0;
	SE_Phase (was);
}

/* ---- the flat screen: the status bar, the console, the menus ---- */

static unsigned		flat_colour = 0x00ffffff;

static void RE_SetColor (const float *rgba)
{
	flat_colour = rgba ? colour_word (level (rgba[0] * 255.0f), level (rgba[1] * 255.0f), level (rgba[2] * 255.0f), level (rgba[3] * 255.0f)) : 0x00ffffff;
}

static void RE_StretchPic (float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader)
{
	const se_shader_t	*sh = shader_handle (hShader);
	gpu_state		s;
	unsigned		*v, x0, y0, x1, y1, u0, u1, v0, v1;

	if (!sh || !sh->stages)
		return;
	se_counts[5]++;
	if (matrix_is != 2) {
		float	m[16];

		memset (m, 0, sizeof m);
		m[0] = 2.0f / config.vidWidth, m[5] = -2.0f / config.vidHeight, m[10] = -1, m[12] = -1, m[13] = 1, m[15] = 1;
		glViewport (0, 0, config.vidWidth, config.vidHeight);
		gpuProjection (m);
		memset (m, 0, sizeof m);
		m[0] = m[5] = m[10] = m[15] = 1;
		gpuModelview (m);
		matrix_is = 2;
	}
	/* (drawn in the order they come, in the one pass that blends without depth: the first stage of each) */
	stage_state (&s, &stages[sh->first_stage], 1);
	s.vertex = GPU_QUADS;
	s.colour = flat_colour;
	if ((v = gpuDrawRoom (&s, 4, 6)) == NULL)
		return;
	x0 = float_bits (x), y0 = float_bits (y), x1 = float_bits (x + w), y1 = float_bits (y + h);
	u0 = (unsigned)(int)(s1 * 1024.0f) & 0xffff, u1 = (unsigned)(int)(s2 * 1024.0f) & 0xffff;
	v0 = (unsigned)(int)(t1 * 1024.0f) << 16, v1 = (unsigned)(int)(t2 * 1024.0f) << 16;
	v[0] = x0, v[1] = y0, v[2] = 0, v[3] = u0 | v0;
	v[4] = x1, v[5] = y0, v[6] = 0, v[7] = u1 | v0;
	v[8] = x1, v[9] = y1, v[10] = 0, v[11] = u1 | v1;
	v[12] = x0, v[13] = y1, v[14] = 0, v[15] = u0 | v1;
}

static void RE_StretchRaw (int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {}
static void RE_UploadCinematic (int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {}

/* ---- a frame ---- */

static void RE_BeginFrame (stereoFrame_t stereoFrame)
{
	world_seen = 0;
	matrix_is = 0;
	glClear (GL_COLOR_BUFFER_BIT);
}

static void RE_EndFrame (int *frontEndMsec, int *backEndMsec)
{
	int	was;

	if (frontEndMsec)
		*frontEndMsec = 0;
	if (backEndMsec)
		*backEndMsec = 0;
	if (!world_seen)
		world_hide ();
	was = SE_Phase (4);
	gpuSwap (0);
	SE_Phase (was);
	frame_number++;
	SE_FrameDone ();
}

/* ---- what the game registers ---- */

static qhandle_t register_shader (const char *name, const char *kind)
{
	char	clean[64];
	int	i;

	SE_Wanted (kind, name);
	clean_name (clean, name, 1);
	i = find (shaders, shader_count, sizeof (se_shader_t), clean);
	/* (a run that lists what the game asks for has it go on asking: nothing is missing) */
	return i < 0 ? se_wanted : shaders[i].number + 1;
}

static qhandle_t RE_RegisterShader (const char *name) { return register_shader (name, "shader"); }
static qhandle_t RE_RegisterShaderNoMip (const char *name) { return register_shader (name, "shader2d"); }

static qhandle_t RE_RegisterModel (const char *name)
{
	char	clean[64];
	int	i;

	SE_Wanted ("model", name);
	if (name[0] == '*')
		return INLINE_MODEL + atoi (name + 1);
	clean_name (clean, name, 0);
	i = find (models, model_count, sizeof (se_model_t), clean) + 1;
	return i ? i : se_wanted;
}

static qhandle_t RE_RegisterSkin (const char *name)
{
	char	clean[64];
	int	i;

	SE_Wanted ("skin", name);
	clean_name (clean, name, 0);
	i = find (skins, skin_count, sizeof (se_skin_t), clean) + 1;
	return i ? i : se_wanted;
}

static int RE_LerpTag (orientation_t *tag, qhandle_t handle, int startFrame, int endFrame, float frac, const char *tagName)
{
	const se_model_t	*m;
	const char		*names;
	const float		*t;
	int			i, frame;

	if (handle > 0 && handle <= model_count) {
		m = &models[handle - 1];
		names = (const char *)dat + m->tags_at;
		/* (the frame the model is drawn in: draw_model takes the nearer of the two as well) */
		frame = frac >= 0.5f ? endFrame : startFrame;
		if (frame < 0 || frame >= m->frames)
			frame = 0;
		for (i = 0; i < m->tags; i++)
			if (!strcmp (names + 64 * i, tagName)) {
				t = (const float *)(names + 64 * m->tags) + 12 * (frame * m->tags + i);
				VectorCopy (t, tag->origin);
				VectorCopy (t + 3, tag->axis[0]);
				VectorCopy (t + 6, tag->axis[1]);
				VectorCopy (t + 9, tag->axis[2]);
				return qtrue;
			}
	}
	AxisClear (tag->axis);
	VectorClear (tag->origin);
	return qfalse;
}

static void RE_ModelBounds (qhandle_t handle, vec3_t mins, vec3_t maxs)
{
	VectorClear (mins);
	VectorClear (maxs);
	if (handle >= INLINE_MODEL && world && (unsigned)(handle - INLINE_MODEL) < world->models) {
		VectorCopy (bmodels[handle - INLINE_MODEL].mins, mins);
		VectorCopy (bmodels[handle - INLINE_MODEL].maxs, maxs);
	} else if (handle > 0 && handle <= model_count) {
		VectorCopy (models[handle - 1].mins, mins);
		VectorCopy (models[handle - 1].maxs, maxs);
	}
}

static void RE_BeginRegistration (glconfig_t *out)
{
	int	i;

	if (!opened) {
		window_open ();
		data_open ();
		for (i = 0; i < 256; i++)
			sines[i] = sin (i * (2 * M_PI / 256));
		facings = seglMemory (SE_NORMALS * 16);
		if (!facings)
			ri.Error (ERR_FATAL, "no GPU memory");
		for (i = 0; i < SE_NORMALS; i++) {
			memcpy (facings + 4 * i, normals + 3 * i, 12);
			facings[4 * i + 3] = 0;
		}
		glClearColor (0, 0, 0, 0);
	}
	memset (&config, 0, sizeof config);
	Q_strncpyz (config.renderer_string, "the machine's GPU device", sizeof config.renderer_string);
	Q_strncpyz (config.vendor_string, "ShaderEmu", sizeof config.vendor_string);
	Q_strncpyz (config.version_string, "1", sizeof config.version_string);
	config.maxTextureSize = 256;
	config.numTextureUnits = 1;
	config.colorBits = 32, config.depthBits = 24;
	seglSize (&config.vidWidth, &config.vidHeight);
	config.windowAspect = (float)config.vidWidth / config.vidHeight;
	config.displayFrequency = 60;
	*out = config;
}

static void RE_Shutdown (qboolean destroyWindow)
{
	if (opened && destroyWindow)
		world_free ();
}

static void RE_SetWorldVisData (const byte *vis) {}
static void RE_EndRegistration (void) {}
static int RE_MarkFragments (int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints, vec3_t pointBuffer,
	int maxFragments, markFragment_t *fragmentBuffer) { return 0; }
static void RE_RegisterFont (const char *fontName, int pointSize, fontInfo_t *font) {}
static void RE_RemapShader (const char *oldShader, const char *newShader, const char *offsetTime) {}
static qboolean RE_GetEntityToken (char *buffer, int size) { return qfalse; }
static qboolean RE_inPVS (const vec3_t p1, const vec3_t p2) { return qtrue; }
static void RE_TakeVideoFrame (int h, int w, byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg) {}

/* What the client's game asks most often, for it to call (quake3.patch, cg_syscalls.c): the modules are the same program. */
void SE_AddRefEntity (const refEntity_t *e) { RE_AddRefEntityToScene (e); }
void SE_SetColor (const float *rgba) { RE_SetColor (rgba); }
void SE_StretchPic (float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader)
{
	RE_StretchPic (x, y, w, h, s1, t1, s2, t2, hShader);
}
int SE_LerpTag (orientation_t *tag, qhandle_t model, int startFrame, int endFrame, float frac, const char *tagName)
{
	return RE_LerpTag (tag, model, startFrame, endFrame, frac, tagName);
}

refexport_t *GetRefAPI (int apiVersion, refimport_t *rimp)
{
	static refexport_t re;

	ri = *rimp;
	memset (&re, 0, sizeof re);
	if (apiVersion != REF_API_VERSION)
		return NULL;
	re.Shutdown = RE_Shutdown;
	re.BeginRegistration = RE_BeginRegistration;
	re.RegisterModel = RE_RegisterModel;
	re.RegisterSkin = RE_RegisterSkin;
	re.RegisterShader = RE_RegisterShader;
	re.RegisterShaderNoMip = RE_RegisterShaderNoMip;
	re.LoadWorld = RE_LoadWorld;
	re.SetWorldVisData = RE_SetWorldVisData;
	re.EndRegistration = RE_EndRegistration;
	re.BeginFrame = RE_BeginFrame;
	re.EndFrame = RE_EndFrame;
	re.MarkFragments = RE_MarkFragments;
	re.LerpTag = RE_LerpTag;
	re.ModelBounds = RE_ModelBounds;
	re.ClearScene = RE_ClearScene;
	re.AddRefEntityToScene = RE_AddRefEntityToScene;
	re.AddPolyToScene = RE_AddPolyToScene;
	re.LightForPoint = RE_LightForPoint;
	re.AddLightToScene = RE_AddLightToScene;
	re.AddAdditiveLightToScene = RE_AddLightToScene;
	re.RenderScene = RE_RenderScene;
	re.SetColor = RE_SetColor;
	re.DrawStretchPic = RE_StretchPic;
	re.DrawStretchRaw = RE_StretchRaw;
	re.UploadCinematic = RE_UploadCinematic;
	re.RegisterFont = RE_RegisterFont;
	re.RemapShader = RE_RemapShader;
	re.GetEntityToken = RE_GetEntityToken;
	re.inPVS = RE_inPVS;
	re.TakeVideoFrame = RE_TakeVideoFrame;
	return &re;
}
