// SPDX-License-Identifier: GPL-3.0-or-later
//
// img.c — Image I/O front-end for Vitte Light VM (C17, complet)
// Namespace: "img"
//
// Build examples:
//   # stb (https://github.com/nothings/stb)
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_STB -c img.c
//   cc ... img.o
//   # + resize
//   cc -std=c17 -O2 -DVL_HAVE_STB -DVL_HAVE_STB_RESIZE -c img.c
//
// Model:
//   - Decode: PNG/JPEG/BMP/GIF/PSD/TGA/HDR… via stb_image (to RGBA8).
//   - Encode: PNG/JPEG via stb_image_write.
//   - Optional resize via stb_image_resize.
//   - Handle-based objects pour buffers RGBA en mémoire.
//
// API (C symbol layer):
//   int  img_load_file(const char* path);                              // >0 id | <0 err
//   int  img_load_mem(const void* data, size_t len);                   // >0 id | <0 err
//   int  img_size(int h, int* w, int* height);                         // 0 | <0
//   int  img_pixels(int h, const unsigned char** p, size_t* nbytes);   // 0 | <0
//   int  img_free(int h);                                              // 0 | <0
//
//   // Save raw RGBA
//   int  img_save_png(const char* path, const void* rgba, int w, int h, int stride);
//   int  img_save_jpg(const char* path, const void* rgba, int w, int h, int stride, int quality);
//
//   // Resize into new handle (requires VL_HAVE_STB_RESIZE)
//   int  img_resize(int h, int new_w, int new_h);                      // >0 id | <0 err
//
// Notes:
//   - Buffers internes: RGBA8 contigu (stride = w*4).
//   - Erreurs: -EINVAL, -ENOSYS, -ENOMEM, -EIO.
//   - Couche VM: copier via vl_push_lstring(...) côté binding.
//
// Deps VM optionnels: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifndef ENOSYS
#  define ENOSYS 38
#endif
#ifndef EINVAL
#  define EINVAL 22
#endif
#ifndef EIO
#  define EIO 5
#endif

#ifndef VL_EXPORT
#  if defined(_WIN32) && !defined(__clang__)
#    define VL_EXPORT __declspec(dllexport)
#  else
#    define VL_EXPORT
#  endif
#endif

// ----------------------------- stb -------------------------------------
#ifdef VL_HAVE_STB
#  ifndef STB_IMAGE_IMPLEMENTATION
#    define STB_IMAGE_IMPLEMENTATION
#  endif
#  ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#    define STB_IMAGE_WRITE_IMPLEMENTATION
#  endif
#  include "stb_image.h"
#  include "stb_image_write.h"
#  ifdef VL_HAVE_STB_RESIZE
#    ifndef STB_IMAGE_RESIZE_IMPLEMENTATION
#      define STB_IMAGE_RESIZE_IMPLEMENTATION
#    endif
#    include "stb_image_resize2.h" /* ou "stb_image_resize.h" selon version */
#  endif
#endif

typedef struct {
  int   w, h;
  unsigned char* rgba; // size = w*h*4
} img_buf;

#ifndef VL_IMG_MAX_HANDLES
#  define VL_IMG_MAX_HANDLES 64
#endif

static img_buf g_tbl[VL_IMG_MAX_HANDLES];

static void img_clear(img_buf* b) {
  if (b->rgba) free(b->rgba);
  b->rgba = NULL; b->w = b->h = 0;
}

static int img_alloc_handle(void) {
  for (int i = 1; i < VL_IMG_MAX_HANDLES; ++i)
    if (g_tbl[i].rgba == NULL && g_tbl[i].w == 0 && g_tbl[i].h == 0) return i;
  return -ENOMEM;
}

static int img_store_new(unsigned char* rgba, int w, int h) {
  int id = img_alloc_handle();
  if (id < 0) { free(rgba); return -ENOMEM; }
  g_tbl[id].rgba = rgba;
  g_tbl[id].w = w;
  g_tbl[id].h = h;
  return id;
}

// ----------------------------- API -------------------------------------

VL_EXPORT int img_load_file(const char* path) {
#ifndef VL_HAVE_STB
  (void)path; return -ENOSYS;
#else
  if (!path || !*path) return -EINVAL;
  int w=0,h=0,comp=0;
  unsigned char* p = stbi_load(path, &w, &h, &comp, 4);
  if (!p) return -EIO;
  return img_store_new(p, w, h);
#endif
}

VL_EXPORT int img_load_mem(const void* data, size_t len) {
#ifndef VL_HAVE_STB
  (void)data; (void)len; return -ENOSYS;
#else
  if (!data || len == 0) return -EINVAL;
  int w=0,h=0,comp=0;
  unsigned char* p = stbi_load_from_memory((const unsigned char*)data, (int)len, &w, &h, &comp, 4);
  if (!p) return -EIO;
  return img_store_new(p, w, h);
#endif
}

VL_EXPORT int img_size(int h, int* w, int* height) {
  if (h <= 0 || h >= VL_IMG_MAX_HANDLES) return -EINVAL;
  if (!g_tbl[h].rgba) return -EINVAL;
  if (w) *w = g_tbl[h].w;
  if (height) *height = g_tbl[h].h;
  return 0;
}

VL_EXPORT int img_pixels(int h, const unsigned char** p, size_t* nbytes) {
  if (h <= 0 || h >= VL_IMG_MAX_HANDLES) return -EINVAL;
  if (!g_tbl[h].rgba) return -EINVAL;
  if (p) *p = g_tbl[h].rgba;
  if (nbytes) *nbytes = (size_t)g_tbl[h].w * (size_t)g_tbl[h].h * 4u;
  return 0;
}

VL_EXPORT int img_free(int h) {
  if (h <= 0 || h >= VL_IMG_MAX_HANDLES) return -EINVAL;
  if (!g_tbl[h].rgba) return 0;
  img_clear(&g_tbl[h]);
  return 0;
}

VL_EXPORT int img_save_png(const char* path, const void* rgba, int w, int h, int stride) {
#ifndef VL_HAVE_STB
  (void)path; (void)rgba; (void)w; (void)h; (void)stride; return -ENOSYS;
#else
  if (!path || !rgba || w <= 0 || h <= 0) return -EINVAL;
  if (stride <= 0) stride = w * 4;
  int ok = stbi_write_png(path, w, h, 4, rgba, stride);
  return ok ? 0 : -EIO;
#endif
}

VL_EXPORT int img_save_jpg(const char* path, const void* rgba, int w, int h, int stride, int quality) {
#ifndef VL_HAVE_STB
  (void)path; (void)rgba; (void)w; (void)h; (void)stride; (void)quality; return -ENOSYS;
#else
  if (!path || !rgba || w <= 0 || h <= 0) return -EINVAL;
  if (quality <= 0) quality = 90;
  if (stride <= 0) stride = w * 4;
  // stb_image_write attend RGB, on convertit RGBA -> RGB en mémoire temporaire
  size_t rgb_sz = (size_t)w * (size_t)h * 3u;
  unsigned char* rgb = (unsigned char*)malloc(rgb_sz);
  if (!rgb) return -ENOMEM;
  const unsigned char* src = (const unsigned char*)rgba;
  unsigned char* dst = rgb;
  for (int y=0; y<h; ++y) {
    const unsigned char* s = src + (size_t)y * (size_t)stride;
    for (int x=0; x<w; ++x) {
      dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2];
      s += 4; dst += 3;
    }
  }
  int ok = stbi_write_jpg(path, w, h, 3, rgb, quality);
  free(rgb);
  return ok ? 0 : -EIO;
#endif
}

VL_EXPORT int img_resize(int h, int new_w, int new_h) {
#if !defined(VL_HAVE_STB) || !defined(VL_HAVE_STB_RESIZE)
  (void)h; (void)new_w; (void)new_h; return -ENOSYS;
#else
  if (h <= 0 || h >= VL_IMG_MAX_HANDLES) return -EINVAL;
  if (!g_tbl[h].rgba || new_w <= 0 || new_h <= 0) return -EINVAL;

  size_t out_sz = (size_t)new_w * (size_t)new_h * 4u;
  unsigned char* out = (unsigned char*)malloc(out_sz);
  if (!out) return -ENOMEM;

  int ok =
#if defined(STBIR_FLAG_ALPHA_PREMULTIPLIED) || defined(STBIR_TYPE_UINT8)
    stbir_resize_uint8(g_tbl[h].rgba, g_tbl[h].w, g_tbl[h].h, 0,
                       out, new_w, new_h, 0, 4);
#else
    stbir_resize(g_tbl[h].rgba, g_tbl[h].w, g_tbl[h].h, 0,
                 out, new_w, new_h, 0, 4);
#endif
  if (!ok) { free(out); return -EIO; }
  return img_store_new(out, new_w, new_h);
#endif
}