/*
libm_shaderemu.c - the C library's float functions that it computes in double, which this
machine does in software, made of float ones (docs/games/halflife.md)
*/
#include <math.h>

float tanf( float x )
{
	float s, c;

	sincosf( x, &s, &c );
	return s / c;
}
