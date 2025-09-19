// SPDX-License-Identifier: GPL-3.0-or-later
//
// pthread.c — Abstraction threads/mutex/cond portable (C17)
// Namespace: "pth"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c pthread.c
//
// Fournit:
//   - Threads : pth_thread, pth_thread_create, pth_thread_join, pth_yield, pth_sleep_ms
//   - Mutex   : pth_mutex,  pth_mutex_init/lock/trylock/unlock/destroy
//   - Cond    : pth_cond,   pth_cond_init/signal/broadcast/wait/timedwait_ms/destroy
//   - Once    : pth_once,   pth_once_init/pth_once_run
//   - TLS     : pth_tls_key, pth_tls_create/get/set/delete

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <process.h>
#else
  #include <pthread.h>
  #include <time.h>
  #include <sys/time.h>
  #include <unistd.h>
  #include <errno.h>     /* ETIMEDOUT */
  #include <sched.h>     /* sched_yield */
#endif

#ifndef PTH_API
#define PTH_API
#endif

typedef int (*pth_start_routine)(void*);

typedef struct {
#if defined(_WIN32)
    HANDLE h;
#else
    pthread_t th;
#endif
} pth_thread;

typedef struct {
#if defined(_WIN32)
    SRWLOCK l;
#else
    pthread_mutex_t m;
#endif
} pth_mutex;

typedef struct {
#if defined(_WIN32)
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t c;
#endif
} pth_cond;

typedef struct {
#if defined(_WIN32)
    INIT_ONCE once;
#else
    pthread_once_t once;
#endif
    void (*fn)(void);
} pth_once;

typedef struct {
#if defined(_WIN32)
    DWORD k;
#else
    pthread_key_t k;
#endif
} pth_tls_key;

/* ===== util ===== */

static void _pth_sleep_ms(unsigned ms){
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts; ts.tv_sec = ms/1000u; ts.tv_nsec = (long)(ms%1000u)*1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ===== threads ===== */

#if defined(_WIN32)
struct _pth_tramp { pth_start_routine fn; void* arg; };
static unsigned __stdcall _pth_tr(void* p){
    struct _pth_tramp t = *(struct _pth_tramp*)p;
    free(p);
    int rc = t.fn ? t.fn(t.arg) : 0;
    return (unsigned)rc;
}
#endif

PTH_API int pth_thread_create(pth_thread* t, pth_start_routine fn, void* arg, size_t stack_size){
    if (!t || !fn) return -1;
#if defined(_WIN32)
    struct _pth_tramp* box = (struct _pth_tramp*)malloc(sizeof *box);
    if (!box) return -1;
    box->fn = fn; box->arg = arg;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack_size, _pth_tr, box, 0, NULL);
    if (!h){ free(box); return -1; }
    t->h = (HANDLE)h;
    return 0;
#else
    pthread_attr_t a, *ap=NULL;
    if (stack_size){
        ap=&a; if (pthread_attr_init(&a)!=0) return -1;
        pthread_attr_setstacksize(&a, stack_size);
    }
    int rc = pthread_create(&t->th, ap, (void*(*)(void*))fn, arg);
    if (ap) pthread_attr_destroy(ap);
    return rc==0?0:-1;
#endif
}

PTH_API int pth_thread_join(pth_thread* t, int* exit_code){
    if (!t) return -1;
#if defined(_WIN32)
    DWORD rc = WaitForSingleObject(t->h, INFINITE);
    if (rc!=WAIT_OBJECT_0) return -1;
    DWORD ec=0; GetExitCodeThread(t->h,&ec);
    if (exit_code) *exit_code=(int)ec;
    CloseHandle(t->h); t->h=NULL;
    return 0;
#else
    void* ret=NULL; if (pthread_join(t->th,&ret)!=0) return -1;
    if (exit_code) *exit_code = (int)(intptr_t)ret;
    return 0;
#endif
}

PTH_API void pth_yield(void){
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}

PTH_API void pth_sleep_ms(unsigned ms){ _pth_sleep_ms(ms); }

/* ===== mutex ===== */

PTH_API int pth_mutex_init(pth_mutex* m){
    if (!m) return -1;
#if defined(_WIN32)
    InitializeSRWLock(&m->l);
    return 0;
#else
    return pthread_mutex_init(&m->m, NULL)==0?0:-1;
#endif
}
PTH_API int pth_mutex_destroy(pth_mutex* m){
    if (!m) return -1;
#if defined(_WIN32)
    (void)m; return 0;
#else
    return pthread_mutex_destroy(&m->m)==0?0:-1;
#endif
}
PTH_API int pth_mutex_lock(pth_mutex* m){
    if (!m) return -1;
#if defined(_WIN32)
    AcquireSRWLockExclusive(&m->l);
    return 0;
#else
    return pthread_mutex_lock(&m->m)==0?0:-1;
#endif
}
PTH_API int pth_mutex_trylock(pth_mutex* m){
    if (!m) return -1;
#if defined(_WIN32)
    return TryAcquireSRWLockExclusive(&m->l) ? 0 : -1;
#else
    int rc = pthread_mutex_trylock(&m->m);
    return rc==0?0:-1;
#endif
}
PTH_API int pth_mutex_unlock(pth_mutex* m){
    if (!m) return -1;
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&m->l);
    return 0;
#else
    return pthread_mutex_unlock(&m->m)==0?0:-1;
#endif
}

/* ===== condition ===== */

PTH_API int pth_cond_init(pth_cond* c){
    if (!c) return -1;
#if defined(_WIN32)
    InitializeConditionVariable(&c->cv);
    return 0;
#else
    return pthread_cond_init(&c->c,NULL)==0?0:-1;
#endif
}
PTH_API int pth_cond_destroy(pth_cond* c){
    if (!c) return -1;
#if defined(_WIN32)
    (void)c; return 0;
#else
    return pthread_cond_destroy(&c->c)==0?0:-1;
#endif
}
PTH_API int pth_cond_signal(pth_cond* c){
    if (!c) return -1;
#if defined(_WIN32)
    WakeConditionVariable(&c->cv);
    return 0;
#else
    return pthread_cond_signal(&c->c)==0?0:-1;
#endif
}
PTH_API int pth_cond_broadcast(pth_cond* c){
    if (!c) return -1;
#if defined(_WIN32)
    WakeAllConditionVariable(&c->cv);
    return 0;
#else
    return pthread_cond_broadcast(&c->c)==0?0:-1;
#endif
}

PTH_API int pth_cond_wait(pth_cond* c, pth_mutex* m){
    if (!c||!m) return -1;
#if defined(_WIN32)
    SleepConditionVariableSRW(&c->cv, &m->l, INFINITE, 0);
    return 0;
#else
    return pthread_cond_wait(&c->c, &m->m)==0?0:-1;
#endif
}

/* Retour: 0 signal, 1 timeout, -1 erreur. */
PTH_API int pth_cond_timedwait_ms(pth_cond* c, pth_mutex* m, unsigned ms){
    if (!c||!m) return -1;
#if defined(_WIN32)
    BOOL ok = SleepConditionVariableSRW(&c->cv, &m->l, (ms==0)?1:ms, 0);
    if (ok) return 0;
    return GetLastError()==ERROR_TIMEOUT ? 1 : -1;
#else
    if (ms==0){
        struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
        ts.tv_nsec += 1000000L; if (ts.tv_nsec>=1000000000L){ ts.tv_sec++; ts.tv_nsec-=1000000000L; }
        int rc = pthread_cond_timedwait(&c->c, &m->m, &ts);
        return rc==0?0: (rc==ETIMEDOUT?1:-1);
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);
    ts.tv_sec  += ms/1000u;
    ts.tv_nsec += (long)(ms%1000u)*1000000L;
    if (ts.tv_nsec>=1000000000L){ ts.tv_sec++; ts.tv_nsec-=1000000000L; }
    int rc = pthread_cond_timedwait(&c->c, &m->m, &ts);
    if (rc==0) return 0;
    return (rc==ETIMEDOUT)?1:-1;
#endif
}

/* ===== once ===== */

#if defined(_WIN32)
static BOOL CALLBACK _pth_once_win(PINIT_ONCE once, PVOID param, PVOID* ctx){
    (void)once; (void)ctx; void (*fn)(void)=(void(*)(void))param; if (fn) fn(); return TRUE;
}
#endif

PTH_API int pth_once_init(pth_once* o, void (*fn)(void)){
    if (!o||!fn) return -1;
    o->fn = fn;
#if defined(_WIN32)
    o->once = (INIT_ONCE)INIT_ONCE_STATIC_INIT;
#else
    /* PTHREAD_ONCE_INIT est un initialiseur. Utiliser un compound literal. */
    o->once = (pthread_once_t)PTHREAD_ONCE_INIT;
#endif
    return 0;
}
PTH_API int pth_once_run(pth_once* o){
    if (!o||!o->fn) return -1;
#if defined(_WIN32)
    return InitOnceExecuteOnce(&o->once, _pth_once_win, (PVOID)o->fn, NULL) ? 0 : -1;
#else
    return pthread_once(&o->once, o->fn)==0?0:-1;
#endif
}

/* ===== TLS ===== */

PTH_API int pth_tls_create(pth_tls_key* k, void (*destructor)(void*)){
    if (!k) return -1;
#if defined(_WIN32)
    (void)destructor;
    DWORD slot = TlsAlloc();
    if (slot==TLS_OUT_OF_INDEXES) return -1;
    k->k = slot; return 0;
#else
    return pthread_key_create(&k->k, destructor)==0?0:-1;
#endif
}
PTH_API int pth_tls_delete(pth_tls_key* k){
    if (!k) return -1;
#if defined(_WIN32)
    BOOL ok = TlsFree(k->k); return ok?0:-1;
#else
    return pthread_key_delete(k->k)==0?0:-1;
#endif
}
PTH_API void* pth_tls_get(pth_tls_key* k){
    if (!k) return NULL;
#if defined(_WIN32)
    return TlsGetValue(k->k);
#else
    return pthread_getspecific(k->k);
#endif
}
PTH_API int pth_tls_set(pth_tls_key* k, void* v){
    if (!k) return -1;
#if defined(_WIN32)
    return TlsSetValue(k->k, v)?0:-1;
#else
    return pthread_setspecific(k->k, v)==0?0:-1;
#endif
}

/* ===== Test ===== */
#ifdef PTH_TEST
#include <stdio.h>
typedef struct { int id; pth_mutex* mu; pth_cond* cv; int* shared; } ctx_t;

static int worker(void* arg){
    ctx_t* c=(ctx_t*)arg;
    for (int i=0;i<5;i++){
        pth_mutex_lock(c->mu);
        (*c->shared)++;
        pth_mutex_unlock(c->mu);
        pth_sleep_ms(20);
    }
    pth_mutex_lock(c->mu);
    pth_cond_signal(c->cv);
    pth_mutex_unlock(c->mu);
    return c->id;
}
static void once_fn(void){ puts("once"); }

int main(void){
    pth_mutex mu; pth_cond cv; pth_mutex_init(&mu); pth_cond_init(&cv);
    int shared=0;

    pth_once o; pth_once_init(&o, once_fn); pth_once_run(&o); pth_once_run(&o);

    pth_thread th1, th2;
    ctx_t a={1,&mu,&cv,&shared}, b={2,&mu,&cv,&shared};
    pth_thread_create(&th1, worker, &a, 0);
    pth_thread_create(&th2, worker, &b, 0);

    pth_mutex_lock(&mu);
    int waitrc = pth_cond_timedwait_ms(&cv,&mu,200);
    (void)waitrc;
    pth_mutex_unlock(&mu);

    int ec1,ec2; pth_thread_join(&th1,&ec1); pth_thread_join(&th2,&ec2);
    printf("ec=%d,%d shared=%d\n", ec1, ec2, shared);

    pth_cond_destroy(&cv); pth_mutex_destroy(&mu);
    return 0;
}
#endif