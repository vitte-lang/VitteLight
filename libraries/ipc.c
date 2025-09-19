// SPDX-License-Identifier: GPL-3.0-or-later
//
// ipc.c — Local IPC (Unix domain sockets, framing u32) for Vitte Light VM (C17)
// Namespace: "ipc"
//
// Build examples:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c ipc.c
//
// Model:
//   - Serveur: socket AF_UNIX, listen/backlog, accept.
//   - Client: connect AF_UNIX.
//   - I/O: send/recv bruts + messages (u32 length prefix, network order).
//   - Non-bloquant: fcntl(O_NONBLOCK).
//
// API (C symbol layer):
//   int  ipc_listen_unix(const char* path, int backlog);     // >=0 fd | <0 err
//   int  ipc_accept(int listen_fd);                           // >=0 fd | <0 err
//   int  ipc_connect_unix(const char* path, int timeout_ms);  // >=0 fd | <0 err
//   int  ipc_close(int fd);                                   // 0
//   int  ipc_set_nonblock(int fd, int yes);                   // 0 | <0
//
//   // I/O arêtes nues (best-effort; renvoie >=0 octets écrits/lus, ou <0 err)
//   long ipc_send(int fd, const void* buf, size_t len);       // -EAGAIN possible
//   long ipc_recv(int fd, void* buf, size_t cap);             // -EAGAIN possible
//
//   // Messages encadrés (u32 big-endian, limite 16 MiB par défaut)
//   int  ipc_send_msg(int fd, const void* buf, size_t len);   // 0 | <0
//   long ipc_recv_msg(int fd, void* buf, size_t cap);         // >0 n | 0 EOF | <0 err
//
// Notes:
//   - Couche neutre VM. Binding VM: copier via vl_push_lstring.
//   - Erreurs: -EINVAL, -ENOSYS, -ENOMEM, -EIO, -ETIMEDOUT, -EAGAIN.
//   - Supprime le path existant avant bind (sécurité).
//
// Deps VM optionnels: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifndef EINVAL
#  define EINVAL 22
#endif
#ifndef ENOSYS
#  define ENOSYS 38
#endif
#ifndef EIO
#  define EIO 5
#endif
#ifndef ETIMEDOUT
#  define ETIMEDOUT 110
#endif

#ifndef VL_EXPORT
#  if defined(_WIN32) && !defined(__clang__)
#    define VL_EXPORT __declspec(dllexport)
#  else
#    define VL_EXPORT
#  endif
#endif

#if defined(_WIN32)
// Windows non implémenté ici (nommable via Named Pipes)
VL_EXPORT int  ipc_listen_unix(const char* path, int backlog){ (void)path;(void)backlog; return -ENOSYS; }
VL_EXPORT int  ipc_accept(int listen_fd){ (void)listen_fd; return -ENOSYS; }
VL_EXPORT int  ipc_connect_unix(const char* path, int timeout_ms){ (void)path;(void)timeout_ms; return -ENOSYS; }
VL_EXPORT int  ipc_close(int fd){ (void)fd; return 0; }
VL_EXPORT int  ipc_set_nonblock(int fd, int yes){ (void)fd;(void)yes; return -ENOSYS; }
VL_EXPORT long ipc_send(int fd, const void* buf, size_t len){ (void)fd;(void)buf;(void)len; return -ENOSYS; }
VL_EXPORT long ipc_recv(int fd, void* buf, size_t cap){ (void)fd;(void)buf;(void)cap; return -ENOSYS; }
VL_EXPORT int  ipc_send_msg(int fd, const void* buf, size_t len){ (void)fd;(void)buf;(void)len; return -ENOSYS; }
VL_EXPORT long ipc_recv_msg(int fd, void* buf, size_t cap){ (void)fd;(void)buf;(void)cap; return -ENOSYS; }

#else  // POSIX

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/in.h>   // htonl/ntohl
#include <unistd.h>
#include <fcntl.h>

#ifndef IPC_MAX_MSG
#  define IPC_MAX_MSG (16u*1024u*1024u) // 16 MiB
#endif

static int set_nonblock(int fd, int yes) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -EIO;
  if (yes) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, fl) == -1 ? -EIO : 0;
}

VL_EXPORT int ipc_set_nonblock(int fd, int yes){ return set_nonblock(fd, yes); }

VL_EXPORT int ipc_close(int fd){ if (fd>=0) (void)close(fd); return 0; }

static int mk_sock_unix(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -EIO;
  // CLOEXEC de préférence
#ifdef O_CLOEXEC
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
  return fd;
}

VL_EXPORT int ipc_listen_unix(const char* path, int backlog) {
  if (!path || !*path) return -EINVAL;
  int fd = mk_sock_unix();
  if (fd < 0) return fd;

  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  size_t n = strlen(path);
  if (n >= sizeof(sa.sun_path)) { close(fd); return -EINVAL; }
  strncpy(sa.sun_path, path, sizeof(sa.sun_path)-1);

  // Supprime l’ancien socket path si existe
  unlink(path);

  if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) { close(fd); return -EIO; }
  if (chmod(path, 0660) != 0) { /* non fatal */ }

  if (listen(fd, backlog > 0 ? backlog : 16) != 0) { close(fd); unlink(path); return -EIO; }
  return fd;
}

VL_EXPORT int ipc_accept(int listen_fd) {
  if (listen_fd < 0) return -EINVAL;
  int fd = accept(listen_fd, NULL, NULL);
  if (fd < 0) return -EIO;
#ifdef O_CLOEXEC
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
  return fd;
}

static int connect_with_timeout(int fd, const struct sockaddr* sa, socklen_t slen, int timeout_ms) {
  // Passage non-bloquant si timeout
  int restore_block = 0;
  if (timeout_ms > 0) {
    if (set_nonblock(fd, 1) == 0) restore_block = 1;
  }
  int rc = connect(fd, sa, slen);
  if (rc == 0) {
    if (restore_block) set_nonblock(fd, 0);
    return 0;
  }
  if (timeout_ms <= 0) return -EIO;

  if (errno != EINPROGRESS) return -EIO;

  fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
  struct timeval tv;
  tv.tv_sec  = timeout_ms/1000;
  tv.tv_usec = (timeout_ms%1000)*1000;
  rc = select(fd+1, NULL, &wf, NULL, &tv);
  if (rc <= 0) return rc == 0 ? -ETIMEDOUT : -EIO;

  int err = 0; socklen_t len = sizeof(err);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) return -EIO;

  if (restore_block) set_nonblock(fd, 0);
  return 0;
}

VL_EXPORT int ipc_connect_unix(const char* path, int timeout_ms) {
  if (!path || !*path) return -EINVAL;
  int fd = mk_sock_unix();
  if (fd < 0) return fd;

  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  size_t n = strlen(path);
  if (n >= sizeof(sa.sun_path)) { close(fd); return -EINVAL; }
  strncpy(sa.sun_path, path, sizeof(sa.sun_path)-1);

  int rc = connect_with_timeout(fd, (struct sockaddr*)&sa, (socklen_t)sizeof(sa), timeout_ms);
  if (rc != 0) { close(fd); return rc; }
  return fd;
}

// ---- Raw I/O ----
VL_EXPORT long ipc_send(int fd, const void* buf, size_t len) {
  if (fd < 0 || (!buf && len)) return -EINVAL;
  ssize_t n = send(fd, buf, len, 0);
  if (n < 0) return (errno==EAGAIN||errno==EWOULDBLOCK) ? -EAGAIN : -EIO;
  return (long)n;
}

VL_EXPORT long ipc_recv(int fd, void* buf, size_t cap) {
  if (fd < 0 || (!buf && cap)) return -EINVAL;
  ssize_t n = recv(fd, buf, cap, 0);
  if (n < 0) return (errno==EAGAIN||errno==EWOULDBLOCK) ? -EAGAIN : -EIO;
  return (long)n; // 0 == EOF
}

// ---- Message framing (u32 big-endian) ----
static int send_all(int fd, const void* b, size_t n){
  const unsigned char* p = (const unsigned char*)b;
  while (n){
    ssize_t k = send(fd, p, n, 0);
    if (k < 0){
      if (errno==EINTR) continue;
      return (errno==EAGAIN||errno==EWOULDBLOCK)? -EAGAIN : -EIO;
    }
    p += (size_t)k; n -= (size_t)k;
  }
  return 0;
}

static int recv_all(int fd, void* b, size_t n){
  unsigned char* p = (unsigned char*)b;
  while (n){
    ssize_t k = recv(fd, p, n, 0);
    if (k == 0) return -EIO; // EOF prématuré
    if (k < 0){
      if (errno==EINTR) continue;
      return (errno==EAGAIN||errno==EWOULDBLOCK)? -EAGAIN : -EIO;
    }
    p += (size_t)k; n -= (size_t)k;
  }
  return 0;
}

// hton/ntoh pour u32 portable sans dépendre d'endian.h
static uint32_t to_be32(uint32_t x){
  unsigned char b[4] = { (unsigned char)(x>>24), (unsigned char)(x>>16),
                         (unsigned char)(x>>8),  (unsigned char)(x) };
  uint32_t y; memcpy(&y, b, 4); return y;
}
static uint32_t from_be32(uint32_t y){
  unsigned char b[4]; memcpy(b, &y, 4);
  return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|((uint32_t)b[3]);
}

VL_EXPORT int ipc_send_msg(int fd, const void* buf, size_t len){
  if (fd < 0) return -EINVAL;
  if (len > IPC_MAX_MSG) return -EINVAL;
  uint32_t be = to_be32((uint32_t)len);
  int rc = send_all(fd, &be, sizeof(be));
  if (rc != 0) return rc;
  if (len == 0) return 0;
  return send_all(fd, buf, len);
}

VL_EXPORT long ipc_recv_msg(int fd, void* buf, size_t cap){
  if (fd < 0) return -EINVAL;
  uint32_t be = 0;
  int rc = recv_all(fd, &be, sizeof(be));
  if (rc != 0) return rc;
  uint32_t n = from_be32(be);
  if (n > IPC_MAX_MSG) return -EINVAL;
  if (n == 0) return 0;
  if (cap < n) return -ENOMEM;
  rc = recv_all(fd, buf, n);
  if (rc != 0) return rc;
  return (long)n;
}

#endif // POSIX