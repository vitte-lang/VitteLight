// vitte-light/libraries/auxlib.c
// Bibliothèque auxiliaire portable pour outils VitteLight.
// Logging, chaînes, chemins, fichiers, mkdir -p, aléa, CRC32.
// Indépendant du runtime VL; n'utilise que la libc. Optionnellement mem.h.
//
// API (demandez `auxlib.h` si vous voulez un header dédié):
//   // Logging
//   enum { VL_LOG_ERROR=0, VL_LOG_WARN=1, VL_LOG_INFO=2, VL_LOG_DEBUG=3 };
//   void vl_log_set_level(int lvl);      // défaut: INFO
//   void vl_log_use_color(int on);       // auto si stderr TTY
//   void vl_logf(int lvl, const char *fmt, ...);
//
//   // Chaînes
//   char *vl_trim_inplace(char *s);      // trim ASCII; retourne s
//   int   vl_strcasecmp_ascii(const char *a, const char *b); //
//   case-insensitive size_t vl_strlcpy(char *dst, const char *src, size_t n);
//   size_t vl_strlcat(char *dst, const char *src, size_t n);
//
//   // Chemins
//   int   vl_path_is_abs(const char *p);
//   int   vl_path_join(char *out, size_t n, const char *a, const char *b);
//   int   vl_path_dirname(const char *path, char *out, size_t n);
//   int   vl_path_basename(const char *path, char *out, size_t n);
//
//   // Fichiers
//   int   vl_file_read_all(const char *path, unsigned char **buf, size_t *n);
//   int   vl_file_write_all(const char *path, const void *data, size_t n);
//   int   vl_mkdir_p(const char *path);  // crée l'arborescence
//
//   // Aléa et CRC
//   int   vl_rand_bytes(void *buf, size_t n);      // 1 si ok
//   unsigned long vl_crc32(const void *data, size_t n); // IEEE 802.3
//
// Build: cc -std=c99 -O2 -Wall -Wextra -pedantic -c libraries/auxlib.c

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#define VL_PATH_SEP1 '\\'
#define VL_PATH_SEP2 '/'
#else
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define VL_PATH_SEP1 '/'
#define VL_PATH_SEP2 '/'
#endif

// ───────────────────────── Logging ─────────────────────────
static int g_log_level = 2;   // INFO
static int g_log_color = -1;  // -1 => auto

static int isatty_stderr(void) {
#ifdef _WIN32
  DWORD mode;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  return h && GetConsoleMode(h, &mode);
#else
  return isatty(2);
#endif
}

void vl_log_set_level(int lvl) {
  if (lvl < 0) lvl = 0;
  if (lvl > 3) lvl = 3;
  g_log_level = lvl;
}
void vl_log_use_color(int on) { g_log_color = on ? 1 : 0; }

static const char *lvl_name(int lvl) {
  switch (lvl) {
    case 0:
      return "ERROR";
    case 1:
      return "WARN";
    case 2:
      return "INFO";
    default:
      return "DEBUG";
  }
}
static const char *lvl_ansi(int lvl) {
  switch (lvl) {
    case 0:
      return "\x1b[31m";
    case 1:
      return "\x1b[33m";
    case 2:
      return "\x1b[36m";
    default:
      return "\x1b[90m";
  }
}

void vl_logf(int lvl, const char *fmt, ...) {
  if (lvl > g_log_level) return;
  int color = (g_log_color == 1) || (g_log_color < 0 && isatty_stderr());
  if (color) fputs(lvl_ansi(lvl), stderr);
  fprintf(stderr, "[%s] ", lvl_name(lvl));
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  if (color) fputs("\x1b[0m", stderr);
  fputc('\n', stderr);
}

// ───────────────────────── Chaînes ─────────────────────────
size_t vl_strlcpy(char *dst, const char *src, size_t n) {
  size_t L = src ? strlen(src) : 0;
  if (n) {
    size_t m = (L >= n) ? n - 1 : L;
    if (m) memcpy(dst, src, m);
    dst[m] = '\0';
  }
  return L;
}
size_t vl_strlcat(char *dst, const char *src, size_t n) {
  size_t d = dst ? strlen(dst) : 0;
  if (d >= n) return d + (src ? strlen(src) : 0);
  return d + vl_strlcpy(dst + d, src, n - d);
}

static char *lstrip(char *s) {
  while (*s && (unsigned char)*s <= 0x20) s++;
  return s;
}
static char *rstrip(char *s) {
  size_t L = strlen(s);
  while (L && (unsigned char)s[L - 1] <= 0x20) L--;
  s[L] = '\0';
  return s;
}
char *vl_trim_inplace(char *s) {
  if (!s) return s;
  return rstrip(lstrip(s));
}

int vl_strcasecmp_ascii(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a || !b) return a ? 1 : -1;
  for (;;) {
    unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
    if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
    if (ca != cb || ca == 0) return (int)ca - (int)cb;
  }
}

// ───────────────────────── Chemins ─────────────────────────
static int is_sep(char c) { return c == VL_PATH_SEP1 || c == VL_PATH_SEP2; }

int vl_path_is_abs(const char *p) {
  if (!p || !*p) return 0;

#ifdef _WIN32
      // ex: C:\ or \\server\share
      if ((strlen(p) >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':') ||
          (strlen(p) >= 2 && is_sep(p[0]) && is_sep(p[1]))) return 1;
  return is_sep(p[0]);
#else
  return p[0] == '/';
#endif
}

int vl_path_join(char *out, size_t n, const char *a, const char *b) {
  if (!out || n == 0) return 0;
  out[0] = '\0';
  if (!a || !*a) return (int)(vl_strlcpy(out, b ? b : "", n) < n);
  if (!b || !*b) return (int)(vl_strlcpy(out, a, n) < n);
  char tmp[PATH_MAX];
  size_t la = strlen(a);
  int need_sep = !(la && is_sep(a[la - 1]));
  size_t k = 0;
  k += vl_strlcpy(tmp + k, a, sizeof(tmp) - k);
  if (need_sep && k < sizeof(tmp) - 1) {
    tmp[k++] = VL_PATH_SEP1;
    tmp[k] = '\0';
  }
  k += vl_strlcpy(tmp + k, b, sizeof(tmp) - k);
  return (int)(vl_strlcpy(out, tmp, n) < n);
}

int vl_path_dirname(const char *path, char *out, size_t n) {
  if (!path || !out || !n) return 0;
  size_t L = strlen(path);
  if (L == 0) {
    out[0] = '.';
    out[1] = '\0';
    return 1;
  }
  size_t i = L;
  while (i > 0 && !is_sep(path[i - 1])) i--;
  if (i == 0) {
    out[0] = '.';
    out[1] = '\0';
    return 1;
  }                                                   // pas de dir => '.'
  while (i > 0 && i > 1 && is_sep(path[i - 1])) i--;  // strip trailing seps
  if (i > n - 1) i = n - 1;
  memcpy(out, path, i);
  out[i] = '\0';
  return 1;
}

int vl_path_basename(const char *path, char *out, size_t n) {
  if (!path || !out || !n) return 0;
  size_t L = strlen(path);
  size_t i = L;
  while (i > 0 && is_sep(path[i - 1])) i--;
  size_t j = i;
  while (j > 0 && !is_sep(path[j - 1])) j--;
  size_t len = i - j;
  if (len > n - 1) len = n - 1;
  memcpy(out, path + j, len);
  out[len] = '\0';
  return 1;
}

// ───────────────────────── Fichiers ─────────────────────────
int vl_file_read_all(const char *path, unsigned char **buf, size_t *n) {
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

int vl_file_write_all(const char *path, const void *data, size_t n) {
  if (!path) return 0;
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  size_t wr = data && n ? fwrite(data, 1, n, f) : 0;
  int ok = (wr == n && fflush(f) == 0 && fclose(f) == 0);
  if (!ok) {
    fclose(f);
  }
#ifdef _WIN32
  (void)ok;  // rien d'autre
#else
  // fsync n'est pas strictement nécessaire pour outils CLI, omis pour
  // simplicité
#endif
  return ok;
}

static int mk_single_dir(const char *p) {
  if (!p || !*p) return 1;

#ifdef _WIN32 if (_mkdir(p) == 0) return 1;
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
  size_t L = strlen(tmp);  // normaliser séparateurs Windows
#ifdef _WIN32
  for (size_t i = 0; i < L; i++) {
    if (tmp[i] == '/') tmp[i] = '\\';
  }
#endif
  // ignorer préfixe absolu (ex: C:\ ou //server)
  size_t i = 0;

#ifdef _WIN32 if (L >= 2 && isalpha((unsigned char)tmp[0]) && tmp[1] == ':')
      i = 2;
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

// ───────────────────────── Aléa ─────────────────────────
int vl_rand_bytes(void *buf, size_t n) {
  if (!buf) return 0;
  unsigned char *p = (unsigned char *)buf;
  size_t done = 0;

#ifdef _WIN32 HCRYPTPROV h = 0;
  if (CryptAcquireContextA(&h, NULL, NULL, PROV_RSA_FULL,
                           CRYPT_VERIFYCONTEXT)) {
    BOOL ok = CryptGenRandom(h, (DWORD)n, p);
    CryptReleaseContext(h, 0);
    return ok ? 1 : 0;
  }
  // fallback PRNG faible
  for (size_t i = 0; i < n; i++) {
    p[i] = (unsigned char)(rand() >> 3);
  }
  return 1;
#else
  FILE *f = fopen("/dev/urandom", "rb");
  if (f) {
    size_t rd = fread(p, 1, n, f);
    fclose(f);
    if (rd == n) return 1;
  }
  // fallback PRNG faible
  for (size_t i = 0; i < n; i++) {
    p[i] = (unsigned char)(rand() >> 3);
  }
  return 1;
#endif
}

// ───────────────────────── CRC32 ─────────────────────────
unsigned long vl_crc32(const void *data, size_t n) {
  const unsigned char *p = (const unsigned char *)data;
  unsigned long c = ~0ul;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) {
      unsigned long m = -(c & 1ul);
      c = (c >> 1) ^ (0xEDB88320ul & m);
    }
  }
  return ~c;
}

// ───────────────────────── Tests ─────────────────────────
#ifdef VL_AUXLIB_TEST_MAIN
int main(void) {
  vl_logf(VL_LOG_INFO, "test auxlib");
  char b[64];
  vl_path_join(b, sizeof(b), "/usr", "bin");
  printf("join=%s\n", b);
  printf("basename:");
  vl_path_basename("/a/b/c.txt", b, sizeof(b));
  puts(b);
  printf("dirname:");
  vl_path_dirname("/a/b/c.txt", b, sizeof(b));
  puts(b);
  unsigned char r[16];
  vl_rand_bytes(r, sizeof(r));
  printf("crc32=%08lX\n", vl_crc32(r, sizeof(r)));
  return 0;
}
#endif
