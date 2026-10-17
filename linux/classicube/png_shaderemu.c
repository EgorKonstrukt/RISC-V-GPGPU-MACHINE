/* The texture pack's pictures through libmedia (docs/games/classicube.md): the pack, all of them in PNGs
   (store_zip.py), decoded on the worker cores beside the game's start, and each picture's stub
   given its pages of it. Another PNG is read whole and decoded on the workers. */
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include "Core.h"
#include "Bitmap.h"
#include "Stream.h"
#include "Errors.h"
#include "Platform.h"
#include "String_.h"
#include "Utils.h"
#include "media.h"
#include "mcw.h"

#define SE_PNG_WORKERS 15	/* the game's own number (Generator.c): asked for here first */
#define SE_PACK_NAME "shaderemu-pack.png"
#define SE_PACK_ZIP "texpacks/default.zip"
#define SE_PACK_MOST 64		/* pictures given out and not freed yet */
#define SE_PACK_NICE 8		/* the decoding thread's: 10 was as quick with less to spare, 5 and 15 slower */

/* The rest of a stream after its first `have` bytes, in memory from malloc. */
static cc_result ReadAll(struct Stream* stream, const cc_uint8* first, cc_uint32 have, cc_uint8** data, cc_uint32* size) {
	cc_uint32 room = 65536, got;
	cc_uint8* file = (cc_uint8*)malloc(room);
	cc_result res;

	if (!file) return ERR_OUT_OF_MEMORY;
	memcpy(file, first, have);
	for (;;) {
		if (have == room) {
			cc_uint8* more = (cc_uint8*)realloc(file, room *= 2);
			if (!more) { free(file); return ERR_OUT_OF_MEMORY; }
			file = more;
		}
		res = stream->Read(stream, file + have, room - have, &got);
		if (res) { free(file); return res; }
		if (!got) break;
		have += got;
	}
	*data = file, *size = have;
	return 0;
}

static int Workers(void) {
	const char* inline_mesh = getenv("CLASSICUBE_MESH");
	if (!(inline_mesh && !strcmp(inline_mesh, "inline")) && !mcw_count()) mcw_open(SE_PNG_WORKERS);
	return mcw_count() ? SE_PNG_WORKERS : 0;
}

/* A pack decoded: its pages, its name (the last IDAT's check, before IEND), and which pages
   pictures were given out of. */
struct SEPack { cc_uint8* pixels; cc_uint32 bytes, sum, row, at, rows; const char* why; cc_uint8 given[2048]; };
static struct SEPack pack, early;
/* The pack read at the start, and the thread that decodes it */
static cc_uint8* early_file;
static cc_uint32 early_size;
static pthread_t decoder;
static int decoding;	/* 1 while it runs, 2 when joined */

static cc_uint32 PackSum(const cc_uint8* file, cc_uint32 size) {
	return size < 28 ? 0 : (cc_uint32)file[size - 16] << 24 | file[size - 15] << 16 | file[size - 14] << 8 | file[size - 13];
}

/* How long the PNG at `f` is (to its IEND chunk's end); 0 when it is not one. */
static cc_uint32 PngLength(const cc_uint8* f, cc_uint32 size) {
	cc_uint32 at = 8, n;
	if (size < 8 || memcmp(f, "\x89PNG\r\n\x1a\n", 8)) return 0;
	while (at + 12 <= size) {
		n = (cc_uint32)f[at] << 24 | f[at + 1] << 16 | f[at + 2] << 8 | f[at + 3];
		if (n > size - at - 12) return 0;
		if (!memcmp(f + at + 4, "IEND", 4)) return at + 12 + n;
		at += 12 + n;
	}
	return 0;
}

/* The pack's PNGs, of rows alike, one after another into one picture: its pages mapped. */
static cc_bool PackStart(struct SEPack* p, const cc_uint8* file, cc_uint32 size) {
	struct media_info info;
	cc_uint32 at, n, rows = 0;

	memset(p, 0, sizeof *p);
	p->sum = PackSum(file, size);
	for (at = 0; at < size; at += n) {
		if (!(n = PngLength(file + at, size - at)) || media_probe(file + at, n, &info) || (p->row && p->row != info.width * 4)) { p->why = "not PNGs"; return false; }
		p->row = info.width * 4, rows += info.height;
	}
	p->bytes = (p->row * rows + 4095) & ~4095u;
	if (!p->bytes || p->bytes > sizeof p->given * 4096) { p->why = "too large"; return false; }
	p->pixels = (cc_uint8*)mmap(NULL, p->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (p->pixels == MAP_FAILED) { p->pixels = NULL; p->why = "no memory"; return false; }
	mcw_touch(p->pixels, p->bytes);
	return true;
}

/* The next `most` of its PNGs decoded (0: all that are left); whether all went well. */
static cc_bool PackParts(struct SEPack* p, const cc_uint8* file, cc_uint32 size, int workers, int most) {
	struct media_image out;
	struct media_info info;
	cc_uint32 n;
	int done = 0;

	for (; p->at < size && !p->why; p->at += n, p->rows += info.height) {
		if (most && done++ == most) break;
		n = PngLength(file + p->at, size - p->at);
		media_probe(file + p->at, n, &info);
		memset(&out, 0, sizeof out);
		out.width = info.width, out.height = info.height, out.channels = 4;
		out.stride = p->row, out.pixels = p->pixels + p->rows * p->row;
		p->why = media_image_decode(file + p->at, n, &out, workers);
	}
	return !p->why;
}

/* A pack's pages that no picture was given out of. */
static void PackDrop(struct SEPack* p) {
	cc_uint32 page;
	if (!p->pixels) p->bytes = 0;
	for (page = 0; page < p->bytes / 4096; page++) {
		if (!p->given[page]) munmap(p->pixels + page * 4096, 4096);
	}
	p->pixels = NULL, p->bytes = 0;
}

static void* EarlyJob(void* unused) {
	/* (it mostly waits for the workers: at the game's own priority each look ended a pass the */
	/*  game could have had, and it had half the core) */
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), SE_PACK_NICE);
	PackParts(&early, early_file, early_size, SE_PNG_WORKERS, 0);
	return unused;
}

/* The decoder joined: no other code of the game's asks the workers for anything before. */
static void Settle(void) {
	if (decoding != 1) return;
	pthread_join(decoder, NULL);
	decoding = 2;
}

/* The game's start: the default pack's first entry, the pack, read and given to a thread that
   decodes it. Nothing when the zip begins with something else. */
void SE_PackEarly(void) {
	cc_uint8 head[30];
	cc_uint32 name, extra;
	int fd = open(SE_PACK_ZIP, O_RDONLY);

	if (fd < 0) return;
	if (read(fd, head, 30) == 30 && Mem_ReadU32_LE(head) == 0x04034b50 && head[8] == 0 && head[9] == 0) {
		early_size = Mem_ReadU32_LE(head + 18);
		name = head[26] | head[27] << 8, extra = head[28] | head[29] << 8;
		early_file = name == sizeof SE_PACK_NAME - 1 ? (cc_uint8*)malloc(name + extra + early_size) : NULL;
		if (early_file && (cc_uint32)read(fd, early_file, name + extra) == name + extra && !memcmp(early_file, SE_PACK_NAME, name) &&
		    lseek(fd, 30 + name + extra, SEEK_SET) >= 0 && (cc_uint32)read(fd, early_file, early_size) == early_size && Workers() &&
		    PackStart(&early, early_file, early_size) && PackParts(&early, early_file, early_size, SE_PNG_WORKERS, 1) &&
		    !pthread_create(&decoder, NULL, EarlyJob, NULL)) {
			decoding = 1;
		} else {
			PackDrop(&early);
			free(early_file);
			early_file = NULL;
		}
	}
	close(fd);
}

/* Pictures given out: each its own pages of a pack, unmapped when the game frees it. */
static struct { cc_uint8* at; cc_uint32 bytes; } live[SE_PACK_MOST];
static cc_uint8 *live_lo, *live_hi;

static void LiveBounds(void) {
	int i;
	live_lo = live_hi = NULL;
	for (i = 0; i < SE_PACK_MOST; i++) {
		if (!live[i].at) continue;
		if (!live_lo || live[i].at < live_lo) live_lo = live[i].at;
		if (live[i].at + live[i].bytes > live_hi) live_hi = live[i].at + live[i].bytes;
	}
}

/* Mem_Free's: whether `mem` is a pack's picture (its pages are let go). */
int SE_PackFree(void* mem) {
	cc_uint8* at = (cc_uint8*)mem;
	int i;
	if (at < live_lo || at >= live_hi) return 0;
	for (i = 0; i < SE_PACK_MOST; i++) {
		if (live[i].at != at) continue;
		munmap(at, live[i].bytes);
		live[i].at = NULL;
		LiveBounds();
		return 1;
	}
	return 0;
}

/* A zip entry: the pack is decoded here, or taken from the start's decoder (true); every other
   entry is left to the game. */
cc_bool SE_PackEntry(const cc_string* name, struct Stream* stream) {
	cc_uint8* file;
	cc_uint32 size;

	Settle();
	if (name->length != sizeof SE_PACK_NAME - 1 || memcmp(name->buffer, SE_PACK_NAME, name->length)) return false;
	PackDrop(&pack);
	if (ReadAll(stream, NULL, 0, &file, &size)) file = NULL, size = 0;
	if (early_file && size == early_size && PackSum(file, size) == early.sum && !early.why) {
		pack = early;
		early.pixels = NULL, early.bytes = 0;
	} else {
		if (file && PackStart(&pack, file, size)) PackParts(&pack, file, size, Workers(), 0);
		if (early_file) PackDrop(&early);
	}
	free(file);
	free(early_file);
	early_file = NULL;
	if (pack.why) PackDrop(&pack);
	return true;
}

/* A stub's picture: its pages of the pack. */
cc_result SE_PackPicture(struct Bitmap* bmp, struct Stream* stream) {
	cc_uint8 stub[16];
	cc_uint32 sum, at, width, height, bytes, page;
	cc_result res = Stream_Read(stream, stub, sizeof stub);
	int i;

	if (res) return res;
	sum = Mem_ReadU32_LE(stub), at = Mem_ReadU32_LE(stub + 4);
	width = Mem_ReadU32_LE(stub + 8), height = Mem_ReadU32_LE(stub + 12);
	bytes = (width * height * 4 + 4095) & ~4095u;
	if (!pack.pixels || sum != pack.sum || (at & 4095) || !width || !height || width > 8192 || height > 8192 ||
	    at + bytes > pack.bytes || pack.given[at / 4096]) return PNG_ERR_INVALID_SIG;
	for (i = 0; i < SE_PACK_MOST && live[i].at; i++) {}
	if (i == SE_PACK_MOST) return ERR_OUT_OF_MEMORY;
	for (page = at / 4096; page < (at + bytes) / 4096; page++) pack.given[page] = 1;
	live[i].at = pack.pixels + at, live[i].bytes = bytes;
	LiveBounds();
	bmp->scan0 = (BitmapCol*)(pack.pixels + at);
	bmp->width = (int)width, bmp->height = (int)height;
	return 0;
}

cc_result SE_PngDecode(struct Bitmap* bmp, struct Stream* stream, const cc_uint8* signature) {
	struct media_image out;
	struct media_info info;
	cc_uint8* file;
	cc_uint32 size;
	const char* why;
	cc_result res;

	Settle();
	if ((res = ReadAll(stream, signature, 8, &file, &size))) return res;
	if (media_probe(file, size, &info) || info.kind != MEDIA_PNG) { free(file); return PNG_ERR_INVALID_SIG; }
	bmp->width = info.width, bmp->height = info.height;
	bmp->scan0 = (BitmapCol*)Mem_TryAlloc(info.height, info.width * 4);
	if (!bmp->scan0) { free(file); return ERR_OUT_OF_MEMORY; }
	/* (its pages there before a worker writes them) */
	mcw_touch(bmp->scan0, (unsigned)info.width * (unsigned)info.height * 4);

	memset(&out, 0, sizeof out);
	out.width = info.width, out.height = info.height, out.channels = 4, out.bgr = 1;
	out.stride = info.width * 4, out.pixels = (uint8_t*)bmp->scan0;
	why = media_image_decode(file, size, &out, Workers());
	free(file);
	if (why) {
		Mem_Free(bmp->scan0);
		bmp->scan0 = NULL, bmp->width = 0, bmp->height = 0;
		return PNG_ERR_INVALID_SIG;
	}
	return 0;
}
