"""The nommu signal trampoline is written to the user stack: a fence.i commits it before the handler can return into it."""
import sys
p = sys.argv[1] + '/arch/riscv/kernel/signal.c'
s = open(p).read()
old = "\tregs->ra = (unsigned long)&frame->sigreturn_code;\n#endif /* CONFIG_MMU */"
new = "\tlocal_flush_icache_all();\n\tregs->ra = (unsigned long)&frame->sigreturn_code;\n#endif /* CONFIG_MMU */"
if 'local_flush_icache_all();\n\tregs->ra' not in s:
    assert s.count(old) == 1
    s = s.replace(old, new)
    if '#include <asm/cacheflush.h>' not in s:
        s = s.replace('#include <asm/ucontext.h>', '#include <asm/ucontext.h>\n#include <asm/cacheflush.h>', 1)
    open(p, 'w').write(s)
print('signal.c patched')
