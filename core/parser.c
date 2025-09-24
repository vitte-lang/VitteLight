// SPDX-License-Identifier: MIT
/* ============================================================================
   parser.c — Stub minimal pour l’API parser.h

   Cette implémentation fournit des diagnostics explicites « non implémenté »
   afin de permettre au reste du runtime de se compiler et de détecter
   proprement l’absence de parser réel.
   ============================================================================ */

#include "parser.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VT_MEM_H_SENTINEL
/* Arène simplifiée utilisée uniquement pour stocker un diagnostic. */
typedef struct parser_arena {
  vt_diag diag;
  char*   msg;
} parser_arena;
#endif

/* -------------------------------------------------------------------------- */
static parser_arena_t* vt__arena_new(void) {
  parser_arena_t* arena = (parser_arena_t*)calloc(1, sizeof(parser_arena_t));
  return arena;
}

static void vt__arena_free(parser_arena_t* arena) {
  if (!arena) return;
#ifndef VT_MEM_H_SENTINEL
  parser_arena* a = (parser_arena*)arena;
  free(a->msg);
#endif
  free(arena);
}

static vt_parse_result vt__make_error(const char* file, const char* fmt, ...) {
  vt_parse_result res;
  memset(&res, 0, sizeof res);

  parser_arena_t* arena = vt__arena_new();
  if (!arena) return res;

#ifndef VT_MEM_H_SENTINEL
  parser_arena* a = (parser_arena*)arena;
  va_list ap;
  va_start(ap, fmt);
  int needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (needed < 0) {
    a->msg = NULL;
  } else {
    a->msg = (char*)malloc((size_t)needed + 1);
    if (a->msg) {
      va_start(ap, fmt);
      vsnprintf(a->msg, (size_t)needed + 1, fmt, ap);
      va_end(ap);
    }
  }
  a->diag.line = 1;
  a->diag.col  = 1;
  a->diag.file = file;
  a->diag.msg  = a->msg ? a->msg : (char*)"parser: not implemented";

  res.diags  = &a->diag;
  res.ndiags = 1;
#else
  (void)fmt; (void)file;
#endif
  res.arena = arena;
  res.module = NULL;
  return res;
}

/* -------------------------------------------------------------------------- */
vt_parse_result vt_parse_source(const char* source_utf8,
                                const char* filename_opt) {
  (void)source_utf8;
  return vt__make_error(filename_opt, "parser stub: not implemented");
}

vt_parse_result vt_parse_file(const char* path) {
  if (!path) return vt_parse_source(NULL, NULL);
  FILE* f = fopen(path, "rb");
  if (!f) {
    return vt__make_error(path, "cannot open file: %s", strerror(errno));
  }
  fclose(f);
  return vt__make_error(path, "parser stub: not implemented");
}

void vt_parse_free(vt_parse_result* result) {
  if (!result) return;
  vt__arena_free(result->arena);
  result->arena = NULL;
  result->module = NULL;
  result->diags = NULL;
  result->ndiags = 0;
}

void vt_ast_dump(FILE* out, vt_parse_result* result) {
  if (!out) out = stderr;
  if (!result || result->ndiags == 0) {
    fprintf(out, "<parser stub: no AST>\n");
    return;
  }
#ifndef VT_MEM_H_SENTINEL
  const vt_diag* d = result->diags;
  fprintf(out, "<parser stub> %s:%d:%d: %s\n",
          d->file ? d->file : "(input)", d->line, d->col,
          d->msg ? d->msg : "parser stub");
#else
  fprintf(out, "<parser stub: diagnostics unavailable>\n");
#endif
}
