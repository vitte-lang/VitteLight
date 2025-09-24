// SPDX-License-Identifier: GPL-3.0-or-later
//
// crypto.c — Minimal crypto utils for Vitte Light (C17, portable)
// Namespace: "crypto"
//
// Build:
//   With OpenSSL 1.1.1+ (recommended):
//     cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_OPENSSL -c crypto.c -lcrypto
//   Without OpenSSL: functions return ENOSYS except random and equals.
//
// Provides:
//   // RNG
//   int  crypto_random(void* buf, size_t n);           // 0/-1 (errno set)
//   // Constant-time compare
//   int  crypto_equals(const void* a, const void* b, size_t n); // 1/0
//
//   // AEAD (AES-256-GCM and CHACHA20-POLY1305)
//   // alg ∈ "AES-256-GCM" | "CHACHA20-POLY1305"
//   size_t crypto_aead_keybytes(const char* alg);      // 32 or 32
//   size_t crypto_aead_noncebytes(const char* alg);    // 12
//   size_t crypto_aead_tagbytes(const char* alg);      // 16
//
//   // Encrypt: out = ciphertext||tag (tag appended). out_len = pt_len + tagbytes.
//   int  crypto_aead_encrypt(const char* alg,
//                            const uint8_t* key, size_t keylen,
//                            const uint8_t* nonce, size_t noncelen,
//                            const uint8_t* ad, size_t adlen,
//                            const uint8_t* pt, size_t ptlen,
//                            uint8_t* out, size_t* out_len);
//
//   // Decrypt from ciphertext||tag
//   int  crypto_aead_decrypt(const char* alg,
//                            const uint8_t* key, size_t keylen,
//                            const uint8_t* nonce, size_t noncelen,
//                            const uint8_t* ad, size_t adlen,
//                            const uint8_t* ct_tag, size_t ct_tag_len,
//                            uint8_t* out_pt, size_t* out_ptlen);

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

// SPDX-License-Identifier: GPL-3.0-or-later
//
// crypto.c — Minimal crypto utils for Vitte Light (C17, portable)

#include <stdio.h>    // <- nécessaire pour FILE, fopen, fclose, fread, etc.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <bcrypt.h> /* pour BCryptGenRandom */
  #pragma comment(lib, "bcrypt.lib")
#endif

// ---------- util ----------

int crypto_equals(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    uint32_t d = 0;
    for (size_t i=0;i<n;i++) d |= (uint32_t)(x[i] ^ y[i]);
    return d == 0;
}

// ---------- RNG (OS) ----------

int crypto_random(void* buf, size_t n) {
    if (!buf && n) { errno = EINVAL; return -1; }
#if defined(VL_HAVE_OPENSSL)
    #include <openssl/rand.h>
    if (RAND_bytes((unsigned char*)buf, (int)n) != 1) { errno = EIO; return -1; }
    return 0;
#elif defined(_WIN32)
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) { errno = EIO; return -1; }
    return 0;
#else
    // /dev/urandom
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) { errno = EIO; return -1; }
    size_t rd = fread(buf, 1, n, f);
    fclose(f);
    if (rd != n) { errno = EIO; return -1; }
    return 0;
#endif
}

// ---------- AEAD sizes ----------

static int alg_is_aes(const char* alg){
    return alg && (strcmp(alg,"AES-256-GCM")==0 || strcmp(alg,"AES_GCM_256")==0);
}
static int alg_is_chacha(const char* alg){
    return alg && (strcmp(alg,"CHACHA20-POLY1305")==0 || strcmp(alg,"CHACHA20_POLY1305")==0);
}

#if defined(VL_HAVE_OPENSSL)

size_t crypto_aead_keybytes(const char* alg){
    if (alg_is_aes(alg)) return 32;
    if (alg_is_chacha(alg)) return 32;
    return 0;
}
size_t crypto_aead_noncebytes(const char* alg){
    if (alg_is_aes(alg) || alg_is_chacha(alg)) return 12;
    return 0;
}
size_t crypto_aead_tagbytes(const char* alg){
    if (alg_is_aes(alg) || alg_is_chacha(alg)) return 16;
    return 0;
}

// ---------- AEAD with OpenSSL ----------

#include <openssl/evp.h>

static const EVP_CIPHER* cipher_from_alg(const char* alg){
    if (alg_is_aes(alg))    return EVP_aes_256_gcm();
#if defined(EVP_chacha20_poly1305)
    if (alg_is_chacha(alg)) return EVP_chacha20_poly1305();
#endif
    return NULL;
}

int crypto_aead_encrypt(const char* alg,
                        const uint8_t* key, size_t keylen,
                        const uint8_t* nonce, size_t noncelen,
                        const uint8_t* ad, size_t adlen,
                        const uint8_t* pt, size_t ptlen,
                        uint8_t* out, size_t* out_len)
{
    if (!alg || !key || !nonce || (!pt && ptlen) || !out || !out_len) { errno = EINVAL; return -1; }
    const EVP_CIPHER* c = cipher_from_alg(alg);
    if (!c) { errno = ENOSYS; return -1; }
    if (keylen != crypto_aead_keybytes(alg) || noncelen != crypto_aead_noncebytes(alg)) { errno = EINVAL; return -1; }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { errno = EIO; return -1; }

    int rc = -1, len = 0, outl = 0;
    if (EVP_EncryptInit_ex(ctx, c, NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)noncelen, NULL) != 1) goto done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto done;

    if (ad && adlen) {
        if (EVP_EncryptUpdate(ctx, NULL, &len, ad, (int)adlen) != 1) goto done;
    }

    if (ptlen) {
        if (EVP_EncryptUpdate(ctx, out, &len, pt, (int)ptlen) != 1) goto done;
        outl = len;
    }

    if (EVP_EncryptFinal_ex(ctx, out + outl, &len) != 1) goto done;
    outl += len;

    uint8_t tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) != 1) goto done;

    memcpy(out + outl, tag, 16);
    outl += 16;

    *out_len = (size_t)outl;
    rc = 0;
done:
    EVP_CIPHER_CTX_free(ctx);
    if (rc != 0) errno = EIO;
    return rc;
}

int crypto_aead_decrypt(const char* alg,
                        const uint8_t* key, size_t keylen,
                        const uint8_t* nonce, size_t noncelen,
                        const uint8_t* ad, size_t adlen,
                        const uint8_t* ct_tag, size_t ct_tag_len,
                        uint8_t* out_pt, size_t* out_ptlen)
{
    if (!alg || !key || !nonce || !ct_tag || ct_tag_len < 16 || !out_pt || !out_ptlen) { errno = EINVAL; return -1; }
    const EVP_CIPHER* c = cipher_from_alg(alg);
    if (!c) { errno = ENOSYS; return -1; }
    if (keylen != crypto_aead_keybytes(alg) || noncelen != crypto_aead_noncebytes(alg)) { errno = EINVAL; return -1; }

    size_t taglen = 16;
    size_t ctlen = ct_tag_len - taglen;
    const uint8_t* tag = ct_tag + ctlen;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { errno = EIO; return -1; }

    int rc = -1, len = 0, outl = 0;
    if (EVP_DecryptInit_ex(ctx, c, NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)noncelen, NULL) != 1) goto done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto done;

    if (ad && adlen) {
        if (EVP_DecryptUpdate(ctx, NULL, &len, ad, (int)adlen) != 1) goto done;
    }

    if (ctlen) {
        if (EVP_DecryptUpdate(ctx, out_pt, &len, ct_tag, (int)ctlen) != 1) goto done;
        outl = len;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, (int)taglen, (void*)tag) != 1) goto done;

    if (EVP_DecryptFinal_ex(ctx, out_pt + outl, &len) != 1) { errno = EAUTH; goto done; }
    outl += len;
    *out_ptlen = (size_t)outl;
    rc = 0;
done:
    EVP_CIPHER_CTX_free(ctx);
    if (rc != 0 && errno == 0) errno = EIO;
    return rc;
}

#else
// ---------- stubs when OpenSSL missing ----------

size_t crypto_aead_keybytes(const char* alg){ (void)alg; return 0; }
size_t crypto_aead_noncebytes(const char* alg){ (void)alg; return 0; }
size_t crypto_aead_tagbytes(const char* alg){ (void)alg; return 0; }

int crypto_aead_encrypt(const char* alg,
                        const uint8_t* key, size_t keylen,
                        const uint8_t* nonce, size_t noncelen,
                        const uint8_t* ad, size_t adlen,
                        const uint8_t* pt, size_t ptlen,
                        uint8_t* out, size_t* out_len)
{
    (void)alg;(void)key;(void)keylen;(void)nonce;(void)noncelen;(void)ad;(void)adlen;(void)pt;(void)ptlen;(void)out;(void)out_len;
    errno = ENOSYS; return -1;
}

int crypto_aead_decrypt(const char* alg,
                        const uint8_t* key, size_t keylen,
                        const uint8_t* nonce, size_t noncelen,
                        const uint8_t* ad, size_t adlen,
                        const uint8_t* ct_tag, size_t ct_tag_len,
                        uint8_t* out_pt, size_t* out_ptlen)
{
    (void)alg;(void)key;(void)keylen;(void)nonce;(void)noncelen;(void)ad;(void)adlen;(void)ct_tag;(void)ct_tag_len;(void)out_pt;(void)out_ptlen;
    errno = ENOSYS; return -1;
}
#endif

// ---------- Optional demo ----------
#ifdef CRYPTO_DEMO
#include <stdio.h>
int main(void){
#if defined(VL_HAVE_OPENSSL)
    uint8_t key[32], nonce[12];
    crypto_random(key, sizeof key);
    crypto_random(nonce, sizeof nonce);
    const char* alg = "CHACHA20-POLY1305";
    const uint8_t ad[] = "hdr";
    const uint8_t msg[] = "hello";
    uint8_t ct[sizeof msg + 16]; size_t ctlen=0;
    if (crypto_aead_encrypt(alg, key, 32, nonce, 12, ad, sizeof ad, msg, sizeof msg, ct, &ctlen)!=0){ perror("enc"); return 1; }
    uint8_t pt[sizeof msg]; size_t ptlen=0;
    if (crypto_aead_decrypt(alg, key, 32, nonce, 12, ad, sizeof ad, ct, ctlen, pt, &ptlen)!=0){ perror("dec"); return 1; }
    printf("ok: %zu bytes\n", ptlen);
#else
    puts("OpenSSL not enabled.");
#endif
    return 0;
}
#endif
