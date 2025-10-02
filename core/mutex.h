/* ============================================================================
 * mutex.h — Portable sync primitives (C11)
 * VitteLight / Vitl runtime
 * Types: vl_mutex, vl_cond, vl_rwlock
 * ========================================================================== */
#ifndef VITTELIGHT_MUTEX_H
#define VITTELIGHT_MUTEX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Visibility */
#ifndef VL_SYNC_API
#  if defined(_WIN32) && defined(VL_SYNC_DLL)
#    ifdef VL_SYNC_BUILD
#      define VL_SYNC_API __declspec(dllexport)
#    else
#      define VL_SYNC_API __declspec(dllimport)
#    endif
#  else
#    define VL_SYNC_API
#  endif
#endif

#include <stddef.h>

/* ===== Opaque types (layout matches mutex.c) ============================== */

typedef struct {
#if defined(_WIN32)
    void* cs;          /* CRITICAL_SECTION */
#else
    char  _storage[sizeof(void*) * 5]; /* fits pthread_mutex_t */
#endif
    int   recursive;
} vl_mutex;

typedef struct {
#if defined(_WIN32)
    void* cv;          /* CONDITION_VARIABLE */
#else
    char  _storage[sizeof(void*) * 6]; /* fits pthread_cond_t */
#endif
} vl_cond;

typedef struct {
#if defined(_WIN32)
    void* rw;          /* SRWLOCK */
#else
    char  _storage[sizeof(void*) * 6]; /* fits pthread_rwlock_t */
#endif
} vl_rwlock;

/* ===== Errors ============================================================= */

VL_SYNC_API const char* vl_sync_last_error(void); /* static buffer, empty if none */

/* ===== Sleep / yield ====================================================== */

VL_SYNC_API void vl_sleep_ms(unsigned long ms);
VL_SYNC_API void vl_thread_yield(void);

/* ===== Mutex ============================================================== */

VL_SYNC_API int  vl_mutex_init(vl_mutex* m, int recursive); /* 0/-1 */
VL_SYNC_API void vl_mutex_destroy(vl_mutex* m);
VL_SYNC_API int  vl_mutex_lock(vl_mutex* m);     /* 0/-1 */
VL_SYNC_API int  vl_mutex_trylock(vl_mutex* m);  /* 0 or EBUSY or -1 */
VL_SYNC_API int  vl_mutex_unlock(vl_mutex* m);   /* 0/-1 */

/* ===== Condition variable ================================================= */

VL_SYNC_API int  vl_cond_init(vl_cond* c);                      /* 0/-1 */
VL_SYNC_API void vl_cond_destroy(vl_cond* c);
VL_SYNC_API int  vl_cond_wait(vl_cond* c, vl_mutex* m);         /* 0/-1 */
VL_SYNC_API int  vl_cond_timedwait_ms(vl_cond* c, vl_mutex* m,
                                      unsigned long ms);        /* 0, ETIMEDOUT, or -1 */
VL_SYNC_API int  vl_cond_signal(vl_cond* c);                    /* 0/-1 */
VL_SYNC_API int  vl_cond_broadcast(vl_cond* c);                 /* 0/-1 */

/* ===== RWLock ============================================================= */

VL_SYNC_API int  vl_rwlock_init(vl_rwlock* r);                  /* 0/-1 */
VL_SYNC_API void vl_rwlock_destroy(vl_rwlock* r);
VL_SYNC_API int  vl_rwlock_rdlock(vl_rwlock* r);                /* 0/-1 */
VL_SYNC_API int  vl_rwlock_tryrdlock(vl_rwlock* r);             /* 0, EBUSY, or -1 */
VL_SYNC_API int  vl_rwlock_wrlock(vl_rwlock* r);                /* 0/-1 */
VL_SYNC_API int  vl_rwlock_trywrlock(vl_rwlock* r);             /* 0, EBUSY, or -1 */

/* On POSIX:
 *   vl_rwlock_unlock(r) releases either reader or writer lock.
 * On Windows:
 *   Call the explicit variant matching the lock you hold.
 */
#if defined(_WIN32)
VL_SYNC_API int  vl_rwlock_unlock_rd(vl_rwlock* r);             /* 0/-1 */
VL_SYNC_API int  vl_rwlock_unlock_wr(vl_rwlock* r);             /* 0/-1 */
#else
VL_SYNC_API int  vl_rwlock_unlock(vl_rwlock* r);                /* 0/-1 */
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITTELIGHT_MUTEX_H */
