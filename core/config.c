// SPDX-License-Identifier: MIT
/* ============================================================================
   core/config.c — Parser/stockage clé=valeur (INI/CFG) pour Vitte/Vitl
   C11, portable POSIX/Windows. API définie dans core/config.h.
   ============================================================================
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------- */
/* Helpers internes                                                           */
/* -------------------------------------------------------------------------- */
static inline char* xstrdup(const char* s) {
  size_t n = strlen(s) + 1;
  char* d = (char*)malloc(n);
  if (!d) { fprintf(stderr, "config: OOM\n"); abort(); }
  memcpy(d, s, n);
  return d;
}

static inline void* xrealloc(void* p, size_t n) {
  void* q = realloc(p, n ? n : 1);
  if (!q) { fprintf(stderr, "config: realloc OOM (%zu)\n", n); abort(); }
  return q;
}

static char* trim(char* s) {
  while (isspace((unsigned char)*s)) s++;
  if (*s == 0) return s;
  char* end = s + strlen(s) - 1;
  while (end > s && isspace((unsigned char)*end)) *end-- = 0;
  return s;
}

/* -------------------------------------------------------------------------- */
/* Init/free                                                                  */
/* -------------------------------------------------------------------------- */
API_EXPORT void config_init(config_t* c) {
  c->s = NULL; c->n = 0; c->cap = 0;
}
API_EXPORT void config_free(config_t* c) {
  for (size_t i=0;i<c->n;i++) {
    config_section* sec = &c->s[i];
    free(sec->name);
    for (size_t j=0;j<sec->n;j++) {
      free(sec->kv[j].key);
      free(sec->kv[j].val);
    }
    free(sec->kv);
  }
  free(c->s);
  c->s=NULL; c->n=0; c->cap=0;
}

/* -------------------------------------------------------------------------- */
/* Internal: get or create section                                            */
/* -------------------------------------------------------------------------- */
static config_section* config_getsec_mut(config_t* c, const char* name, bool create) {
  for (size_t i=0;i<c->n;i++) {
    if (strcmp(c->s[i].name, name)==0) return &c->s[i];
  }
  if (!create) return NULL;
  if (c->n == c->cap) {
    size_t nc = c->cap? c->cap*2:4;
    c->s = (config_section*)xrealloc(c->s, nc*sizeof(config_section));
    c->cap = nc;
  }
  config_section* sec = &c->s[c->n++];
  sec->name = xstrdup(name);
  sec->kv = NULL; sec->n=0; sec->cap=0;
  return sec;
}

static void config_putkv(config_section* sec, const char* k, const char* v) {
  for (size_t i=0;i<sec->n;i++) {
    if (strcmp(sec->kv[i].key,k)==0) {
      free(sec->kv[i].val);
      sec->kv[i].val = xstrdup(v);
      return;
    }
  }
  if (sec->n == sec->cap) {
    size_t nc = sec->cap? sec->cap*2:4;
    sec->kv = (config_kv*)xrealloc(sec->kv, nc*sizeof(config_kv));
    sec->cap = nc;
  }
  sec->kv[sec->n].key = xstrdup(k);
  sec->kv[sec->n].val = xstrdup(v);
  sec->n++;
}

/* -------------------------------------------------------------------------- */
/* Load file                                                                  */
/* -------------------------------------------------------------------------- */
API_EXPORT int config_load_file(config_t* c, const char* path) {
  FILE* f = fopen(path,"r");
  if (!f) return -1;
  char line[1024];
  config_section* cur = config_getsec_mut(c,"",true); /* global section */
  while (fgets(line,sizeof(line),f)) {
    char* s = trim(line);
    if (*s=='#' || *s==';' || *s==0) continue;
    if (*s=='[') {
      char* e = strchr(s,']');
      if (!e) continue;
      *e=0;
      cur = config_getsec_mut(c, trim(s+1), true);
      continue;
    }
    char* eq = strchr(s,'=');
    if (!eq) continue;
    *eq=0;
    char* k = trim(s);
    char* v = trim(eq+1);
    config_putkv(cur,k,v);
  }
  fclose(f);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Lookup                                                                     */
/* -------------------------------------------------------------------------- */
API_EXPORT const char* config_get(const config_t* c, const char* section, const char* key) {
  for (size_t i=0;i<c->n;i++) {
    if (strcmp(c->s[i].name, section)==0) {
      for (size_t j=0;j<c->s[i].n;j++) {
        if (strcmp(c->s[i].kv[j].key,key)==0)
          return c->s[i].kv[j].val;
      }
    }
  }
  return NULL;
}
API_EXPORT const char* config_get_def(const config_t* c, const char* section, const char* key, const char* def) {
  const char* v = config_get(c,section,key);
  return v? v: def;
}
API_EXPORT long config_get_long(const config_t* c, const char* section, const char* key, long def) {
  const char* v = config_get(c,section,key);
  if (!v) return def;
  char* end=NULL;
  long x = strtol(v,&end,0);
  if (!end||*end) return def;
  return x;
}
API_EXPORT double config_get_double(const config_t* c, const char* section, const char* key, double def) {
  const char* v = config_get(c,section,key);
  if (!v) return def;
  char* end=NULL;
  double x = strtod(v,&end);
  if (!end||*end) return def;
  return x;
}
API_EXPORT bool config_get_bool(const config_t* c, const char* section, const char* key, bool def) {
  const char* v = config_get(c,section,key);
  if (!v) return def;
  if (strcasecmp(v,"true")==0 || strcmp(v,"1")==0 || strcasecmp(v,"yes")==0 || strcasecmp(v,"on")==0)
    return true;
  if (strcasecmp(v,"false")==0 || strcmp(v,"0")==0 || strcasecmp(v,"no")==0 || strcasecmp(v,"off")==0)
    return false;
  return def;
}

/* -------------------------------------------------------------------------- */
/* Debug / dump                                                               */
/* -------------------------------------------------------------------------- */
API_EXPORT void config_dump(const config_t* c, FILE* out) {
  for (size_t i=0;i<c->n;i++) {
    const config_section* sec=&c->s[i];
    if (sec->name[0]) fprintf(out,"[%s]\n",sec->name);
    for (size_t j=0;j<sec->n;j++) {
      fprintf(out,"%s=%s\n",sec->kv[j].key, sec->kv[j].val);
    }
  }
}

/* ========================================================================= */
#ifdef CONFIG_DEMO_MAIN
int main(int argc, char**argv){
  config_t c; config_init(&c);
  if (argc>1) config_load_file(&c, argv[1]);
  printf("foo=%s\n", config_get_def(&c,"","foo","<none>"));
  config_dump(&c, stdout);
  config_free(&c);
  return 0;
}
#endif
