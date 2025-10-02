/* ============================================================================
   endian.c — Détection et conversions endianness (C11, portable)
   - Détection compile-time : macros VT_LITTLE_ENDIAN / VT_BIG_ENDIAN
   - Fonctions de swap 16/32/64 bits
   - Fonctions de conversion htole/htobe/letoh/betoh
   - Basé sur <endian.h>, <sys/endian.h>, ou implémentation manuelle
   Licence : MIT
   ============================================================================
*/
#include <stdint.h>
#include <stddef.h>

/* ----------------------------------------------------------------------------
   Détection endianness compile-time
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
/* Fallback runtime test */
static int vt__is_le_runtime(void) {
  union { uint32_t u; uint8_t b[4]; } x = { 0x01020304u };
  return x.b[0] == 0x04;
}
#  define VT_LITTLE_ENDIAN (vt__is_le_runtime())
#  define VT_BIG_ENDIAN    (!VT_LITTLE_ENDIAN)
#endif

/* ----------------------------------------------------------------------------
   Fonctions de byte swap
---------------------------------------------------------------------------- */
static inline uint16_t vt_bswap16(uint16_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap16(x);
#else
  return (uint16_t)((x << 8) | (x >> 8));
#endif
}
static inline uint32_t vt_bswap32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap32(x);
#else
  return ((x & 0x000000FFu) << 24) |
         ((x & 0x0000FF00u) << 8)  |
         ((x & 0x00FF0000u) >> 8)  |
         ((x & 0xFF000000u) >> 24);
#endif
}
static inline uint64_t vt_bswap64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap64(x);
#else
  return ((x & 0x00000000000000FFULL) << 56) |
         ((x & 0x000000000000FF00ULL) << 40) |
         ((x & 0x0000000000FF0000ULL) << 24) |
         ((x & 0x00000000FF000000ULL) << 8)  |
         ((x & 0x000000FF00000000ULL) >> 8)  |
         ((x & 0x0000FF0000000000ULL) >> 24) |
         ((x & 0x00FF000000000000ULL) >> 40) |
         ((x & 0xFF00000000000000ULL) >> 56);
#endif
}

/* ----------------------------------------------------------------------------
   Conversion host <-> little/big endian
---------------------------------------------------------------------------- */
static inline uint16_t vt_htole16(uint16_t x) {
#if VT_LITTLE_ENDIAN
  return x;
#else
  return vt_bswap16(x);
#endif
}
static inline uint32_t vt_htole32(uint32_t x) {
#if VT_LITTLE_ENDIAN
  return x;
#else
  return vt_bswap32(x);
#endif
}
static inline uint64_t vt_htole64(uint64_t x) {
#if VT_LITTLE_ENDIAN
  return x;
#else
  return vt_bswap64(x);
#endif
}
static inline uint16_t vt_htobe16(uint16_t x) {
#if VT_BIG_ENDIAN
  return x;
#else
  return vt_bswap16(x);
#endif
}
static inline uint32_t vt_htobe32(uint32_t x) {
#if VT_BIG_ENDIAN
  return x;
#else
  return vt_bswap32(x);
#endif
}
static inline uint64_t vt_htobe64(uint64_t x) {
#if VT_BIG_ENDIAN
  return x;
#else
  return vt_bswap64(x);
#endif
}
static inline uint16_t vt_letoh16(uint16_t x) { return vt_htole16(x); }
static inline uint32_t vt_letoh32(uint32_t x) { return vt_htole32(x); }
static inline uint64_t vt_letoh64(uint64_t x) { return vt_htole64(x); }
static inline uint16_t vt_betoh16(uint16_t x) { return vt_htobe16(x); }
static inline uint32_t vt_betoh32(uint32_t x) { return vt_htobe32(x); }
static inline uint64_t vt_betoh64(uint64_t x) { return vt_htobe64(x); }

/* ----------------------------------------------------------------------------
   Test (désactivé)
   cc -std=c11 -DVT_ENDIAN_TEST endian.c
---------------------------------------------------------------------------- */
#ifdef VT_ENDIAN_TEST
#include <stdio.h>
int main(void) {
  printf("Endian: %s\n", VT_LITTLE_ENDIAN ? "little" : "big");
  printf("bswap16(0x1234)=0x%04x\n", vt_bswap16(0x1234));
  printf("bswap32(0x12345678)=0x%08x\n", vt_bswap32(0x12345678));
  printf("bswap64(0x1122334455667788)=0x%016llx\n",
         (unsigned long long)vt_bswap64(0x1122334455667788ULL));

  uint32_t v = 0xAABBCCDDu;
  printf("htole32=0x%08x, htobe32=0x%08x\n", vt_htole32(v), vt_htobe32(v));
  printf("letoh32=0x%08x, betoh32=0x%08x\n", vt_letoh32(v), vt_betoh32(v));
  return 0;
}
#endif
