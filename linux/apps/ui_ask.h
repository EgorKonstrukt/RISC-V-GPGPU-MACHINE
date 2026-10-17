/* A question in a window of its own, with a button for each answer ("Save the changes?").
 * ui_ask() comes back with the answer's number, or -1 when the window was closed or Escape
 * pressed. The program `nxask` is the same for a script. */
#ifndef SHADEREMU_APPS_UI_ASK_H
#define SHADEREMU_APPS_UI_ASK_H

#include "ui.h"
#include "ui_icons.h"

#define UI_ASK_LINES 5
#define UI_ASK_ANSWERS 4
#define UI_ASK_BUTTON_H 24

static struct {
	const char *line[UI_ASK_LINES];
	int length[UI_ASK_LINES], lines;
	const char *const *answers;
	int count, focus, width, height, x[UI_ASK_ANSWERS], w[UI_ASK_ANSWERS];
	int icon, indent;	/* the icon's cell (-1: none), and how far the text stands right of it */
} ui_asked;
static const char *ui_ask_icon;	/* set: the icon of that name left of the text ("error", "warning", "information") */
static int ui_ask_aside;	/* set: low on the screen, and it never has the keyboard (its buttons are clicked) */
static int (*ui_ask_over)(void);	/* when set, looked at twice a second: true ends the question, which answers -2 */

static void
ui_ask_draw(GR_WINDOW_ID w)
{
	int i, y = ui_asked.height - UI_ASK_BUTTON_H - 10;

	ui_fill(w, 0, 0, ui_asked.width, ui_asked.height, UI_FACE);
	if (ui_seven)	/* (Windows 7 puts the answers on a strip of their own) */
		ui_fill(w, 0, y - 10, ui_asked.width, ui_asked.height - y + 10, MWRGB(240, 240, 240)),
		ui_fill(w, 0, 0, ui_asked.width, y - 10, WHITE);
	ui_icon(w, ui_gc, 14, 12, ui_asked.icon, 32);
	for (i = 0; i < ui_asked.lines; i++)
		ui_text(w, 14 + ui_asked.indent, 14 + i * 16, ui_asked.line[i], ui_asked.length[i], BLACK, 0);
	for (i = 0; i < ui_asked.count; i++) {
		int tw = ui_width(&ui_var_widths, ui_var, ui_asked.answers[i], -1);

		if (ui_seven)
			ui_button_seven(w, ui_asked.x[i], y, ui_asked.w[i], UI_ASK_BUTTON_H, 0);
		else
			ui_bevel(w, ui_asked.x[i], y, ui_asked.w[i], UI_ASK_BUTTON_H, 0);
		ui_text(w, ui_asked.x[i] + (ui_asked.w[i] - tw) / 2, y + 6, ui_asked.answers[i], -1, BLACK, 0);
		if (i == ui_asked.focus)
			ui_focus(w, ui_asked.x[i] + 5, y + 5, ui_asked.w[i] - 10, UI_ASK_BUTTON_H - 10);
	}
}

/*
 * Asks `text` (lines parted by '\n') and waits for one of `count` answers; the first has the
 * keyboard. Tab and the arrows go round them, Enter or Space presses, a letter presses the
 * answer that starts with it. `others` gets the program's other windows' events meanwhile.
 */
static int
ui_ask(const char *title, const char *text, const char *const *answers, int count, void (*others)(GR_EVENT *))
{
	const GR_SCREEN_INFO *screen = ui_screen();
	GR_WINDOW_ID w;
	GR_EVENT event;
	int i, wide = 0, row = 0, answer = -3, x, ch;

	count = count > UI_ASK_ANSWERS ? UI_ASK_ANSWERS : count;
	for (ui_asked.lines = 0; ui_asked.lines < UI_ASK_LINES && *text; ui_asked.lines++) {
		int n = (int)strcspn(text, "\n"), tw = ui_width(&ui_var_widths, ui_var, text, n);

		ui_asked.line[ui_asked.lines] = text, ui_asked.length[ui_asked.lines] = n;
		wide = tw > wide ? tw : wide;
		text += n + (text[n] == '\n');
	}
	for (i = 0; i < count; i++) {
		ui_asked.w[i] = ui_width(&ui_var_widths, ui_var, answers[i], -1) + 24;
		ui_asked.w[i] = ui_asked.w[i] < 76 ? 76 : ui_asked.w[i];
		row += ui_asked.w[i] + 8;
	}
	ui_asked.answers = answers, ui_asked.count = count, ui_asked.focus = 0;
	ui_asked.icon = ui_ask_icon ? ui_icon_of(ui_ask_icon) : -1;
	ui_asked.indent = ui_asked.icon >= 0 ? 46 : 0;
	ui_asked.width = (wide + ui_asked.indent > row ? wide + ui_asked.indent : row) + 28;
	ui_asked.width = ui_asked.width < 260 ? 260 : ui_asked.width > screen->cols - 20 ? screen->cols - 20 : ui_asked.width;
	ui_asked.height = 14 + (ui_asked.icon >= 0 && ui_asked.lines < 2 ? 2 : ui_asked.lines) * 16 + 14 + UI_ASK_BUTTON_H + 10;
	for (i = 0, x = ui_asked.width - 6 - row; i < count; x += ui_asked.w[i++] + 8)
		ui_asked.x[i] = x;
	if (getenv("UI_ASK_PRINT")) {	/* for a test without a screen: what is asked */
		printf("ask: %s:", title);
		for (i = 0; i < ui_asked.lines; i++)
			printf(" %.*s", ui_asked.length[i], ui_asked.line[i]);
		for (i = 0; i < count; i++)
			printf(" [%s at %d]", answers[i], ui_asked.x[i] + ui_asked.w[i] / 2);
		printf(" in %d x %d\n", ui_asked.width, ui_asked.height);
		fflush(stdout);
	}
	w = GrNewWindowEx(UI_FIXED | GR_WM_PROPS_NOAUTOMOVE | (ui_ask_aside ? GR_WM_PROPS_NOFOCUS : 0), (char *)title, GR_ROOT_WINDOW_ID,
		(screen->cols - ui_asked.width) / 2, ui_ask_aside ? screen->rows - ui_asked.height - 80 : (screen->rows - ui_asked.height) / 3,
		ui_asked.width, ui_asked.height, UI_FACE);
	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(w);
	GrRaiseWindow(w);
	if (!ui_ask_aside)
		GrSetFocus(w);
	while (answer == -3) {
		if (!ui_ask_over)
			GrGetNextEvent(&event);
		else if (GrGetNextEventTimeout(&event, 500), event.type == GR_EVENT_TYPE_TIMEOUT) {
			answer = ui_ask_over() ? -2 : -3;
			continue;
		}
		if (event.general.wid != w) {
			if (others && event.type != GR_EVENT_TYPE_NONE)
				others(&event);
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			ui_ask_draw(w);
			if (getenv("UI_ASK_PRINT")) {	/* where it is, for a test to click in */
				ui_window_origin(w, &x, &i);
				printf("ask: at %d,%d\n", x, i);
				fflush(stdout);
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			answer = -1;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			for (i = 0; i < count && !ui_wheel(&event); i++)
				if (ui_inside(event.button.x, event.button.y, ui_asked.x[i], ui_asked.height - UI_ASK_BUTTON_H - 10,
					      ui_asked.w[i], UI_ASK_BUTTON_H))
					answer = i;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			ch = event.keystroke.ch;
			if (ch == MWKEY_ESCAPE)
				answer = -1;
			else if (ui_press_key(ch))
				answer = ui_asked.focus;
			else if (ui_tab(&event, &ui_asked.focus, count) || ch == MWKEY_LEFT || ch == MWKEY_RIGHT) {
				if (ch != MWKEY_TAB)
					ui_asked.focus = (ui_asked.focus + (ch == MWKEY_LEFT ? count - 1 : 1)) % count;
				ui_ask_draw(w);
			} else
				for (i = 0; i < count && answer == -3; i++)
					if (ui_starts(answers[i], ch))
						answer = i;
			break;
		}
	}
	GrDestroyWindow(w);
	return answer;
}

#endif
