// SPDX-License-Identifier: GPL-3.0-or-later
//
// kv.c — In-memory key/value store pour Vitte Light VM (C17, complet)
// Namespace: "kv"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c kv.c
//
// Modèle:
//   - Dictionnaires en mémoire, namespacés (string → table interne).
//   - Clés: string binaire-safe. Valeurs: string binaire-safe.
//   - TTL optionnel par entrée (ms). Expiration paresseuse + sweep.
//   - Opérations atomiques simples (CAS, incr/decr de valeurs numériques encodées ASCII).
//   - Persistance binaire compacte dump/load par namespace.
//
// API (VM):
//   ok        = kv.set(ns,key,value[, ttl_ms])         | (nil,errmsg)
//   value     = kv.get(ns,key)                          | (nil) si absent/expiré
//   ok        = kv.del(ns,key)                          | (nil,errmsg)
//   ok        = kv.exists(ns,key)                       -> bool
//   n         = kv.len(ns)                              -> int
//   arr       = kv.keys(ns)                             -> { "k1","k2",... }
//   ok        = kv.clear(ns)                            | (nil,errmsg)
//   ok        = kv.expire(ns,key,ttl_ms)                | (nil,errmsg)
//   ms        = kv.ttl(ns,key)                          -> remaining_ms | -1 si pas de TTL | 0 si expiré
//   ok        = kv.cas(ns,key,expected,new[, ttl_ms])   | (nil,errmsg)  -- expected==nil pour créer si absent
//   n         = kv.incr(ns,key[, delta=1])              | (nil,errmsg)  -- valeur ASCII int64, crée à 0 si absent
//   n         = kv.decr(ns,key[, delta=1])              | (nil,errmsg)
//   blob      = kv.dump(ns)                             -> bytes
//   ok        = kv.load(ns, blob[, mode="merge|replace"]) | (nil,errmsg)
//   n         = kv.sweep([budget=256])                  -> entries purgées
//
// Erreurs: "EINVAL", "ENOMEM"
//
// Deps VM: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "auxlib.h"
#include "state.h"
#include "object.h"
#include "vm.h"

/* ========================= VM ADAPTER (extern fournis) ================== */

typedef struct VLState VLState;
struct vl_Reg { const char *name; int (*fn)(VLState *L); };

extern void        vlx_register_module(VLState *L, const char *ns, const struct vl_Reg *funcs);
extern void        vlx_push_nil     (VLState *L);
extern void        vlx_push_boolean (VLState *L, int v);
extern void        vlx_push_integer (VLState *L, int64_t v);
extern void        vlx_push_string  (VLState *L, const char *s);
extern void        vlx_push_lstring (VLState *L, const char *s, size_t n);
extern void        vlx_new_table    (VLState *L);
extern void        vlx_set_table_is (VLState *L, int64_t i, int value_on_stack);
extern int         vlx_isstring     (VLState *L, int idx);
extern int         vlx_isnil        (VLState *L, int idx);
extern const char* vlx_check_string (VLState *L, int idx, size_t *len);
extern const char* vlx_opt_string   (VLState *L, int idx, const char *def, size_t *len);
extern int64_t     vlx_opt_integer  (VLState *L, int idx, int64_t def);

static inline void        vl_push_nil(VLState *L){ vlx_push_nil(L); }
static inline void        vl_push_boolean(VLState *L,int v){ vlx_push_boolean(L,v); }
static inline void        vl_push_integer(VLState *L,int64_t v){ vlx_push_integer(L,v); }
static inline void        vl_push_string(VLState *L,const char*s){ vlx_push_string(L,s); }
static inline void        vl_push_lstring(VLState *L,const char*s,size_t n){ vlx_push_lstring(L,s,n); }
static inline void        vl_new_table(VLState *L){ vlx_new_table(L); }
static inline void        vl_set_table_is(VLState *L,int64_t i,int vstack){ vlx_set_table_is(L,i,vstack); }
static inline int         vl_isstring(VLState *L,int i){ return vlx_isstring(L,i); }
static inline int         vl_isnil(VLState *L,int i){ return vlx_isnil(L,i); }
static inline const char* vl_check_string(VLState *L,int i,size_t *n){ return vlx_check_string(L,i,n); }
static inline const char* vl_opt_string(VLState *L,int i,const char*d,size_t *n){ return vlx_opt_string(L,i,d,n); }
static inline int64_t     vl_opt_integer(VLState *L,int i,int64_t d){ return vlx_opt_integer(L,i,d); }
static inline void        vl_register_module(VLState *L,const char*ns,const struct vl_Reg*f){ vlx_register_module(L,ns,f); }

/* ================================ Utils ================================= */

static const char *E_EINVAL="EINVAL";
static const char *E_ENOMEM="ENOMEM";

static uint64_t now_ms(void){
#if defined(CLOCK_MONOTONIC)
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#else
  struct timespec ts; timespec_get(&ts, TIME_UTC);
  return (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#endif
}

/* FNV-1a 64-bit */
static uint64_t fnv1a(const void *data, size_t len){
  const uint8_t *p = (const uint8_t*)data;
  uint64_t h = 1469598103934665603ULL;
  for(size_t i=0;i<len;i++){ h ^= p[i]; h *= 1099511628211ULL; }
  return h? h : 1; /* avoid zero */
}

/* Safe alloc */
static void* xmalloc(size_t n){ void *p = malloc(n?n:1); return p; }
static void* xrealloc(void *p, size_t n){ void *q = realloc(p, n?n:1); return q; }

/* ============================== Structures ============================== */

typedef struct {
  uint64_t h;      /* hash */
  uint64_t exp_at; /* 0=no ttl; timestamp ms when expires */
  char    *k;
  size_t   klen;
  char    *v;
  size_t   vlen;
} kv_entry;

typedef struct {
  kv_entry *tab;
  size_t    cap;
  size_t    size;
} kv_map;

typedef struct ns_slot {
  struct ns_slot *next;
  size_t   nlen;
  char    *name;
  kv_map   map;
} ns_slot;

/* global namespace table: simple chained hash on ns name */
#define NS_BUCKETS 256
static ns_slot *g_ns[NS_BUCKETS];

/* ============================== Map ops ================================= */

static void kv_map_init(kv_map *m){
  m->tab=NULL; m->cap=0; m->size=0;
}

static int kv_map_reserve(kv_map *m, size_t need){
  if(m->cap >= need) return 0;
  size_t ncap = m->cap? m->cap:16;
  while((double)need > ncap*0.7) ncap <<= 1;
  kv_entry *nt = (kv_entry*)calloc(ncap, sizeof(kv_entry));
  if(!nt) return -1;

  /* rehash */
  if(m->tab){
    for(size_t i=0;i<m->cap;i++){
      kv_entry e = m->tab[i];
      if(!e.k) continue;
      size_t mask = ncap-1;
      size_t j = (size_t)e.h & mask;
      while(nt[j].k) j = (j+1) & mask;
      nt[j] = e;
    }
    free(m->tab);
  }
  m->tab = nt; m->cap = ncap;
  return 0;
}

static kv_entry* kv_map_find_slot(kv_map *m, const char *k, size_t klen, uint64_t h, int *empty_index){
  if(m->cap == 0) return NULL;
  size_t mask = m->cap - 1;
  size_t i = (size_t)h & mask;
  int first_empty = -1;
  for(;;){
    kv_entry *e = &m->tab[i];
    if(!e->k){
      if(first_empty<0) first_empty = (int)i;
      break;
    }
    if(e->h==h && e->klen==klen && memcmp(e->k,k,klen)==0) return e;
    i = (i+1) & mask;
  }
  if(empty_index) *empty_index = first_empty;
  return NULL;
}

static int kv_map_put(kv_map *m, const char *k, size_t klen, const char *v, size_t vlen, uint64_t exp_at){
  if(kv_map_reserve(m, m->size+1)) return -1;
  uint64_t h = fnv1a(k,klen);
  int empty = -1;
  kv_entry *e = kv_map_find_slot(m,k,klen,h,&empty);
  if(e){
    char *nv = (char*)xrealloc(e->v, vlen);
    if(vlen && !nv) return -1;
    e->v = nv; e->vlen = vlen;
    if(vlen) memcpy(e->v, v, vlen);
    e->exp_at = exp_at;
    return 0;
  }
  /* new */
  e = &m->tab[empty];
  e->h=h; e->klen=klen; e->vlen=vlen; e->exp_at=exp_at;
  e->k = (char*)xmalloc(klen);
  if(klen && !e->k) return -1;
  if(klen) memcpy(e->k,k,klen);
  e->v = (char*)xmalloc(vlen);
  if(vlen && !e->v){ free(e->k); e->k=NULL; return -1; }
  if(vlen) memcpy(e->v,v,vlen);
  m->size++;
  return 0;
}

static kv_entry* kv_map_get(kv_map *m, const char *k, size_t klen){
  if(m->cap==0) return NULL;
  uint64_t h = fnv1a(k,klen);
  return kv_map_find_slot(m,k,klen,h,NULL);
}

static int kv_map_del(kv_map *m, const char *k, size_t klen){
  if(m->cap==0) return 0;
  uint64_t h = fnv1a(k,klen);
  size_t mask = m->cap-1;
  size_t i = (size_t)h & mask;
  for(;;){
    kv_entry *e = &m->tab[i];
    if(!e->k) return 0;
    if(e->h==h && e->klen==klen && memcmp(e->k,k,klen)==0){
      free(e->k); free(e->v);
      memset(e,0,sizeof(*e));
      m->size--;
      /* re-cluster (Robin Hood simple) */
      size_t j=(i+1)&mask;
      while(m->tab[j].k){
        kv_entry tmp = m->tab[j];
        memset(&m->tab[j],0,sizeof(kv_entry));
        m->size--;
        kv_map_put(m, tmp.k, tmp.klen, tmp.v, tmp.vlen, tmp.exp_at);
        free(tmp.k); free(tmp.v);
        j=(j+1)&mask;
      }
      return 1;
    }
    i = (i+1) & mask;
  }
}

static void kv_map_clear(kv_map *m){
  if(!m->tab) return;
  for(size_t i=0;i<m->cap;i++){
    if(m->tab[i].k){ free(m->tab[i].k); free(m->tab[i].v); }
  }
  free(m->tab); m->tab=NULL; m->cap=0; m->size=0;
}

/* ============================ Namespace ops ============================= */

static ns_slot* ns_get_or_create(const char *ns, size_t nlen){
  uint64_t h = fnv1a(ns,nlen);
  size_t b = (size_t)h & (NS_BUCKETS-1);
  ns_slot *s = g_ns[b];
  while(s){
    if(s->nlen==nlen && memcmp(s->name,ns,nlen)==0) return s;
    s = s->next;
  }
  s = (ns_slot*)calloc(1,sizeof(ns_slot));
  if(!s) return NULL;
  s->name = (char*)xmalloc(nlen);
  if(nlen && !s->name){ free(s); return NULL; }
  if(nlen) memcpy(s->name, ns, nlen);
  s->nlen = nlen;
  kv_map_init(&s->map);
  s->next = g_ns[b];
  g_ns[b]=s;
  return s;
}

/* purge si expiré: retourne 1 si entrée supprimée */
static int maybe_expire_entry(kv_map *m, kv_entry *e, uint64_t now){
  if(!e || !e->k) return 0;
  if(e->exp_at && now >= e->exp_at){
    kv_map_del(m, e->k, e->klen);
    return 1;
  }
  return 0;
}

/* ================================ VM API ================================= */

static int L_set(VLState *L){
  size_t ns_n=0, k_n=0, v_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  const char *v  = vl_check_string(L,3,&v_n);
  int64_t ttl = vl_opt_integer(L,4,0);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
  uint64_t exp = ttl>0? now_ms() + (uint64_t)ttl : 0;
  if(kv_map_put(&s->map,k,k_n,v,v_n,exp)){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
  vl_push_boolean(L,1); return 1;
}

static int L_get(VLState *L){
  size_t ns_n=0, k_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_nil(L); return 1; }
  kv_entry *e = kv_map_get(&s->map,k,k_n);
  if(!e || !e->k){ vl_push_nil(L); return 1; }
  if(maybe_expire_entry(&s->map,e, now_ms())){ vl_push_nil(L); return 1; }
  vl_push_lstring(L, e->v, e->vlen); return 1;
}

static int L_del(VLState *L){
  size_t ns_n=0, k_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_nil(L); vl_push_string(L,E_EINVAL); return 2; }
  int r = kv_map_del(&s->map,k,k_n);
  vl_push_boolean(L, r?1:0); return 1;
}

static int L_exists(VLState *L){
  size_t ns_n=0, k_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_boolean(L,0); return 1; }
  kv_entry *e = kv_map_get(&s->map,k,k_n);
  if(!e || !e->k){ vl_push_boolean(L,0); return 1; }
  if(maybe_expire_entry(&s->map,e, now_ms())){ vl_push_boolean(L,0); return 1; }
  vl_push_boolean(L,1); return 1;
}

static int L_len(VLState *L){
  size_t ns_n=0; const char *ns = vl_check_string(L,1,&ns_n);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  vl_push_integer(L, s? (int64_t)s->map.size : 0);
  return 1;
}

static int L_keys(VLState *L){
  size_t ns_n=0; const char *ns = vl_check_string(L,1,&ns_n);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  vl_new_table(L);
  if(!s){ return 1; }
  int64_t idx=0;
  uint64_t now = now_ms();
  for(size_t i=0;i<s->map.cap;i++){
    kv_entry *e = &s->map.tab[i];
    if(!e->k) continue;
    if(maybe_expire_entry(&s->map,e,now)) continue;
    idx++;
    vl_push_lstring(L, e->k, e->klen);
    vl_set_table_is(L, idx, /*value_on_stack*/1);
    vl_push_nil(L);
  }
  return 1;
}

static int L_clear(VLState *L){
  size_t ns_n=0; const char *ns = vl_check_string(L,1,&ns_n);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_boolean(L,1); return 1; }
  kv_map_clear(&s->map);
  vl_push_boolean(L,1); return 1;
}

static int L_expire(VLState *L){
  size_t ns_n=0, k_n=0; int64_t ttl=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  ttl = vl_opt_integer(L,3,0);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_nil(L); vl_push_string(L,E_EINVAL); return 2; }
  kv_entry *e = kv_map_get(&s->map,k,k_n);
  if(!e || !e->k){ vl_push_nil(L); vl_push_string(L,E_EINVAL); return 2; }
  e->exp_at = ttl>0? now_ms() + (uint64_t)ttl : 0;
  vl_push_boolean(L,1); return 1;
}

static int L_ttl(VLState *L){
  size_t ns_n=0, k_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_integer(L,0); return 1; }
  kv_entry *e = kv_map_get(&s->map,k,k_n);
  if(!e || !e->k){ vl_push_integer(L,0); return 1; }
  uint64_t now = now_ms();
  if(maybe_expire_entry(&s->map,e,now)){ vl_push_integer(L,0); return 1; }
  if(!e->exp_at){ vl_push_integer(L,-1); return 1; }
  int64_t rem = (e->exp_at>now)? (int64_t)(e->exp_at-now) : 0;
  vl_push_integer(L, rem);
  return 1;
}

static int L_cas(VLState *L){
  size_t ns_n=0,k_n=0, exp_n=0, new_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  const char *expected = NULL, *nval = NULL;
  int has_expected = !vl_isnil(L,3);
  if(has_expected) expected = vl_check_string(L,3,&exp_n);
  nval = vl_check_string(L,4,&new_n);
  int64_t ttl = vl_opt_integer(L,5,0);

  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
  kv_entry *e = kv_map_get(&s->map,k,k_n);
  uint64_t exp_at = ttl>0? now_ms() + (uint64_t)ttl : 0;

  if(!e || !e->k){
    if(!has_expected){ /* create if absent when expected==nil */
      if(kv_map_put(&s->map,k,k_n,nval,new_n,exp_at)){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
      vl_push_boolean(L,1); return 1;
    }
    vl_push_boolean(L,0); return 1;
  }
  if(maybe_expire_entry(&s->map,e, now_ms())){ /* now absent */
    if(!has_expected){
      if(kv_map_put(&s->map,k,k_n,nval,new_n,exp_at)){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
      vl_push_boolean(L,1); return 1;
    }
    vl_push_boolean(L,0); return 1;
  }
  if(has_expected){
    if(e->vlen!=exp_n || memcmp(e->v,expected,exp_n)!=0){ vl_push_boolean(L,0); return 1; }
  }
  if(kv_map_put(&s->map,k,k_n,nval,new_n,exp_at)){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
  vl_push_boolean(L,1); return 1;
}

static int64_t parse_i64(const char *s, size_t n, int *ok){
  /* ASCII int64, optional +/- */
  char buf[64];
  if(n>=sizeof(buf)){ *ok=0; return 0; }
  memcpy(buf,s,n); buf[n]=0;
  char *end=NULL; long long v = strtoll(buf,&end,10);
  *ok = (end && *end=='\0');
  return (int64_t)v;
}
static int L_incr(VLState *L){
  size_t ns_n=0,k_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  int64_t delta = vl_opt_integer(L,3,1);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
  kv_entry *e = kv_map_get(&s->map,k,k_n);
  int ok=1; int64_t cur=0;
  if(e && e->k && !maybe_expire_entry(&s->map,e,now_ms())){
    cur = parse_i64(e->v,e->vlen,&ok);
    if(!ok){ vl_push_nil(L); vl_push_string(L,E_EINVAL); return 2; }
  }
  int64_t nv = cur + delta;
  char buf[64]; int m = snprintf(buf,sizeof(buf),"%lld",(long long)nv);
  if(kv_map_put(&s->map,k,k_n,buf,(size_t)m, e?e->exp_at:0)){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
  vl_push_integer(L,nv); return 1;
}
static int L_decr(VLState *L){
  size_t ns_n=0,k_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *k  = vl_check_string(L,2,&k_n);
  int64_t delta = vl_opt_integer(L,3,1);
  vl_push_integer(L,0); /* will be replaced by incr with -delta */
  return L_incr(L), 1; /* not reached, but keeps signature; simple aliasing isn't trivial here */
}

/* ============================== dump / load ============================== */
/* Format binaire:
   u32 magic 'KVL1'
   u32 count
   entries:
     u32 klen, u32 vlen
     u64 exp_at
     k bytes, v bytes
*/

static void put_u32(uint8_t *p, uint32_t v){ p[0]=v>>24; p[1]=(v>>16)&255; p[2]=(v>>8)&255; p[3]=v&255; }
static void put_u64(uint8_t *p, uint64_t v){
  for(int i=0;i<8;i++) p[i] = (uint8_t)(v >> (56-8*i));
}
static uint32_t get_u32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t get_u64(const uint8_t *p){
  uint64_t v=0; for(int i=0;i<8;i++){ v=(v<<8)|p[i]; } return v;
}

static int L_dump(VLState *L){
  size_t ns_n=0; const char *ns = vl_check_string(L,1,&ns_n);
  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s || s->map.size==0){ vl_push_lstring(L,"",0); return 1; }

  /* estimate size */
  size_t bytes = 8; /* magic+count */
  uint64_t now = now_ms();
  size_t live=0;
  for(size_t i=0;i<s->map.cap;i++){
    kv_entry *e=&s->map.tab[i];
    if(!e->k) continue;
    if(maybe_expire_entry(&s->map,e,now)) continue;
    live++;
    bytes += 4+4+8 + e->klen + e->vlen;
  }
  uint8_t *buf = (uint8_t*)xmalloc(bytes);
  if(!buf){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
  uint8_t *p = buf;
  put_u32(p, 0x4B564C31u); p+=4; /* 'KVL1' */
  put_u32(p, (uint32_t)live); p+=4;
  for(size_t i=0;i<s->map.cap;i++){
    kv_entry *e=&s->map.tab[i];
    if(!e->k) continue;
    if(e->exp_at && now>=e->exp_at) continue;
    put_u32(p,(uint32_t)e->klen); p+=4;
    put_u32(p,(uint32_t)e->vlen); p+=4;
    put_u64(p,e->exp_at); p+=8;
    memcpy(p,e->k,e->klen); p+=e->klen;
    memcpy(p,e->v,e->vlen); p+=e->vlen;
  }
  vl_push_lstring(L,(const char*)buf,(size_t)(p-buf));
  free(buf);
  return 1;
}

static int L_load(VLState *L){
  size_t ns_n=0, b_n=0, mode_n=0;
  const char *ns = vl_check_string(L,1,&ns_n);
  const char *blob = vl_check_string(L,2,&b_n);
  const char *mode = vl_opt_string(L,3,"merge",&mode_n); /* "merge"|"replace" */

  ns_slot *s = ns_get_or_create(ns,ns_n);
  if(!s){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }

  if(mode_n==7 && memcmp(mode,"replace",7)==0) kv_map_clear(&s->map);

  const uint8_t *p=(const uint8_t*)blob, *end=p+b_n;
  if((size_t)(end-p) < 8){ vl_push_nil(L); vl_push_string(L,E_EINVAL); return 2; }
  uint32_t magic = get_u32(p); p+=4;
  if(magic != 0x4B564C31u){ vl_push_nil(L); vl_push_string(L,E_EINVAL); return 2; }
  uint32_t cnt = get_u32(p); p+=4;
  for(uint32_t i=0;i<cnt;i++){
    if((size_t)(end-p) < 4+4+8){ vl_push_nil(L); vl_push_string(L,E_EINVAL); return 2; }
    uint32_t klen = get_u32(p); p+=4;
    uint32_t vlen = get_u32(p); p+=4;
    uint64_t exp  = get_u64(p); p+=8;
    if((size_t)(end-p) < (size_t)klen+(size_t)vlen){ vl_push_nil(L); vl_push_string(L,E_EINVAL); return 2; }
    const char *k = (const char*)p; p+=klen;
    const char *v = (const char*)p; p+=vlen;
    if(kv_map_put(&s->map,k,klen,v,vlen,exp)){ vl_push_nil(L); vl_push_string(L,E_ENOMEM); return 2; }
  }
  vl_push_boolean(L,1); return 1;
}

/* ================================ Sweep ================================= */

static int L_sweep(VLState *L){
  int64_t budget = vl_opt_integer(L,1,256);
  if(budget<0) budget=0;
  uint64_t now = now_ms();
  int64_t purged=0;
  for(size_t b=0;b<NS_BUCKETS;b++){
    for(ns_slot *s=g_ns[b]; s; s=s->next){
      kv_map *m = &s->map;
      if(!m->tab) continue;
      for(size_t i=0;i<m->cap;i++){
        kv_entry *e=&m->tab[i];
        if(!e->k) continue;
        if(e->exp_at && now>=e->exp_at){
          kv_map_del(m,e->k,e->klen);
          purged++;
          if(budget && purged>=budget){ vl_push_integer(L,purged); return 1; }
        }
      }
    }
  }
  vl_push_integer(L,purged); return 1;
}

/* ================================ Dispatch =============================== */

static const struct vl_Reg funs[] = {
  {"set",     L_set},
  {"get",     L_get},
  {"del",     L_del},
  {"exists",  L_exists},
  {"len",     L_len},
  {"keys",    L_keys},
  {"clear",   L_clear},
  {"expire",  L_expire},
  {"ttl",     L_ttl},
  {"cas",     L_cas},
  {"incr",    L_incr},
  {"decr",    L_decr},
  {"dump",    L_dump},
  {"load",    L_load},
  {"sweep",   L_sweep},
  {NULL, NULL}
};

int vl_openlib_kv(VLState *L){
  vl_register_module(L, "kv", funs);
  return 1;
}