/* ============================================================================
   /core/api.c — Runtime C99 « ultra complet » pour applis Vitte/Vitl
   Mono-fichier amalgamé. Zéro dépendance hors libc.
   Plateformes: POSIX (Linux/macOS), Windows.
   ============================================================================
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 199901L
#endif
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/utf8.h"

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#define PATH_SEP '\\'
#define mkdir_p(path) _mkdir(path)
#define statx _stat64
#include <sys/types.h>
#include <sys/utime.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define PATH_SEP '/'
#define mkdir_p(path) mkdir(path, 0777)
#define statx stat
#endif

/* --------------------------------------------------------------------------
   Export / visibilité
   -------------------------------------------------------------------------- */
#if defined(_WIN32)
#define API_EXPORT __declspec(dllexport)
#else
#define API_EXPORT __attribute__((visibility("default")))
#endif

/* --------------------------------------------------------------------------
   Types de base
   -------------------------------------------------------------------------- */
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;
typedef size_t usize;
typedef ptrdiff_t isize;

/* --------------------------------------------------------------------------
   Utilitaires généraux
   -------------------------------------------------------------------------- */
#define ARR_LEN(a) ((isize)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, l, h) (((x) < (l)) ? (l) : (((x) > (h)) ? (h) : (x)))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static inline void* xmalloc(usize n) {
  void* p = malloc(n ? n : 1);
  if (!p) {
    fprintf(stderr, "OOM (%zu bytes)\n", (size_t)n);
    abort();
  }
  return p;
}
static inline void* xrealloc(void* p, usize n) {
  void* q = realloc(p, n ? n : 1);
  if (!q) {
    fprintf(stderr, "OOM-realloc (%zu bytes)\n", (size_t)n);
    abort();
  }
  return q;
}
static inline char* xstrdup(const char* s) {
  usize n = strlen(s);
  char* d = (char*)xmalloc(n + 1);
  memcpy(d, s, n + 1);
  return d;
}

/* --------------------------------------------------------------------------
   Erreur simple
   -------------------------------------------------------------------------- */
typedef struct Err {
  int code; /* 0 = OK */
  char msg[256];
} Err;

static inline Err ok(void) {
  Err e;
  e.code = 0;
  e.msg[0] = 0;
  return e;
}
static inline Err errf(int code, const char* fmt, ...) {
  Err e;
  e.code = code;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  return e;
}

/* --------------------------------------------------------------------------
   Logger minimal (ANSI)
   -------------------------------------------------------------------------- */
typedef enum {
  VL_LOG_TRACE = 0,
  VL_LOG_DEBUG,
  VL_LOG_INFO,
  VL_LOG_WARN,
  VL_LOG_ERROR
} LogLevel;
static LogLevel g_log_level = VL_LOG_INFO;
static bool g_log_color = true;

API_EXPORT void log_set_level(LogLevel lvl) { g_log_level = lvl; }
API_EXPORT void log_set_color(bool on) { g_log_color = on; }

static const char* lvl_name(LogLevel l) {
  switch (l) {
    case VL_LOG_TRACE:
      return "TRACE";
    case VL_LOG_DEBUG:
      return "DEBUG";
    case VL_LOG_INFO:
      return "INFO";
    case VL_LOG_WARN:
      return "WARN";
    default:
      return "ERROR";
  }
}
static const char* lvl_color(LogLevel l) {
  if (!g_log_color) return "";
  switch (l) {
    case VL_LOG_TRACE:
      return "\x1b[90m";
    case VL_LOG_DEBUG:
      return "\x1b[36m";
    case VL_LOG_INFO:
      return "\x1b[32m";
    case VL_LOG_WARN:
      return "\x1b[33m";
    default:
      return "\x1b[31m";
  }
}
static inline void vl_vlogf(LogLevel lvl, const char* fmt, va_list ap) {
  if (lvl < g_log_level) return;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  fprintf((lvl >= VL_LOG_WARN) ? stderr : stdout, "%s[%s]\x1b[0m %s\n",
          lvl_color(lvl), lvl_name(lvl), buf);
}
API_EXPORT void vl_logf(LogLevel lvl, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vl_vlogf(lvl, fmt, ap);
  va_end(ap);
}

/* --------------------------------------------------------------------------
   Chrono / horloge
   -------------------------------------------------------------------------- */
API_EXPORT u64 time_ns_monotonic(void) {
#if defined(_WIN32)
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (u64)((1e9 * (double)c.QuadPart) / (double)f.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}
API_EXPORT u64 time_ms_wall(void) {
#if defined(_WIN32)
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  /* Windows epoch 1601 → Unix 1970 offset */
  const u64 EPOCH_DIFF_100NS = 116444736000000000ull;
  u64 t100ns = (u64)u.QuadPart - EPOCH_DIFF_100NS;
  return t100ns / 10000ull;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (u64)ts.tv_sec * 1000ull + (u64)(ts.tv_nsec / 1000000ull);
#endif
}
API_EXPORT void sleep_ms(u32 ms) {
#if defined(_WIN32)
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
#endif
}

/* --------------------------------------------------------------------------
   Aléatoire
   -------------------------------------------------------------------------- */
typedef struct Lcg {
  u64 s;
} Lcg;
static inline Lcg lcg_new(u64 seed) {
  if (!seed) seed = 0x9e3779b97f4a7c15ull;
  Lcg r = {seed};
  return r;
}
static inline u64 lcg_next(Lcg* r) {
  r->s = r->s * 6364136223846793005ull + 1442695040888963407ull;
  return r->s;
}
API_EXPORT u64 rand_u64(void) {
#if defined(_WIN32)
  /* rand_s: 32-bit; combine */
  unsigned int a = 0, b = 0;
  rand_s(&a);
  rand_s(&b);
  return ((u64)a << 32) | b;
#else
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f) { /* fallback LCG seeded by time */
    static Lcg g;
    static bool init = false;
    if (!init) {
      g = lcg_new((u64)time_ns_monotonic());
      init = true;
    }
    return lcg_next(&g);
  }
  u64 x = 0;
  fread(&x, 1, sizeof(x), f);
  fclose(f);
  return x;
#endif
}
API_EXPORT u64 rand_range_u64(u64 lo, u64 hi) {
  if (hi <= lo) return lo;
  u64 span = hi - lo + 1;
  return lo + (rand_u64() % span);
}

/* --------------------------------------------------------------------------
   StringBuilder
   -------------------------------------------------------------------------- */
typedef struct StrBuf {
  char* data;
  usize len;
  usize cap;
} StrBuf;

API_EXPORT void sb_init(StrBuf* sb) {
  sb->data = NULL;
  sb->len = 0;
  sb->cap = 0;
}
API_EXPORT void sb_free(StrBuf* sb) {
  free(sb->data);
  sb->data = NULL;
  sb->len = sb->cap = 0;
}
static void sb_grow(StrBuf* sb, usize need) {
  if (sb->len + need + 1 <= sb->cap) return;
  usize ncap = sb->cap ? sb->cap : 64;
  while (ncap < sb->len + need + 1) ncap = ncap * 2;
  sb->data = (char*)xrealloc(sb->data, ncap);
  sb->cap = ncap;
}
API_EXPORT void sb_append(StrBuf* sb, const char* s) {
  usize n = strlen(s);
  sb_grow(sb, n);
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}
API_EXPORT void sb_append_n(StrBuf* sb, const char* s, usize n) {
  sb_grow(sb, n);
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}
API_EXPORT void sb_append_fmt(StrBuf* sb, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char tmp[1024];
  int k = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (k < 0) return;
  if ((usize)k < sizeof(tmp)) {
    sb_append_n(sb, tmp, (usize)k);
    return;
  }
  /* rare: allouer juste ce qu'il faut */
  char* big = (char*)xmalloc((usize)k + 1);
  va_list aq;
  va_start(aq, fmt);
  vsnprintf(big, (usize)k + 1, fmt, aq);
  va_end(aq);
  sb_append_n(sb, big, (usize)k);
  free(big);
}

/* --------------------------------------------------------------------------
   Vecteur dynamique générique (macro)
   -------------------------------------------------------------------------- */
#define VEC(T) \
  struct {     \
    T* data;   \
    usize len; \
    usize cap; \
  }
#define vec_init(v)   \
  do {                \
    (v)->data = NULL; \
    (v)->len = 0;     \
    (v)->cap = 0;     \
  } while (0)
#define vec_free(v)          \
  do {                       \
    free((v)->data);         \
    (v)->data = NULL;        \
    (v)->len = (v)->cap = 0; \
  } while (0)
#define vec_reserve(v, need)                                           \
  do {                                                                 \
    if ((v)->len + (need) <= (v)->cap) break;                          \
    usize ncap = (v)->cap ? (v)->cap : 8;                              \
    while (ncap < (v)->len + (need)) ncap <<= 1;                       \
    (v)->data = (void*)xrealloc((v)->data, ncap * sizeof(*(v)->data)); \
    (v)->cap = ncap;                                                   \
  } while (0)
#define vec_push(v, val)           \
  do {                             \
    vec_reserve((v), 1);           \
    (v)->data[(v)->len++] = (val); \
  } while (0)

API_EXPORT usize utf8_encode_1(u32 cp, char out[4]) {
  if (cp <= 0x7f) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp <= 0x7ff) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp <= 0xffff) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

/* --------------------------------------------------------------------------
   Fichiers (lecture/écriture atomique simple)
   -------------------------------------------------------------------------- */
API_EXPORT Err file_read_all(const char* path, VEC(u8) * out) {
  FILE* f = fopen(path, "rb");
  if (!f) return errf(errno, "open '%s' failed: %s", path, strerror(errno));
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return errf(errno, "seek end");
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return errf(errno, "ftell");
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return errf(errno, "seek start");
  }
  vec_init(out);
  vec_reserve(out, (usize)sz);
  out->len = (usize)sz;
  if (sz && fread(out->data, 1, (usize)sz, f) != (usize)sz) {
    fclose(f);
    vec_free(out);
    return errf(errno, "read");
  }
  fclose(f);
  return ok();
}
API_EXPORT Err file_write_all(const char* path, const void* data, usize n) {
  FILE* f = fopen(path, "wb");
  if (!f) return errf(errno, "open '%s' failed: %s", path, strerror(errno));
  if (n && fwrite(data, 1, n, f) != n) {
    int e = errno;
    fclose(f);
    return errf(e, "write");
  }
  if (fclose(f) != 0) return errf(errno, "close");
  return ok();
}
API_EXPORT bool file_exists(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

/* --------------------------------------------------------------------------
   Chemins / dossiers
   -------------------------------------------------------------------------- */
API_EXPORT void path_join(const char* a, const char* b, char* out,
                          usize out_cap) {
  usize na = strlen(a), nb = strlen(b);
  if (na + 1 + nb + 1 > out_cap) { /* tronque */
    snprintf(out, out_cap, "%s%c%s", a, PATH_SEP, b);
    return;
  }
  memcpy(out, a, na);
  out[na] = PATH_SEP;
  memcpy(out + na + 1, b, nb);
  out[na + 1 + nb] = '\0';
}
API_EXPORT Err dir_ensure(const char* path) {
  /* naïf: tente mkdir; si existe déjà, OK */
  if (mkdir_p(path) == 0) return ok();
#if defined(_WIN32)
  if (errno == EEXIST) return ok();
#else
  if (errno == EEXIST) return ok();
#endif
  return errf(errno, "mkdir '%s' failed: %s", path, strerror(errno));
}

/* --------------------------------------------------------------------------
   Hash (FNV-1a 64)
   -------------------------------------------------------------------------- */
API_EXPORT u64 hash64(const void* data, usize n) {
  const u8* p = (const u8*)data;
  u64 h = 1469598103934665603ull;
  for (usize i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}
API_EXPORT u64 hash_str(const char* s) { return hash64(s, strlen(s)); }

/* --------------------------------------------------------------------------
   Table de hachage (open addressing, string → u64)
   -------------------------------------------------------------------------- */
typedef struct {
  char* key; /* propriété: dupliquée (malloc) */
  u64 val;
  u64 hash;
  bool used;
} HSlot;

typedef struct {
  HSlot* slots;
  usize cap;
  usize len;
} MapStrU64;

static usize map_ideal_cap(usize need) {
  usize c = 16;
  while (c < need * 2) c <<= 1;
  return c;
}
static void map_rehash(MapStrU64* m, usize ncap) {
  HSlot* ns = (HSlot*)calloc(ncap, sizeof(HSlot));
  for (usize i = 0; i < m->cap; i++) {
    HSlot* s = &m->slots[i];
    if (!s->used) continue;
    usize j = s->hash & (ncap - 1);
    while (ns[j].used) j = (j + 1) & (ncap - 1);
    ns[j] = *s;
  }
  free(m->slots);
  m->slots = ns;
  m->cap = ncap;
}
API_EXPORT void map_init(MapStrU64* m) {
  m->slots = NULL;
  m->cap = 0;
  m->len = 0;
}
API_EXPORT void map_free(MapStrU64* m) {
  if (m->slots) {
    for (usize i = 0; i < m->cap; i++)
      if (m->slots[i].used) free(m->slots[i].key);
    free(m->slots);
  }
  m->slots = NULL;
  m->cap = 0;
  m->len = 0;
}
API_EXPORT void map_put(MapStrU64* m, const char* key, u64 val) {
  if (m->cap == 0) {
    m->cap = 16;
    m->slots = (HSlot*)calloc(m->cap, sizeof(HSlot));
  }
  if ((m->len * 10) / m->cap >= 7) {
    map_rehash(m, m->cap * 2);
  }
  u64 h = hash_str(key);
  usize i = h & (m->cap - 1);
  for (;;) {
    HSlot* s = &m->slots[i];
    if (!s->used) {
      s->used = true;
      s->hash = h;
      s->key = xstrdup(key);
      s->val = val;
      m->len++;
      return;
    }
    if (s->hash == h && strcmp(s->key, key) == 0) {
      s->val = val;
      return;
    }
    i = (i + 1) & (m->cap - 1);
  }
}
API_EXPORT bool map_get(const MapStrU64* m, const char* key, u64* out) {
  if (m->cap == 0) return false;
  u64 h = hash_str(key);
  usize i = h & (m->cap - 1);
  for (;;) {
    const HSlot* s = &m->slots[i];
    if (!s->used) return false;
    if (s->hash == h && strcmp(s->key, key) == 0) {
      if (out) *out = s->val;
      return true;
    }
    i = (i + 1) & (m->cap - 1);
  }
}

/* --------------------------------------------------------------------------
   Arena allocator (bump)
   -------------------------------------------------------------------------- */
typedef struct Arena {
  u8* base;
  usize cap;
  usize off;
} Arena;

API_EXPORT Arena arena_new(usize cap) {
  Arena a;
  a.base = (u8*)xmalloc(cap);
  a.cap = cap;
  a.off = 0;
  return a;
}
API_EXPORT void arena_free(Arena* a) {
  free(a->base);
  a->base = NULL;
  a->cap = a->off = 0;
}
API_EXPORT void arena_reset(Arena* a) { a->off = 0; }
API_EXPORT void* arena_alloc(Arena* a, usize n, usize align) {
  usize p = ALIGN_UP(a->off, align ? align : 1);
  if (p + n > a->cap) {
    fprintf(stderr, "arena: OOM (%zu/%zu)\n", (size_t)(p + n), (size_t)a->cap);
    abort();
  }
  void* mem = a->base + p;
  a->off = p + n;
  return mem;
}
API_EXPORT char* arena_strdup(Arena* a, const char* s) {
  usize n = strlen(s) + 1;
  char* d = (char*)arena_alloc(a, n, 1);
  memcpy(d, s, n);
  return d;
}

/* --------------------------------------------------------------------------
   Petite API JSON (écriture seulement, sans échappement avancé)
   -------------------------------------------------------------------------- */
typedef struct {
  StrBuf sb;
  int depth;
  bool first_in_level[64];
} JsonW;

API_EXPORT void jw_begin(JsonW* w) {
  sb_init(&w->sb);
  w->depth = 0;
  memset(w->first_in_level, 1, sizeof(w->first_in_level));
}
API_EXPORT void jw_free(JsonW* w) { sb_free(&w->sb); }
static void jw_sep(JsonW* w) {
  if (!w->first_in_level[w->depth])
    sb_append(&w->sb, ",");
  else
    w->first_in_level[w->depth] = false;
}
API_EXPORT void jw_obj_begin(JsonW* w) {
  jw_sep(w);
  sb_append(&w->sb, "{");
  w->depth++;
  w->first_in_level[w->depth] = true;
}
API_EXPORT void jw_obj_end(JsonW* w) {
  w->depth--;
  sb_append(&w->sb, "}");
}
API_EXPORT void jw_arr_begin(JsonW* w) {
  jw_sep(w);
  sb_append(&w->sb, "[");
  w->depth++;
  w->first_in_level[w->depth] = true;
}
API_EXPORT void jw_arr_end(JsonW* w) {
  w->depth--;
  sb_append(&w->sb, "]");
}
API_EXPORT void jw_key(JsonW* w, const char* k) {
  jw_sep(w);
  sb_append_fmt(&w->sb, "\"%s\":", k);
}
API_EXPORT void jw_str(JsonW* w, const char* v) {
  jw_sep(w);
  sb_append_fmt(&w->sb, "\"%s\"", v);
}
API_EXPORT void jw_i64(JsonW* w, i64 v) {
  jw_sep(w);
  sb_append_fmt(&w->sb, "%lld", (long long)v);
}
API_EXPORT void jw_f64(JsonW* w, f64 v) {
  jw_sep(w);
  sb_append_fmt(&w->sb, "%.17g", v);
}
API_EXPORT void jw_bool(JsonW* w, bool v) {
  jw_sep(w);
  sb_append(&w->sb, v ? "true" : "false");
}
API_EXPORT void jw_null(JsonW* w) {
  jw_sep(w);
  sb_append(&w->sb, "null");
}
API_EXPORT const char* jw_cstr(JsonW* w) {
  return w->sb.data ? w->sb.data : "";
}

/* --------------------------------------------------------------------------
   Process/env (lecture)
   -------------------------------------------------------------------------- */
API_EXPORT const char* env_get(const char* key) {
#if defined(_WIN32)
  static char buf[32768];
  DWORD n = GetEnvironmentVariableA(key, buf, (DWORD)sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return NULL;
  return buf;
#else
  return getenv(key);
#endif
}

/* --------------------------------------------------------------------------
   Mini-CLI ANSI helpers (paint)
   -------------------------------------------------------------------------- */
API_EXPORT const char* ansi_reset(void) { return "\x1b[0m"; }
API_EXPORT const char* ansi_bold(void) { return "\x1b[1m"; }
API_EXPORT const char* ansi_red(void) { return "\x1b[31m"; }
API_EXPORT const char* ansi_green(void) { return "\x1b[32m"; }
API_EXPORT const char* ansi_yellow(void) { return "\x1b[33m"; }
API_EXPORT const char* ansi_blue(void) { return "\x1b[34m"; }
API_EXPORT void ansi_paint_to(StrBuf* out, const char* text, const char* pre) {
  sb_append(out, pre);
  sb_append(out, text);
  sb_append(out, ansi_reset());
}

/* --------------------------------------------------------------------------
   Démonstration intégrée (peut servir de test rapide)
   -------------------------------------------------------------------------- */
#ifdef API_DEMO_MAIN
int main(void) {
  log_set_level(VL_LOG_DEBUG);
  vl_logf(VL_LOG_INFO, "api init t=%llums", (unsigned long long)time_ms_wall());

  /* RNG */
  vl_logf(VL_LOG_DEBUG, "rand=%llu", (unsigned long long)rand_u64());

  /* StrBuf + JSON */
  JsonW jw;
  jw_begin(&jw);
  jw_obj_begin(&jw);
  jw_key(&jw, "hello");
  jw_str(&jw, "world");
  jw_key(&jw, "num");
  jw_i64(&jw, 42);
  jw_key(&jw, "ok");
  jw_bool(&jw, true);
  jw_obj_end(&jw);
  vl_logf(VL_LOG_INFO, "json: %s", jw_cstr(&jw));
  jw_free(&jw);

  /* Map */
  MapStrU64 mp;
  map_init(&mp);
  map_put(&mp, "a", 1);
  map_put(&mp, "b", 2);
  map_put(&mp, "a", 3);
  u64 v = 0;
  if (map_get(&mp, "a", &v))
    vl_logf(VL_LOG_INFO, "map[a]=%llu", (unsigned long long)v);
  map_free(&mp);

  /* Arena */
  Arena a = arena_new(1024);
  char* s = arena_strdup(&a, "arena-string");
  vl_logf(VL_LOG_INFO, "arena str: %s", s);
  arena_free(&a);

  /* File roundtrip */
  const char* path = "api_demo.txt";
  const char* msg = "Hello from api.c\n";
  Err e = file_write_all(path, msg, strlen(msg));
  if (e.code) vl_logf(VL_LOG_ERROR, "write failed: %s", e.msg);
  VEC(u8) bin;
  if (!e.code) {
    e = file_read_all(path, &bin);
  }
  if (!e.code) {
    StrBuf sb;
    sb_init(&sb);
    ansi_paint_to(&sb, (const char*)bin.data, ansi_green());
    vl_logf(VL_LOG_INFO, "%s", sb.data);
    sb_free(&sb);
    vec_free(&bin);
  }

  /* UTF-8 */
  const char* u8s = "été";
  usize i = 0;
  while (u8s[i]) {
    usize adv = 0;
    u32 cp = utf8_decode_1(u8s + i, strlen(u8s) - i, &adv);
    vl_logf(VL_LOG_DEBUG, "cp U+%04X", (unsigned)cp);
    i += adv ? adv : 1;
  }

  vl_logf(VL_LOG_INFO, "done");
  return 0;
}
#endif

/* ========================================================================== */
