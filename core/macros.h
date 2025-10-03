/* ============================================================================
 * macros.h — Public API for macro table (C11)
 * VitteLight / Vitl runtime
 * Store, query and manage string macros (key=value)
 * ========================================================================== */
#ifndef VITTELIGHT_MACROS_H
#define VITTELIGHT_MACROS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Visibility */
#ifndef MACROS_API
#  if defined(_WIN32) && defined(MACROS_DLL)
#    ifdef MACROS_BUILD
#      define MACROS_API __declspec(dllexport)
#    else
#      define MACROS_API __declspec(dllimport)
#    endif
#  else
#    define MACROS_API
#  endif
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* ===== Types ============================================================== */

typedef struct {
    char* name;
    char* value;
} macro_entry;

typedef struct {
    macro_entry* entries;
    size_t       count;
    size_t       cap;
} macro_table;

/* ===== API ================================================================ */

/* Initialize / free */
MACROS_API void   macros_init(macro_table* t);
MACROS_API void   macros_free(macro_table* t);

/* Add or replace macro. Returns 0 ok, -1 error. */
MACROS_API int    macros_define(macro_table* t, const char* name, const char* value);

/* Undefine macro. Returns 1 if removed, 0 if not found. */
MACROS_API int    macros_undef(macro_table* t, const char* name);

/* Query value or NULL. */
MACROS_API const char* macros_get(const macro_table* t, const char* name);

/* Existence test. */
MACROS_API bool   macros_has(const macro_table* t, const char* name);

/* Count of macros. */
MACROS_API size_t macros_count(const macro_table* t);

/* Indexed access (NULL if out of range). */
MACROS_API const char* macros_name_at (const macro_table* t, size_t idx);
MACROS_API const char* macros_value_at(const macro_table* t, size_t idx);

/* Dump macros as #define lines to file. */
MACROS_API void   macros_dump(const macro_table* t, FILE* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITTELIGHT_MACROS_H */
