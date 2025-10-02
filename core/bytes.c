// SPDX-License-Identifier: MIT
/* ============================================================================
   core/bytes.c — Utilitaires C11 « ultra complets » pour buffers binaires
   API prévue par core/bytes.h (lecture/écriture, varints, endian, hex, b64).
   Dépendances : libc uniquement.
   ============================================================================
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#include <intrin.h>
#endif

#include "bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Portabilité types / export (si bytes.h n’a pas été inclus correctement)    */
/* -------------------------------------------------------------------------- */
#ifndef API_EXPORT
# if defined(_WIN32)
#  define API_EXPORT __declspec(dllexport)
# else
#  define API_EXPORT __attribute__((visibility("default")))
# endif
#endif

#ifndef VL_TYPES_MIN
#define VL_TYPES_MIN 1
#include <stddef.h>
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef size_t   usize;
typedef ptrdiff_t isize;
#endif

/* -------------------------------------------------------------------------- */
/* Structures locales si absentes (compat bytes.h)                            */
/* -------------------------------------------------------------------------- */
#ifndef BYTES_STRUCTS_DEFINED
typedef struct {
  u8*   data;
  usize len;
  usize cap;
  usize pos; /* position de lecture/écriture logique */
} ByteBuf;

typedef struct {
  const u8* data;
  usize     len;
  usize     pos;
} ByteRO;
#endif

/* -------------------------------------------------------------------------- */
/* Helpers généraux                                                           */
/* -------------------------------------------------------------------------- */
static inline void* xrealloc(void* p, usize n) {
  void* q = realloc(p, n ? n : 1);
  if (!q) { fprintf(stderr, "bytes: realloc OOM (%zu)\n", (size_t)n); abort(); }
  return q;
}
static inline void* xmalloc(usize n) {
  void* p = malloc(n ? n : 1);
  if (!p) { fprintf(stderr, "bytes: malloc OOM (%zu)\n", (size_t)n); abort(); }
  return p;
}

#define CLAMP(x,l,h) ((x)<(l)?(l):((x)>(h)?(h):(x)))
#define MIN(a,b) ((a)<(b)?(a):(b))

/* -------------------------------------------------------------------------- */
/* Endian helpers                                                             */
/* -------------------------------------------------------------------------- */
static inline u16 bswap16(u16 v){
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap16(v);
#elif defined(_MSC_VER)
  return _byteswap_ushort(v);
#else
  return (u16)((v<<8)|(v>>8));
#endif
}
static inline u32 bswap32(u32 v){
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap32(v);
#elif defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return ((v & 0x000000FFu) << 24) |
         ((v & 0x0000FF00u) << 8 ) |
         ((v & 0x00FF0000u) >> 8 ) |
         ((v & 0xFF000000u) >> 24);
#endif
}
static inline u64 bswap64(u64 v){
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap64(v);
#elif defined(_MSC_VER)
  return _byteswap_uint64(v);
#else
  v = ((v & 0x00000000FFFFFFFFull) << 32) | ((v & 0xFFFFFFFF00000000ull) >> 32);
  v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v & 0xFFFF0000FFFF0000ull) >> 16);
  v = ((v & 0x00FF00FF00FF00FFull) << 8 ) | ((v & 0xFF00FF00FF00FF00ull) >> 8 );
  return v;
#endif
}

static inline int host_is_little(void){
  const uint16_t x = 1;
  return *(const uint8_t*)&x == 1;
}

static inline u16 to_le16(u16 v){ return host_is_little()? v : bswap16(v); }
static inline u32 to_le32(u32 v){ return host_is_little()? v : bswap32(v); }
static inline u64 to_le64(u64 v){ return host_is_little()? v : bswap64(v); }

static inline u16 to_be16(u16 v){ return host_is_little()? bswap16(v) : v; }
static inline u32 to_be32(u32 v){ return host_is_little()? bswap32(v) : v; }
static inline u64 to_be64(u64 v){ return host_is_little()? bswap64(v) : v; }

static inline u16 from_le16(u16 v){ return to_le16(v); }
static inline u32 from_le32(u32 v){ return to_le32(v); }
static inline u64 from_le64(u64 v){ return to_le64(v); }

static inline u16 from_be16(u16 v){ return to_be16(v); }
static inline u32 from_be32(u32 v){ return to_be32(v); }
static inline u64 from_be64(u64 v){ return to_be64(v); }

/* -------------------------------------------------------------------------- */
/* ByteBuf: gestion mémoire                                                   */
/* -------------------------------------------------------------------------- */
API_EXPORT void bb_init(ByteBuf* b) {
  b->data = NULL; b->len = 0; b->cap = 0; b->pos = 0;
}
API_EXPORT void bb_free(ByteBuf* b) {
  if (!b) return;
  free(b->data);
  b->data = NULL; b->len = b->cap = b->pos = 0;
}
API_EXPORT void bb_clear(ByteBuf* b) {
  if (!b) return;
  b->len = 0; b->pos = 0;
}
API_EXPORT void bb_reserve(ByteBuf* b, usize need) {
  if (b->cap >= need) return;
  usize ncap = b->cap ? b->cap : 64;
  while (ncap < need) ncap <<= 1;
  b->data = (u8*)xrealloc(b->data, ncap);
  b->cap = ncap;
}
API_EXPORT void bb_resize(ByteBuf* b, usize n) {
  bb_reserve(b, n);
  b->len = n;
  if (b->pos > b->len) b->pos = b->len;
}
API_EXPORT u8* bb_mut_data(ByteBuf* b){ return b->data; }
API_EXPORT const u8* bb_data(const ByteBuf* b){ return b->data; }
API_EXPORT usize bb_len(const ByteBuf* b){ return b->len; }
API_EXPORT usize bb_cap(const ByteBuf* b){ return b->cap; }
API_EXPORT usize bb_pos(const ByteBuf* b){ return b->pos; }
API_EXPORT void  bb_seek(ByteBuf* b, usize pos){ b->pos = CLAMP(pos, 0, b->len); }

/* write raw */
API_EXPORT void bb_write(ByteBuf* b, const void* src, usize n) {
  if (!n) return;
  usize need = b->pos + n;
  if (need > b->len) {
    if (need > b->cap) bb_reserve(b, need);
    b->len = need;
  }
  memcpy(b->data + b->pos, src, n);
  b->pos += n;
}
/* append raw at end regardless of pos; returns start offset */
API_EXPORT usize bb_append(ByteBuf* b, const void* src, usize n){
  usize start = b->len;
  if (n) {
    usize need = b->len + n;
    if (need > b->cap) bb_reserve(b, need);
    memcpy(b->data + b->len, src, n);
    b->len = need;
  }
  return start;
}

/* ensure room then return pointer for caller to fill */
API_EXPORT u8* bb_alloc_tail(ByteBuf* b, usize n){
  usize start = b->len;
  if (n) {
    usize need = b->len + n;
    if (need > b->cap) bb_reserve(b, need);
    b->len = need;
  }
  return b->data + start;
}

/* -------------------------------------------------------------------------- */
/* Écritures scalaires                                                        */
/* -------------------------------------------------------------------------- */
API_EXPORT void bb_w_u8 (ByteBuf* b, u8  v){ bb_write(b, &v, 1); }
API_EXPORT void bb_w_i8 (ByteBuf* b, i8  v){ bb_write(b, &v, 1); }
API_EXPORT void bb_w_le16(ByteBuf* b, u16 v){ u16 t=to_le16(v); bb_write(b,&t,2); }
API_EXPORT void bb_w_le32(ByteBuf* b, u32 v){ u32 t=to_le32(v); bb_write(b,&t,4); }
API_EXPORT void bb_w_le64(ByteBuf* b, u64 v){ u64 t=to_le64(v); bb_write(b,&t,8); }
API_EXPORT void bb_w_be16(ByteBuf* b, u16 v){ u16 t=to_be16(v); bb_write(b,&t,2); }
API_EXPORT void bb_w_be32(ByteBuf* b, u32 v){ u32 t=to_be32(v); bb_write(b,&t,4); }
API_EXPORT void bb_w_be64(ByteBuf* b, u64 v){ u64 t=to_be64(v); bb_write(b,&t,8); }

/* IEEE754 (binaire identique) */
API_EXPORT void bb_w_f32_le(ByteBuf* b, float f){ u32 u; memcpy(&u,&f,4); bb_w_le32(b,u); }
API_EXPORT void bb_w_f64_le(ByteBuf* b, double d){ u64 u; memcpy(&u,&d,8); bb_w_le64(b,u); }
API_EXPORT void bb_w_f32_be(ByteBuf* b, float f){ u32 u; memcpy(&u,&f,4); bb_w_be32(b,u); }
API_EXPORT void bb_w_f64_be(ByteBuf* b, double d){ u64 u; memcpy(&u,&d,8); bb_w_be64(b,u); }

/* Varints (LEB128-U, ZigZag-S) */
API_EXPORT void bb_w_varu(ByteBuf* b, u64 v){
  while (v >= 0x80u) {
    u8 byte = (u8)((v & 0x7Fu) | 0x80u);
    bb_w_u8(b, byte);
    v >>= 7;
  }
  bb_w_u8(b, (u8)v);
}
static inline u64 zigzag_enc(i64 x){ return (u64)((x << 1) ^ (x >> 63)); }
API_EXPORT void bb_w_vari(ByteBuf* b, i64 v){ bb_w_varu(b, zigzag_enc(v)); }

/* Chaînes UTF-8 prefixées longueur varuint */
API_EXPORT void bb_w_str(ByteBuf* b, const char* s){
  usize n = s ? strlen(s) : 0;
  bb_w_varu(b, (u64)n);
  if (n) bb_write(b, s, n);
}

/* -------------------------------------------------------------------------- */
/* Lectures via ByteRO                                                        */
/* -------------------------------------------------------------------------- */
API_EXPORT void br_init(ByteRO* r, const void* data, usize n){
  r->data = (const u8*)data; r->len = n; r->pos = 0;
}
API_EXPORT usize br_remaining(const ByteRO* r){ return (r->pos <= r->len) ? (r->len - r->pos) : 0; }
API_EXPORT const u8* br_peek(const ByteRO* r, usize* avail){
  if (avail) *avail = br_remaining(r);
  return r->data + r->pos;
}
API_EXPORT bool br_skip(ByteRO* r, usize n){
  if (br_remaining(r) < n) return false;
  r->pos += n; return true;
}
API_EXPORT bool br_read(ByteRO* r, void* dst, usize n){
  if (br_remaining(r) < n) return false;
  memcpy(dst, r->data + r->pos, n);
  r->pos += n;
  return true;
}
API_EXPORT bool br_r_u8 (ByteRO* r, u8*  v){ return br_read(r,v,1); }
API_EXPORT bool br_r_i8 (ByteRO* r, i8*  v){ return br_read(r,v,1); }
API_EXPORT bool br_r_le16(ByteRO* r, u16* v){ u16 t; if(!br_read(r,&t,2))return false; *v=from_le16(t); return true; }
API_EXPORT bool br_r_le32(ByteRO* r, u32* v){ u32 t; if(!br_read(r,&t,4))return false; *v=from_le32(t); return true; }
API_EXPORT bool br_r_le64(ByteRO* r, u64* v){ u64 t; if(!br_read(r,&t,8))return false; *v=from_le64(t); return true; }
API_EXPORT bool br_r_be16(ByteRO* r, u16* v){ u16 t; if(!br_read(r,&t,2))return false; *v=from_be16(t); return true; }
API_EXPORT bool br_r_be32(ByteRO* r, u32* v){ u32 t; if(!br_read(r,&t,4))return false; *v=from_be32(t); return true; }
API_EXPORT bool br_r_be64(ByteRO* r, u64* v){ u64 t; if(!br_read(r,&t,8))return false; *v=from_be64(t); return true; }

API_EXPORT bool br_r_f32_le(ByteRO* r, float*  f){ u32 u; if(!br_r_le32(r,&u))return false; memcpy(f,&u,4); return true; }
API_EXPORT bool br_r_f64_le(ByteRO* r, double* d){ u64 u; if(!br_r_le64(r,&u))return false; memcpy(d,&u,8); return true; }
API_EXPORT bool br_r_f32_be(ByteRO* r, float*  f){ u32 u; if(!br_r_be32(r,&u))return false; memcpy(f,&u,4); return true; }
API_EXPORT bool br_r_f64_be(ByteRO* r, double* d){ u64 u; if(!br_r_be64(r,&u))return false; memcpy(d,&u,8); return true; }

API_EXPORT bool br_r_varu(ByteRO* r, u64* out){
  u64 v = 0; int shift = 0;
  while (1) {
    if (shift >= 64) return false;
    u8 b = 0;
    if (!br_r_u8(r, &b)) return false;
    v |= (u64)(b & 0x7Fu) << shift;
    if ((b & 0x80u) == 0) break;
    shift += 7;
  }
  *out = v; return true;
}
static inline i64 zigzag_dec(u64 u){ return (i64)((u >> 1) ^ (~(u & 1) + 1)); }
API_EXPORT bool br_r_vari(ByteRO* r, i64* out){
  u64 u; if (!br_r_varu(r, &u)) return false;
  *out = zigzag_dec(u); return true;
}
API_EXPORT bool br_r_str(ByteRO* r, const char** s, usize* n){
  u64 ln = 0; if (!br_r_varu(r, &ln)) return false;
  if (br_remaining(r) < (usize)ln) return false;
  *s = (const char*)(r->data + r->pos);
  *n = (usize)ln;
  r->pos += (usize)ln;
  return true;
}

/* -------------------------------------------------------------------------- */
/* Hex encode/decode + hexdump                                                */
/* -------------------------------------------------------------------------- */
static const char HEXU[] = "0123456789ABCDEF";
static const char HEXL[] = "0123456789abcdef";

API_EXPORT void hex_encode(const void* data, usize n, bool upper, ByteBuf* out){
  const u8* p = (const u8*)data;
  bb_reserve(out, out->len + n*2);
  const char* ALPH = upper ? HEXU : HEXL;
  for (usize i=0;i<n;i++){
    u8 v = p[i];
    u8 h = (u8)(v>>4), l = (u8)(v & 0xF);
    bb_w_u8(out, (u8)ALPH[h]);
    bb_w_u8(out, (u8)ALPH[l]);
  }
}

static inline int hex_val(int c){
  if (c>='0' && c<='9') return c-'0';
  if (c>='a' && c<='f') return c-'a'+10;
  if (c>='A' && c<='F') return c-'A'+10;
  return -1;
}
API_EXPORT bool hex_decode(const char* s, usize n, ByteBuf* out){
  if ((n & 1u) != 0) return false;
  bb_reserve(out, out->len + n/2);
  for (usize i=0;i<n;i+=2){
    int hi = hex_val((unsigned char)s[i]);
    int lo = hex_val((unsigned char)s[i+1]);
    if (hi < 0 || lo < 0) return false;
    u8 v = (u8)((hi<<4) | lo);
    bb_w_u8(out, v);
  }
  return true;
}

API_EXPORT void hexdump(FILE* fp, const void* data, usize n, usize base_addr){
  const u8* p = (const u8*)data;
  for (usize i=0;i<n;i+=16){
    fprintf(fp, "%08zx  ", base_addr + i);
    usize row = MIN(16, n - i);
    for (usize j=0;j<16;j++){
      if (j<row) fprintf(fp, "%02X ", p[i+j]);
      else fputs("   ", fp);
      if (j==7) fputc(' ', fp);
    }
    fputc(' ', fp);
    for (usize j=0;j<row;j++){
      u8 c = p[i+j];
      fputc((c>=32 && c<127)? c : '.', fp);
    }
    fputc('\n', fp);
  }
}

/* -------------------------------------------------------------------------- */
/* Base64 (RFC 4648, sans sauts de ligne)                                     */
/* -------------------------------------------------------------------------- */
static const char B64TAB[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline u8 b64_val(int c){
  if (c>='A'&&c<='Z') return (u8)(c-'A');
  if (c>='a'&&c<='z') return (u8)(c-'a'+26);
  if (c>='0'&&c<='9') return (u8)(c-'0'+52);
  if (c=='+') return 62;
  if (c=='/') return 63;
  return 0xFFu;
}

API_EXPORT void b64_encode(const void* data, usize n, ByteBuf* out){
  const u8* s = (const u8*)data;
  usize q = n/3, r = n%3;
  bb_reserve(out, out->len + 4*((n+2)/3));
  for (usize i=0;i<q;i++){
    u32 v = (u32)(s[3*i]<<16 | s[3*i+1]<<8 | s[3*i+2]);
    bb_w_u8(out, (u8)B64TAB[(v>>18)&63]);
    bb_w_u8(out, (u8)B64TAB[(v>>12)&63]);
    bb_w_u8(out, (u8)B64TAB[(v>>6 )&63]);
    bb_w_u8(out, (u8)B64TAB[(v    )&63]);
  }
  if (r){
    u32 v = (u32)(s[3*q]<<16);
    if (r==2) v |= (u32)(s[3*q+1]<<8);
    bb_w_u8(out, (u8)B64TAB[(v>>18)&63]);
    bb_w_u8(out, (u8)B64TAB[(v>>12)&63]);
    if (r==2){
      bb_w_u8(out, (u8)B64TAB[(v>>6)&63]);
      bb_w_u8(out, '=');
    } else {
      bb_w_u8(out, '=');
      bb_w_u8(out, '=');
    }
  }
}

API_EXPORT bool b64_decode(const char* s, usize n, ByteBuf* out){
  if (n==0) return true;
  if (n%4 != 0) return false;
  /* worst-case size */
  bb_reserve(out, out->len + (n/4)*3);
  for (usize i=0;i<n;i+=4){
    u8 a=b64_val(s[i]), b=b64_val(s[i+1]);
    if (a==0xFFu || b==0xFFu) return false;
    u8 c = (s[i+2]=='=')? 0xFEu : b64_val(s[i+2]);
    u8 d = (s[i+3]=='=')? 0xFEu : b64_val(s[i+3]);
    if ((c==0xFFu)||(d==0xFFu)) return false;

    u32 v = ((u32)a<<18) | ((u32)b<<12) |
            ((c<=63? (u32)c : 0)<<6) |
            ((d<=63? (u32)d : 0));

    bb_w_u8(out, (u8)((v>>16)&0xFF));
    if (c != 0xFEu) bb_w_u8(out, (u8)((v>>8)&0xFF));
    if (d != 0xFEu) bb_w_u8(out, (u8)(v&0xFF));
  }
  return true;
}

/* -------------------------------------------------------------------------- */
/* Slicing / views                                                            */
/* -------------------------------------------------------------------------- */
API_EXPORT ByteRO bb_view(const ByteBuf* b, usize off, usize n){
  ByteRO r;
  off = CLAMP(off, 0, b->len);
  usize end = CLAMP(off + n, 0, b->len);
  r.data = b->data + off;
  r.len  = end - off;
  r.pos  = 0;
  return r;
}

/* -------------------------------------------------------------------------- */
/* Recherche naïve et utilitaires simples                                     */
/* -------------------------------------------------------------------------- */
API_EXPORT isize bytes_find(const u8* hay, usize hay_n, const u8* needle, usize nee_n){
  if (nee_n == 0) return 0;
  if (hay_n < nee_n) return -1;
  for (usize i=0;i<=hay_n-nee_n;i++){
    if (memcmp(hay+i, needle, nee_n)==0) return (isize)i;
  }
  return -1;
}

API_EXPORT bool bytes_eq(const u8* a, const u8* b, usize n){
  if (n==0) return true;
  return memcmp(a,b,n)==0;
}

/* -------------------------------------------------------------------------- */
/* Démo facultative                                                           */
/* -------------------------------------------------------------------------- */
#ifdef BYTES_DEMO_MAIN
int main(void){
  ByteBuf b; bb_init(&b);
  bb_w_be32(&b, 0xDEADBEEF);
  bb_w_str(&b, "hello");
  ByteRO r = bb_view(&b, 0, bb_len(&b));

  u32 v=0; br_r_be32(&r,&v);
  const char* s=NULL; usize n=0; br_r_str(&r,&s,&n);
  printf("v=%08X s=%.*s\n", v, (int)n, s);

  ByteBuf hx; bb_init(&hx); hex_encode(bb_data(&b), bb_len(&b), false, &hx);
  printf("hex=%.*s\n", (int)bb_len(&hx), (const char*)bb_data(&hx));

  ByteBuf b64; bb_init(&b64); b64_encode(bb_data(&b), bb_len(&b), &b64);
  printf("b64=%.*s\n", (int)bb_len(&b64), (const char*)bb_data(&b64));

  hexdump(stdout, bb_data(&b), bb_len(&b), 0);

  bb_free(&b); bb_free(&hx); bb_free(&b64);
  return 0;
}
#endif
