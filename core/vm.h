// vitte-light/core/vm.h
// VM VitteLight: contexte, natives, chargement de module, exécution.
// Implémentation: core/vm.c

#ifndef VITTE_LIGHT_CORE_VM_H
#define VITTE_LIGHT_CORE_VM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "api.h"     // VL_Status, VL_Value
#include "state.h"   // introspection, traces, hooks
#include "undump.h"  // VL_Module

// ───────────────────── Contexte ─────────────────────
struct VL_Context;  // opaque

// Création / destruction
struct VL_Context *vl_ctx_new(void);
void vl_ctx_free(struct VL_Context *ctx);

// Attache/détache un module VLBC décodé (voir undump.h)
VL_Status vl_ctx_attach_module(struct VL_Context *ctx, const VL_Module *m);
void vl_ctx_detach_module(struct VL_Context *ctx);

// ───────────────────── Natives ─────────────────────
// Prototype d’une fonction native:
//   args[0..argc-1]  (empilés dans cet ordre)
//   ret peut rester nil si pas de valeur de retour
VL_Status vl_register_native(struct VL_Context *ctx, const char *name,
                             VL_Status (*fn)(struct VL_Context *ctx,
                                             const VL_Value *args, uint8_t argc,
                                             VL_Value *ret, void *udata),
                             void *udata);

// Enregistre les natives standard ("print")
void vl_ctx_register_std(struct VL_Context *ctx);

// ───────────────────── Exécution ─────────────────────
// Exécute une instruction. Retour: VL_OK, VL_DONE, ou erreur.
VL_Status vl_step(struct VL_Context *ctx);
// Boucle d’exécution. max_steps=0 => illimité. VL_OK si HALT atteint.
VL_Status vl_run(struct VL_Context *ctx, uint64_t max_steps);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_VM_H
