/* ============================================================================
   conf.h — Configuration loader/manager (C17, portable)
   Features:
     - Load from files (INI-like), environment (PREFIX_*), and argv
     - Sections: [db] host=...  → key "db.host"
     - Env prefix mapping: APP_DB__HOST → "db.host" (double underscore → '.')
     - Types: string, bool, int64 (dec/hex/bin with _), double (with K/M/G/T)
     - Variable expansion: "url=${DB__HOST}:${DB__PORT}" (conf first, then env)
     - Save back to file (grouped by section), enumerate keys
     - Header-only option (CONF_HEADER_ONLY) or pair with conf.c
   Config:
     - Define CONF_API for export (e.g., __declspec(dllexport))
     - Define CONF_HEADER_ONLY to inline the implementation here
     - Define CONF_MALLOC/CONF_REALLOC/CONF_FREE to override allocators
   Notes:
     - ASCII case-insensitive keys, normalized to lowercase
     - Not inherently thread-safe; guard externally if needed
   License: MIT
   ============================================================================
 */
#ifndef VT_CONF_H
#define VT_CONF_H
#pragma once

/* -------------------------------------------------------------------------- */
/* Export / Allocators                                                        */
/* -------------------------------------------------------------------------- */
#ifndef CONF_API
#define CONF_API extern
#endif

#ifndef CONF_MALLOC
#include <stdlib.h>
#define CONF_MALLOC malloc
#define CONF_REALLOC realloc
#define CONF_FREE free
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Data Model                                                                 */
/* -------------------------------------------------------------------------- */
typedef struct {
  char* key;      /* canonical lowercase key, e.g. "db.host" */
  char* val;      /* raw value after basic unescape; expansion on demand */
  uint64_t h;     /* hash(key) for map */
  unsigned state; /* 0=empty,1=filled,2=tombstone */
} vt_conf_entry;

typedef struct {
  vt_conf_entry* tab;
  size_t cap; /* power of two */
  size_t len; /* number of filled entries */
  char err[256];
  char* basedir; /* for @include resolution */
} vt_conf;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */
CONF_API int vt_conf_init(vt_conf* c);
CONF_API void vt_conf_reset(vt_conf* c); /* remove all entries, keep capacity */
CONF_API void vt_conf_free(vt_conf* c);

/* Merge sources (later wins) */
CONF_API int vt_conf_load_file(
    vt_conf* c, const char* path); /* INI-like, supports @include "path" */
CONF_API int vt_conf_load_env(
    vt_conf* c, const char* prefix); /* PREFIX_FOO=, PREFIX_A__B= → "a.b" */
CONF_API int vt_conf_load_argv(vt_conf* c, int argc, char** argv);
/*  --k=v, --section.k=v, --no-flag (→ 0), --flag (→ 1) */

/* Set/Unset/Get */
CONF_API int vt_conf_set(vt_conf* c, const char* key,
                         const char* val); /* copy */
CONF_API int vt_conf_unset(vt_conf* c, const char* key);
CONF_API const char* vt_conf_get(const vt_conf* c,
                                 const char* key); /* raw string or NULL */

/* Typed access (with defaults). Support unit suffix for doubles: k,m,g,t
 * (×1000) or KiB/MiB... (×1024) */
CONF_API int vt_conf_get_bool(const vt_conf* c, const char* key, int def);
CONF_API int64_t vt_conf_get_i64(const vt_conf* c, const char* key,
                                 int64_t def);
CONF_API double vt_conf_get_f64(const vt_conf* c, const char* key, double def);
/* Copy string with fallback; returns bytes written (excluding NUL) */
CONF_API size_t vt_conf_get_str(const vt_conf* c, const char* key, char* out,
                                size_t outsz, const char* def);

/* Expansion (${KEY} from conf first then environment). Returns 0 on success. */
CONF_API int vt_conf_expand(const vt_conf* c, const char* in, char** out_heap);

/* Save / Iterate */
typedef void (*vt_conf_each_fn)(const char* key, const char* val, void* ud);
CONF_API void vt_conf_foreach(const vt_conf* c, vt_conf_each_fn cb, void* ud);
CONF_API int vt_conf_save_ini(const vt_conf* c, const char* path);

/* Validation / Debug */
CONF_API int vt_conf_require(const vt_conf* c, const char* const* keys,
                             size_t n, char* why, size_t why_sz);
CONF_API const char* vt_conf_last_error(const vt_conf* c);

/* -------------------------------------------------------------------------- */
/* Implementation                                                             */
/* -------------------------------------------------------------------------- */
#ifdef CONF_HEADER_ONLY
/* ======== Local Utilities ======== */
#if defined(__GNUC__) || defined(__clang__)
#define VT_CONF_INLINE static __inline__ __attribute__((always_inline))
#else
#define VT_CONF_INLINE static inline
#endif

VT_CONF_INLINE uint64_t vt__hash64(const void* data, size_t n) {
  const unsigned char* p = (const unsigned char*)data;
  uint64_t h = 1469598103934665603ull; /* FNV-1a */
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h ? h : 1469598103934665603ull;
}
VT_CONF_INLINE int vt__is_space(int c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}
VT_CONF_INLINE int vt__tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}
VT_CONF_INLINE char* vt__dup(const char* s) {
  if (!s) {
    char* z = (char*)CONF_MALLOC(1);
    if (z) z[0] = 0;
    return z;
  }
  size_t n = strlen(s);
  char* p = (char*)CONF_MALLOC(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}
VT_CONF_INLINE char* vt__dup_n(const char* s, size_t n) {
  char* p = (char*)CONF_MALLOC(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}
VT_CONF_INLINE void vt__set_err(vt_conf* c, const char* msg) {
  if (!c) return;
  strncpy(c->err, msg ? msg : "", sizeof(c->err) - 1);
  c->err[sizeof(c->err) - 1] = 0;
}
VT_CONF_INLINE void vt__key_canon(char* s) {
  /* Trim spaces around separators, collapse spaces, to lowercase */
  /* Also convert runs of whitespace around '=' handled earlier; here only
   * lowercase */
  for (char* p = s; *p; ++p) *p = (char)vt__tolower((unsigned char)*p);
}
VT_CONF_INLINE char* vt__join_keys(const char* sec, const char* k) {
  if (!sec || !*sec) return vt__dup(k ? k : "");
  size_t a = strlen(sec), b = k ? strlen(k) : 0;
  char* out = (char*)CONF_MALLOC(a + 1 + b + 1);
  if (!out) return NULL;
  memcpy(out, sec, a);
  out[a] = '.';
  if (k) memcpy(out + a + 1, k, b);
  out[a + 1 + b] = 0;
  return out;
}
VT_CONF_INLINE uint64_t vt__hash_key(const char* k) {
  return vt__hash64(k, strlen(k));
}

VT_CONF_INLINE size_t vt__next_pow2(size_t x) {
  size_t p = 1;
  while (p < (x < 8 ? 8 : x)) p <<= 1;
  return p;
}
VT_CONF_INLINE int vt__eq_key(const char* a, const char* b) {
  return strcmp(a, b) == 0;
}

/* ======== Map Core ======== */
static int vt__map_grow(vt_conf* c, size_t need) {
  size_t newcap = vt__next_pow2(need * 2);
  vt_conf_entry* nt =
      (vt_conf_entry*)CONF_MALLOC(newcap * sizeof(vt_conf_entry));
  if (!nt) {
    vt__set_err(c, "oom");
    return -1;
  }
  for (size_t i = 0; i < newcap; i++) {
    nt[i].state = 0;
    nt[i].key = NULL;
    nt[i].val = NULL;
    nt[i].h = 0;
  }

  /* reinsert */
  for (size_t i = 0; i < c->cap; i++) {
    vt_conf_entry e = c->tab[i];
    if (e.state != 1) continue;
    size_t m = newcap - 1;
    size_t pos = (size_t)e.h & m;
    while (nt[pos].state == 1) {
      pos = (pos + 1) & m;
    }
    nt[pos] = e;
  }
  CONF_FREE(c->tab);
  c->tab = nt;
  c->cap = newcap;
  return 0;
}
static vt_conf_entry* vt__map_lookup(vt_conf* c, const char* key, uint64_t h,
                                     int for_insert) {
  if (c->cap == 0) {
    c->cap = 16;
    c->tab = (vt_conf_entry*)CONF_MALLOC(c->cap * sizeof(vt_conf_entry));
    if (!c->tab) {
      vt__set_err(c, "oom");
      return NULL;
    }
    for (size_t i = 0; i < c->cap; i++) {
      c->tab[i].state = 0;
      c->tab[i].key = NULL;
      c->tab[i].val = NULL;
      c->tab[i].h = 0;
    }
  }
  size_t mask = c->cap - 1;
  size_t pos = (size_t)h & mask;
  vt_conf_entry* first_tomb = NULL;
  for (;;) {
    vt_conf_entry* e = &c->tab[pos];
    if (e->state == 0) { /* empty */
      return (for_insert && first_tomb) ? first_tomb : e;
    }
    if (e->state == 2) { /* tombstone */
      if (for_insert && !first_tomb) first_tomb = e;
    } else if (e->h == h && vt__eq_key(e->key, key)) {
      return e;
    }
    pos = (pos + 1) & mask;
  }
}
static int vt__map_set(vt_conf* c, const char* key, const char* val) {
  if ((c->len + 1) * 2 >= c->cap) {
    if (vt__map_grow(c, c->len + 1)) return -1;
  }
  uint64_t h = vt__hash_key(key);
  vt_conf_entry* e = vt__map_lookup(c, key, h, 1);
  if (!e) {
    vt__set_err(c, "lookup failed");
    return -1;
  }
  if (e->state == 1) {
    /* replace */
    char* nv = vt__dup(val ? val : "");
    if (!nv) {
      vt__set_err(c, "oom");
      return -1;
    }
    CONF_FREE(e->val);
    e->val = nv;
    return 0;
  }
  /* insert */
  e->key = vt__dup(key);
  e->val = vt__dup(val ? val : "");
  if (!e->key || !e->val) {
    CONF_FREE(e->key);
    CONF_FREE(e->val);
    e->key = NULL;
    e->val = NULL;
    vt__set_err(c, "oom");
    return -1;
  }
  e->h = h;
  e->state = 1;
  c->len++;
  return 0;
}

/* ======== Public impl ======== */
int vt_conf_init(vt_conf* c) {
  if (!c) return -1;
  memset(c, 0, sizeof(*c));
  c->cap = 0;
  c->tab = NULL;
  c->len = 0;
  c->err[0] = 0;
  c->basedir = NULL;
  return 0;
}
void vt_conf_reset(vt_conf* c) {
  if (!c) return;
  for (size_t i = 0; i < c->cap; i++) {
    if (c->tab[i].state == 1) {
      CONF_FREE(c->tab[i].key);
      CONF_FREE(c->tab[i].val);
    }
    c->tab[i].key = NULL;
    c->tab[i].val = NULL;
    c->tab[i].state = 0;
  }
  c->len = 0;
}
void vt_conf_free(vt_conf* c) {
  if (!c) return;
  vt_conf_reset(c);
  CONF_FREE(c->tab);
  c->tab = NULL;
  c->cap = 0;
  CONF_FREE(c->basedir);
  c->basedir = NULL;
}

int vt_conf_set(vt_conf* c, const char* key, const char* val) {
  if (!c || !key) {
    errno = EINVAL;
    return -1;
  }
  char* kdup = vt__dup(key);
  if (!kdup) {
    vt__set_err(c, "oom");
    return -1;
  }
  vt__key_canon(kdup);
  int rc = vt__map_set(c, kdup, val ? val : "");
  CONF_FREE(kdup);
  return rc;
}
int vt_conf_unset(vt_conf* c, const char* key) {
  if (!c || !key) return -1;
  char* k = vt__dup(key);
  if (!k) return -1;
  vt__key_canon(k);
  uint64_t h = vt__hash_key(k);
  vt_conf_entry* e = vt__map_lookup(c, k, h, 0);
  if (e && e->state == 1) {
    CONF_FREE(e->key);
    CONF_FREE(e->val);
    e->key = NULL;
    e->val = NULL;
    e->state = 2;
    c->len--;
  }
  CONF_FREE(k);
  return 0;
}
const char* vt_conf_get(const vt_conf* c, const char* key) {
  if (!c || !key) return NULL;
  char* k = vt__dup(key);
  if (!k) return NULL;
  vt__key_canon(k);
  uint64_t h = vt__hash_key(k);
  /* cast away const for lookup helper */
  vt_conf_entry* e = vt__map_lookup((vt_conf*)c, k, h, 0);
  const char* out = (e && e->state == 1) ? e->val : NULL;
  CONF_FREE(k);
  return out;
}

/* ---- typed parsing ---- */
static int vt__parse_bool(const char* s, int* out) {
  if (!s) {
    *out = 0;
    return 0;
  }
  /* trim */
  while (vt__is_space((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && vt__is_space((unsigned char)s[n - 1])) n--;
  if (n == 0) {
    *out = 0;
    return 0;
  }
  /* case-insensitive */
  char b0 = (char)vt__tolower((unsigned char)s[0]);
  if ((n == 1 && (b0 == '1' || b0 == 'y')) ||
      (n == 2 && (b0 == 'o' && (vt__tolower(s[1]) == 'n'))) ||
      (n == 3 && b0 == 'y' && vt__tolower(s[1]) == 'e' &&
       vt__tolower(s[2]) == 's') ||
      (n == 4 && !strncmp("true", s, 4) && (vt__tolower(s[0]) == 't'))) {
    *out = 1;
    return 0;
  }
  if ((n == 1 && (b0 == '0' || b0 == 'n')) ||
      (n == 3 && !strncmp("off", s, 3)) ||
      (n == 5 && !strncmp("false", s, 5))) {
    *out = 0;
    return 0;
  }
  /* fallback: non-empty numeric? */
  char* end = NULL;
  long v = strtol(s, &end, 10);
  if (end && end > s) {
    *out = (v != 0);
    return 0;
  }
  return -1;
}
static int64_t vt__parse_i64(const char* s, int* ok) {
  if (!s) {
    if (ok) *ok = 0;
    return 0;
  }
  /* skip spaces, allow 0x / 0b / underscores */
  int neg = 0;
  const char* p = s;
  while (vt__is_space((unsigned char)*p)) p++;
  if (*p == '+' || *p == '-') {
    neg = (*p == '-');
    p++;
  }
  int base = 10;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
  } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
    base = 2;
    p += 2;
  }
  int64_t v = 0;
  for (; *p; ++p) {
    if (*p == '_') continue;
    int d;
    if (*p >= '0' && *p <= '9')
      d = *p - '0';
    else if (*p >= 'a' && *p <= 'f')
      d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F')
      d = *p - 'A' + 10;
    else if (vt__is_space((unsigned char)*p))
      break;
    else {
      if (ok) *ok = 0;
      return 0;
    }
    if (d >= base) {
      if (ok) *ok = 0;
      return 0;
    }
    v = v * base + d;
  }
  if (ok) *ok = 1;
  return neg ? -v : v;
}
static double vt__parse_f64(const char* s, int* ok) {
  if (!s) {
    if (ok) *ok = 0;
    return 0.0;
  }
  char* end = NULL;
#if defined(_MSC_VER)
  double v = strtod(s, &end);
#else
  double v = strtod(s, &end);
#endif
  if (!end || end == s) {
    if (ok) *ok = 0;
    return 0.0;
  }
  /* optional unit: k,m,g,t or Ki/Mi/Gi/Ti (decimal vs binary) */
  double mul = 1.0;
  if (*end) {
    if ((end[0] == 'k' || end[0] == 'K') && !end[1])
      mul = 1000.0;
    else if ((end[0] == 'm' || end[0] == 'M') && !end[1])
      mul = 1000.0 * 1000.0;
    else if ((end[0] == 'g' || end[0] == 'G') && !end[1])
      mul = 1e9;
    else if ((end[0] == 't' || end[0] == 'T') && !end[1])
      mul = 1e12;
    else if ((end[0] == 'K' || end[0] == 'k') &&
             (end[1] == 'i' || end[1] == 'I') && !end[2])
      mul = 1024.0;
    else if ((end[0] == 'M' || end[0] == 'm') &&
             (end[1] == 'i' || end[1] == 'I') && !end[2])
      mul = 1024.0 * 1024.0;
    else if ((end[0] == 'G' || end[0] == 'g') &&
             (end[1] == 'i' || end[1] == 'I') && !end[2])
      mul = 1024.0 * 1024.0 * 1024.0;
    else if ((end[0] == 'T' || end[0] == 't') &&
             (end[1] == 'i' || end[1] == 'I') && !end[2])
      mul = 1024.0 * 1024.0 * 1024.0 * 1024.0;
  }
  if (ok) *ok = 1;
  return v * mul;
}

int vt_conf_get_bool(const vt_conf* c, const char* key, int def) {
  const char* s = vt_conf_get(c, key);
  int v;
  return (s && vt__parse_bool(s, &v) == 0) ? v : def;
}
int64_t vt_conf_get_i64(const vt_conf* c, const char* key, int64_t def) {
  const char* s = vt_conf_get(c, key);
  int ok = 0;
  int64_t v = vt__parse_i64(s, &ok);
  return ok ? v : def;
}
double vt_conf_get_f64(const vt_conf* c, const char* key, double def) {
  const char* s = vt_conf_get(c, key);
  int ok = 0;
  double v = vt__parse_f64(s, &ok);
  return ok ? v : def;
}
size_t vt_conf_get_str(const vt_conf* c, const char* key, char* out,
                       size_t outsz, const char* def) {
  const char* s = vt_conf_get(c, key);
  const char* src = s ? s : (def ? def : "");
  size_t n = strlen(src);
  if (out && outsz) {
    size_t w = (n < outsz - 1) ? n : (outsz - 1);
    memcpy(out, src, w);
    out[w] = 0;
  }
  return n;
}

const char* vt_conf_last_error(const vt_conf* c) {
  return (c && c->err[0]) ? c->err : NULL;
}

/* ---- env & argv ---- */
#if defined(_WIN32)
#include <windows.h>
#endif

int vt_conf_load_env(vt_conf* c, const char* prefix) {
  if (!c) return -1;
  size_t lp = prefix ? strlen(prefix) : 0;

#if defined(_WIN32)
  LPCH env = GetEnvironmentStringsA();
  if (!env) {
    vt__set_err(c, "GetEnvironmentStringsA failed");
    return -1;
  }
  for (LPCH p = env; *p;) {
    size_t n = strlen(p);
    const char* kv = p;
    p += n + 1;
    const char* eq = strchr(kv, '=');
    if (!eq) continue;
    size_t klen = (size_t)(eq - kv);
    if (lp && (klen < lp || _strnicmp(kv, prefix, (unsigned)lp) != 0)) continue;
    /* strip prefix */
    const char* k0 = kv + lp;
    /* build canonical key: double '_' -> '.', single '_' -> '_' (keep) */
    char* k = (char*)CONF_MALLOC(klen - lp + 1);
    if (!k) {
      FreeEnvironmentStringsA(env);
      vt__set_err(c, "oom");
      return -1;
    }
    size_t wi = 0;
    for (size_t i = 0; i < klen - lp; i++) {
      char ch = k0[i];
      if (ch == '_' && i + 1 < klen - lp && k0[i + 1] == '_') {
        k[wi++] = '.';
        i++;
      } else
        k[wi++] = (char)vt__tolower((unsigned char)ch);
    }
    k[wi] = 0;
    vt__map_set(c, k, eq + 1);
    CONF_FREE(k);
  }
  FreeEnvironmentStringsA(env);
#else
  extern char** environ;
  if (!environ) return 0;
  for (char** p = environ; *p; ++p) {
    const char* kv = *p;
    const char* eq = strchr(kv, '=');
    if (!eq) continue;
    size_t klen = (size_t)(eq - kv);
    if (lp && (klen < lp || strncasecmp(kv, prefix, lp) != 0)) continue;
    const char* k0 = kv + lp;
    char* k = (char*)CONF_MALLOC(klen - lp + 1);
    if (!k) {
      vt__set_err(c, "oom");
      return -1;
    }
    size_t wi = 0;
    for (size_t i = 0; i < klen - lp; i++) {
      char ch = k0[i];
      if (ch == '_' && i + 1 < klen - lp && k0[i + 1] == '_') {
        k[wi++] = '.';
        i++;
      } else
        k[wi++] = (char)vt__tolower((unsigned char)ch);
    }
    k[wi] = 0;
    vt__map_set(c, k, eq + 1);
    CONF_FREE(k);
  }
#endif
  return 0;
}

int vt_conf_load_argv(vt_conf* c, int argc, char** argv) {
  if (!c) return -1;
  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strncmp(a, "--", 2) != 0) continue;
    a += 2;
    if (strncmp(a, "no-", 3) == 0) {
      const char* key = a + 3;
      char* k = vt__dup(key);
      if (!k) {
        vt__set_err(c, "oom");
        return -1;
      }
      vt__key_canon(k);
      int rc = vt__map_set(c, k, "0");
      CONF_FREE(k);
      if (rc) return rc;
      continue;
    }
    const char* eq = strchr(a, '=');
    if (eq) {
      char* k = vt__dup_n(a, (size_t)(eq - a));
      char* v = vt__dup(eq + 1);
      if (!k || !v) {
        CONF_FREE(k);
        CONF_FREE(v);
        vt__set_err(c, "oom");
        return -1;
      }
      vt__key_canon(k);
      int rc = vt__map_set(c, k, v);
      CONF_FREE(k);
      CONF_FREE(v);
      if (rc) return rc;
    } else {
      char* k = vt__dup(a);
      if (!k) {
        vt__set_err(c, "oom");
        return -1;
      }
      vt__key_canon(k);
      int rc = vt__map_set(c, k, "1");
      CONF_FREE(k);
      if (rc) return rc;
    }
  }
  return 0;
}

/* ---- file parser (INI-like) ---- */
static char* vt__dirname_dup(const char* path) {
  if (!path) return NULL;
  const char* slash = strrchr(path, '/');
#if defined(_WIN32)
  const char* bslash = strrchr(path, '\\');
  if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
  if (!slash) return vt__dup("");
  size_t n = (size_t)(slash - path);
  return vt__dup_n(path, n);
}
static void vt__trim_inplace(char* s) {
  size_t n = strlen(s);
  size_t i = 0;
  while (i < n && vt__is_space((unsigned char)s[i])) i++;
  size_t j = n;
  while (j > i && vt__is_space((unsigned char)s[j - 1])) j--;
  if (i > 0) memmove(s, s + i, j - i);
  s[j - i] = 0;
}
static void vt__unquote(char* s) {
  size_t n = strlen(s);
  if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                 (s[0] == '\'' && s[n - 1] == '\''))) {
    /* simple unescape for \" \n \t \\ */
    char q = s[0];
    char* w = s;
    const char* r = s + 1;
    const char* end = s + n - 1;
    while (r < end) {
      char ch = *r++;
      if (ch == '\\' && r < end) {
        char esc = *r++;
        if (esc == 'n')
          ch = '\n';
        else if (esc == 't')
          ch = '\t';
        else if (esc == 'r')
          ch = '\r';
        else
          ch = esc;
      }
      *w++ = ch;
    }
    *w = 0;
  }
}
int vt_conf_load_file(vt_conf* c, const char* path) {
  if (!c || !path) return -1;
  FILE* f = fopen(path, "rb");
  if (!f) {
    snprintf(c->err, sizeof(c->err), "open %s: %s", path, strerror(errno));
    return -1;
  }
  /* read all */
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    vt__set_err(c, "fseek");
    return -1;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    vt__set_err(c, "ftell");
    return -1;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    vt__set_err(c, "fseek");
    return -1;
  }
  char* buf = (char*)CONF_MALLOC((size_t)n + 1);
  if (!buf) {
    fclose(f);
    vt__set_err(c, "oom");
    return -1;
  }
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (rd != (size_t)n) {
    CONF_FREE(buf);
    vt__set_err(c, "fread");
    return -1;
  }
  buf[n] = 0;

  /* base dir for @include */
  CONF_FREE(c->basedir);
  c->basedir = vt__dirname_dup(path);

  char* s = buf;
  /* strip UTF-8 BOM if present */
  if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
      (unsigned char)s[2] == 0xBF)
    s += 3;

  char section[256] = {0};
  size_t line_no = 0;
  while (*s) {
    char* line = s;
    /* find end of line */
    while (*s && *s != '\n' && *s != '\r') s++;
    char sep = *s;
    if (sep == '\r' && s[1] == '\n') {
      *s = 0;
      s += 2;
    } else if (sep) {
      *s = 0;
      s++;
    }
    line_no++;
    /* strip comments (# ; or //) — only if not inside quotes */
    char* p = line;
    int inq = 0;
    char qch = 0;
    for (char* t = p; *t; ++t) {
      if (!inq && (*t == '\'' || *t == '"')) {
        inq = 1;
        qch = *t;
        continue;
      } else if (inq && *t == qch) {
        inq = 0;
        continue;
      }
      if (!inq) {
        if (*t == '#' || *t == ';') {
          *t = 0;
          break;
        }
        if (*t == '/' && t[1] == '/') {
          *t = 0;
          break;
        }
      }
    }
    vt__trim_inplace(line);
    if (line[0] == 0) continue;

    if (line[0] == '[') {
      char* r = strrchr(line, ']');
      if (!r) {
        snprintf(c->err, sizeof(c->err), "%s:%zu: unmatched '['", path,
                 line_no);
        CONF_FREE(buf);
        return -1;
      }
      *r = 0;
      strncpy(section, line + 1, sizeof(section) - 1);
      section[sizeof(section) - 1] = 0;
      vt__trim_inplace(section);
      /* normalize */
      for (char* x = section; *x; ++x)
        *x = (char)vt__tolower((unsigned char)*x);
      continue;
    }

    if (strncmp(line, "@include", 8) == 0) {
      const char* q = line + 8;
      while (vt__is_space((unsigned char)*q)) q++;
      if (*q == '"' || *q == '\'') {
        char* inc = vt__dup(q);
        if (!inc) {
          vt__set_err(c, "oom");
          CONF_FREE(buf);
          return -1;
        }
        vt__unquote(inc);
        /* join with basedir if relative */
        char full[1024];
        if (c->basedir && inc[0] &&
            !(inc[0] == '/' || inc[0] == '\\' ||
              (strlen(inc) > 1 && inc[1] == ':'))) {
#if defined(_WIN32)
          snprintf(full, sizeof(full), "%s\\%s", c->basedir, inc);
#else
          snprintf(full, sizeof(full), "%s/%s", c->basedir, inc);
#endif
        } else {
          snprintf(full, sizeof(full), "%s", inc);
        }
        CONF_FREE(inc);
        if (vt_conf_load_file(c, full) != 0) {
          CONF_FREE(buf);
          return -1;
        } /* bubble up error text */
        continue;
      }
    }

    char* eq = strchr(line, '=');
    if (!eq) {
      snprintf(c->err, sizeof(c->err), "%s:%zu: expected key=value", path,
               line_no);
      CONF_FREE(buf);
      return -1;
    }
    *eq = 0;
    char* k = line;
    char* v = eq + 1;
    vt__trim_inplace(k);
    vt__trim_inplace(v);
    vt__unquote(v);
    /* canonical key: section + '.' + key (lowercase) */
    char* full = vt__join_keys(section, k);
    if (!full) {
      vt__set_err(c, "oom");
      CONF_FREE(buf);
      return -1;
    }
    vt__key_canon(full);
    if (vt__map_set(c, full, v) != 0) {
      CONF_FREE(full);
      CONF_FREE(buf);
      return -1;
    }
    CONF_FREE(full);
  }

  CONF_FREE(buf);
  return 0;
}

/* ---- expansion ---- */
int vt_conf_expand(const vt_conf* c, const char* in, char** out_heap) {
  if (!in || !out_heap) return -1;
  size_t cap = strlen(in) + 64;
  char* out = (char*)CONF_MALLOC(cap);
  if (!out) return -1;
  size_t o = 0;
  int depth = 0;
  const char* s = in;
  while (*s) {
    if (*s == '$' && s[1] == '{') {
      s += 2;
      const char* start = s;
      while (*s && *s != '}') s++;
      if (*s != '}') {
        CONF_FREE(out);
        return -1;
      }
      size_t klen = (size_t)(s - start);
      char* k = vt__dup_n(start, klen);
      vt__key_canon(k);
      const char* rep = vt_conf_get(c, k);
      if (!rep) {
        rep = getenv(k);
      }
      if (!rep) rep = "";
      size_t rn = strlen(rep);
      if (o + rn + 1 > cap) {
        cap = (cap * 2) + rn + 16;
        char* tmp = (char*)CONF_REALLOC(out, cap);
        if (!tmp) {
          CONF_FREE(out);
          CONF_FREE(k);
          return -1;
        }
        out = tmp;
      }
      memcpy(out + o, rep, rn);
      o += rn;
      out[o] = 0;
      CONF_FREE(k);
      s++; /* skip '}' */
      if (++depth > 128) {
        CONF_FREE(out);
        return -1;
      } /* prevent deep recursion abuse */
    } else {
      if (o + 2 > cap) {
        cap *= 2;
        char* tmp = (char*)CONF_REALLOC(out, cap);
        if (!tmp) {
          CONF_FREE(out);
          return -1;
        }
        out = tmp;
      }
      out[o++] = *s++;
      out[o] = 0;
    }
  }
  *out_heap = out;
  return 0;
}

/* ---- foreach/save/require ---- */
void vt_conf_foreach(const vt_conf* c, vt_conf_each_fn cb, void* ud) {
  if (!c || !cb) return;
  for (size_t i = 0; i < c->cap; i++) {
    const vt_conf_entry* e = &c->tab[i];
    if (e->state == 1) cb(e->key, e->val, ud);
  }
}

static int vt__key_section(const char* k, char* sec, size_t secsz,
                           const char** leaf) {
  const char* dot = strchr(k, '.');
  if (!dot) {
    if (leaf) *leaf = k;
    if (sec && secsz) sec[0] = 0;
    return 0;
  }
  size_t n = (size_t)(dot - k);
  if (sec && secsz) {
    size_t w = (n < secsz - 1) ? n : secsz - 1;
    memcpy(sec, k, w);
    sec[w] = 0;
  }
  if (leaf) *leaf = dot + 1;
  return 0;
}

int vt_conf_save_ini(const vt_conf* c, const char* path) {
  if (!c || !path) return -1;
  /* collect into a simple array first to sort */
  size_t cnt = c->len, idx = 0;
  typedef struct {
    const char* k;
    const char* v;
  } pair;
  pair* arr = (pair*)CONF_MALLOC(cnt * sizeof(pair));
  if (!arr) return -1;
  for (size_t i = 0; i < c->cap; i++) {
    if (c->tab[i].state == 1) {
      arr[idx++] = (pair){c->tab[i].key, c->tab[i].val};
    }
  }

  /* sort lexicographically by key (simple insertion sort to avoid qsort
   * dependency issues) */
  for (size_t i = 1; i < idx; i++) {
    pair key = arr[i];
    size_t j = i;
    while (j > 0 && strcmp(arr[j - 1].k, key.k) > 0) {
      arr[j] = arr[j - 1];
      j--;
    }
    arr[j] = key;
  }

  FILE* f = fopen(path, "wb");
  if (!f) {
    CONF_FREE(arr);
    return -1;
  }

  char cursec[256] = "";
  for (size_t i = 0; i < idx; i++) {
    char sec[256];
    const char* leaf = NULL;
    vt__key_section(arr[i].k, sec, sizeof(sec), &leaf);
    if (sec[0] && strcmp(sec, cursec) != 0) {
      fprintf(f, "\n[%s]\n", sec);
      strncpy(cursec, sec, sizeof(cursec) - 1);
      cursec[sizeof(cursec) - 1] = 0;
    } else if (!sec[0] && cursec[0]) {
      cursec[0] = 0;
      fprintf(f, "\n");
    }
    /* quote value if contains whitespace or comment chars */
    const char* v = arr[i].v ? arr[i].v : "";
    int needq = 0;
    for (const char* p = v; *p; ++p) {
      if (vt__is_space((unsigned char)*p) || *p == '#' || *p == ';') {
        needq = 1;
        break;
      }
    }
    if (needq) {
      fputc('"', f);
      for (const char* p = v; *p; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        if (*p == '\n') {
          fputc('\\', f);
          fputc('n', f);
        } else if (*p == '\t') {
          fputc('\\', f);
          fputc('t', f);
        } else
          fputc(*p, f);
      }
      fputc('"', f);
      fprintf(f, "\n");
    } else {
      fprintf(f, "%s=%s\n", leaf ? leaf : arr[i].k, v);
    }
  }

  fclose(f);
  CONF_FREE(arr);
  return 0;
}

int vt_conf_require(const vt_conf* c, const char* const* keys, size_t n,
                    char* why, size_t why_sz) {
  if (!c || !keys) return -1;
  for (size_t i = 0; i < n; i++) {
    if (!keys[i]) continue;
    if (!vt_conf_get(c, keys[i])) {
      if (why && why_sz) {
        snprintf(why, why_sz, "missing required key: %s", keys[i]);
      }
      return -1;
    }
  }
  return 0;
}

#endif /* CONF_HEADER_ONLY */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_CONF_H */
