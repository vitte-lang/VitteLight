/* ============================================================================
   hashmap.c — Table de hachage générique, rapide, portable (C17, MIT)
   - Open addressing avec Robin Hood + tombstones
   - API générique via callbacks: hash(key) et equals(a,b)
   - O(1) amorti: put/get/del. Redimension dynamique
   - Confort: variante « string map » (clé UTF-8, dup/free automatique)
   - Thread-safety: non (à sérialiser côté appelant)
   - Zéro dépendance hors libc
   ============================================================================
*/
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------------
   API publique si aucun header n’est fourni (vt_hashmap_*)
---------------------------------------------------------------------------- */
#ifndef VT_HASHMAP_HAVE_HEADER
#ifndef VT_HASHMAP_API
#define VT_HASHMAP_API extern
#endif

typedef uint64_t (*vt_hash_fn)(const void* key, void* udata);
typedef int      (*vt_eq_fn)(const void* a, const void* b, void* udata);
typedef void     (*vt_key_free_fn)(void* key, void* udata);

typedef struct vt_hashmap vt_hashmap;

/* construction / destruction */
VT_HASHMAP_API vt_hashmap* vt_hashmap_new(vt_hash_fn hf, vt_eq_fn eq,
                                          vt_key_free_fn kfree,
                                          void* udata);
VT_HASHMAP_API void        vt_hashmap_free(vt_hashmap* m);

/* taille / stats */
VT_HASHMAP_API size_t vt_hashmap_len(const vt_hashmap* m);
VT_HASHMAP_API size_t vt_hashmap_capacity(const vt_hashmap* m);

/* opérations clés */
VT_HASHMAP_API int    vt_hashmap_put(vt_hashmap* m, void* key, void* value); /* 0=ok, -1=OOM */
VT_HASHMAP_API int    vt_hashmap_get(const vt_hashmap* m, const void* key, void** out_val); /* 1=found,0=miss */
VT_HASHMAP_API int    vt_hashmap_del(vt_hashmap* m, const void* key); /* 1=deleted,0=miss */

/* itération (stable tant que la map ne grossit pas) */
typedef int (*vt_hashmap_iter_fn)(void* key, void* value, void* udata);
VT_HASHMAP_API void   vt_hashmap_foreach(vt_hashmap* m, vt_hashmap_iter_fn it, void* udata);

/* ----------------------------------------------------------------------------
   Variante « string map »
---------------------------------------------------------------------------- */
VT_HASHMAP_API vt_hashmap* vt_hashmap_new_string(void);              /* clés char* dupliquées */
VT_HASHMAP_API int         vt_hashmap_put_str(vt_hashmap* m, const char* key, void* value);
VT_HASHMAP_API int         vt_hashmap_get_str(const vt_hashmap* m, const char* key, void** out_val);
VT_HASHMAP_API int         vt_hashmap_del_str(vt_hashmap* m, const char* key);

#endif /* VT_HASHMAP_HAVE_HEADER */

/* ----------------------------------------------------------------------------
   Implémentation
---------------------------------------------------------------------------- */
#ifndef VT_HM_INLINE
#define VT_HM_INLINE static inline
#endif

/* FNV-1a 64 pour la variante string */
VT_HM_INLINE uint64_t vt__fnv1a64(const void* data, size_t len) {
  const unsigned char* p = (const unsigned char*)data;
  uint64_t h = 1469598103934665603ull;
  for (size_t i=0;i<len;i++){ h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

/* Entrée de bucket */
typedef struct {
  uint64_t hash;  /* 0 = vide */
  void*    key;
  void*    val;
  uint32_t dib;   /* distance from initial bucket (Robin Hood) */
  uint8_t  state; /* 0 empty, 1 used, 2 tomb */
} vt_hm_entry;

struct vt_hashmap {
  vt_hm_entry* tab;
  size_t cap;     /* puissance de 2 */
  size_t len;     /* entrées valides */
  size_t used;    /* used + tombstones */
  vt_hash_fn hash_fn;
  vt_eq_fn   eq_fn;
  vt_key_free_fn key_free;
  void* udata;
};

/* puissance de 2 >= x, min 16 */
VT_HM_INLINE size_t vt__pow2_at_least(size_t x) {
  size_t n = 16;
  while (n < x) n <<= 1;
  return n;
}
VT_HM_INLINE size_t vt__mask(const vt_hashmap* m){ return m->cap - 1; }
VT_HM_INLINE size_t vt__index(uint64_t h, const vt_hashmap* m){ return (size_t)h & vt__mask(m); }

VT_HM_INLINE void vt__swap_entries(vt_hm_entry* a, vt_hm_entry* b) {
  vt_hm_entry t = *a; *a = *b; *b = t;
}

static int vt__grow(vt_hashmap* m, size_t need_len) {
  /* grow si load factor > 0.85 sur used (incl. tombstones) ou manque 1 */
  if (m->cap && m->used + 1 <= (size_t)((double)m->cap * 0.85) && need_len <= m->cap) return 0;
  size_t ncap = vt__pow2_at_least(m->cap ? m->cap<<1 : 16);
  if (ncap < need_len) ncap = vt__pow2_at_least(need_len);
  vt_hm_entry* nt = (vt_hm_entry*)calloc(ncap, sizeof(vt_hm_entry));
  if (!nt) return -1;

  size_t ocap = m->cap;
  vt_hm_entry* ot = m->tab;

  m->tab = nt; m->cap = ncap; m->len = 0; m->used = 0;

  if (ot) {
    for (size_t i=0;i<ocap;i++) {
      vt_hm_entry e = ot[i];
      if (e.state == 1) {
        /* réinjecte */
        e.dib = 0;
        size_t idx = (size_t)e.hash & (ncap - 1);
        for (;;) {
          vt_hm_entry* slot = &m->tab[idx];
          if (slot->state != 1) {
            *slot = e; slot->state = 1; m->len++; m->used++; break;
          }
          if (slot->dib < e.dib) { vt__swap_entries(slot, &e); }
          idx = (idx + 1) & (ncap - 1);
          e.dib++;
        }
      }
    }
    free(ot);
  }
  return 0;
}

vt_hashmap* vt_hashmap_new(vt_hash_fn hf, vt_eq_fn eq,
                           vt_key_free_fn kfree, void* udata) {
  if (!hf || !eq) return NULL;
  vt_hashmap* m = (vt_hashmap*)calloc(1, sizeof(*m));
  if (!m) return NULL;
  m->hash_fn = hf; m->eq_fn = eq; m->key_free = kfree; m->udata = udata;
  /* table allouée à la première insertion */
  return m;
}

void vt_hashmap_free(vt_hashmap* m) {
  if (!m) return;
  if (m->tab && m->key_free) {
    for (size_t i=0;i<m->cap;i++) if (m->tab[i].state==1) m->key_free(m->tab[i].key, m->udata);
  }
  free(m->tab);
  free(m);
}

size_t vt_hashmap_len(const vt_hashmap* m){ return m ? m->len : 0; }
size_t vt_hashmap_capacity(const vt_hashmap* m){ return m ? m->cap : 0; }

int vt_hashmap_put(vt_hashmap* m, void* key, void* value) {
  if (!m) return -1;
  if (m->cap == 0) {
    if (vt__grow(m, 16) != 0) return -1;
  } else if (vt__grow(m, m->len + 1) != 0) {
    return -1;
  }

  uint64_t h = m->hash_fn(key, m->udata);
  vt_hm_entry e = {.hash=h, .key=key, .val=value, .dib=0, .state=1};

  size_t idx = vt__index(h, m);
  for (;;) {
    vt_hm_entry* s = &m->tab[idx];
    if (s->state != 1) {
      /* vide ou tombstone -> insère */
      if (s->state == 0) m->used++;
      *s = e; s->state = 1; m->len++;
      return 0;
    }
    if (s->hash == h && m->eq_fn(s->key, key, m->udata)) {
      /* update */
      if (m->key_free && s->key != key) m->key_free(s->key, m->udata);
      s->key = key; s->val = value;
      return 0;
    }
    if (s->dib < e.dib) { vt__swap_entries(s, &e); }
    idx = (idx + 1) & vt__mask(m);
    e.dib++;
  }
}

int vt_hashmap_get(const vt_hashmap* m, const void* key, void** out_val) {
  if (!m || m->cap==0) return 0;
  uint64_t h = m->hash_fn(key, m->udata);
  size_t idx = vt__index(h, m);
  uint32_t dib = 0;
  for (;;) {
    const vt_hm_entry* s = &m->tab[idx];
    if (s->state == 0) return 0;          /* trou -> absent */
    if (s->state == 1) {
      if (s->hash == h && m->eq_fn(s->key, key, m->udata)) {
        if (out_val) *out_val = s->val;
        return 1;
      }
      if (s->dib < dib) return 0;         /* RH property -> impossible après */
    }
    idx = (idx + 1) & vt__mask(m);
    dib++;
  }
}

int vt_hashmap_del(vt_hashmap* m, const void* key) {
  if (!m || m->cap==0) return 0;
  uint64_t h = m->hash_fn(key, m->udata);
  size_t idx = vt__index(h, m);
  uint32_t dib = 0;
  for (;;) {
    vt_hm_entry* s = &m->tab[idx];
    if (s->state == 0) return 0;
    if (s->state == 1) {
      if (s->hash == h && m->eq_fn(s->key, key, m->udata)) {
        if (m->key_free) m->key_free(s->key, m->udata);
        s->state = 2; s->key = NULL; s->val = NULL; s->hash = 0; s->dib = 0;
        m->len--;
        return 1;
      }
      if (s->dib < dib) return 0;
    }
    idx = (idx + 1) & vt__mask(m);
    dib++;
  }
}

void vt_hashmap_foreach(vt_hashmap* m, vt_hashmap_iter_fn it, void* u) {
  if (!m || !it || m->cap==0) return;
  for (size_t i=0;i<m->cap;i++) {
    vt_hm_entry* e = &m->tab[i];
    if (e->state==1) {
      if (it(e->key, e->val, u)) return;
    }
  }
}

/* ----------------------------------------------------------------------------
   Variante « string map »
---------------------------------------------------------------------------- */
static uint64_t vt__hash_cstr(const void* k, void* udata) {
  (void)udata;
  const char* s = (const char*)k;
  return vt__fnv1a64(s, strlen(s));
}
static int vt__eq_cstr(const void* a, const void* b, void* udata) {
  (void)udata; return strcmp((const char*)a, (const char*)b) == 0;
}
static void vt__kfree_cstr(void* k, void* udata) {
  (void)udata; free(k);
}
static char* vt__strdup(const char* s){
  size_t n = strlen(s)+1;
  char* d = (char*)malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

vt_hashmap* vt_hashmap_new_string(void) {
  return vt_hashmap_new(vt__hash_cstr, vt__eq_cstr, vt__kfree_cstr, NULL);
}
int vt_hashmap_put_str(vt_hashmap* m, const char* key, void* value) {
  char* dup = vt__strdup(key);
  if (!dup) return -1;
  if (vt_hashmap_put(m, dup, value) != 0) { free(dup); return -1; }
  return 0;
}
int vt_hashmap_get_str(const vt_hashmap* m, const char* key, void** out_val) {
  return vt_hashmap_get(m, key, out_val);
}
int vt_hashmap_del_str(vt_hashmap* m, const char* key) {
  return vt_hashmap_del(m, key);
}

/* ----------------------------------------------------------------------------
   Tests optionnels
   cc -std=c17 -DVT_HASHMAP_TEST hashmap.c
---------------------------------------------------------------------------- */
#ifdef VT_HASHMAP_TEST
#include <stdio.h>
static int dump_it(void* k, void* v, void* u){
  (void)u; printf("%s => %p\n", (char*)k, v); return 0;
}
int main(void){
  vt_hashmap* m = vt_hashmap_new_string();
  for (int i=0;i<10000;i++){
    char buf[32]; snprintf(buf,sizeof buf,"key_%d",i);
    if (vt_hashmap_put_str(m, buf, (void*)(uintptr_t)i)!=0) { puts("put fail"); return 1; }
  }
  void* out=0;
  if (vt_hashmap_get_str(m, "key_42", &out)) printf("key_42 => %zu\n",(size_t)out);
  vt_hashmap_del_str(m, "key_42");
  printf("len=%zu cap=%zu\n", vt_hashmap_len(m), vt_hashmap_capacity(m));
  vt_hashmap_foreach(m, dump_it, NULL);
  vt_hashmap_free(m);
  return 0;
}
#endif
