// ltm.c (stub)
// vitte-light/core/tm.c
// Horloges et minuteurs portables pour VitteLight: temps réel, monotone,
// veille, deadline, chronomètre, format ISO‑8601. Zéro dépendance interne.
//
// API suggérée (si vous voulez un header, demandez `tm.h`):
//   uint64_t vl_wall_time_ns(void);      // Epoch UTC en ns
//   uint64_t vl_mono_time_ns(void);      // Horloge monotone en ns (origine
//   indéfinie) int      vl_sleep_ms(uint32_t ms);   // 0 si ok int
//   vl_sleep_ns(uint64_t ns);   // 0 si ok
//
//   typedef struct { uint64_t t0; } VL_Stopwatch;  // chrono monotone
//   static inline void     vl_sw_start(VL_Stopwatch *sw);
//   static inline uint64_t vl_sw_elapsed_ns(const VL_Stopwatch *sw);
//
//   typedef struct { uint64_t due_ns; } VL_Deadline; // sur horloge monotone
//   static inline VL_Deadline vl_deadline_in_ns(uint64_t ns);
//   static inline int         vl_deadline_expired(VL_Deadline d);
//   static inline uint64_t    vl_deadline_remaining_ns(VL_Deadline d);
//
//   size_t vl_time_iso8601_utc (uint64_t epoch_ns, char *buf, size_t n);
//   size_t vl_time_iso8601_local(uint64_t epoch_ns, char *buf, size_t n);
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/tm.c

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <sysinfoapi.h>
#include <windows.h>
#else
#include <errno.h>
#include <sys/time.h>
#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 199309L)
#include <unistd.h>
#endif
#endif

#ifndef VL_TM_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define VL_TM_INLINE static __inline__ __attribute__((always_inline))
#else
#define VL_TM_INLINE static __inline
#endif
#endif

// ───────────────────────── Horloge monotone ─────────────────────────
#ifdef _WIN32
static uint64_t qpc_freq_hz(void) {
  static LARGE_INTEGER f = {{0}};
  static int inited = 0;
  if (!inited) {
    QueryPerformanceFrequency(&f);
    inited = 1;
  }
  return (uint64_t)f.QuadPart;
}
uint64_t vl_mono_time_ns(void) {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  uint64_t freq = qpc_freq_hz();
  // convertir en ns sans overflow: (c*1e9)/freq
  __int128 num = (__int128)c.QuadPart * 1000000000ull;
  uint64_t ns = (uint64_t)(num / freq);
  return ns;
}
#else
uint64_t vl_mono_time_ns(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
#endif
}
#endif

// ───────────────────────── Temps réel (epoch UTC) ─────────────────────────
#ifdef _WIN32
static uint64_t filetime_to_unix_ns(const FILETIME *ft) {
  // FILETIME = 100ns depuis 1601-01-01. Unix epoch offset = 11644473600 s.
  ULARGE_INTEGER u;
  u.LowPart = ft->dwLowDateTime;
  u.HighPart = ft->dwHighDateTime;
  uint64_t t100 = u.QuadPart;  // 100ns units
  const uint64_t EPOCH_DIFF_100NS = 11644473600ull * 10000000ull;
  if (t100 < EPOCH_DIFF_100NS) return 0;
  uint64_t unix_100 = t100 - EPOCH_DIFF_100NS;
  return unix_100 * 100ull;  // -> ns
}
uint64_t vl_wall_time_ns(void) {
  FILETIME ft;
#ifdef GetSystemTimePreciseAsFileTime
  GetSystemTimePreciseAsFileTime(&ft);
#else
  GetSystemTimeAsFileTime(&ft);
#endif
  return filetime_to_unix_ns(&ft);
}
#else
uint64_t vl_wall_time_ns(void) {
#if defined(CLOCK_REALTIME)
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
#endif
}
#endif

// ───────────────────────── Veille ─────────────────────────
int vl_sleep_ms(uint32_t ms) {
#ifdef _WIN32
  Sleep(ms);
  return 0;
#else
#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 199309L)
  struct timespec ts;
  ts.tv_sec = ms / 1000u;
  ts.tv_nsec = (long)(ms % 1000u) * 1000000l;
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
  return 0;
#else
  struct timeval tv;
  tv.tv_sec = ms / 1000u;
  tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
  select(0, NULL, NULL, NULL, &tv);
  return 0;
#endif
#endif
}

int vl_sleep_ns(uint64_t ns) {
#ifdef _WIN32
  // Sleep() a une résolution ms. On arrondit.
  uint32_t ms = (uint32_t)((ns + 999999ull) / 1000000ull);
  Sleep(ms);
  return 0;
#else
#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 199309L)
  struct timespec ts;
  ts.tv_sec = (time_t)(ns / 1000000000ull);
  ts.tv_nsec = (long)(ns % 1000000000ull);
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
  return 0;
#else
  return vl_sleep_ms((uint32_t)((ns + 999999ull) / 1000000ull));
#endif
#endif
}

// ───────────────────────── Chronomètre et deadline ─────────────────────────
typedef struct {
  uint64_t t0;
} VL_Stopwatch;
VL_TM_INLINE void vl_sw_start(VL_Stopwatch *sw) {
  if (sw) sw->t0 = vl_mono_time_ns();
}
VL_TM_INLINE uint64_t vl_sw_elapsed_ns(const VL_Stopwatch *sw) {
  return sw ? (vl_mono_time_ns() - sw->t0) : 0;
}

typedef struct {
  uint64_t due_ns;
} VL_Deadline;
VL_TM_INLINE VL_Deadline vl_deadline_in_ns(uint64_t ns) {
  VL_Deadline d;
  d.due_ns = vl_mono_time_ns() + ns;
  return d;
}
VL_TM_INLINE int vl_deadline_expired(VL_Deadline d) {
  return (vl_mono_time_ns() >= d.due_ns);
}
VL_TM_INLINE uint64_t vl_deadline_remaining_ns(VL_Deadline d) {
  uint64_t now = vl_mono_time_ns();
  return now >= d.due_ns ? 0ull : (d.due_ns - now);
}

// ───────────────────────── Format ISO‑8601 ─────────────────────────
static size_t write_2(char *b, size_t i, int v) {
  b[i + 0] = (char)('0' + (v / 10) % 10);
  b[i + 1] = (char)('0' + v % 10);
  return i + 2;
}
static size_t write_3(char *b, size_t i, int v) {
  b[i + 0] = (char)('0' + (v / 100) % 10);
  b[i + 1] = (char)('0' + (v / 10) % 10);
  b[i + 2] = (char)('0' + v % 10);
  return i + 3;
}

static size_t iso8601_core(const struct tm *tm, int nano, int zulu, char *buf,
                           size_t n) {
  // YYYY-MM-DDTHH:MM:SS(.fffffffff)Z?
  if (n < 20) return 0;
  size_t k = 0;
  int year = tm->tm_year + 1900;
  buf[k++] = (char)('0' + (year / 1000) % 10);
  buf[k++] = (char)('0' + (year / 100) % 10);
  buf[k++] = (char)('0' + (year / 10) % 10);
  buf[k++] = (char)('0' + (year % 10));
  buf[k++] = '-';
  k = write_2(buf, k, tm->tm_mon + 1);
  buf[k++] = '-';
  k = write_2(buf, k, tm->tm_mday);
  buf[k++] = 'T';
  k = write_2(buf, k, tm->tm_hour);
  buf[k++] = ':';
  k = write_2(buf, k, tm->tm_min);
  buf[k++] = ':';
  k = write_2(buf, k, tm->tm_sec);
  if (nano > 0) {
    if (k + 1 + 9 > n) return 0;
    buf[k++] = '.';
    int ms = (nano / 1000000) % 1000, us = (nano / 1000) % 1000,
        ns = nano % 1000;
    k = write_3(buf, k, ms);
    k = write_3(buf, k, us);
    k = write_3(buf, k, ns);
  }
  if (zulu) {
    if (k + 1 > n) return 0;
    buf[k++] = 'Z';
  }
  if (k < n) buf[k] = '\0';
  return k;
}

static size_t tm_utc_from_epoch(uint64_t epoch_s, struct tm *out) {
#ifdef _WIN32
  __time64_t t = (__time64_t)epoch_s;
  return _gmtime64_s(out, &t) == 0 ? 1 : 0;
#else
  time_t t = (time_t)epoch_s;
  return gmtime_r(&t, out) ? 1 : 0;
#endif
}

static size_t tm_local_from_epoch(uint64_t epoch_s, struct tm *out) {
#ifdef _WIN32
  __time64_t t = (__time64_t)epoch_s;
  return _localtime64_s(out, &t) == 0 ? 1 : 0;
#else
  time_t t = (time_t)epoch_s;
  return localtime_r(&t, out) ? 1 : 0;
#endif
}

size_t vl_time_iso8601_utc(uint64_t epoch_ns, char *buf, size_t n) {
  uint64_t sec = epoch_ns / 1000000000ull;
  int nano = (int)(epoch_ns % 1000000000ull);
  struct tm tm;
  if (!tm_utc_from_epoch(sec, &tm)) return 0;
  return iso8601_core(&tm, nano, 1, buf, n);
}

size_t vl_time_iso8601_local(uint64_t epoch_ns, char *buf, size_t n) {
  uint64_t sec = epoch_ns / 1000000000ull;
  int nano = (int)(epoch_ns % 1000000000ull);
  struct tm tm;
  if (!tm_local_from_epoch(sec, &tm)) return 0;
  return iso8601_core(&tm, nano, 0, buf, n);
}

// ───────────────────────── Tests ─────────────────────────
#ifdef VL_TM_TEST_MAIN
int main(void) {
  uint64_t w = vl_wall_time_ns();
  uint64_t m = vl_mono_time_ns();
  char iso[40], loc[40];
  vl_time_iso8601_utc(w, iso, sizeof(iso));
  vl_time_iso8601_local(w, loc, sizeof(loc));
  printf("wall=%llu ns\nmono=%llu ns\nutc=%s\nloc=%s\n", (unsigned long long)w,
         (unsigned long long)m, iso, loc);
  VL_Stopwatch sw;
  vl_sw_start(&sw);
  vl_sleep_ms(10);
  uint64_t dt = vl_sw_elapsed_ns(&sw);
  printf("sleep ~10ms => %llu us\n", (unsigned long long)(dt / 1000ull));
  VL_Deadline d = vl_deadline_in_ns(5 * 1000 * 1000ull);
  while (!vl_deadline_expired(d)) {
  }
  puts("deadline ok");
  return 0;
}
#endif
