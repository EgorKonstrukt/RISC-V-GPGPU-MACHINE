/* mpeg.h: an MPEG-1 file taken apart for the worker cores (mpeg.c, over pl_mpeg): its video
 * as closed groups of pictures, each decoded by itself, and its sound as chunks of frames.
 * A frame is three planes of bytes, one after another: luma, then blue and red chroma at half
 * the size each way (what the GPU's fragment mode 6 shows: docs/gpu.md). */
#ifndef SHADEREMU_MPEG_H
#define SHADEREMU_MPEG_H
#include <stdint.h>

#define MPEG_VIDEOS 48		/* decoders at most, each on a worker of its own */
#define MPEG_AUDIOS 16
#define MPEG_GOP_MOST 16	/* pictures in a group, at most */
#define MPEG_CHUNK 14		/* sound frames in a chunk: half a second at 32 kHz */
#define MPEG_AGAIN 0x80000000u	/* in a group's number: go on with it where the last job left it */

struct mpeg_info {
	int width, height;	/* multiples of 16 */
	int fps_milli;		/* pictures in a thousand seconds */
	int frames, gops, gop_most;
	unsigned frame_bytes;	/* a frame's three planes, a multiple of 16 */
	int rate;		/* the sound's samples a second; 0 when there is none */
	int chunks, pcm_frames;	/* its chunks, and its stereo frames in all */
};

/* The pictures' size, from the first sequence header in the file's first bytes: quick, for a
 * window to be made before the file is taken apart. False when none is found there. */
int mpeg_peek(const uint8_t *file, unsigned bytes, int *width, int *height);
/* Takes the file apart, on the core that calls. NULL, or what is wrong with it. */
const char *mpeg_open(uint8_t *file, unsigned bytes, struct mpeg_info *info);
int mpeg_gop_of(int frame);	/* the group a picture is in */
int mpeg_gop_first(int gop);	/* a group's first picture's number, and how many it has */
int mpeg_gop_frames(int gop);
int16_t *mpeg_pcm(void);	/* the sound as it is decoded: left and right, 16 bits each */

/* A decoder with room for one group's pictures, for worker `worker` to use; -1 when there are enough. */
int mpeg_video_new(int worker);
int mpeg_audio_new(void);
/* Picture `n` of the decoder's group added to a sum, as the frame it is copied as. */
uint32_t mpeg_video_sum(int video, int n, uint32_t sum);
int mpeg_video_done(int video);	/* pictures of its group the decoder has got through */

/* The workers' jobs (a0, a1): each returns how many pictures, or sound frames, it did. */
uint32_t mpeg_decode_gop(uint32_t video, uint32_t gop);
uint32_t mpeg_copy_one(uint32_t which, uint32_t to);	/* picture (which & 255) of decoder (which >> 8)'s group, as a frame at `to` */
uint32_t mpeg_decode_chunk(uint32_t audio, uint32_t chunk);

/* The sound decoded from its first frame to its last in one go, summed: what the chunks must come to. */
uint32_t mpeg_sound_whole(void);

extern volatile int mpeg_stop;	/* set to end the jobs that are running */
extern volatile int mpeg_drop;	/* set to end the pictures' jobs only: what they decode is not wanted (a seek) */

/* A sum of a frame's bytes, for a test to compare with another machine's. */
uint32_t mpeg_sum(const uint8_t *frame, unsigned bytes, uint32_t sum);
#endif
