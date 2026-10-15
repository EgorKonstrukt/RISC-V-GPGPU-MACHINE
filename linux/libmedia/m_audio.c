/*
 * Sound (docs/media.md): every kind as 16-bit frames, in order on one core, or in pieces that
 * any core decodes by itself (an MP3 file's frames, a FLAC file's blocks, a Vorbis file from
 * any sample, a WAV file anywhere). media_audio_decode_all gives the pieces to every worker.
 */
#include <math.h>
#include <stdlib.h>
#include "m_audio.h"

void *
m_audio_pages(uint32_t bytes)
{
	uint32_t *m = m_map(bytes + 16);

	if (!m)
		return NULL;
	m[0] = bytes + 16;
	return m + 4;
}

void
m_audio_pages_free(void *memory)
{
	if (memory)
		m_unmap((uint32_t *)memory - 4, ((uint32_t *)memory)[-4]);
}

/* ---------------------------------------------------------------- WAV */

struct wav {
	const uint8_t *samples;
	uint32_t frames;
	int width, channels, is_float;	/* bytes a sample, the file's channels (the first two are kept) */
};

const char *
m_wav_open(struct media_audio *a)
{
	const uint8_t *f = a->data, *data = NULL;
	uint32_t at = 12, size, bytes = 0;
	int format = 0, bits = 0, channels = 0, rate = 0;
	struct wav *w;

	while (at + 8 <= a->size) {
		const uint8_t *c = f + at;

		size = m_le32(c + 4);
		if (size > a->size - at - 8)
			size = a->size - at - 8;	/* (a file written as it was recorded says 0 or ~0) */
		if (!memcmp(c, "fmt ", 4) && size >= 16) {
			format = (int)m_le16(c + 8);
			channels = (int)m_le16(c + 10);
			rate = (int)m_le32(c + 12);
			bits = (int)m_le16(c + 22);
			if (format == 0xfffe && size >= 26)
				format = (int)m_le16(c + 32);	/* what an extensible header says it is */
		} else if (!memcmp(c, "data", 4)) {
			data = c + 8;
			bytes = size;
			break;
		}
		at += 8 + ((size + 1) & ~1u);
	}
	if (!data || channels < 1 || rate <= 0 || !((format == 1 && bits >= 8 && bits <= 32) || (format == 3 && (bits == 32 || bits == 64))))
		return "a WAV file, but not of plain samples";
	if (!(w = calloc(1, sizeof *w)))
		return "not enough memory";
	w->samples = data;
	w->width = (bits + 7) / 8;
	w->channels = channels;
	w->is_float = format == 3;
	w->frames = bytes / (uint32_t)(w->width * channels);
	a->own = w;
	a->info.rate = rate;
	a->info.channels = channels > 2 ? 2 : channels;
	a->info.frames = w->frames;
	a->unit = 1;
	return NULL;
}

/* Rounded to the nearest, a half to even (as lrintf, which musl makes a call of): 1.5 * 2^23
 * added and taken away leaves no fraction. */
static inline int16_t
wav_float(float x)
{
	const float magic = 12582912.0f;
	float y = x * 32768.0f;

	y = y < -32768.0f ? -32768.0f : y > 32767.0f ? 32767.0f : y;
	return (int16_t)(int)(y + magic - magic);
}

/* Frames from..from+n-1 as 16 bits: the top two bytes of an integer, floats rounded (as ffmpeg does). */
static void
wav_convert(const struct wav *w, int channels, uint32_t from, uint32_t n, int16_t *to)
{
	const uint8_t *p = w->samples + from * (uint32_t)(w->width * w->channels);
	uint32_t step = (uint32_t)(w->width * w->channels), i;
	int c;

	if (w->width == 2 && w->channels == channels) {
		memcpy(to, p, (size_t)n * 2 * channels);
		return;
	}
	/* a loop for each kind: one with a test a sample was 36 instructions a sample */
	if (w->is_float && w->width == 4) {
		for (i = 0; i < n; i++, p += step)
			for (c = 0; c < channels; c++) {
				float x;

				memcpy(&x, p + c * 4, 4);
				*to++ = wav_float(x);
			}
	} else if (w->is_float) {
		for (i = 0; i < n; i++, p += step)
			for (c = 0; c < channels; c++) {
				double x;
				long v;

				memcpy(&x, p + c * 8, 8);
				v = lrint(x * 32768.0);
				*to++ = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
			}
	} else if (w->width == 1) {
		for (i = 0; i < n; i++, p += step)
			for (c = 0; c < channels; c++)
				*to++ = (int16_t)((p[c] - 128) << 8);	/* (bytes are unsigned) */
	} else {
		/* the top two bytes */
		const uint8_t *top = p + w->width - 2;

		for (i = 0; i < n; i++, top += step)
			for (c = 0; c < channels; c++)
				*to++ = (int16_t)(top[c * w->width] | top[c * w->width + 1] << 8);
	}
}

int
m_wav_read(struct media_audio *a, int16_t *to, int frames)
{
	struct wav *w = a->own;

	if ((uint32_t)frames > w->frames - a->at)
		frames = (int)(w->frames - a->at);
	wav_convert(w, a->info.channels, a->at, (uint32_t)frames, to);
	return frames;
}

uint32_t
m_wav_seek(struct media_audio *a, uint32_t frame)
{
	struct wav *w = a->own;

	return frame > w->frames ? w->frames : frame;
}

int
m_wav_decoder(struct media_audio *a, int d)
{
	a->dec[d] = a->own;	/* (nothing of its own: a copy) */
	return 1;
}

uint32_t
m_wav_step(struct media_audio *a, int d, uint32_t piece, int16_t *to, uint32_t done)
{
	uint32_t want = media_audio_piece_frames(a, piece), n = want - done > 8192 ? 8192 : want - done;

	(void)d;
	wav_convert(a->own, a->info.channels, piece * a->piece + done, n, to + done * (uint32_t)a->info.channels);
	return done + n;
}

void
m_wav_close(struct media_audio *a)
{
	free(a->own);
}

/* ---------------------------------------------------------------- every kind */

#define KIND(a, call, ...) \
	((a)->info.kind == MEDIA_WAV ? m_wav_##call(__VA_ARGS__) : (a)->info.kind == MEDIA_MP3 ? m_mp3_##call(__VA_ARGS__) : \
	 (a)->info.kind == MEDIA_FLAC ? m_flac_##call(__VA_ARGS__) : (a)->info.kind == MEDIA_OGG ? m_vorbis_##call(__VA_ARGS__) : \
	 m_mod_##call(__VA_ARGS__))

struct media_audio *
media_audio_open(const void *data, uint32_t size, const char **why)
{
	const uint8_t *f = data;
	struct media_audio *a = calloc(1, sizeof *a);
	const char *no = NULL;

	*why = NULL;
	if (!a) {
		*why = "not enough memory";
		return NULL;
	}
	a->data = f;
	a->size = size;
	if (size < 64) {
		no = "too short to be a sound";
	} else if (!memcmp(f, "RIFF", 4) && !memcmp(f + 8, "WAVE", 4)) {
		a->info.kind = MEDIA_WAV, no = m_wav_open(a);
	} else if (!memcmp(f, "fLaC", 4)) {
		a->info.kind = MEDIA_FLAC, no = m_flac_open(a);
	} else if (!memcmp(f, "OggS", 4)) {
		a->info.kind = MEDIA_OGG, no = m_vorbis_open(a);
	} else {
		/* a tracker tune has its tag far in; anything else may be MP3 frames */
		a->info.kind = MEDIA_MOD;
		if ((no = m_mod_open(a)) != NULL) {
			a->info.kind = MEDIA_MP3;
			no = m_mp3_open(a) ? "not a WAV, FLAC, MP3, Ogg Vorbis or MOD file" : NULL;
		}
	}
	if (!no && (a->info.rate < 1000 || a->info.rate > 192000 || a->info.channels < 1 || a->info.channels > 2)) {
		media_audio_close(a);
		*why = "a sample rate or channels this does not take";
		return NULL;
	}
	if (no) {
		*why = no;
		free(a->own);	/* (a kind that failed has nothing else) */
		free(a);
		return NULL;
	}
	/* pieces of about half a second (a Vorbis file's: a second, each begins with a seek) */
	media_audio_set_piece(a, a->info.kind == MEDIA_OGG ? (uint32_t)a->info.rate : a->info.kind == MEDIA_WAV ? 8192u :
			      (uint32_t)a->info.rate / 2);
	return a;
}

void
media_audio_get_info(const struct media_audio *audio, struct media_audio_info *info)
{
	*info = audio->info;
}

uint32_t
media_audio_set_piece(struct media_audio *a, uint32_t frames)
{
	uint32_t unit = a->unit, p;

	a->info.pieces = 0;
	if (!unit || !a->info.frames)
		return 0;
	p = frames < unit ? unit : (frames + unit - 1) / unit * unit;
	/* (two cores never store to the same 16 bytes of an output) */
	while ((p * (uint32_t)a->info.channels * 2) & 15)
		p += unit;
	a->piece = p;
	a->info.pieces = (a->info.frames + p - 1) / p;
	return a->info.pieces;
}

int
media_audio_read(struct media_audio *a, int16_t *to, int frames)
{
	int got = frames > 0 ? KIND(a, read, a, to, frames) : 0;

	a->at += (uint32_t)got;
	return got;
}

uint32_t
media_audio_seek(struct media_audio *a, uint32_t frame)
{
	return a->at = KIND(a, seek, a, frame);
}

int
media_audio_decoders(struct media_audio *a, int n)
{
	if (n > M_DECODERS)
		n = M_DECODERS;
	while (a->info.pieces && a->decoders < n && KIND(a, decoder, a, a->decoders))
		a->decoders++;
	return a->decoders;
}

uint32_t
media_audio_piece_first(const struct media_audio *a, uint32_t piece)
{
	return piece >= a->info.pieces ? a->info.frames : piece * a->piece;
}

uint32_t
media_audio_piece_frames(const struct media_audio *a, uint32_t piece)
{
	uint32_t first = piece * a->piece;

	if (piece >= a->info.pieces)
		return 0;
	return a->info.frames - first < a->piece ? a->info.frames - first : a->piece;
}

uint32_t
media_audio_piece_of(const struct media_audio *a, uint32_t frame)
{
	return a->piece ? frame / a->piece : 0;
}

uint32_t
media_audio_piece_step(struct media_audio *a, int d, uint32_t piece, int16_t *to, uint32_t done)
{
	uint32_t want = media_audio_piece_frames(a, piece);

	if (done >= want)
		return want;
	if (d < 0 || d >= a->decoders) {
		/* (no decoder: silence, as for a piece that cannot be decoded) */
		memset(to + done * (uint32_t)a->info.channels, 0, (want - done) * (uint32_t)a->info.channels * 2);
		return want;
	}
	return KIND(a, step, a, d, piece, to, done);
}

int
media_audio_piece(struct media_audio *a, int d, uint32_t piece, int16_t *to)
{
	uint32_t want = media_audio_piece_frames(a, piece), done = 0;

	while (done < want)
		done = media_audio_piece_step(a, d, piece, to, done);
	return (int)want;
}

int
media_audio_half(struct media_audio *a, int on)
{
	return a->info.kind == MEDIA_MP3 && m_mp3_half(a, on);
}

int
media_audio_mix_rate(struct media_audio *a, int rate)
{
	return a->info.kind == MEDIA_MOD ? m_mod_rate(a, rate) : a->info.rate;
}

struct all {
	struct media_audio *a;
	int16_t *to;
};

static void
all_pieces(void *ctx, uint32_t from, uint32_t to)
{
	struct all *all = ctx;
	int d = mcw_self();

	for (; from < to; from++)
		media_audio_piece(all->a, d, from, all->to + media_audio_piece_first(all->a, from) * (uint32_t)all->a->info.channels);
}

uint32_t
media_audio_decode_all(struct media_audio *a, int16_t *to, uint32_t most, int workers)
{
	uint32_t ch = (uint32_t)a->info.channels, whole, done, n;
	struct all all = { a, to };
	int got;

	if (!a->info.pieces || ((uintptr_t)to & 15)) {
		/* in order on this core (a tracker tune, a FLAC file of blocks not all as long) */
		media_audio_seek(a, 0);
		for (done = 0; done < most; done += n)
			if (!(n = (uint32_t)media_audio_read(a, to + done * ch, most - done > 4096 ? 4096 : (int)(most - done))))
				break;
		return done;
	}
	whole = most >= a->info.frames ? a->info.pieces : most / a->piece;
	got = m_workers_begin(workers);
	media_audio_decoders(a, mcw_count() + 1);
	mcw_touch(to, media_audio_piece_first(a, whole) * ch * 2);
	m_for(whole, all_pieces, &all, 0, got < a->decoders - 1 ? got : a->decoders - 1);
	m_workers_end();
	done = media_audio_piece_first(a, whole);
	if (whole < a->info.pieces && most > done) {
		/* the piece it ends in, on this core, and as much of it as was asked */
		int16_t *rest = malloc((size_t)a->piece * ch * 2);

		if (rest) {
			media_audio_piece(a, 0, whole, rest);
			memcpy(to + done * ch, rest, (most - done) * ch * 2);
			free(rest);
			done = most;
		}
	}
	return done;
}

void
media_audio_close(struct media_audio *a)
{
	if (!a)
		return;
	KIND(a, close, a);
	free(a);
}
