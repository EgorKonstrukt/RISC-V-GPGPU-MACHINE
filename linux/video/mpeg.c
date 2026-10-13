/*
 * mpeg.c: pl_mpeg (Dominic Szablewski's MPEG-1 decoder, MIT; fetched by build.sh) arranged
 * for the worker cores: docs/video.md. The file is taken apart once on the first core; after
 * that a job reads the streams and writes only memory that is its decoder's own (mcw_alloc:
 * whole 16 bytes, which no other core writes).
 *
 * A decoder is used again for group after group: it is put back as it was made (a copy of
 * the whole struct, its frames' memory kept) and pointed at the next group's bytes.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mpeg.h"

#ifdef MPEG_HOST
/* a build for another machine, to compare sums with (tools: linux/video/build.sh) */
static void *mcw_alloc(unsigned bytes) { return aligned_alloc(16, (bytes + 15) & ~15u); }
static void mcw_touch(void *memory, unsigned bytes) { (void)memory; (void)bytes; }
static int mcw_recalled(int k) { (void)k; return 0; }
#else
#include "mcw.h"
#endif

/* Memory a worker may write: there now, every page of it. `room` is zeros; `space` is whatever
 * it was, for pictures that are written before they are read (clearing megabytes takes seconds). */
static void *
space(unsigned bytes)
{
	void *memory = mcw_alloc(bytes);

	if (!memory)
		abort();
	mcw_touch(memory, bytes);
	return memory;
}

static void *
room(unsigned bytes)
{
	return memset(space(bytes), 0, bytes);
}

/* A decoder's own three pictures are never used (a picture is decoded where the group keeps
 * it, below): every decoder has the same ones, and they are brought in once. */
static unsigned own_bytes;
static void *own_frames;

static void *
large(unsigned bytes)
{
	if (!own_bytes || bytes != own_bytes)
		return space(bytes);
	if (!own_frames)
		own_frames = space(bytes);
	return own_frames;
}

#define PLM_NO_STDIO
#define PLM_MALLOC(sz) ((sz) > 4096 ? large((unsigned)(sz)) : room((unsigned)(sz)))
#define PLM_FREE(p) ((void)(p))
#define PLM_REALLOC(p, sz) (abort(), (void *)(p))	/* (only a buffer that grows asks, and none here does) */
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

struct video {
	plm_video_t *decoder, *made;	/* made: as it was before its first picture */
	plm_buffer_t buffer;		/* over the group being decoded */
	uint8_t *frames;		/* the group's pictures, one after another */
	uint8_t *last[3];		/* where the last pictures were decoded, by their number's remainder */
	int worker, done;
};
struct audio {
	plm_audio_t *decoder, *made;
	plm_buffer_t buffer;
};

static struct mpeg_info info;
static uint8_t *video_es, *audio_es;
static unsigned video_bytes, audio_bytes;
static struct { unsigned at, bytes; int first, frames; } *gops;
static unsigned *sound_at;		/* where each sound frame begins, and one more: the end */
static int sound_frames;
static int16_t *pcm;
static struct video *videos[MPEG_VIDEOS];
static struct audio *audios[MPEG_AUDIOS];
static int video_count, audio_count;
volatile int mpeg_stop;		/* the program is leaving: jobs end where they are */
volatile int mpeg_drop;		/* a seek: the groups being decoded are not wanted any more */

/* A buffer over bytes that are there already, without asking pl_mpeg for memory. */
static void
over(plm_buffer_t *buffer, uint8_t *bytes, unsigned length)
{
	memset(buffer, 0, sizeof *buffer);
	buffer->bytes = bytes;
	buffer->capacity = buffer->length = buffer->total_size = length;
	buffer->mode = PLM_BUFFER_MODE_FIXED_MEM;
}

/* The next start code (00 00 01, then which) from `at` on: where it is, or `bytes` when there is none. */
static unsigned
next_code(const uint8_t *es, unsigned bytes, unsigned at, int *code)
{
	/*
	 * `at` is where the code's 1 would be. A byte over 1 there is no part of a code, so
	 * none has its 1 at the two places after it either: two bytes of three are not read.
	 */
	for (at += 2; at + 1 < bytes;) {
		unsigned b = es[at];

		if (b > 1)
			at += 3;
		else if (b == 0)
			at++;
		else if (es[at - 1] == 0 && es[at - 2] == 0) {
			*code = es[at + 1];
			return at - 2;
		} else
			at += 3;
	}
	return bytes;
}

/* The length of the layer II sound frame that begins at p, or 0 when none does. */
static unsigned
sound_frame(const uint8_t *p)
{
	static const int kbps[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
	static const int hz[4] = { 44100, 48000, 32000, 0 };
	int bits = kbps[p[2] >> 4], rate = hz[(p[2] >> 2) & 3];

	if (p[0] != 0xff || (p[1] & 0xfe) != 0xfc || !bits || !rate)
		return 0;
	return (unsigned)(144000 * bits / rate) + ((p[2] >> 1) & 1);
}

int
mpeg_peek(const uint8_t *file, unsigned bytes, int *width, int *height)
{
	unsigned at;

	/* 00 00 01 B3, then twelve bits of width and twelve of height */
	for (at = 0; at + 8 <= bytes && at < 65536; at++)
		if (file[at] == 0 && file[at + 1] == 0 && file[at + 2] == 1 && file[at + 3] == 0xb3) {
			*width = file[at + 4] << 4 | file[at + 5] >> 4;
			*height = (file[at + 5] & 15) << 8 | file[at + 6];
			return *width > 0 && *height > 0;
		}
	return 0;
}

const char *
mpeg_open(uint8_t *file, unsigned bytes, struct mpeg_info *out)
{
	plm_buffer_t whole, probe;
	plm_demux_t *demux;
	plm_packet_t *packet;
	plm_video_t *video;
	unsigned at, n, headers = 0, groups = 0;
	int code, begins, g;

	video_es = space(bytes);
	audio_es = space(bytes);
	over(&whole, file, bytes);
	demux = plm_demux_create(&whole, 0);
	while ((packet = plm_demux_decode(demux)) != NULL) {
		if (packet->type == PLM_DEMUX_PACKET_VIDEO_1 && video_bytes + packet->length <= bytes) {
			memcpy(video_es + video_bytes, packet->data, packet->length);
			video_bytes += (unsigned)packet->length;
		} else if (packet->type == PLM_DEMUX_PACKET_AUDIO_1 && audio_bytes + packet->length <= bytes) {
			memcpy(audio_es + audio_bytes, packet->data, packet->length);
			audio_bytes += (unsigned)packet->length;
		}
	}
	if (!video_bytes)
		return "not an MPEG-1 program stream with video in it";
	over(&probe, video_es, video_bytes);
	video = plm_video_create_with_buffer(&probe, 0);
	if (!plm_video_has_header(video))
		return "its video has no sequence header";
	info.width = plm_video_get_width(video);
	info.height = plm_video_get_height(video);
	info.fps_milli = (int)(plm_video_get_framerate(video) * 1000.0 + 0.5);
	if ((info.width | info.height) & 15 || info.width > 1024 || info.height > 768 || info.fps_milli < 1000)
		return "its pictures' width and height must be multiples of 16, 1024 x 768 at most";
	info.frame_bytes = (unsigned)(info.width * info.height * 3 / 2);

	/* Its groups of pictures: each begins at a sequence header when every group has one, else
	 * at its own header. One pass over the stream's start codes notes the headers and the
	 * pictures (a slice's code is neither); the groups are made from the notes. */
	{
		struct { unsigned at; int code; } *notes = NULL;
		unsigned noted = 0, held = 0, k;

		for (at = next_code(video_es, video_bytes, 0, &code); at < video_bytes; at = next_code(video_es, video_bytes, at + 4, &code)) {
			if (code != 0xb3 && code != 0xb8 && code != 0x00)
				continue;
			if (noted == held && (notes = realloc(notes, (held += 1024) * sizeof *notes)) == NULL)
				abort();
			notes[noted].at = at;
			notes[noted++].code = code;
			headers += code == 0xb3;
			groups += code == 0xb8;
		}
		begins = headers > 1 ? 0xb3 : 0xb8;
		gops = room(((headers > 1 ? headers : groups) + 1) * sizeof *gops);
		g = -1;
		for (k = 0; k < noted; k++) {
			at = notes[k].at;
			if (notes[k].code == begins) {
				if (g >= 0)
					gops[g].bytes = at - gops[g].at;
				g++;
				gops[g].at = g ? at : 0;
				gops[g].first = info.frames;
			} else if (notes[k].code == 0x00 && g >= 0) {
				gops[g].frames++;	/* a picture */
				info.frames++;
			}
		}
		free(notes);
	}
	if (g < 0)
		return "its video has no groups of pictures";
	gops[g].bytes = video_bytes - gops[g].at;
	info.gops = g + 1;
	for (g = 0; g < info.gops; g++) {
		if (gops[g].frames > MPEG_GOP_MOST)
			return "a group of its pictures is longer than 16 (encode it with -g 6, closed)";
		if (gops[g].frames > info.gop_most)
			info.gop_most = gops[g].frames;
	}

	/* its sound's frames (layer II, 1152 samples each) */
	for (at = 0; at + 4 <= audio_bytes && (n = sound_frame(audio_es + at)) != 0 && at + n <= audio_bytes; at += n)
		sound_frames++;
	if (sound_frames) {
		static const int hz[4] = { 44100, 48000, 32000, 0 };

		sound_at = room((unsigned)(sound_frames + 1) * sizeof *sound_at);
		for (g = 0, at = 0; g < sound_frames; at += sound_frame(audio_es + at), g++)
			sound_at[g] = at;
		sound_at[g] = at;
		info.rate = hz[(audio_es[2] >> 2) & 3];
		info.chunks = (sound_frames + MPEG_CHUNK - 1) / MPEG_CHUNK;
		info.pcm_frames = sound_frames * PLM_AUDIO_SAMPLES_PER_FRAME;
		pcm = space((unsigned)info.pcm_frames * 4);
	}
	*out = info;
	return NULL;
}

int
mpeg_gop_of(int frame)
{
	int low = 0, high = info.gops - 1, middle;

	while (low < high) {
		middle = (low + high + 1) / 2;
		if (gops[middle].first <= frame)
			low = middle;
		else
			high = middle - 1;
	}
	return low;
}

int mpeg_gop_first(int gop) { return gops[gop].first; }
int mpeg_gop_frames(int gop) { return gops[gop].frames; }
int16_t *mpeg_pcm(void) { return pcm; }

int
mpeg_video_new(int worker)
{
	struct video *x;
	plm_buffer_t probe;

	if (video_count == MPEG_VIDEOS)
		return -1;
	x = room(sizeof *x);
	over(&probe, video_es, video_bytes);
	own_bytes = 3 * info.frame_bytes;
	x->decoder = plm_video_create_with_buffer(&probe, 0);
	own_bytes = 0;
	plm_video_set_no_delay(x->decoder, 1);	/* no B pictures: each is out as soon as it is decoded */
	x->made = room(sizeof *x->made);
	*x->made = *x->decoder;
	x->frames = space((unsigned)info.gop_most * info.frame_bytes);
	x->worker = worker;
	videos[video_count] = x;
	return video_count++;
}

int
mpeg_audio_new(void)
{
	struct audio *x;
	plm_buffer_t probe;

	if (audio_count == MPEG_AUDIOS || !sound_frames)
		return -1;
	x = room(sizeof *x);
	over(&probe, audio_es, 0);	/* (nothing to read: it finds its first header in a job) */
	x->decoder = plm_audio_create_with_buffer(&probe, 0);
	x->made = room(sizeof *x->made);
	*x->made = *x->decoder;
	audios[audio_count] = x;
	return audio_count++;
}


/* The next picture is decoded where it is kept, and the picture before it is read there:
 * nothing is copied. Its planes lie in pl_mpeg's order, red chroma before blue (a block at a
 * plane's edge is predicted from bytes past it, as it was); the width and height are whole
 * macroblocks, so the planes are the picture's. */
static void
decode_into(plm_video_t *decoder, uint8_t *to, uint8_t *before)
{
	unsigned luma = (unsigned)(info.width * info.height);

	/* (what a block that is not predicted keeps: plm_fast.py) */
	decoder->stale = before ? before - to : 0;
	decoder->frame_current.y.data = to;
	decoder->frame_current.cr.data = to + luma;
	decoder->frame_current.cb.data = to + luma + luma / 4;
}

/* A macroblock of a kept picture copied from another's. */
static void
block_from(uint8_t *to, const uint8_t *from, int column, int row)
{
	unsigned luma = (unsigned)(info.width * info.height), at = (unsigned)(row * 16 * info.width + column * 16);
	unsigned chroma = luma + (unsigned)(row * 8 * (info.width / 2) + column * 8);
	int y;

	for (y = 0; y < 16; y++)
		memcpy(to + at + (unsigned)(y * info.width), from + at + (unsigned)(y * info.width), 16);
	for (y = 0; y < 8; y++) {
		memcpy(to + chroma + (unsigned)(y * (info.width / 2)), from + chroma + (unsigned)(y * (info.width / 2)), 8);
		memcpy(to + chroma + luma / 4 + (unsigned)(y * (info.width / 2)), from + chroma + luma / 4 + (unsigned)(y * (info.width / 2)), 8);
	}
}

int mpeg_video_done(int video) { return videos[video]->done; }

/* A kept picture as a frame: luma, blue chroma, red chroma. */
static void
as_frame(uint8_t *to, const uint8_t *kept)
{
	unsigned luma = (unsigned)(info.width * info.height);

	memcpy(to, kept, luma);
	memcpy(to + luma, kept + luma + luma / 4, luma / 4);
	memcpy(to + luma + luma / 4, kept + luma, luma / 4);
}

uint32_t
mpeg_video_sum(int video, int n, uint32_t sum)
{
	const uint8_t *kept = videos[video]->frames + (unsigned)n * info.frame_bytes;
	unsigned luma = (unsigned)(info.width * info.height);

	sum = mpeg_sum(kept, luma, sum);
	sum = mpeg_sum(kept + luma + luma / 4, luma / 4, sum);
	return mpeg_sum(kept + luma, luma / 4, sum);
}

uint32_t
mpeg_decode_gop(uint32_t video, uint32_t gop)
{
	struct video *x = videos[video];
	plm_frame_t *frame;
	uint8_t *to, *before;
	int g = (int)(gop & ~MPEG_AGAIN), mb;

	if (!(gop & MPEG_AGAIN)) {
		*x->decoder = *x->made;
		over(&x->buffer, video_es + gops[g].at, gops[g].bytes);
		x->decoder->buffer = &x->buffer;
		x->decoder->start_code = -1;
		x->done = 0;
	}
	while (x->done < gops[g].frames) {
		/* pl_mpeg went round three pictures of its own: a block it does not predict kept the
		 * picture three before, or the one of the group before that lay there */
		to = x->frames + (unsigned)x->done * info.frame_bytes;
		before = x->last[x->done % 3];
		decode_into(x->decoder, to, before);
		x->last[x->done % 3] = to;
		if ((frame = plm_video_decode(x->decoder)) == NULL)
			break;
		/* and so did the macroblocks after the last one a picture's slices came to */
		if (before && before != to)
			for (mb = x->decoder->macroblock_address + 1; mb < x->decoder->mb_size; mb++)
				block_from(to, before, mb % x->decoder->mb_width, mb / x->decoder->mb_width);
		x->done++;
		if (mpeg_stop || mpeg_drop)
			return (uint32_t)gops[g].frames;
		if (mcw_recalled(x->worker))	/* the kernel wants this core: the rest in another job */
			break;
	}
	return (uint32_t)x->done;
}

uint32_t
mpeg_copy_one(uint32_t which, uint32_t to)
{
	as_frame((uint8_t *)to, videos[which >> 8]->frames + (which & 255) * info.frame_bytes);
	return 1;
}

uint32_t
mpeg_decode_chunk(uint32_t audio, uint32_t chunk)
{
	struct audio *x = audios[audio];
	int first = (int)chunk * MPEG_CHUNK, end = first + MPEG_CHUNK < sound_frames ? first + MPEG_CHUNK : sound_frames;
	int from = first ? first - 1 : 0, f, i, v;	/* a frame early: its samples fill the filter's past */
	plm_samples_t *samples;
	int16_t *to = pcm + (unsigned)first * PLM_AUDIO_SAMPLES_PER_FRAME * 2;

	*x->decoder = *x->made;
	over(&x->buffer, audio_es + sound_at[from], sound_at[end] - sound_at[from]);
	x->decoder->buffer = &x->buffer;
	x->decoder->next_frame_data_size = 0;
	/* where the filter's ring would be had every frame before been decoded (36 steps of 64 a
	 * frame): the same sums in the same order, and so the same samples to the last bit */
	x->decoder->v_pos = (int)(0u - 2304u * (unsigned)from) & 1023;
	for (f = from; f < end && !mpeg_stop && (samples = plm_audio_decode(x->decoder)) != NULL; f++) {
		if (f < first)
			continue;
		for (i = 0; i < PLM_AUDIO_SAMPLES_PER_FRAME * 2; i++) {
			v = (int)(samples->interleaved[i] * 32767.0f);
			*to++ = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
	return (uint32_t)(f - first);
}

uint32_t
mpeg_sound_whole(void)
{
	plm_buffer_t buffer;
	plm_audio_t *decoder;
	plm_samples_t *samples;
	int16_t out[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
	uint32_t sum = 0;
	int i, v;

	over(&buffer, audio_es, audio_bytes);
	decoder = plm_audio_create_with_buffer(&buffer, 0);
	while ((samples = plm_audio_decode(decoder)) != NULL) {
		for (i = 0; i < PLM_AUDIO_SAMPLES_PER_FRAME * 2; i++) {
			v = (int)(samples->interleaved[i] * 32767.0f);
			out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		sum = mpeg_sum((const uint8_t *)out, sizeof out, sum);
	}
	return sum;
}

uint32_t
mpeg_sum(const uint8_t *frame, unsigned bytes, uint32_t sum)
{
	uint32_t word;
	unsigned i;

	for (i = 0; i + 4 <= bytes; i += 4) {
		memcpy(&word, frame + i, 4);
		sum = (sum << 5 | sum >> 27) ^ word;
	}
	return sum;
}
