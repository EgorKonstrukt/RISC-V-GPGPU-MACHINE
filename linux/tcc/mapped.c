/* mapped.c: the libraries TinyCC links, read from memory. Linked into the guest's tcc under
 * its open, read, lseek and close (build.sh: --wrap). The linker takes an archive's members a
 * section at a time, a seek and a read each: 4,000 system calls for the smallest program.
 * A file ending in .a or .o is mapped when it is opened, and those calls copy from it. */
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
ssize_t __real_read(int fd, void *to, size_t count);
off_t __real_lseek(int fd, off_t offset, int whence);
int __real_close(int fd);

#define MOST 32
#define WINDOWS 8
/* An archive as pack.py left it, what programs take of it first: its length, and the page
 * each window of that part ends at (docs/cc.md, "Fewer pages of the ROM"). */
static const struct packed { const char *name; size_t size; unsigned short ends[WINDOWS]; } packed[] = {
#include "packed.h"
};
/* by descriptor; bytes NULL: not mapped. ends: the windows not asked for yet are the bits of left */
static struct { char *bytes; size_t size, at; const unsigned short *ends; unsigned left; } mapped[MOST];

/* Before `count` bytes at `from` are read: the windows they are in, each one copy from the ROM. */
static void
wanted(int fd, size_t from, size_t count)
{
	const unsigned short *ends = mapped[fd].ends;
	size_t first = from >> 12, last = (from + count - 1) >> 12, start = 0;
	int k;

	for (k = 0; k < WINDOWS && ends[k]; start = ends[k++]) {
		if (!(mapped[fd].left >> k & 1) || first >= ends[k] || last < start)
			continue;
		mapped[fd].left &= ~(1u << k);
		/* (a page by itself is as well read by its fault) */
		if (ends[k] - start > 1)
			madvise(mapped[fd].bytes + (start << 12), (size_t)(ends[k] - start) << 12, MADV_WILLNEED);
	}
}

int
__wrap_open(const char *path, int flags, ...)
{
	size_t length = strlen(path);
	struct stat info;
	va_list more;
	int mode, fd;
	unsigned k;
	void *bytes;

	va_start(more, flags);
	mode = (flags & O_CREAT) ? va_arg(more, int) : 0;
	va_end(more);
	fd = __real_open(path, flags, mode);
	if (fd < 0 || fd >= MOST || (flags & O_ACCMODE) != O_RDONLY || length < 2 || path[length - 2] != '.' ||
	    (path[length - 1] != 'a' && path[length - 1] != 'o'))
		return fd;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
		return fd;
	bytes = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (bytes == MAP_FAILED)
		return fd;
	/* (a member here and a member there: nothing is gained by reading ahead of a fault) */
	madvise(bytes, (size_t)info.st_size, MADV_RANDOM);
	mapped[fd].bytes = bytes;
	mapped[fd].size = (size_t)info.st_size;
	mapped[fd].at = 0;
	mapped[fd].left = 0;
	for (k = 0; k < sizeof packed / sizeof packed[0]; k++) {
		size_t n = strlen(packed[k].name);

		if (packed[k].size == mapped[fd].size && length > n && path[length - n - 1] == '/' && !strcmp(path + length - n, packed[k].name)) {
			mapped[fd].ends = packed[k].ends;
			for (n = 0; n < WINDOWS && packed[k].ends[n]; n++)
				mapped[fd].left |= 1u << n;
		}
	}
	return fd;
}

ssize_t
__wrap_read(int fd, void *to, size_t count)
{
	size_t left;

	if (fd < 0 || fd >= MOST || !mapped[fd].bytes)
		return __real_read(fd, to, count);
	left = mapped[fd].at < mapped[fd].size ? mapped[fd].size - mapped[fd].at : 0;
	count = count < left ? count : left;
	if (mapped[fd].left && count)
		wanted(fd, mapped[fd].at, count);
	memcpy(to, mapped[fd].bytes + mapped[fd].at, count);
	mapped[fd].at += count;
	return (ssize_t)count;
}

/* The next `count` bytes of a mapped file where they are, for a reader that only looks at
 * them (an archive's index); NULL when the file is not mapped or has not so many. */
void *
tcc_mapped(int fd, size_t count)
{
	char *here;

	if (fd < 0 || fd >= MOST || !mapped[fd].bytes || mapped[fd].at > mapped[fd].size || count > mapped[fd].size - mapped[fd].at)
		return NULL;
	if (mapped[fd].left && count)
		wanted(fd, mapped[fd].at, count);
	here = mapped[fd].bytes + mapped[fd].at;
	mapped[fd].at += count;
	return here;
}

off_t
__wrap_lseek(int fd, off_t offset, int whence)
{
	off_t to;

	if (fd < 0 || fd >= MOST || !mapped[fd].bytes)
		return __real_lseek(fd, offset, whence);
	to = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (off_t)mapped[fd].at + offset : (off_t)mapped[fd].size + offset;
	if (to < 0)
		return -1;
	mapped[fd].at = (size_t)to;
	return to;
}

int
__wrap_close(int fd)
{
	if (fd >= 0 && fd < MOST && mapped[fd].bytes) {
		munmap(mapped[fd].bytes, mapped[fd].size);
		mapped[fd].bytes = NULL;
	}
	return __real_close(fd);
}
