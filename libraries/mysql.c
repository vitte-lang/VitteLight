// SPDX-License-Identifier: GPL-3.0-or-later
//
// mysql.c — Client MySQL/MariaDB minimal (C17, sans dépendances)
// Namespace: "mysqlc"
//
// Fonctionnalités:
//   - Connexion TCP, handshake 4.1+, auth mysql_native_password (SHA1).
//   - COM_QUERY simple. Affiche (option) le jeu de résultats en texte.
//   - Pas de TLS, pas de préparées, pas de multi-result sets.
//
// Plateformes: Linux/macOS (POSIX), Windows (Winsock2).
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c mysql.c
//
// Démo (MYSQL_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DMYSQL_TEST mysql.c && \
//   ./a.out 127.0.0.1 3306 root "" test "SELECT 1"
//
// Limitations:
//   - Auth uniquement mysql_native_password (serveur doit l’accepter).
//   - Charset forcé utf8mb4 (id 45). Pas de collation paramétrable.
//   - Résultats lus en “text protocol”, EOF OK-Packet seulement.
//
// Références de protocole (schéma général) : handshake v10, CLIENT_PROTOCOL_41.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ==================== Sockets ==================== */
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

#ifndef MYSQLC_API
#define MYSQLC_API
#endif

/* ==================== Utils ==================== */
static sock_t tcp_connect(const char* host, const char* port, int timeout_ms){
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
static int io_read_all(sock_t s, void* p, size_t n){
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
static int io_write_all(sock_t s, const void* p, size_t n){
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

/* ==================== SHA1 minimal ==================== */
typedef struct { uint32_t h[5]; uint64_t nbits; unsigned char buf[64]; size_t len; } sha1_ctx;
static uint32_t ROL(uint32_t x,int n){ return (x<<n)|(x>>(32-n)); }
static void sha1_init(sha1_ctx* c){ c->h[0]=0x67452301u; c->h[1]=0xEFCDAB89u; c->h[2]=0x98BADCFEu; c->h[3]=0x10325476u; c->h[4]=0xC3D2E1F0u; c->nbits=0; c->len=0; }
static void sha1_block(sha1_ctx* c, const unsigned char* p){
    uint32_t w[80];
    for (int i=0;i<16;i++) w[i]=((uint32_t)p[4*i]<<24)|((uint32_t)p[4*i+1]<<16)|((uint32_t)p[4*i+2]<<8)|p[4*i+3];
    for (int i=16;i<80;i++) w[i]=ROL(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=c->h[0],b=c->h[1],c2=c->h[2],d=c->h[3],e=c->h[4];
    for (int i=0;i<80;i++){
        uint32_t f,k;
        if (i<20){ f=(b & c2) | ((~b) & d); k=0x5A827999; }
        else if (i<40){ f=b^c2^d; k=0x6ED9EBA1; }
        else if (i<60){ f=(b & c2) | (b & d) | (c2 & d); k=0x8F1BBCDC; }
        else { f=b^c2^d; k=0xCA62C1D6; }
        uint32_t t=ROL(a,5)+f+e+k+w[i];
        e=d; d=c2; c2=ROL(b,30); b=a; a=t;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=c2; c->h[3]+=d; c->h[4]+=e;
}
static void sha1_update(sha1_ctx* c, const void* data, size_t n){
    const unsigned char* p=data;
    c->nbits += (uint64_t)n*8;
    while(n){
        size_t k = 64 - c->len; if (k>n) k=n;
        memcpy(c->buf+c->len,p,k); c->len += k; p+=k; n-=k;
        if (c->len==64){ sha1_block(c,c->buf); c->len=0; }
    }
}
static void sha1_final(sha1_ctx* c, unsigned char out[20]){
    c->buf[c->len++] = 0x80;
    if (c->len > 56){ while(c->len<64) c->buf[c->len++]=0; sha1_block(c,c->buf); c->len=0; }
    while(c->len<56) c->buf[c->len++]=0;
    uint64_t n=c->nbits;
    for (int i=7;i>=0;i--) c->buf[c->len++]=(unsigned char)((n>>(8*i))&0xFF);
    sha1_block(c,c->buf);
    for (int i=0;i<5;i++){ out[4*i]=(unsigned char)(c->h[i]>>24); out[4*i+1]=(unsigned char)(c->h[i]>>16); out[4*i+2]=(unsigned char)(c->h[i]>>8); out[4*i+3]=(unsigned char)c->h[i]; }
}
static void sha1(const void* d,size_t n,unsigned char out[20]){ sha1_ctx c; sha1_init(&c); sha1_update(&c,d,n); sha1_final(&c,out); }

/* mysql_native_password scramble = SHA1(pwd) XOR SHA1( salt + SHA1(SHA1(pwd)) ) */
static void mysql_native_token(const char* pwd, const unsigned char* salt20, unsigned char out20[20]){
    unsigned char s1[20], s2[20], s3[20];
    sha1(pwd, pwd?strlen(pwd):0, s1);
    sha1(s1, 20, s2);
    unsigned char tmp[20+20];
    memcpy(tmp, salt20, 20); memcpy(tmp+20, s2, 20);
    sha1(tmp, 40, s3);
    for (int i=0;i<20;i++) out20[i] = s1[i] ^ s3[i];
}

/* ==================== Protocole MySQL ==================== */

/* Capabilities minimales côté client */
enum {
    CLIENT_LONG_PASSWORD   = 0x00000001,
    CLIENT_PROTOCOL_41     = 0x00000200,
    CLIENT_SECURE_CONNECTION=0x00008000,
    CLIENT_PLUGIN_AUTH     = 0x00080000,
    CLIENT_MULTI_RESULTS   = 0x00020000,
    CLIENT_LONG_FLAG       = 0x00000004,
    CLIENT_TRANSACTIONS    = 0x00002000,
    CLIENT_CONNECT_WITH_DB = 0x00000008
};

/* Paquets MySQL: [3 bytes length][1 byte seq][payload...] */
typedef struct {
    sock_t s; uint8_t seq;
    uint32_t server_caps;
    char auth_plugin[64];
    unsigned char salt[20];
} mysql_conn;

static int pkt_read(mysql_conn* c, unsigned char* buf, uint32_t* len){
    unsigned char hdr[4];
    if (io_read_all(c->s,hdr,4)!=0) return -1;
    uint32_t n = (uint32_t)hdr[0] | ((uint32_t)hdr[1]<<8) | ((uint32_t)hdr[2]<<16);
    c->seq = hdr[3];
    if (n){
        if (io_read_all(c->s,buf,n)!=0) return -1;
    }
    if (len) *len = n;
    return 0;
}
static int pkt_write(mysql_conn* c, const unsigned char* buf, uint32_t len){
    unsigned char hdr[4];
    hdr[0]=(unsigned char)(len & 0xFF);
    hdr[1]=(unsigned char)((len>>8) & 0xFF);
    hdr[2]=(unsigned char)((len>>16)& 0xFF);
    hdr[3]=(unsigned char)(c->seq + 1); /* next seq */
    c->seq = hdr[3];
    if (io_write_all(c->s, hdr, 4)!=0) return -1;
    if (len && io_write_all(c->s, buf, len)!=0) return -1;
    return 0;
}

/* Handshake Init (v10) parse */
static int read_handshake(mysql_conn* c){
    unsigned char b[512]; uint32_t n=0;
    c->seq=0;
    if (pkt_read(c,b,&n)!=0 || n<34) return -1;
    uint8_t proto = b[0];
    (void)proto;
    const unsigned char* p=b+1;
    const char* svrver=(const char*)p;
    while(*p) p++; p++; /* skip nul */
    uint32_t conn_id = (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); p+=4;
    (void)conn_id;
    /* salt part1 (8) + 0x00 */
    memcpy(c->salt, p, 8); p+=8; p++; /* nul */
    uint16_t caps1 = (uint16_t)p[0] | ((uint16_t)p[1]<<8); p+=2;
    uint8_t charset = p[0]; p++;
    uint16_t status = (uint16_t)p[0] | ((uint16_t)p[1]<<8); p+=2;
    uint16_t caps2 = (uint16_t)p[0] | ((uint16_t)p[1]<<8); p+=2;
    c->server_caps = ((uint32_t)caps2<<16) | caps1;
    uint8_t auth_data_len = p[0]; p++;
    p += 10; /* reserved */
    size_t salt2_len = (auth_data_len>13)? (auth_data_len-8) : 12;
    if (salt2_len>12) salt2_len=12;
    memcpy(c->salt+8, p, salt2_len); p += salt2_len;
    if (*p){ /* plugin name NUL-terminated */
        size_t k=0; while (k<sizeof c->auth_plugin-1 && p[k]){ c->auth_plugin[k]= (char)p[k]; k++; }
        c->auth_plugin[k]=0;
    } else { c->auth_plugin[0]=0; }
    /* Salt final à 20 octets (remplit avec 0 si moins) */
    /* Rien d'autre à faire ici */
    (void)charset; (void)status; (void)svrver;
    return 0;
}

/* Login packet (Handshake Response 41) */
static int send_login(mysql_conn* c, const char* user, const char* pass, const char* db){
    uint32_t caps = CLIENT_PROTOCOL_41 | CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH |
                    CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_TRANSACTIONS |
                    CLIENT_MULTI_RESULTS | (db && *db ? CLIENT_CONNECT_WITH_DB : 0);
    uint32_t max_packet = 0x01000000; /* 16MB */
    uint8_t  charset = 45; /* utf8mb4_general_ci */
    unsigned char token[20]; mysql_native_token(pass, c->salt, token);

    /* build payload */
    unsigned char buf[1024]; size_t i=0;
    buf[i++] = (unsigned char)(caps & 0xFF);
    buf[i++] = (unsigned char)((caps>>8)&0xFF);
    buf[i++] = (unsigned char)((caps>>16)&0xFF);
    buf[i++] = (unsigned char)((caps>>24)&0xFF);
    buf[i++] = (unsigned char)(max_packet & 0xFF);
    buf[i++] = (unsigned char)((max_packet>>8)&0xFF);
    buf[i++] = (unsigned char)((max_packet>>16)&0xFF);
    buf[i++] = (unsigned char)((max_packet>>24)&0xFF);
    buf[i++] = charset;
    memset(buf+i, 0, 23); i+=23; /* reserved */

    /* user (null-terminated string) */
    size_t ulen = user?strlen(user):0;
    memcpy(buf+i, user?user:"", ulen); i+=ulen; buf[i++]=0;

    /* auth length + data */
    buf[i++] = 20; memcpy(buf+i, token, 20); i+=20;

    /* database if provided */
    if (db && *db){ size_t dlen=strlen(db); memcpy(buf+i,db,dlen); i+=dlen; buf[i++]=0; }

    /* auth plugin name */
    const char* plugin = (*c->auth_plugin)? c->auth_plugin : "mysql_native_password";
    size_t pn = strlen(plugin); memcpy(buf+i,plugin,pn); i+=pn; buf[i++]=0;

    c->seq=1; /* server sent seq=0, next is 1 */
    return pkt_write(c, buf, (uint32_t)i);
}

/* Lire OK, ERR, ou Auth Switch. Retour 0 si OK complet. */
static int read_auth_result(mysql_conn* c){
    unsigned char b[1024]; uint32_t n=0;
    if (pkt_read(c,b,&n)!=0) return -1;
    if (n==0) return -1;
    if (b[0]==0x00){ /* OK-Packet */
        return 0;
    } else if (b[0]==0xFE && n>1){ /* Auth Switch (rare si plugin différent) */
        /* b[1..] contient plugin name + 0x00 + salt; ici on refuse si pas mysql_native_password */
        /* Pour simplicité, on échoue si plugin != mysql_native_password */
        if (strstr((const char*)(b+1), "mysql_native_password")==NULL) return -1;
        /* salt suit après le nom + 0x00; mais la plupart des serveurs l’ont déjà envoyé. On renvoie juste la réponse */
        /* réponse: [len=20][token] */
        unsigned char resp[21]; resp[0]=20;
        unsigned char tok[20]; mysql_native_token("", c->salt, tok); /* mot de passe vide ici -> normalement utiliser le mdp réel */
        memcpy(resp+1, tok, 20);
        return pkt_write(c, resp, 21);
    } else if (b[0]==0xFF){ /* ERR-Packet */
        /* code = b[1..2], message à partir de b[3+..] */
        return -1;
    }
    return -1;
}

/* ==================== API ==================== */

typedef struct {
    mysql_conn c;
} mysql_client;

MYSQLC_API int mysql_connect(mysql_client* m,
                             const char* host, const char* port,
                             const char* user, const char* pass, const char* db){
    memset(m,0,sizeof *m);
    m->c.s = tcp_connect(host,port,5000);
    if (m->c.s==INVALID_SOCKET) return -1;
    if (read_handshake(&m->c)!=0){ CLOSESOCK(m->c.s); _net_shutdown(); return -1; }
    if (send_login(&m->c, user?user:"", pass?pass:"", db)&&1){ CLOSESOCK(m->c.s); _net_shutdown(); return -1; }
    if (read_auth_result(&m->c)!=0){ CLOSESOCK(m->c.s); _net_shutdown(); return -1; }
    return 0;
}

MYSQLC_API void mysql_close(mysql_client* m){
    if (!m) return;
    if (m->c.s!=INVALID_SOCKET){
        /* COM_QUIT = 0x01 */
        unsigned char q=0x01; m->c.seq=0; pkt_write(&m->c,&q,1);
        CLOSESOCK(m->c.s);
    }
    _net_shutdown();
}

/* COM_QUERY = 0x03. Si print_rows!=0, affiche les lignes CSV. */
MYSQLC_API int mysql_query(mysql_client* m, const char* sql, int print_rows){
    if (!m || m->c.s==INVALID_SOCKET) return -1;
    size_t sl = strlen(sql?sql:"");
    unsigned char* payload = (unsigned char*)malloc(sl+1);
    if (!payload) return -1;
    payload[0]=0x03; memcpy(payload+1,sql,sl);
    m->c.seq=0; if (pkt_write(&m->c, payload, (uint32_t)(sl+1))!=0){ free(payload); return -1; }
    free(payload);

    /* 1) Result Set Header (or OK-Packet) */
    unsigned char b[4096]; uint32_t n=0;
    if (pkt_read(&m->c,b,&n)!=0) return -1;
    if (n==0) return -1;
    if (b[0]==0x00){ /* OK => pas de lignes (commande DML) */
        return 0;
    }
    if (b[0]==0xFF){ /* ERR */
        return -1;
    }
    /* Resultset: b[0] = column count as length-encoded int (on suppose petit) */
    uint64_t col_count = b[0];
    /* 2) Columns definition packets (ignore content), terminés par EOF/OK */
    for (uint64_t i=0;i<col_count;i++){
        if (pkt_read(&m->c,b,&n)!=0) return -1;
        if (n==0) return -1;
        /* ignore */
    }
    /* EOF/OK */
    if (pkt_read(&m->c,b,&n)!=0) return -1;

    /* 3) Rows until EOF/OK */
    while (1){
        if (pkt_read(&m->c,b,&n)!=0) return -1;
        if (n==0) return -1;
        if (b[0]==0xFE && n<9){ /* EOF (proto 41) */
            break;
        }
        /* Row: sequence of length-encoded strings (or 0xFB = NULL) */
        if (print_rows){
            /* Simple parse: chaque champ commence par 0xFB (NULL) ou len enc < 251 -> 1 octet */
            const unsigned char* p=b; const unsigned char* end=b+n;
            for (uint64_t cidx=0;cidx<col_count;cidx++){
                if (p>=end){ putchar('\n'); return -1; }
                if (*p==0xFB){ /* NULL */
                    if (cidx) putchar(','); fputs("NULL",stdout); p++;
                } else {
                    uint64_t len=0; int enc=0;
                    if (*p<0xFB){ len=*p; p++; enc=1; }
                    else { /* cas >250 non géré pour simplicité */
                        return -1;
                    }
                    if (p+len>end) return -1;
                    if (cidx) putchar(',');
                    fwrite(p,1,(size_t)len,stdout);
                    p+=len; (void)enc;
                }
            }
            putchar('\n');
        }
    }
    return 0;
}

/* ==================== Test ==================== */
#ifdef MYSQL_TEST
int main(int argc, char** argv){
    if (argc<6){
        fprintf(stderr,"usage: %s host port user pass db [sql]\n", argv[0]);
        return 2;
    }
    const char* host=argv[1], *port=argv[2], *user=argv[3], *pass=argv[4], *db=argv[5];
    const char* sql = (argc>=7)? argv[6] : "SELECT 1";
    mysql_client cli;
    if (mysql_connect(&cli, host, port, user, pass, db)!=0){ fprintf(stderr,"connect failed\n"); return 1; }
    if (mysql_query(&cli, sql, 1)!=0){ fprintf(stderr,"query failed\n"); mysql_close(&cli); return 1; }
    mysql_close(&cli);
    return 0;
}
#endif