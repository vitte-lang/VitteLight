/* ============================================================================
 * log.c — Portable logger for VitteLight (C11)
 *  - Levels: TRACE, DEBUG, INFO, WARN, ERROR, FATAL
 *  - Thread-safe, ISO-8601 time, optional ms, ANSI colors
 *  - Windows: active Virtual Terminal when possible
 * Build: cc -std=c11 -O2 -Wall -Wextra -pedantic -c log.c
 * Opts : -DLOG_DISABLE_COLOR -DLOG_DISABLE_TIME -DLOG_USE_UTC -DLOG_NO_TRACE
 * ========================================================================== */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  define isatty _isatty
#  define fileno _fileno
#else
#  include <sys/time.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

/* ===== Public API (declared here; make a log.h si besoin) ================= */

typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO  = 2,
    LOG_WARN  = 3,
    LOG_ERROR = 4,
    LOG_FATAL = 5
} log_level_t;

typedef void (*log_time_cb)(char* buf, size_t cap); /* remplit buf (NUL) */

void log_init(log_level_t level);
void log_set_level(log_level_t level);
void log_set_use_color(bool enable);
void log_set_output(FILE* fp);
void log_set_time_cb(log_time_cb cb);
void log_set_prefix(const char* prefix); /* ex: "vitte" ou "vitl" */

void log_log(log_level_t level, const char* fmt, ...);
void log_vlog(log_level_t level, const char* fmt, va_list ap);

#ifndef LOG_NO_TRACE
void log_trace(const char* fmt, ...);
#endif
void log_debug(const char* fmt, ...);
void log_info (const char* fmt, ...);
void log_warn (const char* fmt, ...);
void log_error(const char* fmt, ...);
void log_fatal(const char* fmt, ...);

/* ===== Impl =============================================================== */

#ifndef LOG_DEFAULT_LEVEL
#  define LOG_DEFAULT_LEVEL LOG_INFO
#endif

#ifndef LOG_TIME_BUFSZ
#  define LOG_TIME_BUFSZ 32
#endif

#ifndef LOG_PREFIX_BUFSZ
#  define LOG_PREFIX_BUFSZ 32
#endif

#ifndef LOG_LINE_BUFSZ
#  define LOG_LINE_BUFSZ 1024
#endif

typedef struct {
    FILE*       fp;
    log_level_t level;
    bool        want_color;
    bool        term_supports_color;
    char        prefix[LOG_PREFIX_BUFSZ];
    log_time_cb time_cb;
#if defined(_WIN32)
    HANDLE      hConsole;
    bool        vt_enabled;
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} log_state;

static log_state G;

/* ---- Colors -------------------------------------------------------------- */

static const char* level_name(log_level_t L) {
    switch (L) {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_FATAL: return "FATAL";
        default:        return "?";
    }
}

static const char* level_color(log_level_t L) {
#if defined(LOG_DISABLE_COLOR)
    (void)L; return "";
#else
    switch (L) {
        case LOG_TRACE: return "\x1b[2m";       /* dim */
        case LOG_DEBUG: return "\x1b[36m";      /* cyan */
        case LOG_INFO:  return "\x1b[32m";      /* green */
        case LOG_WARN:  return "\x1b[33m";      /* yellow */
        case LOG_ERROR: return "\x1b[31m";      /* red */
        case LOG_FATAL: return "\x1b[97;41m";   /* white on red */
        default:        return "";
    }
#endif
}

static const char* ansi_reset(void) {
#if defined(LOG_DISABLE_COLOR)
    return "";
#else
    return "\x1b[0m";
#endif
}

/* ---- Time --------------------------------------------------------------- */

static void time_iso8601_default(char* buf, size_t cap) {
#if defined(LOG_DISABLE_TIME)
    if (cap) buf[0] = 0;
    return;
#else
    if (!buf || cap == 0) return;
# if defined(_WIN32)
    SYSTEMTIME st;
#   if defined(LOG_USE_UTC)
    GetSystemTime(&st);
#   else
    GetLocalTime(&st);
#   endif
    /* YYYY-MM-DDThh:mm:ss.mmmZ? */
#   if defined(LOG_USE_UTC)
    _snprintf(buf, cap, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
              (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
              (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
              (unsigned)st.wMilliseconds);
#   else
    _snprintf(buf, cap, "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
              (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
              (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
              (unsigned)st.wMilliseconds);
#   endif
# else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec;
    struct tm tmv;
#   if defined(LOG_USE_UTC)
    gmtime_r(&t, &tmv);
#   else
    localtime_r(&t, &tmv);
#   endif
    int n = (int)strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tmv);
    if (n <= 0) { if (cap) buf[0] = 0; return; }
    if ((size_t)n + 5 < cap) {
        /* append .mmm and optional Z */
        int ms = (int)(tv.tv_usec / 1000);
#       if defined(LOG_USE_UTC)
        snprintf(buf + n, cap - (size_t)n, ".%03dZ", ms);
#       else
        snprintf(buf + n, cap - (size_t)n, ".%03d", ms);
#       endif
    }
# endif
#endif
}

/* ---- TTY color capability ----------------------------------------------- */

static bool detect_tty_color(FILE* fp) {
#if defined(LOG_DISABLE_COLOR)
    (void)fp; return false;
#else
    if (!fp) return false;
    if (!isatty(fileno(fp))) return false;
#if defined(_WIN32)
    /* Try enabling Virtual Terminal sequences */
    HANDLE h = (HANDLE)_get_osfhandle(fileno(fp));
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
        if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            return false;
    }
    return true;
#else
    return true; /* assume ANSI */
#endif
#endif
}

/* ---- Locking ------------------------------------------------------------- */

static void lock_init(void) {
#if defined(_WIN32)
    InitializeCriticalSection(&G.lock);
#else
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&G.lock, &a);
    pthread_mutexattr_destroy(&a);
#endif
}
static void lock_enter(void) {
#if defined(_WIN32)
    EnterCriticalSection(&G.lock);
#else
    pthread_mutex_lock(&G.lock);
#endif
}
static void lock_leave(void) {
#if defined(_WIN32)
    LeaveCriticalSection(&G.lock);
#else
    pthread_mutex_unlock(&G.lock);
#endif
}

/* ---- State --------------------------------------------------------------- */

static void safe_strcpy(char* dst, size_t cap, const char* s) {
    if (!dst || !cap) return;
    if (!s) { dst[0] = 0; return; }
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

void log_init(log_level_t level) {
    memset(&G, 0, sizeof(G));
    G.fp = stderr;
    G.level = level;
    G.want_color = true;
    G.term_supports_color = detect_tty_color(G.fp);
    G.time_cb = &time_iso8601_default;
#if defined(_WIN32)
    G.hConsole = (HANDLE)_get_osfhandle(fileno(G.fp));
#endif
    lock_init();
}

void log_set_level(log_level_t level) { G.level = level; }

void log_set_use_color(bool enable) {
#if defined(LOG_DISABLE_COLOR)
    (void)enable;
    G.want_color = false;
    G.term_supports_color = false;
#else
    G.want_color = enable;
    G.term_supports_color = enable && detect_tty_color(G.fp);
#endif
}

void log_set_output(FILE* fp) {
    if (!fp) return;
    lock_enter();
    G.fp = fp;
    G.term_supports_color = G.want_color && detect_tty_color(G.fp);
#if defined(_WIN32)
    G.hConsole = (HANDLE)_get_osfhandle(fileno(G.fp));
#endif
    lock_leave();
}

void log_set_time_cb(log_time_cb cb) { G.time_cb = cb ? cb : time_iso8601_default; }

void log_set_prefix(const char* prefix) { safe_strcpy(G.prefix, sizeof(G.prefix), prefix); }

/* ---- Core print ---------------------------------------------------------- */

static void write_line(log_level_t L, const char* msg) {
    /* Single locked write to avoid interleaving */
    lock_enter();
    fputs(msg, G.fp);
    fputc('\n', G.fp);
    fflush(G.fp);
    lock_leave();
}

void log_vlog(log_level_t level, const char* fmt, va_list ap) {
    if (level < G.level) return;

    char tbuf[LOG_TIME_BUFSZ]; tbuf[0] = 0;
    if (G.time_cb) G.time_cb(tbuf, sizeof tbuf);

    char body[LOG_LINE_BUFSZ];
    va_list aq; va_copy(aq, ap);
    int n = vsnprintf(body, sizeof body, fmt, aq);
    va_end(aq);
    if (n < 0) { body[0] = 0; }

    char final[LOG_LINE_BUFSZ + 96];
    const bool use_color = G.want_color && G.term_supports_color;

    if (use_color) {
        snprintf(final, sizeof final, "%s%s%s%s%s%s%s %s",
                 G.prefix[0] ? "[" : "",
                 G.prefix[0] ? G.prefix : "",
                 G.prefix[0] ? "] " : "",
#if defined(LOG_DISABLE_TIME)
                 "",
#else
                 (tbuf[0] ? tbuf : ""),
#endif
#if defined(LOG_DISABLE_TIME)
                 "",
#else
                 (tbuf[0] ? " " : ""),
#endif
                 level_color(level), level_name(level), ansi_reset());
    } else {
        snprintf(final, sizeof final, "%s%s%s%s%s%s",
                 G.prefix[0] ? "[" : "",
                 G.prefix[0] ? G.prefix : "",
                 G.prefix[0] ? "] " : "",
#if defined(LOG_DISABLE_TIME)
                 "",
#else
                 (tbuf[0] ? tbuf : ""),
#endif
#if defined(LOG_DISABLE_TIME)
                 "",
#else
                 (tbuf[0] ? " " : ""),
#endif
                 level_name(level));
    }

    size_t len = strlen(final);
    if (len + 2 < sizeof(final)) { final[len++] = ':'; final[len++] = ' '; final[len] = 0; }

    size_t cap = sizeof(final) - len - 1;
    strncat(final, body, cap);

    write_line(level, final);
    if (level == LOG_FATAL) {
        /* flush aggressively */
        fflush(G.fp);
    }
}

void log_log(log_level_t level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vlog(level, fmt, ap);
    va_end(ap);
}

/* ---- Convenience --------------------------------------------------------- */

#ifndef LOG_NO_TRACE
void log_trace(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_vlog(LOG_TRACE, fmt, ap); va_end(ap);
}
#endif

void log_debug(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_vlog(LOG_DEBUG, fmt, ap); va_end(ap);
}
void log_info(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_vlog(LOG_INFO, fmt, ap); va_end(ap);
}
void log_warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_vlog(LOG_WARN, fmt, ap); va_end(ap);
}
void log_error(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_vlog(LOG_ERROR, fmt, ap); va_end(ap);
}
void log_fatal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); log_vlog(LOG_FATAL, fmt, ap); va_end(ap);
}

/* ---- Default init at first call (optional) ------------------------------ */

__attribute__((constructor))
static void log_ctor(void) {
    /* Safe on MSVC too: ignored attribute; call at first use if not supported */
    if (!G.fp) log_init(LOG_DEFAULT_LEVEL);
}

#if defined(_WIN32) && defined(_MSC_VER)
/* MSVC lacks constructor attribute; fallback: ensure link reference */
#pragma section(".CRT$XCU",read)
static void __cdecl log_msvc_ctor(void) { if (!G.fp) log_init(LOG_DEFAULT_LEVEL); }
__declspec(allocate(".CRT$XCU")) void (*log_msvc_ctor_)(void) = log_msvc_ctor;
#endif

/* ===== Optional self-test ================================================= */
#ifdef LOG_MAIN
int main(void) {
    log_init(LOG_TRACE);
    log_set_prefix("vitte");
    log_trace("trace %d", 1);
    log_debug("debug %s", "x");
    log_info ("info");
    log_warn ("warn");
    log_error("error code=%d", 42);
    log_set_use_color(false);
    log_info("no color");
    return 0;
}
#endif
