// SPDX-License-Identifier: MIT
/* ============================================================================
   /core/code.h — En-tête pour l’API CLI Vitte/Vitl
   Dépend de /core/api.h (types, Err, VEC, StrBuf, etc.).
   ============================================================================ */
#ifndef CORE_CODE_H
#define CORE_CODE_H

#include "core/api.h"   /* Err, StrBuf, VEC, u32/u64, etc. */
#include <stdio.h>      /* FILE */
#include <stddef.h>     /* size_t */
#include <stdbool.h>    /* bool */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Version / nom d’app
---------------------------------------------------------------------------- */
#ifndef CODE_APP_NAME
#define CODE_APP_NAME "vitte-cli"
#endif
#ifndef CODE_APP_VERSION
#define CODE_APP_VERSION "1.0.0"
#endif

/* ----------------------------------------------------------------------------
   Statuts & commandes
---------------------------------------------------------------------------- */
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

/* ----------------------------------------------------------------------------
   Structures publiques
---------------------------------------------------------------------------- */
typedef struct CodeKV {
  const char* word;
  u64         count;
} CodeKV;

typedef struct CodeBench {
  double seconds;
  double gib;
  double gib_per_s;
  u64    accumulator;
} CodeBench;

/* Vecteurs typés basés sur le macro VEC(T) de api.h */
typedef VEC(u32)    vec_u32;
typedef VEC(u8)     vec_u8;
typedef VEC(CodeKV) vec_CodeKV;

/* ----------------------------------------------------------------------------
   Helpers
---------------------------------------------------------------------------- */
static inline CodeStatus code_status_from_err(const Err* e) {
  return (e && e->code != 0)
           ? ((e->code > 0 && e->code <= 255) ? (CodeStatus)e->code
                                              : CODE_EINTERNAL)
           : CODE_OK;
}

/* ----------------------------------------------------------------------------
   Aide / info
---------------------------------------------------------------------------- */
void code_usage(FILE* out);
void code_print_info(void);

/* ----------------------------------------------------------------------------
   API unitaires
---------------------------------------------------------------------------- */
Err  code_cmd_rand_to_strbuf(int n, StrBuf* out);
Err  code_hash_file(const char* path, u64* out_hash);
Err  code_cat_file_numbered(const char* path, StrBuf* out);
Err  code_emit_demo_json(int argc, char** argv, const char* out_path, StrBuf* out);

/* UTF-8 / fréquences (signatures avec vecteurs typés) */
Err  code_utf8_list_cps(const char* s, vec_u32* out_codepoints);
Err  code_freq_pairs(const char* path, vec_CodeKV* out_pairs);
void code_freq_sort_desc(vec_CodeKV* xs);

Err  code_bench_hash64(size_t bytes, int iters, CodeBench* out);
Err  code_ansi_render(const char* text, StrBuf* out);
void code_demo(void);

/* ----------------------------------------------------------------------------
   CLI
---------------------------------------------------------------------------- */
bool code_cmd_parse(const char* s, CodeCmd* out_cmd);
int  code_main(int argc, char** argv);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_CODE_H */