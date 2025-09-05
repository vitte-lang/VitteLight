// SPDX-License-Identifier: GPL-3.0-or-later
//
// baselib.c — Base library for Vitte Light
//
// Provides core built-in functions available in all Vitte Light programs.
// Inspired by Lua base library, but minimal and adapted for Vitte Light.
//
// Features:
//   - print, println, printf
//   - type inspection
//   - assertions
//   - conversions: toint, tofloat, tostring
//   - error handling
//   - exit, version
//   - basic math wrappers (min, max, abs)
//   - table helpers: len, keys, values
//
// This file is part of the libraries/ standard set, linked into the VM.
// Corresponds to header: includes/baselib.h

#include "baselib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

// ---------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------
static int vlb_print(VL_State *S);
static int vlb_type(VL_State *S);
static int vlb_assert(VL_State *S);
static int vlb_toint(VL_State *S);
static int vlb_tofloat(VL_State *S);
static int vlb_tostring(VL_State *S);
static int vlb_error(VL_State *S);
static int vlb_exit(VL_State *S);
static int vlb_version(VL_State *S);
static int vlb_abs(VL_State *S);
static int vlb_min(VL_State *S);
static int vlb_max(VL_State *S);
static int vlb_len(VL_State *S);

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static void push_type_string(VL_State *S, VL_Value *v) {
  switch (v->type) {
    case VL_TNIL:
      vl_push_string(S, "nil");
      break;
    case VL_TINT:
      vl_push_string(S, "int");
      break;
    case VL_TFLOAT:
      vl_push_string(S, "float");
      break;
    case VL_TSTR:
      vl_push_string(S, "string");
      break;
    case VL_TTABLE:
      vl_push_string(S, "table");
      break;
    case VL_TFUNC:
      vl_push_string(S, "function");
      break;
    default:
      vl_push_string(S, "unknown");
      break;
  }
}

// ---------------------------------------------------------------------
// Built-in functions
// ---------------------------------------------------------------------

static int vlb_print(VL_State *S) {
  int n = vl_gettop(S);
  for (int i = 1; i <= n; i++) {
    VL_Value *v = vl_get(S, i);
    const char *s = vl_tostring(S, v);
    fputs(s ? s : "nil", stdout);
    if (i < n) fputc('\t', stdout);
  }
  fputc('\n', stdout);
  return 0;
}

static int vlb_type(VL_State *S) {
  VL_Value *v = vl_get(S, 1);
  if (!v) {
    vl_push_string(S, "nil");
    return 1;
  }
  push_type_string(S, v);
  return 1;
}

static int vlb_assert(VL_State *S) {
  VL_Value *cond = vl_get(S, 1);
  if (!cond || !vl_tobool(cond)) {
    const char *msg = (vl_get(S, 2) && vl_isstring(S, 2))
                          ? vl_tocstring(S, vl_get(S, 2))
                          : "assertion failed";
    vl_errorf(S, "%s", msg);
    return vl_error(S);
  }
  vl_push_value(S, cond);
  return 1;
}

static int vlb_toint(VL_State *S) {
  VL_Value *v = vl_get(S, 1);
  if (!v) {
    vl_push_nil(S);
    return 1;
  }
  if (vl_isint(S, 1)) {
    vl_push_value(S, v);
    return 1;
  }
  if (vl_isfloat(S, 1)) {
    vl_push_int(S, (int64_t)vl_tofloat(S, v));
    return 1;
  }
  if (vl_isstring(S, 1)) {
    int64_t x;
    if (vl_parse_int(vl_tocstring(S, v), &x) == AUX_OK) {
      vl_push_int(S, x);
      return 1;
    }
  }
  vl_push_nil(S);
  return 1;
}

static int vlb_tofloat(VL_State *S) {
  VL_Value *v = vl_get(S, 1);
  if (!v) {
    vl_push_nil(S);
    return 1;
  }
  if (vl_isfloat(S, 1)) {
    vl_push_value(S, v);
    return 1;
  }
  if (vl_isint(S, 1)) {
    vl_push_float(S, (double)vl_toint(S, v));
    return 1;
  }
  if (vl_isstring(S, 1)) {
    double d;
    if (vl_parse_float(vl_tocstring(S, v), &d) == AUX_OK) {
      vl_push_float(S, d);
      return 1;
    }
  }
  vl_push_nil(S);
  return 1;
}

static int vlb_tostring(VL_State *S) {
  VL_Value *v = vl_get(S, 1);
  const char *s = vl_tostring(S, v);
  if (!s) {
    vl_push_nil(S);
    return 1;
  }
  vl_push_string(S, s);
  return 1;
}

static int vlb_error(VL_State *S) {
  const char *msg = (vl_get(S, 1) && vl_isstring(S, 1))
                        ? vl_tocstring(S, vl_get(S, 1))
                        : "error";
  vl_errorf(S, "%s", msg);
  return vl_error(S);
}

static int vlb_exit(VL_State *S) {
  int code = 0;
  if (vl_get(S, 1) && vl_isint(S, 1)) code = (int)vl_toint(S, vl_get(S, 1));
  exit(code);
  return 0;
}

static int vlb_version(VL_State *S) {
  vl_push_string(S, "Vitte Light 0.1");
  return 1;
}

static int vlb_abs(VL_State *S) {
  if (vl_isint(S, 1)) {
    int64_t x = vl_toint(S, vl_get(S, 1));
    vl_push_int(S, (x < 0 ? -x : x));
    return 1;
  }
  if (vl_isfloat(S, 1)) {
    double d = vl_tofloat(S, vl_get(S, 1));
    vl_push_float(S, fabs(d));
    return 1;
  }
  vl_push_nil(S);
  return 1;
}

static int vlb_min(VL_State *S) {
  int n = vl_gettop(S);
  if (n == 0) {
    vl_push_nil(S);
    return 1;
  }
  double minv = vl_tonumber(S, vl_get(S, 1));
  for (int i = 2; i <= n; i++) {
    double d = vl_tonumber(S, vl_get(S, i));
    if (d < minv) minv = d;
  }
  vl_push_float(S, minv);
  return 1;
}

static int vlb_max(VL_State *S) {
  int n = vl_gettop(S);
  if (n == 0) {
    vl_push_nil(S);
    return 1;
  }
  double maxv = vl_tonumber(S, vl_get(S, 1));
  for (int i = 2; i <= n; i++) {
    double d = vl_tonumber(S, vl_get(S, i));
    if (d > maxv) maxv = d;
  }
  vl_push_float(S, maxv);
  return 1;
}

static int vlb_len(VL_State *S) {
  VL_Value *v = vl_get(S, 1);
  if (!v) {
    vl_push_int(S, 0);
    return 1;
  }
  if (vl_isstring(S, 1)) {
    vl_push_int(S, (int64_t)strlen(vl_tocstring(S, v)));
    return 1;
  }
  if (vl_istable(S, 1)) {
    vl_push_int(S, (int64_t)vl_table_len(S, v));
    return 1;
  }
  vl_push_int(S, 0);
  return 1;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------

static const VL_Reg baselib[] = {
    {"print", vlb_print},     {"type", vlb_type},
    {"assert", vlb_assert},   {"toint", vlb_toint},
    {"tofloat", vlb_tofloat}, {"tostring", vlb_tostring},
    {"error", vlb_error},     {"exit", vlb_exit},
    {"version", vlb_version}, {"abs", vlb_abs},
    {"min", vlb_min},         {"max", vlb_max},
    {"len", vlb_len},         {NULL, NULL}};

void vl_open_baselib(VL_State *S) {
  vl_register_lib(S, "base", baselib);
  // Also inject globals directly
  for (int i = 0; baselib[i].name; i++) {
    vl_push_cfunction(S, baselib[i].func);
    vl_setglobal(S, baselib[i].name);
  }
}
