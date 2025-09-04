// vitte-light/libraries/curl.c
// Client HTTP(S) ultra‑complet pour VitteLight.
// Implémentation primaire via libcurl (WITH_LIBCURL). Fallback optionnel
// via binaire "curl" (si libcurl indisponible) en utilisant
// iolib::vl_exec_capture.
//
// Expose des natives haut niveau:
//   http_get(url[, timeout_ms])                  -> str|nil
//   http_head(url[, timeout_ms])                 -> int            // status
//   http_request(url, method, body, headers,
//                timeout_ms, follow, verify)     -> str|nil        // corps
//   http_download(url, path[, timeout_ms])       -> bool
//   http_upload(url, path[, method="PUT"])       -> bool
//   http_last_status()                           -> int            // dernier
//   code http_last_headers()                          -> str            // en
//   bytes http_last_error()                            -> str|nil
//   http_last_url()                              -> str
//   http_set_proxy(proxy_url|nil)                -> nil
//   http_set_user_agent(ua|nil)                  -> nil
//   http_set_cacert(path|nil)                    -> nil
//   http_set_default_timeout_ms(ms)              -> nil
//
// Conventions:
//  - headers: chaîne contenant des lignes "Name: Value\n..." (\r\n accepté)
//  - body: bytes (str VL). Aucune transformation.
//  - verify: 1 => vérification TLS, 0 => désactivée.
//  - follow: 1 => suivre redirections.
//  - Retour nil en cas d'erreur; http_last_error() fournit le diagnostic.
//  - http_request conserve aussi les en‑têtes bruts du serveur (dernier hop).
//
// Build libcurl:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -DWITH_LIBCURL \
//      -c libraries/curl.c && cc ... libraries/curl.o -lcurl
// Fallback sans libcurl (nécessite l'outil 'curl' dans le PATH):
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/curl.c

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "ctype.h"
#include "mem.h"     // VL_Buffer
#include "string.h"  // VL_String, vl_make_strn
#include "tm.h"      // vl_mono_time_ns

// Fallback externe (optionnel)
int vl_exec_capture(const char *cmd, char **out_text, size_t *out_len,
                    int *out_status);

// ───────────────────────── State ─────────────────────────
static struct {
  int last_status;
  VL_Buffer last_hdrs;      // bytes
  VL_Buffer last_err;       // cstring
  VL_Buffer last_url;       // cstring
  VL_Buffer scratch;        // réutilisé
  char *proxy;              // cstring (heap), NULL si none
  char *ua;                 // cstring
  char *cacert;             // cstring (chemin fichier CA)
  uint32_t def_timeout_ms;  // 0 = pas de limite
} g_http;

static void http_state_init(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;
  memset(&g_http, 0, sizeof(g_http));
}
static void buf_set_cstr(VL_Buffer *b, const char *s) {
  vl_buf_reset(b);
  if (!s) {
    return;
  }
  vl_buf_append(b, s, strlen(s));
  vl_buf_append(b, "\0", 1);
  b->n--;
}
static void buf_move_from(VL_Buffer *dst, VL_Buffer *src) {
  vl_buf_reset(dst);
  vl_buf_append(dst, src->d, src->n);
}
static void set_last_error(const char *msg) {
  vl_buf_reset(&g_http.last_err);
  if (msg) vl_buf_append(&g_http.last_err, msg, strlen(msg));
  vl_buf_append(&g_http.last_err, "\0", 1);
  if (g_http.last_err.n) g_http.last_err.n--;
}
static void set_last_url(const char *u) {
  buf_set_cstr(&g_http.last_url, u ? u : "");
}
static void set_last_status(int st) { g_http.last_status = st; }
static void set_last_headers(const void *p, size_t n) {
  vl_buf_reset(&g_http.last_hdrs);
  if (p && n) vl_buf_append(&g_http.last_hdrs, p, n);
}

static void free_c(char **p) {
  if (*p) {
    free(*p);
    *p = NULL;
  }
}

// ───────────────────────── Helpers ─────────────────────────
static int split_lines_headers(const char *headers, struct curl_slist **out) {
#if defined(WITH_LIBCURL)
  if (!headers || !*headers) return 1;
  const char *p = headers;
  struct curl_slist *lst = NULL;
  while (*p) {
    const char *e = p;
    while (*e && *e != '\n' && *e != '\r') e++;
    size_t L = (size_t)(e - p);
    if (L) {
      char *line = (char *)malloc(L + 1);
      if (!line) {
        curl_slist_free_all(lst);
        return 0;
      }
      memcpy(line, p, L);
      line[L] = '\0';
      lst = curl_slist_append(lst, line);
      free(line);
    }
    while (*e == '\r' || *e == '\n') e++;
    p = e;
  }
  *out = lst;
  return 1;
#else
  (void)headers;
  (void)out;
  return 1;
#endif
}

static void write_shell_escaped(
    VL_Buffer *b, const char *s) {  // POSIX quoting using single quotes
  vl_buf_append(b, "'", 1);
  for (const char *p = s; *p; ++p) {
    if (*p == '\'') {
      vl_buf_append(b, "'\"'\"'", 6);
    } else {
      vl_buf_append(b, p, 1);
    }
  }
  vl_buf_append(b, "'", 1);
}

static int parse_boolish(const VL_Value *v, int def) {
  if (!v) return def;
  switch (v->type) {
    case VT_NIL:
      return def;
    case VT_BOOL:
      return v->as.b ? 1 : 0;
    case VT_INT:
      return v->as.i != 0;
    case VT_FLOAT:
      return v->as.f != 0.0;
    case VT_STR:
      return (v->as.s && v->as.s->len) ? 1 : 0;
    default:
      return def;
  }
}

static int64_t want_i64(const VL_Value *v, int *ok) {
  int64_t x = 0;
  *ok = vl_value_as_int(v, &x);
  return x;
}

// ───────────────────────── Impl libcurl ─────────────────────────
#if defined(WITH_LIBCURL)
#include <curl/curl.h>

static size_t cb_write_body(char *ptr, size_t sz, size_t nm, void *ud) {
  VL_Buffer *b = (VL_Buffer *)ud;
  size_t n = sz * nm;
  vl_buf_append(b, ptr, n);
  return n;
}
static size_t cb_write_hdr(char *ptr, size_t sz, size_t nm, void *ud) {
  VL_Buffer *b = (VL_Buffer *)ud;
  size_t n = sz * nm;
  vl_buf_append(b, ptr, n);
  return n;
}

static int libcurl_do(const char *url, const char *method, const void *body,
                      size_t body_n, const char *headers, uint32_t timeout_ms,
                      int follow, int verify, VL_Buffer *out_body) {
  CURLcode rc;
  CURL *h = curl_easy_init();
  if (!h) {
    set_last_error("curl_easy_init");
    return 0;
  }
  vl_buf_reset(out_body);
  vl_buf_reset(&g_http.last_hdrs);
  set_last_status(0);
  set_last_error(NULL);
  set_last_url(url);
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING,
                   "");  // auto decode gzip/deflate/brotli si support
  if (timeout_ms) curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  if (g_http.ua) curl_easy_setopt(h, CURLOPT_USERAGENT, g_http.ua);
  if (g_http.proxy) curl_easy_setopt(h, CURLOPT_PROXY, g_http.proxy);
  if (!verify) {
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  if (g_http.cacert) curl_easy_setopt(h, CURLOPT_CAINFO, g_http.cacert);

  struct curl_slist *hdr = NULL;
  if (!split_lines_headers(headers, &hdr)) {
    curl_easy_cleanup(h);
    set_last_error("headers OOM");
    return 0;
  }

  if (method && *method) {
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
  }
  if (body && body_n) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body_n);
  }
  if (hdr) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);

  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, cb_write_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, out_body);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, cb_write_hdr);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &g_http.last_hdrs);

  rc = curl_easy_perform(h);
  long code = 0;
  if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
  set_last_status((int)code);
  if (hdr) curl_slist_free_all(hdr);
  if (rc != CURLE_OK) {
    set_last_error(curl_easy_strerror(rc));
    curl_easy_cleanup(h);
    return 0;
  }
  curl_easy_cleanup(h);
  return 1;
}

#else  // ───────────── Fallback par binaire curl(1) ─────────────

static int libcurl_do(const char *url, const char *method, const void *body,
                      size_t body_n, const char *headers, uint32_t timeout_ms,
                      int follow, int verify, VL_Buffer *out_body) {
  // Construit: curl -sS -i [-L] [--max-time N] [-k] [-X METHOD] [-H ...]
  // [--data-binary @-]
  set_last_url(url);
  set_last_status(0);
  set_last_error(NULL);
  vl_buf_reset(&g_http.last_hdrs);
  vl_buf_reset(out_body);
  VL_Buffer cmd;
  vl_buf_init(&cmd);
  const char *bin = getenv("CURL");
  if (!bin || !*bin) bin = "curl";
  vl_buf_printf(&cmd, "%s -sS -i", bin);
  if (follow) vl_buf_printf(&cmd, " -L");
  if (timeout_ms)
    vl_buf_printf(&cmd, " --max-time %.3f", (double)timeout_ms / 1000.0);
  if (!verify) vl_buf_printf(&cmd, " -k");
  if (g_http.ua) {
    vl_buf_printf(&cmd, " -A ");
    write_shell_escaped(&cmd, g_http.ua);
  }
  if (g_http.proxy) {
    vl_buf_printf(&cmd, " -x ");
    write_shell_escaped(&cmd, g_http.proxy);
  }
  if (g_http.cacert) {
    vl_buf_printf(&cmd, " --cacert ");
    write_shell_escaped(&cmd, g_http.cacert);
  }
  if (method && *method) {
    vl_buf_printf(&cmd, " -X ");
    write_shell_escaped(&cmd, method);
  }
  // headers
  if (headers && *headers) {
    const char *p = headers;
    while (*p) {
      const char *e = p;
      while (*e && *e != '\n' && *e != '\r') e++;
      if (e > p) {
        vl_buf_printf(&cmd, " -H ");
        VL_Buffer ln;
        vl_buf_init(&ln);
        vl_buf_append(&ln, p, (size_t)(e - p));
        vl_buf_append(&ln, "\0", 1);
        write_shell_escaped(&cmd, (const char *)ln.d);
        vl_buf_free(&ln);
      }
      while (*e == '\n' || *e == '\r') e++;
      p = e;
    }
  }
  // body via stdin if fourni
  if (body && body_n) {
    vl_buf_printf(&cmd, " --data-binary @-");
  }
  vl_buf_printf(&cmd, " ");
  write_shell_escaped(&cmd, url);

  char *out = NULL;
  size_t on = 0;
  int st = 0;
  int ok;
  if (body && body_n) {  // besoin d'une commande "sh -c" pour pipe echo -> curl
    // Pour simplicité et portabilité différée, on traite data en base64? Non.
    // On passe via popen non supporté ici. On se rabat sur un fichier temp.
    char tfile[512];
    snprintf(tfile, sizeof(tfile), "/tmp/vl_curl_%" PRIu64 ".bin",
             (uint64_t)vl_mono_time_ns());
    FILE *f = fopen(tfile, "wb");
    if (!f) {
      set_last_error("tempfile");
      vl_buf_free(&cmd);
      return 0;
    }
    fwrite(body, 1, body_n, f);
    fclose(f);
    VL_Buffer cmd2;
    vl_buf_init(&cmd2);
    vl_buf_append(&cmd2, cmd.d, cmd.n);
    vl_buf_printf(&cmd2, " --data-binary @");
    write_shell_escaped(&cmd2, tfile);
    ok = vl_exec_capture((const char *)cmd2.d, &out, &on, &st);
    remove(tfile);
    vl_buf_free(&cmd2);
  } else {
    ok = vl_exec_capture((const char *)cmd.d, &out, &on, &st);
  }
  vl_buf_free(&cmd);
  if (!ok) {
    set_last_error("exec curl");
    return 0;
  }
  // Parse -i output: headers CRLF CRLF body
  size_t i = 0;
  int found = 0;
  for (i = 0; i + 3 < on; i++) {
    if (out[i] == '\r' && out[i + 1] == '\n' && out[i + 2] == '\r' &&
        out[i + 3] == '\n') {
      found = 1;
      break;
    }
    if (out[i] == '\n' && out[i + 1] == '\n') {
      found = 1;
      break;
    }
  }
  if (found) {
    set_last_headers(out, i + ((out[i] == '\r') ? 4 : 2));
    vl_buf_append(out_body, out + i + ((out[i] == '\r') ? 4 : 2),
                  on - (i + ((out[i] == '\r') ? 4 : 2)));
  } else {  // Pas de headers? Considère tout comme body
    vl_buf_append(out_body, out, on);
  }
  // Status via première ligne HTTP/1.x ...
  int code = 0;
  if (g_http.last_hdrs.n > 0) {
    const char *h = (const char *)g_http.last_hdrs.d;
    const char *sp = strchr(h, ' ');
    if (sp) code = atoi(sp + 1);
  }
  if (!code) code = (st == 0) ? 200 : 0;
  set_last_status(code);
  free(out);
  return 1;
}
#endif

// ───────────────────────── Natives ─────────────────────────
#define RET_NIL()                \
  do {                           \
    if (ret) *(ret) = vlv_nil(); \
    return VL_OK;                \
  } while (0)
#define RET_INT(v)                           \
  do {                                       \
    if (ret) *(ret) = vlv_int((int64_t)(v)); \
    return VL_OK;                            \
  } while (0)
#define RET_BOOL(v)                       \
  do {                                    \
    if (ret) *(ret) = vlv_bool((v) != 0); \
    return VL_OK;                         \
  } while (0)
#define RET_STR(p, n)                                                   \
  do {                                                                  \
    VL_Value __s = vl_make_strn(ctx, (const char *)(p), (uint32_t)(n)); \
    if (__s.type != VT_STR) return VL_ERR_OOM;                          \
    if (ret) *ret = __s;                                                \
    return VL_OK;                                                       \
  } while (0)

static int need_str(const VL_Value *v) {
  return v && v->type == VT_STR && v->as.s;
}

static VL_Status nb_http_request(struct VL_Context *ctx, const VL_Value *a,
                                 uint8_t c, VL_Value *ret, void *ud) {
  (void)ud;
  http_state_init();
  if (c < 2 || !need_str(&a[0]) || !need_str(&a[1])) return VL_ERR_TYPE;
  const char *url = a[0].as.s->data;
  const char *method = a[1].as.s->data;
  const void *body = NULL;
  size_t body_n = 0;
  const char *hdrs = NULL;
  uint32_t tmo = g_http.def_timeout_ms;
  int follow = 1, verify = 1;
  int ok = 1;
  int64_t x = 0;
  if (c >= 3 && a[2].type != VT_NIL) {
    if (!need_str(&a[2])) return VL_ERR_TYPE;
    body = a[2].as.s->data;
    body_n = a[2].as.s->len;
  }
  if (c >= 4 && a[3].type != VT_NIL) {
    if (!need_str(&a[3])) return VL_ERR_TYPE;
    hdrs = a[3].as.s->data;
  }
  if (c >= 5 && a[4].type != VT_NIL) {
    x = want_i64(&a[4], &ok);
    if (!ok || x < 0) return VL_ERR_TYPE;
    tmo = (uint32_t)x;
  }
  if (c >= 6 && a[5].type != VT_NIL) follow = parse_boolish(&a[5], 1);
  if (c >= 7 && a[6].type != VT_NIL) verify = parse_boolish(&a[6], 1);

  VL_Buffer body_out;
  vl_buf_init(&body_out);
  int success = libcurl_do(url, method, body, body_n, hdrs, tmo, follow, verify,
                           &body_out);
  if (!success) {
    vl_buf_free(&body_out);
    RET_NIL();
  }
  RET_STR(body_out.d, body_out.n);
}

static VL_Status nb_http_get(struct VL_Context *ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *ud) {
  (void)ud;
  http_state_init();
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  uint32_t tmo = g_http.def_timeout_ms;
  if (c >= 2 && a[1].type != VT_NIL) {
    int ok = 1;
    int64_t x = want_i64(&a[1], &ok);
    if (!ok || x < 0) return VL_ERR_TYPE;
    tmo = (uint32_t)x;
  }
  VL_Buffer out;
  vl_buf_init(&out);
  int ok = libcurl_do(a[0].as.s->data, "GET", NULL, 0, NULL, tmo, 1, 1, &out);
  if (!ok) {
    vl_buf_free(&out);
    RET_NIL();
  }
  RET_STR(out.d, out.n);
}

static VL_Status nb_http_head(struct VL_Context *ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *ud) {
  (void)ud;
  http_state_init();
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  uint32_t tmo = g_http.def_timeout_ms;
  if (c >= 2 && a[1].type != VT_NIL) {
    int ok = 1;
    int64_t x = want_i64(&a[1], &ok);
    if (!ok || x < 0) return VL_ERR_TYPE;
    tmo = (uint32_t)x;
  }
#if defined(WITH_LIBCURL)
  VL_Buffer dummy;
  vl_buf_init(&dummy);
  int ok =
      libcurl_do(a[0].as.s->data, "HEAD", NULL, 0, NULL, tmo, 1, 1, &dummy);
  vl_buf_free(&dummy);
  if (!ok) {
    RET_INT(0);
  }
  RET_INT(g_http.last_status);
#else
  VL_Buffer out;
  vl_buf_init(&out);
  int ok = libcurl_do(a[0].as.s->data, "HEAD", NULL, 0, NULL, tmo, 1, 1, &out);
  vl_buf_free(&out);
  if (!ok) {
    RET_INT(0);
  }
  RET_INT(g_http.last_status);
#endif
}

static VL_Status nb_http_download(struct VL_Context *ctx, const VL_Value *a,
                                  uint8_t c, VL_Value *ret, void *ud) {
  (void)ud;
  http_state_init();
  if (c < 2 || !need_str(&a[0]) || !need_str(&a[1])) return VL_ERR_TYPE;
  uint32_t tmo = g_http.def_timeout_ms;
  if (c >= 3 && a[2].type != VT_NIL) {
    int ok = 1;
    int64_t x = want_i64(&a[2], &ok);
    if (!ok || x < 0) return VL_ERR_TYPE;
    tmo = (uint32_t)x;
  }
  VL_Buffer out;
  vl_buf_init(&out);
  int ok = libcurl_do(a[0].as.s->data, "GET", NULL, 0, NULL, tmo, 1, 1, &out);
  if (!ok) {
    vl_buf_free(&out);
    RET_BOOL(0);
  }
  FILE *f = fopen(a[1].as.s->data, "wb");
  if (!f) {
    vl_buf_free(&out);
    RET_BOOL(0);
  }
  size_t wr = fwrite(out.d, 1, out.n, f);
  int ok2 = (wr == out.n && fflush(f) == 0 && fclose(f) == 0);
  if (!ok2) fclose(f);
  vl_buf_free(&out);
  RET_BOOL(ok2);
}

static VL_Status nb_http_upload(struct VL_Context *ctx, const VL_Value *a,
                                uint8_t c, VL_Value *ret, void *ud) {
  (void)ud;
  http_state_init();
  if (c < 2 || !need_str(&a[0]) || !need_str(&a[1])) return VL_ERR_TYPE;
  const char *method = (c >= 3 && need_str(&a[2]))
                           ? a[2].as.s->data
                           : "PUT";  // lit fichier et envoie
  unsigned char *buf = NULL;
  size_t n = 0;
  extern int vl_read_file_all(const char *, unsigned char **, size_t *);
  if (!vl_read_file_all(a[1].as.s->data, &buf, &n)) return VL_ERR_IO;
  VL_Buffer out;
  vl_buf_init(&out);
  int ok = libcurl_do(a[0].as.s->data, method, buf, n, NULL,
                      g_http.def_timeout_ms, 1, 1, &out);
  free(buf);
  if (!ok) {
    vl_buf_free(&out);
    RET_BOOL(0);
  }
  vl_buf_free(&out);
  RET_BOOL(g_http.last_status >= 200 && g_http.last_status < 300);
}

static VL_Status nb_http_last_status(struct VL_Context *ctx, const VL_Value *a,
                                     uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  http_state_init();
  RET_INT(g_http.last_status);
}
static VL_Status nb_http_last_headers(struct VL_Context *ctx, const VL_Value *a,
                                      uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  http_state_init();
  RET_STR(g_http.last_hdrs.d, g_http.last_hdrs.n);
}
static VL_Status nb_http_last_error(struct VL_Context *ctx, const VL_Value *a,
                                    uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  http_state_init();
  if (g_http.last_err.n == 0) RET_NIL();
  RET_STR(g_http.last_err.d, g_http.last_err.n);
}
static VL_Status nb_http_last_url(struct VL_Context *ctx, const VL_Value *a,
                                  uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  http_state_init();
  RET_STR(g_http.last_url.d, g_http.last_url.n);
}

static VL_Status nb_http_set_proxy(struct VL_Context *ctx, const VL_Value *a,
                                   uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  http_state_init();
  free_c(&g_http.proxy);
  if (c < 1 || a[0].type == VT_NIL) {
    RET_NIL();
  }
  if (!need_str(&a[0])) return VL_ERR_TYPE;
  g_http.proxy = strdup(a[0].as.s->data);
  RET_NIL();
}
static VL_Status nb_http_set_user_agent(struct VL_Context *ctx,
                                        const VL_Value *a, uint8_t c,
                                        VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  http_state_init();
  free_c(&g_http.ua);
  if (c < 1 || a[0].type == VT_NIL) {
    RET_NIL();
  }
  if (!need_str(&a[0])) return VL_ERR_TYPE;
  g_http.ua = strdup(a[0].as.s->data);
  RET_NIL();
}
static VL_Status nb_http_set_cacert(struct VL_Context *ctx, const VL_Value *a,
                                    uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  http_state_init();
  free_c(&g_http.cacert);
  if (c < 1 || a[0].type == VT_NIL) {
    RET_NIL();
  }
  if (!need_str(&a[0])) return VL_ERR_TYPE;
  g_http.cacert = strdup(a[0].as.s->data);
  RET_NIL();
}
static VL_Status nb_http_set_default_timeout_ms(struct VL_Context *ctx,
                                                const VL_Value *a, uint8_t c,
                                                VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  http_state_init();
  if (c < 1 || a[0].type == VT_NIL) {
    g_http.def_timeout_ms = 0;
    RET_NIL();
  }
  int ok = 1;
  int64_t x = want_i64(&a[0], &ok);
  if (!ok || x < 0) return VL_ERR_TYPE;
  g_http.def_timeout_ms = (uint32_t)x;
  RET_NIL();
}

// ───────────────────────── Registration ─────────────────────────
void vl_register_curl(struct VL_Context *ctx) {
  if (!ctx) return;
  http_state_init();
#if defined(WITH_LIBCURL)
  static int once = 0;
  if (!once) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    once = 1;
  }
#endif
  vl_register_native(ctx, "http_get", nb_http_get, NULL);
  vl_register_native(ctx, "http_head", nb_http_head, NULL);
  vl_register_native(ctx, "http_request", nb_http_request, NULL);
  vl_register_native(ctx, "http_download", nb_http_download, NULL);
  vl_register_native(ctx, "http_upload", nb_http_upload, NULL);

  vl_register_native(ctx, "http_last_status", nb_http_last_status, NULL);
  vl_register_native(ctx, "http_last_headers", nb_http_last_headers, NULL);
  vl_register_native(ctx, "http_last_error", nb_http_last_error, NULL);
  vl_register_native(ctx, "http_last_url", nb_http_last_url, NULL);

  vl_register_native(ctx, "http_set_proxy", nb_http_set_proxy, NULL);
  vl_register_native(ctx, "http_set_user_agent", nb_http_set_user_agent, NULL);
  vl_register_native(ctx, "http_set_cacert", nb_http_set_cacert, NULL);
  vl_register_native(ctx, "http_set_default_timeout_ms",
                     nb_http_set_default_timeout_ms, NULL);
}

// ───────────────────────── Test rapide (optionnel) ─────────────────────────
#ifdef VL_CURL_TEST_MAIN
int main(void) {
  const char *u = "https://httpbin.org/get";
  VL_Buffer body;
  vl_buf_init(&body);
  int ok = libcurl_do(u, "GET", NULL, 0, NULL, 3000, 1, 1, &body);
  fprintf(stderr, "ok=%d status=%d body=%zu hdrs=%zu err='%s'\n", ok,
          g_http.last_status, body.n, g_http.last_hdrs.n,
          g_http.last_err.n ? (char *)g_http.last_err.d : "");
  if (body.n) {
    fwrite(body.d, 1, body.n, stdout);
    fputc('\n', stdout);
  }
  vl_buf_free(&body);
#if defined(WITH_LIBCURL)
  curl_global_cleanup();
#endif
  return ok ? 0 : 1;
}
#endif
