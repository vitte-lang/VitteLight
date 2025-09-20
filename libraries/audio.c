// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio.c — Outils audio C17 portables pour Vitte Light
// Namespace: "aud"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c audio.c
//
// Fournit:
//   - PCM conversions: i16<->f32, clamp, deinterleave/interleave
//   - WAV (PCM 16-bit) I/O: aud_wav_write16, aud_wav_read16
//   - Mesures: peak, rms
//   - Resampler linéaire: up/down simple (mono/stéréo interleavé)
//   - Ring buffer d’échantillons float (interleavé)
//   - Générateurs de fenêtres: hann
//
// Notes:
//   - Endianness: WAV little-endian. Gère htole/letoh fallback.
//   - Pas de dépendances externes.
//
// Option test:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DAUD_TEST audio.c && ./a.out

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

/* ==================== Endianness helpers ==================== */
static inline uint16_t _aud_bswap16(uint16_t v){ return (uint16_t)((v>>8) | (v<<8)); }
static inline uint32_t _aud_bswap32(uint32_t v){
    return (v>>24) | ((v>>8)&0x0000FF00u) | ((v<<8)&0x00FF0000u) | (v<<24);
}

/* Détection d’endianess: MSVC/Windows ou macros GCC/Clang. */
#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
  #define aud_le16(x) (x)
  #define aud_le32(x) (x)
#elif defined(__BIG_ENDIAN__) || \
      (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
  #define aud_le16(x) _aud_bswap16((uint16_t)(x))
  #define aud_le32(x) _aud_bswap32((uint32_t)(x))
#else
  /* Fallback conservateur: suppose little-endian. Ajustez si besoin. */
  #define aud_le16(x) (x)
  #define aud_le32(x) (x)
#endif

#ifndef AUD_API
#define AUD_API
#endif

typedef float  f32;
typedef double f64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t  i16;

/* ==================== Clamp & conversions ==================== */

static inline f32  aud_clampf(f32 x, f32 lo, f32 hi){ return x<lo?lo:(x>hi?hi:x); }
AUD_API void aud_i16_to_f32(const i16* src, f32* dst, size_t n){
    const f32 s = 1.0f/32768.0f;
    for (size_t i=0;i<n;i++) dst[i] = (f32)src[i] * s;
}
AUD_API void aud_f32_to_i16(const f32* src, i16* dst, size_t n){
    for (size_t i=0;i<n;i++){
        f32 x = aud_clampf(src[i], -1.0f, 1.0f);
        int v = (int)lrintf(x * 32767.0f);
        if (v > 32767) v = 32767; if (v < -32768) v = -32768;
        dst[i] = (i16)v;
    }
}

/* De/interleave (nframes * channels) */
AUD_API void aud_deinterleave_f32(const f32* in, f32** out_ch, int ch, size_t frames){
    for (int c=0;c<ch;c++){
        f32* o = out_ch[c];
        for (size_t i=0;i<frames;i++) o[i] = in[i*ch + c];
    }
}
AUD_API void aud_interleave_f32(const f32* const* in_ch, f32* out, int ch, size_t frames){
    for (size_t i=0;i<frames;i++){
        for (int c=0;c<ch;c++) out[i*ch + c] = in_ch[c][i];
    }
}

/* ==================== WAV 16-bit PCM I/O ==================== */

#pragma pack(push,1)
typedef struct {
    u32 riff_id;    /* "RIFF" */
    u32 riff_sz;    /* 4 + (8+fmt) + (8+data) */
    u32 wave_id;    /* "WAVE" */
    u32 fmt_id;     /* "fmt " */
    u32 fmt_sz;     /* 16 for PCM */
    u16 audio_fmt;  /* 1 = PCM */
    u16 num_ch;
    u32 sample_rate;
    u32 byte_rate;  /* sr*ch*bits/8 */
    u16 block_align;/* ch*bits/8 */
    u16 bits_per_sample;
    u32 data_id;    /* "data" */
    u32 data_sz;    /* nbytes */
} aud_wav_hdr16;
#pragma pack(pop)

static u32 _mk4(const char s[4]){ return ((u32)s[0])|((u32)s[1]<<8)|((u32)s[2]<<16)|((u32)s[3]<<24); }

AUD_API int aud_wav_write16(const char* path, const i16* interleaved, size_t frames, int ch, int sr){
    if (!path || !interleaved || ch<=0 || sr<=0) return -1;
    FILE* f = fopen(path, "wb"); if (!f) return -1;
    size_t nbytes = frames * (size_t)ch * 2;
    aud_wav_hdr16 h;
    h.riff_id = _mk4("RIFF");
    h.riff_sz = aud_le32(4 + (8+16) + (8 + (u32)nbytes));
    h.wave_id = _mk4("WAVE");
    h.fmt_id  = _mk4("fmt ");
    h.fmt_sz  = aud_le32(16);
    h.audio_fmt = aud_le16(1);
    h.num_ch   = aud_le16((u16)ch);
    h.sample_rate = aud_le32((u32)sr);
    h.byte_rate   = aud_le32((u32)(sr*ch*2));
    h.block_align = aud_le16((u16)(ch*2));
    h.bits_per_sample = aud_le16(16);
    h.data_id = _mk4("data");
    h.data_sz = aud_le32((u32)nbytes);

    int ok = (fwrite(&h, 1, sizeof h, f)==sizeof h) &&
             (fwrite(interleaved, 1, nbytes, f)==nbytes);
    fclose(f);
    return ok?0:-1;
}

/* Lecture de WAV PCM16 basique. Alloue *out (i16*). Retourne frames, ou 0 en erreur. */
AUD_API size_t aud_wav_read16(const char* path, i16** out, int* ch, int* sr){
    if (!path || !out || !ch || !sr) return 0;
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    aud_wav_hdr16 h;
    if (fread(&h,1,sizeof h,f) != sizeof h){ fclose(f); return 0; }
    if (h.riff_id!=_mk4("RIFF") || h.wave_id!=_mk4("WAVE") || h.fmt_id!=_mk4("fmt ") || h.data_id!=_mk4("data")){
        fclose(f); return 0;
    }
    u16 fmt = h.audio_fmt;
    u16 bits = h.bits_per_sample;
    u16 nch  = h.num_ch;
    u32 sr_  = h.sample_rate;
    u32 datasz = h.data_sz;
    if (fmt!=aud_le16(1) || bits!=aud_le16(16) || nch==0 || sr_==0){ fclose(f); return 0; }
    size_t nbytes = datasz;
    size_t nsamp = nbytes/2;
    i16* buf = (i16*)malloc(nbytes);
    if (!buf){ fclose(f); return 0; }
    if (fread(buf,1,nbytes,f)!=nbytes){ free(buf); fclose(f); return 0; }
    fclose(f);
    *out = buf; *ch = (int)nch; *sr = (int)sr_;
    return nsamp/(size_t)nch;
}

/* ==================== Mesures ==================== */
AUD_API void aud_peak_rms_f32(const f32* in, size_t n, f32* peak, f32* rms){
    f32 p=0.0f; f64 acc=0.0;
    for (size_t i=0;i<n;i++){
        f32 a = fabsf(in[i]);
        if (a>p) p=a;
        acc += (f64)in[i]*(f64)in[i];
    }
    if (peak) *peak=p;
    if (rms)  *rms = n? (f32)sqrt(acc/(f64)n) : 0.0f;
}

/* ==================== Resampler linéaire ==================== */
/* in/out interleavé. ratio=out_sr/in_sr. state: frac position par canal commun. */
typedef struct {
    double pos; /* position source en frames */
} aud_resamp_lin;

AUD_API void aud_resamp_lin_init(aud_resamp_lin* s){ s->pos=0.0; }

/* Retourne frames écrites. out_frames_max est la capacité. */
AUD_API size_t aud_resamp_lin_process(aud_resamp_lin* st,
                                      const f32* in, size_t in_frames, int ch,
                                      f32* out, size_t out_frames_max,
                                      double ratio){
    if (ratio<=0.0 || ch<=0) return 0;
    size_t outw=0;
    double pos = st->pos;
    const double end = (double)in_frames - 1.000001; /* éviter dépassement sur t+1 */
    while (outw < out_frames_max){
        double t = pos;
        if (t > end) break;
        size_t i0 = (size_t)t;
        double frac = t - (double)i0;
        size_t i1 = i0 + 1 < in_frames ? i0 + 1 : i0;
        for (int c=0;c<ch;c++){
            f32 a = in[i0*ch + c];
            f32 b = in[i1*ch + c];
            out[outw*ch + c] = (f32)((1.0-frac)*a + frac*b);
        }
        pos += ratio;
        outw++;
    }
    st->pos = pos - (double)in_frames; /* autorise feed par blocs, position relative */
    if (st->pos < 0.0) st->pos = 0.0;   /* si buffer entièrement consommé */
    return outw;
}

/* ==================== Ring buffer float interleavé ==================== */

typedef struct {
    f32*  buf;
    size_t cap_frames;
    size_t r, w;      /* indices en frames */
    int    ch;
    bool   full;
} aud_ring;

AUD_API int aud_ring_init(aud_ring* rb, size_t cap_frames, int ch){
    if (!rb || cap_frames==0 || ch<=0) return -1;
    rb->buf = (f32*)malloc(cap_frames * (size_t)ch * sizeof(f32));
    if (!rb->buf) return -1;
    rb->cap_frames = cap_frames; rb->r=rb->w=0; rb->ch=ch; rb->full=false; return 0;
}
AUD_API void aud_ring_free(aud_ring* rb){
    if (!rb) return; free(rb->buf); rb->buf=NULL; rb->cap_frames=rb->r=rb->w=0; rb->ch=0; rb->full=false;
}
static inline size_t _ring_len(const aud_ring* rb){
    if (rb->full) return rb->cap_frames;
    if (rb->w>=rb->r) return rb->w - rb->r;
    return rb->cap_frames - (rb->r - rb->w);
}
AUD_API size_t aud_ring_available(const aud_ring* rb){ return _ring_len(rb); }
AUD_API size_t aud_ring_space(const aud_ring* rb){ return rb->cap_frames - _ring_len(rb); }

AUD_API size_t aud_ring_push(aud_ring* rb, const f32* interleaved, size_t frames){
    size_t ch = (size_t)rb->ch;
    size_t push = frames;
    size_t space = aud_ring_space(rb);
    if (push > space) push = space;
    size_t first = (rb->w >= rb->r && !rb->full) ? (rb->cap_frames - rb->w) : (rb->r - rb->w);
    if (first > push) first = push;
    memcpy(&rb->buf[rb->w*ch], interleaved, first*ch*sizeof(f32));
    if (push > first){
        memcpy(&rb->buf[0], interleaved + first*ch, (push-first)*ch*sizeof(f32));
    }
    rb->w = (rb->w + push) % rb->cap_frames;
    rb->full = (rb->w == rb->r);
    return push;
}
AUD_API size_t aud_ring_pop(aud_ring* rb, f32* interleaved_out, size_t frames){
    size_t ch = (size_t)rb->ch;
    size_t avail = aud_ring_available(rb);
    if (frames > avail) frames = avail;
    size_t first = (rb->w > rb->r || rb->full) ? (rb->cap_frames - rb->r) : (rb->w - rb->r);
    if (first > frames) first = frames;
    memcpy(interleaved_out, &rb->buf[rb->r*ch], first*ch*sizeof(f32));
    if (frames > first){
        memcpy(interleaved_out + first*ch, &rb->buf[0], (frames-first)*ch*sizeof(f32));
    }
    rb->r = (rb->r + frames) % rb->cap_frames;
    rb->full = false;
    return frames;
}

/* ==================== Fenêtres ==================== */
AUD_API void aud_window_hann(f32* w, size_t n){
    if (!w || n==0) return;
    for (size_t i=0;i<n;i++){
        w[i] = 0.5f - 0.5f * (f32)cos((2.0*M_PI*i)/(n-1? n-1 : 1));
    }
}

/* ==================== Test ==================== */
#ifdef AUD_TEST
int main(void){
    /* Sine 440 Hz, 1s, stéréo */
    int sr=48000, ch=2; size_t frames=sr;
    f32* x = (f32*)malloc(frames*ch*sizeof(f32));
    for (size_t i=0;i<frames;i++){
        f32 s = sinf(2.0f*(f32)M_PI*440.0f*(f32)i/(f32)sr);
        x[i*ch+0]=s; x[i*ch+1]=s;
    }
    i16* pcm = (i16*)malloc(frames*ch*sizeof(i16));
    aud_f32_to_i16(x, pcm, frames*ch);
    aud_wav_write16("test.wav", pcm, frames, ch, sr);

    /* lecture */
    i16* in=NULL; int rch=0, rsr=0; size_t fr = aud_wav_read16("test.wav",&in,&rch,&rsr);
    printf("read frames=%zu ch=%d sr=%d\n", fr, rch, rsr);

    /* resample x0.5 */
    aud_resamp_lin rs; aud_resamp_lin_init(&rs);
    size_t out_cap = frames; f32* y = (f32*)malloc(out_cap*ch*sizeof(f32));
    size_t yfr = aud_resamp_lin_process(&rs, x, frames, ch, y, out_cap, 0.5);
    printf("resampled frames=%zu\n", yfr);

    /* ring */
    aud_ring rb; aud_ring_init(&rb, 1024, ch);
    size_t p = aud_ring_push(&rb, x, 900);
    size_t q = aud_ring_pop(&rb, y, 256);
    printf("ring push=%zu pop=%zu avail=%zu space=%zu\n", p, q, aud_ring_available(&rb), aud_ring_space(&rb));
    aud_ring_free(&rb);

    /* metrics */
    f32 peak, rms; aud_peak_rms_f32(x, frames*ch, &peak, &rms);
    printf("peak=%.3f rms=%.3f\n", peak, rms);

    free(y); free(x); free(pcm); free(in);
    return 0;
}
#endif