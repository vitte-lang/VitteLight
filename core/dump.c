/* ============================================================================
   dump.c — utilitaire binaire ultra complet (C17, cross-platform)
   Fonctions:
     - info        : type (ELF/PE/Mach-O), taille, horodatage, permissions
     - hexdump     : hex/ascii, colonnes/groupes, offset/longueur
     - strings     : extraction chaînes ASCII ou UTF-16LE, seuil min
     - hash        : CRC32, SHA-256
     - entropy     : Shannon globale ou fenêtrée
     - diff        : comparaison de deux fichiers, résumé + contexte
     - slice       : extrait une tranche vers un fichier
   Dépendances: standard C. Optionnel: debug.h (VT_*). Sinon fallback.
   Build (POSIX): cc -std=c17 -O2 dump.c -o dump
   Build (Win)  : cl /std:c17 /O2 dump.c
   Licence: MIT.
   ============================================================================
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ---------------------------------------------------------------------------
   Logging: utilise debug.h si présent, sinon macros fallback
--------------------------------------------------------------------------- */
#if __has_include("debug.h")
#include "debug.h"
#define D_LOG_INIT()       \
  do {                     \
    vt_log_config c = {0}; \
    c.level = VT_LL_INFO;  \
    vt_log_init(&c);       \
  } while (0)
#define D_LOG_SHUTDOWN() vt_log_shutdown()
#define D_INFO(...) VT_INFO(__VA_ARGS__)
#define D_WARN(...) VT_WARN(__VA_ARGS__)
#define D_ERROR(...) VT_ERROR(__VA_ARGS__)
#else
#define D_LOG_INIT() ((void)0)
#define D_LOG_SHUTDOWN() ((void)0)
#define D_INFO(...)               \
  do {                            \
    fprintf(stderr, "[INFO] ");   \
    fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr);          \
  } while (0)
#define D_WARN(...)               \
  do {                            \
    fprintf(stderr, "[WARN] ");   \
    fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr);          \
  } while (0)
#define D_ERROR(...)              \
  do {                            \
    fprintf(stderr, "[ERR ] ");   \
    fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr);          \
  } while (0)
#endif

#if defined(VT_ENABLE_TOOLS)

/* ---------------------------------------------------------------------------
   Utilitaires généraux
--------------------------------------------------------------------------- */
static int streq(const char* a, const char* b) {
  return a && b && strcmp(a, b) == 0;
}

static int strieq(const char* a, const char* b) {
  if (!a || !b) return 0;
  while (*a && *b) {
    char ca = (char)((*a >= 'A' && *a <= 'Z') ? *a - 'A' + 'a' : *a);
    char cb = (char)((*b >= 'A' && *b <= 'Z') ? *b - 'A' + 'a' : *b);
    if (ca != cb) return 0;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

static uint64_t parse_u64(const char* s, int* ok) {
  if (!s || !*s) {
    if (ok) *ok = 0;
    return 0;
  }
  char* end = NULL;
  errno = 0;
#if defined(_MSC_VER)
  unsigned long long v = _strtoui64(s, &end, 0);
#else
  unsigned long long v = strtoull(s, &end, 0);
#endif
  if (errno || end == s) {
    if (ok) *ok = 0;
    return 0;
  }
  if (ok) *ok = 1;
  return (uint64_t)v;
}

/* suffixes k,m,g, % (pourcentage de fichier si size connue) */
static uint64_t parse_size_suff(const char* s, uint64_t total, int* ok) {
  if (!s) {
    if (ok) *ok = 0;
    return 0;
  }
  size_t n = strlen(s);
  if (n == 0) {
    if (ok) *ok = 0;
    return 0;
  }
  char suf = s[n - 1];
  if (suf == '%' && total > 0) {
    char buf[64];
    if (n >= sizeof buf) {
      if (ok) *ok = 0;
      return 0;
    }
    memcpy(buf, s, n - 1);
    buf[n - 1] = 0;
    int oknum = 0;
    uint64_t pct = parse_u64(buf, &oknum);
    if (!oknum) {
      if (ok) *ok = 0;
      return 0;
    }
    if (pct > 100) pct = 100;
    if (ok) *ok = 1;
    return (total * pct) / 100u;
  }
  uint64_t mul = 1;
  if (suf == 'k' || suf == 'K' || suf == 'm' || suf == 'M' || suf == 'g' ||
      suf == 'G') {
    if (suf == 'k' || suf == 'K')
      mul = 1024ull;
    else if (suf == 'm' || suf == 'M')
      mul = 1024ull * 1024ull;
    else
      mul = 1024ull * 1024ull * 1024ull;
    char buf[64];
    if (n >= sizeof buf) {
      if (ok) *ok = 0;
      return 0;
    }
    memcpy(buf, s, n - 1);
    buf[n - 1] = 0;
    int oknum = 0;
    uint64_t v = parse_u64(buf, &oknum);
    if (!oknum) {
      if (ok) *ok = 0;
      return 0;
    }
    if (ok) *ok = 1;
    return v * mul;
  }
  int oknum = 0;
  uint64_t v = parse_u64(s, &oknum);
  if (!oknum) {
    if (ok) *ok = 0;
    return 0;
  }
  if (ok) *ok = 1;
  return v;
}

/* Clamp add */
static size_t add_s(size_t a, size_t b) {
  size_t m = (size_t)-1;
  if (m - a < b) return m;
  return a + b;
}

/* ---------------------------------------------------------------------------
   Mappage fichier (mmap/MapViewOfFile) + fallback fread
--------------------------------------------------------------------------- */
typedef struct {
  const uint8_t* ptr;
  size_t len;
#if defined(_WIN32)
  HANDLE hFile, hMap;
#endif
  int via_map; /* 1=map, 0=malloc */
} map_t;

static int map_file(const char* path, map_t* m) {
  memset(m, 0, sizeof *m);
#if defined(_WIN32)
  HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hf == INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(hf, &sz)) {
    CloseHandle(hf);
    return -1;
  }
  if (sz.QuadPart == 0) {
    m->ptr = (const uint8_t*)"";
    m->len = 0;
    m->via_map = 1;
    m->hFile = hf;
    m->hMap = NULL;
    return 0;
  }
  HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!hm) {
    CloseHandle(hf);
    return -1;
  }
  void* v = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
  if (!v) {
    /* fallback fread */
    CloseHandle(hm);
    CloseHandle(hf);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
      fclose(f);
      return -1;
    }
    long L = ftell(f);
    if (L < 0) {
      fclose(f);
      return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
      fclose(f);
      return -1;
    }
    uint8_t* buf = (uint8_t*)malloc((size_t)L);
    if (!buf) {
      fclose(f);
      return -2;
    }
    size_t rd = fread(buf, 1, (size_t)L, f);
    fclose(f);
    if (rd != (size_t)L) {
      free(buf);
      return -1;
    }
    m->ptr = buf;
    m->len = rd;
    m->via_map = 0;
    return 0;
  }
  m->ptr = (const uint8_t*)v;
  m->len = (size_t)sz.QuadPart;
  m->via_map = 1;
  m->hFile = hf;
  m->hMap = hm;
  return 0;
#else
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  if (st.st_size == 0) {
    m->ptr = (const uint8_t*)"";
    m->len = 0;
    m->via_map = 1;
    close(fd);
    return 0;
  }
  void* v = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (v == MAP_FAILED) {
    /* fallback fread */
    FILE* f = fdopen(fd, "rb");
    if (!f) {
      close(fd);
      return -1;
    }
    uint8_t* buf = (uint8_t*)malloc((size_t)st.st_size);
    if (!buf) {
      fclose(f);
      return -2;
    }
    size_t rd = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (rd != (size_t)st.st_size) {
      free(buf);
      return -1;
    }
    m->ptr = buf;
    m->len = rd;
    m->via_map = 0;
    return 0;
  }
  m->ptr = (const uint8_t*)v;
  m->len = (size_t)st.st_size;
  m->via_map = 1;
  close(fd);
  return 0;
#endif
}

static void unmap_file(map_t* m) {
#if defined(_WIN32)
  if (m->via_map && m->ptr && m->len) UnmapViewOfFile(m->ptr);
  if (m->hMap) CloseHandle(m->hMap);
  if (m->hFile && m->hFile != INVALID_HANDLE_VALUE) CloseHandle(m->hFile);
#else
  if (m->via_map && m->ptr && m->len) munmap((void*)m->ptr, m->len);
#endif
  if (!m->via_map && m->ptr && m->len) free((void*)m->ptr);
  memset(m, 0, sizeof *m);
}

/* ---------------------------------------------------------------------------
   Détection type (ELF/PE/Mach-O)
--------------------------------------------------------------------------- */
typedef enum { FT_UNKNOWN = 0, FT_ELF, FT_PE, FT_MACHO } ftype_t;

static ftype_t detect_type(const uint8_t* p, size_t n) {
  if (n >= 4 && p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F')
    return FT_ELF;
  if (n >= 64 && p[0] == 'M' && p[1] == 'Z') {
    uint32_t peoff = 0;
    if (n >= 0x3C + 4) {
      peoff = (uint32_t)p[0x3C] | ((uint32_t)p[0x3D] << 8) |
              ((uint32_t)p[0x3E] << 16) | ((uint32_t)p[0x3F] << 24);
    }
    if (peoff + 4 <= n && p[peoff] == 'P' && p[peoff + 1] == 'E' &&
        p[peoff + 2] == 0 && p[peoff + 3] == 0)
      return FT_PE;
  }
  if (n >= 4) {
    uint32_t m = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
    if (m == 0xFEEDFACEu || m == 0xFEEDFACFu || m == 0xCAFEBABEu ||
        m == 0xCEFAEDFEu || m == 0xCFFAEDFEu)
      return FT_MACHO;
  }
  return FT_UNKNOWN;
}

static const char* ftype_name(ftype_t t) {
  switch (t) {
    case FT_ELF:
      return "ELF";
    case FT_PE:
      return "PE";
    case FT_MACHO:
      return "Mach-O";
    default:
      return "unknown";
  }
}

/* ---------------------------------------------------------------------------
   Hexdump
--------------------------------------------------------------------------- */
static void hexdump(const uint8_t* p, size_t len, uint64_t base, int cols,
                    int group, int ascii) {
  if (cols <= 0) cols = 16;
  if (group <= 0) group = 1;
  char ascii_buf[256];
  for (size_t off = 0; off < len; off += (size_t)cols) {
    size_t n = (len - off < (size_t)cols) ? (len - off) : (size_t)cols;
    printf("%08llx  ", (unsigned long long)(base + off));
    /* hex area */
    for (int i = 0; i < cols; ++i) {
      if (i < (int)n) {
        printf("%02x", p[off + i]);
      } else {
        printf("  ");
      }
      if (i + 1 < cols) {
        if (group > 0 && ((i + 1) % group) == 0) putchar(' ');
      }
    }
    if (ascii) {
      /* pad spaces to align ascii block */
      int hexchars = cols * 2 + (cols - 1) / group + 1;
      if (group <= 1) hexchars = cols * 2 + 1;
      int printed =
          (int)(n * 2 + ((n - 1) >= 0 ? ((int)((n - 1) / group) + 1) : 0));
      for (int s = printed; s < hexchars; ++s) putchar(' ');
      /* ascii */
      int k = 0;
      for (size_t i = 0; i < n; i++) {
        unsigned char c = p[off + i];
        ascii_buf[k++] = (c >= 32 && c <= 126) ? (char)c : '.';
      }
      ascii_buf[k] = 0;
      printf("%s", ascii_buf);
    }
    putchar('\n');
  }
}

/* ---------------------------------------------------------------------------
   CRC32 (IEEE 802.3) + SHA-256 (petite implémentation)
--------------------------------------------------------------------------- */
static uint32_t crc32_tab[256];
static void crc32_init(void) {
  uint32_t poly = 0xEDB88320u;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++) c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
    crc32_tab[i] = c;
  }
}
static uint32_t crc32_compute(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) c = crc32_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* ---- SHA-256 ---- */
typedef struct {
  uint32_t h[8];
  uint64_t bits;
  uint8_t buf[64];
  size_t blen;
} sha256_t;
static uint32_t ROR(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}
static uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}
static uint32_t BSIG0(uint32_t x) {
  return ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22);
}
static uint32_t BSIG1(uint32_t x) {
  return ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25);
}
static uint32_t SSIG0(uint32_t x) { return ROR(x, 7) ^ ROR(x, 18) ^ (x >> 3); }
static uint32_t SSIG1(uint32_t x) {
  return ROR(x, 17) ^ ROR(x, 19) ^ (x >> 10);
}

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static void sha256_init(sha256_t* s) {
  s->h[0] = 0x6a09e667u;
  s->h[1] = 0xbb67ae85u;
  s->h[2] = 0x3c6ef372u;
  s->h[3] = 0xa54ff53au;
  s->h[4] = 0x510e527fu;
  s->h[5] = 0x9b05688cu;
  s->h[6] = 0x1f83d9abu;
  s->h[7] = 0x5be0cd19u;
  s->bits = 0;
  s->blen = 0;
}
static void sha256_block(sha256_t* s, const uint8_t b[64]) {
  uint32_t W[64];
  for (int i = 0; i < 16; i++) {
    W[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) |
           ((uint32_t)b[4 * i + 2] << 8) | ((uint32_t)b[4 * i + 3]);
  }
  for (int i = 16; i < 64; i++)
    W[i] = SSIG1(W[i - 2]) + W[i - 7] + SSIG0(W[i - 15]) + W[i - 16];
  uint32_t a = s->h[0], b0 = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4],
           f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t T1 = h + BSIG1(e) + Ch(e, f, g) + K256[i] + W[i];
    uint32_t T2 = BSIG0(a) + Maj(a, b0, c);
    h = g;
    g = f;
    f = e;
    e = d + T1;
    d = c;
    c = b0;
    b0 = a;
    a = T1 + T2;
  }
  s->h[0] += a;
  s->h[1] += b0;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}
static void sha256_update(sha256_t* s, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  s->bits += (uint64_t)len * 8u;
  if (s->blen) {
    size_t need = 64 - s->blen;
    size_t take = (len < need) ? len : need;
    memcpy(s->buf + s->blen, p, take);
    s->blen += take;
    p += take;
    len -= take;
    if (s->blen == 64) {
      sha256_block(s, s->buf);
      s->blen = 0;
    }
  }
  while (len >= 64) {
    sha256_block(s, p);
    p += 64;
    len -= 64;
  }
  if (len) {
    memcpy(s->buf, p, len);
    s->blen = len;
  }
}
static void sha256_final(sha256_t* s, uint8_t out[32]) {
  /* padding */
  uint8_t pad[64] = {0x80};
  size_t padlen = (s->blen < 56) ? (56 - s->blen) : (120 - s->blen);
  sha256_update(s, pad, padlen);
  uint8_t lenb[8];
  for (int i = 0; i < 8; i++)
    lenb[7 - i] = (uint8_t)((s->bits >> (i * 8)) & 0xFFu);
  sha256_update(s, lenb, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i + 0] = (uint8_t)(s->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)(s->h[i]);
  }
}

/* ---------------------------------------------------------------------------
   Entropie de Shannon (0..8 bits) + version fenêtrée
--------------------------------------------------------------------------- */
static double entropy_shannon(const uint8_t* p, size_t n) {
  if (n == 0) return 0.0;
  uint32_t hist[256] = {0};
  for (size_t i = 0; i < n; i++) hist[p[i]]++;
  double H = 0.0;
  for (int i = 0; i < 256; i++) {
    if (!hist[i]) continue;
    double q = (double)hist[i] / (double)n;
    H -= q * (log(q) / log(2.0));
  }
  return H;
}

static void entropy_window(const uint8_t* p, size_t n, size_t win,
                           size_t step) {
  if (win == 0) win = 4096;
  if (step == 0) step = win;
  for (size_t off = 0; off < n;) {
    size_t m = (n - off < win) ? (n - off) : win;
    double H = entropy_shannon(p + off, m);
    printf("%08llx  len=%5zu  H=%.4f\n", (unsigned long long)off, m, H);
    if (n - off <= step) break;
    off += step;
  }
}

/* ---------------------------------------------------------------------------
   Strings extraction (ASCII et UTF-16LE)
--------------------------------------------------------------------------- */
static int is_printable_ascii(unsigned c) { return (c >= 32 && c <= 126); }

static void extract_strings_ascii(const uint8_t* p, size_t n, size_t min_len) {
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && is_printable_ascii(p[j])) j++;
    size_t L = j - i;
    if (L >= min_len) {
      fwrite(p + i, 1, L, stdout);
      fputc('\n', stdout);
    }
    if (j == n) break;
    i = j + 1;
  }
}

static void extract_strings_utf16le(const uint8_t* p, size_t n,
                                    size_t min_len) {
  size_t i = 0;
  while (i + 1 < n) {
    size_t j = i;
    size_t chars = 0;
    while (j + 1 < n) {
      uint16_t u = (uint16_t)p[j] | ((uint16_t)p[j + 1] << 8);
      if (!(u >= 32 && u <= 126)) break; /* ASCII dans U+0020..U+007E */
      j += 2;
      chars++;
    }
    if (chars >= min_len) {
      /* convert to ASCII */
      for (size_t k = 0; k < chars; k++) {
        putchar((char)p[i + 2 * k]);
      }
      putchar('\n');
    }
    if (j == n) break;
    i = j + 2;
  }
}

/* ---------------------------------------------------------------------------
   Diff binaire
--------------------------------------------------------------------------- */
static void diff_files(const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
                       size_t context, int summary_only) {
  size_t n = (na < nb) ? na : nb;
  size_t diffs = 0, first_off = (size_t)-1, last_off = 0;

  /* pass 1: stats */
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i]) {
      diffs++;
      if (first_off == (size_t)-1) first_off = i;
      last_off = i;
    }
  }
  diffs += (na > nb) ? (na - nb) : (nb - na);
  D_INFO("diff: A=%zu bytes, B=%zu bytes, common=%zu, total_diffs=%zu", na, nb,
         n, diffs);
  if (diffs == 0) {
    puts("identical");
    return;
  }

  if (summary_only) {
    printf("first_diff=0x%zx last_diff=0x%zx\n", first_off, last_off);
    return;
  }

  /* pass 2: show hunks */
  size_t i = 0;
  while (i < n) {
    if (a[i] == b[i]) {
      i++;
      continue;
    }
    size_t start = (i > context) ? i - context : 0;
    size_t end = i;
    while (end < n && a[end] != b[end]) end++;
    size_t endctx = (end + context < n) ? (end + context) : n;

    printf("\n@@ 0x%zx..0x%zx (len=%zu)\n", start, endctx, endctx - start);
    /* show side by side in hex ascii */
    size_t off = start;
    while (off < endctx) {
      size_t m = (endctx - off < 16) ? (endctx - off) : 16;
      printf("%08zx  ", off);
      for (size_t k = 0; k < m; k++) printf("%02x", a[off + k]);
      for (size_t k = m; k < 16; k++) printf("  ");
      printf("  |  ");
      for (size_t k = 0; k < m; k++) printf("%02x", b[off + k]);
      for (size_t k = m; k < 16; k++) printf("  ");
      printf("  |  ");
      for (size_t k = 0; k < m; k++) {
        unsigned ca = a[off + k];
        putchar((ca >= 32 && ca <= 126) ? (char)ca : '.');
      }
      printf(" | ");
      for (size_t k = 0; k < m; k++) {
        unsigned cb = b[off + k];
        putchar((cb >= 32 && cb <= 126) ? (char)cb : '.');
      }
      putchar('\n');
      off += m;
    }
    i = end;
  }
}

/* ---------------------------------------------------------------------------
   Info fichier: taille, type, permissions, mtime
--------------------------------------------------------------------------- */
static void print_file_info(const char* path, const uint8_t* p, size_t n) {
  ftype_t t = detect_type(p, n);
  printf("path: %s\n", path);
  printf("size: %zu bytes\n", n);
  printf("type: %s\n", ftype_name(t));
#if !defined(_WIN32)
  struct stat st;
  if (stat(path, &st) == 0) {
    char mode[11] = "----------";
    if (S_ISDIR(st.st_mode)) mode[0] = 'd';
    if (S_ISLNK(st.st_mode)) mode[0] = 'l';
    if (st.st_mode & S_IRUSR) mode[1] = 'r';
    if (st.st_mode & S_IWUSR) mode[2] = 'w';
    if (st.st_mode & S_IXUSR) mode[3] = 'x';
    if (st.st_mode & S_IRGRP) mode[4] = 'r';
    if (st.st_mode & S_IWGRP) mode[5] = 'w';
    if (st.st_mode & S_IXGRP) mode[6] = 'x';
    if (st.st_mode & S_IROTH) mode[7] = 'r';
    if (st.st_mode & S_IWOTH) mode[8] = 'w';
    if (st.st_mode & S_IXOTH) mode[9] = 'x';
    printf("mode: %s\n", mode);
    char ts[64];
    struct tm tmv;
    localtime_r(&st.st_mtime, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    printf("mtime: %s\n", ts);
    printf("uid: %u gid: %u\n", (unsigned)st.st_uid, (unsigned)st.st_gid);
  }
#else
  /* Windows: simple timestamp via GetFileAttributesEx */
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
    ULARGE_INTEGER ui;
    ui.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    ui.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    time_t secs = (time_t)((ui.QuadPart - 116444736000000000ULL) / 10000000ULL);
    char ts[64];
    struct tm* tmv = localtime(&secs);
    if (tmv) {
      strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", tmv);
      printf("mtime: %s\n", ts);
    }
    printf("attrs: 0x%08x\n", (unsigned)fad.dwFileAttributes);
  }
#endif
}

/* ---------------------------------------------------------------------------
   Slice
--------------------------------------------------------------------------- */
static int write_slice(const char* out, const uint8_t* p, size_t off,
                       size_t len, size_t total) {
  if (off > total) {
    D_ERROR("offset > size");
    return -1;
  }
  size_t avail = total - off;
  if (len > avail) len = avail;
  FILE* f = fopen(out, "wb");
  if (!f) {
    D_ERROR("open out failed: %s", strerror(errno));
    return -1;
  }
  size_t wr = fwrite(p + off, 1, len, f);
  fclose(f);
  if (wr != len) {
    D_ERROR("write truncated");
    return -1;
  }
  D_INFO("wrote %zu bytes to %s", len, out);
  return 0;
}

/* ---------------------------------------------------------------------------
   Usage
--------------------------------------------------------------------------- */
static void usage(const char* prog) {
  fprintf(stderr,
          "Usage: %s <command> [options]\n"
          "Commands:\n"
          "  info <file>\n"
          "  hexdump <file> [--cols N] [--group N] [--ascii on|off] [--offset "
          "OFF] [--length LEN]\n"
          "  strings <file> [--min N] [--utf16]\n"
          "  hash <file> [--crc32] [--sha256]\n"
          "  entropy <file> [--window N] [--step N]\n"
          "  diff <A> <B> [--context N] [--summary]\n"
          "  slice <file> --offset OFF --length LEN --out PATH\n"
          "Notes: OFF/LEN accept 0x..., suffixes k/m/g, or percent (e.g., 10%% "
          "of file).\n",
          prog);
}

/* ---------------------------------------------------------------------------
   main
--------------------------------------------------------------------------- */
int main(int argc, char** argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  D_LOG_INIT();
  crc32_init();

  const char* cmd = argv[1];

  /* ---------------- info ---------------- */
  if (streq(cmd, "info")) {
    const char* path = argv[2];
    map_t m;
    if (map_file(path, &m) != 0) {
      D_ERROR("open: %s", strerror(errno));
      D_LOG_SHUTDOWN();
      return 1;
    }
    print_file_info(path, m.ptr, m.len);
    unmap_file(&m);
    D_LOG_SHUTDOWN();
    return 0;
  }

  /* ---------------- hexdump ---------------- */
  if (streq(cmd, "hexdump")) {
    const char* path = argv[2];
    int cols = 16, group = 1, ascii = 1;
    uint64_t off = 0, len = (uint64_t)-1;
    /* parse options */
    for (int i = 3; i < argc; i++) {
      if (streq(argv[i], "--cols") && i + 1 < argc) {
        int ok;
        cols = (int)parse_u64(argv[++i], &ok);
      } else if (streq(argv[i], "--group") && i + 1 < argc) {
        int ok;
        group = (int)parse_u64(argv[++i], &ok);
      } else if (streq(argv[i], "--ascii") && i + 1 < argc) {
        ascii = strieq(argv[++i], "on") ? 1 : 0;
      } else if (streq(argv[i], "--offset") && i + 1 < argc) {
        int ok = 0;
        off = parse_size_suff(argv[++i], 0, &ok);
      } else if (streq(argv[i], "--length") && i + 1 < argc) {
        int ok = 0;
        len = parse_size_suff(argv[++i], 0, &ok);
      }
    }
    map_t m;
    if (map_file(path, &m) != 0) {
      D_ERROR("open: %s", strerror(errno));
      D_LOG_SHUTDOWN();
      return 1;
    }
    if (off > m.len) {
      D_ERROR("offset beyond EOF");
      unmap_file(&m);
      D_LOG_SHUTDOWN();
      return 1;
    }
    size_t avail = m.len - (size_t)off;
    size_t take =
        (len == (uint64_t)-1) ? avail : (size_t)((len > avail) ? avail : len);
    hexdump(m.ptr + off, take, off, cols, group, ascii);
    unmap_file(&m);
    D_LOG_SHUTDOWN();
    return 0;
  }

  /* ---------------- strings ---------------- */
  if (streq(cmd, "strings")) {
    const char* path = argv[2];
    size_t minlen = 4;
    int utf16 = 0;
    for (int i = 3; i < argc; i++) {
      if (streq(argv[i], "--min") && i + 1 < argc) {
        int ok;
        minlen = (size_t)parse_u64(argv[++i], &ok);
      } else if (streq(argv[i], "--utf16"))
        utf16 = 1;
    }
    map_t m;
    if (map_file(path, &m) != 0) {
      D_ERROR("open: %s", strerror(errno));
      D_LOG_SHUTDOWN();
      return 1;
    }
    if (utf16)
      extract_strings_utf16le(m.ptr, m.len, minlen);
    else
      extract_strings_ascii(m.ptr, m.len, minlen);
    unmap_file(&m);
    D_LOG_SHUTDOWN();
    return 0;
  }

  /* ---------------- hash ---------------- */
  if (streq(cmd, "hash")) {
    const char* path = argv[2];
    int want_crc = 0, want_sha = 0;
    for (int i = 3; i < argc; i++) {
      if (streq(argv[i], "--crc32"))
        want_crc = 1;
      else if (streq(argv[i], "--sha256"))
        want_sha = 1;
    }
    if (!want_crc && !want_sha) {
      want_crc = 1;
      want_sha = 1;
    }
    map_t m;
    if (map_file(path, &m) != 0) {
      D_ERROR("open: %s", strerror(errno));
      D_LOG_SHUTDOWN();
      return 1;
    }
    if (want_crc) {
      uint32_t c = crc32_compute(m.ptr, m.len);
      printf("CRC32: %08x\n", c);
    }
    if (want_sha) {
      sha256_t s;
      sha256_init(&s);
      sha256_update(&s, m.ptr, m.len);
      uint8_t out[32];
      sha256_final(&s, out);
      printf("SHA256: ");
      for (int i = 0; i < 32; i++) printf("%02x", out[i]);
      putchar('\n');
    }
    unmap_file(&m);
    D_LOG_SHUTDOWN();
    return 0;
  }

  /* ---------------- entropy ---------------- */
  if (streq(cmd, "entropy")) {
    const char* path = argv[2];
    size_t win = 0, step = 0;
    for (int i = 3; i < argc; i++) {
      if (streq(argv[i], "--window") && i + 1 < argc) {
        int ok;
        win = (size_t)parse_u64(argv[++i], &ok);
      } else if (streq(argv[i], "--step") && i + 1 < argc) {
        int ok;
        step = (size_t)parse_u64(argv[++i], &ok);
      }
    }
    map_t m;
    if (map_file(path, &m) != 0) {
      D_ERROR("open: %s", strerror(errno));
      D_LOG_SHUTDOWN();
      return 1;
    }
    if (win == 0) {
      double H = entropy_shannon(m.ptr, m.len);
      printf("size=%zu  H=%.6f bits/byte\n", m.len, H);
    } else {
      entropy_window(m.ptr, m.len, win, step);
    }
    unmap_file(&m);
    D_LOG_SHUTDOWN();
    return 0;
  }

  /* ---------------- diff ---------------- */
  if (streq(cmd, "diff")) {
    if (argc < 4) {
      usage(argv[0]);
      D_LOG_SHUTDOWN();
      return 2;
    }
    const char *A = argv[2], *B = argv[3];
    size_t context = 16;
    int summary = 0;
    for (int i = 4; i < argc; i++) {
      if (streq(argv[i], "--context") && i + 1 < argc) {
        int ok;
        context = (size_t)parse_u64(argv[++i], &ok);
      } else if (streq(argv[i], "--summary"))
        summary = 1;
    }
    map_t a, b;
    if (map_file(A, &a) != 0) {
      D_ERROR("open A: %s", strerror(errno));
      D_LOG_SHUTDOWN();
      return 1;
    }
    if (map_file(B, &b) != 0) {
      D_ERROR("open B: %s", strerror(errno));
      unmap_file(&a);
      D_LOG_SHUTDOWN();
      return 1;
    }
    diff_files(a.ptr, a.len, b.ptr, b.len, context, summary);
    unmap_file(&a);
    unmap_file(&b);
    D_LOG_SHUTDOWN();
    return 0;
  }

  /* ---------------- slice ---------------- */
  if (streq(cmd, "slice")) {
    const char* path = argv[2];
    const char* out = NULL;
    uint64_t off = 0, len = 0, have_off = 0, have_len = 0;
    for (int i = 3; i < argc; i++) {
      if (streq(argv[i], "--offset") && i + 1 < argc) {
        int ok;
        off = parse_size_suff(argv[++i], 0, &ok);
        have_off = 1;
      } else if (streq(argv[i], "--length") && i + 1 < argc) {
        int ok;
        len = parse_size_suff(argv[++i], 0, &ok);
        have_len = 1;
      } else if (streq(argv[i], "--out") && i + 1 < argc) {
        out = argv[++i];
      }
    }
    if (!out || !have_len) {
      usage(argv[0]);
      D_LOG_SHUTDOWN();
      return 2;
    }
    map_t m;
    if (map_file(path, &m) != 0) {
      D_ERROR("open: %s", strerror(errno));
      D_LOG_SHUTDOWN();
      return 1;
    }
    if (!have_off) off = 0;
    if (write_slice(out, m.ptr, (size_t)off, (size_t)len, m.len) != 0) {
      unmap_file(&m);
      D_LOG_SHUTDOWN();
      return 1;
    }
    unmap_file(&m);
    D_LOG_SHUTDOWN();
    return 0;
  }

  usage(argv[0]);
  D_LOG_SHUTDOWN();
  return 2;
}

#endif /* VT_ENABLE_TOOLS */
