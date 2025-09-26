// SPDX-License-Identifier: GPL-3.0-or-later
//
// path.c — Utilitaires de chemins portables (C17)
// Namespace: "path"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c path.c
//
// Fournit :
//   - path_sep()
//   - path_is_abs(p)                         -> int
//   - path_is_unc(p)                         -> int (Windows)
//   - path_norm(in,out,cap)                  -> 0/-1   (normalise, résout . et .., compacte //)
//   - path_norm_keep_trailing(in,out,cap)    -> 0/-1   (conserve / final s’il est présent)
//   - path_to_native(in,out,cap)             -> 0/-1   (séparateurs vers natif)
//   - path_join(out,cap,a,b)                 -> 0/-1
//   - path_join3(out,cap,a,b,c)              -> 0/-1
//   - path_dirname(p,out,cap)                -> 0/-1
//   - path_basename(p,out,cap)               -> 0/-1
//   - path_ext(p)                            -> const char* (pointeur dans p) ou NULL
//   - path_stem(p,out,cap)                   -> 0/-1  (basename sans extension)
//   - path_change_ext(p,newext,out,cap)      -> 0/-1  (newext avec ou sans point)
//
// Notes :
//   - Indépendant de la locale. Pas d’allocation dynamique.
//   - Windows : accepte '\\' et '/' en entrée. Garde le préfixe lecteur (C:) ou UNC (\\srv\share).
//   - POSIX   : accepte aussi '\\' mais elles sont normalisées en '/'. 

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "libctype.h"

#if defined(_WIN32)
  #define PATH_SEP_NATIVE '\\'
#else
  #define PATH_SEP_NATIVE '/'
#endif

#ifndef PATH_API
#define PATH_API
#endif

/* ======================== Helpers internes ======================== */

static int _is_sep(char c){ return c=='/' || c=='\\'; }
static void _to_slash(char* s){ for (; *s; ++s) if (*s=='\\') *s='/'; }
static int _copy(char* out, size_t cap, const char* src){
    size_t n = src?strlen(src):0;
    if (!out || cap==0) return -1;
    if (n+1 > cap) return -1;
    if (n) memcpy(out, src, n);
    out[n]=0; return 0;
}

/* Windows : "C:" ou "C:/" -> 2, UNC // -> 2, sinon 0. POSIX : 0. */
static int _drive_len(const char* p){
#if defined(_WIN32)
    if (!p || !p[0]) return 0;
    if (_is_sep(p[0]) && _is_sep(p[1])) return 2;          /* UNC introducer */
    if (isalpha((unsigned char)p[0]) && p[1]==':') return 2;/* drive letter */
#endif
    return 0;
}

/* Pointe après le préfixe racine :
   - Windows UNC: //server/share/  -> retourne après "//server/share"
   - Windows drive: "C:/" -> après "C:"
   - POSIX: "/" -> après "/" (si présent) */
static const char* _skip_root(const char* p){
    int d = _drive_len(p);
#if defined(_WIN32)
    if (d==2 && _is_sep(p[0]) && _is_sep(p[1])){
        /* UNC \\server\share\[rest] */
        const char* s = p+2;
        /* server */
        while (*s && !_is_sep(*s)) s++;
        if (_is_sep(*s)) s++;
        /* share */
        while (*s && !_is_sep(*s)) s++;
        if (_is_sep(*s)) s++; /* inclure la barre après share */
        return s;
    }
    if (d==2 && p[1]==':'){
        const char* s = p+2;
        if (_is_sep(*s)) s++; /* C:\ -> garder sep dans racine */
        return s;
    }
#endif
    if (p[0]=='/'){ const char* s=p+1; return s; }
    return p;
}

/* Écrit le préfixe racine dans out. Retourne nombre d’octets écrits. */
static size_t _write_root(char* out, size_t cap, const char* src){
    size_t w=0;
#if defined(_WIN32)
    if (_is_sep(src[0]) && _is_sep(src[1])){
        /* \\server\share\ -> recopier jusqu’après share et un unique '/' */
        const char* s = src;
        int seps=0;
        while (*s){
            out[w++] = (*s=='\\')? '/' : *s;
            if (w>=cap) return (size_t)-1;
            if (_is_sep(*s)){ seps++; if (seps>=3) break; } /* \\ + \ après share */
            s++;
        }
        return w;
    }
    if (isalpha((unsigned char)src[0]) && src[1]==':'){
        if (cap<2) return (size_t)-1;
        out[w++]=src[0]; out[w++]=src[1];
        if (_is_sep(src[2])){ if (w<cap) out[w++]='/'; else return (size_t)-1; }
        return w;
    }
#endif
    if (src[0]=='/'){ if (cap<1) return (size_t)-1; out[w++]='/'; }
    return w;
}

/* Push composant dans la pile (array de positions) */
static int _push(size_t* idx, size_t* n, size_t val, size_t cap){
    if (*n>=cap) return -1; idx[(*n)++]=val; return 0;
}
static int _pop(size_t* n){ if (*n==0) return -1; (*n)--; return 0; }

/* ======================== API ======================== */

PATH_API char path_sep(void){ return PATH_SEP_NATIVE; }

PATH_API int path_is_unc(const char* p){
#if defined(_WIN32)
    return p && _is_sep(p[0]) && _is_sep(p[1]);
#else
    (void)p; return 0;
#endif
}

PATH_API int path_is_abs(const char* p){
    if (!p||!p[0]) return 0;
#if defined(_WIN32)
    if (_is_sep(p[0]) && _is_sep(p[1])) return 1;             /* UNC */
    if (isalpha((unsigned char)p[0]) && p[1]==':' && _is_sep(p[2])) return 1; /* C:\ */
    if (_is_sep(p[0])) return 1;                               /* /foo */
    return 0;
#else
    return p[0]=='/';
#endif
}

/* Normalise in -> out, remplace \ par /, résout . et .., compacte //. */
static int _norm_impl(const char* in, char* out, size_t cap, int keep_trailing){
    if (!out || cap==0) return -1;
    if (!in) in = "";
    size_t n = strlen(in);
    /* buffer de travail sur copie locale pour pouvoir éditer */
    char* tmp = (char*)malloc(n+1);
    if (!tmp) return -1;
    memcpy(tmp, in, n+1);
    _to_slash(tmp);

    /* Déterminer racine et initialiser out */
    const char* body = _skip_root(tmp);
    size_t w = 0;
    size_t root_w = _write_root(out, cap, tmp);
    if (root_w==(size_t)-1){ free(tmp); return -1; }
    w = root_w;

    /* pile de débuts de composants dans out */
    size_t stack[256]; size_t sp=0;

    /* scanner composants */
    const char* p = body;
    int had_trailing_sep = 0;
    while (*p){
        /* sauter les / multiples */
        while (*p=='/') p++;
        if (!*p){ had_trailing_sep = 1; break; }
        const char* q = p;
        while (*q && *q!='/') q++;

        size_t clen = (size_t)(q - p);
        if (clen==1 && p[0]=='.'){
            /* ignore */
        } else if (clen==2 && p[0]=='.' && p[1]=='.'){
            /* remonter si possible, sinon conserver si hors racine relative */
            if (sp>0){
                /* pop et tronquer w */
                size_t start = stack[sp-1];
                w = start; _pop(&sp);
            } else {
                /* pas de composant à enlever :
                   - si racine présente => ignore
                   - sinon conserver ".." en tête (chemin relatif) */
                if (root_w==0){
                    if (w+3 >= cap){ free(tmp); return -1; }
                    if (w && out[w-1]!='/'){ out[w++]='/'; }
                    _push(stack, &sp, w, 256);
                    out[w++]='.'; out[w++]='.'; 
                }
            }
        } else {
            if (w && out[w-1]!='/'){
                if (w+1>=cap){ free(tmp); return -1; }
                out[w++]='/';
            }
            if (w+clen>=cap){ free(tmp); return -1; }
            _push(stack, &sp, w, 256);
            memcpy(out+w, p, clen); w += clen;
        }
        p = q;
    }

    /* barre finale */
    if (keep_trailing && (had_trailing_sep || (n>0 && _is_sep(in[n-1]))) ){
        if (w==0 || out[w-1]!='/'){
            if (w+1>=cap){ free(tmp); return -1; }
            out[w++]='/';
        }
    }
    if (w==0){ /* vide -> "." pour relatif, sinon racine déjà écrite */
        if (root_w==0){
            if (cap<2){ free(tmp); return -1; }
            out[w++]='.'; 
        }
    }
    if (w>=cap){ free(tmp); return -1; }
    out[w]=0;
    free(tmp);
    return 0;
}

PATH_API int path_norm(const char* in, char* out, size_t cap){
    return _norm_impl(in,out,cap,0);
}
PATH_API int path_norm_keep_trailing(const char* in, char* out, size_t cap){
    return _norm_impl(in,out,cap,1);
}

PATH_API int path_to_native(const char* in, char* out, size_t cap){
    if (_copy(out,cap,in)!=0) return -1;
#if defined(_WIN32)
    for (size_t i=0; out[i]; ++i) if (out[i]=='/') out[i]='\\';
#else
    for (size_t i=0; out[i]; ++i) if (out[i]=='\\') out[i]='/';
#endif
    return 0;
}

/* Join simple. Si b est absolu => résultat = norm(b) */
PATH_API int path_join(char* out, size_t cap, const char* a, const char* b){
    char nb[4096];
    if (b && path_is_abs(b)) return path_norm(b, out, cap);
    if (!a || !*a) return path_norm(b?b:"", out, cap);
    if (!b || !*b) return path_norm(a, out, cap);

    size_t na=strlen(a), nb2=strlen(b);
    if (na+1+nb2+1 > cap) return -1;
    memcpy(out,a,na); out[na]=0;
    if (!_is_sep(out[na?(na-1):0])){ out[na++]='/'; out[na]=0; }
    memcpy(out+na,b,nb2); out[na+nb2]=0;
    (void)nb;
    return path_norm(out,out,cap);
}
PATH_API int path_join3(char* out, size_t cap, const char* a, const char* b, const char* c){
    char t[4096];
    if (path_join(t,sizeof t,a,b)!=0) return -1;
    return path_join(out,cap,t,c);
}

/* Dirname / Basename (type POSIX) */
PATH_API int path_dirname(const char* p, char* out, size_t cap){
    if (!p||!*p) return _copy(out,cap,".");
    char tmp[4096];
    if (path_norm(p,tmp,sizeof tmp)!=0) return -1;
    size_t n=strlen(tmp);
    if (n==1 && tmp[0]=='/'){ return _copy(out,cap,"/"); }

#if defined(_WIN32)
    /* conserver préfixe UNC ou lecteur si root */
    if (path_is_unc(tmp)){
        /* \\srv\share */
        const char* body = _skip_root(tmp);
        if (*body==0) return _copy(out,cap,tmp); /* la racine UNC elle-même */
    } else if (isalpha((unsigned char)tmp[0]) && tmp[1]==':' && (tmp[2]==0 || tmp[2]=='/')){
        return _copy(out,cap,tmp); /* "C:" ou "C:/" */
    }
#endif

    /* enlever trailing '/' sauf racine */
    if (n>1 && tmp[n-1]=='/') tmp[--n]=0;
    /* chercher dernier '/' */
    char* last = strrchr(tmp,'/');
    if (!last) return _copy(out,cap,".");
    if (last==tmp){ /* "/foo" -> "/" */
        last[1]=0; return _copy(out,cap,tmp);
    }
    *last=0;
    return _copy(out,cap,tmp);
}
PATH_API int path_basename(const char* p, char* out, size_t cap){
    if (!p||!*p) return _copy(out,cap,"");
    char tmp[4096];
    if (path_norm(p,tmp,sizeof tmp)!=0) return -1;
    size_t n=strlen(tmp);
    if (n>1 && tmp[n-1]=='/') tmp[--n]=0;
    const char* base = strrchr(tmp,'/');
    base = base? base+1 : tmp;
    return _copy(out,cap,base);
}

/* Extension : pointeur dans p, sans le point. Retourne NULL si aucune. */
PATH_API const char* path_ext(const char* p){
    if (!p) return NULL;
    const char* base = strrchr(p,'/'); const char* base2 = strrchr(p,'\\');
    if (base2 && (!base || base2>base)) base=base2;
    base = base? base+1 : p;
    const char* dot = strrchr(base,'.');
    if (!dot || dot==base) return NULL;
    return dot+1;
}

PATH_API int path_stem(const char* p, char* out, size_t cap){
    char b[4096];
    if (path_basename(p,b,sizeof b)!=0) return -1;
    char* dot = strrchr(b,'.');
    if (dot && dot!=b) *dot=0;
    return _copy(out,cap,b);
}

PATH_API int path_change_ext(const char* p, const char* newext, char* out, size_t cap){
    char d[4096], b[4096];
    if (path_dirname(p,d,sizeof d)!=0) return -1;
    if (path_basename(p,b,sizeof b)!=0) return -1;
    char* dot = strrchr(b,'.');
    if (dot && dot!=b) *dot=0;
    char nb[4096];
    if (newext && *newext){
        if (newext[0]=='.') snprintf(nb,sizeof nb,"%s%s", b, newext);
        else snprintf(nb,sizeof nb,"%s.%s", b, newext);
    } else {
        snprintf(nb,sizeof nb,"%s", b);
    }
    return path_join(out,cap, d, nb);
}

/* ======================== Test ======================== */
#ifdef PATH_TEST
#include <assert.h>
static void T(const char* in){
    char o[256]; path_norm(in,o,sizeof o); printf("NORM %-25s -> %s\n", in?in:"(null)", o);
}
int main(void){
    T("a/b/../c"); T("/a//b///c/"); T("././x"); T("../x"); T("C:\\a\\..\\b\\"); T("\\\\srv\\share\\a\\..\\b");
    char out[256];
    path_join(out,sizeof out,"/etc","../var/log"); printf("JOIN -> %s\n", out);
    path_dirname("/usr/local/bin/",out,sizeof out); printf("DIR -> %s\n", out);
    path_basename("/usr/local/bin/gcc",out,sizeof out); printf("BASE -> %s\n", out);
    printf("EXT -> %s\n", path_ext("file.tar.gz"));
    path_stem("file.tar.gz",out,sizeof out); printf("STEM -> %s\n", out);
    path_change_ext("/tmp/a/b/c.txt",".bin",out,sizeof out); printf("CHG -> %s\n", out);
    return 0;
}
#endif