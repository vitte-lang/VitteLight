// vitte-light/core/object.c
// Objets runtime VitteLight: chaînes, tableaux, maps (hash) et utilitaires.
// Conçu pour s'intégrer à api.c sans dépendance cyclique.
//
// Fournit:
//   Chaînes (VL_String*)
//     - vl_make_strn(ctx, s, n) -> VL_Value (VT_STR)
//     - vl_make_str (ctx, cstr)  -> VL_Value (VT_STR)
//     - vl_string_eq(a,b), vl_string_hash(s)
//   Tableaux (VL_Array)
//     - vl_array_init/clear/push/pop/get/set/reserve
//   Maps (VL_Map)  (clés: VL_String*, valeurs: VL_Value)
//     - vl_map_init/clear/get/put/del/rehash, et helpers d'itération simple
//
// Notes:
//  - S'appuie uniquement sur api.h (VL_Context, VL_Value, VL_Status) et ctype.h
//  pour helpers.
//  - Si gc.h est présent, enregistre les chaînes allouées via
//  vl_gc_on_string_alloc().
//  - Hash: FNV-1a 32-bit, stable little-endian.
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/object.c

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "ctype.h"
#if __has_include("gc.h")
#include "gc.h"
#endif

// ───────────────────────── Types internes (sync api.c)
// ───────────────────────── Ces redéclarations doivent rester strictement
// cohérentes avec api.c.

#ifndef VL_INTERNAL_TYPES_DECLARED
#define VL_INTERNAL_TYPES_DECLARED 1

typedef struct VL_String {
  uint32_t hash, len;
  char data[];
} VL_String;

typedef struct {
  VL_Value *data;
  size_t len, cap;
} VL_Array;

typedef struct {
  VL_String **keys;
  VL_Value *vals;
  size_t cap, len, tomb;
} VL_Map;

#endif  // VL_INTERNAL_TYPES_DECLARED

// Sentinelle de tombstone pour les tables ouvertes
#define VL_TOMBSTONE ((VL_String *)(uintptr_t)1)

// ───────────────────────── Hash util ─────────────────────────
static uint32_t vl_hash_bytes(const void *p, size_t n) {
  const uint8_t *s = (const uint8_t *)p;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= s[i];
    h *= 16777619u;
  }
  return h ? h : 1u;
}
static inline uint32_t vl_string_hash(const char *s, size_t n) {
  return vl_hash_bytes(s, n);
}

// ───────────────────────── Chaînes ─────────────────────────
static VL_String *vl_string_new(size_t n) {
  size_t bytes = sizeof(VL_String) + n + 1;
  VL_String *st = (VL_String *)malloc(bytes);
  if (!st) return NULL;
  st->hash = 0;
  st->len = (uint32_t)n;
  st->data[n] = '\0';
  return st;
}

// Exposées via api.h: fabriques VL_Value
VL_Value vl_make_strn(struct VL_Context *ctx, const char *s, size_t n) {
  VL_Value out = vlv_nil();
  if (!s) return out;
  if (n > (size_t)0xFFFFFFFFu) return out;
  VL_String *st = vl_string_new(n);
  if (!st) return out;
  memcpy(st->data, s, n);
  st->hash = vl_string_hash(st->data, n);
#ifdef vl_gc_on_string_alloc
  vl_gc_on_string_alloc(ctx, st);
#elif defined(VITTE_LIGHT_CORE_GC_H)
  vl_gc_on_string_alloc(ctx, st);
#endif
  out.type = VT_STR;
  out.as.s = st;
  return out;
}
VL_Value vl_make_str(struct VL_Context *ctx, const char *cstr) {
  if (!cstr) return vlv_nil();
  return vl_make_strn(ctx, cstr, strlen(cstr));
}

int vl_string_eq(const VL_String *a, const VL_String *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  if (a->hash != b->hash || a->len != b->len) return 0;
  return memcmp(a->data, b->data, a->len) == 0;
}

// ───────────────────────── Tableaux ─────────────────────────
static int vl_array_reserve(VL_Array *ar, size_t need) {
  if (need <= ar->cap) return 1;
  size_t cap = ar->cap ? ar->cap * 2 : 8;
  while (cap < need) cap = cap + cap / 2;
  VL_Value *p = (VL_Value *)realloc(ar->data, cap * sizeof(VL_Value));
  if (!p) return 0;
  ar->data = p;
  ar->cap = cap;
  return 1;
}
void vl_array_init(VL_Array *ar) { memset(ar, 0, sizeof(*ar)); }
void vl_array_clear(VL_Array *ar) {
  if (!ar) return;
  free(ar->data);
  memset(ar, 0, sizeof(*ar));
}
int vl_array_push(VL_Array *ar, VL_Value v) {
  if (!vl_array_reserve(ar, ar->len + 1)) return 0;
  ar->data[ar->len++] = v;
  return 1;
}
int vl_array_pop(VL_Array *ar, VL_Value *out) {
  if (ar->len == 0) return 0;
  VL_Value v = ar->data[--ar->len];
  if (out) *out = v;
  return 1;
}
int vl_array_get(VL_Array *ar, size_t i, VL_Value *out) {
  if (i >= ar->len) return 0;
  if (out) *out = ar->data[i];
  return 1;
}
int vl_array_set(VL_Array *ar, size_t i, VL_Value v) {
  if (i >= ar->len) return 0;
  ar->data[i] = v;
  return 1;
}
size_t vl_array_len(const VL_Array *ar) { return ar ? ar->len : 0; }

// ───────────────────────── Maps (open addressing, Robin Hood light)
// ─────────────────────────
static size_t vl__map_ideal_slot(size_t cap, uint32_t hash) {
  return (size_t)(hash % (uint32_t)cap);
}
static int vl__is_empty_key(VL_String *k) {
  return k == NULL || k == VL_TOMBSTONE;
}

static int vl_map_rehash(VL_Map *m, size_t new_cap) {
  VL_String **nkeys = (VL_String **)calloc(new_cap, sizeof(VL_String *));
  VL_Value *nvals = (VL_Value *)calloc(new_cap, sizeof(VL_Value));
  if (!nkeys || !nvals) {
    free(nkeys);
    free(nvals);
    return 0;
  }
  size_t nlen = 0, ntomb = 0;
  for (size_t i = 0; i < m->cap; i++) {
    VL_String *k = m->keys[i];
    if (!k || k == VL_TOMBSTONE) continue;
    VL_Value v = m->vals[i];
    size_t mask = new_cap;  // open addressing
    size_t idx = (size_t)(k->hash % (uint32_t)new_cap);
    while (nkeys[idx]) {
      idx = (idx + 1 == new_cap) ? 0 : idx + 1;
    }
    nkeys[idx] = k;
    nvals[idx] = v;
    nlen++;
  }
  free(m->keys);
  free(m->vals);
  m->keys = nkeys;
  m->vals = nvals;
  m->cap = new_cap;
  m->len = nlen;
  m->tomb = ntomb;
  return 1;
}

void vl_map_init(VL_Map *m, size_t initial_cap) {
  memset(m, 0, sizeof(*m));
  size_t cap = 8;
  while (cap < initial_cap) cap <<= 1;
  m->keys = (VL_String **)calloc(cap, sizeof(VL_String *));
  m->vals = (VL_Value *)calloc(cap, sizeof(VL_Value));
  if (!m->keys || !m->vals) {
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof(*m));
  }
}
void vl_map_clear(VL_Map *m) {
  if (!m) return;
  free(m->keys);
  free(m->vals);
  memset(m, 0, sizeof(*m));
}

static int vl__map_maybe_grow(VL_Map *m) {
  size_t live = m->len + m->tomb;
  if (m->cap == 0) {
    vl_map_init(m, 8);
    return m->cap != 0;
  }
  if (live * 100 >= m->cap * 70) {
    return vl_map_rehash(m, m->cap ? m->cap * 2 : 8);
  }
  return 1;
}

int vl_map_put(VL_Map *m, VL_String *key, VL_Value val) {
  if (!key) return 0;
  if (!vl__map_maybe_grow(m)) return 0;
  size_t idx = (size_t)(key->hash % (uint32_t)m->cap);
  size_t first_tomb = (size_t)-1;
  for (;;) {
    VL_String *k = m->keys[idx];
    if (!k) {  // empty slot
      size_t use = (first_tomb != (size_t)-1) ? first_tomb : idx;
      if (m->keys[use] == VL_TOMBSTONE) {
        m->tomb--;
      }
      m->keys[use] = key;
      m->vals[use] = val;
      m->len++;
      return 1;
    }
    if (k == VL_TOMBSTONE) {
      if (first_tomb == (size_t)-1) first_tomb = idx;
    } else if (k == key || (k->hash == key->hash && k->len == key->len &&
                            memcmp(k->data, key->data, k->len) == 0)) {
      m->vals[idx] = val;
      return 1;
    }
    idx = (idx + 1 == m->cap) ? 0 : idx + 1;
  }
}

int vl_map_get(const VL_Map *m, const VL_String *key, VL_Value *out) {
  if (!m || !m->cap || !key) return 0;
  size_t idx = (size_t)(key->hash % (uint32_t)m->cap);
  for (;;) {
    VL_String *k = m->keys[idx];
    if (!k) return 0;
    if (k != VL_TOMBSTONE && k->hash == key->hash && k->len == key->len &&
        memcmp(k->data, key->data, k->len) == 0) {
      if (out) *out = m->vals[idx];
      return 1;
    }
    idx = (idx + 1 == m->cap) ? 0 : idx + 1;
  }
}

int vl_map_del(VL_Map *m, const VL_String *key) {
  if (!m || !m->cap || !key) return 0;
  size_t idx = (size_t)(key->hash % (uint32_t)m->cap);
  for (;;) {
    VL_String *k = m->keys[idx];
    if (!k) return 0;
    if (k != VL_TOMBSTONE && k->hash == key->hash && k->len == key->len &&
        memcmp(k->data, key->data, k->len) == 0) {
      m->keys[idx] = VL_TOMBSTONE;
      m->vals[idx] = vlv_nil();
      m->len--;
      m->tomb++;
      return 1;
    }
    idx = (idx + 1 == m->cap) ? 0 : idx + 1;
  }
}

// Itération linéaire simple: renvoie l'index suivant occupé ou -1 si fini
ssize_t vl_map_next(const VL_Map *m, ssize_t i) {
  if (!m || !m->cap) return -1;
  size_t k = (i < 0) ? 0 : (size_t)(i + 1);
  for (; k < m->cap; k++) {
    VL_String *s = m->keys[k];
    if (s && s != VL_TOMBSTONE) return (ssize_t)k;
  }
  return -1;
}

// ───────────────────────── Aides haut niveau autour de VL_Value
// ───────────────────────── Conversions string<->VL_Value et accès aux maps via
// chaînes C.

static VL_String *vl_intern_unsafe(struct VL_Context *ctx, const char *s,
                                   size_t n) {
  (void)ctx;
  VL_String *st = vl_string_new(n);
  if (!st) return NULL;
  memcpy(st->data, s, n);
  st->hash = vl_string_hash(s, n);
  st->data[n] = '\0';
#ifdef vl_gc_on_string_alloc
  vl_gc_on_string_alloc(ctx, st);
#elif defined(VITTE_LIGHT_CORE_GC_H)
  vl_gc_on_string_alloc(ctx, st);
#endif
  return st;
}

int vl_map_put_cstr(VL_Map *m, struct VL_Context *ctx, const char *key,
                    VL_Value v) {
  VL_String *ks = vl_intern_unsafe(ctx, key, strlen(key));
  if (!ks) return 0;
  return vl_map_put(m, ks, v);
}
int vl_map_get_cstr(const VL_Map *m, struct VL_Context *ctx, const char *key,
                    VL_Value *out) {
  (void)ctx;
  VL_String tmp;
  tmp.hash = vl_string_hash(key, strlen(key));
  tmp.len = (uint32_t)strlen(key);
  tmp.data = (char *)key;
  return vl_map_get(m, &tmp, out);
}
int vl_map_del_cstr(VL_Map *m, struct VL_Context *ctx, const char *key) {
  (void)ctx;
  VL_String tmp;
  tmp.hash = vl_string_hash(key, strlen(key));
  tmp.len = (uint32_t)strlen(key);
  tmp.data = (char *)key;
  return vl_map_del(m, &tmp);
}

// ───────────────────────── Tests ─────────────────────────
#ifdef VL_OBJECT_TEST_MAIN
static void t_map(void) {
  VL_Map m;
  vl_map_init(&m, 4);
  VL_Context *ctx = NULL;  // pas nécessaire pour ce test
  for (int i = 0; i < 1000; i++) {
    char k[32];
    snprintf(k, sizeof(k), "k%d", i);
    VL_Value v = vlv_int(i);
    vl_map_put_cstr(&m, ctx, k, v);
  }
  for (int i = 0; i < 1000; i++) {
    char k[32];
    snprintf(k, sizeof(k), "k%d", i);
    VL_Value out;
    int ok = vl_map_get_cstr(&m, ctx, k, &out);
    assert(ok && out.type == VT_INT && out.as.i == i);
  }
  ssize_t it = -1;
  size_t count = 0;
  while ((it = vl_map_next(&m, it)) >= 0) {
    count++;
  }
  assert(count == m.len);
  vl_map_clear(&m);
}
static void t_array(void) {
  VL_Array a;
  vl_array_init(&a);
  for (int i = 0; i < 100; i++) vl_array_push(&a, vlv_int(i));
  for (int i = 0; i < 100; i++) {
    VL_Value t;
    vl_array_get(&a, (size_t)i, &t);
    assert(t.as.i == i);
  }
  for (int i = 0; i < 100; i++) {
    VL_Value t;
    vl_array_pop(&a, &t);
  }
  assert(a.len == 0);
  vl_array_clear(&a);
}
int main(void) {
  t_map();
  t_array();
  puts("ok");
  return 0;
}
#endif
