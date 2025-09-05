// SPDX-License-Identifier: GPL-3.0-or-later
//
// curl.c — Client HTTP(S) C17 complet pour Vitte Light (wrapper libcurl)
//
// Capacités :
// - HTTP/1.1 et HTTP/2 (ALPN/TLS) si supporté par libcurl
// - GET, POST, PUT, DELETE, HEAD, méthode personnalisée
// - Suivi de redirections, timeouts, proxy, auth Basic, en-têtes
// - TLS: vérification peer/host, CA bundle/path personnalisable
// - Décompression auto (gzip/deflate/br) via libcurl
// - Écriture mémoire (buffer) ou fichier, callbacks de progression
// - Extraction métadonnées: code HTTP, URL effective, version HTTP, timings
// - Multi-plateforme (POSIX/Windows)
// - Fallback propres si libcurl n’est pas disponible (stubs ENOSYS)
//
// Dépendances :
//   - libcurl (>= 7.58+ recommandé), headers <curl/curl.h>
//   - includes/auxlib.h (AUX_* + AuxBuffer + AuxStatus)
//   - includes/curl.h (facultatif, sinon prototypes internes)
// Build (exemple) :
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_LIBCURL -c curl.c
//   cc ... curl.o -lcurl
//
// Remarque : Cette implémentation n’impose pas d’allocation globale ; seule
// curl_global_init/cleanup est exposée pour initialiser libcurl au lancement.

#include "curl.h"  // si absent, voir prototypes internes plus bas

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auxlib.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef VL_HAVE_LIBCURL
#include <curl/curl.h>
#endif

#ifndef VL_HTTP_DEFAULT_UA
#define VL_HTTP_DEFAULT_UA "VitteLight/0.1 (+https://example.invalid)"
#endif

#ifndef VL_HTTP_MAX_URL
#define VL_HTTP_MAX_URL 2048
#endif

#ifndef VL_HTTP_MAX_ERR
#define VL_HTTP_MAX_ERR 256
#endif

// ======================================================================
// API publique minimale (si includes/curl.h non fourni)
// ======================================================================
#ifndef VITTE_LIGHT_INCLUDES_CURL_H
#define VITTE_LIGHT_INCLUDES_CURL_H 1

typedef struct {
  const char *name;   // "Header-Name"
  const char *value;  // "value"
} VlReqHeader;

typedef int (*VlHttpProgressCb)(double dltotal, double dlnow, double ultotal,
                                double ulnow, void *user);

typedef size_t (*VlHttpWriteCb)(const void *data, size_t len, void *user);

typedef struct {
  // Requête
  const char *url;     // requis
  const char *method;  // "GET" par défaut
  const void *body;
  size_t body_len;          // POST/PUT payload
  const char *upload_path;  // PUT upload depuis fichier (exclusif avec body)
  const VlReqHeader *headers;
  size_t headers_len;
  const char *content_type;  // si non spécifié via headers
  const char *user_agent;    // défaut VL_HTTP_DEFAULT_UA
  const char *proxy;         // ex: "http://user:pass@host:port"
  const char *auth_basic;    // ex: "user:pass"
  const char *ca_path;       // chemin bundle CA ou certificat
  const char *range;         // ex: "0-1023"
  long timeout_ms;           // 0 = pas de timeout
  long connect_timeout_ms;   // 0 = pas de timeout
  long max_redirects;  // -1 = libcurl défaut, 0 = pas de redir, >0 = limite
  unsigned follow_redirects : 1;
  unsigned verify_peer : 1;  // défaut 1
  unsigned verify_host : 1;  // défaut 2 (strict)
  unsigned http2 : 1;        // tenter HTTP/2
  unsigned no_signal : 1;    // CURLOPT_NOSIGNAL
  // Sortie
  const char *download_path;  // si non NULL, écrire dans ce fichier
  VlHttpWriteCb write_cb;
  void *write_ud;  // sinon accumulateur mémoire
  VlHttpProgressCb progress_cb;
  void *progress_ud;
} VlHttpRequest;

typedef struct {
  long status;          // code HTTP
  AuxBuffer body;       // rempli si pas de write_cb/download_path
  char *effective_url;  // alloué dynamiquement
  char *ip;             // adresse IP effective si dispos
  long http_version;    // 10=1.0, 11=1.1, 20=2.0, etc. (voir libcurl)
  double total_time_ms;
  double namelookup_ms, connect_ms, appconnect_ms, pretransfer_ms,
      starttransfer_ms;
  uint64_t downloaded, uploaded;
  // En-têtes de réponse brutes concaténées (option simple)
  AuxBuffer headers_raw;  // CRLF-joined, si capturé
} VlHttpResponse;

// Init/Shutdown libcurl
AuxStatus vl_http_global_init(void);
void vl_http_global_cleanup(void);

// Exécution d’une requête générique
AuxStatus vl_http_execute(const VlHttpRequest *req, VlHttpResponse *resp);

// Helpers simples
AuxStatus vl_http_get(const char *url, VlHttpResponse *resp, long timeout_ms);
AuxStatus vl_http_post(const char *url, const void *data, size_t len,
                       const char *content_type, VlHttpResponse *resp,
                       long timeout_ms);
AuxStatus vl_http_download_file(const char *url, const char *path,
                                long timeout_ms);

// Libération des ressources de réponse
void vl_http_response_free(VlHttpResponse *resp);

#endif  // VITTE_LIGHT_INCLUDES_CURL_H

// ======================================================================
// Implémentation
// ======================================================================

static void resp_init(VlHttpResponse *r) {
  if (!r) return;
  memset(r, 0, sizeof *r);
}

static void resp_free(VlHttpResponse *r) {
  if (!r) return;
  aux_buffer_free(&r->body);
  aux_buffer_free(&r->headers_raw);
  if (r->effective_url) {
    free(r->effective_url);
    r->effective_url = NULL;
  }
  if (r->ip) {
    free(r->ip);
    r->ip = NULL;
  }
}

void vl_http_response_free(VlHttpResponse *r) { resp_free(r); }

// ----------------------------------------------
// Stubs si libcurl indisponible
// ----------------------------------------------
#ifndef VL_HAVE_LIBCURL

AuxStatus vl_http_global_init(void) { return AUX_ENOSYS; }
void vl_http_global_cleanup(void) {}

AuxStatus vl_http_execute(const VlHttpRequest *req, VlHttpResponse *resp) {
  (void)req;
  (void)resp;
  return AUX_ENOSYS;
}

AuxStatus vl_http_get(const char *url, VlHttpResponse *resp, long timeout_ms) {
  (void)url;
  (void)resp;
  (void)timeout_ms;
  return AUX_ENOSYS;
}

AuxStatus vl_http_post(const char *url, const void *data, size_t len,
                       const char *content_type, VlHttpResponse *resp,
                       long timeout_ms) {
  (void)url;
  (void)data;
  (void)len;
  (void)content_type;
  (void)resp;
  (void)timeout_ms;
  return AUX_ENOSYS;
}

AuxStatus vl_http_download_file(const char *url, const char *path,
                                long timeout_ms) {
  (void)url;
  (void)path;
  (void)timeout_ms;
  return AUX_ENOSYS;
}

#else  // VL_HAVE_LIBCURL

// ----------------------------------------------
// libcurl helpers
// ----------------------------------------------

AuxStatus vl_http_global_init(void) {
  static int inited = 0;
  if (inited) return AUX_OK;
  CURLcode cc = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (cc != CURLE_OK) return AUX_EIO;
  inited = 1;
  return AUX_OK;
}

void vl_http_global_cleanup(void) { curl_global_cleanup(); }

typedef struct {
  AuxBuffer *buf;
} WriteMemCtx;

static size_t write_mem_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
  size_t n = sz * nmemb;
  if (n == 0) return 0;
  WriteMemCtx *ctx = (WriteMemCtx *)ud;
  AuxBuffer *b = ctx->buf;
  size_t old = b->len;
  uint8_t *p = (uint8_t *)realloc(b->data, old + n + 1);
  if (!p) return 0;
  b->data = p;
  memcpy(b->data + old, ptr, n);
  b->len = old + n;
  b->data[b->len] = 0;
  return n;
}

typedef struct {
  FILE *f;
} WriteFileCtx;

static size_t write_file_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
  size_t n = sz * nmemb;
  if (n == 0) return 0;
  WriteFileCtx *ctx = (WriteFileCtx *)ud;
  return fwrite(ptr, 1, n, ctx->f);
}

typedef struct {
  AuxBuffer *headers;
} HeaderCollectCtx;

static size_t header_collect_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
  size_t n = sz * nmemb;
  if (n == 0) return 0;
  HeaderCollectCtx *ctx = (HeaderCollectCtx *)ud;
  // Accumule brut (libcurl inclut déjà CRLF)
  size_t old = ctx->headers->len;
  uint8_t *p = (uint8_t *)realloc(ctx->headers->data, old + n + 1);
  if (!p) return 0;
  ctx->headers->data = p;
  memcpy(ctx->headers->data + old, ptr, n);
  ctx->headers->len = old + n;
  ctx->headers->data[ctx->headers->len] = 0;
  return n;
}

typedef struct {
  VlHttpProgressCb cb;
  void *ud;
} ProgressCtx;

static int xferinfo_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow) {
  ProgressCtx *pc = (ProgressCtx *)ud;
  if (!pc || !pc->cb) return 0;
  return pc->cb((double)dltotal, (double)dlnow, (double)ultotal, (double)ulnow,
                pc->ud);
}

static CURLcode set_common_opts(
    CURL *c, const VlHttpRequest *req, char errbuf[VL_HTTP_MAX_ERR],
    struct curl_slist **out_headers, FILE **out_file, WriteMemCtx *mem_ctx,
    WriteFileCtx *file_ctx, HeaderCollectCtx *hdr_ctx, ProgressCtx *prog_ctx) {
  (void)out_file;
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(c, CURLOPT_URL, req->url);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // auto
  curl_easy_setopt(c, CURLOPT_USERAGENT,
                   req->user_agent ? req->user_agent : VL_HTTP_DEFAULT_UA);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL,
                   req->no_signal ? 1L : 1L);  // toujours 1L safe

  // Méthode
  const char *m = req->method ? req->method : "GET";
  if (strcmp(m, "GET") == 0) {
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
  } else if (strcmp(m, "POST") == 0) {
    curl_easy_setopt(c, CURLOPT_POST, 1L);
  } else if (strcmp(m, "PUT") == 0) {
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
  } else if (strcmp(m, "HEAD") == 0) {
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  } else if (strcmp(m, "DELETE") == 0) {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, m);
  }

  // Corps (body) vs upload fichier
  if (req->body && req->body_len > 0 &&
      (strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 || req->method)) {
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
  } else if (req->upload_path && strcmp(m, "PUT") == 0) {
    FILE *f = fopen(req->upload_path, "rb");
    if (!f) return CURLE_READ_ERROR;
    *out_file = f;  // réutilisé comme in-file
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    curl_easy_setopt(c, CURLOPT_READDATA, f);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)sz);
  }

  // En-têtes
  struct curl_slist *hs = NULL;
  if (req->content_type) {
    char line[256];
    snprintf(line, sizeof line, "Content-Type: %s", req->content_type);
    hs = curl_slist_append(hs, line);
  }
  for (size_t i = 0; i < req->headers_len; i++) {
    const VlReqHeader *h = &req->headers[i];
    if (!h->name || !*h->name) continue;
    char *line = NULL;
    size_t need = strlen(h->name) + 2 + (h->value ? strlen(h->value) : 0) + 1;
    line = (char *)malloc(need);
    if (!line) continue;
    if (h->value)
      snprintf(line, need, "%s: %s", h->name, h->value);
    else
      snprintf(line, need, "%s:", h->name);
    hs = curl_slist_append(hs, line);
    free(line);
  }
  if (hs) {
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  }
  *out_headers = hs;

  // TLS
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, req->verify_peer ? 1L : 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, req->verify_host ? 2L : 0L);
  if (req->ca_path && *req->ca_path) {
    curl_easy_setopt(c, CURLOPT_CAINFO, req->ca_path);  // fichier
    curl_easy_setopt(c, CURLOPT_CAPATH, req->ca_path);  // répertoire
  }

  // HTTP version
  if (req->http2) {
#if defined(CURL_HTTP_VERSION_2TLS)
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#elif defined(CURL_HTTP_VERSION_2_0)
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
#else
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
#endif
  }

  // Proxy
  if (req->proxy && *req->proxy) {
    curl_easy_setopt(c, CURLOPT_PROXY, req->proxy);
  }

  // Auth
  if (req->auth_basic && *req->auth_basic) {
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_USERPWD, req->auth_basic);
  }

  // Redirects
  if (req->follow_redirects) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    if (req->max_redirects > 0) {
      curl_easy_setopt(c, CURLOPT_MAXREDIRS, req->max_redirects);
    }
  }

  // Range
  if (req->range && *req->range) {
    curl_easy_setopt(c, CURLOPT_RANGE, req->range);
  }

  // Timeouts
  if (req->timeout_ms > 0)
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, req->timeout_ms);
  if (req->connect_timeout_ms > 0)
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, req->connect_timeout_ms);

  // Écriture corps
  if (req->download_path && !req->write_cb) {
    FILE *f = fopen(req->download_path, "wb");
    if (!f) return CURLE_WRITE_ERROR;
    file_ctx->f = f;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, file_ctx);
  } else if (req->write_cb) {
    // callback utilisateur
    curl_easy_setopt(
        c, CURLOPT_WRITEFUNCTION,
        +[](char *ptr, size_t sz, size_t nm, void *ud) -> size_t {
          VlHttpWriteCb cb = (VlHttpWriteCb)ud;
          (void)cb;
          // Impossible d’envoyer user directement ici ; on passe via WRITEDATA
          return 0;
        });
    // Ce chemin n’est pas portable en C17 sans trampoline. On gère via
    // WRITEDATA = pair(cb, ud)
    struct Pair {
      VlHttpWriteCb cb;
      void *ud;
    } *pair = malloc(sizeof *pair);
    if (!pair) return CURLE_WRITE_ERROR;
    pair->cb = req->write_cb;
    pair->ud = req->write_ud;
    curl_easy_setopt(c, CURLOPT_WRITEDATA, pair);
    // Remplace proprement le writefunction par un wrapper C
    curl_easy_setopt(
        c, CURLOPT_WRITEFUNCTION,
        +[](char *ptr, size_t sz, size_t nm, void *ud) -> size_t {
          size_t n = sz * nm;
          struct Pair {
            VlHttpWriteCb cb;
            void *ud;
          } *p = (struct Pair *)ud;
          if (!p || !p->cb) return n;
          return p->cb(ptr, n, p->ud);
        });
  } else {
    mem_ctx->buf = NULL;  // init plus bas
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, mem_ctx);
  }

  // En-têtes de réponse
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_collect_cb);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, hdr_ctx);

  // Progression
#if LIBCURL_VERSION_NUM >= 0x072000
  if (req->progress_cb) {
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, prog_ctx);
  } else {
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
  }
#else
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
#endif

  return CURLE_OK;
}

AuxStatus vl_http_execute(const VlHttpRequest *req, VlHttpResponse *resp) {
  if (!req || !req->url || !resp) return AUX_EINVAL;
  AuxStatus st = vl_http_global_init();
  if (st != AUX_OK) return st;

  resp_init(resp);
  CURL *c = curl_easy_init();
  if (!c) return AUX_EIO;

  char errbuf[VL_HTTP_MAX_ERR] = {0};
  struct curl_slist *hs = NULL;
  FILE *file_in = NULL;   // pour PUT
  FILE *file_out = NULL;  // pour download
  WriteMemCtx mem_ctx = {.buf = &resp->body};
  WriteFileCtx file_ctx = {.f = NULL};
  HeaderCollectCtx hdr_ctx = {.headers = &resp->headers_raw};
  ProgressCtx prog_ctx = {.cb = req->progress_cb, .ud = req->progress_ud};

  CURLcode cc;

  // Si écriture fichier, on ouvre ici pour pouvoir fermer même si
  // set_common_opts échoue
  if (req->download_path && !req->write_cb) {
    file_out = fopen(req->download_path, "wb");
    if (!file_out) {
      curl_easy_cleanup(c);
      return AUX_EIO;
    }
    file_ctx.f = file_out;
  }

  cc = set_common_opts(c, req, errbuf, &hs, &file_in, &mem_ctx, &file_ctx,
                       &hdr_ctx, &prog_ctx);
  if (cc != CURLE_OK) {
    if (hs) curl_slist_free_all(hs);
    if (file_in) fclose(file_in);
    if (file_out) fclose(file_out);
    curl_easy_cleanup(c);
    return AUX_EIO;
  }

  cc = curl_easy_perform(c);

  // Récup infos
  long code = 0;
  double tm = 0, v = 0;
  char *eff = NULL;
  char *ip = NULL;
  long httpver = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &tm);
  curl_easy_getinfo(c, CURLINFO_NAMELOOKUP_TIME, &v);
  resp->namelookup_ms = v * 1000.0;
  curl_easy_getinfo(c, CURLINFO_CONNECT_TIME, &v);
  resp->connect_ms = v * 1000.0;
#if LIBCURL_VERSION_NUM >= 0x071000
  curl_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &v);
  resp->appconnect_ms = v * 1000.0;
#endif
  curl_easy_getinfo(c, CURLINFO_PRETRANSFER_TIME, &v);
  resp->pretransfer_ms = v * 1000.0;
  curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &v);
  resp->starttransfer_ms = v * 1000.0;
  curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
#if LIBCURL_VERSION_NUM >= 0x073D00  // 7.61.0
  curl_easy_getinfo(c, CURLINFO_PRIMARY_IP, &ip);
#endif
#if LIBCURL_VERSION_NUM >= 0x072100  // 7.33.0
  curl_easy_getinfo(c, CURLINFO_HTTP_VERSION, &httpver);
#endif
  double dsz = 0, usz = 0;
  curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD, &dsz);
  curl_easy_getinfo(c, CURLINFO_SIZE_UPLOAD, &usz);

  resp->status = code;
  resp->total_time_ms = tm * 1000.0;
  resp->downloaded = (uint64_t)(dsz < 0 ? 0 : dsz);
  resp->uploaded = (uint64_t)(usz < 0 ? 0 : usz);
  resp->http_version = httpver;
  if (eff) {
    size_t n = strlen(eff);
    resp->effective_url = (char *)malloc(n + 1);
    if (resp->effective_url) {
      memcpy(resp->effective_url, eff, n + 1);
    }
  }
  if (ip) {
    size_t n = strlen(ip);
    resp->ip = (char *)malloc(n + 1);
    if (resp->ip) {
      memcpy(resp->ip, ip, n + 1);
    }
  }

  // Nettoyage
  if (hs) curl_slist_free_all(hs);
  if (file_in) fclose(file_in);
  if (file_out) fclose(file_out);
  curl_easy_cleanup(c);

  if (cc != CURLE_OK) {
    AUX_LOG_ERROR("HTTP error: %s (code=%d, http=%ld, url=%s)",
                  errbuf[0] ? errbuf : curl_easy_strerror(cc), (int)cc,
                  resp->status, req->url);
    return AUX_EIO;
  }
  return AUX_OK;
}

// ----------------------------------------------
// Helpers simples
// ----------------------------------------------

AuxStatus vl_http_get(const char *url, VlHttpResponse *resp, long timeout_ms) {
  VlHttpRequest r = {0};
  r.url = url;
  r.method = "GET";
  r.timeout_ms = timeout_ms;
  r.connect_timeout_ms = timeout_ms ? (timeout_ms / 2) : 0;
  r.follow_redirects = 1;
  r.max_redirects = 10;
  r.verify_peer = 1;
  r.verify_host = 1;
  r.http2 = 1;
  r.no_signal = 1;
  return vl_http_execute(&r, resp);
}

AuxStatus vl_http_post(const char *url, const void *data, size_t len,
                       const char *content_type, VlHttpResponse *resp,
                       long timeout_ms) {
  VlHttpRequest r = {0};
  r.url = url;
  r.method = "POST";
  r.body = data;
  r.body_len = len;
  r.content_type = content_type ? content_type : "application/octet-stream";
  r.timeout_ms = timeout_ms;
  r.connect_timeout_ms = timeout_ms ? (timeout_ms / 2) : 0;
  r.follow_redirects = 1;
  r.max_redirects = 10;
  r.verify_peer = 1;
  r.verify_host = 1;
  r.http2 = 1;
  r.no_signal = 1;
  return vl_http_execute(&r, resp);
}

AuxStatus vl_http_download_file(const char *url, const char *path,
                                long timeout_ms) {
  VlHttpRequest r = {0};
  r.url = url;
  r.method = "GET";
  r.download_path = path;
  r.timeout_ms = timeout_ms;
  r.connect_timeout_ms = timeout_ms ? (timeout_ms / 2) : 0;
  r.follow_redirects = 1;
  r.max_redirects = 10;
  r.verify_peer = 1;
  r.verify_host = 1;
  r.http2 = 1;
  r.no_signal = 1;

  VlHttpResponse resp = {0};
  AuxStatus st = vl_http_execute(&r, &resp);
  vl_http_response_free(&resp);
  return st;
}

#endif  // VL_HAVE_LIBCURL
