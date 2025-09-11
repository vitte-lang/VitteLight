// SPDX-License-Identifier: GPL-3.0-or-later
//
// kafka.c — Client Kafka pour Vitte Light VM (C17, complet, librdkafka)
// Namespace: "kafka"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_RDKAFKA -c kafka.c
//   cc ... kafka.o -lrdkafka
//
// Modèle:
//   - Backend: librdkafka (rdkafka.h).
//   - Producteur et consommateur simples, configs via table clé→valeur.
//   - Chaînes binaires via vl_push_lstring. ENOSYS si librdkafka absente.
//
// API (VM):
//   p = kafka.producer(conf_tbl)                 -> handle | (nil,errmsg)
//   c = kafka.consumer(conf_tbl, topics_tbl)     -> handle | (nil,errmsg)
//   ok = kafka.produce(p, topic, value[, key][, headers_tbl][, partition][, timestamp_ms])
//   n  = kafka.poll(h[, timeout_ms=0])           -> events processed
//   ok = kafka.flush(p[, timeout_ms=5000])
//   t,part,off,key,val,hdrs = kafka.consume(c[, timeout_ms=1000]) | (nil,"EAGAIN")
//   ok = kafka.commit(c)                         -> manual commit
//   ok = kafka.close(h)                          -> libère handle
//   s = kafka.version()                          -> "librdkafka <ver>"
//
// Erreurs: "EINVAL", "ENOSYS", "ENOMEM", "EKAFKA"
//
// Deps VM: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "auxlib.h"
#include "state.h"
#include "object.h"
#include "vm.h"

#ifdef VL_HAVE_RDKAFKA
  #include <librdkafka/rdkafka.h>
#endif

/* ========================= VM ADAPTER (extern fournis) ================== */

typedef struct VLState VLState;
struct vl_Reg { const char *name; int (*fn)(VLState *L); };

extern void        vlx_register_module(VLState *L, const char *ns, const struct vl_Reg *funcs);
extern void        vlx_push_nil     (VLState *L);
extern void        vlx_push_boolean (VLState *L, int v);
extern void        vlx_push_integer (VLState *L, int64_t v);
extern void        vlx_push_string  (VLState *L, const char *s);
extern void        vlx_push_lstring (VLState *L, const char *s, size_t n);
extern int         vlx_istable      (VLState *L, int idx);
extern int         vlx_isstring     (VLState *L, int idx);
extern int64_t     vlx_opt_integer  (VLState *L, int idx, int64_t def);
extern int         vlx_opt_boolean  (VLState *L, int idx, int def);
extern const char* vlx_check_string (VLState *L, int idx, size_t *len);
extern int         vlx_table_foreach_kv_string(VLState *L, int idx,
                    int (*cb)(const char *k, const char *v, void *ud), void *ud);
extern void        vlx_new_table    (VLState *L);
extern void        vlx_set_table_kv (VLState *L, const char *k, const char *v);
extern void        vlx_set_table_kvi(VLState *L, const char *k, int64_t v);

static inline void vl_push_nil(VLState *L){ vlx_push_nil(L); }
static inline void vl_push_boolean(VLState *L,int v){ vlx_push_boolean(L,v); }
static inline void vl_push_integer(VLState *L,int64_t v){ vlx_push_integer(L,v); }
static inline void vl_push_string(VLState *L,const char*s){ vlx_push_string(L,s); }
static inline void vl_push_lstring(VLState *L,const char*s,size_t n){ vlx_push_lstring(L,s,n); }
static inline int  vl_istable(VLState *L,int i){ return vlx_istable(L,i); }
static inline int  vl_isstring(VLState *L,int i){ return vlx_isstring(L,i); }
static inline int64_t vl_opt_integer(VLState *L,int i,int64_t d){ return vlx_opt_integer(L,i,d); }
static inline int  vl_opt_boolean(VLState *L,int i,int d){ return vlx_opt_boolean(L,i,d); }
static inline const char* vl_check_string(VLState *L,int i,size_t*n){ return vlx_check_string(L,i,n); }
static inline void vl_new_table(VLState *L){ vlx_new_table(L); }
static inline void vl_set_table_kv(VLState *L,const char*k,const char*v){ vlx_set_table_kv(L,k,v); }
static inline void vl_set_table_kvi(VLState *L,const char*k,int64_t v){ vlx_set_table_kvi(L,k,v); }
static inline int  vl_table_foreach_kv_string(VLState *L, int idx,
                      int (*cb)(const char*, const char*, void*), void *ud){
  return vlx_table_foreach_kv_string(L, idx, cb, ud);
}
static inline void vl_register_module(VLState *L,const char*ns,const struct vl_Reg*funcs){ vlx_register_module(L,ns,funcs); }

/* ================================ Const ================================= */

static const char *E_EINVAL = "EINVAL";
static const char *E_ENOSYS = "ENOSYS";
static const char *E_ENOMEM = "ENOMEM";
static const char *E_EKAFKA = "EKAFKA";
static const char *E_EAGAIN = "EAGAIN";

/* =============================== Handles ================================ */

#ifdef VL_HAVE_RDKAFKA

typedef enum { H_PRODUCER=1, H_CONSUMER=2 } kh_kind;

typedef struct {
  kh_kind kind;
  rd_kafka_t *rk;
} kh;

#define MAX_KH 1024
static kh *g_tab[MAX_KH];

static int kh_put(kh *h){
  for(int i=1;i<MAX_KH;i++){ if(!g_tab[i]){ g_tab[i]=h; return i; } }
  return 0;
}
static kh* kh_get(int id){ return (id>0 && id<MAX_KH)? g_tab[id]:NULL; }
static void kh_del(int id){ if(id>0 && id<MAX_KH) g_tab[id]=NULL; }

static int conf_cb(const char *k, const char *v, void *ud){
  rd_kafka_conf_t *conf = (rd_kafka_conf_t*)ud;
  if(rd_kafka_conf_set(conf, k, v, NULL, 0) != RD_KAFKA_CONF_OK) return -1;
  return 0;
}
static int hdrs_cb(const char *k, const char *v, void *ud){
  rd_kafka_headers_t *hs = (rd_kafka_headers_t*)ud;
  if(rd_kafka_header_add(hs, k, -1, v, (size_t)strlen(v)) != RD_KAFKA_RESP_ERR_NO_ERROR) return -1;
  return 0;
}

#endif

/* ================================ Helpers =============================== */

static void push_err(VLState *L, const char *e){ vl_push_nil(L); vl_push_string(L, e); }

/* =============================== Producer =============================== */

static int kf_producer(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  push_err(L, E_ENOSYS); return 2;
#else
  if(!vl_istable(L,1)){ push_err(L,E_EINVAL); return 2; }

  char errstr[512];
  rd_kafka_conf_t *conf = rd_kafka_conf_new();
  if(vl_table_foreach_kv_string(L, 1, conf_cb, conf)!=0){
    rd_kafka_conf_destroy(conf);
    push_err(L, E_EINVAL); return 2;
  }
  rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
  if(!rk){ push_err(L, E_EKAFKA); return 2; }

  kh *h = (kh*)calloc(1,sizeof(kh)); if(!h){ rd_kafka_destroy(rk); push_err(L,E_ENOMEM); return 2; }
  h->kind = H_PRODUCER; h->rk = rk;
  int id = kh_put(h);
  if(!id){ free(h); rd_kafka_destroy(rk); push_err(L,E_ENOMEM); return 2; }
  vl_push_integer(L, id);
  return 1;
#endif
}

/* =============================== Consumer =============================== */

static int kf_consumer(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  push_err(L, E_ENOSYS); return 2;
#else
  if(!vl_istable(L,1) || !vl_istable(L,2)){ push_err(L,E_EINVAL); return 2; }

  char errstr[512];
  rd_kafka_conf_t *conf = rd_kafka_conf_new();
  if(vl_table_foreach_kv_string(L, 1, conf_cb, conf)!=0){
    rd_kafka_conf_destroy(conf);
    push_err(L, E_EINVAL); return 2;
  }
  rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
  if(!rk){ push_err(L, E_EKAFKA); return 2; }

  /* Assign group handlers minimal: enable consumption via poll */
  rd_kafka_poll_set_consumer(rk);

  /* Build topic list from table 2: keys ignored, values are topic names */
  rd_kafka_topic_partition_list_t *subs = rd_kafka_topic_partition_list_new(16);
  int add_topic_cb(const char *k, const char *v, void *ud){
    (void)k; rd_kafka_topic_partition_list_add((rd_kafka_topic_partition_list_t*)ud, v, RD_KAFKA_PARTITION_UA);
    return 0;
  }
  if(vl_table_foreach_kv_string(L, 2, add_topic_cb, subs) != 0){
    rd_kafka_topic_partition_list_destroy(subs);
    rd_kafka_destroy(rk);
    push_err(L, E_EINVAL); return 2;
  }
  rd_kafka_resp_err_t e = rd_kafka_subscribe(rk, subs);
  rd_kafka_topic_partition_list_destroy(subs);
  if(e != RD_KAFKA_RESP_ERR_NO_ERROR){
    rd_kafka_destroy(rk); push_err(L, E_EKAFKA); return 2;
  }

  kh *h = (kh*)calloc(1,sizeof(kh)); if(!h){ rd_kafka_destroy(rk); push_err(L,E_ENOMEM); return 2; }
  h->kind = H_CONSUMER; h->rk = rk;
  int id = kh_put(h);
  if(!id){ free(h); rd_kafka_destroy(rk); push_err(L,E_ENOMEM); return 2; }
  vl_push_integer(L, id);
  return 1;
#endif
}

/* ================================ Produce =============================== */

static int kf_produce(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  push_err(L, E_ENOSYS); return 2;
#else
  int64_t hid = vlx_opt_integer(L, 1, 0);
  size_t tlen=0,vlen=0,klen=0;
  const char *topic = vl_check_string(L, 2, &tlen);
  const char *val   = vl_check_string(L, 3, &vlen);
  const char *key   = NULL;
  int argi = 4;
  if(vl_isstring(L,argi)){ key = vl_check_string(L, argi++, &klen); }

  rd_kafka_headers_t *hdrs = NULL;
  if(vl_istable(L,argi)){
    hdrs = rd_kafka_headers_new(8);
    if(vl_table_foreach_kv_string(L, argi++, hdrs_cb, hdrs)!=0){
      rd_kafka_headers_destroy(hdrs);
      push_err(L, E_EINVAL); return 2;
    }
  }
  int32_t partition = (int32_t)vl_opt_integer(L, argi++, RD_KAFKA_PARTITION_UA);
  int64_t ts_ms     = vl_opt_integer(L, argi++, 0);

  kh *h = kh_get((int)hid);
  if(!h || h->kind!=H_PRODUCER){ push_err(L,E_EINVAL); if(hdrs) rd_kafka_headers_destroy(hdrs); return 2; }

  rd_kafka_topic_t *rkt = rd_kafka_topic_new(h->rk, topic, NULL);
  if(!rkt){ if(hdrs) rd_kafka_headers_destroy(hdrs); push_err(L,E_EKAFKA); return 2; }

  rd_kafka_resp_err_t e = rd_kafka_producev(
    h->rk,
    RD_KAFKA_V_RKT(rkt),
    RD_KAFKA_V_PARTITION(partition),
    RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
    RD_KAFKA_V_VALUE((void*)val, (size_t)vlen),
    RD_KAFKA_V_KEY((void*)key, key? (size_t)klen:0),
    RD_KAFKA_V_HEADERS(hdrs),
    RD_KAFKA_V_TIMESTAMP(ts_ms? ts_ms : 0),
    RD_KAFKA_V_END);

  rd_kafka_topic_destroy(rkt);
  if(hdrs) rd_kafka_headers_destroy(hdrs);
  if(e != RD_KAFKA_RESP_ERR_NO_ERROR){ push_err(L,E_EKAFKA); return 2; }

  vl_push_boolean(L,1);
  return 1;
#endif
}

/* ================================ Poll/Flush ============================= */

static int kf_poll(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  push_err(L, E_ENOSYS); return 2;
#else
  int64_t hid = vlx_opt_integer(L, 1, 0);
  int timeout_ms = (int)vl_opt_integer(L, 2, 0);
  kh *h = kh_get((int)hid);
  if(!h){ push_err(L,E_EINVAL); return 2; }
  int n = rd_kafka_poll(h->rk, timeout_ms);
  vl_push_integer(L, n);
  return 1;
#endif
}

static int kf_flush(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  push_err(L, E_ENOSYS); return 2;
#else
  int64_t hid = vlx_opt_integer(L, 1, 0);
  int timeout_ms = (int)vl_opt_integer(L, 2, 5000);
  kh *h = kh_get((int)hid);
  if(!h || h->kind!=H_PRODUCER){ push_err(L,E_EINVAL); return 2; }
  rd_kafka_resp_err_t e = rd_kafka_flush(h->rk, timeout_ms);
  if(e != RD_KAFKA_RESP_ERR_NO_ERROR){ push_err(L,E_EKAFKA); return 2; }
  vl_push_boolean(L,1);
  return 1;
#endif
}

/* ================================= Consume =============================== */

static int kf_consume(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  push_err(L, E_ENOSYS); return 2;
#else
  int64_t hid = vlx_opt_integer(L, 1, 0);
  int timeout_ms = (int)vl_opt_integer(L, 2, 1000);
  kh *h = kh_get((int)hid);
  if(!h || h->kind!=H_CONSUMER){ push_err(L,E_EINVAL); return 2; }

  rd_kafka_message_t *m = rd_kafka_consumer_poll(h->rk, timeout_ms);
  if(!m){ push_err(L, E_EAGAIN); return 2; }

  if(m->err){
    rd_kafka_message_destroy(m);
    push_err(L, E_EKAFKA); return 2;
  }

  /* topic */
  vl_push_string(L, rd_kafka_topic_name(m->rkt));
  /* partition */
  vl_push_integer(L, (int64_t)m->partition);
  /* offset */
  vl_push_integer(L, (int64_t)m->offset);
  /* key */
  if(m->key && m->key_len>0) vl_push_lstring(L, (const char*)m->key, (size_t)m->key_len);
  else vl_push_lstring(L, "", 0);
  /* value */
  if(m->payload && m->len>0) vl_push_lstring(L, (const char*)m->payload, (size_t)m->len);
  else vl_push_lstring(L, "", 0);
  /* headers */
  vl_new_table(L);
  rd_kafka_headers_t *hs=NULL;
  if(rd_kafka_message_headers(m, &hs) == RD_KAFKA_RESP_ERR_NO_ERROR && hs){
    size_t cnt = rd_kafka_header_cnt(hs);
    for(size_t i=0;i<cnt;i++){
      const char *name; const void *val; size_t sz;
      if(rd_kafka_header_get_all(hs, i, &name, &val, &sz) == RD_KAFKA_RESP_ERR_NO_ERROR && name){
        vl_set_table_kv(L, name, (val && sz)? (char*)val : "");
      }
    }
  }
  rd_kafka_message_destroy(m);
  return 6;
#endif
}

/* ================================= Commit ================================ */

static int kf_commit(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  push_err(L, E_ENOSYS); return 2;
#else
  int64_t hid = vlx_opt_integer(L, 1, 0);
  kh *h = kh_get((int)hid);
  if(!h || h->kind!=H_CONSUMER){ push_err(L,E_EINVAL); return 2; }
  rd_kafka_resp_err_t e = rd_kafka_commit(h->rk, NULL, 0);
  if(e != RD_KAFKA_RESP_ERR_NO_ERROR){ push_err(L,E_EKAFKA); return 2; }
  vl_push_boolean(L,1);
  return 1;
#endif
}

/* ================================= Close ================================= */

static int kf_close(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  push_err(L, E_ENOSYS); return 2;
#else
  int64_t hid = vlx_opt_integer(L, 1, 0);
  kh *h = kh_get((int)hid);
  if(!h){ push_err(L,E_EINVAL); return 2; }
  if(h->kind == H_CONSUMER){
    rd_kafka_consumer_close(h->rk);
  }
  rd_kafka_destroy(h->rk);
  kh_del((int)hid);
  free(h);
  vl_push_boolean(L,1);
  return 1;
#endif
}

/* ================================= Misc ================================= */

static int kf_version(VLState *L){
#ifndef VL_HAVE_RDKAFKA
  vl_push_string(L, "librdkafka (absent)"); return 1;
#else
  char buf[128];
  snprintf(buf,sizeof(buf),"librdkafka %s", rd_kafka_version_str());
  vl_push_string(L, buf);
  return 1;
#endif
}

/* ================================ Dispatch =============================== */

static const struct vl_Reg funs[] = {
  {"producer", kf_producer},
  {"consumer", kf_consumer},
  {"produce",  kf_produce},
  {"poll",     kf_poll},
  {"flush",    kf_flush},
  {"consume",  kf_consume},
  {"commit",   kf_commit},
  {"close",    kf_close},
  {"version",  kf_version},
  {NULL, NULL}
};

int vl_openlib_kafka(VLState *L){
  vl_register_module(L, "kafka", funs);
  return 1;
}