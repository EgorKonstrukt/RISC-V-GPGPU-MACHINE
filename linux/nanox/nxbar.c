/*
 * nxbar: a bar along the bottom of the screen with a Start button and its menu of programs,
 * a button for every open window, and a clock.
 *
 * The menu is whatever the files /usr/share/nxapps.* list, one "Label=command" a line, or
 * "Folder/Label=command" for a program in a folder (a folder may be in a folder): a program's
 * build adds itself by putting such a file in the image. A folder's programs open to its side
 * (../apps/ui_menu.h). Clicking a window's button brings the window to the front; the right
 * button there offers to minimise or close it.
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define MWINCLUDECOLORS
#include "nano-X.h"
#include "../apps/ui_icons.h"
#include "../apps/ui_menu.h"

#define BAR_HEIGHT 26
#define START_WIDTH 64
#define CLOCK_WIDTH 52
#define TASK_WIDTH 110
#define MAX_ITEMS 96
#define MAX_TASKS 12
#define REGISTRY "/usr/share"
#define FACE MWRGB(192, 192, 192)
#define SHADOW MWRGB(128, 128, 128)
#define TASK_RESTORE 1000	/* ids of a window's own menu; a program's is its line's number plus one */
#define TASK_MINIMISE 1001
#define TASK_CLOSE 1002

static struct { char path[64], command[64]; } items[MAX_ITEMS];	/* path: Folder/Folder/Label */
static int item_count;
/* The menu as ui_menu.h wants it: a folder's rows are one run of this, its own folders' after it. */
static struct ui_menu_item rows[MAX_ITEMS + 32];
static char row_names[MAX_ITEMS + 32][32];
static struct ui_icon_ref row_icons[MAX_ITEMS + 32];
static int row_count;
static const struct ui_menu_item *top;
static int top_count;
static struct { GR_WINDOW_ID frame, client; char title[20]; int hidden; struct ui_icon_ref icon; } tasks[MAX_TASKS];
static int task_count, task_asked = -1;

static GR_WINDOW_ID bar;
static GR_GC_ID gc;
static GR_SCREEN_INFO screen;

/* The rows of the folder whose path is `prefix` ("" or "Games/"): its folders in the order they
 * were first named, then its programs. Returns how many, and where they are. */
static int
build(const char *prefix, const struct ui_menu_item **out)
{
	int first = row_count, count = 0, length = strlen(prefix), pass, i, j, n;

	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < item_count && row_count < MAX_ITEMS + 32; i++) {
			const char *rest = items[i].path + length, *slash = strchr(rest, '/');
			char lower[32];

			if (strncmp(items[i].path, prefix, length) != 0 || (slash != NULL) != (pass == 0))
				continue;
			n = slash ? (int)(slash - rest) : (int)strlen(rest);
			for (j = first; slash && j < first + count; j++)
				if ((int)strlen(row_names[j]) == n && !strncmp(row_names[j], rest, n))
					break;
			if (slash && j < first + count)
				continue;	/* a folder named before */
			snprintf(row_names[row_count], sizeof row_names[0], "%.*s", n, rest);
			for (j = 0; row_names[row_count][j]; j++)
				lower[j] = tolower((unsigned char)row_names[row_count][j]);
			lower[j] = 0;
			rows[row_count].label = row_names[row_count];
			rows[row_count].id = slash ? 0 : i + 1;
			/* a program's own icon (its file's), a folder's by its name */
			row_icons[row_count].file[0] = 0;
			if (!slash)
				ui_icon_of_command(items[i].command, &row_icons[row_count]);
			else
				row_icons[row_count].cell = ui_icon_of(lower) >= 0 ? ui_icon_of(lower) : ui_icon_of("folder");
			rows[row_count].icon = row_icons[row_count].cell;
			rows[row_count].icon_file = row_icons[row_count].file;
			rows[row_count].sub = NULL;
			rows[row_count].sub_count = 0;
			rows[row_count].flags = 0;
			row_count++;
			count++;
		}
	for (j = first; j < first + count; j++)
		if (rows[j].id == 0) {
			char deeper[80];

			snprintf(deeper, sizeof deeper, "%s%s/", prefix, row_names[j]);
			rows[j].sub_count = build(deeper, &rows[j].sub);
		}
	*out = rows + first;
	return count;
}

/* The menu: every line of every registry file, files in name order. */
static void
load_items(void)
{
	struct dirent **names;
	int n = scandir(REGISTRY, &names, NULL, alphasort), i;

	for (i = 0; i < n; i++) {
		char path[300], line[128];
		FILE *file;

		if (strncmp(names[i]->d_name, "nxapps.", 7) != 0)
			continue;
		snprintf(path, sizeof path, REGISTRY "/%s", names[i]->d_name);
		file = fopen(path, "r");
		while (file && item_count < MAX_ITEMS && fgets(line, sizeof line, file)) {
			char *command = strchr(line, '=');

			line[strcspn(line, "\r\n")] = 0;
			if (!command || command == line || !command[1])
				continue;
			*command++ = 0;
			snprintf(items[item_count].path, sizeof items[0].path, "%s", line);
			snprintf(items[item_count].command, sizeof items[0].command, "%s", command);
			item_count++;
		}
		if (file)
			fclose(file);
	}
	top_count = build("", &top);
}

/* A rectangle with a lit top-left and a shaded bottom-right edge, or the reverse. */
static void
bevel(GR_WINDOW_ID w, int x, int y, int width, int height, int pressed)
{
	GrSetGCForeground(gc, pressed ? SHADOW : WHITE);
	GrLine(w, gc, x, y, x + width - 1, y);
	GrLine(w, gc, x, y, x, y + height - 1);
	GrSetGCForeground(gc, pressed ? WHITE : SHADOW);
	GrLine(w, gc, x, y + height - 1, x + width - 1, y + height - 1);
	GrLine(w, gc, x + width - 1, y, x + width - 1, y + height - 1);
}

/* A task button's width: TASK_WIDTH, or less when that many would not fit before the clock. */
static int
task_width(void)
{
	int room = screen.cols - CLOCK_WIDTH - 6 - (START_WIDTH + 12);
	int width = task_count ? room / task_count - 4 : TASK_WIDTH;

	return width > TASK_WIDTH ? TASK_WIDTH : width < 8 ? 8 : width;
}

static int
task_x(int i)
{
	return START_WIDTH + 12 + i * (task_width() + 4);
}

static void
draw_bar(void)
{
	char text[16];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	int i, width = task_width();

	GrSetGCForeground(gc, FACE);
	GrFillRect(bar, gc, 0, 0, screen.cols, BAR_HEIGHT);
	GrSetGCForeground(gc, WHITE);
	GrLine(bar, gc, 0, 0, screen.cols - 1, 0);
	bevel(bar, 3, 3, START_WIDTH, BAR_HEIGHT - 6, ui_menu_is_open() && task_asked < 0);
	ui_icon(bar, gc, 8, 5, ui_icon_of("start"), 16);
	GrSetGCForeground(gc, BLACK);
	GrText(bar, gc, 28, 7, (void *)"Start", 5, GR_TFASCII | GR_TFTOP);
	GrText(bar, gc, 29, 7, (void *)"Start", 5, GR_TFASCII | GR_TFTOP);	/* twice: bold */
	for (i = 0; i < task_count; i++) {
		int count = strlen(tasks[i].title), w, h, b;

		/* as much of the title as the button has room for */
		for (; count > 0; count--) {
			GrGetGCTextSize(gc, tasks[i].title, count, GR_TFASCII, &w, &h, &b);
			if (w <= width - 28)
				break;
		}
		bevel(bar, task_x(i), 3, width, BAR_HEIGHT - 6, 0);
		if (width >= 28)
			ui_icon_draw(bar, gc, task_x(i) + 4, 5, &tasks[i].icon, 16);
		GrSetGCForeground(gc, tasks[i].hidden ? SHADOW : BLACK);	/* minimised: greyed */
		GrText(bar, gc, task_x(i) + 24, 7, tasks[i].title, count, GR_TFASCII | GR_TFTOP);
	}
	bevel(bar, screen.cols - CLOCK_WIDTH - 3, 3, CLOCK_WIDTH, BAR_HEIGHT - 6, 1);
	snprintf(text, sizeof(text), "%02d:%02d", tm->tm_hour, tm->tm_min);
	GrSetGCForeground(gc, BLACK);
	GrText(bar, gc, screen.cols - CLOCK_WIDTH + 8, 7, text, strlen(text), GR_TFASCII | GR_TFTOP);
}

/* The icon of the program a window is of: its file's own, or its name's in the sheets. It is
 * looked for once a window: this is asked every second. */
static const struct ui_icon_ref *
icon_of_frame(GR_WINDOW_ID frame, GR_WINDOW_ID client)
{
	static struct { GR_WINDOW_ID frame; struct ui_icon_ref icon; } known[MAX_TASKS];
	static int next;
	GR_WINDOW_INFO info;
	char path[40], program[96];
	int i, n;

	for (i = 0; i < MAX_TASKS; i++)
		if (known[i].frame == frame)
			return &known[i].icon;
	GrGetWindowInfo(client, &info);
	snprintf(path, sizeof path, "/proc/%d/exe", info.processid);
	n = (int)readlink(path, program, sizeof program - 1);
	program[n > 0 ? n : 0] = 0;
	i = next++ % MAX_TASKS;
	known[i].frame = frame;
	ui_icon_of_program(program, "default", &known[i].icon);
	return &known[i].icon;
}

static int
by_frame(const void *a, const void *b)
{
	return (int)*(const GR_WINDOW_ID *)a - (int)*(const GR_WINDOW_ID *)b;
}

/*
 * The open windows: the root's children but our own two, once they have been seen on the
 * screen (one that is not shown now is minimised). A decorated window is a frame the window
 * manager made around the program's window, which is the one with the title. Kept in the
 * order the windows were made, so buttons stay where they are. True if anything changed.
 */
static int
find_tasks(void)
{
	static GR_WINDOW_ID before[MAX_TASKS], seen[64];
	static int before_hidden[MAX_TASKS], before_count = -1, seen_count;
	GR_WINDOW_ID still[64];
	int still_count = 0, k;
	GR_WINDOW_ID parent, *children = NULL, *inside;
	GR_COUNT n = 0, m, i;
	int changed;

	task_count = 0;
	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	if (n > 1)
		qsort(children, n, sizeof *children, by_frame);
	for (i = 0; i < n && task_count < MAX_TASKS; i++) {
		GR_WINDOW_INFO info;
		GR_WM_PROPERTIES props;
		GR_WINDOW_ID client = children[i];

		if (children[i] == bar)
			continue;
		GrGetWindowInfo(children[i], &info);
		if (info.props & GR_WM_PROPS_NODECORATE)
			continue;
		for (k = 0; k < seen_count && seen[k] != children[i]; k++)
			;
		if (!info.mapped && k == seen_count)
			continue;	/* never shown yet */
		if (still_count < 64)
			still[still_count++] = children[i];
		inside = NULL;
		m = 0;
		GrQueryTree(children[i], &parent, &inside, &m);
		if (m > 0)
			client = inside[0];
		free(inside);
		props.title = NULL;
		GrGetWMProperties(client, &props);
		tasks[task_count].frame = children[i];
		tasks[task_count].client = client;
		tasks[task_count].hidden = !info.mapped;
		tasks[task_count].icon = *icon_of_frame(children[i], client);
		snprintf(tasks[task_count].title, sizeof tasks[0].title, "%.13s", props.title ? (char *)props.title : "window");
		free(props.title);
		task_count++;
	}
	free(children);
	memcpy(seen, still, sizeof still);
	seen_count = still_count;
	changed = task_count != before_count;
	for (i = 0; i < task_count && !changed; i++)
		changed = before[i] != tasks[i].frame || before_hidden[i] != tasks[i].hidden;
	for (i = 0; i < task_count; i++) {
		before[i] = tasks[i].frame;
		before_hidden[i] = tasks[i].hidden;
	}
	before_count = task_count;
	return changed;
}

/* Opens the Start menu, its first row picked out when a key opened it, or closes whatever
 * menu is open. Open, it has the keyboard; closed, the window system hands that back. */
static void
show_menu(int open, int keyboard)
{
	task_asked = -1;
	if (open)
		ui_menu_open(top, top_count, 0, screen.rows - BAR_HEIGHT, 1, keyboard);
	else
		ui_menu_close();
	draw_bar();
}

/* A window's own menu, above its button. */
static void
show_task_menu(int task, int x)
{
	static struct ui_menu_item menu[3];

	menu[0].label = tasks[task].hidden ? "Restore" : "Bring to front";
	menu[0].id = TASK_RESTORE;
	menu[1].label = "Minimise";
	menu[1].id = TASK_MINIMISE;
	menu[1].flags = tasks[task].hidden ? UI_MENU_GREY : 0;
	menu[2].label = "Close";
	menu[2].id = TASK_CLOSE;
	menu[2].flags = UI_MENU_LINE;
	menu[0].icon = menu[1].icon = menu[2].icon = -1;
	ui_menu_open(menu, 3, x, screen.rows - BAR_HEIGHT, 1, 0);
	task_asked = task;
}

static void
task_front(int task)
{
	if (tasks[task].hidden)
		GrMapWindow(tasks[task].frame);
	GrRaiseWindow(tasks[task].frame);
	GrSetFocus(tasks[task].client);
}

/* Starts a command: words separated by spaces, no shell (which would cost half a second). */
static void
launch(const char *command)
{
	char text[64], *words[8];
	int n = 0;

	if (fork() != 0)
		return;
	snprintf(text, sizeof text, "%s", command);
	for (words[n] = strtok(text, " "); words[n] && n < 6; words[n] = strtok(NULL, " "))
		n++;
	words[n] = NULL;
	if (n)
		execvp(words[0], words);
	_exit(127);
}

/* "Open this" from the host (docs/open.md): a count among the machine's control words, and an
 * address in two runs of them. The last count acted on is kept in a file, for a bar started
 * again, and each new one starts `open` with the address. */
#define OPEN_COUNT 144		/* word indices: 0x87000240 */
#define OPEN_FIRST 148		/* 176 bytes of the address */
#define OPEN_MORE 196		/* and 80 more */
#define OPEN_SEEN "/tmp/nxopen.count"

static void
watch_open(void)
{
	static volatile uint32_t *regs;
	static uint32_t seen;
	static int tried;
	char address[257];
	uint32_t count, length;
	FILE *file;

	if (!tried) {
		int fd = open("/dev/gpu", O_RDWR);

		tried = 1;
		if (fd >= 0)
			regs = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x01000000);
		if (regs == MAP_FAILED)
			regs = NULL;
		if ((file = fopen(OPEN_SEEN, "r")) != NULL) {
			if (fscanf(file, "%u", &seen) != 1)
				seen = 0;
			fclose(file);
		}
	}
	if (!regs || (count = regs[OPEN_COUNT]) == seen)
		return;
	seen = count;
	if ((file = fopen(OPEN_SEEN, "w")) != NULL) {
		fprintf(file, "%u\n", seen);
		fclose(file);
	}
	length = regs[OPEN_COUNT + 1];
	if (length == 0 || length > 255)
		return;
	memcpy(address, (const void *)&regs[OPEN_FIRST], 176);
	memcpy(address + 176, (const void *)&regs[OPEN_MORE], 80);
	address[length] = 0;
	if (fork() == 0) {
		execlp("open", "open", address, (char *)NULL);
		_exit(127);
	}
}

/* What a menu's row does: a program is started, a window brought forward, put away or closed. */
static void
chosen(int id)
{
	int task = task_asked;

	task_asked = -1;
	if (id >= 1 && id <= item_count)
		launch(items[id - 1].command);
	else if (task >= 0 && task < task_count && id == TASK_RESTORE)
		task_front(task);
	else if (task >= 0 && task < task_count && id == TASK_MINIMISE)
		GrUnmapWindow(tasks[task].frame);
	else if (task >= 0 && task < task_count && id == TASK_CLOSE)
		GrCloseWindow(tasks[task].client);
}

static unsigned
milliseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

int
main(void)
{
	GR_EVENT event;
	int minute = -1, id;
	unsigned closed_at = 0;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxbar: cannot reach the Nano-X server\n");
		return 1;
	}
	signal(SIGCHLD, SIG_IGN);	/* started programs are not ours to wait for */
	GrGetScreenInfo(&screen);
	gc = GrNewGC();
	GrSetGCUseBackground(gc, GR_FALSE);
	load_items();

	bar = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_NOFOCUS,
		"nxbar", GR_ROOT_WINDOW_ID, 0, screen.rows - BAR_HEIGHT, screen.cols, BAR_HEIGHT, FACE);
	GrSelectEvents(bar, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN);
	GrMapWindow(bar);
	/* the Windows key, whatever window has the keyboard */
	GrGrabKey(bar, MWKEY_LMETA, GR_GRAB_HOTKEY);
	GrGrabKey(bar, MWKEY_RMETA, GR_GRAB_HOTKEY);
	/* NXBAR_SHOW=1 starts with the menu open, NXBAR_SHOW=Folder with that folder's beside it: for a test without a pointer */
	if (getenv("NXBAR_SHOW")) {
		show_menu(1, 0);
		ui_menu_enter_named(getenv("NXBAR_SHOW"));
	}

	for (;;) {
		static time_t looked;
		time_t now;
		int i;

		GrGetNextEventTimeout(&event, 1000);
		switch (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.hotkey ? UI_MENU_NOT : ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			chosen(id);
			/* fall through */
		case UI_MENU_CLOSED:
			closed_at = milliseconds();
			draw_bar();
			/* fall through */
		case UI_MENU_TOOK:
			event.type = GR_EVENT_TYPE_NONE;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == bar)
				draw_bar();
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.hotkey)
				show_menu(!ui_menu_is_open(), 1);	/* opened by a key: the first row is picked out */
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if ((event.button.buttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)) || event.button.wid != bar)
				break;
			if (event.button.x < START_WIDTH + 6) {
				/* the press that closed the menu a moment ago was this one: it stays closed */
				if (ui_menu_is_open() || milliseconds() - closed_at > 300)
					show_menu(!ui_menu_is_open(), 0);
				break;
			}
			for (i = 0; i < task_count; i++)
				if (event.button.x >= task_x(i) && event.button.x < task_x(i) + task_width()) {
					if (event.button.buttons & GR_BUTTON_R)
						show_task_menu(i, task_x(i));
					else
						task_front(i);
				}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid != bar)
				break;	/* a menu's, come after the menu went */
			GrClose();
			return 0;
		}
		/* once a second: windows that came or went, and the minute */
		now = time(NULL);
		if (now == looked)
			continue;
		looked = now;
		watch_open();
		if (find_tasks() || localtime(&now)->tm_min != minute) {
			minute = localtime(&now)->tm_min;
			draw_bar();
		}
	}
}
