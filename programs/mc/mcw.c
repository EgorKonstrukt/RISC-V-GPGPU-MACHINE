/*
 * The worker cores for a Linux program: mcw.h. Nothing of the C library is used here (the
 * system calls are made directly), so that a program with a runtime of its own can link it.
 */
#include "mcw.h"

#define STACK_BYTES 0x10000u
#define STACK_TOUCHED 0x3000u	/* of a stack's top, here before its worker: the rest when it is stepped on (nxphys steps on three) */
#define CHUNK_BYTES 0x40000u

static uint8_t *page;		/* the mailboxes */
static uint8_t *stacks;
static int device = -1;		/* kept open: the kernel parks the workers when it closes */
static int count, stack_count;
/* Worker k of this program (1 to count) is that core; 0: it has none now (the kernel shares
 * the cores out, mc.h), and what is posted to it is run by the core that posts it. */
static int core_of[MC_MAX_CORES];
static int starting[MC_MAX_CORES];	/* the core being started for worker k, not there yet */
static uint32_t seq[MC_MAX_CORES];
static uint32_t result_of[MC_MAX_CORES];	/* worker k's last job's answer, */
static uint8_t settled[MC_MAX_CORES];	/* which is here when this is set: nothing of k's is out */
static uint32_t root, own_tp, offer_seen, my_tag;
static int lent, lost, share;
static uint8_t *chunk;
static uint32_t chunk_left;
unsigned mcw_faults;		/* pages touched for a worker so far */
unsigned mcw_calls;		/* and system calls made for one */

static mc_job *job_of(int k) { return (mc_job *)(page + MC_JOB_AT(core_of[k])); }
static mc_answer *answer_of(int k) { return (mc_answer *)(page + MC_ANSWER_AT(core_of[k])); }
/* (a core that is being started may stop at a page too: it is served as one that runs) */
static int core_now(int k) { return core_of[k] ? core_of[k] : starting[k]; }
static mc_fault *fault_of(int k) { return (mc_fault *)(page + MC_FAULT_AT(core_now(k))); }
static volatile uint32_t *resume_of(int k) { return (volatile uint32_t *)(page + MC_RESUME_AT(core_now(k))); }
static int writing(int core);
static void written(void);
/* Worker k's answer is taken: its mailbox says so (MC_TAKEN_AT), and only then is the core one
 * the kernel may take back. */
static void took(int k)
{
	settled[k] = 1;
	if (core_of[k]) *(volatile uint32_t *)(page + MC_TAKEN_AT(core_of[k])) = seq[k];
}

static long call6(long number, long a, long b, long c, long d, long e, long f)
{
	register long a7 __asm__("a7") = number;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;
	__asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
	return a0;
}
static void *map(long bytes, int fd, long offset)
{
	/* mmap2: shared and of the file, or private and of nothing; the offset is in pages */
	long r = call6(222, 0, bytes, 3, fd >= 0 ? 1 : 0x22, fd, offset >> 12);
	return r < 0 && r > -4096 ? 0 : (void *)r;
}

static void say(const char *text, uint32_t value)
{
	char line[96];
	int n = 0, i;

	while (*text && n < 80) line[n++] = *text++;
	for (i = 28; i >= 0; i -= 4) line[n++] = "0123456789abcdef"[(value >> i) & 15];
	line[n++] = '\n';
	call6(64, 2, (long)line, n, 0, 0, 0);
}

/*
 * A worker starts here, with a stack and its number: the global pointer is the program's, the
 * thread pointer core 0's, which waits at the stack's top (what the C library keeps there is
 * shared, then: a job does not use it).
 */
void mcw_entry(void);
uint32_t mcw_loop(uint32_t core) __attribute__((used));	/* (called from the lines below only) */
__asm__(".text\n"
	".globl mcw_entry\n"
	"mcw_entry:\n"
	".option push\n"
	".option norelax\n"
	"	la gp, __global_pointer$\n"
	".option pop\n"
	"	lw tp, 0(sp)\n"
	"	call mcw_loop\n"
	"	ebreak\n");

uint32_t mcw_loop(uint32_t core)
{
	mc_job *job = (mc_job *)(page + MC_JOB_AT(core));
	mc_answer *answer = (mc_answer *)(page + MC_ANSWER_AT(core));
	uint32_t last = job->seq;

	answer->done = last;
	answer->alive = MC_ALIVE | core;
	for (;;) {
		uint32_t at = job->seq;
		if (at != last) {
			mcw_fn fn = (mcw_fn)job->fn;
			if (!fn) {
				answer->alive = 0;
				answer->done = at;
				__asm__ volatile("ebreak");	/* the machine parks this core */
			}
			answer->result = fn(job->a0, job->a1);
			answer->done = at;
			last = at;
		}
		mc_next_pass();	/* asleep until the job's first word changes */
	}
}

/* A worker that has stopped: the page it wanted is touched, or the program ends. */
static void serve(int k)
{
	mc_fault *fault;
	uint32_t at, cause, address;

	if (!core_now(k)) return;
	/* (looked at first: most passes a worker has asked nothing, and taking its core costs two stores) */
	if (fault_of(k)->count == *resume_of(k)) return;
	/* (only a core that is this program's still: another's stops at pages of its own) */
	if (!writing(core_now(k))) return;
	fault = fault_of(k);
	at = fault->count;
	if (at == *resume_of(k)) {
		written();
		return;
	}
	cause = fault->cause;
	address = fault->address;
	if (cause == 12 || cause == 13) {
		/* a page not there yet, or not yet used: a read brings it */
		volatile uint8_t byte = *(volatile uint8_t *)address;
		(void)byte;
	} else if (cause == 15) {
		/* a store: a byte stored as it is makes the page ours to write, and changes nothing
		 * (the machine keeps no store that changes nothing, so no other core's is undone) */
		volatile uint8_t *byte = (volatile uint8_t *)address;
		*byte = *byte;
	} else if (cause == 8) {
		/* a system call: made here for it, in the program and the files they share. (Not
		 * the ones that are about which thread calls: a new thread, a new program.) */
		const volatile uint32_t *c = (const volatile uint32_t *)(page + MC_CALL_AT(core_now(k)));
		long number = (long)c[0], result = -38;	/* ENOSYS */

		if (number != 220 && number != 221 && number != 435)	/* clone, execve, clone3 */
			result = call6(number, (long)c[1], (long)c[2], (long)c[3], (long)c[4], (long)c[5], (long)c[6]);
		resume_of(k)[1] = (uint32_t)result;
		mcw_calls++;
	} else {
		say("mcw: a worker core stopped, cause ", cause);
		say("mcw:   at pc ", fault->pc);
		say("mcw:   address ", address);
		say("mcw:   its count ", at);
		say("mcw:   core 0 had answered ", *resume_of(k));
		say("mcw:   worker ", (uint32_t)core_now(k));
		call6(94, 70, 0, 0, 0, 0, 0);	/* exit_group */
	}
	mcw_faults++;
	*resume_of(k) = at;
	written();
}

int mcw_count(void) { return count; }

void mcw_rest(void)
{
	static int none;	/* a kernel from before the rest: the pause hint, as it was */

	if (device >= 0 && !none && call6(29, device, MC_REST, 0, 0, 0, 0) == 0) return;
	none = device >= 0;
	mc_next_pass();
}

int mcw_most(void)
{
	static int most = -1;
	volatile uint32_t *words;
	int fd;

	if (most >= 0) return most;
	most = 0;
	if ((fd = (int)call6(56, -100, (long)"/dev/gpu", 2, 0, 0, 0)) < 0) return 0;
	/* the machine's control words: how many cores it could have, or (a machine whose cores are as they are) has */
	if ((words = map(4096, fd, 0x01000000)) != 0) {
		uint32_t cores = words[0x3cc / 4] ? words[0x3cc / 4] : words[0x3c0 / 4];

		most = cores > MC_MAX_CORES ? MC_MAX_CORES - 1 : cores > 0 ? (int)cores - 1 : 0;
		call6(215, (long)words, 4096, 0, 0, 0, 0);
	}
	call6(57, fd, 0, 0, 0, 0, 0);
	return most;
}

void mcw_lend(void) { lent = 1; }

int mcw_asked(void)
{
	int k, asked = lost;

	if (!page) return 0;
	for (k = 1; k <= count; k++)
		if (core_of[k] && (((volatile uint32_t *)(page + MC_RECALL_AT))[core_of[k] / 32] >> (core_of[k] % 32) & 1)) asked = 1;
	return asked;
}

int mcw_recalled(int k)
{
	int core = core_of[k];

	return core && page && (((volatile uint32_t *)(page + MC_RECALL_AT))[core / 32] >> (core % 32) & 1);
}

int mcw_bound(int k) { return k >= 1 && k <= count && core_of[k] != 0; }

static volatile uint32_t *owner_of_core(int core) { return (volatile uint32_t *)(page + MC_OWNER_AT(core)); }

/* This program is about to write to a core's mailbox: none of its cores is taken meanwhile. */
static int writing(int core)
{
	*(volatile uint32_t *)(page + MC_POSTING_AT(my_tag)) = 1;
	if (*owner_of_core(core) == my_tag) return 1;
	*(volatile uint32_t *)(page + MC_POSTING_AT(my_tag)) = 0;
	return 0;
}
static void written(void) { *(volatile uint32_t *)(page + MC_POSTING_AT(my_tag)) = 0; }

/* Worker k's core is given its start: it says it is there some passes later (arrived()). */
static void start(int k, int core)
{
	volatile uint32_t *at = (volatile uint32_t *)(page + MC_START_AT(core));
	uint32_t *top = (uint32_t *)(stacks + (uint32_t)k * STACK_BYTES - 16);

	top[0] = own_tp;
	if (!writing(core)) return;	/* (taken back already) */
	starting[k] = core;
	at[1] = (uint32_t)mcw_entry;
	at[2] = (uint32_t)top;
	at[3] = root;
	at[0] = MC_START;
	written();
}

/* Whether the core started for worker k is there: it is k's from then on. */
static int arrived(int k)
{
	int core = starting[k];
	mc_answer *answer = (mc_answer *)(page + MC_ANSWER_AT(core));

	if (!core) return 0;
	if (*owner_of_core(core) != my_tag) {
		starting[k] = 0;	/* (taken back before it was there) */
		return 0;
	}
	if (answer->alive != (MC_ALIVE | (uint32_t)core)) return 0;
	if (!writing(core)) {
		starting[k] = 0;
		return 0;
	}
	*(volatile uint32_t *)(page + MC_START_AT(core)) = 0;
	written();
	starting[k] = 0;
	core_of[k] = core;
	seq[k] = 0;
	return 1;
}

/* The kernel is asked for this program's share again: the cores that are new to it are
 * started for the workers that have none. How many it has or is starting. */
static int claim(int want)
{
	uint32_t answer[2 + MC_MASK_WORDS] = {(uint32_t)want};
	uint8_t has[MC_MAX_CORES] = {0};	/* the cores a worker has or is being started on */
	int core, k, n = 0, free_k = 1;

	offer_seen = *(volatile uint32_t *)(page + MC_OFFER_AT);
	if (call6(29, device, MC_WORKERS_ALL, (long)answer, 0, 0, 0) != 0) return 0;
	root = answer[1];
	share = (int)(answer[0] & 0xfff);
	my_tag = answer[0] >> 12;
	for (k = 1; k < MC_MAX_CORES; k++) has[core_of[k]] = has[starting[k]] = 1;
	for (core = 1; core < MC_MAX_CORES; core++) {
		if (!(answer[2 + core / 32] >> (core % 32) & 1)) continue;
		n++;
		if (has[core]) continue;		/* one it has */
		/* the first worker without a core: none before it loses one in here */
		for (; free_k <= want && (core_of[free_k] || starting[free_k]); free_k++) {}
		k = free_k;
		if (k > want) {
			/* (more than it has workers for: back it goes) */
			uint32_t back[MC_MASK_WORDS] = {0};

			back[core / 32] = 1u << (core % 32);
			call6(29, device, MC_GIVE, (long)back, 0, 0, 0);
			n--;
			continue;
		}
		start(k, core);
	}
	return n;
}

/* Worker k has no core any more: its last answer is kept here. */
static void unbind(int k, int give)
{
	int core = core_of[k];

	if (!core) return;
	if (!settled[k]) {
		result_of[k] = answer_of(k)->result;
		took(k);
	}
	core_of[k] = 0;
	lost = 1;
	if (give) {
		uint32_t back[MC_MASK_WORDS] = {0};

		back[core / 32] = 1u << (core % 32);
		call6(29, device, MC_GIVE, (long)back, 0, 0, 0);
	}
}

/*
 * What has become of worker k's core, looked at before anything is done with it. Only between
 * jobs: the kernel takes no core whose job is out, and neither does this give one up.
 */
static void look(int k)
{
	int core = core_of[k];

	if (starting[k]) {
		/* (its first steps may stop at pages: left unserved, it never arrived, and what was
		 * posted to it ran on the posting core for good) */
		serve(k);
		arrived(k);
	}
	core = core_of[k];
	if (core) {
		if (answer_of(k)->done != seq[k]) return;	/* at a job */
		if (*owner_of_core(core) != my_tag) unbind(k, 0);	/* the kernel took it back */
		else if (((volatile uint32_t *)(page + MC_RECALL_AT))[core / 32] >> (core % 32) & 1) unbind(k, 1);
	} else if (!starting[k] && *(volatile uint32_t *)(page + MC_OFFER_AT) != offer_seen) {
		claim(count);	/* cores have come free: this program's share again */
	}
}

int mcw_open(int limit)
{
	uint32_t i;
	int fd, k, pass;

	if (page) return count;
	if (limit > MC_MAX_CORES - 1) limit = MC_MAX_CORES - 1;
	/* (O_RDWR | O_CLOEXEC: a program that runs another in its place gives its workers back) */
	if (limit <= 0 || (fd = (int)call6(56, -100, (long)"/dev/gpu", 2 | 0x80000, 0, 0, 0)) < 0) return 0;
	device = fd;
	for (k = 0; k < MC_MAX_CORES; k++) {
		core_of[k] = starting[k] = 0;
		settled[k] = 1;
		result_of[k] = seq[k] = 0;
	}
	lost = share = 0;
	if (lent) call6(29, fd, MC_LENT, 0, 0, 0, 0);
	if (!(page = map(MC_PAGE_SIZE, fd, MC_PAGE_GPU_OFFSET))) goto none;
	/* (the mailboxes' pages here now, and this program's to write: a worker's first store to its
	 * own, which is its first step, would stop it until this core had stored there) */
	mcw_touch(page, MC_PAGE_SIZE);
	/* (the stacks of an earlier open serve again when they are enough) */
	if (stack_count < limit) {
		if (!(stacks = map((long)(limit + 1) * STACK_BYTES, -1, 0))) goto none;
		stack_count = limit + 1;
	}
	/* the top of each stack there before its worker steps on it */
	for (k = 1; k < stack_count; k++)
		for (i = 4096; i <= STACK_TOUCHED; i += 4096) stacks[(uint32_t)k * STACK_BYTES - i] = 0;
	__asm__ volatile("mv %0, tp" : "=r"(own_tp));
	/* the code a worker starts in, read here: a page of it no core has run stops it (cause 12) */
	mcw_touch_code((const void *)mcw_entry, 64);
	mcw_touch_code((const void *)mcw_loop, 512);
	/* this program's share of the cores (the kernel waits a while for those other programs
	 * have over theirs); from here the kernel parks them when the program ends */
	claim(limit);
	/* as many workers as its share, whether or not their cores are here yet: one whose core
	 * is another program's until that one's job ends has it some passes later (look()) */
	count = share < limit ? share : limit;
	if (count <= 0) goto none;
	/* a core that is there says so within a few passes; its first steps may want pages */
	for (pass = 0; pass < 48; pass++) {
		int all = 1;
		for (k = 1; k <= count; k++) {
			if (starting[k] && !arrived(k)) all = 0;
			serve(k);
		}
		if (all) break;
		mcw_rest();
	}
	mc_next_pass();
	return count;
none:
	if (stacks) call6(215, (long)stacks, (long)stack_count * STACK_BYTES, 0, 0, 0, 0);
	stack_count = 0;
	if (page) call6(215, (long)page, MC_PAGE_SIZE, 0, 0, 0, 0);
	call6(57, device, 0, 0, 0, 0, 0);	/* and the kernel has the cores back */
	device = -1;
	page = 0;
	stacks = 0;
	count = 0;
	return 0;
}

void mcw_close(void)
{
	int k, passes, step;

	if (!page) return;
	/* The workers leave their loops, in two steps: the jobs that are out end, then the leaving
	 * is posted. A worker with a job out is never posted to: it would sleep on the new job's
	 * word and never wake for it. All are waited for at once, their pages brought meanwhile. */
	for (step = 0; step < 2; step++) {
		if (step == 1)
			for (k = 1; k <= count; k++)
				if (core_of[k] && answer_of(k)->done == seq[k] && writing(core_of[k])) {
					mc_job *j = job_of(k);

					j->fn = 0;
					j->seq = ++seq[k];
					settled[k] = 0;
					written();
				} else {
					core_of[k] = 0;
				}
		for (passes = 0; passes < 1500; passes++) {
			int out = 0;

			for (k = 1; k <= count; k++) {
				if (!core_of[k]) continue;
				serve(k);
				if (answer_of(k)->done != seq[k]) out++;
			}
			if (!out) break;
			mcw_rest();
		}
	}
	mc_next_pass();
	call6(215, (long)page, MC_PAGE_SIZE, 0, 0, 0, 0);
	call6(57, device, 0, 0, 0, 0, 0);
	device = -1;
	page = 0;
	count = 0;
	/* (the stacks stay: a worker is on one until the machine has parked it) */
}

void mcw_touch_code(const void *code, unsigned bytes)
{
	const volatile uint8_t *at = (const volatile uint8_t *)((uintptr_t)code & ~4095u);
	const volatile uint8_t *end = (const volatile uint8_t *)code + bytes;

	for (; at < end; at += 4096) (void)*at;
}

void mcw_touch(void *memory, unsigned bytes)
{
	volatile uint8_t *at = (volatile uint8_t *)((uintptr_t)memory & ~4095u);
	volatile uint8_t *end = (volatile uint8_t *)memory + bytes;

	/* a word or-ed with nothing: the page is there and this program's to write, and nothing
	 * changes. One instruction that stores, so one fault: a load and then a store were two. */
	for (; at < end; at += 4096) __asm__ volatile("amoor.w zero, zero, (%0)" : : "r"(at) : "memory");
}

void *mcw_alloc(unsigned bytes)
{
	void *memory;

	bytes = (bytes + 15) & ~15u;
	if (bytes > CHUNK_BYTES) return map((bytes + 4095) & ~4095u, -1, 0);
	if (bytes > chunk_left) {
		if (!(chunk = map(CHUNK_BYTES, -1, 0))) return 0;
		chunk_left = CHUNK_BYTES;
	}
	memory = chunk;
	chunk += bytes;
	chunk_left -= bytes;
	return memory;
}

void mcw_post(int k, mcw_fn fn, uint32_t a0, uint32_t a1)
{
	mc_job *j;

	look(k);
	if (core_of[k]) {
		if (writing(core_of[k])) {
			j = job_of(k);
			j->fn = (uint32_t)fn;
			j->a0 = a0;
			j->a1 = a1;
			j->seq = ++seq[k];
			settled[k] = 0;
			written();
			return;
		}
		/* (the kernel took the core between the look and now) */
		core_of[k] = 0;
		lost = 1;
	}
	/* no core: the job is done here and now */
	result_of[k] = fn ? fn(a0, a1) : 0;
	settled[k] = 1;
}

int mcw_done(int k)
{
	if (settled[k]) {
		look(k);
		return 1;
	}
	serve(k);
	if (answer_of(k)->done != seq[k]) return 0;
	result_of[k] = answer_of(k)->result;
	took(k);
	look(k);
	return 1;
}

uint32_t mcw_result(int k) { return settled[k] || !core_of[k] ? result_of[k] : answer_of(k)->result; }

int mcw_wait(int k)
{
	int passes = 0, other;

	while (!mcw_done(k)) {
		for (other = 1; other <= count; other++) serve(other);
		mcw_rest();
		passes++;
	}
	return passes;
}

int mcw_self(void)
{
	uint8_t here;

	if (!mcw_on_worker()) return 0;
	return (int)((uint32_t)(&here - stacks) / STACK_BYTES) + 1;
}

int mcw_slot(void) { return page ? (int)(my_tag & 15) : -1; }

void *mcw_map(uint32_t offset, uint32_t bytes) { return device >= 0 ? map((long)bytes, device, (long)offset) : 0; }

int mcw_on_worker(void)
{
	uint8_t here;

	return stacks && &here >= stacks && &here < stacks + (uint32_t)stack_count * STACK_BYTES;
}
