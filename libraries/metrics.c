// SPDX-License-Identifier: GPL-3.0-or-later
//
// metrics.c — Vitte Light metrics standard library (C17, portable)
// Namespace: "metrics"
//
// Coverage:
//   Time sources:     now_ns(), now_ms(), mono_ns(), mono_ms()
//   Sleep:            sleep_ms(u64)
//   CPU time:         proc_cpu_time_ns()
//   Memory usage:     proc_rss_bytes(), proc_vms_bytes()
//   Counters:         counter_inc(name[, delta]), counter_get/set/reset
//   Timers:           tic(name), toc(name)->ns, timer_get(name)->ns
//   Averages:         ewma_update(name, value[, alpha]), ewma_get(name)
//   Summary:          snapshot() → struct with core fields
//
// Depends: includes/auxlib.h, state.h, object.h, vm.h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <processthreadsapi.h>
#include <psapi.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <stdio.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "includes/auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

// ===== public typedefs =======================================================
typedef uint64_t u64;
typedef int64_t i64;

// ===== time primitives =======================================================
static u64 _timespec_to_ns(struct timespec ts) {
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

u64 metrics_now_ns(void) {
#if defined(_WIN32)
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER uli;
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  // Windows FILETIME: 100ns since 1601-01-01
  const u64 EPOCH_DIFFERENCE_100NS = 116444736000000000ull;
  u64 now100 = uli.QuadPart - EPOCH_DIFFERENCE_100NS;
  return now100 * 100ull;
#elif defined(CLOCK_REALTIME)
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return _timespec_to_ns(ts);
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (u64)tv.tv_sec * 1000000000ull + (u64)tv.tv_usec * 1000ull;
#endif
}

u64 metrics_now_ms(void) { return metrics_now_ns() / 1000000ull; }

u64 metrics_mono_ns(void) {
#if defined(_WIN32)
  static u64 freq = 0;
  static LARGE_INTEGER s;
  if (freq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    freq = (u64)f.QuadPart;
  }
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  // scale to ns
  return (u64)((__int128)1000000000ull * (u64)c.QuadPart / freq);
#elif defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return _timespec_to_ns(ts);
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (u64)tv.tv_sec * 1000000000ull + (u64)tv.tv_usec * 1000ull;
#endif
}

u64 metrics_mono_ms(void) { return metrics_mono_ns() / 1000000ull; }

void metrics_sleep_ms(u64 ms) {
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  struct timespec req;
  req.tv_sec = (time_t)(ms / 1000ull);
  req.tv_nsec = (long)((ms % 1000ull) * 1000000ull);
  while (nanosleep(&req, &req) == -1) { /* retry on EINTR */
  }
#endif
}

// ===== process CPU time and memory ==========================================
u64 metrics_proc_cpu_time_ns(void) {
#if defined(_WIN32)
  FILETIME ct, et, kt, ut;
  if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
    ULARGE_INTEGER k, u;
    k.LowPart = kt.dwLowDateTime;
    k.HighPart = kt.dwHighDateTime;
    u.LowPart = ut.dwLowDateTime;
    u.HighPart = ut.dwHighDateTime;
    // FILETIME in 100ns units
    return (u64)(k.QuadPart + u.QuadPart) * 100ull;
  }
  return 0;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    u64 user = (u64)ru.ru_utime.tv_sec * 1000000000ull +
               (u64)ru.ru_utime.tv_usec * 1000ull;
    u64 sys = (u64)ru.ru_stime.tv_sec * 1000000000ull +
              (u64)ru.ru_stime.tv_usec * 1000ull;
    return user + sys;
  }
  return 0;
#endif
}

u64 metrics_proc_rss_bytes(void) {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&pmc,
                           sizeof(pmc))) {
    return (u64)pmc.WorkingSetSize;
  }
  return 0;
#elif defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &count) == KERN_SUCCESS) {
    return (u64)info.resident_size;
  }
  return 0;
#else
  // Linux: read /proc/self/statm
  FILE *f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long pages = 0;
  if (fscanf(f, "%*s %ld", &pages) != 1) {
    fclose(f);
    return 0;
  }
  fclose(f);
  long pg = sysconf(_SC_PAGESIZE);
  if (pg <= 0) pg = 4096;
  return (u64)pages * (u64)pg;
#endif
}

u64 metrics_proc_vms_bytes(void) {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&pmc,
                           sizeof(pmc))) {
    return (u64)pmc.PrivateUsage;
  }
  return 0;
#elif defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &count) == KERN_SUCCESS) {
    return (u64)info.virtual_size;
  }
  return 0;
#else
  // Linux: read /proc/self/statm first field total program size in pages
  FILE *f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long total_pages = 0;
  if (fscanf(f, "%ld", &total_pages) != 1) {
    fclose(f);
    return 0;
  }
  fclose(f);
  long pg = sysconf(_SC_PAGESIZE);
  if (pg <= 0) pg = 4096;
  return (u64)total_pages * (u64)pg;
#endif
}

// ===== light registry for named metrics =====================================
#ifndef METRICS_MAX_ITEMS
#define METRICS_MAX_ITEMS 128
#endif

typedef struct {
  char name[48];
  i64 value;
  u64 t0_ns;     // for tic/toc
  double ewma;   // exponentially weighted moving average
  double alpha;  // smoothing factor in [0,1]
  bool has_t0;
  bool has_ewma;
  bool used;
} metrics_slot_t;

static metrics_slot_t G[METRICS_MAX_ITEMS];

static int slot_find(const char *name) {
  for (int i = 0; i < METRICS_MAX_ITEMS; ++i) {
    if (G[i].used && strncmp(G[i].name, name, sizeof(G[i].name)) == 0) return i;
  }
  return -1;
}

static int slot_ensure(const char *name) {
  int idx = slot_find(name);
  if (idx >= 0) return idx;
  for (int i = 0; i < METRICS_MAX_ITEMS; ++i) {
    if (!G[i].used) {
      memset(&G[i], 0, sizeof(G[i]));
      aux_strlcpy(G[i].name, name, sizeof(G[i].name));
      G[i].used = true;
      return i;
    }
  }
  return -1;  // full
}

// ===== counters ==============================================================
i64 metrics_counter_inc(const char *name, i64 delta) {
  int i = slot_ensure(name);
  if (i < 0) return 0;
  G[i].value += delta;
  return G[i].value;
}

void metrics_counter_set(const char *name, i64 value) {
  int i = slot_ensure(name);
  if (i < 0) return;
  G[i].value = value;
}

i64 metrics_counter_get(const char *name) {
  int i = slot_find(name);
  return (i >= 0) ? G[i].value : 0;
}

void metrics_counter_reset(const char *name) {
  int i = slot_find(name);
  if (i >= 0) G[i].value = 0;
}

// ===== timers ================================================================
void metrics_tic(const char *name) {
  int i = slot_ensure(name);
  if (i < 0) return;
  G[i].t0_ns = metrics_mono_ns();
  G[i].has_t0 = true;
}

u64 metrics_toc(const char *name) {
  int i = slot_find(name);
  if (i < 0 || !G[i].has_t0) return 0;
  u64 dt = metrics_mono_ns() - G[i].t0_ns;
  G[i].has_t0 = false;
  G[i].value = (i64)dt;  // store last duration in ns
  return dt;
}

u64 metrics_timer_get_ns(const char *name) {
  int i = slot_find(name);
  if (i < 0) return 0;
  return (u64)((G[i].value < 0) ? 0 : G[i].value);
}

// ===== EWMA ==================================================================
double metrics_ewma_update(const char *name, double x, double alpha) {
  if (alpha < 0.0) alpha = 0.0;
  if (alpha > 1.0) alpha = 1.0;
  int i = slot_ensure(name);
  if (i < 0) return 0.0;
  if (!G[i].has_ewma) {
    G[i].ewma = x;
    G[i].alpha = alpha;
    G[i].has_ewma = true;
  } else {
    double a = (alpha > 0.0 ? alpha : G[i].alpha);
    G[i].ewma = a * x + (1.0 - a) * G[i].ewma;
  }
  return G[i].ewma;
}

double metrics_ewma_get(const char *name) {
  int i = slot_find(name);
  return (i >= 0 && G[i].has_ewma) ? G[i].ewma : 0.0;
}

// ===== VM integration (optional, safe no-op if symbols absent) ==============
static int vm_bind_metrics(VM *vm) {
  if (!vm) return 0;

  // Bind as native functions if VM exposes aux_native_* helpers.
  // These helpers are expected in auxlib.h / vm.h in Vitte Light.
  // Fallback to 0 if not available at link time.

#ifdef AUX_BIND_BEGIN
  AUX_BIND_BEGIN(vm, "metrics");
  AUX_BIND_FN("now_ns", metrics_now_ns);
  AUX_BIND_FN("now_ms", metrics_now_ms);
  AUX_BIND_FN("mono_ns", metrics_mono_ns);
  AUX_BIND_FN("mono_ms", metrics_mono_ms);
  AUX_BIND_FN("sleep_ms", metrics_sleep_ms);
  AUX_BIND_FN("proc_cpu_time_ns", metrics_proc_cpu_time_ns);
  AUX_BIND_FN("proc_rss_bytes", metrics_proc_rss_bytes);
  AUX_BIND_FN("proc_vms_bytes", metrics_proc_vms_bytes);
  AUX_BIND_FN("counter_inc", metrics_counter_inc);
  AUX_BIND_FN("counter_set", metrics_counter_set);
  AUX_BIND_FN("counter_get", metrics_counter_get);
  AUX_BIND_FN("counter_reset", metrics_counter_reset);
  AUX_BIND_FN("tic", metrics_tic);
  AUX_BIND_FN("toc", metrics_toc);
  AUX_BIND_FN("timer_get_ns", metrics_timer_get_ns);
  AUX_BIND_FN("ewma_update", metrics_ewma_update);
  AUX_BIND_FN("ewma_get", metrics_ewma_get);
  AUX_BIND_END();
#endif

  (void)vm;  // if not used
  return 1;
}

// Called by runtime during stdlib init. Weak if the loader expects a symbol.
int vitte_std_metrics_open(VM *vm) {
  // zero registry
  memset(G, 0, sizeof(G));
  return vm_bind_metrics(vm);
}

// ===== snapshot for quick reporting ==========================================
typedef struct {
  u64 now_ns;
  u64 mono_ns;
  u64 cpu_time_ns;
  u64 rss_bytes;
  u64 vms_bytes;
} metrics_snapshot_t;

metrics_snapshot_t metrics_snapshot(void) {
  metrics_snapshot_t s;
  s.now_ns = metrics_now_ns();
  s.mono_ns = metrics_mono_ns();
  s.cpu_time_ns = metrics_proc_cpu_time_ns();
  s.rss_bytes = metrics_proc_rss_bytes();
  s.vms_bytes = metrics_proc_vms_bytes();
  return s;
}
