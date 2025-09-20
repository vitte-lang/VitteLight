// SPDX-License-Identifier: GPL-3.0-or-later
//
// cbor.c — Minimal CBOR encoder/decoder (RFC 7049/8949 subset) for Vitte Light
// Namespace: "cbor" — C17, portable, no deps
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c cbor.c
//
// Coverage (definite + basic indefinite):
//   - Unsigned (maj 0), Negative (maj 1)
//   - Byte string (maj 2), Text string UTF-8 (maj 3)
//   - Array (maj 4), Map (maj 5) — definite or indefinite with break (0xFF)
//   - Simple/Bool/Null/Undefined (maj 7; values 20/21/22/23)
//   - Float64 (maj 7, ai=27). Float32/half omitted to keep surface small
//   - Optional semantic Tag (maj 6) stored on node
//
// API model:
//   - Builder: write into dynamically growing buffer
//       void  cbor_buf_init(cbor_buf* b);
//       void  cbor_buf_free(cbor_buf* b);
//       int   cbor_write_uint(cbor_buf* b, uint64_t u);
//       int   cbor_write_nint(cbor_buf* b, int64_t i);           // encodes -1 - n
//       int   cbor_write_bstr(cbor_buf* b, const void* p, size_t n);
//       int   cbor_write_tstr(cbor_buf* b, const char* s, size_t n);
//       int   cbor_write_array(cbor_buf* b, size_t count);        // definite
//       int   cbor_write_array_indef(cbor_buf* b);                // 0x9F
//       int   cbor_write_map(cbor_buf* b, size_t count);          // definite
//       int   cbor_write_map_indef(cbor_buf* b);                  // 0xBF
//       int   cbor_write_break(cbor_buf* b);                      // 0xFF
//       int   cbor_write_bool(cbor_buf* b, int v);
//       int   cbor_write_null(cbor_buf* b);
//       int   cbor_write_undef(cbor_buf* b);
//       int   cbor_write_f64(cbor_buf* b, double x);
//       int   cbor_write_tag(cbor_buf* b, uint64_t tag);
//
//   - DOM decode (safe limits):
//       typedef enum { CBOR_UINT, CBOR_NINT, CBOR_BSTR, CBOR_TSTR,
//                      CBOR_ARRAY, CBOR_MAP, CBOR_BOOL, CBOR_NULL,
//                      CBOR_UNDEF, CBOR_FLOAT, CBOR_TAG } cbor_type;
//       typedef struct cbor_item cbor_item;
//       int   cbor_decode(const void* buf, size_t n,
//                         cbor_item** out, size_t max_depth, size_t max_items);
//       void  cbor_free(cbor_item* it);
//       char* cbor_to_json(const cbor_item* it);  // debug; caller free()
//
// Notes:
//   - Returns 0 on success, -1 on error (OOM or malformed).
//   - Decoder supports indefinite strings/arrays/maps collecting chunks until break.
//   - Text strings are stored as raw bytes without UTF-8 validation.
//   - Maps keep insertion order as [key,value] pairs array.
//
// Optional test:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DCBOR_TEST cbor.c && ./a.out

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================= Buffer writer ================= */

typedef struct {
    uint8_t* data;
    size_t len, cap;
} cbor_buf;

static int cb_grow(cbor_buf* b, size_t add) {
    if (add > SIZE_MAX - b->len) return -1;
    size_t need = b->len + add;
    if (need <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < need) {
        if (ncap > SIZE_MAX/2) { ncap = need; break; }
        ncap *= 2;
    }
    uint8_t* p = (uint8_t*)realloc(b->data, ncap);
    if (!p) return -1;
    b->data = p; b->cap = ncap;
    return 0;
}
static int cb_put1(cbor_buf* b, uint8_t v){ if(cb_grow(b,1))return -1; b->data[b->len++]=v; return 0; }
static int cb_putn(cbor_buf* b, const void* p, size_t n){ if(cb_grow(b,n))return -1; memcpy(b->data+b->len,p,n); b->len+=n; return 0; }

void cbor_buf_init(cbor_buf* b){ b->data=NULL; b->len=0; b->cap=0; }
void cbor_buf_free(cbor_buf* b){ free(b->data); b->data=NULL; b->len=b->cap=0; }

static int cb_write_head(cbor_buf* b, uint8_t maj, uint64_t ai) {
    if (ai < 24) { return cb_put1(b, (uint8_t)((maj<<5) | (uint8_t)ai)); }
    else if (ai <= 0xFF)   { if(cb_put1(b,(maj<<5)|24))return -1; uint8_t u=(uint8_t)ai; return cb_put1(b,u); }
    else if (ai <= 0xFFFF) { if(cb_put1(b,(maj<<5)|25))return -1; uint16_t u=(uint16_t)ai; uint8_t be[2]={u>>8,u}; return cb_putn(b,be,2); }
    else if (ai <= 0xFFFFFFFFu) {
        if (cb_put1(b,(maj<<5)|26)) return -1;
        uint32_t u=(uint32_t)ai; uint8_t be[4]={u>>24,u>>16,u>>8,u};
        return cb_putn(b,be,4);
    } else {
        if (cb_put1(b,(maj<<5)|27)) return -1;
        uint64_t u=ai; uint8_t be[8]={u>>56,u>>48,u>>40,u>>32,u>>24,u>>16,u>>8,u};
        return cb_putn(b,be,8);
    }
}

int cbor_write_uint(cbor_buf* b, uint64_t u){ return cb_write_head(b, 0, u); }
int cbor_write_tag(cbor_buf* b, uint64_t tag){ return cb_write_head(b, 6, tag); }

int cbor_write_nint(cbor_buf* b, int64_t i){
    if (i >= 0) return -1;
    uint64_t n = (uint64_t)(-1 - i);
    return cb_write_head(b, 1, n);
}
int cbor_write_bstr(cbor_buf* b, const void* p, size_t n){
    if (cb_write_head(b,2,n)) return -1;
    return cb_putn(b,p,n);
}
int cbor_write_tstr(cbor_buf* b, const char* s, size_t n){
    if (cb_write_head(b,3,n)) return -1;
    return cb_putn(b,s,n);
}
int cbor_write_array(cbor_buf* b, size_t count){ return cb_write_head(b,4,count); }
int cbor_write_array_indef(cbor_buf* b){ return cb_put1(b, (4u<<5)|31u); }
int cbor_write_map(cbor_buf* b, size_t count){ return cb_write_head(b,5,count); }
int cbor_write_map_indef(cbor_buf* b){ return cb_put1(b, (5u<<5)|31u); }
int cbor_write_break(cbor_buf* b){ return cb_put1(b, 0xFF); }
int cbor_write_bool(cbor_buf* b, int v){ return cb_put1(b, (7u<<5)|(v?21u:20u)); }
int cbor_write_null(cbor_buf* b){ return cb_put1(b, (7u<<5)|22u); }
int cbor_write_undef(cbor_buf* b){ return cb_put1(b, (7u<<5)|23u); }
int cbor_write_f64(cbor_buf* b, double x){
    if (cb_put1(b,(7u<<5)|27u)) return -1;
    union { double d; uint64_t u; } u; u.d = x;
    uint8_t be[8]={u.u>>56,u.u>>48,u.u>>40,u.u>>32,u.u>>24,u.u>>16,u.u>>8,u.u};
    return cb_putn(b,be,8);
}

/* ================= DOM types ================= */

typedef enum {
    CBOR_UINT, CBOR_NINT, CBOR_BSTR, CBOR_TSTR,
    CBOR_ARRAY, CBOR_MAP, CBOR_BOOL, CBOR_NULL,
    CBOR_UNDEF, CBOR_FLOAT, CBOR_TAG
} cbor_type;

typedef struct cbor_pair {
    struct cbor_item* k;
    struct cbor_item* v;
} cbor_pair;

typedef struct cbor_item {
    cbor_type t;
    uint64_t tag;      /* 0 = no tag; otherwise semantic tag value */
    union {
        uint64_t  u;
        int64_t   i;
        double    f;
        struct { uint8_t* p; size_t n; } bytes;   /* bstr / tstr */
        struct { struct cbor_item** v; size_t n, cap; } arr;
        struct { cbor_pair* v; size_t n, cap; } map;
        int      b;
    } as;
} cbor_item;

static void* xmalloc(size_t n){ void* p = malloc(n?n:1); if(!p){fprintf(stderr,"cbor: OOM\n"); exit(1);} return p; }
static void* xrealloc(void* p, size_t n){ void* q = realloc(p, n?n:1); if(!q){fprintf(stderr,"cbor: OOM\n"); exit(1);} return q; }

static cbor_item* cbor_new(cbor_type t){
    cbor_item* it = (cbor_item*)xmalloc(sizeof(*it));
    memset(it,0,sizeof(*it)); it->t=t; it->tag=0;
    return it;
}
void cbor_free(cbor_item* it){
    if (!it) return;
    switch (it->t){
        case CBOR_BSTR: case CBOR_TSTR: free(it->as.bytes.p); break;
        case CBOR_ARRAY:
            for (size_t i=0;i<it->as.arr.n;i++) cbor_free(it->as.arr.v[i]);
            free(it->as.arr.v); break;
        case CBOR_MAP:
            for (size_t i=0;i<it->as.map.n;i++){ cbor_free(it->as.map.v[i].k); cbor_free(it->as.map.v[i].v); }
            free(it->as.map.v); break;
        default: break;
    }
    free(it);
}

/* ================= Reader ================= */

typedef struct {
    const uint8_t* p; size_t n, off;
    size_t max_depth, max_items;
    size_t items_seen;
} cbor_reader;

static int rd_u8(cbor_reader* r, uint8_t* out){ if(r->off>=r->n) return -1; *out=r->p[r->off++]; return 0; }
static int rd_bytes(cbor_reader* r, void* dst, size_t n){
    if (n > r->n - r->off) return -1; memcpy(dst, r->p + r->off, n); r->off += n; return 0;
}
static int rd_uint_ai(cbor_reader* r, uint8_t ai, uint64_t* out){
    if (ai < 24) { *out=ai; return 0; }
    if (ai==24){ uint8_t v; if(rd_u8(r,&v))return -1; *out=v; return 0; }
    if (ai==25){ uint8_t b[2]; if(rd_bytes(r,b,2))return -1; *out=((uint64_t)b[0]<<8)|b[1]; return 0; }
    if (ai==26){ uint8_t b[4]; if(rd_bytes(r,b,4))return -1; *out=((uint64_t)b[0]<<24)|((uint64_t)b[1]<<16)|((uint64_t)b[2]<<8)|b[3]; return 0; }
    if (ai==27){ uint8_t b[8]; if(rd_bytes(r,b,8))return -1;
        *out=((uint64_t)b[0]<<56)|((uint64_t)b[1]<<48)|((uint64_t)b[2]<<40)|((uint64_t)b[3]<<32)|
             ((uint64_t)b[4]<<24)|((uint64_t)b[5]<<16)|((uint64_t)b[6]<<8)|b[7]; return 0; }
    return -1; /* ai 28..31 are reserved except 31 for indefinite on heads */
}

static int decode_item(cbor_reader* r, size_t depth, cbor_item** out);

static int decode_string_chunks(cbor_reader* r, int text, uint8_t maj, cbor_item** out, size_t depth){
    /* maj is 2 or 3; we already consumed initial byte with ai=31. */
    cbor_item* it = cbor_new(text?CBOR_TSTR:CBOR_BSTR);
    size_t cap=0, len=0; uint8_t* buf=NULL;
    for(;;){
        if (r->items_seen++ > r->max_items) { free(buf); cbor_free(it); return -1; }
        uint8_t ib; if(rd_u8(r,&ib)) { free(buf); cbor_free(it); return -1; }
        if (ib==0xFF){ /* break */ break; }
        uint8_t m = ib>>5, ai = ib & 31;
        if (m != maj) { free(buf); cbor_free(it); return -1; }
        uint64_t n; if (rd_uint_ai(r, ai, &n)) { free(buf); cbor_free(it); return -1; }
        if (n > SIZE_MAX - len) { free(buf); cbor_free(it); return -1; }
        if (cap < len + n) { size_t ncap = cap?cap:64; while(ncap < len+n) ncap*=2; buf = (uint8_t*)xrealloc(buf, ncap); cap=ncap; }
        if (rd_bytes(r, buf+len, (size_t)n)) { free(buf); cbor_free(it); return -1; }
        len += (size_t)n;
    }
    it->as.bytes.p = buf; it->as.bytes.n = len; *out = it; return 0;
}

static int decode_array_indef(cbor_reader* r, cbor_item** out, size_t depth){
    if (depth >= r->max_depth) return -1;
    cbor_item* it = cbor_new(CBOR_ARRAY);
    for(;;){
        if (r->items_seen++ > r->max_items){ cbor_free(it); return -1; }
        if (r->off >= r->n) { cbor_free(it); return -1; }
        if (r->p[r->off] == 0xFF){ r->off++; break; }
        cbor_item* elem=NULL;
        if (decode_item(r, depth+1, &elem)){ cbor_free(it); return -1; }
        if (it->as.arr.n == it->as.arr.cap){
            size_t nc = it->as.arr.cap? it->as.arr.cap*2 : 4;
            it->as.arr.v = (cbor_item**)xrealloc(it->as.arr.v, nc*sizeof(*it->as.arr.v));
            it->as.arr.cap = nc;
        }
        it->as.arr.v[it->as.arr.n++] = elem;
    }
    *out = it; return 0;
}

static int decode_map_indef(cbor_reader* r, cbor_item** out, size_t depth){
    if (depth >= r->max_depth) return -1;
    cbor_item* it = cbor_new(CBOR_MAP);
    for(;;){
        if (r->items_seen++ > r->max_items){ cbor_free(it); return -1; }
        if (r->off >= r->n) { cbor_free(it); return -1; }
        if (r->p[r->off] == 0xFF){ r->off++; break; }
        cbor_item *k=NULL,*v=NULL;
        if (decode_item(r, depth+1, &k)){ cbor_free(it); return -1; }
        if (decode_item(r, depth+1, &v)){ cbor_free(k); cbor_free(it); return -1; }
        if (it->as.map.n == it->as.map.cap){
            size_t nc = it->as.map.cap? it->as.map.cap*2 : 4;
            it->as.map.v = (cbor_pair*)xrealloc(it->as.map.v, nc*sizeof(*it->as.map.v));
            it->as.map.cap = nc;
        }
        it->as.map.v[it->as.map.n++] = (cbor_pair){k,v};
    }
    *out = it; return 0;
}

static int decode_item(cbor_reader* r, size_t depth, cbor_item** out){
    if (r->items_seen++ > r->max_items) return -1;
    if (depth > r->max_depth) return -1;
    uint8_t ib; if (rd_u8(r,&ib)) return -1;
    uint8_t maj = ib>>5, ai = ib & 31;
    uint64_t val=0;

    if (maj == 0){ /* uint */
        if (rd_uint_ai(r, ai, &val)) return -1;
        cbor_item* it=cbor_new(CBOR_UINT); it->as.u = val; *out=it; return 0;
    }
    if (maj == 1){ /* nint */
        if (rd_uint_ai(r, ai, &val)) return -1;
        cbor_item* it=cbor_new(CBOR_NINT); it->as.i = -(int64_t)val - 1; *out=it; return 0;
    }
    if (maj == 2 || maj == 3){ /* bstr/tstr */
        if (ai == 31) { return decode_string_chunks(r, maj==3, maj, out, depth); }
        if (rd_uint_ai(r, ai, &val)) return -1;
        if (val > SIZE_MAX) return -1;
        uint8_t* buf = (uint8_t*)xmalloc((size_t)val);
        if (rd_bytes(r, buf, (size_t)val)) { free(buf); return -1; }
        cbor_item* it = cbor_new(maj==3?CBOR_TSTR:CBOR_BSTR);
        it->as.bytes.p = buf; it->as.bytes.n = (size_t)val; *out=it; return 0;
    }
    if (maj == 4){ /* array */
        if (ai == 31) return decode_array_indef(r,out,depth);
        if (rd_uint_ai(r, ai, &val)) return -1;
        if (depth >= r->max_depth) return -1;
        cbor_item* it = cbor_new(CBOR_ARRAY);
        for (uint64_t i=0;i<val;i++){
            cbor_item* e=NULL;
            if (decode_item(r, depth+1, &e)){ cbor_free(it); return -1; }
            if (it->as.arr.n == it->as.arr.cap){
                size_t nc = it->as.arr.cap? it->as.arr.cap*2 : 4;
                it->as.arr.v = (cbor_item**)xrealloc(it->as.arr.v, nc*sizeof(*it->as.arr.v));
                it->as.arr.cap = nc;
            }
            it->as.arr.v[it->as.arr.n++] = e;
        }
        *out=it; return 0;
    }
    if (maj == 5){ /* map */
        if (ai == 31) return decode_map_indef(r,out,depth);
        if (rd_uint_ai(r, ai, &val)) return -1;
        if (depth >= r->max_depth) return -1;
        cbor_item* it = cbor_new(CBOR_MAP);
        for (uint64_t i=0;i<val;i++){
            cbor_item *k=NULL,*v=NULL;
            if (decode_item(r, depth+1, &k)){ cbor_free(it); return -1; }
            if (decode_item(r, depth+1, &v)){ cbor_free(k); cbor_free(it); return -1; }
            if (it->as.map.n == it->as.map.cap){
                size_t nc = it->as.map.cap? it->as.map.cap*2 : 4;
                it->as.map.v = (cbor_pair*)xrealloc(it->as.map.v, nc*sizeof(*it->as.map.v));
                it->as.map.cap = nc;
            }
            it->as.map.v[it->as.map.n++] = (cbor_pair){k,v};
        }
        *out=it; return 0;
    }
    if (maj == 6){ /* tag */
        if (rd_uint_ai(r, ai, &val)) return -1;
        cbor_item* tagged=NULL;
        if (decode_item(r, depth+1, &tagged)) return -1;
        tagged->tag = val; *out = tagged; return 0;
    }
    if (maj == 7){
        if (ai == 20 || ai == 21){ cbor_item* it=cbor_new(CBOR_BOOL); it->as.b = (ai==21); *out=it; return 0; }
        if (ai == 22){ *out=cbor_new(CBOR_NULL); return 0; }
        if (ai == 23){ *out=cbor_new(CBOR_UNDEF); return 0; }
        if (ai == 27){ /* float64 */
            uint8_t b[8]; if(rd_bytes(r,b,8)) return -1;
            union { uint64_t u; double d; } u;
            u.u=((uint64_t)b[0]<<56)|((uint64_t)b[1]<<48)|((uint64_t)b[2]<<40)|((uint64_t)b[3]<<32)|
                ((uint64_t)b[4]<<24)|((uint64_t)b[5]<<16)|((uint64_t)b[6]<<8)|b[7];
            cbor_item* it=cbor_new(CBOR_FLOAT); it->as.f=u.d; *out=it; return 0;
        }
        /* simple values 24..31 excluded except break which is handled by callers */
    }
    return -1;
}

int cbor_decode(const void* buf, size_t n, cbor_item** out, size_t max_depth, size_t max_items){
    if (!buf || !out) return -1;
    cbor_reader r = { (const uint8_t*)buf, n, 0, max_depth?max_depth:64, max_items?max_items:100000, 0 };
    cbor_item* it=NULL;
    if (decode_item(&r, 0, &it)) return -1;
    *out = it; return 0;
}

/* ================== JSON-ish pretty printer ================== */

static int json_append(char** s, size_t* n, size_t* cap, const char* src, size_t m){
    if (*n + m + 1 > *cap){
        size_t nc = *cap? *cap : 128;
        while (nc < *n + m + 1) nc*=2;
        *s = (char*)xrealloc(*s, nc);
        *cap = nc;
    }
    memcpy(*s + *n, src, m); *n += m; (*s)[*n]=0; return 0;
}
static void json_puts(char** s,size_t* n,size_t* cap,const char* lit){ json_append(s,n,cap,lit,strlen(lit)); }
static void json_putch(char** s,size_t* n,size_t* cap,char c){ json_append(s,n,cap,&c,1); }
static void json_str_esc(char** s,size_t* n,size_t* cap,const uint8_t* p,size_t m){
    json_putch(s,n,cap,'"');
    for(size_t i=0;i<m;i++){
        unsigned char c=p[i];
        if (c=='"'||c=='\\'){ json_putch(s,n,cap,'\\'); json_putch(s,n,cap,(char)c); }
        else if (c>=0x20 && c!=0x7F){ json_putch(s,n,cap,(char)c); }
        else {
            char buf[7]; snprintf(buf,sizeof buf,"\\u%04X", (unsigned)c);
            json_puts(s,n,cap,buf);
        }
    }
    json_putch(s,n,cap,'"');
}
char* cbor_to_json(const cbor_item* it){
    char* s=NULL; size_t n=0,cap=0;
    if (!it){ json_puts(&s,&n,&cap,"null"); return s; }
    if (it->tag){
        json_puts(&s,&n,&cap,"{\"$tag\":"); char num[32]; snprintf(num,sizeof num,"%llu",(unsigned long long)it->tag);
        json_puts(&s,&n,&cap,num); json_puts(&s,&n,&cap,",\"v\":");
        char* inner=cbor_to_json((const cbor_item*) &(cbor_item){.t=it->t,.tag=0,.as=it->as});
        json_puts(&s,&n,&cap, inner); free(inner); json_putch(&s,&n,&cap,'}');
        return s;
    }
    switch (it->t){
        case CBOR_UINT:{ char buf[32]; snprintf(buf,sizeof buf,"%llu",(unsigned long long)it->as.u); json_puts(&s,&n,&cap,buf); break; }
        case CBOR_NINT:{ char buf[32]; snprintf(buf,sizeof buf,"%lld",(long long)it->as.i); json_puts(&s,&n,&cap,buf); break; }
        case CBOR_FLOAT:{ char buf[64]; snprintf(buf,sizeof buf,"%.17g", it->as.f); json_puts(&s,&n,&cap,buf); break; }
        case CBOR_BOOL: json_puts(&s,&n,&cap, it->as.b?"true":"false"); break;
        case CBOR_NULL: json_puts(&s,&n,&cap,"null"); break;
        case CBOR_UNDEF: json_puts(&s,&n,&cap,"\"undefined\""); break;
        case CBOR_TSTR: json_str_esc(&s,&n,&cap, it->as.bytes.p, it->as.bytes.n); break;
        case CBOR_BSTR:{
            json_puts(&s,&n,&cap,"{\"bstr\":\"");
            static const char HEX[]="0123456789abcdef";
            for (size_t i=0;i<it->as.bytes.n;i++){ char h[2]={HEX[it->as.bytes.p[i]>>4],HEX[it->as.bytes.p[i]&15]}; json_append(&s,&n,&cap,h,2); }
            json_puts(&s,&n,&cap,"\"}"); break;
        }
        case CBOR_ARRAY:{
            json_putch(&s,&n,&cap,'[');
            for(size_t i=0;i<it->as.arr.n;i++){
                if(i) json_putch(&s,&n,&cap,',');
                char* sub=cbor_to_json(it->as.arr.v[i]); json_puts(&s,&n,&cap,sub); free(sub);
            }
            json_putch(&s,&n,&cap,']'); break;
        }
        case CBOR_MAP:{
            json_putch(&s,&n,&cap,'{');
            for(size_t i=0;i<it->as.map.n;i++){
                if(i) json_putch(&s,&n,&cap,',');
                /* keys can be any CBOR; render JSON of key as string */
                char* ks=cbor_to_json(it->as.map.v[i].k);
                json_putch(&s,&n,&cap,'"'); json_puts(&s,&n,&cap,ks); json_putch(&s,&n,&cap,'"'); free(ks);
                json_putch(&s,&n,&cap,':');
                char* vs=cbor_to_json(it->as.map.v[i].v); json_puts(&s,&n,&cap,vs); free(vs);
            }
            json_putch(&s,&n,&cap,'}'); break;
        }
        default: json_puts(&s,&n,&cap,"null"); break;
    }
    return s;
}

/* ================= Example usage / self-test ================= */
#ifdef CBOR_TEST
int main(void){
    cbor_buf b; cbor_buf_init(&b);
    /* Encode: { "hello": "world", "n": 42, "arr": [true, null, -5], "pi": 3.14 } */
    cbor_write_map(&b, 4);
      cbor_write_tstr(&b,"hello",5); cbor_write_tstr(&b,"world",5);
      cbor_write_tstr(&b,"n",1);     cbor_write_uint(&b,42);
      cbor_write_tstr(&b,"arr",3);
        cbor_write_array(&b,3); cbor_write_bool(&b,1); cbor_write_null(&b); cbor_write_nint(&b,-5);
      cbor_write_tstr(&b,"pi",2);    cbor_write_f64(&b,3.14);

    cbor_item* root=NULL;
    if (cbor_decode(b.data, b.len, &root, 64, 10000) != 0){
        fprintf(stderr,"decode error\n"); cbor_buf_free(&b); return 1;
    }
    char* js = cbor_to_json(root);
    printf("%s\n", js);
    free(js);
    cbor_free(root);
    cbor_buf_free(&b);
    return 0;
}
#endif