// SPDX-License-Identifier: GPL-3.0-or-later
//
// threadpool.c — Pool de threads portable basé sur pth (C17, plus complet)
// Namespace: "tp"
//
// Dépend de pthread.c (namespace pth).
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c pthread.c threadpool.c
//
// API:
//   Types:
//     typedef void (*tp_task_fn)(void*);
//     typedef struct tp_pool tp_pool;
//   Création / arrêt:
//     int  tp_init(tp_pool* p, size_t nthreads, size_t queue_cap);     // cap>=1
//     void tp_shutdown(tp_pool* p, int drain);                          // drain=1 vide avant arrêt, 0 abort
//   Soumission:
//     int  tp_submit(tp_pool* p, tp_task_fn fn, void* arg);             // bloque si file pleine
//     int  tp_try_submit(tp_pool* p, tp_task_fn fn, void* arg);         // non bloquant, -1 si pleine
//     int  tp_timed_submit(tp_pool* p, tp_task_fn fn, void* arg,
//                          unsigned timeout_ms);                        // 0 ok, -2 timeout
//     int  tp_submit_batch(tp_pool* p, const tp_task_fn* fns,
//                          void* const* args, size_t n);                // bloque par fragments
//   Attentes:
//     void tp_wait_idle(tp_pool* p);                                    // file vide + aucun travail actif
//   Parallélisme de plage:
//     int  tp_parallel_for(tp_pool* p, size_t begin, size_t end,
//                          size_t chunk,
//                          void (*cb)(size_t i0,size_t i1,void* u), void* u);
//   Infos / stats (instantanées):
//     size_t tp_queue_len(tp_pool* p);
//     size_t tp_queue_cap(tp_pool* p);
//     size_t tp_threads(tp_pool* p);
//     size_t tp_active(tp_pool* p);      // tâches en cours hors file
//     size_t tp_completed(tp_pool* p);   // compteur cumulé (approx, non atomique inter-plateforme)
//
// Remarques:
//   - File bornée MPMC bloquante.
//   - Pas d’allocation dans le chemin worker sauf pour le ctx de lancement.
//   - Pas de noms/règles temps-réel pour rester portable.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifndef TP_API
#define TP_API
#endif

/* ===== pth (abstraction OS) ===== */
#ifndef PTH_API
#define PTH_API
#endif

typedef int (*pth_start_routine)(void*);

typedef struct {
#if defined(_WIN32)
    void* h;
#else
    unsigned long th; /* opaque */
#endif
} pth_thread;

typedef struct {
#if defined(_WIN32)
    void* l;
#else
    char m_[40];
#endif
} pth_mutex;

typedef struct {
#if defined(_WIN32)
    void* cv;
#else
    char c_[48];
#endif
} pth_cond;

PTH_API int  pth_thread_create(pth_thread*, pth_start_routine, void*, size_t);
PTH_API int  pth_thread_join(pth_thread*, int*);
PTH_API void pth_yield(void);
PTH_API void pth_sleep_ms(unsigned);
PTH_API int  pth_mutex_init(pth_mutex*);
PTH_API int  pth_mutex_destroy(pth_mutex*);
PTH_API int  pth_mutex_lock(pth_mutex*);
PTH_API int  pth_mutex_trylock(pth_mutex*);
PTH_API int  pth_mutex_unlock(pth_mutex*);
PTH_API int  pth_cond_init(pth_cond*);
PTH_API int  pth_cond_destroy(pth_cond*);
PTH_API int  pth_cond_signal(pth_cond*);
PTH_API int  pth_cond_broadcast(pth_cond*);
PTH_API int  pth_cond_wait(pth_cond*, pth_mutex*);
PTH_API int  pth_cond_timedwait_ms(pth_cond*, pth_mutex*, unsigned);

/* ===== thread pool ===== */

typedef void (*tp_task_fn)(void*);

typedef struct { tp_task_fn fn; void* arg; } tp_task;

typedef struct {
    tp_task* q; size_t cap, head, tail, len;
} tp_queue;

typedef struct tp_pool {
    pth_thread* th; size_t nth;

    pth_mutex   mu;
    pth_cond    cv_not_empty;
    pth_cond    cv_not_full;
    pth_cond    cv_idle;

    tp_queue    que;

    int         stop;        /* 0=run, 1=drain then stop, 2=abort now */
    size_t      active;      /* tâches en cours */
    size_t      completed;   /* cumul best-effort */
} tp_pool;

/* ===== queue helpers ===== */
static int  q_init(tp_queue* q, size_t cap){
    if (!q || cap==0) return -1;
    q->q = (tp_task*)calloc(cap, sizeof *q->q);
    if (!q->q) return -1;
    q->cap=cap; q->head=q->tail=q->len=0;
    return 0;
}
static void q_free(tp_queue* q){ if (!q) return; free(q->q); memset(q,0,sizeof *q); }
static int  q_push(tp_queue* q, tp_task t){
    if (q->len==q->cap) return -1;
    q->q[q->tail]=t;
    q->tail = (q->tail+1u) % q->cap;
    q->len++;
    return 0;
}
static int  q_pop(tp_queue* q, tp_task* out){
    if (q->len==0) return -1;
    *out = q->q[q->head];
    q->head = (q->head+1u) % q->cap;
    q->len--;
    return 0;
}

/* ===== worker ===== */
typedef struct { tp_pool* p; } tp_ctx;

static int tp_worker(void* arg){
    tp_ctx* c = (tp_ctx*)arg;
    tp_pool* p = c->p;
    free(c);

    for(;;){
        if (pth_mutex_lock(&p->mu)!=0) return 0;
        while (p->que.len==0 && p->stop==0){
            pth_cond_wait(&p->cv_not_empty, &p->mu);
        }
        if (p->stop){
            int quit = (p->stop==2) || (p->stop==1 && p->que.len==0);
            if (quit){ pth_mutex_unlock(&p->mu); return 0; }
        }
        tp_task t={0};
        (void)q_pop(&p->que,&t);
        p->active++;
        pth_cond_signal(&p->cv_not_full);
        pth_mutex_unlock(&p->mu);

        if (t.fn) t.fn(t.arg);

        if (pth_mutex_lock(&p->mu)!=0) return 0;
        p->active--;
        p->completed++;
        if (p->active==0 && p->que.len==0) pth_cond_broadcast(&p->cv_idle);
        pth_mutex_unlock(&p->mu);
    }
}

/* ===== API ===== */

TP_API int tp_init(tp_pool* p, size_t nthreads, size_t queue_cap){
    if (!p || nthreads==0 || queue_cap==0) return -1;
    memset(p,0,sizeof *p);
    if (pth_mutex_init(&p->mu)!=0) return -1;
    if (pth_cond_init(&p->cv_not_empty)!=0) return -1;
    if (pth_cond_init(&p->cv_not_full)!=0) return -1;
    if (pth_cond_init(&p->cv_idle)!=0) return -1;
    if (q_init(&p->que, queue_cap)!=0) return -1;
    p->th = (pth_thread*)calloc(nthreads, sizeof *p->th); if(!p->th) return -1;
    p->nth = nthreads;

    for (size_t i=0;i<nthreads;i++){
        tp_ctx* c = (tp_ctx*)malloc(sizeof *c); if(!c) return -1;
        c->p = p;
        if (pth_thread_create(&p->th[i], tp_worker, c, 0)!=0) return -1;
    }
    return 0;
}

TP_API void tp_shutdown(tp_pool* p, int drain){
    if (!p) return;
    if (pth_mutex_lock(&p->mu)!=0) return;
    p->stop = drain?1:2;
    pth_cond_broadcast(&p->cv_not_empty);
    pth_cond_broadcast(&p->cv_not_full);
    pth_mutex_unlock(&p->mu);

    for (size_t i=0;i<p->nth;i++) (void)pth_thread_join(&p->th[i], NULL);
    free(p->th); p->th=NULL; p->nth=0;

    pth_mutex_destroy(&p->mu);
    pth_cond_destroy(&p->cv_not_empty);
    pth_cond_destroy(&p->cv_not_full);
    pth_cond_destroy(&p->cv_idle);
    q_free(&p->que);
    memset(p,0,sizeof *p);
}

TP_API int tp_submit(tp_pool* p, tp_task_fn fn, void* arg){
    if (!p || !fn) return -1;
    if (pth_mutex_lock(&p->mu)!=0) return -1;
    while (p->que.len==p->que.cap && p->stop==0){
        pth_cond_wait(&p->cv_not_full, &p->mu);
    }
    if (p->stop){ pth_mutex_unlock(&p->mu); return -1; }
    (void)q_push(&p->que, (tp_task){fn,arg});
    pth_cond_signal(&p->cv_not_empty);
    pth_mutex_unlock(&p->mu);
    return 0;
}

TP_API int tp_try_submit(tp_pool* p, tp_task_fn fn, void* arg){
    if (!p || !fn) return -1;
    if (pth_mutex_lock(&p->mu)!=0) return -1;
    int rc = 0;
    if (p->stop || p->que.len==p->que.cap) rc=-1;
    else {
        (void)q_push(&p->que, (tp_task){fn,arg});
        pth_cond_signal(&p->cv_not_empty);
    }
    pth_mutex_unlock(&p->mu);
    return rc;
}

TP_API int tp_timed_submit(tp_pool* p, tp_task_fn fn, void* arg, unsigned timeout_ms){
    if (!p || !fn) return -1;
    if (pth_mutex_lock(&p->mu)!=0) return -1;
    int rc=0;
    while (p->que.len==p->que.cap && p->stop==0){
        int w = pth_cond_timedwait_ms(&p->cv_not_full, &p->mu, timeout_ms?timeout_ms:1);
        if (w==1){ rc=-2; break; }            /* timeout */
        if (w<0){ rc=-1; break; }             /* erreur */
    }
    if (rc==0){
        if (p->stop) rc=-1;
        else {
            (void)q_push(&p->que,(tp_task){fn,arg});
            pth_cond_signal(&p->cv_not_empty);
        }
    }
    pth_mutex_unlock(&p->mu);
    return rc;
}

TP_API int tp_submit_batch(tp_pool* p, const tp_task_fn* fns, void* const* args, size_t n){
    if (!p || !fns || n==0) return -1;
    for (size_t i=0;i<n;i++){
        void* a = args? args[i] : NULL;
        if (tp_submit(p, fns[i], a)!=0) return -1;
    }
    return 0;
}

TP_API void tp_wait_idle(tp_pool* p){
    if (!p) return;
    if (pth_mutex_lock(&p->mu)!=0) return;
    while (p->active>0 || p->que.len>0){
        pth_cond_wait(&p->cv_idle, &p->mu);
    }
    pth_mutex_unlock(&p->mu);
}

/* ===== Parallel for ===== */
typedef struct {
    size_t next, end, chunk;
    void (*cb)(size_t,size_t,void*);
    void* u;
    pth_mutex mu;
} tp_parfor_state;

static void tp_parfor_worker(void* arg){
    tp_parfor_state* S=(tp_parfor_state*)arg;
    for(;;){
        if (pth_mutex_lock(&S->mu)!=0) return;
        size_t i0=S->next;
        if (i0>=S->end){ pth_mutex_unlock(&S->mu); return; }
        size_t i1 = i0 + S->chunk; if (i1>S->end) i1=S->end;
        S->next = i1;
        pth_mutex_unlock(&S->mu);
        S->cb(i0,i1,S->u);
    }
}

TP_API int tp_parallel_for(tp_pool* p, size_t begin, size_t end, size_t chunk,
                           void (*cb)(size_t,size_t,void*), void* u){
    if (!p || !cb || end<begin) return -1;
    if (chunk==0) chunk=1;
    tp_parfor_state S; S.next=begin; S.end=end; S.chunk=chunk; S.cb=cb; S.u=u;
    pth_mutex_init(&S.mu);

    size_t k = p->nth? p->nth : 1, launched=0;
    for (size_t i=0;i<k;i++){
        if (tp_submit(p, (tp_task_fn)tp_parfor_worker, &S)==0) launched++;
        else break;
    }
    tp_wait_idle(p);
    pth_mutex_destroy(&S.mu);
    return launched?0:-1;
}

/* ===== Infos / stats ===== */
TP_API size_t tp_queue_len(tp_pool* p){ if(!p) return 0; if (pth_mutex_lock(&p->mu)!=0) return 0; size_t v=p->que.len; pth_mutex_unlock(&p->mu); return v; }
TP_API size_t tp_queue_cap(tp_pool* p){ return p? p->que.cap : 0; }
TP_API size_t tp_threads(tp_pool* p){ return p? p->nth : 0; }
TP_API size_t tp_active(tp_pool* p){ if(!p) return 0; if (pth_mutex_lock(&p->mu)!=0) return 0; size_t v=p->active; pth_mutex_unlock(&p->mu); return v; }
TP_API size_t tp_completed(tp_pool* p){ if(!p) return 0; if (pth_mutex_lock(&p->mu)!=0) return 0; size_t v=p->completed; pth_mutex_unlock(&p->mu); return v; }

/* ===== Test minimal ===== */
#ifdef TP_TEST
#include <stdio.h>
static void w(void* a){ volatile unsigned long s=0; for (unsigned i=0;i<100000;i++) s+=i; (void)s; if(a) (*(int*)a)++; }
static void pf(size_t i0,size_t i1,void* u){ (void)u; for(size_t i=i0;i<i1;i++) w(NULL); }
int main(void){
    tp_pool tp; if (tp_init(&tp,4,128)!=0){ fprintf(stderr,"init fail\n"); return 1; }
    int cnt=0; for (int i=0;i<200;i++) tp_submit(&tp,w,&cnt);
    tp_wait_idle(&tp);
    printf("completed=%zu cnt=%d q=%zu\n", tp_completed(&tp), cnt, tp_queue_len(&tp));
    tp_parallel_for(&tp,0,1000,50,pf,NULL);
    tp_shutdown(&tp,1);
    return 0;
}
#endif