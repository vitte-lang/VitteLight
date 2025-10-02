/* ============================================================================
   core/arena.c — Allocateur « bump » C11 ultra complet pour Vitte/Vitl
   Mono-fichier autonome. Zéro dépendance hors libc.
   Plateformes: POSIX (Linux/macOS), Windows.
   ============================================================================
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#endif

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
typedef int8_t  i8;   typedef uint8_t  u8;
typedef int16_t i16;  typedef uint16_t u16;
typedef int32_t i32;  typedef uint32_t u32;
typedef int64_t i64;  typedef uint64_t u64;
typedef float   f32;  typedef double   f64;
typedef size_t  usize;typedef ptrdiff_t isize;

/* --------------------------------------------------------------------------
   Utilitaires généraux
   -------------------------------------------------------------------------- */
#define ALIGN_UP(x, a)   ( ((x) + ((a) - 1)) & ~((a) - 1) )
#define ARR_LEN(a)       ((isize)(sizeof(a)/sizeof((a)[0])))

static inline void* xmalloc(usize n) {
  void* p = malloc(n ? n : 1);
  if (!p) { fprintf(stderr, "arena: OOM (%zu)\n", (size_t)n); abort(); }
  return p;
}

/* --------------------------------------------------------------------------
   Configuration compile-time
   -------------------------------------------------------------------------- */
#ifndef ARENA_DEFAULT_CAP
#define ARENA_DEFAULT_CAP (1u << 20) /* 1 MiB si cap==0 */
#endif

#ifndef ARENA_ZERO_ON_ALLOC
#define ARENA_ZERO_ON_ALLOC 0 /* 1 => memset 0 les blocs retournés */
#endif

#ifndef ARENA_POISON_RESET
#define ARENA_POISON_RESET 1 /* 1 => motif sur reset() */
#endif

#ifndef ARENA_POISON_FREE
#define ARENA_POISON_FREE 1 /* 1 => motif sur free() */
#endif

#ifndef ARENA_STATS
#define ARENA_STATS 0       /* 1 => stats globales */
#endif

#define ARENA_POISON_VAL 0xA5
#define ARENA_FREE_VAL   0xDD

/* --------------------------------------------------------------------------
   Erreur simple (optionnel pour logs)
   -------------------------------------------------------------------------- */
typedef struct Err { int code; char msg[256]; } Err;
static inline Err ok(void){ Err e; e.code=0; e.msg[0]=0; return e; }

/* --------------------------------------------------------------------------
   Arena — API publique attendue par core/api.h
   -------------------------------------------------------------------------- */
typedef struct Arena {
  u8*   base;
  usize cap;
  usize off;
} Arena;

/* helpers internes */
static inline bool is_pow2(usize x){ return x && ((x & (x-1))==0); }

/* C11: _Alignof(max_align_t) garantit un alignement sûr pour types standards */
static inline usize norm_align(usize a) {
  if (a == 0) return _Alignof(max_align_t);
  if (!is_pow2(a)) return _Alignof(max_align_t);
  return a;
}

/* addition bornée */
static inline bool add_ok(usize a, usize b, usize* out){
  if ((SIZE_MAX - a) < b) return false;
  *out = a + b; return true;
}

/* --------------------------------------------------------------------------
   Stats globales (optionnel)
   -------------------------------------------------------------------------- */
#if ARENA_STATS
typedef struct {
  usize peak;   /* offset max atteint */
  usize total;  /* octets délivrés au total */
  usize calls;  /* nb d'allocations */
  usize resets; /* nb de reset */
} ArenaStats;
static ArenaStats g_astats = {0,0,0,0};
static inline void stats_alloc(Arena* a, usize before){
  (void)a;
  g_astats.calls++;
  if (a->off > g_astats.peak) g_astats.peak = a->off;
  g_astats.total += (a->off - before);
}
static inline void stats_reset(void){ g_astats.resets++; }
API_EXPORT void arena_stats_get(usize* peak, usize* total, usize* calls, usize* resets){
  if (peak)   *peak   = g_astats.peak;
  if (total)  *total  = g_astats.total;
  if (calls)  *calls  = g_astats.calls;
  if (resets) *resets = g_astats.resets;
}
#else
static inline void stats_alloc(Arena* a, usize before){ (void)a; (void)before; }
static inline void stats_reset(void){}
#endif

/* --------------------------------------------------------------------------
   Implémentation
   -------------------------------------------------------------------------- */
API_EXPORT Arena arena_new(usize cap) {
  Arena a; a.base=NULL; a.cap=0; a.off=0;
  if (cap == 0) cap = ARENA_DEFAULT_CAP;
  a.base = (u8*)malloc(cap);
  if (!a.base) { fprintf(stderr, "arena_new: OOM (%zu)\n", (size_t)cap); return a; }
  a.cap = cap; a.off = 0;
#if ARENA_POISON_RESET
  memset(a.base, ARENA_POISON_VAL, a.cap);
#endif
  return a;
}

API_EXPORT void arena_free(Arena* a) {
  if (!a) return;
#if ARENA_POISON_FREE
  if (a->base && a->cap) memset(a->base, ARENA_FREE_VAL, a->cap);
#endif
  free(a->base);
  a->base=NULL; a->cap=0; a->off=0;
}

API_EXPORT void arena_reset(Arena* a) {
  if (!a || !a->base) return;
#if ARENA_POISON_RESET
  if (a->off) memset(a->base, ARENA_POISON_VAL, a->off);
#endif
  a->off = 0;
  stats_reset();
}

API_EXPORT void* arena_alloc(Arena* a, usize n, usize align) {
  if (!a || !a->base) { fprintf(stderr, "arena_alloc: arena invalide\n"); return NULL; }
  if (n == 0) n = 1;
  align = norm_align(align);
  usize aligned = ALIGN_UP(a->off, align);

  usize end = 0;
  if (!add_ok(aligned, n, &end) || end > a->cap) {
    fprintf(stderr, "arena_alloc: OOM need=%zu align=%zu off=%zu cap=%zu\n",
            (size_t)n, (size_t)align, (size_t)a->off, (size_t)a->cap);
    return NULL;
  }

  void* p = a->base + aligned;
#if ARENA_ZERO_ON_ALLOC
  memset(p, 0, n);
#endif

  usize before = a->off;
  a->off = end;
  stats_alloc(a, before);
  return p;
}

API_EXPORT char* arena_strdup(Arena* a, const char* s) {
  if (!s) return NULL;
  usize len = (usize)strlen(s) + 1;
  char* d = (char*)arena_alloc(a, len, 1);
  if (!d) return NULL;
  memcpy(d, s, len);
  return d;
}

/* --------------------------------------------------------------------------
   Diagnostic optionnel
   -------------------------------------------------------------------------- */
API_EXPORT usize arena_capacity(const Arena* a){ return a ? a->cap : 0; }
API_EXPORT usize arena_offset  (const Arena* a){ return a ? a->off : 0; }
API_EXPORT bool  arena_valid   (const Arena* a){ return a && a->base && a->cap; }

/* --------------------------------------------------------------------------
   Démo locale (désactivée par défaut)
   -------------------------------------------------------------------------- */
#ifdef ARENA_DEMO_MAIN
static void check_align(void* p, usize a){
  if (a) {
    if (((uintptr_t)p) % a != 0)
      fprintf(stderr, "bad align: %p %% %zu != 0\n", p, (size_t)a);
  }
}
int main(void){
  Arena A = arena_new(4096);
  if (!arena_valid(&A)) return 1;

  void* p1 = arena_alloc(&A, 3, 1);   check_align(p1, 1);
  void* p2 = arena_alloc(&A, 8, 8);   check_align(p2, 8);
  void* p3 = arena_alloc(&A, 16, 32); check_align(p3, 32);

  char* s1 = arena_strdup(&A, "hello");
  char* s2 = arena_strdup(&A, "world");
  printf("%s %s\n", s1, s2);

  printf("off=%zu cap=%zu\n", (size_t)arena_offset(&A), (size_t)arena_capacity(&A));
  arena_reset(&A);
  printf("after reset off=%zu\n", (size_t)arena_offset(&A));

  char* s3 = arena_strdup(&A, "reset-ok");
  printf("%s\n", s3);

  arena_free(&A);
  return 0;
}
#endif

/* ========================================================================== */
