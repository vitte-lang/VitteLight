/* ============================================================================
   zio.c — I/O unifié et portable (C17)
   - Flux fichier (FILE*), mémoire (RO/RW), mmap RO (POSIX/Windows)
   - Optionnel: gzip via zlib (définir VT_ZIO_WITH_ZLIB)
   - API de haut niveau: read/write/seek/tell/size/eof/flush/close, read_all,
   read_line, helpers endian (LE/BE), memory-writer avec extraction du buffer.
   - Préfixe: vt_zio_*
   Licence: MIT.
   ============================================================================
 */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef VT_ZIO_WITH_ZLIB
#include <zlib.h>
#endif

/* --------------------------------------------------------------------------
   Vtable / base
   -------------------------------------------------------------------------- */
typedef struct vt_zio vt_zio;

typedef struct vt_zio_vtbl {
  size_t (*read)(vt_zio*, void*, size_t);
  size_t (*write)(vt_zio*, const void*, size_t);
  int (*seek)(vt_zio*, int64_t, int);
  int64_t (*tell)(vt_zio*);
  int64_t (*size)(vt_zio*);
  int (*flush)(vt_zio*);
  int (*eof)(vt_zio*);
  int (*error)(vt_zio*);
  void (*close)(vt_zio*);
  int (*getc)(vt_zio*);
  int (*ungetc)(vt_zio*, int);
} vt_zio_vtbl;

enum {
  VT_ZIO_FILE = 1,
  VT_ZIO_MEM_RO = 2,
  VT_ZIO_MEM_RW = 3,
  VT_ZIO_MMAP = 4,
  VT_ZIO_GZIP = 5
};

struct vt_zio {
  const vt_zio_vtbl* v;
  int kind;
  int own;      /* possède la ressource sous-jacente */
  int err;      /* sticky error */
  int eof_flag; /* sticky eof */
  union {
    struct {
      FILE* f;
    } file;
    struct {
      const uint8_t* p;
      size_t n;
      size_t pos;
      int ungot;
    } mem_ro;
    struct {
      uint8_t* p;
      size_t n;
      size_t cap;
      size_t pos;
      int ungot;
      int owner;
    } mem_rw;
    struct {
#if defined(_WIN32)
      HANDLE hFile, hMap;
#else
      int fd;
#endif
      const uint8_t* p;
      size_t n;
      size_t pos;
      int ungot;
    } mmap;
#ifdef VT_ZIO_WITH_ZLIB
    struct {
      gzFile gz;
    } gz;
#endif
  } u;
};

/* --------------------------------------------------------------------------
   Helpers portables
   -------------------------------------------------------------------------- */
static int vt__fseek64(FILE* f, int64_t off, int whence) {
#if defined(_WIN32)
  return _fseeki64(f, off, whence);
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
  return fseeko(f, (off_t)off, whence);
#else
  /* Fallback non-64, peut échouer pour >2Go */
  if (off > LONG_MAX || off < LONG_MIN) {
    errno = EOVERFLOW;
    return -1;
  }
  return fseek(f, (long)off, whence);
#endif
}
static int64_t vt__ftell64(FILE* f) {
#if defined(_WIN32)
  return _ftelli64(f);
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
  off_t t = ftello(f);
  return t < 0 ? -1 : (int64_t)t;
#else
  long t = ftell(f);
  return t < 0 ? -1 : (int64_t)t;
#endif
}

/* --------------------------------------------------------------------------
   Protos publics minimaux (pour compile sans zio.h)
   -------------------------------------------------------------------------- */
vt_zio* vt_zio_open_file(const char* path, const char* mode);
vt_zio* vt_zio_wrap_file(FILE* f, int take_ownership);
vt_zio* vt_zio_from_ro_memory(const void* data, size_t size);
vt_zio* vt_zio_new_mem_writer(size_t initial_cap);
int vt_zio_mem_writer_take(vt_zio* z, void** out_ptr,
                           size_t* out_size); /* transfert propriété */
vt_zio* vt_zio_open_mmap_rdonly(const char* path);
#ifdef VT_ZIO_WITH_ZLIB
vt_zio* vt_zio_open_gzip(const char* path, const char* mode);
#endif

size_t vt_zio_read(vt_zio* z, void* buf, size_t n);
size_t vt_zio_write(vt_zio* z, const void* buf, size_t n);
int vt_zio_seek(vt_zio* z, int64_t off, int whence);
int64_t vt_zio_tell(vt_zio* z);
int64_t vt_zio_size(vt_zio* z);
int vt_zio_flush(vt_zio* z);
int vt_zio_eof(vt_zio* z);
int vt_zio_error(vt_zio* z);
void vt_zio_close(vt_zio* z);
int vt_zio_getc(vt_zio* z);
int vt_zio_ungetc(vt_zio* z, int c);

/* High-level */
int vt_zio_read_all(vt_zio* z, void** out, size_t* out_len); /* malloc */
ssize_t vt_zio_read_line(vt_zio* z, char** line,
                         size_t* cap); /* \n gardé, -1=eof */
int vt_zio_write_all(vt_zio* z, const void* data, size_t len);

/* Endian helpers */
static int vt_zio_read_u8(vt_zio* z, uint8_t* v) {
  return vt_zio_read(z, v, 1) == 1 ? 0 : -1;
}
static int vt_zio_read_le16(vt_zio* z, uint16_t* v) {
  uint8_t b[2];
  if (vt_zio_read(z, b, 2) != 2) return -1;
  *v = (uint16_t)(b[0] | (b[1] << 8));
  return 0;
}
static int vt_zio_read_le32(vt_zio* z, uint32_t* v) {
  uint8_t b[4];
  if (vt_zio_read(z, b, 4) != 4) return -1;
  *v = (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24));
  return 0;
}
static int vt_zio_read_be16(vt_zio* z, uint16_t* v) {
  uint8_t b[2];
  if (vt_zio_read(z, b, 2) != 2) return -1;
  *v = (uint16_t)((b[0] << 8) | b[1]);
  return 0;
}
static int vt_zio_read_be32(vt_zio* z, uint32_t* v) {
  uint8_t b[4];
  if (vt_zio_read(z, b, 4) != 4) return -1;
  *v = (uint32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
  return 0;
}

/* --------------------------------------------------------------------------
   FILE* backend
   -------------------------------------------------------------------------- */
static size_t vt__file_read(vt_zio* z, void* buf, size_t n) {
  size_t r = fread(buf, 1, n, z->u.file.f);
  if (r < n) {
    if (feof(z->u.file.f)) z->eof_flag = 1;
    if (ferror(z->u.file.f)) z->err = 1;
  }
  return r;
}
static size_t vt__file_write(vt_zio* z, const void* buf, size_t n) {
  size_t w = fwrite(buf, 1, n, z->u.file.f);
  if (w < n) z->err = 1;
  return w;
}
static int vt__file_seek(vt_zio* z, int64_t off, int whence) {
  int rc = vt__fseek64(z->u.file.f, off, whence);
  if (rc != 0) z->err = 1;
  if (rc == 0) z->eof_flag = 0;
  return rc;
}
static int64_t vt__file_tell(vt_zio* z) { return vt__ftell64(z->u.file.f); }
static int64_t vt__file_size(vt_zio* z) {
  int64_t cur = vt__ftell64(z->u.file.f);
  if (cur < 0) return -1;
  if (vt__fseek64(z->u.file.f, 0, SEEK_END) != 0) return -1;
  int64_t end = vt__ftell64(z->u.file.f);
  vt__fseek64(z->u.file.f, cur, SEEK_SET);
  return end;
}
static int vt__file_flush(vt_zio* z) { return fflush(z->u.file.f); }
static int vt__file_eof(vt_zio* z) { return feof(z->u.file.f) || z->eof_flag; }
static int vt__file_error(vt_zio* z) { return ferror(z->u.file.f) || z->err; }
static void vt__file_close(vt_zio* z) {
  if (z->own && z->u.file.f) fclose(z->u.file.f);
}
static int vt__file_getc(vt_zio* z) {
  int c = fgetc(z->u.file.f);
  if (c == EOF) {
    if (feof(z->u.file.f)) z->eof_flag = 1;
    if (ferror(z->u.file.f)) z->err = 1;
  }
  return c;
}
static int vt__file_ungetc(vt_zio* z, int c) {
  return ungetc(c, z->u.file.f) == EOF ? -1 : c;
}

static const vt_zio_vtbl VT__FILE_VTBL = {
    vt__file_read,  vt__file_write, vt__file_seek,  vt__file_tell,
    vt__file_size,  vt__file_flush, vt__file_eof,   vt__file_error,
    vt__file_close, vt__file_getc,  vt__file_ungetc};

/* --------------------------------------------------------------------------
   Memory RO backend
   -------------------------------------------------------------------------- */
static size_t vt__mro_read(vt_zio* z, void* buf, size_t n) {
  size_t left = z->u.mem_ro.ungot != -1 ? 1 : 0;
  uint8_t tmp;
  uint8_t* out = (uint8_t*)buf;
  size_t done = 0;

  if (left) {
    out[done++] = (uint8_t)z->u.mem_ro.ungot;
    z->u.mem_ro.ungot = -1;
  }

  size_t avail =
      (z->u.mem_ro.pos < z->u.mem_ro.n) ? (z->u.mem_ro.n - z->u.mem_ro.pos) : 0;
  size_t take = (n - done) < avail ? (n - done) : avail;
  if (take) {
    memcpy(out + done, z->u.mem_ro.p + z->u.mem_ro.pos, take);
    z->u.mem_ro.pos += take;
    done += take;
  }
  if (done < n) z->eof_flag = 1;
  (void)tmp;
  return done;
}
static size_t vt__mro_write(vt_zio* z, const void* buf, size_t n) {
  (void)z;
  (void)buf;
  (void)n;
  errno = EBADF;
  z->err = 1;
  return 0;
}
static int vt__mro_seek(vt_zio* z, int64_t off, int whence) {
  size_t base;
  if (whence == SEEK_SET)
    base = 0;
  else if (whence == SEEK_CUR)
    base = z->u.mem_ro.pos;
  else if (whence == SEEK_END)
    base = z->u.mem_ro.n;
  else {
    errno = EINVAL;
    return -1;
  }
  if (off < 0 && (size_t)(-off) > base) {
    errno = EINVAL;
    return -1;
  }
  size_t np = base + (size_t)off;
  if (np > z->u.mem_ro.n) {
    errno = EINVAL;
    return -1;
  }
  z->u.mem_ro.pos = np;
  z->u.mem_ro.ungot = -1;
  z->eof_flag = 0;
  return 0;
}
static int64_t vt__mro_tell(vt_zio* z) { return (int64_t)z->u.mem_ro.pos; }
static int64_t vt__mro_size(vt_zio* z) { return (int64_t)z->u.mem_ro.n; }
static int vt__mro_flush(vt_zio* z) {
  (void)z;
  return 0;
}
static int vt__mro_eof(vt_zio* z) {
  return z->eof_flag || z->u.mem_ro.pos >= z->u.mem_ro.n;
}
static int vt__mro_error(vt_zio* z) { return z->err; }
static void vt__mro_close(vt_zio* z) { /* rien */ }
static int vt__mro_getc(vt_zio* z) {
  if (z->u.mem_ro.ungot != -1) {
    int c = z->u.mem_ro.ungot;
    z->u.mem_ro.ungot = -1;
    return c;
  }
  if (z->u.mem_ro.pos >= z->u.mem_ro.n) {
    z->eof_flag = 1;
    return EOF;
  }
  return z->u.mem_ro.p[z->u.mem_ro.pos++];
}
static int vt__mro_ungetc(vt_zio* z, int c) {
  if (z->u.mem_ro.ungot != -1) return -1;
  z->u.mem_ro.ungot = (uint8_t)c;
  return c;
}
static const vt_zio_vtbl VT__MRO_VTBL = {
    vt__mro_read,  vt__mro_write, vt__mro_seek,  vt__mro_tell,
    vt__mro_size,  vt__mro_flush, vt__mro_eof,   vt__mro_error,
    vt__mro_close, vt__mro_getc,  vt__mro_ungetc};

/* --------------------------------------------------------------------------
   Memory RW backend (dynamic buffer)
   -------------------------------------------------------------------------- */
static int vt__grow(uint8_t** p, size_t* cap, size_t need) {
  if (*cap >= need) return 0;
  size_t ncap = *cap ? *cap : 256;
  while (ncap < need) ncap = ncap + ncap / 2 + 1;
  void* np = realloc(*p, ncap);
  if (!np) return -1;
  *p = (uint8_t*)np;
  *cap = ncap;
  return 0;
}
static size_t vt__mrw_read(vt_zio* z, void* buf, size_t n) {
  size_t pre = 0;
  if (z->u.mem_rw.ungot != -1) {
    ((uint8_t*)buf)[0] = (uint8_t)z->u.mem_rw.ungot;
    z->u.mem_rw.ungot = -1;
    pre = 1;
  }
  size_t avail =
      (z->u.mem_rw.pos < z->u.mem_rw.n) ? (z->u.mem_rw.n - z->u.mem_rw.pos) : 0;
  size_t take = (n - pre) < avail ? (n - pre) : avail;
  if (take) {
    memcpy((uint8_t*)buf + pre, z->u.mem_rw.p + z->u.mem_rw.pos, take);
    z->u.mem_rw.pos += take;
  }
  if (pre + take < n) z->eof_flag = 1;
  return pre + take;
}
static size_t vt__mrw_write(vt_zio* z, const void* buf, size_t n) {
  size_t need_pos = z->u.mem_rw.pos + n;
  if (vt__grow(&z->u.mem_rw.p, &z->u.mem_rw.cap, need_pos) != 0) {
    z->err = 1;
    return 0;
  }
  memcpy(z->u.mem_rw.p + z->u.mem_rw.pos, buf, n);
  z->u.mem_rw.pos += n;
  if (z->u.mem_rw.pos > z->u.mem_rw.n) z->u.mem_rw.n = z->u.mem_rw.pos;
  return n;
}
static int vt__mrw_seek(vt_zio* z, int64_t off, int whence) {
  size_t base;
  if (whence == SEEK_SET)
    base = 0;
  else if (whence == SEEK_CUR)
    base = z->u.mem_rw.pos;
  else if (whence == SEEK_END)
    base = z->u.mem_rw.n;
  else {
    errno = EINVAL;
    return -1;
  }
  if (off < 0 && (size_t)(-off) > base) {
    errno = EINVAL;
    return -1;
  }
  size_t np = base + (size_t)off;
  /* autoriser seek au-delà et étendre à l'écriture */
  z->u.mem_rw.pos = np;
  if (np > z->u.mem_rw.n) {
    if (vt__grow(&z->u.mem_rw.p, &z->u.mem_rw.cap, np) != 0) {
      z->err = 1;
      return -1;
    }
    /* zero-fill la zone intermédiaire */
    memset(z->u.mem_rw.p + z->u.mem_rw.n, 0, np - z->u.mem_rw.n);
    z->u.mem_rw.n = np;
  }
  z->u.mem_rw.ungot = -1;
  z->eof_flag = 0;
  return 0;
}
static int64_t vt__mrw_tell(vt_zio* z) { return (int64_t)z->u.mem_rw.pos; }
static int64_t vt__mrw_size(vt_zio* z) { return (int64_t)z->u.mem_rw.n; }
static int vt__mrw_flush(vt_zio* z) {
  (void)z;
  return 0;
}
static int vt__mrw_eof(vt_zio* z) {
  return z->eof_flag || z->u.mem_rw.pos >= z->u.mem_rw.n;
}
static int vt__mrw_error(vt_zio* z) { return z->err; }
static void vt__mrw_close(vt_zio* z) {
  if (z->u.mem_rw.owner && z->u.mem_rw.p) free(z->u.mem_rw.p);
}
static int vt__mrw_getc(vt_zio* z) {
  if (z->u.mem_rw.ungot != -1) {
    int c = z->u.mem_rw.ungot;
    z->u.mem_rw.ungot = -1;
    return c;
  }
  if (z->u.mem_rw.pos >= z->u.mem_rw.n) {
    z->eof_flag = 1;
    return EOF;
  }
  return z->u.mem_rw.p[z->u.mem_rw.pos++];
}
static int vt__mrw_ungetc(vt_zio* z, int c) {
  if (z->u.mem_rw.ungot != -1) return -1;
  z->u.mem_rw.ungot = (uint8_t)c;
  return c;
}
static const vt_zio_vtbl VT__MRW_VTBL = {
    vt__mrw_read,  vt__mrw_write, vt__mrw_seek,  vt__mrw_tell,
    vt__mrw_size,  vt__mrw_flush, vt__mrw_eof,   vt__mrw_error,
    vt__mrw_close, vt__mrw_getc,  vt__mrw_ungetc};

/* --------------------------------------------------------------------------
   MMAP RO backend
   -------------------------------------------------------------------------- */
static size_t vt__mmap_read(vt_zio* z, void* buf, size_t n) {
  size_t pre = 0;
  if (z->u.mmap.ungot != -1) {
    ((uint8_t*)buf)[0] = (uint8_t)z->u.mmap.ungot;
    z->u.mmap.ungot = -1;
    pre = 1;
  }
  size_t avail =
      (z->u.mmap.pos < z->u.mmap.n) ? (z->u.mmap.n - z->u.mmap.pos) : 0;
  size_t take = (n - pre) < avail ? (n - pre) : avail;
  if (take) {
    memcpy((uint8_t*)buf + pre, z->u.mmap.p + z->u.mmap.pos, take);
    z->u.mmap.pos += take;
  }
  if (pre + take < n) z->eof_flag = 1;
  return pre + take;
}
static size_t vt__mmap_write(vt_zio* z, const void* b, size_t n) {
  (void)z;
  (void)b;
  (void)n;
  errno = EBADF;
  z->err = 1;
  return 0;
}
static int vt__mmap_seek(vt_zio* z, int64_t off, int whence) {
  size_t base;
  if (whence == SEEK_SET)
    base = 0;
  else if (whence == SEEK_CUR)
    base = z->u.mmap.pos;
  else if (whence == SEEK_END)
    base = z->u.mmap.n;
  else {
    errno = EINVAL;
    return -1;
  }
  if (off < 0 && (size_t)(-off) > base) {
    errno = EINVAL;
    return -1;
  }
  size_t np = base + (size_t)off;
  if (np > z->u.mmap.n) {
    errno = EINVAL;
    return -1;
  }
  z->u.mmap.pos = np;
  z->u.mmap.ungot = -1;
  z->eof_flag = 0;
  return 0;
}
static int64_t vt__mmap_tell(vt_zio* z) { return (int64_t)z->u.mmap.pos; }
static int64_t vt__mmap_size(vt_zio* z) { return (int64_t)z->u.mmap.n; }
static int vt__mmap_flush(vt_zio* z) {
  (void)z;
  return 0;
}
static int vt__mmap_eof(vt_zio* z) {
  return z->eof_flag || z->u.mmap.pos >= z->u.mmap.n;
}
static int vt__mmap_error(vt_zio* z) { return z->err; }
static void vt__mmap_close(vt_zio* z) {
#if defined(_WIN32)
  if (z->u.mmap.p) UnmapViewOfFile((void*)z->u.mmap.p);
  if (z->u.mmap.hMap) CloseHandle(z->u.mmap.hMap);
  if (z->u.mmap.hFile) CloseHandle(z->u.mmap.hFile);
#else
  if (z->u.mmap.p) munmap((void*)z->u.mmap.p, z->u.mmap.n);
  if (z->u.mmap.fd >= 0) close(z->u.mmap.fd);
#endif
}
static int vt__mmap_getc(vt_zio* z) {
  if (z->u.mmap.ungot != -1) {
    int c = z->u.mmap.ungot;
    z->u.mmap.ungot = -1;
    return c;
  }
  if (z->u.mmap.pos >= z->u.mmap.n) {
    z->eof_flag = 1;
    return EOF;
  }
  return z->u.mmap.p[z->u.mmap.pos++];
}
static int vt__mmap_ungetc(vt_zio* z, int c) {
  if (z->u.mmap.ungot != -1) return -1;
  z->u.mmap.ungot = (uint8_t)c;
  return c;
}
static const vt_zio_vtbl VT__MMAP_VTBL = {
    vt__mmap_read,  vt__mmap_write, vt__mmap_seek,  vt__mmap_tell,
    vt__mmap_size,  vt__mmap_flush, vt__mmap_eof,   vt__mmap_error,
    vt__mmap_close, vt__mmap_getc,  vt__mmap_ungetc};

/* --------------------------------------------------------------------------
   GZIP backend (zlib)
   -------------------------------------------------------------------------- */
#ifdef VT_ZIO_WITH_ZLIB
static size_t vt__gz_read(vt_zio* z, void* buf, size_t n) {
  int r = gzread(z->u.gz.gz, buf, (unsigned)n);
  if (r < 0) {
    z->err = 1;
    return 0;
  }
  if ((size_t)r < n) z->eof_flag = 1;
  return (size_t)r;
}
static size_t vt__gz_write(vt_zio* z, const void* buf, size_t n) {
  int w = gzwrite(z->u.gz.gz, buf, (unsigned)n);
  if (w == 0) {
    z->err = 1;
    return 0;
  }
  return (size_t)w;
}
static int vt__gz_seek(vt_zio* z, int64_t off, int whence) {
#if ZLIB_VERNUM >= 0x1250 /* gzseek 64-bit ok */
  z_off_t pos = gzseek(z->u.gz.gz, (z_off_t)off, whence);
  if (pos < 0) {
    z->err = 1;
    return -1;
  }
  z->eof_flag = 0;
  return 0;
#else
  (void)z;
  (void)off;
  (void)whence;
  errno = ENOTSUP;
  return -1;
#endif
}
static int64_t vt__gz_tell(vt_zio* z) {
  z_off_t p = gztell(z->u.gz.gz);
  return p < 0 ? -1 : (int64_t)p;
}
static int64_t vt__gz_size(vt_zio* z) {
  (void)z;
  errno = ENOTSUP;
  return -1;
}
static int vt__gz_flush(vt_zio* z) {
  return gzflush(z->u.gz.gz, Z_SYNC_FLUSH) == Z_OK ? 0 : -1;
}
static int vt__gz_eof(vt_zio* z) { return gzeof(z->u.gz.gz) || z->eof_flag; }
static int vt__gz_error(vt_zio* z) {
  int err;
  const char* msg = gzerror(z->u.gz.gz, &err);
  (void)msg;
  return err == Z_OK ? 0 : 1;
}
static void vt__gz_close(vt_zio* z) {
  if (z->own && z->u.gz.gz) gzclose(z->u.gz.gz);
}
static int vt__gz_getc(vt_zio* z) {
  int c = gzgetc(z->u.gz.gz);
  if (c == -1) {
    if (gzeof(z->u.gz.gz))
      z->eof_flag = 1;
    else
      z->err = 1;
  }
  return c;
}
static int vt__gz_ungetc(vt_zio* z, int c) {
  return gzungetc(c, z->u.gz.gz) == -1 ? -1 : c;
}

static const vt_zio_vtbl VT__GZ_VTBL = {vt__gz_read, vt__gz_write, vt__gz_seek,
                                        vt__gz_tell, vt__gz_size,  vt__gz_flush,
                                        vt__gz_eof,  vt__gz_error, vt__gz_close,
                                        vt__gz_getc, vt__gz_ungetc};
#endif

/* --------------------------------------------------------------------------
   Constructeurs
   -------------------------------------------------------------------------- */
static vt_zio* vt__new(int kind, const vt_zio_vtbl* vtbl) {
  vt_zio* z = (vt_zio*)calloc(1, sizeof(vt_zio));
  if (!z) return NULL;
  z->v = vtbl;
  z->kind = kind;
  z->own = 1;
  z->err = 0;
  z->eof_flag = 0;
  return z;
}

vt_zio* vt_zio_open_file(const char* path, const char* mode) {
  FILE* f = fopen(path, mode);
  if (!f) return NULL;
  vt_zio* z = vt__new(VT_ZIO_FILE, &VT__FILE_VTBL);
  if (!z) {
    fclose(f);
    return NULL;
  }
  z->u.file.f = f;
  return z;
}

vt_zio* vt_zio_wrap_file(FILE* f, int take_ownership) {
  if (!f) {
    errno = EINVAL;
    return NULL;
  }
  vt_zio* z = vt__new(VT_ZIO_FILE, &VT__FILE_VTBL);
  if (!z) return NULL;
  z->u.file.f = f;
  z->own = take_ownership ? 1 : 0;
  return z;
}

vt_zio* vt_zio_from_ro_memory(const void* data, size_t size) {
  vt_zio* z = vt__new(VT_ZIO_MEM_RO, &VT__MRO_VTBL);
  if (!z) return NULL;
  z->u.mem_ro.p = (const uint8_t*)data;
  z->u.mem_ro.n = size;
  z->u.mem_ro.pos = 0;
  z->u.mem_ro.ungot = -1;
  z->own = 0;
  return z;
}

vt_zio* vt_zio_new_mem_writer(size_t initial_cap) {
  vt_zio* z = vt__new(VT_ZIO_MEM_RW, &VT__MRW_VTBL);
  if (!z) return NULL;
  z->u.mem_rw.p = NULL;
  z->u.mem_rw.n = 0;
  z->u.mem_rw.cap = 0;
  z->u.mem_rw.pos = 0;
  z->u.mem_rw.ungot = -1;
  z->u.mem_rw.owner = 1;
  if (initial_cap) {
    if (vt__grow(&z->u.mem_rw.p, &z->u.mem_rw.cap, initial_cap) != 0) {
      free(z);
      return NULL;
    }
  }
  return z;
}

int vt_zio_mem_writer_take(vt_zio* z, void** out_ptr, size_t* out_size) {
  if (!z || z->kind != VT_ZIO_MEM_RW) {
    errno = EINVAL;
    return -1;
  }
  if (out_ptr)
    *out_ptr = z->u.mem_rw.p;
  else
    free(z->u.mem_rw.p);
  if (out_size) *out_size = z->u.mem_rw.n;
  z->u.mem_rw.p = NULL;
  z->u.mem_rw.n = z->u.mem_rw.cap = z->u.mem_rw.pos = 0;
  z->u.mem_rw.owner = 0;
  return 0;
}

vt_zio* vt_zio_open_mmap_rdonly(const char* path) {
#if defined(_WIN32)
  HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hf == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(hf, &sz)) {
    CloseHandle(hf);
    return NULL;
  }
  HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!hm) {
    CloseHandle(hf);
    return NULL;
  }
  void* p = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
  if (!p) {
    CloseHandle(hm);
    CloseHandle(hf);
    return NULL;
  }

  vt_zio* z = vt__new(VT_ZIO_MMAP, &VT__MMAP_VTBL);
  if (!z) {
    UnmapViewOfFile(p);
    CloseHandle(hm);
    CloseHandle(hf);
    return NULL;
  }
  z->u.mmap.hFile = hf;
  z->u.mmap.hMap = hm;
  z->u.mmap.p = (const uint8_t*)p;
  z->u.mmap.n = (size_t)sz.QuadPart;
  z->u.mmap.pos = 0;
  z->u.mmap.ungot = -1;
  return z;
#else
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return NULL;
  }
  size_t n = (size_t)st.st_size;
  void* p = n ? mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0) : NULL;
  if (n && p == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  vt_zio* z = vt__new(VT_ZIO_MMAP, &VT__MMAP_VTBL);
  if (!z) {
    if (n) munmap(p, n);
    close(fd);
    return NULL;
  }
  z->u.mmap.fd = fd;
  z->u.mmap.p = (const uint8_t*)p;
  z->u.mmap.n = n;
  z->u.mmap.pos = 0;
  z->u.mmap.ungot = -1;
  return z;
#endif
}

#ifdef VT_ZIO_WITH_ZLIB
vt_zio* vt_zio_open_gzip(const char* path, const char* mode) {
  gzFile gz = gzopen(path, mode);
  if (!gz) return NULL;
  vt_zio* z = vt__new(VT_ZIO_GZIP, &VT__GZ_VTBL);
  if (!z) {
    gzclose(gz);
    return NULL;
  }
  z->u.gz.gz = gz;
  return z;
}
#endif

/* --------------------------------------------------------------------------
   API générique
   -------------------------------------------------------------------------- */
size_t vt_zio_read(vt_zio* z, void* buf, size_t n) {
  return z->v->read(z, buf, n);
}
size_t vt_zio_write(vt_zio* z, const void* buf, size_t n) {
  return z->v->write(z, buf, n);
}
int vt_zio_seek(vt_zio* z, int64_t off, int whence) {
  return z->v->seek(z, off, whence);
}
int64_t vt_zio_tell(vt_zio* z) { return z->v->tell(z); }
int64_t vt_zio_size(vt_zio* z) { return z->v->size(z); }
int vt_zio_flush(vt_zio* z) { return z->v->flush(z); }
int vt_zio_eof(vt_zio* z) { return z->v->eof(z); }
int vt_zio_error(vt_zio* z) { return z->v->error(z); }
void vt_zio_close(vt_zio* z) {
  if (!z) return;
  if (z->v && z->v->close) z->v->close(z);
  free(z);
}
int vt_zio_getc(vt_zio* z) { return z->v->getc(z); }
int vt_zio_ungetc(vt_zio* z, int c) { return z->v->ungetc(z, c); }

/* --------------------------------------------------------------------------
   High-level helpers
   -------------------------------------------------------------------------- */
int vt_zio_read_all(vt_zio* z, void** out, size_t* out_len) {
  if (!z || !out || !out_len) {
    errno = EINVAL;
    return -1;
  }
  const size_t CHUNK = 1 << 16;
  uint8_t* p = NULL;
  size_t cap = 0, n = 0;
  for (;;) {
    if (n == cap) {
      size_t ncap = cap ? cap * 2 : CHUNK;
      void* np = realloc(p, ncap);
      if (!np) {
        free(p);
        return -1;
      }
      p = (uint8_t*)np;
      cap = ncap;
    }
    size_t want = cap - n;
    size_t got = vt_zio_read(z, p + n, want);
    n += got;
    if (got < want) { /* EOF or error */
      if (vt_zio_error(z)) {
        free(p);
        return -1;
      }
      break;
    }
  }
  *out = p;
  *out_len = n;
  return 0;
}

ssize_t vt_zio_read_line(vt_zio* z, char** line, size_t* cap) {
  if (!z || !line || !cap) {
    errno = EINVAL;
    return -1;
  }
  if (!*line || *cap == 0) {
    *cap = 256;
    *line = (char*)malloc(*cap);
    if (!*line) return -1;
  }
  size_t len = 0;
  for (;;) {
    int c = vt_zio_getc(z);
    if (c == EOF) {
      if (len == 0) return -1;
      (*line)[len] = '\0';
      return (ssize_t)len;
    }
    if (len + 1 >= *cap) {
      size_t ncap = *cap + *cap / 2 + 1;
      char* np = (char*)realloc(*line, ncap);
      if (!np) return -1;
      *line = np;
      *cap = ncap;
    }
    (*line)[len++] = (char)c;
    if (c == '\n') {
      (*line)[len] = '\0';
      return (ssize_t)len;
    }
  }
}

int vt_zio_write_all(vt_zio* z, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  size_t off = 0;
  while (off < len) {
    size_t w = vt_zio_write(z, p + off, len - off);
    if (w == 0) return -1;
    off += w;
  }
  return 0;
}

/* --------------------------------------------------------------------------
   Tests légers (compilés si VT_ZIO_TEST défini)
   -------------------------------------------------------------------------- */
#ifdef VT_ZIO_TEST
static void vt__assert(int cond, const char* msg) {
  if (!cond) {
    fprintf(stderr, "ZIO assert: %s\n", msg);
    abort();
  }
}

int main(void) {
  /* memory writer/reader roundtrip */
  vt_zio* mw = vt_zio_new_mem_writer(0);
  const char* s = "hello\nworld";
  vt__assert(vt_zio_write_all(mw, s, strlen(s)) == 0, "write_all");
  vt__assert(vt_zio_seek(mw, 0, SEEK_SET) == 0, "seek0");
  char* line = NULL;
  size_t cap = 0;
  ssize_t r = vt_zio_read_line(mw, &line, &cap);
  vt__assert(r > 0 && strncmp(line, "hello\n", 6) == 0, "read_line1");
  r = vt_zio_read_line(mw, &line, &cap);
  vt__assert(r > 0 && strncmp(line, "world", 5) == 0, "read_line2");
  void* blob = NULL;
  size_t n = 0;
  vt__assert(vt_zio_seek(mw, 0, SEEK_SET) == 0, "seek again");
  vt__assert(vt_zio_read_all(mw, &blob, &n) == 0, "read_all");
  vt__assert(n == strlen(s), "read_all size");
  free(blob);
  free(line);
  void* taken = NULL;
  size_t tn = 0;
  vt__assert(vt_zio_mem_writer_take(mw, &taken, &tn) == 0, "take");
  vt__assert(taken && tn == strlen(s), "take ok");
  free(taken);
  vt_zio_close(mw);

  /* mmap self (optional) */
  /* Platform dependent path; skip silently if fails */
  vt_zio* mm = vt_zio_open_mmap_rdonly("zio.c");
  if (mm) {
    int64_t sz = vt_zio_size(mm);
    vt__assert(sz >= 0, "mmap size");
    vt_zio_close(mm);
  }
  return 0;
}
#endif
