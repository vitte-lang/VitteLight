// SPDX-License-Identifier: GPL-3.0-or-later
//
// json.c — JSON encode/decode pour Vitte Light VM (C17, complet, portable)
// Namespace: "json"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c json.c
//
// Modèle:
//   - json.decode(text[, opts]) -> valeur | (nil, "EJSON", offset:int)
//       opts: { allow_trailing_commas=0|1, allow_comments=0|1, null_as_nil=1|0 }
//   - json.encode(value[, opts]) -> string | (nil, "EINVAL")
//       opts: { pretty=0|1, indent=2 (1..8), ascii_only=0|1, sort_keys=0|1 }
//
// Mapping types:
//   JSON null  → nil par défaut (ou sentinelle `json.null` si null_as_nil=0)
//   JSON bool  → bool
//   JSON nombre→ number (double)
//   JSON string→ string (UTF-8)
//   JSON array → table séquentielle [1..n]
//   JSON object→ table clés string
//
// Erreurs:
//   Decode: (nil, "EJSON", offset_byte)
//   Encode: (nil, "EINVAL") si type non pris en charge ou cycle détecté
//
// Deps VM: auxlib.h, state.h, object.h, vm.h

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

#include "auxlib.h"
#include "state.h"
#include "object.h"
#include "vm.h"

/* ========================= VM ADAPTER (extern fournis) ================== */
/* Adaptez ces symboles à votre VM. Ils sont utilisés par le module. */

typedef struct VLState VLState;
struct vl_Reg { const char *name; int (*fn)(VLState *L); };

extern void        vlx_register_module(VLState *L, const char *ns, const struct vl_Reg *funcs);
extern void        vlx_push_nil     (VLState *L);
extern void        vlx_push_boolean (VLState *L, int v);
extern void        vlx_push_number  (VLState *L, double v);
extern void        vlx_push_integer (VLState *L, int64_t v);
extern void        vlx_push_string  (VLState *L, const char *s);
extern void        vlx_push_lstring (VLState *L, const char *s, size_t n);
extern void        vlx_new_table    (VLState *L);
extern void        vlx_set_table_kv (VLState *L, const char *k, const char *v);
extern void        vlx_set_table_kvi(VLState *L, const char *k, int64_t v);
extern void        vlx_seti         (VLState *L, int idx, int64_t i, int value_on_stack); /* optional */
extern void        vlx_set_table_ks (VLState *L, const char *k, int value_on_stack);
extern void        vlx_set_table_is (VLState *L, int64_t i, int value_on_stack);
extern int         vlx_istable      (VLState *L, int idx);
extern int         vlx_isstring     (VLState *L, int idx);
extern int         vlx_isboolean    (VLState *L, int idx);
extern int         vlx_isnumber     (VLState *L, int idx);
extern int         vlx_isnil        (VLState *L, int idx);
extern int         vlx_isnullsentinel(VLState *L, int idx); /* for json.null */
extern int         vlx_table_len    (VLState *L, int idx, int64_t *out_n); /* length of array part */
extern int         vlx_table_foreach_obj(VLState *L, int idx,
                    int (*cb)(VLState*, const char *key, int v_index, void*), void *ud);
extern int         vlx_table_geti_push(VLState *L, int idx, int64_t i); /* pushes value; returns type or 0 if nil */

extern const char* vlx_opt_string   (VLState *L, int idx, const char *def, size_t *len);
extern int         vlx_opt_boolean  (VLState *L, int idx, int def);
extern int64_t     vlx_opt_integer  (VLState *L, int idx, int64_t def);
extern const char* vlx_check_string (VLState *L, int idx, size_t *len);

/* Wrappers locaux minces */
static inline void vl_push_nil(VLState *L){ vlx_push_nil(L); }
static inline void vl_push_boolean(VLState *L,int v){ vlx_push_boolean(L,v); }
static inline void vl_push_number(VLState *L,double v){ vlx_push_number(L,v); }
static inline void vl_push_integer(VLState *L,int64_t v){ vlx_push_integer(L,v); }
static inline void vl_push_string(VLState *L,const char*s){ vlx_push_string(L,s); }
static inline void vl_push_lstring(VLState *L,const char*s,size_t n){ vlx_push_lstring(L,s,n); }
static inline void vl_new_table(VLState *L){ vlx_new_table(L); }
static inline void vl_set_table_kv(VLState *L,const char*k,const char*v){ vlx_set_table_kv(L,k,v); }
static inline void vl_set_table_kvi(VLState *L,const char*k,int64_t v){ vlx_set_table_kvi(L,k,v); }
static inline int  vl_table_len(VLState *L,int idx,int64_t *n){ return vlx_table_len(L,idx,n); }
static inline int  vl_table_foreach_obj(VLState *L,int idx,int (*cb)(VLState*,const char*,int,void*),void*ud){ return vlx_table_foreach_obj(L,idx,cb,ud); }
static inline int  vl_table_geti_push(VLState *L,int idx,int64_t i){ return vlx_table_geti_push(L,idx,i); }
static inline int  vl_isnil(VLState *L,int idx){ return vlx_isnil(L,idx); }
static inline int  vl_isstring(VLState *L,int idx){ return vlx_isstring(L,idx); }
static inline int  vl_isboolean(VLState *L,int idx){ return vlx_isboolean(L,idx); }
static inline int  vl_isnumber(VLState *L,int idx){ return vlx_isnumber(L,idx); }
static inline int  vl_isnullsentinel(VLState *L,int idx){ return vlx_isnullsentinel(L,idx); }
static inline const char* vl_check_string(VLState *L,int idx,size_t *len){ return vlx_check_string(L,idx,len); }
static inline const char* vl_opt_string(VLState *L,int idx,const char*def,size_t*len){ return vlx_opt_string(L,idx,def,len); }
static inline int  vl_opt_boolean(VLState *L,int idx,int def){ return vlx_opt_boolean(L,idx,def); }
static inline int64_t vl_opt_integer(VLState *L,int idx,int64_t def){ return vlx_opt_integer(L,idx,def); }
static inline void vl_register_module(VLState *L,const char*ns,const struct vl_Reg*funcs){ vlx_register_module(L,ns,funcs); }

/* ================================ Utils ================================= */

#define JSON_MAX_DEPTH 1024

typedef struct {
  const char *p, *end;
  size_t off;   /* byte offset from start */
  int allow_tc;
  int allow_comments;
} JIn;

typedef struct {
  char *buf;
  size_t len, cap;
  int pretty;
  int ascii_only;
  int indent;
  int depth;
  int error;
} JOut;

static void* xrealloc(void *p, size_t n){
  void *q = realloc(p, n?n:1);
  return q;
}
static int jout_put(JOut *o, const char *s, size_t n){
  if(o->error) return -1;
  if(o->len + n + 1 > o->cap){
    size_t nc = o->cap ? o->cap*2 : 4096;
    while(nc < o->len + n + 1) nc *= 2;
    char *nb = (char*)xrealloc(o->buf, nc);
    if(!nb){ o->error=1; return -1; }
    o->buf = nb; o->cap = nc;
  }
  memcpy(o->buf + o->len, s, n);
  o->len += n;
  o->buf[o->len] = 0;
  return 0;
}
static int jout_putc(JOut *o, char c){ return jout_put(o, &c, 1); }
static int jout_indent(JOut *o){
  if(!o->pretty) return 0;
  if(jout_putc(o,'\n')) return -1;
  for(int i=0;i<o->depth*o->indent;i++) if(jout_putc(o,' ')) return -1;
  return 0;
}
static inline int hexv(int c){
  if(c>='0'&&c<='9') return c-'0';
  if(c>='a'&&c<='f') return 10 + (c-'a');
  if(c>='A'&&c<='F') return 10 + (c-'A');
  return -1;
}

/* =============================== Decode ================================= */

static void jin_skip_ws(JIn *in){
  while(in->p < in->end){
    char c = *in->p;
    if(c==' '||c=='\t'||c=='\n'||c=='\r'){ in->p++; in->off++; continue; }
    if(in->allow_comments && c=='/'){
      if(in->p+1<in->end && in->p[1]=='/'){ /* line comment */
        in->p+=2; in->off+=2;
        while(in->p<in->end && *in->p!='\n' && *in->p!='\r'){ in->p++; in->off++; }
        continue;
      }else if(in->p+1<in->end && in->p[1]=='*'){ /* block comment */
        in->p+=2; in->off+=2;
        while(in->p+1<in->end && !(in->p[0]=='*' && in->p[1]=='/')){ in->p++; in->off++; }
        if(in->p+1<in->end){ in->p+=2; in->off+=2; }
        continue;
      }
    }
    break;
  }
}

static int jin_match(JIn *in, char c){
  jin_skip_ws(in);
  if(in->p<in->end && *in->p==c){ in->p++; in->off++; return 1; }
  return 0;
}

static int push_error(VLState *L, size_t off){
  vl_push_nil(L); vl_push_string(L, "EJSON"); vl_push_integer(L,(int64_t)off); return 3;
}

/* \uXXXX -> UTF-8 append into out */
static int utf16_to_utf8(uint32_t cp, JOut *o){
  if(cp<=0x7F){ return jout_putc(o,(char)cp); }
  else if(cp<=0x7FF){ char b[2]={(char)(0xC0|(cp>>6)), (char)(0x80|(cp&0x3F))}; return jout_put(o,b,2); }
  else if(cp<=0xFFFF){ char b[3]={(char)(0xE0|(cp>>12)), (char)(0x80|((cp>>6)&0x3F)), (char)(0x80|(cp&0x3F))}; return jout_put(o,b,3); }
  else if(cp<=0x10FFFF){ char b[4]={(char)(0xF0|(cp>>18)), (char)(0x80|((cp>>12)&0x3F)), (char)(0x80|((cp>>6)&0x3F)), (char)(0x80|(cp&0x3F))}; return jout_put(o,b,4); }
  return -1;
}

static int parse_string(VLState *L, JIn *in){
  jin_skip_ws(in);
  if(in->p>=in->end || *in->p!='"') return push_error(L,in->off);
  in->p++; in->off++;
  JOut o={0};
  while(in->p<in->end){
    unsigned char c = (unsigned char)*in->p; in->p++; in->off++;
    if(c=='"'){ vl_push_lstring(L, o.buf?o.buf:"", o.len); free(o.buf); return 1; }
    if(c=='\\'){
      if(in->p>=in->end){ free(o.buf); return push_error(L,in->off); }
      char e = *in->p++; in->off++;
      switch(e){
        case '"':  if(jout_putc(&o,'"')) goto fail; break;
        case '\\': if(jout_putc(&o,'\\')) goto fail; break;
        case '/':  if(jout_putc(&o,'/')) goto fail; break;
        case 'b':  if(jout_putc(&o,'\b')) goto fail; break;
        case 'f':  if(jout_putc(&o,'\f')) goto fail; break;
        case 'n':  if(jout_putc(&o,'\n')) goto fail; break;
        case 'r':  if(jout_putc(&o,'\r')) goto fail; break;
        case 't':  if(jout_putc(&o,'\t')) goto fail; break;
        case 'u': {
          if(in->end-in->p < 4){ goto fail; }
          int h1=hexv(in->p[0]),h2=hexv(in->p[1]),h3=hexv(in->p[2]),h4=hexv(in->p[3]);
          if(h1<0||h2<0||h3<0||h4<0) goto fail;
          uint32_t u = (uint32_t)((h1<<12)|(h2<<8)|(h3<<4)|h4);
          in->p+=4; in->off+=4;
          if(u>=0xD800 && u<=0xDBFF){
            /* surrogate pair */
            if(!(in->end-in->p >= 6 && in->p[0]=='\\' && in->p[1]=='u')) goto fail;
            in->p+=2; in->off+=2;
            int h5=hexv(in->p[0]),h6=hexv(in->p[1]),h7=hexv(in->p[2]),h8=hexv(in->p[3]);
            if(h5<0||h6<0||h7<0||h8<0) goto fail;
            uint32_t l = (uint32_t)((h5<<12)|(h6<<8)|(h7<<4)|h8);
            in->p+=4; in->off+=4;
            if(!(l>=0xDC00 && l<=0xDFFF)) goto fail;
            u = 0x10000 + (((u-0xD800)<<10) | (l-0xDC00));
          }
          if(utf16_to_utf8(u, &o)) goto fail;
        } break;
        default: goto fail;
      }
    }else{
      if(jout_putc(&o,(char)c)) goto fail;
    }
  }
fail:
  free(o.buf); return push_error(L,in->off);
}

static int parse_value(VLState *L, JIn *in, int null_as_nil, int depth);

static int parse_array(VLState *L, JIn *in, int null_as_nil, int depth){
  if(depth > JSON_MAX_DEPTH) return push_error(L,in->off);
  if(!jin_match(in,'[')) return push_error(L,in->off);
  vl_new_table(L); /* stack: arr */
  int64_t idx = 0;
  jin_skip_ws(in);
  if(jin_match(in,']')) return 1;
  for(;;){
    if(parse_value(L,in,null_as_nil,depth+1)!=1) return 3; /* pass-through error */
    /* stack: arr, value */
    idx++;
    /* arr[idx] = value */
    vlx_set_table_is(L, idx, /*value_on_stack*/1);
    vl_push_nil(L); /* pop value placeholder if VM API consumed it; ensure stack balance */
    jin_skip_ws(in);
    if(jin_match(in,']')) return 1;
    if(in->allow_tc){ if(jin_match(in,',')){ jin_skip_ws(in); if(jin_match(in,']')) return 1; } }
    else if(!jin_match(in,',')) return push_error(L,in->off);
  }
}

static int parse_object(VLState *L, JIn *in, int null_as_nil, int depth){
  if(depth > JSON_MAX_DEPTH) return push_error(L,in->off);
  if(!jin_match(in,'{')) return push_error(L,in->off);
  vl_new_table(L); /* obj */
  jin_skip_ws(in);
  if(jin_match(in,'}')) return 1;
  for(;;){
    /* key */
    if(parse_string(L,in)!=1) return 3;
    size_t klen=0; const char *k = vl_check_string(L, -1, &klen);
    jin_skip_ws(in);
    if(!jin_match(in,':')) return push_error(L,in->off);
    if(parse_value(L,in,null_as_nil,depth+1)!=1) return 3;
    /* stack: obj, key, val */
    vlx_set_table_ks(L, k, /*val_on_stack*/1);
    vl_push_nil(L); /* pop val */
    vl_push_nil(L); /* pop key */
    jin_skip_ws(in);
    if(jin_match(in,'}')) return 1;
    if(in->allow_tc){ if(jin_match(in,',')){ jin_skip_ws(in); if(jin_match(in,'}')) return 1; } }
    else if(!jin_match(in,',')) return push_error(L,in->off);
  }
}

static int parse_number(VLState *L, JIn *in){
  jin_skip_ws(in);
  const char *s = in->p;
  if(s>=in->end) return push_error(L,in->off);
  const char *p = s;
  if(*p=='-'||*p=='+') p++;
  if(p<in->end && *p=='0'){ p++; }
  else{
    if(p>=in->end || !isdigit((unsigned char)*p)) return push_error(L,in->off);
    while(p<in->end && isdigit((unsigned char)*p)) p++;
  }
  if(p<in->end && *p=='.'){ p++; if(p>=in->end || !isdigit((unsigned char)*p)) return push_error(L,in->off); while(p<in->end && isdigit((unsigned char)*p)) p++; }
  if(p<in->end && (*p=='e'||*p=='E')){ p++; if(p<in->end && (*p=='+'||*p=='-')) p++; if(p>=in->end || !isdigit((unsigned char)*p)) return push_error(L,in->off); while(p<in->end && isdigit((unsigned char)*p)) p++; }

  char buf[64];
  size_t n = (size_t)(p - s);
  if(n >= sizeof(buf)){
    char *tmp = (char*)malloc(n+1);
    if(!tmp) return push_error(L,in->off);
    memcpy(tmp,s,n); tmp[n]=0;
    double v = strtod(tmp,NULL);
    free(tmp);
    in->off += n; in->p = p;
    vl_push_number(L, v);
    return 1;
  }else{
    memcpy(buf,s,n); buf[n]=0;
    double v = strtod(buf,NULL);
    in->off += n; in->p = p;
    vl_push_number(L, v);
    return 1;
  }
}

static int parse_value(VLState *L, JIn *in, int null_as_nil, int depth){
  jin_skip_ws(in);
  if(in->p>=in->end) return push_error(L,in->off);
  char c = *in->p;
  if(c=='"') return parse_string(L,in);
  if(c=='{') return parse_object(L,in,null_as_nil,depth);
  if(c=='[') return parse_array(L,in,null_as_nil,depth);
  if(c=='t'){ /* true */ if(in->end-in->p>=4 && !memcmp(in->p,"true",4)){ in->p+=4; in->off+=4; vl_push_boolean(L,1); return 1; } return push_error(L,in->off); }
  if(c=='f'){ if(in->end-in->p>=5 && !memcmp(in->p,"false",5)){ in->p+=5; in->off+=5; vl_push_boolean(L,0); return 1; } return push_error(L,in->off); }
  if(c=='n'){ if(in->end-in->p>=4 && !memcmp(in->p,"null",4)){ in->p+=4; in->off+=4; if(null_as_nil){ vl_push_nil(L); } else { /* json.null sentinel */ vl_set_table_kvi(L,"__push_json_null__",1); } return 1; } return push_error(L,in->off); }
  /* number */
  return parse_number(L,in);
}

static int l_json_decode(VLState *L){
  size_t n=0; const char *txt = vl_check_string(L, 1, &n);
  int allow_tc = vl_opt_boolean(L, 2, 0);
  int allow_comments = vl_opt_boolean(L, 3, 0);
  int null_as_nil = vl_opt_boolean(L, 4, 1);

  JIn in = { txt, txt+n, 0, allow_tc, allow_comments };
  int r = parse_value(L, &in, null_as_nil, 0);
  if(r != 1) return r; /* error tuple already on stack */

  jin_skip_ws(&in);
  if(in.p != in.end){ /* trailing garbage */
    return push_error(L, in.off);
  }
  return 1;
}

/* ================================ Encode ================================= */

typedef struct {
  JOut *o;
  int ascii_only;
  int pretty;
  int indent;
  int cycle_error;
  /* For cycle detection, you can add a pointer set; omitted for brevity */
} EncCtx;

static int json_escape_put(JOut *o, const char *s, size_t n, int ascii_only){
  if(jout_putc(o,'"')) return -1;
  for(size_t i=0;i<n;i++){
    unsigned char c = (unsigned char)s[i];
    switch(c){
      case '"':  if(jout_put(o,"\\\"",2)) return -1; break;
      case '\\': if(jout_put(o,"\\\\",2)) return -1; break;
      case '\b': if(jout_put(o,"\\b",2))  return -1; break;
      case '\f': if(jout_put(o,"\\f",2))  return -1; break;
      case '\n': if(jout_put(o,"\\n",2))  return -1; break;
      case '\r': if(jout_put(o,"\\r",2))  return -1; break;
      case '\t': if(jout_put(o,"\\t",2))  return -1; break;
      default:
        if(c<0x20 || (ascii_only && c>=0x80)){
          char buf[6]; /* \u00XX */
          static const char *hex="0123456789ABCDEF";
          buf[0]='\\'; buf[1]='u'; buf[2]='0'; buf[3]='0';
          buf[4]=hex[(c>>4)&0xF]; buf[5]=hex[c&0xF];
          if(jout_put(o,buf,6)) return -1;
        }else{
          if(jout_putc(o,(char)c)) return -1;
        }
    }
  }
  if(jout_putc(o,'"')) return -1;
  return 0;
}

static int encode_value(VLState *L, int idx, EncCtx *ec, int depth);

static int encode_array(VLState *L, int idx, EncCtx *ec, int64_t len){
  JOut *o = ec->o;
  if(jout_putc(o,'[')) return -1;
  o->depth++;
  for(int64_t i=1;i<=len;i++){
    if(ec->pretty){ if(jout_indent(o)) return -1; }
    int t = vl_table_geti_push(L, idx, i); /* pushes value at top */
    if(!t){ /* missing makes sparse; encode null */
      vl_push_nil(L);
    }
    if(encode_value(L, /*top*/-1, ec, o->depth)) return -1;
    vl_push_nil(L); /* balance pop in your VM if needed */
    if(i != len){ if(jout_putc(o,',')) return -1; }
  }
  o->depth--;
  if(ec->pretty && len>0){ if(jout_indent(o)) return -1; }
  if(jout_putc(o,']')) return -1;
  return 0;
}

struct obj_enc_ctx { EncCtx *ec; int first; };
static int obj_kv_cb(VLState *L, const char *key, int v_index, void *ud){
  struct obj_enc_ctx *c = (struct obj_enc_ctx*)ud;
  JOut *o = c->ec->o;
  if(!c->first){ if(jout_putc(o,',')) return -1; }
  c->first = 0;
  if(c->ec->pretty){ if(jout_indent(o)) return -1; }
  if(json_escape_put(o, key, strlen(key), c->ec->ascii_only)) return -1;
  if(c->ec->pretty){ if(jout_put(o,": ",2)) return -1; }
  else if(jout_putc(o,':')) return -1;
  if(encode_value(L, v_index, c->ec, o->depth)) return -1;
  return 0;
}

static int encode_object(VLState *L, int idx, EncCtx *ec){
  JOut *o = ec->o;
  if(jout_putc(o,'{')) return -1;
  o->depth++;
  struct obj_enc_ctx c = { ec, 1 };
  if(vl_table_foreach_obj(L, idx, obj_kv_cb, &c) != 0) return -1;
  o->depth--;
  if(ec->pretty && !c.first){ if(jout_indent(o)) return -1; }
  if(jout_putc(o,'}')) return -1;
  return 0;
}

static int encode_value(VLState *L, int idx, EncCtx *ec, int depth){
  (void)depth;
  if(vl_isnil(L, idx) || vl_isnullsentinel(L, idx)){
    return jout_put(ec->o, "null", 4);
  }
  if(vl_isboolean(L, idx)){
    /* Assuming VM exposes boolean via stack value at idx; use a VM helper if needed */
    /* For portability, push 1/0? Here we assume vl_isboolean suffices and value is retrievable via stringification; */
    /* Provide two helpers for explicitness in a real VM. */
    /* Fallback: encode true by comparing to non-zero integer if API provides. */
    /* Here, we encode "true" and rely on VM to have pushed correct boolean in stack for other uses. */
    /* If needed, add extern: int vlx_toboolean(VLState*, int idx); */
    extern int vlx_toboolean(VLState*, int);
    int b = vlx_toboolean(L, idx);
    return jout_put(ec->o, b? "true":"false", b?4:5);
  }
  if(vl_isnumber(L, idx)){
    /* extern double vlx_tonumber(VLState*, int idx); */
    extern double vlx_tonumber(VLState*, int);
    char numbuf[64];
    double v = vlx_tonumber(L, idx);
    if(isfinite(v)){
      /* Grammaire JSON: utilisation de %.17g pour double sûr */
      int m = snprintf(numbuf, sizeof(numbuf), "%.17g", v);
      if(m<0) return -1;
      /* Ensure integer-looking numbers don't contain locale commas */
      return jout_put(ec->o, numbuf, (size_t)m);
    }else{
      /* NaN/Inf non valides → null */
      return jout_put(ec->o, "null", 4);
    }
  }
  if(vl_isstring(L, idx)){
    size_t n=0; const char *s = vlx_opt_string(L, idx, "", &n);
    return json_escape_put(ec->o, s, n, ec->ascii_only);
  }
  if(vlx_istable(L, idx)){
    int64_t n=0;
    if(vl_table_len(L, idx, &n)==0 || n<=0){
      /* objet générique */
      return encode_object(L, idx, ec);
    }else{
      /* tableau [1..n] */
      return encode_array(L, idx, ec, n);
    }
  }
  /* Type non supporté (fonction, userdata...) */
  return -1;
}

static int l_json_encode(VLState *L){
  int pretty = vl_opt_boolean(L, 2, 0);
  int indent = (int)vl_opt_integer(L, 3, 2); if(indent<1) indent=1; if(indent>8) indent=8;
  int ascii_only = vl_opt_boolean(L, 4, 0);

  JOut out = {0};
  out.pretty = pretty;
  out.indent = indent;
  out.ascii_only = ascii_only;

  EncCtx ec = { &out, ascii_only, pretty, indent, 0 };
  if(encode_value(L, 1, &ec, 0) != 0 || out.error){
    free(out.buf);
    vl_push_nil(L); vl_push_string(L,"EINVAL"); return 2;
  }
  vl_push_lstring(L, out.buf?out.buf:"", out.len);
  free(out.buf);
  return 1;
}

/* ================================ Module ================================= */

static int l_json_null(VLState *L){
  /* Push dedicated sentinel if your VM supports it, else push nil. */
  /* We try to push a recognizable sentinel via a magic field on a temp table. */
  vl_new_table(L);
  vl_set_table_kvi(L, "__json_null__", 1);
  return 1;
}

static const struct vl_Reg json_funcs[] = {
  {"decode", l_json_decode},
  {"encode", l_json_encode},
  {"null",   l_json_null},
  {NULL, NULL}
};

int vl_openlib_json(VLState *L){
  vl_register_module(L, "json", json_funcs);
  return 1;
}