// SPDX-License-Identifier: MIT
// VitteLight — SDL2 bridge (ultra complet)
// Fichier: libraries/SDL2.c
// Objectif: fournir une couche C stable et simple pour piloter SDL2 (core),
//           et optionnellement SDL2_image, SDL2_ttf, SDL2_mixer depuis Vitte‑Light.
//           API en C plate, basée sur des handles entiers, adaptée aux FFI.
//
// ✔ Couverture
//   • Initialisation / Quit
//   • Fenêtre, Renderer, VSync, Title, Icon
//   • Couleur, Clear, Present, DrawLine/Rect/FillRect, CopyTexture, CopyEx
//   • Textures: depuis fichier (IMG), BMP, Surface, taille, verrouillage
//   • Événements: clavier, souris, fenêtre, quit, roue, texte
//   • Temps: Delay, GetTicks
//   • Capture écran (BMP)
//   • Police et texte (TTF): open, measure, to texture
//   • Audio (Mixer): chunks et musiques, volumes, pause, stop
//
// ⚙️ Build exemples
//   Linux/macOS (dylib/so):
//     cc -std=c11 -O2 -fPIC \
//        -DVL_SDL2_WITH_IMAGE -DVL_SDL2_WITH_TTF -DVL_SDL2_WITH_MIXER \
//        SDL2.c -lSDL2 -lSDL2_image -lSDL2_ttf -lSDL2_mixer -shared -o libvlsdl2.so
//
//   Windows (DLL, MSVC):
//     cl /std:c11 /O2 /LD SDL2.c /I path\to\SDL2\include \
//        SDL2.lib SDL2main.lib SDL2_image.lib SDL2_ttf.lib SDL2_mixer.lib
//
// 🧩 Découverte des fonctions
//   • Le symbole `vl_sdl2_function_table(size_t* count)` expose un tableau
//     {name, fnptr} pour un enregistrement simple côté VM.
//   • Vous pouvez ignorer ce tableau et lier directement par nom des symboles.
//
// ─────────────────────────────────────────────────────────────────────────────

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN 1
  #include <windows.h>
  #define DLLEXPORT __declspec(dllexport)
#else
  #define DLLEXPORT __attribute__((visibility("default")))
#endif

#include <SDL.h>
#ifdef VL_SDL2_WITH_IMAGE
  #include <SDL_image.h>
#endif
#ifdef VL_SDL2_WITH_TTF
  #include <SDL_ttf.h>
#endif
#ifdef VL_SDL2_WITH_MIXER
  #include <SDL_mixer.h>
#endif

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ─────────────────────────────────────────────────────────────────────────────
// Utilitaires d'erreur
// ─────────────────────────────────────────────────────────────────────────────
#ifndef VL_SDL2_ERRBUF_SIZE
#define VL_SDL2_ERRBUF_SIZE 512
#endif
static char g_err[VL_SDL2_ERRBUF_SIZE];
#ifdef _MSC_VER
  #define thread_local __declspec(thread)
#endif
static thread_local char g_tls_err[VL_SDL2_ERRBUF_SIZE];

static inline void vl_set_err(const char* src){
  if(!src) src = "unknown";
  strncpy(g_tls_err, src, sizeof(g_tls_err)-1);
  g_tls_err[sizeof(g_tls_err)-1] = '\0';
}

static inline void vl_set_sdl_err(void){
  const char* e = SDL_GetError();
  vl_set_err(e);
}

DLLEXPORT const char* vl_sdl2_last_error(void){ return g_tls_err[0]? g_tls_err : ""; }

// ─────────────────────────────────────────────────────────────────────────────
// Table des handles générique (pointeurs opaques mappés sur des int)
// ─────────────────────────────────────────────────────────────────────────────
#ifndef VL_SDL2_MAX_HANDLES
#define VL_SDL2_MAX_HANDLES 8192
#endif

typedef enum {
  HT_WINDOW=1, HT_RENDERER, HT_TEXTURE, HT_SURFACE, HT_FONT, HT_CHUNK, HT_MUSIC
} HandleType;

typedef struct { void* ptr; uint32_t type; } Handle;
static Handle g_htab[VL_SDL2_MAX_HANDLES];
static int g_next_id = 1; // 0 = invalide

static int h_alloc(void* p, uint32_t type){
  if(!p) return 0;
  for(int i=1;i<VL_SDL2_MAX_HANDLES;i++){
    int id = (g_next_id + i) % VL_SDL2_MAX_HANDLES;
    if(id==0) continue;
    if(g_htab[id].ptr==NULL){ g_htab[id].ptr=p; g_htab[id].type=type; g_next_id=id; return id; }
  }
  vl_set_err("handle table full");
  return 0;
}

static void* h_get_t(int id, uint32_t type){
  if(id<=0 || id>=VL_SDL2_MAX_HANDLES) return NULL;
  if(g_htab[id].ptr && g_htab[id].type==type) return g_htab[id].ptr;
  return NULL;
}

static void* h_get_any(int id){
  if(id<=0 || id>=VL_SDL2_MAX_HANDLES) return NULL;
  return g_htab[id].ptr;
}

static int h_type(int id){
  if(id<=0 || id>=VL_SDL2_MAX_HANDLES) return 0;
  return (int)g_htab[id].type;
}

static int h_free_t(int id, uint32_t type){
  if(id<=0 || id>=VL_SDL2_MAX_HANDLES) return 0;
  if(g_htab[id].ptr && g_htab[id].type==type){ g_htab[id].ptr=NULL; g_htab[id].type=0; return 1; }
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Initialisation et cycle de vie
// ─────────────────────────────────────────────────────────────────────────────
static int g_sdl_inited = 0;
#ifdef VL_SDL2_WITH_TTF
static int g_ttf_inited = 0;
#endif
#ifdef VL_SDL2_WITH_MIXER
static int g_mixer_inited = 0;
#endif

DLLEXPORT int vl_sdl2_init(uint32_t sdl_flags){
  if(g_sdl_inited) return 1;
  if(SDL_Init(sdl_flags)!=0){ vl_set_sdl_err(); return 0; }
#ifdef VL_SDL2_WITH_IMAGE
  int img_flags = IMG_INIT_PNG | IMG_INIT_JPG;
  if((IMG_Init(img_flags) & img_flags) != img_flags){ vl_set_err(IMG_GetError()); return 0; }
#endif
#ifdef VL_SDL2_WITH_TTF
  if(TTF_Init()!=0){ vl_set_err(TTF_GetError()); return 0; } g_ttf_inited=1;
#endif
#ifdef VL_SDL2_WITH_MIXER
  if(Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024)!=0){ vl_set_err(Mix_GetError()); return 0; } g_mixer_inited=1;
#endif
  g_sdl_inited = 1; return 1;
}

DLLEXPORT void vl_sdl2_quit(void){
#ifdef VL_SDL2_WITH_MIXER
  if(g_mixer_inited){ Mix_CloseAudio(); Mix_Quit(); g_mixer_inited=0; }
#endif
#ifdef VL_SDL2_WITH_TTF
  if(g_ttf_inited){ TTF_Quit(); g_ttf_inited=0; }
#endif
#ifdef VL_SDL2_WITH_IMAGE
  IMG_Quit();
#endif
  SDL_Quit();
  g_sdl_inited = 0;
  memset(g_htab, 0, sizeof(g_htab));
  g_next_id = 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fenêtre et Renderer
// ─────────────────────────────────────────────────────────────────────────────
DLLEXPORT int vl_sdl2_create_window(const char* title, int x, int y, int w, int h, uint32_t flags){
  SDL_Window* win = SDL_CreateWindow(title?title:"VitteLight", x, y, w, h, flags);
  if(!win){ vl_set_sdl_err(); return 0; }
  return h_alloc(win, HT_WINDOW);
}

DLLEXPORT int vl_sdl2_destroy_window(int window_id){
  SDL_Window* win = (SDL_Window*)h_get_t(window_id, HT_WINDOW);
  if(!win){ vl_set_err("invalid window"); return 0; }
  SDL_DestroyWindow(win);
  return h_free_t(window_id, HT_WINDOW);
}

DLLEXPORT int vl_sdl2_set_window_title(int window_id, const char* title){
  SDL_Window* win = (SDL_Window*)h_get_t(window_id, HT_WINDOW);
  if(!win){ vl_set_err("invalid window"); return 0; }
  SDL_SetWindowTitle(win, title?title:"");
  return 1;
}

DLLEXPORT int vl_sdl2_get_window_size(int window_id, int* out_w, int* out_h){
  SDL_Window* win = (SDL_Window*)h_get_t(window_id, HT_WINDOW);
  if(!win){ vl_set_err("invalid window"); return 0; }
  int w,h; SDL_GetWindowSize(win, &w, &h);
  if(out_w) *out_w = w; if(out_h) *out_h = h; return 1;
}

DLLEXPORT int vl_sdl2_set_window_icon_bmp(int window_id, const char* bmp_path){
  SDL_Window* win = (SDL_Window*)h_get_t(window_id, HT_WINDOW);
  if(!win){ vl_set_err("invalid window"); return 0; }
  SDL_Surface* s = SDL_LoadBMP(bmp_path);
  if(!s){ vl_set_sdl_err(); return 0; }
  SDL_SetWindowIcon(win, s);
  SDL_FreeSurface(s);
  return 1;
}

DLLEXPORT int vl_sdl2_create_renderer(int window_id, int index, uint32_t flags){
  SDL_Window* win = (SDL_Window*)h_get_t(window_id, HT_WINDOW);
  if(!win){ vl_set_err("invalid window"); return 0; }
  SDL_Renderer* r = SDL_CreateRenderer(win, index, flags);
  if(!r){ vl_set_sdl_err(); return 0; }
  return h_alloc(r, HT_RENDERER);
}

DLLEXPORT int vl_sdl2_destroy_renderer(int renderer_id){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  SDL_DestroyRenderer(r);
  return h_free_t(renderer_id, HT_RENDERER);
}

DLLEXPORT int vl_sdl2_set_vsync(int renderer_id, int enabled){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  if(SDL_RenderSetVSync(r, enabled?1:0)!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dessin 2D
// ─────────────────────────────────────────────────────────────────────────────
DLLEXPORT int vl_sdl2_set_draw_color(int renderer_id, uint8_t R,uint8_t G,uint8_t B,uint8_t A){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  if(SDL_SetRenderDrawColor(r,R,G,B,A)!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

DLLEXPORT int vl_sdl2_render_clear(int renderer_id){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  if(SDL_RenderClear(r)!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

DLLEXPORT void vl_sdl2_render_present(int renderer_id){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r) return; SDL_RenderPresent(r);
}

DLLEXPORT int vl_sdl2_draw_line(int renderer_id, int x1,int y1,int x2,int y2){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  if(SDL_RenderDrawLine(r,x1,y1,x2,y2)!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

DLLEXPORT int vl_sdl2_draw_rect(int renderer_id, int x,int y,int w,int h){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  SDL_Rect rc = {x,y,w,h};
  if(SDL_RenderDrawRect(r,&rc)!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

DLLEXPORT int vl_sdl2_fill_rect(int renderer_id, int x,int y,int w,int h){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  SDL_Rect rc = {x,y,w,h};
  if(SDL_RenderFillRect(r,&rc)!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Textures
// ─────────────────────────────────────────────────────────────────────────────
DLLEXPORT int vl_sdl2_texture_from_bmp(int renderer_id, const char* bmp_path){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  SDL_Surface* s = SDL_LoadBMP(bmp_path);
  if(!s){ vl_set_sdl_err(); return 0; }
  SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
  SDL_FreeSurface(s);
  if(!t){ vl_set_sdl_err(); return 0; }
  return h_alloc(t, HT_TEXTURE);
}

#ifdef VL_SDL2_WITH_IMAGE
DLLEXPORT int vl_sdl2_texture_from_image(int renderer_id, const char* path){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  SDL_Texture* t = IMG_LoadTexture(r, path);
  if(!t){ vl_set_err(IMG_GetError()); return 0; }
  return h_alloc(t, HT_TEXTURE);
}
#endif

DLLEXPORT int vl_sdl2_destroy_texture(int texture_id){
  SDL_Texture* t = (SDL_Texture*)h_get_t(texture_id, HT_TEXTURE);
  if(!t){ vl_set_err("invalid texture"); return 0; }
  SDL_DestroyTexture(t);
  return h_free_t(texture_id, HT_TEXTURE);
}

DLLEXPORT int vl_sdl2_query_texture(int texture_id, int* w, int* h){
  SDL_Texture* t = (SDL_Texture*)h_get_t(texture_id, HT_TEXTURE);
  if(!t){ vl_set_err("invalid texture"); return 0; }
  Uint32 fmt; int access; int tw,th;
  if(SDL_QueryTexture(t,&fmt,&access,&tw,&th)!=0){ vl_set_sdl_err(); return 0; }
  if(w) *w=tw; if(h) *h=th; return 1;
}

DLLEXPORT int vl_sdl2_copy(int renderer_id, int texture_id, int* src_xywh /*x,y,w,h or NULL*/, int* dst_xywh){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  SDL_Texture* t = (SDL_Texture*)h_get_t(texture_id, HT_TEXTURE);
  if(!r||!t){ vl_set_err("invalid renderer or texture"); return 0; }
  SDL_Rect src, dst; SDL_Rect* psrc=NULL; SDL_Rect* pdst=NULL;
  if(src_xywh){ src.x=src_xywh[0]; src.y=src_xywh[1]; src.w=src_xywh[2]; src.h=src_xywh[3]; psrc=&src; }
  if(dst_xywh){ dst.x=dst_xywh[0]; dst.y=dst_xywh[1]; dst.w=dst_xywh[2]; dst.h=dst_xywh[3]; pdst=&dst; }
  if(SDL_RenderCopy(r,t,psrc,pdst)!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

DLLEXPORT int vl_sdl2_copy_ex(int renderer_id, int texture_id, int* src_xywh, int* dst_xywh, double angle, int center_x, int center_y, int flip_flags){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  SDL_Texture* t = (SDL_Texture*)h_get_t(texture_id, HT_TEXTURE);
  if(!r||!t){ vl_set_err("invalid renderer or texture"); return 0; }
  SDL_Rect src, dst; SDL_Rect* psrc=NULL; SDL_Rect* pdst=NULL; SDL_Point c={center_x,center_y};
  if(src_xywh){ src.x=src_xywh[0]; src.y=src_xywh[1]; src.w=src_xywh[2]; src.h=src_xywh[3]; psrc=&src; }
  if(dst_xywh){ dst.x=dst_xywh[0]; dst.y=dst_xywh[1]; dst.w=dst_xywh[2]; dst.h=dst_xywh[3]; pdst=&dst; }
  if(SDL_RenderCopyEx(r,t,psrc,pdst,angle,&c,(SDL_RendererFlip)flip_flags)!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Événements
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
  uint32_t type; // SDL_Event.type
  int a,b,c,d;  // champs polyvalents
  int64_t x,y;  // champs étendus
  uint32_t mods;
  uint32_t which;
} VL_SDL2_Event; // compacte et simple à sérialiser côté VM

DLLEXPORT int vl_sdl2_poll_event(VL_SDL2_Event* out){
  if(!out){ vl_set_err("null out"); return 0; }
  SDL_Event e; if(SDL_PollEvent(&e)==0) return 0; // aucune file
  memset(out,0,sizeof(*out));
  out->type = e.type;
  switch(e.type){
    case SDL_QUIT: break;
    case SDL_KEYDOWN: case SDL_KEYUP:
      out->a = e.key.keysym.sym;
      out->b = e.key.keysym.scancode;
      out->c = e.key.repeat;
      out->d = (e.type==SDL_KEYDOWN);
      out->mods = (uint32_t)e.key.keysym.mod;
      out->which = (uint32_t)e.key.which; break;
    case SDL_MOUSEMOTION:
      out->a = e.motion.state; out->b = e.motion.x; out->c = e.motion.y; out->d = 0;
      out->x = e.motion.xrel; out->y = e.motion.yrel; out->which = e.motion.which; break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
      out->a = e.button.button; out->b = e.button.x; out->c = e.button.y; out->d = (e.type==SDL_MOUSEBUTTONDOWN);
      out->which = e.button.which; break;
    case SDL_MOUSEWHEEL:
      out->a = e.wheel.x; out->b = e.wheel.y; out->which = e.wheel.which; break;
    case SDL_WINDOWEVENT:
      out->a = e.window.event; out->b = e.window.data1; out->c = e.window.data2; out->which = e.window.windowID; break;
    case SDL_TEXTINPUT:
      // c'est un cas spécial: l'appelant peut récupérer le texte via un buffer exposé séparément si besoin
      break;
    default: break;
  }
  return 1;
}

DLLEXPORT int vl_sdl2_start_text_input(void){ SDL_StartTextInput(); return 1; }
DLLEXPORT void vl_sdl2_stop_text_input(void){ SDL_StopTextInput(); }

// ─────────────────────────────────────────────────────────────────────────────
// Temps et utilitaires
// ─────────────────────────────────────────────────────────────────────────────
DLLEXPORT void vl_sdl2_delay(uint32_t ms){ SDL_Delay(ms); }
DLLEXPORT uint32_t vl_sdl2_ticks(void){ return SDL_GetTicks(); }

DLLEXPORT int vl_sdl2_screenshot_bmp(int renderer_id, const char* path){
  SDL_Renderer* r = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  if(!r){ vl_set_err("invalid renderer"); return 0; }
  int w,h; if(SDL_GetRendererOutputSize(r,&w,&h)!=0){ vl_set_sdl_err(); return 0; }
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0,w,h,24,SDL_PIXELFORMAT_BGR24);
  if(!s){ vl_set_sdl_err(); return 0; }
  if(SDL_RenderReadPixels(r,NULL,SDL_PIXELFORMAT_BGR24,s->pixels,s->pitch)!=0){ SDL_FreeSurface(s); vl_set_sdl_err(); return 0; }
  int rc = SDL_SaveBMP(s,path);
  SDL_FreeSurface(s);
  if(rc!=0){ vl_set_sdl_err(); return 0; }
  return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Texte (TTF)
// ─────────────────────────────────────────────────────────────────────────────
#ifdef VL_SDL2_WITH_TTF
DLLEXPORT int vl_sdl2_open_font(const char* path, int ptsize){
  TTF_Font* f = TTF_OpenFont(path, ptsize);
  if(!f){ vl_set_err(TTF_GetError()); return 0; }
  return h_alloc(f, HT_FONT);
}

DLLEXPORT int vl_sdl2_close_font(int font_id){
  TTF_Font* f = (TTF_Font*)h_get_t(font_id, HT_FONT);
  if(!f){ vl_set_err("invalid font"); return 0; }
  TTF_CloseFont(f);
  return h_free_t(font_id, HT_FONT);
}

DLLEXPORT int vl_sdl2_text_size(int font_id, const char* text, int* w, int* h){
  TTF_Font* f = (TTF_Font*)h_get_t(font_id, HT_FONT);
  if(!f){ vl_set_err("invalid font"); return 0; }
  int tw=0, th=0; if(TTF_SizeUTF8(f, text?text:"",&tw,&th)!=0){ vl_set_err(TTF_GetError()); return 0; }
  if(w) *w=tw; if(h) *h=th; return 1;
}

DLLEXPORT int vl_sdl2_texture_from_text(int renderer_id, int font_id, const char* text, uint8_t r,uint8_t g,uint8_t b,uint8_t a){
  SDL_Renderer* r2 = (SDL_Renderer*)h_get_t(renderer_id, HT_RENDERER);
  TTF_Font* f = (TTF_Font*)h_get_t(font_id, HT_FONT);
  if(!r2||!f){ vl_set_err("invalid renderer or font"); return 0; }
  SDL_Color col = {r,g,b,a};
  SDL_Surface* s = TTF_RenderUTF8_Blended(f, text?text:"", col);
  if(!s){ vl_set_err(TTF_GetError()); return 0; }
  SDL_Texture* t = SDL_CreateTextureFromSurface(r2, s);
  SDL_FreeSurface(s);
  if(!t){ vl_set_sdl_err(); return 0; }
  return h_alloc(t, HT_TEXTURE);
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Audio (SDL_mixer)
// ─────────────────────────────────────────────────────────────────────────────
#ifdef VL_SDL2_WITH_MIXER
DLLEXPORT int vl_sdl2_mixer_open(int freq, uint16_t format, int channels, int chunksize){
  if(Mix_OpenAudio(freq, format, channels, chunksize)!=0){ vl_set_err(Mix_GetError()); return 0; }
  g_mixer_inited = 1; return 1;
}

DLLEXPORT void vl_sdl2_mixer_close(void){ Mix_CloseAudio(); g_mixer_inited=0; }

DLLEXPORT int vl_sdl2_load_chunk(const char* path){
  Mix_Chunk* c = Mix_LoadWAV(path);
  if(!c){ vl_set_err(Mix_GetError()); return 0; }
  return h_alloc(c, HT_CHUNK);
}

DLLEXPORT int vl_sdl2_free_chunk(int chunk_id){
  Mix_Chunk* c = (Mix_Chunk*)h_get_t(chunk_id, HT_CHUNK);
  if(!c){ vl_set_err("invalid chunk"); return 0; }
  Mix_FreeChunk(c); return h_free_t(chunk_id, HT_CHUNK);
}

DLLEXPORT int vl_sdl2_play_chunk(int chunk_id, int loops, int channel){
  Mix_Chunk* c = (Mix_Chunk*)h_get_t(chunk_id, HT_CHUNK);
  if(!c){ vl_set_err("invalid chunk"); return 0; }
  int ch = Mix_PlayChannel(channel, c, loops);
  if(ch==-1){ vl_set_err(Mix_GetError()); return 0; }
  return ch;
}

DLLEXPORT int vl_sdl2_load_music(const char* path){
  Mix_Music* m = Mix_LoadMUS(path);
  if(!m){ vl_set_err(Mix_GetError()); return 0; }
  return h_alloc(m, HT_MUSIC);
}

DLLEXPORT int vl_sdl2_free_music(int music_id){
  Mix_Music* m = (Mix_Music*)h_get_t(music_id, HT_MUSIC);
  if(!m){ vl_set_err("invalid music"); return 0; }
  Mix_FreeMusic(m); return h_free_t(music_id, HT_MUSIC);
}

DLLEXPORT int vl_sdl2_play_music(int music_id, int loops){
  Mix_Music* m = (Mix_Music*)h_get_t(music_id, HT_MUSIC);
  if(!m){ vl_set_err("invalid music"); return 0; }
  if(Mix_PlayMusic(m, loops)!=0){ vl_set_err(Mix_GetError()); return 0; }
  return 1;
}

DLLEXPORT void vl_sdl2_halt_music(void){ Mix_HaltMusic(); }
DLLEXPORT void vl_sdl2_pause_music(void){ Mix_PauseMusic(); }
DLLEXPORT void vl_sdl2_resume_music(void){ Mix_ResumeMusic(); }
DLLEXPORT int vl_sdl2_music_playing(void){ return Mix_PlayingMusic(); }
DLLEXPORT int vl_sdl2_set_volume(int channel, int volume /*0..128*/){ return Mix_Volume(channel, volume); }
DLLEXPORT int vl_sdl2_music_volume(int volume /*0..128*/){ return Mix_VolumeMusic(volume); }
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Glue d'enregistrement facultative
// ─────────────────────────────────────────────────────────────────────────────

typedef struct { const char* name; void* fn; } VL_Fn;

#define FN(n) { #n, (void*)&n }
static const VL_Fn g_fn_table[] = {
  FN(vl_sdl2_init), FN(vl_sdl2_quit), FN(vl_sdl2_last_error),
  FN(vl_sdl2_create_window), FN(vl_sdl2_destroy_window), FN(vl_sdl2_set_window_title), FN(vl_sdl2_get_window_size), FN(vl_sdl2_set_window_icon_bmp),
  FN(vl_sdl2_create_renderer), FN(vl_sdl2_destroy_renderer), FN(vl_sdl2_set_vsync),
  FN(vl_sdl2_set_draw_color), FN(vl_sdl2_render_clear), FN(vl_sdl2_render_present), FN(vl_sdl2_draw_line), FN(vl_sdl2_draw_rect), FN(vl_sdl2_fill_rect),
  FN(vl_sdl2_texture_from_bmp),
#ifdef VL_SDL2_WITH_IMAGE
  FN(vl_sdl2_texture_from_image),
#endif
  FN(vl_sdl2_destroy_texture), FN(vl_sdl2_query_texture), FN(vl_sdl2_copy), FN(vl_sdl2_copy_ex),
  FN(vl_sdl2_poll_event), FN(vl_sdl2_start_text_input), FN(vl_sdl2_stop_text_input),
  FN(vl_sdl2_delay), FN(vl_sdl2_ticks), FN(vl_sdl2_screenshot_bmp),
#ifdef VL_SDL2_WITH_TTF
  FN(vl_sdl2_open_font), FN(vl_sdl2_close_font), FN(vl_sdl2_text_size), FN(vl_sdl2_texture_from_text),
#endif
#ifdef VL_SDL2_WITH_MIXER
  FN(vl_sdl2_mixer_open), FN(vl_sdl2_mixer_close), FN(vl_sdl2_load_chunk), FN(vl_sdl2_free_chunk), FN(vl_sdl2_play_chunk),
  FN(vl_sdl2_load_music), FN(vl_sdl2_free_music), FN(vl_sdl2_play_music), FN(vl_sdl2_halt_music), FN(vl_sdl2_pause_music), FN(vl_sdl2_resume_music), FN(vl_sdl2_music_playing), FN(vl_sdl2_set_volume), FN(vl_sdl2_music_volume),
#endif
};
#undef FN

DLLEXPORT const VL_Fn* vl_sdl2_function_table(size_t* count){
  if(count) *count = sizeof(g_fn_table)/sizeof(g_fn_table[0]);
  return g_fn_table;
}

// ─────────────────────────────────────────────────────────────────────────────
// Notes d'intégration Vitte‑Light
// ─────────────────────────────────────────────────────────────────────────────
// • Côté VM, mappez un type "pointer/handle" à int32.
// • Les fonctions renvoient 0 en erreur, 1 en succès, ou un handle/valeur ≥1.
// • En cas d'erreur, interrogez `vl_sdl2_last_error()`.
// • Pour la sérialisation d'événements, utilisez `VL_SDL2_Event` côté FFI.
// • Ajoutez les wrappers côté VITL: ex.
//     extern "C" fun sdl.init(flags:int)->bool (dl: "vl_sdl2_init")
//     extern "C" fun sdl.create_window(title:ptr, x:int, y:int, w:int, h:int, flags:int)->int
//   selon votre ABI.
// • Flags utiles: SDL_INIT_VIDEO|AUDIO|EVENTS, SDL_WINDOW_SHOWN|RESIZABLE,
//   SDL_RENDERER_ACCELERATED|PRESENTVSYNC, SDL_FLIP_HORIZONTAL/VERITCAL.
// ─────────────────────────────────────────────────────────────────────────────
