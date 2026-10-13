/*
 * nxedit: a text editor. nxedit FILE opens or creates the file; without one the text has no
 * file until Save asks for a name. Its menus (File, Edit, and the right button's) say the
 * keys. A text with changes is asked about before it is let go: Save, Don't save, Cancel.
 */
#define _GNU_SOURCE
#include "ui.h"
#include "ui_files.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include <errno.h>

#define MENU_H 20
#define STATUS 18
#define MARGIN 3
#define UNDO_MOST 512
/* (the pointer's moves are asked for while a button is held: ui_moves) */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_BUTTON_DOWN | \
	GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ)

static char **lines;		/* each allocated, no newline */
static int count, room;
static int cx, cy;		/* the cursor: column, line */
static int ax, ay, marked;	/* where a selection began, when there is one: it ends at the cursor */
static int top, left;		/* first line and column shown */
static int rows, columns;	/* that fit the window */
static int dragging;		/* the scroll bar's thumb is held */
static int selecting;		/* the pointer is held in the text */
static int width, height, modified;
static char path[512];
static int named;		/* the text has a file: Save writes it without asking */
static char notice[200];
static GR_WINDOW_ID window;
static char sought[64];		/* what Ctrl+F looks for */
static char line_asked[12];	/* the line Ctrl+G goes to */
enum { ASK_NONE, ASK_FIND, ASK_LINE };
static int asking;		/* one of them is being typed, in the status line */

enum { C_NEW = 1, C_OPEN, C_SAVE, C_SAVE_AS, C_EXIT, C_UNDO, C_REDO, C_CUT, C_COPY, C_PASTE, C_DELETE, C_ALL, C_FIND, C_NEXT, C_LINE };
static const char *const menu_names[] = { "File", "Edit" };
static int menu_x[3];
static const struct ui_menu_item file_items[] = {
	{ "New   Ctrl+N", -1, C_NEW }, { "Open...   Ctrl+O", -1, C_OPEN }, { "Save   Ctrl+S", -1, C_SAVE }, { "Save as...", -1, C_SAVE_AS },
	{ "Exit   Ctrl+Q", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item edit_items[] = {
	{ "Undo   Ctrl+Z", -1, C_UNDO }, { "Redo   Ctrl+Y", -1, C_REDO }, { "Cut   Ctrl+X", -1, C_CUT, NULL, 0, UI_MENU_LINE },
	{ "Copy   Ctrl+C", -1, C_COPY }, { "Paste   Ctrl+V", -1, C_PASTE }, { "Delete", -1, C_DELETE },
	{ "Select all   Ctrl+A", -1, C_ALL }, { "Find...   Ctrl+F", -1, C_FIND, NULL, 0, UI_MENU_LINE }, { "Find next   F3", -1, C_NEXT },
	{ "Go to line...   Ctrl+G", -1, C_LINE },
};
static const struct ui_menu_item pointer_items[] = {
	{ "Cut", -1, C_CUT }, { "Copy", -1, C_COPY }, { "Paste", -1, C_PASTE }, { "Delete", -1, C_DELETE },
	{ "Select all", -1, C_ALL, NULL, 0, UI_MENU_LINE },
};
static const struct { const struct ui_menu_item *items; int count; } menus[2] = { { file_items, 5 }, { edit_items, 10 } };

/* What was done, to undo: text put in from (x0, y0) to (x1, y1), or text taken out at (x0, y0).
 * A change taken back is kept the same way, to be made again. */
struct change { int inserted, x0, y0, x1, y1, length, joined; char *text; };
static struct change undo[UNDO_MOST], redo[UNDO_MOST];
static int undo_count, redo_count;
static int turning;		/* 1: a change is being taken back, 2: made again */

static void
say(const char *text)
{
	snprintf(notice, sizeof notice, "%s", text);
}

static void
insert_line(int at, const char *text, int length)
{
	if (count == room) {
		room = room ? room * 2 : 64;
		lines = realloc(lines, room * sizeof *lines);
	}
	memmove(lines + at + 1, lines + at, (count - at) * sizeof *lines);
	lines[at] = malloc(length + 1);
	memcpy(lines[at], text, length);
	lines[at][length] = 0;
	count++;
}

/* ---- the text's two changes, which everything else is made of ---- */

static void
push(struct change *stack, int *n, struct change c)
{
	if (*n == UNDO_MOST) {
		free(stack[0].text);
		memmove(stack, stack + 1, (UNDO_MOST - 1) * sizeof stack[0]);
		--*n;
	}
	stack[(*n)++] = c;
}

static void
forget(struct change *stack, int *n)
{
	while (*n)
		free(stack[--*n].text);
}

static void
remember(int inserted, int x0, int y0, int x1, int y1, char *text, int length)
{
	struct change c = { inserted, x0, y0, x1, y1, length, 0, NULL }, *last;

	if (!turning) {
		forget(redo, &redo_count);	/* a new change: what was taken back cannot be made again */
		c.joined = inserted && length == 1 && text[0] != '\n';
		/* a letter typed after a letter is one change with it */
		last = undo_count ? &undo[undo_count - 1] : NULL;
		if (c.joined && last && last->inserted && last->joined && last->y1 == y0 && last->x1 == x0) {
			last->x1 = x1;
			free(text);
			return;
		}
	}
	if (inserted)
		free(text);
	else
		c.text = text;
	if (turning == 1)
		push(redo, &redo_count, c);
	else
		push(undo, &undo_count, c);
}

/* The text from (x0, y0) to (x1, y1), lines parted by a newline; the caller frees it. */
static char *
text_of(int x0, int y0, int x1, int y1, int *length)
{
	int n = 0, y;
	char *text;

	for (y = y0; y <= y1; y++)
		n += (y == y1 ? x1 : (int)strlen(lines[y]) + 1) - (y == y0 ? x0 : 0);
	text = malloc(n + 1);
	n = 0;
	for (y = y0; y <= y1; y++) {
		int from = y == y0 ? x0 : 0, to = y == y1 ? x1 : (int)strlen(lines[y]);

		memcpy(text + n, lines[y] + from, to - from);
		n += to - from;
		if (y < y1)
			text[n++] = '\n';
	}
	text[n] = 0;
	*length = n;
	return text;
}

/* Puts text in at (x, y) and leaves the cursor after it. */
static void
text_insert(int x, int y, const char *text, int length)
{
	int x0 = x, y0 = y, tail_length = strlen(lines[y]) - x;
	char *tail = malloc(tail_length + 1), *copy = malloc(length + 1);

	memcpy(copy, text, length);
	copy[length] = 0;
	memcpy(tail, lines[y] + x, tail_length + 1);
	lines[y][x] = 0;
	while (length > 0) {
		const char *end = memchr(text, '\n', length);
		int n = end ? (int)(end - text) : length, i;

		lines[y] = realloc(lines[y], x + n + tail_length + 1);
		for (i = 0; i < n; i++)
			lines[y][x + i] = text[i] == '\t' || (unsigned char)text[i] < 32 || (unsigned char)text[i] > 126 ? ' ' : text[i];
		x += n;
		lines[y][x] = 0;
		text += n, length -= n;
		if (end) {
			insert_line(++y, "", 0);
			x = 0;
			text++, length--;
		}
	}
	lines[y] = realloc(lines[y], x + tail_length + 1);
	memcpy(lines[y] + x, tail, tail_length + 1);
	free(tail);
	cx = x, cy = y;
	modified = 1;
	remember(1, x0, y0, x, y, copy, (int)strlen(copy));
}

/* Takes the text from (x0, y0) to (x1, y1) out and leaves the cursor there. */
static void
text_delete(int x0, int y0, int x1, int y1)
{
	int length, y, rest;
	char *text;

	if (y0 == y1 && x0 == x1)
		return;
	text = text_of(x0, y0, x1, y1, &length);
	rest = strlen(lines[y1]) - x1;
	if (y1 == y0) {
		memmove(lines[y0] + x0, lines[y0] + x1, rest + 1);
	} else {
		lines[y0] = realloc(lines[y0], x0 + rest + 1);
		memcpy(lines[y0] + x0, lines[y1] + x1, rest + 1);
	}
	for (y = y0 + 1; y <= y1; y++)
		free(lines[y]);
	memmove(lines + y0 + 1, lines + y1 + 1, (count - y1 - 1) * sizeof *lines);
	count -= y1 - y0;
	cx = x0, cy = y0;
	modified = 1;
	remember(0, x0, y0, x1, y1, text, length);
}

/* Takes the last change back, or (`again`) makes the last one taken back again. */
static void
turn(int again)
{
	int *n = again ? &redo_count : &undo_count;
	struct change c;

	if (!*n) {
		say(again ? "nothing to redo" : "nothing to undo");
		return;
	}
	c = (again ? redo : undo)[--*n];
	turning = again ? 2 : 1;
	if (c.inserted)
		text_delete(c.x0, c.y0, c.x1, c.y1);
	else
		text_insert(c.x0, c.y0, c.text, c.length);
	turning = 0;
	free(c.text);
	marked = 0;
}

/* ---- the selection ---- */

/* The selection's two ends in the text's order; false when there is none. */
static int
selection(int *x0, int *y0, int *x1, int *y1)
{
	if (!marked || (ax == cx && ay == cy))
		return 0;
	if (ay < cy || (ay == cy && ax < cx))
		*x0 = ax, *y0 = ay, *x1 = cx, *y1 = cy;
	else
		*x0 = cx, *y0 = cy, *x1 = ax, *y1 = ay;
	return 1;
}

static int
delete_selection(void)
{
	int x0, y0, x1, y1;

	if (!selection(&x0, &y0, &x1, &y1))
		return 0;
	text_delete(x0, y0, x1, y1);
	marked = 0;
	return 1;
}

static void
copy_selection(void)
{
	int x0, y0, x1, y1, length;
	char *text;

	if (!selection(&x0, &y0, &x1, &y1)) {
		say("nothing is selected");
		return;
	}
	text = text_of(x0, y0, x1, y1, &length);
	ui_clip_set(text, length);
	free(text);
	say("copied");
}

/* ---- the file ---- */

static void
titled(void)
{
	char title[300];

	snprintf(title, sizeof title, "%.200s - Editor", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	if (window)
		GrSetWindowTitle(window, title);
}

/* No text at all: no line either, which the caller gives it. */
static void
clear_text(void)
{
	while (count)
		free(lines[--count]);
	forget(undo, &undo_count);
	forget(redo, &redo_count);
	cx = cy = ax = ay = marked = top = left = modified = 0;
}

/* A text with no file yet: Save will ask what to call it. */
static void
new_text(void)
{
	clear_text();
	insert_line(0, "", 0);
	snprintf(path, sizeof path, "/root/untitled.txt");
	named = 0;
	titled();
}

/* The file's text in place of what there was. A file that is not there is a new one. */
static void
open_path(const char *file)
{
	FILE *in = fopen(file, "r");
	char *text = NULL;
	size_t size = 0;
	ssize_t n;
	int why = errno, i;

	clear_text();
	/* a line of any length, its tabs made spaces on the way */
	while (in && (n = getline(&text, &size, in)) >= 0) {
		while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
			n--;
		for (i = 0; i < n; i++)
			if (text[i] == '\t')
				text[i] = ' ';
		insert_line(count, text, (int)n);
	}
	free(text);
	if (in)
		fclose(in);
	else if (why == ENOENT)
		say("new file");
	else
		snprintf(notice, sizeof notice, "cannot be read (%s)", strerror(why));
	if (count == 0)
		insert_line(0, "", 0);
	snprintf(path, sizeof path, "%s", file);
	named = 1;
	titled();
}

/* Writes the text to a file, which is then the text's own; false, and why, when it cannot. */
static int
write_to(const char *to)
{
	FILE *file = fopen(to, "w");
	int i, fine;

	for (i = 0; file && i < count; i++) {
		fputs(lines[i], file);
		fputc('\n', file);
	}
	fine = file && !ferror(file);
	if (file && fclose(file) != 0)
		fine = 0;
	if (!fine) {
		snprintf(notice, sizeof notice, "NOT saved: cannot write %.90s (%s)", to, strerror(errno));
		return 0;
	}
	if (to != path)
		snprintf(path, sizeof path, "%s", to);
	named = 1, modified = 0;
	titled();
	snprintf(notice, sizeof notice, "saved as %.150s", path);
	return 1;
}

/* A line of the text; `cleared`: its ground is white already (draw_all). */
static void
draw_line_on(int line, int cleared)
{
	int y = MENU_H + MARGIN + (line - top) * ui_fh, length, x0, y0, x1, y1, from = 0, to = 0, shown;

	if (line < top || line >= top + rows)
		return;
	if (!cleared)
		ui_fill(window, 0, y, width - UI_SCROLL_W, ui_fh, WHITE);
	if (line >= count)
		return;
	length = strlen(lines[line]);
	shown = length - left > columns ? columns : length - left;
	if (selection(&x0, &y0, &x1, &y1) && line >= y0 && line <= y1) {
		/* the selected columns of this line; a line's end counts as one more */
		from = (line == y0 ? x0 : 0) - left;
		to = (line == y1 ? x1 : length + 1) - left;
		from = from < 0 ? 0 : from > columns ? columns : from;
		to = to < 0 ? 0 : to > columns ? columns : to;
		if (to > from)
			ui_fill(window, MARGIN + from * ui_fw, y, (to - from) * ui_fw, ui_fh, UI_SELECTED);
	}
	if (shown > 0) {
		const char *text = lines[line] + left;

		if (to > from) {
			int a = from > shown ? shown : from, b = to > shown ? shown : to;

			if (a > 0)
				ui_text(window, MARGIN, y, text, a, BLACK, 1);
			if (b > a)
				ui_text(window, MARGIN + a * ui_fw, y, text + a, b - a, WHITE, 1);
			if (shown > b)
				ui_text(window, MARGIN + b * ui_fw, y, text + b, shown - b, BLACK, 1);
		} else {
			/* (spaces at its ends draw nothing, and each is a word of the request's) */
			int lead = 0;

			while (lead < shown && text[lead] == ' ')
				lead++;
			while (shown > lead && text[shown - 1] == ' ')
				shown--;
			if (shown > lead)
				ui_text(window, MARGIN + lead * ui_fw, y, text + lead, shown - lead, BLACK, 1);
		}
	}
	if (line == cy)
		ui_fill(window, MARGIN + (cx - left) * ui_fw, y, 2, ui_fh, BLACK);
}

static void
draw_line(int line)
{
	draw_line_on(line, 0);
}

static int status_edge = 1;	/* the status line's edge is to be drawn too */

/* Puts a text, or a number, at the end of another; where that ends is returned. */
static char *
put(char *to, const char *text)
{
	while (*text)
		*to++ = *text++;
	*to = 0;
	return to;
}

static char *
put_number(char *to, int n)
{
	char digits[12];
	int at = sizeof digits - 1;

	for (digits[at] = 0; n > 0 || at == (int)sizeof digits - 1; n /= 10)
		digits[--at] = (char)('0' + n % 10);
	return put(to, digits + at);
}

static void
draw_status(void)
{
	char text[800];

	if (asking == ASK_FIND)
		snprintf(text, sizeof text, "Find: %s_   (Enter finds, Escape leaves)", sought);
	else if (asking == ASK_LINE)
		snprintf(text, sizeof text, "Go to line: %s_   (of %d; Enter goes, Escape leaves)", line_asked, count);
	else	/* (what every key changes, made without printf: 2,800 instructions a key) */
		put(put(put_number(put(put_number(put(put(put(text, path), modified ? " *" : ""), "   line "), cy + 1), ", column "), cx + 1), "   "), notice);
	/* its edge is drawn with the whole window (draw_all); a key changes the line inside it */
	if (status_edge) {
		ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
		ui_bevel(window, 0, height - STATUS, width, STATUS, 1);
		status_edge = 0;
	} else
		ui_fill(window, 1, height - STATUS + 1, width - 2, STATUS - 2, UI_FACE);
	ui_text_fit(window, 6, height - STATUS + 3, width - 12, text, BLACK);
}

/* The text in view, its scroll bar and the status line: what a move through the text changes. */
static void
draw_text(void)
{
	int line;

	ui_fill(window, 0, MENU_H, width - UI_SCROLL_W, height - MENU_H - STATUS, WHITE);
	for (line = top; line < top + rows; line++)
		draw_line_on(line, 1);
	ui_scrollbar(window, width - UI_SCROLL_W, MENU_H, height - MENU_H - STATUS, count, rows, top);
	draw_status();
}

static void
draw_all(void)
{
	int i;

	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < 2; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
	status_edge = 1;
	draw_text();
}

static int scroll_drawn;	/* NXEDIT_SCROLL=draw: a view that moved is drawn whole, as it was before */

/* The view moved from line `was` to top, with nothing selected: the lines that stay in view are
 * copied to their new rows (one request, where each was one), those that came are drawn, and
 * the cursor's bar is taken from the line `bar` it was on. All of it is drawn from too far. */
static void
view_moved(int was, int bar)
{
	int by = top - was, stay = rows - abs(by), y = MENU_H + MARGIN, line;

	if (stay < 1 || scroll_drawn) {
		draw_text();
		return;
	}
	GrCopyArea(window, ui_gc, 0, y + (by < 0 ? -by * ui_fh : 0), width - UI_SCROLL_W, stay * ui_fh, window, 0, y + (by > 0 ? by * ui_fh : 0),
		   MWROP_COPY);
	for (line = by > 0 ? top + stay : top; line < (by > 0 ? top + rows : top - by); line++)
		draw_line(line);
	if (bar != cy && bar >= top && bar < top + rows && (by > 0 ? bar < top + stay : bar >= top - by))
		draw_line(bar);
	if (bar != cy && cy >= top && cy < top + rows && (by > 0 ? cy < top + stay : cy >= top - by))
		draw_line(cy);
	ui_scrollbar(window, width - UI_SCROLL_W, MENU_H, height - MENU_H - STATUS, count, rows, top);
	draw_status();
}

/* Shows the text from another line on, leaving the cursor where it is in the text. */
static void
scroll_to(int line)
{
	int was = top, x0, y0, x1, y1;

	if (line > count - rows)
		line = count - rows;
	if (line < 0)
		line = 0;
	if (line != top) {
		top = line;
		if (selection(&x0, &y0, &x1, &y1))
			draw_text();
		else
			view_moved(was, cy);
	}
}

/* Brings the cursor into view; true if everything had to move. */
static int
follow(void)
{
	int old_top = top, old_left = left;

	if (cy < top)
		top = cy;
	if (cy >= top + rows)
		top = cy - rows + 1;
	if (cx < left)
		left = cx;
	if (cx >= left + columns)
		left = cx - columns + 1;
	return top != old_top || left != old_left;
}

/* Looks for the sought text from just after the cursor, round the end to the start; selects it. */
static void
find_next(void)
{
	int n = strlen(sought), y, pass, x0, y0, x1, y1, start_x = cx, start_y = cy;

	if (!n) {
		say("Ctrl+F says what to find");
		return;
	}
	if (selection(&x0, &y0, &x1, &y1))
		start_x = x0 + 1, start_y = y0;	/* past the one found last */
	for (pass = 0; pass <= count; pass++) {
		const char *line, *at;
		int from = 0;

		y = (start_y + pass) % count;
		line = lines[y];
		if (pass == 0)
			from = start_x < (int)strlen(line) ? start_x : (int)strlen(line);
		at = strstr(line + from, sought);
		if (at) {
			ax = (int)(at - line), ay = y;
			cx = ax + n, cy = y;
			marked = 1;
			say("found");
			return;
		}
	}
	say("not found");
}

static void
resized(int w, int h)
{
	width = w;
	height = h;
	rows = (height - MENU_H - STATUS - MARGIN) / ui_fh;
	columns = (width - 2 * MARGIN - UI_SCROLL_W) / ui_fw;
	if (rows < 1)
		rows = 1;
	if (columns < 1)
		columns = 1;
	follow();
}

/* This window's events while a question or a file's window is up. */
static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_UPDATE && event->update.wid == window && event->update.utype == GR_UPDATE_SIZE)
		resized(event->update.width, event->update.height);
	else if (event->type != GR_EVENT_TYPE_EXPOSURE || event->exposure.wid != window)
		return;
	draw_all();
}

/* Asks what to save the text as and writes it there; false if it was not written. */
static int
save_as(void)
{
	char where[512], chosen[512], *slash;

	snprintf(where, sizeof where, "%s", path);
	if ((slash = strrchr(where, '/')) != NULL)
		*slash = 0;
	return ui_save_file("Save the text as", !slash ? "/root" : slash == where ? "/" : where, NULL, slash ? slash + 1 : where,
			    chosen, sizeof chosen, others) && write_to(chosen);
}

static int
save(void)
{
	return named ? write_to(path) : save_as();
}

/* True if the text may be let go: it has no changes, or the answer was to save them or not to. */
static int
may_drop(void)
{
	static const char *const answers[] = { "Save", "Don't save", "Cancel" };
	char text[400];

	if (!modified)
		return 1;
	snprintf(text, sizeof text, "%.200s has changes that are not saved.\nSave them?", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	switch (ui_ask("Editor", text, answers, 3, others)) {
	case 0: return save();
	case 1: return 1;
	}
	return 0;
}

/* Begins the typing of an answer in the status line. */
static void
ask(int what)
{
	asking = what;
	line_asked[0] = 0;
	draw_status();
}

/* What a menu's row, or the key that stands for it, does. */
static void
command(int id)
{
	char chosen[512], where[512], *slash, *text;
	int n;

	notice[0] = 0;
	switch (id) {
	case C_NEW:
		if (may_drop())
			new_text();
		break;
	case C_OPEN:
		if (!may_drop())
			break;
		snprintf(where, sizeof where, "%s", path);
		if ((slash = strrchr(where, '/')) != NULL)
			slash[slash == where ? 1 : 0] = 0;
		if (ui_choose_file("Open a text", slash ? where : "/root", NULL, chosen, sizeof chosen, others))
			open_path(chosen);
		break;
	case C_SAVE:
		if (named) {
			write_to(path);
			draw_status();
			return;
		}
		/* fall through: a text with no file is asked what to be saved as */
	case C_SAVE_AS:
		save_as();
		break;
	case C_EXIT:
		if (may_drop()) {
			GrClose();
			exit(0);
		}
		break;
	case C_UNDO:
	case C_REDO:
		turn(id == C_REDO);
		break;
	case C_COPY:
		copy_selection();
		draw_status();
		return;
	case C_CUT:
		copy_selection();
		delete_selection();
		break;
	case C_PASTE:
		if ((text = ui_clip_get(&n)) == NULL) {
			say("the clipboard is empty");
			break;
		}
		delete_selection();
		text_insert(cx, cy, text, n);
		free(text);
		break;
	case C_DELETE:
		if (!delete_selection())
			say("nothing is selected");
		break;
	case C_ALL:
		ax = ay = 0;
		cy = count - 1, cx = strlen(lines[cy]);
		marked = 1;
		break;
	case C_FIND:
	case C_LINE:
		ask(id == C_FIND ? ASK_FIND : ASK_LINE);
		return;
	case C_NEXT:
		find_next();
		break;
	}
	follow();
	draw_all();
}

/* A key of the line Ctrl+F or Ctrl+G opens in the status bar. */
static void
ask_key(int ch)
{
	char *answer = asking == ASK_FIND ? sought : line_asked;
	int n = strlen(answer), most = asking == ASK_FIND ? (int)sizeof sought : (int)sizeof line_asked, line;

	if (ch == MWKEY_ESCAPE) {
		asking = ASK_NONE;
	} else if (ch == MWKEY_ENTER || ch == '\n') {
		if (asking == ASK_FIND)
			find_next();
		else if ((line = atoi(line_asked)) > 0)
			cy = (line > count ? count : line) - 1, cx = 0, marked = 0;
		asking = ASK_NONE;
		follow();
		draw_all();
		return;
	} else if (ch == MWKEY_BACKSPACE || ch == 127) {
		if (n)
			answer[n - 1] = 0;
	} else if (ch >= 32 && ch < 127 && n < most - 1 && (asking == ASK_FIND || isdigit(ch))) {
		answer[n] = ch, answer[n + 1] = 0;
	}
	draw_status();
}

/* The command a letter with Ctrl is, or 0. */
static int
command_of(int letter)
{
	static const char letters[] = "nosqzyxcvafg";
	static const int ids[] = { C_NEW, C_OPEN, C_SAVE, C_EXIT, C_UNDO, C_REDO, C_CUT, C_COPY, C_PASTE, C_ALL, C_FIND, C_LINE };
	const char *at = letter ? strchr(letters, letter) : NULL;

	return at ? ids[at - letters] : 0;
}

static void
key(int ch, int modifiers)
{
	int was = cy, length = strlen(lines[cy]), whole = 0, moves = 0, letter = 0, line;
	int shift = (modifiers & MWKMOD_SHIFT) != 0, ctrl = (modifiers & MWKMOD_CTRL) != 0, edits, cut = 0, x0, y0, x1, y1;
	int had = selection(&x0, &y0, &x1, &y1), was_marked = marked, was_ax = ax, was_ay = ay;	/* had: something was shown selected */
	int was_top = top, was_left = left;

	if (asking) {
		ask_key(ch);
		return;
	}
	notice[0] = 0;
	/* a letter with Ctrl comes as the letter with the modifier, or as its control code */
	if (ctrl && ch < 128 && isalpha(ch))
		letter = tolower(ch);
	else if (ch >= 1 && ch <= 26 && ch != MWKEY_BACKSPACE && ch != MWKEY_TAB && ch != '\n' && ch != MWKEY_ENTER)
		letter = 'a' + ch - 1;
	if (letter || ch == MWKEY_F3) {
		if (ch == MWKEY_F3 || command_of(letter))
			command(ch == MWKEY_F3 ? C_NEXT : command_of(letter));
		return;
	}
	if (ch == MWKEY_F10) {
		/* the menus, for the keyboard: File's, its first row picked out */
		ui_window_origin(window, &x0, &y0);
		ui_menu_open(menus[0].items, menus[0].count, x0 + menu_x[0], y0 + MENU_H, 0, 1);
		return;
	}
	switch (ch) {
	case MWKEY_LEFT:
	case MWKEY_RIGHT:
	case MWKEY_UP:
	case MWKEY_DOWN:
	case MWKEY_PAGEUP:
	case MWKEY_PAGEDOWN:
	case MWKEY_HOME:
	case MWKEY_END:
		moves = 1;
		if (shift && !marked)
			ax = cx, ay = cy, marked = 1;
		if (!shift)
			marked = 0;
		break;
	}
	edits = ch == MWKEY_ENTER || ch == '\n' || ch == MWKEY_BACKSPACE || ch == 127 || ch == MWKEY_DELETE || ch == '\t' ||
		(ch >= 32 && ch <= 126);
	if (edits) {
		cut = delete_selection();	/* what is selected goes, whatever is typed */
		marked = 0;
		whole = cut;
	}
	switch (ch) {
	case MWKEY_LEFT:
		if (cx > 0)
			cx--;
		else if (cy > 0)
			cx = strlen(lines[--cy]);
		break;
	case MWKEY_RIGHT:
		if (cx < length)
			cx++;
		else if (cy + 1 < count)
			cy++, cx = 0;
		break;
	case MWKEY_UP:
		if (cy > 0)
			cy--;
		break;
	case MWKEY_DOWN:
		if (cy + 1 < count)
			cy++;
		break;
	case MWKEY_PAGEUP:
		cy = cy > rows ? cy - rows : 0;
		break;
	case MWKEY_PAGEDOWN:
		cy = cy + rows < count ? cy + rows : count - 1;
		break;
	case MWKEY_HOME:
		cx = 0;
		if (ctrl)
			cy = 0;		/* with Ctrl: the text's start */
		break;
	case MWKEY_END:
		if (ctrl)
			cy = count - 1;	/* with Ctrl: the text's end */
		cx = strlen(lines[cy]);
		break;
	case MWKEY_ENTER:
	case '\n':
		text_insert(cx, cy, "\n", 1);
		whole = 1;
		break;
	case MWKEY_BACKSPACE:
	case 127:
		if (cut) {
		} else if (cx > 0) {
			text_delete(cx - 1, cy, cx, cy);
		} else if (cy > 0) {
			text_delete(strlen(lines[cy - 1]), cy - 1, 0, cy);
			whole = 1;
		}
		break;
	case MWKEY_DELETE:
		if (cut) {
		} else if (cx < length) {
			text_delete(cx, cy, cx + 1, cy);
		} else if (cy + 1 < count) {
			text_delete(cx, cy, 0, cy + 1);
			whole = 1;
		}
		break;
	default: {
		char typed;

		if (moves)
			break;
		if (ch == '\t')
			ch = ' ';
		if (ch < 32 || ch > 126)
			return;
		typed = (char)ch;
		text_insert(cx, cy, &typed, 1);
	}
	}
	if (cx > (int)strlen(lines[cy]))
		cx = strlen(lines[cy]);
	if (marked && ax == cx && ay == cy && !shift)
		marked = 0;
	if (follow() || whole) {
		/* the cursor's move took the view with it, up or down: no text changed and none is selected */
		if (moves && !whole && !had && !selection(&x0, &y0, &x1, &y1) && left == was_left && top != was_top)
			view_moved(was_top, was);
		else
			draw_text();
	} else if (moves && shift && (!was_marked || (ax == was_ax && ay == was_ay))) {
		/* the selection grew or shrank at the cursor's end: the lines the cursor passed */
		for (line = was < cy ? was : cy; line <= (was < cy ? cy : was); line++)
			draw_line(line);
		draw_status();
	} else if (had || selection(&x0, &y0, &x1, &y1)) {
		draw_text();
	} else {
		draw_line(was);
		if (cy != was)
			draw_line(cy);
		draw_status();
	}
}

/* The place in the text under a point of the window. */
static void
place(int px, int py, int *x, int *y)
{
	int line = top + (py - MENU_H - MARGIN) / ui_fh, column;

	if (py < MENU_H + MARGIN)
		line = top;
	if (line >= count)
		line = count - 1;
	column = left + (px - MARGIN + ui_fw / 2) / ui_fw;
	if (column < 0)
		column = 0;
	if (column > (int)strlen(lines[line]))
		column = strlen(lines[line]);
	*x = column, *y = line;
}

static int
in_word(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}

/* The left button went down in the text: the cursor goes there, and a selection begins with
 * it; a second press at the same place selects the word there. */
static void
click(const GR_EVENT *event)
{
	static unsigned last_time;
	static int last_x = -1, last_y;
	int x0, y0, x1, y1, had = selection(&x0, &y0, &x1, &y1), was = cy;
	const char *line;

	place(event->button.x, event->button.y, &cx, &cy);
	ax = cx, ay = cy;
	marked = 1;
	selecting = 1;
	notice[0] = 0;
	line = lines[cy];
	if (cx == last_x && cy == last_y && event->button.time - last_time < 500) {
		while (ax > 0 && in_word(line[ax - 1]))
			ax--;
		while (in_word(line[cx]))
			cx++;
		selecting = 0;
		had = 1;
	}
	last_x = ax == cx ? cx : -1, last_y = cy, last_time = event->button.time;
	if (had)
		draw_all();
	else {
		draw_line(was);
		if (cy != was)
			draw_line(cy);
		draw_status();
	}
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	int i, x, id, text_bottom;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	scroll_drawn = getenv("NXEDIT_SCROLL") && !strcmp(getenv("NXEDIT_SCROLL"), "draw");
	if (argc > 1)
		open_path(argv[1]);
	else
		new_text();
	for (i = 0, x = 2; i < 2; i++) {
		menu_x[i] = x;
		x += ui_width(&ui_var_widths, ui_var, menu_names[i], -1) + 16;
	}
	menu_x[2] = x;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Editor", GR_ROOT_WINDOW_ID, -1, -1,
		80 * ui_fw + 2 * MARGIN + UI_SCROLL_W, 24 * ui_fh + MARGIN + MENU_H + STATUS, WHITE);
	titled();
	GrSelectEvents(window, EVENTS);
	resized(80 * ui_fw + 2 * MARGIN + UI_SCROLL_W, 24 * ui_fh + MARGIN + MENU_H + STATUS);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		GrGetNextEvent(&event);
		ui_origin_note(&event);
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			GrSetFocus(window);
			command(id);
			continue;
		case UI_MENU_CLOSED:
			GrSetFocus(window);
			/* fall through */
		case UI_MENU_TOOK:
			continue;
		}
		text_bottom = height - STATUS;
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE && event.update.wid == window) {
				resized(event.update.width, event.update.height);
				draw_all();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event)) {
				scroll_to(top + 3 * ui_wheel_sum(&event));
				break;
			}
			if (event.button.y < MENU_H) {
				for (i = 0; i < 2; i++)
					if (event.button.x >= menu_x[i] && event.button.x < menu_x[i + 1]) {
						int wx, wy;

						ui_window_origin(window, &wx, &wy);
						ui_menu_open(menus[i].items, menus[i].count, wx + menu_x[i], wy + MENU_H, 0, 0);
					}
				break;
			}
			if (event.button.y >= text_bottom)
				break;
			if (event.button.changebuttons & GR_BUTTON_R) {
				ui_menu_open(pointer_items, 5, event.button.rootx, event.button.rooty, 0, 0);
				break;
			}
			ui_moves(window, EVENTS, 1);
			if (event.button.x >= width - UI_SCROLL_W) {
				dragging = 1;
				scroll_to(ui_scroll_to(event.button.y, MENU_H, text_bottom - MENU_H, count, rows));
			} else if (event.button.changebuttons & GR_BUTTON_L)
				click(&event);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = selecting = 0;
			if (!(event.button.buttons & (GR_BUTTON_L | GR_BUTTON_R | GR_BUTTON_M)))
				ui_moves(window, EVENTS, 0);
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (dragging && (event.mouse.buttons & GR_BUTTON_L)) {
				scroll_to(ui_scroll_to(event.mouse.y, MENU_H, text_bottom - MENU_H, count, rows));
			} else if (selecting && (event.mouse.buttons & GR_BUTTON_L)) {
				int px, py, from, to, line;

				place(event.mouse.x, event.mouse.y < text_bottom ? event.mouse.y : text_bottom - 1, &px, &py);
				if (px != cx || py != cy) {
					from = py < cy ? py : cy, to = py < cy ? cy : py;
					cx = px, cy = py;
					for (line = from; line <= to; line++)
						draw_line(line);
					draw_status();
				}
			} else {
				dragging = selecting = 0;
				if (!event.mouse.buttons)
					ui_moves(window, EVENTS, 0);
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid == window)	/* (a menu's may come after the menu went) */
				command(C_EXIT);
			break;
		}
	}
}
