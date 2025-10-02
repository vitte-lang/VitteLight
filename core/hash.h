/* ============================================================================
   hash.h — API de hachage portable (C17, licence MIT)
   - FNV-1a 32/64
   - MurmurHash3 (x86_32)
   - CRC32
   - SHA-256 (streaming et one-shot)
   Lier avec hash.c
   ============================================================================
*/
#ifndef VT_HASH_H
#define VT_HASH_H
#pragma once

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint32_t, uint64_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VT_HASH_API
#define VT_HASH_API extern
#endif

/* ----------------------------------------------------------------------------
   FNV-1a
---------------------------------------------------------------------------- */
VT_HASH_API uint32_t vt_fnv1a32(const void* data, size_t len, uint32_t seed);
VT_HASH_API uint64_t vt_fnv1a64(const void* data, size_t len, uint64_t seed);

/* ----------------------------------------------------------------------------
   MurmurHash3 (x86_32)
---------------------------------------------------------------------------- */
VT_HASH_API uint32_t vt_murmur3_32(const void* data, size_t len, uint32_t seed);

/* ----------------------------------------------------------------------------
   CRC32
---------------------------------------------------------------------------- */
VT_HASH_API uint32_t vt_crc32(const void* data, size_t len, uint32_t seed);

/* ----------------------------------------------------------------------------
   SHA-256
---------------------------------------------------------------------------- */
typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  size_t buffer_len;
} vt_sha256_ctx;

VT_HASH_API void vt_sha256_init(vt_sha256_ctx* ctx);
VT_HASH_API void vt_sha256_update(vt_sha256_ctx* ctx, const void* data, size_t len);
VT_HASH_API void vt_sha256_final(vt_sha256_ctx* ctx, uint8_t out[32]);

/* one-shot */
VT_HASH_API void vt_sha256(const void* data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_HASH_H */
