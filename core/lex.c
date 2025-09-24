// SPDX-License-Identifier: MIT
/* ============================================================================
   lex.c — Lexer Vitte/Vitl (C17, portable)

   - Identifiants: [A-Za-z_][A-Za-z0-9_]*
   - Nombres: décimal, hexa 0x, binaire 0b, flottants + exp, '_' autorisé
   - Littéraux: int, float, bool (true/false), char, string
     Escapes: \n \r \t \0 \\ \" \xHH \uXXXX
   - Commentaires: //, //!, (slash-star ... ) avec imbrication
   - Opérateurs/délimiteurs: une et deux/3-char
     (== != <= >= -> :: ... += -= *= /= %= && || << >>)
   - Positions: ligne, colonne, offset
   - API: init mémoire/fichier, next/peek, expect, dump
   ============================================================================ */

#define _CRT_SECURE_NO_WARNINGS

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
   Utilitaires de base
--------------------------------------------------------------------------- */
#ifndef VT_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define VT_INLINE static __inline__ __attribute__((always_inline))
#else
#define VT_INLINE static inline
#endif
#endif

#ifndef VT_LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define VT_LIKELY(x) __builtin_expect(!!(x), 1)
#define VT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define VT_LIKELY(x) (x)
#define VT_UNLIKELY(x) (x)
#endif
#endif

#define VT_COUNTOF(a) ((int)(sizeof(a) / sizeof((a)[0])))

            /* ---------------------------------------------------------------------------
               Types de tokens
            ---------------------------------------------------------------------------
          */
            typedef enum vt_tok_kind {
              /* Spéciaux */
              TK_EOF = 0,
              TK_ERROR,

              /* Ident / litteraux */
              TK_IDENT,
              TK_INT,
              TK_FLOAT,
              TK_BOOL,
              TK_CHAR,
              TK_STRING,

              /* Mots-clés */
              TK_KW_module,
              TK_KW_import,
              TK_KW_use,
              TK_KW_as,
              TK_KW_pub,
              TK_KW_const,
              TK_KW_let,
              TK_KW_mut,
              TK_KW_fn,
              TK_KW_return,
              TK_KW_if,
              TK_KW_else,
              TK_KW_match,
              TK_KW_while,
              TK_KW_for,
              TK_KW_in,
              TK_KW_break,
              TK_KW_continue,
              TK_KW_type,
              TK_KW_impl,
              TK_KW_where,
              TK_KW_test,
              TK_KW_true,
              TK_KW_false, /* mappés sur TK_BOOL à l’émission */

              /* Opérateurs composés d’affectation */
              TK_EQ,
              TK_PLUSEQ,
              TK_MINUSEQ,
              TK_MULEQ,
              TK_DIVEQ,
              TK_MODEQ,
              TK_SHLEQ,
              TK_SHREQ,
              TK_ANDEQ,
              TK_XOREQ,
              TK_OREQ,

              /* Opérateurs logiques/bitwise/relatifs */
              TK_LOR,
              TK_LAND,
              TK_BOR,
              TK_BXOR,
              TK_BAND,
              TK_EQEQ,
              TK_NEQ,
              TK_LT,
              TK_LTE,
              TK_GT,
              TK_GTE,
              TK_SHL,
              TK_SHR,
              TK_PLUS,
              TK_MINUS,
              TK_STAR,
              TK_SLASH,
              TK_PERCENT,
              TK_BANG,
              TK_AMP,

              /* Ranges */
              TK_DOT,
              TK_DOTDOT,
              TK_DOTDOTEQ,

              /* Délimiteurs et séparateurs */
              TK_LP,
              TK_RP,
              TK_LB,
              TK_RB,
              TK_LC,
              TK_RC,
              TK_COMMA,
              TK_COLON,
              TK_SEMI,
              TK_DCOLON,
              TK_ARROW,
              TK_FATARROW
            } vt_tok_kind;

/* Position source */
typedef struct {
  uint32_t line;   /* 1-based */
  uint32_t col;    /* 1-based, octets */
  uint32_t offset; /* 0-based */
} vt_src_pos;

/* Valeurs numériques */
typedef struct {
  int is_neg;   /* pour int/float s’ils portent un signe lexical (rare) */
  uint64_t u64; /* pour int (non signé interne) */
  double f64;   /* pour float */
} vt_num;

/* Jeton */
typedef struct {
  vt_tok_kind kind;
  vt_src_pos pos;  /* début du lexème */
  const char* lex; /* pointeur dans le buffer source */
  uint32_t len;    /* longueur en octets */
  vt_num num;      /* si INT/FLOAT */
  int bool_val;    /* si BOOL */
  /* Pour STRING/CHAR: contenu décodé à la demande via util (non stocké ici) */
} vt_token;

/* Lexer */
typedef struct {
  const char* src;
  size_t len;
  size_t cur; /* index courant 0..len */
  uint32_t line;
  uint32_t col;

  /* Lookahead simple */
  vt_token la;
  int has_la;

  /* Options */
  int keep_doc; /* si 1, renvoie les //! comme TK_ERROR? Ici: ignorés comme
                   commentaires */

  /* Buffer d’erreur temporaire */
  char errbuf[256];
} vt_lexer;

/* ---------------------------------------------------------------------------
   Table des mots-clés
--------------------------------------------------------------------------- */
typedef struct {
  const char* s;
  vt_tok_kind k;
} kwent;
static const kwent KW[] = {
    {"module", TK_KW_module}, {"import", TK_KW_import},
    {"use", TK_KW_use},       {"as", TK_KW_as},
    {"pub", TK_KW_pub},       {"const", TK_KW_const},
    {"let", TK_KW_let},       {"mut", TK_KW_mut},
    {"fn", TK_KW_fn},         {"return", TK_KW_return},
    {"if", TK_KW_if},         {"else", TK_KW_else},
    {"match", TK_KW_match},   {"while", TK_KW_while},
    {"for", TK_KW_for},       {"in", TK_KW_in},
    {"break", TK_KW_break},   {"continue", TK_KW_continue},
    {"type", TK_KW_type},     {"impl", TK_KW_impl},
    {"where", TK_KW_where},   {"test", TK_KW_test},
    {"true", TK_KW_true},     {"false", TK_KW_false}};

/* Recherche mot-clé (ASCII, sensible à la casse) */
VT_INLINE vt_tok_kind vt_kw_lookup(const char* s, uint32_t n) {
  /* table petite → lineaire */
  for (int i = 0; i < VT_COUNTOF(KW); ++i) {
    if (KW[i].s[0] == s[0] && strlen(KW[i].s) == n &&
        memcmp(KW[i].s, s, n) == 0)
      return KW[i].k;
  }
  return TK_IDENT;
}

/* ---------------------------------------------------------------------------
   Helpers lexer
--------------------------------------------------------------------------- */
VT_INLINE int vt_is_letter(int c) {
  return (c == '_') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
VT_INLINE int vt_is_digit(int c) { return (c >= '0' && c <= '9'); }
VT_INLINE int vt_is_hexd(int c) {
  return vt_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

VT_INLINE int vt_hex_value(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

VT_INLINE int vt_eof(vt_lexer* lx) { return lx->cur >= lx->len; }
VT_INLINE char vt_peek(vt_lexer* lx) {
  return vt_eof(lx) ? '\0' : lx->src[lx->cur];
}
VT_INLINE char vt_peek2(vt_lexer* lx) {
  return (lx->cur + 1 >= lx->len) ? '\0' : lx->src[lx->cur + 1];
}
VT_INLINE char vt_peekn(vt_lexer* lx, size_t n) {
  return (lx->cur + n >= lx->len) ? '\0' : lx->src[lx->cur + n];
}

VT_INLINE char vt_get(vt_lexer* lx) {
  if (VT_UNLIKELY(vt_eof(lx))) return '\0';
  char c = lx->src[lx->cur++];
  if (c == '\n') {
    lx->line++;
    lx->col = 1;
  } else
    lx->col++;
  return c;
}

VT_INLINE vt_src_pos vt_pos_now(vt_lexer* lx) {
  vt_src_pos p;
  p.line = lx->line;
  p.col = lx->col;
  p.offset = (uint32_t)lx->cur;
  return p;
}

/* Saut d’espaces + commentaires (slash-star imbriqué, //, //! doc ignoré) */
static void vt_skip_ws(vt_lexer* lx) {
  for (;;) {
    char c = vt_peek(lx);
    /* espaces */
    while (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      vt_get(lx);
      c = vt_peek(lx);
    }
    /* commentaires */
    if (c == '/' && vt_peek2(lx) == '/') {
      vt_get(lx);
      vt_get(lx);
      /* skip jusqu’au \n ou EOF */
      while (!vt_eof(lx) && vt_peek(lx) != '\n') vt_get(lx);
      continue;
    }
    if (c == '/' && vt_peek2(lx) == '*') {
      vt_get(lx);
      vt_get(lx);
      int depth = 1;
      while (!vt_eof(lx) && depth > 0) {
        char d = vt_get(lx);
        if (d == '/' && vt_peek(lx) == '*') {
          vt_get(lx);
          depth++;
        } else if (d == '*' && vt_peek(lx) == '/') {
          vt_get(lx);
          depth--;
        }
      }
      continue;
    }
    break;
  }
}

/* ---------------------------------------------------------------------------
   Parsing littéraux
--------------------------------------------------------------------------- */
typedef struct {
  uint64_t u;
  int ok;
} u64p;

static u64p vt_parse_u64_base(const char* s, uint32_t n, int base) {
  u64p r = {0, 1};
  for (uint32_t i = 0; i < n; i++) {
    unsigned v;
    char c = s[i];
    if (c == '_') continue;
    if (base == 16) {
      if (!vt_is_hexd(c)) {
        r.ok = 0;
        break;
      }
      if (c >= '0' && c <= '9')
        v = c - '0';
      else if (c >= 'a' && c <= 'f')
        v = 10 + (c - 'a');
      else
        v = 10 + (c - 'A');
    } else if (base == 2) {
      if (!(c == '0' || c == '1')) {
        r.ok = 0;
        break;
      }
      v = (unsigned)(c - '0');
    } else { /* 10 */
      if (!vt_is_digit(c)) {
        r.ok = 0;
        break;
      }
      v = (unsigned)(c - '0');
    }
    uint64_t prev = r.u;
    r.u = r.u * base + v;
    if (r.u < prev) {
      r.ok = 0;
      break;
    } /* overflow simple */
  }
  return r;
}

typedef struct {
  double f;
  int ok;
} f64p;

static int vt_all_digits_underscore(const char* s, uint32_t n) {
  if (n == 0) return 0;
  int any = 0;
  for (uint32_t i = 0; i < n; i++) {
    char c = s[i];
    if (!(vt_is_digit(c) || c == '_')) return 0;
    if (vt_is_digit(c)) any = 1;
  }
  return any;
}

/* parse float forme:
   - d+._d* [exp]
   - d+[exp]
   avec '_' ignorés; exp = [eE][+/-]?d+ */
static f64p vt_parse_float(const char* s, uint32_t n) {
  f64p r = {0, 0};
  /* construire un buffer compact sans '_' et normaliser exp */
  char* tmp = (char*)malloc(n + 1);
  if (!tmp) return r;
  uint32_t m = 0;
  for (uint32_t i = 0; i < n; i++)
    if (s[i] != '_') tmp[m++] = s[i];
  tmp[m] = '\0';
  char* endp = NULL;
#if defined(_MSC_VER)
  r.f = strtod(tmp, &endp);
#else
  r.f = strtod(tmp, &endp);
#endif
  if (endp && *endp == '\0') r.ok = 1;
  free(tmp);
  return r;
}

/* décodage \xHH et \n\r\t\0\\\" pour string/char
   Retourne longueur écrite, -1 si erreur. */
static int vt_unescape(const char* in, uint32_t n, char* out, uint32_t outcap,
                       char quote) {
  uint32_t o = 0;
  for (uint32_t i = 0; i < n; i++) {
    char c = in[i];
    if (c == '\\') {
      if (i + 1 >= n) return -1;
      char e = in[++i];
      switch (e) {
        case '\\':
          c = '\\';
          break;
        case '\"':
          c = '\"';
          break;
        case '\'':
          c = '\'';
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case '0':
          c = '\0';
          break;
        case 'x': {
          if (i + 2 >= n) return -1;
          char h1 = in[++i], h2 = in[++i];
          if (!vt_is_hexd(h1) || !vt_is_hexd(h2)) return -1;
          int v1 = vt_hex_value(h1);
          int v2 = vt_hex_value(h2);
          if (v1 < 0 || v2 < 0) return -1;
          unsigned v = (unsigned)((v1 << 4) | v2);
          if (o >= outcap) return -1;
          out[o++] = (char)v;
          continue;
        }
        default:
          return -1;
      }
      if (o >= outcap) return -1;
      out[o++] = c;
    } else {
      if ((unsigned char)c < 0x20 && c != '\t')
        return -1;               /* contrôle interdit */
      if (c == quote) return -1; /* ne devrait pas apparaître sans escape */
      if (o >= outcap) return -1;
      out[o++] = c;
    }
  }
  return (int)o;
}

/* ---------------------------------------------------------------------------
   Construction de token
--------------------------------------------------------------------------- */
VT_INLINE vt_token vt_tok_make(vt_tok_kind k, vt_src_pos p, const char* s,
                               uint32_t n) {
  vt_token t;
  memset(&t, 0, sizeof t);
  t.kind = k;
  t.pos = p;
  t.lex = s;
  t.len = n;
  return t;
}
VT_INLINE vt_token vt_tok_err(vt_lexer* lx, vt_src_pos p, const char* msg) {
  vt_token t = vt_tok_make(TK_ERROR, p, lx->src + p.offset, 0);
  (void)msg; /* place-holder: message via vt_lex_last_error() si besoin */
  return t;
}

/* ---------------------------------------------------------------------------
   API
--------------------------------------------------------------------------- */
void vt_lex_init(vt_lexer* lx, const char* src, size_t len) {
  memset(lx, 0, sizeof *lx);
  lx->src = src;
  lx->len = len;
  lx->cur = 0;
  lx->line = 1;
  lx->col = 1;
}
int vt_lex_init_from_file(vt_lexer* lx, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return -1;
  }
  rewind(f);
  char* buf = (char*)malloc((size_t)sz);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    free(buf);
    return -1;
  }
  vt_lex_init(lx, buf, (size_t)sz);
  return 0;
}
void vt_lex_dispose_from_file(vt_lexer* lx) {
  /* libère src si alloué par init_from_file */
  free((void*)lx->src);
  memset(lx, 0, sizeof *lx);
}

/* Diagnostic simple: extrait la ligne courante */
static void vt_extract_line(const char* src, size_t len, uint32_t offset,
                            const char** line_beg, uint32_t* line_len,
                            uint32_t* col_in_line) {
  size_t b = offset, e = offset;
  while (b > 0 && src[b - 1] != '\n') b--;
  while (e < len && src[e] != '\n') e++;
  *line_beg = src + b;
  *line_len = (uint32_t)(e - b);
  /* recalcul col relative */
  uint32_t col = 1;
  for (size_t i = b; i < offset; i++) col += 1;
  *col_in_line = col;
}

/* Pour messages externes éventuels */
const char* vt_lex_format_error(vt_lexer* lx, const char* msg, vt_src_pos p) {
  const char* lb;
  uint32_t ln, col;
  vt_extract_line(lx->src, lx->len, p.offset, &lb, &ln, &col);
  int n =
      snprintf(lx->errbuf, sizeof(lx->errbuf), "error:%u:%u: %s\n%.*s\n%*s^\n",
               p.line, p.col, msg, (int)ln, lb, (int)(col - 1), "");
  (void)n;
  return lx->errbuf;
}

/* ---------------------------------------------------------------------------
   Lexing principal
--------------------------------------------------------------------------- */
static vt_token vt_lex_number(vt_lexer* lx, vt_src_pos p) {
  /* on est au début sur [0-9] */
  size_t start = lx->cur;
  int is_float = 0;

  /* 0x / 0b */
  if (vt_peek(lx) == '0' && (vt_peek2(lx) == 'x' || vt_peek2(lx) == 'X')) {
    vt_get(lx);
    vt_get(lx);
    size_t s2 = lx->cur;
    while (vt_is_hexd(vt_peek(lx)) || vt_peek(lx) == '_') vt_get(lx);
    if (lx->cur == s2) return vt_tok_err(lx, p, "hex literal requires digits");
    const char* lex = lx->src + s2;
    uint32_t n = (uint32_t)(lx->cur - s2);
    u64p r = vt_parse_u64_base(lex, n, 16);
    if (!r.ok) return vt_tok_err(lx, p, "invalid hex literal");
    vt_token t =
        vt_tok_make(TK_INT, p, lx->src + start, (uint32_t)(lx->cur - start));
    t.num.u64 = r.u;
    t.num.is_neg = 0;
    return t;
  }
  if (vt_peek(lx) == '0' && (vt_peek2(lx) == 'b' || vt_peek2(lx) == 'B')) {
    vt_get(lx);
    vt_get(lx);
    size_t s2 = lx->cur;
    while (vt_peek(lx) == '0' || vt_peek(lx) == '_' || vt_peek(lx) == '1')
      vt_get(lx);
    if (lx->cur == s2)
      return vt_tok_err(lx, p, "binary literal requires digits");
    const char* lex = lx->src + s2;
    uint32_t n = (uint32_t)(lx->cur - s2);
    u64p r = vt_parse_u64_base(lex, n, 2);
    if (!r.ok) return vt_tok_err(lx, p, "invalid binary literal");
    vt_token t =
        vt_tok_make(TK_INT, p, lx->src + start, (uint32_t)(lx->cur - start));
    t.num.u64 = r.u;
    t.num.is_neg = 0;
    return t;
  }

  /* décimal ou float: [0-9][_0-9]* ('.' [0-9][_0-9]*)? (exp)? | d+(exp) */
  while (vt_is_digit(vt_peek(lx)) || vt_peek(lx) == '_') vt_get(lx);

  /* fraction ? */
  if (vt_peek(lx) == '.' && vt_peek2(lx) != '.') { /* éviter range .. */
    is_float = 1;
    vt_get(lx);
    while (vt_is_digit(vt_peek(lx)) || vt_peek(lx) == '_') vt_get(lx);
  }

  /* exp ? */
  if (vt_peek(lx) == 'e' || vt_peek(lx) == 'E') {
    is_float = 1;
    size_t save = lx->cur;
    vt_get(lx);
    if (vt_peek(lx) == '+' || vt_peek(lx) == '-') vt_get(lx);
    if (!vt_is_digit(vt_peek(lx))) {
      lx->cur = save;
    } else {
      while (vt_is_digit(vt_peek(lx)) || vt_peek(lx) == '_') vt_get(lx);
    }
  }

  const char* lex = lx->src + start;
  uint32_t n = (uint32_t)(lx->cur - start);

  if (is_float) {
    f64p r = vt_parse_float(lex, n);
    if (!r.ok) return vt_tok_err(lx, p, "invalid float literal");
    vt_token t = vt_tok_make(TK_FLOAT, p, lex, n);
    t.num.f64 = r.f;
    t.num.is_neg = 0;
    return t;
  } else {
    if (!vt_all_digits_underscore(lex, n))
      return vt_tok_err(lx, p, "invalid decimal literal");
    u64p r = vt_parse_u64_base(lex, n, 10);
    if (!r.ok) return vt_tok_err(lx, p, "integer overflow");
    vt_token t = vt_tok_make(TK_INT, p, lex, n);
    t.num.u64 = r.u;
    t.num.is_neg = 0;
    return t;
  }
}

static vt_token vt_lex_ident_or_kw(vt_lexer* lx, vt_src_pos p) {
  size_t start = lx->cur;
  vt_get(lx); /* premier LETTER déjà vu par l’appelant */
  while (vt_is_letter(vt_peek(lx)) || vt_is_digit(vt_peek(lx))) vt_get(lx);
  const char* s = lx->src + start;
  uint32_t n = (uint32_t)(lx->cur - start);
  vt_tok_kind kk = vt_kw_lookup(s, n);
  if (kk == TK_KW_true || kk == TK_KW_false) {
    vt_token t = vt_tok_make(TK_BOOL, p, s, n);
    t.bool_val = (kk == TK_KW_true);
    return t;
  }
  if (kk != TK_IDENT) return vt_tok_make(kk, p, s, n);
  return vt_tok_make(TK_IDENT, p, s, n);
}

static vt_token vt_lex_string(vt_lexer* lx, vt_src_pos p) {
  /* consomme l'ouverture " */
  vt_get(lx);
  size_t start = lx->cur;
  int ok = 1;
  while (!vt_eof(lx)) {
    char c = vt_get(lx);
    if (c == '\"') break;
    if (c == '\\') {
      if (vt_eof(lx)) {
        ok = 0;
        break;
      }
      char e = vt_get(lx);
      if (!(e == '\\' || e == '\"' || e == 'n' || e == 'r' || e == 't' ||
            e == '0' || e == 'x' || e == '\'')) {
        ok = 0;
        break;
      }
      if (e == 'x') {
        if (!vt_is_hexd(vt_peek(lx)) || !vt_is_hexd(vt_peekn(lx, 1))) {
          ok = 0;
          break;
        }
        vt_get(lx);
        vt_get(lx);
      }
    } else if ((unsigned char)c < 0x20) {
      ok = 0;
      break;
    }
  }
  if (vt_eof(lx) || lx->src[lx->cur - 1] != '\"' || !ok) {
    return vt_tok_err(lx, p, "unterminated or invalid string literal");
  }
  const char* lex = lx->src + start - 1; /* incl. opening " */
  uint32_t n = (uint32_t)(lx->cur - (start - 1));
  return vt_tok_make(TK_STRING, p, lex, n);
}

static vt_token vt_lex_char(vt_lexer* lx, vt_src_pos p) {
  vt_get(lx); /* ' */
  if (vt_eof(lx)) return vt_tok_err(lx, p, "unterminated char literal");
  char c = vt_get(lx);
  char tmp[8];
  int outn = 0;
  if (c == '\\') {
    /* escape */
    size_t save = lx->cur;
    char esc = vt_peek(lx);
    if (esc == 'x') {
      vt_get(lx);
      if (!vt_is_hexd(vt_peek(lx)) || !vt_is_hexd(vt_peekn(lx, 1)))
        return vt_tok_err(lx, p, "invalid \\xHH in char literal");
      tmp[outn++] = (char)0; /* value computed by unescape later if needed */
      vt_get(lx);
      vt_get(lx);
    } else if (esc == 'n' || esc == 'r' || esc == 't' || esc == '0' ||
               esc == '\\' || esc == '\'' || esc == '\"') {
      vt_get(lx);
      tmp[outn++] = 'x'; /* marker */
    } else {
      (void)save;
      return vt_tok_err(lx, p, "invalid escape in char literal");
    }
  } else {
    if ((unsigned char)c < 0x20)
      return vt_tok_err(lx, p, "control char not allowed");
    tmp[outn++] = c;
  }
  if (vt_peek(lx) != '\'')
    return vt_tok_err(lx, p, "char literal must contain 1 code unit");
  vt_get(lx);
  const char* lex = lx->src + p.offset;
  uint32_t n = (uint32_t)(lx->cur - p.offset);
  return vt_tok_make(TK_CHAR, p, lex, n);
}

static vt_token vt_make_simple(vt_lexer* lx, vt_src_pos p, vt_tok_kind k,
                               size_t len) {
  vt_token t = vt_tok_make(k, p, lx->src + p.offset, (uint32_t)len);
  /* consommer len-1 car l’appelant a déjà consommé 1 via vt_get? On va tout
   * consommer ici. */
  for (size_t i = 0; i < len; i++) vt_get(lx);
  return t;
}

/* next brut (sans skip) pour traiter op multi-char */
static vt_token vt_lex_one(vt_lexer* lx) {
  vt_src_pos p = vt_pos_now(lx);
  char c = vt_peek(lx);
  if (c == '\0') return vt_tok_make(TK_EOF, p, lx->src + lx->cur, 0);

  /* ident/kw */
  if (vt_is_letter(c)) return vt_lex_ident_or_kw(lx, p);

  /* nombre */
  if (vt_is_digit(c)) return vt_lex_number(lx, p);

  /* string/char */
  if (c == '\"') return vt_lex_string(lx, p);
  if (c == '\'') return vt_lex_char(lx, p);

  /* opérateurs et ponctuation */
  /* deux/ trois caractères d’abord */
  char c1 = c, c2 = vt_peek2(lx), c3 = vt_peekn(lx, 2);

  /* :: */
  if (c1 == ':' && c2 == ':') return vt_make_simple(lx, p, TK_DCOLON, 2);
  /* .. / ..= / . */
  if (c1 == '.') {
    if (c2 == '.' && c3 == '=') return vt_make_simple(lx, p, TK_DOTDOTEQ, 3);
    if (c2 == '.') return vt_make_simple(lx, p, TK_DOTDOT, 2);
    return vt_make_simple(lx, p, TK_DOT, 1);
  }
  /* -> , => */
  if (c1 == '-' && c2 == '>') return vt_make_simple(lx, p, TK_ARROW, 2);
  if (c1 == '=' && c2 == '>') return vt_make_simple(lx, p, TK_FATARROW, 2);

  /* ==, !=, <=, >= */
  if (c1 == '=' && c2 == '=') return vt_make_simple(lx, p, TK_EQEQ, 2);
  if (c1 == '!' && c2 == '=') return vt_make_simple(lx, p, TK_NEQ, 2);
  if (c1 == '<' && c2 == '=') return vt_make_simple(lx, p, TK_LTE, 2);
  if (c1 == '>' && c2 == '=') return vt_make_simple(lx, p, TK_GTE, 2);

  /* <<=, >>=, +=, -=, *=, /=, %=, &=, ^=, |= */
  if (c1 == '<' && c2 == '<' && c3 == '=')
    return vt_make_simple(lx, p, TK_SHLEQ, 3);
  if (c1 == '>' && c2 == '>' && c3 == '=')
    return vt_make_simple(lx, p, TK_SHREQ, 3);
  if (c1 == '+' && c2 == '=') return vt_make_simple(lx, p, TK_PLUSEQ, 2);
  if (c1 == '-' && c2 == '=') return vt_make_simple(lx, p, TK_MINUSEQ, 2);
  if (c1 == '*' && c2 == '=') return vt_make_simple(lx, p, TK_MULEQ, 2);
  if (c1 == '/' && c2 == '=') return vt_make_simple(lx, p, TK_DIVEQ, 2);
  if (c1 == '%' && c2 == '=') return vt_make_simple(lx, p, TK_MODEQ, 2);
  if (c1 == '&' && c2 == '=') return vt_make_simple(lx, p, TK_ANDEQ, 2);
  if (c1 == '^' && c2 == '=') return vt_make_simple(lx, p, TK_XOREQ, 2);
  if (c1 == '|' && c2 == '=') return vt_make_simple(lx, p, TK_OREQ, 2);

  /* ||, &&, <<, >> */
  if (c1 == '|' && c2 == '|') return vt_make_simple(lx, p, TK_LOR, 2);
  if (c1 == '&' && c2 == '&') return vt_make_simple(lx, p, TK_LAND, 2);
  if (c1 == '<' && c2 == '<') return vt_make_simple(lx, p, TK_SHL, 2);
  if (c1 == '>' && c2 == '>') return vt_make_simple(lx, p, TK_SHR, 2);

  /* simples */
  switch (c1) {
    case '=':
      return vt_make_simple(lx, p, TK_EQ, 1);
    case '|':
      return vt_make_simple(lx, p, TK_BOR, 1);
    case '^':
      return vt_make_simple(lx, p, TK_BXOR, 1);
    case '&':
      return vt_make_simple(lx, p, TK_BAND, 1);
    case '<':
      return vt_make_simple(lx, p, TK_LT, 1);
    case '>':
      return vt_make_simple(lx, p, TK_GT, 1);
    case '+':
      return vt_make_simple(lx, p, TK_PLUS, 1);
    case '-':
      return vt_make_simple(lx, p, TK_MINUS, 1);
    case '*':
      return vt_make_simple(lx, p, TK_STAR, 1);
    case '/':
      return vt_make_simple(lx, p, TK_SLASH, 1);
    case '%':
      return vt_make_simple(lx, p, TK_PERCENT, 1);
    case '!':
      return vt_make_simple(lx, p, TK_BANG, 1);
    case '(':
      return vt_make_simple(lx, p, TK_LP, 1);
    case ')':
      return vt_make_simple(lx, p, TK_RP, 1);
    case '[':
      return vt_make_simple(lx, p, TK_LB, 1);
    case ']':
      return vt_make_simple(lx, p, TK_RB, 1);
    case '{':
      return vt_make_simple(lx, p, TK_LC, 1);
    case '}':
      return vt_make_simple(lx, p, TK_RC, 1);
    case ',':
      return vt_make_simple(lx, p, TK_COMMA, 1);
    case ':':
      return vt_make_simple(lx, p, TK_COLON, 1);
    case ';':
      return vt_make_simple(lx, p, TK_SEMI, 1);
    default:
      break;
  }

  /* caractère inconnu */
  vt_get(lx);
  return vt_tok_err(lx, p, "unknown character");
}

/* Obtenir le prochain token utile (ignore espaces/commentaires) */
vt_token vt_lex_next(vt_lexer* lx) {
  if (lx->has_la) {
    lx->has_la = 0;
    return lx->la;
  }
  vt_skip_ws(lx);
  vt_token t = vt_lex_one(lx);
  /* Remapper true/false déjà traité en TK_BOOL. Les autres restes inchangés. */
  return t;
}

/* Peek (1-token lookahead) */
vt_token vt_lex_peek(vt_lexer* lx) {
  if (!lx->has_la) {
    lx->la = vt_lex_next(lx);
    lx->has_la = 1;
  }
  return lx->la;
}

/* Expect */
int vt_lex_expect(vt_lexer* lx, vt_tok_kind k, vt_token* out,
                  const char** err) {
  vt_token t = vt_lex_next(lx);
  if (t.kind != k) {
    if (err) {
      *err = vt_lex_format_error(lx, "unexpected token", t.pos);
    }
    return 0;
  }
  if (out) *out = t;
  return 1;
}

/* Décodage pratique des strings/char après coup
   - Pour TK_STRING: enlève les quotes et unescape dans buffer fourni
   - Pour TK_CHAR: idem, renvoie 1 code-unit (longueur 1) */
int vt_lex_decode_string(const vt_token* t, char* out, uint32_t outcap,
                         uint32_t* outlen) {
  if (t->kind != TK_STRING) return 0;
  const char* s = t->lex + 1; /* skip " */
  uint32_t n = t->len >= 2 ? t->len - 2 : 0;
  int k = vt_unescape(s, n, out, outcap, '\"');
  if (k < 0) return 0;
  if (outlen) *outlen = (uint32_t)k;
  return 1;
}
int vt_lex_decode_char(const vt_token* t, char* out) {
  if (t->kind != TK_CHAR) return 0;
  const char* s = t->lex + 1; /* between quotes */
  uint32_t n = t->len >= 2 ? t->len - 2 : 0;
  char tmp[8];
  int k = vt_unescape(s, n, tmp, sizeof tmp, '\'');
  if (k != 1) return 0;
  *out = tmp[0];
  return 1;
}

/* Dump simple pour debug */
const char* vt_tok_name(vt_tok_kind k) {
  switch (k) {
#define C(x) \
  case x:    \
    return #x;
    C(TK_EOF)
    C(TK_ERROR)
    C(TK_IDENT) C(TK_INT) C(TK_FLOAT) C(TK_BOOL) C(TK_CHAR) C(TK_STRING) C(
        TK_KW_module) C(TK_KW_import) C(TK_KW_use) C(TK_KW_as) C(TK_KW_pub)
        C(TK_KW_const) C(TK_KW_let) C(TK_KW_mut) C(TK_KW_fn) C(TK_KW_return) C(
            TK_KW_if) C(TK_KW_else) C(TK_KW_match) C(TK_KW_while) C(TK_KW_for)
            C(TK_KW_in) C(TK_KW_break) C(TK_KW_continue) C(TK_KW_type)
                C(TK_KW_impl) C(TK_KW_where) C(TK_KW_test) C(TK_KW_true) C(
                    TK_KW_false) C(TK_EQ) C(TK_PLUSEQ) C(TK_MINUSEQ) C(TK_MULEQ)
                    C(TK_DIVEQ) C(TK_MODEQ) C(TK_SHLEQ) C(TK_SHREQ) C(TK_ANDEQ)
                        C(TK_XOREQ) C(TK_OREQ) C(TK_LOR) C(TK_LAND) C(TK_BOR) C(
                            TK_BXOR) C(TK_BAND) C(TK_EQEQ) C(TK_NEQ) C(TK_LT)
                            C(TK_LTE) C(TK_GT) C(TK_GTE) C(TK_SHL) C(TK_SHR)
                                C(TK_PLUS) C(TK_MINUS) C(TK_STAR) C(TK_SLASH)
                                    C(TK_PERCENT) C(TK_BANG) C(TK_AMP) C(TK_DOT)
                                        C(TK_DOTDOT) C(TK_DOTDOTEQ) C(TK_LP)
                                            C(TK_RP) C(TK_LB) C(TK_RB) C(TK_LC)
                                                C(TK_RC) C(TK_COMMA) C(TK_COLON)
                                                    C(TK_SEMI) C(TK_DCOLON)
                                                        C(TK_ARROW)
                                                            C(TK_FATARROW)
#undef C
                                                                default
        : return "?";
  }
}

void vt_tok_dump(const vt_token* t) {
  printf("%-14s @%u:%u  lex=\"%.*s\"", vt_tok_name(t->kind), t->pos.line,
         t->pos.col, (int)t->len, t->lex);
  if (t->kind == TK_INT) printf("  u64=%llu", (unsigned long long)t->num.u64);
  if (t->kind == TK_FLOAT) printf("  f64=%.17g", t->num.f64);
  if (t->kind == TK_BOOL) printf("  bool=%s", t->bool_val ? "true" : "false");
  putchar('\n');
}

/* ---------------------------------------------------------------------------
   Exemple de main (désactivé par défaut)
   Compile: cc -std=c17 -O2 lex.c -DVT_LEX_MAIN
--------------------------------------------------------------------------- */
#ifdef VT_LEX_MAIN
int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file>\n", argv[0]);
    return 1;
  }
  vt_lexer lx;
  if (vt_lex_init_from_file(&lx, argv[1]) != 0) {
    fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
    return 1;
  }
  for (;;) {
    vt_token t = vt_lex_next(&lx);
    vt_tok_dump(&t);
    if (t.kind == TK_ERROR || t.kind == TK_EOF) break;
  }
  vt_lex_dispose_from_file(&lx);
  return 0;
}
#endif
