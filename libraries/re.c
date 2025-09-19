// SPDX-License-Identifier: GPL-3.0-or-later
//
// re.c — Mini-moteur d’expressions régulières portable (C17, sans dépendances)
// Namespace: "re"
//
// Syntaxe prise en charge (compat Rob Pike + extensions):
//   .        : n’importe quel caractère (sauf '\0')
//   ^ / $    : ancrages début/fin
//   c        : caractère littéral (échapper avec \ si besoin)
//   \x       : échappe x (., [, ], ^, \, *, +, ?)
//   [abc]    : classe de caractères
//   [a-z]    : intervalle
//   [^abc]   : classe négative (au début de la classe)
//   a* a+ a? : quantificateurs 0+ / 1+ / 0-1 appliqués à l’atome précédent
//
// Limitations:
//   - Pas de parenthèses ni d’alternation (|) ni de backrefs.
//   - Pas de classes POSIX ([:alpha:]).
//   - ASCII par défaut; option insensible à la casse.
//
// API:
//   re_pat      : handle de motif compilé (stocke motif + options)
//   RE_ICASE    : flag insensible à la casse
//   re_compile(r, pattern, flags) -> 0/-1
//   re_search(r, text)            -> 1/0 (trouve quelque part)
//   re_match_full(r, text)        -> 1/0 (équivalent à ^pat$)
//   re_match_prefix(r, text)      -> 1/0 (match à partir du début)
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c re.c
//
// Test (RE_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DRE_TEST re.c && ./a.out

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef RE_API
#define RE_API
#endif

enum { RE_ICASE = 1 };

typedef struct {
    char* pat;
    int   flags;
} re_pat;

/* ====================== util ====================== */

static inline int _eq(int a, int b, int icase){
    if (!icase) return a==b;
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

/* Avance sur un atome à partir de p et teste si le caractère c le matche.
   Sorties:
     *pend    : position après l’atome
     *matched : 1 si c correspond à l’atome, 0 sinon
   Retourne 0 si ok, -1 si motif invalide (classe sans ']') */
static int _atom_match(const char* p, int icase, int c, const char** pend, int* matched){
    *matched = 0;
    if (*p=='.'){
        *pend = p+1;
        *matched = (c!='\0');
        return 0;
    }
    if (*p=='\\'){
        if (p[1]=='\0'){ *pend=p+1; *matched=0; return 0; }
        *pend = p+2;
        *matched = _eq((unsigned char)p[1], c, icase);
        return 0;
    }
    if (*p=='['){
        int neg=0; int ok=0;
        const char* q = p+1;
        if (*q=='^'){ neg=1; q++; }
        if (*q==']') { /* ']' en premier est littéral */
            ok = (c==']'); q++;
        }
        while (*q && *q!=']'){
            if (q[0]=='\\' && q[1]){ /* échappé dans classe */
                int ch = (unsigned char)q[1];
                if (_eq(ch, c, icase)) ok=1;
                q+=2;
            } else if (q[1]=='-' && q[2] && q[2]!=']'){
                int a=(unsigned char)q[0], b=(unsigned char)q[2];
                if (icase){ a=tolower(a); b=tolower(b); int cc=tolower((unsigned char)c); if (a<=cc && cc<=b) ok=1; }
                else { if (a<=(unsigned char)c && (unsigned char)c<=b) ok=1; }
                q+=3;
            } else {
                if (_eq((unsigned char)q[0], c, icase)) ok=1;
                q++;
            }
        }
        if (*q!=']') return -1; /* pas de fin de classe */
        *pend = q+1;
        *matched = neg? !ok : ok;
        return 0;
    }
    /* littéral simple */
    *pend = p+1;
    *matched = _eq((unsigned char)*p, c, icase);
    return 0;
}

/* ====================== cœur du moteur ====================== */

static int _match_here(const char* pat, const char* text, int icase);

static int _match_qmark(const char* atom, const char* after_atom, const char* rest, const char* text, int icase){
    int m=0; const char* end=NULL;
    if (_atom_match(atom, icase, (unsigned char)*text, &end, &m)!=0) return 0;
    if (m && _match_here(rest, text+1, icase)) return 1;
    return _match_here(rest, text, icase);
}

static int _match_plus(const char* atom, const char* after_atom, const char* rest, const char* text, int icase){
    /* au moins une occurrence */
    int m=0; const char* end=NULL;
    if (_atom_match(atom, icase, (unsigned char)*text, &end, &m)!=0) return 0;
    if (!m) return 0;
    /* consommer une puis passer à star-like */
    const char* t = text+1;
    /* greedily consume */
    int mm=1;
    while (mm){
        int tm=0; if (_atom_match(atom, icase, (unsigned char)*t, &end, &tm)!=0) break;
        if (!tm) break; t++;
    }
    /* backtrack */
    while (t>=text+1){
        if (_match_here(rest, t, icase)) return 1;
        t--;
    }
    return 0;
}

static int _match_star(const char* atom, const char* after_atom, const char* rest, const char* text, int icase){
    /* consommer le plus possible puis backtrack */
    const char* t = text;
    while (1){
        int m=0; const char* end=NULL;
        if (_atom_match(atom, icase, (unsigned char)*t, &end, &m)!=0) return 0;
        if (!m) break;
        t++;
    }
    do {
        if (_match_here(rest, t, icase)) return 1;
    } while (t-- > text);
    return 0;
}

/* Retourne vrai si pat matche à partir de text */
static int _match_here(const char* pat, const char* text, int icase){
    /* fin de motif -> succès */
    if (*pat=='\0') return 1;

    /* $ en fin */
    if (pat[0]=='$' && pat[1]=='\0') return *text=='\0';

    /* quantificateurs: on doit regarder atome + suffixe (*,+,?) */
    const char* after_atom = pat;
    int dummy=0;
    if (pat[0] && pat[0]!='$'){
        /* lire atome pour savoir sa longueur */
        if (_atom_match(pat, icase, (unsigned char)*text, &after_atom, &dummy)!=0) return 0;
        char q = *after_atom;
        if (q=='*'){
            const char* rest = after_atom+1;
            return _match_star(pat, after_atom, rest, text, icase);
        } else if (q=='+'){
            const char* rest = after_atom+1;
            return _match_plus(pat, after_atom, rest, text, icase);
        } else if (q=='?'){
            const char* rest = after_atom+1;
            return _match_qmark(pat, after_atom, rest, text, icase);
        }
        /* pas de quantificateur: consommer 1 si possible */
        int m=0; const char* end=NULL;
        if (_atom_match(pat, icase, (unsigned char)*text, &end, &m)!=0) return 0;
        if (!m) return 0;
        return _match_here(after_atom, text+1, icase);
    }

    /* Cas improbable: pat commence par $ géré plus haut, ou vide */
    return 0;
}

/* ====================== Interface ====================== */

RE_API int re_compile(re_pat* r, const char* pattern, int flags){
    if (!r || !pattern) return -1;
    size_t n = strlen(pattern);
    r->pat = (char*)malloc(n+1);
    if (!r->pat) return -1;
    memcpy(r->pat, pattern, n+1);
    r->flags = flags;
    return 0;
}

RE_API void re_free(re_pat* r){
    if (!r) return;
    free(r->pat); r->pat=NULL; r->flags=0;
}

/* match depuis le début (comme ^pat) */
RE_API int re_match_prefix(const re_pat* r, const char* text){
    if (!r||!r->pat||!text) return 0;
    const char* pat = r->pat;
    int icase = (r->flags & RE_ICASE) ? 1 : 0;

    if (*pat=='^') pat++;          /* ancre explicite supportée */
    return _match_here(pat, text, icase);
}

/* match sur tout le texte (équivalent ^pat$) */
RE_API int re_match_full(const re_pat* r, const char* text){
    if (!r||!r->pat||!text) return 0;
    const char* pat = r->pat;
    char buf[1024];
    size_t lp = strlen(pat);
    if (lp+3 < sizeof buf){
        /* composer ^pat$ si pas déjà ancré */
        int has_begin = (lp>0 && pat[0]=='^');
        int has_end   = (lp>0 && pat[lp-1]=='$');
        if (!has_begin || !has_end){
            size_t i=0;
            if (!has_begin) buf[i++]='^';
            memcpy(buf+i, pat, lp); i+=lp;
            if (!has_end) buf[i++]='$';
            buf[i]=0;
            re_pat tmp={ (char*)buf, r->flags };
            return re_match_full(&tmp, text); /* récursif mais borne */
        }
    }
    /* déjà ancré dans le motif */
    const char* p = pat;
    int icase = (r->flags & RE_ICASE) ? 1 : 0;
    if (*p=='^') p++;
    /* on exige que le moteur consomme jusqu’à '\0' grâce à $ dans le motif */
    return _match_here(p, text, icase);
}

/* recherche quelque part (comme grep) */
RE_API int re_search(const re_pat* r, const char* text){
    if (!r||!r->pat||!text) return 0;
    const char* pat = r->pat;
    int icase = (r->flags & RE_ICASE) ? 1 : 0;
    if (*pat=='^') return _match_here(pat+1, text, icase);
    for (const char* t=text; ; ++t){
        if (_match_here(pat, t, icase)) return 1;
        if (*t=='\0') break;
    }
    return 0;
}

/* ====================== Test ====================== */
#ifdef RE_TEST
#include <stdio.h>
static void T(const char* pat, const char* s, int flags, int exp, const char* tag){
    re_pat r; re_compile(&r, pat, flags);
    int got = re_search(&r, s);
    printf("[%s] /%s/ %s -> %d\n", tag, pat, s, got);
    if (got!=exp){ fprintf(stderr,"FAIL %s\n", tag); exit(1); }
    re_free(&r);
}
int main(void){
    T("a.c",  "abc", 0, 1, "dot");
    T("^ab",  "zab", 0, 0, "anchor");
    T("^ab",  "ab",  0, 1, "anchor2");
    T("ab$",  "xxab",0, 1, "end");
    T("a*b",  "aaab",0, 1, "star");
    T("a+b",  "b",   0, 0, "plus0");
    T("a+b",  "aaab",0, 1, "plus");
    T("ab?c", "ac",  0, 1, "q0");
    T("ab?c", "abc", 0, 1, "q1");
    T("[a-c]+", "abcc", 0, 1, "range");
    T("[^0-9]+", "abc!", 0, 1, "neg");
    T("x\\+y", "x+y", 0, 1, "escape");
    T("[\\]\\[]", "]", 0, 1, "class-esc1");
    T("[\\]\\[]", "[", 0, 1, "class-esc2");
    /* icase */
    re_pat r; re_compile(&r, "Hello", RE_ICASE); printf("icase=%d\n", re_search(&r,"heLLo")); re_free(&r);
    puts("ok");
    return 0;
}
#endif