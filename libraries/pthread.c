// SPDX-License-Identifier: GPL-3.0-or-later
//
// pthread.c — Threading primitives for Vitte Light (C17, ultra complet)
// ---------------------------------------------------------------------
// Portabilité:
//   - POSIX: pthreads (+ rwlock, cond, TLS, once). Utilise
//   nanosleep/sched_yield.
//   - Windows: CreateThread, SRWLOCK, CONDITION_VARIABLE, TLS, InitOnce.
//   - Sémophore: natif (POSIX sem) si dispo, sinon fallback mutex+cond.
//   - Stubs si aucune API disponible -> AUX_ENOSYS.
//
// API C (header de secours fourni ci-dessous si includes/thread.h absent):
//   Types opaques: VlThread, VlMutex, VlRWLock, VlCond, VlSem, VlTlsKey, VlOnce
//   Threads:
//     AuxStatus vl_thread_create(VlThread **out, void *(*fn)(void*), void *arg,
//                                size_t stack_size, int detached);
//     AuxStatus vl_thread_join(VlThread *t, void **ret);
//     AuxStatus vl_thread_detach(VlThread *t);
//     void*     vl_thread_handle(VlThread *t);                // pointeur natif
//     (best effort) uint64_t  vl_thread_current_id(void);                   //
//     identifiant stable best-effort void      vl_thread_yield(void); void
//     vl_thread_sleep_ms(uint64_t ms);
//
//   Mutex:
//     AuxStatus vl_mutex_init(VlMutex *m, int recursive);     // 0|1
//     void      vl_mutex_destroy(VlMutex *m);
//     AuxStatus vl_mutex_lock(VlMutex *m);
//     AuxStatus vl_mutex_trylock(VlMutex *m);                 // AUX_BUSY si
//     occupé AuxStatus vl_mutex_unlock(VlMutex *m);
//
//   RWLock:
//     AuxStatus vl_rwlock_init(VlRWLock *rw);
//     void      vl_rwlock_destroy(VlRWLock *rw);
//     AuxStatus vl_rwlock_rdlock(VlRWLock *rw);
//     AuxStatus vl_rwlock_wrlock(VlRWLock *rw);
//     AuxStatus vl_rwlock_tryrdlock(VlRWLock *rw);
//     AuxStatus vl_rwlock_trywrlock(VlRWLock *rw);
//     AuxStatus vl_rwlock_unlock(VlRWLock *rw);
//
//   Condition:
//     AuxStatus vl_cond_init(VlCond *c);
//     void      vl_cond_destroy(VlCond *c);
//     AuxStatus vl_cond_wait(VlCond *c, VlMutex *m);
//     AuxStatus vl_cond_timedwait_ms(VlCond *c, VlMutex *m, uint64_t
//     timeout_ms); // AUX_ETIMEDOUT AuxStatus vl_cond_signal(VlCond *c);
//     AuxStatus vl_cond_broadcast(VlCond *c);
//
//   Sémaphore:
//     AuxStatus vl_sem_init(VlSem *s, unsigned initial);
//     void      vl_sem_destroy(VlSem *s);
//     AuxStatus vl_sem_post(VlSem *s);
//     AuxStatus vl_sem_wait(VlSem *s);
//     AuxStatus vl_sem_trywait(VlSem *s);                     // AUX_BUSY
//     AuxStatus vl_sem_timedwait_ms(VlSem *s, uint64_t timeout_ms); //
//     AUX_ETIMEDOUT
//
//   TLS (thread local storage):
//     AuxStatus vl_tlskey_create(VlTlsKey *k, void (*dtor)(void*));
//     void      vl_tlskey_delete(VlTlsKey *k);
//     AuxStatus vl_tls_set(VlTlsKey *k, void *ptr);
//     void*     vl_tls_get(VlTlsKey *k);
//
//   Once:
//     void      vl_once_init(VlOnce *o);
//     int       vl_once_run(VlOnce *o, void (*initfn)(void)); // 1 si exécuté,
//     0 si déjà fait
//
//   Fences (optionnel, C11 atomics si dispo):
//     void      vl_fence_acquire(void);
//     void      vl_fence_release(void);
//     void      vl_fence_seq_cst(void);
//
// VM (facultatif, sûr pour une VM non thread-safe):
//   Namespace "thread":
//     thread.sleep_ms(ms)
//     thread.yield()
//     thread.current_id() -> int
//     thread.cpu_count()  -> int
//
// Dépendances: auxlib.h (+ AuxStatus), state.h/object.h/vm.h pour la partie VM.

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <synchapi.h>
#include <windows.h>
#else
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <unistd.h>
#if __has_include(<semaphore.h>)
#include <semaphore.h>
#define VL_HAVE_POSIX_SEM 1
#endif
#endif

// ======================================================================
// Header de secours (si includes/thread.h absent)
// ======================================================================
#ifndef VITTE_LIGHT_INCLUDES_THREAD_H
#define VITTE_LIGHT_INCLUDES_THREAD_H 1
typedef struct VlThread VlThread;
typedef struct VlMutex VlMutex;
typedef struct VlRWLock VlRWLock;
typedef struct VlCond VlCond;
typedef struct VlSem VlSem;
typedef struct VlTlsKey VlTlsKey;
typedef struct VlOnce VlOnce;

// Threads
AuxStatus vl_thread_create(VlThread **out, void *(*fn)(void *), void *arg,
                           size_t stack_size, int detached);
AuxStatus vl_thread_join(VlThread *t, void **ret);
AuxStatus vl_thread_detach(VlThread *t);
void *vl_thread_handle(VlThread *t);
uint64_t vl_thread_current_id(void);
void vl_thread_yield(void);
void vl_thread_sleep_ms(uint64_t ms);

// Mutex
AuxStatus vl_mutex_init(VlMutex *m, int recursive);
void vl_mutex_destroy(VlMutex *m);
AuxStatus vl_mutex_lock(VlMutex *m);
AuxStatus vl_mutex_trylock(VlMutex *m);
AuxStatus vl_mutex_unlock(VlMutex *m);

// RWLock
AuxStatus vl_rwlock_init(VlRWLock *rw);
void vl_rwlock_destroy(VlRWLock *rw);
AuxStatus vl_rwlock_rdlock(VlRWLock *rw);
AuxStatus vl_rwlock_wrlock(VlRWLock *rw);
AuxStatus vl_rwlock_tryrdlock(VlRWLock *rw);
AuxStatus vl_rwlock_trywrlock(VlRWLock *rw);
AuxStatus vl_rwlock_unlock(VlRWLock *rw);

// Cond
AuxStatus vl_cond_init(VlCond *c);
void vl_cond_destroy(VlCond *c);
AuxStatus vl_cond_wait(VlCond *c, VlMutex *m);
AuxStatus vl_cond_timedwait_ms(VlCond *c, VlMutex *m, uint64_t timeout_ms);
AuxStatus vl_cond_signal(VlCond *c);
AuxStatus vl_cond_broadcast(VlCond *c);

// Sem
AuxStatus vl_sem_init(VlSem *s, unsigned initial);
void vl_sem_destroy(VlSem *s);
AuxStatus vl_sem_post(VlSem *s);
AuxStatus vl_sem_wait(VlSem *s);
AuxStatus vl_sem_trywait(VlSem *s);
AuxStatus vl_sem_timedwait_ms(VlSem *s, uint64_t timeout_ms);

// TLS
AuxStatus vl_tlskey_create(VlTlsKey *k, void (*dtor)(void *));
void vl_tlskey_delete(VlTlsKey *k);
AuxStatus vl_tls_set(VlTlsKey *k, void *ptr);
void *vl_tls_get(VlTlsKey *k);

// Once
void vl_once_init(VlOnce *o);
int vl_once_run(VlOnce *o, void (*initfn)(void));

// Fences
void vl_fence_acquire(void);
void vl_fence_release(void);
void vl_fence_seq_cst(void);

// VM
void vl_open_threadlib(VL_State *S);
#endif

// ======================================================================
// Implémentation par plateforme
// ======================================================================

#if defined(_WIN32)
// ------------------------------ Windows -------------------------------
struct VlThread {
  HANDLE h;
  uint64_t id;
  int detached;
  void *(*fn)(void *);
  void *arg;
  void *ret;
};

static unsigned __stdcall vl__thunk(void *p) {
  VlThread *t = (VlThread *)p;
  void *r = NULL;
  if (t && t->fn) r = t->fn(t->arg);
  t->ret = r;
  return 0;
}

AuxStatus vl_thread_create(VlThread **out, void *(*fn)(void *), void *arg,
                           size_t stack_size, int detached) {
  if (!out || !fn) return AUX_EINVAL;
  VlThread *t = (VlThread *)calloc(1, sizeof *t);
  if (!t) return AUX_ENOMEM;
  t->fn = fn;
  t->arg = arg;
  t->detached = detached ? 1 : 0;
  uintptr_t th = _beginthreadex(NULL, (unsigned)(stack_size ? stack_size : 0),
                                vl__thunk, t, 0, (unsigned *)&t->id);
  if (!th) {
    free(t);
    return AUX_EIO;
  }
  t->h = (HANDLE)th;
  if (t->detached) {
    CloseHandle(t->h);
    t->h = NULL;
  }
  *out = t;
  return AUX_OK;
}

AuxStatus vl_thread_join(VlThread *t, void **ret) {
  if (!t || t->detached || !t->h) return AUX_EINVAL;
  DWORD rc = WaitForSingleObject(t->h, INFINITE);
  if (rc != WAIT_OBJECT_0) return AUX_EIO;
  if (ret) *ret = t->ret;
  CloseHandle(t->h);
  t->h = NULL;
  free(t);
  return AUX_OK;
}

AuxStatus vl_thread_detach(VlThread *t) {
  if (!t || t->detached) return AUX_EINVAL;
  if (t->h) CloseHandle(t->h);
  t->h = NULL;
  t->detached = 1;
  free(t);
  return AUX_OK;
}

void *vl_thread_handle(VlThread *t) { return t ? (void *)t->h : NULL; }
uint64_t vl_thread_current_id(void) { return (uint64_t)GetCurrentThreadId(); }
void vl_thread_yield(void) { SwitchToThread(); }
void vl_thread_sleep_ms(uint64_t ms) {
  Sleep((DWORD)(ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : ms));
}

// Mutex
typedef struct {
  CRITICAL_SECTION cs;
  int is_init;
} WinCS;
struct VlMutex {
  WinCS cs;
};

AuxStatus vl_mutex_init(VlMutex *m, int recursive) {
  (void)recursive;
  if (!m) return AUX_EINVAL;
  InitializeCriticalSection(&m->cs.cs);
  m->cs.is_init = 1;
  return AUX_OK;
}
void vl_mutex_destroy(VlMutex *m) {
  if (m && m->cs.is_init) {
    DeleteCriticalSection(&m->cs.cs);
    m->cs.is_init = 0;
  }
}
AuxStatus vl_mutex_lock(VlMutex *m) {
  if (!m || !m->cs.is_init) return AUX_EINVAL;
  EnterCriticalSection(&m->cs.cs);
  return AUX_OK;
}
AuxStatus vl_mutex_trylock(VlMutex *m) {
  if (!m || !m->cs.is_init) return AUX_EINVAL;
  return TryEnterCriticalSection(&m->cs.cs) ? AUX_OK : AUX_BUSY;
}
AuxStatus vl_mutex_unlock(VlMutex *m) {
  if (!m || !m->cs.is_init) return AUX_EINVAL;
  LeaveCriticalSection(&m->cs.cs);
  return AUX_OK;
}

// RWLock
struct VlRWLock {
  SRWLOCK l;
  int inited;
};
AuxStatus vl_rwlock_init(VlRWLock *rw) {
  if (!rw) return AUX_EINVAL;
  InitializeSRWLock(&rw->l);
  rw->inited = 1;
  return AUX_OK;
}
void vl_rwlock_destroy(VlRWLock *rw) { (void)rw; }
AuxStatus vl_rwlock_rdlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  AcquireSRWLockShared(&rw->l);
  return AUX_OK;
}
AuxStatus vl_rwlock_wrlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  AcquireSRWLockExclusive(&rw->l);
  return AUX_OK;
}
AuxStatus vl_rwlock_tryrdlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  return TryAcquireSRWLockShared(&rw->l) ? AUX_OK : AUX_BUSY;
}
AuxStatus vl_rwlock_trywrlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  return TryAcquireSRWLockExclusive(&rw->l) ? AUX_OK : AUX_BUSY;
}
AuxStatus vl_rwlock_unlock(VlRWLock *rw) {
  if (!rw || !rw->inited)
    return AUX_EINVAL;  // unknown mode, prefer exclusive release try
  // Windows requires matching release; we cannot detect mode. Provide both
  // attempts.
  ReleaseSRWLockExclusive(&rw->l);  // harmless if not held exclusively
  ReleaseSRWLockShared(&rw->l);
  return AUX_OK;
}

// Cond
struct VlCond {
  CONDITION_VARIABLE cv;
  int inited;
};
AuxStatus vl_cond_init(VlCond *c) {
  if (!c) return AUX_EINVAL;
  InitializeConditionVariable(&c->cv);
  c->inited = 1;
  return AUX_OK;
}
void vl_cond_destroy(VlCond *c) { (void)c; }
AuxStatus vl_cond_wait(VlCond *c, VlMutex *m) {
  if (!c || !m || !c->inited || !m->cs.is_init) return AUX_EINVAL;
  return SleepConditionVariableCS(&c->cv, &m->cs.cs, INFINITE) ? AUX_OK
                                                               : AUX_EIO;
}
AuxStatus vl_cond_timedwait_ms(VlCond *c, VlMutex *m, uint64_t ms) {
  if (!c || !m || !c->inited || !m->cs.is_init) return AUX_EINVAL;
  BOOL ok = SleepConditionVariableCS(
      &c->cv, &m->cs.cs, (DWORD)(ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : ms));
  if (ok) return AUX_OK;
  return GetLastError() == ERROR_TIMEOUT ? AUX_ETIMEDOUT : AUX_EIO;
}
AuxStatus vl_cond_signal(VlCond *c) {
  if (!c || !c->inited) return AUX_EINVAL;
  WakeConditionVariable(&c->cv);
  return AUX_OK;
}
AuxStatus vl_cond_broadcast(VlCond *c) {
  if (!c || !c->inited) return AUX_EINVAL;
  WakeAllConditionVariable(&c->cv);
  return AUX_OK;
}

// Sem
struct VlSem {
  HANDLE h;
};
AuxStatus vl_sem_init(VlSem *s, unsigned initial) {
  if (!s) return AUX_EINVAL;
  s->h = CreateSemaphoreA(NULL, (LONG)initial, 0x7fffffff, NULL);
  return s->h ? AUX_OK : AUX_EIO;
}
void vl_sem_destroy(VlSem *s) {
  if (s && s->h) {
    CloseHandle(s->h);
    s->h = NULL;
  }
}
AuxStatus vl_sem_post(VlSem *s) {
  if (!s || !s->h) return AUX_EINVAL;
  return ReleaseSemaphore(s->h, 1, NULL) ? AUX_OK : AUX_EIO;
}
AuxStatus vl_sem_wait(VlSem *s) {
  if (!s || !s->h) return AUX_EINVAL;
  DWORD rc = WaitForSingleObject(s->h, INFINITE);
  return rc == WAIT_OBJECT_0 ? AUX_OK : AUX_EIO;
}
AuxStatus vl_sem_trywait(VlSem *s) {
  if (!s || !s->h) return AUX_EINVAL;
  DWORD rc = WaitForSingleObject(s->h, 0);
  return rc == WAIT_OBJECT_0 ? AUX_OK : AUX_BUSY;
}
AuxStatus vl_sem_timedwait_ms(VlSem *s, uint64_t ms) {
  if (!s || !s->h) return AUX_EINVAL;
  DWORD rc =
      WaitForSingleObject(s->h, (DWORD)(ms > 0xFFFFFFFFu ? 0xFFFFFFFFu : ms));
  if (rc == WAIT_OBJECT_0) return AUX_OK;
  return rc == WAIT_TIMEOUT ? AUX_ETIMEDOUT : AUX_EIO;
}

// TLS
struct VlTlsKey {
  DWORD k;
  int inited;
  void (*dtor)(void *);
};
AuxStatus vl_tlskey_create(VlTlsKey *k, void (*dtor)(void *)) {
  if (!k) return AUX_EINVAL;
  DWORD idx = TlsAlloc();
  if (idx == TLS_OUT_OF_INDEXES) return AUX_EIO;
  k->k = idx;
  k->inited = 1;
  k->dtor = dtor;
  return AUX_OK;
}
void vl_tlskey_delete(VlTlsKey *k) {
  if (k && k->inited) {  // no destructor iteration on Windows
    TlsFree(k->k);
    k->inited = 0;
    k->k = 0;
    k->dtor = NULL;
  }
}
AuxStatus vl_tls_set(VlTlsKey *k, void *ptr) {
  if (!k || !k->inited) return AUX_EINVAL;
  return TlsSetValue(k->k, ptr) ? AUX_OK : AUX_EIO;
}
void *vl_tls_get(VlTlsKey *k) {
  if (!k || !k->inited) return NULL;
  return TlsGetValue(k->k);
}

// Once
struct VlOnce {
  INIT_ONCE once;
  int initd;
};
void vl_once_init(VlOnce *o) {
  if (o) {
    o->once = INIT_ONCE_STATIC_INIT;
    o->initd = 1;
  }
}
static BOOL CALLBACK vl__once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)ctx;
  void (*fn)(void) = (void (*)(void))param;
  if (fn) fn();
  return TRUE;
}
int vl_once_run(VlOnce *o, void (*initfn)(void)) {
  if (!o || !o->initd || !initfn) return 0;
  InitOnceExecuteOnce(&o->once, vl__once_cb, initfn, NULL);
  return 1;
}

// Fences
void vl_fence_acquire(void) { MemoryBarrier(); }
void vl_fence_release(void) { MemoryBarrier(); }
void vl_fence_seq_cst(void) { MemoryBarrier(); }

#else
// ------------------------------ POSIX -------------------------------
struct VlThread {
  pthread_t th;
  int detached;
};

static void *vl__start(void *pwrap) {
  // pwrap est une struct { fn,arg } allouée avant pthread_create
  void **pp = (void **)pwrap;
  void *(*fn)(void *) = (void *(*)(void *))pp[0];
  void *arg = pp[1];
  free(pwrap);
  return fn ? fn(arg) : NULL;
}

AuxStatus vl_thread_create(VlThread **out, void *(*fn)(void *), void *arg,
                           size_t stack_size, int detached) {
  if (!out || !fn) return AUX_EINVAL;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (stack_size) pthread_attr_setstacksize(&attr, stack_size);
  if (detached) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  VlThread *t = (VlThread *)calloc(1, sizeof *t);
  if (!t) {
    pthread_attr_destroy(&attr);
    return AUX_ENOMEM;
  }
  t->detached = detached ? 1 : 0;

  // pack fn+arg
  void **pack = (void **)malloc(2 * sizeof(void *));
  if (!pack) {
    free(t);
    pthread_attr_destroy(&attr);
    return AUX_ENOMEM;
  }
  pack[0] = (void *)fn;
  pack[1] = arg;

  int rc = pthread_create(&t->th, &attr, vl__start, pack);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    free(pack);
    free(t);
    return AUX_EIO;
  }
  *out = t;
  return AUX_OK;
}

AuxStatus vl_thread_join(VlThread *t, void **ret) {
  if (!t || t->detached) return AUX_EINVAL;
  void *r = NULL;
  int rc = pthread_join(t->th, &r);
  if (rc != 0) return AUX_EIO;
  if (ret) *ret = r;
  free(t);
  return AUX_OK;
}

AuxStatus vl_thread_detach(VlThread *t) {
  if (!t || t->detached) return AUX_EINVAL;
  int rc = pthread_detach(t->th);
  if (rc != 0) return AUX_EIO;
  t->detached = 1;
  free(t);
  return AUX_OK;
}

void *vl_thread_handle(VlThread *t) { return t ? (void *)t->th : NULL; }

uint64_t vl_thread_current_id(void) {
#if defined(__APPLE__)
  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);
  return tid;
#elif defined(__linux__)
  return (uint64_t)syscall(186 /* gettid on many archs */);
#else
  // fallback hash of pthread_self
  union {
    pthread_t t;
    uint64_t u[2];
  } u = {.t = pthread_self()};
  return u.u[0] ^ u.u[1];
#endif
}

void vl_thread_yield(void) { sched_yield(); }

void vl_thread_sleep_ms(uint64_t ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000ULL);
  ts.tv_nsec = (long)((ms % 1000ULL) * 1000000L);
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

// Mutex
struct VlMutex {
  pthread_mutex_t m;
  int inited;
};
AuxStatus vl_mutex_init(VlMutex *m, int recursive) {
  if (!m) return AUX_EINVAL;
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(
      &a, recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
  int rc = pthread_mutex_init(&m->m, &a);
  pthread_mutexattr_destroy(&a);
  if (rc != 0) return AUX_EIO;
  m->inited = 1;
  return AUX_OK;
}
void vl_mutex_destroy(VlMutex *m) {
  if (m && m->inited) {
    pthread_mutex_destroy(&m->m);
    m->inited = 0;
  }
}
AuxStatus vl_mutex_lock(VlMutex *m) {
  if (!m || !m->inited) return AUX_EINVAL;
  return pthread_mutex_lock(&m->m) == 0 ? AUX_OK : AUX_EIO;
}
AuxStatus vl_mutex_trylock(VlMutex *m) {
  if (!m || !m->inited) return AUX_EINVAL;
  int rc = pthread_mutex_trylock(&m->m);
  return rc == 0 ? AUX_OK : (rc == EBUSY ? AUX_BUSY : AUX_EIO);
}
AuxStatus vl_mutex_unlock(VlMutex *m) {
  if (!m || !m->inited) return AUX_EINVAL;
  return pthread_mutex_unlock(&m->m) == 0 ? AUX_OK : AUX_EIO;
}

// RWLock
struct VlRWLock {
  pthread_rwlock_t rw;
  int inited;
};
AuxStatus vl_rwlock_init(VlRWLock *rw) {
  if (!rw) return AUX_EINVAL;
  if (pthread_rwlock_init(&rw->rw, NULL) != 0) return AUX_EIO;
  rw->inited = 1;
  return AUX_OK;
}
void vl_rwlock_destroy(VlRWLock *rw) {
  if (rw && rw->inited) {
    pthread_rwlock_destroy(&rw->rw);
    rw->inited = 0;
  }
}
AuxStatus vl_rwlock_rdlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  return pthread_rwlock_rdlock(&rw->rw) == 0 ? AUX_OK : AUX_EIO;
}
AuxStatus vl_rwlock_wrlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  return pthread_rwlock_wrlock(&rw->rw) == 0 ? AUX_OK : AUX_EIO;
}
AuxStatus vl_rwlock_tryrdlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  int rc = pthread_rwlock_tryrdlock(&rw->rw);
  return rc == 0 ? AUX_OK : (rc == EBUSY ? AUX_BUSY : AUX_EIO);
}
AuxStatus vl_rwlock_trywrlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  int rc = pthread_rwlock_trywrlock(&rw->rw);
  return rc == 0 ? AUX_OK : (rc == EBUSY ? AUX_BUSY : AUX_EIO);
}
AuxStatus vl_rwlock_unlock(VlRWLock *rw) {
  if (!rw || !rw->inited) return AUX_EINVAL;
  return pthread_rwlock_unlock(&rw->rw) == 0 ? AUX_OK : AUX_EIO;
}

// Cond
struct VlCond {
  pthread_cond_t c;
  int inited;
};
AuxStatus vl_cond_init(VlCond *c) {
  if (!c) return AUX_EINVAL;
  if (pthread_cond_init(&c->c, NULL) != 0) return AUX_EIO;
  c->inited = 1;
  return AUX_OK;
}
void vl_cond_destroy(VlCond *c) {
  if (c && c->inited) {
    pthread_cond_destroy(&c->c);
    c->inited = 0;
  }
}
AuxStatus vl_cond_wait(VlCond *c, VlMutex *m) {
  if (!c || !m || !c->inited || !m->inited) return AUX_EINVAL;
  int rc = pthread_cond_wait(&c->c, &m->m);
  return rc == 0 ? AUX_OK : AUX_EIO;
}
AuxStatus vl_cond_timedwait_ms(VlCond *c, VlMutex *m, uint64_t ms) {
  if (!c || !m || !c->inited || !m->inited) return AUX_EINVAL;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t nsec = (uint64_t)ts.tv_nsec + (ms % 1000ULL) * 1000000ULL;
  ts.tv_sec += (time_t)(ms / 1000ULL) + (time_t)(nsec / 1000000000ULL);
  ts.tv_nsec = (long)(nsec % 1000000000ULL);
  int rc = pthread_cond_timedwait(&c->c, &m->m, &ts);
  if (rc == 0) return AUX_OK;
  return rc == ETIMEDOUT ? AUX_ETIMEDOUT : AUX_EIO;
}
AuxStatus vl_cond_signal(VlCond *c) {
  if (!c || !c->inited) return AUX_EINVAL;
  return pthread_cond_signal(&c->c) == 0 ? AUX_OK : AUX_EIO;
}
AuxStatus vl_cond_broadcast(VlCond *c) {
  if (!c || !c->inited) return AUX_EINVAL;
  return pthread_cond_broadcast(&c->c) == 0 ? AUX_OK : AUX_EIO;
}

// Sem
#if defined(VL_HAVE_POSIX_SEM)
struct VlSem {
  sem_t s;
  int inited;
};
AuxStatus vl_sem_init(VlSem *s, unsigned initial) {
  if (!s) return AUX_EINVAL;
  if (sem_init(&s->s, 0, (unsigned)initial) != 0) return AUX_EIO;
  s->inited = 1;
  return AUX_OK;
}
void vl_sem_destroy(VlSem *s) {
  if (s && s->inited) {
    sem_destroy(&s->s);
    s->inited = 0;
  }
}
AuxStatus vl_sem_post(VlSem *s) {
  if (!s || !s->inited) return AUX_EINVAL;
  return sem_post(&s->s) == 0 ? AUX_OK : AUX_EIO;
}
AuxStatus vl_sem_wait(VlSem *s) {
  if (!s || !s->inited) return AUX_EINVAL;
  int rc;
  do {
    rc = sem_wait(&s->s);
  } while (rc != 0 && errno == EINTR);
  return rc == 0 ? AUX_OK : AUX_EIO;
}
AuxStatus vl_sem_trywait(VlSem *s) {
  if (!s || !s->inited) return AUX_EINVAL;
  return sem_trywait(&s->s) == 0 ? AUX_OK
                                 : (errno == EAGAIN ? AUX_BUSY : AUX_EIO);
}
AuxStatus vl_sem_timedwait_ms(VlSem *s, uint64_t ms) {
  if (!s || !s->inited) return AUX_EINVAL;
#if defined(__APPLE__)
  // macOS n'a pas sem_timedwait pour les semaphores anonymes -> fallback
  uint64_t deadline = aux_now_millis() + ms;
  while (aux_now_millis() < deadline) {
    if (vl_sem_trywait(s) == AUX_OK) return AUX_OK;
    vl_thread_sleep_ms(1);
  }
  return AUX_ETIMEDOUT;
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t nsec = (uint64_t)ts.tv_nsec + (ms % 1000ULL) * 1000000ULL;
  ts.tv_sec += (time_t)(ms / 1000ULL) + (time_t)(nsec / 1000000000ULL);
  ts.tv_nsec = (long)(nsec % 1000000000ULL);
  int rc = sem_timedwait(&s->s, &ts);
  if (rc == 0) return AUX_OK;
  return errno == ETIMEDOUT ? AUX_ETIMEDOUT : AUX_EIO;
#endif
}
#else
// Fallback semaphore (mutex+cond)
struct VlSem {
  VlMutex m;
  VlCond c;
  unsigned count;
  int inited;
};
AuxStatus vl_sem_init(VlSem *s, unsigned initial) {
  if (!s) return AUX_EINVAL;
  AuxStatus a = vl_mutex_init(&s->m, 0);
  if (a != AUX_OK) return a;
  a = vl_cond_init(&s->c);
  if (a != AUX_OK) {
    vl_mutex_destroy(&s->m);
    return a;
  }
  s->count = initial;
  s->inited = 1;
  return AUX_OK;
}
void vl_sem_destroy(VlSem *s) {
  if (s && s->inited) {
    vl_cond_destroy(&s->c);
    vl_mutex_destroy(&s->m);
    s->inited = 0;
  }
}
AuxStatus vl_sem_post(VlSem *s) {
  if (!s || !s->inited) return AUX_EINVAL;
  vl_mutex_lock(&s->m);
  s->count++;
  vl_cond_signal(&s->c);
  vl_mutex_unlock(&s->m);
  return AUX_OK;
}
AuxStatus vl_sem_wait(VlSem *s) {
  if (!s || !s->inited) return AUX_EINVAL;
  vl_mutex_lock(&s->m);
  while (s->count == 0) {
    AuxStatus r = vl_cond_wait(&s->c, &s->m);
    if (r != AUX_OK) {
      vl_mutex_unlock(&s->m);
      return r;
    }
  }
  s->count--;
  vl_mutex_unlock(&s->m);
  return AUX_OK;
}
AuxStatus vl_sem_trywait(VlSem *s) {
  if (!s || !s->inited) return AUX_EINVAL;
  AuxStatus r = vl_mutex_lock(&s->m);
  if (r != AUX_OK) return r;
  if (s->count == 0) {
    vl_mutex_unlock(&s->m);
    return AUX_BUSY;
  }
  s->count--;
  vl_mutex_unlock(&s->m);
  return AUX_OK;
}
AuxStatus vl_sem_timedwait_ms(VlSem *s, uint64_t ms) {
  if (!s || !s->inited) return AUX_EINVAL;
  uint64_t deadline = aux_now_millis() + ms;
  AuxStatus r = vl_mutex_lock(&s->m);
  if (r != AUX_OK) return r;
  while (s->count == 0) {
    uint64_t now = aux_now_millis();
    if (now >= deadline) {
      vl_mutex_unlock(&s->m);
      return AUX_ETIMEDOUT;
    }
    uint64_t left = deadline - now;
    // micro-wait: cond wait avec timeout ≈1..left ms
    uint64_t slice = left > 10 ? 10 : left;
    vl_mutex_unlock(&s->m);
    vl_thread_sleep_ms(slice);
    vl_mutex_lock(&s->m);
  }
  s->count--;
  vl_mutex_unlock(&s->m);
  return AUX_OK;
}
#endif

// TLS
struct VlTlsKey {
  pthread_key_t k;
  int inited;
};
AuxStatus vl_tlskey_create(VlTlsKey *k, void (*dtor)(void *)) {
  if (!k) return AUX_EINVAL;
  if (pthread_key_create(&k->k, dtor) != 0) return AUX_EIO;
  k->inited = 1;
  return AUX_OK;
}
void vl_tlskey_delete(VlTlsKey *k) {
  if (k && k->inited) {
    pthread_key_delete(k->k);
    k->inited = 0;
  }
}
AuxStatus vl_tls_set(VlTlsKey *k, void *ptr) {
  if (!k || !k->inited) return AUX_EINVAL;
  return pthread_setspecific(k->k, ptr) == 0 ? AUX_OK : AUX_EIO;
}
void *vl_tls_get(VlTlsKey *k) {
  if (!k || !k->inited) return NULL;
  return pthread_getspecific(k->k);
}

// Once
struct VlOnce {
  pthread_once_t o;
  int initd;
};
void vl_once_init(VlOnce *o) {
  if (o) {
    o->o = PTHREAD_ONCE_INIT;
    o->initd = 1;
  }
}
static void (*vl__once_fn)(void);
static void vl__once_tramp(void) {
  if (vl__once_fn) vl__once_fn();
}
int vl_once_run(VlOnce *o, void (*initfn)(void)) {
  if (!o || !o->initd || !initfn) return 0;
  vl__once_fn = initfn;
  pthread_once(&o->o, vl__once_tramp);
  return 1;
}

// Fences
#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
void vl_fence_acquire(void) { atomic_thread_fence(memory_order_acquire); }
void vl_fence_release(void) { atomic_thread_fence(memory_order_release); }
void vl_fence_seq_cst(void) { atomic_thread_fence(memory_order_seq_cst); }
#else
void vl_fence_acquire(void) { __sync_synchronize(); }
void vl_fence_release(void) { __sync_synchronize(); }
void vl_fence_seq_cst(void) { __sync_synchronize(); }
#endif

#endif  // platform split

// ======================================================================
// VM binding minimal et sûr (pas de création de threads VM ici)
// ======================================================================

static int vlthr_sleep_ms(VL_State *S) {
  VL_Value *v = vl_get(S, 1);
  if (!v || !(vl_isint(S, 1) || vl_isfloat(S, 1))) {
    vl_errorf(S, "ms:int expected");
    return vl_error(S);
  }
  uint64_t ms =
      (uint64_t)(vl_isint(S, 1) ? vl_toint(S, v) : (int64_t)vl_tonumber(S, v));
  vl_thread_sleep_ms(ms);
  vl_push_bool(S, 1);
  return 1;
}

static int vlthr_yield(VL_State *S) {
  (void)S;
  vl_thread_yield();
  vl_push_bool(S, 1);
  return 1;
}

static int vlthr_current_id(VL_State *S) {
  vl_push_int(S, (int64_t)vl_thread_current_id());
  return 1;
}

static int vlthr_cpu_count(VL_State *S) {
  int n = 1;
#if defined(_WIN32)
  DWORD cnt = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (cnt > 0) n = (int)cnt;
#else
#if defined(_SC_NPROCESSORS_ONLN)
  long v = sysconf(_SC_NPROCESSORS_ONLN);
  if (v > 0 && v < 1 << 20) n = (int)v;
#endif
#endif
  vl_push_int(S, (int64_t)n);
  return 1;
}

static const VL_Reg threadlib[] = {{"sleep_ms", vlthr_sleep_ms},
                                   {"yield", vlthr_yield},
                                   {"current_id", vlthr_current_id},
                                   {"cpu_count", vlthr_cpu_count},
                                   {NULL, NULL}};

void vl_open_threadlib(VL_State *S) { vl_register_lib(S, "thread", threadlib); }
