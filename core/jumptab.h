// vitte-light/core/jumptab.h
// Interface exécution VM via jump-table / computed goto.
// Implémentation: core/jumptab.c

#ifndef VITTE_LIGHT_CORE_JUMPTAB_H
#define VITTE_LIGHT_CORE_JUMPTAB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "api.h"  // VL_Context, VL_Status

// Exécute une instruction en utilisant le dispatch jump-table.
VL_Status vl_step_jumptab(struct VL_Context *ctx);

// Boucle d'exécution continue avec jump-table. max_steps=0 => illimité.
VL_Status vl_run_jumptab(struct VL_Context *ctx, uint64_t max_steps);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_JUMPTAB_H
