/* ============================================================================
 * mmap.c — Portable memory mapping (C11) for VitteLight / Vitl
 *  API:
 *    - mm_map_file(path, &r, prot, shared)     // map file
 *    - mm_map_anon(&r, size, prot)             // anonymous map
 *    - mm_sync(&r, off, len, async)            // flush changes
 *    - mm_protect(&r, prot)                    // change protections
 *    - mm_unmap(&r)                            // release
 *    - mm_last_error()                         // last error string
 *
 *  prot   : bitmask { MM_PROT_READ=1, MM_PROT_WRITE=2, MM_PROT_EXEC=4 }
 *  shared : 0 → private (copy-on-write), 1 → shared (write-through)
 *  return : 0 success, -1 error (see mm_last_error)
 * ========================================================================== */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  define MM_IS_WIN 1
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <fcntl.h>
#  include <unistd.h>
#  define MM_IS_WIN 0
#endif

/* ===== Public-ish types =================================================== */

enum { MM_PROT_READ = 1, MM_PROT_WRITE = 2, MM_PROT_EXEC = 4 };

typedef struct {
    void*  ptr;
    size_t size;
    int    writable;
#if MM_IS_WIN
    HANDLE hFile;
    HANDLE hMap;
#else
    int    fd;
#endif
} mm_region;

/* ===== Error buffer ======================================================= */

static char g_mm_err[256];

static void mm_set_err(const char* where, const char* what) {
#if MM_IS_WIN
    DWORD code = GetLastError();
    char  sys[160] = {0};
    if (code) {
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, 0, sys, (DWORD)sizeof(sys), NULL);
        sys[sizeof(sys)-1] = 0;
    }
    _snprintf(g_mm_err, sizeof g_mm_err, "%s: %s%s%s",
              where ? where : "", what ? what : "",
              code ? " — " : "", code ? sys : "");
#else
    int e = errno;
    const char* es = e ? strerror(e) : "";
    snprintf(g_mm_err, sizeof g_mm_err, "%s: %s%s%s",
             where ? where : "", what ? what : "",
             e ? " — " : "", es);
#endif
    g_mm_err[sizeof g_mm_err - 1] = 0;
}

const char* mm_last_error(void) { return g_mm_err[0] ? g_mm_err : ""; }

/* ===== Helpers ============================================================ */

static int mm_is_power_of_two(size_t x) { return x && ((x & (x - 1)) == 0); }

/* Map prot bits to platform flags */
#if MM_IS_WIN
static DWORD win_page_prot(int prot) {
    const int rw  = (prot & MM_PROT_WRITE) != 0;
    const int ex  = (prot & MM_PROT_EXEC)  != 0;
    if (rw && ex) return PAGE_EXECUTE_READWRITE;
    if (rw)       return PAGE_READWRITE;
    if (ex)       return PAGE_EXECUTE_READ;
    return PAGE_READONLY;
}
static DWORD win_view_access(int prot, int shared) {
    (void)shared; /* View access does not encode sharing choice */
    DWORD acc = 0;
    if (prot & MM_PROT_WRITE) acc |= FILE_MAP_WRITE;
    if (prot & MM_PROT_READ)  acc |= FILE_MAP_READ;
    if (prot & MM_PROT_EXEC)  acc |= FILE_MAP_EXECUTE;
    if (acc == 0) acc = FILE_MAP_READ;
    return acc;
}
#else
static int posix_prot(int prot) {
    int p = 0;
    if (prot & MM_PROT_READ)  p |= PROT_READ;
    if (prot & MM_PROT_WRITE) p |= PROT_WRITE;
    if (prot & MM_PROT_EXEC)  p |= PROT_EXEC;
    return p ? p : PROT_READ;
}
static int posix_flags(int shared) {
    return shared ? MAP_SHARED : MAP_PRIVATE;
}
#endif

/* ===== API: map file ====================================================== */

int mm_map_file(const char* path, mm_region* out, int prot, int shared) {
    if (!path || !out) { mm_set_err("mm_map_file", "bad args"); return -1; }
    memset(out, 0, sizeof *out);
    out->writable = (prot & MM_PROT_WRITE) != 0;

#if MM_IS_WIN
    DWORD access = (prot & MM_PROT_WRITE) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    HANDLE hf = CreateFileA(path, access, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { mm_set_err("CreateFile", path); return -1; }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(hf, &sz)) {
        mm_set_err("GetFileSizeEx", path);
        CloseHandle(hf);
        return -1;
    }

    HANDLE hm = NULL;
    if (sz.QuadPart > 0) {
        hm = CreateFileMappingA(hf, NULL, win_page_prot(prot), 0, 0, NULL);
        if (!hm) {
            mm_set_err("CreateFileMapping", path);
            CloseHandle(hf);
            return -1;
        }
        void* view = MapViewOfFile(hm, win_view_access(prot, shared), 0, 0, 0);
        if (!view) {
            mm_set_err("MapViewOfFile", path);
            CloseHandle(hm);
            CloseHandle(hf);
            return -1;
        }
        out->ptr = view;
    } else {
        out->ptr = NULL; /* empty file */
    }

    out->size = (size_t)sz.QuadPart;
    out->hFile = hf;
    out->hMap  = hm;
    g_mm_err[0] = 0;
    return 0;

#else
    int oflag = O_RDONLY;
    if (prot & MM_PROT_WRITE) oflag = O_RDWR;
    int fd = open(path, oflag);
    if (fd < 0) { mm_set_err("open", path); return -1; }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        mm_set_err("fstat", path);
        close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size;

    void* p = NULL;
    if (size > 0) {
        p = mmap(NULL, size, posix_prot(prot), posix_flags(shared), fd, 0);
        if (p == MAP_FAILED) {
            mm_set_err("mmap", path);
            close(fd);
            return -1;
        }
    }

    out->ptr  = p;
    out->size = size;
    out->fd   = fd;
    g_mm_err[0] = 0;
    return 0;
#endif
}

/* ===== API: map anonymous ================================================= */

int mm_map_anon(mm_region* out, size_t size, int prot) {
    if (!out) { mm_set_err("mm_map_anon", "bad args"); return -1; }
    memset(out, 0, sizeof *out);
    out->writable = (prot & MM_PROT_WRITE) != 0;

#if MM_IS_WIN
    if (size == 0) { g_mm_err[0] = 0; return 0; }
    /* Anonymous: use pagefile-backed section */
    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, win_page_prot(prot),
                                   (DWORD)((uint64_t)size >> 32),
                                   (DWORD)(size & 0xFFFFFFFFu), NULL);
    if (!hm) { mm_set_err("CreateFileMapping", "anon"); return -1; }

    void* view = MapViewOfFile(hm, win_view_access(prot, 0), 0, 0, size);
    if (!view) {
        mm_set_err("MapViewOfFile", "anon");
        CloseHandle(hm);
        return -1;
    }

    out->ptr  = view;
    out->size = size;
    out->hFile = NULL;
    out->hMap  = hm;
    g_mm_err[0] = 0;
    return 0;

#else
    if (size == 0) { g_mm_err[0] = 0; return 0; }
#  if defined(MAP_ANONYMOUS)
    void* p = mmap(NULL, size, posix_prot(prot), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#  else
    /* Fallback: map /dev/zero */
    int fd = open("/dev/zero", O_RDWR);
    if (fd < 0) { mm_set_err("open", "/dev/zero"); return -1; }
    void* p = mmap(NULL, size, posix_prot(prot), MAP_PRIVATE, fd, 0);
    close(fd);
#  endif
    if (p == MAP_FAILED) { mm_set_err("mmap", "anon"); return -1; }

    out->ptr  = p;
    out->size = size;
    out->fd   = -1;
    g_mm_err[0] = 0;
    return 0;
#endif
}

/* ===== API: sync ========================================================== */

int mm_sync(mm_region* r, size_t off, size_t len, bool async) {
    if (!r) { mm_set_err("mm_sync", "bad args"); return -1; }
    if (!r->ptr || r->size == 0) return 0;
    if (off > r->size) return 0;
    if (len == 0 || off + len > r->size) len = r->size - off;

#if MM_IS_WIN
    /* Windows: FlushViewOfFile flushes the range, FlushFileBuffers ensures disk write for shared files */
    BYTE* base = (BYTE*)r->ptr + off;
    if (!FlushViewOfFile(base, len)) { mm_set_err("FlushViewOfFile", NULL); return -1; }
    if (r->hFile && !FlushFileBuffers(r->hFile) && !async) { mm_set_err("FlushFileBuffers", NULL); return -1; }
    g_mm_err[0] = 0; return 0;
#else
    int flags = async ? MS_ASYNC : MS_SYNC;
    if (msync((char*)r->ptr + off, len, flags) != 0) { mm_set_err("msync", NULL); return -1; }
    g_mm_err[0] = 0; return 0;
#endif
}

/* ===== API: protect ======================================================= */

int mm_protect(mm_region* r, int prot) {
    if (!r) { mm_set_err("mm_protect", "bad args"); return -1; }
    if (!r->ptr || r->size == 0) return 0;

#if MM_IS_WIN
    DWORD newp = 0;
    /* VirtualProtect uses page-level flags different from mapping creation */
    if (prot & MM_PROT_WRITE) {
        newp = (prot & MM_PROT_EXEC) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    } else if (prot & MM_PROT_READ) {
        newp = (prot & MM_PROT_EXEC) ? PAGE_EXECUTE_READ : PAGE_READONLY;
    } else if (prot & MM_PROT_EXEC) {
        newp = PAGE_EXECUTE;
    } else {
        newp = PAGE_NOACCESS;
    }
    DWORD oldp;
    if (!VirtualProtect(r->ptr, r->size, newp, &oldp)) { mm_set_err("VirtualProtect", NULL); return -1; }
    g_mm_err[0] = 0; return 0;
#else
    if (mprotect(r->ptr, r->size, posix_prot(prot)) != 0) { mm_set_err("mprotect", NULL); return -1; }
    g_mm_err[0] = 0; return 0;
#endif
}

/* ===== API: unmap ========================================================= */

void mm_unmap(mm_region* r) {
    if (!r) return;
#if MM_IS_WIN
    if (r->ptr) UnmapViewOfFile(r->ptr);
    if (r->hMap)  CloseHandle(r->hMap);
    if (r->hFile) CloseHandle(r->hFile);
#else
    if (r->ptr && r->size) munmap(r->ptr, r->size);
    if (r->fd > 0) close(r->fd);
#endif
    memset(r, 0, sizeof *r);
}

/* ===== Optional self-test ================================================ */
#ifdef MMAP_MAIN
#include <assert.h>
int main(int argc, char** argv) {
    mm_region r = {0};
    if (argc > 1) {
        if (mm_map_file(argv[1], &r, MM_PROT_READ, 0) != 0) {
            fprintf(stderr, "map_file: %s\n", mm_last_error());
            return 1;
        }
        printf("file mapped: %zu bytes\n", r.size);
        mm_unmap(&r);
    }
    if (mm_map_anon(&r, 4096, MM_PROT_READ|MM_PROT_WRITE) != 0) {
        fprintf(stderr, "map_anon: %s\n", mm_last_error());
        return 1;
    }
    ((unsigned char*)r.ptr)[0] = 0xAB;
    mm_sync(&r, 0, 4096, false);
    mm_protect(&r, MM_PROT_READ);
#if !MM_IS_WIN
    /* mprotect to read-only then write should SIGSEGV; we skip crash */
#endif
    mm_unmap(&r);
    puts("ok");
    return 0;
}
#endif
