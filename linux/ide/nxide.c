/* nxide: a small place to write C in, build it and run it (docs/ide.md): the text coloured
 * by a lexer (syntax.h), every change kept (text.h), the machine's own compiler with its
 * complaints marked at their lines, and the program run below or under the debugger (debug.h). */
#define _GNU_SOURCE
#include "ui.h"
#include "ui_files.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/time.h>
#include "text.h"
#include "syntax.h"
#include "debug.h"

#define MENU_H 20
#define TOOL_H 28
#define STATUS 18
#define TAB_H 18
#define PANEL_H 150
#define OUT_MOST 600

enum {
	C_NONE, C_NEW, C_OPEN, C_SAVE, C_SAVE_AS, C_EXIT, C_UNDO, C_REDO, C_CUT, C_COPY, C_PASTE, C_DELETE, C_FIND, C_FIND_NEXT, C_GOTO,
	C_ALL, C_LINE, C_WORD, C_INDENT, C_UNINDENT, C_COMMENT, C_PANEL, C_NUMBERS, C_CHECKING, C_BUILD, C_RUN, C_DEBUG, C_STOP,
	C_OVER, C_INTO, C_OUT, C_MARK, C_UNMARK_ALL, C_KEYS, C_ABOUT, C_EXAMPLE	/* C_EXAMPLE + n: the nth example */
};
enum { P_OUTPUT, P_PROBLEMS, P_VARIABLES, P_STACK, PANELS };
enum { J_NONE, J_BUILD, J_RUN, J_CHECK };
enum { ASK_NONE, ASK_FIND, ASK_GOTO, ASK_SAVE };

static GR_WINDOW_ID window;
static int width = 720, height = 500;
static char path[300], program[320], notice[160];
static int top, left, rows, columns, gutter;
static int numbers_on = 1, panel_on = 1, panel = P_OUTPUT, checking_on = 1;
static int dragging, selecting, leaving;
static unsigned click_at;
static int click_x, click_y;

/* the lines' colours: whether each begins inside a comment, known up to sx_known */
static signed char *sx_starts;
static int sx_known, sx_room;
static unsigned char *sx_kinds;
static int sx_kinds_room;
static const GR_COLOR sx_colours[SX_KINDS] = { MWRGB(0, 0, 0), MWRGB(0, 0, 200), MWRGB(0, 110, 130), MWRGB(130, 0, 130),
					      MWRGB(160, 40, 20), MWRGB(0, 110, 60), MWRGB(0, 128, 0), MWRGB(110, 80, 0) };

/* the status line when it asks for something */
static int asking;
static char answer[200], sought[64];

/* what runs beside: a build, the program, or a quiet check of what is being typed */
static struct { pid_t pid; int fd, kind; } job;
static int then_do;		/* what a build that works goes on to: C_RUN or C_DEBUG */
static char job_text[8192];	/* a build's or a check's own lines, for the problems */
static int job_length;
static int debug_fd = -1;	/* the debugged program's output */
static unsigned edited_at;	/* when the text last changed, and whether it was checked since */
static int unchecked;

static char *out_lines[OUT_MOST];
static int out_count, out_top, out_fresh, worker_marked;
static char out_part[200];
static struct problem { int line, error; char text[150]; } problems[64];
static int problem_count, problem_top;
static char variables[48][160], stack[16][160];
static int variable_count, variable_top, stack_count;

static char examples[12][40];
static int example_count;
static int print_on;		/* NXIDE_PRINT: what happens, on the console, for a test */
static char script[2000];	/* NXIDE_SCRIPT: commands done one after another, when nothing runs */

static void command(int id);
static void draw_all(void);

static unsigned
milliseconds(void)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	return (unsigned)(now.tv_sec * 1000 + now.tv_usec / 1000);
}

static void
say(const char *text)
{
	snprintf(notice, sizeof notice, "%s", text);
	if (print_on) {
		printf("nxide: %s\n", text);
		fflush(stdout);
	}
}

/* ---- the layout ---- */

static int
editor_y(void)
{
	return MENU_H + TOOL_H;
}

static int
panel_y(void)
{
	return height - STATUS - (panel_on ? PANEL_H : 0);
}

static void
resized(int w, int h)
{
	width = w, height = h;
	gutter = numbers_on ? 5 * ui_fw + 16 : 16;
	rows = (panel_y() - editor_y()) / ui_fh;
	columns = (width - UI_SCROLL_W - gutter - 4) / ui_fw;
	if (rows < 1)
		rows = 1;
	if (columns < 8)
		columns = 8;
}

/* ---- colours: a line's kinds need to know whether it begins inside a comment ---- */

static unsigned char *
kinds_of(int line)
{
	int n = (int)strlen(tx_lines[line]) + 4;

	if (tx_count + 2 > sx_room) {
		sx_room = tx_count * 2 + 64;
		sx_starts = realloc(sx_starts, (size_t)sx_room);
	}
	if (n > sx_kinds_room)
		sx_kinds = realloc(sx_kinds, (size_t)(sx_kinds_room = n * 2));
	if (sx_known < 1)
		sx_starts[0] = 0, sx_known = 1;
	while (sx_known <= line) {
		int m = (int)strlen(tx_lines[sx_known - 1]) + 4;

		if (m > sx_kinds_room)
			sx_kinds = realloc(sx_kinds, (size_t)(sx_kinds_room = m * 2));
		sx_starts[sx_known] = (signed char)sx_line(tx_lines[sx_known - 1], sx_starts[sx_known - 1], sx_kinds);
		sx_known++;
	}
	sx_line(tx_lines[line], sx_starts[line], sx_kinds);
	return sx_kinds;
}

/* ---- drawing ---- */

static int
problem_at(int line)
{
	int i, found = 0;

	for (i = 0; i < problem_count; i++)
		if (problems[i].line == line + 1)
			found |= problems[i].error ? 2 : 1;
	return found;
}

static void
draw_line(int line)
{
	int y = editor_y() + (line - top) * ui_fh, x = gutter + 3, length, x0, y0, x1, y1, from = 0, to = 0, a, b, wrong;
	int here = dbg_state == DBG_STOPPED && dbg_line_now == line + 1;
	unsigned char *kinds;
	const char *text;
	char number[12];

	if (line < top || line >= top + rows)
		return;
	ui_fill(window, 0, y, gutter, ui_fh, MWRGB(232, 232, 232));
	ui_fill(window, gutter, y, width - UI_SCROLL_W - gutter, ui_fh, here ? MWRGB(255, 255, 150) : WHITE);
	if (line >= tx_count)
		return;
	if (numbers_on) {
		snprintf(number, sizeof number, "%4d", line + 1);
		ui_text(window, 14, y, number, -1, UI_SHADOW, 1);
	}
	if (dbg_marked(line + 1)) {
		GrSetGCForeground(ui_gc, MWRGB(200, 20, 20));
		GrFillEllipse(window, ui_gc, 7, y + ui_fh / 2, 4, 4);
	}
	if (here)
		ui_text(window, 2, y, ">", 1, BLACK, 1);
	if ((wrong = problem_at(line)) != 0)
		ui_fill(window, gutter - 3, y, 3, ui_fh, wrong & 2 ? MWRGB(220, 0, 0) : MWRGB(230, 150, 0));
	text = tx_lines[line];
	length = (int)strlen(text);
	if (tx_selection(&x0, &y0, &x1, &y1) && line >= y0 && line <= y1) {
		/* the selected columns of this line; a line's end counts as one more */
		from = (line == y0 ? x0 : 0) - left;
		to = (line == y1 ? x1 : length + 1) - left;
		from = from < 0 ? 0 : from > columns ? columns : from;
		to = to < 0 ? 0 : to > columns ? columns : to;
		if (to > from)
			ui_fill(window, x + from * ui_fw, y, (to - from) * ui_fw, ui_fh, UI_SELECTED);
	}
	kinds = kinds_of(line);
	/* a run of one colour is one request; spaces go with what is before them */
	for (a = left; a < length && a < left + columns; a = b) {
		int kind = kinds[a], selected = a - left >= from && a - left < to;

		for (b = a + 1; b < length && b < left + columns && (kinds[b] == kind || text[b] == ' ') &&
				(b - left >= from && b - left < to) == selected; b++)
			continue;
		ui_text(window, x + (a - left) * ui_fw, y, text + a, b - a, selected ? WHITE : sx_colours[kind], 1);
	}
	if (line == tx_y && tx_x >= left && tx_x <= left + columns)
		ui_fill(window, x + (tx_x - left) * ui_fw, y, 2, ui_fh, BLACK);
}

static void
draw_editor_from(int line)
{
	for (line = line < top ? top : line; line < top + rows; line++)
		draw_line(line);
	ui_fill(window, 0, editor_y() + rows * ui_fh, width - UI_SCROLL_W, panel_y() - editor_y() - rows * ui_fh, WHITE);
	ui_scrollbar(window, width - UI_SCROLL_W, editor_y(), panel_y() - editor_y(), tx_count, rows, top);
}

static const char *const menu_names[] = { "File", "Edit", "Selection", "View", "Run", "Help" };
static int menu_x[7];

static void
draw_menu_bar(void)
{
	int i;

	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < 6; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
}

static const struct { const char *label; int id, gap; } tools[] = {
	{ "New", C_NEW, 0 }, { "Open", C_OPEN, 0 }, { "Save", C_SAVE, 0 }, { "Undo", C_UNDO, 8 }, { "Redo", C_REDO, 0 },
	{ "Build", C_BUILD, 8 }, { "Run", C_RUN, 0 }, { "Debug", C_DEBUG, 0 }, { "Stop", C_STOP, 0 },
	{ "Over", C_OVER, 8 }, { "Into", C_INTO, 0 }, { "Out", C_OUT, 0 },
};
#define TOOLS ((int)(sizeof tools / sizeof tools[0]))
static int tool_x[TOOLS + 1], tool_w[TOOLS];

/* Whether a command can be done as things stand. */
static int
can(int id)
{
	int idle = job.kind != J_BUILD && job.kind != J_RUN && dbg_state == DBG_GONE;

	switch (id) {
	case C_UNDO: return tx_done_count > 0;
	case C_REDO: return tx_undone_count > 0;
	case C_BUILD: case C_RUN: return idle;
	case C_DEBUG: return idle || dbg_state == DBG_STOPPED;
	case C_STOP: return !idle;
	case C_OVER: case C_INTO: case C_OUT: return dbg_state == DBG_STOPPED;
	}
	return 1;
}

static void
draw_tools(void)
{
	int i;

	ui_fill(window, 0, MENU_H, width, TOOL_H, UI_FACE);
	for (i = 0; i < TOOLS; i++) {
		const char *label = tools[i].id == C_DEBUG && dbg_state == DBG_STOPPED ? "Go on" : tools[i].label;

		ui_bevel(window, tool_x[i], MENU_H + 3, tool_w[i], 22, 0);
		ui_text(window, tool_x[i] + 7, MENU_H + 7, label, -1, can(tools[i].id) ? BLACK : UI_SHADOW, 0);
	}
	ui_fill(window, 0, MENU_H + TOOL_H - 1, width, 1, UI_SHADOW);
}

static void
draw_status(void)
{
	static const char *const asks[] = { "", "Find: ", "Go to line: ", "Save as: " };
	char text[400];

	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_bevel(window, 0, height - STATUS, width, STATUS, 1);
	if (asking)
		snprintf(text, sizeof text, "%s%s_   (Enter, or Escape to leave)", asks[asking], answer);
	else
		snprintf(text, sizeof text, "%s%s   %s", path, tx_modified ? " *" : "", notice);
	ui_text_fit(window, 6, height - STATUS + 3, width - 150, text, BLACK);
	snprintf(text, sizeof text, "line %d, column %d", tx_y + 1, tx_x + 1);
	ui_text(window, width - 132, height - STATUS + 3, text, -1, BLACK, 0);
}

static void
draw_panel(void)
{
	static const char *const names[PANELS] = { "Output", "Problems", "Variables", "Call stack" };
	int y = panel_y(), i, shown = (PANEL_H - TAB_H - 2) / 15, count, first;
	char label[40];

	if (!panel_on)
		return;
	ui_fill(window, 0, y, width, TAB_H, UI_FACE);
	ui_fill(window, 0, y, width, 1, UI_SHADOW);
	for (i = 0; i < PANELS; i++) {
		snprintf(label, sizeof label, i == P_PROBLEMS && problem_count ? "%s (%d)" : "%s", names[i], problem_count);
		if (i == panel)
			ui_fill(window, 4 + i * 96, y + 2, 92, TAB_H - 2, WHITE);
		ui_text(window, 10 + i * 96, y + 3, label, -1, BLACK, 0);
	}
	ui_fill(window, 0, y + TAB_H, width, PANEL_H - TAB_H, WHITE);
	count = panel == P_OUTPUT ? out_count : panel == P_PROBLEMS ? problem_count : panel == P_VARIABLES ? variable_count : stack_count;
	first = panel == P_OUTPUT ? out_top : panel == P_PROBLEMS ? problem_top : panel == P_VARIABLES ? variable_top : 0;
	for (i = 0; i < shown && first + i < count; i++) {
		int k = first + i, py = y + TAB_H + 2 + i * 15;

		if (panel == P_OUTPUT)
			ui_text_fit(window, 6, py, width - 12, out_lines[k], BLACK);
		else if (panel == P_PROBLEMS) {
			snprintf(label, sizeof label, "line %d", problems[k].line);
			ui_text(window, 6, py, label, -1, problems[k].error ? MWRGB(200, 0, 0) : MWRGB(170, 100, 0), 0);
			ui_text_fit(window, 76, py, width - 82, problems[k].text, BLACK);
		} else
			ui_text_fit(window, 6, py, width - 12, panel == P_VARIABLES ? variables[k] : stack[k], BLACK);
	}
	out_fresh = 0;
}

static void
draw_all(void)
{
	draw_menu_bar();
	draw_tools();
	draw_editor_from(top);
	draw_panel();
	draw_status();
}

static void
show_panel(int which)
{
	panel = which;
	if (!panel_on) {
		panel_on = 1;
		resized(width, height);
		draw_all();
	} else
		draw_panel();
}

/* ---- the view follows the cursor ---- */

static int
follow(void)
{
	int old_top = top, old_left = left;

	if (tx_y < top)
		top = tx_y;
	if (tx_y >= top + rows)
		top = tx_y - rows + 1;
	if (tx_x < left)
		left = tx_x;
	if (tx_x >= left + columns)
		left = tx_x - columns + 1;
	return top != old_top || left != old_left;
}

static void
scroll_to(int line)
{
	if (line > tx_count - rows)
		line = tx_count - rows;
	if (line < 0)
		line = 0;
	if (line != top) {
		top = line;
		draw_editor_from(top);
	}
}

static void
go_to_line(int line)
{
	if (line < 1)
		line = 1;
	if (line > tx_count)
		line = tx_count;
	tx_y = line - 1, tx_x = 0, tx_marked = 0;
	if (tx_y < top || tx_y >= top + rows)
		top = tx_y > rows / 3 ? tx_y - rows / 3 : 0;
	left = 0;
	draw_editor_from(top);
	draw_status();
}

/* After anything that may have changed the text: draws what it has to, and no more. */
static void
changed(int was_y)
{
	int first = tx_touched, whole = tx_shifted, before;

	if (first == 1 << 30) {
		draw_line(was_y);
		draw_line(tx_y);
		return;
	}
	/* the line after: if it now begins inside a comment and did not, all below it changed colour */
	before = first + 1 < sx_known ? sx_starts[first + 1] : -1;
	if (sx_known > first + 1)
		sx_known = first + 1;
	if (!whole && first + 1 < tx_count) {
		kinds_of(first + 1);
		whole = before >= 0 && sx_starts[first + 1] != before;
	}
	tx_touched = 1 << 30, tx_shifted = 0;
	edited_at = milliseconds(), unchecked = 1;
	if (whole)
		draw_editor_from(first);
	else {
		draw_line(first);
		draw_line(was_y);
		draw_line(tx_y);
	}
}

/* ---- the panels' text ---- */

static void
out_line(const char *text)
{
	if (out_count == OUT_MOST) {
		int i;

		for (i = 0; i < OUT_MOST / 2; i++)
			free(out_lines[i]);
		memmove(out_lines, out_lines + OUT_MOST / 2, (OUT_MOST - OUT_MOST / 2) * sizeof out_lines[0]);
		out_count -= OUT_MOST / 2;
	}
	/* the workers' library says so when one of them is stopped by a mark (debug_changed) */
	if (strstr(text, "a worker core stopped, cause 00000002"))
		worker_marked = 1;
	out_lines[out_count++] = strdup(text);
	out_top = out_count > (PANEL_H - TAB_H - 2) / 15 ? out_count - (PANEL_H - TAB_H - 2) / 15 : 0;
	out_fresh = 1;
	if (print_on)
		printf("nxide| %s\n", text);
}

/* What a program or the compiler wrote: whole lines to the output, the rest kept for later. */
static void
out_bytes(const char *bytes, int count)
{
	int n = (int)strlen(out_part), i;

	for (i = 0; i < count; i++) {
		if (bytes[i] == '\n' || n == (int)sizeof out_part - 1) {
			out_part[n] = 0;
			out_line(out_part);
			n = 0;
		}
		if (bytes[i] != '\n' && bytes[i] != '\r')
			out_part[n++] = bytes[i] == '\t' ? ' ' : bytes[i];
	}
	out_part[n] = 0;
}

/* The compiler's lines ("file:LINE: error: what") as problems at their lines. */
static void
problems_from(const char *text)
{
	problem_count = problem_top = 0;
	while (*text && problem_count < (int)(sizeof problems / sizeof problems[0])) {
		const char *end = strchr(text, '\n'), *at;
		int length = end ? (int)(end - text) : (int)strlen(text);

		for (at = text; at < text + length; at++)
			if (at[0] == ':' && isdigit((unsigned char)at[1]) && at > text) {
				struct problem *p = &problems[problem_count];
				const char *what = at + 1;

				p->line = (int)strtol(at + 1, (char **)&what, 10);
				if (*what != ':')
					continue;
				what += what[1] == ' ' ? 2 : 1;
				p->error = !!memmem(what, (size_t)(text + length - what), "error", 5);
				if (!p->error && !memmem(what, (size_t)(text + length - what), "warning", 7))
					break;
				snprintf(p->text, sizeof p->text, "%.*s", (int)(text + length - what), what);
				problem_count++;
				break;
			}
		text += length + (end != NULL);
	}
}

/* ---- what runs beside ---- */

static void
job_start(int kind, const char *shell)
{
	int ends[2];

	if (pipe(ends) < 0)
		return;
	job.pid = fork();
	if (job.pid == 0) {
		setpgid(0, 0);
		dup2(ends[1], 1), dup2(ends[1], 2);
		close(ends[0]), close(ends[1]);
		execl("/bin/sh", "sh", "-c", shell, (char *)NULL);
		_exit(127);
	}
	close(ends[1]);
	fcntl(ends[0], F_SETFL, O_NONBLOCK);
	job.fd = ends[0], job.kind = kind;
	job_length = 0;
	job_text[0] = 0;
}

static void
job_stop(void)
{
	if (job.kind == J_NONE)
		return;
	kill(-job.pid, SIGKILL);
	kill(job.pid, SIGKILL);
	waitpid(job.pid, NULL, 0);
	close(job.fd);
	job.kind = J_NONE;
}

static void
debug_shown(void)
{
	int i;

	variable_count = dbg_variables(variables, 48, dbg_function_now, dbg_regs[8]);
	stack_count = dbg_stack(stack, 16);
	variable_top = 0;
	if (print_on) {
		for (i = 0; i < variable_count; i++)
			printf("nxide: variable: %s\n", variables[i]);
		for (i = 0; i < stack_count; i++)
			printf("nxide: in %s\n", stack[i]);
	}
}

/* The debugged program stopped, or is gone. */
static void
debug_changed(void)
{
	char text[160];

	if (dbg_state == DBG_GONE) {
		if (dbg_exit < 0)
			snprintf(text, sizeof text, "the program was ended by signal %d", -dbg_exit);
		else
			snprintf(text, sizeof text, "the program ended, with %d", dbg_exit);
		if (worker_marked)
			snprintf(text, sizeof text, "a worker core came to a marked line and the program ended: only the first core's lines can be marked");
		worker_marked = 0;
		say(text);
		out_line(text);
		variable_count = stack_count = 0;
		draw_all();
		return;
	}
	if (dbg_signal)
		snprintf(text, sizeof text, "signal %d (%s) at line %d: Go on passes it to the program", dbg_signal, strsignal(dbg_signal), dbg_line_now);
	else
		snprintf(text, sizeof text, "stopped at line %d, in %s", dbg_line_now, dbg_function_now >= 0 ? dbg_functions[dbg_function_now].name : "?");
	say(text);
	debug_shown();
	panel = panel == P_STACK ? P_STACK : P_VARIABLES;
	if (dbg_line_now) {
		tx_y = dbg_line_now - 1 < tx_count ? dbg_line_now - 1 : tx_count - 1;
		tx_x = 0, tx_marked = 0;
		if (tx_y < top || tx_y >= top + rows)
			top = tx_y > rows / 3 ? tx_y - rows / 3 : 0;
	}
	draw_all();
}

static void
debug_begin(void)
{
	int ends[2];

	if (!dbg_load(program)) {
		say("this program has nothing for a debugger in it");
		return;
	}
	if (pipe(ends) < 0)
		return;
	if (!dbg_start(program, ends[1])) {
		close(ends[0]), close(ends[1]);
		say("the program did not start");
		return;
	}
	close(ends[1]);
	fcntl(ends[0], F_SETFL, O_NONBLOCK);
	debug_fd = ends[0];
	/* a mark is an instruction changed in memory every core runs: a worker that comes to one
	 * stops for good, and nothing here can hold or release it */
	say(dbg_shared && dbg_mark_count ? "debugging. This program has worker cores: a mark on a line they run ends it (only the first core can be held)"
					 : "debugging");
	/* to the first marked line, or with none to the first line of main */
	if (dbg_mark_count)
		dbg_go(DBG_RUN);
	else
		dbg_step(DBG_INTO);
	draw_tools();
	draw_status();
}

static void
job_ended(int kind, int status)
{
	char text[160], shell[700];
	int errors = 0, i;

	if (kind == J_RUN) {
		if (WIFSIGNALED(status))
			snprintf(text, sizeof text, "the program was ended by signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
		else
			snprintf(text, sizeof text, "the program ended, with %d", WEXITSTATUS(status));
		say(text);
		out_line(text);
		draw_all();
		return;
	}
	problems_from(job_text);
	for (i = 0; i < problem_count; i++)
		errors += problems[i].error;
	if (kind == J_CHECK) {
		draw_editor_from(top);
		draw_panel();
		return;
	}
	if (errors || !WIFEXITED(status) || WEXITSTATUS(status)) {
		snprintf(text, sizeof text, "not built: %d error%s", errors, errors == 1 ? "" : "s");
		say(text);
		panel = problem_count ? P_PROBLEMS : P_OUTPUT;
		if (problem_count)
			go_to_line(problems[0].line);
	} else {
		snprintf(text, sizeof text, "built: %s%s", program, problem_count ? ", with warnings" : "");
		say(text);
		if (then_do == C_RUN) {
			snprintf(shell, sizeof shell, "exec '%s'", program);
			job_start(J_RUN, shell);
			say("running");
			panel = P_OUTPUT;
		} else if (then_do == C_DEBUG)
			debug_begin();
	}
	then_do = 0;
	draw_all();
}

/* Looks at what runs beside; true when something was drawn. */
static void
poll_all(void)
{
	char bytes[512];
	int n, status;

	if (job.kind != J_NONE) {
		pid_t ended = waitpid(job.pid, &status, WNOHANG);

		while ((n = (int)read(job.fd, bytes, sizeof bytes)) > 0) {
			if (job.kind != J_RUN && job_length + n < (int)sizeof job_text) {
				memcpy(job_text + job_length, bytes, (size_t)n);
				job_text[job_length += n] = 0;
			}
			if (job.kind != J_CHECK)
				out_bytes(bytes, n);
		}
		if (ended == job.pid || (ended < 0 && errno == ECHILD)) {
			int kind = job.kind;

			if (ended < 0)
				status = 0;	/* (it ended unseen: what it wrote says how) */

			close(job.fd);
			job.kind = J_NONE;
			job_ended(kind, status);
		}
	}
	if (debug_fd >= 0) {
		while ((n = (int)read(debug_fd, bytes, sizeof bytes)) > 0)
			out_bytes(bytes, n);
		if (dbg_state == DBG_GONE) {
			close(debug_fd);
			debug_fd = -1;
		}
	}
	if (dbg_poll())
		debug_changed();
	if (out_fresh && panel == P_OUTPUT)
		draw_panel();
	/* nothing typed for a while: the compiler looks at the text as it is, quietly */
	if (checking_on && unchecked && job.kind == J_NONE && dbg_state == DBG_GONE && milliseconds() - edited_at > 1500) {
		FILE *file = fopen("/tmp/ide-check.c", "w");
		int i;

		unchecked = 0;
		if (!file)
			return;
		for (i = 0; i < tx_count; i++)
			fprintf(file, "%s\n", tx_lines[i]);
		fclose(file);
		job_start(J_CHECK, "exec tcc -static -mfpu -c /tmp/ide-check.c -o /tmp/ide-check.o 2>&1");
	}
}

/* ---- files ---- */

static void
named(const char *file)
{
	const char *leaf;
	char title[340];
	int n;

	snprintf(path, sizeof path, "%s", file);
	leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	n = (int)strcspn(leaf, ".");
	snprintf(program, sizeof program, "/tmp/ide-%.*s", n, leaf);
	snprintf(title, sizeof title, "%s - IDE", leaf);
	if (window)
		GrSetWindowTitle(window, title);
	if (print_on)
		printf("nxide: the file is %s\n", path);
}

static void
opened(void)
{
	sx_known = 0;
	top = left = 0;
	problem_count = 0;
	dbg_mark_count = 0;
	tx_touched = 1 << 30, tx_shifted = 0;
	unchecked = 1, edited_at = milliseconds();
}

static void
open_file(const char *file)
{
	if (job.kind == J_CHECK)
		job_stop();
	named(file);
	say(tx_load(path) ? "" : "a new file");
	opened();
}

/* An example is the user's copy of it once there is one: /root/NAME.c. */
static void
open_example(int n)
{
	char own[120], source[120];

	snprintf(own, sizeof own, "/root/%s.c", examples[n]);
	snprintf(source, sizeof source, "/usr/share/example-%s.c", examples[n]);
	if (access(own, R_OK) == 0) {
		open_file(own);
		return;
	}
	open_file(source);
	named(own);
	tx_modified = 1;
	say("an example: Save keeps your copy of it");
}

static void
save(void)
{
	say(tx_save(path) ? "saved" : "cannot write the file");
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
}

/* ---- the menus ---- */

static struct ui_menu_item example_items[12];
static const struct ui_menu_item file_items[] = {
	{ "New", -1, C_NEW }, { "Open...   Ctrl+O", -1, C_OPEN }, { "Save   Ctrl+S", -1, C_SAVE }, { "Save as...", -1, C_SAVE_AS },
	{ "Examples", -1, 0, example_items, 0, UI_MENU_LINE }, { "Exit   Ctrl+Q", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item edit_items[] = {
	{ "Undo   Ctrl+Z", -1, C_UNDO }, { "Redo   Ctrl+Y", -1, C_REDO }, { "Cut   Ctrl+X", -1, C_CUT, NULL, 0, UI_MENU_LINE },
	{ "Copy   Ctrl+C", -1, C_COPY }, { "Paste   Ctrl+V", -1, C_PASTE }, { "Delete", -1, C_DELETE },
	{ "Find...   Ctrl+F", -1, C_FIND, NULL, 0, UI_MENU_LINE }, { "Find next   F3", -1, C_FIND_NEXT }, { "Go to line...   Ctrl+G", -1, C_GOTO },
};
static const struct ui_menu_item selection_items[] = {
	{ "Select all   Ctrl+A", -1, C_ALL }, { "Select the line   Ctrl+L", -1, C_LINE }, { "Select the word   Ctrl+W", -1, C_WORD },
	{ "Indent   Tab", -1, C_INDENT, NULL, 0, UI_MENU_LINE }, { "Unindent   Shift+Tab", -1, C_UNINDENT }, { "Comment or uncomment   Ctrl+/", -1, C_COMMENT },
};
static const struct ui_menu_item view_items[] = {
	{ "The panel below", -1, C_PANEL }, { "Line numbers", -1, C_NUMBERS }, { "Check while typing", -1, C_CHECKING },
};
static const struct ui_menu_item run_items[] = {
	{ "Build   F7", -1, C_BUILD }, { "Run   F6", -1, C_RUN }, { "Debug, or go on   F5", -1, C_DEBUG }, { "Stop   Shift+F5", -1, C_STOP },
	{ "Step over   F10", -1, C_OVER, NULL, 0, UI_MENU_LINE }, { "Step into   F11", -1, C_INTO }, { "Step out   F12", -1, C_OUT },
	{ "Mark or unmark the line   F9", -1, C_MARK, NULL, 0, UI_MENU_LINE }, { "Unmark every line", -1, C_UNMARK_ALL },
};
static const struct ui_menu_item help_items[] = { { "Keys", -1, C_KEYS }, { "About", -1, C_ABOUT } };
static struct { const struct ui_menu_item *items; int count; } menus[6] = {
	{ file_items, 6 }, { edit_items, 9 }, { selection_items, 6 }, { view_items, 3 }, { run_items, 9 }, { help_items, 2 },
};

static void
menu_open(int which, int keyboard)
{
	static struct ui_menu_item files[6];
	struct { int x, y; } info;

	ui_window_origin(window, &info.x, &info.y);
	if (which == 0) {	/* (the examples are found when the program starts: their count is not constant) */
		memcpy(files, file_items, sizeof files);
		files[4].sub_count = example_count;
		ui_menu_open(files, 6, info.x + menu_x[0], info.y + MENU_H, 0, keyboard);
		return;
	}
	ui_menu_open(menus[which].items, menus[which].count, info.x + menu_x[which], info.y + MENU_H, 0, keyboard);
}

/* ---- commands ---- */

static void
copy_selection(void)
{
	int x0, y0, x1, y1, length;
	char *text;

	if (!tx_selection(&x0, &y0, &x1, &y1)) {
		say("nothing is selected");
		return;
	}
	text = tx_text(x0, y0, x1, y1, &length);
	ui_clip_set(text, length);
	free(text);
	say("copied");
}

/* The lines the selection touches (or the cursor's), each given `spaces` more or fewer in front. */
static void
indent(int spaces)
{
	int x0, y0, x1, y1, y, n;

	if (!tx_selection(&x0, &y0, &x1, &y1))
		y0 = y1 = tx_y, x1 = 1;
	if (x1 == 0 && y1 > y0)
		y1--;
	for (y = y0; y <= y1; y++) {
		if (spaces > 0 && tx_lines[y][0])
			tx_insert(0, y, "    ", spaces);
		for (n = 0; spaces < 0 && n < -spaces && tx_lines[y][n] == ' '; n++)
			continue;
		if (spaces < 0 && n)
			tx_delete(0, y, n, y);
	}
	tx_ax = 0, tx_ay = y0, tx_x = (int)strlen(tx_lines[y1]), tx_y = y1;
	tx_marked = y1 > y0;
}

static void
comment(void)
{
	int x0, y0, x1, y1, y, n, all = 1;

	if (!tx_selection(&x0, &y0, &x1, &y1))
		y0 = y1 = tx_y, x1 = 1;
	if (x1 == 0 && y1 > y0)
		y1--;
	for (y = y0; y <= y1; y++) {
		n = (int)strspn(tx_lines[y], " ");
		all &= !tx_lines[y][n] || !strncmp(tx_lines[y] + n, "//", 2);
	}
	for (y = y0; y <= y1; y++) {
		n = (int)strspn(tx_lines[y], " ");
		if (!tx_lines[y][n])
			continue;
		if (all)
			tx_delete(n, y, n + (tx_lines[y][n + 2] == ' ' ? 3 : 2), y);
		else
			tx_insert(n, y, "// ", 3);
	}
	tx_ax = 0, tx_ay = y0, tx_x = (int)strlen(tx_lines[y1]), tx_y = y1;
	tx_marked = y1 > y0;
}

static void
select_word(void)
{
	const char *line = tx_lines[tx_y];
	int a = tx_x, b = tx_x;

	while (a > 0 && (isalnum((unsigned char)line[a - 1]) || line[a - 1] == '_'))
		a--;
	while (isalnum((unsigned char)line[b]) || line[b] == '_')
		b++;
	tx_ax = a, tx_ay = tx_y, tx_x = b, tx_marked = b > a;
}

static void
build(int then)
{
	char shell[800];

	if (job.kind == J_CHECK)
		job_stop();
	if (!tx_save(path)) {
		say("cannot write the file");
		return;
	}
	then_do = then;
	problem_count = 0;
	out_line("");
	snprintf(shell, sizeof shell, "cc -g '%s' -o '%s' 2>&1", path, program);
	out_line(shell);
	job_start(J_BUILD, shell);
	say("building...");
	panel = P_OUTPUT;
	unchecked = 0;
}

static void
help(int about)
{
	static const char *const keys[] = {
		"F7 builds, F6 builds and runs, F5 builds and runs under the debugger (and goes on from a stop), Shift+F5 stops.",
		"F9 marks the line to stop at (or click before its number). F10 steps over a line, F11 into a call, F12 out of the function.",
		"Ctrl+Z and Ctrl+Y take a change back and make it again. Ctrl+F finds, F3 finds the next, Ctrl+G goes to a line.",
		"Tab and Shift+Tab indent the selected lines, Ctrl+/ comments them. Ctrl+L selects the line, Ctrl+W the word.",
		"A red bar before a line is the compiler's error there, an orange one its warning: Problems below says what.",
	};
	static const char *const what[] = {
		"nxide: an editor, the machine's own C compiler (TinyCC) and a debugger, all running in the machine.",
		"The colours are a lexer's; the compiler checks the text a moment after you stop typing. File > Examples has programs to begin from.",
	};
	int i;

	out_line("");
	for (i = 0; i < (about ? 2 : 5); i++)
		out_line(about ? what[i] : keys[i]);
	show_panel(P_OUTPUT);
}

static void
ask(int what, const char *start)
{
	asking = what;
	snprintf(answer, sizeof answer, "%s", start);
	draw_status();
}

static void
command_done(int id)
{
	static const char *const fresh = "#include <stdio.h>\n\nint main(void)\n{\n    printf(\"hello\\n\");\n    return 0;\n}\n";
	char chosen[300], *text;
	int was = tx_y, length, i;

	if (!can(id)) {
		say("not now");
		draw_status();
		return;
	}
	tx_step++;
	notice[0] = 0;
	if (id != C_EXIT)
		leaving = 0;
	switch (id) {
	case C_NEW:
		open_file("/root/untitled.c");
		if (tx_count == 1 && !tx_lines[0][0]) {
			tx_insert(0, 0, fresh, (int)strlen(fresh));
			tx_x = tx_y = 0;
		}
		opened();
		draw_all();
		return;
	case C_OPEN:
		if (ui_choose_file("Open", "/root", ".c .h .txt", chosen, sizeof chosen, others))
			open_file(chosen);
		draw_all();
		return;
	case C_SAVE:
		save();
		break;
	case C_SAVE_AS:
		ask(ASK_SAVE, path);
		return;
	case C_EXIT:
		if (tx_modified && !leaving) {
			leaving = 1;
			say("not saved: Ctrl+S saves, Exit once more leaves without");
			draw_status();
			return;
		}
		job_stop();
		dbg_end();
		GrClose();
		exit(0);
	case C_UNDO:
	case C_REDO:
		if (!tx_replay(id == C_REDO))
			say(id == C_REDO ? "nothing to make again" : "nothing to take back");
		break;
	case C_CUT:
		copy_selection();
		tx_delete_selection();
		break;
	case C_COPY:
		copy_selection();
		break;
	case C_PASTE:
		if ((text = ui_clip_get(&length)) != NULL) {
			tx_delete_selection();
			tx_insert(tx_x, tx_y, text, length);
			free(text);
		}
		break;
	case C_DELETE:
		tx_delete_selection();
		break;
	case C_FIND:
		ask(ASK_FIND, sought);
		return;
	case C_FIND_NEXT:
		say(tx_find(sought) ? "found" : sought[0] ? "not found" : "Ctrl+F says what to find");
		break;
	case C_GOTO:
		ask(ASK_GOTO, "");
		return;
	case C_ALL:
		tx_ax = tx_ay = 0, tx_y = tx_count - 1, tx_x = (int)strlen(tx_lines[tx_y]), tx_marked = 1;
		break;
	case C_LINE:
		tx_ax = 0, tx_ay = tx_y, tx_marked = 1;
		if (tx_y + 1 < tx_count)
			tx_y++, tx_x = 0;
		else
			tx_x = (int)strlen(tx_lines[tx_y]);
		break;
	case C_WORD:
		select_word();
		break;
	case C_INDENT:
		indent(TX_TAB);
		break;
	case C_UNINDENT:
		indent(-TX_TAB);
		break;
	case C_COMMENT:
		comment();
		break;
	case C_PANEL:
		panel_on = !panel_on;
		resized(width, height);
		draw_all();
		return;
	case C_NUMBERS:
		numbers_on = !numbers_on;
		resized(width, height);
		draw_all();
		return;
	case C_CHECKING:
		checking_on = !checking_on;
		say(checking_on ? "the compiler checks the text while you type" : "the text is checked only when it is built");
		break;
	case C_BUILD:
		build(0);
		draw_all();
		return;
	case C_RUN:
		build(C_RUN);
		draw_all();
		return;
	case C_DEBUG:
		if (dbg_state == DBG_STOPPED) {
			dbg_go(DBG_RUN);
			say("running");
		} else
			build(C_DEBUG);
		draw_all();
		return;
	case C_STOP:
		if (job.kind == J_BUILD || job.kind == J_RUN)
			job_stop();
		dbg_end();
		then_do = 0;
		variable_count = stack_count = 0;
		say("stopped");
		draw_all();
		return;
	case C_OVER:
	case C_INTO:
	case C_OUT:
		dbg_step(id == C_OVER ? DBG_OVER : id == C_INTO ? DBG_INTO : DBG_OUT);
		draw_tools();
		return;
	case C_MARK:
		for (i = 0; i < dbg_mark_count && dbg_marks[i] != tx_y + 1; i++)
			continue;
		if (i < dbg_mark_count)
			dbg_marks[i] = dbg_marks[--dbg_mark_count];
		else if (dbg_mark_count < (int)(sizeof dbg_marks / sizeof dbg_marks[0]))
			dbg_marks[dbg_mark_count++] = tx_y + 1;
		draw_line(tx_y);
		return;
	case C_UNMARK_ALL:
		dbg_mark_count = 0;
		draw_editor_from(top);
		return;
	case C_KEYS:
	case C_ABOUT:
		help(id == C_ABOUT);
		return;
	default:
		if (id >= C_EXAMPLE && id < C_EXAMPLE + example_count) {
			open_example(id - C_EXAMPLE);
			draw_all();
		}
		return;
	}
	if (follow() || tx_marked || id == C_UNDO || id == C_REDO) {
		changed(was);
		draw_editor_from(top);
	} else
		changed(was);
	draw_tools();
	draw_status();
}

/* A command, from a menu, a key of the row, the keyboard or a test: with NXIDE_PRINT, what it left. */
static void
command(int id)
{
	command_done(id);
	if (print_on) {
		printf("nxide: command %d: at %d,%d of %d lines, %s, %d to take back, %d to make again\n", id, tx_x, tx_y, tx_count,
		       tx_modified ? "changed" : "as saved", tx_done_count, tx_undone_count);
		fflush(stdout);
	}
}

/* ---- keys ---- */

/* A key of the line the status bar asks with. */
static void
ask_key(int ch)
{
	int n = (int)strlen(answer), what = asking;

	if (ch == MWKEY_ESCAPE) {
		asking = ASK_NONE;
	} else if (ch == MWKEY_ENTER || ch == '\r' || ch == '\n') {
		asking = ASK_NONE;
		if (what == ASK_FIND) {
			snprintf(sought, sizeof sought, "%s", answer);
			command(C_FIND_NEXT);
			return;
		}
		if (what == ASK_GOTO)
			go_to_line(atoi(answer));
		if (what == ASK_SAVE && answer[0] == '/') {
			named(answer);
			save();
		}
	} else if (ch == MWKEY_BACKSPACE || ch == 127) {
		if (n)
			answer[n - 1] = 0;
	} else if (ch >= 32 && ch < 127 && n < (int)sizeof answer - 1) {
		answer[n] = (char)ch, answer[n + 1] = 0;
	}
	draw_status();
}

static void
key(int ch, int modifiers)
{
	int was = tx_y, length = (int)strlen(tx_lines[tx_y]), shift = (modifiers & MWKMOD_SHIFT) != 0, moves = 0, had = tx_marked, n, id = 0;
	char one;

	if (asking) {
		ask_key(ch);
		return;
	}
	/* a letter with Ctrl comes as the letter with the modifier, or as its control code */
	if ((modifiers & MWKMOD_CTRL) && ch < 128 && (isalpha(ch) || ch == '/'))
		ch = ch == '/' ? 31 : tolower(ch) - 'a' + 1;
	switch (ch) {
	case 1: id = C_ALL; break;
	case 3: id = C_COPY; break;
	case 6: id = C_FIND; break;
	case 7: id = C_GOTO; break;
	case 12: id = C_LINE; break;
	case 14: id = C_NEW; break;
	case 15: id = C_OPEN; break;
	case 17: id = C_EXIT; break;
	case 19: id = C_SAVE; break;
	case 22: id = C_PASTE; break;
	case 23: id = C_WORD; break;
	case 24: id = C_CUT; break;
	case 25: id = C_REDO; break;
	case 26: id = C_UNDO; break;
	case 31: id = C_COMMENT; break;
	case MWKEY_F3: id = C_FIND_NEXT; break;
	case MWKEY_F5: id = shift ? C_STOP : C_DEBUG; break;
	case MWKEY_F6: id = C_RUN; break;
	case MWKEY_F7: id = C_BUILD; break;
	case MWKEY_F9: id = C_MARK; break;
	case MWKEY_F10: id = C_OVER; break;
	case MWKEY_F11: id = C_INTO; break;
	case MWKEY_F12: id = C_OUT; break;
	}
	if (id) {
		command(id);
		return;
	}
	tx_step++;
	notice[0] = 0;
	switch (ch) {
	case MWKEY_LEFT: case MWKEY_RIGHT: case MWKEY_UP: case MWKEY_DOWN: case MWKEY_HOME: case MWKEY_END: case MWKEY_PAGEUP: case MWKEY_PAGEDOWN:
		moves = 1;
		if (shift && !tx_marked)
			tx_ax = tx_x, tx_ay = tx_y, tx_marked = 1;
		if (!shift)
			tx_marked = 0;
		if (ch == MWKEY_LEFT) {
			if (tx_x > 0)
				tx_x--;
			else if (tx_y > 0)
				tx_y--, tx_x = (int)strlen(tx_lines[tx_y]);
		} else if (ch == MWKEY_RIGHT) {
			if (tx_x < length)
				tx_x++;
			else if (tx_y + 1 < tx_count)
				tx_y++, tx_x = 0;
		} else if (ch == MWKEY_UP || ch == MWKEY_DOWN || ch == MWKEY_PAGEUP || ch == MWKEY_PAGEDOWN) {
			tx_y += ch == MWKEY_UP ? -1 : ch == MWKEY_DOWN ? 1 : ch == MWKEY_PAGEUP ? -rows : rows;
			tx_y = tx_y < 0 ? 0 : tx_y >= tx_count ? tx_count - 1 : tx_y;
			if (tx_x > (int)strlen(tx_lines[tx_y]))
				tx_x = (int)strlen(tx_lines[tx_y]);
		} else if (ch == MWKEY_HOME) {
			/* to the first letter, and from there to the line's start */
			n = (int)strspn(tx_lines[tx_y], " ");
			tx_x = (modifiers & MWKMOD_CTRL) ? 0 : tx_x == n ? 0 : n;
			if (modifiers & MWKMOD_CTRL)
				tx_y = 0;
		} else {
			if (modifiers & MWKMOD_CTRL)
				tx_y = tx_count - 1;
			tx_x = (int)strlen(tx_lines[tx_y]);
		}
		break;
	case MWKEY_ENTER: case '\n': {
		/* the new line begins where this one does, and a step further in after a brace */
		char lead[200] = "\n";

		tx_delete_selection();
		n = (int)strspn(tx_lines[tx_y], " ");
		n = n > tx_x ? tx_x : n > 150 ? 150 : n;
		memset(lead + 1, ' ', (size_t)n);
		if (tx_x > 0 && tx_lines[tx_y][tx_x - 1] == '{')
			memset(lead + 1 + n, ' ', TX_TAB), n += TX_TAB;
		tx_insert(tx_x, tx_y, lead, n + 1);
		break;
	}
	case MWKEY_BACKSPACE: case 127:
		if (tx_delete_selection())
			break;
		if (tx_x > 0) {
			/* in the spaces before a line's text, back to the stop before */
			n = (int)strspn(tx_lines[tx_y], " ") >= tx_x && tx_x % TX_TAB == 0 ? TX_TAB : 1;
			tx_delete(tx_x - n, tx_y, tx_x, tx_y);
		} else if (tx_y > 0)
			tx_delete((int)strlen(tx_lines[tx_y - 1]), tx_y - 1, 0, tx_y);
		break;
	case MWKEY_DELETE:
		if (tx_delete_selection())
			break;
		if (tx_x < length)
			tx_delete(tx_x, tx_y, tx_x + 1, tx_y);
		else if (tx_y + 1 < tx_count)
			tx_delete(tx_x, tx_y, 0, tx_y + 1);
		break;
	case MWKEY_TAB:
		if (tx_marked && tx_ay != tx_y) {
			indent(shift ? -TX_TAB : TX_TAB);
			break;
		}
		if (shift) {
			indent(-TX_TAB);
			break;
		}
		tx_delete_selection();
		tx_insert(tx_x, tx_y, "\t", 1);
		break;
	case MWKEY_ESCAPE:
		tx_marked = 0;
		break;
	default:
		if (ch < 32 || ch > 126)
			return;
		tx_delete_selection();
		/* a closing brace alone on its line goes back a step */
		if (ch == '}' && tx_x >= TX_TAB && (int)strspn(tx_lines[tx_y], " ") == length && tx_x == length)
			tx_delete(tx_x - TX_TAB, tx_y, tx_x, tx_y);
		one = (char)ch;
		tx_insert(tx_x, tx_y, &one, 1);
		tx_marked = 0;
	}
	if (!moves && ch != MWKEY_ESCAPE && !(tx_marked && (ch == MWKEY_TAB || ch == '\t')))
		tx_marked = 0;
	if (follow() || had || tx_marked) {
		changed(was);
		draw_editor_from(top);
	} else
		changed(was);
	if (tx_done_count <= 1 || tx_undone_count == 0)
		draw_tools();
	draw_status();
}

/* ---- the pointer ---- */

static void
place(int px, int py, int *x, int *y)
{
	int line = top + (py - editor_y()) / ui_fh, column = left + (px - gutter - 3 + ui_fw / 2) / ui_fw;

	line = line < 0 ? 0 : line >= tx_count ? tx_count - 1 : line;
	column = column < 0 ? 0 : column > (int)strlen(tx_lines[line]) ? (int)strlen(tx_lines[line]) : column;
	*x = column, *y = line;
}

static void
pressed(GR_EVENT *event)
{
	int x = event->button.x, y = event->button.y, i, shown = (PANEL_H - TAB_H - 2) / 15;

	if (ui_wheel(event)) {
		int by = 3 * ui_wheel_sum(event);

		if (panel_on && y >= panel_y()) {
			int *first = panel == P_OUTPUT ? &out_top : panel == P_PROBLEMS ? &problem_top : &variable_top;
			int count = panel == P_OUTPUT ? out_count : panel == P_PROBLEMS ? problem_count : variable_count;

			*first += by;
			*first = *first > count - shown ? count - shown : *first;
			*first = *first < 0 ? 0 : *first;
			draw_panel();
		} else
			scroll_to(top + by);
		return;
	}
	if (!(event->button.changebuttons & GR_BUTTON_L))
		return;
	if (y < MENU_H) {
		for (i = 0; i < 6; i++)
			if (x >= menu_x[i] && x < menu_x[i + 1])
				menu_open(i, 0);
	} else if (y < editor_y()) {
		for (i = 0; i < TOOLS; i++)
			if (x >= tool_x[i] && x < tool_x[i] + tool_w[i])
				command(tools[i].id);
	} else if (y >= height - STATUS) {
		return;
	} else if (panel_on && y >= panel_y()) {
		if (y < panel_y() + TAB_H) {
			if (x < 4 + PANELS * 96)
				show_panel((x - 4) / 96);
		} else if (panel == P_PROBLEMS) {
			i = problem_top + (y - panel_y() - TAB_H - 2) / 15;
			if (i >= 0 && i < problem_count)
				go_to_line(problems[i].line);
		}
	} else if (x >= width - UI_SCROLL_W) {
		dragging = 1;
		scroll_to(ui_scroll_to(y, editor_y(), panel_y() - editor_y(), tx_count, rows));
	} else if (x < gutter) {
		/* before a line's number: its mark */
		place(gutter + 3, y, &tx_x, &tx_y);
		command(C_MARK);
		draw_editor_from(top);
	} else {
		unsigned now = milliseconds();
		int had = tx_marked, was = tx_y;

		place(x, y, &tx_x, &tx_y);
		if (now - click_at < 500 && click_x == tx_x && click_y == tx_y) {
			select_word();
			click_at = 0;
		} else {
			tx_ax = tx_x, tx_ay = tx_y, tx_marked = 1;
			selecting = 1;
			click_at = now, click_x = tx_x, click_y = tx_y;
		}
		if (had || tx_ax != tx_x)
			draw_editor_from(top);
		else
			draw_line(was), draw_line(tx_y);
		draw_status();
	}
}

/* ---- a test's hands: NXIDE_SCRIPT="mark:14 debug over over go quit" ---- */

/* The whole text as one piece, for comparing; the caller frees it. */
static char *
whole_text(void)
{
	int length;

	return tx_text(0, 0, (int)strlen(tx_lines[tx_count - 1]), tx_count - 1, &length);
}

/* What must always hold of the cursor and the selection. */
static void
sound_or_die(const char *after, int step)
{
	int bad = tx_count < 1 || tx_y < 0 || tx_y >= tx_count || tx_x < 0 || tx_x > (int)strlen(tx_lines[tx_y]) ||
		  (tx_marked && (tx_ay < 0 || tx_ay >= tx_count || tx_ax < 0 || tx_ax > (int)strlen(tx_lines[tx_ay]))) || top < 0 || left < 0;

	if (!bad)
		return;
	printf("nxide: storm: WRONG after %s at step %d: cursor %d,%d of %d lines, mark %d at %d,%d\n", after, step, tx_x, tx_y, tx_count,
	       tx_marked, tx_ax, tx_ay);
	fflush(stdout);
	exit(3);
}

/* `steps` keys and commands chosen by a fixed sequence of chances, through the same two
 * functions a hand's go through; then every change taken back must leave the text as it
 * was, and every one made again the text as the storm left it. */
static void
storm(int steps)
{
	static const int moves[] = { MWKEY_LEFT, MWKEY_RIGHT, MWKEY_UP, MWKEY_DOWN, MWKEY_HOME, MWKEY_END, MWKEY_PAGEUP, MWKEY_PAGEDOWN };
	static const int commands[] = { C_COMMENT, C_INDENT, C_UNINDENT, C_CUT, C_COPY, C_PASTE, C_LINE, C_WORD, C_UNDO, C_REDO, C_UNDO, C_FIND_NEXT };
	static const char letters[] = "abc xyz_01 (){};=+*/\"'#<>.,";
	char *before = whole_text(), *after, *now;
	unsigned chance = 12345;
	int i, taken = 0, again = 0;

	snprintf(sought, sizeof sought, "int");
	for (i = 0; i < steps; i++) {
		unsigned r;

		chance = chance * 1103515245u + 12345u;
		r = chance >> 8;
		if (r % 100 < 45)
			key(letters[r / 100 % (sizeof letters - 1)], 0);
		else if (r % 100 < 55)
			key(MWKEY_ENTER, 0);
		else if (r % 100 < 63)
			key(r / 100 % 2 ? MWKEY_BACKSPACE : MWKEY_DELETE, 0);
		else if (r % 100 < 85)
			key(moves[r / 100 % 8], r / 1000 % 3 == 0 ? MWKMOD_SHIFT : 0);
		else if (r % 100 < 88)
			key(MWKEY_TAB, r / 100 % 2 ? MWKMOD_SHIFT : 0);
		else if (r % 100 < 99)
			command(commands[r / 100 % 12]);
		else
			command(C_ALL);
		sound_or_die("a key", i);
	}
	after = whole_text();
	while (tx_done_count && taken < 100000) {
		command(C_UNDO);
		sound_or_die("undo", taken++);
	}
	now = whole_text();
	printf("nxide: storm: %d steps; %d changes taken back: the text is %s\n", steps, taken, strcmp(now, before) ? "NOT as it was" : "as it was");
	free(now);
	while (tx_undone_count && again < 100000) {
		command(C_REDO);
		sound_or_die("redo", again++);
	}
	now = whole_text();
	printf("nxide: storm: %d made again: the text is %s\n", again, strcmp(now, after) ? "NOT as the storm left it" : "as the storm left it");
	free(now), free(before), free(after);
	fflush(stdout);
}

/* A key by its name, for the script: "up", "s-down" (with Shift), "enter", "tab", "f9"... */
static void
named_key(const char *name)
{
	static const struct { const char *name; int ch; } keys[] = {
		{ "up", MWKEY_UP }, { "down", MWKEY_DOWN }, { "left", MWKEY_LEFT }, { "right", MWKEY_RIGHT }, { "home", MWKEY_HOME },
		{ "end", MWKEY_END }, { "pageup", MWKEY_PAGEUP }, { "pagedown", MWKEY_PAGEDOWN }, { "delete", MWKEY_DELETE },
		{ "backspace", MWKEY_BACKSPACE }, { "tab", MWKEY_TAB }, { "enter", MWKEY_ENTER }, { "escape", MWKEY_ESCAPE },
		{ "f3", MWKEY_F3 }, { "f5", MWKEY_F5 }, { "f6", MWKEY_F6 }, { "f7", MWKEY_F7 }, { "f9", MWKEY_F9 }, { "f10", MWKEY_F10 },
	};
	int shift = !strncmp(name, "s-", 2), control = !strncmp(name, "c-", 2), i;

	name += shift || control ? 2 : 0;
	if (control && name[0] && !name[1]) {
		key(name[0], MWKMOD_CTRL);
		return;
	}
	for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++)
		if (!strcmp(name, keys[i].name))
			key(keys[i].ch, shift ? MWKMOD_SHIFT : control ? MWKMOD_CTRL : 0);
}

static void
scripted(void)
{
	static const struct { const char *word; int id; } words[] = {
		{ "build", C_BUILD }, { "run", C_RUN }, { "debug", C_DEBUG }, { "go", C_DEBUG }, { "over", C_OVER }, { "into", C_INTO },
		{ "out", C_OUT }, { "stop", C_STOP }, { "undo", C_UNDO }, { "redo", C_REDO }, { "save", C_SAVE }, { "quit", C_EXIT },
		{ "comment", C_COMMENT }, { "all", C_ALL }, { "indent", C_INDENT }, { "unindent", C_UNINDENT }, { "new", C_NEW },
		{ "cut", C_CUT }, { "copy", C_COPY }, { "paste", C_PASTE }, { "word", C_WORD }, { "panel", C_PANEL }, { "numbers", C_NUMBERS },
		{ "checking", C_CHECKING }, { "keys", C_KEYS }, { "about", C_ABOUT }, { "unmark", C_UNMARK_ALL }, { "next", C_FIND_NEXT },
	};
	char word[64];
	int n, i;

	if (!script[0] || job.kind == J_BUILD || job.kind == J_RUN || dbg_state == DBG_RUNNING || then_do)
		return;
	n = (int)strcspn(script, " ");
	snprintf(word, sizeof word, "%.*s", n, script);
	memmove(script, script + n + (script[n] == ' '), strlen(script + n + (script[n] == ' ')) + 1);
	if (print_on)
		printf("nxide: script: %s\n", word);
	if (!strncmp(word, "mark:", 5)) {
		tx_y = atoi(word + 5) - 1;
		command(C_MARK);
	} else if (!strncmp(word, "type:", 5)) {
		for (i = 5; word[i]; i++)
			key(word[i] == '_' ? ' ' : word[i] == '|' ? MWKEY_ENTER : word[i], 0);
	} else if (!strncmp(word, "line:", 5)) {
		go_to_line(atoi(word + 5));
	} else if (!strncmp(word, "example:", 8)) {
		for (i = 0; i < example_count; i++)
			if (!strcmp(examples[i], word + 8))
				command(C_EXAMPLE + i);
		printf("nxide: opened %s: %d lines, cursor %d,%d\n", path, tx_count, tx_x, tx_y);
	} else if (!strncmp(word, "open:", 5)) {
		open_file(word + 5);
		draw_all();
		printf("nxide: opened %s: %d lines\n", path, tx_count);
	} else if (!strncmp(word, "find:", 5)) {
		snprintf(sought, sizeof sought, "%s", word + 5);
		command(C_FIND_NEXT);
		printf("nxide: find %s: cursor %d,%d\n", sought, tx_x, tx_y);
	} else if (!strncmp(word, "key:", 4)) {
		named_key(word + 4);
	} else if (!strncmp(word, "menu:", 5)) {
		menu_open(atoi(word + 5), 1);
		ui_menu_close();
	} else if (!strncmp(word, "storm:", 6)) {
		storm(atoi(word + 6));
	} else if (!strcmp(word, "where")) {
		printf("nxide: at %d,%d of %d lines, %s, %d to take back, %d to make again, %d problems\n", tx_x, tx_y, tx_count,
		       tx_modified ? "changed" : "as saved", tx_done_count, tx_undone_count, problem_count);
	} else if (!strcmp(word, "text")) {
		for (i = 0; i < tx_count; i++)
			printf("nxide: text %d: %s\n", i + 1, tx_lines[i]);
	} else if (!strcmp(word, "problems")) {
		for (i = 0; i < problem_count; i++)
			printf("nxide: problem: line %d: %s\n", problems[i].line, problems[i].text);
	} else if (!strcmp(word, "wait")) {
		usleep(2500000);
	} else
		for (i = 0; i < (int)(sizeof words / sizeof words[0]); i++)
			if (!strcmp(word, words[i].word))
				command(words[i].id);
	fflush(stdout);
}

static int
by_name(const void *a, const void *b)
{
	return strcmp(a, b);
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	DIR *share = opendir("/usr/share");
	struct dirent *entry;
	int i, x, chosen, w, h, b;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	/* the desktop's bar ignores its children's ends, and whatever it starts inherits that: a
	 * build would then end unseen (waitpid finds no child), and the IDE wait for it for ever */
	signal(SIGCHLD, SIG_DFL);
	print_on = getenv("NXIDE_PRINT") != NULL;
	if (getenv("NXIDE_SCRIPT"))
		snprintf(script, sizeof script, "%s", getenv("NXIDE_SCRIPT"));
	while (share && (entry = readdir(share)) != NULL && example_count < 12)
		if (!strncmp(entry->d_name, "example-", 8) && strlen(entry->d_name) < 40 && strstr(entry->d_name, ".c"))
			snprintf(examples[example_count++], sizeof examples[0], "%.*s", (int)strcspn(entry->d_name + 8, "."), entry->d_name + 8);
	if (share)
		closedir(share);
	qsort(examples, (size_t)example_count, sizeof examples[0], by_name);
	for (i = 0; i < example_count; i++) {
		example_items[i].label = examples[i];
		example_items[i].icon = -1;
		example_items[i].id = C_EXAMPLE + i;
	}
	for (i = 0, x = 4; i < 6; i++) {
		GrGetGCTextSize(ui_gc, (char *)menu_names[i], -1, GR_TFASCII, &w, &h, &b);
		menu_x[i] = x;
		x += w + 16;
	}
	menu_x[6] = x;
	for (i = 0, x = 4; i < TOOLS; i++) {
		GrGetGCTextSize(ui_gc, (char *)(tools[i].id == C_DEBUG ? "Go on" : tools[i].label), -1, GR_TFASCII, &w, &h, &b);
		x += tools[i].gap;
		tool_x[i] = x, tool_w[i] = w + 14;
		x += tool_w[i] + 2;
	}
	for (i = 0; i < TOOLS && print_on; i++)
		printf("nxide: key %s at %d\n", tools[i].label, tool_x[i] + tool_w[i] / 2);
	for (i = 0; i < 6 && print_on; i++)
		printf("nxide: menu %s at %d\n", menu_names[i], (menu_x[i] + menu_x[i + 1]) / 2);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "IDE", GR_ROOT_WINDOW_ID, -1, -1, width, height, WHITE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_BUTTON_DOWN |
			       GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_TIMEOUT);
	resized(width, height);
	if (argc > 1)
		open_file(argv[1]);
	else {
		for (i = 0; i < example_count && strcmp(examples[i], "hello"); i++)
			continue;
		if (i < example_count)
			open_example(i);
		else
			command(C_NEW);
	}
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		/* often while something runs beside or the text waits to be checked, seldom otherwise */
		GrGetNextEventTimeout(&event, job.kind != J_NONE || dbg_state == DBG_RUNNING || debug_fd >= 0 ? 40 : unchecked || script[0] ? 300 : 2000);
		switch (ui_menu_event(&event, &chosen)) {
		case UI_MENU_CHOSEN:
			command(chosen);
			continue;
		case UI_MENU_TOOK:
		case UI_MENU_CLOSED:
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE) {
				resized(event.update.width, event.update.height);
				draw_all();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			pressed(&event);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = selecting = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (dragging && (event.mouse.buttons & GR_BUTTON_L)) {
				scroll_to(ui_scroll_to(event.mouse.y, editor_y(), panel_y() - editor_y(), tx_count, rows));
			} else if (selecting && (event.mouse.buttons & GR_BUTTON_L)) {
				int nx, ny, from, to;

				place(event.mouse.x, event.mouse.y < panel_y() ? event.mouse.y : panel_y() - 1, &nx, &ny);
				if (nx != tx_x || ny != tx_y) {
					from = ny < tx_y ? ny : tx_y, to = ny < tx_y ? tx_y : ny;
					tx_x = nx, tx_y = ny;
					for (i = from; i <= to; i++)
						draw_line(i);
					draw_status();
				}
			} else
				dragging = selecting = 0;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			command(C_EXIT);
			break;
		}
		poll_all();
		scripted();
	}
}
