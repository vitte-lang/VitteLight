/* ============================================================================
   vm.h — API de la machine virtuelle VTBC (C17, portable)
   - VM à pile, frames d’appel, constantes, globaux
   - Dispatch bytecode (voir opcodes.h)
   - Natives C (vt_cfunc) + helpers
   - Chargement d’images via undump.h (optionnel)
   Licence: MIT.
   ============================================================================
 */
#ifndef VT_VM_H
#define VT_VM_H
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
   Export
   -------------------------------------------------------------------------- */
#ifndef VT_VM_API
#define VT_VM_API extern
#endif

/* --------------------------------------------------------------------------
   Valeurs dynamiques (utilise object.h si disponible)
   -------------------------------------------------------------------------- */
#ifdef __has_include
#if __has_include("object.h")
#include "object.h" /* doit définir VT_OBJECT_H / vt_value */
#endif
#endif

#ifndef VT_OBJECT_H
typedef enum {
  VT_T_NIL = 0,
  VT_T_BOOL,
  VT_T_INT,
  VT_T_FLOAT,
  VT_T_STR,
  VT_T_NATIVE,
  VT_T_OBJ
} vt_type;

typedef struct vt_string_stub {
  size_t len;
  char* data;
} vt_string_stub;

typedef struct vt_value {
  uint8_t t;
  union {
    int64_t i;
    double f;
    int b;
    vt_string_stub* s;
    void* p;
  };
} vt_value;
#endif /* !VT_OBJECT_H */

/* --------------------------------------------------------------------------
   Avant-propos VM
   -------------------------------------------------------------------------- */
typedef struct vt_vm vt_vm; /* type opaque */

typedef vt_value (*vt_cfunc)(vt_vm* vm, int argc, vt_value* argv);

/* Configuration optionnelle à la création (conseils, non bloquants). */
typedef struct vt_vm_config {
  int initial_stack_cap;       /* 0 = défaut */
  int initial_frame_cap;       /* 0 = défaut */
  uint64_t default_step_limit; /* 0 = illimité */
  int enable_traces;           /* 0/1 (si debug.h présent) */
} vt_vm_config;

/* --------------------------------------------------------------------------
   API
   -------------------------------------------------------------------------- */

/* Construction / destruction */
VT_VM_API vt_vm* vt_vm_new(const vt_vm_config* cfg);
VT_VM_API void vt_vm_free(vt_vm* vm);

/* Liaison de natives (symbol id → fonction C) */
VT_VM_API int vt_vm_set_native(vt_vm* vm, uint16_t symbol_id, vt_cfunc fn);

/* Helper pour matérialiser une valeur "native" (si vous manipulez directement
 * la pile) */
VT_VM_API vt_value vt_make_native(vt_cfunc fn);

/* Chargement d’une image bytecode (VTBC). Nécessite undump.h dans l’impl. */
VT_VM_API int vt_vm_load_image(vt_vm* vm, const char* path);

/* Exécution : boucle jusqu’à OP_HALT ou erreur. step_limit=0 → illimité.
   Retourne 0 si OK, code négatif (errno-like) sinon. */
VT_VM_API int vt_vm_run(vt_vm* vm, uint64_t step_limit);

/* --------------------------------------------------------------------------
   Notes:
   - vt_vm_run peut écrire un message d’erreur interne (non exposé ici).
   - L’ABI des opcodes est définie dans opcodes.h; l’image VTBC fournit
   CODE/KCON/STRS.
   - vt_value est minimale si object.h absent; si présent, la représentation
     concrète vient de votre runtime.
   -------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_VM_H */
