/* ============================================================================
   jumptab.c — Table de dispatch générique (C17, MIT)
   - Table 256 entrées pour interpréteur/VM, handlers à la volée
   - Handler signature souple avec accès au flux et à l’IP modifiable
   - Profilage par opcode (compteurs 64-bit) + dump
   - API autonome si jumptab.h absent
   ============================================================================
*/
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ----------------------------------------------------------------------------
   API publique si jumptab.h absent
---------------------------------------------------------------------------- */
#ifndef VT_JUMPTAB_HAVE_HEADER
#ifndef VT_JT_API
#define VT_JT_API extern
#endif

/* Handler:
   - ctx     : état utilisateur
   - op      : opcode courant
   - code,len: flux d’octets
   - ip      : index dans le flux; le handler peut avancer ip (ex: consommer imm)
*/
typedef void (*vt_jt_handler)(void* ctx, uint8_t op,
                              const uint8_t* code, size_t len, size_t* ip);

typedef struct {
  vt_jt_handler table[256];
  vt_jt_handler def_handler;    /* fallback si non défini */
  uint64_t      hits[256];      /* compteurs par opcode */
  int           profile_on;     /* 0/1 */
} vt_jumptab;

VT_JT_API void vt_jt_init(vt_jumptab* jt, vt_jt_handler def_handler);
VT_JT_API void vt_jt_reset(vt_jumptab* jt);                   /* retire handlers, garde profil on/off */
VT_JT_API void vt_jt_set(vt_jumptab* jt, uint8_t op, vt_jt_handler h);
VT_JT_API vt_jt_handler vt_jt_get(const vt_jumptab* jt, uint8_t op);

VT_JT_API void vt_jt_profile(vt_jumptab* jt, int on);         /* active/désactive */
VT_JT_API void vt_jt_profile_clear(vt_jumptab* jt);
VT_JT_API void vt_jt_profile_dump(const vt_jumptab* jt, FILE* out);

VT_JT_API void vt_jt_dispatch_one(vt_jumptab* jt, void* ctx,
                                  uint8_t op, const uint8_t* code,
                                  size_t len, size_t* ip);

/* Boucle simple: lit code[0..len-1], appelle handlers.
   S’arrête si *ip déborde ou si un handler place *ip>=len. */
VT_JT_API void vt_jt_run_stream(vt_jumptab* jt, void* ctx,
                                const uint8_t* code, size_t len);

#endif /* VT_JUMPTAB_HAVE_HEADER */

/* ----------------------------------------------------------------------------
   Implémentation
---------------------------------------------------------------------------- */
static void vt__noop(void* ctx, uint8_t op, const uint8_t* code, size_t len, size_t* ip) {
  (void)ctx; (void)op; (void)code; (void)len; (void)ip;
}

void vt_jt_init(vt_jumptab* jt, vt_jt_handler def_handler) {
  if (!jt) return;
  for (int i = 0; i < 256; i++) jt->table[i] = NULL;
  jt->def_handler = def_handler ? def_handler : vt__noop;
  memset(jt->hits, 0, sizeof jt->hits);
  jt->profile_on = 0;
}

void vt_jt_reset(vt_jumptab* jt) {
  if (!jt) return;
  for (int i = 0; i < 256; i++) jt->table[i] = NULL;
  jt->def_handler = vt__noop;
}

void vt_jt_set(vt_jumptab* jt, uint8_t op, vt_jt_handler h) {
  if (!jt) return;
  jt->table[op] = h;
}

vt_jt_handler vt_jt_get(const vt_jumptab* jt, uint8_t op) {
  if (!jt) return NULL;
  return jt->table[op] ? jt->table[op] : jt->def_handler;
}

void vt_jt_profile(vt_jumptab* jt, int on) {
  if (!jt) return;
  jt->profile_on = on ? 1 : 0;
}

void vt_jt_profile_clear(vt_jumptab* jt) {
  if (!jt) return;
  memset(jt->hits, 0, sizeof jt->hits);
}

void vt_jt_profile_dump(const vt_jumptab* jt, FILE* out) {
  if (!jt) return;
  if (!out) out = stderr;
  uint64_t total = 0;
  for (int i = 0; i < 256; i++) total += jt->hits[i];
  fprintf(out, "jumptab profile: total=%llu\n", (unsigned long long)total);
  for (int i = 0; i < 256; i++) {
    if (jt->hits[i]) {
      double pct = total ? (100.0 * (double)jt->hits[i] / (double)total) : 0.0;
      fprintf(out, "  0x%02X  hits=%llu  %.2f%%\n",
              i, (unsigned long long)jt->hits[i], pct);
    }
  }
}

/* Dispatch d’un seul opcode */
void vt_jt_dispatch_one(vt_jumptab* jt, void* ctx,
                        uint8_t op, const uint8_t* code,
                        size_t len, size_t* ip) {
  if (!jt) return;
  if (jt->profile_on) jt->hits[op]++;
  vt_jt_handler h = jt->table[op];
  if (!h) h = jt->def_handler;
  h(ctx, op, code, len, ip);
}

/* Boucle séquentielle */
void vt_jt_run_stream(vt_jumptab* jt, void* ctx,
                      const uint8_t* code, size_t len) {
  if (!jt || !code) return;
  size_t ip = 0;
  while (ip < len) {
    uint8_t op = code[ip++];
    size_t before = ip;
    vt_jt_dispatch_one(jt, ctx, op, code, len, &ip);
    /* Sécurité: empêche blocage si handler n’avance pas */
    if (ip < before) ip = before;      /* pas de recul implicite */
    if (ip == before) {                /* handler n’a rien consommé */
      /* Par défaut, on continue. L’utilisateur peut faire ip=len pour stop. */
    }
  }
}

/* ----------------------------------------------------------------------------
   Tests optionnels
   cc -std=c17 -DVT_JUMPTAB_TEST jumptab.c
---------------------------------------------------------------------------- */
#ifdef VT_JUMPTAB_TEST
#include <assert.h>

typedef struct {
  int acc;
} VM;

static void OP_ADD1(void* c, uint8_t op, const uint8_t* code, size_t len, size_t* ip){
  (void)op; (void)code; (void)len;
  ((VM*)c)->acc += 1;
}
static void OP_ADDK(void* c, uint8_t op, const uint8_t* code, size_t len, size_t* ip){
  (void)op; (void)len;
  if (*ip < len) {
    ((VM*)c)->acc += (int)code[(*ip)++];
  }
}
static void OP_HALT(void* c, uint8_t op, const uint8_t* code, size_t len, size_t* ip){
  (void)c; (void)op; (void)code; (void)len;
  *ip = len; /* stop */
}
static void OP_DEF(void* c, uint8_t op, const uint8_t* code, size_t len, size_t* ip){
  (void)code; (void)len; (void)ip;
  ((VM*)c)->acc -= (int)op; /* juste pour voir que fallback est appelé */
}

int main(void){
  vt_jumptab jt; vt_jt_init(&jt, OP_DEF);
  vt_jt_profile(&jt, 1);
  vt_jt_set(&jt, 0x01, OP_ADD1);
  vt_jt_set(&jt, 0x02, OP_ADDK);
  vt_jt_set(&jt, 0xFF, OP_HALT);

  /* code: ADD1, ADDK 5, ADD1, HALT */
  uint8_t code[] = {0x01, 0x02, 5, 0x01, 0xFF};
  VM vm = {.acc = 0};
  vt_jt_run_stream(&jt, &vm, code, sizeof code);
  assert(vm.acc == 7);
  vt_jt_profile_dump(&jt, stdout);
  return 0;
}
#endif
