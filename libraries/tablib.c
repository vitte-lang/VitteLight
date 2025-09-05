// SPDX-License-Identifier: GPL-3.0-or-later
//
// tablib.c — Tabular data library for Vitte Light VM (C17, complet)
// Namespace: "tab"
//
// Modèle: table en mémoire, lignes dynamiques, colonnes nommées, cellules
// typées. Types cellule: 0=nil, 1=int64, 2=float64, 3=text(UTF-8 opaque).
//
// Gestion par identifiants entiers (slots), pas d’userdata VM.
//
// API:
//   -- Lifecycle
//     tab.new()                              -> id | (nil, errmsg)
//     tab.free(id)                           -> true
//     tab.clear(id)                          -> true
//     tab.reserve(id, rows:int, cols:int)    -> true | (nil, errmsg)
//     tab.clone(id)                          -> newid | (nil, errmsg)
//
//   -- Dimensions / colonnes
//     tab.nrows(id)                          -> int
//     tab.ncols(id)                          -> int
//     tab.columns_csv(id[, sep=","])         -> string
//     tab.add_col(id, name)                  -> colIndex | (nil, errmsg)     //
//     append at end tab.insert_col(id, at:int, name)       -> true | (nil,
//     errmsg) tab.drop_col(id, col)                  -> true | (nil, errmsg)
//     tab.rename_col(id, col, newname)       -> true | (nil, errmsg)
//     tab.col_index(id, name)                -> int (0 if not found)
//
//   -- Lignes
//     tab.append_row(id)                     -> rowIndex | (nil, errmsg)
//     tab.insert_row(id, at:int)             -> true | (nil, errmsg)
//     tab.drop_row(id, row)                  -> true | (nil, errmsg)
//
//   -- Accès cellules (1-based row/col)
//     tab.get(id, row, col)                  -> nil|int|float|string
//     tab.set_null(id, row, col)             -> true | (nil, errmsg)
//     tab.set_int(id, row, col, v:int64)     -> true | (nil, errmsg)
//     tab.set_float(id, row, col, v:number)  -> true | (nil, errmsg)
//     tab.set_text(id, row, col, s:string)   -> true | (nil, errmsg)
//
//   -- Recherche / tri
//     tab.find_str(id, col, needle[, nocase=false]) -> row:int (0 if not found)
//     tab.find_int(id, col, v:int64)                -> row:int
//     tab.find_float(id, col, v:number[, eps=0])    -> row:int
//     tab.sort_by(id, col[, numeric=false[, desc=false[, na_last=true]]]) ->
//     true | (nil, errmsg)
//
//   -- Import/Export CSV (RFC 4180-like, CRLF/ LF, quote="")
//     tab.to_csv(id[, sep=",", header=true])         -> csv:string
//     tab.from_csv(id, csv[, sep=",", header=true])  -> true | (nil, errmsg)
//     tab.set_header_from_first_row(id)              -> true | (nil, errmsg)
//
//   -- Export JSON Lines (une ligne par objet; valeurs nil -> null;
//   int/float/text)
//     tab.to_jsonl(id)                               -> string
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

// ------------------------------------------------------------
// VM helpers
// ------------------------------------------------------------
static const char *tb_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t tb_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static double tb_check_num(VL_State *S, int idx) {
  VL_Value *v = vl_get(S, idx);
  if (!v) {
    vl_errorf(S, "argument #%d: number expected", idx);
    return vl_error(S);
  }
  return vl_tonumber(S, v);
}
static int tb_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int tb_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)tb_check_int(S, idx);
  return defv;
}
static char tb_opt_sep(VL_State *S, int idx, char defc) {
  if (!vl_get(S, idx)) return defc;
  if (vl_isstring(S, idx)) {
    const char *p = tb_check_str(S, idx);
    return p && *p ? p[0] : defc;
  }
  return defc;
}

// ------------------------------------------------------------
// Core model
// ------------------------------------------------------------
typedef enum { TB_NIL = 0, TB_INT = 1, TB_FLOAT = 2, TB_TEXT = 3 } TbType;
typedef struct {
  TbType t;
  union {
    int64_t i;
    double f;
    char *s;
  } v;
} Cell;

typedef struct {
  int used;
  size_t nrows, ncols;
  size_t cap_rows, cap_cols;
  char **colnames;  // ncols
  Cell *cells;      // cap_rows * cap_cols
} Table;

static Table *g_tab = NULL;
static int g_cap = 0;

static int ensure_tab_cap(int need) {
  if (need <= g_cap) return 1;
  int ncap = g_cap ? g_cap : 16;
  while (ncap < need) ncap <<= 1;
  Table *nt = (Table *)realloc(g_tab, (size_t)ncap * sizeof *nt);
  if (!nt) return 0;
  for (int i = g_cap; i < ncap; i++) {
    nt[i].used = 0;
    nt[i].nrows = nt[i].ncols = 0;
    nt[i].cap_rows = nt[i].cap_cols = 0;
    nt[i].colnames = NULL;
    nt[i].cells = NULL;
  }
  g_tab = nt;
  g_cap = ncap;
  return 1;
}
static int alloc_slot(void) {
  for (int i = 1; i < g_cap; i++)
    if (!g_tab[i].used) return i;
  if (!ensure_tab_cap(g_cap ? g_cap * 2 : 16)) return 0;
  for (int i = 1; i < g_cap; i++)
    if (!g_tab[i].used) return i;
  return 0;
}

static void cell_free(Cell *c) {
  if (!c) return;
  if (c->t == TB_TEXT && c->v.s) {
    free(c->v.s);
    c->v.s = NULL;
  }
  c->t = TB_NIL;
}
static void cell_set_text(Cell *c, const char *s) {
  cell_free(c);
  if (!s) {
    c->t = TB_NIL;
    return;
  }
  size_t n = strlen(s);
  char *d = (char *)malloc(n + 1);
  if (!d) {
    c->t = TB_NIL;
    return;
  }
  memcpy(d, s, n + 1);
  c->t = TB_TEXT;
  c->v.s = d;
}
static void cell_copy(Cell *dst, const Cell *src) {
  cell_free(dst);
  dst->t = src->t;
  if (src->t == TB_TEXT) {
    if (src->v.s) {
      size_t n = strlen(src->v.s);
      dst->v.s = (char *)malloc(n + 1);
      if (dst->v.s)
        memcpy(dst->v.s, src->v.s, n + 1);
      else
        dst->t = TB_NIL;
    } else
      dst->v.s = NULL;
  } else if (src->t == TB_INT)
    dst->v.i = src->v.i;
  else if (src->t == TB_FLOAT)
    dst->v.f = src->v.f;
}
static int ensure_colcap(Table *T, size_t cap_cols) {
  if (cap_cols <= T->cap_cols) return 1;
  size_t ncap = T->cap_cols ? T->cap_cols : 4;
  while (ncap < cap_cols) ncap <<= 1;
  // realloc colnames
  char **nc = (char **)realloc(T->colnames, ncap * sizeof *nc);
  if (!nc) return 0;
  for (size_t i = T->cap_cols; i < ncap; i++) nc[i] = NULL;
  T->colnames = nc;
  // realloc cells with new row stride
  size_t old_cap_cols = T->cap_cols ? T->cap_cols : cap_cols;  // handle 0
  if (T->cells == NULL) {
    T->cells =
        (Cell *)calloc((T->cap_rows ? T->cap_rows : 1) * ncap, sizeof(Cell));
    if (!T->cells) return 0;
    T->cap_cols = ncap;
    return 1;
  }
  Cell *ncells =
      (Cell *)calloc((T->cap_rows ? T->cap_rows : 1) * ncap, sizeof(Cell));
  if (!ncells) return 0;
  // move old data row by row
  for (size_t r = 0; r < T->cap_rows; r++) {
    for (size_t c = 0; c < T->ncols && c < old_cap_cols; c++) {
      Cell *src = &T->cells[r * old_cap_cols + c];
      Cell *dst = &ncells[r * ncap + c];
      if (r < T->nrows) cell_copy(dst, src);
    }
  }
  // free old cells
  for (size_t r = 0; r < T->nrows; r++) {
    for (size_t c = T->ncols; c < old_cap_cols; c++) {
      Cell *src = &T->cells[r * old_cap_cols + c];
      cell_free(src);
    }
  }
  free(T->cells);
  T->cells = ncells;
  T->cap_cols = ncap;
  return 1;
}
static int ensure_rowcap(Table *T, size_t cap_rows) {
  if (cap_rows <= T->cap_rows) return 1;
  size_t ncap = T->cap_rows ? T->cap_rows : 4;
  while (ncap < cap_rows) ncap <<= 1;
  Cell *ncells = (Cell *)calloc(
      ncap * (T->cap_cols ? T->cap_cols : (T->ncols ? T->ncols : 1)),
      sizeof(Cell));
  if (!ncells) return 0;
  size_t stride_old = (T->cap_cols ? T->cap_cols : (T->ncols ? T->ncols : 1));
  size_t stride_new = (T->cap_cols ? T->cap_cols : (T->ncols ? T->ncols : 1));
  if (T->cells) {
    for (size_t r = 0; r < T->nrows; r++) {
      for (size_t c = 0; c < T->ncols; c++) {
        Cell *src = &T->cells[r * stride_old + c];
        Cell *dst = &ncells[r * stride_new + c];
        cell_copy(dst, src);
      }
    }
    // free old
    for (size_t r = 0; r < T->nrows; r++) {
      for (size_t c = 0; c < stride_old; c++) {
        Cell *src = &T->cells[r * stride_old + c];
        cell_free(src);
      }
    }
    free(T->cells);
  }
  T->cells = ncells;
  T->cap_rows = ncap;
  return 1;
}
static inline Cell *at(Table *T, size_t r, size_t c) {
  size_t stride = (T->cap_cols ? T->cap_cols : T->ncols);
  return &T->cells[r * stride + c];
}
static int check_id(int id) { return id > 0 && id < g_cap && g_tab[id].used; }

static void table_free(Table *T) {
  if (!T) return;
  // free cell heap
  size_t stride = (T->cap_cols ? T->cap_cols : (T->ncols ? T->ncols : 1));
  for (size_t r = 0; r < T->nrows; r++) {
    for (size_t c = 0; c < stride; c++) {
      cell_free(&T->cells[r * stride + c]);
    }
  }
  free(T->cells);
  T->cells = NULL;
  // free colnames
  for (size_t c = 0; c < T->ncols; c++) {
    free(T->colnames[c]);
    T->colnames[c] = NULL;
  }
  free(T->colnames);
  T->colnames = NULL;
  T->nrows = T->ncols = T->cap_rows = T->cap_cols = 0;
  T->used = 0;
}

static int colname_set(Table *T, size_t idx, const char *name) {
  if (idx >= T->cap_cols && !ensure_colcap(T, idx + 1)) return 0;
  size_t n = strlen(name ? name : "");
  char *d = (char *)malloc(n + 1);
  if (!d) return 0;
  memcpy(d, name ? name : "", n + 1);
  if (idx < T->ncols && T->colnames[idx]) free(T->colnames[idx]);
  T->colnames[idx] = d;
  return 1;
}
static int colname_cmp_ci(const char *a, const char *b) {
  for (;; a++, b++) {
    int ca = tolower((unsigned char)*a);
    int cb = tolower((unsigned char)*b);
    if (ca != cb) return ca < cb ? -1 : 1;
    if (!*a || !*b) return 0;
  }
}

// ------------------------------------------------------------
// VM — Lifecycle
// ------------------------------------------------------------
static int vltab_new(VL_State *S) {
  int id = alloc_slot();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_tab[id].used = 1;
  g_tab[id].nrows = 0;
  g_tab[id].ncols = 0;
  g_tab[id].cap_rows = 0;
  g_tab[id].cap_cols = 0;
  g_tab[id].colnames = NULL;
  g_tab[id].cells = NULL;
  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vltab_free(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  if (check_id(id)) table_free(&g_tab[id]);
  vl_push_bool(S, 1);
  return 1;
}

static int vltab_clear(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  // clear cells
  size_t stride = (T->cap_cols ? T->cap_cols : (T->ncols ? T->ncols : 1));
  for (size_t r = 0; r < T->nrows; r++)
    for (size_t c = 0; c < stride; c++) cell_free(&T->cells[r * stride + c]);
  T->nrows = 0;
  vl_push_bool(S, 1);
  return 1;
}

static int vltab_reserve(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  int rows = (int)tb_check_int(S, 2);
  int cols = (int)tb_check_int(S, 3);
  if (!check_id(id) || rows < 0 || cols < 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  if (cols > 0 && !ensure_colcap(T, (size_t)cols)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  if (rows > 0 && !ensure_rowcap(T, (size_t)rows)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

static int vltab_clone(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int nid = alloc_slot();
  if (!nid) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  Table *A = &g_tab[id], *B = &g_tab[nid];
  B->used = 1;
  B->nrows = A->nrows;
  B->ncols = A->ncols;
  B->cap_rows = A->nrows;
  B->cap_cols = A->ncols ? A->ncols : 1;
  B->colnames = (char **)calloc(B->cap_cols, sizeof(char *));
  B->cells = (Cell *)calloc(
      (B->cap_rows ? B->cap_rows : 1) * (B->cap_cols ? B->cap_cols : 1),
      sizeof(Cell));
  if (!B->colnames || !B->cells) {
    table_free(B);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  for (size_t c = 0; c < B->ncols; c++)
    colname_set(B, c, A->colnames[c] ? A->colnames[c] : "");
  for (size_t r = 0; r < B->nrows; r++)
    for (size_t c = 0; c < B->ncols; c++) cell_copy(at(B, r, c), at(A, r, c));
  vl_push_int(S, (int64_t)nid);
  return 1;
}

// ------------------------------------------------------------
// VM — Dimensions / colonnes
// ------------------------------------------------------------
static int vltab_nrows(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_int(S, 0);
    return 1;
  }
  vl_push_int(S, (int64_t)g_tab[id].nrows);
  return 1;
}
static int vltab_ncols(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_int(S, 0);
    return 1;
  }
  vl_push_int(S, (int64_t)g_tab[id].ncols);
  return 1;
}

static int vltab_columns_csv(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  char sep = tb_opt_sep(S, 2, ',');
  if (!check_id(id)) {
    vl_push_string(S, "");
    return 1;
  }
  Table *T = &g_tab[id];
  AuxBuffer b = {0};
  for (size_t c = 0; c < T->ncols; c++) {
    const char *nm = T->colnames[c] ? T->colnames[c] : "";
    // CSV escaping minimal if sep present or quotes
    int needq = 0;
    for (const char *p = nm; *p; ++p)
      if (*p == sep || *p == '"' || *p == '\n' || *p == '\r') {
        needq = 1;
        break;
      }
    if (c) aux_buffer_append_byte(&b, (uint8_t)sep);
    if (!needq)
      aux_buffer_append(&b, (const uint8_t *)nm, strlen(nm));
    else {
      aux_buffer_append_byte(&b, '"');
      for (const char *p = nm; *p; ++p) {
        if (*p == '"') {
          aux_buffer_append_byte(&b, '"');
          aux_buffer_append_byte(&b, '"');
        } else
          aux_buffer_append_byte(&b, (uint8_t)*p);
      }
      aux_buffer_append_byte(&b, '"');
    }
  }
  vl_push_lstring(S, (const char *)b.data, (int)b.len);
  aux_buffer_free(&b);
  return 1;
}

static int vltab_add_col(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  const char *name = tb_check_str(S, 2);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  if (!ensure_colcap(T, T->ncols + 1)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  if (!colname_set(T, T->ncols, name)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  // Make sure rows have capacity
  if (!ensure_rowcap(T, T->nrows ? T->nrows : 1)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  T->ncols++;
  vl_push_int(S, (int64_t)T->ncols);
  return 1;
}

static int vltab_insert_col(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  int at = (int)tb_check_int(S, 2);
  const char *name = tb_check_str(S, 3);
  if (!check_id(id) || at < 1) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  if ((size_t)at > T->ncols + 1) at = (int)T->ncols + 1;
  if (!ensure_colcap(T, T->ncols + 1)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  size_t stride = T->cap_cols;
  // shift names
  for (size_t c = T->ncols; c >= (size_t)at && c > 0; c--) {
    T->colnames[c] = T->colnames[c - 1];
  }
  T->colnames[at - 1] = NULL;
  if (!colname_set(T, at - 1, name)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  // shift cells right
  for (size_t r = 0; r < T->nrows; r++) {
    for (size_t c = T->ncols; c >= (size_t)at && c > 0; c--) {
      Cell *dst = &T->cells[r * stride + c];
      Cell *src = &T->cells[r * stride + (c - 1)];
      cell_copy(dst, src);
    }
    // clear new cell
    Cell *nc = &T->cells[r * stride + (at - 1)];
    cell_free(nc);
    nc->t = TB_NIL;
  }
  T->ncols++;
  vl_push_bool(S, 1);
  return 1;
}

static int vltab_drop_col(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  int col = (int)tb_check_int(S, 2);
  if (!check_id(id) || col < 1) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  if ((size_t)col > T->ncols || T->ncols == 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  size_t stride = T->cap_cols;
  // free cells of that column
  for (size_t r = 0; r < T->nrows; r++)
    cell_free(&T->cells[r * stride + (col - 1)]);
  // shift cells left
  for (size_t r = 0; r < T->nrows; r++) {
    for (size_t c = (size_t)col; c < T->ncols; c++) {
      Cell *dst = &T->cells[r * stride + (c - 1)];
      Cell *src = &T->cells[r * stride + c];
      cell_free(dst);
      cell_copy(dst, src);
    }
    // clear tail cell
    cell_free(&T->cells[r * stride + (T->ncols - 1)]);
  }
  // free name
  if (T->colnames[col - 1]) {
    free(T->colnames[col - 1]);
    T->colnames[col - 1] = NULL;
  }
  for (size_t c = (size_t)col; c < T->ncols; c++)
    T->colnames[c - 1] = T->colnames[c];
  T->colnames[T->ncols - 1] = NULL;
  T->ncols--;
  vl_push_bool(S, 1);
  return 1;
}

static int vltab_rename_col(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  int col = (int)tb_check_int(S, 2);
  const char *name = tb_check_str(S, 3);
  if (!check_id(id) || col < 1) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  if ((size_t)col > T->ncols) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  if (!colname_set(T, (size_t)col - 1, name)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

static int vltab_col_index(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  const char *name = tb_check_str(S, 2);
  if (!check_id(id)) {
    vl_push_int(S, 0);
    return 1;
  }
  Table *T = &g_tab[id];
  for (size_t c = 0; c < T->ncols; c++) {
    if (!T->colnames[c]) continue;
    if (strcmp(T->colnames[c], name) == 0) {
      vl_push_int(S, (int64_t)(c + 1));
      return 1;
    }
  }
  vl_push_int(S, 0);
  return 1;
}

// ------------------------------------------------------------
// VM — Lignes
// ------------------------------------------------------------
static int vltab_append_row(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  if (!ensure_rowcap(T, T->nrows + 1)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  // init row nil
  size_t stride = (T->cap_cols ? T->cap_cols : (T->ncols ? T->ncols : 1));
  for (size_t c = 0; c < T->ncols; c++) {
    Cell *x = &T->cells[T->nrows * stride + c];
    cell_free(x);
    x->t = TB_NIL;
  }
  T->nrows++;
  vl_push_int(S, (int64_t)T->nrows);
  return 1;
}
static int vltab_insert_row(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  int at = (int)tb_check_int(S, 2);
  if (!check_id(id) || at < 1) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  if ((size_t)at > T->nrows + 1) at = (int)T->nrows + 1;
  if (!ensure_rowcap(T, T->nrows + 1)) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  size_t stride = T->cap_cols ? T->cap_cols : T->ncols;
  // shift down
  for (size_t r = T->nrows; r >= (size_t)at && r > 0; r--) {
    for (size_t c = 0; c < T->ncols; c++) {
      Cell *dst = &T->cells[r * stride + c];
      Cell *src = &T->cells[(r - 1) * stride + c];
      cell_copy(dst, src);
    }
  }
  // clear new row
  for (size_t c = 0; c < T->ncols; c++) {
    Cell *x = &T->cells[((size_t)at - 1) * stride + c];
    cell_free(x);
    x->t = TB_NIL;
  }
  T->nrows++;
  vl_push_bool(S, 1);
  return 1;
}
static int vltab_drop_row(VL_State *S) {
  int id = (int)tb_check_int(S, 1);
  int row = (int)tb_check_int(S, 2);
  if (!check_id(id) || row < 1) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  Table *T = &g_tab[id];
  if ((size_t)row > T->nrows || T->nrows == 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  size_t stride = T->cap_cols ? T->cap_cols : T->ncols;
  // free row cells
  for (size_t c = 0; c < T->ncols; c++)
    cell_free(&T->cells[((size_t)row - 1) * stride + c]);
  // shift up
    for (size_t r=(size_t
