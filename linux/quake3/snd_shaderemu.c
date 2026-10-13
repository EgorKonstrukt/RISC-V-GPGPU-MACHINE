/*
 * Quake III Arena's sound on the machine's sound card (docs/quake3.md, docs/sound.md), in place
 * of the game's mixer. A sound is a voice of the card pointed at its samples where bake.py's
 * file lies in the ROM: nothing is loaded or mixed here. What stays the game's is which of a
 * few channels a sound takes and how loud it is in each ear, worked out once a frame.
 */
#include "client/client.h"
#include "client/snd_local.h"
#include "../userland/shaderemu_sound.h"
#include "se_data.h"
#include "se_q3.h"

#define DYNAMIC		12		/* sounds things make */
#define LOOPS		8		/* voices for sounds that go on (a weapon's hum, a lift): the loudest there are */
#define MAX_ASKED	64		/* such sounds asked for in a frame */
#define FULL_VOLUME	80.0f		/* as near as this a sound is as loud as it gets */
#define ATTENUATE	0.0008f		/* and it fades to nothing over 1,250 units more */
#define GAIN		0.5f		/* of the game's own level: a fight's mix runs into the card's limit */

/* (the cinematics' and the recorder's view of the mixer there is not) */
dma_t		dma;
int		s_soundtime, s_rawend[MAX_RAW_STREAMS];

cvar_t		*s_volume;
static const se_sound_t *sounds;
static int	sound_count, ready, voice_count;
static unsigned	rom;
static int	voices[DYNAMIC + LOOPS];

typedef struct {
	int		sfx, entnum, entchannel, follows;	/* follows: its place is its entity's, wherever that goes */
	vec3_t		origin;
	unsigned	end_ms;
} heard_t;
static heard_t dynamic[DYNAMIC];
static struct { int sfx, entnum, loud; vec3_t origin; } asked[MAX_ASKED];
static int	asked_count;
static struct { int sfx, entnum, loud; } looping[LOOPS];
static vec3_t	entity_at[MAX_GENTITIES], ear, ear_axis[3];
static int	ear_entity;

/* The card's volume word for a sound at a place, as the game's mixer had it; *loud is how loud in all. */
static uint32_t heard (const float *origin, int local, int *loud)
{
	vec3_t	to;
	float	dist, left, right, scale = s_volume->value * GAIN * 256;
	int	l, r;

	if (local) {
		left = right = scale;
	} else {
		VectorSubtract (origin, ear, to);
		dist = VectorNormalize (to) - FULL_VOLUME;
		dist = dist < 0 ? 0 : dist * ATTENUATE;
		scale *= 1.0f - dist;
		left = scale * 0.5f * (1.0f + DotProduct (to, ear_axis[1]));
		right = scale * 0.5f * (1.0f - DotProduct (to, ear_axis[1]));
	}
	l = left < 0 ? 0 : left > 256 ? 256 : (int)left;
	r = right < 0 ? 0 : right > 256 ? 256 : (int)right;
	if (loud)
		*loud = l + r;
	return SND_VOLUME (l, r);
}

static void play (int voice, int sfx, int looped, uint32_t volume)
{
	const se_sound_t *s = &sounds[sfx - 1];

	snd_play (voices[voice], (s->width == 1 ? SND_PCM8 : SND_PCM16) | SND_SMOOTH | (looped ? SND_LOOPED : 0), rom + s->at, s->samples,
		0, snd_step (s->rate), volume);
}

void S_StartSound (vec3_t origin, int entnum, int entchannel, sfxHandle_t sfx)
{
	unsigned	now = (unsigned)Sys_Milliseconds ();
	heard_t	*c, probe;
	uint32_t	volume;
	int		i, pick = -1, loud;

	if (!ready || sfx <= 0 || sfx > sound_count || entnum < 0 || entnum >= MAX_GENTITIES)
		return;
	memset (&probe, 0, sizeof probe);
	probe.sfx = sfx, probe.entnum = entnum, probe.entchannel = entchannel;
	probe.follows = origin == NULL;
	VectorCopy (origin ? origin : entity_at[entnum], probe.origin);
	for (i = 0; i < DYNAMIC; i++) {
		c = &dynamic[i];
		if (c->sfx && (int)(c->end_ms - now) > 0) {
			/* the same sound from the same thing, just begun: once is enough */
			if (c->entnum == entnum && c->sfx == sfx && c->end_ms - now + 50 > sounds[sfx - 1].samples * 1000 / sounds[sfx - 1].rate)
				return;
			/* a thing's channel is taken over (but not the one for any number of sounds) */
			if (c->entnum == entnum && c->entchannel == entchannel && entchannel != CHAN_AUTO) {
				pick = i;
				break;
			}
			if (c->entnum == ear_entity && entnum != ear_entity)
				continue;	/* the listener's own are not cut off by another's */
		}
		if (pick < 0 || (int)(c->end_ms - dynamic[pick].end_ms) < 0)
			pick = i;
	}
	if (pick < 0)
		return;
	volume = heard (probe.origin, entnum == ear_entity, &loud);
	if (!loud)
		return;		/* not heard from here */
	probe.end_ms = now + sounds[sfx - 1].samples * 1000 / sounds[sfx - 1].rate;
	dynamic[pick] = probe;
	play (pick, sfx, 0, volume);
}

void S_StartLocalSound (sfxHandle_t sfx, int channelNum)
{
	S_StartSound (NULL, ear_entity, channelNum, sfx);
}

void S_StopAllSounds (void)
{
	int	i;

	for (i = 0; i < voice_count; i++)
		snd_stop (voices[i]);
	memset (dynamic, 0, sizeof dynamic);
	memset (looping, 0, sizeof looping);
	asked_count = 0;
}

void S_ClearLoopingSounds (qboolean killall)
{
	asked_count = 0;
}

void S_AddLoopingSound (int entityNum, const vec3_t origin, const vec3_t velocity, sfxHandle_t sfx)
{
	if (!ready || sfx <= 0 || sfx > sound_count || asked_count == MAX_ASKED)
		return;
	asked[asked_count].sfx = sfx, asked[asked_count].entnum = entityNum;
	VectorCopy (origin, asked[asked_count].origin);
	asked_count++;
}

void S_AddRealLoopingSound (int entityNum, const vec3_t origin, const vec3_t velocity, sfxHandle_t sfx)
{
	S_AddLoopingSound (entityNum, origin, velocity, sfx);
}

void S_StopLoopingSound (int entityNum)
{
	int	i, n = 0;

	for (i = 0; i < asked_count; i++)
		if (asked[i].entnum != entityNum)
			asked[n++] = asked[i];
	asked_count = n;
}

void S_Respatialize (int entityNum, const vec3_t origin, vec3_t axis[3], int inwater)
{
	ear_entity = entityNum;
	VectorCopy (origin, ear);
	AxisCopy (axis, ear_axis);
}

void S_UpdateEntityPosition (int entityNum, const vec3_t origin)
{
	if (entityNum >= 0 && entityNum < MAX_GENTITIES)
		VectorCopy (origin, entity_at[entityNum]);
}

/* Once a frame: every playing sound as loud as it is from where the listener is now. */
void S_Update (void)
{
	unsigned	now = (unsigned)Sys_Milliseconds ();
	int		i, k, loud, was = SE_Phase (5);

	if (!ready) {
		SE_Phase (was);
		return;
	}
	for (i = 0; i < DYNAMIC; i++) {
		heard_t *c = &dynamic[i];

		if (!c->sfx || (int)(c->end_ms - now) <= 0)
			continue;
		if (c->follows)
			VectorCopy (entity_at[c->entnum], c->origin);
		SND_VOICE[voices[i]].volume = heard (c->origin, c->entnum == ear_entity, NULL);
	}
	/* the sounds that go on: one still asked for keeps its voice, one no more asked for gives it up */
	for (k = 0; k < LOOPS; k++) {
		if (!looping[k].sfx)
			continue;
		for (i = 0; i < asked_count; i++)
			if (asked[i].sfx == looping[k].sfx && asked[i].entnum == looping[k].entnum)
				break;
		if (i < asked_count) {
			SND_VOICE[voices[DYNAMIC + k]].volume = heard (asked[i].origin, 0, &looping[k].loud);
			asked[i].sfx = 0;	/* (has its voice) */
			if (looping[k].loud)
				continue;
		}
		snd_stop (voices[DYNAMIC + k]);
		looping[k].sfx = 0;
	}
	/* and one newly asked for takes a free voice, or a fainter one's */
	for (i = 0; i < asked_count; i++) {
		uint32_t volume;
		int	take = -1;

		if (!asked[i].sfx)
			continue;
		volume = heard (asked[i].origin, 0, &loud);
		if (!loud)
			continue;
		for (k = 0; k < LOOPS && DYNAMIC + k < voice_count; k++)
			if (!looping[k].sfx) {
				take = k;
				break;
			} else if (looping[k].loud < loud && (take < 0 || looping[k].loud < looping[take].loud)) {
				take = k;
			}
		if (take < 0)
			continue;
		looping[take].sfx = asked[i].sfx, looping[take].entnum = asked[i].entnum, looping[take].loud = loud;
		play (DYNAMIC + take, asked[i].sfx, 1, volume);
	}
	SE_Phase (was);
}

sfxHandle_t S_RegisterSound (const char *sample, qboolean compressed)
{
	char	name[64];
	int	i, low = 0, high = sound_count - 1;

	SE_Wanted ("sound", sample);
	for (i = 0; i < 63 && sample[i]; i++)
		name[i] = sample[i] == '\\' ? '/' : sample[i] >= 'A' && sample[i] <= 'Z' ? sample[i] + 32 : sample[i];
	name[i] = 0;
	while (low <= high) {
		int mid = (low + high) >> 1, c = strcmp (name, sounds[mid].name);

		if (!c)
			return mid + 1;
		if (c < 0)
			high = mid - 1;
		else
			low = mid + 1;
	}
	return 0;
}

void S_Init (void)
{
	s_volume = Cvar_Get ("s_volume", "0.8", CVAR_ARCHIVE);
	sounds = SE_DataSection ("SNDS", sizeof (se_sound_t), &sound_count);
	SE_Data (&rom);
	if (!sounds || !rom || snd_open ()) {
		Com_Printf ("No sound card\n");
		return;
	}
	while (voice_count < DYNAMIC + LOOPS && (voices[voice_count] = snd_claim ()) >= 0)
		voice_count++;
	ready = voice_count >= DYNAMIC;
	Com_Printf ("Sound: %d voices of the machine's card, %d sounds in the ROM\n", voice_count, sound_count);
}

void S_Shutdown (void)
{
	if (ready)
		S_StopAllSounds ();
}

void S_StartBackgroundTrack (const char *intro, const char *loop) {}
void S_StopBackgroundTrack (void) {}
void S_RawSamples (int stream, int samples, int rate, int width, int channels, const byte *data, float volume, int entityNum) {}
void S_DisableSounds (void) { S_StopAllSounds (); }
void S_BeginRegistration (void) {}
void S_DisplayFreeMemory (void) {}
void S_ClearSoundBuffer (void) {}
void SNDDMA_Activate (void) {}
void S_UpdateBackgroundTrack (void) {}
