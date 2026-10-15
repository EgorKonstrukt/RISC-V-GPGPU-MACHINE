/* mcw.h and mcwx.h on the build computer, for libmedia's tests: a worker is a thread
 * (MCW_HOST_WORKERS of them at most, 8 by default). What the machine's cores keep to (stores
 * seen a pass later, 16 bytes a core) is not checked here: rvc_cpu does that. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "mcwx.h"

#define MOST 64
static int count;
static __thread int self;
static struct { pthread_t t; int running; mcw_fn fn; uint32_t a0, a1, result; volatile int done; } w[MOST];

int mcw_count(void) { return count; }
int mcw_most(void) { return MOST - 1; }
int mcw_self(void) { return self; }
int mcw_on_worker(void) { return self != 0; }
int mcw_bound(int k) { return k >= 1 && k <= count; }
int mcw_slot(void) { return count ? 0 : -1; }
void mcw_lend(void) {}
int mcw_asked(void) { return 0; }
int mcw_recalled(int k) { (void)k; return 0; }
void mcw_rest(void) { sched_yield(); }
void mcw_touch(void *m, unsigned b) { (void)m, (void)b; }
void mcw_touch_code(const void *c, unsigned b) { (void)c, (void)b; }
void *mcw_map(uint32_t o, uint32_t b) { (void)o, (void)b; return 0; }
void *mcw_alloc(unsigned bytes) { void *m = NULL; posix_memalign(&m, 16, (bytes + 15) & ~15u); memset(m, 0, bytes); return m; }

int
mcw_open(int limit)
{
	const char *most = getenv("MCW_HOST_WORKERS");
	int n = most ? atoi(most) : 8;

	if (count)
		return count;
	count = limit < n ? limit : n;
	if (count < 0)
		count = 0;
	return count;
}

static void *
run(void *arg)
{
	int k = (int)(intptr_t)arg;

	self = k;
	w[k].result = w[k].fn(w[k].a0, w[k].a1);
	w[k].done = 1;
	return NULL;
}

int
mcw_wait(int k)
{
	if (w[k].running) {
		pthread_join(w[k].t, NULL);
		w[k].running = 0;
	}
	return 0;
}

void
mcw_post(int k, mcw_fn fn, uint32_t a0, uint32_t a1)
{
	mcw_wait(k);
	w[k].fn = fn, w[k].a0 = a0, w[k].a1 = a1, w[k].done = 0;
	if (k < 1 || k > count) {
		w[k].result = fn(a0, a1);
		w[k].done = 1;
		return;
	}
	w[k].running = 1;
	pthread_create(&w[k].t, NULL, run, (void *)(intptr_t)k);
}

int mcw_done(int k) { return w[k].done; }
uint32_t mcw_result(int k) { return w[k].result; }

void
mcw_close(void)
{
	int k;

	for (k = 1; k <= count; k++)
		mcw_wait(k);
	count = 0;
}

/* ---- the parallel loop ---- */

struct round { uint32_t n, grain, shares; mcw_body fn; void *ctx; };
static struct round rounds[MOST];

static void
share(const struct round *r, uint32_t s)
{
	uint64_t from;

	for (from = (uint64_t)s * r->grain; from < r->n; from += (uint64_t)r->grain * r->shares)
		r->fn(r->ctx, (uint32_t)from, (uint32_t)(from + r->grain < r->n ? from + r->grain : r->n));
}

static uint32_t
share_job(uint32_t s, uint32_t unused)
{
	(void)unused;
	share(&rounds[s], s);
	return 0;
}

static uint32_t gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

void
mcw_for_opt(uint32_t n, mcw_body fn, void *ctx, const mcw_for_opts *o)
{
	int workers = o && o->workers > 0 && o->workers < count ? o->workers : count, k, first = !(o && o->first_idle) || !workers;
	uint32_t shares = (uint32_t)workers + (first ? 1 : 0), grain;
	struct round r;

	grain = o && o->grain ? o->grain : (n + shares * 4 - 1) / (shares * 4);
	if (o && o->out_bytes) {
		uint32_t unit = 16 / gcd(o->out_bytes, 16);

		grain = (grain + unit - 1) / unit * unit;
	}
	if (!grain)
		grain = 1;
	r.n = n, r.grain = grain, r.shares = shares, r.fn = fn, r.ctx = ctx;
	for (k = 1; k <= workers; k++) {
		rounds[first ? k : k - 1] = r;
		mcw_post(k, share_job, (uint32_t)(first ? k : k - 1), 0);
	}
	if (first) {
		rounds[0] = r;
		share(&r, 0);
	}
	for (k = 1; k <= workers; k++)
		mcw_wait(k);
}

void mcw_for(uint32_t n, mcw_body fn, void *ctx) { mcw_for_opt(n, fn, ctx, NULL); }
/* (a round begun is made at once here) */
void mcw_for_begin(uint32_t n, mcw_body fn, void *ctx, const mcw_for_opts *o) { mcw_for_opt(n, fn, ctx, o); }
int mcw_for_ready(void) { return 1; }
void mcw_for_wait(void) {}
void mcw_for_linger(int passes) { (void)passes; }
void mcw_for_end(void) {}

/* ---- scratch ---- */

static __thread struct { uint8_t *at; uint32_t used, room; void *blocks[4096]; int nblocks; } scratch;

void *
mcw_scratch(uint32_t bytes)
{
	void *m;

	bytes = (bytes + 15) & ~15u;
	if (scratch.nblocks < 4096) {
		posix_memalign(&m, 16, bytes ? bytes : 16);
		scratch.blocks[scratch.nblocks++] = m;
		return m;
	}
	abort();
}

void
mcw_scratch_reset(void)
{
	while (scratch.nblocks)
		free(scratch.blocks[--scratch.nblocks]);
}

/* ---- calls ---- */

/* mcw_call() is made at once; mcw_try_call() is a thread of its own (handles 1 to CALLS), as a
 * worker of the machine's is: as many out as there are workers, each with a worker's number. */
#define CALLS 16
static struct { pthread_t t; mcw_fn fn; uint32_t a0, a1, result; int out; volatile int done; } calls[CALLS];

static void *
call_run(void *arg)
{
	int h = (int)(intptr_t)arg;

	self = h + 1;
	calls[h].result = calls[h].fn(calls[h].a0, calls[h].a1);
	__atomic_store_n(&calls[h].done, 1, __ATOMIC_RELEASE);
	return NULL;
}

mcw_handle mcw_call(mcw_fn fn, uint32_t a0, uint32_t a1) { return (mcw_handle)(fn(a0, a1) | 0x80000000u); }

mcw_handle
mcw_try_call(mcw_fn fn, uint32_t a0, uint32_t a1)
{
	int h;

	if (!count)
		return 0;
	for (h = 0; h < CALLS && h < count && calls[h].out; h++)
		;
	if (h == CALLS || h >= count)
		return 0;
	calls[h].fn = fn, calls[h].a0 = a0, calls[h].a1 = a1, calls[h].done = 0, calls[h].out = 1;
	pthread_create(&calls[h].t, NULL, call_run, (void *)(intptr_t)h);
	return h + 1;
}

int
mcw_ready(mcw_handle h)
{
	if (h < 1 || h > CALLS)
		return 1;
	return __atomic_load_n(&calls[h - 1].done, __ATOMIC_ACQUIRE);
}

uint32_t
mcw_join(mcw_handle h)
{
	if (h < 1 || h > CALLS)
		return (uint32_t)h & 0x7fffffffu;
	pthread_join(calls[h - 1].t, NULL);
	calls[h - 1].out = 0;
	return calls[h - 1].result;
}
