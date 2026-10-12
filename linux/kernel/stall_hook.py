"""Takes passes the kernel ended out of three common paths (docs/boot.md, "Passes that end early").

Pages wanted zeroed are cleared sixty-four at a time: a block in one parallel fill, handed out a
page at a time (mm/page_alloc.c), and a fault's zeroed page is asked for as one (asm/page.h).
A whole page the ROM driver copied in the commit pass needs no fence.i when it is mapped to
run: nothing of it is in the write cache (phram.c, fs/romfs/super.c). A fill under 2 KB
stays on the processor (memset.S). The ROM is read ahead, a window's pages allocated as one
block and copied by one parallel copy (mm/readahead.c, romfs, mtdcore.c). Idempotent."""
import sys
root = sys.argv[1]


def patch(rel, pairs, mark='ShaderEmu stall'):
    path = root + '/' + rel
    s = open(path).read()
    if mark in s:
        return
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old)
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print('stall hook:', rel)


patch('mm/page_alloc.c', [
('''struct page *__alloc_pages(gfp_t gfp, unsigned int order, int preferred_nid,
							nodemask_t *nodemask)
{
''', '''/*
 * ShaderEmu stall: clearing a page is one parallel fill, which ends the machine's pass.
 * A block of sixty-four is cleared in one fill and its pages are handed out from here.
 */
#define ZEROED_ORDER 6
static struct page *zeroed_pages[1 << ZEROED_ORDER];
static int zeroed_count;

static struct page *zeroed_page(gfp_t gfp)
{
	struct page *page = NULL, *spare[1 << ZEROED_ORDER];
	unsigned long flags;
	int i, spares = 0;

	local_irq_save(flags);
	if (zeroed_count)
		page = zeroed_pages[--zeroed_count];
	local_irq_restore(flags);
	if (page)
		return page;
	/* (not at any price: a page by itself is cleared the old way) */
	page = __alloc_pages((gfp & ~(__GFP_ZERO | __GFP_DIRECT_RECLAIM | __GFP_COMP | __GFP_NOFAIL)) | __GFP_NOWARN | __GFP_NORETRY,
			     ZEROED_ORDER, 0, NULL);
	if (!page)
		return NULL;
	split_page(page, ZEROED_ORDER);
	memset(page_address(page), 0, PAGE_SIZE << ZEROED_ORDER);
	local_irq_save(flags);
	for (i = 1; i < 1 << ZEROED_ORDER; i++) {
		if (zeroed_count < 1 << ZEROED_ORDER)
			zeroed_pages[zeroed_count++] = page + i;
		else
			spare[spares++] = page + i;
	}
	local_irq_restore(flags);
	while (spares)
		__free_page(spare[--spares]);
	return page;
}

struct page *__alloc_pages(gfp_t gfp, unsigned int order, int preferred_nid,
							nodemask_t *nodemask)
{
'''),
('''	if (unlikely(order >= MAX_ORDER)) {
		WARN_ON_ONCE(!(gfp & __GFP_NOWARN));
		return NULL;
	}

	gfp &= gfp_allowed_mask;
''', '''	if (unlikely(order >= MAX_ORDER)) {
		WARN_ON_ONCE(!(gfp & __GFP_NOWARN));
		return NULL;
	}

	if (order == 0 && (gfp & __GFP_ZERO)) {
		page = zeroed_page(gfp);
		if (page)
			return page;
	}

	gfp &= gfp_allowed_mask;
'''),
])

patch('arch/riscv/include/asm/page.h', [
('''#define clear_user_page(pgaddr, vaddr, page)	memset((pgaddr), 0, PAGE_SIZE)
''', '''#define clear_user_page(pgaddr, vaddr, page)	memset((pgaddr), 0, PAGE_SIZE)
/* ShaderEmu stall: a fault's zeroed page comes from the allocator's cleared block */
#define alloc_zeroed_user_highpage_movable(vma, vaddr) \\
	alloc_page_vma(GFP_HIGHUSER_MOVABLE | __GFP_ZERO, vma, vaddr)
#define __HAVE_ARCH_ALLOC_ZEROED_USER_HIGHPAGE_MOVABLE
'''),
])

patch('drivers/mtd/devices/phram.c', [
('''static int phram_read(struct mtd_info *mtd, loff_t from, size_t len,
''', '''/* ShaderEmu stall: the buffer of the last read, when all of a page was copied in the commit pass */
void *shaderemu_copied_page;
EXPORT_SYMBOL(shaderemu_copied_page);

static int phram_read(struct mtd_info *mtd, loff_t from, size_t len,
'''),
('''		csr_write(0x0b0, 1);
		memcpy(buf + head + body, src + head + body, len - head - body);
''', '''		csr_write(0x0b0, 1);
		memcpy(buf + head + body, src + head + body, len - head - body);
		shaderemu_copied_page = body == PAGE_SIZE && !((unsigned long)buf & ~PAGE_MASK) ? buf : NULL;
'''),
('''	} else {
		memcpy(buf, src, len);
	}
	*retlen = len;
''', '''	} else {
		memcpy(buf, src, len);
		shaderemu_copied_page = NULL;
	}
	*retlen = len;
'''),
])

patch('fs/romfs/super.c', [
('''static int romfs_readpage(struct file *file, struct page *page)
{
''', '''/* ShaderEmu stall: set by the ROM driver (phram.c) */
extern void *shaderemu_copied_page;

static int romfs_readpage(struct file *file, struct page *page)
{
'''),
('''	flush_dcache_page(page);
	kunmap(page);
	unlock_page(page);
	return ret;
''', '''	flush_dcache_page(page);
	/*
	 * ShaderEmu stall: the commit pass wrote this whole page and the pass ended there, so
	 * no store to it waits in the write cache: mapping it to run needs no fence.i.
	 */
	if (ret == 0 && fillsize == PAGE_SIZE && shaderemu_copied_page == buf)
		set_bit(PG_dcache_clean, &page->flags);
	shaderemu_copied_page = NULL;
	kunmap(page);
	unlock_page(page);
	return ret;
'''),
])

# A fill under 2 KB is quicker stored a word at a time (a few hundred instructions and at most
# a third of the write cache) than as a parallel fill, which ends the pass.
path = root + '/arch/riscv/lib/memset.S'
s = open(path).read()
old = '\tli t0, 1024\n\tbltu a2, t0, 9f\n\tor t1, a0, a2\n'
if old in s:
    open(path, 'w').write(s.replace(old, '\tli t0, 2048\n\tbltu a2, t0, 9f\n\tor t1, a0, a2\n'))
    print('stall hook: arch/riscv/lib/memset.S')


# (a tree hooked when the block was sixteen pages)
path = root + '/mm/page_alloc.c'
s = open(path).read()
if '#define ZEROED_ORDER 4' in s:
    open(path, 'w').write(s.replace('#define ZEROED_ORDER 4', '#define ZEROED_ORDER 6').replace('A block of sixteen is cleared', 'A block of sixty-four is cleared'))
    print('stall hook: mm/page_alloc.c (blocks of sixty-four)')

# tmpfs and shared memory cleared each new page by itself: theirs come from the cleared block too
patch('mm/shmem.c', [
('''		page = shmem_alloc_and_acct_page(gfp, inode,
''', '''		/* ShaderEmu zeroed: from the allocator's cleared block (mm/page_alloc.c) */
		page = shmem_alloc_and_acct_page(gfp | __GFP_ZERO, inode,
'''),
('''		for (i = 0; i < compound_nr(page); i++) {
			clear_highpage(page + i);
			flush_dcache_page(page + i);
		}
		SetPageUptodate(page);
''', '''		for (i = 0; i < compound_nr(page); i++) {
			/* (one allocated just now, and not a huge one, came cleared) */
			if (!alloced || PageTransHuge(page))
				clear_highpage(page + i);
			flush_dcache_page(page + i);
		}
		SetPageUptodate(page);
'''),
], 'ShaderEmu zeroed')

# Reading ahead: a file of the ROM was read a page at a time, a parallel copy and so a pass
# each. A window of pages that lie one after another in memory and in the file is one copy.
AHEAD = 'ShaderEmu readahead'
patch('drivers/mtd/mtdcore.c', [
('''	bdi->ra_pages = 0;
''', '''	bdi->ra_pages = 32;	/* ShaderEmu readahead: a window is one parallel copy */
'''),
], AHEAD)

patch('drivers/mtd/devices/phram.c', [
('''		shaderemu_copied_page = body == PAGE_SIZE && !((unsigned long)buf & ~PAGE_MASK) ? buf : NULL;
''', '''		/* ShaderEmu readahead: or several whole pages */
		shaderemu_copied_page = body == len && !((len | (unsigned long)buf) & ~PAGE_MASK) ? buf : NULL;
'''),
], AHEAD)

patch('mm/readahead.c', [
('''	gfp_t gfp_mask = readahead_gfp_mask(mapping);
	unsigned long i;
''', '''	gfp_t gfp_mask = readahead_gfp_mask(mapping);
	unsigned long i;
	/*
	 * ShaderEmu readahead: the window's pages from one block, in the file's order, so that a
	 * file system that reads ahead (romfs) fills them with one parallel copy.
	 */
	unsigned int block_order = get_order(nr_to_read << PAGE_SHIFT);
	struct page *block = NULL;
	unsigned long block_used = 0;

	if (nr_to_read > 1 && nr_to_read <= BITS_PER_LONG && mapping->a_ops->readahead && !mapping->a_ops->readpages)
		block = alloc_pages((gfp_mask & ~(__GFP_DIRECT_RECLAIM | __GFP_COMP)) | __GFP_NOWARN | __GFP_NORETRY, block_order);
	if (block)
		split_page(block, block_order);
'''),
('''		folio = filemap_alloc_folio(gfp_mask, 0);
		if (!folio)
			break;
''', '''		if (block) {
			folio = page_folio(block + i);
			block_used |= 1UL << i;
		} else {
			folio = filemap_alloc_folio(gfp_mask, 0);
		}
		if (!folio)
			break;
'''),
('''	read_pages(ractl, &page_pool, false);
	filemap_invalidate_unlock_shared(mapping);
''', '''	read_pages(ractl, &page_pool, false);
	/* (the block's pages the window did not take: already cached, or past its end) */
	for (i = 0; block && i < 1UL << block_order; i++)
		if (i >= BITS_PER_LONG || !(block_used & (1UL << i)))
			__free_page(block + i);
	filemap_invalidate_unlock_shared(mapping);
'''),
], AHEAD)

patch('fs/romfs/super.c', [
('''static const struct address_space_operations romfs_aops = {
	.readpage	= romfs_readpage
};
''', '''/*
 * ShaderEmu readahead: whole pages that follow one another in memory and in the file are
 * read with one call, which the ROM driver makes one parallel copy (and so one pass ended).
 */
#define ROMFS_RUN 32

static void romfs_read_run(struct inode *inode, struct page **run, int n)
{
	void *buf = page_address(run[0]);
	unsigned long pos = ROMFS_I(inode)->i_dataoffset + page_offset(run[0]);
	int ret = romfs_dev_read(inode->i_sb, pos, buf, (size_t)n << PAGE_SHIFT);
	bool clean = ret == 0 && shaderemu_copied_page == buf;
	int i;

	shaderemu_copied_page = NULL;
	for (i = 0; i < n; i++) {
		if (ret == 0)
			SetPageUptodate(run[i]);
		else
			SetPageError(run[i]);
		flush_dcache_page(run[i]);
		if (clean)
			set_bit(PG_dcache_clean, &run[i]->flags);
		unlock_page(run[i]);
		put_page(run[i]);
	}
}

static void romfs_readahead(struct readahead_control *rac)
{
	struct inode *inode = rac->mapping->host;
	loff_t size = i_size_read(inode);
	struct page *run[ROMFS_RUN], *page;
	int n = 0;

	while ((page = readahead_page(rac))) {
		bool whole = page_offset(page) + PAGE_SIZE <= size;

		if (n && (!whole || n == ROMFS_RUN || page != run[n - 1] + 1 || page->index != run[n - 1]->index + 1)) {
			romfs_read_run(inode, run, n);
			n = 0;
		}
		if (whole) {
			run[n++] = page;
		} else {
			/* the file's last page, part of it zeros: the old way (it unlocks the page) */
			romfs_readpage(NULL, page);
			put_page(page);
		}
	}
	if (n)
		romfs_read_run(inode, run, n);
}

static const struct address_space_operations romfs_aops = {
	.readpage	= romfs_readpage,
	.readahead	= romfs_readahead
};
'''),
], AHEAD)
