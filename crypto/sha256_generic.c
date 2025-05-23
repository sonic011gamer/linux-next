// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Crypto API wrapper for the generic SHA256 code from lib/crypto/sha256.c
 *
 * Copyright (c) Jean-Luc Cooke <jlcooke@certainkey.com>
 * Copyright (c) Andrew McDonald <andrew@mcdonald.org.uk>
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 * SHA224 Support Copyright 2007 Intel Corporation <jonathan.lynch@intel.com>
 */
#include <crypto/internal/hash.h>
#include <crypto/sha2.h>
#include <crypto/sha256_base.h>
#include <linux/kernel.h>
#include <linux/module.h>

const u8 sha224_zero_message_hash[SHA224_DIGEST_SIZE] = {
	0xd1, 0x4a, 0x02, 0x8c, 0x2a, 0x3a, 0x2b, 0xc9, 0x47,
	0x61, 0x02, 0xbb, 0x28, 0x82, 0x34, 0xc4, 0x15, 0xa2,
	0xb0, 0x1f, 0x82, 0x8e, 0xa6, 0x2a, 0xc5, 0xb3, 0xe4,
	0x2f
};
EXPORT_SYMBOL_GPL(sha224_zero_message_hash);

const u8 sha256_zero_message_hash[SHA256_DIGEST_SIZE] = {
	0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
	0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
	0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
	0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
};
EXPORT_SYMBOL_GPL(sha256_zero_message_hash);

static void sha256_block(struct crypto_sha256_state *sctx, const u8 *input,
			 int blocks)
{
	sha256_transform_blocks(sctx, input, blocks);
}

static int crypto_sha256_update(struct shash_desc *desc, const u8 *data,
				unsigned int len)
{
	return sha256_base_do_update_blocks(desc, data, len, sha256_block);
}

static int crypto_sha256_finup(struct shash_desc *desc, const u8 *data,
			       unsigned int len, u8 *hash)
{
	sha256_base_do_finup(desc, data, len, sha256_block);
	return sha256_base_finish(desc, hash);
}

static struct shash_alg sha256_algs[2] = { {
	.digestsize	=	SHA256_DIGEST_SIZE,
	.init		=	sha256_base_init,
	.update		=	crypto_sha256_update,
	.finup		=	crypto_sha256_finup,
	.descsize	=	sizeof(struct crypto_sha256_state),
	.base		=	{
		.cra_name	=	"sha256",
		.cra_driver_name=	"sha256-generic",
		.cra_priority	=	100,
		.cra_flags	=	CRYPTO_AHASH_ALG_BLOCK_ONLY |
					CRYPTO_AHASH_ALG_FINUP_MAX,
		.cra_blocksize	=	SHA256_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
	}
}, {
	.digestsize	=	SHA224_DIGEST_SIZE,
	.init		=	sha224_base_init,
	.update		=	crypto_sha256_update,
	.finup		=	crypto_sha256_finup,
	.descsize	=	sizeof(struct crypto_sha256_state),
	.base		=	{
		.cra_name	=	"sha224",
		.cra_driver_name=	"sha224-generic",
		.cra_priority	=	100,
		.cra_flags	=	CRYPTO_AHASH_ALG_BLOCK_ONLY |
					CRYPTO_AHASH_ALG_FINUP_MAX,
		.cra_blocksize	=	SHA224_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
	}
} };

static int __init sha256_generic_mod_init(void)
{
	return crypto_register_shashes(sha256_algs, ARRAY_SIZE(sha256_algs));
}

static void __exit sha256_generic_mod_fini(void)
{
	crypto_unregister_shashes(sha256_algs, ARRAY_SIZE(sha256_algs));
}

subsys_initcall(sha256_generic_mod_init);
module_exit(sha256_generic_mod_fini);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SHA-224 and SHA-256 Secure Hash Algorithm");

MODULE_ALIAS_CRYPTO("sha224");
MODULE_ALIAS_CRYPTO("sha224-generic");
MODULE_ALIAS_CRYPTO("sha256");
MODULE_ALIAS_CRYPTO("sha256-generic");
