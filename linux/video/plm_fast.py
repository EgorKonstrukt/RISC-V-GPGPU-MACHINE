"""pl_mpeg.h: bits and codes read from a word of the stream, not a bit at a time.

    python3 plm_fast.py pl_mpeg.h
The header reads a variable-length code by walking its table a bit a turn, and each bit is a
call that asks whether the buffer has it and loads its byte again: some thirty instructions a
bit, a third of a picture. Here four bytes are taken once and the same table is walked on
them in a register (the same code, so the same pictures); near the buffer's end, and for a
count the word cannot hold, the header's own way is kept. Idempotent.
"""
import sys

path = sys.argv[1]
s = open(path).read()
if 'ShaderEmu: a word of the stream' in s:
    sys.exit()


def replace(old, new):
    global s
    if s.count(old) != 1:
        sys.exit('plm_fast.py: the decoder has changed')
    s = s.replace(old, new)


replace('''int plm_buffer_read(plm_buffer_t *self, int count) {
	if (!plm_buffer_has(self, count)) {
		return 0;
	}
''', '''/* ShaderEmu: a word of the stream from the bit it is at, when four bytes are there */
#define PLM_WORD_AT(self, at, p) \\
	(((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (uint32_t)(p)[3]) << ((at) & 7))

int plm_buffer_read(plm_buffer_t *self, int count) {
	size_t at = self->bit_index;

	if (count > 0 && count <= 24 && (at >> 3) + 4 <= self->length) {
		const uint8_t *p = self->bytes + (at >> 3);

		self->bit_index = at + count;
		return (int)(PLM_WORD_AT(self, at, p) >> (32 - count));
	}
	if (!plm_buffer_has(self, count)) {
		return 0;
	}
''')
replace('''int16_t plm_buffer_read_vlc(plm_buffer_t *self, const plm_vlc_t *table) {
	plm_vlc_t state = {0, 0};
''', '''int16_t plm_buffer_read_vlc(plm_buffer_t *self, const plm_vlc_t *table) {
	plm_vlc_t state = {0, 0};
	size_t at = self->bit_index;

	if ((at >> 3) + 4 <= self->length) {
		const uint8_t *p = self->bytes + (at >> 3);
		uint32_t bits = PLM_WORD_AT(self, at, p);
		int taken = 0;

		do {
			state = table[state.index + (bits >> 31)];
			bits <<= 1;
			taken++;
		} while (state.index > 0 && taken < 24);
		if (state.index <= 0) {
			self->bit_index = at + taken;
			return state.value;
		}
		state.index = 0;
	}
''')
# A coefficient's code, its sign and the block's end from one word of the stream.
replace('''		int run = 0;
		uint16_t coeff = plm_buffer_read_vlc_uint(self->buffer, PLM_VIDEO_DCT_COEFF);

		if ((coeff == 0x0001) && (n > 0) && (plm_buffer_read(self->buffer, 1) == 0)) {
			// end_of_block
			break;
		}
		if (coeff == 0xffff) {
''', '''		int run = 0;
		uint16_t coeff = 0;
		plm_buffer_t *bf = self->buffer;
		size_t at = bf->bit_index;
		int whole = 0;

		if ((at >> 3) + 4 <= bf->length) {
			const uint8_t *p = bf->bytes + (at >> 3);
			uint32_t bits = PLM_WORD_AT(bf, at, p);
			plm_vlc_t state = {0, 0};
			int taken = 0;

			do {
				state = ((const plm_vlc_t *)PLM_VIDEO_DCT_COEFF)[state.index + (bits >> 31)];
				bits <<= 1;
				taken++;
			} while (state.index > 0 && taken < 23);
			if (state.index <= 0) {
				whole = 1;
				coeff = (uint16_t)state.value;
				if ((coeff == 0x0001) && (n > 0)) {
					int more = bits >> 31;

					bits <<= 1;
					taken++;
					if (!more) {
						bf->bit_index = at + taken;
						break;
					}
				}
				if (coeff != 0xffff) {
					run = coeff >> 8;
					level = coeff & 0xff;
					if (bits >> 31) {
						level = -level;
					}
					taken++;
				}
				bf->bit_index = at + taken;
			}
		}
		if (!whole) {
			coeff = plm_buffer_read_vlc_uint(self->buffer, PLM_VIDEO_DCT_COEFF);
			if ((coeff == 0x0001) && (n > 0) && (plm_buffer_read(self->buffer, 1) == 0)) {
				// end_of_block
				break;
			}
		}
		if (whole && coeff != 0xffff) {
			// (its run, level and sign were in the word)
		}
		else if (coeff == 0xffff) {
''')

# The transform only where the block has coefficients: a column with none below its first
# row comes out as that row's value eight times, and so does a row with none beside its first.
replace('''		int de_zig_zagged = PLM_VIDEO_ZIG_ZAG[n];
		n++;
''', '''		int de_zig_zagged = PLM_VIDEO_ZIG_ZAG[n];
		n++;
		cols |= 1 << (de_zig_zagged & 7);
		tall |= (de_zig_zagged >= 8) << (de_zig_zagged & 7);
''')
replace('''	// Decode AC coefficients (+DC for non-intra)
	int level = 0;
''', '''	// Decode AC coefficients (+DC for non-intra)
	int level = 0;
	int cols = 1, tall = 0;	// columns with a coefficient, and with one below the first row
''')
if s.count('plm_video_idct(s);') != 2 or s.count('void plm_video_idct(int *block);') != 1:
    sys.exit('plm_fast.py: the decoder has changed')
s = s.replace('plm_video_idct(s);', 'plm_video_idct(s, cols, tall);')
s = s.replace('void plm_video_idct(int *block);', 'void plm_video_idct(int *block, int cols, int tall);')
replace('''void plm_video_idct(int *block) {
	int
		b1, b3, b4, b6, b7, tmp1, tmp2, m0,
		x0, x1, x2, x3, x4, y3, y4, y5, y6, y7;

	// Transform columns
	for (int i = 0; i < 8; ++i) {
''', '''void plm_video_idct(int *block, int cols, int tall) {
	int
		b1, b3, b4, b6, b7, tmp1, tmp2, m0,
		x0, x1, x2, x3, x4, y3, y4, y5, y6, y7;
	int rows = tall ? 64 : 8;

	// Transform columns
	for (int i = 0; i < 8; ++i) {
		if (!(tall >> i & 1)) {
			if (cols >> i & 1) {
				m0 = block[i];
				block[1 * 8 + i] = m0; block[2 * 8 + i] = m0; block[3 * 8 + i] = m0; block[4 * 8 + i] = m0;
				block[5 * 8 + i] = m0; block[6 * 8 + i] = m0; block[7 * 8 + i] = m0;
			}
			continue;
		}
''')
replace('''	// Transform rows
	for (int i = 0; i < 64; i += 8) {
''', '''	// Transform rows
	if (!(cols & 0xfe)) {
		for (int i = 0; i < 64; i += 8) {
			m0 = (block[i] + 128) >> 8;
			block[i] = m0; block[i + 1] = m0; block[i + 2] = m0; block[i + 3] = m0;
			block[i + 4] = m0; block[i + 5] = m0; block[i + 6] = m0; block[i + 7] = m0;
		}
		return;
	}
	for (int i = 0; i < rows; i += 8) {
''')
replace('''		block[7 + i] = (y4 - b7 + 128) >> 8;
	}
}
''', '''		block[7 + i] = (y4 - b7 + 128) >> 8;
	}
	// (no coefficient below the first row: eight rows the same, the first one's)
	for (int i = rows; i < 64; i += 8) {
		memcpy(block + i, block, 8 * sizeof(int));
	}
}
''')
# A block whose vector points outside the picture is not predicted: it keeps what its place
# had, which was the picture three before. With pictures decoded where they are kept (mpeg.c)
# that picture lies `stale` bytes from this one, and the block is brought from there.
replace('''	uint8_t *frames_data;
''', '''	uint8_t *frames_data;
	long stale;
''')
replace('''	if (si > max_address || di > max_address) {
		return; // corrupt video
	}
''', '''	if (si > max_address || di > max_address) {
		if (self->stale && di <= max_address) {
			for (int row = 0; row < block_size; row++) {
				memcpy(d + di + row * dw, d + self->stale + di + row * dw, block_size);
			}
		}
		return; // corrupt video
	}
''')
# A block predicted from whole pixels is a copy: rows of words, not a byte a turn.
replace('''		PLM_MB_CASE(0, 0, 0, (s[si]));
''', '''		case 0:
			for (int row = 0; row < block_size; row++) {
				memcpy(d + di + row * dw, s + si + row * dw, block_size);
			}
			break;
''')
open(path, 'w').write(s)
print('plm_fast.py: done')
