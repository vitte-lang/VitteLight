// SPDX-License-Identifier: GPL-3.0-or-later
//
// archive.c — TAR/ZIP bindings for Vitte Light VM (C17, complet)
// Namespace: "archive"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_LIBARCHIVE -c archive.c
//   cc ... archive.o -larchive
//
// Portabilité:
//   - Implémentation réelle si VL_HAVE_LIBARCHIVE et <archive.h> présents.
//   - Sinon: stubs -> (nil, "ENOSYS").
//
// Modèle:
//   - Reader: open -> next() itératif -> (extract_all|close)
//   - Writer: create(format) -> add_file()/add_dir() -> finish()
//
// Types d’entrée (type:int):
//   1=file, 2=dir, 3=symlink, 4=hardlink, 5=char, 6=block, 7=fifo, 8=socket
//
// API:
//
//   Lecture / Listing
//     archive.open(path) -> id | (nil, errmsg)
//     archive.next(id)
//         -> name:string, size:int64, mtime:int64, type:int | (nil,"eof") |
//         (nil,errmsg)
//     archive.extract_all(id, destdir[, strip_components=0]) -> count:int |
//     (nil,errmsg) archive.close(id) -> true
//
//   Écriture
//     archive.create(path[, format="tgz"[, level=-1]]) -> id | (nil,errmsg)
//       // formats: "tgz" (tar+gzip), "tar", "zip"
//     archive.add_file(id, src_path[, arcname[, mode:int[, mtime:int64]]]) ->
//     true | (nil,errmsg) archive.add_dir(id, arcname[, mode:int[,
//     mtime:int64]])               -> true | (nil,errmsg) archive.finish(id) ->
//     true | (nil,errmsg)
//
// Notes:
//   - extract_all applique strip_components et neutralise ".." / chemins
//   absolus.
//   - add_file lit le fichier par blocs et stream vers l’archive.
//   - mtime en secondes UNIX. mode optionnel (ex: 0644, 0755).
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef _WIN32
#include <io.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define PATH_SEP '/'
#endif

#ifdef VL_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *ar_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t ar_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int ar_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int ar_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)ar_check_int(S, idx);
  return defv;
}

// ---------------------------------------------------------------------
// Stubs si libarchive absente
// ---------------------------------------------------------------------
#ifndef VL_HAVE_LIBARCHIVE

#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)
static int vlarch_open(VL_State *S) {
  (void)ar_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlarch_next(VL_State *S) { NOSYS_PAIR(S); }
static int vlarch_extract_all(VL_State *S) { NOSYS_PAIR(S); }
static int vlarch_close(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}
static int vlarch_create(VL_State *S) { NOSYS_PAIR(S); }
static int vlarch_add_file(VL_State *S) { NOSYS_PAIR(S); }
static int vlarch_add_dir(VL_State *S) { NOSYS_PAIR(S); }
static int vlarch_finish(VL_State *S) { NOSYS_PAIR(S); }

#else
// ---------------------------------------------------------------------
// Implémentation réelle (libarchive)
// ---------------------------------------------------------------------

typedef enum { H_UNUSED = 0, H_READER = 1, H_WRITER = 2 } HandleKind;

typedef struct ArcHandle {
  int used;
  HandleKind kind;
  struct archive *a;        // reader or writer
  struct archive *aw_disk;  // for extract (writer-disk), otherwise NULL
  int finished;             // writer finished
} ArcHandle;

static ArcHandle *g_h = NULL;
static int g_h_cap = 0;

static int ensure_h_cap(int need) {
  if (need <= g_h_cap) return 1;
  int ncap = g_h_cap ? g_h_cap : 8;
  while (ncap < need) ncap <<= 1;
  ArcHandle *nh = (ArcHandle *)realloc(g_h, (size_t)ncap * sizeof *nh);
  if (!nh) return 0;
  for (int i = g_h_cap; i < ncap; i++) {
    nh[i].used = 0;
    nh[i].kind = H_UNUSED;
    nh[i].a = NULL;
    nh[i].aw_disk = NULL;
    nh[i].finished = 0;
  }
  g_h = nh;
  g_h_cap = ncap;
  return 1;
}
static int alloc_h_slot(void) {
  for (int i = 1; i < g_h_cap; i++)
    if (!g_h[i].used) return i;
  if (!ensure_h_cap(g_h_cap ? g_h_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_h_cap; i++)
    if (!g_h[i].used) return i;
  return 0;
}
static int check_id(int id) {
  return id > 0 && id < g_h_cap && g_h[id].used && g_h[id].a;
}

static int push_aerr(VL_State *S, struct archive *a, const char *fallback) {
  const char *m = a ? archive_error_string(a) : NULL;
  vl_push_nil(S);
  vl_push_string(S, (m && *m) ? m : (fallback ? fallback : "EIO"));
  return 2;
}

// Sanitize path inside archive: remove drive letter, leading '/', and ".."
static void sanitize_arcname(const char *in, AuxBuffer *out,
                             int strip_components) {
  aux_buffer_reset(out);
  if (!in) return;
  const char *p = in;

  // Remove Windows drive prefix "C:\"
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':' && (p[2] == '/' || p[2] == '\\'))
    p += 3;
  // Remove leading slashes
  while (*p == '/' || *p == '\\') p++;

  // Build normalized path
  const char *seg = p;
  int skipped = 0;
  while (*seg) {
    // find next sep
    const char *q = seg;
    while (*q && *q != '/' && *q != '\\') q++;
    size_t L = (size_t)(q - seg);
    if (L == 0) {
      if (*q) {
        seg = q + 1;
        continue;
      } else
        break;
    }

    if (L == 1 && seg[0] == '.') {
      // skip
    } else if ((L == 2 && seg[0] == '.' && seg[1] == '.')) {
      // skip traversal
    } else {
      if (skipped >= strip_components) {
        if (out->len && out->data[out->len - 1] != (uint8_t)'/')
          aux_buffer_append(out, (const uint8_t *)"/", 1);
        aux_buffer_append(out, (const uint8_t *)seg, L);
      } else {
        skipped++;
      }
    }
    seg = *q ? q + 1 : q;
  }
  // Remove any leading slash accidentally added
  if (out->len && out->data[0] == (uint8_t)'/') {
    memmove(out->data, out->data + 1, out->len - 1);
    out->len--;
  }
}

// Map AE_IF* to small ints
static int map_filetype(mode_t m) {
  switch (m & AE_IFMT) {
    case AE_IFREG:
      return 1;
    case AE_IFDIR:
      return 2;
    case AE_IFLNK:
      return 3;
    case AE_IFSOCK:
      return 8;
    case AE_IFCHR:
      return 5;
    case AE_IFBLK:
      return 6;
    case AE_IFIFO:
      return 7;
    default:
      return 1;
  }
}

// ---------- Reader ----------

static int vlarch_open(VL_State *S) {
  const char *path = ar_check_str(S, 1);
  int id = alloc_h_slot();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  struct archive *a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);
  int rc = archive_read_open_filename(a, path, 10240);
  if (rc != ARCHIVE_OK) {
    int ret = push_aerr(S, a, "open");
    archive_read_free(a);
    return ret;
  }

  g_h[id].used = 1;
  g_h[id].kind = H_READER;
  g_h[id].a = a;
  g_h[id].aw_disk = NULL;
  g_h[id].finished = 0;
  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlarch_next(VL_State *S) {
  int id = (int)ar_check_int(S, 1);
  if (!check_id(id) || g_h[id].kind != H_READER) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  struct archive_entry *ent = NULL;
  int rc = archive_read_next_header(g_h[id].a, &ent);
  if (rc == ARCHIVE_EOF) {
    vl_push_nil(S);
    vl_push_string(S, "eof");
    return 2;
  }
  if (rc != ARCHIVE_OK) return push_aerr(S, g_h[id].a, "next");

  const char *name = archive_entry_pathname(ent);
  int64_t size = (int64_t)archive_entry_size(ent);
  int64_t mtime = (int64_t)archive_entry_mtime(ent);
  int type = map_filetype(archive_entry_filetype(ent));

  vl_push_string(S, name ? name : "");
  vl_push_int(S, size);
  vl_push_int(S, mtime);
  vl_push_int(S, (int64_t)type);
  return 4;
}

static int vlarch_extract_all(VL_State *S) {
  int id = (int)ar_check_int(S, 1);
  const char *dest = ar_check_str(S, 2);
  int strip = ar_opt_int(S, 3, 0);
  if (!check_id(id) || g_h[id].kind != H_READER) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  // Writer-disk
  struct archive *ad = archive_write_disk_new();
  archive_write_disk_set_options(
      ad, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL |
              ARCHIVE_EXTRACT_FFLAGS | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
              ARCHIVE_EXTRACT_SECURE_SYMLINKS |
              ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS);
  archive_write_disk_set_standard_lookup(ad);

  struct archive *a = g_h[id].a;
  struct archive_entry *ent;
  int64_t count = 0;
  AuxBuffer norm = {0};

  // Iterate entries from current position to EOF
  int rc;
  while ((rc = archive_read_next_header(a, &ent)) == ARCHIVE_OK) {
    const char *name = archive_entry_pathname(ent);
    sanitize_arcname(name ? name : "", &norm, strip);
    if (norm.len == 0) {  // fully stripped -> skip
      archive_read_data_skip(a);
      continue;
    }

    // Prefix with dest/
    AuxBuffer full = {0};
    aux_buffer_append(&full, (const uint8_t *)dest, strlen(dest));
    if (full.len && full.data[full.len - 1] != (uint8_t)PATH_SEP) {
      const char s = PATH_SEP;
      aux_buffer_append(&full, (const uint8_t *)&s, 1);
    }
    aux_buffer_append(&full, norm.data, norm.len);

    // Create a shadow entry with adjusted path
    struct archive_entry *out = archive_entry_clone(ent);
    archive_entry_set_pathname(out, (const char *)full.data);

    // Write header
    int w = archive_write_header(ad, out);
    if (w != ARCHIVE_OK) {
      archive_entry_free(out);
      aux_buffer_free(&full);
      aux_buffer_free(&norm);
      int ret = push_aerr(S, ad, "write_header");
      archive_write_free(ad);
      return ret;
    }

    // Copy data if regular file
    if (archive_entry_size(out) > 0) {
      const size_t BUF = 64 * 1024;
      void *buf = malloc(BUF);
      if (!buf) {
        archive_entry_free(out);
        aux_buffer_free(&full);
        aux_buffer_free(&norm);
        archive_write_free(ad);
        vl_push_nil(S);
        vl_push_string(S, "ENOMEM");
        return 2;
      }
      ssize_t r;
      while ((r = archive_read_data(a, buf, BUF)) > 0) {
        if (archive_write_data(ad, buf, (size_t)r) < 0) {
          free(buf);
          archive_entry_free(out);
          aux_buffer_free(&full);
          aux_buffer_free(&norm);
          int ret = push_aerr(S, ad, "write_data");
          archive_write_free(ad);
          return ret;
        }
      }
      free(buf);
      if (r < 0) {
        archive_entry_free(out);
        aux_buffer_free(&full);
        aux_buffer_free(&norm);
        int ret = push_aerr(S, a, "read_data");
        archive_write_free(ad);
        return ret;
      }
    }
    archive_entry_free(out);
    count++;
    // Finish entry
    archive_write_finish_entry(ad);
  }
  aux_buffer_free(&norm);
  archive_write_free(ad);

  if (rc != ARCHIVE_EOF) return push_aerr(S, a, "read");
  vl_push_int(S, count);
  return 1;
}

static int vlarch_close(VL_State *S) {
  int id = (int)ar_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_bool(S, 1);
    return 1;
  }
  if (g_h[id].kind == H_READER) {
    archive_read_close(g_h[id].a);
    archive_read_free(g_h[id].a);
  } else if (g_h[id].kind == H_WRITER) {
    if (!g_h[id].finished) archive_write_close(g_h[id].a);
    archive_write_free(g_h[id].a);
  }
  if (g_h[id].aw_disk) {
    archive_write_free(g_h[id].aw_disk);
    g_h[id].aw_disk = NULL;
  }
  g_h[id].a = NULL;
  g_h[id].used = 0;
  g_h[id].kind = H_UNUSED;
  g_h[id].finished = 0;
  vl_push_bool(S, 1);
  return 1;
}

// ---------- Writer ----------

static int set_writer_format(struct archive *a, const char *fmt, int level) {
  if (!fmt || strcmp(fmt, "tgz") == 0 || strcmp(fmt, "tar.gz") == 0) {
    if (archive_write_add_filter_gzip(a) != ARCHIVE_OK) return -1;
    if (level >= 0 && level <= 9)
      archive_write_set_filter_option(a, "gzip", "compression-level",
                                      (char[]){(char)('0' + level), 0});
    if (archive_write_set_format_pax_restricted(a) != ARCHIVE_OK) return -1;
    return 0;
  } else if (strcmp(fmt, "tar") == 0) {
    if (archive_write_set_format_pax_restricted(a) != ARCHIVE_OK) return -1;
    return 0;
  } else if (strcmp(fmt, "zip") == 0) {
    if (archive_write_set_format_zip(a) != ARCHIVE_OK) return -1;
    // gzip filter not used for zip; libarchive handles deflate internally
    if (level >= 0 && level <= 9)
      archive_write_set_format_option(a, "zip", "compression-level",
                                      (char[]){(char)('0' + level), 0});
    return 0;
  }
  return -1;
}

static int vlarch_create(VL_State *S) {
  const char *path = ar_check_str(S, 1);
  const char *fmt =
      (vl_get(S, 2) && vl_isstring(S, 2)) ? ar_check_str(S, 2) : "tgz";
  int level = ar_opt_int(S, 3, -1);

  int id = alloc_h_slot();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  struct archive *a = archive_write_new();
  if (set_writer_format(a, fmt, level) != 0) {
    archive_write_free(a);
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  if (archive_write_open_filename(a, path) != ARCHIVE_OK) {
    int ret = push_aerr(S, a, "open");
    archive_write_free(a);
    return ret;
  }

  g_h[id].used = 1;
  g_h[id].kind = H_WRITER;
  g_h[id].a = a;
  g_h[id].aw_disk = NULL;
  g_h[id].finished = 0;
  vl_push_int(S, (int64_t)id);
  return 1;
}

static int write_entry_data_from_file(struct archive *a, FILE *fp) {
  char buf[64 * 1024];
  size_t r;
  while ((r = fread(buf, 1, sizeof buf, fp)) > 0) {
    if (archive_write_data(a, buf, r) < 0) return -1;
  }
  if (ferror(fp)) return -1;
  return 0;
}

// archive.add_file(id, src_path[, arcname[, mode[, mtime]]])
static int vlarch_add_file(VL_State *S) {
  int id = (int)ar_check_int(S, 1);
  const char *src = ar_check_str(S, 2);
  const char *arcname_in =
      (vl_get(S, 3) && vl_isstring(S, 3)) ? ar_check_str(S, 3) : src;
  int mode_in = (vl_get(S, 4) ? (int)ar_check_int(S, 4) : -1);
  int64_t mtime_in = (vl_get(S, 5) ? ar_check_int(S, 5) : -1);

  if (!check_id(id) || g_h[id].kind != H_WRITER || g_h[id].finished) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  FILE *fp = fopen(src, "rb");
  if (!fp) {
    vl_push_nil(S);
    vl_push_string(S, "ENOENT");
    return 2;
  }

  struct stat st;
  memset(&st, 0, sizeof st);
  if (stat(src, &st) != 0) {
    fclose(fp);
    vl_push_nil(S);
    vl_push_string(S, "stat");
    return 2;
  }

  // sanitize arcname (no strip here)
  AuxBuffer norm = {0};
  sanitize_arcname(arcname_in, &norm, 0);

  struct archive_entry *e = archive_entry_new();
  archive_entry_set_pathname(e, (const char *)norm.data);
  archive_entry_set_size(e, (la_int64_t)st.st_size);
  archive_entry_set_filetype(e, AE_IFREG);
  archive_entry_set_perm(e, mode_in >= 0 ? mode_in : (st.st_mode & 0777));
  if (mtime_in >= 0) archive_entry_set_mtime(e, (time_t)mtime_in, 0);
#ifdef st_mtim
  else
    archive_entry_set_mtime(e, st.st_mtime, 0);
#else
  else
    archive_entry_set_mtime(e, st.st_mtime, 0);
#endif

  if (archive_write_header(g_h[id].a, e) != ARCHIVE_OK) {
    archive_entry_free(e);
    aux_buffer_free(&norm);
    fclose(fp);
    return push_aerr(S, g_h[id].a, "write_header");
  }
  if (write_entry_data_from_file(g_h[id].a, fp) != 0) {
    archive_entry_free(e);
    aux_buffer_free(&norm);
    fclose(fp);
    return push_aerr(S, g_h[id].a, "write_data");
  }
  fclose(fp);
  archive_entry_free(e);
  aux_buffer_free(&norm);
  vl_push_bool(S, 1);
  return 1;
}

// archive.add_dir(id, arcname[, mode[, mtime]])
static int vlarch_add_dir(VL_State *S) {
  int id = (int)ar_check_int(S, 1);
  const char *arcname_in = ar_check_str(S, 2);
  int mode_in = (vl_get(S, 3) ? (int)ar_check_int(S, 3) : 0755);
  int64_t mtime_in = (vl_get(S, 4) ? ar_check_int(S, 4) : -1);
  if (!check_id(id) || g_h[id].kind != H_WRITER || g_h[id].finished) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  AuxBuffer norm = {0};
  sanitize_arcname(arcname_in, &norm, 0);
  // ensure trailing '/'
  if (!norm.len || norm.data[norm.len - 1] != (uint8_t)'/') {
    const char slash = '/';
    aux_buffer_append(&norm, (const uint8_t *)&slash, 1);
  }

  struct archive_entry *e = archive_entry_new();
  archive_entry_set_pathname(e, (const char *)norm.data);
  archive_entry_set_filetype(e, AE_IFDIR);
  archive_entry_set_perm(e, mode_in & 0777);
  if (mtime_in >= 0) archive_entry_set_mtime(e, (time_t)mtime_in, 0);

  if (archive_write_header(g_h[id].a, e) != ARCHIVE_OK) {
    archive_entry_free(e);
    aux_buffer_free(&norm);
    return push_aerr(S, g_h[id].a, "write_header");
  }
  archive_entry_free(e);
  aux_buffer_free(&norm);
  vl_push_bool(S, 1);
  return 1;
}

static int vlarch_finish(VL_State *S) {
  int id = (int)ar_check_int(S, 1);
  if (!check_id(id) || g_h[id].kind != H_WRITER) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  if (g_h[id].finished) {
    vl_push_bool(S, 1);
    return 1;
  }
  if (archive_write_close(g_h[id].a) != ARCHIVE_OK)
    return push_aerr(S, g_h[id].a, "close");
  g_h[id].finished = 1;
  vl_push_bool(S, 1);
  return 1;
}

#endif  // VL_HAVE_LIBARCHIVE

// ---------------------------------------------------------------------
// Registration VM
// ---------------------------------------------------------------------
static const VL_Reg archlib[] = {
    // Reader
    {"open", vlarch_open},
    {"next", vlarch_next},
    {"extract_all", vlarch_extract_all},
    {"close", vlarch_close},

    // Writer
    {"create", vlarch_create},
    {"add_file", vlarch_add_file},
    {"add_dir", vlarch_add_dir},
    {"finish", vlarch_finish},

    {NULL, NULL}};

void vl_open_archivelib(VL_State *S) { vl_register_lib(S, "archive", archlib); }
