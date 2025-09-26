// SPDX-License-Identifier: GPL-3.0-or-later
//
// init.c — Runtime bootstrap for Vitte Light (C17)
//
// Goals:
// - Centralize logging setup, RNG seed, and global subsystems (HTTP, etc.)
// - Open selected standard libraries on a VM state
// - Provide a single shutdown path
//
// Depends: includes/auxlib.h, state.h, vm.h, and the libraries you enable.
// Optional subsystems: libcurl (HTTP). All others are opt-in via flags.

#include "libctype.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "state.h"
#include "vm.h"

// ———————————————————————————————————————————————————————————————————————
// Forward decls for stdlibs actually present in the tree
// Add externs here only for libraries you compiled in.
// ———————————————————————————————————————————————————————————————————————
extern void vl_open_baselib(VL_State *S);
extern void vl_open_corolib(VL_State *S);

// If you have other libs, declare them here:
// extern void vl_open_iolib(VL_State *S);
// extern void vl_open_mathlib(VL_State *S);
// extern void vl_open_strlib(VL_State *S);
// extern void vl_open_oslib(VL_State *S);
// extern void vl_open_cryptolib(VL_State *S);
// extern void vl_open_curlib(VL_State *S);
// extern void vl_open_dblib(VL_State *S);
// extern void vl_open_dllib(VL_State *S);
// extern void vl_open_ffilib(VL_State *S);

// Optional HTTP (libcurl) global init (provided by libraries/curl.c)
AuxStatus vl_http_global_init(void);
void vl_http_global_cleanup(void);

// ———————————————————————————————————————————————————————————————————————
// Public configuration
// ———————————————————————————————————————————————————————————————————————

typedef struct VlStdLibs {
  unsigned base : 1;       // base library (print, type, assert, …)
  unsigned coroutine : 1;  // coroutine library
  // Extend flags as you add libs:
  unsigned io : 1;
  unsigned math : 1;
  unsigned str : 1;
  unsigned os : 1;
  unsigned crypto : 1;
  unsigned curl : 1;
  unsigned db : 1;
  unsigned dl : 1;
  unsigned ffi : 1;
} VlStdLibs;

typedef struct VlInitOptions {
  // Logging
  FILE *log_sink;         // default: stderr
  AuxLogLevel log_level;  // default: AUX_LOG_INFO
  int color_logs;         // default: auto TTY

  // Subsystems
  int init_http;      // default: 1 if libcurl linked, else 0
  int shutdown_http;  // default: same as init_http

  // Libraries to open
  VlStdLibs stdlib;  // default: base=1, coroutine=1

  // VM niceties
  const char *global_version;  // default: "Vitte Light 0.1"
} VlInitOptions;

// ———————————————————————————————————————————————————————————————————————
// Environment parsing helpers
// ———————————————————————————————————————————————————————————————————————

static AuxLogLevel parse_log_level_env(const char *s, AuxLogLevel defv) {
  if (!s || !*s) return defv;
  // Accept numeric or names
  if (isdigit((unsigned char)s[0])) {
    int v = atoi(s);
    if (v < 0) v = 0;
    if (v > AUX_LOG_FATAL) v = AUX_LOG_FATAL;
    return (AuxLogLevel)v;
  }
  // normalize lowercase
  char buf[16] = {0};
  size_t n = strlen(s);
  if (n >= sizeof buf) n = sizeof buf - 1;
  for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
  if (strcmp(buf, "trace") == 0) return AUX_LOG_TRACE;
  if (strcmp(buf, "debug") == 0) return AUX_LOG_DEBUG;
  if (strcmp(buf, "info") == 0) return AUX_LOG_INFO;
  if (strcmp(buf, "warn") == 0) return AUX_LOG_WARN;
  if (strcmp(buf, "error") == 0) return AUX_LOG_ERROR;
  if (strcmp(buf, "fatal") == 0) return AUX_LOG_FATAL;
  return defv;
}

static int parse_bool_env(const char *s, int defv) {
  if (!s || !*s) return defv;
  if (!strcasecmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") ||
      !strcasecmp(s, "on"))
    return 1;
  if (!strcasecmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "no") ||
      !strcasecmp(s, "off"))
    return 0;
  return defv;
}

// ———————————————————————————————————————————————————————————————————————
// Defaults
// ———————————————————————————————————————————————————————————————————————

static void vl_init_options_fill_defaults(VlInitOptions *opt) {
  memset(opt, 0, sizeof *opt);
  opt->log_sink = stderr;
  opt->log_level = AUX_LOG_INFO;
  opt->color_logs = 1;

  // Enable HTTP if available. We cannot detect link-time here, so leave 1.
  opt->init_http = 1;
  opt->shutdown_http = 1;

  opt->stdlib.base = 1;
  opt->stdlib.coroutine = 1;

  opt->global_version = "Vitte Light 0.1";
}

// ———————————————————————————————————————————————————————————————————————
// RNG seed (optional). VM may not need it; keep helper for future use.
// ———————————————————————————————————————————————————————————————————————

static uint64_t secure_seed_u64(void) {
  uint64_t s = 0;
  if (aux_rand_bytes(&s, sizeof s) == AUX_OK && s != 0) return s;
  // Fallback
  s ^= (uint64_t)aux_now_nanos();
  s ^= (uint64_t)(uintptr_t)&s;
  return s ? s : 0x9E3779B97F4A7C15ull;
}

// ———————————————————————————————————————————————————————————————————————
// Opening stdlibs
// ———————————————————————————————————————————————————————————————————————

static void vl_open_selected_stdlibs(VL_State *S, const VlStdLibs *f) {
  if (f->base) vl_open_baselib(S);
  if (f->coroutine) vl_open_corolib(S);

  // Plug additional libs when available:
  // if (f->io)     vl_open_iolib(S);
  // if (f->math)   vl_open_mathlib(S);
  // if (f->str)    vl_open_strlib(S);
  // if (f->os)     vl_open_oslib(S);
  // if (f->crypto) vl_open_cryptolib(S);
  // if (f->curl)   vl_open_curlib(S);
  // if (f->db)     vl_open_dblib(S);
  // if (f->dl)     vl_open_dllib(S);
  // if (f->ffi)    vl_open_ffilib(S);
}

// ———————————————————————————————————————————————————————————————————————
// Public API
// ———————————————————————————————————————————————————————————————————————

typedef struct VlRuntime {
  int http_inited;
} VlRuntime;

static VlRuntime g_rt = {0};

void vl_runtime_fill_defaults(VlInitOptions *opt) {
  if (!opt) return;
  vl_init_options_fill_defaults(opt);
}

AuxStatus vl_runtime_init(VL_State *S, const VlInitOptions *user_opt) {
  if (!S) return AUX_EINVAL;

  VlInitOptions opt;
  if (user_opt)
    opt = *user_opt;
  else
    vl_init_options_fill_defaults(&opt);

  // Env overrides
  const char *env_log = aux_getenv("VITTL_LOG");
  const char *env_nocolor = aux_getenv("NO_COLOR");
  const char *env_color = aux_getenv("VITTL_COLOR");
  const char *env_http = aux_getenv("VITTL_HTTP");
  if (env_log) opt.log_level = parse_log_level_env(env_log, opt.log_level);
  if (env_color) opt.color_logs = parse_bool_env(env_color, opt.color_logs);
  if (env_nocolor) opt.color_logs = 0;
  if (env_http) {
    int on = parse_bool_env(env_http, opt.init_http);
    opt.init_http = on;
    opt.shutdown_http = on;
  }

  // Logging
  aux_log_init(opt.log_sink ? opt.log_sink : stderr, opt.log_level,
               opt.color_logs);

  AUX_LOG_DEBUG("vl_runtime_init: log_level=%d color=%d http=%d",
                (int)opt.log_level, opt.color_logs, opt.init_http);

  // Subsystems
  if (opt.init_http) {
    AuxStatus s = vl_http_global_init();
    if (s == AUX_OK) {
      g_rt.http_inited = 1;
    } else {
      AUX_LOG_WARN("HTTP global init failed (libcurl missing?): %s",
                   aux_status_str(s));
    }
  }

  // VM bootstrap niceties
  if (opt.global_version && *opt.global_version) {
    vl_push_string(S, opt.global_version);
    vl_setglobal(S, "_VERSION");
  }

  // Optional: provide a random seed global
  {
    char seedbuf[32];
    uint64_t seed = secure_seed_u64();
    snprintf(seedbuf, sizeof seedbuf, "0x%016" PRIx64, seed);
    vl_push_string(S, seedbuf);
    vl_setglobal(S, "_RANDOM_SEED");
  }

  // Open stdlibs
  vl_open_selected_stdlibs(S, &opt.stdlib);

  return AUX_OK;
}

void vl_runtime_shutdown(void) {
  if (g_rt.http_inited) {
    vl_http_global_cleanup();
    g_rt.http_inited = 0;
  }
  aux_shutdown_logging();
}

// ———————————————————————————————————————————————————————————————————————
// Convenience: open all default stdlibs on a state
// ———————————————————————————————————————————————————————————————————————

AuxStatus vl_open_all_stdlibs(VL_State *S) {
  if (!S) return AUX_EINVAL;
  VlStdLibs f = {0};
  f.base = 1;
  f.coroutine = 1;
  vl_open_selected_stdlibs(S, &f);
  return AUX_OK;
}

// ———————————————————————————————————————————————————————————————————————
// Optional CLI integration helpers (no I/O aside from logging)
// ———————————————————————————————————————————————————————————————————————

const char *vl_runtime_build_banner(void) {
  // Static string is fine; extend with git hash if you generate it
  return "Vitte Light Runtime — C17 — GPL-3.0-or-later";
}
