/* ============================================================================
   tm.c — Horloges, dates, formats ISO 8601, timers (C17)
   - Horloge monotone haute résolution (ns)
   - Horloge murale UTC/local + décalage zone (±HH:MM)
   - Sleep ns/ms, deadlines
   - Formatage/parse ISO-8601 avec fraction nanosecondes
   - Conversions struct tm ⇄ epoch, utilitaires
   - Windows / Linux / macOS, sans dépendances externes
   Lier avec tm.h. Licence: MIT.
   ============================================================================
 */
#include "tm.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ----------------------------------------------------------------------------
   Platform
---------------------------------------------------------------------------- */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <processthreadsapi.h>
#include <synchapi.h>
#include <sysinfoapi.h>
#include <windows.h>
#ifndef TIMESPEC_TO_FILETIME_100NS
#define TIMESPEC_TO_FILETIME_100NS 10000000ULL
#endif
#endif

#if !defined(_WIN32)
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(CLOCK_MONOTONIC)
#define VT_HAVE_CLOCK_MONOTONIC 1
#endif
#if defined(CLOCK_REALTIME)
#define VT_HAVE_CLOCK_REALTIME 1
#endif
#endif

#ifndef VT_LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define VT_LIKELY(x) __builtin_expect(!!(x), 1)
#define VT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define VT_LIKELY(x) (x)
#define VT_UNLIKELY(x) (x)
#endif
#endif

/* ----------------------------------------------------------------------------
   Helpers
---------------------------------------------------------------------------- */
static inline void vt__normalize(vt_time* t) {
  if (!t) return;
  if (t->nsec >= 1000000000L) {
    int64_t s = t->nsec / 1000000000L;
    t->sec += s;
    t->nsec -= s * 1000000000L;
  } else if (t->nsec < 0) {
    int64_t s = (-t->nsec + 999999999L) / 1000000000L;
    t->sec -= s;
    t->nsec += s * 1000000000L;
  }
}

static inline int64_t vt__llabs(int64_t x) { return x < 0 ? -x : x; }

/* ----------------------------------------------------------------------------
   Monotonic now (ns)
---------------------------------------------------------------------------- */
#if defined(_WIN32)
static int vt__qpc_ready = 0;
static LARGE_INTEGER vt__qpc_freq;

static void vt__init_qpc(void) {
  if (vt__qpc_ready) return;
  QueryPerformanceFrequency(&vt__qpc_freq);
  vt__qpc_ready = 1;
}
#endif

uint64_t vt_ns_now_monotonic(void) {
#if defined(_WIN32)
  vt__init_qpc();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  // ns = ticks * 1e9 / freq with rounding
  __int128 ticks = (__int128)c.QuadPart;
  __int128 num = ticks * 1000000000LL;
  return (uint64_t)(num / vt__qpc_freq.QuadPart);
#elif defined(VT_HAVE_CLOCK_MONOTONIC)
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  }
  // Fallback to realtime if monotonic unavailable
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;
#endif
}

/* ----------------------------------------------------------------------------
   Wall clock UTC now (vt_time), and epoch⇄tm conversions
---------------------------------------------------------------------------- */
#if defined(_WIN32)
typedef VOID(WINAPI* PFN_GetSystemTimePreciseAsFileTime)(LPFILETIME);
static PFN_GetSystemTimePreciseAsFileTime vt__gstp = NULL;

static void vt__init_wall(void) {
  static int inited = 0;
  if (inited) return;
  HMODULE h = GetModuleHandleA("kernel32.dll");
  if (h)
    vt__gstp = (PFN_GetSystemTimePreciseAsFileTime)GetProcAddress(
        h, "GetSystemTimePreciseAsFileTime");
  inited = 1;
}

static void vt__utc_now_win(vt_time* out) {
  vt__init_wall();
  FILETIME ft;
  if (vt__gstp)
    vt__gstp(&ft);
  else
    GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER t100;
  t100.LowPart = ft.dwLowDateTime;
  t100.HighPart = ft.dwHighDateTime;
  // Windows epoch 1601-01-01 → Unix 1970-01-01 offset
  const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
  uint64_t u100 = (t100.QuadPart >= EPOCH_DIFF_100NS)
                      ? (t100.QuadPart - EPOCH_DIFF_100NS)
                      : 0;
  out->sec = (int64_t)(u100 / 10000000ULL);
  out->nsec = (int32_t)((u100 % 10000000ULL) * 100);
}
#endif

int vt_utc_now(vt_time* out) {
  if (!out) return -1;
#if defined(_WIN32)
  vt__utc_now_win(out);
  return 0;
#elif defined(VT_HAVE_CLOCK_REALTIME)
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
  out->sec = (int64_t)ts.tv_sec;
  out->nsec = (int32_t)ts.tv_nsec;
  return 0;
#else
  struct timeval tv;
  if (gettimeofday(&tv, NULL) != 0) return -1;
  out->sec = (int64_t)tv.tv_sec;
  out->nsec = (int32_t)tv.tv_usec * 1000;
  return 0;
#endif
}

static int vt__gmtime_safe(time_t t, struct tm* out_tm) {
#if defined(_WIN32)
  return gmtime_s(out_tm, &t) == 0 ? 0 : -1;
#elif defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__) || \
    defined(__unix__)
  return gmtime_r(&t, out_tm) ? 0 : -1;
#else
  struct tm* p = gmtime(&t);
  if (!p) return -1;
  *out_tm = *p;
  return 0;
#endif
}

static int vt__localtime_safe(time_t t, struct tm* out_tm) {
#if defined(_WIN32)
  return localtime_s(out_tm, &t) == 0 ? 0 : -1;
#elif defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__) || \
    defined(__unix__)
  return localtime_r(&t, out_tm) ? 0 : -1;
#else
  struct tm* p = localtime(&t);
  if (!p) return -1;
  *out_tm = *p;
  return 0;
#endif
}

int vt_time_to_tm_utc(const vt_time* t, struct tm* out_tm) {
  if (!t || !out_tm) return -1;
  time_t s = (time_t)t->sec;
  return vt__gmtime_safe(s, out_tm);
}

int vt_time_to_tm_local(const vt_time* t, struct tm* out_tm) {
  if (!t || !out_tm) return -1;
  time_t s = (time_t)t->sec;
  return vt__localtime_safe(s, out_tm);
}

int vt_time_from_tm_utc(const struct tm* in_tm, long nsec, vt_time* out) {
  if (!in_tm || !out) return -1;
#if defined(_WIN32)
  // _mkgmtime: tm (UTC) → time_t
  time_t s = _mkgmtime((struct tm*)in_tm);
#else
  // timegm: GNU/BSD; fallback with TZ hack is avoided here
#if defined(__USE_BSD) || defined(__APPLE__) || defined(__GLIBC__)
  time_t s = timegm((struct tm*)in_tm);
#else
  // Conservative fallback: treat as local then subtract offset
  struct tm g = *in_tm;
  time_t ls = mktime(&g);
  struct tm lg;
  vt__gmtime_safe(ls, &lg);
  time_t gs = mktime(&lg);
  time_t off = ls - gs;
  time_t s = ls - off;
#endif
#endif
  if (s == (time_t)-1) return -1;
  out->sec = (int64_t)s;
  out->nsec = (int32_t)nsec;
  vt__normalize(out);
  return 0;
}

int vt_time_from_tm_local(const struct tm* in_tm, long nsec, vt_time* out) {
  if (!in_tm || !out) return -1;
  time_t s = mktime((struct tm*)in_tm);  // interprets as local
  if (s == (time_t)-1) return -1;
  out->sec = (int64_t)s;
  out->nsec = (int32_t)nsec;
  vt__normalize(out);
  return 0;
}

/* ----------------------------------------------------------------------------
   Timezone offset minutes for given epoch seconds (DST aware)
---------------------------------------------------------------------------- */
int vt_local_offset_minutes(int64_t epoch_sec) {
  time_t tt = (time_t)epoch_sec;
  struct tm g, l;
  if (vt__gmtime_safe(tt, &g) != 0) return 0;
  if (vt__localtime_safe(tt, &l) != 0) return 0;
#if defined(_WIN32)
  time_t ug = _mkgmtime(&g);
#else
#if defined(__USE_BSD) || defined(__APPLE__) || defined(__GLIBC__)
  time_t ug = timegm(&g);
#else
  struct tm gg = g;
  time_t ug = mktime(&gg);
#endif
#endif
  time_t ul = mktime(&l);
  if (ug == (time_t)-1 || ul == (time_t)-1) return 0;
  long diff = (long)difftime(ul, ug);  // seconds east of UTC
  return (int)(diff / 60);
}

/* ----------------------------------------------------------------------------
   Sleep
---------------------------------------------------------------------------- */
int vt_sleep_ns(uint64_t ns) {
#if defined(_WIN32)
  // Sleep is ms resolution; use waitable timer for sub-ms
  if (ns == 0) {
    Sleep(0);
    return 0;
  }
  HANDLE h = CreateWaitableTimerW(NULL, TRUE, NULL);
  if (!h) return -1;
  // Relative due time in 100ns ticks, negative for relative
  LARGE_INTEGER due;
  // round to nearest 100ns
  __int128 t100 = -((__int128)ns / 100ULL);
  if ((ns % 100ULL) != 0) t100 -= 1;
  due.QuadPart = (LONGLONG)t100;
  if (!SetWaitableTimer(h, &due, 0, NULL, NULL, FALSE)) {
    CloseHandle(h);
    return -1;
  }
  DWORD rc = WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
  return (rc == WAIT_OBJECT_0) ? 0 : -1;
#else
  struct timespec req;
  req.tv_sec = (time_t)(ns / 1000000000ULL);
  req.tv_nsec = (long)(ns % 1000000000ULL);
  while (nanosleep(&req, &req) != 0) {
    if (errno != EINTR) return -1;
  }
  return 0;
#endif
}

int vt_sleep_ms(uint64_t ms) { return vt_sleep_ns(ms * 1000000ULL); }

/* Sleep until absolute deadline in monotonic ns. */
int vt_sleep_until_ns(uint64_t deadline_ns) {
  uint64_t now = vt_ns_now_monotonic();
  if (now >= deadline_ns) return 0;
  return vt_sleep_ns(deadline_ns - now);
}

/* ----------------------------------------------------------------------------
   Add/Sub utility
---------------------------------------------------------------------------- */
vt_time vt_time_add_ns(vt_time t, int64_t ns) {
  t.sec += ns / 1000000000LL;
  t.nsec += (int32_t)(ns % 1000000000LL);
  vt__normalize(&t);
  return t;
}
int64_t vt_time_diff_ns(vt_time a, vt_time b) {
  // a - b in ns
  int64_t ds = a.sec - b.sec;
  int64_t dn = (int64_t)a.nsec - (int64_t)b.nsec;
  return ds * 1000000000LL + dn;
}

/* ----------------------------------------------------------------------------
   ISO-8601 Formatting
   Forms:
     UTC   : YYYY-MM-DDTHH:MM:SS[.fffffffff]Z
     Local : YYYY-MM-DDTHH:MM:SS[.fffffffff]{+|-}HH:MM
---------------------------------------------------------------------------- */
static void vt__two(char* p, int v) {
  p[0] = '0' + (v / 10) % 10;
  p[1] = '0' + v % 10;
}

static int vt__write_iso_core(char* dst, size_t cap, const struct tm* tm,
                              int frac_ns, int write_frac) {
  if (cap < 19) return -1;
  int year = tm->tm_year + 1900;
  int mon = tm->tm_mon + 1;
  int day = tm->tm_mday;
  int hh = tm->tm_hour;
  int mm = tm->tm_min;
  int ss = tm->tm_sec;

  size_t i = 0;
  dst[i++] = '0' + (year / 1000) % 10;
  dst[i++] = '0' + (year / 100) % 10;
  dst[i++] = '0' + (year / 10) % 10;
  dst[i++] = '0' + year % 10;
  dst[i++] = '-';
  vt__two(&dst[i], mon);
  i += 2;
  dst[i++] = '-';
  vt__two(&dst[i], day);
  i += 2;
  dst[i++] = 'T';
  vt__two(&dst[i], hh);
  i += 2;
  dst[i++] = ':';
  vt__two(&dst[i], mm);
  i += 2;
  dst[i++] = ':';
  vt__two(&dst[i], ss);
  i += 2;

  if (write_frac && frac_ns > 0) {
    // print .fffffffff trimming trailing zeros
    if (cap < i + 1 + 9) return -1;
    dst[i++] = '.';
    int digits[9];
    for (int k = 8; k >= 0; k--) {
      digits[k] = frac_ns % 10;
      frac_ns /= 10;
    }
    int last = 8;
    while (last >= 0 && digits[last] == 0) last--;
    for (int k = 0; k <= last; k++) dst[i++] = '0' + digits[k];
  }
  return (int)i;
}

int vt_time_format_iso8601_utc(const vt_time* t, char* dst, size_t cap,
                               int with_frac) {
  if (!t || !dst) return -1;
  struct tm tm;
  if (vt_time_to_tm_utc(t, &tm) != 0) return -1;
  int n = vt__write_iso_core(dst, cap, &tm, t->nsec, with_frac);
  if (n < 0) return -1;
  if ((size_t)(n + 1) > cap) return -1;
  dst[n++] = 'Z';
  if ((size_t)n >= cap) return -1;
  dst[n] = '\0';
  return n;
}

int vt_time_format_iso8601_local(const vt_time* t, char* dst, size_t cap,
                                 int with_frac) {
  if (!t || !dst) return -1;
  struct tm tm;
  if (vt_time_to_tm_local(t, &tm) != 0) return -1;
  int base = vt__write_iso_core(dst, cap, &tm, t->nsec, with_frac);
  if (base < 0) return -1;
  int offm = vt_local_offset_minutes(t->sec);
  int sgn = offm < 0 ? -1 : 1;
  int ao = vt__llabs(offm);
  int oh = ao / 60;
  int om = ao % 60;

  size_t need = (size_t)base + 6 + 1;  // ±HH:MM + NUL
  if (need > cap) return -1;
  dst[base++] = (char)(sgn < 0 ? '-' : '+');
  dst[base++] = '0' + (oh / 10) % 10;
  dst[base++] = '0' + (oh % 10);
  dst[base++] = ':';
  dst[base++] = '0' + (om / 10) % 10;
  dst[base++] = '0' + (om % 10);
  dst[base] = '\0';
  return (int)base;
}

/* ----------------------------------------------------------------------------
   ISO-8601 Parsing (subset)
   Accepts:
     YYYY-MM-DDTHH:MM:SS[.fffffffff](Z|±HH:MM)
---------------------------------------------------------------------------- */
static int vt__dig(const char* s) {
  return (*s >= '0' && *s <= '9') ? (*s - '0') : -1;
}
static int vt__parse_n(const char* s, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    int d = vt__dig(s + i);
    if (d < 0) return -1;
    v = v * 10 + d;
  }
  return v;
}

int vt_time_parse_iso8601(const char* z, vt_time* out) {
  if (!z || !out) return -1;
  // Minimal length check
  size_t L = strlen(z);
  if (L < 20) return -1;

  // YYYY-MM-DDTHH:MM:SS
  if (z[4] != '-' || z[7] != '-' || (z[10] != 'T' && z[10] != 't') ||
      z[13] != ':' || z[16] != ':')
    return -1;
  int Y = vt__parse_n(z + 0, 4);
  int m = vt__parse_n(z + 5, 2);
  int d = vt__parse_n(z + 8, 2);
  int H = vt__parse_n(z + 11, 2);
  int M = vt__parse_n(z + 14, 2);
  int S = vt__parse_n(z + 17, 2);
  if (Y < 1970 || m < 1 || m > 12 || d < 1 || d > 31 || H < 0 || H > 23 ||
      M < 0 || M > 59 || S < 0 || S > 60)
    return -1;

  const char* p = z + 19;
  long nsec = 0;
  if (*p == '.') {
    p++;
    // up to 9 digits
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 9) {
      nsec = nsec * 10 + (*p - '0');
      digits++;
      p++;
    }
    // scale if less than 9 digits
    for (int i = digits; i < 9; i++) nsec *= 10;
  }

  int off_min = 0;
  if (*p == 'Z' || *p == 'z') {
    p++;
  } else if (*p == '+' || *p == '-') {
    int sgn = (*p == '-') ? -1 : 1;
    p++;
    if (!(vt__dig(p) >= 0 && vt__dig(p + 1) >= 0 && p[2] == ':' &&
          vt__dig(p + 3) >= 0 && vt__dig(p + 4) >= 0))
      return -1;
    int oh = vt__parse_n(p, 2);
    int om = vt__parse_n(p + 3, 2);
    off_min = sgn * (oh * 60 + om);
    p += 5;
  } else {
    return -1;
  }
  if (*p != '\0') return -1;

  struct tm tm;
  memset(&tm, 0, sizeof tm);
  tm.tm_year = Y - 1900;
  tm.tm_mon = m - 1;
  tm.tm_mday = d;
  tm.tm_hour = H;
  tm.tm_min = M;
  tm.tm_sec = S;

  vt_time t;
  if (off_min == 0) {
    if (vt_time_from_tm_utc(&tm, nsec, &t) != 0) return -1;
  } else {
    // treat as UTC then subtract offset
    if (vt_time_from_tm_utc(&tm, nsec, &t) != 0) return -1;
    t = vt_time_add_ns(t, -(int64_t)off_min * 60LL * 1000000000LL);
  }
  *out = t;
  return 0;
}

/* ----------------------------------------------------------------------------
   Stopwatch / Timer
---------------------------------------------------------------------------- */
void vt_timer_start(vt_timer* w) {
  if (!w) return;
  w->start_ns = vt_ns_now_monotonic();
  w->elapsed_ns = 0;
  w->running = 1;
}
void vt_timer_stop(vt_timer* w) {
  if (!w || !w->running) return;
  uint64_t now = vt_ns_now_monotonic();
  w->elapsed_ns += now - w->start_ns;
  w->running = 0;
}
void vt_timer_resume(vt_timer* w) {
  if (!w || w->running) return;
  w->start_ns = vt_ns_now_monotonic();
  w->running = 1;
}
uint64_t vt_timer_elapsed_ns(const vt_timer* w) {
  if (!w) return 0;
  if (!w->running) return w->elapsed_ns;
  uint64_t now = vt_ns_now_monotonic();
  return w->elapsed_ns + (now - w->start_ns);
}

/* ----------------------------------------------------------------------------
   Convenience: now millis / micros
---------------------------------------------------------------------------- */
uint64_t vt_ms_now_monotonic(void) {
  return vt_ns_now_monotonic() / 1000000ULL;
}
uint64_t vt_us_now_monotonic(void) { return vt_ns_now_monotonic() / 1000ULL; }

/* ----------------------------------------------------------------------------
   RFC 3339 wrappers (alias ISO-8601)
---------------------------------------------------------------------------- */
int vt_time_format_rfc3339_utc(const vt_time* t, char* dst, size_t cap) {
  return vt_time_format_iso8601_utc(t, dst, cap, 1);
}
int vt_time_format_rfc3339_local(const vt_time* t, char* dst, size_t cap) {
  return vt_time_format_iso8601_local(t, dst, cap, 1);
}
