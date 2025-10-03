// SPDX-License-Identifier: GPL-3.0-or-later
//
// redis.c — Client Redis RESP2 minimal et portable (C17, sans dépendances)
// Namespace: "rds"
//
// Fonctionnalités:
//   - Connexion TCP (IPv4/IPv6), sans TLS.
//   - Envoi de commandes variadiques type redis-cli (argv/argc).
//   - Parse des réponses RESP2: +simple, -error, :integer, $bulk, *array.
//   - Helper redis_cmdf(format, ...) et redis_cmd(argc, argv).
//   - Helpers: PING, GET, SETEX.
//   - Pub/Sub: lecture de messages via redis_read_reply (type ARRAY ["message", channel, payload]).
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c redis.c
//
// Test (REDIS_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DREDIS_TEST redis.c && ./a.out 127.0.0.1 6379
//
// Notes:
//   - Encodage binaire sûr (length-prefix). Pas de limites implicites sauf plafonds internes.
//   - Timeout socket simple via SO_RCVTIMEO/SO_SNDTIMEO.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ==================== Réseau ==================== */
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

#ifndef RDS_API
#define RDS_API
#endif

/* ==================== Types de réponses ==================== */

typedef enum {
    RDS_T_NIL=0,   /* $-1 ou *-1 */
    RDS_T_SIMPLE,  /* +... */
    RDS_T_ERROR,   /* -... */
    RDS_T_INT,     /* :... */
    RDS_T_BULK,    /* $len\r\nbytes... */
    RDS_T_ARRAY    /* *N ... */
} rds_type;

typedef struct rds_reply rds_reply;
struct rds_reply {
    rds_type type;
    long long integer;      /* pour INT */
    char* str;              /* SIMPLE, ERROR, BULK (peut contenir \0 pour BULK) */
    size_t len;             /* longueur de str pour BULK, sinon strlen(str) */
    rds_reply** elems;      /* ARRAY */
    size_t nelem;
};

typedef struct {
    sock_t s;
} rds_client;

/* ==================== IO helpers ==================== */

static int _io_readn(sock_t s, void* p, size_t n){
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
static int _io_writen(sock_t s, const void* p, size_t n){
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
static int _readline(sock_t s, char* buf, size_t cap, size_t* out_n){ /* lit jusqu'à CRLF, sans CRLF dans buf */
    size_t i=0;
    while (i+1<cap){
        char c;
        if (_io_readn(s,&c,1)!=0) return -1;
        if (c=='\r'){
            char lf;
            if (_io_readn(s,&lf,1)!=0) return -1;
            if (lf!='\n') return -1;
            buf[i]=0; if (out_n) *out_n=i; return 0;
        }
        buf[i++]=c;
    }
    return -1;
}

/* ==================== API connexion ==================== */

RDS_API int rds_connect(rds_client* c, const char* host, const char* port, int timeout_ms){
    if (!c) return -1;
    memset(c,0,sizeof *c);
    if (_net_init()!=0) return -1;

    struct addrinfo hints; memset(&hints,0,sizeof hints);
    hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    struct addrinfo* res=NULL;
    if (getaddrinfo(host,port,&hints,&res)!=0) return -1;

    sock_t s=INVALID_SOCKET;
    for (struct addrinfo* it=res; it; it=it->ai_next){
        s=(sock_t)socket(it->ai_family,it->ai_socktype,it->ai_protocol);
        if (s==INVALID_SOCKET) continue;
#if defined(_WIN32)
        if (timeout_ms>0){ DWORD tv=(DWORD)timeout_ms;
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&tv,sizeof tv);
            setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&tv,sizeof tv);
        }
#else
        if (timeout_ms>0){ struct timeval tv={timeout_ms/1000,(timeout_ms%1000)*1000};
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
            setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
        }
#endif
        if (connect(s,it->ai_addr,(socklen_cast)it->ai_addrlen)==0) break;
        CLOSESOCK(s); s=INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s==INVALID_SOCKET) return -1;
    c->s=s;
    return 0;
}
RDS_API void rds_close(rds_client* c){
    if (!c) return;
    if (c->s!=INVALID_SOCKET){ CLOSESOCK(c->s); c->s=INVALID_SOCKET; }
    _net_shutdown();
}

/* ==================== Construction de commande RESP ==================== */

static int _is_space(char ch){ return ch==' '||ch=='\t'||ch=='\n'||ch=='\r'; }

/* Envoi d'une commande argv/argc en RESP. */
RDS_API int rds_cmd_argv(rds_client* c, int argc, const char* const* argv, const size_t* argl){
    if (!c || c->s==INVALID_SOCKET || argc<1) return -1;

    /* calcul taille */
    size_t tot=0;
    char hdr[64];
    int n = snprintf(hdr,sizeof hdr,"*%d\r\n",argc);
    if (n<0) return -1;
    tot += (size_t)n;
    for (int i=0;i<argc;i++){
        size_t L = argl? argl[i] : (argv[i]?strlen(argv[i]):0);
        n = snprintf(hdr,sizeof hdr,"$%zu\r\n", L);
        if (n<0) return -1;
        tot += (size_t)n + L + 2;
    }

    /* écrire en flux: d’abord *N */
    n = snprintf(hdr,sizeof hdr,"*%d\r\n",argc);
    if (_io_writen(c->s,hdr,(size_t)n)!=0) return -1;
    for (int i=0;i<argc;i++){
        const char* a = argv[i]?argv[i]:"";
        size_t L = argl? argl[i] : strlen(a);
        n = snprintf(hdr,sizeof hdr,"$%zu\r\n", L);
        if (_io_writen(c->s,hdr,(size_t)n)!=0) return -1;
        if (L && _io_writen(c->s,a,L)!=0) return -1;
        if (_io_writen(c->s,"\r\n",2)!=0) return -1;
    }
    return 0;
}

/* Variante format printf, découpe naïve par espaces sauf séquences protégées entre { } ou " " */
RDS_API int rds_cmdf(rds_client* c, const char* fmt, ...){
    if (!c||!fmt) return -1;
    char buf[4096];
    va_list ap; va_start(ap,fmt);
#if defined(_WIN32)
    _vsnprintf(buf,sizeof buf,fmt,ap);
#else
    vsnprintf(buf,sizeof buf,fmt,ap);
#endif
    va_end(ap);

    /* tokenization simple: "quoted" et {raw} gardent espaces, échappement \x */
    const char* argv[256]; size_t argl[256]; int argc=0;
    const char* p=buf;
    while (*p){
        while (_is_space(*p)) p++;
        if (!*p) break;
        if (*p=='"'){
            p++; const char* start=p;
            char* out=(char*)start; /* in place */
            while (*p && *p!='"'){
                if (*p=='\\' && p[1]){ p++; *out++=*p++; }
                else *out++=*p++;
            }
            *out=0; size_t L=(size_t)(out-start);
            if (*p=='"') p++;
            argv[argc]=start; argl[argc]=L; argc++;
        } else if (*p=='{'){
            p++; const char* start=p;
            while (*p && *p!='}') p++;
            size_t L=(size_t)(p-start);
            argv[argc]=start; argl[argc]=L; argc++;
            if (*p=='}') p++;
        } else {
            const char* start=p;
            while (*p && !_is_space(*p)) p++;
            size_t L=(size_t)(p-start);
            argv[argc]=start; argl[argc]=L; argc++;
        }
        if (argc>=256) break;
    }
    if (argc==0) return -1;
    return rds_cmd_argv(c, argc, argv, argl);
}

/* ==================== Parse des réponses ==================== */

static rds_reply* _rnew(rds_type t){
    rds_reply* r = (rds_reply*)calloc(1,sizeof *r);
    if (!r) return NULL; r->type=t; return r;
}
RDS_API void rds_free_reply(rds_reply* r){
    if (!r) return;
    if (r->type==RDS_T_BULK || r->type==RDS_T_SIMPLE || r->type==RDS_T_ERROR){
        free(r->str);
    }
    if (r->type==RDS_T_ARRAY && r->elems){
        for (size_t i=0;i<r->nelem;i++) rds_free_reply(r->elems[i]);
        free(r->elems);
    }
    free(r);
}

static int _parse_int(const char* s, long long* out){
    int neg=0; long long v=0;
    if (*s=='-'){ neg=1; s++; }
    if (!*s) return -1;
    while (*s){
        if (*s<'0'||*s>'9') return -1;
        v = v*10 + (*s - '0');
        s++;
    }
    *out = neg ? -v : v; return 0;
}

RDS_API int rds_read_reply(rds_client* c, rds_reply** out){
    if (!c||c->s==INVALID_SOCKET||!out) return -1;
    char t;
    if (_io_readn(c->s,&t,1)!=0) return -1;

    if (t=='+' || t=='-' || t==':' || t=='$' || t=='*'){
        if (t=='+' || t=='-' || t==':'){
            char line[8192]; size_t ln=0;
            if (_readline(c->s,line,sizeof line,&ln)!=0) return -1;
            if (t=='+'){
                rds_reply* r=_rnew(RDS_T_SIMPLE); if(!r) return -1;
                r->str=(char*)malloc(ln+1); if(!r->str){ free(r); return -1; }
                memcpy(r->str,line,ln+1); r->len=ln; *out=r; return 0;
            } else if (t=='-'){
                rds_reply* r=_rnew(RDS_T_ERROR); if(!r) return -1;
                r->str=(char*)malloc(ln+1); if(!r->str){ free(r); return -1; }
                memcpy(r->str,line,ln+1); r->len=ln; *out=r; return 0;
            } else { /* :integer */
                long long v=0; if (_parse_int(line,&v)!=0) return -1;
                rds_reply* r=_rnew(RDS_T_INT); if(!r) return -1; r->integer=v; *out=r; return 0;
            }
        } else if (t=='$'){ /* bulk */
            char line[64]; size_t ln=0;
            if (_readline(c->s,line,sizeof line,&ln)!=0) return -1;
            long long L=0; if (_parse_int(line,&L)!=0) return -1;
            if (L==-1){ rds_reply* r=_rnew(RDS_T_NIL); if(!r) return -1; *out=r; return 0; }
            if (L<0 || L> (long long) (64*1024*1024)) return -1;
            rds_reply* r=_rnew(RDS_T_BULK); if(!r) return -1;
            r->str=(char*)malloc((size_t)L+1); if(!r->str){ free(r); return -1; }
            if (_io_readn(c->s,r->str,(size_t)L)!=0){ rds_free_reply(r); return -1; }
            r->str[L]=0; r->len=(size_t)L;
            char crlf[2]; if (_io_readn(c->s,crlf,2)!=0){ rds_free_reply(r); return -1; }
            *out=r; return 0;
        } else { /* *array */
            char line[64]; size_t ln=0;
            if (_readline(c->s,line,sizeof line,&ln)!=0) return -1;
            long long N=0; if (_parse_int(line,&N)!=0) return -1;
            if (N==-1){ rds_reply* r=_rnew(RDS_T_NIL); if(!r) return -1; *out=r; return 0; }
            if (N<0 || N>4096) return -1;
            rds_reply* r=_rnew(RDS_T_ARRAY); if(!r) return -1;
            r->elems=(rds_reply**)calloc((size_t)N,sizeof *r->elems); if(!r->elems){ free(r); return -1; }
            r->nelem=(size_t)N;
            for (size_t i=0;i<r->nelem;i++){
                if (rds_read_reply(c,&r->elems[i])!=0){ rds_free_reply(r); return -1; }
            }
            *out=r; return 0;
        }
    }
    return -1;
}

/* ==================== Helpers ==================== */

RDS_API int rds_ping(rds_client* c){
    if (rds_cmdf(c,"PING")!=0) return -1;
    rds_reply* r=NULL; if (rds_read_reply(c,&r)!=0) return -1;
    int ok = (r->type==RDS_T_SIMPLE && r->str && (strcmp(r->str,"PONG")==0));
    rds_free_reply(r);
    return ok?0:-1;
}
RDS_API int rds_auth(rds_client* c, const char* username, const char* password){
    int rc;
    if (username && *username) rc = rds_cmdf(c,"AUTH %s %s", username, password?password:"");
    else rc = rds_cmdf(c,"AUTH %s", password?password:"");
    if (rc!=0) return -1;
    rds_reply* r=NULL; if (rds_read_reply(c,&r)!=0) return -1;
    int ok = (r->type==RDS_T_SIMPLE && r->str && (strcmp(r->str,"OK")==0));
    rds_free_reply(r);
    return ok?0:-1;
}
RDS_API int rds_select(rds_client* c, int db){
    if (rds_cmdf(c,"SELECT %d", db)!=0) return -1;
    rds_reply* r=NULL; if (rds_read_reply(c,&r)!=0) return -1;
    int ok = (r->type==RDS_T_SIMPLE && r->str && (strcmp(r->str,"OK")==0));
    rds_free_reply(r);
    return ok?0:-1;
}
RDS_API int rds_setex(rds_client* c, const char* key, const void* val, size_t n, int ttl_sec){
    const char* argv[5]; size_t argl[5];
    argv[0]="SET"; argl[0]=3;
    argv[1]=key; argl[1]=strlen(key);
    argv[2]="EX"; argl[2]=2;
    char ttlb[32]; snprintf(ttlb,sizeof ttlb,"%d",ttl_sec);
    argv[3]=ttlb; argl[3]=strlen(ttlb);
    argv[4]=(const char*)val; argl[4]=n;
    if (rds_cmd_argv(c,5,argv,argl)!=0) return -1;
    rds_reply* r=NULL; if (rds_read_reply(c,&r)!=0) return -1;
    int ok = (r->type==RDS_T_SIMPLE && r->str && strcmp(r->str,"OK")==0);
    rds_free_reply(r); return ok?0:-1;
}
RDS_API int rds_get(rds_client* c, const char* key, const void** data, size_t* len){
    if (rds_cmdf(c,"GET %s", key)!=0) return -1;
    rds_reply* r=NULL; if (rds_read_reply(c,&r)!=0) return -1;
    int rc=-1;
    if (r->type==RDS_T_BULK){ if (data) *data=r->str; if (len) *len=r->len; /* transfer ownership */ r->str=NULL; rc=0; }
    else if (r->type==RDS_T_NIL){ if (data) *data=NULL; if (len) *len=0; rc=1; }
    rds_free_reply(r); return rc;
}

/* Envoi générique et renvoi de la réponse (ownership à l’appelant). */
RDS_API int rds_command(rds_client* c, int argc, const char* const* argv, const size_t* argl, rds_reply** out){
    if (rds_cmd_argv(c,argc,argv,argl)!=0) return -1;
    return rds_read_reply(c,out);
}

/* ==================== Test ==================== */
#ifdef REDIS_TEST
int main(int argc, char** argv){
    const char* host = (argc>1)? argv[1] : "127.0.0.1";
    const char* port = (argc>2)? argv[2] : "6379";

    rds_client c;
    if (rds_connect(&c, host, port, 3000)!=0){ fprintf(stderr,"connect fail\n"); return 1; }
    if (rds_ping(&c)!=0){ fprintf(stderr,"ping fail\n"); rds_close(&c); return 1; }

    /* SETEX */
    const char val[] = "hello";
    if (rds_setex(&c,"k1",val,sizeof val-1,10)!=0){ fprintf(stderr,"setex fail\n"); rds_close(&c); return 1; }

    /* GET */
    const void* data=NULL; size_t n=0;
    int grc = rds_get(&c,"k1",&data,&n);
    if (grc==0){ printf("GET k1 -> %.*s\n",(int)n,(const char*)data); free((void*)data); }
    else if (grc==1){ printf("GET k1 -> nil\n"); }
    else { printf("GET error\n"); }

    /* INFO Server */
    if (rds_cmdf(&c,"INFO server")==0){
        rds_reply* r=NULL; if (rds_read_reply(&c,&r)==0){
            if (r->type==RDS_T_BULK) printf("INFO len=%zu\n", r->len);
            rds_free_reply(r);
        }
    }

    rds_close(&c);
    return 0;
}
#endif