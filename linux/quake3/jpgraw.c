/*
 * A JPEG file as raw pixels, for bake.py (built for the build's own computer from ioquake3's
 * copy of libjpeg): "width height\n" and then red, green, blue a pixel, rows from the top.
 */
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>

int main (int argc, char **argv)
{
	struct jpeg_decompress_struct	cinfo;
	struct jpeg_error_mgr		jerr;
	FILE				*in, *out;
	unsigned char			*row;

	if (argc != 3 || (in = fopen (argv[1], "rb")) == NULL || (out = fopen (argv[2], "wb")) == NULL)
		return 1;
	cinfo.err = jpeg_std_error (&jerr);
	jpeg_create_decompress (&cinfo);
	jpeg_stdio_src (&cinfo, in);
	jpeg_read_header (&cinfo, TRUE);
	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress (&cinfo);
	fprintf (out, "%u %u\n", cinfo.output_width, cinfo.output_height);
	row = malloc (cinfo.output_width * 3);
	while (cinfo.output_scanline < cinfo.output_height) {
		jpeg_read_scanlines (&cinfo, &row, 1);
		fwrite (row, 3, cinfo.output_width, out);
	}
	jpeg_finish_decompress (&cinfo);
	fclose (out);
	return 0;
}
