/*
 * nxplay: an audio player (docs/play.md). nxplay FILE plays a WAV, MOD, FLAC, MP3 or Ogg Vorbis
 * file through the sound card's stream voice, decoded by libmedia (docs/media.md). A tune is
 * decoded in pieces, ahead of what is heard; an MP3, Vorbis or FLAC file's pieces go to the
 * worker cores. The NXPLAY_* variables are in docs/play.md.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include "ui.h"
#include "ui_files.h"
#include "ui_glyph.h"
#include <dirent.h>
#include "shaderemu_sound.h"
#include "mcw.h"
#include "media.h"

#define RING ring_frames	/* the stream's ring, in frames: a power of two */
#define RING_LONG 262144u	/* six seconds of 44.1 kHz: room for every piece that is being decoded */
#define RING_SHORT 65536u	/* when sound memory has no megabyte left */
#define SLICE 2048	/* frames this core decodes between two looks at everything else */
#define OWN_UNDER 8	/* with fewer workers than this the first core takes pieces too */
#define FLAC_WORKERS 24	/* a FLAC file's: a block is a tenth of a second, a worker's for half a second */
#define SLOTS 2048
#define W 440
#define KEYS_Y 70
#define KEY_H 24
#define VOLUME_X 358
#define VOLUME_W 74
#define LIST_Y 122
#define LIST_ROWS 8
#define H (LIST_Y + LIST_ROWS * UI_ROW + 4 + 8)
#define KINDS ".wav .mp3 .flac .mod .ogg"
#define TUNES 256

enum { K_NONE, K_WAV, K_MOD, K_FLAC, K_MP3, K_OGG };
enum { STOPPED, BUFFERING, PLAYING, ENDED };
enum { SLOT_FREE, SLOT_MINE, SLOT_WORKER, SLOT_READY, SLOT_DROPPED };

/* Frames are the library's: at half rate a frame is two of the file's. */
static struct {
	char path[512];
	const char *name;
	uint8_t *file;
	uint32_t size;
	struct media_audio *audio;
	int kind, rate, channels, half;
	int pieced;		/* the library's pieces, on the workers (else read in order on this core) */
	uint32_t total;		/* ~0u while not known */
	uint32_t piece;		/* frames a piece (the library's first, when pieced) */
	uint32_t origin;	/* the frame piece 0 begins at, in order */
} tune;

/* A piece's memory: 16 bytes its worker writes (frames done), 16 this core writes (give up),
 * then the samples. */
static struct slot {
	uint32_t *head;
	int16_t *pcm, *own;	/* where its samples go: its own memory, or its place in the ring */
	uint32_t piece, frames, done;
	int state, direct;
} slots[SLOTS];
static int slot_count, slot_most;
static uint32_t slot_bytes;
static struct slot *mine, *job[MC_MAX_CORES];
static int workers, workers_wanted = MC_MAX_CORES - 1;

static struct {
	int on, voice, volume, running;
	int16_t *ring;
	uint32_t ring_at;	/* the ring's physical address */
	uint32_t written;
	uint32_t base;		/* the tune's frame at the ring's first */
	uint32_t first;		/* and the piece that begins there */
} out;
static uint32_t ring_frames = RING_SHORT;

static int state, paused, quit;
static uint32_t play_piece, play_at, next_piece;
static uint32_t consumed;	/* without a card: frames taken */
static uint32_t decoded, work_ms, last_ms;	/* since the last seek, for the speed */
static int was_working, decided, half_wanted = -1, underruns;
static uint64_t own_cycles, worker_cycles, all_decoded;
static uint32_t began_ms, began_cycles, limit_seconds, memory_mb;
static int want_sum, exit_at_end, mod_rate = 22050, piece_frames_asked;
static int own_under = OWN_UNDER;
static uint32_t sum = 2166136261u, sum_count;
static char note[160];

static GR_WINDOW_ID window;
static int window_on, drag, drag_value, shown_second = -1, shown_state = -1;
/* The list: the tunes of the folder the one played is in, by name. */
static char tunes[TUNES][100], list_folder[512];
static struct ui_list list = { 8, LIST_Y, W - 16, LIST_ROWS * UI_ROW + 4, 0, 0, -1 };
static int repeat, shuffle, muted;
static int want_next;		/* the tune ended: the next of the list is to be played */
enum { B_PREV, B_PLAY, B_STOP, B_NEXT, B_REPEAT, B_SHUFFLE, B_OPEN, B_MUTE, BUTTONS };
static const struct { short x, w; } keys[BUTTONS] = { {8, 30}, {40, 30}, {72, 30}, {104, 30}, {142, 54}, {200, 56}, {264, 58}, {330, 24} };

static uint32_t
cycles(void)
{
	uint32_t n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

static uint32_t
now_ms(void)
{
	struct timeval t;

	gettimeofday(&t, NULL);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_usec / 1000);
}

static void
say(const char *text)
{
	snprintf(note, sizeof note, "%s", text);
	printf("nxplay: %s\n", text);
	fflush(stdout);
	shown_state = -1;
}

static uint32_t
pieces(void)
{
	if (tune.pieced)
		return media_audio_piece_of(tune.audio, tune.total - 1) + 1;
	return tune.total == ~0u ? ~0u : (tune.total - tune.origin + tune.piece - 1) / tune.piece;
}

/* The frame a piece begins at, and its frames as decoded (`whole`) or as played (up to the tune's end). */
static uint32_t
piece_start(uint32_t piece)
{
	return tune.pieced ? media_audio_piece_first(tune.audio, piece) : tune.origin + piece * tune.piece;
}

static uint32_t
piece_size(uint32_t piece, int whole)
{
	uint32_t from = piece_start(piece), n = tune.pieced ? media_audio_piece_frames(tune.audio, piece) : tune.piece;

	return whole || tune.total - from >= n ? n : tune.total - from;
}

static uint32_t
piece_frames(uint32_t piece)
{
	return piece_size(piece, 0);
}

/* ---- a piece on a worker ---- */

/* A worker's job: piece `piece` into the slot at `at` (or the place its head names) with its
 * own decoder, a step at a time: it says how far it is, and stops when told to. */
static uint32_t
piece_job(uint32_t piece, uint32_t at)
{
	volatile uint32_t *head = (volatile uint32_t *)at;
	int16_t *to = head[5] ? (int16_t *)head[5] : (int16_t *)(at + 32);
	uint32_t before = cycles(), want = media_audio_piece_frames(tune.audio, piece), done = 0;
	int d = (int)head[7];

	while (done < want && !head[4]) {
		done = media_audio_piece_step(tune.audio, d, piece, to, done);
		head[0] = done;
	}
	return cycles() - before;
}

/* ---- the card ---- */

/* The frame of the ring the card is at. */
static uint32_t
card_at(void)
{
	return SND_STATE[out.voice].seen != SND_VOICE[out.voice].key ? 0 : SND_STATE[out.voice].position;
}

/* A step of 0 holds the voice where it is. */
static void
voice_run(int on)
{
	out.running = on;
	if (out.on)
		SND_VOICE[out.voice].step = on ? snd_step((uint32_t)tune.rate) : 0;
}

/* The tune's frame that is heard now. */
static uint32_t
position(void)
{
	uint32_t at = out.base + (out.on ? card_at() : consumed);

	return state == ENDED || at > tune.total ? tune.total : at;
}

/* ---- pieces ---- */

/* A slot for one more piece while fewer than `most` are in use; `look`: only whether there is one. */
static struct slot *
slot_get(int most, int look)
{
	struct slot *s = NULL;
	uint8_t *memory;
	int i, used = 0;

	for (i = 0; i < slot_count; i++) {
		if (slots[i].state != SLOT_FREE)
			used++;
		else if (!s)
			s = &slots[i];
	}
	if (used >= most || s || slot_count >= slot_most)
		return used >= most ? NULL : s;
	if (look)
		return &slots[slot_count];
	memory = mmap(NULL, slot_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) {
		slot_most = slot_count;
		return NULL;
	}
	/* its pages there before a worker stores to them */
	mcw_touch(memory, slot_bytes);
	s = &slots[slot_count++];
	s->head = (uint32_t *)memory;
	s->pcm = s->own = (int16_t *)(memory + 32);
	return s;
}

static struct slot *
slot_of(uint32_t piece)
{
	int i;

	for (i = 0; i < slot_count; i++)
		if (slots[i].state == SLOT_READY && slots[i].piece == piece)
			return &slots[i];
	return NULL;
}

/*
 * A piece's place in the ring, when it has one now: it is decoded there, and copied by nobody.
 * The stream's frames are the pieces' in order, so the place is known before the piece is
 * made; it must be clear of what is still to be heard, and not go round the ring's end.
 * `far`: it has none because the ring is full that far ahead.
 */
static int16_t *
piece_place(uint32_t piece, int *far)
{
	uint32_t first = piece_start(piece) - out.base, n = piece_size(piece, 1);
	uint32_t at = first & (RING - 1);

	*far = out.on && first + n > card_at() + RING;
	if (!out.on || *far || at + n > RING)
		return NULL;
	return out.ring + at * (uint32_t)tune.channels;
}

/* Whether a worker still writes for a piece that was dropped: its place may be another's by now. */
static int
dropping(void)
{
	int i;

	for (i = 0; i < slot_count; i++)
		if (slots[i].state == SLOT_DROPPED)
			return 1;
	return 0;
}

/* Forgets every piece decoded or begun; a worker's is given back when it has heard. */
static void
drop_all(void)
{
	int i;

	for (i = 0; i < slot_count; i++) {
		if (slots[i].state == SLOT_WORKER) {
			slots[i].head[4] = 1;
			slots[i].state = SLOT_DROPPED;
		} else if (slots[i].state != SLOT_DROPPED) {
			slots[i].state = SLOT_FREE;
		}
	}
	mine = NULL;
}

/* Frames decoded since the last seek, the workers' unfinished pieces included. */
static uint32_t
progress(void)
{
	uint32_t frames = decoded;
	int k;

	for (k = 1; k <= workers; k++)
		if (job[k] && job[k]->state == SLOT_WORKER)
			frames += job[k]->head[0];
	return frames;
}

/* Sound decoded for a second of decoding, in thousandths; 0 while too little is known. */
static uint32_t
speed(void)
{
	if (work_ms < 300)
		return 0;
	return (uint32_t)((uint64_t)progress() * 1000000u / ((uint64_t)work_ms * (uint32_t)tune.rate)) + 1;
}

static void
seek_to(uint32_t frame)
{
	drop_all();
	if (tune.total != ~0u && frame >= tune.total)
		frame = tune.total ? tune.total - 1 : 0;
	if (tune.pieced) {
		play_piece = next_piece = media_audio_piece_of(tune.audio, frame);
		out.base = piece_start(play_piece);
	} else {
		tune.origin = out.base = media_audio_seek(tune.audio, frame);
		play_piece = next_piece = 0;
	}
	out.first = play_piece;
	play_at = consumed = 0;
	decoded = work_ms = 0;
	was_working = 0;
	out.written = 0;
	if (out.on)
		snd_play(out.voice, SND_STREAM | SND_SMOOTH | (tune.channels == 2 ? SND_STEREO : 0), out.ring_at, RING, 0, 0,
			 SND_VOLUME(out.volume, out.volume));
	out.running = 0;
	state = BUFFERING;
}

/* One step of this core's own piece: a step of the library's piece (an MP3 frame, a FLAC
 * block...), or a slice of a tune read in order. */
static void
own_step(void)
{
	struct slot *s = mine;
	uint32_t want = piece_frames(s->piece), before = cycles(), was = s->done;

	if (tune.pieced) {
		s->done = media_audio_piece_step(tune.audio, 0, s->piece, s->pcm, s->done);
	} else {
		int ask = want - s->done > SLICE ? SLICE : (int)(want - s->done);
		int got = ask > 0 ? media_audio_read(tune.audio, s->pcm + s->done * (uint32_t)tune.channels, ask) : 0;

		s->done += (uint32_t)got;
		if (got < ask) {
			/* the tune's end, sooner than it said or where it did not say */
			tune.total = tune.origin + s->piece * tune.piece + s->done;
			want = s->done;
		}
	}
	decoded += s->done - was;
	all_decoded += s->done - was;
	own_cycles += cycles() - before;
	if (s->done >= want) {
		s->frames = want;
		s->state = SLOT_READY;
		mine = NULL;
	}
}

/* Finished pieces into the ring, in order, as far as it has room: the few that were not
 * decoded in it are copied. */
static void
feed(void)
{
	struct slot *s;

	while ((s = slot_of(play_piece)) != NULL) {
		const int16_t *from = s->pcm + play_at * (uint32_t)tune.channels;
		uint32_t n = s->frames - play_at, i;

		if (!n) {
			s->state = SLOT_FREE;
			play_piece++;
			play_at = 0;
			continue;
		}
		if (s->direct) {
			/* (it was decoded where it lies: the card is told, and that is all) */
			out.written += n;
			SND_VOICE[out.voice].loop = out.written;
		} else if (out.on) {
			uint32_t room = RING - (out.written - card_at()), at = out.written & (RING - 1);

			n = n > room ? room : n;
			n = n > RING - at ? RING - at : n;
			if (!n)
				return;
			memcpy(out.ring + at * (uint32_t)tune.channels, from, n * (uint32_t)tune.channels * 2);
			out.written += n;
			SND_VOICE[out.voice].loop = out.written;
		} else {
			consumed += n;
		}
		if (want_sum) {
			for (i = 0; i < n * (uint32_t)tune.channels; i++)
				sum = (sum ^ (uint16_t)from[i]) * 16777619u;
			sum_count += n * (uint32_t)tune.channels;
		}
		play_at += n;
	}
}

static void
stats(void)
{
	uint32_t ms = now_ms() - began_ms, all = cycles() - began_cycles;
	uint64_t per = all_decoded ? (own_cycles + worker_cycles) * (uint32_t)tune.rate / all_decoded : 0;
	static const char *kinds[] = { "", "wav", "mod", "flac", "mp3", "ogg" };

	if (!all_decoded)
		return;
	printf("playstat: %s %d Hz %d ch%s, %u ms of sound decoded in %u ms: %u thousand instructions a second of sound "
	       "(this core %u, %d workers %u), this core ran %u thousand in all, %d underruns\n",
	       kinds[tune.kind], tune.rate, tune.channels, tune.half ? " at half rate" : "",
	       (uint32_t)(all_decoded * 1000 / (uint32_t)tune.rate), ms, (uint32_t)(per / 1000),
	       (uint32_t)(own_cycles * (uint32_t)tune.rate / all_decoded / 1000), workers,
	       (uint32_t)(worker_cycles * (uint32_t)tune.rate / all_decoded / 1000), all / 1000, underruns);
	if (want_sum)
		printf("nxplay: sum %08x of %u samples\n", sum, sum_count);
	fflush(stdout);
	all_decoded = own_cycles = worker_cycles = 0;
}

static void
finish(void)
{
	voice_run(0);
	state = ENDED;
	stats();
	if (exit_at_end || !window_on)
		quit = 1;
	else
		want_next = 1;
}

/* What the library says of the tune now (its rate and frames are halved at half rate). */
static void
tune_info(void)
{
	struct media_audio_info info;

	media_audio_get_info(tune.audio, &info);
	tune.rate = info.rate;
	tune.channels = info.channels;
	tune.half = info.half;
	tune.total = info.frames ? info.frames : ~0u;
	if (limit_seconds && tune.total > limit_seconds * (uint32_t)tune.rate)
		tune.total = limit_seconds * (uint32_t)tune.rate;
	if (tune.pieced)
		tune.piece = media_audio_piece_frames(tune.audio, 0);
}

/* Drops every piece and waits for the workers to leave theirs (at their next step). */
static void
settle(void)
{
	int k;

	drop_all();
	for (k = 1; k <= workers; k++)
		if (job[k]) {
			mcw_wait(k);
			worker_cycles += mcw_result(k);
			job[k]->state = SLOT_FREE;
			job[k] = NULL;
		}
}

/* An MP3 file from where it is on at half the rate. */
static void
go_half(const char *why)
{
	uint32_t at = position();

	settle();
	media_audio_half(tune.audio, 1);
	tune_info();
	say(why);
	seek_to(at / 2);
}

/* Whether to play, wait for more, or change how it is decoded. */
static void
decide(int stuck)
{
	uint32_t count = pieces(), ahead, left, need, rate, s, p;
	struct slot *ready;

	if (state != BUFFERING && state != PLAYING)
		return;
	if (!out.on) {
		if (play_piece >= count)
			finish();
		return;
	}
	ahead = out.written - card_at();
	if (state == PLAYING) {
		if (ahead || paused)
			return;
		if (play_piece >= count) {
			finish();
		} else {
			voice_run(0);
			state = BUFFERING;
			underruns++;
			/* With workers this is the one sign that they do not keep up: the second time, an
			 * MP3 file goes on at half the rate. (Their first two seconds say nothing: they are
			 * being started, and it was taken for slowness: half the rate and 13 s of waiting
			 * for a tune that was decoded 1.8 times as fast as it plays.) */
			if (workers && tune.kind == K_MP3 && half_wanted < 0 && !tune.half && underruns >= 2) {
				go_half("it ran dry twice: decoding it at half the rate");
			} else {
				say("it ran dry: waiting for more to be decoded");
			}
		}
		return;
	}
	for (p = play_piece; (ready = slot_of(p)) != NULL; p++)
		ahead += ready->frames - (p == play_piece ? play_at : 0);
	s = speed();
	if (!decided && !workers && work_ms >= 2000 && p < count) {
		decided = 1;
		/* too slow: an MP3 file at half the rate first, and if that is too slow too, or for
		 * the other kinds, as much decoded before it plays as it takes to play without a gap */
		if (s < 1050 && tune.kind == K_MP3 && half_wanted < 0 && !tune.half) {
			decided = 0;
			go_half("cannot decode this as fast as it plays: decoding it at half the rate");
			return;
		}
		if (s < 1050)
			say("cannot decode this as fast as it plays: decoding ahead before it plays");
	}
	rate = (uint32_t)tune.rate;
	left = tune.total == ~0u ? 0 : tune.total - position();
	/* at that speed the rest is decoded while this much plays */
	need = s >= 1100 ? rate / 2 : left - (uint32_t)((uint64_t)left * s / 1100) + rate;
	if (workers)
		need = rate * 3 / 2;	/* (a second and a half in hand, whatever their start looked like) */
	if (p >= count || stuck || (s && ahead >= need)) {
		state = PLAYING;
		voice_run(!paused);
		if (paused)
			return;
		printf("nxplay: plays from %u ms of the tune, %u ms after it was opened, with %u ms decoded ahead\n",
		       (uint32_t)((uint64_t)position() * 1000 / (uint32_t)tune.rate), now_ms() - began_ms,
		       (uint32_t)((uint64_t)ahead * 1000 / rate));
		fflush(stdout);
	}
}

/* Everything that goes on by itself; true while this core has decoding to do. */
static int
pump(void)
{
	uint32_t count = pieces(), now = now_ms(), s;
	struct slot *slot;
	int16_t *place;
	int k, most, far, held, working = 0, active = state == BUFFERING || state == PLAYING;

	if (was_working)
		work_ms += now - last_ms;
	last_ms = now;
	for (k = 1; k <= workers; k++) {
		if (!(slot = job[k]) || !mcw_done(k))
			continue;
		worker_cycles += mcw_result(k);
		job[k] = NULL;
		if (slot->state == SLOT_DROPPED) {
			slot->state = SLOT_FREE;
			continue;
		}
		slot->frames = piece_frames(slot->piece);
		slot->state = SLOT_READY;
		decoded += piece_frames(slot->piece);
		all_decoded += piece_frames(slot->piece);
	}
	/* as far ahead as memory lets it when decoding is slow, a few pieces when it is not */
	s = speed();
	most = s && s < 1100 && !workers ? slot_most : workers + 4;
	held = active && dropping();
	for (k = 1; k <= workers && active; k++) {
		if (job[k]) {
			working = 1;
		} else if (!held && next_piece < count && ((place = piece_place(next_piece, &far)) != NULL || !far) &&
			   (slot = slot_get(most, 0)) != NULL) {
			/* (with workers a piece the ring has no room for yet waits: there are seconds in hand) */
			slot->piece = next_piece++;
			slot->state = SLOT_WORKER;
			slot->head[0] = slot->head[4] = 0;
			slot->head[5] = (uint32_t)place;
			slot->head[7] = (uint32_t)k;
			slot->direct = place != NULL;
			slot->pcm = place ? place : slot->own;
			job[k] = slot;
			mcw_post(k, piece_job, slot->piece, (uint32_t)slot->head);
			working = 1;
		}
	}
	/* with workers enough the first core decodes nothing: it gives pieces out sooner for it */
	if (!mine && !held && active && next_piece < count && (workers < own_under || !workers) &&
	    ((place = piece_place(next_piece, &far)) != NULL || !far || !workers) &&
	    (mine = slot_get(most, 0)) != NULL) {
		mine->piece = next_piece++;
		mine->state = SLOT_MINE;
		mine->done = 0;
		mine->direct = place != NULL;
		mine->pcm = place ? place : mine->own;
	}
	if (mine)
		own_step();
	feed();
	decide(active && !working && !mine && !held && next_piece < count && !slot_get(most, 1));
	was_working = working || mine != NULL;
	return mine != NULL;
}

/* ---- a file ---- */

static void
unload(void)
{
	int i;

	settle();
	if (out.on && tune.kind)
		snd_stop(out.voice);
	stats();
	for (i = 0; i < slot_count; i++)
		munmap(slots[i].head, slot_bytes);
	slot_count = 0;
	if (tune.pieced && tune.kind == K_FLAC) {	/* (a FLAC file's workers are fewer than another kind's) */
		mcw_close();
		workers = 0;
	}
	if (tune.audio)
		media_audio_close(tune.audio);
	free(tune.file);
	memset(&tune, 0, sizeof tune);
	state = STOPPED;
}

/* Up to `most` workers, or those it has when they are no more than that. */
static void
workers_open(int most)
{
	extern char __DATA_BEGIN__[], _end[];

	if (workers > most) {
		mcw_close();
		workers = 0;
	}
	if (workers || workers_wanted <= 0)
		return;
	workers = mcw_open(workers_wanted < most ? workers_wanted : most);
	/* what starts as zeros is not there for a worker until this core has touched it */
	if (workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	if (workers && out.on)
		mcw_touch(out.ring, RING * 4);
}

/* Why not, or NULL with the tune ready to play from its start. */
static const char *
load(const char *path)
{
	FILE *file = fopen(path, "rb");
	struct sysinfo memory;
	struct media_audio_info info;
	const char *why;
	uint64_t room;
	long size;

	if (!file)
		return "cannot open the file";
	unload();
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	rewind(file);
	if (size < 64 || !(tune.file = malloc((size_t)size)) || fread(tune.file, 1, (size_t)size, file) != (size_t)size) {
		fclose(file);
		free(tune.file);
		tune.file = NULL;
		return "cannot read the file";
	}
	fclose(file);
	tune.size = (uint32_t)size;
	snprintf(tune.path, sizeof tune.path, "%s", path);
	tune.name = strrchr(tune.path, '/') ? strrchr(tune.path, '/') + 1 : tune.path;
	if (!(tune.audio = media_audio_open(tune.file, tune.size, &why))) {
		free(tune.file);
		tune.file = NULL;
		return why;
	}
	media_audio_get_info(tune.audio, &info);
	tune.kind = info.kind == MEDIA_WAV ? K_WAV : info.kind == MEDIA_MOD ? K_MOD : info.kind == MEDIA_FLAC ? K_FLAC :
		    info.kind == MEDIA_MP3 ? K_MP3 : K_OGG;
	if (tune.kind == K_MOD)
		media_audio_mix_rate(tune.audio, mod_rate);
	if (tune.kind == K_MP3 && half_wanted > 0)
		media_audio_half(tune.audio, 1);
	tune_info();
	/* An MP3, Vorbis or FLAC file's pieces go to the workers (a FLAC file's to fewer: a block is
	 * a tenth of a second); WAV and MOD are one core's work, and so is any kind without workers. */
	if (info.pieces && workers_wanted > 0 && (tune.kind == K_MP3 || tune.kind == K_OGG || tune.kind == K_FLAC))
		workers_open(tune.kind == K_FLAC ? FLAC_WORKERS : MC_MAX_CORES);
	tune.pieced = info.pieces && workers > 0 && tune.kind != K_WAV && tune.kind != K_MOD &&
		      media_audio_decoders(tune.audio, workers + 1) == workers + 1;
	if (tune.pieced) {
		uint32_t unit = (media_audio_set_piece(tune.audio, 1), media_audio_piece_frames(tune.audio, 0));

		/* MP3: half a second; with many workers (63 make 8 s of sound in 2.5 s) four frames, heard
		 * a piece sooner. FLAC: blocks of a sixteenth of a second (half a second with few
		 * workers). Vorbis: a second, half with many (a piece begins with a seek). */
		if (tune.kind == K_MP3)
			media_audio_set_piece(tune.audio, piece_frames_asked > 0 ? (uint32_t)piece_frames_asked * unit :
					      workers >= 24 ? 4 * unit : (uint32_t)tune.rate / 2);
		else if (tune.kind == K_FLAC)
			media_audio_set_piece(tune.audio, (uint32_t)tune.rate / (workers >= 12 ? 16 : 2));
		else
			media_audio_set_piece(tune.audio, (uint32_t)tune.rate / (workers >= 24 ? 2 : 1));
		tune_info();
	} else {
		/* the workers are someone else's meanwhile */
		mcw_close();
		workers = 0;
		tune.piece = (uint32_t)tune.rate / 2 > 4096 ? (uint32_t)tune.rate / 2 : 4096;
	}
	/* a frame more than a piece: a frame that is not what the table says is caught after it is written */
	slot_bytes = (32 + (tune.piece + 1152) * (uint32_t)tune.channels * 2 + 4095) & ~4095u;
	sysinfo(&memory);
	room = memory_mb ? (uint64_t)memory_mb << 20 : (uint64_t)memory.freeram * memory.mem_unit / 2;
	slot_most = room / slot_bytes > SLOTS ? SLOTS : (int)(room / slot_bytes);
	if (slot_most < workers + 4)
		slot_most = workers + 4;
	decided = underruns = 0;
	sum = 2166136261u;
	sum_count = 0;
	began_ms = now_ms();
	began_cycles = cycles();
	note[0] = 0;
	paused = 0;
	seek_to(0);
	return NULL;
}

/* ---- the window ---- */

static void
draw_status(void)
{
	static const char *kinds[] = { "", "WAV", "MOD", "FLAC", "MP3", "Ogg Vorbis" };
	uint32_t at = drag == 1 && tune.total != ~0u ? (uint32_t)((uint64_t)tune.total * (uint32_t)drag_value / 1000) : position();
	uint32_t seconds = tune.kind ? at / (uint32_t)tune.rate : 0, all = tune.kind && tune.total != ~0u ? tune.total / (uint32_t)tune.rate : 0;
	char line[160], more[40] = "";
	int n;

	if (workers)
		snprintf(more, sizeof more, ", %d workers", workers);
	n = snprintf(line, sizeof line, "%u:%02u / %u:%02u", seconds / 60, seconds % 60, all / 60, all % 60);
	if (tune.kind)
		snprintf(line + n, sizeof line - (size_t)n, "   %s %d Hz %s%s%s%s", kinds[tune.kind], tune.rate, tune.channels == 2 ? "stereo" : "mono",
			 tune.half ? ", half rate" : "", more, paused ? " (paused)" : state == BUFFERING ? " (decoding)" : "");
	ui_fill(window, 0, 24, W, 44, UI_FACE);
	ui_text_fit(window, 8, 26, W - 16, line, BLACK);
	ui_slider(window, 8, 46, W - 16, all && tune.total ? (int)((uint64_t)at * 1000 / tune.total) : 0, 1000);
	shown_second = (int)seconds;
	shown_state = state * 2 + paused;
}

static const char *
list_label(int row)
{
	return tunes[row];
}

static void
draw_volume(void)
{
	ui_tool(window, keys[B_MUTE].x, KEYS_Y, keys[B_MUTE].w, KEY_H, muted ? UI_G_MUTE : UI_G_SOUND, muted);
	ui_slider(window, VOLUME_X, KEYS_Y + 3, VOLUME_W, out.volume, 256);
}

static void
draw(void)
{
	static const int signs[] = { UI_G_PREV, UI_G_PLAY, UI_G_STOP, UI_G_NEXT };
	int playing = tune.kind && !paused && state != ENDED && state != STOPPED, i;

	ui_fill(window, 0, 0, W, H, UI_FACE);
	ui_text_fit(window, 8, 8, W - 16, tune.kind ? tune.name : "no tune: Open... has them, or the list below", BLACK);
	if (tune.kind)
		ui_text_fit(window, 9, 8, W - 16, tune.name, BLACK);	/* twice: bold */
	draw_status();
	for (i = B_PREV; i <= B_NEXT; i++)
		ui_tool(window, keys[i].x, KEYS_Y, keys[i].w, KEY_H, i == B_PLAY && playing ? UI_G_PAUSE : signs[i],
			(i == B_PREV || i == B_NEXT) && list.count < 2 ? 2 : 0);
	ui_button(window, keys[B_REPEAT].x, KEYS_Y, keys[B_REPEAT].w, KEY_H, "Repeat", repeat);
	ui_button(window, keys[B_SHUFFLE].x, KEYS_Y, keys[B_SHUFFLE].w, KEY_H, "Shuffle", shuffle);
	ui_button(window, keys[B_OPEN].x, KEYS_Y, keys[B_OPEN].w, KEY_H, "Open...", 0);
	draw_volume();
	ui_text_fit(window, 8, 102, W - 16, note, BLACK);
	ui_list_draw(window, &list, list_label);
}

/* How loud: 0 to 256. Muted, the card plays nothing and the slider keeps its place. */
static void
set_volume(int volume)
{
	out.volume = volume < 0 ? 0 : volume > 256 ? 256 : volume;
	if (out.on)
		SND_VOICE[out.voice].volume = muted ? 0 : SND_VOLUME(out.volume, out.volume);
}

static int
by_name(const void *a, const void *b)
{
	return strcasecmp(a, b);
}

/* The list is the tunes of a folder; the one being played is its selected row. */
static void
read_list(const char *folder)
{
	DIR *names;
	struct dirent *item;
	int i;

	if (strcmp(folder, list_folder)) {
		snprintf(list_folder, sizeof list_folder, "%s", folder);
		list.count = list.top = 0;
		names = opendir(folder);
		while (names && list.count < TUNES && (item = readdir(names)) != NULL)
			if (item->d_name[0] != '.' && strlen(item->d_name) < sizeof tunes[0] && ui_files_wanted(item->d_name, KINDS))
				strcpy(tunes[list.count++], item->d_name);
		if (names)
			closedir(names);
		qsort(tunes, (size_t)list.count, sizeof tunes[0], by_name);
	}
	list.selected = -1;
	for (i = 0; i < list.count && tune.kind; i++)
		if (!strcmp(tunes[i], tune.name))
			list.selected = i;
	ui_list_show(&list);
}

/* Plays a file from its start; the list becomes its folder's. False, and why, when it cannot. */
static int
open_tune(const char *path)
{
	char folder[512], title[140];
	const char *why = load(path), *slash;

	if (why) {
		say(why);
		return 0;
	}
	slash = strrchr(tune.path, '/');
	snprintf(folder, sizeof folder, "%.*s", slash > tune.path ? (int)(slash - tune.path) : 1, slash ? tune.path : ".");
	read_list(folder);
	set_volume(out.volume);		/* (the voice is a new one's) */
	if (window_on) {
		snprintf(title, sizeof title, "%.120s - Music", tune.name);
		GrSetWindowTitle(window, title);
	}
	return 1;
}

/* Plays row `row` of the list. */
static void
play_row(int row)
{
	char path[700];

	if (row < 0 || row >= list.count)
		return;
	snprintf(path, sizeof path, "%s/%s", list_folder[1] ? list_folder : "", tunes[row]);
	open_tune(path);
}

/* The tune `by` rows on in the list (any other one when shuffling), round its ends. One that
 * ended by itself (`ended`) is followed by the list's first only when repeating. */
static void
step(int by, int ended)
{
	int row = list.selected + by;

	if (list.count < 1)
		return;
	if (shuffle && list.count > 1)
		row = (list.selected + 1 + rand() % (list.count - 1)) % list.count;
	else if (row >= list.count && ended && !repeat)
		return;
	play_row((row % list.count + list.count) % list.count);
}

static void choose(void);

static void
toggle(void)
{
	if (!tune.kind) {
		/* nothing to play yet: the list's selected row, or a file to be chosen */
		if (list.selected >= 0)
			play_row(list.selected);
		else
			choose();
		return;
	}
	if (state == ENDED || state == STOPPED) {
		paused = 0;
		seek_to(0);
	} else {
		paused = !paused;
		if (state == PLAYING)
			voice_run(!paused);
	}
}

static void
other_windows(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->general.wid == window)
		draw();
	/* the player's own box while the file window is up: both go */
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		ui_files_stop = quit = 1;
}

/* The file chooser has the events until it is done: the tune waits meanwhile. */
static void
choose(void)
{
	char path[600], folder[512] = "/usr/share";
	int held = paused;

	if (access("/root/Music", R_OK) == 0)	/* the image's own tunes (tools/make_media.py) */
		strcpy(folder, "/root/Music");
	if (tune.kind && strrchr(tune.path, '/') > tune.path)
		snprintf(folder, sizeof folder, "%.*s", (int)(strrchr(tune.path, '/') - tune.path), tune.path);
	paused = 1;
	voice_run(0);
	if (ui_choose_file("Open a sound file", folder, KINDS, path, sizeof path, other_windows)) {
		open_tune(path);
	} else {
		paused = held;
		if (state == PLAYING)
			voice_run(!paused);
	}
	last_ms = now_ms();
}

static void
handle(GR_EVENT *event)
{
	int x, y, how, i, key = -1;

	switch (event->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		draw();
		return;
	case GR_EVENT_TYPE_CLOSE_REQ:
		quit = 1;
		return;
	case GR_EVENT_TYPE_KEY_DOWN:
		/* the arrows and the paging keys are the list's; Enter plays its selected row */
		if (ui_list_key(&list, event->keystroke.ch))
			break;
		switch (event->keystroke.ch < 128 ? tolower((int)event->keystroke.ch) : (int)event->keystroke.ch) {
		case ' ': toggle(); break;
		case MWKEY_ENTER: play_row(list.selected); break;
		case 'o': choose(); break;
		case 'q': case MWKEY_ESCAPE: quit = 1; break;
		case '+': case '=': set_volume(out.volume + 16); break;
		case '-': set_volume(out.volume - 16); break;
		case 'm': muted = !muted, set_volume(out.volume), say(muted ? "muted" : "the sound is on again"); break;
		case 'n': step(1, 0); break;
		case 'p': step(-1, 0); break;
		case 'r': repeat = !repeat, say(repeat ? "repeat: the list starts again after its last" : "repeat is off"); break;
		case 's': shuffle = !shuffle, say(shuffle ? "shuffle: any tune of the list next" : "shuffle is off"); break;
		case MWKEY_LEFT:
			if (tune.kind)
				seek_to(position() > 10u * (uint32_t)tune.rate ? position() - 10u * (uint32_t)tune.rate : 0);
			break;
		case MWKEY_RIGHT:
			if (tune.kind)
				seek_to(position() + 10u * (uint32_t)tune.rate);
			break;
		default: return;
		}
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		x = event->button.x;
		y = event->button.y;
		if (ui_wheel(event) && ui_inside(x, y, list.x, list.y, list.width, list.height)) {
			/* (over the list the wheel moves the list, if it is longer than its room; elsewhere it is the volume) */
			ui_list_event(&list, event);
		} else if (ui_wheel(event)) {
			set_volume(out.volume - 16 * ui_wheel(event));
			printf("nxplay: volume %d of 256\n", out.volume);
			fflush(stdout);
		} else if (!(event->button.changebuttons & GR_BUTTON_L)) {
			/* (the left button presses: another does nothing) */
		} else if (ui_inside(x, y, 8, 46, W - 16, 18) && tune.kind && tune.total != ~0u) {
			drag = 1;
			drag_value = ui_slider_value(x, 8, W - 16, 1000);
		} else if (ui_inside(x, y, VOLUME_X, KEYS_Y, VOLUME_W, KEY_H)) {
			drag = 2;
			set_volume(ui_slider_value(x, VOLUME_X, VOLUME_W, 256));
			printf("nxplay: volume %d of 256\n", out.volume);
			fflush(stdout);
		} else if ((how = ui_list_event(&list, event)) != 0) {
			if (how == 2)
				play_row(list.selected);	/* the selected row clicked again */
		} else {
			for (i = 0; i < BUTTONS; i++)
				if (ui_inside(x, y, keys[i].x, KEYS_Y, keys[i].w, KEY_H))
					key = i;
			switch (key) {
			case B_PREV: step(-1, 0); break;
			case B_PLAY: toggle(); break;
			case B_STOP:
				if (tune.kind) {
					paused = 1;
					seek_to(0);
				}
				break;
			case B_NEXT: step(1, 0); break;
			case B_REPEAT: repeat = !repeat, say(repeat ? "repeat: the list starts again after its last" : "repeat is off"); break;
			case B_SHUFFLE: shuffle = !shuffle, say(shuffle ? "shuffle: any tune of the list next" : "shuffle is off"); break;
			case B_OPEN: choose(); break;
			case B_MUTE: muted = !muted, set_volume(out.volume), say(muted ? "muted" : "the sound is on again"); break;
			default: return;
			}
		}
		break;
	case GR_EVENT_TYPE_MOUSE_MOTION:
		if (!drag && (event->mouse.buttons & GR_BUTTON_L) && ui_list_event(&list, event))
			break;		/* (the list's scroll bar, held) */
		if (!drag || !(event->mouse.buttons & GR_BUTTON_L))
			return;
		if (drag == 1) {
			drag_value = ui_slider_value(event->mouse.x, 8, W - 16, 1000);
			draw_status();
		} else {
			set_volume(ui_slider_value(event->mouse.x, VOLUME_X, VOLUME_W, 256));
			draw_volume();
		}
		return;
	case GR_EVENT_TYPE_BUTTON_UP:
		if (!drag)
			return;
		if (drag == 1)
			seek_to((uint32_t)((uint64_t)tune.total * (uint32_t)drag_value / 1000));
		drag = 0;
		break;
	default:
		return;
	}
	draw();
}

static void
events(int busy)
{
	GR_EVENT event;

	if (busy)
		GrCheckNextEvent(&event);
	else
		GrGetNextEventTimeout(&event, state == PLAYING || state == BUFFERING ? 30 : 200);
	while (!quit && event.type != GR_EVENT_TYPE_NONE && event.type != GR_EVENT_TYPE_TIMEOUT) {
		handle(&event);
		GrCheckNextEvent(&event);
	}
	if (quit)
		return;
	if (shown_state != state * 2 + paused)
		draw();
	else if (tune.kind && (int)(position() / (uint32_t)tune.rate) != shown_second && !drag)
		draw_status();
}

static void
leave(int sig)
{
	quit = 1;
}

static int
number(const char *name, int otherwise)
{
	const char *value = getenv(name);

	return value && *value ? atoi(value) : otherwise;
}

int
main(int argc, char **argv)
{
	const char *why = NULL;
	int busy;

	exit_at_end = number("NXPLAY_EXIT", 0);
	limit_seconds = (uint32_t)number("NXPLAY_SECONDS", 0);
	/* 48: a tune is some ten cores' work, and every worker is time to start (with all 255 it
	 * played 5.4 s after it was opened, with 48 after 2.9 s) */
	workers_wanted = number("NXPLAY_WORKERS", 48);
	want_sum = number("NXPLAY_SUM", 0);
	half_wanted = number("NXPLAY_HALF", -1);
	memory_mb = (uint32_t)number("NXPLAY_MEMORY", 0);
	mod_rate = number("NXPLAY_MODRATE", mod_rate);
	piece_frames_asked = number("NXPLAY_PIECE", 0);
	if (number("NXPLAY_OWN", 0))		/* the first core takes pieces whatever the workers */
		own_under = MC_MAX_CORES;
	out.volume = number("NXPLAY_VOLUME", 200);
	signal(SIGINT, leave);
	signal(SIGTERM, leave);
	atexit(mcw_close);
	window_on = !number("NXPLAY_NOWINDOW", 0) && access("/tmp/.nano-X", F_OK) == 0 && GrOpen() >= 0;
	out.on = snd_open() == 0 && (out.voice = snd_claim()) >= 0;
	if (out.on && (out.ring = snd_memory(RING_LONG * 4, &out.ring_at)) != NULL)
		ring_frames = RING_LONG;
	else
		out.on = out.on && (out.ring = snd_memory(RING_SHORT * 4, &out.ring_at)) != NULL;
	if (!out.on)
		say("no sound card here: decoding only");
	if (window_on) {
		/* the window first: opening a file starts the workers, which is seconds, and a desktop
		 * with nothing new on it looks stuck */
		ui_init();
		window = GrNewWindowEx(UI_FIXED, "Music", GR_ROOT_WINDOW_ID, -1, -1, W, H, UI_FACE);
		GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP |
				       GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
		GrMapWindow(window);
		if (argc > 1) {
			ui_framed();
			snprintf(note, sizeof note, "opening it...");
			draw();
			GrFlush();
		}
	}
	if (argc > 1 && !open_tune(argv[1]))
		why = note;
	if (argc < 2 && window_on)	/* nothing asked for: the image's own tunes to choose from */
		read_list(access("/root/Music", R_OK) == 0 ? "/root/Music" : "/usr/share");
	if (!window_on && (argc < 2 || why)) {
		if (argc < 2)
			fprintf(stderr, "nxplay FILE: plays a WAV, MOD, FLAC, MP3 or Ogg Vorbis file (docs/play.md)\n");
		return 1;
	}
	while (!quit) {
		if (want_next) {
			want_next = 0;
			step(1, 1);
		}
		busy = pump();
		if (window_on)
			events(busy);
		else if (!busy && !quit && out.on)
			usleep(5000);
		else if (!busy && !quit)
			mcw_rest();
	}
	if (out.on && tune.kind)
		snd_stop(out.voice);
	stats();
	if (window_on)
		GrClose();
	return 0;
}
