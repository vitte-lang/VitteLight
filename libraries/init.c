// vitte-light/core/init.c
// Runtime init/teardown pour VitteLight: contexte VM, options, plugins,
// chargement VLBC, exécution, et intégration baselib.
//
// Fournit une façade simple de haut niveau:
//   - VL_RuntimeOptions / VL_Runtime
//   - vl_rt_init(), vl_rt_free()
//   - vl_rt_set_trace(), vl_rt_register_stdlibs()
//   - vl_rt_load_vlbc_file(), vl_rt_load_vlbc_buffer()
//   - vl_rt_run(max_steps)
//   - vl_rt_load_plugins_from_env("VITTE_PLUGINS")
//
// Plugins (optionnel): définir WITH_DLIB et fournir dlib.h/dlib.c.
// Un plugin doit exposer: int vl_plugin_init(struct VL_Context* ctx);
// Retour non-zero => OK. 0 => échec.
//
// Build typique:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c core/init.c
//   # Linkez avec: core/vm.c core/undump.c core/zio.c core/tm.c ...
//   # Pour plugins: ajoutez -DWITH_DLIB et libraries/dlib.c + dlib.h
//
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "state.h"  // traces
#include "tm.h"     // horloge
#include "undump.h"
#include "vm.h"
#include "zio.h"  // I/O utilitaires

// Baselib facultative (déclarée dans libraries/baselib.c)
void vl_register_baselib(
    struct VL_Context *ctx);  // fwd decl; pas d'en-tête requis

#ifdef WITH_DLIB
#include "dlib.h"
#endif

#ifndef VL_ENV_TRACE
#define VL_ENV_TRACE "VITTE_TRACE"
#endif
#ifndef VL_ENV_PLUGINS
#define VL_ENV_PLUGINS "VITTE_PLUGINS"
#endif
#ifndef VL_ENV_MAXSTEPS
#define VL_ENV_MAXSTEPS "VITTE_MAX_STEPS"
#endif

// ───────────────────────── Structures ─────────────────────────
typedef struct VL_RuntimeOptions {
  uint32_t trace_mask;  // VL_TRACE_* bitmask
  uint64_t max_steps;   // 0=illimité
  int with_std;         // registre natives standard (print)
  int with_baselib;     // registre baselib (strings, I/O, temps)
  FILE *out;            // sortie de la VM (stdout si NULL)
  const char *plugins;  // liste séparée par ':' (';' sous Windows accepté)
} VL_RuntimeOptions;

typedef struct VL_Runtime {
  struct VL_Context *ctx;
  VL_Module mod;
  int has_mod;
  uint64_t max_steps;
} VL_Runtime;

// ───────────────────────── Utils ─────────────────────────
static uint32_t parse_trace_mask(const char *flags) {
  if (!flags) return 0;
  uint32_t m = 0;
  const char *p = flags;
  char tok[32];
  while (*p) {
    size_t k = 0;
    while (*p == ',' || *p == ' ' || *p == '\t') p++;
    while (*p && *p != ',' && *p != ';' && k < sizeof(tok) - 1) {
      tok[k++] = (char)tolower((unsigned char)*p++);
    }
    tok[k] = '\0';
    if (k == 0) break;
    if (strcmp(tok, "op") == 0)
      m |= VL_TRACE_OP;
    else if (strcmp(tok, "stack") == 0)
      m |= VL_TRACE_STACK;
    else if (strcmp(tok, "global") == 0)
      m |= VL_TRACE_GLOBAL;
    else if (strcmp(tok, "call") == 0)
      m |= VL_TRACE_CALL;
    else if (strcmp(tok, "all") == 0)
      m |= 0xFFFFFFFFu;
    if (*p == ',' || *p == ';') p++;
  }
  return m;
}

static uint64_t parse_u64(const char *s) {
  if (!s || !*s) return 0;
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 10);
  (void)end;
  return (uint64_t)v;
}

static void apply_env_overrides(VL_RuntimeOptions *opt) {
  const char *t = getenv(VL_ENV_TRACE);
  if (t && *t) opt->trace_mask |= parse_trace_mask(t);
  const char *ms = getenv(VL_ENV_MAXSTEPS);
  if (ms && *ms) opt->max_steps = parse_u64(ms);
  const char *pl = getenv(VL_ENV_PLUGINS);
  if (pl && *pl && !opt->plugins) opt->plugins = pl;
}

static void ctx_set_output(struct VL_Context *ctx, FILE *out) {
  (void)ctx;
  (void)out; /* VM courante écrit sur stdout via ctx->out dans vm.c si exposé */
}

// ───────────────────────── API ─────────────────────────
void vl_rt_default_options(VL_RuntimeOptions *o) {
  if (!o) return;
  memset(o, 0, sizeof(*o));
  o->with_std = 1;
  o->with_baselib = 1;
}

int vl_rt_init(VL_Runtime *rt, const VL_RuntimeOptions *opt_in) {
  if (!rt) return 0;
  memset(rt, 0, sizeof(*rt));
  VL_RuntimeOptions opt;
  if (opt_in)
    opt = *opt_in;
  else
    vl_rt_default_options(&opt);
  apply_env_overrides(&opt);
  rt->ctx = vl_ctx_new();
  if (!rt->ctx) return 0;
  if (opt.with_std) vl_ctx_register_std(rt->ctx);
  if (opt.with_baselib) vl_register_baselib(rt->ctx);
  if (opt.trace_mask) vl_trace_enable(rt->ctx, opt.trace_mask);
  if (opt.out) ctx_set_output(rt->ctx, opt.out);
  rt->max_steps = opt.max_steps;
  return 1;
}

void vl_rt_free(VL_Runtime *rt) {
  if (!rt) return;
  if (rt->has_mod) {
    vl_module_free(&rt->mod);
    rt->has_mod = 0;
  }
  if (rt->ctx) {
    vl_ctx_free(rt->ctx);
    rt->ctx = NULL;
  }
  memset(rt, 0, sizeof(*rt));
}

void vl_rt_set_trace(VL_Runtime *rt, uint32_t mask) {
  if (!rt || !rt->ctx) return;
  vl_trace_disable(rt->ctx, 0xFFFFFFFFu);
  if (mask) vl_trace_enable(rt->ctx, mask);
}

VL_Status vl_rt_attach(VL_Runtime *rt, const VL_Module *m) {
  if (!rt || !rt->ctx || !m) return VL_ERR_INVAL;
  return vl_ctx_attach_module(rt->ctx, m);
}

VL_Status vl_rt_load_vlbc_buffer(VL_Runtime *rt, const uint8_t *data, size_t n,
                                 char *err, size_t errn) {
  if (!rt || !data) return VL_ERR_INVAL;
  if (rt->has_mod) {
    vl_module_free(&rt->mod);
    rt->has_mod = 0;
  }
  VL_Status st = vl_module_from_buffer(data, n, &rt->mod, err, errn);
  if (st == VL_OK) {
    rt->has_mod = 1;
    st = vl_ctx_attach_module(rt->ctx, &rt->mod);
  }
  return st;
}

VL_Status vl_rt_load_vlbc_file(VL_Runtime *rt, const char *path, char *err,
                               size_t errn) {
  if (!rt || !path) return VL_ERR_INVAL;
  if (rt->has_mod) {
    vl_module_free(&rt->mod);
    rt->has_mod = 0;
  }
  VL_Status st = vl_module_from_file(path, &rt->mod, err, errn);
  if (st == VL_OK) {
    rt->has_mod = 1;
    st = vl_ctx_attach_module(rt->ctx, &rt->mod);
  }
  return st;
}

VL_Status vl_rt_run(VL_Runtime *rt, uint64_t max_steps) {
  if (!rt || !rt->ctx) return VL_ERR_INVAL;
  uint64_t lim = max_steps ? max_steps : rt->max_steps;
  return vl_run(rt->ctx, lim);
}

// ───────────────────────── Plugins (optionnel) ─────────────────────────
#ifdef WITH_DLIB
static int split_next(const char **ps, char *out, size_t n) {
  const char *s = *ps;
  if (!s || !*s) return 0;  // séparateurs: ':' ';'
  size_t k = 0;
  while (*s && *s != ':' && *s != ';') {
    if (k + 1 < n) out[k++] = *s;
    s++;
  }
  out[k] = '\0';
  if (*s == ':' || *s == ';') s++;
  *ps = s;
  return k > 0;
}

int vl_rt_load_plugin(VL_Runtime *rt, const char *lib) {
  if (!rt || !rt->ctx || !lib || !*lib) return 0;
  VL_DLib dl;
  vl_dlib_init(&dl);
  if (!vl_dlib_open_best(&dl, lib, 1)) {
    fprintf(stderr, "plugin open '%s': %s\n", lib, vl_dlib_error(&dl));
    vl_dlib_close(&dl);
    return 0;
  }
  typedef int (*initfn)(struct VL_Context *);
  initfn init = (initfn)vl_dlib_sym(&dl, "vl_plugin_init");
  if (!init) {
    fprintf(stderr, "plugin sym '%s': %s\n", lib, vl_dlib_error(&dl));
    vl_dlib_close(&dl);
    return 0;
  }
  int ok = init(rt->ctx);
  if (!ok) {
    fprintf(stderr, "plugin init failed: %s\n", lib);
  }
  vl_dlib_close(&dl);
  return ok != 0;
}

int vl_rt_load_plugins_from_env(VL_Runtime *rt, const char *env_name) {
  if (!rt) return 0;
  const char *v = getenv(env_name ? env_name : VL_ENV_PLUGINS);
  if (!v || !*v) return 1;
  char buf[512];
  int all_ok = 1;
  while (split_next(&v, buf, sizeof(buf))) {
    if (!vl_rt_load_plugin(rt, buf)) all_ok = 0;
  }
  return all_ok;
}
#else
int vl_rt_load_plugin(VL_Runtime *rt, const char *lib) {
  (void)rt;
  (void)lib;
  fprintf(stderr, "plugins: build sans WITH_DLIB\n");
  return 0;
}
int vl_rt_load_plugins_from_env(VL_Runtime *rt, const char *env_name) {
  (void)rt;
  (void)env_name;
  return 1;
}
#endif

// ───────────────────────── Démo main (optionnel) ─────────────────────────
#ifdef VL_INIT_TEST_MAIN
int main(int argc, char **argv) {
  VL_Runtime rt;
  VL_RuntimeOptions opt;
  vl_rt_default_options(&opt);
  opt.trace_mask = parse_trace_mask(getenv(VL_ENV_TRACE));
  if (!vl_rt_init(&rt, &opt)) {
    fprintf(stderr, "init failed\n");
    return 1;
  }
#ifdef WITH_DLIB
  vl_rt_load_plugins_from_env(&rt, NULL);
#endif
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file.vlbc>\n", argv[0]);
    vl_rt_free(&rt);
    return 2;
  }
  char err[256];
  VL_Status st = vl_rt_load_vlbc_file(&rt, argv[1], err, sizeof(err));
  if (st != VL_OK) {
    fprintf(stderr, "load: %s\n", err);
    vl_rt_free(&rt);
    return 1;
  }
  st = vl_rt_run(&rt, 0);
  if (st != VL_OK) {
    fprintf(stderr, "run: %d\n", st);
  }
  vl_rt_free(&rt);
  return st == VL_OK ? 0 : 1;
}
#endif
