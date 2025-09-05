/* ============================================================================
   /core/code.h — En-tête « ultra complet » pour l’API CLI
   Dépend de /core/api.h. Compatible C/C++.
   ============================================================================
 */
#ifndef CORE_CODE_H
#define CORE_CODE_H

#include <stdbool.h> /* bool */
#include <stddef.h>  /* size_t */
#include <stdint.h>  /* uint*_t */
#include <stdio.h>   /* FILE */

#include "core/api.h" /* Err, StrBuf, VEC, u64, u32, etc. */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Version */
/* -------------------------------------------------------------------------- */
#ifndef CODE_APP_NAME
#define CODE_APP_NAME "vitte-cli"
#endif
#ifndef CODE_APP_VERSION
#define CODE_APP_VERSION "1.0.0"
#endif

/* -------------------------------------------------------------------------- */
/* Statuts, commandes, structures */
/* -------------------------------------------------------------------------- */
typedef enum CodeStatus {
  CODE_OK = 0,
  CODE_EINVAL = 1,
  CODE_EIO = 2,
  CODE_EINTERNAL = 3
} CodeStatus;

typedef enum CodeCmd {
  CMD_HELP = 0,
  CMD_INFO,
  CMD_RAND,
  CMD_HASH,
  CMD_CAT,
  CMD_JSON,
  CMD_UTF8,
  CMD_FREQ,
  CMD_BENCH,
  CMD_ANSI,
  CMD_DEMO
} CodeCmd;

typedef struct CodeKV {
  const char* word;
  u64 count;
} CodeKV;

typedef struct CodeBench {
  double seconds;
  double gib;
  double gib_per_s;
  u64 accumulator;
} CodeBench;

/* Mappe Err → CodeStatus sans modifier Err. */
static inline CodeStatus code_status_from_err(const Err* e) {
  return (e && e->code != 0)
             ? ((e->code > 0 && e->code <= 255) ? (CodeStatus)e->code
                                                : CODE_EINTERNAL)
             : CODE_OK;
}

/* -------------------------------------------------------------------------- */
/* Aide et informations */
/* -------------------------------------------------------------------------- */
void code_usage(FILE* out);
void code_print_info(void);

/* -------------------------------------------------------------------------- */
/* API « unitaires » */
/* -------------------------------------------------------------------------- */
Err code_cmd_rand_to_strbuf(int n, StrBuf* out);
Err code_hash_file(const char* path, u64* out_hash);
Err code_cat_file_numbered(const char* path, StrBuf* out);
Err code_emit_demo_json(int argc, char** argv, const char* out_path,
                        StrBuf* out);
Err code_utf8_list_cps(const char* s, VEC(u32) * out_codepoints);
Err code_freq_pairs(const char* path, VEC(CodeKV) * out_pairs);
void code_freq_sort_desc(VEC(CodeKV) * xs);
Err code_bench_hash64(size_t bytes, int iters, CodeBench* out);
Err code_ansi_render(const char* text, StrBuf* out);
void code_demo(void);

/* -------------------------------------------------------------------------- */
/* CLI */
/* -------------------------------------------------------------------------- */
bool code_cmd_parse(const char* s, CodeCmd* out_cmd);
int code_main(int argc, char** argv);

/* -------------------------------------------------------------------------- */
/* Helpers header-only */
/* -------------------------------------------------------------------------- */
static inline CodeStatus code_fprint_strbuf(FILE* f, const StrBuf* sb) {
  if (!sb || !sb->data) return CODE_OK;
  return (fwrite(sb->data, 1, sb->len, f) == sb->len) ? CODE_OK : CODE_EIO;
}

static inline void code_print_topk(const VEC(CodeKV) * xs, int topk) {
  int limit = (topk > 0 && (usize)topk < xs->len) ? topk : (int)xs->len;
  for (int i = 0; i < limit; i++) {
    printf("%8llu  %s\n", (unsigned long long)xs->data[i].count,
           xs->data[i].word ? xs->data[i].word : "");
  }
}

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_CODE_H */
