// SPDX-License-Identifier: GPL-3.0-or-later
//
// utf8lib.c — UTF-8 library for Vitte Light VM (C17, ultra complet)
// Namespace: "utf8"
//
// Conventions:
//   - Byte indexing is 1-based. Negative indexes are from end (-1 == last
//   byte).
//   - Codepoint indexing is 1-based. Negative indexes from end.
//   - Best-effort algorithms for width and grapheme counting (not full UAX#29).
//
// API:
//   Length, validation:
//     utf8.byte_len(s)                     -> int
//     utf8.len(s)                          -> int                         //
//     codepoints utf8.valid(s)                        -> bool, errpos:int //
//     errpos=0 if ok
//
//   Decode / encode:
//     utf8.decode_at(s, byte_index)        -> cp:int, nbytes:int |
//     (nil,"ERANGE"/"EINVAL") utf8.encode(cp:int)                  ->
//     bytes:string | (nil,"ERANGE")
//
//   Navigation (byte cursor in [1..len+1]):
//     utf8.next(s[, byte_index=1])         -> next_index:int, cp:int      //
//     0,0 if end utf8.prev(s[, byte_index=byte_len+1])-> prev_index:int, cp:int
//     // 0,0 if begin
//
//   Indexing by codepoints:
//     utf8.offset_of(s, cp_index)          -> byte_index:int              //
//     clamped to [1..len+1] utf8.cp_at(s, cp_index)              -> cp:int |
//     (nil,"ERANGE") utf8.sub(s, i[, j])                  -> substring by
//     codepoints utf8.find_cp(s, cp[, start_cp=1])    -> pos_cp:int (0 if not
//     found)
//
//   Replacement:
//     utf8.replace_cp(s, from_cp:int, to_cp:int[, max=-1]) -> out:string,
//     count:int
//
//   Display width (approx):
//     utf8.width_cp(cp:int)                -> int (0|1|2)
//     utf8.width(s)                        -> int                          //
//     sum of width_cp
//
//   Grapheme count (approx):
//     utf8.graphemes(s)                    -> int                          //
//     simple combining suppression
//
// Depends: auxlib.h, state.h, object.h, vm.h

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *u_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t u_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int u_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)u_check_int(S, idx);
  return defv;
}

// ---------------------------------------------------------------------
// Core UTF-8 helpers
// ---------------------------------------------------------------------
// Returns bytes consumed (1..4), 0 on invalid. Writes cp if out_cp!=NULL.
// Rejects overlongs and non-scalar (surrogates, > U+10FFFF).
static size_t u8_decode_one(const unsigned char *s, size_t n,
                            uint32_t *out_cp) {
  if (n == 0) return 0;
  unsigned char c0 = s[0];
  if (c0 < 0x80) {
    if (out_cp) *out_cp = c0;
    return 1;
  }
  if ((c0 & 0xE0) == 0xC0) {
    if (n < 2) return 0;
    unsigned char c1 = s[1];
    if ((c1 & 0xC0) != 0x80) return 0;
    uint32_t cp = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
    if (cp < 0x80) return 0;  // overlong
    if (out_cp) *out_cp = cp;
    return 2;
  }
  if ((c0 & 0xF0) == 0xE0) {
    if (n < 3) return 0;
    unsigned char c1 = s[1], c2 = s[2];
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return 0;
    uint32_t cp = ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(c1 & 0x3F) << 6) |
                  (uint32_t)(c2 & 0x3F);
    if (cp < 0x800) return 0;                    // overlong
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;  // surrogate
    if (out_cp) *out_cp = cp;
    return 3;
  }
  if ((c0 & 0xF8) == 0xF0) {
    if (n < 4) return 0;
    unsigned char c1 = s[1], c2 = s[2], c3 = s[3];
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
      return 0;
    uint32_t cp = ((uint32_t)(c0 & 0x07) << 18) |
                  ((uint32_t)(c1 & 0x3F) << 12) | ((uint32_t)(c2 & 0x3F) << 6) |
                  (uint32_t)(c3 & 0x3F);
    if (cp < 0x10000 || cp > 0x10FFFF) return 0;  // overlong or out of range
    if (out_cp) *out_cp = cp;
    return 4;
  }
  return 0;
}

static int u8_encode_one(uint32_t cp, char out[4]) {
  if (cp <= 0x7F) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp <= 0x07FF) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
  if (cp <= 0xFFFF) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }
  return 0;
}

static size_t u8_strlen(const unsigned char *s, size_t n) {
  size_t i = 0, cnt = 0;
  while (i < n) {
    size_t c = u8_decode_one(s + i, n - i, NULL);
    if (!c) {  // invalid byte, treat as 1-byte replacement advance
      i++;
      cnt++;
      continue;
    }
    i += c;
    cnt++;
  }
  return cnt;
}

static int u8_valid_full(const unsigned char *s, size_t n, size_t *errpos) {
  size_t i = 0;
  while (i < n) {
    size_t c = u8_decode_one(s + i, n - i, NULL);
    if (!c) {
      if (errpos) *errpos = i + 1;
      return 0;
    }
    i += c;
  }
  if (errpos) *errpos = 0;
  return 1;
}

// Move to previous codepoint start from byte index (1..n+1). Returns new index
// (1..n) or 0 if none.
static size_t u8_prev_start(const unsigned char *s, size_t n, size_t idx1b) {
  if (idx1b <= 1) return 0;
  size_t i = idx1b - 1;  // 1-based -> position before idx
  if (i > n) i = n;
  // step back over continuation bytes
  size_t k = i;
  size_t back = 0;
  while (k >= 1 && (s[k - 1] & 0xC0) == 0x80 && back < 3) {
    k--;
    back++;
  }
  // k now at a start byte candidate
  return k;
}

// Map cp index to byte offset (1-based). Clamp to [1..n+1].
static size_t u8_cp_to_byte(const unsigned char *s, size_t n, int64_t cpi) {
  // count total cps
  size_t total = u8_strlen(s, n);
  int64_t idx = cpi;
  if (idx < 0) idx = (int64_t)total + idx + 1;  // -1 -> total
  if (idx < 1) idx = 1;
  if ((uint64_t)idx > total + 1) idx = (int64_t)total + 1;

  // walk to cp idx, returning byte position of that cp (or end for total+1)
  size_t i = 0, cp = 1;
  if ((uint64_t)idx == total + 1) return n + 1;
  while (i < n && (int64_t)cp < idx) {
    size_t c = u8_decode_one(s + i, n - i, NULL);
    if (!c) c = 1;
    i += c;
    cp++;
  }
  return i + 1;  // 1-based
}

// ---------------------------------------------------------------------
// Display width (approx, Unicode-EastAsian-like)
// ---------------------------------------------------------------------
static int is_combining(uint32_t cp) {
  // Main combining blocks + VS + ZWJ
  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
      (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
      (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0x200D ||  // ZWJ
      (cp >= 0xFE00 && cp <= 0xFE0F)                     // VS selectors
  )
    return 1;
  return 0;
}
static int is_wide(uint32_t cp) {
  if ((cp >= 0x1100 && cp <= 0x115F) || cp == 0x2329 || cp == 0x232A ||
      (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
      (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
      (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) ||
      (cp >= 0x1F300 && cp <= 0x1FAFF) ||  // emoji blocks (approx)
      (cp >= 0x20000 && cp <= 0x3FFFD))
    return 1;
  return 0;
}
static int width_cp_approx(uint32_t cp) {
  if (cp == 0) return 0;
  if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;  // control
  if (is_combining(cp)) return 0;
  if (is_wide(cp)) return 2;
  return 1;
}

// ---------------------------------------------------------------------
// VM functions
// ---------------------------------------------------------------------

static int vm_u_byte_len(VL_State *S) {
  const char *s = u_check_str(S, 1);
  vl_push_int(S, (int64_t)strlen(s));
  return 1;
}

static int vm_u_len(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  vl_push_int(S, (int64_t)u8_strlen(s, strlen((const char *)s)));
  return 1;
}

static int vm_u_valid(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  size_t n = strlen((const char *)s);
  size_t err = 0;
  int ok = u8_valid_full(s, n, &err);
  vl_push_bool(S, ok ? 1 : 0);
  vl_push_int(S, (int64_t)err);
  return 2;
}

static int vm_u_decode_at(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  int64_t bi = u_check_int(S, 2);
  size_t n = strlen((const char *)s);
  if (n == 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  // clamp 1-based byte index
  size_t pos;
  if (bi < 0) {
    int64_t p = (int64_t)n + bi + 1;
    if (p < 1) p = 1;
    if ((uint64_t)p > n) p = (int64_t)n;
    pos = (size_t)p;
  } else {
    if (bi < 1) bi = 1;
    if ((uint64_t)bi > n) bi = (int64_t)n;
    pos = (size_t)bi;
  }
  uint32_t cp = 0;
  size_t used = u8_decode_one(s + (pos - 1), n - (pos - 1), &cp);
  if (!used) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_int(S, (int64_t)cp);
  vl_push_int(S, (int64_t)used);
  return 2;
}

static int vm_u_encode(VL_State *S) {
  uint32_t cp = (uint32_t)u_check_int(S, 1);
  char b[4];
  int m = u8_encode_one(cp, b);
  if (m == 0) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  vl_push_lstring(S, b, m);
  return 1;
}

static int vm_u_next(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  size_t n = strlen((const char *)s);
  int start = u_opt_int(S, 2, 1);
  if (start < 1) start = 1;
  if ((uint64_t)start > n + 1) start = (int)n + 1;
  if ((size_t)start == n + 1) {
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    return 2;
  }
  uint32_t cp = 0;
  size_t used = u8_decode_one(s + (start - 1), n - (start - 1), &cp);
  if (!used) {  // skip invalid byte
    vl_push_int(S, start + 1);
    vl_push_int(S, (int64_t)0xFFFD);
    return 2;
  }
  vl_push_int(S, (int64_t)(start + (int)used));
  vl_push_int(S, (int64_t)cp);
  return 2;
}

static int vm_u_prev(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  size_t n = strlen((const char *)s);
  int idx = u_opt_int(S, 2, (int)n + 1);
  if (idx < 1) idx = 1;
  if ((uint64_t)idx > n + 1) idx = (int)n + 1;
  size_t ps = u8_prev_start(s, n, (size_t)idx);
  if (ps == 0) {
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    return 2;
  }
  uint32_t cp = 0;
  size_t used = u8_decode_one(s + (ps - 1), n - (ps - 1), &cp);
  if (!used) {
    vl_push_int(S, (int64_t)(ps - 1));
    vl_push_int(S, (int64_t)0xFFFD);
    return 2;
  }
  vl_push_int(S, (int64_t)ps);
  vl_push_int(S, (int64_t)cp);
  return 2;
}

static int vm_u_offset_of(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  int64_t cpi = u_check_int(S, 2);
  size_t n = strlen((const char *)s);
  size_t off = u8_cp_to_byte(s, n, cpi);
  vl_push_int(S, (int64_t)off);
  return 1;
}

static int vm_u_cp_at(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  int64_t cpi = u_check_int(S, 2);
  size_t n = strlen((const char *)s);
  // compute start byte
  size_t off = u8_cp_to_byte(s, n, cpi);
  if (off == n + 1) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  uint32_t cp = 0;
  size_t used = u8_decode_one(s + (off - 1), n - (off - 1), &cp);
  if (!used) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_int(S, (int64_t)cp);
  return 1;
}

static int vm_u_sub(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  size_t n = strlen((const char *)s);
  int have_j = (vl_get(S, 3) != NULL);
  int64_t I = u_check_int(S, 2);
  int64_t J = have_j ? u_check_int(S, 3) : (int64_t)1e15;

  // total cps
  size_t total = u8_strlen(s, n);

  auto size_t cp_to_abs = [&](int64_t k) -> size_t {
    if (k < 0) k = (int64_t)total + k + 1;
    if (k < 1) k = 1;
    if ((uint64_t)k > total) k = (int64_t)total;
    return (size_t)k;
  };
  size_t a_cp = cp_to_abs(I);
  size_t b_cp = have_j ? cp_to_abs(J) : total;
  if (b_cp < a_cp || total == 0) {
    vl_push_string(S, "");
    return 1;
  }

  // Walk to byte offsets
  size_t a_byte = 0, b_byte = 0;
  size_t cp = 1, i = 0;
  while (i < n && cp < a_cp) {
    size_t c = u8_decode_one(s + i, n - i, NULL);
    if (!c) c = 1;
    i += c;
    cp++;
  }
  a_byte = i;
  while (i < n && cp <= b_cp) {
    size_t c = u8_decode_one(s + i, n - i, NULL);
    if (!c) c = 1;
    i += c;
    cp++;
  }
  b_byte = i;
  vl_push_lstring(S, (const char *)s + a_byte, (int)(b_byte - a_byte));
  return 1;
}

static int vm_u_find_cp(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  uint32_t needle = (uint32_t)u_check_int(S, 2);
  int start_cp = u_opt_int(S, 3, 1);
  size_t n = strlen((const char *)s);
  // start at cp index
  size_t startb = u8_cp_to_byte(s, n, start_cp);
  if (startb == n + 1) {
    vl_push_int(S, 0);
    return 1;
  }
  size_t i = startb - 1;
  size_t cpidx = (size_t)(start_cp < 1 ? 1 : start_cp);
  // recompute cpidx by scanning (safe)
  cpidx = 1;
  size_t tmp = 0;
  while (tmp < i) {
    size_t c = u8_decode_one(s + tmp, n - tmp, NULL);
    if (!c) c = 1;
    tmp += c;
    cpidx++;
  }

  while (i < n) {
    uint32_t cp = 0;
    size_t used = u8_decode_one(s + i, n - i, &cp);
    if (!used) {
      i++;
      cpidx++;
      continue;
    }
    if (cp == needle) {
      vl_push_int(S, (int64_t)cpidx);
      return 1;
    }
    i += used;
    cpidx++;
  }
  vl_push_int(S, 0);
  return 1;
}

static int vm_u_replace_cp(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  uint32_t from = (uint32_t)u_check_int(S, 2);
  uint32_t to = (uint32_t)u_check_int(S, 3);
  int maxrep = u_opt_int(S, 4, -1);
  size_t n = strlen((const char *)s);

  char enc[4];
  int encn = u8_encode_one(to, enc);
  if (!encn) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }

  AuxBuffer out = {0};
  size_t i = 0, count = 0;
  while (i < n) {
    uint32_t cp = 0;
    size_t used = u8_decode_one(s + i, n - i, &cp);
    if (!used) {  // invalid -> pass-through
      aux_buffer_append(&out, s + i, 1);
      i++;
      continue;
    }
    if (cp == from && (maxrep < 0 || (int)count < maxrep)) {
      aux_buffer_append(&out, (const uint8_t *)enc, (size_t)encn);
      count++;
    } else {
      aux_buffer_append(&out, s + i, used);
    }
    i += used;
  }
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  vl_push_int(S, (int64_t)count);
  aux_buffer_free(&out);
  return 2;
}

static int vm_u_width_cp(VL_State *S) {
  uint32_t cp = (uint32_t)u_check_int(S, 1);
  vl_push_int(S, (int64_t)width_cp_approx(cp));
  return 1;
}

static int vm_u_width(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  size_t n = strlen((const char *)s);
  int64_t w = 0;
  size_t i = 0;
  while (i < n) {
    uint32_t cp = 0;
    size_t used = u8_decode_one(s + i, n - i, &cp);
    if (!used) {
      i++;
      continue;
    }
    w += width_cp_approx(cp);
    i += used;
  }
  vl_push_int(S, w);
  return 1;
}

static int vm_u_graphemes(VL_State *S) {
  const unsigned char *s = (const unsigned char *)u_check_str(S, 1);
  size_t n = strlen((const char *)s);
  size_t i = 0;
  int64_t g = 0;
  int in_cluster = 0;
  while (i < n) {
    uint32_t cp = 0;
    size_t used = u8_decode_one(s + i, n - i, &cp);
    if (!used) {
      i++;
      in_cluster = 0;
      g++;
      continue;
    }
    if (!in_cluster) {
      g++;
      in_cluster = 1;
    }
    // combine marks and joiners keep cluster open
    if (!is_combining(cp) && cp != 0x200D) {
      in_cluster = 0;
    }
    i += used;
  }
  vl_push_int(S, g);
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------
static const VL_Reg utf8lib[] = {{"byte_len", vm_u_byte_len},
                                 {"len", vm_u_len},
                                 {"valid", vm_u_valid},

                                 {"decode_at", vm_u_decode_at},
                                 {"encode", vm_u_encode},

                                 {"next", vm_u_next},
                                 {"prev", vm_u_prev},

                                 {"offset_of", vm_u_offset_of},
                                 {"cp_at", vm_u_cp_at},
                                 {"sub", vm_u_sub},
                                 {"find_cp", vm_u_find_cp},
                                 {"replace_cp", vm_u_replace_cp},

                                 {"width_cp", vm_u_width_cp},
                                 {"width", vm_u_width},
                                 {"graphemes", vm_u_graphemes},

                                 {NULL, NULL}};

void vl_open_utf8lib(VL_State *S) { vl_register_lib(S, "utf8", utf8lib); }
