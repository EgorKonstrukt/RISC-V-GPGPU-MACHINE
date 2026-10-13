/* ui_glyph.h: the signs on a key (play, pause, an arrow, a loudspeaker), drawn of lines and
 * rectangles, which the GPU draws: no picture to keep or to load. ui_tool() is a key with one. */
#ifndef SHADEREMU_APPS_UI_GLYPH_H
#define SHADEREMU_APPS_UI_GLYPH_H

#include "ui.h"

enum { UI_G_PLAY, UI_G_PAUSE, UI_G_STOP, UI_G_PREV, UI_G_NEXT, UI_G_SOUND, UI_G_MUTE, UI_G_LEFT, UI_G_RIGHT, UI_G_UP,
       UI_G_PLUS, UI_G_MINUS, UI_G_TURN_LEFT, UI_G_TURN_RIGHT, UI_G_HOME, UI_G_AGAIN, UI_G_FOLDER, UI_G_LIST, UI_G_TILES, UI_G_TERMINAL,
       UI_G_MARK };

/* A triangle whose base is the column at x, `size` high either way, its point `size` columns
 * to the right (dir 1) or to the left (-1). */
static void
ui_glyph_point(GR_DRAW_ID w, int x, int cy, int size, int dir)
{
	int k;

	for (k = 0; k <= size; k++)
		GrLine(w, ui_gc, x + dir * k, cy - (size - k), x + dir * k, cy + (size - k));
}

/* The same, pointing up from the row at y. */
static void
ui_glyph_point_up(GR_DRAW_ID w, int cx, int y, int size)
{
	int k;

	for (k = 0; k <= size; k++)
		GrLine(w, ui_gc, cx - (size - k), y - k, cx + (size - k), y - k);
}

/* Draws a sign with its middle at cx, cy: some 11 pixels across. */
static void
ui_glyph(GR_DRAW_ID w, int cx, int cy, int which, GR_COLOR colour)
{
	int k;

	GrSetGCForeground(ui_gc, colour);
	switch (which) {
	case UI_G_PLAY:
		ui_glyph_point(w, cx - 3, cy, 5, 1);
		break;
	case UI_G_PAUSE:
		GrFillRect(w, ui_gc, cx - 4, cy - 5, 3, 11);
		GrFillRect(w, ui_gc, cx + 2, cy - 5, 3, 11);
		break;
	case UI_G_STOP:
		GrFillRect(w, ui_gc, cx - 4, cy - 4, 9, 9);
		break;
	case UI_G_PREV:
		GrFillRect(w, ui_gc, cx - 5, cy - 5, 2, 11);
		ui_glyph_point(w, cx + 4, cy, 5, -1);
		break;
	case UI_G_NEXT:
		GrFillRect(w, ui_gc, cx + 4, cy - 5, 2, 11);
		ui_glyph_point(w, cx - 4, cy, 5, 1);
		break;
	case UI_G_SOUND:
	case UI_G_MUTE:
		/* a loudspeaker: its box, its cone, and what comes out of it (or a cross: nothing does) */
		GrFillRect(w, ui_gc, cx - 6, cy - 2, 3, 5);
		for (k = 0; k < 4; k++)
			GrLine(w, ui_gc, cx - 3 + k, cy - 2 - k, cx - 3 + k, cy + 2 + k);
		if (which == UI_G_SOUND) {
			GrLine(w, ui_gc, cx + 3, cy - 2, cx + 3, cy + 2);
			GrLine(w, ui_gc, cx + 5, cy - 4, cx + 5, cy + 4);
		} else {
			GrLine(w, ui_gc, cx + 2, cy - 3, cx + 6, cy + 3);
			GrLine(w, ui_gc, cx + 2, cy + 3, cx + 6, cy - 3);
		}
		break;
	case UI_G_LEFT:
		ui_glyph_point(w, cx - 1, cy, 5, -1);
		GrFillRect(w, ui_gc, cx, cy - 1, 6, 3);
		break;
	case UI_G_RIGHT:
		ui_glyph_point(w, cx + 1, cy, 5, 1);
		GrFillRect(w, ui_gc, cx - 5, cy - 1, 6, 3);
		break;
	case UI_G_UP:
		ui_glyph_point_up(w, cx, cy, 5);
		GrFillRect(w, ui_gc, cx - 1, cy + 1, 3, 5);
		break;
	case UI_G_PLUS:
		GrFillRect(w, ui_gc, cx - 1, cy - 4, 3, 9);
		/* fall through: its bar across */
	case UI_G_MINUS:
		GrFillRect(w, ui_gc, cx - 4, cy - 1, 9, 3);
		break;
	case UI_G_TURN_LEFT:
	case UI_G_TURN_RIGHT:
		/* three sides of a square and an arrow's point at its open end */
		k = which == UI_G_TURN_LEFT ? -1 : 1;
		GrFillRect(w, ui_gc, cx - 4, cy - 3, 9, 2);
		GrFillRect(w, ui_gc, cx - 4 * k - (k < 0), cy - 3, 2, 8);
		GrFillRect(w, ui_gc, cx - 4, cy + 4, 9, 2);
		GrFillRect(w, ui_gc, cx + 4 * k - (k > 0), cy + 1, 2, 4);
		ui_glyph_point_up(w, cx + 4 * k, cy - 1, 3);
		break;
	case UI_G_HOME:
		ui_glyph_point_up(w, cx, cy - 1, 5);
		GrFillRect(w, ui_gc, cx - 4, cy, 9, 5);
		GrSetGCForeground(ui_gc, UI_FACE);
		GrFillRect(w, ui_gc, cx - 1, cy + 2, 3, 3);
		break;
	case UI_G_AGAIN:
		/* a ring with a gap, and a point at the gap */
		GrRect(w, ui_gc, cx - 4, cy - 4, 9, 9);
		GrRect(w, ui_gc, cx - 3, cy - 3, 7, 7);
		GrSetGCForeground(ui_gc, UI_FACE);
		GrFillRect(w, ui_gc, cx + 1, cy - 5, 5, 4);
		GrSetGCForeground(ui_gc, colour);
		ui_glyph_point(w, cx + 1, cy - 3, 3, 1);
		break;
	case UI_G_FOLDER:
		GrFillRect(w, ui_gc, cx - 5, cy - 4, 5, 2);
		GrFillRect(w, ui_gc, cx - 5, cy - 2, 11, 7);
		break;
	case UI_G_LIST:
		for (k = -4; k <= 4; k += 4) {
			GrFillRect(w, ui_gc, cx - 5, cy + k - 1, 2, 2);
			GrFillRect(w, ui_gc, cx - 2, cy + k - 1, 8, 2);
		}
		break;
	case UI_G_TILES:
		for (k = 0; k < 4; k++)
			GrFillRect(w, ui_gc, cx - 5 + (k & 1) * 6, cy - 5 + (k >> 1) * 6, 5, 5);
		break;
	case UI_G_TERMINAL:	/* a screen with a prompt on it: > _ */
		GrRect(w, ui_gc, cx - 6, cy - 5, 13, 11);
		GrLine(w, ui_gc, cx - 4, cy - 2, cx - 2, cy);
		GrLine(w, ui_gc, cx - 2, cy, cx - 4, cy + 2);
		GrLine(w, ui_gc, cx, cy + 2, cx + 3, cy + 2);
		break;
	case UI_G_MARK:		/* a ribbon between a book's pages, its end cut to two points */
		GrFillRect(w, ui_gc, cx - 3, cy - 5, 7, 8);
		for (k = 0; k < 3; k++) {
			GrLine(w, ui_gc, cx - 3, cy + 3 + k, cx - 1 - k, cy + 3 + k);
			GrLine(w, ui_gc, cx + 1 + k, cy + 3 + k, cx + 3, cy + 3 + k);
		}
		break;
	}
}

/* A key with a sign on it; `how`: 1 pressed in (a choice that is on), 2 greyed. */
static void
ui_tool(GR_DRAW_ID w, int x, int y, int width, int height, int which, int how)
{
	ui_fill(w, x, y, width, height, how == 1 ? MWRGB(214, 214, 214) : UI_FACE);
	ui_bevel(w, x, y, width, height, how == 1);
	ui_glyph(w, x + width / 2 + (how == 1), y + height / 2 + (how == 1), which, how == 2 ? UI_SHADOW : BLACK);
}

#endif
