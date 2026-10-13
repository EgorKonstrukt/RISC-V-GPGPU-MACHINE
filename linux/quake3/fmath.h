/*
 * The game's trigonometry in floats, included before every file of it (build.sh): Quake's
 * (linux/quake/fmath.h says why), and the arccosine this game also asks for.
 */
#ifndef SHADEREMU_QUAKE3_FMATH_H
#define SHADEREMU_QUAKE3_FMATH_H

#include "../quake/fmath.h"

static inline float q3_acos (float c)
{
	float s = c >= 1.0f || c <= -1.0f ? 0.0f : sqrtf (1.0f - c * c);

	return q_atan2 (s, c);
}

#define acos(x) q3_acos (x)
#define round(x) roundf (x)

#endif
