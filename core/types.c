// SPDX-License-Identifier: GPL-3.0-or-later
// core/types.c — Implémentation du système de valeurs taggées VitteLight

#include "types.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Helpers internes                                                           */
/* -------------------------------------------------------------------------- */
static char* vl__strdup(const char* s) {
    if (!s) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    size_t n = strlen(s) + 1;
    char* dup = (char*)malloc(n);
    if (!dup) return NULL;
    memcpy(dup, s, n);
    return dup;
}

static void vl__set_nil(VL_Value* v) {
    if (!v) return;
    v->t = VT_T_NIL;
    v->v.p = NULL;
}

/* -------------------------------------------------------------------------- */
/* Constructeurs                                                               */
/* -------------------------------------------------------------------------- */
VL_Value vl_make_nil(void) {
    VL_Value v; vl__set_nil(&v); return v;
}

VL_Value vl_make_bool(int b) {
    VL_Value v; v.t = VT_T_BOOL; v.v.i = b ? 1 : 0; return v;
}

VL_Value vl_make_int(int64_t i) {
    VL_Value v; v.t = VT_T_INT; v.v.i = i; return v;
}

VL_Value vl_make_float(double f) {
    VL_Value v; v.t = VT_T_FLOAT; v.v.f = f; return v;
}

VL_Value vl_make_ptr_tag(VL_TypeTag t, void* p) {
    VL_Value v; v.t = t; v.v.p = p; return v;
}

VL_Value vl_make_cstring(const char* s) {
    VL_Value v = vl_make_nil();
    char* dup = vl__strdup(s);
    if (!dup) return v; /* OOM: retourne NIL */
    v.t = VT_T_STRING;
    v.v.p = dup;
    return v;
}

/* -------------------------------------------------------------------------- */
/* Gestion mémoire / copie                                                     */
/* -------------------------------------------------------------------------- */
void vl_value_free(VL_Value* v) {
    if (!v) return;
    if (v->t == VT_T_STRING && v->v.p) {
        free(v->v.p);
    }
    vl__set_nil(v);
}

int vl_value_copy(VL_Value* dst, const VL_Value* src) {
    if (!dst) return -1;
    if (dst == src) return 0;
    if (!src) {
        vl__set_nil(dst);
        return 0;
    }
    if (src->t == VT_T_STRING) {
        char* dup = vl__strdup((const char*)src->v.p);
        if (!dup) return -1;
        vl_value_free(dst);
        dst->t = VT_T_STRING;
        dst->v.p = dup;
        return 0;
    }
    /* types simples: copie directe */
    vl_value_free(dst);
    *dst = *src;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Accesseurs / tests                                                          */
/* -------------------------------------------------------------------------- */
int vl_is_nil(const VL_Value* v)     { return !v || v->t == VT_T_NIL; }
int vl_is_bool(const VL_Value* v)    { return v && v->t == VT_T_BOOL; }
int vl_is_int(const VL_Value* v)     { return v && v->t == VT_T_INT; }
int vl_is_float(const VL_Value* v)   { return v && v->t == VT_T_FLOAT; }
int vl_is_string(const VL_Value* v)  { return v && v->t == VT_T_STRING; }

int64_t vl_as_int(const VL_Value* v)   { return (v && v->t == VT_T_INT)   ? v->v.i : 0; }
double  vl_as_float(const VL_Value* v) { return (v && v->t == VT_T_FLOAT) ? v->v.f : 0.0; }
void*   vl_as_ptr(const VL_Value* v)   { return v ? v->v.p : NULL; }

/* -------------------------------------------------------------------------- */
/* Utilitaires                                                                 */
/* -------------------------------------------------------------------------- */
const char* vl_type_name(VL_TypeTag t) {
    switch (t) {
        case VT_T_NIL:      return "nil";
        case VT_T_BOOL:     return "bool";
        case VT_T_INT:      return "int";
        case VT_T_FLOAT:    return "float";
        case VT_T_STRING:   return "string";
        case VT_T_TABLE:    return "table";
        case VT_T_FUNC:     return "func";
        case VT_T_NATIVE:   return "native";
        case VT_T_USERDATA: return "userdata";
        default:            return "unknown";
    }
}

void vl_print_value(const VL_Value* v, FILE* out) {
    if (!out) out = stdout;
    if (!v) {
        fputs("nil", out);
        return;
    }
    switch (v->t) {
        case VT_T_NIL:
            fputs("nil", out);
            break;
        case VT_T_BOOL:
            fputs(v->v.i ? "true" : "false", out);
            break;
        case VT_T_INT:
            fprintf(out, "%" PRId64, v->v.i);
            break;
        case VT_T_FLOAT:
            fprintf(out, "%g", v->v.f);
            break;
        case VT_T_STRING:
            fprintf(out, "\"%s\"", v->v.p ? (const char*)v->v.p : "");
            break;
        default:
            fprintf(out, "<%s@%p>", vl_type_name(v->t), v->v.p);
            break;
    }
}

int vl_value_cmp(const VL_Value* a, const VL_Value* b) {
    if (a == b) return 0;
    if (!a) return b ? -1 : 0;
    if (!b) return 1;

    if (a->t != b->t) return (int)a->t - (int)b->t;

    switch (a->t) {
        case VT_T_BOOL:
        case VT_T_INT:
            if (a->v.i < b->v.i) return -1;
            if (a->v.i > b->v.i) return 1;
            return 0;
        case VT_T_FLOAT:
            if (a->v.f < b->v.f) return -1;
            if (a->v.f > b->v.f) return 1;
            return 0;
        case VT_T_STRING: {
            const char* sa = a->v.p ? (const char*)a->v.p : "";
            const char* sb = b->v.p ? (const char*)b->v.p : "";
            int rc = strcmp(sa, sb);
            return (rc < 0) ? -1 : (rc > 0 ? 1 : 0);
        }
        case VT_T_NIL:
            return 0;
        default: {
            uintptr_t pa = (uintptr_t)a->v.p;
            uintptr_t pb = (uintptr_t)b->v.p;
            if (pa < pb) return -1;
            if (pa > pb) return 1;
            return 0;
        }
    }
}

/* -------------------------------------------------------------------------- */
#ifdef TYPES_TEST
#include <stdio.h>
int main(void) {
    VL_Value a = vl_make_int(42);
    VL_Value b = vl_make_float(3.14);
    VL_Value c = vl_make_bool(1);
    VL_Value d = vl_make_nil();
    VL_Value e = vl_make_cstring("hello");

    vl_print_value(&a, stdout); fputc('\n', stdout);
    vl_print_value(&b, stdout); fputc('\n', stdout);
    vl_print_value(&c, stdout); fputc('\n', stdout);
    vl_print_value(&d, stdout); fputc('\n', stdout);
    vl_print_value(&e, stdout); fputc('\n', stdout);

    vl_value_free(&e);
    return 0;
}
#endif
