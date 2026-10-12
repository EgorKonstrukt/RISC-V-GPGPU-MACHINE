"""Keeps a pool of cleared memory that the idle loop fills (docs/boot.md, "Passes that end early").

A parallel fill ends the machine's pass. stall_hook.py cleared sixty-four pages in one; here a
megabyte is cleared at a time, by the idle loop when it can (a pass that would end there
anyway), and requests of up to eight pages wanted zeroed are cut from it: page faults, page
tables, tmpfs, and a new task's kernel stack. Runs after stall_hook.py. Idempotent."""
import sys
root = sys.argv[1]
MARK = 'ShaderEmu zero pool'

path = root + '/mm/page_alloc.c'
s = open(path).read()
if MARK not in s:
    start = s.index('/*\n * ShaderEmu stall: clearing a page')
    end = s.index('struct page *__alloc_pages(gfp_t gfp, unsigned int order, int preferred_nid,\n\t\t\t\t\t\t\tnodemask_t *nodemask)\n{\n', start)
    pool = r'''/*
 * ShaderEmu zero pool: clearing memory is a parallel fill, which ends the machine's pass.
 * Blocks are cleared ahead, by the idle loop when it can, and requests are cut from them
 * (in place of the ShaderEmu stall hook's block of sixty-four pages).
 * A page here has a count of 0 (a block is not split: that is a store to every page of it).
 */
#define ZERO_BLOCK_ORDER 8
#define ZERO_BLOCKS 4
#define ZERO_MAX_ORDER 3
#define ZERO_SINGLES 32
static struct page *zero_blocks[ZERO_BLOCKS];
static int zero_block_count;
static struct page *zero_run;		/* the block being handed out: zero_left pages from here */
static unsigned int zero_left;
static struct page *zero_singles[ZERO_SINGLES];	/* pages a larger request's alignment passed over */
static int zero_single_count;
static unsigned long zero_idle_after = INITIAL_JIFFIES;

static struct page *zero_block_new(gfp_t gfp, unsigned int order)
{
	struct page *page = __alloc_pages(gfp | __GFP_NOWARN | __GFP_NORETRY, order, 0, NULL);

	if (page)
		set_page_count(page, 0);
	return page;
}

static void zero_page_free(struct page *page)
{
	set_page_refcounted(page);
	__free_page(page);
}

static void zero_block_free(struct page *page, unsigned int pages)
{
	while (pages--)
		zero_page_free(page++);
}

static struct page *zeroed_pages(gfp_t gfp, unsigned int order)
{
	struct page *page = NULL, *block, *passed[1 << ZERO_MAX_ORDER];
	unsigned int n = 1 << order, block_order = ZERO_BLOCK_ORDER;
	unsigned long flags;
	int passes = 0;

	for (;;) {
		local_irq_save(flags);
		if (!order && zero_single_count)
			page = zero_singles[--zero_single_count];
		while (!page) {
			while (zero_left && (page_to_pfn(zero_run) & (n - 1))) {
				if (zero_single_count < ZERO_SINGLES)
					zero_singles[zero_single_count++] = zero_run;
				else
					passed[passes++] = zero_run;
				zero_run++;
				zero_left--;
			}
			if (zero_left >= n) {
				page = zero_run;
				zero_run += n;
				zero_left -= n;
			} else if (zero_block_count) {
				zero_run = zero_blocks[--zero_block_count];
				zero_left = 1 << ZERO_BLOCK_ORDER;
			} else {
				break;
			}
		}
		local_irq_restore(flags);
		while (passes)
			zero_page_free(passed[--passes]);
		if (page)
			break;
		/* nothing cleared ahead: a block now (not at any price: then the old way) */
		block = NULL;
		while (!block && block_order >= order + 2) {
			block = zero_block_new(0, block_order);
			if (!block)
				block_order -= 2;
		}
		if (!block)
			return NULL;
		memset(page_address(block), 0, PAGE_SIZE << block_order);
		local_irq_save(flags);
		if (!zero_left) {
			zero_run = block;
			zero_left = 1 << block_order;
			block = NULL;
		}
		local_irq_restore(flags);
		if (block)
			zero_block_free(block, 1 << block_order);
	}
	set_page_refcounted(page);
	clear_page_pfmemalloc(page);
	if (order && (gfp & __GFP_COMP))
		prep_compound_page(page, order);
	return page;
}

/* The idle loop's part: one block cleared a time it is entered, while the pool lacks any. */
noinline void zeroed_idle(void)
{
	struct page *block;
	unsigned long flags;

	if (zero_block_count >= ZERO_BLOCKS || time_before(jiffies, zero_idle_after))
		return;
	block = zero_block_new(__GFP_KSWAPD_RECLAIM, ZERO_BLOCK_ORDER);
	if (!block) {
		zero_idle_after = jiffies + HZ;
		return;
	}
	/* (the fill is asked for here, not in memset, so that the pass is seen to end in the idle loop) */
	csr_write(0x0b1, 0);
	csr_write(0x0b2, (unsigned long)page_address(block));
	csr_write(0x0b3, PAGE_SIZE << ZERO_BLOCK_ORDER);
	csr_write(0x0b0, 2);
	local_irq_save(flags);
	if (zero_block_count < ZERO_BLOCKS) {
		zero_blocks[zero_block_count++] = block;
		block = NULL;
	}
	local_irq_restore(flags);
	if (block)
		zero_block_free(block, 1 << ZERO_BLOCK_ORDER);
}

/* Memory that is short takes the blocks back, and the idle loop leaves them be for a while. */
static unsigned long zero_pool_count(struct shrinker *shrinker, struct shrink_control *sc)
{
	return (unsigned long)zero_block_count << ZERO_BLOCK_ORDER;
}

static unsigned long zero_pool_scan(struct shrinker *shrinker, struct shrink_control *sc)
{
	struct page *block = NULL;
	unsigned long flags;

	local_irq_save(flags);
	if (zero_block_count)
		block = zero_blocks[--zero_block_count];
	zero_idle_after = jiffies + 10 * HZ;
	local_irq_restore(flags);
	if (!block)
		return SHRINK_STOP;
	zero_block_free(block, 1 << ZERO_BLOCK_ORDER);
	return 1 << ZERO_BLOCK_ORDER;
}

static struct shrinker zero_pool_shrinker = {
	.count_objects = zero_pool_count,
	.scan_objects = zero_pool_scan,
	.seeks = DEFAULT_SEEKS,
};

static int __init zero_pool_init(void)
{
	return register_shrinker(&zero_pool_shrinker);
}
core_initcall(zero_pool_init);

'''
    s = s[:start] + pool + s[end:]
    old = '''	if (order == 0 && (gfp & __GFP_ZERO)) {
		page = zeroed_page(gfp);
		if (page)
			return page;
	}
'''
    new = '''	if (order <= ZERO_MAX_ORDER && (gfp & __GFP_ZERO)) {
		page = zeroed_pages(gfp, order);
		if (page)
			return page;
	}
'''
    assert s.count(old) == 1
    s = s.replace(old, new)
    old = '''	/* Use the single page allocator for one page. */
	if (nr_pages - nr_populated == 1)
		goto failed;
'''
    new = old + '''
	/* ShaderEmu zero pool: pages wanted zeroed (vzalloc) a page at a time, which is from the pool */
	if (gfp & __GFP_ZERO)
		goto failed;
'''
    assert s.count(old) == 1
    s = s.replace(old, new)
    old = '#include <linux/mm.h>\n'
    assert s.count(old) == 1
    s = s.replace(old, old + '#include <linux/shrinker.h>\n#include <asm/csr.h>\n')
    open(path, 'w').write(s)
    print('zero pool hook: mm/page_alloc.c')

# A device's announcement cleared 4 KB for its text (a fill each, a hundred in a boot): only
# the pointers before the text need to be zero.
path = root + '/lib/kobject_uevent.c'
s = open(path).read()
if MARK not in s:
    old = '''	env = kzalloc(sizeof(struct kobj_uevent_env), GFP_KERNEL);
	if (!env)
		return -ENOMEM;
'''
    new = '''	/* ShaderEmu zero pool: the text is written before it is read, and is not cleared */
	env = kmalloc(sizeof(struct kobj_uevent_env), GFP_KERNEL);
	if (!env)
		return -ENOMEM;
	memset(env, 0, offsetof(struct kobj_uevent_env, buf));
	env->buf[0] = 0;
	env->buflen = 0;
'''
    assert s.count(old) == 1
    open(path, 'w').write(s.replace(old, new))
    print('zero pool hook: lib/kobject_uevent.c')

# A file of the ROM under a page long: its one page was cleared past the file's end by stores
# (or a fill). It is asked for zeroed, which is from the pool, and only the file is copied in.
path = root + '/fs/romfs/super.c'
s = open(path).read()
if MARK not in s:
    for old, new in (
('''	if (fillsize < PAGE_SIZE)
		memset(buf + fillsize, 0, PAGE_SIZE - fillsize);
''', '''	/* ShaderEmu zero pool: a short file's page came zeroed (romfs_iget) */
	if (fillsize < PAGE_SIZE && !(mapping_gfp_mask(page->mapping) & __GFP_ZERO)) {
		unsigned long at = fillsize, n;

		/* (after a copy that ended the pass, the rest in pieces memset stores itself) */
		for (; at < PAGE_SIZE; at += n) {
			n = fillsize >= 1024 && PAGE_SIZE - at > 1536 ? 1536 : PAGE_SIZE - at;
			memset(buf + at, 0, n);
		}
	}
'''),
('''	case ROMFH_REG:
		i->i_fop = &romfs_ro_fops;
		i->i_data.a_ops = &romfs_aops;
''', '''	case ROMFH_REG:
		i->i_fop = &romfs_ro_fops;
		i->i_data.a_ops = &romfs_aops;
		/* ShaderEmu zero pool: see romfs_readpage */
		if (i->i_size < PAGE_SIZE)
			mapping_set_gfp_mask(i->i_mapping, mapping_gfp_mask(i->i_mapping) | __GFP_ZERO);
''')):
        assert s.count(old) == 1, old
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print('zero pool hook: fs/romfs/super.c')

path = root + '/kernel/sched/idle.c'
s = open(path).read()
if 'zeroed_idle' not in s:
    old = '\ttick_nohz_idle_enter();\n'
    assert s.count(old) == 1
    s = s.replace(old, '\t{\n\t\t/* ShaderEmu zero pool (mm/page_alloc.c) */\n\t\textern void zeroed_idle(void);\n\t\tzeroed_idle();\n\t}\n' + old)
    open(path, 'w').write(s)
    print('zero pool hook: kernel/sched/idle.c')
