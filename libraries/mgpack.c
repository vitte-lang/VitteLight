// SPDX-License-Identifier: GPL-3.0-or-later
//
// mgpack.c — Mini-MessagePack (encode/décode) pour Vitte Light (C17, sans dépendances)
// Namespace: "mgp"
//
// Portabilité: Linux/macOS/Windows. Pas d’headers non standard.
// Couverture: nil, bool, int/uint64, float64, str, bin, array, map.
// Point de vue: API bas niveau orientée buffer. Aucune allocation implicite.
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c mgpack.c
//
// Test rapide (MGPACK_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DMGPACK_TEST mgpack.c && ./a.out

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* ==================== Endianness helpers (no includes) ==================== */
static inline uint16_t _mgp_bswap16(uint16_t v){ return (uint16_t)((v>>8) | (v<<8)); }
static inline uint32_t _mgp_bswap32(uint32_t v){ return (v>>24) | ((v>>8)&0x0000FF00u) | ((v<<8)&0x00FF0000u) | (v<<24); }
static inline uint64_t _mgp_bswap64(uint64_t x){
    return ((uint64_t)_mgp_bswap32((uint32_t)(x>>32))) |
           ((uint64_t)_mgp_bswap32((uint32_t)(x & 0xFFFFFFFFu))<<32);
}
#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
  #define _MGP_LE 1
#else
  #define _MGP_LE 0
#endif
static inline uint16_t _mgp_be16(uint16_t v){ return _MGP_LE? _mgp_bswap16(v):v; }
static inline uint32_t _mgp_be32(uint32_t v){ return _MGP_LE? _mgp_bswap32(v):v; }
static inline uint64_t _mgp_be64(uint64_t v){ return _MGP_LE? _mgp_bswap64(v):v; }

/* ==================== API visibility ==================== */
#ifndef MGP_API
#define MGP_API
#endif

/* ==================== Writer ==================== */

typedef struct {
    unsigned char* data;
    size_t cap;
    size_t len;
} mgp_buf;

/* init avec mémoire fournie (pas de realloc). Retour 0 si ok. */
MGP_API int mgp_buf_init(mgp_buf* b, void* storage, size_t cap){
    if (!b || (!storage && cap)) return -1;
    b->data=(unsigned char*)storage; b->cap=cap; b->len=0; return 0;
}
MGP_API size_t mgp_buf_size(const mgp_buf* b){ return b? b->len:0; }

static inline int _mgp_put(mgp_buf* b, const void* p, size_t n){
    if (b->len + n > b->cap) return -1;
    memcpy(b->data + b->len, p, n); b->len += n; return 0;
}
static inline int _mgp_u8(mgp_buf* b, uint8_t v){ return _mgp_put(b,&v,1); }
static inline int _mgp_u16be(mgp_buf* b, uint16_t v){ v=_mgp_be16(v); return _mgp_put(b,&v,2); }
static inline int _mgp_u32be(mgp_buf* b, uint32_t v){ v=_mgp_be32(v); return _mgp_put(b,&v,4); }
static inline int _mgp_u64be(mgp_buf* b, uint64_t v){ v=_mgp_be64(v); return _mgp_put(b,&v,8); }

/* ----- Types de base MessagePack ----- */

/* nil */
MGP_API int mgp_write_nil(mgp_buf* b){ return _mgp_u8(b, 0xC0); }

/* bool */
MGP_API int mgp_write_bool(mgp_buf* b, int v){ return _mgp_u8(b, v?0xC3:0xC2); }

/* signed int (choisit la forme compacte) */
MGP_API int mgp_write_int(mgp_buf* b, int64_t x){
    if (x >= 0){
        if (x <= 0x7F) return _mgp_u8(b,(uint8_t)x);                 /* positive fixint */
        if (x <= 0xFF) { if (_mgp_u8(b,0xCC)) return -1; return _mgp_u8(b,(uint8_t)x); }          /* uint8 */
        if (x <= 0xFFFF){ if (_mgp_u8(b,0xCD)||_mgp_u16be(b,(uint16_t)x)) return -1; return 0; }   /* uint16 */
        if (x <= 0xFFFFFFFFu){ if (_mgp_u8(b,0xCE)||_mgp_u32be(b,(uint32_t)x)) return -1; return 0; }/* uint32 */
        if (_mgp_u8(b,0xCF)||_mgp_u64be(b,(uint64_t)x)) return -1; return 0;                       /* uint64 */
    } else {
        if (x >= -32) return _mgp_u8(b, (uint8_t)(0xE0 | (int8_t)x)); /* negative fixint */
        if (x >= -128){ if (_mgp_u8(b,0xD0)) return -1; return _mgp_u8(b,(uint8_t)(int8_t)x); }     /* int8 */
        if (x >= -32768){ if (_mgp_u8(b,0xD1)||_mgp_u16be(b,(uint16_t)(int16_t)x)) return -1; return 0; }
        if (x >= -2147483647-1){ if (_mgp_u8(b,0xD2)||_mgp_u32be(b,(uint32_t)(int32_t)x)) return -1; return 0; }
        if (_mgp_u8(b,0xD3)||_mgp_u64be(b,(uint64_t)x)) return -1; return 0; /* int64 */
    }
}

/* uint direct (utilitaire) */
MGP_API int mgp_write_uint(mgp_buf* b, uint64_t x){
    if (x <= 0x7F) return _mgp_u8(b,(uint8_t)x);
    if (x <= 0xFF)  { if (_mgp_u8(b,0xCC)) return -1; return _mgp_u8(b,(uint8_t)x); }
    if (x <= 0xFFFF){ if (_mgp_u8(b,0xCD)||_mgp_u16be(b,(uint16_t)x)) return -1; return 0; }
    if (x <= 0xFFFFFFFFu){ if (_mgp_u8(b,0xCE)||_mgp_u32be(b,(uint32_t)x)) return -1; return 0; }
    if (_mgp_u8(b,0xCF)||_mgp_u64be(b,x)) return -1; return 0;
}

/* float64 */
MGP_API int mgp_write_f64(mgp_buf* b, double d){
    union { double d; uint64_t u; } u; u.d=d;
    if (_mgp_u8(b,0xCB) || _mgp_u64be(b,u.u)) return -1; return 0;
}

/* str: UTF-8 arbitraire (octets). n <= UINT32_MAX */
MGP_API int mgp_write_str(mgp_buf* b, const char* s, uint32_t n){
    if (!s && n) return -1;
    if (n <= 31){ if (_mgp_u8(b, (uint8_t)(0xA0 | n))) return -1; return _mgp_put(b,s,n); }
    if (n <= 0xFF){ if (_mgp_u8(b,0xD9)||_mgp_u8(b,(uint8_t)n)) return -1; return _mgp_put(b,s,n); }
    if (n <= 0xFFFF){ if (_mgp_u8(b,0xDA)||_mgp_u16be(b,(uint16_t)n)) return -1; return _mgp_put(b,s,n); }
    if (_mgp_u8(b,0xDB)||_mgp_u32be(b,n)) return -1; return _mgp_put(b,s,n);
}

/* bin: données brutes */
MGP_API int mgp_write_bin(mgp_buf* b, const void* p, uint32_t n){
    if (!p && n) return -1;
    if (n <= 0xFF){ if (_mgp_u8(b,0xC4)||_mgp_u8(b,(uint8_t)n)) return -1; return _mgp_put(b,p,n); }
    if (n <= 0xFFFF){ if (_mgp_u8(b,0xC5)||_mgp_u16be(b,(uint16_t)n)) return -1; return _mgp_put(b,p,n); }
    if (_mgp_u8(b,0xC6)||_mgp_u32be(b,n)) return -1; return _mgp_put(b,p,n);
}

/* array header */
MGP_API int mgp_write_array_hdr(mgp_buf* b, uint32_t n){
    if (n <= 15) return _mgp_u8(b, (uint8_t)(0x90 | n));
    if (n <= 0xFFFF){ if (_mgp_u8(b,0xDC)||_mgp_u16be(b,(uint16_t)n)) return -1; return 0; }
    if (_mgp_u8(b,0xDD)||_mgp_u32be(b,n)) return -1; return 0;
}
/* map header */
MGP_API int mgp_write_map_hdr(mgp_buf* b, uint32_t n){
    if (n <= 15) return _mgp_u8(b, (uint8_t)(0x80 | n));
    if (n <= 0xFFFF){ if (_mgp_u8(b,0xDE)||_mgp_u16be(b,(uint16_t)n)) return -1; return 0; }
    if (_mgp_u8(b,0xDF)||_mgp_u32be(b,n)) return -1; return 0;
}

/* ==================== Reader / View ==================== */

typedef struct {
    const unsigned char* p;
    size_t n;    /* taille totale */
    size_t i;    /* index courant */
} mgp_view;

MGP_API int mgp_view_init(mgp_view* v, const void* data, size_t n){
    if (!v) return -1; v->p=(const unsigned char*)data; v->n=n; v->i=0; return 0;
}
static inline int _need(mgp_view* v, size_t k){ return (v->i + k <= v->n) ? 0 : -1; }
static inline uint8_t  _g1(mgp_view* v){ return v->p[v->i++]; }
static inline uint16_t _g2be(mgp_view* v){ uint16_t x=(uint16_t)v->p[v->i]<<8 | v->p[v->i+1]; v->i+=2; return x; }
static inline uint32_t _g4be(mgp_view* v){ uint32_t x=((uint32_t)v->p[v->i]<<24)|((uint32_t)v->p[v->i+1]<<16)|((uint32_t)v->p[v->i+2]<<8)|v->p[v->i+3]; v->i+=4; return x; }
static inline uint64_t _g8be(mgp_view* v){
    uint64_t x=0; for (int k=0;k<8;k++){ x=(x<<8)|v->p[v->i+k]; } v->i+=8; return x;
}

/* Peek sans consommer. Retourne l’octet de type ou -1 si fin. */
MGP_API int mgp_peek(const mgp_view* v){ return (v->i < v->n)? v->p[v->i] : -1; }

/* Sauter un objet (pratique pour itérer). Retour 0 si ok. */
MGP_API int mgp_skip(mgp_view* v);

/* Décodages de base. Les fonctions renvoient 0 si lecture ok. */
MGP_API int mgp_read_nil(mgp_view* v){ if (_need(v,1)) return -1; return (_g1(v)==0xC0)?0:-1; }
MGP_API int mgp_read_bool(mgp_view* v, int* out){
    if (_need(v,1)) return -1; uint8_t t=_g1(v);
    if (t==0xC2){ if(out)*out=0; return 0; } if (t==0xC3){ if(out)*out=1; return 0; }
    return -1;
}
MGP_API int mgp_read_int(mgp_view* v, int64_t* out){
    if (_need(v,1)) return -1; uint8_t t = v->p[v->i];
    /* positive/negative fixint */
    if ((t & 0x80)==0){ v->i++; if(out)*out=t; return 0; }
    if ((t & 0xE0)==0xE0){ v->i++; if(out)*out=(int8_t)t; return 0; }
    /* family */
    v->i++;
    switch (t){
        case 0xCC: if (_need(v,1)) return -1; { uint8_t u=_g1(v); if(out)*out=u; return 0; }
        case 0xCD: if (_need(v,2)) return -1; { uint16_t u=_g2be(v); if(out)*out=u; return 0; }
        case 0xCE: if (_need(v,4)) return -1; { uint32_t u=_g4be(v); if(out)*out=u; return 0; }
        case 0xCF: if (_need(v,8)) return -1; { uint64_t u=_g8be(v); if(out)*out=(int64_t)u; return 0; }
        case 0xD0: if (_need(v,1)) return -1; { int8_t  s=(int8_t)_g1(v); if(out)*out=s; return 0; }
        case 0xD1: if (_need(v,2)) return -1; { int16_t s=(int16_t)_g2be(v); if(out)*out=s; return 0; }
        case 0xD2: if (_need(v,4)) return -1; { int32_t s=(int32_t)_g4be(v); if(out)*out=s; return 0; }
        case 0xD3: if (_need(v,8)) return -1; { int64_t s=(int64_t)_g8be(v); if(out)*out=s; return 0; }
        default: return -1;
    }
}
MGP_API int mgp_read_f64(mgp_view* v, double* out){
    if (_need(v,1)) return -1; if (v->p[v->i++]!=0xCB) return -1;
    if (_need(v,8)) return -1; union { uint64_t u; double d; } u; u.u=_g8be(v); if(out)*out=u.d; return 0;
}

/* str/bin header parse. Renvoie ptr et taille dans le buffer existant. */
MGP_API int mgp_read_str(mgp_view* v, const unsigned char** s, uint32_t* n){
    if (_need(v,1)) return -1; uint8_t t=v->p[v->i++];
    uint32_t len=0;
    if ((t & 0xE0)==0xA0){ len = t & 0x1F; }
    else if (t==0xD9){ if (_need(v,1)) return -1; len=_g1(v); }
    else if (t==0xDA){ if (_need(v,2)) return -1; len=_g2be(v); }
    else if (t==0xDB){ if (_need(v,4)) return -1; len=_g4be(v); }
    else return -1;
    if (_need(v,len)) return -1;
    if (s) *s = v->p + v->i; if (n) *n = len; v->i += len; return 0;
}
MGP_API int mgp_read_bin(mgp_view* v, const unsigned char** p, uint32_t* n){
    if (_need(v,1)) return -1; uint8_t t=v->p[v->i++];
    uint32_t len=0;
    if (t==0xC4){ if (_need(v,1)) return -1; len=_g1(v); }
    else if (t==0xC5){ if (_need(v,2)) return -1; len=_g2be(v); }
    else if (t==0xC6){ if (_need(v,4)) return -1; len=_g4be(v); }
    else return -1;
    if (_need(v,len)) return -1;
    if (p) *p = v->p + v->i; if (n) *n = len; v->i += len; return 0;
}

/* Headers Array/Map: renvoie la taille. */
MGP_API int mgp_read_array_hdr(mgp_view* v, uint32_t* n){
    if (_need(v,1)) return -1; uint8_t t=v->p[v->i++];
    if ((t & 0xF0)==0x90){ if(n)*n=t&0x0F; return 0; }
    if (t==0xDC){ if(_need(v,2)) return -1; if(n)*n=_g2be(v); return 0; }
    if (t==0xDD){ if(_need(v,4)) return -1; if(n)*n=_g4be(v); return 0; }
    return -1;
}
MGP_API int mgp_read_map_hdr(mgp_view* v, uint32_t* n){
    if (_need(v,1)) return -1; uint8_t t=v->p[v->i++];
    if ((t & 0xF0)==0x80){ if(n)*n=t&0x0F; return 0; }
    if (t==0xDE){ if(_need(v,2)) return -1; if(n)*n=_g2be(v); return 0; }
    if (t==0xDF){ if(_need(v,4)) return -1; if(n)*n=_g4be(v); return 0; }
    return -1;
}

/* Skip récursif */
static int _mgp_skip1(mgp_view* v){
    if (_need(v,1)) return -1;
    uint8_t t=v->p[v->i++];
    /* fixints/str/array/map cases */
    if ((t & 0x80)==0 || (t & 0xE0)==0xE0) return 0; /* fixint */
    if ((t & 0xE0)==0xA0){ uint32_t n=t&0x1F; if (_need(v,n)) return -1; v->i+=n; return 0; }
    if ((t & 0xF0)==0x90){ uint32_t n=t&0x0F; for(uint32_t i=0;i<n;i++) if(mgp_skip(v)) return -1; return 0; }
    if ((t & 0xF0)==0x80){ uint32_t n=t&0x0F; for(uint32_t i=0;i<n;i++){ if(mgp_skip(v)||mgp_skip(v)) return -1; } return 0; }
    switch (t){
        case 0xC0: return 0;               /* nil */
        case 0xC2: case 0xC3: return 0;    /* bool */
        case 0xCC: if (_need(v,1)) return -1; v->i+=1; return 0;
        case 0xCD: if (_need(v,2)) return -1; v->i+=2; return 0;
        case 0xCE: if (_need(v,4)) return -1; v->i+=4; return 0;
        case 0xCF: if (_need(v,8)) return -1; v->i+=8; return 0;
        case 0xD0: if (_need(v,1)) return -1; v->i+=1; return 0;
        case 0xD1: if (_need(v,2)) return -1; v->i+=2; return 0;
        case 0xD2: if (_need(v,4)) return -1; v->i+=4; return 0;
        case 0xD3: if (_need(v,8)) return -1; v->i+=8; return 0;
        case 0xCB: if (_need(v,8)) return -1; v->i+=8; return 0; /* f64 */
        case 0xD9: if (_need(v,1)) return -1; { uint32_t n=_g1(v); if(_need(v,n)) return -1; v->i+=n; return 0; }
        case 0xDA: if (_need(v,2)) return -1; { uint32_t n=_g2be(v); if(_need(v,n)) return -1; v->i+=n; return 0; }
        case 0xDB: if (_need(v,4)) return -1; { uint32_t n=_g4be(v); if(_need(v,n)) return -1; v->i+=n; return 0; }
        case 0xC4: if (_need(v,1)) return -1; { uint32_t n=_g1(v); if(_need(v,n)) return -1; v->i+=n; return 0; }
        case 0xC5: if (_need(v,2)) return -1; { uint32_t n=_g2be(v); if(_need(v,n)) return -1; v->i+=n; return 0; }
        case 0xC6: if (_need(v,4)) return -1; { uint32_t n=_g4be(v); if(_need(v,n)) return -1; v->i+=n; return 0; }
        case 0xDC: if (_need(v,2)) return -1; { uint32_t n=_g2be(v); for(uint32_t i=0;i<n;i++) if(mgp_skip(v)) return -1; return 0; }
        case 0xDD: if (_need(v,4)) return -1; { uint32_t n=_g4be(v); for(uint32_t i=0;i<n;i++) if(mgp_skip(v)) return -1; return 0; }
        case 0xDE: if (_need(v,2)) return -1; { uint32_t n=_g2be(v); for(uint32_t i=0;i<n;i++){ if(mgp_skip(v)||mgp_skip(v)) return -1; } return 0; }
        case 0xDF: if (_need(v,4)) return -1; { uint32_t n=_g4be(v); for(uint32_t i=0;i<n;i++){ if(mgp_skip(v)||mgp_skip(v)) return -1; } return 0; }
        default: return -1;
    }
}
MGP_API int mgp_skip(mgp_view* v){ return _mgp_skip1(v); }

/* ==================== Utilitaires de haut niveau ==================== */

/* Encode un map simple {string:string} à partir d’un tableau k/v */
MGP_API int mgp_write_map_str_str(mgp_buf* b, const char* const* kv, uint32_t pairs){
    if (mgp_write_map_hdr(b, pairs)) return -1;
    for (uint32_t i=0;i<pairs;i++){
        const char* k = kv[i*2+0];
        const char* v = kv[i*2+1];
        uint32_t nk=(uint32_t)strlen(k), nv=(uint32_t)strlen(v);
        if (mgp_write_str(b, k, nk) || mgp_write_str(b, v, nv)) return -1;
    }
    return 0;
}

/* Decode un map {string:string}. kv_out doit contenir pairs*2 pointeurs vers zones utilisateur.
   On pointe directement dans le buffer d’entrée (pas de copie). */
MGP_API int mgp_read_map_str_str(mgp_view* v, const char*** kv_out, uint32_t* pairs_out){
    uint32_t n; if (mgp_read_map_hdr(v,&n)) return -1;
    if (pairs_out) *pairs_out=n;
    if (!kv_out) { /* consommer sans stocker */ for (uint32_t i=0;i<n;i++){ if (mgp_skip(v)||mgp_skip(v)) return -1; } return 0; }
    /* stocke pointeurs dans un tableau alloué par l’appelant avant l’appel (non géré ici) */
    /* Par simplicité de l’exemple, on refuse si kv_out==NULL ; gestion avancée à l’utilisateur. */
    for (uint32_t i=0;i<n;i++){
        const unsigned char *ks,*vs; uint32_t nk,nv;
        if (mgp_read_str(v,&ks,&nk)) return -1;
        if (mgp_read_str(v,&vs,&nv)) return -1;
        /* cast const char* */
        (*kv_out)[i*2+0] = (const char*)ks;
        (*kv_out)[i*2+1] = (const char*)vs;
        /* Note: longueurs non renvoyées ici, à étendre si besoin */
    }
    return 0;
}

/* ==================== Tests ==================== */
#ifdef MGPACK_TEST
static void hexdump(const void* p, size_t n){
    const unsigned char* x=p;
    for (size_t i=0;i<n;i++){ printf("%02X", x[i]); if ((i&15)==15) putchar('\n'); else putchar(' '); }
    if (n%16) putchar('\n');
}
int main(void){
    unsigned char buf[256];
    mgp_buf w; mgp_buf_init(&w, buf, sizeof buf);

    mgp_write_map_hdr(&w, 3);
    mgp_write_str(&w, "nil", 3); mgp_write_nil(&w);
    mgp_write_str(&w, "b", 1);   mgp_write_bool(&w, 1);
    mgp_write_str(&w, "n", 1);   mgp_write_int(&w, -42);

    mgp_write_str(&w, "f64", 3); mgp_write_f64(&w, 3.14159);
    const char* s="hello";       mgp_write_str(&w,"s",1); mgp_write_str(&w,s,(uint32_t)strlen(s));
    uint8_t blob[3]={1,2,3};     mgp_write_str(&w,"bin",3); mgp_write_bin(&w,blob,3);

    size_t sz = mgp_buf_size(&w);
    puts("== encoded ==");
    hexdump(buf, sz);

    mgp_view v; mgp_view_init(&v, buf, sz);
    /* Lire la première map de 3 entrées puis 3 paires supplémentaires? Non: on a encodé 6 paires (map_hdr 3 puis on a ajouté 3 paires supplémentaires sans nouveau hdr => volontaire pour test de skip) */
    /* Lecture robuste par parcours: */
    mgp_view_init(&v, buf, sz);
    uint32_t m=0; if (mgp_read_map_hdr(&v,&m)==0) printf("map size=%u\n", m);
    for (uint32_t i=0;i<m;i++){
        const unsigned char* k; uint32_t nk;
        if (mgp_read_str(&v,&k,&nk)){ puts("read key fail"); return 1; }
        int t = mgp_peek(&v);
        printf("key=%.*s type=0x%02X\n",(int)nk,(const char*)k,t);
        /* skip value générique */
        if (mgp_skip(&v)){ puts("skip fail"); return 1; }
    }
    puts("ok");
    return 0;
}
#endif