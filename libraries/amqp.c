// SPDX-License-Identifier: GPL-3.0-or-later
//
// amqp.c — Client AMQP 0-9-1 minimal pour Vitte Light (C17, sans dépendances externes)
// Namespace: "amqp"
//
// Objectif : se connecter à un broker (ex: RabbitMQ), ouvrir un canal, (option) déclarer une queue,
// publier un message (basic.publish) et fermer proprement.
//
// Support : Linux/macOS (POSIX sockets) et Windows (Winsock2)
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c amqp.c
//
// Démo (voir AMQP_TEST en bas):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DAMQP_TEST amqp.c && \
//     ./a.out 127.0.0.1 5672 guest guest / qname "hello world"
//
// Limites :
//   - Pas de TLS, pas de SASL avancé (PLAIN uniquement).
//   - Pas de consumer. Publication simple (confirmations non gérées).
//   - frame_max simple (on ne segmente pas le corps si > frame_max).
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK closesocket
  #define socklen_cast int
  static int net_init(void){ WSADATA w; return WSAStartup(MAKEWORD(2,2), &w); }
  static void net_shutdown(void){ WSACleanup(); }
#else
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  typedef int sock_t;
  #define INVALID_SOCKET (-1)
  #define CLOSESOCK close
  #define socklen_cast socklen_t
  static int net_init(void){ return 0; }
  static void net_shutdown(void){}
#endif

#ifndef AMQP_API
#define AMQP_API
#endif

/* ============================== Helpers ============================== */

static int net_connect(const char* host, const char* port, int timeout_ms){
    if (net_init()!=0) return INVALID_SOCKET;
    struct addrinfo hints; memset(&hints,0,sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, port, &hints, &res)!=0) return INVALID_SOCKET;
    sock_t s = INVALID_SOCKET;
    for (struct addrinfo* p=res; p; p=p->ai_next){
        s = (sock_t)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s==INVALID_SOCKET) continue;
#if !defined(_WIN32)
        if (timeout_ms>0){
            struct timeval tv; tv.tv_sec = timeout_ms/1000; tv.tv_usec = (timeout_ms%1000)*1000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }
#else
        if (timeout_ms>0){
            DWORD tv = (DWORD)timeout_ms;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
        }
#endif
        if (connect(s, p->ai_addr, (socklen_cast)p->ai_addrlen)==0) break;
        CLOSESOCK(s); s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}
static int io_write_all(sock_t s, const void* buf, size_t n){
    const unsigned char* p=(const unsigned char*)buf;
    while (n){
#if defined(_WIN32)
        int k = send(s, (const char*)p, (int)n, 0);
#else
        ssize_t k = send(s, p, n, 0);
#endif
        if (k<=0) return -1;
        p+=k; n-=k;
    }
    return 0;
}
static int io_read_all(sock_t s, void* buf, size_t n){
    unsigned char* p=(unsigned char*)buf;
    while (n){
#if defined(_WIN32)
        int k = recv(s, (char*)p, (int)n, 0);
#else
        ssize_t k = recv(s, p, n, 0);
#endif
        if (k<=0) return -1;
        p+=k; n-=k;
    }
    return 0;
}

/* ============================== AMQP basics ============================== */

#define AMQP_FRAME_METHOD   1u
#define AMQP_FRAME_HEADER   2u
#define AMQP_FRAME_BODY     3u
#define AMQP_FRAME_HEARTBEAT 8u
#define AMQP_FRAME_END      0xCEu

/* Classes/Methods */
#define CLASS_CONNECTION 10u
#define METHOD_CONNECTION_START      10u
#define METHOD_CONNECTION_START_OK   11u
#define METHOD_CONNECTION_TUNE       30u
#define METHOD_CONNECTION_TUNE_OK    31u
#define METHOD_CONNECTION_OPEN       40u
#define METHOD_CONNECTION_OPEN_OK    41u
#define METHOD_CONNECTION_CLOSE      50u
#define METHOD_CONNECTION_CLOSE_OK   51u

#define CLASS_CHANNEL    20u
#define METHOD_CHANNEL_OPEN    10u
#define METHOD_CHANNEL_OPEN_OK 11u
#define METHOD_CHANNEL_CLOSE   40u
#define METHOD_CHANNEL_CLOSE_OK 41u

#define CLASS_EXCHANGE   40u
#define CLASS_QUEUE      50u
#define METHOD_QUEUE_DECLARE    10u
#define METHOD_QUEUE_DECLARE_OK 11u

#define CLASS_BASIC      60u
#define METHOD_BASIC_PUBLISH    40u

/* Writer buffer */
typedef struct {
    unsigned char* p;
    size_t cap, len;
} bufw;

static void bw_init(bufw* b, unsigned char* mem, size_t cap){ b->p=mem; b->cap=cap; b->len=0; }
static int  bw_put(bufw* b, const void* src, size_t n){
    if (b->len+n > b->cap) return -1;
    memcpy(b->p+b->len, src, n); b->len += n; return 0;
}
static int  bw_u8(bufw* b, uint8_t v){ return bw_put(b,&v,1); }
static int  bw_u16(bufw* b, uint16_t v){ unsigned char x[2]={ (unsigned char)(v>>8), (unsigned char)(v&0xFF)}; return bw_put(b,x,2); }
static int  bw_u32(bufw* b, uint32_t v){ unsigned char x[4]={ (unsigned char)(v>>24),(unsigned char)(v>>16),(unsigned char)(v>>8),(unsigned char)v}; return bw_put(b,x,4); }
static int  bw_u64(bufw* b, uint64_t v){
    unsigned char x[8]={ (unsigned char)(v>>56),(unsigned char)(v>>48),(unsigned char)(v>>40),(unsigned char)(v>>32),
                         (unsigned char)(v>>24),(unsigned char)(v>>16),(unsigned char)(v>>8),(unsigned char)v};
    return bw_put(b,x,8);
}
static int  bw_shortstr(bufw* b, const char* s){
    size_t n = s?strlen(s):0; if (n>255) n=255;
    if (bw_u8(b,(uint8_t)n)!=0) return -1;
    return bw_put(b, s, n);
}
static int  bw_longstr(bufw* b, const void* s, uint32_t n){
    if (bw_u32(b, n)!=0) return -1;
    return bw_put(b, s, n);
}
/* Table vide uniquement (longueur 0) */
static int  bw_table_empty(bufw* b){ return bw_u32(b, 0); }

/* Frame writer */
static int amqp_send_frame(sock_t s, uint8_t type, uint16_t channel, const void* payload, uint32_t size){
    unsigned char hdr[7];
    hdr[0]=type; hdr[1]=(unsigned char)(channel>>8); hdr[2]=(unsigned char)channel;
    hdr[3]=(unsigned char)(size>>24); hdr[4]=(unsigned char)(size>>16); hdr[5]=(unsigned char)(size>>8); hdr[6]=(unsigned char)size;
    if (io_write_all(s, hdr, 7)!=0) return -1;
    if (size && io_write_all(s, payload, size)!=0) return -1;
    unsigned char end = AMQP_FRAME_END;
    return io_write_all(s, &end, 1);
}

/* Frame reader de base (renvoie type, chan, taille; stocke dans buf alloué par l'appelant) */
static int amqp_read_frame(sock_t s, uint8_t* type, uint16_t* channel, unsigned char* payload, uint32_t* size){
    unsigned char hdr[7];
    if (io_read_all(s, hdr, 7)!=0) return -1;
    *type = hdr[0];
    *channel = ((uint16_t)hdr[1]<<8) | hdr[2];
    uint32_t sz = ((uint32_t)hdr[3]<<24)|((uint32_t)hdr[4]<<16)|((uint32_t)hdr[5]<<8)|hdr[6];
    *size = sz;
    if (sz){
        if (io_read_all(s, payload, sz)!=0) return -1;
    }
    unsigned char end=0;
    if (io_read_all(s, &end, 1)!=0 || end!=AMQP_FRAME_END) return -1;
    return 0;
}

/* ============================== Handshake ============================== */

typedef struct {
    sock_t s;
    uint16_t channel_max;
    uint32_t frame_max;
    uint16_t heartbeat;
} amqp_conn;

AMQP_API int amqp_connect_plain(amqp_conn* c,
                                const char* host, const char* port,
                                const char* user, const char* pass,
                                const char* vhost,
                                int timeout_ms){
    memset(c, 0, sizeof *c);
    c->s = net_connect(host, port, timeout_ms);
    if (c->s==INVALID_SOCKET) return -1;

    /* protocol header */
    const unsigned char ph[8] = {'A','M','Q','P',0,0,9,1};
    if (io_write_all(c->s, ph, 8)!=0) { CLOSESOCK(c->s); return -1; }

    unsigned char buf[4096]; uint8_t type; uint16_t ch; uint32_t sz;

    /* expect connection.start (class 10, method 10) */
    if (amqp_read_frame(c->s,&type,&ch,buf,&sz)!=0 || type!=AMQP_FRAME_METHOD) { CLOSESOCK(c->s); return -1; }
    uint16_t cls = ((uint16_t)buf[0]<<8)|buf[1];
    uint16_t mth = ((uint16_t)buf[2]<<8)|buf[3];
    if (cls!=CLASS_CONNECTION || mth!=METHOD_CONNECTION_START){ CLOSESOCK(c->s); return -1; }
    /* skip server props + mechanisms + locales (nous répondons PLAIN/en_US) */

    /* send connection.start-ok */
    unsigned char payload[1024]; bufw w; bw_init(&w,payload,sizeof payload);
    bw_u16(&w, CLASS_CONNECTION); bw_u16(&w, METHOD_CONNECTION_START_OK);
    bw_table_empty(&w);                    /* client-properties */
    bw_shortstr(&w, "PLAIN");              /* mechanism */
    /* response: 0 user 0 pass (longstr) */
    {
        size_t ulen = user?strlen(user):0, plen = pass?strlen(pass):0;
        size_t rlen = 1 + ulen + 1 + plen;
        unsigned char* tmp = (unsigned char*)malloc(rlen);
        tmp[0]=0; memcpy(tmp+1, user, ulen); tmp[1+ulen]=0; memcpy(tmp+2+ulen, pass, plen);
        bw_longstr(&w, tmp, (uint32_t)rlen);
        free(tmp);
    }
    bw_shortstr(&w, "en_US");              /* locale */
    if (amqp_send_frame(c->s, AMQP_FRAME_METHOD, 0, payload, (uint32_t)w.len)!=0){ CLOSESOCK(c->s); return -1; }

    /* expect connection.tune */
    if (amqp_read_frame(c->s,&type,&ch,buf,&sz)!=0 || type!=AMQP_FRAME_METHOD) { CLOSESOCK(c->s); return -1; }
    cls = ((uint16_t)buf[0]<<8)|buf[1];
    mth = ((uint16_t)buf[2]<<8)|buf[3];
    if (cls!=CLASS_CONNECTION || mth!=METHOD_CONNECTION_TUNE){ CLOSESOCK(c->s); return -1; }
    c->channel_max = ((uint16_t)buf[4]<<8)|buf[5];
    c->frame_max   = ((uint32_t)buf[6]<<24)|((uint32_t)buf[7]<<16)|((uint32_t)buf[8]<<8)|buf[9];
    c->heartbeat   = ((uint16_t)buf[10]<<8)|buf[11];
    if (c->channel_max==0) c->channel_max=2047;
    if (c->frame_max==0) c->frame_max=131072;
    /* send tune-ok with our choices (on accepte ce que le serveur dit) */
    bw_init(&w,payload,sizeof payload);
    bw_u16(&w, CLASS_CONNECTION); bw_u16(&w, METHOD_CONNECTION_TUNE_OK);
    bw_u16(&w, c->channel_max);
    bw_u32(&w, c->frame_max);
    bw_u16(&w, c->heartbeat);
    if (amqp_send_frame(c->s, AMQP_FRAME_METHOD, 0, payload, (uint32_t)w.len)!=0){ CLOSESOCK(c->s); return -1; }

    /* connection.open vhost */
    bw_init(&w,payload,sizeof payload);
    bw_u16(&w, CLASS_CONNECTION); bw_u16(&w, METHOD_CONNECTION_OPEN);
    bw_shortstr(&w, vhost && *vhost ? vhost : "/");
    bw_shortstr(&w, "");         /* reserved-1: capabilities */
    bw_u8(&w, 0);                /* insist=false */
    if (amqp_send_frame(c->s, AMQP_FRAME_METHOD, 0, payload, (uint32_t)w.len)!=0){ CLOSESOCK(c->s); return -1; }

    /* expect open-ok */
    if (amqp_read_frame(c->s,&type,&ch,buf,&sz)!=0 || type!=AMQP_FRAME_METHOD){ CLOSESOCK(c->s); return -1; }
    cls = ((uint16_t)buf[0]<<8)|buf[1];
    mth = ((uint16_t)buf[2]<<8)|buf[3];
    if (cls!=CLASS_CONNECTION || mth!=METHOD_CONNECTION_OPEN_OK){ CLOSESOCK(c->s); return -1; }

    return 0;
}

AMQP_API int amqp_channel_open(amqp_conn* c, uint16_t channel){
    unsigned char payload[256]; bufw w; bw_init(&w,payload,sizeof payload);
    bw_u16(&w, CLASS_CHANNEL); bw_u16(&w, METHOD_CHANNEL_OPEN);
    bw_shortstr(&w, ""); /* out-of-band */
    if (amqp_send_frame(c->s, AMQP_FRAME_METHOD, channel, payload, (uint32_t)w.len)!=0) return -1;

    uint8_t type; uint16_t ch; uint32_t sz; unsigned char buf[1024];
    if (amqp_read_frame(c->s,&type,&ch,buf,&sz)!=0 || type!=AMQP_FRAME_METHOD || ch!=channel) return -1;
    uint16_t cls = ((uint16_t)buf[0]<<8)|buf[1];
    uint16_t mth = ((uint16_t)buf[2]<<8)|buf[3];
    return (cls==CLASS_CHANNEL && mth==METHOD_CHANNEL_OPEN_OK) ? 0 : -1;
}

/* queue.declare (passive=0,durable=1,exclusive=0,auto_delete=0,no_wait=0,args={}) */
AMQP_API int amqp_queue_declare(amqp_conn* c, uint16_t channel, const char* qname, int durable){
    unsigned char payload[512]; bufw w; bw_init(&w,payload,sizeof payload);
    bw_u16(&w, CLASS_QUEUE); bw_u16(&w, METHOD_QUEUE_DECLARE);
    bw_u16(&w, 0);                 /* reserved-1 */
    bw_shortstr(&w, qname?qname:"");
    uint8_t bits = 0;
    /* flags: bit positions are separate booleans in spec, we serialize as individual bits in 5 octets */
    /* passive, durable, exclusive, auto-delete, no-wait */
    bw_u8(&w, 0); /* passive=0 */
    bw_u8(&w, durable?1:0);
    bw_u8(&w, 0); /* exclusive=0 */
    bw_u8(&w, 0); /* auto-delete=0 */
    bw_u8(&w, 0); /* no-wait=0 */
    (void)bits;
    bw_table_empty(&w);            /* arguments */
    if (amqp_send_frame(c->s, AMQP_FRAME_METHOD, channel, payload, (uint32_t)w.len)!=0) return -1;

    uint8_t type; uint16_t ch; uint32_t sz; unsigned char buf[1024];
    if (amqp_read_frame(c->s,&type,&ch,buf,&sz)!=0 || type!=AMQP_FRAME_METHOD || ch!=channel) return -1;
    uint16_t cls = ((uint16_t)buf[0]<<8)|buf[1];
    uint16_t mth = ((uint16_t)buf[2]<<8)|buf[3];
    return (cls==CLASS_QUEUE && mth==METHOD_QUEUE_DECLARE_OK) ? 0 : -1;
}

/* basic.publish vers exchange "" (default), routing_key=qname */
AMQP_API int amqp_basic_publish(amqp_conn* c, uint16_t channel,
                                const char* exchange, const char* routing_key,
                                const void* body, size_t body_len){
    if (!body) body_len=0;
    if (c->frame_max && body_len + 8 > c->frame_max){ /* pas de segmentation ici */
        return -2;
    }
    unsigned char payload[512]; bufw w; bw_init(&w,payload,sizeof payload);
    /* METHOD */
    bw_u16(&w, CLASS_BASIC); bw_u16(&w, METHOD_BASIC_PUBLISH);
    bw_u16(&w, 0); /* reserved-1 */
    bw_shortstr(&w, exchange?exchange:"");
    bw_shortstr(&w, routing_key?routing_key:"");
    bw_u8(&w, 0);  /* mandatory=0, immediate=0 */
    if (amqp_send_frame(c->s, AMQP_FRAME_METHOD, channel, payload, (uint32_t)w.len)!=0) return -1;

    /* HEADER: content header (class-id, weight, body-size, property-flags=0) */
    unsigned char hdr[14]; bufw h; bw_init(&h,hdr,sizeof hdr);
    bw_u16(&h, CLASS_BASIC);
    bw_u16(&h, 0);            /* weight */
    bw_u64(&h, (uint64_t)body_len);
    bw_u16(&h, 0);            /* property flags = 0 (no properties) */
    if (amqp_send_frame(c->s, AMQP_FRAME_HEADER, channel, hdr, (uint32_t)h.len)!=0) return -1;

    /* BODY (unique frame) */
    if (body_len){
        if (amqp_send_frame(c->s, AMQP_FRAME_BODY, channel, body, (uint32_t)body_len)!=0) return -1;
    }
    return 0;
}

AMQP_API int amqp_channel_close(amqp_conn* c, uint16_t channel){
    unsigned char payload[64]; bufw w; bw_init(&w,payload,sizeof payload);
    bw_u16(&w, CLASS_CHANNEL); bw_u16(&w, METHOD_CHANNEL_CLOSE);
    bw_u16(&w, 0); /* reply-code */
    bw_shortstr(&w, "");
    bw_u16(&w, 0); /* class-id */
    bw_u16(&w, 0); /* method-id */
    if (amqp_send_frame(c->s, AMQP_FRAME_METHOD, channel, payload, (uint32_t)w.len)!=0) return -1;

    /* expect close-ok */
    uint8_t type; uint16_t ch; uint32_t sz; unsigned char buf[256];
    if (amqp_read_frame(c->s,&type,&ch,buf,&sz)!=0 || type!=AMQP_FRAME_METHOD || ch!=channel) return -1;
    uint16_t cls = ((uint16_t)buf[0]<<8)|buf[1];
    uint16_t mth = ((uint16_t)buf[2]<<8)|buf[3];
    return (cls==CLASS_CHANNEL && mth==METHOD_CHANNEL_CLOSE_OK)?0:-1;
}

AMQP_API int amqp_connection_close(amqp_conn* c){
    unsigned char payload[64]; bufw w; bw_init(&w,payload,sizeof payload);
    bw_u16(&w, CLASS_CONNECTION); bw_u16(&w, METHOD_CONNECTION_CLOSE);
    bw_u16(&w, 0); /* reply-code */
    bw_shortstr(&w, "");
    bw_u16(&w, 0); /* class-id */
    bw_u16(&w, 0); /* method-id */
    if (amqp_send_frame(c->s, AMQP_FRAME_METHOD, 0, payload, (uint32_t)w.len)!=0) return -1;

    /* expect close-ok */
    uint8_t type; uint16_t ch; uint32_t sz; unsigned char buf[256];
    if (amqp_read_frame(c->s,&type,&ch,buf,&sz)!=0 || type!=AMQP_FRAME_METHOD || ch!=0) return -1;
    uint16_t cls = ((uint16_t)buf[0]<<8)|buf[1];
    uint16_t mth = ((uint16_t)buf[2]<<8)|buf[3];
    int ok = (cls==CLASS_CONNECTION && mth==METHOD_CONNECTION_CLOSE_OK)?0:-1;
    CLOSESOCK(c->s); net_shutdown();
    return ok;
}

/* Helper de haut niveau: connect + open ch + (optionnel) declare + publish + close */
AMQP_API int amqp_simple_publish(const char* host, const char* port,
                                 const char* user, const char* pass,
                                 const char* vhost,
                                 const char* queue_name,
                                 const void* body, size_t body_len,
                                 int declare_queue, int durable){
    amqp_conn c;
    if (amqp_connect_plain(&c, host, port, user, pass, vhost, 5000)!=0) return -1;
    if (amqp_channel_open(&c, 1)!=0){ amqp_connection_close(&c); return -1; }
    if (declare_queue){
        if (amqp_queue_declare(&c, 1, queue_name, durable)!=0){
            amqp_channel_close(&c,1); amqp_connection_close(&c); return -1;
        }
    }
    if (amqp_basic_publish(&c, 1, "", queue_name, body, body_len)!=0){
        amqp_channel_close(&c,1); amqp_connection_close(&c); return -1;
    }
    amqp_channel_close(&c,1);
    amqp_connection_close(&c);
    return 0;
}

/* ============================== Test ============================== */
#ifdef AMQP_TEST
int main(int argc, char** argv){
    if (argc < 8){
        fprintf(stderr,"usage: %s host port user pass vhost queue \"message\"\n", argv[0]);
        return 2;
    }
    const char* host=argv[1], *port=argv[2], *user=argv[3], *pass=argv[4], *vhost=argv[5], *q=argv[6], *msg=argv[7];
    int rc = amqp_simple_publish(host, port, user, pass, vhost, q, msg, (size_t)strlen(msg), 1, 1);
    if (rc!=0){ fprintf(stderr,"publish failed (%d)\n", rc); return 1; }
    puts("ok");
    return 0;
}
#endif