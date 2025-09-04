// vitte-light/libraries/iolib.c
// IO utility layer: filesystem, dirs, temp, subprocess, text and binary
// helpers. Portable C99. No hard dependency on the VM. Works standalone.
//
// Highlights
//  - File queries: exists, is_file/dir, size, mtime_ns
//  - File ops: read_all/write_all (text or binary), copy, rename, remove
//  - Dirs: mkdir_p, list_dir (optionally recursive), simple glob (* ?)
//  - Temp: tmpdir, mktemp file/dir
//  - Subprocess: exec & capture stdout, get exit code
//  - Hexdump helper forwards to core/zio.c when available
//
// Ask for iolib.h if you want a public header. All functions prefixed `vl_`.
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -c libraries/iolib.c

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#else
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

// Optional forward decl from core/zio.c for hexdump
void vl_hexdump(const void *data, size_t n, size_t base_off, FILE *out);

// ───────────────────────── Small utils ─────────────────────────
static size_t vl_strlcpy(char *dst, const char *src, size_t n) {
  size_t L = src ? strlen(src) : 0;
  if (n) {
    size_t m = (L >= n) ? n - 1 : L;
    if (m) memcpy(dst, src, m);
    dst[m] = '\0';
  }
  return L;
}
static size_t vl_strlcat(char *dst, const char *src, size_t n) {
  size_t d = dst ? strlen(dst) : 0;
  if (d >= n) return d + (src ? strlen(src) : 0);
  return d + vl_strlcpy(dst + d, src, n - d);
}
static int is_sep(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\\
';
#else
  return c == '/';
#endif
}
static int path_join(char *out, size_t n, const char *a, const char *b) {
  if (!out || n == 0) return 0;
  out[0] = '\0';
  if (!a || !*a) return (int)(vl_strlcpy(out, b ? b : "", n) < n);
  if (!b || !*b) return (int)(vl_strlcpy(out, a, n) < n);
  size_t la = strlen(a);
  int need_sep = !(la && is_sep(a[la - 1]));
  vl_strlcpy(out, a, n);
  if (need_sep) vl_strlcat(out, "/", n);
  return (int)(vl_strlcat(out, b, n) < n);
}

// ───────────────────────── File queries ─────────────────────────
int vl_file_exists(const char *path) {
  if (!path || !*path) return 0;
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(path);
  return (attr != INVALID_FILE_ATTRIBUTES);
#else
  return access(path, F_OK) == 0;
#endif
}

int vl_is_dir(const char *path) {
  if (!path || !*path) return 0;
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(path);
  return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int vl_is_file(const char *path) {
  if (!path || !*path) return 0;
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(path);
  return (attr != INVALID_FILE_ATTRIBUTES) &&
         !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

int vl_file_size(const char *path, uint64_t *out) {
  if (!path || !out) return 0;
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
  ULARGE_INTEGER u;
  u.HighPart = fad.nFileSizeHigh;
  u.LowPart = fad.nFileSizeLow;
  *out = (uint64_t)u.QuadPart;
  return 1;
#else
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  *out = (uint64_t)st.st_size;
  return 1;
#endif
}

int vl_file_mtime_ns(const char *path, uint64_t *out) {
  if (!path || !out) return 0;
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
  ULARGE_INTEGER u;
  u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
  u.LowPart = fad.ftLastWriteTime.dwLowDateTime;  // 100-ns since 1601
  uint64_t ns = (uint64_t)u.QuadPart * 100ull;
  *out = ns;
  return 1;
#else
#if defined(CLOCK_REALTIME)
  struct stat st;
  if (stat(path, &st) != 0) return 0;
#if defined(__APPLE__) || defined(__MACH__)
  *out = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull +
         (uint64_t)st.st_mtimespec.tv_nsec;
  return 1;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L
  *out = (uint64_t)st.st_mtim.tv_sec * 1000000000ull +
         (uint64_t)st.st_mtim.tv_nsec;
  return 1;
#else
  *out = (uint64_t)st.st_mtime * 1000000000ull;
  return 1;
#endif
#else
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  *out = (uint64_t)st.st_mtime * 1000000000ull;
  return 1;
#endif
#endif
}

// ───────────────────────── Read / Write ─────────────────────────
int vl_read_file_all(const char *path, unsigned char **buf, size_t *n) {
  if (!path || !buf || !n) return 0;
  *buf = NULL;
  *n = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return 0;
  }
  rewind(f);
  unsigned char *p = (unsigned char *)malloc((size_t)sz);
  if (!p) {
    fclose(f);
    return 0;
  }
  size_t rd = fread(p, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    free(p);
    return 0;
  }
  *buf = p;
  *n = (size_t)sz;
  return 1;
}

int vl_write_file_all(const char *path, const void *data, size_t n) {
  if (!path) return 0;
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  size_t wr = data && n ? fwrite(data, 1, n, f) : 0;
  int ok = (wr == n && fflush(f) == 0 && fclose(f) == 0);
  if (!ok) fclose(f);
  return ok;
}

int vl_copy_file(const char *src, const char *dst, int overwrite) {
  if (!src || !dst) return 0;
#if defined(_WIN32)
  if (!overwrite) {
    DWORD attr = GetFileAttributesA(dst);
    if (attr != INVALID_FILE_ATTRIBUTES) return 0;
  }
  return CopyFileA(src, dst, overwrite ? FALSE : TRUE);
#else
  FILE *in = fopen(src, "rb");
  if (!in) return 0;
  FILE *out = fopen(dst, overwrite ? "wb" : "wbx");
  if (!out) {
    fclose(in);
    return 0;
  }
  char buf[1 << 16];
  size_t rd;
  int ok = 1;
  while ((rd = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, rd, out) != rd) {
      ok = 0;
      break;
    }
  }
  if (ferror(in)) ok = 0;
  ok = ok && (fflush(out) == 0) && (fclose(out) == 0);
  fclose(in);
  return ok;
#endif
}

int vl_rename_file(const char *src, const char *dst) {
  if (!src || !dst) return 0;
  return rename(src, dst) == 0;
}
int vl_remove_file(const char *path) {
  if (!path) return 0;
  return remove(path) == 0;
}

// ───────────────────────── mkdir -p ─────────────────────────
static int mk_single_dir(const char *p) {
  if (!p || !*p) return 1;
#if defined(_WIN32)
  if (_mkdir(p) == 0) return 1;
  if (errno == EEXIST) return 1;
  return 0;
#else
  if (mkdir(p, 0777) == 0) return 1;
  if (errno == EEXIST) return 1;
  return 0;
#endif
}

int vl_mkdir_p(const char *path) {
  if (!path || !*path) return 0;
  char tmp[PATH_MAX];
  vl_strlcpy(tmp, path, sizeof(tmp));
  size_t L = strlen(tmp);
#if defined(_WIN32)
  for (size_t i = 0; i < L; i++) {
    if (tmp[i] == '/')
      tmp[i] = '\\\
';
  }
#endif
  size_t i = 0;
#if defined(_WIN32)
  if (L >= 2 && isalpha((unsigned char)tmp[0]) && tmp[1] == ':') i = 2;
  if (L >= 2 && is_sep(tmp[0]) && is_sep(tmp[1])) {
    i = 2;
    while (i < L && !is_sep(tmp[i])) i++;
    while (i < L && is_sep(tmp[i])) i++;
  }
#else
  if (L >= 1 && tmp[0] == '/') i = 1;
  while (i < L && tmp[i] == '/') i++;
#endif
  for (; i <= L; i++) {
    if (tmp[i] == 0 || is_sep(tmp[i])) {
      char c = tmp[i];
      tmp[i] = 0;
      if (!mk_single_dir(tmp)) return 0;
      tmp[i] = c;
      while (i < L && is_sep(tmp[i])) i++;
    }
  }
  return 1;
}

// ───────────────────────── Directory listing ─────────────────────────
typedef struct VL_DirEntry {
  char *path;
  int is_dir;
  uint64_t size;
} VL_DirEntry;

typedef struct VL_DirList {
  VL_DirEntry *v;
  size_t n, cap;
} VL_DirList;

static int push_dirent(VL_DirList *dl, const char *path, int is_dir,
                       uint64_t size) {
  if (dl->n == dl->cap) {
    size_t nc = dl->cap ? dl->cap * 2 : 64;
    void *nv = realloc(dl->v, nc * sizeof(VL_DirEntry));
    if (!nv) return 0;
    dl->v = (VL_DirEntry *)nv;
    dl->cap = nc;
  }
  size_t L = strlen(path);
  char *p = (char *)malloc(L + 1);
  if (!p) return 0;
  memcpy(p, path, L + 1);
  dl->v[dl->n++] = (VL_DirEntry){p, is_dir, size};
  return 1;
}

static int wildcard_match(const char *pat,
                          const char *s) {  // * and ? only, ASCII
  const char *star = NULL, *ss = NULL;
  while (*s) {
    if (*pat == '*') {
      star = ++pat;
      ss = s;
      continue;
    }
    if (*pat == '?' || *pat == *s) {
      pat++;
      s++;
      continue;
    }
    if (star) {
      pat = star;
      s = ++ss;
      continue;
    }
    return 0;
  }
  while (*pat == '*') pat++;
  return *pat == '\0';
}

#if defined(_WIN32)
static int list_dir_win(const char *dir, const char *pattern, int recursive,
                        VL_DirList *out) {
  char spec[PATH_MAX];
  if (pattern && *pattern)
    path_join(spec, sizeof(spec), dir, pattern);
  else
    path_join(spec, sizeof(spec), dir, "*");
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(spec, &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  char path[PATH_MAX];
  do {
    const char *name = fd.cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    path_join(path, sizeof(path), dir, name);
    int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    uint64_t sz =
        ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
    if (!push_dirent(out, path, isdir, sz)) {
      FindClose(h);
      return 0;
    }
    if (recursive && isdir) {
      list_dir_win(path, pattern, recursive, out);
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return 1;
}
#else
static int list_dir_posix(const char *dir, const char *pattern, int recursive,
                          VL_DirList *out) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct dirent *e;
  char path[PATH_MAX];
  while ((e = readdir(d))) {
    const char *name = e->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    path_join(path, sizeof(path), dir, name);
    int isdir = 0;
    uint64_t sz = 0;
    struct stat st;
    if (lstat(path, &st) == 0) {
      isdir = S_ISDIR(st.st_mode);
      sz = (uint64_t)st.st_size;
    }
    if (!pattern || wildcard_match(pattern, name)) {
      if (!push_dirent(out, path, isdir, sz)) {
        closedir(d);
        return 0;
      }
    }
    if (recursive && isdir) {
      if (!list_dir_posix(path, pattern, recursive, out)) {
        closedir(d);
        return 0;
      }
    }
  }
  closedir(d);
  return 1;
}
#endif

int vl_list_dir(const char *dir, const char *pattern, int recursive,
                VL_DirEntry **out_v, size_t *out_n) {
  if (!dir || !out_v || !out_n) return 0;
  VL_DirList dl = {0};
  int ok = 0;
#if defined(_WIN32)
  ok = list_dir_win(dir, pattern, recursive, &dl);
#else
  ok = list_dir_posix(dir, pattern, recursive, &dl);
#endif
  if (!ok) {
    free(dl.v);
    return 0;
  }
  *out_v = dl.v;
  *out_n = dl.n;
  return 1;
}

void vl_direntries_free(VL_DirEntry *v, size_t n) {
  if (!v) return;
  for (size_t i = 0; i < n; i++) {
    free(v[i].path);
  }
  free(v);
}

// ───────────────────────── Temp helpers ─────────────────────────
int vl_tmpdir(char *out, size_t n) {
  if (!out || n == 0) return 0;
#if defined(_WIN32)
  DWORD m = GetTempPathA((DWORD)n, out);
  if (m == 0 || m >= n) return 0;
  size_t L = strlen(out);
  if (L && !is_sep(out[L - 1])) vl_strlcat(out, "/", n);
  return 1;
#else
  const char *td = getenv("TMPDIR");
  if (!td || !*td) td = "/tmp";
  return (int)(vl_strlcpy(out, td, n) < n);
#endif
}

static uint32_t xorshift32(void) {
  static uint32_t s = 0x12345678u;
  uint32_t x = s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s = x;
  return x;
}

int vl_mktemp_file(char *out, size_t n, const char *prefix) {
  if (!out || n < 8) return 0;
  char dir[PATH_MAX];
  if (!vl_tmpdir(dir, sizeof(dir))) return 0;
  for (int i = 0; i < 100; i++) {
    char cand[PATH_MAX];
    snprintf(cand, sizeof(cand), "%s%s%08x.tmp", dir, prefix ? prefix : "vl_",
             xorshift32());
    FILE *f = fopen(cand, "wbx");
    if (f) {
      fclose(f);
      return (int)(vl_strlcpy(out, cand, n) < n);
    }
  }
  return 0;
}

int vl_mktemp_dir(char *out, size_t n, const char *prefix) {
  if (!out || n < 8) return 0;
  char dir[PATH_MAX];
  if (!vl_tmpdir(dir, sizeof(dir))) return 0;
  for (int i = 0; i < 100; i++) {
    char cand[PATH_MAX];
    snprintf(cand, sizeof(cand), "%s%s%08x.d", dir, prefix ? prefix : "vl_",
             xorshift32());
    if (vl_mkdir_p(cand)) return (int)(vl_strlcpy(out, cand, n) < n);
  }
  return 0;
}

// ───────────────────────── Subprocess ─────────────────────────
int vl_exec_capture(const char *cmd, char **out_text, size_t *out_len,
                    int *out_status) {
  if (!cmd || !out_text || !out_len) return 0;
  *out_text = NULL;
  *out_len = 0;
  if (out_status) *out_status = -1;
#if defined(_WIN32)
  FILE *p = _popen(cmd, "rt");
  if (!p) return 0;
#else
  FILE *p = popen(cmd, "r");
  if (!p) return 0;
#endif
  size_t cap = 4096, n = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) {
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    return 0;
  }
  for (;;) {
    if (cap - n < 1024) {
      size_t nc = cap * 2;
      char *nb = (char *)realloc(buf, nc);
      if (!nb) {
        free(buf);
#if defined(_WIN32)
        _pclose(p);
#else
        pclose(p);
#endif
        return 0;
      }
      buf = nb;
      cap = nc;
    }
    size_t rd = fread(buf + n, 1, cap - n, p);
    n += rd;
    if (rd == 0) break;
  }
#if defined(_WIN32)
  int st = _pclose(p);
#else
  int st = pclose(p);
#endif
  buf = (char *)realloc(buf, n + 1);
  if (!buf) return 0;
  buf[n] = '\0';
  *out_text = buf;
  *out_len = n;
  if (out_status) *out_status = st;
  return 1;
}

// ───────────────────────── Hexdump passthrough ─────────────────────────
void vl_hexdump_file(const char *path, size_t base_off, FILE *out) {
  if (!out) out = stdout;
  unsigned char *b = NULL;
  size_t n = 0;
  if (!vl_read_file_all(path, &b, &n)) {
    fprintf(out, "<read fail %s>\n", path ? path : "(null)");
    return;
  }
  if (vl_hexdump)
    vl_hexdump(b, n, base_off, out);
  else {  // fallback minimal
    for (size_t i = 0; i < n; i++) {
      if ((i % 16) == 0) fprintf(out, "%08zx  ", base_off + i);
      fprintf(out, "%02X ", b[i]);
      if ((i % 16) == 15) fputc('\n', out);
    }
    if (n % 16) fputc('\n', out);
  }
  free(b);
}

// ───────────────────────── Self-test ─────────────────────────
#ifdef VL_IOLIB_TEST_MAIN
int main(int argc, char **argv) {
  printf("exists('/'): %d\n", vl_file_exists("/"));
  char tfile[PATH_MAX];
  if (vl_mktemp_file(tfile, sizeof(tfile), "demo_")) {
    vl_write_file_all(tfile, "hello", 5);
    uint64_t sz = 0;
    vl_file_size(tfile, &sz);
    printf("tmp file %s size=%" PRIu64 "\n", tfile, sz);
    vl_hexdump_file(tfile, 0, stdout);
    vl_remove_file(tfile);
  }
  VL_DirEntry *v = NULL;
  size_t n = 0;
  if (vl_list_dir(".", "*.c", 0, &v, &n)) {
    for (size_t i = 0; i < n; i++) {
      printf("%s%s\n", v[i].path, v[i].is_dir ? "/" : "");
    }
    vl_direntries_free(v, n);
  }
  if (argc > 1) {
    char *out = NULL;
    size_t on = 0;
    int st = 0;
    if (vl_exec_capture(argv[1], &out, &on, &st)) {
      printf("[%d] %.*s\n", st, (int)on, out);
      free(out);
    }
  }
  return 0;
}
#endif
