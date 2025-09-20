// SPDX-License-Identifier: GPL-3.0-or-later
//
// ncurses.c — Enveloppe TUI minimale pour Vitte Light (C17)
// Namespace: "tui"
//
// POSIX: ncursesw (recommandé) ou ncurses.
// Windows: PDCurses (pdcurses).
//
// Build (Linux/macOS):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c ncurses.c -lncursesw
// Test:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DTUI_TEST ncurses.c -lncursesw && ./a.out
//
// Fournit :
//   - Initialisation/fin: tui_init(), tui_end()
//   - Couleurs: tui_has_colors(), tui_pair(fg,bg)->pair_id
//   - I/O non bloquante: tui_getch(), tui_readline(prompt,buf,cap)
//   - Dessin: tui_clear(), tui_box(x,y,w,h), tui_print(x,y,attr,fmt,...)
//   - Progress bar: tui_progress(x,y,w,frac,label)
//   - Status bar: tui_status(text)
//   - Fenêtres dérivées simples: tui_win_make / tui_win_free
//   - Redimensionnement: tui_size(&cols,&rows), KEY_RESIZE relu via tui_getch()
//
// Remarque: évite l’allocation dynamique sauf pour tui_readline (temporaires).

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
  #include <curses.h>   /* PDCurses (pdcurses) */
#else
  #include <locale.h>
  #include <wchar.h>
  #include <curses.h>   /* ncurses/ncursesw */
#endif

#ifndef TUI_API
#define TUI_API
#endif

typedef struct { WINDOW* w; int x,y,wid,hei; } tui_win;

/* ========================= Core ========================= */

static int _tui_cols=0, _tui_rows=0;
static int _tui_status_en=0;

TUI_API int tui_init(void){
#if !defined(_WIN32)
    setlocale(LC_ALL,"");
#endif
    if (initscr()==NULL) return -1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    _tui_cols = COLS; _tui_rows = LINES;
    if (has_colors()){
        start_color();
#if defined(NCURSES_VERSION)
        use_default_colors();
#endif
    }
    _tui_status_en=1;
    return 0;
}
TUI_API void tui_end(void){
    endwin();
}

TUI_API void tui_size(int* cols, int* rows){
    if (cols) *cols = COLS;
    if (rows) *rows = LINES;
    _tui_cols = COLS; _tui_rows = LINES;
}

TUI_API int tui_has_colors(void){ return has_colors(); }

/* fg/bg in [-1..7] pour default + couleurs de base si supporte use_default_colors */
TUI_API short tui_pair(short fg, short bg){
    static short next=1;
    short id = next++;
#if defined(NCURSES_VERSION)
    init_pair(id, fg, bg);
#else
    init_pair(id, fg<0?COLOR_WHITE:fg, bg<0?COLOR_BLACK:bg);
#endif
    return id;
}

/* ========================= Drawing ========================= */

TUI_API void tui_clear(void){
    erase();
}

TUI_API void tui_box(int x,int y,int w,int h){
    if (w<=0||h<=0) return;
    /* Utilise un sous-window pour tracé propre */
    WINDOW* win = newwin(h, w, y, x);
    if (!win) return;
    box(win, 0, 0);
    wrefresh(win);
    delwin(win);
}

TUI_API void tui_print(int x,int y,int attr, const char* fmt, ...){
    va_list ap; va_start(ap,fmt);
    char buf[1024];
    vsnprintf(buf,sizeof buf,fmt,ap);
    va_end(ap);
    if (attr) attron(attr);
    mvaddnstr(y,x,buf, (int)strlen(buf));
    if (attr) attroff(attr);
}

TUI_API void tui_progress(int x,int y,int w,double frac,const char* label){
    if (w<4) w=4;
    if (frac<0) frac=0; if (frac>1) frac=1;
    int fill = (int)((w-2)*frac + 0.5);
    mvaddch(y,x,'[');
    for (int i=0;i<w-2;i++) mvaddch(y,x+1+i, i<fill ? '=' : ' ');
    mvaddch(y,x+w-1,']');
    if (label) {
        int lx = x + w + 1;
        mvaddnstr(y,lx,label,(int)strlen(label));
    }
}

/* Status bar: une ligne en bas. */
TUI_API void tui_status(const char* text){
    if (!_tui_status_en) return;
    int r = LINES-1;
    int w = COLS;
    attr_t old; short pair; wattr_get(stdscr,&old,&pair,NULL);
    short p = tui_pair(COLOR_BLACK, COLOR_CYAN);
    attron(COLOR_PAIR(p));
    mvhline(r,0,' ',w);
    if (text) mvaddnstr(r,1,text,(int)(w-2));
    attroff(COLOR_PAIR(p));
    /* Coin droit avec horloge simple: laissé vide pour neutralité */
}

/* ========================= Input ========================= */

TUI_API int tui_getch(void){
    int c = getch();
    if (c==ERR) return -1;
    if (c==KEY_RESIZE){
        clear(); refresh(); /* réaligner */
        _tui_cols = COLS; _tui_rows = LINES;
        return KEY_RESIZE;
    }
    return c;
}

/* Lecture de ligne simple avec édition basique. Bloquante. */
TUI_API int tui_readline(const char* prompt, char* out, size_t cap){
    if (!out || cap==0) return -1;
    echo(); curs_set(1); nodelay(stdscr, FALSE);
    move(LINES-2,0); clrtoeol();
    if (prompt) addstr(prompt);
    int y=LINES-2, x=(int)strlen(prompt?prompt:"");
    move(y,x);
    int len=0;
    while (1){
        int c = getch();
        if (c=='\n' || c=='\r') break;
        if (c==KEY_BACKSPACE || c==127 || c==8){
            if (len>0){ len--; mvaddch(y,x+len,' '); move(y,x+len); }
            continue;
        }
        if (c==27){ len=0; break; } /* ESC -> annuler */
        if (c>=32 && c<127 && len<(int)cap-1){
            out[len++]=(char)c; addch(c);
        }
    }
    out[len]=0;
    noecho(); curs_set(0); nodelay(stdscr, TRUE);
    return len;
}

/* ========================= Sub-windows ========================= */

TUI_API int tui_win_make(tui_win* w, int x,int y,int wid,int hei){
    if (!w) return -1;
    w->w = newwin(hei,wid,y,x);
    if (!w->w) return -1;
    w->x=x; w->y=y; w->wid=wid; w->hei=hei;
    keypad(w->w, TRUE); nodelay(w->w, TRUE);
    box(w->w,0,0); wrefresh(w->w);
    return 0;
}
TUI_API void tui_win_free(tui_win* w){
    if (!w || !w->w) return;
    delwin(w->w); w->w=NULL;
}
TUI_API void tui_win_clear(tui_win* w){ if (w&&w->w){ werase(w->w); box(w->w,0,0); wrefresh(w->w);} }
TUI_API void tui_win_print(tui_win* w,int x,int y,int attr,const char* fmt,...){
    if (!w||!w->w) return;
    va_list ap; va_start(ap,fmt); char buf[1024]; vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    if (attr) wattron(w->w,attr);
    mvwaddnstr(w->w,y,x,buf,(int)strlen(buf));
    if (attr) wattroff(w->w,attr);
    wrefresh(w->w);
}

/* ========================= Flush ========================= */

TUI_API void tui_refresh(void){ refresh(); }

/* ========================= Test ========================= */
#ifdef TUI_TEST
#include <time.h>
static void msleep(int ms){
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts={ms/1000,(ms%1000)*1000000L}; nanosleep(&ts,NULL);
#endif
}
int main(void){
    if (tui_init()!=0){ fprintf(stderr,"init fail\n"); return 1; }

    short p_ok = tui_pair(COLOR_GREEN, -1);
    short p_warn = tui_pair(COLOR_YELLOW, -1);

    tui_clear();
    tui_box(0,0,COLS,3);
    tui_print(2,1, A_BOLD | COLOR_PAIR(p_ok), "Vitte Light — Demo ncurses");
    tui_status("F1:Quit  F2:Input  Any:progress");

    tui_win logw; tui_win_make(&logw, 2,4, COLS-4, LINES-8);
    tui_win_print(&logw,2,1, COLOR_PAIR(p_warn), "Logs:");

    double frac=0.0;
    int running=1;
    while (running){
        tui_progress(2,3,COLS-4,frac,"chargement");
        tui_refresh();
        int c = tui_getch();
        if (c==KEY_RESIZE){
            tui_clear(); tui_box(0,0,COLS,3);
            tui_status("Resized");
            tui_win_free(&logw); tui_win_make(&logw,2,4,COLS-4,LINES-8);
        } else if (c==KEY_F(1) || c=='q'){
            running=0;
        } else if (c==KEY_F(2)){
            char line[128];
            int n = tui_readline("Entrée: ", line, sizeof line);
            tui_win_print(&logw,2,2,0,"Input len=%d: %s", n, line);
        } else if (c!=-1){
            tui_win_print(&logw,2,3,0,"Key=%d", c);
        }
        frac += 0.02; if (frac>1.0) frac=0.0;
        msleep(50);
    }

    tui_end();
    return 0;
}
#endif