"""dr_flac.h: a subframe is predicted in 32 bits whenever its own coefficients allow it.

    python3 flac_bound.py dr_flac.h
The format's test is on widths (sample bits + coefficient bits + log2 of the order over 32
means 64-bit sums), which a 16-bit file meets in most subframes: a fifth of the decoder on a
machine with 32-bit registers. The sum of the coefficients' sizes times the largest sample
bounds every partial sum; under 2^31, 32-bit arithmetic gives the same samples. Idempotent.
"""
import sys

path = sys.argv[1]
s = open(path).read()
if 'drflac__needs_64_bits' in s:
    sys.exit()
call = 'if (drflac__use_64_bit_prediction(bitsPerSample, lpcOrder, lpcPrecision)) {\n        while (pSamplesOut < pSamplesOutEnd) {'
if s.count(call) != 1:
    sys.exit('flac_bound.py: the decoder has changed')
s = s.replace(call, call.replace('drflac__use_64_bit_prediction(bitsPerSample, lpcOrder, lpcPrecision)',
                                 'drflac__needs_64_bits(bitsPerSample, lpcOrder, lpcPrecision, coefficients)'))
head = 'static drflac_bool32 drflac__decode_samples_with_residual__rice__scalar(drflac_bs* bs,'
if s.count(head) != 1:
    sys.exit('flac_bound.py: the decoder has changed')
s = s.replace(head, '''/* ShaderEmu: every partial sum is at most the coefficients' sizes times 2^(bits - 1) */
static DRFLAC_INLINE drflac_bool32 drflac__needs_64_bits(drflac_uint32 bitsPerSample, drflac_uint32 order, drflac_uint32 precision, const drflac_int32* coefficients)
{
    drflac_uint32 i, sizes = 0;

    if (!drflac__use_64_bit_prediction(bitsPerSample, order, precision)) {
        return DRFLAC_FALSE;
    }
    if (bitsPerSample > 24 || order > 32) {
        return DRFLAC_TRUE;
    }
    for (i = 0; i < order; i++) {
        sizes += (drflac_uint32)(coefficients[i] < 0 ? -coefficients[i] : coefficients[i]);
    }
    return sizes >= ((drflac_uint32)1 << (32 - bitsPerSample));
}

''' + head)
open(path, 'w').write(s)
print('flac_bound.py: done')
