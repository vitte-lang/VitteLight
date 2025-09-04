// vitte-light/libraries/ncurses.c
// Liaison ncurses pour VitteLight: TUI, couleurs, clavier, curseur.
// C99. Conçu pour ncursesw (UTF‑8). Fonctionne aussi avec ncurses/PDCurses.
//
// Entrée publique:
//   void vl_register_ncurses(struct VL_Context *ctx);
//
// Natives exposées (stdscr uniquement):
//   term_init([raw=1][,echo=0][,cbreak=1][,keypad=1][,color=1]) -> bool
//   term_end() -> nil
//   term_clear() -> nil
//   term_refresh() -> nil
//   term_move(y,x) -> bool
//   term_addstr(bytes) -> bool           // bytes UTF‑8
//   term_addch(code) -> bool             // code Unicode ou A_CHARTEXT
//   term_getch() -> int                  // KEY_* ou codepoint
//   term_timeout(ms) -> nil              // -1 blocant, 0 non‑blocant
//   term_nodelay(flag) -> nil
//   term_cursor(vis) -> bool             // 0/1/2
//   term_echo(flag) -> nil
//   term_cbreak(flag) -> nil
//   term_keypad(flag) -> nil
//   term_rows() -> int
//   term_cols() -> int
//   term_size() -> int                   // (rows<<32)|cols
//   term_getyx() -> int                  // (y<<32)|x
//   term_clrtoeol() -> nil
//   term_clrtobot() -> nil
//   term_flushinp() -> nil
//   term_beep() -> nil
//   term_flash() -> nil
//   term_scrollok(flag) -> nil
//   term_has_colors() -> bool
//   term_start_color() -> bool
//   term_init_pair(pair, fg, bg) -> bool
//   term_color_pair(pair) -> int         // attribut pour attr_on/off
//   term_attr_on(mask) -> nil
//   term_attr_off(mask) -> nil
//   term_bold(flag) -> nil               // helpers rapides
//   term_underline(flag) -> nil
//   term_reverse(flag) -> nil
//
// Build (Linux/BSD):
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -D_XOPEN_SOURCE_EXTENDED \
//      -Icore -c libraries/ncurses.c && cc ... -lncursesw
// Sur Windows avec PDCurses: adaptez l’édition de liens (-lpdcurses).

#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "api.h"
#include "ctype.h"
#include "string.h"  // VL_String, vl_make_strn

// Essayez <curses.h> en premier (ncurses installe généralement curses.h)
#include <curses.h>

// ───────────────────────── État global ─────────────────────────
static int g_term_inited = 0;

// ───────────────────────── VM glue ─────────────────────────
#define RET_NIL()                \
  do {                           \
    if (ret) *(ret) = vlv_nil(); \
    return VL_OK;                \
  } while (0)
#define RET_INT(v)                           \
  do {                                       \
    if (ret) *(ret) = vlv_int((int64_t)(v)); \
    return VL_OK;                            \
  } while (0)
#define RET_BOOL(v)                       \
  do {                                    \
    if (ret) *(ret) = vlv_bool((v) != 0); \
    return VL_OK;                         \
  } while (0)

static int need_str(const VL_Value *v) {
  return v && v->type == VT_STR && v->as.s;
}
static int64_t want_i64(const VL_Value *v, int *ok) {
  int64_t x = 0;
  *ok = vl_value_as_int(v, &x);
  return x;
}
static int want_boolish(const VL_Value *v, int def) {
  if (!v || v->type == VT_NIL) return def;
  if (v->type == VT_BOOL) return v->as.b ? 1 : 0;
  if (v->type == VT_INT) return v->as.i != 0;
  if (v->type == VT_FLOAT) return v->as.f != 0.0;
  if (v->type == VT_STR) return v->as.s && v->as.s->len > 0;
  return def;
}

// ───────────────────────── Helpers ─────────────────────────
static void apply_modes(int rawm, int echom, int cbrkm, int keym) {
  if (rawm)
    raw();
  else
    noraw();
  if (echom)
    echo();
  else
    noecho();
  if (cbrkm)
    cbreak();
  else
    nocbreak();
  keypad(stdscr, keym ? TRUE : FALSE);
}

static int ensure_inited(void) { return g_term_inited; }

// ───────────────────────── Impl natives ─────────────────────────
static VL_Status t_init(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  // initialise locale pour UTF‑8 si possible
  if (!setlocale(LC_ALL, "")) setlocale(LC_CTYPE, "");
  if (!g_term_inited) {
    if (initscr() == NULL) RET_BOOL(0);
    g_term_inited = 1;
  }
  int rawm = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  int echom = (c >= 2) ? want_boolish(&a[1], 0) : 0;
  int cbrkm = (c >= 3) ? want_boolish(&a[2], 1) : 1;
  int keym = (c >= 4) ? want_boolish(&a[3], 1) : 1;
  int wantc = (c >= 5) ? want_boolish(&a[4], 1) : 1;
  apply_modes(rawm, echom, cbrkm, keym);
  if (has_colors() && wantc) {
    start_color();
  }
#ifdef NCURSES_VERSION
  // UTF‑8 input en ncursesw
  use_default_colors();
#endif
  RET_BOOL(1);
}

static VL_Status t_end(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                       VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (g_term_inited) {
    endwin();
    g_term_inited = 0;
  }
  RET_NIL();
}

static VL_Status t_clear(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (!ensure_inited()) return VL_ERR_INVAL;
  clear();
  RET_NIL();
}
static VL_Status t_refresh(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (!ensure_inited()) return VL_ERR_INVAL;
  refresh();
  RET_NIL();
}

static VL_Status t_move(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  if (c < 2) return VL_ERR_INVAL;
  int ok = 1;
  int64_t y = want_i64(&a[0], &ok);
  if (!ok) return VL_ERR_TYPE;
  int64_t x = want_i64(&a[1], &ok);
  if (!ok) return VL_ERR_TYPE;
  if (!ensure_inited()) return VL_ERR_INVAL;
  int rc = move((int)y, (int)x);
  RET_BOOL(rc == OK);
}

static VL_Status t_addstr(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  if (c < 1 || !need_str(&a[0])) return VL_ERR_TYPE;
  if (!ensure_inited()) return VL_ERR_INVAL;
  int rc = addnstr(a[0].as.s->data, (int)a[0].as.s->len);
  RET_BOOL(rc == OK);
}

static VL_Status t_addch(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  if (c < 1) return VL_ERR_INVAL;
  if (!ensure_inited()) return VL_ERR_INVAL;
  int ok = 1;
  int64_t code = want_i64(&a[0], &ok);
  if (!ok) return VL_ERR_TYPE;
  int rc = addch((chtype)code);
  RET_BOOL(rc == OK);
}

static VL_Status t_getch(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  (void)a;
  (void)c;
  if (!ensure_inited()) return VL_ERR_INVAL;
  int ch = getch();
  RET_INT((int64_t)ch);
}

static VL_Status t_timeout(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  if (c < 1) return VL_ERR_INVAL;
  int ok = 1;
  int64_t ms = want_i64(&a[0], &ok);
  if (!ok) return VL_ERR_TYPE;
  timeout((int)ms);
  RET_NIL();
}
static VL_Status t_nodelay(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  int flag = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  nodelay(stdscr, flag ? TRUE : FALSE);
  RET_NIL();
}

static VL_Status t_cursor(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  if (c < 1) return VL_ERR_INVAL;
  int ok = 1;
  int64_t v = want_i64(&a[0], &ok);
  if (!ok) return VL_ERR_TYPE;
  int rc = curs_set((int)v);
  RET_BOOL(rc != ERR);
}
static VL_Status t_echo(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  int on = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  if (on)
    echo();
  else
    noecho();
  RET_NIL();
}
static VL_Status t_cbreak(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  int on = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  if (on)
    cbreak();
  else
    nocbreak();
  RET_NIL();
}
static VL_Status t_keypad(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                          VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  int on = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  keypad(stdscr, on ? TRUE : FALSE);
  RET_NIL();
}

static VL_Status t_rows(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *ud) {
  (void)a;
  (void)c;
  (void)ud;
  (void)ctx;
  if (!ensure_inited()) return VL_ERR_INVAL;
  int y, x;
  getmaxyx(stdscr, y, x);
  RET_INT((int64_t)y);
}
static VL_Status t_cols(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *ud) {
  (void)a;
  (void)c;
  (void)ud;
  (void)ctx;
  if (!ensure_inited()) return VL_ERR_INVAL;
  int y, x;
  getmaxyx(stdscr, y, x);
  RET_INT((int64_t)x);
}
static VL_Status t_size(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *ud) {
  (void)a;
  (void)c;
  (void)ud;
  (void)ctx;
  if (!ensure_inited()) return VL_ERR_INVAL;
  int y, x;
  getmaxyx(stdscr, y, x);
  int64_t pack = ((int64_t)y << 32) | ((uint32_t)x);
  RET_INT(pack);
}
static VL_Status t_getyx_pack(struct VL_Context *ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *ud) {
  (void)a;
  (void)c;
  (void)ud;
  (void)ctx;
  if (!ensure_inited()) return VL_ERR_INVAL;
  int y, x;
  getyx(stdscr, y, x);
  int64_t pack = ((int64_t)y << 32) | ((uint32_t)x);
  RET_INT(pack);
}

static VL_Status t_clrtoeol(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (!ensure_inited()) return VL_ERR_INVAL;
  clrtoeol();
  RET_NIL();
}
static VL_Status t_clrtobot(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (!ensure_inited()) return VL_ERR_INVAL;
  clrtobot();
  RET_NIL();
}
static VL_Status t_flushinp(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (!ensure_inited()) return VL_ERR_INVAL;
  flushinp();
  RET_NIL();
}
static VL_Status t_beep(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (!ensure_inited()) return VL_ERR_INVAL;
  beep();
  RET_NIL();
}
static VL_Status t_flash(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                         VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (!ensure_inited()) return VL_ERR_INVAL;
  flash();
  RET_NIL();
}
static VL_Status t_scrollok(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  int on = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  scrollok(stdscr, on ? TRUE : FALSE);
  RET_NIL();
}

// Couleurs et attributs
static VL_Status t_has_colors(struct VL_Context *ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  RET_BOOL(has_colors());
}
static VL_Status t_start_color(struct VL_Context *ctx, const VL_Value *a,
                               uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)a;
  (void)c;
  (void)ud;
  if (!has_colors()) RET_BOOL(0);
  RET_BOOL(start_color() == OK);
}
static VL_Status t_init_pair(struct VL_Context *ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  if (c < 3) return VL_ERR_INVAL;
  int ok = 1;
  int64_t p = want_i64(&a[0], &ok);
  if (!ok) return VL_ERR_TYPE;
  int64_t f = want_i64(&a[1], &ok);
  if (!ok) return VL_ERR_TYPE;
  int64_t b = want_i64(&a[2], &ok);
  if (!ok) return VL_ERR_TYPE;
  int rc = init_pair((short)p, (short)f, (short)b);
  RET_BOOL(rc == OK);
}
static VL_Status t_color_pair(struct VL_Context *ctx, const VL_Value *a,
                              uint8_t c, VL_Value *ret, void *ud) {
  (void)ctx;
  (void)ud;
  if (c < 1) return VL_ERR_INVAL;
  int ok = 1;
  int64_t p = want_i64(&a[0], &ok);
  if (!ok) return VL_ERR_TYPE;
  RET_INT((int64_t)COLOR_PAIR((int)p));
}
static VL_Status t_attr_on(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  if (c < 1) return VL_ERR_INVAL;
  int ok = 1;
  int64_t m = want_i64(&a[0], &ok);
  if (!ok) return VL_ERR_TYPE;
  attron((attr_t)m);
  RET_NIL();
}
static VL_Status t_attr_off(struct VL_Context *ctx, const VL_Value *a,
                            uint8_t c, VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  if (c < 1) return VL_ERR_INVAL;
  int ok = 1;
  int64_t m = want_i64(&a[0], &ok);
  if (!ok) return VL_ERR_TYPE;
  attroff((attr_t)m);
  RET_NIL();
}

static VL_Status t_bold(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                        VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  int on = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  if (on)
    attron(A_BOLD);
  else
    attroff(A_BOLD);
  RET_NIL();
}
static VL_Status t_underline(struct VL_Context *ctx, const VL_Value *a,
                             uint8_t c, VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  int on = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  if (on)
    attron(A_UNDERLINE);
  else
    attroff(A_UNDERLINE);
  RET_NIL();
}
static VL_Status t_reverse(struct VL_Context *ctx, const VL_Value *a, uint8_t c,
                           VL_Value *ret, void *ud) {
  (void)ud;
  (void)ctx;
  int on = (c >= 1) ? want_boolish(&a[0], 1) : 1;
  if (on)
    attron(A_REVERSE);
  else
    attroff(A_REVERSE);
  RET_NIL();
}

// ───────────────────────── Enregistrement ─────────────────────────
void vl_register_ncurses(struct VL_Context *ctx) {
  if (!ctx) return;
  vl_register_native(ctx, "term_init", t_init, NULL);
  vl_register_native(ctx, "term_end", t_end, NULL);
  vl_register_native(ctx, "term_clear", t_clear, NULL);
  vl_register_native(ctx, "term_refresh", t_refresh, NULL);
  vl_register_native(ctx, "term_move", t_move, NULL);
  vl_register_native(ctx, "term_addstr", t_addstr, NULL);
  vl_register_native(ctx, "term_addch", t_addch, NULL);
  vl_register_native(ctx, "term_getch", t_getch, NULL);
  vl_register_native(ctx, "term_timeout", t_timeout, NULL);
  vl_register_native(ctx, "term_nodelay", t_nodelay, NULL);
  vl_register_native(ctx, "term_cursor", t_cursor, NULL);
  vl_register_native(ctx, "term_echo", t_echo, NULL);
  vl_register_native(ctx, "term_cbreak", t_cbreak, NULL);
  vl_register_native(ctx, "term_keypad", t_keypad, NULL);
  vl_register_native(ctx, "term_rows", t_rows, NULL);
  vl_register_native(ctx, "term_cols", t_cols, NULL);
  vl_register_native(ctx, "term_size", t_size, NULL);
  vl_register_native(ctx, "term_getyx", t_getyx_pack, NULL);
  vl_register_native(ctx, "term_clrtoeol", t_clrtoeol, NULL);
  vl_register_native(ctx, "term_clrtobot", t_clrtobot, NULL);
  vl_register_native(ctx, "term_flushinp", t_flushinp, NULL);
  vl_register_native(ctx, "term_beep", t_beep, NULL);
  vl_register_native(ctx, "term_flash", t_flash, NULL);
  vl_register_native(ctx, "term_scrollok", t_scrollok, NULL);

  vl_register_native(ctx, "term_has_colors", t_has_colors, NULL);
  vl_register_native(ctx, "term_start_color", t_start_color, NULL);
  vl_register_native(ctx, "term_init_pair", t_init_pair, NULL);
  vl_register_native(ctx, "term_color_pair", t_color_pair, NULL);
  vl_register_native(ctx, "term_attr_on", t_attr_on, NULL);
  vl_register_native(ctx, "term_attr_off", t_attr_off, NULL);
  vl_register_native(ctx, "term_bold", t_bold, NULL);
  vl_register_native(ctx, "term_underline", t_underline, NULL);
  vl_register_native(ctx, "term_reverse", t_reverse, NULL);
}

// ───────────────────────── Notes
// * getch() renvoie KEY_* pour les touches spéciales (flèches, F1, etc.).
// * term_size()/term_getyx() empaquettent deux entiers 32b en un int64:
//     haut 32 bits = y/rows, bas 32 bits = x/cols.
// * Pour l’UTF‑8, liez ncursesw et assurez‑vous que LC_ALL est configurée.
