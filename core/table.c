/* ============================================================================
   table.c — Hash table générique (C17), robin-hood open addressing.
   - Clés = octets arbitraires (void*, size_t). Valeurs = void*.
   - Callbacks configurables: hash, égalité, free(key), free(val).
   - Copy-on-insert optionnel des clés (copy_keys=1) ou adoption de pointeurs.
   - Effacement par backward-shift (pas de tombstones). Rehash auto.
   - Iteration stable via vt_table_next.
   - Thread-safety non incluse par design.
   Licence: MIT.
   ============================================================================
 */
#include "table.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------------
   Helpers
---------------------------------------------------------------------------- */
#ifndef VT_LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define VT_LIKELY(x) __builtin_expect(!!(x), 1)
#define VT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define VT_LIKELY(x) (x)
#define VT_UNLIKELY(x) (x)
#endif
#endif

static inline uint64_t vt__rotl64(uint64_t x, unsigned r) {
  return (x << r) | (x >> (64 - r));
}

static uint64_t vt__fnv1a64(const void* p, size_t n, void* u) {
  (void)u;
  const uint8_t* b = (const uint8_t*)p;
  uint64_t h = 1469598103934665603ull; /* FNV offset basis */
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 1099511628211ull; /* FNV prime */
  }
  /* légère avalanche */
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

static int vt__bytes_eq(const void* a, size_t alen, const void* b, size_t blen,
                        void* u) {
  (void)u;
  return (alen == blen) && (memcmp(a, b, alen) == 0);
}

static size_t vt__next_pow2(size_t x) {
  if (x < 4) return 4;
  x--;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  x |= x >> 32;
  return x + 1;
}

/* ----------------------------------------------------------------------------
   Structures internes
---------------------------------------------------------------------------- */
typedef struct vt_entry {
  uint64_t hash; /* 0 = vide */
  uint32_t dib;  /* distance to initial bucket (1..), 0 si vide */
  size_t klen;
  void* kptr; /* possiblement allouée si copy_keys=1 */
  void* vptr;
} vt_entry;

struct vt_table {
  vt_table_config cfg;
  vt_entry* slots;
  size_t cap;     /* taille du tableau (toujours puissance de 2) */
  size_t len;     /* nb d’entrées occupées */
  size_t grow_at; /* seuil de redimensionnement */
  bool copy_keys; /* si 1 → duplique les clés à l’insertion */
};

/* ----------------------------------------------------------------------------
   Allocation
---------------------------------------------------------------------------- */
#ifndef VT_MALLOC
#define VT_MALLOC(sz) malloc(sz)
#define VT_FREE(p) free(p)
#define VT_REALLOC(p, sz) realloc(p, sz)
#endif

static void vt__free_key_val(const vt_table* t, vt_entry* e) {
  if (e->hash == 0) return;
  if (t->cfg.free_key && (t->copy_keys || t->cfg.free_key_always)) {
    t->cfg.free_key(e->kptr, t->cfg.udata);
  }
  if (t->cfg.free_val) {
    t->cfg.free_val(e->vptr, t->cfg.udata);
  }
}

/* ----------------------------------------------------------------------------
   Core robin-hood
---------------------------------------------------------------------------- */
static inline size_t vt__mask(const vt_table* t) { return t->cap - 1; }

static void vt__set_capacity(vt_table* t, size_t new_cap) {
  vt_entry* old = t->slots;
  size_t old_cap = t->cap;

  vt_entry* slots = (vt_entry*)VT_MALLOC(new_cap * sizeof(vt_entry));
  assert(slots && "allocation failed");
  for (size_t i = 0; i < new_cap; i++) {
    slots[i].hash = 0;
    slots[i].dib = 0;
    slots[i].klen = 0;
    slots[i].kptr = NULL;
    slots[i].vptr = NULL;
  }

  t->slots = slots;
  t->cap = new_cap;
  t->len = 0;
  t->grow_at =
      (size_t)(t->cap * (t->cfg.max_load <= 0.0f ? 0.85f : t->cfg.max_load));

  if (!old) return;

  /* réinsertion des vieux éléments */
  for (size_t i = 0; i < old_cap; i++) {
    vt_entry e = old[i];
    if (e.hash == 0) continue;

    /* insertion robin-hood */
    size_t idx = e.hash & vt__mask(t);
    e.dib = 1;
    for (;;) {
      vt_entry* cur = &t->slots[idx];
      if (cur->hash == 0) {
        *cur = e;
        t->len++;
        break;
      }
      if (cur->dib < e.dib) {
        vt_entry tmp = *cur;
        *cur = e;
        e = tmp;
      }
      idx = (idx + 1) & vt__mask(t);
      e.dib++;
    }
  }

  VT_FREE(old);
}

/* Trouve le slot et remplit out_idx si trouvé */
static vt_entry* vt__find(const vt_table* t, const void* key, size_t klen,
                          uint64_t h, size_t* out_idx) {
  if (t->cap == 0) return NULL;
  size_t idx = h & vt__mask(t);
  uint32_t dib = 1;

  for (;;) {
    vt_entry* cur = &t->slots[idx];
    if (cur->hash == 0) return NULL; /* chaîne stoppée */
    if (cur->dib < dib) return NULL; /* robin-hood invariant */
    if (cur->hash == h &&
        t->cfg.eq(cur->kptr, cur->klen, key, klen, t->cfg.udata)) {
      if (out_idx) *out_idx = idx;
      return cur;
    }
    idx = (idx + 1) & vt__mask(t);
    dib++;
  }
}

/* Insertion ou remplacement. Renvoie pointeur vers slot final. */
static vt_entry* vt__insert(vt_table* t, const void* key, size_t klen,
                            void* val, uint64_t h, bool* replaced,
                            void** old_val) {
  if (t->cap == 0 || VT_UNLIKELY(t->len + 1 > t->grow_at)) {
    size_t nc = vt__next_pow2(
        t->cap ? t->cap * 2 : (t->cfg.initial_cap ? t->cfg.initial_cap : 16));
    vt__set_capacity(t, nc);
  }

  size_t idx = h & vt__mask(t);
  vt_entry e;
  e.hash = h;
  e.dib = 1;
  e.klen = klen;
  e.vptr = val;

  /* gestion clé */
  if (t->copy_keys) {
    void* dup = VT_MALLOC(klen);
    assert(dup && "alloc key");
    memcpy(dup, key, klen);
    e.kptr = dup;
  } else {
    e.kptr = (void*)key; /* adoption */
  }

  for (;;) {
    vt_entry* cur = &t->slots[idx];
    if (cur->hash == 0) {
      *cur = e;
      t->len++;
      if (replaced) *replaced = false;
      if (old_val) *old_val = NULL;
      return cur;
    }
    if (cur->hash == h &&
        t->cfg.eq(cur->kptr, cur->klen, key, klen, t->cfg.udata)) {
      /* remplacement valeur */
      void* prev = cur->vptr;
      cur->vptr = val;
      if (t->copy_keys) {
        /* On a alloué e.kptr, mais la clé existait déjà → libérer duplicat */
        VT_FREE(e.kptr);
      }
      if (replaced) *replaced = true;
      if (old_val) *old_val = prev;
      return cur;
    }
    if (cur->dib < e.dib) {
      vt_entry tmp = *cur;
      *cur = e;
      e = tmp; /* continue avec l’ancien élément à réinsérer */
    }
    idx = (idx + 1) & vt__mask(t);
    e.dib++;
  }
}

/* Suppression par backward-shift à partir de idx */
static void vt__erase_at(vt_table* t, size_t idx) {
  vt_entry* slots = t->slots;
  size_t mask = vt__mask(t);
  size_t i = idx;

  /* libérer clé/val */
  vt__free_key_val(t, &slots[i]);

  for (;;) {
    size_t j = (i + 1) & mask;
    if (slots[j].hash == 0 || slots[j].dib == 1) {
      /* fin de cluster: vider i et stop */
      slots[i].hash = 0;
      slots[i].dib = 0;
      slots[i].klen = 0;
      slots[i].kptr = NULL;
      slots[i].vptr = NULL;
      break;
    }
    /* shift left */
    slots[i] = slots[j];
    slots[i].dib--;
    i = j;
  }

  t->len--;
}

/* ----------------------------------------------------------------------------
   API
---------------------------------------------------------------------------- */
int vt_table_init(vt_table* t, const vt_table_config* cfg) {
  if (!t) return -1;
  memset(t, 0, sizeof(*t));

  t->cfg.hash = cfg && cfg->hash ? cfg->hash : vt__fnv1a64;
  t->cfg.eq = cfg && cfg->eq ? cfg->eq : vt__bytes_eq;
  t->cfg.free_key = cfg ? cfg->free_key : NULL;
  t->cfg.free_val = cfg ? cfg->free_val : NULL;
  t->cfg.udata = cfg ? cfg->udata : NULL;
  t->cfg.max_load = cfg && cfg->max_load > 0.f ? cfg->max_load : 0.85f;
  t->cfg.initial_cap = cfg && cfg->initial_cap ? cfg->initial_cap : 16;
  t->cfg.free_key_always = cfg ? cfg->free_key_always : 0;

  t->copy_keys = cfg ? cfg->copy_keys : true;

  t->slots = NULL;
  t->cap = 0;
  t->len = 0;
  t->grow_at = 0;

  vt__set_capacity(t, vt__next_pow2(t->cfg.initial_cap));
  return 0;
}

void vt_table_free(vt_table* t) {
  if (!t || !t->slots) return;
  for (size_t i = 0; i < t->cap; i++) {
    vt__free_key_val(t, &t->slots[i]);
  }
  VT_FREE(t->slots);
  t->slots = NULL;
  t->cap = t->len = t->grow_at = 0;
}

void vt_table_clear(vt_table* t) {
  if (!t || !t->slots) return;
  for (size_t i = 0; i < t->cap; i++) {
    vt__free_key_val(t, &t->slots[i]);
    t->slots[i].hash = 0;
    t->slots[i].dib = 0;
    t->slots[i].klen = 0;
    t->slots[i].kptr = NULL;
    t->slots[i].vptr = NULL;
  }
  t->len = 0;
}

size_t vt_table_len(const vt_table* t) { return t ? t->len : 0; }

bool vt_table_put(vt_table* t, const void* key, size_t klen, void* val,
                  void** old_val_out) {
  if (!t || !key) return false;
  uint64_t h = t->cfg.hash(key, klen, t->cfg.udata);
  bool replaced = false;
  void* oldv = NULL;
  vt__insert(t, key, klen, val, h, &replaced, &oldv);
  if (old_val_out)
    *old_val_out = oldv;
  else if (replaced && t->cfg.free_val)
    t->cfg.free_val(oldv, t->cfg.udata);
  return true;
}

bool vt_table_get(const vt_table* t, const void* key, size_t klen,
                  void** val_out) {
  if (!t || !key) return false;
  uint64_t h = t->cfg.hash(key, klen, t->cfg.udata);
  vt_entry* e = vt__find(t, key, klen, h, NULL);
  if (!e) return false;
  if (val_out) *val_out = e->vptr;
  return true;
}

bool vt_table_del(vt_table* t, const void* key, size_t klen,
                  void** old_val_out) {
  if (!t || !key) return false;
  size_t idx = 0;
  uint64_t h = t->cfg.hash(key, klen, t->cfg.udata);
  vt_entry* e = vt__find(t, key, klen, h, &idx);
  if (!e) return false;

  /* si on veut renvoyer la valeur, la sauver avant free_val */
  void* saved_val = e->vptr;
  void* saved_key = e->kptr;
  size_t saved_klen = e->klen;

  /* neutraliser free_val si l’appelant veut récupérer old_val_out */
  if (old_val_out && t->cfg.free_val) {
    /* on va contourner vt__free_key_val pour la valeur */
    e->vptr = NULL;
  }
  vt__erase_at(t, idx);

  if (old_val_out)
    *old_val_out = saved_val;
  else if (t->cfg.free_val)
    t->cfg.free_val(saved_val, t->cfg.udata);

  /* si la clé était copiée et que free_key n’est pas défini, la libérer
   * nous-mêmes */
  if (t->copy_keys && !t->cfg.free_key) {
    VT_FREE(saved_key);
  } else if (t->copy_keys && t->cfg.free_key && t->cfg.free_key_always == 0) {
    /* déjà libérée par vt__free_key_val */
    (void)saved_key;
    (void)saved_klen;
  }
  return true;
}

bool vt_table_has(const vt_table* t, const void* key, size_t klen) {
  if (!t || !key) return false;
  uint64_t h = t->cfg.hash(key, klen, t->cfg.udata);
  return vt__find(t, key, klen, h, NULL) != NULL;
}

void* vt_table_getptr(const vt_table* t, const void* key, size_t klen) {
  void* v = NULL;
  if (vt_table_get(t, key, klen, &v)) return v;
  return NULL;
}

/* Remplace in-place la valeur à un slot existant. Renvoie false si absent. */
bool vt_table_replace(vt_table* t, const void* key, size_t klen, void* val,
                      void** old_val_out) {
  if (!t || !key) return false;
  uint64_t h = t->cfg.hash(key, klen, t->cfg.udata);
  vt_entry* e = vt__find(t, key, klen, h, NULL);
  if (!e) return false;
  void* prev = e->vptr;
  e->vptr = val;
  if (old_val_out)
    *old_val_out = prev;
  else if (t->cfg.free_val)
    t->cfg.free_val(prev, t->cfg.udata);
  return true;
}

/* Réserve pour n éléments au load factor courant. */
void vt_table_reserve(vt_table* t, size_t n) {
  if (!t) return;
  size_t need =
      (size_t)((double)n /
               (double)(t->cfg.max_load <= 0.f ? 0.85f : t->cfg.max_load)) +
      1;
  need = vt__next_pow2(need < 16 ? 16 : need);
  if (need > t->cap) vt__set_capacity(t, need);
}

/* Changement de stratégie de clés: copy/adopt. Non thread-safe. */
void vt_table_set_copy_keys(vt_table* t, bool copy) {
  if (!t) return;
  t->copy_keys = copy;
}

/* ----------------------------------------------------------------------------
   Itération
---------------------------------------------------------------------------- */
void vt_table_iter_init(vt_table_iter* it) {
  if (!it) return;
  it->idx = SIZE_MAX;
}

bool vt_table_next(vt_table* t, vt_table_iter* it, const void** kptr,
                   size_t* klen, void** vptr) {
  if (!t || !it) return false;
  size_t i = (it->idx == SIZE_MAX) ? 0 : (it->idx + 1);
  for (; i < t->cap; i++) {
    vt_entry* e = &t->slots[i];
    if (e->hash != 0) {
      if (kptr) *kptr = e->kptr;
      if (klen) *klen = e->klen;
      if (vptr) *vptr = e->vptr;
      it->idx = i;
      return true;
    }
  }
  return false;
}

/* ----------------------------------------------------------------------------
   Utilitaires standards de hachage (exposés par table.h)
---------------------------------------------------------------------------- */
uint64_t vt_hash_bytes(const void* p, size_t n) {
  return vt__fnv1a64(p, n, NULL);
}

uint64_t vt_hash_cstr(const char* z) {
  return vt__fnv1a64(z, z ? strlen(z) : 0, NULL);
}

int vt_keyeq_bytes(const void* a, size_t alen, const void* b, size_t blen) {
  return vt__bytes_eq(a, alen, b, blen, NULL);
}

/* ----------------------------------------------------------------------------
   Debug/asserts basiques (optionnel)
---------------------------------------------------------------------------- */
#ifndef NDEBUG
static int vt__is_power_of_two(size_t x) { return x && !(x & (x - 1)); }
void vt_table__self_check(const vt_table* t) {
  assert(t);
  assert(vt__is_power_of_two(t->cap));
  size_t seen = 0;
  for (size_t i = 0; i < t->cap; i++) {
    const vt_entry* e = &t->slots[i];
    if (e->hash == 0) {
      assert(e->dib == 0);
      continue;
    }
    seen++;
    size_t home = e->hash & vt__mask(t);
    size_t dist = (i + t->cap - home) & vt__mask(t);
    assert(dist + 1 == e->dib);
  }
  assert(seen == t->len);
}
#endif
