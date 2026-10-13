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
#include "registry.h"
#include "ui_files.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_glyph.h"
#include "web_layout.h"

#define MENU_H 20
#define BAR 26
#define TOP (MENU_H + BAR)	/* the window's row where the page begins */
#define STATUS 18
#define STATUS_CELL 96		/* the status line's right part: what is to come, or the page's size */
#define WIDTH 600
#define HEIGHT 440
#define TOOLS 5
#define TOOL_W 26
#define FIELD_X (3 + TOOLS * (TOOL_W + 3))
#define HOME "/usr/share/web-index.html"
#define HISTORY 16
#define BOOKMARKS 24		/* a menu of them fits the screen, and the registry stays small */
#define FETCH_MOST 0x40000	/* bytes the host hands over at a time */
#define PAGE_MOST (3 << 19)	/* of a page that are read */
#define SHEET_MOST (3 << 18)
#define REG_REQUEST 64		/* words of the control block: requests made, length of the address, kind, offset */
#define REG_REPLY 68		/* requests answered, bytes, status, a picture's width | height << 16 */
#define REG_ADDRESS 72		/* the address, 256 bytes */
#define REG_PIXMAPS (0x1800 / 4 + 28)	/* pixmaps the GPU keeps, counted by the screen's driver */
#define REG_HOST 15		/* bits 8-15, 16-23: the GPU's target as this host has it, in 16s of pixels (0: not said) */
#define MAX_PICTURES 160
#define TILE_BYTES (3200 * 1024)	/* of the GPU's memory for the page as drawn */

static GR_WINDOW_ID window;
static int width = WIDTH, height = HEIGHT;
static int line_h;

static char *page;		/* the HTML as it came */
static int page_length;
static char address[256], typed[256], status[200];
static int editing;		/* the address field has the keyboard */
static int offered;		/* its text is still the one it opened with: a typed character replaces it */
static int picked = -1;		/* the link the keyboard is on (Tab): its element, or -1 */
static struct { char address[256], title[60]; int top; } history[HISTORY];
static int history_count, history_at;	/* the pages of this session, and which of them is shown */
static struct bookmark { char address[256], title[60]; } bookmarks[BOOKMARKS];
static int bookmark_count;
static char home[256] = HOME;
static int load_pictures = 1;
static char sought[64];		/* what Find looks for */
static int finding;		/* it is being typed, in the status line */
static int found = -1, found_from;	/* the text it was found in (an item), and from which character */
static int menu_at = -1;	/* the menu of the bar that is open, or -1 */
static int shown_busy, shown_open;	/* as the Stop key and the status line were drawn last */
static int print;		/* NXWEB_PRINT: says what a key, a click or a menu's row left */
static char link_asked[256];	/* the link the right button's menu is about */
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
static int wanted_open(void);
static int bookmark_of(const char *where);

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
	return height - TOP - STATUS;
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

	if (!load_pictures)
		return -1;	/* (the options: the page says what each would have been) */
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

/* How wide `n` characters of a text are, from its character `from`. */
static int
item_width(const struct web_item *it, int from, int n)
{
	int i, w = 0;

	for (i = 0; i < n; i++)
		w += web_most(0, web_advance(it->face, web_u[it->a + from + i]));
	return w;
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
		if (found >= 0 && it == &web_items[found]) {
			/* what Find found: those characters again, white on the selection's colour */
			int n = web_least((int)strlen(sought), it->b - found_from), x = it->x + item_width(it, 0, found_from);

			ui_fill(to, x, y, item_width(it, found_from, n), it->h, UI_SELECTED);
			page_text(to, x, y, web_u + it->a + found_from, n, it->face, (it->flags & IF_ITALIC) != 0, 0xffffff);
		}
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

/* The pixmap the page is drawn in, for the window as it is now: as high as TILE_BYTES allow
 * and no higher than the GPU's target, whose rows are all it draws (720 in the Unity world).
 * The driver counts the pixmaps the GPU keeps; one it does not keep is no use (0). */
static int
tile_make(void)
{
	GR_FONT_INFO info;
	uint32_t before = regs ? regs[REG_PIXMAPS] : 0;
	int w = view_width(), h = TILE_BYTES / (4 * w);
	int most = regs && (regs[REG_HOST] >> 16 & 255) ? (regs[REG_HOST] >> 16 & 255) * 16 : 2048;

	if (tile_w == w)
		return tile_gpu;	/* (made, or tried in vain, for this width already) */
	if (tile)
		GrDestroyWindow(tile);
	tile = 0;
	tile_drawn = 0;
	tile_gpu = 0;
	tile_w = w;
	tile_h = h > most ? most : h;
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

static const char *const menu_names[] = { "File", "Edit", "View", "Go", "Bookmarks", "Help" };
#define MENUS 6
static int menu_x[MENUS + 1];

static void
draw_menus(void)
{
	int i;

	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < MENUS; i++) {
		if (i == menu_at)
			ui_fill(window, menu_x[i], 1, menu_x[i + 1] - menu_x[i], MENU_H - 3, UI_SELECTED);
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, i == menu_at ? WHITE : BLACK, 0);
	}
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
}

/* A key of the bar: back, forward, again (Stop while something is to come), home, bookmark. */
static void
draw_tool(int i)
{
	static const int signs[TOOLS] = { UI_G_LEFT, UI_G_RIGHT, UI_G_AGAIN, UI_G_HOME, UI_G_MARK };
	int how = i == 0 ? (history_at < 1) * 2 : i == 1 ? (history_at + 1 >= history_count) * 2 : i == 4 && bookmark_of(address) >= 0;

	ui_tool(window, 3 + i * (TOOL_W + 3), MENU_H + 3, TOOL_W, BAR - 6, i == 2 && shown_busy ? UI_G_STOP : signs[i], how);
}

/* The address, or the one being typed: its end, when the field does not hold it all. */
static void
draw_address(void)
{
	const char *text = editing ? typed : address;
	int x = FIELD_X, y = MENU_H + 3, w = width - x - 3, room = w - 12, tw;

	ui_fill(window, x, y, w, BAR - 6, WHITE);
	ui_bevel(window, x, y, w, BAR - 6, 1);
	while (editing && text[0] && ui_width(&ui_var_widths, ui_var, text, -1) > room)
		text++;
	tw = ui_width(&ui_var_widths, ui_var, text, -1);
	if (editing && offered)
		ui_fill(window, x + 3, y + 2, (tw < room ? tw : room) + 4, BAR - 10, UI_SELECTED);
	ui_text_fit(window, x + 5, y + 4, room, text, editing && offered ? WHITE : BLACK);
	if (editing && !offered)
		ui_fill(window, x + 5 + tw, y + 3, 1, BAR - 12, BLACK);
}

static void
draw_bar(void)
{
	int i;

	ui_fill(window, 0, MENU_H, width, BAR, UI_FACE);
	for (i = 0; i < TOOLS; i++)
		draw_tool(i);
	draw_address();
}

/* What the browser has to say, or what Find is being told, and in the right part what is
 * still to come of the page, or how much of it there was. */
static void
draw_status(void)
{
	char text[240], cell[40];
	int y = height - STATUS, w = width - STATUS_CELL - 4;

	if (finding)
		snprintf(text, sizeof text, "Find: %s_   (Enter finds, Escape leaves it)", sought);
	else
		snprintf(text, sizeof text, "%s", status[0] ? status : "Done");
	if (shown_busy)
		snprintf(cell, sizeof cell, shown_open ? "%d to come" : "Loading...", shown_open);
	else
		snprintf(cell, sizeof cell, page_length < 10000 ? "%d bytes" : "%d KB", page_length < 10000 ? page_length : page_length / 1000);
	ui_fill(window, 0, y, width, STATUS, UI_FACE);
	ui_bevel(window, 1, y + 1, w, STATUS - 2, 1);
	ui_bevel(window, w + 2, y + 1, STATUS_CELL, STATUS - 2, 1);
	ui_text_fit(window, 6, y + 3, w - 10, text, BLACK);
	ui_text_fit(window, w + 7, y + 3, STATUS_CELL - 10, cell, BLACK);
}

/* Says something in the status line. */
static void
note(const char *text)
{
	snprintf(status, sizeof status, "%.199s", text);
	if (window)
		draw_status();
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
		draw_rows(window, top - TOP, top, top + view);
		draw_menus();
		draw_bar();
		draw_status();
	} else {
		if (!tile_drawn || top < tile_top || top + view > tile_top + tile_h) {
			/* the view in its middle, or nearer the way it is going: never out of a low pixmap */
			int spare = tile_h - view, lead = view / 2 < spare / 4 ? view / 2 : spare / 4;

			tile_top = top - spare / 2 + (goal > top ? lead : goal < top ? -lead : 0);
			tile_top = tile_top > web_page_h - tile_h ? web_page_h - tile_h : tile_top;
			tile_top = tile_top < 0 ? 0 : tile_top;
			draw_rows(tile, tile_top, tile_top, tile_top + tile_h);
			tile_drawn = 1;
		}
		GrCopyArea(window, ui_gc, 0, TOP, view_width(), view, tile, 0, top - tile_top, MWROP_COPY);
	}
	ui_scrollbar(window, width - UI_SCROLL_W, TOP, view, web_page_h, view, top);
}

static void
draw_all(void)
{
	draw_menus();
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
	found = -1;
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
	snprintf(history[history_at].title, sizeof history[0].title, "%.59s", web_title);
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
	char html[1100];
	int n = snprintf(html, sizeof html, "<body style=\"font-family:sans-serif\"><h2>%s</h2><p>%s</p><p><a href=\"%s\">Home</a></p>", heading_text, text, home);

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

/* Help's page (about:keys). */
static const char keys_page[] =
	"<html><head><title>The browser's keys</title></head><body style=\"font-family:sans-serif\"><h2>The browser's keys</h2>"
	"<table cellpadding=3>"
	"<tr><td><b>Ctrl+L</b>, <b>F6</b><td>the address: type one and press Enter (Escape leaves it)"
	"<tr><td><b>Alt+Left</b>, <b>Backspace</b><td>back"
	"<tr><td><b>Alt+Right</b><td>forward"
	"<tr><td><b>Alt+Home</b><td>the home page (View, Options says which)"
	"<tr><td><b>F5</b>, <b>Ctrl+R</b><td>the page again, from the network"
	"<tr><td><b>Escape</b><td>stops what is still to come"
	"<tr><td><b>Ctrl+D</b><td>a bookmark for this page, or none any more"
	"<tr><td><b>Ctrl+F</b>, <b>F3</b><td>find in the page, and the next place"
	"<tr><td><b>Ctrl+O</b>, <b>Ctrl+S</b><td>open a file, save the page as one"
	"<tr><td><b>Ctrl+U</b><td>the page's source, in the editor"
	"<tr><td><b>Ctrl+C</b>, <b>Ctrl+V</b><td>the address to the clipboard, an address from it"
	"<tr><td><b>Tab</b>, <b>Enter</b><td>from link to link, and follow the one picked"
	"<tr><td>arrows, <b>Page Up</b>, <b>Page Down</b>, <b>Space</b>, <b>Home</b>, <b>End</b><td>scroll"
	"<tr><td><b>F10</b><td>the menus: arrows, Enter, Escape"
	"<tr><td><b>Ctrl+Q</b><td>close"
	"</table><p>The right button on a link or on the page has a menu of its own. "
	"Bookmarks and options are kept in the registry, which lasts when the machine's storage is saved.</p>";

/* Goes to an address. `how`: 1 remembers the page left behind, 2 asks the host even for a
 * page the image keeps a copy of. */
static void
go(const char *where, int how)
{
	char file[80], plain[256];
	char *hash;

	if (how)
		sent_on = 0;
	if (how == 1 && history_count) {
		/* a new page: the ones Forward would have gone to are forgotten */
		history[history_at].top = top;
		history_count = history_at + 1;
		if (history_count == HISTORY)
			memmove(&history[0], &history[1], sizeof history[0] * --history_count);
		history_at = history_count++;
		history[history_at].title[0] = 0;
	}
	history_count += !history_count;
	snprintf(address, sizeof address, "%s", strncmp(where, "file://", 7) ? where : where + 7);
	strcpy(history[history_at].address, address);
	editing = finding = loading = by_hand = 0;
	picked = -1;
	if (part_file)
		fclose(part_file);
	part_file = NULL;
	snprintf(plain, sizeof plain, "%s", address);
	if ((hash = strchr(plain, '#')) != NULL)
		*hash = 0;
	if (!strncmp(address, "about:", 6)) {
		show(keys_page, sizeof keys_page - 1, "");
		return;
	}
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

/* ---- what is kept ---- */

/* The options and the bookmarks are keys of the desktop's registry (registry.h): web.home,
 * web.pictures, web.bookmarks (how many) and web.bookmark.N, each "ADDRESS TITLE". */
static void
kept_read(void)
{
	char key[32], *space;
	int i, count = reg_int("web.bookmarks", 0);

	snprintf(home, sizeof home, "%s", reg_get("web.home", HOME));
	load_pictures = reg_int("web.pictures", 1) != 0;
	for (i = 0; i < count && bookmark_count < BOOKMARKS; i++) {
		struct bookmark *b = &bookmarks[bookmark_count];

		snprintf(key, sizeof key, "web.bookmark.%d", i);
		snprintf(b->address, sizeof b->address, "%s", reg_get(key, ""));
		if ((space = strchr(b->address, ' ')) != NULL)
			*space++ = 0;
		snprintf(b->title, sizeof b->title, "%s", space ? space : "");
		bookmark_count += b->address[0] != 0;
	}
}

static int
options_write(void)
{
	return reg_set("web.home", home) & reg_set_int("web.pictures", load_pictures);
}

/* Writes the bookmarks from number `from` on, and how many there are; one more key than
 * there are bookmarks is taken out (the last, when one was taken away). */
static int
bookmarks_write(int from)
{
	char key[32], value[340];
	int i, fine = 1;

	for (i = from; i < bookmark_count; i++) {
		snprintf(key, sizeof key, "web.bookmark.%d", i);
		snprintf(value, sizeof value, "%s%s%.50s", bookmarks[i].address, bookmarks[i].title[0] ? " " : "", bookmarks[i].title);
		fine &= reg_set(key, value);
	}
	snprintf(key, sizeof key, "web.bookmark.%d", bookmark_count);
	reg_set(key, NULL);
	return fine & reg_set_int("web.bookmarks", bookmark_count);
}

/* The bookmark of an address, or -1. */
static int
bookmark_of(const char *where)
{
	int i;

	for (i = 0; i < bookmark_count; i++)
		if (!strcmp(bookmarks[i].address, where))
			return i;
	return -1;
}

/* Adds a bookmark for an address; with `toggle`, one that is there is taken away instead. */
static void
bookmark(const char *where, const char *title, int toggle)
{
	int at = bookmark_of(where);
	const char *did = "a bookmark is added";

	if (at >= 0 && !toggle) {
		note("that link has a bookmark already");
		return;
	}
	if (at < 0 && bookmark_count == BOOKMARKS) {
		note("there is room for 24 bookmarks: take one away first (Bookmarks, Remove)");
		return;
	}
	if (at < 0 && (strchr(where, ' ') || strlen(where) > 180)) {
		note("an address with a space in it, or one over 180 characters, gets no bookmark");
		return;
	}
	if (at >= 0) {
		memmove(&bookmarks[at], &bookmarks[at + 1], sizeof bookmarks[0] * (--bookmark_count - at));
		did = "the bookmark is taken away";
	} else {
		at = bookmark_count++;
		snprintf(bookmarks[at].address, sizeof bookmarks[0].address, "%s", where);
		snprintf(bookmarks[at].title, sizeof bookmarks[0].title, "%s", title);
	}
	snprintf(status, sizeof status, "%s%s", did, bookmarks_write(at) ? "" : " (not kept: /persistence cannot be written)");
	draw_status();
	draw_tool(4);
}

/* ---- what the menus' rows do ---- */

static int
busy(void)
{
	return loading || by_hand || (regs && wanted_open());
}

/* Gives up what the host has not sent yet: the page, or its sheets and pictures. */
static void
stop(void)
{
	int i;

	loading = by_hand = 0;
	for (i = 0; i < picture_count; i++)
		if (pictures[i].state == PICTURE_WANTED || pictures[i].state == PICTURE_BY_HAND)
			pictures[i].state = PICTURE_FAILED;
	for (i = 0; i < web_sheet_count; i++)
		if (web_sheets[i].state == SHEET_WANTED || web_sheets[i].state == SHEET_BY_HAND)
			web_sheets[i].state = SHEET_FAILED;
	if (part_file)
		fclose(part_file);
	part_file = NULL;
	asking = -1;
	note("stopped");
}

/* The page laid out anew with its pictures looked for again, or with none (the options). */
static void
pictures_again(void)
{
	int i;

	for (i = 0; i < picture_count; i++)
		if (pictures[i].state == PICTURE_HERE && pictures[i].ours)
			unlink(pictures[i].file);
	if (loading && asking >= 0 && asking < SHEET) {
		loading = 0;
		if (part_file)
			fclose(part_file);
		part_file = NULL;
		asking = -1;
	}
	picture_count = 0;
	lay_out(0, 0);
	top = goal = top > web_page_h - view_height() ? web_page_h - view_height() : top;
	show_view();
}

/* The next place the page's text has what is sought, from the last one found or from the
 * view's top, and round to the page's start. A place is inside one text of one face. */
static void
find_next(void)
{
	int n = strlen(sought), item = found, from = found_from + 1, tried, i, j;

	if (!n || !web_item_count)
		return;
	if (item < 0)
		for (item = from = 0; item < web_item_count - 1 && web_items[item].y + web_items[item].h <= top; item++)
			;
	for (tried = 0; tried <= web_item_count; tried++, item = (item + 1) % web_item_count, from = 0) {
		const struct web_item *it = &web_items[item];

		for (i = from; it->kind == I_TEXT && i + n <= it->b; i++) {
			for (j = 0; j < n && web_u[it->a + i + j] < 128 && tolower(web_u[it->a + i + j]) == tolower((unsigned char)sought[j]); j++)
				;
			if (j < n)
				continue;
			found = item, found_from = i;
			if (it->y < top || it->y + it->h > top + view_height())
				top = goal = it->y - view_height() / 3;
			snprintf(status, sizeof status, "found \"%s\" (F3: the next place)", sought);
			tile_drawn = 0;
			show_view();
			draw_status();
			return;
		}
	}
	snprintf(status, sizeof status, "\"%s\" is not in the page's text", sought);
	if (found >= 0) {
		found = -1;
		tile_drawn = 0;
		show_view();
	}
	draw_status();
}

/* A key of what Find is told, in the status line. */
static void
find_key(int ch)
{
	int n = strlen(sought);

	if (ch == MWKEY_ESCAPE) {
		finding = 0;
	} else if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r') {
		finding = 0;
		find_next();
		return;
	} else if (ch == MWKEY_BACKSPACE || ch == 127) {
		if (n)
			sought[n - 1] = 0;
	} else if (ch >= 32 && ch < 127 && n < (int)sizeof sought - 1) {
		sought[n] = ch, sought[n + 1] = 0;
	}
	draw_status();
}

/* Opens the address field with a text in it, which `select` shows as one to type over. */
static void
field_open(const char *text, int select)
{
	int was = focus, told = finding;

	snprintf(typed, sizeof typed, "%.250s", text);
	editing = 1;
	offered = select;
	finding = 0;
	focus = -1;
	if (was >= 0)
		field_again(was);
	draw_address();
	if (told)
		draw_status();
}

/* What was typed as an address, as one to go to: a bare name is a site. */
static void
typed_address(const char *text, char *where, int room)
{
	snprintf(where, room, "%s%s", text[0] == '/' || strstr(text, ":") ? "" : "https://", text);
	where[255] = 0;
	if (where[0] != '/' && strlen(where) < 254 && (strlen(where) < 8 || !strchr(where + 8, '/')))
		strcat(where, "/");
}

/* To another page of this session's, where it was left. */
static void
travel(int to)
{
	history[history_at].top = top;
	history_at = to;
	go(history[to].address, 0);
	if (!loading) {
		top = goal = history[to].top;
		show_view();
	}
}

static void
resized(int w, int h)
{
	int wider = w != width;

	if (w == width && h == height)
		return;
	width = w;
	height = h;
	if (wider)
		lay_out(1, 1);	/* (the sheets' media queries ask about the width) */
	tile_drawn = 0;
	draw_all();
}

/* This window's events while the options or a file's window is up. */
static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_UPDATE && event->update.wid == window && event->update.utype == GR_UPDATE_SIZE)
		resized(event->update.width, event->update.height);
	else if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
}

/* ---- the options' window ---- */

#define OPT_W 400
#define OPT_H 150
enum { O_HOME, O_THIS, O_FIRST, O_PICTURES, O_OK, O_CANCEL, O_COUNT };
static const struct { int x, y, w, h; const char *label; } opt_at[O_COUNT] = {
	{ 84, 10, OPT_W - 94, 20, NULL }, { 84, 36, 100, 22, "This page" }, { 190, 36, 100, 22, "The first one" },
	{ 10, 72, 180, 15, NULL }, { OPT_W - 170, OPT_H - 32, 76, 22, "OK" }, { OPT_W - 86, OPT_H - 32, 76, 22, "Cancel" },
};
static struct { char home[256]; int pictures, focus; } opt;

static void
options_draw(GR_WINDOW_ID w)
{
	const char *text = opt.home;
	int i, j, room = opt_at[O_HOME].w - 12;

	ui_fill(w, 0, 0, OPT_W, OPT_H, UI_FACE);
	ui_text(w, 10, 14, "Home page", -1, BLACK, 0);
	ui_fill(w, opt_at[O_HOME].x, opt_at[O_HOME].y, opt_at[O_HOME].w, opt_at[O_HOME].h, WHITE);
	ui_bevel(w, opt_at[O_HOME].x, opt_at[O_HOME].y, opt_at[O_HOME].w, opt_at[O_HOME].h, 1);
	while (text[0] && ui_width(&ui_var_widths, ui_var, text, -1) > room)
		text++;
	ui_text(w, opt_at[O_HOME].x + 5, 14, text, -1, BLACK, 0);
	if (opt.focus == O_HOME)
		ui_fill(w, opt_at[O_HOME].x + 5 + ui_width(&ui_var_widths, ui_var, text, -1), 13, 1, 14, BLACK);
	for (i = O_THIS; i < O_COUNT; i++) {
		if (opt_at[i].label)
			ui_button(w, opt_at[i].x, opt_at[i].y, opt_at[i].w, opt_at[i].h, opt_at[i].label, 0);
		if (i == opt.focus)
			ui_focus(w, opt_at[i].x + 4, opt_at[i].y + 4, opt_at[i].w - 8, opt_at[i].h - 8);
	}
	/* a box with a tick in it when the choice is on */
	ui_fill(w, 14, 73, 13, 13, WHITE);
	ui_bevel(w, 14, 73, 13, 13, 1);
	GrSetGCForeground(ui_gc, BLACK);
	for (j = 0; opt.pictures && j < 7; j++)
		GrLine(w, ui_gc, 17 + j, 73 + (j < 3 ? 5 + j : 9 - j), 17 + j, 73 + (j < 3 ? 7 + j : 11 - j));
	ui_text(w, 34, 74, "Load the pages' pictures", -1, BLACK, 0);
	ui_text(w, 10, 98, "Kept in the registry, which lasts when the storage is saved.", -1, UI_SHADOW, 0);
}

/* The options, in a window of their own until OK or Cancel: the home page's address, typed,
 * and whether pictures are loaded. Tab goes round, Enter is OK, Escape is Cancel. */
static void
options(void)
{
	GR_WINDOW_ID w = GrNewWindowEx(UI_FIXED, "Options", GR_ROOT_WINDOW_ID, -1, -1, OPT_W, OPT_H, UI_FACE);
	GR_EVENT event;
	int done = 0, press, i, n, ch, x, y;

	snprintf(opt.home, sizeof opt.home, "%s", home);
	opt.pictures = load_pictures;
	opt.focus = O_HOME;
	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(w);
	GrRaiseWindow(w);
	GrSetFocus(w);
	while (!done) {
		GrGetNextEvent(&event);
		if (event.general.wid != w) {
			if (event.type != GR_EVENT_TYPE_NONE)
				others(&event);
			continue;
		}
		press = -1;
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (print) {	/* where it is, for a test to click in */
				ui_window_origin(w, &x, &y);
				printf("nxweb: options at %d,%d\n", x, y);
				fflush(stdout);
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			done = 2;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			for (i = 0; i < O_COUNT && !ui_wheel(&event); i++)
				if (ui_inside(event.button.x, event.button.y, opt_at[i].x, opt_at[i].y, opt_at[i].w, opt_at[i].h))
					press = opt.focus = i;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			ch = event.keystroke.ch;
			n = strlen(opt.home);
			if (ch == MWKEY_ESCAPE)
				done = 2;
			else if (ui_tab(&event, &opt.focus, O_COUNT))
				break;
			else if (ch == MWKEY_ENTER)
				press = opt.focus == O_HOME || opt.focus == O_PICTURES ? O_OK : opt.focus;
			else if (opt.focus != O_HOME)
				press = ch == ' ' ? opt.focus : -1;
			else if ((ch == MWKEY_BACKSPACE || ch == 127) && n)
				opt.home[n - 1] = 0;
			else if (ch >= 32 && ch < 127 && n < 250)
				opt.home[n] = ch, opt.home[n + 1] = 0;
			break;
		default:
			continue;
		}
		switch (press) {
		case O_THIS: snprintf(opt.home, sizeof opt.home, "%s", address); break;
		case O_FIRST: strcpy(opt.home, HOME); break;
		case O_PICTURES: opt.pictures = !opt.pictures; break;
		case O_OK: done = 1; break;
		case O_CANCEL: done = 2; break;
		}
		if (!done)
			options_draw(w);
	}
	GrDestroyWindow(w);
	if (done != 1)
		return;
	if (opt.home[0]) {
		char where[280];

		typed_address(opt.home, where, sizeof where);
		strcpy(home, where);
	} else
		strcpy(home, HOME);
	i = opt.pictures != load_pictures;
	load_pictures = opt.pictures;
	note(options_write() ? "the options are in the registry" : "the options hold until the browser closes (/persistence cannot be written)");
	if (i)
		pictures_again();
}

/* ---- the menus ---- */

enum { C_ADDRESS = 1, C_OPEN, C_SAVE, C_CLOSE, C_COPY, C_PASTE, C_FIND, C_NEXT, C_STOP, C_RELOAD, C_PICTURES, C_SOURCE, C_OPTIONS,
       C_BACK, C_FORWARD, C_HOME, C_MARK, C_KEYS, C_ABOUT, C_LINK, C_LINK_COPY, C_LINK_MARK,
       C_VISIT = 100, C_BOOKMARK = 200, C_REMOVE = 300 };	/* and from each of these three, a row a page */

/* A menu is made when it opens: what its rows say and which are grey depend on the page. */
static struct ui_menu_item rows[HISTORY + BOOKMARKS + 8], removals[BOOKMARKS];
static char labels[BOOKMARKS][56];
static int row_count;

static struct ui_menu_item *
row(const char *label, int id, int flags)
{
	struct ui_menu_item *r = &rows[row_count++];

	memset(r, 0, sizeof *r);
	r->label = label;
	r->icon = -1;
	r->id = id;
	r->flags = flags;
	return r;
}

/* A page as a row: its title, or its address when it has none. */
static const char *
page_label(int k, const char *title, const char *where)
{
	const char *text = title[0] ? title : where;

	snprintf(labels[k], sizeof labels[0], "%.48s%s", text, strlen(text) > 48 ? "..." : "");
	return labels[k];
}

static void
menu_make(int which)
{
	int i, marked = bookmark_of(address) >= 0, grey = UI_MENU_GREY;

	row_count = 0;
	switch (which) {
	case 0:
		row("Open address...   Ctrl+L", C_ADDRESS, 0);
		row("Open file...   Ctrl+O", C_OPEN, 0);
		row("Save page as...   Ctrl+S", C_SAVE, page_length ? 0 : grey);
		row("Close   Ctrl+Q", C_CLOSE, UI_MENU_LINE);
		break;
	case 1:
		row("Copy the address   Ctrl+C", C_COPY, 0);
		row("Paste an address   Ctrl+V", C_PASTE, 0);
		row("Find in page...   Ctrl+F", C_FIND, UI_MENU_LINE);
		row("Find next   F3", C_NEXT, sought[0] ? 0 : grey);
		break;
	case 2:
		row("Stop   Escape", C_STOP, busy() ? 0 : grey);
		row("Reload   F5", C_RELOAD, 0);
		row("Pictures", C_PICTURES, UI_MENU_LINE | (load_pictures ? UI_MENU_CHECK : 0));
		row("Page source   Ctrl+U", C_SOURCE, page_length ? 0 : grey);
		row("Options...", C_OPTIONS, UI_MENU_LINE);
		break;
	case 3:
		row("Back   Alt+Left", C_BACK, history_at > 0 ? 0 : grey);
		row("Forward   Alt+Right", C_FORWARD, history_at + 1 < history_count ? 0 : grey);
		row("Home   Alt+Home", C_HOME, 0);
		/* the session's pages, the newest first, a tick at the one shown */
		for (i = history_count - 1; i >= 0; i--)
			row(page_label(i, history[i].title, history[i].address), C_VISIT + i,
			    (i == history_count - 1 ? UI_MENU_LINE : 0) | (i == history_at ? UI_MENU_CHECK : 0));
		break;
	case 4:
		row(marked ? "Remove this page   Ctrl+D" : "Add this page   Ctrl+D", C_MARK, 0);
		row("Remove", 0, bookmark_count ? 0 : grey)->sub = removals;
		rows[1].sub_count = bookmark_count;
		for (i = 0; i < bookmark_count; i++) {
			row(page_label(i, bookmarks[i].title, bookmarks[i].address), C_BOOKMARK + i, i ? 0 : UI_MENU_LINE)->icon = ui_icon_of("page");
			removals[i] = rows[row_count - 1];
			removals[i].id = C_REMOVE + i;
			removals[i].flags = 0;
		}
		break;
	case 5:
		row("Keys", C_KEYS, 0);
		row("About this machine", C_ABOUT, 0);
		break;
	}
}

/* NXWEB_PRINT: the rows of the menu just opened, for a test to read. */
static void
menu_say(const char *name)
{
	int i;

	for (i = 0; print && i < row_count; i++)
		printf("nxweb: menu %s %d: %s%s%s\n", name, i, rows[i].label, (rows[i].flags & UI_MENU_GREY) ? " (grey)" : "",
		       (rows[i].flags & UI_MENU_CHECK) ? " (on)" : "");
}

/* Opens a menu of the bar under its name; `keyboard` picks out its first row. */
static void
menu_open(int which, int keyboard)
{
	int x, y;

	ui_window_origin(window, &x, &y);
	menu_make(which);
	menu_at = which;
	draw_menus();
	ui_menu_open(rows, row_count, x + menu_x[which], y + MENU_H, 0, keyboard);
	menu_say(menu_names[which]);
}

/* What a menu's row, a key of the bar, or the key that stands for one does. */
static void
command(int id)
{
	char chosen[512], name[128], again[256], *text;
	FILE *file;
	int n;

	switch (id) {
	case C_ADDRESS:
		field_open(address, 1);
		break;
	case C_PASTE:
		if ((text = ui_clip_get(&n)) == NULL) {
			note("the clipboard is empty");
			break;
		}
		text[strcspn(text, "\r\n\t")] = 0;
		field_open(text, 0);
		free(text);
		break;
	case C_COPY:
		ui_clip_set(address, strlen(address));
		note("the address is on the clipboard");
		break;
	case C_OPEN:
		if (ui_choose_file("Open a page", "/root", ".html .htm", chosen, sizeof chosen, others) && strlen(chosen) < sizeof address)
			go(chosen, 1);
		break;
	case C_SAVE:
		snprintf(name, sizeof name, "%.100s", strrchr(address, '/') ? strrchr(address, '/') + 1 : "");
		name[strcspn(name, "?#")] = 0;
		if (!strchr(name, '.'))
			strcpy(name, "page.html");
		if (!page_length || !ui_save_file("Save the page as", "/root", NULL, name, chosen, sizeof chosen, others))
			break;
		file = fopen(chosen, "wb");
		n = file && fwrite(page, 1, page_length, file) == (size_t)page_length;
		if (file && fclose(file))
			n = 0;
		if (n)
			snprintf(status, sizeof status, "saved as %.180s", chosen);
		else
			snprintf(status, sizeof status, "%.170s could not be written", chosen);
		draw_status();
		break;
	case C_CLOSE:
		GrClose();
		exit(0);
	case C_FIND:
		finding = 1;
		editing = 0;
		sought[0] = 0;
		draw_address();
		draw_status();
		break;
	case C_NEXT:
		find_next();
		break;
	case C_STOP:
		stop();
		break;
	case C_RELOAD:
		strcpy(again, address);
		go(again, 2);
		break;
	case C_PICTURES:
		load_pictures = !load_pictures;
		options_write();
		pictures_again();
		note(load_pictures ? "pictures are loaded" : "pictures are not loaded: the page says what each would be");
		break;
	case C_SOURCE:
		if (!page_length || (file = fopen("/tmp/nxweb-source.html", "wb")) == NULL)
			break;
		fwrite(page, 1, page_length, file);
		fclose(file);
		ui_run("nxedit", "/tmp/nxweb-source.html");
		break;
	case C_OPTIONS:
		options();
		break;
	case C_BACK:
		if (history_at > 0)
			travel(history_at - 1);
		break;
	case C_FORWARD:
		if (history_at + 1 < history_count)
			travel(history_at + 1);
		break;
	case C_HOME:
		go(home, 1);
		break;
	case C_MARK:
		bookmark(address, web_title, 1);
		break;
	case C_KEYS:
		go("about:keys", 1);
		break;
	case C_ABOUT:
		go("/usr/share/web-about.html", 1);
		break;
	case C_LINK:
		go(link_asked, 1);
		break;
	case C_LINK_COPY:
		ui_clip_set(link_asked, strlen(link_asked));
		note("the link's address is on the clipboard");
		break;
	case C_LINK_MARK:
		bookmark(link_asked, "", 0);
		break;
	default:
		if (id >= C_REMOVE && id < C_REMOVE + bookmark_count)
			bookmark(bookmarks[id - C_REMOVE].address, "", 1);
		else if (id >= C_BOOKMARK && id < C_BOOKMARK + bookmark_count)
			go(bookmarks[id - C_BOOKMARK].address, 1);
		else if (id >= C_VISIT && id < C_VISIT + history_count && id - C_VISIT != history_at)
			travel(id - C_VISIT);
	}
}

/* ---- the pointer and the keys ---- */

/* The address of the link at a place of the window, made whole; false where there is none. */
static int
link_at(int x, int y, char *whole)
{
	char href[600];
	int i, link;

	for (i = web_item_count - 1; i >= 0; i--) {
		const struct web_item *it = &web_items[i];

		if (it->kind == I_BOX || !ui_inside(x, y - TOP + top, it->x, it->y, it->w, it->h))
			continue;
		return (it->kind == I_TEXT || it->kind == I_IMAGE) && (link = web_link_of(it->node)) >= 0 &&
		       web_attr_text(link, "href", href, sizeof href) && href[0] != '#' && web_resolve(href, whole);
	}
	return 0;
}

/* The right button's menu: what can be done with the link under it, and with the page. */
static void
context(const GR_EVENT *event)
{
	int x = event->button.x, y = event->button.y, has;

	if (y < TOP || y >= height - STATUS || x >= width - UI_SCROLL_W)
		return;
	has = link_at(x, y, link_asked);
	row_count = 0;
	if (has) {
		row("Open the link", C_LINK, 0);
		row("Copy the link's address", C_LINK_COPY, 0);
		row("Bookmark the link", C_LINK_MARK, 0);
		note(link_asked);
	}
	row("Back", C_BACK, (has ? UI_MENU_LINE : 0) | (history_at > 0 ? 0 : UI_MENU_GREY));
	row("Forward", C_FORWARD, history_at + 1 < history_count ? 0 : UI_MENU_GREY);
	row("Reload", C_RELOAD, 0);
	row(bookmark_of(address) >= 0 ? "Remove this page's bookmark" : "Bookmark this page", C_MARK, UI_MENU_LINE);
	row("Page source", C_SOURCE, page_length ? 0 : UI_MENU_GREY);
	ui_menu_open(rows, row_count, event->button.rootx, event->button.rooty, 0, 0);
	menu_say("pointer");
}

static void
click(int x, int y)
{
	static const int tools[TOOLS] = { C_BACK, C_FORWARD, C_RELOAD, C_HOME, C_MARK };
	int i, was = focus;

	if (y < MENU_H) {
		for (i = 0; i < MENUS; i++)
			if (x >= menu_x[i] && x < menu_x[i + 1])
				menu_open(i, 0);
		return;
	}
	if (y < TOP) {
		if (x < FIELD_X) {
			i = (x - 3) / (TOOL_W + 3);
			if (x >= 3 && i < TOOLS && (x - 3) % (TOOL_W + 3) < TOOL_W)
				command(i == 2 && busy() ? C_STOP : tools[i]);
		} else if (editing) {
			offered = 0;	/* a second click: the text is to be added to */
			draw_address();
		} else
			field_open(address, 1);
		return;
	}
	if (editing) {
		editing = 0;
		draw_address();
	}
	if (finding) {
		finding = 0;
		draw_status();
	}
	if (x >= width - UI_SCROLL_W) {
		dragging = 1;
		scroll_to(ui_scroll_to(y, TOP, view_height(), web_page_h, view_height()), 1);
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

		if (it->kind == I_BOX || !ui_inside(x, y - TOP + top, it->x, it->y, it->w, it->h))
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
	int i, found_link = -1, seen = picked < 0, last = -1, y = 0;
	char href[600];

	for (i = 0; i < web_item_count; i++) {
		int link = web_items[i].kind == I_TEXT || web_items[i].kind == I_IMAGE ? web_link_of(web_items[i].node) : -1;

		if (link < 0 || link == last)
			continue;
		if (back) {
			if (link == picked)
				break;
			found_link = link, y = web_items[i].y;
		} else if (seen && link != picked) {
			found_link = link, y = web_items[i].y;
			break;
		}
		seen |= link == picked;
		last = link;
	}
	picked = found_link;
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

/* The command a letter with Ctrl is, or 0. */
static int
command_of(int letter)
{
	static const char letters[] = "losqwcvfdur";
	static const int ids[] = { C_ADDRESS, C_OPEN, C_SAVE, C_CLOSE, C_CLOSE, C_COPY, C_PASTE, C_FIND, C_MARK, C_SOURCE, C_RELOAD };
	const char *at = letter ? strchr(letters, letter) : NULL;

	return at ? ids[at - letters] : 0;
}

/* Keys: the menus' own (they say which), then with the address field or one of the page's
 * fields open they are typed into it. Otherwise arrows and the paging keys scroll, Tab goes
 * from link to link and Enter follows, Backspace goes back, Escape stops. */
static void
key(int ch, int modifiers)
{
	int n = strlen(typed), view = view_height(), letter = 0;

	if (finding) {
		find_key(ch);
		return;
	}
	/* a letter with Ctrl comes as the letter with the modifier, or as its control code */
	if ((modifiers & MWKMOD_CTRL) && ch < 128 && isalpha(ch))
		letter = tolower(ch);
	else if (ch >= 1 && ch <= 26 && ch != MWKEY_BACKSPACE && ch != MWKEY_TAB && ch != '\n' && ch != MWKEY_ENTER)
		letter = 'a' + ch - 1;
	if (letter) {
		if (command_of(letter))
			command(command_of(letter));
		return;
	}
	switch (ch) {
	case MWKEY_F3: command(C_NEXT); return;
	case MWKEY_F5: command(C_RELOAD); return;
	case MWKEY_F6: command(C_ADDRESS); return;
	case MWKEY_F10: menu_open(0, 1); return;
	case MWKEY_LEFT: case MWKEY_RIGHT: case MWKEY_HOME:
		if (!(modifiers & MWKMOD_ALT))
			break;
		command(ch == MWKEY_LEFT ? C_BACK : ch == MWKEY_RIGHT ? C_FORWARD : C_HOME);
		return;
	}
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
		case MWKEY_UP: scroll_to(goal - 3 * line_h, 0); break;
		case MWKEY_DOWN: scroll_to(goal + 3 * line_h, 0); break;
		case MWKEY_PAGEUP: scroll_to(goal - view + line_h, 0); break;
		case MWKEY_PAGEDOWN: case ' ': scroll_to(goal + view - line_h, 0); break;
		case MWKEY_HOME: scroll_to(0, 0); break;
		case MWKEY_END: scroll_to(web_page_h, 0); break;
		case MWKEY_BACKSPACE: command(C_BACK); break;
		case MWKEY_ESCAPE:
			if (busy())
				stop();
			else if (found >= 0) {
				found = -1;
				tile_drawn = 0;
				show_view();
			}
			break;
		}
		return;
	}
	if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r') {
		char where[280];

		typed_address(typed, where, sizeof where);
		go(where, 1);
		return;
	}
	if (ch == MWKEY_ESCAPE)
		editing = 0;
	else if (ch == MWKEY_BACKSPACE || ch == 127)
		typed[n ? n - 1 : 0] = 0;
	else if (ch >= 32 && ch < 127 && n < 250) {
		n = offered ? 0 : n;
		typed[n] = ch, typed[n + 1] = 0;
	}
	offered = 0;
	draw_address();
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	GR_FONT_INFO info;
	int fd, id;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	signal(SIGCHLD, SIG_IGN);	/* (the editor, started for a page's source) */
	verbose = getenv("NXWEB_TIME") != NULL;
	dump = getenv("NXWEB_DUMP") != NULL;
	print = getenv("NXWEB_PRINT") != NULL;
	int c, plain;

	GrGetFontInfo(ui_var, &info);
	/* (the same answer is what ui_width would ask for at the first label) */
	for (c = 0; c < 256; c++)
		ui_var_widths.of[c] = info.widths[c >= info.firstchar && c <= info.lastchar ? c : info.firstchar];
	ui_var_widths.known = 1;
	line_h = (info.height > ui_fh ? info.height : ui_fh) + 2;
	for (c = 0, menu_x[0] = 2; c < MENUS; c++)
		menu_x[c + 1] = menu_x[c] + ui_width(&ui_var_widths, ui_var, menu_names[c], -1) + 16;
	kept_read();
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
	go(argc > 1 ? argv[1] : home, 0);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Web", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, WHITE);
	set_title();
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		int menu, open;

		GrGetNextEventTimeout(&event, top != goal ? 15 : loading ? 100 : by_hand ? 3000 : regs && wanted_open() ? 400 : 60000);
		ui_origin_note(&event);
		if (menu_at >= 0 && ui_menu_depth == 1 && event.type == GR_EVENT_TYPE_KEY_DOWN &&
		    (event.keystroke.ch == MWKEY_LEFT || (event.keystroke.ch == MWKEY_RIGHT &&
		     !(ui_menu_level[0].hover >= 0 && ui_menu_level[0].items[ui_menu_level[0].hover].sub)))) {
			/* Left and Right in a menu of the bar: the menu beside it */
			menu_open((menu_at + (event.keystroke.ch == MWKEY_LEFT ? MENUS - 1 : 1)) % MENUS, 1);
			menu = UI_MENU_TOOK;
		} else
			menu = ui_menu_event(&event, &id);
		if (menu == UI_MENU_CHOSEN || menu == UI_MENU_CLOSED) {
			int was = menu_at;

			menu_at = -1;
			if (was >= 0)
				draw_menus();
			GrSetFocus(window);
			if (menu == UI_MENU_CHOSEN)
				command(id);
		}
		switch (menu ? GR_EVENT_TYPE_NONE : event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE && event.update.wid == window)
				resized(event.update.width, event.update.height);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event))
				scroll_to(goal + 3 * line_h * ui_wheel_sum(&event), 0);
			else if (event.button.changebuttons & GR_BUTTON_R)
				context(&event);
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
				scroll_to(ui_scroll_to(event.mouse.y, TOP, view_height(), web_page_h, view_height()), 1);
			else {
				dragging = 0;
				ui_moves(window, EVENTS, 0);
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid != window)	/* (a menu's may come after the menu went) */
				break;
			GrClose();
			return 0;
		}
		if (print && (menu == UI_MENU_CHOSEN || (!menu && (event.type == GR_EVENT_TYPE_KEY_DOWN || event.type == GR_EVENT_TYPE_BUTTON_DOWN)))) {
			int i;

			printf("nxweb: at %s, page %d of %d, top %d, %d bookmarks, home %s, pictures %d, typing %d \"%s\", status \"%s\"\n", address,
			       history_at + 1, history_count, top, bookmark_count, home, load_pictures, editing, editing ? typed : "", status);
			/* where the link the keyboard is on stands in the window, for a test to click */
			for (i = 0; picked >= 0 && i < web_item_count; i++)
				if (web_items[i].kind == I_TEXT && web_link_of(web_items[i].node) == picked) {
					printf("nxweb: the link picked is at %d,%d\n", web_items[i].x + 2, web_items[i].y - top + TOP + 2);
					break;
				}
			fflush(stdout);
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
		/* the Stop key and the status line's right part say what is still to come */
		open = regs ? wanted_open() : 0;
		if ((loading || by_hand || open) != shown_busy || open != shown_open) {
			shown_busy = loading || by_hand || open;
			shown_open = open;
			draw_tool(2);
			draw_status();
		}
	}
}
