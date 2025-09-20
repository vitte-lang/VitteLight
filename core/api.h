// SPDX-License-Identifier: MIT
/* ============================================================================
   core/api.h — En-tête C99 pour runtime Vitte/Vitl
   Public API. Compatible C et C++.
   ============================================================================
 */
#ifndef CORE_API_H
#define CORE_API_H

#if defined(_MSC_VER)
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 199901L
#endif
#endif

#include <stdbool.h>  /* bool              */
#include <stddef.h>   /* size_t, ptrdiff_t */
#include <stdint.h>   /* int*_t, uint*_t   */
#include <stdio.h>    /* FILE, fprintf     */
#include <stdlib.h>   /* malloc, realloc   */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Export                                                                     */
/* -------------------------------------------------------------------------- */
#if defined(_WIN32)
# if defined(API_BUILD)
#  define API_EXPORT __declspec(dllexport)
# else
#  define API_EXPORT __declspec(dllimport)
# endif
#else
# define API_EXPORT __attribute__((visibility("default")))
#endif

/* -------------------------------------------------------------------------- */
/* Types scalaires                                                            */
/* -------------------------------------------------------------------------- */
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef float     f32;
typedef double    f64;
typedef size_t    usize;
typedef ptrdiff_t isize;

/* -------------------------------------------------------------------------- */
/* Erreur                                                                     */
/* -------------------------------------------------------------------------- */
typedef struct Err {
  int  code;       /* 0 = OK */
  char msg[256];   /* message formaté */
} Err;

static inline Err api_ok(void) {
  Err e; e.code = 0; e.msg[0] = 0; return e;
}

#include <stdarg.h>

static inline Err api_errf(int code, const char* fmt, ...) {
  Err e; e.code = code;
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
#   if defined(_MSC_VER)
    _vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
#   else
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
#   endif
    va_end(ap);
  } else {
    e.msg[0] = 0;
  }
  return e;
}

/* -------------------------------------------------------------------------- */
/* Logger                                                                     */
/* -------------------------------------------------------------------------- */
typedef enum {
  LOG_TRACE = 0,
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR
} LogLevel;

API_EXPORT void log_set_level(LogLevel lvl);
API_EXPORT void log_set_color(bool on);
API_EXPORT void logf(LogLevel lvl, const char* fmt, ...);

/* -------------------------------------------------------------------------- */
/* Temps / sommeil                                                            */
/* -------------------------------------------------------------------------- */
API_EXPORT u64 time_ns_monotonic(void);
API_EXPORT u64 time_ms_wall(void);
API_EXPORT void sleep_ms(u32 ms);

/* -------------------------------------------------------------------------- */
/* Aléatoire                                                                  */
/* -------------------------------------------------------------------------- */
API_EXPORT u64 rand_u64(void);
API_EXPORT u64 rand_range_u64(u64 lo, u64 hi);

/* -------------------------------------------------------------------------- */
/* StringBuilder                                                              */
/* -------------------------------------------------------------------------- */
typedef struct StrBuf {
  char*  data;
  usize  len;
  usize  cap;
} StrBuf;

API_EXPORT void sb_init(StrBuf* sb);
API_EXPORT void sb_free(StrBuf* sb);
API_EXPORT void sb_append(StrBuf* sb, const char* s);
API_EXPORT void sb_append_n(StrBuf* sb, const char* s, usize n);
API_EXPORT void sb_append_fmt(StrBuf* sb, const char* fmt, ...);

/* -------------------------------------------------------------------------- */
/* Vecteur dynamique (macros header-only)                                     */
/* -------------------------------------------------------------------------- */
#define VEC(T) \
  struct {     \
    T*    data; \
    usize len;  \
    usize cap;  \
  }

#define vec_init(v)   do { (v)->data=NULL; (v)->len=0; (v)->cap=0; } while (0)
#define vec_free(v)   do { free((v)->data); (v)->data=NULL; (v)->len=(v)->cap=0; } while (0)

#define vec_reserve(v, need) do {                                        \
  if ((v)->len + (need) <= (v)->cap) break;                              \
  usize _ncap = (v)->cap ? (v)->cap : 8;                                 \
  while (_ncap < (v)->len + (need)) _ncap <<= 1;                         \
  (v)->data = (void*)realloc((v)->data, _ncap * sizeof(*(v)->data));     \
  if (!(v)->data) { fprintf(stderr,"vec_reserve OOM\n"); abort(); }       \
  (v)->cap = _ncap;                                                      \
} while (0)

#define vec_push(v,val) do { vec_reserve((v),1); (v)->data[(v)->len++]=(val);} while(0)

/* Alias nommés pour éviter les "struct <unnamed>" incompatibles */
typedef VEC(u8)  vec_u8;
typedef VEC(u32) vec_u32;
typedef VEC(char) vec_char;

/* -------------------------------------------------------------------------- */
/* UTF-8                                                                      */
/* -------------------------------------------------------------------------- */
API_EXPORT u32   utf8_decode_1(const char* s, usize n, usize* adv);
API_EXPORT usize utf8_encode_1(u32 cp, char out[4]);

/* -------------------------------------------------------------------------- */
/* Fichiers                                                                   */
/* -------------------------------------------------------------------------- */
API_EXPORT Err  file_read_all (const char* path, vec_u8* out);
API_EXPORT Err  file_write_all(const char* path, const void* data, usize n);
API_EXPORT bool file_exists   (const char* path);

/* -------------------------------------------------------------------------- */
/* Chemins / dossiers                                                         */
/* -------------------------------------------------------------------------- */
API_EXPORT void path_join(const char* a, const char* b, char* out, usize out_cap);
API_EXPORT Err  dir_ensure(const char* path);

/* -------------------------------------------------------------------------- */
/* Hash                                                                       */
/* -------------------------------------------------------------------------- */
API_EXPORT u64 hash64(const void* data, usize n);
API_EXPORT u64 hash_str(const char* s);

/* -------------------------------------------------------------------------- */
/* Hash map string -> u64                                                     */
/* -------------------------------------------------------------------------- */
struct HSlot;
typedef struct {
  struct HSlot* slots;
  usize         cap;
  usize         len;
} MapStrU64;

API_EXPORT void map_init(MapStrU64* m);
API_EXPORT void map_free(MapStrU64* m);
API_EXPORT void map_put (MapStrU64* m, const char* key, u64 val);
API_EXPORT bool map_get (const MapStrU64* m, const char* key, u64* out_val);

/* -------------------------------------------------------------------------- */
/* Arena allocator (bump)                                                     */
/* -------------------------------------------------------------------------- */
typedef struct Arena {
  u8*    base;
  usize  cap;
  usize  off;
} Arena;

API_EXPORT Arena arena_new(usize cap);
API_EXPORT void  arena_free(Arena* a);
API_EXPORT void  arena_reset(Arena* a);
API_EXPORT void* arena_alloc(Arena* a, usize n, usize align);
API_EXPORT char* arena_strdup(Arena* a, const char* s);

/* -------------------------------------------------------------------------- */
/* JSON writer                                                                */
/* -------------------------------------------------------------------------- */
typedef struct {
  StrBuf sb;
  int    depth;
  bool   first_in_level[64];
} JsonW;

API_EXPORT void jw_begin(JsonW* w);
API_EXPORT void jw_free(JsonW* w);
API_EXPORT void jw_obj_begin(JsonW* w);
API_EXPORT void jw_obj_end(JsonW* w);
API_EXPORT void jw_arr_begin(JsonW* w);
API_EXPORT void jw_arr_end(JsonW* w);
API_EXPORT void jw_key(JsonW* w, const char* k);
API_EXPORT void jw_str(JsonW* w, const char* v);
API_EXPORT void jw_i64(JsonW* w, i64 v);
API_EXPORT void jw_f64(JsonW* w, f64 v);
API_EXPORT void jw_bool(JsonW* w, bool v);
API_EXPORT void jw_null(JsonW* w);
API_EXPORT const char* jw_cstr(JsonW* w);

/* -------------------------------------------------------------------------- */
/* Environnement                                                              */
/* -------------------------------------------------------------------------- */
API_EXPORT const char* env_get(const char* key);

/* -------------------------------------------------------------------------- */
/* ANSI helpers                                                               */
/* -------------------------------------------------------------------------- */
API_EXPORT const char* ansi_reset(void);
API_EXPORT const char* ansi_bold(void);
API_EXPORT const char* ansi_red(void);
API_EXPORT const char* ansi_green(void);
API_EXPORT const char* ansi_yellow(void);
API_EXPORT const char* ansi_blue(void);
API_EXPORT void ansi_paint_to(StrBuf* out, const char* text, const char* pre);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_API_H */