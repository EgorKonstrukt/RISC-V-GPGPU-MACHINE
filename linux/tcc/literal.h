/* literal.h: a float or double written in a program's text, to the nearest value, without the
 * C library's strtof (which works in long doubles, a library call each here: 7,000 instructions
 * a number). speed_hook.py puts this into TinyCC's tccpp.c. Short numbers only: the others
 * are left to the library, and the result is the library's bit for bit. */
#include <stdint.h>
#include <string.h>

/* The digits of "12.5e-3" as a whole number and a power of ten: false for a number with more
 * than `most` digits that count, or with anything but digits, a point and an exponent. */
static int
literal_parts(const char *s, int most, uint64_t *digits, int *power)
{
	uint64_t m = 0;
	int n = 0, point = 0, after = 0, e = 0, sign = 1;

	for (; (*s >= '0' && *s <= '9') || (*s == '.' && !point); s++) {
		if (*s == '.') {
			point = 1;
			continue;
		}
		if (n || *s != '0')
			n++;
		if (n > most)
			return 0;
		m = m * 10 + (uint64_t)(*s - '0');
		after += point;
	}
	if (*s == 'e' || *s == 'E') {
		s++;
		if (*s == '-' || *s == '+')
			sign = *s++ == '-' ? -1 : 1;
		if (*s < '0' || *s > '9')
			return 0;
		for (; *s >= '0' && *s <= '9'; s++)
			if ((e = e * 10 + (*s - '0')) > 999)
				return 0;
	}
	if (*s)
		return 0;
	*digits = m;
	*power = sign * e - after;
	return 1;
}

static const uint64_t literal_tens[] = { 1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull,
	1000000000ull, 10000000000ull };

/* A float of up to nine digits and a power of ten within ten: whole-number arithmetic, the
 * quotient rounded to 24 bits once (half to even, with the division's remainder counted). */
static int
literal_float(const char *s, float *value)
{
	uint64_t m, q, rest, half, low;
	uint32_t bits, mantissa;
	int power, shift = 0, drop;

	if (!literal_parts(s, 9, &m, &power) || power > 10 || power < -10)
		return 0;
	if (m == 0) {
		*value = 0.0f;
		return 1;
	}
	if (power >= 0) {
		q = m * literal_tens[power];
		rest = 0;
	} else {
		while (!(m >> 63))
			m <<= 1, shift++;
		q = m / literal_tens[-power];
		rest = m % literal_tens[-power];
	}
	for (drop = 0; (q >> drop) >> 24; drop++)
		continue;
	mantissa = (uint32_t)(q >> drop);
	if (drop) {
		half = 1ull << (drop - 1);
		low = q & ((1ull << drop) - 1);
		if (low > half || (low == half && (rest || (mantissa & 1))))
			mantissa++;
		if (mantissa >> 24)
			mantissa >>= 1, drop++;
	} else {
		/* (fewer than 24 bits, and nothing left over: the number as it is) */
		if (rest)
			return 0;
		while (!(mantissa >> 23))
			mantissa <<= 1, drop--;
	}
	bits = (uint32_t)(drop - shift + 23 + 127) << 23 | (mantissa & 0x7fffff);
	memcpy(value, &bits, 4);
	return 1;
}

/* A double of up to fifteen digits and a power of ten within 22: both are whole numbers a
 * double holds, and one multiplication or division of them is rounded once, as it should be. */
static int
literal_double(const char *s, double *value)
{
	static const double tens[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16,
		1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
	uint64_t m;
	int power;

	if (!literal_parts(s, 15, &m, &power) || power > 22 || power < -22)
		return 0;
	*value = power >= 0 ? (double)m * tens[power] : (double)m / tens[-power];
	return 1;
}
