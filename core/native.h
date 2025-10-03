/* ============================================================================
 * native.h — Public API for native function registry (C11)
 * VitteLight / Vitl runtime
 * ========================================================================== */
#ifndef VITTELIGHT_NATIVE_H
#define VITTELIGHT_NATIVE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Visibility */
#ifndef NATIVE_API
#  if defined(_WIN32) && defined(NATIVE_DLL)
#    ifdef NATIVE_BUILD
#      define NATIVE_API __declspec(dllexport)
#    else
#      define NATIVE_API __declspec(dllimport)
#    endif
#  else
#    define NATIVE_API
#  endif
#endif

#include <stddef.h>
#include <stdint.h>

/* ===== Variant ABI ======================================================== */

enum {
    NAT_I64 = 1,
    NAT_F64 = 2,
    NAT_STR = 3,
    NAT_PTR = 4
};

typedef struct {
    int type; /* NAT_* */
    union {
        int64_t     i64;
        double      f64;
        const char* str;
        void*       ptr;
    };
} nat_val;

/* fn(udata, args, nargs, ret_out) → 0 on success, non-zero on error */
typedef int (*native_fn)(void* udata, const nat_val* args, size_t nargs, nat_val* ret);

/* Opaque registry type (defined in native.c) */
typedef struct native_registry native_registry;

/* ===== Core registry ====================================================== */

NATIVE_API void  native_init(native_registry* R);
NATIVE_API void  native_free(native_registry* R);

NATIVE_API int   native_register  (native_registry* R, const char* name, native_fn fn, void* udata); /* 0/-1 */
NATIVE_API int   native_unregister(native_registry* R, const char* name);                             /* 1/0  */

NATIVE_API native_fn native_find(const native_registry* R, const char* name, void** out_udata);       /* fn/NULL */
NATIVE_API int       native_call(const native_registry* R, const char* name,
                                 const nat_val* args, size_t nargs, nat_val* ret);                    /* 0/-1 */

/* Convenience: register small built-ins (time.now_ms, time.sleep_ms, env.getenv) */
NATIVE_API void  native_register_basics(native_registry* R);

/* ===== Dynamic loading (optional) =========================================
 * The shared library must export:
 *   void vitl_register_natives(native_registry* R);
 */
NATIVE_API int   native_load_library(native_registry* R, const char* path); /* 0/-1 */
NATIVE_API void  native_unload_libraries(native_registry* R);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITTELIGHT_NATIVE_H */
