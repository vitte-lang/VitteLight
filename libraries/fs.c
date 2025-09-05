// SPDX-License-Identifier: GPL-3.0-or-later
//
// fs.c — Filesystem utilities for Vitte Light VM (C17, complet)
// Namespace: "fs"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c fs.c
//
// Modèle API:
//   Chemins en UTF-8 côté VM. Sorties binaires via vl_push_lstring.
//
//   Fichiers
//     fs.readfile(path[, max_bytes=-1]) -> bytes | (nil,errmsg)
//     fs.writefile(path, bytes[, mode=0644]) -> true | (nil,errmsg)
//     fs.appendfile(path, bytes) -> true | (nil,errmsg)
//     fs.exists(path) -> bool
//
//   Métadonnées
//     fs.stat(path)  -> mode:int, size:int64, mtime:int64, type:string |
//     (nil,errmsg) fs.lstat(path) -> mode:int, size:int64, mtime:int64,
//     type:string | (nil,errmsg) fs.realpath(path) -> string | (nil,errmsg)
//
//   Répertoires
//     fs.readdir(path) -> usv:string  // rows: name, type, size, mtime
//     fs.mkdir(path[, mode=0755]) -> true | (nil,errmsg)
//     fs.mkdirp(path[, mode=0755]) -> true | (nil,errmsg)
//     fs.rmdir(path) -> true | (nil,errmsg)
//
//   Ops
//     fs.rename(old, new) -> true | (nil,errmsg)
//     fs.unlink(path)     -> true | (nil,errmsg)
//     fs.symlink(target, linkpath) -> true | (nil,errmsg)  // stubs on Windows
//     fs.readlink(path)   -> target | (nil,errmsg)         // stubs on Windows
//     fs.chmod(path, mode:int) -> true | (nil,errmsg)
//     fs.utime(path, atime:int64, mtime:int64) -> true | (nil,errmsg)
//     fs.copyfile(src, dst[, overwrite=true[, preserve_mode=true]]) ->
//     bytes:int64 | (nil,errmsg) fs.du(path[, follow_symlinks=false]) ->
//     bytes:int64 | (nil,errmsg)
//
//   Divers
//     fs.glob(pattern) -> usv:string | (nil,errmsg)
//     fs.tmpdir() -> string
//     fs.home()   -> string
//     fs.sep()    -> string  // "/" ou "\"
//
// Notes:
//   - L’USV utilise US=0x1F entre champs, RS=0x1E entre lignes.
//   - Les erreurs renvoient des codes style errno: "ENOENT", "EACCES", etc.
//   Fallback "EIO".
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <windows.h>
#define PATH_SEP '\\'
#define stat64 _stati64
#define fseeko _fseeki64
#define ftello _ftelli64
#else
#include <dirent.h>
#include <glob.h>
#include <unistd.h>
#include <utime.h>
#define PATH_SEP '/'
#endif

#define US 0x1F
#define RS 0x1E

// ---------------------------------------------------------------------
// Helpers VM
// ---------------------------------------------------------------------
static const char *fs_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t fs_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int fs_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int fs_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)fs_check_int(S, idx);
  return defv;
}
static const char *fs_opt_str(VL_State *S, int idx, const char *defv) {
  if (!vl_get(S, idx) || !vl_isstring(S, idx)) return defv;
  return fs_check_str(S, idx);
}

static const char *errno_name(int e) {
  switch (e) {
    case 0:
      return "OK";
#ifdef E2BIG
    case E2BIG:
      return "E2BIG";
#endif
#ifdef EACCES
    case EACCES:
      return "EACCES";
#endif
#ifdef EAGAIN
    case EAGAIN:
      return "EAGAIN";
#endif
#ifdef EBADF
    case EBADF:
      return "EBADF";
#endif
#ifdef EBUSY
    case EBUSY:
      return "EBUSY";
#endif
#ifdef EEXIST
    case EEXIST:
      return "EEXIST";
#endif
#ifdef EFAULT
    case EFAULT:
      return "EFAULT";
#endif
#ifdef EFBIG
    case EFBIG:
      return "EFBIG";
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
#ifdef EMFILE
    case EMFILE:
      return "EMFILE";
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
#ifdef EROFS
    case EROFS:
      return "EROFS";
#endif
#ifdef EXDEV
    case EXDEV:
      return "EXDEV";
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

static const char *ftype_string(mode_t m) {
#ifdef _WIN32
  // Minimal mapping on Windows
  if ((m & _S_IFDIR) == _S_IFDIR) return "dir";
  if ((m & _S_IFREG) == _S_IFREG) return "file";
  return "other";
#else
  if (S_ISDIR(m)) return "dir";
  if (S_ISREG(m)) return "file";
#ifdef S_ISLNK
  if (S_ISLNK(m)) return "symlink";
#endif
#ifdef S_ISCHR
  if (S_ISCHR(m)) return "char";
#endif
#ifdef S_ISBLK
  if (S_ISBLK(m)) return "block";
#endif
#ifdef S_ISFIFO
  if (S_ISFIFO(m)) return "fifo";
#endif
#ifdef S_ISSOCK
  if (S_ISSOCK(m)) return "sock";
#endif
  return "other";
#endif
}

// Read whole file into buffer
static int read_all_file(const char *path, int64_t max_bytes, AuxBuffer *out,
                         int *err) {
  *err = 0;
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    *err = errno;
    return 0;
  }
  if (max_bytes < 0) {
    // try to pre-size
#ifdef _WIN32
    struct _stati64 st;
    if (_stati64(path, &st) == 0 && st.st_size > 0)
      aux_buffer_reserve(out, (size_t)st.st_size);
#else
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0)
      aux_buffer_reserve(out, (size_t)st.st_size);
#endif
  }
  const size_t CH = 64 * 1024;
  uint8_t *buf = (uint8_t *)malloc(CH);
  if (!buf) {
    fclose(fp);
    *err = ENOMEM;
    return 0;
  }
  size_t nread;
  int64_t remain = max_bytes;
  while ((nread = fread(buf, 1, CH, fp)) > 0) {
    if (max_bytes >= 0) {
      if ((int64_t)nread > remain) nread = (size_t)remain;
      aux_buffer_append(out, buf, nread);
      remain -= (int64_t)nread;
      if (remain <= 0) break;
    } else {
      aux_buffer_append(out, buf, nread);
    }
  }
  free(buf);
  if (ferror(fp)) {
    *err = EIO;
    fclose(fp);
    return 0;
  }
  fclose(fp);
  return 1;
}

static int write_all_file(const char *path, const char *bytes, const char *mode,
                          int set_mode, int perm, int *err) {
  *err = 0;
  FILE *fp = fopen(path, mode);
  if (!fp) {
    *err = errno;
    return 0;
  }
  size_t n = strlen(bytes);  // VM strings may hold binary? outputs yes; inputs
                             // constraint: NUL not allowed.
  size_t w = fwrite(bytes, 1, n, fp);
  if (w != n) {
    *err = EIO;
    fclose(fp);
    return 0;
  }
  if (fclose(fp) != 0) {
    *err = errno;
    return 0;
  }
#ifndef _WIN32
  if (set_mode) {
    if (chmod(path, (mode_t)perm) != 0) {
      *err = errno;
      return 0;
    }
  }
#else
  (void)set_mode;
  (void)perm;
#endif
  return 1;
}

// mkdir -p
static int mkdir_p(const char *path, int perm) {
  if (!path || !*path) {
    errno = EINVAL;
    return -1;
  }
  char *tmp = (char *)malloc(strlen(path) + 1);
  if (!tmp) {
    errno = ENOMEM;
    return -1;
  }
  strcpy(tmp, path);
  size_t len = strlen(tmp);
  // strip trailing separators
  while (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
    tmp[--len] = 0;
  if (len == 0) {
    free(tmp);
    return 0;
  }

  char sep1 = '/', sep2 = '\\';
  for (size_t i = 1; i < len; i++) {
    if (tmp[i] == sep1 || tmp[i] == sep2) {
      char c = tmp[i];
      tmp[i] = 0;
      if (strlen(tmp) > 0) {
#ifdef _WIN32
        if (_mkdir(tmp) != 0 && errno != EEXIST) {
          tmp[i] = c;
          free(tmp);
          return -1;
        }
#else
        if (mkdir(tmp, (mode_t)perm) != 0 && errno != EEXIST) {
          tmp[i] = c;
          free(tmp);
          return -1;
        }
#endif
      }
      tmp[i] = c;
    }
  }
#ifdef _WIN32
  if (_mkdir(tmp) != 0 && errno != EEXIST) {
    free(tmp);
    return -1;
  }
#else
  if (mkdir(tmp, (mode_t)perm) != 0 && errno != EEXIST) {
    free(tmp);
    return -1;
  }
#endif
  free(tmp);
  return 0;
}

static void usv_append_field(AuxBuffer *b, const char *s) {
  if (s && *s) aux_buffer_append(b, (const uint8_t *)s, strlen(s));
  uint8_t u = US;
  aux_buffer_append(b, &u, 1);
}
static void usv_append_int(AuxBuffer *b, long long v) {
  char tmp[64];
  snprintf(tmp, sizeof tmp, "%lld", v);
  usv_append_field(b, tmp);
}
static void usv_end_row(AuxBuffer *b) {
  if (b->len && b->data[b->len - 1] == (uint8_t)US)
    b->data[b->len - 1] = (uint8_t)RS;
  else {
    uint8_t r = RS;
    aux_buffer_append(b, &r, 1);
  }
}

// ---------------------------------------------------------------------
// VM functions
// ---------------------------------------------------------------------

// fs.readfile(path[, max_bytes=-1])
static int vfs_readfile(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  int64_t maxb = vl_get(S, 2) ? fs_check_int(S, 2) : -1;
  AuxBuffer out = {0};
  int err = 0;
  if (!read_all_file(p, maxb, &out, &err)) {
    aux_buffer_free(&out);
    return push_errno(S, err ? err : EIO);
  }
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

// fs.writefile(path, bytes[, mode=0644])
static int vfs_writefile(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  const char *bytes = fs_check_str(S, 2);
  int perm = fs_opt_int(S, 3, 0644);
  int err = 0;
  if (!write_all_file(p, bytes, "wb", 1, perm, &err))
    return push_errno(S, err ? err : EIO);
  vl_push_bool(S, 1);
  return 1;
}
// fs.appendfile(path, bytes)
static int vfs_appendfile(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  const char *bytes = fs_check_str(S, 2);
  int err = 0;
  if (!write_all_file(p, bytes, "ab", 0, 0, &err))
    return push_errno(S, err ? err : EIO);
  vl_push_bool(S, 1);
  return 1;
}

// fs.exists(path)
static int vfs_exists(VL_State *S) {
  const char *p = fs_check_str(S, 1);
#ifdef _WIN32
  struct _stati64 st;
  int rc = _stati64(p, &st);
#else
  struct stat st;
  int rc = stat(p, &st);
#endif
  vl_push_bool(S, rc == 0 ? 1 : 0);
  return 1;
}

static int do_stat_common(VL_State *S, const char *p, int follow) {
#ifdef _WIN32
  struct _stati64 st;
  int rc = _stati64(p, &st);
  if (rc != 0) return push_errno(S, errno);
  vl_push_int(S, (int64_t)st.st_mode);
  vl_push_int(S, (int64_t)st.st_size);
  vl_push_int(S, (int64_t)st.st_mtime);
  vl_push_string(S, ftype_string(st.st_mode));
  return 4;
#else
  struct stat st;
  int rc = follow ? stat(p, &st) : lstat(p, &st);
  if (rc != 0) return push_errno(S, errno);
  vl_push_int(S, (int64_t)st.st_mode);
  vl_push_int(S, (int64_t)st.st_size);
  vl_push_int(S, (int64_t)st.st_mtime);
  vl_push_string(S, ftype_string(st.st_mode));
  return 4;
#endif
}
// fs.stat(path)
static int vfs_stat(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  return do_stat_common(S, p, 1);
}
// fs.lstat(path)
static int vfs_lstat(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  return do_stat_common(S, p, 0);
}

// fs.realpath(path)
static int vfs_realpath(VL_State *S) {
  const char *p = fs_check_str(S, 1);
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetFullPathNameA(p, (DWORD)sizeof buf, buf, NULL);
  if (n == 0 || n >= sizeof buf) return push_errno(S, EIO);
  vl_push_string(S, buf);
  return 1;
#else
  char *rp = realpath(p, NULL);
  if (!rp) return push_errno(S, errno);
  vl_push_string(S, rp);
  free(rp);
  return 1;
#endif
}

// fs.readdir(path) -> USV: name,type,size,mtime
static int vfs_readdir(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  AuxBuffer out = {0};
#ifdef _WIN32
  char pattern[MAX_PATH];
  snprintf(pattern, sizeof pattern, "%s\\*.*", p);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return push_errno(S, errno ? errno : ENOENT);
  }
  do {
    const char *name = fd.cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    LARGE_INTEGER sz;
    sz.HighPart = fd.nFileSizeHigh;
    sz.LowPart = fd.nFileSizeLow;
    FILETIME ft = fd.ftLastWriteTime;
    // convert FILETIME to unix epoch seconds
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    int64_t t = (int64_t)((uli.QuadPart - 116444736000000000ULL) / 10000000ULL);

    usv_append_field(&out, name);
    usv_append_field(&out, isdir ? "dir" : "file");
    usv_append_int(&out, (long long)sz.QuadPart);
    usv_append_int(&out, (long long)t);
    usv_end_row(&out);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR *d = opendir(p);
  if (!d) {
    return push_errno(S, errno);
  }
  struct dirent *de;
  while ((de = readdir(d))) {
    const char *name = de->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

    char fpath[4096];
    size_t lp = strlen(p);
    snprintf(fpath, sizeof fpath, "%s%s%s", p,
             (lp && p[lp - 1] == PATH_SEP) ? "" : "/", name);

    struct stat st;
    if (lstat(fpath, &st) != 0) {
      continue;
    }

    usv_append_field(&out, name);
    usv_append_field(&out, ftype_string(st.st_mode));
    usv_append_int(&out, (long long)st.st_size);
    usv_append_int(&out, (long long)st.st_mtime);
    usv_end_row(&out);
  }
  closedir(d);
#endif
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

// fs.mkdir(path[, mode=0755])
static int vfs_mkdir(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  int perm = fs_opt_int(S, 2, 0755);
#ifdef _WIN32
  if (_mkdir(p) != 0) return push_errno(S, errno);
#else
  if (mkdir(p, (mode_t)perm) != 0) return push_errno(S, errno);
#endif
  vl_push_bool(S, 1);
  return 1;
}

// fs.mkdirp(path[, mode=0755])
static int vfs_mkdirp(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  int perm = fs_opt_int(S, 2, 0755);
  if (mkdir_p(p, perm) != 0) return push_errno(S, errno);
  vl_push_bool(S, 1);
  return 1;
}

// fs.rmdir(path)
static int vfs_rmdir(VL_State *S) {
  const char *p = fs_check_str(S, 1);
#ifdef _WIN32
  if (_rmdir(p) != 0) return push_errno(S, errno);
#else
  if (rmdir(p) != 0) return push_errno(S, errno);
#endif
  vl_push_bool(S, 1);
  return 1;
}

// fs.rename(old,new)
static int vfs_rename(VL_State *S) {
  const char *a = fs_check_str(S, 1);
  const char *b = fs_check_str(S, 2);
  if (rename(a, b) != 0) return push_errno(S, errno);
  vl_push_bool(S, 1);
  return 1;
}

// fs.unlink(path)
static int vfs_unlink(VL_State *S) {
  const char *p = fs_check_str(S, 1);
#ifdef _WIN32
  if (DeleteFileA(p) == 0) return push_errno(S, errno ? errno : EIO);
#else
  if (unlink(p) != 0) return push_errno(S, errno);
#endif
  vl_push_bool(S, 1);
  return 1;
}

// fs.symlink(target, linkpath)
static int vfs_symlink(VL_State *S) {
  const char *t = fs_check_str(S, 1);
  const char *l = fs_check_str(S, 2);
#ifdef _WIN32
  (void)t;
  (void)l;
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
#else
  if (symlink(t, l) != 0) return push_errno(S, errno);
  vl_push_bool(S, 1);
  return 1;
#endif
}

// fs.readlink(path)
static int vfs_readlink(VL_State *S) {
  const char *p = fs_check_str(S, 1);
#ifdef _WIN32
  (void)p;
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
#else
  char buf[4096];
  ssize_t n = readlink(p, buf, sizeof buf - 1);
  if (n < 0) return push_errno(S, errno);
  buf[n] = 0;
  vl_push_string(S, buf);
  return 1;
#endif
}

// fs.chmod(path, mode)
static int vfs_chmod(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  int mode = (int)fs_check_int(S, 2);
#ifdef _WIN32
  (void)mode;
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
#else
  if (chmod(p, (mode_t)mode) != 0) return push_errno(S, errno);
  vl_push_bool(S, 1);
  return 1;
#endif
}

// fs.utime(path, atime, mtime)
static int vfs_utime(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  int64_t at = fs_check_int(S, 2);
  int64_t mt = fs_check_int(S, 3);
#ifdef _WIN32
  (void)at;
  (void)mt;
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
#else
  struct utimbuf ub;
  ub.actime = (time_t)at;
  ub.modtime = (time_t)mt;
  if (utime(p, &ub) != 0) return push_errno(S, errno);
  vl_push_bool(S, 1);
  return 1;
#endif
}

// fs.copyfile(src,dst[, overwrite=true[, preserve_mode=true]]) -> bytes
static int vfs_copyfile(VL_State *S) {
  const char *src = fs_check_str(S, 1);
  const char *dst = fs_check_str(S, 2);
  int overwrite = fs_opt_bool(S, 3, 1);
  int preserve = fs_opt_bool(S, 4, 1);

#ifdef _WIN32
  if (!overwrite) {
    DWORD attr = GetFileAttributesA(dst);
    if (attr != INVALID_FILE_ATTRIBUTES) {
      return push_errno(S, EEXIST);
    }
  }
  // Manual copy for byte count
  FILE *fi = fopen(src, "rb");
  if (!fi) return push_errno(S, errno);
  FILE *fo = fopen(dst, overwrite ? "wb" : "wx");
  if (!fo) {
    int e = errno;
    fclose(fi);
    return push_errno(S, e);
  }
  char *buf = (char *)malloc(256 * 1024);
  if (!buf) {
    fclose(fi);
    fclose(fo);
    return push_errno(S, ENOMEM);
  }
  size_t n;
  long long total = 0;
  while ((n = fread(buf, 1, 256 * 1024, fi)) > 0) {
    if (fwrite(buf, 1, n, fo) != n) {
      free(buf);
      fclose(fi);
      fclose(fo);
      return push_errno(S, EIO);
    }
    total += (long long)n;
  }
  free(buf);
  if (fclose(fi) != 0 || fclose(fo) != 0) return push_errno(S, EIO);
  (void)preserve;  // not supported
  vl_push_int(S, total);
  return 1;
#else
  struct stat st;
  if (stat(src, &st) != 0) return push_errno(S, errno);
  int exists = (stat(dst, &st) == 0);
  if (exists && !overwrite) return push_errno(S, EEXIST);

  FILE *fi = fopen(src, "rb");
  if (!fi) return push_errno(S, errno);
  FILE *fo = fopen(dst, overwrite ? "wb" : "wbx");
  if (!fo) {
    int e = errno;
    fclose(fi);
    return push_errno(S, e);
  }
  char *buf = (char *)malloc(256 * 1024);
  if (!buf) {
    fclose(fi);
    fclose(fo);
    return push_errno(S, ENOMEM);
  }
  size_t n;
  long long total = 0;
  while ((n = fread(buf, 1, 256 * 1024, fi)) > 0) {
    if (fwrite(buf, 1, n, fo) != n) {
      free(buf);
      fclose(fi);
      fclose(fo);
      return push_errno(S, EIO);
    }
    total += (long long)n;
  }
  free(buf);
  if (fclose(fi) != 0 || fclose(fo) != 0) return push_errno(S, EIO);
  if (preserve) {
    struct stat ss;
    if (stat(src, &ss) == 0) {
      chmod(dst, ss.st_mode & 0777);
    }
  }
  vl_push_int(S, total);
  return 1;
#endif
}

// fs.du(path[, follow_symlinks=false]) -> bytes
static int64_t du_walk(const char *p, int follow, int *err) {
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  char patt[MAX_PATH];
  snprintf(patt, sizeof patt, "%s\\*.*", p);
  HANDLE h = FindFirstFileA(patt, &fd);
  if (h == INVALID_HANDLE_VALUE) {  // maybe file
    struct _stati64 st;
    if (_stati64(p, &st) == 0) return st.st_size;
    *err = errno;
    return -1;
  }
  int64_t total = 0;
  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
      continue;
    char child[MAX_PATH];
    snprintf(child, sizeof child, "%s\\%s", p, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      int64_t s = du_walk(child, follow, err);
      if (s < 0) {
        FindClose(h);
        return -1;
      }
      total += s;
    } else {
      LARGE_INTEGER sz;
      sz.HighPart = fd.nFileSizeHigh;
      sz.LowPart = fd.nFileSizeLow;
      total += sz.QuadPart;
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return total;
#else
  struct stat st;
  if ((follow ? stat(p, &st) : lstat(p, &st)) != 0) {
    *err = errno;
    return -1;
  }
  if (S_ISREG(st.st_mode)) return st.st_size;
  if (S_ISLNK(st.st_mode) && !follow) return 0;
  if (!S_ISDIR(st.st_mode)) return 0;

  DIR *d = opendir(p);
  if (!d) {
    *err = errno;
    return -1;
  }
  int64_t total = 0;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    char child[4096];
    size_t lp = strlen(p);
    snprintf(child, sizeof child, "%s%s%s", p,
             (lp && p[lp - 1] == PATH_SEP) ? "" : "/", de->d_name);
    int64_t s = du_walk(child, follow, err);
    if (s < 0) {
      closedir(d);
      return -1;
    }
    total += s;
  }
  closedir(d);
  return total;
#endif
}
static int vfs_du(VL_State *S) {
  const char *p = fs_check_str(S, 1);
  int follow = fs_opt_bool(S, 2, 0);
  int err = 0;
  int64_t sz = du_walk(p, follow, &err);
  if (sz < 0) return push_errno(S, err ? err : EIO);
  vl_push_int(S, sz);
  return 1;
}

// fs.glob(pattern) -> USV paths
static int vfs_glob(VL_State *S) {
  const char *pat = fs_check_str(S, 1);
  AuxBuffer out = {0};
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return push_errno(S, errno ? errno : ENOENT);
  }
  do {
    usv_append_field(&out, fd.cFileName);
    usv_end_row(&out);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  glob_t g;
  memset(&g, 0, sizeof g);
  int rc = glob(pat, GLOB_TILDE, NULL, &g);
  if (rc != 0) {
    globfree(&g);
    return push_errno(S, ENOENT);
  }
  for (size_t i = 0; i < g.gl_pathc; i++) {
    usv_append_field(&out, g.gl_pathv[i]);
    usv_end_row(&out);
  }
  globfree(&g);
#endif
  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

// fs.tmpdir()
static int vfs_tmpdir(VL_State *S) {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetTempPathA((DWORD)sizeof buf, buf);
  if (n == 0 || n >= sizeof buf) return push_errno(S, EIO);
  vl_push_string(S, buf);
  return 1;
#else
  const char *c = getenv("TMPDIR");
  if (!c) c = "/tmp";
  vl_push_string(S, c);
  return 1;
#endif
}

// fs.home()
static int vfs_home(VL_State *S) {
#ifdef _WIN32
  const char *p = getenv("USERPROFILE");
  if (!p) p = getenv("HOMEDRIVE");
  if (!p) p = "";
  vl_push_string(S, p);
  return 1;
#else
  const char *p = getenv("HOME");
  if (!p) p = "";
  vl_push_string(S, p);
  return 1;
#endif
}

// fs.sep()
static int vfs_sep(VL_State *S) {
#ifdef _WIN32
  vl_push_string(S, "\\");
  return 1;
#else
  vl_push_string(S, "/");
  return 1;
#endif
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------
static const VL_Reg fslib[] = {{"readfile", vfs_readfile},
                               {"writefile", vfs_writefile},
                               {"appendfile", vfs_appendfile},
                               {"exists", vfs_exists},

                               {"stat", vfs_stat},
                               {"lstat", vfs_lstat},
                               {"realpath", vfs_realpath},

                               {"readdir", vfs_readdir},
                               {"mkdir", vfs_mkdir},
                               {"mkdirp", vfs_mkdirp},
                               {"rmdir", vfs_rmdir},

                               {"rename", vfs_rename},
                               {"unlink", vfs_unlink},
                               {"symlink", vfs_symlink},
                               {"readlink", vfs_readlink},
                               {"chmod", vfs_chmod},
                               {"utime", vfs_utime},
                               {"copyfile", vfs_copyfile},
                               {"du", vfs_du},

                               {"glob", vfs_glob},
                               {"tmpdir", vfs_tmpdir},
                               {"home", vfs_home},
                               {"sep", vfs_sep},

                               {NULL, NULL}};

void vl_open_fslib(VL_State *S) { vl_register_lib(S, "fs", fslib); }
