// SPDX-License-Identifier: GPL-3.0-or-later
//
// iolib.c — I/O standard library for Vitte Light (C17, portable)
// ------------------------------------------------------------------
// Espace de noms: "io"
//
// Fonctions (orientées fichiers et chemins, sans userdata):
//   io.read(path)                  -> string | (nil, errmsg)
//   io.write(path, data, [mkdirs]) -> bool | (nil, errmsg)
//   io.append(path, data)          -> bool | (nil, errmsg)
//   io.exists(path)                -> bool
//   io.is_file(path)               -> bool
//   io.is_dir(path)                -> bool
//   io.remove(path)                -> bool | (nil, errmsg)
//   io.rename(old, new)            -> bool | (nil, errmsg)
//   io.mkdirs(path)                -> bool | (nil, errmsg)
//   io.listdir(path)               -> names_string, count    // noms séparés
//   par '\n' io.cwd()                       -> string | (nil, errmsg)
//   io.stat(path)                  -> size:int, mtime:int64, atime:int64 |
//   (nil, errmsg) io.read_stdin()                -> string | (nil, errmsg)
//   io.write_stdout(data)          -> bytes_written:int | (nil, errmsg)
//   io.write_stderr(data)          -> bytes_written:int | (nil, errmsg)
//
// Dépendances:
//   - includes/auxlib.h  (AuxStatus, AuxBuffer, helpers fichiers/chemins)
//   - includes/state.h, includes/object.h, includes/vm.h (API VM style Lua)
//
// Notes:
//   - Pas d’objets "fichier" (userdata) ici pour rester API-minimale.
//   - io.listdir retourne deux valeurs: une chaîne avec un nom par ligne, puis
//   le nombre d’entrées.
//   - io.stat retourne 3 valeurs numériques (size, mtime, atime) ou (nil,
//   errmsg).
//   - Toutes les écritures sont binaires. Les lectures renvoient des octets
//   sous forme de string VM.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// ------------------------------------------------------------------
// Helpers VM: lecture d'arguments
// ------------------------------------------------------------------

static const char *vli_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}

static int vli_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  VL_Value *v = vl_get(S, idx);
  return vl_tobool(v) ? 1 : 0;
}

// ------------------------------------------------------------------
// io.read(path)
// ------------------------------------------------------------------

static int vli_read(VL_State *S) {
  const char *path = vli_check_str(S, 1);
  AuxBuffer buf = {0};
  AuxStatus st = aux_read_file(path, &buf);
  if (st != AUX_OK) {
    vl_push_nil(S);
    vl_push_string(S, aux_status_str(st));
    return 2;
  }
  // buf.data is zero-terminated by aux_read_file
  vl_push_lstring(S, (const char *)buf.data, (int)buf.len);
  aux_buffer_free(&buf);
  return 1;
}

// ------------------------------------------------------------------
// io.write(path, data, [mkdirs=false])
// ------------------------------------------------------------------

static int vli_write(VL_State *S) {
  const char *path = vli_check_str(S, 1);
  const char *data = vli_check_str(S, 2);
  int mkdirs = vli_opt_bool(S, 3, 0);

  size_t len = strlen(data);
  AuxStatus st = aux_write_file(path, data, len, mkdirs);
  if (st != AUX_OK) {
    vl_push_nil(S);
    vl_push_string(S, aux_status_str(st));
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

// ------------------------------------------------------------------
// io.append(path, data)
// ------------------------------------------------------------------

static int vli_append(VL_State *S) {
  const char *path = vli_check_str(S, 1);
  const char *data = vli_check_str(S, 2);
  // open append-binary
  FILE *f = fopen(path, "ab");
  if (!f) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  size_t len = strlen(data);
  size_t wr = len ? fwrite(data, 1, len, f) : 0;
  int ok = (fclose(f) == 0) && (wr == len);
  if (!ok) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

// ------------------------------------------------------------------
// io.exists / io.is_file / io.is_dir
// ------------------------------------------------------------------

static int vli_exists(VL_State *S) {
  const char *path = vli_check_str(S, 1);
  vl_push_bool(S, aux_path_exists(path));
  return 1;
}

static int vli_is_file(VL_State *S) {
  const char *path = vli_check_str(S, 1);
  vl_push_bool(S, aux_is_file(path));
  return 1;
}

static int vli_is_dir(VL_State *S) {
  const char *path = vli_check_str(S, 1);
  vl_push_bool(S, aux_is_dir(path));
  return 1;
}

// ------------------------------------------------------------------
// io.remove(path)
// ------------------------------------------------------------------

static int vli_remove(VL_State *S) {
  const char *path = vli_check_str(S, 1);
#if defined(_WIN32)
  int rc = remove(path);
#else
  int rc = unlink(path);
  if (rc != 0 && errno == EISDIR) rc = rmdir(path);
#endif
  if (rc != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

// ------------------------------------------------------------------
// io.rename(old, new)
// ------------------------------------------------------------------

static int vli_rename(VL_State *S) {
  const char *oldp = vli_check_str(S, 1);
  const char *newp = vli_check_str(S, 2);
  int rc = rename(oldp, newp);
  if (rc != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

// ------------------------------------------------------------------
// io.mkdirs(path)
// ------------------------------------------------------------------

static int vli_mkdirs(VL_State *S) {
  const char *path = vli_check_str(S, 1);
  AuxStatus st = aux_mkdirs(path);
  if (st != AUX_OK) {
    vl_push_nil(S);
    vl_push_string(S, aux_status_str(st));
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

// ------------------------------------------------------------------
// io.listdir(path) -> "name1\nname2\n...", count
// ------------------------------------------------------------------

static int vli_listdir(VL_State *S) {
  const char *path = vli_check_str(S, 1);

#if defined(_WIN32)
  char pattern[PATH_MAX];
  size_t n = strnlen(path, sizeof pattern - 4);
  if (n >= sizeof pattern - 4) {
    vl_push_nil(S);
    vl_push_string(S, "ERANGE");
    return 2;
  }
  memcpy(pattern, path, n);
  pattern[n] = 0;
  // Ensure trailing separator
  if (n > 0 && pattern[n - 1] != '\\' && pattern[n - 1] != '/') {
    pattern[n++] = '\\';
    pattern[n] = 0;
  }
  strcat(pattern, "*");

  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }

  // Build into dynamic buffer separated by '\n'
  AuxBuffer buf = {0};
  int count = 0;
  do {
    const char *name = fd.cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    size_t L = strlen(name);
    uint8_t *p = (uint8_t *)realloc(buf.data, buf.len + L + 2);
    if (!p) {
      FindClose(h);
      aux_buffer_free(&buf);
      vl_push_nil(S);
      vl_push_string(S, "ENOMEM");
      return 2;
    }
    buf.data = p;
    memcpy(buf.data + buf.len, name, L);
    buf.len += L;
    buf.data[buf.len++] = '\n';
    buf.data[buf.len] = 0;
    count++;
  } while (FindNextFileA(h, &fd));
  FindClose(h);

  if (buf.len && buf.data[buf.len - 1] == '\n')
    buf.data[buf.len - 1] = 0, buf.len--;
  vl_push_lstring(S, (const char *)buf.data, (int)buf.len);
  vl_push_int(S, (int64_t)count);
  aux_buffer_free(&buf);
  return 2;

#else
  DIR *d = opendir(path);
  if (!d) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  AuxBuffer buf = {0};
  int count = 0;
  for (;;) {
    struct dirent *de = readdir(d);
    if (!de) break;
    const char *name = de->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    size_t L = strlen(name);
    uint8_t *p = (uint8_t *)realloc(buf.data, buf.len + L + 2);
    if (!p) {
      closedir(d);
      aux_buffer_free(&buf);
      vl_push_nil(S);
      vl_push_string(S, "ENOMEM");
      return 2;
    }
    buf.data = p;
    memcpy(buf.data + buf.len, name, L);
    buf.len += L;
    buf.data[buf.len++] = '\n';
    buf.data[buf.len] = 0;
    count++;
  }
  closedir(d);
  if (buf.len && buf.data[buf.len - 1] == '\n')
    buf.data[buf.len - 1] = 0, buf.len--;
  vl_push_lstring(S, (const char *)buf.data, (int)buf.len);
  vl_push_int(S, (int64_t)count);
  aux_buffer_free(&buf);
  return 2;
#endif
}

// ------------------------------------------------------------------
// io.cwd()
// ------------------------------------------------------------------

static int vli_cwd(VL_State *S) {
#if defined(_WIN32)
  char buf[PATH_MAX];
  DWORD n = GetCurrentDirectoryA((DWORD)AUX_ARRAY_LEN(buf), buf);
  if (n == 0 || n >= AUX_ARRAY_LEN(buf)) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, buf);
  return 1;
#else
  char buf[PATH_MAX];
  if (!getcwd(buf, sizeof buf)) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, buf);
  return 1;
#endif
}

// ------------------------------------------------------------------
// io.stat(path) -> size:int, mtime:int64, atime:int64
// ------------------------------------------------------------------

static int vli_stat(VL_State *S) {
  const char *path = vli_check_str(S, 1);
#if defined(_WIN32)
  struct _stat64 st;
  if (_stat64(path, &st) != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_int(S, (int64_t)st.st_size);
  vl_push_int(S, (int64_t)st.st_mtime);
  vl_push_int(S, (int64_t)st.st_atime);
  return 3;
#else
  struct stat st;
  if (stat(path, &st) != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_int(S, (int64_t)st.st_size);
  vl_push_int(S, (int64_t)st.st_mtime);
  vl_push_int(S, (int64_t)st.st_atime);
  return 3;
#endif
}

// ------------------------------------------------------------------
// io.read_stdin()
// ------------------------------------------------------------------

static int vli_read_stdin(VL_State *S) {
  // Read all from stdin into buffer
  AuxBuffer buf = {0};
  size_t cap = 0;
  for (;;) {
    if (buf.len + 4096 + 1 > cap) {
      cap = cap ? cap * 2 : 8192;
      uint8_t *p = (uint8_t *)realloc(buf.data, cap);
      if (!p) {
        aux_buffer_free(&buf);
        vl_push_nil(S);
        vl_push_string(S, "ENOMEM");
        return 2;
      }
      buf.data = p;
    }
    size_t n = fread(buf.data + buf.len, 1, 4096, stdin);
    buf.len += n;
    if (n < 4096) {
      if (ferror(stdin)) {
        aux_buffer_free(&buf);
        vl_push_nil(S);
        vl_push_string(S, "EIO");
        return 2;
      }
      break;
    }
  }
  if (buf.data) buf.data[buf.len] = 0;
  vl_push_lstring(S, (const char *)buf.data, (int)buf.len);
  aux_buffer_free(&buf);
  return 1;
}

// ------------------------------------------------------------------
// io.write_stdout(data) / io.write_stderr(data)
// ------------------------------------------------------------------

static int vli_write_stdout(VL_State *S) {
  const char *data = vli_check_str(S, 1);
  size_t len = strlen(data);
  size_t wr = len ? fwrite(data, 1, len, stdout) : 0;
  fflush(stdout);
  if (wr != len) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_int(S, (int64_t)wr);
  return 1;
}

static int vli_write_stderr(VL_State *S) {
  const char *data = vli_check_str(S, 1);
  size_t len = strlen(data);
  size_t wr = len ? fwrite(data, 1, len, stderr) : 0;
  fflush(stderr);
  if (wr != len) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_int(S, (int64_t)wr);
  return 1;
}

// ------------------------------------------------------------------
// Registration
// ------------------------------------------------------------------

static const VL_Reg iolib[] = {{"read", vli_read},
                               {"write", vli_write},
                               {"append", vli_append},
                               {"exists", vli_exists},
                               {"is_file", vli_is_file},
                               {"is_dir", vli_is_dir},
                               {"remove", vli_remove},
                               {"rename", vli_rename},
                               {"mkdirs", vli_mkdirs},
                               {"listdir", vli_listdir},
                               {"cwd", vli_cwd},
                               {"stat", vli_stat},
                               {"read_stdin", vli_read_stdin},
                               {"write_stdout", vli_write_stdout},
                               {"write_stderr", vli_write_stderr},
                               {NULL, NULL}};

void vl_open_iolib(VL_State *S) { vl_register_lib(S, "io", iolib); }
