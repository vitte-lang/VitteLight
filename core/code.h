// vitte-light/core/code.h
// En-tête public pour la CLI/SDK de tooling VitteLight
// (assembleur/désassembleur VLBC). Ce header expose une petite API réutilisable
// par d’autres outils C/C++. Implémentations de référence dans core/code.c ABI:
// C99, compatible C++ via extern "C".

#ifndef VITTE_LIGHT_CORE_CODE_H
#define VITTE_LIGHT_CORE_CODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ───────────────────── OpCodes (doit rester en phase avec api.c)
// ───────────────────── Note: ces valeurs doivent matcher l’impl VM.

enum VL_OpCode {
  OP_NOP = 0,
  OP_PUSHI = 1,
  OP_PUSHF = 2,
  OP_PUSHS = 3,
  OP_ADD = 4,
  OP_SUB = 5,
  OP_MUL = 6,
  OP_DIV = 7,
  OP_EQ = 8,
  OP_NEQ = 9,
  OP_LT = 10,
  OP_GT = 11,
  OP_LE = 12,
  OP_GE = 13,
  OP_PRINT = 14,
  OP_POP = 15,
  OP_STOREG = 16,
  OP_LOADG = 17,
  OP_CALLN = 18,
  OP_HALT = 19,
};

// ───────────────────── Buffer VLBC générique ─────────────────────

typedef struct VLBC_Buffer {
  uint8_t *data;  // propriété de l’appelant après retour
  size_t len;     // taille utile
} VLBC_Buffer;

// Libère (free) le buffer alloué par l’assembleur ou read_file.
void vlbc_free(VLBC_Buffer *buf);

// ───────────────────── Assembleur ─────────────────────
// Assemble du texte source (VLA sm minimal) vers un blob VLBC v1.
// Retourne true en cas de succès; le buffer retourné doit être libéré via
// vlbc_free. src n’a pas besoin d’être NUL-terminé si n>0.
bool vlbc_assemble(const char *src, size_t n, VLBC_Buffer *out_vlbc);

// ───────────────────── Désassembleur ─────────────────────
// Désassemble un blob VLBC v1 et écrit une représentation lisible dans out
// (stdout si NULL). Retourne true en cas de succès.
bool vlbc_disassemble(const uint8_t *vlbc, size_t n, FILE *out);

// ───────────────────── I/O utilitaires ─────────────────────
// Lit entièrement un fichier binaire en mémoire; renvoie true et remplit out en
// cas de succès. Le buffer doit être libéré via vlbc_free.
bool vlbc_read_file(const char *path, VLBC_Buffer *out);

// Écrit un buffer binaire sur disque; renvoie true si tout a été écrit.
bool vlbc_write_file(const char *path, const void *data, size_t n);

// ───────────────────── Conveniences haut niveau ─────────────────────
// Chaîne de compilation courte: assemble depuis un fichier .vlasm directement
// en .vlbc. Retourne true si ok.
bool vlbc_assemble_file(const char *in_vlasm_path, const char *out_vlbc_path);

// Désassemble un fichier .vlbc vers un flux texte (stdout si out==NULL).
bool vlbc_disassemble_file(const char *in_vlbc_path, FILE *out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_CODE_H
