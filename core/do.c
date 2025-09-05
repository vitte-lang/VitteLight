/* ============================================================================
   do.c — outil de démonstration/diagnostic pour debug.h/debug.c (C17)
   - CLI complète: niveau, format, couleur, fichier + rotation, bench, hexdump,
     backtrace, crash volontaire, échantillons multi-niveaux.
   - Cross-platform. Sans dépendance à getopt.
   - Compile: cc -std=c17 do.c debug.c -ldl -pthread (POSIX)
              cl /std:c17 do.c debug.c Dbghelp.lib (Windows)
   Licence: MIT
   ============================================================================
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"

/* ----------------------------------------------------------------------------
   Utils
---------------------------------------------------------------------------- */
static int streq(const char* a, const char* b) {
  return a && b && strcmp(a, b) == 0;
}

static uint64_t parse_u64(const char* s, int* ok) {
  char* end = NULL;
  errno = 0;
#if defined(_MSC_VER)
  unsigned long long v = _strtoui64(s, &end, 10);
#else
  unsigned long long v = strtoull(s, &end, 10);
#endif
  if (errno || end == s || *end != '\0') {
    if (ok) *ok = 0;
    return 0;
  }
  if (ok) *ok = 1;
  return (uint64_t)v;
}
static uint64_t parse_size(const char* s, int* ok) {
  /* accepte suffixes k,m,g (base 1024) */
  if (!s || !*s) {
    if (ok) *ok = 0;
    return 0;
  }
  size_t n = strlen(s);
  char suf = (char)((n > 0) ? s[n - 1] : 0);
  uint64_t mul = 1;
  char buf[64];
  if ((suf == 'k' || suf == 'K' || suf == 'm' || suf == 'M' || suf == 'g' ||
       suf == 'G') &&
      n < sizeof buf) {
    memcpy(buf, s, n - 1);
    buf[n - 1] = 0;
    if (suf == 'k' || suf == 'K')
      mul = 1024ull;
    else if (suf == 'm' || suf == 'M')
      mul = 1024ull * 1024ull;
    else
      mul = 1024ull * 1024ull * 1024ull;
    s = buf;
  }
  int oknum = 0;
  uint64_t v = parse_u64(s, &oknum);
  if (!oknum) {
    if (ok) *ok = 0;
    return 0;
  }
  if (ok) *ok = 1;
  return v * mul;
}

static vt_log_level parse_level(const char* s, int* ok) {
  if (!s) {
    if (ok) *ok = 0;
    return VT_LL_INFO;
  }
  if (!strcasecmp(s, "trace")) {
    if (ok) *ok = 1;
    return VT_LL_TRACE;
  }
  if (!strcasecmp(s, "debug")) {
    if (ok) *ok = 1;
    return VT_LL_DEBUG;
  }
  if (!strcasecmp(s, "info")) {
    if (ok) *ok = 1;
    return VT_LL_INFO;
  }
  if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) {
    if (ok) *ok = 1;
    return VT_LL_WARN;
  }
  if (!strcasecmp(s, "error")) {
    if (ok) *ok = 1;
    return VT_LL_ERROR;
  }
  if (!strcasecmp(s, "fatal")) {
    if (ok) *ok = 1;
    return VT_LL_FATAL;
  }
  if (ok) *ok = 0;
  return VT_LL_INFO;
}

static vt_log_format parse_format(const char* s, int* ok) {
  if (!s) {
    if (ok) *ok = 0;
    return VT_FMT_TEXT;
  }
  if (!strcasecmp(s, "text")) {
    if (ok) *ok = 1;
    return VT_FMT_TEXT;
  }
  if (!strcasecmp(s, "json")) {
    if (ok) *ok = 1;
    return VT_FMT_JSON;
  }
  if (ok) *ok = 0;
  return VT_FMT_TEXT;
}

static int parse_color(const char* s, int* auto_mode, int* ok) {
  /* renvoie 0/1 pour off/on, auto_mode=1 si auto */
  if (!s) {
    if (ok) *ok = 0;
    return 1;
  }
  if (!strcasecmp(s, "auto")) {
    if (auto_mode) *auto_mode = 1;
    if (ok) *ok = 1;
    return 1;
  }
  if (!strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcasecmp(s, "1")) {
    if (auto_mode) *auto_mode = 0;
    if (ok) *ok = 1;
    return 1;
  }
  if (!strcasecmp(s, "off") || !strcasecmp(s, "false") || !strcasecmp(s, "0")) {
    if (auto_mode) *auto_mode = 0;
    if (ok) *ok = 1;
    return 0;
  }
  if (ok) *ok = 0;
  return 1;
}

static void usage(const char* prog) {
  fprintf(stderr,
          "Usage: %s [options]\n"
          "Options:\n"
          "  --level <trace|debug|info|warn|error|fatal>\n"
          "  --format <text|json>\n"
          "  --color  <auto|on|off>\n"
          "  --file   <path>            # log vers fichier\n"
          "  --rotate <N|Nk|Nm|Ng>      # rotation par taille\n"
          "  --crash-handlers           # installe les handlers\n"
          "  --backtrace                # imprime une backtrace\n"
          "  --hexdump <file>           # hexdump du fichier\n"
          "  --bench <N> [--message S]  # émet N lignes niveau INFO\n"
          "  --emit-sample              # émet TRACE..FATAL\n"
          "  --fatal                    # déclenche FATAL (abort)\n"
          "  --json / --text            # alias de --format\n"
          "  -h | --help\n",
          prog);
}

/* ----------------------------------------------------------------------------
   Actions
---------------------------------------------------------------------------- */
static int read_file(const char* path, unsigned char** out, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long L = ftell(f);
  if (L < 0) {
    fclose(f);
    return -1;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  unsigned char* buf = (unsigned char*)malloc((size_t)L);
  if (!buf) {
    fclose(f);
    return -2;
  }
  size_t rd = fread(buf, 1, (size_t)L, f);
  fclose(f);
  if (rd != (size_t)L) {
    free(buf);
    return -1;
  }
  *out = buf;
  *out_len = rd;
  return 0;
}

static void do_emit_sample(void) {
  VT_TRACE("trace message");
  VT_DEBUG("debug message x=%d", 7);
  VT_INFO("info message");
  VT_WARN("warn message");
  VT_ERROR("error message");
  /* VT_FATAL termine le process, on ne l'émet pas ici. */
}

static void do_bench(uint64_t n, const char* msg) {
  if (n == 0) return;
  for (uint64_t i = 0; i < n; i++) {
    VT_INFO("%s #%llu", msg ? msg : "bench", (unsigned long long)i);
  }
  vt_log_force_flush();
}

/* ----------------------------------------------------------------------------
   main
---------------------------------------------------------------------------- */
int main(int argc, char** argv) {
  vt_log_config cfg = {.level = VT_LL_INFO,
                       .format = VT_FMT_TEXT,
                       .use_color = 1,
                       .file_path = NULL,
                       .rotate_bytes = 0,
                       .capture_crash = 0};

  /* parsing simple */
  const char* hexdump_path = NULL;
  int do_bt = 0, do_samples = 0, do_fatal = 0;
  uint64_t bench_n = 0;
  const char* bench_msg = NULL;

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (streq(a, "-h") || streq(a, "--help")) {
      usage(argv[0]);
      return 0;
    } else if (streq(a, "--level") && i + 1 < argc) {
      int ok = 0;
      cfg.level = parse_level(argv[++i], &ok);
      if (!ok) {
        fprintf(stderr, "Invalid --level\n");
        return 2;
      }
    } else if (streq(a, "--format") && i + 1 < argc) {
      int ok = 0;
      cfg.format = parse_format(argv[++i], &ok);
      if (!ok) {
        fprintf(stderr, "Invalid --format\n");
        return 2;
      }
    } else if (streq(a, "--json")) {
      cfg.format = VT_FMT_JSON;
    } else if (streq(a, "--text")) {
      cfg.format = VT_FMT_TEXT;
    } else if (streq(a, "--color") && i + 1 < argc) {
      int ok = 0, is_auto = 0;
      int on = parse_color(argv[++i], &is_auto, &ok);
      if (!ok) {
        fprintf(stderr, "Invalid --color\n");
        return 2;
      }
      cfg.use_color = on; /* 'auto' sera géré côté lib via isatty */
      /* NB: debug.c active déjà auto via isatty; ici on ne force que la
       * préférence. */
    } else if (streq(a, "--file") && i + 1 < argc) {
      cfg.file_path = argv[++i];
    } else if (streq(a, "--rotate") && i + 1 < argc) {
      int ok = 0;
      uint64_t sz = parse_size(argv[++i], &ok);
      if (!ok) {
        fprintf(stderr, "Invalid --rotate\n");
        return 2;
      }
      cfg.rotate_bytes = (size_t)sz;
    } else if (streq(a, "--crash-handlers")) {
      cfg.capture_crash = 1;
    } else if (streq(a, "--backtrace")) {
      do_bt = 1;
    } else if (streq(a, "--hexdump") && i + 1 < argc) {
      hexdump_path = argv[++i];
    } else if (streq(a, "--bench") && i + 1 < argc) {
      int ok = 0;
      bench_n = parse_u64(argv[++i], &ok);
      if (!ok) {
        fprintf(stderr, "Invalid --bench\n");
        return 2;
      }
    } else if (streq(a, "--message") && i + 1 < argc) {
      bench_msg = argv[++i];
    } else if (streq(a, "--emit-sample")) {
      do_samples = 1;
    } else if (streq(a, "--fatal")) {
      do_fatal = 1;
    } else {
      fprintf(stderr, "Unknown arg: %s\n", a);
      usage(argv[0]);
      return 2;
    }
  }

  /* init logger */
  if (vt_log_init(&cfg) != 0) {
    fprintf(stderr, "vt_log_init failed\n");
    return 3;
  }

  VT_INFO(
      "logger ready | level=%d format=%d color=%d file=%s rotate=%zu "
      "capture_crash=%d",
      (int)cfg.level, (int)cfg.format, cfg.use_color,
      cfg.file_path ? cfg.file_path : "<stderr>", cfg.rotate_bytes,
      cfg.capture_crash);

  if (do_samples) {
    do_emit_sample();
  }

  if (bench_n) {
    VT_INFO("bench start: %llu lines", (unsigned long long)bench_n);
    do_bench(bench_n, bench_msg);
    VT_INFO("bench end");
  }

  if (hexdump_path) {
    unsigned char* buf = NULL;
    size_t len = 0;
    int rc = read_file(hexdump_path, &buf, &len);
    if (rc == 0) {
      VT_INFO("hexdump file='%s' bytes=%zu", hexdump_path, len);
      vt_debug_hexdump(buf, len, hexdump_path);
      free(buf);
    } else {
      VT_ERROR("read_file('%s') failed rc=%d errno=%d", hexdump_path, rc,
               errno);
    }
  }

  if (do_bt) {
    VT_WARN("printing backtrace on demand");
    vt_debug_backtrace();
  }

  if (do_fatal) {
    VT_FATAL("fatal requested by --fatal");
    /* vt_log_write(VT_LL_FATAL,...) appelle abort() dans la lib */
  }

  VT_INFO("done");
  vt_log_shutdown();
  return 0;
}
