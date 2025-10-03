/* ============================================================================
 * module.c — Module loader & registry (C11) for VitteLight / Vitl
 *  - Kinds: FILE (mapped blob), DYLIB (shared library)
 *  - Search paths, refcount, init/term hooks
 *  - Zero non-libc deps; uses loader.h + log.h
 * ========================================================================== */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "loader.h"
#include "log.h"

/* ===== Platform =========================================================== */
#if defined(_WIN32)
#  define MOD_DYLIB_EXT ".dll"
#else
#  if defined(__APPLE__)
#    define MOD_DYLIB_EXT ".dylib"
#  else
#    define MOD_DYLIB_EXT ".so"
#  endif
#endif

/* ===== Types ============================================================== */

typedef enum { MOD_FILE = 1, MOD_DYLIB = 2 } module_kind;

typedef struct module module;

typedef int  (*mod_init_fn)(module* m);   /* optional. return 0 on success */
typedef void (*mod_term_fn)(module* m);   /* optional */

typedef struct {
    void*  ptr;    /* mapped or heap blob (read-only) */
    size_t size;
    bool   is_mapped;
#if defined(_WIN32)
    void  *hFile, *hMap; /* mirrors loader_map fields when mapped */
#else
    int    fd;
#endif
} mod_blob;

struct module {
    char*        name;
    char*        path;
    module_kind  kind;
    int          refcnt;

    /* one of: */
    loader_dylib dylib;
    mod_blob     blob;

    /* optional hooks resolved for DYLIB, ignored for FILE */
    mod_init_fn  on_init;
    mod_term_fn  on_term;
};

typedef struct {
    module** items;
    size_t   count, cap;

    char**   search_paths;
    size_t   sp_count, sp_cap;
} module_registry;

/* ===== Small utils ======================================================== */

static char* mod_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

static void* grow_array(void* ptr, size_t elem, size_t cap, size_t min_needed, size_t* out_cap) {
    size_t newcap = cap ? cap * 2 : 8;
    if (newcap < min_needed) newcap = min_needed;
    void* np = realloc(ptr, newcap * elem);
    if (!np) return NULL;
    *out_cap = newcap;
    return np;
}

/* ===== Blob helpers (wrap loader_map) ==================================== */

static int blob_open_file(const char* path, mod_blob* b) {
    memset(b, 0, sizeof *b);
    loader_map m = {0};
    if (loader_map_open(path, &m) != 0) {
        log_error("module: map_open failed: %s", loader_last_error());
        return -1;
    }
    b->ptr = m.ptr;
    b->size = m.size;
#if defined(_WIN32)
    b->is_mapped = m.is_mapped;
    b->hFile = m.hFile;
    b->hMap  = m.hMap;
#else
    b->is_mapped = m.is_mapped;
    b->fd = m.fd;
#endif
    return 0;
}

static void blob_close(mod_blob* b) {
    if (!b) return;
#if defined(_WIN32)
    if (b->is_mapped) {
        if (b->ptr) UnmapViewOfFile(b->ptr);
        if (b->hMap)  CloseHandle((HANDLE)b->hMap);
        if (b->hFile) CloseHandle((HANDLE)b->hFile);
    } else {
        free(b->ptr);
    }
#else
    if (b->is_mapped) {
        if (b->ptr && b->size) munmap(b->ptr, b->size);
        if (b->fd >= 0) close(b->fd);
    } else {
        free(b->ptr);
    }
#endif
    memset(b, 0, sizeof *b);
}

/* ===== Public-ish registry API (no header requested) ===================== */

void module_registry_init(module_registry* R) {
    if (!R) return;
    memset(R, 0, sizeof *R);
}

void module_registry_free(module_registry* R) {
    if (!R) return;
    for (size_t i = 0; i < R->count; ++i) {
        module* m = R->items[i];
        if (!m) continue;
        if (m->kind == MOD_DYLIB) {
            if (m->on_term) m->on_term(m);
            loader_dylib_close(&m->dylib);
        } else {
            blob_close(&m->blob);
        }
        free(m->name);
        free(m->path);
        free(m);
    }
    free(R->items);
    for (size_t i = 0; i < R->sp_count; ++i) free(R->search_paths[i]);
    free(R->search_paths);
    memset(R, 0, sizeof *R);
}

/* Add a search path (dedup naive). Returns 0/-1. */
int module_registry_add_search_path(module_registry* R, const char* dir) {
    if (!R || !dir || !*dir) return -1;
    for (size_t i = 0; i < R->sp_count; ++i) {
        if (strcmp(R->search_paths[i], dir) == 0) return 0;
    }
    if (R->sp_count + 1 > R->sp_cap) {
        void* np = grow_array(R->search_paths, sizeof(char*), R->sp_cap, R->sp_count + 1, &R->sp_cap);
        if (!np) return -1;
        R->search_paths = (char**)np;
    }
    R->search_paths[R->sp_count++] = mod_strdup(dir);
    return 0;
}

static module* module_find_by_name(module_registry* R, const char* name) {
    if (!R || !name) return NULL;
    for (size_t i = 0; i < R->count; ++i) {
        if (R->items[i] && strcmp(R->items[i]->name, name) == 0) return R->items[i];
    }
    return NULL;
}

static void registry_insert(module_registry* R, module* m) {
    if (R->count + 1 > R->cap) {
        void* np = grow_array(R->items, sizeof(module*), R->cap, R->count + 1, &R->cap);
        if (!np) return; /* OOM: caller should check ref later */
        R->items = (module**)np;
    }
    R->items[R->count++] = m;
}

/* Build candidate absolute path: dir + sep + base */
static int build_join(char* out, size_t cap, const char* dir, const char* base) {
    if (dir && *dir) return (int)loader_join_path(out, cap, dir, base);
    size_t need = strlen(base);
    if (need + 1 > cap) return (int)need;
    memcpy(out, base, need + 1);
    return (int)need;
}

/* Try to resolve a file in search paths. Returns 0 on success and fills dst. */
static int resolve_in_paths(module_registry* R, const char* base, char* dst, size_t cap) {
    /* 1) Exact as given if exists */
    if (loader_file_exists(base) == 1) {
        size_t need = strlen(base);
        if (need + 1 > cap) return -1;
        memcpy(dst, base, need + 1);
        return 0;
    }
    /* 2) Try each search path */
    for (size_t i = 0; i < R->sp_count; ++i) {
        char buf[1024];
        int n = build_join(buf, sizeof buf, R->search_paths[i], base);
        if (n < 0) continue;
        if (loader_file_exists(buf) == 1) {
            size_t need = strlen(buf);
            if (need + 1 > cap) return -1;
            memcpy(dst, buf, need + 1);
            return 0;
        }
    }
    return -1;
}

/* Heuristics: given a module name, produce candidate filenames.
   We try as-is, then name + .vitl, name + .vbc, then dynlib suffix. */
static int make_candidates(const char* name, char* out1, size_t c1,
                           char* out2, size_t c2, char* out3, size_t c3, char* out4, size_t c4) {
    if (!name || !*name) return -1;
    /* name (as path) */
    size_t n = strlen(name);
    if (n + 1 > c1) return -1;
    memcpy(out1, name, n + 1);

    /* name.vitl */
    const char* ext1 = ".vitl";
    if (n + strlen(ext1) + 1 <= c2) {
        memcpy(out2, name, n);
        memcpy(out2 + n, ext1, strlen(ext1) + 1);
    } else { out2[0] = 0; }

    /* name.vbc */
    const char* ext2 = ".vbc";
    if (n + strlen(ext2) + 1 <= c3) {
        memcpy(out3, name, n);
        memcpy(out3 + n, ext2, strlen(ext2) + 1);
    } else { out3[0] = 0; }

    /* name + dylib */
    const char* ext3 = MOD_DYLIB_EXT;
    if (n + strlen(ext3) + 1 <= c4) {
        memcpy(out4, name, n);
        memcpy(out4 + n, ext3, strlen(ext3) + 1);
    } else { out4[0] = 0; }

    return 0;
}

/* Load as FILE blob */
static module* load_file_module(const char* resolved_path, const char* logical_name) {
    mod_blob b;
    if (blob_open_file(resolved_path, &b) != 0) return NULL;

    module* m = (module*)calloc(1, sizeof *m);
    if (!m) { blob_close(&b); return NULL; }
    m->kind = MOD_FILE;
    m->name = mod_strdup(logical_name ? logical_name : resolved_path);
    m->path = mod_strdup(resolved_path);
    m->refcnt = 1;
    m->blob = b;

    log_debug("module: loaded FILE '%s' (%zu bytes)", m->path, m->blob.size);
    return m;
}

/* Load as DYLIB and resolve hooks */
static module* load_dylib_module(const char* resolved_path, const char* logical_name) {
    loader_dylib lib;
    if (loader_dylib_open(resolved_path, &lib) != 0) {
        log_error("module: dlopen failed: %s", loader_last_error());
        return NULL;
    }

    module* m = (module*)calloc(1, sizeof *m);
    if (!m) { loader_dylib_close(&lib); return NULL; }
    m->kind = MOD_DYLIB;
    m->name = mod_strdup(logical_name ? logical_name : resolved_path);
    m->path = mod_strdup(resolved_path);
    m->refcnt = 1;
    m->dylib = lib;

    /* Optional symbols: vitl_module_init / vitl_module_term */
    m->on_init = (mod_init_fn)loader_dylib_sym(&m->dylib, "vitl_module_init");
    m->on_term = (mod_term_fn)loader_dylib_sym(&m->dylib, "vitl_module_term");

    if (m->on_init) {
        int rc = m->on_init(m);
        if (rc != 0) {
            log_error("module: init hook failed for '%s' rc=%d", m->name, rc);
            loader_dylib_close(&m->dylib);
            free(m->name); free(m->path); free(m);
            return NULL;
        }
    }

    log_debug("module: loaded DYLIB '%s'", m->path);
    return m;
}

/* Public: load by name or path. Return ptr or NULL. Increments ref if already present. */
module* module_registry_load(module_registry* R, const char* name_or_path) {
    if (!R || !name_or_path || !*name_or_path) return NULL;

    /* If already loaded by that logical name, bump ref. */
    module* already = module_find_by_name(R, name_or_path);
    if (already) { already->refcnt++; return already; }

    /* Build candidates */
    char c1[512], c2[512], c3[512], c4[512];
    if (make_candidates(name_or_path, c1, sizeof c1, c2, sizeof c2, c3, sizeof c3, c4, sizeof c4) != 0)
        return NULL;

    /* Resolve against search paths */
    char resolved[1024];
    const char* candidates[] = { c1, c2, c3, c4 };
    for (size_t i = 0; i < 4; ++i) {
        if (!candidates[i] || !*candidates[i]) continue;
        if (resolve_in_paths(R, candidates[i], resolved, sizeof resolved) == 0) {
            /* Decide by extension if dynlib, else file */
            const char* p = resolved;
            size_t L = strlen(p);
            bool is_dylib = false;
            if (L >= strlen(MOD_DYLIB_EXT)) {
                if (strcmp(p + (L - strlen(MOD_DYLIB_EXT)), MOD_DYLIB_EXT) == 0) is_dylib = true;
            }
            module* m = is_dylib ? load_dylib_module(resolved, name_or_path)
                                 : load_file_module(resolved, name_or_path);
            if (!m) return NULL;
            registry_insert(R, m);
            return m;
        }
    }

    log_error("module: not found '%s'", name_or_path);
    return NULL;
}

/* Public: unload one ref. If ref hits 0, actually unload. Returns remaining refcnt or -1 on error. */
int module_registry_unload(module_registry* R, const char* name) {
    if (!R || !name) return -1;
    for (size_t i = 0; i < R->count; ++i) {
        module* m = R->items[i];
        if (!m || strcmp(m->name, name) != 0) continue;

        if (m->refcnt > 1) { m->refcnt--; return m->refcnt; }

        /* refcnt == 1 → destroy */
        if (m->kind == MOD_DYLIB) {
            if (m->on_term) m->on_term(m);
            loader_dylib_close(&m->dylib);
        } else {
            blob_close(&m->blob);
        }
        free(m->name);
        free(m->path);
        free(m);

        /* compact array */
        memmove(&R->items[i], &R->items[i+1], (R->count - i - 1) * sizeof(module*));
        R->count--;
        return 0;
    }
    return -1;
}

/* Get pointer by name. */
module* module_registry_get(module_registry* R, const char* name) {
    return module_find_by_name(R, name);
}

/* Expose basic info for clients that only have the pointer */
const char* module_name(const module* m) { return m ? m->name : NULL; }
const char* module_path(const module* m) { return m ? m->path : NULL; }
int         module_is_dylib(const module* m) { return m && m->kind == MOD_DYLIB; }
int         module_is_file (const module* m) { return m && m->kind == MOD_FILE; }
int         module_refcnt  (const module* m) { return m ? m->refcnt : 0; }

/* For FILE modules, get raw blob (ptr/size). Returns 0/-1. */
int module_get_blob(const module* m, const void** ptr, size_t* size) {
    if (!m || m->kind != MOD_FILE) return -1;
    if (ptr)  *ptr  = m->blob.ptr;
    if (size) *size = m->blob.size;
    return 0;
}

/* For DYLIB modules, resolve a symbol. */
void* module_dylib_sym(const module* m, const char* name) {
    if (!m || m->kind != MOD_DYLIB) return NULL;
    return loader_dylib_sym(&((module*)m)->dylib, name);
}

/* Convenience: add common default search paths, last wins. */
void module_registry_add_defaults(module_registry* R, const char* exe_dir, const char* lib_dir) {
    if (!R) return;
    if (exe_dir && *exe_dir) module_registry_add_search_path(R, exe_dir);
    if (lib_dir && *lib_dir) module_registry_add_search_path(R, lib_dir);
    /* local dir */
    module_registry_add_search_path(R, ".");
}

/* ===== Optional self-test ================================================ */
#ifdef MODULE_MAIN
int main(int argc, char** argv) {
    log_init(LOG_DEBUG);
    log_set_prefix("module");

    module_registry R;
    module_registry_init(&R);
    module_registry_add_defaults(&R, ".", "./libs");

    if (argc > 1) {
        /* Try load path or logical name. */
        module* m = module_registry_load(&R, argv[1]);
        if (!m) {
            log_error("failed to load '%s'", argv[1]);
            module_registry_free(&R);
            return 1;
        }
        log_info("loaded %s kind=%s ref=%d",
                 module_name(m),
                 module_is_dylib(m) ? "dylib" : "file",
                 module_refcnt(m));

        if (module_is_file(m)) {
            const void* p; size_t n;
            if (module_get_blob(m, &p, &n) == 0)
                log_info("blob size=%zu first=%02X", n, n ? ((const unsigned char*)p)[0] : 0);
        } else {
            void* f = module_dylib_sym(m, "vitl_module_version");
            log_info("sym vitl_module_version=%p", f);
        }

        /* Load again to test refcnt */
        module* m2 = module_registry_load(&R, argv[1]);
        log_info("again ref=%d", module_refcnt(m2));

        /* Unload twice */
        module_registry_unload(&R, argv[1]);
        int left = module_registry_unload(&R, argv[1]);
        log_info("after unload left=%d", left);
    }

    module_registry_free(&R);
    return 0;
}
#endif
