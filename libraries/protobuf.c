// SPDX-License-Identifier: GPL-3.0-or-later
//
// protobuf.c — Encodeur/Décodeur Protobuf minimal (C17, sans dépendances)
// Namespace: "pb"
//
// But:
//   - Buffer writer/reader sans alloc cachée.
//   - Varint(u32/u64), ZigZag(i32/i64), fixed32/fixed64, bytes/string.
//   - Écriture de champs: write_varint / write_fixed* / write_bytes / write_string.
//   - Sous-message: pb_w_submsg_begin/end (length-delimited).
//   - Lecture par itération: pb_next() -> (field_no, wire_type, vues sur données).
//
// Limites:
//   - Pas de maps/packed auto (utilisateur gère).
//   - Pas de float/double helpers (utiliser fixed32/64 avec memcpy).
//   - Pas de vérif d’UTF-8.
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c protobuf.c
//
// Test (PROTOBUF_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DPROTOBUF_TEST protobuf.c && ./a.out

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#ifndef PB_API
#define PB_API
#endif

/* ============================== Writer ============================== */

typedef struct {
    uint8_t* buf; size_t cap; size_t len; int err;
} pb_w;

PB_API void pb_w_init(pb_w* w, void* buf, size_t cap){ w->buf=(uint8_t*)buf; w->cap=cap; w->len=0; w->err=0; }
PB_API size_t pb_w_size(const pb_w* w){ return w->len; }
PB_API int pb_w_ok(const pb_w* w){ return !w->err; }

static int _w_put(pb_w* w, const void* p, size_t n){
    if (w->err) return -1;
    if (w->len + n > w->cap){ w->err=1; return -1; }
    memcpy(w->buf + w->len, p, n); w->len += n; return 0;
}
static int _w_put_u8(pb_w* w, uint8_t b){ return _w_put(w,&b,1); }

static int _w_varint(pb_w* w, uint64_t v){
    uint8_t tmp[10]; size_t i=0;
    while (v>=0x80){ tmp[i++]=(uint8_t)((v&0x7F)|0x80); v>>=7; }
    tmp[i++]=(uint8_t)v;
    return _w_put(w,tmp,i);
}

static uint64_t _zz32(int32_t x){ return (uint32_t)((x<<1) ^ (x>>31)); }
static uint64_t _zz64(int64_t x){ return (uint64_t)((x<<1) ^ (x>>63)); }

enum { PB_WVARINT=0, PB_WFIXED64=1, PB_WLEN=2, PB_WFIXED32=5 };

static int _w_key(pb_w* w, uint32_t field_no, uint8_t wt){
    uint64_t key = ((uint64_t)field_no<<3) | wt;
    return _w_varint(w, key);
}

/* Champs */
PB_API int pb_w_varint(pb_w* w, uint32_t field_no, uint64_t v){
    if (_w_key(w,field_no,PB_WVARINT)!=0) return -1;
    return _w_varint(w,v);
}
PB_API int pb_w_svarint32(pb_w* w, uint32_t field_no, int32_t v){
    return pb_w_varint(w, field_no, _zz32(v));
}
PB_API int pb_w_svarint64(pb_w* w, uint32_t field_no, int64_t v){
    return pb_w_varint(w, field_no, _zz64(v));
}
PB_API int pb_w_fixed32(pb_w* w, uint32_t field_no, uint32_t v){
    if (_w_key(w,field_no,PB_WFIXED32)!=0) return -1;
    uint8_t b[4]={ (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    return _w_put(w,b,4);
}
PB_API int pb_w_fixed64(pb_w* w, uint32_t field_no, uint64_t v){
    if (_w_key(w,field_no,PB_WFIXED64)!=0) return -1;
    uint8_t b[8]; for (int i=0;i<8;i++) b[i]=(uint8_t)(v>>(8*i));
    return _w_put(w,b,8);
}
PB_API int pb_w_bytes(pb_w* w, uint32_t field_no, const void* data, size_t n){
    if (_w_key(w,field_no,PB_WLEN)!=0) return -1;
    if (_w_varint(w,(uint64_t)n)!=0) return -1;
    return _w_put(w,data,n);
}
PB_API int pb_w_string(pb_w* w, uint32_t field_no, const char* s){
    size_t n = s?strlen(s):0;
    return pb_w_bytes(w, field_no, s, n);
}

/* Sous-message: écrit la clé, la longueur plus tard. */
typedef struct { size_t len_pos; size_t start_payload; } pb_sub;

PB_API int pb_w_submsg_begin(pb_w* w, uint32_t field_no, pb_sub* out){
    if (!out) return -1;
    if (_w_key(w,field_no,PB_WLEN)!=0) return -1;
    /* réserver au max 5 octets pour la longueur (varint) en placeholder 0x80..0x00 */
    out->len_pos = w->len;
    uint8_t pad[5]={0x80,0x80,0x80,0x80,0x00};
    if (_w_put(w,pad,5)!=0) return -1;
    out->start_payload = w->len;
    return 0;
}

/* Encode la longueur réelle dans les 5 octets réservés, puis compacte si possible. */
PB_API int pb_w_submsg_end(pb_w* w, const pb_sub* sb){
    if (!sb || w->err) return -1;
    size_t payload_len = w->len - sb->start_payload;
    /* encoder varint dans tmp */
    uint8_t tmp[10]; size_t m=0; uint64_t v=payload_len;
    do{ uint8_t d=(uint8_t)(v&0x7F); v>>=7; if (v) d|=0x80; tmp[m++]=d; }while(v);
    /* décaler le payload si m<5 pour compacter */
    size_t pad = 5 - m;
    if (pad){
        /* déplacer payload vers la gauche */
        memmove(w->buf + sb->len_pos + m, w->buf + sb->len_pos + 5, payload_len);
        w->len -= pad;
    }
    memcpy(w->buf + sb->len_pos, tmp, m);
    return 0;
}

/* ============================== Reader ============================== */

typedef struct {
    const uint8_t* p; size_t n; size_t i; int err;
} pb_r;

PB_API void pb_r_init(pb_r* r, const void* buf, size_t n){ r->p=(const uint8_t*)buf; r->n=n; r->i=0; r->err=0; }
PB_API int pb_r_ok(const pb_r* r){ return !r->err; }
PB_API size_t pb_r_left(const pb_r* r){ return (r->i<=r->n)? (r->n - r->i) : 0; }

static int _r_get(pb_r* r, uint8_t* out){
    if (r->i>=r->n){ r->err=1; return -1; }
    *out = r->p[r->i++]; return 0;
}
static int _r_varint(pb_r* r, uint64_t* out){
    uint64_t v=0; int s=0; uint8_t b=0; for (int i=0;i<10;i++){
        if (_r_get(r,&b)!=0) return -1;
        v |= ((uint64_t)(b&0x7F))<<s; s+=7;
        if (!(b&0x80)){ *out=v; return 0; }
    }
    r->err=1; return -1;
}
static int64_t _unz32(uint64_t z){ return (int32_t)((z>>1) ^ -(int32_t)(z&1)); }
static int64_t _unz64(uint64_t z){ return (int64_t)((z>>1) ^ -(int64_t)(z&1)); }

/* Vue sur un champ length-delimited */
typedef struct { const uint8_t* p; size_t n; } pb_view;

/* Itération: retourne 1 si champ lu, 0 fin, -1 erreur.
   Remplit field_no, wire_type et:
     - si wt=0: *val_u64
     - si wt=1: *val_u64 (fixed64 dans bits)
     - si wt=5: *(uint32_t*)val_u64 inférieur (fixed32)
     - si wt=2: *view (p,n) */
PB_API int pb_next(pb_r* r, uint32_t* field_no, uint8_t* wt, uint64_t* val_u64, pb_view* view){
    if (pb_r_left(r)==0) return 0;
    uint64_t key=0;
    if (_r_varint(r,&key)!=0) return -1;
    *field_no = (uint32_t)(key>>3);
    *wt = (uint8_t)(key & 7);
    switch (*wt){
        case PB_WVARINT: {
            if (_r_varint(r,val_u64)!=0) return -1;
            return 1;
        }
        case PB_WFIXED64: {
            if (pb_r_left(r)<8){ r->err=1; return -1; }
            uint64_t v=0; for (int i=0;i<8;i++) v |= ((uint64_t)r->p[r->i++])<<(8*i);
            *val_u64=v; return 1;
        }
        case PB_WFIXED32: {
            if (pb_r_left(r)<4){ r->err=1; return -1; }
            uint32_t v=0; for (int i=0;i<4;i++) v |= ((uint32_t)r->p[r->i++])<<(8*i);
            *val_u64=v; return 1;
        }
        case PB_WLEN: {
            uint64_t ln=0; if (_r_varint(r,&ln)!=0) return -1;
            if (pb_r_left(r)<ln){ r->err=1; return -1; }
            view->p = r->p + r->i; view->n = (size_t)ln; r->i += (size_t)ln;
            return 1;
        }
        default: r->err=1; return -1;
    }
}

/* Helpers de lecture */
PB_API int pb_view_as_string(const pb_view* v, const char** s, size_t* n){
    if (!v||!s||!n) return -1; *s=(const char*)v->p; *n=v->n; return 0;
}
PB_API int pb_view_as_bytes(const pb_view* v, const uint8_t** p, size_t* n){
    if (!v||!p||!n) return -1; *p=v->p; *n=v->n; return 0;
}
PB_API int pb_view_as_reader(const pb_view* v, pb_r* sub){
    if (!v||!sub) return -1; pb_r_init(sub, v->p, v->n); return 0;
}

/* ============================== Test ============================== */
#ifdef PROTOBUF_TEST
#include <assert.h>
int main(void){
    uint8_t buf[256]; pb_w w; pb_w_init(&w,buf,sizeof buf);

    // message:
    // 1: int32  = -5 (svarint)
    // 2: string = "hi"
    // 3: submsg { 1: u64=42, 2: bytes= [01 02] }
    pb_w_svarint32(&w,1,-5);
    pb_w_string(&w,2,"hi");

    pb_sub sb; pb_w_submsg_begin(&w,3,&sb);
      pb_w_varint(&w,1,42);
      const uint8_t b2[2]={1,2}; pb_w_bytes(&w,2,b2,2);
    pb_w_submsg_end(&w,&sb);

    assert(pb_w_ok(&w));

    pb_r r; pb_r_init(&r,buf,pb_w_size(&w));
    uint32_t f; uint8_t wt; uint64_t u; pb_view v;

    int got1=0,got2=0,got3a=0,got3b=0;
    while (pb_next(&r,&f,&wt,&u,&v)>0){
        if (f==1 && wt==0){ /* svarint32 décodage */
            int32_t x=(int32_t)_unz32(u);
            assert(x==-5); got1=1;
        } else if (f==2 && wt==2){
            const char* s; size_t n; pb_view_as_string(&v,&s,&n);
            assert(n==2 && s[0]=='h' && s[1]=='i'); got2=1;
        } else if (f==3 && wt==2){
            pb_r sub; pb_view_as_reader(&v,&sub);
            while (pb_next(&sub,&f,&wt,&u,&v)>0){
                if (f==1 && wt==0){ assert(u==42); got3a=1; }
                else if (f==2 && wt==2){ const uint8_t* p; size_t n; pb_view_as_bytes(&v,&p,&n); assert(n==2 && p[0]==1 && p[1]==2); got3b=1; }
            }
        }
    }
    assert(got1&&got2&&got3a&&got3b&&pb_r_ok(&r));
    puts("protobuf ok");
    return 0;
}
#endif