// SPDX-License-Identifier: GPL-2.0
/*
 * Cryptographic API.
 *
 * s390 implementation of the AES Cipher Algorithm with protected keys.
 *
 * s390 Version:
 *   Copyright IBM Corp. 2017, 2023
 *   Author(s): Martin Schwidefsky <schwidefsky@de.ibm.com>
 *		Harald Freudenberger <freude@de.ibm.com>
 */

#define KMSG_COMPONENT "paes_s390"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <linux/bug.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/cpufeature.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <crypto/internal/skcipher.h>
#include <crypto/xts.h>
#include <asm/cpacf.h>
#include <asm/pkey.h>

/*
 * Key blobs smaller/bigger than these defines are rejected
 * by the common code even before the individual setkey function
 * is called. As paes can handle different kinds of key blobs
 * and padding is also possible, the limits need to be generous.
 */
#define PAES_MIN_KEYSIZE	16
#define PAES_MAX_KEYSIZE	MAXEP11AESKEYBLOBSIZE
#define PAES_256_PROTKEY_SIZE	(32 + 32)	/* key + verification pattern */
#define PXTS_256_PROTKEY_SIZE	(32 + 32 + 32)	/* k1 + k2 + verification pattern */

static u8 *ctrblk;
static DEFINE_MUTEX(ctrblk_lock);

static cpacf_mask_t km_functions, kmc_functions, kmctr_functions;

struct paes_protkey {
	u32 type;
	u32 len;
	u8 protkey[PXTS_256_PROTKEY_SIZE];
};

struct key_blob {
	/*
	 * Small keys will be stored in the keybuf. Larger keys are
	 * stored in extra allocated memory. In both cases does
	 * key point to the memory where the key is stored.
	 * The code distinguishes by checking keylen against
	 * sizeof(keybuf). See the two following helper functions.
	 */
	u8 *key;
	u8 keybuf[128];
	unsigned int keylen;
};

/*
 * make_clrkey_token() - wrap the raw key ck with pkey clearkey token
 * information.
 * @returns the size of the clearkey token
 */
static inline u32 make_clrkey_token(const u8 *ck, size_t cklen, u8 *dest)
{
	struct clrkey_token {
		u8  type;
		u8  res0[3];
		u8  version;
		u8  res1[3];
		u32 keytype;
		u32 len;
		u8 key[];
	} __packed *token = (struct clrkey_token *)dest;

	token->type = 0x00;
	token->version = 0x02;
	token->keytype = (cklen - 8) >> 3;
	token->len = cklen;
	memcpy(token->key, ck, cklen);

	return sizeof(*token) + cklen;
}

static inline int _key_to_kb(struct key_blob *kb,
			     const u8 *key,
			     unsigned int keylen)
{
	switch (keylen) {
	case 16:
	case 24:
	case 32:
		/* clear key value, prepare pkey clear key token in keybuf */
		memset(kb->keybuf, 0, sizeof(kb->keybuf));
		kb->keylen = make_clrkey_token(key, keylen, kb->keybuf);
		kb->key = kb->keybuf;
		break;
	default:
		/* other key material, let pkey handle this */
		if (keylen <= sizeof(kb->keybuf))
			kb->key = kb->keybuf;
		else {
			kb->key = kmalloc(keylen, GFP_KERNEL);
			if (!kb->key)
				return -ENOMEM;
		}
		memcpy(kb->key, key, keylen);
		kb->keylen = keylen;
		break;
	}

	return 0;
}

static inline int _xts_key_to_kb(struct key_blob *kb,
				 const u8 *key,
				 unsigned int keylen)
{
	size_t cklen = keylen / 2;

	memset(kb->keybuf, 0, sizeof(kb->keybuf));

	switch (keylen) {
	case 32:
	case 64:
		/* clear key value, prepare pkey clear key tokens in keybuf */
		kb->key = kb->keybuf;
		kb->keylen  = make_clrkey_token(key, cklen, kb->key);
		kb->keylen += make_clrkey_token(key + cklen, cklen,
						kb->key + kb->keylen);
		break;
	default:
		/* other key material, let pkey handle this */
		if (keylen <= sizeof(kb->keybuf)) {
			kb->key = kb->keybuf;
		} else {
			kb->key = kmalloc(keylen, GFP_KERNEL);
			if (!kb->key)
				return -ENOMEM;
		}
		memcpy(kb->key, key, keylen);
		kb->keylen = keylen;
		break;
	}

	return 0;
}

static inline void _free_kb_keybuf(struct key_blob *kb)
{
	if (kb->key && kb->key != kb->keybuf
	    && kb->keylen > sizeof(kb->keybuf)) {
		kfree_sensitive(kb->key);
		kb->key = NULL;
	}
	memzero_explicit(kb->keybuf, sizeof(kb->keybuf));
}

struct s390_paes_ctx {
	struct key_blob kb;
	struct paes_protkey pk;
	spinlock_t pk_lock;
	unsigned long fc;
};

struct s390_pxts_ctx {
	struct key_blob kb;
	struct paes_protkey pk[2];
	spinlock_t pk_lock;
	unsigned long fc;
};

static inline int __paes_keyblob2pkey(const u8 *key, unsigned int keylen,
				      struct paes_protkey *pk)
{
	int i, rc = -EIO;

	/* try three times in case of busy card or no mem */
	for (i = 0; rc && i < 3; i++) {
		if ((rc == -EBUSY || rc == -ENOMEM) && in_task()) {
			if (msleep_interruptible(1000))
				return -EINTR;
		}
		rc = pkey_key2protkey(key, keylen, pk->protkey, &pk->len,
				      &pk->type, PKEY_XFLAG_NOMEMALLOC);
	}

	return rc;
}

static inline int __paes_convert_key(struct s390_paes_ctx *ctx)
{
	struct paes_protkey pk;
	int rc;

	pk.len = sizeof(pk.protkey);
	rc = __paes_keyblob2pkey(ctx->kb.key, ctx->kb.keylen, &pk);
	if (rc)
		return rc;

	spin_lock_bh(&ctx->pk_lock);
	memcpy(&ctx->pk, &pk, sizeof(pk));
	spin_unlock_bh(&ctx->pk_lock);

	return 0;
}

static int ecb_paes_init(struct crypto_skcipher *tfm)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->kb.key = NULL;
	spin_lock_init(&ctx->pk_lock);

	return 0;
}

static void ecb_paes_exit(struct crypto_skcipher *tfm)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
}

static inline int __ecb_paes_set_key(struct s390_paes_ctx *ctx)
{
	unsigned long fc;
	int rc;

	rc = __paes_convert_key(ctx);
	if (rc)
		return rc;

	/* Pick the correct function code based on the protected key type */
	fc = (ctx->pk.type == PKEY_KEYTYPE_AES_128) ? CPACF_KM_PAES_128 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_192) ? CPACF_KM_PAES_192 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_256) ? CPACF_KM_PAES_256 : 0;

	/* Check if the function code is available */
	ctx->fc = (fc && cpacf_test_func(&km_functions, fc)) ? fc : 0;

	return ctx->fc ? 0 : -EINVAL;
}

static int ecb_paes_set_key(struct crypto_skcipher *tfm, const u8 *in_key,
			    unsigned int key_len)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);
	int rc;

	_free_kb_keybuf(&ctx->kb);
	rc = _key_to_kb(&ctx->kb, in_key, key_len);
	if (rc)
		return rc;

	return __ecb_paes_set_key(ctx);
}

static int ecb_paes_crypt(struct skcipher_request *req, unsigned long modifier)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct {
		u8 key[PAES_256_PROTKEY_SIZE];
	} param;
	struct skcipher_walk walk;
	unsigned int nbytes, n, k;
	int rc;

	rc = skcipher_walk_virt(&walk, req, false);
	if (rc)
		return rc;

	spin_lock_bh(&ctx->pk_lock);
	memcpy(param.key, ctx->pk.protkey, PAES_256_PROTKEY_SIZE);
	spin_unlock_bh(&ctx->pk_lock);

	while ((nbytes = walk.nbytes) != 0) {
		/* only use complete blocks */
		n = nbytes & ~(AES_BLOCK_SIZE - 1);
		k = cpacf_km(ctx->fc | modifier, &param,
			     walk.dst.virt.addr, walk.src.virt.addr, n);
		if (k)
			rc = skcipher_walk_done(&walk, nbytes - k);
		if (k < n) {
			if (__paes_convert_key(ctx))
				return skcipher_walk_done(&walk, -EIO);
			spin_lock_bh(&ctx->pk_lock);
			memcpy(param.key, ctx->pk.protkey, PAES_256_PROTKEY_SIZE);
			spin_unlock_bh(&ctx->pk_lock);
		}
	}
	return rc;
}

static int ecb_paes_encrypt(struct skcipher_request *req)
{
	return ecb_paes_crypt(req, 0);
}

static int ecb_paes_decrypt(struct skcipher_request *req)
{
	return ecb_paes_crypt(req, CPACF_DECRYPT);
}

static struct skcipher_alg ecb_paes_alg = {
	.base.cra_name		=	"ecb(paes)",
	.base.cra_driver_name	=	"ecb-paes-s390",
	.base.cra_priority	=	401,	/* combo: aes + ecb + 1 */
	.base.cra_blocksize	=	AES_BLOCK_SIZE,
	.base.cra_ctxsize	=	sizeof(struct s390_paes_ctx),
	.base.cra_module	=	THIS_MODULE,
	.base.cra_list		=	LIST_HEAD_INIT(ecb_paes_alg.base.cra_list),
	.init			=	ecb_paes_init,
	.exit			=	ecb_paes_exit,
	.min_keysize		=	PAES_MIN_KEYSIZE,
	.max_keysize		=	PAES_MAX_KEYSIZE,
	.setkey			=	ecb_paes_set_key,
	.encrypt		=	ecb_paes_encrypt,
	.decrypt		=	ecb_paes_decrypt,
};

static int cbc_paes_init(struct crypto_skcipher *tfm)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->kb.key = NULL;
	spin_lock_init(&ctx->pk_lock);

	return 0;
}

static void cbc_paes_exit(struct crypto_skcipher *tfm)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
}

static inline int __cbc_paes_set_key(struct s390_paes_ctx *ctx)
{
	unsigned long fc;
	int rc;

	rc = __paes_convert_key(ctx);
	if (rc)
		return rc;

	/* Pick the correct function code based on the protected key type */
	fc = (ctx->pk.type == PKEY_KEYTYPE_AES_128) ? CPACF_KMC_PAES_128 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_192) ? CPACF_KMC_PAES_192 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_256) ? CPACF_KMC_PAES_256 : 0;

	/* Check if the function code is available */
	ctx->fc = (fc && cpacf_test_func(&kmc_functions, fc)) ? fc : 0;

	return ctx->fc ? 0 : -EINVAL;
}

static int cbc_paes_set_key(struct crypto_skcipher *tfm, const u8 *in_key,
			    unsigned int key_len)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);
	int rc;

	_free_kb_keybuf(&ctx->kb);
	rc = _key_to_kb(&ctx->kb, in_key, key_len);
	if (rc)
		return rc;

	return __cbc_paes_set_key(ctx);
}

static int cbc_paes_crypt(struct skcipher_request *req, unsigned long modifier)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct {
		u8 iv[AES_BLOCK_SIZE];
		u8 key[PAES_256_PROTKEY_SIZE];
	} param;
	struct skcipher_walk walk;
	unsigned int nbytes, n, k;
	int rc;

	rc = skcipher_walk_virt(&walk, req, false);
	if (rc)
		return rc;

	memcpy(param.iv, walk.iv, AES_BLOCK_SIZE);
	spin_lock_bh(&ctx->pk_lock);
	memcpy(param.key, ctx->pk.protkey, PAES_256_PROTKEY_SIZE);
	spin_unlock_bh(&ctx->pk_lock);

	while ((nbytes = walk.nbytes) != 0) {
		/* only use complete blocks */
		n = nbytes & ~(AES_BLOCK_SIZE - 1);
		k = cpacf_kmc(ctx->fc | modifier, &param,
			      walk.dst.virt.addr, walk.src.virt.addr, n);
		if (k) {
			memcpy(walk.iv, param.iv, AES_BLOCK_SIZE);
			rc = skcipher_walk_done(&walk, nbytes - k);
		}
		if (k < n) {
			if (__paes_convert_key(ctx))
				return skcipher_walk_done(&walk, -EIO);
			spin_lock_bh(&ctx->pk_lock);
			memcpy(param.key, ctx->pk.protkey, PAES_256_PROTKEY_SIZE);
			spin_unlock_bh(&ctx->pk_lock);
		}
	}
	return rc;
}

static int cbc_paes_encrypt(struct skcipher_request *req)
{
	return cbc_paes_crypt(req, 0);
}

static int cbc_paes_decrypt(struct skcipher_request *req)
{
	return cbc_paes_crypt(req, CPACF_DECRYPT);
}

static struct skcipher_alg cbc_paes_alg = {
	.base.cra_name		=	"cbc(paes)",
	.base.cra_driver_name	=	"cbc-paes-s390",
	.base.cra_priority	=	402,	/* ecb-paes-s390 + 1 */
	.base.cra_blocksize	=	AES_BLOCK_SIZE,
	.base.cra_ctxsize	=	sizeof(struct s390_paes_ctx),
	.base.cra_module	=	THIS_MODULE,
	.base.cra_list		=	LIST_HEAD_INIT(cbc_paes_alg.base.cra_list),
	.init			=	cbc_paes_init,
	.exit			=	cbc_paes_exit,
	.min_keysize		=	PAES_MIN_KEYSIZE,
	.max_keysize		=	PAES_MAX_KEYSIZE,
	.ivsize			=	AES_BLOCK_SIZE,
	.setkey			=	cbc_paes_set_key,
	.encrypt		=	cbc_paes_encrypt,
	.decrypt		=	cbc_paes_decrypt,
};

static int xts_paes_init(struct crypto_skcipher *tfm)
{
	struct s390_pxts_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->kb.key = NULL;
	spin_lock_init(&ctx->pk_lock);

	return 0;
}

static void xts_paes_exit(struct crypto_skcipher *tfm)
{
	struct s390_pxts_ctx *ctx = crypto_skcipher_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
}

static inline int __xts_paes_convert_key(struct s390_pxts_ctx *ctx)
{
	struct paes_protkey pk0, pk1;
	size_t split_keylen;
	int rc;

	pk0.len = sizeof(pk0.protkey);
	pk1.len = sizeof(pk1.protkey);

	rc = __paes_keyblob2pkey(ctx->kb.key, ctx->kb.keylen, &pk0);
	if (rc)
		return rc;

	switch (pk0.type) {
	case PKEY_KEYTYPE_AES_128:
	case PKEY_KEYTYPE_AES_256:
		/* second keytoken required */
		if (ctx->kb.keylen % 2)
			return -EINVAL;
		split_keylen = ctx->kb.keylen / 2;

		rc = __paes_keyblob2pkey(ctx->kb.key + split_keylen,
					 split_keylen, &pk1);
		if (rc)
			return rc;

		if (pk0.type != pk1.type)
			return -EINVAL;
		break;
	case PKEY_KEYTYPE_AES_XTS_128:
	case PKEY_KEYTYPE_AES_XTS_256:
		/* single key */
		pk1.type = 0;
		break;
	default:
		/* unsupported protected keytype */
		return -EINVAL;
	}

	spin_lock_bh(&ctx->pk_lock);
	ctx->pk[0] = pk0;
	ctx->pk[1] = pk1;
	spin_unlock_bh(&ctx->pk_lock);

	return 0;
}

static inline int __xts_paes_set_key(struct s390_pxts_ctx *ctx)
{
	unsigned long fc;
	int rc;

	rc = __xts_paes_convert_key(ctx);
	if (rc)
		return rc;

	/* Pick the correct function code based on the protected key type */
	switch (ctx->pk[0].type) {
	case PKEY_KEYTYPE_AES_128:
		fc = CPACF_KM_PXTS_128;
		break;
	case PKEY_KEYTYPE_AES_256:
		fc = CPACF_KM_PXTS_256;
		break;
	case PKEY_KEYTYPE_AES_XTS_128:
		fc = CPACF_KM_PXTS_128_FULL;
		break;
	case PKEY_KEYTYPE_AES_XTS_256:
		fc = CPACF_KM_PXTS_256_FULL;
		break;
	default:
		fc = 0;
		break;
	}

	/* Check if the function code is available */
	ctx->fc = (fc && cpacf_test_func(&km_functions, fc)) ? fc : 0;

	return ctx->fc ? 0 : -EINVAL;
}

static int xts_paes_set_key(struct crypto_skcipher *tfm, const u8 *in_key,
			    unsigned int in_keylen)
{
	struct s390_pxts_ctx *ctx = crypto_skcipher_ctx(tfm);
	u8 ckey[2 * AES_MAX_KEY_SIZE];
	unsigned int ckey_len;
	int rc;

	if ((in_keylen == 32 || in_keylen == 64) &&
	    xts_verify_key(tfm, in_key, in_keylen))
		return -EINVAL;

	_free_kb_keybuf(&ctx->kb);
	rc = _xts_key_to_kb(&ctx->kb, in_key, in_keylen);
	if (rc)
		return rc;

	rc = __xts_paes_set_key(ctx);
	if (rc)
		return rc;

	/*
	 * It is not possible on a single protected key (e.g. full AES-XTS) to
	 * check, if k1 and k2 are the same.
	 */
	if (ctx->pk[0].type == PKEY_KEYTYPE_AES_XTS_128 ||
	    ctx->pk[0].type == PKEY_KEYTYPE_AES_XTS_256)
		return 0;
	/*
	 * xts_verify_key verifies the key length is not odd and makes
	 * sure that the two keys are not the same. This can be done
	 * on the two protected keys as well
	 */
	ckey_len = (ctx->pk[0].type == PKEY_KEYTYPE_AES_128) ?
		AES_KEYSIZE_128 : AES_KEYSIZE_256;
	memcpy(ckey, ctx->pk[0].protkey, ckey_len);
	memcpy(ckey + ckey_len, ctx->pk[1].protkey, ckey_len);
	return xts_verify_key(tfm, ckey, 2*ckey_len);
}

static int paes_xts_crypt_full(struct skcipher_request *req,
			       unsigned long modifier)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct s390_pxts_ctx *ctx = crypto_skcipher_ctx(tfm);
	unsigned int keylen, offset, nbytes, n, k;
	struct {
		u8 key[64];
		u8 tweak[16];
		u8 nap[16];
		u8 wkvp[32];
	} fxts_param = {
		.nap = {0},
	};
	struct skcipher_walk walk;
	int rc;

	rc = skcipher_walk_virt(&walk, req, false);
	if (rc)
		return rc;

	keylen = (ctx->pk[0].type == PKEY_KEYTYPE_AES_XTS_128) ? 32 : 64;
	offset = (ctx->pk[0].type == PKEY_KEYTYPE_AES_XTS_128) ? 32 : 0;

	spin_lock_bh(&ctx->pk_lock);
	memcpy(fxts_param.key + offset, ctx->pk[0].protkey, keylen);
	memcpy(fxts_param.wkvp, ctx->pk[0].protkey + keylen,
	       sizeof(fxts_param.wkvp));
	spin_unlock_bh(&ctx->pk_lock);
	memcpy(fxts_param.tweak, walk.iv, sizeof(fxts_param.tweak));
	fxts_param.nap[0] = 0x01; /* initial alpha power (1, little-endian) */

	while ((nbytes = walk.nbytes) != 0) {
		/* only use complete blocks */
		n = nbytes & ~(AES_BLOCK_SIZE - 1);
		k = cpacf_km(ctx->fc | modifier, fxts_param.key + offset,
			     walk.dst.virt.addr, walk.src.virt.addr, n);
		if (k)
			rc = skcipher_walk_done(&walk, nbytes - k);
		if (k < n) {
			if (__xts_paes_convert_key(ctx))
				return skcipher_walk_done(&walk, -EIO);
			spin_lock_bh(&ctx->pk_lock);
			memcpy(fxts_param.key + offset, ctx->pk[0].protkey,
			       keylen);
			memcpy(fxts_param.wkvp, ctx->pk[0].protkey + keylen,
			       sizeof(fxts_param.wkvp));
			spin_unlock_bh(&ctx->pk_lock);
		}
	}

	return rc;
}

static int paes_xts_crypt(struct skcipher_request *req, unsigned long modifier)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct s390_pxts_ctx *ctx = crypto_skcipher_ctx(tfm);
	unsigned int keylen, offset, nbytes, n, k;
	struct {
		u8 key[PAES_256_PROTKEY_SIZE];
		u8 tweak[16];
		u8 block[16];
		u8 bit[16];
		u8 xts[16];
	} pcc_param;
	struct {
		u8 key[PAES_256_PROTKEY_SIZE];
		u8 init[16];
	} xts_param;
	struct skcipher_walk walk;
	int rc;

	rc = skcipher_walk_virt(&walk, req, false);
	if (rc)
		return rc;

	keylen = (ctx->pk[0].type == PKEY_KEYTYPE_AES_128) ? 48 : 64;
	offset = (ctx->pk[0].type == PKEY_KEYTYPE_AES_128) ? 16 : 0;

	memset(&pcc_param, 0, sizeof(pcc_param));
	memcpy(pcc_param.tweak, walk.iv, sizeof(pcc_param.tweak));
	spin_lock_bh(&ctx->pk_lock);
	memcpy(pcc_param.key + offset, ctx->pk[1].protkey, keylen);
	memcpy(xts_param.key + offset, ctx->pk[0].protkey, keylen);
	spin_unlock_bh(&ctx->pk_lock);
	cpacf_pcc(ctx->fc, pcc_param.key + offset);
	memcpy(xts_param.init, pcc_param.xts, 16);

	while ((nbytes = walk.nbytes) != 0) {
		/* only use complete blocks */
		n = nbytes & ~(AES_BLOCK_SIZE - 1);
		k = cpacf_km(ctx->fc | modifier, xts_param.key + offset,
			     walk.dst.virt.addr, walk.src.virt.addr, n);
		if (k)
			rc = skcipher_walk_done(&walk, nbytes - k);
		if (k < n) {
			if (__xts_paes_convert_key(ctx))
				return skcipher_walk_done(&walk, -EIO);
			spin_lock_bh(&ctx->pk_lock);
			memcpy(xts_param.key + offset,
			       ctx->pk[0].protkey, keylen);
			spin_unlock_bh(&ctx->pk_lock);
		}
	}

	return rc;
}

static inline int xts_paes_crypt(struct skcipher_request *req, unsigned long modifier)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct s390_pxts_ctx *ctx = crypto_skcipher_ctx(tfm);

	switch (ctx->fc) {
	case CPACF_KM_PXTS_128:
	case CPACF_KM_PXTS_256:
		return paes_xts_crypt(req, modifier);
	case CPACF_KM_PXTS_128_FULL:
	case CPACF_KM_PXTS_256_FULL:
		return paes_xts_crypt_full(req, modifier);
	default:
		return -EINVAL;
	}
}

static int xts_paes_encrypt(struct skcipher_request *req)
{
	return xts_paes_crypt(req, 0);
}

static int xts_paes_decrypt(struct skcipher_request *req)
{
	return xts_paes_crypt(req, CPACF_DECRYPT);
}

static struct skcipher_alg xts_paes_alg = {
	.base.cra_name		=	"xts(paes)",
	.base.cra_driver_name	=	"xts-paes-s390",
	.base.cra_priority	=	402,	/* ecb-paes-s390 + 1 */
	.base.cra_blocksize	=	AES_BLOCK_SIZE,
	.base.cra_ctxsize	=	sizeof(struct s390_pxts_ctx),
	.base.cra_module	=	THIS_MODULE,
	.base.cra_list		=	LIST_HEAD_INIT(xts_paes_alg.base.cra_list),
	.init			=	xts_paes_init,
	.exit			=	xts_paes_exit,
	.min_keysize		=	2 * PAES_MIN_KEYSIZE,
	.max_keysize		=	2 * PAES_MAX_KEYSIZE,
	.ivsize			=	AES_BLOCK_SIZE,
	.setkey			=	xts_paes_set_key,
	.encrypt		=	xts_paes_encrypt,
	.decrypt		=	xts_paes_decrypt,
};

static int ctr_paes_init(struct crypto_skcipher *tfm)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->kb.key = NULL;
	spin_lock_init(&ctx->pk_lock);

	return 0;
}

static void ctr_paes_exit(struct crypto_skcipher *tfm)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);

	_free_kb_keybuf(&ctx->kb);
}

static inline int __ctr_paes_set_key(struct s390_paes_ctx *ctx)
{
	unsigned long fc;
	int rc;

	rc = __paes_convert_key(ctx);
	if (rc)
		return rc;

	/* Pick the correct function code based on the protected key type */
	fc = (ctx->pk.type == PKEY_KEYTYPE_AES_128) ? CPACF_KMCTR_PAES_128 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_192) ? CPACF_KMCTR_PAES_192 :
		(ctx->pk.type == PKEY_KEYTYPE_AES_256) ?
		CPACF_KMCTR_PAES_256 : 0;

	/* Check if the function code is available */
	ctx->fc = (fc && cpacf_test_func(&kmctr_functions, fc)) ? fc : 0;

	return ctx->fc ? 0 : -EINVAL;
}

static int ctr_paes_set_key(struct crypto_skcipher *tfm, const u8 *in_key,
			    unsigned int key_len)
{
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);
	int rc;

	_free_kb_keybuf(&ctx->kb);
	rc = _key_to_kb(&ctx->kb, in_key, key_len);
	if (rc)
		return rc;

	return __ctr_paes_set_key(ctx);
}

static unsigned int __ctrblk_init(u8 *ctrptr, u8 *iv, unsigned int nbytes)
{
	unsigned int i, n;

	/* only use complete blocks, max. PAGE_SIZE */
	memcpy(ctrptr, iv, AES_BLOCK_SIZE);
	n = (nbytes > PAGE_SIZE) ? PAGE_SIZE : nbytes & ~(AES_BLOCK_SIZE - 1);
	for (i = (n / AES_BLOCK_SIZE) - 1; i > 0; i--) {
		memcpy(ctrptr + AES_BLOCK_SIZE, ctrptr, AES_BLOCK_SIZE);
		crypto_inc(ctrptr + AES_BLOCK_SIZE, AES_BLOCK_SIZE);
		ctrptr += AES_BLOCK_SIZE;
	}
	return n;
}

static int ctr_paes_crypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct s390_paes_ctx *ctx = crypto_skcipher_ctx(tfm);
	u8 buf[AES_BLOCK_SIZE], *ctrptr;
	struct {
		u8 key[PAES_256_PROTKEY_SIZE];
	} param;
	struct skcipher_walk walk;
	unsigned int nbytes, n, k;
	int rc, locked;

	rc = skcipher_walk_virt(&walk, req, false);
	if (rc)
		return rc;

	spin_lock_bh(&ctx->pk_lock);
	memcpy(param.key, ctx->pk.protkey, PAES_256_PROTKEY_SIZE);
	spin_unlock_bh(&ctx->pk_lock);

	locked = mutex_trylock(&ctrblk_lock);

	while ((nbytes = walk.nbytes) >= AES_BLOCK_SIZE) {
		n = AES_BLOCK_SIZE;
		if (nbytes >= 2*AES_BLOCK_SIZE && locked)
			n = __ctrblk_init(ctrblk, walk.iv, nbytes);
		ctrptr = (n > AES_BLOCK_SIZE) ? ctrblk : walk.iv;
		k = cpacf_kmctr(ctx->fc, &param, walk.dst.virt.addr,
				walk.src.virt.addr, n, ctrptr);
		if (k) {
			if (ctrptr == ctrblk)
				memcpy(walk.iv, ctrptr + k - AES_BLOCK_SIZE,
				       AES_BLOCK_SIZE);
			crypto_inc(walk.iv, AES_BLOCK_SIZE);
			rc = skcipher_walk_done(&walk, nbytes - k);
		}
		if (k < n) {
			if (__paes_convert_key(ctx)) {
				if (locked)
					mutex_unlock(&ctrblk_lock);
				return skcipher_walk_done(&walk, -EIO);
			}
			spin_lock_bh(&ctx->pk_lock);
			memcpy(param.key, ctx->pk.protkey, PAES_256_PROTKEY_SIZE);
			spin_unlock_bh(&ctx->pk_lock);
		}
	}
	if (locked)
		mutex_unlock(&ctrblk_lock);
	/*
	 * final block may be < AES_BLOCK_SIZE, copy only nbytes
	 */
	if (nbytes) {
		memset(buf, 0, AES_BLOCK_SIZE);
		memcpy(buf, walk.src.virt.addr, nbytes);
		while (1) {
			if (cpacf_kmctr(ctx->fc, &param, buf,
					buf, AES_BLOCK_SIZE,
					walk.iv) == AES_BLOCK_SIZE)
				break;
			if (__paes_convert_key(ctx))
				return skcipher_walk_done(&walk, -EIO);
			spin_lock_bh(&ctx->pk_lock);
			memcpy(param.key, ctx->pk.protkey, PAES_256_PROTKEY_SIZE);
			spin_unlock_bh(&ctx->pk_lock);
		}
		memcpy(walk.dst.virt.addr, buf, nbytes);
		crypto_inc(walk.iv, AES_BLOCK_SIZE);
		rc = skcipher_walk_done(&walk, nbytes);
	}

	return rc;
}

static struct skcipher_alg ctr_paes_alg = {
	.base.cra_name		=	"ctr(paes)",
	.base.cra_driver_name	=	"ctr-paes-s390",
	.base.cra_priority	=	402,	/* ecb-paes-s390 + 1 */
	.base.cra_blocksize	=	1,
	.base.cra_ctxsize	=	sizeof(struct s390_paes_ctx),
	.base.cra_module	=	THIS_MODULE,
	.base.cra_list		=	LIST_HEAD_INIT(ctr_paes_alg.base.cra_list),
	.init			=	ctr_paes_init,
	.exit			=	ctr_paes_exit,
	.min_keysize		=	PAES_MIN_KEYSIZE,
	.max_keysize		=	PAES_MAX_KEYSIZE,
	.ivsize			=	AES_BLOCK_SIZE,
	.setkey			=	ctr_paes_set_key,
	.encrypt		=	ctr_paes_crypt,
	.decrypt		=	ctr_paes_crypt,
	.chunksize		=	AES_BLOCK_SIZE,
};

static inline void __crypto_unregister_skcipher(struct skcipher_alg *alg)
{
	if (!list_empty(&alg->base.cra_list))
		crypto_unregister_skcipher(alg);
}

static void paes_s390_fini(void)
{
	__crypto_unregister_skcipher(&ctr_paes_alg);
	__crypto_unregister_skcipher(&xts_paes_alg);
	__crypto_unregister_skcipher(&cbc_paes_alg);
	__crypto_unregister_skcipher(&ecb_paes_alg);
	if (ctrblk)
		free_page((unsigned long) ctrblk);
}

static int __init paes_s390_init(void)
{
	int rc;

	/* Query available functions for KM, KMC and KMCTR */
	cpacf_query(CPACF_KM, &km_functions);
	cpacf_query(CPACF_KMC, &kmc_functions);
	cpacf_query(CPACF_KMCTR, &kmctr_functions);

	if (cpacf_test_func(&km_functions, CPACF_KM_PAES_128) ||
	    cpacf_test_func(&km_functions, CPACF_KM_PAES_192) ||
	    cpacf_test_func(&km_functions, CPACF_KM_PAES_256)) {
		rc = crypto_register_skcipher(&ecb_paes_alg);
		if (rc)
			goto out_err;
	}

	if (cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_128) ||
	    cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_192) ||
	    cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_256)) {
		rc = crypto_register_skcipher(&cbc_paes_alg);
		if (rc)
			goto out_err;
	}

	if (cpacf_test_func(&km_functions, CPACF_KM_PXTS_128) ||
	    cpacf_test_func(&km_functions, CPACF_KM_PXTS_256)) {
		rc = crypto_register_skcipher(&xts_paes_alg);
		if (rc)
			goto out_err;
	}

	if (cpacf_test_func(&kmctr_functions, CPACF_KMCTR_PAES_128) ||
	    cpacf_test_func(&kmctr_functions, CPACF_KMCTR_PAES_192) ||
	    cpacf_test_func(&kmctr_functions, CPACF_KMCTR_PAES_256)) {
		ctrblk = (u8 *) __get_free_page(GFP_KERNEL);
		if (!ctrblk) {
			rc = -ENOMEM;
			goto out_err;
		}
		rc = crypto_register_skcipher(&ctr_paes_alg);
		if (rc)
			goto out_err;
	}

	return 0;
out_err:
	paes_s390_fini();
	return rc;
}

module_init(paes_s390_init);
module_exit(paes_s390_fini);

MODULE_ALIAS_CRYPTO("ecb(paes)");
MODULE_ALIAS_CRYPTO("cbc(paes)");
MODULE_ALIAS_CRYPTO("ctr(paes)");
MODULE_ALIAS_CRYPTO("xts(paes)");

MODULE_DESCRIPTION("Rijndael (AES) Cipher Algorithm with protected keys");
MODULE_LICENSE("GPL");
