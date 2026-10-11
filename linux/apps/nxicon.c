/*
 * nxicon: gives a program an icon of its own, at the end of its file, where the desktop, the
 * Start menu, the bar and a window's caption look for it (ui_icons.h).
 *
 *	nxicon PROGRAM PICTURE.ppm	the picture (a PPM file, any size; magenta is clear) becomes its icon
 *	nxicon PROGRAM			says whether it has one
 *	nxicon PROGRAM --none		takes it away
 *
 * The picture is brought to 32 x 32 and to 16 x 16 by taking the nearest pixel, so one drawn
 * at 32 x 32 is kept as it is. A PNG or JPEG picture: nxview --decode IT /tmp/it.ppm first.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MARK "NXICON1"
#define ICON_BYTES (32 * 32 * 3 + 16 * 16 * 3 + 8)

static int
has_icon(int fd, long *size)
{
	char mark[8];

	*size = lseek(fd, 0, SEEK_END);
	return *size >= ICON_BYTES && pread(fd, mark, 8, *size - 8) == 8 && !memcmp(mark, MARK, 8);
}

int
main(int argc, char **argv)
{
	unsigned char *picture, icon[ICON_BYTES], *to = icon;
	int fd, w = 0, h = 0, most = 0, start = 0, size, x, y, n;
	char head[64];
	long length;
	FILE *file;

	if (argc < 2) {
		fprintf(stderr, "nxicon PROGRAM [PICTURE.ppm | --none]: a program's own icon, at the end of its file\n");
		return 1;
	}
	if ((fd = open(argv[1], argc > 2 ? O_RDWR : O_RDONLY)) < 0) {
		perror(argv[1]);
		return 1;
	}
	if (argc < 3) {
		printf("%s has %s\n", argv[1], has_icon(fd, &length) ? "an icon of its own" : "no icon of its own");
		return 0;
	}
	if (has_icon(fd, &length) && ftruncate(fd, length -= ICON_BYTES) != 0) {
		perror(argv[1]);
		return 1;
	}
	if (!strcmp(argv[2], "--none"))
		return 0;
	if ((file = fopen(argv[2], "rb")) == NULL) {
		perror(argv[2]);
		return 1;
	}
	n = (int)fread(head, 1, sizeof head - 1, file);
	head[n > 0 ? n : 0] = 0;
	if (sscanf(head, "P6 %d %d %d%n", &w, &h, &most, &start) != 3 || most != 255 || w < 1 || h < 1 || w > 4096 || h > 4096) {
		fprintf(stderr, "nxicon: %s is not a PPM picture (P6, 255)\n", argv[2]);
		return 1;
	}
	picture = malloc((size_t)w * h * 3);
	fseek(file, start + 1, SEEK_SET);
	if (!picture || fread(picture, 3, (size_t)w * h, file) != (size_t)w * h) {
		fprintf(stderr, "nxicon: %s is cut short\n", argv[2]);
		return 1;
	}
	for (size = 32; size >= 16; size /= 2)
		for (y = 0; y < size; y++)
			for (x = 0; x < size; x++, to += 3)
				memcpy(to, picture + ((y * 2 + 1) * h / (size * 2) * w + (x * 2 + 1) * w / (size * 2)) * 3, 3);
	memcpy(to, MARK, 8);
	if (pwrite(fd, icon, sizeof icon, length) != (int)sizeof icon) {
		perror(argv[1]);
		return 1;
	}
	return 0;
}
