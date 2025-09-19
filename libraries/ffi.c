// SPDX-License-Identifier: GPL-3.0-or-later
//
// ffi.c — Minimal FFI helpers for Vitte Light (C17, portable)
// Namespace: "ffi"
//
// Build:
//   POSIX:   cc -std=c17 -O2 -Wall -Wextra -pedantic -c ffi.c -ldl
//   Windows: cl /std:c17 /O2 /W4 /c ffi.c
//
// Scope:
//   - Dynamic open/sym/close with last-error string.
//   - Trivial cdecl call shims for common signatures.
//   - No libffi. Portable but limited. You must pick a shim that matches
//     the real C signature. UB if you mismatch.
//   - Pointers are passed as void*. Integers as int64_t. FP as double.
//
// API:
//   typedef struct ffi_lib ffi_lib;
//
//   // Library handling
//   ffi_lib*     ffi_open(const char* path);  // NULL on error
//   void         ffi_close(ffi_lib* lib);     // safe on NULL
//   void*        ffi_sym(ffi_lib* lib, const char* name); // NULL on error
//   const char*  ffi_error(void);             // thread-local buffer or static
//
//   // Integer return (int64_t), up to 4 integer args
//   int64_t ffi_call_i64_0(void* fn);
//   int64_t ffi_call_i64_1(void* fn, int64_t a0);
//   int64_t ffi_call_i64_2(void* fn, int64_t a0, int64_t a1);
//   int64_t ffi_call_i64_3(void* fn, int64_t a0, int64_t a1, int64_t a2);
//   int64_t ffi_call_i64_4(void* fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
//
//   // Pointer args variants (common cases)
//   int64_t ffi_call_i64_p1(void* fn, void* p0);
//   int64_t ffi_call_i64_p2(void* fn, void* p0, void* p1);
//   int64_t ffi_call_i64_p3(void* fn, void* p0, void* p1, void* p2);
//
//   // Double return, up to 4 double args
//   double  ffi_call_f64_0(void* fn);
//   double  ffi_call_f64_1(void* fn, double a0);
//   double  ffi_call_f64_2(void* fn, double a0, double a1);
//   double  ffi_call_f64_3(void* fn, double a0, double a1, double a2);
//   double  ffi_call_f64_4(void* fn, double a0, double a1, double a2, double a3);
//
//   // Void return, pointer args (setup/free patterns)
//   void    ffi_call_void_p1(void* fn, void* p0);
//   void    ffi_call_void_p2(void* fn, void* p0, void* p1);
//   void    ffi_call_void_p3(void* fn, void* p0, void* p1, void* p2);
//
// Notes:
//   - cdecl assumed. On Windows this matches default C. For stdcall etc. add shims.
//   - Variadic C functions are not supported.
//   - On ARM/RISCV/x86_64 LP64/LLP64, int64_t/double mapping works as expected.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define FFI_CDECL __cdecl
  typedef HMODULE ffi_handle_t;
#else
  #include <dlfcn.h>
  #define FFI_CDECL
  typedef void*   ffi_handle_t;
#endif

// ---------- Error buffer ----------

#if defined(_WIN32)
static __declspec(thread) char g_err[256];
#else
static __thread char g_err[256];
#endif

static void set_err(const char* s) {
    if (!s) { g_err[0] = '\0'; return; }
    snprintf(g_err, sizeof(g_err), "%s", s);
}

const char* ffi_error(void) {
    return g_err[0] ? g_err : NULL;
}

// ---------- Library wrapper ----------

typedef struct ffi_lib {
    ffi_handle_t h;
} ffi_lib;

ffi_lib* ffi_open(const char* path) {
    set_err(NULL);
    if (!path) { set_err("null path"); errno = EINVAL; return NULL; }
#if defined(_WIN32)
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        DWORD e = GetLastError();
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e,
                       MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT), g_err, (DWORD)sizeof(g_err), NULL);
        return NULL;
    }
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        set_err(e ? e : "dlopen failed");
        return NULL;
    }
#endif
    ffi_lib* lib = (ffi_lib*)calloc(1, sizeof(*lib));
    if (!lib) {
#if defined(_WIN32)
        FreeLibrary(h);
#else
        dlclose(h);
#endif
        set_err("oom");
        return NULL;
    }
    lib->h = h;
    return lib;
}

void ffi_close(ffi_lib* lib) {
    if (!lib) return;
#if defined(_WIN32)
    if (lib->h) FreeLibrary(lib->h);
#else
    if (lib->h) dlclose(lib->h);
#endif
    free(lib);
}

void* ffi_sym(ffi_lib* lib, const char* name) {
    set_err(NULL);
    if (!lib || !lib->h || !name) { set_err("bad args"); errno = EINVAL; return NULL; }
#if defined(_WIN32)
    FARPROC p = GetProcAddress(lib->h, name);
    if (!p) {
        DWORD e = GetLastError();
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e,
                       MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT), g_err, (DWORD)sizeof(g_err), NULL);
        return NULL;
    }
    return (void*)p;
#else
    dlerror(); // clear
    void* p = dlsym(lib->h, name);
    const char* e = dlerror();
    if (e) { set_err(e); return NULL; }
    return p;
#endif
}

// ---------- Call shims (cdecl) ----------
// CAUTION: the function pointer type must match the real symbol signature.
// Using the wrong shim is undefined behavior.

typedef int64_t (FFI_CDECL *fn_i64_0)(void);
typedef int64_t (FFI_CDECL *fn_i64_1)(int64_t);
typedef int64_t (FFI_CDECL *fn_i64_2)(int64_t,int64_t);
typedef int64_t (FFI_CDECL *fn_i64_3)(int64_t,int64_t,int64_t);
typedef int64_t (FFI_CDECL *fn_i64_4)(int64_t,int64_t,int64_t,int64_t);

typedef int64_t (FFI_CDECL *fn_i64_p1)(void*);
typedef int64_t (FFI_CDECL *fn_i64_p2)(void*,void*);
typedef int64_t (FFI_CDECL *fn_i64_p3)(void*,void*,void*);

typedef double  (FFI_CDECL *fn_f64_0)(void);
typedef double  (FFI_CDECL *fn_f64_1)(double);
typedef double  (FFI_CDECL *fn_f64_2)(double,double);
typedef double  (FFI_CDECL *fn_f64_3)(double,double,double);
typedef double  (FFI_CDECL *fn_f64_4)(double,double,double,double);

typedef void    (FFI_CDECL *fn_void_p1)(void*);
typedef void    (FFI_CDECL *fn_void_p2)(void*,void*);
typedef void    (FFI_CDECL *fn_void_p3)(void*,void*,void*);

// Integer returns
int64_t ffi_call_i64_0(void* fn)                                           { return ((fn_i64_0)fn)(); }
int64_t ffi_call_i64_1(void* fn, int64_t a0)                               { return ((fn_i64_1)fn)(a0); }
int64_t ffi_call_i64_2(void* fn, int64_t a0, int64_t a1)                   { return ((fn_i64_2)fn)(a0,a1); }
int64_t ffi_call_i64_3(void* fn, int64_t a0, int64_t a1, int64_t a2)       { return ((fn_i64_3)fn)(a0,a1,a2); }
int64_t ffi_call_i64_4(void* fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3) { return ((fn_i64_4)fn)(a0,a1,a2,a3); }

// Pointer arg variants
int64_t ffi_call_i64_p1(void* fn, void* p0)                                { return ((fn_i64_p1)fn)(p0); }
int64_t ffi_call_i64_p2(void* fn, void* p0, void* p1)                      { return ((fn_i64_p2)fn)(p0,p1); }
int64_t ffi_call_i64_p3(void* fn, void* p0, void* p1, void* p2)            { return ((fn_i64_p3)fn)(p0,p1,p2); }

// Double returns
double  ffi_call_f64_0(void* fn)                                           { return ((fn_f64_0)fn)(); }
double  ffi_call_f64_1(void* fn, double a0)                                 { return ((fn_f64_1)fn)(a0); }
double  ffi_call_f64_2(void* fn, double a0, double a1)                      { return ((fn_f64_2)fn)(a0,a1); }
double  ffi_call_f64_3(void* fn, double a0, double a1, double a2)           { return ((fn_f64_3)fn)(a0,a1,a2); }
double  ffi_call_f64_4(void* fn, double a0, double a1, double a2, double a3){ return ((fn_f64_4)fn)(a0,a1,a2,a3); }

// Void returns, pointer args
void    ffi_call_void_p1(void* fn, void* p0)                                { ((fn_void_p1)fn)(p0); }
void    ffi_call_void_p2(void* fn, void* p0, void* p1)                      { ((fn_void_p2)fn)(p0,p1); }
void    ffi_call_void_p3(void* fn, void* p0, void* p1, void* p2)            { ((fn_void_p3)fn)(p0,p1,p2); }

// ---------- Tiny self-test (optional) ----------
#ifdef FFI_DEMO
#include <stdio.h>
static int64_t FFI_CDECL add2(int64_t a, int64_t b){ return a+b; }
static double  FFI_CDECL mul3(double a,double b,double c){ return a*b*c; }
static void    FFI_CDECL touch(void* p){ if(p) *(int*)p += 7; }

int main(void){
    printf("i64: %lld\n",(long long)ffi_call_i64_2((void*)add2, 10, 32));
    printf("f64: %.3f\n",ffi_call_f64_3((void*)mul3, 1.5, 2.0, 3.0));
    int x=1; ffi_call_void_p1((void*)touch, &x); printf("x=%d\n",x);
    return 0;
}
#endif