// SPDX-License-Identifier: GPL-3.0-or-later
//
// amqp.c — RabbitMQ AMQP 0-9-1 bindings for Vitte Light VM (C17, complet)
// Namespace: "amqp"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_RABBITMQ_C -c amqp.c
//   cc ... amqp.o -lrabbitmq
//
// Portabilité:
//   - Impl. réelle si VL_HAVE_RABBITMQ_C et <amqp.h> présents (rabbitmq-c).
//   - Sinon: stubs -> (nil, "ENOSYS").
//
// Modèle:
//   - 1 identifiant = 1 connexion AMQP (socket TCP).
//   - Canaux explicites (ch:int). Le canal 1 peut être ouvert après connect.
//   - Consommation bloquante avec timeout via amqp_consume_message().
//   - Propriétés publiées minimales: content_type, delivery_mode.
//
// API:
//
//   Connexion
//     amqp.connect(host, port:int, vhost, user, pass[, heartbeat=60[,
//     frame_max=131072]])
//         -> id:int | (nil, errmsg)
//     amqp.open_channel(id, ch:int)   -> true | (nil, errmsg)
//     amqp.close_channel(id, ch:int)  -> true | (nil, errmsg)
//     amqp.close(id)                  -> true | (nil, errmsg)
//     amqp.errstr([code:int])         -> string
//     amqp.lib_version()              -> string
//
//   QoS
//     amqp.qos(id, ch, prefetch_count:int[, global=false]) -> true | (nil,
//     errmsg)
//
//   Exchanges/queues
//     amqp.exchange_declare(id, ch, exchange, type[, durable=true[,
//     auto_delete=false]]) -> true | (nil, errmsg) amqp.exchange_delete(id, ch,
//     exchange[, if_unused=false])                          -> true | (nil,
//     errmsg) amqp.queue_declare(id, ch, queue[, durable=true[,
//     exclusive=false[, auto_delete=false]]])
//         -> name:string | (nil, errmsg)
//     amqp.queue_bind(id, ch, queue, exchange, routing_key) -> true | (nil,
//     errmsg) amqp.queue_delete(id, ch, queue[, if_unused=false[,
//     if_empty=false]]) -> true | (nil, errmsg)
//
//   Publication
//     amqp.publish(id, ch, exchange, routing_key, body:string
//                  [, content_type="application/octet-stream"[,
//                  delivery_mode=2[, mandatory=false[, immediate=false]]]])
//         -> true | (nil, errmsg)
//
//   Consommation / lecture
//     amqp.consume(id, ch, queue[, consumer_tag=""[, no_ack=false[,
//     exclusive=false]]]) -> consumer_tag:string | (nil, errmsg)
//     amqp.consume_next(id[, timeout_ms=0])
//         -> body:string, routing_key:string, exchange:string,
//         delivery_tag:int64, redelivered:int, content_type:string |
//         (nil,"timeout") | (nil, errmsg)
//     amqp.get(id, ch, queue[, no_ack=false])
//         -> body:string | (nil,"empty") | (nil, errmsg)
//     amqp.ack(id, ch, delivery_tag:int64[, multiple=false]) -> true | (nil,
//     errmsg) amqp.nack(id, ch, delivery_tag:int64[, requeue=true[,
//     multiple=false]]) -> true | (nil, errmsg) amqp.reject(id, ch,
//     delivery_tag:int64[, requeue=true]) -> true | (nil, errmsg)
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef VL_HAVE_RABBITMQ_C
#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>
/* SSL (facultatif) : <amqp_ssl_socket.h> */
#endif

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *aq_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t aq_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int aq_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int aq_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)aq_check_int(S, idx);
  return defv;
}

#ifndef VL_HAVE_RABBITMQ_C
// ---------------------------------------------------------------------
// STUBS (rabbitmq-c absent)
// ---------------------------------------------------------------------
#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)

static int vlamqp_connect(VL_State *S) {
  (void)aq_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlamqp_open_channel(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_close_channel(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_close(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_errstr(VL_State *S) {
  (void)aq_opt_int(S, 1, 0);
  vl_push_string(S, "rabbitmq-c not built");
  return 1;
}
static int vlamqp_lib_version(VL_State *S) {
  vl_push_string(S, "unavailable");
  return 1;
}
static int vlamqp_qos(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_exchange_declare(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_exchange_delete(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_queue_declare(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_queue_bind(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_queue_delete(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_publish(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_consume(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_consume_next(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_get(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_ack(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_nack(VL_State *S) { NOSYS_PAIR(S); }
static int vlamqp_reject(VL_State *S) { NOSYS_PAIR(S); }

#else
// ---------------------------------------------------------------------
// Implémentation réelle (rabbitmq-c)
// ---------------------------------------------------------------------

typedef struct AmqpConn {
  int used;
  amqp_connection_state_t conn;
  amqp_socket_t *sock;
} AmqpConn;

static AmqpConn *g_conns = NULL;
static int g_conn_cap = 0;

static int ensure_conn_cap(int need) {
  if (need <= g_conn_cap) return 1;
  int ncap = g_conn_cap ? g_conn_cap : 8;
  while (ncap < need) ncap <<= 1;
  AmqpConn *nc = (AmqpConn *)realloc(g_conns, (size_t)ncap * sizeof *nc);
  if (!nc) return 0;
  for (int i = g_conn_cap; i < ncap; i++) {
    nc[i].used = 0;
    nc[i].conn = NULL;
    nc[i].sock = NULL;
  }
  g_conns = nc;
  g_conn_cap = ncap;
  return 1;
}
static int alloc_conn_slot(void) {
  for (int i = 1; i < g_conn_cap; i++)
    if (!g_conns[i].used) return i;
  if (!ensure_conn_cap(g_conn_cap ? g_conn_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_conn_cap; i++)
    if (!g_conns[i].used) return i;
  return 0;
}
static int check_cid(int id) {
  return id > 0 && id < g_conn_cap && g_conns[id].used && g_conns[id].conn &&
         g_conns[id].sock;
}

static int push_amqp_err(VL_State *S, int liberr, const char *fallback) {
  const char *m = amqp_error_string2(liberr);
  vl_push_nil(S);
  vl_push_string(S, (m && *m) ? m : (fallback ? fallback : "EIO"));
  return 2;
}
static int push_rpc_err(VL_State *S, amqp_rpc_reply_t r) {
  switch (r.reply_type) {
    case AMQP_RESPONSE_NORMAL:
      vl_push_bool(S, 1);
      return 1;
    case AMQP_RESPONSE_NONE:
      vl_push_nil(S);
      vl_push_string(S, "no response");
      return 2;
    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
      return push_amqp_err(S, r.library_errno, "library");
    case AMQP_RESPONSE_SERVER_EXCEPTION:
      // Best-effort textualisation
      if (r.reply.id == AMQP_CONNECTION_CLOSE_METHOD) {
        amqp_connection_close_t *m = (amqp_connection_close_t *)r.reply.decoded;
        vl_push_nil(S);
        vl_push_lstring(S, (const char *)m->reply_text.bytes,
                        (int)m->reply_text.len);
        return 2;
      }
      if (r.reply.id == AMQP_CHANNEL_CLOSE_METHOD) {
        amqp_channel_close_t *m = (amqp_channel_close_t *)r.reply.decoded;
        vl_push_nil(S);
        vl_push_lstring(S, (const char *)m->reply_text.bytes,
                        (int)m->reply_text.len);
        return 2;
      }
      vl_push_nil(S);
      vl_push_string(S, "server exception");
      return 2;
    default:
      vl_push_nil(S);
      vl_push_string(S, "EIO");
      return 2;
  }
}

static int vlamqp_lib_version(VL_State *S) {
#ifdef AMQP_VERSION
  vl_push_string(S, AMQP_VERSION);
#else
  vl_push_string(S, "rabbitmq-c");
#endif
  return 1;
}
static int vlamqp_errstr(VL_State *S) {
  int code = aq_opt_int(S, 1, 0);
  const char *m = amqp_error_string2(code);
  vl_push_string(S, m ? m : "");
  return 1;
}

// amqp.connect(host, port, vhost, user, pass, [heartbeat], [frame_max])
static int vlamqp_connect(VL_State *S) {
  const char *host = aq_check_str(S, 1);
  int port = (int)aq_check_int(S, 2);
  const char *vhost = aq_check_str(S, 3);
  const char *user = aq_check_str(S, 4);
  const char *pass = aq_check_str(S, 5);
  int heartbeat = aq_opt_int(S, 6, 60);
  int frame_max = aq_opt_int(S, 7, 131072);

  int id = alloc_conn_slot();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  amqp_connection_state_t c = amqp_new_connection();
  amqp_socket_t *sock = amqp_tcp_socket_new(c);
  if (!sock) {
    amqp_destroy_connection(c);
    vl_push_nil(S);
    vl_push_string(S, "socket");
    return 2;
  }

  int rc = amqp_socket_open(sock, host, port);
  if (rc != AMQP_STATUS_OK) {
    amqp_destroy_connection(c);
    return push_amqp_err(S, rc, "socket_open");
  }

  // Tune -> login
  amqp_rpc_reply_t r =
      amqp_login(c, vhost, 0 /*channel_max*/, frame_max, heartbeat,
                 AMQP_SASL_METHOD_PLAIN, user, pass);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) {
    amqp_connection_close(c, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(c);
    return push_rpc_err(S, r);
  }

  g_conns[id].used = 1;
  g_conns[id].conn = c;
  g_conns[id].sock = sock;

  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlamqp_close(VL_State *S) {
  int id = (int)aq_check_int(S, 1);
  if (!check_cid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_connection_close(g_conns[id].conn, AMQP_REPLY_SUCCESS);
  amqp_destroy_connection(g_conns[id].conn);
  g_conns[id].conn = NULL;
  g_conns[id].sock = NULL;
  g_conns[id].used = 0;
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_open_channel(VL_State *S) {
  int id = (int)aq_check_int(S, 1);
  int ch = (int)aq_check_int(S, 2);
  if (!check_cid(id) || ch <= 0 || ch > 65535) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_channel_open(g_conns[id].conn, ch);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) return push_rpc_err(S, r);
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_close_channel(VL_State *S) {
  int id = (int)aq_check_int(S, 1);
  int ch = (int)aq_check_int(S, 2);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_channel_close(g_conns[id].conn, ch, AMQP_REPLY_SUCCESS);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) return push_rpc_err(S, r);
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_qos(VL_State *S) {
  int id = (int)aq_check_int(S, 1);
  int ch = (int)aq_check_int(S, 2);
  int prefetch = (int)aq_check_int(S, 3);
  int global = aq_opt_bool(S, 4, 0);
  if (!check_cid(id) || ch <= 0 || prefetch < 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_basic_qos(g_conns[id].conn, ch, 0, prefetch, global ? 1 : 0);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) return push_rpc_err(S, r);
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_exchange_declare(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  const char *ex = aq_check_str(S, 3);
  const char *type = aq_check_str(S, 4);
  int durable = aq_opt_bool(S, 5, 1);
  int auto_delete = aq_opt_bool(S, 6, 0);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_exchange_declare(g_conns[id].conn, ch, amqp_cstring_bytes(ex),
                        amqp_cstring_bytes(type), 0, durable, auto_delete, 0,
                        amqp_empty_table);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) return push_rpc_err(S, r);
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_exchange_delete(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  const char *ex = aq_check_str(S, 3);
  int if_unused = aq_opt_bool(S, 4, 0);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_exchange_delete(g_conns[id].conn, ch, amqp_cstring_bytes(ex), if_unused);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) return push_rpc_err(S, r);
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_queue_declare(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  const char *q = aq_check_str(S, 3);
  int durable = aq_opt_bool(S, 4, 1);
  int exclusive = aq_opt_bool(S, 5, 0);
  int auto_delete = aq_opt_bool(S, 6, 0);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_queue_declare_ok_t *ok =
      amqp_queue_declare(g_conns[id].conn, ch, amqp_cstring_bytes(q), 0,
                         durable, exclusive, auto_delete, amqp_empty_table);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL || !ok) return push_rpc_err(S, r);
  // renvoie le nom effectif (utile si q == "")
  vl_push_lstring(S, (const char *)ok->queue.bytes, (int)ok->queue.len);
  return 1;
}

static int vlamqp_queue_bind(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  const char *queue = aq_check_str(S, 3);
  const char *ex = aq_check_str(S, 4);
  const char *key = aq_check_str(S, 5);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_queue_bind(g_conns[id].conn, ch, amqp_cstring_bytes(queue),
                  amqp_cstring_bytes(ex), amqp_cstring_bytes(key),
                  amqp_empty_table);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) return push_rpc_err(S, r);
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_queue_delete(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  const char *queue = aq_check_str(S, 3);
  int if_unused = aq_opt_bool(S, 4, 0);
  int if_empty = aq_opt_bool(S, 5, 0);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_queue_delete(g_conns[id].conn, ch, amqp_cstring_bytes(queue), if_unused,
                    if_empty);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) return push_rpc_err(S, r);
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_publish(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  const char *ex = aq_check_str(S, 3);
  const char *key = aq_check_str(S, 4);
  const char *body = aq_check_str(S, 5);
  const char *ctype = (vl_get(S, 6) && vl_isstring(S, 6))
                          ? aq_check_str(S, 6)
                          : "application/octet-stream";
  int delivery_mode = aq_opt_int(S, 7, 2);  // 2 = persistent
  int mandatory = aq_opt_bool(S, 8, 0);
  int immediate = aq_opt_bool(S, 9, 0);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  amqp_basic_properties_t props;
  props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
  props.content_type = amqp_cstring_bytes(ctype);
  props.delivery_mode = (uint8_t)delivery_mode;

  int rc = amqp_basic_publish(g_conns[id].conn, ch, amqp_cstring_bytes(ex),
                              amqp_cstring_bytes(key), mandatory, immediate,
                              &props, amqp_cstring_bytes(body));
  if (rc != AMQP_STATUS_OK) return push_amqp_err(S, rc, "publish");
  vl_push_bool(S, 1);
  return 1;
}

static int vlamqp_consume(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  const char *queue = aq_check_str(S, 3);
  const char *tag =
      (vl_get(S, 4) && vl_isstring(S, 4)) ? aq_check_str(S, 4) : "";
  int no_ack = aq_opt_bool(S, 5, 0);
  int exclusive = aq_opt_bool(S, 6, 0);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_basic_consume_ok_t *ok = amqp_basic_consume(
      g_conns[id].conn, ch, amqp_cstring_bytes(queue), amqp_cstring_bytes(tag),
      0 /*no_local*/, no_ack, exclusive, amqp_empty_table);
  amqp_rpc_reply_t r = amqp_get_rpc_reply(g_conns[id].conn);
  if (r.reply_type != AMQP_RESPONSE_NORMAL || !ok) return push_rpc_err(S, r);
  vl_push_lstring(S, (const char *)ok->consumer_tag.bytes,
                  (int)ok->consumer_tag.len);
  return 1;
}

// amqp.consume_next(id[, timeout_ms=0]) -> body, routing_key, exchange,
// delivery_tag, redelivered, content_type | (nil,"timeout")
static int vlamqp_consume_next(VL_State *S) {
  int id = (int)aq_check_int(S, 1);
  int timeout_ms = aq_opt_int(S, 2, 0);
  if (!check_cid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  amqp_envelope_t env;
  amqp_maybe_release_buffers(g_conns[id].conn);
  struct timeval tv, *ptv = NULL;
  if (timeout_ms > 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ptv = &tv;
  }
  amqp_rpc_reply_t r = amqp_consume_message(g_conns[id].conn, &env, ptv, 0);
  if (r.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION &&
      r.library_errno == AMQP_STATUS_TIMEOUT) {
    vl_push_nil(S);
    vl_push_string(S, "timeout");
    return 2;
  }
  if (r.reply_type != AMQP_RESPONSE_NORMAL) {
    return push_rpc_err(S, r);
  }
  // Returns: body, routing_key, exchange, delivery_tag, redelivered,
  // content_type
  vl_push_lstring(S, (const char *)env.message.body.bytes,
                  (int)env.message.body.len);
  vl_push_lstring(S, (const char *)env.routing_key.bytes,
                  (int)env.routing_key.len);
  vl_push_lstring(S, (const char *)env.exchange.bytes, (int)env.exchange.len);
  vl_push_int(S, (int64_t)env.delivery_tag);
  vl_push_int(S, env.redelivered ? 1 : 0);

  const char *ctype = "";
  if (env.message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
    ctype = (const char *)env.message.properties.content_type.bytes;
  }
  vl_push_string(S, ctype ? ctype : "");
  amqp_destroy_envelope(&env);
  return 6;
}

// amqp.get(id, ch, queue[, no_ack=false]) -> body | (nil,"empty")
static int vlamqp_get(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  const char *queue = aq_check_str(S, 3);
  int no_ack = aq_opt_bool(S, 4, 0);
  if (!check_cid(id) || ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  amqp_rpc_reply_t r = amqp_basic_get(
      g_conns[id].conn, ch, amqp_cstring_bytes(queue), no_ack ? 1 : 0);
  if (r.reply_type != AMQP_RESPONSE_NORMAL) {
    return push_rpc_err(S, r);
  }
  amqp_message_t msg;
  memset(&msg, 0, sizeof msg);
  r = amqp_read_message(g_conns[id].conn, ch, &msg, 0);
  if (r.reply_type == AMQP_RESPONSE_NORMAL) {
    vl_push_lstring(S, (const char *)msg.body.bytes, (int)msg.body.len);
    amqp_destroy_message(&msg);
    return 1;
  }
  if (r.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION &&
      r.library_errno == AMQP_STATUS_NOT_FOUND) {
    vl_push_nil(S);
    vl_push_string(S, "empty");
    return 2;
  }
  return push_rpc_err(S, r);
}

static int vlamqp_ack(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  int64_t tag = aq_check_int(S, 3);
  int multiple = aq_opt_bool(S, 4, 0);
  if (!check_cid(id) || ch <= 0 || tag <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc =
      amqp_basic_ack(g_conns[id].conn, ch, (uint64_t)tag, multiple ? 1 : 0);
  if (rc != AMQP_STATUS_OK) return push_amqp_err(S, rc, "ack");
  vl_push_bool(S, 1);
  return 1;
}
static int vlamqp_nack(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  int64_t tag = aq_check_int(S, 3);
  int requeue = aq_opt_bool(S, 4, 1);
  int multiple = aq_opt_bool(S, 5, 0);
  if (!check_cid(id) || ch <= 0 || tag <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = amqp_basic_nack(g_conns[id].conn, ch, (uint64_t)tag,
                           multiple ? 1 : 0, requeue ? 1 : 0);
  if (rc != AMQP_STATUS_OK) return push_amqp_err(S, rc, "nack");
  vl_push_bool(S, 1);
  return 1;
}
static int vlamqp_reject(VL_State *S) {
  int id = (int)aq_check_int(S, 1), ch = (int)aq_check_int(S, 2);
  int64_t tag = aq_check_int(S, 3);
  int requeue = aq_opt_bool(S, 4, 1);
  if (!check_cid(id) || ch <= 0 || tag <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc =
      amqp_basic_reject(g_conns[id].conn, ch, (uint64_t)tag, requeue ? 1 : 0);
  if (rc != AMQP_STATUS_OK) return push_amqp_err(S, rc, "reject");
  vl_push_bool(S, 1);
  return 1;
}

#endif  // VL_HAVE_RABBITMQ_C

// ---------------------------------------------------------------------
// Registration VM
// ---------------------------------------------------------------------
static const VL_Reg amqplib[] = {{"connect", vlamqp_connect},
                                 {"open_channel", vlamqp_open_channel},
                                 {"close_channel", vlamqp_close_channel},
                                 {"close", vlamqp_close},
                                 {"errstr", vlamqp_errstr},
                                 {"lib_version", vlamqp_lib_version},

                                 {"qos", vlamqp_qos},

                                 {"exchange_declare", vlamqp_exchange_declare},
                                 {"exchange_delete", vlamqp_exchange_delete},
                                 {"queue_declare", vlamqp_queue_declare},
                                 {"queue_bind", vlamqp_queue_bind},
                                 {"queue_delete", vlamqp_queue_delete},

                                 {"publish", vlamqp_publish},

                                 {"consume", vlamqp_consume},
                                 {"consume_next", vlamqp_consume_next},
                                 {"get", vlamqp_get},
                                 {"ack", vlamqp_ack},
                                 {"nack", vlamqp_nack},
                                 {"reject", vlamqp_reject},

                                 {NULL, NULL}};

void vl_open_amqplib(VL_State *S) { vl_register_lib(S, "amqp", amqplib); }
