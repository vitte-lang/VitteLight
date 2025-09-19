// SPDX-License-Identifier: GPL-3.0-or-later
//
// url.c — Utilitaires URL (parse, build, encode, decode, query) C17, portable
// Namespace: "url"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c url.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef URL_API
#define URL_API
#endif

typedef struct url_parts {
    char* scheme;
    char* user;
    char* password;
    char* host;
    int   port;
    char* path;
    char* query;
    char* fragment;
} url_parts;

/* ======================= helpers ======================= */

static char* url__strdup0(const char* s){
    if(!s){ char* z=(char*)malloc(1); if(z) z[0]=0; return z; }
    size_t n=strlen(s); char* d=(char*)malloc(n+1); if(!d) return NULL;
    memcpy(d,s,n+1); return d;
}
static char* url__substr(const char* b, const char* e){
    if (!b) return NULL; if(!e) e=b+strlen(b);
    if (e<b) e=b;
    size_t n=(size_t)(e-b);
    char* s=(char*)malloc(n+1); if(!s) return NULL;
    memcpy(s,b,n); s[n]=0; return s;
}

URL_API void url_free(url_parts* u){
    if(!u) return;
    free(u->scheme); free(u->user); free(u->password);
    free(u->host); free(u->path); free(u->query); free(u->fragment);
    memset(u,0,sizeof *u); u->port=-1;
}

/* ======================= percent encode/decode ======================= */

static int url__is_unreserved(int c){
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~';
}
static int url__hex(int c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
}

/* space_plus: 1 => ' ' devient '+'. out peut être NULL pour compter. */
URL_API size_t url_pct_encode(const unsigned char* src,size_t n,int space_plus,char* out,size_t cap){
    size_t w=0;
    for(size_t i=0;i<n;i++){
        unsigned char c=src[i];
        if (c==' ' && space_plus){
            if (out && w<cap) out[w] = '+';
            w+=1;
        } else if (url__is_unreserved((int)c)){
            if (out && w<cap) out[w] = (char)c;
            w+=1;
        } else {
            if (out && w+3<=cap){
                out[w]='%';
                static const char hx[]="0123456789ABCDEF";
                out[w+1]=hx[c>>4]; out[w+2]=hx[c&15];
            }
            w+=3;
        }
    }
    if (out && w<cap) out[w]=0;
    return w;
}

/* plus_space: 1 => '+' devient ' '. Renvoie nombre d'octets écrits hors NUL. */
URL_API size_t url_pct_decode(const char* src,int plus_space,unsigned char* out,size_t cap){
    size_t w=0;
    for(size_t i=0; src[i]; i++){
        unsigned char c=(unsigned char)src[i];
        if (plus_space && c=='+') c=' ';
        else if (c=='%' && src[i+1] && src[i+2]){
            int h=url__hex(src[i+1]), l=url__hex(src[i+2]);
            if (h>=0 && l>=0){ c=(unsigned char)((h<<4)|l); i+=2; }
        }
        if (out && w<cap) out[w]=c;
        w++;
    }
    if (out && w<cap) out[w]=0;
    return w;
}

/* ======================= parse/build ======================= */

URL_API int url_parse(const char* s, url_parts* u){
    if (!s||!u) return -1;
    memset(u,0,sizeof *u); u->port=-1;

    const char* p=s;
    const char* colon=strchr(p,':');
    const char* slash=strstr(p,"//");
    if (colon && slash && colon<slash){
        u->scheme = url__substr(p, colon);
        p = colon + 1;
    }
    if (p[0]=='/' && p[1]=='/'){
        p+=2;
        const char* auth_end = p;
        while (*auth_end && *auth_end!='/' && *auth_end!='?' && *auth_end!='#') auth_end++;

        const char* at = NULL;
        for (const char* t=p; t<auth_end; t++){ if (*t=='@'){ at=t; break; } }

        const char* host_b = p;
        if (at){
            const char* colon2 = NULL;
            for (const char* t=p; t<at; t++){ if (*t==':'){ colon2=t; break; } }
            if (colon2){
                u->user = url__substr(p, colon2);
                u->password = url__substr(colon2+1, at);
            } else {
                u->user = url__substr(p, at);
            }
            host_b = at+1;
        }

        const char* host_e = auth_end;
        int port=-1;
        if (host_b<host_e && *host_b=='['){
            const char* rb = memchr(host_b, ']', (size_t)(host_e - host_b));
            if (!rb) { url_free(u); return -1; }
            u->host = url__substr(host_b, rb+1);
            const char* pp = (rb+1<host_e && rb[1]==':') ? rb+2 : NULL;
            if (pp){
                char* tmp=url__substr(pp,host_e); if(!tmp){ url_free(u); return -1; }
                port = atoi(tmp); free(tmp);
            }
        } else {
            const char* last_colon = NULL;
            for (const char* t=host_b; t<host_e; t++) if (*t==':') last_colon=t;
            if (last_colon){
                u->host = url__substr(host_b, last_colon);
                char* tmp=url__substr(last_colon+1, host_e); if(!tmp){ url_free(u); return -1; }
                port = atoi(tmp); free(tmp);
            } else {
                u->host = url__substr(host_b, host_e);
            }
        }
        if (port>=0) u->port = port;
        p = auth_end;
    }

    const char* path_b = p;
    while (*p && *p!='?' && *p!='#') p++;
    u->path = url__substr(path_b, p);

    if (*p=='?'){
        p++;
        const char* qb = p;
        while (*p && *p!='#') p++;
        u->query = url__substr(qb, p);
    }
    if (*p=='#'){
        p++;
        u->fragment = url__strdup0(p);
    }
    return 0;
}

URL_API int url_build(const url_parts* u, char* out, size_t cap){
    if (!u||!out||cap==0) return -1;
    size_t w=0;
    #define PUT(S) do{ const char* _s=(S); if(_s){ size_t L=strlen(_s); if (w+L>=cap) return -1; memcpy(out+w,_s,L); w+=L; } }while(0)
    #define PUTC(C) do{ if (w+1>=cap) return -1; out[w++]=(char)(C); }while(0)

    if (u->scheme && *u->scheme){ PUT(u->scheme); PUTC(':'); }
    if (u->host || u->user || u->password){
        PUT("//");
        if (u->user && *u->user){
            PUT(u->user);
            if (u->password && *u->password){ PUTC(':'); PUT(u->password); }
            PUTC('@');
        }
        if (u->host) PUT(u->host);
        if (u->port>=0){ char buf[16]; int n=snprintf(buf,sizeof buf,":%d",u->port); if (n<0) return -1; PUT(buf); }
    }
    if (u->path){ if (*u->path && *u->path!='/') PUTC('/'); PUT(u->path); }
    if (u->query && *u->query){ PUTC('?'); PUT(u->query); }
    if (u->fragment && *u->fragment){ PUTC('#'); PUT(u->fragment); }
    if (w>=cap) return -1;
    out[w]=0; return (int)w;
    #undef PUT
    #undef PUTC
}

/* ======================= query helpers ======================= */

typedef int (*url_qs_cb)(const char* k,const char* v,void* u);

URL_API int url_qs_each(const char* query, url_qs_cb cb, void* u){
    if (!cb) return -1;
    if (!query || !*query) return 0;
    const char* p=query;
    while (*p){
        const char* k=p; const char* v=NULL; const char* e=p;
        while (*e && *e!='&') e++;
        const char* eq=k;
        for (; eq<e; eq++){ if (*eq=='='){ v=eq+1; break; } }
        size_t kn = (size_t)((v?eq:e)-k);
        size_t vn = v? (size_t)(e-v) : 0;

        char* kd=(char*)malloc(kn+1); char* vd=(char*)malloc(vn+1);
        if (!kd || (!vd && vn>0)){ free(kd); free(vd); return -1; }
        memcpy(kd,k,kn); kd[kn]=0;
        if (vn){ memcpy(vd,v,vn); vd[vn]=0; } else { if(vd) vd[0]=0; }

        size_t kmax = kn+1, vmax = vn+1;
        unsigned char* kbuf=(unsigned char*)malloc(kmax);
        unsigned char* vbuf=(unsigned char*)malloc(vmax?vmax:1);
        if (!kbuf || (!vbuf && vmax)){ free(kd); free(vd); free(kbuf); free(vbuf); return -1; }
        url_pct_decode(kd,1,kbuf,kmax);
        if (vn) url_pct_decode(vd,1,vbuf,vmax); else if (vbuf) vbuf[0]=0;

        int rc = cb((char*)kbuf, (char*)(vbuf?vbuf:(unsigned char*)""), u);

        free(kd); free(vd); free(kbuf); free(vbuf);
        if (rc) return rc;

        p = *e? e+1 : e;
        while (*p=='&') p++;
    }
    return 0;
}

/* C17: pas de fonctions imbriquées. Implémentation sans callback interne. */
URL_API int url_qs_get(const char* query, const char* key, char* out, size_t cap){
    if (!key || !out || cap==0) return -1;
    out[0]=0;
    if (!query || !*query) return 1;

    const char* p=query;
    while (*p){
        const char* e=p; while (*e && *e!='&') e++;
        const char* eq=p; const char* vptr=NULL;
        for (; eq<e; eq++){ if (*eq=='='){ vptr=eq+1; break; } }

        size_t kn = (size_t)((vptr?eq:e)-p);
        size_t vn = vptr? (size_t)(e-vptr) : 0;

        /* decode key */
        unsigned char* kbuf=(unsigned char*)malloc(kn+1);
        if(!kbuf) return -1;
        memcpy(kbuf,p,kn); kbuf[kn]=0;
        url_pct_decode((char*)kbuf,1,kbuf,kn+1);

        int match = (strcmp((char*)kbuf, key)==0);
        free(kbuf);

        if (match){
            /* decode value */
            if (!vptr){ /* key without value */
                if (cap<1) return -1;
                out[0]=0;
                return 0;
            }
            unsigned char* vbuf=(unsigned char*)malloc(vn+1);
            if(!vbuf) return -1;
            memcpy(vbuf,vptr,vn); vbuf[vn]=0;
            url_pct_decode((char*)vbuf,1,vbuf,vn+1);
            size_t L=strlen((char*)vbuf);
            if (L>=cap){ free(vbuf); return -1; }
            memcpy(out,vbuf,L+1);
            free(vbuf);
            return 0;
        }

        p = *e? e+1 : e;
        while (*p=='&') p++;
    }
    return 1; /* absent */
}

/* Ajoute ou met à jour key=val. *query est realloc'd. key/val non encodés (on encode). */
URL_API int url_qs_set(char** query, const char* key, const char* val){
    if (!query || !key) return -1;
    const char* q = *query ? *query : "";
    size_t klen = url_pct_encode((const unsigned char*)key, strlen(key), 1, NULL, 0);
    size_t vlen = val? url_pct_encode((const unsigned char*)val, strlen(val), 1, NULL, 0) : 0;
    char* kenc=(char*)malloc(klen+1); if(!kenc) return -1;
    char* venc=NULL; if (val){ venc=(char*)malloc(vlen+1); if(!venc){ free(kenc); return -1; } }
    url_pct_encode((const unsigned char*)key, strlen(key), 1, kenc, klen+1);
    if (val) url_pct_encode((const unsigned char*)val, strlen(val), 1, venc, vlen+1);

    size_t qlen = strlen(q);
    size_t newcap = qlen + klen + 1 + (val?1+vlen:0) + 2;
    char* out = (char*)malloc(newcap); if(!out){ free(kenc); free(venc); return -1; }
    out[0]=0;

    int replaced=0;
    const char* p=q;
    while (*p){
        const char* e=p; while (*e && *e!='&') e++;
        const char* eq=p; const char* vptr=NULL;
        for (; eq<e; eq++){ if (*eq=='='){ vptr=eq+1; break; } }
        size_t kn=(size_t)((vptr?eq:e)-p);
        if (kn==klen && strncmp(p,kenc,klen)==0){
            if (*out) strcat(out,"&");
            strcat(out,kenc);
            if (val){ strcat(out,"="); strcat(out,venc); }
            replaced=1;
        } else {
            if (*out) strcat(out,"&");
            strncat(out,p,(size_t)(e-p));
        }
        p = *e? e+1 : e;
        while (*p=='&') p++;
    }
    if (!replaced){
        if (*out && out[strlen(out)-1]!='&') strcat(out,"&");
        strcat(out,kenc);
        if (val){ strcat(out,"="); strcat(out,venc); }
    }
    free(kenc); free(venc);
    free(*query);
    *query = out;
    return 0;
}

/* ======================= Tests ======================= */
#ifdef URL_TEST
static int print_kv(const char* k,const char* v,void* u){
    (void)u; printf("k=\"%s\" v=\"%s\"\n", k, v); return 0;
}
int main(void){
    const char* s="https://user:pa%73s@[2001:db8::1]:8443/a/b/../c?x=1&y=hello+world#frag";
    url_parts u; if (url_parse(s,&u)!=0){ puts("parse error"); return 1; }
    printf("scheme=%s user=%s pass=%s host=%s port=%d path=%s query=%s frag=%s\n",
        u.scheme?u.scheme:"", u.user?u.user:"", u.password?u.password:"",
        u.host?u.host:"", u.port, u.path?u.path:"", u.query?u.query:"", u.fragment?u.fragment:"");

    char buf[512]; int n=url_build(&u,buf,sizeof buf);
    printf("build(%d) = %s\n", n, buf);

    puts("-- query each");
    url_qs_each(u.query, print_kv, NULL);

    char v[64]; int grc=url_qs_get(u.query,"y",v,sizeof v);
    printf("get y -> rc=%d val=\"%s\"\n", grc, grc==0?v:"");

    char* q2 = u.query ? url__strdup0(u.query) : url__strdup0("");
    url_qs_set(&q2,"y","bye bye");
    url_qs_set(&q2,"z","ok");
    printf("set -> %s\n", q2);

    const char* raw="a b/©";
    char enc[128]; url_pct_encode((const unsigned char*)raw, strlen(raw), 1, enc, sizeof enc);
    printf("enc: %s\n", enc);
    unsigned char dec[128]; url_pct_decode(enc,1,dec,sizeof dec);
    printf("dec: %s\n", dec);

    free(q2);
    url_free(&u);
    return 0;
}
#endif