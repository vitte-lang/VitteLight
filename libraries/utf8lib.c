// SPDX-License-Identifier: GPL-3.0-or-later
//
// utf8lib.c — Utilitaires UTF-8 avancés (C17, portable, ultra complet)
// Namespace: "u8"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c utf8lib.c
//
// Fournit:
//   - Décodage / encodage points de code (U+0000..U+10FFFF)
//   - Itération avant/arrière sur code points
//   - Validation stricte (pas de surlongs ni de surrogates)
//   - Nettoyage: strip_invalid (remplace par U+FFFD), trim_bom
//   - Mesures: u8_cp_count, u8_cp_at, u8_cp_slice
//   - Recherche: find/rfind par code point
//   - Conversion: tolower/toupper (ASCII + Latin-1 de base)
//   - Comparaison insensible à la casse (approx ASCII/Latin-1)
//   - Normalisation simple NFD→NFC (option: compose é, ê, ö, etc. basique)
//   - Tests (U8_TEST)
//
// Note: pour normalisation Unicode complète, utiliser libicu ou utf8proc.
//

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>

/* ---------- Helpers ---------- */

static int _is_cont(unsigned char c){ return (c & 0xC0u) == 0x80u; }
static int _is_surrogate(uint32_t cp){ return cp>=0xD800u && cp<=0xDFFFu; }

/* ---------- Decode / Encode ---------- */

int u8_decode(const char* s, size_t n, uint32_t* cp, size_t* used){
    if (!s || n==0){ if(used) *used=0; return 0; }
    const unsigned char* p=(const unsigned char*)s;
    unsigned char c=p[0];

    if (c<0x80u){ if(cp) *cp=c; if(used) *used=1; return 1; }

    if ((c&0xE0u)==0xC0u){
        if (n<2||!_is_cont(p[1])){ if(used) *used=(n<2?0:1); return -1; }
        uint32_t x=((uint32_t)(c&0x1Fu)<<6)|(uint32_t)(p[1]&0x3Fu);
        if (x<0x80u){ if(used) *used=2; return -1; }
        if(cp) *cp=x; if(used) *used=2; return 1;
    }
    if ((c&0xF0u)==0xE0u){
        if (n<3||!_is_cont(p[1])||!_is_cont(p[2])){ if(used) *used=(n<3? ( _is_cont(p[1])?2:1):1); return -1; }
        uint32_t x=((uint32_t)(c&0x0Fu)<<12)|((uint32_t)(p[1]&0x3Fu)<<6)|(uint32_t)(p[2]&0x3Fu);
        if (x<0x800u||_is_surrogate(x)){ if(used) *used=3; return -1; }
        if(cp) *cp=x; if(used) *used=3; return 1;
    }
    if ((c&0xF8u)==0xF0u){
        if (n<4||!_is_cont(p[1])||!_is_cont(p[2])||!_is_cont(p[3])){ if(used) *used=(n<4? ( _is_cont(p[1])+_is_cont(p[2])):1); return -1; }
        uint32_t x=((uint32_t)(c&0x07u)<<18)|((uint32_t)(p[1]&0x3Fu)<<12)|((uint32_t)(p[2]&0x3Fu)<<6)|(uint32_t)(p[3]&0x3Fu);
        if (x<0x10000u||x>0x10FFFFu){ if(used) *used=4; return -1; }
        if(cp) *cp=x; if(used) *used=4; return 1;
    }
    if(used) *used=1; return -1;
}

size_t u8_encode(uint32_t cp, char out[4]){
    if (!out) return 0;
    if (cp<=0x7Fu){ out[0]=(char)cp; return 1; }
    if (cp<=0x7FFu){ out[0]=(char)(0xC0|(cp>>6)); out[1]=(char)(0x80|(cp&0x3Fu)); return 2; }
    if (_is_surrogate(cp)||cp>0x10FFFFu) return 0;
    if (cp<=0xFFFFu){
        out[0]=(char)(0xE0|(cp>>12));
        out[1]=(char)(0x80|((cp>>6)&0x3Fu));
        out[2]=(char)(0x80|(cp&0x3Fu));
        return 3;
    }
    out[0]=(char)(0xF0|(cp>>18));
    out[1]=(char)(0x80|((cp>>12)&0x3Fu));
    out[2]=(char)(0x80|((cp>>6)&0x3Fu));
    out[3]=(char)(0x80|(cp&0x3Fu));
    return 4;
}

/* ---------- Iteration ---------- */

const char* u8_next(const char* s,const char* end,uint32_t* cp){
    if(!s) return NULL;
    size_t n=(size_t)(end?(end>=s?(size_t)(end-s):0):(size_t)~0u);
    if(n==0) return NULL;
    size_t used=0; uint32_t c=0;
    int rc=u8_decode(s,n,&c,&used);
    if(rc<=0) return NULL;
    if(cp) *cp=c; return s+(ptrdiff_t)used;
}

const char* u8_prev(const char* begin,const char* s,uint32_t* cp){
    if(!begin||!s||s<=begin) return NULL;
    const unsigned char* b=(const unsigned char*)begin;
    const unsigned char* q=(const unsigned char*)s;
    for(int back=1;back<=4&&q-back>=b;back++){
        uint32_t c=0; size_t used=0;
        if(u8_decode((const char*)(q-back),back,&c,&used)==1&&used==(size_t)back){
            if(cp) *cp=c; return (const char*)(q-back);
        }
    }
    return NULL;
}

/* ---------- Validation / nettoyage ---------- */

int u8_valid(const char* s,size_t n){
    size_t i=0; while(i<n){
        uint32_t cp; size_t used;
        if(u8_decode(s+i,n-i,&cp,&used)!=1) return 0;
        i+=used;
    }
    return 1;
}

size_t u8_strip_invalid(const char* s,size_t n,char* out){
    const char* p=s; const char* e=s+n; size_t w=0;
    while(p<e){
        uint32_t cp=0; size_t used=0;
        int rc=u8_decode(p,(size_t)(e-p),&cp,&used);
        if(rc==1){ if(out) memcpy(out+w,p,used); w+=used; p+=used; }
        else if(rc==0){ char rep[4]; size_t k=u8_encode(0xFFFDu,rep); if(out) memcpy(out+w,rep,k); w+=k; break; }
        else{ char rep[4]; size_t k=u8_encode(0xFFFDu,rep); if(out) memcpy(out+w,rep,k); w+=k; p+=1; }
    }
    return w;
}

size_t u8_trim_bom(const char* s,size_t n){
    if(n>=3){ const unsigned char* p=(const unsigned char*)s; if(p[0]==0xEF&&p[1]==0xBB&&p[2]==0xBF) return n-3; }
    return n;
}

/* ---------- Mesures / accès ---------- */

size_t u8_cp_count(const char* s,size_t n){
    size_t i=0,cnt=0;
    while(i<n){ uint32_t cp; size_t used; int rc=u8_decode(s+i,n-i,&cp,&used); if(rc!=1){ used=(used?used:1); } i+=used; cnt++; }
    return cnt;
}

int u8_cp_at(const char* s,size_t n,size_t index,uint32_t* cp){
    size_t i=0,idx=0;
    while(i<n){ uint32_t c; size_t used; int rc=u8_decode(s+i,n-i,&c,&used); if(rc!=1){ c=0xFFFD; used=(used?used:1); } if(idx==index){ if(cp) *cp=c; return 0; } i+=used; idx++; }
    return -1;
}

size_t u8_cp_slice(const char* s,size_t n,size_t i0,size_t i1,char* out,size_t cap){
    size_t i=0,idx=0,w=0;
    while(i<n&&idx<i1){ uint32_t c; size_t used; int rc=u8_decode(s+i,n-i,&c,&used); if(rc!=1){ c=0xFFFD; used=(used?used:1); } if(idx>=i0){ char tmp[4]; size_t k=(rc==1?0:u8_encode(0xFFFD,tmp)); if(rc==1){ if(out&&w+used<=cap) memcpy(out+w,s+i,used); w+=used; } else { if(out&&w+k<=cap) memcpy(out+w,tmp,k); w+=k; } } i+=used; idx++; }
    return w;
}

/* ---------- Recherche ---------- */

typedef long ssize_t;
ssize_t u8_find_cp(const char* s,size_t n,uint32_t cp){
    size_t i=0,idx=0; while(i<n){ uint32_t c; size_t used; int rc=u8_decode(s+i,n-i,&c,&used); if(rc!=1){ c=0xFFFD; used=(used?used:1); } if(c==cp) return (ssize_t)idx; i+=used; idx++; } return -1;
}
ssize_t u8_rfind_cp(const char* s,size_t n,uint32_t cp){
    const char* p=s+n; long idx=(long)u8_cp_count(s,n)-1;
    while(p>s){ uint32_t c; const char* q=u8_prev(s,p,&c); if(!q) break; if(c==cp) return idx; p=q; idx--; }
    return -1;
}

/* ---------- Conversion ASCII/Latin-1 ---------- */

uint32_t u8_tolower(uint32_t cp){ if(cp<128) return (uint32_t)tolower((int)cp); if(cp>=0xC0&&cp<=0xDE&&cp!=0xD7) return cp+32; return cp; }
uint32_t u8_toupper(uint32_t cp){ if(cp<128) return (uint32_t)toupper((int)cp); if(cp>=0xE0&&cp<=0xFE&&cp!=0xF7) return cp-32; return cp; }

/* Comparaison insensible ASCII/Latin-1 */
int u8_casecmp(const char* a,size_t na,const char* b,size_t nb){
    size_t ia=0,ib=0; while(ia<na&&ib<nb){ uint32_t ca,cb; size_t ua,ub; int ra=u8_decode(a+ia,na-ia,&ca,&ua); int rb=u8_decode(b+ib,nb-ib,&cb,&ub); if(ra!=1){ ca=0xFFFD; ua=1; } if(rb!=1){ cb=0xFFFD; ub=1; } ca=u8_tolower(ca); cb=u8_tolower(cb); if(ca!=cb) return (ca<cb?-1:1); ia+=ua; ib+=ub; } if(ia==na&&ib==nb) return 0; return (ia==na?-1:1);
}

/* ---------- Normalisation simplifiée (ASCII accentué → NFC) ---------- */
/* Table limitée à quelques combinaisons communes */
uint32_t u8_compose_basic(uint32_t base,uint32_t mark){
    if(base=='e'&&mark==0x0301) return 0xE9; /* é */
    if(base=='a'&&mark==0x0300) return 0xE0; /* à */
    if(base=='o'&&mark==0x0308) return 0xF6; /* ö */
    return 0;
}

/* Simpliste: ne gère qu’une combinaison de 2 cp. */
size_t u8_normalize_nfc_basic(const char* s,size_t n,char* out,size_t cap){
    size_t i=0,w=0; uint32_t prev=0;
    while(i<n){ uint32_t c; size_t used; int rc=u8_decode(s+i,n-i,&c,&used); if(rc!=1){ c=0xFFFD; used=1; }
        if(prev){ uint32_t comp=u8_compose_basic(prev,c); if(comp){ char tmp[4]; size_t k=u8_encode(comp,tmp); if(out&&w+k<=cap) memcpy(out+w,tmp,k); w+=k; prev=0; i+=used; continue; } else { char tmp[4]; size_t k=u8_encode(prev,tmp); if(out&&w+k<=cap) memcpy(out+w,tmp,k); w+=k; prev=0; } }
        prev=c; i+=used;
    }
    if(prev){ char tmp[4]; size_t k=u8_encode(prev,tmp); if(out&&w+k<=cap) memcpy(out+w,tmp,k); w+=k; }
    return w;
}

/* ---------- Test ---------- */
#ifdef U8_TEST
#include <stdio.h>
static void dump(const char* s,size_t n){ for(size_t i=0;i<n;i++) printf("%02X", (unsigned char)s[i]); }
int main(void){
    const char* txt="héllo"; size_t n=strlen(txt);
    printf("valid=%d count=%zu\n", u8_valid(txt,n), u8_cp_count(txt,n));
    uint32_t cp; u8_cp_at(txt,n,1,&cp); printf("cp[1]=U+%04X\n",cp);
    char buf[64]; size_t w=u8_cp_slice(txt,n,0,3,buf,sizeof buf); printf("slice= "); dump(buf,w); puts("");
    printf("casecmp=%d\n", u8_casecmp("HELLO",5,"hello",5));
    const char* comp="e\xCC\x81"; char out[16]; size_t wn=u8_normalize_nfc_basic(comp,3,out,sizeof out); printf("norm: "); dump(out,wn); puts("");
    return 0;
}
#endif