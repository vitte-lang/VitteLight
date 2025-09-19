// SPDX-License-Identifier: GPL-3.0-or-later
//
// corolib.c — Coroutines + tiny cooperative scheduler (C17, portable)
// Namespace: "coro"
//
// Features:
//   - Stackful coroutines: POSIX ucontext or Windows Fibers.
//   - Tiny round-robin scheduler with sleep (coro_sleep_ms).
//   - Safe yield/resume, current() accessor, status flags.
//   - No threads; cooperative only.
//
// Build:
//   POSIX:   cc -std=c17 -O2 -Wall -Wextra -pedantic -c corolib.c
//   Windows: cl /std:c17 /O2 /W4 /c corolib.c
//
// Public API:
//   typedef struct coro       coro_t;
//   typedef struct coro_sched coro_sched_t;
//
//   // Coroutines
//   coro_t*  coro_create(void (*fn)(void*), void* arg, size_t stack_size);
//   void     coro_resume(coro_t* c);                 // start or continue
//   void     coro_yield(void);                       // yield to caller/scheduler
//   void     coro_yield_to(coro_t* other);           // yield directly to other (same thread)
//   int      coro_is_done(const coro_t* c);          // 1 if finished
//   coro_t*  coro_current(void);                     // NULL if not inside a coro
//   void     coro_free(coro_t* c);                   // free resources (not while running)
//
//   // Sleep (cooperative): marks current coro as sleeping until deadline and yields.
//   void     coro_sleep_ms(int ms);
//
//   // Scheduler (optional convenience)
//   coro_sched_t* coro_sched_create(void);
//   void          coro_sched_add(coro_sched_t* s, coro_t* c);
//   void          coro_sched_run(coro_sched_t* s);   // runs until all added coroutines are done
//   void          coro_sched_destroy(coro_sched_t* s);
//
// Notes:
//   - One thread only. No preemption. Always call from the same thread you created things on.
//   - Yield inside the same thread only. Do not call from signal handlers.
//   - Sleep uses a millisecond monotonic clock where available; otherwise best effort.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#if defined(_WIN32)
// ================= Windows Fibers =================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct coro coro_t;
typedef struct coro_sched coro_sched_t;

struct coro {
    void (*fn)(void*);
    void* arg;
    void* fiber;          // this coroutine's fiber
    int   done;
    int   running;
    uint64_t wake_ms;     // scheduler wake deadline
    coro_sched_t* owner;  // scheduler owning this coro (for yield_to safety)
};

struct coro_sched {
    coro_t** v;
    size_t n, cap;
};

static __declspec(thread) coro_t* t_current = NULL;
static __declspec(thread) void*   t_main_fiber = NULL;

static uint64_t now_ms(void){
    LARGE_INTEGER f, c;
    if (QueryPerformanceFrequency(&f) && QueryPerformanceCounter(&c)) {
        return (uint64_t)((c.QuadPart * 1000ULL) / (uint64_t)f.QuadPart);
    }
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    return (uint64_t)(u.QuadPart / 10000ULL); // 100ns -> ms
}

static void __stdcall fiber_entry(void* arg){
    coro_t* c = (coro_t*)arg;
    t_current = c;
    c->running = 1;
    c->fn(c->arg);
    c->running = 0;
    c->done = 1;
    t_current = NULL;
    // back to main fiber
    SwitchToFiber(t_main_fiber);
}

coro_t* coro_create(void (*fn)(void*), void* arg, size_t stack_size){
    if (!fn) { errno = EINVAL; return NULL; }
    if (!t_main_fiber) t_main_fiber = ConvertThreadToFiber(NULL);
    coro_t* c = (coro_t*)calloc(1, sizeof(*c));
    if (!c) { errno = ENOMEM; return NULL; }
    c->fn = fn; c->arg = arg; c->wake_ms = 0;
    c->fiber = CreateFiber(stack_size? stack_size: 0, fiber_entry, c);
    if (!c->fiber) { free(c); errno = EIO; return NULL; }
    return c;
}

void coro_resume(coro_t* c){
    if (!c || !c->fiber || c->done) return;
    void* prev = t_main_fiber ? t_main_fiber : ConvertThreadToFiber(NULL);
    t_main_fiber = prev; // ensure set
    SwitchToFiber(c->fiber);
}

void coro_yield(void){
    if (!t_current) return;       // not inside a coro
    SwitchToFiber(t_main_fiber);
}

void coro_yield_to(coro_t* other){
    if (!t_current || !other || other->done) return;
    SwitchToFiber(other->fiber);
}

int  coro_is_done(const coro_t* c){ return c ? c->done : 1; }
coro_t* coro_current(void){ return t_current; }

void coro_free(coro_t* c){
    if (!c) return;
    if (c->running) { errno = EBUSY; return; }
    if (c->fiber) DeleteFiber(c->fiber);
    free(c);
}

void coro_sleep_ms(int ms){
    if (!t_current) return;
    uint64_t n = now_ms();
    t_current->wake_ms = n + (ms > 0 ? (uint64_t)ms : 0);
    coro_yield();
}

// ------------- scheduler -------------
static void sched_push(coro_sched_t* s, coro_t* c){
    if (s->n == s->cap){
        size_t nc = s->cap? s->cap*2 : 8;
        coro_t** nv = (coro_t**)realloc(s->v, nc * sizeof(*nv));
        if (!nv) { perror("oom"); abort(); }
        s->v = nv; s->cap = nc;
    }
    c->owner = s;
    s->v[s->n++] = c;
}

coro_sched_t* coro_sched_create(void){
    coro_sched_t* s = (coro_sched_t*)calloc(1,sizeof(*s));
    if (!s) { errno = ENOMEM; return NULL; }
    return s;
}

void coro_sched_add(coro_sched_t* s, coro_t* c){
    if (!s || !c) return;
    sched_push(s, c);
}

void coro_sched_run(coro_sched_t* s){
    if (!s) return;
    size_t alive;
    do {
        alive = 0;
        uint64_t n = now_ms();
        for (size_t i=0;i<s->n;i++){
            coro_t* c = s->v[i];
            if (!c || c->done) continue;
            alive = 1;
            if (c->wake_ms && n < c->wake_ms) continue; // still sleeping
            c->wake_ms = 0;
            coro_resume(c);
        }
        if (alive) {
            // prevent busy spin if everyone sleeps in future
            Sleep(1);
        }
    } while (alive);
}

void coro_sched_destroy(coro_sched_t* s){
    if (!s) return;
    free(s->v);
    free(s);
}

#elif defined(__unix__) || defined(__APPLE__)
// ================= POSIX ucontext =================
#include <ucontext.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct coro coro_t;
typedef struct coro_sched coro_sched_t;

struct coro {
    ucontext_t ctx;
    ucontext_t caller;     // where to return on yield
    void (*fn)(void*);
    void* arg;
    char* stack;
    size_t stack_size;
    int   done;
    int   running;
    uint64_t wake_ms;
    coro_sched_t* owner;
};

struct coro_sched {
    coro_t** v;
    size_t n, cap;
};

static __thread coro_t* t_current = NULL;

static uint64_t now_ms(void){
#if defined(CLOCK_MONOTONIC)
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000ull + (uint64_t)ts.tv_nsec/1000000ull;
#else
    struct timeval tv; gettimeofday(&tv,NULL);
    return (uint64_t)tv.tv_sec*1000ull + (uint64_t)tv.tv_usec/1000ull;
#endif
}

static void coro_trampoline(void){
    coro_t* c = t_current;
    if (!c) return;
    c->running = 1;
    c->fn(c->arg);
    c->running = 0;
    c->done = 1;
    setcontext(&c->caller); // never returns
}

coro_t* coro_create(void (*fn)(void*), void* arg, size_t stack_size){
    if (!fn) { errno = EINVAL; return NULL; }
    coro_t* c = (coro_t*)calloc(1,sizeof(*c));
    if (!c) { errno = ENOMEM; return NULL; }
    c->fn = fn; c->arg = arg;
    c->stack_size = stack_size ? stack_size : (64*1024);
    c->stack = (char*)malloc(c->stack_size);
    if (!c->stack) { free(c); errno = ENOMEM; return NULL; }
    if (getcontext(&c->ctx) != 0) { free(c->stack); free(c); return NULL; }
    c->ctx.uc_link = &c->caller;
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = c->stack_size;
    makecontext(&c->ctx, coro_trampoline, 0);
    return c;
}

void coro_resume(coro_t* c){
    if (!c || c->done) return;
    t_current = c;
    if (swapcontext(&c->caller, &c->ctx) != 0) { perror("swapcontext"); }
    t_current = NULL;
}

void coro_yield(void){
    if (!t_current) return;
    if (swapcontext(&t_current->ctx, &t_current->caller) != 0) { perror("swapcontext"); }
}

void coro_yield_to(coro_t* other){
    if (!t_current || !other || other->done) return;
    coro_t* prev = t_current;
    t_current = other;
    if (swapcontext(&prev->ctx, &other->ctx) != 0) { perror("swapcontext"); }
    t_current = prev; // restored on resume back
}

int  coro_is_done(const coro_t* c){ return c ? c->done : 1; }
coro_t* coro_current(void){ return t_current; }

void coro_free(coro_t* c){
    if (!c) return;
    if (c->running) { errno = EBUSY; return; }
    free(c->stack);
    free(c);
}

void coro_sleep_ms(int ms){
    if (!t_current) return;
    uint64_t n = now_ms();
    t_current->wake_ms = n + (ms > 0 ? (uint64_t)ms : 0);
    coro_yield();
}

// ------------- scheduler -------------
static void sched_push(coro_sched_t* s, coro_t* c){
    if (s->n == s->cap){
        size_t nc = s->cap? s->cap*2 : 8;
        coro_t** nv = (coro_t**)realloc(s->v, nc * sizeof(*nv));
        if (!nv) { perror("oom"); abort(); }
        s->v = nv; s->cap = nc;
    }
    c->owner = s;
    s->v[s->n++] = c;
}

coro_sched_t* coro_sched_create(void){
    coro_sched_t* s = (coro_sched_t*)calloc(1,sizeof(*s));
    if (!s) { errno = ENOMEM; return NULL; }
    return s;
}

void coro_sched_add(coro_sched_t* s, coro_t* c){
    if (!s || !c) return;
    sched_push(s, c);
}

void coro_sched_run(coro_sched_t* s){
    if (!s) return;
    size_t alive;
    do {
        alive = 0;
        uint64_t n = now_ms();
        int idled = 1;
        for (size_t i=0;i<s->n;i++){
            coro_t* c = s->v[i];
            if (!c || c->done) continue;
            alive = 1;
            if (c->wake_ms && n < c->wake_ms) continue;
            idled = 0;
            c->wake_ms = 0;
            coro_resume(c);
        }
        if (alive && idled) {
            // everybody sleeping; sleep a bit to avoid CPU spin
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1*1000*1000 }; // 1ms
            nanosleep(&ts, NULL);
        }
    } while (alive);
}

void coro_sched_destroy(coro_sched_t* s){
    if (!s) return;
    free(s->v);
    free(s);
}

#else
// ================= Stubs =================
typedef struct coro { int done; } coro_t;
typedef struct coro_sched { int _; } coro_sched_t;

coro_t*  coro_create(void (*fn)(void*), void* arg, size_t stack_size){ (void)fn;(void)arg;(void)stack_size; errno=ENOSYS; return NULL; }
void     coro_resume(coro_t* c){ (void)c; errno=ENOSYS; }
void     coro_yield(void){ errno=ENOSYS; }
void     coro_yield_to(coro_t* other){ (void)other; errno=ENOSYS; }
int      coro_is_done(const coro_t* c){ (void)c; return 1; }
coro_t*  coro_current(void){ return NULL; }
void     coro_free(coro_t* c){ (void)c; }
void     coro_sleep_ms(int ms){ (void)ms; errno=ENOSYS; }

coro_sched_t* coro_sched_create(void){ errno=ENOSYS; return NULL; }
void          coro_sched_add(coro_sched_t* s, coro_t* c){ (void)s;(void)c; errno=ENOSYS; }
void          coro_sched_run(coro_sched_t* s){ (void)s; errno=ENOSYS; }
void          coro_sched_destroy(coro_sched_t* s){ (void)s; }
#endif

// ================= Optional demo =================
#ifdef CORO_DEMO
static void worker(void* arg){
    const char* name = (const char*)arg;
    for (int i=0;i<5;i++){
        printf("[%s] i=%d\n", name, i);
        coro_sleep_ms(50);
    }
}

int main(void){
    coro_sched_t* S = coro_sched_create();
    coro_t* a = coro_create(worker, "A", 0);
    coro_t* b = coro_create(worker, "B", 0);
    coro_sched_add(S, a);
    coro_sched_add(S, b);
    coro_sched_run(S);
    coro_free(a); coro_free(b); coro_sched_destroy(S);
    return 0;
}
#endif