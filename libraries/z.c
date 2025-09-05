// SPDX-License-Identifier: GPL-3.0-or-later
//
// z.c — zlib/gzip bindings pour Vitte Light (C17, complet)
// Namespace VM: "z"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_ZLIB -c z.c
//   cc ... z.o -lz
//
// Portabilité:
//   - Si VL_HAVE_ZLIB et <zlib.h> présents: implémentation réelle.
//   - Sinon: stubs -> (nil,"ENOSYS").
//
// API (one-shot):
//   z.version()                              -> string
//   z.deflate(data[, level=-1[, raw=false[, gzip=false]]]) -> bytes |
//   (nil,errmsg) z.inflate(data[, raw=false[, gzip=false[, max_out=16777216]]])
//   -> bytes | (nil,errmsg) z.gzip(data[, level=-1])                 -> bytes |
//   (nil,errmsg) z.gunzip(data[, max_out=16777216])       -> bytes |
//   (nil,errmsg) z.crc32(data[, seed=0])                  -> uint32
//   z.adler32(data[, seed=1])                -> uint32
//
// API (streaming):
//   -- Deflate
//     z.deflate_init([level=-1[, raw=false[, gzip=false]]]) -> id |
//     (nil,errmsg) z.deflate_chunk(id, bytes[, finish=false])            ->
//     out:string, done:bool | (nil,errmsg) z.deflate_end(id) -> true
//   -- Inflate
//     z.inflate_init([raw=false[, gzip=false]])             -> id |
//     (nil,errmsg) z.inflate_chunk(id, bytes[, finish=false[,
//     max_out_chunk=65536]]) -> out:string, done:bool | (nil,errmsg)
//     z.inflate_end(id)                                     -> true
//
// Notes:
//   - "raw" utilise windowBits = -MAX_WBITS.
//   - "gzip" utilise windowBits = MAX_WBITS+16 (gzip wrapper).
//   - ni "raw" ni "gzip" -> zlib stream (windowBits = MAX_WBITS).
//   - One-shot inflate: si "gzip=true" ignore "raw" et auto-détecte gzip via
//   16+MAX_WBITS.
//   - VM strings supposées 0-terminées; données binaires possibles en sortie
//   via vl_push_lstring().
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef VL_HAVE_ZLIB
#include <zlib.h>
#endif

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *z_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t z_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int z_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int z_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)z_check_int(S, idx);
  return defv;
}

#ifndef VL_HAVE_ZLIB
// ---------------------------------------------------------------------
// STUBS (zlib absent)
// ---------------------------------------------------------------------
#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)
static int vlz_version(VL_State *S) {
  (void)S;
  vl_push_string(S, "zlib not built");
  return 1;
}
static int vlz_deflate(VL_State *S) {
  (void)z_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlz_inflate(VL_State *S) {
  (void)z_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlz_gzip(VL_State *S) {
  (void)z_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlz_gunzip(VL_State *S) {
  (void)z_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlz_crc32(VL_State *S) {
  (void)z_check_str(S, 1);
  vl_push_int(S, 0);
  return 1;
}
static int vlz_adler32(VL_State *S) {
  (void)z_check_str(S, 1);
  vl_push_int(S, 0);
  return 1;
}

static int vlz_d_init(VL_State *S) { NOSYS_PAIR(S); }
static int vlz_d_chunk(VL_State *S) { NOSYS_PAIR(S); }
static int vlz_d_end(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}
static int vlz_i_init(VL_State *S) { NOSYS_PAIR(S); }
static int vlz_i_chunk(VL_State *S) { NOSYS_PAIR(S); }
static int vlz_i_end(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}

#else
// ---------------------------------------------------------------------
// Implémentation réelle (zlib)
// ---------------------------------------------------------------------

// --------- util erreurs ---------
static int push_zerr(VL_State *S, int code, z_stream *zs) {
  const char *m = zs && zs->msg ? zs->msg
                                : (code == Z_MEM_ERROR      ? "ENOMEM"
                                   : code == Z_BUF_ERROR    ? "EAGAIN"
                                   : code == Z_STREAM_ERROR ? "EINVAL"
                                                            : "EIO");
  vl_push_nil(S);
  vl_push_string(S, m);
  return 2;
}

// --------- one-shot deflate/inflate ---------
static int do_deflate_buffer(VL_State *S, const uint8_t *in, size_t inlen,
                             int level, int wbits) {
  z_stream zs;
  memset(&zs, 0, sizeof zs);
  int rc = deflateInit2(&zs, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY);
  if (rc != Z_OK) return push_zerr(S, rc, &zs);

  AuxBuffer out = {0};
  uint8_t buf[65536];

  zs.next_in = (Bytef *)in;
  zs.avail_in = (uInt)inlen;
  int flush = Z_FINISH;
  do {
    zs.next_out = buf;
    zs.avail_out = (uInt)sizeof buf;
    rc = deflate(&zs, flush);
    if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR &&
        rc != Z_STREAM_END) {
      deflateEnd(&zs);
      aux_buffer_free(&out);
      return push_zerr(S, rc, &zs);
    }
    size_t produced = sizeof buf - zs.avail_out;
    if (produced) aux_buffer_append(&out, buf, produced);
  } while (rc != Z_STREAM_END);

  deflateEnd(&zs);
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

static int do_inflate_buffer(VL_State *S, const uint8_t *in, size_t inlen,
                             int wbits, size_t max_out) {
  z_stream zs;
  memset(&zs, 0, sizeof zs);
  int rc = inflateInit2(&zs, wbits);
  if (rc != Z_OK) return push_zerr(S, rc, &zs);

  AuxBuffer out = {0};
  uint8_t buf[65536];

  zs.next_in = (Bytef *)in;
  zs.avail_in = (uInt)inlen;

  while (1) {
    zs.next_out = buf;
    zs.avail_out = (uInt)sizeof buf;
    rc = inflate(&zs, Z_NO_FLUSH);
    if (rc == Z_STREAM_END) {
      size_t produced = sizeof buf - zs.avail_out;
      if (produced) aux_buffer_append(&out, buf, produced);
      break;
    }
    if (rc != Z_OK && rc != Z_BUF_ERROR) {
      inflateEnd(&zs);
      aux_buffer_free(&out);
      return push_zerr(S, rc, &zs);
    }
    size_t produced = sizeof buf - zs.avail_out;
    if (produced) aux_buffer_append(&out, buf, produced);
    if (out.len > max_out) {
      inflateEnd(&zs);
      aux_buffer_free(&out);
      vl_push_nil(S);
      vl_push_string(S, "ERANGE");
      return 2;
    }
    if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;  // no progress
  }

  inflateEnd(&zs);
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

// VM: z.version()
static int vlz_version(VL_State *S) {
  vl_push_string(S, zlibVersion());
  return 1;
}

// VM: z.deflate(data[, level[, raw[, gzip]]])
static int vlz_deflate(VL_State *S) {
  const char *data = z_check_str(S, 1);
  int level = z_opt_int(S, 2, -1);
  int raw = z_opt_bool(S, 3, 0);
  int gzip = z_opt_bool(S, 4, 0);
  if (raw && gzip) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int wbits = gzip ? (MAX_WBITS + 16) : (raw ? -MAX_WBITS : MAX_WBITS);
  return do_deflate_buffer(S, (const uint8_t *)data, strlen(data), level,
                           wbits);
}

// VM: z.inflate(data[, raw[, gzip[, max_out]]])
static int vlz_inflate(VL_State *S) {
  const char *data = z_check_str(S, 1);
  int raw = z_opt_bool(S, 2, 0);
  int gzip = z_opt_bool(S, 3, 0);
  size_t max_out = (size_t)z_opt_int(S, 4, 16 * 1024 * 1024);  // 16 MiB default
  if (raw && gzip) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int wbits = gzip ? (MAX_WBITS + 16) : (raw ? -MAX_WBITS : MAX_WBITS);
  return do_inflate_buffer(S, (const uint8_t *)data, strlen(data), wbits,
                           max_out);
}

// VM: z.gzip(data[, level])
static int vlz_gzip(VL_State *S) {
  const char *data = z_check_str(S, 1);
  int level = z_opt_int(S, 2, -1);
  return do_deflate_buffer(S, (const uint8_t *)data, strlen(data), level,
                           MAX_WBITS + 16);
}

// VM: z.gunzip(data[, max_out])
static int vlz_gunzip(VL_State *S) {
  const char *data = z_check_str(S, 1);
  size_t max_out = (size_t)z_opt_int(S, 2, 16 * 1024 * 1024);
  return do_inflate_buffer(S, (const uint8_t *)data, strlen(data),
                           MAX_WBITS + 16, max_out);
}

// VM: z.crc32(bytes[, seed])
static int vlz_crc32(VL_State *S) {
  const char *data = z_check_str(S, 1);
  uint32_t seed = (uint32_t)z_opt_int(S, 2, 0);
  uLong v = crc32(seed, (const Bytef *)data, (uInt)strlen(data));
  vl_push_int(S, (int64_t)(uint32_t)v);
  return 1;
}

// VM: z.adler32(bytes[, seed])
static int vlz_adler32(VL_State *S) {
  const char *data = z_check_str(S, 1);
  uint32_t seed = (uint32_t)z_opt_int(S, 2, 1);
  uLong v = adler32(seed, (const Bytef *)data, (uInt)strlen(data));
  vl_push_int(S, (int64_t)(uint32_t)v);
  return 1;
}

// --------- streaming ---------
typedef struct ZDefl {
  int used;
  int finished;
  z_stream zs;
} ZDefl;

typedef struct ZInfl {
  int used;
  int finished;
  z_stream zs;
} ZInfl;

static ZDefl *g_d = NULL;
static int g_d_cap = 0;
static ZInfl *g_i = NULL;
static int g_i_cap = 0;

static int ensure_d_cap(int need) {
  if (need <= g_d_cap) return 1;
  int ncap = g_d_cap ? g_d_cap : 8;
  while (ncap < need) ncap <<= 1;
  ZDefl *nd = (ZDefl *)realloc(g_d, (size_t)ncap * sizeof *nd);
  if (!nd) return 0;
  for (int i = g_d_cap; i < ncap; i++) {
    nd[i].used = 0;
    nd[i].finished = 0;
    memset(&nd[i].zs, 0, sizeof nd[i].zs);
  }
  g_d = nd;
  g_d_cap = ncap;
  return 1;
}
static int ensure_i_cap(int need) {
  if (need <= g_i_cap) return 1;
  int ncap = g_i_cap ? g_i_cap : 8;
  while (ncap < need) ncap <<= 1;
  ZInfl *ni = (ZInfl *)realloc(g_i, (size_t)ncap * sizeof *ni);
  if (!ni) return 0;
  for (int i = g_i_cap; i < ncap; i++) {
    ni[i].used = 0;
    ni[i].finished = 0;
    memset(&ni[i].zs, 0, sizeof ni[i].zs);
  }
  g_i = ni;
  g_i_cap = ncap;
  return 1;
}
static int alloc_d_slot(void) {
  for (int i = 1; i < g_d_cap; i++)
    if (!g_d[i].used) return i;
  if (!ensure_d_cap(g_d_cap ? g_d_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_d_cap; i++)
    if (!g_d[i].used) return i;
  return 0;
}
static int alloc_i_slot(void) {
  for (int i = 1; i < g_i_cap; i++)
    if (!g_i[i].used) return i;
  if (!ensure_i_cap(g_i_cap ? g_i_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_i_cap; i++)
    if (!g_i[i].used) return i;
  return 0;
}
static int check_did(int id) { return id > 0 && id < g_d_cap && g_d[id].used; }
static int check_iid(int id) { return id > 0 && id < g_i_cap && g_i[id].used; }

// VM: z.deflate_init([level, raw, gzip])
static int vlz_d_init(VL_State *S) {
  int level = z_opt_int(S, 1, -1);
  int raw = z_opt_bool(S, 2, 0);
  int gzip = z_opt_bool(S, 3, 0);
  if (raw && gzip) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int wbits = gzip ? (MAX_WBITS + 16) : (raw ? -MAX_WBITS : MAX_WBITS);

  int id = alloc_d_slot();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  ZDefl *d = &g_d[id];
  memset(d, 0, sizeof *d);
  int rc =
      deflateInit2(&d->zs, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY);
  if (rc != Z_OK) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  d->used = 1;
  d->finished = 0;
  vl_push_int(S, (int64_t)id);
  return 1;
}

// VM: z.deflate_chunk(id, bytes[, finish=false]) -> out, done
static int vlz_d_chunk(VL_State *S) {
  int id = (int)z_check_int(S, 1);
  const char *in = z_check_str(S, 2);
  int finish = z_opt_bool(S, 3, 0);
  if (!check_did(id) || g_d[id].finished) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  ZDefl *d = &g_d[id];
  AuxBuffer out = {0};
  uint8_t buf[65536];

  d->zs.next_in = (Bytef *)in;
  d->zs.avail_in = (uInt)strlen(in);

  int flush = finish ? Z_FINISH : Z_NO_FLUSH;
  int rc;
  do {
    d->zs.next_out = buf;
    d->zs.avail_out = (uInt)sizeof buf;
    rc = deflate(&d->zs, flush);
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
      aux_buffer_free(&out);
      return push_zerr(S, rc, &d->zs);
    }
    size_t produced = sizeof buf - d->zs.avail_out;
    if (produced) aux_buffer_append(&out, buf, produced);
  } while (d->zs.avail_out == 0);

  if (rc == Z_STREAM_END) d->finished = 1;

  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  vl_push_bool(S, d->finished ? 1 : 0);
  aux_buffer_free(&out);
  return 2;
}

// VM: z.deflate_end(id)
static int vlz_d_end(VL_State *S) {
  int id = (int)z_check_int(S, 1);
  if (check_did(id)) {
    deflateEnd(&g_d[id].zs);
    g_d[id].used = 0;
    g_d[id].finished = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

// VM: z.inflate_init([raw=false[, gzip=false]])
static int vlz_i_init(VL_State *S) {
  int raw = z_opt_bool(S, 1, 0);
  int gzip = z_opt_bool(S, 2, 0);
  if (raw && gzip) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int wbits = gzip ? (MAX_WBITS + 16) : (raw ? -MAX_WBITS : MAX_WBITS);

  int id = alloc_i_slot();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  ZInfl *z = &g_i[id];
  memset(z, 0, sizeof *z);
  int rc = inflateInit2(&z->zs, wbits);
  if (rc != Z_OK) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  z->used = 1;
  z->finished = 0;
  vl_push_int(S, (int64_t)id);
  return 1;
}

// VM: z.inflate_chunk(id, bytes[, finish=false[, max_out_chunk=65536]]) -> out,
// done
static int vlz_i_chunk(VL_State *S) {
  int id = (int)z_check_int(S, 1);
  const char *in = z_check_str(S, 2);
  int finish = z_opt_bool(S, 3, 0);
  int cap = z_opt_int(S, 4, 65536);
  if (cap < 1024) cap = 1024;
  if (!check_iid(id) || g_i[id].finished) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  ZInfl *z = &g_i[id];
  AuxBuffer out = {0};
  uint8_t *buf = (uint8_t *)malloc((size_t)cap);
  if (!buf) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  z->zs.next_in = (Bytef *)in;
  z->zs.avail_in = (uInt)strlen(in);

  int rc;
  do {
    z->zs.next_out = buf;
    z->zs.avail_out = (uInt)cap;
    rc = inflate(&z->zs, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
      free(buf);
      aux_buffer_free(&out);
      return push_zerr(S, rc, &z->zs);
    }
    size_t produced = (size_t)cap - z->zs.avail_out;
    if (produced) aux_buffer_append(&out, buf, produced);
    if (rc == Z_STREAM_END) {
      g_i[id].finished = 1;
      break;
    }
    if (rc == Z_BUF_ERROR && z->zs.avail_in == 0) break;
  } while (z->zs.avail_out == 0 || (finish && z->zs.avail_in));

  free(buf);
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  vl_push_bool(S, g_i[id].finished ? 1 : 0);
  aux_buffer_free(&out);
  return 2;
}

// VM: z.inflate_end(id)
static int vlz_i_end(VL_State *S) {
  int id = (int)z_check_int(S, 1);
  if (check_iid(id)) {
    inflateEnd(&g_i[id].zs);
    g_i[id].used = 0;
    g_i[id].finished = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

#endif  // VL_HAVE_ZLIB

// ---------------------------------------------------------------------
// Registration VM
// ---------------------------------------------------------------------
static const VL_Reg zlibreg[] = {{"version", vlz_version},

                                 // one-shot
                                 {"deflate", vlz_deflate},
                                 {"inflate", vlz_inflate},
                                 {"gzip", vlz_gzip},
                                 {"gunzip", vlz_gunzip},
                                 {"crc32", vlz_crc32},
                                 {"adler32", vlz_adler32},

                                 // streaming
                                 {"deflate_init", vlz_d_init},
                                 {"deflate_chunk", vlz_d_chunk},
                                 {"deflate_end", vlz_d_end},
                                 {"inflate_init", vlz_i_init},
                                 {"inflate_chunk", vlz_i_chunk},
                                 {"inflate_end", vlz_i_end},

                                 {NULL, NULL}};

void vl_open_zlib(VL_State *S) { vl_register_lib(S, "z", zlibreg); }
