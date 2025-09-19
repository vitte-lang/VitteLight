/* ============================================================================
   core/code.h — En-tête API CLI (C99/C11 compatible)
   Dépendances légères: core/api.h pour Err, StrBuf, u32/u64 et vecteurs.
   ============================================================================ */
#ifndef CORE_CODE_H
#define CORE_CODE_H

/* C standard */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Projet */
#include "core/api.h"   /* Err, StrBuf, u32/u64, macros VEC(...) éventuelles */

/* -------------------------------------------------------------------------- */
/* Compat C++                                                                 */
/* -------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Version                                                                    */
/* -------------------------------------------------------------------------- */
#ifndef CODE_APP_NAME
#define CODE_APP_NAME "vitte-cli"
#endif
#ifndef CODE_APP_VERSION
#define CODE_APP_VERSION "1.0.0"
#endif

/* -------------------------------------------------------------------------- */
/* Vecteurs: éviter les types anonymes dans l’interface publique              */
/* On forward-declare des types opaques compatibles avec un layout standard:  */
/*   struct vec_T { T* data; usize len; usize cap; }                          */
/* api.h doit fournir: typedef size_t usize;                                  */
/* -------------------------------------------------------------------------- */
struct vec_u32;      /* équiv. logique à VEC(u32)  */
struct vec_CodeKV;   /* équiv. logique à VEC(CodeKV) */

/* -------------------------------------------------------------------------- */
/* Statuts et commandes                                                       */
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

/* -------------------------------------------------------------------------- */
/* Structures de données                                                       */
/* -------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------- */
/* Helpers inline                                                              */
/* -------------------------------------------------------------------------- */
static inline CodeStatus code_status_from_err(const Err* e) {
  if (!e || e->code == 0) return CODE_OK;
  return (e->code > 0 && e->code <= 255) ? (CodeStatus)e->code : CODE_EINTERNAL;
}

static inline CodeStatus code_fprint_strbuf(FILE* f, const StrBuf* sb) {
  if (!sb || !sb->data) return CODE_OK;
  return (fwrite(sb->data, 1, sb->len, f) == sb->len) ? CODE_OK : CODE_EIO;
}

/* Affichage Top-K paires mot/fréquence.
   Layout attendu pour struct vec_CodeKV : { CodeKV* data; usize len; usize cap; } */
static inline void code_print_topk(const struct vec_CodeKV* xs, int topk) {
  if (!xs) return;
  const CodeKV* data = (const CodeKV*)((const void*)(((const char*)xs) + 0)); /* aliasing ok: doc layout */
  /* On suppose data/len/cap aux premiers champs dans cet ordre, comme VEC(T) */
  const CodeKV* arr = *(const CodeKV* const*)(&data); /* neutralisé ci-dessous — voir note */
  (void)arr; /* évite un warning si l’implémentation diffère */

  /* Reprend explicitement via champs nommés attendus */
  const CodeKV* a = ((const struct vec_CodeKV*)xs)->data;
  usize n         = ((const struct vec_CodeKV*)xs)->len;

  int limit = (topk > 0 && (usize)topk < n) ? topk : (int)n;
  for (int i = 0; i < limit; i++) {
    printf("%8llu  %s\n",
           (unsigned long long)a[i].count,
           a[i].word ? a[i].word : "");
  }
}

/* -------------------------------------------------------------------------- */
/* API « unitaires »                                                          */
/* -------------------------------------------------------------------------- */
void code_usage(FILE* out);
void code_print_info(void);

Err  code_cmd_rand_to_strbuf(int n, StrBuf* out);
Err  code_hash_file(const char* path, u64* out_hash);
Err  code_cat_file_numbered(const char* path, StrBuf* out);
Err  code_emit_demo_json(int argc, char** argv, const char* out_path, StrBuf* out);

/* Utiliser les types opaques plutôt que VEC(T) public */
Err  code_utf8_list_cps(const char* s, struct vec_u32* out_codepoints);
Err  code_freq_pairs(const char* path, struct vec_CodeKV* out_pairs);
void code_freq_sort_desc(struct vec_CodeKV* xs);

Err  code_bench_hash64(size_t bytes, int iters, CodeBench* out);
Err  code_ansi_render(const char* text, StrBuf* out);
void code_demo(void);

/* -------------------------------------------------------------------------- */
/* CLI                                                                        */
/* -------------------------------------------------------------------------- */
bool code_cmd_parse(const char* s, CodeCmd* out_cmd);
int  code_main(int argc, char** argv);

/* -------------------------------------------------------------------------- */
#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_CODE_H */