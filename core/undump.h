/* ============================================================================
   undump.h — Chargeur d’images/bytecode VT (C17, portable)
   Format conteneur "VTBC" (LE). Vérifs: bornes, CRC-32, tailles.
   API opaque: vt_img*. Wrappers handle void* fournis.
   Licence: MIT.
   ============================================================================
 */
#ifndef VT_UNDUMP_H
#define VT_UNDUMP_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint8_t, uint64_t */
#include <stdio.h>  /* FILE */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export
---------------------------------------------------------------------------- */
#ifndef VT_UNDUMP_API
#define VT_UNDUMP_API extern
#endif

/* ----------------------------------------------------------------------------
   Types opaques et itérateurs
---------------------------------------------------------------------------- */
typedef struct vt_img vt_img; /* opaque */

typedef struct vt_strs_it {
  const uint8_t* cur;
  const uint8_t* end;
} vt_strs_it;

/* ----------------------------------------------------------------------------
   Chargement / libération
   Notes de retour: 0 = OK, <0 = -errno/-E* (style POSIX).
---------------------------------------------------------------------------- */
VT_UNDUMP_API int vt_img_load_memory(const void* data, size_t size,
                                     int copy, /* 0: map, 1: copie */
                                     vt_img** out_img);

VT_UNDUMP_API int vt_img_load_file(const char* path, vt_img** out_img);
VT_UNDUMP_API void vt_img_release(vt_img* img);

/* ----------------------------------------------------------------------------
   Accès sections et infos
   tag4: tableau de 4 chars, ex: (const char[4]){'C','O','D','E'}
---------------------------------------------------------------------------- */
VT_UNDUMP_API int vt_img_find(const vt_img* img, const char tag4[4],
                              const uint8_t** out_ptr, size_t* out_sz);

VT_UNDUMP_API void vt_img_info(const vt_img* img, FILE* out /* NULL=stderr */);

/* ----------------------------------------------------------------------------
   STRS iterator (concat de chaînes NUL-terminées)
---------------------------------------------------------------------------- */
VT_UNDUMP_API void vt_strs_begin(const vt_img* img, vt_strs_it* it);
VT_UNDUMP_API const char* vt_strs_next(vt_strs_it* it);

/* ----------------------------------------------------------------------------
   Wrappers "handle" (void*) pour intégrations dynamiques simples
   Équivalents de vt_img_* ci-dessus.
---------------------------------------------------------------------------- */
VT_UNDUMP_API int vt_undump_load_file(const char* path, void** out_handle);
VT_UNDUMP_API void vt_undump_release(void* handle);
VT_UNDUMP_API int vt_undump_find(void* handle, const char tag4[4],
                                 const void** out_ptr, size_t* out_sz);
VT_UNDUMP_API void vt_undump_info(void* handle, FILE* out /* NULL=stderr */);

/* ----------------------------------------------------------------------------
   Aide: tags connus (exemples d’appel)
   vt_img_find(img, (const char[4]){'C','O','D','E'}, &p, &n);
   vt_img_find(img, (const char[4]){'K','C','O','N'}, &p, &n);
   vt_img_find(img, (const char[4]){'S','T','R','S'}, &p, &n);
   vt_img_find(img, (const char[4]){'S','Y','M','S'}, &p, &n);
   vt_img_find(img, (const char[4]){'F','U','N','C'}, &p, &n);
   vt_img_find(img, (const char[4]){'D','B','G','\0'}, &p, &n);
---------------------------------------------------------------------------- */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_UNDUMP_H */
