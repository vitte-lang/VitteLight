// SPDX-License-Identifier: GPL-3.0-or-later
//
// strlib.c — Utilitaires chaînes “batteries incluses” (C17, portable)
// Namespace: "str"
//
// Fournit:
//   - Trim/lstrip/rstrip
//   - startswith/endswith (cs/ci), strstr_ci, icmp
//   - tolower/upper in-place, ASCII
//   - split/join (séparateur multi-char-set), free de tableau
//   - replace_all (littéral), collapse_spaces
//   - escape/unescape style C (\n, \t, \xNN)
//   - hex encode/decode
//   - base64 encode/decode
//   - StringBuilder: sb_* (init, append, appendn, appendf, data, len, clear, free)
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c strlib.c
//
// Test (STR_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DSTR_TEST strlib.c && ./a.out

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

#ifndef STR_API
#define STR_API
#endif

/* ===================== helpers ===================== */

static inline int _eq_ci(unsigned char a, unsigned char b){
    return (int)tolower(a) == (int)tolower(b);
}
static inline int _is_space(unsigned char c){
    return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f';
}
static inline int _issep(unsigned char c, const char* seps){
    for (const char* q=seps; *q; q++) if ((unsigned char)*q==c) return 1;
    return 0;
}
static inline int _hexval(int c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return 10+c-'a';
    if (c>='A'&&c<='F') return 10+c-'A';
    return -1;
}

/* ===================== Trim ===================== */

STR_API char* str_lstrip(char* s){
    if (!s) return NULL;
    char* p=s;
    while (*p && _is_space((unsigned char)*p)) p++;
    if (p!=s) memmove(s,p,(size_t)(strlen(p)+1));
    return s;
}
STR_API char* str_rstrip(char* s){
    if (!s) return NULL;
    size_t n=strlen(s);
    while (n && _is_space((unsigned char)s[n-1])) n--;
    s[n]=0;
    return s;
}
STR_API char* str_strip(char* s){ return str_rstrip(str_lstrip(s)); }

/* ===================== Predicates ===================== */

STR_API int str_startswith(const char* s, const char* pre){
    if (!s||!pre) return 0;
    size_t lp=strlen(pre);
    return strncmp(s,pre,lp)==0;
}
STR_API int str_startswith_ci(const char* s, const char* pre){
    if (!s||!pre) return 0;
    while (*pre){
        if (!*s || !_eq_ci((unsigned char)*s,(unsigned char)*pre)) return 0;
        s++; pre++;
    }
    return 1;
}
STR_API int str_endswith(const char* s, const char* suf){
    if (!s||!suf) return 0;
    size_t ls=strlen(s), lu=strlen(suf);
    if (lu>ls) return 0;
    return memcmp(s+ls-lu,suf,lu)==0;
}
STR_API int str_endswith_ci(const char* s, const char* suf){
    if (!s||!suf) return 0;
    size_t ls=strlen(s), lu=strlen(suf);
    if (lu>ls) return 0;
    for (size_t i=0;i<lu;i++)
        if (!_eq_ci((unsigned char)s[ls-lu+i], (unsigned char)suf[i])) return 0;
    return 1;
}
STR_API int str_icmp(const char* a, const char* b){
    if (!a||!b) return a?1:b?-1:0;
    while(*a && *b){
        int da=tolower((unsigned char)*a), db=tolower((unsigned char)*b);
        if (da!=db) return da-db;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
STR_API char* str_strstr_ci(const char* hay, const char* needle){
    if (!hay||!needle||!*needle) return (char*)hay;
    size_t ln=strlen(needle);
    for (const char* p=hay; *p; p++){
        size_t i=0;
        while (i<ln && p[i] && _eq_ci((unsigned char)p[i], (unsigned char)needle[i])) i++;
        if (i==ln) return (char*)p;
        if (!p[i]) break;
    }
    return NULL;
}

/* ===================== Case convert (ASCII) ===================== */

STR_API void str_tolower_inplace(char* s){ if(!s) return; for (;*s;s++) *s=(char)tolower((unsigned char)*s); }
STR_API void str_toupper_inplace(char* s){ if(!s) return; for (;*s;s++) *s=(char)toupper((unsigned char)*s); }

/* ===================== Split / Join ===================== */

typedef struct { char** v; size_t n; } str_vec;

STR_API void str_vec_free(str_vec* sv){
    if (!sv) return;
    for (size_t i=0;i<sv->n;i++) free(sv->v[i]);
    free(sv->v); sv->v=NULL; sv->n=0;
}

/* keep_empty: 0 ignore empty, 1 conserve éléments vides */
STR_API int str_split(const char* s, const char* seps, int keep_empty, str_vec* out){
    if (!s||!seps||!out) return -1;
    out->v=NULL; out->n=0;
    size_t cap=0;
    const char* p=s; const char* start=p;
    while (*p){
        if (_issep((unsigned char)*p, seps)){
            size_t frag_len=(size_t)(p-start);
            if (keep_empty || frag_len){
                if (out->n==cap){
                    size_t nc=cap?cap*2:8;
                    char** nv=(char**)realloc(out->v,nc*sizeof *nv);
                    if(!nv){ str_vec_free(out); return -1; }
                    out->v=nv; cap=nc;
                }
                char* frag=(char*)malloc(frag_len+1);
                if(!frag){ str_vec_free(out); return -1; }
                memcpy(frag,start,frag_len); frag[frag_len]=0;
                out->v[out->n++]=frag;
            }
            p++; start=p; continue;
        }
        p++;
    }
    size_t frag_len=(size_t)(p-start);
    if (keep_empty || frag_len){
        if (out->n==cap){
            size_t nc=cap?cap*2:8;
            char** nv=(char**)realloc(out->v,nc*sizeof *nv);
            if(!nv){ str_vec_free(out); return -1; }
            out->v=nv; cap=nc;
        }
        char* frag=(char*)malloc(frag_len+1);
        if(!frag){ str_vec_free(out); return -1; }
        memcpy(frag,start,frag_len); frag[frag_len]=0;
        out->v[out->n++]=frag;
    }
    return 0;
}

STR_API char* str_join(const char* sep, const str_vec* sv){
    if (!sv || sv->n==0){ char* z=(char*)malloc(1); if(z) z[0]=0; return z; }
    size_t sl = sep?strlen(sep):0, tot=0;
    for (size_t i=0;i<sv->n;i++){ tot += strlen(sv->v[i]); if (i+1<sv->n) tot += sl; }
    char* out=(char*)malloc(tot+1); if(!out) return NULL;
    char* w=out;
    for (size_t i=0;i<sv->n;i++){
        size_t L=strlen(sv->v[i]); memcpy(w, sv->v[i], L); w+=L;
        if (i+1<sv->n && sl){ memcpy(w, sep, sl); w+=sl; }
    }
    *w=0; return out;
}

/* ===================== Replace / Spaces ===================== */

STR_API char* str_replace_all(const char* s, const char* what, const char* with){
    if (!s||!what||!*what) return s?strdup(s):NULL;
    if (!with) with="";
    size_t lw=strlen(what), lrep=strlen(with);
    size_t cnt=0;
    for (const char* p=s; (p=strstr(p,what)); p+=lw) cnt++;
    if (!cnt) return strdup(s);
    size_t base=strlen(s);
    size_t outlen = base + cnt*(lrep-lw);
    char* out=(char*)malloc(outlen+1); if(!out) return NULL;
    char* w=out; const char* p=s;
    while (1){
        const char* q=strstr(p,what);
        if (!q){ size_t L=strlen(p); memcpy(w,p,L); w+=L; break; }
        size_t L=(size_t)(q-p); memcpy(w,p,L); w+=L;
        memcpy(w,with,lrep); w+=lrep; p=q+lw;
    }
    *w=0; return out;
}

STR_API void str_collapse_spaces_inplace(char* s){
    if (!s) return;
    char* r=s; int in_space=0;
    for (char* p=s; *p; p++){
        if (_is_space((unsigned char)*p)){
            if (!in_space){ *r++=' '; in_space=1; }
        } else { *r++=*p; in_space=0; }
    }
    *r=0;
}

/* ===================== Escape / Unescape (C-like) ===================== */

STR_API char* str_escape_c(const char* s){
    if (!s) return NULL;
    size_t cap=16, n=0;
    char* out=(char*)malloc(cap); if(!out) return NULL;
    for (const unsigned char* p=(const unsigned char*)s; *p; p++){
        char buf[5]; size_t k=0;
        switch (*p){
            case '\n': buf[0]='\\'; buf[1]='n'; k=2; break;
            case '\r': buf[0]='\\'; buf[1]='r'; k=2; break;
            case '\t': buf[0]='\\'; buf[1]='t'; k=2; break;
            case '\\': buf[0]='\\'; buf[1]='\\'; k=2; break;
            case '\"': buf[0]='\\'; buf[1]='\"'; k=2; break;
            default:
                if (*p<32 || *p==127){ snprintf(buf,sizeof buf,"\\x%02X", (unsigned)*p); k=4; }
                else { buf[0]=(char)*p; k=1; }
        }
        if (n+k+1>cap){
            while (cap<n+k+1) cap*=2;
            char* nb=(char*)realloc(out,cap); if(!nb){ free(out); return NULL; }
            out=nb;
        }
        memcpy(out+n, buf, k); n+=k;
    }
    out[n]=0; return out;
}

STR_API char* str_unescape_c(const char* s){
    if (!s) return NULL;
    size_t cap=strlen(s)+1;
    char* out=(char*)malloc(cap); if(!out) return NULL;
    size_t n=0;
    for (const char* p=s; *p; p++){
        if (*p!='\\'){ out[n++]=*p; continue; }
        p++;
        if (!*p){ out[n++]='\\'; break; }
        switch (*p){
            case 'n': out[n++]='\n'; break;
            case 'r': out[n++]='\r'; break;
            case 't': out[n++]='\t'; break;
            case '\\': out[n++]='\\'; break;
            case '\"': out[n++]='\"'; break;
            case 'x': {
                int h1=_hexval((unsigned char)p[1]), h2=(h1>=0)?_hexval((unsigned char)p[2]):-1;
                if (h1>=0 && h2>=0){ out[n++]=(char)((h1<<4)|h2); p+=2; }
                else out[n++]='x';
            } break;
            default: out[n++]=*p; break;
        }
        if (n+1>cap){ cap*=2; char* nb=(char*)realloc(out,cap); if(!nb){ free(out); return NULL; } out=nb; }
    }
    out[n]=0; return out;
}

/* ===================== Hex ===================== */

STR_API char* str_hex_encode(const void* data, size_t n, int uppercase){
    if (!data && n) return NULL;
    const unsigned char* b=(const unsigned char*)data;
    const char* hex = uppercase? "0123456789ABCDEF" : "0123456789abcdef";
    char* out=(char*)malloc(n*2+1); if(!out) return NULL;
    for (size_t i=0;i<n;i++){ out[2*i]=hex[b[i]>>4]; out[2*i+1]=hex[b[i]&0xF]; }
    out[2*n]=0; return out;
}
STR_API int str_hex_decode(const char* hex, void** out_data, size_t* out_n){
    if (!hex||!out_data||!out_n) return -1;
    size_t L=strlen(hex); if (L%2) return -1;
    unsigned char* out=(unsigned char*)malloc(L/2); if(!out) return -1;
    for (size_t i=0;i<L; i+=2){
        int h1=_hexval((unsigned char)hex[i]), h2=_hexval((unsigned char)hex[i+1]);
        if (h1<0||h2<0){ free(out); return -1; }
        out[i/2]=(unsigned char)((h1<<4)|h2);
    }
    *out_data=out; *out_n=L/2; return 0;
}

/* ===================== Base64 ===================== */

static const char _b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static inline int _b64val(int c){
    if (c>='A'&&c<='Z') return c-'A';
    if (c>='a'&&c<='z') return 26 + c-'a';
    if (c>='0'&&c<='9') return 52 + c-'0';
    if (c=='+') return 62; if (c=='/') return 63;
    if (c=='=') return -2; return -1;
}
STR_API char* str_base64_encode(const void* data, size_t n){
    if (!data && n) return NULL;
    const unsigned char* in=(const unsigned char*)data;
    size_t outlen = ((n+2)/3)*4;
    char* out=(char*)malloc(outlen+1); if(!out) return NULL;
    size_t i=0,o=0;
    while (i<n){
        uint32_t v=0; size_t left=n-i;
        v  = (uint32_t)in[i]<<16;
        if (left>1) v |= (uint32_t)in[i+1]<<8;
        if (left>2) v |= (uint32_t)in[i+2];
        out[o++] = _b64tab[(v>>18)&63];
        out[o++] = _b64tab[(v>>12)&63];
        out[o++] = (left>1)? _b64tab[(v>>6)&63] : '=';
        out[o++] = (left>2)? _b64tab[v&63]      : '=';
        i += 3;
    }
    out[o]=0; return out;
}
STR_API int str_base64_decode(const char* s, void** out_data, size_t* out_n){
    if (!s||!out_data||!out_n) return -1;
    size_t L=strlen(s);
    /* trim trailing ws */
    while (L && (s[L-1]=='\n'||s[L-1]=='\r'||s[L-1]==' '||s[L-1]=='\t')) L--;
    size_t cap=(L/4)*3; if (!cap) cap=1;
    unsigned char* out=(unsigned char*)malloc(cap); if(!out) return -1;
    size_t o=0; uint32_t buf=0; int pad=0; int n=0;
    for (size_t i=0;i<L;i++){
        int v=_b64val((unsigned char)s[i]);
        if (v==-1) continue; /* ignore whitespace/invalids */
        if (v==-2){ v=0; pad++; }
        buf=(buf<<6) | (uint32_t)v; n++;
        if (n==4){
            if (o+3>cap){ size_t nc=cap+3; unsigned char* nb=(unsigned char*)realloc(out,nc); if(!nb){ free(out); return -1; } out=nb; cap=nc; }
            out[o++] = (unsigned char)((buf>>16)&0xFF);
            if (pad<2) out[o++] = (unsigned char)((buf>>8)&0xFF);
            if (pad<1) out[o++] = (unsigned char)(buf&0xFF);
            buf=0; n=0; pad=0;
        }
    }
    *out_data=out; *out_n=o; return 0;
}

/* ===================== StringBuilder ===================== */

typedef struct {
    char*  buf;
    size_t len;
    size_t cap;
} str_sb;

STR_API void sb_init(str_sb* sb){ if(!sb) return; sb->buf=NULL; sb->len=0; sb->cap=0; }
STR_API void sb_free(str_sb* sb){ if(!sb) return; free(sb->buf); sb->buf=NULL; sb->len=sb->cap=0; }
STR_API void sb_clear(str_sb* sb){ if(!sb) return; sb->len=0; if(sb->buf) sb->buf[0]=0; }
static int _sb_ensure(str_sb* sb, size_t need){
    if (need<=sb->cap) return 0;
    size_t nc = sb->cap? sb->cap:64; while (nc<need) nc*=2;
    char* nb=(char*)realloc(sb->buf,nc); if(!nb) return -1; sb->buf=nb; sb->cap=nc; return 0;
}
STR_API int sb_appendn(str_sb* sb, const void* data, size_t n){
    if(!sb||(!data&&n)) return -1; if (_sb_ensure(sb, sb->len+n+1)!=0) return -1;
    memcpy(sb->buf+sb->len, data, n); sb->len+=n; sb->buf[sb->len]=0; return 0;
}
STR_API int sb_append(str_sb* sb, const char* s){ return sb_appendn(sb, s?s:"", s?strlen(s):0); }
STR_API int sb_appendf(str_sb* sb, const char* fmt, ...){
    if(!sb||!fmt) return -1; va_list ap; va_start(ap,fmt);
    char tmp[1024];
#if defined(_WIN32)
    int n=_vsnprintf(tmp,sizeof tmp,fmt,ap);
#else
    int n=vsnprintf(tmp,sizeof tmp,fmt,ap);
#endif
    va_end(ap);
    if (n<0) return -1;
    if ((size_t)n < sizeof tmp) return sb_appendn(sb, tmp, (size_t)n);
    size_t need=(size_t)n+1; char* big=(char*)malloc(need); if(!big) return -1;
    va_start(ap,fmt);
#if defined(_WIN32)
    _vsnprintf(big,need,fmt,ap);
#else
    vsnprintf(big,need,fmt,ap);
#endif
    va_end(ap);
    int rc=sb_appendn(sb,big,(size_t)n); free(big); return rc;
}
STR_API const char* sb_data(const str_sb* sb){ return sb && sb->buf? sb->buf : ""; }
STR_API size_t sb_len(const str_sb* sb){ return sb? sb->len : 0; }

/* ===================== Tests ===================== */
#ifdef STR_TEST
static void _assert(int cond, const char* msg){ if(!cond){ fprintf(stderr,"ASSERT: %s\n", msg); exit(1);} }
int main(void){
    char a1[64]="  Hello  "; _assert(strcmp(str_strip(a1),"Hello")==0,"strip");

    _assert(str_startswith("foobar","foo"),"starts"); _assert(str_endswith_ci("AbC","bc"),"ends ci");
    _assert(str_icmp("aBc","Abc")==0,"icmp");

    str_vec v; _assert(str_split("a,,b,c", ",", 0, &v)==0, "split"); _assert(v.n==3,"split cnt");
    char* j=str_join("-", &v); _assert(strcmp(j,"a-b-c")==0,"join"); free(j); str_vec_free(&v);

    char* r = str_replace_all("xx--xx--","--","="); _assert(strcmp(r,"xx=xx=")==0,"repl"); free(r);

    char* esc=str_escape_c("hi\t\n"); char* un=str_unescape_c(esc); _assert(strcmp(un,"hi\t\n")==0,"esc/un"); free(esc); free(un);

    const unsigned char dat[]={0,1,0xFE,0xFF}; char* hx=str_hex_encode(dat,sizeof dat,0);
    void* back=NULL; size_t bn=0; _assert(str_hex_decode(hx,&back,&bn)==0 && bn==4 && ((unsigned char*)back)[2]==0xFE,"hex"); free(hx); free(back);

    char* b64=str_base64_encode("hello",5); void* db=NULL; size_t dn=0; _assert(str_base64_decode(b64,&db,&dn)==0 && dn==5 && memcmp(db,"hello",5)==0,"b64"); free(b64); free(db);

    str_sb sb; sb_init(&sb); sb_append(&sb,"Hello "); sb_appendf(&sb,"%d",123); _assert(strcmp(sb_data(&sb),"Hello 123")==0,"sb"); sb_free(&sb);

    puts("ok");
    return 0;
}
#endif