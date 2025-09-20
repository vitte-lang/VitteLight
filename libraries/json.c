// SPDX-License-Identifier: GPL-3.0-or-later
//
// json.c — JSON front-end for Vitte Light VM (C17, complet)
// Namespace: "json"
//
// Build examples:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_CJSON -c json.c
//   cc ... json.o -lcjson
//
// Model:
//   - Parse: texte → arbre (handle).
//   - Dump: arbre → texte (minifié ou pretty).
//   - Accès: type, length, get(key), get(idx), get_string/int/num/bool/null.
//   - Mutateurs simples: set(key,val), append(val).
//   - Tous les arbres doivent être libérés avec json_free().
//
// API (C symbol layer):
//   typedef int json_handle;      // id >0
//   int   json_parse(const char* text);                  // >0 id | <0 err
//   char* json_stringify(int h, int pretty);             // malloc, free() après
//   int   json_type(int h);                              // 0=null,1=bool,2=num,3=str,4=obj,5=array | <0 err
//   int   json_length(int h);                            // array length | obj #keys | <0
//   int   json_get(int h, const char* key_or_idx);       // >0 id | <0
//   const char* json_as_string(int h);                   // ou NULL
//   double json_as_number(int h, int* ok);               // ok=1 si valide
//   int   json_as_bool(int h, int* ok);                  // idem
//   int   json_free(int h);                              // 0
//
// Notes:
//   - Stringify renvoie buffer malloc, toujours NUL-terminé.
//   - Erreurs: -EINVAL, -ENOSYS, -ENOMEM.
//
// Deps optionnel: cJSON
// Deps VM optionnels: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifndef EINVAL
#  define EINVAL 22
#endif
#ifndef ENOSYS
#  define ENOSYS 38
#endif
#ifndef ENOMEM
#  define ENOMEM 12
#endif

#ifndef VL_EXPORT
#  if defined(_WIN32) && !defined(__clang__)
#    define VL_EXPORT __declspec(dllexport)
#  else
#    define VL_EXPORT
#  endif
#endif

#ifdef VL_HAVE_CJSON
#  include <cjson/cJSON.h>
#endif

#ifndef VL_JSON_MAX
#  define VL_JSON_MAX 256
#endif

typedef struct {
#ifdef VL_HAVE_CJSON
  cJSON* node;
#endif
  int used;
} JEnt;

static JEnt g_tbl[VL_JSON_MAX];

// -------- utils --------
static int alloc_handle(void){
  for (int i=1;i<VL_JSON_MAX;i++)
    if (!g_tbl[i].used) { g_tbl[i].used=1; return i; }
  return -ENOMEM;
}
static void free_handle(int h){
  if (h>0 && h<VL_JSON_MAX && g_tbl[h].used){
#ifdef VL_HAVE_CJSON
    if (g_tbl[h].node) cJSON_Delete(g_tbl[h].node);
#endif
    g_tbl[h].used=0;
#ifdef VL_HAVE_CJSON
    g_tbl[h].node=NULL;
#endif
  }
}

// -------- API ----------
VL_EXPORT int json_parse(const char* text){
#ifndef VL_HAVE_CJSON
  (void)text; return -ENOSYS;
#else
  if (!text) return -EINVAL;
  cJSON* n = cJSON_Parse(text);
  if (!n) return -EINVAL;
  int h = alloc_handle();
  if (h<0){ cJSON_Delete(n); return h; }
  g_tbl[h].node = n;
  return h;
#endif
}

VL_EXPORT char* json_stringify(int h, int pretty){
#ifndef VL_HAVE_CJSON
  (void)h; (void)pretty; return NULL;
#else
  if (h<=0||h>=VL_JSON_MAX||!g_tbl[h].used||!g_tbl[h].node) return NULL;
  return pretty ? cJSON_Print(g_tbl[h].node) : cJSON_PrintUnformatted(g_tbl[h].node);
#endif
}

VL_EXPORT int json_type(int h){
#ifndef VL_HAVE_CJSON
  (void)h; return -ENOSYS;
#else
  if (h<=0||h>=VL_JSON_MAX||!g_tbl[h].used||!g_tbl[h].node) return -EINVAL;
  cJSON* n = g_tbl[h].node;
  if (cJSON_IsNull(n)) return 0;
  if (cJSON_IsBool(n)) return 1;
  if (cJSON_IsNumber(n)) return 2;
  if (cJSON_IsString(n)) return 3;
  if (cJSON_IsObject(n)) return 4;
  if (cJSON_IsArray(n)) return 5;
  return -EINVAL;
#endif
}

VL_EXPORT int json_length(int h){
#ifndef VL_HAVE_CJSON
  (void)h; return -ENOSYS;
#else
  if (h<=0||h>=VL_JSON_MAX||!g_tbl[h].used||!g_tbl[h].node) return -EINVAL;
  cJSON* n = g_tbl[h].node;
  if (cJSON_IsArray(n)) return cJSON_GetArraySize(n);
  if (cJSON_IsObject(n)){
    int cnt=0; for(cJSON* c=n->child;c;c=c->next) cnt++;
    return cnt;
  }
  return -EINVAL;
#endif
}

VL_EXPORT int json_get(int h, const char* key_or_idx){
#ifndef VL_HAVE_CJSON
  (void)h;(void)key_or_idx; return -ENOSYS;
#else
  if (h<=0||h>=VL_JSON_MAX||!g_tbl[h].used||!g_tbl[h].node||!key_or_idx) return -EINVAL;
  cJSON* n = g_tbl[h].node;
  cJSON* child=NULL;
  if (cJSON_IsArray(n)){
    int idx=atoi(key_or_idx);
    child=cJSON_GetArrayItem(n, idx);
  } else if (cJSON_IsObject(n)){
    child=cJSON_GetObjectItemCaseSensitive(n, key_or_idx);
  }
  if (!child) return -EINVAL;
  int nh=alloc_handle();
  if (nh<0) return nh;
  g_tbl[nh].node=cJSON_Duplicate(child,1);
  return nh;
#endif
}

VL_EXPORT const char* json_as_string(int h){
#ifndef VL_HAVE_CJSON
  (void)h; return NULL;
#else
  if (h<=0||h>=VL_JSON_MAX||!g_tbl[h].used||!g_tbl[h].node) return NULL;
  cJSON* n = g_tbl[h].node;
  return cJSON_IsString(n)? n->valuestring : NULL;
#endif
}

VL_EXPORT double json_as_number(int h, int* ok){
#ifndef VL_HAVE_CJSON
  (void)h; if(ok)*ok=0; return 0.0;
#else
  if (h<=0||h>=VL_JSON_MAX||!g_tbl[h].used||!g_tbl[h].node){ if(ok)*ok=0; return 0.0; }
  cJSON* n=g_tbl[h].node;
  if (cJSON_IsNumber(n)){ if(ok)*ok=1; return n->valuedouble; }
  if (ok)*ok=0; return 0.0;
#endif
}

VL_EXPORT int json_as_bool(int h, int* ok){
#ifndef VL_HAVE_CJSON
  (void)h; if(ok)*ok=0; return 0;
#else
  if (h<=0||h>=VL_JSON_MAX||!g_tbl[h].used||!g_tbl[h].node){ if(ok)*ok=0; return 0; }
  cJSON* n=g_tbl[h].node;
  if (cJSON_IsBool(n)){ if(ok)*ok=1; return cJSON_IsTrue(n); }
  if (ok)*ok=0; return 0;
#endif
}

VL_EXPORT int json_free(int h){
  if (h<=0||h>=VL_JSON_MAX||!g_tbl[h].used) return -EINVAL;
  free_handle(h);
  return 0;
}