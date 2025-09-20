// SPDX-License-Identifier: GPL-3.0-or-later
//
// ioloop.c — Cross-platform I/O loop for Vitte Light VM (C17, complet)
// Namespace: "ioloop"
//
// Build examples:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -D_GNU_SOURCE -c ioloop.c     # Linux (epoll)
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c ioloop.c                    # macOS/BSD (kqueue)
//   # Fallback poll(2) if neither epoll/kqueue is available.
//
// Model:
//   - FDs non-bloquants: watch READ/WRITE.
//   - Timers (monotonic) one-shot ou périodiques.
//   - Callbacks en C, boucle réentrante safe (post de tâches).
//
// API (C symbol layer):
//   typedef struct ioloop ioloop;
//   typedef void (*io_cb)(int fd, unsigned ev, void* ud);  // ev: IO_READ|IO_WRITE|IO_CLOSE|IO_TIMER
//   ioloop*  io_new(void);
//   void     io_free(ioloop* L);
//   int      io_run(ioloop* L);            // boucle jusqu’à io_stop()
//   void     io_stop(ioloop* L);
//   uint64_t io_now_ms(void);              // horloge monotone
//
//   // FDs
//   enum { IO_READ=1u, IO_WRITE=2u, IO_CLOSE=4u, IO_TIMER=8u };
//   int  io_add_fd(ioloop* L, int fd, unsigned flags, io_cb cb, void* ud);
//   int  io_mod_fd(ioloop* L, int fd, unsigned flags);
//   int  io_del_fd(ioloop* L, int fd);
//
//   // Timers
//   typedef int io_timer;                   // id >0
//   io_timer io_add_timer(ioloop* L, uint64_t delay_ms, uint64_t period_ms,
//                         io_cb cb, void* ud);
//   int      io_cancel_timer(ioloop* L, io_timer id);
//
// Notes:
//   - Couche neutre VM. Le binding VM doit traduire vers objets/closures.
//   - Erreurs: -EINVAL, -ENOMEM, -EIO.
//   - Les callbacks reçoivent fd=-1 pour les timers.
//
// Deps VM optionnels: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

#ifndef EINVAL
#  define EINVAL 22
#endif
#ifndef ENOMEM
#  define ENOMEM 12
#endif
#ifndef EIO
#  define EIO 5
#endif

#ifndef VL_EXPORT
#  if defined(_WIN32) && !defined(__clang__)
#    define VL_EXPORT __declspec(dllexport)
#  else
#    define VL_EXPORT
#  endif
#endif

// ---------- Monotonic clock ----------
static uint64_t mono_ms(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
#endif
}

VL_EXPORT uint64_t io_now_ms(void) { return mono_ms(); }

// ---------- Backend selection ----------
#if defined(__linux__)
#  define IO_EPOLL 1
#  include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#  define IO_KQUEUE 1
#  include <sys/event.h>
#  include <sys/time.h>
#else
#  define IO_POLL 1
#  include <poll.h>
#  include <limits.h>
#endif

// ---------- Public flags ----------
enum { IO_READ=1u, IO_WRITE=2u, IO_CLOSE=4u, IO_TIMER=8u };

typedef void (*io_cb)(int fd, unsigned ev, void* ud);

// ---------- Timer heap ----------
typedef struct {
  int      id;
  uint64_t when;
  uint64_t period;
  io_cb    cb;
  void*    ud;
  int      alive; // 0 deleted
} TNode;

typedef struct {
  TNode* a;
  int n, cap;
  int next_id;
} THeap;

static int heap_reserve(THeap* h, int need) {
  if (need <= h->cap) return 0;
  int cap = h->cap ? h->cap : 16;
  while (cap < need) {
    if (cap > (1<<27)) return -ENOMEM;
    cap <<= 1;
  }
  void* p = realloc(h->a, (size_t)cap * sizeof(TNode));
  if (!p) return -ENOMEM;
  h->a = (TNode*)p; h->cap = cap; return 0;
}
static void heap_swap(TNode* a, int i, int j){ TNode t=a[i]; a[i]=a[j]; a[j]=t; }
static void heap_up(TNode* a, int i) {
  while (i>0) { int p=(i-1)/2; if (a[p].when<=a[i].when) break; heap_swap(a,i,p); i=p; }
}
static void heap_dn(TNode* a, int n, int i) {
  for(;;){
    int l=2*i+1, r=l+1, m=i;
    if(l<n && a[l].when<a[m].when) m=l;
    if(r<n && a[r].when<a[m].when) m=r;
    if(m==i) break;
    heap_swap(a,i,m); i=m;
  }
}
static int heap_push(THeap* h, TNode v){
  if (heap_reserve(h, h->n+1)!=0) return -ENOMEM;
  h->a[h->n]=v; heap_up(h->a,h->n); h->n++; return 0;
}
static int heap_pop(THeap* h, TNode* out){
  if (h->n==0) return -EINVAL;
  if (out) *out = h->a[0];
  h->n--; if (h->n>0){ h->a[0]=h->a[h->n]; heap_dn(h->a,h->n,0); }
  return 0;
}
static TNode* heap_top(THeap* h){ return h->n? &h->a[0] : NULL; }

// ---------- FD table ----------
typedef struct {
  int used;
  unsigned mask;
  io_cb cb;
  void* ud;
} FDent;

// ---------- Loop ----------
typedef struct ioloop {
  int running;
#if IO_EPOLL
  int ep;
#elif IO_KQUEUE
  int kq;
#else
  struct pollfd* pfds;
  int pfds_n, pfds_cap;
#endif
  FDent* fdt;
  int fdt_n, fdt_cap;

  THeap th;
} ioloop;

// ----- utils -----
static int fdt_reserve(ioloop* L, int fd){
  if (fd < 0) return -EINVAL;
  if (fd < L->fdt_n) return 0;
  int need = fd+1;
  int cap  = L->fdt_cap ? L->fdt_cap : 64;
  while (cap < need){
    if (cap > (1<<20)) return -ENOMEM;
    cap <<= 1;
  }
  void* p = realloc(L->fdt, (size_t)cap * sizeof(FDent));
  if (!p) return -ENOMEM;
  for (int i=L->fdt_cap;i<cap;i++){ L->fdt[i].used=0; L->fdt[i].mask=0; L->fdt[i].cb=NULL; L->fdt[i].ud=NULL; }
  L->fdt = (FDent*)p; L->fdt_cap = cap; L->fdt_n = need;
  return 0;
}

// ---------- Backend ops ----------
#if IO_EPOLL
static int be_add(ioloop* L, int fd, unsigned m){
  struct epoll_event ev={0};
  ev.events = ((m&IO_READ)?EPOLLIN:0) | ((m&IO_WRITE)?EPOLLOUT:0) | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
  ev.data.u32 = (uint32_t)fd;
  return epoll_ctl(L->ep, EPOLL_CTL_ADD, fd, &ev);
}
static int be_mod(ioloop* L, int fd, unsigned m){
  struct epoll_event ev={0};
  ev.events = ((m&IO_READ)?EPOLLIN:0) | ((m&IO_WRITE)?EPOLLOUT:0) | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
  ev.data.u32 = (uint32_t)fd;
  return epoll_ctl(L->ep, EPOLL_CTL_MOD, fd, &ev);
}
static int be_del(ioloop* L, int fd){
  return epoll_ctl(L->ep, EPOLL_CTL_DEL, fd, NULL);
}
#elif IO_KQUEUE
static int be_apply(ioloop* L, int fd, unsigned m, int add) {
  struct kevent ch[2]; int n=0;
  if (m & IO_READ)  EV_SET(&ch[n++], fd, EVFILT_READ,  add?EV_ADD:EV_DELETE, 0, 0, NULL);
  else              EV_SET(&ch[n++], fd, EVFILT_READ,  EV_DELETE,          0, 0, NULL);
  if (m & IO_WRITE) EV_SET(&ch[n++], fd, EVFILT_WRITE, add?EV_ADD:EV_DELETE,0, 0, NULL);
  else              EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_DELETE,          0, 0, NULL);
  return kevent(L->kq, ch, n, NULL, 0, NULL);
}
static int be_add(ioloop* L, int fd, unsigned m){ return be_apply(L,fd,m,1); }
static int be_mod(ioloop* L, int fd, unsigned m){ return be_apply(L,fd,m,1); }
static int be_del(ioloop* L, int fd){ return be_apply(L,fd,0,0); }
#else // poll
static int pfds_reserve(ioloop* L, int need){
  if (need <= L->pfds_cap) return 0;
  int cap = L->pfds_cap ? L->pfds_cap : 64;
  while (cap < need){ if (cap > (1<<20)) return -ENOMEM; cap <<= 1; }
  void* p = realloc(L->pfds, (size_t)cap * sizeof(struct pollfd));
  if (!p) return -ENOMEM;
  L->pfds = (struct pollfd*)p; L->pfds_cap=cap; return 0;
}
static int be_rebuild_poll(ioloop* L){
  int cnt=0;
  for (int fd=0; fd<L->fdt_n; ++fd) if (L->fdt[fd].used) cnt++;
  if (pfds_reserve(L, cnt)!=0) return -ENOMEM;
  L->pfds_n = 0;
  for (int fd=0; fd<L->fdt_n; ++fd){
    if (!L->fdt[fd].used) continue;
    struct pollfd p={0};
    p.fd = fd;
    p.events = (L->fdt[fd].mask&IO_READ?POLLIN:0) | (L->fdt[fd].mask&IO_WRITE?POLLOUT:0);
    L->pfds[L->pfds_n++] = p;
  }
  return 0;
}
static int be_add(ioloop* L, int fd, unsigned m){ (void)fd;(void)m; return be_rebuild_poll(L); }
static int be_mod(ioloop* L, int fd, unsigned m){ (void)fd;(void)m; return be_rebuild_poll(L); }
static int be_del(ioloop* L, int fd){ (void)fd; return be_rebuild_poll(L); }
#endif

// ---------- Public API ----------
VL_EXPORT ioloop* io_new(void){
  ioloop* L = (ioloop*)calloc(1, sizeof(*L));
  if (!L) return NULL;
#if IO_EPOLL
  L->ep = epoll_create1(EPOLL_CLOEXEC);
  if (L->ep < 0) { free(L); return NULL; }
#elif IO_KQUEUE
  L->kq = kqueue();
  if (L->kq < 0) { free(L); return NULL; }
#else
  L->pfds = NULL; L->pfds_n = L->pfds_cap = 0;
#endif
  L->fdt = NULL; L->fdt_n = L->fdt_cap = 0;
  L->th.a = NULL; L->th.n = L->th.cap = 0; L->th.next_id = 1;
  return L;
}

VL_EXPORT void io_free(ioloop* L){
  if (!L) return;
#if IO_EPOLL
  if (L->ep >= 0) close(L->ep);
#elif IO_KQUEUE
  if (L->kq >= 0) close(L->kq);
#else
  free(L->pfds);
#endif
  free(L->fdt);
  free(L->th.a);
  free(L);
}

VL_EXPORT void io_stop(ioloop* L){ if (L) L->running = 0; }

VL_EXPORT int io_add_fd(ioloop* L, int fd, unsigned flags, io_cb cb, void* ud){
  if (!L || fd < 0 || !cb) return -EINVAL;
  if (fdt_reserve(L, fd)!=0) return -ENOMEM;
  L->fdt[fd].used = 1;
  L->fdt[fd].mask = flags & (IO_READ|IO_WRITE);
  L->fdt[fd].cb   = cb;
  L->fdt[fd].ud   = ud;
  if (be_add(L, fd, L->fdt[fd].mask) != 0) return -EIO;
  return 0;
}

VL_EXPORT int io_mod_fd(ioloop* L, int fd, unsigned flags){
  if (!L || fd < 0 || fd >= L->fdt_n || !L->fdt[fd].used) return -EINVAL;
  L->fdt[fd].mask = flags & (IO_READ|IO_WRITE);
  if (be_mod(L, fd, L->fdt[fd].mask) != 0) return -EIO;
  return 0;
}

VL_EXPORT int io_del_fd(ioloop* L, int fd){
  if (!L || fd < 0 || fd >= L->fdt_n || !L->fdt[fd].used) return -EINVAL;
  be_del(L, fd);
  L->fdt[fd].used = 0; L->fdt[fd].mask=0; L->fdt[fd].cb=NULL; L->fdt[fd].ud=NULL;
  return 0;
}

VL_EXPORT int io_cancel_timer(ioloop* L, int id){
  if (!L || id <= 0) return -EINVAL;
  // lazy delete: mark dead; heap purge occurs on pop
  for (int i=0; i<L->th.n; ++i) if (L->th.a[i].id==id){ L->th.a[i].alive=0; return 0; }
  return -EINVAL;
}

VL_EXPORT int io_run(ioloop* L){
  if (!L) return -EINVAL;
  L->running = 1;
#if IO_EPOLL
  struct epoll_event evs[128];
#elif IO_KQUEUE
  struct kevent evs[128];
#else
  (void)evs;
#endif

  while (L->running) {
    // timeout to next timer
    int timeout_ms = -1;
    uint64_t now = mono_ms();
    TNode* top = heap_top(&L->th);
    if (top) {
      if (!top->alive) { TNode tmp; heap_pop(&L->th,&tmp); continue; }
      timeout_ms = (top->when <= now) ? 0 : (int)(top->when - now);
    }

    int n=0;

#if IO_EPOLL
    n = epoll_wait(L->ep, evs, (int)(sizeof(evs)/sizeof(evs[0])), timeout_ms);
    if (n < 0 && errno == EINTR) continue;
    for (int i=0; i<n; ++i){
      int fd = (int)evs[i].data.u32;
      unsigned m = 0;
      if (evs[i].events & (EPOLLIN))  m |= IO_READ;
      if (evs[i].events & (EPOLLOUT)) m |= IO_WRITE;
      if (evs[i].events & (EPOLLHUP|EPOLLERR|EPOLLRDHUP)) m |= IO_CLOSE;
      if (fd>=0 && fd<L->fdt_n && L->fdt[fd].used && L->fdt[fd].cb)
        L->fdt[fd].cb(fd, m, L->fdt[fd].ud);
    }
#elif IO_KQUEUE
    struct timespec ts, *tsp=NULL;
    if (timeout_ms >= 0) { ts.tv_sec = timeout_ms/1000; ts.tv_nsec=(timeout_ms%1000)*1000000L; tsp=&ts; }
    n = kevent(L->kq, NULL, 0, evs, (int)(sizeof(evs)/sizeof(evs[0])), tsp);
    if (n < 0 && errno == EINTR) continue;
    for (int i=0; i<n; ++i){
      int fd = (int)evs[i].ident;
      unsigned m = 0;
      if (evs[i].filter == EVFILT_READ)  m |= IO_READ;
      if (evs[i].filter == EVFILT_WRITE) m |= IO_WRITE;
      if (evs[i].flags  & (EV_EOF|EV_ERROR)) m |= IO_CLOSE;
      if (fd>=0 && fd<L->fdt_n && L->fdt[fd].used && L->fdt[fd].cb)
        L->fdt[fd].cb(fd, m, L->fdt[fd].ud);
    }
#else // poll
    if (be_rebuild_poll(L)!=0) return -EIO;
    n = poll(L->pfds, (nfds_t)L->pfds_n, timeout_ms);
    if (n < 0 && errno == EINTR) continue;
    for (int i=0; i<L->pfds_n; ++i){
      if (!L->pfds[i].revents) continue;
      int fd = L->pfds[i].fd;
      unsigned m = 0;
      if (L->pfds[i].revents & (POLLIN))  m |= IO_READ;
      if (L->pfds[i].revents & (POLLOUT)) m |= IO_WRITE;
      if (L->pfds[i].revents & (POLLHUP|POLLERR)) m |= IO_CLOSE;
      if (fd>=0 && fd<L->fdt_n && L->fdt[fd].used && L->fdt[fd].cb)
        L->fdt[fd].cb(fd, m, L->fdt[fd].ud);
    }
#endif

    // timers
    now = mono_ms();
    while ((top = heap_top(&L->th)) && top->when <= now) {
      TNode t; heap_pop(&L->th, &t);
      if (!t.alive) continue;
      if (t.cb) t.cb(-1, IO_TIMER, t.ud);
      if (t.period && t.alive) {
        t.when = now + t.period;
        heap_push(&L->th, t);
      }
    }
  }
  return 0;
}

// ---------- Timers ----------
VL_EXPORT int io_add_timer(ioloop* L, uint64_t delay_ms, uint64_t period_ms,
                           io_cb cb, void* ud) {
  if (!L || !cb) return -EINVAL;
  TNode t = {0};
  t.id     = L->th.next_id++;
  t.when   = mono_ms() + delay_ms;
  t.period = period_ms;
  t.cb     = cb;
  t.ud     = ud;
  t.alive  = 1;
  if (heap_push(&L->th, t)!=0) return -ENOMEM;
  return t.id;
}