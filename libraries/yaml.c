// SPDX-License-Identifier: GPL-3.0-or-later
//
// yaml.c — Mini YAML (subset) : lecture/écriture arbre YAML (C17, portable)
// Namespace: "yaml"
//
// Objectif: parser tolérant d’un sous-ensemble YAML:
//   - Espaces (indentation en multiples d’espaces, pas de tabulations)
//   - Mappings:  key: value    ou   key:    puis bloc indenté
//   - Séquences: - item        ou   -       puis bloc indenté
//   - Scalars:   non-quotés, "double-quotés" (avec \n\t\"\\), 'simples'
//   - Commentaires commençant par # hors guillemets
// Non géré: ancres, alias, tags, multi-docs, block scalars (|, >), floats spéciaux.
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c yaml.c
//
// API minimale:
//   typedef enum { YAML_SCALAR=1, YAML_MAP=2, YAML_SEQ=3 } yaml_kind;
//   typedef struct yaml_node yaml_node;
//
//   /* Chargement / libération */
//   yaml_node* yaml_load_mem(const char* buf, size_t n);   // NULL si erreur
//   yaml_node* yaml_load_file(const char* path);           // NULL si erreur
//   void       yaml_free(yaml_node* n);
//
//   /* Interrogation */
//   yaml_kind  yaml_type(const yaml_node* n);
//   const char* yaml_scalar(const yaml_node* n);            // NULL si pas SCALAR
//   size_t     yaml_map_size(const yaml_node* n);           // paires
//   const char*yaml_map_key (const yaml_node* n, size_t i); // i<map_size
//   const yaml_node* yaml_map_get(const yaml_node* n, const char* key); // NULL si absent
//   size_t     yaml_seq_size(const yaml_node* n);
//   const yaml_node* yaml_seq_at(const yaml_node* n, size_t i);         // NULL si hors borne
//
//   /* Emission (joli) */
//   int yaml_emit_fp(const yaml_node* n, FILE* fp);
//   int yaml_emit_file(const yaml_node* n, const char* path);
//
// Remarque: conçu pour la configuration. Les erreurs renvoient NULL/-1.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libctype.h"
#include <stddef.h> /* ptrdiff_t */

#ifndef YAML_API
#define YAML_API
#endif

/* ===================== Types publics ===================== */

typedef enum { YAML_SCALAR=1, YAML_MAP=2, YAML_SEQ=3 } yaml_kind;

typedef struct {
    char* key;
    struct yaml_node* val;
} yaml_pair;

typedef struct yaml_node {
    yaml_kind  kind;
    union {
        char*      scalar;
        struct { yaml_pair* items; size_t len, cap; } map;
        struct { struct yaml_node** items; size_t len, cap; } seq;
    } u;
} yaml_node;

/* ===================== Helpers alloc ===================== */

static void* xmalloc(size_t n){ void* p=malloc(n?n:1); return p; }
static void* xrealloc(void* p, size_t n){ void* q=realloc(p, n?n:1); return q; }
static char* xstrndup0(const char* s, size_t n){ char* d=(char*)xmalloc(n+1); if(!d) return NULL; memcpy(d,s,n); d[n]=0; return d; }
static char* xstrdup0(const char* s){ return s? xstrndup0(s, strlen(s)) : xstrndup0("",0); }

/* ===================== Node makers ===================== */

static yaml_node* yn_scalar_new(const char* s, size_t n){
    yaml_node* y=(yaml_node*)calloc(1,sizeof *y); if(!y) return NULL;
    y->kind=YAML_SCALAR; y->u.scalar = xstrndup0(s, n==(size_t)-1? strlen(s):n);
    if(!y->u.scalar){ free(y); return NULL; }
    return y;
}
static yaml_node* yn_map_new(void){
    yaml_node* y=(yaml_node*)calloc(1,sizeof *y); if(!y) return NULL;
    y->kind=YAML_MAP; return y;
}
static yaml_node* yn_seq_new(void){
    yaml_node* y=(yaml_node*)calloc(1,sizeof *y); if(!y) return NULL;
    y->kind=YAML_SEQ; return y;
}

static int yn_map_put_move(yaml_node* m, char* key, yaml_node* val){
    if(!m||m->kind!=YAML_MAP||!key||!val) return -1;
    if (m->u.map.len==m->u.map.cap){
        size_t nc = m->u.map.cap? m->u.map.cap*2 : 8;
        yaml_pair* ni=(yaml_pair*)xrealloc(m->u.map.items, nc*sizeof *ni);
        if(!ni) return -1; m->u.map.items=ni; m->u.map.cap=nc;
    }
    m->u.map.items[m->u.map.len].key=key;
    m->u.map.items[m->u.map.len].val=val;
    m->u.map.len++;
    return 0;
}
static int yn_seq_push_move(yaml_node* s, yaml_node* val){
    if(!s||s->kind!=YAML_SEQ||!val) return -1;
    if (s->u.seq.len==s->u.seq.cap){
        size_t nc = s->u.seq.cap? s->u.seq.cap*2 : 8;
        yaml_node** ni=(yaml_node**)xrealloc(s->u.seq.items, nc*sizeof *ni);
        if(!ni) return -1; s->u.seq.items=ni; s->u.seq.cap=nc;
    }
    s->u.seq.items[s->u.seq.len++]=val; return 0;
}

/* ===================== Public getters ===================== */

YAML_API yaml_kind yaml_type(const yaml_node* n){ return n? n->kind:0; }
YAML_API const char* yaml_scalar(const yaml_node* n){ return (n&&n->kind==YAML_SCALAR)? n->u.scalar:NULL; }
YAML_API size_t yaml_map_size(const yaml_node* n){ return (n&&n->kind==YAML_MAP)? n->u.map.len:0; }
YAML_API const char* yaml_map_key(const yaml_node* n, size_t i){
    if(!n||n->kind!=YAML_MAP||i>=n->u.map.len) return NULL;
    return n->u.map.items[i].key;
}
YAML_API const yaml_node* yaml_map_get(const yaml_node* n, const char* key){
    if(!n||n->kind!=YAML_MAP||!key) return NULL;
    for(size_t i=0;i<n->u.map.len;i++) if(n->u.map.items[i].key && strcmp(n->u.map.items[i].key,key)==0) return n->u.map.items[i].val;
    return NULL;
}
YAML_API size_t yaml_seq_size(const yaml_node* n){ return (n&&n->kind==YAML_SEQ)? n->u.seq.len:0; }
YAML_API const yaml_node* yaml_seq_at(const yaml_node* n, size_t i){
    if(!n||n->kind!=YAML_SEQ||i>=n->u.seq.len) return NULL;
    return n->u.seq.items[i];
}

/* ===================== Free ===================== */

YAML_API void yaml_free(yaml_node* n){
    if(!n) return;
    if (n->kind==YAML_SCALAR){
        free(n->u.scalar);
    } else if (n->kind==YAML_MAP){
        for(size_t i=0;i<n->u.map.len;i++){
            free(n->u.map.items[i].key);
            yaml_free(n->u.map.items[i].val);
        }
        free(n->u.map.items);
    } else if (n->kind==YAML_SEQ){
        for(size_t i=0;i<n->u.seq.len;i++) yaml_free(n->u.seq.items[i]);
        free(n->u.seq.items);
    }
    free(n);
}

/* ===================== Lexer de lignes ===================== */

typedef struct { const char* s; size_t n; size_t off; size_t lineno; } ybuf_t;
static int y_eof(ybuf_t* b){ return b->off>=b->n; }

/* Lit une ligne brute, sans \n final. Renvoie pointeur dans le buffer d’entrée. */
static const char* y_getline(ybuf_t* b, size_t* ln){
    if (y_eof(b)){ *ln=0; return NULL; }
    size_t i=b->off;
    while (i<b->n && b->s[i]!='\n' && b->s[i]!='\r') i++;
    const char* p=b->s + b->off;
    *ln = i - b->off;
    /* sauter fin de ligne */
    if (i<b->n && b->s[i]=='\r') i++;
    if (i<b->n && b->s[i]=='\n') i++;
    b->off = i; b->lineno++;
    return p;
}

/* Trim fin + enlève commentaires # hors guillemets */
static size_t y_strip_comment(char* dst, const char* src, size_t n){
    int in_s=0, in_d=0;
    size_t w=0;
    for(size_t i=0;i<n;i++){
        char c=src[i];
        if (c=='\'' && !in_d) in_s = !in_s;
        else if (c=='\"' && !in_s) in_d = !in_d;
        if (c=='#' && !in_s && !in_d){ break; }
        dst[w++]=c;
    }
    /* rtrim spaces */
    while (w && isspace((unsigned char)dst[w-1])) w--;
    dst[w]=0;
    return w;
}

/* Compte indentation (espaces). Refuse tabulations. Renvoie -1 si tab. */
static int y_count_indent(const char* s, size_t n){
    int col=0;
    for(size_t i=0;i<n;i++){
        if (s[i]==' ') col++;
        else if (s[i]=='\t') return -1;
        else break;
    }
    return col;
}

/* Déquote scalaires "..." et '...' + unescape basique pour " \n \t \" \\ */
static char* y_unquote_scalar(const char* s, size_t n){
    if (n==0) return xstrndup0("",0);
    if (s[0]=='\"' && n>=2 && s[n-1]=='\"'){
        /* double quotes: unescape */
        char* out=(char*)xmalloc(n); if(!out) return NULL;
        size_t w=0;
        for(size_t i=1;i<n-1;i++){
            char c=s[i];
            if (c=='\\' && i+1<n-1){
                char e=s[++i];
                if (e=='n') out[w++]='\n';
                else if (e=='t') out[w++]='\t';
                else if (e=='r') out[w++]='\r';
                else if (e=='\\') out[w++]='\\';
                else if (e=='\"') out[w++]='\"';
                else out[w++]=e;
            } else out[w++]=c;
        }
        out[w]=0; return out;
    }
    if (s[0]=='\'' && n>=2 && s[n-1]=='\''){
        /* single quotes: '' -> ' */
        char* out=(char*)xmalloc(n); if(!out) return NULL;
        size_t w=0;
        for(size_t i=1;i<n-1;i++){
            char c=s[i];
            if (c=='\'' && i+1<n-1 && s[i+1]=='\''){ out[w++]='\''; i++; }
            else out[w++]=c;
        }
        out[w]=0; return out;
    }
    /* plain: trim both sides */
    size_t b=0, e=n;
    while (b<e && isspace((unsigned char)s[b])) b++;
    while (e>b && isspace((unsigned char)s[e-1])) e--;
    return xstrndup0(s+b, e-b);
}

/* ===================== Parser récursif par indentation ===================== */

typedef struct {
    ybuf_t* buf;
    /* ligne courante mise en forme */
    char*   line; size_t linelen;
    int     indent;
    int     valid;
    /* pour “remettre” une ligne lors d’une remontée */
    const char* hold_src; size_t hold_len; int hold_indent; int have_hold;
} yctx_t;

static void yctx_init(yctx_t* y, ybuf_t* b){
    memset(y,0,sizeof *y); y->buf=b; y->indent=0; y->valid=0;
}
static void yctx_free_line(yctx_t* y){ free(y->line); y->line=NULL; y->linelen=0; }

/* Charge la prochaine ligne logique: renvoie 1 si ok, 0 si EOF. */
static int y_next(yctx_t* y){
    yctx_free_line(y);
    if (y->have_hold){
        y->have_hold=0;
        y->line = xstrndup0(y->hold_src, y->hold_len);
        y->linelen = y->hold_len;
        y->indent = y->hold_indent;
        y->valid = 1;
        return 1;
    }
    size_t n0=0; const char* p=NULL;
    for(;;){
        p = y_getline(y->buf, &n0);
        if (!p) { y->valid=0; return 0; }
        /* strip comment and trailing ws */
        char* tmp = (char*)xmalloc(n0+1); if(!tmp){ y->valid=0; return 0; }
        size_t n = y_strip_comment(tmp, p, n0);
        /* skip empty lines */
        size_t i=0; while(i<n && isspace((unsigned char)tmp[i])) i++;
        if (i==n){ free(tmp); continue; }
        int ind = y_count_indent(tmp, n);
        if (ind<0){ free(tmp); continue; } /* ignore tabs lines */
        y->line = tmp; y->linelen = n; y->indent = ind; y->valid=1; return 1;
    }
}

/* Remet en file d’attente la ligne courante (utilisé quand l’indent remonte). */
static void y_unread(yctx_t* y){
    if (!y->valid) return;
    y->hold_src = y->line;
    y->hold_len = y->linelen;
    y->hold_indent = y->indent;
    y->have_hold = 1;
    y->line = NULL; y->linelen=0; y->valid=0;
}

/* Parse un bloc (à une indentation de base donnée). */
static yaml_node* y_parse_block(yctx_t* y, int base_ind);

/* Tente de lire une séquence si la première ligne commence par “- ” ou “-” */
static yaml_node* y_parse_seq(yctx_t* y, int base_ind){
    yaml_node* seq = yn_seq_new(); if(!seq) return NULL;

    while (y->valid && y->indent==base_ind){
        /* ligne doit démarrer par '-' optionnellement suivi d’un espace */
        size_t i=0;
        while (i<y->linelen && y->line[i]==' ') i++;
        if (i>=y->linelen || y->line[i]!='-'){ break; }
        i++;
        /* après '-' :
           - si rien (ou seulement espaces) -> item bloc indenté suivant
           - si espace puis contenu -> scalar court sur la même ligne
           - si “key:” -> en fait un map inline? on traite comme valeur scalaire à droite
        */
        while (i<y->linelen && y->line[i]==' ') i++;
        yaml_node* item=NULL;

        if (i>=y->linelen){ /* item vide -> bloc suivant plus indenté */
            if (!y_next(y)){ /* fin */
                item = yn_scalar_new("", 0);
                if(!item){ yaml_free(seq); return NULL; }
                if (yn_seq_push_move(seq, item)!=0){ yaml_free(item); yaml_free(seq); return NULL; }
                break;
            }
            if (!y->valid){ item = yn_scalar_new("",0); }
            else if (y->indent<=base_ind){ /* pas de bloc -> item vide */
                y_unread(y);
                item = yn_scalar_new("",0);
            }else{
                /* parse sous-bloc à indent supérieure */
                item = y_parse_block(y, y->indent);
            }
            if(!item){ yaml_free(seq); return NULL; }
            if (yn_seq_push_move(seq, item)!=0){ yaml_free(item); yaml_free(seq); return NULL; }
            /* nous sommes déjà positionnés après sous-bloc */
            continue;
        }

        /* valeur sur la même ligne */
        /* essaie “key: value” => on le traite comme SCALAR brut (heuristique simple) */
        char* val = y_unquote_scalar(y->line + i, y->linelen - i);
        if(!val){ yaml_free(seq); return NULL; }
        item = yn_scalar_new(val, (size_t)-1);
        free(val);
        if(!item){ yaml_free(seq); return NULL; }
        if (yn_seq_push_move(seq, item)!=0){ yaml_free(item); yaml_free(seq); return NULL; }

        /* ligne consommée, lire la prochaine pour voir si d’autres items */
        if (!y_next(y)) break;
        if (!y->valid) break;
        if (y->indent<base_ind){ y_unread(y); break; }
        if (y->indent>base_ind){ /* ligne de suite trop indentée => rattacher comme sous-bloc du dernier item */
            yaml_node* sub = y_parse_block(y, y->indent);
            if (!sub){ yaml_free(seq); return NULL; }
            /* Si item est SCALAR et sub est MAP/SEQ, on remplace l’item par sub (common YAML “- key: v”) */
            /* Pour rester simple: si sub existe et pas vide, on remplace. */
            seq->u.seq.items[seq->u.seq.len-1] = sub;
        }
    }
    return seq;
}

/* Tente de lire un mapping à partir d’une ligne "key:" ou "key: value" */
static yaml_node* y_parse_map(yctx_t* y, int base_ind){
    yaml_node* map = yn_map_new(); if(!map) return NULL;

    while (y->valid && y->indent==base_ind){
        /* découpe “key: ...” */
        const char* s = y->line + base_ind;
        const char* colon = NULL;
        /* trouver ':' hors guillemets simples/doubles */
        int in_s=0,in_d=0;
        for(size_t k=0;k<y->linelen - (size_t)base_ind; k++){
            char c=s[k];
            if (c=='\'' && !in_d) in_s=!in_s;
            else if (c=='\"' && !in_s) in_d=!in_d;
            if (c==':' && !in_s && !in_d){ colon = s+k; break; }
        }
        if (!colon){ break; } /* pas un map, peut-être une séquence */
        /* clé = trim(s..colon) */
        size_t kb=0, ke=(size_t)(colon - s);
        while (kb<ke && isspace((unsigned char)s[kb])) kb++;
        while (ke>kb && isspace((unsigned char)s[ke-1])) ke--;
        char* kstr = y_unquote_scalar(s+kb, ke-kb);
        if(!kstr){ yaml_free(map); return NULL; }

        /* après ':' */
        const char* rest = colon+1;
        while (*rest==' ') rest++;
        yaml_node* val=NULL;

        if (*rest==0){ /* valeur sur lignes suivantes (bloc indenté) */
            if (!y_next(y)){ /* fin -> valeur vide */
                val = yn_scalar_new("",0);
            } else if (!y->valid || y->indent<=base_ind){
                /* pas de bloc -> valeur vide et on remet la ligne si indent==base_ind */
                if (y->valid && y->indent==base_ind) y_unread(y);
                val = yn_scalar_new("",0);
            } else {
                val = y_parse_block(y, y->indent);
            }
            if(!val){ free(kstr); yaml_free(map); return NULL; }
        } else {
            /* valeur inline scalaire (ou pseudo-struct) -> on prend en scalaire */
            char* vstr = y_unquote_scalar(rest, y->linelen - (size_t)(rest - y->line));
            if(!vstr){ free(kstr); yaml_free(map); return NULL; }
            val = yn_scalar_new(vstr, (size_t)-1);
            free(vstr);
            if(!val){ free(kstr); yaml_free(map); return NULL; }

            /* lire la ligne suivante pour détecter un sous-bloc attaché (indent > base) */
            if (y_next(y)){
                if (y->valid && y->indent>base_ind){
                    yaml_node* sub = y_parse_block(y, y->indent);
                    if (!sub){ free(kstr); yaml_free(val); yaml_free(map); return NULL; }
                    /* on remplace la valeur scalaire par le sous-bloc */
                    yaml_free(val); val=sub;
                } else if (y->valid && y->indent<base_ind){
                    y_unread(y);
                } else if (y->valid && y->indent==base_ind){
                    y_unread(y);
                }
            }
        }

        if (yn_map_put_move(map, kstr, val)!=0){ free(kstr); yaml_free(val); yaml_free(map); return NULL; }

        /* avancer si on n’a pas déjà lu la prochaine */
        if (!y->valid && !y_next(y)) break;
        if (!y->valid) break;
        if (y->indent<base_ind){ y_unread(y); break; }
        if (y->indent>base_ind){ /* ligne trop indentée au niveau map -> rattacher? on considère comme sous-bloc invalide */
            yaml_node* sub = y_parse_block(y, y->indent);
            if (!sub){ yaml_free(map); return NULL; }
            /* Pas de clé à associer: on ignore proprement en le libérant */
            yaml_free(sub);
        }
        /* sinon boucle continue si indent==base */
    }
    return map;
}

static yaml_node* y_parse_block(yctx_t* y, int base_ind){
    /* Décider seq vs map vs scalar à partir de la ligne courante. */
    if (!y->valid) return yn_scalar_new("",0);

    /* séquence si commence par '-' (après indentation) */
    size_t i=0;
    while (i<y->linelen && y->line[i]==' ') i++;
    if (i<y->linelen && y->line[i]=='-'){
        return y_parse_seq(y, base_ind);
    }

    /* mapping si contient ':' hors guillemets */
    const char* s=y->line + base_ind;
    int in_s=0,in_d=0; int has_colon=0;
    for(size_t k=0;k<y->linelen - (size_t)base_ind; k++){
        char c=s[k];
        if (c=='\'' && !in_d) in_s=!in_s;
        else if (c=='\"' && !in_s) in_d=!in_d;
        if (c==':' && !in_s && !in_d){ has_colon=1; break; }
    }
    if (has_colon){
        return y_parse_map(y, base_ind);
    }

    /* sinon: scalaire simple à cette ligne, puis on avance */
    char* v = y_unquote_scalar(y->line + base_ind, y->linelen - (size_t)base_ind);
    if(!v) return NULL;
    yaml_node* sc = yn_scalar_new(v, (size_t)-1);
    free(v);
    if (!sc) return NULL;

    if (!y_next(y)) return sc;
    if (!y->valid) return sc;
    if (y->indent>base_ind){
        yaml_node* sub = y_parse_block(y, y->indent);
        if (sub){ yaml_free(sc); return sub; }
    }
    if (y->indent<base_ind) y_unread(y);
    else y_unread(y);
    return sc;
}

/* ===================== Chargeurs ===================== */

YAML_API yaml_node* yaml_load_mem(const char* buf, size_t n){
    if (!buf) return NULL;
    ybuf_t b = { buf, n?n:strlen(buf), 0, 0 };
    yctx_t y; yctx_init(&y,&b);
    /* lire première ligne non vide */
    if (!y_next(&y)){
        yctx_free_line(&y);
        return yn_scalar_new("",0);
    }
    yaml_node* root = y_parse_block(&y, y.indent);
    yctx_free_line(&y);
    return root;
}

YAML_API yaml_node* yaml_load_file(const char* path){
    FILE* f=fopen(path,"rb"); if(!f) return NULL;
    if (fseek(f,0,SEEK_END)!=0){ fclose(f); return NULL; }
    long L=ftell(f); if (L<0){ fclose(f); return NULL; }
    if (fseek(f,0,SEEK_SET)!=0){ fclose(f); return NULL; }
    char* buf=(char*)xmalloc((size_t)L+1); if(!buf){ fclose(f); return NULL; }
    size_t r=fread(buf,1,(size_t)L,f); fclose(f);
    if (r!=(size_t)L){ free(buf); return NULL; }
    buf[L]=0;
    yaml_node* y = yaml_load_mem(buf,(size_t)L);
    free(buf);
    return y;
}

/* ===================== Emetteur ===================== */

static void y_indent(FILE* fp, int n){ for(int i=0;i<n;i++) fputc(' ',fp); }

static void y_emit_str(FILE* fp, const char* s){
    if (!s){ fputs("''",fp); return; }
    /* choix: émettre en double-quote si caractères spéciaux présents */
    int need_q=0;
    for (const unsigned char* p=(const unsigned char*)s; *p; p++){
        if (isspace(*p) || *p=='#' || *p==':' || *p=='-' || *p=='\"' || *p=='\''){
            need_q=1; break;
        }
    }
    if (!*s) need_q=1;
    if (!need_q){ fputs(s,fp); return; }
    fputc('\"',fp);
    for (const unsigned char* p=(const unsigned char*)s; *p; p++){
        if (*p=='\\' || *p=='\"'){ fputc('\\',fp); fputc(*p,fp); }
        else if (*p=='\n'){ fputs("\\n",fp); }
        else if (*p=='\t'){ fputs("\\t",fp); }
        else fputc(*p,fp);
    }
    fputc('\"',fp);
}

static int y_emit_rec(const yaml_node* n, FILE* fp, int ind){
    if (!n) return 0;
    if (n->kind==YAML_SCALAR){
        y_emit_str(fp, n->u.scalar); fputc('\n',fp); return 0;
    }
    if (n->kind==YAML_SEQ){
        for (size_t i=0;i<n->u.seq.len;i++){
            y_indent(fp, ind); fputs("- ",fp);
            const yaml_node* it=n->u.seq.items[i];
            if (!it || it->kind==YAML_SCALAR){ y_emit_rec(it,fp,ind+2); }
            else { fputc('\n',fp); y_emit_rec(it,fp,ind+2); }
        }
        return 0;
    }
    if (n->kind==YAML_MAP){
        for (size_t i=0;i<n->u.map.len;i++){
            y_indent(fp, ind);
            y_emit_str(fp, n->u.map.items[i].key);
            fputs(": ",fp);
            const yaml_node* v = n->u.map.items[i].val;
            if (!v || v->kind==YAML_SCALAR){ y_emit_rec(v,fp,ind+2); }
            else { fputc('\n',fp); y_emit_rec(v,fp,ind+2); }
        }
        return 0;
    }
    return -1;
}

YAML_API int yaml_emit_fp(const yaml_node* n, FILE* fp){
    if (!n||!fp) return -1;
    return y_emit_rec(n, fp, 0);
}
YAML_API int yaml_emit_file(const yaml_node* n, const char* path){
    FILE* f=fopen(path,"wb"); if(!f) return -1;
    int rc = yaml_emit_fp(n,f);
    fclose(f); return rc;
}

/* ===================== Test ===================== */
#ifdef YAML_TEST
int main(void){
    const char* doc =
        "# demo\n"
        "name: \"Vitte Light\"\n"
        "version: 1.2\n"
        "features:\n"
        "  - cli\n"
        "  - core\n"
        "  - \"yaml io\"\n"
        "build:\n"
        "  cc: gcc\n"
        "  flags: -O2\n";
    yaml_node* y = yaml_load_mem(doc, 0);
    if(!y){ fprintf(stderr,"parse error\n"); return 1; }
    const yaml_node* features = yaml_map_get(y, "features");
    printf("features count=%zu\n", yaml_seq_size(features));
    yaml_emit_file(y, "out.yaml");
    yaml_free(y);
    return 0;
}
#endif