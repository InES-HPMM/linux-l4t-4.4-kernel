/*
 * Quick & dirty crypto testing module.
 *
 * This will only exist until we have a better testing mechanism
 * (e.g. a char device).
 *
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 * Copyright (c) 2002 Jean-Francois Dive <jef@linuxbe.org>
 * Copyright (c) 2007 Nokia Siemens Networks
 * Copyright (c) 2016-2017, NVIDIA Corporation. All Rights Reserved.
 *
 * Updated RFC4106 AES-GCM testing.
 *    Authors: Aidan O'Mahony (aidan.o.mahony@intel.com)
 *             Adrian Hoban <adrian.hoban@intel.com>
 *             Gabriele Paoloni <gabriele.paoloni@intel.com>
 *             Tadeusz Struk (tadeusz.struk@intel.com)
 *             Copyright (c) 2010, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#include <crypto/aead.h>
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <crypto/akcipher.h>
#include <linux/err.h>
#include <linux/fips.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/timex.h>
#include <linux/interrupt.h>

#include "tcrypt.h"

/*
 * Need slab memory for testing (size in number of pages).
 */
#define TVMEMSIZE	4

/*
 * Used by test_cipher_speed()
 */
#define DECRYPT		0
#define ENCRYPT		1
#define SIGN		2
#define VERIFY		3

#define MAX_DIGEST_SIZE		64

/*
 * return a string with the driver name
 */
#define get_driver_name(tfm_type, tfm) crypto_tfm_alg_driver_name(tfm_type ## _tfm(tfm))

/*
 * Used by test_cipher_speed()
 */
static unsigned int sec;
static unsigned long dsize;
static unsigned int bsize;
static unsigned int bcnt;
static char *alg = NULL;
static u32 type;
static u32 mask;
static int mode;
static char *tvmem[TVMEMSIZE];

static char *check[] = {
	"des", "md5", "des3_ede", "rot13", "sha1", "sha224", "sha256",
	"blowfish", "twofish", "serpent", "sha384", "sha512", "md4", "aes",
	"cast6", "arc4", "michael_mic", "deflate", "crc32c", "tea", "xtea",
	"khazad", "wp512", "wp384", "wp256", "tnepres", "xeta",  "fcrypt",
	"camellia", "seed", "salsa20", "rmd128", "rmd160", "rmd256", "rmd320",
	"lzo", "cts", "zlib", NULL
};

struct tcrypt_result {
	struct completion completion;
	int err;
};

static void tcrypt_complete(struct crypto_async_request *req, int err)
{
	struct tcrypt_result *res = req->data;

	if (err == -EINPROGRESS)
		return;

	res->err = err;
	complete(&res->completion);
}

static int test_cipher_jiffies(struct blkcipher_desc *desc, int enc,
				struct scatterlist *sg, int blen, int secs)
{
	unsigned long start, end;
	int bcount;
	int ret;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		if (enc)
			ret = crypto_blkcipher_encrypt(desc, sg, sg, blen);
		else
			ret = crypto_blkcipher_decrypt(desc, sg, sg, blen);

		if (ret)
			return ret;
	}

	pr_info("%d operations in %d seconds (%ld bytes)\n",
		bcount, secs, (long)bcount * blen);
	return 0;
}

static int test_cipher_cycles(struct blkcipher_desc *desc, int enc,
			struct scatterlist *sg, int blen)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;

	local_irq_disable();

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		if (enc)
			ret = crypto_blkcipher_encrypt(desc, sg, sg, blen);
		else
			ret = crypto_blkcipher_decrypt(desc, sg, sg, blen);

		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (enc)
			ret = crypto_blkcipher_encrypt(desc, sg, sg, blen);
		else
			ret = crypto_blkcipher_decrypt(desc, sg, sg, blen);
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

out:
	local_irq_enable();

	if (ret == 0)
		pr_info("1 operation in %lu cycles (%d bytes)\n",
			(cycles + 4) / 8, blen);

	return ret;
}

static inline int do_one_aead_op(struct aead_request *req, int ret)
{
	if (ret == -EINPROGRESS || ret == -EBUSY) {
		struct tcrypt_result *tr = req->base.data;

		ret = wait_for_completion_interruptible(&tr->completion);
		if (!ret)
			ret = tr->err;
		reinit_completion(&tr->completion);
	}

	return ret;
}

static int test_aead_jiffies(struct aead_request *req, int enc,
				int blen, int secs)
{
	unsigned long start, end;
	int bcount;
	int ret;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		if (enc)
			ret = do_one_aead_op(req, crypto_aead_encrypt(req));
		else
			ret = do_one_aead_op(req, crypto_aead_decrypt(req));

		if (ret)
			return ret;
	}

	pr_info("%d operations in %d seconds (%ld bytes)\n",
		bcount, secs, (long)bcount * blen);
	return 0;
}

static int test_aead_cycles(struct aead_request *req, int enc, int blen)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;

	local_irq_disable();

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		if (enc)
			ret = do_one_aead_op(req, crypto_aead_encrypt(req));
		else
			ret = do_one_aead_op(req, crypto_aead_decrypt(req));

		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (enc)
			ret = do_one_aead_op(req, crypto_aead_encrypt(req));
		else
			ret = do_one_aead_op(req, crypto_aead_decrypt(req));
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

out:
	local_irq_enable();

	if (ret == 0)
		pr_info("1 operation in %lu cycles (%d bytes)\n",
			(cycles + 4) / 8, blen);

	return ret;
}

static u32 block_sizes[] = { 16, 64, 256, 512, 1024, 8192, 0 };
static u32 aead_sizes[] = { 16, 64, 256, 512, 1024, 2048, 4096, 8192, 0 };

#define XBUFSIZE 8
#define MAX_IVLEN 32

static int testmgr_alloc_buf(char *buf[XBUFSIZE])
{
	int i;

	for (i = 0; i < XBUFSIZE; i++) {
		buf[i] = (void *)__get_free_page(GFP_KERNEL);
		if (!buf[i])
			goto err_free_buf;
	}

	return 0;

err_free_buf:
	while (i-- > 0)
		free_page((unsigned long)buf[i]);

	return -ENOMEM;
}

static void testmgr_free_buf(char *buf[XBUFSIZE])
{
	int i;

	for (i = 0; i < XBUFSIZE; i++)
		free_page((unsigned long)buf[i]);
}

static void sg_init_aead(struct scatterlist *sg, char *xbuf[XBUFSIZE],
			unsigned int buflen)
{
	int np = (buflen + PAGE_SIZE - 1)/PAGE_SIZE;
	int k, rem;

	if (np > XBUFSIZE) {
		rem = PAGE_SIZE;
		np = XBUFSIZE;
	} else {
		rem = buflen % PAGE_SIZE;
	}

	sg_init_table(sg, np + 1);
	np--;
	for (k = 0; k < np; k++)
		sg_set_buf(&sg[k + 1], xbuf[k], PAGE_SIZE);

	sg_set_buf(&sg[k + 1], xbuf[k], rem);
}

static void test_aead_speed(const char *algo, int enc, unsigned int secs,
			struct aead_speed_template *template,
			unsigned int tcount, u8 authsize,
			unsigned int aad_size, u8 *keysize)
{
	unsigned int i, j;
	struct crypto_aead *tfm;
	int ret = -ENOMEM;
	const char *key;
	struct aead_request *req;
	struct scatterlist *sg;
	struct scatterlist *sgout;
	const char *e;
	void *assoc;
	char *iv;
	char *xbuf[XBUFSIZE];
	char *xoutbuf[XBUFSIZE];
	char *axbuf[XBUFSIZE];
	unsigned int *b_size;
	unsigned int iv_len;
	struct tcrypt_result result;

	iv = kzalloc(MAX_IVLEN, GFP_KERNEL);
	if (!iv)
		return;

	if (aad_size >= PAGE_SIZE) {
		pr_err("associate data length (%u) too big\n", aad_size);
		goto out_noxbuf;
	}

	if (enc == ENCRYPT)
		e = "encryption";
	else
		e = "decryption";

	if (testmgr_alloc_buf(xbuf))
		goto out_noxbuf;
	if (testmgr_alloc_buf(axbuf))
		goto out_noaxbuf;
	if (testmgr_alloc_buf(xoutbuf))
		goto out_nooutbuf;

	sg = kmalloc(sizeof(*sg) * 9 * 2, GFP_KERNEL);
	if (!sg)
		goto out_nosg;
	sgout = &sg[9];

	tfm = crypto_alloc_aead(algo, 0, 0);

	if (IS_ERR(tfm)) {
		pr_err("alg: aead: Failed to load transform for %s: %ld\n",
			algo, PTR_ERR(tfm));
		goto out_notfm;
	}

	init_completion(&result.completion);
	pr_info("\ntesting speed of %s (%s) %s\n", algo,
			get_driver_name(crypto_aead, tfm), e);

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("alg: aead: Failed to allocate request for %s\n",
			algo);
		goto out_noreq;
	}

	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				  tcrypt_complete, &result);

	i = 0;
	do {
		b_size = aead_sizes;
		do {
			assoc = axbuf[0];
			memset(assoc, 0xff, aad_size);

			if ((*keysize + *b_size) > TVMEMSIZE * PAGE_SIZE) {
				pr_err("template(%u) too big for tvmem (%lu)\n",
					*keysize + *b_size,
					TVMEMSIZE * PAGE_SIZE);
				goto out;
			}

			key = tvmem[0];
			for (j = 0; j < tcount; j++) {
				if (template[j].klen == *keysize) {
					key = template[j].key;
					break;
				}
			}
			ret = crypto_aead_setkey(tfm, key, *keysize);
			ret = crypto_aead_setauthsize(tfm, authsize);

			iv_len = crypto_aead_ivsize(tfm);
			if (iv_len)
				memset(iv, 0xff, iv_len);

			crypto_aead_clear_flags(tfm, ~0);
			pr_info("test %u (%d bit key,%d byte blocks):",
					i, *keysize * 8, *b_size);


			memset(tvmem[0], 0xff, PAGE_SIZE);

			if (ret) {
				pr_err("setkey() failed flags=%x\n",
						crypto_aead_get_flags(tfm));
				goto out;
			}

			sg_init_aead(sg, xbuf,
				    *b_size + (enc ? authsize : 0));

			sg_init_aead(sgout, xoutbuf,
				    *b_size + (enc ? authsize : 0));

			sg_set_buf(&sg[0], assoc, aad_size);
			sg_set_buf(&sgout[0], assoc, aad_size);

			aead_request_set_crypt(req, sg, sgout, *b_size, iv);
			aead_request_set_ad(req, aad_size);

			if (secs)
				ret = test_aead_jiffies(req, enc, *b_size,
							secs);
			else
				ret = test_aead_cycles(req, enc, *b_size);

			if (ret) {
				pr_err("%s() failed return code=%d\n", e, ret);
				break;
			}
			b_size++;
			i++;
		} while (*b_size);
		keysize++;
	} while (*keysize);

out:
	aead_request_free(req);
out_noreq:
	crypto_free_aead(tfm);
out_notfm:
	kfree(sg);
out_nosg:
	testmgr_free_buf(xoutbuf);
out_nooutbuf:
	testmgr_free_buf(axbuf);
out_noaxbuf:
	testmgr_free_buf(xbuf);
out_noxbuf:
	kfree(iv);
	return;
}

static void test_cipher_speed(const char *algo, int enc, unsigned int secs,
				struct cipher_speed_template *template,
				unsigned int tcount, u8 *keysize)
{
	unsigned int ret, i, j, iv_len;
	const char *key;
	char iv[128];
	struct crypto_blkcipher *tfm;
	struct blkcipher_desc desc;
	const char *e;
	u32 *b_size;

	if (enc == ENCRYPT)
		e = "encryption";
	else
		e = "decryption";

	tfm = crypto_alloc_blkcipher(algo, 0, CRYPTO_ALG_ASYNC);

	if (IS_ERR(tfm)) {
		pr_info("failed to load transform for %s: %ld\n", algo,
			PTR_ERR(tfm));
		return;
	}
	desc.tfm = tfm;
	desc.flags = 0;

	pr_info("\ntesting speed of %s (%s) %s\n", algo,
			get_driver_name(crypto_blkcipher, tfm), e);

	i = 0;
	do {

		b_size = block_sizes;
		do {
			struct scatterlist sg[TVMEMSIZE];

			if ((*keysize + *b_size) > TVMEMSIZE * PAGE_SIZE) {
				pr_info("template %u too big for tvmem (%lu)\n",
					*keysize + *b_size,
					TVMEMSIZE * PAGE_SIZE);
				goto out;
			}

			pr_info("test %u (%d bit key, %d byte blocks): ",
					i, *keysize * 8, *b_size);

			memset(tvmem[0], 0xff, PAGE_SIZE);

			/* set key, plain text and IV */
			key = tvmem[0];
			for (j = 0; j < tcount; j++) {
				if (template[j].klen == *keysize) {
					key = template[j].key;
					break;
				}
			}

			ret = crypto_blkcipher_setkey(tfm, key, *keysize);
			if (ret) {
				pr_info("setkey() failed flags=%x\n",
					crypto_blkcipher_get_flags(tfm));
				goto out;
			}

			sg_init_table(sg, TVMEMSIZE);
			sg_set_buf(sg, tvmem[0] + *keysize,
				PAGE_SIZE - *keysize);
			for (j = 1; j < TVMEMSIZE; j++) {
				sg_set_buf(sg + j, tvmem[j], PAGE_SIZE);
				memset (tvmem[j], 0xff, PAGE_SIZE);
			}

			iv_len = crypto_blkcipher_ivsize(tfm);
			if (iv_len) {
				memset(&iv, 0xff, iv_len);
				crypto_blkcipher_set_iv(tfm, iv, iv_len);
			}

			if (secs)
				ret = test_cipher_jiffies(&desc, enc, sg,
							*b_size, secs);
			else
				ret = test_cipher_cycles(&desc, enc, sg,
							*b_size);

			if (ret) {
				pr_info("%s() failed flags=%x\n",
					e, desc.flags);
				break;
			}
			b_size++;
			i++;
		} while (*b_size);
		keysize++;
	} while (*keysize);

out:
	crypto_free_blkcipher(tfm);
}

static int test_hash_jiffies_digest(struct hash_desc *desc,
				struct scatterlist *sg, int blen,
				char *out, int secs)
{
	unsigned long start, end;
	int bcount;
	int ret;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		ret = crypto_hash_digest(desc, sg, blen, out);
		if (ret)
			return ret;
	}

	pr_info("%6u opers/sec, %9lu bytes/sec\n",
		bcount / secs, ((long)bcount * blen) / secs);

	return 0;
}

static int test_hash_jiffies(struct hash_desc *desc, struct scatterlist *sg,
			int blen, int plen, char *out, int secs)
{
	unsigned long start, end;
	int bcount, pcount;
	int ret;

	if (plen == blen)
		return test_hash_jiffies_digest(desc, sg, blen, out, secs);

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		ret = crypto_hash_init(desc);
		if (ret)
			return ret;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = crypto_hash_update(desc, sg, plen);
			if (ret)
				return ret;
		}
		/* we assume there is enough space in 'out' for the result */
		ret = crypto_hash_final(desc, out);
		if (ret)
			return ret;
	}

	pr_info("%6u opers/sec, %9lu bytes/sec\n",
		bcount / secs, ((long)bcount * blen) / secs);

	return 0;
}

static int test_hash_cycles_digest(struct hash_desc *desc,
				struct scatterlist *sg, int blen, char *out)
{
	unsigned long cycles = 0;
	int i;
	int ret;

	local_irq_disable();

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = crypto_hash_digest(desc, sg, blen, out);
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();

		ret = crypto_hash_digest(desc, sg, blen, out);
		if (ret)
			goto out;

		end = get_cycles();

		cycles += end - start;
	}

out:
	local_irq_enable();

	if (ret)
		return ret;

	pr_info("%6lu cycles/operation, %4lu cycles/byte\n",
		cycles / 8, cycles / (8 * blen));

	return 0;
}

static int test_hash_cycles(struct hash_desc *desc, struct scatterlist *sg,
				int blen, int plen, char *out)
{
	unsigned long cycles = 0;
	int i, pcount;
	int ret;

	if (plen == blen)
		return test_hash_cycles_digest(desc, sg, blen, out);

	local_irq_disable();

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = crypto_hash_init(desc);
		if (ret)
			goto out;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = crypto_hash_update(desc, sg, plen);
			if (ret)
				goto out;
		}
		ret = crypto_hash_final(desc, out);
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();

		ret = crypto_hash_init(desc);
		if (ret)
			goto out;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = crypto_hash_update(desc, sg, plen);
			if (ret)
				goto out;
		}
		ret = crypto_hash_final(desc, out);
		if (ret)
			goto out;

		end = get_cycles();

		cycles += end - start;
	}

out:
	local_irq_enable();

	if (ret)
		return ret;

	pr_info("%6lu cycles/operation, %4lu cycles/byte\n",
		cycles / 8, cycles / (8 * blen));

	return 0;
}

static void test_hash_sg_init(struct scatterlist *sg, unsigned long dsize)
{
	int i;

	if (dsize) {
		sg_init_table(sg, 1);
		sg_set_buf(sg, tvmem[0], dsize);
		memset(tvmem[0], 0xff, dsize);
	} else {
		sg_init_table(sg, TVMEMSIZE);
		for (i = 0; i < TVMEMSIZE; i++) {
			sg_set_buf(sg + i, tvmem[i], PAGE_SIZE);
			memset(tvmem[i], 0xff, PAGE_SIZE);
		}
	}
}

static void test_hash_speed(const char *algo, unsigned int secs,
			struct hash_speed *speed)
{
	struct scatterlist sg[TVMEMSIZE];
	struct crypto_hash *tfm;
	struct hash_desc desc;
	static char output[1024];
	int i;
	int ret;

	tfm = crypto_alloc_hash(algo, 0, CRYPTO_ALG_ASYNC);

	if (IS_ERR(tfm)) {
		pr_info("failed to load transform for %s: %ld\n", algo,
			PTR_ERR(tfm));
		return;
	}

	pr_info("\ntesting speed of %s (%s)\n", algo,
			get_driver_name(crypto_hash, tfm));

	desc.tfm = tfm;
	desc.flags = 0;

	if (crypto_hash_digestsize(tfm) > sizeof(output)) {
		pr_info("digestsize(%u) > outputbuffer(%zu)\n",
			crypto_hash_digestsize(tfm), sizeof(output));
		goto out;
	}

	test_hash_sg_init(sg, 0);
	for (i = 0; speed[i].blen != 0; i++) {
		if (speed[i].blen > TVMEMSIZE * PAGE_SIZE) {
			pr_info(
				"template (%u) too big for tvmem (%lu)\n",
				speed[i].blen, TVMEMSIZE * PAGE_SIZE);
			goto out;
		}

		if (speed[i].klen)
			crypto_hash_setkey(tfm, tvmem[0], speed[i].klen);

		pr_info(
		"test%3u (%5u byte blocks,%5u bytes per update,%4u updates): ",
			i, speed[i].blen, speed[i].plen,
			speed[i].blen / speed[i].plen);
		if (secs)
			ret = test_hash_jiffies(&desc, sg, speed[i].blen,
						speed[i].plen, output, secs);
		else
			ret = test_hash_cycles(&desc, sg, speed[i].blen,
						speed[i].plen, output);
		if (ret) {
			pr_info("hashing failed ret=%d\n", ret);
			break;
		}
	}
out:
	crypto_free_hash(tfm);
}

static inline int do_one_ahash_op(struct ahash_request *req, int ret)
{
	if (ret == -EINPROGRESS || ret == -EBUSY) {
		struct tcrypt_result *tr = req->base.data;

		wait_for_completion(&tr->completion);
		reinit_completion(&tr->completion);
		ret = tr->err;
	}
	return ret;
}

static int test_ahash_jiffies_digest(struct ahash_request *req, int blen,
					char *out, int secs)
{
	unsigned long start, end;
	int bcount;
	int ret;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			return ret;
	}

	pr_info("%6u opers/sec, %9lu bytes/sec\n",
		bcount / secs, ((long)bcount * blen) / secs);

	return 0;
}

static int test_ahash_jiffies(struct ahash_request *req, int blen,
				int plen, char *out, int secs)
{
	unsigned long start, end;
	int bcount, pcount;
	int ret;

	if (plen == blen)
		return test_ahash_jiffies_digest(req, blen, out, secs);

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		ret = do_one_ahash_op(req, crypto_ahash_init(req));
		if (ret)
			return ret;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = do_one_ahash_op(req, crypto_ahash_update(req));
			if (ret)
				return ret;
		}
		/* we assume there is enough space in 'out' for the result */
		ret = do_one_ahash_op(req, crypto_ahash_final(req));
		if (ret)
			return ret;
	}

	pr_cont("%6u opers/sec, %9lu bytes/sec\n",
		bcount / secs, ((long)bcount * blen) / secs);

	return 0;
}

static int test_ahash_perf(struct ahash_request *req, unsigned long dsize)
{
	int ret, i;
	struct timespec before, after;
	unsigned long before_t, after_t;
	unsigned long tot_time = 0;
	unsigned long long bps = 0;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			return ret;
	}
	/* The real thing. */
	for (i = 0; i < 10; i++) {
		getnstimeofday(&before);

		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			return ret;

		getnstimeofday(&after);

		before_t = before.tv_nsec;
		after_t = ((after.tv_sec - before.tv_sec) * 1000000000) +
				after.tv_nsec;

		tot_time += (after_t - before_t);
	}

	tot_time = tot_time / 10;
	bps = (unsigned long long)(dsize * 1000000000) / (tot_time);

	pr_info("\nPerformance: %llu MegaBytes/sec", (bps / (1024 * 1024)));

	return 0;
}

static int test_ahash_cycles_digest(struct ahash_request *req, int blen,
				char *out)
{
	unsigned long cycles = 0;
	int ret, i;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();

		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			goto out;

		end = get_cycles();

		cycles += end - start;
	}

out:
	if (ret)
		return ret;

	pr_cont("%6lu cycles/operation, %4lu cycles/byte\n",
		cycles / 8, cycles / (8 * blen));

	return 0;
}

static int test_ahash_cycles(struct ahash_request *req, int blen,
			int plen, char *out)
{
	unsigned long cycles = 0;
	int i, pcount, ret;

	if (plen == blen)
		return test_ahash_cycles_digest(req, blen, out);

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_one_ahash_op(req, crypto_ahash_init(req));
		if (ret)
			goto out;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = do_one_ahash_op(req, crypto_ahash_update(req));
			if (ret)
				goto out;
		}
		ret = do_one_ahash_op(req, crypto_ahash_final(req));
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();

		ret = do_one_ahash_op(req, crypto_ahash_init(req));
		if (ret)
			goto out;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = do_one_ahash_op(req, crypto_ahash_update(req));
			if (ret)
				goto out;
		}
		ret = do_one_ahash_op(req, crypto_ahash_final(req));
		if (ret)
			goto out;

		end = get_cycles();

		cycles += end - start;
	}

out:
	if (ret)
		return ret;

	pr_cont("%6lu cycles/operation, %4lu cycles/byte\n",
		cycles / 8, cycles / (8 * blen));

	return 0;
}

static void test_ahash_speed(const char *algo, unsigned int secs,
			unsigned int dsize, struct hash_speed *speed)
{
	struct scatterlist sg[TVMEMSIZE];
	struct tcrypt_result tresult;
	struct ahash_request *req;
	struct crypto_ahash *tfm;
	char *output;
	int i, ret;

	tfm = crypto_alloc_ahash(algo, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n",
			algo, PTR_ERR(tfm));
		return;
	}

	pr_info("\ntesting speed of async %s (%s)\n", algo,
			get_driver_name(crypto_ahash, tfm));

	if (crypto_ahash_digestsize(tfm) > MAX_DIGEST_SIZE) {
		pr_err("digestsize(%u) > %d\n", crypto_ahash_digestsize(tfm),
			MAX_DIGEST_SIZE);
		goto out;
	}

	test_hash_sg_init(sg, dsize);

	req = ahash_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("ahash request allocation failure\n");
		goto out;
	}

	init_completion(&tresult.completion);
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				tcrypt_complete, &tresult);

	output = kmalloc(MAX_DIGEST_SIZE, GFP_KERNEL);
	if (!output)
		goto out_nomem;

	for (i = 0; (!dsize && speed[i].blen != 0); i++) {
		if (speed[i].blen > TVMEMSIZE * PAGE_SIZE) {
			pr_err("template (%u) too big for tvmem (%lu)\n",
				speed[i].blen, TVMEMSIZE * PAGE_SIZE);
			break;
		}

		pr_info("test%3u "
			"(%5u byte blocks,%5u bytes per update,%4u updates): ",
				i, speed[i].blen, speed[i].plen,
				speed[i].blen / speed[i].plen);

		ahash_request_set_crypt(req, sg, output, speed[i].plen);

		if (secs)
			ret = test_ahash_jiffies(req, speed[i].blen,
						speed[i].plen, output, secs);
		else
			ret = test_ahash_cycles(req, speed[i].blen,
						speed[i].plen, output);

		if (ret) {
			pr_err("hashing failed ret=%d\n", ret);
			break;
		}
	}

	if (dsize) {
		ahash_request_set_crypt(req, &sg[0], output, dsize);
		ret = test_ahash_perf(req, dsize);
		if (ret)
			pr_err("hashing failed ret=%d\n", ret);
	}

	kfree(output);

out_nomem:
	ahash_request_free(req);

out:
	crypto_free_ahash(tfm);
}

static inline int do_one_acipher_op(struct ablkcipher_request *req, int ret)
{
	if (ret == -EINPROGRESS || ret == -EBUSY) {
		struct tcrypt_result *tr = req->base.data;

		wait_for_completion(&tr->completion);
		reinit_completion(&tr->completion);
		ret = tr->err;
	}

	return ret;
}

static int test_acipher_jiffies(struct ablkcipher_request *req, int enc,
				int blen, int secs)
{
	unsigned long start, end;
	int bcount;
	int ret;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		if (enc)
			ret = do_one_acipher_op(req,
						crypto_ablkcipher_encrypt(req));
		else
			ret = do_one_acipher_op(req,
						crypto_ablkcipher_decrypt(req));
		if (ret)
			return ret;
	}

	pr_cont("%d operations in %d seconds (%ld bytes)\n",
		bcount, secs, (long)bcount * blen);
	return 0;
}

static int test_acipher_cycles(struct ablkcipher_request *req, int enc,
				int blen)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		if (enc)
			ret = do_one_acipher_op(req,
						crypto_ablkcipher_encrypt(req));
		else
			ret = do_one_acipher_op(req,
						crypto_ablkcipher_decrypt(req));

		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (enc)
			ret = do_one_acipher_op(req,
					crypto_ablkcipher_encrypt(req));
		else
			ret = do_one_acipher_op(req,
					crypto_ablkcipher_decrypt(req));
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

out:
	if (ret == 0)
		pr_cont("1 operation in %lu cycles (%d bytes)\n",
			(cycles + 4) / 8, blen);

	return ret;
}

#define CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_AMOUNT (32*512)
#define CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_SIZE (1024/2)
#define CUSTOMIZED_ACIPHER_SPEED_TEST_TOTAL_BYTES \
		(CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_AMOUNT * \
		CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_SIZE)
#define CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE 16
#define CUSTOMIZED_ACIPHER_SPEED_TEST_MAX_OUTSTANDING_BLOCKS 1024
#define CUSTOMIZED_ACIPHER_SPEED_TEST_NO_RUNS 5
#define CUSTOMIZED_ACIPHER_SPEED_TEST_TARGET_ENCRYPT_SPEED 240
#define CUSTOMIZED_ACIPHER_SPEED_TEST_TARGET_DECRYPT_SPEED 260

static atomic_t atomic_counter;

struct customized_tcrypt_result {
	u8 iv[CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE];
	u8 *block;
	struct completion completion;
	struct completion restart;
	struct ablkcipher_request *req;
	struct scatterlist sg;
	int err;
};

static void customized_tcrypt_complete(struct crypto_async_request *req,
					int err)
{
	struct customized_tcrypt_result *res = req->data;

	if (err == -EINPROGRESS) {
		complete(&res->restart);
		return;
	}

	res->err = err;
	atomic_add(1, &atomic_counter);
	ablkcipher_request_free(res->req);
	kfree(res->block);
}

static unsigned int customized_blocks[] = {
		1024 / 2,
		1024,
		1024 * 2,
		1024 * 4,
		1024 * 8,
		1024 * 16,
		1024 * 32,
		1024 * 64
};

static unsigned int acipher_speed(const char *algo, int enc,
				unsigned int bsize, unsigned int bcnt)
{
	unsigned int ret, k, perf = 0;
	const char *e;
	struct crypto_ablkcipher *tfm;
	u8 keysize = CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE;
	u32 blocksize = customized_blocks[bsize];
	char key[32] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa,
		0xb, 0xc, 0xd, 0xe, 0xf, 0xf, 0xe, 0xd, 0xc, 0xb, 0xa, 0x9, 0x8,
		0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0x0 };
	struct timespec before, after;
	unsigned long before_a, after_a, diff_in_ms;
	unsigned long blocks_to_test =
		CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_AMOUNT * bcnt;
	unsigned long bytes_tested = blocks_to_test * blocksize;
	unsigned long bytes_per_ms = 0;
	u32 val = 0;

	atomic_set(&atomic_counter, 0);

	if (enc == ENCRYPT) {
		e = "encryption";
		pr_info("Testing Encryption\n");
	} else {
		e = "decryption";
		pr_info("Testing Decryption\n");
	}

	tfm = crypto_alloc_ablkcipher(algo, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n", algo,
				PTR_ERR(tfm));
		return PTR_ERR(tfm);
	}

	pr_info("testing speed of async %s (%s) %s\n", algo,
			get_driver_name(crypto_ablkcipher, tfm), e);
	pr_info("testing  (%d bit key, %d byte blocks)\n",
			keysize * 8, blocksize);

	memset(tvmem[0], 0xff, PAGE_SIZE);

	crypto_ablkcipher_clear_flags(tfm, ~0);

	ret = crypto_ablkcipher_setkey(tfm, key, keysize);
	if (ret) {
		pr_err("setkey() failed flags=%x\n",
				crypto_ablkcipher_get_flags(tfm));
		goto out;
	}

	getnstimeofday(&before);

	for (k = 0; k < blocks_to_test; k++) {
		struct ablkcipher_request *req;
		u8 *alloc_addr;
		struct customized_tcrypt_result *tresult;
		struct scatterlist *sg;
		u8 *block, *iv;

		alloc_addr = kmalloc(((blocksize / PAGE_SIZE) + 1) * PAGE_SIZE,
				GFP_KERNEL);
		tresult = (struct customized_tcrypt_result *)
						(alloc_addr + blocksize);
		if (!tresult) {
			pr_err("out of memory?\n");
			goto out;
		}
		tresult->block = alloc_addr;

		init_completion(&tresult->completion);
		init_completion(&tresult->restart);

		req = ablkcipher_request_alloc(tfm, GFP_KERNEL);
		if (!req) {
			pr_err(
			"tcrypt: skcipher:Failed to allocate request for %s\n",
				algo);
			goto out;
		}

		ablkcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			customized_tcrypt_complete, tresult);
		tresult->req = req;

		sg = &tresult->sg;
		block = tresult->block;
		iv = tresult->iv;
		sg_init_table(sg, 1);
		sg_set_buf(sg, block, blocksize);

		memset(iv, k % CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE,
			CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE);

		ablkcipher_request_set_crypt(req, sg, sg, blocksize, iv);
		if (enc)
			ret = crypto_ablkcipher_encrypt(req);
		else
			ret = crypto_ablkcipher_decrypt(req);

		switch (ret) {
		/* async */
		case -EBUSY:
			wait_for_completion_interruptible(&tresult->restart);
			reinit_completion(&tresult->restart);
			break;
		case -EINPROGRESS:
			break;
			/* sync */
		case 0:
			customized_tcrypt_complete(&req->base, 0);
			break;
			/* error */
		default:
			pr_err("error detected\n");
			return ret;
		}
	}

	while (val < blocks_to_test)
		val = atomic_read(&atomic_counter);

	getnstimeofday(&after);
	before_a = before.tv_nsec;
	after_a = ((after.tv_sec - before.tv_sec) * 1000000000) + after.tv_nsec;
	diff_in_ms = (after_a - before_a) / 1000000;

	pr_info("difference: %ld(ms)\n", diff_in_ms);
	pr_info("bytes tested: %ldMB %ldKB %ldB\n",
		bytes_tested / 1024 / 1024, (bytes_tested / 1024) % 1024,
		bytes_tested % 1024);

	bytes_per_ms = bytes_tested / diff_in_ms;
	perf = (bytes_per_ms * 1000) / (1024 * 1024);
	pr_info("Test speed: %ld.%03ld(MB/s)\n",
		(bytes_per_ms * 1000) / (1024 * 1024),
		((bytes_per_ms * 1000) / 1024) % 1024);
out:
	crypto_free_ablkcipher(tfm);

	return perf;
}

static int customized_test_acipher_speed(const char *algo, unsigned int bsize,
		unsigned int bcnt)
{
	int i, no_runs, target_enc_speed, target_dec_speed;
	int max_enc_speed = 0, max_dec_speed = 0, speed;

	no_runs = CUSTOMIZED_ACIPHER_SPEED_TEST_NO_RUNS;
	target_enc_speed = CUSTOMIZED_ACIPHER_SPEED_TEST_TARGET_ENCRYPT_SPEED;
	target_dec_speed = CUSTOMIZED_ACIPHER_SPEED_TEST_TARGET_DECRYPT_SPEED;

	for (i = 0; i < no_runs; i++) {
		speed = acipher_speed("cbc(aes)", ENCRYPT, bsize, bcnt);
		if (max_enc_speed < speed)
			max_enc_speed = speed;
		speed = acipher_speed("cbc(aes)", DECRYPT, bsize, bcnt);
		if (max_dec_speed < speed)
			max_dec_speed = speed;
	}

	pr_info("Target Encrypt speed: %d(MB/s) Decrypt speed: %d(MB/s)\n",
		target_enc_speed, target_dec_speed);
	pr_info("Test Encrypt speed: %d(MB/s) Decrypt speed: %d(MB/s)\n",
		max_enc_speed, max_dec_speed);

	if (max_enc_speed >= target_enc_speed &&
		       max_dec_speed >= target_dec_speed)
		return 0;
	else {
		pr_err("AES Encrypt/Decrypt target performance is not met\n");
		return 1;
	}
}

static void test_acipher_speed(const char *algo, int enc, unsigned int secs,
				struct cipher_speed_template *template,
				unsigned int tcount, u8 *keysize)
{
	unsigned int ret, i, j, k, iv_len;
	struct tcrypt_result tresult;
	const char *key;
	char iv[128];
	struct ablkcipher_request *req;
	struct crypto_ablkcipher *tfm;
	const char *e;
	u32 *b_size;

	if (enc == ENCRYPT)
		e = "encryption";
	else
		e = "decryption";

	init_completion(&tresult.completion);

	tfm = crypto_alloc_ablkcipher(algo, 0, 0);

	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n", algo,
			PTR_ERR(tfm));
		return;
	}

	pr_info("\ntesting speed of async %s (%s) %s\n", algo,
			get_driver_name(crypto_ablkcipher, tfm), e);

	req = ablkcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("tcrypt: skcipher: Failed to allocate request for %s\n",
			algo);
		goto out;
	}

	ablkcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
					tcrypt_complete, &tresult);

	i = 0;
	do {
		b_size = block_sizes;

		do {
			struct scatterlist sg[TVMEMSIZE];

			if ((*keysize + *b_size) > TVMEMSIZE * PAGE_SIZE) {
				pr_err(
				"template (%u) too big for tvmem (%lu)\n",
					*keysize + *b_size,
					TVMEMSIZE * PAGE_SIZE);
				goto out_free_req;
			}

			pr_info("test %u (%d bit key, %d byte blocks): ", i,
				*keysize * 8, *b_size);

			memset(tvmem[0], 0xff, PAGE_SIZE);

			/* set key, plain text and IV */
			key = tvmem[0];
			for (j = 0; j < tcount; j++) {
				if (template[j].klen == *keysize) {
					key = template[j].key;
					break;
				}
			}

			crypto_ablkcipher_clear_flags(tfm, ~0);

			ret = crypto_ablkcipher_setkey(tfm, key, *keysize);
			if (ret) {
				pr_err("setkey() failed flags=%x\n",
					crypto_ablkcipher_get_flags(tfm));
				goto out_free_req;
			}

			k = *keysize + *b_size;
			sg_init_table(sg, DIV_ROUND_UP(k, PAGE_SIZE));

			if (k > PAGE_SIZE) {
				sg_set_buf(sg, tvmem[0] + *keysize,
					PAGE_SIZE - *keysize);
				k -= PAGE_SIZE;
				j = 1;
				while (k > PAGE_SIZE) {
					sg_set_buf(sg + j, tvmem[j], PAGE_SIZE);
					memset(tvmem[j], 0xff, PAGE_SIZE);
					j++;
					k -= PAGE_SIZE;
				}
				sg_set_buf(sg + j, tvmem[j], k);
				memset(tvmem[j], 0xff, k);
			} else {
				sg_set_buf(sg, tvmem[0] + *keysize, *b_size);
			}

			iv_len = crypto_ablkcipher_ivsize(tfm);
			if (iv_len)
				memset(&iv, 0xff, iv_len);

			ablkcipher_request_set_crypt(req, sg, sg, *b_size, iv);

			if (secs)
				ret = test_acipher_jiffies(req, enc,
							*b_size, secs);
			else
				ret = test_acipher_cycles(req, enc,
							*b_size);

			if (ret) {
				pr_err("%s() failed flags=%x\n", e,
					crypto_ablkcipher_get_flags(tfm));
				break;
			}
			b_size++;
			i++;
		} while (*b_size);
		keysize++;
	} while (*keysize);

out_free_req:
	ablkcipher_request_free(req);
out:
	crypto_free_ablkcipher(tfm);
}

static inline int do_one_akcipher_op(struct akcipher_request *r, int ret)
{
	if (ret == -EINPROGRESS || ret == -EBUSY) {
		struct tcrypt_result *tr = r->base.data;

		wait_for_completion(&tr->completion);
		reinit_completion(&tr->completion);
		ret = tr->err;
	}
	return ret;
}

static int test_akcipher_jiffies(struct akcipher_request *r, int op, int secs)
{
	unsigned long start, end;
	int count, ret;

	for (start = jiffies, end = start + secs * HZ, count = 0;
	     time_before(jiffies, end); count++) {

		switch (op) {
		case SIGN:
			ret = do_one_akcipher_op(r, crypto_akcipher_sign(r));
			break;
		case VERIFY:
			ret = do_one_akcipher_op(r, crypto_akcipher_verify(r));
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret)
			return ret;
	}

	pr_info("%d operations in %d seconds\n", count, secs);
	return 0;
}

static int test_akcipher_cycles(struct akcipher_request *r, int op)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		switch (op) {
		case SIGN:
			ret = do_one_akcipher_op(r, crypto_akcipher_sign(r));
			break;
		case VERIFY:
			ret = do_one_akcipher_op(r, crypto_akcipher_verify(r));
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		switch (op) {
		case SIGN:
			ret = do_one_akcipher_op(r, crypto_akcipher_sign(r));
			break;
		case VERIFY:
			ret = do_one_akcipher_op(r, crypto_akcipher_verify(r));
			break;
		default:
			ret = -EINVAL;
			break;
		}
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}
out:
	if (ret == 0)
		pr_info("1 operation in %lu cycles\n", (cycles + 4) / 8);

	return ret;
}

static void test_akcipher_speed(const char *algo, int op, unsigned int secs,
				struct akcipher_speed_template *template,
				unsigned int tcount, u8 *keysize)
{
	unsigned int ret, i, j;
	struct tcrypt_result tresult;
	const char *key;
	struct akcipher_request *req;
	struct crypto_akcipher *tfm;
	unsigned int m_size = 0;
	unsigned int nbytes = 0;
	const char *o;

	if (op == SIGN)
		o = "sign";
	else if (op == VERIFY)
		o = "verify";
	else
		return;

	tfm = crypto_alloc_akcipher(algo, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n", algo,
		       PTR_ERR(tfm));
		return;
	}

	req = akcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("tcrypt: akcipher: Failed to allocate request for %s\n",
		       algo);
		goto out;
	}

	init_completion(&tresult.completion);
	akcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      tcrypt_complete, &tresult);

	i = 0;
	do {
		struct scatterlist sg[TVMEMSIZE];

		memset(tvmem[0], 0xff, PAGE_SIZE);

		/* set key */
		key = tvmem[0];
		for (j = 0; j < tcount; j++) {
			if (template[j].key_len == *keysize) {
				key = template[j].key;
				break;
			}
		}

		ret = crypto_akcipher_set_pub_key(tfm, key, *keysize);
		if (ret) {
			pr_err("set_pub_key() failed\n");
			goto out_free_req;
		}

		ret = crypto_akcipher_set_priv_key(tfm, key, *keysize);
		if (ret) {
			pr_err("set_priv_key() failed\n");
			goto out_free_req;
		}

		/* set up src/dst buffs */
		sg_init_table(sg, TVMEMSIZE);
		if (op == SIGN) {
			m_size = template[j].m_size;
			nbytes = template[j].c_size / 3;

			memcpy(tvmem[0], template[j].m, m_size);

			sg_set_buf(&sg[0], tvmem[0], m_size);
			akcipher_request_set_crypt(req, sg, sg,
						   m_size, PAGE_SIZE);
		} else if (op == VERIFY) {
			m_size = template[j].m_size;
			nbytes = template[j].c_size / 3;

			memcpy(tvmem[0], template[j].m, m_size);
			memcpy(tvmem[1], (u8 *)(template[j].c) + nbytes,
			       nbytes);
			memcpy(tvmem[2], (u8 *)(template[j].c) + 2 * nbytes,
			       nbytes);

			sg_set_buf(&sg[0], tvmem[0], m_size);
			sg_set_buf(&sg[1], tvmem[1], nbytes);
			sg_set_buf(&sg[2], tvmem[2], nbytes);

			akcipher_request_set_crypt(req, sg, sg,
						   m_size + 2 * nbytes,
						   PAGE_SIZE);
		} else {
			pr_err("invalid op\n");
			ret = -EINVAL;
			goto out_free_req;
		}


		pr_info("\ntesting speed of %s (%s) %s with keysize %d\n",
			algo, get_driver_name(crypto_akcipher, tfm), o,
			nbytes * 8);

		if (secs)
			ret = test_akcipher_jiffies(req, op, secs);
		else
			ret = test_akcipher_cycles(req, op);

		if (ret) {
			pr_err("%s() failed\n", o);
			break;
		}

		i++;
		keysize++;

	} while (*keysize);

out_free_req:
	akcipher_request_free(req);
out:
	crypto_free_akcipher(tfm);
}

static void test_available(void)
{
	char **name = check;

	while (*name) {
		pr_info("alg %s %s", *name,
			crypto_has_alg(*name, 0, 0) ?
			"found\n" : "not found\n");
		name++;
	}
}

static inline int tcrypt_test(const char *alg)
{
	int ret;

	ret = alg_test(alg, alg, 0, 0);
	/* non-fips algs return -EINVAL in fips mode */
	if (fips_enabled && ret == -EINVAL)
		ret = 0;
	return ret;
}

static int do_test(const char *alg, u32 type, u32 mask, int m)
{
	int i;
	int ret = 0;

	switch (m) {
	case 0:
		if (alg) {
			if (!crypto_has_alg(alg, type,
					    mask ?: CRYPTO_ALG_TYPE_MASK))
				ret = -ENOENT;
			break;
		}

		for (i = 1; i < 200; i++)
			ret += do_test(NULL, 0, 0, i);
		break;

	case 1:
		ret += tcrypt_test("md5");
		break;

	case 2:
		ret += tcrypt_test("sha1");
		break;

	case 3:
		ret += tcrypt_test("ecb(des)");
		ret += tcrypt_test("cbc(des)");
		ret += tcrypt_test("ctr(des)");
		break;

	case 4:
		ret += tcrypt_test("ecb(des3_ede)");
		ret += tcrypt_test("cbc(des3_ede)");
		ret += tcrypt_test("ctr(des3_ede)");
		break;

	case 5:
		ret += tcrypt_test("md4");
		break;

	case 6:
		ret += tcrypt_test("sha256");
		break;

	case 7:
		ret += tcrypt_test("ecb(blowfish)");
		ret += tcrypt_test("cbc(blowfish)");
		ret += tcrypt_test("ctr(blowfish)");
		break;

	case 8:
		ret += tcrypt_test("ecb(twofish)");
		ret += tcrypt_test("cbc(twofish)");
		ret += tcrypt_test("ctr(twofish)");
		ret += tcrypt_test("lrw(twofish)");
		ret += tcrypt_test("xts(twofish)");
		break;

	case 9:
		ret += tcrypt_test("ecb(serpent)");
		ret += tcrypt_test("cbc(serpent)");
		ret += tcrypt_test("ctr(serpent)");
		ret += tcrypt_test("lrw(serpent)");
		ret += tcrypt_test("xts(serpent)");
		break;

	case 10:
		ret += tcrypt_test("ecb(aes)");
		ret += tcrypt_test("cbc(aes)");
		ret += tcrypt_test("ctr(aes)");
		ret += tcrypt_test("ofb(aes)");
		break;

	case 11:
		ret += tcrypt_test("sha384");
		break;

	case 12:
		ret += tcrypt_test("sha512");
		break;

	case 13:
		ret += tcrypt_test("deflate");
		break;

	case 14:
		ret += tcrypt_test("ecb(cast5)");
		ret += tcrypt_test("cbc(cast5)");
		ret += tcrypt_test("ctr(cast5)");
		break;

	case 15:
		ret += tcrypt_test("ecb(cast6)");
		ret += tcrypt_test("cbc(cast6)");
		ret += tcrypt_test("ctr(cast6)");
		ret += tcrypt_test("lrw(cast6)");
		ret += tcrypt_test("xts(cast6)");
		break;

	case 16:
		ret += tcrypt_test("ecb(arc4)");
		break;

	case 17:
		ret += tcrypt_test("michael_mic");
		break;

	case 18:
		ret += tcrypt_test("crc32c");
		break;

	case 19:
		ret += tcrypt_test("ecb(tea)");
		break;

	case 20:
		ret += tcrypt_test("ecb(xtea)");
		break;

	case 21:
		ret += tcrypt_test("ecb(khazad)");
		break;

	case 22:
		ret += tcrypt_test("wp512");
		break;

	case 23:
		ret += tcrypt_test("wp384");
		break;

	case 24:
		ret += tcrypt_test("wp256");
		break;

	case 25:
		ret += tcrypt_test("ecb(tnepres)");
		break;

	case 26:
		ret += tcrypt_test("ecb(anubis)");
		ret += tcrypt_test("cbc(anubis)");
		break;

	case 27:
		ret += tcrypt_test("tgr192");
		break;

	case 28:
		ret += tcrypt_test("tgr160");
		break;

	case 29:
		ret += tcrypt_test("tgr128");
		break;

	case 30:
		ret += tcrypt_test("ecb(xeta)");
		break;

	case 31:
		ret += tcrypt_test("pcbc(fcrypt)");
		break;

	case 32:
		ret += tcrypt_test("ecb(camellia)");
		ret += tcrypt_test("cbc(camellia)");
		ret += tcrypt_test("ctr(camellia)");
		ret += tcrypt_test("lrw(camellia)");
		ret += tcrypt_test("xts(camellia)");
		break;

	case 33:
		ret += tcrypt_test("sha224");
		break;

	case 34:
		ret += tcrypt_test("salsa20");
		break;

	case 35:
		ret += tcrypt_test("gcm(aes)");
		ret += tcrypt_test("lrw(aes)");
		ret += tcrypt_test("xts(aes)");
		ret += tcrypt_test("rfc3686(ctr(aes))");
		break;

	case 36:
		ret += tcrypt_test("lzo");
		break;

	case 37:
		ret += tcrypt_test("ccm(aes)");
		break;

	case 38:
		ret += tcrypt_test("cts(cbc(aes))");
		break;

	case 39:
		ret += tcrypt_test("rmd128");
		break;

	case 40:
		ret += tcrypt_test("rmd160");
		break;

	case 41:
		ret += tcrypt_test("rmd256");
		break;

	case 42:
		ret += tcrypt_test("rmd320");
		break;

	case 43:
		ret += tcrypt_test("ecb(seed)");
		break;

	case 44:
		ret += tcrypt_test("zlib");
		break;

	case 45:
		ret += tcrypt_test("rfc4309(ccm(aes))");
		break;

	case 46:
		ret += tcrypt_test("ghash");
		break;

	case 47:
		ret += tcrypt_test("crct10dif");
		break;

	case 48:
		ret += tcrypt_test("ecdh");
		break;

	case 100:
		ret += tcrypt_test("hmac(md5)");
		break;

	case 101:
		ret += tcrypt_test("hmac(sha1)");
		break;

	case 102:
		ret += tcrypt_test("hmac(sha256)");
		break;

	case 103:
		ret += tcrypt_test("hmac(sha384)");
		break;

	case 104:
		ret += tcrypt_test("hmac(sha512)");
		break;

	case 105:
		ret += tcrypt_test("hmac(sha224)");
		break;

	case 106:
		ret += tcrypt_test("xcbc(aes)");
		break;

	case 107:
		ret += tcrypt_test("hmac(rmd128)");
		break;

	case 108:
		ret += tcrypt_test("hmac(rmd160)");
		break;

	case 109:
		ret += tcrypt_test("vmac(aes)");
		break;

	case 110:
		ret += tcrypt_test("hmac(crc32)");
		break;

	case 150:
		ret += tcrypt_test("ansi_cprng");
		break;

	case 151:
		ret += tcrypt_test("rfc4106(gcm(aes))");
		break;

	case 152:
		ret += tcrypt_test("rfc4543(gcm(aes))");
		break;

	case 153:
		ret += tcrypt_test("cmac(aes)");
		break;

	case 154:
		ret += tcrypt_test("cmac(des3_ede)");
		break;

	case 155:
		ret += tcrypt_test("authenc(hmac(sha1),cbc(aes))");
		break;

	case 156:
		ret += tcrypt_test("authenc(hmac(md5),ecb(cipher_null))");
		break;

	case 157:
		ret += tcrypt_test("authenc(hmac(sha1),ecb(cipher_null))");
		break;
	case 181:
		ret += tcrypt_test("authenc(hmac(sha1),cbc(des))");
		break;
	case 182:
		ret += tcrypt_test("authenc(hmac(sha1),cbc(des3_ede))");
		break;
	case 183:
		ret += tcrypt_test("authenc(hmac(sha224),cbc(des))");
		break;
	case 184:
		ret += tcrypt_test("authenc(hmac(sha224),cbc(des3_ede))");
		break;
	case 185:
		ret += tcrypt_test("authenc(hmac(sha256),cbc(des))");
		break;
	case 186:
		ret += tcrypt_test("authenc(hmac(sha256),cbc(des3_ede))");
		break;
	case 187:
		ret += tcrypt_test("authenc(hmac(sha384),cbc(des))");
		break;
	case 188:
		ret += tcrypt_test("authenc(hmac(sha384),cbc(des3_ede))");
		break;
	case 189:
		ret += tcrypt_test("authenc(hmac(sha512),cbc(des))");
		break;
	case 190:
		ret += tcrypt_test("authenc(hmac(sha512),cbc(des3_ede))");
		break;
	case 200:
		test_cipher_speed("ecb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ecb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("lrw(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("lrw(aes)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("xts(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_cipher_speed("xts(aes)", DECRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_cipher_speed("ctr(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		break;

	case 201:
		test_cipher_speed("ecb(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("ecb(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("cbc(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("cbc(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("ctr(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("ctr(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		break;

	case 202:
		test_cipher_speed("ecb(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ecb(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("lrw(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("lrw(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("xts(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_cipher_speed("xts(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		break;

	case 203:
		test_cipher_speed("ecb(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("ecb(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("cbc(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("cbc(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("ctr(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("ctr(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		break;

	case 204:
		test_cipher_speed("ecb(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_cipher_speed("ecb(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		test_cipher_speed("cbc(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_cipher_speed("cbc(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		break;

	case 205:
		test_cipher_speed("ecb(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ecb(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("lrw(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("lrw(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("xts(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_cipher_speed("xts(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		break;

	case 206:
		test_cipher_speed("salsa20", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		break;

	case 207:
		test_cipher_speed("ecb(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ecb(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("cbc(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("cbc(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ctr(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ctr(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("lrw(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_cipher_speed("lrw(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_cipher_speed("xts(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_cipher_speed("xts(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 208:
		test_cipher_speed("ecb(arc4)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		break;

	case 209:
		test_cipher_speed("ecb(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("ecb(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("cbc(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("cbc(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("ctr(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("ctr(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		break;

	case 210:
		test_cipher_speed("ecb(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ecb(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("cbc(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("cbc(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ctr(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ctr(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("lrw(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_cipher_speed("lrw(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_cipher_speed("xts(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_cipher_speed("xts(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 211:
		test_aead_speed("rfc4106(gcm(aes))", ENCRYPT, sec,
				NULL, 0, 16, 16, aead_speed_template_20);
		test_aead_speed("gcm(aes)", ENCRYPT, sec,
				NULL, 0, 16, 8, aead_speed_template_20);
		break;

	case 212:
		test_aead_speed("rfc4309(ccm(aes))", ENCRYPT, sec,
				NULL, 0, 16, 16, aead_speed_template_19);
		break;

	case 213:
		test_aead_speed("rfc7539esp(chacha20,poly1305)", ENCRYPT, sec,
				NULL, 0, 16, 8, aead_speed_template_36);
		break;

	case 214:
		test_cipher_speed("chacha20", ENCRYPT, sec, NULL, 0,
				  speed_template_32);
		break;


	case 300:
		if (alg) {
			test_hash_speed(alg, sec, generic_hash_speed_template);
			break;
		}

		/* fall through */

	case 301:
		test_hash_speed("md4", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 302:
		test_hash_speed("md5", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 303:
		test_hash_speed("sha1", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 304:
		test_hash_speed("sha256", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 305:
		test_hash_speed("sha384", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 306:
		test_hash_speed("sha512", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 307:
		test_hash_speed("wp256", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 308:
		test_hash_speed("wp384", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 309:
		test_hash_speed("wp512", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 310:
		test_hash_speed("tgr128", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 311:
		test_hash_speed("tgr160", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 312:
		test_hash_speed("tgr192", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 313:
		test_hash_speed("sha224", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 314:
		test_hash_speed("rmd128", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 315:
		test_hash_speed("rmd160", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 316:
		test_hash_speed("rmd256", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 317:
		test_hash_speed("rmd320", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 318:
		test_hash_speed("ghash-generic", sec, hash_speed_template_16);
		if (mode > 300 && mode < 400) break;

	case 319:
		test_hash_speed("crc32c", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 320:
		test_hash_speed("crct10dif", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;

	case 321:
		test_hash_speed("poly1305", sec, poly1305_speed_template);
		if (mode > 300 && mode < 400) break;

	case 399:
		break;

	case 400:
		if (alg) {
			test_ahash_speed(alg, sec, dsize,
				generic_hash_speed_template);
			break;
		}

		/* fall through */

	case 401:
		test_ahash_speed("md4", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 402:
		test_ahash_speed("md5", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 403:
		test_ahash_speed("sha1", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 404:
		test_ahash_speed("sha256", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 405:
		test_ahash_speed("sha384", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 406:
		test_ahash_speed("sha512", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 407:
		test_ahash_speed("wp256", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 408:
		test_ahash_speed("wp384", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 409:
		test_ahash_speed("wp512", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 410:
		test_ahash_speed("tgr128", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 411:
		test_ahash_speed("tgr160", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 412:
		test_ahash_speed("tgr192", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 413:
		test_ahash_speed("sha224", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 414:
		test_ahash_speed("rmd128", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 415:
		test_ahash_speed("rmd160", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 416:
		test_ahash_speed("rmd256", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 417:
		test_ahash_speed("rmd320", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;

	case 499:
		break;

	case 500:
		test_acipher_speed("ecb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ecb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cbc(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cbc(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("lrw(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_acipher_speed("lrw(aes)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_acipher_speed("xts(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_acipher_speed("xts(aes)", DECRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_acipher_speed("ctr(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ctr(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cfb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cfb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ofb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ofb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("rfc3686(ctr(aes))", ENCRYPT, sec, NULL, 0,
				speed_template_20_28_36);
		test_acipher_speed("rfc3686(ctr(aes))", DECRYPT, sec, NULL, 0,
				speed_template_20_28_36);
		break;

	case 501:
		test_acipher_speed("ecb(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("ecb(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("cbc(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("cbc(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("cfb(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("cfb(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("ofb(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("ofb(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		break;

	case 502:
		test_acipher_speed("ecb(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("ecb(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("cbc(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("cbc(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("cfb(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("cfb(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("ofb(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("ofb(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		break;

	case 503:
		test_acipher_speed("ecb(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ecb(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("lrw(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("lrw(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("xts(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_acipher_speed("xts(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 504:
		test_acipher_speed("ecb(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ecb(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cbc(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cbc(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ctr(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ctr(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("lrw(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_acipher_speed("lrw(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_acipher_speed("xts(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_acipher_speed("xts(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		break;

	case 505:
		test_acipher_speed("ecb(arc4)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		break;

	case 506:
		test_acipher_speed("ecb(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("ecb(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("cbc(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("cbc(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("ctr(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("ctr(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		break;

	case 507:
		test_acipher_speed("ecb(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ecb(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("lrw(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("lrw(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("xts(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_acipher_speed("xts(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 508:
		test_acipher_speed("ecb(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ecb(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("lrw(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("lrw(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("xts(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_acipher_speed("xts(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 509:
		test_acipher_speed("ecb(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("ecb(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("cbc(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("cbc(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("ctr(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("ctr(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		break;

	case 555:
		if (customized_test_acipher_speed("cbc(aes)", bsize, bcnt))
			return -EIO;
		break;

	case 560:
		ret += tcrypt_test("ecdsa");
		break;

	case 561:
#ifndef CONFIG_CRYPTO_FIPS
		test_akcipher_speed("ecdsa", SIGN, sec,
				    ecdsa_speed_template, ECDSA_SPEED_VECTORS,
				    akc_speed_template_P192);
		test_akcipher_speed("ecdsa", VERIFY, sec,
				    ecdsa_speed_template, ECDSA_SPEED_VECTORS,
				    akc_speed_template_P192);
#endif
		test_akcipher_speed("ecdsa", SIGN, sec,
				    ecdsa_speed_template, ECDSA_SPEED_VECTORS,
				    akc_speed_template_P256);
		test_akcipher_speed("ecdsa", VERIFY, sec,
				    ecdsa_speed_template, ECDSA_SPEED_VECTORS,
				    akc_speed_template_P256);
		break;

	case 1000:
		test_available();
		break;
	}

	return ret;
}

static int __init tcrypt_mod_init(void)
{
	int err = -ENOMEM;
	int i;

	if (dsize) {
		tvmem[0] = kmalloc(dsize, GFP_KERNEL);
		if (!tvmem[0])
			goto err_free_tv;
	} else {
		for (i = 0; i < TVMEMSIZE; i++) {
			tvmem[i] = (void *)__get_free_page(GFP_KERNEL);
			if (!tvmem[i])
				goto err_free_tv;
		}
	}

	err = do_test(alg, type, mask, mode);

	if (err) {
		pr_info("tcrypt: one or more tests failed!\n");
		goto err_free_tv;
	}

err_free_tv:
	if (dsize &&  tvmem[0]) {
		kfree(tvmem[0]);
	} else {
		for (i = 0; i < TVMEMSIZE && tvmem[i]; i++)
			free_page((unsigned long)tvmem[i]);
	}
	return err;
}

/*
 * If an init function is provided, an exit function must also be provided
 * to allow module unload.
 */
static void __exit tcrypt_mod_fini(void) { }

module_init(tcrypt_mod_init);
module_exit(tcrypt_mod_fini);

module_param(alg, charp, 0);
module_param(type, uint, 0);
module_param(mask, uint, 0);
module_param(mode, int, 0);
module_param(sec, uint, 0);
module_param(dsize, ulong, 0);
module_param(bsize, uint, 0);
module_param(bcnt, uint, 0);
/* When this parameter (sec) is not supplied,
 * it calculates in CPU cycles instead
 */
MODULE_PARM_DESC(sec, "Length in seconds of speed tests");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Quick & dirty crypto testing module");
MODULE_AUTHOR("James Morris <jmorris@intercode.com.au>");
