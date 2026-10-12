"""Makes romfs read a directory's entries and names straight from the ROM's mapping. A lookup
walks its directory an entry at a time (our files are at the end of each), and every entry
was two or three reads of 16 bytes through the MTD layer's checks. Page-sized reads stay with
the driver, which copies them in the machine's commit pass (phram_hook.py). Idempotent."""
import sys
path = sys.argv[1] + '/fs/romfs/storage.c'
s = open(path).read()
if 'ShaderEmu' not in s:
    old = '#define ROMFS_MTD_READ(sb, ...) mtd_read((sb)->s_mtd, ##__VA_ARGS__)\n'
    new = '''#define ROMFS_MTD_READ(sb, ...) romfs_rom_read((sb)->s_mtd, ##__VA_ARGS__)

/* ShaderEmu: the device's memory, when it is memory (NULL if not), and how much of it. */
static const u_char *romfs_rom_of(struct mtd_info *mtd, size_t *size)
{
	static struct mtd_info *asked;
	static u_char *rom;
	static size_t rom_size;

	if (asked != mtd) {
		void *virt = NULL;
		size_t got = 0;

		rom = NULL;
		if (mtd_point(mtd, 0, mtd->size, &got, &virt, NULL) == 0 && virt) {
			rom = virt;
			rom_size = got;
		}
		asked = mtd;
	}
	*size = rom_size;
	return rom;
}

const u_char *romfs_rom(struct super_block *sb, size_t *size)
{
	return sb->s_mtd ? romfs_rom_of(sb->s_mtd, size) : NULL;
}

/* A short read is a copy from that memory. */
static int romfs_rom_read(struct mtd_info *mtd, loff_t pos, size_t len, size_t *retlen, u_char *buf)
{
	if (len <= 64) {
		size_t size;
		const u_char *rom = romfs_rom_of(mtd, &size);

		if (rom && pos >= 0 && pos + len <= size) {
			memcpy(buf, rom + pos, len);
			*retlen = len;
			return 0;
		}
	}
	return mtd_read(mtd, pos, len, retlen, buf);
}
'''
    assert s.count(old) == 1
    open(path, 'w').write(s.replace(old, new))

    path = sys.argv[1] + '/fs/romfs/internal.h'
    s = open(path).read()
    old = 'extern int romfs_dev_read(struct super_block *sb, unsigned long pos,\n'
    assert s.count(old) == 1
    open(path, 'w').write(s.replace(old, 'extern const u_char *romfs_rom(struct super_block *sb, size_t *size);\n' + old))

    path = sys.argv[1] + '/fs/romfs/super.c'
    s = open(path).read()
    old = '''	name = dentry->d_name.name;
	len = dentry->d_name.len;

	for (;;) {
'''
    new = '''	name = dentry->d_name.name;
	len = dentry->d_name.len;

#ifdef CONFIG_ROMFS_ON_MTD
	{
		/* ShaderEmu: the entries are compared where they lie in the ROM */
		size_t size;
		const u_char *rom = romfs_rom(dir->i_sb, &size);

		while (rom && len > 0 && offset && offset < maxoff && offset + ROMFH_SIZE + len + 1 <= size) {
			const u_char *entry = rom + offset;
			u32 next = be32_to_cpup((const __be32 *)entry);

			if (entry[ROMFH_SIZE] == (u_char)name[0] && !entry[ROMFH_SIZE + len] &&
			    !memcmp(entry + ROMFH_SIZE, name, len)) {
				if ((next & ROMFH_TYPE) == ROMFH_HRD)
					offset = be32_to_cpup((const __be32 *)entry + 1) & ROMFH_MASK;
				return d_splice_alias(romfs_iget(dir->i_sb, offset), dentry);
			}
			offset = next & ROMFH_MASK;
		}
	}
#endif

	for (;;) {
'''
    assert s.count(old) == 1
    open(path, 'w').write(s.replace(old, new))
    print('romfs hook added')
