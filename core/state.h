// vitte-light/core/state.h
// Inspection et contrôle d’état de la VM VitteLight (opaque-friendly).
// Header public optionnel pour outils de debug, REPL, tests.
// Implémentations attendues côté runtime (ex: core/state.c ou api.c).
//
// Fournit :
//  - Lecture du compteur d’instructions, IP, bornes du code
//  - Accès non-mutable à la pile et aux globals
//  - Résolution des constantes (kstr)
//  - Hooks de traçage et masques de trace
//  - Dumps human-readable (pile, état complet)
//
// Dépendances publiques : <stddef.h>, <stdint.h>, <stdio.h>, "api.h"

#ifndef VITTE_LIGHT_CORE_STATE_H
#define VITTE_LIGHT_CORE_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "api.h"  // VL_Context, VL_Value, VL_Status

// ───────────────────── Types opaques ─────────────────────
struct VL_Context;  // défini dans api.c

// ───────────────────── Lecture d’état courant ─────────────────────
// IP courant en octets depuis le début du segment code
size_t vl_state_ip(const struct VL_Context *ctx);
// Modifie l’IP. Vérifie les bornes du flux (VL_ERR_BAD_BYTECODE si invalide)
VL_Status vl_state_set_ip(struct VL_Context *ctx, size_t ip);

// Pointeur sur le buffer code et sa taille (lecture seule)
const uint8_t *vl_state_code(const struct VL_Context *ctx, size_t *out_len);

// Compteurs d’exécution
uint64_t vl_state_steps_total(const struct VL_Context *ctx);  // steps cumulés

// ───────────────────── Pile d’exécution ─────────────────────
// Taille de pile
size_t vl_state_stack_size(const struct VL_Context *ctx);
// Lecture depuis le haut de pile: idx=0 => top, 1 => suivant…
int vl_state_stack_peek(const struct VL_Context *ctx, size_t idx,
                        VL_Value *out);
// Lecture par index absolu [0 .. size-1]
int vl_state_stack_at(const struct VL_Context *ctx, size_t index,
                      VL_Value *out);

// ───────────────────── Variables globales / constantes ─────────────────────
// Nombre de globals initialisées (ou capacité selon l’impl.)
size_t vl_state_globals_count(const struct VL_Context *ctx);
// Lecture/écriture par index de nom (kstr index). get=1 si trouvé.
int vl_state_global_get(const struct VL_Context *ctx, uint32_t name_si,
                        VL_Value *out);
VL_Status vl_state_global_set(struct VL_Context *ctx, uint32_t name_si,
                              VL_Value v);

// Pool de constantes (kstr)
uint32_t vl_state_kstr_count(const struct VL_Context *ctx);
// Renvoie un pointeur vers la chaîne UTF‑8 et sa longueur (hors NUL). NULL si
// out‑of‑range.
const char *vl_state_kstr_at(const struct VL_Context *ctx, uint32_t si,
                             uint32_t *out_len);

// ───────────────────── Traçage / hooks ─────────────────────
// Masques de trace
enum {
  VL_TRACE_NONE = 0u,
  VL_TRACE_OP = 1u << 0,      // mnémoniques / bytes
  VL_TRACE_STACK = 1u << 1,   // pile après chaque op
  VL_TRACE_GLOBAL = 1u << 2,  // accès globals
  VL_TRACE_CALL = 1u << 3,    // CALLN et natives
};

uint32_t vl_trace_mask(const struct VL_Context *ctx);
void vl_trace_enable(struct VL_Context *ctx, uint32_t mask);
void vl_trace_disable(struct VL_Context *ctx, uint32_t mask);

// Callback par step (après fetch, avant exécution ou après selon impl.)
typedef void (*VL_StepHook)(struct VL_Context *ctx, uint8_t opcode, void *user);
void vl_set_step_hook(struct VL_Context *ctx, VL_StepHook hook, void *user);

// ───────────────────── Dumps human‑readable ─────────────────────
// Imprime une vue compacte de la pile: "[i] <type>:value" par ligne.
VL_Status vl_state_dump_stack(const struct VL_Context *ctx, FILE *out);
// Imprime IP, fenêtre d’octets autour d’IP, et pile/trace optionnels selon
// mask.
VL_Status vl_state_dump(const struct VL_Context *ctx, FILE *out, uint32_t mask);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_STATE_H
