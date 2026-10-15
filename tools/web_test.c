/*
 * The browser's page engine on the host (docs/web.md): lays a page out with the image's own
 * fonts and what tools of linux/apps/web/preload.py cached, says what came of it, and draws
 * it into a PPM file. No window system and no machine: a change to css.h, web.h or
 * web_layout.h is tried here first.
 *
 *   gcc -O1 -g -fsanitize=address,undefined -Ilinux/apps tools/web_test.c -o web_test
 *   web_test DIR ADDRESS|FILE [--width 572] [--ppm OUT.ppm] [--items] [--sheets no] [--why X Y]
 *
 * DIR has the fonts (font-*.sef) and the cache (web-cache.idx, webc-*). The cache's JPEGs and
 * PNGs are drawn grey unless libmedia is in: -DWEB_TEST_MEDIA -Ilinux/libmedia -Iprograms/mc,
 * linux/libmedia/m_{core,simple,png,png_enc,jpeg,jpeg_enc,gif}.c, linux/libmedia/test/mcw_host.c, -lpthread -lm.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char cache_dir[400];
#define WEB_CACHE_DIR cache_dir
#include "web_layout.h"
#ifdef WEB_TEST_MEDIA
#include "media.h"
#endif

static char *
slurp(const char *path, int *size)
{
	FILE *f = fopen(path, "rb");
	char *data;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	data = malloc(n + 1);
	*size = (int)fread(data, 1, n, f);
	data[*size] = 0;
	fclose(f);
	return data;
}

static const void *
web_file(const char *name, int *size)
{
	char path[600];

	snprintf(path, sizeof path, "%s/%s", cache_dir, name);
	return slurp(path, size);
}

static char pictures[512][300];
static int picture_count;

static int
web_picture(int node, const char *address, int *w, int *h)
{
	char file[400];

	(void)node;
	if (!web_cache_find(address, 'I', file, sizeof file, w, h) || picture_count == 512)
		return -1;
	strcpy(pictures[picture_count], file);
	return picture_count++;
}

/* ---- drawing, as the GPU would ---- */

static unsigned char *pixels;
static int pw, ph;

static void
fill(int x, int y, int w, int h, int colour)
{
	int i, j;

	for (j = y < 0 ? 0 : y; j < y + h && j < ph; j++)
		for (i = x < 0 ? 0 : x; i < x + w && i < pw; i++) {
			unsigned char *p = pixels + 3 * (j * pw + i);

			p[0] = colour >> 16, p[1] = colour >> 8, p[2] = colour;
		}
}

static void
draw_text(const struct web_item *it)
{
	const struct web_face *f = &web_faces[it->face];
	const struct sef_head *h = (const struct sef_head *)f->file;
	int x = it->x, i, gx, gy;

	for (i = 0; i < it->b; i++) {
		int g = sef_glyph(f->file, web_u[it->a + i] < 32 ? 32 : web_u[it->a + i]);
		const uint32_t *entry;
		const unsigned char *bits;
		int gw, gh, dx, dy;

		if (g < 0)
			continue;
		entry = (const uint32_t *)(f->file + h->table_at + 16 * g);
		bits = f->file + entry[0];
		gw = entry[1], gh = entry[2];
		dx = (signed char)(entry[3] & 255), dy = (signed char)(entry[3] >> 8 & 255);
		for (gy = 0; gy < gh << f->twice; gy++)
			for (gx = 0; gx < gw << f->twice; gx++) {
				int sx = gx >> f->twice, sy = gy >> f->twice, level;
				int px = x + (dx << f->twice) + gx, py = it->y + (dy << f->twice) + gy, k;
				unsigned char *p;

				if (h->bpp == 1)
					level = bits[sy * ((gw + 7) / 8) + sx / 8] << (sx & 7) & 0x80 ? 15 : 0;
				else
					level = sx & 1 ? bits[sy * ((gw + 1) / 2) + sx / 2] & 15 : bits[sy * ((gw + 1) / 2) + sx / 2] >> 4;
				if (it->flags & IF_ITALIC)
					px += (f->ascent - (dy << f->twice) - gy) / 5;
				if (!level || px < 0 || py < 0 || px >= pw || py >= ph)
					continue;
				p = pixels + 3 * (py * pw + px);
				for (k = 0; k < 3; k++)
					p[k] = (p[k] * (15 - level) + (it->colour >> (16 - 8 * k) & 255) * level) / 15;
			}
		x += sef_advance(f->file, g) << f->twice;
	}
	if (it->flags & IF_UNDER)
		fill(it->x, it->y + f->ascent + 1, it->w, 1, it->colour);
	if (it->flags & IF_THROUGH)
		fill(it->x, it->y + f->ascent * 2 / 3, it->w, 1, it->colour);
}

static void
draw_image(const struct web_item *it)
{
	int size, w, h, start = 0, stride, i, j;
	char *data = slurp(pictures[it->a], &size);
	const unsigned char *src;
#ifdef WEB_TEST_MEDIA
	struct media_image out = { 0 };

	/* the cache's JPEGs and PNGs, decoded as the browser decodes them */
	out.channels = 3;
	if (data && size > 2 && strncmp(data, "P6", 2) && !media_image_decode(data, (uint32_t)size, &out, 0)) {
		free(data);
		data = malloc((size_t)out.stride * out.height + 32);
		w = out.width, h = out.height;
		start = sprintf(data, "P6 %d %d 255", w, h);
		memcpy(data + start + 1, out.pixels, (size_t)out.stride * h);
		stride = out.stride;
		media_image_free(&out);
	} else
#endif
	if (!data || sscanf(data, "P6 %d %d 255%n", &w, &h, &start) != 2) {
		fill(it->x, it->y, it->w, it->h, 0xe0e0e0);
		free(data);
		return;
	} else
		stride = 3 * w;
	start++;
	src = (const unsigned char *)data + start;
	for (j = 0; j < it->h; j++)
		for (i = 0; i < it->w; i++) {
			const unsigned char *from = src + (j * h / it->h) * stride + 3 * (i * w / it->w);
			int x = it->x + i, y = it->y + j;

			if (x >= 0 && y >= 0 && x < pw && y < ph)
				memcpy(pixels + 3 * (y * pw + x), from, 3);
		}
	free(data);
}

static void
draw(const char *out)
{
	FILE *f;
	int i;

	pixels = malloc(3 * pw * ph);
	fill(0, 0, pw, ph, web_page_bg);
	for (i = 0; i < web_item_count; i++) {
		const struct web_item *it = &web_items[i];

		if (it->kind == I_BOX) {
			if (it->colour >= 0)
				fill(it->x, it->y, it->w, it->h, it->colour);
			if (it->flags) {
				fill(it->x, it->y, it->w, 1, it->b), fill(it->x, it->y + it->h - 1, it->w, 1, it->b);
				fill(it->x, it->y, 1, it->h, it->b), fill(it->x + it->w - 1, it->y, 1, it->h, it->b);
			}
		} else if (it->kind == I_TEXT)
			draw_text(it);
		else if (it->kind == I_IMAGE)
			draw_image(it);
		else {
			fill(it->x, it->y, it->w, it->h, 0x808080);
			fill(it->x + 1, it->y + 1, it->w - 2, it->h - 2, 0xffffff);
		}
	}
	f = fopen(out, "wb");
	fprintf(f, "P6\n%d %d\n255\n", pw, ph);
	fwrite(pixels, 3, pw * ph, f);
	fclose(f);
}

int
main(int argc, char **argv)
{
	const char *out = NULL, *where;
	char file[400];
	char *html;
	int width = 572, n, i, list = 0, sheets = 1, texts = 0, boxes = 0, images = 0, fields = 0, wide = 0, over = 0, chars = 0;
	int links = 0, last_link = -1, repeat = 1, why_x = 0, why_y = -1;
	clock_t began = clock();

	if (argc < 3) {
		fprintf(stderr, "web_test DIR ADDRESS|FILE [--width N] [--ppm OUT] [--items] [--sheets no]\n");
		return 2;
	}
	snprintf(cache_dir, sizeof cache_dir, "%s", argv[1]);
	where = argv[2];
	for (i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--width") && i + 1 < argc)
			width = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "--repeat") && i + 1 < argc)
			repeat = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--why") && i + 2 < argc)
			why_x = atoi(argv[i + 1]), why_y = atoi(argv[i + 2]), i += 2;
		else if (!strcmp(argv[i], "--items"))
			list = 1;
		else if (!strcmp(argv[i], "--sheets") && i + 1 < argc)
			sheets = strcmp(argv[++i], "no") != 0;
	}
	snprintf(web_address, sizeof web_address, "%s", where);
	if (strstr(where, "://")) {
		if (!web_cache_find(where, 'P', file, sizeof file, NULL, NULL)) {
			fprintf(stderr, "not in the cache: %s\n", where);
			return 1;
		}
	} else
		snprintf(file, sizeof file, "%s", where);
	if ((html = slurp(file, &n)) == NULL) {
		fprintf(stderr, "cannot read %s\n", file);
		return 1;
	}
	web_find_sheets(html, n, width);
	for (i = 0; i < web_sheet_count; i++) {
		struct web_sheet *sh = &web_sheets[i];
		char path[400];

		if (sh->state != SHEET_WANTED)
			continue;
		sh->state = SHEET_FAILED;
		if (sheets && (sh->address[0] == '/' ? (snprintf(path, sizeof path, "%s", sh->address), 1) :
			       web_cache_find(sh->address, 'S', path, sizeof path, NULL, NULL)) &&
		    (sh->text = slurp(path, &sh->length)) != NULL)
			sh->state = SHEET_HERE, sh->own = 1;
		printf("sheet %s: %s\n", sh->address, sh->state == SHEET_HERE ? "here" : "not here");
	}
	if (!sheets)
		web_sheet_count = 0;
	for (i = 0; i < repeat; i++) {	/* (--repeat: for a profile) */
		web_read_sheets(width, 400);
		web_parse(html, n);
		web_style_all();
		web_layout(width);
	}
	for (i = 0; i < web_item_count; i++) {
		const struct web_item *it = &web_items[i];
		int j;

		texts += it->kind == I_TEXT, boxes += it->kind == I_BOX, images += it->kind == I_IMAGE, fields += it->kind == I_FIELD;
		if (it->kind == I_TEXT) {
			chars += it->b;
			wide += it->x + it->w > width || it->x < 0;
			if (web_link_of(it->node) >= 0 && web_link_of(it->node) != last_link)
				links++, last_link = web_link_of(it->node);
			/* text over text is a mistake of the layout's: the items just before it are looked at */
			for (j = i - 1; j >= 0 && j > i - 40; j--) {
				const struct web_item *o = &web_items[j];

				if (o->kind == I_TEXT && it->x < o->x + o->w && o->x < it->x + it->w && it->y + 2 < o->y + o->h - 2 && o->y + 2 < it->y + it->h - 2 &&
				    it->w > 0 && o->w > 0) {
					over++;
					break;
				}
			}
		}
		if (why_y >= 0 && it->kind != I_BOX && why_x >= it->x && why_x < it->x + it->w && why_y >= it->y && why_y < it->y + it->h) {
			/* what the thing at that place is inside, from the inside out */
			int up;

			for (up = web_nodes[it->node].c.tag == T_TEXT ? web_nodes[it->node].c.parent : it->node; up >= 0; up = web_nodes[up].c.parent) {
				const struct css_style *st = web_style_of(up);
				const char *cls = "";
				int cn = 0;

				web_attr(up, "class", &cls, &cn);
				printf("  <%s class=\"%.*s\"> display %d float %d white %d width %d maxw %d minw %d m %d,%d p %d,%d y %d flex-column %d wrap %d grow %d basis %d grid %d\n",
				       css_names + css_name_at[web_nodes[up].c.tag], cn > 60 ? 60 : cn, cls, st->display, st->floated, st->white, st->width, st->maxw, st->minw,
				       st->m[3], st->m[1], st->p[3], st->p[1], web_nodes[up].y, st->column, st->wrap, st->grow, st->basis, st->grid);
			}
			why_y = -1;
		}
		if (list) {
			printf("%c %4d %5d %4d %3d", "TBIF"[it->kind], it->x, it->y, it->w, it->h);
			if (it->kind == I_TEXT) {
				printf(" %06x f%d ", it->colour, it->face);
				for (j = 0; j < it->b && j < 60; j++)
					putchar(web_u[it->a + j] < 127 ? web_u[it->a + j] : '~');
			} else if (it->kind == I_BOX)
				printf(" %06x", it->colour & 0xffffff);
			printf("  <%s>\n", web_nodes[it->node].c.tag == T_TEXT ? "text" : css_names + css_name_at[web_nodes[it->node].c.tag]);
		}
	}
	printf("%s: \"%s\" %d bytes, %d nodes, %d rules, %d sheets; %d x %d; %d texts (%d characters, %d links), %d boxes, %d pictures, %d fields; "
	       "%d texts past the edge, %d over another; %.0f ms\n", where, web_title, n, web_node_count, css_rule_count, web_sheet_count, width,
	       web_page_h, texts, chars, links, boxes, images, fields, wide, over, (clock() - began) * 1000.0 / CLOCKS_PER_SEC);
	if (out) {
		pw = width;
		ph = web_page_h < 12000 ? web_page_h + 8 : 12000;
		draw(out);
	}
	return 0;
}
