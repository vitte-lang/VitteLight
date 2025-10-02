/* ============================================================================
   endian.h — Endianness helpers (C11, portable)
   - Détection compile-time (VT_LITTLE_ENDIAN / VT_BIG_ENDIAN)
   - Fonctions de swap 16/32/64 bits
   - Fonctions de conversion host <-> little/big endian
   Licence: MIT
   ============================================================================
*/
#ifndef VT_ENDIAN_H
#define VT_ENDIAN_H
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Détection compile-time
---------------------------------------------------------------------------- */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
#  if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#    define VT_LITTLE_ENDIAN 1
#    define VT_BIG_ENDIAN    0
#  elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#    define VT_LITTLE_ENDIAN 0
#    define VT_BIG_ENDIAN    1
#  else
#    error "Unknown __BYTE_ORDER__"
#  endif
#elif defined(_WIN32)
/* Windows est toujours little endian */
#  define VT_LITTLE_ENDIAN 1
#  define VT_BIG_ENDIAN    0
#else
/* fallback runtime (rare) */
#  define VT_LITTLE_ENDIAN 1
#  define VT_BIG_ENDIAN    0
#endif

/* ----------------------------------------------------------------------------
   API
---------------------------------------------------------------------------- */
#ifndef VT_ENDIAN_API
#define VT_ENDIAN_API extern
#endif

/* byte swap */
VT_ENDIAN_API uint16_t vt_bswap16(uint16_t x);
VT_ENDIAN_API uint32_t vt_bswap32(uint32_t x);
VT_ENDIAN_API uint64_t vt_bswap64(uint64_t x);

/* host -> little endian */
VT_ENDIAN_API uint16_t vt_htole16(uint16_t x);
VT_ENDIAN_API uint32_t vt_htole32(uint32_t x);
VT_ENDIAN_API uint64_t vt_htole64(uint64_t x);

/* host -> big endian */
VT_ENDIAN_API uint16_t vt_htobe16(uint16_t x);
VT_ENDIAN_API uint32_t vt_htobe32(uint32_t x);
VT_ENDIAN_API uint64_t vt_htobe64(uint64_t x);

/* little endian -> host */
VT_ENDIAN_API uint16_t vt_letoh16(uint16_t x);
VT_ENDIAN_API uint32_t vt_letoh32(uint32_t x);
VT_ENDIAN_API uint64_t vt_letoh64(uint64_t x);

/* big endian -> host */
VT_ENDIAN_API uint16_t vt_betoh16(uint16_t x);
VT_ENDIAN_API uint32_t vt_betoh32(uint32_t x);
VT_ENDIAN_API uint64_t vt_betoh64(uint64_t x);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_ENDIAN_H */
