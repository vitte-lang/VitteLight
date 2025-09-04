// vitte-light/libraries/baselib.c
// Bibliothèque de base VitteLight: natives utilitaires orientées chaînes,
// conversions, temps, fichiers et aléa. Sans dépendance forte au runtime VM.
//
// Fournit: void vl_register_baselib(struct VL_Context *ctx);
// Natives exposées au niveau global:
//   print        (déjà dans vm.c), ici on ajoute:
//   println(x,...)         -> nil                 // alias qui force un '\n'
//   len(s:str)             -> int                 // longueur bytes
//   substr(s, start[, n])  -> str                 // sous-chaîne bornée
//   find(s, sub)           -> int                 // index ou -1
//   lower(s) / upper(s)    -> str                 // ASCII
//   trim(s)                -> str                 // ASCII <= 0x20
//   int(x) / float(x) / bool(x) / str(x)         // conversions de base
//   clock_ns()             -> int                 // horloge monotone ns
//   sleep_ms(ms)           -> nil                 // veille
//   readfile(path)         -> str (binaire ok)    // charge fichier
//   writefile(path, data)  -> bool                // écrit fichier
//   hexdump(data)          -> nil                 // trace hex sur stdout
//   rand_u32()             -> int                 // PRNG xorshift
//   srand(seed_u64)        -> nil                 // seed PRNG
//   assert(cond[, msg])    -> nil | erreur        // VL_ERR_INVAL si faux
//   panic(msg)             -> erreur              // VL_ERR_INVAL
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/baselib.c

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>  // libc

#include "api.h"
#include "ctype.h"
#include "mem.h"     // VL_Buffer pour str(x)
#include "string.h"  // VL_String, vl_make_strn
#include "tm.h"
#include "vm.h"  // vl_register_native
#include "zio.h"

// ───────────────────────── Helpers ─────────────────────────
#define RET_NIL()                \
  do {                           \
    if (ret) *(ret) = vlv_nil(); \
    return VL_OK;                \
  } while (0)
#define RET_INT(v)                           \
  do {                                       \
    if (ret) *(ret) = vlv_int((int64_t)(v)); \
    return VL_OK;                            \
  } while (0)
#define RET_FLOAT(v)                          \
  do {                                        \
    if (ret) *(ret) = vlv_float((double)(v)); \
    return VL_OK;                             \
  } while (0)
#define RET_BOOL(v)                       \
  do {                                    \
    if (ret) *(ret) = vlv_bool((v) != 0); \
    return VL_OK;                         \
  } while (0)

static inline VL_Value make_cstr(struct VL_Context *ctx, const char *s) {
  return vl_make_strn(ctx, s ? s : "", (uint32_t)(s ? strlen(s) : 0));
}
static inline VL_Value make_bytes(struct VL_Context *ctx, const void *p,
                                  size_t n) {
  return vl_make_strn(ctx, (const char *)p, (uint32_t)n);
}

static int as_int(const VL_Value *v, int64_t *out) {
  return vl_value_as_int(v, out);
}
static int as_float(const VL_Value *v, double *out) {
  return vl_value_as_float(v, out);
}

// ───────────────────────── PRNG ─────────────────────────
static uint64_t g_rng = 88172645463393265ull;  // seed par défaut
static inline uint32_t xorshift32(void) {
  static uint32_t s = 0x12345678u;
  uint32_t x = s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s = x;
  return x;
}
static inline uint64_t xorshift64(void) {
  uint64_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_rng = x;
  return x;
}

// ───────────────────────── Natives impl ─────────────────────────
static VL_Status nb_println(struct VL_Context *ctx, const VL_Value *args,
                            uint8_t argc, VL_Value *ret, void *ud) {
  (void)ud;
  FILE *out = stdout;
  for (uint8_t i = 0; i < argc; i++) {
    vl_value_print(&args[i], out);
    if (i + 1 < argc) fputc(' ', out);
  }
  fputc('\n', out);
  RET_NIL();
}

static VL_Status nb_len(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 1 || a[0].type != VT_STR || !a[0].as.s) return VL_ERR_TYPE;
  RET_INT(a[0].as.s->len);
}

static VL_Status nb_substr(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *u) {
  (void)u;
  if (c < 2) return VL_ERR_INVAL;
  if (a[0].type != VT_STR || !a[0].as.s) return VL_ERR_TYPE;
  int64_t st = 0, len = -1;
  if (!as_int(&a[1], &st)) return VL_ERR_TYPE;
  if (c >= 3 && !as_int(&a[2], &len)) return VL_ERR_TYPE;
  int64_t L = (int64_t)a[0].as.s->len;
  if (st < 0) st = 0;
  if (st > L) st = L;
  int64_t n = (len < 0) ? (L - st) : len;
  if (n < 0) n = 0;
  if (st + n > L) n = L - st;
  VL_Value s = vl_make_strn(ctx, a[0].as.s->data + st, (uint32_t)n);
  if (s.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = s;
  return VL_OK;
}

static VL_Status nb_find(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 2) return VL_ERR_INVAL;
  if (a[0].type != VT_STR || !a[0].as.s) return VL_ERR_TYPE;
  if (a[1].type != VT_STR || !a[1].as.s) return VL_ERR_TYPE;
  const char *h = a[0].as.s->data;
  size_t hn = a[0].as.s->len;
  const char *n = a[1].as.s->data;
  size_t nn = a[1].as.s->len;
  if (nn == 0) RET_INT(0);  // vide
  // recherche naïve
  for (size_t i = 0; i + nn <= hn; i++) {
    if (memcmp(h + i, n, nn) == 0) {
      RET_INT((int64_t)i);
    }
  }
  RET_INT(-1);
}

static VL_Status nb_lower(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || a[0].type != VT_STR || !a[0].as.s) return VL_ERR_TYPE;
  size_t L = a[0].as.s->len;
  char *tmp = (char *)malloc(L);
  if (!tmp) return VL_ERR_OOM;
  for (size_t i = 0; i < L; i++) {
    unsigned char ch = a[0].as.s->data[i];
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    tmp[i] = (char)ch;
  }
  VL_Value s = make_bytes(ctx, tmp, L);
  free(tmp);
  if (s.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = s;
  return VL_OK;
}
static VL_Status nb_upper(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || a[0].type != VT_STR || !a[0].as.s) return VL_ERR_TYPE;
  size_t L = a[0].as.s->len;
  char *tmp = (char *)malloc(L);
  if (!tmp) return VL_ERR_OOM;
  for (size_t i = 0; i < L; i++) {
    unsigned char ch = a[0].as.s->data[i];
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    tmp[i] = (char)ch;
  }
  VL_Value s = make_bytes(ctx, tmp, L);
  free(tmp);
  if (s.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = s;
  return VL_OK;
}

static VL_Status nb_trim(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || a[0].type != VT_STR || !a[0].as.s) return VL_ERR_TYPE;
  const char *s = a[0].as.s->data;
  size_t L = a[0].as.s->len;
  size_t i = 0, j = L;
  while (i < j && (unsigned char)s[i] <= 0x20) i++;
  while (j > i && (unsigned char)s[j - 1] <= 0x20) j--;
  VL_Value v = make_bytes(ctx, s + i, j - i);
  if (v.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = v;
  return VL_OK;
}

static VL_Status nb_to_int(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 1) return VL_ERR_INVAL;
  int64_t v;
  if (!as_int(&a[0], &v)) return VL_ERR_TYPE;
  RET_INT(v);
}
static VL_Status nb_to_float(struct VL_Context *ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 1) return VL_ERR_INVAL;
  double v;
  if (!as_float(&a[0], &v)) return VL_ERR_TYPE;
  RET_FLOAT(v);
}
static VL_Status nb_to_bool(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 1) return VL_ERR_INVAL;
  RET_BOOL(vl_value_truthy(&a[0]));
}

static VL_Status nb_to_str(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *u) {
  (void)u;
  if (c < 1) {
    RET_NIL();
  }
  if (a[0].type == VT_STR) {
    if (ret) *ret = a[0];
    return VL_OK;
  }
  char tmp[256];
  size_t n = vl_value_to_cstr(&a[0], tmp, sizeof(tmp));
  if (n < sizeof(tmp) - 1) {
    VL_Value v = make_cstr(ctx, tmp);
    if (ret) *ret = v;
    return (v.type == VT_STR) ? VL_OK : VL_ERR_OOM;
  }
  // chemin long
  VL_Buffer b;
  vl_buf_init(&b);
  char buf[256];
  size_t k = vl_value_to_cstr(&a[0], buf, sizeof(buf));
  vl_buf_append(&b, buf, k);
  VL_Value v = make_bytes(ctx, b.d, b.n);
  vl_buf_free(&b);
  if (ret) *ret = v;
  return (v.type == VT_STR) ? VL_OK : VL_ERR_OOM;
}

static VL_Status nb_clockns(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)u;
  RET_INT((int64_t)vl_mono_time_ns());
}
static VL_Status nb_sleepms(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  (void)ctx;
  if (c < 1) return VL_ERR_INVAL;
  int64_t ms = 0;
  if (!as_int(&a[0], &ms)) return VL_ERR_TYPE;
  if (ms < 0) ms = 0;
  vl_sleep_ms((uint32_t)ms);
  RET_NIL();
}

static VL_Status nb_readfile(struct VL_Context *ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  if (c < 1 || a[0].type != VT_STR || !a[0].as.s) return VL_ERR_TYPE;
  uint8_t *buf = NULL;
  size_t n = 0;
  if (!vl_read_file_all(a[0].as.s->data, &buf, &n)) return VL_ERR_IO;
  VL_Value v = make_bytes(ctx, buf, n);
  free(buf);
  if (v.type != VT_STR) return VL_ERR_OOM;
  if (ret) *ret = v;
  return VL_OK;
}
static VL_Status nb_writefile(struct VL_Context *ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *u) {
  (void)u;
  (void)ctx;
  if (c < 2 || a[0].type != VT_STR || !a[0].as.s || a[1].type != VT_STR ||
      !a[1].as.s)
    return VL_ERR_TYPE;
  const char *path = a[0].as.s->data;
  FILE *f = fopen(path, "wb");
  if (!f) {
    RET_BOOL(0);
  }
  size_t wr = fwrite(a[1].as.s->data, 1, a[1].as.s->len, f);
  int ok = (wr == a[1].as.s->len && fflush(f) == 0 && fclose(f) == 0);
  if (!ok) fclose(f);
  RET_BOOL(ok);
}

static VL_Status nb_hexdump(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 1 || a[0].type != VT_STR || !a[0].as.s) {
    return VL_ERR_TYPE;
  }
  vl_hexdump(a[0].as.s->data, a[0].as.s->len, 0, stdout);
  RET_NIL();
}

static VL_Status nb_rand_u32(struct VL_Context *ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *u) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)u;
  uint32_t x = (uint32_t)xorshift64();
  RET_INT((int64_t)x);
}
static VL_Status nb_srand(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)ctx;
  (void)u;
  if (c < 1) return VL_ERR_INVAL;
  int64_t s = 0;
  if (!as_int(&a[0], &s)) return VL_ERR_TYPE;
  g_rng = (uint64_t)s ?: 88172645463393265ull;
  RET_NIL();
}

static VL_Status nb_assert(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *u) {
  (void)u;
  if (c < 1) return VL_ERR_INVAL;
  int ok = vl_value_truthy(&a[0]);
  if (ok) {
    RET_NIL();
  }
  const char *msg = (c >= 2 && a[1].type == VT_STR && a[1].as.s)
                        ? a[1].as.s->data
                        : "assertion failed";
  fprintf(stderr, "assert: %s\n", msg);
  return VL_ERR_INVAL;
}
static VL_Status nb_panic(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *u) {
  (void)ctx;
  (void)ret;
  (void)u;
  const char *msg =
      (c >= 1 && a[0].type == VT_STR && a[0].as.s) ? a[0].as.s->data : "panic";
  fprintf(stderr, "panic: %s\n", msg);
  return VL_ERR_INVAL;
}

// ───────────────────────── Enregistrement ─────────────────────────
void vl_register_baselib(struct VL_Context *ctx) {
  if (!ctx) return;
  vl_register_native(ctx, "println", nb_println, NULL);
  vl_register_native(ctx, "len", nb_len, NULL);
  vl_register_native(ctx, "substr", nb_substr, NULL);
  vl_register_native(ctx, "find", nb_find, NULL);
  vl_register_native(ctx, "lower", nb_lower, NULL);
  vl_register_native(ctx, "upper", nb_upper, NULL);
  vl_register_native(ctx, "trim", nb_trim, NULL);
  vl_register_native(ctx, "int", nb_to_int, NULL);
  vl_register_native(ctx, "float", nb_to_float, NULL);
  vl_register_native(ctx, "bool", nb_to_bool, NULL);
  vl_register_native(ctx, "str", nb_to_str, NULL);
  vl_register_native(ctx, "clock_ns", nb_clockns, NULL);
  vl_register_native(ctx, "sleep_ms", nb_sleepms, NULL);
  vl_register_native(ctx, "readfile", nb_readfile, NULL);
  vl_register_native(ctx, "writefile", nb_writefile, NULL);
  vl_register_native(ctx, "hexdump", nb_hexdump, NULL);
  vl_register_native(ctx, "rand_u32", nb_rand_u32, NULL);
  vl_register_native(ctx, "srand", nb_srand, NULL);
  vl_register_native(ctx, "assert", nb_assert, NULL);
  vl_register_native(ctx, "panic", nb_panic, NULL);
}

// ───────────────────────── Test rapide ─────────────────────────
#ifdef VL_BASELIB_TEST_MAIN
int main(void) {
  VL_Value a = vlv_int(42);
  VL_Value b = vlv_float(3.5);
  VL_Value s;
  char buf[64];
  VL_Value v = vlv_bool(1);
  vl_value_to_cstr(&v, buf, sizeof(buf));
  puts(buf);
  (void)a;
  (void)b;
  (void)s;
  return 0;
}
#endif
