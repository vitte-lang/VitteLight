/* ============================================================================
   debug.c — utilitaires de debug/logging ultra complets (C17, cross-platform)
   - Niveaux: TRACE, DEBUG, INFO, WARN, ERROR, FATAL
   - Formats: texte (coloré) ou JSON ligne par ligne
   - Sorties: stderr par défaut, fichier avec rotation par taille
   - Thread-safe, horodatage local, TID, source (file:line:func)
   - Hexdump, backtrace (POSIX/Windows), capture signaux/SEH
   - Windows: activation VT100, DbgHelp pour symboles
   - Intégration: inclure “debug.h” si disponible, sinon prototypes ci-dessous.
   Licence: MIT
   ============================================================================
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN 1
#include <DbgHelp.h> /* link: Dbghelp.lib */
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#pragma comment(lib, "Dbghelp.lib")
#else
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

/* ----------------------------------------------------------------------------
   API publique (si debug.h absent)
---------------------------------------------------------------------------- */
#ifndef VT_DEBUG_HAVE_HEADER
#ifndef VT_DEBUG_API
#define VT_DEBUG_API extern
#endif
typedef enum {
  VT_LL_TRACE = 0,
  VT_LL_DEBUG,
  VT_LL_INFO,
  VT_LL_WARN,
  VT_LL_ERROR,
  VT_LL_FATAL
} vt_log_level;

typedef enum { VT_FMT_TEXT = 0, VT_FMT_JSON = 1 } vt_log_format;

typedef struct {
  vt_log_level level;    /* niveau minimal */
  vt_log_format format;  /* texte ou JSON */
  int use_color;         /* 1=colors si TTY */
  const char* file_path; /* NULL=stderr */
  size_t rotate_bytes;   /* 0=pas de rotation */
  int capture_crash;     /* installe handlers */
} vt_log_config;

VT_DEBUG_API int vt_log_init(const vt_log_config* cfg);
VT_DEBUG_API void vt_log_shutdown(void);
VT_DEBUG_API void vt_log_set_level(vt_log_level lvl);
VT_DEBUG_API vt_log_level vt_log_get_level(void);
VT_DEBUG_API void vt_log_set_format(vt_log_format fmt);
VT_DEBUG_API void vt_log_enable_color(int on);
VT_DEBUG_API void vt_log_force_flush(void);
VT_DEBUG_API void vt_log_set_file(const char* path, size_t rotate_bytes);

VT_DEBUG_API void vt_log_write(vt_log_level lvl, const char* file, int line,
                               const char* func, const char* fmt, ...);

VT_DEBUG_API void vt_debug_hexdump(const void* data, size_t len,
                                   const char* label);
VT_DEBUG_API void vt_debug_backtrace(void);
VT_DEBUG_API void vt_debug_install_crash_handlers(void);
#endif

/* ----------------------------------------------------------------------------
   Config interne et synchronisation
---------------------------------------------------------------------------- */
typedef struct {
  vt_log_level level;
  vt_log_format format;
  int color_enabled; /* demandé par l’utilisateur */
  int color_active;  /* effectif (TTY/VT ok) */
  FILE* out;
  char file_path[1024];
  size_t rotate_bytes;
  size_t written_bytes;

#if defined(_WIN32)
  CRITICAL_SECTION lock;
  HANDLE hErr;
  int vt_enabled;
#else
  pthread_mutex_t lock;
#endif
} vt_log_state;

static vt_log_state g_log;

static int vt__isatty(void) {
#if defined(_WIN32)
  return _isatty(_fileno(g_log.out ? g_log.out : stderr));
#else
  return isatty(fileno(g_log.out ? g_log.out : stderr));
#endif
}

static void vt__lock(void) {
#if defined(_WIN32)
  EnterCriticalSection(&g_log.lock);
#else
  pthread_mutex_lock(&g_log.lock);
#endif
}
static void vt__unlock(void) {
#if defined(_WIN32)
  LeaveCriticalSection(&g_log.lock);
#else
  pthread_mutex_unlock(&g_log.lock);
#endif
}

/* ----------------------------------------------------------------------------
   Windows: activer VT100 + file handle
---------------------------------------------------------------------------- */
#if defined(_WIN32)
static void vt__enable_vt100(void) {
  DWORD mode = 0;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (h == INVALID_HANDLE_VALUE || h == NULL) return;
  if (!GetConsoleMode(h, &mode)) return;
  mode |= 0x0004; /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
  if (SetConsoleMode(h, mode)) g_log.vt_enabled = 1;
}
#endif

/* ----------------------------------------------------------------------------
   Noms niveaux + couleurs
---------------------------------------------------------------------------- */
static const char* vt__lvl_name(vt_log_level l) {
  switch (l) {
    case VT_LL_TRACE:
      return "TRACE";
    case VT_LL_DEBUG:
      return "DEBUG";
    case VT_LL_INFO:
      return "INFO";
    case VT_LL_WARN:
      return "WARN";
    case VT_LL_ERROR:
      return "ERROR";
    default:
      return "FATAL";
  }
}
static const char* vt__lvl_color(vt_log_level l) {
  /* CSI */
  switch (l) {
    case VT_LL_TRACE:
      return "\x1b[90m"; /* bright black */
    case VT_LL_DEBUG:
      return "\x1b[36m"; /* cyan */
    case VT_LL_INFO:
      return "\x1b[32m"; /* green */
    case VT_LL_WARN:
      return "\x1b[33m"; /* yellow */
    case VT_LL_ERROR:
      return "\x1b[31m"; /* red */
    default:
      return "\x1b[41;97m"; /* red bg, white fg */
  }
}

/* ----------------------------------------------------------------------------
   Horodatage local "YYYY-MM-DD HH:MM:SS.mmm"
---------------------------------------------------------------------------- */
static void vt__fmt_timestamp(char* dst, size_t cap) {
  struct timespec ts;
#if defined(_WIN32)
  /* Windows 10+ : timespec via _timespec64 ? Simple: GetSystemTime +
   * milliseconds */
  SYSTEMTIME st;
  GetLocalTime(&st);
  snprintf(dst, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u", (unsigned)st.wYear,
           (unsigned)st.wMonth, (unsigned)st.wDay, (unsigned)st.wHour,
           (unsigned)st.wMinute, (unsigned)st.wSecond,
           (unsigned)st.wMilliseconds);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmval;
  localtime_r(&ts.tv_sec, &tmval);
  int ms = (int)(ts.tv_nsec / 1000000);
  snprintf(dst, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d", 1900 + tmval.tm_year,
           1 + tmval.tm_mon, tmval.tm_mday, tmval.tm_hour, tmval.tm_min,
           tmval.tm_sec, ms);
#endif
}

/* ----------------------------------------------------------------------------
   Thread ID
---------------------------------------------------------------------------- */
static unsigned long long vt__tid(void) {
#if defined(_WIN32)
  return (unsigned long long)GetCurrentThreadId();
#elif defined(__APPLE__)
  uint64_t tid;
  pthread_threadid_np(NULL, &tid);
  return (unsigned long long)tid;
#else
  return (unsigned long long)pthread_self();
#endif
}

/* ----------------------------------------------------------------------------
   Rotation fichier
---------------------------------------------------------------------------- */
static int vt__ensure_rotation_unlocked(size_t next_write) {
  if (!g_log.out || g_log.out == stderr) return 0;
  if (g_log.rotate_bytes == 0) return 0;
  if (g_log.written_bytes + next_write < g_log.rotate_bytes) return 0;

  /* Fermer et renommer: <file>.1 (écrase) */
  fclose(g_log.out);
  g_log.out = NULL;
  char bak[1200] = {0};
  snprintf(bak, sizeof(bak), "%s.1", g_log.file_path);
#if defined(_WIN32)
  MoveFileExA(g_log.file_path, bak, MOVEFILE_REPLACE_EXISTING);
#else
  rename(g_log.file_path, bak);
#endif
  g_log.out = fopen(g_log.file_path, "ab");
  g_log.written_bytes = 0;
  if (!g_log.out) {
    g_log.out = stderr;
    return -1;
  }
  return 0;
}

/* ----------------------------------------------------------------------------
   Init / Shutdown
---------------------------------------------------------------------------- */
int vt_log_init(const vt_log_config* cfg) {
  memset(&g_log, 0, sizeof(g_log));
  g_log.level = cfg ? cfg->level : VT_LL_INFO;
  g_log.format = cfg ? cfg->format : VT_FMT_TEXT;
  g_log.color_enabled = cfg ? cfg->use_color : 1;
  g_log.rotate_bytes = cfg ? cfg->rotate_bytes : 0;
  if (cfg && cfg->file_path) {
    strncpy(g_log.file_path, cfg->file_path, sizeof(g_log.file_path) - 1);
    g_log.out = fopen(g_log.file_path, "ab");
    if (!g_log.out) g_log.out = stderr;
  } else {
    g_log.out = stderr;
  }

#if defined(_WIN32)
  InitializeCriticalSection(&g_log.lock);
  g_log.hErr = GetStdHandle(STD_ERROR_HANDLE);
  vt__enable_vt100();
#else
  pthread_mutexattr_t at;
  pthread_mutexattr_init(&at);
  pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_log.lock, &at);
  pthread_mutexattr_destroy(&at);
#endif

  g_log.color_active = g_log.color_enabled && vt__isatty();
  if (cfg && cfg->capture_crash) vt_debug_install_crash_handlers();
  return 0;
}

void vt_log_shutdown(void) {
  vt__lock();
  FILE* f = g_log.out;
  if (f && f != stderr) fclose(f);
  g_log.out = stderr;
  g_log.written_bytes = 0;
  vt__unlock();
#if defined(_WIN32)
  DeleteCriticalSection(&g_log.lock);
#else
  pthread_mutex_destroy(&g_log.lock);
#endif
}

void vt_log_set_level(vt_log_level lvl) {
  vt__lock();
  g_log.level = lvl;
  vt__unlock();
}
vt_log_level vt_log_get_level(void) { return g_log.level; }
void vt_log_set_format(vt_log_format fmt) {
  vt__lock();
  g_log.format = fmt;
  vt__unlock();
}
void vt_log_enable_color(int on) {
  vt__lock();
  g_log.color_enabled = on ? 1 : 0;
  g_log.color_active = g_log.color_enabled && vt__isatty();
  vt__unlock();
}
void vt_log_force_flush(void) {
  vt__lock();
  if (g_log.out) fflush(g_log.out);
  vt__unlock();
}
void vt_log_set_file(const char* path, size_t rotate_bytes) {
  vt__lock();
  if (g_log.out && g_log.out != stderr) fclose(g_log.out);
  g_log.rotate_bytes = rotate_bytes;
  g_log.written_bytes = 0;
  if (path) {
    strncpy(g_log.file_path, path, sizeof(g_log.file_path) - 1);
    g_log.out = fopen(g_log.file_path, "ab");
    if (!g_log.out) g_log.out = stderr;
  } else {
    g_log.file_path[0] = '\0';
    g_log.out = stderr;
  }
  vt__unlock();
}

/* ----------------------------------------------------------------------------
   Echappement JSON minimal
---------------------------------------------------------------------------- */
static void vt__json_escape(const char* s, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; s[i] && o + 6 < cap; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = c;
    } else if (c >= 0x20 && c != 0x7F) {
      out[o++] = c;
    } else {
      /* \u00XX */
      int n = snprintf(out + o, cap - o, "\\u%04X", (unsigned)c);
      o += (n > 0) ? (size_t)n : 0;
    }
  }
  out[o] = 0;
}

/* ----------------------------------------------------------------------------
   Ecriture d’un message
---------------------------------------------------------------------------- */
void vt_log_write(vt_log_level lvl, const char* file, int line,
                  const char* func, const char* fmt, ...) {
  if (lvl < g_log.level) return;

  char ts[48];
  vt__fmt_timestamp(ts, sizeof ts);
  unsigned long long tid = vt__tid();

  char msgbuf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
  va_end(ap);

  vt__lock();

  /* Rotation éventuelle (approx: longueur estimée) */
  size_t approx = strlen(msgbuf) + 128;
  vt__ensure_rotation_unlocked(approx);

  if (g_log.format == VT_FMT_JSON) {
    char fbuf[1024];
    char funcbuf[1024];
    char jmsg[4096];
    vt__json_escape(file ? file : "", fbuf, sizeof fbuf);
    vt__json_escape(func ? func : "", funcbuf, sizeof funcbuf);
    vt__json_escape(msgbuf, jmsg, sizeof jmsg);
    int n =
        fprintf(g_log.out,
                "{\"ts\":\"%s\",\"lvl\":\"%s\",\"tid\":%llu,"
                "\"file\":\"%s\",\"line\":%d,\"func\":\"%s\",\"msg\":\"%s\"}\n",
                ts, vt__lvl_name(lvl), (unsigned long long)tid, fbuf, line,
                funcbuf, jmsg);
    if (n > 0) g_log.written_bytes += (size_t)n;
  } else {
    if (g_log.color_active) {
      int n = fprintf(g_log.out, "%s%s\x1b[0m %s | %llu | %s:%d:%s | %s\n",
                      vt__lvl_color(lvl), vt__lvl_name(lvl), ts,
                      (unsigned long long)tid, file ? file : "", line,
                      func ? func : "", msgbuf);
      if (n > 0) g_log.written_bytes += (size_t)n;
    } else {
      int n = fprintf(g_log.out, "%s %s | %llu | %s:%d:%s | %s\n",
                      vt__lvl_name(lvl), ts, (unsigned long long)tid,
                      file ? file : "", line, func ? func : "", msgbuf);
      if (n > 0) g_log.written_bytes += (size_t)n;
    }
  }
  fflush(g_log.out);

  if (lvl == VT_LL_FATAL) {
    vt__unlock();
    vt_debug_backtrace();
    /* flush then abort */
    vt__lock();
    fflush(g_log.out);
    vt__unlock();
    abort();
  }

  vt__unlock();
}

VT_DEBUG_API void vt_assert_fail(const char* cond, const char* file, int line,
                                 const char* func, const char* fmt, ...) {
  char msgbuf[4096];
  size_t off = 0;
  if (cond && *cond) {
    int n = snprintf(msgbuf, sizeof msgbuf, "assertion failed: %s", cond);
    off = (n > 0) ? (size_t)n : 0;
  } else {
    int n = snprintf(msgbuf, sizeof msgbuf, "assertion failed");
    off = (n > 0) ? (size_t)n : 0;
  }

  if (off >= sizeof msgbuf) off = sizeof msgbuf - 1;

  if (fmt && *fmt) {
    if (off + 2 < sizeof msgbuf) {
      msgbuf[off++] = ':';
      msgbuf[off++] = ' ';
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgbuf + off, sizeof msgbuf - off, fmt, ap);
    va_end(ap);
  } else {
    msgbuf[off] = '\0';
  }

  vt_log_write(VT_LL_FATAL, file, line, func, "%s", msgbuf);
}

/* ----------------------------------------------------------------------------
   Hexdump
---------------------------------------------------------------------------- */
void vt_debug_hexdump(const void* data, size_t len, const char* label) {
  const unsigned char* p = (const unsigned char*)data;
  vt__lock();
  if (label)
    vt_log_write(VT_LL_DEBUG, __FILE__, __LINE__, __func__,
                 "[hexdump] %s (%zu bytes)", label, len);
  for (size_t i = 0; i < len; i += 16) {
    char ascii[17];
    ascii[16] = 0;
    fprintf(g_log.out, "  %08zx  ", i);
    for (size_t j = 0; j < 16; j++) {
      if (i + j < len) {
        fprintf(g_log.out, "%02x ", p[i + j]);
        ascii[j] = (p[i + j] >= 32 && p[i + j] < 127) ? (char)p[i + j] : '.';
      } else {
        fputs("   ", g_log.out);
        ascii[j] = ' ';
      }
      if ((j & 7) == 7) fputc(' ', g_log.out);
    }
    fprintf(g_log.out, " |%s|\n", ascii);
  }
  fflush(g_log.out);
  vt__unlock();
}

/* ----------------------------------------------------------------------------
   Backtrace
---------------------------------------------------------------------------- */
#if defined(_WIN32)
/* Windows: symbolisation via DbgHelp */
static void vt__sym_init(void) {
  static int inited = 0;
  if (inited) return;
  HANDLE h = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  (void)SymInitialize(h, NULL, TRUE);
  inited = 1;
}
#endif

void vt_debug_backtrace(void) {
#if defined(_WIN32)
  void* frames[64] = {0};
  USHORT n = CaptureStackBackTrace(0, 64, frames, NULL);
  vt__lock();
  vt__sym_init();
  HANDLE proc = GetCurrentProcess();
  char symbuf[sizeof(SYMBOL_INFO) + 512];
  PSYMBOL_INFO sym = (PSYMBOL_INFO)symbuf;
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 512;
  fprintf(g_log.out, "Backtrace (%u frames):\n", (unsigned)n);
  for (USHORT i = 0; i < n; i++) {
    DWORD64 addr = (DWORD64)(frames[i]);
    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, sym)) {
      fprintf(g_log.out, "  #%02u  %p  %s +0x%llx\n", (unsigned)i, (void*)addr,
              sym->Name, (unsigned long long)disp);
    } else {
      fprintf(g_log.out, "  #%02u  %p  <no symbol>\n", (unsigned)i,
              (void*)addr);
    }
  }
  fflush(g_log.out);
  vt__unlock();
#else
  void* buf[64];
  int n = backtrace(buf, 64);
  char** syms = backtrace_symbols(buf, n);
  vt__lock();
  fprintf(g_log.out, "Backtrace (%d frames):\n", n);
  for (int i = 0; i < n; i++) {
    fprintf(g_log.out, "  #%02d %s\n", i, syms ? syms[i] : "<no symbol>");
  }
  fflush(g_log.out);
  vt__unlock();
  free(syms);
#endif
}

/* ----------------------------------------------------------------------------
   Crash handlers (SIGSEGV etc. / SEH)
---------------------------------------------------------------------------- */
#if !defined(_WIN32)
static void vt__sig_handler(int sig, siginfo_t* info, void* uctx) {
  (void)info;
  (void)uctx;
  vt_log_write(VT_LL_FATAL, __FILE__, __LINE__, __func__, "Signal %d reçu",
               sig);
}
#endif

void vt_debug_install_crash_handlers(void) {
#if defined(_WIN32)
  /* Minimal SEH: print backtrace on unhandled exception */
  /* Note: pour un handling robuste, préférer un filter séparé dans l’app. */
  static LONG(__stdcall * prev)(EXCEPTION_POINTERS*) = NULL;
  (void)prev;
  /* Pas de remplacement global ici pour éviter effets de bord.
     L’app peut installer son propre filter et appeler vt_debug_backtrace(). */
#else
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = vt__sig_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
#ifdef SIGBUS
  sigaction(SIGBUS, &sa, NULL);
#endif
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
#endif
}

/* ----------------------------------------------------------------------------
   Exemple d’utilisation (désactivé par défaut):
   gcc -DVT_DEBUG_TEST debug.c -ldl -pthread
---------------------------------------------------------------------------- */
#ifdef VT_DEBUG_TEST
int main(void) {
  vt_log_config cfg = {.level = VT_LL_TRACE,
                       .format = VT_FMT_TEXT,
                       .use_color = 1,
                       .file_path = NULL,
                       .rotate_bytes = 0,
                       .capture_crash = 1};
  vt_log_init(&cfg);
  vt_log_write(VT_LL_INFO, __FILE__, __LINE__, __func__, "Hello %s", "world");
  vt_log_write(VT_LL_WARN, __FILE__, __LINE__, __func__, "Warn: x=%d", 42);
  vt_debug_hexdump("ABCDEFG", 7, "sample");
  vt_debug_backtrace();
  vt_log_shutdown();
  return 0;
}
#endif
