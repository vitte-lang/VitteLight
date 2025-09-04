// vitte-light/core/func.h
// API natives avec signatures et mini-stdlib VitteLight.
// Implémentation: core/func.c

#ifndef VITTE_LIGHT_CORE_FUNC_H
#define VITTE_LIGHT_CORE_FUNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api.h"

// ───────────────────── Enregistrement avec signature ─────────────────────
// Signature DSL:
//   "i" int64, "f" double, "s" string, "b" bool, "n" nil, "a" any
//   ex: "i,i->i"  add int->int ; "s,*->s" concat strings
// Varargs: ajoutez ",*" pour autoriser des args supplémentaires (type libre).
//
// Retour: VL_OK si succès, sinon code d’erreur.
VL_Status vl_register_native_sig(struct VL_Context *ctx, const char *name,
                                 const char *sig, VL_NativeFn user_fn,
                                 void *user_ud);

// ───────────────────── Mini-stdlib natives ─────────────────────
// Flags à combiner.
#define VLF_STD_MATH 0x01
#define VLF_STD_STR 0x02

VL_Status vl_register_std_natives(struct VL_Context *ctx, unsigned flags);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_FUNC_H
