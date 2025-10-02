/* ============================================================================
   json.c — JSON minimal mais complet (C17, MIT)
   - Parser DOM: null, bool, number (double), string (UTF-8), array, object
   - Stringify compact ou pretty avec indentation configurable
   - API autonome si json.h absent
   - Robustesse: espaces/UTF-8, \uXXXX → UTF-8, contrôle d’erreurs précis
   - Limites: nombres parsés via strtod (IEEE-754), pas d’infinity/NaN
   ============================================================================
*/
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------------
   API publique si json.h absent
---------------------------------------------------------------------------- */
#ifndef VT_JSON_HAVE_HEADER
#ifndef VT_JSON_API
#define VT_JSON_API extern
#endif

typedef enum {
  VTJ_NULL = 0, VTJ_BOOL, VTJ_NUM, VTJ_STR, VTJ_ARR, VTJ_OBJ
} vtj_type;

typedef struct vt_json vt_json;

VT_JSON_API vt_json* vtj_null(void);
VT_JSON_API vt_json* vtj_bool(int b);
VT_JSON_API vt_json* vtj_num(double x);
VT_JSON_API vt_json* vtj_str(const char* s);       /* duplique s (UTF-8) */
VT_JSON_API vt_json* vtj_arr(void);
VT_JSON_API vt_json* vtj_obj(void);

VT_JSON_API int      vtj_arr_push(vt_json* a, vt_json* v);               /* 0/-1 */
VT_JSON_API int      vtj_obj_put(vt_json* o, const char* k, vt_json* v); /* 0/-1 (dup k) */

VT_JSON_API vtj_type vtj_typeof(const vt_json* v);
VT_JSON_API size_t   vtj_len(const vt_json* v);      /* arr/obj */
VT_JSON_API double   vtj_as_num(const vt_json* v, int* ok);
VT_JSON_API int      vtj_as_bool(const vt_json* v, int* ok);
VT_JSON_API const char* vtj_as_str(const vt_json* v, int* ok);

/* lookup object; returns NULL if absent */
VT_JSON_API vt_json* vtj_obj_get(const vt_json* o, const char* key);
VT_JSON_API vt_json* vtj_arr_get(const vt_json* a, size_t idx);

VT_JSON_API void     vtj_free(vt_json* v);

/* Parsing */
typedef struct {
  const char* msg;  /* NULL si OK */
  size_t line, col; /* 1-based */
  size_t byte_off;  /* 0-based */
} vtj_error;

VT_JSON_API vt_json* vtj_parse(const char* json, vtj_error* err);
VT_JSON_API vt_json* vtj_parse_n(const char* json, size_t n, vtj_error* err);
VT_JSON_API vt_json* vtj_parse_file(const char* path, vtj_error* err); /* lit puis parse */

/* Stringify */
typedef struct {
  int pretty;       /* 0=compact */
  int indent;       /* espaces par niveau si pretty */
  int ascii_only;   /* 1 = escape tout non-ASCII avec \uXXXX */
} vtj_write_opts;

VT_JSON_API char* vtj_stringify(const vt_json* v, const vtj_write_opts* opt);
/* Renvoie malloc()+NUL, à free() par l’appelant */

#endif /* VT_JSON_HAVE_HEADER */

/* ----------------------------------------------------------------------------
   Implémentation DOM
---------------------------------------------------------------------------- */
struct kv {
  char* k;
  vt_json* v;
};
typedef struct {
  vt_json** items;
  size_t len, cap;
} vec_v;
typedef struct {
  struct kv* items;
  size_t len, cap;
} vec_kv;

struct vt_json {
  vtj_type t;
  union {
    double num;
    int    b;
    char*  s;
    vec_v  a;
    vec_kv o;
  } u;
};

static void* xmalloc(size_t n){ void* p = malloc(n?n:1); if(!p){fprintf(stderr,"OOM %zu\n",n); abort();} return p; }
static void* xrealloc(void* p,size_t n){ void* q = realloc(p,n?n:1); if(!q){fprintf(stderr,"OOM %zu\n",n); abort();} return q; }
static char* xstrdup(const char* s){ size_t n=strlen(s)+1; char* d=xmalloc(n); memcpy(d,s,n); return d; }

vt_json* vtj_null(void){ vt_json* v=xmalloc(sizeof *v); v->t=VTJ_NULL; return v; }
vt_json* vtj_bool(int b){ vt_json* v=xmalloc(sizeof *v); v->t=VTJ_BOOL; v->u.b=!!b; return v; }
vt_json* vtj_num(double x){ vt_json* v=xmalloc(sizeof *v); v->t=VTJ_NUM; v->u.num=x; return v; }
vt_json* vtj_str(const char* s){ vt_json* v=xmalloc(sizeof *v); v->t=VTJ_STR; v->u.s=xstrdup(s?s:""); return v; }
vt_json* vtj_arr(void){ vt_json* v=xmalloc(sizeof *v); v->t=VTJ_ARR; v->u.a.items=NULL; v->u.a.len=v->u.a.cap=0; return v; }
vt_json* vtj_obj(void){ vt_json* v=xmalloc(sizeof *v); v->t=VTJ_OBJ; v->u.o.items=NULL; v->u.o.len=v->u.o.cap=0; return v; }

int vtj_arr_push(vt_json* a, vt_json* v){
  if(!a || a->t!=VTJ_ARR) return -1;
  if(a->u.a.len==a->u.a.cap){ size_t nc=a->u.a.cap? a->u.a.cap*2:4; a->u.a.items=xrealloc(a->u.a.items,nc*sizeof(vt_json*)); a->u.a.cap=nc; }
  a->u.a.items[a->u.a.len++]=v; return 0;
}
int vtj_obj_put(vt_json* o, const char* k, vt_json* v){
  if(!o || o->t!=VTJ_OBJ) return -1;
  /* update if exists */
  for(size_t i=0;i<o->u.o.len;i++){
    if(strcmp(o->u.o.items[i].k,k)==0){ vtj_free(o->u.o.items[i].v); o->u.o.items[i].v=v; return 0; }
  }
  if(o->u.o.len==o->u.o.cap){ size_t nc=o->u.o.cap? o->u.o.cap*2:4; o->u.o.items=xrealloc(o->u.o.items,nc*sizeof(struct kv)); o->u.o.cap=nc; }
  o->u.o.items[o->u.o.len].k=xstrdup(k); o->u.o.items[o->u.o.len].v=v; o->u.o.len++;
  return 0;
}

vtj_type vtj_typeof(const vt_json* v){ return v?v->t:VTJ_NULL; }
size_t   vtj_len(const vt_json* v){
  if(!v) return 0;
  if(v->t==VTJ_ARR) return v->u.a.len;
  if(v->t==VTJ_OBJ) return v->u.o.len;
  return 0;
}
double vtj_as_num(const vt_json* v, int* ok){ if(ok) *ok= (v && v->t==VTJ_NUM); return (v&&v->t==VTJ_NUM)? v->u.num:0.0; }
int    vtj_as_bool(const vt_json* v, int* ok){ if(ok) *ok= (v && v->t==VTJ_BOOL); return (v&&v->t==VTJ_BOOL)? v->u.b:0; }
const char* vtj_as_str(const vt_json* v, int* ok){ if(ok) *ok=(v&&v->t==VTJ_STR); return (v&&v->t==VTJ_STR)? v->u.s:NULL; }

vt_json* vtj_obj_get(const vt_json* o, const char* key){
  if(!o || o->t!=VTJ_OBJ) return NULL;
  for(size_t i=0;i<o->u.o.len;i++) if(strcmp(o->u.o.items[i].k,key)==0) return o->u.o.items[i].v;
  return NULL;
}
vt_json* vtj_arr_get(const vt_json* a, size_t idx){
  if(!a || a->t!=VTJ_ARR) return NULL;
  return (idx<a->u.a.len)? a->u.a.items[idx]:NULL;
}

void vtj_free(vt_json* v){
  if(!v) return;
  switch(v->t){
    case VTJ_NULL: case VTJ_NUM: case VTJ_BOOL: break;
    case VTJ_STR: free(v->u.s); break;
    case VTJ_ARR:
      for(size_t i=0;i<v->u.a.len;i++) vtj_free(v->u.a.items[i]);
      free(v->u.a.items); break;
    case VTJ_OBJ:
      for(size_t i=0;i<v->u.o.len;i++){ free(v->u.o.items[i].k); vtj_free(v->u.o.items[i].v); }
      free(v->u.o.items); break;
  }
  free(v);
}

/* ----------------------------------------------------------------------------
   UTF-8 + \uXXXX helpers
---------------------------------------------------------------------------- */
static int hexv(int c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static size_t utf8_encode(uint32_t cp, char out[4]){
  if(cp<=0x7F){ out[0]=(char)cp; return 1; }
  if(cp<=0x7FF){ out[0]=0xC0|(cp>>6); out[1]=0x80|(cp&0x3F); return 2; }
  if(cp<=0xFFFF){ out[0]=0xE0|(cp>>12); out[1]=0x80|((cp>>6)&0x3F); out[2]=0x80|(cp&0x3F); return 3; }
  out[0]=0xF0|(cp>>18); out[1]=0x80|((cp>>12)&0x3F); out[2]=0x80|((cp>>6)&0x3F); out[3]=0x80|(cp&0x3F); return 4;
}

/* ----------------------------------------------------------------------------
   Lexer/Parser
---------------------------------------------------------------------------- */
typedef struct {
  const char* s;
  size_t n, i, line, col;
  vtj_error* err;
} P;

static void set_err(P* p, const char* msg){
  if(!p->err) return;
  p->err->msg = msg; p->err->line = p->line; p->err->col = p->col; p->err->byte_off = p->i;
}
static int at_end(P* p){ return p->i >= p->n; }
static int peek(P* p){ return at_end(p)? EOF : (unsigned char)p->s[p->i]; }
static int adv(P* p){
  if(at_end(p)) return EOF;
  int c = (unsigned char)p->s[p->i++];
  if(c=='\n'){ p->line++; p->col=1; } else p->col++;
  return c;
}
static void skip_ws(P* p){
  for(;;){
    int c = peek(p);
    if(c=='/' && p->i+1<p->n && p->s[p->i+1]=='/'){ /* ligne */
      while(!at_end(p) && adv(p)!='\n');
      continue;
    }
    if(c=='/' && p->i+1<p->n && p->s[p->i+1]=='*'){ /* bloc */
      p->i+=2; p->col+=2;
      while(p->i+1<p->n && !(p->s[p->i]=='*' && p->s[p->i+1]=='/')) adv(p);
      if(p->i+1<p->n){ p->i+=2; p->col+=2; }
      continue;
    }
    if(c==' '||c=='\t'||c=='\n'||c=='\r') { adv(p); continue; }
    break;
  }
}

static vt_json* parse_val(P* p);

static vt_json* parse_string(P* p){
  if(adv(p)!='"'){ set_err(p,"expected '\"'"); return NULL; }
  size_t cap=32, len=0; char* buf=xmalloc(cap);
  while(!at_end(p)){
    int c=adv(p);
    if(c=='"'){ buf[len]=0; vt_json* v=vtj_str(buf); free(buf); return v; }
    if(c=='\\'){
      int e=adv(p);
      switch(e){
        case '"': case '\\': case '/': if(len+1>=cap){cap*=2; buf=xrealloc(buf,cap);} buf[len++]=(char)e; break;
        case 'b': if(len+1>=cap){cap*=2; buf=xrealloc(buf,cap);} buf[len++]='\b'; break;
        case 'f': if(len+1>=cap){cap*=2; buf=xrealloc(buf,cap);} buf[len++]='\f'; break;
        case 'n': if(len+1>=cap){cap*=2; buf=xrealloc(buf,cap);} buf[len++]='\n'; break;
        case 'r': if(len+1>=cap){cap*=2; buf=xrealloc(buf,cap);} buf[len++]='\r'; break;
        case 't': if(len+1>=cap){cap*=2; buf=xrealloc(buf,cap);} buf[len++]='\t'; break;
        case 'u': {
          int h1=hexv(adv(p)), h2=hexv(adv(p)), h3=hexv(adv(p)), h4=hexv(adv(p));
          if(h1<0||h2<0||h3<0||h4<0){ set_err(p,"bad \\uXXXX"); free(buf); return NULL; }
          uint32_t cp = (uint32_t)((h1<<12)|(h2<<8)|(h3<<4)|h4);
          /* surrogate pair */
          if(cp>=0xD800 && cp<=0xDBFF){
            if(adv(p)!='\\' || adv(p)!='u'){ set_err(p,"bad surrogate"); free(buf); return NULL; }
            int g1=hexv(adv(p)), g2=hexv(adv(p)), g3=hexv(adv(p)), g4=hexv(adv(p));
            if(g1<0||g2<0||g3<0||g4<0){ set_err(p,"bad \\uXXXX"); free(buf); return NULL; }
            uint32_t low = (uint32_t)((g1<<12)|(g2<<8)|(g3<<4)|g4);
            if(low<0xDC00 || low>0xDFFF){ set_err(p,"bad low surrogate"); free(buf); return NULL; }
            cp = 0x10000 + (((cp - 0xD800)<<10) | (low - 0xDC00));
          }
          char tmp[4]; size_t k=utf8_encode(cp,tmp);
          if(len+k>=cap){ while(len+k>=cap) cap*=2; buf=xrealloc(buf,cap); }
          memcpy(buf+len,tmp,k); len+=k;
        } break;
        default: set_err(p,"bad escape"); free(buf); return NULL;
      }
      continue;
    }
    if((unsigned)c < 0x20){ set_err(p,"control char in string"); free(buf); return NULL; }
    if(len+1>=cap){ cap*=2; buf=xrealloc(buf,cap); }
    buf[len++]=(char)c;
  }
  set_err(p,"unterminated string"); free(buf); return NULL;
}

static vt_json* parse_number(P* p){
  size_t start=p->i;
  int c=peek(p);
  if(c=='-' ){ adv(p); c=peek(p); }
  if(c=='0'){ adv(p); }
  else if(isdigit(c)){ do{ adv(p); c=peek(p);}while(isdigit(c)); }
  else { set_err(p,"bad number"); return NULL; }
  if(peek(p)=='.'){ adv(p); if(!isdigit(peek(p))){ set_err(p,"bad number"); return NULL; } do{ adv(p);}while(isdigit(peek(p))); }
  c=peek(p);
  if(c=='e'||c=='E'){ adv(p); c=peek(p); if(c=='+'||c=='-') adv(p); if(!isdigit(peek(p))){ set_err(p,"bad exponent"); return NULL; } do{ adv(p);}while(isdigit(peek(p))); }
  size_t end=p->i;
  char tmp[64];
  size_t n=end-start;
  char* num = (n<sizeof tmp)? tmp : (char*)xmalloc(n+1);
  memcpy(num, p->s+start, n); num[n]=0;
  char* ep=NULL; errno=0;
  double x=strtod(num,&ep);
  if((n>=sizeof tmp)) free(num);
  if(errno==ERANGE){ set_err(p,"number out of range"); return NULL; }
  return vtj_num(x);
}

static vt_json* parse_array(P* p){
  if(adv(p)!='['){ set_err(p,"expected '['"); return NULL; }
  vt_json* a = vtj_arr();
  skip_ws(p);
  if(peek(p)==']'){ adv(p); return a; }
  for(;;){
    skip_ws(p);
    vt_json* v=parse_val(p);
    if(!v){ vtj_free(a); return NULL; }
    vtj_arr_push(a,v);
    skip_ws(p);
    int c=adv(p);
    if(c==']') return a;
    if(c!=','){ set_err(p,"expected ',' or ']'"); vtj_free(a); return NULL; }
    skip_ws(p);
  }
}

static vt_json* parse_object(P* p){
  if(adv(p)!='{'){ set_err(p,"expected '{'"); return NULL; }
  vt_json* o = vtj_obj();
  skip_ws(p);
  if(peek(p)=='}'){ adv(p); return o; }
  for(;;){
    skip_ws(p);
    if(peek(p)!='"'){ set_err(p,"expected object key"); vtj_free(o); return NULL; }
    vt_json* ks = parse_string(p);
    if(!ks){ vtj_free(o); return NULL; }
    skip_ws(p);
    if(adv(p) != ':'){ set_err(p,"expected ':'"); vtj_free(ks); vtj_free(o); return NULL; }
    skip_ws(p);
    vt_json* vv = parse_val(p);
    if(!vv){ vtj_free(ks); vtj_free(o); return NULL; }
    vtj_obj_put(o, ks->u.s, vv);
    vtj_free(ks); /* key duplicated by obj_put */
    skip_ws(p);
    int c=adv(p);
    if(c=='}') return o;
    if(c!=','){ set_err(p,"expected ',' or '}'"); vtj_free(o); return NULL; }
    skip_ws(p);
  }
}

static vt_json* parse_lit(P* p, const char* lit, vt_json* v){
  for(size_t i=0; lit[i]; i++){ if(adv(p)!=lit[i]){ set_err(p,"bad literal"); vtj_free(v); return NULL; } }
  return v;
}

static vt_json* parse_val(P* p){
  skip_ws(p);
  int c = peek(p);
  if(c=='"') return parse_string(p);
  if(c=='{' ) return parse_object(p);
  if(c=='[' ) return parse_array(p);
  if(c=='t' ) return parse_lit(p,"true", vtj_bool(1));
  if(c=='f' ) return parse_lit(p,"false",vtj_bool(0));
  if(c=='n' ) return parse_lit(p,"null", vtj_null());
  if(c=='-' || isdigit(c)) return parse_number(p);
  set_err(p,"unexpected token");
  return NULL;
}

vt_json* vtj_parse_n(const char* s, size_t n, vtj_error* err){
  P p={.s=s,.n=n,.i=0,.line=1,.col=1,.err=err};
  if(err){ err->msg=NULL; err->line=1; err->col=1; err->byte_off=0; }
  skip_ws(&p);
  vt_json* v = parse_val(&p);
  if(!v) return NULL;
  skip_ws(&p);
  if(!at_end(&p)){ set_err(&p,"extra data after JSON value"); vtj_free(v); return NULL; }
  return v;
}
vt_json* vtj_parse(const char* s, vtj_error* err){
  return vtj_parse_n(s, s?strlen(s):0, err);
}

/* tiny read-all */
static int read_all_file(const char* path, char** out, size_t* n){
  *out=NULL; if(n) *n=0;
  FILE* f = fopen(path,"rb"); if(!f) return -1;
  if(fseek(f,0,SEEK_END)!=0){ fclose(f); return -1; }
  long sz = ftell(f); if(sz<0){ fclose(f); return -1; }
  rewind(f);
  char* buf = (char*)xmalloc((size_t)sz+1);
  size_t rd = fread(buf,1,(size_t)sz,f); fclose(f); buf[rd]=0;
  *out=buf; if(n) *n=rd; return 0;
}
vt_json* vtj_parse_file(const char* path, vtj_error* err){
  char* buf=NULL; size_t n=0;
  if(read_all_file(path,&buf,&n)!=0){ if(err){ err->msg="io error"; err->line=0; err->col=0; err->byte_off=0; } return NULL; }
  vt_json* v = vtj_parse_n(buf,n,err);
  free(buf);
  return v;
}

/* ----------------------------------------------------------------------------
   Stringify
---------------------------------------------------------------------------- */
static void sb_putc(char** buf, size_t* len, size_t* cap, char c){
  if(*len+1>=*cap){ *cap = (*cap? *cap*2 : 64); *buf = xrealloc(*buf, *cap); }
  (*buf)[(*len)++] = c; (*buf)[*len]=0;
}
static void sb_puts(char** buf, size_t* len, size_t* cap, const char* s){
  while(*s) sb_putc(buf,len,cap,*s++);
}
static void emit_indent(char** b, size_t* l, size_t* c, int n){ for(int i=0;i<n;i++) sb_putc(b,l,c,' '); }

static void emit_esc_string(char** b, size_t* l, size_t* c, const char* s, int ascii_only){
  sb_putc(b,l,c,'"');
  for(const unsigned char* p=(const unsigned char*)s; *p; ++p){
    unsigned ch = *p;
    switch(ch){
      case '"':  sb_puts(b,l,c,"\\\""); break;
      case '\\': sb_puts(b,l,c,"\\\\"); break;
      case '\b': sb_puts(b,l,c,"\\b");  break;
      case '\f': sb_puts(b,l,c,"\\f");  break;
      case '\n': sb_puts(b,l,c,"\\n");  break;
      case '\r': sb_puts(b,l,c,"\\r");  break;
      case '\t': sb_puts(b,l,c,"\\t");  break;
      default:
        if(ch < 0x20 || (ascii_only && ch>=0x80)){
          char tmp[7]; snprintf(tmp,sizeof tmp,"\\u%04X", ch);
          sb_puts(b,l,c,tmp);
        } else {
          sb_putc(b,l,c,(char)ch);
        }
    }
  }
  sb_putc(b,l,c,'"');
}

static void emit_json(char** b,size_t* l,size_t* cap,const vt_json* v,const vtj_write_opts* o,int depth){
  switch(v->t){
    case VTJ_NULL: sb_puts(b,l,cap,"null"); break;
    case VTJ_BOOL: sb_puts(b,l,cap, v->u.b? "true":"false"); break;
    case VTJ_NUM: {
      char tmp[64];
      /* %.17g garde précision double */
      int n = snprintf(tmp,sizeof tmp,"%.17g", v->u.num);
      /* S’assure qu’un entier s’imprime comme 1 et pas 1.00000e+00 si possible */
      sb_puts(b,l,cap,tmp);
    } break;
    case VTJ_STR: emit_esc_string(b,l,cap,v->u.s, o && o->ascii_only); break;
    case VTJ_ARR: {
      sb_putc(b,l,cap,'[');
      if(v->u.a.len){
        if(o && o->pretty){ sb_putc(b,l,cap,'\n'); }
        for(size_t i=0;i<v->u.a.len;i++){
          if(o && o->pretty){ emit_indent(b,l,cap, (depth+1)*(o->indent?o->indent:2)); }
          emit_json(b,l,cap,v->u.a.items[i],o,depth+1);
          if(i+1<v->u.a.len) sb_putc(b,l,cap,',');
          if(o && o->pretty){ sb_putc(b,l,cap,'\n'); }
        }
        if(o && o->pretty){ emit_indent(b,l,cap, depth*(o->indent?o->indent:2)); }
      }
      sb_putc(b,l,cap,']');
    } break;
    case VTJ_OBJ: {
      sb_putc(b,l,cap,'{');
      if(v->u.o.len){
        if(o && o->pretty){ sb_putc(b,l,cap,'\n'); }
        for(size_t i=0;i<v->u.o.len;i++){
          if(o && o->pretty){ emit_indent(b,l,cap, (depth+1)*(o->indent?o->indent:2)); }
          emit_esc_string(b,l,cap,v->u.o.items[i].k, o && o->ascii_only);
          sb_putc(b,l,cap,':');
          if(o && o->pretty) sb_putc(b,l,cap,' ');
          emit_json(b,l,cap,v->u.o.items[i].v,o,depth+1);
          if(i+1<v->u.o.len) sb_putc(b,l,cap,',');
          if(o && o->pretty){ sb_putc(b,l,cap,'\n'); }
        }
        if(o && o->pretty){ emit_indent(b,l,cap, depth*(o->indent?o->indent:2)); }
      }
      sb_putc(b,l,cap,'}');
    } break;
  }
}

char* vtj_stringify(const vt_json* v, const vtj_write_opts* opt){
  if(!v){ char* z=xmalloc(5); memcpy(z,"null",5); return z; }
  char* buf=NULL; size_t len=0,cap=0;
  emit_json(&buf,&len,&cap,v,opt,0);
  sb_putc(&buf,&len,&cap,'\0'); len--; /* ensure NUL */
  return buf;
}

/* ----------------------------------------------------------------------------
   Tests rapides
   cc -std=c17 -DVT_JSON_TEST json.c
---------------------------------------------------------------------------- */
#ifdef VT_JSON_TEST
int main(void){
  const char* s = "{ \"a\":1, \"b\":[true, false, null, \"é\\u20AC\"], /*comment*/ \"c\": {\"x\":2} }";
  vtj_error er;
  vt_json* v = vtj_parse(s,&er);
  if(!v){ fprintf(stderr,"parse error: %s at %zu:%zu\n", er.msg, er.line, er.col); return 1; }
  vtj_write_opts opt={.pretty=1,.indent=2,.ascii_only=0};
  char* out = vtj_stringify(v,&opt);
  puts(out);
  free(out);
  vtj_free(v);
  return 0;
}
#endif
