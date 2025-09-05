/* vitlc.c — Vitte Light Compiler Driver (C17)
   SPDX-License-Identifier: MIT

   Rôle:
     - Parse CLI et fichiers @response
     - Pipeline: lex → parse → sema → IR → bytecode (BC)
     - Sorties: --emit={bc,obj,asm,ir,ast,tokens,pp} ; -c ; -S ; -E
     - -o, -O0..3, -g, -Wall, -Werror, --color, --json-diagnostics
     - Dépendances: -MMD [-MF file] [-MT target]
     - Chrono des passes: --time-passes
     - Lecture stdin avec "-"
     - Écriture atomique + mkdir -p implicite du dossier de sortie
     - Toolchain externe optionnelle: --cc, --ld, --as, --ar, --sysroot, --target
     - Inclut/Libs: -I, -L, -l
     - Mode multi-fichiers: compile et link en une commande si pas -c/-S/-E

   Intégrations cœur (déclarées extern, à fournir par core/):
     Diagnostics:
       typedef struct VL_DiagSink VL_DiagSink;
       VL_DiagSink* vl_diag_create(FILE* err, int use_color, int json);
       void vl_diag_destroy(VL_DiagSink*);
       void vl_diag_error(VL_DiagSink*, const char* file, int line, int col,
                          const char* code, const char* fmt, ...);
       void vl_diag_warn (VL_DiagSink*, const char* file, int line, int col,
                          const char* code, const char* fmt, ...);

     Lexing/Parsing/IR/BC:
       typedef struct VL_Tokens   VL_Tokens;
       typedef struct VL_Ast      VL_Ast;
       typedef struct VL_Module   VL_Module;   // après Sema
       typedef struct VL_Ir       VL_Ir;
       typedef struct VL_Bytecode VL_Bytecode;

       int  vl_lex_source (const char* path_or_null_for_stdin,
                           const char* src_or_null,
                           VL_Tokens** out_tokens, VL_DiagSink* d);
       void vl_tokens_free(VL_Tokens*);

       int  vl_parse      (const VL_Tokens*, VL_Ast** out_ast, VL_DiagSink* d);
       void vl_ast_free   (VL_Ast*);

       int  vl_sema       (const VL_Ast*, VL_Module** out_mod, VL_DiagSink* d);
       void vl_module_free(VL_Module*);

       int  vl_ir_build   (const VL_Module*, VL_Ir** out_ir, VL_DiagSink* d);
       void vl_ir_free    (VL_Ir*);

       int  vl_bc_emit    (const VL_Ir*, VL_Bytecode** out_bc, VL_DiagSink* d,
                           int opt_level, int compress /*0=none,1=zstd?*/);
void vl_bc_free(VL_Bytecode*);

// Sérialisation:
int vl_bc_write_file(const VL_Bytecode*, const char* out_path, VL_DiagSink* d);
// Dumps lisibles:
int vl_dump_tokens(const VL_Tokens*, FILE* out);
int vl_dump_ast(const VL_Ast*, FILE* out, int pretty);
int vl_dump_ir(const VL_Ir*, FILE* out, int pretty);

Préprocesseur(optionnel, sinon stub interne pass - through)
    : int vl_preprocess_file(const char* in_path_or_null_for_stdin,
                             const char* include_paths[], size_t n_includes,
                             FILE* out, VL_DiagSink* d);

Construire : cc - std =
    c17 - O2 - Wall - Wextra - pedantic -
    o vitlc vitlc.c core /*.o ...
                          */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR_P(path) (_mkdir(path) == 0 || errno == EEXIST)
#define PATH_SEP '\\'
#else
#include <libgen.h>
#include <unistd.h>
#define MKDIR_P(path) (mkdir(path, 0777) == 0 || errno == EEXIST)
#define PATH_SEP '/'
#endif

    /* ========= Extern core API (voir en-tête ci-dessus) ========= */
    typedef struct VL_DiagSink VL_DiagSink;
typedef struct VL_Tokens VL_Tokens;
typedef struct VL_Ast VL_Ast;
typedef struct VL_Module VL_Module;
typedef struct VL_Ir VL_Ir;
typedef struct VL_Bytecode VL_Bytecode;

extern VL_DiagSink* vl_diag_create(FILE* err, int use_color, int json);
extern void vl_diag_destroy(VL_DiagSink*);
extern void vl_diag_error(VL_DiagSink*, const char*, int, int, const char*,
                          const char*, ...);
extern void vl_diag_warn(VL_DiagSink*, const char*, int, int, const char*,
                         const char*, ...);

extern int vl_preprocess_file(const char*, const char*[], size_t, FILE*,
                              VL_DiagSink*);

extern int vl_lex_source(const char*, const char*, VL_Tokens**, VL_DiagSink*);
extern void vl_tokens_free(VL_Tokens*);

extern int vl_parse(const VL_Tokens*, VL_Ast**, VL_DiagSink*);
extern void vl_ast_free(VL_Ast*);

extern int vl_sema(const VL_Ast*, VL_Module**, VL_DiagSink*);
extern void vl_module_free(VL_Module*);

extern int vl_ir_build(const VL_Module*, VL_Ir**, VL_DiagSink*);
extern void vl_ir_free(VL_Ir*);

extern int vl_bc_emit(const VL_Ir*, VL_Bytecode**, VL_DiagSink*, int, int);
extern void vl_bc_free(VL_Bytecode*);

extern int vl_bc_write_file(const VL_Bytecode*, const char*, VL_DiagSink*);

extern int vl_dump_tokens(const VL_Tokens*, FILE*);
extern int vl_dump_ast(const VL_Ast*, FILE*, int);
extern int vl_dump_ir(const VL_Ir*, FILE*, int);

/* ========= Utilitaires ========= */

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static char* xstrdup(const char* s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char* p = (char*)malloc(n);
  if (!p) {
    perror("malloc");
    exit(1);
  }
  memcpy(p, s, n);
  return p;
}

static void* xmalloc(size_t n) {
  void* p = malloc(n);
  if (!p) {
    perror("malloc");
    exit(1);
  }
  return p;
}

static char* path_dirname(const char* path) {
  if (!path) return NULL;
  char* tmp = xstrdup(path);
#if defined(_WIN32)
  // rudimentaire
  char* last = tmp + strlen(tmp);
  while (last > tmp && *last != '\\' && *last != '/') last--;
  if (last == tmp) {
    tmp[1] = '\0';
    return tmp;
  }
  *last = '\0';
  return tmp;
#else
  char* d = xstrdup(tmp);
  // basename/dirname posix modifient le buffer, on duplique
  char* dn = xstrdup(dirname(tmp));
  free(d);
  free(tmp);
  return dn;
#endif
}

static int mkdirs_for_file(const char* out_path) {
  if (!out_path) return 0;
  char* dir = path_dirname(out_path);
  if (!dir) return 0;
  // créer hiérarchie simple "a/b/c"
  char* p = dir;
  if (p[0] == PATH_SEP) p++;
  for (; *p; ++p) {
    if (*p == '/' || *p == '\\') {
      char saved = *p;
      *p = '\0';
      if (dir[0]) MKDIR_P(dir);
      *p = saved;
    }
  }
  if (dir[0]) MKDIR_P(dir);
  free(dir);
  return 0;
}

static int atomic_write_bytes(const void* data, size_t n, const char* path) {
  mkdirs_for_file(path);
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
  FILE* f = fopen(tmp, "wb");
  if (!f) {
    perror("fopen tmp");
    return -1;
  }
  if (n && fwrite(data, 1, n, f) != n) {
    perror("fwrite");
    fclose(f);
    remove(tmp);
    return -1;
  }
  if (fclose(f) != 0) {
    perror("fclose");
    remove(tmp);
    return -1;
  }
#if defined(_WIN32)
  remove(path);  // Windows remplace pas atomiquement toujours
#endif
  if (rename(tmp, path) != 0) {
    perror("rename");
    remove(tmp);
    return -1;
  }
  return 0;
}

/* ========= CLI ========= */

typedef enum {
  EMIT_AUTO = 0,
  EMIT_PP,
  EMIT_TOKENS,
  EMIT_AST,
  EMIT_IR,
  EMIT_BC,
  EMIT_ASM,
  EMIT_OBJ
} EmitKind;

typedef struct {
  char** items;
  size_t count, cap;
} StrVec;

static void sv_init(StrVec* v) {
  v->items = NULL;
  v->count = 0;
  v->cap = 0;
}
static void sv_push(StrVec* v, const char* s) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->items = (char**)realloc(v->items, v->cap * sizeof(char*));
    if (!v->items) {
      perror("realloc");
      exit(1);
    }
  }
  v->items[v->count++] = xstrdup(s);
}
static void sv_free(StrVec* v) {
  for (size_t i = 0; i < v->count; i++) free(v->items[i]);
  free(v->items);
}

typedef struct {
  // Frontend
  EmitKind emit;
  int opt_level;        // 0..3
  int compress;         // 0 none, 1 zstd
  int pretty;           // pretty-print dumps
  int color;            // 0|1|auto(2) — ici 0/1
  int json_diag;        // JSON diagnostics
  int werror;           // warnings as error
  int debug;            // -g
  int wall;             // -Wall equivalent
  int time_passes;      // --time-passes
  int preprocess_only;  // -E
  int compile_only;     // -c
  int assemble_only;    // -S
  int pic, pie;
  int sanitize_addr, sanitize_ub;

  const char* output;   // -o
  const char* target;   // --target
  const char* sysroot;  // --sysroot

  StrVec include_dirs;  // -I
  StrVec lib_dirs;      // -L
  StrVec libs;          // -l
  StrVec inputs;        // fichiers en entrée

  // Dépendances
  int gen_deps;            // -MMD
  const char* dep_file;    // -MF
  const char* dep_target;  // -MT

  // Toolchain externe
  const char* cc;       // --cc
  const char* ld;       // --ld
  const char* as_path;  // --as
  const char* ar_path;  // --ar
} Options;

static void options_init(Options* o) {
  memset(o, 0, sizeof(*o));
  o->emit = EMIT_AUTO;
  o->opt_level = 2;
  o->color = 1;
  sv_init(&o->include_dirs);
  sv_init(&o->lib_dirs);
  sv_init(&o->libs);
  sv_init(&o->inputs);
}

static void options_free(Options* o) {
  sv_free(&o->include_dirs);
  sv_free(&o->lib_dirs);
  sv_free(&o->libs);
  sv_free(&o->inputs);
}

static void usage(FILE* out) {
  fprintf(out,
          "Usage: vitlc [options] file1.vitl [file2.vitl ...]\n"
          "  -o <file>               Spécifie le fichier de sortie\n"
          "  -c / -S / -E            Compile-only / Assemble-only / "
          "Preprocess-only\n"
          "  --emit=<kind>           kind = pp|tokens|ast|ir|bc|asm|obj\n"
          "  -O0..-O3                Niveau d'optimisation\n"
          "  -g                      Infos debug\n"
          "  -Wall -Werror           Avertissements, erreurs fatales\n"
          "  --color[=0|1]           Couleurs diagnostics\n"
          "  --json-diagnostics      Diagnostics en JSON\n"
          "  --time-passes           Chronométrer les passes\n"
          "  -I <dir>                Inclure\n"
          "  -L <dir> -l <name>      Liens\n"
          "  --target <triple>       Triplet cible\n"
          "  --sysroot <path>        Sysroot\n"
          "  -MMD -MF <file> [-MT t] Générer dépendances Make\n"
          "  --cc/--ld/--as/--ar     Outils externes\n"
          "  --compress=zstd         Compression BC (si supportée)\n"
          "  @rspfile                Lire options depuis un fichier\n");
}

/* Expand @response files into argv vector */
static int is_space(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}
static void expand_response(StrVec* out, int argc, char** argv) {
  for (int i = 0; i < argc; i++) {
    const char* a = argv[i];
    if (a[0] == '@' && a[1]) {
      FILE* f = fopen(a + 1, "rb");
      if (!f) {
        perror("open @file");
        exit(1);
      }
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      char* buf = (char*)xmalloc((size_t)n + 1);
      fread(buf, 1, (size_t)n, f);
      buf[n] = 0;
      fclose(f);
      // simple tokenizer: whitespace separated, supports quotes
      char* p = buf;
      while (*p) {
        while (is_space(*p)) p++;
        if (!*p) break;
        char* start = p;
        int inq = 0;
        StrVec tmp;
        sv_init(&tmp);
        char tok[4096];
        size_t tlen = 0;
        while (*p) {
          if (*p == '"') {
            inq = !inq;
            p++;
            continue;
          }
          if (!inq && is_space(*p)) break;
          if (tlen + 1 < sizeof(tok)) tok[tlen++] = *p;
          p++;
        }
        tok[tlen] = '\0';
        sv_push(out, tok);
        if (*p) p++;
        sv_free(&tmp);
      }
      free(buf);
    } else {
      sv_push(out, a);
    }
  }
}

/* Parse CLI simple, sans getopt-long pour portabilité minimale */
static int starts_with(const char* s, const char* p) {
  return strncmp(s, p, strlen(p)) == 0;
}

static int parse_args(Options* o, int argc, char** argv) {
  StrVec v;
  sv_init(&v);
  expand_response(&v, argc, argv);
  int i = 1;
  while (i < (int)v.count) {
    const char* a = v.items[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      options_free(o);
      sv_free(&v);
      exit(0);
    } else if (strcmp(a, "-o") == 0 && i + 1 < (int)v.count) {
      o->output = v.items[++i];
    } else if (strcmp(a, "-c") == 0) {
      o->compile_only = 1;
    } else if (strcmp(a, "-S") == 0) {
      o->assemble_only = 1;
    } else if (strcmp(a, "-E") == 0) {
      o->preprocess_only = 1;
      o->emit = EMIT_PP;
    } else if (starts_with(a, "--emit=")) {
      const char* k = a + 7;
      if (strcmp(k, "pp") == 0)
        o->emit = EMIT_PP;
      else if (strcmp(k, "tokens") == 0)
        o->emit = EMIT_TOKENS;
      else if (strcmp(k, "ast") == 0)
        o->emit = EMIT_AST;
      else if (strcmp(k, "ir") == 0)
        o->emit = EMIT_IR;
      else if (strcmp(k, "bc") == 0)
        o->emit = EMIT_BC;
      else if (strcmp(k, "asm") == 0)
        o->emit = EMIT_ASM;
      else if (strcmp(k, "obj") == 0)
        o->emit = EMIT_OBJ;
      else {
        fprintf(stderr, "unknown --emit=%s\n", k);
        return -1;
      }
    } else if (strcmp(a, "-g") == 0) {
      o->debug = 1;
    } else if (strcmp(a, "-Wall") == 0) {
      o->wall = 1;
    } else if (strcmp(a, "-Werror") == 0) {
      o->werror = 1;
    } else if (starts_with(a, "-O")) {
      int lvl = a[2] ? atoi(a + 2) : 2;
      if (lvl < 0 || lvl > 3) {
        fprintf(stderr, "invalid %s\n", a);
        return -1;
      }
      o->opt_level = lvl;
    } else if (starts_with(a, "--color")) {
      const char* eq = strchr(a, '=');
      o->color = eq ? atoi(eq + 1) : 1;
    } else if (strcmp(a, "--json-diagnostics") == 0) {
      o->json_diag = 1;
    } else if (strcmp(a, "--time-passes") == 0) {
      o->time_passes = 1;
    } else if (strcmp(a, "-I") == 0 && i + 1 < (int)v.count) {
      sv_push(&o->include_dirs, v.items[++i]);
    } else if (strcmp(a, "-L") == 0 && i + 1 < (int)v.count) {
      sv_push(&o->lib_dirs, v.items[++i]);
    } else if (strcmp(a, "-l") == 0 && i + 1 < (int)v.count) {
      sv_push(&o->libs, v.items[++i]);
    } else if (strcmp(a, "--target") == 0 && i + 1 < (int)v.count) {
      o->target = v.items[++i];
    } else if (strcmp(a, "--sysroot") == 0 && i + 1 < (int)v.count) {
      o->sysroot = v.items[++i];
    } else if (strcmp(a, "-MMD") == 0) {
      o->gen_deps = 1;
    } else if (strcmp(a, "-MF") == 0 && i + 1 < (int)v.count) {
      o->dep_file = v.items[++i];
    } else if (strcmp(a, "-MT") == 0 && i + 1 < (int)v.count) {
      o->dep_target = v.items[++i];
    } else if (strcmp(a, "--cc") == 0 && i + 1 < (int)v.count) {
      o->cc = v.items[++i];
    } else if (strcmp(a, "--ld") == 0 && i + 1 < (int)v.count) {
      o->ld = v.items[++i];
    } else if (strcmp(a, "--as") == 0 && i + 1 < (int)v.count) {
      o->as_path = v.items[++i];
    } else if (strcmp(a, "--ar") == 0 && i + 1 < (int)v.count) {
      o->ar_path = v.items[++i];
    } else if (strcmp(a, "--compress=zstd") == 0) {
      o->compress = 1;
    } else if (a[0] == '-' && a[1]) {
      fprintf(stderr, "Unknown option: %s\n", a);
      sv_free(&v);
      return -1;
    } else {
      sv_push(&o->inputs, a);
    }
    i++;
  }
  if (o->inputs.count == 0) {
    fprintf(stderr, "No input files\n");
    sv_free(&v);
    return -1;
  }
  sv_free(&v);
  return 0;
}

/* ========= Dépendances (Make) ========= */

static int write_deps(const Options* o, const char* src, const char** deps,
                      size_t ndeps) {
  if (!o->gen_deps) return 0;
  const char* out = o->dep_file ? o->dep_file : "deps.d";
  const char* tgt =
      o->dep_target ? o->dep_target : (o->output ? o->output : "a.out");
  mkdirs_for_file(out);
  FILE* f = fopen(out, "wb");
  if (!f) {
    perror("open depfile");
    return -1;
  }
  fprintf(f, "%s:", tgt);
  // source est aussi une dépendance
  fprintf(f, " %s", src ? src : "-");
  for (size_t i = 0; i < ndeps; i++) fprintf(f, " %s", deps[i]);
  fputc('\n', f);
  fclose(f);
  return 0;
}

/* ========= Pipeline d’un fichier ========= */

typedef struct {
  VL_Tokens* toks;
  VL_Ast* ast;
  VL_Module* mod;
  VL_Ir* ir;
  VL_Bytecode* bc;
} Units;

static void units_clear(Units* u) {
  if (u->bc) {
    vl_bc_free(u->bc);
    u->bc = NULL;
  }
  if (u->ir) {
    vl_ir_free(u->ir);
    u->ir = NULL;
  }
  if (u->mod) {
    vl_module_free(u->mod);
    u->mod = NULL;
  }
  if (u->ast) {
    vl_ast_free(u->ast);
    u->ast = NULL;
  }
  if (u->toks) {
    vl_tokens_free(u->toks);
    u->toks = NULL;
  }
}

static int preprocess_to(FILE* out, const char* in_path, const Options* o,
                         VL_DiagSink* d) {
  if (vl_preprocess_file) {
    const char** inc = (const char**)o->include_dirs.items;
    return vl_preprocess_file(in_path, inc, o->include_dirs.count, out, d);
  }
  // fallback: passer la source telle quelle
  FILE* in = in_path && strcmp(in_path, "-") ? fopen(in_path, "rb") : stdin;
  if (!in) {
    perror("open input");
    return -1;
  }
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    if (fwrite(buf, 1, n, out) != n) {
      perror("write");
      return -1;
    }
  if (in != stdin) fclose(in);
  return 0;
}

static int compile_single(const Options* o, const char* input_path,
                          const char* out_path_or_null, VL_DiagSink* d) {
  Units u = {0};
  double t0 = now_seconds();
  int rc;

  /* Préprocess */
  if (o->emit == EMIT_PP || o->preprocess_only) {
    FILE* out = stdout;
    if (out_path_or_null && strcmp(out_path_or_null, "-") != 0) {
      mkdirs_for_file(out_path_or_null);
      out = fopen(out_path_or_null, "wb");
      if (!out) {
        perror("open out");
        return -1;
      }
    }
    rc = preprocess_to(out, input_path, o, d);
    if (out != stdout) fclose(out);
    if (o->time_passes)
      fprintf(stderr, "[time] preprocess: %.3fs\n", now_seconds() - t0);
    return rc;
  }

  /* Lex */
  double t_lex = now_seconds();
  rc = vl_lex_source(input_path, NULL, &u.toks, d);
  if (rc != 0) {
    units_clear(&u);
    return rc;
  }
  if (o->wall) { /* core fera déjà les warns contextuels */
  }
  if (o->emit == EMIT_TOKENS) {
    vl_dump_tokens(u.toks, stdout);
    units_clear(&u);
    if (o->time_passes)
      fprintf(stderr, "[time] lex: %.3fs\n", now_seconds() - t_lex);
    return 0;
  }
  if (o->time_passes)
    fprintf(stderr, "[time] lex: %.3fs\n", now_seconds() - t_lex);

  /* Parse */
  double t_parse = now_seconds();
  rc = vl_parse(u.toks, &u.ast, d);
  if (rc != 0) {
    units_clear(&u);
    return rc;
  }
  if (o->emit == EMIT_AST) {
    vl_dump_ast(u.ast, stdout, o->pretty);
    units_clear(&u);
    if (o->time_passes)
      fprintf(stderr, "[time] parse: %.3fs\n", now_seconds() - t_parse);
    return 0;
  }
  if (o->time_passes)
    fprintf(stderr, "[time] parse: %.3fs\n", now_seconds() - t_parse);

  /* Sema */
  double t_sema = now_seconds();
  rc = vl_sema(u.ast, &u.mod, d);
  if (rc != 0) {
    units_clear(&u);
    return rc;
  }
  if (o->time_passes)
    fprintf(stderr, "[time] sema: %.3fs\n", now_seconds() - t_sema);

  /* IR */
  double t_ir = now_seconds();
  rc = vl_ir_build(u.mod, &u.ir, d);
  if (rc != 0) {
    units_clear(&u);
    return rc;
  }
  if (o->emit == EMIT_IR) {
    vl_dump_ir(u.ir, stdout, o->pretty);
    units_clear(&u);
    if (o->time_passes)
      fprintf(stderr, "[time] ir: %.3fs\n", now_seconds() - t_ir);
    return 0;
  }
  if (o->time_passes)
    fprintf(stderr, "[time] ir: %.3fs\n", now_seconds() - t_ir);

  /* Bytecode */
  double t_bc = now_seconds();
  rc = vl_bc_emit(u.ir, &u.bc, d, o->opt_level, o->compress);
  if (rc != 0) {
    units_clear(&u);
    return rc;
  }
  if (o->time_passes)
    fprintf(stderr, "[time] bc: %.3fs\n", now_seconds() - t_bc);

  /* Écriture BC ou poursuite toolchain */
  int out_kind = (o->emit == EMIT_AUTO) ? EMIT_BC : o->emit;
  if (out_kind == EMIT_BC || o->compile_only || o->assemble_only) {
    const char* outp = out_path_or_null;
    char buf[1024];
    if (!outp) {
      // déduire .vitbc
      const char* in = input_path ? input_path : "stdin";
      const char* dot = strrchr(in, '.');
      size_t len = dot ? (size_t)(dot - in) : strlen(in);
      if (len > sizeof(buf) - 8) len = sizeof(buf) - 8;
      memcpy(buf, in, len);
      buf[len] = 0;
      strcat(buf, ".vitbc");
      outp = buf;
    }
    mkdirs_for_file(outp);
    rc = vl_bc_write_file(u.bc, outp, d);
    units_clear(&u);
    return rc;
  }

  // Si --emit=obj|asm ou link final, ici on supposerait des backends natifs.
  // Par défaut non implémenté dans ce driver: on stoppe avec erreur claire.
  vl_diag_error(d, input_path ? input_path : "-", 0, 0, "E_BACKEND",
                "native backend (--emit=%s) not linked in this build",
                (out_kind == EMIT_OBJ ? "obj" : "asm"));
  units_clear(&u);
  return -2;
}

/* ========= Link simple multi-BC (placeholder) ========= */

static int link_simple_exe(const Options* o, VL_DiagSink* d) {
  // Stratégie minimale:
  // - Si on n’a que des .vitbc, et aucun backend natif, on peut empaqueter
  //   un "bundle" ou produire un .vitpkg. Ici, on refuse si pas backend.
  (void)o;
  (void)d;
  fprintf(stderr, "linker: no native backend available in this build\n");
  return -2;
}

/* ========= main ========= */

int main(int argc, char** argv) {
  Options opt;
  options_init(&opt);
  if (parse_args(&opt, argc, argv) != 0) {
    usage(stderr);
    options_free(&opt);
    return 2;
  }

  VL_DiagSink* diag = vl_diag_create(stderr, opt.color, opt.json_diag);
  if (!diag) {
    fprintf(stderr, "diag init failed\n");
    options_free(&opt);
    return 2;
  }

  double t_all = now_seconds();
  int global_rc = 0;

  // Cas multi-fichiers
  if (opt.inputs.count > 1 && !opt.preprocess_only) {
    // Si pas -c/-S et pas --emit explicite non-linkable, tenter link final
    int needs_link =
        !(opt.compile_only || opt.assemble_only || opt.emit == EMIT_PP ||
          opt.emit == EMIT_TOKENS || opt.emit == EMIT_AST ||
          opt.emit == EMIT_IR || opt.emit == EMIT_BC);
    // On compile chaque fichier → .vitbc
    for (size_t i = 0; i < opt.inputs.count; i++) {
      const char* in = opt.inputs.items[i];
      char outp[1024];
      if (opt.output && opt.inputs.count == 1) {
        snprintf(outp, sizeof(outp), "%s", opt.output);
      } else {
        const char* base = in;
        const char* dot = strrchr(base, '.');
        size_t len = dot ? (size_t)(dot - base) : strlen(base);
        if (len > sizeof(outp) - 8) len = sizeof(outp) - 8;
        memcpy(outp, base, len);
        outp[len] = 0;
        strcat(outp, ".vitbc");
      }
      int rc = compile_single(&opt, in, outp, diag);
      if (rc != 0) {
        global_rc = rc;
        goto done;
      }
    }
    if (needs_link) {
      global_rc = link_simple_exe(&opt, diag);
      goto done;
    }
    goto done;
  }

  // Cas standard: un seul fichier
  {
    const char* in = opt.inputs.items[0];
    const char* outp = opt.output;
    int rc = compile_single(&opt, in, outp, diag);
    global_rc = rc;
  }

done:
  if (opt.time_passes
