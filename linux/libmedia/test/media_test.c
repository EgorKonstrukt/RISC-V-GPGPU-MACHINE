/*
 * libmedia's test program (docs/system/media.md), for the build computer (with mcw_host.c) and the guest:
 *   media_test decode FILE OUT.ppm [WORKERS [CHANNELS]]   a picture, its sum and its time
 *   media_test fit FILE W H OUT.ppm [WORKERS]             to a size, covered
 *   media_test encode IN OUT.(png|jpg|gif|bmp|ppm) [QUALITY [WORKERS]]
 *   media_test anim FILE [WORKERS]                        an animation's frames' sums
 *   media_test audio FILE OUT.raw [WORKERS [PIECE]]       16-bit sound, its sum (WORKERS -1: in order, -2/-3: pieces backwards)
 *   media_test video FILE [WORKERS]                       a film's pictures' sums (MEDIA_TEST_FRAMES=FILE: the pictures)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "media.h"
#include "mcw.h"
#include "mcwx.h"

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static uint8_t *
read_all(const char *path, uint32_t *size)
{
	struct stat st;
	int fd = open(path, O_RDONLY);
	uint8_t *f;

	if (fd < 0 || fstat(fd, &st))
		return NULL;
	*size = (uint32_t)st.st_size;
	f = mmap(NULL, *size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
	close(fd);
	return f == MAP_FAILED ? NULL : f;
}

static int
write_ppm(const char *path, const struct media_image *im)
{
	FILE *o = fopen(path, "wb");
	int y, x;

	if (!o)
		return 0;
	fprintf(o, im->channels == 4 ? "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" : "P6\n%d %d\n255\n",
		im->width, im->height);
	for (y = 0; y < im->height; y++)
		for (x = 0; x < im->width; x++)
			fwrite(im->pixels + (uint32_t)y * (uint32_t)im->stride + x * im->channels, 1, im->channels, o);
	fclose(o);
	return 1;
}

static uint32_t
image_sum(const struct media_image *im)
{
	uint32_t s = 2166136261u;
	int y;

	for (y = 0; y < im->height; y++)
		s = media_sum(im->pixels + (uint32_t)y * (uint32_t)im->stride, (uint32_t)(im->width * im->channels), s);
	return s;
}

static volatile uint32_t busy_free;
static mcw_handle busy_h[64];
static int busy_n;

static uint32_t
busy_job(uint32_t unused0, uint32_t unused1)
{
	(void)unused0, (void)unused1;
	while (!busy_free)
		;
	return 0;
}

int
main(int argc, char **argv)
{
	uint32_t size;
	uint8_t *f;
	double t0;

	if (argc < 3) {
		fprintf(stderr, "media_test decode|fit|encode|anim|audio|video ...\n");
		return 2;
	}
	if (!(f = read_all(argv[2], &size))) {
		fprintf(stderr, "cannot read %s\n", argv[2]);
		return 1;
	}
	if (!strcmp(argv[1], "decode") && argc >= 4) {
		struct media_image im;
		const char *why;
		struct media_info info;
		int repeat = getenv("MT_REPEAT") ? atoi(getenv("MT_REPEAT")) : 1, r;

		/* MT_OPEN=N: the workers opened before the clock starts, as a program that keeps them has them */
		if (getenv("MT_OPEN")) {
			int got;

			t0 = now();
			got = mcw_open(atoi(getenv("MT_OPEN")));
			printf("opened %d workers in %.1f ms\n", got, (now() - t0) * 1000);
		}
		/* MT_BUSY=N: N workers kept busy by calls of the program's own while it decodes */
		if (getenv("MT_BUSY"))
			for (busy_n = 0; busy_n < atoi(getenv("MT_BUSY")) && busy_n < 64; busy_n++)
				if (!(busy_h[busy_n] = mcw_try_call(busy_job, 0, 0)))
					break;
		memset(&im, 0, sizeof im);
		im.channels = argc > 5 ? atoi(argv[5]) : 3;
		media_probe(f, size, &info);
		/* MT_PREALLOC: the pixels mapped before the clock starts, and how long that took */
		if (getenv("MT_PREALLOC")) {
			t0 = now();
			im.width = info.width, im.height = info.height, im.stride = (info.width * im.channels + 15) & ~15;
			im.pixels = mmap(NULL, (size_t)im.stride * im.height, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
			printf("mapped %d KB in %.1f ms\n", im.stride * im.height / 1024, (now() - t0) * 1000);
		}
		for (r = 1; r < repeat; r++) {
			t0 = now();
			why = media_image_decode(f, size, &im, argc > 4 ? atoi(argv[4]) : 0);
			printf("decode %s: %.1f ms\n", argv[2], (now() - t0) * 1000);
			/* MT_KEEP, MT_PREALLOC: the next decodes go into the same pixels, as into a window's */
			if (!getenv("MT_KEEP") && !getenv("MT_PREALLOC")) {
				media_image_free(&im);
				im.channels = argc > 5 ? atoi(argv[5]) : 3;
			}
		}
		t0 = now();
		why = media_image_decode(f, size, &im, argc > 4 ? atoi(argv[4]) : 0);
		if (why) {
			printf("decode %s: %s\n", argv[2], why);
			return 1;
		}
		printf("decode %s: %s %dx%d%s, sum %08x, %.1f ms\n", argv[2], media_kind_name(info.kind), im.width, im.height,
		       info.parallel ? " (laid out for the workers)" : "", image_sum(&im), (now() - t0) * 1000);
		if (busy_n) {
			busy_free = 1;
			printf("%d busy calls joined\n", busy_n);
			while (busy_n > 0)
				mcw_join(busy_h[--busy_n]);
		}
		write_ppm(argv[3], &im);
		return 0;
	}
	if (!strcmp(argv[1], "fit") && argc >= 6) {
		struct media_image im;
		const char *why;

		memset(&im, 0, sizeof im);
		im.width = atoi(argv[3]), im.height = atoi(argv[4]), im.channels = 3, im.stride = (im.width * 3 + 15) & ~15;
		im.pixels = calloc((size_t)im.stride, (size_t)im.height);
		im.background = 0;
		t0 = now();
		why = media_image_decode_fit(f, size, &im, 1, argc > 6 ? atoi(argv[6]) : 0);
		if (why) {
			printf("fit %s: %s\n", argv[2], why);
			return 1;
		}
		printf("fit %s: %dx%d, sum %08x, %.1f ms\n", argv[2], im.width, im.height, image_sum(&im), (now() - t0) * 1000);
		write_ppm(argv[5], &im);
		return 0;
	}
	if (!strcmp(argv[1], "encode") && argc >= 4) {
		struct media_image im;
		const char *why;

		memset(&im, 0, sizeof im);
		im.channels = 3;
		if ((why = media_image_decode(f, size, &im, 0)) != NULL) {
			printf("encode %s: %s\n", argv[2], why);
			return 1;
		}
		t0 = now();
		why = media_image_save(argv[3], &im, argc > 4 ? atoi(argv[4]) : 0, argc > 5 ? atoi(argv[5]) : 0);
		printf("encode %s -> %s: %s, %.1f ms\n", argv[2], argv[3], why ? why : "written", (now() - t0) * 1000);
		return why != NULL;
	}
	if (!strcmp(argv[1], "anim")) {
		const char *why;
		struct media_anim *a;
		int n, delay;

		t0 = now();
		a = media_anim_open(f, size, argc > 3 ? atoi(argv[3]) : 0, &why);
		if (!a) {
			printf("anim %s: %s\n", argv[2], why);
			return 1;
		}
		printf("anim %s: %dx%d, %d frames, opened in %.1f ms\n", argv[2], media_anim_width(a), media_anim_height(a),
		       media_anim_frames(a), (now() - t0) * 1000);
		for (n = 0; n < media_anim_frames(a); n++) {
			const uint8_t *c = media_anim_frame(a, n, &delay);

			printf("frame %d: %d ms, sum %08x\n", n, delay, media_sum(c, (uint32_t)(media_anim_width(a) * media_anim_height(a) * 4), 2166136261u));
		}
		media_anim_close(a);
		return 0;
	}
	/* animenc IN OUT.gif [WORKERS]: an animation's frames (as decoded) written again as one */
	if (!strcmp(argv[1], "animenc") && argc >= 4) {
		const char *why;
		struct media_anim *a = media_anim_open(f, size, argc > 4 ? atoi(argv[4]) : 0, &why);
		struct media_image *frames;
		int n, count, *delays;
		uint32_t bytes, frame_bytes;
		uint8_t *gif;
		FILE *o;

		if (!a) {
			printf("animenc %s: %s\n", argv[2], why);
			return 1;
		}
		count = media_anim_frames(a);
		frame_bytes = (uint32_t)(media_anim_width(a) * media_anim_height(a) * 4);
		frames = calloc((size_t)count, sizeof *frames);
		delays = calloc((size_t)count, sizeof *delays);
		for (n = 0; n < count; n++) {
			frames[n].width = media_anim_width(a), frames[n].height = media_anim_height(a);
			frames[n].channels = 4, frames[n].stride = frames[n].width * 4;
			frames[n].pixels = malloc(frame_bytes);
			memcpy(frames[n].pixels, media_anim_frame(a, n, &delays[n]), frame_bytes);
		}
		t0 = now();
		gif = media_gif_anim_encode(frames, count, delays, media_anim_loops(a), &bytes, argc > 4 ? atoi(argv[4]) : 0);
		if (!gif) {
			printf("animenc %s: not encoded\n", argv[2]);
			return 1;
		}
		printf("animenc %s: %d frames, %u bytes, %.1f ms\n", argv[2], count, bytes, (now() - t0) * 1000);
		if ((o = fopen(argv[3], "wb")) != NULL)
			fwrite(gif, 1, bytes, o), fclose(o);
		media_free(gif, bytes);
		media_anim_close(a);
		return 0;
	}
	if (!strcmp(argv[1], "inflate") && argc >= 4) {	/* inflate FILE OUT [WORKERS]: deflate in segments */
		uint32_t total = media_inflated_size(f, size);
		uint8_t *out = total ? aligned_alloc(16, (total + 15) & ~15u) : NULL;
		const char *why = out ? media_inflate(f, size, out, total, argc > 4 ? atoi(argv[4]) : 0) : "not deflate in segments";
		FILE *o;

		if (why) {
			printf("inflate %s: %s\n", argv[2], why);
			return 1;
		}
		o = fopen(argv[3], "wb");
		fwrite(out, 1, total, o);
		fclose(o);
		printf("inflate: %u bytes, fnv %08x\n", total, media_sum(out, total, 2166136261u));
		return 0;
	}
	if (!strcmp(argv[1], "audio") && argc >= 4) {
		const char *why;
		struct media_audio *a = media_audio_open(f, size, &why);
		struct media_audio_info info;
		int16_t *pcm;
		uint32_t frames, most;
		int n;
		FILE *o;

		if (!a) {
			printf("audio %s: %s\n", argv[2], why);
			return 1;
		}
		int how = argc > 4 ? atoi(argv[4]) : 0;

		if (argc > 5)
			media_audio_set_piece(a, (uint32_t)atoi(argv[5]));
		if (getenv("MEDIA_HALF"))
			media_audio_half(a, 1);
		media_audio_get_info(a, &info);
		most = info.frames ? info.frames : 48000u * 600;
		pcm = aligned_alloc(16, ((size_t)most * info.channels * 2 + 15) & ~(size_t)15);
		t0 = now();
		if (how >= 0) {
			frames = media_audio_decode_all(a, pcm, most, how);
		} else if (how == -1 || !info.pieces) {
			/* -1: in order on this core */
			for (frames = 0; (n = media_audio_read(a, pcm + frames * info.channels, 4096)) > 0; frames += (uint32_t)n)
				;
		} else {
			/* -2: the pieces backwards on one decoder (each one a seek); -3: the same, then seeks of the reader checked */
			uint32_t p = info.pieces, bad = 0, k, at;
			int16_t *again = malloc(4096 * 2 * 2);

			media_audio_decoders(a, 1);
			while (p--)
				media_audio_piece(a, 0, p, pcm + media_audio_piece_first(a, p) * info.channels);
			frames = info.frames;
			for (k = 0; how == -3 && k < 200; k++) {
				at = (uint32_t)((uint64_t)frames * ((k * 7919u) % 1000u) / 1000u);
				if (media_audio_seek(a, at) != at || (n = media_audio_read(a, again, 4096)) <= 0 ||
				    memcmp(again, pcm + at * info.channels, (size_t)n * info.channels * 2))
					bad++;
			}
			if (how == -3)
				printf("audio: 200 seeks of the reader, %u not as the pieces\n", bad);
		}
		printf("audio %s: %s %d Hz %d ch, %u frames (%u pieces), sum %08x, %.1f ms\n", argv[2], media_kind_name(info.kind), info.rate,
		       info.channels, frames, info.pieces, media_sum(pcm, frames * info.channels * 2, 2166136261u), (now() - t0) * 1000);
		if ((o = fopen(argv[3], "wb"))) {
			fwrite(pcm, 2, (size_t)frames * info.channels, o);
			fclose(o);
		}
		media_audio_close(a);
		return 0;
	}
	if (!strcmp(argv[1], "video")) {
		const char *why;
		struct media_video *v;
		struct media_video_info info;
		const uint32_t *pal;
		int n, k, quiet = getenv("MEDIA_TEST_QUIET") != NULL;
		uint32_t all = 0, sound = 0, got, at;
		int16_t *pcm;

		/* MT_OPEN=N, MT_BUSY=N: as for decode */
		if (getenv("MT_OPEN"))
			printf("opened %d workers\n", mcw_open(atoi(getenv("MT_OPEN"))));
		if (getenv("MT_BUSY"))
			for (busy_n = 0; busy_n < atoi(getenv("MT_BUSY")) && busy_n < 64; busy_n++)
				if (!(busy_h[busy_n] = mcw_try_call(busy_job, 0, 0)))
					break;
		t0 = now();
		if (!(v = media_video_open(f, size, argc > 3 ? atoi(argv[3]) : 0, &why))) {
			printf("video %s: %s\n", argv[2], why);
			return 1;
		}
		media_video_get_info(v, &info);
		printf("video %s: %s %dx%d, %d frames, %d.%03d a second, sound %d Hz %d ch %u frames, %d workers\n", argv[2],
		       media_kind_name(info.kind), info.width, info.height, info.frames, info.fps_milli / 1000, info.fps_milli % 1000,
		       info.rate, info.channels, info.sound_frames, info.workers);
		/* `places`: pictures in places of the program's own, as a player has them */
		if (argc > 4 && !strcmp(argv[4], "places")) {
			int slots = info.least_places + 3;
			uint8_t **places = malloc(sizeof *places * (size_t)slots);

			for (k = 0; k < slots; k++)
				places[k] = mcw_alloc(info.frame_bytes);
			if ((why = media_video_places(v, places, slots)) != NULL)
				printf("video: places: %s\n", why);
		}
		for (n = 0; n < info.frames; n++) {
			const uint8_t *frame;
			uint32_t sum;

			while (!(frame = media_video_frame(v, n, &pal))) {
				media_video_pump(v);
				mcw_rest();	/* (the workers have it: a player's first core rests) */
			}
			/* MEDIA_TEST_FRAMES=FILE: every picture as kept, one after another (a palette's 256 words after each) */
			if (getenv("MEDIA_TEST_FRAMES")) {
				FILE *o = fopen(getenv("MEDIA_TEST_FRAMES"), n ? "ab" : "wb");

				if (o) {
					fwrite(frame, 1, info.frame_bytes, o);
					if (pal)
						fwrite(pal, 4, 256, o);
					fclose(o);
				}
			}
			if (getenv("MEDIA_TEST_NOSUM"))	/* (for counting the decoding alone) */
				continue;
			sum = media_sum(frame, info.frame_bytes, 2166136261u);
			if (pal)
				sum = media_sum(pal, 1024, sum);
			for (at = 0; at + 4 <= info.frame_bytes; at += 4)
				all = (all << 5 | all >> 27) ^ (uint32_t)(frame[at] | frame[at + 1] << 8 | frame[at + 2] << 16 | (uint32_t)frame[at + 3] << 24);
			if (!quiet)
				printf("frame %d: %u ms, sum %08x\n", n, media_video_time(v, n), sum);
		}
		printf("video: pictures' sum %08x (nxvideo's), %.1f ms\n", all, (now() - t0) * 1000);
		if (info.rate) {
			pcm = malloc((size_t)info.sound_frames * info.channels * 2 + 16);
			for (got = 0; got < info.sound_frames;) {
				uint32_t more = media_video_sound(v, got, pcm + got * info.channels, info.sound_frames - got);

				if (!more) {
					media_video_pump(v);
					mcw_rest();
				}
				got += more;
			}
			for (at = 0; at + 4 <= got * info.channels * 2; at += 4) {
				const uint8_t *b = (const uint8_t *)pcm + at;

				sound = (sound << 5 | sound >> 27) ^ (uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24);
			}
			/* MEDIA_TEST_SOUND=FILE: the sound as 16-bit samples */
			if (getenv("MEDIA_TEST_SOUND")) {
				FILE *o = fopen(getenv("MEDIA_TEST_SOUND"), "wb");

				if (o)
					fwrite(pcm, 2, (size_t)got * (size_t)info.channels, o), fclose(o);
			}
			printf("video: sound's sum %08x (nxvideo's), fnv %08x, %.1f ms\n", sound, media_sum(pcm, got * info.channels * 2, 2166136261u),
			       (now() - t0) * 1000);
		}
		media_video_close(v);
		if (busy_n) {
			busy_free = 1;
			printf("%d busy calls joined\n", busy_n);
			while (busy_n > 0)
				mcw_join(busy_h[--busy_n]);
		}
		return 0;
	}
	fprintf(stderr, "media_test: what is %s?\n", argv[1]);
	return 2;
}
