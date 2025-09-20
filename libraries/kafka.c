// SPDX-License-Identifier: GPL-3.0-or-later
//
// kafka.c — Apache Kafka front-end for Vitte Light VM (C17, complet)
// Namespace: "kafka"
//
// Build examples:
//   # Requiert librdkafka (https://github.com/confluentinc/librdkafka)
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_RDKAFKA -c kafka.c
//   cc ... kafka.o -lrdkafka
//
// Model:
//   - Producteur: new_producer(config kv), produce(), flush(), poll().
//   - Consommateur: new_consumer(config kv, group), subscribe(topics), poll_msg().
//   - Messages binaires (clé + payload). Copies effectuées côté C.
//   - Gestion par handles, 1 thread (pas d’IO interne).
//
// API (C symbol layer):
//   Types et erreurs:
//     enum { KF_OK=0, KF_EINVAL=-22, KF_ENOSYS=-38, KF_ENOMEM=-12, KF_EIO=-5 };
//
//   Producteur:
//     int kafka_new_producer(const char* const* conf_kv, size_t n_kv);        // >0 id | <0
//     int kafka_produce(int h, const char* topic, int32_t partition,          // 0 | <0
//                       const void* key, size_t klen,
//                       const void* payload, size_t len,
//                       int64_t timestamp_ms); // <0 → erreur
//     int kafka_flush(int h, int timeout_ms);                                  // 0|<0
//     int kafka_poll(int h, int timeout_ms);                                   // 0|<0
//
//   Consommateur:
//     int kafka_new_consumer(const char* group_id,
//                            const char* const* conf_kv, size_t n_kv);        // >0 id | <0
//     int kafka_subscribe(int h, const char* const* topics, size_t n_topics);  // 0|<0
//     // poll_msg: alloue *key/*payload (malloc). L’appelant doit free().
//     // Si pas de message dans timeout, renvoie 1 (aucun message).
//     int kafka_poll_msg(int h, int timeout_ms,
//                        char** topic, int32_t* partition, int64_t* offset,
//                        void** key, size_t* klen,
//                        void** payload, size_t* len,
//                        int64_t* timestamp_ms);                               // 0|1|<0
//     int kafka_commit(int h);                                                 // 0|<0
//
//   Commun:
//     int kafka_close(int h);                                                  // 0
//
// Notes:
//   - Couche C neutre VM. Les bindings VM doivent copier les buffers via vl_push_lstring.
//   - conf_kv: tableau pairs "key=value". Exemple: {"bootstrap.servers=localhost:9092", ...}.
//   - Erreurs: -EINVAL, -ENOSYS, -ENOMEM, -EIO. 1 = timeout sans message.
//
// Deps VM optionnels: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifndef EINVAL
#  define EINVAL 22
#endif
#ifndef ENOSYS
#  define ENOSYS 38
#endif
#ifndef ENOMEM
#  define ENOMEM 12
#endif
#ifndef EIO
#  define EIO 5
#endif

#ifndef VL_EXPORT
#  if defined(_WIN32) && !defined(__clang__)
#    define VL_EXPORT __declspec(dllexport)
#  else
#    define VL_EXPORT
#  endif
#endif

enum { KF_OK=0, KF_EINVAL=-EINVAL, KF_ENOSYS=-ENOSYS, KF_ENOMEM=-ENOMEM, KF_EIO=-EIO };

#ifdef VL_HAVE_RDKAFKA
#  include <rdkafka/rdkafka.h>
#endif

#ifndef VL_KAFKA_MAX
#  define VL_KAFKA_MAX 64
#endif

typedef enum { K_NONE=0, K_PROD, K_CONS } k_kind;

typedef struct {
  k_kind kind;
#ifdef VL_HAVE_RDKAFKA
  rd_kafka_t* rk;
  rd_kafka_conf_t* conf;
  // producteur: rien de plus
  // consommateur: subscription interne
#endif
} k_handle;

static k_handle g_k[VL_KAFKA_MAX];

// ------------------- utils -------------------
static int alloc_h(void){
  for (int i=1;i<VL_KAFKA_MAX;i++) if (g_k[i].kind==K_NONE) return i;
  return -ENOMEM;
}
static void clear_h(k_handle* h){
#ifdef VL_HAVE_RDKAFKA
  if (h->rk)   { rd_kafka_destroy(h->rk); h->rk=NULL; }
  if (h->conf) { rd_kafka_conf_destroy(h->conf); h->conf=NULL; }
#endif
  h->kind = K_NONE;
}

#ifdef VL_HAVE_RDKAFKA
static int apply_conf_pairs(rd_kafka_conf_t* conf, const char* const* kv, size_t n){
  if (!kv || n==0) return 0;
  char errstr[512];
  for (size_t i=0;i<n;i++){
    const char* p = kv[i];
    if (!p || !*p) continue;
    const char* eq = strchr(p, '=');
    if (!eq) return -EINVAL;
    size_t klen = (size_t)(eq - p);
    char* key = (char*)malloc(klen+1);
    if (!key) return -ENOMEM;
    memcpy(key, p, klen); key[klen]=0;
    const char* val = eq+1;
    if (rd_kafka_conf_set(conf, key, val, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      free(key);
      return -EINVAL;
    }
    free(key);
  }
  return 0;
}
#endif

// ------------------- Producteur -------------------
VL_EXPORT int kafka_new_producer(const char* const* conf_kv, size_t n_kv){
#ifndef VL_HAVE_RDKAFKA
  (void)conf_kv;(void)n_kv; return KF_ENOSYS;
#else
  int id = alloc_h(); if (id<0) return id;
  k_handle* H = &g_k[id]; memset(H,0,sizeof(*H));

  rd_kafka_conf_t* conf = rd_kafka_conf_new();
  if (!conf) return KF_ENOMEM;

  // callbacks minimales: dr (delivery report) silencieux
  rd_kafka_conf_set_dr_msg_cb(conf, NULL);

  if (apply_conf_pairs(conf, conf_kv, n_kv)!=0) { rd_kafka_conf_destroy(conf); return KF_EINVAL; }

  char errstr[512];
  rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
  if (!rk) { rd_kafka_conf_destroy(conf); return KF_EIO; }

  H->kind = K_PROD;
  H->rk   = rk;
  H->conf = NULL; // conf transférée à rk
  return id;
#endif
}

VL_EXPORT int kafka_produce(int h, const char* topic, int32_t partition,
                            const void* key, size_t klen,
                            const void* payload, size_t len,
                            int64_t timestamp_ms) {
#ifndef VL_HAVE_RDKAFKA
  (void)h;(void)topic;(void)partition;(void)key;(void)klen;(void)payload;(void)len;(void)timestamp_ms;
  return KF_ENOSYS;
#else
  if (h<=0 || h>=VL_KAFKA_MAX || g_k[h].kind!=K_PROD) return KF_EINVAL;
  if (!topic) return KF_EINVAL;

  rd_kafka_t* rk = g_k[h].rk;
  rd_kafka_topic_t* rkt = rd_kafka_topic_new(rk, topic, NULL);
  if (!rkt) return KF_EIO;

  // Copie les buffers (RD_KAFKA_VTYPE_COPY)
  rd_kafka_headers_t* hdrs = NULL;
  rd_kafka_resp_err_t e = rd_kafka_producev(
      rk,
      RD_KAFKA_V_RKT(rkt),
      RD_KAFKA_V_PARTITION(partition),
      RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
      RD_KAFKA_V_VALUE((void*)payload, (size_t)len),
      RD_KAFKA_V_KEY((void*)key, (size_t)klen),
      RD_KAFKA_V_TIMESTAMP(timestamp_ms),
      RD_KAFKA_V_HEADERS(hdrs),
      RD_KAFKA_V_END);
  rd_kafka_topic_destroy(rkt);
  if (e != RD_KAFKA_RESP_ERR_NO_ERROR) return KF_EIO;
  return KF_OK;
#endif
}

VL_EXPORT int kafka_flush(int h, int timeout_ms){
#ifndef VL_HAVE_RDKAFKA
  (void)h;(void)timeout_ms; return KF_ENOSYS;
#else
  if (h<=0 || h>=VL_KAFKA_MAX || g_k[h].kind!=K_PROD) return KF_EINVAL;
  int ms = timeout_ms>=0 ? timeout_ms : 5000;
  return rd_kafka_flush(g_k[h].rk, ms)==RD_KAFKA_RESP_ERR_NO_ERROR ? KF_OK : KF_EIO;
#endif
}

VL_EXPORT int kafka_poll(int h, int timeout_ms){
#ifndef VL_HAVE_RDKAFKA
  (void)h;(void)timeout_ms; return KF_ENOSYS;
#else
  if (h<=0 || h>=VL_KAFKA_MAX || g_k[h].kind!=K_PROD) return KF_EINVAL;
  int ms = timeout_ms>=0 ? timeout_ms : 100;
  rd_kafka_poll(g_k[h].rk, ms);
  return KF_OK;
#endif
}

// ------------------- Consommateur -------------------
VL_EXPORT int kafka_new_consumer(const char* group_id,
                                 const char* const* conf_kv, size_t n_kv){
#ifndef VL_HAVE_RDKAFKA
  (void)group_id;(void)conf_kv;(void)n_kv; return KF_ENOSYS;
#else
  int id = alloc_h(); if (id<0) return id;
  k_handle* H = &g_k[id]; memset(H,0,sizeof(*H));

  rd_kafka_conf_t* conf = rd_kafka_conf_new();
  if (!conf) return KF_ENOMEM;

  char errstr[512];

  if (group_id && *group_id) {
    if (rd_kafka_conf_set(conf, "group.id", group_id, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      rd_kafka_conf_destroy(conf); return KF_EINVAL;
    }
  }
  // Auto-commit par défaut
  rd_kafka_conf_set(conf, "enable.auto.commit", "true", NULL, 0);
  // Isolation lecture
  rd_kafka_conf_set(conf, "isolation.level", "read_committed", NULL, 0);

  if (apply_conf_pairs(conf, conf_kv, n_kv)!=0) { rd_kafka_conf_destroy(conf); return KF_EINVAL; }

  rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
  if (!rk) { rd_kafka_conf_destroy(conf); return KF_EIO; }

  // Assigner le consumer à l’instance
  rd_kafka_poll_set_consumer(rk);

  H->kind = K_CONS;
  H->rk   = rk;
  H->conf = NULL;
  return id;
#endif
}

VL_EXPORT int kafka_subscribe(int h, const char* const* topics, size_t n_topics){
#ifndef VL_HAVE_RDKAFKA
  (void)h;(void)topics;(void)n_topics; return KF_ENOSYS;
#else
  if (h<=0 || h>=VL_KAFKA_MAX || g_k[h].kind!=K_CONS) return KF_EINVAL;
  if (!topics || n_topics==0) return KF_EINVAL;

  rd_kafka_topic_partition_list_t* lst = rd_kafka_topic_partition_list_new((int)n_topics);
  if (!lst) return KF_ENOMEM;
  for (size_t i=0;i<n_topics;i++){
    if (topics[i] && *topics[i]) rd_kafka_topic_partition_list_add(lst, topics[i], RD_KAFKA_PARTITION_UA);
  }
  rd_kafka_resp_err_t e = rd_kafka_subscribe(g_k[h].rk, lst);
  rd_kafka_topic_partition_list_destroy(lst);
  return e==RD_KAFKA_RESP_ERR_NO_ERROR ? KF_OK : KF_EIO;
#endif
}

VL_EXPORT int kafka_poll_msg(int h, int timeout_ms,
                             char** out_topic, int32_t* out_partition, int64_t* out_offset,
                             void** out_key, size_t* out_klen,
                             void** out_payload, size_t* out_len,
                             int64_t* out_ts_ms){
#ifndef VL_HAVE_RDKAFKA
  (void)h;(void)timeout_ms;(void)out_topic;(void)out_partition;(void)out_offset;
  (void)out_key;(void)out_klen;(void)out_payload;(void)out_len;(void)out_ts_ms;
  return KF_ENOSYS;
#else
  if (h<=0 || h>=VL_KAFKA_MAX || g_k[h].kind!=K_CONS) return KF_EINVAL;
  int ms = timeout_ms>=0 ? timeout_ms : 100;

  rd_kafka_message_t* m = rd_kafka_consumer_poll(g_k[h].rk, ms);
  if (!m) return 1; // timeout sans message

  int rc = KF_OK;
  if (m->err) {
    // Erreurs non fatales: renvoyer EIO mais libérer proprement
    rc = KF_EIO;
  } else {
    // topic
    if (out_topic) {
      const char* t = rd_kafka_topic_name(m->rkt);
      size_t tl = strlen(t);
      char* tp = (char*)malloc(tl+1);
      if (!tp) rc = KF_ENOMEM;
      else { memcpy(tp,t,tl+1); *out_topic = tp; }
    }
    if (out_partition) *out_partition = m->partition;
    if (out_offset)    *out_offset    = m->offset;

    // timestamp
    if (out_ts_ms) {
      rd_kafka_timestamp_type_t ttype;
      int64_t ts = rd_kafka_message_timestamp(m, &ttype);
      *out_ts_ms = ts;
    }

    // key
    if (out_key && out_klen) {
      if (m->key && m->key_len) {
        void* kbuf = malloc(m->key_len);
        if (!kbuf) rc = KF_ENOMEM;
        else { memcpy(kbuf, m->key, m->key_len); *out_key = kbuf; *out_klen = m->key_len; }
      } else { *out_key=NULL; *out_klen=0; }
    }

    // payload
    if (out_payload && out_len) {
      if (m->payload && m->len) {
        void* p = malloc(m->len);
        if (!p) rc = KF_ENOMEM;
        else { memcpy(p, m->payload, m->len); *out_payload=p; *out_len=m->len; }
      } else { *out_payload=NULL; *out_len=0; }
    }
  }

  rd_kafka_message_destroy(m);
  return rc;
#endif
}

VL_EXPORT int kafka_commit(int h){
#ifndef VL_HAVE_RDKAFKA
  (void)h; return KF_ENOSYS;
#else
  if (h<=0 || h>=VL_KAFKA_MAX || g_k[h].kind!=K_CONS) return KF_EINVAL;
  rd_kafka_resp_err_t e = rd_kafka_commit(g_k[h].rk, NULL, 0);
  return e==RD_KAFKA_RESP_ERR_NO_ERROR ? KF_OK : KF_EIO;
#endif
}

// ------------------- Commun -------------------
VL_EXPORT int kafka_close(int h){
  if (h<=0 || h>=VL_KAFKA_MAX) return KF_EINVAL;
  clear_h(&g_k[h]);
  return KF_OK;
}