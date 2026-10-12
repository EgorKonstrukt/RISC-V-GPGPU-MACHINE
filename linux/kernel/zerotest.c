/* zerotest [MB [ROUNDS]]: memory the kernel hands out as zero is zero, after memory with other
 * contents was used and freed: anonymous pages, tmpfs pages (mapped and read), and a child's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

static unsigned long bad;

static void dirty(size_t bytes, unsigned seed)
{
    unsigned *p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t i;
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    for (i = 0; i < bytes / 4; i++) p[i] = 0xa5a50000u + seed + (unsigned)i;
    munmap(p, bytes);
}

static void check(const void *at, size_t bytes, const char *what)
{
    const unsigned *p = at;
    size_t i;
    for (i = 0; i < bytes / 4; i++)
        if (p[i]) { if (bad++ < 8) printf("zerotest: %s word %lu is %08x\n", what, (unsigned long)i, p[i]); }
}

int main(int argc, char **argv)
{
    size_t mb = argc > 1 ? atoi(argv[1]) : 12, bytes = mb << 20;
    int rounds = argc > 2 ? atoi(argv[2]) : 3, r, fd, status;
    static unsigned buf[4096];
    for (r = 0; r < rounds; r++) {
        void *p;
        ssize_t n;
        dirty(bytes, r);
        /* a tmpfs file of other contents, deleted */
        fd = open("/tmp/zt.dirty", O_RDWR | O_CREAT | O_TRUNC, 0600);
        memset(buf, 0x5a, sizeof buf);
        for (n = 0; n < (ssize_t)(bytes / 2); n += sizeof buf) if (write(fd, buf, sizeof buf) < 0) break;
        close(fd);
        unlink("/tmp/zt.dirty");
        /* fresh anonymous memory */
        p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ((volatile char *)p)[bytes / 2] = 0;
        check(p, bytes, "anonymous");
        memset(p, 0x77, bytes);
        munmap(p, bytes);
        /* a fresh tmpfs file: mapped, then read */
        fd = open("/tmp/zt.fresh", O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (ftruncate(fd, bytes / 2) < 0) perror("ftruncate");
        p = mmap(0, bytes / 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ((volatile char *)p)[4096 * 3] = 0;
        check(p, bytes / 2, "tmpfs mapped");
        munmap(p, bytes / 2);
        if (pwrite(fd, "x", 1, bytes / 2 + 4096 * 9 + 5) < 0) perror("pwrite");
        lseek(fd, bytes / 2, SEEK_SET);
        n = read(fd, buf, 4096 * 9);
        check(buf, n > 0 ? n : 0, "tmpfs read");
        close(fd);
        unlink("/tmp/zt.fresh");
        /* a child's: its stack, page tables and heap come from the same place */
        if (fork() == 0) {
            unsigned *q = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (q == MAP_FAILED) _exit(3);
            check(q, 1 << 20, "child");
            _exit(bad ? 1 : 0);
        }
        wait(&status);
        if (status) bad++;
        if (r & 1) usleep(300000);   /* (the idle loop clears ahead meanwhile) */
    }
    printf("zerotest: %s, %lu MB in %d rounds, %lu wrong\n", bad ? "FAIL" : "PASS", (unsigned long)mb, rounds, bad);
    return bad != 0;
}
