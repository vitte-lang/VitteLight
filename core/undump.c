// vitte-light/core/undump.c
// Chargeur VLBC ("undump"): parse un module binaire, vérifie et expose
// code + pool de chaînes. Sans dépendance au layout interne de VL_Context.
//
// Fournit:
//   typedef struct VL_Module { char **kstr; uint32_t kcount; uint8_t *code;
//   uint32_t code_len; } VL_Module; void       vl_module_init   (VL_Module *m);
//   void       vl_module_free   (VL_Module *m);
//   VL_Status  vl_module_from_buffer(const uint8_t *data, size_t n, VL_Module
//   *out, char *err, size_t errn); VL_Status  vl_module_from_file  (const char
//   *path, VL_Module *out, char *err, size_t errn); const char*vl_module_kstr
//   (const VL_Module *m, uint32_t si, uint32_t *out_len); void
//   vl_module_disasm(const VL_Module *m, FILE *out);
//
// Notes:
//  - Format VLBC: 'VLBC' u8(version) | u32(kcount) | [u32(len) bytes]*kcount |
//  u32(code_size) | code
//  - Vérifications: bornes, tailles max (limits.h), validation flux via
//  vl_validate_code().
//  - Mémoire: kstr[i] NUL‑terminées, code copié. vl_module_free() libère tout.
//  - Intégration VM: une fonction d'attache côté api.c peut consommer
//  VL_Module.
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -c core/undump.c

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"      // VL_Status
#include "limits.h"   // VLBC_MAX_* si dispo
#include "opcodes.h"  // vl_validate_code, vl_disasm_program, VLBC_MAGIC/VERSION

#ifndef VLBC_MAGIC
#define VLBC_MAGIC "VLBC"
#endif
#ifndef VLBC_VERSION
#define VLBC_VERSION 1u
#endif
#ifndef VLBC_MAX_STRINGS
#define VLBC_MAX_STRINGS 65535u
#endif
#ifndef VLBC_MAX_CODE_BYTES
#define VLBC_MAX_CODE_BYTES (16u * 1024u * 1024u)  // 16 MiB
#endif

// ───────────────────────── Module ─────────────────────────
typedef struct VL_Module {
  char **kstr;
  uint32_t kcount;
  uint8_t *code;
  uint32_t code_len;
} VL_Module;

void vl_module_init(VL_Module *m) {
  if (!m) return;
  memset(m, 0, sizeof(*m));
}

void vl_module_free(VL_Module *m) {
  if (!m) return;
  if (m->kstr) {
    for (uint32_t i = 0; i < m->kcount; i++) free(m->kstr[i]);
    free(m->kstr);
  }
  free(m->code);
  memset(m, 0, sizeof(*m));
}

static void set_err(char *err, size_t errn, const char *fmt, ...) {
  if (!err || !errn) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, errn, fmt, ap);
  va_end(ap);
}

// ───────────────────────── I/O LE ─────────────────────────
static int rd_u8(const uint8_t *p, size_t n, size_t *io, uint8_t *out) {
  if (*io + 1 > n) return 0;
  *out = p[(*io)++];
  return 1;
}
static int rd_u32(const uint8_t *p, size_t n, size_t *io, uint32_t *out) {
  if (*io + 4 > n) return 0;
  uint32_t v = (uint32_t)p[*io] | ((uint32_t)p[*io + 1] << 8) |
               ((uint32_t)p[*io + 2] << 16) | ((uint32_t)p[*io + 3] << 24);
  *io += 4;
  *out = v;
  return 1;
}

// ───────────────────────── Lecture mémoire ─────────────────────────
VL_Status vl_module_from_buffer(const uint8_t *data, size_t n, VL_Module *out,
                                char *err, size_t errn) {
  if (!data || !out) return VL_ERR_INVAL;
  vl_module_init(out);
  if (n < 5u + 4u + 4u) {
    set_err(err, errn, "fichier trop court");
    return VL_ERR_BAD_DATA;
  }

  size_t i = 0;  // curseur
  // magic
  if (!(data[0] == (uint8_t)VLBC_MAGIC[0] &&
        data[1] == (uint8_t)VLBC_MAGIC[1] &&
        data[2] == (uint8_t)VLBC_MAGIC[2] &&
        data[3] == (uint8_t)VLBC_MAGIC[3])) {
    set_err(err, errn, "magic invalide");
    return VL_ERR_BAD_DATA;
  }
  i = 4;

  // version
  uint8_t ver = 0;
  if (!rd_u8(data, n, &i, &ver)) {
    set_err(err, errn, "entête tronquée");
    return VL_ERR_BAD_DATA;
  }
  if (ver != VLBC_VERSION) {
    set_err(err, errn, "version non supportée (%u)", (unsigned)ver);
    return VL_ERR_BAD_DATA;
  }

  // kcount
  uint32_t kcount = 0;
  if (!rd_u32(data, n, &i, &kcount)) {
    set_err(err, errn, "kcount manquant");
    return VL_ERR_BAD_DATA;
  }
  if (kcount > VLBC_MAX_STRINGS) {
    set_err(err, errn, "kcount trop grand (%u)", (unsigned)kcount);
    return VL_ERR_BAD_DATA;
  }

  // allouer tableau de pointeurs
  char **list = NULL;
  if (kcount) {
    list = (char **)calloc(kcount, sizeof(char *));
    if (!list) {
      set_err(err, errn, "OOM kstr list");
      return VL_ERR_OOM;
    }
  }

  // lire chaînes
  for (uint32_t si = 0; si < kcount; si++) {
    uint32_t L = 0;
    if (!rd_u32(data, n, &i, &L)) {
      set_err(err, errn, "kstr[%u]: len manquant", si);
      goto fail;
    }
    if (i + (size_t)L > n) {
      set_err(err, errn, "kstr[%u]: hors limites", si);
      goto fail;
    }
    char *s = (char *)malloc((size_t)L + 1u);
    if (!s) {
      set_err(err, errn, "OOM kstr[%u]", si);
      goto fail;
    }
    memcpy(s, data + i, L);
    s[L] = '\0';
    i += (size_t)L;
    list[si] = s;
  }

  // code
  uint32_t code_len = 0;
  if (!rd_u32(data, n, &i, &code_len)) {
    set_err(err, errn, "taille code manquante");
    goto fail;
  }
  if (code_len > VLBC_MAX_CODE_BYTES) {
    set_err(err, errn, "code trop long (%u)", (unsigned)code_len);
    goto fail;
  }
  if (i + (size_t)code_len > n) {
    set_err(err, errn, "section code tronquée");
    goto fail;
  }

  uint8_t *code = NULL;
  if (code_len) {
    code = (uint8_t *)malloc(code_len);
    if (!code) {
      set_err(err, errn, "OOM code");
      goto fail;
    }
    memcpy(code, data + i, code_len);
  }

  // validation structurelle du code
  if (vl_validate_code(code, code_len, kcount) != VL_OK) {
    set_err(err, errn, "bytecode invalide");
    free(code);
    goto fail;
  }

  // ok
  out->kstr = list;
  out->kcount = kcount;
  out->code = code;
  out->code_len = code_len;
  return VL_OK;

fail:
  if (list) {
    for (uint32_t j = 0; j < kcount; j++) free(list[j]);
    free(list);
  }
  return VL_ERR_BAD_DATA;
}

// ───────────────────────── Lecture fichier ─────────────────────────
static int read_file_all(const char *path, uint8_t **buf, size_t *n) {
  *buf = NULL;
  *n = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return 0;
  }
  rewind(f);
  uint8_t *p = (uint8_t *)malloc((size_t)sz);
  if (!p) {
    fclose(f);
    return 0;
  }
  size_t rd = fread(p, 1, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    free(p);
    return 0;
  }
  *buf = p;
  *n = (size_t)sz;
  return 1;
}

VL_Status vl_module_from_file(const char *path, VL_Module *out, char *err,
                              size_t errn) {
  if (!path || !out) return VL_ERR_INVAL;
  uint8_t *buf = NULL;
  size_t n = 0;
  if (!read_file_all(path, &buf, &n)) return VL_ERR_IO;
  VL_Status st = vl_module_from_buffer(buf, n, out, err, errn);
  free(buf);
  return st;
}

// ───────────────────────── Accès / outils ─────────────────────────
const char *vl_module_kstr(const VL_Module *m, uint32_t si, uint32_t *out_len) {
  if (!m || si >= m->kcount) return NULL;
  if (out_len) *out_len = (uint32_t)strlen(m->kstr[si]);
  return m->kstr[si];
}

void vl_module_disasm(const VL_Module *m, FILE *out) {
  if (!m) return;
  if (!out) out = stdout;
  vl_disasm_program(m->code, m->code_len, out);
}

// ───────────────────────── Test autonome ─────────────────────────
#ifdef VL_UNDUMP_TEST_MAIN
int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s file.vlbc\n", argv[0]);
    return 2;
  }
  VL_Module mod;
  char err[256];
  VL_Status st = vl_module_from_file(argv[1], &mod, err, sizeof(err));
  if (st != VL_OK) {
    fprintf(stderr, "error: %s\n", err);
    return 1;
  }
  printf("kstr=%u, code=%u bytes\n", mod.kcount, mod.code_len);
  vl_module_disasm(&mod, stdout);
  vl_module_free(&mod);
  return 0;
}
#endif
