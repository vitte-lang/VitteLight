/* ============================================================================
   fs.h — API fichiers/système cross-platform (C11, UTF-8)
   - Existence, stat, lecture/écriture intégrale, copie, déplacement
   - mkdir -p, suppression récursive, itération de répertoire
   - Gestion chemins: join, norm, basename, dirname
   - Répertoires: cwd, temp, home
   Licence: MIT
   ============================================================================
*/
#ifndef VT_FS_H
#define VT_FS_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint64_t, uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export
---------------------------------------------------------------------------- */
#ifndef VT_FS_API
#define VT_FS_API extern
#endif

/* ----------------------------------------------------------------------------
   Structures
---------------------------------------------------------------------------- */
typedef struct {
  uint64_t size;      /* taille en octets */
  uint64_t mtime_s;   /* epoch seconds (meilleure précision possible) */
  int is_file;
  int is_dir;
  uint32_t mode;      /* bits st_mode POSIX si dispo, sinon 0 */
} vt_fs_stat;

/* callback itération de répertoire */
typedef int (*vt_fs_dir_cb)(const char* path_utf8, const char* name_utf8,
                            int is_dir, void* user);

/* ----------------------------------------------------------------------------
   Fonctions de base
---------------------------------------------------------------------------- */
VT_FS_API int  vt_fs_exists(const char* path);
VT_FS_API int  vt_fs_is_file(const char* path);
VT_FS_API int  vt_fs_is_dir(const char* path);
VT_FS_API int  vt_fs_stat_path(const char* path, vt_fs_stat* st);

/* ----------------------------------------------------------------------------
   Création/suppression
---------------------------------------------------------------------------- */
VT_FS_API int  vt_fs_mkdir(const char* path);          /* 0/-1 */
VT_FS_API int  vt_fs_mkdirs(const char* path);         /* mkdir -p */
VT_FS_API int  vt_fs_remove_file(const char* path);    /* 0/-1 */
VT_FS_API int  vt_fs_rmdir(const char* path);          /* 0/-1 (vide uniquement) */
VT_FS_API int  vt_fs_remove_all(const char* path);     /* récursif */

/* ----------------------------------------------------------------------------
   Lecture/écriture intégrales
---------------------------------------------------------------------------- */
VT_FS_API int  vt_fs_read_all(const char* path, char** out_buf, size_t* out_len);
/* lit le fichier entier dans un buffer malloc() + NUL. Caller free() */

VT_FS_API int  vt_fs_write_all(const char* path, const void* data, size_t len);
/* écrase le fichier (créé si absent) */

/* ----------------------------------------------------------------------------
   Copie/déplacement
---------------------------------------------------------------------------- */
VT_FS_API int  vt_fs_copy_file(const char* src, const char* dst, int overwrite);
VT_FS_API int  vt_fs_move(const char* src, const char* dst, int overwrite);

/* ----------------------------------------------------------------------------
   Itération de répertoire
---------------------------------------------------------------------------- */
VT_FS_API int  vt_fs_iterdir(const char* dir, vt_fs_dir_cb cb, void* user);

/* ----------------------------------------------------------------------------
   Répertoires spéciaux
---------------------------------------------------------------------------- */
VT_FS_API int  vt_fs_cwd(char* out, size_t cap);
VT_FS_API int  vt_fs_chdir(const char* path);
VT_FS_API int  vt_fs_tempdir(char* out, size_t cap);
VT_FS_API int  vt_fs_homedir(char* out, size_t cap);

/* ----------------------------------------------------------------------------
   Chemins
---------------------------------------------------------------------------- */
VT_FS_API void vt_fs_path_join(char* out, size_t cap, const char* a, const char* b);
VT_FS_API void vt_fs_path_norm(char* io, int to_posix_sep);
VT_FS_API const char* vt_fs_basename(const char* path);
VT_FS_API void vt_fs_dirname(const char* path, char* out, size_t cap);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_FS_H */
