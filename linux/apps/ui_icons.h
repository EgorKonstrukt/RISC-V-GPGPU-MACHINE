/* ui_icons.h: the desktop's icons. A program carries its own at the end of its file (32 x 32
 * and 16 x 16 pixels, then the mark "NXICON1": nxicon puts one there, and the image's builder
 * does for the image's programs), so a program anyone adds has an icon without a list to be
 * in. Kinds of file and places, and programs without one, are cells of two sheets in the ROM
 * (tools/make_icons.py). Either way an icon is one request and one rectangle of the GPU's,
 * drawn from where the file lies. Needs nano-X.h only. */
#ifndef UI_ICONS_H
#define UI_ICONS_H
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define UI_ICONS_INDEX "/usr/share/icons.idx"

/* An icon: a file's own when `file` is not empty, or the cell when that file carries none
 * (the window system looks: ui_icon_draw); with no file, the cell of the sheets (-1: none). */
struct ui_icon_ref {
	int cell;
	char file[96];
};

static struct { char name[24]; int cell; } ui_icon_table[96];
static int ui_icon_count = -1;

/* The cell of a name (a program, a kind of file, a place: linux/apps/icons.txt), or -1. */
static int
ui_icon_of(const char *name)
{
	int i;

	if (ui_icon_count < 0) {
		/* the list is a name and a number a line: read in one piece, and no scanf for it */
		static char text[4096];
		int fd = open(UI_ICONS_INDEX, O_RDONLY), n = fd < 0 ? 0 : (int)read(fd, text, sizeof text - 1), at = 0, k;

		if (fd >= 0)
			close(fd);
		text[n < 0 ? 0 : n] = 0;
		for (ui_icon_count = 0; ui_icon_count < 96; ui_icon_count++) {
			while (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')
				at++;
			for (k = 0; text[at] > ' ' && k < 23; k++)
				ui_icon_table[ui_icon_count].name[k] = text[at++];
			ui_icon_table[ui_icon_count].name[k] = 0;
			while (text[at] == ' ' || text[at] == '\t')
				at++;
			if (!k || text[at] < '0' || text[at] > '9')
				break;
			for (k = 0; text[at] >= '0' && text[at] <= '9'; at++)
				k = k * 10 + text[at] - '0';
			ui_icon_table[ui_icon_count].cell = k;
		}
	}
	for (i = 0; i < ui_icon_count; i++)
		if (!strcmp(ui_icon_table[i].name, name))
			return ui_icon_table[i].cell;
	return -1;
}

/* Whether a file ends in an icon of its own. */
static int
ui_icon_in(const char *path)
{
	char mark[8];
	int fd = open(path, O_RDONLY), has;

	has = fd >= 0 && lseek(fd, -8, SEEK_END) >= 0 && read(fd, mark, 8) == 8 && !memcmp(mark, "NXICON1", 8);
	if (fd >= 0)
		close(fd);
	return has;
}

/* A program's icon by its file: its own, else the sheets' cell of its name, else `other`'s. */
static void
ui_icon_of_program(const char *path, const char *other, struct ui_icon_ref *icon)
{
	const char *slash = strrchr(path, '/');

	icon->file[0] = 0;
	icon->cell = ui_icon_of(slash ? slash + 1 : path);
	if (icon->cell < 0)
		icon->cell = ui_icon_of(other);
	if (strlen(path) < sizeof icon->file)
		strcpy(icon->file, path);
}

/* The icon of the program a command starts: its first word's ("setsid" left out), looked for
 * where programs are; a script that starts NAME.bin has that program's. */
static void
ui_icon_of_command(const char *command, struct ui_icon_ref *icon)
{
	static const char *folders[] = { "", "/usr/bin/", "/bin/", "/usr/sbin/" };	/* (as PATH has them) */
	char word[64], path[96];
	unsigned i;

	icon->file[0] = 0;
	if (!strncmp(command, "setsid ", 7))
		command += 7;
	if (!strncmp(command, "nx restart", 10)) {
		icon->cell = ui_icon_of("restart");
		return;
	}
	snprintf(word, sizeof word, "%.*s", (int)strcspn(command, " "), command);
	for (i = word[0] == '/' ? 0 : 1; i < (word[0] == '/' ? 1u : 4u); i++) {
		snprintf(path, sizeof path, "%s%s", folders[i], word);
		if (access(path, F_OK) != 0)
			continue;
		icon->cell = ui_icon_of(strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
		/* a name the sheets have: the file's own icon or that cell, and the file is not opened */
		if (icon->cell >= 0 && strlen(path) < sizeof icon->file) {
			strcpy(icon->file, path);
			return;
		}
		if (strlen(path) < sizeof icon->file && ui_icon_in(path)) {
			strcpy(icon->file, path);
			icon->cell = -1;
			return;
		}
		if (icon->cell < 0)
			icon->cell = ui_icon_of("");
		if (!icon->file[0] && icon->cell < 0) {
			snprintf(path, sizeof path, "%s%s.bin", folders[i], word);
			ui_icon_of_program(path, "default", icon);
		}
		return;
	}
	icon->cell = ui_icon_of("default");
}

/* Draws a cell of the sheets, 16 or 32 pixels. Nothing for a cell of -1. */
static void
ui_icon(GR_DRAW_ID w, GR_GC_ID gc, int x, int y, int cell, int size)
{
	if (cell >= 0)
		GrDrawImageFromFile(w, gc, x, y, size, size,
			size == 16 ? "/usr/share/icons16.ppm" : "/usr/share/icons32.ppm", GR_IMAGE_CELL(size, cell));
}

/* Draws an icon, 16 or 32 pixels: the file's own, or the cell. */
static void
ui_icon_draw(GR_DRAW_ID w, GR_GC_ID gc, int x, int y, const struct ui_icon_ref *icon, int size)
{
	if (icon->file[0])
		GrDrawImageFromFile(w, gc, x, y, size, size, (char *)icon->file,
			icon->cell >= 0 ? GR_IMAGE_ICON_OR(size, icon->cell) : GR_IMAGE_ICON(size));
	else
		ui_icon(w, gc, x, y, icon->cell, size);
}
#endif
