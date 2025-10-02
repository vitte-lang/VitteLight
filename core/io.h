/* ============================================================================
   io.h — API I/O portable (C17, MIT)
   - Lecture/écriture intégrale de fichiers (UTF-8 paths)
   - Append, lecture ligne par ligne (malloc), printf-like
   - Cross-platform (POSIX/Windows, UTF-16 conversion sur Win32)
   ============================================================================ */
#ifndef VT_IO_H
#define VT_IO_H
#pragma once

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VT_IO_API
#define VT_IO_API extern
#endif

/* ----------------------------------------------------------------------------
   API
---------------------------------------------------------------------------- */

/* Lire tout un fichier dans un buffer malloc (NUL-terminé).
   Retourne 0 si succès, -1 si erreur. */
VT_IO_API int vt_io_read_all(const char* path, char** out_buf, size_t* out_len);

/* Écrire entièrement un buffer dans un fichier (overwrite). */
VT_IO_API int vt_io_write_all(const char* path, const void* buf, size_t len);

/* Append au fichier (créé si absent). */
VT_IO_API int vt_io_append_all(const char* path, const void* buf, size_t len);

/* Lire une ligne (sans \n) depuis FILE*.
   Retourne malloc+NUL ou NULL si EOF. out_len optionnel. */
VT_IO_API char* vt_io_read_line(FILE* f, size_t* out_len);

/* fprintf wrappers. */
VT_IO_API void vt_io_writef(FILE* f, const char* fmt, ...);
VT_IO_API void vt_io_printf(const char* fmt, ...);
VT_IO_API void vt_io_eprintf(const char* fmt, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_IO_H */
