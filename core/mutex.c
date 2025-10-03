/* ============================================================================
 * mutex.c — Portable sync primitives (C11)
 *  Types: vl_mutex, vl_cond, vl_rwlock
 *  API  :
 *    int  vl_mutex_init(vl_mutex*, int recursive);
 *    void vl_mutex_destroy(vl_mutex*);
 *    int  vl_mutex_lock(vl_mutex*);
 *    int  vl_mutex_trylock(vl_mutex*);
 *    int  vl_mutex_unlock(vl_mutex*);
 *
 *    int  vl_cond_init(vl_cond*);
 *    void vl_cond_destroy(vl_cond*);
 *    int  vl_cond_wait(vl_cond*, vl_mutex*);
 *    int  vl_cond_timedwait_ms(vl_cond*, vl_mutex*, unsigned long ms); // 0 ok, ETIMEDOUT
 *    int  vl_cond_signal(vl_cond*);
 *    int  vl_cond_broadcast(vl_cond*);
 *
 *    int  vl_rwlock_init(vl_rwlock*);
 *    void vl_rwlock_destroy(vl_rwlock*);
 *    int  vl_rwlock_rdlock(vl_rwlock*);
 *    int  vl_rwlock_tryrdlock(vl_rwlock*);
 *    int  vl_rwlock_wrlock(vl_rwlock*);
 *    int  vl_rwlock_trywrlock(vl_rwlock*);
 *    int  vl_rwlock_unlock(vl_rwlock*);
 *
 *    void vl_sleep_ms(unsigned long ms);
 *    void vl_thread_yield(void);
 *
 *    const char* vl_sync_last_error(void); // message statique
 * ========================================================================== */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <synchapi.h>
#  include <processthreadsapi.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/time.h>
#endif

/* ===== Public-ish types =================================================== */

typedef struct {
#if defined(_WIN32)
    CRITICAL_SECTION cs;  /* réentrant par nature */
#else
    pthread_mutex_t  m;
#endif
    int recursive;
} vl_mutex;

typedef struct {
#if defined(_WIN32)
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t    cv;
#endif
} vl_cond;

typedef struct {
#if defined(_WIN32)
    SRWLOCK rw;
#else
    pthread_rwlock_t rw;
#endif
} vl_rwlock;

/* ===== Error buffer ======================================================= */

static char g_sync_err[256];

static void set_err(const char* where, const char* what) {
#if defined(_WIN32)
    DWORD code = GetLastError();
    char  sys[160] = {0};
    if (code) {
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, 0, sys, (DWORD)sizeof(sys), NULL);
        sys[sizeof(sys)-1] = 0;
    }
    _snprintf(g_sync_err, sizeof g_sync_err, "%s: %s%s%s",
              where ? where : "", what ? what : "",
              code ? " — " : "", code ? sys : "");
#else
    int e = errno;
    const char* es = e ? strerror(e) : "";
    snprintf(g_sync_err, sizeof g_sync_err, "%s: %s%s%s",
             where ? where : "", what ? what : "",
             e ? " — " : "", es);
#endif
    g_sync_err[sizeof g_sync_err - 1] = 0;
}
const char* vl_sync_last_error(void) { return g_sync_err[0] ? g_sync_err : ""; }

/* ===== Time helpers ======================================================= */

static void sleep_ms_portable(unsigned long ms) {
#if defined(_WIN32)
    Sleep(ms ? (DWORD)ms : 0);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000ul);
    ts.tv_nsec = (long)((ms % 1000ul) * 1000000ul);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#endif
}

void vl_sleep_ms(unsigned long ms) { sleep_ms_portable(ms); }

void vl_thread_yield(void) {
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}

/* ===== Mutex ============================================================== */

int vl_mutex_init(vl_mutex* m, int recursive) {
    if (!m) { set_err("vl_mutex_init", "bad args"); return -1; }
    m->recursive = recursive ? 1 : 0;
#if defined(_WIN32)
    /* CRITICAL_SECTION est réentrant; spin count raisonnable */
    if (!InitializeCriticalSectionAndSpinCount(&m->cs, 4000)) {
        set_err("InitializeCriticalSection", NULL);
        return -1;
    }
    g_sync_err[0] = 0;
    return 0;
#else
    pthread_mutexattr_t a;
    if (pthread_mutexattr_init(&a) != 0) { set_err("pthread_mutexattr_init", NULL); return -1; }
    if (recursive) {
        if (pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) != 0) {
            set_err("pthread_mutexattr_settype", "RECURSIVE");
            pthread_mutexattr_destroy(&a);
            return -1;
        }
    } else {
        (void)0; /* default normal */
    }
    int rc = pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    if (rc != 0) { errno = rc; set_err("pthread_mutex_init", NULL); return -1; }
    g_sync_err[0] = 0;
    return 0;
#endif
}

void vl_mutex_destroy(vl_mutex* m) {
    if (!m) return;
#if defined(_WIN32)
    DeleteCriticalSection(&m->cs);
#else
    (void)pthread_mutex_destroy(&m->m);
#endif
}

int vl_mutex_lock(vl_mutex* m) {
    if (!m) { set_err("vl_mutex_lock", "bad args"); return -1; }
#if defined(_WIN32)
    EnterCriticalSection(&m->cs);
    return 0;
#else
    int rc = pthread_mutex_lock(&m->m);
    if (rc) { errno = rc; set_err("pthread_mutex_lock", NULL); return -1; }
    return 0;
#endif
}

int vl_mutex_trylock(vl_mutex* m) {
    if (!m) { set_err("vl_mutex_trylock", "bad args"); return -1; }
#if defined(_WIN32)
    return TryEnterCriticalSection(&m->cs) ? 0 : EBUSY;
#else
    int rc = pthread_mutex_trylock(&m->m);
    if (rc && rc != EBUSY) { errno = rc; set_err("pthread_mutex_trylock", NULL); return -1; }
    return rc; /* 0 or EBUSY */
#endif
}

int vl_mutex_unlock(vl_mutex* m) {
    if (!m) { set_err("vl_mutex_unlock", "bad args"); return -1; }
#if defined(_WIN32)
    LeaveCriticalSection(&m->cs);
    return 0;
#else
    int rc = pthread_mutex_unlock(&m->m);
    if (rc) { errno = rc; set_err("pthread_mutex_unlock", NULL); return -1; }
    return 0;
#endif
}

/* ===== Condition variable ================================================ */

int vl_cond_init(vl_cond* c) {
    if (!c) { set_err("vl_cond_init", "bad args"); return -1; }
#if defined(_WIN32)
    InitializeConditionVariable(&c->cv);
    g_sync_err[0] = 0;
    return 0;
#else
    int rc = pthread_cond_init(&c->cv, NULL);
    if (rc) { errno = rc; set_err("pthread_cond_init", NULL); return -1; }
    g_sync_err[0] = 0;
    return 0;
#endif
}

void vl_cond_destroy(vl_cond* c) {
    if (!c) return;
#if defined(_WIN32)
    (void)c; /* rien à faire */
#else
    (void)pthread_cond_destroy(&c->cv);
#endif
}

int vl_cond_wait(vl_cond* c, vl_mutex* m) {
    if (!c || !m) { set_err("vl_cond_wait", "bad args"); return -1; }
#if defined(_WIN32)
    /* Timeout infini */
    if (!SleepConditionVariableCS(&c->cv, &m->cs, INFINITE)) {
        set_err("SleepConditionVariableCS", NULL); return -1;
    }
    return 0;
#else
    int rc = pthread_cond_wait(&c->cv, &m->m);
    if (rc) { errno = rc; set_err("pthread_cond_wait", NULL); return -1; }
    return 0;
#endif
}

int vl_cond_timedwait_ms(vl_cond* c, vl_mutex* m, unsigned long ms) {
    if (!c || !m) { set_err("vl_cond_timedwait_ms", "bad args"); return -1; }
#if defined(_WIN32)
    BOOL ok = SleepConditionVariableCS(&c->cv, &m->cs, (DWORD)ms);
    if (ok) return 0;
    DWORD e = GetLastError();
    if (e == ERROR_TIMEOUT) return ETIMEDOUT;
    set_err("SleepConditionVariableCS", "timed"); return -1;
#else
    /* pthread_cond_timedwait attend un temps absolu */
    struct timespec ts;
#  if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#  else
    struct timeval tv; gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec; ts.tv_nsec = tv.tv_usec * 1000L;
#  endif
    ts.tv_sec  += (time_t)(ms / 1000ul);
    long addns  = (long)((ms % 1000ul) * 1000000ul);
    ts.tv_nsec += addns;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

    int rc = pthread_cond_timedwait(&c->cv, &m->m, &ts);
    if (rc == ETIMEDOUT) return ETIMEDOUT;
    if (rc) { errno = rc; set_err("pthread_cond_timedwait", NULL); return -1; }
    return 0;
#endif
}

int vl_cond_signal(vl_cond* c) {
    if (!c) { set_err("vl_cond_signal", "bad args"); return -1; }
#if defined(_WIN32)
    WakeConditionVariable(&c->cv);
    return 0;
#else
    int rc = pthread_cond_signal(&c->cv);
    if (rc) { errno = rc; set_err("pthread_cond_signal", NULL); return -1; }
    return 0;
#endif
}

int vl_cond_broadcast(vl_cond* c) {
    if (!c) { set_err("vl_cond_broadcast", "bad args"); return -1; }
#if defined(_WIN32)
    WakeAllConditionVariable(&c->cv);
    return 0;
#else
    int rc = pthread_cond_broadcast(&c->cv);
    if (rc) { errno = rc; set_err("pthread_cond_broadcast", NULL); return -1; }
    return 0;
#endif
}

/* ===== RWLock ============================================================= */

int vl_rwlock_init(vl_rwlock* r) {
    if (!r) { set_err("vl_rwlock_init", "bad args"); return -1; }
#if defined(_WIN32)
    InitializeSRWLock(&r->rw);
    g_sync_err[0] = 0;
    return 0;
#else
    int rc = pthread_rwlock_init(&r->rw, NULL);
    if (rc) { errno = rc; set_err("pthread_rwlock_init", NULL); return -1; }
    g_sync_err[0] = 0;
    return 0;
#endif
}

void vl_rwlock_destroy(vl_rwlock* r) {
    if (!r) return;
#if defined(_WIN32)
    (void)r; /* rien à faire */
#else
    (void)pthread_rwlock_destroy(&r->rw);
#endif
}

int vl_rwlock_rdlock(vl_rwlock* r) {
    if (!r) { set_err("vl_rwlock_rdlock", "bad args"); return -1; }
#if defined(_WIN32)
    AcquireSRWLockShared(&r->rw);
    return 0;
#else
    int rc = pthread_rwlock_rdlock(&r->rw);
    if (rc) { errno = rc; set_err("pthread_rwlock_rdlock", NULL); return -1; }
    return 0;
#endif
}

int vl_rwlock_tryrdlock(vl_rwlock* r) {
    if (!r) { set_err("vl_rwlock_tryrdlock", "bad args"); return -1; }
#if defined(_WIN32)
    return TryAcquireSRWLockShared(&r->rw) ? 0 : EBUSY;
#else
    int rc = pthread_rwlock_tryrdlock(&r->rw);
    if (rc && rc != EBUSY) { errno = rc; set_err("pthread_rwlock_tryrdlock", NULL); return -1; }
    return rc;
#endif
}

int vl_rwlock_wrlock(vl_rwlock* r) {
    if (!r) { set_err("vl_rwlock_wrlock", "bad args"); return -1; }
#if defined(_WIN32)
    AcquireSRWLockExclusive(&r->rw);
    return 0;
#else
    int rc = pthread_rwlock_wrlock(&r->rw);
    if (rc) { errno = rc; set_err("pthread_rwlock_wrlock", NULL); return -1; }
    return 0;
#endif
}

int vl_rwlock_trywrlock(vl_rwlock* r) {
    if (!r) { set_err("vl_rwlock_trywrlock", "bad args"); return -1; }
#if defined(_WIN32)
    return TryAcquireSRWLockExclusive(&r->rw) ? 0 : EBUSY;
#else
    int rc = pthread_rwlock_trywrlock(&r->rw);
    if (rc && rc != EBUSY) { errno = rc; set_err("pthread_rwlock_trywrlock", NULL); return -1; }
    return rc;
#endif
}

int vl_rwlock_unlock(vl_rwlock* r) {
    if (!r) { set_err("vl_rwlock_unlock", "bad args"); return -1; }
#if defined(_WIN32)
    /* Heuristique: pas de moyen portable pour savoir si exclusif ou partagé.
       On essaie exclusive d'abord, si échec le comportement est indéfini.
       WinAPI requiert de connaître le mode. On exige que l'appelant sache. */
    /* Solution: ne rien faire ici. Fournir deux API explicites: */
    set_err("vl_rwlock_unlock", "use _unlock_rd or _unlock_wr on Windows");
    return -1;
#else
    int rc = pthread_rwlock_unlock(&r->rw);
    if (rc) { errno = rc; set_err("pthread_rwlock_unlock", NULL); return -1; }
    return 0;
#endif
}

/* Windows-only explicit unlocks to avoid mode ambiguity */
#if defined(_WIN32)
int vl_rwlock_unlock_rd(vl_rwlock* r) {
    if (!r) { set_err("vl_rwlock_unlock_rd", "bad args"); return -1; }
    ReleaseSRWLockShared(&r->rw); return 0;
}
int vl_rwlock_unlock_wr(vl_rwlock* r) {
    if (!r) { set_err("vl_rwlock_unlock_wr", "bad args"); return -1; }
    ReleaseSRWLockExclusive(&r->rw); return 0;
}
#endif

/* ===== Optional self-test ================================================ */
#ifdef MUTEX_MAIN
#include <stdio.h>
int main(void) {
    vl_mutex m; vl_cond c; vl_rwlock rw;
    vl_mutex_init(&m, 1);
    vl_cond_init(&c);
    vl_rwlock_init(&rw);

    vl_mutex_lock(&m);
    vl_cond_signal(&c);
    vl_cond_broadcast(&c);
    vl_mutex_unlock(&m);

    vl_rwlock_rdlock(&rw);
#if defined(_WIN32)
    vl_rwlock_unlock_rd(&rw);
#else
    vl_rwlock_unlock(&rw);
#endif

    vl_rwlock_wrlock(&rw);
#if defined(_WIN32)
    vl_rwlock_unlock_wr(&rw);
#else
    vl_rwlock_unlock(&rw);
#endif

    vl_cond_destroy(&c);
    vl_mutex_destroy(&m);
    vl_rwlock_destroy(&rw);
    puts("ok");
    return 0;
}
#endif
