/*
 * The client's game on a worker core (docs/quake3.md, "The client's game a frame ahead"): it
 * writes what it would have drawn into a list, and this core makes the GPU's commands of the
 * list a frame later. QUAKE3_CLIENT=inline, or no second worker: this core's, as it was.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "client/client.h"
#include "mcwx.h"
#include "se_q3.h"

enum { MODE_UNSET = -1, MODE_INLINE, MODE_WORKER };
#define WORKER		2
#define STACK_BYTES	0x80000
#define FIRST_FRAMES	4		/* of a game this core makes itself before the worker has them */
#define LIST(frame)	(1 + (frame) % SE_LISTS)	/* which of the lists a frame is written to */

unsigned	se_client_stack;	/* its stack's top (q_shared.h, SE_OnWorker) */
unsigned	se_client_jobs, se_client_cycles, se_client_passes;	/* for the q3stat: lines */
int		se_client_list;		/* the list of the frame the worker is at (server_shaderemu.c keeps by it) */

/* what a frame is given, by its list: this core's, written before the frame is asked for */
static struct { int serverTime, stereo, demo, cmdNumber, snapNumber, snapTime, unused[2]; } given[SE_LISTS] __attribute__ ((aligned (16)));
/* how many frames have been asked for: this core's. The worker goes on to the next without waiting to be told */
static struct { unsigned frames, unused[3]; } asked __attribute__ ((aligned (16)));
/* how many it has made, what the last cost, and a frame's two words for the next move: the worker's */
static struct { unsigned frames, cycles, jobs, unused; } made __attribute__ ((aligned (16)));
static struct { int has, value; float scale; int unused; } answer[SE_LISTS] __attribute__ ((aligned (16)));
static char	stack[STACK_BYTES] __attribute__ ((aligned (16)));
static int	mode = MODE_UNSET, first_frames;
static unsigned	taken;			/* frames this core has taken */
static mcw_handle	job;		/* the worker's call (mcwx.h), while it is out */

intptr_t cgame_vmMain (int, int, int, int, int, int, int, int, int, int, int, int, int);
uint32_t se_call_on_stack (uint32_t (*fn) (uint32_t), uint32_t arg, void *stack_top);

static void setup (void)
{
	const char	*how;
	unsigned	i;

	if (mode != MODE_UNSET)
		return;
	how = getenv ("QUAKE3_CLIENT");
	mode = MODE_INLINE;
	if (!(how && !strcmp (how, "inline")) && SE_Workers () >= WORKER) {
		mode = MODE_WORKER;
		se_client_stack = (unsigned)(uintptr_t)(stack + STACK_BYTES);
		for (i = 4096; i <= 0x20000; i += 4096)
			stack[STACK_BYTES - i] = 0;
		SE_SceneTouch (mcw_touch);
	}
	printf ("quake3: the client's game is on %s\n", mode == MODE_WORKER ? "a worker core, a frame ahead" : "this core");
}

/* ---- on the worker ---- */

static uint32_t frame_body (uint32_t list)
{
	if (!setjmp (se_worker_abort[1].at)) {
		se_client_list = (int)list;
		answer[list - 1].has = 0;
		SE_SceneRecord ((int)list);
		SE_SoundRecord ((int)list);
		cgame_vmMain (CG_DRAW_ACTIVE_FRAME, given[list - 1].serverTime, given[list - 1].stereo, given[list - 1].demo, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	}
	return 0;
}

/* The frames asked for, one after another: the next may have been asked for while the last was made. */
static uint32_t client_job (uint32_t a, uint32_t b)
{
	made.jobs++;
	while (made.frames < asked.frames) {
		unsigned	began = SE_Cycles ();

		se_call_on_stack (frame_body, LIST (made.frames + 1), stack + STACK_BYTES);
		made.cycles = SE_Cycles () - began;
		made.frames++;
	}
	return 0;
}

int SE_ClientPinned (int *cmdNumber, int *snapshotNumber, int *serverTime)
{
	if (SE_OnWorker () != WORKER)
		return 0;
	if (cmdNumber)
		*cmdNumber = given[se_client_list - 1].cmdNumber;
	if (snapshotNumber)
		*snapshotNumber = given[se_client_list - 1].snapNumber;
	if (serverTime)
		*serverTime = given[se_client_list - 1].snapTime;
	return 1;
}

int SE_ClientUserCmdValue (int value, float scale)
{
	if (SE_OnWorker () != WORKER)
		return 0;
	answer[se_client_list - 1].has = 1, answer[se_client_list - 1].value = value, answer[se_client_list - 1].scale = scale;
	return 1;
}

/* ---- on this core ---- */

/* The worker is given the frames asked for, if it is not at them already. */
static void start (void)
{
	if (job && mcw_ready (job))
		mcw_join (job), job = 0;
	if (!job && made.frames < asked.frames)
		job = mcw_call (client_job, 0, 0);
}

/* One more frame is asked for: its list was last drawn from two frames ago. */
static void ask (int serverTime, int stereo, int demo)
{
	int	list = LIST (asked.frames + 1);

	given[list - 1].serverTime = serverTime, given[list - 1].stereo = stereo, given[list - 1].demo = demo;
	given[list - 1].cmdNumber = cl.cmdNumber, given[list - 1].snapNumber = cl.snap.messageNum, given[list - 1].snapTime = cl.snap.serverTime;
	asked.frames++;
	start ();
}

/* Until the worker has made so many frames. */
static void wait (unsigned frames)
{
	while (made.frames < frames) {
		start ();
		if (made.frames >= frames)
			break;	/* (no worker core: this one made them, there and then) */
		mcw_rest ();
		se_client_passes++;
		SE_ServerPoll ();	/* (the server's frame is not kept waiting for this core's) */
	}
}

/* The next frame is taken: what the worker kept of it is done here. Which list its picture is; 0 when it ended in an error. */
static int take (void)
{
	int	list = LIST (taken + 1), failed;

	wait (taken + 1);
	taken++;
	se_client_cycles += made.cycles;
	se_client_jobs++;
	failed = SE_WorkerKept (list);
	SE_SoundReplay (list);
	if (answer[list - 1].has)
		cl.cgameUserCmdValue = answer[list - 1].value, cl.cgameSensitivity = answer[list - 1].scale;
	return failed ? 0 : list;
}

void SE_ClientSettle (void)
{
	wait (asked.frames);
}

void SE_ClientPoll (void)
{
	if (job)
		mcw_ready (job);	/* (a page it stopped at is brought) */
}

void SE_ClientReset (void)
{
	while (taken < asked.frames)
		take ();
	first_frames = 0;
}

/* Whether the next frame of the client's game may be the worker's. */
static int ahead (void)
{
	return mode == MODE_WORKER && clc.state == CA_ACTIVE && !clc.demoplaying && !se_wanted && cl.snap.valid &&
		first_frames >= FIRST_FRAMES && clc.lastExecutedServerCommand >= cl.snap.serverCommandNum;
}

void SE_ClientFrame (int serverTime, int stereo, int demo)
{
	int	drawn;

	setup ();
	if (!ahead () || taken == asked.frames) {
		int	first = ahead ();

		/* this core's, as it always was: frames the worker has are taken and not shown */
		while (taken < asked.frames)
			if (!take ())
				SE_WorkerRaise (LIST (taken));
		first_frames = clc.state == CA_ACTIVE ? first_frames + 1 : 0;
		SE_SceneSum (1);
		VM_Call (cgvm, CG_DRAW_ACTIVE_FRAME, serverTime, stereo, demo);
		SE_SceneSum (0);
		/* (the first of the worker's: it makes this frame again, for this core to show next) */
		if (first)
			ask (serverTime, stereo, demo);
		return;
	}
	/* the next is asked for behind the one the worker is at, which is waited for and shown */
	if (asked.frames - taken < 2)
		ask (serverTime, stereo, demo);
	drawn = take ();
	if (!drawn)
		SE_WorkerRaise (LIST (taken));
	SE_SceneSum (1);
	SE_SceneReplay (drawn);
	SE_SceneSum (0);
}
