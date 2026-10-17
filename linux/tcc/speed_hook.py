"""Two things TinyCC spent a compile on here (docs/system/cc.md). Idempotent.

A number with a point went through the C library's strtof, whose long doubles are library
calls on this machine: literal.h reads the short ones itself. And an archive's symbols were
looked up by name at every pass over it: their hashes and places are kept for the passes.
"""
import os, sys

src = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))


def patch(name, edits):
    path = src + '/' + name
    s = open(path).read()
    if 'ShaderEmu: speed' in s:
        return
    for old, new in edits:
        assert s.count(old) == 1, old
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print(name + ': patched for speed')


literal = open(os.path.join(here, 'literal.h')).read()
patch('tccpp.c', [
    ('''static void parse_number(const char *p)
{''', '''/* ShaderEmu: speed (linux/tcc/literal.h) */
''' + literal + '''
static void parse_number(const char *p)
{'''),
    ('''                tokc.f = strtof(token_buf, NULL);''',
     '''                if (!literal_float(token_buf, &tokc.f))
                    tokc.f = strtof(token_buf, NULL);'''),
    ('''            } else {
                tok = TOK_CDOUBLE;
                tokc.d = strtod(token_buf, NULL);
            }''', '''            } else {
                tok = TOK_CDOUBLE;
                if (!literal_double(token_buf, &tokc.d))
                    tokc.d = strtod(token_buf, NULL);
            }'''),
])

patch('tccelf.c', [
    ('''static int tcc_load_alacarte(TCCState *s1, int fd, int size, int entrysize)
{''', '''/* ShaderEmu: speed. find_elf_sym with the name's hash worked out already. */
static int find_elf_sym_hashed(Section *s, const char *name, ElfW(Word) hash)
{
    Section *hs = s->hash;
    int nbuckets, sym_index;

    if (!hs)
        return 0;
    nbuckets = ((int *)hs->data)[0];
    sym_index = ((int *)hs->data)[2 + hash % nbuckets];
    while (sym_index != 0) {
        ElfW(Sym) *sym = &((ElfW(Sym) *)s->data)[sym_index];

        if (!strcmp(name, (char *) s->link->data + sym->st_name))
            return sym_index;
        sym_index = ((int *)hs->data)[2 + nbuckets + sym_index];
    }
    return 0;
}

static int tcc_load_alacarte(TCCState *s1, int fd, int size, int entrysize)
{
    /* each name's hash and place, and whether it is defined by now (it stays so) */
    struct { ElfW(Word) hash; int at, done; } *known = NULL;'''),
    ('''    ar_names = (char *) ar_index + nsyms * entrysize;

    do {
        bound = 0;
        for (p = ar_names, i = 0; i < nsyms; i++, p += strlen(p)+1) {
            Section *s = symtab_section;
            sym_index = find_elf_sym(s, p);
            if (!sym_index)
                continue;
            sym = &((ElfW(Sym) *)s->data)[sym_index];
            if(sym->st_shndx != SHN_UNDEF)
                continue;''', '''    ar_names = (char *) ar_index + nsyms * entrysize;
    known = tcc_malloc((nsyms + 1) * sizeof *known);
    for (p = ar_names, i = 0; i < nsyms; i++, p += strlen(p)+1) {
        known[i].hash = elf_hash((unsigned char *) p);
        known[i].at = (int)(p - ar_names);
        known[i].done = 0;
    }

    do {
        bound = 0;
        for (i = 0; i < nsyms; i++) {
            Section *s = symtab_section;
            if (known[i].done)
                continue;
            p = ar_names + known[i].at;
            sym_index = find_elf_sym_hashed(s, p, known[i].hash);
            if (!sym_index)
                continue;
            sym = &((ElfW(Sym) *)s->data)[sym_index];
            if(sym->st_shndx != SHN_UNDEF) {
                known[i].done = 1;
                continue;
            }'''),
    ('''    ret = 0;
 the_end:
    tcc_free(data);
    return ret;''', '''    ret = 0;
 the_end:
    tcc_free(known);
    tcc_free(data);
    return ret;'''),
])
