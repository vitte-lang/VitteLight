// SPDX-License-Identifier: MIT
/* ============================================================================
   /core/code.c — Implémentation « ultra complète » de l’API CLI
   Dépend de /core/api.h et /core/code.h
   Build (exécutable autonome) :
     cc -O2 -std=c99 core/api.c core/code.c -DCODE_STANDALONE -o vitte-cli
   ============================================================================
 */

#include "core/code.h"
#include "core/api.h"   // pour Err, StrBuf, vec_init, vec_push, etc.
#include "core/utf8.h"  // utf8_decode_1

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Vecteurs typés */
typedef VEC(u8)     vec_u8;
typedef VEC(u32)    vec_u32;
typedef VEC(char)   vec_char;
typedef VEC(CodeKV) vec_CodeKV; /* <- important si non défini dans code.h */

/* Accès interne à la hash-map pour itération */
struct HSlot {
  char* key;
  u64 val;
  u64 hash;
  bool used;
};

/* -------------------------------------------------------------------------- */
/* Utils locaux */
/* -------------------------------------------------------------------------- */
static char* xstrdup(const char* s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char* p = (char*)malloc(n);
  if (!p) { fprintf(stderr, "OOM\n"); abort(); }
  memcpy(p, s, n);
  return p;
}
static int cmp_kv_desc(const void* a, const void* b) {
  const CodeKV *x = (const CodeKV*)a, *y = (const CodeKV*)b;
  if (x->count < y->count) return 1;
  if (x->count > y->count) return -1;
  if (!x->word && !y->word) return 0;
  if (!x->word) return 1;
  if (!y->word) return -1;
  return strcmp(x->word, y->word);
}
static int is_word_cp(u32 cp) {
  if (cp == '_' || (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
      (cp >= 'a' && cp <= 'z')) return 1;
  if (cp >= 128) return 1;
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Aide et info */
/* -------------------------------------------------------------------------- */
void code_usage(FILE* out) {
  fprintf(out,
          "%s %s\n"
          "Usage:\n"
          "  app help                         Aide\n"
          "  app info                         Infos runtime\n"
          "  app rand [N]                     N aléas\n"
          "  app hash <fichier>               hash64 d’un fichier\n"
          "  app cat <fichier>                cat numéroté\n"
          "  app json [out.json]              JSON de démo\n"
          "  app utf8 <texte>                 liste des codepoints\n"
          "  app freq <fichier> [topK]        fréquences des mots\n"
          "  app bench [bytes] [iters]        bench hash64\n"
          "  app ansi <texte>                 sortie colorée\n"
          "  app demo                         démonstration\n",
          CODE_APP_NAME, CODE_APP_VERSION);
}
void code_print_info(void) {
  log_set_color(true);
  logf(LOG_INFO, "app=%s v=%s", CODE_APP_NAME, CODE_APP_VERSION);
  logf(LOG_INFO, "wall_ms=%llu", (unsigned long long)time_ms_wall());
  logf(LOG_INFO, "mono_ns=%llu", (unsigned long long)time_ns_monotonic());
  const char* u = env_get("USER");
  if (!u) u = env_get("USERNAME");
  logf(LOG_INFO, "user=%s", u ? u : "<unknown>");
#if defined(_WIN32)
  logf(LOG_INFO, "os=windows");
#elif defined(__APPLE__)
  logf(LOG_INFO, "os=macos");
#else
  logf(LOG_INFO, "os=linux");
#endif
}

/* -------------------------------------------------------------------------- */
/* Commandes unitaires */
/* -------------------------------------------------------------------------- */
Err code_cmd_rand_to_strbuf(int n, StrBuf* out) {
  if (n <= 0) n = 5;
  if (!out) return api_errf(CODE_EINVAL, "nil out");
  for (int i = 0; i < n; i++)
    sb_append_fmt(out, "%llu\n", (unsigned long long)rand_u64());
  return api_ok();
}

Err code_hash_file(const char* path, u64* out_hash) {
  if (!path || !out_hash) return api_errf(CODE_EINVAL, "args");
  vec_u8 buf;
  Err e = file_read_all(path, &buf);
  if (e.code) return e;
  *out_hash = hash64(buf.data, buf.len);
  vec_free(&buf);
  return api_ok();
}

Err code_cat_file_numbered(const char* path, StrBuf* out) {
  if (!path || !out) return api_errf(CODE_EINVAL, "args");
  vec_u8 buf;
  Err e = file_read_all(path, &buf);
  if (e.code) return e;
  usize i = 0;
  int ln = 1;
  while (i < buf.len) {
    usize s = i;
    while (i < buf.len && buf.data[i] != '\n') i++;
    usize len = i - s;
    sb_append_fmt(out, "%s%5d%s  ", ansi_blue(), ln, ansi_reset());
    sb_append_n(out, (const char*)buf.data + s, len);
    sb_append(out, "\n");
    ln++;
    i += (i < buf.len && buf.data[i] == '\n') ? 1 : 0;
  }
  vec_free(&buf);
  return api_ok();
}

Err code_emit_demo_json(int argc, char** argv, const char* out_path,
                        StrBuf* out) {
  if (!out) return api_errf(CODE_EINVAL, "nil out");
  JsonW jw;
  jw_begin(&jw);
  jw_obj_begin(&jw);
  jw_key(&jw, "app");     jw_str(&jw, CODE_APP_NAME);
  jw_key(&jw, "version"); jw_str(&jw, CODE_APP_VERSION);
  jw_key(&jw, "time_ms"); jw_i64(&jw, (i64)time_ms_wall());
  jw_key(&jw, "rand");    jw_i64(&jw, (i64)(rand_u64() & 0xffffffffu));
  jw_key(&jw, "args");    jw_arr_begin(&jw);
  for (int i = 0; i < argc; i++) jw_str(&jw, argv[i]);
  jw_arr_end(&jw);
  jw_key(&jw, "env_user");
  jw_str(&jw, env_get("USER")
             ? env_get("USER")
             : (env_get("USERNAME") ? env_get("USERNAME") : "unknown"));
  jw_obj_end(&jw);
  sb_append(out, jw_cstr(&jw));
  if (out_path) {
    Err e = file_write_all(out_path, jw_cstr(&jw), strlen(jw_cstr(&jw)));
    jw_free(&jw);
    if (e.code) return e;
  } else {
    jw_free(&jw);
  }
  return api_ok();
}

Err code_utf8_list_cps(const char* s, vec_u32* out) {
  if (!s || !out) return api_errf(CODE_EINVAL, "args");
  vec_init(out);
  size_t n = strlen(s), i = 0;
  while (i < n) {
    size_t adv = 0;
    u32 cp = utf8_decode_1(s + i, n - i, &adv);
    if (adv == 0) {
      vec_push(out, (u32)(unsigned char)s[i]);
      i += 1;
    } else {
      vec_push(out, cp);
      i += adv;
    }
  }
  return api_ok();
}

Err code_freq_pairs(const char* path, vec_CodeKV* out_pairs) {
  if (!path || !out_pairs) return api_errf(CODE_EINVAL, "args");
  vec_u8 file;
  Err e = file_read_all(path, &file);
  if (e.code) return e;

  MapStrU64 map;
  map_init(&map);
  usize i = 0;
  while (i < file.len) {
    char tmp[256];
    int ti = 0, seen = 0;
    for (;;) {
      if (i >= file.len) break;
      usize adv = 0;
      u32 cp = utf8_decode_1((const char*)file.data + i, file.len - i, &adv);
      if (!is_word_cp(cp) || ti >= 255) {
        if (seen) { i += adv ? adv : 1; break; }
        else { i += adv ? adv : 1; continue; }
      }
      tmp[ti++] = (cp < 128) ? (char)cp : '_';
      seen = 1;
      i += adv ? adv : 1;
    }
    if (ti > 0) {
      tmp[ti] = 0;
      u64 v = 0;
      if (map_get(&map, tmp, &v)) map_put(&map, tmp, v + 1);
      else map_put(&map, tmp, 1);
    }
  }

  vec_init(out_pairs);
  for (usize s = 0; s < map.cap; s++) {
    struct HSlot* slot = &map.slots[s];
    if (slot->used) {
      CodeKV kv;
      kv.word = xstrdup(slot->key);
      kv.count = slot->val;
      vec_push(out_pairs, kv);
    }
  }

  map_free(&map);
  vec_free(&file);
  return api_ok();
}

void code_freq_sort_desc(vec_CodeKV* xs) {
  if (!xs || xs->len == 0) return;
  qsort(xs->data, xs->len, sizeof(CodeKV), cmp_kv_desc);
}

Err code_bench_hash64(size_t bytes, int iters, CodeBench* out) {
  if (bytes == 0 || iters <= 0 || !out) return api_errf(CODE_EINVAL, "args");
  vec_u8 buf;
  vec_init(&buf);
  vec_reserve(&buf, bytes);
  buf.len = bytes;
  for (size_t i = 0; i < bytes; i++) buf.data[i] = (u8)(rand_u64() & 0xFF);

  u64 t0 = time_ns_monotonic();
  u64 acc = 0;
  for (int i = 0; i < iters; i++) acc ^= hash64(buf.data, buf.len);
  u64 t1 = time_ns_monotonic();

  double sec = (t1 - t0) / 1e9;
  double gib = ((double)bytes * (double)iters) / (1024.0 * 1024.0 * 1024.0);
  out->seconds = sec;
  out->gib = gib;
  out->gib_per_s = gib / sec;
  out->accumulator = acc;

  vec_free(&buf);
  return api_ok();
}

Err code_ansi_render(const char* text, StrBuf* out) {
  if (!text || !out) return api_errf(CODE_EINVAL, "args");
  ansi_paint_to(out, text, ansi_green());
  ansi_paint_to(out, " ", ansi_reset());
  ansi_paint_to(out, "[bold]", ansi_bold());
  return api_ok();
}

/* -------------------------------------------------------------------------- */
/* Démo */
/* -------------------------------------------------------------------------- */
void code_demo(void) {
  code_print_info();
  char out_path[512];
  path_join("out", "demo.txt", out_path, sizeof(out_path));
  (void)dir_ensure("out");

  StrBuf js;
  sb_init(&js);
  (void)code_emit_demo_json(0, NULL, "out/demo.json", &js);
  puts(js.data ? js.data : "");
  sb_free(&js);

  const char* txt = "Hello demo\nLine 2\n";
  (void)file_write_all(out_path, txt, strlen(txt));
  u64 h = 0;
  if (!code_hash_file(out_path, &h).code)
    printf("%016llx  %s\n", (unsigned long long)h, out_path);
  StrBuf cat;
  sb_init(&cat);
  if (!code_cat_file_numbered(out_path, &cat).code)
    fputs(cat.data ? cat.data : "", stdout);
  sb_free(&cat);

  StrBuf ansi;
  sb_init(&ansi);
  (void)code_ansi_render("Bonjour", &ansi);
  puts(ansi.data ? ansi.data : "");
  sb_free(&ansi);
}

/* -------------------------------------------------------------------------- */
/* Parsing commande et boucle CLI */
/* -------------------------------------------------------------------------- */
bool code_cmd_parse(const char* s, CodeCmd* out_cmd) {
  if (!s || !out_cmd) return false;
  struct { const char* n; CodeCmd c; } map[] = {
    {"help", CMD_HELP}, {"info", CMD_INFO}, {"rand", CMD_RAND},
    {"hash", CMD_HASH}, {"cat", CMD_CAT}, {"json", CMD_JSON},
    {"utf8", CMD_UTF8}, {"freq", CMD_FREQ}, {"bench", CMD_BENCH},
    {"ansi", CMD_ANSI}, {"demo", CMD_DEMO}
  };
  for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
    if (strcmp(s, map[i].n) == 0) { *out_cmd = map[i].c; return true; }
  }
  return false;
}

int code_main(int argc, char** argv) {
  if (argc < 2) { code_usage(stdout); return 0; }
  CodeCmd cmd;
  if (!code_cmd_parse(argv[1], &cmd)) { code_usage(stderr); return 1; }

  switch (cmd) {
    case CMD_HELP:  code_usage(stdout); return 0;
    case CMD_INFO:  code_print_info();  return 0;

    case CMD_RAND: {
      int n = (argc >= 3) ? atoi(argv[2]) : 5;
      StrBuf sb; sb_init(&sb);
      Err e = code_cmd_rand_to_strbuf(n, &sb);
      if (e.code) { logf(LOG_ERROR, "%s", e.msg); sb_free(&sb); return code_status_from_err(&e); }
      fputs(sb.data ? sb.data : "", stdout);
      sb_free(&sb); return 0;
    }

    case CMD_HASH: {
      if (argc < 3) { logf(LOG_ERROR, "hash: besoin d’un chemin"); return CODE_EINVAL; }
      u64 h = 0; Err e = code_hash_file(argv[2], &h);
      if (e.code) { logf(LOG_ERROR, "%s", e.msg); return code_status_from_err(&e); }
      printf("%016llx  %s\n", (unsigned long long)h, argv[2]); return 0;
    }

    case CMD_CAT: {
      if (argc < 3) { logf(LOG_ERROR, "cat: besoin d’un chemin"); return CODE_EINVAL; }
      StrBuf sb; sb_init(&sb);
      Err e = code_cat_file_numbered(argv[2], &sb);
      if (e.code) { logf(LOG_ERROR, "%s", e.msg); sb_free(&sb); return code_status_from_err(&e); }
      fputs(sb.data ? sb.data : "", stdout);
      sb_free(&sb); return 0;
    }

    case CMD_JSON: {
      const char* out = (argc >= 3) ? argv[2] : NULL;
      StrBuf sb; sb_init(&sb);
      Err e = code_emit_demo_json(argc, argv, out, &sb);
      if (e.code) { logf(LOG_ERROR, "%s", e.msg); sb_free(&sb); return code_status_from_err(&e); }
      if (!out) fputs(sb.data ? sb.data : "", stdout);
      sb_free(&sb); return 0;
    }

    case CMD_UTF8: {
      if (argc < 3) { logf(LOG_ERROR, "utf8: besoin d’un texte"); return CODE_EINVAL; }
      vec_u32 cps; Err e = code_utf8_list_cps(argv[2], &cps);
      if (e.code) { logf(LOG_ERROR, "%s", e.msg); return code_status_from_err(&e); }
      for (usize i = 0; i < cps.len; i++) printf("U+%04X\n", (unsigned)cps.data[i]);
      vec_free(&cps); return 0;
    }

    case CMD_FREQ: {
      if (argc < 3) { logf(LOG_ERROR, "freq: besoin d’un fichier"); return CODE_EINVAL; }
      int topk = (argc >= 4) ? atoi(argv[3]) : 20;
      vec_CodeKV xs; Err e = code_freq_pairs(argv[2], &xs);
      if (e.code) { logf(LOG_ERROR, "%s", e.msg); return code_status_from_err(&e); }
      code_freq_sort_desc(&xs);
      int limit = (topk > 0 && (usize)topk < xs.len) ? topk : (int)xs.len;
      for (int i = 0; i < limit; i++) {
        printf("%8llu  %s\n", (unsigned long long)xs.data[i].count,
               xs.data[i].word ? xs.data[i].word : "");
        free((void*)xs.data[i].word);
      }
      vec_free(&xs); return 0;
    }

    case CMD_BENCH: {
      size_t bytes = (argc >= 3) ? (size_t)strtoull(argv[2], NULL, 10) : ((size_t)1 << 20);
      int iters = (argc >= 4) ? atoi(argv[3]) : 200;
      CodeBench r; Err e = code_bench_hash64(bytes, iters, &r);
      if (e.code) { logf(LOG_ERROR, "%s", e.msg); return code_status_from_err(&e); }
      printf("hash64: %.3fs, %.2f GiB, %.2f GiB/s, acc=%016llx\n",
             r.seconds, r.gib, r.gib_per_s, (unsigned long long)r.accumulator);
      return 0;
    }

    case CMD_ANSI: {
      if (argc < 3) { logf(LOG_ERROR, "ansi: besoin d’un texte"); return CODE_EINVAL; }
      StrBuf sb; sb_init(&sb);
      Err e = code_ansi_render(argv[2], &sb);
      if (e.code) { logf(LOG_ERROR, "%s", e.msg); sb_free(&sb); return code_status_from_err(&e); }
      puts(sb.data ? sb.data : "");
      sb_free(&sb); return 0;
    }

    case CMD_DEMO:
      code_demo(); return 0;
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* main optionnel */
/* -------------------------------------------------------------------------- */
#ifdef CODE_STANDALONE
int main(int argc, char** argv) { return code_main(argc, argv); }
#endif