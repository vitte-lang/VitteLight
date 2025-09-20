// SPDX-License-Identifier: GPL-3.0-or-later
//
// tablib.c — Tableaux texte simples: CSV/TSV, sélection, tri, impression (C17, portable)
// Namespace: "tab"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c tablib.c
//
// Test (TAB_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DTAB_TEST tablib.c && ./a.out

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef TAB_API
#define TAB_API
#endif

typedef struct {
    char** names;      /* ncol */
    size_t ncol, capcol;

    char*** rows;      /* nrow x ncol (cells) */
    size_t nrow, caprow;
} tab_table;

/* ====================== helpers ====================== */

static char* _strdup0(const char* s){
    if (!s) { char* z=(char*)malloc(1); if(z) z[0]=0; return z; }
    size_t n=strlen(s); char* d=(char*)malloc(n+1); if(!d) return NULL; memcpy(d,s,n+1); return d;
}
static void _free_row(char** row, size_t ncol){
    if (!row) return; for (size_t i=0;i<ncol;i++) free(row[i]); free(row);
}
static int _ensure_cols(tab_table* t, size_t need){
    if (need<=t->capcol) return 0;
    size_t nc = t->capcol? t->capcol:4; while (nc<need) nc*=2;
    char** nn = (char**)realloc(t->names, nc*sizeof *nn); if(!nn) return -1;
    t->names = nn;
    /* extend existing rows with empty cells */
    for (size_t r=0;r<t->nrow;r++){
        char** nr = (char**)realloc(t->rows[r], nc*sizeof *nr); if(!nr) return -1;
        for (size_t c=t->capcol; c<nc; c++) nr[c] = _strdup0("");
        t->rows[r] = nr;
    }
    t->capcol = nc;
    return 0;
}
static int _ensure_rows(tab_table* t, size_t need){
    if (need<=t->caprow) return 0;
    size_t nr = t->caprow? t->caprow:8; while (nr<need) nr*=2;
    char*** nrows = (char***)realloc(t->rows, nr*sizeof *nrows); if(!nrows) return -1;
    t->rows = nrows; t->caprow = nr; return 0;
}

/* ====================== API ====================== */

TAB_API void tab_init(tab_table* t, size_t cols){
    if (!t) return; memset(t,0,sizeof *t);
    if (cols){
        t->names = (char**)calloc(cols, sizeof *t->names);
        t->rows  = NULL;
        t->ncol = 0; t->capcol = cols;
    }
}
TAB_API void tab_free(tab_table* t){
    if (!t) return;
    for (size_t i=0;i<t->ncol;i++) free(t->names[i]);
    free(t->names);
    for (size_t r=0;r<t->nrow;r++) _free_row(t->rows[r], t->ncol);
    free(t->rows);
    memset(t,0,sizeof *t);
}
TAB_API void tab_clear_rows(tab_table* t){
    if (!t) return;
    for (size_t r=0;r<t->nrow;r++) _free_row(t->rows[r], t->ncol);
    free(t->rows); t->rows=NULL; t->nrow=0; t->caprow=0;
}

TAB_API int tab_add_col(tab_table* t, const char* name){
    if (!t) return -1;
    if (_ensure_cols(t, t->ncol+1)!=0) return -1;
    t->names[t->ncol] = _strdup0(name?name:"");
    if (!t->names[t->ncol]) return -1;
    return (int)(t->ncol++);
}
TAB_API int tab_col_index(const tab_table* t, const char* name){
    if (!t||!name) return -1;
    for (size_t i=0;i<t->ncol;i++) if (strcmp(t->names[i]?t->names[i]:"", name)==0) return (int)i;
    return -1;
}

TAB_API int tab_add_row(tab_table* t){
    if (!t) return -1;
    if (_ensure_rows(t, t->nrow+1)!=0) return -1;
    char** row = (char**)malloc((t->capcol?t->capcol:(t->ncol?t->ncol:1)) * sizeof *row);
    if (!row) return -1;
    if (t->capcol==0 && t->ncol>0){
        if (_ensure_cols(t, t->ncol)!=0){ free(row); return -1; }
    }
    for (size_t c=0;c<t->ncol;c++) row[c]=_strdup0("");
    for (size_t c=t->ncol;c<t->capcol;c++) row[c]=_strdup0("");
    t->rows[t->nrow] = row;
    return (int)(t->nrow++);
}

TAB_API int tab_set(tab_table* t, size_t r, size_t c, const char* val){
    if (!t || r>=t->nrow) return -1;
    if (c>=t->ncol){
        if (_ensure_cols(t, c+1)!=0) return -1;
        t->ncol = c+1;
    }
    char** row = t->rows[r];
    char* nv = _strdup0(val?val:"");
    if (!nv) return -1;
    free(row[c]); row[c]=nv;
    return 0;
}
TAB_API const char* tab_get(const tab_table* t, size_t r, size_t c){
    if (!t || r>=t->nrow || c>=t->ncol) return NULL;
    return t->rows[r][c];
}

/* ====================== CSV/TSV read/write ====================== */

static int _csv_flush_cell(tab_table* t, size_t* prow_idx, size_t* pcur_c,
                           const char* buf, size_t n){
    if (*prow_idx==SIZE_MAX){
        int r = tab_add_row(t);
        if (r<0) return -1;
        *prow_idx = (size_t)r;
        *pcur_c = 0;
    }
    if (t->ncol==0){
        if (tab_add_col(t,"")<0) return -1;
    }
    if (*pcur_c>=t->ncol){
        if (tab_add_col(t,"")<0) return -1;
    }
    char* s=(char*)malloc(n+1); if(!s) return -1;
    memcpy(s,buf,n); s[n]=0;
    free(t->rows[*prow_idx][*pcur_c]);
    t->rows[*prow_idx][*pcur_c]=s;
    (*pcur_c)++;
    return 0;
}
static void _csv_end_row(size_t* prow_idx){ *prow_idx = SIZE_MAX; }

static int _read_csv_core(tab_table* t, FILE* f, int sep, int allow_crlf){
    if (!t||!f) return -1;
    enum { INIT=0, FIELD, QUOTED, QUOTED_ESC } st = INIT;
    size_t cap=1024, n=0; char* buf=(char*)malloc(cap); if(!buf) return -1;
    size_t row_idx = SIZE_MAX;
    size_t cur_c = 0;

    int ch;
    while ((ch=fgetc(f))!=EOF){
        if (st==INIT){
            n=0;
            if (ch=='\"'){ st=QUOTED; continue; }
            if (ch==sep){
                if (_csv_flush_cell(t,&row_idx,&cur_c,buf,n)!=0){ free(buf); return -1; }
                st=INIT; continue;
            }
            if (ch=='\n' || (allow_crlf && ch=='\r')){
                if (_csv_flush_cell(t,&row_idx,&cur_c,buf,n)!=0){ free(buf); return -1; }
                if (allow_crlf && ch=='\r'){
                    int c2 = fgetc(f);
                    if (c2!='\n' && c2!=EOF) ungetc(c2,f);
                }
                _csv_end_row(&row_idx);
                st=INIT; continue;
            }
            if (n+1>=cap){ cap*=2; char* nb=(char*)realloc(buf,cap); if(!nb){ free(buf); return -1; } buf=nb; }
            buf[n++]=(char)ch; st=FIELD; continue;
        }
        if (st==FIELD){
            if (ch==sep){
                if (_csv_flush_cell(t,&row_idx,&cur_c,buf,n)!=0){ free(buf); return -1; }
                n=0; st=INIT; continue;
            }
            if (ch=='\n' || (allow_crlf && ch=='\r')){
                if (_csv_flush_cell(t,&row_idx,&cur_c,buf,n)!=0){ free(buf); return -1; }
                if (allow_crlf && ch=='\r'){
                    int c2 = fgetc(f);
                    if (c2!='\n' && c2!=EOF) ungetc(c2,f);
                }
                _csv_end_row(&row_idx);
                n=0; st=INIT; continue;
            }
            if (n+1>=cap){ cap*=2; char* nb=(char*)realloc(buf,cap); if(!nb){ free(buf); return -1; } buf=nb; }
            buf[n++]=(char)ch; continue;
        }
        if (st==QUOTED){
            if (ch=='\"'){ st=QUOTED_ESC; continue; }
            if (n+1>=cap){ cap*=2; char* nb=(char*)realloc(buf,cap); if(!nb){ free(buf); return -1; } buf=nb; }
            buf[n++]=(char)ch; continue;
        }
        if (st==QUOTED_ESC){
            if (ch=='\"'){
                if (n+1>=cap){ cap*=2; char* nb=(char*)realloc(buf,cap); if(!nb){ free(buf); return -1; } buf=nb; }
                buf[n++]='\"'; st=QUOTED; continue;
            }
            if (ch==sep){
                if (_csv_flush_cell(t,&row_idx,&cur_c,buf,n)!=0){ free(buf); return -1; }
                n=0; st=INIT; continue;
            }
            if (ch=='\n' || (allow_crlf && ch=='\r')){
                if (_csv_flush_cell(t,&row_idx,&cur_c,buf,n)!=0){ free(buf); return -1; }
                if (allow_crlf && ch=='\r'){
                    int c2 = fgetc(f);
                    if (c2!='\n' && c2!=EOF) ungetc(c2,f);
                }
                _csv_end_row(&row_idx);
                n=0; st=INIT; continue;
            }
            if (n+1>=cap){ cap*=2; char* nb=(char*)realloc(buf,cap); if(!nb){ free(buf); return -1; } buf=nb; }
            buf[n++]=(char)ch; st=FIELD; continue;
        }
    }
    /* EOF: flush pending final cell/row */
    if (st==FIELD || st==QUOTED_ESC || st==INIT || st==QUOTED){
        if (_csv_flush_cell(t,&row_idx,&cur_c,buf,n)!=0){ free(buf); return -1; }
        _csv_end_row(&row_idx);
    }
    free(buf);
    return 0;
}

TAB_API int tab_read_csv(tab_table* t, FILE* f, int sep, int allow_crlf){
    if (sep==0) sep=',';
    return _read_csv_core(t,f,sep,allow_crlf?1:0);
}
TAB_API int tab_read_tsv(tab_table* t, FILE* f){
    return _read_csv_core(t,f,'\t',1);
}

static void _csv_write_cell(FILE* out, const char* s, int sep){
    int needq=0;
    for (const unsigned char* p=(const unsigned char*)s; *p; p++){
        if (*p==',' || *p=='\"' || *p=='\n' || *p=='\r' || (sep!='\0' && *p==(unsigned char)sep)){ needq=1; break; }
    }
    if (!needq){ fputs(s,out); return; }
    fputc('\"',out);
    for (const unsigned char* p=(const unsigned char*)s; *p; p++){
        if (*p=='\"') fputc('\"',out);
        fputc(*p,out);
    }
    fputc('\"',out);
}
TAB_API int tab_write_csv(const tab_table* t, FILE* out, int sep){
    if (!t||!out) return -1; if (sep==0) sep=',';
    for (size_t c=0;c<t->ncol;c++){
        if (c) fputc(sep,out);
        _csv_write_cell(out, t->names[c]?t->names[c]:"", sep);
    }
    fputc('\n',out);
    for (size_t r=0;r<t->nrow;r++){
        for (size_t c=0;c<t->ncol;c++){
            if (c) fputc(sep,out);
            _csv_write_cell(out, t->rows[r][c]?t->rows[r][c]:"", sep);
        }
        fputc('\n',out);
    }
    return 0;
}

/* ====================== Tri / Filtre / Print ====================== */

static int _cmp_cs(const void* a, const void* b, void* th){
    size_t col = *(size_t*)th;
    char* const* ra = *(char* const* const*)a;
    char* const* rb = *(char* const* const*)b;
    const char* sa = ra[col]?ra[col]:"";
    const char* sb = rb[col]?rb[col]:"";
    return strcmp(sa,sb);
}
static int _cmp_ci(const void* a, const void* b, void* th){
    size_t col = *(size_t*)th;
    char* const* ra = *(char* const* const*)a;
    char* const* rb = *(char* const* const*)b;
    const unsigned char* sa = (const unsigned char*)(ra[col]?ra[col]:"");
    const unsigned char* sb = (const unsigned char*)(rb[col]?rb[col]:"");
    while (*sa && *sb){
        int da = tolower(*sa), db = tolower(*sb);
        if (da!=db) return da-db;
        sa++; sb++;
    }
    return (int)(*sa) - (int)(*sb);
}

#if defined(__GNUC__) || defined(__clang__)
static void _qsort_r(void* base, size_t nmemb, size_t size,
                     int (*compar)(const void*, const void*, void*), void* th){
#if defined(__APPLE__)
    qsort_r(base, nmemb, size, th, compar);
#else
    qsort_r(base, nmemb, size, compar, th);
#endif
}
#else
static size_t _g_col; static int _g_ci; static int _g_desc;
static int _cmp_fallback(const void* a, const void* b){
    char* const* ra = *(char* const* const*)a;
    char* const* rb = *(char* const* const*)b;
    const char* sa = ra[_g_col]?ra[_g_col]:"";
    const char* sb = rb[_g_col]?rb[_g_col]:"";
    int r=0;
    if (_g_ci){
        const unsigned char* ua=(const unsigned char*)sa; const unsigned char* ub=(const unsigned char*)sb;
        while (*ua && *ub){ int da=tolower(*ua), db=tolower(*ub); if (da!=db){ r=da-db; goto done; } ua++; ub++; }
        r=(int)(*ua)-(int)(*ub);
    } else r=strcmp(sa,sb);
done:
    return _g_desc? -r : r;
}
#endif

TAB_API int tab_sort(tab_table* t, size_t col, int case_insensitive, int descending){
    if (!t || col>=t->ncol) return -1;
#if defined(__GNUC__) || defined(__clang__)
    size_t th = col;
    int (*cmp)(const void*,const void*,void*) = case_insensitive? _cmp_ci : _cmp_cs;
    _qsort_r(t->rows, t->nrow, sizeof *t->rows, cmp, &th);
    if (descending){
        for (size_t i=0;i<t->nrow/2;i++){ char** tmp=t->rows[i]; t->rows[i]=t->rows[t->nrow-1-i]; t->rows[t->nrow-1-i]=tmp; }
    }
#else
    _g_col=col; _g_ci=case_insensitive?1:0; _g_desc=descending?1:0;
    qsort(t->rows, t->nrow, sizeof *t->rows, _cmp_fallback);
#endif
    return 0;
}

/* Supprime en place les lignes pour lesquelles keep_cb==0. */
typedef int (*tab_keep_cb)(char* const* row, size_t ncol, void* u);
TAB_API size_t tab_filter_rows(tab_table* t, tab_keep_cb keep, void* u){
    if (!t||!keep) return 0;
    size_t w=0;
    for (size_t r=0;r<t->nrow;r++){
        if (keep((char* const*)t->rows[r], t->ncol, u)){
            if (w!=r) t->rows[w]=t->rows[r];
            w++;
        } else {
            _free_row(t->rows[r], t->ncol);
        }
    }
    t->nrow=w;
    return w;
}

/* Impression en colonnes. */
TAB_API void tab_print_cols(const tab_table* t, FILE* out, int padding){
    if (!t||!out) return; if (padding<1) padding=2;
    size_t* w = (size_t*)calloc(t->ncol? t->ncol:1, sizeof *w); if(!w) return;
    for (size_t c=0;c<t->ncol;c++){
        size_t m = t->names[c]?strlen(t->names[c]):0;
        for (size_t r=0;r<t->nrow;r++){
            size_t L = t->rows[r][c]?strlen(t->rows[r][c]):0;
            if (L>m) m=L;
        }
        w[c]=m;
    }
    for (size_t c=0;c<t->ncol;c++){
        const char* s = t->names[c]?t->names[c]:"";
        fprintf(out, "%-*s", (int)(w[c] + (c+1<t->ncol?padding:0)), s);
    }
    fputc('\n', out);
    for (size_t r=0;r<t->nrow;r++){
        for (size_t c=0;c<t->ncol;c++){
            const char* s = t->rows[r][c]?t->rows[r][c]:"";
            fprintf(out, "%-*s", (int)(w[c] + (c+1<t->ncol?padding:0)), s);
        }
        fputc('\n', out);
    }
    free(w);
}

/* ====================== Test ====================== */
#ifdef TAB_TEST
static int keep_nonempty_first(char* const* row, size_t ncol, void* u){
    (void)u; (void)ncol; return row[0] && row[0][0];
}
int main(void){
    tab_table t; tab_init(&t, 0);
    tab_add_col(&t, "Name");
    tab_add_col(&t, "Age");
    for (int i=0;i<3;i++){
        int r=tab_add_row(&t);
        char buf[16]; snprintf(buf,sizeof buf,"%d", 20+i);
        tab_set(&t,(size_t)r,0, i==1?"":(i==0?"Alice":"Bob"));
        tab_set(&t,(size_t)r,1, buf);
    }

    puts("Before:");
    tab_print_cols(&t, stdout, 2);

    tab_sort(&t, 0, 1, 0);
    puts("\nSorted by Name (CI):");
    tab_print_cols(&t, stdout, 2);

    tab_filter_rows(&t, keep_nonempty_first, NULL);
    puts("\nFiltered non-empty Name:");
    tab_print_cols(&t, stdout, 2);

    FILE* f=fopen("table.csv","wb"); tab_write_csv(&t,f,','); fclose(f);
    tab_clear_rows(&t);
    f=fopen("table.csv","rb"); tab_read_csv(&t,f,',',1); fclose(f);
    puts("\nReloaded CSV:");
    tab_print_cols(&t, stdout, 2);

    tab_free(&t);
    return 0;
}
#endif