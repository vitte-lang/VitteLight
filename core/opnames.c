// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/opnames.c — Table de noms pour opcodes VitteLight
// Permet de mapper un code opcode → nom lisible pour le désassembleur.
// Compatible avec disasm, trace et outils CLI.
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c opnames.c

#include <stdint.h>
#include <stddef.h>

#if defined(__has_include)
#  if __has_include("opcodes.h")
#    include "opcodes.h"
#  elif __has_include("core/opcodes.h")
#    include "core/opcodes.h"
#  endif
#endif

#ifndef VL_MAX_OPCODE
#  define VL_MAX_OPCODE 256
#endif

/* ───────────── Table globale ─────────────
 * Remplie selon les opcodes connus. Les slots non utilisés = "???".
 */
const char *const vl_opnames[VL_MAX_OPCODE] = {
    [0x00] = "HALT",
    [0x01] = "NOP",
    [0x02] = "BREAK",

    /* Constantes / Stack */
    [0x10] = "PUSHS",
    [0x11] = "PUSHI",
    [0x12] = "PUSHF",
    [0x13] = "PUSHNIL",
    [0x14] = "PUSHBOOL",
    [0x15] = "PUSHK",
    [0x16] = "POP",
    [0x17] = "DUP",
    [0x18] = "SWAP",

    /* Arithmétique */
    [0x20] = "ADD",
    [0x21] = "SUB",
    [0x22] = "MUL",
    [0x23] = "DIV",
    [0x24] = "MOD",
    [0x25] = "NEG",
    [0x26] = "INC",
    [0x27] = "DEC",

    /* Comparaisons */
    [0x30] = "CMP",
    [0x31] = "EQ",
    [0x32] = "NE",
    [0x33] = "LT",
    [0x34] = "LE",
    [0x35] = "GT",
    [0x36] = "GE",

    /* Logique */
    [0x40] = "AND",
    [0x41] = "OR",
    [0x42] = "XOR",
    [0x43] = "NOT",

    /* Sauts */
    [0x50] = "JUMP",
    [0x51] = "JZ",
    [0x52] = "JNZ",
    [0x53] = "JLT",
    [0x54] = "JLE",
    [0x55] = "JGT",
    [0x56] = "JGE",

    /* Fonctions */
    [0x60] = "CALL",
    [0x61] = "CALLN",
    [0x62] = "RET",

    /* Tables / Objets */
    [0x70] = "NEWTABLE",
    [0x71] = "GETFIELD",
    [0x72] = "SETFIELD",
    [0x73] = "GETINDEX",
    [0x74] = "SETINDEX",

    /* Globals / Locals */
    [0x80] = "GETGLOBAL",
    [0x81] = "SETGLOBAL",
    [0x82] = "GETLOCAL",
    [0x83] = "SETLOCAL",

    /* VM / Divers */
    [0x90] = "TRACE",
    [0x91] = "PRINT",
    [0x92] = "DUMPSTACK",
};

/* ───────────── Fallback ───────────── */

const char *vl_op_name(uint8_t op) {
    size_t idx = (size_t)op;
    size_t cap = VL_MAX_OPCODE;
    if (cap > (sizeof vl_opnames / sizeof vl_opnames[0]))
        cap = sizeof vl_opnames / sizeof vl_opnames[0];
    if (idx < cap && vl_opnames[idx])
        return vl_opnames[idx];
    return "UNKNOWN";
}

#ifdef OPNAMES_TEST
#include <stdio.h>
int main(void) {
    for (int i = 0; i < 0x95; i++) {
        printf("%02X: %s\n", i, vl_op_name((uint8_t)i));
    }
    return 0;
}
#endif
