/* ============================================================================
   dl.c — Chargeur dynamique ultra complet (C11, portable)
   POSIX: dlopen/dlsym/dlclose (+dladdr, dlvsym si dispo). macOS iOS-like ok.
   Windows: LoadLibraryExA/GetProcAddress/FreeLibrary/QueryFullProcessImageName.
   ----------------------------------------------------------------------------
   API publique (sans header séparé) :
     - Erreurs thread-local :
         const char* vt_dl_error(void);

     - Flags unifiés :
         VT_DL_LAZY, VT_DL_NOW, VT_DL_LOCAL, VT_DL_GLOBAL,
         VT_DL_NOLOAD, VT_DL_NODELETE

     - Ouverture/fermeture :
         void* vt_dl_open(const char* path);                         // tel quel
         void* vt_dl_open2(const char* path, int flags);             // flags
         void* vt_dl_open_with_ext(const char* base, int flags);     // ajoute ext
         void* vt_dl_open_name(const char* soname, int flags);       // "m" -> libm.so/.dll
         int    vt_dl_close(void* handle);                           // 0/-1
         void* vt_dl_self(void);                                     // handle self
#if !defined(_WIN32)
         void* vt_dl_next(void);                                     // RTLD_NEXT si dispo
#endif

     - Symboles :
         void* vt_dl_sym(void* handle, const char* name);
#if !defined(_WIN32)
         void* vt_dl_symv(void* handle, const char* name, const char* version); // dlvsym
#endif

     - Infos modules :
         int vt_dl_get_module_path(void* handle, char* out, size_t cap); // chemin absolu
         int vt_dl_get_module_dir(void* handle, char* out, size_t cap);  // dir du module

     - Résolution nommage :
         const char* vt_dl_default_ext(void);                // ".so", ".dylib", ".dll"
         const char* vt_dl_default_prefix(void);             // "lib" sur POSIX
         int  vt_dl_add_ext_if_missing(const char* in, char* out, size_t cap);
         int  vt_dl_add_prefix_if_missing(const char* in, char* out, size_t cap);
         int  vt_dl_build_name(const char* base, char* out, size_t cap); // prefix+ext

     - Recherche simple (liste de répertoires) :
         void vt_dl_clear_search_dirs(void);
         int  vt_dl_push_search_dir(const char* dir);        // empile un dir (<=16)
         int  vt_dl_pop_search_dir(void);
         void* vt_dl_search_open(const char* base_or_path, int flags); // parcourt dirs

   Notes:
     - Tous les buffers sont sûrs et bornés.
     - vt_dl_error() retourne un buffer thread-local ; réutiliser avant prochain appel.
     - Pas de dépendance hors libc et dl/Win32.
   Licence: MIT
   ============================================================================
*/
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN 1
  #include <windows.h>
  #include <psapi.h>
  #include <io.h>
  #include <fcntl.h>
#else
  #include <dlfcn.h>
  #include <unistd.h>
  #include <limits.h>
  #include <errno.h>
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>
  #endif
#endif

/* ----------------------------------------------------------------------------
   Qualifs thread-local
---------------------------------------------------------------------------- */
#if defined(_WIN32)
  #define VT_THREAD_LOCAL __declspec(thread)
#else
  #if defined(__STDC_NO_THREADS__)
    #define VT_THREAD_LOCAL __thread
  #else
    #define VT_THREAD_LOCAL _Thread_local
  #endif
#endif

/* ----------------------------------------------------------------------------
   Flags portables (bitmask)
---------------------------------------------------------------------------- */
enum {
  VT_DL_LAZY     = 1 << 0, /* défaut POSIX */
  VT_DL_NOW      = 1 << 1,
  VT_DL_LOCAL    = 1 << 2, /* défaut */
  VT_DL_GLOBAL   = 1 << 3,
  VT_DL_NOLOAD   = 1 << 4, /* POSIX si dispo */
  VT_DL_NODELETE = 1 << 5  /* POSIX si dispo */
};

/* ----------------------------------------------------------------------------
   Erreur thread-local
---------------------------------------------------------------------------- */
static VT_THREAD_LOCAL char vt__dl_err[512];

static void vt__set_err(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(vt__dl_err, sizeof vt__dl_err, fmt, ap);
  va_end(ap);
}

const char* vt_dl_error(void) {
#if defined(_WIN32)
  if (vt__dl_err[0]) return vt__dl_err;
  static VT_THREAD_LOCAL char buf[512];
  DWORD code = GetLastError();
  if (!code) return "";
  DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, code, 0, buf, (DWORD)sizeof(buf), NULL);
  if (!n) { _snprintf(buf, sizeof buf, "Win32 error %lu", (unsigned long)code); }
  /* strip \r\n */
  for (char* p = buf; *p; ++p) if (*p == '\r' || *p == '\n') { *p = 0; break; }
  return buf;
#else
  const char* s = dlerror();
  return s ? s : vt__dl_err;
#endif
}

/* ----------------------------------------------------------------------------
   Helpers mini
---------------------------------------------------------------------------- */
static int vt__is_abs_path(const char* p) {
  if (!p || !*p) return 0;
#if defined(_WIN32)
  /* "C:\", "\\server\share\..." or "/..." (msys) */
  if ((p[0] && p[1] == ':') || (p[0] == '\\' && p[1] == '\\') || p[0] == '/') return 1;
  return 0;
#else
  return p[0] == '/';
#endif
}
static void vt__path_join(char* out, size_t cap, const char* a, const char* b) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (!a || !*a) { (void)snprintf(out, cap, "%s", b ? b : ""); return; }
  if (!b || !*b) { (void)snprintf(out, cap, "%s", a); return; }
  size_t na = strlen(a);
  int need_sep =
#if defined(_WIN32)
    !(na && (a[na-1] == '\\' || a[na-1] == '/'));
#else
    !(na && a[na-1] == '/');
#endif
#if defined(_WIN32)
  (void)snprintf(out, cap, "%s%s%s", a, need_sep ? "\\" : "", b);
#else
  (void)snprintf(out, cap, "%s%s%s", a, need_sep ? "/" : "", b);
#endif
}
static void vt__basename(const char* path, char* out, size_t cap) {
  if (!path || !*path) { if (cap) out[0]=0; return; }
  const char* s = path;
#if defined(_WIN32)
  const char *p1 = strrchr(s, '\\'), *p2 = strrchr(s, '/');
  const char* p = (p1 && p2) ? (p1 > p2 ? p1 : p2) : (p1 ? p1 : p2);
#else
  const char* p = strrchr(s, '/');
#endif
  const char* base = p ? p+1 : s;
  (void)snprintf(out, cap, "%s", base);
}
static void vt__dirname(const char* path, char* out, size_t cap) {
  if (!path || !*path) { if (cap) out[0]=0; return; }
  char tmp[1024];
  (void)snprintf(tmp, sizeof tmp, "%s", path);
#if defined(_WIN32)
  char* p1 = strrchr(tmp, '\\');
  char* p2 = strrchr(tmp, '/');
  char* p = (p1 && p2) ? (p1 > p2 ? p1 : p2) : (p1 ? p1 : p2);
#else
  char* p = strrchr(tmp, '/');
#endif
  if (!p) { if (cap) out[0]=0; return; }
  *p = 0;
  (void)snprintf(out, cap, "%s", tmp);
}

/* ----------------------------------------------------------------------------
   Extensions et préfixe par plateforme
---------------------------------------------------------------------------- */
const char* vt_dl_default_ext(void) {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}
const char* vt_dl_default_prefix(void) {
#if defined(_WIN32)
  return ""; /* pas de "lib" */
#else
  return "lib";
#endif
}

#if !defined(_WIN32)
  #define VT_STRCASECMP strcasecmp
#else
  #define VT_STRCASECMP _stricmp
#endif

int vt_dl_add_ext_if_missing(const char* in, char* out, size_t cap) {
  if (!in || !out || cap == 0) return -1;
  const char* ext = vt_dl_default_ext();
  size_t n = strlen(in), e = strlen(ext);
  int has = (n >= e && VT_STRCASECMP(in + (n - e), ext) == 0);
  if (has) { if (n + 1 > cap) return -1; memcpy(out, in, n + 1); return 0; }
  if (n + e + 1 > cap) return -1;
  memcpy(out, in, n);
  memcpy(out + n, ext, e + 1);
  return 0;
}
int vt_dl_add_prefix_if_missing(const char* in, char* out, size_t cap) {
  if (!in || !out || cap == 0) return -1;
  const char* pre = vt_dl_default_prefix();
  size_t n = strlen(in), p = strlen(pre);
  int has = (n >= p && strncmp(in, pre, p) == 0);
  if (has || p == 0) { if (n + 1 > cap) return -1; memcpy(out, in, n + 1); return 0; }
  if (n + p + 1 > cap) return -1;
  memcpy(out, pre, p);
  memcpy(out + p, in, n + 1);
  return 0;
}
/* Construit "lib<base>.ext" ou "<base>.dll" suivant OS */
int vt_dl_build_name(const char* base, char* out, size_t cap) {
  if (!base || !out || cap == 0) return -1;
  char tmp[1024];
  if (vt_dl_add_prefix_if_missing(base, tmp, sizeof tmp) != 0) return -1;
  return vt_dl_add_ext_if_missing(tmp, out, cap);
}

/* ----------------------------------------------------------------------------
   Open impl
---------------------------------------------------------------------------- */
static void* vt__dl_open_raw(const char* path, int flags) {
  if (!path || !*path) { vt__set_err("null path"); return NULL; }
#if defined(_WIN32)
  DWORD dw = 0;
  /* LOAD_LIBRARY_SEARCH_* requires SetDefaultDllDirectories, avoided for wide support */
  HMODULE h = LoadLibraryExA(path, NULL, dw);
  if (!h) vt__set_err("LoadLibraryExA failed for '%s'", path);
  return (void*)h;
#else
  int f = 0;
  f |= (flags & VT_DL_NOW)     ? RTLD_NOW    : RTLD_LAZY;
  f |= (flags & VT_DL_GLOBAL)  ? RTLD_GLOBAL : RTLD_LOCAL;
  #ifdef RTLD_NOLOAD
    if (flags & VT_DL_NOLOAD)   f |= RTLD_NOLOAD;
  #endif
  #ifdef RTLD_NODELETE
    if (flags & VT_DL_NODELETE) f |= RTLD_NODELETE;
  #endif
  dlerror(); /* clear */
  void* h = dlopen(path, f);
  if (!h) vt__set_err("%s", vt_dl_error());
  return h;
#endif
}

void* vt_dl_open(const char* path) {
  return vt__dl_open_raw(path, VT_DL_LAZY | VT_DL_LOCAL);
}
void* vt_dl_open2(const char* path, int flags) {
  return vt__dl_open_raw(path, flags);
}
void* vt_dl_open_with_ext(const char* base, int flags) {
  if (!base) { vt__set_err("null path"); return NULL; }
  /* si chemin absolu ou contient un séparateur, tenter tel quel puis ext */
  int looks_path =
#if defined(_WIN32)
      (strchr(base, '\\') || strchr(base, '/')) || vt__is_abs_path(base);
#else
      (strchr(base, '/')) || vt__is_abs_path(base);
#endif
  if (looks_path) {
    void* h = vt__dl_open_raw(base, flags);
    if (h) return h;
    char buf[1024];
    if (vt_dl_add_ext_if_missing(base, buf, sizeof buf) == 0)
      return vt__dl_open_raw(buf, flags);
    return NULL;
  }
  /* sinon: considérer comme nom de module simple */
  char name[1024];
  if (vt_dl_build_name(base, name, sizeof name) != 0) {
    vt__set_err("build name failed");
    return NULL;
  }
  return vt__dl_open_raw(name, flags);
}
void* vt_dl_open_name(const char* soname, int flags) {
  return vt_dl_open_with_ext(soname, flags);
}

void* vt_dl_self(void) {
#if defined(_WIN32)
  return (void*)GetModuleHandleA(NULL);
#else
  #if defined(RTLD_DEFAULT)
    return RTLD_DEFAULT;
  #else
    return dlopen(NULL, RTLD_LAZY);
  #endif
#endif
}
#if !defined(_WIN32)
void* vt_dl_next(void) {
  #if defined(RTLD_NEXT)
  return RTLD_NEXT;
  #else
  return NULL;
  #endif
}
#endif

/* ----------------------------------------------------------------------------
   Symboles
---------------------------------------------------------------------------- */
void* vt_dl_sym(void* handle, const char* name) {
  if (!name) { vt__set_err("null symbol"); return NULL; }
#if defined(_WIN32)
  FARPROC p = GetProcAddress((HMODULE)(handle ? handle : GetModuleHandleA(NULL)), name);
  if (!p) vt__set_err("GetProcAddress failed for '%s'", name);
  return (void*)p;
#else
  dlerror(); /* clear */
  void* p = dlsym(handle ? handle : vt_dl_self(), name);
  const char* e = dlerror();
  if (e) { vt__set_err("%s", e); return NULL; }
  return p;
#endif
}

#if !defined(_WIN32)
void* vt_dl_symv(void* handle, const char* name, const char* version) {
  if (!name || !version) { vt__set_err("null symbol/version"); return NULL; }
  #if defined(__GLIBC__) || defined(__USE_GNU) || defined(RTLD_DEFAULT)
    /* dlvsym est GNU ; protégé par feature test macros */
    typedef void* (*dlvsym_fn)(void*, const char*, const char*);
    static dlvsym_fn pdlvsym = NULL;
    if (!pdlvsym) {
      /* dlsym(RTLD_DEFAULT,"dlvsym") est portable sur glibc */
      void* self = vt_dl_self();
      pdlvsym = (dlvsym_fn)dlsym(self, "dlvsym");
    }
    if (pdlvsym) {
      void* p = pdlvsym(handle ? handle : vt_dl_self(), name, version);
      if (!p) vt__set_err("dlvsym failed for '%s@%s'", name, version);
      return p;
    }
    vt__set_err("dlvsym unavailable on this platform");
    return NULL;
  #else
    (void)handle; (void)name; (void)version;
    vt__set_err("dlvsym unsupported");
    return NULL;
  #endif
}
#endif

/* ----------------------------------------------------------------------------
   Fermer
---------------------------------------------------------------------------- */
int vt_dl_close(void* handle) {
  if (!handle) return 0;
#if defined(_WIN32)
  BOOL ok = FreeLibrary((HMODULE)handle);
  if (!ok) { vt__set_err("FreeLibrary failed"); return -1; }
  return 0;
#else
  int rc = dlclose(handle);
  if (rc != 0) { vt__set_err("%s", vt_dl_error()); return -1; }
  return 0;
#endif
}

/* ----------------------------------------------------------------------------
   Informations chemin module
---------------------------------------------------------------------------- */
int vt_dl_get_module_path(void* handle, char* out, size_t cap) {
  if (!out || cap == 0) return -1;
  out[0] = 0;
#if defined(_WIN32)
  HMODULE h = (HMODULE)(handle ? handle : GetModuleHandleA(NULL));
  DWORD n = GetModuleFileNameA(h, out, (DWORD)cap);
  if (n == 0 || n >= cap) { vt__set_err("GetModuleFileNameA failed"); return -1; }
  return 0;
#elif defined(__APPLE__)
  /* Si handle == self, _NSGetExecutablePath */
  if (!handle || handle == vt_dl_self()) {
    uint32_t size = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &size) != 0) { vt__set_err("path too small"); return -1; }
    return 0;
  }
  /* Fallback dladdr pour retrouver image */
  Dl_info info;
  if (dladdr(handle, &info) == 0 || !info.dli_fname) { vt__set_err("dladdr failed"); return -1; }
  (void)snprintf(out, cap, "%s", info.dli_fname);
  return 0;
#else
  if (!handle || handle == vt_dl_self()) {
    /* /proc/self/exe si dispo */
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n < 0) { vt__set_err("readlink /proc/self/exe failed: %s", strerror(errno)); return -1; }
    out[n] = 0;
    return 0;
  }
  Dl_info info;
  if (dladdr(handle, &info) == 0 || !info.dli_fname) { vt__set_err("dladdr failed"); return -1; }
  (void)snprintf(out, cap, "%s", info.dli_fname);
  return 0;
#endif
}
int vt_dl_get_module_dir(void* handle, char* out, size_t cap) {
  char path[1024];
  if (vt_dl_get_module_path(handle, path, sizeof path) != 0) return -1;
  vt__dirname(path, out, cap);
  return 0;
}

/* ----------------------------------------------------------------------------
   Gestion d’une petite pile de répertoires de recherche (process-local)
---------------------------------------------------------------------------- */
#define VT_DL_MAX_DIRS 16
static char vt__dirs[VT_DL_MAX_DIRS][512];
static int  vt__ndirs = 0;

void vt_dl_clear_search_dirs(void) { vt__ndirs = 0; }
int vt_dl_push_search_dir(const char* dir) {
  if (!dir || !*dir) return -1;
  if (vt__ndirs >= VT_DL_MAX_DIRS) return -1;
  (void)snprintf(vt__dirs[vt__ndirs], sizeof vt__dirs[vt__ndirs], "%s", dir);
  vt__ndirs++;
  return 0;
}
int vt_dl_pop_search_dir(void) {
  if (vt__ndirs <= 0) return -1;
  vt__ndirs--;
  return 0;
}

/* Essaie ouverture en parcourant la pile de répertoires si base non absolu.
   base_or_path peut être un chemin complet OU un nom de module (ex: "m"). */
void* vt_dl_search_open(const char* base_or_path, int flags) {
  if (!base_or_path || !*base_or_path) { vt__set_err("null name"); return NULL; }

  /* Si absolu ou contient un séparateur, on tente directement + ext éventuelle */
  int has_sep =
#if defined(_WIN32)
    (strchr(base_or_path, '\\') || strchr(base_or_path, '/'));
#else
    (strchr(base_or_path, '/'));
#endif
  if (vt__is_abs_path(base_or_path) || has_sep) {
    void* h = vt__dl_open_raw(base_or_path, flags);
    if (h) return h;
    char with_ext[1024];
    if (vt_dl_add_ext_if_missing(base_or_path, with_ext, sizeof with_ext) == 0)
      return vt__dl_open_raw(with_ext, flags);
    return NULL;
  }

  /* Sinon : nom logique. On construit "lib<name>.ext" ou "<name>.dll" */
  char built[1024];
  if (vt_dl_build_name(base_or_path, built, sizeof built) != 0) {
    vt__set_err("build name failed");
    return NULL;
  }

  /* 1) Essai courant (dépend de l’OS, le chargeur a déjà ses paths) */
  void* h = vt__dl_open_raw(built, flags);
  if (h) return h;

  /* 2) Parcourt les répertoires empilés */
  for (int i = vt__ndirs - 1; i >= 0; --i) {
    char full[1024];
    vt__path_join(full, sizeof full, vt__dirs[i], built);
    h = vt__dl_open_raw(full, flags);
    if (h) return h;
  }

  return NULL;
}

/* ----------------------------------------------------------------------------
   Démo (désactivée)
   cc -std=c11 -DVT_DL_TEST dl.c -ldl
---------------------------------------------------------------------------- */
#ifdef VT_DL_TEST
#include <stdio.h>
int main(void) {
  vt_dl_push_search_dir("/lib");
  vt_dl_push_search_dir("/usr/lib");

#if defined(__APPLE__)
  const char* mod = "m";
#else
  const char* mod = "m";
#endif

  void* h = vt_dl_search_open(mod, VT_DL_NOW | VT_DL_LOCAL);
  if (!h) { fprintf(stderr, "open: %s\n", vt_dl_error()); return 1; }

  void* sym = vt_dl_sym(h, "cos");
  if (!sym) fprintf(stderr, "sym: %s\n", vt_dl_error());

  char p[1024], d[1024];
  if (vt_dl_get_module_path(h, p, sizeof p)==0) printf("path: %s\n", p);
  if (vt_dl_get_module_dir(h, d, sizeof d)==0)  printf("dir : %s\n", d);

  vt_dl_close(h);
  puts("OK");
  return 0;
}
#endif
