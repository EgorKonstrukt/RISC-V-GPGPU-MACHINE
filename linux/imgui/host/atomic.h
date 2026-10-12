// What musl's qsort.c asks of musl, for building it on the host (bake_font.cpp): the font's
// rectangles are packed in the order the guest's own qsort leaves equal ones in.
#ifndef BAKE_ATOMIC_H
#define BAKE_ATOMIC_H
#include <stddef.h>
#define a_ctz_l(x) __builtin_ctzl(x)
#define weak_alias(old, new) extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
void __qsort_r(void* base, size_t count, size_t width, int (*compare)(const void*, const void*, void*), void* argument);
#endif
