/* libmedia's sound (docs/media.md): what m_audio.c shares with each kind's decoder. */
#ifndef SHADEREMU_MEDIA_AUDIO_H
#define SHADEREMU_MEDIA_AUDIO_H

#include "m_int.h"

#define M_DECODERS MC_MAX_CORES

struct media_audio {
	struct media_audio_info info;	/* as the caller sees it (at half rate: halved) */
	const uint8_t *data;
	uint32_t size;
	uint32_t unit;		/* a piece is whole units (an MP3 frame's samples, a FLAC block) */
	uint32_t piece;		/* frames a piece, a multiple of unit and of 16 bytes */
	uint32_t at;		/* where media_audio_read is */
	int half, decoders;
	void *dec[M_DECODERS];	/* decoder d's state: memory no other core stores to */
	void *own;		/* the kind's state, made on the first core */
};

/*
 * Each kind: open fills info (and unit, 0 for no pieces), read and seek are in order on the
 * first core, decoder makes decoder d there, and step goes on with a piece on any core: from
 * frame `done` of it, some frames more, and how many of it are done then.
 */
#define M_AUDIO_KIND(k) \
	const char *m_##k##_open(struct media_audio *a); \
	int m_##k##_read(struct media_audio *a, int16_t *to, int frames); \
	uint32_t m_##k##_seek(struct media_audio *a, uint32_t frame); \
	int m_##k##_decoder(struct media_audio *a, int d); \
	uint32_t m_##k##_step(struct media_audio *a, int d, uint32_t piece, int16_t *to, uint32_t done); \
	void m_##k##_close(struct media_audio *a);
M_AUDIO_KIND(wav)
M_AUDIO_KIND(mp3)
M_AUDIO_KIND(flac)
M_AUDIO_KIND(vorbis)
M_AUDIO_KIND(mod)

/* MP3 only: half the rate (decoders are made again); whether it took. */
int m_mp3_half(struct media_audio *a, int on);
/* A tracker tune's mixing rate. */
int m_mod_rate(struct media_audio *a, int rate);

/* Memory of a decoder's own: whole pages, there before a worker stores to them. */
void *m_audio_pages(uint32_t bytes);
void m_audio_pages_free(void *memory);

#endif
