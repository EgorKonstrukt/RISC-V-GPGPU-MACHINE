/* nxvideo [FILE | --decode FILE]: plays an MPEG-1 film, a VQA, MJPEG in AVI or a GIF animation,
 * decoded ahead on the workers by libmedia into the GPU memory it is shown from; the picture
 * shown is the one the sound card has got to. Keys and switches: docs/video.md. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "media.h"
#include "mcw.h"

#define SAMPLE "/root/Videos/Big Buck Bunny.mpg"

static struct media_video *film;
static struct media_video_info info;

static unsigned
cycles(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

static uint8_t *
load(const char *path, unsigned *bytes)
{
	int fd = open(path, O_RDONLY);
	long size = fd < 0 ? -1 : lseek(fd, 0, SEEK_END), got = 0, n;
	uint8_t *file;

	if (size <= 0 || size > 64 << 20 || (file = mcw_alloc((unsigned)size)) == NULL) {
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

/* The sum the tests compare (each four bytes rotated in), as the player before libmedia had it. */
static uint32_t
rotated(const uint8_t *bytes, unsigned length, uint32_t sum)
{
	uint32_t word;
	unsigned i;

	for (i = 0; i + 4 <= length; i += 4) {
		memcpy(&word, bytes + i, 4);
		sum = (sum << 5 | sum >> 27) ^ word;
	}
	return sum;
}

/* Every picture and all of the sound decoded here, one after another, with their sums:
 * what any other way of decoding the file must come to. */
static int
decode_all(const char *path)
{
	unsigned bytes, before, spent, sum = 0;
	uint8_t *file = load(path, &bytes);
	const char *wrong;
	const uint32_t *palette;
	const uint8_t *picture;
	int16_t *pcm;
	uint32_t got = 0, more;
	int n;

	if (!file)
		return 1;
	if ((film = media_video_open(file, bytes, 0, &wrong)) == NULL) {
		fprintf(stderr, "nxvideo: %s: %s\n", path, wrong);
		return 1;
	}
	media_video_get_info(film, &info);
	printf("nxvideo: %s %d x %d, %d.%03d pictures a second, %d pictures, sound %d Hz\n", media_kind_name(info.kind), info.width,
	       info.height, info.fps_milli / 1000, info.fps_milli % 1000, info.frames, info.rate);
	before = cycles();
	for (n = 0; n < info.frames; n++) {
		while ((picture = media_video_frame(film, n, &palette)) == NULL)
			media_video_pump(film);
		sum = rotated(picture, info.frame_bytes, sum);
		if (palette)
			sum = rotated((const uint8_t *)palette, 1024, sum);
	}
	spent = cycles() - before;
	printf("nxvideo: pictures' sum %08x, %u thousand instructions a picture (the sound's with them)\n", sum,
	       info.frames ? spent / 1000 / (unsigned)info.frames : 0);
	if (info.rate) {
		pcm = malloc((size_t)info.sound_frames * (size_t)info.channels * 2 + 16);
		while (pcm && got < info.sound_frames) {
			if (!(more = media_video_sound(film, got, pcm + got * (uint32_t)info.channels, info.sound_frames - got)))
				media_video_pump(film);
			got += more;
		}
		printf("nxvideo: sound's sum %08x\n", pcm ? rotated((const uint8_t *)pcm, got * (uint32_t)info.channels * 2, 0) : 0);
	}
	media_video_close(film);
	return 0;
}

#include <GLES/segl.h>
#include "ui.h"
#include "ui_files.h"
#include "ui_ask.h"
#include "ui_glyph.h"
#include "mc.h"
#include "shaderemu_sound.h"

#define ONE 65536
#define RING 65536u		/* the sound stream's ring, in frames */
#define FEED_MOST 1024u		/* frames put into the ring a turn of the loop, */
#define FEED_AHEAD 4096u	/* while the card has so many still to play */
#define AHEAD 36		/* pictures in hand before it plays, or plays on after it ran dry: groups come in bursts */
#define BAR 40			/* the strip of controls under the picture */
#define KEY_W 62
#define TOOL_W 28		/* a key with a sign: Play or Pause, Stop, the loudspeaker */
#define VOLUME_W 56

static uint8_t *ring_a, *ring_b;	/* GPU memory for pictures: two pieces */
static int slots_a, slots;
static uint8_t **places;
static uint8_t *canvas[2];		/* an animation's pictures, as the GPU's words, in turn */
static int canvas_turn;
static const uint32_t *palette_shown;
static int shown = -1, dropped, summed = -1, summing;
static uint32_t picture_sum;
static int wait_for_groups = 1;	/* NXVIDEO_START=1: it plays as soon as it has pictures in hand, as it did */

static int sound_on, voice, sound_running;
static int still;		/* paused and gone elsewhere: the picture there is shown once it is decoded */
static int volume = 200, muted;	/* how loud, of 256; muted, the slider keeps its place */
static int16_t *sound_ring;
static uint32_t sound_ring_at, written, sound_from;	/* frames given to the card, and the film's frame its first is */
static unsigned clock_base, clock_wall, clock_from;	/* the film's time at a moment, that moment, and the time playing began at */
static uint32_t clock_seen;
static int running, paused, ended, opened;

static GR_WINDOW_ID window, picture;	/* the program's window, and the part of it the GPU draws the film in */
static GLuint texture;
static int win_w, pic_x, pic_y, pic_w, pic_h;	/* the window's width; where in it the film is */
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

/* The last picture in its place with none missing before it, from where it plays. */
static int
ready(int *in_time)
{
	int have = media_video_ready(film, in_time);

	return have > shown ? have : shown;
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

static int
sound_fed(void)
{
	return !sound_on || sound_from + written >= info.sound_frames;
}

/* Decoded sound into the card's ring (stereo: a film of one channel has it on both), as far as
 * that has room. */
static void
feed(void)
{
	static int16_t one[FEED_MOST];
	uint32_t room, at, n, got, i;
	int ahead;

	if (!sound_on)
		return;
	while (!sound_fed()) {
		room = RING - (written - card_at());
		at = written & (RING - 1);
		n = info.sound_frames - sound_from - written;
		n = n > room ? room : n;
		n = n > RING - at ? RING - at : n;
		/* 4 KB a turn while the card has an eighth of a second to go on with: more is more
		 * than a pass's stores, and the pass would end here with a picture still to show */
		ahead = written - card_at() >= FEED_AHEAD;
		if ((ahead || info.channels == 1) && n > FEED_MOST)
			n = FEED_MOST;
		if (!n)
			return;
		if (info.channels == 2) {
			got = media_video_sound(film, sound_from + written, sound_ring + at * 2, n);
		} else {
			got = media_video_sound(film, sound_from + written, one, n);
			for (i = 0; i < got; i++)
				sound_ring[(at + i) * 2] = sound_ring[(at + i) * 2 + 1] = one[i];
		}
		if (!got)
			return;
		written += got;
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
	return film ? media_video_time(film, frame < 0 ? 0 : frame) : 0;	/* (the strip is drawn before the file is open) */
}

/* ---- the strip under the picture ---- */

static int
bar_width(void)
{
	return win_w;
}

/* The loudspeaker's key is left of Open..., and the volume between them where there is room. */
static int
mute_x(void)
{
	int open = bar_width() - KEY_W - 6;

	return open - 6 - TOOL_W - (bar_width() >= 520 ? VOLUME_W + 4 : 0);
}

/* Where the seek bar is: between the Stop key and the time. */
static void
seek_bar(int *x, int *w)
{
	*x = 6 + 2 * TOOL_W + 2 + 8;
	*w = mute_x() - 96 - 6 - *x;
}

static void
set_volume(int to)
{
	volume = to < 0 ? 0 : to > 256 ? 256 : to;
	if (sound_on)
		SND_VOICE[voice].volume = muted ? 0 : SND_VOLUME(volume, volume);
	if (getenv("NXVIDEO_PRINT")) {	/* for a test: what the keys did */
		printf("nxvideo: volume %d%s\n", volume, muted ? ", muted" : "");
		fflush(stdout);
	}
}

static void
draw_bar(void)
{
	char text[40];
	unsigned at = time_of(held >= 0 ? held : shown), all = time_of(info.frames);
	int x, w, wide = bar_width();

	ui_fill(window, 0, bar_y, wide, BAR, UI_FACE);
	ui_tool(window, 6, bar_y + 8, TOOL_W, 24, paused || ended || !opened ? UI_G_PLAY : UI_G_PAUSE, opened ? 0 : 2);
	ui_tool(window, 6 + TOOL_W + 2, bar_y + 8, TOOL_W, 24, UI_G_STOP, opened ? 0 : 2);
	ui_tool(window, mute_x(), bar_y + 8, TOOL_W, 24, muted || !sound_on ? UI_G_MUTE : UI_G_SOUND, !sound_on ? 2 : muted);
	if (wide >= 520)
		ui_slider(window, mute_x() + TOOL_W + 4, bar_y + 11, VOLUME_W, volume, 256);
	ui_button(window, wide - KEY_W - 6, bar_y + 8, KEY_W, 24, " Open...", 0);
	seek_bar(&x, &w);
	if (saying[0]) {
		ui_text(window, x + 4, bar_y + 13, saying, -1, BLACK, 0);
		return;
	}
	if (w > 40)
		ui_slider(window, x, bar_y + 11, w, held >= 0 ? held : shown < 0 ? 0 : shown, info.frames > 1 ? info.frames - 1 : 1);
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

/* An animation's canvas (red, green, blue, alpha bytes) as the GPU's words, 0xTTRRGGBB. */
static uint8_t *
as_words(const uint8_t *in)
{
	uint32_t *out = (uint32_t *)canvas[canvas_turn ^= 1];
	unsigned i, n = (unsigned)(info.width * info.height);

	seglSettle();
	for (i = 0; i < n; i++, in += 4)
		out[i] = (uint32_t)(255 - in[3]) << 24 | (uint32_t)in[0] << 16 | (uint32_t)in[1] << 8 | in[2];
	return (uint8_t *)out;
}

static void
show(int frame)
{
	static const GLfixed corners[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	const uint32_t *palette;
	const uint8_t *pixels = media_video_frame(film, frame, &palette);

	if (!pixels)
		return;
	glBindTexture(GL_TEXTURE_2D, texture);
	if (info.format == MEDIA_FRAME_YUV420) {
		seglTexturePointer(pixels, info.width, info.height, SEGL_YUV420);
	} else if (info.format == MEDIA_FRAME_PAL8) {
		if (palette && palette != palette_shown) {
			memcpy(seglPalette(), palette, 1024);
			palette_shown = palette;
		}
		seglTexturePointer(pixels, info.width, info.height, GL_COLOR_INDEX8_EXT);
	} else if (info.format == MEDIA_FRAME_WORDS) {
		seglTexturePointer(pixels, info.width, info.height, GL_RGBA);
	} else {
		seglTexturePointer(as_words(pixels), info.width, info.height, GL_RGBA);
	}
	seglQuad(corners, uv, texture, 255, 255, 0);
	/* not waited for (that is a pass's end): no picture is decoded into the shown one's place,
	 * and play_from() settles before the places are anybody's */
	seglSwapEarly();
	shown = frame;
}

/* Plays on from a picture's group: what was decoded is dropped, and the sound is kept as decoded. */
static void
play_from(int frame)
{
	seglSettle();
	if (shown >= 0)
		summing = 0;	/* (NXVIDEO_SUM: the pictures of a film played from its start once) */
	shown = media_video_seek(film, frame) - 1;
	summed = shown;
	clock_from = clock_base = time_of(shown + 1);
	sound_from = info.rate ? (uint32_t)((uint64_t)clock_from * (unsigned)info.rate / 1000) : 0;
	written = 0;
	clock_seen = 0;
	ended = 0;
	running = 0;
	still = 1;
	if (sound_on)
		snd_play(voice, SND_STREAM | SND_SMOOTH | SND_STEREO, sound_ring_at, RING, 0, 0, muted ? 0 : SND_VOLUME(volume, volume));
	sound_running = 0;
}

static void
seek(int frame)
{
	if (!opened)
		return;
	run(0);
	play_from(frame < 0 ? 0 : frame >= info.frames ? info.frames - 1 : frame);
	draw_bar();
}

static void
leave(void)
{
	if (sound_on)
		snd_stop(voice);
	media_video_close(film);	/* (the pictures being decoded end at their next one: the workers are waited for) */
	exit(0);
}

static int closing;	/* the window's box was pressed while a file was being chosen */

static void
while_choosing(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_bar();
	/* the player's own box while the file window is up: both go */
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		ui_files_stop = closing = 1;
}

/* Another file: this program again with it (its places are of this film's size). */
static void
open_another(void)
{
	char path[512];
	int was = running;

	run(0);
	if (!ui_choose_file("Open a video", access("/root/Videos", R_OK) == 0 ? "/root/Videos" : "/root", ".mpg .mpeg .vqa .avi .gif", path,
			    sizeof path, while_choosing)) {
		if (closing)
			leave();
		if (was && !paused)
			run(1);
		return;
	}
	if (sound_on)
		snd_stop(voice);
	media_video_close(film);
	GrClose();
	execlp("nxvideo", "nxvideo", path, (char *)NULL);
	exit(1);
}

/* Black round the film, where the window is larger than it. */
static void
draw_round(void)
{
	ui_fill(window, 0, 0, win_w, pic_y, BLACK);
	ui_fill(window, 0, pic_y + pic_h, win_w, bar_y - pic_y - pic_h, BLACK);
	ui_fill(window, 0, pic_y, pic_x, pic_h, BLACK);
	ui_fill(window, pic_x + pic_w, pic_y, win_w - pic_x - pic_w, pic_h, BLACK);
}

/* The window is another size (its caption's box): the film as large as fits above the strip
 * of controls, in the middle, and the strip along the bottom. */
static void
fit(int w, int h)
{
	win_w = w;
	bar_y = h - BAR;
	pic_w = w;
	pic_h = info.height * w / info.width;
	if (pic_h > bar_y) {
		pic_h = bar_y > 16 ? bar_y : 16;
		pic_w = info.width * pic_h / info.height;
	}
	pic_x = (w - pic_w) / 2;
	pic_y = (bar_y - pic_h) / 2;
	GrMoveWindow(picture, pic_x, pic_y);
	GrResizeWindow(picture, pic_w, pic_h);
	draw_round();
	draw_bar();
	if (!texture)
		return;		/* (nothing is drawn by the GPU yet) */
	seglWindowChanged();
	glViewport(0, 0, pic_w, pic_h);
	if (shown >= 0)
		show(shown);
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

/* Stop: the film waits at its start. */
static void
stop(void)
{
	if (!opened)
		return;
	if (!paused && !ended)
		toggle();
	seek(0);
	paused = 1;
	draw_bar();
}

static void
events(void)
{
	static int loud_held;	/* the volume's knob is held */
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
			if (e.exposure.wid == window) {
				draw_round();
				draw_bar();
			}
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (e.update.wid == window && e.update.utype == GR_UPDATE_SIZE && (e.update.width != win_w || e.update.height != bar_y + BAR))
				fit(e.update.width, e.update.height);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&e)) {	/* the wheel is the volume */
				set_volume(volume - 16 * ui_wheel(&e));
				draw_bar();
				break;
			}
			if (!(e.button.changebuttons & GR_BUTTON_L))
				break;
			if (e.button.wid != window || e.button.y < bar_y) {
				if (opened)
					toggle();	/* a click on the film: pause, or play on */
				break;
			}
			if (e.button.x < 6 + TOOL_W)
				toggle();
			else if (e.button.x < 6 + 2 * TOOL_W + 4)
				stop();
			else if (e.button.x >= bar_width() - KEY_W - 6)
				open_another();
			else if (e.button.x >= mute_x() && e.button.x < mute_x() + TOOL_W) {
				muted = !muted;
				set_volume(volume);
				draw_bar();
			} else if (e.button.x >= mute_x() + TOOL_W && bar_width() >= 520) {
				loud_held = 1;
				set_volume(ui_slider_value(e.button.x, mute_x() + TOOL_W + 4, VOLUME_W, 256));
				draw_bar();
			} else if (opened && e.button.x >= x && e.button.x < x + w) {
				held = ui_slider_value(e.button.x, x, w, info.frames - 1);
				draw_bar();
			}
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (held >= 0 && (e.mouse.buttons & GR_BUTTON_L)) {
				held = ui_slider_value(e.mouse.x, x, w, info.frames - 1);
				draw_bar();
			} else if (loud_held && (e.mouse.buttons & GR_BUTTON_L)) {
				set_volume(ui_slider_value(e.mouse.x, mute_x() + TOOL_W + 4, VOLUME_W, 256));
				draw_bar();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			loud_held = 0;
			if (held >= 0) {
				seek(held);
				held = -1;
				draw_bar();
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			step = info.fps_milli < 1000 ? 5 : 5 * info.fps_milli / 1000;	/* five seconds of pictures */
			switch (e.keystroke.ch) {
			case 'q': case MWKEY_ESCAPE: leave(); break;
			case ' ': toggle(); break;
			case 's': stop(); break;
			case 'm': muted = !muted; set_volume(volume); draw_bar(); break;
			case MWKEY_UP: case '+': case '=': set_volume(volume + 16); draw_bar(); break;
			case MWKEY_DOWN: case '-': set_volume(volume - 16); draw_bar(); break;
			case MWKEY_END: seek(info.frames - 1); break;
			case 'o': open_another(); break;
			case MWKEY_LEFT: seek(shown - step); break;
			case MWKEY_RIGHT: seek(shown + step); break;
			case MWKEY_HOME: seek(0); break;
			}
			break;
		}
	}
}

/* Why a file is not played, on the console and (on a desktop) in a window: the player went
 * without a word, and from Open... the film before went with it. Returns 1, for main. */
static int
refuse(const char *path, const char *why, int opened)
{
	static const char *const answers[] = { "OK" };
	char text[300];

	fprintf(stderr, "nxvideo: %s: %s\n", path, why);
	if (!opened && GrOpen() < 0)
		return 1;
	if (!opened)
		ui_init();
	snprintf(text, sizeof text, "%.150s\n%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, why);
	ui_ask("Video", text, answers, 1, NULL);
	GrClose();
	return 1;
}

/* The pictures' size by a look at the file's start (an MPEG-1 sequence header, a VQA's, an
 * AVI's main header or a GIF's own), for the window to be there before the file is taken apart. */
static int
peek(const uint8_t *file, unsigned bytes, int *width, int *height)
{
	struct media_info about;
	unsigned at;

	if (bytes >= 30 && !memcmp(file, "FORM", 4) && !memcmp(file + 8, "WVQAVQHD", 8)) {
		*width = file[26] | file[27] << 8;
		*height = file[28] | file[29] << 8;
		return *width > 0 && *height > 0;
	}
	if (bytes >= 72 && !memcmp(file, "RIFF", 4) && !memcmp(file + 8, "AVI LIST", 8) && !memcmp(file + 20, "hdrlavih", 8)) {
		*width = file[64] | file[65] << 8;
		*height = file[68] | file[69] << 8;
		return *width > 0 && *height > 0;
	}
	if (!media_probe(file, bytes, &about) && about.kind == MEDIA_GIF) {
		*width = about.width, *height = about.height;
		return *width > 0 && *height > 0;
	}
	/* 00 00 01 B3, then twelve bits of width and twelve of height */
	for (at = 0; at + 8 <= bytes && at < 65536; at++)
		if (file[at] == 0 && file[at + 1] == 0 && file[at + 2] == 1 && file[at + 3] == 0xb3) {
			*width = file[at + 4] << 4 | file[at + 5] >> 4;
			*height = (file[at + 5] & 15) << 8 | file[at + 6];
			return *width > 0 && *height > 0 && !((*width | *height) & 15);
		}
	return 0;
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
	int wanted, k, hold = -1, was_shown = -2, ahead, once = getenv("NXVIDEO_ONCE") != NULL;

	if (argc > 2 && !strcmp(argv[1], "--decode"))
		return decode_all(argv[2]);
	if ((file = load(path, &bytes)) == NULL)
		return refuse(path, "cannot be read", 0);
	/* the pictures' size by a look at the file's start, and the window at once: taking the file
	 * apart and starting the workers is seconds, and a desktop with nothing new on it looks stuck */
	if (!peek(file, bytes, &info.width, &info.height))
		return refuse(path, "is not a film this plays: MPEG-1, VQA, MJPEG in AVI or GIF (docs/video.md)", 0);
	info.frames = 1;
	info.fps_milli = 24000;
	summing = getenv("NXVIDEO_SUM") != NULL;
	if ((text = getenv("NXVIDEO_HOLD")) != NULL)
		hold = atoi(text);
	wait_for_groups = getenv("NXVIDEO_START") == NULL;
	if (GrOpen() < 0) {
		fprintf(stderr, "nxvideo: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	ui_init();
	GrGetScreenInfo(&screen);
	for (scale = 3; scale > 1 && (info.width * scale > screen.cols - 20 || info.height * scale + BAR > screen.rows - 60); scale--)
		;
	bar_y = info.height * scale;
	win_w = pic_w = info.width * scale, pic_h = bar_y;
	name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	snprintf(title, sizeof title, "%.120s - Video", name);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE, title, GR_ROOT_WINDOW_ID, -1, -1, info.width * scale, bar_y + BAR, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_MOTION |
			       GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	picture = GrNewWindow(window, 0, 0, info.width * scale, bar_y, 0, BLACK, BLACK);
	GrMapWindow(picture);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	say("opening it...");

	/* the workers: an MPEG-1 film takes what it needs of them (a decoder each for 16 at the
	 * sample's size, more for larger pictures, and 8 for the sound), a VQA's frame rows 16 */
	wanted = (text = getenv("NXVIDEO_WORKERS")) != NULL ? atoi(text) : file[0] == 'F' ? 16 : file[0] == 'G' ? 8 : 64;
	if ((film = media_video_open(file, bytes, wanted, &wrong)) == NULL) {
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
	media_video_get_info(film, &info);
	if (seglInit(picture) < 0) {
		return refuse(path, "this machine has no GPU to show it with", 1);
	}
	machine = (const volatile uint32_t *)((const char *)seglPalette() - 0x400);
	/* pictures in GPU memory: what is left to a program, and the display's spare framebuffer */
	left = seglMemoryLeft() & ~15u;
	ring_a = seglMemory(left);
	ring_b = seglMemorySpare(&spare);
	if (info.format == MEDIA_FRAME_RGBA) {
		/* an animation's canvas, as the GPU's words, in two places in turn */
		if (!ring_a || left < 2 * info.frame_bytes)
			return refuse(path, "no GPU memory for pictures of this size", 1);
		canvas[0] = ring_a, canvas[1] = ring_a + info.frame_bytes;
		slots = 2;
	} else {
		slots_a = ring_a ? (int)(left / info.frame_bytes) : 0;
		slots = slots_a + (ring_b ? (int)(spare / info.frame_bytes) : 0);
		places = malloc(sizeof *places * (size_t)(slots > 0 ? slots : 1));
		for (k = 0; k < slots; k++)
			places[k] = k < slots_a ? ring_a + (unsigned)k * info.frame_bytes : ring_b + (unsigned)(k - slots_a) * info.frame_bytes;
		if (slots < info.least_places || media_video_places(film, places, slots) != NULL)
			return refuse(path, "no GPU memory for pictures of this size", 1);
	}
	glGenTextures(1, &texture);
	glViewport(0, 0, info.width * scale, bar_y);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);

	sound_on = info.rate && snd_open() == 0 && (voice = snd_claim()) >= 0 && (sound_ring = snd_memory(RING * 4, &sound_ring_at)) != NULL;
	if (info.workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	printf("nxvideo: %s, %s %d x %d, %d.%03d pictures a second, %d of them; %d workers decode; room for %d pictures\n", name,
	       media_kind_name(info.kind), info.width, info.height, info.fps_milli / 1000, info.fps_milli % 1000, info.frames, info.workers,
	       slots);
	fflush(stdout);
	/* (pictures in hand before it plays: as many as there are places for, and a group's worth) */
	ahead = AHEAD < slots - 1 ? AHEAD : slots - 1;
	opened = 1;
	play_from(0);
	say("getting the first pictures ready...");

	for (;;) {
		int have, due, target, drew = 0, in_time;

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
		media_video_pump(film);
		feed();
		have = ready(&in_time);
		/* NXVIDEO_SUM: every picture where it is shown from, once, as it comes there */
		if (summing && info.format != MEDIA_FRAME_RGBA) {
			while (summed < have) {
				summed++;
				picture_sum = rotated(places[summed % slots], info.frame_bytes, picture_sum);
			}
			if (summed == info.frames - 1) {
				printf("nxvideo: sum %08x of %d pictures\n", picture_sum, info.frames);
				fflush(stdout);
				summing = 0;
			}
		}
		if (!running && !paused && !ended && (have >= shown + ahead || have == info.frames - 1) &&
		    (!sound_on || written > 0 || sound_fed()) && (have == info.frames - 1 || !wait_for_groups || in_time)) {
			saying[0] = 0;
			run(1);
			draw_bar();
		}
		if (paused && still && have > shown) {
			still = 0;
			show(shown + 1);
			draw_bar();
		}
		if (running) {
			due = media_video_at(film, film_time());
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
		if (ended && sound_fed() && (!sound_on || card_at() >= written)) {
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
			printf("nxvideostat: picture %d of %d, %d left out, %d ready, %d workers, film at %u ms\n", shown, info.frames, dropped, have,
			       info.workers, film_time());
			fflush(stdout);
			stat_at = now;
		}
		/* nothing to show: the kernel has the core until the next pass (docs/multicore.md) */
		if (!drew)
			mcw_rest();
	}
}
