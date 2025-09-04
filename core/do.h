// vitte-light/core/do.h
// Interface publique facultative pour l’outil "do" (tout-en-un) de VitteLight.
// Implémentation principale: core/do.c
// Remarque: pour exposer ces symboles depuis do.c, compiler avec
// -DVL_DO_EXPORTS. Sinon, do.c ne fournit que le binaire CLI (main).

#ifndef VITTE_LIGHT_CORE_DO_H
#define VITTE_LIGHT_CORE_DO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "api.h"
#include "code.h"  // pour l’enum des opcodes et VLBC_Buffer
#include "ctype.h"
#include "debug.h"

// ───────────────────── Utilitaires mémoire ─────────────────────
// Les buffers renvoyés par les fonctions d’assemblage sont alloués via malloc.
// Libérez-les avec free() ou vl_do_free().
static inline void vl_do_free(void *p) {
  if (p) free(p);
}

// ───────────────────── Exports facultatifs (activer avec -DVL_DO_EXPORTS)
// ─────────────────────
#if defined(VL_DO_EXPORTS)

// Nom humain d’un opcode (utilise le mapping interne de do.c)
const char *vl_do_op_name(uint8_t op);

// Horloge haute-résolution en millisecondes (source: CLOCK_MONOTONIC ou
// équivalent)
double vl_do_now_ms(void);

// Fabrique une VM en lisant l’environnement (ex: VL_STACK_CAP). Enregistre
// print/now_ms.
struct VL_Context *vl_do_make_vm_from_env(void);

// Charge et exécute un buffer VLBC dans la VM. trace!=0 active la trace
// pas-à-pas.
int vl_do_run_vlbc(struct VL_Context *vm, const uint8_t *buf, size_t n,
                   int trace, uint64_t max_steps);

// Assemble du texte ASM (une chaîne ou un fichier lu en RAM) en VLBC.
// Retourne true et remplit *out_buf/*out_len. Le caller libère *out_buf via
// free().
bool vl_do_assemble_src(const char *src_utf8, size_t n, uint8_t **out_buf,
                        size_t *out_len);

// Boucle REPL assembleur intégrée.
void vl_do_repl(void);

#endif  // VL_DO_EXPORTS

// ───────────────────── Front CLI (optionnel) ─────────────────────
// Si vous souhaitez intégrer le binaire tel quel dans une autre appli,
// vous pouvez relier do.c et appeler son main.
// Déclaration ici pour commodité (l’implémentation reste dans do.c).
int main(int argc, char **argv);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_DO_H
