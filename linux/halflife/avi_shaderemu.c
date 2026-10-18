/*
avi_shaderemu.c - Uplink's films (media/intro.avi, the menu's logo.avi) on the machine, in place
of avi_ffmpeg.c (docs/games/halflife.md, "The start"). They are Microsoft RLE, 8 bits a pixel: a
frame is decoded into its bytes and drawn by the GPU as a paletted texture. The sound is the
card's, played where it lies in the ROM: data.py puts a film's sound in one chunk.
*/
#include "common.h"
#include "client.h"
#include "avi.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
int mcw_done( int k );
void mcw_touch_code( const void *code, unsigned bytes );

int hlglWorker( void );
void hlglPostLate( int k, uint32_t ( *fn )( uint32_t, uint32_t ), uint32_t a0, uint32_t a1 );
unsigned hlglWait( int k );
void *hlglJobMemory( unsigned bytes );

int SE_FilmSound( int fd, uint32_t offset, uint32_t bytes, int rate, int channels, int width );
void SE_FilmSoundKeep( void );
void SE_FilmSoundStop( void );

typedef struct { uint32_t at, size; } piece_t;	// a chunk's data in the file
struct movie_state_s
{
	int fd, opened, active, quiet, paused, rle, audio;
	int xres, yres, frames, decoded, texture, made, x, y, w, h;
	double frame_time, started;
	piece_t *video, sound;
	byte *pixels, *buffer;	// the frame, rows top to bottom; a chunk read
	uint32_t buffer_size;
	int rate, channels, width, sound_on;
	byte palette[256 * 4];
	char name[16];
	struct film_s *film;	// the frames decoded whole on a worker, once it is done
	int shown;
};

// A short film without sound decoded whole on a worker, its frames and palettes kept for the
// next time it is opened (mcw_alloc's memory is never freed)
#define FILM_BYTES ( 8 << 20 )
typedef struct film_s
{
	char path[64];
	uint32_t file_size;
	int job, done, frames, size;
	const byte *file;
	piece_t *video;
	byte *frames_at, *palettes;
	byte palette[256 * 4];
	int rle, xres, yres;
} film_t;
static film_t films[2];

static movie_state_t avi[2];
static poolhandle_t avi_mempool;

static uint32_t le32( const byte *p ) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void palette_from( movie_state_t *Avi, const byte *bgrx, int first, int count, int rgbx )
{
	for( int i = 0; i < count && first + i < 256; i++ )
	{
		byte *e = Avi->palette + ( first + i ) * 4;
		e[0] = bgrx[i * 4 + ( rgbx ? 0 : 2 )], e[1] = bgrx[i * 4 + 1], e[2] = bgrx[i * 4 + ( rgbx ? 2 : 0 )], e[3] = 255;
	}
}

// A chunk's data into the buffer: its size, or -1
static int chunk_read( movie_state_t *Avi, const piece_t *p )
{
	if( p->size > Avi->buffer_size )
	{
		if( Avi->buffer )
			Mem_Free( Avi->buffer );
		Avi->buffer_size = ( p->size + 4095 ) & ~4095u;
		Avi->buffer = Mem_Malloc( avi_mempool, Avi->buffer_size );
	}
	return pread( Avi->fd, Avi->buffer, p->size, p->at & 0x7fffffffu ) == (ssize_t)p->size ? (int)p->size : -1;
}

// One frame's chunk over the last: RLE8 runs, deltas and literal runs, rows from the bottom
static void frame_decode( int rle, int w, int h, byte *pixels, const byte *s, int n )
{
	const byte *end = s + n;
	int x = 0, y = h - 1;

	if( !rle )
	{
		int stride = ( w + 3 ) & ~3;
		for( y = 0; y < h && ( y + 1 ) * stride <= n; y++ )
			memcpy( pixels + ( h - 1 - y ) * w, s + y * stride, w );
		return;
	}
	if( n >= 2 && s[0] == 0 && s[1] == 1 )
		return;	// (a frame the same as the last)
	while( s + 2 <= end && y >= 0 )
	{
		int count = s[0], code = s[1];
		byte *row = pixels + y * w;
		s += 2;
		if( count )
		{
			if( x + count > w ) count = w - x;
			memset( row + x, code, count );
			x += count;
		}
		else if( code == 0 )
			x = 0, y--;
		else if( code == 1 )
			break;
		else if( code == 2 )
		{
			if( s + 2 > end ) break;
			x += s[0], y -= s[1], s += 2;
		}
		else
		{
			int copy = code;
			if( s + code > end ) break;
			if( x + copy > w ) copy = w - x;
			if( copy > 0 ) memcpy( row + x, s, copy );
			x += code, s += ( code + 1 ) & ~1;
		}
	}
}

qboolean AVI_SetParm( movie_state_t *Avi, enum movie_parms_e parm, ... )
{
	qboolean ret = true;
	va_list va;

	va_start( va, parm );
	while( parm != AVI_PARM_LAST )
	{
		switch( parm )
		{
		case AVI_RENDER_TEXNUM: Avi->texture = va_arg( va, int ); break;
		case AVI_RENDER_X: Avi->x = va_arg( va, int ); break;
		case AVI_RENDER_Y: Avi->y = va_arg( va, int ); break;
		case AVI_RENDER_W: Avi->w = va_arg( va, int ); break;
		case AVI_RENDER_H: Avi->h = va_arg( va, int ); break;
		case AVI_REWIND: Avi->decoded = -1, Avi->started = 0; break;
		case AVI_ENTNUM: case AVI_VOLUME: (void)va_arg( va, int ); break;
		case AVI_ATTN: (void)va_arg( va, double ); break;
		case AVI_PAUSE: Avi->paused = true; break;
		case AVI_RESUME: Avi->paused = false; break;
		default: ret = false;
		}
		parm = va_arg( va, enum movie_parms_e );
	}
	va_end( va );
	return ret;
}

int AVI_GetVideoFrameNumber( movie_state_t *Avi, float time )
{
	return Avi->frame_time > 0 ? (int)( time / Avi->frame_time ) : 0;
}

byte *AVI_GetVideoFrame( movie_state_t *Avi, int frame ) { return Avi->pixels; }

qboolean AVI_GetVideoInfo( movie_state_t *Avi, int *xres, int *yres, float *duration )
{
	if( !Avi->active )
		return false;
	if( xres ) *xres = Avi->xres;
	if( yres ) *yres = Avi->yres;
	if( duration ) *duration = Avi->frames * Avi->frame_time;
	return true;
}

qboolean AVI_HaveAudioTrack( const movie_state_t *Avi ) { return Avi && Avi->active && Avi->audio; }

// Every frame of a film into its memory in order, each over a copy of the one before: on a worker
static uint32_t film_job( uint32_t a0, uint32_t a1 )
{
	film_t *f = (film_t *)a0;
	byte palette[256 * 4];

	memcpy( palette, f->palette, sizeof( palette ));
	for( int i = 0; i < f->frames; i++ )
	{
		const piece_t *p = &f->video[i];
		const byte *c = f->file + ( p->at & 0x7fffffffu );
		byte *to = f->frames_at + i * f->size;

		if( i ) memcpy( to, to - f->size, f->size );
		if( p->size && ( p->at & 0x80000000u ))	// (palette_from's, rgbx)
			for( int k = 0; k < ( c[1] ? c[1] : 256 ) && c[0] + k < 256; k++ )
			{
				byte *e = palette + ( c[0] + k ) * 4;
				e[0] = c[4 + k * 4], e[1] = c[5 + k * 4], e[2] = c[6 + k * 4], e[3] = 255;
			}
		else if( p->size )
			frame_decode( f->rle, f->xres, f->yres, to, c, p->size );
		memcpy( f->palettes + i * 1024, palette, 1024 );
	}
	return 0;
}

// The film opened decoded on a worker, or found decoded before (FILM=read: as it plays, here)
static void film_give( movie_state_t *Avi, const char *path )
{
	static int off = -1;
	uint32_t size = (uint32_t)Avi->xres * Avi->yres;
	film_t *f = NULL;

	if( off < 0 ) off = getenv( "FILM" ) && !strcmp( getenv( "FILM" ), "read" );
	if( off || Avi->audio || size * Avi->frames > FILM_BYTES || Q_strlen( path ) >= sizeof( f->path ))
		return;
	for( int i = 0; i < 2 && !f; i++ )
		if( !Q_strcmp( films[i].path, path ) && films[i].frames == Avi->frames && films[i].size == (int)size )
			f = &films[i];
	for( int i = 0; i < 2 && !f; i++ )
		if( !films[i].path[0] )
			f = &films[i];
	if( !f || ( f->path[0] && Q_strcmp( f->path, path )))
		return;
	Avi->film = f, Avi->shown = -1;
	if( f->path[0] )
		return;
	f->file_size = lseek( Avi->fd, 0, SEEK_END );
	f->file = mmap( NULL, f->file_size, PROT_READ, MAP_PRIVATE, Avi->fd, 0 );
	f->frames_at = hlglJobMemory( size * Avi->frames );
	f->palettes = hlglJobMemory( 1024 * Avi->frames );
	f->video = hlglJobMemory( sizeof( piece_t ) * Avi->frames );
	if( f->file == MAP_FAILED || !f->frames_at || !f->palettes || !f->video || ( f->job = hlglWorker( )) <= 0 )
	{
		if( f->file != MAP_FAILED && f->file ) munmap( (void *)f->file, f->file_size );
		Avi->film = NULL;
		return;	// (the memory is kept for nothing: mcw_alloc's is never freed)
	}
	mcw_touch_code( f->file, f->file_size );	// (a worker stops at a page this core has not read)
	memcpy( f->video, Avi->video, sizeof( piece_t ) * Avi->frames );
	memcpy( f->palette, Avi->palette, sizeof( f->palette ));
	f->frames = Avi->frames, f->size = size, f->rle = Avi->rle, f->xres = Avi->xres, f->yres = Avi->yres;
	Q_strncpy( f->path, path, sizeof( f->path ));
	hlglPostLate( f->job, film_job, (uint32_t)f, 0 );
}

// The decoded film's frame for now (true), once its job is done
static qboolean film_show( movie_state_t *Avi, double now, qboolean *over )
{
	film_t *f = Avi->film;

	if( !f->done && !( f->done = mcw_done( f->job )))
		return false;
	if( !Avi->started || Avi->paused )
		Avi->started = now - ( Avi->decoded + 1 ) * Avi->frame_time;
	int target = (int)(( now - Avi->started ) / Avi->frame_time );
	if(( *over = target >= Avi->frames ))
		return true;
	Avi->decoded = target = Q_max( target, 0 );
	if( target != Avi->shown || !Avi->made )
	{
		rgbdata_t pic = { 0 };
		pic.width = Avi->xres, pic.height = Avi->yres, pic.depth = 1, pic.type = PF_INDEXED_32;
		pic.flags = IMAGE_HAS_COLOR, pic.palette = f->palettes + target * 1024;
		pic.buffer = f->frames_at + target * f->size, pic.size = f->size;
		int have = Avi->made ? Avi->made : ref.dllFuncs.GL_FindTexture( Avi->name );
		extern int hlglTexLater;
		hlglTexLater = 1;	// (its tiles copied on a worker: the frame's bytes stay where they are)
		Avi->made = ref.dllFuncs.GL_LoadTextureFromBuffer( Avi->name, &pic, TF_NOMIPMAP | TF_CLAMP, have != 0 );
		hlglTexLater = 0;
		Avi->shown = target;
	}
	return true;
}

/* The film's frame for now: every frame since the last decoded (each draws over the one before),
 * then the picture as a paletted texture, drawn at the place given or left in the texture given. */
qboolean AVI_Think( movie_state_t *Avi )
{
	double now = Platform_DoubleTime();
	int target, made = 0;

	static int loops = -1, drawn, looped;
	if( loops < 0 ) loops = getenv( "FILM_LOOPS" ) != NULL;	// a line at every loop: the frames that drew it, for rvc_cpu --count-from
	if( !Avi->active )
		return false;
	drawn++;
	if( loops && Avi->decoded == -1 )
		fprintf( stderr, "film: loop %d, %d frames\n", ++looped, drawn );
	qboolean over;
	if( Avi->film && film_show( Avi, now, &over ))
	{
		if( over )
			return false;
		goto draw;
	}
	if( !Avi->started || Avi->paused )
		Avi->started = now - ( Avi->decoded + 1 ) * Avi->frame_time;
	if( Avi->audio && !Avi->sound_on && Avi->decoded < 0 && !Avi->paused )
		Avi->sound_on = SE_FilmSound( Avi->fd, Avi->sound.at, Avi->sound.size, Avi->rate, Avi->channels, Avi->width );
	if( Avi->sound_on )
		SE_FilmSoundKeep();
	target = (int)(( now - Avi->started ) / Avi->frame_time );
	if( target >= Avi->frames )
		return false;
	if( target - Avi->decoded > 8 )	// (too far behind: the film waits, its sound does not)
		Avi->started += ( target - Avi->decoded - 8 ) * Avi->frame_time, target = Avi->decoded + 8;
	while( Avi->decoded < target )
	{
		piece_t *p = &Avi->video[++Avi->decoded];
		int n;
		if( !p->size )
			continue;
		if(( n = chunk_read( Avi, p )) < 0 )
			return false;
		if( p->at & 0x80000000u )	// (a palette's change: first, count, flags, then r, g, b, flags)
			palette_from( Avi, Avi->buffer + 4, Avi->buffer[0], Avi->buffer[1] ? Avi->buffer[1] : 256, 1 );
		else frame_decode( Avi->rle, Avi->xres, Avi->yres, Avi->pixels, Avi->buffer, n ), made = 1;
	}
	if( made || !Avi->made )
	{
		rgbdata_t pic = { 0 };
		pic.width = Avi->xres, pic.height = Avi->yres, pic.depth = 1, pic.type = PF_INDEXED_32;
		pic.flags = IMAGE_HAS_COLOR, pic.palette = Avi->palette, pic.buffer = Avi->pixels, pic.size = Avi->xres * Avi->yres;
		int have = Avi->made ? Avi->made : ref.dllFuncs.GL_FindTexture( Avi->name );	// (a film opened again keeps its texture)
		Avi->made = ref.dllFuncs.GL_LoadTextureFromBuffer( Avi->name, &pic, TF_NOMIPMAP | TF_CLAMP, have != 0 );
	}
draw:
	if( Avi->made && Avi->texture == 0 )
		ref.dllFuncs.R_DrawStretchPic( Avi->x, Avi->y, Avi->w >= 0 ? Avi->w : refState.width, Avi->h >= 0 ? Avi->h : refState.height,
			0, 0, 1, 1, Avi->made );
	return true;
}

// The headers, the palette and where every frame's chunk lies (the index, or the chunks walked)
void AVI_OpenVideo( movie_state_t *Avi, const char *filename, qboolean load_audio, int quiet )
{
	byte head[16384], *p, *end;
	uint32_t movi = 0, movi_size = 0, file_size, at;
	int video_stream = -1, audio_stream = -1, stream = 0, vids = 0, count = 0, n;
	char name[16];

	AVI_CloseVideo( Avi );
	Q_strncpy( name, Avi->name, sizeof( name ));
	memset( Avi, 0, sizeof( *Avi ));
	Q_strncpy( Avi->name, name, sizeof( Avi->name ));
	Avi->quiet = quiet, Avi->decoded = -1, Avi->w = Avi->h = -1;
	if( !avi_mempool || !filename || ( Avi->fd = open( filename, O_RDONLY | O_CLOEXEC )) < 0 )
		goto fail;
	Avi->opened = 1;
	file_size = lseek( Avi->fd, 0, SEEK_END );
	if(( n = pread( Avi->fd, head, sizeof( head ), 0 )) < 64 || memcmp( head, "RIFF", 4 ) || memcmp( head + 8, "AVI ", 4 ))
		goto fail;
	// walk the header list (its LISTs opened, their chunks read) until movi
	for( p = head + 12, end = head + n; p + 8 <= end; )
	{
		uint32_t size = le32( p + 4 );
		if( !memcmp( p, "LIST", 4 ))
		{
			if( !memcmp( p + 8, "movi", 4 ))
			{
				movi = (uint32_t)( p - head ) + 12, movi_size = size - 4;
				break;
			}
			p += 12;
			continue;
		}
		if( !memcmp( p, "avih", 4 ) && p + 48 <= end )
			Avi->frame_time = le32( p + 8 ) / 1e6;
		else if( !memcmp( p, "strh", 4 ) && p + 16 <= end )
		{
			if( !memcmp( p + 8, "vids", 4 ) && video_stream < 0 )
				video_stream = stream;
			else if( !memcmp( p + 8, "auds", 4 ) && audio_stream < 0 )
				audio_stream = stream;
			if( !memcmp( p + 8, "vids", 4 ) && p + 40 <= end && le32( p + 28 ))	// (its rate and scale)
				Avi->frame_time = (double)le32( p + 28 ) / le32( p + 32 );
			stream++;
		}
		else if( !memcmp( p, "strf", 4 ) && stream - 1 == video_stream && p + 48 <= end )
		{
			int bits = p[22] | p[23] << 8, compression = le32( p + 24 ), colours = le32( p + 40 );
			Avi->xres = le32( p + 12 ), Avi->yres = abs( (int)le32( p + 16 ));
			if( bits != 8 || ( compression != 0 && compression != 1 ))
			{
				if( !quiet ) Con_Printf( S_ERROR "AVI: %s is not 8-bit RLE\n", filename );
				goto fail;
			}
			Avi->rle = compression == 1;
			colours = colours ? Q_min( colours, 256 ) : 256;
			if( p + 48 + colours * 4 <= end )
				palette_from( Avi, p + 48, 0, colours, 0 );
		}
		else if( !memcmp( p, "strf", 4 ) && stream - 1 == audio_stream && p + 24 <= end )
		{
			if(( p[8] | p[9] << 8 ) == 1 )	// PCM
				Avi->channels = p[10], Avi->rate = le32( p + 12 ), Avi->width = ( p[22] | p[23] << 8 ) / 8;
		}
		p += 8 + (( size + 1 ) & ~1u );
	}
	if( !movi || video_stream < 0 || Avi->xres <= 0 || Avi->yres <= 0 || Avi->frame_time <= 0 )
		goto fail;
	// the chunks of movi, in the order they are: two digits of the stream, then dc or db (a frame), wb, pc
	{
		int room = 256;
		Avi->video = Mem_Malloc( avi_mempool, room * sizeof( piece_t ));
		for( at = movi; at + 8 <= movi + movi_size && at + 8 <= file_size; )
		{
			byte c[12];
			uint32_t size;
			if( pread( Avi->fd, c, 12, at ) < 8 )
				break;
			size = le32( c + 4 );
			if( !memcmp( c, "LIST", 4 ))	// (rec lists)
			{
				at += 12;
				continue;
			}
			n = ( c[0] - '0' ) * 10 + ( c[1] - '0' );
			if( n == video_stream && ( c[2] == 'd' || ( c[2] == 'p' && c[3] == 'c' )))
			{
				if( vids == room )
					Avi->video = Mem_Realloc( avi_mempool, Avi->video, ( room *= 2 ) * sizeof( piece_t ));
				Avi->video[vids].at = ( at + 8 ) | ( c[2] == 'p' ? 0x80000000u : 0 );
				Avi->video[vids].size = size;
				vids++;
			}
			else if( n == audio_stream && c[2] == 'w' && c[3] == 'b' && size )
			{
				if( !count++ )
					Avi->sound.at = at + 8, Avi->sound.size = size;
			}
			at += 8 + (( size + 1 ) & ~1u );
		}
	}
	Avi->frames = vids;
	// the sound is played from the ROM: only in one chunk (data.py), else the film is silent
	Avi->audio = load_audio && count == 1 && Avi->rate && Avi->channels;
	if( load_audio && count > 1 && !quiet )
		Con_Printf( "AVI: %s has its sound in %d chunks: played without it\n", filename, count );
	Avi->pixels = Mem_Calloc( avi_mempool, Avi->xres * Avi->yres );
	Avi->active = vids > 0;
	if( Avi->active )
	{
		film_give( Avi, filename );
		return;
	}
fail:
	if( !quiet && filename )
		Con_Printf( S_ERROR "AVI: could not play %s\n", filename );
	AVI_CloseVideo( Avi );
}

void AVI_CloseVideo( movie_state_t *Avi )
{
	if( !Avi )
		return;
	if( Avi->sound_on )
		SE_FilmSoundStop();
	if( Avi->opened )
		close( Avi->fd );
	if( avi_mempool )
	{
		if( Avi->video ) Mem_Free( Avi->video );
		if( Avi->pixels ) Mem_Free( Avi->pixels );
		if( Avi->buffer ) Mem_Free( Avi->buffer );
	}
	Avi->video = NULL, Avi->pixels = Avi->buffer = NULL, Avi->buffer_size = 0, Avi->film = NULL;
	Avi->opened = Avi->active = Avi->sound_on = 0;
}

movie_state_t *AVI_GetState( int num ) { return &avi[num]; }
qboolean AVI_IsActive( movie_state_t *Avi ) { return Avi ? Avi->active : false; }

qboolean AVI_Initailize( void )
{
	if( Sys_CheckParm( "-noavi" ))
	{
		Con_Printf( "AVI: Disabled\n" );
		return false;
	}
	if( !avi_mempool )
		avi_mempool = Mem_AllocPool( "AVI Zone" );
	for( int i = 0; i < 2; i++ )
		Q_snprintf( avi[i].name, sizeof( avi[i].name ), "*avi%d", i );
	return true;
}

void AVI_Shutdown( void )
{
	for( int i = 0; i < 2; i++ )
		AVI_CloseVideo( &avi[i] );
	Mem_FreePool( &avi_mempool );
}

movie_state_t *AVI_LoadVideo( const char *filename, qboolean load_audio )
{
	string path;
	const char *fullpath;
	movie_state_t *Avi;
	static int films;

	if( !avi_mempool )
		return NULL;
	Q_snprintf( path, sizeof( path ), "media/%s", filename );
	COM_DefaultExtension( path, ".avi", sizeof( path ));
	if( !( fullpath = FS_GetDiskPath( path, false )))
		return NULL;
	Avi = Mem_Calloc( avi_mempool, sizeof( movie_state_t ));
	Q_snprintf( Avi->name, sizeof( Avi->name ), "*film%d", films++ );
	AVI_OpenVideo( Avi, fullpath, load_audio, false );
	if( !AVI_IsActive( Avi ))
	{
		AVI_FreeVideo( Avi );
		return NULL;
	}
	return Avi;
}

void AVI_FreeVideo( movie_state_t *Avi )
{
	if( !Avi )
		return;
	AVI_CloseVideo( Avi );
	if( Avi != &avi[0] && Avi != &avi[1] && avi_mempool && Mem_IsAllocatedExt( avi_mempool, Avi ))
		Mem_Free( Avi );
}
