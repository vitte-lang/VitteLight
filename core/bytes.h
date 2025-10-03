// SPDX-License-Identifier: MIT
/* ============================================================================
   core/bytes.h — En-tête C11 pour utilitaires « ByteBuf » et « ByteRO »
   Buffers binaires, lecture/écriture scalaires, varints, hex, base64.
   ============================================================================
 */
#ifndef CORE_BYTES_H
#define CORE_BYTES_H

#if defined(_MSC_VER)
#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 201112L
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>  /* FILE* */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Export                                                                     */
/* -------------------------------------------------------------------------- */
#if !defined(API_EXPORT)
# if defined(_WIN32)
#  define API_EXPORT __declspec(dllexport)
# else
#  if defined(__GNUC__) || defined(__clang__)
#   define API_EXPORT __attribute__((visibility("default")))
#  else
#   define API_EXPORT
#  endif
# endif
#endif

/* -------------------------------------------------------------------------- */
/* Types scalaires                                                            */
/* -------------------------------------------------------------------------- */
typedef uint8_t   u8;
typedef int8_t    i8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int64_t   i64;
typedef size_t    usize;
typedef ptrdiff_t isize;

/* -------------------------------------------------------------------------- */
/* Structures de base                                                         */
/* -------------------------------------------------------------------------- */
typedef struct {
  u8*   data;
  usize len;
  usize cap;
  usize pos; /* position courante */
} ByteBuf;

typedef struct {
  const u8* data;
  usize     len;
  usize     pos;
} ByteRO;

#define BYTES_STRUCTS_DEFINED 1

/* -------------------------------------------------------------------------- */
/* Gestion ByteBuf                                                            */
/* -------------------------------------------------------------------------- */
API_EXPORT void   bb_init(ByteBuf* b);
API_EXPORT void   bb_free(ByteBuf* b);
API_EXPORT void   bb_clear(ByteBuf* b);
API_EXPORT void   bb_reserve(ByteBuf* b, usize need);
API_EXPORT void   bb_resize(ByteBuf* b, usize n);
API_EXPORT u8*    bb_mut_data(ByteBuf* b);
API_EXPORT const u8* bb_data(const ByteBuf* b);
API_EXPORT usize  bb_len(const ByteBuf* b);
API_EXPORT usize  bb_cap(const ByteBuf* b);
API_EXPORT usize  bb_pos(const ByteBuf* b);
API_EXPORT void   bb_seek(ByteBuf* b, usize pos);
API_EXPORT void   bb_write(ByteBuf* b, const void* src, usize n);
API_EXPORT usize  bb_append(ByteBuf* b, const void* src, usize n);
API_EXPORT u8*    bb_alloc_tail(ByteBuf* b, usize n);

/* -------------------------------------------------------------------------- */
/* Écritures scalaires                                                        */
/* -------------------------------------------------------------------------- */
API_EXPORT void bb_w_u8 (ByteBuf* b, u8 v);
API_EXPORT void bb_w_i8 (ByteBuf* b, i8 v);
API_EXPORT void bb_w_le16(ByteBuf* b, u16 v);
API_EXPORT void bb_w_le32(ByteBuf* b, u32 v);
API_EXPORT void bb_w_le64(ByteBuf* b, u64 v);
API_EXPORT void bb_w_be16(ByteBuf* b, u16 v);
API_EXPORT void bb_w_be32(ByteBuf* b, u32 v);
API_EXPORT void bb_w_be64(ByteBuf* b, u64 v);
API_EXPORT void bb_w_f32_le(ByteBuf* b, float f);
API_EXPORT void bb_w_f64_le(ByteBuf* b, double d);
API_EXPORT void bb_w_f32_be(ByteBuf* b, float f);
API_EXPORT void bb_w_f64_be(ByteBuf* b, double d);
API_EXPORT void bb_w_varu(ByteBuf* b, u64 v);
API_EXPORT void bb_w_vari(ByteBuf* b, i64 v);
API_EXPORT void bb_w_str (ByteBuf* b, const char* s);

/* -------------------------------------------------------------------------- */
/* Lecture ByteRO                                                             */
/* -------------------------------------------------------------------------- */
API_EXPORT void   br_init(ByteRO* r, const void* data, usize n);
API_EXPORT usize  br_remaining(const ByteRO* r);
API_EXPORT const u8* br_peek(const ByteRO* r, usize* avail);
API_EXPORT bool   br_skip(ByteRO* r, usize n);
API_EXPORT bool   br_read(ByteRO* r, void* dst, usize n);

API_EXPORT bool br_r_u8 (ByteRO* r, u8* v);
API_EXPORT bool br_r_i8 (ByteRO* r, i8* v);
API_EXPORT bool br_r_le16(ByteRO* r, u16* v);
API_EXPORT bool br_r_le32(ByteRO* r, u32* v);
API_EXPORT bool br_r_le64(ByteRO* r, u64* v);
API_EXPORT bool br_r_be16(ByteRO* r, u16* v);
API_EXPORT bool br_r_be32(ByteRO* r, u32* v);
API_EXPORT bool br_r_be64(ByteRO* r, u64* v);

API_EXPORT bool br_r_f32_le(ByteRO* r, float* f);
API_EXPORT bool br_r_f64_le(ByteRO* r, double* d);
API_EXPORT bool br_r_f32_be(ByteRO* r, float* f);
API_EXPORT bool br_r_f64_be(ByteRO* r, double* d);

API_EXPORT bool br_r_varu(ByteRO* r, u64* out);
API_EXPORT bool br_r_vari(ByteRO* r, i64* out);
API_EXPORT bool br_r_str (ByteRO* r, const char** s, usize* n);

/* -------------------------------------------------------------------------- */
/* Hex / Base64                                                               */
/* -------------------------------------------------------------------------- */
API_EXPORT void hex_encode(const void* data, usize n, bool upper, ByteBuf* out);
API_EXPORT bool hex_decode(const char* s, usize n, ByteBuf* out);
API_EXPORT void hexdump(FILE* fp, const void* data, usize n, usize base_addr);

API_EXPORT void b64_encode(const void* data, usize n, ByteBuf* out);
API_EXPORT bool b64_decode(const char* s, usize n, ByteBuf* out);

/* -------------------------------------------------------------------------- */
/* Slices / recherche                                                         */
/* -------------------------------------------------------------------------- */
API_EXPORT ByteRO bb_view(const ByteBuf* b, usize off, usize n);
API_EXPORT isize bytes_find(const u8* hay, usize hay_n, const u8* needle, usize nee_n);
API_EXPORT bool  bytes_eq(const u8* a, const u8* b, usize n);

/* -------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CORE_BYTES_H */
