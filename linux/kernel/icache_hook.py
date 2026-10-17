"""One fence.i for a program's pages, not one each (arch/riscv/mm/cacheflush.c; docs/machine/boot.md,
"Passes that end early"). A page written by the kernel (a program just compiled into /tmp) is
mapped to run with a fence.i, which ends the machine's pass and with it commits every store
there is. So the file's pages round it in the page cache (sixteen) are as clean as it is, and
are marked so: they are mapped without one. Runs after stall_hook.py. Idempotent."""
import sys
path = sys.argv[1] + '/arch/riscv/mm/cacheflush.c'
s = open(path).read()
if 'ShaderEmu icache' not in s:
    old = '''	if (!test_and_set_bit(PG_dcache_clean, &page->flags))
		flush_icache_all();
}
'''
    new = '''	if (!test_and_set_bit(PG_dcache_clean, &page->flags)) {
		/*
		 * ShaderEmu icache: the fence.i ends the machine's pass, which commits all stores:
		 * the file's pages round this one in the page cache are clean too, as of now.
		 */
		struct address_space *mapping = page_mapping(page);
		struct page *next[16];
		unsigned int i, n;

		flush_icache_all();
		if (!mapping || PageAnon(page) || PageSwapCache(page))
			return;
		n = find_get_pages_range(mapping, &(pgoff_t){ page->index & ~15UL }, page->index | 15, 16, next);
		for (i = 0; i < n; i++) {
			set_bit(PG_dcache_clean, &next[i]->flags);
			put_page(next[i]);
		}
	}
}
'''
    assert s.count(old) == 1
    s = s.replace(old, new)
    old = '#include <asm/cacheflush.h>\n'
    assert s.count(old) == 1
    s = s.replace(old, '#include <linux/pagemap.h>\n' + old)
    open(path, 'w').write(s)
    print('icache hook: arch/riscv/mm/cacheflush.c')
