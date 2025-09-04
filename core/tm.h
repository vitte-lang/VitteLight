// vitte-light/core/tm.h
// Horloges et minuteurs portables pour VitteLight.
// Implémentation: core/tm.c

#ifndef VITTE_LIGHT_CORE_TM_H
#define VITTE_LIGHT_CORE_TM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#ifndef VL_TM_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define VL_TM_INLINE static __inline__ __attribute__((always_inline))
#else
#define VL_TM_INLINE static __inline
#endif
#endif

// ───────────────────── Horloges ─────────────────────
// Epoch UTC en nanosecondes
uint64_t vl_wall_time_ns(void);
// Horloge monotone en nanosecondes (origine indéfinie)
uint64_t vl_mono_time_ns(void);

// ───────────────────── Veille ─────────────────────
int vl_sleep_ms(uint32_t ms);  // 0 si succès
int vl_sleep_ns(uint64_t ns);  // 0 si succès

// ───────────────────── Chronomètre ─────────────────────
typedef struct {
  uint64_t t0;
} VL_Stopwatch;
VL_TM_INLINE void vl_sw_start(VL_Stopwatch *sw) {
  if (sw) sw->t0 = vl_mono_time_ns();
}
VL_TM_INLINE uint64_t vl_sw_elapsed_ns(const VL_Stopwatch *sw) {
  return sw ? (vl_mono_time_ns() - sw->t0) : 0;
}

// ───────────────────── Deadline ─────────────────────
typedef struct {
  uint64_t due_ns;
} VL_Deadline;
VL_TM_INLINE VL_Deadline vl_deadline_in_ns(uint64_t ns) {
  VL_Deadline d;
  d.due_ns = vl_mono_time_ns() + ns;
  return d;
}
VL_TM_INLINE int vl_deadline_expired(VL_Deadline d) {
  return (vl_mono_time_ns() >= d.due_ns);
}
VL_TM_INLINE uint64_t vl_deadline_remaining_ns(VL_Deadline d) {
  uint64_t now = vl_mono_time_ns();
  return now >= d.due_ns ? 0ull : (d.due_ns - now);
}

// ───────────────────── Format ISO‑8601 ─────────────────────
// Ecrit une date/heure à partir d'un epoch en ns dans buf.
// Retourne nombre d'octets écrits (hors NUL) ou 0 si échec.
size_t vl_time_iso8601_utc(uint64_t epoch_ns, char *buf, size_t n);
size_t vl_time_iso8601_local(uint64_t epoch_ns, char *buf, size_t n);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VITTE_LIGHT_CORE_TM_H
