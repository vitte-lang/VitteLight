// SPDX-License-Identifier: GPL-3.0-or-later
//
// ipc.c — IPC complet pour Vitte Light VM (C17, POSIX + stubs Windows)
// Namespace: "ipc"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c ipc.c
//
// Modèle:
//   - Primitives bas niveau: pipe, socketpair, UNIX socket, read/write/send/recv.
//   - Serveur UNIX: bind/listen/accept ; Client UNIX: connect.
//   - Réglages fd: non-blocking, close-on-exec, TCP-like shutdown (générique).
//   - Intégrable avec ioloop via fd non-bloquants (aucun couplage direct requis).
//   - Chaînes binaires via vl_push_lstring. Aucune allocation cachée côté VM.
//
// API (VM):
//   r,w          = ipc.pipe([nonblock=0])                         | (nil,errmsg)
//   a,b          = ipc.socketpair([nonblock=0])                    | (nil,errmsg)
//   s            = ipc.unix_listen(path[, backlog=16, unlink=0])   | (nil,errmsg)
//   s            = ipc.unix_connect(path[, nonblock=0])            | (nil,errmsg)
//   c            = ipc.accept(s[, nonblock=1])                     | (nil,errmsg)
//   ok           = ipc.shutdown(fd, how)   -- how: 0=rd,1=wr,2=rw   | (nil,errmsg)
//   n            = ipc.write(fd, data)                              | (nil,errmsg)
//   data         = ipc.read(fd, maxlen)                             | (nil,errmsg)
//   n            = ipc.send(fd, data[, flags=0])                    | (nil,errmsg)
//   data         = ipc.recv(fd, maxlen[, flags=0])                  | (nil,errmsg)
//   ok           = ipc.set_nonblock(fd, on)                         | (nil,errmsg)
//   ok           = ipc.set_cloexec(fd, on)                          | (nil,errmsg)
//   ok           = ipc.close(fd)                                    | (nil,errmsg)
//
// Erreurs retournées: "EINVAL", "ENOSYS", "ENOMEM", "EIO"
//
// Deps VM: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <windows.h>
  #pragma comment(lib, "Ws2_32.lib")
  /* Windows: on simule partiellement avec pipes nommés/sockets WinSock.
     Ici, on retourne ENOSYS pour les fonctions UNIX spécifiques. */
  #define close_fd(fd) closesocket((SOCKET)(intptr_t)(fd))
  static int wsa_once = 0;
  static void ensure_wsa(void){ if(!wsa_once){ WSADATA w; WSAStartup(MAKEWORD(2,2), &w); wsa_once=1; } }
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <sys/stat.h>
  #define close_fd(fd) close(fd)
#endif

#include "auxlib.h"
#include "state.h"
#include "object.h"
#include "vm.h"

/* ========================= VM ADAPTER (extern fournis) ================== */

static void        vl_push_nil     (VLState *L);
static void        vl_push_string  (VLState *L, const char *s);
static void        vl_push_lstring (VLState *L, const char *s, size_t n);
static void        vl_push_integer (VLState *L, int64_t v);
static int64_t     vl_check_integer(VLState *L, int idx);
static int64_t     vl_opt_integer  (VLState *L, int idx, int64_t def);
static int         vl_opt_boolean  (VLState *L, int idx, int def);
static const char *vl_check_string (VLState *L, int idx, size_t *len);
struct vl_Reg { const char *name; int (*fn)(VLState *L); };
static void        vl_register_module(VLState *L, const char *ns, const struct vl_Reg *funcs);

/* ================================ Utils ================================= */

static const char *E_EINVAL = "EINVAL";
static const char *E_ENOSYS = "ENOSYS";
static const char *E_ENOMEM = "ENOMEM";
static const char *E_EIO    = "EIO";

#ifndef _WIN32
static int set_nonblock_fd(int fd, int on){
  int flags = fcntl(fd, F_GETFL, 0);
  if(flags < 0) return -1;
  if(on) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}
static int set_cloexec_fd(int fd, int on){
  int flags = fcntl(fd, F_GETFD, 0);
  if(flags < 0) return -1;
  if(on) flags |= FD_CLOEXEC; else flags &= ~FD_CLOEXEC;
  return fcntl(fd, F_SETFD, flags);
}
#endif

/* =============================== pipe =================================== */

static int ipc_pipe(VLState *L){
  int nonblock = vl_opt_boolean(L, 1, 0);
#if defined(_WIN32)
  (void)nonblock;
  vl_push_nil(L); vl_push_string(L, E_ENOSYS); return 2;
#else
  int fds[2];
  if(pipe(fds) < 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  if(nonblock){
    set_nonblock_fd(fds[0],1);
    set_nonblock_fd(fds[1],1);
  }
  vl_push_integer(L, fds[0]);
  vl_push_integer(L, fds[1]);
  return 2;
#endif
}

/* ============================ socketpair ================================ */

static int ipc_socketpair(VLState *L){
  int nonblock = vl_opt_boolean(L, 1, 0);
#if defined(_WIN32)
  (void)nonblock;
  vl_push_nil(L); vl_push_string(L, E_ENOSYS); return 2;
#else
  int sv[2];
  if(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0){
    vl_push_nil(L); vl_push_string(L, E_EIO); return 2;
  }
  if(nonblock){
    set_nonblock_fd(sv[0],1);
    set_nonblock_fd(sv[1],1);
  }
  vl_push_integer(L, sv[0]);
  vl_push_integer(L, sv[1]);
  return 2;
#endif
}

/* ============================ UNIX listen =============================== */

static int ipc_unix_listen(VLState *L){
#if defined(_WIN32)
  vl_push_nil(L); vl_push_string(L, E_ENOSYS); return 2;
#else
  size_t n=0; const char *path = vl_check_string(L, 1, &n);
  int backlog = (int)vl_opt_integer(L, 2, 16);
  int do_unlink = vl_opt_boolean(L, 3, 0);

  if(!path || n==0){ vl_push_nil(L); vl_push_string(L, E_EINVAL); return 2; }
  if(do_unlink) unlink(path);

  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if(s < 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }

  struct sockaddr_un addr; memset(&addr,0,sizeof(addr));
  addr.sun_family = AF_UNIX;
  if(n >= sizeof(addr.sun_path)){ close_fd(s); vl_push_nil(L); vl_push_string(L, E_EINVAL); return 2; }
  memcpy(addr.sun_path, path, n);
  addr.sun_path[n] = 0;

  /* s'assurer que le répertoire existe côté appelant si besoin */

  if(bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0){
    close_fd(s); vl_push_nil(L); vl_push_string(L, E_EIO); return 2;
  }
  if(listen(s, backlog) < 0){
    close_fd(s); vl_push_nil(L); vl_push_string(L, E_EIO); return 2;
  }
  /* permissions rw pour owner par défaut ; l’appelant peut chmod(path) */

  vl_push_integer(L, s);
  return 1;
#endif
}

/* ============================ UNIX connect ============================== */

static int ipc_unix_connect(VLState *L){
#if defined(_WIN32)
  vl_push_nil(L); vl_push_string(L, E_ENOSYS); return 2;
#else
  size_t n=0; const char *path = vl_check_string(L, 1, &n);
  int nonblock = vl_opt_boolean(L, 2, 0);
  if(!path || n==0){ vl_push_nil(L); vl_push_string(L, E_EINVAL); return 2; }

  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if(s < 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  if(nonblock) set_nonblock_fd(s,1);

  struct sockaddr_un addr; memset(&addr,0,sizeof(addr));
  addr.sun_family = AF_UNIX;
  if(n >= sizeof(addr.sun_path)){ close_fd(s); vl_push_nil(L); vl_push_string(L, E_EINVAL); return 2; }
  memcpy(addr.sun_path, path, n); addr.sun_path[n]=0;

  if(connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0){
    /* en non-bloquant, EINPROGRESS est acceptable → on renvoie le fd */
    if(nonblock && (errno==EINPROGRESS)){
      vl_push_integer(L, s); return 1;
    }
    close_fd(s); vl_push_nil(L); vl_push_string(L, E_EIO); return 2;
  }
  vl_push_integer(L, s);
  return 1;
#endif
}

/* ================================ accept ================================ */

static int ipc_accept(VLState *L){
#if defined(_WIN32)
  vl_push_nil(L); vl_push_string(L, E_ENOSYS); return 2;
#else
  int64_t s = vl_check_integer(L, 1);
  int nonblock = vl_opt_boolean(L, 2, 1);
  int c = accept((int)s, NULL, NULL);
  if(c < 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  if(nonblock) set_nonblock_fd(c,1);
  vl_push_integer(L, c);
  return 1;
#endif
}

/* =============================== shutdown =============================== */

static int ipc_shutdown(VLState *L){
  int64_t fd  = vl_check_integer(L,1);
  int64_t how = vl_check_integer(L,2); /* 0=rd,1=wr,2=rw */
#if defined(_WIN32)
  ensure_wsa();
  int w = SD_BOTH;
  if(how==0) w = SD_RECEIVE; else if(how==1) w = SD_SEND; else w = SD_BOTH;
  if(shutdown((SOCKET)(intptr_t)fd, w) != 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L,1);
  return 1;
#else
  int w = SHUT_RDWR;
  if(how==0) w = SHUT_RD; else if(how==1) w = SHUT_WR; else w = SHUT_RDWR;
  if(shutdown((int)fd, w) != 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L,1);
  return 1;
#endif
}

/* ============================== read/write ============================== */

static int ipc_read(VLState *L){
  int64_t fd = vl_check_integer(L,1);
  int64_t maxlen = vl_check_integer(L,2);
  if(maxlen <= 0){ vl_push_nil(L); vl_push_string(L, E_EINVAL); return 2; }
  char *buf = (char*)malloc((size_t)maxlen);
  if(!buf){ vl_push_nil(L); vl_push_string(L, E_ENOMEM); return 2; }
#if defined(_WIN32)
  ensure_wsa();
  int n = recv((SOCKET)(intptr_t)fd, buf, (int)maxlen, 0);
  if(n < 0){ free(buf); vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_lstring(L, buf, (size_t)n);
  free(buf);
  return 1;
#else
  ssize_t n = read((int)fd, buf, (size_t)maxlen);
  if(n < 0){ free(buf); vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_lstring(L, buf, (size_t)n);
  free(buf);
  return 1;
#endif
}

static int ipc_write(VLState *L){
  int64_t fd = vl_check_integer(L,1);
  size_t len=0; const char *data = vl_check_string(L,2,&len);
#if defined(_WIN32)
  ensure_wsa();
  int n = send((SOCKET)(intptr_t)fd, data, (int)len, 0);
  if(n < 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L, (int64_t)n);
  return 1;
#else
  ssize_t n = write((int)fd, data, len);
  if(n < 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L, (int64_t)n);
  return 1;
#endif
}

/* ============================== send/recv ================================ */

static int ipc_send(VLState *L){
  int64_t fd = vl_check_integer(L,1);
  size_t len=0; const char *data = vl_check_string(L,2,&len);
  int64_t flags = vl_opt_integer(L,3,0);
#if defined(_WIN32)
  ensure_wsa();
  int n = send((SOCKET)(intptr_t)fd, data, (int)len, (int)flags);
  if(n < 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L, (int64_t)n);
  return 1;
#else
  ssize_t n = send((int)fd, data, len, (int)flags);
  if(n < 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L, (int64_t)n);
  return 1;
#endif
}

static int ipc_recv(VLState *L){
  int64_t fd = vl_check_integer(L,1);
  int64_t maxlen = vl_check_integer(L,2);
  int64_t flags = vl_opt_integer(L,3,0);
  if(maxlen <= 0){ vl_push_nil(L); vl_push_string(L, E_EINVAL); return 2; }
  char *buf = (char*)malloc((size_t)maxlen);
  if(!buf){ vl_push_nil(L); vl_push_string(L, E_ENOMEM); return 2; }
#if defined(_WIN32)
  ensure_wsa();
  int n = recv((SOCKET)(intptr_t)fd, buf, (int)maxlen, (int)flags);
  if(n < 0){ free(buf); vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_lstring(L, buf, (size_t)n);
  free(buf);
  return 1;
#else
  ssize_t n = recv((int)fd, buf, (size_t)maxlen, (int)flags);
  if(n < 0){ free(buf); vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_lstring(L, buf, (size_t)n);
  free(buf);
  return 1;
#endif
}

/* ============================== fd options =============================== */

static int ipc_set_nonblock(VLState *L){
  int64_t fd = vl_check_integer(L,1);
  int on = vl_opt_boolean(L,2,1);
#if defined(_WIN32)
  ensure_wsa();
  u_long m = on ? 1UL : 0UL;
  if(ioctlsocket((SOCKET)(intptr_t)fd, FIONBIO, &m) != 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L,1);
  return 1;
#else
  if(set_nonblock_fd((int)fd,on) != 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L,1);
  return 1;
#endif
}

static int ipc_set_cloexec(VLState *L){
  int64_t fd = vl_check_integer(L,1);
  int on = vl_opt_boolean(L,2,1);
#if defined(_WIN32)
  (void)fd; (void)on;
  /* Pas d’équivalent direct pour un socket existant; on ignore. */
  vl_push_integer(L,1);
  return 1;
#else
  if(set_cloexec_fd((int)fd,on) != 0){ vl_push_nil(L); vl_push_string(L, E_EIO); return 2; }
  vl_push_integer(L,1);
  return 1;
#endif
}

/* ================================= close ================================= */

static int ipc_close(VLState *L){
  int64_t fd = vl_check_integer(L,1);
  if(fd < 0){ vl_push_nil(L); vl_push_string(L, E_EINVAL); return 2; }
  close_fd((int)fd);
  vl_push_integer(L,1);
  return 1;
}

/* ================================ Dispatch =============================== */

static const struct vl_Reg funcs[] = {
  {"pipe",          ipc_pipe},
  {"socketpair",    ipc_socketpair},
  {"unix_listen",   ipc_unix_listen},
  {"unix_connect",  ipc_unix_connect},
  {"accept",        ipc_accept},
  {"shutdown",      ipc_shutdown},
  {"write",         ipc_write},
  {"read",          ipc_read},
  {"send",          ipc_send},
  {"recv",          ipc_recv},
  {"set_nonblock",  ipc_set_nonblock},
  {"set_cloexec",   ipc_set_cloexec},
  {"close",         ipc_close},
  {NULL, NULL}
};

int vl_openlib_ipc(VLState *L){
  vl_register_module(L, "ipc", funcs);
  return 1;
}