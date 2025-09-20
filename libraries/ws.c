// SPDX-License-Identifier: GPL-3.0-or-later
//
// ws.c — Client WebSocket minimal, portable (C17)
// Namespace: "ws"
//
// Build (POSIX):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c ws.c
//
// Build (TLS via OpenSSL):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DHAVE_OPENSSL -c ws.c \
//      -lssl -lcrypto
//
// Build (Windows):
//   cl /std:c17 /O2 ws.c ws2_32.lib
//
// API (client):
//   typedef struct ws_client ws_client;
//   ws_client* ws_connect(const char* url, int timeout_ms);            // ws://... | wss://...
//   void       ws_close(ws_client* c, uint16_t code, const char* reason);
//   void       ws_free(ws_client* c);
//   int        ws_send_text(ws_client* c, const void* buf, size_t n);  // 0 ok
//   int        ws_send_bin (ws_client* c, const void* buf, size_t n);  // 0 ok
//   int        ws_recv(ws_client* c, void* out, size_t cap, size_t* out_n, int* fin);
//   int        ws_ping(ws_client* c, const void* data, size_t n);      // 0 ok
//   int        ws_pong(ws_client* c, const void* data, size_t n);      // 0 ok

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET ws_sock_t;
  #define WS_CLOSESOCK(s) closesocket(s)
  #define WS_SOCKERR() WSAGetLastError()
  #define WS_EWOULDBLOCK WSAEWOULDBLOCK
  static int ws__winsock_once(void){ static int inited=0; if(!inited){ WSADATA w; if(WSAStartup(MAKEWORD(2,2),&w)!=0) return -1; inited=1;} return 0; }
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  typedef int ws_sock_t;
  #define WS_CLOSESOCK(s) close(s)
  #define WS_SOCKERR() errno
  #define WS_EWOULDBLOCK EWOULDBLOCK
  static int ws__winsock_once(void){ return 0; }
#endif

#if defined(HAVE_OPENSSL)
  #include <openssl/ssl.h>
  #include <openssl/err.h>
  #include <openssl/sha.h>
#endif

/* ======================= small helpers ======================= */

static const char* ws_strcasestr(const char* h, const char* n){
    if(!h||!n||!*n) return (char*)h;
    size_t nl=strlen(n);
    for(const char* p=h; *p; p++){
        size_t i=0;
        while(i<nl){
            int a=(unsigned char)p[i];
            int b=(unsigned char)n[i];
            if ('A'<=a && a<='Z') a+=32;
            if ('A'<=b && b<='Z') b+=32;
            if (a!=b) break;
            i++;
        }
        if (i==nl) return p;
    }
    return NULL;
}

/* ================= Mini SHA1 + Base64 (always enabled) ================= */

typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; size_t off; } ws_sha1_t;
static uint32_t ws__rol(uint32_t x,int s){ return (x<<s)|(x>>(32-s)); }
static void ws_sha1_init(ws_sha1_t* s){ s->h[0]=0x67452301u; s->h[1]=0xEFCDAB89u; s->h[2]=0x98BADCFEu; s->h[3]=0x10325476u; s->h[4]=0xC3D2E1F0u; s->len=0; s->off=0; }
static void ws_sha1_blk(ws_sha1_t* s,const unsigned char b[64]){
  uint32_t w[80];
  for(int i=0;i<16;i++) w[i]=(b[4*i]<<24)|(b[4*i+1]<<16)|(b[4*i+2]<<8)|(b[4*i+3]);
  for(int i=16;i<80;i++) w[i]=ws__rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
  uint32_t a=s->h[0],b0=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
  for(int i=0;i<80;i++){
    uint32_t f,k;
    if(i<20){ f=(b0&c)|((~b0)&d); k=0x5A827999u; }
    else if(i<40){ f=b0^c^d; k=0x6ED9EBA1u; }
    else if(i<60){ f=(b0&c)|(b0&d)|(c&d); k=0x8F1BBCDCu; }
    else{ f=b0^c^d; k=0xCA62C1D6u; }
    uint32_t t=ws__rol(a,5)+f+e+k+w[i];
    e=d; d=c; c=ws__rol(b0,30); b0=a; a=t;
  }
  s->h[0]+=a; s->h[1]+=b0; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static void ws_sha1_upd(ws_sha1_t* s,const void* data,size_t n){
  const unsigned char* p=(const unsigned char*)data; s->len+=n*8ull;
  while(n){
    size_t t=64-s->off; if(t>n) t=n;
    memcpy(s->buf+s->off,p,t); s->off+=t; p+=t; n-=t;
    if(s->off==64){ ws_sha1_blk(s,s->buf); s->off=0; }
  }
}
static void ws_sha1_fin(ws_sha1_t* s,unsigned char out[20]){
  unsigned char pad[64]={0x80}; size_t padlen=(s->off<56)?(56-s->off):(56+64-s->off);
  ws_sha1_upd(s,pad,padlen);
  unsigned char lb[8]; for(int i=0;i<8;i++) lb[7-i]=(unsigned char)((s->len>>(8*i))&0xFFu);
  ws_sha1_upd(s,lb,8);
  for(int i=0;i<5;i++){ out[4*i]=(unsigned char)(s->h[i]>>24); out[4*i+1]=(unsigned char)(s->h[i]>>16); out[4*i+2]=(unsigned char)(s->h[i]>>8); out[4*i+3]=(unsigned char)(s->h[i]); }
}

static int ws_b64(const unsigned char* src,size_t n,char* out,size_t cap){
  static const char* T="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t w=0; for(size_t i=0;i<n;i+=3){
    unsigned v=(src[i]<<16)|((i+1<n?src[i+1]:0)<<8)|(i+2<n?src[i+2]:0);
    char c0=T[(v>>18)&63], c1=T[(v>>12)&63], c2=(i+1<n)?T[(v>>6)&63]:'=', c3=(i+2<n)?T[v&63]:'=';
    if(w+4>=cap) return -1; out[w++]=c0; out[w++]=c1; out[w++]=c2; out[w++]=c3;
  }
  if(w<cap) out[w]=0; return (int)w;
}

/* ================= URL parse (ws://host[:port]/path) ================= */

typedef struct { int tls; char host[256]; char path[1024]; char portstr[8]; } ws_url;
static int ws_parse_url(const char* s, ws_url* u){
  memset(u,0,sizeof *u); u->tls=0; strcpy(u->portstr,"80");
  if (strncmp(s,"ws://",5)==0) s+=5;
  else if (strncmp(s,"wss://",6)==0){ u->tls=1; strcpy(u->portstr,"443"); s+=6; }
  else return -1;
  const char* p=s; const char* host_b=p;
  while(*p && *p!='/' && *p!=':') p++;
  size_t hl=(size_t)(p-host_b); if(hl==0||hl>=sizeof u->host) return -1;
  memcpy(u->host,host_b,hl); u->host[hl]=0;
  if (*p==':'){ p++; const char* pb=p; while(*p && *p!='/') p++; size_t pl=(size_t)(p-pb); if(pl==0||pl>=sizeof u->portstr) return -1; memcpy(u->portstr,pb,pl); u->portstr[pl]=0; }
  if (*p=='/') strncpy(u->path,p,sizeof u->path-1); else strcpy(u->path,"/");
  return 0;
}

/* ================= Transport abstraction ================= */

typedef struct {
  ws_sock_t s;
  int is_tls;
#if defined(HAVE_OPENSSL)
  SSL_CTX* ctx;
  SSL*     ssl;
#endif
} ws_io;

static int ws_set_nonblock(ws_sock_t s,int nb){
#if defined(_WIN32)
  u_long m = nb?1:0; return ioctlsocket(s, FIONBIO, &m)==0?0:-1;
#else
  int fl=fcntl(s,F_GETFL,0); if(fl<0) return -1;
  return fcntl(s,F_SETFL, nb? (fl|O_NONBLOCK) : (fl & ~O_NONBLOCK))==0?0:-1;
#endif
}

static int ws_connect_tcp(ws_io* io, const char* host, const char* port, int timeout_ms){
  if (ws__winsock_once()!=0) return -1;
  struct addrinfo hints; memset(&hints,0,sizeof hints); hints.ai_socktype=SOCK_STREAM; hints.ai_family=AF_UNSPEC;
  struct addrinfo* ai=NULL;
  if (getaddrinfo(host,port,&hints,&ai)!=0) return -1;
  int rc=-1;
  for (struct addrinfo* p=ai; p; p=p->ai_next){
    ws_sock_t s = (ws_sock_t)socket(p->ai_family,p->ai_socktype,p->ai_protocol);
#if defined(_WIN32)
    if (s==INVALID_SOCKET) continue;
#else
    if (s<0) continue;
#endif
    ws_set_nonblock(s,1);
    int c = connect(s,p->ai_addr,(socklen_t)p->ai_addrlen);
#if defined(_WIN32)
    int inprog = (c==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK);
#else
    int inprog = (c<0 && (errno==EINPROGRESS));
#endif
    if (c==0 || inprog){
      fd_set wf; FD_ZERO(&wf); FD_SET(s,&wf);
      struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
      int sel = select((int)(s+1),NULL,&wf,NULL,&tv);
      if (sel==1){
        int err=0; socklen_t el=sizeof err;
#if defined(_WIN32)
        getsockopt(s,SOL_SOCKET,SO_ERROR,(char*)&err,&el);
#else
        getsockopt(s,SOL_SOCKET,SO_ERROR,(void*)&err,&el);
#endif
        if (err==0){ io->s=s; rc=0; break; }
      }
    }
    WS_CLOSESOCK(s);
  }
  freeaddrinfo(ai);
  ws_set_nonblock(io->s,0);
  return rc;
}

#if defined(HAVE_OPENSSL)
static int ws_tls_wrap(ws_io* io, const char* host){
  SSL_load_error_strings(); OpenSSL_add_ssl_algorithms();
  io->ctx = SSL_CTX_new(TLS_client_method());
  if(!io->ctx) return -1;
  io->ssl = SSL_new(io->ctx);
  if(!io->ssl) return -1;
  SSL_set_tlsext_host_name(io->ssl, host);
  SSL_set_fd(io->ssl, (int)io->s);
  if (SSL_connect(io->ssl) <= 0) return -1;
  return 0;
}
#endif

static int ws_iowrite(ws_io* io, const void* b, size_t n){
#if defined(HAVE_OPENSSL)
  if (io->is_tls) return (int)SSL_write(io->ssl, b, (int)n);
#endif
#if defined(_WIN32)
  return (int)send(io->s, (const char*)b, (int)n, 0);
#else
  return (int)send(io->s, b, n, 0);
#endif
}
static int ws_ioread(ws_io* io, void* b, size_t n){
#if defined(HAVE_OPENSSL)
  if (io->is_tls) return (int)SSL_read(io->ssl, b, (int)n);
#endif
#if defined(_WIN32)
  return (int)recv(io->s, (char*)b, (int)n, 0);
#else
  return (int)recv(io->s, b, n, 0);
#endif
}
static void ws_io_close(ws_io* io){
  if (!io) return;
#if defined(HAVE_OPENSSL)
  if (io->ssl){ SSL_shutdown(io->ssl); SSL_free(io->ssl); io->ssl=NULL; }
  if (io->ctx){ SSL_CTX_free(io->ctx); io->ctx=NULL; }
#endif
#if defined(_WIN32)
  if (io->s!=INVALID_SOCKET) WS_CLOSESOCK(io->s);
#else
  if (io->s>=0) WS_CLOSESOCK(io->s);
#endif
  memset(io,0,sizeof *io);
}

/* ================= Client ================= */

typedef struct ws_client {
  ws_io   io;
  int     open;
  char    key_b64[32];
} ws_client;

static uint32_t ws__rand32(void){
  uint32_t x=(uint32_t)time(NULL);
  x ^= (uint32_t)(uintptr_t)&x;
#if defined(_WIN32)
  x ^= (uint32_t)GetCurrentProcessId();
#else
  x ^= (uint32_t)getpid();
#endif
  x = x*1664525u + 1013904223u;
  return x;
}

static int ws_handshake(ws_client* c, const char* host, const char* path){
  unsigned char rnd[16];
  for (int i=0;i<4;i++){ uint32_t r=ws__rand32(); memcpy(rnd+4*i,&r,4); }
  if (ws_b64(rnd,16,c->key_b64,sizeof c->key_b64)<0) return -1;

  char req[2048];
  int n = snprintf(req,sizeof req,
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n", path, host, c->key_b64);
  if (n<=0 || (size_t)n>=sizeof req) return -1;

  if (ws_iowrite(&c->io, req, (size_t)n) != n) return -1;

  /* Lire réponse jusqu'à CRLF CRLF */
  char buf[4096]; int got=0;
  for(;;){
    int r = ws_ioread(&c->io, buf+got, sizeof buf - 1 - got);
    if (r<=0) return -1;
    got += r; buf[got]=0;
    if (strstr(buf,"\r\n\r\n")) break;
    if ((size_t)got >= sizeof buf - 1) return -1;
  }
  if (strncmp(buf,"HTTP/1.1 101",12)!=0 && strncmp(buf,"HTTP/1.0 101",12)!=0) return -1;

  const char* acc = ws_strcasestr(buf, "Sec-WebSocket-Accept:");
  if (!acc) return -1;

  static const char GUID[]="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char sh[20];
#if defined(HAVE_OPENSSL)
  {
    unsigned char tmp[128];
    int m = snprintf((char*)tmp,sizeof tmp,"%s%s", c->key_b64, GUID);
    if (m<=0 || (size_t)m>=sizeof tmp) return -1;
    SHA1(tmp,(size_t)m, sh);
  }
#else
  ws_sha1_t shc; ws_sha1_init(&shc);
  ws_sha1_upd(&shc, c->key_b64, strlen(c->key_b64));
  ws_sha1_upd(&shc, GUID, strlen(GUID));
  ws_sha1_fin(&shc, sh);
#endif
  char expect[64]; ws_b64(sh,20,expect,sizeof expect);

  const char* v=acc+strlen("Sec-WebSocket-Accept:");
  while(*v==' '||*v=='\t') v++;
  char gotv[64]; size_t j=0;
  while(*v && *v!='\r' && *v!='\n' && j+1<sizeof gotv){ gotv[j++]=*v++; }
  gotv[j]=0;
  while(j>0 && (gotv[j-1]==' '||gotv[j-1]=='\t')) gotv[--j]=0;

  if (strcmp(gotv, expect)!=0) return -1;

  c->open=1; return 0;
}

/* Frame write (FIN=1). Client masking required. */
static int ws_write_frame(ws_client* c, int opcode, const void* payload, size_t n){
  if (!c||!c->open) return -1;
  unsigned char hdr[14]; size_t hl=0;
  hdr[0] = 0x80u | (opcode & 0x0Fu);
  if (n < 126){ hdr[1] = 0x80u | (unsigned char)n; hl=2; }
  else if (n <= 0xFFFFu){ hdr[1]=0x80u | 126; hdr[2]=(unsigned char)(n>>8); hdr[3]=(unsigned char)(n); hl=4; }
  else { hdr[1]=0x80u | 127; for(int i=0;i<8;i++) hdr[2+i]=(unsigned char)((uint64_t)n >> (56 - 8*i)); hl=10; }
  uint32_t m = ws__rand32();
  hdr[hl+0]=(unsigned char)(m>>24);
  hdr[hl+1]=(unsigned char)(m>>16);
  hdr[hl+2]=(unsigned char)(m>>8);
  hdr[hl+3]=(unsigned char)(m);
  hl += 4;

  if (ws_iowrite(&c->io, hdr, hl) != (int)hl) return -1;

  const unsigned char* p=(const unsigned char*)payload;
  unsigned char buf[1024];
  size_t off=0;
  while (off<n){
    size_t chunk = n-off; if (chunk>sizeof buf) chunk=sizeof buf;
    for (size_t i=0;i<chunk;i++) buf[i] = p[off+i] ^ ((unsigned char*)&m)[i&3];
    int w = ws_iowrite(&c->io, buf, chunk);
    if (w!=(int)chunk) return -1;
    off += chunk;
  }
  return 0;
}

static int ws_read_exact(ws_client* c, void* buf, size_t n){
  size_t got=0;
  while (got<n){
    int r = ws_ioread(&c->io, (unsigned char*)buf + got, n-got);
    if (r<=0) return -1;
    got += (size_t)r;
  }
  return 0;
}

/* ================= Public API ================= */

ws_client* ws_connect(const char* url, int timeout_ms){
  if (!url) return NULL;
  ws_url U; if (ws_parse_url(url,&U)!=0) return NULL;

  ws_client* c = (ws_client*)calloc(1,sizeof *c); if(!c) return NULL;
  if (ws_connect_tcp(&c->io, U.host, U.portstr, timeout_ms<=0?5000:timeout_ms)!=0){ free(c); return NULL; }
  c->io.is_tls = U.tls;
#if defined(HAVE_OPENSSL)
  if (U.tls){ if (ws_tls_wrap(&c->io, U.host)!=0){ ws_io_close(&c->io); free(c); return NULL; } }
#else
  if (U.tls){ ws_io_close(&c->io); free(c); return NULL; }
#endif
  if (ws_handshake(c, U.host, U.path)!=0){ ws_io_close(&c->io); free(c); return NULL; }
  return c;
}

void ws_close(ws_client* c, uint16_t code, const char* reason){
  if (!c) return;
  if (c->open){
    unsigned char tmp[2+125]; size_t n=0;
    if (code){ tmp[0]=(unsigned char)(code>>8); tmp[1]=(unsigned char)code; n=2;
      if (reason){ size_t L=strlen(reason); if (L>123) L=123; memcpy(tmp+2, reason, L); n+=L; }
    }
    ws_write_frame(c, 0x8, tmp, n);
  }
  c->open=0;
  ws_io_close(&c->io);
}

void ws_free(ws_client* c){ if(!c) return; ws_close(c, 1000, ""); free(c); }

int ws_send_text(ws_client* c, const void* buf, size_t n){ return ws_write_frame(c, 0x1, buf, n); }
int ws_send_bin (ws_client* c, const void* buf, size_t n){ return ws_write_frame(c, 0x2, buf, n); }
int ws_ping(ws_client* c, const void* data, size_t n){ if(n>125) n=125; return ws_write_frame(c, 0x9, data, n); }
int ws_pong(ws_client* c, const void* data, size_t n){ if(n>125) n=125; return ws_write_frame(c, 0xA, data, n); }

/* Réception d’une trame unique. Retourne opcode (>0) ou <0 erreur. */
int ws_recv(ws_client* c, void* out, size_t cap, size_t* out_n, int* fin){
  if (!c||!c->open) return -1;
  unsigned char h2[2];
  if (ws_read_exact(c, h2, 2)!=0) return -1;
  int FIN = (h2[0] & 0x80) ? 1 : 0;
  int OPC = (h2[0] & 0x0F);
  int MASK= (h2[1] & 0x80) ? 1 : 0; /* serveur ne doit pas masquer */
  uint64_t len = (h2[1] & 0x7F);
  if (len==126){
    unsigned char ex[2]; if (ws_read_exact(c, ex, 2)!=0) return -1;
    len = (uint16_t)(ex[0]<<8 | ex[1]);
  } else if (len==127){
    unsigned char ex[8]; if (ws_read_exact(c, ex, 8)!=0) return -1;
    len = ((uint64_t)ex[0]<<56)|((uint64_t)ex[1]<<48)|((uint64_t)ex[2]<<40)|((uint64_t)ex[3]<<32)|
          ((uint64_t)ex[4]<<24)|((uint64_t)ex[5]<<16)|((uint64_t)ex[6]<<8)|((uint64_t)ex[7]);
  }
  unsigned char mask[4]={0,0,0,0};
  if (MASK){ if (ws_read_exact(c, mask, 4)!=0) return -1; }

  size_t toread = (len>cap)? (size_t)cap : (size_t)len;
  size_t got=0;
  while (got<toread){
    int r = ws_ioread(&c->io, (unsigned char*)out + got, toread-got);
    if (r<=0) return -1;
    got += (size_t)r;
  }
  size_t remain = (size_t)(len - toread);
  while (remain){
    unsigned char dump[1024]; size_t chunk = remain>sizeof dump? sizeof dump : remain;
    int r = ws_ioread(&c->io, dump, chunk);
    if (r<=0) return -1;
    remain -= (size_t)r;
  }
  if (MASK){ for (size_t i=0;i<got;i++) ((unsigned char*)out)[i] ^= mask[i&3]; }
  if (out_n) *out_n=got;
  if (fin) *fin=FIN;

  if (OPC==0x9){ ws_pong(c, out, got); }      /* répondre au ping */
  if (OPC==0x8){ c->open=0; }                 /* close */

  return OPC;
}

/* ================= Demo (WS_TEST) ================= */
#ifdef WS_TEST
int main(int argc, char** argv){
  if (argc<2){ fprintf(stderr,"Usage: %s ws://echo.websocket.events/\n", argv[0]); return 1; }
  ws_client* c = ws_connect(argv[1], 5000);
  if (!c){ fprintf(stderr,"connect fail\n"); return 2; }
  const char* msg="hello";
  if (ws_send_text(c, msg, strlen(msg))!=0){ fprintf(stderr,"send fail\n"); ws_free(c); return 3; }
  char buf[4096]; size_t n=0; int fin=0;
  int op = ws_recv(c, buf, sizeof buf-1, &n, &fin);
  if (op>0){ buf[n]=0; printf("op=%d fin=%d n=%zu msg=%s\n", op, fin, n, buf); }
  ws_close(c,1000,"bye");
  ws_free(c);
  return 0;
}
#endif