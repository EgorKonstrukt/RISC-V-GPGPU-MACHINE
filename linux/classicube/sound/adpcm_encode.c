/* 16-bit mono samples on stdin to the sound card's IMA ADPCM on stdout (docs/machine/sound.md): the
 * codes, low half of a byte first, padded to 4 bytes, then a checkpoint word every 32 samples.
 * Each code is the best of the 16 for the next sample, decoded as the card decodes it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const int steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
	1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static void decode(int code, int *value, int *index)
{
	int size = steps[*index], change = (size >> 3) + (code & 4 ? size : 0) + (code & 2 ? size >> 1 : 0) + (code & 1 ? size >> 2 : 0);
	int v = *value + (code & 8 ? -change : change), i = *index + (code & 4 ? 2 * (code & 3) + 2 : -1);

	*value = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
	*index = i < 0 ? 0 : i > 88 ? 88 : i;
}

int main(void)
{
	size_t n = 0, most = 1 << 20, k;
	int16_t *in = malloc(most * 2);
	int value = 0, index = 0;

	while ((k = fread(in + n, 2, most - n, stdin)) > 0)
		if ((n += k) == most)
			in = realloc(in, (most *= 2) * 2);
	uint8_t *codes = calloc((n + 1) / 2 + 4, 1);
	uint32_t *marks = malloc(((n + 31) / 32 + 1) * 4);
	for (k = 0; k < n; k++) {
		int best = 0, error = 1 << 30;

		if ((k & 31) == 0)
			marks[k >> 5] = (uint32_t)(value & 0xffff) | (uint32_t)index << 16;
		for (int code = 0; code < 16; code++) {
			int v = value, i = index, e;

			decode(code, &v, &i);
			e = abs(v - in[k]);
			if (e < error)
				error = e, best = code;
		}
		decode(best, &value, &index);
		codes[k >> 1] |= (uint8_t)(best << (4 * (k & 1)));
	}
	fwrite(codes, 1, ((n + 1) / 2 + 3) & ~(size_t)3, stdout);
	fwrite(marks, 4, (n + 31) / 32, stdout);
	return 0;
}
