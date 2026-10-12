/* ui_state.h: what a graphics context was last told. A colour or a font it has already is not
 * sent again: every text of a redraw set both, a request each. Needs nano-X.h. The macros at
 * the end put it under every GrSetGCForeground and GrSetGCFont that follows (ui.h, ui_menu.h). */
#ifndef SHADEREMU_APPS_UI_STATE_H
#define SHADEREMU_APPS_UI_STATE_H

#define UI_STATE_MOST 4
static struct { GR_GC_ID gc; GR_COLOR colour; GR_FONT_ID font; int known; } ui_state[UI_STATE_MOST];

/* The record kept for a context (bit 0 of `known`: its colour, bit 1: its font). */
static int
ui_state_of(GR_GC_ID gc)
{
	int i, spare = (int)(gc % UI_STATE_MOST);

	for (i = 0; i < UI_STATE_MOST; i++) {
		if (ui_state[i].gc == gc)
			return i;
		if (!ui_state[i].gc)
			spare = i;
	}
	i = spare;
	ui_state[i].gc = gc;
	ui_state[i].known = 0;
	return i;
}

static void
ui_state_foreground(GR_GC_ID gc, GR_COLOR colour)
{
	int i = ui_state_of(gc);

	if ((ui_state[i].known & 1) && ui_state[i].colour == colour)
		return;
	ui_state[i].colour = colour;
	ui_state[i].known |= 1;
	GrSetGCForeground(gc, colour);
}

static void
ui_state_font(GR_GC_ID gc, GR_FONT_ID font)
{
	int i = ui_state_of(gc);

	if ((ui_state[i].known & 2) && ui_state[i].font == font)
		return;
	ui_state[i].font = font;
	ui_state[i].known |= 2;
	GrSetGCFont(gc, font);
}

/* A context that is gone: its number may be another's later. */
static void
ui_state_destroy(GR_GC_ID gc)
{
	ui_state[ui_state_of(gc)].known = 0;
	GrDestroyGC(gc);
}

#define GrSetGCForeground(gc, colour) ui_state_foreground(gc, colour)
#define GrSetGCFont(gc, font) ui_state_font(gc, font)
#define GrDestroyGC(gc) ui_state_destroy(gc)
#endif
