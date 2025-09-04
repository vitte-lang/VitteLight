// vitte-light/core/stave.c
// Sérialisation compacte de VL_Value (snapshot mono-valeur) + I/O fichier.
// Format binaire stable: "VLVS" v1 | Tag(1) | Payload
//   Tags: 'N' nil, 'B' bool(u8), 'I' int(i64 LE), 'F' float(f64 LE), 'S'
//   string(u32 len LE + bytes)
// Non supporté ici: ARRAY, MAP, FUNC, NATIVE (retour VL_ERR_UNSUPPORTED).
//
// API suggérée (header optionnel stave.h):
//   VL_Status vl_value_save_to_buffer(const VL_Value *v, VL_Buffer *out);
//   VL_Status vl_value_load_from_buffer(struct VL_Context *ctx, const uint8_t
//   *data, size_t n, VL_Value *out); VL_Status vl_value_save_file(const
//   VL_Value *v, const char *path); VL_Status vl_value_load_file(struct
//   VL_Context *ctx, const char *path, VL_Value *out);
//
// Dépendances: api.h (VL_Value, VL_Status, vlv_*), object.h (VL_String,
// vl_make_strn), mem.h (VL_Buffer, vl_write_file) Build: cc -std=c99 -O2 -Wall
// -Wextra -pedantic -c core/stave.c

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "mem.h"
#include "object.h"

#ifndef VL_SER_MAGIC
#define VL_SER_MAGIC "VLVS"
#endif
#ifndef VL_SER_VERSION
#define VL_SER_VERSION 1u
#endif

// ───────────────────────── Helpers LE ─────────────────────────
static void wr_u8(VL_Buffer *b, uint8_t v) { vl_buf_putc(b, (int)v); }
static void wr_u32(VL_Buffer *b, uint32_t v) {
  uint8_t tmp[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 24)};
  vl_buf_append(b, tmp, 4);
}
static void wr_u64(VL_Buffer *b, uint64_t v) {
  uint8_t tmp[8];
  for (int i = 0; i < 8; i++) tmp[i] = (uint8_t)(v >> (8 * i));
  vl_buf_append(b, tmp, 8);
}
static void wr_f64(VL_Buffer *b, double d) {
  union {
    uint64_t u;
    double d;
  } u;
  u.d = d;
  wr_u64(b, u.u);
}

static int rd_u8(const uint8_t *p, size_t n, size_t *io, uint8_t *out) {
  if (*io + 1 > n) return 0;
  *out = p[(*io)++];
  return 1;
}
static int rd_u32(const uint8_t *p, size_t n, size_t *io, uint32_t *out) {
  if (*io + 4 > n) return 0;
  *out = (uint32_t)p[*io] | ((uint32_t)p[*io + 1] << 8) |
         ((uint32_t)p[*io + 2] << 16) | ((uint32_t)p[*io + 3] << 24);
  *io += 4;
  return 1;
}
static int rd_u64(const uint8_t *p, size_t n, size_t *io, uint64_t *out) {
  if (*io + 8 > n) return 0;
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64_t)p[*io + i]) << (8 * i);
  *io += 8;
  *out = v;
  return 1;
}
static int rd_f64(const uint8_t *p, size_t n, size_t *io, double *out) {
  union {
    uint64_t u;
    double d;
  } u;
  if (!rd_u64(p, n, io, &u.u)) return 0;
  *out = u.d;
  return 1;
}

// ───────────────────────── Écriture ─────────────────────────
static void write_header(VL_Buffer *b) {
  vl_buf_append(b, VL_SER_MAGIC, 4);
  wr_u8(b, (uint8_t)VL_SER_VERSION);
}

VL_Status vl_value_save_to_buffer(const VL_Value *v, VL_Buffer *out) {
  if (!v || !out) return VL_ERR_INVAL;
  // header
  write_header(out);
  // tag + payload
  switch (v->type) {
    case VT_NIL:
      wr_u8(out, 'N');
      break;
    case VT_BOOL:
      wr_u8(out, 'B');
      wr_u8(out, (uint8_t)(v->as.b ? 1 : 0));
      break;
    case VT_INT:
      wr_u8(out, 'I');
      wr_u64(out, (uint64_t)v->as.i);
      break;
    case VT_FLOAT:
      wr_u8(out, 'F');
      wr_f64(out, (double)v->as.f);
      break;
    case VT_STR: {
      wr_u8(out, 'S');
      const VL_String *s = v->as.s;
      uint32_t L = s ? s->len : 0u;
      wr_u32(out, L);
      if (L) {
        vl_buf_append(out, s->data, L);
      }
    } break;
    default:
      return VL_ERR_UNSUPPORTED;
  }
  return VL_OK;
}

// ───────────────────────── Lecture ─────────────────────────
VL_Status vl_value_load_from_buffer(struct VL_Context *ctx, const uint8_t *data,
                                    size_t n, VL_Value *out) {
  if (!data || n < 5 || !out) return VL_ERR_INVAL;
  size_t i = 0;  // i = offset
  // magic
  if (!(data[0] == (uint8_t)VL_SER_MAGIC[0] &&
        data[1] == (uint8_t)VL_SER_MAGIC[1] &&
        data[2] == (uint8_t)VL_SER_MAGIC[2] &&
        data[3] == (uint8_t)VL_SER_MAGIC[3]))
    return VL_ERR_BAD_DATA;
  i = 4;
  uint8_t ver = 0;
  if (!rd_u8(data, n, &i, &ver)) return VL_ERR_BAD_DATA;
  if (ver != VL_SER_VERSION) return VL_ERR_BAD_DATA;

  uint8_t tag = 0;
  if (!rd_u8(data, n, &i, &tag)) return VL_ERR_BAD_DATA;
  switch (tag) {
    case 'N':
      *out = vlv_nil();
      break;
    case 'B': {
      uint8_t b = 0;
      if (!rd_u8(data, n, &i, &b)) return VL_ERR_BAD_DATA;
      *out = vlv_bool(b != 0);
    } break;
    case 'I': {
      uint64_t u = 0;
      if (!rd_u64(data, n, &i, &u)) return VL_ERR_BAD_DATA;
      *out = vlv_int((int64_t)u);
    } break;
    case 'F': {
      double d = 0;
      if (!rd_f64(data, n, &i, &d)) return VL_ERR_BAD_DATA;
      *out = vlv_float(d);
    } break;
    case 'S': {
      uint32_t L = 0;
      if (!rd_u32(data, n, &i, &L)) return VL_ERR_BAD_DATA;
      if (i + (size_t)L > n) return VL_ERR_BAD_DATA;
      VL_Value sv = vl_make_strn(ctx, (const char *)(data + i), (size_t)L);
      if (sv.type != VT_STR) return VL_ERR_OOM;
      *out = sv;
      i += (size_t)L;
    } break;
    default:
      return VL_ERR_BAD_DATA;
  }
  return VL_OK;
}

// ───────────────────────── Fichiers ─────────────────────────
static int read_file_all(const char *path, uint8_t **buf, size_t *n) {
  *buf = NULL;
  *n = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return 0;
  }
  rewind(f);
  uint8_t *p = (uint8_t *)malloc((size_t)sz);
  if (!p) {
    fclose(f);
    return 0;
  }
  size_t rd = fread(p, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    free(p);
    return 0;
  }
  *buf = p;
  *n = (size_t)sz;
  return 1;
}

VL_Status vl_value_save_file(const VL_Value *v, const char *path) {
  if (!v || !path) return VL_ERR_INVAL;
  VL_Buffer b;
  vl_buf_init(&b);
  VL_Status st = vl_value_save_to_buffer(v, &b);
  if (st != VL_OK) {
    vl_buf_free(&b);
    return st;
  }
  int ok = vl_write_file(path, b.d, b.n);
  vl_buf_free(&b);
  return ok ? VL_OK : VL_ERR_IO;
}

VL_Status vl_value_load_file(struct VL_Context *ctx, const char *path,
                             VL_Value *out) {
  if (!path || !out) return VL_ERR_INVAL;
  uint8_t *buf = NULL;
  size_t n = 0;
  if (!read_file_all(path, &buf, &n)) return VL_ERR_IO;
  VL_Status st = vl_value_load_from_buffer(ctx, buf, n, out);
  free(buf);
  return st;
}

// ───────────────────────── Tests ─────────────────────────
#ifdef VL_STAVE_TEST_MAIN
int main(void) {
  VL_Context *ctx = NULL;  // si vl_make_strn a besoin de ctx, passez un vrai
                           // contexte dans vos tests
  VL_Value a = vlv_int(-42), b = vlv_float(3.5), c = vlv_bool(1);
  VL_Value s = vl_make_strn(ctx, "hello", 5);
  VL_Buffer buf;
  vl_buf_init(&buf);
  vl_value_save_to_buffer(&a, &buf);
  vl_value_save_to_buffer(&b, &buf);
  vl_value_save_to_buffer(&c, &buf);
  vl_value_save_to_buffer(&s, &buf);
  // lecture unité par unité
  size_t off = 0;
  VL_Value r;
  r = vlv_nil();
  VL_Status st = vl_value_load_from_buffer(ctx, buf.d + off, buf.n - off, &r);
  (void)st;
  off += 5 + 1 + 8;  // header+tag+payload approximé pour la démo
  r = vlv_nil();
  st = vl_value_load_from_buffer(ctx, buf.d + off, buf.n - off, &r);
  (void)st;
  vl_buf_free(&buf);
  return 0;
}
#endif
