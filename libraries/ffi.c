// SPDX-License-Identifier: GPL-3.0-or-later
//
// ffi.c — Portable FFI layer for Vitte Light (C17)
// -------------------------------------------------
// Features (with libffi):
//   - Type model: void, integers (i/u8/16/32/64), floats (f32/f64), ptr
//   - ABI selection: default, sysv, unix64, win64 (mapped to ffi_abi)
//   - CIF builder: prepare call interface from arrays or signature string
//   - Calls: vl_ffi_call(cif, fn_ptr, argv, retbuf)
//   - Helpers: size/alignment for types, dl-symbol lookup helpers
//   - Optional signature parser: e.g. "i32(i64, f64, ptr)"
// Fallback without libffi (-DVL_HAVE_LIBFFI not set): stubs return AUX_ENOSYS.
//
// Depends: includes/auxlib.h, dl.h (for symbol loading), optional
// includes/ffi.h
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_LIBFFI -c ffi.c
//   cc ... ffi.o -lffi
//

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "dl.h"

#ifdef VL_HAVE_LIBFFI
#include <ffi.h>
#endif

// ======================================================================
// Public header fallback (if includes/ffi.h is not present)
// ======================================================================
#ifndef VITTE_LIGHT_INCLUDES_FFI_H
#define VITTE_LIGHT_INCLUDES_FFI_H 1

typedef enum {
  VL_FFI_VOID = 0,
  VL_FFI_I8,
  VL_FFI_U8,
  VL_FFI_I16,
  VL_FFI_U16,
  VL_FFI_I32,
  VL_FFI_U32,
  VL_FFI_I64,
  VL_FFI_U64,
  VL_FFI_F32,
  VL_FFI_F64,
  VL_FFI_PTR
} VlFfiType;

typedef enum {
  VL_FFI_ABI_DEFAULT = 0,
  VL_FFI_ABI_SYSV,
  VL_FFI_ABI_UNIX64,
  VL_FFI_ABI_WIN64
} VlFfiAbi;

typedef struct VlFfiCif VlFfiCif;

// CIF lifecycle
AuxStatus vl_ffi_cif_new(VlFfiAbi abi, VlFfiType ret, const VlFfiType *args,
                         int nargs, VlFfiCif **out);
void vl_ffi_cif_free(VlFfiCif *cif);

// Call
AuxStatus vl_ffi_call(const VlFfiCif *cif, void *fn_ptr, void **argv,
                      void *retbuf /* sized for ret type, NULL if void */);

// Helpers
size_t vl_ffi_type_size(VlFfiType t);
size_t vl_ffi_type_align(VlFfiType t);

// Signature parsing: "ret(arg1, arg2, ...)"
// tokens: void,i8,u8,i16,u16,i32,u32,i64,u64,f32,f64,ptr
AuxStatus vl_ffi_parse_sig(const char *sig, VlFfiType *out_ret,
                           VlFfiType *out_args, int max_args, int *out_nargs);

// Dynamic loading helpers
AuxStatus vl_ffi_open_sym(const char *lib_stem_or_path, const char *symbol,
                          int dl_flags, VlDl **out_lib, void **out_sym);

#endif  // VITTE_LIGHT_INCLUDES_FFI_H

// ======================================================================
// Internal representation
// ======================================================================

struct VlFfiCif {
#ifdef VL_HAVE_LIBFFI
  ffi_cif cif;
  ffi_type **arg_types;  // array[nargs]
  ffi_type *ret_type;
  int nargs;
  VlFfiType v_ret;
  VlFfiType *v_args;  // array[nargs] for convenience
#else
  int nargs;
  VlFfiType v_ret;
  VlFfiType *v_args;
#endif
  VlFfiAbi abi;
};

// ----------------------------------------------------------------------
// Utilities
// ----------------------------------------------------------------------

static inline int min_int(int a, int b) { return a < b ? a : b; }

size_t vl_ffi_type_size(VlFfiType t) {
  switch (t) {
    case VL_FFI_VOID:
      return 0;
    case VL_FFI_I8:
    case VL_FFI_U8:
      return 1;
    case VL_FFI_I16:
    case VL_FFI_U16:
      return 2;
    case VL_FFI_I32:
    case VL_FFI_U32:
    case VL_FFI_F32:
      return 4;
    case VL_FFI_I64:
    case VL_FFI_U64:
    case VL_FFI_F64:
      return 8;
    case VL_FFI_PTR:
    default:
      return sizeof(void *);
  }
}

size_t vl_ffi_type_align(VlFfiType t) {
  // conservative equals size up to 8
  size_t s = vl_ffi_type_size(t);
  if (s == 0) return 1;
  if (s > alignof(max_align_t)) return alignof(max_align_t);
  return s;
}

static int tok_eq(const char *s, int n, const char *kw) {
  return (int)strlen(kw) == n && strncasecmp(s, kw, n) == 0;
}

AuxStatus vl_ffi_parse_sig(const char *sig, VlFfiType *out_ret,
                           VlFfiType *out_args, int max_args, int *out_nargs) {
  if (!sig || !out_ret || !out_args || max_args < 0 || !out_nargs)
    return AUX_EINVAL;
  const char *p = sig;
  while (isspace((unsigned char)*p)) p++;

  // parse type token
  const char *t0 = p;
  while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
  int n = (int)(p - t0);
  if (n <= 0) return AUX_EINVAL;

  VlFfiType ret = VL_FFI_VOID;
#define MAPTOK(name, TT)   \
  if (tok_eq(t0, n, name)) \
    ret = TT;              \
  else
  MAPTOK("void", VL_FFI_VOID)
  MAPTOK("i8", VL_FFI_I8)
  MAPTOK("u8", VL_FFI_U8) MAPTOK("i16", VL_FFI_I16) MAPTOK("u16", VL_FFI_U16)
      MAPTOK("i32", VL_FFI_I32) MAPTOK("u32", VL_FFI_U32)
          MAPTOK("i64", VL_FFI_I64) MAPTOK("u64", VL_FFI_U64)
              MAPTOK("f32", VL_FFI_F32) MAPTOK("f64", VL_FFI_F64)
                  MAPTOK("ptr", VL_FFI_PTR) {
    return AUX_EINVAL;
  }
#undef MAPTOK

  while (isspace((unsigned char)*p)) p++;
  if (*p != '(') return AUX_EINVAL;
  p++;  // skip '('

  int argc = 0;
  while (1) {
    while (isspace((unsigned char)*p)) p++;
    if (*p == ')') {
      p++;
      break;
    }
    if (argc >= max_args) return AUX_ERANGE;

    const char *a0 = p;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    int m = (int)(p - a0);
    if (m <= 0) return AUX_EINVAL;

    VlFfiType at = VL_FFI_VOID;
#define MAPA(name, TT)     \
  if (tok_eq(a0, m, name)) \
    at = TT;               \
  else
    MAPA("void", VL_FFI_VOID)  // only valid if first and only arg
    MAPA("i8", VL_FFI_I8)
    MAPA("u8", VL_FFI_U8) MAPA("i16", VL_FFI_I16) MAPA("u16", VL_FFI_U16)
        MAPA("i32", VL_FFI_I32) MAPA("u32", VL_FFI_U32) MAPA("i64", VL_FFI_I64)
            MAPA("u64", VL_FFI_U64) MAPA("f32", VL_FFI_F32)
                MAPA("f64", VL_FFI_F64) MAPA("ptr", VL_FFI_PTR) {
      return AUX_EINVAL;
    }
#undef MAPA

    if (at == VL_FFI_VOID) {
      // only allowed as the sole argument
      while (isspace((unsigned char)*p)) p++;
      if (*p != ')') return AUX_EINVAL;
      p++;
      argc = 0;
      break;
    }

    out_args[argc++] = at;

    while (isspace((unsigned char)*p)) p++;
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p == ')') {
      p++;
      break;
    }
    return AUX_EINVAL;
  }

  *out_ret = ret;
  *out_nargs = argc;
  return AUX_OK;
}

// ======================================================================
// libffi-backed implementation
// ======================================================================
#ifdef VL_HAVE_LIBFFI

static ffi_abi map_abi(VlFfiAbi abi) {
#if defined(_WIN32) && defined(_M_X64)
  (void)abi;
  return FFI_WIN64;
#else
  switch (abi) {
    case VL_FFI_ABI_SYSV:
      return FFI_SYSV;
#ifdef FFI_UNIX64
    case VL_FFI_ABI_UNIX64:
      return FFI_UNIX64;
#endif
#ifdef FFI_WIN64
    case VL_FFI_ABI_WIN64:
      return FFI_WIN64;
#endif
    case VL_FFI_ABI_DEFAULT:
    default:
      return FFI_DEFAULT_ABI;
  }
#endif
}

static ffi_type *map_type(VlFfiType t) {
  switch (t) {
    case VL_FFI_VOID:
      return &ffi_type_void;
    case VL_FFI_I8:
      return &ffi_type_sint8;
    case VL_FFI_U8:
      return &ffi_type_uint8;
    case VL_FFI_I16:
      return &ffi_type_sint16;
    case VL_FFI_U16:
      return &ffi_type_uint16;
    case VL_FFI_I32:
      return &ffi_type_sint32;
    case VL_FFI_U32:
      return &ffi_type_uint32;
    case VL_FFI_I64:
      return &ffi_type_sint64;
    case VL_FFI_U64:
      return &ffi_type_uint64;
    case VL_FFI_F32:
      return &ffi_type_float;
    case VL_FFI_F64:
      return &ffi_type_double;
    case VL_FFI_PTR:
      return &ffi_type_pointer;
    default:
      return NULL;
  }
}

AuxStatus vl_ffi_cif_new(VlFfiAbi abi, VlFfiType ret, const VlFfiType *args,
                         int nargs, VlFfiCif **out) {
  if (!out || nargs < 0) return AUX_EINVAL;
  *out = NULL;

  VlFfiCif *c = (VlFfiCif *)calloc(1, sizeof *c);
  if (!c) return AUX_ENOMEM;

  c->abi = abi;
  c->v_ret = ret;
  c->nargs = nargs;

  if (nargs > 0) {
    c->v_args = (VlFfiType *)malloc(sizeof(VlFfiType) * (size_t)nargs);
    c->arg_types = (ffi_type **)malloc(sizeof(ffi_type *) * (size_t)nargs);
    if (!c->v_args || !c->arg_types) {
      free(c->v_args);
      free(c->arg_types);
      free(c);
      return AUX_ENOMEM;
    }
    for (int i = 0; i < nargs; i++) {
      c->v_args[i] = args[i];
      c->arg_types[i] = map_type(args[i]);
      if (!c->arg_types[i]) {
        vl_ffi_cif_free(c);
        return AUX_EINVAL;
      }
    }
  }

  c->ret_type = map_type(ret);
  if (!c->ret_type) {
    vl_ffi_cif_free(c);
    return AUX_EINVAL;
  }

  ffi_status fs = ffi_prep_cif(&c->cif, map_abi(abi), (unsigned)nargs,
                               c->ret_type, c->arg_types);
  if (fs != FFI_OK) {
    vl_ffi_cif_free(c);
    return AUX_EIO;
  }

  *out = c;
  return AUX_OK;
}

void vl_ffi_cif_free(VlFfiCif *c) {
  if (!c) return;
  free(c->v_args);
  free(c->arg_types);
  free(c);
}

AuxStatus vl_ffi_call(const VlFfiCif *cif, void *fn_ptr, void **argv,
                      void *retbuf) {
  if (!cif || !fn_ptr) return AUX_EINVAL;
  if (cif->v_ret != VL_FFI_VOID && !retbuf) return AUX_EINVAL;
  ffi_call((ffi_cif *)&cif->cif, FFI_FN(fn_ptr), retbuf, argv);
  return AUX_OK;
}

#else  // !VL_HAVE_LIBFFI

AuxStatus vl_ffi_cif_new(VlFfiAbi abi, VlFfiType ret, const VlFfiType *args,
                         int nargs, VlFfiCif **out) {
  if (!out || nargs < 0) return AUX_EINVAL;
  VlFfiCif *c = (VlFfiCif *)calloc(1, sizeof *c);
  if (!c) return AUX_ENOMEM;
  c->abi = abi;
  c->v_ret = ret;
  c->nargs = nargs;
  if (nargs > 0) {
    c->v_args = (VlFfiType *)malloc(sizeof(VlFfiType) * (size_t)nargs);
    if (!c->v_args) {
      free(c);
      return AUX_ENOMEM;
    }
    memcpy(c->v_args, args, sizeof(VlFfiType) * (size_t)nargs);
  }
  *out = c;
  return AUX_OK;
}

void vl_ffi_cif_free(VlFfiCif *c) {
  if (!c) return;
  free(c->v_args);
  free(c);
}

AuxStatus vl_ffi_call(const VlFfiCif *cif, void *fn_ptr, void **argv,
                      void *retbuf) {
  (void)cif;
  (void)fn_ptr;
  (void)argv;
  (void)retbuf;
  return AUX_ENOSYS;
}

#endif  // VL_HAVE_LIBFFI

// ======================================================================
// Dynamic loading helpers
// ======================================================================

AuxStatus vl_ffi_open_sym(const char *lib_stem_or_path, const char *symbol,
                          int dl_flags, VlDl **out_lib, void **out_sym) {
  if (!lib_stem_or_path || !symbol || !out_lib || !out_sym) return AUX_EINVAL;
  *out_lib = NULL;
  *out_sym = NULL;

  VlDl *h = NULL;
  AuxStatus s;
  // Try exact path first, then platform-specific candidates
  s = vl_dl_open(lib_stem_or_path, dl_flags, &h);
  if (s != AUX_OK) {
    s = vl_dl_open_ext(lib_stem_or_path, dl_flags, &h);
    if (s != AUX_OK) return s;
  }
  void *sym = vl_dl_sym_ptr(h, symbol);
  if (!sym) {
    vl_dl_close(h);
    return AUX_EIO;
  }

  *out_lib = h;
  *out_sym = sym;
  return AUX_OK;
}

// ======================================================================
// End
// ======================================================================
