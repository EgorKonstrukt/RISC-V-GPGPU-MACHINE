/*
 * MOD (docs/media.md): a ProTracker tune (31 samples, 4 channels, or more with the tags 6CHN,
 * 8CHN, nnCH) played in integers, in order on one core: the sequencer a tick at a time, and a
 * mixer of the nearest sample. XM is not played.
 */
#include <stdlib.h>
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "m_audio.h"

#define MOD_CHANNELS 32
#define MOD_CHUNK 256	/* frames mixed at a time */

struct mod_sample {
	const int8_t *data;
	uint32_t length, loop_at, loop_length;	/* loop_length 0: played once */
	int volume, finetune;
};

struct mod_channel {
	const struct mod_sample *sample, *named;	/* named: the row's, taken at the next note */
	uint32_t at, step;		/* where in the sample, and a frame's step: 18.14 */
	int period, target, volume, finetune;
	int out_period, out_volume;	/* what this tick sounds like, effects included */
	int effect, value;
	int slide, vib_speed, vib_depth, vib_at, trem_speed, trem_depth, trem_at;
	int offset, loop_row, loop_count, delayed;
};

struct mod_state {
	const uint8_t *order, *patterns;
	int channels, orders, shift;
	struct mod_sample samples[32];
	struct mod_channel ch[MOD_CHANNELS];
	int rate;
	uint32_t clock;			/* a period's step is this over the period */
	int speed, bpm, tick, row, at, delay;
	int next_row, next_order, stay, ended;
	uint32_t tick_left, tick_carry;
	uint8_t seen[128];
};
static struct mod_state *modp;	/* the tune the calls below are about */
#define mod (*modp)

/* 65536 * 2^(-finetune / 96), finetune as written (8 to 15 are -8 to -1); and by semitones */
static const uint32_t mod_tune[16] = {
	65536, 65065, 64596, 64132, 63670, 63212, 62757, 62306, 69433, 68933, 68438, 67945, 67456, 66971, 66489, 66011
};
static const uint32_t mod_semitone[16] = {
	65536, 61858, 58386, 55109, 52016, 49097, 46341, 43740, 41285, 38968, 36781, 34716, 32768, 30929, 29193, 27554
};
static const uint8_t mod_sine[32] = {
	0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
	255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24
};

static int
mod_wave(int at, int depth)
{
	int v = mod_sine[at & 31] * depth >> 7;

	return (at & 32) ? -v : v;
}

static void
mod_enter(int order, int row)
{
	/* an order played before is the tune going round: its end */
	if (order >= mod.orders || mod.seen[order]) {
		mod.ended = 1;
		return;
	}
	mod.seen[order] = 1;
	mod.at = order;
	mod.row = row;
}

static void
mod_reset(void)
{
	memset(mod.ch, 0, sizeof mod.ch);
	memset(mod.seen, 0, sizeof mod.seen);
	mod.speed = 6;
	mod.bpm = 125;
	mod.tick = mod.delay = mod.ended = mod.stay = 0;
	mod.next_order = -1;
	mod.next_row = 0;
	mod.tick_left = mod.tick_carry = 0;
	mod_enter(0, 0);
}

static void
mod_note(struct mod_channel *ch, int period)
{
	if (ch->named)
		ch->sample = ch->named;
	ch->period = (int)((uint32_t)period * mod_tune[ch->finetune & 15] >> 16);
	ch->at = 0;
	ch->vib_at = ch->trem_at = 0;
}

static void
mod_volume_slide(struct mod_channel *ch)
{
	ch->volume += (ch->value >> 4) ? ch->value >> 4 : -(ch->value & 15);
	ch->volume = ch->volume < 0 ? 0 : ch->volume > 64 ? 64 : ch->volume;
}

static void
mod_slide(struct mod_channel *ch)
{
	if (!ch->target || !ch->period)
		return;
	if (ch->period < ch->target)
		ch->period = ch->period + ch->slide > ch->target ? ch->target : ch->period + ch->slide;
	else
		ch->period = ch->period - ch->slide < ch->target ? ch->target : ch->period - ch->slide;
}

/* A row's first tick: its notes, and the effects that happen once. */
static void
mod_row(void)
{
	const uint8_t *cell = mod.patterns + ((uint32_t)mod.order[mod.at] * 64 + mod.row) * mod.channels * 4;
	int c;

	for (c = 0; c < mod.channels; c++, cell += 4) {
		struct mod_channel *ch = &mod.ch[c];
		int n = (cell[0] & 0xf0) | cell[2] >> 4, period = (cell[0] & 15) << 8 | cell[1];
		int effect = cell[2] & 15, value = cell[3], x = value & 15;

		ch->effect = effect;
		ch->value = value;
		ch->delayed = 0;
		if (n > 0 && n < 32) {
			ch->named = &mod.samples[n];
			ch->volume = ch->named->volume;
			ch->finetune = ch->named->finetune;
		}
		if (effect == 0xE && value >> 4 == 5)
			ch->finetune = x;
		if (period) {
			if (effect == 3 || effect == 5)
				ch->target = (int)((uint32_t)period * mod_tune[ch->finetune & 15] >> 16);
			else if (effect == 0xE && value >> 4 == 0xD && x)
				ch->delayed = period;
			else
				mod_note(ch, period);
		}
		switch (effect) {
		case 3:
			if (value)
				ch->slide = value;
			break;
		case 4:
			if (value >> 4)
				ch->vib_speed = value >> 4;
			if (x)
				ch->vib_depth = x;
			break;
		case 7:
			if (value >> 4)
				ch->trem_speed = value >> 4;
			if (x)
				ch->trem_depth = x;
			break;
		case 9:
			if (value)
				ch->offset = value;
			if (period && ch->sample && (uint32_t)ch->offset << 8 < ch->sample->length)
				ch->at = (uint32_t)ch->offset << 22;
			break;
		case 0xB:
			mod.next_order = value;
			mod.next_row = 0;
			break;
		case 0xC:
			ch->volume = value > 64 ? 64 : value;
			break;
		case 0xD:
			if (mod.next_order < 0)
				mod.next_order = mod.at + 1;
			mod.next_row = (value >> 4) * 10 + x;
			if (mod.next_row > 63)
				mod.next_row = 0;
			break;
		case 0xE:
			switch (value >> 4) {
			case 1: ch->period -= x; break;
			case 2: ch->period += x; break;
			case 6:
				if (!x) {
					ch->loop_row = mod.row;
				} else if (ch->loop_count ? --ch->loop_count : (ch->loop_count = x)) {
					mod.next_row = ch->loop_row;
					mod.stay = 1;
				}
				break;
			case 0xA: ch->volume = ch->volume + x > 64 ? 64 : ch->volume + x; break;
			case 0xB: ch->volume = ch->volume < x ? 0 : ch->volume - x; break;
			case 0xE: mod.delay = x; break;
			}
			break;
		case 0xF:
			if (!value)
				mod.ended = 1;
			else if (value < 32)
				mod.speed = value;
			else
				mod.bpm = value;
			break;
		}
	}
}

/* The other ticks of a row: what slides and what wobbles. */
static void
mod_effects(void)
{
	int c;

	for (c = 0; c < mod.channels; c++) {
		struct mod_channel *ch = &mod.ch[c];
		int x = ch->value & 15;

		switch (ch->effect) {
		case 1:
			ch->period = ch->period - ch->value < 113 ? 113 : ch->period - ch->value;
			break;
		case 2:
			ch->period = ch->period + ch->value > 856 ? 856 : ch->period + ch->value;
			break;
		case 3:
			mod_slide(ch);
			break;
		case 5:
			mod_slide(ch);
			mod_volume_slide(ch);
			break;
		case 4:
			ch->vib_at += ch->vib_speed;
			break;
		case 6:
			ch->vib_at += ch->vib_speed;
			mod_volume_slide(ch);
			break;
		case 7:
			ch->trem_at += ch->trem_speed;
			break;
		case 0xA:
			mod_volume_slide(ch);
			break;
		case 0xE:
			if (ch->value >> 4 == 9 && x && mod.tick % x == 0)
				ch->at = 0;
			else if (ch->value >> 4 == 0xC && mod.tick == x)
				ch->volume = 0;
			else if (ch->value >> 4 == 0xD && mod.tick == x && ch->delayed)
				mod_note(ch, ch->delayed);
			break;
		}
	}
}

/* One tick of the sequencer: every channel's pitch and volume until the next. */
static void
mod_tick(void)
{
	int c, per_row;

	if (mod.tick == 0)
		mod_row();
	else
		mod_effects();
	for (c = 0; c < mod.channels; c++) {
		struct mod_channel *ch = &mod.ch[c];
		int period = ch->period, volume = ch->volume;

		if (ch->effect == 0 && ch->value) {
			int step = mod.tick % 3;

			period = (int)((uint32_t)period * mod_semitone[step == 0 ? 0 : step == 1 ? ch->value >> 4 : ch->value & 15] >> 16);
		} else if (ch->effect == 4 || ch->effect == 6) {
			period += mod_wave(ch->vib_at, ch->vib_depth);
		} else if (ch->effect == 7) {
			volume += mod_wave(ch->trem_at, ch->trem_depth);
			volume = volume < 0 ? 0 : volume > 64 ? 64 : volume;
		}
		if (period != ch->out_period) {
			ch->out_period = period;
			ch->step = period > 0 ? mod.clock / (uint32_t)period : 0;
		}
		ch->out_volume = volume;
	}
	per_row = mod.speed * (1 + mod.delay);
	if (++mod.tick >= per_row) {
		mod.tick = mod.delay = 0;
		if (mod.stay)
			mod.row = mod.next_row;
		else if (mod.next_order >= 0)
			mod_enter(mod.next_order, mod.next_row);
		else if (++mod.row == 64)
			mod_enter(mod.at + 1, 0);
		mod.stay = 0;
		mod.next_order = -1;
		mod.next_row = 0;
	}
	/* a tick is 2.5 / bpm seconds */
	mod.tick_carry += (uint32_t)mod.rate * 5;
	mod.tick_left = mod.tick_carry / (uint32_t)(mod.bpm * 2);
	mod.tick_carry -= mod.tick_left * (uint32_t)(mod.bpm * 2);
}

/* Adds a channel's next `count` frames to its side's sums. */
static void
mod_mix(struct mod_channel *ch, int32_t *sum, int count)
{
	const struct mod_sample *s = ch->sample;
	uint32_t at = ch->at, step = ch->step, end, loop;
	int volume = ch->out_volume;

	if (!s || !s->length || !step)
		return;
	end = (s->loop_length ? s->loop_at + s->loop_length : s->length) << 14;
	loop = s->loop_length << 14;
	while (count > 0) {
		uint32_t run;

		if (at >= end) {
			if (!loop)
				break;
			at = end - loop + (at - end) % loop;
		}
		run = (end - at + step - 1) / step;
		if (run > (uint32_t)count)
			run = (uint32_t)count;
		count -= (int)run;
		if (volume) {
			const int8_t *data = s->data;

			do {
				*sum++ += data[at >> 14] * volume;
				at += step;
			} while (--run);
		} else {
			at += step * run;
			sum += run;
		}
	}
	ch->at = at;
}

/* Four channels' next `count` frames straight into the stream: each frame is the four
 * samples added in registers, for as long as no channel comes to its sample's end. */
static void
mod_mix4(int16_t *to, int count)
{
	static const int8_t nothing[1];
	const int8_t *data[4];
	uint32_t at[4], step[4], end[4], loop[4], kept[4];
	int volume[4], alive[4], c, shift = mod.shift;

	for (c = 0; c < 4; c++) {
		const struct mod_sample *s = mod.ch[c].sample;

		alive[c] = s && s->length && mod.ch[c].step;
		kept[c] = at[c] = mod.ch[c].at;
		if (alive[c]) {
			data[c] = s->data, step[c] = mod.ch[c].step, volume[c] = mod.ch[c].out_volume;
			end[c] = (s->loop_length ? s->loop_at + s->loop_length : s->length) << 14;
			loop[c] = s->loop_length << 14;
		} else {
			data[c] = nothing, at[c] = step[c] = 0, volume[c] = 0;
		}
	}
	while (count > 0) {
		uint32_t run = (uint32_t)count, a0, a1, a2, a3, s0, s1, s2, s3;
		const int8_t *d0, *d1, *d2, *d3;
		int v0, v1, v2, v3;

		for (c = 0; c < 4; c++) {
			uint32_t most;

			if (!alive[c])
				continue;
			if (at[c] >= end[c]) {
				if (!loop[c]) {
					/* played out: it stays where it is, and adds nothing */
					alive[c] = 0, kept[c] = at[c];
					data[c] = nothing, at[c] = step[c] = 0, volume[c] = 0;
					continue;
				}
				at[c] = end[c] - loop[c] + (at[c] - end[c]) % loop[c];
			}
			most = (end[c] - at[c] + step[c] - 1) / step[c];
			if (most < run)
				run = most;
		}
		count -= (int)run;
		a0 = at[0], a1 = at[1], a2 = at[2], a3 = at[3], s0 = step[0], s1 = step[1], s2 = step[2], s3 = step[3];
		d0 = data[0], d1 = data[1], d2 = data[2], d3 = data[3], v0 = volume[0], v1 = volume[1], v2 = volume[2], v3 = volume[3];
		for (; run; run--) {
			/* the Amiga's sides: left, right, right, left; a quarter of each is heard on the other */
			int left = d0[a0 >> 14] * v0 + d3[a3 >> 14] * v3, right = d1[a1 >> 14] * v1 + d2[a2 >> 14] * v2;
			int l = (left * 3 + right) >> shift, r = (right * 3 + left) >> shift;

			a0 += s0, a1 += s1, a2 += s2, a3 += s3;
			to[0] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
			to[1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
			to += 2;
		}
		at[0] = a0, at[1] = a1, at[2] = a2, at[3] = a3;
	}
	for (c = 0; c < 4; c++)
		mod.ch[c].at = alive[c] ? at[c] : kept[c];
}

/*
 * The next frames of the tune, left and right; fewer than asked for at its end. Without `to`
 * only the sequencer runs (the samples stay where they are): that is how a place is found.
 */
static int
mod_read(int16_t *to, int frames)
{
	static int32_t left[MOD_CHUNK], right[MOD_CHUNK];
	int got = 0, c, i;

	while (got < frames) {
		int n = frames - got;

		if (!mod.tick_left) {
			if (mod.ended)
				break;
			mod_tick();
		}
		if ((uint32_t)n > mod.tick_left)
			n = (int)mod.tick_left;
		if (to && mod.channels == 4) {
			mod_mix4(to, n);
			to += 2 * n;
		} else if (to) {
			if (n > MOD_CHUNK)
				n = MOD_CHUNK;
			memset(left, 0, n * sizeof left[0]);
			memset(right, 0, n * sizeof right[0]);
			/* the Amiga's sides: left, right, right, left */
			for (c = 0; c < mod.channels; c++)
				mod_mix(&mod.ch[c], ((c + 1) & 2) ? right : left, n);
			/* a quarter of each side is heard on the other */
			for (i = 0; i < n; i++) {
				int l = (left[i] * 3 + right[i]) >> mod.shift, r = (right[i] * 3 + left[i]) >> mod.shift;

				*to++ = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
				*to++ = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
			}
		}
		mod.tick_left -= (uint32_t)n;
		got += n;
	}
	return got;
}

/* From the start to `frame`, or to the end; where the tune then is. */
static uint32_t
mod_seek(uint32_t frame)
{
	uint32_t at = 0;
	int n;

	mod_reset();
	while (at < frame && (n = mod_read(NULL, frame - at > 1u << 20 ? 1 << 20 : (int)(frame - at))) > 0)
		at += (uint32_t)n;
	return at;
}

/* True if the file is a tune this plays; `rate` is what mod_read() then gives. */
static int
mod_open(const uint8_t *file, uint32_t size, int rate)
{
	const uint8_t *tag = file + 1080, *data;
	int i, patterns = 0;

	if (size < 1084 + 1024)
		return 0;
	memset(&mod, 0, sizeof mod);
	if (!memcmp(tag, "M.K.", 4) || !memcmp(tag, "M!K!", 4) || !memcmp(tag, "FLT4", 4) || !memcmp(tag, "4CHN", 4))
		mod.channels = 4;
	else if (!memcmp(tag + 1, "CHN", 3) && tag[0] >= '1' && tag[0] <= '9')
		mod.channels = tag[0] - '0';
	else if (!memcmp(tag + 2, "CH", 2) && tag[0] >= '1' && tag[0] <= '3' && tag[1] >= '0' && tag[1] <= '9')
		mod.channels = (tag[0] - '0') * 10 + tag[1] - '0';
	if (mod.channels < 1 || mod.channels > MOD_CHANNELS)
		return 0;
	mod.orders = file[950];
	mod.order = file + 952;
	for (i = 0; i < 128; i++)
		if (mod.order[i] > patterns)
			patterns = mod.order[i];
	patterns++;
	mod.patterns = file + 1084;
	data = mod.patterns + (uint32_t)patterns * 64 * mod.channels * 4;
	if (mod.orders < 1 || mod.orders > 128 || data > file + size)
		return 0;
	for (i = 1; i < 32; i++) {
		const uint8_t *h = file + 20 + 30 * (i - 1);
		struct mod_sample *s = &mod.samples[i];

		s->data = (const int8_t *)data;
		s->length = (uint32_t)(h[22] << 8 | h[23]) * 2;
		s->finetune = h[24] & 15;
		s->volume = h[25] > 64 ? 64 : h[25];
		s->loop_at = (uint32_t)(h[26] << 8 | h[27]) * 2;
		s->loop_length = (uint32_t)(h[28] << 8 | h[29]) * 2;
		if (s->length > (uint32_t)(file + size - data))
			s->length = (uint32_t)(file + size - data);	/* a file cut short */
		if (s->loop_length <= 2 || s->loop_at >= s->length)
			s->loop_length = 0;
		else if (s->loop_at + s->loop_length > s->length)
			s->loop_length = s->length - s->loop_at;
		data += s->length;
	}
	mod.rate = rate;
	/* the Amiga's clock over a period is a note's samples a second */
	mod.clock = (uint32_t)(((uint64_t)3546895 << 14) / (uint32_t)rate);
	/* four channels at their loudest are 16 bits; more are scaled down */
	for (mod.shift = 1; (4 << (mod.shift - 1)) < mod.channels; mod.shift++)
		continue;
	mod_reset();
	return 1;
}

const char *
m_mod_open(struct media_audio *a)
{
	struct mod_state *m = calloc(1, sizeof *m);

	if (!m)
		return "not enough memory";
	modp = m;
	if (!mod_open(a->data, a->size, 22050)) {
		free(m);
		return "not a tracker tune";
	}
	a->own = m;
	a->info.rate = 22050;
	a->info.channels = 2;
	a->info.frames = mod_seek(~0u);	/* its length: the sequencer run to its end */
	mod_seek(0);
	return NULL;
}

int
m_mod_rate(struct media_audio *a, int rate)
{
	if (rate < 8000 || rate > 96000)
		return a->info.rate;
	modp = a->own;
	mod_open(a->data, a->size, rate);
	a->info.rate = rate;
	a->info.frames = mod_seek(~0u);
	mod_seek(0);
	a->at = 0;
	return rate;
}

int
m_mod_read(struct media_audio *a, int16_t *to, int frames)
{
	modp = a->own;
	return mod_read(to, frames);
}

uint32_t
m_mod_seek(struct media_audio *a, uint32_t frame)
{
	modp = a->own;
	return mod_seek(frame);
}

/* (no pieces: in order only) */
int
m_mod_decoder(struct media_audio *a, int d)
{
	(void)a, (void)d;
	return 0;
}

uint32_t
m_mod_step(struct media_audio *a, int d, uint32_t piece, int16_t *to, uint32_t done)
{
	(void)a, (void)d, (void)piece, (void)to;
	return done;
}

void
m_mod_close(struct media_audio *a)
{
	free(a->own);
}
