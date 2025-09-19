// SPDX-License-Identifier: GPL-3.0-or-later
//
// uuid.c — UUID v4/v5 utilitaires portables (C17)
// Namespace: "uuid"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c uuid.c
//
// API:
//   typedef struct { unsigned char b[16]; } uuid_t;
//   void   uuid_nil(uuid_t* u);
//   int    uuid_is_nil(const uuid_t* u);
//   int    uuid_parse(const char* s, uuid_t* out);          // 0 ok
//   int    uuid_unparse(const uuid_t* u, char out[37]);     // 0 ok
//   int    uuid_cmp(const uuid_t* a, const uuid_t* b);      // memcmp order
//   int    uuid_v4(uuid_t* out);                            // crypto RNG
//   int    uuid_v5(uuid_t* out, const uuid_t* ns, const void* name, size_t n);

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>   /* FILE*, fopen, fread, fclose on POSIX */

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <wincrypt.h>
#endif

typedef struct { unsigned char b[16]; } uuid_t;

#ifndef UUID_API
#define UUID_API
#endif

/* ===== RNG ===== */
static int uuid__randbytes(unsigned char* p, size_t n){
#if defined(_WIN32)
  HCRYPTPROV prov=0;
  if (CryptAcquireContextA(&prov,NULL,NULL,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT)){
    BOOL ok = CryptGenRandom(prov,(DWORD)n,(BYTE*)p);
    CryptReleaseContext(prov,0);
    return ok?0:-1;
  }
  return -1;
#else
  FILE* f = fopen("/dev/urandom","rb");
  if (!f) return -1;
  size_t r = fread(p,1,n,f);
  fclose(f);
  return r==n?0:-1;
#endif
}

/* ===== utils ===== */
UUID_API void uuid_nil(uuid_t* u){ if(u) memset(u->b,0,16); }
UUID_API int uuid_is_nil(const uuid_t* u){
    if(!u) return 1; for (int i=0;i<16;i++) if (u->b[i]) return 0; return 1;
}
UUID_API int uuid_cmp(const uuid_t* a, const uuid_t* b){
    return memcmp(a->b,b->b,16);
}

static int hexval(int c){
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
static void hex2(unsigned char v, char out[2]){
    static const char hx[]="0123456789abcdef";
    out[0]=hx[(v>>4)&15]; out[1]=hx[v&15];
}

UUID_API int uuid_parse(const char* s, uuid_t* out){
    if(!s||!out) return -1;
    char buf[32]; int j=0;
    for (const char* p=s; *p && j<32; p++){
        if (*p=='-'||*p=='{'||*p=='}') continue;
        int h=hexval(*p); if(h<0) return -1;
        p++; if(!*p) return -1;
        int l=hexval(*p); if(l<0) return -1;
        buf[j++] = (char)((h<<4)|l);
    }
    if (j!=16) return -1;
    memcpy(out->b, buf, 16);
    return 0;
}
UUID_API int uuid_unparse(const uuid_t* u, char out[37]){
    if(!u||!out) return -1;
    const int dash[4]={4,6,8,10};
    int di=0; size_t w=0;
    for (int i=0;i<16;i++){
        if (di<4 && i==dash[di]){ out[w++]='-'; di++; }
        char hx[2]; hex2(u->b[i],hx); out[w++]=hx[0]; out[w++]=hx[1];
    }
    out[w]=0; return 0;
}

/* ===== v4 ===== */
UUID_API int uuid_v4(uuid_t* out){
    if(!out) return -1;
    if (uuid__randbytes(out->b,16)!=0) return -1;
    out->b[6] = (unsigned char)((out->b[6] & 0x0F) | 0x40);
    out->b[8] = (unsigned char)((out->b[8] & 0x3F) | 0x80);
    return 0;
}

/* ===== SHA-1 minimal ===== */
typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; size_t off; } sha1_t;
static uint32_t rol(uint32_t x, int s){ return (x<<s) | (x>>(32-s)); }
static void sha1_init(sha1_t* s){ s->h[0]=0x67452301u; s->h[1]=0xEFCDAB89u; s->h[2]=0x98BADCFEu; s->h[3]=0x10325476u; s->h[4]=0xC3D2E1F0u; s->len=0; s->off=0; }
static void sha1_block(sha1_t* s, const unsigned char blk[64]){
    uint32_t w[80];
    for(int i=0;i<16;i++) w[i] = ((uint32_t)blk[4*i]<<24)|((uint32_t)blk[4*i+1]<<16)|((uint32_t)blk[4*i+2]<<8)|((uint32_t)blk[4*i+3]);
    for(int i=16;i<80;i++) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
    for(int i=0;i<80;i++){
        uint32_t f,k;
        if(i<20){ f=(b&c)|((~b)&d); k=0x5A827999u; }
        else if(i<40){ f=b^c^d; k=0x6ED9EBA1u; }
        else if(i<60){ f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDCu; }
        else { f=b^c^d; k=0xCA62C1D6u; }
        uint32_t temp = rol(a,5) + f + e + k + w[i];
        e=d; d=c; c=rol(b,30); b=a; a=temp;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static void sha1_update(sha1_t* s, const void* data, size_t n){
    const unsigned char* p=(const unsigned char*)data;
    s->len += (uint64_t)n*8ull;
    while(n){
        size_t take = 64 - s->off; if (take>n) take=n;
        memcpy(s->buf + s->off, p, take);
        s->off += take; p += take; n -= take;
        if (s->off==64){ sha1_block(s, s->buf); s->off=0; }
    }
}
static void sha1_final(sha1_t* s, unsigned char out[20]){
    unsigned char pad[64]={0x80};
    size_t padlen = (s->off<56)? (56 - s->off) : (56 + 64 - s->off);
    sha1_update(s, pad, padlen);
    unsigned char lenb[8];
    for(int i=0;i<8;i++) lenb[7-i]=(unsigned char)((s->len>>(8*i))&0xFFu);
    sha1_update(s, lenb, 8);
    for(int i=0;i<5;i++){
        out[4*i  ]=(unsigned char)(s->h[i]>>24);
        out[4*i+1]=(unsigned char)(s->h[i]>>16);
        out[4*i+2]=(unsigned char)(s->h[i]>>8);
        out[4*i+3]=(unsigned char)(s->h[i]);
    }
}

/* ===== v5 ===== */
UUID_API int uuid_v5(uuid_t* out, const uuid_t* ns, const void* name, size_t n){
    if(!out||!ns||(!name&&n)) return -1;
    sha1_t sh; sha1_init(&sh);
    sha1_update(&sh, ns->b, 16);
    if (n) sha1_update(&sh, name, n);
    unsigned char dig[20]; sha1_final(&sh, dig);
    memcpy(out->b, dig, 16);
    out->b[6] = (unsigned char)((out->b[6] & 0x0F) | 0x50);
    out->b[8] = (unsigned char)((out->b[8] & 0x3F) | 0x80);
    return 0;
}

/* ===== Tests ===== */
#ifdef UUID_TEST
int main(void){
    uuid_t u4; uuid_v4(&u4); char s[37]; uuid_unparse(&u4,s); printf("v4: %s\n", s);
    uuid_t ns_dns; uuid_parse("6ba7b810-9dad-11d1-80b4-00c04fd430c8",&ns_dns);
    uuid_t u5; uuid_v5(&u5,&ns_dns,"example.org",11); uuid_unparse(&u5,s); printf("v5: %s\n", s);
    return 0;
}
#endif