/* ============================================================================
   zio.h — I/O unifié et portable (C17)
   Flux supportés :
     - Fichiers (FILE*)
     - Mémoire read-only et read/write (buffer dynamique)
     - mappage mémoire read-only (mmap / CreateFileMapping)
     - (Optionnel) gzip via zlib si VT_ZIO_WITH_ZLIB est défini
   API : read / write / seek / tell / size / eof / error / flush / close,
         read_all, read_line, write_all, helpers endian (LE/BE).
   Préfixe : vt_zio_*
   Licence : MIT
   ============================================================================
 */
#ifndef VT_ZIO_H
#define VT_ZIO_H
#pragma once

#include <stddef.h> /* size_t, ptrdiff_t */
#include <stdint.h> /* uint8_t, uint16_t, uint32_t */
#include <stdio.h>  /* FILE*, SEEK_* */

/* --------------------------------------------------------------------------
   Export / C++
   -------------------------------------------------------------------------- */
#ifndef VT_ZIO_API
#define VT_ZIO_API extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
   ssize_t de secours (Windows sans POSIX)
   -------------------------------------------------------------------------- */
#if defined(_WIN32) && !defined(_SSIZE_T_DEFINED) && !defined(__SSIZE_T) && \
    !defined(__MINGW32__)
typedef ptrdiff_t ssize_t;
#define _SSIZE_T_DEFINED
#else
#include <sys/types.h> /* ssize_t (POSIX) */
#endif

/* Opaque handle */
typedef struct vt_zio vt_zio;

/* Kind (informative, pour introspection si besoin) */
typedef enum {
  VT_ZIO_KIND_UNKNOWN = 0,
  VT_ZIO_KIND_FILE = 1,
  VT_ZIO_KIND_MEM_RO = 2,
  VT_ZIO_KIND_MEM_RW = 3,
  VT_ZIO_KIND_MMAP_RO = 4,
  VT_ZIO_KIND_GZIP = 5
} vt_zio_kind;

/* --------------------------------------------------------------------------
   Constructeurs / Destructeur
   -------------------------------------------------------------------------- */

/* Ouvre un fichier en mode texte/binaire (ex: "rb", "wb"). */
VT_ZIO_API vt_zio* vt_zio_open_file(const char* path, const char* mode);

/* Wrappe un FILE* existant. take_ownership=1 => fclose à close(). */
VT_ZIO_API vt_zio* vt_zio_wrap_file(FILE* f, int take_ownership);

/* Crée un lecteur mémoire read-only sur (data,size). Caller garde la propriété.
 */
VT_ZIO_API vt_zio* vt_zio_from_ro_memory(const void* data, size_t size);

/* Crée un writer mémoire avec capacité initiale (peut être 0). */
VT_ZIO_API vt_zio* vt_zio_new_mem_writer(size_t initial_cap);

/* Transfert la propriété du buffer interne (writer mémoire). Met le zio à vide.
 */
VT_ZIO_API int vt_zio_mem_writer_take(vt_zio* z, void** out_ptr,
                                      size_t* out_size);

/* Mappage fichier read-only (ERREUR si non supporté). */
VT_ZIO_API vt_zio* vt_zio_open_mmap_rdonly(const char* path);

#ifdef VT_ZIO_WITH_ZLIB
/* Ouvre un .gz via zlib (mode "rb"/"wb"/"ab"). */
VT_ZIO_API vt_zio* vt_zio_open_gzip(const char* path, const char* mode);
#endif

/* Ferme et libère (flush si nécessaire). */
VT_ZIO_API void vt_zio_close(vt_zio* z);

/* --------------------------------------------------------------------------
   I/O de base
   -------------------------------------------------------------------------- */
VT_ZIO_API size_t vt_zio_read(vt_zio* z, void* buf, size_t n);
VT_ZIO_API size_t vt_zio_write(vt_zio* z, const void* buf, size_t n);
VT_ZIO_API int vt_zio_seek(vt_zio* z, int64_t off,
                           int whence); /* SEEK_SET/CUR/END */
VT_ZIO_API int64_t vt_zio_tell(vt_zio* z);
VT_ZIO_API int64_t vt_zio_size(vt_zio* z); /* -1 si inconnu/non supporté */
VT_ZIO_API int vt_zio_flush(vt_zio* z);    /* 0=OK, -1=err */
VT_ZIO_API int vt_zio_eof(vt_zio* z);      /* 1 si EOF (sticky), 0 sinon */
VT_ZIO_API int vt_zio_error(vt_zio* z);    /* 1 si erreur (sticky), 0 sinon */

/* Caractère par caractère + ungetc */
VT_ZIO_API int vt_zio_getc(vt_zio* z); /* EOF=-1 */
VT_ZIO_API int vt_zio_ungetc(vt_zio* z, int c);

/* --------------------------------------------------------------------------
   Aides haut-niveau
   -------------------------------------------------------------------------- */

/* Lit tout dans un buffer malloc() (caller free()). 0=OK, -1=err. */
VT_ZIO_API int vt_zio_read_all(vt_zio* z, void** out, size_t* out_len);

/* Lit une ligne (inclut '\n' si présent). Alloue/étend *line (realloc).
   Retour: longueur >=0, -1 si EOF sans lecture. */
VT_ZIO_API ssize_t vt_zio_read_line(vt_zio* z, char** line, size_t* cap);

/* Écrit tout (blocs) — 0=OK, -1=err. */
VT_ZIO_API int vt_zio_write_all(vt_zio* z, const void* data, size_t len);

/* --------------------------------------------------------------------------
   Helpers endian (inline) — lisent et décodent des entiers 8/16/32 bits
   Retour 0=OK, -1=EOF/erreur.
   -------------------------------------------------------------------------- */
static inline int vt_zio_read_u8(vt_zio* z, uint8_t* v) {
  return vt_zio_read(z, v, 1) == 1 ? 0 : -1;
}
static inline int vt_zio_read_le16(vt_zio* z, uint16_t* v) {
  uint8_t b[2];
  if (vt_zio_read(z, b, 2) != 2) return -1;
  *v = (uint16_t)(b[0] | (uint16_t)b[1] << 8);
  return 0;
}
static inline int vt_zio_read_le32(vt_zio* z, uint32_t* v) {
  uint8_t b[4];
  if (vt_zio_read(z, b, 4) != 4) return -1;
  *v = (uint32_t)(b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
                  (uint32_t)b[3] << 24);
  return 0;
}
static inline int vt_zio_read_be16(vt_zio* z, uint16_t* v) {
  uint8_t b[2];
  if (vt_zio_read(z, b, 2) != 2) return -1;
  *v = (uint16_t)((uint16_t)b[0] << 8 | b[1]);
  return 0;
}
static inline int vt_zio_read_be32(vt_zio* z, uint32_t* v) {
  uint8_t b[4];
  if (vt_zio_read(z, b, 4) != 4) return -1;
  *v = (uint32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
                  (uint32_t)b[2] << 8 | b[3]);
  return 0;
}

/* (optionnel) accès au genre si impl exposée (retourne VT_ZIO_KIND_UNKNOWN si
 * non dispo) */
VT_ZIO_API vt_zio_kind
vt_zio_get_kind(const vt_zio* z); /* implémentation facultative */

/* -------------------------------------------------------------------------- */
#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_ZIO_H */
