/* nxview: a picture viewer (docs/nanox.md): a PPM, PNG, JPEG, PGM, BMP, GIF or XPM picture as
 * large as fits, under a row of keys (Open, the folder's one before and after, a turn either
 * way, smaller, larger, Fit, Edit). A PNG or JPEG is decoded on the worker cores into a PPM
 * in memory (ui_image.h). nxview --decode FILE OUT.ppm decodes with no window. */
#include <dirent.h>
#include <sys/time.h>
#include "ui.h"
#include "ui_files.h"
#include "ui_image.h"

static GR_WINDOW_ID window;
static GR_IMAGE_ID image;
static GR_IMAGE_INFO info;
static const char *ppm;		/* the file, when the display draws it itself */
static const char *wrong;	/* why a PNG or JPEG picture cannot be shown */
static char decoded[64];	/* the PPM file made of one */
static int width, height;

static void
forget(void)
{
	if (decoded[0])
		unlink(decoded);
}

/* Decodes a PNG or JPEG file into a PPM file and says what that took. */
static const char *
decode(const char *from, const char *to, int *pw, int *ph)
{
	const char *asked = getenv("NXVIEW_WORKERS"), *why;
	int workers = asked ? atoi(asked) : mcw_most() > 3 ? mcw_most() : 3;	/* as many as the machine has (and the picture has work for) */
	struct timeval then, now;
	unsigned then_n, now_n;

	gettimeofday(&then, NULL);
	__asm__ volatile("rdcycle %0" : "=r"(then_n));
	why = ui_image_to_ppm(from, to, pw, ph, workers);
	__asm__ volatile("rdcycle %0" : "=r"(now_n));
	gettimeofday(&now, NULL);
	if (!why) {
		/* a sum of the pixels, for a test to compare with the same decoder's on another machine */
		unsigned sum = 0, n = 0;
		FILE *file = getenv("NXVIEW_SUM") ? fopen(to, "rb") : NULL;
		int c;

		while (file && (c = fgetc(file)) != EOF)
			sum = (sum << 5 | sum >> 27) ^ (unsigned)c, n++;
		if (file)
			fclose(file);
		fprintf(stderr, "nxview: %s, %d x %d: decoded in %ld ms, %u thousand instructions on this core, asking for %d workers",
			from, *pw, *ph, (long)((now.tv_sec - then.tv_sec) * 1000 + (now.tv_usec - then.tv_usec) / 1000), (now_n - then_n) / 1000, workers);
		if (file)
			fprintf(stderr, "; sum %08x of %u bytes", sum, n);
		fprintf(stderr, "\n");
	}
	return why;
}

static int
is_coded(const char *name)
{
	size_t n = strlen(name);

	return (n > 4 && (!strcasecmp(name + n - 4, ".png") || !strcasecmp(name + n - 4, ".jpg"))) || (n > 5 && !strcasecmp(name + n - 5, ".jpeg"));
}

/* ---- the window: a row of keys, the picture under it, a line of what it is ---- */

#define BAR 28
#define STATUS 18
#define DARK MWRGB(40, 40, 40)
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ | \
	GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP)

static const struct { const char *label; int key; } keys[] = {
	{ "Open", 'o' }, { "<", MWKEY_LEFT }, { ">", MWKEY_RIGHT }, { "Turn left", 'l' }, { "Turn right", 'r' },
	{ "-", '-' }, { "+", '+' }, { "Fit", 'f' }, { "Edit", 'e' },
};
#define KEYS ((int)(sizeof keys / sizeof keys[0]))
static int key_x[KEYS + 1], key_w[KEYS];
static char path[512], turned[64], notice[120];
static int zoom = 100;		/* percent of the size that fits */
static int pan_x, pan_y, held, held_x, held_y;
static int turns;		/* quarter turns to the right the picture is shown with */

static const char *
shown(void)
{
	return turns ? turned : ppm;
}

static void
draw(void)
{
	int room_h = height - BAR - STATUS, w = width, h = room_h, pw = info.width, ph = info.height, i;
	char text[700];

	if (turns & 1)
		pw = info.height, ph = info.width;
	ui_fill(window, 0, BAR, width, room_h, DARK);
	if (ppm) {
		/* whole, as large as fits, times the zoom; a drag moves it */
		w = pw * room_h > ph * width ? width : pw * room_h / ph;
		h = w * ph / pw;
		w = w * zoom / 100, h = h * zoom / 100;
		GrDrawImageFromFile(window, ui_gc, (width - w) / 2 + pan_x, BAR + (room_h - h) / 2 + pan_y, w, h, (char *)shown(), 0);
	} else if (image) {
		if (info.width * room_h > info.height * width)
			h = info.height * width / info.width;
		else
			w = info.width * room_h / info.height;
		GrDrawImageToFit(window, ui_gc, (width - w) / 2, BAR + (room_h - h) / 2, w, h, image);
	} else {
		ui_text(window, 10, BAR + 10, wrong ? "This is" : path[0] ? "cannot show this file" : "Open has the pictures", -1, WHITE, 0);
		if (wrong)
			ui_text(window, 10, BAR + 28, wrong, -1, WHITE, 0);
	}
	/* the keys and the line go over whatever of a large picture reaches them */
	ui_fill(window, 0, 0, width, BAR, UI_FACE);
	for (i = 0; i < KEYS; i++)
		ui_button(window, key_x[i], 3, key_w[i], 22, keys[i].label, 0);
	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_bevel(window, 0, height - STATUS, width, STATUS, 1);
	if (ppm || image)
		snprintf(text, sizeof text, "%.300s   %d x %d   %d%%   %s", path, pw, ph, zoom, notice);
	else
		snprintf(text, sizeof text, "%.300s   %s", path, notice);
	ui_text_fit(window, 6, height - STATUS + 3, width - 12, text, BLACK);
}

/* The picture turned: a PPM file of its own, made from the one that is shown unturned. */
static void
turn(int by)
{
	FILE *in, *out;
	unsigned char *from, *to;
	int w, h, most, x, y, nw, nh;

	if (!ppm) {
		snprintf(notice, sizeof notice, "only a PPM, PNG or JPEG picture is turned");
		return;
	}
	turns = (turns + by) & 3;
	pan_x = pan_y = 0;
	if (!turns || !(in = fopen(ppm, "rb")))
		return;
	if (fscanf(in, "P6 %d %d %d", &w, &h, &most) != 3) {
		fclose(in);
		turns = 0;
		return;
	}
	fgetc(in);
	from = malloc((size_t)w * (size_t)h * 3), to = malloc((size_t)w * (size_t)h * 3);
	if (!from || !to || fread(from, 3, (size_t)w * (size_t)h, in) != (size_t)w * (size_t)h) {
		fclose(in);
		free(from), free(to);
		turns = 0;
		return;
	}
	fclose(in);
	nw = turns & 1 ? h : w, nh = turns & 1 ? w : h;
	for (y = 0; y < nh; y++)
		for (x = 0; x < nw; x++) {
			int sx = turns == 1 ? y : turns == 2 ? w - 1 - x : w - 1 - y, sy = turns == 1 ? h - 1 - x : turns == 2 ? h - 1 - y : x;

			const unsigned char *s = from + (sy * w + sx) * 3;
			unsigned char *d = to + (y * nw + x) * 3;

			d[0] = s[0], d[1] = s[1], d[2] = s[2];
		}
	snprintf(turned, sizeof turned, "/tmp/nxview-%d-turned.ppm", (int)getpid());
	if ((out = fopen(turned, "wb")) != NULL) {
		fprintf(out, "P6\n%d %d\n255\n", nw, nh);
		fwrite(to, 3, (size_t)nw * (size_t)nh, out);
		fclose(out);
	} else
		turns = 0;
	free(from), free(to);
}

static void
forget_all(void)
{
	forget();
	if (turned[0])
		unlink(turned);
}

/* Takes a file as the picture to show; `first` is before there is a window to say so in. */
static void
take(const char *file)
{
	char title[600];
	int pw, ph;

	forget();
	if (image)
		GrFreeImage(image);
	image = 0, ppm = NULL, wrong = NULL, decoded[0] = 0;
	turns = pan_x = pan_y = 0, zoom = 100, notice[0] = 0;
	memset(&info, 0, sizeof info);
	snprintf(path, sizeof path, "%s", file);
	snprintf(title, sizeof title, "%s - Viewer", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	if (window)
		GrSetWindowTitle(window, title);
	if (is_coded(path)) {
		/* what it is doing, first: decoding is seconds, and a window with nothing new looks stuck */
		if (window) {
			ui_fill(window, 0, BAR, width, height - BAR - STATUS, DARK);
			ui_text(window, 10, BAR + 10, "decoding it...", -1, WHITE, 0);
			GrFlush();
		}
		snprintf(decoded, sizeof decoded, "/tmp/nxview-%d.ppm", (int)getpid());
		wrong = decode(path, decoded, &pw, &ph);
		if (wrong)
			decoded[0] = 0;
		else
			ppm = decoded, info.width = pw, info.height = ph;
	} else if (ui_picture_size(path, &pw, &ph)) {
		ppm = path, info.width = pw, info.height = ph;
	} else if ((image = GrLoadImageFromFile(path, 0)) != 0) {
		GrGetImageInfo(image, &info);
	}
}

static int
is_picture(const char *name)
{
	static const char *const ends[] = { ".ppm", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".pgm", ".xpm" };
	size_t n = strlen(name), i;

	for (i = 0; i < sizeof ends / sizeof ends[0]; i++)
		if (n > strlen(ends[i]) && !strcasecmp(name + n - strlen(ends[i]), ends[i]))
			return 1;
	return 0;
}

static int
picture_entry(const struct dirent *entry)
{
	return is_picture(entry->d_name);
}

static int
by_name(const struct dirent **a, const struct dirent **b)
{
	return strcasecmp((*a)->d_name, (*b)->d_name);
}

/* The picture `by` after this one in its folder, by name, round the end to the start. */
static void
next(int by)
{
	char where[512], file[1024];
	const char *slash = strrchr(path, '/'), *leaf = slash ? slash + 1 : path;
	struct dirent **names;
	int n, i, at = -1, count = 0, pictures[512];

	snprintf(where, sizeof where, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
	/* (only the pictures are kept and sorted: a folder may have hundreds of other names) */
	n = scandir(where[0] ? where : "/", &names, picture_entry, by_name);
	for (i = 0; i < n && count < 512; i++) {
		if (!strcmp(names[i]->d_name, leaf))
			at = count;
		pictures[count++] = i;
	}
	if (count > 1 || (count == 1 && at < 0)) {
		at = at < 0 ? 0 : (at + by + count) % count;
		snprintf(file, sizeof file, "%s/%s", where, names[pictures[at]]->d_name);
		take(file);
		snprintf(notice, sizeof notice, "%d of %d", at + 1, count);
	} else
		snprintf(notice, sizeof notice, "the only picture here");
	for (i = 0; i < n; i++)
		free(names[i]);
	if (n >= 0)
		free(names);
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw();
}

/* A key, or the key of the row that stands for it. False to leave. */
static int
key(int ch)
{
	char chosen[512];

	notice[0] = 0;
	switch (ch) {
	case 'q': case MWKEY_ESCAPE:
		return 0;
	case 'o':
		if (ui_choose_file("Open a picture", "/root/Pictures", ".ppm .png .jpg .jpeg .gif .bmp", chosen, sizeof chosen, others))
			take(chosen);
		break;
	case MWKEY_LEFT: case MWKEY_PAGEUP: next(-1); break;
	case MWKEY_RIGHT: case MWKEY_PAGEDOWN: case ' ': next(1); break;
	case 'l': turn(-1); break;
	case 'r': turn(1); break;
	case '-': zoom = zoom > 25 ? zoom / 2 : 25; break;
	case '+': case '=': zoom = zoom < 800 ? zoom * 2 : 800; break;
	case 'f': case '1': zoom = 100, pan_x = pan_y = 0; break;
	case 'e':
		/* the paint program with this file: what it saves is a PPM beside it */
		if (path[0] && real_vfork() == 0) {
			execlp("nxpaint", "nxpaint", path, (char *)NULL);
			_exit(127);
		}
		break;
	default:
		return 1;
	}
	if (zoom <= 100)
		pan_x = pan_y = 0;
	draw();
	return 1;
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	GR_SCREEN_INFO screen;
	int pw, ph, i, x;

	if (argc > 3 && !strcmp(argv[1], "--decode")) {
		const char *why = decode(argv[2], argv[3], &pw, &ph);

		if (why)
			fprintf(stderr, "nxview: %s: %s\n", argv[2], why);
		return why != NULL;
	}
	if (GrOpen() < 0)
		return 1;
	ui_init();
	screen = *ui_screen();
	for (i = 0, x = 4; i < KEYS; i++) {
		key_x[i] = x, key_w[i] = ui_width(&ui_var_widths, ui_var, keys[i].label, -1) + 14;
		x += key_w[i] + (i == 0 || i == 2 || i == 4 || i == 7 ? 10 : 2);
	}
	key_x[KEYS] = x;
	/* the window first, as large as most pictures want; a picture is fitted into it */
	width = screen.cols - 60 < 560 ? screen.cols - 60 : 560;
	height = screen.rows - 110 < 440 ? screen.rows - 110 : 440;
	if (width < key_x[KEYS])
		width = key_x[KEYS];
	atexit(forget_all);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Viewer", GR_ROOT_WINDOW_ID, -1, -1, width, height, DARK);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	if (argc > 1 && is_coded(argv[1])) {
		/* seconds of decoding: the window framed and saying so first (docs/nanox.md) */
		ui_framed();
		draw();
		take(argv[1]);
		draw();
	} else if (argc > 1) {
		take(argv[1]);	/* (nothing to wait for: the window's first exposure draws it) */
	}
	if (getenv("NXVIEW_PRINT")) {
		for (i = 0; i < KEYS; i++)
			printf("nxview: key %s at %d\n", keys[i].label, key_x[i] + key_w[i] / 2);
		printf("nxview: %s, %d x %d%s\n", path, info.width, info.height, ppm ? "" : image ? " (not a PPM)" : " (not shown)");
		fflush(stdout);
	}
	for (;;) {
		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE) {
				width = event.update.width;
				height = event.update.height;
				draw();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event)) {
				key(ui_wheel_sum(&event) < 0 ? '+' : '-');
				break;
			}
			if (event.button.y < BAR) {
				for (i = 0; i < KEYS; i++)
					if (event.button.x >= key_x[i] && event.button.x < key_x[i] + key_w[i] && !key(keys[i].key))
						return 0;
				if (getenv("NXVIEW_PRINT")) {
					printf("nxview: %s, %d x %d, turned %d, %d%%, %s\n", path, info.width, info.height, turns, zoom, notice);
					fflush(stdout);
				}
			} else {
				held = 1, held_x = event.button.x - pan_x, held_y = event.button.y - pan_y;
				ui_moves(window, EVENTS, 1);	/* (the pointer's moves, while the button is held) */
			}
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			/* a picture larger than its room is moved about by a drag */
			if (held && (event.mouse.buttons & GR_BUTTON_L) && zoom > 100) {
				pan_x = event.mouse.x - held_x, pan_y = event.mouse.y - held_y;
				draw();
			} else
				held = 0;
			if (!event.mouse.buttons)
				ui_moves(window, EVENTS, 0);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			held = 0;
			ui_moves(window, EVENTS, 0);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (key(event.keystroke.ch))
				break;
			/* Escape and q close it */
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
