// SPDX-License-Identifier: GPL-3.0-or-later
//
// font.c — FreeType + HarfBuzz bindings for Vitte Light VM (C17, complete)
// Namespace: "font"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_FREETYPE
//   -DVL_HAVE_HARFBUZZ -c font.c cc ... font.o -lfreetype -lharfbuzz
//
// Model:
//   - One handle id = one FT_Face + hb_font_t pair.
//   - Shaping with HarfBuzz -> USV rows per glyph:
//   gid,cluster,x_adv,y_adv,x_off,y_off (float px).
//   - Rasterization: A8 bitmap from glyph sequence (AA). Returns bbox and
//   baseline origin.
//
// API:
//   Init/Done
//     font.init() -> true | (nil,errmsg)
//     font.done() -> true
//     font.version() -> string
//
//   Faces
//     font.load(path[, face_index=0]) -> id | (nil,errmsg)
//     font.free(id) -> true
//     font.set_size(id, px:number[, dpi:int=96]) -> ascender_px:number,
//     descender_px:number, height_px:number | (nil,errmsg) font.info(id) ->
//     family, style, units_per_em:int, has_kerning:int, is_color:int |
//     (nil,errmsg)
//
//   Shaping
//     font.shape(id, utf8[, lang=""[, script=""[, dir="ltr"]]])
//       -> usv:string  // rows: gid:uint, cluster:int, x_adv:float,
//       y_adv:float, x_off:float, y_off:float
//
//   Rendering
//     font.rasterize(id, layout_usv[, aa=true])
//       -> w:int, h:int, origin_x:int, origin_y:int, a8_bitmap:string |
//       (nil,errmsg)
//
// Notes:
//   - Positions from HarfBuzz are in 26.6; API returns pixel floats
//   (value/64.0).
//   - VM strings must not contain NUL on input. Outputs use vl_push_lstring for
//   binary.
//
// Deps: auxlib.h, state.h, object.h, vm.h

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#define US 0x1F
#define RS 0x1E

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *ft_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t ft_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static double ft_check_num(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? (double)vl_toint(S, vl_get(S, idx))
                            : vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: number expected", idx);
  vl_error(S);
  return 0.0;
}
static int ft_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static const char *ft_opt_str(VL_State *S, int idx, const char *defv) {
  if (!vl_get(S, idx) || !vl_isstring(S, idx)) return defv;
  return ft_check_str(S, idx);
}

#if defined(VL_HAVE_FREETYPE) && defined(VL_HAVE_HARFBUZZ)
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <hb-ft.h>
#include <hb.h>
#define FONT_STUB 0
#else
#define FONT_STUB 1
#endif

// ---------------------------------------------------------------------
// Stubs
// ---------------------------------------------------------------------
#if FONT_STUB

#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)
static int vlf_init(VL_State *S) {
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
}
static int vlf_done(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}
static int vlf_version(VL_State *S) {
  vl_push_string(S, "unavailable");
  return 1;
}
static int vlf_load(VL_State *S) {
  (void)ft_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlf_free(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}
static int vlf_set_size(VL_State *S) { NOSYS_PAIR(S); }
static int vlf_info(VL_State *S) { NOSYS_PAIR(S); }
static int vlf_shape(VL_State *S) { NOSYS_PAIR(S); }
static int vlf_raster(VL_State *S) { NOSYS_PAIR(S); }

#else
// ---------------------------------------------------------------------
// Real implementation
// ---------------------------------------------------------------------

typedef struct FaceH {
  int used;
  FT_Face face;
  hb_font_t *hb_font;
} FaceH;

static FT_Library g_ft = NULL;
static FaceH *g_faces = NULL;
static int g_cap = 0;

static int ensure_cap(int need) {
  if (need <= g_cap) return 1;
  int n = g_cap ? g_cap : 8;
  while (n < need) n <<= 1;
  FaceH *nf = (FaceH *)realloc(g_faces, (size_t)n * sizeof *nf);
  if (!nf) return 0;
  for (int i = g_cap; i < n; i++) {
    nf[i].used = 0;
    nf[i].face = NULL;
    nf[i].hb_font = NULL;
  }
  g_faces = nf;
  g_cap = n;
  return 1;
}
static int alloc_face(void) {
  for (int i = 1; i < g_cap; i++)
    if (!g_faces[i].used) return i;
  if (!ensure_cap(g_cap ? g_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_cap; i++)
    if (!g_faces[i].used) return i;
  return 0;
}
static int chk_id(int id) {
  return id > 0 && id < g_cap && g_faces[id].used && g_faces[id].face &&
         g_faces[id].hb_font;
}

static int vlf_init(VL_State *S) {
  if (!g_ft) {
    FT_Error e = FT_Init_FreeType(&g_ft);
    if (e) {
      vl_push_nil(S);
      vl_push_string(S, "freetype");
      return 2;
    }
  }
  vl_push_bool(S, 1);
  return 1;
}
static int vlf_done(VL_State *S) {
  if (g_faces) {
    for (int i = 1; i < g_cap; i++) {
      if (g_faces[i].used) {
        if (g_faces[i].hb_font) hb_font_destroy(g_faces[i].hb_font);
        if (g_faces[i].face) FT_Done_Face(g_faces[i].face);
        g_faces[i].used = 0;
        g_faces[i].face = NULL;
        g_faces[i].hb_font = NULL;
      }
    }
  }
  if (g_ft) {
    FT_Done_FreeType(g_ft);
    g_ft = NULL;
  }
  vl_push_bool(S, 1);
  return 1;
}
static int vlf_version(VL_State *S) {
  char buf[128];
  snprintf(buf, sizeof buf, "freetype %d.%d.%d, harfbuzz %u.%u.%u",
           (int)FREETYPE_MAJOR, (int)FREETYPE_MINOR, (int)FREETYPE_PATCH,
           (unsigned)HB_VERSION_MAJOR, (unsigned)HB_VERSION_MINOR,
           (unsigned)HB_VERSION_MICRO);
  vl_push_string(S, buf);
  return 1;
}

static int vlf_load(VL_State *S) {
  const char *path = ft_check_str(S, 1);
  int face_index = (int)ft_check_int(S, 2);
  if (!g_ft) {
    FT_Error e = FT_Init_FreeType(&g_ft);
    if (e) {
      vl_push_nil(S);
      vl_push_string(S, "freetype");
      return 2;
    }
  }

  FT_Face face = NULL;
  FT_Error e = FT_New_Face(g_ft, path, face_index, &face);
  if (e || !face) {
    vl_push_nil(S);
    vl_push_string(S, "ENOENT");
    return 2;
  }

  // Default size to 12px for sensible metrics until set_size is called
  FT_Set_Char_Size(face, 0, 12 * 64, 96, 96);

  hb_font_t *hb_font = hb_ft_font_create_referenced(face);
  if (!hb_font) {
    FT_Done_Face(face);
    vl_push_nil(S);
    vl_push_string(S, "harfbuzz");
    return 2;
  }

  int id = alloc_face();
  if (!id) {
    hb_font_destroy(hb_font);
    FT_Done_Face(face);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_faces[id].used = 1;
  g_faces[id].face = face;
  g_faces[id].hb_font = hb_font;

  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlf_free(VL_State *S) {
  int id = (int)ft_check_int(S, 1);
  if (id > 0 && id < g_cap && g_faces[id].used) {
    if (g_faces[id].hb_font) hb_font_destroy(g_faces[id].hb_font);
    if (g_faces[id].face) FT_Done_Face(g_faces[id].face);
    g_faces[id].hb_font = NULL;
    g_faces[id].face = NULL;
    g_faces[id].used = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

static int vlf_set_size(VL_State *S) {
  int id = (int)ft_check_int(S, 1);
  double px = ft_check_num(S, 2);
  int dpi = (int)ft_check_int(S, 3);
  if (dpi <= 0) dpi = 96;
  if (!chk_id(id) || px <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  FT_Error e =
      FT_Set_Char_Size(g_faces[id].face, 0, (FT_F26Dot6)(px * 64.0), dpi, dpi);
  if (e) {
    vl_push_nil(S);
    vl_push_string(S, "freetype");
    return 2;
  }

  // hb-ft picks up FT size via metrics on demand; ensure scale sync
  hb_ft_font_changed(g_faces[id].hb_font);

  double asc = g_faces[id].face->size->metrics.ascender / 64.0;
  double desc = g_faces[id].face->size->metrics.descender / 64.0;  // negative
  double hgt = g_faces[id].face->size->metrics.height / 64.0;

  vl_push_float(S, asc);
  vl_push_float(S, desc);
  vl_push_float(S, hgt);
  return 3;
}

static int vlf_info(VL_State *S) {
  int id = (int)ft_check_int(S, 1);
  if (!chk_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  FT_Face f = g_faces[id].face;
  vl_push_string(S, f->family_name ? f->family_name : "");
  vl_push_string(S, f->style_name ? f->style_name : "");
  vl_push_int(S, (int64_t)f->units_per_EM);
  vl_push_int(S, (f->face_flags & FT_FACE_FLAG_KERNING) ? 1 : 0);
  vl_push_int(S, (f->face_flags & FT_FACE_FLAG_COLOR) ? 1 : 0);
  return 5;
}

static hb_direction_t parse_dir(const char *d) {
  if (!d) return HB_DIRECTION_LTR;
  if (strcmp(d, "rtl") == 0) return HB_DIRECTION_RTL;
  if (strcmp(d, "ttb") == 0) return HB_DIRECTION_TTB;
  if (strcmp(d, "btt") == 0) return HB_DIRECTION_BTT;
  return HB_DIRECTION_LTR;
}

// font.shape(id, text, lang, script, dir) -> USV rows
static int vlf_shape(VL_State *S) {
  int id = (int)ft_check_int(S, 1);
  const char *txt = ft_check_str(S, 2);
  const char *lang = ft_opt_str(S, 3, "");
  const char *script = ft_opt_str(S, 4, "");
  const char *dir = ft_opt_str(S, 5, "ltr");
  if (!chk_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  hb_buffer_t *buf = hb_buffer_create();
  hb_buffer_add_utf8(buf, txt, (int)strlen(txt), 0, (int)strlen(txt));
  if (lang && *lang)
    hb_buffer_set_language(buf, hb_language_from_string(lang, -1));
  if (script && *script)
    hb_buffer_set_script(buf, hb_script_from_string(script, -1));
  else
    hb_buffer_guess_segment_properties(buf);
  hb_buffer_set_direction(buf, parse_dir(dir));

  hb_shape(g_faces[id].hb_font, buf, NULL, 0);

  unsigned n = hb_buffer_get_length(buf);
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, NULL);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, NULL);

  AuxBuffer out = {0};
  char tmp[64];
  for (unsigned i = 0; i < n; i++) {
    // gid
    snprintf(tmp, sizeof tmp, "%u", info[i].codepoint);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    uint8_t u = US;
    aux_buffer_append(&out, &u, 1);
    // cluster (byte offset in UTF-8)
    snprintf(tmp, sizeof tmp, "%u", info[i].cluster);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    u = US;
    aux_buffer_append(&out, &u, 1);
    // x_adv, y_adv
    double xadv = pos[i].x_advance / 64.0, yadv = pos[i].y_advance / 64.0;
    snprintf(tmp, sizeof tmp, "%.6g", xadv);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    u = US;
    aux_buffer_append(&out, &u, 1);
    snprintf(tmp, sizeof tmp, "%.6g", yadv);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    u = US;
    aux_buffer_append(&out, &u, 1);
    // x_off, y_off
    double xoff = pos[i].x_offset / 64.0, yoff = pos[i].y_offset / 64.0;
    snprintf(tmp, sizeof tmp, "%.6g", xoff);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    u = US;
    aux_buffer_append(&out, &u, 1);
    snprintf(tmp, sizeof tmp, "%.6g", yoff);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    uint8_t r = RS;
    aux_buffer_append(&out, &r, 1);
  }

  hb_buffer_destroy(buf);
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

// Parse a double from ascii (len-limited), fallback 0
static double parse_d(const uint8_t *s, size_t n) {
  char buf[64];
  if (n >= sizeof buf) n = sizeof buf - 1;
  memcpy(buf, s, n);
  buf[n] = 0;
  return atof(buf);
}

// font.rasterize(id, usv[, aa=true]) -> w,h,ox,oy, a8
static int vlf_raster(VL_State *S) {
  int id = (int)ft_check_int(S, 1);
  const char *usv = ft_check_str(S, 2);
  int aa = ft_opt_bool(S, 3, 1);
  if (!chk_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  FT_Face face = g_faces[id].face;
  const uint8_t *p = (const uint8_t *)usv;
  size_t n = strlen(usv);

  // First pass: compute bbox
  double pen_x = 0.0, pen_y = 0.0;
  double min_x = 1e9, min_y = 1e9, max_x = -1e9, max_y = -1e9;

  size_t i = 0;
  while (i <= n) {
    int at_end = (i == n);
    uint8_t c = at_end ? RS : p[i];
    if (c == RS || c == US) {
      // parse row fields when hitting RS: we need gid and offsets and advances
      // We backtrack find row start
    }
    // Parse row: gid|cluster|xadv|yadv|xoff|yoff RS
    // Implement a small parser per row
    size_t start = i;
    // find end of row
    while (i < n && p[i] != RS) i++;
    size_t row_end = i;
    if (row_end == start) {
      i++;
      continue;
    }  // empty row
    // split by US into 6 fields
    const uint8_t *f[6] = {0};
    size_t L[6] = {0};
    int fi = 0;
    size_t k = start;
    size_t last = start;
    while (k < row_end && fi < 6) {
      if (p[k] == US) {
        f[fi] = p + last;
        L[fi] = k - last;
        fi++;
        last = k + 1;
      }
      k++;
    }
    if (fi < 5) {
      i++;
      continue;
    }  // malformed
    f[fi] = p + last;
    L[fi] = row_end - last;  // sixth
    // read values
    // gid
    unsigned gid = 0;
    for (size_t t = 0; t < L[0]; t++) {
      if (f[0][t] < '0' || f[0][t] > '9') {
        gid = 0;
        break;
      }
      gid = gid * 10 + (unsigned)(f[0][t] - '0');
    }
    double xadv = parse_d(f[2], L[2]);
    double yadv = parse_d(f[3], L[3]);
    double xoff = parse_d(f[4], L[4]);
    double yoff = parse_d(f[5], L[5]);

    // load glyph to get bitmap bbox
    if (FT_Load_Glyph(face, (FT_UInt)gid, FT_LOAD_DEFAULT)) { /*skip*/
    } else {
      if (FT_Render_Glyph(face->glyph, aa ? FT_RENDER_MODE_NORMAL
                                          : FT_RENDER_MODE_MONO) == 0) {
        FT_GlyphSlot g = face->glyph;
        double gx = pen_x + xoff + g->bitmap_left;
        double gy = pen_y - yoff - g->bitmap_top;  // y grows down in bitmaps
        double gw = g->bitmap.width;
        double gh = g->bitmap.rows;
        if (gw > 0 && gh > 0) {
          if (gx < min_x) min_x = gx;
          if (gy < min_y) min_y = gy;
          if (gx + gw > max_x) max_x = gx + gw;
          if (gy + gh > max_y) max_y = gy + gh;
        }
      }
    }
    pen_x += xadv;
    pen_y += yadv;

    i = (row_end < n) ? row_end + 1 : row_end;
  }

  if (!(max_x > min_x && max_y > min_y)) {
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    vl_push_lstring(S, "", 0);
    return 5;
  }

  int ox = (int)floor(min_x);
  int oy = (int)floor(min_y);
  int w = (int)ceil(max_x) - ox;
  int h = (int)ceil(max_y) - oy;

  uint8_t *a8 = (uint8_t *)calloc((size_t)w * (size_t)h, 1);
  if (!a8) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  // Second pass: blit
  pen_x = 0.0;
  pen_y = 0.0;
  i = 0;
  while (i <= n) {
    size_t start = i;
    while (i < n && p[i] != RS) i++;
    size_t row_end = i;
    if (row_end == start) {
      i++;
      continue;
    }
    const uint8_t *f[6] = {0};
    size_t L[6] = {0};
    int fi = 0;
    size_t k = start;
    size_t last = start;
    while (k < row_end && fi < 6) {
      if (p[k] == US) {
        f[fi] = p + last;
        L[fi] = k - last;
        fi++;
        last = k + 1;
      }
      k++;
    }
    if (fi < 5) {
      i = (row_end < n) ? row_end + 1 : row_end;
      continue;
    }
    f[fi] = p + last;
    L[fi] = row_end - last;

    unsigned gid = 0;
    for (size_t t = 0; t < L[0]; t++) {
      if (f[0][t] < '0' || f[0][t] > '9') {
        gid = 0;
        break;
      }
      gid = gid * 10 + (unsigned)(f[0][t] - '0');
    }
    double xadv = parse_d(f[2], L[2]);
    double yadv = parse_d(f[3], L[3]);
    double xoff = parse_d(f[4], L[4]);
    double yoff = parse_d(f[5], L[5]);

    if (FT_Load_Glyph(face, (FT_UInt)gid, FT_LOAD_DEFAULT) == 0) {
      if (FT_Render_Glyph(face->glyph, aa ? FT_RENDER_MODE_NORMAL
                                          : FT_RENDER_MODE_MONO) == 0) {
        FT_GlyphSlot g = face->glyph;
        int dst_x = (int)floor(pen_x + xoff + g->bitmap_left) - ox;
        int dst_y = (int)floor(pen_y - yoff - g->bitmap_top) - oy;
        FT_Bitmap *bm = &g->bitmap;

        if (bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
          for (int yy = 0; yy < (int)bm->rows; yy++) {
            int ty = dst_y + yy;
            if (ty < 0 || ty >= h) continue;
            const uint8_t *src = bm->buffer + (size_t)yy * bm->pitch;
            uint8_t *dst = a8 + (size_t)ty * w;
            for (int xx = 0; xx < (int)bm->width; xx++) {
              int tx = dst_x + xx;
              if (tx < 0 || tx >= w) continue;
              uint8_t s = src[xx];
              // simple "over" on A8: max
              uint8_t *pd = &dst[tx];
              *pd = (s > *pd) ? s : *pd;
            }
          }
        } else if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
          for (int yy = 0; yy < (int)bm->rows; yy++) {
            int ty = dst_y + yy;
            if (ty < 0 || ty >= h) continue;
            const uint8_t *src = bm->buffer + (size_t)yy * bm->pitch;
            uint8_t *dst = a8 + (size_t)ty * w;
            for (int xx = 0; xx < (int)bm->width; xx++) {
              int tx = dst_x + xx;
              if (tx < 0 || tx >= w) continue;
              int bit = (src[xx >> 3] >> (7 - (xx & 7))) & 1;
              if (bit) {
                uint8_t *pd = &dst[tx];
                *pd = 255;
              }
            }
          }
        }
      }
    }

    pen_x += xadv;
    pen_y += yadv;
    i = (row_end < n) ? row_end + 1 : row_end;
  }

  vl_push_int(S, (int64_t)w);
  vl_push_int(S, (int64_t)h);
  vl_push_int(S,
              (int64_t)(-ox));  // origin_x: where baseline x=0 maps in bitmap
  vl_push_int(S,
              (int64_t)(-oy));  // origin_y: where baseline y=0 maps in bitmap
  vl_push_lstring(S, (const char *)a8, w * h);
  free(a8);
  return 5;
}

#endif  // real impl

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------
static const VL_Reg fontlib[] = {
    {"init", vlf_init},   {"done", vlf_done},        {"version", vlf_version},

    {"load", vlf_load},   {"free", vlf_free},        {"set_size", vlf_set_size},
    {"info", vlf_info},

    {"shape", vlf_shape}, {"rasterize", vlf_raster},

    {NULL, NULL}};

void vl_open_fontlib(VL_State *S) { vl_register_lib(S, "font", fontlib); }
