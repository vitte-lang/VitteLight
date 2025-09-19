// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/types.c — Gestion des types de base pour VitteLight VM
// Définitions pour VL_TypeTag, VL_Value et helpers.
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c types.c

#include "zio.h"
#include "vm.h"
#include "vl_compat.h"
#include "tm.h"
#include "table.h"
#include "string.h"
#include "state.h"
#include "parser.h"
#include "opcodes.h"
#include "object.h"
#include "mem.h"
#include "jumptab.h"
#include "gc.h"
#include "func.h"
#include "do.h"
#include "debug.h"
#include "ctype.h"
#include "code.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ───────────── Définition des types ───────────── */

typedef enum {
    VT_T_NIL = 0,
    VT_T_BOOL,
    VT_T_INT,
    VT_T_FLOAT,
    VT_T_STRING,
    VT_T_TABLE,
    VT_T_FUNC,
    VT_T_NATIVE,
    VT_T_USERDATA,
    VT_T_MAX
} VL_TypeTag;

typedef struct {
    VL_TypeTag t;
    union {
        int64_t i;
        double f;
        void* p;
    } v;
} VL_Value;

/* ───────────── Helpers ───────────── */

int vl_is_int(const VL_Value* v)   { return v && v->t == VT_T_INT; }
int vl_is_float(const VL_Value* v) { return v && v->t == VT_T_FLOAT; }
int vl_is_bool(const VL_Value* v)  { return v && v->t == VT_T_BOOL; }
int vl_is_nil(const VL_Value* v)   { return !v || v->t == VT_T_NIL; }
int vl_is_string(const VL_Value* v){ return v && v->t == VT_T_STRING; }

int64_t vl_as_int(const VL_Value* v)   { return v->v.i; }
double  vl_as_float(const VL_Value* v) { return v->v.f; }
void*   vl_as_ptr(const VL_Value* v)   { return v->v.p; }

VL_Value vl_make_int(int64_t x) {
    VL_Value v; v.t = VT_T_INT; v.v.i = x; return v;
}
VL_Value vl_make_float(double x) {
    VL_Value v; v.t = VT_T_FLOAT; v.v.f = x; return v;
}
VL_Value vl_make_bool(int b) {
    VL_Value v; v.t = VT_T_BOOL; v.v.i = (b != 0); return v;
}
VL_Value vl_make_nil(void) {
    VL_Value v; v.t = VT_T_NIL; v.v.p = NULL; return v;
}
VL_Value vl_make_ptr(VL_TypeTag t, void* p) {
    VL_Value v; v.t = t; v.v.p = p; return v;
}

/* ───────────── Debug ───────────── */

const char* vl_type_name(VL_TypeTag t) {
    switch (t) {
        case VT_T_NIL:     return "nil";
        case VT_T_BOOL:    return "bool";
        case VT_T_INT:     return "int";
        case VT_T_FLOAT:   return "float";
        case VT_T_STRING:  return "string";
        case VT_T_TABLE:   return "table";
        case VT_T_FUNC:    return "func";
        case VT_T_NATIVE:  return "native";
        case VT_T_USERDATA:return "userdata";
        default:           return "unknown";
    }
}

void vl_print_value(const VL_Value* v, FILE* out) {
    if (!v) { fputs("nil", out); return; }
    switch (v->t) {
        case VT_T_NIL:    fputs("nil", out); break;
        case VT_T_BOOL:   fputs(v->v.i ? "true" : "false", out); break;
        case VT_T_INT:    fprintf(out, "%" PRId64, v->v.i); break;
        case VT_T_FLOAT:  fprintf(out, "%g", v->v.f); break;
        case VT_T_STRING: fprintf(out, "\"%s\"", (const char*)v->v.p); break;
        default:          fprintf(out, "<%s @%p>", vl_type_name(v->t), v->v.p); break;
    }
}

/* ───────────── Test ───────────── */
#ifdef TYPES_TEST
int main(void) {
    VL_Value a = vl_make_int(42);
    VL_Value b = vl_make_float(3.14);
    VL_Value c = vl_make_bool(1);
    VL_Value d = vl_make_nil();
    VL_Value e = vl_make_ptr(VT_T_STRING, "hello");

    vl_print_value(&a, stdout); fputc('\n', stdout);
    vl_print_value(&b, stdout); fputc('\n', stdout);
    vl_print_value(&c, stdout); fputc('\n', stdout);
    vl_print_value(&d, stdout); fputc('\n', stdout);
    vl_print_value(&e, stdout); fputc('\n', stdout);
    return 0;
}
#endif