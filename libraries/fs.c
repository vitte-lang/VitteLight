// SPDX-License-Identifier: GPL-3.0-or-later
//
// fs.c — Portable filesystem helpers for Vitte Light (C17)
// Namespace: "fs"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c fs.c
//
// Provides:
//   // Stat
//   typedef struct { uint64_t size; int is_dir; int exists; uint64_t mtime_sec; } fs_stat_t;
//   int      fs_stat(const char* path, fs_stat_t* out);
//   int      fs_exists(const char* path);     // 1 yes, 0 no, -1 error
//   int      fs_isfile(const char* path);     // 1 yes, 0 no, -1 error
//   int      fs_isdir(const char* path);      // 1 yes, 0 no, -1 error
//
//   // Paths
//   char     fs_sep(void);                    // '/' or '\\'
//   int      fs_join(char* out, size_t n, const char* a, const char* b);
//   int      fs_dirname(char* out, size_t n, const char* path);
//   int      fs_basename(char* out, size_t n, const char* path);
//
//   // Dirs
//   int      fs_mkdir(const char* path);      // creates one level; 0| -1
//   int      fs_mkdir_p(const char* path);    // recursive;        0| -1
//   int      fs_rmdir(const char* path);      // remove empty dir
//   int      fs_rmdir_r(const char* path);    // remove recursively
//
//   // Files
//   int      fs_remove(const char* path);     // file or dir (if empty)
//   int      fs_copy_file(const char* src, const char* dst);
//   int      fs_move(const char* src, const char* dst, int replace);
//   int      fs_read_file(const char* path, void** data, size_t* size);  // malloc buffer
//   int      fs_write_file(const char* path, const void* data, size_t size);
//   int      fs_write_file_atomic(const char* path, const void* data, size_t size);
//
//   // Iteration
//   typedef int (*fs_iter_cb)(const char* path, int is_dir, void* user);
//   int      fs_listdir(const char* dir, int recursive, fs_iter_cb cb, void* user);
//
// Notes:
//   - Functions return 0 on success unless stated otherwise. errno is set on error.
//   - Paths are UTF-8 on POSIX. On Windows these APIs use ANSI narrow; integrate UTF-16 if needed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #define fs_mkdir_one(p,mode) _mkdir(p)
  #define FS_SEP '\\'
#else
  #include <dirent.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define fs_mkdir_one(p,mode) mkdir(p, mode)
  #define FS_SEP '/'
#endif

typedef struct { uint64_t size; int is_dir; int exists; uint64_t mtime_sec; } fs_stat_t;

// --------- Small helpers ---------

static int fs_is_sep(char c) { return c == '/' || c == '\\'; }
char fs_sep(void) { return FS_SEP; }

int fs_join(char* out, size_t n, const char* a, const char* b) {
    if (!out || n==0 || !a || !b) { errno = EINVAL; return -1; }
    size_t la = strlen(a);
    int need = (la > 0 && !fs_is_sep(a[la-1]));
    int rc = snprintf(out, n, "%s%s%s", a, need ? (char[]){FS_SEP,0} : "", b);
    if (rc < 0 || (size_t)rc >= n) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int fs_dirname(char* out, size_t n, const char* path) {
    if (!out || n==0 || !path) { errno = EINVAL; return -1; }
    size_t len = strlen(path);
    if (len == 0) { snprintf(out, n, "."); return 0; }
    size_t i = len;
    while (i>0 && !fs_is_sep(path[i-1])) i--;
    if (i==0) { snprintf(out, n, "."); return 0; }
    while (i>1 && fs_is_sep(path[i-1])) i--;
    if (i >= n) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, path, i);
    out[i] = '\0';
    return 0;
}

int fs_basename(char* out, size_t n, const char* path) {
    if (!out || n==0 || !path) { errno = EINVAL; return -1; }
    const char* p = path + strlen(path);
    while (p>path && !fs_is_sep(*(p-1))) p--;
    if (snprintf(out, n, "%s", p) >= (int)n) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

// --------- Stat / existence ---------

int fs_stat(const char* path, fs_stat_t* out) {
    if (!path || !out) { errno = EINVAL; return -1; }
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        out->exists = 0; out->is_dir = 0; out->size = 0; out->mtime_sec = 0;
        return 0;
    }
    out->exists = 1;
    out->is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    ULARGE_INTEGER u; u.LowPart = fad.nFileSizeLow; u.HighPart = fad.nFileSizeHigh;
    out->size = out->is_dir ? 0u : (uint64_t)u.QuadPart;
    ULARGE_INTEGER t; t.LowPart = fad.ftLastWriteTime.dwLowDateTime; t.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    out->mtime_sec = (uint64_t)(t.QuadPart / 10000000ULL); // 100ns to sec
    return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) { out->exists = 0; out->is_dir = 0; out->size = 0; out->mtime_sec = 0; return 0; }
        return -1;
    }
    out->exists = 1;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->size   = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0u;
    out->mtime_sec = (uint64_t)st.st_mtime;
    return 0;
#endif
}

int fs_exists(const char* path)  { fs_stat_t s; if (fs_stat(path,&s)!=0) return -1; return s.exists; }
int fs_isdir(const char* path)   { fs_stat_t s; if (fs_stat(path,&s)!=0) return -1; return s.exists ? s.is_dir : 0; }
int fs_isfile(const char* path)  { fs_stat_t s; if (fs_stat(path,&s)!=0) return -1; return s.exists ? !s.is_dir : 0; }

// --------- Dirs ---------

int fs_mkdir(const char* path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
#if defined(_WIN32)
    return _mkdir(path) == 0 ? 0 : -1;
#else
    return mkdir(path, 0777) == 0 ? 0 : -1;
#endif
}

int fs_mkdir_p(const char* path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    char* tmp = strdup(path);
    if (!tmp) return -1;
    size_t len = strlen(tmp);
#if defined(_WIN32)
    size_t i = (len >= 2 && tmp[1] == ':') ? 2 : 0; // skip "C:"
#else
    size_t i = 0;
#endif
    for (; i < len; ++i) {
        if (fs_is_sep(tmp[i])) {
            if (i == 0) continue; // root
            tmp[i] = '\0';
            if (fs_mkdir_one(tmp, 0777) != 0 && errno != EEXIST) { free(tmp); return -1; }
            tmp[i] = FS_SEP;
        }
    }
    if (fs_mkdir_one(tmp, 0777) != 0 && errno != EEXIST) { free(tmp); return -1; }
    free(tmp);
    return 0;
}

int fs_rmdir(const char* path) {
    if (!path) { errno = EINVAL; return -1; }
#if defined(_WIN32)
    return _rmdir(path) == 0 ? 0 : -1;
#else
    return rmdir(path) == 0 ? 0 : -1;
#endif
}

// Remove directory recursively
static int fs_rmdir_r_inner(const char* path) {
#if defined(_WIN32)
    char pattern[MAX_PATH];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", path) >= (int)sizeof(pattern)) { errno = ENAMETOOLONG; return -1; }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // empty or not found; try remove itself
        return fs_rmdir(path);
    }
    do {
        const char* name = fd.cFileName;
        if (strcmp(name,".")==0 || strcmp(name,"..")==0) continue;
        char full[MAX_PATH];
        if (snprintf(full, sizeof(full), "%s\\%s", path, name) >= (int)sizeof(full)) { FindClose(h); errno=ENAMETOOLONG; return -1; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fs_rmdir_r_inner(full) != 0) { FindClose(h); return -1; }
        } else {
            if (DeleteFileA(full) == 0) { FindClose(h); return -1; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return fs_rmdir(path);
#else
    DIR* d = opendir(path);
    if (!d) return fs_rmdir(path);
    struct dirent* de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) continue;
        char full[4096];
        if (snprintf(full, sizeof(full), "%s/%s", path, de->d_name) >= (int)sizeof(full)) { closedir(d); errno=ENAMETOOLONG; return -1; }
        struct stat st;
        if (lstat(full, &st) != 0) { closedir(d); return -1; }
        if (S_ISDIR(st.st_mode)) {
            if (fs_rmdir_r_inner(full) != 0) { closedir(d); return -1; }
        } else {
            if (unlink(full) != 0) { closedir(d); return -1; }
        }
    }
    closedir(d);
    return fs_rmdir(path);
#endif
}

int fs_rmdir_r(const char* path) {
    if (!path) { errno = EINVAL; return -1; }
    return fs_rmdir_r_inner(path);
}

// --------- Files ---------

int fs_remove(const char* path) {
    if (!path) { errno = EINVAL; return -1; }
#if defined(_WIN32)
    // Try file first, then dir
    if (DeleteFileA(path)) return 0;
    return _rmdir(path) == 0 ? 0 : -1;
#else
    return remove(path) == 0 ? 0 : -1;
#endif
}

int fs_copy_file(const char* src, const char* dst) {
    if (!src || !dst) { errno = EINVAL; return -1; }
#if defined(_WIN32)
    // false=do not overwrite, but we mimic POSIX overwrite behavior by removing first
    (void)DeleteFileA(dst);
    if (!CopyFileA(src, dst, FALSE)) return -1;
    return 0;
#else
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[1<<16];
    size_t n;
    while ((n = fread(buf,1,sizeof buf,in)) > 0) {
        if (fwrite(buf,1,n,out) != n) { fclose(in); fclose(out); return -1; }
    }
    int rc = (ferror(in) ? -1 : 0);
    if (fflush(out) != 0) rc = -1;
    if (fclose(out) != 0) rc = -1;
    fclose(in);
    return rc;
#endif
}

int fs_move(const char* src, const char* dst, int replace) {
    if (!src || !dst) { errno = EINVAL; return -1; }
#if defined(_WIN32)
    DWORD flags = replace ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH : MOVEFILE_WRITE_THROUGH;
    return MoveFileExA(src, dst, flags) ? 0 : -1;
#else
    if (!replace) {
        fs_stat_t s; if (fs_stat(dst,&s)!=0) return -1; if (s.exists) { errno = EEXIST; return -1; }
    }
    return rename(src, dst) == 0 ? 0 : -1;
#endif
}

int fs_read_file(const char* path, void** data, size_t* size) {
    if (!path || !data || !size) { errno = EINVAL; return -1; }
    *data = NULL; *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long end = ftell(f);
    if (end < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t n = (size_t) end;
    void* buf = malloc(n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, n, f);
    fclose(f);
    if (rd != n) { free(buf); return -1; }
    ((char*)buf)[n] = '\0'; // convenience for text
    *data = buf; *size = n;
    return 0;
}

int fs_write_file(const char* path, const void* data, size_t size) {
    if (!path || (!data && size)) { errno = EINVAL; return -1; }
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t wr = size ? fwrite(data, 1, size, f) : 0;
    int rc = (wr == size && fflush(f) == 0 && fclose(f) == 0) ? 0 : -1;
    if (rc != 0) fclose(f);
    return rc;
}

int fs_write_file_atomic(const char* path, const void* data, size_t size) {
    if (!path) { errno = EINVAL; return -1; }
    char dir[1024], base[512], tmp[1600];
    if (fs_dirname(dir, sizeof(dir), path) != 0) return -1;
    if (fs_basename(base, sizeof(base), path) != 0) return -1;
#if defined(_WIN32)
    if (snprintf(tmp, sizeof(tmp), "%s\\.%s.tmp.%lu", dir, base, (unsigned long)GetCurrentProcessId()) >= (int)sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
#else
    if (snprintf(tmp, sizeof(tmp), "%s/.%s.tmp.%ld", dir, base, (long)getpid()) >= (int)sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
#endif
    if (fs_write_file(tmp, data, size) != 0) { (void)fs_remove(tmp); return -1; }
#if defined(_WIN32)
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { (void)fs_remove(tmp); return -1; }
#else
    if (rename(tmp, path) != 0) { (void)fs_remove(tmp); return -1; }
#endif
    return 0;
}

// --------- Iteration ---------

typedef int (*fs_iter_cb)(const char* path, int is_dir, void* user);

static int fs_listdir_inner(const char* dir, int recursive, fs_iter_cb cb, void* user) {
#if defined(_WIN32)
    char pattern[MAX_PATH];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", dir) >= (int)sizeof(pattern)) { errno=ENAMETOOLONG; return -1; }
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int rc = 0;
    do {
        const char* name = fd.cFileName;
        if (strcmp(name,".")==0 || strcmp(name,"..")==0) continue;
        char full[MAX_PATH];
        if (snprintf(full, sizeof(full), "%s\\%s", dir, name) >= (int)sizeof(full)) { rc=-1; break; }
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        if (cb && cb(full, is_dir, user) != 0) { rc = 1; break; }
        if (recursive && is_dir) {
            rc = fs_listdir_inner(full, 1, cb, user);
            if (rc != 0) break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return rc < 0 ? -1 : 0;
#else
    DIR* d = opendir(dir);
    if (!d) return -1;
    struct dirent* de;
    int rc = 0;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) continue;
        char full[4096];
        if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= (int)sizeof(full)) { rc=-1; break; }
        struct stat st;
        if (lstat(full, &st) != 0) { rc=-1; break; }
        int is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        if (cb && cb(full, is_dir, user) != 0) { rc = 1; break; }
        if (recursive && is_dir) {
            rc = fs_listdir_inner(full, 1, cb, user);
            if (rc != 0) break;
        }
    }
    closedir(d);
    return rc < 0 ? -1 : 0;
#endif
}

int fs_listdir(const char* dir, int recursive, fs_iter_cb cb, void* user) {
    if (!dir) { errno = EINVAL; return -1; }
    return fs_listdir_inner(dir, recursive ? 1 : 0, cb, user);
}

// --------- Demo (optional) ---------
#ifdef FS_DEMO
#include <stdio.h>
static int print_cb(const char* p, int is_dir, void* u) {
    (void)u; printf("%s%s\n", p, is_dir ? "/" : ""); return 0;
}
int main(int argc, char** argv) {
    const char* d = argc>1 ? argv[1] : ".";
    fs_stat_t st; fs_stat(d,&st);
    printf("exists=%d dir=%d size=%llu mtime=%llu\n", st.exists, st.is_dir,
           (unsigned long long)st.size, (unsigned long long)st.mtime_sec);
    fs_listdir(d, 0, print_cb, NULL);
    return 0;
}
#endif