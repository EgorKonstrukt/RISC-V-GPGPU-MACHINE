/*
 * One deflate stream inflated on many cores (docs/system/media.md, "One deflate stream on many cores").
 *
 * The compressed bytes are cut into as many parts as there are cores. A core finds the first
 * block of its part (a dynamic block whose header and codes are whole, which decodes to its end
 * and is followed by another good header) and inflates from there to where the next part's
 * block begins, into a ring of 16-bit values: a byte, or which byte of the 32 KB before its part
 * a copy took (not known yet). The first core then follows the parts from the stream's start,
 * each window from the one before, and every part is inflated again, now knowing its window,
 * into its place. The first part is made in place at once. CRC-32 on the way, joined at the end.
 */
#include <stdlib.h>
#include "m_int.h"

/* ---------------------------------------------------------------- CRC-32 */

static uint32_t crc_table[8][256];

static void
crc_init(void)
{
	uint32_t i, k, c;

	if (crc_table[0][1])
		return;
	for (i = 0; i < 256; i++) {
		for (c = i, k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
		crc_table[0][i] = c;
	}
	for (i = 0; i < 256; i++)
		for (k = 1; k < 8; k++)
			crc_table[k][i] = (crc_table[k - 1][i] >> 8) ^ crc_table[0][crc_table[k - 1][i] & 255];
}

uint32_t
media_crc32(uint32_t crc, const void *data, uint32_t bytes)
{
	const uint8_t *p = data;

	crc_init();
	crc = ~crc;
	while (bytes && ((uintptr_t)p & 3))
		crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 255], bytes--;
	for (; bytes >= 8; bytes -= 8, p += 8) {
		uint32_t a = crc ^ *(const uint32_t *)p, b = *(const uint32_t *)(p + 4);

/* (a byte's place in a table as a shift and a mask: the machine has no shifted add) */
#define T(k, at) (*(const uint32_t *)((const uint8_t *)crc_table[k] + (at)))
		crc = T(7, (a << 2) & 0x3fc) ^ T(6, (a >> 6) & 0x3fc) ^ T(5, (a >> 14) & 0x3fc) ^ T(4, (a >> 22) & 0x3fc) ^
		      T(3, (b << 2) & 0x3fc) ^ T(2, (b >> 6) & 0x3fc) ^ T(1, (b >> 14) & 0x3fc) ^ T(0, (b >> 22) & 0x3fc);
#undef T
	}
	while (bytes--)
		crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 255];
	return ~crc;
}

/* (a times b) modulo the CRC's polynomial, bit-reflected; x^(2^n) for the join */
static uint32_t
mult_mod(uint32_t a, uint32_t b)
{
	uint32_t m = 1u << 31, p = 0;

	for (;;) {
		if (a & m) {
			p ^= b;
			if (!(a & (m - 1)))
				break;
		}
		m >>= 1;
		b = b & 1 ? (b >> 1) ^ 0xedb88320u : b >> 1;
	}
	return p;
}

uint32_t
media_crc32_join(uint32_t crc1, uint32_t crc2, uint32_t bytes2)
{
	uint32_t p = 1u << 31, x = 1u << 30, n = bytes2, k;

	/* x^(8 * bytes2): x^(2^k) squared up as the bits of 8 * bytes2 call for it */
	for (k = 0; k < 3; k++)
		x = mult_mod(x, x);
	while (n) {
		if (n & 1)
			p = mult_mod(x, p);
		n >>= 1;
		x = mult_mod(x, x);
	}
	return mult_mod(p, crc1) ^ crc2;
}

/* ---------------------------------------------------------------- bits and codes */

struct bits { const uint8_t *in; uint32_t size, pos, hold; int count, over; };

static inline void
bits_at(struct bits *b, const uint8_t *in, uint32_t size, uint32_t bit)
{
	b->in = in, b->size = size, b->pos = bit >> 3, b->hold = 0, b->count = 0, b->over = 0;
	if (bit & 7) {
		b->hold = b->pos < size ? in[b->pos] >> (bit & 7) : 0;
		b->count = 8 - (int)(bit & 7);
		b->pos++;
	}
}

static inline uint32_t bits_pos(const struct bits *b) { return b->pos * 8 - (uint32_t)b->count; }

static inline void
need(struct bits *b, int n)
{
	while (b->count < n) {
		uint32_t byte = 0;

		if (b->pos < b->size)
			byte = b->in[b->pos];
		else
			b->over++;
		b->pos++;
		b->hold |= byte << b->count;
		b->count += 8;
	}
}

static inline uint32_t
take(struct bits *b, int n)
{
	uint32_t v;

	need(b, n);
	v = b->hold & ((1u << n) - 1);
	b->hold >>= n;
	b->count -= n;
	return v;
}

#define QB 10
struct huff { uint16_t quick[1 << QB], count[16], symbol[288]; };
enum { CODES, LENS, DISTS };

/* The canonical code of these lengths; -1 for a set no good encoder makes (as zlib refuses). */
static int
huff_make(struct huff *h, const uint8_t *lengths, int n, int kind)
{
	uint16_t offset[16], next[16];
	int i, code = 0, left = 1, most = 0;

	memset(h->count, 0, sizeof h->count);
	memset(h->quick, 0, sizeof h->quick);
	for (i = 0; i < n; i++)
		h->count[lengths[i]]++;
	h->count[0] = 0;
	for (i = 1; i < 16; i++) {
		left <<= 1;
		left -= h->count[i];
		if (left < 0)
			return -1;
		if (h->count[i])
			most = i;
	}
	if (!most)
		return kind == DISTS ? 0 : -1;	/* (a block of literals alone has no distances) */
	if (left > 0 && (kind == CODES || most != 1))
		return -1;
	offset[1] = 0;
	for (i = 1; i < 15; i++)
		offset[i + 1] = offset[i] + h->count[i];
	for (i = 1; i <= 15; i++) {
		next[i] = (uint16_t)code;
		code = (code + h->count[i]) << 1;
	}
	for (i = 0; i < n; i++) {
		int length = lengths[i];

		if (!length)
			continue;
		h->symbol[offset[length]++] = (uint16_t)i;
		if (length <= QB) {
			int straight = next[length], turned = 0, k;

			for (k = 0; k < length; k++)
				turned |= (straight >> k & 1) << (length - 1 - k);
			for (k = turned; k < 1 << QB; k += 1 << length)
				h->quick[k] = (uint16_t)(length << 12 | i);
		}
		next[length]++;
	}
	return 0;
}

static inline int
code_slow(struct bits *b, const struct huff *h)
{
	int code = 0, first = 0, index = 0, length;

	need(b, 15);
	for (length = 1; length <= 15; length++) {
		int count = h->count[length];

		code |= (int)(b->hold & 1);
		b->hold >>= 1;
		b->count--;
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

static inline int
code(struct bits *b, const struct huff *h)
{
	uint16_t q;

	need(b, 15);
	q = h->quick[b->hold & ((1 << QB) - 1)];
	if (q) {
		b->hold >>= q >> 12;
		b->count -= q >> 12;
		return q & 0xfff;
	}
	return code_slow(b, h);
}

static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
					   2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* A dynamic block's header from the bits after its type: its two codes. -1 if not a good one. */
static int
dynamic_codes(struct bits *b, struct huff *lit, struct huff *dist, struct huff *meta)
{
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	uint8_t lengths[320];
	int hlit = (int)take(b, 5) + 257, hdist = (int)take(b, 5) + 1, hclen = (int)take(b, 4) + 4, i;

	if (hlit > 286 || hdist > 30)
		return -1;
	memset(lengths, 0, 19);
	for (i = 0; i < hclen; i++)
		lengths[order[i]] = (uint8_t)take(b, 3);
	if (huff_make(meta, lengths, 19, CODES))
		return -1;
	for (i = 0; i < hlit + hdist;) {
		int symbol = code(b, meta), repeat, value;

		if (symbol < 0)
			return -1;
		if (symbol < 16) {
			lengths[i++] = (uint8_t)symbol;
			continue;
		}
		if (symbol == 16) {
			if (!i)
				return -1;
			value = lengths[i - 1];
			repeat = 3 + (int)take(b, 2);
		} else if (symbol == 17) {
			value = 0;
			repeat = 3 + (int)take(b, 3);
		} else {
			value = 0;
			repeat = 11 + (int)take(b, 7);
		}
		if (i + repeat > hlit + hdist)
			return -1;
		while (repeat--)
			lengths[i++] = (uint8_t)value;
	}
	if (!lengths[256])
		return -1;
	if (huff_make(lit, lengths, hlit, LENS) || huff_make(dist, lengths + hlit, hdist, DISTS))
		return -1;
	return b->over > 4 ? -1 : 0;
}

static void
fixed_codes(struct huff *lit, struct huff *dist)
{
	uint8_t lengths[288];
	int i;

	for (i = 0; i < 288; i++)
		lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
	huff_make(lit, lengths, 288, LENS);
	for (i = 0; i < 32; i++)
		lengths[i] = 5;	/* (30 and 31 complete the code and never come) */
	huff_make(dist, lengths, 32, LENS);
}

/* ---------------------------------------------------------------- one part's inflate */

#define WINDOW 32768u
#define MARK 0x8000u		/* a 16-bit value with this bit: byte (value & 0x7fff) of the window before the part */

struct dec {
	struct bits b;
	struct huff lit, dist, meta;
	/* bytes: out[0..n) of cap, the window's wlen bytes before out[0] */
	uint8_t *out;
	uint32_t n, cap, wlen;
	const uint8_t *window;
	/* values: a ring of WINDOW, rn made so far, the last that was a mark (+1) */
	uint16_t *ring;
	uint32_t rn, marked;
	int last, bad;
};

enum { BLOCK_OK, BLOCK_BAD, BLOCK_FULL };

/* A code's symbol the slow way (longer than QB bits) from 15 bits or more in hold; -1 if none. */
static inline int
slow_bits(uint32_t *hold, int *count, const struct huff *h)
{
	int code = 0, first = 0, index = 0, length;

	for (length = 1; length <= 15; length++) {
		int n = h->count[length];

		code |= (int)(*hold & 1);
		*hold >>= 1;
		(*count)--;
		if (code - n < first)
			return h->symbol[index + (code - first)];
		index += n;
		first += n;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/*
 * A block's symbols to its end, as bytes (wide 0) or values (wide 1). The reader and the output
 * are locals here: a store through a pointer could be one to *d, for all the compiler knows.
 */
static inline int
symbols(struct dec *d, int wide)
{
	const uint8_t *in = d->b.in;
	const struct huff *lit = &d->lit, *dist = &d->dist;
	uint32_t size = d->b.size, pos = d->b.pos, hold = d->b.hold, n = d->n, cap = d->cap, rn = d->rn, marked = d->marked;
	uint8_t *out = d->out;
	uint16_t *ring = d->ring;
	int count = d->b.count, r = BLOCK_OK;
/* 25 bits or more in hold; past the input, zeros */
#define FILL() do { \
		if (pos + 4 <= size) { \
			while (count <= 24) \
				hold |= (uint32_t)in[pos++] << count, count += 8; \
		} else { \
			while (count <= 24) \
				hold |= (uint32_t)(pos < size ? in[pos] : 0) << count, pos++, count += 8; \
		} \
	} while (0)
#define DECODE(symbol, h) do { \
		uint16_t q_ = (h)->quick[hold & ((1 << QB) - 1)]; \
		if (q_) { \
			hold >>= q_ >> 12, count -= q_ >> 12; \
			symbol = q_ & 0xfff; \
		} else \
			symbol = slow_bits(&hold, &count, h); \
	} while (0)
#define BITS(v, k) do { v = hold & ((1u << (k)) - 1); hold >>= (k); count -= (k); } while (0)

	for (;;) {
		int symbol, ds, extra;
		uint32_t length, distance, more;

		FILL();
		DECODE(symbol, lit);
		if (symbol < 256) {
			if (symbol < 0) {
				r = BLOCK_BAD;
				break;
			}
			if (wide) {
				ring[rn++ & (WINDOW - 1)] = (uint16_t)symbol;
			} else {
				if (n >= cap) {
					r = BLOCK_FULL;
					break;
				}
				out[n++] = (uint8_t)symbol;
			}
			continue;
		}
		if (symbol == 256)
			break;
		if (symbol > 285) {
			r = BLOCK_BAD;
			break;
		}
		extra = length_extra[symbol - 257];
		BITS(more, extra);
		length = length_base[symbol - 257] + more;
		FILL();
		DECODE(ds, dist);
		if (ds < 0 || ds >= 30) {
			r = BLOCK_BAD;
			break;
		}
		extra = distance_extra[ds];
		if (extra > 8)
			FILL();
		BITS(more, extra);
		distance = distance_base[ds] + more;
		if (wide) {
			uint32_t stop = rn + length, any = 0;

			if (distance > rn + WINDOW) {
				r = BLOCK_BAD;
				break;
			}
			/* from before the part: marks, until the copy reaches what the part made */
			for (; rn < stop && distance > rn; rn++)
				ring[rn & (WINDOW - 1)] = (uint16_t)(MARK | (WINDOW - (distance - rn))), any = MARK;
			for (; rn < stop; rn++) {
				uint32_t v = ring[(rn - distance) & (WINDOW - 1)];

				ring[rn & (WINDOW - 1)] = (uint16_t)v;
				any |= v;
			}
			if (any & MARK)
				marked = rn;	/* (at most this far: whether the window ends clean is all it decides) */
		} else {
			uint8_t *to = out + n, *stop;

			if (length > cap - n) {
				r = BLOCK_FULL;
				break;
			}
			stop = to + length;
			if (distance <= n) {
				const uint8_t *from = to - distance;

				if (distance == 1) {
					uint8_t v = to[-1];

					while (to < stop)
						*to++ = v;
				} else {
					while (to < stop)
						*to++ = *from++;
				}
			} else {
				int32_t s = (int32_t)n - (int32_t)distance;

				if (distance - n > d->wlen) {
					r = BLOCK_BAD;
					break;
				}
				for (; to < stop; s++)
					*to++ = s < 0 ? d->window[(int32_t)d->wlen + s] : out[s];
			}
			n += length;
		}
	}
#undef FILL
#undef DECODE
#undef BITS
	if (pos * 8 - (uint32_t)count > size * 8)
		r = BLOCK_BAD;	/* (it took bits the input does not have) */
	d->b.pos = pos, d->b.hold = hold, d->b.count = count;
	d->n = n, d->rn = rn, d->marked = marked;
	return r;
}

/* One block from its header on, as bytes (wide 0) or as values (wide 1). */
static inline int
block(struct dec *d, int wide)
{
	struct bits *b = &d->b;
	uint32_t kind;

	d->last = (int)take(b, 1);
	kind = take(b, 2);
	if (kind == 0) {
		uint32_t length, check;

		take(b, b->count & 7);
		length = take(b, 16);
		check = take(b, 16);
		if ((length ^ 0xffff) != check)
			return BLOCK_BAD;
		/* whole bytes from here: what the bit buffer holds goes back */
		b->pos -= (uint32_t)b->count >> 3, b->count = 0, b->hold = 0;
		if (b->pos + length > b->size)
			return BLOCK_BAD;
		if (wide) {
			uint32_t i;

			for (i = 0; i < length; i++)
				d->ring[(d->rn + i) & (WINDOW - 1)] = b->in[b->pos + i];
			d->rn += length;
		} else {
			if (length > d->cap - d->n)
				return BLOCK_FULL;
			memcpy(d->out + d->n, b->in + b->pos, length);
			d->n += length;
		}
		b->pos += length;
		return BLOCK_OK;
	}
	if (kind == 3)
		return BLOCK_BAD;
	if (kind == 1)
		fixed_codes(&d->lit, &d->dist);
	else if (dynamic_codes(b, &d->lit, &d->dist, &d->meta))
		return BLOCK_BAD;
	return symbols(d, wide);
}

/* ---------------------------------------------------------------- finding a block */

/* Whether a good dynamic block begins at `bit`: its header and codes, its symbols to its end,
 * and a good header after it. */
static int
block_at(struct dec *d, const uint8_t *in, uint32_t size, uint32_t bit)
{
	struct bits *b = &d->b;
	uint32_t kind;

	bits_at(b, in, size, bit);
	if (take(b, 1) || take(b, 2) != 2 || dynamic_codes(b, &d->lit, &d->dist, &d->meta))
		return 0;
	for (;;) {
		int symbol = code(b, &d->lit), ds;

		if (symbol < 0 || symbol > 285 || b->over)
			return 0;
		if (symbol < 256)
			continue;
		if (symbol == 256)
			break;
		if (length_extra[symbol - 257])
			take(b, length_extra[symbol - 257]);
		ds = code(b, &d->dist);
		if (ds < 0 || ds >= 30)
			return 0;
		if (distance_extra[ds])
			take(b, distance_extra[ds]);
	}
	take(b, 1);
	kind = take(b, 2);
	if (kind == 3)
		return 0;
	if (kind == 2 && dynamic_codes(b, &d->lit, &d->dist, &d->meta))
		return 0;
	return !b->over;
}

/*
 * How far to step from a bit whose next 13 bits are x before a block's header could begin: 0
 * when x itself could be one (not final, dynamic, at most 286 and 30 codes). A bit further on
 * keeps fewer of the 13 known; a field not wholly known may still be good.
 */
static uint8_t skip[1 << 13];
static uint16_t kraft[1 << 12];	/* four code lengths of 3 bits: what they take of 128 */

static void
skip_init(void)
{
	uint32_t x, k, b;

	if (skip[0])
		return;
	for (x = 0; x < 1u << 13; x++) {
		for (k = 0; k < 13; k++) {
			uint32_t w = x >> k, known = 13 - k, good = 1;

			for (b = 0; b < 3 && b < known; b++)
				good &= ((w >> b) & 1) == ((4u >> b) & 1);
			if (known >= 8 && ((w >> 3) & 31) > 29)
				good = 0;
			if (known >= 13 && ((w >> 8) & 31) > 29)
				good = 0;
			if (good)
				break;
		}
		skip[x] = (uint8_t)k;
	}
	for (x = 0; x < 1u << 12; x++) {
		uint32_t sum = 0;

		for (b = 0; b < 4; b++)
			sum += (x >> (3 * b)) & 7 ? 128u >> ((x >> (3 * b)) & 7) : 0;
		kraft[x] = (uint16_t)sum;
	}
}

/* The first good dynamic block in bits [from, to); ~0u if none. */
static uint32_t
find_block(struct dec *d, const uint8_t *in, uint32_t size, uint32_t from, uint32_t to)
{
	uint32_t bit = from;

	if (to > (size > 16 ? size - 16 : 0) * 8)
		to = (size > 16 ? size - 16 : 0) * 8;
	while (bit < to) {
		const uint8_t *at = in + (bit >> 3);
		uint32_t x = ((at[0] | at[1] << 8 | (uint32_t)at[2] << 16) >> (bit & 7)) & 0x1fff, step = skip[x];

		if (step) {
			bit += step;
			continue;
		}
		{
			/* the code lengths' code must be whole: 3 bits each from bit 17, four at a time */
			uint32_t p = bit + 17;
			int hclen = (int)(((at[1] | at[2] << 8) >> ((bit & 7) + 5)) & 15) + 4, i, left = 128;

			for (i = 0; i < hclen && left >= 0; i += 4, p += 12) {
				const uint8_t *q = in + (p >> 3);
				uint32_t g = ((q[0] | q[1] << 8 | (uint32_t)q[2] << 16) >> (p & 7)) & 0xfff;

				if (hclen - i < 4)
					g &= (1u << (3 * (hclen - i))) - 1;
				left -= kraft[g];
			}
			if (!left && block_at(d, in, size, bit))
				return bit;
		}
		bit++;
	}
	return ~0u;
}

/* ---------------------------------------------------------------- the parts */

struct part {
	uint32_t start, end, len, crc;	/* bits; bytes made */
	uint32_t marked, fin, bad, bytes;	/* marked ~0u: no marks in its last WINDOW */
	uint8_t head[16], tail[16];
};
/* what a part's second job makes, apart from what the first core reads while it runs */
struct made { uint32_t crc, bad, pad[2]; uint8_t head[16], tail[16]; };

struct stream {
	const uint8_t *in;
	uint32_t in_size, parts, cap;
	uint8_t *out;
	struct part *p;		/* parts' records, 64 bytes each */
	struct made *made;	/* 48 bytes each */
	struct dec *dec;	/* a decoder for each part */
	uint16_t *rings;	/* WINDOW values each */
	uint8_t *after;		/* WINDOW bytes each: the window after the part */
	uint32_t *offset, *wlen;
	const uint8_t **window;
	uint32_t *next;		/* the part that follows each in the chain */
};

/* Inflates d from where its bits are to the end of the stream or to `stop` (a block's start),
 * the parts after p whose start it passes not being blocks after all. */
static void
inflate_to(struct stream *s, struct dec *d, uint32_t k, struct part *p, int wide)
{
	uint32_t j = k + 1;

	for (;;) {
		uint32_t at = bits_pos(&d->b);
		int r;

		while (j < s->parts && (s->p[j].start == ~0u || s->p[j].start < at))
			j++;
		if (j < s->parts && s->p[j].start == at)
			break;
		r = block(d, wide);
		if (r != BLOCK_OK) {
			p->bad = r == BLOCK_FULL ? 2 : 1;
			break;
		}
		if (d->last) {
			p->fin = 1;
			break;
		}
	}
	p->end = bits_pos(&d->b);
}

static void
find_job(void *ctx, uint32_t from, uint32_t to)
{
	struct stream *s = ctx;
	uint32_t k;

	for (k = from; k < to; k++) {
		uint32_t lo = (uint32_t)((uint64_t)s->in_size * k / s->parts) * 8, hi = (uint32_t)((uint64_t)s->in_size * (k + 1) / s->parts) * 8;

		s->p[k].start = k ? find_block(&s->dec[k], s->in, s->in_size, lo, hi) : 0;
	}
}

static void
first_job(void *ctx, uint32_t from, uint32_t to)
{
	struct stream *s = ctx;
	uint32_t k;

	for (k = from; k < to; k++) {
		struct part *p = &s->p[k];
		struct dec *d = &s->dec[k];

		if (p->start == ~0u)
			continue;
		bits_at(&d->b, s->in, s->in_size, p->start);
		if (k == 0) {
			d->out = s->out, d->n = 0, d->cap = s->cap, d->wlen = 0, d->window = NULL;
			inflate_to(s, d, k, p, 0);
			p->len = d->n;
			p->crc = media_crc32(0, s->out, d->n);
			continue;
		}
		d->ring = s->rings + (size_t)k * WINDOW, d->rn = 0, d->marked = 0;
		inflate_to(s, d, k, p, 1);
		p->len = d->rn, p->marked = d->marked;
		/* the window after it as bytes where they are known (all of it when no mark is left) */
		if (p->len >= WINDOW) {
			uint8_t *a = s->after + (size_t)k * WINDOW;
			uint32_t i;

			for (i = 0; i < WINDOW; i++)
				a[i] = (uint8_t)d->ring[(p->len - WINDOW + i) & (WINDOW - 1)];
			p->bytes = 1;
			if (p->len - p->marked >= WINDOW)
				p->marked = ~0u;
		}
	}
}

/* (one stream at a time, from a program's first core: a job's argument is a part's number) */
static struct stream *inflating;

/* Part k again, its window known, into its place. */
static uint32_t
second_job(uint32_t k, uint32_t unused)
{
	struct stream *s = inflating;
	struct part *p = &s->p[k], q = *p;
	struct made *m = &s->made[k];
	struct dec *d = &s->dec[k];
	uint8_t *at = s->out + s->offset[k];

	(void)unused;
	bits_at(&d->b, s->in, s->in_size, q.start);
	d->out = at, d->n = 0, d->cap = q.len, d->window = s->window[k], d->wlen = s->wlen[k];
	inflate_to(s, d, k, &q, 0);
	m->bad = d->n != q.len || q.bad;
	m->crc = media_crc32(0, at, d->n);
	memcpy(m->head, at, q.len < 16 ? q.len : 16);
	if (q.len >= 16)
		memcpy(m->tail, at + q.len - 16, 16);
	return 0;
}

uint32_t
media_inflate_room(void)
{
	return (uint32_t)sizeof(struct dec);
}

const char *
media_inflate_one(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, uint32_t *used, uint32_t *crc,
		  void *room)
{
	struct dec *d = room;
	int r;

	crc_init();
	bits_at(&d->b, in, in_size, 0);
	d->out = out, d->n = 0, d->cap = out_size, d->wlen = 0, d->window = NULL;
	do
		r = block(d, 0);
	while (r == BLOCK_OK && !d->last);
	if (made)
		*made = d->n;
	if (used)
		*used = (bits_pos(&d->b) + 7) >> 3;
	if (crc)
		*crc = media_crc32(0, out, d->n);
	return r == BLOCK_FULL ? "larger than the room for it" : r != BLOCK_OK ? "the stream is damaged" : NULL;
}

static void *
take_room(uint8_t **room, uint32_t bytes)
{
	void *at = *room;

	*room += (bytes + 15) & ~15u;
	return at;
}

const char *
media_inflate_stream(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, uint32_t *used, uint32_t *crc,
		     int workers)
{
	struct stream s;
	mcw_for_opts o;
	uint32_t parts, k, c, total = 0, room_bytes, sum = 0, posted[8] = {0};
	uint8_t *room, *at;
	const char *why = NULL;
	int got;

	crc_init();
	skip_init();
	memset(&s, 0, sizeof s);
	s.in = in, s.in_size = in_size, s.out = out, s.cap = out_size;
	/* a part looks for a block in 32 KB, less than gzip's blocks (some 68 KB packed) are apart:
	 * every block is then the first of some part's bits, and no part makes two */
	parts = in_size / 32768;
	got = m_workers_begin(parts > 1 ? (int)parts - 1 < workers ? (int)parts - 1 : workers : 0);
	parts = parts < (uint32_t)got + 1 ? parts : (uint32_t)got + 1;
	if (parts < 1)
		parts = 1;
	s.parts = parts;
	room_bytes = parts * (uint32_t)(sizeof(struct part) + sizeof(struct made) + sizeof(struct dec) + 16 * 4 + 64) + (parts - 1) * WINDOW * 3 + 64;
	if (!(room = m_map(room_bytes))) {
		m_workers_end();
		return "not enough memory";
	}
	at = room;
	s.p = take_room(&at, parts * (uint32_t)sizeof(struct part));
	s.made = take_room(&at, parts * (uint32_t)sizeof(struct made));
	s.dec = take_room(&at, parts * (uint32_t)sizeof(struct dec));
	s.offset = take_room(&at, parts * 4);
	s.wlen = take_room(&at, parts * 4);
	s.window = take_room(&at, parts * (uint32_t)sizeof(void *));
	s.next = take_room(&at, parts * 4);
	s.rings = (uint16_t *)take_room(&at, (parts - 1) * WINDOW * 2) - WINDOW;	/* (part 0 has none) */
	s.after = (uint8_t *)take_room(&at, (parts - 1) * WINDOW) - WINDOW;
	memset(s.p, 0, parts * sizeof(struct part));
	memset((void *)s.window, 0, parts * sizeof(void *));

	memset(&o, 0, sizeof o);
	o.workers = got;
	o.grain = 1;
	if (parts > 1)
		mcw_for_opt(parts, find_job, &s, &o);
	mcw_for_opt(parts, first_job, &s, &o);
	mcw_for_end();
	inflating = &s;

	/* the chain: from part 0, each to the part that starts where it ended; a part's worker
	 * makes it again as soon as its window is known (part k on worker k) */
	for (c = 0;;) {
		struct part *p = &s.p[c];

		if (p->bad) {
			why = p->bad == 2 ? "larger than the room for it" : "the stream is damaged";
			break;
		}
		s.offset[c] = total;
		total += p->len;
		if (total > out_size) {
			why = "larger than the room for it";
			break;
		}
		if (p->fin)
			break;
		for (k = c + 1; k < parts && s.p[k].start != p->end; k++)
			;
		if (k >= parts) {
			why = "the stream ends before its last block";
			break;
		}
		s.next[c] = k;
		/* its window: the 32 KB before it */
		if (c == 0) {
			s.wlen[k] = p->len < WINDOW ? p->len : WINDOW;
			s.window[k] = s.out + p->len - s.wlen[k];
		} else if (p->bytes) {
			/* the bytes its core knew; the marks among them from the window before it */
			const uint8_t *w = s.window[c];
			const uint32_t *pairs = (const uint32_t *)(s.rings + (size_t)c * WINDOW);
			uint32_t lo = WINDOW - s.wlen[c], off = p->len - WINDOW, j, bad = 0;
			uint8_t *a = s.after + (size_t)c * WINDOW;

			/* (the serial part of the whole stream: a value of the pair at a time, no loop) */
			for (j = 0; p->marked != ~0u && j < WINDOW / 2; j++) {
				uint32_t pair = pairs[j], v;

				if (!(pair & (MARK << 16 | MARK)))
					continue;
				if (pair & MARK) {
					v = pair & 0x7fff;
					if (v < lo) {
						bad = 1;
						break;
					}
					a[(2 * j - off) & (WINDOW - 1)] = w[v - lo];
				}
				if (pair & MARK << 16) {
					v = pair >> 16 & 0x7fff;
					if (v < lo) {
						bad = 1;
						break;
					}
					a[(2 * j + 1 - off) & (WINDOW - 1)] = w[v - lo];
				}
			}
			if (bad) {
				why = "the stream is damaged";
				break;
			}
			s.window[k] = a, s.wlen[k] = WINDOW;
		} else {
			/* the first core resolves its marks from the window before it */
			const uint8_t *w = s.window[c];
			uint32_t wl = s.wlen[c], keep = p->len >= WINDOW ? 0 : (WINDOW - p->len < wl ? WINDOW - p->len : wl);
			uint32_t from = p->len >= WINDOW ? p->len - WINDOW : 0, i;
			uint8_t *a = s.after + (size_t)c * WINDOW;
			const uint16_t *ring = s.rings + (size_t)c * WINDOW;

			memmove(a, w + wl - keep, keep);
			for (i = from; i < p->len; i++) {
				uint32_t v = ring[i & (WINDOW - 1)];

				if (v & MARK) {
					v &= 0x7fff;
					if (v < WINDOW - wl) {
						why = "the stream is damaged";
						break;
					}
					v = w[v - (WINDOW - wl)];
				}
				a[keep + i - from] = (uint8_t)v;
			}
			if (why)
				break;
			s.window[k] = a, s.wlen[k] = keep + p->len - from;
		}
		s.offset[k] = total;
		if (s.p[k].len > out_size - total) {
			why = "larger than the room for it";
			break;
		}
		mcw_post((int)k, second_job, k, 0);
		posted[k >> 5] |= 1u << (k & 31);
		c = k;
	}
	for (k = 1; k < parts; k++)
		if (posted[k >> 5] >> (k & 31) & 1)
			mcw_wait((int)k);
	if (!why) {
		uint32_t last = c;

		/* the texels two parts share, as each made them; the CRC joined */
		for (c = 0;; c = s.next[c]) {
			struct part *p = &s.p[c];
			struct made *m = &s.made[c];

			if (c && m->bad) {
				why = "the stream is damaged";
				break;
			}
			if (c) {
				memcpy(s.out + s.offset[c], m->head, p->len < 16 ? p->len : 16);
				if (p->len >= 16)
					memcpy(s.out + s.offset[c] + p->len - 16, m->tail, 16);
			}
			sum = c ? media_crc32_join(sum, m->crc, p->len) : p->crc;
			if (c == last)
				break;
		}
		if (used)
			*used = (s.p[last].end + 7) >> 3;
	}
	if (made)
		*made = why ? 0 : total;
	if (crc)
		*crc = sum;
	m_unmap(room, room_bytes);
	m_workers_end();
	return why;
}
