// lcorolib.c (stub)
// vitte-light/libraries/corolib.c
// Coroutines coopératives en C99 (style protothreads) + mini ordonnanceur.
// Zéro allocation cachée; tout est explicite. Fournit aussi des helpers
// d'await et de sleep sur horloge monotone (tm.h).
//
// API (demandez `corolib.h` pour un header dédié si besoin):
//   typedef struct VL_Coro { unsigned state; int done; uint64_t wake_ns; }
//   VL_Coro; #define VL_CORO_BEGIN(co) #define VL_CORO_YIELD(co) #define
//   VL_CORO_AWAIT(co, cond) #define VL_CORO_SLEEP_NS(co, ns) #define
//   VL_CORO_SLEEP_MS(co, ms) #define VL_CORO_END(co) typedef int
//   (*VL_CoroFn)(VL_Coro *co, void *ud); // 0=encore, 1=terminé
//
//   // Ordonnanceur simple
//   typedef struct VL_Task { VL_Coro co; VL_CoroFn fn; void *ud; } VL_Task;
//   typedef struct VL_Sched { VL_Task *t; size_t n, cap; } VL_Sched;
//   void   vl_sched_init(VL_Sched *s);
//   void   vl_sched_free(VL_Sched *s);
//   int    vl_sched_add(VL_Sched *s, VL_CoroFn fn, void *ud);       // index ou
//   -1 size_t vl_sched_step(VL_Sched *s);                              //
//   actifs restants void   vl_sched_run_until_empty(VL_Sched *s, uint32_t
//   idle_ms); // boucle avec veille
//
//   // Canal MPMC lock-free simple (anneau; non bloquant)
//   typedef struct VL_Channel { void **q; size_t cap, r, w, n; } VL_Channel;
//   int    vl_chan_init(VL_Channel *c, size_t cap);
//   void   vl_chan_free(VL_Channel *c);
//   int    vl_chan_send(VL_Channel *c, void *msg); // 1 si ok, 0 si plein
//   int    vl_chan_recv(VL_Channel *c, void **msg); // 1 si ok, 0 si vide
//
// Intégration optionnelle VM (définir VL_COROLIB_WITH_VM avant l'include):
//   int vl_coro_vm_timeslice(VL_Coro *co, struct VL_Context *ctx,
//                            uint64_t max_steps, uint64_t slice_ns);
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/corolib.c

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tm.h"  // horloge monotone + sleep

// ───────────────────────── Base coroutine ─────────────────────────
// Le champ `state` contient la prochaine étiquette à exécuter.
// Les macros utilisent __LINE__ pour générer des points de reprise uniques.

typedef struct VL_Coro {
  unsigned state;
  int done;
  uint64_t wake_ns;
} VL_Coro;

#define VL_CORO_BEGIN(co) \
  switch ((co)->state) {  \
    case 0:
#define VL_CORO_YIELD(co)   \
  do {                      \
    (co)->state = __LINE__; \
    return 0;               \
    case __LINE__:;         \
  } while (0)
#define VL_CORO_AWAIT(co, cond) \
  do {                          \
    while (!(cond)) {           \
      VL_CORO_YIELD(co);        \
    }                           \
  } while (0)
#define VL_CORO_SLEEP_NS(co, ns)                           \
  do {                                                     \
    (co)->wake_ns = vl_mono_time_ns() + (uint64_t)(ns);    \
    VL_CORO_AWAIT(co, vl_mono_time_ns() >= (co)->wake_ns); \
  } while (0)
#define VL_CORO_SLEEP_MS(co, ms) \
  VL_CORO_SLEEP_NS(co, ((uint64_t)(ms)) * 1000000ull)
#define VL_CORO_END(co) \
  }                     \
  (co)->done = 1;       \
  return 1

// ───────────────────────── Ordonnanceur ─────────────────────────

typedef int (*VL_CoroFn)(VL_Coro *co, void *ud);

typedef struct VL_Task {
  VL_Coro co;
  VL_CoroFn fn;
  void *ud;
} VL_Task;

typedef struct VL_Sched {
  VL_Task *t;
  size_t n, cap;
} VL_Sched;

static int ensure_cap(void **ptr, size_t *cap, size_t need, size_t esz) {
  if (need <= *cap) return 1;
  size_t nc = (*cap) ? (*cap * 2) : 8;
  while (nc < need) nc += nc / 2;
  void *np = realloc(*ptr, nc * esz);
  if (!np) return 0;
  *ptr = np;
  *cap = nc;
  return 1;
}

void vl_sched_init(VL_Sched *s) {
  if (!s) return;
  s->t = NULL;
  s->n = 0;
  s->cap = 0;
}

void vl_sched_free(VL_Sched *s) {
  if (!s) return;
  free(s->t);
  s->t = NULL;
  s->n = s->cap = 0;
}

int vl_sched_add(VL_Sched *s, VL_CoroFn fn, void *ud) {
  if (!s || !fn) return -1;
  if (!ensure_cap((void **)&s->t, &s->cap, s->n + 1, sizeof(VL_Task)))
    return -1;
  VL_Task *tk = &s->t[s->n];
  memset(tk, 0, sizeof(*tk));
  tk->fn = fn;
  tk->ud = ud;
  tk->co.state = 0;
  tk->co.done = 0;
  tk->co.wake_ns = 0;
  return (int)(s->n++);
}

static void task_remove_unordered(VL_Sched *s, size_t i) {
  if (i >= s->n) return;
  s->t[i] = s->t[s->n - 1];
  s->n--;
}

size_t vl_sched_step(VL_Sched *s) {
  if (!s) return 0;
  for (size_t i = 0; i < s->n;) {
    int done = s->t[i].fn(&s->t[i].co, s->t[i].ud);
    if (done || s->t[i].co.done) {
      task_remove_unordered(s, i);
    } else {
      i++;
    }
  }
  return s->n;
}

void vl_sched_run_until_empty(VL_Sched *s, uint32_t idle_ms) {
  if (!s) return;
  while (s->n) {
    size_t left = vl_sched_step(s);
    if (left && idle_ms) {
      vl_sleep_ms(idle_ms);
    }
  }
}

// ───────────────────────── Canal circulaire ─────────────────────────

typedef struct VL_Channel {
  void **q;
  size_t cap, r, w, n;
} VL_Channel;

int vl_chan_init(VL_Channel *c, size_t cap) {
  if (!c || cap == 0) return 0;
  c->q = (void **)calloc(cap, sizeof(void *));
  if (!c->q) return 0;
  c->cap = cap;
  c->r = c->w = c->n = 0;
  return 1;
}
void vl_chan_free(VL_Channel *c) {
  if (!c) return;
  free(c->q);
  memset(c, 0, sizeof(*c));
}

int vl_chan_send(VL_Channel *c, void *msg) {
  if (!c || !c->q) return 0;
  if (c->n == c->cap) return 0;
  c->q[c->w] = msg;
  c->w = (c->w + 1u) % c->cap;
  c->n++;
  return 1;
}
int vl_chan_recv(VL_Channel *c, void **msg) {
  if (!c || !c->q || !msg) return 0;
  if (c->n == 0) return 0;
  *msg = c->q[c->r];
  c->r = (c->r + 1u) % c->cap;
  c->n--;
  return 1;
}

// ───────────────────────── Intégration optionnelle VM
// ─────────────────────────
#ifdef VL_COROLIB_WITH_VM
#include "state.h"
#include "vm.h"
// Exécute des pas jusqu'à HALT, ou jusqu'à épuisement du budget de steps/temps.
// Retour: 1 si terminé (HALT), 0 sinon.
int vl_coro_vm_timeslice(VL_Coro *co, struct VL_Context *ctx,
                         uint64_t max_steps, uint64_t slice_ns) {
  VL_CORO_BEGIN(co);
  for (;;) {
    uint64_t start = vl_mono_time_ns();
    uint64_t steps = 0;
    for (;;) {
      if (max_steps && steps >= max_steps) break;
      VL_Status st = vl_step(ctx);
      if (st == VL_DONE) {
        VL_CORO_END(co);
      }
      if (st != VL_OK) {  // erreur => stop
        VL_CORO_END(co);
      }
      steps++;
      if (slice_ns && (vl_mono_time_ns() - start) >= slice_ns) break;
    }
    VL_CORO_YIELD(co);
  }
  VL_CORO_END(co);
}
#endif

// ───────────────────────── Démo & autotest ─────────────────────────
#ifdef VL_COROLIB_TEST_MAIN
// tache 1: clignote 3 fois
static int blink(VL_Coro *co, void *ud) {
  (void)ud;
  VL_CORO_BEGIN(co);
  for (int i = 0; i < 3; i++) {
    printf("blink %d\n", i);
    VL_CORO_SLEEP_MS(co, 50);
  }
  VL_CORO_END(co);
}
// tache 2: ping-pong via canal
typedef struct {
  VL_Channel *ch;
  int id;
} PingArg;
static int pinger(VL_Coro *co, void *ud) {
  PingArg *pa = (PingArg *)ud;
  VL_CORO_BEGIN(co);
  for (int i = 0; i < 5; i++) {
    char *msg = "ping";
    while (!vl_chan_send(pa->ch, msg)) VL_CORO_YIELD(co);
    VL_CORO_SLEEP_MS(co, 10);
  }
  VL_CORO_END(co);
}
static int ponger(VL_Coro *co, void *ud) {
  PingArg *pa = (PingArg *)ud;
  void *msg = NULL;
  VL_CORO_BEGIN(co);
  for (;;) {
    while (!vl_chan_recv(pa->ch, &msg)) VL_CORO_YIELD(co);
    printf("got %s\n", (char *)msg);
  }
  VL_CORO_END(co);
}
int main(void) {
  VL_Sched sch;
  vl_sched_init(&sch);
  VL_Channel ch;
  vl_chan_init(&ch, 4);
  PingArg a = {&ch, 1};
  vl_sched_add(&sch, blink, NULL);
  vl_sched_add(&sch, pinger, &a);
  vl_sched_add(&sch, ponger, &a);
  vl_sched_run_until_empty(&sch, 1);
  vl_chan_free(&ch);
  vl_sched_free(&sch);
  return 0;
}
#endif
