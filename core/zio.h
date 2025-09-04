// vitte-light/core/zio.h
// ZIO: utilitaires I/O bas-niveau portables.
// Lecteurs mémoire/fichier, écrivains buffer/fichier, primitives LE/BE,
// varints LEB128, lecture de lignes, hexdump, base64.
// Implémentation: core/zio.c

#ifndef VITTE_LIGHT_CORE_ZIO_H
#define VITTE_LIGHT_CORE_ZIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#if defined(_WIN32)
#if !defined(ssize_t)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#else
#include <sys/types.h>
#endif

#include "mem.h"  // VL_Buffer

// ───────────────────── Reader ─────────────────────
// Structure publique pour allocation sur pile. Les champs sont internes.
typedef struct VL_Reader {
  int kind;  // interne
  const uint8_t *m;
  size_t mn;  // mémoire
  size_t mi;
  FILE *fp;
  int own_fp;  // fichier
  uint8_t fbuf[8192];
  size_t bi, bn;
  size_t pos;
  int eof;
  int err;  // état
} VL_Reader;

void vl_r_init_mem(VL_Reader *r, const void *data, size_t n);
int vl_r_init_FILE(VL_Reader *r, FILE *fp);          // n'own pas FILE*
int vl_r_init_file(VL_Reader *r, const char *path);  // own le FILE*
void vl_r_close(VL_Reader *r);                       // ferme si own

size_t vl_r_pos(const VL_Reader *r);
int vl_r_eof(const VL_Reader *r);
int vl_r_error(const VL_Reader *r);

int vl_r_getc(VL_Reader *r);            // -1 si EOF/err
int vl_r_peek(VL_Reader *r, int *out);  // 1 si dispo
size_t vl_r_read(VL_Reader *r, void *dst, size_t n);
int vl_r_skip(VL_Reader *r, size_t n);

// Primitives LE/BE
int vl_r_u8(VL_Reader *r, uint8_t *v);
int vl_r_u16le(VL_Reader *r, uint16_t *v);
int vl_r_u32le(VL_Reader *r, uint32_t *v);
int vl_r_u64le(VL_Reader *r, uint64_t *v);
int vl_r_f64le(VL_Reader *r, double *v);
int vl_r_u16be(VL_Reader *r, uint16_t *v);
int vl_r_u32be(VL_Reader *r, uint32_t *v);
int vl_r_u64be(VL_Reader *r, uint64_t *v);

// Varint LEB128 non signé
int vl_r_varu64(VL_Reader *r, uint64_t *out);

// Lecture d'une ligne dans VL_Buffer. Retourne nb d'octets ajoutés ou -1.
ssize_t vl_r_read_line(VL_Reader *r, VL_Buffer *out, int keep_nl);

// Lecture intégrale d'un fichier en mémoire (malloc). 1 si ok.
int vl_read_file_all(const char *path, uint8_t **buf, size_t *n);

// ───────────────────── Writer ─────────────────────
// Structure publique pour allocation sur pile. Les champs sont internes.
typedef struct VL_Writer {
  int kind;
  VL_Buffer *buf;
  FILE *fp;
  int own_fp;
  int err;
  size_t count;
} VL_Writer;

void vl_w_init_buf(VL_Writer *w, VL_Buffer *buf);
int vl_w_init_FILE(VL_Writer *w, FILE *fp);          // n'own pas FILE*
int vl_w_init_file(VL_Writer *w, const char *path);  // own le FILE*
void vl_w_close(VL_Writer *w);                       // flush + close si own

int vl_w_error(const VL_Writer *w);
size_t vl_w_count(const VL_Writer *w);

size_t vl_w_write(VL_Writer *w, const void *src, size_t n);
int vl_w_putc(VL_Writer *w, int c);
int vl_w_printf(VL_Writer *w, const char *fmt, ...);
int vl_w_flush(VL_Writer *w);

// Primitives LE/BE
int vl_w_u8(VL_Writer *w, uint8_t v);
int vl_w_u16le(VL_Writer *w, uint16_t v);
int vl_w_u32le(VL_Writer *w, uint32_t v);
int vl_w_u64le(VL_Writer *w, uint64_t v);
int vl_w_f64le(VL_Writer *w, double v);
int vl_w_u16be(VL_Writer *w, uint16_t v);
int vl_w_u32be(VL_Writer *w, uint32_t v);
int vl_w_u64be(VL_Writer *w, uint64_t v);

// Varint LEB128 non signé
int vl_w_varu64(VL_Writer *w, uint64_t v);

// ───────────────────── Outils ─────────────────────
void vl_hexdump(const void *data, size_t n, size_t base_off, FILE *out);
int vl_base64_encode(const void *src, size_t n, VL_Writer *w);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_ZIO_H
