/*
 * The server's frame on a worker core (docs/quake3.md, docs/multicore.md). A frame of the game
 * is the server's (the players' moves, the bots, what everything in the level does, and what
 * each client is told of it) and then the client's (the picture). With bots the first is most
 * of a core by itself. Here it runs on a worker while this core draws, and the two go at
 * their own pace: the client draws from what the server last told it, as it does in a game
 * over a network, which is what the two already are to each other.
 *
 * What the server's frame does on a worker it does to the server's own data, which this core
 * leaves alone meanwhile: a console command of the server's or the game's waits for the frame
 * to end. What it shares with the client it does not touch from there. The packets it is
 * sent wait for its next frame and the ones it sends go when its frame is taken; console
 * lines, commands and cvar changes are kept and done by this core afterwards; an error ends
 * the job and is raised here; and the engine has two of what two cores would both write
 * (quake3.patch: a command's words, the marks of a trace, the box a thing is clipped as, the
 * buffers va() returns, a zone for the server's allocations). The build gives every variable
 * its own 16 bytes, for two cores must not store to the same 16.
 *
 * It is on this core while a level loads, while any client is not yet in the game, and when
 * QUAKE3_SERVER=inline says so or the machine has no worker.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "server/server.h"
#include "mcw.h"
#include "se_q3.h"

enum { MODE_UNSET = -1, MODE_INLINE, MODE_WORKER };
enum { KEPT_PRINT, KEPT_COMMAND, KEPT_CVAR, KEPT_CVAR_FORCED, KEPT_RELIABLE };
#define BOX_BYTES	0x10000		/* of packets waiting for the server's frame, and of those it sent */
#define MOST_MSEC	200		/* of the game's time a frame of the server's catches up, at most */
#define STACK_BYTES	0x80000		/* the server's frame is deeper than a worker's own stack */

unsigned	se_main_stack;		/* where the first core's stack is (q_shared.h, SE_OnWorker) */
unsigned	se_server_jobs, se_server_cycles, se_server_passes;	/* for the q3stat: lines */

typedef struct { unsigned used, unused[3]; unsigned char data[BOX_BYTES]; } box_t;
static box_t	inbox __attribute__ ((aligned (16)));		/* this core's: packets for the server */
static box_t	job_in __attribute__ ((aligned (16)));		/* the job's: those it has to read */
static box_t	outbox __attribute__ ((aligned (16)));		/* the job's: packets the server sent */
/* what else a worker writes while it has a job: the server's, and the client's game's for each of its two lists (client_shaderemu.c) */
static struct {
	unsigned	used, lost, failed;
	int		code;
	char		error[1024];
	char		kept[8192];	/* a kind, then text ending in 0 (a cvar: its name, 0, its value, 0) */
} works[3] __attribute__ ((aligned (16)));
static char	job_stack[STACK_BYTES] __attribute__ ((aligned (16)));
se_abort_t	se_worker_abort[2];
static int	mode = MODE_UNSET, out, pending, workers = -1;

/* fn(arg) on another stack */
void CL_AddReliableCommand (const char *cmd, qboolean isDisconnectCmd);
uint32_t se_call_on_stack (uint32_t (*fn) (uint32_t), uint32_t arg, void *stack_top);
__asm__ (".text\n.globl se_call_on_stack\n.type se_call_on_stack, @function\nse_call_on_stack:\n"
	"	addi a2, a2, -16\n	sw ra, 12(a2)\n	sw sp, 8(a2)\n	mv sp, a2\n	mv t0, a0\n	mv a0, a1\n	jalr t0\n"
	"	lw ra, 12(sp)\n	lw sp, 8(sp)\n	ret\n");

/* The program's worker cores: 1 is the server's, 2 the client's game's. How many there are. */
int SE_Workers (void)
{
	if (workers < 0) {
		workers = mcw_open (2);
		if (workers > 0)
			atexit (mcw_close);
	}
	return workers;
}

static void setup (void)
{
	const char	*how;
	unsigned	i;

	if (mode != MODE_UNSET)
		return;
	how = getenv ("QUAKE3_SERVER");	/* (asked once: this is called every frame) */
	mode = MODE_INLINE;
	if (!(how && !strcmp (how, "inline")) && SE_Workers () > 0) {
		mode = MODE_WORKER;
		/* (the stack's top is there before the worker steps on it) */
		for (i = 4096; i <= 0x20000; i += 4096)
			job_stack[STACK_BYTES - i] = 0;
	}
	printf ("quake3: the server's frame is on %s\n", mode == MODE_WORKER ? "a worker core" : "this core");
}

/* ---- what the worker keeps for this core ---- */

static void keep (int kind, const char *a, const char *b)
{
	unsigned	la = strlen (a) + 1, lb = b ? strlen (b) + 1 : 0;
	int		w = SE_OnWorker () == 2 ? se_client_list : 0;

	if (works[w].used + 1 + la + lb > sizeof works[w].kept) {
		works[w].lost++;
		return;
	}
	works[w].kept[works[w].used++] = (char)kind;
	memcpy (works[w].kept + works[w].used, a, la);
	works[w].used += la;
	if (b) {
		memcpy (works[w].kept + works[w].used, b, lb);
		works[w].used += lb;
	}
}

void SE_ServerKeepPrint (const char *text) { keep (KEPT_PRINT, text, NULL); }
void SE_ServerKeepCommand (const char *text) { keep (KEPT_COMMAND, text, NULL); }
void SE_ServerKeepCvar (const char *name, const char *value, int force) { keep (force ? KEPT_CVAR_FORCED : KEPT_CVAR, name, value ? value : ""); }
void SE_ClientKeepReliable (const char *text) { keep (KEPT_RELIABLE, text, NULL); }

void SE_ServerWorkerError (int code, const char *text)
{
	int	w = SE_OnWorker () == 2 ? se_client_list : 0;

	Q_strncpyz (works[w].error, text, sizeof works[w].error);
	works[w].code = code;
	works[w].failed = 1;
	longjmp (se_worker_abort[w != 0].at, 1);
}

/* What a worker kept (0: the server's; 1, 2: the client's game's, by its list) is done by this core; 1 when it ended in an error (SE_WorkerRaise). */
int SE_WorkerKept (int w)
{
	unsigned	at;

	for (at = 0; at < works[w].used; ) {
		int	kind = works[w].kept[at++];
		char	*a = works[w].kept + at;

		at += strlen (a) + 1;
		if (kind == KEPT_PRINT) {
			Com_Printf ("%s", a);
		} else if (kind == KEPT_COMMAND) {
			Cbuf_AddText (a);
		} else if (kind == KEPT_RELIABLE) {
			CL_AddReliableCommand (a, qfalse);
		} else {
			Cvar_Set2 (a, works[w].kept + at, kind == KEPT_CVAR_FORCED);
			at += strlen (works[w].kept + at) + 1;
		}
	}
	works[w].used = 0;
	if (works[w].lost) {
		Com_Printf ("(%u of the %s lines lost)\n", works[w].lost, w ? "client's game's" : "server's");
		works[w].lost = 0;
	}
	at = works[w].failed;
	works[w].failed = 0;
	return (int)at;
}

void SE_WorkerRaise (int w)
{
	Com_Error (works[w].code, "%s", works[w].error);
}

/* ---- packets ---- */

typedef struct { netadr_t adr; int length, unused; } packet_t;

static int box_add (box_t *box, netadr_t adr, int length, const void *data)
{
	unsigned	bytes = sizeof (packet_t) + (((unsigned)length + 15) & ~15u);
	packet_t	*p = (packet_t *)(box->data + box->used);

	if (length < 0 || box->used + bytes > BOX_BYTES)
		return 0;
	p->adr = adr;
	p->length = length;
	memcpy (p + 1, data, length);
	box->used += bytes;
	return 1;
}

/* A packet for the server: kept for its next frame (1), unless the server is always this core's (0). */
int SE_ServerInbox (netadr_t from, msg_t *msg)
{
	setup ();
	if (mode != MODE_WORKER)
		return 0;
	box_add (&inbox, from, msg->cursize, msg->data);	/* (one there is no room for is lost, as on a network) */
	return 1;
}

/* One the server sends from the worker: it goes when this core takes the frame. */
void SE_ServerOutbox (netadr_t to, int length, const void *data)
{
	box_add (&outbox, to, length, data);
}

/* The server reads the packets of a box. */
static void box_read (box_t *box)
{
	unsigned	at;

	for (at = 0; at < box->used; ) {
		packet_t	*p = (packet_t *)(box->data + at);
		msg_t		msg;

		at += sizeof (packet_t) + (((unsigned)p->length + 15) & ~15u);
		MSG_Init (&msg, (byte *)(p + 1), p->length);
		msg.cursize = p->length;
		if (com_sv_running->integer)
			SV_PacketEvent (p->adr, &msg);
	}
	box->used = 0;
}

/* ---- the job ---- */

static uint32_t job_body (uint32_t msec)
{
	if (!setjmp (se_worker_abort[0].at)) {
		int	left = (int)msec, step = sv_fps->integer > 0 ? 1000 / sv_fps->integer : 100;

		box_read (&job_in);
		/* (a frame of the game at a time: the bots look where they are before each, not once for several) */
		do {
			SV_Frame (left < step ? left : step);
			left -= step;
		} while (left > 0 && com_sv_running->integer);
	}
	return 0;
}

static uint32_t server_job (uint32_t msec, uint32_t unused)
{
	unsigned	began = SE_Cycles ();

	se_call_on_stack (job_body, msec, job_stack + STACK_BYTES);
	return SE_Cycles () - began;
}

/* A frame the worker had has ended: what it kept and sent is done here. */
static void finish (void)
{
	unsigned	at;
	int		failed;

	out = 0;
	se_server_cycles += mcw_result (1);
	se_server_jobs++;
	failed = SE_WorkerKept (0);
	for (at = 0; at < outbox.used; ) {
		packet_t *p = (packet_t *)(outbox.data + at);

		at += sizeof (packet_t) + (((unsigned)p->length + 15) & ~15u);
		NET_SendPacket (NS_SERVER, p->length, p + 1, p->adr);
	}
	outbox.used = 0;
	if (failed)
		SE_WorkerRaise (0);
}

/* Before anything on this core that reads or changes the server's data. */
void SE_ServerSettle (void)
{
	if (!out)
		return;
	se_server_passes += mcw_wait (1);
	finish ();
}

/* Called a few times in a frame of the client's: a worker stopped at a page nobody has touched is helped on. */
void SE_ServerPoll (void)
{
	if (out)
		mcw_done (1);
	SE_ClientPoll ();
}

/* Whether the server's next frame is this core's: a level loading, somebody joining or leaving. */
static int slow (void)
{
	int	i;

	if (sv.state != SS_GAME || sv.restarting || !SE_ClientPlaying ())
		return 1;
	for (i = 0; i < sv_maxclients->integer; i++)
		if (svs.clients[i].state != CS_FREE && svs.clients[i].state != CS_ACTIVE)
			return 1;
	return 0;
}

/* In place of SV_Frame(): so much time has passed. */
void SE_ServerFrame (int msec)
{
	int	frame_msec;

	setup ();
	if (!com_sv_running->integer || sv_killserver->integer) {
		SE_ServerSettle ();
		SV_Frame (msec);
		return;
	}
	if (mode != MODE_WORKER) {
		/* (a frame of the game at a time here too, when this core's frame was several long) */
		int	step = sv_fps->integer > 0 ? 1000 / sv_fps->integer : 100;

		if (msec > MOST_MSEC)
			msec = MOST_MSEC;
		do {
			SV_Frame (msec < step ? msec : step);
			msec -= step;
		} while (msec > 0 && com_sv_running->integer);
		return;
	}
	pending += msec;
	if (out) {
		if (!mcw_done (1))
			return;
		finish ();
		if (!com_sv_running->integer)
			return;
	}
	if (slow () || (sv_paused->integer && cl_paused->integer)) {
		int	step = sv_fps->integer > 0 ? 1000 / sv_fps->integer : 100;

		box_read (&inbox);
		if (pending > MOST_MSEC)
			pending = MOST_MSEC;
		do {
			SV_Frame (pending < step ? pending : step);
			pending -= step;
		} while (pending > 0 && com_sv_running->integer);
		pending = 0;
		return;
	}
	/* the infostrings, if a variable of them has changed: this core sets those flags too */
	if (cvar_modifiedFlags & CVAR_SERVERINFO) {
		SV_SetConfigstring (CS_SERVERINFO, Cvar_InfoString (CVAR_SERVERINFO));
		cvar_modifiedFlags &= ~CVAR_SERVERINFO;
	}
	if (cvar_modifiedFlags & CVAR_SYSTEMINFO) {
		SV_SetConfigstring (CS_SYSTEMINFO, Cvar_InfoString_Big (CVAR_SYSTEMINFO));
		cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;
	}
	/* not before a frame of the game is due: a job that only reads the client's moves is a core woken for little */
	frame_msec = sv_fps->integer > 0 ? (int)(1000 / sv_fps->integer * com_timescale->value) : 100;
	if (sv.timeResidual + pending < frame_msec && inbox.used < BOX_BYTES / 2)
		return;
	memcpy (job_in.data, inbox.data, inbox.used);
	job_in.used = inbox.used;
	inbox.used = 0;
	mcw_post (1, server_job, (uint32_t)(pending > MOST_MSEC ? MOST_MSEC : pending), 0);
	pending = 0;
	out = 1;
}
