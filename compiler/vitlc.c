// vitte-light/compiler/vitlc.c
// VitteLight Compiler/Linker CLI (vitlc)
// Sous-commandes: compile, link, build, inspect, help, version
// Entrées acceptées: .asm (assemble vers VLBC) et .vlbc (objet binaire VL)
// Sortie: VLBC monolithique (pool de chaînes fusionné, code patché)
//
// Tâches majeures:
//  - Assemblage ASM -> VLBC via parser.h (vl_asm / vl_asm_file)
//  - Chargement VLBC via undump.h (VL_Module)
//  - Fusion des pools kstr avec déduplication
//  - Réécriture des indices kstr dans le code (PUSHS, CALLN, LOADG, STOREG)
//  - Écriture d'un VLBC final (en-tête + kstr + code)
//  - Fichiers auxiliaires: --map pour tracer les remappings de si
//
// Build (Unix):
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -o vitlc \
//      compiler/vitlc.c \
//      core/parser.c core/undump.c core/opcodes.c core/mem.c core/zio.c \
//      core/table.c core/string.c
//
// Exemple:
//   ./vitlc compile in.asm -o out.vlbc
//   ./vitlc link lib1.vlbc lib2.asm -o prog.vlbc --map prog.map
//   ./vitlc inspect prog.vlbc --strings

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#define strcasecmp _stricmp
#else
#include <strings.h>
#include <unistd.h>
#endif

#include "api.h"
#include "mem.h"      // VL_Buffer, vl_write_file
#include "opcodes.h"  // OP_* et helpers de disasm/validate
#include "parser.h"   // vl_asm / vl_asm_file
#include "table.h"    // VL_Table cstr -> index
#include "undump.h"   // VL_Module, vl_module_from_* / VLBC_VERSION
#include "zio.h"      // vl_read_file_all, VL_Writer

#ifndef VITLC_VERSION
#define VITLC_VERSION "0.3"
#endif
#ifndef VLBC_MAGIC
#define VLBC_MAGIC "VLBC"
#endif

// ───────────────────────── UI ─────────────────────────
static int want_color(FILE *f) {
  const char *e = getenv("NO_COLOR");
  if (e && *e) return 0;
  return isatty(fileno(f));
}
static void eprintf_col(int usec, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (usec) fprintf(stderr, "\x1b[31m");
  vfprintf(stderr, fmt, ap);
  if (usec) fprintf(stderr, "\x1b[0m");
  fputc('\n', stderr);
  va_end(ap);
}
static void dief(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void usage(FILE *out) {
  fprintf(out,
          "vitlc %s (compiler/linker)\n\n"
          "Usage: vitlc <cmd> [options] [files]\n\n"
          "Commands:\n"
          "  compile <in.asm>|- [-o out.vlbc]\n"
          "  link <in.{vlbc|asm}>... [-o out.vlbc] [--map file] [--disasm "
          "out.txt]\n"
          "  build ...            alias de link\n"
          "  inspect <in.vlbc> [--strings] [--hexdump]\n"
          "  version | --version\n"
          "  help | --help\n",
          VITLC_VERSION);
}

// ───────────────────────── I/O helpers ─────────────────────────
static int write_all(const char *path, const void *data, size_t n) {
  return vl_write_file(path, data, n);
}

static char *slurp_stdin(size_t *out_n) {
  VL_Buffer b;
  vl_buf_init(&b);
  char tmp[4096];
  size_t rd;
  while ((rd = fread(tmp, 1, sizeof(tmp), stdin)) > 0) {
    vl_buf_append(&b, tmp, rd);
  }
  char *s = (char *)malloc(b.n + 1);
  if (!s) {
    vl_buf_free(&b);
    return NULL;
  }
  memcpy(s, b.d, b.n);
  s[b.n] = '\0';
  if (out_n) *out_n = b.n;
  vl_buf_free(&b);
  return s;
}

static int has_ext(const char *p, const char *ext) {
  size_t lp = strlen(p), le = strlen(ext);
  if (lp < le) return 0;
  return strcasecmp(p + lp - le, ext) == 0;
}

// ───────────────────────── Assemblage ─────────────────────────
static int asm_from_path(const char *in, uint8_t **out_bytes,
                         size_t *out_size) {
  char err[512];
  if (!vl_asm_file(in, out_bytes, out_size, err, sizeof(err))) {
    eprintf_col(want_color(stderr), "asm(%s): %s", in, err);
    return 0;
  }
  return 1;
}
static int asm_from_string(const char *src, uint8_t **out_bytes,
                           size_t *out_size) {
  char err[512];
  if (!vl_asm(src, strlen(src), out_bytes, out_size, err, sizeof(err))) {
    eprintf_col(want_color(stderr), "asm(stdin): %s", err);
    return 0;
  }
  return 1;
}

// ───────────────────────── Module loader ─────────────────────────
static int module_from_vlbc_buf(const uint8_t *bytes, size_t n,
                                VL_Module *out) {
  char err[256];
  VL_Status st = vl_module_from_buffer(bytes, n, out, err, sizeof(err));
  if (st != VL_OK) {
    eprintf_col(want_color(stderr), "undump: %s", err[0] ? err : "error");
    return 0;
  }
  return 1;
}
static int module_from_path(const char *path, VL_Module *out) {
  char err[256];
  VL_Status st = vl_module_from_file(path, out, err, sizeof(err));
  if (st != VL_OK) {
    eprintf_col(want_color(stderr), "undump(%s): %s", path,
                err[0] ? err : "error");
    return 0;
  }
  return 1;
}

// ───────────────────────── KSTR fusion ─────────────────────────
// Map cstr -> new_si, via VL_Table (clé dupliquée)
static uint32_t add_kstr_dedup(VL_Table *map, char ***out_arr, uint32_t *out_n,
                               const char *s) {
  void *v = NULL;
  if (vl_tab_get_cstr(map, s, &v)) {
    uintptr_t u = (uintptr_t)v;
    return (uint32_t)(u - 1u);
  }
  // nouveau
  uint32_t new_si = *out_n;
  char *dup = (char *)malloc(strlen(s) + 1);
  if (!dup) dief("OOM kstr");
  strcpy(dup, s);
  // étendre tableau
  char **arr = *out_arr;
  arr = (char **)realloc(arr, (size_t)(new_si + 1u) * sizeof(char *));
  if (!arr) dief("OOM kstr arr");
  arr[new_si] = dup;
  *out_arr = arr;
  *out_n = new_si + 1u;
  vl_tab_put_cstr(map, dup, (void *)(uintptr_t)(new_si + 1u));  // stocke si+1
  return new_si;
}

// pour un module, bâtit old->new mapping
static uint32_t *build_si_map(VL_Table *glob, char ***dst_kstr, uint32_t *dst_n,
                              const VL_Module *m) {
  uint32_t *map = (uint32_t *)malloc((size_t)m->kcount * sizeof(uint32_t));
  if (!map) dief("OOM si map");
  for (uint32_t i = 0; i < m->kcount; i++) {
    const char *s = m->kstr[i] ? m->kstr[i] : "";
    map[i] = add_kstr_dedup(glob, dst_kstr, dst_n, s);
  }
  return map;
}

// ───────────────────────── Patching code ─────────────────────────
static inline uint32_t rd_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static inline void wr_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static inline uint64_t rd_u64le(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
  return v;
}

static size_t insn_size(uint8_t op) {
  switch (op) {
    case OP_NOP:
      return 1;
    case OP_PUSHI:
      return 1 + 8;  // u64
    case OP_PUSHF:
      return 1 + 8;  // f64
    case OP_PUSHS:
      return 1 + 4;  // u32 si
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_EQ:
    case OP_NEQ:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE:
    case OP_PRINT:
    case OP_POP:
    case OP_HALT:
      return 1;
    case OP_STOREG:
      return 1 + 4;  // u32 si
    case OP_LOADG:
      return 1 + 4;  // u32 si
    case OP_CALLN:
      return 1 + 4 + 1;  // u32 si + u8 argc
    default:
      return 0;  // inconnu -> invalide
  }
}

static int patch_code_kstr(uint8_t *dst, const uint8_t *src, size_t n,
                           const uint32_t *si_map, uint32_t map_len) {
  size_t i = 0;
  while (i < n) {
    uint8_t op = src[i];
    size_t sz = insn_size(op);
    if (sz == 0 || i + sz > n) return 0;  // invalide
    memcpy(dst + i, src + i, sz);
    switch (op) {
      case OP_PUSHS:
      case OP_STOREG:
      case OP_LOADG: {
        uint32_t old = rd_u32le(src + i + 1);
        if (old >= map_len) return 0;
        uint32_t neu = si_map[old];
        wr_u32le(dst + i + 1, neu);
      } break;
      case OP_CALLN: {
        uint32_t old = rd_u32le(src + i + 1);
        if (old >= map_len) return 0;
        uint32_t neu = si_map[old];
        wr_u32le(dst + i + 1, neu); /* argc byte left as-is */
      } break;
      default:
        break;
    }
    i += sz;
  }
  return 1;
}

// ───────────────────────── Linker ─────────────────────────
typedef struct InMod {
  VL_Module m;
  uint8_t *tmp_bc;
  size_t tmp_bn;
  char *name;
  uint32_t *si_map;
} InMod;

static void free_inmod(InMod *im) {
  if (!im) return;
  if (im->si_map) free(im->si_map);
  if (im->tmp_bc) free(im->tmp_bc);
  if (im->name) free(im->name);
  vl_module_free(&im->m);
  memset(im, 0, sizeof(*im));
}

static int load_input(const char *path, InMod *out) {
  memset(out, 0, sizeof(*out));
  out->name = strdup(path);
  if (has_ext(path, ".vlbc")) return module_from_path(path, &out->m);
  if (has_ext(path, ".asm")) {
    if (!asm_from_path(path, &out->tmp_bc, &out->tmp_bn)) return 0;
    return module_from_vlbc_buf(out->tmp_bc, out->tmp_bn, &out->m);
  }
  eprintf_col(want_color(stderr), "format non supporté: %s", path);
  return 0;
}

static int link_modules(int n, InMod *imods, const char *out_vlbc,
                        const char *map_path, const char *disasm_out) {
  // 1) fusion kstr
  VL_Table dict;
  vl_tab_init_cstr(&dict, 256);
  char **kstr = NULL;
  uint32_t kcount = 0;
  for (int i = 0; i < n; i++) {
    imods[i].si_map = build_si_map(&dict, &kstr, &kcount, &imods[i].m);
  }

  // 2) taille code total
  size_t total_code = 0;
  for (int i = 0; i < n; i++) {
    total_code += imods[i].m.code_len;
  }

  // 3) patch + concat code
  uint8_t *code = (uint8_t *)malloc(total_code ? total_code : 1);
  if (!code) dief("OOM code");
  size_t off = 0;
  for (int i = 0; i < n; i++) {
    if (!patch_code_kstr(code + off, imods[i].m.code, imods[i].m.code_len,
                         imods[i].si_map, imods[i].m.kcount))
      dief("patch échec dans %s", imods[i].name);
    off += imods[i].m.code_len;
  }

  // 4) validation structurelle
  if (vl_validate_code(code, (uint32_t)total_code, kcount) != VL_OK)
    dief("bytecode final invalide");

  // 5) écrire VLBC
  VL_Writer w;
  if (!vl_w_init_file(&w, out_vlbc ? out_vlbc : "a.vlbc")) dief("open sortie");
  // magic + version
  vl_w_write(&w, VLBC_MAGIC, 4);
  vl_w_u8(&w, (uint8_t)VLBC_VERSION);
  // kcount
  vl_w_u32le(&w, kcount);
  for (uint32_t i = 0; i; kcount && i < kcount; i++) {
  }  // keep compiler quiet if unused
  for (uint32_t i = 0; i < kcount; i++) {
    uint32_t L = (uint32_t)strlen(kstr[i]);
    vl_w_u32le(&w, L);
    vl_w_write(&w, kstr[i], L);
  }
  // code
  vl_w_u32le(&w, (uint32_t)total_code);
  vl_w_write(&w, code, total_code);
  vl_w_close(&w);

  // 6) map optionnelle
  if (map_path) {
    FILE *fp = fopen(map_path, "wb");
    if (!fp) dief("open map");
    fprintf(fp, "# vitte-light link map\n");
    for (int i = 0; i < n; i++) {
      fprintf(fp, "[%s]\n", imods[i].name);
      for (uint32_t si = 0; si < imods[i].m.kcount; si++) {
        fprintf(fp, "  %u -> %u\n", si, imods[i].si_map[si]);
      }
    }
    fclose(fp);
  }

  // 7) disasm optionnelle
  if (disasm_out) {
    FILE *fp = fopen(disasm_out, "wb");
    if (!fp) dief("open disasm");  // utiliser désassembleur en mémoire
    // Fake module pour réutiliser vl_module_disasm
    VL_Module tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.kstr = kstr;
    tmp.kcount = kcount;
    tmp.code = code;
    tmp.code_len = (uint32_t)total_code;
    vl_disasm_program(tmp.code, tmp.code_len, fp);
    fclose(fp);
  }

  // 8) free
  for (uint32_t i = 0; i < kcount; i++) free(kstr[i]);
  free(kstr);
  free(code);
  vl_tab_release(&dict);
  return 1;
}

// ───────────────────────── Commands ─────────────────────────
static int cmd_compile(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "compile: besoin d'un fichier .asm ou '-'\n");
    return 2;
  }
  const char *in = argv[1];
  const char *out = NULL;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out = argv[++i];
    }
  }
  uint8_t *bytes = NULL;
  size_t n = 0;
  int ok = 0;
  if (strcmp(in, "-") == 0) {
    char *src = slurp_stdin(&n);
    if (!src) dief("lecture stdin");
    ok = asm_from_string(src, &bytes, &n);
    free(src);
  } else {
    ok = asm_from_path(in, &bytes, &n);
  }
  if (!ok) return 1;
  if (!out) out = "a.vlbc";
  if (!write_all(out, bytes, n)) dief("écriture: %s", out);
  free(bytes);
  return 0;
}

static int cmd_inspect(int argc, char **argv) {
  if (argc < 2) dief("inspect: besoin d'un .vlbc");
  int do_str = 0, do_hex = 0;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--strings") == 0)
      do_str = 1;
    else if (strcmp(argv[i], "--hexdump") == 0)
      do_hex = 1;
  }
  VL_Module m;
  if (!module_from_path(argv[1], &m)) return 1;
  printf("VLBC: kstr=%u code=%u bytes\n", m.kcount, m.code_len);
  if (do_str) {
    for (uint32_t i = 0; i < m.kcount; i++) {
      printf("[%u] %s\n", i, m.kstr[i] ? m.kstr[i] : "");
    }
  }
  if (do_hex) {
    vl_hexdump(m.code, m.code_len, 0, stdout);
  }
  vl_module_free(&m);
  return 0;
}

static int cmd_link(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "link: besoin d'au moins un fichier .vlbc/.asm\n");
    return 2;
  }
  const char *out = "a.vlbc";
  const char *map = NULL;
  const char *disasm_out = NULL;
  int n_inputs = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
      map = argv[++i];
    } else if (strcmp(argv[i], "--disasm") == 0 && i + 1 < argc) {
      disasm_out = argv[++i];
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "link: option inconnue: %s\n", argv[i]);
      return 2;
    } else
      n_inputs++;
  }
  if (n_inputs == 0) {
    fprintf(stderr, "link: aucun input\n");
    return 2;
  }
  InMod *arr = (InMod *)calloc((size_t)n_inputs, sizeof(InMod));
  if (!arr) dief("OOM inputs");
  int k = 0;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--map") == 0 ||
          strcmp(argv[i], "--disasm") == 0)
        i++;
      continue;
    }
    if (!load_input(argv[i], &arr[k])) {
      while (k >= 0) {
        free_inmod(&arr[k]);
        k--;
      }
      free(arr);
      return 1;
    }
    k++;
  }
  int ok = link_modules(k, arr, out, map, disasm_out);
  for (int i = 0; i < k; i++) free_inmod(&arr[i]);
  free(arr);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(stdout);
    return 0;
  }
  const char *cmd = argv[1];
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
      strcmp(cmd, "-h") == 0) {
    usage(stdout);
    return 0;
  }
  if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
    printf("vitlc %s\n", VITLC_VERSION);
    return 0;
  }
  if (strcmp(cmd, "compile") == 0) return cmd_compile(argc - 1, argv + 1);
  if (strcmp(cmd, "link") == 0 || strcmp(cmd, "build") == 0)
    return cmd_link(argc - 1, argv + 1);
  if (strcmp(cmd, "inspect") == 0) return cmd_inspect(argc - 1, argv + 1);
  // compat: sans commande explicite, tenter link sur les restes
  return cmd_link(argc - 1, argv + 1);
}
