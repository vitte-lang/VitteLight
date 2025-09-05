/* ============================================================================
   state.h — État global du compilateur/VM Vitte/Vitl
   API C17, UTF-8. Licence MIT.
   - Gestion de configuration, interning, sources, parsing, diagnostics.
   - Types opaques. Thread-safe côté implémentation.
   ============================================================================
 */
#ifndef VT_STATE_H
#define VT_STATE_H
#pragma once
#define VT_STATE_H_SENTINEL 1

#include <stddef.h> /* size_t */
#include <stdio.h>  /* FILE */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
   Export
--------------------------------------------------------------------------- */
#ifndef VT_STATE_API
#define VT_STATE_API extern
#endif

/* ---------------------------------------------------------------------------
   Types opaques
--------------------------------------------------------------------------- */
typedef struct vt_state vt_state;

/* Configuration initiale.
   - log_level: 0..5 = TRACE..FATAL (cf. debug.h) ; par défaut 1 (DEBUG)
   - use_color: 1 active les SGR sur TTY
   - module_search_path: chaîne style "std:.;lib" (nullable)
   - arena_reserve: taille d’amorçage de l’arène interne en octets (hint)
   - interner_init: capacité initiale de l’interner (entrée hash)            */
typedef struct vt_state_config {
  int log_level;
  int use_color;
  const char* module_search_path;
  size_t arena_reserve;
  size_t interner_init;
} vt_state_config;

/* ---------------------------------------------------------------------------
   Cycle de vie
--------------------------------------------------------------------------- */
VT_STATE_API vt_state* vt_state_create(const vt_state_config* cfg);
/* Détruit l’état et libère toutes les ressources (arènes, interner, GC, etc.).
 */
VT_STATE_API void vt_state_destroy(vt_state* st);

/* ---------------------------------------------------------------------------
   Sources et parsing
--------------------------------------------------------------------------- */
/* Ajoute une source:
     - path: chemin normalisé ou arbitraire (UTF-8). Interné en interne.
     - contents_opt_utf8: si non-NULL, parse depuis ce buffer; sinon charge
   path. Retours: 0=OK, 1=déjà présent, <0=erreur E/S. */
VT_STATE_API int vt_state_add_source(vt_state* st, const char* path,
                                     const char* contents_opt_utf8);

/* Parse toutes les sources enregistrées.
   Retourne 0 si tout va bien, <0 si des erreurs de parsing ont été détectées
   ou si le parser n’est pas disponible. */
VT_STATE_API int vt_state_parse_all(vt_state* st);

/* Dump lisible de tous les AST et diagnostics. out=NULL → stderr. */
VT_STATE_API void vt_state_dump_ast(FILE* out, vt_state* st);

/* ---------------------------------------------------------------------------
   Interning (chaînes uniques, stables)
--------------------------------------------------------------------------- */
/* Intern une chaîne C terminée par '\0'. Retourne un pointeur stable (lifetime
 * = st). */
VT_STATE_API const char* vt_intern_cstr(vt_state* st, const char* s);
/* Intern depuis (s,n). Retourne un identifiant dérivé du hash (stable dans le
 * run). */
VT_STATE_API size_t vt_intern_id(vt_state* st, const char* s, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_STATE_H */
