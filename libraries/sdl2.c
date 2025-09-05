// SPDX-License-Identifier: GPL-3.0-or-later
//
// sdl2.c — SDL2 bindings pour Vitte Light (C17, complet)
// Namespace: "sdl"
//
// Portabilité:
//   - Active avec -DVL_HAVE_SDL2 et <SDL2/SDL.h> ou <SDL.h> présent.
//   - Sinon, stubs renvoyant (nil, "ENOSYS").
//
// Portée (1 fenêtre/renderer global):
//   Init/quit:
//     sdl.init(title, w:int, h:int, [vsync=true], [resizable=true],
//     [highdpi=false]) -> true | (nil, errmsg) sdl.quit() -> true
//   Fenêtre / renderer:
//     sdl.set_title(title) -> true | (nil, errmsg)
//     sdl.window_size() -> w:int, h:int
//     sdl.set_logical_size(w:int, h:int) -> true | (nil, errmsg)
//     sdl.clear(r:int,g:int,b:int,[a=255]) -> true | (nil, errmsg)
//     sdl.present() -> true
//     sdl.set_draw_color(r,g,b,[a=255]) -> true
//     sdl.draw_line(x1,y1,x2,y2) -> true
//     sdl.draw_rect(x,y,w,h) -> true
//     sdl.fill_rect(x,y,w,h) -> true
//   Textures RGBA32 (pitch = w*4):
//     sdl.tex_create(w:int, h:int) -> id:int | (nil, errmsg)
//     sdl.tex_update(id:int, bytes:string) -> true | (nil, errmsg)   // len ==
//     w*h*4 sdl.render_tex(id:int, dx:int, dy:int, [dw:int, dh:int]) -> true |
//     (nil, errmsg) sdl.destroy_tex(id:int) -> true
//   Événements:
//     sdl.poll_event() -> type:int, a:int, b:int, c:int, d:int
//       Types: 0=none,1=quit,2=keydown(scancode,repeat),3=keyup(scancode,0),
//              4=mousedown(btn,x,y),5=mouseup(btn,x,y),
//              6=mousemotion(x,y,dx,dy),7=wheel(dx,dy)
//   Divers:
//     sdl.delay(ms:int) -> true
//     sdl.ticks_ms() -> int64
//     sdl.show_cursor(on:bool) -> prev:int
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef VL_HAVE_SDL2
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#else
#undef VL_HAVE_SDL2
#endif
#endif

// ---------------------------------------------------------------------
// Aides VM
// ---------------------------------------------------------------------

static const char *sdl_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t sdl_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int sdl_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int sdl_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)sdl_check_int(S, idx);
  return defv;
}

#ifndef VL_HAVE_SDL2
// ---------------------------------------------------------------------
// STUBS (SDL2 absent)
// ---------------------------------------------------------------------
static int err_nosys(VL_State *S) {
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
}
static int vlsdl_init(VL_State *S) {
  (void)S;
  return err_nosys(S);
}
static int vlsdl_quit(VL_State *S) {
  (void)S;
  return err_nosys(S);
}
static int vlsdl_set_title(VL_State *S) {
  (void)sdl_check_str(S, 1);
  return err_nosys(S);
}
static int vlsdl_window_size(VL_State *S) {
  (void)S;
  return err_nosys(S);
}
static int vlsdl_set_logical_size(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_int(S, 2);
  return err_nosys(S);
}
static int vlsdl_clear(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_int(S, 2);
  (void)sdl_check_int(S, 3);
  return err_nosys(S);
}
static int vlsdl_present(VL_State *S) { return err_nosys(S); }
static int vlsdl_set_draw_color(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_int(S, 2);
  (void)sdl_check_int(S, 3);
  return err_nosys(S);
}
static int vlsdl_draw_line(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_int(S, 2);
  (void)sdl_check_int(S, 3);
  (void)sdl_check_int(S, 4);
  return err_nosys(S);
}
static int vlsdl_draw_rect(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_int(S, 2);
  (void)sdl_check_int(S, 3);
  (void)sdl_check_int(S, 4);
  return err_nosys(S);
}
static int vlsdl_fill_rect(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_int(S, 2);
  (void)sdl_check_int(S, 3);
  (void)sdl_check_int(S, 4);
  return err_nosys(S);
}
static int vlsdl_tex_create(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_int(S, 2);
  return err_nosys(S);
}
static int vlsdl_tex_update(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_str(S, 2);
  return err_nosys(S);
}
static int vlsdl_render_tex(VL_State *S) {
  (void)sdl_check_int(S, 1);
  (void)sdl_check_int(S, 2);
  (void)sdl_check_int(S, 3);
  return err_nosys(S);
}
static int vlsdl_destroy_tex(VL_State *S) {
  (void)sdl_check_int(S, 1);
  return err_nosys(S);
}
static int vlsdl_poll_event(VL_State *S) {
  (void)S;
  return err_nosys(S);
}
static int vlsdl_delay(VL_State *S) {
  (void)sdl_check_int(S, 1);
  return err_nosys(S);
}
static int vlsdl_ticks_ms(VL_State *S) {
  (void)S;
  return err_nosys(S);
}
static int vlsdl_show_cursor(VL_State *S) {
  (void)sdl_opt_bool(S, 1, 1);
  return err_nosys(S);
}

#else
// ---------------------------------------------------------------------
// Implémentation SDL2 réelle
// ---------------------------------------------------------------------

static SDL_Window *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static int g_inited = 0;

typedef struct TexEntry {
  int used;
  int w, h;
  SDL_Texture *tex;
} TexEntry;

static TexEntry *g_tex = NULL;
static int g_tex_cap = 0;

// ---- helpers
static int push_sdl_err(VL_State *S) {
  const char *e = SDL_GetError();
  vl_push_nil(S);
  vl_push_string(S, e && *e ? e : "EIO");
  return 2;
}
static int ensure_tex_cap(int need) {
  if (need <= g_tex_cap) return 1;
  int ncap = g_tex_cap ? g_tex_cap : 16;
  while (ncap < need) ncap <<= 1;
  TexEntry *nt = (TexEntry *)realloc(g_tex, (size_t)ncap * sizeof *nt);
  if (!nt) return 0;
  for (int i = g_tex_cap; i < ncap; i++) {
    nt[i].used = 0;
    nt[i].w = nt[i].h = 0;
    nt[i].tex = NULL;
  }
  g_tex = nt;
  g_tex_cap = ncap;
  return 1;
}
static int tex_alloc_slot(void) {
  for (int i = 1; i < g_tex_cap; i++)
    if (!g_tex[i].used) return i;
  if (!ensure_tex_cap(g_tex_cap ? g_tex_cap * 2 : 16)) return 0;
  for (int i = 1; i < g_tex_cap; i++)
    if (!g_tex[i].used) return i;
  return 0;
}

// ---- VM functions

static int vlsdl_init(VL_State *S) {
  const char *title = sdl_check_str(S, 1);
  int w = (int)sdl_check_int(S, 2);
  int h = (int)sdl_check_int(S, 3);
  int vsync = sdl_opt_bool(S, 4, 1);
  int resizable = sdl_opt_bool(S, 5, 1);
  int highdpi = sdl_opt_bool(S, 6, 0);

  if (!g_inited) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return push_sdl_err(S);
    g_inited = 1;
  }
  if (g_win || g_ren) {
    vl_push_bool(S, 1);
    return 1;
  }

  Uint32 wflags = SDL_WINDOW_SHOWN;
  if (resizable) wflags |= SDL_WINDOW_RESIZABLE;
  if (highdpi) wflags |= SDL_WINDOW_ALLOW_HIGHDPI;

  g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, w, h, wflags);
  if (!g_win) return push_sdl_err(S);

  Uint32 rflags =
      SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
  g_ren = SDL_CreateRenderer(g_win, -1, rflags);
  if (!g_ren) {
    SDL_DestroyWindow(g_win);
    g_win = NULL;
    return push_sdl_err(S);
  }

  SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_quit(VL_State *S) {
  (void)S;
  if (g_ren) {
    SDL_DestroyRenderer(g_ren);
    g_ren = NULL;
  }
  if (g_win) {
    SDL_DestroyWindow(g_win);
    g_win = NULL;
  }
  if (g_inited) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();
    g_inited = 0;
  }
  // libération textures
  if (g_tex) {
    for (int i = 1; i < g_tex_cap; i++)
      if (g_tex[i].used && g_tex[i].tex) SDL_DestroyTexture(g_tex[i].tex);
    free(g_tex);
    g_tex = NULL;
    g_tex_cap = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_set_title(VL_State *S) {
  const char *t = sdl_check_str(S, 1);
  if (!g_win) return push_sdl_err(S);
  SDL_SetWindowTitle(g_win, t);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_window_size(VL_State *S) {
  if (!g_win) return push_sdl_err(S);
  int w = 0, h = 0;
  SDL_GetWindowSize(g_win, &w, &h);
  vl_push_int(S, (int64_t)w);
  vl_push_int(S, (int64_t)h);
  return 2;
}

static int vlsdl_set_logical_size(VL_State *S) {
  if (!g_ren) return push_sdl_err(S);
  int w = (int)sdl_check_int(S, 1);
  int h = (int)sdl_check_int(S, 2);
  if (SDL_RenderSetLogicalSize(g_ren, w, h) != 0) return push_sdl_err(S);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_set_draw_color(VL_State *S) {
  if (!g_ren) return push_sdl_err(S);
  int r = (int)sdl_check_int(S, 1), g = (int)sdl_check_int(S, 2),
      b = (int)sdl_check_int(S, 3);
  int a = sdl_opt_int(S, 4, 255);
  if (SDL_SetRenderDrawColor(g_ren, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a) !=
      0)
    return push_sdl_err(S);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_clear(VL_State *S) {
  if (!g_ren) return push_sdl_err(S);
  int r = (int)sdl_check_int(S, 1), g = (int)sdl_check_int(S, 2),
      b = (int)sdl_check_int(S, 3);
  int a = sdl_opt_int(S, 4, 255);
  SDL_SetRenderDrawColor(g_ren, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a);
  if (SDL_RenderClear(g_ren) != 0) return push_sdl_err(S);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_present(VL_State *S) {
  if (!g_ren) return push_sdl_err(S);
  SDL_RenderPresent(g_ren);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_draw_line(VL_State *S) {
  if (!g_ren) return push_sdl_err(S);
  int x1 = (int)sdl_check_int(S, 1), y1 = (int)sdl_check_int(S, 2);
  int x2 = (int)sdl_check_int(S, 3), y2 = (int)sdl_check_int(S, 4);
  if (SDL_RenderDrawLine(g_ren, x1, y1, x2, y2) != 0) return push_sdl_err(S);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_draw_rect(VL_State *S) {
  if (!g_ren) return push_sdl_err(S);
  SDL_Rect r = {(int)sdl_check_int(S, 1), (int)sdl_check_int(S, 2),
                (int)sdl_check_int(S, 3), (int)sdl_check_int(S, 4)};
  if (SDL_RenderDrawRect(g_ren, &r) != 0) return push_sdl_err(S);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_fill_rect(VL_State *S) {
  if (!g_ren) return push_sdl_err(S);
  SDL_Rect r = {(int)sdl_check_int(S, 1), (int)sdl_check_int(S, 2),
                (int)sdl_check_int(S, 3), (int)sdl_check_int(S, 4)};
  if (SDL_RenderFillRect(g_ren, &r) != 0) return push_sdl_err(S);
  vl_push_bool(S, 1);
  return 1;
}

// --- Textures RGBA32 (SDL_PIXELFORMAT_RGBA32) ---

static int vlsdl_tex_create(VL_State *S) {
  if (!g_ren) return push_sdl_err(S);
  int w = (int)sdl_check_int(S, 1);
  int h = (int)sdl_check_int(S, 2);
  if (w <= 0 || h <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
  if (!t) return push_sdl_err(S);
  int id = tex_alloc_slot();
  if (!id) {
    SDL_DestroyTexture(t);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_tex[id].used = 1;
  g_tex[id].w = w;
  g_tex[id].h = h;
  g_tex[id].tex = t;
  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlsdl_tex_update(VL_State *S) {
  int id = (int)sdl_check_int(S, 1);
  const char *bytes = sdl_check_str(S, 2);
  if (id <= 0 || id >= g_tex_cap || !g_tex[id].used) return push_sdl_err(S);
  int w = g_tex[id].w, h = g_tex[id].h;
  size_t need = (size_t)w * (size_t)h * 4;
  size_t got =
      strlen(bytes);  // VM strings are raw bytes; strlen OK since strings are
                      // 0-terminated by VM. Use length API if available.
  if (got < need) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  if (SDL_UpdateTexture(g_tex[id].tex, NULL, bytes, w * 4) != 0)
    return push_sdl_err(S);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_render_tex(VL_State *S) {
  int id = (int)sdl_check_int(S, 1);
  int dx = (int)sdl_check_int(S, 2);
  int dy = (int)sdl_check_int(S, 3);
  int dw = sdl_opt_int(S, 4, -1);
  int dh = sdl_opt_int(S, 5, -1);
  if (!g_ren || id <= 0 || id >= g_tex_cap || !g_tex[id].used)
    return push_sdl_err(S);

  SDL_Rect dst;
  if (dw <= 0 || dh <= 0) {
    dst.x = dx;
    dst.y = dy;
    dst.w = g_tex[id].w;
    dst.h = g_tex[id].h;
  } else {
    dst.x = dx;
    dst.y = dy;
    dst.w = dw;
    dst.h = dh;
  }

  if (SDL_RenderCopy(g_ren, g_tex[id].tex, NULL, &dst) != 0)
    return push_sdl_err(S);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_destroy_tex(VL_State *S) {
  int id = (int)sdl_check_int(S, 1);
  if (id <= 0 || id >= g_tex_cap || !g_tex[id].used) {
    vl_push_bool(S, 1);
    return 1;
  }
  if (g_tex[id].tex) SDL_DestroyTexture(g_tex[id].tex);
  g_tex[id].used = 0;
  g_tex[id].tex = NULL;
  g_tex[id].w = g_tex[id].h = 0;
  vl_push_bool(S, 1);
  return 1;
}

// --- Événements ---

static int vlsdl_poll_event(VL_State *S) {
  if (!g_inited) {
    vl_push_int(S, (int64_t)0);
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    return 5;
  }
  SDL_Event e;
  if (!SDL_PollEvent(&e)) {
    vl_push_int(S, (int64_t)0);
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    vl_push_int(S, 0);
    return 5;
  }

  switch (e.type) {
    case SDL_QUIT:
      vl_push_int(S, (int64_t)1);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      return 5;
    case SDL_KEYDOWN:
      vl_push_int(S, (int64_t)2);
      vl_push_int(S, (int64_t)e.key.keysym.scancode);
      vl_push_int(S, (int64_t)(e.key.repeat ? 1 : 0));
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      return 5;
    case SDL_KEYUP:
      vl_push_int(S, (int64_t)3);
      vl_push_int(S, (int64_t)e.key.keysym.scancode);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      return 5;
    case SDL_MOUSEBUTTONDOWN:
      vl_push_int(S, (int64_t)4);
      vl_push_int(S, (int64_t)e.button.button);
      vl_push_int(S, (int64_t)e.button.x);
      vl_push_int(S, (int64_t)e.button.y);
      vl_push_int(S, 0);
      return 5;
    case SDL_MOUSEBUTTONUP:
      vl_push_int(S, (int64_t)5);
      vl_push_int(S, (int64_t)e.button.button);
      vl_push_int(S, (int64_t)e.button.x);
      vl_push_int(S, (int64_t)e.button.y);
      vl_push_int(S, 0);
      return 5;
    case SDL_MOUSEMOTION:
      vl_push_int(S, (int64_t)6);
      vl_push_int(S, (int64_t)e.motion.x);
      vl_push_int(S, (int64_t)e.motion.y);
      vl_push_int(S, (int64_t)e.motion.xrel);
      vl_push_int(S, (int64_t)e.motion.yrel);
      return 5;
    case SDL_MOUSEWHEEL:
      vl_push_int(S, (int64_t)7);
      vl_push_int(S, (int64_t)e.wheel.x);
      vl_push_int(S, (int64_t)e.wheel.y);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      return 5;
    default:
      vl_push_int(S, (int64_t)0);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      vl_push_int(S, 0);
      return 5;
  }
}

static int vlsdl_delay(VL_State *S) {
  int ms = (int)sdl_check_int(S, 1);
  SDL_Delay((Uint32)(ms < 0 ? 0 : ms));
  vl_push_bool(S, 1);
  return 1;
}

static int vlsdl_ticks_ms(VL_State *S) {
  vl_push_int(S, (int64_t)SDL_GetTicks64());
  return 1;
}

static int vlsdl_show_cursor(VL_State *S) {
  int on = sdl_opt_bool(S, 1, 1);
  int prev = SDL_ShowCursor(on ? SDL_ENABLE : SDL_DISABLE);
  vl_push_int(S, (int64_t)prev);
  return 1;
}

#endif  // VL_HAVE_SDL2

// ---------------------------------------------------------------------
// Enregistrement VM
// ---------------------------------------------------------------------

static const VL_Reg sdllib[] = {{"init", vlsdl_init},
                                {"quit", vlsdl_quit},

                                {"set_title", vlsdl_set_title},
                                {"window_size", vlsdl_window_size},
                                {"set_logical_size", vlsdl_set_logical_size},

                                {"set_draw_color", vlsdl_set_draw_color},
                                {"clear", vlsdl_clear},
                                {"present", vlsdl_present},
                                {"draw_line", vlsdl_draw_line},
                                {"draw_rect", vlsdl_draw_rect},
                                {"fill_rect", vlsdl_fill_rect},

                                {"tex_create", vlsdl_tex_create},
                                {"tex_update", vlsdl_tex_update},
                                {"render_tex", vlsdl_render_tex},
                                {"destroy_tex", vlsdl_destroy_tex},

                                {"poll_event", vlsdl_poll_event},
                                {"delay", vlsdl_delay},
                                {"ticks_ms", vlsdl_ticks_ms},
                                {"show_cursor", vlsdl_show_cursor},

                                {NULL, NULL}};

void vl_open_sdllib(VL_State *S) { vl_register_lib(S, "sdl", sdllib); }
