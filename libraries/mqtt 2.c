// SPDX-License-Identifier: GPL-3.0-or-later
//
// mqtt.c — Client MQTT 3.1.1 minimal (C17, sans dépendances)
// Namespace: "mqtt"
//
// Fonctionnalités:
//   - TCP direct (pas de TLS).
//   - CONNECT / PUBLISH QoS 0 / SUBSCRIBE QoS 0 / PING / DISCONNECT.
//   - Keepalive automatique via mqtt_loop().
//   - Callbacks simples pour messages SUBSCRIBE.
//
// Plateformes: Linux/macOS (POSIX), Windows (Winsock2).
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c mqtt.c
//
// Démo (MQTT_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DMQTT_TEST mqtt.c && \
//   ./a.out 127.0.0.1 1883 client-1 topic "hello"
//
// Limites: pas de QoS1/2, pas de TLS, pas de propriétés v5.
//
// ----------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib,"Ws2_32.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK closesocket
  #define socklen_cast int
  static int _net_init(void){ WSADATA w; return WSAStartup(MAKEWORD(2,2), &w); }
  static void _net_shutdown(void){ WSACleanup(); }
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
  static int _net_init(void){ return 0; }
  static void _net_shutdown(void){}
#endif

#ifndef MQTT_API
#define MQTT_API
#endif

// ======================== Utils ========================

static uint64_t _now_ms(void){
#if defined(_WIN32)
    static LARGE_INTEGER f={0}; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart*1000ULL)/ (uint64_t)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000ULL + (uint64_t)(ts.tv_nsec/1000000ULL);
#endif
}
static int _io_write_all(sock_t s, const void* p, size_t n){
    const unsigned char* b=(const unsigned char*)p;
    while(n){
#if defined(_WIN32)
        int k=send(s,(const char*)b,(int)n,0);
#else
        ssize_t k=send(s,b,n,0);
#endif
        if (k<=0) return -1; b+=k; n-=k;
    }
    return 0;
}
static int _io_read_all(sock_t s, void* p, size_t n){
    unsigned char* b=(unsigned char*)p;
    while(n){
#if defined(_WIN32)
        int k=recv(s,(char*)b,(int)n,0);
#else
        ssize_t k=recv(s,b,n,0);
#endif
        if (k<=0) return -1; b+=k; n-=k;
    }
    return 0;
}
static sock_t _tcp_connect(const char* host, const char* port, int timeout_ms){
    if (_net_init()!=0) return INVALID_SOCKET;
    struct addrinfo hints; memset(&hints,0,sizeof hints);
    hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    struct addrinfo* res=NULL;
    if (getaddrinfo(host,port,&hints,&res)!=0) return INVALID_SOCKET;
    sock_t s=INVALID_SOCKET;
    for (struct addrinfo* it=res; it; it=it->ai_next){
        s=(sock_t)socket(it->ai_family,it->ai_socktype,it->ai_protocol);
        if (s==INVALID_SOCKET) continue;
#if defined(_WIN32)
        if (timeout_ms>0){
            DWORD tv=(DWORD)timeout_ms;
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&tv,sizeof tv);
            setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&tv,sizeof tv);
        }
#else
        if (timeout_ms>0){
            struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
            setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
        }
#endif
        if (connect(s,it->ai_addr,(socklen_cast)it->ai_addrlen)==0) break;
        CLOSESOCK(s); s=INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

// ======================== MQTT core ========================

typedef struct {
    sock_t s;
    uint16_t keepalive_s;
    uint64_t last_tx_ms;
    uint64_t last_rx_ms;
    int connected;
    void (*on_msg)(const char* topic, const unsigned char* payload, uint32_t len, void* u);
    void* user;
} mqtt_client;

#define MQTT_PKT_CONNECT     0x10
#define MQTT_PKT_CONNACK     0x20
#define MQTT_PKT_PUBLISH     0x30
#define MQTT_PKT_PUBACK      0x40
#define MQTT_PKT_SUBSCRIBE   0x80
#define MQTT_PKT_SUBACK      0x90
#define MQTT_PKT_PINGREQ     0xC0
#define MQTT_PKT_PINGRESP    0xD0
#define MQTT_PKT_DISCONNECT  0xE0

static int _enc_rl(uint32_t x, unsigned char* out, size_t* n){
    size_t i=0; do{ unsigned char d=(unsigned char)(x%128); x/=128; if (x) d|=0x80; out[i++]=d; }while(x && i<4);
    *n=i; return (x==0)?0:-1;
}
static int _dec_rl(mqtt_client* c, uint32_t* out, size_t* rl_len){
    unsigned char b; int m=1; uint32_t v=0; size_t i=0;
    do{
        if (_io_read_all(c->s,&b,1)!=0) return -1;
        v += (uint32_t)(b & 0x7F) * (uint32_t)m;
        m *= 128; i++;
        if (i>4) return -1;
    }while(b & 0x80);
    *out=v; if(rl_len)*rl_len=i; c->last_rx_ms=_now_ms(); return 0;
}
static int _w_u16(unsigned char* p, uint16_t v){ p[0]=(unsigned char)(v>>8); p[1]=(unsigned char)v; return 2; }

static int _send_connect(mqtt_client* c, const char* client_id, const char* user, const char* pass, int clean_start){
    unsigned char var[12]; size_t vi=0;
    // Protocol Name "MQTT"
    var[vi++]=0; var[vi++]=4; var[vi++]='M'; var[vi++]='Q'; var[vi++]='T'; var[vi++]='T';
    var[vi++]=4; // Level 4 (3.1.1)
    unsigned char flags=0;
    if (clean_start) flags|=0x02;
    if (user && *user) flags|=0x80;
    if (pass && *pass) flags|=0x40;
    var[vi++]=flags;
    var[vi++]=(unsigned char)(c->keepalive_s>>8);
    var[vi++]=(unsigned char)(c->keepalive_s&0xFF);

    // Payload: Client ID, [User], [Pass]
    size_t cid_len=strlen(client_id?client_id:"");
    size_t usr_len=user?strlen(user):0;
    size_t pwd_len=pass?strlen(pass):0;
    size_t pay_len=2+cid_len + (user?2+usr_len:0) + (pass?2+pwd_len:0);

    unsigned char hdr[5]; size_t rl_len=vi + pay_len;
    size_t encn; if (_enc_rl((uint32_t)rl_len, hdr+1, &encn)!=0) return -1;
    hdr[0]=MQTT_PKT_CONNECT;
    if (_io_write_all(c->s,hdr,1+encn)!=0) return -1;
    if (_io_write_all(c->s,var,vi)!=0) return -1;

    unsigned char tmp[2];
    _w_u16(tmp,(uint16_t)cid_len);
    if (_io_write_all(c->s,tmp,2)!=0 || (cid_len && _io_write_all(c->s,client_id,cid_len)!=0)) return -1;
    if (user){
        _w_u16(tmp,(uint16_t)usr_len);
        if (_io_write_all(c->s,tmp,2)!=0 || (usr_len && _io_write_all(c->s,user,usr_len)!=0)) return -1;
    }
    if (pass){
        _w_u16(tmp,(uint16_t)pwd_len);
        if (_io_write_all(c->s,tmp,2)!=0 || (pwd_len && _io_write_all(c->s,pass,pwd_len)!=0)) return -1;
    }
    c->last_tx_ms=_now_ms();
    return 0;
}
static int _recv_connack(mqtt_client* c){
    unsigned char b;
    if (_io_read_all(c->s,&b,1)!=0 || b!=MQTT_PKT_CONNACK) return -1;
    uint32_t rl; if (_dec_rl(c,&rl,NULL)!=0 || rl<2) return -1;
    unsigned char body[4]; if (_io_read_all(c->s,body,rl)!=0) return -1;
    c->last_rx_ms=_now_ms();
    return (body[1]==0)?0:-1; // return code 0 => success
}

static int _send_ping(mqtt_client* c){
    unsigned char buf[2]={MQTT_PKT_PINGREQ,0};
    if (_io_write_all(c->s,buf,2)!=0) return -1;
    c->last_tx_ms=_now_ms(); return 0;
}
static int _send_disconnect(mqtt_client* c){
    unsigned char buf[2]={MQTT_PKT_DISCONNECT,0};
    if (_io_write_all(c->s,buf,2)!=0) return -1;
    c->last_tx_ms=_now_ms(); return 0;
}

static int _send_publish_qos0(mqtt_client* c, const char* topic, const void* payload, uint32_t len, int retain){
    size_t tlen=strlen(topic?topic:"");
    unsigned char fh = MQTT_PKT_PUBLISH | (retain?0x01:0);
    unsigned char hdr[5]; size_t encn;
    uint32_t rl = 2 + (uint32_t)tlen + len; // no packet id (QoS0)
    if (_enc_rl(rl, hdr+1, &encn)!=0) return -1;
    hdr[0]=fh;
    if (_io_write_all(c->s,hdr,1+encn)!=0) return -1;
    unsigned char tmp2[2]; _w_u16(tmp2,(uint16_t)tlen);
    if (_io_write_all(c->s,tmp2,2)!=0) return -1;
    if (tlen && _io_write_all(c->s,topic,tlen)!=0) return -1;
    if (len && _io_write_all(c->s,payload,len)!=0) return -1;
    c->last_tx_ms=_now_ms(); return 0;
}

static int _send_subscribe_qos0(mqtt_client* c, const char* topic){
    static uint16_t pid=1; uint16_t id=pid++; // wrap ok
    size_t tlen=strlen(topic?topic:"");
    uint32_t rl = 2 /*pkt id*/ + 2 + (uint32_t)tlen + 1 /*QoS*/;
    unsigned char hdr[5]; size_t encn; if (_enc_rl(rl,hdr+1,&encn)!=0) return -1;
    hdr[0]=MQTT_PKT_SUBSCRIBE | 0x02; // QoS1 for SUBSCRIBE control packet (spec)
    if (_io_write_all(c->s,hdr,1+encn)!=0) return -1;
    unsigned char tmp[2]; _w_u16(tmp,id);
    if (_io_write_all(c->s,tmp,2)!=0) return -1;
    _w_u16(tmp,(uint16_t)tlen);
    if (_io_write_all(c->s,tmp,2)!=0) return -1;
    if (tlen && _io_write_all(c->s,topic,tlen)!=0) return -1;
    unsigned char req_qos=0; if (_io_write_all(c->s,&req_qos,1)!=0) return -1;
    c->last_tx_ms=_now_ms(); return 0;
}

static int _handle_publish(mqtt_client* c, unsigned char fh, uint32_t rl){
    int qos = (fh>>1)&0x03;
    unsigned char tmp[2];
    if (_io_read_all(c->s,tmp,2)!=0) return -1;
    uint16_t tlen=((uint16_t)tmp[0]<<8)|tmp[1];
    char* topic=(char*)malloc((size_t)tlen+1); if(!topic) return -1;
    if (_io_read_all(c->s,topic,tlen)!=0){ free(topic); return -1; }
    topic[tlen]=0;
    rl -= 2 + tlen;
    if (qos){ // QoS1/2 non géré
        // Consommer packet id et payload quand même puis abandonner
        if (_io_read_all(c->s,tmp,2)!=0){ free(topic); return -1; }
        rl -= 2;
    }
    unsigned char* payload=NULL;
    if (rl){
        payload=(unsigned char*)malloc(rl);
        if(!payload){ free(topic); return -1; }
        if (_io_read_all(c->s,payload,rl)!=0){ free(topic); free(payload); return -1; }
    }
    c->last_rx_ms=_now_ms();
    if (c->on_msg) c->on_msg(topic,payload,rl,c->user);
    free(topic); free(payload);
    return 0;
}

// ======================== API ========================

MQTT_API int mqtt_connect(mqtt_client* c,
                          const char* host, const char* port,
                          const char* client_id,
                          const char* user, const char* pass,
                          uint16_t keepalive_sec,
                          void (*on_msg)(const char*, const unsigned char*, uint32_t, void*),
                          void* user_ptr){
    memset(c,0,sizeof *c);
    c->s = _tcp_connect(host,port,5000);
    if (c->s==INVALID_SOCKET) return -1;
    c->keepalive_s = keepalive_sec? keepalive_sec : 60;
    c->on_msg = on_msg; c->user=user_ptr;
    if (_send_connect(c, client_id?client_id:"client", user, pass, 1)!=0){ CLOSESOCK(c->s); _net_shutdown(); return -1; }
    if (_recv_connack(c)!=0){ CLOSESOCK(c->s); _net_shutdown(); return -1; }
    c->connected=1; c->last_rx_ms=_now_ms(); return 0;
}

MQTT_API int mqtt_publish_qos0(mqtt_client* c, const char* topic, const void* payload, uint32_t len, int retain){
    if (!c||!c->connected) return -1;
    return _send_publish_qos0(c, topic, payload, len, retain);
}

MQTT_API int mqtt_subscribe_qos0(mqtt_client* c, const char* topic){
    if (!c||!c->connected) return -1;
    return _send_subscribe_qos0(c, topic);
}

MQTT_API int mqtt_loop(mqtt_client* c, int timeout_ms){
    if (!c||!c->connected) return -1;
    // Ping si inactivité TX > keepalive*500 ms
    uint64_t now=_now_ms();
    if (c->keepalive_s && now - c->last_tx_ms > (uint64_t)c->keepalive_s*500ULL){
        if (_send_ping(c)!=0) return -1;
    }
    // Attendre un octet du header (avec timeouts côté socket)
    unsigned char fh;
#if defined(_WIN32)
    int k = recv(c->s,(char*)&fh,1,0);
#else
    ssize_t k = recv(c->s,&fh,1,0);
#endif
    if (k==0) return -1; // fermé
    if (k<0){
#if defined(_WIN32)
        int err = WSAGetLastError();
        (void)err;
#else
        if (errno==EWOULDBLOCK || errno==EAGAIN){
            if (timeout_ms>0){
#if defined(_WIN32)
                Sleep((DWORD)timeout_ms);
#else
                struct timespec ts; ts.tv_sec=timeout_ms/1000; ts.tv_nsec=(timeout_ms%1000)*1000000L; nanosleep(&ts,NULL);
#endif
            }
            return 0;
        }
#endif
        return -1;
    }
    // Sinon, on a reçu un paquet
    uint32_t rl; if (_dec_rl(c,&rl,NULL)!=0) return -1;
    switch (fh & 0xF0){
        case MQTT_PKT_PUBLISH: return _handle_publish(c, fh, rl);
        case MQTT_PKT_PINGRESP: {
            // Consommer RL==0
            if (rl){ unsigned char junk[4]; if (_io_read_all(c->s,junk,rl)!=0) return -1; }
            c->last_rx_ms=_now_ms(); return 0;
        }
        case MQTT_PKT_SUBACK: {
            unsigned char buf[4];
            if (rl>sizeof buf) return -1;
            if (_io_read_all(c->s,buf,rl)!=0) return -1;
            c->last_rx_ms=_now_ms(); return 0;
        }
        default: {
            // Consommer et ignorer
            size_t left=rl; unsigned char tmp[256];
            while(left){
                size_t chunk = left>sizeof tmp? sizeof tmp : left;
                if (_io_read_all(c->s,tmp,chunk)!=0) return -1;
                left -= chunk;
            }
            c->last_rx_ms=_now_ms();
            return 0;
        }
    }
}

MQTT_API int mqtt_disconnect(mqtt_client* c){
    if (!c) return -1;
    if (c->connected) _send_disconnect(c);
    if (c->s!=INVALID_SOCKET) CLOSESOCK(c->s);
    _net_shutdown();
    c->connected=0; c->s=INVALID_SOCKET; return 0;
}

// ======================== Test ========================
#ifdef MQTT_TEST
static void on_msg(const char* topic, const unsigned char* payload, uint32_t len, void* u){
    (void)u;
    printf("msg topic=%s len=%u: ", topic, len);
    for (uint32_t i=0;i<len;i++) putchar(payload[i]);
    putchar('\n');
}
int main(int argc, char** argv){
    if (argc<6){
        fprintf(stderr,"usage: %s host port client_id topic [message]\n", argv[0]);
        return 2;
    }
    const char* host=argv[1], *port=argv[2], *cid=argv[3], *topic=argv[4];
    const char* msg = (argc>=6)? argv[5] : NULL;

    mqtt_client c;
    if (mqtt_connect(&c, host, port, cid, NULL, NULL, 30, on_msg, NULL)!=0){
        fprintf(stderr,"connect fail\n"); return 1;
    }
    if (msg){
        if (mqtt_publish_qos0(&c, topic, msg, (uint32_t)strlen(msg), 0)!=0)
            fprintf(stderr,"publish fail\n");
    } else {
        mqtt_subscribe_qos0(&c, topic);
        uint64_t end=_now_ms()+10000; // écouter 10s
        while (_now_ms()<end){
            mqtt_loop(&c, 100);
        }
    }
    mqtt_disconnect(&c);
    puts("ok"); return 0;
}
#endif