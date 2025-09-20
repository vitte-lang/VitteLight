// SPDX-License-Identifier: GPL-3.0-or-later
//
// loadlib.c — Dynamic library loader for Vitte Light VM (C17, portable)
// Namespace: "loadlib"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c loadlib.c
//
// Features:
//   - Cross-platform wrapper for dynamic libraries.
//   - loadlib_open(path) → handle
//   - loadlib_sym(handle, symbol) → function pointer
//   - loadlib_close(handle)
//   - loadlib_error() → last error string
//
// Notes:
//   - On POSIX: dlopen/dlsym/dlclose.
//   - On Windows: LoadLibraryA/GetProcAddress/FreeLibrary.
//   - Error strings are thread-local where available.
//   - Safe: returns NULL on failure.
//   - Suitable for plugin systems in Vitte Light.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
typedef HMODULE lib_handle;
#else
#  include <dlfcn.h>
typedef void* lib_handle;
#endif

// --- API ---
lib_handle loadlib_open(const char* path);
void*      loadlib_sym(lib_handle h, const char* sym);
int        loadlib_close(lib_handle h);
const char* loadlib_error(void);

// --- Implementation ---

lib_handle loadlib_open(const char* path) {
    if (!path) return NULL;
#if defined(_WIN32)
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* loadlib_sym(lib_handle h, const char* sym) {
    if (!h || !sym) return NULL;
#if defined(_WIN32)
    return (void*) GetProcAddress(h, sym);
#else
    return dlsym(h, sym);
#endif
}

int loadlib_close(lib_handle h) {
    if (!h) return -1;
#if defined(_WIN32)
    return FreeLibrary(h) ? 0 : -1;
#else
    return dlclose(h);
#endif
}

const char* loadlib_error(void) {
#if defined(_WIN32)
    static __thread char buf[256];
    DWORD err = GetLastError();
    if (err == 0) return NULL;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   buf, sizeof(buf), NULL);
    return buf;
#else
    const char* e = dlerror();
    return e ? e : NULL;
#endif
}

// --- Demo ---
#ifdef LOADLIB_DEMO
typedef int (*add_fn)(int,int);

int main(void) {
    lib_handle h = loadlib_open("./libmylib.so");
    if (!h) {
        fprintf(stderr, "open failed: %s\n", loadlib_error());
        return 1;
    }
    add_fn add = (add_fn) loadlib_sym(h, "add");
    if (!add) {
        fprintf(stderr, "symbol failed: %s\n", loadlib_error());
        loadlib_close(h);
        return 1;
    }
    printf("add(2,3)=%d\n", add(2,3));
    loadlib_close(h);
    return 0;
}
#endif