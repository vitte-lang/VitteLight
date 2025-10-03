/* ============================================================================
 * macros.c — Portable macro table for VitteLight (C11)
 *  - Store name→value pairs (string,string)
 *  - Add, define/undef, lookup, iterate
 *  - Backed by dynamic array, no external deps
 * ========================================================================== */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* ===== Types ============================================================== */

typedef struct {
    char* name;
    char* value;
} macro_entry;

typedef struct {
    macro_entry* entries;
    size_t       count;
    size_t       cap;
} macro_table;

/* ===== Helpers ============================================================ */

static char* str_dup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

static void ensure_cap(macro_table* t, size_t need) {
    if (t->cap >= need) return;
    size_t newcap = t->cap ? t->cap * 2 : 8;
    if (newcap < need) newcap = need;
    macro_entry* ne = (macro_entry*)realloc(t->entries, newcap * sizeof(macro_entry));
    if (!ne) return; /* OOM → silent, caller must check */
    t->entries = ne;
    t->cap = newcap;
}

/* ===== API ================================================================ */

void macros_init(macro_table* t) {
    if (!t) return;
    t->entries = NULL;
    t->count = 0;
    t->cap = 0;
}

void macros_free(macro_table* t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) {
        free(t->entries[i].name);
        free(t->entries[i].value);
    }
    free(t->entries);
    t->entries = NULL;
    t->count = 0;
    t->cap = 0;
}

/* Add or replace a macro. Returns 0 ok, -1 error. */
int macros_define(macro_table* t, const char* name, const char* value) {
    if (!t || !name) return -1;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].name, name) == 0) {
            free(t->entries[i].value);
            t->entries[i].value = str_dup(value ? value : "");
            return t->entries[i].value ? 0 : -1;
        }
    }
    ensure_cap(t, t->count + 1);
    if (t->count + 1 > t->cap) return -1; /* realloc failed */
    macro_entry* e = &t->entries[t->count++];
    e->name = str_dup(name);
    e->value = str_dup(value ? value : "");
    if (!e->name || !e->value) return -1;
    return 0;
}

/* Remove a macro by name. Returns 1 if removed, 0 if not found. */
int macros_undef(macro_table* t, const char* name) {
    if (!t || !name) return 0;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].name, name) == 0) {
            free(t->entries[i].name);
            free(t->entries[i].value);
            /* shift */
            memmove(&t->entries[i], &t->entries[i+1], (t->count - i - 1) * sizeof(macro_entry));
            t->count--;
            return 1;
        }
    }
    return 0;
}

/* Lookup. Returns value or NULL if not found. */
const char* macros_get(const macro_table* t, const char* name) {
    if (!t || !name) return NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].name, name) == 0)
            return t->entries[i].value;
    }
    return NULL;
}

/* Test existence. */
bool macros_has(const macro_table* t, const char* name) {
    return macros_get(t, name) != NULL;
}

/* Dump to FILE* */
void macros_dump(const macro_table* t, FILE* out) {
    if (!t || !out) return;
    for (size_t i = 0; i < t->count; i++) {
        fprintf(out, "#define %s %s\n", t->entries[i].name,
                t->entries[i].value ? t->entries[i].value : "");
    }
}

/* Count */
size_t macros_count(const macro_table* t) {
    return t ? t->count : 0;
}

/* Index access */
const char* macros_name_at(const macro_table* t, size_t idx) {
    return (t && idx < t->count) ? t->entries[idx].name : NULL;
}
const char* macros_value_at(const macro_table* t, size_t idx) {
    return (t && idx < t->count) ? t->entries[idx].value : NULL;
}

/* ===== Self-test ========================================================== */
#ifdef MACROS_MAIN
int main(void) {
    macro_table T;
    macros_init(&T);

    macros_define(&T, "FOO", "123");
    macros_define(&T, "BAR", "baz");
    macros_define(&T, "EMPTY", "");

    macros_dump(&T, stdout);

    printf("FOO? %s\n", macros_get(&T, "FOO"));
    printf("HAS BAR? %d\n", macros_has(&T, "BAR"));
    printf("COUNT = %zu\n", macros_count(&T));

    macros_undef(&T, "BAR");
    macros_dump(&T, stdout);

    macros_free(&T);
    return 0;
}
#endif
