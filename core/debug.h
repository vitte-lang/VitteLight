/* ============================================================================
   debug.h — logging/debug cross-platform (C17)
   Niveaux: TRACE..FATAL. Formats: texte ou JSON. Sorties: stderr ou fichier.
   Thread-safe. Horodatage, TID, source (file:line:func). Hexdump, backtrace.
   Lier avec debug.c. Licence: MIT.
   ============================================================================
 */
#ifndef VT_DEBUG_H
#define VT_DEBUG_H
#pragma once

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export et annotations printf-like
---------------------------------------------------------------------------- */
#ifndef VT_DEBUG_API
#define VT_DEBUG_API extern
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VT_PRINTF(fmt_idx, va_idx) \
  __attribute__((format(printf, fmt_idx, va_idx)))
#else
#define VT_PRINTF(fmt_idx, va_idx)
#endif

/* ----------------------------------------------------------------------------
   Niveaux et formats
---------------------------------------------------------------------------- */
typedef enum {
  VT_LL_TRACE = 0,
  VT_LL_DEBUG = 1,
  VT_LL_INFO = 2,
  VT_LL_WARN = 3,
  VT_LL_ERROR = 4,
  VT_LL_FATAL = 5
} vt_log_level;

typedef enum { VT_FMT_TEXT = 0, VT_FMT_JSON = 1 } vt_log_format;

/* ----------------------------------------------------------------------------
   Configuration runtime
---------------------------------------------------------------------------- */
typedef struct {
  vt_log_level level;    /* niveau minimal */
  vt_log_format format;  /* texte ou JSON */
  int use_color;         /* 1=couleur si TTY */
  const char* file_path; /* NULL=stderr */
  size_t rotate_bytes;   /* 0=désactivé */
  int capture_crash;     /* installe handlers */
} vt_log_config;

/* ----------------------------------------------------------------------------
   API
---------------------------------------------------------------------------- */
VT_DEBUG_API int vt_log_init(const vt_log_config* cfg);
VT_DEBUG_API void vt_log_shutdown(void);

VT_DEBUG_API void vt_log_set_level(vt_log_level lvl);
VT_DEBUG_API vt_log_level vt_log_get_level(void);

VT_DEBUG_API void vt_log_set_format(vt_log_format fmt);
VT_DEBUG_API void vt_log_enable_color(int on);
VT_DEBUG_API void vt_log_force_flush(void);
VT_DEBUG_API void vt_log_set_file(const char* path, size_t rotate_bytes);

/* message: printf-like */
VT_DEBUG_API void vt_log_write(vt_log_level lvl, const char* file, int line,
                               const char* func, const char* fmt, ...)
    VT_PRINTF(5, 6);

/* utilitaires */
VT_DEBUG_API void vt_debug_hexdump(const void* data, size_t len,
                                   const char* label);
VT_DEBUG_API void vt_debug_backtrace(void);
VT_DEBUG_API void vt_debug_install_crash_handlers(void);

/* ----------------------------------------------------------------------------
   Macros de confort
---------------------------------------------------------------------------- */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define VT_FUNC __func__
#elif defined(_MSC_VER)
#define VT_FUNC __FUNCTION__
#else
#define VT_FUNC "?"
#endif

/* Seuil compile-time. Par défaut: tout. */
#ifndef VT_LOG_COMPILETIME_LEVEL
#define VT_LOG_COMPILETIME_LEVEL VT_LL_TRACE
#endif

/* Appel interne conditionnel */
#define VT__LOG_ENABLED(LVL) ((LVL) >= VT_LOG_COMPILETIME_LEVEL)

/* Macro générique */
#define VT_LOG(LVL, ...)                                             \
  do {                                                               \
    if (VT__LOG_ENABLED(LVL))                                        \
      vt_log_write((LVL), __FILE__, __LINE__, VT_FUNC, __VA_ARGS__); \
  } while (0)

/* Niveaux dédiés */
#define VT_TRACE(...) VT_LOG(VT_LL_TRACE, __VA_ARGS__)
#define VT_DEBUG(...) VT_LOG(VT_LL_DEBUG, __VA_ARGS__)
#define VT_INFO(...)  VT_LOG(VT_LL_INFO,  __VA_ARGS__)
#define VT_WARN(...)  VT_LOG(VT_LL_WARN,  __VA_ARGS__)
#define VT_ERROR(...) VT_LOG(VT_LL_ERROR, __VA_ARGS__)
#define VT_FATAL(...) VT_LOG(VT_LL_FATAL, __VA_ARGS__)

VT_DEBUG_API void vt_assert_fail(const char* cond, const char* file, int line,
                                 const char* func, const char* fmt, ...)
    VT_PRINTF(5, 6);

/* Assert light: log fatal with condition and custom message */
#define VT_ASSERT(COND, ...)                                                \
  do {                                                                      \
    if (!(COND)) {                                                          \
      vt_assert_fail(#COND, __FILE__, __LINE__, VT_FUNC, __VA_ARGS__);      \
    }                                                                       \
  } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_DEBUG_H */
