// SPDX-License-Identifier: GPL-3.0-or-later
//
// auxlib.c — Librairie utilitaire d’appoint pour Vitte Light (C17, portable, ultra complète)
// Namespace: "aux"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c auxlib.c
//
// Fournit :
//   - Parsing d’arguments type getopt long/short, groupes -abc, --opt=val, fin avec --
//     * Répétables, valeurs optionnelles, capture des positionnels
//     * Générateur d’aide/usage auto à partir d’un tableau de specs
//     * Sous-commandes: dispatch basé sur argv[1]
//   - Parsing "k=v,k2=v2" (kvlist) avec échappements \, \= et \,
//   - Conversion humain lisible: octets, durées; ajout aux_human_rate(bytes/sec)
//   - TTY: détection, largeur terminal, couleurs ANSI optionnelles
//   - Progress bar: ETA + débit; Spinner; masquage/affichage curseur
//   - Prompts: oui/non; saisie mot de passe masquée (POSIX/Win)
//   - Wildcard match (*, ?) sensible/insensible à la casse
//   - Tempfile path generator (unique), création optionnelle
//   - Petites tables: impression en colonnes auto (aux_table_print)
//
// Usage test :
//   cc -std=c17 -O2 -Wall -DAUX_TEST auxlib.c && ./a.out --help
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <io.h>
  #include <process.h>
  #include <conio.h>
  #define PATH_SEP '\\'
  #define isatty _isatty
  #define fileno _fileno
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/ioctl.h>
  #include <termios.h>
  #define PATH_SEP '/'
#endif

#ifndef AUX_API
#define AUX_API
#endif

/* ================== TTY / Couleurs / Term size ================== */

AUX_API int aux_isatty(FILE* f){
    return isatty(fileno(f));
}

AUX_API int aux_term_width(void){
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info))
        return (int)(info.srWindow.Right - info.srWindow.Left + 1);
    return 80;
#else
    struct winsize ws;
    if (ioctl(fileno(stdout), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
#endif
}

AUX_API void aux_ansi_show_cursor(int show){
    if (!aux_isatty(stdout)) return;
    fputs(show ? "\x1b[?25h" : "\x1b[?25l", stdout);
    fflush(stdout);
}

AUX_API void aux_ansi_color(FILE* f, int color_code){
    if (!aux_isatty(f)) return;
    if (color_code < 0) fputs("\x1b[0m", f);
    else fprintf(f, "\x1b[%dm", color_code);
}

/* ================== Args ================== */

typedef enum { AUX_ARG_FLAG=0, AUX_ARG_REQ=1, AUX_ARG_OPT=2 } aux_arg_kind;

typedef struct {
    const char* opt;     /* "v" ou "verbose" */
    aux_arg_kind kind;   /* FLAG, REQ value, OPT value */
    const char* metavar; /* affichage usage, ex: FILE */
    const char* help;    /* texte d’aide */
    int present;         /* rempli par parseur */
    const char* value;   /* rempli si REQ/OPT */
    int repeat;          /* 1 si autoriser plusieurs fois (ex: -I) */
} aux_arg;

typedef struct {
    const char* name;          /* sous-commande, ex: "init" (ou NULL pour sans sous-commande) */
    const char* brief;         /* description courte */
    aux_arg*    args;          /* tableau d’options */
    size_t      nargs;
} aux_cmd_spec;

/* Parse argv. Remplit args[]. first_nonopt: index premier positionnel.
   Retour 0/-1. Respecte --. Gère -abc, -oVAL, -o VAL, --long=VAL. */
AUX_API int aux_parse_args(int argc, char** argv, aux_arg* args, size_t nargs, int* first_nonopt);

/* Génère une aide compacte sur FILE* out. prog est argv[0] canonique. */
AUX_API void aux_print_usage(FILE* out, const char* prog, const aux_arg* args, size_t nargs, const char* extra_positional);

/* Sous-commandes: imprime la liste, ou l’aide pour une sous-commande. */
AUX_API void aux_print_cmds(FILE* out, const char* prog, const aux_cmd_spec* cmds, size_t ncmds);

/* Trouve la spec d’une sous-commande (argv[1]). Retourne index ou -1 si non trouvée. */
AUX_API int aux_find_cmd(const aux_cmd_spec* cmds, size_t ncmds, const char* name);

static aux_arg* _find_short(aux_arg* a, size_t n, char c){
    for (size_t i=0;i<n;i++) if (a[i].opt && strlen(a[i].opt)==1 && a[i].opt[0]==c) return &a[i];
    return NULL;
}
static aux_arg* _find_long(aux_arg* a, size_t n, const char* s){
    for (size_t i=0;i<n;i++) if (a[i].opt && strlen(a[i].opt)>1 && strcmp(a[i].opt,s)==0) return &a[i];
    return NULL;
}

AUX_API int aux_parse_args(int argc, char** argv, aux_arg* args, size_t nargs, int* first_nonopt){
    for (size_t z=0; z<nargs; z++){ args[z].present=0; if (!args[z].repeat) args[z].value=NULL; }
    int i=1;
    for (; i<argc; i++){
        char* t=argv[i];
        if (t[0]!='-' || !t[1]) break;
        if (strcmp(t,"--")==0){ i++; break; }
        if (t[1]!='-'){
            /* -abc  -oVAL  -o VAL */
            for (int k=1; t[k]; k++){
                aux_arg* a=_find_short(args,nargs,t[k]);
                if(!a) return -1;
                a->present++;
                if (a->kind==AUX_ARG_FLAG) continue;
                const char* v=NULL;
                if (t[k+1]){ v=&t[k+1]; k=(int)strlen(t)-1; }
                else {
                    if (i+1>=argc) { if (a->kind==AUX_ARG_OPT) { v=NULL; } else return -1; }
                    else v=argv[++i];
                }
                if (v) a->value = a->repeat && a->value ? a->value : NULL, a->value = v;
            }
        } else {
            char* eq=strchr(t+2,'=');
            if(eq){
                *eq=0; aux_arg* a=_find_long(args,nargs,t+2); *eq='=';
                if(!a) return -1;
                a->present++;
                const char* v = eq+1;
                if (a->kind==AUX_ARG_FLAG) return -1;
                a->value = v;
            } else {
                aux_arg* a=_find_long(args,nargs,t+2);
                if(!a) return -1;
                a->present++;
                if (a->kind!=AUX_ARG_FLAG){
                    if(i+1>=argc) { if (a->kind==AUX_ARG_OPT) { a->value=NULL; } else return -1; }
                    else a->value=argv[++i];
                }
            }
        }
    }
    if(first_nonopt) *first_nonopt=i;
    return 0;
}

AUX_API void aux_print_usage(FILE* out, const char* prog, const aux_arg* args, size_t nargs, const char* extra_positional){
    fprintf(out, "Usage: %s", prog?prog:"prog");
    for (size_t i=0;i<nargs;i++){
        const aux_arg* a=&args[i];
        if (!a->opt) continue;
        int is_long = strlen(a->opt)>1;
        const char* mv = a->metavar ? a->metavar : "VAL";
        if (a->kind==AUX_ARG_FLAG){
            fprintf(out, " [%s%s%s]", is_long?"--":"-", a->opt, is_long?"":"");
        } else if (a->kind==AUX_ARG_REQ){
            fprintf(out, " %s%s%s %s", is_long?"--":"-", a->opt, is_long?"":"", mv);
        } else {
            fprintf(out, " [%s%s%s %s]", is_long?"--":"-", a->opt, is_long?"":"", mv);
        }
    }
    if (extra_positional && *extra_positional) fprintf(out, " %s", extra_positional);
    fputc('\n', out);

    /* tableau d’aide */
    int w=0;
    for (size_t i=0;i<nargs;i++){
        if (!args[i].opt) continue;
        int len = (int)strlen(args[i].opt);
        if (args[i].metavar) len += 1 + (int)strlen(args[i].metavar);
        if (len > w) w = len;
    }
    if (w<10) w=10;
    for (size_t i=0;i<nargs;i++){
        const aux_arg* a=&args[i];
        if (!a->opt) continue;
        char left[128]; left[0]=0;
        if (strlen(a->opt)==1){
            if (a->kind==AUX_ARG_FLAG) snprintf(left,sizeof left,"-%s", a->opt);
            else snprintf(left,sizeof left,"-%s %s", a->opt, a->metavar?a->metavar:"VAL");
        } else {
            if (a->kind==AUX_ARG_FLAG) snprintf(left,sizeof left,"--%s", a->opt);
            else snprintf(left,sizeof left,"--%s %s", a->opt, a->metavar?a->metavar:"VAL");
        }
        fprintf(out, "  %-*s  %s\n", w, left, a->help?a->help:"");
    }
}

AUX_API void aux_print_cmds(FILE* out, const char* prog, const aux_cmd_spec* cmds, size_t ncmds){
    fprintf(out, "Usage: %s <command> [options]\n\nCommands:\n", prog?prog:"prog");
    int w=0; for(size_t i=0;i<ncmds;i++){ int len=(int)strlen(cmds[i].name?cmds[i].name:""); if(len>w) w=len; }
    for(size_t i=0;i<ncmds;i++){
        if (!cmds[i].name) continue;
        fprintf(out,"  %-*s  %s\n", w, cmds[i].name, cmds[i].brief?cmds[i].brief:"");
    }
}

AUX_API int aux_find_cmd(const aux_cmd_spec* cmds, size_t ncmds, const char* name){
    if (!name) return -1;
    for (size_t i=0;i<ncmds;i++)
        if (cmds[i].name && strcmp(cmds[i].name,name)==0) return (int)i;
    return -1;
}

/* ================== KV list (avec échappements) ================== */

typedef int (*aux_kv_cb)(const char* k,const char* v,void* u);

AUX_API int aux_parse_kvlist(const char* s, aux_kv_cb cb, void* u){
    if(!s) return 0;
    const char* p=s;
    while(*p){
        /* key */
        const char* k=p; size_t kn=0; int esc=0;
        while (p[0] && (esc || (p[0] != '=' && p[0] != ','))){
            if (!esc && p[0]=='\\') { esc=1; p++; continue; }
            esc=0; p++; kn++;
        }
        if (p[0] != '=') return -1;
        char* kb = (char*)malloc(kn+1); if(!kb) return -1;
        /* copy key with unescape */
        {
            const char* t=k; size_t j=0; int e=0;
            while (t < p){
                if (!e && *t=='\\'){ e=1; t++; continue; }
                kb[j++]=*t++; e=0;
            }
            kb[j]=0;
        }
        p++; /* skip '=' */
        /* value up to comma not escaped */
        const char* vstart = p; size_t vn=0; esc=0;
        while (p[0] && (esc || p[0]!=',')){
            if (!esc && p[0]=='\\'){ esc=1; p++; continue; }
            esc=0; p++; vn++;
        }
        char* vb=(char*)malloc(vn+1); if(!vb){ free(kb); return -1; }
        {
            const char* t=vstart; size_t j=0; int e=0;
            while (t < vstart + (ptrdiff_t)vn){
                if (!e && *t=='\\'){ e=1; t++; continue; }
                vb[j++]=*t++; e=0;
            }
            vb[j]=0;
        }
        int rc=cb(kb,vb,u);
        free(kb); free(vb);
        if (rc) return rc;
        if (p[0]==',') p++;
        while (*p==',') p++;
    }
    return 0;
}

/* ================== Human ================== */

AUX_API void aux_human_bytes(double b,char out[16]){
    const char* u[]={"","K","M","G","T","P","E"}; int i=0;
    while(b>=1024.0 && i<6){ b/=1024.0; i++; }
    if(b>=100) snprintf(out,16,"%.0f%s",b,u[i]);
    else if(b>=10) snprintf(out,16,"%.1f%s",b,u[i]);
    else snprintf(out,16,"%.2f%s",b,u[i]);
}
AUX_API void aux_human_rate(double bytes_per_sec, char out[32]){
    char s[16]; aux_human_bytes(bytes_per_sec, s);
    snprintf(out,32,"%s/s", s);
}
AUX_API void aux_human_duration(double s,char out[32]){
    if(s < 1e-3){ snprintf(out,32,"%.0fµs", s*1e6); return; }
    if(s < 1.0){ snprintf(out,32,"%.0fms", s*1e3); return; }
    long sec=(long)(s+0.5), h=sec/3600; sec%=3600; long m=sec/60; sec%=60;
    if(h) snprintf(out,32,"%ldh%02ldm",h,m);
    else if(m) snprintf(out,32,"%ldm%02lds",m,sec);
    else snprintf(out,32,"%lds",sec);
}

/* ================== Progress / Spinner ================== */

typedef struct {
    double start; size_t total,current; int width,tty; int use_color;
    double last_t; size_t last_c;
} aux_progress;

static double _now_s(void){
#if defined(_WIN32)
    static LARGE_INTEGER f={0}; LARGE_INTEGER c;
    if(!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart/(double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec*1e-9;
#endif
}

AUX_API void aux_progress_begin(aux_progress* p,size_t tot,int w){
    p->start=_now_s(); p->total=tot; p->current=0;
    p->width = (w>0) ? w : ((aux_term_width()-30>10) ? aux_term_width()-30 : 30);
    p->tty=aux_isatty(stderr);
    p->use_color = p->tty;
    p->last_t = p->start; p->last_c = 0;
    if (p->tty) aux_ansi_show_cursor(0);
}
AUX_API void aux_progress_update(aux_progress* p,size_t cur){
    p->current=cur;
    if(!p->tty) return;
    double now=_now_s();
    double frac=p->total? (double)p->current/(double)p->total:0.0;
    if(frac<0)frac=0;if(frac>1)frac=1;
    int fill=(int)(frac*p->width+0.5);

    double elapsed = now - p->start;
    double inst_dt = now - p->last_t;
    size_t inst_dc = (p->current >= p->last_c)? (p->current - p->last_c) : 0;
    double rate = inst_dt>0.001 ? (double)inst_dc/inst_dt : 0.0;
    double eta = (p->total && frac>0.0)? elapsed*(1.0/frac - 1.0) : 0.0;
    char h_eta[32], h_rate[32];
    aux_human_duration(eta, h_eta);
    aux_human_rate(rate, h_rate);

    fprintf(stderr,"\r");
    if (p->use_color) aux_ansi_color(stderr, 36);
    fputc('[', stderr);
    for(int i=0;i<p->width;i++) fputc(i<fill?'=':' ',stderr);
    fputc(']', stderr);
    if (p->use_color) aux_ansi_color(stderr, -1);
    fprintf(stderr," %3d%%  ", (int)(frac*100+0.5));
    char cur_h[16], tot_h[16];
    aux_human_bytes((double)p->current, cur_h);
    aux_human_bytes((double)p->total, tot_h);
    fprintf(stderr,"%s/%s  ETA %s  %s   ", cur_h, p->total?tot_h:"?", h_eta, h_rate);
    fflush(stderr);

    if (inst_dt >= 0.2){ p->last_t=now; p->last_c=p->current; }
}
AUX_API void aux_progress_end(aux_progress* p,const char* s){
    if(p->tty){
        aux_progress_update(p, p->total ? p->total : p->current);
        fprintf(stderr," %s\n", s?s:"");
        aux_ansi_show_cursor(1);
    } else if (s){ fprintf(stderr,"%s\n", s); }
}

/* Spinner. Appelez régulièrement. */
AUX_API void aux_spinner_step(const char* label){
    static const char* frames = "|/-\\";
    static int i=0;
    if(aux_isatty(stderr)){
        fprintf(stderr,"\r%c %s", frames[i&3], label?label:"");
        fflush(stderr);
    }
    i++;
}

/* ================== Prompts ================== */

AUX_API int aux_prompt_yn(const char* q,int def_yes){
    if(!aux_isatty(stdin)) return def_yes ? 1 : 0;
    fprintf(stderr,"%s %s ",q,def_yes?"[Y/n]":"[y/N]");
    fflush(stderr);
    char buf[8]; if(!fgets(buf,sizeof buf,stdin)) return def_yes?1:0;
    if(buf[0]=='\n'||!buf[0]) return def_yes?1:0;
    char c=(char)aux_to_lower_ascii((unsigned char)buf[0]);
    if(c=='y') return 1; if(c=='n') return 0; return def_yes?1:0;
}

/* Lecture masquée (mot de passe). Retourne longueur, -1 si erreur. */
AUX_API int aux_prompt_password(const char* prompt, char* out, size_t cap){
    if (!out || cap==0) return -1;
    fputs(prompt?prompt:"Password: ", stderr); fflush(stderr);
#if defined(_WIN32)
    size_t i=0; int ch=0;
    while ((ch=_getch())!='\r' && ch!='\n' && ch!=EOF){
        if ((ch=='\b' || ch==127) && i>0){ i--; continue; }
        if (i+1<cap){ out[i++]=(char)ch; }
    }
    out[i]=0; fputc('\n', stderr); return (int)i;
#else
    struct termios oldt, newt;
    if (tcgetattr(fileno(stdin), &oldt)!=0) return -1;
    newt = oldt; newt.c_lflag &= ~(ECHO);
    if (tcsetattr(fileno(stdin), TCSANOW, &newt)!=0) return -1;
    int ok = fgets(out, (int)cap, stdin)!=NULL;
    tcsetattr(fileno(stdin), TCSANOW, &oldt);
    fputc('\n', stderr);
    if (!ok) return -1;
    size_t n=strlen(out); if (n && out[n-1]=='\n') out[n-1]=0;
    return (int)strlen(out);
#endif
}

/* ================== Wildmatch ================== */

static inline int _aux_equ_ci(char a,char b,int ci){
    if (!ci) return a==b;
    return (int)tolower((unsigned char)a) == (int)tolower((unsigned char)b);
}
AUX_API int aux_wildmatch_ci(const char* pat,const char* str,int case_insensitive){
    const char *p=pat,*s=str,*star=NULL,*ss=NULL;
    while(*s){
        if (*p=='?' || _aux_equ_ci(*p,*s,case_insensitive)){ p++; s++; continue; }
        if (*p=='*'){ star=p++; ss=s; continue; }
        if (star){ p=star+1; s=++ss; continue; }
        return 0;
    }
    while(*p=='*') p++;
    return *p==0;
}
AUX_API int aux_wildmatch(const char* pat,const char* str){ return aux_wildmatch_ci(pat,str,0); }

/* ================== Temp files ================== */

AUX_API int aux_mktmp(char* out,size_t cap,const char* prefix,int create_file){
    if(!out||cap<8) return -1;
    const char* pre=prefix&&*prefix?prefix:"tmp";
    /* base dir */
    char dir[512];
#if defined(_WIN32)
    DWORD n=GetTempPathA((DWORD)sizeof dir, dir); if (!n) return -1;
#else
    const char* t=getenv("TMPDIR"); if(!t) t="/tmp/";
    snprintf(dir,sizeof dir,"%s",t);
    size_t dl=strlen(dir); if (dl && dir[dl-1]!='/') strncat(dir,"/",sizeof(dir)-dl-1);
#endif
    unsigned x = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)&x;
#if defined(_WIN32)
    x ^= (unsigned)GetCurrentProcessId();
#else
    x ^= (unsigned)getpid();
#endif
    for (int tries=0; tries<100; tries++, x+=0x9E3779B9u){
        char name[128];
        snprintf(name,sizeof name,"%s-%08x", pre, x);
        size_t need = strlen(dir) + strlen(name) + 1;
        if (need >= cap) return -1;
        snprintf(out,cap,"%s%s",dir,name);
#if defined(_WIN32)
        if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES){
            if (create_file){
                HANDLE h=CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
                if (h==INVALID_HANDLE_VALUE) continue;
                CloseHandle(h);
            }
            return (int)need;
        }
#else
        if (access(out, F_OK)!=0){
            if (create_file){
                FILE* f=fopen(out,"wb"); if (!f) continue; fclose(f);
            }
            return (int)need;
        }
#endif
    }
    return -1;
}

/* ================== Table util ================== */

AUX_API void aux_table_print(FILE* out, const char* const* rows, size_t nrows, int cols, int padding){
    if (!out || !rows || cols<=0) return;
    int w[16]={0}; if (cols>16) cols=16;
    for (size_t r=0;r<nrows;r++){
        for (int c=0;c<cols;c++){
            const char* cell = rows[r*cols + c] ? rows[r*cols + c] : "";
            int len = (int)strlen(cell);
            if (len > w[c]) w[c]=len;
        }
    }
    for (size_t r=0;r<nrows;r++){
        for (int c=0;c<cols;c++){
            const char* cell = rows[r*cols + c] ? rows[r*cols + c] : "";
            fprintf(out, "%-*s", w[c] + ((c+1<cols)?padding:0), cell);
        }
        fputc('\n', out);
    }
}

/* ================== Test ================== */
#ifdef AUX_TEST
static int kv_cb(const char* k,const char* v,void* u){ (void)u; printf("kv %s=%s\n",k,v); return 0; }

int main(int argc,char**argv){
    /* sous-commandes de démonstration */
    aux_arg common[] = {
        {"h",  AUX_ARG_FLAG, NULL, "afficher l'aide", 0,NULL,0},
        {"help",AUX_ARG_FLAG,NULL, "afficher l'aide", 0,NULL,0},
        {"v",  AUX_ARG_FLAG, NULL, "mode verbeux",     0,NULL,0},
        {"output",AUX_ARG_REQ,"FILE","fichier de sortie",0,NULL,0},
        {"I",  AUX_ARG_REQ, "DIR", "include path (répétable)",0,NULL,1},
        {NULL, 0,NULL,NULL,0,NULL,0}
    };
    int first=0;
    if (aux_parse_args(argc,argv,common,(sizeof common/sizeof common[0])-1,&first)!=0){
        fprintf(stderr,"Erreur d'arguments\n");
        aux_print_usage(stderr, argv[0], common, (sizeof common/sizeof common[0])-1, "[ARGS...]");
        return 1;
    }
    if (common[0].present || common[1].present){
        aux_print_usage(stdout, argv[0], common, (sizeof common/sizeof common[0])-1, "[ARGS...]");
        puts("\nExemples:\n  prog -v --output out.txt -I inc -I lib foo=1,bar\\=x\\,y");
        return 0;
    }
    printf("first_nonopt=%d\n", first);
    for (size_t i=0;i<(sizeof common/sizeof common[0])-1;i++){
        printf("--%-7s present=%d value=%s\n",
            common[i].opt?common[i].opt:"?", common[i].present, common[i].value?common[i].value:"");
    }

    /* kvlist */
    aux_parse_kvlist("a=1,b=2,c=hello\\,world,d=val\\=eq", kv_cb, NULL);

    /* human */
    char hb[16], rate[32], hd[32];
    aux_human_bytes(1536000.0, hb);
    aux_human_rate(1.25e6, rate);
    aux_human_duration(3723.0, hd);
    printf("bytes=%s rate=%s dur=%s\n", hb, rate, hd);

    /* progress */
    aux_progress p; aux_progress_begin(&p, 1000, 40);
    for (int i=0;i<=1000;i++){
#if !defined(_WIN32)
        usleep(10000);
#else
        Sleep(10);
#endif
        aux_progress_update(&p, (size_t)i);
    }
    aux_progress_end(&p, "OK");

    /* spinner */
    for (int i=0;i<24;i++){
#if !defined(_WIN32)
        usleep(50000);
#else
        Sleep(50);
#endif
        aux_spinner_step("working");
    }
    fprintf(stderr,"\n");

    /* prompt */
    int ok = aux_prompt_yn("Continuer ?", 1);
    printf("yn=%d\n", ok);
    char pw[128]; int pn = aux_prompt_password("Mot de passe: ", pw, sizeof pw);
    printf("pw_len=%d\n", pn);

    /* wildmatch */
    printf("wild cs=%d  ci=%d\n",
        aux_wildmatch("*.c","auxlib.c"),
        aux_wildmatch_ci("*.C","AUXLIB.c",1));

    /* tmp */
    char tmp[512]; int n = aux_mktmp(tmp,sizeof tmp,"aux",1);
    printf("tmp=%s n=%d\n", tmp, n);

    /* table */
    const char* rows[] = {
        "ID","Name","Desc",
        "1","alpha","first item",
        "2","beta","second item with longer text",
    };
    aux_table_print(stdout, rows, 3, 3, 2);

    return 0;
}
#endif
static int aux_to_lower_ascii(int c) {
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}
