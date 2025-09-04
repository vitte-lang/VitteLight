// vitte-light/core/undump.h
// Chargeur VLBC (undump): parse, vérifie et expose kstr + code.
// Implémentation: core/undump.c

#ifndef VITTE_LIGHT_CORE_UNDUMP_H
#define VITTE_LIGHT_CORE_UNDUMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "api.h"  // VL_Status

// ───────────────────── Module VLBC ─────────────────────
// Mémoire possédée par l'instance. Libérer via vl_module_free().
typedef struct VL_Module {
  char **kstr;        // tableau de chaînes NUL-terminées
  uint32_t kcount;    // nombre d'entrées dans kstr
  uint8_t *code;      // buffer code binaire
  uint32_t code_len;  // taille du code en octets
} VL_Module;

// ───────────────────── Cycle de vie ─────────────────────
void vl_module_init(VL_Module *m);
void vl_module_free(VL_Module *m);

// ───────────────────── Chargement ─────────────────────
// Parse depuis un buffer mémoire. Remplit *out si succès.
// err/errn optionnels pour un message lisible.
VL_Status vl_module_from_buffer(const uint8_t *data, size_t n, VL_Module *out,
                                char *err, size_t errn);

// Parse depuis un fichier binaire VLBC.
VL_Status vl_module_from_file(const char *path, VL_Module *out, char *err,
                              size_t errn);

// ───────────────────── Accès & outils ─────────────────────
// Récupère une chaîne du pool par index. Retourne NULL si hors bornes.
// Si out_len != NULL, renvoie la longueur hors NUL.
const char *vl_module_kstr(const VL_Module *m, uint32_t si, uint32_t *out_len);

// Désassemble la section code vers un FILE*.
void vl_module_disasm(const VL_Module *m, FILE *out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_UNDUMP_H
