/* The mesh viewer's meshes (scene.h): read from Wavefront and Stanford files or made here, an
 * object's matrix and the camera's eye. The files' numbers are read by number(): the machine
 * has no doubles and the library's readers work in them. */
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "scene.h"

struct sc_scene sc;

/* A file's mesh before it is one of sc's: points, normals if it had any, and three corners a
 * triangle as numbers of those (a corner's normal -1: the file gave it none). */
struct loaded {
	float *points, *normals;
	int *corners, *corner_normals;
	int point_count, normal_count, triangles, room;
};

static const float ten[39] = {
	1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f, 1e11f, 1e12f,
	1e13f, 1e14f, 1e15f, 1e16f, 1e17f, 1e18f, 1e19f, 1e20f, 1e21f, 1e22f, 1e23f, 1e24f, 1e25f,
	1e26f, 1e27f, 1e28f, 1e29f, 1e30f, 1e31f, 1e32f, 1e33f, 1e34f, 1e35f, 1e36f, 1e37f, 1e38f,
};

static inline int
digit(char c)
{
	return (unsigned)(c - '0') < 10u;
}

/* A decimal number of a text, after blanks: its first nine digits and a power of ten, so right
 * to a float's last bit or the one before. Where it ends; `p` itself where there is none. */
static const char *
number(const char *p, float *out)
{
	const char *from = p;
	uint32_t whole = 0;
	int digits = 0, power = 0, minus = 0, any = 0;
	float f;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '-' || *p == '+')
		minus = *p++ == '-';
	for (; digit(*p); p++, any = 1) {
		if (digits < 9)
			whole = whole * 10 + (uint32_t)(*p - '0'), digits += whole != 0;
		else
			power++;
	}
	if (*p == '.')
		for (p++; digit(*p); p++, any = 1)
			if (digits < 9)
				whole = whole * 10 + (uint32_t)(*p - '0'), digits += whole != 0, power--;
	if (!any)
		return from;
	if ((*p == 'e' || *p == 'E') && (digit(p[1]) || ((p[1] == '-' || p[1] == '+') && digit(p[2])))) {
		int by = 0, down = p[1] == '-';

		for (p += digit(p[1]) ? 1 : 2; digit(*p); p++)
			if (by < 1000)
				by = by * 10 + (*p - '0');
		power += down ? -by : by;
	}
	f = (float)whole;
	for (; power < -38; power += 38)
		f /= ten[38];
	if (power < 0)
		f /= ten[-power];
	else if (power > 38)
		f = whole ? 3e38f : 0.0f;
	else
		f *= ten[power];
	if (!(f < 3e38f))
		f = 3e38f;
	*out = minus ? -f : f;
	return p;
}

/* A whole number of a text, after blanks; `p` itself where there is none. */
static const char *
whole_number(const char *p, int *out)
{
	const char *from = p;
	int n = 0, minus = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '-' || *p == '+')
		minus = *p++ == '-';
	if (!digit(*p))
		return from;
	for (; digit(*p); p++)
		if (n < 100000000)
			n = n * 10 + (*p - '0');
	*out = minus ? -n : n;
	return p;
}

/* A whole file and a zero after it, for free(); its length. */
static char *
file_read(const char *path, long *length)
{
	int file = open(path, O_RDONLY);
	long size, have = 0, got;
	char *text;

	if (file < 0)
		return 0;
	size = (long)lseek(file, 0, SEEK_END);
	lseek(file, 0, SEEK_SET);
	text = size >= 0 ? malloc((size_t)size + 1) : 0;
	while (text && have < size && (got = (long)read(file, text + have, (size_t)(size - have))) > 0)
		have += got;
	close(file);
	if (!text)
		return 0;
	text[have] = 0;
	*length = have;
	return text;
}

/* One more triangle of a file's mesh: false when there is no memory for it. */
static int
loaded_add(struct loaded *l, const int *points, const int *normals)
{
	int k;

	if (l->triangles == l->room) {
		int room = l->room ? l->room * 2 : 1024;
		int *corners = realloc(l->corners, (size_t)room * 3 * sizeof(int)), *corner_normals;

		if (!corners)
			return 0;
		l->corners = corners;
		if (!(corner_normals = realloc(l->corner_normals, (size_t)room * 3 * sizeof(int))))
			return 0;
		l->corner_normals = corner_normals;
		l->room = room;
	}
	for (k = 0; k < 3; k++) {
		l->corners[l->triangles * 3 + k] = points[k];
		l->corner_normals[l->triangles * 3 + k] = normals[k];
	}
	l->triangles++;
	return 1;
}

/* One more corner of a face, which is a fan of triangles about its first. */
static int
fan_add(struct loaded *l, int at, int *points, int *normals, int point, int normal)
{
	if (at < 2) {
		points[at] = point, normals[at] = normal;
		return 1;
	}
	points[2] = point, normals[2] = normal;
	if (!loaded_add(l, points, normals))
		return 0;
	points[1] = point, normals[1] = normal;
	return 1;
}

static const char *
line_after(const char *p)
{
	while (*p && *p != '\n')
		p++;
	return *p ? p + 1 : p;
}

/* A Wavefront file: its v, vn and f lines. What is wrong with it, or 0. */
static const char *
obj_read(const char *text, long length, struct loaded *l)
{
	const char *p;
	int k;

	/* (room for as many as the file could have: a line of either is eight bytes at least) */
	l->points = malloc((size_t)(length / 8 + 1) * 3 * sizeof(float));
	l->normals = malloc((size_t)(length / 8 + 1) * 3 * sizeof(float));
	if (!l->points || !l->normals)
		return "no memory for it";
	for (p = text; *p; p = line_after(p)) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t' || (p[1] == 'n' && (p[2] == ' ' || p[2] == '\t')))) {
			float *to = p[1] == 'n' ? l->normals + 3 * l->normal_count++ : l->points + 3 * l->point_count++;

			p += 2;
			for (k = 0; k < 3; k++) {
				to[k] = 0.0f;
				p = number(p, &to[k]);
			}
		} else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
			int fan_points[3], fan_normals[3], at = 0;

			/* (a corner is point, point/texture, point//normal or point/texture/normal) */
			for (p++;; at++) {
				const char *next;
				int point, normal = 0, texture;

				if ((next = whole_number(p, &point)) == p)
					break;
				p = next;
				if (*p == '/') {
					p = whole_number(p + 1, &texture);
					if (*p == '/')
						p = whole_number(p + 1, &normal);
				}
				/* (from 1, or back from the last one read so far) */
				point = point < 0 ? l->point_count + point : point - 1;
				normal = normal < 0 ? l->normal_count + normal : normal - 1;
				if (!fan_add(l, at, fan_points, fan_normals, point, normal))
					return "no memory for it";
			}
		}
	}
	return 0;
}

/* A Stanford file's kinds of number, and one property of an element: a list has a count's kind. */
enum { PLY_CHAR, PLY_UCHAR, PLY_SHORT, PLY_USHORT, PLY_INT, PLY_UINT, PLY_FLOAT, PLY_DOUBLE, PLY_KINDS };
enum { PLY_OTHER, PLY_X, PLY_NX = PLY_X + 3, PLY_CORNERS = PLY_NX + 3 };
struct ply_property { signed char kind, count_kind, what; };
struct ply_element { int count, properties, vertices; struct ply_property property[32]; };
struct ply { const unsigned char *at, *end; int text, bad; };

static int
ply_kind(const char *name, int length)
{
	static const char *const names[PLY_KINDS * 2] = {"char", "uchar", "short", "ushort", "int", "uint", "float", "double",
	                                                 "int8", "uint8", "int16", "uint16", "int32", "uint32", "float32", "float64"};
	int i;

	for (i = 0; i < PLY_KINDS * 2; i++)
		if ((int)strlen(names[i]) == length && !memcmp(names[i], name, (size_t)length))
			return i % PLY_KINDS;
	return -1;
}

/* A double's bits as the nearest float; too small for one, too large or no number is 0. */
static float
narrowed(uint32_t low, uint32_t high)
{
	int power = (int)(high >> 20 & 0x7ff) - 1023 + 127;
	uint32_t rest = low & 0x1fffffffu;
	union { uint32_t u; float f; } bits;

	if (power <= 0 || power >= 254)
		return 0.0f;
	bits.u = (uint32_t)power << 23 | (high & 0xfffff) << 3 | low >> 29;
	/* (the bits cut off count for one more when they are over a half, or a half after an odd bit) */
	bits.u += rest > 0x10000000u || (rest == 0x10000000u && (bits.u & 1));
	bits.u |= high & 0x80000000u;
	return bits.f;
}

/* The next number of a Stanford file, of a kind: as a float, and as a whole number. */
static float
ply_next(struct ply *in, int kind, int *whole)
{
	static const unsigned char bytes[PLY_KINDS] = {1, 1, 2, 2, 4, 4, 4, 8};
	const unsigned char *p = in->at;
	union { uint32_t u; float f; } bits;
	uint32_t low, high;
	float f = 0.0f;

	*whole = 0;
	if (in->text) {
		const char *next;

		while (p < in->end && *p <= ' ')
			p++;
		if (kind >= PLY_FLOAT) {
			next = number((const char *)p, &f);
			*whole = (int)f;
		} else {
			next = whole_number((const char *)p, whole);
			f = (float)*whole;
			/* (a whole number written with a point: the rest of it is passed over) */
			while (digit(*next) || *next == '.')
				next++;
		}
		if (next == (const char *)p)
			in->bad = 1;
		in->at = (const unsigned char *)next;
		return f;
	}
	if (in->end - p < bytes[kind]) {
		in->bad = 1;
		return 0.0f;
	}
	in->at = p + bytes[kind];
	low = p[0];
	if (bytes[kind] > 1)
		low |= (uint32_t)p[1] << 8;
	if (bytes[kind] > 2)
		low |= (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	switch (kind) {
	case PLY_CHAR: *whole = (signed char)low; break;
	case PLY_SHORT: *whole = (short)low; break;
	case PLY_FLOAT:
		/* (a number that is none, or endless, is 0) */
		bits.u = (low & 0x7f800000u) == 0x7f800000u ? 0 : low;
		*whole = (int)(f = bits.f);
		return f;
	case PLY_DOUBLE:
		high = p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
		*whole = (int)(f = narrowed(low, high));
		return f;
	default: *whole = (int)low;
	}
	return (float)*whole;
}

static int
word_is(const char *p, int length, const char *word)
{
	return (int)strlen(word) == length && !memcmp(p, word, (size_t)length);
}

/* The next word of a header's line: where it begins, and its length (0 at the line's end). */
static const char *
header_word(const char **at, int *length)
{
	const char *p = *at, *word;

	while (*p == ' ' || *p == '\t')
		p++;
	for (word = p; *p > ' '; p++)
		;
	*length = (int)(p - word);
	*at = p;
	return word;
}

/* A Stanford file, as text or as little-endian bytes. What is wrong with it, or 0. */
static const char *
ply_read(const char *text, long length, struct loaded *l)
{
	struct ply_element elements[8], *element = 0;
	struct ply in = {0, (const unsigned char *)text + length, -1, 0};
	const char *p = text, *word;
	int count = 0, size, e, i, k, whole;

	for (;; p = line_after(p)) {
		if (!*p)
			return "its header has no end";
		word = header_word(&p, &size);
		if (word_is(word, size, "end_header")) {
			in.at = (const unsigned char *)line_after(p);
			break;
		}
		if (word_is(word, size, "format")) {
			word = header_word(&p, &size);
			in.text = word_is(word, size, "ascii") ? 1 : word_is(word, size, "binary_little_endian") ? 0 : -1;
		} else if (word_is(word, size, "element")) {
			if (count == 8)
				return "it has too many kinds of thing";
			element = &elements[count++];
			word = header_word(&p, &size);
			element->vertices = word_is(word, size, "vertex") ? 1 : word_is(word, size, "face") ? 0 : -1;
			element->count = element->properties = 0;
			whole_number(p, &element->count);
		} else if (word_is(word, size, "property") && element) {
			struct ply_property *property = &element->property[element->properties];
			static const char *const names[6] = {"x", "y", "z", "nx", "ny", "nz"};

			if (element->properties++ == 32)
				return "a thing of it has too many properties";
			word = header_word(&p, &size);
			property->count_kind = -1;
			if (word_is(word, size, "list")) {
				word = header_word(&p, &size);
				property->count_kind = (signed char)ply_kind(word, size);
				word = header_word(&p, &size);
				if (property->count_kind < 0)
					return "it has a kind of number not known here";
			}
			if ((property->kind = (signed char)ply_kind(word, size)) < 0)
				return "it has a kind of number not known here";
			word = header_word(&p, &size);
			property->what = PLY_OTHER;
			for (i = 0; i < 6; i++)
				if (element->vertices == 1 && property->count_kind < 0 && word_is(word, size, names[i]))
					property->what = (signed char)(PLY_X + i);
			if (element->vertices == 0 && property->count_kind >= 0 && (word_is(word, size, "vertex_indices") || word_is(word, size, "vertex_index")))
				property->what = PLY_CORNERS;
		}
	}
	if (in.text < 0)
		return "it is neither text nor little-endian";
	for (e = 0; e < count; e++) {
		float *points = 0, *normals = 0;

		element = &elements[e];
		if (element->vertices == 1 && !l->points) {
			/* (no room is asked for vertices the file is too short to have) */
			if (element->count < 0 || element->count > length)
				return "it is cut short";
			points = l->points = malloc((size_t)(element->count + 1) * 3 * sizeof(float));
			for (k = 0; k < element->properties; k++)
				if (element->property[k].what >= PLY_NX && !l->normals)
					normals = l->normals = malloc((size_t)(element->count + 1) * 3 * sizeof(float));
			if (!points || (!normals && l->normals))
				return "no memory for it";
			memset(points, 0, (size_t)element->count * 3 * sizeof(float));
			if (normals)
				memset(normals, 0, (size_t)element->count * 3 * sizeof(float));
			l->point_count = element->count;
			l->normal_count = normals ? element->count : 0;
		}
		for (i = 0; i < element->count && !in.bad; i++)
			for (k = 0; k < element->properties; k++) {
				const struct ply_property *property = &element->property[k];
				int fan_points[3], fan_normals[3], many = 1, at;
				float f;

				if (property->count_kind >= 0)
					ply_next(&in, property->count_kind, &many);
				for (at = 0; at < many && !in.bad; at++) {
					f = ply_next(&in, property->kind, &whole);
					if (property->what == PLY_CORNERS) {
						if (!fan_add(l, at, fan_points, fan_normals, whole, l->normals ? whole : -1))
							return "no memory for it";
					} else if (property->what >= PLY_NX && normals) {
						normals[i * 3 + property->what - PLY_NX] = f;
					} else if (property->what >= PLY_X && points) {
						points[i * 3 + property->what - PLY_X] = f;
					}
				}
			}
		if (in.bad)
			return "it is cut short";
	}
	return 0;
}

static inline void
across(const float *a, const float *b, const float *c, float *out)
{
	float e[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, f[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};

	out[0] = e[1] * f[2] - e[2] * f[1], out[1] = e[2] * f[0] - e[0] * f[2], out[2] = e[0] * f[1] - e[1] * f[0];
}

/* Makes a vector one long; false, and it is left, when it has no length. */
static inline int
one_long(float *n)
{
	float d = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];

	if (!(d > 0.0f))
		return 0;
	d = 1.0f / __builtin_sqrtf(d);
	n[0] *= d, n[1] *= d, n[2] *= d;
	return 1;
}

/* A free place of sc.meshes with room for so many triangles, named: its number; -1 when
 * there is no place, -2 when there is no memory. */
static int
mesh_new(const char *name, int triangles)
{
	struct sc_mesh *mesh;
	int i, length = 0;

	for (i = 0; i < SC_MESHES && sc.meshes[i].positions; i++)
		;
	if (i == SC_MESHES)
		return -1;
	mesh = &sc.meshes[i];
	memset(mesh, 0, sizeof *mesh);
	mesh->positions = malloc((size_t)triangles * 9 * sizeof(float));
	mesh->normals = malloc((size_t)triangles * 9 * sizeof(float));
	if (!mesh->positions || !mesh->normals) {
		free(mesh->positions), free(mesh->normals);
		mesh->positions = mesh->normals = 0;
		return -2;
	}
	while (length < SC_NAME - 1 && name[length] && name[length] != '.')
		length++;
	memcpy(mesh->name, name, (size_t)length);
	mesh->triangles = triangles;
	if (sc.mesh_count <= i)
		sc.mesh_count = i + 1;
	return i;
}

/* A mesh's low and high corners, of its positions. */
static void
mesh_bounds(struct sc_mesh *mesh)
{
	const float *at = mesh->positions, *end = at + mesh->triangles * 9;
	float x0 = at[0], y0 = at[1], z0 = at[2], x1 = x0, y1 = y0, z1 = z0;

	for (at += 3; at < end; at += 3) {
		x0 = __builtin_fminf(x0, at[0]), y0 = __builtin_fminf(y0, at[1]), z0 = __builtin_fminf(z0, at[2]);
		x1 = __builtin_fmaxf(x1, at[0]), y1 = __builtin_fmaxf(y1, at[1]), z1 = __builtin_fmaxf(z1, at[2]);
	}
	mesh->low[0] = x0, mesh->low[1] = y0, mesh->low[2] = z0;
	mesh->high[0] = x1, mesh->high[1] = y1, mesh->high[2] = z1;
}

/* A file's mesh as one of sc's: a corner the file gave no normal has its point's, which is
 * the sum of the normals of the faces that use the point, made one long. */
static int
mesh_of(const char *name, const struct loaded *l, const char **wrong)
{
	struct sc_mesh *mesh;
	float *sums = 0;
	int i, k, n, own = 0, slot;

	*wrong = l->triangles ? "it names a point that is not there" : "it has no faces";
	if (!l->triangles)
		return -1;
	for (i = 0; i < l->triangles * 3; i++) {
		if (l->corners[i] < 0 || l->corners[i] >= l->point_count || l->corner_normals[i] >= l->normal_count)
			return -1;
		own |= l->corner_normals[i] < 0;
	}
	*wrong = "no memory for it";
	if (own && !(sums = calloc((size_t)l->point_count * 3, sizeof(float))))
		return -1;
	if ((slot = mesh_new(name, l->triangles)) < 0) {
		free(sums);
		if (slot == -1)
			*wrong = "there are too many meshes";
		return -1;
	}
	mesh = &sc.meshes[slot];
	for (i = 0; i < l->triangles * 3; i++) {
		const float *from = l->points + l->corners[i] * 3;
		float *to = mesh->positions + i * 3;

		to[0] = from[0], to[1] = from[1], to[2] = from[2];
	}
	for (i = 0; own && i < l->triangles; i++) {
		float face[3];

		across(mesh->positions + i * 9, mesh->positions + i * 9 + 3, mesh->positions + i * 9 + 6, face);
		if (one_long(face))
			for (n = 0; n < 3; n++)
				for (k = 0; k < 3; k++)
					sums[l->corners[i * 3 + n] * 3 + k] += face[k];
	}
	for (i = 0; own && i < l->point_count; i++)
		if (!one_long(sums + i * 3))
			sums[i * 3 + 1] = 1.0f;
	for (i = 0; i < l->triangles * 3; i++) {
		const float *from = l->corner_normals[i] < 0 ? sums + l->corners[i] * 3 : l->normals + l->corner_normals[i] * 3;
		float *to = mesh->normals + i * 3;

		to[0] = from[0], to[1] = from[1], to[2] = from[2];
	}
	free(sums);
	mesh_bounds(mesh);
	return slot;
}

int
sc_mesh_load(const char *path, char *why, int size)
{
	struct loaded l = {0};
	const char *name = path, *wrong = "it cannot be read", *p;
	long length = 0;
	char *text = file_read(path, &length);
	int slot = -1;

	for (p = path; *p; p++)
		if (*p == '/')
			name = p + 1;
	if (text) {
		wrong = length >= 4 && !memcmp(text, "ply", 3) && text[3] <= ' ' ? ply_read(text, length, &l) : obj_read(text, length, &l);
		if (!wrong)
			slot = mesh_of(name, &l, &wrong);
	}
	free(text), free(l.points), free(l.normals), free(l.corners), free(l.corner_normals);
	if (slot < 0 && why && size > 0) {
		strncpy(why, wrong, (size_t)size - 1);
		why[size - 1] = 0;
	}
	return slot;
}

/* One more triangle of a mesh made here, wound so that its face looks the way `out` goes. */
static void
made_add(struct sc_mesh *mesh, int *at, const float *a, const float *b, const float *c, const float *out)
{
	float *to = mesh->positions + *at * 9, face[3];

	across(a, b, c, face);
	if (face[0] * out[0] + face[1] * out[1] + face[2] * out[2] < 0.0f) {
		const float *swap = b;

		b = c, c = swap;
	}
	memcpy(to, a, 3 * sizeof(float)), memcpy(to + 3, b, 3 * sizeof(float)), memcpy(to + 6, c, 3 * sizeof(float));
	(*at)++;
}

int
sc_mesh_plane(void)
{
	static const float corner[4][3] = {{-1, 0, 1}, {1, 0, 1}, {1, 0, -1}, {-1, 0, -1}}, up[3] = {0, 1, 0};
	int slot = mesh_new("plane", 2), at = 0, i;

	if (slot < 0)
		return -1;
	made_add(&sc.meshes[slot], &at, corner[0], corner[1], corner[2], up);
	made_add(&sc.meshes[slot], &at, corner[0], corner[2], corner[3], up);
	for (i = 0; i < 6; i++)
		memcpy(sc.meshes[slot].normals + i * 3, up, sizeof up);
	mesh_bounds(&sc.meshes[slot]);
	return slot;
}

int
sc_mesh_box(void)
{
	int slot = mesh_new("box", 12), at = 0, side, i, k;

	if (slot < 0)
		return -1;
	for (side = 0; side < 6; side++) {
		int axis = side >> 1, u = (axis + 1) % 3, w = (axis + 2) % 3;
		float out[3] = {0, 0, 0}, corner[4][3];

		out[axis] = side & 1 ? 1.0f : -1.0f;
		for (i = 0; i < 4; i++) {
			corner[i][axis] = 0.5f * out[axis];
			corner[i][u] = i == 1 || i == 2 ? 0.5f : -0.5f;
			corner[i][w] = i >= 2 ? 0.5f : -0.5f;
		}
		for (i = at * 3; i < at * 3 + 6; i++)
			for (k = 0; k < 3; k++)
				sc.meshes[slot].normals[i * 3 + k] = out[k];
		made_add(&sc.meshes[slot], &at, corner[0], corner[1], corner[2], out);
		made_add(&sc.meshes[slot], &at, corner[0], corner[2], corner[3], out);
	}
	mesh_bounds(&sc.meshes[slot]);
	return slot;
}

/* A point of the ball of radius one half: `ring` from the top, `part` round it; the sines and
 * cosines are of whole steps of the angle between two rings. */
static inline void
ball_point(const float *sines, const float *cosines, int ring, int part, float *out)
{
	out[0] = 0.5f * sines[ring] * cosines[part], out[1] = 0.5f * cosines[ring], out[2] = 0.5f * sines[ring] * sines[part];
}

int
sc_mesh_ball(int rings)
{
	float sines[129], cosines[129], step_sine, step_cosine;
	struct sc_mesh *mesh;
	int slot, at = 0, i, j, parts;

	rings = rings < 2 ? 2 : rings > 64 ? 64 : rings;
	parts = rings * 2;
	if ((slot = mesh_new("ball", 4 * rings * (rings - 1))) < 0)
		return -1;
	mesh = &sc.meshes[slot];
	/* (one sine and cosine of the library's, and each step a turn by it) */
	step_sine = sinf(3.14159265f / (float)rings), step_cosine = cosf(3.14159265f / (float)rings);
	sines[0] = 0.0f, cosines[0] = 1.0f;
	for (i = 1; i <= parts; i++) {
		sines[i] = sines[i - 1] * step_cosine + cosines[i - 1] * step_sine;
		cosines[i] = cosines[i - 1] * step_cosine - sines[i - 1] * step_sine;
	}
	sines[rings] = 0.0f, cosines[rings] = -1.0f;
	sines[parts] = 0.0f, cosines[parts] = 1.0f;
	for (i = 0; i < rings; i++)
		for (j = 0; j < parts; j++) {
			float a[3], b[3], c[3], d[3];

			ball_point(sines, cosines, i, j, a), ball_point(sines, cosines, i, j + 1, b);
			ball_point(sines, cosines, i + 1, j + 1, c), ball_point(sines, cosines, i + 1, j, d);
			/* (at the top and the bottom two of the four are one point) */
			if (i < rings - 1)
				made_add(mesh, &at, a, c, d, d);
			if (i > 0)
				made_add(mesh, &at, a, b, c, b);
		}
	for (i = 0; i < mesh->triangles * 9; i++)
		mesh->normals[i] = 2.0f * mesh->positions[i];
	mesh_bounds(mesh);
	return slot;
}

void
sc_mesh_free(int mesh)
{
	if (mesh < 0 || mesh >= SC_MESHES)
		return;
	free(sc.meshes[mesh].positions), free(sc.meshes[mesh].normals);
	memset(&sc.meshes[mesh], 0, sizeof sc.meshes[mesh]);
	while (sc.mesh_count > 0 && !sc.meshes[sc.mesh_count - 1].positions)
		sc.mesh_count--;
}

/* The object sized, then turned about x, about y and about z, then put in its place: with a
 * point as a column, place * turn z * turn y * turn x * size. */
void
sc_object_matrix(struct sc_object *object)
{
	const float degree = 3.14159265f / 180.0f;
	float sx = sinf(object->turn[0] * degree), cx = cosf(object->turn[0] * degree);
	float sy = sinf(object->turn[1] * degree), cy = cosf(object->turn[1] * degree);
	float sz = sinf(object->turn[2] * degree), cz = cosf(object->turn[2] * degree);
	float *m = object->matrix;

	m[0] = cz * cy * object->size[0], m[1] = sz * cy * object->size[0], m[2] = -sy * object->size[0];
	m[4] = (cz * sy * sx - sz * cx) * object->size[1], m[5] = (sz * sy * sx + cz * cx) * object->size[1], m[6] = cy * sx * object->size[1];
	m[8] = (cz * sy * cx + sz * sx) * object->size[2], m[9] = (sz * sy * cx - cz * sx) * object->size[2], m[10] = cy * cx * object->size[2];
	m[12] = object->place[0], m[13] = object->place[1], m[14] = object->place[2];
	m[3] = m[7] = m[11] = 0.0f, m[15] = 1.0f;
}

/* A right-handed camera, as OpenGL's: right x up is back to the eye. With no turn and no tilt
 * the eye is at +z of the target and looks along -z, +x to its right and +y up; a turn takes
 * the eye towards +x, a tilt lifts it. */
void
sc_material(struct sc_object *object, int kind)
{
	object->rough = kind == SC_PLASTIC ? 0.3f : kind == SC_METAL ? 0.35f : kind == SC_MIRROR || kind == SC_GLASS ? 0.0f : 1.0f;
	object->metal = kind == SC_METAL || kind == SC_MIRROR ? 1.0f : 0.0f;
	object->glass = kind == SC_GLASS ? 1.0f : 0.0f;
	object->ior = 1.5f;
	object->density = kind == SC_GLASS ? 2.0f : 0.0f;
	object->glow = kind == SC_GLOW ? 5.0f : 0.0f;
}

void
sc_camera_set(struct sc_camera *camera)
{
	const float degree = 3.14159265f / 180.0f;
	float st = sinf(camera->turn * degree), ct = cosf(camera->turn * degree);
	float sl = sinf(camera->tilt * degree), cl = cosf(camera->tilt * degree);
	float back[3] = {cl * st, sl, cl * ct};
	int k;

	camera->right[0] = ct, camera->right[1] = 0.0f, camera->right[2] = -st;
	camera->up[0] = -sl * st, camera->up[1] = cl, camera->up[2] = -sl * ct;
	for (k = 0; k < 3; k++) {
		camera->eye[k] = camera->target[k] + camera->far * back[k];
		camera->ahead[k] = -back[k];
	}
}
