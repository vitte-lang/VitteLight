// SPDX-License-Identifier: GPL-3.0-or-later
//
// config.c — Key/Value configuration parser (INI/CFG) for Vitte Light (C17, portable)
// Namespace: "config"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c config.c
//
// Features:
//   - Simple INI-style parser: section [name], key=value, # or ; comments.
//   - Stores values as strings (caller converts to int/float/bool).
//   - Case-sensitive keys/sections.
//   - Provides query functions with default values.
//   - Save back to file (re-serialize).
//   - Dynamic growth with realloc.
//   - Error messages on stderr for invalid lines.
//
// API:
//   typedef struct { char* key; char* val; } config_kv;
//   typedef struct { char* name; config_kv* kv; size_t n, cap; } config_section;
//   typedef struct { config_section* s; size_t n, cap; } config_t;
//
//   void config_init(config_t* c);
//   void config_free(config_t* c);
//
//   int  config_load_file(config_t* c, const char* path);   // 0/-1
//   int  config_save_file(const config_t* c, const char* path); // 0/-1
//
//   const char* config_get(const config_t* c, const char* section, const char* key);
//   const char* config_get_default(const config_t* c, const char* section, const char* key, const char* def);
//   long        config_get_int(const config_t* c, const char* section, const char* key, long def);
//   double      config_get_double(const config_t* c, const char* section, const char* key, double def);
//   int         config_get_bool(const config_t* c, const char* section, const char* key, int def);
//
//   int  config_set(config_t* c, const char* section, const char* key, const char* val);
//   int  config_remove(config_t* c, const char* section, const char* key);
//   int  config_remove_section(config_t* c, const char* section);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libctype.h"
#include <errno.h>

/* ===== Types ===== */
typedef struct { char* key; char* val; } config_kv;
typedef struct { char* name; config_kv* kv; size_t n, cap; } config_section;
typedef struct { config_section* s; size_t n, cap; } config_t;

/* ===== Helpers ===== */
static void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n);
    if (!q && n) {
        fprintf(stderr, "config: out of memory\n");
        exit(1);
    }
    return q;
}

static char* xstrdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = malloc(n);
    if (!p) { fprintf(stderr, "config: out of memory\n"); exit(1); }
    memcpy(p, s, n);
    return p;
}

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char* e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

/* ===== Init/Free ===== */
void config_init(config_t* c) {
    c->s = NULL;
    c->n = c->cap = 0;
}

static void free_section(config_section* sec) {
    free(sec->name);
    for (size_t i = 0; i < sec->n; i++) {
        free(sec->kv[i].key);
        free(sec->kv[i].val);
    }
    free(sec->kv);
}

void config_free(config_t* c) {
    for (size_t i = 0; i < c->n; i++) free_section(&c->s[i]);
    free(c->s);
    c->s = NULL;
    c->n = c->cap = 0;
}

/* ===== Accessors ===== */
static config_section* find_section(config_t* c, const char* name, int create) {
    for (size_t i = 0; i < c->n; i++)
        if (strcmp(c->s[i].name, name) == 0)
            return &c->s[i];
    if (!create) return NULL;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 4;
        c->s = xrealloc(c->s, c->cap * sizeof(*c->s));
    }
    config_section* sec = &c->s[c->n++];
    sec->name = xstrdup(name);
    sec->kv = NULL;
    sec->n = sec->cap = 0;
    return sec;
}

static config_kv* find_kv(config_section* sec, const char* key, int create) {
    for (size_t i = 0; i < sec->n; i++)
        if (strcmp(sec->kv[i].key, key) == 0)
            return &sec->kv[i];
    if (!create) return NULL;
    if (sec->n == sec->cap) {
        sec->cap = sec->cap ? sec->cap * 2 : 4;
        sec->kv = xrealloc(sec->kv, sec->cap * sizeof(*sec->kv));
    }
    config_kv* kv = &sec->kv[sec->n++];
    kv->key = xstrdup(key);
    kv->val = NULL;
    return kv;
}

const char* config_get(const config_t* c, const char* section, const char* key) {
    for (size_t i = 0; i < c->n; i++)
        if (strcmp(c->s[i].name, section) == 0) {
            for (size_t j = 0; j < c->s[i].n; j++)
                if (strcmp(c->s[i].kv[j].key, key) == 0)
                    return c->s[i].kv[j].val;
        }
    return NULL;
}

const char* config_get_default(const config_t* c, const char* section, const char* key, const char* def) {
    const char* v = config_get(c, section, key);
    return v ? v : def;
}

long config_get_int(const config_t* c, const char* section, const char* key, long def) {
    const char* v = config_get(c, section, key);
    if (!v) return def;
    char* end;
    long x = strtol(v, &end, 0);
    return *end ? def : x;
}

double config_get_double(const config_t* c, const char* section, const char* key, double def) {
    const char* v = config_get(c, section, key);
    if (!v) return def;
    char* end;
    double x = strtod(v, &end);
    return *end ? def : x;
}

int config_get_bool(const config_t* c, const char* section, const char* key, int def) {
    const char* v = config_get(c, section, key);
    if (!v) return def;
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0)
        return 1;
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0)
        return 0;
    return def;
}

/* ===== Mutators ===== */
int config_set(config_t* c, const char* section, const char* key, const char* val) {
    config_section* sec = find_section(c, section, 1);
    config_kv* kv = find_kv(sec, key, 1);
    free(kv->val);
    kv->val = xstrdup(val);
    return 0;
}

int config_remove(config_t* c, const char* section, const char* key) {
    config_section* sec = find_section(c, section, 0);
    if (!sec) return -1;
    for (size_t i = 0; i < sec->n; i++) {
        if (strcmp(sec->kv[i].key, key) == 0) {
            free(sec->kv[i].key);
            free(sec->kv[i].val);
            memmove(&sec->kv[i], &sec->kv[i+1], (sec->n - i - 1) * sizeof(*sec->kv));
            sec->n--;
            return 0;
        }
    }
    return -1;
}

int config_remove_section(config_t* c, const char* section) {
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->s[i].name, section) == 0) {
            free_section(&c->s[i]);
            memmove(&c->s[i], &c->s[i+1], (c->n - i - 1) * sizeof(*c->s));
            c->n--;
            return 0;
        }
    }
    return -1;
}

/* ===== Load/Save ===== */
int config_load_file(config_t* c, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    config_section* cur = find_section(c, "", 1); // global section
    for (int lineno = 1; fgets(line, sizeof line, f); lineno++) {
        char* p = trim(line);
        if (*p == 0 || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char* q = strchr(p, ']');
            if (!q) { fprintf(stderr, "%s:%d: missing ']'\n", path, lineno); continue; }
            *q = 0;
            cur = find_section(c, trim(p+1), 1);
            continue;
        }
        char* eq = strchr(p, '=');
        if (!eq) { fprintf(stderr, "%s:%d: missing '='\n", path, lineno); continue; }
        *eq = 0;
        char* key = trim(p);
        char* val = trim(eq+1);
        config_set(c, cur->name, key, val);
    }
    fclose(f);
    return 0;
}

int config_save_file(const config_t* c, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    for (size_t i = 0; i < c->n; i++) {
        const config_section* sec = &c->s[i];
        if (sec->name[0] != 0) fprintf(f, "[%s]\n", sec->name);
        for (size_t j = 0; j < sec->n; j++) {
            fprintf(f, "%s=%s\n", sec->kv[j].key, sec->kv[j].val ? sec->kv[j].val : "");
        }
        if (i + 1 < c->n) fputc('\n', f);
    }
    fclose(f);
    return 0;
}

/* ===== Test Harness (optional) ===== */
#ifdef CONFIG_TEST
int main(void) {
    config_t c; config_init(&c);
    config_load_file(&c, "test.cfg");
    printf("val=%s\n", config_get_default(&c,"section","key","default"));
    config_set(&c,"section","newkey","42");
    config_save_file(&c,"out.cfg");
    config_free(&c);
    return 0;
}
#endif