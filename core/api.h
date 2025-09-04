// vitte-light/core/api.h
// En-tête public C99 de l’API runtime VitteLight
// Utilisation:
//   #include "api.h"
//   VL_Context *vm = vl_create_default();
//   ...
//   vl_destroy(vm);
//
// ABI: C99 pur. Compatible C++ via extern "C".

#ifndef VITTE_LIGHT_CORE_API_H
#define VITTE_LIGHT_CORE_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ───────────────────── Statuts et types de base ─────────────────────

typedef enum {
  VL_OK = 0,
  VL_ERR_OOM,
  VL_ERR_BAD_BYTECODE,
  VL_ERR_RUNTIME,
  VL_ERR_NOT_FOUND,
  VL_ERR_BAD_ARG,
} VL_Status;

typedef enum {
  VT_NIL = 0,
  VT_BOOL,
  VT_INT,
  VT_FLOAT,
  VT_STR,
  VT_ARRAY,  // réservé
  VT_MAP,    // réservé
  VT_FUNC,   // réservé
  VT_NATIVE,
} VL_Type;

// Opaque forward-decl du contexte
struct VL_Context;

typedef struct VL_String VL_String;  // opaque pour l’utilisateur

typedef struct {
  VL_Type type;
  union {
    bool b;
    int64_t i;
    double f;
    VL_String *s;  // chaîne internée, propriété VM
    void *ptr;     // usage interne pour VT_NATIVE
  } as;
} VL_Value;

typedef struct {
  int code;       // VL_Status
  char msg[256];  // dernier message d’erreur
} VL_Error;

// ───────────────────── Callbacks d’alloc et de log ─────────────────────

typedef void *(*VL_AllocFn)(void *ud, void *ptr, size_t oldsz,
                            size_t newsz);  // realloc-like, newsz=0 => free

typedef void (*VL_LogFn)(void *ud, const char *level, const char *fmt, ...);

typedef struct {
  VL_AllocFn alloc;  // optionnel, défaut = realloc/free
  void *alloc_ud;    // optionnel
  VL_LogFn log;      // optionnel, défaut = stderr
  void *log_ud;      // optionnel
  size_t stack_cap;  // optionnel, défaut = 1024 valeurs
} VL_Config;

// ───────────────────── ABI des fonctions natives ─────────────────────

typedef VL_Status (*VL_NativeFn)(struct VL_Context *ctx, VL_Value *args,
                                 size_t argc, VL_Value *ret, void *user);

// ───────────────────── API publique ─────────────────────

// Création/destruction
struct VL_Context *vl_create(const VL_Config *cfg);
void vl_destroy(struct VL_Context *ctx);

// Confort: crée une VM avec alloc/log par défaut et enregistre "print" et
// "now_ms".
struct VL_Context *vl_create_default(void);

// Erreurs
const VL_Error *vl_last_error(struct VL_Context *ctx);
void vl_clear_error(struct VL_Context *ctx);

// Chargement programme (format VLBC v1)
VL_Status vl_load_program_from_memory(struct VL_Context *ctx, const void *buf,
                                      size_t len);

// Exécution
VL_Status vl_run(struct VL_Context *ctx, uint64_t max_steps);
VL_Status vl_step(struct VL_Context *ctx);

// Natives et globaux
VL_Status vl_register_native(struct VL_Context *ctx, const char *name,
                             VL_NativeFn fn, void *user);

VL_Status vl_set_global(struct VL_Context *ctx, const char *name, VL_Value v);
VL_Status vl_get_global(struct VL_Context *ctx, const char *name,
                        VL_Value *out);

// Création de chaînes VM-propriétaire (utile pour initialiser un VL_Value
// string)
VL_Value vl_make_str(struct VL_Context *ctx, const char *utf8);

// ───────────────────── Helpers inline pour composer des VL_Value
// ───────────────────── Note: ces helpers n’allouent pas (sauf vlv_str qui
// appelle vl_make_str), et ne transfèrent aucune propriété mémoire hors VM.

static inline VL_Value vlv_nil(void) {
  VL_Value v;
  v.type = VT_NIL;
  v.as.ptr = NULL;
  return v;
}
static inline VL_Value vlv_bool(bool b) {
  VL_Value v;
  v.type = VT_BOOL;
  v.as.b = b;
  return v;
}
static inline VL_Value vlv_int(int64_t i) {
  VL_Value v;
  v.type = VT_INT;
  v.as.i = i;
  return v;
}
static inline VL_Value vlv_float(double f) {
  VL_Value v;
  v.type = VT_FLOAT;
  v.as.f = f;
  return v;
}
static inline VL_Value vlv_str(VL_String *s) {
  VL_Value v;
  v.type = VT_STR;
  v.as.s = s;
  return v;
}

// Confort: crée la chaîne dans la VM puis retourne le VL_Value correspondant.
static inline VL_Value vlv_cstr(struct VL_Context *ctx, const char *cstr) {
  return vl_make_str(ctx, cstr);
}

// ───────────────────── Remarques d’ABI ─────────────────────
// • Thread-safety: le contexte n’est pas thread-safe. Protégez avec un mutex si
// partagé. • Propriété mémoire: toutes les VL_String sont possédées par la VM.
// • VL_NativeFn: renvoyez VL_OK et mettez *ret si non NULL. Les args sont
// valides le temps de l’appel. • VL_RUN: max_steps=0 => pas de limite; sinon
// arrêt doux après N instructions.

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_API_H
