// SPDX-License-Identifier: GPL-3.0-or-later
//
// ncurses.c — Bibliothèque I/O terminal (ncurses) pour Vitte Light (C17)
// Namespace: "curses"
//
// Portabilité:
//   - Active si compilé avec -DVL_HAVE_NCURSES et <ncurses.h>/<curses.h> dispo.
//   - Sinon, stubs renvoyant (nil,"ENOSYS") ou false.
//
// API (stdscr seulement, pas d’objets fenêtre):
//   curses.initscr()                  -> true | (nil, errmsg)
//   curses.endwin()                   -> true | (nil, errmsg)
//   curses.cbreak(), curses.nocbreak() -> true | (nil, errmsg)
//   curses.raw(), curses.noraw()      -> true | (nil, errmsg)
//   curses.echo(), curses.noecho()    -> true | (nil, errmsg)
//   curses.keypad(enable:boolean)     -> true | (nil, errmsg)
//   curses.curs_set(vis:int)          -> prev_vis:int | (nil, errmsg)   //
//   0,1,2 curses.timeout_ms(ms:int)         -> true | (nil, errmsg) // getch
//   timeout curses.nodelay(enable:boolean)    -> true | (nil, errmsg)
//   curses.clear(), curses.erase(), curses.refresh() -> true | (nil, errmsg)
//   curses.move(y:int, x:int)         -> true | (nil, errmsg)
//   curses.addstr(s:string)           -> true | (nil, errmsg)
//   curses.addch(code:int)            -> true | (nil, errmsg)
//   curses.getch()                    -> keycode:int | (nil, errmsg)     // -1
//   si timeout curses.getmaxyx()                 -> rows:int, cols:int
//   curses.beep(), curses.flash()     -> true | (nil, errmsg)
//
// Couleurs et attributs:
//   curses.start_color()              -> true | (nil, errmsg)
//   curses.has_colors()               -> boolean
//   curses.colors()                   -> int
//   curses.color_pairs()              -> int
//   curses.init_pair(id:int, fg:int, bg:int) -> true | (nil, errmsg)
//   curses.set_color_pair(id:int)     -> true | (nil, errmsg)            //
//   attron(COLOR_PAIR(id)) curses.color(name:string)         -> code:int |
//   (nil, errmsg)        // "black","red",… curses.attr_bold(on:boolean) ->
//   true | (nil, errmsg) curses.attr_underline(on:boolean) -> true | (nil,
//   errmsg) curses.attr_reverse(on:boolean)   -> true | (nil, errmsg)
//   curses.attr_dim(on:boolean)       -> true | (nil, errmsg)
//   curses.attr_standout(on:boolean)  -> true | (nil, errmsg)
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef VL_HAVE_NCURSES
#if __has_include(<ncurses.h>)
#include <ncurses.h>
#else
#include <curses.h>
#endif
#endif

// ---------------------------------------------------------------
// Aide VM
// ---------------------------------------------------------------

static const char *nc_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t nc_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int nc_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}

// ---------------------------------------------------------------
// Implémentation ncurses / stubs
// ---------------------------------------------------------------

#ifndef VL_HAVE_NCURSES
// ---------- STUBS ----------
static int stub_bool_ret(VL_State *S) {
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
}
static int vlnc_initscr(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_endwin(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_cbreak(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_nocbreak(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_raw(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_noraw(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_echo(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_noecho(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_keypad(VL_State *S) {
  (void)nc_opt_bool(S, 1, 1);
  return stub_bool_ret(S);
}
static int vlnc_curs_set(VL_State *S) {
  (void)nc_check_int(S, 1);
  return stub_bool_ret(S);
}
static int vlnc_timeout_ms(VL_State *S) {
  (void)nc_check_int(S, 1);
  return stub_bool_ret(S);
}
static int vlnc_nodelay(VL_State *S) {
  (void)nc_opt_bool(S, 1, 1);
  return stub_bool_ret(S);
}
static int vlnc_clear(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_erase(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_refresh(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_move(VL_State *S) {
  (void)nc_check_int(S, 1);
  (void)nc_check_int(S, 2);
  return stub_bool_ret(S);
}
static int vlnc_addstr(VL_State *S) {
  (void)nc_check_str(S, 1);
  return stub_bool_ret(S);
}
static int vlnc_addch(VL_State *S) {
  (void)nc_check_int(S, 1);
  return stub_bool_ret(S);
}
static int vlnc_getch(VL_State *S) {
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
}
static int vlnc_getmaxyx(VL_State *S) {
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
}
static int vlnc_beep(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_flash(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_start_color(VL_State *S) { return stub_bool_ret(S); }
static int vlnc_has_colors(VL_State *S) {
  vl_push_bool(S, 0);
  return 1;
}
static int vlnc_colors(VL_State *S) {
  vl_push_int(S, 0);
  return 1;
}
static int vlnc_color_pairs(VL_State *S) {
  vl_push_int(S, 0);
  return 1;
}
static int vlnc_init_pair(VL_State *S) {
  (void)nc_check_int(S, 1);
  (void)nc_check_int(S, 2);
  (void)nc_check_int(S, 3);
  return stub_bool_ret(S);
}
static int vlnc_set_color_pair(VL_State *S) {
  (void)nc_check_int(S, 1);
  return stub_bool_ret(S);
}
static int vlnc_color(VL_State *S) {
  (void)nc_check_str(S, 1);
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
}
static int vlnc_attr_bold(VL_State *S) {
  (void)nc_opt_bool(S, 1, 1);
  return stub_bool_ret(S);
}
static int vlnc_attr_underline(VL_State *S) {
  (void)nc_opt_bool(S, 1, 1);
  return stub_bool_ret(S);
}
static int vlnc_attr_reverse(VL_State *S) {
  (void)nc_opt_bool(S, 1, 1);
  return stub_bool_ret(S);
}
static int vlnc_attr_dim(VL_State *S) {
  (void)nc_opt_bool(S, 1, 1);
  return stub_bool_ret(S);
}
static int vlnc_attr_standout(VL_State *S) {
  (void)nc_opt_bool(S, 1, 1);
  return stub_bool_ret(S);
}

#else
// ---------- NCURSES RÉEL ----------
static int g_nc_inited = 0;

static int ok_or_err(VL_State *S, int rc) {
  if (rc == OK) {
    vl_push_bool(S, 1);
    return 1;
  }
  vl_push_nil(S);
  vl_push_string(S, "EIO");
  return 2;
}

static int vlnc_initscr(VL_State *S) {
  (void)S;
  if (g_nc_inited) {
    vl_push_bool(S, 1);
    return 1;
  }
  if (initscr() == NULL) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  // Paramétrage par défaut utile
  if (cbreak() == ERR) {
    endwin();
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  if (noecho() == ERR) {
    endwin();
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  keypad(stdscr, TRUE);
  nodelay(stdscr, FALSE);
  timeout(-1);
  g_nc_inited = 1;
  vl_push_bool(S, 1);
  return 1;
}

static int vlnc_endwin(VL_State *S) {
  (void)S;
  if (!g_nc_inited) {
    vl_push_bool(S, 1);
    return 1;
  }
  int rc = endwin();
  g_nc_inited = 0;
  return ok_or_err(S, rc);
}

static int vlnc_cbreak(VL_State *S) { return ok_or_err(S, cbreak()); }
static int vlnc_nocbreak(VL_State *S) { return ok_or_err(S, nocbreak()); }
static int vlnc_raw(VL_State *S) { return ok_or_err(S, raw()); }
static int vlnc_noraw(VL_State *S) { return ok_or_err(S, noraw()); }
static int vlnc_echo(VL_State *S) {
  echo();
  vl_push_bool(S, 1);
  return 1;
}
static int vlnc_noecho(VL_State *S) {
  noecho();
  vl_push_bool(S, 1);
  return 1;
}

static int vlnc_keypad(VL_State *S) {
  int en = nc_opt_bool(S, 1, 1);
  keypad(stdscr, en ? TRUE : FALSE);
  vl_push_bool(S, 1);
  return 1;
}

static int vlnc_curs_set(VL_State *S) {
  int vis = (int)nc_check_int(S, 1);
  int rc = curs_set(vis);
  if (rc == ERR) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_int(S, (int64_t)rc);
  return 1;
}

static int vlnc_timeout_ms(VL_State *S) {
  int ms = (int)nc_check_int(S, 1);
  timeout(ms);
  vl_push_bool(S, 1);
  return 1;
}

static int vlnc_nodelay(VL_State *S) {
  int en = nc_opt_bool(S, 1, 1);
  nodelay(stdscr, en ? TRUE : FALSE);
  vl_push_bool(S, 1);
  return 1;
}

static int vlnc_clear(VL_State *S) { return ok_or_err(S, clear()); }
static int vlnc_erase(VL_State *S) { return ok_or_err(S, erase()); }
static int vlnc_refresh(VL_State *S) { return ok_or_err(S, refresh()); }

static int vlnc_move(VL_State *S) {
  int y = (int)nc_check_int(S, 1);
  int x = (int)nc_check_int(S, 2);
  return ok_or_err(S, move(y, x));
}

static int vlnc_addstr(VL_State *S) {
  const char *s = nc_check_str(S, 1);
  return ok_or_err(S, addstr(s));
}

static int vlnc_addch(VL_State *S) {
  int code = (int)nc_check_int(S, 1);
  return ok_or_err(S, addch((chtype)code));
}

static int vlnc_getch(VL_State *S) {
  int c = getch();
  if (c == ERR) {
    vl_push_int(S, (int64_t)-1);
    return 1;
  }
  vl_push_int(S, (int64_t)c);
  return 1;
}

static int vlnc_getmaxyx(VL_State *S) {
  int rows = 0, cols = 0;
  getmaxyx(stdscr, rows, cols);
  vl_push_int(S, (int64_t)rows);
  vl_push_int(S, (int64_t)cols);
  return 2;
}

static int vlnc_beep(VL_State *S) { return ok_or_err(S, beep()); }
static int vlnc_flash(VL_State *S) { return ok_or_err(S, flash()); }

// --- Couleurs / Attributs ---

static int vlnc_start_color(VL_State *S) { return ok_or_err(S, start_color()); }
static int vlnc_has_colors(VL_State *S) {
  vl_push_bool(S, has_colors() ? 1 : 0);
  return 1;
}
static int vlnc_colors(VL_State *S) {
  vl_push_int(S, (int64_t)COLORS);
  return 1;
}
static int vlnc_color_pairs(VL_State *S) {
  vl_push_int(S, (int64_t)COLOR_PAIRS);
  return 1;
}

static int vlnc_init_pair(VL_State *S) {
  short id = (short)nc_check_int(S, 1);
  short fg = (short)nc_check_int(S, 2);
  short bg = (short)nc_check_int(S, 3);
  return ok_or_err(S, init_pair(id, fg, bg));
}

static int vlnc_set_color_pair(VL_State *S) {
  short id = (short)nc_check_int(S, 1);
  attron(COLOR_PAIR(id));
  vl_push_bool(S, 1);
  return 1;
}

static int color_from_name(const char *name) {
  // Couleurs de base ncurses
  if (!name) return -1;
  if (!strcasecmp(name, "black")) return COLOR_BLACK;
  if (!strcasecmp(name, "red")) return COLOR_RED;
  if (!strcasecmp(name, "green")) return COLOR_GREEN;
  if (!strcasecmp(name, "yellow")) return COLOR_YELLOW;
  if (!strcasecmp(name, "blue")) return COLOR_BLUE;
  if (!strcasecmp(name, "magenta")) return COLOR_MAGENTA;
  if (!strcasecmp(name, "cyan")) return COLOR_CYAN;
  if (!strcasecmp(name, "white")) return COLOR_WHITE;
  return -1;
}

static int vlnc_color(VL_State *S) {
  const char *n = nc_check_str(S, 1);
  int c = color_from_name(n);
  if (c < 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_int(S, (int64_t)c);
  return 1;
}

static void attr_toggle(chtype m, int on) {
  if (on)
    attron(m);
  else
    attroff(m);
}

static int vlnc_attr_bold(VL_State *S) {
  int on = nc_opt_bool(S, 1, 1);
  attr_toggle(A_BOLD, on);
  vl_push_bool(S, 1);
  return 1;
}
static int vlnc_attr_underline(VL_State *S) {
  int on = nc_opt_bool(S, 1, 1);
  attr_toggle(A_UNDERLINE, on);
  vl_push_bool(S, 1);
  return 1;
}
static int vlnc_attr_reverse(VL_State *S) {
  int on = nc_opt_bool(S, 1, 1);
  attr_toggle(A_REVERSE, on);
  vl_push_bool(S, 1);
  return 1;
}
static int vlnc_attr_dim(VL_State *S) {
  int on = nc_opt_bool(S, 1, 1);
  attr_toggle(A_DIM, on);
  vl_push_bool(S, 1);
  return 1;
}
static int vlnc_attr_standout(VL_State *S) {
  int on = nc_opt_bool(S, 1, 1);
  attr_toggle(A_STANDOUT, on);
  vl_push_bool(S, 1);
  return 1;
}

#endif  // VL_HAVE_NCURSES

// ---------------------------------------------------------------
// Enregistrement VM
// ---------------------------------------------------------------

static const VL_Reg curseslib[] = {{"initscr", vlnc_initscr},
                                   {"endwin", vlnc_endwin},
                                   {"cbreak", vlnc_cbreak},
                                   {"nocbreak", vlnc_nocbreak},
                                   {"raw", vlnc_raw},
                                   {"noraw", vlnc_noraw},
                                   {"echo", vlnc_echo},
                                   {"noecho", vlnc_noecho},
                                   {"keypad", vlnc_keypad},
                                   {"curs_set", vlnc_curs_set},
                                   {"timeout_ms", vlnc_timeout_ms},
                                   {"nodelay", vlnc_nodelay},
                                   {"clear", vlnc_clear},
                                   {"erase", vlnc_erase},
                                   {"refresh", vlnc_refresh},
                                   {"move", vlnc_move},
                                   {"addstr", vlnc_addstr},
                                   {"addch", vlnc_addch},
                                   {"getch", vlnc_getch},
                                   {"getmaxyx", vlnc_getmaxyx},
                                   {"beep", vlnc_beep},
                                   {"flash", vlnc_flash},

                                   {"start_color", vlnc_start_color},
                                   {"has_colors", vlnc_has_colors},
                                   {"colors", vlnc_colors},
                                   {"color_pairs", vlnc_color_pairs},
                                   {"init_pair", vlnc_init_pair},
                                   {"set_color_pair", vlnc_set_color_pair},
                                   {"color", vlnc_color},

                                   {"attr_bold", vlnc_attr_bold},
                                   {"attr_underline", vlnc_attr_underline},
                                   {"attr_reverse", vlnc_attr_reverse},
                                   {"attr_dim", vlnc_attr_dim},
                                   {"attr_standout", vlnc_attr_standout},

                                   {NULL, NULL}};

void vl_open_curseslib(VL_State *S) { vl_register_lib(S, "curses", curseslib); }
