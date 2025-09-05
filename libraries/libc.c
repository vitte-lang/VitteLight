// SPDX-License-Identifier: GPL-3.0-or-later
//
// libc.c — Minimal, portable libc bindings for Vitte Light (C17)
// Namespace: "libc"
//
// Functions:
//   libc.getenv(key)                    -> string|nil
//   libc.setenv(key, value, [overwrite=true]) -> bool | (nil, errmsg)
//   libc.unsetenv(key)                  -> bool | (nil, errmsg)
//   libc.errno()                        -> int
//   libc.strerror([err])                -> string
//   libc.getpid()                       -> int
//   libc.sleep_ms(ms)                   -> bool | (nil, errmsg)
//   libc.time()                         -> int64  // seconds since epoch (UTC)
//   libc.clock_mono_ms()                -> int64  // monotonic
//   libc.clock_mono_ns()                -> int64  // monotonic
//   libc.hostname()                     -> string | (nil, errmsg)
//   libc.system(cmd)                    -> int    // process exit status (best
//   effort) libc.rand_bytes(n)                  -> string(len=n) | (nil,
//   errmsg) libc.gmtime_iso([secs])             -> string
//   "YYYY-MM-DDThh:mm:ssZ" libc.localtime_iso([secs])          -> string
//   "YYYY-MM-DDThh:mm:ss"
//
// Depends: includes/auxlib.h, state.h, object.h, vm.h

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#else
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------

static const char *vlc_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}

static int64_t vlc_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isint(S, idx)) return vl_toint(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}

static int vlc_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}

static int64_t vlc_opt_int(VL_State *S, int idx, int64_t defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx)) return vl_toint(S, vl_get(S, idx));
  return defv;
}

// ---------------------------------------------------------------------
// getenv / setenv / unsetenv
// ---------------------------------------------------------------------

static int vlc_getenv(VL_State *S) {
  const char *key = vlc_check_str(S, 1);
#if defined(_WIN32)
  // Use GetEnvironmentVariableA to avoid CRT static buffer edge cases
  DWORD n = GetEnvironmentVariableA(key, NULL, 0);
  if (n == 0) {
    vl_push_nil(S);
    return 1;
  }
  char *buf = (char *)malloc(n);
  if (!buf) {
    vl_push_nil(S);
    return 1;
  }
  DWORD r = GetEnvironmentVariableA(key, buf, n);
  if (r == 0 || r >= n) {
    free(buf);
    vl_push_nil(S);
    return 1;
  }
  vl_push_string(S, buf);
  free(buf);
  return 1;
#else
  const char *v = getenv(key);
  if (!v) {
    vl_push_nil(S);
    return 1;
  }
  vl_push_string(S, v);
  return 1;
#endif
}

static int vlc_setenv(VL_State *S) {
  const char *key = vlc_check_str(S, 1);
  const char *val = vlc_check_str(S, 2);
  int overwrite = vlc_opt_bool(S, 3, 1);
#if defined(_WIN32)
  if (!overwrite) {
    DWORD n = GetEnvironmentVariableA(key, NULL, 0);
    if (n > 0) {
      vl_push_bool(S, 1);
      return 1;
    }
  }
  int rc = _putenv_s(key, val);
  if (rc != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
#else
  if (setenv(key, val, overwrite ? 1 : 0) != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
#endif
}

static int vlc_unsetenv(VL_State *S) {
  const char *key = vlc_check_str(S, 1);
#if defined(_WIN32)
  // Remove by setting empty value
  int rc = _putenv_s(key, "");
  if (rc != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
#else
  if (unsetenv(key) != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
#endif
}

// ---------------------------------------------------------------------
// errno / strerror
// ---------------------------------------------------------------------

static int vlc_errno(VL_State *S) {
  vl_push_int(S, (int64_t)errno);
  return 1;
}

static int vlc_strerror(VL_State *S) {
  int64_t e = vlc_opt_int(S, 1, (int64_t)errno);
#if defined(_WIN32)
  char buf[256];
  strerror_s(buf, sizeof buf, (int)e);
  vl_push_string(S, buf);
#else
  vl_push_string(S, strerror((int)e));
#endif
  return 1;
}

// ---------------------------------------------------------------------
// pid, sleep, time, monotonic clocks
// ---------------------------------------------------------------------

static int vlc_getpid(VL_State *S) {
#if defined(_WIN32)
  vl_push_int(S, (int64_t)GetCurrentProcessId());
#else
  vl_push_int(S, (int64_t)getpid());
#endif
  return 1;
}

static int vlc_sleep_ms(VL_State *S) {
  int64_t ms = vlc_check_int(S, 1);
  if (ms < 0) ms = 0;
#if defined(_WIN32)
  Sleep((DWORD)ms);
  vl_push_bool(S, 1);
  return 1;
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)((ms % 1000) * 1000000L);
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { /* retry */
  }
  vl_push_bool(S, 1);
  return 1;
#endif
}

static int vlc_time(VL_State *S) {
  (void)S;
  vl_push_int(S, (int64_t)time(NULL));
  return 1;
}

static int vlc_clock_mono_ms(VL_State *S) {
  vl_push_int(S, (int64_t)aux_now_millis());
  return 1;
}

static int vlc_clock_mono_ns(VL_State *S) {
  vl_push_int(S, (int64_t)aux_now_nanos());
  return 1;
}

// ---------------------------------------------------------------------
// hostname
// ---------------------------------------------------------------------

static int vlc_hostname(VL_State *S) {
#if defined(_WIN32)
  char buf[256];
  DWORD n = (DWORD)sizeof buf;
  if (!GetComputerNameA(buf, &n)) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, buf);
  return 1;
#else
  char buf[256];
  if (gethostname(buf, sizeof buf) != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  buf[sizeof(buf) - 1] = 0;
  vl_push_string(S, buf);
  return 1;
#endif
}

// ---------------------------------------------------------------------
// system(cmd) -> exit status
// ---------------------------------------------------------------------

static int vlc_system(VL_State *S) {
  const char *cmd = vlc_check_str(S, 1);
  int rc = system(cmd);
#if defined(_WIN32)
  // On Windows, return value is the exit code.
  vl_push_int(S, (int64_t)rc);
#else
  int status = rc;
  int code = -1;
  if (status != -1) {
    if (WIFEXITED(status))
      code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
      code = 128 + WTERMSIG(status);
  }
  vl_push_int(S, (int64_t)code);
#endif
  return 1;
}

// ---------------------------------------------------------------------
// rand_bytes(n) -> string
// ---------------------------------------------------------------------

static int vlc_rand_bytes(VL_State *S) {
  int64_t n = vlc_check_int(S, 1);
  if (n < 0 || n > 128 * 1024 * 1024) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  uint8_t *buf = (uint8_t *)malloc((size_t)n);
  if (!buf) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  AuxStatus st = aux_rand_bytes(buf, (size_t)n);
  if (st != AUX_OK) {
    free(buf);
    vl_push_nil(S);
    vl_push_string(S, aux_status_str(st));
    return 2;
  }
  vl_push_lstring(S, (const char *)buf, (int)n);
  free(buf);
  return 1;
}

// ---------------------------------------------------------------------
// ISO-8601 formatting helpers
// ---------------------------------------------------------------------

static int vlc_gmtime_iso(VL_State *S) {
  time_t t = (time_t)vlc_opt_int(S, 1, (int64_t)time(NULL));
  char buf[32];
  size_t w = aux_time_iso8601(buf, sizeof buf, t, /*utc=*/true);
  if (w == 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, buf);
  return 1;
}

static int vlc_localtime_iso(VL_State *S) {
  time_t t = (time_t)vlc_opt_int(S, 1, (int64_t)time(NULL));
  char buf[32];
  size_t w = aux_time_iso8601(buf, sizeof buf, t, /*utc=*/false);
  if (w == 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, buf);
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------

static const VL_Reg libclib[] = {{"getenv", vlc_getenv},
                                 {"setenv", vlc_setenv},
                                 {"unsetenv", vlc_unsetenv},
                                 {"errno", vlc_errno},
                                 {"strerror", vlc_strerror},
                                 {"getpid", vlc_getpid},
                                 {"sleep_ms", vlc_sleep_ms},
                                 {"time", vlc_time},
                                 {"clock_mono_ms", vlc_clock_mono_ms},
                                 {"clock_mono_ns", vlc_clock_mono_ns},
                                 {"hostname", vlc_hostname},
                                 {"system", vlc_system},
                                 {"rand_bytes", vlc_rand_bytes},
                                 {"gmtime_iso", vlc_gmtime_iso},
                                 {"localtime_iso", vlc_localtime_iso},
                                 {NULL, NULL}};

// Prefer explicit name to avoid clashing with C libc symbol names.
void vl_open_libclib(VL_State *S) { vl_register_lib(S, "libc", libclib); }
