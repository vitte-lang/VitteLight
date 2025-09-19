/* ============================================================================
   object.c — Système d’objets runtime (C17) pour Vitte/Vitl
   - Tagged values: Nil, Bool, Int, Float, String, Bytes, Array, Map, Func, Ptr
   - Strings/Bytes dynamiques avec hash, utf-8 agnostique
   - Array dynamique (vt_array), Map string→value (open addressing)
   - Refcount explicite + hooks GC optionnels
   - Hash/égalité profonde, stringify/print, clone, move
   - Intégration vt_mem.*, vt_debug.*, vt_func.* optionnelle
   Licence: MIT.
   ============================================================================
 */

#include "object.h" /* expose vt_value et l’API publique */

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ctype.h"
#include "debug.h"
#include "mem.h"
#include "types.h"


/* ----------------------------------------------------------------------------
   Config locale
---------------------------------------------------------------------------- */
#ifndef VT_OBJ_MAP_MIN_CAP
#define VT_OBJ_MAP_MIN_CAP 16u
#endif

#ifndef VT_OBJ_GROW_ARR
#define VT_OBJ_GROW_ARR(cap) (((cap) < 8u) ? 8u : ((cap) + (cap) / 2u))
#endif
#ifndef VT_OBJ_GROW_MAP
#define VT_OBJ_GROW_MAP(cap) \
  (((cap) < VT_OBJ_MAP_MIN_CAP) ? VT_OBJ_MAP_MIN_CAP : ((cap) * 2u))
#endif

/* ----------------------------------------------------------------------------
   Types internes (entêtes heap + structures)
---------------------------------------------------------------------------- */
typedef struct vt_heap_hdr {
  uint32_t tag;  /* debug cookie */
  uint32_t refc; /* compteur de références */
} vt_heap_hdr;

#define VT_HDR(p) ((vt_heap_hdr*)(p))
#define VT_REFC(p) (VT_HDR(p)->refc)

typedef struct vt_str {
  vt_heap_hdr h;
  size_t len;
  size_t cap;
  uint64_t hash; /* FNV-1a, 0=non calculé */
  char* data;    /* NUL-terminé */
} vt_str;

typedef struct vt_bytes {
  vt_heap_hdr h;
  size_t len;
  size_t cap;
  uint64_t hash; /* FNV-1a */
  unsigned char* data;
} vt_bytes;

typedef struct vt_array {
  vt_heap_hdr h;
  size_t len;
  size_t cap;
  vt_value* data;
} vt_array;

typedef struct vt_map_entry {
  vt_str* key; /* clé string conservée (retain) */
  vt_value val;
  uint8_t state; /* 0=empty,1=used,2=tomb */
} vt_map_entry;

typedef struct vt_map {
  vt_heap_hdr h;
  size_t len;        /* éléments valides */
  size_t cap;        /* taille du tableau, puissance de 2 */
  vt_map_entry* tab; /* open addressing (linéaire) */
} vt_map;

/* ----------------------------------------------------------------------------
   Cookies de debug (optionnels)
---------------------------------------------------------------------------- */
#define TAG_STR 0x53545221u /* 'STR!' */
#define TAG_BYT 0x42595421u /* 'BYT!' */
#define TAG_ARR 0x41525221u /* 'ARR!' */
#define TAG_MAP 0x4D415021u /* 'MAP!' */

/* ----------------------------------------------------------------------------
   FNV-1a 64
---------------------------------------------------------------------------- */
static inline uint64_t vt_fnv1a64(const void* data, size_t n) {
  const unsigned char* p = (const unsigned char*)data;
  uint64_t h = 1469598103934665603ull;
  size_t i = 0;
  while (i < n) {
    h ^= (uint64_t)p[i++];
    h *= 1099511628211ull;
  }
  return h;
}

/* Mix pour combiner type et valeur */
static inline uint64_t vt_mix_u64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}
static inline uint64_t vt_mix2(uint64_t a, uint64_t b) {
  return vt_mix_u64(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
}

/* ----------------------------------------------------------------------------
   Refcount helpers
---------------------------------------------------------------------------- */
static inline void vt_heap_retain(void* p) {
  if (!p) return;
  ++VT_REFC(p);
}
static inline void vt_heap_release(void* p, void (*dtor)(void*)) {
  if (!p) return;
  if (--VT_REFC(p) == 0) {
    dtor(p);
  }
}

/* Forward dtors */
static void vt_str_dtor(void* p);
static void vt_bytes_dtor(void* p);
static void vt_array_dtor(void* p);
static void vt_map_dtor(void* p);

/* ----------------------------------------------------------------------------
   Conversions internes entre pointeurs et vt_value
---------------------------------------------------------------------------- */
static inline vt_value vt_make_ptr(vt_type t, void* p) {
  vt_value v;
  v.type = t;
  v.flags = 0;
  v.as.i = 0;
  v.as.p = p;
  return v;
}

/* ----------------------------------------------------------------------------
   Strings
---------------------------------------------------------------------------- */
static vt_str* vt_str_new_cap(size_t cap) {
  vt_str* s = (vt_str*)vt_malloc(sizeof(vt_str));
  VT_HDR(s)->tag = TAG_STR;
  VT_HDR(s)->refc = 1;
  s->len = 0;
  s->cap = cap ? cap : 1;
  s->hash = 0;
  s->data = (char*)vt_malloc(s->cap);
  s->data[0] = '\0';
  return s;
}
static void vt_str_dtor(void* p) {
  vt_str* s = (vt_str*)p;
  vt_free(s->data);
  vt_free(s);
}
static void vt_str_reserve(vt_str* s, size_t need) {
  if (need <= s->cap) return;
  size_t cap = s->cap;
  while (cap < need) cap = VT_OBJ_GROW_ARR(cap);
  char* nd = (char*)vt_realloc(s->data, cap);
  s->data = nd;
  s->cap = cap;
}
static vt_str* vt_str_from(const char* c, size_t n) {
  vt_str* s = vt_str_new_cap(n + 1);
  if (n) {
    memcpy(s->data, c, n);
  }
  s->data[n] = '\0';
  s->len = n;
  s->hash = 0;
  return s;
}
static inline void vt_str_touch_hash(vt_str* s) {
  if (s->hash == 0 && s->len) s->hash = vt_fnv1a64(s->data, s->len);
}

/* ----------------------------------------------------------------------------
   Bytes
---------------------------------------------------------------------------- */
static vt_bytes* vt_bytes_new_cap(size_t cap) {
  vt_bytes* b = (vt_bytes*)vt_malloc(sizeof(vt_bytes));
  VT_HDR(b)->tag = TAG_BYT;
  VT_HDR(b)->refc = 1;
  b->len = 0;
  b->cap = cap ? cap : 1;
  b->hash = 0;
  b->data = (unsigned char*)vt_malloc(b->cap);
  return b;
}
static void vt_bytes_dtor(void* p) {
  vt_bytes* b = (vt_bytes*)p;
  vt_free(b->data);
  vt_free(b);
}
static void vt_bytes_reserve(vt_bytes* b, size_t need) {
  if (need <= b->cap) return;
  size_t cap = b->cap;
  while (cap < need) cap = VT_OBJ_GROW_ARR(cap);
  b->data = (unsigned char*)vt_realloc(b->data, cap);
  b->cap = cap;
}
static inline void vt_bytes_touch_hash(vt_bytes* b) {
  if (b->hash == 0 && b->len) b->hash = vt_fnv1a64(b->data, b->len);
}

/* ----------------------------------------------------------------------------
   Array
---------------------------------------------------------------------------- */
static vt_array* vt_array_new_cap(size_t cap) {
  vt_array* a = (vt_array*)vt_malloc(sizeof(vt_array));
  VT_HDR(a)->tag = TAG_ARR;
  VT_HDR(a)->refc = 1;
  a->len = 0;
  a->cap = cap ? cap : 1;
  a->data = (vt_value*)vt_calloc(a->cap, sizeof(vt_value));
  return a;
}
static void vt_array_dtor(void* p) {
  vt_array* a = (vt_array*)p;
  size_t i;
  for (i = 0; i < a->len; ++i) {
    vt_value_release(&a->data[i]);
  }
  vt_free(a->data);
  vt_free(a);
}
static void vt_array_reserve(vt_array* a, size_t need) {
  if (need <= a->cap) return;
  size_t cap = a->cap;
  while (cap < need) cap = VT_OBJ_GROW_ARR(cap);
  vt_value* nd = (vt_value*)vt_calloc(cap, sizeof(vt_value));
  if (a->len) memcpy(nd, a->data, a->len * sizeof(vt_value));
  vt_free(a->data);
  a->data = nd;
  a->cap = cap;
}

/* ----------------------------------------------------------------------------
   Map (clé = string)
---------------------------------------------------------------------------- */
static vt_map* vt_map_new_cap(size_t cap);
static void vt_map_rehash(vt_map* m, size_t new_cap);

static vt_map* vt_map_new_cap(size_t cap) {
  size_t c = 1;
  while (c < (cap ? cap : VT_OBJ_MAP_MIN_CAP)) c <<= 1;
  vt_map* m = (vt_map*)vt_malloc(sizeof(vt_map));
  VT_HDR(m)->tag = TAG_MAP;
  VT_HDR(m)->refc = 1;
  m->len = 0;
  m->cap = c;
  m->tab = (vt_map_entry*)vt_calloc(c, sizeof(vt_map_entry));
  return m;
}
static void vt_map_dtor(void* p) {
  vt_map* m = (vt_map*)p;
  size_t i;
  for (i = 0; i < m->cap; ++i) {
    vt_map_entry* e = &m->tab[i];
    if (e->state == 1) {
      vt_value_release(&e->val);
      if (e->key) vt_str_release(e->key);
    }
  }
  vt_free(m->tab);
  vt_free(m);
}
static size_t vt_map_probe(vt_map* m, uint64_t h, const char* key, size_t klen,
                           int* found) {
  size_t mask = m->cap - 1;
  size_t i = (size_t)h & mask;
  size_t first_tomb = (size_t)-1;
  for (;;) {
    vt_map_entry* e = &m->tab[i];
    if (e->state == 0) {
      *found = 0;
      return (first_tomb != (size_t)-1) ? first_tomb : i;
    }
    if (e->state == 2) {
      if (first_tomb == (size_t)-1) first_tomb = i;
    } else {
      /* used */
      if (e->key && e->key->len == klen &&
          memcmp(e->key->data, key, klen) == 0) {
        *found = 1;
        return i;
      }
    }
    i = (i + 1) & mask;
  }
}
static void vt_map_rehash(vt_map* m, size_t new_cap) {
  vt_map_entry* old = m->tab;
  size_t old_cap = m->cap;
  m->cap = new_cap;
  m->tab = (vt_map_entry*)vt_calloc(new_cap, sizeof(vt_map_entry));
  m->len = 0;
  for (size_t i = 0; i < old_cap; ++i) {
    vt_map_entry* e = &old[i];
    if (e->state == 1) {
      int found;
      vt_str_touch_hash(e->key);
      size_t pos =
          vt_map_probe(m, e->key->hash, e->key->data, e->key->len, &found);
      vt_map_entry* d = &m->tab[pos];
      d->state = 1;
      d->key = e->key;
      vt_str_retain(d->key);
      d->val = vt_value_clone(&e->val);
      m->len++;
      vt_value_release(&e->val);
      vt_str_release(e->key);
    }
  }
  vt_free(old);
}

/* ----------------------------------------------------------------------------
   API publique — implémentations
---------------------------------------------------------------------------- */

/* --- Types et noms -------------------------------------------------------- */
const char* vt_type_name(vt_type t) {
  switch (t) {
    case VT_NIL:
      return "nil";
    case VT_BOOL:
      return "bool";
    case VT_INT:
      return "int";
    case VT_FLOAT:
      return "float";
    case VT_STR:
      return "string";
    case VT_BYTES:
      return "bytes";
    case VT_ARRAY:
      return "array";
    case VT_MAP:
      return "map";
    case VT_FUNC:
      return "func";
    case VT_PTR:
      return "ptr";
    default:
      return "?";
  }
}

/* --- Nil/Bool/Int/Float --------------------------------------------------- */
vt_value vt_nil(void) {
  vt_value v;
  v.type = VT_NIL;
  v.flags = 0;
  v.as.i = 0;
  return v;
}
vt_value vt_bool(int b) {
  vt_value v;
  v.type = VT_BOOL;
  v.flags = 0;
  v.as.i = (b != 0);
  return v;
}
vt_value vt_int(int64_t x) {
  vt_value v;
  v.type = VT_INT;
  v.flags = 0;
  v.as.i = x;
  return v;
}
vt_value vt_float(double x) {
  vt_value v;
  v.type = VT_FLOAT;
  v.flags = 0;
  v.as.f = x;
  return v;
}

/* --- String --------------------------------------------------------------- */
vt_value vt_string_from(const char* s) {
  if (!s) return vt_nil();
  size_t n = strlen(s);
  vt_str* p = vt_str_from(s, n);
  return vt_make_ptr(VT_STR, p);
}
vt_value vt_string_from_n(const char* s, size_t n) {
  if (!s) return vt_nil();
  vt_str* p = vt_str_from(s, n);
  return vt_make_ptr(VT_STR, p);
}
void vt_str_retain(vt_str* s) { vt_heap_retain(s); }
void vt_str_release(vt_str* s) { vt_heap_release(s, vt_str_dtor); }
const char* vt_string_cstr(const vt_value* v) {
  return (v && v->type == VT_STR && v->as.p) ? ((vt_str*)v->as.p)->data : NULL;
}
size_t vt_string_len(const vt_value* v) {
  return (v && v->type == VT_STR && v->as.p) ? ((vt_str*)v->as.p)->len : 0;
}
int vt_string_append(vt_value* v, const char* s, size_t n) {
  if (!v || v->type != VT_STR || !v->as.p) return 0;
  vt_str* st = (vt_str*)v->as.p;
  size_t need = st->len + n + 1;
  vt_str_reserve(st, need);
  memcpy(st->data + st->len, s, n);
  st->len += n;
  st->data[st->len] = '\0';
  st->hash = 0;
  return 1;
}

/* --- Bytes ---------------------------------------------------------------- */
vt_value vt_bytes_from(const void* data, size_t n) {
  vt_bytes* b = vt_bytes_new_cap(n);
  if (n) memcpy(b->data, data, n);
  b->len = n;
  return vt_make_ptr(VT_BYTES, b);
}
void vt_bytes_retain(vt_bytes* b) { vt_heap_retain(b); }
void vt_bytes_release(vt_bytes* b) { vt_heap_release(b, vt_bytes_dtor); }
size_t vt_bytes_len(const vt_value* v) {
  return (v && v->type == VT_BYTES && v->as.p) ? ((vt_bytes*)v->as.p)->len : 0;
}
const unsigned char* vt_bytes_ptr(const vt_value* v) {
  return (v && v->type == VT_BYTES && v->as.p) ? ((vt_bytes*)v->as.p)->data
                                               : NULL;
}

/* --- Array ---------------------------------------------------------------- */
vt_value vt_array_new(void) {
  vt_array* a = vt_array_new_cap(0);
  return vt_make_ptr(VT_ARRAY, a);
}
void vt_array_retain(vt_array* a) { vt_heap_retain(a); }
void vt_array_release(vt_array* a) { vt_heap_release(a, vt_array_dtor); }
size_t vt_array_len(const vt_value* v) {
  return (v && v->type == VT_ARRAY && v->as.p) ? ((vt_array*)v->as.p)->len : 0;
}
int vt_array_push(vt_value* arr, vt_value v) {
  if (!arr || arr->type != VT_ARRAY || !arr->as.p) return 0;
  vt_array* a = (vt_array*)arr->as.p;
  vt_array_reserve(a, a->len + 1);
  a->data[a->len++] = vt_value_clone(&v);
  return 1;
}
int vt_array_get(const vt_value* arr, size_t idx, vt_value* out) {
  if (!arr || arr->type != VT_ARRAY || !arr->as.p) return 0;
  vt_array* a = (vt_array*)arr->as.p;
  if (idx >= a->len) return 0;
  if (out) *out = vt_value_clone(&a->data[idx]);
  return 1;
}
int vt_array_set(vt_value* arr, size_t idx, vt_value v) {
  if (!arr || arr->type != VT_ARRAY || !arr->as.p) return 0;
  vt_array* a = (vt_array*)arr->as.p;
  if (idx >= a->len) return 0;
  vt_value_release(&a->data[idx]);
  a->data[idx] = vt_value_clone(&v);
  return 1;
}

/* --- Map<string, value> --------------------------------------------------- */
vt_value vt_map_new(void) {
  vt_map* m = vt_map_new_cap(0);
  return vt_make_ptr(VT_MAP, m);
}
void vt_map_retain(vt_map* m) { vt_heap_retain(m); }
void vt_map_release(vt_map* m) { vt_heap_release(m, vt_map_dtor); }

size_t vt_map_len(const vt_value* v) {
  return (v && v->type == VT_MAP && v->as.p) ? ((vt_map*)v->as.p)->len : 0;
}
static void vt_map_maybe_grow(vt_map* m) {
  /* charge factor ~ 0.66 */
  if ((m->len + (m->len >> 1)) >= m->cap) {
    vt_map_rehash(m, VT_OBJ_GROW_MAP(m->cap));
  }
}
int vt_map_set(vt_value* mapv, const char* key, const vt_value* val) {
  if (!mapv || mapv->type != VT_MAP || !mapv->as.p) return 0;
  vt_map* m = (vt_map*)mapv->as.p;
  vt_map_maybe_grow(m);
  size_t klen = strlen(key);
  uint64_t h = vt_fnv1a64(key, klen);
  int found = 0;
  size_t pos = vt_map_probe(m, h, key, klen, &found);
  vt_map_entry* e = &m->tab[pos];
  if (!found) {
    e->state = 1;
    e->key = vt_str_from(key, klen);
    vt_str_touch_hash(e->key);
    if (e->key->hash == 0) e->key->hash = h;
    e->val = vt_value_clone(val);
    m->len++;
  } else {
    vt_value_release(&e->val);
    e->val = vt_value_clone(val);
  }
  return 1;
}
int vt_map_get(const vt_value* mapv, const char* key, vt_value* out) {
  if (!mapv || mapv->type != VT_MAP || !mapv->as.p) return 0;
  vt_map* m = (vt_map*)mapv->as.p;
  size_t klen = strlen(key);
  uint64_t h = vt_fnv1a64(key, klen);
  int found = 0;
  size_t pos = vt_map_probe(m, h, key, klen, &found);
  if (!found) return 0;
  if (out) *out = vt_value_clone(&m->tab[pos].val);
  return 1;
}
int vt_map_has(const vt_value* mapv, const char* key) {
  vt_value tmp;
  return vt_map_get(mapv, key, &tmp) ? (vt_value_release(&tmp), 1) : 0;
}
int vt_map_del(vt_value* mapv, const char* key) {
  if (!mapv || mapv->type != VT_MAP || !mapv->as.p) return 0;
  vt_map* m = (vt_map*)mapv->as.p;
  size_t klen = strlen(key);
  uint64_t h = vt_fnv1a64(key, klen);
  int found = 0;
  size_t pos = vt_map_probe(m, h, key, klen, &found);
  if (!found) return 0;
  vt_map_entry* e = &m->tab[pos];
  vt_value_release(&e->val);
  vt_str_release(e->key);
  e->key = NULL;
  e->state = 2; /* tomb */
  m->len--;
  return 1;
}

/* --- Func & Ptr ----------------------------------------------------------- */
#ifdef HAVE_FUNC_H
#include "func.h"
#endif

vt_value vt_ptr(void* p) { return vt_make_ptr(VT_PTR, p); }

/* --- Clone / Move / Release ---------------------------------------------- */
vt_value vt_value_clone(const vt_value* v) {
  if (!v) return vt_nil();
  switch (v->type) {
    case VT_STR:
      if (v->as.p) vt_str_retain((vt_str*)v->as.p);
      return *v;
    case VT_BYTES:
      if (v->as.p) vt_bytes_retain((vt_bytes*)v->as.p);
      return *v;
    case VT_ARRAY:
      if (v->as.p) vt_array_retain((vt_array*)v->as.p);
      return *v;
    case VT_MAP:
      if (v->as.p) vt_map_retain((vt_map*)v->as.p);
      return *v;
    case VT_FUNC:
#ifdef HAVE_FUNC_H
      if (v->as.p) vt_func_retain((vt_func*)v->as.p);
#endif
      return *v;
    default:
      return *v;
  }
}
void vt_value_move(vt_value* dst, vt_value* src) {
  if (!dst || !src) return;
  *dst = *src;
  src->type = VT_NIL;
  src->as.i = 0;
}
void vt_value_release(vt_value* v) {
  if (!v) return;
  switch (v->type) {
    case VT_STR:
      vt_str_release((vt_str*)v->as.p);
      break;
    case VT_BYTES:
      vt_bytes_release((vt_bytes*)v->as.p);
      break;
    case VT_ARRAY:
      vt_array_release((vt_array*)v->as.p);
      break;
    case VT_MAP:
      vt_map_release((vt_map*)v->as.p);
      break;
    case VT_FUNC:
#ifdef HAVE_FUNC_H
      vt_func_release((vt_func*)v->as.p);
#endif
      break;
    default:
      break;
  }
  v->type = VT_NIL;
  v->as.i = 0;
}

/* --- Hash / Equality ------------------------------------------------------ */
uint64_t vt_value_hash(const vt_value* v) {
  if (!v) return 0;
  switch (v->type) {
    case VT_NIL:
      return 0x9a97f651u;
    case VT_BOOL:
      return vt_mix2(0xB01Lu, (uint64_t)v->as.i);
    case VT_INT:
      return vt_mix2(0x1NTu, (uint64_t)v->as.i);
    case VT_FLOAT: {
      uint64_t u;
      memcpy(&u, &v->as.f, sizeof(u));
      return vt_mix2(0xF10Au, u);
    }
    case VT_STR: {
      vt_str* s = (vt_str*)v->as.p;
      if (!s) return 0;
      vt_str_touch_hash(s);
      return vt_mix2(0x57TRu, s->hash);
    }
    case VT_BYTES: {
      vt_bytes* b = (vt_bytes*)v->as.p;
      if (!b) return 0;
      vt_bytes_touch_hash(b);
      return vt_mix2(0xB7ESu, b->hash);
    }
    case VT_ARRAY: {
      vt_array* a = (vt_array*)v->as.p;
      uint64_t h = 0xA2Ru ^ (uint64_t)a->len;
      for (size_t i = 0; i < a->len; i++) {
        h = vt_mix2(h, vt_value_hash(&a->data[i]));
      }
      return h;
    }
    case VT_MAP: {
      vt_map* m = (vt_map*)v->as.p;
      uint64_t h = 0xMAPu ^ (uint64_t)m->len;
      for (size_t i = 0; i < m->cap; i++)
        if (m->tab[i].state == 1) {
          vt_map_entry* e = &m->tab[i];
          vt_str_touch_hash(e->key);
          h = vt_mix2(h, vt_mix2(e->key->hash, vt_value_hash(&e->val)));
        }
      return h;
    }
    case VT_FUNC:
      return vt_mix2(0xFNCu, (uint64_t)(uintptr_t)v->as.p);
    case VT_PTR:
      return vt_mix2(0xPTRu, (uint64_t)(uintptr_t)v->as.p);
    default:
      return 0;
  }
}
int vt_value_equal(const vt_value* a, const vt_value* b) {
  if (a->type != b->type) return 0;
  switch (a->type) {
    case VT_NIL:
      return 1;
    case VT_BOOL:
      return a->as.i == b->as.i;
    case VT_INT:
      return a->as.i == b->as.i;
    case VT_FLOAT:
      return a->as.f == b->as.f;
    case VT_STR: {
      vt_str* sa = (vt_str*)a->as.p;
      vt_str* sb = (vt_str*)b->as.p;
      if (sa == sb) return 1;
      if (!sa || !sb || sa->len != sb->len) return 0;
      return memcmp(sa->data, sb->data, sa->len) == 0;
    }
    case VT_BYTES: {
      vt_bytes* ba = (vt_bytes*)a->as.p;
      vt_bytes* bb = (vt_bytes*)b->as.p;
      if (ba == bb) return 1;
      if (!ba || !bb || ba->len != bb->len) return 0;
      return memcmp(ba->data, bb->data, ba->len) == 0;
    }
    case VT_ARRAY: {
      vt_array* xa = (vt_array*)a->as.p;
      vt_array* xb = (vt_array*)b->as.p;
      if (xa == xb) return 1;
      if (!xa || !xb || xa->len != xb->len) return 0;
      for (size_t i = 0; i < xa->len; i++)
        if (!vt_value_equal(&xa->data[i], &xb->data[i])) return 0;
      return 1;
    }
    case VT_MAP: {
      vt_map* ma = (vt_map*)a->as.p;
      vt_map* mb = (vt_map*)b->as.p;
      if (ma == mb) return 1;
      if (!ma || !mb || ma->len != mb->len) return 0;
      /* vérifier chaque entrée de ma dans mb */
      for (size_t i = 0; i < ma->cap; i++)
        if (ma->tab[i].state == 1) {
          vt_map_entry* e = &ma->tab[i];
          vt_value tmp;
          if (!vt_map_get(b, e->key->data, &tmp)) return 0;
          int eq = vt_value_equal(&e->val, &tmp);
          vt_value_release(&tmp);
          if (!eq) return 0;
        }
      return 1;
    }
    case VT_FUNC:
      return a->as.p == b->as.p;
    case VT_PTR:
      return a->as.p == b->as.p;
    default:
      return 0;
  }
}

/* --- Stringify ------------------------------------------------------------ */
char* vt_value_to_cstr(const vt_value* v) {
  vt_buf b;
  vt_buf_init(&b);
  switch (v->type) {
    case VT_NIL:
      vt_buf_append_cstr(&b, "nil");
      break;
    case VT_BOOL:
      vt_buf_append_cstr(&b, v->as.i ? "true" : "false");
      break;
    case VT_INT:
      vt_buf_printf(&b, "%" PRId64, v->as.i);
      break;
    case VT_FLOAT:
      vt_buf_printf(&b, "%.17g", v->as.f);
      break;
    case VT_STR: {
      vt_str* s = (vt_str*)v->as.p;
      vt_buf_append_cstr(&b, "\"");
      if (s && s->len) vt_buf_append(&b, s->data, s->len);
      vt_buf_append_cstr(&b, "\"");
    } break;
    case VT_BYTES: {
      vt_bytes* by = (vt_bytes*)v->as.p;
      vt_buf_append_cstr(&b, "0x");
      if (by) {
        for (size_t i = 0; i < by->len; i++)
          vt_buf_printf(&b, "%02x", by->data[i]);
      }
    } break;
    case VT_ARRAY: {
      vt_array* a = (vt_array*)v->as.p;
      vt_buf_append_cstr(&b, "[");
      if (a) {
        for (size_t i = 0; i < a->len; i++) {
          if (i) vt_buf_append_cstr(&b, ", ");
          char* s = vt_value_to_cstr(&a->data[i]);
          vt_buf_append_cstr(&b, s ? s : "nil");
          vt_free(s);
        }
      }
      vt_buf_append_cstr(&b, "]");
    } break;
    case VT_MAP: {
      vt_map* m = (vt_map*)v->as.p;
      vt_buf_append_cstr(&b, "{");
      int first = 1;
      if (m)
        for (size_t i = 0; i < m->cap; i++)
          if (m->tab[i].state == 1) {
            if (!first) vt_buf_append_cstr(&b, ", ");
            first = 0;
            vt_buf_append_cstr(&b, "\"");
            vt_buf_append(&b, m->tab[i].key->data, m->tab[i].key->len);
            vt_buf_append_cstr(&b, "\": ");
            char* s = vt_value_to_cstr(&m->tab[i].val);
            vt_buf_append_cstr(&b, s ? s : "nil");
            vt_free(s);
          }
      vt_buf_append_cstr(&b, "}");
    } break;
    case VT_FUNC:
      vt_buf_printf(&b, "<func@%p>", v->as.p);
      break;
    case VT_PTR:
      vt_buf_printf(&b, "<ptr@%p>", v->as.p);
      break;
    default:
      vt_buf_append_cstr(&b, "?");
  }
  size_t n = 0;
  unsigned char* raw = vt_buf_detach(&b, &n);
  char* out = (char*)raw;
  if (!out) out = (char*)vt_strndup("", 0);
  return out;
}

/* --- Helpers utilitaires -------------------------------------------------- */
int vt_truthy(const vt_value* v) {
  if (!v) return 0;
  switch (v->type) {
    case VT_NIL:
      return 0;
    case VT_BOOL:
      return v->as.i != 0;
    case VT_INT:
      return v->as.i != 0;
    case VT_FLOAT:
      return v->as.f != 0.0;
    case VT_STR:
      return v->as.p && ((vt_str*)v->as.p)->len != 0;
    case VT_BYTES:
      return v->as.p && ((vt_bytes*)v->as.p)->len != 0;
    case VT_ARRAY:
      return v->as.p && ((vt_array*)v->as.p)->len != 0;
    case VT_MAP:
      return v->as.p && ((vt_map*)v->as.p)->len != 0;
    case VT_FUNC:
      return 1;
    case VT_PTR:
      return v->as.p != NULL;
    default:
      return 0;
  }
}

/* --- Concat string -------------------------------------------------------- */
int vt_string_concat(vt_value* a, const vt_value* b) {
  if (!a || a->type != VT_STR) return 0;
  if (b->type == VT_STR && b->as.p) {
    vt_str* sa = (vt_str*)a->as.p;
    vt_str* sb = (vt_str*)b->as.p;
    vt_str_reserve(sa, sa->len + sb->len + 1);
    memcpy(sa->data + sa->len, sb->data, sb->len);
    sa->len += sb->len;
    sa->data[sa->len] = '\0';
    sa->hash = 0;
    return 1;
  } else {
    char* s = vt_value_to_cstr(b);
    size_t n = strlen(s);
    int ok = vt_string_append(a, s, n);
    vt_free(s);
    return ok;
  }
}

/* --- Initialisation globale optionnelle ---------------------------------- */
void vt_object_runtime_init(void) {
  /* Placeholder si besoin (interning, GC hooks, etc.) */
  VT_INFO("object runtime ready");
}
void vt_object_runtime_shutdown(void) {
  /* Placeholder */
  VT_INFO("object runtime shutdown");
}
