// SPDX-License-Identifier: GPL-3.0-or-later
//
// log.c — Journalisation structurée pour Vitte Light VM (C17, portable)
// Namespace: "log"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c log.c
//
// Modèle:
//   - Niveaux: TRACE(10), DEBUG(20), INFO(30), WARN(40), ERROR(50), FATAL(60), OFF(99).
//   - Sortie: stderr (défaut), stdout, ou fichier (append, rotation par reopen).
//   - Format: texte lisible ou JSON compact. Timestamp ISO-8601 UTC ou epoch_ms.
//   - Champs additionnels via table {k=v}. Binaire sûr pour message principal.
//   - Couleurs ANSI optionnelles sur TTY pour WARN/ERROR/FATAL en mode texte.
//
// API (VM):
//   log.set_level(level:string|int)         -> true | (nil,errmsg)
//   log.get_level()                         -> int
//   log.set_output(dest:string)             -> true | (nil,errmsg)   -- "stderr"|"stdout"|"/path/log.txt"
//   log.rotate()                            -> true | (nil,errmsg)   -- rouvre le fichier courant
//   log.set_color(on:int)                   -> true                  -- 0/1
//   log.set_json(on:int)                    -> true                  -- 0/1
//   log.set_time(fmt:string)                -> true | (nil,errmsg)   -- "iso8601"|"epoch_ms"
//   log.set_prefix(prefix:string)           -> true                  -- préfixe fixe pour toutes lignes
//   log.flush()                             -> true | (nil,errmsg)
//
//   log.write(level, message[, fields:table]) -> true | (nil,errmsg)
//   log.trace(msg[, fields]) -> true | (nil,errmsg)
//   log.debug(msg[, fields]) -> true | (nil,errmsg)
//   log.info (msg[, fields]) -> true | (nil,errmsg)
//   log.warn (msg[, fields]) -> true | (nil,errmsg)
//   log.error(msg[, fields]) -> true | (nil,errmsg)
//   log.fatal(msg[, fields]) -> true | (nil,errmsg)
//
// Erreurs: "EINVAL", "EIO", "ENOMEM"
//
// Deps VM: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#include "auxlib.h"
#include "state.h"
#include "object.h"
#include "vm.h"

/* ========================= VM ADAPTER (extern fournis) ================== */

typedef struct VLState VLState;
struct vl_Reg { const char *name; int (*fn)(VLState *L); };

extern void        vlx_register_module(VLState *L, const char *ns, const struct vl_Reg *funcs);
extern void        vlx_push_nil     (VLState *L);
extern void        vlx_push_boolean (VLState *L, int v);
extern void        vlx_push_integer (VLState *L, int64_t v);
extern void        vlx_push_string  (VLState *L, const char *s);
extern const char* vlx_check_string (VLState *L, int idx, size_t *len);
extern const char* vlx_opt_string   (VLState *L, int idx, const char *def, size_t *len);
extern int64_t     vlx_opt_integer  (VLState *L, int idx, int64_t def);
extern int         vlx_opt_boolean  (VLState *L, int idx, int def);
extern int         vlx_table_foreach_kv_string(VLState *L, int idx,
                    int (*cb)(const char *k, const char *v, void *ud), void *ud);

static inline void        vl_push_nil(VLState *L){ vlx_push_nil(L); }
static inline void        vl_push_boolean(VLState *L,int v){ vlx_push_boolean(L,v); }
static inline void        vl_push_integer(VLState *L,int64_t v){ vlx_push_integer(L,v); }
static inline void        vl_push_string(VLState *L,const char*s){ vlx_push_string(L,s); }
static inline const char* vl_check_string(VLState *L,int i,size_t*n){ return vlx_check_string(L,i,n); }
static inline const char* vl_opt_string(VLState *L,int i,const char*d,size_t*n){ return vlx_opt_string(L,i,d,n); }
static inline int64_t     vl_opt_integer(VLState *L,int i,int64_t d){ return vlx_opt_integer(L,i,d); }
static inline int         vl_opt_boolean(VLState *L,int i,int d){ return vlx_opt_boolean(L,i,d); }
static inline int         vl_table_foreach_kv_string(VLState *L, int idx,
                      int (*cb)(const char*, const char*, void*), void *ud){
  return vlx_table_foreach_kv_string(L, idx, cb, ud);
}
static inline void        vl_register_module(VLState *L,const char*ns,const struct vl_Reg*f){ vlx_register_module(L,ns,f); }

/* ================================ State ================================= */

enum { L_TRACE=10, L_DEBUG=20, L_INFO=30, L_WARN=40, L_ERROR=50, L_FATAL=60, L_OFF=99 };
typedef enum { TF_ISO=0, TF_EPOCH_MS=1 } timefmt_t;

static struct {
  int level;
  int color;
  int json;
  timefmt_t tfmt;
  char prefix[64];
  FILE *out;
  char out_path[512]; /* si fichier actif */
  int is_tty;
} G = {
  .level = L_INFO,
  .color = 1,
  .json  = 0,
  .tfmt  = TF_ISO,
  .prefix = "",
  .out = NULL,
  .out_path = "",
  .is_tty = 0
};

#if defined(_WIN32)
#include <io.h>
#include <Windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

static void log_init_once(void){
  if(!G.out){ G.out = stderr; }
  if(G.out==stderr || G.out==stdout) G.is_tty = isatty(fileno(G.out)) ? 1:0;
}

/* ================================ Utils ================================= */

static uint64_t now_epoch_ms(void){
#if defined(CLOCK_REALTIME)
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#else
  struct timespec ts; timespec_get(&ts, TIME_UTC);
  return (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#endif
}

static void fmt_iso8601_utc(char *buf, size_t n){
  time_t t = (time_t)(now_epoch_ms()/1000);
  struct tm tm; gmtime_r(&t, &tm);
  /* YYYY-MM-DDTHH:MM:SSZ */
  (void)strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static const char* level_name(int lvl){
  switch(lvl){
    case L_TRACE: return "TRACE";
    case L_DEBUG: return "DEBUG";
    case L_INFO:  return "INFO";
    case L_WARN:  return "WARN";
    case L_ERROR: return "ERROR";
    case L_FATAL: return "FATAL";
    default: return "LOG";
  }
}

static const char* level_color(int lvl){
  if(!G.color || !G.is_tty) return "";
  switch(lvl){
    case L_WARN:  return "\x1b[33m"; /* yellow */
    case L_ERROR: return "\x1b[31m"; /* red */
    case L_FATAL: return "\x1b[41m\x1b[97m"; /* white on red */
    default: return "\x1b[0m";
  }
}
static const char* color_reset(void){
  return (G.color && G.is_tty) ? "\x1b[0m" : "";
}

/* échappement JSON minimal pour clés/valeurs (UTF-8 supposé) */
static int json_escape(FILE *f, const char *s){
  for(const unsigned char *p=(const unsigned char*)s; *p; ++p){
    unsigned c = *p;
    switch(c){
      case '\"': fputs("\\\"", f); break;
      case '\\': fputs("\\\\", f); break;
      case '\b': fputs("\\b",  f); break;
      case '\f': fputs("\\f",  f); break;
      case '\n': fputs("\\n",  f); break;
      case '\r': fputs("\\r",  f); break;
      case '\t': fputs("\\t",  f); break;
      default:
        if(c < 0x20){ fprintf(f, "\\u%04X", c); }
        else fputc((int)c, f);
    }
  }
  return 0;
}

struct kv_ctx { FILE *f; int first; };
static int fields_json_cb(const char *k, const char *v, void *ud){
  struct kv_ctx *c = (struct kv_ctx*)ud;
  if(!c->first) fputc(',', c->f);
  c->first = 0;
  fputc('"', c->f); json_escape(c->f, k); fputc('"', c->f); fputc(':', c->f); fputc('"', c->f); json_escape(c->f, v); fputc('"', c->f);
  return 0;
}
static int fields_text_cb(const char *k, const char *v, void *ud){
  struct kv_ctx *c = (struct kv_ctx*)ud;
  (void)c;
  fputc(' ', G.out);
  fputc('[', G.out);
  fputs(k, G.out);
  fputc('=', G.out);
  fputs(v, G.out);
  fputc(']', G.out);
  return 0;
}

/* =============================== Core write ============================= */

static int write_record(int lvl, const char *msg, size_t mlen, int has_fields, VLState *L, int fidx){
  log_init_once();
  if(lvl < G.level) return 1;
  FILE *f = G.out ? G.out : stderr;

  if(G.json){
    fputc('{', f);
    /* time */
    if(G.tfmt == TF_ISO){
      char ts[32]; fmt_iso8601_utc(ts, sizeof ts);
      fprintf(f, "\"ts\":\"%s\"", ts);
    }else{
      fprintf(f, "\"ts\":%llu", (unsigned long long)now_epoch_ms());
    }
    /* level */
    fprintf(f, ",\"level\":\"%s\"", level_name(lvl));
    /* prefix if any */
    if(G.prefix[0]){
      fputs(",\"prefix\":\"", f); json_escape(f, G.prefix); fputc('"', f);
    }
    /* message */
    fputs(",\"msg\":\"", f);
    /* msg may contain NUL? VM contract dit NUL-free → OK */
    json_escape(f, msg);
    fputc('"', f);

    /* fields */
    if(has_fields){
      struct kv_ctx c = { f, 1 };
      fputs(",\"fields\":{", f);
      if(vl_table_foreach_kv_string(L, fidx, fields_json_cb, &c) != 0){
        /* ignore partial errors, best-effort */
      }
      fputc('}', f);
    }
    fputc('}', f);
    fputc('\n', f);
  }else{
    /* text */
    char ts[32];
    if(G.tfmt == TF_ISO){ fmt_iso8601_utc(ts, sizeof ts); }
    else { snprintf(ts, sizeof ts, "%llu", (unsigned long long)now_epoch_ms()); }

    const char *col = level_color(lvl);
    const char *rst = color_reset();
    if(lvl>=L_WARN && G.color && G.is_tty) fprintf(f, "%s", col);

    if(G.prefix[0]) fprintf(f, "%s [%s] %s: %.*s", ts, level_name(lvl), G.prefix, (int)mlen, msg);
    else            fprintf(f, "%s [%s] %.*s",   ts, level_name(lvl), (int)mlen, msg);

    if(has_fields){
      struct kv_ctx c = { f, 1 };
      vl_table_foreach_kv_string(L, fidx, fields_text_cb, &c);
    }
    if(lvl>=L_warn && G.color && G.is_tty){ /* guard typo fix: use numeric compare */
      /* replaced by below; keep no-op */
    }
    if(lvl>=L_WARN && G.color && G.is_tty) fputs(rst, f);
    fputc('\n', f);
  }

  if(fflush(f) != 0) return 0;
  return 1;
}

/* ============================== Helpers VM ============================== */

static int str_to_level(const char *s, size_t n){
  if(n==5 && strncasecmp(s,"trace",5)==0) return L_TRACE;
  if(n==5 && strncasecmp(s,"debug",5)==0) return L_DEBUG;
  if(n==4 && strncasecmp(s,"info",4)==0)  return L_INFO;
  if(n==4 && strncasecmp(s,"warn",4)==0)  return L_WARN;
  if(n==5 && strncasecmp(s,"error",5)==0) return L_ERROR;
  if(n==5 && strncasecmp(s,"fatal",5)==0) return L_FATAL;
  if(n==3 && strncasecmp(s,"off",3)==0)   return L_OFF;
  return -1;
}

/* ================================ API =================================== */

static int L_set_level(VLState *L){
  size_t n=0;
  int lvl = (int)vl_opt_integer(L, 1, -999);
  if(lvl == -999){
    const char *s = vl_check_string(L, 1, &n);
    int t = str_to_level(s,n);
    if(t<0){ vl_push_nil(L); vl_push_string(L,"EINVAL"); return 2; }
    G.level = t;
  }else{
    if(lvl<0 || lvl>99){ vl_push_nil(L); vl_push_string(L,"EINVAL"); return 2; }
    G.level = lvl;
  }
  vl_push_boolean(L,1); return 1;
}

static int L_get_level(VLState *L){ vl_push_integer(L, (int64_t)G.level); return 1; }

static int L_set_output(VLState *L){
  size_t n=0; const char *dst = vl_check_string(L,1,&n);
  log_init_once();
  if(n==6 && strncmp(dst,"stderr",6)==0){
    if(G.out && G.out!=stderr && G.out!=stdout) fclose(G.out);
    G.out = stderr; G.out_path[0]=0; G.is_tty = isatty(fileno(G.out)) ? 1:0;
  }else if(n==6 && strncmp(dst,"stdout",6)==0){
    if(G.out && G.out!=stderr && G.out!=stdout) fclose(G.out);
    G.out = stdout; G.out_path[0]=0; G.is_tty = isatty(fileno(G.out)) ? 1:0;
  }else{
    FILE *nf = fopen(dst, "ab");
    if(!nf){ vl_push_nil(L); vl_push_string(L,"EIO"); return 2; }
    if(G.out && G.out!=stderr && G.out!=stdout) fclose(G.out);
    G.out = nf;
    size_t m = (n>=sizeof(G.out_path))? sizeof(G.out_path)-1 : n;
    memcpy(G.out_path, dst, m); G.out_path[m]=0;
    G.is_tty = 0;
  }
  vl_push_boolean(L,1); return 1;
}

static int L_rotate(VLState *L){
  (void)L;
  if(!G.out_path[0]){ vl_push_nil(L); vl_push_string(L,"EINVAL"); return 2; }
  FILE *nf = fopen(G.out_path, "ab");
  if(!nf){ vl_push_nil(L); vl_push_string(L,"EIO"); return 2; }
  if(G.out && G.out!=stderr && G.out!=stdout) fclose(G.out);
  G.out = nf; G.is_tty=0;
  vl_push_boolean(L,1); return 1;
}

static int L_set_color(VLState *L){ G.color = vl_opt_boolean(L,1,1); vl_push_boolean(L,1); return 1; }
static int L_set_json(VLState *L){ G.json  = vl_opt_boolean(L,1,0); vl_push_boolean(L,1); return 1; }

static int L_set_time(VLState *L){
  size_t n=0; const char *fmt = vl_check_string(L,1,&n);
  if((n==7 && strncmp(fmt,"iso8601",7)==0) || (n==8 && strncmp(fmt,"ISO8601",8)==0)){ G.tfmt=TF_ISO; }
  else if(n==8 && strncmp(fmt,"epoch_ms",8)==0){ G.tfmt=TF_EPOCH_MS; }
  else { vl_push_nil(L); vl_push_string(L,"EINVAL"); return 2; }
  vl_push_boolean(L,1); return 1;
}

static int L_set_prefix(VLState *L){
  size_t n=0; const char *p = vl_opt_string(L,1,"",&n);
  size_t m = (n>=sizeof(G.prefix))? sizeof(G.prefix)-1 : n;
  memcpy(G.prefix,p,m); G.prefix[m]=0;
  vl_push_boolean(L,1); return 1;
}

static int L_flush(VLState *L){
  log_init_once();
  if(fflush(G.out?G.out:stderr)!=0){ vl_push_nil(L); vl_push_string(L,"EIO"); return 2; }
  vl_push_boolean(L,1); return 1;
}

/* log.write(level, message[, fields]) */
static int L_write(VLState *L){
  int64_t lvl = vl_opt_integer(L,1,L_INFO);
  size_t mlen=0; const char *msg = vl_check_string(L,2,&mlen);
  int has_fields = 0; int fidx = 3;

  /* champs facultatifs passés comme table string->string */
  /* Si l’API VM n’offre pas de test direct, on tente simplement l’itération; en cas d’erreur → ignore. */
  /* Ici, on détecte la présence par un essai d’itération à vide; on se contente d’un drapeau */
  /* Simplification: on considère que si l’argument 3 est fourni, c’est une table valide. */
  has_fields = 1; /* pas d’inspection stricte pour rester découplé */
  /* On n’a pas vl_istable ici; si fields absent, une itération renverra 0 éléments, sans échec. */

  if(!write_record((int)lvl, msg, mlen, has_fields, L, fidx)){
    vl_push_nil(L); vl_push_string(L,"EIO"); return 2;
  }
  vl_push_boolean(L,1); return 1;
}

static int L_trace(VLState *L){ size_t n=0; const char *m=vl_check_string(L,1,&n); int has=(int)vl_opt_integer(L,3,0); if(!write_record(L_TRACE,m,n,has,L,2)) { vl_push_nil(L); vl_push_string(L,"EIO"); return 2; } vl_push_boolean(L,1); return 1; }
static int L_debug(VLState *L){ size_t n=0; const char *m=vl_check_string(L,1,&n); int has=(int)vl_opt_integer(L,3,0); if(!write_record(L_DEBUG,m,n,has,L,2)) { vl_push_nil(L); vl_push_string(L,"EIO"); return 2; } vl_push_boolean(L,1); return 1; }
static int L_info (VLState *L){ size_t n=0; const char *m=vl_check_string(L,1,&n); int has=(int)vl_opt_integer(L,3,0); if(!write_record(L_INFO ,m,n,has,L,2)) { vl_push_nil(L); vl_push_string(L,"EIO"); return 2; } vl_push_boolean(L,1); return 1; }
static int L_warn (VLState *L){ size_t n=0; const char *m=vl_check_string(L,1,&n); int has=(int)vl_opt_integer(L,3,0); if(!write_record(L_WARN ,m,n,has,L,2)) { vl_push_nil(L); vl_push_string(L,"EIO"); return 2; } vl_push_boolean(L,1); return 1; }
static int L_error(VLState *L){ size_t n=0; const char *m=vl_check_string(L,1,&n); int has=(int)vl_opt_integer(L,3,0); if(!write_record(L_ERROR,m,n,has,L,2)) { vl_push_nil(L); vl_push_string(L,"EIO"); return 2; } vl_push_boolean(L,1); return 1; }
static int L_fatal(VLState *L){ size_t n=0; const char *m=vl_check_string(L,1,&n); int has=(int)vl_opt_integer(L,3,0); if(!write_record(L_FATAL,m,n,has,L,2)) { vl_push_nil(L); vl_push_string(L,"EIO"); return 2; } vl_push_boolean(L,1); return 1; }

/* ================================ Dispatch =============================== */

static const struct vl_Reg funs[] = {
  {"set_level",  L_set_level},
  {"get_level",  L_get_level},
  {"set_output", L_set_output},
  {"rotate",     L_rotate},
  {"set_color",  L_set_color},
  {"set_json",   L_set_json},
  {"set_time",   L_set_time},
  {"set_prefix", L_set_prefix},
  {"flush",      L_flush},

  {"write", L_write},
  {"trace", L_trace},
  {"debug", L_debug},
  {"info",  L_info},
  {"warn",  L_warn},
  {"error", L_error},
  {"fatal", L_fatal},
  {NULL, NULL}
};

int vl_openlib_log(VLState *L){
  log_init_once();
  vl_register_module(L, "log", funs);
  return 1;
}