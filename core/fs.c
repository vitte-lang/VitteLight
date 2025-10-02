/* ============================================================================
   fs.c — Utilitaires fichiers/système ultra complets (C11, portable)
   - API UTF-8 partout. Windows: conversion UTF-8 <-> UTF-16 et Win32W.
   - Existence, type, stat, taille, lecture/écriture intégrale, copie/déplacement
   - mkdir -p, remove récursif, itération de répertoire, chemins (join, norm)
   - Répertoires: home, temp, cwd. Gestion des permissions minimale.
   - Intégration autonome: expose l’API si fs.h absent.
   Licence: MIT
   ============================================================================
*/
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN 1
#  include <windows.h>
#  include <shlwapi.h>
#  pragma comment(lib, "Shlwapi.lib")
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

/* ----------------------------------------------------------------------------
   API publique (si fs.h absent)
---------------------------------------------------------------------------- */
#ifndef VT_FS_HAVE_HEADER
#ifndef VT_FS_API
#define VT_FS_API extern
#endif

typedef struct {
  uint64_t size;      /* bytes */
  uint64_t mtime_s;   /* seconds since epoch (best-effort) */
  int is_file;
  int is_dir;
  uint32_t mode;      /* POSIX st_mode bits si dispo, sinon 0 */
} vt_fs_stat;

typedef int (*vt_fs_dir_cb)(const char* path_utf8, const char* name_utf8,
                            int is_dir, void* user);

VT_FS_API int  vt_fs_exists(const char* path);
VT_FS_API int  vt_fs_is_file(const char* path);
VT_FS_API int  vt_fs_is_dir(const char* path);
VT_FS_API int  vt_fs_stat_path(const char* path, vt_fs_stat* st);

VT_FS_API int  vt_fs_mkdir(const char* path);          /* 0/-1 */
VT_FS_API int  vt_fs_mkdirs(const char* path);         /* mkdir -p */
VT_FS_API int  vt_fs_remove_file(const char* path);    /* 0/-1 */
VT_FS_API int  vt_fs_rmdir(const char* path);          /* 0/-1 (empty only) */
VT_FS_API int  vt_fs_remove_all(const char* path);     /* recursive */

VT_FS_API int  vt_fs_read_all(const char* path, char** out_buf, size_t* out_len); /* malloc+NUL */
VT_FS_API int  vt_fs_write_all(const char* path, const void* data, size_t len);   /* overwrite */
VT_FS_API int  vt_fs_copy_file(const char* src, const char* dst, int overwrite);  /* 0/-1 */
VT_FS_API int  vt_fs_move(const char* src, const char* dst, int overwrite);       /* 0/-1 */

VT_FS_API int  vt_fs_iterdir(const char* dir, vt_fs_dir_cb cb, void* user);       /* 0/-1 */

VT_FS_API int  vt_fs_cwd(char* out, size_t cap);
VT_FS_API int  vt_fs_chdir(const char* path);
VT_FS_API int  vt_fs_tempdir(char* out, size_t cap);
VT_FS_API int  vt_fs_homedir(char* out, size_t cap);

VT_FS_API void vt_fs_path_join(char* out, size_t cap, const char* a, const char* b);
VT_FS_API void vt_fs_path_norm(char* io, int to_posix_sep); /* normalise séparateurs */
VT_FS_API const char* vt_fs_basename(const char* path);
VT_FS_API void vt_fs_dirname(const char* path, char* out, size_t cap);

#endif /* VT_FS_HAVE_HEADER */

/* ----------------------------------------------------------------------------
   Helpers communs
---------------------------------------------------------------------------- */
static int vt__is_sep(char c) {
#if defined(_WIN32)
  return (c=='\\' || c=='/');
#else
  return (c=='/');
#endif
}
static void vt__sncpy(char* out, size_t cap, const char* s) {
  if (!out || cap==0) return;
  if (!s) { out[0]=0; return; }
  #if defined(_MSC_VER)
    strncpy_s(out, cap, s, _TRUNCATE);
  #else
    snprintf(out, cap, "%s", s);
  #endif
}

/* ----------------------------------------------------------------------------
   UTF-8 <-> Wide (Windows) + pass-through POSIX
---------------------------------------------------------------------------- */
#if defined(_WIN32)
static int vt__u8_to_w(const char* s, wchar_t* wout, int wcap) {
  if (!s) return 0;
  return MultiByteToWideChar(CP_UTF8, 0, s, -1, wout, wcap);
}
static int vt__w_to_u8(const wchar_t* ws, char* out, int cap) {
  if (!ws) return 0;
  return WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, cap, NULL, NULL);
}
#endif

/* ----------------------------------------------------------------------------
   Existence / type / stat
---------------------------------------------------------------------------- */
int vt_fs_exists(const char* path) {
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return 0;
  DWORD attr = GetFileAttributesW(w);
  return attr != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st; return stat(path, &st) == 0;
#endif
}
int vt_fs_is_dir(const char* path) {
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return 0;
  DWORD attr = GetFileAttributesW(w);
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st; if (stat(path, &st)!=0) return 0; return S_ISDIR(st.st_mode);
#endif
}
int vt_fs_is_file(const char* path) {
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return 0;
  DWORD attr = GetFileAttributesW(w);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st; if (stat(path, &st)!=0) return 0; return S_ISREG(st.st_mode);
#endif
}

int vt_fs_stat_path(const char* path, vt_fs_stat* st) {
  if (!st) return -1;
  memset(st, 0, sizeof *st);
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return -1;
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a)) return -1;
  st->is_dir  = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st->is_file = !st->is_dir;
  ULARGE_INTEGER sz; sz.LowPart=a.nFileSizeLow; sz.HighPart=a.nFileSizeHigh;
  st->size = (uint64_t)sz.QuadPart;
  /* FILETIME -> epoch seconds */
  ULARGE_INTEGER t; t.LowPart=a.ftLastWriteTime.dwLowDateTime; t.HighPart=a.ftLastWriteTime.dwHighDateTime;
  uint64_t ns100 = t.QuadPart; /* 100-ns since 1601 */
  if (ns100) {
    uint64_t unix_s = (ns100/10000000ULL) - 11644473600ULL;
    st->mtime_s = unix_s;
  }
  st->mode = 0;
  return 0;
#else
  struct stat sb;
  if (stat(path, &sb)!=0) return -1;
  st->size = (uint64_t)sb.st_size;
  st->mtime_s =
#if defined(__APPLE__)
    (uint64_t)sb.st_mtimespec.tv_sec;
#else
    (uint64_t)sb.st_mtime;
#endif
  st->is_dir  = S_ISDIR(sb.st_mode);
  st->is_file = S_ISREG(sb.st_mode);
  st->mode = (uint32_t)sb.st_mode;
  return 0;
#endif
}

/* ----------------------------------------------------------------------------
   mkdir / rmdir / remove_all
---------------------------------------------------------------------------- */
int vt_fs_mkdir(const char* path) {
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return -1;
  if (CreateDirectoryW(w, NULL)) return 0;
  if (GetLastError()==ERROR_ALREADY_EXISTS && vt_fs_is_dir(path)) return 0;
  return -1;
#else
  return mkdir(path, 0777) == 0 || (errno==EEXIST && vt_fs_is_dir(path)) ? 0 : -1;
#endif
}

int vt_fs_mkdirs(const char* path) {
  if (!path || !*path) return -1;
  char buf[32768]; vt__sncpy(buf, sizeof buf, path);
  vt_fs_path_norm(buf, 0);
  size_t n = strlen(buf);
  if (n==0) return -1;

  /* Crée chaque composant */
  for (size_t i=1; i<n; ++i) {
    if (!vt__is_sep(buf[i])) continue;
    char c = buf[i]; buf[i]=0;
    if (*buf) { if (vt_fs_mkdir(buf)!=0 && !vt_fs_is_dir(buf)) return -1; }
    buf[i]=c;
  }
  return vt_fs_mkdir(buf);
}

int vt_fs_remove_file(const char* path) {
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return -1;
  if (DeleteFileW(w)) return 0;
  /* Si c’est un dossier vide, essayer RemoveDirectory */
  if (RemoveDirectoryW(w)) return 0;
  return -1;
#else
  return unlink(path)==0 ? 0 : -1;
#endif
}

int vt_fs_rmdir(const char* path) {
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return -1;
  return RemoveDirectoryW(w) ? 0 : -1;
#else
  return rmdir(path)==0 ? 0 : -1;
#endif
}

/* Remove recursively file or directory tree */
int vt_fs_remove_all(const char* path) {
  if (!vt_fs_exists(path)) return 0;
  if (vt_fs_is_dir(path)) {
#if defined(_WIN32)
    wchar_t wpat[32768]; wchar_t w[32768];
    if (!vt__u8_to_w(path, w, 32768)) return -1;
    /* pattern path\* */
    wcsncpy(wpat, w, 32760);
    size_t L = wcslen(wpat);
    if (L && wpat[L-1] != L'\\' && wpat[L-1] != L'/') { wpat[L++] = L'\\'; wpat[L]=0; }
    wcscat_s(wpat, 32768, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h==INVALID_HANDLE_VALUE) goto remove_dir;
    do {
      if (wcscmp(fd.cFileName, L".")==0 || wcscmp(fd.cFileName, L"..")==0) continue;
      wchar_t child[32768];
      wcsncpy(child, w, 32760);
      size_t k = wcslen(child);
      if (k && child[k-1] != L'\\' && child[k-1] != L'/') { child[k++]=L'\\'; child[k]=0; }
      wcscat_s(child, 32768, fd.cFileName);
      char u8[32768]; vt__w_to_u8(child, u8, 32768);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (vt_fs_remove_all(u8)!=0) { FindClose(h); return -1; }
      } else {
        if (DeleteFileW(child)==0) { FindClose(h); return -1; }
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
remove_dir:
    return RemoveDirectoryW(w) ? 0 : -1;
#else
    DIR* d = opendir(path);
    if (!d) return -1;
    struct dirent* ent;
    char child[4096];
    while ((ent = readdir(d))) {
      if (strcmp(ent->d_name,".")==0 || strcmp(ent->d_name,"..")==0) continue;
      vt_fs_path_join(child, sizeof child, path, ent->d_name);
      if (vt_fs_is_dir(child)) {
        if (vt_fs_remove_all(child)!=0) { closedir(d); return -1; }
      } else {
        if (unlink(child)!=0) { closedir(d); return -1; }
      }
    }
    closedir(d);
    return rmdir(path)==0 ? 0 : -1;
#endif
  } else {
    return vt_fs_remove_file(path);
  }
}

/* ----------------------------------------------------------------------------
   Lecture / écriture intégrales
---------------------------------------------------------------------------- */
int vt_fs_read_all(const char* path, char** out_buf, size_t* out_len) {
  if (!out_buf) return -1;
  *out_buf = NULL; if (out_len) *out_len = 0;

#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return -1;
  HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h==INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER sz; if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return -1; }
  if (sz.QuadPart > SIZE_MAX) { CloseHandle(h); return -1; }
  size_t n = (size_t)sz.QuadPart;
  char* buf = (char*)malloc(n+1); if (!buf) { CloseHandle(h); return -1; }
  size_t off=0;
  while (off < n) {
    DWORD chunk=0; if (!ReadFile(h, buf+off, (DWORD)(n-off), &chunk, NULL)) { free(buf); CloseHandle(h); return -1; }
    if (chunk==0) break;
    off += chunk;
  }
  CloseHandle(h);
  buf[off] = 0;
  *out_buf = buf; if (out_len) *out_len = off;
  return 0;
#else
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END)!=0) { fclose(f); return -1; }
  long n = ftell(f); if (n<0) { fclose(f); return -1; }
  rewind(f);
  char* buf = (char*)malloc((size_t)n + 1); if (!buf) { fclose(f); return -1; }
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[rd] = 0;
  *out_buf = buf; if (out_len) *out_len = rd;
  return 0;
#endif
}

int vt_fs_write_all(const char* path, const void* data, size_t len) {
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return -1;
  HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h==INVALID_HANDLE_VALUE) return -1;
  size_t off=0;
  const char* p = (const char*)data;
  while (off < len) {
    DWORD wr=0; if (!WriteFile(h, p+off, (DWORD)(len-off), &wr, NULL)) { CloseHandle(h); return -1; }
    if (wr==0) break;
    off += wr;
  }
  CloseHandle(h);
  return off==len ? 0 : -1;
#else
  FILE* f = fopen(path, "wb");
  if (!f) return -1;
  size_t wr = data && len ? fwrite(data, 1, len, f) : 0;
  int rc = ferror(f) ? -1 : 0;
  if (fclose(f)!=0) rc = -1;
  return (rc==0 && wr==len) ? 0 : -1;
#endif
}

/* ----------------------------------------------------------------------------
   Copie / déplacement
---------------------------------------------------------------------------- */
int vt_fs_copy_file(const char* src, const char* dst, int overwrite) {
#if defined(_WIN32)
  wchar_t ws[32768], wd[32768];
  if (!vt__u8_to_w(src, ws, 32768) || !vt__u8_to_w(dst, wd, 32768)) return -1;
  BOOL ok = CopyFileW(ws, wd, overwrite ? FALSE : TRUE);
  return ok ? 0 : -1;
#else
  int in = open(src, O_RDONLY);
  if (in<0) return -1;
  struct stat st; fstat(in, &st);
  int out = open(dst, O_WRONLY | O_CREAT | (overwrite?O_TRUNC:O_EXCL), st.st_mode & 0777);
  if (out<0) { close(in); return -1; }

  char buf[1<<16];
  ssize_t n;
  int rc=0;
  while ((n = read(in, buf, sizeof buf)) > 0) {
    char* p = buf; ssize_t left=n;
    while (left>0) {
      ssize_t w = write(out, p, (size_t)left);
      if (w<0) { rc=-1; goto done; }
      left -= w; p += w;
    }
  }
  if (n<0) rc=-1;
done:
  close(in); if (close(out)!=0) rc=-1;
  return rc;
#endif
}

int vt_fs_move(const char* src, const char* dst, int overwrite) {
#if defined(_WIN32)
  wchar_t ws[32768], wd[32768];
  if (!vt__u8_to_w(src, ws, 32768) || !vt__u8_to_w(dst, wd, 32768)) return -1;
  DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH;
  if (overwrite) flags |= MOVEFILE_REPLACE_EXISTING;
  return MoveFileExW(ws, wd, flags) ? 0 : -1;
#else
  if (overwrite) (void)unlink(dst);
  if (rename(src, dst)==0) return 0;
  /* cross-device: fallback copy+remove */
  if (errno==EXDEV) {
    if (vt_fs_copy_file(src, dst, overwrite)==0) return vt_fs_remove_file(src);
  }
  return -1;
#endif
}

/* ----------------------------------------------------------------------------
   Itération de répertoire
---------------------------------------------------------------------------- */
int vt_fs_iterdir(const char* dir, vt_fs_dir_cb cb, void* user) {
  if (!dir || !cb) return -1;
#if defined(_WIN32)
  wchar_t wdir[32768]; if (!vt__u8_to_w(dir, wdir, 32768)) return -1;
  wchar_t pat[32768];
  wcsncpy(pat, wdir, 32760);
  size_t L = wcslen(pat);
  if (L && pat[L-1] != L'\\' && pat[L-1] != L'/') { pat[L++]=L'\\'; pat[L]=0; }
  wcscat_s(pat, 32768, L"*");
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pat, &fd);
  if (h==INVALID_HANDLE_VALUE) return -1;
  int rc=0;
  do {
    if (wcscmp(fd.cFileName, L".")==0 || wcscmp(fd.cFileName, L"..")==0) continue;
    char name[1024]; vt__w_to_u8(fd.cFileName, name, 1024);
    char full[4096];
    char diru8[4096]; vt__w_to_u8(wdir, diru8, 4096);
    vt_fs_path_join(full, sizeof full, diru8, name);
    int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((rc = cb(full, name, isdir, user)) != 0) break;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return rc;
#else
  DIR* d = opendir(dir);
  if (!d) return -1;
  int rc=0;
  struct dirent* e;
  char full[4096];
  while ((e = readdir(d))) {
    if (strcmp(e->d_name,".")==0 || strcmp(e->d_name,"..")==0) continue;
    vt_fs_path_join(full, sizeof full, dir, e->d_name);
    int isdir = vt_fs_is_dir(full);
    rc = cb(full, e->d_name, isdir, user);
    if (rc) break;
  }
  closedir(d);
  return rc;
#endif
}

/* ----------------------------------------------------------------------------
   CWD / chdir / tempdir / homedir
---------------------------------------------------------------------------- */
int vt_fs_cwd(char* out, size_t cap) {
#if defined(_WIN32)
  wchar_t w[32768]; DWORD n = GetCurrentDirectoryW(32768, w);
  if (n==0 || n>=32768) return -1;
  return vt__w_to_u8(w, out, (int)cap) ? 0 : -1;
#else
  return getcwd(out, cap) ? 0 : -1;
#endif
}
int vt_fs_chdir(const char* path) {
#if defined(_WIN32)
  wchar_t w[32768]; if (!vt__u8_to_w(path, w, 32768)) return -1;
  return SetCurrentDirectoryW(w) ? 0 : -1;
#else
  return chdir(path)==0 ? 0 : -1;
#endif
}
int vt_fs_tempdir(char* out, size_t cap) {
#if defined(_WIN32)
  wchar_t w[32768]; DWORD n = GetTempPathW(32768, w);
  if (n==0 || n>=32768) return -1;
  return vt__w_to_u8(w, out, (int)cap) ? 0 : -1;
#else
  const char* s = getenv("TMPDIR");
  if (!s || !*s) s = "/tmp";
  vt__sncpy(out, cap, s);
  return 0;
#endif
}
int vt_fs_homedir(char* out, size_t cap) {
#if defined(_WIN32)
  const char* u = getenv("USERPROFILE");
  if (!u || !*u) {
    const char* h = getenv("HOMEDRIVE"); const char* p = getenv("HOMEPATH");
    if (h && p) {
      char buf[1024]; snprintf(buf, sizeof buf, "%s%s", h, p);
      vt__sncpy(out, cap, buf); return 0;
    }
    return -1;
  }
  vt__sncpy(out, cap, u); return 0;
#else
  const char* h = getenv("HOME");
  if (h && *h) { vt__sncpy(out, cap, h); return 0; }
  return -1;
#endif
}

/* ----------------------------------------------------------------------------
   Chemins: join, norm, basename, dirname
---------------------------------------------------------------------------- */
void vt_fs_path_join(char* out, size_t cap, const char* a, const char* b) {
  if (!out || cap==0) return;
  if (!a || !*a) { vt__sncpy(out, cap, b?b:""); return; }
  if (!b || !*b) { vt__sncpy(out, cap, a); return; }
  size_t na = strlen(a);
  int need_sep = !(na>0 && vt__is_sep(a[na-1]));
#if defined(_WIN32)
  snprintf(out, cap, "%s%s%s", a, need_sep ? "\\" : "", b);
#else
  snprintf(out, cap, "%s%s%s", a, need_sep ? "/" : "", b);
#endif
}

void vt_fs_path_norm(char* io, int to_posix_sep) {
  if (!io) return;
  /* remplace multiples séparateurs par un seul et retire ./ */
  char* r = io; char* w = io;
  int last_sep = 0;
  while (*r) {
    char c = *r++;
#if defined(_WIN32)
    if (!to_posix_sep && c=='/') c='\\';
    if (to_posix_sep && c=='\\') c='/';
    int is_sep = (c=='/' || c=='\\');
#else
    int is_sep = (c=='/');
#endif
    if (is_sep) {
      if (last_sep) continue;
      last_sep = 1;
      *w++ =
#if defined(_WIN32)
        to_posix_sep ? '/' : '\\';
#else
        '/';
#endif
      continue;
    }
    last_sep = 0;
    /* skip "/./" */
    if (c=='.' && (*r=='/' || *r=='\\')) { ++r; last_sep=1; continue; }
    *w++ = c;
  }
  *w = 0;
}

const char* vt_fs_basename(const char* path) {
  if (!path) return "";
  const char* s = path;
#if defined(_WIN32)
  const char *p1 = strrchr(s, '\\'), *p2 = strrchr(s, '/');
  const char* p = (p1 && p2) ? (p1>p2?p1:p2) : (p1?p1:p2);
#else
  const char* p = strrchr(s, '/');
#endif
  return p ? p+1 : s;
}

void vt_fs_dirname(const char* path, char* out, size_t cap) {
  if (!path || !*path) { if (out && cap) out[0]=0; return; }
  char tmp[4096]; vt__sncpy(tmp, sizeof tmp, path);
#if defined(_WIN32)
  char* p1 = strrchr(tmp, '\\');
  char* p2 = strrchr(tmp, '/');
  char* p = (p1 && p2) ? (p1>p2?p1:p2) : (p1?p1:p2);
#else
  char* p = strrchr(tmp, '/');
#endif
  if (!p) { if (out && cap) out[0]=0; return; }
  *p = 0; vt__sncpy(out, cap, tmp);
}

/* ----------------------------------------------------------------------------
   Tests basiques (désactivés)
   cc -std=c11 -DVT_FS_TEST fs.c
---------------------------------------------------------------------------- */
#ifdef VT_FS_TEST
#include <stdio.h>
static int list_cb(const char* full, const char* name, int is_dir, void* u){
  (void)u; printf(" %c %s\n", is_dir?'d':'f', full); return 0;
}
int main(void){
  char tmp[1024]; vt_fs_tempdir(tmp, sizeof tmp);
  char dir[1024]; vt_fs_path_join(dir, sizeof dir, tmp, "vitte_fs_test");
  vt_fs_mkdirs(dir);
  char f[1024]; vt_fs_path_join(f, sizeof f, dir, "hello.txt");
  const char* msg = "hello\n";
  vt_fs_write_all(f, msg, strlen(msg));
  char* buf=NULL; size_t n=0; vt_fs_read_all(f,&buf,&n); printf("read %zu:\n%.*s", n,(int)n,buf); free(buf);
  vt_fs_iterdir(dir, list_cb, NULL);
  char g[1024]; vt_fs_path_join(g,sizeof g,dir,"hello2.txt");
  vt_fs_copy_file(f,g,1);
  vt_fs_move(g,f,1);
  vt_fs_remove_all(dir);
  puts("OK");
  return 0;
}
#endif
