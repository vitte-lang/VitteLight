// SPDX-License-Identifier: GPL-3.0-or-later
//
// auxlib.c — utilitaires portables C17 pour Vitte Light
// - Journalisation thread-safe (TRACE..FATAL), couleurs ANSI optionnelles
// - Fichiers: read_all, write_all, mkdirs, exists, is_file, is_dir
// - Chemins: join, dirname, basename
// - Chaînes: trim, starts_with, ends_with, replace_all
// - Parsing: u64/i64, taille avec suffixes (K/M/G/T), booléens
// - Temps: now_millis, now_nanos, horodatage ISO-8601 local/UTC
// - CRC32 (IEEE), streaming
// - RNG: rand_u64, rand_bytes (CSPRNG si possible)
// - Hexdump
//
// Implémentation C17, sans dépendances externes.
// Plateformes: POSIX et Windows.
//
// Correspond à l’en-tête: includes/auxlib.h

#include "auxlib.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// ============================================================
// Configuration et état global minimal
// ============================================================

#ifndef AUX_ARRAY_LEN
#define AUX_ARRAY_LEN(x) ((int)(sizeof(x) / sizeof((x)[0])))
#endif

typedef struct AuxLogState {
  FILE *sink;
  AuxLogLevel level;
  bool use_color;
#if defined(_WIN32)
  CRITICAL_SECTION mtx;
  bool init;
#else
  pthread_mutex_t mtx;
  bool init;
#endif
} AuxLogState;

static AuxLogState g_log = {0};

static inline void aux_mtx_init_once(void) {
  if (g_log.init) return;
#if defined(_WIN32)
  InitializeCriticalSection(&g_log.mtx);
#else
  pthread_mutex_init(&g_log.mtx, NULL);
#endif
  g_log.init = true;
}

static inline void aux_mtx_lock(void) {
  aux_mtx_init_once();
#if defined(_WIN32)
  EnterCriticalSection(&g_log.mtx);
#else
  pthread_mutex_lock(&g_log.mtx);
#endif
}

static inline void aux_mtx_unlock(void) {
#if defined(_WIN32)
  LeaveCriticalSection(&g_log.mtx);
#else
  pthread_mutex_unlock(&g_log.mtx);
#endif
}

// ============================================================
// Journalisation
// ============================================================

static const char *s_level_str[AUX_LOG_COUNT] = {"TRACE", "DEBUG", "INFO",
                                                 "WARN",  "ERROR", "FATAL"};

static const char *s_level_color[AUX_LOG_COUNT] = {
    "\x1b[90m",  // TRACE - bright black
    "\x1b[36m",  // DEBUG - cyan
    "\x1b[32m",  // INFO  - green
    "\x1b[33m",  // WARN  - yellow
    "\x1b[31m",  // ERROR - red
    "\x1b[35m",  // FATAL - magenta
};

static inline bool aux_is_tty(FILE *f) {
#if defined(_WIN32)
  return _isatty(_fileno(f)) != 0;
#else
  return isatty(fileno(f)) != 0;
#endif
}

void aux_log_init(FILE *sink, AuxLogLevel level, bool color) {
  aux_mtx_init_once();
  aux_mtx_lock();
  g_log.sink = sink ? sink : stderr;
  g_log.level = level;
  g_log.use_color = color && aux_is_tty(g_log.sink);
  aux_mtx_unlock();
}

void aux_log_set_level(AuxLogLevel level) {
  aux_mtx_lock();
  g_log.level = level;
  aux_mtx_unlock();
}

void aux_log_enable_color(bool on) {
  aux_mtx_lock();
  g_log.use_color = on && aux_is_tty(g_log.sink);
  aux_mtx_unlock();
}

void aux_log_set_sink(FILE *sink) {
  aux_mtx_lock();
  g_log.sink = sink ? sink : stderr;
  aux_mtx_unlock();
}

static void aux_format_time_iso(char *buf, size_t n, bool utc) {
  struct timespec ts;
#if defined(_WIN32)
  timespec_get(&ts, TIME_UTC);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  time_t t = (time_t)ts.tv_sec;
  struct tm tmv;
  if (utc) {
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
  } else {
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
  }
  // YYYY-MM-DDThh:mm:ss.mmmZ
  int ms = (int)(ts.tv_nsec / 1000000);
  if (utc) {
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             ms);
  } else {
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03d", tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             ms);
  }
}

void aux_logf(AuxLogLevel lvl, const char *file, int line, const char *func,
              const char *fmt, ...) {
  aux_mtx_init_once();
  if (lvl < 0 || lvl >= AUX_LOG_COUNT) lvl = AUX_LOG_ERROR;

  aux_mtx_lock();
  FILE *out = g_log.sink ? g_log.sink : stderr;
  if (lvl < g_log.level) {
    aux_mtx_unlock();
    return;
  }

  char ts[40];
  aux_format_time_iso(ts, sizeof ts, true);

  if (g_log.use_color) {
    fprintf(out, "%s[%s]%s %s %s:%d %s(): ", s_level_color[lvl],
            s_level_str[lvl], "\x1b[0m", ts, file ? file : "?", line,
            func ? func : "?");
  } else {
    fprintf(out, "[%s] %s %s:%d %s(): ", s_level_str[lvl], ts,
            file ? file : "?", line, func ? func : "?");
  }

  va_list ap;
  va_start(ap, fmt);
  vfprintf(out, fmt, ap);
  va_end(ap);
  fputc('\n', out);

  if (lvl == AUX_LOG_FATAL) {
    fflush(out);
  }
  aux_mtx_unlock();
}

// ============================================================
// Fichiers et chemins
// ============================================================

static bool aux_stat(const char *path, struct stat *st) {
#if defined(_WIN32)
  struct _stat64 s;
  if (_stat64(path, &s) != 0) return false;
  if (st) {
    memset(st, 0, sizeof *st);
    st->st_mode = (unsigned short)s.st_mode;
    st->st_size = (off_t)s.st_size;
  }
  return true;
#else
  return stat(path, st) == 0;
#endif
}

bool aux_path_exists(const char *path) {
  if (!path || !*path) return false;
  return aux_stat(path, NULL);
}

bool aux_is_file(const char *path) {
  struct stat st;
  if (!aux_stat(path, &st)) return false;
#if defined(_WIN32)
  return (st.st_mode & _S_IFREG) != 0;
#else
  return S_ISREG(st.st_mode);
#endif
}

bool aux_is_dir(const char *path) {
  struct stat st;
  if (!aux_stat(path, &st)) return false;
#if defined(_WIN32)
  return (st.st_mode & _S_IFDIR) != 0;
#else
  return S_ISDIR(st.st_mode);
#endif
}

static int aux_mkdir_one(const char *path) {
#if defined(_WIN32)
  int r = _mkdir(path);
  if (r == 0 || errno == EEXIST) return 0;
  return r;
#else
  int r = mkdir(path, 0777);
  if (r == 0 || errno == EEXIST) return 0;
  return r;
#endif
}

AuxStatus aux_mkdirs(const char *path) {
  if (!path || !*path) return AUX_EINVAL;

  char buf[AUX_PATH_MAX];
  size_t n = strnlen(path, AUX_PATH_MAX - 1);
  if (n >= AUX_PATH_MAX) return AUX_ERANGE;
  memcpy(buf, path, n);
  buf[n] = '\0';

  // Normaliser séparateurs sur Windows
#if defined(_WIN32)
  for (size_t i = 0; i < n; i++)
    if (buf[i] == '/') buf[i] = '\\';
  const char sep = '\\';
#else
  const char sep = '/';
#endif

  // Créer hiérarchie
  for (size_t i = 1; i < n; i++) {
    if (buf[i] == sep) {
      char c = buf[i];
      buf[i] = '\0';
      if (*buf) {
        if (aux_mkdir_one(buf) != 0 && errno != EEXIST) {
          return AUX_EIO;
        }
      }
      buf[i] = c;
    }
  }
  if (aux_mkdir_one(buf) != 0 && errno != EEXIST) {
    return AUX_EIO;
  }
  return AUX_OK;
}

AuxStatus aux_path_join(const char *a, const char *b, char *out,
                        size_t out_sz) {
  if (!a || !b || !out || out_sz == 0) return AUX_EINVAL;
#if defined(_WIN32)
  const char sep = '\\';
  const char other = '/';
#else
  const char sep = '/';
  const char other = '\\';
#endif
  size_t na = strnlen(a, out_sz);
  size_t nb = strnlen(b, out_sz);
  if (na >= out_sz) return AUX_ERANGE;
  if (nb >= out_sz) return AUX_ERANGE;

  bool needs_sep = na > 0 && a[na - 1] != sep && a[na - 1] != other;
  int written =
      snprintf(out, out_sz, "%s%s%s", a, needs_sep ? (char[2]){sep, 0} : "",
               (b[0] == sep || b[0] == other) ? b + 1 : b);
  return (written < 0 || (size_t)written >= out_sz) ? AUX_ERANGE : AUX_OK;
}

const char *aux_basename(const char *path) {
  if (!path) return "";
  const char *p = path;
  const char *last = p;
  for (; *p; ++p) {
    if (*p == '/' || *p == '\\') last = p + 1;
  }
  return last;
}

size_t aux_dirname(const char *path, char *out, size_t out_sz) {
  if (!path || !out || out_sz == 0) return 0;
  size_t n = strnlen(path, out_sz);
  if (n >= out_sz) n = out_sz - 1;
  size_t end = n;
  while (end > 0 && path[end - 1] != '/' && path[end - 1] != '\\') end--;
  if (end == 0) {
    out[0] = '.';
    out[1] = '\0';
    return 1;
  }
  if (end >= out_sz) end = out_sz - 1;
  memcpy(out, path, end);
  out[end] = '\0';
  return end;
}

AuxStatus aux_read_file(const char *path, AuxBuffer *out) {
  if (!path || !out) return AUX_EINVAL;
  FILE *f = fopen(path, "rb");
  if (!f) return AUX_EIO;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return AUX_EIO;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return AUX_EIO;
  }
  rewind(f);

  uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return AUX_ENOMEM;
  }

  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    free(buf);
    return AUX_EIO;
  }
  buf[sz] = 0;

  out->data = buf;
  out->len = (size_t)sz;
  return AUX_OK;
}

AuxStatus aux_write_file(const char *path, const void *data, size_t len,
                         bool mkdirs) {
  if (!path || (!data && len)) return AUX_EINVAL;
  if (mkdirs) {
    char dir[AUX_PATH_MAX];
    if (aux_dirname(path, dir, sizeof dir) > 0) {
      AuxStatus s = aux_mkdirs(dir);
      if (s != AUX_OK) return s;
    }
  }
  FILE *f = fopen(path, "wb");
  if (!f) return AUX_EIO;
  size_t wr = len ? fwrite(data, 1, len, f) : 0;
  if (fclose(f) != 0) return AUX_EIO;
  return (wr == len) ? AUX_OK : AUX_EIO;
}

void aux_buffer_free(AuxBuffer *buf) {
  if (!buf || !buf->data) return;
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
}

// ============================================================
// Chaînes
// ============================================================

static inline bool aux_isspace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}

char *aux_ltrim(char *s) {
  if (!s) return NULL;
  while (*s && aux_isspace(*s)) s++;
  return s;
}

void aux_rtrim_inplace(char *s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && aux_isspace(s[n - 1])) {
    s[n - 1] = 0;
    n--;
  }
}

char *aux_trim_inplace(char *s) {
  if (!s) return NULL;
  char *l = aux_ltrim(s);
  if (l != s) memmove(s, l, strlen(l) + 1);
  aux_rtrim_inplace(s);
  return s;
}

bool aux_starts_with(const char *s, const char *prefix) {
  if (!s || !prefix) return false;
  size_t ns = strlen(s), np = strlen(prefix);
  if (np > ns) return false;
  return memcmp(s, prefix, np) == 0;
}

bool aux_ends_with(const char *s, const char *suffix) {
  if (!s || !suffix) return false;
  size_t ns = strlen(s), nx = strlen(suffix);
  if (nx > ns) return false;
  return memcmp(s + ns - nx, suffix, nx) == 0;
}

AuxStatus aux_replace_all_alloc(const char *s, const char *from, const char *to,
                                char **out) {
  if (!s || !from || !to || !out) return AUX_EINVAL;
  if (!*from) return AUX_EINVAL;
  size_t ns = strlen(s), nf = strlen(from), nt = strlen(to);

  // Compter occurrences
  size_t cnt = 0;
  for (const char *p = s; (p = strstr(p, from)); p += nf) cnt++;

  size_t out_sz = ns + cnt * ((nt >= nf) ? (nt - nf) : 0) + 1;
  char *res = (char *)malloc(out_sz);
  if (!res) return AUX_ENOMEM;

  char *w = res;
  const char *p = s;
  const char *hit;
  while ((hit = strstr(p, from))) {
    size_t pre = (size_t)(hit - p);
    memcpy(w, p, pre);
    w += pre;
    memcpy(w, to, nt);
    w += nt;
    p = hit + nf;
  }
  size_t tail = strlen(p);
  memcpy(w, p, tail);
  w += tail;
  *w = 0;

  *out = res;
  return AUX_OK;
}

// ============================================================
// Parsing basique
// ============================================================

AuxStatus aux_parse_u64(const char *s, uint64_t *out) {
  if (!s || !out) return AUX_EINVAL;
  errno = 0;
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno || end == s || *end != '\0') return AUX_EINVAL;
  *out = (uint64_t)v;
  return AUX_OK;
}

AuxStatus aux_parse_i64(const char *s, int64_t *out) {
  if (!s || !out) return AUX_EINVAL;
  errno = 0;
  char *end = NULL;
  long long v = strtoll(s, &end, 0);
  if (errno || end == s || *end != '\0') return AUX_EINVAL;
  *out = (int64_t)v;
  return AUX_OK;
}

AuxStatus aux_parse_bool(const char *s, bool *out) {
  if (!s || !out) return AUX_EINVAL;
  if (strcasecmp(s, "1") == 0 || strcasecmp(s, "true") == 0 ||
      strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0) {
    *out = true;
    return AUX_OK;
  }
  if (strcasecmp(s, "0") == 0 || strcasecmp(s, "false") == 0 ||
      strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0) {
    *out = false;
    return AUX_OK;
  }
  return AUX_EINVAL;
}

AuxStatus aux_parse_size(const char *s, uint64_t *out) {
  if (!s || !out) return AUX_EINVAL;
  size_t n = strlen(s);
  if (n == 0) return AUX_EINVAL;

  char *end = NULL;
  errno = 0;
  double val = strtod(s, &end);
  if (errno || end == s) return AUX_EINVAL;

  uint64_t mul = 1;
  if (*end) {
    if (end[1] && end[2]) return AUX_EINVAL;  // max 2 chars
    switch (*end) {
      case 'k':
      case 'K':
        mul = 1024ULL;
        break;
      case 'm':
      case 'M':
        mul = 1024ULL * 1024ULL;
        break;
      case 'g':
      case 'G':
        mul = 1024ULL * 1024ULL * 1024ULL;
        break;
      case 't':
      case 'T':
        mul = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        break;
      default:
        return AUX_EINVAL;
    }
    if (end[1]) {
      if (end[1] != 'i' && end[1] != 'I')
        return AUX_EINVAL;  // accept Ki, Mi, etc.
    }
  }
  if (val < 0) return AUX_EINVAL;
  long double bytes = (long double)val * (long double)mul;
  if (bytes > (long double)UINT64_MAX) return AUX_ERANGE;
  *out = (uint64_t)(bytes + 0.5L);
  return AUX_OK;
}

// ============================================================
// Temps
// ============================================================

uint64_t aux_now_millis(void) {
#if defined(_WIN32)
  LARGE_INTEGER freq, ctr;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&ctr);
  return (uint64_t)((ctr.QuadPart * 1000ULL) / (uint64_t)freq.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

uint64_t aux_now_nanos(void) {
#if defined(_WIN32)
  LARGE_INTEGER freq, ctr;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&ctr);
  // seconds = ctr/freq -> ns = seconds * 1e9
  long double ns =
      ((long double)ctr.QuadPart * 1000000000.0L) / (long double)freq.QuadPart;
  if (ns < 0) ns = 0;
  return (uint64_t)(ns + 0.5L);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

size_t aux_time_iso8601(char *buf, size_t n, time_t t, bool utc) {
  if (!buf || n == 0) return 0;
  struct tm tmv;
  if (utc) {
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
  } else {
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
  }
  int w = snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d%s",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                   tmv.tm_min, tmv.tm_sec, utc ? "Z" : "");
  if (w < 0) return 0;
  if ((size_t)w >= n) {
    buf[n - 1] = 0;
    return n - 1;
  }
  return (size_t)w;
}

// ============================================================
// CRC32 (IEEE 802.3), table-driven
// ============================================================

static uint32_t crc32_table[256];
static bool crc32_init_done = false;

static void crc32_init(void) {
  if (crc32_init_done) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
    }
    crc32_table[i] = c;
  }
  crc32_init_done = true;
}

uint32_t aux_crc32(const void *data, size_t len) {
  if (!data) return 0;
  crc32_init();
  const uint8_t *p = (const uint8_t *)data;
  uint32_t c = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; i++) {
    c = crc32_table[(c ^ p[i]) & 0xFFU] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFU;
}

void aux_crc32_init(AuxCrc32 *ctx) {
  if (!ctx) return;
  crc32_init();
  ctx->state = 0xFFFFFFFFU;
}

void aux_crc32_update(AuxCrc32 *ctx, const void *data, size_t len) {
  if (!ctx || !data) return;
  const uint8_t *p = (const uint8_t *)data;
  uint32_t c = ctx->state;
  for (size_t i = 0; i < len; i++) {
    c = crc32_table[(c ^ p[i]) & 0xFFU] ^ (c >> 8);
  }
  ctx->state = c;
}

uint32_t aux_crc32_final(AuxCrc32 *ctx) {
  if (!ctx) return 0;
  return ctx->state ^ 0xFFFFFFFFU;
}

// ============================================================
// RNG
// ============================================================

#if defined(_WIN32)
static bool win32_rand_bytes(void *out, size_t len) {
  HCRYPTPROV hProv = 0;
  if (!CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL,
                            CRYPT_VERIFYCONTEXT)) {
    return false;
  }
  BOOL ok = CryptGenRandom(hProv, (DWORD)len, (BYTE *)out);
  CryptReleaseContext(hProv, 0);
  return ok != 0;
}
#endif

AuxStatus aux_rand_bytes(void *out, size_t len) {
  if (!out && len) return AUX_EINVAL;
#if defined(_WIN32)
  if (win32_rand_bytes(out, len)) return AUX_OK;
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    size_t off = 0;
    while (off < len) {
      ssize_t r = read(fd, (uint8_t *)out + off, len - off);
      if (r <= 0) {
        close(fd);
        return AUX_EIO;
      }
      off += (size_t)r;
    }
    close(fd);
    return AUX_OK;
  }
#endif
  // Fallback non-CSPRNG
  for (size_t i = 0; i < len; i++) {
    ((uint8_t *)out)[i] = (uint8_t)(rand() & 0xFF);
  }
  return AUX_OK;
}

uint64_t aux_rand_u64(void) {
  uint64_t v = 0;
  if (aux_rand_bytes(&v, sizeof v) == AUX_OK) return v;
  // Fallback
  v ^= ((uint64_t)rand() << 32) ^ (uint64_t)rand();
  return v;
}

// ============================================================
// Hexdump
// ============================================================

void aux_hexdump(const void *data, size_t len, size_t cols, FILE *out) {
  if (!out) out = stderr;
  if (!data) {
    fprintf(out, "(null)\n");
    return;
  }
  if (cols == 0) cols = 16;

  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i += cols) {
    fprintf(out, "%08zx  ", i);
    size_t j;
    for (j = 0; j < cols && i + j < len; j++) {
      fprintf(out, "%02x ", p[i + j]);
    }
    for (; j < cols; j++) fputs("   ", out);
    fputs(" |", out);
    for (j = 0; j < cols && i + j < len; j++) {
      uint8_t c = p[i + j];
      fputc((c >= 32 && c < 127) ? (char)c : '.', out);
    }
    fputs("|\n", out);
  }
}

// ============================================================
// Environnement
// ============================================================

const char *aux_getenv(const char *key) {
  if (!key || !*key) return NULL;
#if defined(_WIN32)
  static char buf[32767];
  DWORD n = GetEnvironmentVariableA(key, buf, AUX_ARRAY_LEN(buf));
  if (n == 0 || n >= AUX_ARRAY_LEN(buf)) return NULL;
  return buf;
#else
  return getenv(key);
#endif
}

// ============================================================
// Helpers d’erreurs
// ============================================================

const char *aux_status_str(AuxStatus s) {
  switch (s) {
    case AUX_OK:
      return "OK";
    case AUX_EINVAL:
      return "EINVAL";
    case AUX_ENOMEM:
      return "ENOMEM";
    case AUX_EIO:
      return "EIO";
    case AUX_ERANGE:
      return "ERANGE";
    default:
      return "UNKNOWN";
  }
}

void aux_perror(AuxStatus st, const char *ctx) {
  if (ctx && *ctx) {
    AUX_LOG_ERROR("%s: %s", ctx, aux_status_str(st));
  } else {
    AUX_LOG_ERROR("%s", aux_status_str(st));
  }
}

// ============================================================
// Initialisation globale optionnelle
// ============================================================

void aux_init_default_logging(void) {
  aux_log_init(stderr, AUX_LOG_INFO, /*color*/ true);
}

void aux_shutdown_logging(void) {
  // Pas de ressources à libérer dans cette implémentation
}

// ============================================================
// Fin
// ============================================================
