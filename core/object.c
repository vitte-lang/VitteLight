// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/object.c — Objets du runtime VitteLight (C99, portable)
// - En-tête objet minimal (fallback si object.h absent)
// - Chaîne (VL_String) avec hash et stockage propriétaire
// - Userdata opaque (VL_UserData) avec destructeur optionnel
// - Compteur de références + hooks GC no-op par défaut
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c object.c

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__has_include)
#  if __has_include("object.h")
#    include "object.h"
#  elif __has_include("core/object.h")
#    include "core/object.h"
#  endif
#endif

/* ─────────────────────────── Fallback header ───────────────────────────
 * Si object.h n'est pas disponible, on fournit des définitions minimales
 * compatibles avec le reste du runtime. Si votre projet expose déjà ces
 * types via object.h, ce bloc n'est pas utilisé.
 */
#ifndef VL_OBJECT_API_GUARD
#define VL_OBJECT_API_GUARD 1

/* Tags d’objets heap (cohérent avec types.h si présent) */
typedef enum {
    VO_STRING   = 1,
    VO_TABLE    = 2, /* réservé: non implémenté ici */
    VO_FUNC     = 3, /* réservé */
    VO_NATIVE   = 4, /* réservé */
    VO_USERDATA = 5
} VL_ObjKind;

/* En-tête commun d’objet heap */
typedef struct VL_Obj {
    uint8_t   kind;       /* VL_ObjKind */
    uint8_t   marked;     /* GC mark bit (0/1) */
    uint16_t  flags;      /* libre */
    int32_t   refc;       /* refcount >= 1 */
    struct VL_Obj* next;  /* chaînage GC (optionnel) */
} VL_Obj;

/* Chaîne */
typedef struct VL_String {
    VL_Obj   h;
    size_t   len;
    uint32_t hash;
    char*    data; /* NUL-terminated, len octets utiles */
} VL_String;

/* Userdata opaque avec destructeur */
typedef void (*VL_UDDestructor)(void* p, void* uctx);

typedef struct VL_UserData {
    VL_Obj          h;
    void*           ptr;
    VL_UDDestructor dtor;  /* peut être NULL */
    void*           uctx;  /* contexte pour dtor */
} VL_UserData;

/* API */
const char*    vl_obj_type_name(uint8_t kind);
void           vl_obj_retain(VL_Obj* o);
void           vl_obj_release(VL_Obj* o);

VL_String*     vl_str_new(const char* s);
VL_String*     vl_str_new_len(const char* s, size_t n);
size_t         vl_str_len(const VL_String* s);
const char*    vl_str_data(const VL_String* s);
uint32_t       vl_str_hash(const VL_String* s);

VL_UserData*   vl_ud_new(void* p, VL_UDDestructor dtor, void* uctx);
void*          vl_ud_ptr(const VL_UserData* u);

#endif /* VL_OBJECT_API_GUARD */

/* ─────────────────────────── Utils internes ─────────────────────────── */

static uint32_t vl_hash32_fnv1a(const void* data, size_t n){
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    for (size_t i=0;i<n;i++){ h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u; /* éviter 0 */
}

static void* vl_xmalloc(size_t n){
    void* p = malloc(n);
    if (!p){
        fprintf(stderr,"OOM (%zu bytes)\n", n);
        abort();
    }
    return p;
}

/* ─────────────────────────── Base object ─────────────────────────── */

static void vl_obj_init(VL_Obj* o, uint8_t kind){
    o->kind  = kind;
    o->marked= 0;
    o->flags = 0;
    o->refc  = 1;
    o->next  = NULL;
}

const char* vl_obj_type_name(uint8_t kind){
    switch(kind){
        case VO_STRING:   return "string";
        case VO_TABLE:    return "table";
        case VO_FUNC:     return "func";
        case VO_NATIVE:   return "native";
        case VO_USERDATA: return "userdata";
        default:          return "unknown";
    }
}

void vl_obj_retain(VL_Obj* o){
    if (!o) return;
    if (o->refc <= 0) return; /* protégé contre retain après free */
    o->refc++;
}

/* Forward destructeurs spécifiques */
static void vl_str_free(VL_String* s);
static void vl_ud_free(VL_UserData* u);

void vl_obj_release(VL_Obj* o){
    if (!o) return;
    if (o->refc <= 0) return;
    if (--o->refc > 0) return;

    /* Dispatch selon le type */
    switch (o->kind){
        case VO_STRING:   vl_str_free((VL_String*)o);   break;
        case VO_USERDATA: vl_ud_free((VL_UserData*)o);  break;
        /* VO_TABLE/VO_FUNC/VO_NATIVE: à implémenter si utilisés */
        default: free(o); break;
    }
}

/* ─────────────────────────── String ─────────────────────────── */

VL_String* vl_str_new_len(const char* s, size_t n){
    VL_String* str = (VL_String*)vl_xmalloc(sizeof *str);
    vl_obj_init(&str->h, VO_STRING);
    str->len  = n;
    str->data = (char*)vl_xmalloc(n + 1u);
    if (n && s) memcpy(str->data, s, n);
    str->data[n] = '\0';
    str->hash = vl_hash32_fnv1a(str->data, n);
    return str;
}

VL_String* vl_str_new(const char* s){
    if (!s) s = "";
    return vl_str_new_len(s, strlen(s));
}

size_t vl_str_len(const VL_String* s){
    return s ? s->len : 0u;
}

const char* vl_str_data(const VL_String* s){
    return s ? s->data : "";
}

uint32_t vl_str_hash(const VL_String* s){
    return s ? s->hash : 0u;
}

static void vl_str_free(VL_String* s){
    if (!s) return;
    free(s->data);
    free(s);
}

/* ─────────────────────────── Userdata ─────────────────────────── */

VL_UserData* vl_ud_new(void* p, VL_UDDestructor dtor, void* uctx){
    VL_UserData* u = (VL_UserData*)vl_xmalloc(sizeof *u);
    vl_obj_init(&u->h, VO_USERDATA);
    u->ptr  = p;
    u->dtor = dtor;
    u->uctx = uctx;
    return u;
}

void* vl_ud_ptr(const VL_UserData* u){
    return u ? u->ptr : NULL;
}

static void vl_ud_free(VL_UserData* u){
    if (!u) return;
    if (u->dtor) u->dtor(u->ptr, u->uctx);
    free(u);
}

/* ─────────────────────────── Optionnel: mini tests ─────────────────────────── */
#ifdef OBJECT_TEST
int main(void){
    VL_String* s = vl_str_new("hello");
    printf("type=%s len=%zu hash=%u data=%s\n",
           vl_obj_type_name(s->h.kind), vl_str_len(s), vl_str_hash(s), vl_str_data(s));
    vl_obj_release(&s->h);

    VL_UserData* u = vl_ud_new((void*)0x1234, NULL, NULL);
    printf("ud ptr=%p type=%s\n", vl_ud_ptr(u), vl_obj_type_name(u->h.kind));
    vl_obj_release(&u->h);
    return 0;
}
#endif
