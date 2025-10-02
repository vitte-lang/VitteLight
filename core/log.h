/* ============================================================================
 * log.h — Public API for portable logging (C11)
 * VitteLight / Vitl runtime
 * Levels: TRACE, DEBUG, INFO, WARN, ERROR, FATAL
 * Build flags: LOG_DISABLE_COLOR, LOG_DISABLE_TIME, LOG_USE_UTC, LOG_NO_TRACE
 * ============================================================================
 */
#ifndef VITTELIGHT_LOG_H
#define VITTELIGHT_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Visibility */
#ifndef LOG_API
#  if defined(_WIN32) && defined(LOG_DLL)
#    ifdef LOG_BUILD
#      define LOG_API __declspec(dllexport)
#    else
#      define LOG_API __declspec(dllimport)
#    endif
#  else
#    define LOG_API
#  endif
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* ===== Levels ============================================================= */
typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO  = 2,
    LOG_WARN  = 3,
    LOG_ERROR = 4,
    LOG_FATAL = 5
} log_level_t;

/* Custom time callback: fills buf with NUL-terminated timestamp */
typedef void (*log_time_cb)(char* buf, size_t cap);

/* ===== Init / config ====================================================== */
LOG_API void log_init(log_level_t level);
LOG_API void log_set_level(log_level_t level);
LOG_API void log_set_use_color(bool enable);
LOG_API void log_set_output(FILE* fp);
LOG_API void log_set_time_cb(log_time_cb cb);
LOG_API void log_set_prefix(const char* prefix); /* e.g., "vitte" */

/* ===== Core logging ======================================================= */
LOG_API void log_log (log_level_t level, const char* fmt, ...);
LOG_API void log_vlog(log_level_t level, const char* fmt, va_list ap);

/* Convenience */
#ifndef LOG_NO_TRACE
LOG_API void log_trace(const char* fmt, ...);
#endif
LOG_API void log_debug(const char* fmt, ...);
LOG_API void log_info (const char* fmt, ...);
LOG_API void log_warn (const char* fmt, ...);
LOG_API void log_error(const char* fmt, ...);
LOG_API void log_fatal(const char* fmt, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITTELIGHT_LOG_H */
