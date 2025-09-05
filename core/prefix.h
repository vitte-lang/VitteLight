// vitte-light/core/prefix.h
// Préfixe utilitaire commun: détection compilateur/plateforme, attributs,
// visibilités, helpers d'alignement, probas de branchement, static assert,
// min/max/clamp, endianness et bswap, extern C, etc. Inclus par la plupart des
// .c/.h du runtime.

#ifndef VITTE_LIGHT_CORE_PREFIX_H
#define VITTE_LIGHT_CORE_PREFIX_H

// ───────────────────── Standard & détection compilateur ─────────────────────
#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#if !defined(_MSC_VER)
#error "C99 requis pour VitteLight"
#endif
#endif

#if defined(__clang__)
#define VL_CC_CLANG 1
#elif defined(__GNUC__)
#define VL_CC_GCC 1
#elif defined(_MSC_VER)
#define VL_CC_MSVC 1
#else
#define VL_CC_UNKNOWN 1
#endif

// ───────────────────── Portabilité C / C++ ─────────────────────
#ifdef __cplusplus
#define VL_BEGIN_C extern "C" {
#define VL_END_C }
#else
#define VL_BEGIN_C
#define VL_END_C
#endif

// ───────────────────── Visibilité / API export ─────────────────────
#if defined(_WIN32) || defined(_WIN64)
#ifdef VL_BUILDING_DLL
#define VL_EXPORT __declspec(dllexport)
#else
#define VL_EXPORT __declspec(dllimport)
#endif
#define VL_LOCAL
#else
#if defined(VL_CC_GCC) || defined(VL_CC_CLANG)
#define VL_EXPORT __attribute__((visibility("default")))
#define VL_LOCAL __attribute__((visibility("hidden")))
#else
#define VL_EXPORT
#define VL_LOCAL
#endif
#endif
#ifndef VL_API
#define VL_API VL_EXPORT
#endif

// ───────────────────── Attributs et hints ─────────────────────
#if defined(VL_CC_GCC) || defined(VL_CC_CLANG)
#define VL_LIKELY(x) __builtin_expect(!!(x), 1)
#define VL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define VL_ALWAYS_INLINE __attribute__((always_inline)) inline
#define VL_NEVER_INLINE __attribute__((noinline))
#define VL_PURE __attribute__((pure))
#define VL_CONSTFN __attribute__((const))
#define VL_MALLOC_FN __attribute__((malloc))
#define VL_MAYBE_UNUSED __attribute__((unused))
#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define VL_FALLTHROUGH __attribute__((fallthrough))
#else
#define VL_FALLTHROUGH ((void)0)
#endif
#else
#define VL_FALLTHROUGH ((void)0)
#endif
#define VL_UNREACHABLE() __builtin_unreachable()
#define VL_ASSUME(x)                   \
  do {                                 \
    if (!(x)) __builtin_unreachable(); \
  } while (0)
#define VL_ATTR_PRINTF(fmtidx, varidx) \
  __attribute__((format(printf, fmtidx, varidx)))
#else
#define VL_LIKELY(x) (x)
#define VL_UNLIKELY(x) (x)
#ifdef VL_CC_MSVC
#define VL_ALWAYS_INLINE __forceinline
#define VL_NEVER_INLINE __declspec(noinline)
#define VL_PURE
#define VL_CONSTFN
#define VL_MALLOC_FN
#define VL_MAYBE_UNUSED
#define VL_FALLTHROUGH __fallthrough
#define VL_UNREACHABLE() __assume(0)
#define VL_ASSUME(x) __assume(x)
#define VL_ATTR_PRINTF(fmtidx, varidx)
#else
#define VL_ALWAYS_INLINE inline
#define VL_NEVER_INLINE
#define VL_PURE
#define VL_CONSTFN
#define VL_MALLOC_FN
#define VL_MAYBE_UNUSED
#define VL_FALLTHROUGH ((void)0)
#define VL_UNREACHABLE() \
  do {                   \
  } while (0)
#define VL_ASSUME(x) \
  do {               \
    (void)sizeof(x); \
  } while (0)
#define VL_ATTR_PRINTF(fmtidx, varidx)
#endif
#endif

// ───────────────────── Alignement / packed ─────────────────────
#if defined(VL_CC_GCC) || defined(VL_CC_CLANG)
#define VL_ALIGNAS(n) __attribute__((aligned(n)))
#define VL_ALIGNOF(t) __alignof__(t)
#define VL_PACKED __attribute__((packed))
#define VL_ASSUME_ALIGNED(p, n) ((void*)__builtin_assume_aligned((p), (n)))
#else
#define VL_ALIGNAS(n) __declspec(align(n))
#define VL_ALIGNOF(t) __alignof(t)
#define VL_PACKED
#define VL_ASSUME_ALIGNED(p, n) (p)
#endif

// ───────────────────── Utils génériques ─────────────────────
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VL_STRINGIFY(x) #x
#define VL_TOSTR(x) VL_STRINGIFY(x)

#define VL_CONCAT_(a, b) a##b
#define VL_CONCAT(a, b) VL_CONCAT_(a, b)

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define VL_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define VL_STATIC_ASSERT(cond, msg) \
  typedef char VL_CONCAT(vl_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif

#ifndef VL_MIN
#define VL_MIN(a, b) ((a) <= (b) ? (a) : (b))
#endif
#ifndef VL_MAX
#define VL_MAX(a, b) ((a) >= (b) ? (a) : (b))
#endif
#ifndef VL_CLAMP
#define VL_CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))
#endif

#ifndef VL_ARRAY_LEN
#define VL_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef VL_UNUSED
#define VL_UNUSED(x) (void)(x)
#endif

#ifndef VL_ZERO_STRUCT
#define VL_ZERO_STRUCT(x) memset(&(x), 0, sizeof(x))
#endif

// ───────────────────── Endianness + bswap ─────────────────────
#ifndef VL_ENDIAN_LITTLE
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define VL_ENDIAN_LITTLE 1
#elif defined(_WIN32)
#define VL_ENDIAN_LITTLE 1
#else
#define VL_ENDIAN_LITTLE 0
#endif
#endif

static VL_ALWAYS_INLINE uint16_t vl_bswap16(uint16_t x) {
#if defined(__builtin_bswap16) || \
    (defined(VL_CC_CLANG) && __has_builtin(__builtin_bswap16))
  return __builtin_bswap16(x);
#else
  return (uint16_t)((x << 8) | (x >> 8));
#endif
}
static VL_ALWAYS_INLINE uint32_t vl_bswap32(uint32_t x) {
#if defined(__builtin_bswap32) || \
    (defined(VL_CC_CLANG) && __has_builtin(__builtin_bswap32))
  return __builtin_bswap32(x);
#else
  return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
         ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
#endif
}
static VL_ALWAYS_INLINE uint64_t vl_bswap64(uint64_t x) {
#if defined(__builtin_bswap64) || \
    (defined(VL_CC_CLANG) && __has_builtin(__builtin_bswap64))
  return __builtin_bswap64(x);
#else
  return ((uint64_t)vl_bswap32((uint32_t)x) << 32) |
         (uint64_t)vl_bswap32((uint32_t)(x >> 32));
#endif
}

static VL_ALWAYS_INLINE uint16_t vl_le16(uint16_t x) {
  return VL_ENDIAN_LITTLE ? x : vl_bswap16(x);
}
static VL_ALWAYS_INLINE uint32_t vl_le32(uint32_t x) {
  return VL_ENDIAN_LITTLE ? x : vl_bswap32(x);
}
static VL_ALWAYS_INLINE uint64_t vl_le64(uint64_t x) {
  return VL_ENDIAN_LITTLE ? x : vl_bswap64(x);
}
static VL_ALWAYS_INLINE uint16_t vl_be16(uint16_t x) {
  return VL_ENDIAN_LITTLE ? vl_bswap16(x) : x;
}
static VL_ALWAYS_INLINE uint32_t vl_be32(uint32_t x) {
  return VL_ENDIAN_LITTLE ? vl_bswap32(x) : x;
}
static VL_ALWAYS_INLINE uint64_t vl_be64(uint64_t x) {
  return VL_ENDIAN_LITTLE ? vl_bswap64(x) : x;
}

// ───────────────────── Printf-format helper ─────────────────────
#ifndef VL_ATTR_PRINTF
#define VL_ATTR_PRINTF(fmtidx, varidx)
#endif

// ───────────────────── Mode debug ─────────────────────
#ifndef NDEBUG
#define VL_DEBUG 1
#else
#define VL_DEBUG 0
#endif

#endif  // VITTE_LIGHT_CORE_PREFIX_H

