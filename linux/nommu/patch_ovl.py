"""No MMU: overlayfs maps a ROM file where it lies (the real file's capabilities and address), and a
large mapping drops the page cache and tries again before it fails."""
import sys
K = sys.argv[1]


def change(path, old, new, mark):
    p = f'{K}/{path}'
    s = open(p).read()
    if mark in s:
        return
    assert s.count(old) == 1, (path, old[:40])
    open(p, 'w').write(s.replace(old, new))


OVL = '''
#ifndef CONFIG_MMU
/* No MMU: a mapping is made directly onto the real file's memory when that file allows it */
static unsigned ovl_mmap_capabilities(struct file *file)
{
	struct file *realfile = file->private_data;

	return realfile->f_op->mmap_capabilities ? realfile->f_op->mmap_capabilities(realfile) : NOMMU_MAP_COPY;
}

static unsigned long ovl_get_unmapped_area(struct file *file, unsigned long addr, unsigned long len,
					   unsigned long pgoff, unsigned long flags)
{
	struct file *realfile = file->private_data;

	if (!realfile->f_op->get_unmapped_area)
		return (unsigned long)-ENOSYS;
	return realfile->f_op->get_unmapped_area(realfile, addr, len, pgoff, flags);
}
#endif

static long ovl_fallocate('''
change('fs/overlayfs/file.c', '\nstatic long ovl_fallocate(', OVL, 'ovl_mmap_capabilities')
change('fs/overlayfs/file.c', '\t.mmap\t\t= ovl_mmap,\n',
       '\t.mmap\t\t= ovl_mmap,\n#ifndef CONFIG_MMU\n\t.mmap_capabilities = ovl_mmap_capabilities,\n\t.get_unmapped_area = ovl_get_unmapped_area,\n#endif\n',
       '.mmap_capabilities = ovl_mmap_capabilities')
change('fs/drop_caches.c', 'int drop_caches_sysctl_handler(',
       '#ifndef CONFIG_MMU\nvoid nommu_drop_caches(void)\n{\n\titerate_supers(drop_pagecache_sb, NULL);\n\tdrop_slab();\n}\n#endif\n\nint drop_caches_sysctl_handler(',
       'nommu_drop_caches')
change('mm/nommu.c', '\tbase = total >= 16 ? alloc_pages_run(total) : NULL;\n',
       '\tbase = total >= 16 ? alloc_pages_run(total) : NULL;\n\tif (!base && total >= 16) {\n\t\tnommu_drop_caches();\n\t\tbase = alloc_pages_run(total);\n\t}\n',
       'nommu_drop_caches();')
change('mm/nommu.c', 'void *alloc_pages_run(unsigned long nr);\n', 'void *alloc_pages_run(unsigned long nr);\nvoid nommu_drop_caches(void);\n',
       'void nommu_drop_caches(void);')
print('overlay and drop caches patched')
# a program loaded into RAM: its mapping came zeroed, and only the relocations read past the data need clearing
change('fs/binfmt_flat.c', '''	/* zero the BSS,  BRK and stack areas */
	if (clear_user(''', '''	/* zero the BSS,  BRK and stack areas */
#ifndef CONFIG_MMU
	if (flags & FLAT_FLAG_RAM && !(flags & (FLAT_FLAG_GZIP | FLAT_FLAG_GZDATA)) && !id)
		return clear_user((void __user *)(datapos + data_len), full_data - data_len) ? -EFAULT : 0;
#endif
	if (clear_user(''', '!(flags & (FLAT_FLAG_GZIP | FLAT_FLAG_GZDATA)) && !id)\n\t\treturn clear_user(')
print('binfmt_flat patched')
# a program whose file lies in memory (the ROM) is copied whole by one parallel copy, not a page at a time
DIRECT = '''#ifndef CONFIG_MMU
/* No MMU: a file that lies in memory copied by one parallel copy (as phram_read); the length copied, or 0 */
static unsigned long flat_copy_direct(struct file *file, unsigned long to, unsigned long len)
{
	unsigned long from, size = ALIGN(len, 16);

	if (!len || !file->f_op->get_unmapped_area || !file->f_op->mmap_capabilities ||
	    !(file->f_op->mmap_capabilities(file) & NOMMU_MAP_DIRECT))
		return 0;
	from = file->f_op->get_unmapped_area(file, 0, len, 0, MAP_SHARED);
	if (IS_ERR_VALUE(from) || (from & 15) || (to & 15))
		return 0;
	csr_write(0x0b1, from);
	csr_write(0x0b2, to);
	csr_write(0x0b3, size);
	csr_write(0x0b0, 1);
	return size;
}
#endif

static int load_flat_file('''
change('fs/binfmt_flat.c', '\nstatic int load_flat_file(', '\n' + DIRECT, 'flat_copy_direct(struct file')
change('fs/binfmt_flat.c', '''	int ret;

	hdr = ((struct flat_hdr *) bprm->buf);''', '''	int ret;
	unsigned long copied = 0;

	hdr = ((struct flat_hdr *) bprm->buf);''', 'unsigned long copied = 0;')
change('fs/binfmt_flat.c', '''		{
			result = read_code(bprm->file, textpos, 0, text_len);
			if (!IS_ERR_VALUE(result))
				result = read_code(bprm->file, datapos,
						   ntohl(hdr->data_start),
						   full_data);
		}''', '''		{
#ifndef CONFIG_MMU
			if (datapos - textpos == ntohl(hdr->data_start))
				copied = flat_copy_direct(bprm->file, textpos, text_len + full_data);
#endif
			result = 0;
			if (!copied)
				result = read_code(bprm->file, textpos, 0, text_len);
			if (!copied && !IS_ERR_VALUE(result))
				result = read_code(bprm->file, datapos,
						   ntohl(hdr->data_start),
						   full_data);
		}''', 'copied = flat_copy_direct(bprm->file')
# (what the copy's rounding brought past the relocations is cleared with them)
change('fs/binfmt_flat.c', 'return clear_user((void __user *)(datapos + data_len), full_data - data_len) ? -EFAULT : 0;',
       'return clear_user((void __user *)(datapos + data_len),\n\t\t\t\t  max(textpos + copied, datapos + full_data) - (datapos + data_len)) ? -EFAULT : 0;',
       'max(textpos + copied, datapos + full_data)')
print('binfmt_flat: one copy')
