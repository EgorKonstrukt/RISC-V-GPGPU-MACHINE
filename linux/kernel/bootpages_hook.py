"""Has the boot count its pages free from the start (mm/page_alloc.c; docs/machine/boot.md, "Passes that
end early"). Every page was set up as used once and then, to be freed, written again: a store
to each page's own 16 bytes, which filled the machine's write cache sixty times in a boot. A
page of the boot's memory map now starts with the count the freeing wants; the pages that
are kept (the kernel, its early allocations) get theirs when they are marked. Idempotent."""
import re, sys
path = sys.argv[1] + '/mm/page_alloc.c'
s = open(path).read()
MARK = 'ShaderEmu boot pages'
if MARK not in s:
    # every other way a page is set up counts it as before
    s, n = re.subn(r'^(\s*)__init_single_page\((.+?), pfn, (\w+), (\w+)\);\n(?!\t\tif \(context == MEMINIT_HOTPLUG\))',
                   lambda m: m.group(0) + m.group(1) + 'init_page_count(' + m.group(2) + ');\n', s, flags=re.M)
    assert n == 4, n
    for old, new in (
('''	set_page_links(page, zone, nid, pfn);
	init_page_count(page);
''', '''	set_page_links(page, zone, nid, pfn);
	/* ShaderEmu boot pages: the count stays 0, which freeing wants; who keeps a page counts it */
'''),
('''		__init_single_page(page, pfn, zone, nid);
		if (context == MEMINIT_HOTPLUG)
			__SetPageReserved(page);
''', '''		__init_single_page(page, pfn, zone, nid);
		if (context == MEMINIT_HOTPLUG) {
			init_page_count(page);
			__SetPageReserved(page);
		}
'''),
('''			/* Avoid false-positive PageTail() */
			INIT_LIST_HEAD(&page->lru);
''', '''			/* Avoid false-positive PageTail() */
			INIT_LIST_HEAD(&page->lru);

			/* ShaderEmu boot pages: a page that is kept is counted here */
			init_page_count(page);
''')):
        assert s.count(old) == 1, old
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print('boot pages hook: mm/page_alloc.c')
