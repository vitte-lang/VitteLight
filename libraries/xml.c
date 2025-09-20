// SPDX-License-Identifier: GPL-3.0-or-later
//
// xml.c — Mini DOM XML (lecteur/émetteur) portable C17
// Namespace: "xml"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c xml.c
//
// API:
//   typedef struct xml_attr  { char *name,*value; struct xml_attr* next; } xml_attr;
//   typedef struct xml_node  { char *name,*text; xml_attr *attrs; struct xml_node *child,*next,*parent; } xml_node;
//   xml_node* xml_load_mem(const char* buf, size_t n);
//   xml_node* xml_load_file(const char* path);
//   void      xml_free(xml_node* x);
//   const char* xml_attr_get(const xml_node* n, const char* key);
//   xml_node*  xml_child(const xml_node* n, const char* name);
//   xml_node*  xml_next(const xml_node* n, const char* name);
//   int  xml_write_file(const xml_node* x, const char* path);
//   int  xml_write_fp(const xml_node* x, FILE* fp);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>   /* ptrdiff_t */

#ifndef XML_API
#define XML_API
#endif

/* ================= Types ================= */

typedef struct xml_attr  { char *name,*value; struct xml_attr* next; } xml_attr;
typedef struct xml_node  {
    char *name; char *text;
    xml_attr *attrs;
    struct xml_node *child,*next,*parent;
} xml_node;

/* ================= Utils ================= */

static void* xmalloc(size_t n){ void* p=malloc(n?n:1); return p; }
static char* xstrndup0(const char* s, size_t n){
    char* d=(char*)xmalloc(n+1); if(!d) return NULL; memcpy(d,s,n); d[n]=0; return d;
}
static char* xstrdup0(const char* s){ if(!s) return xstrndup0("",0); return xstrndup0(s,strlen(s)); }
static int strieq(const char* a, const char* b){
    if(!a||!b) return 0;
    while(*a && *b){
        int ca=tolower((unsigned char)*a++), cb=tolower((unsigned char)*b++);
        if(ca!=cb) return 0;
    }
    return *a==0 && *b==0;
}

/* --- Entités --- */

static int hexval(int c){
    if (c>='0'&&c<='9') return c-'0';
    c=tolower((unsigned char)c);
    if (c>='a'&&c<='f') return 10+c-'a';
    return -1;
}
static size_t xml_unescape(const char* s, size_t n, char** outp){
    char* out=(char*)xmalloc(n+1); if(!out){ *outp=NULL; return 0; }
    size_t w=0;
    for(size_t i=0;i<n;i++){
        if (s[i]=='&'){
            if (i+3<n && !strncmp(s+i,"lt;",3)){ out[w++]='<'; i+=2; continue; }
            if (i+4<n && !strncmp(s+i,"amp;",4)){ out[w++]='&'; i+=3; continue; }
            if (i+3<n && !strncmp(s+i,"gt;",3)){ out[w++]='>'; i+=2; continue; }
            if (i+6<n && !strncmp(s+i,"quot;",5)){ out[w++]='\"'; i+=4; continue; }
            if (i+6<n && !strncmp(s+i,"apos;",5)){ out[w++]='\''; i+=4; continue; }
            if (i+3<n && s[i+1]=='#'){ /* &#...; or &#x...; */
                unsigned long v=0; int base=10; size_t j=i+2;
                if (j<n && (s[j]=='x'||s[j]=='X')){ base=16; j++; }
                for(; j<n; j++){
                    if (s[j]==';') break;
                    int d = (base==16)? hexval(s[j]) : (isdigit((unsigned char)s[j])? s[j]-'0' : -1);
                    if (d<0){ v=0; break; }
                    v = v* (unsigned)base + (unsigned)d;
                }
                if (j<n && s[j]==';'){
                    if (v<=0x7Fu) out[w++]=(char)v;
                    else if (v<=0x7FFu){ out[w++]=(char)(0xC0 | (v>>6)); out[w++]=(char)(0x80 | (v&0x3F)); }
                    else if (v<=0xFFFFu){ out[w++]=(char)(0xE0 | (v>>12)); out[w++]=(char)(0x80 | ((v>>6)&0x3F)); out[w++]=(char)(0x80 | (v&0x3F)); }
                    else { out[w++]=(char)(0xF0 | (v>>18)); out[w++]=(char)(0x80 | ((v>>12)&0x3F)); out[w++]=(char)(0x80 | ((v>>6)&0x3F)); out[w++]=(char)(0x80 | (v&0x3F)); }
                    i = j; continue;
                }
            }
        }
        out[w++]=s[i];
    }
    out[w]=0; *outp=out; return w;
}

static void xml_escape_fp(const char* s, FILE* fp){
    if (!s){ return; }
    for (const unsigned char* p=(const unsigned char*)s; *p; p++){
        if (*p=='&') fputs("&amp;", fp);
        else if (*p=='<') fputs("&lt;", fp);
        else if (*p=='>') fputs("&gt;", fp);
        else if (*p=='\"') fputs("&quot;", fp);
        else if (*p=='\'') fputs("&apos;", fp);
        else fputc(*p, fp);
    }
}

/* ================= Lexer ================= */

typedef struct { const char* p; const char* e; } cur_t;

static void skip_ws(cur_t* c){ while(c->p<c->e && isspace((unsigned char)*c->p)) c->p++; }
static int  starts(cur_t* c, const char* s){ size_t L=strlen(s); return (size_t)(c->e - c->p) >= L && memcmp(c->p,s,L)==0; }
static int  take(cur_t* c, const char* s){ size_t L=strlen(s); if(starts(c,s)){ c->p += (ptrdiff_t)L; return 1;} return 0; }

static size_t span_name(cur_t* c){
    const char* b=c->p;
    if (c->p>=c->e) return 0;
    unsigned char ch=(unsigned char)*c->p;
    if (!(isalpha(ch) || ch=='_' || ch==':')) return 0;
    c->p++;
    while(c->p<c->e){
        ch=(unsigned char)*c->p;
        if (isalnum(ch) || ch=='_' || ch=='-' || ch=='.' || ch==':') c->p++;
        else break;
    }
    return (size_t)(c->p - b);
}

static char* read_quoted(cur_t* c){
    if (c->p>=c->e || (*c->p!='\"' && *c->p!='\'')) return NULL;
    char q=*c->p++;
    const char* b=c->p;
    while(c->p<c->e && *c->p!=q) c->p++;
    if (c->p>=c->e) return NULL;
    size_t n=(size_t)(c->p-b);
    c->p++; /* skip quote */
    char* raw=xstrndup0(b,n); if(!raw) return NULL;
    char* un=NULL; xml_unescape(raw,n,&un); free(raw);
    return un?un:xstrdup0("");
}

/* ================= DOM helpers ================= */

static xml_node* node_new(const char* name, size_t nname){
    xml_node* n=(xml_node*)calloc(1,sizeof *n); if(!n) return NULL;
    n->name = name? xstrndup0(name, nname==(size_t)-1? strlen(name) : nname) : NULL;
    n->text = xstrdup0("");
    return n;
}
static void attr_prepend(xml_node* n, char* k, char* v){
    xml_attr* a=(xml_attr*)xmalloc(sizeof *a);
    a->name=k; a->value=v; a->next=n->attrs; n->attrs=a;
}
static void node_add_child(xml_node* par, xml_node* ch){
    if(!par->child){ par->child=ch; }
    else { xml_node* p=par->child; while(p->next) p=p->next; p->next=ch; }
    ch->parent=par;
}
static int text_append(xml_node* n, const char* b, size_t m){
    if (!m) return 0;
    size_t L=n->text?strlen(n->text):0;
    char* t=(char*)realloc(n->text, L+m+1); if(!t) return -1;
    memcpy(t+L,b,m); t[L+m]=0; n->text=t; return 0;
}

/* ================= Skippers ================= */

static int skip_comment(cur_t* c){
    if (!take(c,"<!--")) return 0;
    const char* p=c->p;
    const char* end=NULL;
    while (p<c->e){
        const char* t=strstr(p,"-->");
        if (!t){ c->p=c->e; return -1; }
        end=t; break;
    }
    c->p=end+3;
    return 1;
}
static int skip_pi(cur_t* c){
    if (!take(c,"<?")) return 0;
    const char* t=strstr(c->p,"?>"); if(!t){ c->p=c->e; return -1; }
    c->p=t+2; return 1;
}
static int skip_doctype(cur_t* c){
    if (!take(c,"<!DOCTYPE")) return 0;
    int depth=1;
    while(c->p<c->e && depth){
        if (*c->p=='<') depth++;
        else if (*c->p=='>') depth--;
        c->p++;
    }
    return 1;
}
static int take_cdata(cur_t* c, xml_node* cur){
    if (!take(c,"<![CDATA[")) return 0;
    const char* t=strstr(c->p,"]]>"); if(!t){ c->p=c->e; return -1; }
    size_t n=(size_t)(t - c->p);
    if (text_append(cur, c->p, n)!=0) return -1;
    c->p=t+3; return 1;
}
static int skip_misc(cur_t* c, xml_node* cur, int* did){
    *did=0;
    int r=0;
    if ((r=skip_comment(c))!=0){ if(r<0) return -1; *did=1; return 0; }
    if ((r=skip_pi(c))!=0){ if(r<0) return -1; *did=1; return 0; }
    if ((r=skip_doctype(c))!=0){ if(r<0) return -1; *did=1; return 0; }
    if ((r=take_cdata(c,cur))!=0){ if(r<0) return -1; *did=1; return 0; }
    return 0;
}

/* ================= Parser ================= */

static int parse_text(cur_t* c, xml_node* cur){
    const char* b=c->p;
    while(c->p<c->e && *c->p!='<') c->p++;
    size_t n=(size_t)(c->p-b);
    if (n){
        char* un=NULL; xml_unescape(b,n,&un);
        int rc=text_append(cur, un?un:"", un?strlen(un):0);
        free(un);
        return rc;
    }
    return 0;
}

static int parse_tag_open(cur_t* c, char** out_name, xml_attr** out_attrs, int* self_close){
    *out_name=NULL; *out_attrs=NULL; *self_close=0;
    skip_ws(c);
    size_t nl=span_name(c); if(!nl) return -1;
    *out_name=xstrndup0(c->p-nl,nl); if(!*out_name) return -1;

    for(;;){
        skip_ws(c);
        if (take(c,"/>")){ *self_close=1; return 0; }
        if (take(c,">")){ return 0; }
        /* attr */
        size_t kl=span_name(c); if(!kl) return -1;
        char* k=xstrndup0(c->p-kl,kl);
        skip_ws(c);
        if (!take(c,"=")){ free(k); return -1; }
        skip_ws(c);
        char* v=read_quoted(c); if(!v){ free(k); return -1; }
        xml_attr* a=(xml_attr*)xmalloc(sizeof *a);
        a->name=k; a->value=v; a->next=*out_attrs; *out_attrs=a;
    }
}

static int parse_node(cur_t* c, xml_node* parent){
    if (!take(c,"<")) return -1;
    if (starts(c,"/")) return -1; /* caller handles closing */
    char* name=NULL; xml_attr* attrs=NULL; int self_close=0;
    if (parse_tag_open(c,&name,&attrs,&self_close)!=0) return -1;

    xml_node* me=node_new(name,(size_t)-1); free(name);
    /* Reverse attr list to keep input order */
    xml_attr* rev=NULL; while(attrs){ xml_attr* n=attrs->next; attrs->next=rev; rev=attrs; attrs=n; }
    me->attrs=rev;
    node_add_child(parent, me);

    if (self_close) return 0;

    for(;;){
        int did=0;
        if (skip_misc(c,me,&did)<0) return -1;
        if (did) continue;

        if (starts(c,"</")){
            c->p+=2; skip_ws(c);
            size_t nl=span_name(c); if(!nl){ return -1; }
            skip_ws(c); if (!take(c,">")) return -1;
            return 0;
        }
        if (starts(c,"<")){
            if (parse_node(c, me)!=0) return -1;
        } else {
            if (parse_text(c, me)!=0) return -1;
        }
    }
}

XML_API xml_node* xml_load_mem(const char* buf, size_t n){
    if (!buf) return NULL;
    cur_t c = { buf, buf + (n? n : strlen(buf)) };

    xml_node* root = node_new("__doc__", (size_t)-1); if(!root) return NULL;

    skip_ws(&c);
    if (starts(&c,"<?xml")){
        const char* t=strstr(c.p,"?>"); if(!t){ xml_free(root); return NULL; }
        c.p=t+2;
    }

    for(;;){
        int did=0; if (skip_misc(&c, root,&did)<0){ xml_free(root); return NULL; }
        if (!did) break;
    }

    if (!starts(&c,"<")){ xml_free(root); return NULL; }
    if (parse_node(&c, root)!=0){ xml_free(root); return NULL; }

    /* trailing ws/misc allowed */
    for(;;){
        int did=0; skip_ws(&c);
        if (skip_misc(&c, root,&did)<0){ xml_free(root); return NULL; }
        if (!did) break;
    }
    return root;
}

XML_API xml_node* xml_load_file(const char* path){
    FILE* f=fopen(path,"rb"); if(!f) return NULL;
    if (fseek(f,0,SEEK_END)!=0){ fclose(f); return NULL; }
    long L=ftell(f); if (L<0){ fclose(f); return NULL; }
    if (fseek(f,0,SEEK_SET)!=0){ fclose(f); return NULL; }
    char* b=(char*)xmalloc((size_t)L+1); if(!b){ fclose(f); return NULL; }
    size_t r=fread(b,1,(size_t)L,f); fclose(f);
    if (r!=(size_t)L){ free(b); return NULL; }
    b[L]=0;
    xml_node* x=xml_load_mem(b,(size_t)L);
    free(b);
    return x;
}

/* ================= Query ================= */

XML_API const char* xml_attr_get(const xml_node* n, const char* key){
    if (!n||!key) return NULL;
    for (xml_attr* a=n->attrs; a; a=a->next)
        if (strieq(a->name,key)) return a->value?a->value:"";
    return NULL;
}
XML_API xml_node* xml_child(const xml_node* n, const char* name){
    if (!n) return NULL;
    for (xml_node* c=n->child; c; c=c->next)
        if (!name || (c->name && strcmp(c->name,name)==0)) return c;
    return NULL;
}
XML_API xml_node* xml_next(const xml_node* n, const char* name){
    if (!n) return NULL;
    for (xml_node* c=n->next; c; c=c->next)
        if (!name || (c->name && strcmp(c->name,name)==0)) return c;
    return NULL;
}

/* ================= Writer ================= */

static void write_indent(FILE* fp, int d){ for(int i=0;i<d;i++) fputs("  ",fp); }
static void xml_write_rec(const xml_node* x, FILE* fp, int depth){
    if (!x) return;
    if (x->name && strcmp(x->name,"__doc__")==0){
        for (const xml_node* c=x->child; c; c=c->next) xml_write_rec(c,fp,0);
        return;
    }
    write_indent(fp, depth);
    if (x->name){
        fputc('<',fp); fputs(x->name,fp);
        for (const xml_attr* a=x->attrs; a; a=a->next){
            fputc(' ',fp); fputs(a->name?a->name:"",fp); fputc('=',fp); fputc('\"',fp);
            xml_escape_fp(a->value?a->value:"", fp); fputc('\"',fp);
        }
        if ((!x->child) && (!x->text || !*x->text)){
            fputs("/>\n",fp); return;
        }
        fputc('>',fp);
        if (x->text && *x->text){ xml_escape_fp(x->text,fp); }
        if (x->child){
            fputc('\n',fp);
            for (const xml_node* c=x->child; c; c=c->next) xml_write_rec(c,fp,depth+1);
            write_indent(fp, depth);
        }
        fprintf(fp,"</%s>\n", x->name);
    } else {
        if (x->text) { xml_escape_fp(x->text,fp); fputc('\n',fp); }
    }
}
XML_API int xml_write_fp(const xml_node* x, FILE* fp){
    if (!x||!fp) return -1;
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", fp);
    xml_write_rec(x, fp, 0);
    return 0;
}
XML_API int xml_write_file(const xml_node* x, const char* path){
    FILE* f=fopen(path,"wb"); if(!f) return -1;
    int rc=xml_write_fp(x,f);
    fclose(f); return rc;
}

/* ================= Free ================= */

XML_API void xml_free(xml_node* x){
    if (!x) return;
    xml_node* ch=x->child;
    while(ch){ xml_node* nx=ch->next; xml_free(ch); ch=nx; }
    xml_attr* a=x->attrs;
    while(a){ xml_attr* na=a->next; free(a->name); free(a->value); free(a); a=na; }
    free(x->name); free(x->text); free(x);
}

/* ================= Test ================= */
#ifdef XML_TEST
int main(void){
    const char* doc =
        "<?xml version=\"1.0\"?>\n"
        "<root a=\"1\" b=\"x &amp; y\">\n"
        "  <user id=\"42\">Alice &lt;A&gt;</user>\n"
        "  <!-- comment -->\n"
        "  <![CDATA[raw <xml> & stuff]]>\n"
        "</root>";
    xml_node* x=xml_load_mem(doc, 0);
    if (!x){ puts("parse error"); return 1; }
    xml_node* root=xml_child(x,"root");
    printf("b=%s\n", xml_attr_get(root,"b"));
    xml_node* user=xml_child(root,"user");
    printf("user text=%s id=%s\n", user->text, xml_attr_get(user,"id"));
    xml_write_file(x, "out.xml");
    xml_free(x);
    return 0;
}
#endif