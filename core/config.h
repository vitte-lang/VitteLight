/* ============================================================================
 * config.h — INI/CFG parser API for Vitte Light
 * C11, portable, zero external deps
 * ============================================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Types
 * -------------------------------------------------------------------------- */

typedef struct {
    char *key;
    char *val;
} config_kv;

typedef struct {
    char *name;
    config_kv *kv;
    size_t n;
    size_t cap;
} config_section;

typedef struct {
    config_section *s;
    size_t n;
    size_t cap;
} config_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Init/Free */
void config_init(config_t *c);
void config_free(config_t *c);

/* Load from file or string */
int  config_load_file(config_t *c, const char *path);   /* return 0 on success, -1 on error */
int  config_load_string(config_t *c, const char *src);  /* parse from in-memory buffer */

/* Query values */
const char *config_get(const config_t *c, const char *section, const char *key);
const char *config_get_default(const config_t *c, const char *section,
                               const char *key, const char *def);

/* Query as numbers */
int   config_get_int(const config_t *c, const char *section, const char *key, int def);
long  config_get_long(const config_t *c, const char *section, const char *key, long def);
double config_get_double(const config_t *c, const char *section, const char *key, double def);
int   config_get_bool(const config_t *c, const char *section, const char *key, int def);

/* Modify */
int config_set(config_t *c, const char *section, const char *key, const char *val);
int config_remove(config_t *c, const char *section, const char *key);

/* Save */
int config_save_file(const config_t *c, const char *path);

#ifdef __cplusplus
}
#endif
#endif /* CONFIG_H */
