// vitte-light/core/table.c
// Table de hachage générique ultra-complète (Robin Hood + backshift deletion)
// Clés et valeurs = pointeurs opaques. Hooks optionnels pour retain/free.
// Itération stable par index croissant. Sans dépendance au runtime VL.
//
// API minimale (si vous voulez un header, demandez `table.h`):
//   typedef uint32_t (*VL_HashFn)(const void *key, void *udata);
//   typedef int      (*VL_EqualFn)(const void *a, const void *b, void *udata);
//   typedef void *   (*VL_RetainFn)(void *ptr, void *udata); // peut retourner
//   une copie typedef void     (*VL_ReleaseFn)(void *ptr, void *udata);
//
//   typedef struct VL_Table VL_Table;
//   void     vl_tab_init (VL_Table *t, size_t initial_cap,
//                         VL_HashFn hf, VL_EqualFn eq,
//                         VL_RetainFn kret, VL_ReleaseFn kfree,
//                         VL_RetainFn vret, VL_ReleaseFn vfree,
//                         void *udata);
//   void     vl_tab_release(VL_Table *t);
//   size_t   vl_tab_len  (const VL_Table *t);
//   size_t   vl_tab_cap  (const VL_Table *t);
//   int      vl_tab_reserve(VL_Table *t, size_t min_cap);
//   int      vl_tab_put  (VL_Table *t, const void *key, const void *val); //
//   replace si existe int      vl_tab_get  (const VL_Table *t, const void *key,
//   void **out_val); int      vl_tab_del  (VL_Table *t, const void *key);
//   ssize_t  vl_tab_next (const VL_Table *t, ssize_t i, void **out_key, void
//   **out_val);
//
// Helpers C-string:
//   uint32_t vl_hash_cstr(const void *k, void *udata);
//   int      vl_eq_cstr  (const void *a, const void *b, void *udata);
//   void     vl_tab_init_cstr(VL_Table *t, size_t initial_cap); // hash/eq
//   préconfigurés + dup/free int      vl_tab_put_cstr(VL_Table *t, const char
//   *key, const void *val); int      vl_tab_get_cstr(const VL_Table *t, const
//   char *key, void **out_val); int      vl_tab_del_cstr(VL_Table *t, const
//   char *key);
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/table.c

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

// ───────────────────────── Signatures publiques ─────────────────────────
typedef uint32_t (*VL_HashFn)(const void *key, void *udata);
typedef int (*VL_EqualFn)(const void *a, const void *b, void *udata);
typedef void *(*VL_RetainFn)(void *ptr, void *udata);
typedef void (*VL_ReleaseFn)(void *ptr, void *udata);

// ───────────────────────── Implémentation ─────────────────────────

typedef struct VL_TableEntry {
  void *k;
  void *v;
  uint32_t h;
  uint32_t dib;
} VL_TableEntry;

typedef struct VL_Table {
  VL_TableEntry *ent;
  size_t cap;  // puissance de 2 (>=8)
  size_t len;  // # éléments vivants
  VL_HashFn hash;
  VL_EqualFn eq;
  VL_RetainFn kret;
  VL_ReleaseFn kfree;
  VL_RetainFn vret;
  VL_ReleaseFn vfree;
  void *udata;
} VL_Table;

// ───────────────────────── Utils ─────────────────────────
static inline size_t round_pow2(size_t x) {
  if (x < 8) return 8;
  x--;
  for (size_t i = 1; i < sizeof(size_t) * 8; i <<= 1) x |= x >> i;
  return x + 1;
}
static inline size_t idx_mask(const VL_Table *t) { return t->cap - 1; }
static inline uint32_t fnv1a32(const void *p, size_t n) {
  const unsigned char *s = (const unsigned char *)p;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= s[i];
    h *= 16777619u;
  }
  return h ? h : 1u;
}

// ───────────────────────── API ─────────────────────────
void vl_tab_init(VL_Table *t, size_t initial_cap, VL_HashFn hf, VL_EqualFn eq,
                 VL_RetainFn kret, VL_ReleaseFn kfree, VL_RetainFn vret,
                 VL_ReleaseFn vfree, void *udata) {
  memset(t, 0, sizeof(*t));
  t->cap = round_pow2(initial_cap ? initial_cap : 8);
  t->ent = (VL_TableEntry *)calloc(t->cap, sizeof(VL_TableEntry));
  t->hash = hf;
  t->eq = eq;
  t->kret = kret;
  t->kfree = kfree;
  t->vret = vret;
  t->vfree = vfree;
  t->udata = udata;
}

void vl_tab_release(VL_Table *t) {
  if (!t) return;
  if (t->ent) {
    for (size_t i = 0; i < t->cap; i++) {
      if (t->ent[i].k) {
        if (t->kfree) t->kfree(t->ent[i].k, t->udata);
        if (t->vfree && t->ent[i].v) t->vfree(t->ent[i].v, t->udata);
      }
    }
    free(t->ent);
  }
  memset(t, 0, sizeof(*t));
}

size_t vl_tab_len(const VL_Table *t) { return t ? t->len : 0; }
size_t vl_tab_cap(const VL_Table *t) { return t ? t->cap : 0; }

static int vl__tab_reinsert(VL_Table *t, void *k, void *v, uint32_t h) {
  size_t mask = idx_mask(t);
  size_t i = (size_t)h & mask;
  VL_TableEntry cur = {k, v, h, 0};
  for (;;) {
    VL_TableEntry *e = &t->ent[i];
    if (!e->k) {
      *e = cur;
      t->len++;
      return 1;
    }
    if (e->h == cur.h && t->eq &&
        t->eq(e->k, cur.k,
              t->udata)) {  // replace (rehash phase ne devrait pas arriver)
      if (t->vfree && e->v) t->vfree(e->v, t->udata);
      e->v = cur.v;
      return 1;
    }
    if (e->dib < cur.dib) {
      VL_TableEntry tmp = *e;
      *e = cur;
      cur = tmp;
    }
    cur.dib++;
    i = (i + 1) & mask;
  }
}

static int vl__tab_grow(VL_Table *t, size_t ncap) {
  VL_TableEntry *old = t->ent;
  size_t ocap = t->cap;
  t->cap = round_pow2(ncap);
  t->ent = (VL_TableEntry *)calloc(t->cap, sizeof(VL_TableEntry));
  if (!t->ent) {
    t->ent = old;
    t->cap = ocap;
    return 0;
  }
  size_t olen = t->len;
  t->len = 0;
  for (size_t i = 0; i < ocap; i++) {
    if (old[i].k) {
      vl__tab_reinsert(t, old[i].k, old[i].v, old[i].h);
    }
  }
  free(old);
  t->len = olen;
  return 1;
}

int vl_tab_reserve(VL_Table *t, size_t min_cap) {
  if (!t) return 0;
  if (t->cap >= min_cap) return 1;
  return vl__tab_grow(t, min_cap);
}

int vl_tab_put(VL_Table *t, const void *key, const void *val) {
  if (!t || !key) return 0;
  if (!t->hash || !t->eq) return 0;
  if ((t->len + 1) * 100 >= t->cap * 85) {
    if (!vl__tab_grow(t, t->cap * 2)) return 0;
  }
  void *K = (void *)key;
  void *V = (void *)val;
  if (t->kret) K = t->kret((void *)key, t->udata);
  if (t->vret) V = t->vret((void *)val, t->udata);
  uint32_t h = t->hash(key, t->udata);
  size_t mask = idx_mask(t);
  size_t i = (size_t)h & mask;
  VL_TableEntry cur = {K, V, h, 0};
  for (;;) {
    VL_TableEntry *e = &t->ent[i];
    if (!e->k) {
      *e = cur;
      t->len++;
      return 1;
    }
    if (e->h == cur.h && t->eq(e->k, key, t->udata)) {
      // replace
      if (t->kfree) t->kfree(K, t->udata);  // clé retenue inutile
      if (t->vfree && e->v) t->vfree(e->v, t->udata);
      e->v = V;
      return 1;
    }
    if (e->dib < cur.dib) {
      VL_TableEntry tmp = *e;
      *e = cur;
      cur = tmp;
    }
    cur.dib++;
    i = (i + 1) & mask;
  }
}

int vl_tab_get(const VL_Table *t, const void *key, void **out_val) {
  if (!t || !key) return 0;
  size_t mask = idx_mask(t);
  uint32_t h = t->hash(key, t->udata);
  size_t i = (size_t)h & mask;
  uint32_t dib = 0;
  for (;;) {
    const VL_TableEntry *e = &t->ent[i];
    if (!e->k) return 0;
    if (e->dib < dib) return 0;
    if (e->h == h && t->eq(e->k, key, t->udata)) {
      if (out_val) *out_val = e->v;
      return 1;
    }
    dib++;
    i = (i + 1) & mask;
  }
}

static void vl__tab_delete_at(VL_Table *t, size_t i) {
  size_t mask = idx_mask(t);
  // free entrée i
  if (t->kfree && t->ent[i].k) t->kfree(t->ent[i].k, t->udata);
  if (t->vfree && t->ent[i].v) t->vfree(t->ent[i].v, t->udata);
  // backshift deletion
  for (size_t j = i;;) {
    size_t k = (j + 1) & mask;
    VL_TableEntry e = t->ent[k];
    if (!e.k || e.dib == 0) {
      t->ent[j].k = NULL;
      t->ent[j].v = NULL;
      t->ent[j].h = 0;
      t->ent[j].dib = 0;
      break;
    }
    t->ent[j] = e;
    t->ent[j].dib--;
    j = k;
  }
}

int vl_tab_del(VL_Table *t, const void *key) {
  if (!t || !key) return 0;
  size_t mask = idx_mask(t);
  uint32_t h = t->hash(key, t->udata);
  size_t i = (size_t)h & mask;
  uint32_t dib = 0;
  for (;;) {
    VL_TableEntry *e = &t->ent[i];
    if (!e->k) return 0;
    if (e->dib < dib) return 0;
    if (e->h == h && t->eq(e->k, key, t->udata)) {
      vl__tab_delete_at(t, i);
      t->len--;
      return 1;
    }
    dib++;
    i = (i + 1) & mask;
  }
}

ssize_t vl_tab_next(const VL_Table *t, ssize_t i, void **out_key,
                    void **out_val) {
  if (!t || !t->ent) return -1;
  size_t k = (i < 0) ? 0 : (size_t)i + 1;
  for (; k < t->cap; k++) {
    if (t->ent[k].k) {
      if (out_key) *out_key = t->ent[k].k;
      if (out_val) *out_val = t->ent[k].v;
      return (ssize_t)k;
    }
  }
  return -1;
}

// ───────────────────────── Helpers C-string ─────────────────────────
static void *cstr_retain(void *p, void *ud) {
  (void)ud;
  if (!p) return NULL;
  size_t n = strlen((const char *)p) + 1;
  char *q = (char *)malloc(n);
  if (q) memcpy(q, p, n);
  return q;
}
static void cstr_free(void *p, void *ud) {
  (void)ud;
  free(p);
}

uint32_t vl_hash_cstr(const void *k, void *udata) {
  (void)udata;
  const char *s = (const char *)k;
  return s ? fnv1a32(s, strlen(s)) : 2166136261u;
}
int vl_eq_cstr(const void *a, const void *b, void *udata) {
  (void)udata;
  const char *sa = (const char *)a, *sb = (const char *)b;
  if (sa == sb) return 1;
  if (!sa || !sb) return 0;
  return strcmp(sa, sb) == 0;
}

void vl_tab_init_cstr(VL_Table *t, size_t initial_cap) {
  vl_tab_init(t, initial_cap, vl_hash_cstr, vl_eq_cstr, cstr_retain, cstr_free,
              NULL, NULL, NULL);
}
int vl_tab_put_cstr(VL_Table *t, const char *key, const void *val) {
  return vl_tab_put(t, key, val);
}
int vl_tab_get_cstr(const VL_Table *t, const char *key, void **out_val) {
  return vl_tab_get(t, key, out_val);
}
int vl_tab_del_cstr(VL_Table *t, const char *key) { return vl_tab_del(t, key); }

// ───────────────────────── Tests ─────────────────────────
#ifdef VL_TABLE_TEST_MAIN
#include <assert.h>
static void t_basic(void) {
  VL_Table t;
  vl_tab_init_cstr(&t, 8);
  for (int i = 0; i < 10000; i++) {
    char k[32];
    snprintf(k, sizeof(k), "k%d", i);
    int *pv = (int *)malloc(sizeof(int));
    *pv = i;
    int ok = vl_tab_put_cstr(&t, k, pv);
    assert(ok);
  }
  assert(vl_tab_len(&t) == 10000);
  for (int i = 0; i < 10000; i++) {
    char k[32];
    snprintf(k, sizeof(k), "k%d", i);
    void *v = NULL;
    int ok = vl_tab_get_cstr(&t, k, &v);
    assert(ok && v);
    assert(*(int *)v == i);
  }
  for (int i = 0; i < 5000; i++) {
    char k[32];
    snprintf(k, sizeof(k), "k%d", i);
    int ok = vl_tab_del_cstr(&t, k);
    assert(ok);
  }
  assert(vl_tab_len(&t) == 5000);
  // itération
  ssize_t it = -1;
  size_t count = 0;
  void *K, *V;
  while ((it = vl_tab_next(&t, it, &K, &V)) >= 0) {
    (void)K;
    free(V);
    count++;
  }
  assert(count == vl_tab_len(&t));
  vl_tab_release(&t);
}
int main(void) {
  t_basic();
  puts("ok");
  return 0;
}
#endif
