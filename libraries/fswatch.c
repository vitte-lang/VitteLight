// SPDX-License-Identifier: GPL-3.0-or-later
//
// fswatch.c — Cross-platform filesystem watcher for Vitte Light VM (C17,
// complete) Namespace: "fswatch"
//
// Backends:
//   - Linux: inotify (recursive supported; auto-add newly created subdirs).
//   - *BSD/macOS: kqueue (per-path; recursive adds current tree, new subdirs
//   require re-add).
//   - Windows: stubs -> (nil,"ENOSYS").
//
// API
//   h = fswatch.open()                               -> id | (nil,errmsg)
//   fswatch.close(h)                                 -> true
//   fswatch.add(h, path:string[, recursive=false])   -> wid:int | (nil,errmsg)
//   fswatch.rm(h, wid:int)                           -> true | (nil,errmsg)
//   fswatch.count(h)                                 -> int
//   fswatch.next(h[, timeout_ms:int=0])
//       -> usv:string  // rows: fullpath, action, is_dir:int, cookie:int
//       | (nil,"timeout") | (nil,errmsg)
//
// Actions (Linux, mapped on BSD):
//   "create","delete","modify","attrib","move_from","move_to","overflow"
//
// Notes:
//   - USV: US=0x1F between fields, RS=0x1E between rows.
//   - Inputs must be NUL-free; outputs use vl_push_lstring.
//
// Deps: auxlib.h, state.h, object.h, vm.h

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#define US 0x1F
#define RS 0x1E

#if defined(__linux__)
#define FSW_LINUX 1
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
#define FSW_KQUEUE 1
#include <dirent.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#else
#define FSW_STUB 1
#endif

// ---------------------------------------------------------------------
// VM helpers
// ---------------------------------------------------------------------
static const char *fw_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t fw_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int fw_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int fw_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)fw_check_int(S, idx);
  return defv;
}

static const char *errno_name(int e) {
  switch (e) {
#ifdef EACCES
    case EACCES:
      return "EACCES";
#endif
#ifdef EEXIST
    case EEXIST:
      return "EEXIST";
#endif
#ifdef EFAULT
    case EFAULT:
      return "EFAULT";
#endif
#ifdef EINVAL
    case EINVAL:
      return "EINVAL";
#endif
#ifdef EIO
    case EIO:
      return "EIO";
#endif
#ifdef EISDIR
    case EISDIR:
      return "EISDIR";
#endif
#ifdef ENAMETOOLONG
    case ENAMETOOLONG:
      return "ENAMETOOLONG";
#endif
#ifdef ENFILE
    case ENFILE:
      return "ENFILE";
#endif
#ifdef ENOENT
    case ENOENT:
      return "ENOENT";
#endif
#ifdef ENOMEM
    case ENOMEM:
      return "ENOMEM";
#endif
#ifdef ENOSPC
    case ENOSPC:
      return "ENOSPC";
#endif
#ifdef ENOTDIR
    case ENOTDIR:
      return "ENOTDIR";
#endif
#ifdef EPERM
    case EPERM:
      return "EPERM";
#endif
    default:
      return "EIO";
  }
}
static int push_errno(VL_State *S, int e) {
  vl_push_nil(S);
  vl_push_string(S, errno_name(e));
  return 2;
}

// ---------------------------------------------------------------------
// Common USV helpers
// ---------------------------------------------------------------------
static void usv_field(AuxBuffer *b, const char *s) {
  if (s) {
    size_t n = strlen(s);
    if (n) aux_buffer_append(b, (const uint8_t *)s, n);
  }
  uint8_t u = US;
  aux_buffer_append(b, &u, 1);
}
static void usv_int(AuxBuffer *b, long long v) {
  char tmp[64];
  snprintf(tmp, sizeof tmp, "%lld", v);
  usv_field(b, tmp);
}
static void usv_end(AuxBuffer *b) {
  if (b->len && b->data[b->len - 1] == (uint8_t)US)
    b->data[b->len - 1] = (uint8_t)RS;
  else {
    uint8_t r = RS;
    aux_buffer_append(b, &r, 1);
  }
}

// ---------------------------------------------------------------------
// Backend state
// ---------------------------------------------------------------------
typedef struct WatchRow {
  int used;
  int wid;  // public id
#if FSW_LINUX
  int wd;  // inotify watch descriptor
#elif FSW_KQUEUE
  int fd;  // opened fd for kevent ident
#endif
  char *path;     // watched path (dir or file)
  int recursive;  // user flag (Linux: live; kqueue: only initial tree)
} WatchRow;

typedef struct FW {
  int used;
  int next_wid;

#if FSW_LINUX
  int ifd;
#elif FSW_KQUEUE
  int kq;
#endif
  WatchRow *rows;
  int cap;
} FW;

static FW *g_fw = NULL;
static int g_fw_cap = 0;

static int ensure_fw_cap(int need) {
  if (need <= g_fw_cap) return 1;
  int n = g_fw_cap ? g_fw_cap : 8;
  while (n < need) n <<= 1;
  FW *nf = (FW *)realloc(g_fw, (size_t)n * sizeof *nf);
  if (!nf) return 0;
  for (int i = g_fw_cap; i < n; i++) {
    nf[i].used = 0;
    nf[i].next_wid = 1;
#if FSW_LINUX
    nf[i].ifd = -1;
#elif FSW_KQUEUE
    nf[i].kq = -1;
#endif
    nf[i].rows = NULL;
    nf[i].cap = 0;
  }
  g_fw = nf;
  g_fw_cap = n;
  return 1;
}
static int alloc_fw(void) {
  for (int i = 1; i < g_fw_cap; i++)
    if (!g_fw[i].used) return i;
  if (!ensure_fw_cap(g_fw_cap ? g_fw_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_fw_cap; i++)
    if (!g_fw[i].used) return i;
  return 0;
}
static int ensure_rows(FW *H, int need) {
  if (need <= H->cap) return 1;
  int n = H->cap ? H->cap : 16;
  while (n < need) n <<= 1;
  WatchRow *nr = (WatchRow *)realloc(H->rows, (size_t)n * sizeof *nr);
  if (!nr) return 0;
  for (int i = H->cap; i < n; i++) {
    nr[i].used = 0;
    nr[i].wid = 0;
#if FSW_LINUX
    nr[i].wd = -1;
#elif FSW_KQUEUE
    nr[i].fd = -1;
#endif
    nr[i].path = NULL;
    nr[i].recursive = 0;
  }
  H->rows = nr;
  H->cap = n;
  return 1;
}
static WatchRow *row_by_wid(FW *H, int wid) {
  for (int i = 1; i < H->cap; i++)
    if (H->rows[i].used && H->rows[i].wid == wid) return &H->rows[i];
  return NULL;
}

#if FSW_LINUX
static WatchRow *row_by_wd(FW *H, int wd) {
  for (int i = 1; i < H->cap; i++)
    if (H->rows[i].used && H->rows[i].wd == wd) return &H->rows[i];
  return NULL;
}
#endif

static int add_row(FW *H, WatchRow **out) {
  if (!ensure_rows(H, H->cap ? H->cap * 2 : 16)) return 0;
  for (int i = 1; i < H->cap; i++) {
    if (!H->rows[i].used) {
      H->rows[i].used = 1;
      H->rows[i].wid = H->next_wid++;
      *out = &H->rows[i];
      return 1;
    }
  }
  return 0;
}
static void free_row(WatchRow *r) {
  if (!r || !r->used) return;
  free(r->path);
  r->path = NULL;
#if FSW_LINUX
  r->wd = -1;
#elif FSW_KQUEUE
  if (r->fd >= 0) close(r->fd);
  r->fd = -1;
#endif
  r->used = 0;
  r->wid = 0;
  r->recursive = 0;
}

// ---------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------
#if FSW_LINUX

static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
static int is_dir_path(const char *p) {
  struct stat st;
  if (stat(p, &st) != 0) return 0;
  return S_ISDIR(st.st_mode) ? 1 : 0;
}

static int linux_add_watch(FW *H, const char *path, int recursive,
                           AuxBuffer *opt_newdirs_usv);

// recurse helper
static int linux_recurse_add(FW *H, const char *root,
                             AuxBuffer *opt_newdirs_usv) {
  DIR *d = opendir(root);
  if (!d) return 0;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    char sub[4096];
    size_t lr = strlen(root);
    snprintf(sub, sizeof sub, "%s%s%s", root,
             (lr && root[lr - 1] == '/') ? "" : "/", de->d_name);
    struct stat st;
    if (stat(sub, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (!linux_add_watch(H, sub, 1, opt_newdirs_usv)) { /* best-effort */
      }
      // continue recursion
      linux_recurse_add(H, sub, opt_newdirs_usv);
    }
  }
  closedir(d);
  return 1;
}

static int linux_add_watch(FW *H, const char *path, int recursive,
                           AuxBuffer *opt_newdirs_usv) {
  uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
                  IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF |
                  IN_IGNORED | IN_Q_OVERFLOW;

  // When watching directories: include IN_ONLYDIR? No, allow files too.
  int wd = inotify_add_watch(H->ifd, path, mask);
  if (wd < 0) return 0;

  WatchRow *r = NULL;
  if (!add_row(H, &r)) {
    inotify_rm_watch(H->ifd, wd);
    errno = ENOMEM;
    return 0;
  }
  r->wd = wd;
  r->path = strdup(path ? path : "");
  r->recursive = recursive ? 1 : 0;

  if (opt_newdirs_usv) {
    usv_field(opt_newdirs_usv, path);
    usv_field(opt_newdirs_usv, "create");
    usv_int(opt_newdirs_usv, 1);
    usv_int(opt_newdirs_usv, 0);
    usv_end(opt_newdirs_usv);
  }

  // If recursive and this is a directory, add subdirectories now
  if (recursive && is_dir_path(path)) {
    linux_recurse_add(H, path, opt_newdirs_usv);
  }
  return r->wid;
}

#endif  // FSW_LINUX

#if FSW_KQUEUE
static int kq_open_fd(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  int flags = S_ISDIR(st.st_mode) ? O_RDONLY : O_RDONLY;
#ifdef O_EVTONLY
  flags = O_EVTONLY;  // macOS
#endif
  return open(path, flags);
}
static int kq_add_one(FW *H, const char *path, int recursive) {
  (void)recursive;
  int fd = kq_open_fd(path);
  if (fd < 0) return 0;

  struct kevent kev;
  EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_RENAME |
             NOTE_REVOKE,
         0, NULL);
  if (kevent(H->kq, &kev, 1, NULL, 0, NULL) < 0) {
    close(fd);
    return 0;
  }

  WatchRow *r = NULL;
  if (!add_row(H, &r)) {
    close(fd);
    errno = ENOMEM;
    return 0;
  }
  r->fd = fd;
  r->path = strdup(path ? path : "");
  r->recursive = recursive ? 1 : 0;
  return r->wid;
}
static int is_dir_kq(const char *p) {
  struct stat st;
  if (stat(p, &st) != 0) return 0;
  return S_ISDIR(st.st_mode) ? 1 : 0;
}

static void kq_recurse_add(FW *H, const char *root) {
  DIR *d = opendir(root);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    char sub[4096];
    size_t lr = strlen(root);
    snprintf(sub, sizeof sub, "%s%s%s", root,
             (lr && root[lr - 1] == '/') ? "" : "/", de->d_name);
    struct stat st;
    if (stat(sub, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      (void)kq_add_one(H, sub, 1);
      kq_recurse_add(H, sub);
    }
  }
  closedir(d);
}
#endif  // FSW_KQUEUE

// ---------------------------------------------------------------------
// VM: open / close
// ---------------------------------------------------------------------
static int vfw_open(VL_State *S) {
#if FSW_LINUX
  int id = alloc_fw();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_fw[id].used = 1;
  g_fw[id].next_wid = 1;
  g_fw[id].rows = NULL;
  g_fw[id].cap = 0;
  g_fw[id].ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (g_fw[id].ifd < 0) {
    g_fw[id].used = 0;
    return push_errno(S, errno);
  }
  vl_push_int(S, (int64_t)id);
  return 1;
#elif FSW_KQUEUE
  int id = alloc_fw();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_fw[id].used = 1;
  g_fw[id].next_wid = 1;
  g_fw[id].rows = NULL;
  g_fw[id].cap = 0;
  g_fw[id].kq = kqueue();
  if (g_fw[id].kq < 0) {
    g_fw[id].used = 0;
    return push_errno(S, errno);
  }
  vl_push_int(S, (int64_t)id);
  return 1;
#else
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
#endif
}

static int vfw_close(VL_State *S) {
  int id = (int)fw_check_int(S, 1);
  if (id <= 0 || id >= g_fw_cap || !g_fw[id].used) {
    vl_push_bool(S, 1);
    return 1;
  }
  FW *H = &g_fw[id];
  // free rows
  if (H->rows) {
    for (int i = 1; i < H->cap; i++) {
#if FSW_LINUX
      if (H->rows[i].used && H->rows[i].wd >= 0 && H->ifd >= 0)
        inotify_rm_watch(H->ifd, H->rows[i].wd);
#endif
      if (H->rows[i].used) free_row(&H->rows[i]);
    }
    free(H->rows);
    H->rows = NULL;
    H->cap = 0;
  }
#if FSW_LINUX
  if (H->ifd >= 0) close(H->ifd);
  H->ifd = -1;
#elif FSW_KQUEUE
  if (H->kq >= 0) close(H->kq);
  H->kq = -1;
#endif
  H->used = 0;
  H->next_wid = 1;
  vl_push_bool(S, 1);
  return 1;
}

// ---------------------------------------------------------------------
// VM: add / rm / count
// ---------------------------------------------------------------------
static int vfw_add(VL_State *S) {
  int id = (int)fw_check_int(S, 1);
  const char *path = fw_check_str(S, 2);
  int recursive = fw_opt_bool(S, 3, 0);
  if (id <= 0 || id >= g_fw_cap || !g_fw[id].used) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  FW *H = &g_fw[id];

#if FSW_LINUX
  AuxBuffer dummy = {0};
  int wid = linux_add_watch(H, path, recursive, NULL);
  if (!wid) return push_errno(S, errno ? errno : EIO);
  // on Linux, if recursive, also add existing tree (done inside)
  vl_push_int(S, (int64_t)wid);
  return 1;

#elif FSW_KQUEUE
  int wid = kq_add_one(H, path, recursive);
  if (!wid) return push_errno(S, errno ? errno : EIO);
  if (recursive && is_dir_kq(path)) kq_recurse_add(H, path);
  vl_push_int(S, (int64_t)wid);
  return 1;

#else
  (void)H;
  (void)path;
  (void)recursive;
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
#endif
}

static int vfw_rm(VL_State *S) {
  int id = (int)fw_check_int(S, 1);
  int wid = (int)fw_check_int(S, 2);
  if (id <= 0 || id >= g_fw_cap || !g_fw[id].used) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  FW *H = &g_fw[id];
  WatchRow *r = row_by_wid(H, wid);
  if (!r) {
    vl_push_nil(S);
    vl_push_string(S, "ENOENT");
    return 2;
  }
#if FSW_LINUX
  if (H->ifd >= 0 && r->wd >= 0) inotify_rm_watch(H->ifd, r->wd);
#endif
  free_row(r);
  vl_push_bool(S, 1);
  return 1;
}

static int vfw_count(VL_State *S) {
  int id = (int)fw_check_int(S, 1);
  if (id <= 0 || id >= g_fw_cap || !g_fw[id].used) {
    vl_push_int(S, 0);
    return 1;
  }
  FW *H = &g_fw[id];
  int n = 0;
  for (int i = 1; i < H->cap; i++)
    if (H->rows[i].used) n++;
  vl_push_int(S, (int64_t)n);
  return 1;
}

// ---------------------------------------------------------------------
// VM: next() -> USV events or timeout
// ---------------------------------------------------------------------
static int vfw_next(VL_State *S) {
  int id = (int)fw_check_int(S, 1);
  int timeout = fw_opt_int(S, 2, 0);
  if (id <= 0 || id >= g_fw_cap || !g_fw[id].used) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  FW *H = &g_fw[id];

  AuxBuffer out = {0};

#if FSW_LINUX
  struct pollfd pfd = {H->ifd, POLLIN, 0};
  int prc = poll(&pfd, 1, timeout);
  if (prc == 0) {
    vl_push_nil(S);
    vl_push_string(S, "timeout");
    return 2;
  }
  if (prc < 0) {
    return push_errno(S, errno ? errno : EIO);
  }

  // read available events
  uint8_t buf[64 * 1024];
  ssize_t n = read(H->ifd, buf, sizeof buf);
  if (n < 0) {
    if (errno == EAGAIN) {
      vl_push_nil(S);
      vl_push_string(S, "timeout");
      return 2;
    }
    return push_errno(S, errno ? errno : EIO);
  }
  ssize_t off = 0;
  while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
    struct inotify_event *ev = (struct inotify_event *)(buf + off);
    off += sizeof(struct inotify_event) + ev->len;

    WatchRow *r = row_by_wd(H, ev->wd);
    const char *base = r ? r->path : "";
    char full[8192];
    if (ev->len && ev->name && ev->name[0]) {
      size_t lb = strlen(base);
      snprintf(full, sizeof full, "%s%s%s", base,
               (lb && base[lb - 1] == '/') ? "" : "/", ev->name);
    } else {
      snprintf(full, sizeof full, "%s", base);
    }

    const char *action = NULL;
    if (ev->mask & IN_Q_OVERFLOW)
      action = "overflow";
    else if (ev->mask & IN_CREATE)
      action = "create";
    else if (ev->mask & IN_DELETE)
      action = "delete";
    else if (ev->mask & IN_MODIFY)
      action = "modify";
    else if (ev->mask & IN_ATTRIB)
      action = "attrib";
    else if (ev->mask & IN_MOVED_FROM)
      action = "move_from";
    else if (ev->mask & IN_MOVED_TO)
      action = "move_to";
    else if (ev->mask & IN_DELETE_SELF)
      action = "delete";
    else if (ev->mask & IN_MOVE_SELF)
      action = "move_to";
    else if (ev->mask & IN_IGNORED)
      action = "delete";
    else
      action = "modify";

    int is_dir = (ev->mask & IN_ISDIR) ? 1 : 0;
    int cookie =
        (ev->mask & (IN_MOVED_FROM | IN_MOVED_TO)) ? (int)ev->cookie : 0;

    // Auto-add newly created directories for recursive watch
    if (is_dir && r && r->recursive && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
      (void)linux_add_watch(H, full, 1, NULL);
    }

    usv_field(&out, full);
    usv_field(&out, action);
    usv_int(&out, is_dir);
    usv_int(&out, cookie);
    usv_end(&out);
  }

#elif FSW_KQUEUE
  struct timespec ts, *tsp = NULL;
  if (timeout > 0) {
    ts.tv_sec = timeout / 1000;
    ts.tv_nsec = (long)(timeout % 1000) * 1000000L;
    tsp = &ts;
  }
  struct kevent evs[64];
  int nr = kevent(H->kq, NULL, 0, evs, 64, tsp);
  if (nr == 0) {
    vl_push_nil(S);
    vl_push_string(S, "timeout");
    return 2;
  }
  if (nr < 0) {
    return push_errno(S, errno ? errno : EIO);
  }

  for (int i = 0; i < nr; i++) {
    struct kevent *kev = &evs[i];
    // map fd -> row
    WatchRow *r = NULL;
    for (int j = 1; j < H->cap; j++)
      if (H->rows[j].used && H->rows[j].fd == (int)kev->ident) {
        r = &H->rows[j];
        break;
      }
    if (!r) continue;

    const char *action = "modify";
    if (kev->fflags & NOTE_DELETE)
      action = "delete";
    else if (kev->fflags & NOTE_RENAME)
      action = "move_to";
    else if (kev->fflags & NOTE_ATTRIB)
      action = "attrib";
    else if (kev->fflags & NOTE_WRITE)
      action = "modify";
    else if (kev->fflags & NOTE_REVOKE)
      action = "delete";

    int is_dir = 0;
    struct stat st;
    if (stat(r->path, &st) == 0) is_dir = S_ISDIR(st.st_mode) ? 1 : 0;

    usv_field(&out, r->path);
    usv_field(&out, action);
    usv_int(&out, is_dir);
    usv_int(&out, 0);
    usv_end(&out);
  }

#else
  (void)H;
  (void)timeout;
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
#endif

  if (out.len == 0) {
    vl_push_nil(S);
    vl_push_string(S, "timeout");
    return 2;
  }
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------
static const VL_Reg fswatchlib[] = {{"open", vfw_open},   {"close", vfw_close},
                                    {"add", vfw_add},     {"rm", vfw_rm},
                                    {"count", vfw_count}, {"next", vfw_next},
                                    {NULL, NULL}};

void vl_open_fswatch(VL_State *S) { vl_register_lib(S, "fswatch", fswatchlib); }
