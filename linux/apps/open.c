/*
 * open [--dry] ADDRESS-OR-FILE: opens a file, or what an address holds, with the program
 * registered for its kind (docs/open.md). --dry prints the kind and the command only.
 * open --unpack zip|tar|tgz|gz FILE TO: an archive unpacked, with its progress (nxopen-archive).
 */
#include <signal.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "nano-X.h"
#include "ui.h"
#include "ui_progress.h"

#define FETCH_MOST 0x40000	/* bytes the host hands over at a time (docs/fetch.md) */
#define REG_ASKED 64		/* requests made, the address's length, the kind, the offset */
#define REG_REPLY 68		/* requests answered, bytes in all, status, a picture's size */
#define REG_ADDRESS 72
#define KIND_FILE 3
#define HEADER 256		/* of a file a picture carried: mark, length, CRC-32, name */
#define FOLDER "/tmp/open"
#define MAX_KINDS 32

static struct kind {
	char name[16], endings[96], magic[128], command[96];
} kinds[MAX_KINDS];
static int kind_count;
static volatile uint32_t *regs;
static const unsigned char *fetched;
static unsigned long long fetch_total;	/* the answer's size, once the host has said it */

static uint32_t
crc32(const unsigned char *p, size_t n, uint32_t crc)
{
	int k;

	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

static int
registry_entry(const struct dirent *entry)
{
	return !strncmp(entry->d_name, "nxopen.", 7);
}

/* A word of a line into `to` (of `room` bytes), and the place after it and its spaces; NULL
 * when the line has no word there, or one too long for a word of the registry. */
static const char *
word_of(const char *at, char *to, int room)
{
	int n = 0;

	while (*at == ' ' || *at == '\t')
		at++;
	for (; (unsigned char)*at > ' '; at++) {
		if (n == room - 1)
			return NULL;
		to[n++] = *at;
	}
	to[n] = 0;
	while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')
		at++;
	return n ? at : NULL;
}

/* The registry: every line of every /usr/share/nxopen.* file is "kind endings magic command".
 * (Only those files' names are kept and put in order: the folder has a hundred others.) */
static void
read_kinds(void)
{
	struct dirent **names;
	int n = scandir("/usr/share", &names, registry_entry, alphasort), i;

	for (i = 0; i < n; i++) {
		char path[300], line[400];
		FILE *file;

		memcpy(path, "/usr/share/", 11);
		strcpy(path + 11, names[i]->d_name);
		if ((file = fopen(path, "r")) == NULL)
			continue;
		while (fgets(line, sizeof line, file) && kind_count < MAX_KINDS) {
			struct kind *k = &kinds[kind_count];
			const char *at = line;
			size_t length;

			if (line[0] == '#' || !(at = word_of(at, k->name, sizeof k->name)) || !(at = word_of(at, k->endings, sizeof k->endings)) ||
			    !(at = word_of(at, k->magic, sizeof k->magic)))
				continue;
			length = strcspn(at, "\n");
			length = length < sizeof k->command - 1 ? length : sizeof k->command - 1;
			memcpy(k->command, at, length);
			k->command[length] = 0;
			if (k->command[0])
				kind_count++;
		}
		fclose(file);
	}
}

/* True if the bytes begin with one of a kind's marks: a list with commas, \xNN for a byte.
 * A mark that begins with '<' is a page's tag, compared in either case; any other is bytes. */
static int
has_magic(const struct kind *k, const unsigned char *head, int n)
{
	const char *m = k->magic;

	if (!strcmp(m, "-"))
		return 0;
	while (*m) {
		int at = 0, same = 1, tag = *m == '<';

		while (*m && *m != ',') {
			int c = (unsigned char)*m++;

			if (c == '\\' && *m == 'x' && isxdigit((unsigned char)m[1]) && isxdigit((unsigned char)m[2])) {
				char hex[3] = { m[1], m[2], 0 };

				c = (int)strtol(hex, NULL, 16);
				m += 3;
				if (at >= n || head[at] != c)
					same = 0;
			} else if (at >= n || (tag ? tolower(head[at]) != tolower(c) : head[at] != c))
				same = 0;
			at++;
		}
		if (same && at > 0)
			return 1;
		if (*m == ',')
			m++;
	}
	return 0;
}

static int
has_ending(const struct kind *k, const char *name)
{
	const char *dot = strrchr(name, '.'), *e = k->endings;
	size_t n;

	if (!dot || !strcmp(e, "-"))
		return 0;
	n = strlen(++dot);
	while (*e) {
		size_t len = strcspn(e, ",");

		if (len == n && !strncasecmp(e, dot, n))
			return 1;
		e += len + (e[len] == ',');
	}
	return 0;
}

/* A file's kind: by its first bytes where a kind's marks say, else by its name's ending. */
static const struct kind *
kind_of(const char *path, const unsigned char *head, int n)
{
	int i, skip = 0;

	for (i = 0; i < kind_count; i++)
		if (has_magic(&kinds[i], head, n))
			return &kinds[i];
	/* (a page may begin with a byte order mark and blank lines) */
	if (n >= 3 && !memcmp(head, "\xef\xbb\xbf", 3))
		skip = 3;
	while (skip < n && isspace(head[skip]))
		skip++;
	for (i = 0; skip > 0 && i < kind_count; i++)
		if (has_magic(&kinds[i], head + skip, n - skip))
			return &kinds[i];
	for (i = 0; i < kind_count; i++)
		if (has_ending(&kinds[i], path))
			return &kinds[i];
	return NULL;
}

/* Asks the host for a part of what an address holds; the answer's status, or 0 for none. */
static int
ask(const char *address, uint32_t offset, int seconds)
{
	uint32_t mine;
	int waited;
	size_t n = strlen(address);

	/* one request at a time: wait for another program's to be answered */
	for (waited = 0; regs[REG_ASKED] != regs[REG_REPLY] && waited < 200; waited++)
		usleep(50000);
	if (n > 255)
		n = 255;
	memset((void *)&regs[REG_ADDRESS], 0, 256);
	memcpy((void *)&regs[REG_ADDRESS], address, n);
	regs[REG_ASKED + 1] = n;
	regs[REG_ASKED + 2] = KIND_FILE;
	regs[REG_ASKED + 3] = offset;
	mine = regs[REG_ASKED] + 1;
	regs[REG_ASKED] = mine;
	for (waited = 0; regs[REG_REPLY] != mine; waited++) {
		if (waited > seconds * 20)
			return 0;
		if (!fetch_total && waited % 10 == 0) {
			char line[48];

			snprintf(line, sizeof line, "Waiting for the host: %d s", waited / 20);
			progress_show(0, 0, line);
		}
		usleep(50000);
	}
	return regs[REG_REPLY + 2];
}

/* The last piece of an address's path, without what follows a question mark, as a file's name. */
static void
name_from(const char *address, char *name, size_t room)
{
	const char *end = address + strcspn(address, "?#"), *from = end;
	size_t i, n;

	while (from > address && from[-1] != '/')
		from--;
	n = (size_t)(end - from) < room - 1 ? (size_t)(end - from) : room - 1;
	for (i = 0; i < n; i++)
		name[i] = isalnum((unsigned char)from[i]) || strchr("._-", from[i]) ? from[i] : '_';
	name[n] = 0;
	if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
		snprintf(name, room, "download");
}

/* What an address holds, into a file under FOLDER whose path is returned in `path`; 0, with a
 * line said, if it did not come. */
static int
fetch(const char *address, char *path, size_t room)
{
	char name[200];
	uint32_t total, got = 0, skip = 0, crc = 0, want_crc = 0;
	int status, tries, fd, packed, by_hand = 0;
	FILE *file;

	fd = open("/dev/gpu", O_RDWR);
	if (fd < 0 || (regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000)) == MAP_FAILED ||
	    (fetched = mmap(NULL, FETCH_MOST, PROT_READ, MAP_SHARED, fd, 0x016c0000)) == MAP_FAILED) {
		printf("open: this machine has no way to ask its host for a file\n");
		return 0;
	}
	name_from(address, name, sizeof name - 4);
	snprintf(path, room, "Loading %s", name);
	progress_begin(path);
	/* (a world may not have been handed the address yet: asked again for a minute) */
	for (tries = 0; (status = ask(address, 0, 40)) == 1 && tries < 20; tries++) {
		if (!by_hand++)
			printf("open: the host has to be handed this address first\n");
		sleep(3);
	}
	if (status != 200 && status != 201) {
		printf("open: the host could not get %s (%d)\n", address, status);
		return 0;
	}
	total = regs[REG_REPLY + 1];
	fetch_total = total;
	packed = status == 201;
	mkdir(FOLDER, 0777);
	if (!packed && regs[REG_REPLY + 3] != 0) {
		/* a picture the host could only give as pixels: a word each, in parts of 65,536,
		 * kept as a PPM file (a part's bytes made first and written at once: a byte a call
		 * was 76 instructions a pixel, this is 12) */
		static uint8_t rgb[FETCH_MOST / 4 * 3];
		uint32_t w = regs[REG_REPLY + 3] & 0xffff, h = regs[REG_REPLY + 3] >> 16, i, done = 0;

		name_from(address, name, sizeof name - 4);
		snprintf(path, room, FOLDER "/%s.ppm", name);
		if (w == 0 || h == 0 || (file = fopen(path, "wb")) == NULL) {
			printf("open: the picture of %s cannot be kept\n", address);
			return 0;
		}
		fprintf(file, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
		for (;;) {
			uint32_t here = w * h - done < FETCH_MOST / 4 ? w * h - done : FETCH_MOST / 4;

			uint8_t *to = rgb;

			for (i = 0; i < here; i++, to += 3) {
				uint32_t c = ((const uint32_t *)fetched)[i];

				to[0] = c >> 16, to[1] = c >> 8, to[2] = c;
			}
			fwrite(rgb, 3, here, file);
			done += here;
			progress_show(done, (unsigned long long)w * h, progress_megabytes(done * 3ull, w * h * 3ull));
			if (done >= w * h)
				break;
			status = ask(address, done * 4, 40);
			if (status != 200 || regs[REG_REPLY + 3] != (w | h << 16)) {
				printf("open: the host stopped at %u of %u pixels (%d)\n", (unsigned)done, (unsigned)(w * h), status);
				fclose(file);
				return 0;
			}
		}
		fclose(file);
		printf("open: %s, %u x %u\n", path, (unsigned)w, (unsigned)h);
		return 1;
	}
	if (packed) {
		/* a file a picture carried: its own name, and a sum to check it by */
		char own[237];

		if (total < HEADER)
			return 0;
		memcpy(&want_crc, fetched + 16, 4);
		memcpy(own, fetched + 20, 236);
		own[236] = 0;
		name_from(own, name, sizeof name);
		skip = HEADER;
	} else
		name_from(address, name, sizeof name);
	snprintf(path, room, FOLDER "/%s", name);
	if ((file = fopen(path, "wb")) == NULL) {
		printf("open: %s cannot be written\n", path);
		return 0;
	}
	for (;;) {
		uint32_t here = total - got < FETCH_MOST ? total - got : FETCH_MOST;

		if (here > skip) {
			fwrite(fetched + skip, 1, here - skip, file);
			crc = crc32(fetched + skip, here - skip, crc);
		}
		skip = 0;
		got += here;
		progress_show(got, total, progress_megabytes(got, total));
		if (got >= total)
			break;
		status = ask(address, got, 40);
		if ((status != 200 && status != 201) || regs[REG_REPLY + 1] != total) {
			printf("open: the host stopped at %u of %u bytes (%d)\n", (unsigned)got, (unsigned)total, status);
			fclose(file);
			return 0;
		}
	}
	fclose(file);
	if (packed && crc != want_crc) {
		printf("open: %s arrived damaged (sum %08x, should be %08x): was the picture changed on its way?\n", name,
		       (unsigned)crc, (unsigned)want_crc);
		return 0;
	}
	printf("open: %s, %u bytes%s, sum %08x\n", path, (unsigned)(total - (packed ? HEADER : 0)), packed ? " from a picture" : "",
	       (unsigned)crc);
	return 1;
}

/* A program run with its output (and its errors) to be read. */
static FILE *
reading(char *const argv[], pid_t *pid)
{
	int out[2];

	if (pipe(out) != 0 || (*pid = fork()) < 0)
		return NULL;
	if (*pid == 0) {
		dup2(out[1], 1);
		dup2(out[1], 2);
		close(out[0]);
		close(out[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(out[1]);
	return fdopen(out[0], "r");
}

/* An archive unpacked into `to` (a .gz of one file: into the file `to`), with a window that
 * says how far: a zip by its files done of all, anything else by the bytes it has been fed. */
static int
unpack(const char *kind, const char *path, const char *to)
{
	static char block[65536];
	char line[300], title[300];
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	int feed[2], status = 1;
	struct stat info;
	pid_t pid;

	snprintf(title, sizeof title, "Unpacking %s", name);
	progress_begin(title);
	if (!strcmp(kind, "zip")) {
		unsigned files = 0, done = 0;
		char *const list_args[] = { "unzip", "-l", (char *)path, NULL };
		char *const unzip_args[] = { "unzip", "-o", (char *)path, "-d", (char *)to, NULL };
		FILE *list;

		/* (the listing's last line counts the files: "  12345   3 files") */
		if ((list = reading(list_args, &pid)) != NULL) {
			char *at;

			while (fgets(line, sizeof line, list))
				if ((at = strstr(line, " file")) != NULL) {
					while (at > line && at[-1] >= '0' && at[-1] <= '9')
						at--;
					files = (unsigned)atoi(at);
				}
			fclose(list);
			waitpid(pid, &status, 0);
		}
		if ((list = reading(unzip_args, &pid)) == NULL)
			return 0;
		while (fgets(line, sizeof line, list)) {
			char said[48];

			if (strstr(line, "inflating:") || strstr(line, "extracting:") || strstr(line, "creating:"))
				done++;
			snprintf(said, sizeof said, "%u of %u files", done, files);
			progress_show(done, files, said);
		}
		fclose(list);
		waitpid(pid, &status, 0);
		progress_end();
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
	if (stat(path, &info) != 0 || pipe(feed) != 0)
		return 0;
	if ((pid = fork()) == 0) {
		dup2(feed[0], 0);
		close(feed[0]);
		close(feed[1]);
		if (!strcmp(kind, "gz")) {
			int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			if (out < 0)
				_exit(1);
			dup2(out, 1);
			execlp("gunzip", "gunzip", "-c", (char *)NULL);
		} else
			execlp("tar", "tar", !strcmp(kind, "tgz") ? "-xzf" : "-xf", "-", "-C", to, (char *)NULL);
		_exit(127);
	}
	close(feed[0]);
	if (pid > 0) {
		int from = open(path, O_RDONLY);
		unsigned long long fed = 0;
		ssize_t n;

		while (from >= 0 && (n = read(from, block, sizeof block)) > 0) {
			if (write(feed[1], block, (size_t)n) != n)
				break;
			fed += (unsigned long long)n;
			progress_show(fed, (unsigned long long)info.st_size, progress_megabytes(fed, (unsigned long long)info.st_size));
		}
		if (from >= 0)
			close(from);
		close(feed[1]);
		waitpid(pid, &status, 0);
	}
	progress_end();
	return pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* A copy of a file in /root, under its own name. */
static int
save(const char *path, const char *name)
{
	char to[300], block[4096];
	FILE *in = fopen(path, "rb"), *out;
	size_t n;
	int fine = 1;

	snprintf(to, sizeof to, "/root/%.280s", name);
	if (!in || (out = fopen(to, "wb")) == NULL) {
		if (in)
			fclose(in);
		return 0;
	}
	while ((n = fread(block, 1, sizeof block, in)) > 0)
		fine = fine && fwrite(block, 1, n, out) == n;
	fclose(in);
	return fclose(out) == 0 && fine;
}

/* Text between single quotes for the shell, whatever is in it. */
static int
quoted(char *to, int room, const char *text)
{
	int o = 0;

	if (room < 3)
		return 0;
	to[o++] = '\'';
	for (; *text && o + 6 < room; text++) {
		if (*text == '\'')
			o += snprintf(to + o, room - o, "'\\''");
		else
			to[o++] = *text;
	}
	to[o++] = '\'';
	to[o] = 0;
	return o;
}

/* A file nobody is registered for: what it is, and two things to do with it. */
static void
unknown(const char *path, long size)
{
	GR_WINDOW_ID window;
	GR_EVENT event;
	char line[300];
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

	if (GrOpen() < 0) {
		printf("open: no program is registered for %s\n", path);
		return;
	}
	ui_init();
	window = GrNewWindowEx(UI_FIXED, "Open", GR_ROOT_WINDOW_ID, 60, 60, 420, 110, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	for (;;) {
		GrGetNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			break;
		if (event.type == GR_EVENT_TYPE_EXPOSURE) {
			snprintf(line, sizeof line, "%.200s: %ld bytes, and no program is registered for it.", name, size);
			ui_text_fit(window, 12, 14, 396, line, GR_RGB(0, 0, 0));
			ui_button(window, 12, 60, 150, 28, "Open as text", 0);
			ui_button(window, 174, 60, 150, 28, "Save to /root", 0);
		}
		if (event.type != GR_EVENT_TYPE_BUTTON_DOWN)
			continue;
		if (ui_inside(event.button.x, event.button.y, 12, 60, 150, 28)) {
			GrClose();
			/* (as a program of its own: started in this one's place, which had a window, the
			   editor's window got no frame) */
			if (fork() == 0) {
				execlp("nxedit", "nxedit", path, (char *)NULL);
				_exit(1);
			}
			return;
		}
		if (ui_inside(event.button.x, event.button.y, 174, 60, 150, 28)) {
			if (!save(path, name))
				printf("open: %s was not saved\n", path);
			break;
		}
	}
	GrClose();
}

int
main(int argc, char **argv)
{
	char path[300], command[1500], address[256] = "";
	unsigned char head[64];
	const struct kind *k;
	const char *what, *use;
	struct stat info;
	int dry = argc > 2 && !strcmp(argv[1], "--dry"), n, fd, i, o = 0;

	signal(SIGCHLD, SIG_DFL);	/* (the bar that starts this ignores it: the opened program must not) */

	if (argc == 5 && !strcmp(argv[1], "--unpack"))
		return unpack(argv[2], argv[3], argv[4]) ? 0 : 1;
	if (argc < 2 + dry) {
		fprintf(stderr, "usage: open [--dry] ADDRESS-OR-FILE, open --unpack zip|tar|tgz|gz FILE TO\n");
		return 2;
	}
	what = argv[1 + dry];
	/* the host's button for the kept folder comes as an address (docs/persist.md) */
	if (!strcmp(what, "persist:save") && !dry) {
		execlp("nxpersist", "nxpersist", "save", (char *)NULL);
		return 1;
	}
	read_kinds();
	if (strstr(what, "://")) {
		snprintf(address, sizeof address, "%s", what);
		n = fetch(address, path, sizeof path);
		progress_end();
		if (!n)
			return 1;
	} else
		snprintf(path, sizeof path, "%s", what);
	if (stat(path, &info) != 0) {
		printf("open: there is no %s\n", path);
		return 1;
	}
	if (S_ISDIR(info.st_mode)) {
		k = NULL;
		o = snprintf(command, sizeof command, "nxfiles ");
		quoted(command + o, (int)sizeof command - o, path);
		goto run;
	}
	fd = open(path, O_RDONLY);
	n = fd < 0 ? 0 : (int)read(fd, head, sizeof head);
	if (fd >= 0)
		close(fd);
	if (n < 0)
		n = 0;
	k = kind_of(path, head, n);
	/* a program of another machine's is not run, and a script only if it says what runs it */
	if (n >= 20 && !memcmp(head, "\177ELF", 4) && !(head[4] == 1 && head[18] == 243 && head[19] == 0)) {
		printf("open: %s is a program, but not one for this machine (RISC-V, 32 bits)\n", path);
		return 1;
	}
	if (!k) {
		printf("open: %s: unknown kind\n", path);
		if (!dry)
			unknown(path, (long)info.st_size);
		return dry ? 0 : 1;
	}
	/* %s is the file; a page that came from an address is opened as that address, so that
	 * its links and pictures are found */
	use = address[0] && !strncmp(address, "http", 4) && !strcmp(k->name, "page") ? address : path;
	for (i = 0; k->command[i] && o < (int)sizeof command - 1; i++) {
		if (k->command[i] == '%' && k->command[i + 1] == 's') {
			o += quoted(command + o, (int)sizeof command - o, use);
			i++;
		} else
			command[o++] = k->command[i];
	}
	command[o < (int)sizeof command ? o : (int)sizeof command - 1] = 0;
run:
	printf("open: %s is %s: %s\n", path, k ? k->name : "a folder", command);
	fflush(stdout);
	if (dry)
		return 0;
	/* a line of plain words and the file is run as it is: a shell between is a program started
	 * for nothing. Anything a shell would read otherwise is the shell's still. */
	if (!k) {
		execlp("nxfiles", "nxfiles", path, (char *)NULL);
	} else if (!strpbrk(k->command, "'\"\\$|&;<>()*?[]~`#!{}=")) {
		static char line[sizeof k->command];
		char *words[12];

		strcpy(line, k->command);
		n = 0;
		for (words[n] = strtok(line, " \t"); words[n] && n < 10; words[n] = strtok(NULL, " \t")) {
			if (!strcmp(words[n], "%s"))
				words[n] = (char *)use;
			else if (strchr(words[n], '%'))
				break;
			n++;
		}
		if (!words[n] && n) {
			execvp(words[0], words);
			return 127;
		}
	}
	execl("/bin/sh", "sh", "-c", command, (char *)NULL);
	return 127;
}
