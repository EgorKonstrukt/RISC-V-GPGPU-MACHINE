/* ui_launch.h: what a click that starts a program shows until the program's first window is
 * there: the pointer with an hourglass and a note by it, "Starting NAME" (docs/nanox.md, "A
 * program starting"). Ends at another program's window shown, the program gone, or 30 s.
 * Needs nano-X.h and ui_icons.h; UI_LAUNCH=0 shows nothing, UI_LAUNCH=print says when it ends. */
#ifndef UI_LAUNCH_H
#define UI_LAUNCH_H
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include "ui_icons.h"

#define UI_LAUNCH_MOST 30000	/* ms */
#define UI_LAUNCH_HIGH 40

static GR_WINDOW_ID ui_launch_note;
static GR_CURSOR_ID ui_launch_busy;
static GR_GC_ID ui_launch_gc;
static GR_EVENT_MASK ui_launch_root;	/* the root's events as this program has them otherwise */
static pid_t ui_launch_pid;
static unsigned ui_launch_began, ui_launch_shown_s, ui_launch_looked;
static int ui_launch_w, ui_launch_fresh;	/* fresh: drawn at its map, its first exposure is no news */
static char ui_launch_line[48];
static struct ui_icon_ref ui_launch_icon;

static unsigned
ui_launch_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* The pointer with an hourglass beside it: '#' black, 'o' white. */
static GR_CURSOR_ID
ui_launch_cursor(void)
{
	static const char *art[23] = {
		"#",
		"##",
		"#o#",
		"#oo#",
		"#ooo#",
		"#oooo#",
		"#ooooo#",
		"#oooooo#",
		"#ooooooo#  ###########",
		"#oooooooo# #ooooooooo#",
		"#ooooooooo############",
		"#oooooo######ooooooo#",
		"#ooo#oo#    #o#####o#",
		"#oo##oo#     #o###o#",
		"#o#  #oo#     #o#o#",
		"##   #oo#      #o#",
		"#     #oo#    #ooo#",
		"      #oo#   #oo#oo#",
		"       ##   #oo###oo#",
		"            #o#####o#",
		"           ###########",
		"           #ooooooooo#",
		"           ###########",
	};
	GR_BITMAP image[23 * 2], mask[23 * 2];
	int r, c;

	memset(image, 0, sizeof image);
	memset(mask, 0, sizeof mask);
	for (r = 0; r < 23; r++)
		for (c = 0; art[r][c]; c++) {
			GR_BITMAP bit = (GR_BITMAP)(0x8000 >> (c & 15));

			if (art[r][c] == '#' || art[r][c] == 'o')
				mask[r * 2 + (c >> 4)] |= bit;
			if (art[r][c] == '#')
				image[r * 2 + (c >> 4)] |= bit;
		}
	return GrNewCursor(22, 23, 0, 0, WHITE, BLACK, image, mask);
}

static void
ui_launch_draw(void)
{
	unsigned s = (ui_launch_ms() - ui_launch_began) / 1000;
	char line[64];

	ui_launch_shown_s = s;
	GrSetGCForeground(ui_launch_gc, MWRGB(192, 192, 192));
	GrFillRect(ui_launch_note, ui_launch_gc, 0, 0, ui_launch_w, UI_LAUNCH_HIGH);
	GrSetGCForeground(ui_launch_gc, BLACK);
	GrRect(ui_launch_note, ui_launch_gc, 0, 0, ui_launch_w, UI_LAUNCH_HIGH);
	GrSetGCForeground(ui_launch_gc, MWRGB(128, 128, 128));
	GrLine(ui_launch_note, ui_launch_gc, 1, 1, ui_launch_w - 2, 1);
	GrLine(ui_launch_note, ui_launch_gc, 1, 1, 1, UI_LAUNCH_HIGH - 2);
	ui_icon_draw(ui_launch_note, ui_launch_gc, 6, 4, &ui_launch_icon, 32);
	GrSetGCForeground(ui_launch_gc, BLACK);
	GrText(ui_launch_note, ui_launch_gc, 44, 7, ui_launch_line, -1, GR_TFASCII | GR_TFTOP);
	snprintf(line, sizeof line, s ? "%u s" : "", s);
	GrSetGCForeground(ui_launch_gc, MWRGB(64, 64, 64));
	GrText(ui_launch_note, ui_launch_gc, 44, 22, line, -1, GR_TFASCII | GR_TFTOP);
	GrFlush();
}

static void
ui_launch_end(const char *why)
{
	const char *say = getenv("UI_LAUNCH");

	if (!ui_launch_pid)
		return;
	if (say && !strcmp(say, "print"))
		fprintf(stderr, "launch: %s after %u ms\n", why, ui_launch_ms() - ui_launch_began);
	ui_launch_pid = 0;
	GrDestroyWindow(ui_launch_note);
	ui_launch_note = 0;
	GrSetWindowCursor(GR_ROOT_WINDOW_ID, 0);
	GrSelectEvents(GR_ROOT_WINDOW_ID, ui_launch_root);
	GrFlush();
}

/* A program is about to be started for a click: `name` and its icon by the pointer, before it
 * is (ui_launch_started), until it shows a window. `root_events`: the root's events this
 * program selects of its own. */
static void
ui_launch_begin(const char *name, const struct ui_icon_ref *icon, GR_EVENT_MASK root_events)
{
	const char *off = getenv("UI_LAUNCH");
	static GR_SCREEN_INFO si;
	GR_WINDOW_ID under;
	GR_COORD px, py, x, y;
	GR_BUTTON buttons;
	GR_SIZE tw, th, tb;

	ui_launch_end("another started");
	if (off && !strcmp(off, "0"))
		return;
	if (!ui_launch_gc) {
		ui_launch_gc = GrNewGC();
		GrSetGCUseBackground(ui_launch_gc, GR_FALSE);
		GrSetGCFont(ui_launch_gc, GrCreateFontEx(GR_FONT_SYSTEM_VAR, 0, 0, NULL));
		ui_launch_busy = ui_launch_cursor();
		GrGetScreenInfo(&si);
	}
	ui_launch_pid = -1;	/* (not known yet) */
	ui_launch_began = ui_launch_ms();
	if (off && !strcmp(off, "print"))
		fprintf(stderr, "launch: %s\n", name);
	ui_launch_root = root_events;
	ui_launch_icon = *icon;
	snprintf(ui_launch_line, sizeof ui_launch_line, "Starting %.32s", name);
	GrGetGCTextSize(ui_launch_gc, ui_launch_line, -1, GR_TFASCII, &tw, &th, &tb);
	ui_launch_w = 44 + tw + 12;
	GrQueryPointer(&under, &px, &py, &buttons);
	/* below and right of the pointer's point, on the screen and above the bar */
	x = px + 14 + ui_launch_w > si.cols ? si.cols - ui_launch_w : px + 14;
	y = py + 22 + UI_LAUNCH_HIGH > si.rows - 30 ? py - UI_LAUNCH_HIGH - 8 : py + 22;
	ui_launch_note = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE |
		GR_WM_PROPS_NOFOCUS, "starting", GR_ROOT_WINDOW_ID, x < 0 ? 0 : x, y < 0 ? 0 : y,
		ui_launch_w, UI_LAUNCH_HIGH, MWRGB(192, 192, 192));
	GrSelectEvents(ui_launch_note, GR_EVENT_MASK_EXPOSURE);
	GrSelectEvents(GR_ROOT_WINDOW_ID, root_events | GR_EVENT_MASK_CHLD_UPDATE);
	GrSetWindowCursor(GR_ROOT_WINDOW_ID, ui_launch_busy);
	GrMapWindow(ui_launch_note);
	ui_launch_fresh = 1;
	ui_launch_draw();	/* (at once, not at its exposure: a pass sooner) */
}

/* The program's number once it is started, or 0 or less when it could not be. */
static void
ui_launch_started(pid_t pid)
{
	if (ui_launch_pid != -1)
		return;
	if (pid > 0)
		ui_launch_pid = pid;
	else
		ui_launch_end("not started");
}

/* Every event, before the program looks at it: true if it was the note's own. */
static int
ui_launch_event(const GR_EVENT *event)
{
	GR_WINDOW_INFO info;

	if (!ui_launch_pid)
		return 0;
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == ui_launch_note) {
		if (!ui_launch_fresh)
			ui_launch_draw();
		ui_launch_fresh = 0;
		return 1;
	}
	/* a window shown, or framed and made active (a framed window's map is not told the root's
	 * listeners), by the program started or one it started: a process numbered since */
	if (event->type == GR_EVENT_TYPE_CHLD_UPDATE && (event->update.utype == GR_UPDATE_MAP ||
	    event->update.utype == GR_UPDATE_ACTIVATE) && event->update.subwid != ui_launch_note) {
		GrGetWindowInfo(event->update.subwid, &info);
		if (info.processid >= ui_launch_pid)
			ui_launch_end("a window shown");
	}
	return 0;
}

/* Whether a window without a frame of the program started, or of one it started, is shown:
 * the root is told nothing of it. A program's own window numbers are bit 30, its process and a
 * count (nx_new_id in the client library). */
static int
ui_launch_bare_shown(void)
{
	GR_WINDOW_ID parent, *kids = NULL;
	GR_WINDOW_INFO info;
	GR_COUNT n = 0, i;
	int shown = 0;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &kids, &n);
	for (i = 0; i < n && !shown; i++) {
		if (!(kids[i] & 0x40000000) || (pid_t)((kids[i] >> 15) & 0x7fff) < ui_launch_pid || kids[i] == ui_launch_note)
			continue;
		GrGetWindowInfo(kids[i], &info);
		shown = info.mapped;
	}
	free(kids);
	return shown;
}

/* How long the program's event loop may wait now: a quarter second while a program starts. */
static int
ui_launch_wait(int otherwise)
{
	return ui_launch_pid ? 250 : otherwise;
}

/* Every wake: the seconds it has waited, or its end when a window without a frame is shown,
 * the program went, or 30 s did. */
static void
ui_launch_tick(void)
{
	unsigned now;

	if (!ui_launch_pid)
		return;
	now = ui_launch_ms();
	if (now - ui_launch_looked < 250)
		return;
	ui_launch_looked = now;
	if (now - ui_launch_began > UI_LAUNCH_MOST)
		ui_launch_end("30 s");
	else if (ui_launch_pid > 0 && kill(ui_launch_pid, 0) < 0 && errno == ESRCH)
		ui_launch_end("the program gone");
	else if (ui_launch_pid > 0 && ui_launch_bare_shown())
		ui_launch_end("a window without a frame shown");
	else if ((now - ui_launch_began) / 1000 != ui_launch_shown_s)
		ui_launch_draw();
}
#endif
