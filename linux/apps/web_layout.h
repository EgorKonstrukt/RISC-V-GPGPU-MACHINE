/*
 * Lays the page's tree (web.h) out for a width: boxes one under the other, text and pictures
 * in lines, tables, rows of boxes side by side (flex, grid), floats. What comes out is a list
 * of things to draw at places of the page (docs/apps/web.md). The program that includes this gives
 * it web_picture(): a picture's number and size by its address.
 */
#ifndef NXWEB_WEB_LAYOUT_H
#define NXWEB_WEB_LAYOUT_H

#include "web.h"

/* A picture's number for the program that draws (-1: it cannot be shown), and its size in its
 * own pixels when that is known (else 0). */
static int web_picture(int node, const char *address, int *w, int *h);

enum { I_TEXT, I_BOX, I_IMAGE, I_FIELD };
enum { IF_UNDER = 1, IF_THROUGH = 2, IF_ITALIC = 4 };

/* A thing to draw. Text: a, b are its characters in web_u; a picture: a is its number; a field
 * (an input, a select) is drawn by the program from its node. */
struct web_item { int x, y, w, h, a, b, colour, node; unsigned char kind, face, flags; };
struct web_piece { int first, last, asc, h; };		/* items that stand on a line's baseline together */
struct web_floats { int n; struct { int x0, x1, y0, y1, right; } f[32]; };

/* The lines being filled in one box. */
struct web_flow {
	int x0, x1, y;			/* the box's edges, and the top of the line being filled */
	int left, right, x;		/* that line's edges beside the floats, and where its next piece goes */
	int piece0, any, space;		/* its pieces; whether it has any; the space owed before the next word */
	int node, lines;
	const struct css_style *style;
	struct web_floats *fl;
};

static struct web_item *web_items;
static int web_item_count, web_item_room;
static struct web_piece *web_pieces;
static int web_piece_count, web_piece_room;
static int web_gap;			/* the room margins have left just above where we are */
static int web_baseline, web_line_count;	/* of the line finished last */
static int web_page_bg, web_page_h;

static int web_block_box(int node, int x, int w, int y, struct web_floats *fl, int fixed);
static void web_measure_box(int node, int *min, int *max);
static void web_inline(struct web_flow *f, int node);
static void web_block_child(struct web_flow *f, int node);

static int
web_most(int a, int b)
{
	return a > b ? a : b;
}

static int
web_least(int a, int b)
{
	return a < b ? a : b;
}

static struct web_item *
web_item_new(int kind, int node, int x, int y, int w, int h, int colour)
{
	struct web_item *it;

	web_items = web_grow(web_items, &web_item_room, web_item_count + 1, sizeof *web_items);
	it = &web_items[web_item_count++];
	memset(it, 0, sizeof *it);
	it->kind = kind;
	it->node = node;
	it->x = x, it->y = y, it->w = w, it->h = h;
	it->colour = colour;
	return it;
}

static void
web_shift(int first, int last, int dx, int dy)
{
	int i;

	if (dx || dy)
		for (i = first; i < last; i++)
			web_items[i].x += dx, web_items[i].y += dy;
}

/* Whether a child is laid out at all: a closed <details> shows only its summary. */
static int
web_shown(int parent, int kid)
{
	const struct web_node *p = &web_nodes[parent];
	const char *value;
	int n;

	if (web_nodes[kid].c.tag != T_TEXT && web_style_of(kid)->display == D_NONE)
		return 0;
	if (p->c.tag == T_details && !(p->flags & N_OPEN) && !web_attr(parent, "open", &value, &n))
		return web_nodes[kid].c.tag == T_summary;
	return 1;
}

static int
web_text_face(const struct css_style *s)
{
	if (!web_faces[s->face].tried)
		web_face(s->face);
	return s->face;
}

/* The width of characters of the page's text in a face (those no face has count for nothing). */
static int
web_text_width(int face, int from, int n)
{
	struct web_face *f = &web_faces[face];
	const unsigned char *plain = f->adv[0];
	const unsigned short *u = web_u + from;
	int w = 0, a;

	if (!plain)
		plain = web_advances(&web_faces[face], 0);
	for (; n-- > 0; u++) {
		/* (nearly every character of a page is one the face has, below 256) */
		if (*u < 256 && plain[*u] < WEB_ADVANCE_LATER)
			w += plain[*u] << f->twice;
		else if (*u < 256 && plain[*u] == WEB_ADVANCE_LATER && (a = web_advance_found(f, *u)) != 255)
			w += a << f->twice;
		else if (web_char_face(face, *u, &a) >= 0)
			w += a;
	}
	return w;
}

static inline int
web_cjk(unsigned c)
{
	return c >= 0x2e80 && (c <= 0x9fff || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef));
}

/* The end of the word that begins at `i` of a text node's characters. */
static int
web_word_end(int i, int end)
{
	const unsigned short *u = web_u;
	int j = i;

	if (web_cjk(u[i]))
		return i + 1;
	while (j < end && u[j] > ' ' && u[j] < 0x2e80)
		j++;
	while (j < end && u[j] != ' ' && u[j] != '\n' && !web_cjk(u[j]))
		j++;
	return j;
}

/* Text kept beside the page's own: a list's mark, a picture's description, capitals. */
static int
web_text_add(const unsigned short *text, int n)
{
	int at;

	web_u = web_grow(web_u, &web_u_room, web_u_n + n + 2, sizeof *web_u);
	web_u[web_u_n++] = 0;
	at = web_u_n;
	memcpy(web_u + at, text, n * sizeof *text);
	web_u_n += n;
	return at;
}

static int
web_text_add_utf8(const char *text)
{
	unsigned short u[256];
	int n = 0, used, left = (int)strlen(text);

	for (; left > 0 && n < 255; text += used, left -= used)
		u[n++] = web_char((const unsigned char *)text, left, &used);
	return web_text_add(u, n) | n << 24;
}

/* ---- measuring ---- */

/* The narrowest and the widest a text node can be laid. */
static void
web_measure_text(int node, int *min, int *max)
{
	struct web_node *e = &web_nodes[node];
	const struct css_style *s = web_style_of(node);
	int face, i, end, line = 0, widest = 0, word = 0;

	if (e->minw >= 0) {
		*min = e->minw;
		*max = e->maxw;
		return;
	}
	face = web_text_face(s);
	end = e->text + e->text_n;
	for (i = e->text; i < end; ) {
		int j, w, pre = s->white == W_PRE || s->white == W_PRE_WRAP;

		if (web_u[i] == '\n' && pre) {
			widest = web_most(widest, line);
			line = 0;
			i++;
			continue;
		}
		if (web_u[i] == ' ' || web_u[i] == '\n') {
			/* (one space for any run of them, unless it is a listing) */
			if (pre || i == e->text || (web_u[i - 1] != ' ' && web_u[i - 1] != '\n'))
				line += web_advance(face, ' ') > 0 ? web_advance(face, ' ') : 4;
			i++;
			continue;
		}
		j = web_word_end(i, end);
		w = web_text_width(face, i, j - i);
		line += w;
		word = web_most(word, w);
		i = j;
	}
	widest = web_most(widest, line);
	e->maxw = *max = widest;
	e->minw = *min = s->white == W_NOWRAP || s->white == W_PRE ? widest : web_least(word, 240);
}

/* What a style adds round its content, sideways: margins, borders, padding in pixels. */
static int
web_extras(const struct css_style *s)
{
	return css_px(s->m[1], 0, 0) + css_px(s->m[3], 0, 0) + s->border_w[1] + s->border_w[3] + css_px(s->p[1], 0, 0) + css_px(s->p[3], 0, 0);
}

static int
web_inside_extras(const struct css_style *s)
{
	return s->border_w[1] + s->border_w[3] + css_px(s->p[1], 0, 0) + css_px(s->p[3], 0, 0);
}

/* A picture's size on the page (0, 0: not known yet, or not shown), fitted to `room`. */
static int
web_image_size(int node, int room, int *w, int *h)
{
	const struct css_style *s = web_style_of(node);
	char src[600], whole[256];
	int nw = 0, nh = 0, handle = -1, aw, ah;

	if (web_attr_text(node, "src", src, sizeof src) && web_resolve(src, whole))
		handle = web_picture(node, whole, &nw, &nh);
	/* (a small picture is a sign, kept pixel for pixel; a large one is as much smaller as the text is) */
	if (nw > 32 || nh > 32)
		nw = web_scaled(nw), nh = web_scaled(nh);
	aw = s->width != CSS_UNSET && s->width != CSS_AUTO ? css_px(s->width, room, 0) : web_attr_len(node, "width", CSS_UNSET);
	ah = s->height != CSS_UNSET && s->height != CSS_AUTO && s->height < CSS_PCT / 2 ? s->height : web_attr_len(node, "height", CSS_UNSET);
	if (aw != CSS_UNSET && aw >= CSS_PCT / 2)
		aw = css_px(aw, room, 0);
	if (ah != CSS_UNSET && ah >= CSS_PCT / 2)
		ah = CSS_UNSET;
	if (aw == CSS_UNSET && ah == CSS_UNSET)
		aw = nw, ah = nh;
	else if (aw == CSS_UNSET)
		aw = nh ? ah * nw / nh : ah;
	else if (ah == CSS_UNSET)
		ah = nw ? aw * nh / nw : aw;
	if (s->maxw != CSS_UNSET && css_px(s->maxw, room, 0) > 0 && aw > css_px(s->maxw, room, 0)) {
		ah = ah * css_px(s->maxw, room, 0) / aw;
		aw = css_px(s->maxw, room, 0);
	}
	if (room > 0 && aw > room) {
		ah = ah * room / aw;
		aw = room;
	}
	*w = aw < 0 ? 0 : aw;
	*h = ah < 0 ? 0 : ah;
	return handle;
}

/* A picture that could not be had is drawn as a box with a sign and one line of text: its size
 * for that line, as a picture's natural size (web_image_size makes that 13/16), at most 300 wide. */
#define WEB_BROKEN_TEXT 20	/* where the text begins, after the sign */
static void
web_broken_size(const char *label, int *w, int *h)
{
	const unsigned char *at = (const unsigned char *)label;
	int face = web_face_for(0, 0, CSS_EM), left = (int)strlen(label), used, x = 0;

	web_face(face);
	for (; left > 0; at += used, left -= used)
		x += web_most(0, web_advance(face, web_char(at, left, &used)));
	x = web_least(x + WEB_BROKEN_TEXT + 6, 300);
	*w = (x * 16 + 12) / CSS_EM;
	*h = ((web_faces[face].height + 8) * 16 + 12) / CSS_EM;
}

/* An input's, a select's or a text area's size. */
static void
web_field_size(int node, int room, int *w, int *h)
{
	const struct css_style *s = web_style_of(node);
	char type[24] = "text", text[200];
	int t = web_nodes[node].c.tag, face = web_face_for(0, 0, CSS_EM), kid;

	web_face(face);
	*h = web_faces[face].height + 6;
	if (t == T_textarea) {
		*w = web_attr_int(node, "cols", 20) * 7;
		*h = web_attr_int(node, "rows", 2) * web_faces[face].height + 6;
	} else if (t == T_select) {
		*w = 40;
		for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next) {
			int text_kid = web_nodes[kid].first;

			if (web_nodes[kid].c.tag == T_option && text_kid >= 0 && web_nodes[text_kid].c.tag == T_TEXT)
				*w = web_most(*w, web_text_width(face, web_nodes[text_kid].text, web_nodes[text_kid].text_n) + 26);
		}
	} else {
		web_attr_text(node, "type", type, sizeof type);
		if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) {
			*w = *h = 13;
		} else if (!strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "reset") || !strcasecmp(type, "image")) {
			int at;

			if (!web_attr_text(node, "value", text, sizeof text) && !web_attr_text(node, "alt", text, sizeof text))
				strcpy(text, !strcasecmp(type, "reset") ? "Reset" : "Submit");
			at = web_text_add_utf8(text);
			*w = web_text_width(face, at & 0xffffff, at >> 24) + 16;
			web_u_n = (at & 0xffffff) - 1;
		} else if (!strcasecmp(type, "file") || !strcasecmp(type, "range") || !strcasecmp(type, "color")) {
			*w = 120;
		} else {
			*w = web_attr_int(node, "size", 20) * 7 + 8;
		}
	}
	if (s->width != CSS_UNSET && s->width != CSS_AUTO && t != T_select)
		*w = css_px(s->width, room, *w);
	if (s->maxw != CSS_UNSET && css_px(s->maxw, room, *w) < *w)
		*w = css_px(s->maxw, room, *w);
	if (room > 0 && *w > room)
		*w = room;
	if (*w < 8)
		*w = 8;
}

static int
web_is_field(int node)
{
	int t = web_nodes[node].c.tag;

	return t == T_input || t == T_select || t == T_textarea;
}

/* The content of an element as lines and boxes: its narrowest and widest. `line` is the width
 * of the line being filled when it is called for what is inside an inline element. */
static void
web_measure_kids(int node, int *line, int *min, int *max)
{
	int kid;

	for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next) {
		const struct css_style *s;
		int a = 0, b = 0, t = web_nodes[kid].c.tag;

		if (!web_shown(node, kid))
			continue;
		if (t == T_TEXT) {
			web_measure_text(kid, &a, &b);
			*line += b;
			*min = web_most(*min, a);
			continue;
		}
		s = web_style_of(kid);
		if (t == T_br) {
			*max = web_most(*max, *line);
			*line = 0;
		} else if (t == T_img) {
			web_image_size(kid, 0, &a, &b);
			*line += a;
			*min = web_most(*min, web_least(a, 120));
		} else if (web_is_field(kid)) {
			web_field_size(kid, 0, &a, &b);
			*line += a;
			*min = web_most(*min, web_least(a, 120));
		} else if (web_block_level(s->display) || s->floated) {
			*max = web_most(*max, *line);
			*line = 0;
			web_measure_box(kid, &a, &b);
			*min = web_most(*min, a);
			*max = web_most(*max, b);
		} else if (s->display == D_INLINE) {
			*line += web_extras(s);
			web_measure_kids(kid, line, min, max);
		} else {
			web_measure_box(kid, &a, &b);
			*line += b;
			*min = web_most(*min, a);
		}
	}
}

/* ---- tables ---- */

#define WEB_COLUMNS 48

struct web_cell { int node, col, span, min, max, first, last, bottom; };
struct web_table {
	struct web_cell *cells;
	int *row_first, *row_node;	/* each row's first cell, and its element (or -1) */
	int rows, cell_count, columns;
	int min[WEB_COLUMNS], max[WEB_COLUMNS], pct[WEB_COLUMNS], width[WEB_COLUMNS];
	int spacing;
};

static void
web_table_row(struct web_table *t, int row, int *room_cells, int *room_rows, int *left)
{
	int kid, col = 0, k;

	if (t->rows + 2 > *room_rows) {
		*room_rows = *room_rows * 2 + 16;
		t->row_first = realloc(t->row_first, *room_rows * sizeof(int));
		t->row_node = realloc(t->row_node, *room_rows * sizeof(int));
	}
	t->row_first[t->rows] = t->cell_count;
	t->row_node[t->rows++] = row;
	for (kid = web_nodes[row].first; kid >= 0; kid = web_nodes[kid].next) {
		struct web_cell *c;
		int span, down;

		if (web_nodes[kid].c.tag == T_TEXT || web_style_of(kid)->display == D_NONE)
			continue;
		/* (a column a cell from a row above still stands in is passed over) */
		while (col < WEB_COLUMNS && left[col] > 0)
			col++;
		if (col >= WEB_COLUMNS)
			break;
		span = web_attr_int(kid, "colspan", 1);
		span = span < 1 ? 1 : span > WEB_COLUMNS - col ? WEB_COLUMNS - col : span;
		down = web_attr_int(kid, "rowspan", 1);
		if (t->cell_count + 1 > *room_cells) {
			*room_cells = *room_cells * 2 + 32;
			t->cells = realloc(t->cells, *room_cells * sizeof *t->cells);
		}
		c = &t->cells[t->cell_count++];
		c->node = kid;
		c->col = col;
		c->span = span;
		for (k = col; k < col + span; k++)
			left[k] = down > 1 ? web_least(down, 1000) : 1;
		col += span;
		t->columns = web_most(t->columns, col);
	}
	for (k = 0; k < WEB_COLUMNS; k++)
		if (left[k] > 0)
			left[k]--;
	t->row_first[t->rows] = t->cell_count;
}

/* A table's rows and cells, and each column's narrowest and widest. The caller frees it. */
static void
web_table_read(int node, struct web_table *t)
{
	int room_cells = 0, room_rows = 0, left[WEB_COLUMNS], kid, row, i, k;
	const char *value;
	int n;

	memset(t, 0, sizeof *t);
	memset(left, 0, sizeof left);
	t->spacing = web_attr(node, "cellspacing", &value, &n) ? web_scaled(web_attr_int(node, "cellspacing", 0)) : 0;
	for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next) {
		const struct css_style *s;

		if (web_nodes[kid].c.tag == T_TEXT || (s = web_style_of(kid))->display == D_NONE || s->display == D_CAPTION)
			continue;
		if (s->display == D_ROW || web_nodes[kid].c.tag == T_tr) {
			web_table_row(t, kid, &room_cells, &room_rows, left);
		} else if (s->display == D_ROW_GROUP) {
			for (row = web_nodes[kid].first; row >= 0; row = web_nodes[row].next)
				if (web_nodes[row].c.tag != T_TEXT && web_style_of(row)->display != D_NONE)
					web_table_row(t, row, &room_cells, &room_rows, left);
		} else {
			/* cells with no row of their own are one row: this element's parent stands for it */
			web_table_row(t, node, &room_cells, &room_rows, left);
			break;
		}
	}
	for (k = 0; k < WEB_COLUMNS; k++)
		t->pct[k] = -1;
	for (i = 0; i < t->cell_count; i++) {
		struct web_cell *c = &t->cells[i];
		const struct css_style *s = web_style_of(c->node);

		web_measure_box(c->node, &c->min, &c->max);
		if (c->span == 1) {
			t->min[c->col] = web_most(t->min[c->col], c->min);
			t->max[c->col] = web_most(t->max[c->col], c->max);
			if (s->width != CSS_UNSET && s->width >= CSS_PCT / 2)
				t->pct[c->col] = web_most(t->pct[c->col], s->width - CSS_PCT);
		}
	}
	/* a cell over several columns asks them for what they lack between them */
	for (i = 0; i < t->cell_count; i++) {
		struct web_cell *c = &t->cells[i];
		int have_min = 0, have_max = 0;

		if (c->span == 1)
			continue;
		for (k = c->col; k < c->col + c->span; k++)
			have_min += t->min[k], have_max += t->max[k];
		have_min += t->spacing * (c->span - 1);
		have_max += t->spacing * (c->span - 1);
		for (k = c->col; k < c->col + c->span; k++) {
			if (c->min > have_min)
				t->min[k] += (c->min - have_min + c->span - 1) / c->span;
			if (c->max > have_max)
				t->max[k] += (c->max - have_max + c->span - 1) / c->span;
		}
	}
	for (k = 0; k < t->columns; k++)
		t->max[k] = web_most(t->max[k], t->min[k]);
}

static void
web_table_free(struct web_table *t)
{
	free(t->cells);
	free(t->row_first);
	free(t->row_node);
}

/* The widths of a table's columns for `target` pixels of them together (0: as they like, up
 * to `most`). Returns what they come to. */
static int
web_table_widths(struct web_table *t, int target, int most)
{
	int sum_min = 0, sum_max = 0, k, total = 0;

	for (k = 0; k < t->columns; k++) {
		if (t->pct[k] >= 0 && (target || most)) {
			int w = (target ? target : most) * t->pct[k] / 1000;

			t->max[k] = web_most(t->min[k], w);
		}
		sum_min += t->min[k];
		sum_max += t->max[k];
	}
	if (!target)
		target = web_least(sum_max, most);
	for (k = 0; k < t->columns; k++) {
		if (target >= sum_max)
			t->width[k] = sum_max ? t->max[k] + (int)((long long)(target - sum_max) * t->max[k] / sum_max) : target / t->columns;
		else if (target <= sum_min || sum_max == sum_min)
			t->width[k] = sum_min ? (int)((long long)target * t->min[k] / sum_min) : 0;
		else
			t->width[k] = t->min[k] + (int)((long long)(t->max[k] - t->min[k]) * (target - sum_min) / (sum_max - sum_min));
		total += t->width[k];
	}
	return total;
}

/* The widest and narrowest an element's box is, with what is round its content. */
static void
web_measure_box(int node, int *min, int *max)
{
	struct web_node *e = &web_nodes[node];
	const struct css_style *s = web_style_of(node);
	int extras = web_extras(s), line = 0, a = 0, b = 0, kid;

	if (e->c.tag == T_TEXT) {
		web_measure_text(node, min, max);
		return;
	}
	if (e->minw < 0) {
		if (e->c.tag == T_img) {
			web_image_size(node, 0, &b, &a);
			a = web_least(b, 120);
		} else if (web_is_field(node)) {
			web_field_size(node, 0, &b, &a);
			a = web_least(b, 120);
		} else if (s->display == D_TABLE) {
			struct web_table t;
			int k;

			web_table_read(node, &t);
			for (k = 0; k < t.columns; k++)
				a += t.min[k], b += t.max[k];
			a += t.spacing * (t.columns + 1);
			b += t.spacing * (t.columns + 1);
			web_table_free(&t);
		} else if ((s->display == D_FLEX || s->display == D_INLINE_FLEX) && !s->column) {
			int gap = s->gap, count = 0;

			for (kid = e->first; kid >= 0; kid = web_nodes[kid].next) {
				int ka, kb;

				if (!web_shown(node, kid) || (web_nodes[kid].flags & N_SPACE))
					continue;
				web_measure_box(kid, &ka, &kb);
				a = web_most(a, ka);
				b += kb + (count++ ? gap : 0);
			}
		} else {
			web_measure_kids(node, &line, &a, &b);
			b = web_most(b, line);
		}
		if (s->white == W_NOWRAP || s->white == W_PRE)
			a = b;
		e->minw = a;
		e->maxw = b;
	}
	a = e->minw;
	b = e->maxw;
	if (s->width != CSS_UNSET && s->width != CSS_AUTO && s->width < CSS_PCT / 2 && e->c.tag != T_table) {
		a = b = web_most(0, s->width - (s->border_box ? web_inside_extras(s) : 0));
	}
	if (s->maxw != CSS_UNSET && s->maxw < CSS_PCT / 2) {
		int most = web_most(0, s->maxw - (s->border_box ? web_inside_extras(s) : 0));

		a = web_least(a, most);
		b = web_least(b, most);
	}
	if (s->minw > 0 && s->minw < CSS_PCT / 2) {
		a = web_most(a, s->minw);
		b = web_most(b, s->minw);
	}
	*min = a + extras;
	*max = b + extras;
}

/* ---- lines ---- */

static int
web_floats_bottom(const struct web_floats *fl, int side)
{
	int i, y = -(1 << 30);

	for (i = 0; fl && i < fl->n; i++)
		if (!side || (side & (fl->f[i].right ? 2 : 1)))
			y = web_most(y, fl->f[i].y1);
	return y;
}

/* A line's edges at the flow's y: the box's, less the floats that stand beside it there. */
static void
web_line_edges(struct web_flow *f)
{
	int i;

	f->left = f->x0;
	f->right = f->x1;
	for (i = 0; f->fl && i < f->fl->n; i++) {
		if (f->y < f->fl->f[i].y0 || f->y >= f->fl->f[i].y1)
			continue;
		if (f->fl->f[i].right)
			f->right = web_least(f->right, f->fl->f[i].x0);
		else
			f->left = web_most(f->left, f->fl->f[i].x1);
	}
	if (f->right < f->left)
		f->right = f->left;
}

/* Begins a line that something `w` wide is to stand in: below the floats that leave too little. */
static void
web_line_begin(struct web_flow *f, int w)
{
	int i, guard = 0;

	for (;;) {
		int next = 1 << 30;

		web_line_edges(f);
		if (w <= f->right - f->left || (f->left == f->x0 && f->right == f->x1) || guard++ > 40)
			break;
		for (i = 0; i < f->fl->n; i++)
			if (f->fl->f[i].y1 > f->y)
				next = web_least(next, f->fl->f[i].y1);
		if (next == 1 << 30)
			break;
		f->y = next;
	}
	f->x = f->left;
	f->piece0 = web_piece_count;
	f->space = 0;
}

static void
web_piece_add(struct web_flow *f, int first, int last, int asc, int h)
{
	struct web_piece *p;

	web_pieces = web_grow(web_pieces, &web_piece_room, web_piece_count + 1, sizeof *web_pieces);
	p = &web_pieces[web_piece_count++];
	p->first = first;
	p->last = last;
	p->asc = asc;
	p->h = h;
	f->any = 1;
}

static int
web_line_height(const struct css_style *s, int content)
{
	int lh = s->line_h > 0 ? s->line_h : s->line_h < 0 ? -s->line_h * s->font_size / 256 : 0;

	return lh > content ? lh : content;
}

/* Ends the line being filled: its pieces come to stand on one baseline, and it is moved over
 * if its box centres or right-aligns. */
static void
web_line_break(struct web_flow *f)
{
	int asc = 0, desc = 0, i, lh, base, dx = 0;

	if (!f->any)
		return;
	for (i = f->piece0; i < web_piece_count; i++) {
		asc = web_most(asc, web_pieces[i].asc);
		desc = web_most(desc, web_pieces[i].h - web_pieces[i].asc);
	}
	lh = web_line_height(f->style, asc + desc);
	base = f->y + (lh - asc - desc) / 2 + asc;
	if (f->style->align && f->x < f->right)
		dx = (f->right - f->x) / (f->style->align == 1 ? 2 : 1);
	for (i = f->piece0; i < web_piece_count; i++)
		web_shift(web_pieces[i].first, web_pieces[i].last, dx, base - web_pieces[i].asc - f->y);
	web_piece_count = f->piece0;
	web_baseline = base;
	web_line_count++;
	f->y += lh;
	f->any = 0;
	f->space = 0;
	f->lines++;
	web_gap = 0;
}

/* Characters of the page's text in a line, in one style: joined to the item before when they
 * go on from it, in one face and one colour. */
static void
web_put_text(struct web_flow *f, int node, const struct css_style *s, int face, int from, int n, int w, int colour)
{
	struct web_item *last = web_item_count ? &web_items[web_item_count - 1] : NULL;
	struct web_face *fc = &web_faces[face];
	int flags = (s->decor & DECOR_UNDER ? IF_UNDER : 0) | (s->decor & DECOR_THROUGH ? IF_THROUGH : 0) | (s->italic ? IF_ITALIC : 0);
	int x = f->x + f->space, joined = f->space > 0;
	struct web_item *it;

	if (f->any && last && last->kind == I_TEXT && last->node == node && last->face == face && last->flags == flags &&
	    last->colour == colour && last->x + last->w + f->space == x && web_piece_count > f->piece0 &&
	    web_pieces[web_piece_count - 1].last == web_item_count && last->a + last->b + joined == from &&
	    (!joined || web_u[from - 1] == ' ' || web_u[from - 1] == '\n')) {
		last->b += n + joined;
		last->w = x + w - last->x;
	} else {
		int rise = s->valign == 5 ? s->font_size / 3 : s->valign == 4 ? -(s->font_size / 4) : 0;

		it = web_item_new(I_TEXT, node, x, f->y, w, fc->height, colour);
		it->a = from;
		it->b = n;
		it->face = face;
		it->flags = flags;
		web_piece_add(f, web_item_count - 1, web_item_count, fc->ascent + rise, fc->height + (rise > 0 ? rise : 0));
	}
	f->x = x + w;
	f->space = 0;
	f->any = 1;
}

/* A word in a line: on the next one if it does not fit, broken where it must be if no line
 * holds it. Its characters that the style's own face lacks are in the face of every script. */
static void
web_put_word(struct web_flow *f, int node, const struct css_style *s, int face, int from, int n, int colour)
{
	/* (text that is not to be broken is broken all the same where its box ends: nothing is cut off here) */
	int i = from, end = from + n, wrap = s->white != W_PRE;
	const unsigned char *plain = web_faces[face].adv[0];

	if (plain && !web_faces[face].twice) {
		/* a word the face has every character of, that fits: put whole, with one walk over it */
		int w = 0;

		for (i = from; i < end && web_u[i] < 256; i++) {
			int a = plain[web_u[i]];

			if (a == WEB_ADVANCE_LATER)
				a = web_advance_found(&web_faces[face], web_u[i]);
			if (a == 255)
				break;
			w += a;
		}
		if (i == end) {
			if (!f->any)
				web_line_begin(f, 0);
			if (wrap && f->any && f->x + f->space + w > f->right) {
				web_line_break(f);
				web_line_begin(f, web_least(w, f->x1 - f->x0));
			}
			if (!wrap || f->x + f->space + w <= f->right) {
				web_put_text(f, node, s, face, from, n, w, colour);
				return;
			}
		}
		i = from;
	}
	while (i < end) {
		int seg_face = -2, w = 0, j, a, fits = end;

		/* a run of characters of one face, as much as the line takes */
		if (!f->any)
			web_line_begin(f, 0);
		for (j = i; j < end; j++) {
			int cf = web_char_face(face, web_u[j], &a);

			if (cf < 0) {
				if (j == i) {
					i++;
					j = i - 1;
				} else
					break;
				continue;
			}
			if (seg_face == -2)
				seg_face = cf;
			else if (cf != seg_face)
				break;
			if (wrap && f->x + f->space + w + a > f->right && fits == end)
				fits = j;
			w += a;
		}
		if (seg_face == -2)
			break;
		if (wrap && f->x + f->space + w > f->right) {
			if (f->any) {
				/* the whole word on a line of its own, if one holds it */
				int whole = web_text_width(face, from, n);

				web_line_break(f);
				web_line_begin(f, web_least(whole, f->x1 - f->x0));
				if (i != from && whole <= f->right - f->left) {
					/* (its first part is on the line above already: it stays broken there) */
				}
				continue;
			}
			if (fits > i && fits < j) {
				/* no line holds it: as much as this one does */
				web_put_text(f, node, s, seg_face, i, fits - i, web_text_width(seg_face, i, fits - i), colour);
				web_line_break(f);
				i = fits;
				continue;
			}
		}
		web_put_text(f, node, s, seg_face, i, j - i, w, colour);
		i = j;
	}
}

static void
web_text_node(struct web_flow *f, int node)
{
	const struct web_node *e = &web_nodes[node];
	const struct css_style *s = web_style_of(node);
	int face = web_text_face(s), i = e->text, end = e->text + e->text_n, space, raw = s->white == W_PRE || s->white == W_PRE_WRAP;

	if (!web_faces[face].file && !web_plain)
		return;
	space = web_advance(face, ' ');
	space = space > 0 ? space : 4;
	while (i < end) {
		int j;

		if (web_u[i] == '\n' && raw) {
			if (f->any)
				web_line_break(f);
			else {
				f->y += web_line_height(f->style, web_faces[face].height);
				web_gap = 0;
			}
			i++;
			continue;
		}
		if (web_u[i] == ' ' || web_u[i] == '\n') {
			if (raw) {
				/* a line's spaces as they are: they move what follows */
				if (!f->any)
					web_line_begin(f, 0);
				f->space += space;
			} else if (f->any)
				f->space = web_most(f->space, space);
			i++;
			continue;
		}
		j = web_word_end(i, end);
		if (s->transform && j - i < 200) {
			unsigned short changed[201];
			int k;

			changed[0] = ' ';
			for (k = 0; k < j - i; k++) {
				unsigned c = web_u[i + k];

				changed[k + 1] = c >= 128 ? c : s->transform == 1 || (s->transform == 3 && !k) ? toupper(c) : s->transform == 2 ? tolower(c) : c;
			}
			web_put_word(f, node, s, face, web_text_add(changed, j - i + 1) + 1, j - i, s->colour);
		} else {
			web_put_word(f, node, s, face, i, j - i, s->colour);
		}
		i = j;
	}
}

/* Something that stands in a line whole: laid out where the line is now, and moved to the
 * next line if it did not fit. */
static void
web_put_atom(struct web_flow *f, int first, int w, int h, int asc, int px, int py)
{
	if (f->any && px + w > f->right) {
		web_line_break(f);
		web_line_begin(f, w);
	}
	web_shift(first, web_item_count, f->x + f->space - px, f->y - py);
	web_piece_add(f, first, web_item_count, asc, h);
	f->x += f->space + w;
	f->space = 0;
}

static void
web_put_image(struct web_flow *f, int node)
{
	const struct css_style *s = web_style_of(node);
	char alt[256];
	int w, h, handle;

	handle = web_image_size(node, f->x1 - f->x0, &w, &h);
	if (handle < 0 || !w || !h) {
		/* not here (yet): what the page says the picture is */
		if (web_attr_text(node, "alt", alt, sizeof alt) && alt[0]) {
			int at = web_text_add_utf8(alt), from = at & 0xffffff, end = from + (at >> 24), i, j;
			int face = web_text_face(s);

			for (i = from; i < end; i = j) {
				if (web_u[i] == ' ') {
					f->space = f->any ? web_most(f->space, 4) : 0;
					j = i + 1;
					continue;
				}
				j = web_word_end(i, end);
				web_put_word(f, node, s, face, i, j - i, 0x707070);
			}
		}
		return;
	}
	if (w <= 2 && h <= 2)
		return;		/* (a pixel that counts visitors) */
	if (!f->any)
		web_line_begin(f, w);
	web_item_new(I_IMAGE, node, f->x + f->space, f->y, w, h, 0)->a = handle;
	web_put_atom(f, web_item_count - 1, w, h, h, f->x + f->space, f->y);
}

/* An element that is a box inside a line (inline-block, a button): as wide as its content. */
static void
web_put_box(struct web_flow *f, int node)
{
	const struct css_style *s = web_style_of(node);
	int min, max, w, first = web_item_count, px, py, bottom, lines = web_line_count, gap = web_gap, mt, mb, room = f->x1 - f->x0;

	web_measure_box(node, &min, &max);
	w = s->width != CSS_UNSET && s->width != CSS_AUTO ? css_px(s->width, room, 0) + (s->border_box ? css_px(s->m[1], room, 0) + css_px(s->m[3], room, 0) : web_extras(s)) : max;
	w = web_least(w, room);
	if (!f->any)
		web_line_begin(f, w);
	px = f->x + f->space;
	py = f->y;
	mt = web_most(0, css_px(s->m[0], room, 0));
	mb = web_most(0, css_px(s->m[2], room, 0));
	web_gap = 0;
	bottom = web_block_box(node, px, w, py + mt, NULL, 1) + mb;
	web_gap = gap;
	/* it stands on the baseline of its own last line, or on its bottom if it has none */
	web_put_atom(f, first, w, bottom - py, web_line_count != lines ? web_baseline - py : bottom - py, px, py);
}

static void
web_put_field(struct web_flow *f, int node)
{
	int w, h;

	web_field_size(node, f->x1 - f->x0, &w, &h);
	if (!f->any)
		web_line_begin(f, w);
	web_item_new(I_FIELD, node, f->x + f->space, f->y, w, h, 0);
	web_put_atom(f, web_item_count - 1, w + 2, h + 2, h - 4, f->x + f->space, f->y);
}

/* Something of a line: text, a picture, an element with more of the same inside. */
static void
web_inline(struct web_flow *f, int node)
{
	const struct web_node *e = &web_nodes[node];
	const struct css_style *s;
	const char *value;
	int kid, n, t = e->c.tag;

	if (t == T_TEXT) {
		web_text_node(f, node);
		return;
	}
	s = web_style_of(node);
	if (s->display == D_NONE)
		return;
	if (t == T_br) {
		if (f->any)
			web_line_break(f);
		else {
			f->y += web_line_height(f->style, web_faces[web_text_face(s)].height);
			web_gap = 0;
		}
		if (web_attr(node, "clear", &value, &n))
			f->y = web_most(f->y, web_floats_bottom(f->fl, 0));
		return;
	}
	if (t == T_wbr)
		return;
	if (t == T_img) {
		web_put_image(f, node);
		return;
	}
	if (web_is_field(node)) {
		web_put_field(f, node);
		return;
	}
	if (web_block_level(s->display) || s->floated) {
		web_block_child(f, node);
		return;
	}
	if (s->display != D_INLINE) {
		web_put_box(f, node);
		return;
	}
	{
		int lead = web_most(0, css_px(s->m[3], 0, 0)) + s->border_w[3] + css_px(s->p[3], 0, 0);
		int trail = web_most(0, css_px(s->m[1], 0, 0)) + s->border_w[1] + css_px(s->p[1], 0, 0);
		int box = -1, lines = f->lines, start = 0, face;

		if (s->white == W_NOWRAP && f->style->white != W_NOWRAP && f->any) {
			/* it is not broken inside: on the next line whole, if this one has not the room */
			int min, max;

			web_measure_box(node, &min, &max);
			if (f->x + f->space + max > f->right && max <= f->x1 - f->x0)
				web_line_break(f);
		}
		if (lead) {
			if (!f->any)
				web_line_begin(f, 0);
			f->space += lead;
		}
		if (s->bg >= 0 || s->border_w[0] || s->border_w[2]) {
			/* its background: behind what of it stands in this line, if it ends in it */
			face = web_text_face(s);
			if (!f->any)
				web_line_begin(f, 0);
			start = f->x + f->space - css_px(s->p[3], 0, 0);
			box = web_item_count;
			web_item_new(I_BOX, node, start, f->y, 0, web_faces[face].height, s->bg >= 0 ? s->bg : CSS_CLEAR);
			web_piece_add(f, box, box + 1, web_faces[face].ascent, web_faces[face].height);
			f->any = f->x > f->left;
		}
		for (kid = e->first; kid >= 0; kid = web_nodes[kid].next)
			if (web_shown(node, kid))
				web_inline(f, kid);
		if (box >= 0 && lines == f->lines && f->x > start) {
			web_items[box].w = f->x - start + css_px(s->p[1], 0, 0);
			if (s->border_w[0] || s->border_w[2])
				web_items[box].b = s->border_c[0] == CSS_CURRENT ? s->colour : s->border_c[0], web_items[box].flags = 1;
			f->any = 1;
		}
		if (trail && f->any)
			f->x += trail;
	}
}

/* ---- boxes ---- */

/* A float: a box at the left or right edge that the lines after it go round. */
static void
web_float_box(struct web_flow *f, int node)
{
	const struct css_style *s = web_style_of(node);
	struct web_floats *fl = f->fl;
	int min, max, w, room = f->x1 - f->x0, gap = web_gap, y, x, bottom, i, guard = 0, mt, mb, iw = 0, ih = 0, handle = -1;

	if (f->any)
		web_line_break(f);
	if (web_nodes[node].c.tag == T_img) {
		handle = web_image_size(node, room, &iw, &ih);
		if (handle < 0 || !iw || !ih)
			return;
		w = iw + web_extras(s);
	} else {
		web_measure_box(node, &min, &max);
		w = s->width != CSS_UNSET && s->width != CSS_AUTO ? css_px(s->width, room, 0) + (s->border_box ? css_px(s->m[1], room, 0) + css_px(s->m[3], room, 0) : web_extras(s)) : max;
	}
	w = web_least(w, room);
	y = f->y;
	if (s->clear)
		y = web_most(y, web_floats_bottom(fl, s->clear));
	for (;;) {
		int left = f->x0, right = f->x1, next = 1 << 30;

		for (i = 0; fl && i < fl->n; i++) {
			if (y < fl->f[i].y0 || y >= fl->f[i].y1)
				continue;
			if (fl->f[i].right)
				right = web_least(right, fl->f[i].x0);
			else
				left = web_most(left, fl->f[i].x1);
			next = web_least(next, fl->f[i].y1);
		}
		x = s->floated == 2 ? right - w : left;
		if (w <= right - left || next == 1 << 30 || guard++ > 40) {
			if (x < f->x0)
				x = f->x0;
			break;
		}
		y = next;
	}
	mt = web_most(0, css_px(s->m[0], room, 0));
	mb = web_most(0, css_px(s->m[2], room, 0));
	web_gap = 0;
	if (handle >= 0) {
		web_item_new(I_IMAGE, node, x + web_most(0, css_px(s->m[3], room, 0)), y + mt, iw, ih, 0)->a = handle;
		bottom = y + mt + ih + mb;
	} else {
		bottom = web_block_box(node, x, w, y + mt, NULL, 1) + mb;
	}
	web_gap = gap;
	if (fl && fl->n < 32) {
		fl->f[fl->n].x0 = x;
		fl->f[fl->n].x1 = x + w;
		fl->f[fl->n].y0 = y;
		fl->f[fl->n].y1 = bottom;
		fl->f[fl->n++].right = s->floated == 2;
	} else {
		f->y = bottom;	/* (no room to remember it: what follows goes under it) */
	}
}

/* A box among the lines: the line being filled ends, and the box goes under it. */
static void
web_block_child(struct web_flow *f, int node)
{
	const struct css_style *s = web_style_of(node);
	int room = f->x1 - f->x0, mt, mb, x = f->x0, w = room;

	if (s->floated) {
		web_float_box(f, node);
		return;
	}
	web_line_break(f);
	if (s->clear)
		f->y = web_most(f->y, web_floats_bottom(f->fl, s->clear));
	mt = web_most(0, css_px(s->m[0], room, 0));
	mb = web_most(0, css_px(s->m[2], room, 0));
	if (mt > web_gap) {
		f->y += mt - web_gap;
		web_gap = mt;
	}
	if ((s->display == D_TABLE || s->display == D_FLEX || s->display == D_GRID) && f->fl && f->fl->n) {
		/* a box that keeps its inside to itself stands beside the floats, not under them */
		int min, max;

		web_measure_box(node, &min, &max);
		web_line_begin(f, web_least(min, room));
		x = f->left;
		w = f->right - f->left;
	}
	f->y = web_block_box(node, x, w, f->y, f->fl, 0);
	if (mb > web_gap) {
		f->y += mb - web_gap;
		web_gap = mb;
	}
}

/* An element's children as lines and boxes in a column `w` wide from x, y. Returns where they end. */
static int
web_flow_kids(int node, int x, int w, int y, struct web_floats *fl)
{
	struct web_flow f;
	int kid;

	memset(&f, 0, sizeof f);
	f.x0 = x;
	f.x1 = x + w;
	f.y = y;
	f.node = node;
	f.style = web_style_of(node);
	f.fl = fl;
	f.piece0 = web_piece_count;
	for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next) {
		if (!web_shown(node, kid))
			continue;
		if (web_nodes[kid].c.tag != T_TEXT && (web_block_level(web_style_of(kid)->display) || web_style_of(kid)->floated))
			web_block_child(&f, kid);
		else
			web_inline(&f, kid);
	}
	web_line_break(&f);
	return f.y;
}

/* One child of a row or a grid as a box of its own, `w` wide with its margins. Returns its bottom. */
static int
web_item_box(int parent, int kid, int x, int w, int y)
{
	const struct css_style *s;
	int gap = web_gap, bottom;

	web_gap = 0;
	if (web_nodes[kid].c.tag == T_TEXT) {
		struct web_flow f;

		memset(&f, 0, sizeof f);
		f.x0 = x;
		f.x1 = x + w;
		f.y = y;
		f.node = parent;
		f.style = web_style_of(parent);
		f.piece0 = web_piece_count;
		web_text_node(&f, kid);
		web_line_break(&f);
		bottom = f.y;
	} else if (web_nodes[kid].c.tag == T_img || web_is_field(kid)) {
		struct web_flow f;

		memset(&f, 0, sizeof f);
		f.x0 = x;
		f.x1 = x + w;
		f.y = y;
		f.node = parent;
		f.style = web_style_of(parent);
		f.piece0 = web_piece_count;
		web_inline(&f, kid);
		web_line_break(&f);
		bottom = f.y;
	} else {
		s = web_style_of(kid);
		bottom = web_block_box(kid, x, w, y + web_most(0, css_px(s->m[0], w, 0)), NULL, 1) + web_most(0, css_px(s->m[2], w, 0));
	}
	web_gap = gap;
	return bottom;
}

struct web_part { int node, base, min, w, grow, first, last, bottom; };

/* A box of a row that is lower than the row is made as high: its background and the lines at
 * its sides grow by `extra`, and the line under it moves down. Its things are items first to last. */
static void
web_stretch(int node, int first, int last, int top, int bottom, int extra)
{
	int i;

	if (extra <= 0 || web_nodes[node].c.tag == T_TEXT)
		return;
	bottom -= web_most(0, css_px(web_style_of(node)->m[2], 0, 0));
	for (i = first; i < last; i++) {
		struct web_item *it = &web_items[i];

		if (it->kind != I_BOX || it->node != node)
			continue;
		if (it->y + it->h == bottom && it->h > 6)
			it->h += extra;
		else if (it->y + it->h == bottom)
			it->y += extra;
	}
	(void)top;
}

/* A row of boxes side by side (display: flex), on more rows when one does not hold them. */
static int
web_flex_row(int node, int x, int w, int y)
{
	const struct css_style *s = web_style_of(node);
	struct web_part *parts = NULL;
	int count = 0, room = 0, kid, i, from, gap = s->gap;

	for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next) {
		const struct css_style *ks;
		struct web_part *p;
		int min, max;

		if (!web_shown(node, kid) || (web_nodes[kid].flags & N_SPACE))
			continue;
		if (count + 1 > room) {
			room = room * 2 + 16;
			parts = realloc(parts, room * sizeof *parts);
		}
		p = &parts[count++];
		p->node = kid;
		web_measure_box(kid, &min, &max);
		p->min = min;
		p->base = max;
		p->grow = 0;
		if (web_nodes[kid].c.tag != T_TEXT) {
			ks = web_style_of(kid);
			p->grow = ks->grow;
			if (ks->basis != CSS_AUTO && ks->basis != CSS_UNSET)
				p->base = css_px(ks->basis, w, 0) + web_extras(ks);
			else if (ks->width != CSS_UNSET && ks->width != CSS_AUTO)
				p->base = css_px(ks->width, w, 0) + (ks->border_box ? css_px(ks->m[1], w, 0) + css_px(ks->m[3], w, 0) : web_extras(ks));
			if (ks->grow && p->base > max && ks->basis == 0)
				p->base = 0;
		}
		if (p->base < p->min && !p->grow)
			p->base = web_least(p->min, w);
	}
	for (from = 0; from < count; ) {
		int to = from, sum = 0, sum_min = 0, grow = 0, free, at, height = 0, k = 0;

		/* the boxes of this row: all of them, or as many as fit when the row may wrap or
		 * even their narrowest together are too wide */
		for (i = from; i < count; i++)
			sum_min += web_least(parts[i].min, w) + (i > from ? gap : 0);
		while (to < count) {
			int add = (s->wrap ? web_most(parts[to].base, web_least(parts[to].min, w)) : web_least(parts[to].min, w)) + (to > from ? gap : 0);

			if (to > from && (s->wrap || sum_min > w) && sum + add > w)
				break;
			sum += add;
			to++;
		}
		sum = sum_min = 0;
		for (i = from; i < to; i++) {
			sum += parts[i].base + (i > from ? gap : 0);
			sum_min += web_least(parts[i].min, parts[i].base) + (i > from ? gap : 0);
			grow += parts[i].grow;
		}
		free = w - sum;
		for (i = from; i < to; i++) {
			struct web_part *p = &parts[i];
			int least = web_least(p->min, p->base);

			if (free >= 0)
				p->w = p->base + (grow ? (int)((long long)free * p->grow / grow) : 0);
			else if (sum > sum_min && w > sum_min)
				p->w = least + (int)((long long)(p->base - least) * (w - sum_min) / (sum - sum_min));
			else
				p->w = web_least(least, w);
		}
		if (free < 0 || grow)
			free = 0;
		at = x + (s->justify == 1 ? free / 2 : s->justify == 2 ? free : s->justify == 4 ? free / (2 * (to - from)) : 0);
		for (i = from; i < to; i++, k++) {
			struct web_part *p = &parts[i];

			p->first = web_item_count;
			p->bottom = web_item_box(node, p->node, at, p->w, y);
			p->last = web_item_count;
			height = web_most(height, p->bottom - y);
			at += p->w + gap + (s->justify == 3 && to - from > 1 ? free / (to - from - 1) : s->justify == 4 ? free / (to - from) : 0);
		}
		for (i = from; i < to; i++) {
			if (s->items)
				web_shift(parts[i].first, parts[i].last, 0, (height - (parts[i].bottom - y)) / (s->items == 1 ? 2 : 1));
			else
				web_stretch(parts[i].node, parts[i].first, parts[i].last, y, parts[i].bottom, height - (parts[i].bottom - y));
		}
		y += height + (to < count ? s->row_gap : 0);
		from = to;
	}
	free(parts);
	return y;
}

/* The columns a grid's text asks for, for `w` pixels: their widths into `width`. */
static int
web_grid_columns(const char *text, int w, int gap, int *width, int room)
{
	int fr[32], count = 0, fixed = 0, frs = 0, i, repeat = 1, unit, value;
	const char *p = text;

	if (room > 32)
		room = 32;
	while (*p && count < room) {
		int this_fr = 0, this_px = -1;

		while (isspace((unsigned char)*p) || *p == ')')
			p++, repeat = *p == ')' ? repeat : repeat;
		if (!*p)
			break;
		if (!strncasecmp(p, "repeat(", 7)) {
			p += 7;
			while (isspace((unsigned char)*p))
				p++;
			if (isdigit((unsigned char)*p)) {
				repeat = atoi(p);
			} else {
				/* as many as fit: of the least width its minmax() names */
				const char *least = strstr(p, "minmax(");
				int px = 160;

				if (least) {
					least += 7;
					if (css_length(&least, &unit, &value) && unit == U_PX)
						px = value / 1000;
				}
				repeat = web_most(1, (w + gap) / (web_most(px, 40) + gap));
			}
			while (*p && *p != ',')
				p++;
			if (*p)
				p++;
			continue;
		}
		if (!strncasecmp(p, "minmax(", 7)) {
			const char *comma = strchr(p, ',');

			p = comma ? comma + 1 : p + 7;
			continue;
		}
		if (*p == '[') {	/* a line's name */
			while (*p && *p != ']')
				p++;
			p += *p == ']';
			continue;
		}
		if (css_number(&p, &value) && !strncasecmp(p, "fr", 2)) {
			this_fr = web_most(1, value / 10);
			p += 2;
		} else {
			const char *q = p;

			if (css_length(&q, &unit, &value) && unit != U_AUTO) {
				this_px = unit == U_PERCENT ? w * (value / 100) / 1000 : unit == U_PX ? value / 1000 : value * CSS_EM / 1000;
				p = q;
			} else {
				this_fr = 100;	/* auto, min-content and the like */
				while (*p && !isspace((unsigned char)*p) && *p != ')')
					p++;
			}
		}
		for (i = 0; i < repeat && count < room; i++) {
			fr[count] = this_fr;
			width[count++] = this_px < 0 ? 0 : this_px;
			fixed += this_px < 0 ? 0 : this_px;
			frs += this_fr;
		}
		repeat = 1;
	}
	if (!count)
		return 0;
	fixed += gap * (count - 1);
	for (i = 0; i < count; i++)
		if (fr[i])
			width[i] = frs && w > fixed ? (int)((long long)(w - fixed) * fr[i] / frs) : 0;
	/* columns too narrow to read in are no columns: one under the other then */
	for (i = 0; i < count; i++)
		if (width[i] < 40)
			return 0;
	return count;
}

static int
web_grid(int node, int x, int w, int y)
{
	const struct css_style *s = web_style_of(node);
	int width[32], columns, kid, col = 0, at = x, height = 0, k;
	struct { int node, first, last, bottom; } cell[32];

	columns = s->grid >= 0 ? web_grid_columns(css_text + s->grid, w, s->gap, width, 32) : 0;
	if (columns < 2)
		columns = 1, width[0] = w;
	for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next) {
		if (!web_shown(node, kid) || (web_nodes[kid].flags & N_SPACE))
			continue;
		cell[col].node = kid;
		cell[col].first = web_item_count;
		cell[col].bottom = web_item_box(node, kid, at, width[col], y);
		cell[col].last = web_item_count;
		height = web_most(height, cell[col].bottom - y);
		at += width[col] + s->gap;
		if (++col == columns) {
			/* a row's boxes are all as high as its highest */
			for (k = 0; k < columns; k++)
				web_stretch(cell[k].node, cell[k].first, cell[k].last, y, cell[k].bottom, height - (cell[k].bottom - y));
			y += height + s->row_gap;
			col = height = 0;
			at = x;
		}
	}
	for (k = 0; k < col; k++)
		web_stretch(cell[k].node, cell[k].first, cell[k].last, y, cell[k].bottom, height - (cell[k].bottom - y));
	return y + height;
}

/* The lines round a box: each side that has a width, in its colour. */
static void
web_borders(int node, const struct css_style *s, int x, int y, int w, int h)
{
	int side;

	for (side = 0; side < 4; side++) {
		int bw = s->border_w[side], colour = s->border_c[side] == CSS_CURRENT ? s->colour : s->border_c[side];

		if (!bw || colour < 0)
			continue;
		if (side == 0)
			web_item_new(I_BOX, node, x, y, w, bw, colour);
		else if (side == 2)
			web_item_new(I_BOX, node, x, y + h - bw, w, bw, colour);
		else
			web_item_new(I_BOX, node, side == 3 ? x : x + w - bw, y, bw, h, colour);
	}
}

/* A table: its columns as wide as their cells ask and the table allows, each row as high as
 * its highest cell. */
static int
web_table(int node, int x, int w, int y, int fixed)
{
	const struct css_style *s = web_style_of(node);
	struct web_table t;
	int inner, target = 0, total, tw, row, i, k, kid, col_x[WEB_COLUMNS + 1], box = -1, top = y, gap = web_gap;
	int edge = s->border_w[3] + s->border_w[1] + css_px(s->p[3], w, 0) + css_px(s->p[1], w, 0);
	int parent = web_nodes[node].c.parent;

	web_table_read(node, &t);
	if (!t.columns) {
		web_table_free(&t);
		return y;
	}
	inner = w - edge - t.spacing * (t.columns + 1);
	if (fixed)
		target = inner;
	else if (s->width != CSS_UNSET && s->width != CSS_AUTO)
		target = web_least(css_px(s->width, w, 0) - edge - t.spacing * (t.columns + 1), inner);
	total = web_table_widths(&t, target > 0 ? target : 0, web_most(inner, 0));
	tw = total + edge + t.spacing * (t.columns + 1);
	if (tw < w && !fixed) {
		if ((s->m[3] == CSS_AUTO && s->m[1] == CSS_AUTO) || (parent >= 0 && (web_nodes[parent].flags & N_CENTER)) ||
		    (parent >= 0 && web_style_of(parent)->align == 1 && web_nodes[parent].c.tag == T_center))
			x += (w - tw) / 2;
		else if (s->m[3] == CSS_AUTO)
			x += w - tw;
		else
			x += web_most(0, css_px(s->m[3], w, 0));
	}
	col_x[0] = x + s->border_w[3] + css_px(s->p[3], w, 0) + t.spacing;
	for (k = 0; k < t.columns; k++)
		col_x[k + 1] = col_x[k] + t.width[k] + t.spacing;
	/* what is said above the table */
	for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next)
		if (web_nodes[kid].c.tag != T_TEXT && web_style_of(kid)->display == D_CAPTION)
			y = web_block_box(kid, x, tw, y, NULL, 1);
	top = y;
	if (s->bg >= 0)
		box = web_item_count, web_item_new(I_BOX, node, x, y, tw, 0, s->bg);
	y += s->border_w[0] + css_px(s->p[0], w, 0) + t.spacing;
	for (row = 0; row < t.rows; row++) {
		const struct css_style *rs = t.row_node[row] != node ? web_style_of(t.row_node[row]) : NULL;
		int row_box = -1, height = rs && rs->minh > 0 ? rs->minh : 0;

		if (rs && rs->height != CSS_UNSET && rs->height != CSS_AUTO && rs->height < CSS_PCT / 2)
			height = web_most(height, rs->height);
		if (rs && rs->bg >= 0)
			row_box = web_item_count, web_item_new(I_BOX, t.row_node[row], col_x[0], y, col_x[t.columns] - t.spacing - col_x[0], 0, rs->bg);
		if (rs)
			web_nodes[t.row_node[row]].y = y;
		for (i = t.row_first[row]; i < t.row_first[row + 1]; i++) {
			struct web_cell *c = &t.cells[i];
			const struct css_style *cs = web_style_of(c->node);
			struct web_floats fl;
			int cx = col_x[c->col], cw = col_x[c->col + c->span] - t.spacing - cx;
			int in_x = cx + cs->border_w[3] + css_px(cs->p[3], cw, 0), in_w = cw - web_inside_extras(cs);

			fl.n = 0;
			web_nodes[c->node].y = y;
			c->first = web_item_count;
			if (cs->bg >= 0)
				web_item_new(I_BOX, c->node, cx, y, cw, 0, cs->bg);
			c->last = web_item_count;	/* (from here on: what moves when the cell is higher than its content) */
			web_gap = 0;
			if (cs->display == D_FLEX && !cs->column)
				c->bottom = web_flex_row(c->node, in_x, web_most(in_w, 0), y + cs->border_w[0] + css_px(cs->p[0], cw, 0));
			else
				c->bottom = web_flow_kids(c->node, in_x, web_most(in_w, 0), y + cs->border_w[0] + css_px(cs->p[0], cw, 0), &fl);
			c->bottom = web_most(c->bottom, web_floats_bottom(&fl, 0)) + css_px(cs->p[2], cw, 0) + cs->border_w[2];
			if (cs->height != CSS_UNSET && cs->height != CSS_AUTO && cs->height < CSS_PCT / 2)
				c->bottom = web_most(c->bottom, y + cs->height);
			c->bottom = web_most(c->bottom, y + cs->minh);
			height = web_most(height, c->bottom - y);
		}
		for (i = t.row_first[row]; i < t.row_first[row + 1]; i++) {
			struct web_cell *c = &t.cells[i];
			const struct css_style *cs = web_style_of(c->node);
			int extra = height - (c->bottom - y), last = web_item_count;
			int cx = col_x[c->col], cw = col_x[c->col + c->span] - t.spacing - cx;

			/* (cells after this one have their items after its own: only its own move) */
			if (i + 1 < t.row_first[row + 1])
				last = t.cells[i + 1].first;
			if (extra > 0 && cs->valign != 1)
				web_shift(c->last, last, 0, cs->valign == 3 ? extra : extra / 2);
			if (c->last > c->first)
				web_items[c->first].h = height;
			web_borders(c->node, cs, cx, y, cw, height);
		}
		if (row_box >= 0)
			web_items[row_box].h = height;
		y += height + t.spacing;
	}
	y += css_px(s->p[2], w, 0) + s->border_w[2];
	if (box >= 0)
		web_items[box].h = y - top;
	web_borders(node, s, x, top, tw, y - top);
	web_table_free(&t);
	web_gap = gap;
	if (y > top)
		web_gap = 0;
	return y;
}

/* A list item's mark, left of its first line. */
static void
web_marker(int node, const struct css_style *s, int x, int y)
{
	unsigned short text[12];
	int n = 0, face = web_text_face(s), at, w, number = 1, sibling, lh;

	if (!web_faces[face].file && !web_plain)
		return;
	if (s->bullet == 2) {
		int parent = web_nodes[node].c.parent;
		char digits[12];

		for (sibling = web_nodes[node].c.prev; sibling >= 0; sibling = web_nodes[sibling].c.prev)
			number += web_nodes[sibling].c.tag == T_li;
		if (parent >= 0)
			number += web_attr_int(parent, "start", 1) - 1;
		snprintf(digits, sizeof digits, "%d.", number);
		for (n = 0; digits[n]; n++)
			text[n] = digits[n];
	} else {
		text[n++] = web_plain ? '*' : 0x2022;
	}
	at = web_text_add(text, n);
	w = web_text_width(face, at, n);
	lh = web_line_height(s, web_faces[face].height);
	web_item_new(I_TEXT, node, x - w - 5, y + (lh - web_faces[face].height) / 2, w, web_faces[face].height, s->colour);
	web_items[web_item_count - 1].a = at;
	web_items[web_item_count - 1].b = n;
	web_items[web_item_count - 1].face = face;
}

/*
 * An element as a box: its margins' inside edge at x, in a column `w` wide, its top at y.
 * `fixed`: it is exactly `w` wide with its side margins, whatever its style says of a width.
 * `fl` are the floats its lines go round; a box that keeps its own gets NULL. Returns its bottom.
 */
static int
web_block_box(int node, int x, int w, int y, struct web_floats *fl, int fixed)
{
	struct web_node *e = &web_nodes[node];
	const struct css_style *s = web_style_of(node);
	struct web_floats own;
	int ml = css_px(s->m[3], w, 0), mr = css_px(s->m[1], w, 0), sides, cw, bw, bx, cx, cy, top = y, box = -1, parent = e->c.parent;
	int pt = css_px(s->p[0], w, 0), pb = css_px(s->p[2], w, 0), floats_from, want;

	e->y = y;
	if (s->display == D_TABLE)
		return web_table(node, x, w, y, fixed);
	ml = ml < 0 ? 0 : ml;
	mr = mr < 0 ? 0 : mr;
	sides = web_inside_extras(s);
	cw = w - ml - mr - sides;
	if (!fixed) {
		want = cw;
		if (s->width != CSS_UNSET && s->width != CSS_AUTO)
			want = css_px(s->width, w, cw) - (s->border_box ? sides : 0);
		if (s->maxw != CSS_UNSET && css_px(s->maxw, w, cw) - (s->border_box ? sides : 0) < want)
			want = css_px(s->maxw, w, cw) - (s->border_box ? sides : 0);
		if (css_px(s->minw, w, 0) > want)
			want = css_px(s->minw, w, 0);
		if (want < cw && want >= 0) {
			/* narrower than its column: centred or to the right, if its margins or its parent say so */
			if ((s->m[3] == CSS_AUTO && s->m[1] == CSS_AUTO) || (parent >= 0 && (web_nodes[parent].flags & N_CENTER)))
				ml += (cw - want) / 2;
			else if (s->m[3] == CSS_AUTO)
				ml += cw - want;
			cw = want;
		}
	}
	if (cw < 0)
		cw = 0;
	bw = cw + sides;
	bx = x + ml;
	cx = bx + s->border_w[3] + css_px(s->p[3], w, 0);
	if (!fl) {
		own.n = 0;
		fl = &own;
	}
	floats_from = fl->n;
	if (node == 0 || node == web_body) {
		if (s->bg >= 0)
			web_page_bg = s->bg;
	} else if (s->bg >= 0) {
		box = web_item_count;
		web_item_new(I_BOX, node, bx, y, bw, 0, s->bg);
	}
	cy = y + s->border_w[0] + pt;
	if (cy > y)
		web_gap = 0;
	if (s->display == D_LIST_ITEM && s->bullet)
		web_marker(node, s, cx, cy);
	if ((s->display == D_FLEX || s->display == D_INLINE_FLEX) && !s->column) {
		cy = web_flex_row(node, cx, cw, cy);
		web_gap = 0;
	} else if (s->display == D_GRID && s->grid >= 0) {
		cy = web_grid(node, cx, cw, cy);
		web_gap = 0;
	} else if (s->display == D_FLEX || s->display == D_INLINE_FLEX || s->display == D_GRID) {
		/* a column of boxes, with the gap between them */
		int kid, any = 0;

		for (kid = e->first; kid >= 0; kid = web_nodes[kid].next) {
			if (!web_shown(node, kid) || (web_nodes[kid].flags & N_SPACE))
				continue;
			cy = web_item_box(node, kid, cx, cw, cy + (any++ ? s->row_gap : 0));
		}
		web_gap = 0;
	} else {
		int before = cy;

		cy = web_flow_kids(node, cx, cw, cy, fl);
		/* a box of nothing but floats holds them (a row of them is a bar) */
		if (cy == before && fl->n > floats_from && fl != &own) {
			int i;

			for (i = floats_from; i < fl->n; i++)
				cy = web_most(cy, fl->f[i].y1);
		}
	}
	if (fl == &own)
		cy = web_most(cy, web_floats_bottom(&own, 0));
	if (s->height != CSS_UNSET && s->height != CSS_AUTO && s->height < CSS_PCT / 2 && e->c.tag != T_html && e->c.tag != T_body)
		cy = web_most(cy, y + s->border_w[0] + pt + s->height - (s->border_box ? s->border_w[0] + s->border_w[2] + pt + pb : 0));
	if (s->minh > 0 && s->minh < CSS_PCT / 2 && e->c.tag != T_html && e->c.tag != T_body)
		cy = web_most(cy, y + s->border_w[0] + pt + s->minh);
	cy += pb + s->border_w[2];
	if (cy > top && (pb || s->border_w[2] || s->border_w[0] || pt))
		web_gap = 0;
	if (box >= 0)
		web_items[box].h = cy - top;
	web_borders(node, s, bx, top, bw, cy - top);
	return cy;
}

/* Lays the page out for a width. Its height is in web_page_h, its background in web_page_bg. */
static void
web_layout(int width)
{
	int i;

	web_item_count = web_piece_count = 0;
	web_gap = 1 << 20;	/* (the page's first margin is not left above it) */
	web_page_bg = 0xffffff;
	web_line_count = 0;
	for (i = 0; i < web_node_count; i++)
		web_nodes[i].minw = web_nodes[i].maxw = -1;
	web_gap = 0;
	web_page_h = web_node_count ? web_block_box(0, 0, width, 0, NULL, 0) : 0;
	for (i = 0; i < web_item_count; i++)
		web_page_h = web_most(web_page_h, web_items[i].y + web_items[i].h);
}

/* The link an element is in: the <a href> itself, or -1. */
static int
web_link_of(int node)
{
	const char *value;
	int n;

	for (; node >= 0; node = web_nodes[node].c.parent)
		if (web_nodes[node].c.tag == T_a && web_attr(node, "href", &value, &n))
			return node;
	return -1;
}

#endif
