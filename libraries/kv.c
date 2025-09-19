// SPDX-License-Identifier: GPL-3.0-or-later
//
// kv.c — In-memory key/value store for Vitte Light VM (C17, portable)
// Namespace: "kv"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c kv.c
//
// Model:
//   - Stores string keys and arbitrary byte values.
//   - Hash table with open addressing (linear probing).
//   - API: create, destroy, put, get, remove, clear, size, iter.
//   - Keys are copied (null-terminated). Values copied as raw bytes.
//
// Note:
//   This module is standalone. No external deps beyond libc.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char   *key;
    void   *value;
    size_t  vlen;
    bool    used;
} KVEntry;

typedef struct {
    KVEntry *entries;
    size_t   capacity;
    size_t   count;
} KVStore;

#define KV_INITIAL_CAPACITY 64
#define KV_LOAD_FACTOR 0.7

// simple FNV-1a 64-bit hash
static uint64_t kv_hash(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 1099511628211ull;
    }
    return h;
}

static KVStore *kv_create_capacity(size_t cap) {
    KVStore *kv = calloc(1, sizeof(KVStore));
    if (!kv) return NULL;
    kv->entries = calloc(cap, sizeof(KVEntry));
    if (!kv->entries) { free(kv); return NULL; }
    kv->capacity = cap;
    kv->count = 0;
    return kv;
}

KVStore *kv_create(void) {
    return kv_create_capacity(KV_INITIAL_CAPACITY);
}

void kv_destroy(KVStore *kv) {
    if (!kv) return;
    for (size_t i = 0; i < kv->capacity; i++) {
        if (kv->entries[i].used) {
            free(kv->entries[i].key);
            free(kv->entries[i].value);
        }
    }
    free(kv->entries);
    free(kv);
}

static bool kv_resize(KVStore *kv, size_t newcap);

bool kv_put(KVStore *kv, const char *key, const void *val, size_t vlen) {
    if (!kv || !key) return false;
    if ((double)(kv->count + 1) / kv->capacity > KV_LOAD_FACTOR) {
        if (!kv_resize(kv, kv->capacity * 2)) return false;
    }

    uint64_t h = kv_hash(key);
    size_t idx = h % kv->capacity;

    for (;;) {
        KVEntry *e = &kv->entries[idx];
        if (!e->used) {
            e->key = strdup(key);
            e->value = malloc(vlen);
            if (!e->key || (!e->value && vlen > 0)) return false;
            memcpy(e->value, val, vlen);
            e->vlen = vlen;
            e->used = true;
            kv->count++;
            return true;
        }
        if (strcmp(e->key, key) == 0) {
            void *newv = malloc(vlen);
            if (!newv && vlen > 0) return false;
            memcpy(newv, val, vlen);
            free(e->value);
            e->value = newv;
            e->vlen = vlen;
            return true;
        }
        idx = (idx + 1) % kv->capacity;
    }
}

void *kv_get(KVStore *kv, const char *key, size_t *vlen_out) {
    if (!kv || !key) return NULL;
    uint64_t h = kv_hash(key);
    size_t idx = h % kv->capacity;

    for (;;) {
        KVEntry *e = &kv->entries[idx];
        if (!e->used) return NULL;
        if (strcmp(e->key, key) == 0) {
            if (vlen_out) *vlen_out = e->vlen;
            return e->value;
        }
        idx = (idx + 1) % kv->capacity;
    }
}

bool kv_remove(KVStore *kv, const char *key) {
    if (!kv || !key) return false;
    uint64_t h = kv_hash(key);
    size_t idx = h % kv->capacity;

    for (;;) {
        KVEntry *e = &kv->entries[idx];
        if (!e->used) return false;
        if (strcmp(e->key, key) == 0) {
            free(e->key);
            free(e->value);
            e->key = NULL;
            e->value = NULL;
            e->vlen = 0;
            e->used = false;
            kv->count--;
            return true;
        }
        idx = (idx + 1) % kv->capacity;
    }
}

void kv_clear(KVStore *kv) {
    if (!kv) return;
    for (size_t i = 0; i < kv->capacity; i++) {
        if (kv->entries[i].used) {
            free(kv->entries[i].key);
            free(kv->entries[i].value);
            kv->entries[i].key = NULL;
            kv->entries[i].value = NULL;
            kv->entries[i].vlen = 0;
            kv->entries[i].used = false;
        }
    }
    kv->count = 0;
}

size_t kv_size(KVStore *kv) {
    return kv ? kv->count : 0;
}

static bool kv_resize(KVStore *kv, size_t newcap) {
    KVEntry *old = kv->entries;
    size_t oldcap = kv->capacity;

    KVEntry *newtab = calloc(newcap, sizeof(KVEntry));
    if (!newtab) return false;

    kv->entries = newtab;
    kv->capacity = newcap;
    kv->count = 0;

    for (size_t i = 0; i < oldcap; i++) {
        KVEntry *e = &old[i];
        if (e->used) {
            kv_put(kv, e->key, e->value, e->vlen);
            free(e->key);
            free(e->value);
        }
    }
    free(old);
    return true;
}

// Iteration
typedef struct {
    KVStore *kv;
    size_t   index;
} KVIter;

KVIter kv_iter(KVStore *kv) {
    KVIter it = { kv, 0 };
    return it;
}

bool kv_next(KVIter *it, const char **key_out, void **val_out, size_t *vlen_out) {
    if (!it || !it->kv) return false;
    while (it->index < it->kv->capacity) {
        KVEntry *e = &it->kv->entries[it->index++];
        if (e->used) {
            if (key_out) *key_out = e->key;
            if (val_out) *val_out = e->value;
            if (vlen_out) *vlen_out = e->vlen;
            return true;
        }
    }
    return false;
}

// Demo
#ifdef KV_DEMO
int main(void) {
    KVStore *kv = kv_create();
    int v1 = 42;
    kv_put(kv, "answer", &v1, sizeof(v1));
    size_t len;
    int *got = kv_get(kv, "answer", &len);
    if (got) printf("answer=%d\n", *got);
    kv_destroy(kv);
}
#endif