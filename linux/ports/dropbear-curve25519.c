/* Dropbear's X25519 and Ed25519 (curve25519.h) by Monocypher 4.0.2 (CC0 or BSD-2-Clause), in place
 * of its TweetNaCl: one scalar multiplication there was 12 s of the machine's time (dropbear.sh). */
#include "includes.h"
#include "dbrandom.h"
#include "curve25519.h"

#if DROPBEAR_CURVE25519_DEP || DROPBEAR_ED25519
#undef MIN
#include "monocypher.c"

void dropbear_curve25519_scalarmult(unsigned char *q, const unsigned char *n, const unsigned char *p)
{
	crypto_x25519(q, n, p);
}

#if DROPBEAR_ED25519
/* (one file with monocypher.c: its macros again, and a static of the same name) */
#undef FOR
#undef COPY
#undef ZERO
#undef WIPE_CTX
#undef WIPE_BUFFER
#undef MIN
#define hash_reduce ed25519_hash_reduce
#include "monocypher-ed25519.c"

/* Dropbear keeps the 32-byte seed as the secret; Monocypher wants the seed and the public key */
void dropbear_ed25519_make_key(unsigned char *pk, unsigned char *sk)
{
	unsigned char seed[32], full[64];

	genrandom(sk, 32);
	memcpy(seed, sk, 32);
	crypto_ed25519_key_pair(full, pk, seed);
	crypto_wipe(full, sizeof(full));
}

void dropbear_ed25519_sign(const unsigned char *m, unsigned long mlen,
			  unsigned char *s, unsigned long *slen,
			  const unsigned char *sk, const unsigned char *pk)
{
	unsigned char full[64];

	memcpy(full, sk, 32);
	memcpy(full + 32, pk, 32);
	crypto_ed25519_sign(s, full, m, mlen);
	crypto_wipe(full, sizeof(full));
	*slen = 64;
}

#if DROPBEAR_SIGNKEY_VERIFY
int dropbear_ed25519_verify(const unsigned char *m, unsigned long mlen,
			    const unsigned char *s, unsigned long slen,
			    const unsigned char *pk)
{
	if (slen < 64)
		return -1;
	return crypto_ed25519_check(s, pk, m, mlen) == 0 ? 0 : -1;
}
#endif
#endif /* DROPBEAR_ED25519 */
#endif
