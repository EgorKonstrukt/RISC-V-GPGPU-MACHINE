/*
img_media.c - TGA and JPEG pictures through libmedia (docs/system/media.md), decoded on the worker cores
straight into the engine's buffer, in place of imagelib's Image_LoadTGA; a JPEG under any name
(linux/halflife/repack.py keeps the names).
*/
#include "imagelib.h"
#include "media.h"

int SE_MediaWorkers( void );

static qboolean load( const char *name, const byte *buffer, fs_offset_t filesize, enum media_kind kind )
{
	struct media_info info;
	struct media_image out = { 0 };
	const char *why;

	if( filesize < 18 || media_probe( buffer, (uint32_t)filesize, &info ) || info.kind != kind )
		return false;
	image.width = info.width, image.height = info.height;
	if( !Image_ValidSize( name ))
		return false;
	image.size = image.width * image.height * 4;
	image.rgba = Mem_Malloc( host.imagepool, image.size );
	// straight into the engine's buffer only when its rows are whole texels (each core writes rows)
	int direct = !((uintptr_t)image.rgba & 15 ) && !(( image.width * 4 ) & 15 );
	out.width = image.width, out.height = image.height, out.channels = 4;
	if( direct )
		out.stride = image.width * 4, out.pixels = image.rgba;
	if(( why = media_image_decode( buffer, (uint32_t)filesize, &out, SE_MediaWorkers( ))))
	{
		Con_DPrintf( S_ERROR "%s: %s\n", name, why );
		Mem_Free( image.rgba ), image.rgba = NULL;
		return false;
	}
	if( !direct )
	{
		for( int y = 0; y < image.height; y++ )
			memcpy( image.rgba + y * image.width * 4, out.pixels + y * out.stride, image.width * 4 );
		media_image_free( &out );
	}
	out.stride = image.width * 4;
	// imagelib keeps a top-down file's rows the other way round when asked not to flip
	if( kind == MEDIA_TGA && Image_CheckFlag( IL_DONTFLIP_TGA ) && ( buffer[17] & 0x20 ))
	{
		byte row[4096];
		for( int y = 0; y < image.height / 2 && out.stride <= sizeof( row ); y++ )
		{
			byte *a = image.rgba + y * out.stride, *b = image.rgba + ( image.height - 1 - y ) * out.stride;
			memcpy( row, a, out.stride ), memcpy( a, b, out.stride ), memcpy( b, row, out.stride );
		}
	}
	image.type = PF_RGBA_32;
	image.depth = 1;
	SetBits( image.flags, IMAGE_HAS_COLOR );
	if( info.alpha )
		SetBits( image.flags, IMAGE_HAS_ALPHA );
	// the fog colour: the mean of every 64th pixel
	uint sum[3] = { 0 }, n = 0;
	for( int i = 0; i < image.width * image.height; i += 64, n++ )
		sum[0] += image.rgba[i * 4], sum[1] += image.rgba[i * 4 + 1], sum[2] += image.rgba[i * 4 + 2];
	for( int c = 0; c < 3; c++ )
		image.fogParams[c] = n ? sum[c] / n : 0;
	return true;
}

qboolean Image_LoadTGA( const char *name, const byte *buffer, fs_offset_t filesize ) { return load( name, buffer, filesize, MEDIA_TGA ); }
qboolean Image_LoadJPEG( const char *name, const byte *buffer, fs_offset_t filesize ) { return load( name, buffer, filesize, MEDIA_JPEG ); }
