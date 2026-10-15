/* libmedia's films (docs/media.md, "Films"): what the kinds of film share. */
#ifndef SHADEREMU_MEDIA_VIDEO_H
#define SHADEREMU_MEDIA_VIDEO_H

#include "m_int.h"

struct media_video;

/* What a kind of film does; frame() is asked only for 0 <= n < frames. */
struct m_film {
	const uint8_t *(*frame)(struct media_video *v, int n, const uint32_t **palette);
	int (*ready)(struct media_video *v, int *in_time);
	int (*seek)(struct media_video *v, int n);
	uint32_t (*sound)(struct media_video *v, uint32_t first, int16_t *to, uint32_t most);
	void (*pump)(struct media_video *v);
	void (*close)(struct media_video *v);
	uint32_t (*time)(const struct media_video *v, int n);	/* NULL: by fps_milli */
};

struct media_video {
	struct media_video_info info;
	const struct m_film *film;
	void *own;			/* the kind's */
	uint8_t **places;		/* where pictures go (media_video_places), or NULL */
	int slots;
	int opened;			/* the workers were opened for the film */
};

/* Up to `want` workers for a film: the program's own, or opened (closed by m_film_workers_end). */
int m_film_workers(struct media_video *v, int want);
void m_film_workers_end(struct media_video *v);
uint32_t m_now_ms(void);

const char *m_mpeg_open(struct media_video *v, const uint8_t *data, uint32_t size, int workers);
const char *m_vqa_open(struct media_video *v, const uint8_t *data, uint32_t size, int workers);
const char *m_avi_open(struct media_video *v, const uint8_t *data, uint32_t size, int workers);

/* Westwood's pieces (m_vq.c): block rows from..to-1 of a frame, and LCW with its source's length. */
struct m_vq {
	const uint8_t *codebook, *pointers;
	uint8_t *out;
	uint32_t blocks_per_row, num_rows, out_width;
	int tall;		/* 4: 4x4 blocks, solid at 0xff; 2: 4x2, solid at 0x0f */
};
void m_vq_rows(void *job, uint32_t from, uint32_t to);
uint32_t m_lcw(const uint8_t *source, uint32_t source_bytes, uint8_t *dest, uint32_t length);

#endif
