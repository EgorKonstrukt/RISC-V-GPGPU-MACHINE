/* ClassiCube's sound on the machine's card (docs/games/classicube.md, "Sound"): a dig or step sound
 * is a voice playing ADPCM where it lies in the ROM; the tune is an MP3 that worker cores
 * decode at half its rate, piece by piece, into a stream voice's ring in sound memory. */
#include "Core.h"
#if CC_AUD_BACKEND == CC_AUD_BACKEND_SHADEREMU
#include "Audio.h"
#include "Errors.h"
#include "Options.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../userland/shaderemu_sound.h"
#include "media.h"
#include "mcwx.h"

struct AudioContext { int count, voice; };	/* voice + 1; 0 for none */
#define AUDIO_OVERRIDE_SOUNDS
#define AUDIO_OVERRIDE_ALLOC
#include "_AudioBase.h"

#define SE_PACK "/usr/share/classicube-sound.pak"
#define SE_TUNE "/usr/share/classicube-music.mp3"
#define SE_RING 262144		/* the stream's frames: 11.9 s at 22,050 Hz */
#define SE_PIECE 44100		/* frames a worker decodes at a time */
#define SE_JOBS 6

struct se_sound { char name[24]; cc_uint32 rate, samples, data, marks; };
static struct se_sound* sounds;
static cc_uint32 pack_at;
static int card;		/* 1: there is one, -1: none */
static int sound_log = -1;

static int Log(void) {
	if (sound_log < 0) sound_log = getenv("CLASSICUBE_SOUND_LOG") != NULL;
	return sound_log;
}

static cc_uint32 Now_Ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (cc_uint32)t.tv_sec * 1000 + (cc_uint32)(t.tv_nsec / 1000000);
}

cc_bool AudioBackend_Init(void) {
	if (!card) card = snd_open() == 0 ? 1 : -1;
	return card > 0;
}
void AudioBackend_Free(void) { }

cc_result Audio_Init(struct AudioContext* ctx, int buffers) {
	int v;
	ctx->count = buffers;
	if (!ctx->voice && card > 0 && (v = snd_claim()) >= 0) ctx->voice = v + 1;
	return 0;
}

/* (the voice stays this program's: Audio_Init takes it again) */
void Audio_Close(struct AudioContext* ctx) {
	if (ctx->count && ctx->voice) snd_stop(ctx->voice - 1);
	ctx->count = 0;
}

void Audio_SetVolume(struct AudioContext* ctx, int volume) { }

cc_result StreamContext_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) { return ERR_NOT_SUPPORTED; }
cc_result StreamContext_Enqueue(struct AudioContext* ctx, struct AudioChunk* chunk) { return ERR_NOT_SUPPORTED; }
cc_result StreamContext_Play(struct AudioContext* ctx) { return ERR_NOT_SUPPORTED; }
cc_result StreamContext_Pause(struct AudioContext* ctx) { return ERR_NOT_SUPPORTED; }
cc_result StreamContext_Update(struct AudioContext* ctx, int* inUse) { return ERR_NOT_SUPPORTED; }

cc_bool SoundContext_FastPlay(struct AudioContext* ctx, struct AudioData* data) { return true; }

/* A sound's chunk is its entry in the pack (AudioBackend_LoadSounds). */
cc_result SoundContext_PlayData(struct AudioContext* ctx, struct AudioData* data) {
	const struct se_sound* s = (const struct se_sound*)data->chunk.data;
	cc_uint32 loud = (cc_uint32)data->volume * 256 / 100;

	if (!ctx->voice || !s) return 0;
	snd_play_aux(ctx->voice - 1, SND_ADPCM | SND_SMOOTH, pack_at + s->data, s->samples, 0,
		snd_step(s->rate * (cc_uint32)data->rate / 100), SND_VOLUME(loud, loud), pack_at + s->marks);
	if (Log()) printf("ccsound: %s on voice %d, volume %d, rate %d\n", s->name, ctx->voice - 1, data->volume, data->rate);
	return 0;
}

cc_result SoundContext_PollBusy(struct AudioContext* ctx, cc_bool* isBusy) {
	*isBusy = ctx->voice && snd_playing(ctx->voice - 1);
	return 0;
}

cc_bool Audio_DescribeError(cc_result res, cc_string* dst) { return false; }
cc_result Audio_AllocChunks(cc_uint32 size, struct AudioChunk* chunks, int numChunks) { return ERR_NOT_SUPPORTED; }
void Audio_FreeChunks(struct AudioChunk* chunks, int numChunks) { }

/* dig_grass1 into digBoard's grass group */
static void Board_Add(struct se_sound* s) {
	struct Soundboard* board = !strncmp(s->name, "dig_", 4) ? &digBoard : !strncmp(s->name, "step_", 5) ? &stepBoard : NULL;
	const char* kind = s->name + (board == &digBoard ? 4 : 5);
	int length = (int)strnlen(kind, 18) - 1, t;
	struct SoundGroup* group;
	struct Sound* snd;

	if (!board || length < 1) return;
	for (t = 0; t < SOUND_COUNT; t++)
		if ((int)strlen(Sound_Names[t]) == length && !strncmp(kind, Sound_Names[t], length)) break;
	if (t == SOUND_COUNT || board->groups[t].count == AUDIO_MAX_SOUNDS) return;
	group = &board->groups[t];
	snd = &group->sounds[group->count++];
	snd->channels = 1, snd->sampleRate = (int)s->rate;
	snd->chunk.data = s, snd->chunk.size = s->samples;
}

void AudioBackend_LoadSounds(void) {
	cc_uint32 head[2], i;
	int fd = open(SE_PACK, O_RDONLY);

	if (fd < 0 || read(fd, head, 8) != 8 || memcmp(head, "CCSN", 4) || !(pack_at = snd_rom(fd, "CCSN", 4)) ||
	    !(sounds = (struct se_sound*)malloc(head[1] * sizeof *sounds)) ||
	    read(fd, sounds, head[1] * sizeof *sounds) != (ssize_t)(head[1] * sizeof *sounds)) {
		printf("ccsound: no sounds (%s)\n", SE_PACK);
		if (fd >= 0) close(fd);
		return;
	}
	close(fd);
	for (i = 0; i < head[1]; i++) Board_Add(&sounds[i]);
	if (Log()) printf("ccsound: %u sounds at %08x\n", (unsigned)head[1], (unsigned)pack_at);
}

/* ---- the tune ---- */
static struct {
	int volume, voice, open, stopping;
	struct media_audio* a;
	struct media_audio_info info;
	const void* file; size_t size;
	cc_int16* ring; cc_uint32 ring_at;
	cc_int16* stage[SE_JOBS];
	mcw_handle job[SE_JOBS];
	cc_uint32 job_piece[SE_JOBS];
	cc_uint8* done;
	cc_uint32 next, ordered, written, again_at;
	cc_uint32 dry_since, dry_ms, dry_times;	/* the stream run dry while the tune is not all made */
} music;

static cc_uint32 Music_Job(cc_uint32 slot, cc_uint32 piece) {
	cc_int16* to = music.stage[slot];
	cc_uint32 n = media_audio_piece_frames(music.a, piece), at = media_audio_piece_first(music.a, piece) & (SE_RING - 1);
	cc_uint32 first = SE_RING - at < n ? SE_RING - at : n;

	media_audio_piece(music.a, mcw_self(), piece, to);
	memcpy(music.ring + 2 * at, to, first * 4);
	memcpy(music.ring, to + 2 * first, (n - first) * 4);
	return n;
}

static void Music_Close(void) {
	int j;
	for (j = 0; j < SE_JOBS; j++)
		if (music.job[j]) mcw_join(music.job[j]), music.job[j] = 0;
	if (music.voice) snd_stop(music.voice - 1);
	if (music.a) media_audio_close(music.a), music.a = NULL;
	if (music.file) munmap((void*)music.file, music.size), music.file = NULL;
	free(music.done), music.done = NULL;
	music.open = 0;
}

static int Music_Open(void) {
	struct stat st;
	const char* why = "";
	int fd = open(SE_TUNE, O_RDONLY), j, v;

	if (!music.voice && (v = snd_claim()) >= 0) music.voice = v + 1;
	if (!music.ring) {
		music.ring = (cc_int16*)snd_memory(SE_RING * 4, &music.ring_at);
		for (j = 0; j < SE_JOBS && music.ring; j++)
			music.stage[j] = (cc_int16*)malloc((SE_PIECE + 4096) * 4);
		if (music.ring) mcw_touch(music.ring, SE_RING * 4);
	}
	if (fd < 0 || fstat(fd, &st) || !music.voice || !music.ring || !mcw_count() ||
	    (music.file = mmap(NULL, music.size = (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		printf("ccmusic: no tune (%s%s)\n", SE_TUNE, mcw_count() ? "" : ", no workers");
		if (fd >= 0) close(fd);
		music.file = NULL;
		return 0;
	}
	close(fd);
	if (!(music.a = media_audio_open(music.file, (cc_uint32)music.size, &why))) {
		printf("ccmusic: %s: %s\n", SE_TUNE, why);
		Music_Close();
		return 0;
	}
	media_audio_half(music.a, 1);
	media_audio_set_piece(music.a, SE_PIECE);
	media_audio_decoders(music.a, mcw_count() + 1);
	media_audio_get_info(music.a, &music.info);
	for (j = 0; j < SE_JOBS; j++) mcw_touch(music.stage[j], (SE_PIECE + 4096) * 4);
	music.done = (cc_uint8*)calloc(music.info.pieces + 1, 1);
	music.next = music.ordered = music.written = music.dry_since = music.dry_ms = music.dry_times = 0;
	music.open = 1;
	snd_play(music.voice - 1, SND_STREAM | SND_STEREO | SND_SMOOTH, music.ring_at, SE_RING, 0, snd_step((cc_uint32)music.info.rate), 0);
	if (Log()) printf("ccmusic: %s, %d Hz %d ch, %u frames in %u pieces\n", SE_TUNE, music.info.rate, music.info.channels,
		(unsigned)music.info.frames, (unsigned)music.info.pieces);
	return 1;
}

static cc_uint32 Music_Played(void) {
	int v = music.voice - 1;
	return SND_STATE[v].seen != SND_VOICE[v].key ? 0 : SND_STATE[v].position;
}

/* Every frame: pieces given out ahead of what is heard, taken in order into the stream. */
static void Music_Tick(void) {
	cc_uint32 played, loud = (cc_uint32)music.volume * 256 / 100;
	int j;

	if (!music.open) {
		if (music.volume && !music.stopping && (cc_int32)(Now_Ms() - music.again_at) >= 0 && card > 0 && !Music_Open())
			music.again_at = Now_Ms() + 600000;
		return;
	}
	if (music.stopping) {
		SND_VOICE[music.voice - 1].volume = 0;
		for (j = 0; j < SE_JOBS; j++)
			if (music.job[j] && !mcw_ready(music.job[j])) return;
		Music_Close();
		return;
	}
	SND_VOICE[music.voice - 1].volume = SND_VOLUME(loud, loud);
	played = Music_Played();
	for (j = 0; j < SE_JOBS; j++)
		if (music.job[j] && mcw_ready(music.job[j])) {
			mcw_join(music.job[j]);
			music.job[j] = 0;
			music.done[music.job_piece[j]] = 1;
		}
	while (music.ordered < music.info.pieces && music.done[music.ordered])
		music.written += media_audio_piece_frames(music.a, music.ordered++);
	SND_VOICE[music.voice - 1].loop = music.written;
	if (music.ordered < music.info.pieces && played >= music.written && music.written) {
		if (!music.dry_since) music.dry_since = Now_Ms() | 1, music.dry_times++;
	} else if (music.dry_since) {
		music.dry_ms += Now_Ms() - music.dry_since, music.dry_since = 0;
	}
	for (j = 0; j < SE_JOBS && music.next < music.info.pieces; j++) {
		cc_uint32 end = media_audio_piece_first(music.a, music.next) + media_audio_piece_frames(music.a, music.next);
		if (music.job[j] || end - played > SE_RING) continue;
		if (!(music.job[j] = mcw_try_call(Music_Job, (cc_uint32)j, music.next))) break;
		music.job_piece[j] = music.next++;
	}
	if (music.ordered == music.info.pieces && played >= music.written) {
		int least = Options_GetInt(OPT_MIN_MUSIC_DELAY, 0, 3600, 120), most = Options_GetInt(OPT_MAX_MUSIC_DELAY, 0, 3600, 420);
		if (Log()) printf("ccmusic: ended at %u frames\n", (unsigned)played);
		Music_Close();
		music.again_at = Now_Ms() + 1000u * (cc_uint32)(least + (most > least ? rand() % (most - least + 1) : 0));
	}
	if (Log() && music.ordered && music.ordered % 8 == 0 && music.done[music.ordered - 1] == 1) {
		music.done[music.ordered - 1] = 2;
		printf("ccmusic: %u of %u pieces made, %u frames written, %u heard, dry %u times for %u ms\n", (unsigned)music.ordered,
			(unsigned)music.info.pieces, (unsigned)music.written, (unsigned)played, (unsigned)music.dry_times, (unsigned)music.dry_ms);
	}
}

/* The game's music volume, 0 to 100 (Audio.c's Music_Start and Music_Stop). */
void SE_Music(int volume) {
	music.volume = volume;
	if (volume) {
		music.stopping = 0;
		AudioBackend_Init();
	} else if (music.open) {
		music.stopping = 1;
	}
}
void SE_Music_Free(void) { if (music.open) Music_Close(); }

void AudioBackend_Tick(void) {
	if (card > 0) Music_Tick();
}
#endif
