// SPDX-License-Identifier: GPL-3.0-or-later
//
// lib.c — Portable utilities for Vitte Light runtime (C17)
// Namespace: "lib"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c lib.c
//
// Provides (portable, no external deps):
//   Version / platform:
//     const char* lib_version(void);
//     const char* lib_platform(void);
//     int         lib_is_little_endian(void);
//   Memory helpers (abort on OOM):
//     void* lib_xmalloc(size_t); void* lib_xcalloc(size_t, size_t);
//     void* lib_xrealloc(void*, size_t); char* lib_xstrdup(const char*);
//   Time and sleep:
//     uint64_t lib_time_ms(void); void lib_sleep_ms(uint32_t);
//   Paths and environment:
//     char     lib_path_sep(void); const char* lib_home_dir(char* buf, size_t n);
//     const char* lib_temp_dir(char* buf, size_t n); const char* lib_getenv(const char*);
//     int      lib_mkdir_p(const char* path); // 0 ok, -1 err (errno set)
//     int      lib_join_path(char* out, size_t n, const char* a, const char* b);
//     int      lib_dirname(char* out, size_t n, const char* path);
//     int      lib_basename(char* out, size_t n, const char* path);
//     int      lib_executable_path(char* out, size_t n);
//   File I/O:
//     int      lib_read_file(const char* path, void** data, size_t* size); // malloc buf
//     int      lib_write_file(const char* path, const void* data, size_t size);
//     int      lib_write_file_atomic(const char* path, const void* data, size_t size);
//
// Notes:
//   - All functions return 0 on success unless documented otherwise.
//   - lib_x* abort on allocation failure to keep call-sites simple.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>  // _mkdir
  #include <io.h>
  #define LIB_PATH_SEP '\\'
  #define mkdir_one(path,mode) _mkdir(path)
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #define LIB_PATH_SEP '/'
  #define mkdir_one(path,mode) mkdir(path, mode)
#endif

#ifndef LIB_VERSION_STR
#define LIB_VERSION_STR "0.1.0"
#endif

// ---------------- Version / platform ----------------

const char* lib_version(void) { return LIB_VERSION_STR; }

const char* lib_platform(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "apple";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "unknown";
#endif
}

int lib_is_little_endian(void) {
    uint16_t x = 1;
    return *(uint8_t*)&x == 1;
}

// ---------------- OOM-safe memory ----------------

static void lib_abort_oom(void) {
    fputs("lib: fatal: out of memory\n", stderr);
    abort();
}

void* lib_xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) lib_abort_oom();
    return p;
}
void* lib_xcalloc(size_t n, size_t m) {
    void* p = calloc(n ? n : 1, m ? m : 1);
    if (!p) lib_abort_oom();
    return p;
}
void* lib_xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) lib_abort_oom();
    return q;
}
char* lib_xstrdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*) lib_xmalloc(n);
    memcpy(p, s, n);
    return p;
}

// ---------------- Time and sleep ----------------

uint64_t lib_time_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000ULL) / (uint64_t)f.QuadPart);
#else
    struct timespec ts;
    // monotonic if possible
    #if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
    #else
    clock_gettime(CLOCK_REALTIME, &ts);
    #endif
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
#endif
}

void lib_sleep_ms(uint32_t ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

// ---------------- Env and dirs ----------------

char lib_path_sep(void) { return LIB_PATH_SEP; }

static int lib_is_sep(char c) { return c == '/' || c == '\\'; }

const char* lib_getenv(const char* k) {
    return k ? getenv(k) : NULL;
}

const char* lib_home_dir(char* buf, size_t n) {
    if (!buf || n == 0) { errno = EINVAL; return NULL; }
    buf[0] = '\0';
#if defined(_WIN32)
    const char* home = getenv("USERPROFILE");
    if (!home) {
        const char* h = getenv("HOMEDRIVE");
        const char* p = getenv("HOMEPATH");
        if (h && p) {
            snprintf(buf, n, "%s%s", h, p);
            return buf;
        }
    }
#else
    const char* home = getenv("HOME");
#endif
    if (!home) { errno = ENOENT; return NULL; }
    snprintf(buf, n, "%s", home);
    return buf;
}

const char* lib_temp_dir(char* buf, size_t n) {
    if (!buf || n == 0) { errno = EINVAL; return NULL; }
#if defined(_WIN32)
    const char* t = getenv("TEMP");
    if (!t) t = getenv("TMP");
    if (!t) t = "C:\\Windows\\Temp";
#else
    const char* t = getenv("TMPDIR");
    if (!t) t = "/tmp";
#endif
    snprintf(buf, n, "%s", t);
    return buf;
}

// mkdir -p
int lib_mkdir_p(const char* path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    char* tmp = lib_xstrdup(path);
    size_t len = strlen(tmp);
    if (len == 0) { free(tmp); return 0; }

    // skip drive on Windows like C:
#if defined(_WIN32)
    size_t i = (len >= 2 && tmp[1] == ':') ? 2 : 0;
#else
    size_t i = 0;
#endif

    for (; i < len; ++i) {
        if (lib_is_sep(tmp[i])) {
            if (i == 0) continue;           // root
            tmp[i] = '\0';
            if (mkdir_one(tmp, 0777) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            tmp[i] = LIB_PATH_SEP;
        }
    }
    if (mkdir_one(tmp, 0777) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

// ---------------- Path helpers ----------------

int lib_join_path(char* out, size_t n, const char* a, const char* b) {
    if (!out || n == 0 || !a || !b) { errno = EINVAL; return -1; }
    size_t la = strlen(a);
    int need_sep = (la > 0 && !lib_is_sep(a[la-1]));
    if (snprintf(out, n, "%s%s%s", a, need_sep ? (char[]){LIB_PATH_SEP,0} : "", b) >= (int)n) {
        errno = ENAMETOOLONG; return -1;
    }
    return 0;
}

int lib_dirname(char* out, size_t n, const char* path) {
    if (!out || n == 0 || !path) { errno = EINVAL; return -1; }
    size_t len = strlen(path);
    if (len == 0) { snprintf(out, n, "."); return 0; }
    size_t i = len;
    while (i > 0 && !lib_is_sep(path[i-1])) i--;
    if (i == 0) { snprintf(out, n, "."); return 0; }
    // remove trailing separators
    while (i > 1 && lib_is_sep(path[i-1])) i--;
    if (i >= n) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, path, i);
    out[i] = '\0';
    return 0;
}

int lib_basename(char* out, size_t n, const char* path) {
    if (!out || n == 0 || !path) { errno = EINVAL; return -1; }
    const char* p = path + strlen(path);
    while (p > path && !lib_is_sep(*(p-1))) p--;
    if (snprintf(out, n, "%s", p) >= (int)n) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

// ---------------- Executable path ----------------

int lib_executable_path(char* out, size_t n) {
    if (!out || n == 0) { errno = EINVAL; return -1; }
#if defined(_WIN32)
    DWORD w = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (w == 0 || w >= n) { errno = ENAMETOOLONG; return -1; }
    return 0;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)n;
    extern int _NSGetExecutablePath(char*, uint32_t*);
    if (_NSGetExecutablePath(out, &sz) != 0) { errno = ENAMETOOLONG; return -1; }
    return 0;
#elif defined(__linux__)
    ssize_t r = readlink("/proc/self/exe", out, n-1);
    if (r < 0) return -1;
    out[r] = '\0';
    return 0;
#else
    errno = ENOSYS; return -1;
#endif
}

// ---------------- File I/O ----------------

int lib_read_file(const char* path, void** data, size_t* size) {
    if (!path || !data || !size) { errno = EINVAL; return -1; }
    *data = NULL; *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long end = ftell(f);
    if (end < 0) { fclose(f); errno = EIO; return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    size_t n = (size_t) end;
    void* buf = lib_xmalloc(n + 1);
    size_t rd = fread(buf, 1, n, f);
    fclose(f);
    if (rd != n) { free(buf); errno = EIO; return -1; }
    ((char*)buf)[n] = '\0'; // NUL-terminate for text convenience
    *data = buf; *size = n;
    return 0;
}

int lib_write_file(const char* path, const void* data, size_t size) {
    if (!path || (!data && size)) { errno = EINVAL; return -1; }
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t wr = size ? fwrite(data, 1, size, f) : 0;
    int rc = (wr == size && fflush(f) == 0 && fclose(f) == 0) ? 0 : -1;
    if (rc != 0) { fclose(f); }
    return rc;
}

int lib_write_file_atomic(const char* path, const void* data, size_t size) {
    if (!path) { errno = EINVAL; return -1; }

    char dir[1024], base[512], tmp[1536];
    if (lib_dirname(dir, sizeof(dir), path) != 0) return -1;
    if (lib_basename(base, sizeof(base), path) != 0) return -1;

#if defined(_WIN32)
    // Use same directory
    if (snprintf(tmp, sizeof(tmp), "%s%c.%s.tmp.%lu", dir, LIB_PATH_SEP, base, (unsigned long)GetCurrentProcessId()) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG; return -1;
    }
#else
    if (snprintf(tmp, sizeof(tmp), "%s/%s.tmp.%ld", dir, base, (long)getpid()) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG; return -1;
    }
#endif

    if (lib_write_file(tmp, data, size) != 0) return -1;

#if defined(_WIN32)
    // Windows replace: MoveFileEx with MOVEFILE_REPLACE_EXISTING
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        (void)remove(tmp);
        return -1;
    }
#else
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        return -1;
    }
#endif
    return 0;
}

// ---------------- Demo ----------------
#ifdef LIB_DEMO
#include <stdio.h>
int main(void) {
    printf("lib %s on %s little=%d sep=%c\n",
           lib_version(), lib_platform(), lib_is_little_endian(), lib_path_sep());
    char home[512]; lib_home_dir(home, sizeof(home)); printf("home=%s\n", home);
    char exe[1024]; if (lib_executable_path(exe, sizeof(exe))==0) printf("exe=%s\n", exe);
    char j[1024]; lib_join_path(j, sizeof(j), home, "test.bin");
    const char msg[] = "hello";
    lib_mkdir_p(j); // harmless if file path; creates dirs along the way
    lib_write_file_atomic(j, msg, sizeof msg);
    void* data; size_t n; lib_read_file(j, &data, &n);
    printf("read %zu: %.*s\n", n, (int)n, (char*)data);
    free(data);
    return 0;
}
#endif