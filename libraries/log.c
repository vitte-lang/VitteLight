// SPDX-License-Identifier: GPL-3.0-or-later
//
// log.c — Logger minimal et robuste pour Vitte Light (C17, portable)
// Namespace: "log"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c log.c
//
// Caractéristiques:
//   - Niveaux: TRACE, DEBUG, INFO, WARN, ERROR, FATAL.
//   - Sortie: FILE* arbitraire (stderr par défaut) ou callback utilisateur.
//   - Format: horodatage ISO-8601, niveau, thread id, tag, message.
//   - Rotation: taille max + backups N (log.txt → log.txt.1 …).
//   - Hexdump utilitaire.
//   - Couleurs ANSI optionnelles.
//   - Fil-safe avec spinlock stdatomic (C11+), sans dépendances externes.
//
// API (déclarations incluses ici pour usage direct) :
//   enum log_level { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };
//   typedef void (*log_callback_t)(enum log_level lvl, const char* tag,
//                                  const char* iso_ts, const char* msg, void* user);
//   void  log_set_level(enum log_level lvl);
//   void  log_set_tag(const char* tag);                 // tag global optionnel
//   void  log_set_fp(FILE* fp);                          // sortie fichier
//   bool  log_open_file(const char* path);              // ouvre et bascule la sortie
//   void  log_set_callback(log_callback_t cb, void* u); // remplace l’output par callback
//   void  log_use_colors(bool on);
//   void  log_set_rotate(size_t max_bytes, int backups); // 0 désactive
//   void  log_write(enum log_level lvl, const char* tag, const char* fmt, ...);
//   void  log_hexdump(enum log_level lvl, const char* tag, const void* data, size_t len);
//   // Macros pratiques:
//   #define LOGT(...) log_write(LOG_TRACE, NULL, __VA_ARGS__)
//   #define LOGD(...) log_write(LOG_DEBUG, NULL, __VA_ARGS__)
//   #define LOGI(...) log_write(LOG_INFO,  NULL, __VA_ARGS__)
//   #define LOGW(...) log_write(LOG_WARN,  NULL, __VA_ARGS__)
//   #define LOGE(...) log_write(LOG_ERROR, NULL, __VA_ARGS__)
//   #define LOGF(...) log_write(LOG_FATAL, NULL, __VA_ARGS__)
//
// Notes:
//   - Toutes les fonctions sont non bloquantes hors I/O disque.
//   - Rotation best-effort. Ignorée si sortie != fichier normal.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>

#if defined(__has_include)
#  if __has_include(<threads.h>)
#    include <threads.h>
#    define VL_HAVE_THREADS_H 1
#  endif
#endif
#ifndef VL_HAVE_THREADS_H
#  include <pthread.h>
#endif

#include <stdatomic.h>
#include <sys/stat.h>

#ifndef LOG_MAX_LINE
#define LOG_MAX_LINE 4096
#endif

enum log_level { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };
typedef void (*log_callback_t)(enum log_level lvl, const char* tag,
                               const char* iso_ts, const char* msg, void* user);

typedef struct {
    FILE* fp;
    enum log_level level;
    bool use_colors;
    char tag[64];
    // rotation
    size_t rotate_max_bytes;
    int    rotate_backups;
    char   path[512];
    // callback
    log_callback_t cb;
    void* cb_user;
    // lock
    atomic_flag lock;
} log_state_t;

static log_state_t G = {
    .fp = NULL,
    .level = LOG_INFO,
    .use_colors = false,
    .tag = {0},
    .rotate_max_bytes = 0,
    .rotate_backups = 0,
    .path = {0},
    .cb = NULL,
    .cb_user = NULL,
    .lock = ATOMIC_FLAG_INIT
};

#define LOCK()   while (atomic_flag_test_and_set_explicit(&G.lock, memory_order_acquire)) { /* spin */ }
#define UNLOCK() atomic_flag_clear_explicit(&G.lock, memory_order_release)

static const char* lvl_name(enum log_level lvl) {
    switch (lvl) {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_FATAL: return "FATAL";
        default:        return "UNKNOWN";
    }
}
static const char* lvl_color(enum log_level lvl) {
    switch (lvl) {
        case LOG_TRACE: return "\x1b[90m"; // bright black
        case LOG_DEBUG: return "\x1b[36m"; // cyan
        case LOG_INFO:  return "\x1b[32m"; // green
        case LOG_WARN:  return "\x1b[33m"; // yellow
        case LOG_ERROR: return "\x1b[31m"; // red
        case LOG_FATAL: return "\x1b[35m"; // magenta
        default:        return "\x1b[0m";
    }
}

static void now_iso8601(char out[32]) {
    // YYYY-MM-DDTHH:MM:SSZ (local time with offset not shown, keep Z-like)
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    // Seconds only. Keep fixed width.
    snprintf(out, 32, "%04d-%02d-%02dT%02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static unsigned long thread_id_u() {
#if defined(VL_HAVE_THREADS_H)
    uintptr_t v = (uintptr_t) thrd_current();
#else
    uintptr_t v = (uintptr_t) pthread_self();
#endif
    // simple mix
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (unsigned long) v;
}

void log_set_level(enum log_level lvl) {
    if (lvl < LOG_TRACE) lvl = LOG_TRACE;
    if (lvl > LOG_FATAL) lvl = LOG_FATAL;
    LOCK(); G.level = lvl; UNLOCK();
}
void log_set_tag(const char* tag) {
    LOCK();
    if (tag) {
        strncpy(G.tag, tag, sizeof(G.tag)-1);
        G.tag[sizeof(G.tag)-1] = '\0';
    } else {
        G.tag[0] = '\0';
    }
    UNLOCK();
}
void log_set_fp(FILE* fp) {
    LOCK(); G.fp = fp; UNLOCK();
}
bool log_open_file(const char* path) {
    if (!path) return false;
    FILE* f = fopen(path, "ab");
    if (!f) return false;
    LOCK();
    if (G.fp && G.fp != stderr && G.fp != stdout) fclose(G.fp);
    G.fp = f;
    strncpy(G.path, path, sizeof(G.path)-1);
    G.path[sizeof(G.path)-1] = '\0';
    UNLOCK();
    return true;
}
void log_set_callback(log_callback_t cb, void* user) {
    LOCK(); G.cb = cb; G.cb_user = user; UNLOCK();
}
void log_use_colors(bool on) {
    LOCK(); G.use_colors = on; UNLOCK();
}
void log_set_rotate(size_t max_bytes, int backups) {
    LOCK();
    G.rotate_max_bytes = max_bytes;
    G.rotate_backups   = backups < 0 ? 0 : backups;
    UNLOCK();
}

static size_t file_size(FILE* fp) {
    long cur = ftell(fp);
    if (cur < 0) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) return 0;
    long end = ftell(fp);
    if (end < 0) end = 0;
    (void)fseek(fp, cur, SEEK_SET);
    return (size_t) end;
}

static void rotate_once(const char* base, int n) {
    // base.(n-1) -> base.n ; base -> base.1
    char dst[768], src[768];
    if (n <= 1) {
        snprintf(dst, sizeof(dst), "%s.1", base);
        // rename base -> base.1
        rename(base, dst);
        return;
    }
    // shift chain up to n
    for (int i = n; i >= 2; --i) {
        snprintf(src, sizeof(src), "%s.%d", base, i-1);
        snprintf(dst, sizeof(dst), "%s.%d", base, i);
        // ignore errors
        rename(src, dst);
    }
    // finally base -> base.1
    snprintf(dst, sizeof(dst), "%s.1", base);
    rename(base, dst);
}

static void maybe_rotate_unlocked() {
    if (!G.fp) return;
    if (G.path[0] == '\0') return;               // pas un fichier géré
    if (G.rotate_max_bytes == 0) return;
    size_t sz = file_size(G.fp);
    if (sz < G.rotate_max_bytes) return;

    // Close then rotate then reopen append.
    FILE* old = G.fp;
    G.fp = NULL;
    fclose(old);

    if (G.rotate_backups > 0) {
        rotate_once(G.path, G.rotate_backups);
    } else {
        // simple truncate by rename→reopen to .1 optional
        rotate_once(G.path, 1);
    }

    FILE* f = fopen(G.path, "wb"); // truncate
    if (f) fclose(f);
    G.fp = fopen(G.path, "ab");
    if (!G.fp) {
        // fallback stderr
        G.fp = stderr;
    }
}

static void emit_line_unlocked(enum log_level lvl, const char* tag,
                               const char* iso_ts, const char* msg)
{
    if (G.cb) {
        G.cb(lvl, tag, iso_ts, msg, G.cb_user);
        return;
    }
    FILE* out = G.fp ? G.fp : stderr;

    if (G.use_colors && out == stderr) {
        fprintf(out, "%s", lvl_color(lvl));
    }
    if (tag && tag[0]) {
        fprintf(out, "%s [%s] %-5s tid=%lu | %s\n",
                iso_ts, tag, lvl_name(lvl), thread_id_u(), msg);
    } else if (G.tag[0]) {
        fprintf(out, "%s [%s] %-5s tid=%lu | %s\n",
                iso_ts, G.tag, lvl_name(lvl), thread_id_u(), msg);
    } else {
        fprintf(out, "%s %-5s tid=%lu | %s\n",
                iso_ts, lvl_name(lvl), thread_id_u(), msg);
    }
    if (G.use_colors && out == stderr) {
        fputs("\x1b[0m", out);
    }
    fflush(out);
}

void log_write(enum log_level lvl, const char* tag, const char* fmt, ...) {
    if (lvl < G.level) return;

    char msg[LOG_MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char ts[32];
    now_iso8601(ts);

    LOCK();
    maybe_rotate_unlocked();
    emit_line_unlocked(lvl, tag, ts, msg);
    UNLOCK();

    if (lvl == LOG_FATAL) {
        // Pas d'abort ici. Laisser l’hôte décider.
    }
}

void log_hexdump(enum log_level lvl, const char* tag, const void* data, size_t len) {
    if (lvl < G.level) return;
    const unsigned char* p = (const unsigned char*) data;
    char line[96];
    char ts[32];
    now_iso8601(ts);

    LOCK();
    maybe_rotate_unlocked();
    for (size_t off = 0; off < len; off += 16) {
        size_t n = (len - off) < 16 ? (len - off) : 16;
        // hex
        char hex[16*3 + 1]; hex[0] = '\0';
        for (size_t i = 0; i < n; ++i) {
            char b[4];
            snprintf(b, sizeof(b), "%02X ", p[off + i]);
            strncat(hex, b, sizeof(hex)-strlen(hex)-1);
        }
        // ascii
        char asc[17];
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = p[off + i];
            asc[i] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        asc[n] = '\0';
        snprintf(line, sizeof(line), "%08zx  %-48s  |%s|", off, hex, asc);
        emit_line_unlocked(lvl, tag, ts, line);
    }
    UNLOCK();
}

// Macros pratiques (répétées ici pour inclusion unique)
#ifndef LOG_MACROS_DEFINED
#define LOG_MACROS_DEFINED
#define LOGT(...) log_write(LOG_TRACE, NULL, __VA_ARGS__)
#define LOGD(...) log_write(LOG_DEBUG, NULL, __VA_ARGS__)
#define LOGI(...) log_write(LOG_INFO,  NULL, __VA_ARGS__)
#define LOGW(...) log_write(LOG_WARN,  NULL, __VA_ARGS__)
#define LOGE(...) log_write(LOG_ERROR, NULL, __VA_ARGS__)
#define LOGF(...) log_write(LOG_FATAL, NULL, __VA_ARGS__)
#endif

// --- Mini démonstration facultative ---
#ifdef LOG_DEMO
int main(void) {
    log_use_colors(true);
    log_set_level(LOG_TRACE);
    LOGI("Démarrage logger");
    log_open_file("app.log");
    log_set_rotate(64 * 1024, 3);
    LOGD("Message %d", 1);
    const char buf[] = "Hello\x00World\x7F\x01";
    log_hexdump(LOG_DEBUG, "net", buf, sizeof(buf)-1);
    LOGW("Alerte simple");
    LOGE("Erreur: %s", "exemple");
    LOGF("Fatal simulé");
    return 0;
}
#endif
