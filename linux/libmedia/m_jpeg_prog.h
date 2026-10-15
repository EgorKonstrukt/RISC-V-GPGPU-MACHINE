/* Progressive JPEG (part of m_jpeg.c): the scans read into the coefficients each on a core of its own,
 * a scan a few rows behind the ones before it of its components, and the strips behind them all. */

/* The scan's bits after the codes: to the next marker that is not a restart. */
static uint32_t
after_scan(const struct jpg *j, const struct scan *s)
{
	const uint8_t *at = s->at;

	while (at + 1 < j->file + j->size && !(at[0] == 0xff && at[1] != 0 && at[1] != 0xff && (at[1] < 0xd0 || at[1] > 0xd7)))
		at++;
	return (uint32_t)(at - j->file);
}

static void
prog_block(const struct huff *dct, const struct huff *act, struct scan *s, int16_t *data, int ss, int se, int ah, int al, int *pred, int *eobrun)
{
	int k, r, sz, rs;

	if (ss == 0) {
		if (ah == 0) {
			int bits = huff_symbol(s, dct);

			*pred += extend(scan_bits(s, bits), bits);
			data[0] = (int16_t)(*pred * (1 << al));
		} else if (scan_bits(s, 1)) {
			data[0] = (int16_t)(data[0] + (1 << al));
		}
		return;
	}
	if (ah == 0) {
		if (*eobrun) {
			--*eobrun;
			return;
		}
		k = ss;
		do {
			rs = huff_symbol(s, act);
			sz = rs & 15, r = rs >> 4;
			if (!sz) {
				if (r < 15) {
					*eobrun = 1 << r;
					if (r)
						*eobrun += scan_bits(s, r);
					--*eobrun;
					break;
				}
				k += 16;
			} else {
				k += r;
				data[zigzag[k < 80 ? k : 79]] = (int16_t)(extend(scan_bits(s, sz), sz) * (1 << al));
				k++;
			}
		} while (k <= se);
		return;
	}
	/* a refining scan: a bit for each coefficient that is not zero, new ones of +-1 << al among the zeros */
	{
		int bit = 1 << al;

		if (*eobrun) {
			--*eobrun;
			for (k = ss; k <= se; k++) {
				int16_t *p = &data[zigzag[k]];

				if (*p && scan_bits(s, 1) && !(*p & bit))
					*p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
			}
			return;
		}
		k = ss;
		do {
			rs = huff_symbol(s, act);
			sz = rs & 15, r = rs >> 4;
			if (!sz) {
				if (r < 15) {
					*eobrun = (1 << r) - 1;
					if (r)
						*eobrun += scan_bits(s, r);
					r = 64;	/* the rest of the block only refined */
				}
			} else {
				sz = scan_bits(s, 1) ? bit : -bit;
			}
			while (k <= se) {
				int16_t *p = &data[zigzag[k++]];

				if (*p) {
					if (scan_bits(s, 1) && !(*p & bit))
						*p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
				} else {
					if (r == 0) {
						*p = (int16_t)sz;
						break;
					}
					--r;
				}
			}
		} while (k <= se && !s->bad);
	}
}

/* A scan as the segments before it left things: where its codes are, its tables, what it reads. */
#define PROG_SCANS 64
struct prog_one {
	uint32_t data, set;
	int restart, n, comps[4], dc[4], ac[4], ss, se, ah, al;
	unsigned mask;		/* its components */
};
static struct prog_one prog[PROG_SCANS];
static int prog_count;
static struct huff (*prog_sets)[2][4];	/* the tables as each scan found them */
static volatile uint32_t *prog_done;	/* each scan's picture rows read, 16 bytes apart */

static const char *
prog_header(struct jpg *j, uint32_t sos, struct prog_one *o)
{
	const uint8_t *f = j->file;
	int n = f[sos + 4], i, c;

	if (n < 1 || n > 4)
		return "a JPEG picture that is damaged";
	o->n = n, o->mask = 0;
	for (i = 0; i < n; i++) {
		o->comps[i] = -1;
		for (c = 0; c < j->components; c++)
			if (j->comp[c].id == f[sos + 5 + 2 * i]) {
				o->comps[i] = c, o->mask |= 1u << c;
				o->dc[i] = f[sos + 6 + 2 * i] >> 4 & 3;
				o->ac[i] = f[sos + 6 + 2 * i] & 3;
			}
		if (o->comps[i] < 0)
			return "a JPEG picture that is damaged";
	}
	o->ss = f[sos + 5 + 2 * n], o->se = f[sos + 6 + 2 * n], o->ah = f[sos + 7 + 2 * n] >> 4, o->al = f[sos + 7 + 2 * n] & 15;
	if (o->se > 63 || o->ss > o->se || (o->ss == 0 && o->se != 0) || (o->ss && n != 1))
		return "a JPEG picture that is damaged";
	o->data = sos + 2 + (uint32_t)(f[sos + 2] << 8 | f[sos + 3]);
	o->restart = j->restart;
	return NULL;
}

/* Until every scan before k that reads one of its components has read the picture's first `rows`. */
static int
prog_wait(struct jpg *j, int k, uint32_t rows)
{
	int t;

	for (t = 0; t < k; t++)
		if ((prog[t].mask & prog[k].mask) && !m_wait(&prog_done[t * 4], rows, j->failed))
			return 0;
	return 1;
}

static uint32_t
prog_scan(uint32_t k, uint32_t unused)
{
	struct jpg *j = J;
	const struct prog_one *o = &prog[k];
	const struct huff (*h)[4] = prog_sets[o->set];
	int pred[4] = {0, 0, 0, 0}, eobrun = 0, left = o->restart, i, c;
	struct scan s;

	(void)unused;
	memset(&s, 0, sizeof s);
	s.at = j->file + o->data;
	s.end = j->file + j->size;
#define PROG_RESTART() do { \
		if (o->restart && left-- == 0) { \
			s.count = 0, s.hold = 0; \
			while (s.at + 1 < s.end && !(s.at[0] == 0xff && s.at[1] >= 0xd0 && s.at[1] <= 0xd7)) \
				s.at++; \
			s.at += 2; \
			pred[0] = pred[1] = pred[2] = pred[3] = 0; \
			eobrun = 0; \
			left = o->restart - 1; \
		} \
	} while (0)
	if (o->n == 1) {
		/* one component: its own blocks in order, as far as its part of the picture goes */
		int cc = o->comps[0], bw = ((j->w * j->comp[cc].h + j->hmax - 1) / j->hmax + 7) / 8;
		int bh = ((j->h * j->comp[cc].v + j->vmax - 1) / j->vmax + 7) / 8, x, y;
		uint32_t row = (uint32_t)(8 * j->vmax / j->comp[cc].v);

		for (y = 0; y < bh && !s.bad; y++) {
			if (!prog_wait(j, (int)k, (uint32_t)(y + 1) * row))
				break;
			for (x = 0; x < bw && !s.bad; x++) {
				PROG_RESTART();
				prog_block(&h[0][o->dc[0]], &h[1][o->ac[0]], &s, j->comp[cc].coef + ((uint32_t)y * (uint32_t)j->comp[cc].bw + (uint32_t)x) * 64,
					   o->ss, o->se, o->ah, o->al, &pred[0], &eobrun);
			}
			prog_done[k * 4] = (uint32_t)(y + 1) * row;
		}
	} else {
		int mx, my, by, bx;

		for (my = 0; my < j->mcus_y && !s.bad; my++) {
			if (!prog_wait(j, (int)k, (uint32_t)(my + 1) * (uint32_t)j->mcu_h))
				break;
			for (mx = 0; mx < j->mcus_x && !s.bad; mx++) {
				PROG_RESTART();
				for (i = 0; i < o->n; i++) {
					c = o->comps[i];
					for (by = 0; by < j->comp[c].v; by++)
						for (bx = 0; bx < j->comp[c].h; bx++) {
							uint32_t b = (uint32_t)(my * j->comp[c].v + by) * (uint32_t)j->comp[c].bw +
								     (uint32_t)(mx * j->comp[c].h + bx);

							prog_block(&h[0][o->dc[i]], &h[1][o->ac[i]], &s, j->comp[c].coef + b * 64, o->ss, o->se, o->ah, o->al,
								   &pred[i], &eobrun);
						}
				}
			}
			prog_done[k * 4] = (uint32_t)(my + 1) * (uint32_t)j->mcu_h;
		}
	}
#undef PROG_RESTART
	if (s.bad)
		*j->failed = 1;
	prog_done[k * 4] = 0x7fffffff;
	return 0;
}

/* Strips from..to-1 from the coefficients. */
static void
prog_strips(void *ctx, uint32_t from, uint32_t to)
{
	struct jpg *j = ctx;
	uint8_t plane[4][16 * 16];
	int per = 16 / j->omcu_h, strip, my, mx, c, by, bx, k;

	for (strip = (int)from; strip < (int)to; strip++)
		for (my = strip * per; my < strip * per + per && my < j->mcus_y; my++)
			for (mx = 0; mx < j->mcus_x; mx++) {
				for (c = 0; c < j->components; c++)
					for (by = 0; by < j->comp[c].v; by++)
						for (bx = 0; bx < j->comp[c].h; bx++) {
							const int16_t *d = j->comp[c].coef + ((uint32_t)(my * j->comp[c].v + by) * (uint32_t)j->comp[c].bw +
											      (uint32_t)(mx * j->comp[c].h + bx)) * 64;
							const uint16_t *q = j->quant[j->comp[c].table];
							int block[64];

							for (k = 0; k < 64; k++)
								block[zigzag[k]] = d[zigzag[k]] * q[k];
							if (j->bs == 8)
								idct(block, plane[c] + by * 8 * 16 + bx * 8, 16);
							else
								block_shrunk(block, plane[c] + by * j->bs * 16 + bx * j->bs, j->bs);
						}
				mcu_pixels(j, plane, mx, my);
			}
}

/* Item k of the pipeline: scan k, or strip k - prog_count once every scan has read its rows. */
static uint32_t
prog_stage(uint32_t k, uint32_t unused)
{
	struct jpg *j = J;
	int t, per = 16 / j->omcu_h;
	uint32_t strip = k - (uint32_t)prog_count, rows = (strip * (uint32_t)per + (uint32_t)per) * (uint32_t)j->mcu_h;

	if ((int)k < prog_count)
		return prog_scan(k, unused);
	for (t = 0; t < prog_count; t++)
		if (!m_wait(&prog_done[t * 4], rows, j->failed))
			return 0;
	prog_strips(j, strip, strip + 1);
	return 0;
}

/* The end of a scan's codes: the next marker that is not a restart (no code reads 0xff then a byte not 0). */
static uint32_t
prog_end(const struct jpg *j, uint32_t at)
{
	const uint8_t *f = j->file;

	while (at + 1 < j->size && !(f[at] == 0xff && f[at + 1] != 0 && f[at + 1] != 0xff && (f[at + 1] < 0xd0 || f[at + 1] > 0xd7)))
		at++;
	return at;
}

static const char *
progressive_decode(struct jpg *j, int got)
{
	const uint8_t *f = j->file;
	uint32_t at, sets = 0;
	const char *why = NULL;
	int c, tables_new = 1;

	for (c = 0; c < j->components; c++) {
		j->comp[c].bw = j->mcus_x * j->comp[c].h;
		j->comp[c].bh = j->mcus_y * j->comp[c].v;
		if (!(j->comp[c].coef = m_map((uint32_t)j->comp[c].bw * (uint32_t)j->comp[c].bh * 128)))
			return "not enough memory for the picture";
	}
	/* the first scan's header is at scan_at less its length: from there, segment after segment */
	prog_count = 0;
	at = j->scan_at;
	{
		/* back to the SOS that header() stopped after */
		uint32_t p = 2;

		while (p + 4 <= j->size) {
			uint32_t length = (uint32_t)(f[p + 2] << 8 | f[p + 3]);

			if (f[p] == 0xff && f[p + 1] == 0xda) {
				at = p;
				break;
			}
			if (f[p] != 0xff)
				return "a JPEG picture that is damaged";
			if (f[p + 1] == 0xff) {
				p++;
				continue;
			}
			p += 2 + length;
		}
	}
	while (at + 4 <= j->size && !why) {
		int marker = f[at + 1], k, i;
		uint32_t length = (uint32_t)(f[at + 2] << 8 | f[at + 3]);

		if (f[at] != 0xff)
			return "a JPEG picture that is damaged";
		if (marker == 0xd9)
			break;
		if (marker == 0xff) {
			at++;
			continue;
		}
		if (marker == 0xda) {
			struct prog_one *o = &prog[prog_count];

			if (prog_count == PROG_SCANS)
				return "a JPEG picture of more scans than this library reads";
			if ((why = prog_header(j, at, o)) != NULL)
				break;
			/* (the tables as they are now, kept where they changed) */
			if (tables_new) {
				if (!prog_sets && !(prog_sets = m_map(sizeof *prog_sets * PROG_SCANS)))
					return "not enough memory for the picture";
				memcpy(prog_sets[sets++], j->huff, sizeof j->huff);
				tables_new = 0;
			}
			o->set = sets - 1;
			prog_count++;
			at = prog_end(j, o->data);
			continue;
		}
		if (marker == 0xc4) {
			for (k = 4; k < (int)length + 2;) {
				struct huff *h = &j->huff[f[at + k] >> 4 & 1][f[at + k] & 3];
				int n = 0;

				k++;
				for (i = 1; i <= 16; i++)
					n += h->length[i] = f[at + k++];
				for (i = 0; i < n && i < 256; i++)
					h->symbol[i] = f[at + k + i];
				k += n;
				huff_make(h);
			}
			tables_new = 1;
		} else if (marker == 0xdb) {
			for (k = 4; k < (int)length + 2;) {
				int wide = f[at + k] >> 4, t = f[at + k] & 3;

				k++;
				for (i = 0; i < 64; i++, k += wide ? 2 : 1)
					j->quant[t][i] = wide ? (uint16_t)(f[at + k] << 8 | f[at + k + 1]) : f[at + k];
			}
		} else if (marker == 0xdd) {
			j->restart = f[at + 4] << 8 | f[at + 5];
		}
		at += 2 + length;
	}
	if (why)
		return why;
	if (!prog_done && !(prog_done = m_map(PROG_SCANS * 16)))
		return "not enough memory for the picture";
	for (c = 0; c < prog_count; c++)
		prog_done[c * 4] = 0;
	*j->failed = 0;
	m_stages((uint32_t)(prog_count + j->strips), prog_stage, got);
	return *j->failed ? "the picture is damaged" : NULL;
}
