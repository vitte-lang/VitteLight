// vitte-light/core/zio.c
// ZIO = utilitaires I/O bas-niveau: lecteurs/écrivains mémoire/fichier,
// lecture/écriture primitives LE/BE, LEB128, lignes, hexdump, ISO erreurs.
// Zéro dépendance externe. Utilise VL_Buffer de mem.h quand utile.
//
// Header optionnel: demandez `zio.h` si vous voulez l’API publique.
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/zio.c

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define close_fd _close
#else
#include <unistd.h>
#define close_fd close
#endif

#include "mem.h"  // VL_Buffer

// ───────────────────────── Lecteur ─────────────────────────

enum { ZR_MEM = 1, ZR_FILE = 2 };

typedef struct VL_Reader {
  int kind;  // ZR_*
  // mémoire
  const uint8_t *m;
  size_t mn, mi;
  // fichier
  FILE *fp;
  int own_fp;
  uint8_t fbuf[8192];
  size_t bi, bn;
  // état
  size_t pos;
  int eof;
  int err;
} VL_Reader;

static void zr_reset(VL_Reader *r) { memset(r, 0, sizeof(*r)); }

void vl_r_init_mem(VL_Reader *r, const void *data, size_t n) {
  zr_reset(r);
  r->kind = ZR_MEM;
  r->m = (const uint8_t *)data;
  r->mn = n;
  r->mi = 0;
  r->pos = 0;
  r->eof = (n == 0);
}

int vl_r_init_FILE(VL_Reader *r, FILE *fp) {
  if (!r || !fp) return 0;
  zr_reset(r);
  r->kind = ZR_FILE;
  r->fp = fp;
  r->own_fp = 0;
  r->bi = r->bn = 0;
  r->pos = 0;
  r->eof = 0;
  r->err = 0;
  return 1;
}
int vl_r_init_file(VL_Reader *r, const char *path) {
  if (!r || !path) return 0;
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  int ok = vl_r_init_FILE(r, fp);
  r->own_fp = 1;
  if (!ok) {
    fclose(fp);
  }
  return ok;
}

void vl_r_close(VL_Reader *r) {
  if (!r) return;
  if (r->kind == ZR_FILE && r->own_fp && r->fp) {
    fclose(r->fp);
  }
  zr_reset(r);
}

static int zr_fill(VL_Reader *r) {
  if (r->kind != ZR_FILE || !r->fp) return 0;
  size_t rd = fread(r->fbuf, 1, sizeof(r->fbuf), r->fp);
  r->bi = 0;
  r->bn = rd;
  if (rd == 0) {
    if (feof(r->fp)) r->eof = 1;
    if (ferror(r->fp)) r->err = 1;
    return 0;
  }
  return 1;
}

size_t vl_r_pos(const VL_Reader *r) { return r ? r->pos : 0; }
int vl_r_eof(const VL_Reader *r) { return r ? r->eof : 1; }
int vl_r_error(const VL_Reader *r) { return r ? r->err : 1; }

static int zr_take_byte(VL_Reader *r) {
  if (r->kind == ZR_MEM) {
    if (r->mi >= r->mn) {
      r->eof = 1;
      return -1;
    }
    r->pos++;
    return (int)r->m[r->mi++];
  }
  // file
  if (r->bi >= r->bn) {
    if (!zr_fill(r)) {
      return -1;
    }
  }
  r->pos++;
  return (int)r->fbuf[r->bi++];
}

int vl_r_getc(VL_Reader *r) {
  if (!r) return -1;
  return zr_take_byte(r);
}

int vl_r_peek(VL_Reader *r, int *out) {
  if (!r) return 0;
  if (r->kind == ZR_MEM) {
    if (r->mi >= r->mn) {
      r->eof = 1;
      return 0;
    }
    if (out) *out = r->m[r->mi];
    return 1;
  }
  if (r->bi >= r->bn) {
    if (!zr_fill(r)) return 0;
  }
  if (out) *out = r->fbuf[r->bi];
  return 1;
}

size_t vl_r_read(VL_Reader *r, void *dst, size_t n) {
  if (!r || !dst || n == 0) return 0;
  size_t done = 0;
  uint8_t *p = (uint8_t *)dst;
  if (r->kind == ZR_MEM) {
    size_t rem = r->mn - r->mi;
    if (n > rem) n = rem;
    memcpy(p, r->m + r->mi, n);
    r->mi += n;
    r->pos += n;
    if (r->mi >= r->mn) r->eof = 1;
    return n;
  }
  while (done < n) {
    if (r->bi >= r->bn) {
      if (!zr_fill(r)) break;
    }
    size_t chunk = r->bn - r->bi;
    if (chunk > n - done) chunk = n - done;
    memcpy(p + done, r->fbuf + r->bi, chunk);
    r->bi += chunk;
    done += chunk;
    r->pos += chunk;
  }
  if (done == 0 && feof(r->fp)) r->eof = 1;
  if (ferror(r->fp)) r->err = 1;
  return done;
}

int vl_r_skip(VL_Reader *r, size_t n) {
  if (!r) return 0;
  uint8_t tmp[256];
  while (n) {
    size_t ch = n > sizeof(tmp) ? sizeof(tmp) : n;
    size_t rd = vl_r_read(r, tmp, ch);
    if (rd == 0) return 0;
    n -= rd;
  }
  return 1;
}

// ─────────── Primitives LE/BE et LEB128 ───────────
int vl_r_u8(VL_Reader *r, uint8_t *v) {
  int c = vl_r_getc(r);
  if (c < 0) return 0;
  *v = (uint8_t)c;
  return 1;
}
int vl_r_u16le(VL_Reader *r, uint16_t *v) {
  uint8_t b[2];
  if (vl_r_read(r, b, 2) != 2) return 0;
  *v = (uint16_t)(b[0] | (b[1] << 8));
  return 1;
}
int vl_r_u32le(VL_Reader *r, uint32_t *v) {
  uint8_t b[4];
  if (vl_r_read(r, b, 4) != 4) return 0;
  *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
       ((uint32_t)b[3] << 24);
  return 1;
}
int vl_r_u64le(VL_Reader *r, uint64_t *v) {
  uint8_t b[8];
  if (vl_r_read(r, b, 8) != 8) return 0;
  uint64_t x = 0;
  for (int i = 0; i < 8; i++) x |= ((uint64_t)b[i]) << (8 * i);
  *v = x;
  return 1;
}
int vl_r_f64le(VL_Reader *r, double *d) {
  uint64_t u = 0;
  if (!vl_r_u64le(r, &u)) return 0;
  union {
    uint64_t u;
    double d;
  } u64;
  u64.u = u;
  *d = u64.d;
  return 1;
}
int vl_r_u16be(VL_Reader *r, uint16_t *v) {
  uint8_t b[2];
  if (vl_r_read(r, b, 2) != 2) return 0;
  *v = (uint16_t)((b[0] << 8) | b[1]);
  return 1;
}
int vl_r_u32be(VL_Reader *r, uint32_t *v) {
  uint8_t b[4];
  if (vl_r_read(r, b, 4) != 4) return 0;
  *v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) |
       ((uint32_t)b[3]);
  return 1;
}
int vl_r_u64be(VL_Reader *r, uint64_t *v) {
  uint8_t b[8];
  if (vl_r_read(r, b, 8) != 8) return 0;
  uint64_t x = 0;
  for (int i = 0; i < 8; i++) x = (x << 8) | b[i];
  *v = x;
  return 1;
}

int vl_r_varu64(VL_Reader *r, uint64_t *out) {  // LEB128 non signé
  uint64_t val = 0;
  int shift = 0;
  for (int i = 0; i < 10; i++) {
    int c = vl_r_getc(r);
    if (c < 0) return 0;
    val |= ((uint64_t)(c & 0x7F)) << shift;
    if (!(c & 0x80)) {
      *out = val;
      return 1;
    }
    shift += 7;
  }
  return 0;  // trop long
}

// ─────────── Lignes / lecture intégrale ───────────
ssize_t vl_r_read_line(VL_Reader *r, VL_Buffer *out, int keep_nl) {
  if (!r || !out) return -1;
  size_t start = out->n;
  for (;;) {
    int ch;
    if (!vl_r_peek(r, &ch)) break;
    if (ch == '\n') {
      (void)vl_r_getc(r);
      if (keep_nl) vl_buf_putc(out, '\n');
      break;
    }
    (void)vl_r_getc(r);
    vl_buf_putc(out, ch);
  }
  return (ssize_t)(out->n - start);
}

int vl_read_file_all(const char *path, uint8_t **buf, size_t *n) {
  if (!path || !buf || !n) return 0;
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

// ───────────────────────── Écrivain ─────────────────────────

enum { ZW_BUF = 1, ZW_FILE = 2 };

typedef struct VL_Writer {
  int kind;
  VL_Buffer *buf;
  FILE *fp;
  int own_fp;
  int err;
  size_t count;
} VL_Writer;

static void zw_reset(VL_Writer *w) { memset(w, 0, sizeof(*w)); }

void vl_w_init_buf(VL_Writer *w, VL_Buffer *buf) {
  zw_reset(w);
  w->kind = ZW_BUF;
  w->buf = buf;
}
int vl_w_init_FILE(VL_Writer *w, FILE *fp) {
  if (!w || !fp) return 0;
  zw_reset(w);
  w->kind = ZW_FILE;
  w->fp = fp;
  w->own_fp = 0;
  return 1;
}
int vl_w_init_file(VL_Writer *w, const char *path) {
  if (!w || !path) return 0;
  FILE *fp = fopen(path, "wb");
  if (!fp) return 0;
  int ok = vl_w_init_FILE(w, fp);
  w->own_fp = 1;
  if (!ok) {
    fclose(fp);
  }
  return ok;
}
void vl_w_close(VL_Writer *w) {
  if (!w) return;
  if (w->kind == ZW_FILE && w->own_fp && w->fp) {
    fflush(w->fp);
    fclose(w->fp);
  }
  zw_reset(w);
}

int vl_w_error(const VL_Writer *w) { return w ? w->err : 1; }
size_t vl_w_count(const VL_Writer *w) { return w ? w->count : 0; }

size_t vl_w_write(VL_Writer *w, const void *src, size_t n) {
  if (!w || !src || n == 0) return 0;
  if (w->kind == ZW_BUF) {
    vl_buf_append(w->buf, src, n);
    w->count += n;
    return n;
  }
  size_t wr = fwrite(src, 1, n, w->fp);
  if (wr != n) w->err = 1;
  w->count += wr;
  return wr;
}
int vl_w_putc(VL_Writer *w, int c) {
  unsigned char ch = (unsigned char)c;
  return (vl_w_write(w, &ch, 1) == 1) ? 1 : 0;
}
int vl_w_printf(VL_Writer *w, const char *fmt, ...) {
  char tmp[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0) return 0;
  if ((size_t)n < sizeof(tmp))
    return vl_w_write(w, tmp, (size_t)n) == (size_t)n;  // long path
  char *dyn = (char *)malloc((size_t)n + 1);
  if (!dyn) return 0;
  va_start(ap, fmt);
  vsnprintf(dyn, (size_t)n + 1, fmt, ap);
  va_end(ap);
  int ok = (vl_w_write(w, dyn, (size_t)n) == (size_t)n);
  free(dyn);
  return ok;
}
int vl_w_flush(VL_Writer *w) {
  if (!w) return 0;
  if (w->kind == ZW_FILE) {
    return fflush(w->fp) == 0;
  }
  return 1;
}

// u8..f64 LE/BE
int vl_w_u8(VL_Writer *w, uint8_t v) { return vl_w_write(w, &v, 1) == 1; }
int vl_w_u16le(VL_Writer *w, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  return vl_w_write(w, b, 2) == 2;
}
int vl_w_u32le(VL_Writer *w, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  return vl_w_write(w, b, 4) == 4;
}
int vl_w_u64le(VL_Writer *w, uint64_t v) {
  uint8_t b[8];
  for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
  return vl_w_write(w, b, 8) == 8;
}
int vl_w_f64le(VL_Writer *w, double d) {
  union {
    uint64_t u;
    double d;
  } u;
  u.d = d;
  return vl_w_u64le(w, u.u);
}
int vl_w_u16be(VL_Writer *w, uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
  return vl_w_write(w, b, 2) == 2;
}
int vl_w_u32be(VL_Writer *w, uint32_t v) {
  uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                  (uint8_t)v};
  return vl_w_write(w, b, 4) == 4;
}
int vl_w_u64be(VL_Writer *w, uint64_t v) {
  uint8_t b[8];
  for (int i = 0; i < 8; i++) b[7 - i] = (uint8_t)(v >> (8 * i));
  return vl_w_write(w, b, 8) == 8;
}

int vl_w_varu64(VL_Writer *w, uint64_t v) {  // LEB128 non signé
  while (v >= 0x80) {
    uint8_t b = (uint8_t)((v & 0x7F) | 0x80);
    if (!vl_w_u8(w, b)) return 0;
    v >>= 7;
  }
  return vl_w_u8(w, (uint8_t)v);
}

// ───────────────────────── Hexdump ─────────────────────────
void vl_hexdump(const void *data, size_t n, size_t base_off, FILE *out) {
  if (!out) out = stdout;
  const uint8_t *p = (const uint8_t *)data;
  size_t i = 0;
  char ascii[17];
  ascii[16] = '\0';
  while (i < n) {
    size_t line = i;
    fprintf(out, "%08zx  ", base_off + line);
    for (int j = 0; j < 16; j++) {
      if (i + j < n)
        fprintf(out, "%02X ", p[i + j]);
      else
        fputs("   ", out);
      if (j == 7) fputc(' ', out);
    }
    fputc(' ', out);
    for (int j = 0; j < 16; j++) {
      unsigned char c = (i + j < n) ? p[i + j] : ' ';
      ascii[j] = (c >= 32 && c < 127) ? c : '.';
    }
    fprintf(out, "%s\n", ascii);
    i += 16;
  }
}

// ───────────────────────── Base64 (encode) ─────────────────────────
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int vl_base64_encode(const void *src, size_t n, VL_Writer *w) {
  const uint8_t *p = (const uint8_t *)src;
  size_t i = 0;
  while (i + 3 <= n) {
    uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
    if (!vl_w_putc(w, B64[(v >> 18) & 63])) return 0;
    if (!vl_w_putc(w, B64[(v >> 12) & 63])) return 0;
    if (!vl_w_putc(w, B64[(v >> 6) & 63])) return 0;
    if (!vl_w_putc(w, B64[v & 63])) return 0;
    i += 3;
  }
  if (i < n) {
    uint32_t v = 0;
    int pad = 0;
    v |= p[i] << 16;
    if (i + 1 < n) {
      v |= p[i + 1] << 8;
    } else {
      pad++;
    }
    if (i + 2 < n) {
      v |= p[i + 2];
    } else {
      pad++;
    }
    if (!vl_w_putc(w, B64[(v >> 18) & 63])) return 0;
    if (!vl_w_putc(w, B64[(v >> 12) & 63])) return 0;
    if (pad == 2) {
      if (!vl_w_putc(w, '=')) return 0;
      if (!vl_w_putc(w, '=')) return 0;
    } else if (pad == 1) {
      if (!vl_w_putc(w, B64[(v >> 6) & 63])) return 0;
      if (!vl_w_putc(w, '=')) return 0;
    } else {
      if (!vl_w_putc(w, B64[(v >> 6) & 63])) return 0;
      if (!vl_w_putc(w, B64[v & 63])) return 0;
    }
  }
  return 1;
}

// ───────────────────────── Mini tests ─────────────────────────
#ifdef VL_ZIO_TEST_MAIN
int main(void) {
  // writer buffer
  VL_Buffer b;
  vl_buf_init(&b);
  VL_Writer w;
  vl_w_init_buf(&w, &b);
  vl_w_printf(&w, "hello %d\n", 42);
  vl_w_u32le(&w, 0x11223344u);
  vl_w_varu64(&w, 300);
  vl_w_putc(&w, '\n');
  // hexdump
  vl_hexdump(b.d, b.n, 0, stdout);
  // reader mem
  VL_Reader r;
  vl_r_init_mem(&r, b.d, b.n);
  uint8_t tmp[6];
  vl_r_read(&r, tmp, 6);
  printf("peek:%d pos:%zu eof:%d\n", (int)tmp[0], vl_r_pos(&r), vl_r_eof(&r));
  vl_r_close(&r);
  vl_w_close(&w);
  vl_buf_free(&b);
  return 0;
}
#endif
