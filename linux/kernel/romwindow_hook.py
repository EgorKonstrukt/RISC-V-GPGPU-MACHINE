"""Fewer reads of the ROM for a file (docs/boot.md, "Passes that end early"): each is a parallel
copy, which ends the machine's pass whatever its length.

A page wanted of a mapped file, or of a program being started, brings the stretch of 32 pages
it is in (the kernel's own windows grew from four pages and lay round the page, so a program
of 31 pages came in five reads); a file of up to 32 pages read from its start comes whole;
a file's last, partial page rides in the copy of the pages before it, and read by itself (a
program's icon, a short file) it is stored by the processor, which does not end the pass.
Runs after stall_hook.py and zeropool_hook.py. Idempotent."""
import sys
root = sys.argv[1]
MARK = 'ShaderEmu window'


def patch(rel, pairs):
    path = root + '/' + rel
    s = open(path).read()
    if MARK in s:
        return
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old)
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print('window hook:', rel)


patch('mm/readahead.c', [
('''/*
 * A minimal readahead algorithm for trivial sequential/random reads.
 */
static void ondemand_readahead(''', '''/*
 * ShaderEmu window: a read of the ROM is one parallel copy and ends the machine's pass. A page
 * wanted of a mapped file or of a program brings the stretch of ROM_WINDOW pages it is in,
 * and a file of no more than that read from its start comes whole. False: the kernel's way.
 */
#define ROM_WINDOW 32UL

bool rom_window(struct readahead_control *ractl, pgoff_t index, bool mapped)
{
	struct address_space *mapping = ractl->mapping;
	struct file_ra_state *ra = ractl->ra;

	if (!mapping->a_ops->readahead || mapping->a_ops->readpages || ra->ra_pages < ROM_WINDOW)
		return false;
	if (!mapped && !(ractl->file && (ractl->file->f_flags & __FMODE_EXEC)) &&
	    (index || i_size_read(mapping->host) > ROM_WINDOW << PAGE_SHIFT))
		return false;
	ra->start = index & ~(ROM_WINDOW - 1);
	ra->size = ROM_WINDOW;
	ra->async_size = 0;
	ractl->_index = ra->start;
	do_page_cache_ra(ractl, ra->size, 0);
	return true;
}

/*
 * A minimal readahead algorithm for trivial sequential/random reads.
 */
static void ondemand_readahead('''),
('''	pgoff_t prev_index;

	/*
	 * If the request exceeds the readahead window, allow the read to
''', '''	pgoff_t prev_index;

	if (rom_window(ractl, index, false))
		return;

	/*
	 * If the request exceeds the readahead window, allow the read to
'''),
])

patch('mm/internal.h', [
('''void force_page_cache_ra(struct readahead_control *, unsigned long nr);
''', '''void force_page_cache_ra(struct readahead_control *, unsigned long nr);
/* ShaderEmu window (mm/readahead.c) */
bool rom_window(struct readahead_control *ractl, pgoff_t index, bool mapped);
'''),
])

patch('mm/filemap.c', [
('''	fpin = maybe_unlock_mmap_for_io(vmf, fpin);
	ra->start = max_t(long, 0, vmf->pgoff - ra->ra_pages / 2);
''', '''	fpin = maybe_unlock_mmap_for_io(vmf, fpin);
	/* ShaderEmu window (mm/readahead.c) */
	if (rom_window(&ractl, vmf->pgoff, true))
		return fpin;
	ra->start = max_t(long, 0, vmf->pgoff - ra->ra_pages / 2);
'''),
])

# The driver says "copied in the commit pass" of any read that was all one copy, not only of
# whole pages: romfs knows which of its pages are whole.
patch('drivers/mtd/devices/phram.c', [
('''	if (len >= 1024 && (unsigned long)buf >= PAGE_OFFSET''', '''	if (len >= PAGE_SIZE && (unsigned long)buf >= PAGE_OFFSET'''),
('''		/* ShaderEmu readahead: or several whole pages */
		shaderemu_copied_page = body == len && !((len | (unsigned long)buf) & ~PAGE_MASK) ? buf : NULL;
''', '''		/* ShaderEmu readahead: or several whole pages; ShaderEmu window: or any whole texels */
		shaderemu_copied_page = body == len ? buf : NULL;
'''),
])

patch('fs/romfs/super.c', [
('''		/* (after a copy that ended the pass, the rest in pieces memset stores itself) */
		for (; at < PAGE_SIZE; at += n) {
			n = fillsize >= 1024 && PAGE_SIZE - at > 1536 ? 1536 : PAGE_SIZE - at;
''', '''		/* (in pieces memset stores itself) */
		for (; at < PAGE_SIZE; at += n) {
			n = PAGE_SIZE - at > 1536 ? 1536 : PAGE_SIZE - at;
'''),
('''static void romfs_read_run(struct inode *inode, struct page **run, int n)
{
	void *buf = page_address(run[0]);
	unsigned long pos = ROMFS_I(inode)->i_dataoffset + page_offset(run[0]);
	int ret = romfs_dev_read(inode->i_sb, pos, buf, (size_t)n << PAGE_SHIFT);
	bool clean = ret == 0 && shaderemu_copied_page == buf;
	int i;

	shaderemu_copied_page = NULL;
	for (i = 0; i < n; i++) {
''', '''static void romfs_read_run(struct inode *inode, struct page **run, int n)
{
	void *buf = page_address(run[0]);
	unsigned long pos = ROMFS_I(inode)->i_dataoffset + page_offset(run[0]);
	loff_t left = i_size_read(inode) - page_offset(run[0]);
	size_t len = min_t(loff_t, left, (loff_t)n << PAGE_SHIFT), body = len & ~(size_t)15;
	int ret = romfs_dev_read(inode->i_sb, pos, buf, body);
	bool clean = ret == 0 && shaderemu_copied_page == buf;
	int i, whole = len >> PAGE_SHIFT;

	shaderemu_copied_page = NULL;
	/*
	 * ShaderEmu window: the file's last page came in the same copy as far as whole texels
	 * go; its last bytes and the zeros after them are stored (in pieces memset stores itself).
	 */
	if (ret == 0 && body < len)
		ret = romfs_dev_read(inode->i_sb, pos + body, buf + body, len - body);
	while (len & ~PAGE_MASK) {
		size_t piece = min_t(size_t, PAGE_SIZE - (len & ~PAGE_MASK), 1536);

		memset(buf + len, 0, piece);
		len += piece;
	}
	for (i = 0; i < n; i++) {
'''),
('''		if (clean)
			set_bit(PG_dcache_clean, &run[i]->flags);
		unlock_page(run[i]);
''', '''		if (clean && i < whole)
			set_bit(PG_dcache_clean, &run[i]->flags);
		unlock_page(run[i]);
'''),
('''		if (n && (!whole || n == ROMFS_RUN || page != run[n - 1] + 1 || page->index != run[n - 1]->index + 1)) {
			romfs_read_run(inode, run, n);
			n = 0;
		}
		if (whole) {
			run[n++] = page;
		} else {
			/* the file's last page, part of it zeros: the old way (it unlocks the page) */
''', '''		if (n && (n == ROMFS_RUN || page != run[n - 1] + 1 || page->index != run[n - 1]->index + 1)) {
			romfs_read_run(inode, run, n);
			n = 0;
		}
		if (whole || n) {
			run[n++] = page;
		} else {
			/* the file's last page by itself, part of it zeros: the old way (it unlocks the page) */
'''),
])
