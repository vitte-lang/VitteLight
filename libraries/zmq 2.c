// SPDX-License-Identifier: GPL-3.0-or-later
//
// zmq.c — Fines abstractions ZeroMQ (optionnel) pour Vitte Light, C17 portable
// Namespace: "zq"
//
// Idée: un très petit wrapper qui compile même sans libzmq.
// - Si HAVE_ZMQ est défini: utilise <zmq.h> réel.
// - Sinon: stubs qui renvoient ZQ_ENOZMQ.
//
// Build avec ZeroMQ:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DHAVE_ZMQ zmq.c -lzmq
// Build sans ZeroMQ (stubs):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic zmq.c
//
// API:
//   typedef struct { void* ctx; }  zq_ctx;
//   typedef struct { void* s;  }   zq_sock;
//   enum { ZQ_OK=0, ZQ_EIO=-1, ZQ_EZMQ=-2, ZQ_EARG=-3, ZQ_ENOZMQ=-5 };
//
//   int  zq_init(zq_ctx* c, int io_threads);
//   int  zq_term(zq_ctx* c);
//   int  zq_socket(zq_ctx* c, zq_sock* s, int type);        // type: ZMQ_REQ/REP/PUB/SUB/..
//   int  zq_close(zq_sock* s);
//   int  zq_bind (zq_sock* s, const char* endpoint);
//   int  zq_connect(zq_sock* s, const char* endpoint);
//   int  zq_setsockopt_int(zq_sock* s, int opt, int val);
//   int  zq_getsockopt_int(zq_sock* s, int opt, int* out);
//   int  zq_subscribe(zq_sock* s, const void* prefix, size_t n);  // SUB only
//   int  zq_send   (zq_sock* s, const void* buf, size_t n, int dontwait);
//   int  zq_send_str(zq_sock* s, const char* z, int dontwait);
//   int  zq_recv   (zq_sock* s, void* buf, size_t cap, size_t* out_n, int dontwait);
//   int  zq_recv_dyn(zq_sock* s, void** out, size_t* out_n, int dontwait); // malloc
//   int  zq_poll(zq_sock* arr, int n, int timeout_ms, int* revents);       // revents bit0=IN, bit1=OUT
//
// Notes:
//   - dontwait !=0 -> ZMQ_DONTWAIT.
//   - zq_poll revents[i]: bit1(1)=IN lisible, bit2(2)=OUT émettable.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef ZQ_API
#define ZQ_API
#endif

enum { ZQ_OK=0, ZQ_EIO=-1, ZQ_EZMQ=-2, ZQ_EARG=-3, ZQ_ENOZMQ=-5 };

typedef struct { void* ctx; }  zq_ctx;
typedef struct { void* s;  }   zq_sock;

#if defined(HAVE_ZMQ)
  #include <zmq.h>
  #define ZQ__CASTCTX(c) ((void*)(c)->ctx)
  #define ZQ__CASTSOCK(s) ((void*)(s)->s)
#else
  /* Stubs sans ZeroMQ */
  #define ZQ__CASTCTX(c) ((void*)(c)->ctx)
  #define ZQ__CASTSOCK(s) ((void*)(s)->s)
#endif

/* ================= Contexte ================= */

ZQ_API int zq_init(zq_ctx* c, int io_threads){
    if (!c) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    if (io_threads <= 0) io_threads = 1;
    void* ctx = zmq_ctx_new();
    if (!ctx) return ZQ_EZMQ;
    if (zmq_ctx_set(ctx, ZMQ_IO_THREADS, io_threads) != 0){
        zmq_ctx_term(ctx);
        return ZQ_EZMQ;
    }
    c->ctx = ctx;
    return ZQ_OK;
#else
    (void)io_threads; c->ctx=NULL; return ZQ_ENOZMQ;
#endif
}

ZQ_API int zq_term(zq_ctx* c){
    if (!c) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    if (!c->ctx) return ZQ_OK;
    if (zmq_ctx_term(ZQ__CASTCTX(c)) != 0) return ZQ_EZMQ;
    c->ctx=NULL; return ZQ_OK;
#else
    c->ctx=NULL; return ZQ_ENOZMQ;
#endif
}

/* ================= Sockets ================= */

ZQ_API int zq_socket(zq_ctx* c, zq_sock* s, int type){
    if (!c||!s) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    void* so = zmq_socket(ZQ__CASTCTX(c), type);
    if (!so) return ZQ_EZMQ;
    s->s = so; return ZQ_OK;
#else
    (void)type; s->s=NULL; return ZQ_ENOZMQ;
#endif
}

ZQ_API int zq_close(zq_sock* s){
    if (!s) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    if (!s->s) return ZQ_OK;
    if (zmq_close(ZQ__CASTSOCK(s)) != 0) return ZQ_EZMQ;
    s->s=NULL; return ZQ_OK;
#else
    s->s=NULL; return ZQ_ENOZMQ;
#endif
}

ZQ_API int zq_bind(zq_sock* s, const char* endpoint){
    if (!s||!endpoint) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    return zmq_bind(ZQ__CASTSOCK(s), endpoint)==0? ZQ_OK : ZQ_EZMQ;
#else
    (void)endpoint; return ZQ_ENOZMQ;
#endif
}

ZQ_API int zq_connect(zq_sock* s, const char* endpoint){
    if (!s||!endpoint) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    return zmq_connect(ZQ__CASTSOCK(s), endpoint)==0? ZQ_OK : ZQ_EZMQ;
#else
    (void)endpoint; return ZQ_ENOZMQ;
#endif
}

/* ================= Options ================= */

ZQ_API int zq_setsockopt_int(zq_sock* s, int opt, int val){
    if (!s) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    return zmq_setsockopt(ZQ__CASTSOCK(s), opt, &val, sizeof val)==0? ZQ_OK : ZQ_EZMQ;
#else
    (void)opt; (void)val; return ZQ_ENOZMQ;
#endif
}
ZQ_API int zq_getsockopt_int(zq_sock* s, int opt, int* out){
    if (!s||!out) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    size_t sz=sizeof *out;
    return zmq_getsockopt(ZQ__CASTSOCK(s), opt, out, &sz)==0? ZQ_OK : ZQ_EZMQ;
#else
    (void)opt; *out=0; return ZQ_ENOZMQ;
#endif
}

ZQ_API int zq_subscribe(zq_sock* s, const void* prefix, size_t n){
    if (!s) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    return zmq_setsockopt(ZQ__CASTSOCK(s), ZMQ_SUBSCRIBE, prefix?prefix:"", n)==0? ZQ_OK : ZQ_EZMQ;
#else
    (void)prefix; (void)n; return ZQ_ENOZMQ;
#endif
}

/* ================= Envoi / Réception ================= */

ZQ_API int zq_send(zq_sock* s, const void* buf, size_t n, int dontwait){
    if (!s) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    int flags = dontwait? ZMQ_DONTWAIT : 0;
    int rc = zmq_send(ZQ__CASTSOCK(s), buf, n, flags);
    return (rc>=0)? ZQ_OK : ((errno==EAGAIN)? ZQ_EIO : ZQ_EZMQ);
#else
    (void)buf; (void)n; (void)dontwait; return ZQ_ENOZMQ;
#endif
}
ZQ_API int zq_send_str(zq_sock* s, const char* z, int dontwait){
    return zq_send(s, z?z:"", z?strlen(z):0, dontwait);
}

ZQ_API int zq_recv(zq_sock* s, void* buf, size_t cap, size_t* out_n, int dontwait){
    if (!s||!buf) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    int flags = dontwait? ZMQ_DONTWAIT : 0;
    int rc = zmq_recv(ZQ__CASTSOCK(s), buf, cap, flags);
    if (rc>=0){ if(out_n) *out_n=(size_t)rc; return ZQ_OK; }
    return (errno==EAGAIN)? ZQ_EIO : ZQ_EZMQ;
#else
    (void)cap; if(out_n) *out_n=0; (void)dontwait; return ZQ_ENOZMQ;
#endif
}

ZQ_API int zq_recv_dyn(zq_sock* s, void** out, size_t* out_n, int dontwait){
    if (!s||!out||!out_n) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    zmq_msg_t m;
    if (zmq_msg_init(&m)!=0) return ZQ_EZMQ;
    int flags = dontwait? ZMQ_DONTWAIT : 0;
    int rc = zmq_msg_recv(&m, ZQ__CASTSOCK(s), flags);
    if (rc<0){ zmq_msg_close(&m); return (errno==EAGAIN)? ZQ_EIO : ZQ_EZMQ; }
    size_t len = (size_t)rc;
    void* p = malloc(len?len:1);
    if (!p){ zmq_msg_close(&m); return ZQ_EIO; }
    memcpy(p, zmq_msg_data(&m), len);
    zmq_msg_close(&m);
    *out = p; *out_n = len;
    return ZQ_OK;
#else
    *out=NULL; *out_n=0; (void)dontwait; return ZQ_ENOZMQ;
#endif
}

/* ================= Poll ================= */

ZQ_API int zq_poll(zq_sock* arr, int n, int timeout_ms, int* revents){
    if (!arr || n<=0) return ZQ_EARG;
#if defined(HAVE_ZMQ)
    zmq_pollitem_t* it = (zmq_pollitem_t*)calloc((size_t)n, sizeof *it);
    if (!it) return ZQ_EIO;
    for (int i=0;i<n;i++){ it[i].socket = ZQ__CASTSOCK(&arr[i]); it[i].events = ZMQ_POLLIN | ZMQ_POLLOUT; }
    int rc = zmq_poll(it, (int)n, timeout_ms);
    if (rc<0){ free(it); return ZQ_EZMQ; }
    if (revents){
        for (int i=0;i<n;i++){
            int ev=0;
            if (it[i].revents & ZMQ_POLLIN)  ev |= 1;
            if (it[i].revents & ZMQ_POLLOUT) ev |= 2;
            revents[i]=ev;
        }
    }
    free(it);
    return rc; /* >=0: nombre d'items prêts */
#else
    (void)timeout_ms;
    if (revents) memset(revents,0,(size_t)n*sizeof *revents);
    return ZQ_ENOZMQ;
#endif
}

/* ================= Test rapide ================= */
#ifdef ZQ_TEST
#include <assert.h>
int main(void){
    zq_ctx c; int rc=zq_init(&c,1);
    if (rc==ZQ_ENOZMQ){ puts("No ZeroMQ linked"); return 0; }
    assert(rc==0);
    zq_sock pub, sub;
    assert(zq_socket(&c,&pub, ZMQ_PUB)==0);
    assert(zq_socket(&c,&sub, ZMQ_SUB)==0);
    assert(zq_bind(&pub, "inproc://demo")==0);
    assert(zq_connect(&sub,"inproc://demo")==0);
    assert(zq_subscribe(&sub,"",0)==0);
    /* petit délai de prêt PUB/SUB */
#if defined(_WIN32)
    Sleep(50);
#else
    struct timespec ts={0,50*1000000L}; nanosleep(&ts,NULL);
#endif
    assert(zq_send_str(&pub,"hello",0)==0);
    char buf[64]; size_t got=0;
    /* poll jusqu’à 1s */
    for (int i=0;i<20;i++){
        int ev=0; zq_poll(&sub,1,50,&ev);
        if (ev&1){ break; }
#if defined(_WIN32)
        Sleep(10);
#else
        struct timespec t2={0,10*1000000L}; nanosleep(&t2,NULL);
#endif
    }
    assert(zq_recv(&sub,buf,sizeof buf,&got,0)==0);
    buf[got<sizeof buf?got:sizeof buf-1]=0;
    printf("recv=%s\n", buf);
    zq_close(&pub); zq_close(&sub); zq_term(&c);
    return 0;
}
#endif