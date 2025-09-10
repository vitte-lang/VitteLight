// SPDX-License-Identifier: GPL-3.0-or-later
//
// img.c — Image I/O & processing front-end for Vitte Light VM (C17, complet)
// Namespace: "img"
//
// Build examples:
//   # Avec STB (https://github.com/nothings/stb)
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_STB -I/path/to/stb -c img.c
//   cc ... img.o
//
//   # Avec STB + resize
//   cc -std=c17 -O2 -DVL_HAVE_STB -DVL_HAVE_STB_RESIZE -I/path/to/stb -c img.c
//
// Modèle:
//   - Chargement: PNG/JPEG/BMP/TGA/GIF/PSD/etc. via stb_image.h
//   - Sauvegarde: PNG/JPEG/BMP/TGA via stb_image_write.h
//   - Redimensionnement: optionnel via stb_image_resize.h
//   - Pixels: octets interleavés, lignes consécutives, origin en haut-gauche par défaut.
//   - VM strings sans NUL en entrée; sorties via vl_push_lstring (binaire OK).
//
// API:
//
//   Lecture & infos
//     img.load(path[, opts]) -> pixels:string, w:int, h:int, ch:int | (nil,errmsg)
//       opts:
//         req_channels: 1|2|3|4        // forcer nombre de canaux
//         flip_y: 0|1                   // renverser vertical à la lecture
//     img.info(path) -> w:int, h:int, ch:int | (nil,errmsg)
//
//   Ecriture
//     img.save(path, pixels, w, h, ch[, opts]) -> true | (nil,errmsg)
//       opts:
//         format: "png"|"jpg"|"bmp"|"tga"  // sinon déduit de l’extension
//         quality: 1..100                  // jpg
//         png_compress: 0..9               // png
//         flip_y: 0|1                      // renverser vertical à l’écriture
//
//   Opérations
//     img.resize(pixels, w, h, ch, new_w, new_h[, opts])
//         -> new_pixels:string | (nil,errmsg)   // nécessite VL_HAVE_STB_RESIZE
//       opts:
//         filter: "nearest"|"bilinear"|"bicubic"|"lanczos" // défaut bilinear
//
//     img.flip_y(pixels, w, h, ch) -> flipped:string
//     img.premul_alpha(pixels, w, h, ch) -> premul:string   // ch==4 requis
//
// Erreurs: "EINVAL", "ENOSYS", "ENOMEM", "EIMG".
//
// Deps: auxlib.h, state.h, object.h, vm.h
//

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "auxlib.h"
#include "state.h"
#include "object.h"
#include "vm.h"

/* ========================= VM ADAPTER (Lua-like) ========================= */

#ifndef VL_API_ASSUMED
#define VL_API_ASSUMED 1
#endif

static const char *vl_check_string(VLState *L, int idx, size_t *len);
static const char *vl_opt_string  (VLState *L, int idx, const char *def, size_t *len);
static int64_t     vl_check_integer(VLState *L, int idx);
static int64_t     vl_opt_integer (VLState *L, int idx, int64_t def);
static int         vl_opt_boolean (VLState *L, int idx, int def);
static void        vl_check_type  (VLState *L, int idx, int t);
static int         vl_istable     (VLState *L, int idx);
static int         vl_isstring    (VLState *L, int idx);
static int         vl_isnil       (VLState *L, int idx);

static void vl_push_boolean(VLState *L, int v);
static void vl_push_integer(VLState *L, int64_t v);
static void vl_push_lstring(VLState *L, const char *s, size_t n);
static void vl_push_string (VLState *L, const char *s);
static void vl_push_nil    (VLState *L);
static void vl_new_table   (VLState *L);

struct vl_Reg { const char *name; int (*fn)(VLState *L); };
static void vl_register_module(VLState *L, const char *ns, const struct vl_Reg *funcs);

/* extern fournis par la VM réelle */
extern const char *vlx_check_string(VLState *L, int idx, size_t *len);
extern const char *vlx_opt_string  (VLState *L, int idx, const char *def, size_t *len);
extern int64_t     vlx_check_integer(VLState *L, int idx);
extern int64_t     vlx_opt_integer (VLState *L, int idx, int64_t def);
extern int         vlx_opt_boolean (VLState *L, int idx, int def);
extern void        vlx_check_table (VLState *L, int idx);
extern int         vlx_istable     (VLState *L, int idx);
extern int         vlx_isstring    (VLState *L, int idx);
extern int         vlx_isnil       (VLState *L, int idx);

extern void vlx_push_boolean(VLState *L, int v);
extern void vlx_push_integer(VLState *L, int64_t v);
extern void vlx_push_lstring(VLState *L, const char *s, size_t n);
extern void vlx_push_string (VLState *L, const char *s);
extern void vlx_push_nil    (VLState *L);
extern void vlx_new_table   (VLState *L);
extern void vlx_register_module(VLState *L, const char *ns, const struct vl_Reg *funcs);

/* redirections */
static inline const char *vl_check_string(VLState *L, int i, size_t *n){ return vlx_check_string(L,i,n); }
static inline const char *vl_opt_string  (VLState *L, int i, const char *d, size_t *n){ return vlx_opt_string(L,i,d,n); }
static inline int64_t     vl_check_integer(VLState *L, int i){ return vlx_check_integer(L,i); }
static inline int64_t     vl_opt_integer (VLState *L, int i, int64_t d){ return vlx_opt_integer(L,i,d); }
static inline int         vl_opt_boolean (VLState *L, int i, int d){ return vlx_opt_boolean(L,i,d); }
static inline void        vl_check_type  (VLState *L, int i, int t){ if(t==1) vlx_check_table(L,i); }
static inline int         vl_istable     (VLState *L, int i){ return vlx_istable(L,i); }
static inline int         vl_isstring    (VLState *L, int i){ return vlx_isstring(L,i); }
static inline int         vl_isnil       (VLState *L, int i){ return vlx_isnil(L,i); }

static inline void vl_push_boolean(VLState *L, int v){ vlx_push_boolean(L,v); }
static inline void vl_push_integer(VLState *L, int64_t v){ vlx_push_integer(L,v); }
static inline void vl_push_lstring(VLState *L, const char *s, size_t n){ vlx_push_lstring(L,s,n); }
static inline void vl_push_string (VLState *L, const char *s){ vlx_push_string(L,s); }
static inline void vl_push_nil    (VLState *L){ vlx_push_nil(L); }
static inline void vl_new_table   (VLState *L){ vlx_new_table(L); }
static inline void vl_register_module(VLState *L, const char *ns, const struct vl_Reg *funcs){ vlx_register_module(L,ns,funcs); }

/* ================================ STB ==================================== */

#ifdef VL_HAVE_STB
  #include <errno.h>
  #define STB_IMAGE_IMPLEMENTATION
  #include "stb_image.h"
  #define STB_IMAGE_WRITE_IMPLEMENTATION
  #include "stb_image_write.h"
  #ifdef VL_HAVE_STB_RESIZE
    #define STB_IMAGE_RESIZE_IMPLEMENTATION
    #include "stb_image_resize.h"
  #endif
#endif

/* =============================== Utils =================================== */

static void *xmalloc(size_t n){
  void *p = malloc(n ? n : 1);
  return p ? p : NULL;
}

static int has_ext_ci(const char *path, const char *ext){
  size_t lp = strlen(path), le = strlen(ext);
  if(le > lp) return 0;
  const char *suf = path + (lp - le);
  for(size_t i=0;i<le;i++){
    char a = suf[i], b = ext[i];
    if(a>='A'&&a<='Z') a = (char)(a+32);
    if(b>='A'&&b<='Z') b = (char)(b+32);
    if(a != b) return 0;
  }
  return 1;
}

static const char *err_enomem = "ENOMEM";
static const char *err_eimg   = "EIMG";
static const char *err_enosys = "ENOSYS";
static const char *err_einval = "EINVAL";

/* ============================== img.info ================================= */

static int img_info(VLState *L){
#ifndef VL_HAVE_STB
  vl_push_nil(L); vl_push_string(L, err_enosys); return 2;
#else
  size_t plen=0; const char *path = vl_check_string(L, 1, &plen);
  int x=0,y=0,n=0;
  stbi_set_flip_vertically_on_load(0);
  if(!stbi_info(path, &x, &y, &n)){
    vl_push_nil(L); vl_push_string(L, err_eimg); return 2;
  }
  vl_push_integer(L, x);
  vl_push_integer(L, y);
  vl_push_integer(L, n);
  return 3;
#endif
}

/* ============================== img.load ================================= */

static int img_load(VLState *L){
#ifndef VL_HAVE_STB
  vl_push_nil(L); vl_push_string(L, err_enosys); return 2;
#else
  size_t plen=0; const char *path = vl_check_string(L, 1, &plen);
  int req_ch = (int)vl_opt_integer(L, 2, 0);
  int flip = vl_opt_boolean(L, 3, 0);
  if(req_ch!=0 && req_ch!=1 && req_ch!=2 && req_ch!=3 && req_ch!=4){
    vl_push_nil(L); vl_push_string(L, err_einval); return 2;
  }
  stbi_set_flip_vertically_on_load(flip?1:0);
  int w=0,h=0,ch=0;
  unsigned char *pix = stbi_load(path, &w, &h, &ch, req_ch?req_ch:0);
  if(!pix){
    vl_push_nil(L); vl_push_string(L, err_eimg); return 2;
  }
  int out_ch = req_ch?req_ch:ch;
  size_t bytes = (size_t)w * (size_t)h * (size_t)out_ch;
  vl_push_lstring(L, (const char*)pix, bytes);
  vl_push_integer(L, w);
  vl_push_integer(L, h);
  vl_push_integer(L, out_ch);
  stbi_image_free(pix);
  return 4;
#endif
}

/* ============================== img.save ================================= */

static int img_save(VLState *L){
#ifndef VL_HAVE_STB
  vl_push_nil(L); vl_push_string(L, err_enosys); return 2;
#else
  size_t path_n=0, pix_n=0;
  const char *path = vl_check_string(L, 1, &path_n);
  const char *pix  = vl_check_string(L, 2, &pix_n);
  int64_t w = vl_check_integer(L, 3);
  int64_t h = vl_check_integer(L, 4);
  int64_t ch = vl_check_integer(L, 5);
  if(w<=0 || h<=0 || (ch!=1 && ch!=2 && ch!=3 && ch!=4)){
    vl_push_nil(L); vl_push_string(L, err_einval); return 2;
  }
  size_t need = (size_t)w * (size_t)h * (size_t)ch;
  if(pix_n < need){
    vl_push_nil(L); vl_push_string(L, err_einval); return 2;
  }

  /* opts */
  const char *fmt = NULL; size_t fmt_n=0;
  int quality = (int)vl_opt_integer(L, 7, 90);
  int png_comp = (int)vl_opt_integer(L, 8, 6);
  int flip = vl_opt_boolean(L, 9, 0);
  fmt = vl_opt_string(L, 6, "", &fmt_n);

  int ok = 0;
  stbi_flip_vertically_on_write(flip?1:0);

  /* format */
  enum { F_PNG, F_JPG, F_BMP, F_TGA } f = F_PNG;
  if(fmt && fmt_n){
    if     (strcasecmp(fmt,"png")==0) f = F_PNG;
    else if(strcasecmp(fmt,"jpg")==0 || strcasecmp(fmt,"jpeg")==0) f = F_JPG;
    else if(strcasecmp(fmt,"bmp")==0) f = F_BMP;
    else if(strcasecmp(fmt,"tga")==0) f = F_TGA;
    else { vl_push_nil(L); vl_push_string(L, err_einval); return 2; }
  }else{
    if     (has_ext_ci(path,".png"))  f = F_PNG;
    else if(has_ext_ci(path,".jpg") || has_ext_ci(path,".jpeg")) f = F_JPG;
    else if(has_ext_ci(path,".bmp"))  f = F_BMP;
    else if(has_ext_ci(path,".tga"))  f = F_TGA;
    else f = F_PNG;
  }

  switch(f){
    case F_PNG: ok = stbi_write_png(path, (int)w, (int)h, (int)ch, (const void*)pix, (int)(w*ch)); break;
    case F_JPG: ok = stbi_write_jpg(path, (int)w, (int)h, (int)ch, (const void*)pix, quality); break;
    case F_BMP: ok = stbi_write_bmp(path, (int)w, (int)h, (int)ch, (const void*)pix); break;
    case F_TGA: ok = stbi_write_tga(path, (int)w, (int)h, (int)ch, (const void*)pix); break;
  }
  if(!ok){ vl_push_nil(L); vl_push_string(L, err_eimg); return 2; }
  vl_push_boolean(L, 1);
  return 1;
#endif
}

/* ============================== img.resize =============================== */

static int img_resize(VLState *L){
#if !defined(VL_HAVE_STB) || !defined(VL_HAVE_STB_RESIZE)
  vl_push_nil(L); vl_push_string(L, err_enosys); return 2;
#else
  size_t in_n=0; const unsigned char *in = (const unsigned char*)vl_check_string(L, 1, &in_n);
  int64_t w  = vl_check_integer(L, 2);
  int64_t h  = vl_check_integer(L, 3);
  int64_t ch = vl_check_integer(L, 4);
  int64_t nw = vl_check_integer(L, 5);
  int64_t nh = vl_check_integer(L, 6);
  if(w<=0||h<=0||nw<=0||nh<=0||(ch<1||ch>4)){ vl_push_nil(L); vl_push_string(L, err_einval); return 2; }
  size_t need = (size_t)w*(size_t)h*(size_t)ch;
  if(in_n < need){ vl_push_nil(L); vl_push_string(L, err_einval); return 2; }

  size_t out_bytes = (size_t)nw*(size_t)nh*(size_t)ch;
  unsigned char *out = (unsigned char*)xmalloc(out_bytes);
  if(!out){ vl_push_nil(L); vl_push_string(L, err_enomem); return 2; }

  /* opts */
  size_t f_n=0; const char *f = vl_opt_string(L, 7, "bilinear", &f_n);
  int ok = 0;

  /* mapping simple des filtres */
  int use_lanczos = 0, use_bicubic = 0, use_nearest = 0;
  if(strcasecmp(f,"lanczos")==0) use_lanczos=1;
  else if(strcasecmp(f,"bicubic")==0) use_bicubic=1;
  else if(strcasecmp(f,"nearest")==0) use_nearest=1;

  if(use_nearest){
    ok = stbir_resize_uint8_generic(in,(int)w,(int)h,0, out,(int)nw,(int)nh,0, (int)ch,
          STBIR_ALPHA_CHANNEL_NONE,0, STBIR_EDGE_CLAMP,STBIR_EDGE_CLAMP,
          STBIR_FILTER_BOX,STBIR_FILTER_BOX, STBIR_COLORSPACE_SRGB, NULL);
  }else if(use_bicubic){
    ok = stbir_resize_uint8_generic(in,(int)w,(int)h,0, out,(int)nw,(int)nh,0, (int)ch,
          STBIR_ALPHA_CHANNEL_NONE,0, STBIR_EDGE_CLAMP,STBIR_EDGE_CLAMP,
          STBIR_FILTER_CUBICBSPLINE,STBIR_FILTER_CUBICBSPLINE, STBIR_COLORSPACE_SRGB, NULL);
  }else if(use_lanczos){
    ok = stbir_resize_uint8_generic(in,(int)w,(int)h,0, out,(int)nw,(int)nh,0, (int)ch,
          STBIR_ALPHA_CHANNEL_NONE,0, STBIR_EDGE_CLAMP,STBIR_EDGE_CLAMP,
          STBIR_FILTER_MITCHELL,STBIR_FILTER_MITCHELL, STBIR_COLORSPACE_SRGB, NULL);
  }else{
    ok = stbir_resize_uint8(in,(int)w,(int)h,0, out,(int)nw,(int)nh,0, (int)ch);
  }

  if(!ok){ free(out); vl_push_nil(L); vl_push_string(L, err_eimg); return 2; }
  vl_push_lstring(L, (const char*)out, out_bytes);
  free(out);
  return 1;
#endif
}

/* ============================= img.flip_y ================================ */

static int img_flip_y(VLState *L){
  size_t n=0; unsigned char *buf = (unsigned char*)vl_check_string(L, 1, &n);
  int64_t w  = vl_check_integer(L, 2);
  int64_t h  = vl_check_integer(L, 3);
  int64_t ch = vl_check_integer(L, 4);
  if(w<=0||h<=0||(ch<1||ch>4)){ vl_push_nil(L); vl_push_string(L, err_einval); return 2; }
  size_t need = (size_t)w*(size_t)h*(size_t)ch;
  if(n < need){ vl_push_nil(L); vl_push_string(L, err_einval); return 2; }

  unsigned char *out = (unsigned char*)xmalloc(need);
  if(!out){ vl_push_nil(L); vl_push_string(L, err_enomem); return 2; }

  size_t stride = (size_t)w*(size_t)ch;
  for(int64_t y=0; y<h; y++){
    memcpy(out + (size_t)y*stride, buf + (size_t)(h-1-y)*stride, stride);
  }
  vl_push_lstring(L, (const char*)out, need);
  free(out);
  return 1;
}

/* =========================== img.premul_alpha ============================ */

static int img_premul_alpha(VLState *L){
  size_t n=0; unsigned char *buf = (unsigned char*)vl_check_string(L, 1, &n);
  int64_t w  = vl_check_integer(L, 2);
  int64_t h  = vl_check_integer(L, 3);
  int64_t ch = vl_check_integer(L, 4);
  if(w<=0||h<=0||ch!=4){ vl_push_nil(L); vl_push_string(L, err_einval); return 2; }
  size_t need = (size_t)w*(size_t)h*(size_t)ch;
  if(n < need){ vl_push_nil(L); vl_push_string(L, err_einval); return 2; }

  unsigned char *out = (unsigned char*)xmalloc(need);
  if(!out){ vl_push_nil(L); vl_push_string(L, err_enomem); return 2; }

  size_t px = (size_t)w*(size_t)h;
  for(size_t i=0;i<px;i++){
    unsigned a = buf[i*4+3];
    out[i*4+3] = (unsigned char)a;
    out[i*4+0] = (unsigned char)((buf[i*4+0] * a + 127) / 255);
    out[i*4+1] = (unsigned char)((buf[i*4+1] * a + 127) / 255);
    out[i*4+2] = (unsigned char)((buf[i*4+2] * a + 127) / 255);
  }
  vl_push_lstring(L, (const char*)out, need);
  free(out);
  return 1;
}

/* ============================== Dispatch ================================= */

static int img_load_entry  (VLState *L){ return img_load(L); }
static int img_save_entry  (VLState *L){ return img_save(L); }
static int img_info_entry  (VLState *L){ return img_info(L); }
static int img_resize_entry(VLState *L){ return img_resize(L); }
static int img_flip_y_entry(VLState *L){ return img_flip_y(L); }
static int img_premul_entry(VLState *L){ return img_premul_alpha(L); }

static const struct vl_Reg img_funcs[] = {
  {"load",   img_load_entry},
  {"save",   img_save_entry},
  {"info",   img_info_entry},
  {"resize", img_resize_entry},
  {"flip_y", img_flip_y_entry},
  {"premul_alpha", img_premul_entry},
  {NULL, NULL}
};

int vl_openlib_img(VLState *L){
  vl_register_module(L, "img", img_funcs);
  return 1;
}