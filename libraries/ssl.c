// SPDX-License-Identifier: GPL-3.0-or-later
//
// ssl.c — TLS/SSL client bindings for Vitte Light (C17, complet)
// Namespace: "ssl"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_OPENSSL -c ssl.c
//   cc ... ssl.o -lssl -lcrypto
//
// Portabilité:
//   - Implémentation réelle si VL_HAVE_OPENSSL et <openssl/ssl.h> présents.
//   - Sinon: stubs retournant (nil, "ENOSYS").
//
// API (client seulement, 1 connexion = 1 id entier):
//   ssl.connect(host, port:int[, verify=true[, timeout_ms=10000[, ca_file[,
//   alpn_csv]]]])
//       -> id:int | (nil, errmsg)
//   ssl.read(id, nbytes:int)        -> string | (nil, errmsg)
//   ssl.write(id, data:string)      -> bytes_written:int | (nil, errmsg)
//   ssl.shutdown(id)                -> true | (nil, errmsg)         // TLS
//   shutdown ssl.close(id)                   -> true                         //
//   ferme socket et libère ssl.peer_cert_pem(id)           -> pem:string |
//   (nil, errmsg) ssl.cipher(id)                  -> string | (nil, errmsg)
//   ssl.version(id)                 -> string | (nil, errmsg)
//   ssl.set_timeout_ms(id, ms:int)  -> true | (nil, errmsg)         // R/W
//   timeouts ssl.errstr([code:int])          -> string                       //
//   OpenSSL error string ssl.openssl_version()           -> string | (nil,
//   errmsg)
//
// Notes:
//   - Connexions TCP en mode bloquant, connect() avec délai via
//   non-bloquant+select.
//   - Timeouts R/W via SO_RCVTIMEO/SO_SNDTIMEO.
//   - SNI activé par défaut sur "host". ALPN via csv "h2,http/1.1".
//   - verify=true utilise store système (SSL_CTX_set_default_verify_paths) ou
//   ca_file.
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET vl_sock_t;
#define VL_INVALID_SOCK INVALID_SOCKET
#define vl_closesock(s) closesocket(s)
#define vl_sock_last_error() WSAGetLastError()
static int vl_net_inited = 0;
static void vl_net_init_once(void) {
  if (!vl_net_inited) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    vl_net_inited = 1;
  }
}
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int vl_sock_t;
#define VL_INVALID_SOCK (-1)
#define vl_closesock(s) close(s)
#define vl_sock_last_error() errno
#endif

#ifdef VL_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

// ---------------------------------------------------------------------
// VM helpers
// ---------------------------------------------------------------------

static const char *ssl_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t ssl_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int ssl_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int ssl_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)ssl_check_int(S, idx);
  return defv;
}

// ---------------------------------------------------------------------
// Stubs (OpenSSL absent)
// ---------------------------------------------------------------------
#ifndef VL_HAVE_OPENSSL

#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)
static int vlssl_connect(VL_State *S) {
  (void)ssl_check_str(S, 1);
  (void)ssl_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlssl_read(VL_State *S) {
  (void)ssl_check_int(S, 1);
  (void)ssl_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlssl_write(VL_State *S) {
  (void)ssl_check_int(S, 1);
  (void)ssl_check_str(S, 2);
  NOSYS_PAIR(S);
}
static int vlssl_shutdown(VL_State *S) {
  (void)ssl_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlssl_close(VL_State *S) {
  (void)ssl_check_int(S, 1);
  vl_push_bool(S, 1);
  return 1;
}
static int vlssl_peer_cert_pem(VL_State *S) {
  (void)ssl_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlssl_cipher(VL_State *S) {
  (void)ssl_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlssl_version(VL_State *S) {
  (void)ssl_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlssl_set_timeout_ms(VL_State *S) {
  (void)ssl_check_int(S, 1);
  (void)ssl_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlssl_errstr(VL_State *S) {
  (void)ssl_opt_int(S, 1, 0);
  vl_push_string(S, "ssl not built");
  return 1;
}
static int vlssl_openssl_version(VL_State *S) {
  (void)S;
  NOSYS_PAIR(S);
}

#else
// ---------------------------------------------------------------------
// Implémentation réelle (OpenSSL >= 1.1.0 recommandé)
// ---------------------------------------------------------------------

typedef struct Conn {
  int used;
  vl_sock_t sock;
  int timeout_ms;
  SSL_CTX *ctx;
  SSL *ssl;
} Conn;

static Conn *g_conn = NULL;
static int g_conn_cap = 0;

static int ensure_conn_cap(int need) {
  if (need <= g_conn_cap) return 1;
  int ncap = g_conn_cap ? g_conn_cap : 16;
  while (ncap < need) ncap <<= 1;
  Conn *nc = (Conn *)realloc(g_conn, (size_t)ncap * sizeof *nc);
  if (!nc) return 0;
  for (int i = g_conn_cap; i < ncap; i++) {
    nc[i].used = 0;
    nc[i].sock = VL_INVALID_SOCK;
    nc[i].timeout_ms = 10000;
    nc[i].ctx = NULL;
    nc[i].ssl = NULL;
  }
  g_conn = nc;
  g_conn_cap = ncap;
  return 1;
}
static int alloc_conn_slot(void) {
  for (int i = 1; i < g_conn_cap; i++)
    if (!g_conn[i].used) return i;
  if (!ensure_conn_cap(g_conn_cap ? g_conn_cap * 2 : 16)) return 0;
  for (int i = 1; i < g_conn_cap; i++)
    if (!g_conn[i].used) return i;
  return 0;
}
static int check_cid(int id) {
  return id > 0 && id < g_conn_cap && g_conn[id].used && g_conn[id].ssl &&
         g_conn[id].ctx && g_conn[id].sock != VL_INVALID_SOCK;
}

static int set_blocking(vl_sock_t s, int blocking) {
#if defined(_WIN32)
  u_long nb = blocking ? 0 : 1;
  return ioctlsocket(s, FIONBIO, &nb) == 0 ? 1 : 0;
#else
  int fl = fcntl(s, F_GETFL, 0);
  if (fl < 0) return 0;
  if (blocking)
    fl &= ~O_NONBLOCK;
  else
    fl |= O_NONBLOCK;
  return fcntl(s, F_SETFL, fl) == 0;
#endif
}

static int set_timeout_ms(vl_sock_t s, int ms) {
#if defined(_WIN32)
  int t = ms;  // milliseconds
  if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t) != 0)
    return 0;
  if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t) != 0)
    return 0;
  return 1;
#else
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (int)((ms % 1000) * 1000);
  if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) return 0;
  if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) return 0;
  return 1;
#endif
}

static int tcp_connect_timeout(vl_sock_t *out, const char *host, int port,
                               int timeout_ms, char *errbuf, size_t errlen) {
#if defined(_WIN32)
  vl_net_init_once();
#endif
  char pbuf[16];
  snprintf(pbuf, sizeof pbuf, "%d", port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;

  struct addrinfo *res = NULL;
  int gai = getaddrinfo(host, pbuf, &hints, &res);
  if (gai != 0 || !res) {
    snprintf(errbuf, errlen, "getaddrinfo");
    return 0;
  }

  vl_sock_t s = VL_INVALID_SOCK;
  int ok = 0;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    s = (vl_sock_t)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == VL_INVALID_SOCK) continue;

    if (!set_blocking(s, 0)) {
      vl_closesock(s);
      s = VL_INVALID_SOCK;
      continue;
    }

    int rc = connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen);
    if (rc == 0) {
      ok = 1;
      goto connected;
    }

#if defined(_WIN32)
    int werr = WSAGetLastError();
    if (werr != WSAEWOULDBLOCK && werr != WSAEINPROGRESS) {
      vl_closesock(s);
      s = VL_INVALID_SOCK;
      continue;
    }
#else
    if (!(errno == EINPROGRESS || errno == EWOULDBLOCK)) {
      vl_closesock(s);
      s = VL_INVALID_SOCK;
      continue;
    }
#endif
    // wait for writability
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (int)((timeout_ms % 1000) * 1000);
    int sel = select((int)(s + 1), NULL, &wfds, NULL, &tv);
    if (sel > 0) {
      int soerr = 0;
      socklen_t sl = (socklen_t)sizeof soerr;
      getsockopt(s, SOL_SOCKET, SO_ERROR, (void *)&soerr, &sl);
      if (soerr == 0) {
        ok = 1;
        goto connected;
      }
    }
    vl_closesock(s);
    s = VL_INVALID_SOCK;
  }

connected:
  if (res) freeaddrinfo(res);
  if (!ok) {
    snprintf(errbuf, errlen, "connect timeout");
    return 0;
  }
  (void)set_blocking(s, 1);
  *out = s;
  return 1;
}

static int parse_alpn_csv(const char *csv, unsigned char **out,
                          unsigned int *outlen) {
  *out = NULL;
  *outlen = 0;
  if (!csv || !*csv) return 1;
  size_t n = strlen(csv);
  unsigned char *buf = (unsigned char *)malloc(n + 16);
  if (!buf) return 0;
  size_t w = 0;
  const char *p = csv;
  while (*p) {
    const char *q = p;
    while (*q && *q != ',') q++;
    size_t L = (size_t)(q - p);
    if (L == 0 || L > 255) {
      free(buf);
      return 0;
    }
    buf[w++] = (unsigned char)L;
    memcpy(buf + w, p, L);
    w += L;
    p = (*q == ',') ? q + 1 : q;
  }
  *out = buf;
  *outlen = (unsigned int)w;
  return 1;
}

static int push_ssl_err(VL_State *S, const char *fallback) {
  unsigned long e = ERR_get_error();
  const char *msg = e ? ERR_reason_error_string(e) : NULL;
  vl_push_nil(S);
  vl_push_string(S, msg && *msg ? msg : (fallback ? fallback : "EIO"));
  return 2;
}

// --- VM: ssl.connect(host, port, verify, timeout_ms, ca_file, alpn_csv)
static int vlssl_connect(VL_State *S) {
  const char *host = ssl_check_str(S, 1);
  int port = (int)ssl_check_int(S, 2);
  int verify = ssl_opt_bool(S, 3, 1);
  int timeout_ms = ssl_opt_int(S, 4, 10000);
  const char *ca_file = NULL;
  if (vl_get(S, 5) && vl_isstring(S, 5)) ca_file = ssl_check_str(S, 5);
  const char *alpn_csv = NULL;
  if (vl_get(S, 6) && vl_isstring(S, 6)) alpn_csv = ssl_check_str(S, 6);

  char nerr[64] = {0};
  vl_sock_t sock = VL_INVALID_SOCK;
  if (!tcp_connect_timeout(&sock, host, port, timeout_ms, nerr, sizeof nerr)) {
    vl_push_nil(S);
    vl_push_string(S, *nerr ? nerr : "ENETUNREACH");
    return 2;
  }

  // Prepare SSL_CTX
  SSL_CTX *ctx = NULL;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  const SSL_METHOD *method = TLS_client_method();
#else
  const SSL_METHOD *method = SSLv23_client_method();
#endif
  ctx = SSL_CTX_new(method);
  if (!ctx) {
    vl_closesock(sock);
    return push_ssl_err(S, "SSL_CTX_new");
  }

  // Verify
  if (verify) {
    if (ca_file && *ca_file) {
      if (SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
        SSL_CTX_free(ctx);
        vl_closesock(sock);
        return push_ssl_err(S, "load CA");
      }
    } else {
      if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        SSL_CTX_free(ctx);
        vl_closesock(sock);
        return push_ssl_err(S, "default trust");
      }
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_verify_depth(ctx, 6);
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }

  // Create SSL
  SSL *ssl = SSL_new(ctx);
  if (!ssl) {
    SSL_CTX_free(ctx);
    vl_closesock(sock);
    return push_ssl_err(S, "SSL_new");
  }

  // SNI
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
  (void)SSL_set_tlsext_host_name(ssl, host);
#endif

  // ALPN
#ifdef TLSEXT_TYPE_application_layer_protocol_negotiation
  if (alpn_csv && *alpn_csv) {
    unsigned char *al = NULL;
    unsigned int al_len = 0;
    if (!parse_alpn_csv(alpn_csv, &al, &al_len)) {
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      vl_closesock(sock);
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    if (SSL_set_alpn_protos(ssl, al, al_len) != 0) {
      free(al);
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      vl_closesock(sock);
      return push_ssl_err(S, "alpn");
    }
#endif
    free(al);
  }
#endif

  // Bind socket
  if (SSL_set_fd(ssl, (int)sock) != 1) {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    vl_closesock(sock);
    return push_ssl_err(S, "SSL_set_fd");
  }

  // Timeouts
  (void)set_timeout_ms(sock, timeout_ms);

  // Handshake
  if (SSL_connect(ssl) != 1) {
    // Get verify result for clarity if enabled
    if (verify) {
      long vr = SSL_get_verify_result(ssl);
      if (vr != X509_V_OK) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        vl_closesock(sock);
        vl_push_nil(S);
        vl_push_string(S, X509_verify_cert_error_string(vr));
        return 2;
      }
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    vl_closesock(sock);
    return push_ssl_err(S, "handshake");
  }

  // Hostname verification (OpenSSL 1.1.0+ provides X509_check_host)
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  if (verify) {
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      vl_closesock(sock);
      vl_push_nil(S);
      vl_push_string(S, "no peer cert");
      return 2;
    }
    int ok = X509_check_host(cert, host, 0, 0, NULL);
    X509_free(cert);
    if (ok != 1) {
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      vl_closesock(sock);
      vl_push_nil(S);
      vl_push_string(S, "hostname mismatch");
      return 2;
    }
  }
#endif

  // Allocate id
  int id = alloc_conn_slot();
  if (!id) {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    vl_closesock(sock);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_conn[id].used = 1;
  g_conn[id].sock = sock;
  g_conn[id].ssl = ssl;
  g_conn[id].ctx = ctx;
  g_conn[id].timeout_ms = timeout_ms;

  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlssl_set_timeout_ms(VL_State *S) {
  int id = (int)ssl_check_int(S, 1);
  int ms = (int)ssl_check_int(S, 2);
  if (!check_cid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  if (!set_timeout_ms(g_conn[id].sock, ms < 0 ? 0 : ms)) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  g_conn[id].timeout_ms = ms < 0 ? 0 : ms;
  vl_push_bool(S, 1);
  return 1;
}

static int vlssl_read(VL_State *S) {
  int id = (int)ssl_check_int(S, 1);
  int n = (int)ssl_check_int(S, 2);
  if (!check_cid(id) || n < 0 || n > 64 * 1024 * 1024) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  if (n == 0) {
    vl_push_lstring(S, "", 0);
    return 1;
  }
  char *buf = (char *)malloc((size_t)n);
  if (!buf) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  int rd = SSL_read(g_conn[id].ssl, buf, n);
  if (rd <= 0) {
    free(buf);
    return push_ssl_err(S, "read");
  }
  vl_push_lstring(S, buf, rd);
  free(buf);
  return 1;
}

static int vlssl_write(VL_State *S) {
  int id = (int)ssl_check_int(S, 1);
  const char *data = ssl_check_str(S, 2);
  if (!check_cid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int len = (int)strlen(data);
  if (len == 0) {
    vl_push_int(S, 0);
    return 1;
  }
  int wr = SSL_write(g_conn[id].ssl, data, len);
  if (wr <= 0) return push_ssl_err(S, "write");
  vl_push_int(S, (int64_t)wr);
  return 1;
}

static void conn_free(Conn *c) {
  if (!c || !c->used) return;
  if (c->ssl) {
    SSL_free(c->ssl);
    c->ssl = NULL;
  }
  if (c->ctx) {
    SSL_CTX_free(c->ctx);
    c->ctx = NULL;
  }
  if (c->sock != VL_INVALID_SOCK) {
    vl_closesock(c->sock);
    c->sock = VL_INVALID_SOCK;
  }
  c->used = 0;
}

static int vlssl_shutdown(VL_State *S) {
  int id = (int)ssl_check_int(S, 1);
  if (!check_cid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = SSL_shutdown(g_conn[id].ssl);
  if (rc < 0) return push_ssl_err(S, "shutdown");
  vl_push_bool(S, 1);
  return 1;
}

static int vlssl_close(VL_State *S) {
  int id = (int)ssl_check_int(S, 1);
  if (id > 0 && id < g_conn_cap && g_conn[id].used) conn_free(&g_conn[id]);
  vl_push_bool(S, 1);
  return 1;
}

static int vlssl_peer_cert_pem(VL_State *S) {
  int id = (int)ssl_check_int(S, 1);
  if (!check_cid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  X509 *cert = SSL_get_peer_certificate(g_conn[id].ssl);
  if (!cert) {
    vl_push_nil(S);
    vl_push_string(S, "ENOENT");
    return 2;
  }
  BIO *m = BIO_new(BIO_s_mem());
  if (!m) {
    X509_free(cert);
    return push_ssl_err(S, "BIO");
  }
  int ok = PEM_write_bio_X509(m, cert);
  X509_free(cert);
  if (!ok) {
    BIO_free(m);
    return push_ssl_err(S, "PEM");
  }
  char *ptr = NULL;
  long n = BIO_get_mem_data(m, &ptr);
  vl_push_lstring(S, ptr, (int)n);
  BIO_free(m);
  return 1;
}

static int vlssl_cipher(VL_State *S) {
  int id = (int)ssl_check_int(S, 1);
  if (!check_cid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  const SSL_CIPHER *c = SSL_get_current_cipher(g_conn[id].ssl);
  if (!c) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, SSL_CIPHER_get_name(c));
  return 1;
}

static int vlssl_version(VL_State *S) {
  int id = (int)ssl_check_int(S, 1);
  if (!check_cid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  const char *v = SSL_get_version(g_conn[id].ssl);
  vl_push_string(S, v ? v : "");
#else
  vl_push_string(S, "TLS");
#endif
  return 1;
}

static int vlssl_errstr(VL_State *S) {
  unsigned long code = (unsigned long)ssl_opt_int(S, 1, 0);
  if (code == 0) code = ERR_peek_last_error();
  if (code == 0) {
    vl_push_string(S, "");
    return 1;
  }
  const char *s = ERR_reason_error_string(code);
  vl_push_string(S, s ? s : "");
  return 1;
}

static int vlssl_openssl_version(VL_State *S) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  vl_push_string(S, OpenSSL_version(OPENSSL_VERSION));
#else
  vl_push_string(S, SSLeay_version(SSLEAY_VERSION));
#endif
  return 1;
}

#endif  // VL_HAVE_OPENSSL

// ---------------------------------------------------------------------
// Registration VM
// ---------------------------------------------------------------------

static const VL_Reg ssllib[] = {{"connect", vlssl_connect},
                                {"read", vlssl_read},
                                {"write", vlssl_write},
                                {"shutdown", vlssl_shutdown},
                                {"close", vlssl_close},
                                {"peer_cert_pem", vlssl_peer_cert_pem},
                                {"cipher", vlssl_cipher},
                                {"version", vlssl_version},
                                {"set_timeout_ms", vlssl_set_timeout_ms},
                                {"errstr", vlssl_errstr},
                                {"openssl_version", vlssl_openssl_version},
                                {NULL, NULL}};

void vl_open_ssllib(VL_State *S) {
#ifdef VL_HAVE_OPENSSL
  (void)S;
  // OpenSSL 1.1+ s'auto-initialise; ERR_load_strings non requis.
#endif
  vl_register_lib(S, "ssl", ssllib);
}
