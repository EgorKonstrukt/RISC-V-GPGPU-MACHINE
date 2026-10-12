"""pl_mpeg.h: the sound's synthesis window four blocks of products at a time.

    python3 plm_window.py pl_mpeg.h
Every output sample is a sum of 16 products; the header adds one product a turn to a sum kept
in memory (load, add, store). Four a turn with the sum in a register are the same additions
in the same order, so the same samples, for a quarter fewer instructions. Idempotent.
"""
import sys

path = sys.argv[1]
s = open(path).read()
if 'ShaderEmu: four blocks' in s:
    sys.exit()
old = '''					memset(self->U, 0, sizeof(self->U));

					int d_index = 512 - (self->v_pos >> 1);
					int v_index = (self->v_pos % 128) >> 1;
					while (v_index < 1024) {
						for (int i = 0; i < 32; ++i) {
							self->U[i] += self->D[d_index++] * self->V[ch][v_index++];
						}

						v_index += 128 - 32;
						d_index += 64 - 32;
					}

					d_index -= (512 - 32);
					v_index = (128 - 32 + 1024) - v_index;
					while (v_index < 1024) {
						for (int i = 0; i < 32; ++i) {
							self->U[i] += self->D[d_index++] * self->V[ch][v_index++];
						}

						v_index += 128 - 32;
						d_index += 64 - 32;
					}
'''
new = '''					memset(self->U, 0, sizeof(self->U));

					// ShaderEmu: four blocks of products a turn, each sum in a register meanwhile
					const float *dq[20], *vq[20];
					int blocks = 0;
					int d_index = 512 - (self->v_pos >> 1);
					int v_index = (self->v_pos % 128) >> 1;
					while (v_index < 1024) {
						dq[blocks] = self->D + d_index;
						vq[blocks++] = self->V[ch] + v_index;
						v_index += 128;
						d_index += 64;
					}

					d_index -= (512 - 32);
					v_index = (128 - 32 + 1024) - v_index;
					while (v_index < 1024) {
						dq[blocks] = self->D + d_index;
						vq[blocks++] = self->V[ch] + v_index;
						v_index += 128;
						d_index += 64;
					}
					int block = 0;
					for (; block + 4 <= blocks; block += 4) {
						const float *d0 = dq[block], *d1 = dq[block + 1], *d2 = dq[block + 2], *d3 = dq[block + 3];
						const float *v0 = vq[block], *v1 = vq[block + 1], *v2 = vq[block + 2], *v3 = vq[block + 3];
						for (int i = 0; i < 32; ++i) {
							float u = self->U[i];
							u += d0[i] * v0[i];
							u += d1[i] * v1[i];
							u += d2[i] * v2[i];
							u += d3[i] * v3[i];
							self->U[i] = u;
						}
					}
					for (; block < blocks; block++) {
						for (int i = 0; i < 32; ++i) {
							self->U[i] += dq[block][i] * vq[block][i];
						}
					}
'''
if s.count(old) != 1:
    sys.exit('plm_window.py: the synthesis loop has changed')
open(path, 'w').write(s.replace(old, new))
print('plm_window.py: done')
