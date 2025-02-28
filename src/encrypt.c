/*
 * encrypt.c - Manage the global encryptor
 *
 * Copyright (C) 2013 - 2015, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 *
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(USE_CRYPTO_OPENSSL)

#include <openssl/md5.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>

#elif defined(USE_CRYPTO_POLARSSL)

#include <polarssl/md5.h>
#include <polarssl/sha1.h>
#include <polarssl/entropy.h>
#include <polarssl/ctr_drbg.h>
#include <polarssl/version.h>
#define CIPHER_UNSUPPORTED "unsupported"

#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <stdio.h>
#endif

#elif defined(USE_CRYPTO_MBEDTLS)

#include <mbedtls/md5.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/version.h>
#define CIPHER_UNSUPPORTED "unsupported"

#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <stdio.h>
#endif

#endif

#include <sodium.h>

#ifndef __MINGW32__
#include <arpa/inet.h>
#endif

#include "cache.h"
#include "encrypt.h"
#include "utils.h"

#define OFFSET_ROL(p, o) ((uint64_t)(*(p + o)) << (8 * o))

static uint8_t *enc_table;
static uint8_t *dec_table;
static uint8_t enc_key[MAX_KEY_LENGTH];
static int enc_key_len;
static int enc_iv_len;
static int enc_method;

static struct cache *iv_cache;

#ifdef DEBUG
static void dump(char *tag, char *text, int len)
{
    int i;
    printf("%s: ", tag);
    for (i = 0; i < len; i++)
        printf("0x%02x ", (uint8_t)text[i]);
    printf("\n");
}

#endif

static const char *supported_ciphers[CIPHER_NUM] = {
    "table",
    "rc4",
    "rc4-md5",
    "aes-128-cfb",
    "aes-192-cfb",
    "aes-256-cfb",
    "bf-cfb",
    "camellia-128-cfb",
    "camellia-192-cfb",
    "camellia-256-cfb",
    "cast5-cfb",
    "des-cfb",
    "idea-cfb",
    "rc2-cfb",
    "seed-cfb",
    "salsa20",
    "chacha20",
    "chacha20-ietf"
};

#ifdef USE_CRYPTO_POLARSSL
static const char *supported_ciphers_polarssl[CIPHER_NUM] = {
    "table",
    "ARC4-128",
    "ARC4-128",
    "AES-128-CFB128",
    "AES-192-CFB128",
    "AES-256-CFB128",
    "BLOWFISH-CFB64",
    "CAMELLIA-128-CFB128",
    "CAMELLIA-192-CFB128",
    "CAMELLIA-256-CFB128",
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    "salsa20",
    "chacha20",
    "chacha20-ietf"
};
#endif

#ifdef USE_CRYPTO_MBEDTLS
static const char *supported_ciphers_mbedtls[CIPHER_NUM] = {
    "table",
    "ARC4-128",
    "ARC4-128",
    "AES-128-CFB128",
    "AES-192-CFB128",
    "AES-256-CFB128",
    "BLOWFISH-CFB64",
    "CAMELLIA-128-CFB128",
    "CAMELLIA-192-CFB128",
    "CAMELLIA-256-CFB128",
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    "salsa20",
    "chacha20",
    "chacha20-ietf"
};
#endif

#ifdef USE_CRYPTO_APPLECC
static const CCAlgorithm supported_ciphers_applecc[CIPHER_NUM] = {
    kCCAlgorithmInvalid,
    kCCAlgorithmRC4,
    kCCAlgorithmRC4,
    kCCAlgorithmAES,
    kCCAlgorithmAES,
    kCCAlgorithmAES,
    kCCAlgorithmBlowfish,
    kCCAlgorithmInvalid,
    kCCAlgorithmInvalid,
    kCCAlgorithmInvalid,
    kCCAlgorithmCAST,
    kCCAlgorithmDES,
    kCCAlgorithmInvalid,
    kCCAlgorithmRC2,
    kCCAlgorithmInvalid,
    kCCAlgorithmInvalid,
    kCCAlgorithmInvalid,
    kCCAlgorithmInvalid
};

#endif

static const int supported_ciphers_iv_size[CIPHER_NUM] = {
    0, 0, 16, 16, 16, 16, 8, 16, 16, 16, 8, 8, 8, 8, 16, 8, 8, 12
};

static const int supported_ciphers_key_size[CIPHER_NUM] = {
    0, 16, 16, 16, 24, 32, 16, 16, 24, 32, 16, 8, 16, 16, 16, 32, 32, 32
};

static int safe_memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *_s1 = (const unsigned char *)s1;
    const unsigned char *_s2 = (const unsigned char *)s2;
    int ret                  = 0;
    size_t i;
    for (i = 0; i < n; i++)
        ret |= _s1[i] ^ _s2[i];
    return !!ret;
}

int balloc(buffer_t *ptr, size_t capacity)
{
    memset(ptr, 0, sizeof(buffer_t));
    ptr->array    = ss_malloc(capacity);
    ptr->capacity = capacity;
    return capacity;
}

int brealloc(buffer_t *ptr, size_t len, size_t capacity)
{
    if (ptr == NULL)
        return -1;
    size_t real_capacity = max(len, capacity);
    if (ptr->capacity < real_capacity) {
        ptr->array    = ss_realloc(ptr->array, real_capacity);
        ptr->capacity = real_capacity;
    }
    return real_capacity;
}

void bfree(buffer_t *ptr)
{
    if (ptr == NULL)
        return;
    ptr->idx      = 0;
    ptr->len      = 0;
    ptr->capacity = 0;
    if (ptr->array != NULL) {
        ss_free(ptr->array);
    }
}

static int crypto_stream_xor_ic(uint8_t *c, const uint8_t *m, uint64_t mlen,
                                const uint8_t *n, uint64_t ic, const uint8_t *k,
                                int method)
{
    switch (method) {
    case SALSA20:
        return crypto_stream_salsa20_xor_ic(c, m, mlen, n, ic, k);
    case CHACHA20:
        return crypto_stream_chacha20_xor_ic(c, m, mlen, n, ic, k);
    case CHACHA20IETF:
        return crypto_stream_chacha20_ietf_xor_ic(c, m, mlen, n, (uint32_t)ic, k);
    }
    // always return 0
    return 0;
}

static int random_compare(const void *_x, const void *_y, uint32_t i,
                          uint64_t a)
{
    uint8_t x = *((uint8_t *)_x);
    uint8_t y = *((uint8_t *)_y);
    return a % (x + i) - a % (y + i);
}

static void merge(uint8_t *left, int llength, uint8_t *right,
                  int rlength, uint32_t salt, uint64_t key)
{
    uint8_t *ltmp = (uint8_t *)malloc(llength * sizeof(uint8_t));
    uint8_t *rtmp = (uint8_t *)malloc(rlength * sizeof(uint8_t));

    uint8_t *ll = ltmp;
    uint8_t *rr = rtmp;

    uint8_t *result = left;

    memcpy(ltmp, left, llength * sizeof(uint8_t));
    memcpy(rtmp, right, rlength * sizeof(uint8_t));

    while (llength > 0 && rlength > 0) {
        if (random_compare(ll, rr, salt, key) <= 0) {
            *result = *ll;
            ++ll;
            --llength;
        } else {
            *result = *rr;
            ++rr;
            --rlength;
        }
        ++result;
    }

    if (llength > 0) {
        while (llength > 0) {
            *result = *ll;
            ++result;
            ++ll;
            --llength;
        }
    } else {
        while (rlength > 0) {
            *result = *rr;
            ++result;
            ++rr;
            --rlength;
        }
    }

    ss_free(ltmp);
    ss_free(rtmp);
}

static void merge_sort(uint8_t array[], int length,
                       uint32_t salt, uint64_t key)
{
    uint8_t middle;
    uint8_t *left, *right;
    int llength;

    if (length <= 1) {
        return;
    }

    middle = length / 2;

    llength = length - middle;

    left  = array;
    right = array + llength;

    merge_sort(left, llength, salt, key);
    merge_sort(right, middle, salt, key);
    merge(left, llength, right, middle, salt, key);
}

int enc_get_iv_len()
{
    return enc_iv_len;
}

unsigned char *enc_md5(const unsigned char *d, size_t n, unsigned char *md)
{
#if defined(USE_CRYPTO_OPENSSL)
    return MD5(d, n, md);
#elif defined(USE_CRYPTO_POLARSSL)
    static unsigned char m[16];
    if (md == NULL) {
        md = m;
    }
    md5(d, n, md);
    return md;
#elif defined(USE_CRYPTO_MBEDTLS)
    static unsigned char m[16];
    if (md == NULL) {
        md = m;
    }
    mbedtls_md5(d, n, md);
    return md;
#endif
}

void enc_table_init(const char *pass)
{
    uint32_t i;
    uint64_t key = 0;
    uint8_t *digest;

    enc_table = ss_malloc(256);
    dec_table = ss_malloc(256);

    digest = enc_md5((const uint8_t *)pass, strlen(pass), NULL);

    for (i = 0; i < 8; i++)
        key += OFFSET_ROL(digest, i);

    for (i = 0; i < 256; ++i)
        enc_table[i] = i;
    for (i = 1; i < 1024; ++i)
        merge_sort(enc_table, 256, i, key);
    for (i = 0; i < 256; ++i)
        // gen decrypt table from encrypt table
        dec_table[enc_table[i]] = i;
}

int cipher_iv_size(const cipher_kt_t *cipher)
{
#if defined(USE_CRYPTO_OPENSSL)
    return EVP_CIPHER_iv_length(cipher);
#elif defined(USE_CRYPTO_POLARSSL) || defined(USE_CRYPTO_MBEDTLS)
    if (cipher == NULL) {
        return 0;
    }
    return cipher->iv_size;
#endif
}

int cipher_key_size(const cipher_kt_t *cipher)
{
#if defined(USE_CRYPTO_OPENSSL)
    return EVP_CIPHER_key_length(cipher);
#elif defined(USE_CRYPTO_POLARSSL)
    if (cipher == NULL) {
        return 0;
    }
    /* Override PolarSSL 32 bit default key size with sane 128 bit default */
    if (cipher->base != NULL && POLARSSL_CIPHER_ID_BLOWFISH ==
        cipher->base->cipher) {
        return 128 / 8;
    }
    return cipher->key_length / 8;
#elif defined(USE_CRYPTO_MBEDTLS)
    /*
     * Semi-API changes (technically public, morally private)
     * Renamed a few headers to include _internal in the name. Those headers are
     * not supposed to be included by users.
     * Changed md_info_t into an opaque structure (use md_get_xxx() accessors).
     * Changed pk_info_t into an opaque structure.
     * Changed cipher_base_t into an opaque structure.
     */
    if (cipher == NULL) {
        return 0;
    }
    /* From Version 1.2.7 released 2013-04-13 Default Blowfish keysize is now 128-bits */
    return cipher->key_bitlen / 8;
#endif
}

int bytes_to_key(const cipher_kt_t *cipher, const digest_type_t *md,
                 const uint8_t *pass, uint8_t *key, uint8_t *iv)
{
    size_t datal;
    datal = strlen((const char *)pass);
#if defined(USE_CRYPTO_OPENSSL)
    return EVP_BytesToKey(cipher, md, NULL, pass, datal, 1, key, iv);
#elif defined(USE_CRYPTO_POLARSSL)
    md_context_t c;
    unsigned char md_buf[MAX_MD_SIZE];
    int niv;
    int nkey;
    int addmd;
    unsigned int mds;
    unsigned int i;
    int rv;

    nkey = cipher_key_size(cipher);
    niv  = cipher_iv_size(cipher);
    rv   = nkey;
    if (pass == NULL) {
        return nkey;
    }

    memset(&c, 0, sizeof(md_context_t));
    if (md_init_ctx(&c, md)) {
        return 0;
    }
    addmd = 0;
    mds   = md_get_size(md);
    for (;;) {
        int error;
        do {
            error = 1;
            if (md_starts(&c)) {
                break;
            }
            if (addmd) {
                if (md_update(&c, &(md_buf[0]), mds)) {
                    break;
                }
            } else {
                addmd = 1;
            }
            if (md_update(&c, pass, datal)) {
                break;
            }
            if (md_finish(&c, &(md_buf[0]))) {
                break;
            }
            error = 0;
        } while (0);
        if (error) {
            md_free_ctx(&c);
            memset(md_buf, 0, MAX_MD_SIZE);
            return 0;
        }

        i = 0;
        if (nkey) {
            for (;;) {
                if (nkey == 0) {
                    break;
                }
                if (i == mds) {
                    break;
                }
                if (key != NULL) {
                    *(key++) = md_buf[i];
                }
                nkey--;
                i++;
            }
        }
        if (niv && (i != mds)) {
            for (;;) {
                if (niv == 0) {
                    break;
                }
                if (i == mds) {
                    break;
                }
                if (iv != NULL) {
                    *(iv++) = md_buf[i];
                }
                niv--;
                i++;
            }
        }
        if ((nkey == 0) && (niv == 0)) {
            break;
        }
    }
    md_free_ctx(&c);
    memset(md_buf, 0, MAX_MD_SIZE);
    return rv;
#elif defined(USE_CRYPTO_MBEDTLS)
/*
 *
 * Generic message digest context.
 *
 * typedef struct {
 *  Information about the associated message digest
 *  const mbedtls_md_info_t *md_info;
 *
 *  Digest-specific context
 *  void *md_ctx;
 *
 *   HMAC part of the context
 *  void *hmac_ctx;
 * } mbedtls_md_context_t; // mbedtls 2.0.0
 *
 * typedef struct {
 *  Information about the associated message digest
 *  const md_info_t *md_info;
 *
 *  Digest-specific context
 *  void *md_ctx;
 * } md_context_t; //polarssl 1.3
 *
 */
    // NOTE: different struct body, initialize new param hmac 0 to disable HMAC
    mbedtls_md_context_t c;
    unsigned char md_buf[MAX_MD_SIZE];
    int niv;
    int nkey;
    int addmd;
    unsigned int mds;
    unsigned int i;
    int rv;

    nkey = cipher_key_size(cipher);
    niv  = cipher_iv_size(cipher);
    rv   = nkey;
    if (pass == NULL) {
        return nkey;
    }

    memset(&c, 0, sizeof(mbedtls_md_context_t));
    // XXX: md_init_ctx superseded by mbedtls_md_setup() in 2.0.0
    // new param hmac      0 to save some memory if HMAC will not be used,
    //                     non-zero is HMAC is going to be used with this context.
    if (mbedtls_md_setup(&c, md, 1)) {
        return 0;
    }
    addmd = 0;
    mds   = mbedtls_md_get_size(md);
    for (;;) {
        int error;
        do {
            error = 1;
            if (mbedtls_md_starts(&c)) {
                break;
            }
            if (addmd) {
                if (mbedtls_md_update(&c, &(md_buf[0]), mds)) {
                    break;
                }
            } else {
                addmd = 1;
            }
            if (mbedtls_md_update(&c, pass, datal)) {
                break;
            }
            if (mbedtls_md_finish(&c, &(md_buf[0]))) {
                break;
            }
            error = 0;
        } while (0);
        if (error) {
            mbedtls_md_free(&c); // md_free_ctx deprecated, Use mbedtls_md_free() instead
            memset(md_buf, 0, MAX_MD_SIZE);
            return 0;
        }

        i = 0;
        if (nkey) {
            for (;;) {
                if (nkey == 0) {
                    break;
                }
                if (i == mds) {
                    break;
                }
                if (key != NULL) {
                    *(key++) = md_buf[i];
                }
                nkey--;
                i++;
            }
        }
        if (niv && (i != mds)) {
            for (;;) {
                if (niv == 0) {
                    break;
                }
                if (i == mds) {
                    break;
                }
                if (iv != NULL) {
                    *(iv++) = md_buf[i];
                }
                niv--;
                i++;
            }
        }
        if ((nkey == 0) && (niv == 0)) {
            break;
        }
    }
    mbedtls_md_free(&c); // NOTE: md_free_ctx deprecated, Use mbedtls_md_free() instead
    memset(md_buf, 0, MAX_MD_SIZE);
    return rv;
#endif
}

int rand_bytes(uint8_t *output, int len)
{
#if defined(USE_CRYPTO_OPENSSL)
    return RAND_bytes(output, len);
#elif defined(USE_CRYPTO_POLARSSL)
    static entropy_context ec = {};
    static ctr_drbg_context cd_ctx = {};
    static unsigned char rand_initialised = 0;
    const size_t blen                     = min(len, CTR_DRBG_MAX_REQUEST);

    if (!rand_initialised) {
#ifdef _WIN32
        HCRYPTPROV hProvider;
        union {
            unsigned __int64 seed;
            BYTE buffer[8];
        } rand_buffer;

        hProvider = 0;
        if (CryptAcquireContext(&hProvider, 0, 0, PROV_RSA_FULL, \
                                CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            CryptGenRandom(hProvider, 8, rand_buffer.buffer);
            CryptReleaseContext(hProvider, 0);
        } else {
            rand_buffer.seed = (unsigned __int64)clock();
        }
#else
        FILE *urand;
        union {
            uint64_t seed;
            uint8_t buffer[8];
        } rand_buffer;

        urand = fopen("/dev/urandom", "r");
        if (urand) {
            int read = fread(&rand_buffer.seed, sizeof(rand_buffer.seed), 1,
                             urand);
            fclose(urand);
            if (read <= 0) {
                rand_buffer.seed = (uint64_t)clock();
            }
        } else {
            rand_buffer.seed = (uint64_t)clock();
        }
#endif
        entropy_init(&ec);
        if (ctr_drbg_init(&cd_ctx, entropy_func, &ec,
                          (const unsigned char *)rand_buffer.buffer, 8) != 0) {
#if POLARSSL_VERSION_NUMBER >= 0x01030000
            entropy_free(&ec);
#endif
            FATAL("Failed to initialize random generator");
        }
        rand_initialised = 1;
    }
    while (len > 0) {
        if (ctr_drbg_random(&cd_ctx, output, blen) != 0) {
            return 0;
        }
        output += blen;
        len    -= blen;
    }
    return 1;
#elif defined(USE_CRYPTO_MBEDTLS)
    static mbedtls_entropy_context ec = {};
    // XXX: ctr_drbg_context changed, [if defined(MBEDTLS_THREADING_C)    mbedtls_threading_mutex_t mutex;]
    static mbedtls_ctr_drbg_context cd_ctx = {};
    static unsigned char rand_initialised = 0;
    const size_t blen                     = min(len, MBEDTLS_CTR_DRBG_MAX_REQUEST);

    if (!rand_initialised) {
#ifdef _WIN32
        HCRYPTPROV hProvider;
        union {
            unsigned __int64 seed;
            BYTE buffer[8];
        } rand_buffer;

        hProvider = 0;
        if (CryptAcquireContext(&hProvider, 0, 0, PROV_RSA_FULL, \
                                CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            CryptGenRandom(hProvider, 8, rand_buffer.buffer);
            CryptReleaseContext(hProvider, 0);
        } else {
            rand_buffer.seed = (unsigned __int64)clock();
        }
#else
        FILE *urand;
        union {
            uint64_t seed;
            uint8_t buffer[8];
        } rand_buffer;

        urand = fopen("/dev/urandom", "r");
        if (urand) {
            int read = fread(&rand_buffer.seed, sizeof(rand_buffer.seed), 1,
                             urand);
            fclose(urand);
            if (read <= 0) {
                rand_buffer.seed = (uint64_t)clock();
            }
        } else {
            rand_buffer.seed = (uint64_t)clock();
        }
#endif
        mbedtls_entropy_init(&ec);
        // XXX: ctr_drbg_init changed, seems we should initialize it before calling mbedtls_ctr_drbg_seed()
        mbedtls_ctr_drbg_init(&cd_ctx);
        if (mbedtls_ctr_drbg_seed(&cd_ctx, mbedtls_entropy_func, &ec,
                                  (const unsigned char *)rand_buffer.buffer, 8) != 0) {
            mbedtls_entropy_free(&ec);
            FATAL("mbed TLS: Failed to initialize random generator");
        }
        rand_initialised = 1;
    }
    while (len > 0) {
        if (mbedtls_ctr_drbg_random(&cd_ctx, output, blen) != 0) {
            return 0;
        }
        output += blen;
        len    -= blen;
    }
    return 1;
#endif
}

const cipher_kt_t *get_cipher_type(int method)
{
    if (method <= TABLE || method >= CIPHER_NUM) {
        LOGE("get_cipher_type(): Illegal method");
        return NULL;
    }

    if (method == RC4_MD5) {
        method = RC4;
    }

    if (method >= SALSA20) {
        return NULL;
    }

    const char *ciphername = supported_ciphers[method];
#if defined(USE_CRYPTO_OPENSSL)
    return EVP_get_cipherbyname(ciphername);
#elif defined(USE_CRYPTO_POLARSSL)
    const char *polarname = supported_ciphers_polarssl[method];
    if (strcmp(polarname, CIPHER_UNSUPPORTED) == 0) {
        LOGE("Cipher %s currently is not supported by PolarSSL library",
             ciphername);
        return NULL;
    }
    return cipher_info_from_string(polarname);
#elif defined(USE_CRYPTO_MBEDTLS)
    const char *mbedtlsname = supported_ciphers_mbedtls[method];
    if (strcmp(mbedtlsname, CIPHER_UNSUPPORTED) == 0) {
        LOGE("Cipher %s currently is not supported by mbed TLS library",
             ciphername);
        return NULL;
    }
    return mbedtls_cipher_info_from_string(mbedtlsname);
#endif
}

const digest_type_t *get_digest_type(const char *digest)
{
    if (digest == NULL) {
        LOGE("get_digest_type(): Digest name is null");
        return NULL;
    }

#if defined(USE_CRYPTO_OPENSSL)
    return EVP_get_digestbyname(digest);
#elif defined(USE_CRYPTO_POLARSSL)
    return md_info_from_string(digest);
#elif defined(USE_CRYPTO_MBEDTLS)
    return mbedtls_md_info_from_string(digest);
#endif
}

void cipher_context_init(cipher_ctx_t *ctx, int method, int enc)
{
    if (method <= TABLE || method >= CIPHER_NUM) {
        LOGE("cipher_context_init(): Illegal method");
        return;
    }

    if (method >= SALSA20) {
        enc_iv_len = supported_ciphers_iv_size[method];
        return;
    }

    const char *ciphername = supported_ciphers[method];
#if defined(USE_CRYPTO_APPLECC)
    cipher_cc_t *cc = &ctx->cc;
    cc->cryptor = NULL;
    cc->cipher  = supported_ciphers_applecc[method];
    if (cc->cipher == kCCAlgorithmInvalid) {
        cc->valid = kCCContextInvalid;
    } else {
        cc->valid = kCCContextValid;
        if (cc->cipher == kCCAlgorithmRC4) {
            cc->mode    = kCCModeRC4;
            cc->padding = ccNoPadding;
        } else {
            cc->mode    = kCCModeCFB;
            cc->padding = ccPKCS7Padding;
        }
        return;
    }
#endif

    cipher_evp_t *evp         = &ctx->evp;
    const cipher_kt_t *cipher = get_cipher_type(method);
#if defined(USE_CRYPTO_OPENSSL)
    if (cipher == NULL) {
        LOGE("Cipher %s not found in OpenSSL library", ciphername);
        FATAL("Cannot initialize cipher");
    }
    EVP_CIPHER_CTX_init(evp);
    if (!EVP_CipherInit_ex(evp, cipher, NULL, NULL, NULL, enc)) {
        LOGE("Cannot initialize cipher %s", ciphername);
        exit(EXIT_FAILURE);
    }
    if (!EVP_CIPHER_CTX_set_key_length(evp, enc_key_len)) {
        EVP_CIPHER_CTX_cleanup(evp);
        LOGE("Invalid key length: %d", enc_key_len);
        exit(EXIT_FAILURE);
    }
    if (method > RC4_MD5) {
        EVP_CIPHER_CTX_set_padding(evp, 1);
    }
#elif defined(USE_CRYPTO_POLARSSL)
    if (cipher == NULL) {
        LOGE("Cipher %s not found in PolarSSL library", ciphername);
        FATAL("Cannot initialize PolarSSL cipher");
    }
    if (cipher_init_ctx(evp, cipher) != 0) {
        FATAL("Cannot initialize PolarSSL cipher context");
    }
#elif defined(USE_CRYPTO_MBEDTLS)
    // XXX: mbedtls_cipher_setup future change
    // NOTE:  Currently also clears structure. In future versions you will be required to call
    //        mbedtls_cipher_init() on the structure first.
    //        void mbedtls_cipher_init( mbedtls_cipher_context_t *ctx );
    if (cipher == NULL) {
        LOGE("Cipher %s not found in mbed TLS library", ciphername);
        FATAL("Cannot initialize mbed TLS cipher");
    }
    mbedtls_cipher_init(evp);
    if (mbedtls_cipher_setup(evp, cipher) != 0) {
        FATAL("Cannot initialize mbed TLS cipher context");
    }
#endif
}

void cipher_context_set_iv(cipher_ctx_t *ctx, uint8_t *iv, size_t iv_len,
                           int enc)
{
    const unsigned char *true_key;

    if (iv == NULL) {
        LOGE("cipher_context_set_iv(): IV is null");
        return;
    }

    if (!enc) {
        memcpy(ctx->iv, iv, iv_len);
    }

    if (enc_method >= SALSA20) {
        return;
    }

    if (enc_method == RC4_MD5) {
        unsigned char key_iv[32];
        memcpy(key_iv, enc_key, 16);
        memcpy(key_iv + 16, iv, 16);
        true_key = enc_md5(key_iv, 32, NULL);
        iv_len   = 0;
    } else {
        true_key = enc_key;
    }

#ifdef USE_CRYPTO_APPLECC
    cipher_cc_t *cc = &ctx->cc;
    if (cc->valid == kCCContextValid) {
        memcpy(cc->iv, iv, iv_len);
        memcpy(cc->key, true_key, enc_key_len);
        cc->iv_len  = iv_len;
        cc->key_len = enc_key_len;
        cc->encrypt = enc ? kCCEncrypt : kCCDecrypt;
        if (cc->cryptor != NULL) {
            CCCryptorRelease(cc->cryptor);
            cc->cryptor = NULL;
        }

        CCCryptorStatus ret;
        ret = CCCryptorCreateWithMode(
            cc->encrypt,
            cc->mode,
            cc->cipher,
            cc->padding,
            cc->iv, cc->key, cc->key_len,
            NULL, 0, 0, 0,
            &cc->cryptor);
        if (ret != kCCSuccess) {
            if (cc->cryptor != NULL) {
                CCCryptorRelease(cc->cryptor);
                cc->cryptor = NULL;
            }
            FATAL("Cannot set CommonCrypto key and IV");
        }
        return;
    }
#endif

    cipher_evp_t *evp = &ctx->evp;
    if (evp == NULL) {
        LOGE("cipher_context_set_iv(): Cipher context is null");
        return;
    }
#if defined(USE_CRYPTO_OPENSSL)
    if (!EVP_CipherInit_ex(evp, NULL, NULL, true_key, iv, enc)) {
        EVP_CIPHER_CTX_cleanup(evp);
        FATAL("Cannot set key and IV");
    }
#elif defined(USE_CRYPTO_POLARSSL)
    // XXX: PolarSSL 1.3.11: cipher_free_ctx deprecated, Use cipher_free() instead.
    if (cipher_setkey(evp, true_key, enc_key_len * 8, enc) != 0) {
        cipher_free_ctx(evp);
        FATAL("Cannot set PolarSSL cipher key");
    }
#if POLARSSL_VERSION_NUMBER >= 0x01030000
    if (cipher_set_iv(evp, iv, iv_len) != 0) {
        cipher_free_ctx(evp);
        FATAL("Cannot set PolarSSL cipher IV");
    }
    if (cipher_reset(evp) != 0) {
        cipher_free_ctx(evp);
        FATAL("Cannot finalize PolarSSL cipher context");
    }
#else
    if (cipher_reset(evp, iv) != 0) {
        cipher_free_ctx(evp);
        FATAL("Cannot set PolarSSL cipher IV");
    }
#endif
#elif defined(USE_CRYPTO_MBEDTLS)
    if (mbedtls_cipher_setkey(evp, true_key, enc_key_len * 8, enc) != 0) {
        mbedtls_cipher_free(evp);
        FATAL("Cannot set mbed TLS cipher key");
    }

    if (mbedtls_cipher_set_iv(evp, iv, iv_len) != 0) {
        mbedtls_cipher_free(evp);
        FATAL("Cannot set mbed TLS cipher IV");
    }
    if (mbedtls_cipher_reset(evp) != 0) {
        mbedtls_cipher_free(evp);
        FATAL("Cannot finalize mbed TLS cipher context");
    }
#endif

#ifdef DEBUG
    dump("IV", (char *)iv, iv_len);
#endif
}

void cipher_context_release(cipher_ctx_t *ctx)
{
    if (enc_method >= SALSA20) {
        return;
    }

#ifdef USE_CRYPTO_APPLECC
    cipher_cc_t *cc = &ctx->cc;
    if (cc->cryptor != NULL) {
        CCCryptorRelease(cc->cryptor);
        cc->cryptor = NULL;
    }
    if (cc->valid == kCCContextValid) {
        return;
    }
#endif

    cipher_evp_t *evp = &ctx->evp;
#if defined(USE_CRYPTO_OPENSSL)
    EVP_CIPHER_CTX_cleanup(evp);
#elif defined(USE_CRYPTO_POLARSSL)
// NOTE: cipher_free_ctx deprecated in PolarSSL 1.3.11
    cipher_free_ctx(evp);
#elif defined(USE_CRYPTO_MBEDTLS)
// NOTE: cipher_free_ctx deprecated
    mbedtls_cipher_free(evp);
#endif
}

static int cipher_context_update(cipher_ctx_t *ctx, uint8_t *output, size_t *olen,
                                 const uint8_t *input, size_t ilen)
{
#ifdef USE_CRYPTO_APPLECC
    cipher_cc_t *cc = &ctx->cc;
    if (cc->valid == kCCContextValid) {
        CCCryptorStatus ret;
        ret = CCCryptorUpdate(cc->cryptor, input, ilen, output,
                              ilen, olen);
        return (ret == kCCSuccess) ? 1 : 0;
    }
#endif
    cipher_evp_t *evp = &ctx->evp;
#if defined(USE_CRYPTO_OPENSSL)
    int err = 0, tlen = *olen;
    err = EVP_CipherUpdate(evp, (uint8_t *)output, &tlen,
                           (const uint8_t *)input, ilen);
    *olen = tlen;
    return err;
#elif defined(USE_CRYPTO_POLARSSL)
    return !cipher_update(evp, (const uint8_t *)input, ilen,
                          (uint8_t *)output, olen);
#elif defined(USE_CRYPTO_MBEDTLS)
    return !mbedtls_cipher_update(evp, (const uint8_t *)input, ilen,
                                  (uint8_t *)output, olen);
#endif
}

int ss_onetimeauth(buffer_t *buf, uint8_t *iv, size_t capacity)
{
    uint8_t hash[ONETIMEAUTH_BYTES * 2];
    uint8_t auth_key[MAX_IV_LENGTH + MAX_KEY_LENGTH];
    memcpy(auth_key, iv, enc_iv_len);
    memcpy(auth_key + enc_iv_len, enc_key, enc_key_len);

    brealloc(buf, ONETIMEAUTH_BYTES + buf->len, capacity);

#if defined(USE_CRYPTO_OPENSSL)
    HMAC(EVP_sha1(), auth_key, enc_iv_len + enc_key_len, (uint8_t *)buf->array, buf->len, (uint8_t *)hash, NULL);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md_hmac(mbedtls_md_info_from_type(
                        MBEDTLS_MD_SHA1), auth_key, enc_iv_len + enc_key_len, (uint8_t *)buf->array, buf->len,
                    (uint8_t *)hash);
#else
    sha1_hmac(auth_key, enc_iv_len + enc_key_len, (uint8_t *)buf->array, buf->len, (uint8_t *)hash);
#endif

    memcpy(buf->array + buf->len, hash, ONETIMEAUTH_BYTES);
    buf->len += ONETIMEAUTH_BYTES;

    return 0;
}

int ss_onetimeauth_verify(buffer_t *buf, uint8_t *iv)
{
    uint8_t hash[ONETIMEAUTH_BYTES * 2];
    uint8_t auth_key[MAX_IV_LENGTH + MAX_KEY_LENGTH];
    memcpy(auth_key, iv, enc_iv_len);
    memcpy(auth_key + enc_iv_len, enc_key, enc_key_len);
    size_t len = buf->len - ONETIMEAUTH_BYTES;

#if defined(USE_CRYPTO_OPENSSL)
    HMAC(EVP_sha1(), auth_key, enc_iv_len + enc_key_len, (uint8_t *)buf->array, len, hash, NULL);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md_hmac(mbedtls_md_info_from_type(
                        MBEDTLS_MD_SHA1), auth_key, enc_iv_len + enc_key_len, (uint8_t *)buf->array, len, hash);
#else
    sha1_hmac(auth_key, enc_iv_len + enc_key_len, (uint8_t *)buf->array, len, hash);
#endif

    return safe_memcmp(buf->array + len, hash, ONETIMEAUTH_BYTES);
}

int ss_encrypt_all(buffer_t *plain, int method, int auth, size_t capacity)
{
    if (method > TABLE) {
        cipher_ctx_t evp;
        cipher_context_init(&evp, method, 1);

        size_t iv_len = enc_iv_len;
        int err       = 1;

        static buffer_t tmp = { 0, 0, 0, NULL };
        brealloc(&tmp, iv_len + plain->len, capacity);
        buffer_t *cipher = &tmp;
        cipher->len = plain->len;

        uint8_t iv[MAX_IV_LENGTH];

        rand_bytes(iv, iv_len);
        cipher_context_set_iv(&evp, iv, iv_len, 1);
        memcpy(cipher->array, iv, iv_len);

        if (auth) {
            ss_onetimeauth(plain, iv, capacity);
            cipher->len = plain->len;
        }

        if (method >= SALSA20) {
            crypto_stream_xor_ic((uint8_t *)(cipher->array + iv_len),
                                 (const uint8_t *)plain->array, (uint64_t)(plain->len),
                                 (const uint8_t *)iv,
                                 0, enc_key, method);
        } else {
            err = cipher_context_update(&evp, (uint8_t *)(cipher->array + iv_len),
                                        &cipher->len, (const uint8_t *)plain->array,
                                        plain->len);
        }

        if (!err) {
            bfree(plain);
            cipher_context_release(&evp);
            return -1;
        }

#ifdef DEBUG
        dump("PLAIN", plain->array, plain->len);
        dump("CIPHER", cipher->array + iv_len, cipher->len);
#endif

        cipher_context_release(&evp);

        brealloc(plain, iv_len + cipher->len, capacity);
        memcpy(plain->array, cipher->array, iv_len + cipher->len);
        plain->len = iv_len + cipher->len;

        return 0;
    } else {
        char *begin = plain->array;
        char *ptr   = plain->array;
        while (ptr < begin + plain->len) {
            *ptr = (char)enc_table[(uint8_t)*ptr];
            ptr++;
        }
        return 0;
    }
}

int ss_encrypt(buffer_t *plain, enc_ctx_t *ctx, size_t capacity)
{
    if (ctx != NULL) {
        static buffer_t tmp = { 0, 0, 0, NULL };

        int err       = 1;
        size_t iv_len = 0;
        if (!ctx->init) {
            iv_len = enc_iv_len;
        }

        brealloc(&tmp, iv_len + plain->len, capacity);
        buffer_t *cipher = &tmp;
        cipher->len = plain->len;

        if (!ctx->init) {
            cipher_context_set_iv(&ctx->evp, ctx->evp.iv, iv_len, 1);
            memcpy(cipher->array, ctx->evp.iv, iv_len);
            ctx->counter = 0;
            ctx->init    = 1;
        }

        if (enc_method >= SALSA20) {
            int padding = ctx->counter % SODIUM_BLOCK_SIZE;
            brealloc(cipher, iv_len + (padding + cipher->len) * 2, capacity);
            if (padding) {
                brealloc(plain, plain->len + padding, capacity);
                memmove(plain->array + padding, plain->array, plain->len);
                memset(plain->array, 0, padding);
            }
            crypto_stream_xor_ic((uint8_t *)(cipher->array + iv_len),
                                 (const uint8_t *)plain->array,
                                 (uint64_t)(plain->len + padding),
                                 (const uint8_t *)ctx->evp.iv,
                                 ctx->counter / SODIUM_BLOCK_SIZE, enc_key,
                                 enc_method);
            ctx->counter += plain->len;
            if (padding) {
                memmove(cipher->array + iv_len,
                        cipher->array + iv_len + padding, cipher->len);
            }
        } else {
            err =
                cipher_context_update(&ctx->evp,
                                      (uint8_t *)(cipher->array + iv_len),
                                      &cipher->len, (const uint8_t *)plain->array,
                                      plain->len);
            if (!err) {
                return -1;
            }
        }

#ifdef DEBUG
        dump("PLAIN", plain->array, plain->len);
        dump("CIPHER", cipher->array + iv_len, cipher->len);
#endif

        brealloc(plain, iv_len + cipher->len, capacity);
        memcpy(plain->array, cipher->array, iv_len + cipher->len);
        plain->len = iv_len + cipher->len;

        return 0;
    } else {
        char *begin = plain->array;
        char *ptr   = plain->array;
        while (ptr < begin + plain->len) {
            *ptr = (char)enc_table[(uint8_t)*ptr];
            ptr++;
        }
        return 0;
    }
}

int ss_decrypt_all(buffer_t *cipher, int method, int auth, size_t capacity)
{
    if (method > TABLE) {
        size_t iv_len = enc_iv_len;
        int ret       = 1;

        if (cipher->len <= iv_len) {
            return -1;
        }

        cipher_ctx_t evp;
        cipher_context_init(&evp, method, 0);

        static buffer_t tmp = { 0, 0, 0, NULL };
        brealloc(&tmp, cipher->len, capacity);
        buffer_t *plain = &tmp;
        plain->len = cipher->len - iv_len;

        uint8_t iv[MAX_IV_LENGTH];
        memcpy(iv, cipher->array, iv_len);
        cipher_context_set_iv(&evp, iv, iv_len, 0);

        if (method >= SALSA20) {
            crypto_stream_xor_ic((uint8_t *)plain->array,
                                 (const uint8_t *)(cipher->array + iv_len),
                                 (uint64_t)(cipher->len - iv_len),
                                 (const uint8_t *)iv, 0, enc_key, method);
        } else {
            ret = cipher_context_update(&evp, (uint8_t *)plain->array, &plain->len,
                                        (const uint8_t *)(cipher->array + iv_len),
                                        cipher->len - iv_len);
        }

        if (auth || (plain->array[0] & ONETIMEAUTH_FLAG)) {
            if (plain->len > ONETIMEAUTH_BYTES) {
                ret = !ss_onetimeauth_verify(plain, iv);
                if (ret) {
                    plain->len -= ONETIMEAUTH_BYTES;
                }
            } else {
                ret = 0;
            }
        }

        if (!ret) {
            bfree(cipher);
            cipher_context_release(&evp);
            return -1;
        }

#ifdef DEBUG
        dump("PLAIN", plain->array, plain->len);
        dump("CIPHER", cipher->array + iv_len, cipher->len - iv_len);
#endif

        cipher_context_release(&evp);

        brealloc(cipher, plain->len, capacity);
        memcpy(cipher->array, plain->array, plain->len);
        cipher->len = plain->len;

        return 0;
    } else {
        char *begin = cipher->array;
        char *ptr   = cipher->array;
        while (ptr < begin + cipher->len) {
            *ptr = (char)dec_table[(uint8_t)*ptr];
            ptr++;
        }
        return 0;
    }
}

int ss_decrypt(buffer_t *cipher, enc_ctx_t *ctx, size_t capacity)
{
    if (ctx != NULL) {
        static buffer_t tmp = { 0, 0, 0, NULL };

        size_t iv_len = 0;
        int err       = 1;

        brealloc(&tmp, cipher->len, capacity);
        buffer_t *plain = &tmp;
        plain->len = cipher->len;

        if (!ctx->init) {
            uint8_t iv[MAX_IV_LENGTH];
            iv_len      = enc_iv_len;
            plain->len -= iv_len;

            memcpy(iv, cipher->array, iv_len);
            cipher_context_set_iv(&ctx->evp, iv, iv_len, 0);
            ctx->counter = 0;
            ctx->init    = 1;

            if (enc_method >= RC4_MD5) {
                if (cache_key_exist(iv_cache, (char *)iv, iv_len)) {
                    bfree(cipher);
                    return -1;
                } else {
                    cache_insert(iv_cache, (char *)iv, iv_len, NULL);
                }
            }
        }

        if (enc_method >= SALSA20) {
            int padding = ctx->counter % SODIUM_BLOCK_SIZE;
            brealloc(plain, (plain->len + padding) * 2, capacity);

            if (padding) {
                brealloc(cipher, cipher->len + padding, capacity);
                memmove(cipher->array + iv_len + padding, cipher->array + iv_len,
                        cipher->len - iv_len);
                memset(cipher->array + iv_len, 0, padding);
            }
            crypto_stream_xor_ic((uint8_t *)plain->array,
                                 (const uint8_t *)(cipher->array + iv_len),
                                 (uint64_t)(cipher->len - iv_len + padding),
                                 (const uint8_t *)ctx->evp.iv,
                                 ctx->counter / SODIUM_BLOCK_SIZE, enc_key,
                                 enc_method);
            ctx->counter += cipher->len - iv_len;
            if (padding) {
                memmove(plain->array, plain->array + padding, plain->len);
            }
        } else {
            err = cipher_context_update(&ctx->evp, (uint8_t *)plain->array, &plain->len,
                                        (const uint8_t *)(cipher->array + iv_len),
                                        cipher->len - iv_len);
        }

        if (!err) {
            bfree(cipher);
            return -1;
        }

#ifdef DEBUG
        dump("PLAIN", plain->array, plain->len);
        dump("CIPHER", cipher->array + iv_len, cipher->len - iv_len);
#endif

        brealloc(cipher, plain->len, capacity);
        memcpy(cipher->array, plain->array, plain->len);
        cipher->len = plain->len;

        return 0;
    } else {
        char *begin = cipher->array;
        char *ptr   = cipher->array;
        while (ptr < begin + cipher->len) {
            *ptr = (char)dec_table[(uint8_t)*ptr];
            ptr++;
        }
        return 0;
    }
}

void enc_ctx_init(int method, enc_ctx_t *ctx, int enc)
{
    memset(ctx, 0, sizeof(enc_ctx_t));
    cipher_context_init(&ctx->evp, method, enc);

    if (enc) {
        rand_bytes(ctx->evp.iv, enc_iv_len);
    }
}

void enc_key_init(int method, const char *pass)
{
    if (method <= TABLE || method >= CIPHER_NUM) {
        LOGE("enc_key_init(): Illegal method");
        return;
    }

    // Inilitialize cache
    cache_create(&iv_cache, 256, NULL);

#if defined(USE_CRYPTO_OPENSSL)
    OpenSSL_add_all_algorithms();
#endif

    uint8_t iv[MAX_IV_LENGTH];

    cipher_kt_t *cipher;
    cipher_kt_t cipher_info;

    if (method == SALSA20 || method == CHACHA20 || method == CHACHA20IETF) {
        if (sodium_init() == -1) {
            FATAL("Failed to initialize sodium");
        }
        // Fake cipher
        cipher = (cipher_kt_t *)&cipher_info;
#if defined(USE_CRYPTO_OPENSSL)
        cipher->key_len = supported_ciphers_key_size[method];
        cipher->iv_len  = supported_ciphers_iv_size[method];
#endif
#if defined(USE_CRYPTO_POLARSSL)
        cipher->base       = NULL;
        cipher->key_length = supported_ciphers_key_size[method] * 8;
        cipher->iv_size    = supported_ciphers_iv_size[method];
#endif
#if defined(USE_CRYPTO_MBEDTLS)
        // XXX: key_length changed to key_bitlen in mbed TLS 2.0.0
        cipher->base       = NULL;
        cipher->key_bitlen = supported_ciphers_key_size[method] * 8;
        cipher->iv_size    = supported_ciphers_iv_size[method];
#endif
    } else {
        cipher = (cipher_kt_t *)get_cipher_type(method);
    }

    if (cipher == NULL) {
        do {
#if defined(USE_CRYPTO_POLARSSL) && defined(USE_CRYPTO_APPLECC)
            if (supported_ciphers_applecc[method] != kCCAlgorithmInvalid) {
                cipher_info.base       = NULL;
                cipher_info.key_length = supported_ciphers_key_size[method] * 8;
                cipher_info.iv_size    = supported_ciphers_iv_size[method];
                cipher                 = (cipher_kt_t *)&cipher_info;
                break;
            }
#endif
#if defined(USE_CRYPTO_MBEDTLS) && defined(USE_CRYPTO_APPLECC)
            // XXX: key_length changed to key_bitlen in mbed TLS 2.0.0
            if (supported_ciphers_applecc[method] != kCCAlgorithmInvalid) {
                cipher_info.base       = NULL;
                cipher_info.key_bitlen = supported_ciphers_key_size[method] * 8;
                cipher_info.iv_size    = supported_ciphers_iv_size[method];
                cipher                 = (cipher_kt_t *)&cipher_info;
                break;
            }
#endif
            LOGE("Cipher %s not found in crypto library",
                 supported_ciphers[method]);
            FATAL("Cannot initialize cipher");
        } while (0);
    }

    const digest_type_t *md = get_digest_type("MD5");
    if (md == NULL) {
        FATAL("MD5 Digest not found in crypto library");
    }

    enc_key_len = bytes_to_key(cipher, md, (const uint8_t *)pass, enc_key, iv);
    if (enc_key_len == 0) {
        FATAL("Cannot generate key and IV");
    }
    if (method == RC4_MD5) {
        enc_iv_len = 16;
    } else {
        enc_iv_len = cipher_iv_size(cipher);
    }
    enc_method = method;
}

int enc_init(const char *pass, const char *method)
{
    int m = TABLE;
    if (method != NULL) {
        for (m = TABLE; m < CIPHER_NUM; m++)
            if (strcmp(method, supported_ciphers[m]) == 0) {
                break;
            }
        if (m >= CIPHER_NUM) {
            LOGE("Invalid cipher name: %s, use table instead", method);
            m = TABLE;
        }
    }
    if (m == TABLE) {
        enc_table_init(pass);
    } else {
        enc_key_init(m, pass);
    }
    return m;
}

int ss_check_hash(buffer_t *buf, chunk_t *chunk, enc_ctx_t *ctx, size_t capacity)
{
    int i, j, k;
    ssize_t blen  = buf->len;
    uint32_t cidx = chunk->idx;

    brealloc(chunk->buf, chunk->len + blen, capacity);
    brealloc(buf, chunk->len + blen, capacity);

    for (i = 0, j = 0, k = 0; i < blen; i++) {
        chunk->buf->array[cidx++] = buf->array[k++];

        if (cidx == CLEN_BYTES) {
            uint16_t clen = ntohs(*((uint16_t *)chunk->buf->array));
            brealloc(chunk->buf, clen + AUTH_BYTES, capacity);
            chunk->len = clen;
        }

        if (cidx == chunk->len + AUTH_BYTES) {
            // Compare hash
            uint8_t hash[ONETIMEAUTH_BYTES * 2];
            uint8_t key[MAX_IV_LENGTH + sizeof(uint32_t)];

            uint32_t c = htonl(chunk->counter);
            memcpy(key, ctx->evp.iv, enc_iv_len);
            memcpy(key + enc_iv_len, &c, sizeof(uint32_t));
#if defined(USE_CRYPTO_OPENSSL)
            HMAC(EVP_sha1(), key, enc_iv_len + sizeof(uint32_t),
                 (uint8_t *)chunk->buf->array + AUTH_BYTES, chunk->len, hash, NULL);
#elif defined(USE_CRYPTO_MBEDTLS)
            mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, enc_iv_len + sizeof(uint32_t),
                            (uint8_t *)chunk->buf->array + AUTH_BYTES, chunk->len, hash);
#else
            sha1_hmac(key, enc_iv_len + sizeof(uint32_t),
                      (uint8_t *)chunk->buf->array + AUTH_BYTES, chunk->len, hash);
#endif

            if (safe_memcmp(hash, chunk->buf->array + CLEN_BYTES, ONETIMEAUTH_BYTES) != 0) {
                return 0;
            }

            // Copy chunk back to buffer
            memmove(buf->array + j + chunk->len, buf->array + k, blen - i - 1);
            memcpy(buf->array + j, chunk->buf->array + AUTH_BYTES, chunk->len);

            // Reset the base offset
            j   += chunk->len;
            k    = j;
            cidx = 0;
            chunk->counter++;
        }
    }

    buf->len   = j;
    chunk->idx = cidx;
    return 1;
}

int ss_gen_hash(buffer_t *buf, uint32_t *counter, enc_ctx_t *ctx, size_t capacity)
{
    ssize_t blen       = buf->len;
    uint16_t chunk_len = htons((uint16_t)blen);
    uint8_t hash[ONETIMEAUTH_BYTES * 2];
    uint8_t key[MAX_IV_LENGTH + sizeof(uint32_t)];
    uint32_t c = htonl(*counter);

    brealloc(buf, AUTH_BYTES + blen, capacity);
    memcpy(key, ctx->evp.iv, enc_iv_len);
    memcpy(key + enc_iv_len, &c, sizeof(uint32_t));
#if defined(USE_CRYPTO_OPENSSL)
    HMAC(EVP_sha1(), key, enc_iv_len + sizeof(uint32_t), (uint8_t *)buf->array, blen, hash, NULL);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md_hmac(mbedtls_md_info_from_type(
                        MBEDTLS_MD_SHA1), key, enc_iv_len + sizeof(uint32_t), (uint8_t *)buf->array, blen, hash);
#else
    sha1_hmac(key, enc_iv_len + sizeof(uint32_t), (uint8_t *)buf->array, blen, hash);
#endif

    memmove(buf->array + AUTH_BYTES, buf->array, blen);
    memcpy(buf->array + CLEN_BYTES, hash, ONETIMEAUTH_BYTES);
    memcpy(buf->array, &chunk_len, CLEN_BYTES);

    *counter = *counter + 1;
    buf->len = blen + AUTH_BYTES;

    return 0;
}
