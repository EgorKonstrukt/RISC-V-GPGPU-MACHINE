/*
 * minimp3 (https://github.com/lieff/minimp3, CC0), compiled twice: here as it is, and in
 * m_audio_mp3half.c with minimp3.patch's half rate. build.sh fetches and patches the header.
 * No SIMD: the machine has scalar single floats.
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#ifdef MP3D_HALF
#define mp3dec_init m_mp3half_init
#define mp3dec_decode_frame m_mp3half_frame
#else
#define mp3dec_init m_mp3full_init
#define mp3dec_decode_frame m_mp3full_frame
#endif
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include "minimp3.h"
