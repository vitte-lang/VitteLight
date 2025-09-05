// SPDX-License-Identifier: GPL-3.0-or-later
//
// rt.c — Runtime core (event loop, timers, cross-thread posting) for Vitte
// Light (C17, ultra complet)
// ---------------------------------------------------------------------------------------------------
// Features:
//   - Monotonic time helpers: vl_rt_now_ms(), vl_rt_now_ns()
//   - Loop: create/free, run, run_once, stop, idle callback
//   - Task queue: thread-safe vl_rt_post(loop, fn, arg)
//   - Timers: one-shot and periodic, O(log N) min-heap scheduler
//   - Thread-safety: mutex-protected queues + wake-up pipe (POSIX) / event
//   (Windows)
//   - Optional VM bindings (namespace "rt"): now_ms(), hrtime_ns(),
//   run_once([ms]), stop()
//
// Design notes:
//   - All callbacks run on the loop thread.
//   - Timers and tasks may be added from other threads; they wake the loop.
//   - No file-descriptor watchers to keep portability simple. Extend at will.
//
// Depends: includes/auxlib.h (AuxStatus, logging, time, arrays), optional:
// state.h/object.h/vm.h

#include "auxlib.h"
#ifdef HAVE_VM_HEADERS
#include "object.h"
#include "state.h"
#include "vm.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE (-1)
#endif
#endif

// ======================================================================
// Public header fallback
// ======================================================================
#ifndef VITTE_LIGHT_INCLUDES_RT_H
#define VITTE_LIGHT_INCLUDES_RT_H 1

typedef struct VlRtLoop VlRtLoop;
typedef struct VlRtTimer VlRtTimer;

typedef void (*VlRtTaskFn)(void *arg);
typedef void (*VlRtTimerFn)(void *arg);
typedef void (*VlRtIdleFn)(void *arg);  // optional per-iteration idle hook

// Time
uint64_t vl_rt_now_ms(void);  // monotonic
uint64_t vl_rt_now_ns(void);  // monotonic

// Loop lifecycle
AuxStatus vl_rt_loop_new(VlRtLoop **out);
void vl_rt_loop_free(VlRtLoop *L);

// Loop control
void vl_rt_loop_set_idle(VlRtLoop *L, VlRtIdleFn fn,
                         void *arg);  // NULL to clear
AuxStatus vl_rt_run(VlRtLoop *L);     // runs until vl_rt_stop()
AuxStatus vl_rt_run_once(VlRtLoop *L,
                         uint64_t max_wait_ms);  // process once (tasks+timers),
                                                 // wait up to max_wait_ms
void vl_rt_stop(VlRtLoop *L);

// Task queue (thread-safe)
AuxStatus vl_rt_post(VlRtLoop *L, VlRtTaskFn fn, void *arg);

// Timers
AuxStatus vl_rt_timer_init(VlRtTimer **out);
void vl_rt_timer_dispose(VlRtTimer *t);  // safe if inactive
// Start at delay_ms from now, repeat every repeat_ms if >0
AuxStatus vl_rt_timer_start(VlRtLoop *L, VlRtTimer *t, VlRtTimerFn cb,
                            void *arg, uint64_t delay_ms, uint64_t repeat_ms);
AuxStatus vl_rt_timer_stop(VlRtLoop *L, VlRtTimer *t);
int vl_rt_timer_active(const VlRtTimer *t);

// Introspection
size_t vl_rt_pending_tasks(const VlRtLoop *L);
size_t vl_rt_active_timers(const VlRtLoop *L);

#endif  // VITTE_LIGHT_INCLUDES_RT_H

// ======================================================================
// Internals
// ======================================================================

typedef struct VlRtTask {
  VlRtTaskFn fn;
  void *arg;
  struct VlRtTask *next;
} VlRtTask;

struct VlRtTimer {
  VlRtTimerFn cb;
  void *arg;
  uint64_t due_ms;
  uint64_t repeat_ms;
  size_t heap_idx;  // 1-based index in heap (0 == not in heap)
};

typedef struct VlRtHeap {
  VlRtTimer **a;  // 1-based array
  size_t n, cap;
} VlRtHeap;

#if defined(_WIN32)
typedef struct {
  HANDLE evt;  // manual-reset event
} VlWake;
#else
typedef struct {
  int rd, wr;  // pipe ends
} VlWake;
#endif

typedef struct VlMutexLite {
#if defined(_WIN32)
  CRITICAL_SECTION cs;
#else
  // simple non-recursive mutex; could be replaced by pthread_mutex_t in
  // pthread.c if desired But we stay self-contained using a minimal spin +
  // futex-less fallback with select sleep. For portability and simplicity: use
  // a POSIX mutex when available. We assume POSIX here.
  pthread_mutex_t m;
#endif
} VlMutexLite;

struct VlRtLoop {
  volatile int stop_flag;

  // tasks
  VlMutexLite q_mtx;
  VlRtTask *q_head;
  VlRtTask *q_tail;
  size_t q_count;

  // timers
  VlMutexLite t_mtx;  // protects heap operations
  VlRtHeap heap;

  // idle
  VlRtIdleFn idle_fn;
  void *idle_arg;

  // wakeup
  VlWake wake;
};

// ---------- time helpers ----------
uint64_t vl_rt_now_ms(void) { return aux_now_millis(); }
uint64_t vl_rt_now_ns(void) { return aux_now_nanos(); }

// ---------- tiny mutex ----------
static void mtx_init(VlMutexLite *m) {
#if defined(_WIN32)
  InitializeCriticalSection(&m->cs);
#else
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_NORMAL);
  pthread_mutex_init(&m->m, &a);
  pthread_mutexattr_destroy(&a);
#endif
}
static void mtx_destroy(VlMutexLite *m) {
#if defined(_WIN32)
  DeleteCriticalSection(&m->cs);
#else
  pthread_mutex_destroy(&m->m);
#endif
}
static void mtx_lock(VlMutexLite *m) {
#if defined(_WIN32)
  EnterCriticalSection(&m->cs);
#else
  pthread_mutex_lock(&m->m);
#endif
}
static void mtx_unlock(VlMutexLite *m) {
#if defined(_WIN32)
  LeaveCriticalSection(&m->cs);
#else
  pthread_mutex_unlock(&m->m);
#endif
}

// ---------- wake mechanism ----------
static AuxStatus wake_init(VlWake *w) {
#if defined(_WIN32)
  w->evt = CreateEventA(NULL, TRUE, FALSE, NULL);  // manual-reset, non-signaled
  return w->evt ? AUX_OK : AUX_EIO;
#else
  int p[2];
#if defined(__linux__)
  if (pipe(p) != 0) return AUX_EIO;
#else
  if (pipe(p) != 0) return AUX_EIO;
#endif
  // non-blocking optional
  int fl;
  fl = fcntl(p[0], F_GETFL, 0);
  fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
  fl = fcntl(p[1], F_GETFL, 0);
  fcntl(p[1], F_SETFL, fl | O_NONBLOCK);
  w->rd = p[0];
  w->wr = p[1];
  return AUX_OK;
#endif
}
static void wake_destroy(VlWake *w) {
#if defined(_WIN32)
  if (w->evt) CloseHandle(w->evt);
  w->evt = NULL;
#else
  if (w->rd != INVALID_HANDLE_VALUE) close(w->rd);
  if (w->wr != INVALID_HANDLE_VALUE) close(w->wr);
  w->rd = w->wr = INVALID_HANDLE_VALUE;
#endif
}
static void wake_signal(VlWake *w) {
#if defined(_WIN32)
  SetEvent(w->evt);
#else
  const uint8_t b = 1;
  (void)write(w->wr, &b, 1);  // ignore EAGAIN
#endif
}
static void wake_consume(VlWake *w) {
#if defined(_WIN32)
  ResetEvent(w->evt);
#else
  uint8_t buf[64];
  while (read(w->rd, buf, sizeof buf) > 0) {
  }  // drain
#endif
}

// ---------- timer heap ----------
static void heap_swap(VlRtHeap *h, size_t i, size_t j) {
  VlRtTimer *ti = h->a[i], *tj = h->a[j];
  h->a[i] = tj;
  h->a[j] = ti;
  tj->heap_idx = i;
  ti->heap_idx = j;
}
static int heap_less(VlRtTimer *a, VlRtTimer *b) {
  if (a->due_ms < b->due_ms) return 1;
  if (a->due_ms > b->due_ms) return 0;
  return (uintptr_t)a < (uintptr_t)b;  // tie-breaker for stability
}
static void heap_up(VlRtHeap *h, size_t i) {
  while (i > 1) {
    size_t p = i >> 1;
    if (heap_less(h->a[p], h->a[i])) break;
    heap_swap(h, i, p);
    i = p;
  }
}
static void heap_down(VlRtHeap *h, size_t i) {
  for (;;) {
    size_t l = i << 1, r = l + 1, m = i;
    if (l <= h->n && heap_less(h->a[l], h->a[m])) m = l;
    if (r <= h->n && heap_less(h->a[r], h->a[m])) m = r;
    if (m == i) break;
    heap_swap(h, i, m);
    i = m;
  }
}
static AuxStatus heap_reserve(VlRtHeap *h, size_t cap) {
  if (cap <= h->cap) return AUX_OK;
  size_t ncap = h->cap ? h->cap : 8;
  while (ncap < cap) ncap <<= 1;
  VlRtTimer **na = (VlRtTimer **)realloc(h->a, (ncap + 1) * sizeof *na);
  if (!na) return AUX_ENOMEM;
  h->a = na;
  h->cap = ncap;
  return AUX_OK;
}
static AuxStatus heap_push(VlRtHeap *h, VlRtTimer *t) {
  AuxStatus s = heap_reserve(h, h->n + 1);
  if (s != AUX_OK) return s;
  h->a[++h->n] = t;
  t->heap_idx = h->n;
  heap_up(h, h->n);
  return AUX_OK;
}
static VlRtTimer *heap_pop(VlRtHeap *h) {
  if (h->n == 0) return NULL;
  VlRtTimer *t = h->a[1];
  h->a[1] = h->a[h->n--];
  if (h->n) {
    h->a[1]->heap_idx = 1;
    heap_down(h, 1);
  }
  t->heap_idx = 0;
  return t;
}
static void heap_erase(VlRtHeap *h, VlRtTimer *t) {
  size_t i = t->heap_idx;
  if (i == 0 || i > h->n) return;
  if (i == h->n) {
    h->a[h->n--] = NULL;
    t->heap_idx = 0;
    return;
  }
  h->a[i] = h->a[h->n];
  h->a[i]->heap_idx = i;
  h->a[h->n--] = NULL;
  t->heap_idx = 0;
  heap_down(h, i);
  heap_up(h, i);
}

// ======================================================================
// Public API
// ======================================================================

AuxStatus vl_rt_loop_new(VlRtLoop **out) {
  if (!out) return AUX_EINVAL;
  VlRtLoop *L = (VlRtLoop *)calloc(1, sizeof *L);
  if (!L) return AUX_ENOMEM;
  mtx_init(&L->q_mtx);
  mtx_init(&L->t_mtx);
#if defined(_WIN32)
  L->wake.evt = NULL;
#else
  L->wake.rd = L->wake.wr = INVALID_HANDLE_VALUE;
#endif
  AuxStatus s = wake_init(&L->wake);
  if (s != AUX_OK) {
    mtx_destroy(&L->q_mtx);
    mtx_destroy(&L->t_mtx);
    free(L);
    return s;
  }
  *out = L;
  return AUX_OK;
}

void vl_rt_loop_free(VlRtLoop *L) {
  if (!L) return;
  L->stop_flag = 1;
  wake_signal(&L->wake);

  // drain tasks
  mtx_lock(&L->q_mtx);
  VlRtTask *t = L->q_head;
  while (t) {
    VlRtTask *n = t->next;
    free(t);
    t = n;
  }
  L->q_head = L->q_tail = NULL;
  L->q_count = 0;
  mtx_unlock(&L->q_mtx);

  // clear timers
  mtx_lock(&L->t_mtx);
  for (size_t i = 1; i <= L->heap.n; ++i) {
    if (L->heap.a[i]) L->heap.a[i]->heap_idx = 0;
  }
  free(L->heap.a);
  L->heap.a = NULL;
  L->heap.n = L->heap.cap = 0;
  mtx_unlock(&L->t_mtx);

  wake_destroy(&L->wake);
  mtx_destroy(&L->q_mtx);
  mtx_destroy(&L->t_mtx);
  free(L);
}

void vl_rt_loop_set_idle(VlRtLoop *L, VlRtIdleFn fn, void *arg) {
  if (!L) return;
  L->idle_fn = fn;
  L->idle_arg = arg;
}

AuxStatus vl_rt_post(VlRtLoop *L, VlRtTaskFn fn, void *arg) {
  if (!L || !fn) return AUX_EINVAL;
  VlRtTask *t = (VlRtTask *)calloc(1, sizeof *t);
  if (!t) return AUX_ENOMEM;
  t->fn = fn;
  t->arg = arg;
  mtx_lock(&L->q_mtx);
  if (L->q_tail)
    L->q_tail->next = t;
  else
    L->q_head = t;
  L->q_tail = t;
  L->q_count++;
  mtx_unlock(&L->q_mtx);
  wake_signal(&L->wake);
  return AUX_OK;
}

AuxStatus vl_rt_timer_init(VlRtTimer **out) {
  if (!out) return AUX_EINVAL;
  VlRtTimer *t = (VlRtTimer *)calloc(1, sizeof *t);
  if (!t) return AUX_ENOMEM;
  *out = t;
  return AUX_OK;
}

void vl_rt_timer_dispose(VlRtTimer *t) {
  if (!t) return;
  // If still active, user should have stopped it before disposing.
  free(t);
}

AuxStatus vl_rt_timer_start(VlRtLoop *L, VlRtTimer *t, VlRtTimerFn cb,
                            void *arg, uint64_t delay_ms, uint64_t repeat_ms) {
  if (!L || !t || !cb) return AUX_EINVAL;
  if (repeat_ms && repeat_ms < 1) repeat_ms = 1;
  t->cb = cb;
  t->arg = arg;
  t->repeat_ms = repeat_ms;
  t->due_ms = vl_rt_now_ms() + delay_ms;
  // insert into heap
  mtx_lock(&L->t_mtx);
  // if already active, remove first
  if (t->heap_idx) heap_erase(&L->heap, t);
  AuxStatus s = heap_push(&L->heap, t);
  mtx_unlock(&L->t_mtx);
  if (s != AUX_OK) return s;
  wake_signal(&L->wake);
  return AUX_OK;
}

AuxStatus vl_rt_timer_stop(VlRtLoop *L, VlRtTimer *t) {
  if (!L || !t) return AUX_EINVAL;
  mtx_lock(&L->t_mtx);
  if (t->heap_idx) heap_erase(&L->heap, t);
  mtx_unlock(&L->t_mtx);
  return AUX_OK;
}

int vl_rt_timer_active(const VlRtTimer *t) { return t && t->heap_idx != 0; }

size_t vl_rt_pending_tasks(const VlRtLoop *L) { return L ? L->q_count : 0; }
size_t vl_rt_active_timers(const VlRtLoop *L) {
  size_t n = 0;
  if (!L) return 0;
  mtx_lock((VlMutexLite *)&L->t_mtx);
  n = L->heap.n;
  mtx_unlock((VlMutexLite *)&L->t_mtx);
  return n;
}

void vl_rt_stop(VlRtLoop *L) {
  if (!L) return;
  L->stop_flag = 1;
  wake_signal(&L->wake);
}

// ---------- core processing ----------

static void process_tasks(VlRtLoop *L) {
  // swap queue under lock, then run without lock
  mtx_lock(&L->q_mtx);
  VlRtTask *head = L->q_head;
  VlRtTask *tail = L->q_tail;
  L->q_head = L->q_tail = NULL;
  size_t n = L->q_count;
  L->q_count = 0;
  (void)n;
  (void)tail;
  mtx_unlock(&L->q_mtx);

  for (VlRtTask *t = head; t;) {
    VlRtTask *nxt = t->next;
    if (t->fn) t->fn(t->arg);
    free(t);
    t = nxt;
  }
}

static uint64_t process_timers(VlRtLoop *L, uint64_t now_ms) {
  // Run all due timers. Return ms until next timer, or UINT64_MAX if none.
  uint64_t next_wait = UINT64_MAX;

  mtx_lock(&L->t_mtx);
  for (;;) {
    if (L->heap.n == 0) {
      next_wait = UINT64_MAX;
      break;
    }
    VlRtTimer *t = L->heap.a[1];
    if (t->due_ms > now_ms) {
      uint64_t delta = t->due_ms - now_ms;
      next_wait = delta;
      break;
    }
    // pop and run outside lock? We need to re-arm periodic ones.
    (void)heap_pop(&L->heap);
    uint64_t repeat = t->repeat_ms;
    // Use a small local copy
    VlRtTimerFn cb = t->cb;
    void *arg = t->arg;
    mtx_unlock(&L->t_mtx);

    // callback without locks
    if (cb) cb(arg);

    // re-acquire to possibly reinsert
    mtx_lock(&L->t_mtx);
    if (repeat) {
      uint64_t due = now_ms + repeat;
      // In case callback took long, compute based on "now", not previous due
      t->due_ms = due;
      (void)heap_push(&L->heap, t);
    } else {
      t->heap_idx = 0;
    }
    // loop to check further due timers
    now_ms = vl_rt_now_ms();
  }
  mtx_unlock(&L->t_mtx);
  return next_wait;
}

AuxStatus vl_rt_run_once(VlRtLoop *L, uint64_t max_wait_ms) {
  if (!L) return AUX_EINVAL;

  // 1) Drain tasks immediately
  process_tasks(L);
  if (L->stop_flag) return AUX_OK;

  // 2) Timers due now
  uint64_t now = vl_rt_now_ms();
  uint64_t wait_ms = process_timers(L, now);
  if (L->stop_flag) return AUX_OK;

  // 3) Idle hook
  if (L->idle_fn) L->idle_fn(L->idle_arg);

  // 4) Wait for either wake or next timer
  uint64_t cap = max_wait_ms;
  if (wait_ms == UINT64_MAX) {
    // no timers
    wait_ms = cap;
  } else {
    if (cap < wait_ms) wait_ms = cap;
  }

#if defined(_WIN32)
  DWORD to = (wait_ms == UINT64_MAX)
                 ? INFINITE
                 : (DWORD)((wait_ms > 0xFFFFFFFEu) ? 0xFFFFFFFEu : wait_ms);
  DWORD rc = WaitForSingleObject(L->wake.evt, to);
  if (rc == WAIT_OBJECT_0) wake_consume(&L->wake);
#else
  struct timeval tv, *ptv = NULL;
  if (wait_ms != UINT64_MAX) {
    tv.tv_sec = (time_t)(wait_ms / 1000ULL);
    tv.tv_usec = (suseconds_t)((wait_ms % 1000ULL) * 1000ULL);
    ptv = &tv;
  }
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(L->wake.rd, &rfds);
  (void)select(L->wake.rd + 1, &rfds, NULL, NULL, ptv);
  if (FD_ISSET(L->wake.rd, &rfds)) wake_consume(&L->wake);
#endif

  return AUX_OK;
}

AuxStatus vl_rt_run(VlRtLoop *L) {
  if (!L) return AUX_EINVAL;
  L->stop_flag = 0;
  for (;;) {
    if (L->stop_flag) break;
    AuxStatus s = vl_rt_run_once(L, /*max_wait_ms*/ 1000);
    if (s != AUX_OK) return s;
  }
  return AUX_OK;
}

// ======================================================================
// Optional VM bindings (minimal, no function scheduling)
//   Namespace "rt":
//     rt.now_ms()            -> int64
//     rt.hrtime_ns()         -> int64
//     rt.run_once([ms])      -> true
//     rt.stop()              -> true
//   The embedding code can keep a global loop and drive it via VM.
// ======================================================================
#ifdef HAVE_VM_HEADERS

static VlRtLoop *g_rt_loop = NULL;

static int vmrt_now_ms(VL_State *S) {
  (void)S;
  vl_push_int(S, (int64_t)vl_rt_now_ms());
  return 1;
}
static int vmrt_hrtime_ns(VL_State *S) {
  (void)S;
  vl_push_int(S, (int64_t)vl_rt_now_ns());
  return 1;
}
static int vmrt_run_once(VL_State *S) {
  uint64_t ms = 1000;
  if (vl_get(S, 1) && (vl_isint(S, 1) || vl_isfloat(S, 1))) {
    ms = (uint64_t)(vl_isint(S, 1) ? vl_toint(S, vl_get(S, 1))
                                   : (int64_t)vl_tonumber(S, vl_get(S, 1)));
  }
  if (!g_rt_loop) {
    if (vl_rt_loop_new(&g_rt_loop) != AUX_OK) {
      vl_push_nil(S);
      vl_push_string(S, "EIO");
      return 2;
    }
  }
  vl_rt_run_once(g_rt_loop, ms);
  vl_push_bool(S, 1);
  return 1;
}
static int vmrt_stop(VL_State *S) {
  (void)S;
  if (g_rt_loop) vl_rt_stop(g_rt_loop);
  vl_push_bool(S, 1);
  return 1;
}

static const VL_Reg rtlib[] = {{"now_ms", vmrt_now_ms},
                               {"hrtime_ns", vmrt_hrtime_ns},
                               {"run_once", vmrt_run_once},
                               {"stop", vmrt_stop},
                               {NULL, NULL}};

void vl_open_rtlib(VL_State *S) { vl_register_lib(S, "rt", rtlib); }

#endif  // HAVE_VM_HEADERS

// ======================================================================
// End
// ======================================================================
