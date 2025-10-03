// SPDX-License-Identifier: MIT
/* ============================================================================
   core/attrs.c — Utilitaires d’attributs et de plate-forme (C11, portable)
   Implémente l’API prévue par core/attrs.h (détection compilateur/OS/arch,
   helpers de prédiction de branche, unreachable, prefetch, cacheline, etc.).
   Dépendances : libc uniquement.
   ============================================================================
 */

#if defined(_MSC_VER)
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "attrs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <intrin.h>
#elif defined(__APPLE__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <unistd.h>
#elif defined(__linux__)
#  include <unistd.h>
#  include <sys/auxv.h>
#  ifndef AT_HWCAP
#    define AT_HWCAP 16
#  endif
#  ifndef AT_HWCAP2
#    define AT_HWCAP2 26
#  endif
#endif

/* -------------------------------------------------------------------------- */
/* Fallbacks si attrs.h n’a pas défini certains symboles                      */
/* -------------------------------------------------------------------------- */
#ifndef API_EXPORT
# if defined(_WIN32)
#  define API_EXPORT __declspec(dllexport)
# else
#  define API_EXPORT __attribute__((visibility("default")))
# endif
#endif

#ifndef ATTR_NORETURN
# if defined(__GNUC__) || defined(__clang__)
#  define ATTR_NORETURN __attribute__((noreturn))
# elif defined(_MSC_VER)
#  define ATTR_NORETURN __declspec(noreturn)
# else
#  define ATTR_NORETURN
# endif
#endif

#ifndef ATTR_ASSUME
# if defined(__clang__) || defined(__GNUC__)
#  define ATTR_ASSUME(expr) do { if (!(expr)) __builtin_unreachable(); } while(0)
# elif defined(_MSC_VER)
#  define ATTR_ASSUME(expr) __assume(expr)
# else
#  define ATTR_ASSUME(expr) do { (void)0; } while(0)
# endif
#endif

#ifndef ATTR_LIKELY
# if defined(__GNUC__) || defined(__clang__)
#  define ATTR_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define ATTR_UNLIKELY(x) __builtin_expect(!!(x), 0)
# else
#  define ATTR_LIKELY(x)   (x)
#  define ATTR_UNLIKELY(x) (x)
# endif
#endif

#ifndef ATTR_PREFETCH_R
# if defined(__GNUC__) || defined(__clang__)
#  define ATTR_PREFETCH_R(p, l) __builtin_prefetch((p), 0, (l))
#  define ATTR_PREFETCH_W(p, l) __builtin_prefetch((p), 1, (l))
# elif defined(_MSC_VER)
#  include <mmintrin.h>
#  define ATTR_PREFETCH_R(p, l) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#  define ATTR_PREFETCH_W(p, l) do { (void)(p); (void)(l); } while(0)
# else
#  define ATTR_PREFETCH_R(p, l) do { (void)(p); (void)(l); } while(0)
#  define ATTR_PREFETCH_W(p, l) do { (void)(p); (void)(l); } while(0)
# endif
#endif

/* -------------------------------------------------------------------------- */
/* Helpers génériques                                                         */
/* -------------------------------------------------------------------------- */
static inline bool is_pow2(size_t x) { return x && ((x & (x - 1)) == 0); }
static inline size_t align_up(size_t x, size_t a) {
  return (x + (a - 1)) & ~(a - 1);
}

/* -------------------------------------------------------------------------- */
/* Chaînes de build (compilateur/OS/arch)                                     */
/* -------------------------------------------------------------------------- */
API_EXPORT const char* vl_compiler(void) {
#if defined(__clang__) && defined(__clang_major__)
  static char buf[64];
  snprintf(buf, sizeof(buf), "clang %d.%d.%d",
           __clang_major__, __clang_minor__, __clang_patchlevel__);
  return buf;
#elif defined(__GNUC__) && defined(__GNUC_MINOR__)
  static char buf[64];
  snprintf(buf, sizeof(buf), "gcc %d.%d.%d",
           __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
  return buf;
#elif defined(_MSC_VER)
  static char buf[64];
  snprintf(buf, sizeof(buf), "msvc %d", (int)_MSC_VER);
  return buf;
#else
  return "unknown-cc";
#endif
}

API_EXPORT const char* vl_os(void) {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#elif defined(__FreeBSD__)
  return "freebsd";
#elif defined(__OpenBSD__)
  return "openbsd";
#elif defined(__NetBSD__)
  return "netbsd";
#else
  return "unknown-os";
#endif
}

API_EXPORT const char* vl_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__riscv) || defined(__riscv__)
  return "riscv";
#elif defined(__ppc64__)
  return "ppc64";
#elif defined(__ppc__)
  return "ppc";
#else
  return "unknown-arch";
#endif
}

/* -------------------------------------------------------------------------- */
/* Drapeaux de capacités                                                      */
/* -------------------------------------------------------------------------- */
API_EXPORT VlAttrCaps vl_attr_caps(void) {
  VlAttrCaps c = {0};

  /* langage / compilateur */
#if __STDC_VERSION__ >= 201112L
  c.c11 = 1;
#endif
#if defined(__STDC_NO_THREADS__)
  c.threads = 0;
#else
  c.threads = 1;
#endif
#if defined(__GNUC__) || defined(__clang__)
  c.gnu = 1;
#endif
#if defined(_MSC_VER)
  c.msvc = 1;
#endif
#if defined(__has_builtin)
# if __has_builtin(__builtin_expect)
  c.builtin_expect = 1;
# endif
# if __has_builtin(__builtin_unreachable)
  c.builtin_unreachable = 1;
# endif
# if __has_builtin(__builtin_prefetch)
  c.builtin_prefetch = 1;
# endif
#endif

  /* CPU features best-effort */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
# if defined(_MSC_VER)
  int cpuInfo[4] = {0};
  __cpuid(cpuInfo, 1);
  c.sse2 = (cpuInfo[3] & (1 << 26)) != 0;
  c.sse4_2 = (cpuInfo[2] & (1 << 20)) != 0;
  __cpuid(cpuInfo, 7);
  c.avx2 = (cpuInfo[1] & (1 << 5)) != 0;
# else
  unsigned int a, b, d, cX;
  a = 0; __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(cX), "=d"(d));
  a = 1; __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(cX), "=d"(d));
  c.sse2   = (d & (1u<<26)) != 0;
  c.sse4_2 = (cX & (1u<<20)) != 0;
  a = 7; b = 0; __asm__ volatile("cpuid" : "+a"(a), "+b"(b), "=c"(cX), "=d"(d));
  c.avx2 = (b & (1u<<5)) != 0;
# endif
#elif defined(__aarch64__) || defined(__arm__)
  /* Linux: getauxval; macOS: assume NEON on arm64; Windows ARM64 assumes NEON */
# if defined(__linux__)
  unsigned long hw1 = getauxval(AT_HWCAP);
  (void)hw1;
  c.neon = 1; /* pratiquement toujours présent sur armv7+ et arm64 */
# else
  c.neon = 1;
# endif
#endif

  return c;
}

/* -------------------------------------------------------------------------- */
/* Cache line size                                                            */
/* -------------------------------------------------------------------------- */
API_EXPORT size_t vl_cacheline_size(void) {
  /* valeurs raisonnables par défaut */
#if defined(__aarch64__) || defined(_M_ARM64)
  size_t fallback = 64;
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
  size_t fallback = 64;
#else
  size_t fallback = 64;
#endif

#if defined(_WIN32)
  DWORD len = 0;
  GetLogicalProcessorInformation(NULL, &len);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return fallback;
  SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buf =
      (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)malloc(len);
  if (!buf) return fallback;
  if (!GetLogicalProcessorInformation(buf, &len)) { free(buf); return fallback; }
  size_t out = fallback;
  DWORD count = len / sizeof(*buf);
  for (DWORD i = 0; i < count; i++) {
    if (buf[i].Relationship == RelationCache && buf[i].Cache.Level == 1) {
      out = buf[i].Cache.LineSize ? buf[i].Cache.LineSize : fallback;
      break;
    }
  }
  free(buf);
  return out;
#elif defined(__APPLE__)
  size_t line = 0; size_t sz = sizeof(line);
  if (sysctlbyname("hw.cachelinesize", &line, &sz, NULL, 0) == 0 && line) return line;
  return fallback;
#elif defined(__linux__)
# ifdef _SC_LEVEL1_DCACHE_LINESIZE
  long v = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  if (v > 0) return (size_t)v;
# endif
  return fallback;
#else
  return fallback;
#endif
}

/* -------------------------------------------------------------------------- */
/* Prefetch helpers                                                           */
/* -------------------------------------------------------------------------- */
API_EXPORT void vl_prefetch_ro(const void* p, int locality /*0..3*/) {
  int loc = locality;
  if (loc < 0) loc = 0; else if (loc > 3) loc = 3;
  (void)loc;
  ATTR_PREFETCH_R(p, loc);
}

API_EXPORT void vl_prefetch_rw(const void* p, int locality /*0..3*/) {
  int loc = locality;
  if (loc < 0) loc = 0; else if (loc > 3) loc = 3;
  (void)loc;
  ATTR_PREFETCH_W(p, loc);
}

/* -------------------------------------------------------------------------- */
/* Unreachable / assume                                                       */
/* -------------------------------------------------------------------------- */
API_EXPORT ATTR_NORETURN void vl_unreachable(const char* why) {
#if defined(__GNUC__) || defined(__clang__)
  if (why) fprintf(stderr, "vl_unreachable: %s\n", why);
  __builtin_trap();
#elif defined(_MSC_VER)
  if (why) { fprintf(stderr, "vl_unreachable: %s\n", why); fflush(stderr); }
  __debugbreak();
  abort();
#else
  if (why) fprintf(stderr, "vl_unreachable: %s\n", why);
  abort();
#endif
}

API_EXPORT void vl_assume(bool cond) {
  ATTR_ASSUME(cond);
}

/* -------------------------------------------------------------------------- */
/* Align helpers exposés                                                      */
/* -------------------------------------------------------------------------- */
API_EXPORT size_t vl_align_up(size_t x, size_t a) {
  if (a == 0) a = sizeof(max_align_t);
  if (!is_pow2(a)) a = sizeof(max_align_t);
  return align_up(x, a);
}
API_EXPORT bool vl_is_pow2(size_t x) { return is_pow2(x); }

/* -------------------------------------------------------------------------- */
/* Infos formatées                                                             */
/* -------------------------------------------------------------------------- */
API_EXPORT void vl_build_info(VlBuildInfo* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  snprintf(out->compiler, sizeof(out->compiler), "%s", vl_compiler());
  snprintf(out->os,       sizeof(out->os),       "%s", vl_os());
  snprintf(out->arch,     sizeof(out->arch),     "%s", vl_arch());
  out->cacheline = vl_cacheline_size();
  out->caps = vl_attr_caps();
}

/* -------------------------------------------------------------------------- */
/* Petite démo (désactivée par défaut)                                        */
/* -------------------------------------------------------------------------- */
#ifdef ATTRS_DEMO_MAIN
int main(void) {
  VlBuildInfo bi;
  vl_build_info(&bi);
  printf("cc=%s os=%s arch=%s cacheline=%zu\n",
         bi.compiler, bi.os, bi.arch, bi.cacheline);
  printf("caps: c11=%d gnu=%d msvc=%d threads=%d sse2=%d avx2=%d neon=%d\n",
         bi.caps.c11, bi.caps.gnu, bi.caps.msvc, bi.caps.threads,
         bi.caps.sse2, bi.caps.avx2, bi.caps.neon);

  int x = 1;
  vl_assume(x == 1);
  vl_prefetch_ro(&bi, 3);

  size_t a = 33;
  printf("align_up(33,16)=%zu pow2(64)=%d\n", vl_align_up(a,16), vl_is_pow2(64));
  return 0;
}
#endif
