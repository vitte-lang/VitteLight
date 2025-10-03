/* ============================================================================
 * module.h — Public API for module loader/registry (C11)
 * VitteLight / Vitl runtime
 * ========================================================================== */
#ifndef VITTELIGHT_MODULE_H
#define VITTELIGHT_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Visibility */
#ifndef MODULE_API
#  if defined(_WIN32) && defined(MODULE_DLL)
#    ifdef MODULE_BUILD
#      define MODULE_API __declspec(dllexport)
#    else
#      define MODULE_API __declspec(dllimport)
#    endif
#  else
#    define MODULE_API
#  endif
#endif

#include <stddef.h>

/* ===== Types ============================================================== */

typedef struct module module;
typedef struct module_registry module_registry;

/* ===== Registry management ================================================ */

/* Initialize / free registry */
MODULE_API void module_registry_init (module_registry* R);
MODULE_API void module_registry_free (module_registry* R);

/* Add search path for resolving modules (directories). */
MODULE_API int  module_registry_add_search_path(module_registry* R, const char* dir);

/* Add default search paths (exe dir, lib dir, "."). */
MODULE_API void module_registry_add_defaults(module_registry* R,
                                             const char* exe_dir,
                                             const char* lib_dir);

/* Load module by name or path. Increments ref if already loaded. */
MODULE_API module* module_registry_load(module_registry* R, const char* name_or_path);

/* Unload one ref. If ref→0, release. Returns remaining refcnt or -1. */
MODULE_API int module_registry_unload(module_registry* R, const char* name);

/* Get pointer by name, or NULL. */
MODULE_API module* module_registry_get(module_registry* R, const char* name);

/* ===== Module inspection ================================================== */

/* Return name, path, type, refcnt. */
MODULE_API const char* module_name   (const module* m);
MODULE_API const char* module_path   (const module* m);
MODULE_API int         module_is_dylib(const module* m);
MODULE_API int         module_is_file (const module* m);
MODULE_API int         module_refcnt  (const module* m);

/* For file modules: get blob ptr/size. Returns 0/-1. */
MODULE_API int module_get_blob(const module* m, const void** ptr, size_t* size);

/* For dylib modules: resolve symbol, or NULL. */
MODULE_API void* module_dylib_sym(const module* m, const char* sym);

/* ===== Notes ==============================================================
 * - Module logical name = given string in load() call.
 * - File modules are memory-mapped blobs (read-only).
 * - Dylib modules may expose optional hooks:
 *     int  vitl_module_init(module* m);
 *     void vitl_module_term(module* m);
 * ========================================================================== */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITTELIGHT_MODULE_H */
