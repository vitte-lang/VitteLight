// SPDX-License-Identifier: GPL-3.0-or-later
//
// codec.c — Encoders/Decoders utilitaires (C17, portable)
// Namespace: "codec"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c codec.c
//
// Couverture:
//   - Base64: codec_b64_encode / codec_b64_decode
//   - Hex:    codec_hex_encode / codec_hex_decode
//   - URL %:  codec_url_encode / codec_url_decode (RFC3986 unreserved)
//   - UTF-8:  codec_utf8_validate
//   - Checksums: codec_crc32, codec_adler32
//
// Modèle d’API: sorties allouées par la fonction, à free() par l’appelant.
//   int codec_b64_encode(const void* in, size_t n, char** out, size_t* outlen);
//   int codec_b64_decode(const char* in, size_t n, unsigned char** out, size_t* outlen);
//   int codec_hex_encode(const void* in, size_t n, int upper, char** out, size_t* outlen);
//   int codec_hex_decode(const char* in, size_t n, unsigned char** out, size_t* outlen);
//   int codec_url_encode(const char* in, size_t n, char** out, size_t* outlen);
//   int codec_url_decode(const char* in, size_t n, char** out, size_t* outlen);
//   int codec_utf8_validate(const unsigned char* s, size_t n); // 1 ok, 0 invalide
//   uint32_t codec_crc32(uint32_t crc, const void* buf, size_t n);      // init 0
//   uint32_t codec_adler32(uint32_t adler, const void* buf, size_t n);  // init 1
//
// Notes:
//   - Retour 0 = OK, -1 = erreur (OOM/entrée invalide).
//   - Pas de dépendances externes.
//
// Option tests:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DCODEC_TEST codec.c && ./a.out

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define RET_ERR() do { return -1; } while (0)
#define RET_OK()  do { return  0; } while (0)

/* ========================= Base64 ========================= */

static const char B64TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const unsigned char B64REV[256] = {
    /* init at first call */
};

static void b64rev_init(unsigned char rev[256]) {
    static int done = 0;
    if (done) return;
    for (int i = 0; i < 256; i++) rev[i] = 0x80; // invalid
    for (int i = 0; i < 64; i++) rev[(unsigned char)B64TAB[i]] = (unsigned char)i;
    rev[(unsigned char)'='] = 0x40; // pad
    done = 1;
}

int codec_b64_encode(const void* in, size_t n, char** out, size_t* outlen) {
    const unsigned char* p = (const unsigned char*)in;
    size_t olen = ((n + 2) / 3) * 4;
    char* o = (char*)malloc(olen + 1);
    if (!o) RET_ERR();
    size_t i = 0, j = 0;
    while (i + 3 <= n) {
        uint32_t v = (p[i] << 16) | (p[i+1] << 8) | p[i+2];
        o[j++] = B64TAB[(v >> 18) & 63];
        o[j++] = B64TAB[(v >> 12) & 63];
        o[j++] = B64TAB[(v >> 6) & 63];
        o[j++] = B64TAB[v & 63];
        i += 3;
    }
    if (i < n) {
        uint32_t v = p[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2) v |= p[i+1] << 8;
        o[j++] = B64TAB[(v >> 18) & 63];
        o[j++] = B64TAB[(v >> 12) & 63];
        o[j++] = (rem == 2) ? B64TAB[(v >> 6) & 63] : '=';
        o[j++] = '=';
    }
    o[j] = 0;
    *out = o;
    if (outlen) *outlen = j;
    RET_OK();
}

int codec_b64_decode(const char* in, size_t n, unsigned char** out, size_t* outlen) {
    b64rev_init((unsigned char*)B64REV);
    const unsigned char* s = (const unsigned char*)in;
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) n--; // trim
    if (n % 4 != 0) RET_ERR();
    size_t olen = (n / 4) * 3;
    if (n >= 4) {
        if (s[n-1] == '=') olen--;
        if (s[n-2] == '=') olen--;
    }
    unsigned char* o = (unsigned char*)malloc(olen ? olen : 1);
    if (!o) RET_ERR();
    size_t i = 0, j = 0;
    for (; i < n; i += 4) {
        unsigned char a = B64REV[s[i]];
        unsigned char b = B64REV[s[i+1]];
        unsigned char c = B64REV[s[i+2]];
        unsigned char d = B64REV[s[i+3]];
        if (a & 0x80 || b & 0x80) { free(o); RET_ERR(); }
        uint32_t v = (a << 18) | (b << 12);
        o[j++] = (v >> 16) & 0xFF;
        if (c != 0x40) {
            if (c & 0x80) { free(o); RET_ERR(); }
            v |= (c << 6);
            o[j++] = (v >> 8) & 0xFF;
            if (d != 0x40) {
                if (d & 0x80) { free(o); RET_ERR(); }
                v |= d;
                o[j++] = v & 0xFF;
            }
        } else {
            if (d != 0x40) { free(o); RET_ERR(); }
        }
    }
    *out = o;
    if (outlen) *outlen = j;
    RET_OK();
}

/* ========================= Hex ========================= */

static inline int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline unsigned to_hex(int v, int upper) {
    v &= 0xF;
    if (v < 10) return '0' + v;
    return (upper ? 'A' : 'a') + (v - 10);
}
static inline int from_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int codec_hex_encode(const void* in, size_t n, int upper, char** out, size_t* outlen) {
    const unsigned char* p = (const unsigned char*)in;
    size_t olen = n * 2;
    char* o = (char*)malloc(olen + 1);
    if (!o) RET_ERR();
    for (size_t i = 0, j = 0; i < n; i++) {
        o[j++] = (char)to_hex(p[i] >> 4, upper);
        o[j++] = (char)to_hex(p[i], upper);
    }
    o[olen] = 0;
    *out = o;
    if (outlen) *outlen = olen;
    RET_OK();
}

int codec_hex_decode(const char* in, size_t n, unsigned char** out, size_t* outlen) {
    if (n % 2 != 0) RET_ERR();
    for (size_t i = 0; i < n; i++) if (!is_hex(in[i])) RET_ERR();
    size_t olen = n / 2;
    unsigned char* o = (unsigned char*)malloc(olen ? olen : 1);
    if (!o) RET_ERR();
    for (size_t i = 0, j = 0; i < n; i += 2) {
        int hi = from_hex(in[i]);
        int lo = from_hex(in[i+1]);
        o[j++] = (unsigned char)((hi << 4) | lo);
    }
    *out = o;
    if (outlen) *outlen = olen;
    RET_OK();
}

/* ========================= URL percent-encoding ========================= */

static int is_unreserved(unsigned char c) {
    // ALPHA / DIGIT / "-" / "." / "_" / "~"
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    return c == '-' || c == '.' || c == '_' || c == '~';
}

int codec_url_encode(const char* in, size_t n, char** out, size_t* outlen) {
    const unsigned char* s = (const unsigned char*)in;
    // worst case: every byte -> %XX (3x)
    size_t cap = n * 3 + 1;
    char* o = (char*)malloc(cap);
    if (!o) RET_ERR();
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (is_unreserved(c)) {
            o[j++] = (char)c;
        } else if (c == ' ') {
            o[j++] = '%'; o[j++] = '2'; o[j++] = '0';
        } else {
            static const char HEX[] = "0123456789ABCDEF";
            o[j++] = '%';
            o[j++] = HEX[(c >> 4) & 0xF];
            o[j++] = HEX[c & 0xF];
        }
    }
    o[j] = 0;
    *out = o;
    if (outlen) *outlen = j;
    RET_OK();
}

int codec_url_decode(const char* in, size_t n, char** out, size_t* outlen) {
    char* o = (char*)malloc(n + 1); // decoded length <= n
    if (!o) RET_ERR();
    size_t j = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == '%') {
            if (i + 2 >= n) { free(o); RET_ERR(); }
            int hi = from_hex(in[i+1]);
            int lo = from_hex(in[i+2]);
            if (hi < 0 || lo < 0) { free(o); RET_ERR(); }
            o[j++] = (char)((hi << 4) | lo);
            i += 3;
        } else {
            o[j++] = (char)c;
            i++;
        }
    }
    o[j] = 0;
    *out = o;
    if (outlen) *outlen = j;
    RET_OK();
}

/* ========================= UTF-8 validator ========================= */

int codec_utf8_validate(const unsigned char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i++];
        if (c < 0x80) continue;
        int extra =
            (c >= 0xC2 && c <= 0xDF) ? 1 :
            (c >= 0xE0 && c <= 0xEF) ? 2 :
            (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
        if (extra < 0 || i + (size_t)extra > n) return 0;

        // first byte specific constraints
        if (c == 0xE0 && (s[i] < 0xA0 || s[i] > 0xBF)) return 0;        // U+0800..U+0FFF start
        else if (c == 0xED && (s[i] < 0x80 || s[i] > 0x9F)) return 0;   // no surrogates
        else if (c == 0xF0 && (s[i] < 0x90 || s[i] > 0xBF)) return 0;   // U+10000..U+3FFFF
        else if (c == 0xF4 && (s[i] < 0x80 || s[i] > 0x8F)) return 0;   // up to U+10FFFF
        else if ((c >= 0xC2 && c <= 0xDF) && (s[i] < 0x80 || s[i] > 0xBF)) return 0;

        // continuation bytes
        for (int k = 0; k < extra; k++) {
            if (s[i] < 0x80 || s[i] > 0xBF) return 0;
            i++;
        }
    }
    return 1;
}

/* ========================= Checksums ========================= */

uint32_t codec_crc32(uint32_t crc, const void* buf, size_t n) {
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = 1;
    }
    crc = ~crc;
    const unsigned char* p = (const unsigned char*)buf;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t codec_adler32(uint32_t adler, const void* buf, size_t n) {
    const unsigned MOD = 65521u;
    uint32_t s1 = adler & 0xFFFFu;
    uint32_t s2 = (adler >> 16) & 0xFFFFu;
    const unsigned char* p = (const unsigned char*)buf;
    while (n > 0) {
        size_t t = n > 5552 ? 5552 : n; // block to limit overflow
        n -= t;
        for (size_t i = 0; i < t; i++) {
            s1 += p[i];
            s2 += s1;
        }
        p += t;
        s1 %= MOD;
        s2 %= MOD;
    }
    return (s2 << 16) | s1;
}

/* ========================= Test (optionnel) ========================= */
#ifdef CODEC_TEST
static void dump_hex(const unsigned char* b, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02X", b[i]);
    puts("");
}
int main(void) {
    const char* msg = "Vitte Light — codec";
    char* b64 = NULL; size_t b64n = 0;
    if (codec_b64_encode(msg, strlen(msg), &b64, &b64n) != 0) return 1;
    unsigned char* raw = NULL; size_t rawn = 0;
    if (codec_b64_decode(b64, b64n, &raw, &rawn) != 0) return 1;

    char* hex = NULL; size_t hexn = 0;
    codec_hex_encode(msg, strlen(msg), 1, &hex, &hexn);
    unsigned char* raw2 = NULL; size_t raw2n = 0;
    codec_hex_decode(hex, hexn, &raw2, &raw2n);

    char* url = NULL; size_t urln = 0;
    codec_url_encode(msg, strlen(msg), &url, &urln);
    char* url_dec = NULL; size_t url_decn = 0;
    codec_url_decode(url, urln, &url_dec, &url_decn);

    printf("b64: %s\n", b64);
    printf("url: %s\n", url);
    printf("utf8 ok: %d\n", codec_utf8_validate((const unsigned char*)msg, strlen(msg)));
    printf("crc32: %08X\n", codec_crc32(0, msg, strlen(msg)));
    printf("adler32: %08X\n", codec_adler32(1, msg, strlen(msg)));

    dump_hex(raw, rawn);
    dump_hex(raw2, raw2n);
    printf("url_dec: %.*s\n", (int)url_decn, url_dec);

    free(b64); free(raw); free(hex); free(raw2); free(url); free(url_dec);
    return 0;
}
#endif