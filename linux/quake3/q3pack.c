/* bake.py's pictures into the files the game decodes and back, with the guest's libmedia built
   for the build computer (docs/system/media.md); its usage line says how it is called. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"

static unsigned char *slurp (const char *path, unsigned *size)
{
	FILE		*f = fopen (path, "rb");
	unsigned char	*data;
	long		n;

	if (!f || fseek (f, 0, SEEK_END) || (n = ftell (f)) < 0 || fseek (f, 0, SEEK_SET))
		exit (2);
	data = malloc (n + 16);
	if (!data || fread (data, 1, n, f) != (size_t)n)
		exit (2);
	fclose (f);
	*size = (unsigned)n;
	return data;
}

static void spill (const char *path, const void *data, unsigned size)
{
	FILE	*f = fopen (path, "wb");

	if (!f || fwrite (data, 1, size, f) != size || fclose (f))
		exit (2);
}

int main (int argc, char **argv)
{
	struct media_image	image;
	unsigned		size;
	unsigned char		*file;
	const char		*why;

	memset (&image, 0, sizeof image);
	if (argc == 7 && !strcmp (argv[1], "jpeg")) {
		image.width = atoi (argv[3]), image.height = atoi (argv[4]), image.channels = 3;
		image.stride = image.width * 3, image.pixels = slurp (argv[5], &size);
		file = media_jpeg_encode (&image, atoi (argv[2]), &size, 4);
	} else if (argc == 7 && !strcmp (argv[1], "png")) {
		image.width = atoi (argv[2]), image.height = atoi (argv[3]), image.channels = atoi (argv[4]);
		image.stride = image.width * image.channels, image.pixels = slurp (argv[5], &size);
		file = media_png_encode (&image, 9, &size, 4);
	} else if (argc == 5 && !strcmp (argv[1], "decode")) {
		file = slurp (argv[3], &size);
		image.channels = atoi (argv[2]), image.bgr = 0;
		if ((why = media_image_decode (file, size, &image, 4)) != NULL) {
			fprintf (stderr, "q3pack: %s\n", why);
			return 1;
		}
		for (int y = 0; y < image.height; y++)
			memmove (image.pixels + y * image.width * image.channels, image.pixels + y * image.stride, image.width * image.channels);
		spill (argv[4], image.pixels, image.width * image.height * image.channels);
		return 0;
	} else {
		fprintf (stderr, "q3pack jpeg QUALITY W H IN OUT | png W H CHANNELS IN OUT | decode CHANNELS IN OUT\n");
		return 2;
	}
	if (!file)
		return 1;
	spill (argv[6], file, size);
	return 0;
}
