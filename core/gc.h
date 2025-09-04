
// vitte-light/core/gc.h
// API d'un GC observateur/optionnel pour VitteLight (mark→sweep sur VL_String)
// Implémentation: core/gc.c

#ifndef VITTE_LIGHT_CORE_GC_H
#define VITTE_LIGHT_CORE_GC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api.h"  // VL_Context, VL_Status, VL_String

// Politique: par défaut le GC ne libère rien (mode observateur).
// Activer la libération: vl_gc_set_ownership(ctx, true) + enregistrer chaque
// VL_String.

// Flags pour vl_gc_collect
#define VL_GC_VERBOSE 0x01

// Attache un état GC au contexte. trigger_bytes=0 -> valeur par défaut (~1MiB)
void vl_gc_attach(struct VL_Context *ctx, size_t trigger_bytes);
// Détache et détruit l'état GC associé au contexte.
void vl_gc_detach(struct VL_Context *ctx);
// Définit le mode de propriété: true => le GC peut free les chaînes qu'il
// connaît.
void vl_gc_set_ownership(struct VL_Context *ctx, bool own_strings);
// Enregistre une VL_String nouvellement allouée auprès du GC.
void vl_gc_register_string(struct VL_Context *ctx, struct VL_String *s);
// Indexe opportunistement les chaînes déjà accessibles (pile, globaux, consts,
// natives).
void vl_gc_preindex_existing(struct VL_Context *ctx);
// A appeler juste après l'allocation d'une chaîne par la VM (enregistre +
// déclenchement heuristique).
void vl_gc_on_string_alloc(struct VL_Context *ctx, struct VL_String *s);

// Lance une collection complète mark→sweep. flags: VL_GC_VERBOSE pour log
// minimal.
VL_Status vl_gc_collect(struct VL_Context *ctx, int flags);
// Récupère les stats courantes du GC (objets suivis, octets estimés, compte de
// frees cumulé).
void vl_gc_stats(struct VL_Context *ctx, size_t *tracked, size_t *bytes,
                 size_t *frees);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_GC_H
