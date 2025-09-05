// SPDX-License-Identifier: GPL-3.0-or-later
//
// corolib.c — Coroutine library for Vitte Light
//
// API (namespace "coroutine"):
//   - coroutine.create(func)         -> thread
//   - coroutine.wrap(func)           -> callable(...)->results | error
//   - coroutine.resume(thread, ...)  -> (true, results...) | (false, errmsg)
//   - coroutine.yield(...)           -> yield to caller
//   - coroutine.status(thread)       -> "running" | "suspended" | "normal" |
//   "dead"
//   - coroutine.running()            -> (thread | nil, is_main:boolean)
//   - coroutine.isyieldable()        -> boolean
//
// Notes:
// - This is a thin wrapper over the VM’s thread/coroutine API.
// - Requires VM primitives analogous to Lua’s: vl_newthread, vl_resume,
// vl_yield,
//   vl_status, vl_pushthread, vl_xmove, etc.
// - Header counterparts: includes/corolib.h (optional), VM: state.h, vm.h,
// object.h.

#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static void coro_arg_check_func(VL_State *S, int idx) {
  if (!vl_isfunction(S, idx)) {
    vl_errorf(S, "bad argument #%d (function expected)", idx);
    vl_error(S);
  }
}

static void coro_arg_check_thread(VL_State *S, int idx) {
  if (!vl_isthread(S, idx)) {
    vl_errorf(S, "bad argument #%d (thread expected)", idx);
    vl_error(S);
  }
}

static const char *coro_status_str(VL_State *co) {
  switch (vl_status(co)) {
    case VL_CORO_RUNNING:
      return "running";
    case VL_CORO_SUSPENDED:
      return "suspended";
    case VL_CORO_NORMAL:
      return "normal";
    case VL_CORO_DEAD:
      return "dead";
    default:
      return "unknown";
  }
}

// Push (false, errmsg) on S
static int coro_push_error_tuple(VL_State *S, VL_State *co) {
  vl_push_bool(S, false);
  // try to pull error object/string from co top
  if (vl_gettop(co) > 0) {
    const char *msg = vl_tostring(co, vl_get(co, -1));
    vl_push_string(S, msg ? msg : "coroutine error");
  } else {
    vl_push_string(S, "coroutine error");
  }
  return 2;
}

// ---------------------------------------------------------------------
// coroutine.create(func)
// ---------------------------------------------------------------------

static int vlcoro_create(VL_State *S) {
  coro_arg_check_func(S, 1);
  VL_State *co = vl_newthread(S);  // stack: [func] -> pushes thread on S
  // move function to coroutine stack as its entry point
  vl_push_value(S, 1);  // duplicate func on S
  vl_xmove(S, co, 1);   // move func to co
  // return thread (already on S top from vl_newthread)
  return 1;
}

// ---------------------------------------------------------------------
// coroutine.resume(thread, ...)
// returns (true, results...) or (false, errmsg)
// ---------------------------------------------------------------------

static int vlcoro_resume(VL_State *S) {
  coro_arg_check_thread(S, 1);
  VL_State *co = vl_tothread(S, 1);

  int status = vl_status(co);
  if (status == VL_CORO_DEAD) {
    vl_push_bool(S, false);
    vl_push_string(S, "cannot resume dead coroutine");
    return 2;
  }

  int narg = vl_gettop(S) - 1;
  if (narg > 0) {
    // move arguments 2..top from S to co
    vl_xmove(S, co, narg);
  }

  int nres = 0;
  int rc = vl_resume(co, S, narg,
                     &nres);  // analogous to Lua: resume co from S with narg
  if (rc == VL_OK || rc == VL_YIELD) {
    // success path: push true then move results back to caller
    vl_push_bool(S, true);
    if (nres > 0) vl_xmove(co, S, nres);
    return 1 + nres;
  }
  // error path: push (false, errmsg)
  return coro_push_error_tuple(S, co);
}

// ---------------------------------------------------------------------
// coroutine.yield(...)
// ---------------------------------------------------------------------

static int vlcoro_yield(VL_State *S) {
  int nres = vl_gettop(S);
  return vl_yield(S, nres);
}

// ---------------------------------------------------------------------
// coroutine.status(thread) -> string
// ---------------------------------------------------------------------

static int vlcoro_status(VL_State *S) {
  coro_arg_check_thread(S, 1);
  VL_State *co = vl_tothread(S, 1);
  vl_push_string(S, coro_status_str(co));
  return 1;
}

// ---------------------------------------------------------------------
// coroutine.running() -> (thread|nil, is_main:boolean)
// - Returns current coroutine handle, and whether it is the main thread.
// ---------------------------------------------------------------------

static int vlcoro_running(VL_State *S) {
  int ismain = vl_is_main_thread(S);
  if (ismain) {
    vl_push_nil(S);
    vl_push_bool(S, true);
    return 2;
  }
  // push current thread handle
  vl_pushthread(S);  // pushes current coroutine as a thread object
  vl_push_bool(S, false);
  return 2;
}

// ---------------------------------------------------------------------
// coroutine.isyieldable() -> boolean
// ---------------------------------------------------------------------

static int vlcoro_isyieldable(VL_State *S) {
  vl_push_bool(S, vl_isyieldable(S));
  return 1;
}

// ---------------------------------------------------------------------
// coroutine.wrap(func)
// Returns a function that resumes the coroutine and either returns
// its results or throws an error in the caller.
// ---------------------------------------------------------------------

static int vlcoro_wrap_closure(VL_State *S) {
  // upvalue 1: the thread
  VL_State *co = vl_tothread_upvalue(S, 1);
  int narg = vl_gettop(S);
  if (narg > 0) vl_xmove(S, co, narg);

  int nres = 0;
  int rc = vl_resume(co, S, narg, &nres);
  if (rc == VL_OK || rc == VL_YIELD) {
    if (nres > 0) vl_xmove(co, S, nres);
    return nres;
  }
  // on error, raise in caller
  const char *msg =
      (vl_gettop(co) > 0) ? vl_tostring(co, vl_get(co, -1)) : "coroutine error";
  vl_errorf(S, "%s", msg ? msg : "coroutine error");
  return vl_error(S);
}

static int vlcoro_wrap(VL_State *S) {
  coro_arg_check_func(S, 1);
  VL_State *co = vl_newthread(S);  // stack: [func] -> thread pushed
  vl_push_value(S, 1);
  vl_xmove(S, co, 1);  // move func into thread
  // create closure with upvalue = co thread
  vl_push_cclosure(S, vlcoro_wrap_closure, /*nup*/ 1);
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------

static const VL_Reg corolib[] = {
    {"create", vlcoro_create},   {"resume", vlcoro_resume},
    {"yield", vlcoro_yield},     {"status", vlcoro_status},
    {"running", vlcoro_running}, {"isyieldable", vlcoro_isyieldable},
    {"wrap", vlcoro_wrap},       {NULL, NULL}};

void vl_open_corolib(VL_State *S) { vl_register_lib(S, "coroutine", corolib); }
