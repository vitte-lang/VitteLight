// SPDX-License-Identifier: GPL-3.0-or-later
//
// csv.c — CSV/TSV encoder-decoder for Vitte Light VM (C17, complet)
// Namespace: "csv"
//
// Design
// - Zéro dépendance externe. Binaire-safe côté sortie via AuxBuffer.
// - Parse RFC4180-like: séparateur, guillemet, CRLF/LF, quotes doublés.
// - Échappement optionnel par caractère d’escape (ex: '\').
// - Format intermédiaire "USV": champs séparés par US(0x1F), lignes par
// RS(0x1E).
//
// API
//   Detection
//     csv.sniff(bytes[, quote="\""[, escape=""[, max_lines=10]]])
//       -> sep:string(1), newline:"CRLF"|"LF"|"CR"|"MIX"
//
//   Decode
//     csv.decode(bytes[, sep=","[, quote="\""[, escape=""[, lax=false]]]])
//       -> usv:string | (nil,errmsg)
//     csv.decode_tsv(bytes) -> usv:string | (nil,errmsg)  // alias: tab sep,
//     quote disabled
//
//   Encode
//     csv.encode(usv[, sep=","[, quote="\""[, newline="\n"]]])
//       -> csv_bytes:string | (nil,errmsg)
//     csv.encode_tsv(usv[, newline="\n"]) -> tsv_bytes:string | (nil,errmsg)
//
//   Iteration (streaming from an in-memory buffer)
//     r = csv.reader(bytes[, sep=","[, quote="\""[, escape=""[, lax=false]]]])
//     -> id | (nil,errmsg) csv.read_row(r) -> usv_row:string | (nil,"eof") |
//     (nil,errmsg) csv.free(r) -> true
//
// Notes
// - "lax=true" permet fin de fichier dans un champ quoté non-clos (le champ est
// clos implicitement).
// - Les VM strings étant 0-terminées, l'entrée ne doit pas contenir d’octets
// NUL.
// - L’USV est pratique pour `tablib.c` ou un split simple côté VM.
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

// US/RS séparateurs internes
#define US 0x1F
#define RS 0x1E

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *cs_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t cs_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int cs_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static const char *cs_opt_str(VL_State *S, int idx, const char *defv) {
  if (!vl_get(S, idx) || !vl_isstring(S, idx)) return defv;
  return cs_check_str(S, idx);
}
static char first_char_or_zero(const char *s) {
  return (s && s[0]) ? s[0] : '\0';
}

// ---------------------------------------------------------------------
// Core parser
// ---------------------------------------------------------------------
typedef struct CsvOpts {
  char sep;    // ',', ';', '\t', etc.
  char quote;  // '"' or '\0' to disable
  char esc;    // '\' or same as quote for double-quote style; '\0' to disable
  int lax;     // allow EOF inside quote
} CsvOpts;

typedef struct CsvReader {
  int used;
  const char *buf;
  size_t len;
  size_t off;
  CsvOpts opt;
} CsvReader;

static CsvReader *g_r = NULL;
static int g_r_cap = 0;

static int ensure_r_cap(int need) {
  if (need <= g_r_cap) return 1;
  int n = g_r_cap ? g_r_cap : 8;
  while (n < need) n <<= 1;
  CsvReader *nr = (CsvReader *)realloc(g_r, (size_t)n * sizeof *nr);
  if (!nr) return 0;
  for (int i = g_r_cap; i < n; i++) {
    nr[i].used = 0;
    nr[i].buf = NULL;
    nr[i].len = 0;
    nr[i].off = 0;
    nr[i].opt.sep = ',';
    nr[i].opt.quote = '"';
    nr[i].opt.esc = 0;
    nr[i].opt.lax = 0;
  }
  g_r = nr;
  g_r_cap = n;
  return 1;
}
static int alloc_r(void) {
  for (int i = 1; i < g_r_cap; i++)
    if (!g_r[i].used) return i;
  if (!ensure_r_cap(g_r_cap ? g_r_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_r_cap; i++)
    if (!g_r[i].used) return i;
  return 0;
}
static int chk_r(int id) {
  return id > 0 && id < g_r_cap && g_r[id].used && g_r[id].buf;
}

static void csv_emit_field(AuxBuffer *row, const uint8_t *data, size_t n) {
  if (n) aux_buffer_append(row, data, n);
  const uint8_t u = US;
  aux_buffer_append(row, &u, 1);
}
static void csv_finish_row(AuxBuffer *out, AuxBuffer *row) {
  // Replace trailing US with RS; if empty row still write RS
  if (row->len && row->data[row->len - 1] == (uint8_t)US)
    row->data[row->len - 1] = (uint8_t)RS;
  else {
    const uint8_t r = RS;
    aux_buffer_append(row, &r, 1);
  }
  aux_buffer_append(out, row->data, row->len);
  aux_buffer_reset(row);
}

static int csv_parse_range(const char *s, size_t n, const CsvOpts *opt,
                           AuxBuffer *out) {
  AuxBuffer field = {0};
  AuxBuffer row = {0};
  int inq = 0;
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (inq) {
      if (opt->esc && c == (unsigned char)opt->esc && opt->esc != opt->quote) {
        // backslash-like escape: take next char verbatim if any
        if (i + 1 < n) {
          aux_buffer_append(&field, (const uint8_t *)&s[i + 1], 1);
          i += 2;
          continue;
        }
        // lone escape at EOF -> treat as literal
        aux_buffer_append(&field, &c, 1);
        i++;
        continue;
      }
      if (opt->quote && c == (unsigned char)opt->quote) {
        // doubled quote -> literal quote
        if (i + 1 < n && (unsigned char)s[i + 1] == (unsigned char)opt->quote) {
          aux_buffer_append(&field, (const uint8_t *)&s[i], 1);
          i += 2;
          continue;
        }
        // closing
        inq = 0;
        i++;
        continue;
      }
      // normal char in quoted field
      aux_buffer_append(&field, &c, 1);
      i++;
      continue;
    } else {
      if (opt->quote && c == (unsigned char)opt->quote) {
        inq = 1;
        i++;
        continue;
      }
      if (c == (unsigned char)opt->sep) {
        csv_emit_field(&row, field.data, field.len);
        aux_buffer_reset(&field);
        i++;
        continue;
      }
      if (c == '\r' || c == '\n') {
        csv_emit_field(&row, field.data, field.len);
        aux_buffer_reset(&field);
        // handle CRLF
        if (c == '\r' && i + 1 < n && s[i + 1] == '\n')
          i += 2;
        else
          i++;
        csv_finish_row(out, &row);
        continue;
      }
      aux_buffer_append(&field, &c, 1);
      i++;
      continue;
    }
  }
  if (inq && !opt->lax) {
    aux_buffer_free(&field);
    aux_buffer_free(&row);
    return 0;  // EINVAL: unterminated quote
  }
  // flush last field/row if any content or if input not empty
  if (field.len || row.len || n > 0) {
    csv_emit_field(&row, field.data, field.len);
    csv_finish_row(out, &row);
  }
  aux_buffer_free(&field);
  aux_buffer_free(&row);
  return 1;
}

static void csv_encode_field(AuxBuffer *out, const uint8_t *s, size_t n,
                             char sep, char quote) {
  int need_q = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c == (unsigned char)sep || c == '\n' || c == '\r' ||
        c == (unsigned char)quote) {
      need_q = 1;
      break;
    }
  }
  // RFC4180 also recommends quoting if leading/trailing spaces
  if (!need_q && n) {
    if (s[0] == ' ' || s[0] == '\t' || s[n - 1] == ' ' || s[n - 1] == '\t')
      need_q = 1;
  }
  if (!need_q || quote == '\0') {
    aux_buffer_append(out, s, n);
    return;
  }
  // Wrap in quotes and double inner quotes
  aux_buffer_append(out, (const uint8_t *)&quote, 1);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c == (unsigned char)quote)
      aux_buffer_append(out, (const uint8_t *)&quote, 1);
    aux_buffer_append(out, &c, 1);
  }
  aux_buffer_append(out, (const uint8_t *)&quote, 1);
}

// ---------------------------------------------------------------------
// Sniff
// ---------------------------------------------------------------------
static int vm_csv_sniff(VL_State *S) {
  const char *bytes = cs_check_str(S, 1);
  const char *qstr = cs_opt_str(S, 2, "\"");
  const char *estr = cs_opt_str(S, 3, "");
  int max_lines = (int)cs_check_int(S, 4);
  if (max_lines <= 0) max_lines = 10;

  char quote = first_char_or_zero(qstr);
  char esc = first_char_or_zero(estr);

  size_t n = strlen(bytes);
  size_t lines = 0;
  // candidates
  const char cands[] = {',', ';', '\t', '|'};
  size_t counts[4] = {0, 0, 0, 0};
  size_t perline[4] = {0, 0, 0, 0};
  int inq = 0;

  // newline detection
  int saw_cr = 0, saw_lf = 0, saw_crlf = 0;

  for (size_t i = 0; i < n && lines < (size_t)max_lines; i++) {
    unsigned char c = (unsigned char)bytes[i];
    if (inq) {
      if (esc && esc != quote && c == (unsigned char)esc) {
        if (i + 1 < n) i++;
        continue;
      }
      if (quote && c == (unsigned char)quote) {
        if (i + 1 < n && (unsigned char)bytes[i + 1] == (unsigned char)quote) {
          i++;
          continue;
        }
        inq = 0;
        continue;
      }
      continue;
    } else {
      if (quote && c == (unsigned char)quote) {
        inq = 1;
        continue;
      }
      for (int k = 0; k < 4; k++) {
        if (c == (unsigned char)cands[k]) {
          counts[k]++;
          perline[k]++;
        }
      }
      if (c == '\r' || c == '\n') {
        if (c == '\r') {
          saw_cr = 1;
          if (i + 1 < n && bytes[i + 1] == '\n') {
            saw_crlf = 1;
            i++;
          }
        } else
          saw_lf = 1;
        lines++;
        // reset perline
        for (int k = 0; k < 4; k++) perline[k] = 0;
      }
    }
  }

  // pick best sep: highest total counts
  int best = 0;
  for (int k = 1; k < 4; k++)
    if (counts[k] > counts[best]) best = k;

  char sep = cands[best];
  const char *nl =
      (saw_crlf
           ? "CRLF"
           : (saw_lf && saw_cr ? "MIX"
                               : (saw_cr ? "CR" : (saw_lf ? "LF" : "LF"))));

  char outsep[2] = {sep, 0};
  vl_push_string(S, outsep);
  vl_push_string(S, nl);
  return 2;
}

// ---------------------------------------------------------------------
// Decode whole-buffer
// ---------------------------------------------------------------------
static int vm_csv_decode(VL_State *S) {
  const char *bytes = cs_check_str(S, 1);
  const char *sepstr = cs_opt_str(S, 2, ",");
  const char *qstr = cs_opt_str(S, 3, "\"");
  const char *estr = cs_opt_str(S, 4, "");
  int lax = cs_opt_bool(S, 5, 0);

  CsvOpts o;
  o.sep = first_char_or_zero(sepstr);
  if (!o.sep) o.sep = ',';
  o.quote = first_char_or_zero(qstr);
  o.esc = first_char_or_zero(estr);
  o.lax = lax ? 1 : 0;

  AuxBuffer out = {0};
  if (!csv_parse_range(bytes, strlen(bytes), &o, &out)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

static int vm_csv_decode_tsv(VL_State *S) {
  const char *bytes = cs_check_str(S, 1);
  CsvOpts o;
  o.sep = '\t';
  o.quote = '\0';
  o.esc = '\0';
  o.lax = 0;
  AuxBuffer out = {0};
  if (!csv_parse_range(bytes, strlen(bytes), &o, &out)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

// ---------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------
static int vm_csv_encode(VL_State *S) {
  const char *usv = cs_check_str(S, 1);
  const char *sepstr = cs_opt_str(S, 2, ",");
  const char *qstr = cs_opt_str(S, 3, "\"");
  const char *nlstr = cs_opt_str(S, 4, "\n");

  char sep = first_char_or_zero(sepstr);
  if (!sep) sep = ',';
  char quote = first_char_or_zero(qstr);  // may be 0 to disable

  AuxBuffer out = {0};

  const uint8_t *p = (const uint8_t *)usv;
  size_t n = strlen(usv);
  size_t start = 0;
  for (size_t i = 0; i <= n; i++) {
    int at_end = (i == n);
    unsigned char c = at_end ? RS : p[i];
    if (c == US || c == RS) {
      // field [start..i)
      csv_encode_field(&out, p + start, i - start, sep, quote);
      if (c == US) {
        const uint8_t sepb = (uint8_t)sep;
        aux_buffer_append(&out, &sepb, 1);
      } else {
        aux_buffer_append(&out, (const uint8_t *)nlstr, strlen(nlstr));
      }
      start = i + 1;
    }
  }
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

static int vm_csv_encode_tsv(VL_State *S) {
  const char *usv = cs_check_str(S, 1);
  const char *nlstr = cs_opt_str(S, 2, "\n");

  AuxBuffer out = {0};
  const uint8_t *p = (const uint8_t *)usv;
  size_t n = strlen(usv);
  size_t start = 0;
  for (size_t i = 0; i <= n; i++) {
    int at_end = (i == n);
    unsigned char c = at_end ? RS : p[i];
    if (c == US || c == RS) {
      // TSV: no quoting; escape tabs/newlines by replacing with spaces
      for (size_t k = start; k < i; k++) {
        unsigned char ch = p[k];
        if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
        aux_buffer_append(&out, &ch, 1);
      }
      if (c == US) {
        const uint8_t t = '\t';
        aux_buffer_append(&out, &t, 1);
      } else {
        aux_buffer_append(&out, (const uint8_t *)nlstr, strlen(nlstr));
      }
      start = i + 1;
    }
  }
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

// ---------------------------------------------------------------------
// Reader (iterator on buffer)
// ---------------------------------------------------------------------
static int vm_csv_reader(VL_State *S) {
  const char *bytes = cs_check_str(S, 1);
  const char *sepstr = cs_opt_str(S, 2, ",");
  const char *qstr = cs_opt_str(S, 3, "\"");
  const char *estr = cs_opt_str(S, 4, "");
  int lax = cs_opt_bool(S, 5, 0);

  int id = alloc_r();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_r[id].used = 1;
  g_r[id].buf = bytes;
  g_r[id].len = strlen(bytes);
  g_r[id].off = 0;
  g_r[id].opt.sep = first_char_or_zero(sepstr);
  if (!g_r[id].opt.sep) g_r[id].opt.sep = ',';
  g_r[id].opt.quote = first_char_or_zero(qstr);
  g_r[id].opt.esc = first_char_or_zero(estr);
  g_r[id].opt.lax = (lax ? 1 : 0);
  vl_push_int(S, (int64_t)id);
  return 1;
}

// read next row as USV row
static int vm_csv_read_row(VL_State *S) {
  int id = (int)cs_check_int(S, 1);
  if (!chk_r(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  CsvReader *r = &g_r[id];
  if (r->off >= r->len) {
    vl_push_nil(S);
    vl_push_string(S, "eof");
    return 2;
  }

  AuxBuffer out = {0};
  AuxBuffer field = {0};

  int inq = 0;
  int got_any = 0;
  size_t i = r->off;
  while (i < r->len) {
    unsigned char c = (unsigned char)r->buf[i];
    if (inq) {
      if (r->opt.esc && r->opt.esc != r->opt.quote &&
          c == (unsigned char)r->opt.esc) {
        if (i + 1 < r->len) {
          aux_buffer_append(&field, (const uint8_t *)&r->buf[i + 1], 1);
          i += 2;
          continue;
        }
        aux_buffer_append(&field, &c, 1);
        i++;
        continue;
      }
      if (r->opt.quote && c == (unsigned char)r->opt.quote) {
        if (i + 1 < r->len &&
            (unsigned char)r->buf[i + 1] == (unsigned char)r->opt.quote) {
          aux_buffer_append(&field, (const uint8_t *)&r->buf[i], 1);
          i += 2;
          continue;
        }
        inq = 0;
        i++;
        continue;
      }
      aux_buffer_append(&field, &c, 1);
      i++;
      continue;
    } else {
      if (r->opt.quote && c == (unsigned char)r->opt.quote) {
        inq = 1;
        i++;
        got_any = 1;
        continue;
      }
      if (c == (unsigned char)r->opt.sep) {
        csv_emit_field(&out, field.data, field.len);
        aux_buffer_reset(&field);
        i++;
        got_any = 1;
        continue;
      }
      if (c == '\r' || c == '\n') {
        csv_emit_field(&out, field.data, field.len);
        aux_buffer_reset(&field);
        if (c == '\r' && i + 1 < r->len && r->buf[i + 1] == '\n')
          i += 2;
        else
          i++;
        r->off = i;
        // finalize row
        if (out.len && out.data[out.len - 1] == (uint8_t)US)
          out.data[out.len - 1] = (uint8_t)RS;
        else {
          const uint8_t rsch = (uint8_t)RS;
          aux_buffer_append(&out, &rsch, 1);
        }
        vl_push_lstring(S, (const char *)out.data, (int)out.len);
        aux_buffer_free(&out);
        aux_buffer_free(&field);
        return 1;
      }
      aux_buffer_append(&field, &c, 1);
      i++;
      got_any = 1;
      continue;
    }
  }
  // EOF
  if (inq && !r->opt.lax) {
    aux_buffer_free(&out);
    aux_buffer_free(&field);
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  r->off = i;
  if (!got_any && field.len == 0) {
    aux_buffer_free(&out);
    aux_buffer_free(&field);
    vl_push_nil(S);
    vl_push_string(S, "eof");
    return 2;
  }
  csv_emit_field(&out, field.data, field.len);
  if (out.len && out.data[out.len - 1] == (uint8_t)US)
    out.data[out.len - 1] = (uint8_t)RS;
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  aux_buffer_free(&field);
  return 1;
}

static int vm_csv_free(VL_State *S) {
  int id = (int)cs_check_int(S, 1);
  if (id > 0 && id < g_r_cap && g_r[id].used) {
    g_r[id].used = 0;
    g_r[id].buf = NULL;
    g_r[id].len = 0;
    g_r[id].off = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------
static const VL_Reg csvlib[] = {{"sniff", vm_csv_sniff},

                                {"decode", vm_csv_decode},
                                {"decode_tsv", vm_csv_decode_tsv},

                                {"encode", vm_csv_encode},
                                {"encode_tsv", vm_csv_encode_tsv},

                                {"reader", vm_csv_reader},
                                {"read_row", vm_csv_read_row},
                                {"free", vm_csv_free},

                                {NULL, NULL}};

void vl_open_csvlib(VL_State *S) { vl_register_lib(S, "csv", csvlib); }
