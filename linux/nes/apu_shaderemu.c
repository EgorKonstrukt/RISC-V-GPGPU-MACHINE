/* The NES's sound on the machine's sound card (docs/games/nes.md, "Sound"): each of its five
 * channels is a voice playing a wave that is in sound memory already, and this file only
 * says how fast and how loud; a worker decodes a DMC recording the first time it is played.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shaderemu_sound.h"
#include "mcwx.h"

extern uint8_t nes6502_getbyte(uint32_t address);

#define CPU_HZ 1789773u
#define WAVE 256		/* samples of a pulse's turn */
#define NOISE_LONG 32767
#define NOISE_SHORT 93
#define DMC_SLOTS 6
#define DMC_MOST (4081 * 8)	/* samples of the longest recording */

/* loudness, as the NES mixes (its linear approximation), 1.2 of it to full scale */
#define PULSE_PEAK 2227		/* of 32768, at volume 15 (a voice's volume is 17 a step) */
#define NOISE_PEAK 1463
#define TRIANGLE_STEP 167	/* a level of its 16, twice over: (2 * level - 15) of these */
#define DMC_VOLUME 132		/* a level of its 127 is one of a byte's */

enum { P1, P2, TRI, NOISE, DMC, CHANNELS };

struct envelope {
	int start, divider, decay;
};

static struct {
	uint8_t reg[4];
	int period, length, enabled;
	struct envelope env;
	int sweep_divider, sweep_reload;
	int linear, linear_reload;	/* the triangle's */
} ch[4];

static struct {
	uint8_t reg[4];
	int level, left, playing;	/* left: quarters of a frame until it has played out */
	int waiting;			/* the slot a worker is decoding, to be played when it has: -1 none */
} dmc = { .level = 0, .waiting = -1 };

/* A recording decoded: where from, and its samples in sound memory. */
static struct slot {
	int address, bytes, level, used;
	uint8_t head[8];
	uint8_t source[4096];
	uint8_t *samples;
	uint32_t at;
} *slots;

static const uint8_t lengths[32] = { 10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
				     12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };
static const uint16_t noise_periods[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
static const uint16_t dmc_periods[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };

int se_apu_due = 65;		/* the scanline the next quarter of a frame is at (nes.c asks) */
static int quarter, five_step, sequence;
static int on, worker, print, test;	/* worker: calls may go to workers (mcwx.h) */
/* the call out: the long noise, or a recording (the first core's: 16 bytes of its own) */
static struct { mcw_handle call; int pad[3]; } job __attribute__((aligned(16)));
void se_apu_look(void);
static int voices[CHANNELS];
static uint32_t base;		/* CPU cycles an output sample, 16.16 */
static uint32_t wave_at[4], triangle_at, noise_at[2];
static struct { uint32_t address, step, volume; int started; } shown[CHANNELS];
static unsigned frames;

/* ---- the card ---- */

/* A voice as it should sound now: started again only when it plays something else. */
static void
voice(int c, uint32_t kind, uint32_t address, uint32_t length, uint32_t step, uint32_t volume)
{
	volatile snd_voice *v;

	if (!on)
		return;
	v = &SND_VOICE[voices[c]];
	if (!shown[c].started || shown[c].address != address) {
		snd_play(voices[c], kind, address, length, 0, step, volume);
		shown[c].started = 1;
		shown[c].address = address;
	} else {
		if (shown[c].step != step)
			v->step = step;
		if (shown[c].volume != volume)
			v->volume = volume;
	}
	shown[c].step = step;
	shown[c].volume = volume;
}

static int
sweep_target(int c)
{
	int change = ch[c].period >> (ch[c].reg[1] & 7);

	return ch[c].reg[1] & 8 ? ch[c].period - change - (c == P1) : ch[c].period + change;
}

static int
level_of(int c)
{
	return ch[c].reg[0] & 0x10 ? ch[c].reg[0] & 15 : ch[c].env.decay;
}

/* What a channel was last shown as (seen): four times a frame it is the same, and nothing is worked out. */
static uint32_t seen[CHANNELS];

static int
as_seen(int c, uint32_t now)
{
	if (seen[c] == (now | 0x80000000))
		return 1;
	seen[c] = now | 0x80000000;
	return 0;
}

static void
show_pulse(int c)
{
	int level = ch[c].length && ch[c].period >= 8 && sweep_target(c) <= 0x7ff ? level_of(c) : 0;

	if (as_seen(c, (uint32_t)ch[c].period | (uint32_t)level << 12 | (uint32_t)(ch[c].reg[0] >> 6) << 16))
		return;
	voice(c, SND_PCM16 | SND_LOOPED, wave_at[ch[c].reg[0] >> 6], WAVE, base / (unsigned)(ch[c].period + 1) * (WAVE / 16),
	      SND_VOLUME(level * 17, level * 17));
}

/* The triangle stands where it is when it is told to stop, as the NES's does: a step of 0. */
static void
show_triangle(void)
{
	int runs = ch[TRI].length && ch[TRI].linear && ch[TRI].period >= 2;

	if (as_seen(TRI, (uint32_t)ch[TRI].period | (uint32_t)runs << 12))
		return;
	voice(TRI, SND_PCM16 | SND_LOOPED, triangle_at, 32, runs ? base / (unsigned)(ch[TRI].period + 1) : 0, SND_VOLUME(256, 256));
}

static void
show_noise(void)
{
	int level = ch[NOISE].length ? level_of(NOISE) : 0, brief = ch[NOISE].reg[2] >> 7;

	if (as_seen(NOISE, (uint32_t)ch[NOISE].reg[2] | (uint32_t)level << 12))
		return;
	voice(NOISE, SND_PCM16 | SND_LOOPED, noise_at[brief], brief ? NOISE_SHORT : NOISE_LONG, base / noise_periods[ch[NOISE].reg[2] & 15],
	      SND_VOLUME(level * 17, level * 17));
}

static void
show(int c)
{
	if (!on)
		return;		/* (no card: nothing to tell it; the channels are followed all the same) */
	if (c <= P2)
		show_pulse(c);
	else if (c == TRI)
		show_triangle();
	else
		show_noise();
}

/* ---- DMC: a recording of one bit a sample, decoded once and then a voice like any ---- */

/* A worker's job, or this core's: the slot's bytes into its samples. Returns how many. */
static uint32_t
dmc_decode(uint32_t which, uint32_t unused)
{
	struct slot *s = &slots[which];
	int level = s->level, i, bit;
	uint8_t *to = s->samples;

	for (i = 0; i < s->bytes; i++)
		for (bit = 0; bit < 8; bit++) {
			if (s->source[i] >> bit & 1)
				level += level <= 125 ? 2 : 0;
			else
				level -= level >= 2 ? 2 : 0;
			*to++ = (uint8_t)(128 + level - 64);
		}
	return (uint32_t)(s->bytes * 8);
}

static void
dmc_play(int which)
{
	struct slot *s = &slots[which];
	unsigned period = dmc_periods[dmc.reg[0] & 15], samples = (unsigned)s->bytes * 8;

	dmc.left = dmc.reg[0] & 0x40 ? 1 << 30 : (int)((samples * period + CPU_HZ / 240 - 1) / (CPU_HZ / 240));
	dmc.playing = 1;
	if (!on)
		return;
	shown[DMC].started = 1;
	snd_play(voices[DMC], SND_PCM8 | (dmc.reg[0] & 0x40 ? SND_LOOPED : 0), s->at, samples, 0, base / period,
		 SND_VOLUME(DMC_VOLUME, DMC_VOLUME));
}

static void
dmc_stop(void)
{
	dmc.playing = dmc.left = 0;
	dmc.waiting = -1;
	if (on && shown[DMC].started)
		snd_stop(voices[DMC]);
}

/* The recording the registers name: one decoded before, or a slot for it and its decoding. */
static void
dmc_start(void)
{
	static int next;
	int address = 0xc000 + dmc.reg[2] * 64, bytes = dmc.reg[3] * 16 + 1, i, k;
	uint8_t head[8];
	struct slot *s;

	if (!slots)
		return;
	for (i = 0; i < 8; i++)
		head[i] = nes6502_getbyte(0x8000 | ((address + i) & 0x7fff));
	for (k = 0; k < DMC_SLOTS; k++)
		if (slots[k].used && slots[k].address == address && slots[k].bytes == bytes && slots[k].level == dmc.level &&
		    !memcmp(slots[k].head, head, 8) && k != dmc.waiting) {
			dmc_play(k);
			return;
		}
	if (dmc.waiting >= 0)
		return;		/* one is being decoded: this one is not heard */
	k = next++ % DMC_SLOTS;
	s = &slots[k];
	s->address = address, s->bytes = bytes, s->level = dmc.level, s->used = 1;
	memcpy(s->head, head, 8);
	for (i = 0; i < bytes; i++)
		s->source[i] = nes6502_getbyte(0x8000 | ((address + i) & 0x7fff));
	/* (on the worker that emulates it is decoded there and then) */
	if (worker && !job.call && !mcw_on_worker()) {
		job.call = mcw_call(dmc_decode, (uint32_t)k, 0);
		dmc.waiting = k;
	} else {
		dmc_decode((uint32_t)k, 0);
		dmc_play(k);
	}
}

/* ---- what the NES's own frame counter does, a quarter of a frame at a time ---- */

static void
envelope(int c)
{
	struct envelope *e = &ch[c].env;

	if (e->start) {
		e->start = 0;
		e->decay = 15;
		e->divider = ch[c].reg[0] & 15;
	} else if (e->divider == 0) {
		e->divider = ch[c].reg[0] & 15;
		if (e->decay)
			e->decay--;
		else if (ch[c].reg[0] & 0x20)
			e->decay = 15;
	} else
		e->divider--;
}

static void
quarter_frame(void)
{
	envelope(P1), envelope(P2), envelope(NOISE);
	if (ch[TRI].linear_reload)
		ch[TRI].linear = ch[TRI].reg[0] & 0x7f;
	else if (ch[TRI].linear)
		ch[TRI].linear--;
	if (!(ch[TRI].reg[0] & 0x80))
		ch[TRI].linear_reload = 0;
}

static void
half_frame(void)
{
	int c;

	for (c = P1; c <= NOISE; c++)
		if (ch[c].length && !(ch[c].reg[0] & (c == TRI ? 0x80 : 0x20)))
			ch[c].length--;
	for (c = P1; c <= P2; c++) {
		int period = (ch[c].reg[1] >> 4) & 7;

		if (ch[c].sweep_divider == 0 && ch[c].reg[1] & 0x80 && ch[c].reg[1] & 7 && ch[c].period >= 8 && sweep_target(c) <= 0x7ff)
			ch[c].period = sweep_target(c) < 0 ? 0 : sweep_target(c);
		if (ch[c].sweep_divider == 0 || ch[c].sweep_reload) {
			ch[c].sweep_divider = period;
			ch[c].sweep_reload = 0;
		} else
			ch[c].sweep_divider--;
	}
}

static void
step(void)
{
	int c, at = sequence++ % (five_step ? 5 : 4);

	if (!five_step || at != 3)
		quarter_frame();
	if (five_step ? at == 1 || at == 4 : at & 1)
		half_frame();
	for (c = P1; c <= NOISE; c++)
		show(c);
	se_apu_look();
	if (dmc.waiting >= 0 && !job.call) {
		c = dmc.waiting;
		dmc.waiting = -1;
		dmc_play(c);
	} else if (dmc.playing && --dmc.left <= 0)
		dmc.playing = 0;
}

/* The call out joined when it is made (asked also so that a worker stopped at a page is
 * helped on); by the first core only. */
void
se_apu_look(void)
{
	if (job.call && !mcw_on_worker() && mcw_ready(job.call)) {
		mcw_join(job.call);
		job.call = 0;
	}
}

/* nes.c calls this when the picture's line has reached se_apu_due. */
void
se_apu_quarter(void)
{
	static const int lines[4] = { 65, 130, 196, 261 };

	step();
	se_apu_due = ++quarter < 4 ? lines[quarter] : 1000;
}

void se_apu_write(uint32_t address, uint8_t value);

/* NES_APU_TEST=1: neither game of the image has a recording, so one is played from the
 * cartridge's first bytes at frame 30, and at frame 60 what the worker made of it is
 * compared with this core's decoding. */
static void
self_test(void)
{
	static uint8_t mine[DMC_MOST];
	uint8_t *theirs;
	int k, n;

	if (frames == 30) {
		se_apu_write(0x4010, 0x0f);
		se_apu_write(0x4012, 0);
		se_apu_write(0x4013, 0x40);
		se_apu_write(0x4015, 0x1f);
		printf("nesapu: test: a recording asked for, %s\n", dmc.waiting >= 0 ? "a worker decodes it" : "decoded here");
	} else if (frames == 60) {
		for (k = 0; k < DMC_SLOTS && !slots[k].used; k++)
			continue;
		if (k == DMC_SLOTS)
			return;
		theirs = slots[k].samples;
		slots[k].samples = mine;
		n = (int)dmc_decode((uint32_t)k, 0);
		slots[k].samples = theirs;
		printf("nesapu: test: %d samples, %s this core's; the card's voice %s\n", n, memcmp(mine, theirs, (size_t)n) ? "NOT" : "the same as",
		       on ? (SND_STATE[voices[DMC]].state & 1 ? "plays" : "has ended") : "is not there");
	}
}

/* A frame of the NES has ended. */
void
se_apu_frame(void)
{
	while (quarter < 4)
		se_apu_quarter();
	quarter = 0;
	se_apu_due = 65;
	frames++;
	if (test)
		self_test();
	if (print)
		printf("nesapu: %u p1 %d %d %d p2 %d %d %d tri %d %d noise %d %d %d dmc %d\n", frames, ch[P1].period,
		       ch[P1].length && ch[P1].period >= 8 && sweep_target(P1) <= 0x7ff ? level_of(P1) : 0, ch[P1].reg[0] >> 6, ch[P2].period,
		       ch[P2].length && ch[P2].period >= 8 && sweep_target(P2) <= 0x7ff ? level_of(P2) : 0, ch[P2].reg[0] >> 6, ch[TRI].period,
		       ch[TRI].length && ch[TRI].linear && ch[TRI].period >= 2, ch[NOISE].reg[2] & 15, ch[NOISE].length ? level_of(NOISE) : 0,
		       ch[NOISE].reg[2] >> 7, dmc.playing);
}

/* ---- the registers, $4000 to $4017 (nofrendo's own handlers call this first) ---- */

void
se_apu_write(uint32_t address, uint8_t value)
{
	int c = (int)(address >> 2) & 7, r = (int)address & 3;

	if (address < 0x4010) {
		ch[c].reg[r] = value;
		if (r == 1 && c <= P2)
			ch[c].sweep_reload = 1;
		if (r == 2 && c != NOISE)
			ch[c].period = (ch[c].period & 0x700) | value;
		if (r == 3) {
			if (c != NOISE)
				ch[c].period = (ch[c].period & 0xff) | (value & 7) << 8;
			if (ch[c].enabled)
				ch[c].length = lengths[value >> 3];
			ch[c].env.start = 1;
			ch[c].linear_reload = 1;
		}
		show(c);
	} else if (address < 0x4014) {
		dmc.reg[r] = value;
		if (r == 1)
			dmc.level = value & 0x7f;
	} else if (address == 0x4015) {
		for (c = P1; c <= NOISE; c++) {
			ch[c].enabled = value >> c & 1;
			if (!ch[c].enabled)
				ch[c].length = 0;
			show(c);
		}
		if (!(value & 0x10))
			dmc_stop();
		else if (!dmc.playing && dmc.waiting < 0)
			dmc_start();
	} else if (address == 0x4017) {
		five_step = value >> 7;
		sequence = 0;
		if (five_step) {
			quarter_frame();
			half_frame();
		}
	}
}

/* ---- the start: the voices, and the waves they play ---- */

static uint32_t
noise_fill(uint32_t to, uint32_t brief)
{
	int16_t *samples = (int16_t *)to;
	unsigned lfsr = 1;
	int i, count = brief ? NOISE_SHORT : NOISE_LONG;

	for (i = 0; i < count; i++) {
		samples[i] = lfsr & 1 ? -NOISE_PEAK : NOISE_PEAK;
		lfsr = lfsr >> 1 | ((lfsr ^ lfsr >> (brief ? 6 : 1)) & 1) << 14;
	}
	return 0;
}

/* `workers`: the program has workers to give calls to. */
void
se_apu_init(int workers)
{
	static const uint8_t duty[4] = { 0x40, 0x60, 0x78, 0x9f };	/* a turn in eight parts, the first bit first */
	const char *sound = getenv("NES_SOUND");
	uint32_t bytes = 4 * WAVE * 2 + 64 + (NOISE_LONG + 1) * 2 + 256 + DMC_SLOTS * 32768u, at;
	int16_t *memory;
	int c, i;

	print = getenv("NES_APU_PRINT") != NULL;
	test = getenv("NES_APU_TEST") != NULL;
	slots = calloc(DMC_SLOTS, sizeof *slots);
	worker = workers;
	if ((sound && !strcmp(sound, "0")) || snd_open())
		goto none;
	for (i = 0; i < 50 && !SND_RATE; i++)
		snd_wait();	/* (the card says its rate once the host has mixed) */
	if (!SND_RATE)
		goto none;
	for (c = 0; c < CHANNELS; c++)
		if ((voices[c] = snd_claim()) < 0)
			goto none;
	if (!(memory = snd_memory(bytes, &at)))
		goto none;
	memset(memory, 0, bytes);	/* (its pages are here before a worker writes them) */
	base = (uint32_t)(((uint64_t)CPU_HZ << 16) / SND_RATE);
	for (c = 0; c < 4; c++) {
		wave_at[c] = at + (uint32_t)c * WAVE * 2;
		for (i = 0; i < WAVE; i++)
			memory[c * WAVE + i] = duty[c] >> (7 - i * 8 / WAVE) & 1 ? PULSE_PEAK : -PULSE_PEAK;
	}
	triangle_at = at + 4 * WAVE * 2;
	for (i = 0; i < 32; i++)
		memory[4 * WAVE + i] = (int16_t)((2 * (i < 16 ? 15 - i : i - 16) - 15) * TRIANGLE_STEP);
	noise_at[1] = triangle_at + 64;
	noise_at[0] = noise_at[1] + 256;
	noise_fill((uint32_t)(memory + 4 * WAVE + 32), 1);
	/* 32,767 samples of the long noise: a worker's, while the cartridge starts */
	if (worker)
		job.call = mcw_call(noise_fill, (uint32_t)(memory + 4 * WAVE + 32 + 128), 0);
	else
		noise_fill((uint32_t)(memory + 4 * WAVE + 32 + 128), 0);
	for (i = 0; i < DMC_SLOTS; i++) {
		slots[i].samples = (uint8_t *)memory + 4 * WAVE * 2 + 64 + 256 + (NOISE_LONG + 1) * 2 + i * 32768;
		slots[i].at = at + (uint32_t)(slots[i].samples - (uint8_t *)memory);
	}
	memset(seen, 0, sizeof seen);
	on = 1;
	fprintf(stderr, "nes: sound on the card's voices %d to %d%s\n", voices[0], voices[CHANNELS - 1],
		worker ? ", recordings decoded by a worker core" : "");
	return;
none:
	/* no card: the channels are still followed, so that a run is the same with and without */
	for (i = 0; i < DMC_SLOTS; i++)
		slots[i].samples = malloc(DMC_MOST);
	worker = 0;
}
