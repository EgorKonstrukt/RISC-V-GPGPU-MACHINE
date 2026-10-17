"""TinyCC's stores in bursts, which ended the machine's passes early (docs/system/cc.md). Idempotent.

A pass keeps about 6 KB of new stores: a buffer cleared whole, a section copied to grow it
and a table copied back after sorting each ended several. After speed_hook.py.
"""
import sys

src = sys.argv[1]


def patch(name, edits):
    path = src + '/' + name
    s = open(path).read()
    if 'ShaderEmu: passes' in s:
        return
    for old, new in edits:
        assert s.count(old) == 1, old
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print(name + ': patched for passes')


patch('libtcc.c', [
    ('''    bf = tcc_mallocz(sizeof(BufferedFile) + buflen);
    bf->buf_ptr = bf->buffer;''', '''    /* ShaderEmu: passes. The buffer is read into, not cleared first. */
    bf = tcc_malloc(sizeof(BufferedFile) + buflen);
    memset(bf, 0, offsetof(BufferedFile, filename));
    memset(bf->unget, 0, sizeof bf->unget);
    bf->buf_ptr = bf->buffer;'''),
])

patch('tccpp.c', [
    ('''    memset(hash_ident, 0, TOK_HASH_SIZE * sizeof(TokenSym *));
''', '''    {
        /* ShaderEmu: passes. The table starts as zeros: cleared only once it was used. */
        static int used;

        if (used)
            memset(hash_ident, 0, TOK_HASH_SIZE * sizeof(TokenSym *));
        used = 1;
    }
'''),
])

patch('tccelf.c', [
    # a section past a few kilobytes is pages of its own: grown by the kernel moving them
    ('''ST_FUNC void free_section(Section *s)
{
    if (!s)
        return;
    tcc_free(s->data);
''', '''/* ShaderEmu: passes. A section's data of SECTION_PAGES bytes or more is pages of its own:
   it grows without a copy, and what is new of it is zeros without being cleared. */
#if defined __linux__ && !defined TCC_IS_NATIVE_PE
# include <sys/mman.h>
# define SECTION_PAGES 4096
# define SECTION_FIRST 65536
#endif

static void section_full(void)
{
    fputs("tcc: memory full (section)", stderr);
    fputc(10, stderr);
    exit(1);
}

/* room for a section's data by that rule (not cleared when it is not pages) */
static unsigned char *section_room(unsigned long *size)
{
#ifdef SECTION_PAGES
    if (*size >= SECTION_PAGES) {
        void *data;

        if (*size < SECTION_FIRST)
            *size = SECTION_FIRST;
        *size = (*size + 4095) & ~4095ul;
        data = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (data == MAP_FAILED)
            section_full();
        return data;
    }
#endif
    return tcc_malloc(*size);
}

static void section_room_free(unsigned char *data, unsigned long size)
{
#ifdef SECTION_PAGES
    if (data && size >= SECTION_PAGES) {
        munmap(data, size);
        return;
    }
#endif
    tcc_free(data);
}

ST_FUNC void free_section(Section *s)
{
    if (!s)
        return;
    section_room_free(s->data, s->data_allocated);
'''),
    ('''    while (size < new_size)
        size = size * 2;
    data = tcc_realloc(sec->data, size);
    memset(data + sec->data_allocated, 0, size - sec->data_allocated);
    sec->data = data;
    sec->data_allocated = size;
''', '''    while (size < new_size)
        size = size * 2;
#ifdef SECTION_PAGES
    if (size >= SECTION_PAGES) {
        if (sec->data_allocated >= SECTION_PAGES) {
            unsigned long wanted = size;

            /* (four times: the pages are there only as far as they are written) */
            while (size < sec->data_allocated * 4)
                size = size * 2;
            data = mremap(sec->data, sec->data_allocated, size, MREMAP_MAYMOVE);
            if (data == MAP_FAILED) {
                /* no MMU: a mapping does not grow, and every page of it is memory */
                size = wanted;
                data = section_room(&size);
                memcpy(data, sec->data, sec->data_allocated);
                munmap(sec->data, sec->data_allocated);
            }
        } else {
            data = section_room(&size);
            if (sec->data_allocated)
                memcpy(data, sec->data, sec->data_allocated);
            tcc_free(sec->data);
        }
        sec->data = data;
        sec->data_allocated = size;
        return;
    }
#endif
    data = tcc_realloc(sec->data, size);
    memset(data + sec->data_allocated, 0, size - sec->data_allocated);
    sec->data = data;
    sec->data_allocated = size;
'''),
    # the sorted symbols become the section's: they were copied back over the old ones
    ('''    ElfW(Sym) *new_syms;
    int nb_syms, i;
    ElfW(Sym) *p, *q;

    nb_syms = s->data_offset / sizeof(ElfW(Sym));
    new_syms = tcc_malloc(nb_syms * sizeof(ElfW(Sym)));
''', '''    ElfW(Sym) *new_syms;
    int nb_syms, i;
    ElfW(Sym) *p, *q;
    unsigned long room;

    nb_syms = s->data_offset / sizeof(ElfW(Sym));
    /* ShaderEmu: passes. With no relocations to number again the table stays as it is, and
       put_syms() writes it in the order: sorting it was a store to every symbol. */
    for (i = 1; i < s1->nb_sections; i++)
        if (s1->sections[i]->sh_type == SHT_RELX && s1->sections[i]->link == s)
            break;
    if (i == s1->nb_sections && s->sh_size == s->data_offset) {
        for (i = 0, room = 0, p = (ElfW(Sym) *)s->data; i < nb_syms; i++, p++)
            room += ELFW(ST_BIND)(p->st_info) == STB_LOCAL;
        if (s->sh_size)
            s->sh_info = room;
        syms_unsorted = s;
        return;
    }
    room = nb_syms * sizeof(ElfW(Sym));
    new_syms = (ElfW(Sym) *)section_room(&room);
'''),
    ('''static void sort_syms(TCCState *s1, Section *s)
{''', '''static Section *syms_unsorted;    /* its symbols are written local ones first by put_syms() */

/* The symbols of a table as sort_syms() would have left them: the local ones, then the
   others, each in the order they have, a run of them a write. */
static int put_syms(FILE *f, Section *s)
{
    ElfW(Sym) *p = (ElfW(Sym) *)s->data;
    int nb_syms = s->data_offset / sizeof(ElfW(Sym)), pass, i, j, local;

    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < nb_syms; i = j) {
            local = ELFW(ST_BIND)(p[i].st_info) == STB_LOCAL;
            for (j = i + 1; j < nb_syms && (ELFW(ST_BIND)(p[j].st_info) == STB_LOCAL) == local; j++)
                ;
            if (local == !pass)
                fwrite(p + i, sizeof *p, j - i, f);
        }
    return nb_syms * sizeof *p;
}

static void sort_syms(TCCState *s1, Section *s)
{'''),
    ('''    /* we copy the new symbols to the old */
    memcpy(s->data, new_syms, nb_syms * sizeof(ElfW(Sym)));
    tcc_free(new_syms);
''', '''    /* ShaderEmu: passes. the new symbols are the section's now (they were copied back) */
    section_room_free(s->data, s->data_allocated);
    s->data = (unsigned char *)new_syms;
    s->data_allocated = room;
'''),
    # an archive's index where the archive is mapped (linux/tcc/mapped.c)
    ('''static int tcc_load_alacarte(TCCState *s1, int fd, int size, int entrysize)
{''', '''/* ShaderEmu: passes. The next bytes of a file that is mapped, where they are (mapped.c;
   not there in a build without it). */
void *tcc_mapped(int fd, size_t count) __attribute__((weak));

static int tcc_load_alacarte(TCCState *s1, int fd, int size, int entrysize)
{
    int index_mapped = 0;'''),
    ('''    data = tcc_malloc(size);
    if (full_read(fd, data, size) != size)
        goto invalid;
    nsyms = get_be(data, entrysize);''', '''    if (tcc_mapped && (data = tcc_mapped(fd, size)) != NULL) {
        index_mapped = 1;
    } else {
        data = tcc_malloc(size);
        if (full_read(fd, data, size) != size)
            goto invalid;
    }
    nsyms = get_be(data, entrysize);'''),
    ('''    tcc_free(known);
    tcc_free(data);
    return ret;''', '''    tcc_free(known);
    if (!index_mapped)
        tcc_free(data);
    return ret;'''),
    # the zeros between a file's parts as one write each, not a call a byte
    ('''static int tcc_output_elf(TCCState *s1, FILE *f, int phnum, ElfW(Phdr) *phdr''', '''/* ShaderEmu: passes. `count` zeros. */
static int put_zeros(FILE *f, int count)
{
    static const char zeros[256];
    int left;

    for (left = count; left > 0; left -= sizeof zeros)
        fwrite(zeros, 1, left < (int)sizeof zeros ? left : (int)sizeof zeros, f);
    return count > 0 ? count : 0;
}

static int tcc_output_elf(TCCState *s1, FILE *f, int phnum, ElfW(Phdr) *phdr'''),
    ('''    while (offset < ehdr.e_shoff) {
        fputc(0, f);
        offset++;
    }
''', '''    offset += put_zeros(f, ehdr.e_shoff - offset);
'''),
    ('''        if (s->sh_type != SHT_NOBITS) {
            while (offset < s->sh_offset) {
                fputc(0, f);
                offset++;
            }
            size = s->sh_size;
            if (size)''', '''        if (s->sh_type != SHT_NOBITS) {
            offset += put_zeros(f, s->sh_offset - offset);
            size = s->sh_size;
            if (size && s == syms_unsorted && size == s->data_offset)
                offset += put_syms(f, s), syms_unsorted = NULL;
            else if (size)'''),
])

patch('tcc.h', [
    ('''#define IO_BUF_SIZE 8192''', '''/* ShaderEmu: passes. (a read of 8 KB was more new stores than a pass keeps) */
#define IO_BUF_SIZE 4096'''),
])

patch('tccrun.c', [
    ('''                s->data = tcc_realloc(s->data, s->data_allocated = s->data_offset);''',
     '''                /* ShaderEmu: passes. (a section that is pages stays as it is) */
                if (s->data_allocated < 4096)
                    s->data = tcc_realloc(s->data, s->data_allocated = s->data_offset);'''),
])
