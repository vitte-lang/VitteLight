/* ============================================================================
   hash.c — Fonctions de hachage portables (C17, licence MIT)
   - FNV-1a 32/64 (rapide, simple)
   - MurmurHash3 x86_32 (non cryptographique, très diffusant)
   - CRC32 (IEEE 802.3, poly 0xEDB88320), one-shot et streaming
   - SHA-256 (cryptographique), one-shot et streaming
   - API autonome si hash.h absent
   ============================================================================
*/
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------------------------
   API publique (si hash.h absent)
---------------------------------------------------------------------------- */
#ifndef VT_HASH_HAVE_HEADER
#ifndef VT_HASH_API
#define VT_HASH_API extern
#endif

VT_HASH_API uint32_t vt_fnv1a32(const void* data, size_t len, uint32_t seed);
VT_HASH_API uint64_t vt_fnv1a64(const void* data, size_t len, uint64_t seed);

VT_HASH_API uint32_t vt_murmur3_32(const void* data, size_t len, uint32_t seed);

VT_HASH_API uint32_t vt_crc32(const void* data, size_t len, uint32_t seed);

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  size_t buffer_len;
} vt_sha256_ctx;

VT_HASH_API void vt_sha256_init(vt_sha256_ctx* ctx);
VT_HASH_API void vt_sha256_update(vt_sha256_ctx* ctx, const void* data, size_t len);
VT_HASH_API void vt_sha256_final(vt_sha256_ctx* ctx, uint8_t out[32]);
VT_HASH_API void vt_sha256(const void* data, size_t len, uint8_t out[32]);

#endif /* VT_HASH_HAVE_HEADER */

/* ----------------------------------------------------------------------------
   FNV-1a
---------------------------------------------------------------------------- */
uint32_t vt_fnv1a32(const void* data, size_t len, uint32_t seed) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t h = seed ? seed : 2166136261u;
  for (size_t i=0;i<len;i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}
uint64_t vt_fnv1a64(const void* data, size_t len, uint64_t seed) {
  const uint8_t* p = (const uint8_t*)data;
  uint64_t h = seed ? seed : 1469598103934665603ULL;
  for (size_t i=0;i<len;i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/* ----------------------------------------------------------------------------
   MurmurHash3 x86_32 (Austin Appleby, public domain)
---------------------------------------------------------------------------- */
uint32_t vt_murmur3_32(const void* key, size_t len, uint32_t seed) {
  const uint8_t* data = (const uint8_t*)key;
  const int nblocks = (int)(len/4);
  uint32_t h1 = seed;
  const uint32_t c1 = 0xcc9e2d51;
  const uint32_t c2 = 0x1b873593;

  /* body */
  const uint32_t* blocks = (const uint32_t*)(data + nblocks*4);
  for (int i=-nblocks; i; i++) {
    uint32_t k1 = blocks[i];
    k1 *= c1; k1 = (k1 << 15) | (k1 >> 17); k1 *= c2;
    h1 ^= k1;
    h1 = (h1 << 13) | (h1 >> 19);
    h1 = h1*5+0xe6546b64;
  }

  /* tail */
  const uint8_t* tail = (const uint8_t*)(data + nblocks*4);
  uint32_t k1 = 0;
  switch(len & 3) {
    case 3: k1 ^= tail[2] << 16;
    case 2: k1 ^= tail[1] << 8;
    case 1: k1 ^= tail[0];
            k1 *= c1; k1 = (k1 << 15)|(k1 >> 17); k1 *= c2; h1 ^= k1;
  }

  /* finalization */
  h1 ^= (uint32_t)len;
  h1 ^= h1 >> 16;
  h1 *= 0x85ebca6b;
  h1 ^= h1 >> 13;
  h1 *= 0xc2b2ae35;
  h1 ^= h1 >> 16;

  return h1;
}

/* ----------------------------------------------------------------------------
   CRC32 (IEEE)
---------------------------------------------------------------------------- */
static uint32_t vt__crc32_table[256];
static int vt__crc32_inited=0;

static void vt__crc32_init(void) {
  if (vt__crc32_inited) return;
  for (uint32_t i=0;i<256;i++) {
    uint32_t c=i;
    for (int j=0;j<8;j++) {
      if (c&1) c=0xEDB88320u^(c>>1);
      else c >>=1;
    }
    vt__crc32_table[i]=c;
  }
  vt__crc32_inited=1;
}
uint32_t vt_crc32(const void* data, size_t len, uint32_t seed) {
  vt__crc32_init();
  const uint8_t* p=(const uint8_t*)data;
  uint32_t c = ~seed;
  for (size_t i=0;i<len;i++) {
    c = vt__crc32_table[(c^p[i])&0xFF] ^ (c>>8);
  }
  return ~c;
}

/* ----------------------------------------------------------------------------
   SHA-256
---------------------------------------------------------------------------- */
#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y)) ^ (~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y)) ^ ((x)&(z)) ^ ((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static const uint32_t K[64]={
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

void vt_sha256_init(vt_sha256_ctx* ctx) {
  ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
  ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
  ctx->bitlen=0; ctx->buffer_len=0;
}
static void vt__sha256_transform(vt_sha256_ctx* ctx,const uint8_t data[64]){
  uint32_t m[64];
  for(int i=0;i<16;i++){
    m[i] = (uint32_t)data[i*4]<<24 | (uint32_t)data[i*4+1]<<16 | (uint32_t)data[i*4+2]<<8 | (uint32_t)data[i*4+3];
  }
  for(int i=16;i<64;i++) m[i] = SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
  uint32_t a=ctx->state[0],b=ctx->state[1],c=ctx->state[2],d=ctx->state[3],e=ctx->state[4],f=ctx->state[5],g=ctx->state[6],h=ctx->state[7];
  for(int i=0;i<64;i++){
    uint32_t t1=h+EP1(e)+CH(e,f,g)+K[i]+m[i];
    uint32_t t2=EP0(a)+MAJ(a,b,c);
    h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
  }
  ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
  ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}
void vt_sha256_update(vt_sha256_ctx* ctx, const void* dat, size_t len) {
  const uint8_t* data=(const uint8_t*)dat;
  for(size_t i=0;i<len;i++){
    ctx->buffer[ctx->buffer_len++]=data[i];
    if(ctx->buffer_len==64){
      vt__sha256_transform(ctx,ctx->buffer);
      ctx->bitlen+=512;
      ctx->buffer_len=0;
    }
  }
}
void vt_sha256_final(vt_sha256_ctx* ctx, uint8_t out[32]){
  uint64_t bitlen = ctx->bitlen + ctx->buffer_len*8;
  size_t i=ctx->buffer_len;
  ctx->buffer[i++]=0x80;
  if(i>56){
    while(i<64) ctx->buffer[i++]=0;
    vt__sha256_transform(ctx,ctx->buffer);
    i=0;
  }
  while(i<56) ctx->buffer[i++]=0;
  for(int j=7;j>=0;j--) ctx->buffer[i++]=(uint8_t)(bitlen>>(j*8));
  vt__sha256_transform(ctx,ctx->buffer);
  for(int j=0;j<8;j++){
    out[j*4]=(uint8_t)(ctx->state[j]>>24);
    out[j*4+1]=(uint8_t)(ctx->state[j]>>16);
    out[j*4+2]=(uint8_t)(ctx->state[j]>>8);
    out[j*4+3]=(uint8_t)(ctx->state[j]);
  }
}
void vt_sha256(const void* data, size_t len, uint8_t out[32]){
  vt_sha256_ctx ctx; vt_sha256_init(&ctx); vt_sha256_update(&ctx,data,len); vt_sha256_final(&ctx,out);
}
