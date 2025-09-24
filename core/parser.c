// SPDX-License-Identifier: GPL-3.0-or-later
// core/parser.c — Assembleur VitteLight “plus complet” (C99, portable)
//
// Expose l’API attendue par vitlc :
//   int vl_asm(const char* src, size_t len, uint8_t** out, size_t* out_n,
//              char* err, size_t errcap);
//   int vl_asm_file(const char* path, uint8_t** out, size_t* out_n,
//                   char* err, size_t errcap);
//
// Caractéristiques :
// - Deux passes avec table de labels :  label:  et références JUMP/JZ/JNZ/CALL
// - Commentaires: # … ou ; … jusqu’à fin de ligne
// - Insensibilité à la casse pour les mnémos
// - Espaces/virgules libres. Séparateur de paramètres: espace et/ou virgule
// - Littéraux:
//     * chaînes "..." avec escapes \n \t \" \\
//     * entiers décimaux, 0xHEX, -123
//     * flottants 1.0, -2.5, 3e-2
// - Instructions supportées (opcodes fallback si opcodes.h absent) :
//     HALT
//     PUSHS "str"
//     PUSHI <int32>
//     PUSHF <f64>
//     POP
//     DUP
//     ADD SUB MUL DIV MOD
//     CMP  (compare a,b -> push int: -1,0,1)
//     JUMP <label>
//     JZ   <label>   ; saute si sommet==0
//     JNZ  <label>   ; saute si sommet!=0
//     CALLN <name>, <argc>
//     RET
//
// Encodage binaire émis (simple, plat) :
//   [u8 OPC] [args…]  ; opérandes en little-endian
//   String: [u8 tag=1][u32 len][bytes]
//   i32    : [u8 tag=2][u32 val]
//   f64    : [u8 tag=3][u64 bits]
//   Label  : résolu en i32 offset relatif (à partir de PC suivant l’OP)
//
// Si ton undump/vm attend un container VLBC différent, ajuste côté undump.
// Ce fichier est autonome et ne dépend que de la libc.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

static int vl_ascii_isalpha(int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static int vl_ascii_isdigit(int c) { return c >= '0' && c <= '9'; }
static int vl_ascii_isalnum(int c) {
  return vl_ascii_isalpha(c) || vl_ascii_isdigit(c);
}
static int vl_ascii_isxdigit(int c) {
  return vl_ascii_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int vl_ascii_isspace(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}
static int vl_ascii_toupper(int c) {
  return (c >= 'a' && c <= 'z') ? (c - ('a' - 'A')) : c;
}

/* ───────── Opcodes (fallback si opcodes.h indisponible) ───────── */
#if defined(__has_include)
#  if __has_include("opcodes.h")
#    include "opcodes.h"
#  elif __has_include("core/opcodes.h")
#    include "core/opcodes.h"
#  endif
#endif

#ifndef OP_HALT
#define OP_HALT  0x00u
#endif
#ifndef OP_PUSHS
#define OP_PUSHS 0x10u
#endif
#ifndef OP_PUSHI
#define OP_PUSHI 0x11u
#endif
#ifndef OP_PUSHF
#define OP_PUSHF 0x12u
#endif
#ifndef OP_POP
#define OP_POP   0x13u
#endif
#ifndef OP_DUP
#define OP_DUP   0x14u
#endif
#ifndef OP_ADD
#define OP_ADD   0x20u
#endif
#ifndef OP_SUB
#define OP_SUB   0x21u
#endif
#ifndef OP_MUL
#define OP_MUL   0x22u
#endif
#ifndef OP_DIV
#define OP_DIV   0x23u
#endif
#ifndef OP_MOD
#define OP_MOD   0x24u
#endif
#ifndef OP_CMP
#define OP_CMP   0x25u
#endif
#ifndef OP_JUMP
#define OP_JUMP  0x30u
#endif
#ifndef OP_JZ
#define OP_JZ    0x31u
#endif
#ifndef OP_JNZ
#define OP_JNZ   0x32u
#endif
#ifndef OP_CALLN
#define OP_CALLN 0x40u
#endif
#ifndef OP_RET
#define OP_RET   0x41u
#endif

/* ───────── API ───────── */
int vl_asm(const char* src, size_t len,
           uint8_t** out_bytes, size_t* out_size,
           char* err, size_t errcap);

int vl_asm_file(const char* path,
                uint8_t** out_bytes, size_t* out_size,
                char* err, size_t errcap);

/* ───────── Buffer dynamique ───────── */
typedef struct { uint8_t* d; size_t n, cap; } ABuf;

static int ab_reserve(ABuf* b, size_t add){
  if (b->n + add <= b->cap) return 1;
  size_t nc = b->cap ? b->cap : 128;
  while (nc < b->n + add) nc <<= 1;
  uint8_t* nd=(uint8_t*)realloc(b->d,nc);
  if(!nd) return 0;
  b->d=nd; b->cap=nc; return 1;
}
static int ab_put8(ABuf* b, uint8_t v){ if(!ab_reserve(b,1))return 0; b->d[b->n++]=v; return 1; }
static int ab_putn(ABuf* b, const void* p, size_t n){
  if(!ab_reserve(b,n)) return 0; memcpy(b->d+b->n,p,n); b->n+=n; return 1;
}
static int ab_put_u32le(ABuf* b, uint32_t v){
  uint8_t x[4]={ (uint8_t)(v), (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
  return ab_putn(b,x,4);
}
static int ab_put_u64le(ABuf* b, uint64_t v){
  uint8_t x[8]; for(int i=0;i<8;i++) x[i]=(uint8_t)(v>>(8*i)); return ab_putn(b,x,8);
}

/* ───────── Err ───────── */
static void set_errf(char* err, size_t cap, const char* msg, size_t line){
  if(!err||cap==0) return;
  if(line){
    char buf[256];
    snprintf(buf,sizeof buf,"L%zu: %s", line, msg);
    strncpy(err,buf,cap-1); err[cap-1]=0;
  } else {
    strncpy(err,msg,cap-1); err[cap-1]=0;
  }
}

/* ───────── Lex helpers ───────── */
typedef struct { const char* s; const char* e; size_t line; } Src;

static void skip_ws_line(Src* S, const char* line_end){
  while (S->s<line_end && (*S->s==' '||*S->s=='\t'||*S->s=='\r')) S->s++;
}
static const char* find_eol(const char* p, const char* e){
  while (p<e && *p!='\n') p++; return p;
}
static void next_line(Src* S){
  const char* e = find_eol(S->s,S->e);
  S->s = (e<S->e ? e+1 : e); S->line++;
}
static int is_ident_start(int c){ return vl_ascii_isalpha(c)||c=='_'; }
static int is_ident_char (int c){ return vl_ascii_isalnum(c)||c=='_'; }

static int ci_eq_kw(const char* p, const char* q, size_t n){
  for(size_t i=0;i<n;i++){
    int a = vl_ascii_toupper((unsigned char)p[i]);
    int b = vl_ascii_toupper((unsigned char)q[i]);
    if(a!=b) return 0;
  }
  return 1;
}

/* ───────── Table labels ───────── */
typedef struct { char* name; int32_t pc; } Label;
typedef struct { char* name; size_t patch_pos; size_t at_line; } Ref;

typedef struct {
  Label* v; size_t n, cap;
} Labels;
typedef struct {
  Ref* v; size_t n, cap;
} Refs;

static int push_label(Labels* L, const char* s, size_t n, int32_t pc){
  if (L->n==L->cap){ size_t nc=L->cap?L->cap*2:16; Label* nv=(Label*)realloc(L->v,nc*sizeof*nv); if(!nv) return 0; L->v=nv; L->cap=nc; }
  L->v[L->n].name=(char*)malloc(n+1); if(!L->v[L->n].name) return 0;
  memcpy(L->v[L->n].name,s,n); L->v[L->n].name[n]=0; L->v[L->n].pc=pc; L->n++; return 1;
}
static int push_ref(Refs* R, const char* s, size_t n, size_t patch_pos, size_t line){
  if (R->n==R->cap){ size_t nc=R->cap?R->cap*2:32; Ref* nv=(Ref*)realloc(R->v,nc*sizeof*nv); if(!nv) return 0; R->v=nv; R->cap=nc; }
  R->v[R->n].name=(char*)malloc(n+1); if(!R->v[R->n].name) return 0;
  memcpy(R->v[R->n].name,s,n); R->v[R->n].name[n]=0; R->v[R->n].patch_pos=patch_pos; R->v[R->n].at_line=line; R->n++; return 1;
}
static int find_label(const Labels* L, const char* name, int32_t* out_pc){
  for(size_t i=0;i<L->n;i++){ if(strcmp(L->v[i].name,name)==0){ *out_pc=L->v[i].pc; return 1; } }
  return 0;
}
static void free_labels(Labels* L){ for(size_t i=0;i<L->n;i++) free(L->v[i].name); free(L->v); }
static void free_refs(Refs* R){ for(size_t i=0;i<R->n;i++) free(R->v[i].name); free(R->v); }

/* ───────── Parsers d’arguments ───────── */
static int parse_qstr(Src* S, const char* eol, ABuf* out, char* err, size_t errcap){
  const char* p=S->s;
  if (p>=eol || *p!='"'){ set_errf(err,errcap,"string attendue", S->line); return 0; }
  p++;
  while (p<eol){
    char c=*p++;
    if (c=='"'){ S->s=p; return 1; }
    if (c=='\\'){
      if (p>=eol){ set_errf(err,errcap,"escape incomplet", S->line); return 0; }
      char esc=*p++;
      switch(esc){
        case 'n': c='\n'; break;
        case 'r': c='\r'; break;
        case 't': c='\t'; break;
        case '\\': c='\\'; break;
        case '"': c='"'; break;
        default: c=esc; break;
      }
    }
    if(!ab_put8(out,(uint8_t)c)){ set_errf(err,errcap,"OOM", S->line); return 0; }
  }
  set_errf(err,errcap,"string non terminée", S->line); return 0;
}

static int parse_int32(Src* S, const char* eol, int32_t* out){
  const char* p=S->s; int neg=0;
  if (p<eol && (*p=='+'||*p=='-')){ neg=(*p=='-'); p++; }
  int base=10;
  if (p+2<=eol && p[0]=='0' && (p[1]=='x'||p[1]=='X')){ base=16; p+=2; }
  if (p>=eol || !vl_ascii_isxdigit((unsigned char)*p)) return 0;
  long long v=0;
  while (p<eol && (base==10? vl_ascii_isdigit((unsigned char)*p)
                            : vl_ascii_isxdigit((unsigned char)*p))){
    int d;
    if (vl_ascii_isdigit((unsigned char)*p)) d = *p - '0';
    else d = 10 + (vl_ascii_toupper((unsigned char)*p) - 'A');
    v = (base==10)? (v*10 + d) : (v*16 + d);
    p++;
  }
  if (neg) v = -v;
  *out = (int32_t)v; S->s=p; return 1;
}

static int parse_float64(Src* S, const char* eol, double* out){
  const char* p=S->s; char* endptr=NULL;
  /* copions jusqu'à eol dans un buffer temp pour strtod */
  char tmp[256]; size_t n=(size_t)(eol - p); if(n>=sizeof tmp) n=sizeof tmp - 1;
  memcpy(tmp,p,n); tmp[n]=0;
  double v = strtod(tmp,&endptr);
  if (endptr==tmp) return 0;
  /* avancer S->s du nombre de chars consommés */
  size_t adv = (size_t)(endptr - tmp);
  S->s += adv; *out = v; return 1;
}

static void skip_separators(Src* S, const char* eol){
  /* espace et/ou virgule */
  while (S->s<eol){
    if (*S->s==' '||*S->s=='\t'||*S->s=='\r'||*S->s==',') S->s++;
    else break;
  }
}

/* ───────── Encodeurs d’arguments ───────── */
static int emit_str_arg(ABuf* code, const ABuf* sbuf){
  if(!ab_put8(code,1u)) return 0;
  if(!ab_put_u32le(code,(uint32_t)sbuf->n)) return 0;
  return ab_putn(code,sbuf->d,sbuf->n);
}
static int emit_i32_arg(ABuf* code, int32_t v){
  if(!ab_put8(code,2u)) return 0;
  return ab_put_u32le(code,(uint32_t)v);
}
static int emit_f64_arg(ABuf* code, double d){
  union { double d; uint64_t u; } u; u.d=d;
  if(!ab_put8(code,3u)) return 0;
  return ab_put_u64le(code,u.u);
}

/* ───────── Première passe: génération code + labels/refs ───────── */
static int assemble_pass(Src* S, ABuf* code, Labels* L, Refs* R, char* err, size_t errcap){
  const char* e = S->e;
  while (S->s < e){
    const char* line_start = S->s;
    const char* eol = find_eol(S->s, e);

    /* retirer commentaires */
    const char* cmt = line_start;
    while (cmt<eol && *cmt!='#' && *cmt!=';') cmt++;
    eol = cmt;

    /* trim */
    S->s = line_start;
    skip_ws_line(S,eol);
    if (S->s>=eol){ next_line(S); continue; }

    /* label ?  ident: */
    if (is_ident_start((unsigned char)*S->s)){
      const char* p=S->s; p++;
      while (p<eol && is_ident_char((unsigned char)*p)) p++;
      if (p<eol && *p==':'){
        size_t n = (size_t)(p - S->s);
        if (!push_label(L, S->s, n, (int32_t)code->n)){
          set_errf(err,errcap,"OOM label", S->line); return 0;
        }
        S->s = p+1; /* after ':' */
        skip_ws_line(S,eol);
        if (S->s>=eol){ next_line(S); continue; }
      }
    }

    /* lire mnemo */
    const char* m0 = S->s;
    while (S->s<eol && !vl_ascii_isspace((unsigned char)*S->s)) S->s++;
    size_t mlen = (size_t)(S->s - m0);
    skip_ws_line(S,eol);

    /* Dispatch */
#   define MN(KW) (mlen==strlen(KW) && ci_eq_kw(m0, KW, mlen))

    if (MN("HALT")){
      if(!ab_put8(code,(uint8_t)OP_HALT)){ set_errf(err,errcap,"OOM", S->line); return 0; }
    }
    else if (MN("PUSHS")){
      ABuf s={0};
      if (!parse_qstr(S,eol,&s,err,errcap)){ free(s.d); return 0; }
      if(!ab_put8(code,(uint8_t)OP_PUSHS) || !emit_str_arg(code,&s)){
        free(s.d); set_errf(err,errcap,"OOM", S->line); return 0;
      }
      free(s.d);
    }
    else if (MN("PUSHI")){
      int32_t v; if(!parse_int32(S,eol,&v)){ set_errf(err,errcap,"entier attendu", S->line); return 0; }
      if(!ab_put8(code,(uint8_t)OP_PUSHI) || !emit_i32_arg(code,v)){ set_errf(err,errcap,"OOM", S->line); return 0; }
    }
    else if (MN("PUSHF")){
      double d; if(!parse_float64(S,eol,&d)){ set_errf(err,errcap,"float attendu", S->line); return 0; }
      if(!ab_put8(code,(uint8_t)OP_PUSHF) || !emit_f64_arg(code,d)){ set_errf(err,errcap,"OOM", S->line); return 0; }
    }
    else if (MN("POP")){
      if(!ab_put8(code,(uint8_t)OP_POP)){ set_errf(err,errcap,"OOM", S->line); return 0; }
    }
    else if (MN("DUP")){
      if(!ab_put8(code,(uint8_t)OP_DUP)){ set_errf(err,errcap,"OOM", S->line); return 0; }
    }
    else if (MN("ADD")||MN("SUB")||MN("MUL")||MN("DIV")||MN("MOD")||MN("CMP")){
      uint8_t op = MN("ADD")?OP_ADD: MN("SUB")?OP_SUB: MN("MUL")?OP_MUL:
                   MN("DIV")?OP_DIV: MN("MOD")?OP_MOD: OP_CMP;
      if(!ab_put8(code,op)){ set_errf(err,errcap,"OOM", S->line); return 0; }
    }
    else if (MN("JUMP")||MN("JZ")||MN("JNZ")){
      uint8_t op = MN("JUMP")?OP_JUMP: MN("JZ")?OP_JZ: OP_JNZ;
      /* label ident */
      skip_separators(S,eol);
      const char* i0=S->s;
      if (S->s>=eol || !is_ident_start((unsigned char)*S->s)){ set_errf(err,errcap,"label attendu", S->line); return 0; }
      S->s++;
      while (S->s<eol && is_ident_char((unsigned char)*S->s)) S->s++;
      size_t in=(size_t)(S->s-i0);
      /* encode op + placeholder rel32 */
      if(!ab_put8(code,op)){ set_errf(err,errcap,"OOM", S->line); return 0; }
      size_t patch_pos = code->n; /* à remplacer en passe 2 */
      if(!emit_i32_arg(code,0)){ set_errf(err,errcap,"OOM", S->line); return 0; }
      if(!push_ref(R,i0,in,patch_pos,S->line)){ set_errf(err,errcap,"OOM ref", S->line); return 0; }
    }
    else if (MN("CALLN")){
      /* name , argc */
      skip_separators(S,eol);
      const char* i0=S->s;
      if (S->s>=eol || !is_ident_start((unsigned char)*S->s)){ set_errf(err,errcap,"ident attendu", S->line); return 0; }
      S->s++;
      while (S->s<eol && is_ident_char((unsigned char)*S->s)) S->s++;
      size_t in=(size_t)(S->s-i0);
      skip_separators(S,eol);
      if (S->s>=eol || *S->s!=','){ set_errf(err,errcap,"',' attendu", S->line); return 0; }
      S->s++; skip_separators(S,eol);
      int32_t argc; if(!parse_int32(S,eol,&argc)){ set_errf(err,errcap,"argc entier attendu", S->line); return 0; }
      ABuf name={0}; if(!ab_putn(&name,i0,in)){ set_errf(err,errcap,"OOM", S->line); return 0; }
      if(!ab_put8(code,(uint8_t)OP_CALLN) || !emit_str_arg(code,&name) || !emit_i32_arg(code,argc)){
        free(name.d); set_errf(err,errcap,"OOM", S->line); return 0;
      }
      free(name.d);
    }
    else if (MN("RET")){
      if(!ab_put8(code,(uint8_t)OP_RET)){ set_errf(err,errcap,"OOM", S->line); return 0; }
    }
    else {
      char msg[128];
      size_t L = (size_t)(eol - m0); if (L>64) L=64;
      snprintf(msg,sizeof msg,"mnémotechnique inconnue: '%.*s'", (int)L, m0);
      set_errf(err,errcap,msg,S->line); return 0;
    }

    /* fin de ligne */
    next_line(S);
  }
  return 1;
}

/* ───────── Passe 2: résolution des labels vers rel32 ───────── */
static int patch_labels(const Labels* L, const Refs* R, ABuf* code, char* err, size_t errcap){
  for(size_t i=0;i<R->n;i++){
    int32_t target_pc;
    if(!find_label(L,R->v[i].name,&target_pc)){
      char msg[128]; snprintf(msg,sizeof msg,"label non défini: %s", R->v[i].name);
      set_errf(err,errcap,msg,R->v[i].at_line); return 0;
    }
    /* lecture PC après l’opérande tag=2, puis écriture rel32 */
    size_t pos = R->v[i].patch_pos;
    /* format: [tag=2][u32 LE placeholder], patch sur u32 */
    if (pos+1+4 > code->n){ set_errf(err,errcap,"patch hors limites", R->v[i].at_line); return 0; }
    if (code->d[pos]!=2u){ set_errf(err,errcap,"slot non-int32 pour label", R->v[i].at_line); return 0; }
    /* PC courant = adresse de l’instruction suivante */
    size_t after = pos + 1 + 4;
    int32_t rel = (int32_t)target_pc - (int32_t)after;
    code->d[pos+1]=(uint8_t)(rel);
    code->d[pos+2]=(uint8_t)(rel>>8);
    code->d[pos+3]=(uint8_t)(rel>>16);
    code->d[pos+4]=(uint8_t)(rel>>24);
  }
  return 1;
}

/* ───────── API ───────── */

int vl_asm(const char* src, size_t len,
           uint8_t** out_bytes, size_t* out_size,
           char* err, size_t errcap)
{
  if (!src){ set_errf(err,errcap,"src=NULL",0); return 0; }
  ABuf code={0};
  Labels L={0}; Refs R={0};

  Src S={ src, src+len, 1 };
  if (!assemble_pass(&S,&code,&L,&R,err,errcap)) goto fail;

  if (!patch_labels(&L,&R,&code,err,errcap)) goto fail;

  free_labels(&L); free_refs(&R);
  *out_bytes = code.d; if(out_size) *out_size=code.n;
  return 1;

fail:
  free_labels(&L); free_refs(&R);
  free(code.d);
  if(out_bytes) *out_bytes=NULL; if(out_size) *out_size=0;
  return 0;
}

int vl_asm_file(const char* path,
                uint8_t** out_bytes, size_t* out_size,
                char* err, size_t errcap)
{
  if(!path){ set_errf(err,errcap,"path=NULL",0); return 0; }
  FILE* f=fopen(path,"rb");
  if(!f){ set_errf(err,errcap,"ouverture échouée",0); return 0; }
  fseek(f,0,SEEK_END); long n=ftell(f); if(n<0){ fclose(f); set_errf(err,errcap,"ftell échoué",0); return 0; }
  fseek(f,0,SEEK_SET);
  char* buf=(char*)malloc((size_t)n+1); if(!buf){ fclose(f); set_errf(err,errcap,"OOM",0); return 0; }
  size_t rd=fread(buf,1,(size_t)n,f); fclose(f); buf[rd]=0;
  int ok = vl_asm(buf, rd, out_bytes, out_size, err, errcap);
  free(buf);
  return ok;
}

