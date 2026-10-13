/*
 * The worker cores for a Linux program (docs/multicore.md): threads of the program that the
 * kernel does not know of. A job is a function of the program and two words for it; a worker
 * runs it on a stack of its own, in the program's memory as the program has it.
 *
 * What a job may do: compute, read and write the program's memory, and make system calls,
 * which this core makes for it when it next looks (mcw_wait(), mcw_done()): two passes of the
 * machine each, so not many. What it may not: use what the C library keeps one of for the
 * program while this core uses it too (malloc, stdio: one side at a time), or leave the
 * function any other way than by returning.
 *
 * What both sides have to keep to: a core sees another's stores one pass of the machine
 * later, and two cores must not store to the same 16 bytes in one pass (addresses that differ
 * only in their last four bits). Give a job memory of its own to write: rows of a buffer
 * whose width is a multiple of 16, allocations from mcw_alloc(), its own stack.
 *
 * The kernel shares the cores out: mcw_open() gives the program's share of them, which is
 * as many as it wants while there are enough for everybody, and may be none, and is none
 * where the machine has none. A program does the work itself then. The share changes as
 * other programs come and go: a worker of this program may lose its core between two jobs
 * (never during one), and have one again later. Nothing changes for the program: the
 * workers are numbered as before, and a job posted to one that has no core is run by the
 * core that posts it, there and then. mcw_bound() says which it would be.
 */
#ifndef MCW_H
#define MCW_H

#include <stdint.h>
#include "mc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t (*mcw_fn)(uint32_t a0, uint32_t a1);

/* For a job, on its worker: whether the kernel wants worker k's core back when the job ends (a
 * job that could go on for long ends early then, and is posted again for the rest). */
int mcw_recalled(int k);

/* How many workers the machine has (63 of 64 cores): all alike, each with a write cache of
 * 1.5 KB of new stores a pass (docs/multicore.md). */
int mcw_most(void);
/* Starts up to `limit` workers; how many there are. */
int mcw_open(int limit);
/* Whether worker k has a core now (a job posted to it will not be run by this one). */
int mcw_bound(int k);
/*
 * For a program that can do without (a monitor that draws with them): called before
 * mcw_open(), its workers are only lent: it has what the programs that keep theirs leave.
 * mcw_asked() says that a core of this program has been taken back or asked for.
 */
void mcw_lend(void);
int mcw_asked(void);
int mcw_count(void);
/* Parks them and gives them up. A program calls it before it leaves (atexit will do). */
void mcw_close(void);
/* Memory that begins and ends on 16 bytes, for a core of its own to write; never freed. */
void *mcw_alloc(unsigned bytes);
/*
 * Makes memory's pages be there and writable now, on this core. A worker stops at every page
 * the program has not touched (or not written) yet until this core next looks, which is two
 * passes when it waits in mcw_wait() and may be a whole frame of a program that only asks
 * mcw_done() now and then: touch what a job will use first. A program's variables that start
 * as zeros are such pages (from __DATA_BEGIN__ to _end is all of its data).
 */
void mcw_touch(void *memory, unsigned bytes);
/* A job for worker k (1 to mcw_count()): it calls fn(a0, a1). */
void mcw_post(int k, mcw_fn fn, uint32_t a0, uint32_t a1);
/* Whether worker k has finished what it was posted last; what the function returned. */
int mcw_done(int k);
uint32_t mcw_result(int k);
/*
 * Ends this core's passes until it has; how many passes that took. A worker that stops at a
 * page the program has not touched yet is helped on from here (and from mcw_done()): the
 * page is touched, which the kernel answers, and the worker goes on. Any other stop ends the
 * program with a line saying where.
 */
int mcw_wait(int k);
/*
 * For the first core when it has nothing to do until the next pass (its workers have the work,
 * or it waits for the clock): the kernel is told, which runs another program or ends the pass
 * as its idle loop does. With no workers open it is the pause hint. Not for a job.
 */
void mcw_rest(void);
/* Whether the code that asks is running on a worker. */
int mcw_on_worker(void);

#ifdef __cplusplus
}
#endif
#endif
