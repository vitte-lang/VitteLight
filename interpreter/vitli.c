// vitte-light/interpreter/vitlc.c
// VitteLight Tooling CLI (vitlc)
// Sous-commandes: run, asm, disasm, dump, repl, bench, help
// Sources ASM -> VLBC via core/parser.c ; VLBC -> exécution via core/vm.c
//
// Build (Unix):
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -o vitlc \
//      interpreter/vitlc.c \
//      core/parser.c core/undump.c core/vm.c core/zio.c core/string.c \
//      core/table.c core/tm.c core/ctype.c core/stave.c
//   # + vos autres .c si nécessaires (mem.c, object.c, opcodes.c, etc.)
//
// Exemples:
//   ./vitlc asm prog.asm -o prog.vlbc
//   ./vitlc run prog.vlbc --trace op,stack
//   ./vitlc run -e "PUSHS \"hi\"\nCALLN print,1\nHALT"
//   ./vitlc disasm prog.vlbc
//   ./vitlc dump prog.vlbc --hexdump
//   ./vitlc bench prog.vlbc -n 1000

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "mem.h"
#include "opcodes.h"
#include "parser.h"
#include "state.h"
#include "string.h"
#include "tm.h"
#include "undump.h"
#include "vl_compat.h"
#include "vm.h"
#include "zio.h"

#ifndef VITLC_VERSION
#define VITLC_VERSION "0.1"
#endif

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void usage(FILE *out) {
  fprintf(out,
          "vitlc %s\n\n"
          "Usage: vitlc <cmd> [options] [file]\n\n"
          "Commands:\n"
          "  run [file.{vlbc|asm}] [--trace <flags>] [--max-steps N] "
          "[--disasm] [--print-stack]\n"
          "      [-e <asm>]\n"
          "  asm <in.asm> [-o out.vlbc]\n"
          "  disasm <in.vlbc>\n"
          "  dump <in.vlbc> [--hexdump] [--strings]\n"
          "  repl\n"
          "  bench <in.{vlbc|asm}> [-n iters] [--trace <flags>]\n"
          "  help\n\n"
          "Trace flags: op,stack,global,call,all\n",
          VITLC_VERSION);
}

// ───────────────────────── Utils fichiers ─────────────────────────
static int has_ext(const char *path, const char *ext) {
  size_t lp = strlen(path), le = strlen(ext);
  if (lp < le) return 0;
  return strcasecmp(path + lp - le, ext) == 0;
}

static int write_all(const char *path, const void *data, size_t n) {
  return vl_write_file(path, data, n);
}

static char *slurp(const char *path, size_t *out_n) {
  uint8_t *buf = NULL;
  size_t n = 0;
  if (!vl_read_file_all(path, &buf, &n)) return NULL;
  char *s = (char *)malloc(n + 1);
  if (!s) {
    free(buf);
    return NULL;
  }
  memcpy(s, buf, n);
  s[n] = '\0';
  free(buf);
  if (out_n) *out_n = n;
  return s;
}

static uint32_t parse_trace_mask(const char *flags) {
  if (!flags) return 0;
  uint32_t m = 0;
  const char *p = flags;
  char tok[32];
  while (*p) {
    size_t k = 0;
    while (*p == ',' || *p == ' ') p++;
    while (*p && *p != ',' && k < sizeof(tok) - 1) {
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
  }
  return m;
}

// ───────────────────────── Assemblage ─────────────────────────
static int asm_from_path(const char *in, uint8_t **out_bytes,
                         size_t *out_size) {
  char err[512];
  if (!vl_asm_file(in, out_bytes, out_size, err, sizeof(err))) {
    fprintf(stderr, "asm: %s\n", err);
    return 0;
  }
  return 1;
}

static int asm_from_string(const char *src, uint8_t **out_bytes,
                           size_t *out_size) {
  char err[512];
  if (!vl_asm(src, strlen(src), out_bytes, out_size, err, sizeof(err))) {
    fprintf(stderr, "asm: %s\n", err);
    return 0;
  }
  return 1;
}

// ───────────────────────── Chargement module ─────────────────────────
static int module_from_vlbc_buf(const uint8_t *bytes, size_t n,
                                VL_Module *out) {
  char err[256];
  VL_Status st = vl_module_from_buffer(bytes, n, out, err, sizeof(err));
  if (st != VL_OK) {
    fprintf(stderr, "undump: %s\n", err[0] ? err : "error");
    return 0;
  }
  return 1;
}

static int module_from_path(const char *path, VL_Module *out) {
  char err[256];
  VL_Status st = vl_module_from_file(path, out, err, sizeof(err));
  if (st != VL_OK) {
    fprintf(stderr, "undump: %s\n", err[0] ? err : "error");
    return 0;
  }
  return 1;
}

static int module_from_asm_path(const char *asm_path, VL_Module *out,
                                uint8_t **tmp, size_t *tn) {
  *tmp = NULL;
  *tn = 0;
  if (!asm_from_path(asm_path, tmp, tn)) return 0;
  int ok = module_from_vlbc_buf(*tmp, *tn, out);
  if (!ok) {
    free(*tmp);
    *tmp = NULL;
    *tn = 0;
  }
  return ok;
}

static int module_from_asm_string(const char *asm_src, VL_Module *out,
                                  uint8_t **tmp, size_t *tn) {
  *tmp = NULL;
  *tn = 0;
  if (!asm_from_string(asm_src, tmp, tn)) return 0;
  int ok = module_from_vlbc_buf(*tmp, *tn, out);
  if (!ok) {
    free(*tmp);
    *tmp = NULL;
    *tn = 0;
  }
  return ok;
}

// ───────────────────────── Exécution ─────────────────────────
static int run_module(const VL_Module *m, uint32_t trace_mask,
                      uint64_t max_steps, int print_stack, int disasm_before) {
  if (disasm_before) {
    vl_module_disasm(m, stdout);
  }
  struct VL_Context *ctx = vl_ctx_new();
  if (!ctx) {
    fprintf(stderr, "OOM ctx\n");
    return 0;
  }
  vl_ctx_register_std(ctx);
  VL_Status st = vl_ctx_attach_module(ctx, m);
  if (st != VL_OK) {
    fprintf(stderr, "attach: %d\n", st);
    vl_ctx_free(ctx);
    return 0;
  }
  if (trace_mask) vl_trace_enable(ctx, trace_mask);
  st = vl_run(ctx, max_steps);
  if (st != VL_OK) {
    fprintf(stderr, "run: status=%d\n", st);
    vl_ctx_free(ctx);
    return 0;
  }
  if (print_stack) {
    vl_state_dump_stack(ctx, stdout);
  }
  vl_ctx_free(ctx);
  return 1;
}

// ───────────────────────── Sous-commandes ─────────────────────────
static int cmd_asm(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "asm: besoin d'un fichier .asm\n");
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
  if (!asm_from_path(in, &bytes, &n)) return 1;
  if (out) {
    if (!write_all(out, bytes, n)) die("écriture échouée: %s", out);
  } else {
    vl_hexdump(bytes, n, 0, stdout);
  }
  free(bytes);
  return 0;
}

static int cmd_disasm(int argc, char **argv) {
  if (argc < 2) die("disasm: besoin d'un .vlbc");
  VL_Module m;
  if (!module_from_path(argv[1], &m)) return 1;
  vl_module_disasm(&m, stdout);
  vl_module_free(&m);
  return 0;
}

static int cmd_dump(int argc, char **argv) {
  if (argc < 2) die("dump: besoin d'un .vlbc");
  int do_hex = 0, do_str = 0;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--hexdump") == 0)
      do_hex = 1;
    else if (strcmp(argv[i], "--strings") == 0)
      do_str = 1;
  }
  VL_Module m;
  if (!module_from_path(argv[1], &m)) return 1;
  printf("VLBC: kstr=%u code=%u bytes\n", m.kcount, m.code_len);
  if (do_str) {
    for (uint32_t i = 0; i < m.kcount; i++) {
      printf("[%u] %s\n", i, m.kstr[i]);
    }
  }
  if (do_hex) vl_hexdump(m.code, m.code_len, 0, stdout);
  vl_module_free(&m);
  return 0;
}

static int cmd_run(int argc, char **argv) {
  const char *file = NULL;
  const char *expr = NULL;
  uint32_t tr = 0;
  uint64_t max_steps = 0;
  int disasm_before = 0, print_stack = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
      tr = parse_trace_mask(argv[++i]);
    } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
      max_steps = (uint64_t)strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--disasm") == 0) {
      disasm_before = 1;
    } else if (strcmp(argv[i], "--print-stack") == 0) {
      print_stack = 1;
    } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
      expr = argv[++i];
    } else if (argv[i][0] != '-') {
      file = argv[i];
    } else {
      fprintf(stderr, "run: option inconnue: %s\n", argv[i]);
      return 2;
    }
  }
  VL_Module m;
  uint8_t *tmp = NULL;
  size_t tn = 0;
  int ok = 0;
  if (expr) {
    ok = module_from_asm_string(expr, &m, &tmp, &tn);
  } else if (file) {
    if (has_ext(file, ".vlbc"))
      ok = module_from_path(file, &m);
    else
      ok = module_from_asm_path(file, &m, &tmp, &tn);
  } else {
    fprintf(stderr, "run: fournir -e <asm> ou un fichier .asm/.vlbc\n");
    return 2;
  }
  if (!ok) {
    free(tmp);
    return 1;
  }
  int rc = run_module(&m, tr, max_steps, print_stack, disasm_before) ? 0 : 1;
  vl_module_free(&m);
  free(tmp);
  return rc;
}

static int cmd_bench(int argc, char **argv) {
  if (argc < 2) die("bench: besoin d'un .asm/.vlbc");
  const char *path = argv[1];
  uint64_t iters = 1000;
  uint32_t tr = 0;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      iters = (uint64_t)strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
      tr = parse_trace_mask(argv[++i]);
    }
  }
  VL_Module m;
  uint8_t *tmp = NULL;
  size_t tn = 0;
  int ok = has_ext(path, ".vlbc") ? module_from_path(path, &m)
                                  : module_from_asm_path(path, &m, &tmp, &tn);
  if (!ok) {
    free(tmp);
    return 1;
  }
  struct VL_Context *ctx = vl_ctx_new();
  if (!ctx) die("OOM ctx");
  vl_ctx_register_std(ctx);
  VL_Status st = vl_ctx_attach_module(ctx, &m);
  if (st != VL_OK) die("attach: %d", st);
  if (tr) vl_trace_enable(ctx, tr);
  VL_Stopwatch sw;
  vl_sw_start(&sw);
  for (uint64_t i = 0; i < iters; i++) {
    st = vl_run(ctx, 0);
    if (st != VL_OK) {
      fprintf(stderr, "run: %d\n", st);
      break;
    }
    vl_state_set_ip(ctx, 0);
  }
  uint64_t ns = vl_sw_elapsed_ns(&sw);
  double per = iters ? (double)ns / (double)iters : 0.0;
  printf("iters=%" PRIu64 " total=%.3f ms per=%.1f us\n", iters, ns / 1e6,
         per / 1e3);
  vl_ctx_free(ctx);
  vl_module_free(&m);
  free(tmp);
  return (st == VL_OK) ? 0 : 1;
}

static int cmd_repl(int argc, char **argv) {
  (void)argc;
  (void)argv;
  puts(
      "VitteLight REPL. Tapez :help pour l'aide. Chaque ligne est de l'ASM. "
      "CTRL+D pour quitter.");
  struct VL_Context *ctx = vl_ctx_new();
  if (!ctx) die("OOM ctx");
  vl_ctx_register_std(ctx);
  VL_Buffer line;
  vl_buf_init(&line);
  for (;;) {
    fputs("> ", stdout);
    fflush(stdout);
    line.n = 0;
    VL_Reader r;
    char *buf = NULL;
    size_t n = 0;  // lecture via fgets
    char tmp[512];
    if (!fgets(tmp, sizeof(tmp), stdin)) {
      puts("");
      break;
    }
    vl_buf_append(&line, tmp, strlen(tmp));
    if (line.n >= 2 && line.d[0] == ':') {
      if (strncmp((char *)line.d, ":help", 5) == 0) {
        puts(":help, :trace <flags>, :disasm <asm>, :stack");
        continue;
      }
      if (strncmp((char *)line.d, ":trace", 6) == 0) {
        char *f = (char *)line.d + 6;
        while (*f == ' ' || *f == '\t') f++;
        uint32_t m = parse_trace_mask(f);
        vl_trace_disable(ctx, 0xFFFFFFFFu);
        vl_trace_enable(ctx, m);
        printf("trace=0x%08x\n", m);
        continue;
      }
      if (strncmp((char *)line.d, ":stack", 6) == 0) {
        vl_state_dump_stack(ctx, stdout);
        continue;
      }
      if (strncmp((char *)line.d, ":disasm", 7) == 0) {
        char *p = (char *)line.d + 7;
        while (*p == ' ' || *p == '\t') p++;
        uint8_t *bc = NULL;
        size_t bn = 0;
        if (!asm_from_string(p, &bc, &bn)) {
          continue;
        }
        VL_Module m;
        if (module_from_vlbc_buf(bc, bn, &m)) {
          vl_module_disasm(&m, stdout);
          vl_module_free(&m);
        }
        free(bc);
        continue;
      }
      puts("commande inconnue");
      continue;
    }
    uint8_t *bc = NULL;
    size_t bn = 0;
    if (!asm_from_string((const char *)line.d, &bc, &bn)) continue;
    VL_Module mod;
    if (!module_from_vlbc_buf(bc, bn, &mod)) {
      free(bc);
      continue;
    }
    VL_Status st = vl_ctx_attach_module(ctx, &mod);
    if (st != VL_OK) {
      fprintf(stderr, "attach: %d\n", st);
      vl_module_free(&mod);
      free(bc);
      continue;
    }
    st = vl_run(ctx, 0);
    if (st != VL_OK) fprintf(stderr, "run: %d\n", st);
    vl_module_free(&mod);
    free(bc);
  }
  vl_buf_free(&line);
  vl_ctx_free(ctx);
  return 0;
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
  if (strcmp(cmd, "asm") == 0) return cmd_asm(argc - 1, argv + 1);
  if (strcmp(cmd, "disasm") == 0) return cmd_disasm(argc - 1, argv + 1);
  if (strcmp(cmd, "dump") == 0) return cmd_dump(argc - 1, argv + 1);
  if (strcmp(cmd, "run") == 0) return cmd_run(argc - 1, argv + 1);
  if (strcmp(cmd, "bench") == 0) return cmd_bench(argc - 1, argv + 1);
  if (strcmp(cmd, "repl") == 0) return cmd_repl(argc - 1, argv + 1);
  // compat: sans commande explicite on fait un run
  return cmd_run(argc - 1, argv + 1);
}
