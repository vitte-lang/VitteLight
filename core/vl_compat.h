// SPDX-License-Identifier: GPL-3.0-or-later
// core/vl_compat.h — Shim VitteLight: types + API minimales

#ifndef VL_COMPAT_H
#define VL_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Status ===== */
typedef enum { VL_OK = 0, VL_ERR = 1 } VL_Status;

/* ===== Trace flags (utilisés par vitlc/vitli) ===== */
enum {
  VL_TRACE_OP     = 1u << 0,
  VL_TRACE_STACK  = 1u << 1,
  VL_TRACE_GLOBAL = 1u << 2,
  VL_TRACE_CALL   = 1u << 3
};

/* ===== Structures publiques minimales ===== */
typedef struct VL_Context VL_Context;

/* Exposé car vitlc imprime ces champs (cmd_dump) */
typedef struct VL_Module {
  uint32_t kcount;     /* nombre de constantes string */
  uint32_t code_len;   /* taille du segment code */
  char   **kstr;       /* tableau de C-strings */
  uint8_t *code;       /* bytecode */
} VL_Module;

/* Petit buffer util pour REPL */
typedef struct VL_Buffer {
  uint8_t *d;
  size_t   n;
  size_t   cap;
} VL_Buffer;

/* Chrono simple */
typedef struct VL_Stopwatch {
  uint64_t start_ns;
} VL_Stopwatch;

/* Lecteur symbolique (placeholder, vitlc l’a déjà mentionné) */
typedef struct VL_Reader {
  const char *data;
  size_t      len;
  size_t      off;
} VL_Reader;

/* ===== Contexte / VM ===== */
VL_Context* vl_ctx_new(void);
void        vl_ctx_free(VL_Context* ctx);
void        vl_ctx_register_std(VL_Context* ctx);

VL_Status   vl_ctx_attach_module(VL_Context* ctx, const VL_Module* m);
VL_Status   vl_run(VL_Context* ctx, uint64_t max_steps);
void        vl_state_set_ip(VL_Context* ctx, uint64_t ip);
void        vl_state_dump_stack(VL_Context* ctx, FILE* out);

/* ===== Trace ===== */
void        vl_trace_enable(VL_Context* ctx, uint32_t mask);
void        vl_trace_disable(VL_Context* ctx, uint32_t mask);

/* ===== Modules ===== */
VL_Status   vl_module_from_buffer(const uint8_t* buf, size_t n,
                                  VL_Module* out, char* err, size_t errsz);
VL_Status   vl_module_from_file(const char* path,
                                VL_Module* out, char* err, size_t errsz);
void        vl_module_free(VL_Module* m);
void        vl_module_disasm(const VL_Module* m, FILE* out);

/* ===== Assembleur ===== */
int         vl_asm_file(const char* path, uint8_t** out, size_t* n,
                        char* err, size_t errsz);
int         vl_asm(const char* src, size_t srclen,
                   uint8_t** out, size_t* n,
                   char* err, size_t errsz);

/* ===== I/O utils ===== */
int         vl_write_file(const char* path, const void* data, size_t n);
int         vl_read_file_all(const char* path, uint8_t** out, size_t* n);
void        vl_hexdump(const void* data, size_t n, size_t base, FILE* out);

/* ===== Buffer utils ===== */
void        vl_buf_init(VL_Buffer* b);
void        vl_buf_free(VL_Buffer* b);
void        vl_buf_append(VL_Buffer* b, const void* d, size_t n);

/* ===== Stopwatch ===== */
void        vl_sw_start(VL_Stopwatch* sw);
uint64_t    vl_sw_elapsed_ns(VL_Stopwatch* sw);

#ifdef __cplusplus
}
#endif
#endif /* VL_COMPAT_H */