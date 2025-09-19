// SPDX-License-Identifier: GPL-3.0-or-later
//
// http.c — HTTP front-end for Vitte Light VM (C17, complet)
// Namespace: "http"
//
// Build examples:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_CURL -c http.c
//   cc ... http.o -lcurl
//
// Model:
//   - GET/POST/PUT/DELETE via libcurl easy API.
//   - En-têtes personnalisés, timeout, User-Agent, redirections.
//   - Renvoie code HTTP, corps et en-têtes bruts.
//   - One-shot helpers. Pas de cookies persistants ni HTTP/2 push.
//
// API (C symbol layer):
//   typedef struct {
//     int   status;        // 200…
//     char* headers;       // buffer NUL-terminé, à free()
//     unsigned char* body; // buffer binaire, à free()
//     size_t body_len;
//   } http_response;
//
//   // headers: tableau de C-strings "Key: Value" de taille nh (peut être NULL, nh=0)
//   // method: "GET" | "POST" | "PUT" | "DELETE" | "PATCH"
//   // timeout_ms: <=0 pour défaut (30s)
//   // data peut être NULL si dlen=0
//   int http_request(const char* method, const char* url,
//                    const char* const* headers, size_t nh,
//                    const void* data, size_t dlen,
//                    int timeout_ms, const char* user_agent,
//                    http_response* out); // 0 | -EINVAL | -ENOSYS | -ENOMEM
//
//   // Helpers
//   int http_get (const char* url, const char* const* headers, size_t nh,
//                 int timeout_ms, const char* ua, http_response* out);
//   int http_post(const char* url, const char* const* headers, size_t nh,
//                 const void* data, size_t dlen,
//                 int timeout_ms, const char* ua, http_response* out);
//   void http_response_free(http_response* r);
//
// Notes:
//   - Cette couche C est neutre VM. Le binding VM doit copier body via vl_push_lstring.
//   - Erreurs: -EINVAL, -ENOSYS, -ENOMEM, -EIO.
//
// Deps VM optionnels: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifndef ENOSYS
#  define ENOSYS 38
#endif
#ifndef EINVAL
#  define EINVAL 22
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

typedef struct {
  int   status;
  char* headers;
  unsigned char* body;
  size_t body_len;
} http_response;

VL_EXPORT void http_response_free(http_response* r) {
  if (!r) return;
  free(r->headers);
  free(r->body);
  r->headers = NULL;
  r->body = NULL;
  r->body_len = 0;
  r->status = 0;
}

#ifdef VL_HAVE_CURL
#  include <curl/curl.h>

typedef struct {
  unsigned char* buf;
  size_t len;
  size_t cap;
} dynbuf;

static int db_reserve(dynbuf* b, size_t need) {
  if (need <= b->cap) return 0;
  size_t ncap = b->cap ? b->cap : 4096;
  while (ncap < need) {
    size_t next = ncap * 2u;
    if (next < ncap) return -ENOMEM;
    ncap = next;
  }
  void* p = realloc(b->buf, ncap);
  if (!p) return -ENOMEM;
  b->buf = (unsigned char*)p;
  b->cap = ncap;
  return 0;
}

static size_t wr_body(char* ptr, size_t sz, size_t nm, void* ud) {
  dynbuf* b = (dynbuf*)ud;
  size_t n = sz * nm;
  if (db_reserve(b, b->len + n + 1) != 0) return 0;
  memcpy(b->buf + b->len, ptr, n);
  b->len += n;
  b->buf[b->len] = 0;
  return n;
}
static size_t wr_hdr(char* ptr, size_t sz, size_t nm, void* ud) {
  dynbuf* b = (dynbuf*)ud;
  size_t n = sz * nm;
  if (db_reserve(b, b->len + n + 1) != 0) return 0;
  memcpy(b->buf + b->len, ptr, n);
  b->len += n;
  b->buf[b->len] = 0;
  return n;
}

static int apply_headers(CURL* c, const char* const* headers, size_t nh, struct curl_slist** out) {
  struct curl_slist* lst = NULL;
  for (size_t i = 0; i < nh; ++i) {
    if (!headers[i] || !*headers[i]) continue;
    lst = curl_slist_append(lst, headers[i]);
    if (!lst) { curl_slist_free_all(lst); return -ENOMEM; }
  }
  *out = lst;
  if (lst) curl_easy_setopt(c, CURLOPT_HTTPHEADER, lst);
  return 0;
}

static int method_to_curl(const char* m, CURL* c, const void* data, size_t dlen) {
  if (!m) return -EINVAL;
  if (strcmp(m, "GET") == 0) {
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    return 0;
  }
  if (strcmp(m, "POST") == 0) {
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, data ? data : "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)dlen);
    return 0;
  }
  if (strcmp(m, "PUT") == 0) {
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_READDATA, NULL);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)dlen);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, data ? data : "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)dlen);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
    return 0;
  }
  if (strcmp(m, "DELETE") == 0 || strcmp(m, "PATCH") == 0) {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, m);
    if (dlen) {
      curl_easy_setopt(c, CURLOPT_POSTFIELDS, data);
      curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)dlen);
    }
    return 0;
  }
  return -EINVAL;
}

VL_EXPORT int http_request(const char* method, const char* url,
                           const char* const* headers, size_t nh,
                           const void* data, size_t dlen,
                           int timeout_ms, const char* user_agent,
                           http_response* out) {
  if (!url || !out) return -EINVAL;
  memset(out, 0, sizeof(*out));

  CURL* c = curl_easy_init();
  if (!c) return -EIO;

  dynbuf b = {0}, h = {0};
  long code = 0;
  struct curl_slist* lst = NULL;

  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_body);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, wr_hdr);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &h);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // enable all supported
  if (user_agent && *user_agent) curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent);
  if (timeout_ms > 0) {
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
  }

  int rc = apply_headers(c, headers, nh, &lst);
  if (rc != 0) { curl_easy_cleanup(c); return rc; }

  rc = method_to_curl(method ? method : "GET", c, data, dlen);
  if (rc != 0) {
    curl_slist_free_all(lst);
    curl_easy_cleanup(c);
    return rc;
  }

  CURLcode e = curl_easy_perform(c);
  if (e != CURLE_OK) {
    curl_slist_free_all(lst);
    curl_easy_cleanup(c);
    free(b.buf); free(h.buf);
    return -EIO;
  }
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

  out->status   = (int)code;
  out->body     = b.buf;
  out->body_len = b.len;
  out->headers  = (char*)h.buf;

  curl_slist_free_all(lst);
  curl_easy_cleanup(c);
  return 0;
}

#else  // !VL_HAVE_CURL

VL_EXPORT int http_request(const char* method, const char* url,
                           const char* const* headers, size_t nh,
                           const void* data, size_t dlen,
                           int timeout_ms, const char* user_agent,
                           http_response* out) {
  (void)method; (void)url; (void)headers; (void)nh;
  (void)data; (void)dlen; (void)timeout_ms; (void)user_agent; (void)out;
  return -ENOSYS;
}

#endif // VL_HAVE_CURL

VL_EXPORT int http_get(const char* url, const char* const* headers, size_t nh,
                       int timeout_ms, const char* ua, http_response* out) {
  return http_request("GET", url, headers, nh, NULL, 0, timeout_ms, ua, out);
}

VL_EXPORT int http_post(const char* url, const char* const* headers, size_t nh,
                        const void* data, size_t dlen,
                        int timeout_ms, const char* ua, http_response* out) {
  return http_request("POST", url, headers, nh, data, dlen, timeout_ms, ua, out);
}