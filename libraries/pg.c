// SPDX-License-Identifier: GPL-3.0-or-later
//
// pg.c — Client PostgreSQL minimal (C17, sans dépendances)
// Namespace: "pgc"
//
// Fonctionnalités:
//   - Connexion TCP, StartupMessage v3, auth cleartext et md5.
//   - COM Query ('Q'), affichage optionnel des lignes en CSV.
//   - Lecture RowDescription/DataRow/CommandComplete/ReadyForQuery.
//   - Sans TLS, sans paramètres avancés.
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c pg.c
//
// Démo (PG_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DPG_TEST pg.c && \
//   ./a.out 127.0.0.1 5432 db user pass "select 1"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

#ifndef PGC_API
#define PGC_API
#endif

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
static sock_t tcp_connect(const char* host,const char* port,int timeout_ms){
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
    return s;
}

/* ==================== Utils ==================== */

static uint32_t be32(uint32_t x){
    unsigned char b[4]; b[0]=(unsigned char)(x>>24); b[1]=(unsigned char)(x>>16); b[2]=(unsigned char)(x>>8); b[3]=(unsigned char)x;
    uint32_t y; memcpy(&y,b,4); return y;
}
static uint32_t rd_be32(const unsigned char* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t rd_be16(const unsigned char* p){ return (uint16_t)((p[0]<<8)|p[1]); }

/* ==================== MD5 minimal ==================== */
/* RFC 1321 compact */
typedef struct{ uint32_t a,b,c,d; uint64_t bits; unsigned char buf[64]; size_t len; } md5_ctx;
static uint32_t _rol(uint32_t x,int n){ return (x<<n)|(x>>(32-n)); }
static void md5_init(md5_ctx* c){ c->a=0x67452301u; c->b=0xefcdab89u; c->c=0x98badcfeu; c->d=0x10325476u; c->bits=0; c->len=0; }
static void md5_step(md5_ctx* c, const unsigned char blk[64]){
    uint32_t A=c->a,B=c->b,C=c->c,D=c->d,X[16];
    for(int i=0;i<16;i++) X[i]= (uint32_t)blk[i*4] | ((uint32_t)blk[i*4+1]<<8) | ((uint32_t)blk[i*4+2]<<16) | ((uint32_t)blk[i*4+3]<<24);
    #define F(x,y,z) ((x&y)|(~x&z))
    #define G(x,y,z) ((x&z)|(y&~z))
    #define H(x,y,z) (x^y^z)
    #define I(x,y,z) (y^(x|~z))
    #define R(a,b,c,d,k,s,t,fun) a=b+_rol(a+fun(b,c,d)+X[k]+t,s)
    R(A,B,C,D, 0, 7,0xd76aa478,F); R(D,A,B,C, 1,12,0xe8c7b756,F); R(C,D,A,B, 2,17,0x242070db,F); R(B,C,D,A, 3,22,0xc1bdceee,F);
    R(A,B,C,D, 4, 7,0xf57c0faf,F); R(D,A,B,C, 5,12,0x4787c62a,F); R(C,D,A,B, 6,17,0xa8304613,F); R(B,C,D,A, 7,22,0xfd469501,F);
    R(A,B,C,D, 8, 7,0x698098d8,F); R(D,A,B,C, 9,12,0x8b44f7af,F); R(C,D,A,B,10,17,0xffff5bb1,F); R(B,C,D,A,11,22,0x895cd7be,F);
    R(A,B,C,D,12, 7,0x6b901122,F); R(D,A,B,C,13,12,0xfd987193,F); R(C,D,A,B,14,17,0xa679438e,F); R(B,C,D,A,15,22,0x49b40821,F);
    R(A,B,C,D, 1, 5,0xf61e2562,G); R(D,A,B,C, 6, 9,0xc040b340,G); R(C,D,A,B,11,14,0x265e5a51,G); R(B,C,D,A, 0,20,0xe9b6c7aa,G);
    R(A,B,C,D, 5, 5,0xd62f105d,G); R(D,A,B,C,10, 9,0x02441453,G); R(C,D,A,B,15,14,0xd8a1e681,G); R(B,C,D,A, 4,20,0xe7d3fbc8,G);
    R(A,B,C,D, 9, 5,0x21e1cde6,G); R(D,A,B,C,14, 9,0xc33707d6,G); R(C,D,A,B, 3,14,0xf4d50d87,G); R(B,C,D,A, 8,20,0x455a14ed,G);
    R(A,B,C,D,13, 5,0xa9e3e905,G); R(D,A,B,C, 2, 9,0xfcefa3f8,G); R(C,D,A,B, 7,14,0x676f02d9,G); R(B,C,D,A,12,20,0x8d2a4c8a,G);
    R(A,B,C,D, 5, 4,0xfffa3942,H); R(D,A,B,C, 8,11,0x8771f681,H); R(C,D,A,B,11,16,0x6d9d6122,H); R(B,C,D,A,14,23,0xfde5380c,H);
    R(A,B,C,D, 1, 4,0xa4beea44,H); R(D,A,B,C, 4,11,0x4bdecfa9,H); R(C,D,A,B, 7,16,0xf6bb4b60,H); R(B,C,D,A,10,23,0xbebfbc70,H);
    R(A,B,C,D,13, 4,0x289b7ec6,H); R(D,A,B,C, 0,11,0xeaa127fa,H); R(C,D,A,B, 3,16,0xd4ef3085,H); R(B,C,D,A, 6,23,0x04881d05,H);
    R(A,B,C,D, 9, 4,0xd9d4d039,H); R(D,A,B,C,12,11,0xe6db99e5,H); R(C,D,A,B,15,16,0x1fa27cf8,H); R(B,C,D,A, 2,23,0xc4ac5665,H);
    R(A,B,C,D, 0, 6,0xf4292244,I); R(D,A,B,C, 7,10,0x432aff97,I); R(C,D,A,B,14,15,0xab9423a7,I); R(B,C,D,A, 5,21,0xfc93a039,I);
    R(A,B,C,D,12, 6,0x655b59c3,I); R(D,A,B,C, 3,10,0x8f0ccc92,I); R(C,D,A,B,10,15,0xffeff47d,I); R(B,C,D,A, 1,21,0x85845dd1,I);
    R(A,B,C,D, 8, 6,0x6fa87e4f,I); R(D,A,B,C,15,10,0xfe2ce6e0,I); R(C,D,A,B, 6,15,0xa3014314,I); R(B,C,D,A,13,21,0x4e0811a1,I);
    R(A,B,C,D, 4, 6,0xf7537e82,I); R(D,A,B,C,11,10,0xbd3af235,I); R(C,D,A,B, 2,15,0x2ad7d2bb,I); R(B,C,D,A, 9,21,0xeb86d391,I);
    c->a += A; c->b += B; c->c += C; c->d += D;
    #undef F #undef G #undef H #undef I #undef R
}
static void md5_update(md5_ctx* c, const void* data, size_t n){
    const unsigned char* p=data; c->bits += (uint64_t)n*8;
    while(n){
        size_t k = 64 - c->len; if (k>n) k=n;
        memcpy(c->buf+c->len, p, k); c->len += k; p += k; n -= k;
        if (c->len==64){ md5_step(c,c->buf); c->len=0; }
    }
}
static void md5_final(md5_ctx* c, unsigned char out[16]){
    c->buf[c->len++] = 0x80;
    if (c->len>56){ while(c->len<64) c->buf[c->len++]=0; md5_step(c,c->buf); c->len=0; }
    while(c->len<56) c->buf[c->len++]=0;
    uint64_t bits=c->bits;
    for (int i=0;i<8;i++){ c->buf[56+i]=(unsigned char)(bits & 0xFF); bits >>= 8; }
    md5_step(c,c->buf);
    uint32_t a=c->a,b=c->b,d=c->d,e=c->c;
    for (int i=0;i<4;i++){ out[i]=(unsigned char)(a & 0xFF); a>>=8; }
    for (int i=0;i<4;i++){ out[4+i]=(unsigned char)(b & 0xFF); b>>=8; }
    for (int i=0;i<4;i++){ out[8+i]=(unsigned char)(e & 0xFF); e>>=8; }
    for (int i=0;i<4;i++){ out[12+i]=(unsigned char)(d & 0xFF); d>>=8; }
}
static void md5(const void* d,size_t n,unsigned char out[16]){ md5_ctx c; md5_init(&c); md5_update(&c,d,n); md5_final(&c,out); }
static void to_hex(const unsigned char* in, size_t n, char* out){ static const char* h="0123456789abcdef";
    for (size_t i=0;i<n;i++){ out[i*2]=h[in[i]>>4]; out[i*2+1]=h[in[i]&15]; } out[n*2]=0; }

/* postgres md5: "md5" + MD5( MD5(password + user) + 4-byte salt ) en hex */
static void pg_md5(const char* user,const char* pass,const unsigned char salt[4], char out[36]){
    unsigned char d1[16], d2[16]; char hex1[33];
    size_t l1=strlen(pass), l2=strlen(user);
    char* tmp=(char*)malloc(l1+l2+1); memcpy(tmp,pass,l1); memcpy(tmp+l1,user,l2); tmp[l1+l2]=0;
    md5(tmp,l1+l2,d1); free(tmp);
    to_hex(d1,16,hex1);
    unsigned char buf[32+4]; memcpy(buf,hex1,32); memcpy(buf+32,salt,4);
    md5(buf,36,d2);
    strcpy(out,"md5"); to_hex(d2,16,out+3);
}

/* ==================== Protocole ==================== */

typedef struct {
    sock_t s;
} pg_conn;

typedef struct {
    pg_conn c;
} pg_client;

/* Envoie StartupMessage v3 */
static int pg_startup(pg_conn* c, const char* db, const char* user){
    /* body: int32 protocol(196608) + key/values + 0 */
    unsigned char body[256]; size_t i=0;
    uint32_t proto = 196608; /* 3.0 */
    memcpy(body+i,&proto,4); i+=4;
    #define KV(k,v) do{ size_t lk=strlen(k)+1, lv=strlen(v)+1; if (i+lk+lv>=sizeof body) return -1; memcpy(body+i,k,lk); i+=lk; memcpy(body+i,v,lv); i+=lv; }while(0)
    KV("user", user?user:"");
    if (db && *db) KV("database", db);
    KV("client_encoding","UTF8");
    #undef KV
    body[i++]=0;

    uint32_t len = (uint32_t)(4 /*len*/ + i);
    unsigned char pkt[4]; *(uint32_t*)pkt = be32(len);
    if (io_write_all(c->s,pkt,4)!=0 || io_write_all(c->s,body,i)!=0) return -1;
    return 0;
}

/* Lit un message serveur: type(1) + len(4). Place tout dans buf (cap>=len-4). */
static int pg_read_msg(pg_conn* c, char* type, unsigned char* buf, uint32_t cap, uint32_t* out_len){
    if (io_read_all(c->s,type,1)!=0) return -1;
    unsigned char l4[4]; if (io_read_all(c->s,l4,4)!=0) return -1;
    uint32_t len = rd_be32(l4); /* longueur totale incluant ces 4 octets */
    if (len<4) return -1;
    uint32_t blen = len-4;
    if (blen>cap) return -1;
    if (io_read_all(c->s,buf,blen)!=0) return -1;
    if (out_len) *out_len = blen;
    return 0;
}

/* Auth: gère AuthenticationOk(0), Cleartext(3), MD5(5). */
static int pg_auth(pg_conn* c, const char* user, const char* pass){
    while (1){
        char t; unsigned char b[1024]; uint32_t n=0;
        if (pg_read_msg(c,&t,b,sizeof b,&n)!=0) return -1;
        if (t=='R'){ /* Authentication */
            if (n<4) return -1;
            uint32_t code = rd_be32(b);
            if (code==0){ /* OK */ continue; }
            else if (code==3){ /* Cleartext */
                size_t lp = pass?strlen(pass):0;
                uint32_t len = (uint32_t)(1 + 4 + lp + 1);
                unsigned char* pkt = (unsigned char*)malloc(len);
                pkt[0]='p'; *(uint32_t*)(pkt+1)=be32((uint32_t)(len-1));
                if (lp) memcpy(pkt+5, pass, lp); pkt[5+lp]=0;
                int rc = io_write_all(c->s, pkt, len); free(pkt); if (rc!=0) return -1;
            } else if (code==5){ /* MD5 */
                if (n<8) return -1; const unsigned char* salt=b+4;
                char md5txt[36]; pg_md5(user?user:"", pass?pass:"", salt, md5txt);
                size_t lm=strlen(md5txt);
                uint32_t len = (uint32_t)(1 + 4 + lm + 1);
                unsigned char* pkt=(unsigned char*)malloc(len);
                pkt[0]='p'; *(uint32_t*)(pkt+1)=be32((uint32_t)(len-1));
                memcpy(pkt+5, md5txt, lm); pkt[5+lm]=0;
                int rc=io_write_all(c->s,pkt,len); free(pkt); if (rc!=0) return -1;
            } else {
                return -1; /* autres méthodes non gérées */
            }
        } else if (t=='S' || t=='K'){ /* ParameterStatus / BackendKeyData -> ignorer */
            continue;
        } else if (t=='E'){ /* ErrorResponse */
            return -1;
        } else if (t=='Z'){ /* ReadyForQuery */
            return 0;
        } else {
            /* ignorer autres messages jusqu'à ReadyForQuery */
            continue;
        }
    }
}

/* ==================== API ==================== */

PGC_API int pg_connect(pg_client* cli,
                       const char* host, const char* port,
                       const char* db, const char* user, const char* pass){
    memset(cli,0,sizeof *cli);
    cli->c.s = tcp_connect(host, port, 5000);
    if (cli->c.s==INVALID_SOCKET) return -1;
    if (pg_startup(&cli->c, db, user)!=0) { CLOSESOCK(cli->c.s); _net_shutdown(); return -1; }
    if (pg_auth(&cli->c, user, pass)!=0){ CLOSESOCK(cli->c.s); _net_shutdown(); return -1; }
    return 0;
}

PGC_API void pg_close(pg_client* cli){
    if (!cli) return;
    if (cli->c.s!=INVALID_SOCKET){
        /* Terminate: 'X' + len(4)=4 */
        unsigned char pkt[5]; pkt[0]='X'; *(uint32_t*)(pkt+1)=be32(4);
        io_write_all(cli->c.s,pkt,5);
        CLOSESOCK(cli->c.s);
    }
    _net_shutdown();
}

/* Envoie Query et lit jusqu’à ReadyForQuery. Affiche CSV si print_rows!=0. */
PGC_API int pg_query(pg_client* cli, const char* sql, int print_rows){
    if (!cli || cli->c.s==INVALID_SOCKET) return -1;
    size_t qs = strlen(sql?sql:"");
    uint32_t len = (uint32_t)(1 + 4 + qs + 1);
    unsigned char* pkt = (unsigned char*)malloc(len);
    pkt[0]='Q'; *(uint32_t*)(pkt+1)=be32((uint32_t)(len-1));
    if (qs) memcpy(pkt+5, sql, qs); pkt[5+qs]=0;
    if (io_write_all(cli->c.s,pkt,len)!=0){ free(pkt); return -1; }
    free(pkt);

    /* colonnes pour CSV */
    char* colnames[256]; int ncols=0;

    while (1){
        char t; unsigned char b[8192]; uint32_t n=0;
        if (pg_read_msg(&cli->c,&t,b,sizeof b,&n)!=0) return -1;
        if (t=='T'){ /* RowDescription */
            if (n<2) return -1;
            ncols = rd_be16(b); if (ncols>256) return -1;
            const unsigned char* p=b+2;
            for (int i=0;i<ncols;i++){
                const char* name=(const char*)p; size_t ln=strlen(name); colnames[i]=(char*)name;
                p += ln+1;
                if (p+18 > b+n) return -1; /* table oid,int16 attnum, type oid,int16 size,int32 mod,int16 fmt */
                p += 18;
            }
            if (print_rows){
                for (int i=0;i<ncols;i++){ if (i) putchar(','); fputs(colnames[i], stdout); }
                putchar('\n');
            }
        } else if (t=='D'){ /* DataRow */
            if (n<2) return -1;
            int nf = rd_be16(b); const unsigned char* p=b+2;
            for (int i=0;i<nf;i++){
                if (p+4>b+n) return -1;
                int32_t ln = (int32_t)rd_be32(p); p+=4;
                if (i) putchar(',');
                if (ln<0){ fputs("NULL",stdout); }
                else {
                    if (p+ln>b+n) return -1;
                    if (print_rows) fwrite(p,1,(size_t)ln,stdout);
                    p += ln;
                }
            }
            if (print_rows) putchar('\n');
        } else if (t=='C'){ /* CommandComplete */ continue; }
        else if (t=='E'){ /* ErrorResponse */ return -1; }
        else if (t=='Z'){ /* ReadyForQuery */ break; }
        else { /* ignorer */ continue; }
    }
    return 0;
}

/* ==================== Test ==================== */
#ifdef PG_TEST
int main(int argc, char** argv){
    if (argc<6){
        fprintf(stderr,"usage: %s host port db user pass [sql]\n", argv[0]);
        return 2;
    }
    const char* host=argv[1], *port=argv[2], *db=argv[3], *user=argv[4], *pass=argv[5];
    const char* sql = (argc>=7)? argv[6] : "select 1";
    pg_client c;
    if (pg_connect(&c, host, port, db, user, pass)!=0){ fprintf(stderr,"connect failed\n"); return 1; }
    if (pg_query(&c, sql, 1)!=0){ fprintf(stderr,"query failed\n"); pg_close(&c); return 1; }
    pg_close(&c);
    return 0;
}
#endif