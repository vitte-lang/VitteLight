// SPDX-License-Identifier: GPL-3.0-or-later
/* ============================================================================
   /compiler/vitlc.c — VitteLight Compiler CLI (C17)
   - Parsing d’options, I/O robustes, mkdir -p cross-platform
   - Hooks : lex/parse/AST/IR (stubs à remplacer par votre frontend)
   - Diagnostics colorés, mesure de temps, code de retour précis
   Build (exemple) :
     cc -std=c17 -O2 -Wall -Wextra -pedantic \
        compiler/vitlc.c core/api.c core/code.c core/utf8.c -o vitlc
   ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h> // _mkdir
  #define PATH_SEP '\\'
#else
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #define PATH_SEP '/'
#endif

/* ————————————————————— Version / App ————————————————————— */
#define VITLC_APP     "vitlc"
#define VITLC_VERSION "0.2.0"

/* ————————————————————— Couleurs (désactivables) ————————————————————— */
static int g_use_color = 1;
#define C_RESET  (g_use_color ? "\x1b[0m"  : "")
#define C_BOLD   (g_use_color ? "\x1b[1m"  : "")
#define C_RED    (g_use_color ? "\x1b[31m" : "")
#define C_YEL    (g_use_color ? "\x1b[33m" : "")
#define C_CYA    (g_use_color ? "\x1b[36m" : "")
#define C_GRN    (g_use_color ? "\x1b[32m" : "")
#define C_BLU    (g_use_color ? "\x1b[34m" : "")

/* ————————————————————— Codes retour ————————————————————— */
enum {
  RC_OK = 0,
  RC_EARGS = 2,
  RC_EIO = 3,
  RC_ELEX = 10,
  RC_EPARSE = 11,
  RC_ESEM = 12,
  RC_EGEN = 13
};

/* ————————————————————— Utilitaires ————————————————————— */
static void die(int rc, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "%s%s: error:%s ", C_BOLD, VITLC_APP, C_RESET);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(rc);
}

static void warnf(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "%s%s: warn:%s ", C_BOLD, VITLC_APP, C_RESET);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static double now_sec(void) {
#if defined(_WIN32)
  LARGE_INTEGER fq, ct;
  QueryPerformanceFrequency(&fq);
  QueryPerformanceCounter(&ct);
  return (double)ct.QuadPart / (double)fq.QuadPart;
#else
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* dirname (simple) : copie dans out (cap octets) */
static void path_dirname(const char* path, char* out, size_t cap) {
  if (!path || !*path) { snprintf(out, cap, "."); return; }
  size_t n = strlen(path);
  while (n && (path[n-1] == '/' || path[n-1] == '\\')) n--;
  size_t i = n;
  while (i && path[i-1] != '/' && path[i-1] != '\\') i--;
  if (i == 0) { snprintf(out, cap, "."); return; }
  size_t k = (i >= cap) ? cap-1 : i;
  memcpy(out, path, k); out[k] = 0;
}

/* join simple : out = a + PATH_SEP + b (si besoin) */
/* mkdir one level */
static int mkdir_one(const char* path) {
#if defined(_WIN32)
  if (_mkdir(path) == 0) return 1;
  if (errno == EEXIST) return 1;
  return 0;
#else
  if (mkdir(path, 0777) == 0) return 1;
  if (errno == EEXIST) return 1;
  return 0;
#endif
}

/* mkdir -p */
static int mkdir_p(const char* path) {
  if (!path || !*path) return 1;
  char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s", path);
  size_t n = strlen(tmp);
  /* normaliser séparateurs sur POSIX pour la boucle; Windows supporte '/' aussi */
  for (size_t i=0;i<n;i++) if (tmp[i]=='\\') tmp[i]='/';
  char* p = tmp;
  if (*p=='/') p++; /* absolu POSIX */
  for (; *p; p++) {
    if (*p=='/') {
      *p = 0;
      if (tmp[0] && !mkdir_one(tmp)) return 0;
      *p = '/';
    }
  }
  return mkdir_one(tmp);
}

/* lecture fichier ; si path=="-" -> stdin */
static char* read_all(const char* path, size_t* out_len) {
  FILE* f = NULL;
  if (path && strcmp(path,"-")==0) f = stdin;
  else f = fopen(path, "rb");
  if (!f) { errno = errno ? errno : EIO; return NULL; }
  char* buf = NULL;
  size_t cap = 0, len = 0;
  for (;;) {
    if (len + 8192 > cap) {
      size_t ncap = cap? cap*2 : 16384;
      char* nb = (char*)realloc(buf, ncap);
      if (!nb) { free(buf); if (f!=stdin) fclose(f); errno = ENOMEM; return NULL; }
      buf = nb; cap = ncap;
    }
    size_t r = fread(buf+len, 1, 8192, f);
    len += r;
    if (r < 8192) { if (feof(f)) break; else { free(buf); if (f!=stdin) fclose(f); return NULL; } }
  }
  if (f!=stdin) fclose(f);
  if (cap==len || !buf) {
    char* nb = (char*)realloc(buf, len+1);
    if (nb) buf = nb;
  }
  if (!buf) { errno = ENOMEM; return NULL; }
  buf[len] = 0;
  if (out_len) *out_len = len;
  return buf;
}

static int write_all(const char* path, const void* data, size_t n) {
  char dname[1024];
  path_dirname(path, dname, sizeof dname);
  if (!mkdir_p(dname)) {
    fprintf(stderr, "%s%s%s: cannot create dir '%s' (%s)\n",
            C_BOLD, VITLC_APP, C_RESET, dname, strerror(errno));
    return 0;
  }
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  size_t w = fwrite(data, 1, n, f);
  fclose(f);
  return w==n;
}

/* ————————————————————— Hooks (stubs) ————————————————————— */
/* Remplacez ces fonctions par vos vraies implémentations (lexer, parser, IR…) */

typedef struct {
  int dummy;
} AST;

typedef struct {
  int dummy;
} IR;

static int lex_dump_tokens(const char* src, const char* label_out) {
  /* TODO: branchez votre vrai lexer ici */
  (void)label_out;
  size_t lines = 1;
  for (const char* p=src; *p; ++p) if (*p=='\n') lines++;
  printf("%s[lexer]%s tokens=fake count=%zu\n", C_CYA, C_RESET, lines*3);
  return 1; /* ok */
}

static int parse_to_ast(const char* src, AST* out) {
  (void)src; if (out) out->dummy = 42; /* TODO: vrai parseur */
  return 1;
}

static int ast_dump(const AST* ast, const char* out_path) {
  (void)ast;
  const char* s = "; AST (factice)\n(node 'root')\n";
  return write_all(out_path, s, strlen(s));
}

static int ast_to_ir(const AST* ast, IR* ir) {
  (void)ast; if (ir) ir->dummy = 1337; /* TODO: vrai générateur IR */
  return 1;
}

static int ir_emit_text(const IR* ir, const char* out_path) {
  (void)ir;
  const char* s = "; IR (factice)\n%0 = const 1\nret %0\n";
  return write_all(out_path, s, strlen(s));
}

static int ir_emit_object(const IR* ir, const char* out_path) {
  (void)ir;
  const char magic[] = "VLBIN\0\1";
  return write_all(out_path, magic, sizeof(magic)-1);
}

/* ————————————————————— Options CLI ————————————————————— */
typedef struct {
  const char* in_path;       /* "-" = stdin */
  const char* out_path;      /* par défaut : "out/a.out" */
  const char* ast_out;       /* si --dump-ast=FILE */
  int   dump_tokens;         /* --dump-tokens */
  int   emit_ir;             /* -emit-ir (texte IR) */
  int   optimize;            /* -O[0..3] (stockage simple) */
  int   trace;               /* --trace */
  int   timeit;              /* --time */
  int   show_version;        /* -v / --version */
  int   show_help;           /* -h / --help */
  /* (stockage simple des -I) */
  const char* include_dirs[32];
  int   include_count;
} Opts;

static void usage(FILE* out) {
  fprintf(out,
    "%s%s %s%s — VitteLight Compiler\n"
    "Usage:\n"
    "  %s <fichier.vitl | -> [options]\n\n"
    "Options générales:\n"
    "  -o <file>         Fichier de sortie (déf: out/a.out)\n"
    "  -I <dir>          Ajouter un répertoire d'includes (mult. autorisé)\n"
    "  -O[0..3]          Niveau d'optimisation (stockage option)\n"
    "  -emit-ir          Émettre IR texte plutôt que binaire objet\n"
    "  --dump-tokens     Afficher les tokens du lexer (diagnostic)\n"
    "  --dump-ast=<f>    Écrire l’AST (texte) dans <f>\n"
    "  --trace           Trace interne (front-end)\n"
    "  --time            Mesurer les étapes (lex/parse/IR/emit)\n"
    "  -v, --version     Afficher la version\n"
    "  -h, --help        Aide\n",
    C_BOLD, VITLC_APP, VITLC_VERSION, C_RESET, VITLC_APP);
}

static int parse_opts(int argc, char** argv, Opts* o) {
  memset(o, 0, sizeof *o);
  o->out_path = "out/a.out";
  for (int i=1;i<argc;i++) {
    const char* a = argv[i];
    if (strcmp(a, "-h")==0 || strcmp(a,"--help")==0) { o->show_help = 1; continue; }
    if (strcmp(a, "-v")==0 || strcmp(a,"--version")==0) { o->show_version = 1; continue; }
    if (strcmp(a, "-emit-ir")==0) { o->emit_ir = 1; continue; }
    if (strcmp(a, "--dump-tokens")==0) { o->dump_tokens = 1; continue; }
    if (strncmp(a, "--dump-ast=", 11)==0) { o->ast_out = a+11; continue; }
    if (strcmp(a, "--trace")==0) { o->trace = 1; continue; }
    if (strcmp(a, "--time")==0) { o->timeit = 1; continue; }
    if (strcmp(a, "-o")==0 && i+1<argc) { o->out_path = argv[++i]; continue; }
    if (strcmp(a, "-I")==0 && i+1<argc) {
      if ( o->include_count < (int)(sizeof o->include_dirs / sizeof o->include_dirs[0]) )
        o->include_dirs[o->include_count++] = argv[++i];
      else warnf("trop de -I (ignoré)");
      continue;
    }
    if (a[0]=='-' && a[1]=='O' && a[2] && !a[3]) { o->optimize = a[2]-'0'; continue; }
    if (!o->in_path) { o->in_path = a; continue; }
    warnf("argument ignoré: %s", a);
  }
  return 1;
}

/* ————————————————————— Programme principal ————————————————————— */
int main(int argc, char** argv) {
  Opts opt; parse_opts(argc, argv, &opt);

  if (opt.show_help) { usage(stdout); return RC_OK; }
  if (opt.show_version) { printf("%s %s\n", VITLC_APP, VITLC_VERSION); return RC_OK; }
  if (!opt.in_path) { usage(stderr); return RC_EARGS; }

  /* Couleur désactivable via NO_COLOR */
  if (getenv("NO_COLOR")) g_use_color = 0;

  /* Lecture source */
  size_t src_len = 0;
  char* src = read_all(opt.in_path, &src_len);
  if (!src) die(RC_EIO, "lecture '%s' échouée (%s)", opt.in_path, strerror(errno));

  if (opt.timeit) fprintf(stderr, "%s== time: start ==%s\n", C_BLU, C_RESET);
  double t_lex0 = now_sec();

  /* Dump tokens si demandé */
  if (opt.dump_tokens) {
    if (!lex_dump_tokens(src, opt.in_path)) { free(src); die(RC_ELEX, "lexing échoué"); }
  }

  double t_lex1 = now_sec();
  if (opt.timeit) fprintf(stderr, "  lex: %.3f ms\n", (t_lex1-t_lex0)*1e3);

  /* Parse → AST */
  double t_parse0 = now_sec();
  AST ast;
  if (!parse_to_ast(src, &ast)) { free(src); die(RC_EPARSE, "parse échoué"); }
  double t_parse1 = now_sec();
  if (opt.timeit) fprintf(stderr, "  parse: %.3f ms\n", (t_parse1-t_parse0)*1e3);

  /* Dump AST éventuel */
  if (opt.ast_out) {
    if (!ast_dump(&ast, opt.ast_out)) { free(src); die(RC_EIO, "écriture AST '%s' échouée", opt.ast_out); }
  }

  /* AST → IR */
  double t_ir0 = now_sec();
  IR ir;
  if (!ast_to_ir(&ast, &ir)) { free(src); die(RC_ESEM, "génération IR échouée"); }
  double t_ir1 = now_sec();
  if (opt.timeit) fprintf(stderr, "  irgen: %.3f ms\n", (t_ir1-t_ir0)*1e3);

  /* Émission */
  double t_emit0 = now_sec();
  int ok = opt.emit_ir ? ir_emit_text(&ir, opt.out_path)
                       : ir_emit_object(&ir, opt.out_path);
  double t_emit1 = now_sec();
  if (!ok) { free(src); die(RC_EGEN, "émission '%s' échouée", opt.out_path); }
  if (opt.timeit) fprintf(stderr, "  emit: %.3f ms\n", (t_emit1-t_emit0)*1e3);

  if (opt.timeit) {
    fprintf(stderr, "%s== time: done ==%s total=%.3f ms  → %s%s%s\n",
      C_BLU, C_RESET, (t_emit1 - t_lex0)*1e3, C_GRN, opt.out_path, C_RESET);
  } else {
    fprintf(stderr, "%sok%s → %s\n", C_GRN, C_RESET, opt.out_path);
  }

  free(src);
  (void)opt.trace; (void)opt.optimize; (void)opt.include_count; (void)opt.include_dirs;
  return RC_OK;
}
