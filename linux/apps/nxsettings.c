/*
 * nxsettings: the desktop's settings, a tab each. Desktop: what the desktop shows (a picture
 * of the image's, any picture file, or a colour). Screen: its size. Sound: how loud. Storage:
 * what the host keeps (docs/persist.md). System: what the machine is. From a script: nxsettings apply | size WxH | volume 0-100 | choose [DIR] | system.
 */
#include <dirent.h>
#include <setjmp.h>
#include <signal.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include "ui.h"
#include "ui_files.h"
#include "ui_ask.h"
#include "shaderemu_sound.h"

#define WIDTH 380
#define HEIGHT 300
#define TAB_W 72
#define TOP 36			/* where a tab's own part begins */
#define MAX_CHOICES 64
#define SIZE_CHOICE "desktop.size"	/* the registry's key for the screen size the desktop starts with: WIDTHxHEIGHT (the nx script reads it) */

enum { TAB_DESKTOP, TAB_SCREEN, TAB_SOUND, TAB_STORAGE, TAB_SYSTEM, TABS };
static const char *tab_names[TABS] = { "Desktop", "Screen", "Sound", "Storage", "System" };
static const char *const storage_names[3] = { "Save now", "Load at start: on", "Clear the kept data" };
static int kept_bytes = -1, kept_most, kept_auto = 1;	/* what the host keeps of /persistence; -1: it keeps nothing */

static struct { char label[64], value[512]; } choices[MAX_CHOICES];
static struct ui_list pictures = { 8, TOP, WIDTH - 16, 196, 0, 0, -1 };
static const char *sizes[] = { "640x480", "800x600", "1024x576", "1024x768", "1280x720", "1600x900", "1920x1080" };
#define SIZES 7
static struct ui_list screens = { 8, TOP + 20, 170, SIZES * UI_ROW + 4, 0, 0, -1 };
static int fits[SIZES], most_w = 1280, most_h = 720;	/* the sizes this host can show, and its largest */
static int tab, have_sound = -1;	/* have_sound: not asked yet */
static int cores = 1, has_float = -1;	/* the machine's cores (its control words say); float instructions: not tried yet */
static unsigned speed_cycles, speed_ms, speed_now;	/* instructions and time at the last look; instructions a second since */
static int focus;	/* what the keyboard works: 0 the row of tabs, 1 and 2 the tab's controls in order */
static GR_WINDOW_ID window;
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ)

static void
add(const char *label, const char *value)
{
	if (pictures.count == MAX_CHOICES)
		return;
	snprintf(choices[pictures.count].label, sizeof choices[0].label, "%s", label);
	snprintf(choices[pictures.count].value, sizeof choices[0].value, "%s", value);
	pictures.count++;
}

/* The pictures in the image, named by what follows "wallpaper-", then some colours. */
static void
find_choices(void)
{
	struct dirent **names;
	int n = scandir("/usr/share", &names, NULL, alphasort), i;
	char label[64], path[300];

	for (i = 0; i < n; i++) {
		const char *name = names[i]->d_name;
		char *dot;

		if (strncmp(name, "wallpaper-", 10) != 0)
			continue;
		snprintf(label, sizeof label, "Picture: %s", name + 10);
		if ((dot = strrchr(label, '.')) != NULL)
			*dot = 0;
		snprintf(path, sizeof path, "/usr/share/%s", name);
		add(label, path);
	}
	add("Colour: teal", "#008080");
	add("Colour: slate", "#304050");
	add("Colour: black", "#000000");
}

static const char *
picture_label(int row)
{
	return choices[row].label;
}

/* The host's largest screen is in its flags, among the machine's control words (docs/gpu.md):
 * width and height in 16s of pixels. A host that does not say shows 1280 x 720. */
static void
find_sizes(void)
{
	int gpu = open("/dev/gpu", O_RDWR), i;
	const unsigned *words = gpu < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000);

	/* how many cores the machine has, or could have (docs/multicore.md: the geometry's words) */
	if (words != MAP_FAILED && (words[0x3cc / 4] || words[0x3c0 / 4]))
		cores = (int)(words[0x3cc / 4] ? words[0x3cc / 4] : words[0x3c0 / 4]);
	if (words != MAP_FAILED && (words[0x3c / 4] >> 8 & 255)) {
		most_w = (words[0x3c / 4] >> 8 & 255) * 16;
		most_h = (words[0x3c / 4] >> 16 & 255) * 16;
	}
	if (words != MAP_FAILED)
		munmap((void *)words, 4096);
	if (gpu >= 0)
		close(gpu);
	for (i = 0; i < SIZES; i++)
		if (atoi(sizes[i]) <= most_w && atoi(strchr(sizes[i], 'x') + 1) <= most_h)
			fits[screens.count++] = i;
}

static const char *
size_label(int row)
{
	static char text[40];
	int w = atoi(sizes[fits[row]]), h = atoi(strchr(sizes[fits[row]], 'x') + 1);

	snprintf(text, sizeof text, "%d x %d%s", w, h, w == ui_screen()->cols && h == ui_screen()->rows ? "  (now)" : "");
	return text;
}

/* A number that follows `name` in a file of /proc. */
static long
field(const char *path, const char *name)
{
	FILE *file = fopen(path, "r");
	char line[160];
	long value = 0;

	while (file && fgets(line, sizeof line, file))
		if (!strncmp(line, name, strlen(name)))
			value = atol(line + strlen(name));
	if (file)
		fclose(file);
	return value;
}

static sigjmp_buf no_float;

static void
float_trap(int signal_number)
{
	(void)signal_number;
	siglongjmp(no_float, 1);
}

/* Whether the processor has float instructions: one is run, and it is an illegal one or not.
 * (Linux's own word on it, /proc/cpuinfo, is its device tree's, which does not name them.) */
static int
float_unit(void)
{
	void (*before)(int) = signal(SIGILL, float_trap);
	volatile int there = 0;

	if (!sigsetjmp(no_float, 1)) {
		__asm__ volatile(".word 0xd0007053");	/* fcvt.s.w ft0, zero */
		there = 1;
	}
	signal(SIGILL, before);
	return there;
}

static unsigned
cycles_now(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

static unsigned
ms_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* The first core's instructions a second since this was last asked (it counts them: rdcycle). */
static void
measure_speed(void)
{
	unsigned cycles = cycles_now(), ms = ms_now();

	if (speed_ms && ms - speed_ms >= 200)
		speed_now = (unsigned)((unsigned long long)(cycles - speed_cycles) * 1000 / (ms - speed_ms));
	if (!speed_ms || ms - speed_ms >= 200) {
		speed_cycles = cycles;
		speed_ms = ms;
	}
}

/* The network device's address, or NULL while it has none. */
static const char *
network_address(void)
{
	static char text[32];
	struct ifreq request;
	int s = socket(AF_INET, SOCK_DGRAM, 0), got;

	if (s < 0)
		return NULL;
	memset(&request, 0, sizeof request);
	strcpy(request.ifr_name, "lan0");
	got = ioctl(s, SIOCGIFADDR, &request);
	close(s);
	if (got != 0)
		return NULL;
	snprintf(text, sizeof text, "%s", inet_ntoa(((struct sockaddr_in *)&request.ifr_addr)->sin_addr));
	return text;
}

/* What the machine is, a line each: every one asked of it as it is now. Returns how many. */
static int
system_lines(char lines[][160], int screen_w, int screen_h)
{
	struct utsname system;
	const char *address;
	FILE *memory = fopen("/proc/meminfo", "r");
	char line[160];
	long total = 0, available = 0;
	int n = 0;

	/* (both numbers in one reading of the file: the kernel writes it out for each) */
	while (memory && fgets(line, sizeof line, memory))
		if (!strncmp(line, "MemTotal:", 9))
			total = atol(line + 9);
		else if (!strncmp(line, "MemAvailable:", 13))
			available = atol(line + 13);
	if (memory)
		fclose(memory);
	uname(&system);
	if (has_float < 0)
		has_float = float_unit();
	if (have_sound < 0)
		have_sound = snd_open() == 0;
	measure_speed();
	snprintf(lines[n++], 160, "%s %s", system.sysname, system.release);
	snprintf(lines[n++], 160, "Processor: %s (RV32IMA%s), %d core%s", system.machine,
		 has_float ? "F: with float instructions" : ": no float instructions", cores, cores == 1 ? "" : "s");
	if (cores > 1)
		snprintf(lines[n++], 160, "Linux runs on the first; the other %d are programs' workers", cores - 1);
	else
		snprintf(lines[n++], 160, "Linux and every program run on it");
	if (speed_now)
		snprintf(lines[n++], 160, "The first core ran %u.%02u million instructions a second just now", speed_now / 1000000,
			 speed_now / 10000 % 100);
	else
		snprintf(lines[n++], 160, "The first core's speed: measuring...");
	snprintf(lines[n++], 160, "Memory: %ld MB, %ld MB free", total / 1024, available / 1024);
	snprintf(lines[n++], 160, "Disk: a ROM of %ld MB; what is written is kept in memory", field("/sys/class/mtd/mtd0/size", "") >> 20);
	if (screen_w)
		snprintf(lines[n++], 160, "Screen: %d x %d (this host shows %d x %d at most)", screen_w, screen_h, most_w, most_h);
	else
		snprintf(lines[n++], 160, "Screen: this host shows %d x %d at most", most_w, most_h);
	if (have_sound > 0)
		snprintf(lines[n++], 160, "Sound card: %u samples a second, volume %d%%", (unsigned)SND_RATE, (int)(SND_MASTER * 100 / 256));
	else
		snprintf(lines[n++], 160, "Sound card: none");
	address = network_address();
	snprintf(lines[n++], 160, "Network: %s", address ? address : "not connected");
	snprintf(lines[n++], 160, "Running for %ld minutes", field("/proc/uptime", "") / 60);
	return n;
}

/* The desktop again, with another size: the window system starts over and every program on it
 * closes, this one too. The script that does it must not die with them; with `ask` it asks
 * first (and is the one to refuse a second press while it does). */
static void
restart_desktop(const char *size, int ask)
{
	if (!ask)
		reg_set(SIZE_CHOICE, size);
	if (fork() == 0) {
		setsid();
		execlp("nx", "nx", "restart", ask ? "ask" : (char *)NULL, size, (char *)NULL);
		_exit(127);
	}
}

/* The volume, as the next session will have it once the folder is saved. */
static void
keep_volume(void)
{
	reg_set_int("sound.volume", (int)(SND_MASTER * 100 / 256));
}

/* nxpersist with a word or two, waited for; its exit status. */
static int
persist(const char *what, const char *more)
{
	int status = -1;
	pid_t child = fork();

	if (child == 0) {
		execlp("nxpersist", "nxpersist", what, more, (char *)NULL);
		_exit(127);
	}
	return child > 0 && waitpid(child, &status, 0) == child ? status : -1;
}

/* What the host keeps, asked of it (nxpersist state). */
static void
read_kept(void)
{
	FILE *from = popen("nxpersist state", "r");
	char line[64];

	kept_bytes = -1;
	while (from && fgets(line, sizeof line, from)) {
		sscanf(line, "stored %d", &kept_bytes);
		sscanf(line, "limit %d", &kept_most);
		sscanf(line, "auto %d", &kept_auto);
	}
	if (from)
		pclose(from);
}

/* The Storage tab's three buttons. */
static void
storage(int which)
{
	static const char *const answers[] = { "Clear", "Cancel" };

	if (which == 0)
		persist("save", NULL);
	else if (which == 1)
		persist("auto", kept_auto ? "0" : "1");
	else if (ui_ask("Storage", "Clear what the host keeps of /persistence?\nThe folder itself stays until the machine is switched off.", answers, 2, NULL) == 0)
		persist("clear", NULL);
	read_kept();
}

/* A short tone through the card, to hear the volume by. */
static void
beep(void)
{
	static short tone[4410];
	struct snd_format format = { 22050, 1 };
	int file = open("/dev/sound", O_WRONLY), i;

	if (file < 0)
		return;
	for (i = 0; i < 4410; i++) {
		int phase = i * 880 % 22050, up = phase * 4 / 22050;	/* a triangle wave of 880 a second */
		int v = (up == 0 ? phase : up == 3 ? phase - 22050 : 11025 - phase) * 4;

		tone[i] = (short)(v * (4410 - i) / 4410 / 4);
	}
	ioctl(file, SND_IOCTL_FORMAT, &format);
	write(file, tone, sizeof tone);
	close(file);	/* returns once it has been played */
}

static char system_shown[12][160];	/* the System tab's lines as they were last drawn */
static int system_count;

/* The System tab's lines again, a second later: the ones that read otherwise are drawn. */
static void
draw_system_changes(void)
{
	static char lines[12][160];
	int n = system_lines(lines, ui_screen()->cols, ui_screen()->rows), i;

	for (i = 0; i < n || i < system_count; i++) {
		if (i < n && i < system_count && !strcmp(lines[i], system_shown[i]))
			continue;
		ui_fill(window, 0, TOP + i * 18, WIDTH, 18, UI_FACE);
		if (i < n)
			ui_text(window, 8, TOP + i * 18, lines[i], -1, BLACK, 0);
	}
	memcpy(system_shown, lines, sizeof lines);
	system_count = n;
}

static void
draw(void)
{
	GR_SCREEN_INFO screen = *ui_screen();
	char text[160];
	int i;

	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	for (i = 0; i < TABS; i++)
		ui_button(window, 8 + i * (TAB_W + 2), 6, TAB_W, 22, tab_names[i], i == tab);
	if (focus == 0)
		ui_focus(window, 8 + tab * (TAB_W + 2) + 4, 6 + 4, TAB_W - 8, 22 - 8);
	switch (tab) {
	case TAB_DESKTOP:
		ui_list_draw(window, &pictures, picture_label);
		ui_button(window, 8, TOP + 204, 180, 22, "Choose a picture file...", 0);
		if (focus == 1)
			ui_focus(window, pictures.x, pictures.y, pictures.width, pictures.height);
		else if (focus == 2)
			ui_focus(window, 8 + 4, TOP + 204 + 4, 180 - 8, 22 - 8);
		ui_text(window, 8, HEIGHT - 26, "A picture file is a PPM, PNG or JPEG (the last two are decoded first).", -1, UI_SHADOW, 0);
		break;
	case TAB_SCREEN:
		snprintf(text, sizeof text, "The screen is %d x %d now.", screen.cols, screen.rows);
		ui_text(window, 8, TOP, text, -1, BLACK, 0);
		ui_list_draw(window, &screens, size_label);
		ui_button(window, 190, TOP + 20, 180, 22, "Use the selected size", 0);
		if (focus == 1)
			ui_focus(window, screens.x, screens.y, screens.width, screens.height);
		else if (focus == 2)
			ui_focus(window, 190 + 4, TOP + 20 + 4, 180 - 8, 22 - 8);
		ui_text(window, 190, TOP + 52, "The desktop starts again:", -1, BLACK, 0);
		ui_text(window, 190, TOP + 68, "every open program closes.", -1, BLACK, 0);
		snprintf(text, sizeof text, "This machine's display shows %d x %d at most.", most_w, most_h);
		ui_text(window, 8, HEIGHT - 26, text, -1, UI_SHADOW, 0);
		break;
	case TAB_SOUND:
		if (have_sound <= 0) {
			ui_text(window, 8, TOP, "This machine has no sound card (or its host plays none).", -1, BLACK, 0);
			break;
		}
		snprintf(text, sizeof text, "Volume of everything the machine plays: %d%%", (int)(SND_MASTER * 100 / 256));
		ui_text(window, 8, TOP, text, -1, BLACK, 0);
		ui_slider(window, 8, TOP + 22, WIDTH - 16, (int)SND_MASTER, 256);
		ui_button(window, 8, TOP + 52, 90, 22, "Play a tone", 0);
		if (focus == 1)
			ui_focus(window, 8, TOP + 22, WIDTH - 16, 18);
		else if (focus == 2)
			ui_focus(window, 8 + 4, TOP + 52 + 4, 90 - 8, 22 - 8);
		snprintf(text, sizeof text, "The card plays %u samples a second.", (unsigned)SND_RATE);
		ui_text(window, 8, TOP + 90, text, -1, UI_SHADOW, 0);
		ui_text(window, 8, TOP + 106, "How loud the host plays it is set on the host.", -1, UI_SHADOW, 0);
		break;
	case TAB_STORAGE:
		if (kept_bytes < 0)
			snprintf(text, sizeof text, "This machine's host keeps nothing between sessions.");
		else if (kept_bytes == 0)
			snprintf(text, sizeof text, "The host keeps nothing of /persistence yet.");
		else
			snprintf(text, sizeof text, "The host keeps %d KB of /persistence (%d at most).", (kept_bytes + 999) / 1000, kept_most / 1000);
		ui_text(window, 8, TOP, text, -1, BLACK, 0);
		for (i = 0; i < 3; i++) {
			ui_button(window, 8, TOP + 24 + i * 28, 170, 22, i == 1 && !kept_auto ? "Load at start: off" : storage_names[i], 0);
			if (focus == i + 1)
				ui_focus(window, 8 + 4, TOP + 24 + i * 28 + 4, 170 - 8, 22 - 8);
		}
		ui_text(window, 8, HEIGHT - 44, "/persistence holds settings and saved games. It is kept", -1, UI_SHADOW, 0);
		ui_text(window, 8, HEIGHT - 26, "only when saved: here, or with the button by the computer.", -1, UI_SHADOW, 0);
		break;
	case TAB_SYSTEM: {
		static char lines[12][160];
		int n = system_lines(lines, screen.cols, screen.rows);

		for (i = 0; i < n; i++)
			ui_text(window, 8, TOP + i * 18, lines[i], -1, BLACK, 0);
		memcpy(system_shown, lines, sizeof lines);
		system_count = n;
		}
		ui_text(window, 8, HEIGHT - 44, "Settings are kept when saved (the Storage tab).", -1, UI_SHADOW, 0);
		ui_text(window, 8, HEIGHT - 26, "ShaderEmu: a RISC-V computer in a pixel shader", -1, UI_SHADOW, 0);
		break;
	}
}

/* Events of this window that come while the file chooser is up. */
static void
while_choosing(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE)
		draw();
}

static void
choose_picture(void)
{
	char path[512], label[64];
	const char *name;

	if (!ui_choose_file("Choose a picture", access("/root/Pictures", R_OK) == 0 ? "/root/Pictures" : "/usr/share",
			    ".ppm .png .jpg .jpeg", path, sizeof path, while_choosing))
		return;
	name = strrchr(path, '/') + 1;
	snprintf(label, sizeof label, "File: %s", name);
	if (!strstr(path, ".ppm")) {
		/* the viewer decodes it (on the worker cores) into a PPM file in memory, which is what the desktop shows */
		static int made;
		char decoded[64];
		pid_t child;
		int status = 1;

		snprintf(decoded, sizeof decoded, "/tmp/nxwallpaper-%d.ppm", made++);
		if ((child = fork()) == 0) {
			execlp("nxview", "nxview", "--decode", path, decoded, (char *)NULL);
			_exit(127);
		}
		if (child < 0 || waitpid(child, &status, 0) < 0 || status != 0)
			return;
		snprintf(path, sizeof path, "%s", decoded);
	}
	add(label, path);
	pictures.selected = pictures.count - 1;
	ui_list_show(&pictures);
	ui_wallpaper(path, 1);
}

/* Shows a tab; the keyboard stays on the row of tabs, or goes there. */
static void
show_tab(int which)
{
	tab = which;
	focus = 0;
	if (tab == TAB_SOUND && have_sound < 0)
		have_sound = snd_open() == 0;
	if (tab == TAB_STORAGE)
		read_kept();
	draw();
}

/* How many places the keyboard can be on the tab shown: the row of tabs, then its controls. */
static int
controls(void)
{
	return tab == TAB_SYSTEM || (tab == TAB_SOUND && have_sound <= 0) ? 1 : tab == TAB_STORAGE ? 4 : 3;
}

/* Tab goes round the row of tabs and the controls of the tab shown; arrows change the tab,
 * the list's row or the slider; Enter or Space presses a button. */
static void
key(const GR_EVENT *event)
{
	int ch = event->keystroke.ch, value;

	if (ui_tab(event, &focus, controls())) {
		draw();
		return;
	}
	if (focus == 0) {
		if (ch == MWKEY_LEFT || ch == MWKEY_RIGHT)
			show_tab((tab + (ch == MWKEY_LEFT ? TABS - 1 : 1)) % TABS);
		return;
	}
	switch (tab) {
	case TAB_DESKTOP:
		if (focus == 2 && ui_press_key(ch))
			choose_picture();
		else if (focus == 1 && (ui_list_key(&pictures, ch) || ui_press_key(ch)) && pictures.selected >= 0)
			ui_wallpaper(choices[pictures.selected].value, 1);
		else
			break;
		draw();
		break;
	case TAB_SCREEN:
		if (ui_press_key(ch) && screens.selected >= 0)
			restart_desktop(sizes[fits[screens.selected]], 1);
		else if (focus == 1 && ui_list_key(&screens, ch))
			draw();
		break;
	case TAB_SOUND:
		if (focus == 2 && ui_press_key(ch)) {
			beep();
			break;
		}
		value = (int)SND_MASTER;
		value = ch == MWKEY_LEFT ? value - 8 : ch == MWKEY_RIGHT ? value + 8 : ch == MWKEY_HOME ? 0 : ch == MWKEY_END ? 256 : -1;
		if (focus == 1 && value != -1) {
			SND_MASTER = value < 0 ? 0 : value > 256 ? 256 : value;
			keep_volume();
			draw();
		}
		break;
	case TAB_STORAGE:
		if (ui_press_key(ch)) {
			storage(focus - 1);
			draw();
		}
		break;
	}
}

static void
press(GR_EVENT *event)
{
	int down = event->type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(event);
	int x = down ? event->button.x : event->mouse.x, y = down ? event->button.y : event->mouse.y, how;

	if (down && y >= 6 && y < 28 && x >= 8 && (x - 8) / (TAB_W + 2) < TABS) {
		show_tab((x - 8) / (TAB_W + 2));
		return;
	}
	switch (tab) {
	case TAB_DESKTOP:
		how = ui_list_event(&pictures, event);
		if (how == 1 && event->type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(event) && pictures.selected >= 0 &&
		    x < pictures.x + pictures.width - UI_SCROLL_W - 2)
			ui_wallpaper(choices[pictures.selected].value, 1);
		if (how) {
			focus = 1;
			draw();
		} else if (down && ui_inside(x, y, 8, TOP + 204, 180, 22)) {
			choose_picture();
			draw();
		}
		break;
	case TAB_SCREEN:
		if (ui_list_event(&screens, event)) {
			focus = 1;
			draw();
		} else if (down && ui_inside(x, y, 190, TOP + 20, 180, 22) && screens.selected >= 0)
			restart_desktop(sizes[fits[screens.selected]], 1);
		break;
	case TAB_SOUND:
		if (have_sound <= 0)
			break;
		/* the slider follows a held button as well as a click */
		if ((down || (event->type == GR_EVENT_TYPE_MOUSE_MOTION && (event->mouse.buttons & GR_BUTTON_L))) &&
		    ui_inside(x, y, 8, TOP + 18, WIDTH - 16, 26)) {
			SND_MASTER = ui_slider_value(x, 8, WIDTH - 16, 256);
			keep_volume();
			draw();
		} else if (down && ui_inside(x, y, 8, TOP + 52, 90, 22)) {
			beep();
		}
		break;
	case TAB_STORAGE:
		for (how = 0; down && how < 3; how++)
			if (ui_inside(x, y, 8, TOP + 24 + how * 28, 170, 22)) {
				focus = how + 1;
				storage(how);
				draw();
			}
		break;
	}
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	char path[512];

	/* nxsettings system: what the System tab says, printed (it needs no desktop) */
	if (argc > 1 && !strcmp(argv[1], "system")) {
		static char lines[12][160];
		int i, n;

		find_sizes();
		system_lines(lines, 0, 0);
		usleep(300000);		/* (a speed is two looks at the count, some time apart) */
		n = system_lines(lines, 0, 0);
		for (i = 0; i < n; i++)
			printf("%s\n", lines[i]);
		return 0;
	}
	/* nxsettings volume N: how loud, without the desktop */
	if (argc > 2 && !strcmp(argv[1], "volume")) {
		if (snd_open())
			return 1;
		SND_MASTER = atoi(argv[2]) * 256 / 100;
		return 0;
	}
	if (GrOpen() < 0)
		return 1;
	ui_init();
	signal(SIGCHLD, SIG_DFL);	/* (the decoder is waited for: see ui_run in ui.h) */
	/* nxsettings apply: put up the desktop as last chosen, and go (the nx script does this) */
	if (argc > 1 && !strcmp(argv[1], "apply")) {
		ui_wallpaper_restore();
		GrFlush();
		GrClose();
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "size")) {
		restart_desktop(argv[2], 0);
		return 0;
	}
	/* nxsettings choose [FOLDER]: the file chooser by itself; prints what was chosen */
	if (argc > 1 && !strcmp(argv[1], "choose")) {
		if (ui_choose_file("Choose a file", argc > 2 ? argv[2] : "/", NULL, path, sizeof path, NULL))
			printf("%s\n", path);
		GrClose();
		return 0;
	}
	find_choices();
	find_sizes();
	if (getenv("NXSETTINGS_TAB")) {		/* the tab to open on, for tests */
		tab = atoi(getenv("NXSETTINGS_TAB")) % TABS;
		if (tab == TAB_SOUND)
			have_sound = snd_open() == 0;
		if (tab == TAB_STORAGE)
			read_kept();
	}
	window = GrNewWindowEx(UI_FIXED, "Settings", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	for (;;) {
		/* only the System tab has something to say by itself, once a second */
		GrGetNextEventTimeout(&event, tab == TAB_SYSTEM ? 1000 : 0);
		if (event.type == GR_EVENT_TYPE_TIMEOUT && tab == TAB_SYSTEM)
			draw_system_changes();
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_MOUSE_MOTION:
			/* the pointer's moves while a button is held (a slider, a list's bar), and not otherwise */
			if (event.type == GR_EVENT_TYPE_BUTTON_DOWN ? !ui_wheel(&event) : !event.mouse.buttons)
				ui_moves(window, EVENTS, event.type == GR_EVENT_TYPE_BUTTON_DOWN);
			press(&event);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(&event);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
