"""One fence.i for a debugger's pokes, not one each (docs/boot.md, "Passes that end early").

A word written into another program's code (ptrace's POKETEXT: a debugger's stops, placed and
lifted several at a time) was a fence.i, which ends the machine's pass. The write is noted
instead, and the fence.i is made at the next switch of tasks: before the poked program runs
again. A program that writes its own code so is flushed at once, as before. Idempotent."""
import sys
root = sys.argv[1]
MARK = 'ShaderEmu poke'


def patch(rel, pairs):
    path = root + '/' + rel
    s = open(path).read()
    if MARK in s:
        return
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old)
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print('poke hook:', rel)


patch('arch/riscv/include/asm/cacheflush.h', [
('''#define flush_icache_user_page(vma, pg, addr, len) \\
	flush_icache_mm(vma->vm_mm, 0)
''', '''/*
 * ShaderEmu poke: code written into another program is fetched new once the machine's pass
 * has ended, which the fence.i at the next switch of tasks sees to (kernel/sched/core.c).
 */
extern int shaderemu_poked;
#define flush_icache_user_page(vma, pg, addr, len) \\
	do { \\
		if ((vma)->vm_mm == current->mm) \\
			flush_icache_mm((vma)->vm_mm, 0); \\
		else \\
			shaderemu_poked = 1; \\
	} while (0)
'''),
])

patch('kernel/sched/core.c', [
('''static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next, struct rq_flags *rf)
{
	prepare_task_switch(rq, prev, next);
''', '''/* ShaderEmu poke: code was written into a program that has not run since (asm/cacheflush.h) */
int shaderemu_poked;

static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next, struct rq_flags *rf)
{
	if (unlikely(shaderemu_poked)) {
		shaderemu_poked = 0;
		flush_icache_all();
	}
	prepare_task_switch(rq, prev, next);
'''),
])
