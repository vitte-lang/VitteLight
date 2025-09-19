// SPDX-License-Identifier: GPL-3.0-or-later
//
// net.c — Helpers réseau portables (C17, IPv4/IPv6)
// Namespace: "net"
//
// Plateformes: Linux/macOS (POSIX), Windows (Winsock2)
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c net.c
//
// Fournit:
//   Init/fin:          net_init(), net_shutdown()
//   TCP client:        net_tcp_connect(host, port, timeout_ms)
//   TCP serveur:       net_tcp_listen(bind_host, port, backlog), net_tcp_accept(ls, ip, iplen, port_out)
//   UDP:               net_udp_socket(bind_host, port), net_udp_sendto(s,buf,n,host,port), net_udp_recvfrom(...)
//   I/O utilitaires:   net_set_nonblock(s,1/0), net_send_all(s,buf,n), net_recv_all(s,buf,n)
//   Close:             net_close(s)
//   Adresses:          net_sockname(s,ip,iplen,port), net_peername(s,ip,iplen,port)
//   HTTP GET minimal:  net_http_get(url, hdr_opt, body_buf, cap, out_len, status_out)
//
// Notes:
//   - timeouts via SO_RCVTIMEO/SO_SNDTIMEO (ms). Non-bloquant possible.
//   - net_http_get: simple, pas de redirections, pas de TLS.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  typedef SOCKET net_sock;
  #define NET_INVALID INVALID_SOCKET
  #define net_errno() WSAGetLastError()
  #define net_close(s) closesocket(s)
  #define socklen_cast int
  static int _net_wsa = 0;
#else
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  typedef int net_sock;
  #define NET_INVALID (-1)
  #define net_errno() errno
  #define net_close(s) close(s)
  #define socklen_cast socklen_t
#endif

#ifndef NET_API
#define NET_API
#endif

/* ========================= Init / Shutdown ========================= */

NET_API int net_init(void){
#if defined(_WIN32)
    if (_net_wsa) return 0;
    WSADATA w; int rc = WSAStartup(MAKEWORD(2,2), &w);
    if (rc==0) _net_wsa=1;
    return rc? -1:0;
#else
    return 0;
#endif
}
NET_API void net_shutdown(void){
#if defined(_WIN32)
    if (_net_wsa){ WSACleanup(); _net_wsa=0; }
#endif
}

/* ========================= Options / util ========================= */

NET_API int net_set_nonblock(net_sock s, int nb){
#if defined(_WIN32)
    u_long m = nb?1:0; return ioctlsocket(s, FIONBIO, &m)==0?0:-1;
#else
    int fl = fcntl(s, F_GETFL, 0); if (fl<0) return -1;
    if (nb) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
    return fcntl(s, F_SETFL, fl)==0?0:-1;
#endif
}

NET_API int net_set_timeout_ms(net_sock s, int rcv_ms, int snd_ms){
#if defined(_WIN32)
    if (rcv_ms>=0){ DWORD t=(DWORD)rcv_ms; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&t,sizeof t); }
    if (snd_ms>=0){ DWORD t=(DWORD)snd_ms; setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&t,sizeof t); }
#else
    if (rcv_ms>=0){ struct timeval tv={rcv_ms/1000,(rcv_ms%1000)*1000}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv); }
    if (snd_ms>=0){ struct timeval tv={snd_ms/1000,(snd_ms%1000)*1000}; setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv); }
#endif
    return 0;
}

NET_API int net_send_all(net_sock s, const void* buf, size_t n){
    const unsigned char* p = (const unsigned char*)buf;
    while (n){
#if defined(_WIN32)
        int k = send(s, (const char*)p, (int)((n>0x7fffffff)?0x7fffffff:n), 0);
#else
        ssize_t k = send(s, p, n, 0);
#endif
        if (k<=0) return -1;
        p += (size_t)k; n -= (size_t)k;
    }
    return 0;
}
NET_API int net_recv_all(net_sock s, void* buf, size_t n){
    unsigned char* p = (unsigned char*)buf;
    while (n){
#if defined(_WIN32)
        int k = recv(s, (char*)p, (int)((n>0x7fffffff)?0x7fffffff:n), 0);
#else
        ssize_t k = recv(s, p, n, 0);
#endif
        if (k<=0) return -1;
        p += (size_t)k; n -= (size_t)k;
    }
    return 0;
}

/* ========================= Connexion TCP ========================= */

NET_API net_sock net_tcp_connect(const char* host, const char* port, int timeout_ms){
    if (net_init()!=0) return NET_INVALID;
    struct addrinfo hints; memset(&hints,0,sizeof hints);
    hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    struct addrinfo* res=NULL;
    if (getaddrinfo(host, port, &hints, &res)!=0) return NET_INVALID;

    net_sock s = NET_INVALID;
    for (struct addrinfo* it=res; it; it=it->ai_next){
        s = (net_sock)socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s==NET_INVALID) continue;
        if (timeout_ms>0) net_set_timeout_ms(s, timeout_ms, timeout_ms);
        if (connect(s, it->ai_addr, (socklen_cast)it->ai_addrlen)==0) break;
        net_close(s); s = NET_INVALID;
    }
    freeaddrinfo(res);
    return s;
}

/* ========================= Serveur TCP ========================= */

NET_API net_sock net_tcp_listen(const char* bind_host, const char* port, int backlog){
    if (net_init()!=0) return NET_INVALID;
    struct addrinfo hints; memset(&hints,0,sizeof hints);
    hints.ai_socktype=SOCK_STREAM; hints.ai_family=AF_UNSPEC; hints.ai_flags=AI_PASSIVE;
    struct addrinfo* res=NULL;
    if (getaddrinfo(bind_host, port, &hints, &res)!=0) return NET_INVALID;
    net_sock s=NET_INVALID;
    for (struct addrinfo* it=res; it; it=it->ai_next){
        s=(net_sock)socket(it->ai_family,it->ai_socktype,it->ai_protocol);
        if (s==NET_INVALID) continue;
        int on=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(const char*)&on,sizeof on);
#if defined(SO_REUSEPORT)
        setsockopt(s,SOL_SOCKET,SO_REUSEPORT,(const char*)&on,sizeof on);
#endif
        if (bind(s,it->ai_addr,(socklen_cast)it->ai_addrlen)==0 && listen(s, backlog>0?backlog:16)==0) break;
        net_close(s); s=NET_INVALID;
    }
    freeaddrinfo(res);
    return s;
}

NET_API net_sock net_tcp_accept(net_sock ls, char* ip, size_t iplen, uint16_t* port_out){
    struct sockaddr_storage sa; socklen_cast sl = (socklen_cast)sizeof sa;
    net_sock s = (net_sock)accept(ls, (struct sockaddr*)&sa, &sl);
    if (s==NET_INVALID) return NET_INVALID;
    if (ip || port_out){
        char h[NI_MAXHOST], p[NI_MAXSERV];
        if (getnameinfo((struct sockaddr*)&sa, sl, h, sizeof h, p, sizeof p, NI_NUMERICHOST|NI_NUMERICSERV)==0){
            if (ip && iplen){ strncpy(ip,h,iplen); if (iplen) ip[iplen-1]=0; }
            if (port_out) *port_out=(uint16_t)atoi(p);
        }
    }
    return s;
}

/* ========================= UDP ========================= */

NET_API net_sock net_udp_socket(const char* bind_host, const char* port){
    if (net_init()!=0) return NET_INVALID;
    struct addrinfo hints; memset(&hints,0,sizeof hints);
    hints.ai_socktype=SOCK_DGRAM; hints.ai_family=AF_UNSPEC; hints.ai_flags = bind_host||port ? AI_PASSIVE : 0;
    struct addrinfo* res=NULL;
    if (getaddrinfo(bind_host, port, &hints, &res)!=0) return NET_INVALID;
    net_sock s=NET_INVALID;
    for (struct addrinfo* it=res; it; it=it->ai_next){
        s=(net_sock)socket(it->ai_family,it->ai_socktype,it->ai_protocol);
        if (s==NET_INVALID) continue;
        int on=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(const char*)&on,sizeof on);
        if (!bind_host && !port) break;
        if (bind(s,it->ai_addr,(socklen_cast)it->ai_addrlen)==0) break;
        net_close(s); s=NET_INVALID;
    }
    freeaddrinfo(res);
    return s;
}

NET_API int net_udp_sendto(net_sock s, const void* buf, size_t n, const char* host, const char* port){
    struct addrinfo hints; memset(&hints,0,sizeof hints);
    hints.ai_socktype=SOCK_DGRAM; hints.ai_family=AF_UNSPEC;
    struct addrinfo* res=NULL;
    if (getaddrinfo(host,port,&hints,&res)!=0) return -1;
    int rc=-1;
    for (struct addrinfo* it=res; it; it=it->ai_next){
#if defined(_WIN32)
        int k = sendto(s,(const char*)buf,(int)n,0,it->ai_addr,(socklen_cast)it->ai_addrlen);
#else
        ssize_t k = sendto(s,buf,n,0,it->ai_addr,(socklen_cast)it->ai_addrlen);
#endif
        if (k==(int)n){ rc=0; break; }
    }
    freeaddrinfo(res);
    return rc;
}

NET_API int net_udp_recvfrom(net_sock s, void* buf, size_t cap, char* ip, size_t iplen, uint16_t* port_out){
    struct sockaddr_storage sa; socklen_cast sl=(socklen_cast)sizeof sa;
#if defined(_WIN32)
    int k = recvfrom(s,(char*)buf,(int)cap,0,(struct sockaddr*)&sa,&sl);
#else
    ssize_t k = recvfrom(s,buf,cap,0,(struct sockaddr*)&sa,&sl);
#endif
    if (k<=0) return -1;
    if (ip || port_out){
        char h[NI_MAXHOST], p[NI_MAXSERV];
        if (getnameinfo((struct sockaddr*)&sa, sl, h,sizeof h, p,sizeof p, NI_NUMERICHOST|NI_NUMERICSERV)==0){
            if (ip && iplen){ strncpy(ip,h,iplen); if (iplen) ip[iplen-1]=0; }
            if (port_out) *port_out=(uint16_t)atoi(p);
        }
    }
    return (int)k;
}

/* ========================= Infos socket ========================= */

NET_API int net_sockname(net_sock s, char* ip, size_t iplen, uint16_t* port){
    struct sockaddr_storage sa; socklen_cast sl=(socklen_cast)sizeof sa;
    if (getsockname(s,(struct sockaddr*)&sa,&sl)!=0) return -1;
    char h[NI_MAXHOST], p[NI_MAXSERV];
    if (getnameinfo((struct sockaddr*)&sa, sl, h,sizeof h, p,sizeof p, NI_NUMERICHOST|NI_NUMERICSERV)!=0) return -1;
    if (ip && iplen){ strncpy(ip,h,iplen); if (iplen) ip[iplen-1]=0; }
    if (port) *port=(uint16_t)atoi(p);
    return 0;
}
NET_API int net_peername(net_sock s, char* ip, size_t iplen, uint16_t* port){
    struct sockaddr_storage sa; socklen_cast sl=(socklen_cast)sizeof sa;
    if (getpeername(s,(struct sockaddr*)&sa,&sl)!=0) return -1;
    char h[NI_MAXHOST], p[NI_MAXSERV];
    if (getnameinfo((struct sockaddr*)&sa, sl, h,sizeof h, p,sizeof p, NI_NUMERICHOST|NI_NUMERICSERV)!=0) return -1;
    if (ip && iplen){ strncpy(ip,h,iplen); if (iplen) ip[iplen-1]=0; }
    if (port) *port=(uint16_t)atoi(p);
    return 0;
}

/* ========================= HTTP GET minimal ========================= */

static int _url_split(const char* url, char* host, size_t hostcap, char* port, size_t portcap,
                      char* path, size_t pathcap){
    /* supporte: http://host[:port]/path    et host[:port]/path (sans schéma) */
    const char* p=url;
    if (!p) return -1;
    if (!strncmp(p,"http://",7)) p += 7;
    const char* slash = strchr(p,'/');
    const char* hp_end = slash ? slash : p + strlen(p);
    const char* colon = NULL;
    for (const char* q=p; q<hp_end; ++q){ if (*q==':'){ colon=q; break; } }
    if (colon){
        size_t hn=(size_t)(colon-p); if (hn>=hostcap) return -1;
        memcpy(host,p,hn); host[hn]=0;
        size_t pn=(size_t)(hp_end-(colon+1)); if (pn>=portcap) return -1;
        memcpy(port,colon+1,pn); port[pn]=0;
    } else {
        size_t hn=(size_t)(hp_end-p); if (hn>=hostcap) return -1;
        memcpy(host,p,hn); host[hn]=0;
        snprintf(port,portcap,"80");
    }
    if (slash){
        size_t kn=strlen(slash); if (kn>=pathcap) return -1;
        memcpy(path,slash,kn+1);
    } else {
        if (pathcap<2) return -1; path[0]='/'; path[1]=0;
    }
    return 0;
}

NET_API int net_http_get(const char* url, const char* extra_headers,
                         char* body_out, size_t body_cap, size_t* body_len, int* status_out){
    char host[256], port[16], path[1024];
    if (_url_split(url,host,sizeof host,port,sizeof port,path,sizeof path)!=0) return -1;
    net_sock s = net_tcp_connect(host, port, 5000);
    if (s==NET_INVALID) return -1;

    char req[2048];
    int n = snprintf(req,sizeof req,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: VitteLight/1\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        path, host, extra_headers?extra_headers:"");
    if (n<0 || (size_t)n>=sizeof req){ net_close(s); return -1; }
    if (net_send_all(s, req, (size_t)n)!=0){ net_close(s); return -1; }

    /* lire la réponse entière en mémoire (troncature si body_cap insuffisant) */
    char line[1024];
    size_t li=0; int hdr_done=0; int status=0;
    /* lecture simple octet par octet des headers jusqu'à \r\n\r\n */
    while (!hdr_done){
        char c; int k =
#if defined(_WIN32)
            recv(s,&c,1,0);
#else
            (int)recv(s,&c,1,0);
#endif
        if (k<=0){ net_close(s); return -1; }
        if (li < sizeof line - 1) line[li++]=c;
        if (li>=4 && line[li-4]=='\r'&&line[li-3]=='\n'&&line[li-2]=='\r'&&line[li-1]=='\n'){
            /* première ligne contient le code */
            line[li]=0;
            const char* sp=strchr(line,' '); status = sp? atoi(sp+1):0;
            hdr_done=1;
        }
    }
    if (status_out) *status_out=status;

    size_t outn=0;
    while (1){
        char buf[4096];
#if defined(_WIN32)
        int k = recv(s,buf,sizeof buf,0);
#else
        int k = (int)recv(s,buf,sizeof buf,0);
#endif
        if (k<=0) break;
        size_t can = (outn + (size_t)k <= body_cap)? (size_t)k : (body_cap - outn);
        if (can) { memcpy(body_out+outn, buf, can); outn += can; }
    }
    if (body_len) *body_len = outn;
    if (body_cap) body_out[outn < body_cap ? outn : body_cap-1] = 0;
    net_close(s);
    return 0;
}

/* ========================= Test ========================= */
#ifdef NET_TEST
int main(int argc, char** argv){
    (void)argc; (void)argv;
    if (net_init()!=0){ fprintf(stderr,"net_init fail\n"); return 1; }

    /* HTTP */
    char body[8192]; size_t n=0; int st=0;
    if (net_http_get("http://example.org/", NULL, body, sizeof body, &n, &st)==0){
        printf("HTTP %d, %zu bytes\n", st, n);
        fwrite(body,1,n,stdout); putchar('\n');
    } else {
        fprintf(stderr,"http fail\n");
    }

    net_shutdown();
    return 0;
}
#endif