/* ============================================================================
 * loader.c — Portable file and dynamic library loader for VitteLight (C11)
 * Single-file, zero non-libc deps. POSIX (Linux/macOS) and Windows.
 * ============================================================================
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== Platform split ===================================================== */
#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  define LOADER_PATH_SEP '\\'
#  define loader_stat _stati64
#  define loader_stat_t struct _stat64
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <fcntl.h>
#  include <unistd.h>
#  define LOADER_PATH_SEP '/'
#  define loader_stat stat
#  define loader_stat_t struct stat
#endif

/* ===== Public-ish API (header-less) ====================================== */
/* Fichiers */
typedef struct {
    void*  data;
    size_t size;
} loader_blob;

typedef struct {
    void*  ptr;
    size_t size;
    bool   is_mapped;
#if defined(_WIN32)
    HANDLE hFile;
    HANDLE hMap;
#else
    int    fd;
#endif
} loader_map;

/* Bibliothèques dynamiques */
typedef struct {
#if defined(_WIN32)
    HMODULE handle;
#else
    void*   handle;
#endif
    char    path[512];
} loader_dylib;

/* ---- Files: read all ---- */
int  loader_read_all(const char* path, loader_blob* out);     /* 0 ok, -1 err */
void loader_blob_free(loader_blob* b);

/* ---- Files: mmap (fallback to heap when unavailable) ---- */
int  loader_map_open(const char* path, loader_map* out);      /* 0 ok, -1 err */
void loader_map_close(loader_map* m);

/* ---- Dynlib load/symbol/close ---- */
int   loader_dylib_open(const char* path, loader_dylib* lib); /* 0 ok, -1 err */
void* loader_dylib_sym(const loader_dylib* lib, const char* name);
void  loader_dylib_close(loader_dylib* lib);

/* ---- Path utils ---- */
size_t loader_join_path(char* dst, size_t cap, const char* a, const char* b);
int    loader_file_exists(const char* path);                  /* 1 yes, 0 no, -1 err */

/* ---- Error helper ---- */
const char* loader_last_error(void); /* thread-local-ish, static buffer */

/* ===== Implementation ===================================================== */

static char g_errbuf[256];

static void set_err(const char* prefix, const char* detail) {
    if (!prefix && !detail) {
        g_errbuf[0] = '\0';
        return;
    }
    if (!detail) detail = "";
#if defined(_WIN32)
    DWORD code = GetLastError();
    char  sys[160] = {0};
    if (code) {
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, 0, sys, (DWORD)sizeof(sys), NULL);
        sys[sizeof(sys)-1] = 0;
    }
    _snprintf(g_errbuf, sizeof(g_errbuf), "%s: %s%s%s",
              prefix ? prefix : "", detail,
              code ? " — " : "", code ? sys : "");
#else
    int e = errno;
    const char* es = e ? strerror(e) : "";
    snprintf(g_errbuf, sizeof(g_errbuf), "%s: %s%s%s",
             prefix ? prefix : "", detail ? detail : "",
             e ? " — " : "", es);
#endif
    g_errbuf[sizeof(g_errbuf)-1] = 0;
}

const char* loader_last_error(void) {
    return g_errbuf[0] ? g_errbuf : "";
}

/* ---- Files: read all ---------------------------------------------------- */

int loader_read_all(const char* path, loader_blob* out) {
    if (!path || !out) { set_err("loader_read_all", "bad args"); return -1; }
    out->data = NULL;
    out->size = 0;

    FILE* f = fopen(path, "rb");
    if (!f) { set_err("fopen", path); return -1; }

    if (fseek(f, 0, SEEK_END) != 0) { set_err("fseek", path); fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { set_err("ftell", path); fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { set_err("fseek", path); fclose(f); return -1; }

    size_t n = (size_t)sz;
    void*  p = NULL;
    if (n == 0) {
        /* Empty file: return non-null pointer with size 0 is optional. Use NULL. */
        p = NULL;
    } else {
        p = malloc(n);
        if (!p) { set_err("malloc", "out of memory"); fclose(f); return -1; }
        size_t rd = fread(p, 1, n, f);
        if (rd != n) {
            set_err("fread", path);
            free(p);
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    out->data = p;
    out->size = n;
    set_err(NULL, NULL);
    return 0;
}

void loader_blob_free(loader_blob* b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

/* ---- Files: mmap -------------------------------------------------------- */

int loader_map_open(const char* path, loader_map* out) {
    if (!path || !out) { set_err("loader_map_open", "bad args"); return -1; }
    memset(out, 0, sizeof(*out));

    /* Stat first to get size and existence. */
    loader_stat_t st;
#if defined(_WIN32)
    if (loader_stat(path, &st) != 0) {
        set_err("stat", path);
        return -1;
    }
#else
    if (loader_stat(path, &st) != 0) {
        set_err("stat", path);
        return -1;
    }
#endif
    if (st.st_size < 0) {
        set_err("stat", "negative size");
        return -1;
    }
    size_t size = (size_t)st.st_size;
    out->size = size;

#if defined(_WIN32)
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        set_err("CreateFile", path);
        goto fallback_heap;
    }

    HANDLE hMap = NULL;
    if (size > 0) {
        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMap) {
            set_err("CreateFileMapping", path);
            CloseHandle(hFile);
            goto fallback_heap;
        }
        void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (!view) {
            set_err("MapViewOfFile", path);
            CloseHandle(hMap);
            CloseHandle(hFile);
            goto fallback_heap;
        }
        out->ptr = view;
        out->is_mapped = true;
        out->hFile = hFile;
        out->hMap  = hMap;
        set_err(NULL, NULL);
        return 0;
    } else {
        /* Empty file → treat like mapped with NULL. */
        out->ptr = NULL;
        out->is_mapped = true;
        out->hFile = hFile;
        out->hMap  = NULL;
        set_err(NULL, NULL);
        return 0;
    }

fallback_heap: ;
    /* Fallback: heap read */
    loader_blob b = {0};
    if (loader_read_all(path, &b) != 0) return -1;
    out->ptr = b.data;
    out->size = b.size;
    out->is_mapped = false;
    out->hFile = NULL;
    out->hMap  = NULL;
    set_err(NULL, NULL);
    return 0;

#else /* POSIX */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err("open", path);
        goto fallback_heap;
    }

    if (size == 0) {
        /* Empty file: keep fd for symmetry or close and return NULL. We close. */
        close(fd);
        out->ptr = NULL;
        out->size = 0;
        out->is_mapped = true; /* logically mapped */
        out->fd = -1;
        set_err(NULL, NULL);
        return 0;
    }

    void* p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        set_err("mmap", path);
        close(fd);
        goto fallback_heap;
    }
    out->ptr = p;
    out->is_mapped = true;
    out->fd = fd;
    set_err(NULL, NULL);
    return 0;

fallback_heap: ;
    loader_blob b = {0};
    if (loader_read_all(path, &b) != 0) return -1;
    out->ptr = b.data;
    out->size = b.size;
    out->is_mapped = false;
    out->fd = -1;
    set_err(NULL, NULL);
    return 0;
#endif
}

void loader_map_close(loader_map* m) {
    if (!m) return;
#if defined(_WIN32)
    if (m->is_mapped) {
        if (m->ptr) UnmapViewOfFile(m->ptr);
        if (m->hMap)  CloseHandle(m->hMap);
        if (m->hFile) CloseHandle(m->hFile);
    } else {
        free(m->ptr);
    }
#else
    if (m->is_mapped) {
        if (m->ptr && m->size) munmap(m->ptr, m->size);
        if (m->fd >= 0) close(m->fd);
    } else {
        free(m->ptr);
    }
#endif
    memset(m, 0, sizeof(*m));
}

/* ---- Dynlib ------------------------------------------------------------- */

int loader_dylib_open(const char* path, loader_dylib* lib) {
    if (!path || !lib) { set_err("loader_dylib_open", "bad args"); return -1; }
    memset(lib, 0, sizeof(*lib));
#if defined(_WIN32)
    HMODULE h = LoadLibraryA(path);
    if (!h) { set_err("LoadLibrary", path); return -1; }
    lib->handle = h;
#else
    /* RTLD_LOCAL prevents symbol leakage, RTLD_NOW for eager bind. */
    void* h = dlopen(path, RTLD_LOCAL | RTLD_NOW);
    if (!h) {
        const char* e = dlerror();
        set_err("dlopen", e ? e : path);
        return -1;
    }
    lib->handle = h;
#endif
    /* Store truncated path for debug. */
    strncpy(lib->path, path, sizeof(lib->path)-1);
    lib->path[sizeof(lib->path)-1] = 0;
    set_err(NULL, NULL);
    return 0;
}

void* loader_dylib_sym(const loader_dylib* lib, const char* name) {
    if (!lib || !lib->handle || !name) { set_err("loader_dylib_sym", "bad args"); return NULL; }
#if defined(_WIN32)
    FARPROC p = GetProcAddress(lib->handle, name);
    if (!p) { set_err("GetProcAddress", name); return NULL; }
    set_err(NULL, NULL);
    return (void*)p;
#else
    dlerror(); /* clear */
    void* p = dlsym(lib->handle, name);
    const char* e = dlerror();
    if (e) { set_err("dlsym", e); return NULL; }
    set_err(NULL, NULL);
    return p;
#endif
}

void loader_dylib_close(loader_dylib* lib) {
    if (!lib || !lib->handle) return;
#if defined(_WIN32)
    FreeLibrary(lib->handle);
#else
    dlclose(lib->handle);
#endif
    memset(lib, 0, sizeof(*lib));
}

/* ---- Paths -------------------------------------------------------------- */

static size_t strlcpy_compat(char* dst, const char* src, size_t cap) {
    size_t n = src ? strlen(src) : 0;
    if (cap) {
        size_t c = (n >= cap) ? cap - 1 : n;
        if (dst && cap) {
            if (c) memcpy(dst, src, c);
            dst[c] = 0;
        }
    }
    return n;
}

size_t loader_join_path(char* dst, size_t cap, const char* a, const char* b) {
    if (!dst || !cap) return 0;
    dst[0] = 0;
    if (!a && !b) return 0;

    size_t len = 0;
    if (a && *a) {
        len = strlcpy_compat(dst, a, cap);
        if (len >= cap) return len;
        if (dst[len ? len - 1 : 0] != LOADER_PATH_SEP) {
            if (len + 1 < cap) {
                dst[len++] = LOADER_PATH_SEP;
                dst[len] = 0;
            } else {
                return len + 1; /* indicate truncation */
            }
        }
    }
    if (b && *b) {
        size_t need = strlen(b);
        if (len + need + 1 > cap) {
            /* partial copy to fill */
            size_t c = (cap > len + 1) ? (cap - len - 1) : 0;
            if (c) memcpy(dst + len, b, c);
            dst[cap - 1] = 0;
            return len + need; /* report intended length */
        }
        memcpy(dst + len, b, need + 1);
        len += need;
    }
    return len;
}

int loader_file_exists(const char* path) {
    if (!path) return -1;
    loader_stat_t st;
    if (loader_stat(path, &st) == 0) return 1;
    return (errno == ENOENT)
#if defined(_WIN32)
           || (GetLastError() == ERROR_FILE_NOT_FOUND)
#endif
           ? 0 : -1;
}

/* ===== Optional self-test ================================================= */
#ifdef LOADER_MAIN
static void hexdump(const void* p, size_t n) {
    const unsigned char* s = (const unsigned char*)p;
    for (size_t i = 0; i < n; ++i) {
        printf("%02X%s", s[i], ((i+1)%16==0 || i+1==n) ? "\n" : " ");
    }
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : NULL;
    if (!path) { printf("usage: loader <file>\n"); return 0; }

    printf("[exists] %s → %d\n", path, loader_file_exists(path));

    loader_blob b = {0};
    if (loader_read_all(path, &b) == 0) {
        printf("[read_all] size=%zu\n", b.size);
        hexdump(b.data, b.size < 32 ? b.size : 32);
    } else {
        fprintf(stderr, "read_all error: %s\n", loader_last_error());
    }

    loader_blob_free(&b);

    loader_map m = {0};
    if (loader_map_open(path, &m) == 0) {
        printf("[map_open] size=%zu mapped=%d\n", m.size, (int)m.is_mapped);
        if (m.ptr && m.size) hexdump(m.ptr, m.size < 32 ? m.size : 32);
        loader_map_close(&m);
    } else {
        fprintf(stderr, "map_open error: %s\n", loader_last_error());
    }

    /* Dynlib smoke test only if a .so/.dylib/.dll is provided */
    if (argc > 2) {
        const char* libp = argv[2];
        loader_dylib lib = {0};
        if (loader_dylib_open(libp, &lib) == 0) {
            printf("[dylib] opened %s\n", lib.path);
            /* Example: resolve "malloc" or a known symbol */
            void* sym = loader_dylib_sym(&lib, "malloc");
            printf("[dylib] sym malloc = %p%s\n", sym, sym ? "" : " (not found)");
            loader_dylib_close(&lib);
        } else {
            fprintf(stderr, "dylib error: %s\n", loader_last_error());
        }
    }

    return 0;
}
#endif
