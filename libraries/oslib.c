// SPDX-License-Identifier: GPL-3.0-or-later
//
// oslib.c — Portable OS library for Vitte Light (C17)
// Namespace: "os"
//
// Scope:
//   - Platform info:   os.name() -> "windows"|"linux"|"darwin"|"bsd"|"unknown"
//                      os.arch() -> "x86_64"|"aarch64"|"arm"|"i386"|...
//   - Paths:           os.cwd() -> string | (nil, errmsg)
//                      os.chdir(path) -> true | (nil, errmsg)
//                      os.tmpdir() -> string
//                      os.home() -> string | (nil, errmsg)
//                      os.mktmpdir([prefix]) -> path | (nil, errmsg)
//   - Process/tools:   os.exec(cmdline) -> int              // system()
//                      os.popen_read(cmdline) -> out, code  // capture stdout
//                      os.isatty(fd:int) -> bool
//                      os.cpu_count() -> int
//                      os.uptime() -> int64  // seconds since boot (best
//                      effort)
//   - Perms (POSIX):   os.umask([newmask]) -> oldmask | (nil, "ENOSYS")
//                      os.chmod(path, mode:int) -> true | (nil, "ENOSYS")
//
// Depends: auxlib.h, state.h, object.h, vm.h

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
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <sys/sysinfo.h>
#endif
#endif

// ------------------------------------------------------------
// VM arg helpers
// ------------------------------------------------------------
static const char *os_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t os_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int os_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}

// ------------------------------------------------------------
// Platform info
// ------------------------------------------------------------
static int vlos_name(VL_State *S) {
#if defined(_WIN32)
  vl_push_string(S, "windows");
#elif defined(__APPLE__)
  vl_push_string(S, "darwin");
#elif defined(__linux__)
  vl_push_string(S, "linux");
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  vl_push_string(S, "bsd");
#else
  vl_push_string(S, "unknown");
#endif
  return 1;
}

static int vlos_arch(VL_State *S) {
#if defined(__x86_64__) || defined(_M_X64)
  vl_push_string(S, "x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
  vl_push_string(S, "aarch64");
#elif defined(__i386__) || defined(_M_IX86)
  vl_push_string(S, "i386");
#elif defined(__arm__) || defined(_M_ARM)
  vl_push_string(S, "arm");
#elif defined(__riscv) && __riscv_xlen == 64
  vl_push_string(S, "riscv64");
#elif defined(__riscv) && __riscv_xlen == 32
  vl_push_string(S, "riscv32");
#else
  vl_push_string(S, "unknown");
#endif
  return 1;
}

// ------------------------------------------------------------
// CWD / CHDIR
// ------------------------------------------------------------
static int vlos_cwd(VL_State *S) {
#if defined(_WIN32)
  char buf[PATH_MAX];
  DWORD n = GetCurrentDirectoryA((DWORD)AUX_ARRAY_LEN(buf), buf);
  if (n == 0 || n >= AUX_ARRAY_LEN(buf)) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, buf);
  return 1;
#else
  char buf[PATH_MAX];
  if (!getcwd(buf, sizeof buf)) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, buf);
  return 1;
#endif
}

static int vlos_chdir(VL_State *S) {
  const char *path = os_check_str(S, 1);
#if defined(_WIN32)
  int rc = _chdir(path);
#else
  int rc = chdir(path);
#endif
  if (rc != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

// ------------------------------------------------------------
// tmpdir / home / mktmpdir
// ------------------------------------------------------------
static int vlos_tmpdir(VL_State *S) {
  (void)S;
  const char *p = NULL;
#if defined(_WIN32)
  p = aux_getenv("TMP");
  if (!p || !*p) p = aux_getenv("TEMP");
  if (!p || !*p) p = "C:\\Windows\\Temp";
#else
  p = aux_getenv("TMPDIR");
  if (!p || !*p) p = "/tmp";
#endif
  vl_push_string(S, p);
