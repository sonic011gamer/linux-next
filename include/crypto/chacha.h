/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common values and helper functions for the ChaCha and XChaCha stream ciphers.
 *
 * XChaCha extends ChaCha's nonce to 192 bits, while provably retaining ChaCha's
 * security.  Here they share the same key size, tfm context, and setkey
 * function; only their IV size and encrypt/decrypt function differ.
 *
 * The ChaCha paper specifies 20, 12, and 8-round variants.  In general, it is
 * recommended to use the 20-round variant ChaCha20.  However, the other
 * variants can be needed in some performance-sensitive scenarios.  The generic
 * ChaCha code currently allows only the 20 and 12-round variants.
 */

#ifndef _CRYPTO_CHACHA_H
#define _CRYPTO_CHACHA_H

#include <linux/unaligned.h>
#include <linux/types.h>

/* 32-bit stream position, then 96-bit nonce (RFC7539 convention) */
#define CHACHA_IV_SIZE		16

#define CHACHA_KEY_SIZE		32
#define CHACHA_BLOCK_SIZE	64
#define CHACHAPOLY_IV_SIZE	12

#define CHACHA_STATE_WORDS	(CHACHA_BLOCK_SIZE / sizeof(u32))

/* 192-bit nonce, then 64-bit stream position */
#define XCHACHA_IV_SIZE		32

void chacha_block_generic(u32 *state, u8 *stream, int nrounds);
static inline void chacha20_block(u32 *state, u8 *stream)
{
	chacha_block_generic(state, stream, 20);
}

void hchacha_block_arch(const u32 *state, u32 *out, int nrounds);
void hchacha_block_generic(const u32 *state, u32 *out, int nrounds);

static inline void hchacha_block(const u32 *state, u32 *out, int nrounds)
{
	if (IS_ENABLED(CONFIG_CRYPTO_ARCH_HAVE_LIB_CHACHA))
		hchacha_block_arch(state, out, nrounds);
	else
		hchacha_block_generic(state, out, nrounds);
}

enum chacha_constants { /* expand 32-byte k */
	CHACHA_CONSTANT_EXPA = 0x61707865U,
	CHACHA_CONSTANT_ND_3 = 0x3320646eU,
	CHACHA_CONSTANT_2_BY = 0x79622d32U,
	CHACHA_CONSTANT_TE_K = 0x6b206574U
};

static inline void chacha_init_consts(u32 *state)
{
	state[0]  = CHACHA_CONSTANT_EXPA;
	state[1]  = CHACHA_CONSTANT_ND_3;
	state[2]  = CHACHA_CONSTANT_2_BY;
	state[3]  = CHACHA_CONSTANT_TE_K;
}

static inline void chacha_init(u32 *state, const u32 *key, const u8 *iv)
{
	chacha_init_consts(state);
	state[4]  = key[0];
	state[5]  = key[1];
	state[6]  = key[2];
	state[7]  = key[3];
	state[8]  = key[4];
	state[9]  = key[5];
	state[10] = key[6];
	state[11] = key[7];
	state[12] = get_unaligned_le32(iv +  0);
	state[13] = get_unaligned_le32(iv +  4);
	state[14] = get_unaligned_le32(iv +  8);
	state[15] = get_unaligned_le32(iv + 12);
}

void chacha_crypt_arch(u32 *state, u8 *dst, const u8 *src,
		       unsigned int bytes, int nrounds);
void chacha_crypt_generic(u32 *state, u8 *dst, const u8 *src,
			  unsigned int bytes, int nrounds);

static inline void chacha_crypt(u32 *state, u8 *dst, const u8 *src,
				unsigned int bytes, int nrounds)
{
	if (IS_ENABLED(CONFIG_CRYPTO_ARCH_HAVE_LIB_CHACHA))
		chacha_crypt_arch(state, dst, src, bytes, nrounds);
	else
		chacha_crypt_generic(state, dst, src, bytes, nrounds);
}

static inline void chacha20_crypt(u32 *state, u8 *dst, const u8 *src,
				  unsigned int bytes)
{
	chacha_crypt(state, dst, src, bytes, 20);
}

#if IS_ENABLED(CONFIG_CRYPTO_ARCH_HAVE_LIB_CHACHA)
bool chacha_is_arch_optimized(void);
#else
static inline bool chacha_is_arch_optimized(void)
{
	return false;
}
#endif

#endif /* _CRYPTO_CHACHA_H */
