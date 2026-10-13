/*
 * nxweb: a browser (docs/web.md). The page is HTML laid out by web.h and web_layout.h with
 * the sheets css.h reads, in the fonts of the ROM; no scripts. It is drawn once into a pixmap
 * the GPU keeps, and scrolling copies from that. A page in the image is read as a file; any
 * other address is asked of the host (docs/fetch.md), which is the only part of this machine
 * that has a network, unless the image keeps a copy (web/preload.py).
 *   nxweb [ADDRESS or FILE]
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include "ui.h"
#include "web_layout.h"

#define BAR 26
#define STATUS 18
#define WIDTH 600
#define HEIGHT 420
#define HOME "/usr/share/web-index.html"
#define FETCH_MOST 0x40000	/* bytes the host hands over at a time */
#define PAGE_MOST (3 << 19)	/* of a page that are read */
#define SHEET_MOST (3 << 18)
#define REG_REQUEST 64		/* words of the control block: requests made, length of the address, kind, offset */
#define REG_REPLY 68		/* requests answered, bytes, status, a picture's width | height << 16 */
#define REG_ADDRESS 72		/* the address, 256 bytes */
#define REG_PIXMAPS (0x1800 / 4 + 28)	/* pixmaps the GPU keeps, counted by the screen's driver */
#define MAX_PICTURES 160
#define TILE_BYTES (3200 * 1024)	/* of the GPU's memory for the page as drawn */

static GR_WINDOW_ID window;
static int width = WIDTH, height = HEIGHT;
static int line_h;

static char *page;		/* the HTML as it came */
static int page_length;
static char address[256], typed[256], status[200];
static int editing;		/* the address field has the keyboard */
static int picked = -1;		/* the link the keyboard is on (Tab): its element, or -1 */
static struct { char address[256]; int top; } history[16];
static int history_count;
static int top, goal;		/* the page's row at the view's top, and where it is scrolling to */
static int dragging;
static int sent_on;		/* pages in a row that sent their reader on to another */
static int verbose, dump;
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_KEY_DOWN | \
	GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ)

static volatile uint32_t *regs;
static const unsigned char *fetched;
static int loading;		/* the request the host has not answered yet, or 0 */
static int waited, ticked;	/* ticked: the event just had was the wait running out */
static int by_hand;		/* the host may not open this address itself: asked again until it may */
static char *part;		/* a page or a sheet that comes in parts: what is here of it */
static int part_have;

/* The page's pictures. One the image keeps is drawn from there; one the host fetches and
 * decodes arrives as pixels (docs/fetch.md), kept as a file the GPU then draws from. */
enum { PICTURE_WANTED, PICTURE_HERE, PICTURE_FAILED, PICTURE_BY_HAND };
static struct picture { char address[256], file[64]; int w, h, state, sized, ours; time_t asked; } *pictures;
static int picture_count;
static int asking = -1;		/* the picture the request that is out is for, or -1: the page */
static FILE *part_file;		/* a picture that comes in parts: its file, its size, pixels so far */
static int part_w, part_h, part_done;
#define SHEET 1000		/* `asking` for sheet 0 */

/* The page as drawn: `tile_h` rows of it from `tile_top`, in a pixmap. */
static GR_WINDOW_ID tile;
static int tile_w, tile_h, tile_top, tile_drawn, tile_gpu;
static GR_FONT_ID fonts[WEB_FACES][2];

/* What a visitor has typed into the page's fields. */
static struct field { int node, n, checked, option; unsigned short text[128]; } *fields;
static int field_count, focus = -1;

static void show_view(void);

/* Instructions the first core has run (the low word of its count): what a step cost. */
static unsigned
now(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

static int
view_height(void)
{
	return height - BAR - STATUS;
}

static int
view_width(void)
{
	return width - UI_SCROLL_W;
}

/* ---- what the engine asks for ---- */

static const void *
web_file(const char *name, int *size)
{
	char path[80];
	struct stat st;
	void *bytes;
	int fd;

	snprintf(path, sizeof path, "/usr/share/%s", name);
	if ((fd = open(path, O_RDONLY)) < 0)
		return NULL;
	bytes = fstat(fd, &st) == 0 ? mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	close(fd);
	*size = (int)st.st_size;
	return bytes == MAP_FAILED ? NULL : bytes;
}

static int
web_picture(int node, const char *where, int *w, int *h)
{
	struct picture *p;
	int i, said_w = web_attr_int(node, "width", -1), said_h = web_attr_int(node, "height", -1);

	for (i = 0; i < picture_count && strcmp(pictures[i].address, where); i++)
		;
	if (i == picture_count) {
		if (picture_count == MAX_PICTURES)
			return -1;
		p = &pictures[picture_count++];
		memset(p, 0, sizeof *p);
		snprintf(p->address, sizeof p->address, "%s", where);
		if (where[0] == '/') {
			/* a picture in the image: drawn by the GPU from the file where it lies */
			snprintf(p->file, sizeof p->file, "%.60s", where);
			p->state = strlen(where) < 60 && ui_picture_size(where, &p->w, &p->h) ? PICTURE_HERE : PICTURE_FAILED;
		} else if (web_cache_find(where, 'I', p->file, sizeof p->file, &p->w, &p->h)) {
			p->state = PICTURE_HERE;
		} else {
			snprintf(p->file, sizeof p->file, "/tmp/nxweb-%d-%d.ppm", (int)getpid(), i);
			p->ours = 1;
			/* (one the page says is a dot or nothing is a spacer or counts visitors: not fetched) */
			p->state = regs && !(said_w >= 0 && said_w <= 2) && !(said_h >= 0 && said_h <= 2) ? PICTURE_WANTED : PICTURE_FAILED;
		}
	}
	p = &pictures[i];
	if (p->state == PICTURE_FAILED)
		return -1;
	*w = p->state == PICTURE_HERE ? p->w : 0;
	*h = p->state == PICTURE_HERE ? p->h : 0;
	if (p->state != PICTURE_HERE && (said_w < 0 || said_h < 0))
		p->sized = 0;	/* (laid out before it came, the page not saying its size: laid out again when it does) */
	return i;
}

/* ---- fields ---- */

static struct field *
field_of(int node)
{
	char value[300];
	struct field *f;
	int i, used, left;
	const char *v;

	for (i = 0; i < field_count; i++)
		if (fields[i].node == node)
			return &fields[i];
	fields = realloc(fields, (field_count + 1) * sizeof *fields);
	f = &fields[field_count++];
	memset(f, 0, sizeof *f);
	f->node = node;
	f->checked = web_attr(node, "checked", &v, &i);
	if (web_nodes[node].c.tag == T_input && web_attr_text(node, "value", value, sizeof value))
		for (v = value, left = strlen(value); left > 0 && f->n < 127; v += used, left -= used)
			f->text[f->n++] = web_char((const unsigned char *)v, left, &used);
	if (web_nodes[node].c.tag == T_select) {
		int kid, k = 0;

		for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next)
			if (web_nodes[kid].c.tag == T_option) {
				if (web_attr(kid, "selected", &v, &i))
					f->option = k;
				k++;
			}
	}
	return f;
}

static const char *
field_type(int node)
{
	static char type[24];

	strcpy(type, "text");
	if (web_nodes[node].c.tag == T_input)
		web_attr_text(node, "type", type, sizeof type);
	else
		strcpy(type, web_nodes[node].c.tag == T_select ? "select" : "textarea");
	return type;
}

static int
field_is_button(const char *type)
{
	return !strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "reset") || !strcasecmp(type, "image");
}

/* ---- drawing ---- */

static GR_COLOR
mw(int rgb)
{
	return MWRGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
}

/* Characters in one of the ROM's faces, their top left corner at x, y. */
static void
page_text(GR_DRAW_ID to, int x, int y, const unsigned short *text, int n, int face, int italic, int colour)
{
	GR_FONT_ID *font = &fonts[face][italic];

	if (web_plain) {
		/* the window system's own font: Latin letters, bold by drawing twice */
		char plain[256];
		int i, bold = face < FACE_UNI && (face / WEB_SIZES) % 2;

		for (; n > 0; n -= i, text += i, x += ui_width(&ui_var_widths, ui_var, plain, i)) {
			for (i = 0; i < n && i < 255; i++)
				plain[i] = text[i] >= 32 && text[i] < 127 ? text[i] : text[i] < 32 ? ' ' : '?';
			ui_text(to, x, y, plain, i, mw(colour), 0);
			if (bold)
				ui_text(to, x + 1, y, plain, i, mw(colour), 0);
		}
		return;
	}
	if (!*font) {
		char path[80];

		snprintf(path, sizeof path, "/usr/share/%s", web_faces[face].name);
		/* (the height asks for a size, the width for a slant in 256ths of a pixel a row) */
		*font = GrCreateFontEx(path, web_faces[face].twice ? 32 : 0, italic ? 51 : 0, NULL);
	}
	GrSetGCFont(ui_gc, *font);
	GrSetGCForeground(ui_gc, mw(colour));
	GrText(to, ui_gc, x, y, (void *)text, n, GR_TFUC16 | GR_TFTOP);
}

static int
plain_face(void)
{
	int face = web_face_for(0, 0, CSS_EM);

	web_face(face);
	return face;
}

static void
draw_field(GR_DRAW_ID to, const struct web_item *it, int y)
{
	struct field *f = field_of(it->node);
	const char *type = field_type(it->node);
	unsigned short label[128];
	char text[200];
	int face = plain_face(), n = 0, w = 0, i, grey = 0, used, left;
	const char *v;

	if (field_is_button(type)) {
		ui_fill(to, it->x, y, it->w, it->h, UI_FACE);
		ui_bevel(to, it->x, y, it->w, it->h, 0);
		if (!web_attr_text(it->node, "value", text, sizeof text) && !web_attr_text(it->node, "alt", text, sizeof text))
			strcpy(text, !strcasecmp(type, "reset") ? "Reset" : "Submit");
		for (v = text, left = strlen(text); left > 0 && n < 127; v += used, left -= used)
			label[n++] = web_char((const unsigned char *)v, left, &used);
		for (i = 0; i < n; i++)
			w += web_most(0, web_advance(face, label[i]));
		page_text(to, it->x + (it->w - w) / 2, y + 3, label, n, face, 0, 0);
		return;
	}
	ui_fill(to, it->x, y, it->w, it->h, WHITE);
	GrSetGCForeground(ui_gc, focus == it->node ? BLACK : UI_SHADOW);
	GrRect(to, ui_gc, it->x, y, it->w, it->h);
	if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) {
		if (f->checked)
			ui_fill(to, it->x + 3, y + 3, it->w - 6, it->h - 6, BLACK);
		return;
	}
	if (!strcasecmp(type, "select")) {
		int kid, k = 0;

		for (kid = web_nodes[it->node].first; kid >= 0; kid = web_nodes[kid].next)
			if (web_nodes[kid].c.tag == T_option && k++ == f->option && web_nodes[kid].first >= 0 &&
			    web_nodes[web_nodes[kid].first].c.tag == T_TEXT) {
				const struct web_node *t = &web_nodes[web_nodes[kid].first];

				n = web_least(t->text_n, 127);
				memcpy(label, web_u + t->text, n * sizeof *label);
			}
		ui_fill(to, it->x + it->w - 16, y + 1, 15, it->h - 2, UI_FACE);
		label[127] = 'v';
		page_text(to, it->x + it->w - 12, y + 3, label + 127, 1, face, 0, 0);
	} else {
		n = f->n;
		memcpy(label, f->text, n * sizeof *label);
		if (!n && web_attr_text(it->node, "placeholder", text, sizeof text)) {
			for (v = text, left = strlen(text); left > 0 && n < 127; v += used, left -= used)
				label[n++] = web_char((const unsigned char *)v, left, &used);
			grey = 1;
		}
		if (!strcasecmp(type, "password"))
			for (i = 0; i < n && !grey; i++)
				label[i] = '*';
	}
	/* as much of the text as the field holds: its end, while it is typed into */
	for (i = n, w = 0; i > 0 && w + web_most(0, web_advance(face, label[i - 1])) < it->w - 8 - (it->h > 30 ? 0 : 0); i--)
		w += web_most(0, web_advance(face, label[i - 1]));
	if (focus != it->node || grey) {
		for (i = 0, w = 0; i < n && w + web_most(0, web_advance(face, label[i])) < it->w - 8; i++)
			w += web_most(0, web_advance(face, label[i]));
		n = i;
		i = 0;
	}
	if (n > i)
		page_text(to, it->x + 4, y + 3, label + i, n - i, face, 0, grey ? 0x808080 : 0);
	if (focus == it->node)
		ui_fill(to, it->x + 4 + (grey ? 0 : w) + 1, y + 3, 1, it->h - 6, BLACK);
}

/* One thing of the page, `dy` above where the page has it. */
static void
draw_item(GR_DRAW_ID to, const struct web_item *it, int dy)
{
	int y = it->y - dy;

	switch (it->kind) {
	case I_BOX:
		if (it->colour >= 0 && it->w > 0 && it->h > 0)
			ui_fill(to, it->x, y, it->w, it->h, mw(it->colour));
		if (it->flags && it->w > 0) {
			GrSetGCForeground(ui_gc, mw(it->b));
			GrRect(to, ui_gc, it->x, y, it->w, it->h);
		}
		break;
	case I_TEXT:
		page_text(to, it->x, y, web_u + it->a, it->b, it->face, (it->flags & IF_ITALIC) != 0, it->colour);
		if (it->flags & IF_UNDER)
			ui_fill(to, it->x, y + web_faces[it->face].ascent + 1, it->w, 1, mw(it->colour));
		if (it->flags & IF_THROUGH)
			ui_fill(to, it->x, y + web_faces[it->face].ascent * 2 / 3, it->w, 1, mw(it->colour));
		if (picked >= 0 && web_link_of(it->node) == picked) {
			GrSetGCForeground(ui_gc, mw(it->colour));
			GrRect(to, ui_gc, it->x - 2, y - 1, it->w + 4, it->h + 2);
		}
		break;
	case I_IMAGE:
		if (pictures[it->a].state != PICTURE_HERE)
			ui_fill(to, it->x, y, it->w, it->h, MWRGB(232, 232, 232));	/* not here yet */
		else
			GrDrawImageFromFile(to, ui_gc, it->x, y, it->w, it->h, pictures[it->a].file, 0);
		break;
	case I_FIELD:
		draw_field(to, it, y);
		break;
	}
}

/* Draws the page's rows y0 to y1 on `to`, whose own top row is the page's `dy`. */
static void
draw_rows(GR_DRAW_ID to, int dy, int y0, int y1)
{
	unsigned began = now();
	int i, drawn = 0;

	ui_fill(to, 0, y0 - dy, view_width(), y1 - y0, mw(web_page_bg));
	for (i = 0; i < web_item_count; i++) {
		const struct web_item *it = &web_items[i];

		if (it->y >= y1 || it->y + it->h + 2 <= y0)
			continue;
		draw_item(to, it, dy);
		drawn++;
	}
	if (verbose)
		printf("nxweb: asked for %d of %d things, rows %d to %d, %u thousand instructions\n", drawn, web_item_count, y0, y1,
		       (now() - began) / 1000);
}

/* The pixmap the page is drawn in, for the window as it is now: as high as TILE_BYTES allow.
 * The driver counts the pixmaps the GPU keeps; one it does not keep is no use (0). */
static int
tile_make(void)
{
	GR_FONT_INFO info;
	uint32_t before = regs ? regs[REG_PIXMAPS] : 0;
	int w = view_width(), h = TILE_BYTES / (4 * w);

	if (tile_w == w)
		return tile_gpu;	/* (made, or tried in vain, for this width already) */
	if (tile)
		GrDestroyWindow(tile);
	tile = 0;
	tile_drawn = 0;
	tile_gpu = 0;
	tile_w = w;
	tile_h = h > 2048 ? 2048 : h;
	if (!regs || tile_h < view_height() + 64 || getenv("NXWEB_DIRECT"))
		return 0;
	tile = GrNewPixmap(w, tile_h, NULL);
	GrGetFontInfo(ui_var, &info);	/* (an answer: the pixmap is made by now) */
	tile_gpu = regs[REG_PIXMAPS] != before;
	if (!tile_gpu) {
		GrDestroyWindow(tile);
		tile = 0;
	}
	return tile_gpu;
}

static void
draw_bar(void)
{
	static const char *names[] = { "Back", "Home", "Reload" };
	int i, x = 3 + 3 * 55;

	ui_fill(window, 0, 0, width, BAR, UI_FACE);
	for (i = 0; i < 3; i++)
		ui_button(window, 3 + i * 55, 3, 52, BAR - 6, names[i], 0);
	ui_fill(window, x, 3, width - x - 3, BAR - 6, WHITE);
	ui_bevel(window, x, 3, width - x - 3, BAR - 6, 1);
	ui_text_fit(window, x + 5, 7, width - x - 14, editing ? typed : address, BLACK);
	if (editing)
		ui_fill(window, x + 5 + ui_width(&ui_var_widths, ui_var, typed, strlen(typed)), 6, 1, BAR - 12, BLACK);
}

static void
draw_status(void)
{
	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_text_fit(window, 6, height - STATUS + 3, width - 12, status, BLACK);
}

/* The view shows rows `top` on: the pixmap is drawn anew when they are not all in it, and
 * the view is a copy of a part of it. With no pixmap the rows are drawn on the window. */
static void
show_view(void)
{
	int view = view_height(), most = web_page_h - view;

	if (!window)
		return;
	top = top > most ? most : top;
	top = top < 0 ? 0 : top;
	if (!tile_make()) {
		draw_rows(window, top - BAR, top, top + view);
		draw_bar();
		draw_status();
	} else {
		if (!tile_drawn || top < tile_top || top + view > tile_top + tile_h) {
			/* the view in its middle, or nearer the way it is going */
			tile_top = top - (tile_h - view) / 2 + (goal > top ? view / 2 : goal < top ? -view / 2 : 0);
			tile_top = tile_top > web_page_h - tile_h ? web_page_h - tile_h : tile_top;
			tile_top = tile_top < 0 ? 0 : tile_top;
			draw_rows(tile, tile_top, tile_top, tile_top + tile_h);
			tile_drawn = 1;
		}
		GrCopyArea(window, ui_gc, 0, BAR, view_width(), view, tile, 0, top - tile_top, MWROP_COPY);
	}
	ui_scrollbar(window, width - UI_SCROLL_W, BAR, view, web_page_h, view, top);
}

static void
draw_all(void)
{
	draw_bar();
	draw_status();
	show_view();
}

static void
scroll_to(int y, int at_once)
{
	int most = web_page_h - view_height();

	y = y > most ? most : y;
	y = y < 0 ? 0 : y;
	goal = y;
	if (at_once && top != y) {
		top = y;
		show_view();
	}
}

/* A step of a scroll that is under way: a third of what is left, eight rows at least. With
 * NXWEB_TIME a scroll says when it ends how its steps went: the host's clock (word 13 of the
 * control block) at each, and whether the GPU's copy of the step before had been made by then
 * (word 14 counts the copies: a step whose picture never got to the window was not seen). */
static void
scroll_step(void)
{
	static unsigned first_ms, last_ms, longest, copies, shown, steps;
	int left = goal - top, step = left / 3;

	if (!left)
		return;
	if (abs(step) < 8)
		step = abs(left) < 8 ? left : left > 0 ? 8 : -8;
	top += step;
	show_view();
	if (verbose && regs) {
		unsigned ms = regs[13], made = regs[14];

		if (!steps)
			first_ms = ms, longest = shown = 0;
		else {
			longest = ms - last_ms > longest ? ms - last_ms : longest;
			shown += made != copies;
		}
		last_ms = ms, copies = made, steps++;
		if (top == goal) {
			printf("nxweb: a scroll of %u steps in %u ms, %u ms the longest between two; %u of the %u before the last were on the screen by the next\n",
			       steps, ms - first_ms, longest, shown, steps - 1);
			steps = 0;
		}
	}
}

/* ---- pages ---- */

static void
say_time(const char *what, unsigned began)
{
	if (verbose)
		printf("nxweb: %s %u thousand instructions\n", what, (now() - began) / 1000);
}

/* Lays the page out again: its sheets read (0: they are as they were), its tree made, its
 * styles worked out, each only when something it comes from changed. */
static void
lay_out(int sheets, int tree)
{
	unsigned began = now();
	int i, texts = 0, chars = 0, boxes = 0, images = 0, items = 0;

	if (sheets) {
		web_read_sheets(view_width(), view_height());
		say_time("sheets", began);
	}
	if (sheets || tree) {
		began = now();
		web_parse(page, page_length);
		say_time("tree", began);
		began = now();
		web_style_all();
		say_time("styles", began);
	}
	began = now();
	for (i = 0; i < picture_count; i++)
		pictures[i].sized = 1;
	web_layout(view_width());
	say_time("layout", began);
	tile_drawn = 0;
	if (dump) {
		for (i = 0; i < web_item_count; i++) {
			const struct web_item *it = &web_items[i];

			texts += it->kind == I_TEXT, boxes += it->kind == I_BOX, images += it->kind == I_IMAGE, items += it->kind == I_FIELD;
			chars += it->kind == I_TEXT ? it->b : 0;
		}
		printf("nxweb: %s: %d nodes, %d rules, %d x %d; %d texts (%d characters), %d boxes, %d pictures, %d fields\n", address,
		       web_node_count, css_rule_count, view_width(), web_page_h, texts, chars, boxes, images, items);
	}
}

static void
set_title(void)
{
	char caption[160];

	if (!window)
		return;
	snprintf(caption, sizeof caption, "%s - Web", web_title[0] ? web_title : address);
	GrSetWindowTitle(window, caption);
}

/* Where the element with an id is on the page, or -1. */
static int
anchor(const char *id)
{
	const char *value;
	int i, n, m = strlen(id);

	for (i = 0; m && i < web_node_count; i++)
		if (web_nodes[i].c.tag != T_TEXT && (web_attr(i, "id", &value, &n) || (web_nodes[i].c.tag == T_a && web_attr(i, "name", &value, &n))) &&
		    n == m && !strncmp(value, id, n))
			return web_nodes[i].y;
	return -1;
}

/* Shows a page whose text is the caller's allocation of n + 1 bytes or more: it is kept. */
static void
show_kept(char *html, int n, const char *note)
{
	const char *hash = strchr(address, '#');
	int i;

	unsigned began = now();

	free(page);
	page = html;
	page_length = n;
	top = goal = 0;
	picked = focus = -1;
	/* another page: the last one's pictures and what was typed go */
	for (i = 0; i < picture_count; i++)
		if (pictures[i].state == PICTURE_HERE && pictures[i].ours)
			unlink(pictures[i].file);
	picture_count = field_count = 0;
	asking = -1;
	snprintf(web_address, sizeof web_address, "%s", address);
	web_find_sheets(page, page_length, view_width());
	for (i = 0; i < web_sheet_count; i++) {
		struct web_sheet *sh = &web_sheets[i];
		char file[80];
		FILE *f;

		if (sh->state != SHEET_WANTED)
			continue;
		/* one in the image, or one the image keeps a copy of, is read at once */
		if (sh->address[0] == '/')
			snprintf(file, sizeof file, "%.79s", sh->address);
		else if (!web_cache_find(sh->address, 'S', file, sizeof file, NULL, NULL))
			file[0] = 0;
		sh->state = regs && sh->address[0] != '/' ? SHEET_WANTED : SHEET_FAILED;
		if (file[0] && (f = fopen(file, "rb")) != NULL) {
			if ((sh->text = malloc(SHEET_MOST)) != NULL) {
				sh->length = fread(sh->text, 1, SHEET_MOST, f);
				sh->own = 1;
				sh->state = SHEET_HERE;
			}
			fclose(f);
		}
	}
	say_time("finding the sheets", began);
	lay_out(1, 1);
	if (getenv("NXWEB_AGAIN"))
		lay_out(1, 1);	/* (what a page costs once the program's own pages are in memory) */
	snprintf(status, sizeof status, "%s", note);
	set_title();
	if (hash && (i = anchor(hash + 1)) >= 0)
		top = goal = i;
	if (window)
		draw_all();
}

static void
show(const char *html, int n, const char *note)
{
	char *copy = malloc(n + 1);

	memcpy(copy, html, n);
	copy[n] = 0;
	show_kept(copy, n, note);
}

static void
say(const char *heading_text, const char *text)
{
	char html[800];
	int n = snprintf(html, sizeof html, "<body style=\"font-family:sans-serif\"><h2>%s</h2><p>%s</p><p><a href=\"" HOME "\">Home</a></p>", heading_text, text);

	show(html, n, "");
}

/* Asks the host for what is at an address, a page (kind 0), a picture (1) or a sheet (2), from
 * a byte of it on: it sees the address and the request's number, and answers when it has it. */
static void
ask_part(const char *what, int kind, uint32_t offset)
{
	const char *hash = strchr(what, '#');
	int n = hash ? (int)(hash - what) : (int)strlen(what);

	memset((void *)&regs[REG_ADDRESS], 0, 256);
	memcpy((void *)&regs[REG_ADDRESS], what, n);
	regs[REG_REQUEST + 1] = n;
	regs[REG_REQUEST + 2] = kind;
	regs[REG_REQUEST + 3] = offset;
	loading = regs[REG_REQUEST] + 1;
	if (!loading)
		loading = 1;
	regs[REG_REQUEST] = loading;
	waited = 0;
}

static void
ask(const char *what, int kind)
{
	part_have = 0;
	ask_part(what, kind, 0);
}

static int
read_file(const char *path, const char *note)
{
	FILE *file = fopen(path, "rb");
	char *text;
	int n;

	if (!file)
		return 0;
	text = malloc(PAGE_MOST + 1);
	n = fread(text, 1, PAGE_MOST, file);
	text[n] = 0;
	fclose(file);
	show_kept(text, n, note);
	return 1;
}

/* Goes to an address. `how`: 1 remembers the page left behind, 2 asks the host even for a
 * page the image keeps a copy of. */
static void
go(const char *where, int how)
{
	char file[80], plain[256];
	char *hash;

	if (how)
		sent_on = 0;
	if (how == 1 && address[0]) {
		if (history_count == 16)
			memmove(&history[0], &history[1], sizeof history[0] * --history_count);
		strcpy(history[history_count].address, address);
		history[history_count++].top = top;
	}
	snprintf(address, sizeof address, "%s", strncmp(where, "file://", 7) ? where : where + 7);
	editing = loading = by_hand = 0;
	picked = -1;
	if (part_file)
		fclose(part_file);
	part_file = NULL;
	snprintf(plain, sizeof plain, "%s", address);
	if ((hash = strchr(plain, '#')) != NULL)
		*hash = 0;
	if (address[0] == '/') {
		if (!read_file(plain, "from this computer"))
			say("No such page", address);
		return;
	}
	if (how != 2 && web_cache_find(plain, 'P', file, sizeof file, NULL, NULL) && read_file(file, "a copy this computer keeps (Reload asks the network)"))
		return;
	if (!regs) {
		say("No network", "This machine has no way to ask its host for a page.");
		return;
	}
	asking = -1;
	ask(address, 0);
	snprintf(status, sizeof status, "asking the host for %.150s ...", address);
	if (window) {
		draw_bar();
		draw_status();
	}
}

/* Text that comes in parts (a page, a sheet): true when all that is wanted of it is in `part`. */
static int
part_text(const char *what, int kind, int total, int most)
{
	int here;

	if (total > most)
		total = most;
	here = total - part_have < FETCH_MOST ? total - part_have : FETCH_MOST;
	if (!part_have)
		part = malloc(total + 1);
	if (!part || here <= 0)
		return 1;
	memcpy(part + part_have, fetched, here);
	part_have += here;
	part[part_have] = 0;
	if (part_have >= total)
		return 1;
	ask_part(what, kind, (uint32_t)part_have);
	return 0;
}

static void
by_hand_note(void)
{
	if (!strstr(status, "by the screen")) {
		snprintf(status, sizeof status, "styles and pictures need their addresses pasted: the button by the screen");
		draw_status();
	}
}

/* Called while a request is out: shows the answer once the host has put it in place. */
static void
check_reply(void)
{
	char note[80];
	int n, code;

	if ((int)regs[REG_REPLY] != loading) {
		if ((waited += ticked) > 300) {	/* (tenths of a second: other events are not time) */
			loading = 0;
			if (asking >= SHEET)
				web_sheets[asking - SHEET].state = SHEET_FAILED;
			else if (asking >= 0)
				pictures[asking].state = PICTURE_FAILED;
			else if (!by_hand)
				say("No answer", "The host did not answer in 30 seconds.");
			if (part_file)
				fclose(part_file);
			part_file = NULL;
			asking = -1;
		}
		return;
	}
	loading = 0;
	n = regs[REG_REPLY + 1];
	code = regs[REG_REPLY + 2];
	if (asking >= SHEET) {
		struct web_sheet *sh = &web_sheets[asking - SHEET];

		sh->state = code == 1 ? SHEET_BY_HAND : SHEET_FAILED;
		sh->asked = time(NULL);
		if (code == 1)
			by_hand_note();
		if (code != 200 || n <= 0 || !part_text(sh->address, 2, n, SHEET_MOST)) {
			if (code != 200 || n <= 0)
				asking = -1;
			else
				sh->state = SHEET_WANTED;	/* (more of it to come) */
			return;
		}
		asking = -1;
		if (!part)
			return;
		sh->text = part;
		sh->length = part_have;
		sh->own = 1;
		sh->state = SHEET_HERE;
		part = NULL;
		lay_out(1, 1);
		show_view();
		return;
	}
	if (asking >= 0) {
		struct picture *p = &pictures[asking];
		int w = regs[REG_REPLY + 3] & 0xffff, h = regs[REG_REPLY + 3] >> 16, i, here;
		FILE *file = part_file;

		part_file = NULL;
		p->state = code == 1 ? PICTURE_BY_HAND : PICTURE_FAILED;
		p->asked = time(NULL);
		if (code == 1)
			by_hand_note();
		if (code != 200 || w <= 0 || h <= 0 || (file && (w != part_w || h != part_h)) ||
		    (!file && (file = fopen(p->file, "wb")) == NULL)) {
			if (file)
				fclose(file);
			asking = -1;
			return;
		}
		/* a word a pixel, 0x00RRGGBB, kept as a PPM file; 65,536 pixels a part */
		if (!ftell(file)) {
			fprintf(file, "P6\n%d %d\n255\n", w, h);
			part_done = 0;
			part_w = w;
			part_h = h;
		}
		here = w * h - part_done < FETCH_MOST / 4 ? w * h - part_done : FETCH_MOST / 4;
		for (i = 0; i < here; i++) {
			uint32_t c = ((const uint32_t *)fetched)[i];

			fputc(c >> 16 & 255, file);
			fputc(c >> 8 & 255, file);
			fputc(c & 255, file);
		}
		part_done += here;
		if (part_done < w * h) {
			part_file = file;
			p->state = PICTURE_FAILED;	/* (until its last part is here) */
			ask_part(p->address, 1, (uint32_t)part_done * 4);
			return;
		}
		fclose(file);
		asking = -1;
		p->w = w;
		p->h = h;
		p->state = PICTURE_HERE;
		if (!p->sized)
			lay_out(0, 0);
		tile_drawn = 0;
		show_view();
		return;
	}
	if (code == 1) {
		/* VRChat: a visitor has to hand the world this address. Said once; then the page is
		 * asked for again every few seconds, and comes when they have. */
		if (!by_hand)
			say("One step by hand",
			    "VRChat does not let a world open an address by itself, only one a visitor gives it. "
			    "This link's address is now in the field marked <b>Link to copy</b> on the panel in the room. "
			    "Copy it from there and paste it into the field marked <b>Paste here</b> beside it. "
			    "The page then opens by itself.");
		by_hand = 1;
		return;
	}
	by_hand = 0;
	if (code != 200) {
		char text[480];

		snprintf(text, sizeof text, "The host could not get %.180s (%d).%s", address, code,
			 code == 3 ? " VRChat opens only addresses that begin with https." : "");
		say("Page not loaded", text);
		return;
	}
	if (!part_text(address, 0, n, PAGE_MOST))
		return;
	snprintf(note, sizeof note, "%d bytes from the host%s", part_have, n > PAGE_MOST ? " (cut short)" : "");
	n = part_have;
	show_kept(part, n, note);
	part = NULL;
}

static void
follow(const char *href)
{
	char whole[256];
	int y;

	if (href[0] == '#') {
		if ((y = href[1] ? anchor(href + 1) : 0) >= 0)
			scroll_to(y, 0);
		return;
	}
	if (web_resolve(href, whole))
		go(whole, 1);
}

/* The sheet or picture to ask the host for next, or -1: the first three it does not have yet,
 * in the page's order (the host has room for three), one that waits for a visitor once a second. */
static int
next_wanted(void)
{
	time_t now = time(NULL);
	int i, open = 0;

	/* style sheets first: one changes the whole page */
	for (i = 0; i < web_sheet_count && open < 3; i++) {
		if (web_sheets[i].state != SHEET_WANTED && web_sheets[i].state != SHEET_BY_HAND)
			continue;
		open++;
		if (web_sheets[i].state == SHEET_WANTED || web_sheets[i].asked != now)
			return SHEET + i;
	}
	for (i = 0; i < picture_count && open < 3; i++) {
		if (pictures[i].state != PICTURE_WANTED && pictures[i].state != PICTURE_BY_HAND)
			continue;
		open++;
		if (pictures[i].state == PICTURE_WANTED || pictures[i].asked != now)
			return i;
	}
	return -1;
}

static int
wanted_open(void)
{
	int i, open = 0;

	for (i = 0; i < picture_count; i++)
		open += pictures[i].state == PICTURE_WANTED || pictures[i].state == PICTURE_BY_HAND;
	for (i = 0; i < web_sheet_count; i++)
		open += web_sheets[i].state == SHEET_WANTED || web_sheets[i].state == SHEET_BY_HAND;
	return open;
}

/* ---- forms ---- */

static void
query_add(char *query, int room, const char *name, const unsigned short *value, int n)
{
	static const char hex[] = "0123456789ABCDEF";
	int k = strlen(query), i;

	if (k && k < room - 1)
		query[k++] = '&';
	for (; *name && k < room - 4; name++)
		query[k++] = *name;
	if (k < room - 1)
		query[k++] = '=';
	for (i = 0; i < n && k < room - 10; i++) {
		unsigned c = value[i];
		unsigned char utf[3];
		int bytes = c < 0x80 ? 1 : c < 0x800 ? 2 : 3, b;

		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			query[k++] = c;
			continue;
		}
		if (c == ' ') {
			query[k++] = '+';
			continue;
		}
		utf[0] = bytes == 1 ? c : bytes == 2 ? 0xc0 | c >> 6 : 0xe0 | c >> 12;
		utf[1] = bytes == 2 ? 0x80 | (c & 0x3f) : 0x80 | (c >> 6 & 0x3f);
		utf[2] = 0x80 | (c & 0x3f);
		for (b = 0; b < bytes; b++) {
			query[k++] = '%';
			query[k++] = hex[utf[b] >> 4];
			query[k++] = hex[utf[b] & 15];
		}
	}
	query[k] = 0;
}

/* Sends the form a field is in, as an address with the fields' values after it. `pressed` is
 * the button that was pressed, or -1. */
static void
submit(int node, int pressed)
{
	char query[1200] = "", action[600] = "", method[16] = "get", name[80], whole[256], value[200];
	unsigned short wide[128];
	int form, i, end, used, left, n;
	const char *v;
	char *cut;

	for (form = node; form >= 0 && web_nodes[form].c.tag != T_form; form = web_nodes[form].c.parent)
		;
	if (form < 0)
		return;
	web_attr_text(form, "method", method, sizeof method);
	if (strcasecmp(method, "get")) {
		snprintf(status, sizeof status, "this form is sent as a post, which this browser cannot do");
		draw_status();
		return;
	}
	/* (the form's elements are the nodes after it, up to the first that is not inside it) */
	for (end = form + 1; end < web_node_count; end++) {
		for (i = end; i >= 0 && i != form; i = web_nodes[i].c.parent)
			;
		if (i < 0)
			break;
	}
	for (i = form + 1; i < end; i++) {
		const char *type;
		struct field *f;

		if (!web_is_field(i) || !web_attr_text(i, "name", name, sizeof name) || !name[0])
			continue;
		type = field_type(i);
		f = field_of(i);
		if (field_is_button(type)) {
			if (i != pressed)
				continue;
		} else if ((!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) && !f->checked) {
			continue;
		}
		n = f->n;
		memcpy(wide, f->text, n * sizeof *wide);
		if (!strcasecmp(type, "select")) {
			int kid, k = 0;

			n = 0;
			for (kid = web_nodes[i].first; kid >= 0; kid = web_nodes[kid].next)
				if (web_nodes[kid].c.tag == T_option && k++ == f->option) {
					if (web_attr_text(kid, "value", value, sizeof value)) {
						for (v = value, left = strlen(value); left > 0 && n < 127; v += used, left -= used)
							wide[n++] = web_char((const unsigned char *)v, left, &used);
					} else if (web_nodes[kid].first >= 0) {
						n = web_least(web_nodes[web_nodes[kid].first].text_n, 127);
						memcpy(wide, web_u + web_nodes[web_nodes[kid].first].text, n * sizeof *wide);
					}
				}
		} else if (!n && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) {
			wide[0] = 'o', wide[1] = 'n', n = 2;
		}
		query_add(query, sizeof query, name, wide, n);
	}
	if (!web_attr_text(form, "action", action, sizeof action) || !action[0])
		snprintf(action, sizeof action, "%s", address);
	if ((cut = strpbrk(action, "?#")) != NULL)
		*cut = 0;
	if (strlen(action) + strlen(query) + 2 > sizeof action) {
		snprintf(status, sizeof status, "the form's address is longer than the host can be asked for");
		draw_status();
		return;
	}
	strcat(action, "?");
	strcat(action, query);
	if (web_resolve(action, whole))
		go(whole, 1);
	else {
		snprintf(status, sizeof status, "the form's address is longer than the host can be asked for");
		draw_status();
	}
}

/* A field drawn again where it is: in the pixmap and in the view. */
static void
field_again(int node)
{
	int i;

	for (i = 0; i < web_item_count; i++)
		if (web_items[i].kind == I_FIELD && web_items[i].node == node && tile_gpu && tile_drawn)
			draw_item(tile, &web_items[i], tile_top);
	if (!tile_gpu)
		tile_drawn = 0;
	show_view();
}

/* ---- the pointer and the keys ---- */

static void
click(int x, int y)
{
	int i, was = focus;

	if (y < BAR) {
		if (x < 3 + 3 * 55) {
			editing = 0;
			if (x < 55 && history_count) {
				int back = history[--history_count].top;

				go(history[history_count].address, 0);
				if (!loading) {
					top = goal = back;
					show_view();
				}
			} else if (x >= 55 && x < 110)
				go(HOME, 1);
			else if (x >= 110)
				go(address, 2);
			return;
		}
		editing = 1;
		strcpy(typed, address);
		draw_bar();
		return;
	}
	if (editing) {
		editing = 0;
		draw_bar();
	}
	if (x >= width - UI_SCROLL_W) {
		dragging = 1;
		scroll_to(ui_scroll_to(y, BAR, view_height(), web_page_h, view_height()), 1);
		return;
	}
	if (y >= height - STATUS)
		return;
	focus = -1;
	/* the thing drawn last at that place is the one on top */
	for (i = web_item_count - 1; i >= 0; i--) {
		const struct web_item *it = &web_items[i];
		int node = it->node, link, up;
		char href[600];

		if (it->kind == I_BOX || !ui_inside(x, y - BAR + top, it->x, it->y, it->w, it->h))
			continue;
		if (it->kind == I_FIELD) {
			const char *type = field_type(node);
			struct field *f = field_of(node);

			if (field_is_button(type)) {
				submit(node, node);
			} else if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) {
				f->checked = !f->checked;
				field_again(node);
			} else if (!strcasecmp(type, "select")) {
				int kid, options = 0;

				for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next)
					options += web_nodes[kid].c.tag == T_option;
				f->option = options ? (f->option + 1) % options : 0;
				field_again(node);
			} else {
				focus = node;
				field_again(node);
			}
			break;
		}
		for (up = node; up >= 0; up = web_nodes[up].c.parent) {
			if (web_nodes[up].c.tag == T_summary && web_nodes[up].c.parent >= 0) {
				web_nodes[web_nodes[up].c.parent].flags ^= N_OPEN;
				lay_out(0, 0);
				show_view();
				return;
			}
			if (web_nodes[up].c.tag == T_button) {
				submit(up, up);
				return;
			}
		}
		if ((link = web_link_of(node)) >= 0 && web_attr_text(link, "href", href, sizeof href)) {
			follow(href);
			return;
		}
		break;
	}
	if (was >= 0 && was != focus)
		field_again(was);
}

/* Moves the keyboard to the page's next link, or the one before, and brings it into view.
 * Past the last one no link has it, and the next press starts over. */
static void
pick_link(int back)
{
	int i, found = -1, seen = picked < 0, last = -1, y = 0;
	char href[600];

	for (i = 0; i < web_item_count; i++) {
		int link = web_items[i].kind == I_TEXT || web_items[i].kind == I_IMAGE ? web_link_of(web_items[i].node) : -1;

		if (link < 0 || link == last)
			continue;
		if (back) {
			if (link == picked)
				break;
			found = link, y = web_items[i].y;
		} else if (seen && link != picked) {
			found = link, y = web_items[i].y;
			break;
		}
		seen |= link == picked;
		last = link;
	}
	picked = found;
	tile_drawn = 0;
	if (picked >= 0) {
		if (web_attr_text(picked, "href", href, sizeof href))
			snprintf(status, sizeof status, "%.190s", href);
		draw_status();
		if (y < top || y + line_h > top + view_height())
			top = goal = y - view_height() / 3;
	}
	show_view();
}

/* Keys: with the address field or one of the page's fields open they are typed into it.
 * Otherwise arrows and the paging keys scroll, Tab goes from link to link and Enter follows,
 * Backspace goes back, F5 loads the page again, and Ctrl+L opens the address field. */
static void
key(int ch, int modifiers)
{
	int n = strlen(typed), view = view_height();

	/* a letter with Ctrl comes as the letter with the modifier, or as its control code */
	if ((modifiers & MWKMOD_CTRL) && (ch == 'l' || ch == 'L'))
		ch = 'l' & 0x1f;
	if (focus >= 0 && !editing) {
		struct field *f = field_of(focus);

		if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r') {
			submit(focus, -1);
			return;
		}
		if (ch == MWKEY_ESCAPE || ch == MWKEY_TAB) {
			int was = focus;

			focus = -1;
			field_again(was);
			return;
		}
		if ((ch == MWKEY_BACKSPACE || ch == 127) && f->n > 0)
			f->n--;
		else if (ch >= 32 && ch < 127 && f->n < 127)
			f->text[f->n++] = ch;
		else
			return;
		field_again(focus);
		return;
	}
	if (!editing) {
		switch (ch) {
		case MWKEY_TAB: pick_link(modifiers & MWKMOD_SHIFT); break;
		case MWKEY_ENTER:
			if (picked >= 0) {
				char href[600];

				if (web_attr_text(picked, "href", href, sizeof href))
					follow(href);
			}
			break;
		case MWKEY_F5: click(120, 10); break;
		case 'l' & 0x1f: click(width - 20, 10); break;
		case MWKEY_UP: scroll_to(goal - 3 * line_h, 0); break;
		case MWKEY_DOWN: scroll_to(goal + 3 * line_h, 0); break;
		case MWKEY_PAGEUP: scroll_to(goal - view + line_h, 0); break;
		case MWKEY_PAGEDOWN: case ' ': scroll_to(goal + view - line_h, 0); break;
		case MWKEY_HOME: scroll_to(0, 0); break;
		case MWKEY_END: scroll_to(web_page_h, 0); break;
		case MWKEY_BACKSPACE: click(10, 10); break;
		}
		return;
	}
	if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r') {
		char where[280];

		/* a bare name is a site */
		snprintf(where, sizeof where, "%s%s", typed[0] == '/' || strstr(typed, ":") ? "" : "https://", typed);
		where[255] = 0;
		if (!strchr(where + 8, '/') && where[0] != '/' && strlen(where) < 254)
			strcat(where, "/");
		go(where, 1);
		return;
	}
	if (ch == MWKEY_ESCAPE)
		editing = 0;
	else if ((ch == MWKEY_BACKSPACE || ch == 127) && n > 0)
		typed[n - 1] = 0;
	else if (ch >= 32 && ch < 127 && n < 250)
		typed[n] = ch, typed[n + 1] = 0;
	draw_bar();
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	GR_FONT_INFO info;
	int fd;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	verbose = getenv("NXWEB_TIME") != NULL;
	dump = getenv("NXWEB_DUMP") != NULL;
	int c, plain;

	GrGetFontInfo(ui_var, &info);
	/* (the same answer is what ui_width would ask for at the first label) */
	for (c = 0; c < 256; c++)
		ui_var_widths.of[c] = info.widths[c >= info.firstchar && c <= info.lastchar ? c : info.firstchar];
	ui_var_widths.known = 1;
	line_h = (info.height > ui_fh ? info.height : ui_fh) + 2;
	pictures = malloc(MAX_PICTURES * sizeof *pictures);
	fd = open("/dev/gpu", O_RDWR);
	if (fd >= 0) {
		void *words = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000);
		void *data = mmap(NULL, FETCH_MOST, PROT_READ, MAP_SHARED, fd, 0x016c0000);

		if (words != MAP_FAILED && data != MAP_FAILED) {
			regs = words;
			fetched = data;
		}
	}
	{
		/* the ROM's fonts, if the image has them and the window system draws them (its font
		 * of such a file says it has every character); else the window system's own */
		static unsigned char widths[256];
		GR_FONT_INFO sef;
		GR_FONT_ID test;

		plain = web_face_for(0, 0, CSS_EM);
		test = web_face(plain)->file ? GrCreateFontEx("/usr/share/font-sans-13.sef", 0, 0, NULL) : 0;

		memset(&sef, 0, sizeof sef);
		if (test)
			GrGetFontInfo(test, &sef);
		if (!test || sef.lastchar != 0xffff || getenv("NXWEB_PLAIN")) {
			fprintf(stderr, "nxweb: the window system's own font (no fonts of the ROM: tools/make_fonts.py, docs/fonts.md)\n");
			memcpy(widths, info.widths, sizeof widths);
			memset(web_faces, 0, sizeof web_faces);
			web_plain = widths;
			web_plain_height = info.height;
			web_plain_ascent = info.baseline;
		} else if (!strcmp(web_faces[plain].name, "font-sans-13.sef"))
			fonts[plain][0] = test;	/* (the page's plain face is this file: not made a second time) */
	}
	go(argc > 1 ? argv[1] : HOME, 0);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Web", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, WHITE);
	set_title();
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		GrGetNextEventTimeout(&event, top != goal ? 15 : loading ? 100 : by_hand ? 3000 : regs && wanted_open() ? 400 : 60000);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE && (event.update.width != width || event.update.height != height)) {
				int wider = event.update.width != width;

				width = event.update.width;
				height = event.update.height;
				if (wider)
					lay_out(1, 1);	/* (the sheets' media queries ask about the width) */
				tile_drawn = 0;
				draw_all();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event))
				scroll_to(goal + 3 * line_h * ui_wheel_sum(&event), 0);
			else {
				click(event.button.x, event.button.y);
				ui_moves(window, EVENTS, dragging);	/* (the pointer's moves, while the scroll bar is held) */
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = 0;
			ui_moves(window, EVENTS, 0);
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (dragging && (event.mouse.buttons & GR_BUTTON_L))
				scroll_to(ui_scroll_to(event.mouse.y, BAR, view_height(), web_page_h, view_height()), 1);
			else {
				dragging = 0;
				ui_moves(window, EVENTS, 0);
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
		ticked = event.type == GR_EVENT_TYPE_TIMEOUT && top == goal;
		if (top != goal)
			scroll_step();
		if (web_refresh[0] && !loading && sent_on < 3) {
			/* the page sends its reader on (three times in a row at most) */
			char where[256];

			strcpy(where, web_refresh);
			web_refresh[0] = 0;
			sent_on++;
			go(where, 0);
		}
		if (loading)
			check_reply();
		else if (by_hand && event.type == GR_EVENT_TYPE_TIMEOUT)
			ask(address, 0);
		else if (!by_hand && regs && (asking = next_wanted()) >= SHEET)
			ask(web_sheets[asking - SHEET].address, 2);
		else if (!by_hand && regs && asking >= 0)
			ask(pictures[asking].address, 1);
	}
}
