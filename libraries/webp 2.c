// SPDX-License-Identifier: GPL-3.0-or-later
//
// webp.c — I/O WebP minimal: load RGBA, save RGBA (C17, portable)
// Namespace: "webpio"
//
// Build (with libwebp):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DHAVE_WEBP -c webp.c \
//      -I/usr/include -L/usr/lib -lwebp
//
// API:
//   int  webp_is(const void* buf, size_t n);                           // 1 si header WEBP
//   int  webp_load_file(const char* path, uint8_t** rgba, int* w, int* h); // 0 ok, -1 err
//   int  webp_save_file(const char* path, const uint8_t* rgba, int w, int h, int quality); // 0 ok
//   void webp_free(uint8_t* p);                                        // libère *rgba de load_file
//
// Notes:
//   - Retourne -1 si libwebp absente (sans HAVE_WEBP).
//   - webp_load_file alloue *rgba; libérer via webp_free().

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef WEBP_API
#define WEBP_API
#endif

#if defined(HAVE_WEBP)
  #include <webp/decode.h>
  #include <webp/encode.h>
#endif

/* ===== Utils ===== */

WEBP_API int webp_is(const void* buf, size_t n){
    if (!buf || n < 12) return 0;
    const unsigned char* p = (const unsigned char*)buf;
    /* "RIFF" .... "WEBP" */
    return p[0]=='R'&&p[1]=='I'&&p[2]=='F'&&p[3]=='F' &&
           p[8]=='W'&&p[9]=='E'&&p[10]=='B'&&p[11]=='P';
}

WEBP_API void webp_free(uint8_t* p){
#if defined(HAVE_WEBP)
    WebPFree(p);
#else
    free(p);
#endif
}

/* ===== Load ===== */

#if !defined(HAVE_WEBP)
/* Fallback stubs */
WEBP_API int webp_load_file(const char* path, uint8_t** rgba, int* w, int* h){
    (void)path; (void)rgba; (void)w; (void)h; return -1;
}
WEBP_API int webp_save_file(const char* path, const uint8_t* rgba, int w, int h, int quality){
    (void)path; (void)rgba; (void)w; (void)h; (void)quality; return -1;
}
#else

static int read_entire_file(const char* path, unsigned char** data, size_t* n){
    *data=NULL; *n=0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)!=0){ fclose(f); return -1; }
    long L = ftell(f);
    if (L < 0){ fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET)!=0){ fclose(f); return -1; }
    unsigned char* buf = (unsigned char*)malloc((size_t)L);
    if (!buf){ fclose(f); return -1; }
    size_t r = fread(buf,1,(size_t)L,f);
    fclose(f);
    if (r!=(size_t)L){ free(buf); return -1; }
    *data=buf; *n=(size_t)L; return 0;
}

WEBP_API int webp_load_file(const char* path, uint8_t** rgba, int* w, int* h){
    if (!path || !rgba || !w || !h) return -1;
    *rgba=NULL; *w=0; *h=0;

    unsigned char* data=NULL; size_t n=0;
    if (read_entire_file(path, &data, &n)!=0) return -1;

    int width=0, height=0;
    if (!WebPGetInfo(data, n, &width, &height)){ free(data); return -1; }

    uint8_t* out = WebPDecodeRGBA(data, n, &width, &height);
    free(data);
    if (!out) return -1;

    *rgba = out; *w=width; *h=height;
    return 0;
}

/* ===== Save ===== */

WEBP_API int webp_save_file(const char* path, const uint8_t* rgba, int w, int h, int quality){
    if (!path || !rgba || w<=0 || h<=0) return -1;
    if (quality<0) quality=0; if (quality>100) quality=100;

    uint8_t* enc = NULL;
    size_t enc_size = WebPEncodeRGBA(rgba, w, h, w*4, (float)quality, &enc);
    if (enc_size==0 || !enc) return -1;

    FILE* f = fopen(path, "wb");
    if (!f){ WebPFree(enc); return -1; }
    size_t wrc = fwrite(enc, 1, enc_size, f);
    int rc = (wrc==enc_size)? 0 : -1;
    fclose(f);
    WebPFree(enc);
    return rc;
}
#endif /* HAVE_WEBP */

/* ===== Test ===== */
#ifdef WEBP_TEST
int main(int argc, char** argv){
    if (argc<3){ fprintf(stderr,"usage: %s in.webp out.webp\n", argv[0]); return 1; }
    uint8_t* rgba=NULL; int w=0,h=0;
    if (webp_load_file(argv[1], &rgba, &w, &h)!=0){ fprintf(stderr,"decode fail\n"); return 2; }
    fprintf(stderr,"decoded %dx%d\n", w, h);
    int rc = webp_save_file(argv[2], rgba, w, h, 90);
    webp_free(rgba);
    if (rc!=0){ fprintf(stderr,"encode fail\n"); return 3; }
    return 0;
}
#endif