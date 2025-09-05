// SPDX-License-Identifier: GPL-3.0-or-later
//
// cbor.c — CBOR (RFC 8949) low-level encoder/decoder for Vitte Light VM (C17,
// complet) Namespace: "cbor"
//
// Design:
//   - Pure C, no external deps. Binary-safe via AuxBuffer.
//   - Encoder: incremental builder API. You push items in CBOR order.
//   - Decoder: pull API returning token + payload per item (stream-friendly).
//
// Encoder API:
//   e = cbor.new()                              -> id | (nil,errmsg)
//   cbor.reset(e)                               -> true
//   cbor.free(e)                                -> true
//   cbor.append_raw(e, bytes)                   -> true
//   cbor.result(e)                              -> bytes
//   cbor.result_len(e)                          -> int
//
//   // Major types
//   cbor.uint(e, u:int64)                       -> true
//   cbor.nint(e, n:int64)                       -> true            // encodes
//   -1-n (negative) cbor.bytes(e, s)                            -> true
//   cbor.text(e, s)                             -> true
//   cbor.array(e, len:int)                      -> true            // definite
//   length cbor.map(e, len:int)                        -> true            //
//   definite length cbor.tag(e, tag:uint64)                     -> true // next
//   item is tagged cbor.simple(e, val:int)                     -> true //
//   0..19, 24..255 (rare) cbor.bool(e, b:bool)                        -> true
//   cbor.null(e)                                -> true
//   cbor.undef(e)                               -> true
//   cbor.float64(e, x:number)                   -> true
//
// Decoder API:
//   d = cbor.decoder(bytes)                     -> id | (nil,errmsg)
//   cbor.next(d)
//      -> "uint",   value:uint64
//       | "nint",   value:int64                 // already negative
//       | "bytes",  data:string
//       | "text",   data:string
//       | "array",  len:int                     // -1 if indefinite (not
//       produced by encoder API) | "map",    len:int                     // -1
//       if indefinite | "tag",    tag:uint64 | "bool",   0|1 | "null" | "undef"
//       | "float",  f:number
//       | (nil,"eof") | (nil,"EINVAL")
//   cbor.free_decoder(d)                        -> true
//
// Notes:
//   - Only definite-length arrays/maps emitted by encoder.
//   - Decoder accepts both definite/indefinite. When 0xFF encountered, returns
//   "break" token.
//   - All integers accepted as signed 64-bit for API; encoding follows CBOR
//   canonical rules.
//
// Depends: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *cb_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t cb_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static double cb_check_num(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? (double)vl_toint(S, vl_get(S, idx))
                            : vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: number expected", idx);
  vl_error(S);
  return 0.0;
}
static int cb_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)cb_check_int(S, idx);
  return defv;
}
static int cb_check_bool(VL_State *S, int idx) {
  if (vl_get(S, idx)) return vl_tobool(vl_get(S, idx)) ? 1 : 0;
  vl_errorf(S, "argument #%d: bool expected", idx);
  vl_error(S);
  return 0;
}

// ---------------------------------------------------------------------
// Core: CBOR write helpers
// ---------------------------------------------------------------------
static void cb_put_u8(AuxBuffer *b, uint8_t v) { aux_buffer_append(b, &v, 1); }
static void cb_put_be16(AuxBuffer *b, uint16_t v) {
  uint8_t t[2] = {(uint8_t)(v >> 8), (uint8_t)v};
  aux_buffer_append(b, t, 2);
}
static void cb_put_be32(AuxBuffer *b, uint32_t v) {
  uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                  (uint8_t)v};
  aux_buffer_append(b, t, 4);
}
static void cb_put_be64(AuxBuffer *b, uint64_t v) {
  uint8_t t[8] = {(uint8_t)(v >> 56), (uint8_t)(v >> 48), (uint8_t)(v >> 40),
                  (uint8_t)(v >> 32), (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 8),  (uint8_t)v};
  aux_buffer_append(b, t, 8);
}
static void cb_put_head(AuxBuffer *b, uint8_t major, uint64_t val) {
  if (val <= 23) {
    cb_put_u8(b, (uint8_t)((major << 5) | (uint8_t)val));
  } else if (val <= 0xFF) {
    cb_put_u8(b, (uint8_t)((major << 5) | 24));
    cb_put_u8(b, (uint8_t)val);
  } else if (val <= 0xFFFF) {
    cb_put_u8(b, (uint8_t)((major << 5) | 25));
    cb_put_be16(b, (uint16_t)val);
  } else if (val <= 0xFFFFFFFFu) {
    cb_put_u8(b, (uint8_t)((major << 5) | 26));
    cb_put_be32(b, (uint32_t)val);
  } else {
    cb_put_u8(b, (uint8_t)((major << 5) | 27));
    cb_put_be64(b, val);
  }
}

// ---------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------
typedef struct CborEnc {
  int used;
  AuxBuffer buf;
} CborEnc;

static CborEnc *g_enc = NULL;
static int g_enc_cap = 0;

static int ensure_enc_cap(int need) {
  if (need <= g_enc_cap) return 1;
  int ncap = g_enc_cap ? g_enc_cap : 8;
  while (ncap < need) ncap <<= 1;
  CborEnc *ne = (CborEnc *)realloc(g_enc, (size_t)ncap * sizeof *ne);
  if (!ne) return 0;
  for (int i = g_enc_cap; i < ncap; i++) {
    ne[i].used = 0;
    memset(&ne[i].buf, 0, sizeof ne[i].buf);
  }
  g_enc = ne;
  g_enc_cap = ncap;
  return 1;
}
static int alloc_enc_slot(void) {
  for (int i = 1; i < g_enc_cap; i++)
    if (!g_enc[i].used) return i;
  if (!ensure_enc_cap(g_enc_cap ? g_enc_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_enc_cap; i++)
    if (!g_enc[i].used) return i;
  return 0;
}
static int check_eid(int id) {
  return id > 0 && id < g_enc_cap && g_enc[id].used;
}

// VM: cbor.new()
static int vlcbor_new(VL_State *S) {
  int id = alloc_enc_slot();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_enc[id].used = 1;
  aux_buffer_reset(&g_enc[id].buf);
  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlcbor_reset(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  aux_buffer_reset(&g_enc[id].buf);
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_free(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  if (id > 0 && id < g_enc_cap && g_enc[id].used) {
    aux_buffer_free(&g_enc[id].buf);
    g_enc[id].used = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_append_raw(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  const char *bytes = cb_check_str(S, 2);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  aux_buffer_append(&g_enc[id].buf, (const uint8_t *)bytes, strlen(bytes));
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_result(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_lstring(S, (const char *)g_enc[id].buf.data, (int)g_enc[id].buf.len);
  return 1;
}
static int vlcbor_result_len(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_int(S, (int64_t)g_enc[id].buf.len);
  return 1;
}

// Primitives
static int vlcbor_uint(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  int64_t v = cb_check_int(S, 2);
  if (!check_eid(id) || v < 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  cb_put_head(&g_enc[id].buf, 0, (uint64_t)v);
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_nint(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  int64_t n = cb_check_int(S, 2);
  if (!check_eid(id) || n > 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  uint64_t m = (uint64_t)(-1 - n);
  cb_put_head(&g_enc[id].buf, 1, m);
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_bytes(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  const char *s = cb_check_str(S, 2);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  size_t n = strlen(s);
  cb_put_head(&g_enc[id].buf, 2, (uint64_t)n);
  aux_buffer_append(&g_enc[id].buf, (const uint8_t *)s, n);
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_text(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  const char *s = cb_check_str(S, 2);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  size_t n = strlen(s);
  cb_put_head(&g_enc[id].buf, 3, (uint64_t)n);
  aux_buffer_append(&g_enc[id].buf, (const uint8_t *)s, n);
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_array(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  int len = (int)cb_check_int(S, 2);
  if (!check_eid(id) || len < 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  cb_put_head(&g_enc[id].buf, 4, (uint64_t)len);
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_map(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  int len = (int)cb_check_int(S, 2);
  if (!check_eid(id) || len < 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  cb_put_head(&g_enc[id].buf, 5, (uint64_t)len);
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_tag(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  int64_t tag = cb_check_int(S, 2);
  if (!check_eid(id) || tag < 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  cb_put_head(&g_enc[id].buf, 6, (uint64_t)tag);
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_simple(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  int val = (int)cb_check_int(S, 2);
  if (!check_eid(id) || val < 0 || val > 255) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  if (val <= 23) {
    cb_put_u8(&g_enc[id].buf, (uint8_t)((7 << 5) | (uint8_t)val));
  } else {
    cb_put_u8(&g_enc[id].buf, (uint8_t)((7 << 5) | 24));
    cb_put_u8(&g_enc[id].buf, (uint8_t)val);
  }
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_bool(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  int b = cb_check_bool(S, 2);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  cb_put_u8(&g_enc[id].buf, (uint8_t)((7 << 5) | (b ? 21 : 20)));
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_null(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  cb_put_u8(&g_enc[id].buf, (uint8_t)((7 << 5) | 22));
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_undef(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  cb_put_u8(&g_enc[id].buf, (uint8_t)((7 << 5) | 23));
  vl_push_bool(S, 1);
  return 1;
}
static int vlcbor_float64(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  double x = cb_check_num(S, 2);
  if (!check_eid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  union {
    double d;
    uint64_t u;
  } u;
  u.d = x;
  cb_put_u8(&g_enc[id].buf, (uint8_t)((7 << 5) | 27));
  cb_put_be64(&g_enc[id].buf, u.u);
  vl_push_bool(S, 1);
  return 1;
}

// ---------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------
typedef struct CborDec {
  int used;
  AuxBuffer src;  // owning copy of input bytes
  size_t off;     // current offset
} CborDec;

static CborDec *g_dec = NULL;
static int g_dec_cap = 0;

static int ensure_dec_cap(int need) {
  if (need <= g_dec_cap) return 1;
  int ncap = g_dec_cap ? g_dec_cap : 8;
  while (ncap < need) ncap <<= 1;
  CborDec *nd = (CborDec *)realloc(g_dec, (size_t)ncap * sizeof *nd);
  if (!nd) return 0;
  for (int i = g_dec_cap; i < ncap; i++) {
    nd[i].used = 0;
    memset(&nd[i].src, 0, sizeof nd[i].src);
    nd[i].off = 0;
  }
  g_dec = nd;
  g_dec_cap = ncap;
  return 1;
}
static int alloc_dec_slot(void) {
  for (int i = 1; i < g_dec_cap; i++)
    if (!g_dec[i].used) return i;
  if (!ensure_dec_cap(g_dec_cap ? g_dec_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_dec_cap; i++)
    if (!g_dec[i].used) return i;
  return 0;
}
static int check_did(int id) {
  return id > 0 && id < g_dec_cap && g_dec[id].used;
}

static int vlcbor_decoder(VL_State *S) {
  const char *bytes = cb_check_str(S, 1);
  size_t n = strlen(bytes);
  int id = alloc_dec_slot();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  aux_buffer_reset(&g_dec[id].src);
  aux_buffer_append(&g_dec[id].src, (const uint8_t *)bytes, n);
  g_dec[id].off = 0;
  g_dec[id].used = 1;
  vl_push_int(S, (int64_t)id);
  return 1;
}
static int vlcbor_free_decoder(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  if (id > 0 && id < g_dec_cap && g_dec[id].used) {
    aux_buffer_free(&g_dec[id].src);
    g_dec[id].used = 0;
    g_dec[id].off = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

static int cbor_read_u8(CborDec *d, uint8_t *out) {
  if (d->off >= d->src.len) return 0;
  *out = d->src.data[d->off++];
  return 1;
}
static int cbor_read_n(CborDec *d, size_t n, uint64_t *out) {
  if (d->off + n > d->src.len) return 0;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    v = (v << 8) | d->src.data[d->off++];
  }
  *out = v;
  return 1;
}

static int vlcbor_next(VL_State *S) {
  int id = (int)cb_check_int(S, 1);
  if (!check_did(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  CborDec *d = &g_dec[id];

  if (d->off >= d->src.len) {
    vl_push_nil(S);
    vl_push_string(S, "eof");
    return 2;
  }

  uint8_t ib;
  if (!cbor_read_u8(d, &ib)) {
    vl_push_nil(S);
    vl_push_string(S, "eof");
    return 2;
  }
  uint8_t major = ib >> 5;
  uint8_t ai = ib & 31;

  // BREAK stop code
  if (ib == 0xFF) {
    vl_push_string(S, "break");
    return 1;
  }

  uint64_t val = 0;
  if (ai < 24) {
    val = ai;
  } else if (ai == 24) {
    uint64_t t;
    if (!cbor_read_n(d, 1, &t)) {
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
    val = t;
  } else if (ai == 25) {
    uint64_t t;
    if (!cbor_read_n(d, 2, &t)) {
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
    val = t;
  } else if (ai == 26) {
    uint64_t t;
    if (!cbor_read_n(d, 4, &t)) {
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
    val = t;
  } else if (ai == 27) {
    uint64_t t;
    if (!cbor_read_n(d, 8, &t)) {
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
    val = t;
  } else if (ai == 31) {
    // Indefinite-length for bytes/text/array/map
    val = (uint64_t)-1;
  } else {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  switch (major) {
    case 0:  // uint
      vl_push_string(S, "uint");
      vl_push_int(S, (int64_t)val);
      return 2;
    case 1:  // nint (negative) value = -1 - val
      vl_push_string(S, "nint");
      vl_push_int(S, (int64_t)(-1 - (int64_t)val));
      return 2;
    case 2: {  // bytes
      if (ai == 31) {
        vl_push_string(S, "bytes");
        vl_push_int(S, -1);
        return 2;
      }  // caller expects chunks then "break"
      if (d->off + val > d->src.len) {
        vl_push_nil(S);
        vl_push_string(S, "EINVAL");
        return 2;
      }
      vl_push_string(S, "bytes");
      vl_push_lstring(S, (const char *)d->src.data + d->off, (int)val);
      d->off += val;
      return 2;
    }
    case 3: {  // text
      if (ai == 31) {
        vl_push_string(S, "text");
        vl_push_int(S, -1);
        return 2;
      }
      if (d->off + val > d->src.len) {
        vl_push_nil(S);
        vl_push_string(S, "EINVAL");
        return 2;
      }
      vl_push_string(S, "text");
      vl_push_lstring(S, (const char *)d->src.data + d->off, (int)val);
      d->off += val;
      return 2;
    }
    case 4: {  // array
      vl_push_string(S, "array");
      vl_push_int(S, ai == 31 ? -1 : (int64_t)val);
      return 2;
    }
    case 5: {  // map
      vl_push_string(S, "map");
      vl_push_int(S, ai == 31 ? -1 : (int64_t)val);
      return 2;
    }
    case 6: {  // tag
      vl_push_string(S, "tag");
      vl_push_int(S, (int64_t)val);
      return 2;
    }
    case 7: {
      if (ai == 20 || ai == 21) {
        vl_push_string(S, "bool");
        vl_push_int(S, ai == 21 ? 1 : 0);
        return 2;
      }
      if (ai == 22) {
        vl_push_string(S, "null");
        return 1;
      }
      if (ai == 23) {
        vl_push_string(S, "undef");
        return 1;
      }
      if (ai == 25 || ai == 26 ||
          ai == 27) {  // float16/32/64; we always parse to float64
        uint64_t raw = 0;
        if (ai == 25) {
          if (!cbor_read_n(d, 2, &raw)) {
            vl_push_nil(S);
            vl_push_string(S, "EINVAL");
            return 2;
          }
          // float16 -> float32 path: decode half quickly (approx)
          uint16_t h = (uint16_t)raw;
          uint32_t sign = (uint32_t)(h >> 15) & 1u;
          uint32_t exp = (uint32_t)(h >> 10) & 0x1Fu;
          uint32_t mant = (uint32_t)(h & 0x3FFu);
          double out;
          if (exp == 0) {
            out = (mant ? (mant / 1024.0) * pow(2, -14) : 0.0);
          } else if (exp == 31) {
            out = mant ? (0.0 / 0.0) : (1.0 / 0.0);
          } else {
            out = (1.0 + mant / 1024.0) * ldexp(1.0, (int)exp - 15);
          }
          if (sign) out = -out;
          vl_push_string(S, "float");
          vl_push_float(S, out);
          return 2;
        }
        if (ai == 26) {
          if (!cbor_read_n(d, 4, &raw)) {
            vl_push_nil(S);
            vl_push_string(S, "EINVAL");
            return 2;
          }
          union {
            uint32_t u;
            float f;
          } u;
          u.u = (uint32_t)raw;
          vl_push_string(S, "float");
          vl_push_float(S, (double)u.f);
          return 2;
        }
        if (ai == 27) {
          if (!cbor_read_n(d, 8, &raw)) {
            vl_push_nil(S);
            vl_push_string(S, "EINVAL");
            return 2;
          }
          union {
            uint64_t u;
            double d;
          } u;
          u.u = raw;
          vl_push_string(S, "float");
          vl_push_float(S, u.d);
          return 2;
        }
      }
      // other simple values
      vl_push_string(S, "simple");
      vl_push_int(S, (int64_t)val);
      return 2;
    }
    default:
      break;
  }

  vl_push_nil(S);
  vl_push_string(S, "EINVAL");
  return 2;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------
static const VL_Reg cborlib[] = {
    // Encoder
    {"new", vlcbor_new},
    {"reset", vlcbor_reset},
    {"free", vlcbor_free},
    {"append_raw", vlcbor_append_raw},
    {"result", vlcbor_result},
    {"result_len", vlcbor_result_len},
    {"uint", vlcbor_uint},
    {"nint", vlcbor_nint},
    {"bytes", vlcbor_bytes},
    {"text", vlcbor_text},
    {"array", vlcbor_array},
    {"map", vlcbor_map},
    {"tag", vlcbor_tag},
    {"simple", vlcbor_simple},
    {"bool", vlcbor_bool},
    {"null", vlcbor_null},
    {"undef", vlcbor_undef},
    {"float64", vlcbor_float64},

    // Decoder
    {"decoder", vlcbor_decoder},
    {"next", vlcbor_next},
    {"free_decoder", vlcbor_free_decoder},

    {NULL, NULL}};

void vl_open_cborlib(VL_State *S) { vl_register_lib(S, "cbor", cborlib); }
