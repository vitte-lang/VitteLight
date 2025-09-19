// SPDX-License-Identifier: GPL-3.0-or-later
// core/vl_compat.c — Implémentations stub des API VL_*

#include "vl_compat.h"
#include <stdlib.h>
#include <string.h>

/* ===== Context ===== */
struct VL_Context { int dummy; };

VL_Context* vl_ctx_new(void) {
  VL_Context* c = (VL_Context*)malloc(sizeof *c);
  if (c) c->dummy = 0;
  return c;
}
void vl_ctx_free(VL_Context* ctx) { free(ctx); }
void vl_ctx_register_std(VL_Context* ctx) { (void)ctx; }

VL_Status vl_ctx_attach_module(VL_Context* ctx, const VL_Module* m) {
  (void)ctx; (void)m; return VL_OK;
}

VL_Status vl_run(VL_Context* ctx, uint64_t max_steps) {
  (void)ctx; (void)max_steps; return VL_OK;
}

void vl_state_set_ip(VL_Context* ctx, uint64_t ip) { (void)ctx; (void)ip; }

void vl_state_dump_stack(VL_Context* ctx, FILE* out) {
  (void)ctx;
  fprintf(out ? out : stdout, "[stack: stub]\n");
}

/* ===== Trace ===== */
void vl_trace_enable(VL_Context* ctx, uint32_t mask) { (void)ctx; (void)mask; }
void vl_trace_disable(VL_Context* ctx, uint32_t mask) { (void)ctx; (void)mask; }

/* ===== Module load/disasm ===== */
VL_Status vl_module_from_buffer(const uint8_t* buf, size_t n,
                                VL_Module* out, char* err, size_t errsz) {
  (void)buf; (void)n;
  if (out) { out->kcount = 0; out->code_len = 0; out->kstr = NULL; out->code = NULL; }
  if (err && errsz) { if (errsz) err[0] = 0; }
  return VL_OK;
}

VL_Status vl_module_from_file(const char* path,
                              VL_Module* out, char* err, size_t errsz) {
  (void)path;
  if (out) { out->kcount = 0; out->code_len = 0; out->kstr = NULL; out->code = NULL; }
  if (err && errsz) { if (errsz) err[0] = 0; }
  return VL_OK;
}

void vl_module_free(VL_Module* m) {
  if (!m) return;
  if (m->kstr) {
    for (uint32_t i=0;i<m->kcount;i++) free(m->kstr[i]);
    free(m->kstr);
  }
  free(m->code);
  m->kstr=NULL; m->code=NULL; m->kcount=0; m->code_len=0;
}

void vl_module_disasm(const VL_Module* m, FILE* out) {
  (void)m;
  fprintf(out ? out : stdout, "[disasm: stub]\n");
}

/* ===== Assembler ===== */
int vl_asm_file(const char* path, uint8_t** out, size_t* n,
                char* err, size_t errsz) {
  (void)path;
  if (out) *out = NULL;
  if (n) *n = 0;
  if (err && errsz) { if (errsz) err[0] = 0; }
  return 1; /* OK (vide) */
}

int vl_asm(const char* src, size_t srclen,
           uint8_t** out, size_t* n,
           char* err, size_t errsz) {
  (void)src; (void)srclen;
  if (out) *out = NULL;
  if (n) *n = 0;
  if (err && errsz) { if (errsz) err[0] = 0; }
  return 1; /* OK (vide) */
}

/* ===== I/O ===== */
int vl_write_file(const char* path, const void* data, size_t n) {
  FILE* f = fopen(path, "wb"); if (!f) return 0;
  size_t w = data && n ? fwrite(data, 1, n, f) : 0;
  fclose(f);
  return w==n;
}

int vl_read_file_all(const char* path, uint8_t** out, size_t* n) {
  if (!out || !n) return 0;
  *out=NULL; *n=0;
  FILE* f = fopen(path, "rb"); if (!f) return 0;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz < 0) { fclose(f); return 0; }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (!buf) { fclose(f); return 0; }
  size_t rd = fread(buf, 1, (size_t)sz, f); fclose(f);
  if (rd != (size_t)sz) { free(buf); return 0; }
  *out = buf; *n = (size_t)sz; return 1;
}

void vl_hexdump(const void* data, size_t n, size_t base, FILE* out) {
  const uint8_t* p = (const uint8_t*)data;
  FILE* o = out ? out : stdout;
  for (size_t i=0;i<n;i++) {
    if ((i & 15) == 0) fprintf(o, "%08zx: ", base + i);
    fprintf(o, "%02x ", p[i]);
    if ((i & 15) == 15 || i == n-1) fputc('\n', o);
  }
}

/* ===== Buffer ===== */
void vl_buf_init(VL_Buffer* b){ if(!b) return; b->d=NULL; b->n=b->cap=0; }
void vl_buf_free(VL_Buffer* b){ if(!b) return; free(b->d); b->d=NULL; b->n=b->cap=0; }
void vl_buf_append(VL_Buffer* b, const void* d, size_t n){
  if(!b || !d || n==0) return;
  if (b->n + n > b->cap){
    size_t nc = b->cap ? b->cap*2 : 64;
    while (nc < b->n+n) nc *= 2;
    uint8_t* nd = (uint8_t*)realloc(b->d, nc);
    if (!nd) return;
    b->d = nd; b->cap = nc;
  }
  memcpy(b->d + b->n, d, n); b->n += n;
}

/* ===== Stopwatch ===== */
void vl_sw_start(VL_Stopwatch* sw){ if(!sw) return; sw->start_ns = 0; }
uint64_t vl_sw_elapsed_ns(VL_Stopwatch* sw){ (void)sw; return 1000000ULL; } /* 1ms */