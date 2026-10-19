/*
snd_shaderemu.c - Half-Life's sound on the machine's sound card (docs/machine/sound.md), in place of the
engine's mixer and its DMA backend. A sound is a voice pointed at its WAV where pak0 lies in the
ROM, or at its MP3 decoded into RAM (linux/halflife/repack.py); the engine keeps its channels,
their volumes and positions (s_mix.c, S_PaintChannels).
*/
#include "common.h"
#include "sound.h"
#include "client.h"
#include "../userland/shaderemu_sound.h"
#include "media.h"
#include "mcwx.h"

#define WF_CARD 0x43	// a wavdata_t of ours: no samples, buffer holds the ROM address and pak0 offset
#define VOICES 30
#define MP3_DELAY 1105	// LAME's samples before the sound and the decoder's (repack.py writes no Xing frame)
#define RAM_SOUND 0xffffffffu	// where[1] of a sound decoded into RAM

int SE_MediaWorkers( void );
typedef struct { struct media_audio *audio; byte *mp3; int16_t *to; uint32_t frames; } queued_t;
static queued_t queue[2048];
static int queued, queueing;

static int ready, pak_fd = -1, voice_count;
static uint32_t pak_rom, last_clock, time_base;
static int voices[VOICES], voice_owner[VOICES];	// a channel's index, or -1
static struct { const void *sfx; double sample; int voice, touched; } shadow[MAX_CHANNELS];
static struct pak_entry { char name[56]; uint32_t at, size; } *pak;
static int pak_count;

static int entry_compare( const void *a, const void *b )
{
	return Q_strcmp((( const struct pak_entry *)a )->name, (( const struct pak_entry *)b )->name );
}

static void pak_open( void )
{
	struct { char id[4]; int at, length; } head;
	struct { char name[56]; int at, size; } *e;

	if(( pak_fd = open( "valve/pak0.PAK", O_RDONLY | O_CLOEXEC )) < 0 || pread( pak_fd, &head, 12, 0 ) != 12 || memcmp( head.id, "PACK", 4 ))
		return;
	pak_rom = snd_rom( pak_fd, "PACK", 4 );
	pak = Mem_Malloc( host.mempool, head.length / 64 * sizeof( *pak ));
	e = Mem_Malloc( host.mempool, head.length );
	int n = pread( pak_fd, e, head.length, head.at ) / 64;
	for( int i = 0; i < n; i++ )
	{
		if( Q_strnicmp( e[i].name, "sound/", 6 ))
			continue;
		Q_strncpy( pak[pak_count].name, e[i].name + 6, sizeof( pak->name ));
		Q_strnlwr( pak[pak_count].name, pak[pak_count].name, sizeof( pak->name ));
		pak[pak_count].at = e[i].at, pak[pak_count].size = e[i].size;
		pak_count++;
	}
	Mem_Free( e );
	qsort( pak, pak_count, sizeof( *pak ), entry_compare );
}

static uint32_t word_at( const byte *p ) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// An MP3 sound: 16-bit samples in RAM after its wavdata_t, decoded now or with the others queued
static wavdata_t *mp3_sound( const char *name, const struct pak_entry *e, uint32_t data, uint32_t bytes, uint32_t samples,
	int rate, uint32_t loop_start, uint32_t loop_samples, int looped )
{
	struct media_audio_info info;
	struct media_audio *audio;
	byte *file = malloc( bytes );
	const char *why = NULL;

	if( !file || pread( pak_fd, file, bytes, e->at + data ) != (int)bytes || !( audio = media_audio_open( file, bytes, &why )))
	{
		Con_DPrintf( S_ERROR "%s: %s\n", name, why ? why : "not read" );
		free( file );
		return NULL;
	}
	media_audio_get_info( audio, &info );
	if( info.channels != 1 || info.frames <= MP3_DELAY )
	{
		media_audio_close( audio ), free( file );
		return NULL;
	}
	wavdata_t *sc = Mem_Calloc( sndpool, sizeof( *sc ) + 12 + 16 + info.frames * 2 );
	int16_t *to = (int16_t *)((( uintptr_t )( sc->buffer + 12 ) + 15 ) & ~(uintptr_t)15 );
	sc->samples = Q_min( samples, info.frames - MP3_DELAY );
	if( loop_samples && loop_start + loop_samples <= sc->samples )
		sc->samples = loop_start + loop_samples;
	if( looped && loop_start < sc->samples )
		sc->loop_start = loop_start, SetBits( sc->flags, SOUND_LOOPED );
	sc->type = WF_CARD;
	sc->rate = rate, sc->width = 2, sc->channels = 1;
	uint32_t where[3] = { (uint32_t)(uintptr_t)( to + MP3_DELAY ), RAM_SOUND, 0 };
	memcpy( sc->buffer, where, sizeof( where ));
	if( queueing && queued < 2048 )
	{
		queue[queued++] = (queued_t){ audio, file, to, info.frames };
		return sc;
	}
	media_audio_decode_all( audio, to, info.frames, SE_MediaWorkers( ));
	media_audio_close( audio ), free( file );
	return sc;
}

void SE_QueueSounds( qboolean on ) { queueing = on; }

static int longer_first( const void *a, const void *b ) { return (int)(( const queued_t *)b )->frames - (int)(( const queued_t *)a )->frames; }
static void decode_job( void *ctx, uint32_t from, uint32_t to )
{
	for( uint32_t i = from; i < to; i++ )
		media_audio_piece( queue[i].audio, 0, 0, queue[i].to );
}

// The sounds a level registered, decoded together: a whole sound a core, the longest first
void SE_DecodeQueued( void )
{
	mcw_for_opts o = { .grain = 1 };

	queueing = 0;
	if( !queued )
		return;
	qsort( queue, queued, sizeof( *queue ), longer_first );
	for( int i = 0; i < queued; i++ )
		media_audio_decoders( queue[i].audio, 1 ), media_audio_set_piece( queue[i].audio, queue[i].frames );
	o.workers = SE_MediaWorkers( );
	double began = Sys_DoubleTime( ), seconds = 0;
	mcw_for_opt( queued, decode_job, NULL, &o );
	mcw_for_end( );
	for( int i = 0; i < queued; i++ )
		media_audio_close( queue[i].audio ), free( queue[i].mp3 ), seconds += queue[i].frames / 11025.0;
	Con_Printf( "sounds: %d MP3 decoded on %d workers, %.0f s of 11 kHz audio, in %.2f s\n", queued, o.workers, seconds, Sys_DoubleTime( ) - began );
	queued = 0;
}

// A sound's rate, width, loop and samples from its WAV's chunks, as snd_wav.c reads them
wavdata_t *SE_LoadSound( const char *name )
{
	struct pak_entry key, *e;
	byte b[48], head[256];
	int have;
	uint32_t pos, data = 0, bytes = 0, loop_start = 0, loop_samples = 0, fact = 0, ckpt = 0;
	int channels = 0, width = 0, rate = 0, looped = 0, mp3 = 0, adpcm = 0;

	if( !pak_rom )
		return NULL;
	Q_strnlwr( name, key.name, sizeof( key.name ));
	COM_FixSlashes( key.name );
	if( !*COM_FileExtension( key.name ))
		Q_strncat( key.name, ".wav", sizeof( key.name ));	// (a sentence's words have none)
	if( !( e = bsearch( &key, pak, pak_count, sizeof( *pak ), entry_compare )) || e->size < 44 )
		return NULL;
	// (the first chunks from one read, a chunk past them by a read of its own)
	if(( have = pread( pak_fd, head, Q_min( sizeof( head ), e->size ), e->at )) < 12 || memcmp( head, "RIFF", 4 ) || memcmp( head + 8, "WAVE", 4 ))
		return NULL;
	memcpy( b, head, 12 );
	for( pos = 12; pos + 8 <= e->size; pos += 8 + (( word_at( b + 4 ) + 1 ) & ~1u ))
	{
		memset( b, 0, sizeof( b ));
		if( pos + sizeof( b ) <= (uint32_t)have )
			memcpy( b, head + pos, sizeof( b ));
		else if( pread( pak_fd, b, sizeof( b ), e->at + pos ) < 8 )
			break;
		if( !memcmp( b, "fmt ", 4 ) && ( b[8] == 1 || b[8] == 0x55 || b[8] == 0x11 ) && !b[9] )
			channels = b[10], rate = word_at( b + 12 ), width = b[22] / 8, mp3 = b[8] == 0x55, adpcm = b[8] == 0x11;
		else if( !memcmp( b, "ckpt", 4 ))
			ckpt = pos + 8;
		else if( !memcmp( b, "fact", 4 ))
			fact = word_at( b + 8 );
		else if( !memcmp( b, "cue ", 4 ))
			loop_start = word_at( b + 32 ), looped = 1;
		else if( !memcmp( b, "LIST", 4 ) && looped && !memcmp( b + 28, "mark", 4 ))
			loop_samples = word_at( b + 24 );
		else if( !memcmp( b, "data", 4 ))
			data = pos + 8, bytes = word_at( b + 4 );
	}
	if( adpcm && channels == 1 && ckpt && fact && !(( e->at + ckpt ) & 3 ))
		width = 1, bytes = fact;	// (the card's ADPCM, repack.py's: fact is its samples)
	if( !data || ( channels != 1 && channels != 2 ) || ( width != 1 && width != 2 ) || !rate )
		return NULL;
	if( mp3 )
		return mp3_sound( name, e, data, bytes, fact, rate, loop_start, loop_samples, looped );
	wavdata_t *sc = Mem_Calloc( sndpool, sizeof( *sc ) + 12 );
	sc->samples = bytes / width / channels;
	if( loop_samples && loop_start + loop_samples <= sc->samples )
		sc->samples = loop_start + loop_samples;
	if( looped && loop_start < sc->samples )
		sc->loop_start = loop_start, SetBits( sc->flags, SOUND_LOOPED );
	sc->type = WF_CARD;
	sc->rate = rate, sc->width = width, sc->channels = channels;
	// the samples in the ROM and in pak0; an ADPCM sound's checkpoints in the ROM, and pak0 for its mouth
	uint32_t where[3] = { pak_rom + e->at + data, e->at + data, 0 };
	if( adpcm )
		where[1] = e->at + ckpt, where[2] = pak_rom + e->at + ckpt;
	memcpy( sc->buffer, where, sizeof( where ));
	return sc->samples ? sc : ( Mem_Free( sc ), NULL );
}

static void stop( int v )
{
	if( voice_owner[v] >= 0 )
		shadow[voice_owner[v]].voice = -1;
	voice_owner[v] = -1;
	snd_stop( voices[v] );
}

// A channel heard this paint: its voice started where the channel is, or kept with its volume and pitch
void SE_CardChannel( int i, channel_t *ch, const wavdata_t *sc, double pitch )
{
	uint32_t address, where[3], loud = ch->leftvol + ch->rightvol;
	int v = shadow[i].voice, looped = FBitSet( sc->flags, SOUND_LOOPED ) && FBitSet( ch->flags, FL_CHAN_USE_LOOP );
	float gain = S_GetMasterVolume( ) * 256 / 255;

	if( !ready || sc->type != WF_CARD )
		return;
	shadow[i].touched = 1;
	if( v >= 0 && ( shadow[i].sfx != ch->sfx || ch->sample < shadow[i].sample - 1 ))
		stop( v ), v = -1;	// started again or the next word
	if( v < 0 )
	{
		int quiet = -1;
		for( v = 0; v < voice_count && voice_owner[v] >= 0; v++ )
			if( quiet < 0 || snd.channels[voice_owner[v]].leftvol + snd.channels[voice_owner[v]].rightvol <
					snd.channels[voice_owner[quiet]].leftvol + snd.channels[voice_owner[quiet]].rightvol )
				quiet = v;
		if( v == voice_count )
		{
			if( quiet < 0 || snd.channels[voice_owner[quiet]].leftvol + snd.channels[voice_owner[quiet]].rightvol >= loud )
				return;
			stop( v = quiet );
		}
		memcpy( where, sc->buffer, sizeof( where ));
		uint32_t at = (uint32_t)ch->sample, end = sc->samples, loop = 0, frame = sc->width * sc->channels, aux = where[2];
		if( ch->forced_end && !looped && (uint32_t)ch->forced_end < end )
			end = (uint32_t)ch->forced_end;
		if( looped && at > sc->loop_start )
			at = sc->loop_start;	// (a voice starts at its address: the loop's start then)
		else if( looped )
			loop = sc->loop_start - at;
		if( aux )	// (ADPCM from a checkpoint: a whole 32 samples in)
			loop += at & 31, at &= ~31u, address = where[0] + at / 2, aux += at / 8;
		else
			address = where[0] + at * frame;
		if( at >= end || ( sc->width == 2 && ( address & 1 )))
			return;
		snd_play_aux( voices[v], ( aux ? SND_ADPCM : sc->width == 1 ? SND_PCM8 : SND_PCM16 ) | SND_SMOOTH | ( sc->channels == 2 ? SND_STEREO : 0 ) |
			( looped ? SND_LOOPED : 0 ), address, end - at, loop, 0, 0, aux );
		voice_owner[v] = i, shadow[i].voice = v, shadow[i].sfx = ch->sfx;
	}
	SND_VOICE[voices[v]].step = SND_RATE ? (uint32_t)( pitch * sc->rate * 65536.0 / SND_RATE ) : 0;
	SND_VOICE[voices[v]].volume = SND_VOLUME((int)( bound( 0, ch->leftvol, 255 ) * gain ), (int)( bound( 0, ch->rightvol, 255 ) * gain ));
}

// After a channel moved on: where it is, and its speaker's mouth from the samples it passed
void SE_CardMoved( int i, channel_t *ch, const wavdata_t *sc, mouth_t *mouth, double from )
{
	byte b[512];
	uint32_t where[3];
	int n, count, scount = mouth ? mouth->sndcount : 0, savg = 0;

	shadow[i].sample = ch->sample;
	if( !mouth || sc->type != WF_CARD || sc->channels != 1 )
		return;
	memcpy( where, sc->buffer, sizeof( where ));
	if( where[2] )	// ADPCM: the checkpoints' values, one every 32 samples
	{
		uint32_t first = (uint32_t)from / 32, words = Q_min(( ch->sample > from ? (uint32_t)( ch->sample - from ) / 32 + 1 : 0 ), sizeof( b ) / 4 );
		if(( n = pread( pak_fd, b, words * 4, where[1] + first * 4 )) < 4 )
			return;
		for( int k = 0; k < n / 4 && scount < 10; scount++ )
		{
			int data = (int16_t)( b[k * 4] | b[k * 4 + 1] << 8 ) >> 8;
			savg += abs( data );
			k += ( 80 + ((byte)data & 0x1F )) / 32 + 1;
		}
		mouth->sndavg += savg, mouth->sndcount = scount;
		if( mouth->sndcount >= 10 )
			mouth->mouthopen = mouth->sndavg / 10, mouth->sndavg = 0, mouth->sndcount = 0;
		return;
	}
	count = Q_min(( ch->sample > from ? ch->sample - from : 0 ) * sc->width, sizeof( b ));
	if( where[1] == RAM_SOUND )
		memcpy( b, (const byte *)(uintptr_t)where[0] + (uint32_t)from * sc->width, n = count );
	else if(( n = pread( pak_fd, b, count, where[1] + (uint32_t)from * sc->width )) <= 0 )
		return;
	if( n <= 0 )
		return;
	for( int k = 0; k < n / sc->width && scount < 10; scount++ )
	{
		int data = sc->width == 1 ? b[k] - 128 : (int16_t)( b[k * 2] | b[k * 2 + 1] << 8 ) >> 8;
		savg += abs( data );
		k += 80 + ((byte)data & 0x1F );
	}
	mouth->sndavg += savg, mouth->sndcount = scount;
	if( mouth->sndcount >= 10 )
		mouth->mouthopen = mouth->sndavg / 10, mouth->sndavg = 0, mouth->sndcount = 0;
}

// The end of a paint: a channel not heard in it has no voice
void SE_CardPainted( void )
{
	for( int v = 0; v < voice_count; v++ )
		if( voice_owner[v] >= 0 && !shadow[voice_owner[v]].touched )
			stop( v );
	for( int i = 0; i < MAX_CHANNELS; i++ )
		shadow[i].touched = 0;
}

// The card's output so far, in the engine's samples
int SE_CardTime( void )
{
	uint32_t clock = SND_CLOCK, rate = SND_RATE;

	if( !rate )
		return time_base;
	time_base += (uint32_t)((uint64_t)( clock - last_clock ) * SOUND_DMA_SPEED / rate );
	last_clock = clock;
	return time_base;
}

// A film's sound (avi_shaderemu.c): its samples in one piece of a file in the ROM, on a voice of its own
static int film_voice = -1;
static uint32_t film_rate;
static void film_keep( void )
{
	float gain = S_GetMasterVolume( ) * 256;
	SND_VOICE[film_voice].step = SND_RATE ? (uint32_t)((uint64_t)film_rate * 65536 / SND_RATE ) : 0;
	SND_VOICE[film_voice].volume = SND_VOLUME((int)gain, (int)gain );
}
int SE_FilmSound( int fd, uint32_t offset, uint32_t bytes, int rate, int channels, int width )
{
	uint32_t rom = ready && film_voice >= 0 ? snd_rom( fd, "RIFF", 4 ) : 0;
	if( !rom || ( width == 2 && (( rom + offset ) & 1 )) || channels < 1 || channels > 2 || width < 1 || width > 2 )
		return 0;
	snd_play_aux( film_voice, ( width == 1 ? SND_PCM8 : SND_PCM16 ) | SND_SMOOTH | ( channels == 2 ? SND_STEREO : 0 ),
		rom + offset, bytes / width / channels, 0, 0, 0, 0 );
	film_rate = rate;
	film_keep( );
	return 1;
}
void SE_FilmSoundKeep( void ) { if( film_rate ) film_keep( ); }
void SE_FilmSoundStop( void )
{
	if( film_voice >= 0 && film_rate )
		snd_stop( film_voice );
	film_rate = 0;
}

void S_Activate( qboolean active ) { }

qboolean SNDDMA_Init( void )
{
	if( snd_open( ) < 0 )
	{
		Con_Printf( "Audio: no sound card\n" );
		return false;
	}
	pak_open( );
	film_voice = snd_claim( );
	while( voice_count < VOICES && ( voices[voice_count] = snd_claim( )) >= 0 )
		voice_owner[voice_count++] = -1;
	for( int i = 0; i < MAX_CHANNELS; i++ )
		shadow[i].voice = -1;
	ready = voice_count > 0 && pak_rom;
	last_clock = SND_CLOCK;
	snd.format.speed = SOUND_DMA_SPEED, snd.format.width = 2, snd.format.channels = 2;
	snd.samples = 0x4000;	// (the engine paints no further ahead than half of it)
	snd.buffer = Mem_Calloc( sndpool, snd.samples * 2 );
	snd.backend_name = "the machine's card";
	snd.initialized = true;
	Con_Printf( "Audio: %d voices of the machine's card, %d sounds in pak0\n", voice_count, pak_count );
	return true;
}

void SNDDMA_BeginPainting( void ) { }
void SNDDMA_Submit( void ) { }

void SNDDMA_Shutdown( void )
{
	for( int v = 0; v < voice_count; v++ )
		stop( v );
	snd.initialized = false;
	if( snd.buffer )
		Mem_Free( snd.buffer ), snd.buffer = NULL;
}

qboolean VoiceCapture_Init( void ) { return false; }
qboolean VoiceCapture_Activate( qboolean activate ) { return false; }
qboolean VoiceCapture_Lock( qboolean lock ) { return false; }
void VoiceCapture_Shutdown( void ) { }
