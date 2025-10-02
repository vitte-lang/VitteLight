/* ============================================================================
   io.c — I/O utilitaires portables (C17, MIT)
   - Lecture/écriture intégrale fichiers (UTF-8 paths)
   - read/write lignes, stdout/stderr helpers
   - fd utils (POSIX), HANDLE (Windows)
   - UTF-8 <-> Wide conversion sur Windows
   ============================================================================ */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN 1
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

#ifndef VT_IO_API
#define VT_IO_API extern
#endif

/* ----------------------------------------------------------------------------
   API publique (si io.h absent)
---------------------------------------------------------------------------- */
#ifndef VT_IO_HAVE_HEADER
VT_IO_API int  vt_io_read_all(const char* path, char** out_buf, size_t* out_len);
VT_IO_API int  vt_io_write_all(const char* path, const void* buf, size_t len);
VT_IO_API int  vt_io_append_all(const char* path, const void* buf, size_t len);
VT_IO_API char* vt_io_read_line(FILE* f, size_t* out_len);
VT_IO_API void vt_io_writef(FILE* f, const char* fmt, ...);
VT_IO_API void vt_io_printf(const char* fmt, ...);
VT_IO_API void vt_io_eprintf(const char* fmt, ...);
#endif

/* ----------------------------------------------------------------------------
   Helpers
---------------------------------------------------------------------------- */
static void* xmalloc(size_t n) {
  void* p = malloc(n ? n : 1);
  if (!p) { fprintf(stderr, "OOM %zu\n", n); abort(); }
  return p;
}

/* ----------------------------------------------------------------------------
   Read whole file
---------------------------------------------------------------------------- */
int vt_io_read_all(const char* path, char** out_buf, size_t* out_len) {
  if (!out_buf) return -1;
  *out_buf = NULL;
  if (out_len) *out_len = 0;

#if defined(_WIN32)
  wchar_t wpath[32768];
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 32768);
  HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return -1; }
  if (sz.QuadPart > SIZE_MAX) { CloseHandle(h); return -1; }
  size_t n = (size_t)sz.QuadPart;
  char* buf = (char*)xmalloc(n + 1);
  DWORD read = 0;
  size_t off = 0;
  while (off < n) {
    if (!ReadFile(h, buf + off, (DWORD)(n - off), &read, NULL)) { free(buf); CloseHandle(h); return -1; }
    if (!read) break;
    off += read;
  }
  CloseHandle(h);
  buf[off] = 0;
  *out_buf = buf;
  if (out_len) *out_len = off;
  return 0;
#else
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return -1; }
  rewind(f);
  char* buf = (char*)xmalloc((size_t)sz + 1);
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = 0;
  *out_buf = buf;
  if (out_len) *out_len = rd;
  return 0;
#endif
}

/* ----------------------------------------------------------------------------
   Write file (overwrite)
---------------------------------------------------------------------------- */
int vt_io_write_all(const char* path, const void* buf, size_t len) {
#if defined(_WIN32)
  wchar_t wpath[32768];
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 32768);
  HANDLE h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  size_t off = 0;
  while (off < len) {
    DWORD wr = 0;
    if (!WriteFile(h, (const char*)buf + off, (DWORD)(len - off), &wr, NULL)) {
      CloseHandle(h);
      return -1;
    }
    if (!wr) break;
    off += wr;
  }
  CloseHandle(h);
  return (off == len) ? 0 : -1;
#else
  FILE* f = fopen(path, "wb");
  if (!f) return -1;
  size_t wr = fwrite(buf, 1, len, f);
  int rc = ferror(f) ? -1 : 0;
  if (fclose(f) != 0) rc = -1;
  return (rc == 0 && wr == len) ? 0 : -1;
#endif
}

/* ----------------------------------------------------------------------------
   Append file
---------------------------------------------------------------------------- */
int vt_io_append_all(const char* path, const void* buf, size_t len) {
#if defined(_WIN32)
  wchar_t wpath[32768];
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 32768);
  HANDLE h = CreateFileW(wpath, FILE_APPEND_DATA, FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  size_t off = 0;
  while (off < len) {
    DWORD wr = 0;
    if (!WriteFile(h, (const char*)buf + off, (DWORD)(len - off), &wr, NULL)) {
      CloseHandle(h);
      return -1;
    }
    if (!wr) break;
    off += wr;
  }
  CloseHandle(h);
  return (off == len) ? 0 : -1;
#else
  FILE* f = fopen(path, "ab");
  if (!f) return -1;
  size_t wr = fwrite(buf, 1, len, f);
  int rc = ferror(f) ? -1 : 0;
  if (fclose(f) != 0) rc = -1;
  return (rc == 0 && wr == len) ? 0 : -1;
#endif
}

/* ----------------------------------------------------------------------------
   Read one line (malloc, strip \n)
---------------------------------------------------------------------------- */
char* vt_io_read_line(FILE* f, size_t* out_len) {
  if (!f) return NULL;
  size_t cap = 128, len = 0;
  char* buf = (char*)xmalloc(cap);
  int c;
  while ((c = fgetc(f)) != EOF) {
    if (len + 1 >= cap) {
      cap *= 2;
      buf = realloc(buf, cap);
      if (!buf) return NULL;
    }
    if (c == '\n') break;
    buf[len++] = (char)c;
  }
  if (len == 0 && c == EOF) { free(buf); return NULL; }
  buf[len] = 0;
  if (out_len) *out_len = len;
  return buf;
}

/* ----------------------------------------------------------------------------
   printf-like wrappers
---------------------------------------------------------------------------- */
void vt_io_writef(FILE* f, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
}
void vt_io_printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stdout, fmt, ap);
  va_end(ap);
}
void vt_io_eprintf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

/* ----------------------------------------------------------------------------
   Tests rapides
   cc -std=c17 -DVT_IO_TEST io.c
---------------------------------------------------------------------------- */
#ifdef VT_IO_TEST
int main(void) {
  vt_io_printf("Hello %s\n", "world");
  vt_io_write_all("io_test.txt", "abc\n", 4);
  vt_io_append_all("io_test.txt", "xyz\n", 4);
  char* buf = NULL; size_t n=0;
  vt_io_read_all("io_test.txt", &buf, &n);
  fprintf(stderr, "file content (%zu bytes):\n%s", n, buf);
  free(buf);
  FILE* f = fopen("io_test.txt","r");
  char* line;
  while ((line = vt_io_read_line(f,&n))) { printf("line: %s\n", line); free(line); }
  fclose(f);
  return 0;
}
#endif
