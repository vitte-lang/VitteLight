// SPDX-License-Identifier: GPL-3.0-or-later
//
// iolib.c — I/O helpers for Vitte Light (C17, portable)
// Namespace: "io"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c iolib.c
//
// Features:
//   - Safe stdin/stdout/stderr wrappers.
//   - Binary/text file read/write.
//   - Line input with dynamic buffer.
//   - File existence/size.
//   - Redirect stdout/stderr to file.
//   - Copy streams.
//
// Notes:
//   - Errors return -1 with errno set. Success 0.
//   - Memory returned is malloc'd; caller frees.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

// ---------------- Basic stdout/stderr ----------------

int io_puts(const char* s) {
    if (!s) { errno = EINVAL; return -1; }
    if (fputs(s, stdout) == EOF) return -1;
    if (fputc('\n', stdout) == EOF) return -1;
    return fflush(stdout);
}

int io_puterr(const char* s) {
    if (!s) { errno = EINVAL; return -1; }
    if (fputs(s, stderr) == EOF) return -1;
    if (fputc('\n', stderr) == EOF) return -1;
    return fflush(stderr);
}

int io_print(const char* s) {
    if (!s) { errno = EINVAL; return -1; }
    if (fputs(s, stdout) == EOF) return -1;
    return fflush(stdout);
}

// ---------------- Line input ----------------

char* io_readline(FILE* f) {
    if (!f) f = stdin;
    size_t cap = 128, len = 0;
    char* buf = malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char* tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        if (c == '\n') break;
        buf[len++] = (char)c;
    }
    if (len == 0 && c == EOF) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

// ---------------- File I/O ----------------

int io_read_file(const char* path, void** data, size_t* size) {
    if (!path || !data || !size) { errno = EINVAL; return -1; }
    *data = NULL; *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long end = ftell(f);
    if (end < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t n = (size_t) end;
    void* buf = malloc(n+1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, n, f);
    fclose(f);
    if (rd != n) { free(buf); return -1; }
    ((char*)buf)[n] = '\0';
    *data = buf; *size = n;
    return 0;
}

int io_write_file(const char* path, const void* data, size_t size) {
    if (!path || (!data && size)) { errno = EINVAL; return -1; }
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t wr = size ? fwrite(data, 1, size, f) : 0;
    int rc = (wr == size && fflush(f) == 0 && fclose(f) == 0) ? 0 : -1;
    if (rc != 0) fclose(f);
    return rc;
}

// ---------------- File info ----------------

bool io_exists(const char* path) {
    if (!path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

long io_filesize(const char* path) {
    if (!path) { errno = EINVAL; return -1; }
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    fclose(f);
    return sz;
}

// ---------------- Redirect ----------------

int io_redirect_stdout(const char* path) {
    if (!path) { errno = EINVAL; return -1; }
    FILE* f = freopen(path, "w", stdout);
    return f ? 0 : -1;
}

int io_redirect_stderr(const char* path) {
    if (!path) { errno = EINVAL; return -1; }
    FILE* f = freopen(path, "w", stderr);
    return f ? 0 : -1;
}

// ---------------- Copy streams ----------------

int io_copy_stream(FILE* in, FILE* out) {
    if (!in) in = stdin;
    if (!out) out = stdout;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) return -1;
    }
    return ferror(in) ? -1 : 0;
}

// ---------------- Demo ----------------
#ifdef IOLIB_DEMO
int main(void) {
    io_puts("Hello world to stdout");
    io_puterr("Hello to stderr");
    printf("Enter line: ");
    char* line = io_readline(stdin);
    if (line) {
        io_puts(line);
        free(line);
    }
    const char* msg = "sample text";
    io_write_file("out.txt", msg, strlen(msg));
    void* data; size_t sz;
    if (io_read_file("out.txt", &data, &sz)==0) {
        printf("read %zu: %s\n", sz, (char*)data);
        free(data);
    }
    return 0;
}
#endif