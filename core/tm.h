/* ============================================================================
   tm.h — Horloges, dates, ISO-8601, timers (C17)
   Expose vt_time (epoch sec + nsec) et vt_timer. Implémentation: tm.c
   Licence: MIT.
   ============================================================================
 */
#ifndef VT_TM_H
#define VT_TM_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h> /* int64_t, uint64_t, int32_t */
#include <time.h>   /* struct tm */

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
   Export
--------------------------------------------------------------------------- */
#ifndef VT_TM_API
#define VT_TM_API extern
#endif

/* --------------------------------------------------------------------------
   Types
--------------------------------------------------------------------------- */
typedef struct vt_time {
  int64_t sec;  /* secondes depuis epoch (UTC) */
  int32_t nsec; /* 0..999,999,999 */
} vt_time;

typedef struct vt_timer {
  uint64_t start_ns;
  uint64_t elapsed_ns;
  int running; /* bool-like */
} vt_timer;

/* --------------------------------------------------------------------------
   Horloges
--------------------------------------------------------------------------- */
/* Monotone en nanosecondes. */
VT_TM_API uint64_t vt_ns_now_monotonic(void);
VT_TM_API uint64_t vt_ms_now_monotonic(void);
VT_TM_API uint64_t vt_us_now_monotonic(void);

/* Mur UTC → vt_time courant. */
VT_TM_API int vt_utc_now(vt_time* out);

/* Conversions UTC/local ⇄ struct tm (nsec fourni séparément à l’aller). */
VT_TM_API int vt_time_to_tm_utc(const vt_time* t, struct tm* out_tm);
VT_TM_API int vt_time_to_tm_local(const vt_time* t, struct tm* out_tm);
VT_TM_API int vt_time_from_tm_utc(const struct tm* in_tm, long nsec,
                                  vt_time* out);
VT_TM_API int vt_time_from_tm_local(const struct tm* in_tm, long nsec,
                                    vt_time* out);

/* Décalage local vs UTC en minutes (DST inclus) pour un epoch donné. */
VT_TM_API int vt_local_offset_minutes(int64_t epoch_sec);

/* Sleeps */
VT_TM_API int vt_sleep_ns(uint64_t ns);
VT_TM_API int vt_sleep_ms(uint64_t ms);
VT_TM_API int vt_sleep_until_ns(uint64_t deadline_ns /* monotone ns */);

/* Ops de base */
VT_TM_API vt_time vt_time_add_ns(vt_time t, int64_t ns);
VT_TM_API int64_t vt_time_diff_ns(vt_time a, vt_time b); /* a-b en ns */

/* --------------------------------------------------------------------------
   ISO-8601 / RFC3339
   - format_* renvoie nombre d’octets écrits (sans NUL) ou <0 si erreur.
   - with_frac=1 pour inclure .fffffffff si nsec>0.
--------------------------------------------------------------------------- */
VT_TM_API int vt_time_format_iso8601_utc(const vt_time* t, char* dst,
                                         size_t cap, int with_frac);
VT_TM_API int vt_time_format_iso8601_local(const vt_time* t, char* dst,
                                           size_t cap, int with_frac);
VT_TM_API int vt_time_parse_iso8601(const char* z, vt_time* out);

VT_TM_API int vt_time_format_rfc3339_utc(const vt_time* t, char* dst,
                                         size_t cap);
VT_TM_API int vt_time_format_rfc3339_local(const vt_time* t, char* dst,
                                           size_t cap);

/* --------------------------------------------------------------------------
   Timers
--------------------------------------------------------------------------- */
VT_TM_API void vt_timer_start(vt_timer* w);
VT_TM_API void vt_timer_stop(vt_timer* w);
VT_TM_API void vt_timer_resume(vt_timer* w);
VT_TM_API uint64_t vt_timer_elapsed_ns(const vt_timer* w);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* VT_TM_H */
