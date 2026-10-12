/* ui_width.h: a text's width worked out by the program, from the font's widths asked for
 * once: asking the window system for each text is a wait for the answer, and for whatever
 * was being drawn when the question came. Needs nano-X.h. */
#ifndef SHADEREMU_APPS_UI_WIDTH_H
#define SHADEREMU_APPS_UI_WIDTH_H

#include <string.h>

struct ui_widths { int known; unsigned char of[256]; };

/* The width of `count` characters (all of them when negative) in a font of the window
 * system's own; font 0 is the one a new GC has. Text of one byte a character. */
static int
ui_width(struct ui_widths *widths, GR_FONT_ID font, const char *text, int count)
{
	int width = 0;

	if (!widths->known) {
		GR_FONT_INFO info;
		int c;

		GrGetFontInfo(font, &info);
		/* (a character the font lacks is drawn, and measured, as its first) */
		for (c = 0; c < 256; c++)
			widths->of[c] = info.widths[c >= info.firstchar && c <= info.lastchar ? c : info.firstchar];
		widths->known = 1;
	}
	if (count < 0)
		count = (int)strlen(text);
	while (count-- > 0)
		width += widths->of[(unsigned char)*text++];
	return width;
}

#endif
