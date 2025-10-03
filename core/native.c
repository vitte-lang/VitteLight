/* ============================================================================
 * native.c — Portable native function registry (C11) for VitteLight / Vitl
 *  - Register C functions by name, call with small variant ABI
 *  - Optional: load a shared library and let it register natives through
 *      extern "C" void vitl_register_natives(native_registry* R);
 *  - Zero non-libc deps; no pthreads required
 *
 *  Public-ish API in this file (generate native.h on request):
 *    enum nat_type { NAT_I64, NAT_F64, NAT_STR, NAT_PTR };
 *    typedef struct { int type; union { int64_t i64; double f64; const char* str; void* ptr; }; } nat_val;
 *    typedef int (*native_fn)(void* udata, const nat_val* args, size_t nargs, nat_val* ret);
 *
 *    typedef struct native_registry native_registry;
 *    void  native_init(native_registry* R);
 *    void  native_free(native_registry* R);
 *    int   native_register(native_registry* R, const char* name, native_fn fn, void* udata); /* 0/-1 */
 *    int   native_unregister(native_registry* R, const char* name);                          /* 1/0 */
 *    native_fn native_find(const native_registry* R, const char* name, void** out_udata);    /* fn/NULL */
 *    int   native_call(const native_registry* R, const char* name,
 *                      const nat_val* args, size_t nargs, nat_val* ret);                    /* 0/-1 */
 *
 *    // Optional dynamic loading helpers (POSIX/Windows):
 *    int   native_load_library(native_registry* R, const char* path);  /* 0/-1. calls vitl_register_natives */
 *    void  native_unload_libraries(native_registry* R);                /* unload all loaded libs */
 *
 *  Build:
 *    cc -std=c11 -O2 -Wall -Wextra -pedantic -c native.c
 * ========================================================================== */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

/* ===== Platform dynamic loader (minimal, local) ========================== */
#if defined(_WIN32)
#  include <windows.h>
typedef HMODULE nat_lib_handle;
static nat_lib_handle nat_dlopen(const char* p) { return LoadLibraryA(p); }
static void*          nat_dlsym(nat_lib_handle h, const char* s) { return (void*)GetProcAddress(h, s); }
static void           nat_dlclose(nat_lib_handle h) { if (h) FreeLibrary(h); }
static const char*    nat_dlerr(void) {
    static char buf[160];
    DWORD code = GetLastError();
    if (!code) return "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, buf, (DWORD)sizeof(buf), NULL);
    buf[sizeof buf - 1] = 0;
    return buf;
}
#else
#  include <dlfcn.h>
typedef void* nat_lib_handle;
static nat_lib_handle nat_dlopen(const char* p) { return dlopen(p, RTLD_LOCAL|RTLD_NOW); }
static void*          nat_dlsym(nat_lib_handle h, const char* s) { return dlsym(h, s); }
static void           nat_dlclose(nat_lib_handle h) { if (h) dlclose(h); }
static const char*    nat_dlerr(void) { const char* e = dlerror(); return e ? e : ""; }
#endif

/* ===== Variant ABI ======================================================== */

enum { NAT_I64 = 1, NAT_F64 = 2, NAT_STR = 3, NAT_PTR = 4 };

typedef struct {
    int type;
    union {
        int64_t     i64;
        double      f64;
        const char* str;
        void*       ptr;
    };
} nat_val;

typedef int (*native_fn)(void* udata, const nat_val* args, size_t nargs, nat_val* ret);

/* ===== Registry types ===================================================== */

typedef struct {
    char*     name;
    native_fn fn;
    void*     udata;
} nat_entry;

typedef struct {
    nat_lib_handle handle;
    char*          path;
} nat_loaded_lib;

typedef struct native_registry {
    nat_entry*     items;
    size_t         count, cap;

    nat_loaded_lib* libs;
    size_t          lib_count, lib_cap;

    char           err[256]; /* last error text (debug) */
} native_registry;

/* ===== Small utils ======================================================== */

static char* nat_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

static void* grow(void* ptr, size_t elem_size, size_t cap, size_t need, size_t* out_cap) {
    size_t nc = cap ? cap * 2 : 8;
    if (nc < need) nc = need;
    void* np = realloc(ptr, elem_size * nc);
    if (!np) return NULL;
    *out_cap = nc;
    return np;
}

static void set_err(native_registry* R, const char* where, const char* what) {
    if (!R) return;
#if defined(_WIN32)
    _snprintf(R->err, sizeof R->err, "%s: %s", where ? where : "", what ? what : "");
#else
    snprintf(R->err, sizeof R->err, "%s: %s", where ? where : "", what ? what : "");
#endif
    R->err[sizeof R->err - 1] = 0;
}

/* ===== Core API =========================================================== */

void native_init(native_registry* R) {
    if (!R) return;
    memset(R, 0, sizeof *R);
}

void native_free(native_registry* R) {
    if (!R) return;
    for (size_t i = 0; i < R->count; ++i) free(R->items[i].name);
    free(R->items);
    for (size_t i = 0; i < R->lib_count; ++i) {
        nat_dlclose(R->libs[i].handle);
        free(R->libs[i].path);
    }
    free(R->libs);
    memset(R, 0, sizeof *R);
}

int native_register(native_registry* R, const char* name, native_fn fn, void* udata) {
    if (!R || !name || !fn) return -1;
    /* replace if exists */
    for (size_t i = 0; i < R->count; ++i) {
        if (strcmp(R->items[i].name, name) == 0) {
            R->items[i].fn = fn;
            R->items[i].udata = udata;
            return 0;
        }
    }
    if (R->count + 1 > R->cap) {
        void* np = grow(R->items, sizeof(nat_entry), R->cap, R->count + 1, &R->cap);
        if (!np) { set_err(R, "native_register", "oom"); return -1; }
        R->items = (nat_entry*)np;
    }
    nat_entry* e = &R->items[R->count++];
    e->name  = nat_strdup(name);
    e->fn    = fn;
    e->udata = udata;
    if (!e->name) { R->count--; set_err(R, "native_register", "oom"); return -1; }
    return 0;
}

int native_unregister(native_registry* R, const char* name) {
    if (!R || !name) return 0;
    for (size_t i = 0; i < R->count; ++i) {
        if (strcmp(R->items[i].name, name) == 0) {
            free(R->items[i].name);
            memmove(&R->items[i], &R->items[i+1], (R->count - i - 1) * sizeof(nat_entry));
            R->count--;
            return 1;
        }
    }
    return 0;
}

native_fn native_find(const native_registry* R, const char* name, void** out_udata) {
    if (!R || !name) return NULL;
    for (size_t i = 0; i < R->count; ++i) {
        if (strcmp(R->items[i].name, name) == 0) {
            if (out_udata) *out_udata = R->items[i].udata;
            return R->items[i].fn;
        }
    }
    return NULL;
}

int native_call(const native_registry* R, const char* name,
                const nat_val* args, size_t nargs, nat_val* ret) {
    if (!R || !name) return -1;
    void* udata = NULL;
    native_fn fn = native_find(R, name, &udata);
    if (!fn) return -1;
    return fn(udata, args, nargs, ret);
}

/* ===== Dynamic loading ==================================================== */
/* Library must export:  void vitl_register_natives(native_registry* R) */
typedef void (*vitl_register_natives_fn)(native_registry*);

int native_load_library(native_registry* R, const char* path) {
    if (!R || !path) return -1;
    nat_lib_handle h = nat_dlopen(path);
    if (!h) { set_err(R, "dlopen", nat_dlerr()); return -1; }

    vitl_register_natives_fn reg = (vitl_register_natives_fn)nat_dlsym(h, "vitl_register_natives");
    if (!reg) {
        set_err(R, "dlsym", "vitl_register_natives not found");
        nat_dlclose(h);
        return -1;
    }

    if (R->lib_count + 1 > R->lib_cap) {
        void* np = grow(R->libs, sizeof(nat_loaded_lib), R->lib_cap, R->lib_count + 1, &R->lib_cap);
        if (!np) { nat_dlclose(h); set_err(R, "native_load_library", "oom"); return -1; }
        R->libs = (nat_loaded_lib*)np;
    }

    nat_loaded_lib* L = &R->libs[R->lib_count++];
    L->handle = h;
    L->path   = nat_strdup(path);
    if (!L->path) { nat_dlclose(h); R->lib_count--; set_err(R, "native_load_library", "oom"); return -1; }

    reg(R); /* let the library populate the registry */
    return 0;
}

void native_unload_libraries(native_registry* R) {
    if (!R) return;
    for (size_t i = 0; i < R->lib_count; ++i) {
        nat_dlclose(R->libs[i].handle);
        free(R->libs[i].path);
    }
    free(R->libs);
    R->libs = NULL;
    R->lib_cap = R->lib_count = 0;
}

/* ===== Built-in helpers (optional) ======================================= */
/* Small sample natives that can be registered by host runtime if desired.   */

static int nat_now_ms(void* u, const nat_val* a, size_t n, nat_val* r) {
    (void)u; (void)a; (void)n;
#if defined(_WIN32)
    LARGE_INTEGER fq, ct;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&ct);
    double ms = (double)ct.QuadPart * 1000.0 / (double)fq.QuadPart;
    if (r) { r->type = NAT_F64; r->f64 = ms; }
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
    if (r) { r->type = NAT_F64; r->f64 = ms; }
#endif
    return 0;
}

static int nat_sleep_ms(void* u, const nat_val* a, size_t n, nat_val* r) {
    (void)r; (void)u;
    if (n < 1 || a[0].type != NAT_I64) return -1;
    int64_t ms = a[0].i64;
#if defined(_WIN32)
    Sleep(ms > 0 ? (DWORD)ms : 0);
#else
    struct timespec ts; ts.tv_sec = (time_t)(ms / 1000); ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#endif
    return 0;
}

static int nat_getenv(void* u, const nat_val* a, size_t n, nat_val* r) {
    (void)u;
    if (n < 1 || a[0].type != NAT_STR || !r) return -1;
    const char* v = getenv(a[0].str ? a[0].str : "");
    r->type = NAT_STR;
    r->str  = v ? v : "";
    return 0;
}

/* Optional: expose a helper to quickly pre-register those */
void native_register_basics(native_registry* R) {
    if (!R) return;
    native_register(R, "time.now_ms",   nat_now_ms,  NULL);
    native_register(R, "time.sleep_ms", nat_sleep_ms, NULL);
    native_register(R, "env.getenv",    nat_getenv,  NULL);
}

/* ===== Optional self-test ================================================= */
#ifdef NATIVE_MAIN
static void print_val(const nat_val* v) {
    if (!v) { puts("(null)"); return; }
    switch (v->type) {
        case NAT_I64: printf("I64(%lld)\n", (long long)v->i64); break;
        case NAT_F64: printf("F64(%f)\n", v->f64); break;
        case NAT_STR: printf("STR(\"%s\")\n", v->str ? v->str : ""); break;
        case NAT_PTR: printf("PTR(%p)\n", v->ptr); break;
        default: printf("?"); break;
    }
}

int main(int argc, char** argv) {
    native_registry R; native_init(&R);
    native_register_basics(&R);

    nat_val ret;
    if (native_call(&R, "time.now_ms", NULL, 0, &ret) == 0) print_val(&ret);

    nat_val arg = { .type = NAT_I64, .i64 = 50 };
    native_call(&R, "time.sleep_ms", &arg, 1, NULL);

    nat_val key = { .type = NAT_STR, .str = "PATH" };
    if (native_call(&R, "env.getenv", &key, 1, &ret) == 0) print_val(&ret);

    /* If a path is passed, try to load it and let it register something */
    if (argc > 1) {
        if (native_load_library(&R, argv[1]) == 0) {
            /* library is expected to have registered functions; try a demo name */
            native_call(&R, "demo.native", NULL, 0, NULL);
        } else {
            fprintf(stderr, "load error: %s\n", R.err);
        }
    }

    native_unload_libraries(&R);
    native_free(&R);
    return 0;
}
#endif
