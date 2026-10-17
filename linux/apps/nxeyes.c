/*
 * nxeyes: two eyes whose pupils follow the pointer anywhere on the screen. The window is an
 * ordinary one, moved and sized by its frame, and the eyes fill it. The pointer is read from
 * the machine's input words (docs/machine/input.md), not from the window system's moves: those come
 * to a program that selects them on the root for every pixel, and keep the server's quiet
 * rectangle away from the whole screen. A right click gives the menu: blinking, and Close.
 */
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"

#define REG_POINTER	8		/* the input words: x, y, buttons */
#define REG_CLOCK	13		/* the host's clock, in milliseconds */
#define FAST_MS		66		/* a look at the pointer while it moves, */
#define SLOW_MS		250		/* while it has been still a second, */
#define SLEEPY_MS	500		/* and after ten seconds of that */
#define BLINK_KEY	"nxeyes.blink"
#define BLINK_SHUT_MS	140

enum { CHOOSE_BLINK = 1, CHOOSE_CLOSE };

static GR_WINDOW_ID win;
static int width = 180, height = 110;
static const volatile uint32_t *input;	/* the machine's control words, or NULL: ask the server */
static int px = -1, py = -1;		/* the pointer on the screen, as last drawn for */
static struct { int cx, cy, rx, ry, pr, x, y, drawn; } eye[2];
static int blink, shut;
static unsigned next_blink, opened_at;

static unsigned
now_ms(void)
{
	struct timeval tv;

	if (input)
		return input[REG_CLOCK];
	gettimeofday(&tv, NULL);
	return (unsigned)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void
pointer(int *x, int *y)
{
	GR_WINDOW_ID under;
	GR_BUTTON buttons;
	GR_COORD qx, qy;

	if (input) {
		*x = (int)input[REG_POINTER], *y = (int)input[REG_POINTER + 1];
		return;
	}
	GrQueryPointer(&under, &qx, &qy, &buttons);
	*x = qx, *y = qy;
}

static unsigned long long
isqrt(unsigned long long n)
{
	unsigned long long r = 0, bit = 1ull << 62;

	while (bit > n)
		bit >>= 2;
	for (; bit; bit >>= 2)
		if (n >= r + bit)
			n -= r + bit, r = (r >> 1) + bit;
		else
			r >>= 1;
	return r;
}

/* Where eye k's pupil goes for the pointer at sx, sy (the window's corner at ox, oy): toward
 * it, as far as the white of an ellipse allows. */
static void
aim(int k, int sx, int sy, int ox, int oy)
{
	int a = eye[k].rx - eye[k].pr - eye[k].rx / 6, b = eye[k].ry - eye[k].pr - eye[k].ry / 6;
	int dx = sx - ox - eye[k].cx, dy = sy - oy - eye[k].cy;
	long long q;

	if (a < 1 || b < 1) {
		eye[k].x = eye[k].cx, eye[k].y = eye[k].cy;
		return;
	}
	/* inside when (dx/a)^2 + (dy/b)^2 <= 1; else scaled back onto the edge */
	q = (long long)dx * dx * b * b + (long long)dy * dy * a * a;
	if (q > (long long)a * a * b * b) {
		long long s = (long long)isqrt((unsigned long long)q), ab = (long long)a * b;

		dx = (int)(dx * ab / s), dy = (int)(dy * ab / s);
	}
	eye[k].x = eye[k].cx + dx, eye[k].y = eye[k].cy + dy;
}

static void
layout(void)
{
	int k, half = width / 2;

	for (k = 0; k < 2; k++) {
		eye[k].cx = half * k + half / 2;
		eye[k].cy = height / 2;
		eye[k].rx = half / 2 - 4;
		eye[k].ry = height / 2 - 4;
		if (eye[k].rx < 4) eye[k].rx = 4;
		if (eye[k].ry < 4) eye[k].ry = 4;
		eye[k].pr = (eye[k].rx < eye[k].ry ? eye[k].rx : eye[k].ry) / 3;
		if (eye[k].pr < 2) eye[k].pr = 2;
		eye[k].drawn = 0;
	}
}

static int
outline(int k)
{
	int r = eye[k].rx < eye[k].ry ? eye[k].rx : eye[k].ry;

	return r / 12 < 2 ? 2 : r / 12;
}

static void
pupil(int k, GR_COLOR colour)
{
	GrSetGCForeground(ui_gc, colour);
	GrFillEllipse(win, ui_gc, eye[k].x, eye[k].y, eye[k].pr, eye[k].pr);
}

/* The pupils where the pointer is now: the old one wiped with the white, the new one drawn. */
static void
pupils(int force)
{
	int ox, oy, k, x, y;

	if (shut)
		return;
	ui_window_origin(win, &ox, &oy);
	for (k = 0; k < 2; k++) {
		x = eye[k].x, y = eye[k].y;
		aim(k, px, py, ox, oy);
		if (!force && eye[k].drawn && x == eye[k].x && y == eye[k].y)
			continue;
		if (eye[k].drawn) {
			int nx = eye[k].x, ny = eye[k].y;

			eye[k].x = x, eye[k].y = y;
			pupil(k, WHITE);
			eye[k].x = nx, eye[k].y = ny;
		}
		pupil(k, MWRGB(24, 24, 32));
		GrSetGCForeground(ui_gc, WHITE);	/* the glint */
		GrFillEllipse(win, ui_gc, eye[k].x - eye[k].pr / 3, eye[k].y - eye[k].pr / 3, eye[k].pr / 5, eye[k].pr / 5);
		eye[k].drawn = 1;
	}
}

static void
draw(void)
{
	int k, t;

	ui_fill(win, 0, 0, width, height, UI_FACE);
	for (k = 0; k < 2; k++) {
		t = outline(k);
		GrSetGCForeground(ui_gc, MWRGB(32, 32, 40));
		GrFillEllipse(win, ui_gc, eye[k].cx, eye[k].cy, eye[k].rx, eye[k].ry);
		if (shut) {
			/* the lid, shut: a skin-coloured eye with a line across */
			GrSetGCForeground(ui_gc, ui_seven ? MWRGB(236, 200, 170) : MWRGB(224, 184, 150));
			GrFillEllipse(win, ui_gc, eye[k].cx, eye[k].cy, eye[k].rx - t, eye[k].ry - t);
			GrSetGCForeground(ui_gc, MWRGB(32, 32, 40));
			GrFillRect(win, ui_gc, eye[k].cx - eye[k].rx + t, eye[k].cy - t / 2, 2 * (eye[k].rx - t), t);
			continue;
		}
		GrSetGCForeground(ui_gc, WHITE);
		GrFillEllipse(win, ui_gc, eye[k].cx, eye[k].cy, eye[k].rx - t, eye[k].ry - t);
		eye[k].drawn = 0;
	}
	pupils(1);
}

static void
plan_blink(void)
{
	next_blink = now_ms() + 3000 + (unsigned)(rand() % 6000);
}

static void
menu(int x, int y)
{
	static struct ui_menu_item items[] = {
		{ "Blink", -1, CHOOSE_BLINK, NULL, 0, 0, NULL },
		{ "Close", -1, CHOOSE_CLOSE, NULL, 0, UI_MENU_LINE, NULL },
	};

	items[0].flags = blink ? UI_MENU_CHECK : 0;
	ui_menu_open(items, 2, x, y, 0, 0);
}

/* The next event, or GR_EVENT_TYPE_TIMEOUT after `ms`: the server is asked once
 * (GrPrepareSelect) and the wait is a select() on its socket, so a timeout costs it nothing. */
static GR_EVENT taken;
static int took;

static void
take(GR_EVENT *event)
{
	taken = *event;
	took = 1;
}

static void
next_event(GR_EVENT *event, int ms)
{
	static int asked, fd = -1;
	struct timeval limit;
	fd_set set;
	int most = 0;

	FD_ZERO(&set);
	took = 0;
	GrServiceSelect(&set, take);
	if (took)
		asked = 0;
	if (!took && !asked) {
		GrPrepareSelect(&most, &set);
		for (fd = most; fd > 0 && !FD_ISSET(fd, &set); fd--)
			continue;
		asked = 1;
	}
	if (!took) {
		GrFlush();	/* (what was drawn since the server was last asked) */
		FD_ZERO(&set);
		FD_SET(fd, &set);
		limit.tv_sec = ms / 1000, limit.tv_usec = ms % 1000 * 1000;
		if (select(fd + 1, &set, NULL, NULL, &limit) > 0) {
			GrServiceSelect(&set, take);
			asked = 0;
		}
	}
	if (took)
		*event = taken;
	else {
		memset(event, 0, sizeof *event);
		event->type = GR_EVENT_TYPE_TIMEOUT;
	}
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	unsigned moved_at, t;
	int fd, id, x, y, wait;
	void *words;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxeyes: no window system\n");
		return 1;
	}
	ui_init();
	fd = open("/dev/gpu", O_RDONLY);
	words = fd < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x01000000);
	if (fd >= 0)
		close(fd);
	if (words != MAP_FAILED)
		input = words;
	blink = reg_int(BLINK_KEY, 1);
	srand((unsigned)time(NULL));

	win = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Eyes", GR_ROOT_WINDOW_ID, -1, -1, width, height, UI_FACE);
	GrSelectEvents(win, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ |
		GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN);
	layout();
	GrMapWindow(win);
	pointer(&px, &py);
	moved_at = now_ms();
	plan_blink();

	for (;;) {
		t = now_ms();
		wait = t - moved_at < 1000 ? FAST_MS : t - moved_at < 11000 ? SLOW_MS : SLEEPY_MS;
		if (blink) {
			unsigned due = shut ? opened_at : next_blink;

			if ((int)(due - t) < wait)
				wait = (int)(due - t) < 1 ? 1 : (int)(due - t);
		}
		next_event(&event, wait);
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			if (id == CHOOSE_CLOSE)
				goto done;
			blink = !blink;
			reg_set_int(BLINK_KEY, blink);
			if (shut)
				shut = 0, draw();
			plan_blink();
			continue;
		case UI_MENU_TOOK:
		case UI_MENU_CLOSED:
			continue;
		}
		ui_origin_note(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_CLOSE_REQ:
			goto done;
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == win)
				draw();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.wid == win && (event.update.width != width || event.update.height != height) &&
			    event.update.width > 0 && event.update.height > 0) {
				width = event.update.width, height = event.update.height;
				layout();
				draw();
			} else if (event.update.wid == win)
				pupils(0);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid == win && (event.button.changebuttons & GR_BUTTON_R))
				menu(event.button.rootx, event.button.rooty);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.ch == 'q' || event.keystroke.ch == MWKEY_ESCAPE)
				goto done;
			break;
		}
		if (ui_restyle())
			draw();
		t = now_ms();
		if (blink && !shut && (int)(t - next_blink) >= 0) {
			shut = 1, opened_at = t + BLINK_SHUT_MS;
			draw();
		} else if (shut && (int)(t - opened_at) >= 0) {
			shut = 0;
			plan_blink();
			draw();
		}
		pointer(&x, &y);
		if (x != px || y != py) {
			px = x, py = y, moved_at = t;
			pupils(0);
		}
	}
done:
	GrClose();
	return 0;
}
