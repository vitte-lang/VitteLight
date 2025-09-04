// vitte-light/core/parser.h
// En-tête de l’assembleur VitteLight (ASM -> conteneur VLBC)
// Implémentation: core/parser.c
//
// Entrée: texte source ASM (UTF-8). Sortie: buffer binaire VLBC alloué via
// malloc(). L’appelant doit free() le buffer retourné.
//
// Mise en forme VLBC (rappel):
//   magic 'VLBC' (4) | version(u8)
//   kstr_count(u32)  | [ len(u32) bytes ] * kstr_count
//   code_size(u32)   | code_bytes[code_size]
//
// Dépendances publiques minimales: <stddef.h>, <stdint.h>.

#ifndef VITTE_LIGHT_CORE_PARSER_H
#define VITTE_LIGHT_CORE_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

// Assemble depuis un buffer mémoire.
// - src/n : source ASM
// - out_bytes/out_size : résultats (buffer malloc() + taille)
// - err/errn : message d’erreur humain (optionnel). Rempli si retour 0.
// Retourne 1 si succès, 0 si échec.
int vl_asm(const char *src, size_t n, uint8_t **out_bytes, size_t *out_size,
           char *err, size_t errn);

// Assemble depuis un fichier.
// - path : chemin du fichier source ASM
// - out_bytes/out_size : résultats (buffer malloc() + taille)
// - err/errn : message d’erreur humain (optionnel). Rempli si retour 0.
// Retourne 1 si succès, 0 si échec.
int vl_asm_file(const char *path, uint8_t **out_bytes, size_t *out_size,
                char *err, size_t errn);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_PARSER_H
