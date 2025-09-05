/* ============================================================================
   do.h — API programmatique du pilote CLI pour debug.h/debug.c (C17)
   Expose un wrapper propre autour des fonctions "outil" de do.c :
   - Parsing d’arguments
   - Exécution des actions (samples, bench, hexdump, backtrace, fatal)
   - Aides de mapping niveaux/formats
   Lier avec debug.c et l’implémentation correspondante de do.c.
   Licence: MIT.
   ============================================================================
 */
#ifndef VT_DO_H
#define VT_DO_H
#pragma once

#include <stddef.h> /* size_t   */
#include <stdint.h> /* uint64_t */

#include "debug.h" /* vt_log_level, vt_log_format, vt_log_config */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
   Export / annotations
--------------------------------------------------------------------------- */
#ifndef VT_DO_API
#define VT_DO_API extern
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VT_DO_PRINTF(fmt_idx, va_idx) \
  __attribute__((format(printf, fmt_idx, va_idx)))
#else
#define VT_DO_PRINTF(fmt_idx, va_idx)
#endif

/* ---------------------------------------------------------------------------
   Options de l’outil (miroir des flags CLI)
--------------------------------------------------------------------------- */
typedef struct vt_do_opts {
  /* Logger (reprend vt_log_config) */
  vt_log_config log;

  /* Actions */
  int show_backtrace;       /* --backtrace */
  const char* hexdump_path; /* --hexdump <file> */
  uint64_t bench_n;         /* --bench <N> */
  const char* bench_msg;    /* --message */
  int emit_sample;          /* --emit-sample */
  int fatal;                /* --fatal */

  /* Interne: impression usage si demandé */
  int want_help;
} vt_do_opts;

/* ---------------------------------------------------------------------------
   Initialisation et parsing
--------------------------------------------------------------------------- */

/* Remplit des valeurs par défaut cohérentes. */
VT_DO_API void vt_do_default_opts(vt_do_opts* o);

/* Parse argv en options. Renvoie 0 si OK, >0 si erreur.
   - Remplit o->want_help si -h/--help a été rencontré (l’appelant peut alors
   sortir).
*/
VT_DO_API int vt_do_parse_args(int argc, char** argv, vt_do_opts* o);

/* Imprime l’aide (usage) sur stderr. */
VT_DO_API void vt_do_print_usage(const char* prog);

/* ---------------------------------------------------------------------------
   Exécution haut niveau
--------------------------------------------------------------------------- */

/* Exécute le scénario demandé:
   - vt_log_init(o->log)
   - actions (emit_sample/bench/hexdump/backtrace/fatal)
   - vt_log_shutdown()
   Retourne un code de sortie (0 succès).
*/
VT_DO_API int vt_do_run(const vt_do_opts* o);

/* ---------------------------------------------------------------------------
   Actions unitaires (réutilisables)
--------------------------------------------------------------------------- */

/* Emet des messages TRACE..ERROR (pas de FATAL). */
VT_DO_API void vt_do_emit_sample(void);

/* Emet N lignes niveau INFO (bench simple). Force flush à la fin. */
VT_DO_API void vt_do_bench(uint64_t n, const char* msg);

/* Charge un fichier et fait un hexdump via vt_debug_hexdump(). 0 si OK. */
VT_DO_API int vt_do_hexdump_file(const char* path);

/* Demande une backtrace immédiate. */
VT_DO_API void vt_do_backtrace(void);

/* Déclenche un log FATAL (habituellement termine le process). */
VT_DO_API void vt_do_fatal_now(const char* reason) VT_DO_PRINTF(1, 0);

/* ---------------------------------------------------------------------------
   Helpers de mapping niveaux / formats / couleur
--------------------------------------------------------------------------- */

/* Niveaux ↔ texte */
VT_DO_API const char* vt_do_level_name(vt_log_level lvl); /* "trace".."fatal" */
VT_DO_API int vt_do_level_parse(const char* s, vt_log_level* out);

/* Formats ↔ texte */
VT_DO_API const char* vt_do_format_name(vt_log_format f); /* "text"|"json" */
VT_DO_API int vt_do_format_parse(const char* s, vt_log_format* out);

/* Couleur: "auto"|"on"|"off" → out_on (0/1) et out_auto (0/1).
   Renvoie 0 si OK. Exemple: "auto" => out_on=1, out_auto=1. */
VT_DO_API int vt_do_color_parse(const char* s, int* out_on, int* out_auto);

/* Taille avec suffixe (k,m,g en base 1024). 0 si succès. */
VT_DO_API int vt_do_parse_size(const char* s, uint64_t* out_bytes);

/* ---------------------------------------------------------------------------
   Logging direct (raccourcis) — optionnels
--------------------------------------------------------------------------- */

/* Change dynamiquement le niveau minimal. */
VT_DO_API void vt_do_set_level(vt_log_level lvl);

/* Forcer le flush des buffers. */
VT_DO_API void vt_do_flush(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_DO_H */
