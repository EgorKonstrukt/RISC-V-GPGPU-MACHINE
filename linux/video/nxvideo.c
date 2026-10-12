/*
 * nxvideo: plays an MPEG-1 file (video, and layer II sound) on the worker cores: docs/video.md.
 *
 *	nxvideo [FILE]		plays it in a window, again and again (the image's sample without FILE)
 *	nxvideo --decode FILE	no window: decodes every picture on this core and prints sums
 *
 * A group of pictures is one worker's job: it decodes the group into memory of its own, and
 * when the frames before it have been shown copies it into GPU memory, where a frame is a
 * texture as it lies (three planes of bytes; the GPU makes the colours). The sound is decoded
 * in chunks by more workers, and the picture shown is the one the sound card has got to.
 * Under the picture: Play or Pause, a bar that shows where the film is and is dragged to go
 * elsewhere, the time, and Open. Space pauses, Left and Right go five seconds back and on,
 * Home to the start, o opens another, q leaves. Its window is there before the file is taken
 * apart. NXVIDEO_WORKERS=N asks for N workers (0: everything on this core),
 * NXVIDEO_HOLD=N stops at picture N, NXVIDEO_SUM=1 prints a sum of all the pictures once they
 * have all been decoded, NXVIDEO_ONCE=1 plays once and leaves.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mpeg.h"

#define SAMPLE "/root/Videos/Big Buck Bunny.mpg"

static struct mpeg_info info;

static unsigned
cycles(void)
{
#ifdef MPEG_HOST
	return 0;
#else
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
#endif
}

#ifdef MPEG_HOST
static void *file_room(unsigned bytes) { return malloc(bytes); }
#else
#include "mcw.h"
static void *file_room(unsigned bytes) { return mcw_alloc(bytes); }
#endif

static uint8_t *
load(const char *path, unsigned *bytes)
{
	int fd = open(path, O_RDONLY);
	long size = fd < 0 ? -1 : lseek(fd, 0, SEEK_END), got = 0, n;
	uint8_t *file;

	if (size <= 0 || size > 64 << 20 || (file = file_room((unsigned)size)) == NULL) {
		fprintf(stderr, "nxvideo: %s cannot be read\n", path);
		return NULL;
	}
	lseek(fd, 0, SEEK_SET);
	while (got < size && (n = read(fd, file + got, (size_t)(size - got))) > 0)
		got += n;
	close(fd);
	*bytes = (unsigned)got;
	return file;
}

/* Every picture and every chunk of sound decoded here, one after another, with their sums:
 * what any other way of decoding the file must come to. */
static int
decode_all(const char *path)
{
	unsigned bytes, before, sum = 0, spent = 0;
	uint8_t *file = load(path, &bytes);
	const char *wrong;
	int video, audio, g, i, n;

	if (!file)
		return 1;
	if ((wrong = mpeg_open(file, bytes, &info)) != NULL) {
		fprintf(stderr, "nxvideo: %s: %s\n", path, wrong);
		return 1;
	}
	printf("nxvideo: %d x %d, %d.%03d pictures a second, %d pictures in %d groups, sound %d Hz in %d chunks\n", info.width, info.height,
		info.fps_milli / 1000, info.fps_milli % 1000, info.frames, info.gops, info.rate, info.chunks);
	video = mpeg_video_new(0);
	for (g = 0; g < info.gops; g++) {
		before = cycles();
		n = (int)mpeg_decode_gop((uint32_t)video, (uint32_t)g);
		spent += cycles() - before;
		for (i = 0; i < n; i++)
			sum = mpeg_sum(mpeg_video_frame(video, i), info.frame_bytes, sum);
		if (n != mpeg_gop_frames(g))
			printf("nxvideo: group %d gave %d pictures of %d\n", g, n, mpeg_gop_frames(g));
	}
	printf("nxvideo: pictures' sum %08x, %u thousand instructions a picture\n", sum, info.frames ? spent / 1000 / (unsigned)info.frames : 0);
	if ((audio = mpeg_audio_new()) >= 0) {
		before = cycles();
		for (g = 0; g < info.chunks; g++)
			mpeg_decode_chunk((uint32_t)audio, (uint32_t)g);
		spent = cycles() - before;
		printf("nxvideo: sound's sum %08x, %u thousand instructions a second of it\n",
			mpeg_sum((const uint8_t *)mpeg_pcm(), (unsigned)info.pcm_frames * 4, 0),
			(unsigned)((uint64_t)spent * (unsigned)info.rate / (unsigned)info.pcm_frames / 1000));
		if (getenv("NXVIDEO_WHOLE"))
			printf("nxvideo: sound decoded in one piece: sum %08x\n", mpeg_sound_whole());
	}
	return 0;
}

#ifdef MPEG_HOST
int
main(int argc, char **argv)
{
	return argc > 2 ? decode_all(argv[2]) : 1;
}
#else

#include <GLES/segl.h>
#include "ui.h"
#include "ui_files.h"
#include "mc.h"
#include "shaderemu_sound.h"

#define ONE 65536
#define RING 65536u		/* the sound stream's ring, in frames */
#define FEED_MOST 1024u		/* frames put into the ring a turn of the loop, */
#define FEED_AHEAD 4096u	/* while the card has so many still to play */
#define AHEAD 36		/* pictures in hand before it plays, or plays on after it ran dry: groups come in bursts */
#define BAR 40			/* the strip of controls under the picture */
#define KEY_W 62

enum { IDLE, DECODING, DECODED, COPYING, SOUND };
static struct unit {		/* a worker and the decoder it has */
	int video, audio;	/* one of them is -1 */
	int state, gop, frames;
} units[MC_MAX_CORES];
static int unit_count, sounds, workers;
static struct { int busy; uint32_t result; } jobs[MC_MAX_CORES];
static unsigned char *gop_state;	/* 0 to do, 1 being decoded, 2 its pictures are in GPU memory */
static uint32_t *gop_sum;
static int first_gop, next_gop, next_chunk, chunks_done;
static int ready_gop, ready_last;	/* ready(): the first group not yet in GPU memory, and the picture before it */
static unsigned char *chunk_done;

static uint8_t *ring_a, *ring_b;	/* GPU memory for frames: two pieces */
static int slots_a, slots;
static int shown = -1, dropped;
static int seeking = -1;		/* the picture to go to once the groups being decoded have been dropped */

static int sound_on, voice, sound_running;
static int16_t *sound_ring;
static uint32_t sound_ring_at, written, sound_from;	/* frames given to the card, and the film's frame its first is */
static unsigned clock_base, clock_wall, clock_from;	/* the film's time at a moment, that moment, and the time playing began at */
static uint32_t clock_seen;
static int running, paused, ended, opened;

static GR_WINDOW_ID window, picture;	/* the program's window, and the part of it the GPU draws the film in */
static GLuint texture;
static int scale, bar_y, held = -1;	/* held: the seek bar's knob is, at this picture */
static const char *name;
static char saying[80];

/* The machine's control words once the GPU's memory is mapped: the buttons and the keys' count. */
static const volatile uint32_t *machine;

static unsigned
milliseconds(void)
{
	struct timespec now;

	/* (the kernel's clock, not the machine's clock word: between the sound card's steps the
	 * film goes by this, and by the host's clock it ran ahead of its pictures) */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* A job for unit k: on its worker, or here and now when there are none. */
static void
post(int k, mcw_fn fn, uint32_t a0, uint32_t a1)
{
	if (workers)
		mcw_post(k, fn, a0, a1);
	else
		jobs[k].result = fn(a0, a1);
	jobs[k].busy = 1;
}

static int
finished(int k)
{
	if (!jobs[k].busy || (workers && !mcw_done(k)))
		return 0;
	if (workers)
		jobs[k].result = mcw_result(k);
	jobs[k].busy = 0;
	return 1;
}

static uint8_t *
slot(int frame)
{
	int n = frame % slots;

	return n < slots_a ? ring_a + (unsigned)n * info.frame_bytes : ring_b + (unsigned)(n - slots_a) * info.frame_bytes;
}

/* The last picture of those in GPU memory with none missing before it, from where it plays. */
static int
ready(void)
{
	/* (groups only become ready, so the walk goes on from where it stopped: play_from() begins it again) */
	for (; ready_gop < info.gops && gop_state[ready_gop] == 2; ready_gop++)
		ready_last = mpeg_gop_first(ready_gop) + mpeg_gop_frames(ready_gop) - 1;
	return ready_gop > first_gop ? ready_last : shown;
}

/* Gives every unit that has nothing to do its next job, and takes what the others finished. */
static void
dispatch(void)
{
	int k, i;

	for (k = 1; k <= unit_count; k++) {
		struct unit *u = &units[k];

		if (u->state == SOUND && finished(k)) {
			chunk_done[u->gop] = 1;
			u->state = IDLE;
		} else if (u->state == DECODING && finished(k)) {
			if (seeking >= 0)
				u->state = IDLE;	/* (it ended early: what it has is of no use) */
			else if ((int)jobs[k].result < u->frames)	/* its core was wanted: the rest of the group */
				post(k, mpeg_decode_gop, (uint32_t)u->video, (uint32_t)u->gop | MPEG_AGAIN);
			else
				u->state = DECODED;
		} else if (u->state == COPYING && finished(k)) {
			if (gop_sum)
				for (i = 0; i < u->frames; i++)
					gop_sum[u->gop] = mpeg_sum(slot(mpeg_gop_first(u->gop) + i), info.frame_bytes, gop_sum[u->gop]);
			gop_state[u->gop] = 2;
			u->state = IDLE;
		}
		if (u->state == DECODED && seeking >= 0)
			u->state = IDLE;
		/* a group goes to GPU memory once the frames its places hold have been shown */
		if (u->state == DECODED && mpeg_gop_first(u->gop) + u->frames - 1 <= shown + slots - 1) {
			for (i = 0; i < u->frames; i++)
				mpeg_video_to(u->video, i, slot(mpeg_gop_first(u->gop) + i));
			post(k, mpeg_copy_gop, (uint32_t)u->video, (uint32_t)u->frames);
			u->state = COPYING;
		}
		if (u->state != IDLE)
			continue;
		if (u->audio >= 0 && next_chunk < info.chunks) {
			u->gop = next_chunk++;
			u->state = SOUND;
			post(k, mpeg_decode_chunk, (uint32_t)u->audio, (uint32_t)u->gop);
		} else if (u->video >= 0 && next_gop < info.gops && seeking < 0) {
			u->gop = next_gop++;
			u->frames = mpeg_gop_frames(u->gop);
			gop_state[u->gop] = 1;
			u->state = DECODING;
			post(k, mpeg_decode_gop, (uint32_t)u->video, (uint32_t)u->gop);
		}
	}
	while (chunks_done < info.chunks && chunk_done[chunks_done])
		chunks_done++;
}

/* The sound frame the card is at, of all it has been given since it last began. */
static uint32_t
card_at(void)
{
	return SND_STATE[voice].seen != SND_VOICE[voice].key ? 0 : SND_STATE[voice].position;
}

static void
sound_run(int on)
{
	sound_running = on;
	if (sound_on)
		SND_VOICE[voice].step = on ? snd_step((uint32_t)info.rate) : 0;
}

/* Decoded sound into the card's ring, as far as that has room. */
static void
feed(void)
{
	uint32_t have = (uint32_t)chunks_done * MPEG_CHUNK * 1152u, room, at, n;
	int ahead;

	if (!sound_on)
		return;
	if (have > (uint32_t)info.pcm_frames)
		have = (uint32_t)info.pcm_frames;
	while (sound_from + written < have) {
		room = RING - (written - card_at());
		at = written & (RING - 1);
		n = have - sound_from - written;
		n = n > room ? room : n;
		n = n > RING - at ? RING - at : n;
		/* 4 KB a turn while the card has an eighth of a second to go on with: more is more
		 * than a pass's stores, and the pass would end here with a picture still to show */
		ahead = written - card_at() >= FEED_AHEAD;
		if (ahead && n > FEED_MOST)
			n = FEED_MOST;
		if (!n)
			return;
		memcpy(sound_ring + at * 2, mpeg_pcm() + (sound_from + written) * 2, n * 4);
		written += n;
		SND_VOICE[voice].loop = written;
		if (ahead)
			return;
	}
}

/* The film's time in milliseconds: where the sound card is, and the clock from there on. */
static unsigned
film_time(void)
{
	unsigned now = milliseconds();
	uint32_t at;

	if (sound_on && sound_running && (at = card_at()) != clock_seen && at <= written) {
		clock_seen = at;
		/* (whole seconds and the rest apart: a 64-bit division is a long library call) */
		clock_base = clock_from + at / (unsigned)info.rate * 1000 + at % (unsigned)info.rate * 1000 / (unsigned)info.rate;
		clock_wall = now;
	}
	return clock_base + (running ? now - clock_wall : 0);
}

static void
run(int on)
{
	unsigned now = film_time();

	clock_base = now;
	clock_wall = milliseconds();
	running = on;
	sound_run(on);
}

static unsigned
time_of(int frame)
{
	return (unsigned)((uint64_t)(frame < 0 ? 0 : frame) * 1000000u / (unsigned)info.fps_milli);
}

/* ---- the strip under the picture ---- */

static int
bar_width(void)
{
	return info.width * scale;
}

/* Where the seek bar is: between the Play key and the time. */
static void
seek_bar(int *x, int *w)
{
	*x = 6 + KEY_W + 8;
	*w = bar_width() - *x - 96 - KEY_W - 12;
}

static void
draw_bar(void)
{
	char text[40];
	unsigned at = time_of(held >= 0 ? held : seeking >= 0 ? seeking : shown), all = time_of(info.frames);
	int x, w, wide = bar_width();

	ui_fill(window, 0, bar_y, wide, BAR, UI_FACE);
	ui_button(window, 6, bar_y + 8, KEY_W, 24, !opened ? "" : paused || ended ? " Play" : " Pause", 0);
	ui_button(window, wide - KEY_W - 6, bar_y + 8, KEY_W, 24, " Open...", 0);
	seek_bar(&x, &w);
	if (saying[0]) {
		ui_text(window, x + 4, bar_y + 13, saying, -1, BLACK, 0);
		return;
	}
	if (w > 40)
		ui_slider(window, x, bar_y + 11, w, held >= 0 ? held : seeking >= 0 ? seeking : shown < 0 ? 0 : shown, info.frames > 1 ? info.frames - 1 : 1);
	/* after going elsewhere the film waits for a group of pictures: say so where the time is */
	if (opened && !running && !paused && !ended)
		snprintf(text, sizeof text, "decoding...");
	else
		snprintf(text, sizeof text, "%u:%02u / %u:%02u", at / 60000, at / 1000 % 60, all / 60000, all / 1000 % 60);
	ui_text(window, x + w + 8, bar_y + 13, text, -1, BLACK, 0);
}

static void
say(const char *text)
{
	snprintf(saying, sizeof saying, "%s", text);
	draw_bar();
	GrFlush();
}

static void
show(int frame)
{
	static const GLfixed corners[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};

	glBindTexture(GL_TEXTURE_2D, texture);
	seglTexturePointer(slot(frame), info.width, info.height, SEGL_YUV420);
	seglQuad(corners, uv, texture, 255, 255, 0);
	/* not waited for (that is a pass's end): no group is copied into the shown picture's place,
	 * and play_from() settles before the places are anybody's */
	seglSwapEarly();
	shown = frame;
}

/* Plays on from a picture's group: nothing is being decoded, and the sound is kept as decoded. */
static void
play_from(int frame)
{
	seglSettle();
	first_gop = next_gop = ready_gop = mpeg_gop_of(frame < 0 ? 0 : frame >= info.frames ? info.frames - 1 : frame);
	memset(gop_state, 0, (size_t)info.gops);
	shown = mpeg_gop_first(first_gop) - 1;
	clock_from = clock_base = time_of(shown + 1);
	sound_from = info.rate ? (uint32_t)((uint64_t)clock_from * (unsigned)info.rate / 1000) : 0;
	written = 0;
	clock_seen = 0;
	ended = 0;
	running = 0;
	if (sound_on)
		snd_play(voice, SND_STREAM | SND_SMOOTH | SND_STEREO, sound_ring_at, RING, 0, 0, SND_VOLUME(200, 200));
	sound_running = 0;
}

/* Goes to a picture: the groups being decoded are told to end, and it plays on from there
 * once they have (the loop below). */
static void
seek(int frame)
{
	if (!opened)
		return;
	seeking = frame < 0 ? 0 : frame >= info.frames ? info.frames - 1 : frame;
	run(0);
	mpeg_drop = 1;
	draw_bar();
}

static int
all_idle(int pictures_only)
{
	int k;

	for (k = 1; k <= unit_count; k++)
		if (units[k].state != IDLE && !(pictures_only && units[k].state == SOUND))
			return 0;
	return 1;
}

static void
leave(void)
{
	mpeg_stop = 1;	/* the groups being decoded end at their next picture: the workers are waited for */
	if (sound_on)
		snd_stop(voice);
	exit(0);
}

static void
while_choosing(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_bar();
}

/* Another file: this program again with it (its decoders are of this file's size). */
static void
open_another(void)
{
	char path[512];
	int was = running;

	run(0);
	if (!ui_choose_file("Open a video", access("/root/Videos", R_OK) == 0 ? "/root/Videos" : "/root", ".mpg .mpeg", path, sizeof path,
			    while_choosing)) {
		if (was && !paused)
			run(1);
		return;
	}
	mpeg_stop = 1;
	if (sound_on)
		snd_stop(voice);
	mcw_close();
	GrClose();
	execlp("nxvideo", "nxvideo", path, (char *)NULL);
	exit(1);
}

static void
toggle(void)
{
	if (ended) {
		seek(0);
		paused = 0;
	} else {
		paused = !paused;	/* (it plays on by itself once it is let go: the loop below) */
		if (paused)
			run(0);
	}
	draw_bar();
}

static void
events(void)
{
	GR_EVENT e;
	int x, w, step;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return;
		seek_bar(&x, &w);
		switch (e.type) {
		case GR_EVENT_TYPE_CLOSE_REQ:
			leave();
			break;
		case GR_EVENT_TYPE_EXPOSURE:
			if (e.exposure.wid == window)
				draw_bar();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (e.button.wid != window || e.button.y < bar_y || !(e.button.changebuttons & GR_BUTTON_L))
				break;
			if (e.button.x < 6 + KEY_W + 4)
				toggle();
			else if (e.button.x >= bar_width() - KEY_W - 6)
				open_another();
			else if (opened && e.button.x >= x && e.button.x < x + w) {
				held = ui_slider_value(e.button.x, x, w, info.frames - 1);
				draw_bar();
			}
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (held >= 0 && (e.mouse.buttons & GR_BUTTON_L)) {
				held = ui_slider_value(e.mouse.x, x, w, info.frames - 1);
				draw_bar();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (held >= 0) {
				seek(held);
				held = -1;
				draw_bar();
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			step = 5 * info.fps_milli / 1000;	/* five seconds of pictures */
			switch (e.keystroke.ch) {
			case 'q': case MWKEY_ESCAPE: leave(); break;
			case ' ': toggle(); break;
			case 'o': open_another(); break;
			case MWKEY_LEFT: seek((seeking >= 0 ? seeking : shown) - step); draw_bar(); break;
			case MWKEY_RIGHT: seek((seeking >= 0 ? seeking : shown) + step); draw_bar(); break;
			case MWKEY_HOME: seek(0); draw_bar(); break;
			}
			break;
		}
	}
}

int
main(int argc, char **argv)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	extern char __DATA_BEGIN__[], _end[];
	const char *path = argc > 1 ? argv[1] : SAMPLE, *text, *wrong;
	GR_SCREEN_INFO screen;
	char title[160];
	unsigned bytes, spare = 0, left, stat_at = 0, bar_at = 0, now, input = 0, hot_at = 0, asked_at = 0;
	uint8_t *file;
	int wanted, k, hold = -1, most, was_shown = -2, once = getenv("NXVIDEO_ONCE") != NULL;

	if (argc > 2 && !strcmp(argv[1], "--decode"))
		return decode_all(argv[2]);
	if ((file = load(path, &bytes)) == NULL)
		return 1;
	/* the pictures' size by a look at the file's start, and the window at once: taking the file
	 * apart and making the decoders is seconds, and a desktop with nothing new on it looks stuck */
	if (!mpeg_peek(file, bytes, &info.width, &info.height) || (info.width | info.height) & 15) {
		fprintf(stderr, "nxvideo: %s is not MPEG-1 video this plays (docs/video.md)\n", path);
		return 1;
	}
	info.frames = 1;
	info.fps_milli = 24000;
	if ((text = getenv("NXVIDEO_HOLD")) != NULL)
		hold = atoi(text);
	if (GrOpen() < 0) {
		fprintf(stderr, "nxvideo: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	ui_init();
	GrGetScreenInfo(&screen);
	for (scale = 3; scale > 1 && (info.width * scale > screen.cols - 20 || info.height * scale + BAR > screen.rows - 60); scale--)
		;
	bar_y = info.height * scale;
	name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	snprintf(title, sizeof title, "%.120s - Video", name);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE, title, GR_ROOT_WINDOW_ID, -1, -1, info.width * scale, bar_y + BAR, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_MOTION |
			       GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	picture = GrNewWindow(window, 0, 0, info.width * scale, bar_y, 0, BLACK, BLACK);
	GrMapWindow(picture);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	say("opening it...");

	if ((wrong = mpeg_open(file, bytes, &info)) != NULL) {
		fprintf(stderr, "nxvideo: %s: %s\n", path, wrong);
		say(wrong);
		for (;;) {	/* (the window says so until it is closed) */
			GR_EVENT e;

			GrGetNextEvent(&e);
			if (e.type == GR_EVENT_TYPE_CLOSE_REQ || e.type == GR_EVENT_TYPE_KEY_DOWN)
				return 1;
			if (e.type == GR_EVENT_TYPE_EXPOSURE)
				draw_bar();
		}
	}
	if (seglInit(picture) < 0) {
		fprintf(stderr, "nxvideo: this machine has no GPU\n");
		return 1;
	}
	machine = (const volatile uint32_t *)((const char *)seglPalette() - 0x400);
	/* frames in GPU memory: what is left to a program, and the display's spare framebuffer */
	left = seglMemoryLeft() & ~15u;
	ring_a = seglMemory(left);
	ring_b = seglMemorySpare(&spare);
	slots_a = ring_a ? (int)(left / info.frame_bytes) : 0;
	slots = slots_a + (ring_b ? (int)(spare / info.frame_bytes) : 0);
	if (slots < 2 * info.gop_most) {
		fprintf(stderr, "nxvideo: no GPU memory for pictures of this size\n");
		return 1;
	}
	glGenTextures(1, &texture);
	glViewport(0, 0, info.width * scale, bar_y);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);

	/* the workers: some for the sound, the rest a decoder each, as many as 24 MB of their
	 * groups' pictures allow */
	say("starting the workers...");
	wanted = (text = getenv("NXVIDEO_WORKERS")) != NULL ? atoi(text) : MPEG_VIDEOS + 8;
	atexit(mcw_close);
	workers = wanted > 0 ? mcw_open(wanted) : 0;
	most = (int)((24u << 20) / ((unsigned)info.gop_most * info.frame_bytes));
	/* (a second of sound is some 17 million instructions: several cores' work, as the pictures are;
	 * one worker: the pictures have it, and the film is silent) */
	sounds = !info.rate || workers == 1 ? 0 : workers >= MPEG_VIDEOS + 8 ? 8 : workers >= 6 ? workers / 4 : 1;
	unit_count = workers ? workers : 1 + sounds;
	if (unit_count > sounds + most)
		unit_count = sounds + most;
	if (unit_count > sounds + MPEG_VIDEOS)
		unit_count = sounds + MPEG_VIDEOS;
	/* Their decoders, all before a worker has a job: memory brought in on this core while
	 * workers run holds them up (made one a turn of the loop, no picture was ready in 13 s). */
	for (k = 1; k <= unit_count; k++) {
		units[k].video = units[k].audio = -1;
		if (k <= sounds)
			units[k].audio = mpeg_audio_new();
		else
			units[k].video = mpeg_video_new(k);
	}
	gop_state = calloc((size_t)info.gops + 1, 1);
	chunk_done = calloc((size_t)info.chunks + 1, 1);
	if (getenv("NXVIDEO_SUM"))
		gop_sum = calloc((size_t)info.gops + 1, sizeof *gop_sum);
	sound_on = sounds && snd_open() == 0 && (voice = snd_claim()) >= 0 && (sound_ring = snd_memory(RING * 4, &sound_ring_at)) != NULL;
	if (workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	printf("nxvideo: %s, %d x %d, %d.%03d pictures a second, %d of them; %d workers decode, %d the sound; room for %d pictures\n", name,
		info.width, info.height, info.fps_milli / 1000, info.fps_milli % 1000, info.frames, unit_count - sounds, sounds, slots);
	fflush(stdout);
	opened = 1;
	play_from(0);
	say("getting the first pictures ready...");

	for (;;) {
		int have, due, target, drew = 0;

		/* The window system is asked when the button or key words change, for a while
		 * after (the event is on its way) and while a button is down; else a few times a
		 * second: a question to it is more than a pass of this core. */
		now = milliseconds();
		if (input != (machine[0x28 / 4] ^ machine[0x2c / 4] << 8)) {
			input = machine[0x28 / 4] ^ machine[0x2c / 4] << 8;
			hot_at = now;
		}
		if (now - asked_at >= 150 || now - hot_at < 120 || (machine[0x28 / 4] & 7)) {
			asked_at = now;
			events();
		}
		dispatch();
		feed();
		if (seeking >= 0 && all_idle(1)) {
			/* the groups that were being decoded are gone: on from the picture asked for */
			mpeg_drop = 0;
			play_from(seeking);
			seeking = -1;
			draw_bar();
		}
		have = ready();
		if (gop_sum && first_gop == 0 && have == info.frames - 1) {
			uint32_t sum = 0;

			for (k = 0; k < info.gops; k++)
				sum = (sum << 7 | sum >> 25) ^ gop_sum[k];
			printf("nxvideo: sum %08x of %d pictures\n", sum, info.frames);
			fflush(stdout);
			free(gop_sum);
			gop_sum = NULL;
		}
		if (!running && !paused && !ended && seeking < 0 && (have >= shown + AHEAD || have == info.frames - 1) &&
		    (!sound_on || written > 0 || chunks_done == info.chunks)) {
			saying[0] = 0;
			run(1);
			draw_bar();
		}
		if (running) {
			/* film_time() * fps_milli / 1,000,000 exactly, in 32 bits */
			unsigned film = film_time(), turns = film / 1000 * (unsigned)info.fps_milli;

			due = (int)(turns / 1000 + (turns % 1000 * 1000 + film % 1000 * (unsigned)info.fps_milli) / 1000000u);
			target = due < have ? due : have;
			if (hold >= 0 && target > hold)
				target = hold;
			if (target > shown) {
				dropped += target - shown - 1;
				show(target);
				drew = 1;
			}
			if (hold >= 0 && shown == hold) {
				run(0);
				draw_bar();
				GrFlush();
				printf("nxvideo: holding picture %d\n", hold);
				fflush(stdout);
				for (;;)
					mc_next_pass();
			}
			if (shown == info.frames - 1) {
				running = 0;	/* (the sound card plays what it still has) */
				ended = 1;
			} else if (due > have + 1) {
				run(0);	/* it has run dry: the sound waits for the pictures */
			}
		}
		if (ended && seeking < 0 && all_idle(0) && chunks_done == info.chunks && (!sound_on || card_at() >= written)) {
			if (once)
				leave();
			play_from(0);
		}
		now = milliseconds();
		/* the bar follows the film four times a second, and at once when it stops or starts */
		if (now - bar_at >= 250 && shown != was_shown && held < 0) {
			draw_bar();
			was_shown = shown;
			bar_at = now;
		}
		if (now - stat_at >= 1000) {
			int busy = 0;

			for (k = 1; k <= unit_count; k++)
				busy += units[k].state == DECODING || units[k].state == COPYING || units[k].state == SOUND;
			printf("nxvideostat: picture %d of %d, %d left out, %d ready, %d of %d working, film at %u ms\n", shown, info.frames, dropped,
				have, busy, unit_count, film_time());
			fflush(stdout);
			stat_at = now;
		}
		/* nothing to show: the kernel has the core until the next pass (docs/multicore.md) */
		if (!drew)
			mcw_rest();
	}
}
#endif
