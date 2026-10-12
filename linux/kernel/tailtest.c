/* tailtest FILE...: a mapped file's last page is zero past the file's end, and its bytes a sum. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    unsigned long files = 0, wrong = 0, sum = 0, tails = 0;
    int k;
    for (k = 1; k < argc; k++) {
        struct stat st;
        int fd = open(argv[k], O_RDONLY);
        size_t pages, i;
        unsigned char *p;
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || !st.st_size) { if (fd >= 0) close(fd); continue; }
        pages = ((size_t)st.st_size + 4095) & ~(size_t)4095;
        p = mmap(0, pages, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) continue;
        files++;
        for (i = 0; i < (size_t)st.st_size; i++) sum = sum * 31 + p[i];
        for (; i < pages; i++, tails++)
            if (p[i]) { if (wrong++ < 5) printf("tailtest: %s byte %lu past its end is %02x\n", argv[k], (unsigned long)i, p[i]); }
        munmap(p, pages);
    }
    printf("tailtest: %s, %lu files, %lu bytes past their ends, %lu not zero, sum %08lx\n", wrong ? "FAIL" : "PASS", files, tails, wrong, sum);
    return wrong != 0;
}
