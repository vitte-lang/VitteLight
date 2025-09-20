// SPDX-License-Identifier: GPL-3.0-or-later
//
// dl.c — Cross-platform dynamic loader for Vitte Light (C17)
// Namespace: "dl"  —  version 1.1 “plus complet”
//
// Build:
//   POSIX:   cc -std=c17 -O2 -Wall -Wextra -pedantic -c dl.c -ldl
//   Windows: cl /std:c17 /O2 /W4 /c dl.c
//
// What’s new vs minimal:
//   - dl_open_any(): open by stem or filename, searches common paths.
//   - Custom search paths: dl_add_search_path, dl_clear_search_paths.
//   - Helpers: dl_ext, dl_prefix, dl_format_name, dl_join, dl_is_abs, dl_exists.
//   - Error buffer is thread-local; dl_error() returns NULL if none.
//   - dl_sym_optional(): returns NULL without setting errno.
//   - Windows: accepts UTF-8 narrow (ANSI); keep simple.
//
// API:
//   typedef struct dl_lib dl_lib;
//   const char* dl_ext(void);                 // ".so" | ".dll" | ".dylib"
//   const char* dl_prefix(void);              // "lib" on POSIX, "" on Windows
//   int  dl_format_name(char* out, size_t n, const char* stem);
//   int  dl_join(char* out, size_t n, const char* a, const char* b);
//   int  dl_is_abs(const char* path);         // 1/0
//   int  dl_exists(const char* path);         // 1/0
//
//   // Search path management (process-local in this TU):
//   int  dl_add_search_path(const char* dir); // appended; returns 0/-1
//   void dl_clear_search_paths(void);
//
//   // Openers:
//   dl_lib* dl_open(const char* path);        // exact path
//   dl_lib* dl_open_any(const char* name);    // stem or filename; searches
//   void    dl_close(dl_lib* lib);
//
//   // Symbols:
//   void*   dl_sym(dl_lib* lib, const char* name);          // sets errno on error
//   void*   dl_sym_optional(dl_lib* lib, const char* name); // NULL if not found
//
//   // Last error (thread-local string or NULL):
//   const char* dl_error(void);
//
// Notes:
//   - dl_open_any() resolution order:
//       1) If name has a path sep or is absolute → dl_open(name).
//       2) If name ends with platform ext → search paths + cwd for name.
//       3) Else format with prefix+ext (e.g. libNAME.so) and try:
//            - user search paths (added via dl_add_search_path)
//            - current working dir
//            - env paths (Windows PATH; POSIX LD_LIBRARY_PATH + standard dirs)
//            - standard system dirs (best-effort)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  typedef HMODULE dl_handle_t;
  #define DL_SEP '\\'
  #define PATH_SEP ';'
  #define access_ok(p) (_access(p, 0) == 0)
#else
  #include <dlfcn.h>
  #include <unistd.h>
  typedef void* dl_handle_t;
  #define DL_SEP '/'
  #define PATH_SEP ':'
  #define access_ok(p) (access(p, F_OK) == 0)
#endif

#ifndef DL_MAX_PATH
#define DL_MAX_PATH 4096
#endif

typedef struct dl_lib { dl_handle_t h; } dl_lib;

// -------- error buffer (thread-local) --------
#if defined(_WIN32)
static __declspec(thread) char g_err[256];
#else
static __thread char g_err[256];
#endif
static void set_err(const char* s){ if(!s){g_err[0]='\0';return;} snprintf(g_err,sizeof(g_err),"%s",s); }
const char* dl_error(void){ return g_err[0] ? g_err : NULL; }

// -------- tiny utils --------
static int ends_with(const char* s, const char* suf){
    size_t ls = s?strlen(s):0, lu = suf?strlen(suf):0;
    return (lu && ls>=lu && memcmp(s+ls-lu, suf, lu)==0) ? 1 : 0;
}
static int has_sep(const char* p){ for(;p && *p; ++p) if(*p=='/'||*p=='\\') return 1; return 0; }

const char* dl_ext(void){
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}
const char* dl_prefix(void){
#if defined(_WIN32)
    return "";
#else
    return "lib";
#endif
}
int dl_is_abs(const char* path){
    if(!path||!*path) return 0;
#if defined(_WIN32)
    if ((strlen(path)>=2 && path[1]==':') || (path[0]=='\\' && path[1]=='\\')) return 1;
    return (path[0]=='\\') ? 1 : 0;
#else
    return path[0]=='/' ? 1 : 0;
#endif
}
int dl_join(char* out, size_t n, const char* a, const char* b){
    if(!out||n==0||!a||!b){ errno=EINVAL; return -1; }
    size_t la=strlen(a);
    int need = (la>0 && a[la-1]!=DL_SEP);
    int rc = snprintf(out,n,"%s%s%s",a,need?(char[]){DL_SEP,0}:"",b);
    if(rc<0 || (size_t)rc>=n){ errno=ENAMETOOLONG; return -1; }
    return 0;
}
int dl_exists(const char* path){ return (path && *path && access_ok(path)) ? 1 : 0; }

int dl_format_name(char* out, size_t n, const char* stem){
    if(!out||n==0||!stem||!*stem){ errno=EINVAL; return -1; }
    int rc = snprintf(out,n,"%s%s%s", dl_prefix(), stem, dl_ext());
    if(rc<0 || (size_t)rc>=n){ errno=ENAMETOOLONG; return -1; }
    return 0;
}

// -------- search path store --------
typedef struct { char** v; size_t n, cap; } vec_t;
static vec_t g_paths;

static int vec_push(vec_t* s, const char* str){
    if(s->n==s->cap){
        size_t nc = s->cap? s->cap*2 : 8;
        char** nv = (char**)realloc(s->v, nc*sizeof(*nv));
        if(!nv) return -1;
        s->v = nv; s->cap = nc;
    }
    s->v[s->n] = (char*)malloc(strlen(str)+1);
    if(!s->v[s->n]) return -1;
    strcpy(s->v[s->n], str);
    s->n++;
    return 0;
}
int dl_add_search_path(const char* dir){
    if(!dir||!*dir){ errno=EINVAL; return -1; }
    return vec_push(&g_paths, dir);
}
void dl_clear_search_paths(void){
    for(size_t i=0;i<g_paths.n;i++) free(g_paths.v[i]);
    free(g_paths.v); g_paths.v=NULL; g_paths.n=0; g_paths.cap=0;
}

// -------- open/close/sym --------
dl_lib* dl_open(const char* path){
    set_err(NULL);
    if(!path||!*path){ set_err("dl: null path"); errno=EINVAL; return NULL; }
#if defined(_WIN32)
    HMODULE h = LoadLibraryA(path);
    if(!h){
        DWORD e = GetLastError();
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,e,MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT), g_err, (DWORD)sizeof(g_err), NULL);
        return NULL;
    }
#else
    void* h = dlopen(path, RTLD_NOW|RTLD_LOCAL);
    if(!h){ set_err(dlerror()); return NULL; }
#endif
    dl_lib* L = (dl_lib*)calloc(1,sizeof(*L));
    if(!L){
#if defined(_WIN32)
        FreeLibrary(h);
#else
        dlclose(h);
#endif
        set_err("dl: oom");
        return NULL;
    }
    L->h = h;
    return L;
}

void dl_close(dl_lib* lib){
    if(!lib) return;
#if defined(_WIN32)
    if(lib->h) FreeLibrary(lib->h);
#else
    if(lib->h) dlclose(lib->h);
#endif
    free(lib);
}

void* dl_sym(dl_lib* lib, const char* name){
    set_err(NULL);
    if(!lib||!lib->h||!name){ set_err("dl: bad args"); errno=EINVAL; return NULL; }
#if defined(_WIN32)
    FARPROC p = GetProcAddress(lib->h, name);
    if(!p){
        DWORD e = GetLastError();
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,e,MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT), g_err, (DWORD)sizeof(g_err), NULL);
        return NULL;
    }
    return (void*)p;
#else
    dlerror();
    void* p = dlsym(lib->h, name);
    const char* e = dlerror();
    if(e){ set_err(e); return NULL; }
    return p;
#endif
}

void* dl_sym_optional(dl_lib* lib, const char* name){
    // Same as dl_sym but does not touch errno or g_err on miss.
    if(!lib||!lib->h||!name) return NULL;
#if defined(_WIN32)
    return (void*)GetProcAddress(lib->h, name);
#else
    dlerror();
    void* p = dlsym(lib->h, name);
    (void)dlerror(); // discard
    return p;
#endif
}

// -------- environment paths helpers --------
static void try_env_paths(const char* envkey, int (*cb)(const char* dir, void* u), void* u){
    const char* v = getenv(envkey);
    if(!v||!*v) return;
    const char* s = v;
    while(*s){
        const char* e = strchr(s, PATH_SEP);
        size_t len = e? (size_t)(e-s) : strlen(s);
        if(len){
            char dir[DL_MAX_PATH];
            if(len >= sizeof(dir)) len = sizeof(dir)-1;
            memcpy(dir, s, len); dir[len] = '\0';
            if(cb(dir,u)!=0) return;
        }
        s = e? e+1 : s+len;
    }
}

struct search_need { const char* candidate; dl_lib* out; };

static int try_open_here(const char* dir, void* u){
    struct search_need* need = (struct search_need*)u;
    char path[DL_MAX_PATH];
    if (dl_join(path,sizeof(path),dir,need->candidate)!=0) return 0;
    if (dl_exists(path)){
        dl_lib* L = dl_open(path);
        if(L){ need->out = L; return 1; }
    }
    return 0;
}

// -------- dl_open_any: main resolver --------
dl_lib* dl_open_any(const char* name){
    set_err(NULL);
    if(!name||!*name){ set_err("dl: empty name"); errno=EINVAL; return NULL; }

    // 1) If absolute or has separator → open directly
    if (dl_is_abs(name) || has_sep(name)) {
        return dl_open(name);
    }

    // Prepare candidates:
    // if already has extension, try as is; else build prefixed name.
    char cand1[DL_MAX_PATH];
    if (ends_with(name, dl_ext())) {
        snprintf(cand1, sizeof(cand1), "%s", name);
    } else {
        if (dl_format_name(cand1, sizeof(cand1), name) != 0) return NULL;
    }

    // 2) Try user search paths (in order)
    struct search_need need = { cand1, NULL };
    for(size_t i=0;i<g_paths.n;i++){
        if(try_open_here(g_paths.v[i], &need)) return need.out;
    }

    // 3) Try current working directory
    if (dl_exists(cand1)) {
        dl_lib* L = dl_open(cand1);
        if (L) return L;
    }

    // 4) Try environment paths
#if defined(_WIN32)
    try_env_paths("PATH", try_open_here, &need);
#else
    try_env_paths("LD_LIBRARY_PATH", try_open_here, &need);
    if(need.out) return need.out;
    try_env_paths("DYLD_LIBRARY_PATH", try_open_here, &need); // macOS if allowed
#endif
    if(need.out) return need.out;

    // 5) Try standard system dirs (best-effort)
#if defined(_WIN32)
    char sysdir[DL_MAX_PATH];
    UINT n = GetSystemDirectoryA(sysdir, (UINT)sizeof(sysdir));
    if(n>0 && n < sizeof(sysdir)){
        if(try_open_here(sysdir, &need)) return need.out;
    }
#else
    const char* std_dirs[] = {
    #if defined(__APPLE__)
        "/usr/local/lib", "/opt/homebrew/lib", "/usr/lib",
    #else
        "/usr/local/lib", "/usr/lib64", "/usr/lib", "/lib64", "/lib",
    #endif
    };
    for (size_t i=0;i<sizeof(std_dirs)/sizeof(std_dirs[0]);++i){
        if(try_open_here(std_dirs[i], &need)) return need.out;
    }
#endif

    set_err("dl: not found");
    errno = ENOENT;
    return NULL;
}

// -------- Optional demo --------
#ifdef DL_DEMO
#include <stdio.h>
typedef int (*puts_fn)(const char*);
int main(void){
#if defined(_WIN32)
    const char* stem = "msvcrt";
#elif defined(__APPLE__)
    const char* stem = "System"; // /usr/lib/libSystem.dylib
#else
    const char* stem = "c";      // libc
#endif
    dl_add_search_path("."); // example
    dl_lib* L = dl_open_any(stem);
    if(!L){ fprintf(stderr,"open_any failed: %s\n", dl_error()); return 1; }
    puts_fn P = (puts_fn)dl_sym(L,"puts");
    if(!P){ fprintf(stderr,"sym failed: %s\n", dl_error()); dl_close(L); return 1; }
    P("dl_open_any demo ok");
    dl_close(L);
    return 0;
}
#endif