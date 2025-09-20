// SPDX-License-Identifier: GPL-3.0-or-later
//
// baselib.c — Fondations C17 portables pour Vitte Light (ultra complet)
// Namespace: "base"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c baselib.c
//
// Couverture:
//   - Types & utils: align, bswap, clamp, min/max, memzero, hexdump
//   - String View (sv): trim/find/split/starts/ends/to_{u64,i64,double}
//   - SmallBuf (sb_*) : tableau dynamique type-safe (macro, sans header externe)
//   - Arena: alloc alignée, mark/reset, strdup
//   - HashMap string→void*: put/get/del, foreach, fnv1a64
//   - Fichiers: read_all/write_all/exists/size/remove/copy/move
//   - Dossiers: mkdir_p, listdir (callback), rm_rf (récursif)
//   - Chemins: join, dirname, basename, normalize (./, //, .. simplifiés)
//   - Temps: now_ns, sleep_ms, cpu_time_ns, iso8601_utc
//   - RNG: xoroshiro128**, rng_bytes, rng_range
//   - UUID v4
//   - Environnement: env_get, home_dir, temp_dir, exe_path
//   - Logging: niveaux, couleurs ANSI auto, redirection FILE*
//   - Process (léger): run_capture(cmd, out*, outlen*) POSIX/popen, Windows/_popen
//
// Notes:
//   - Sans dépendances externes. Windows + POSIX pris en charge.
//   - Tout API marquée BASE_API.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <time.h>
#include <stdarg.h>

#if defined(_WIN32)
  #define NOMINMAX
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <Shlwapi.h>
  #pragma comment(lib, "Shlwapi.lib")
  #define PATH_SEP '\\'
  #define access _access
  #define F_OK 0
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/time.h>
  #include <dirent.h>
  #include <limits.h>
  #define PATH_SEP '/'
#endif

/* ======================= Types & utilitaires ======================= */

#define BASE_API /* public */

typedef  uint8_t  u8;  typedef  int8_t  i8;
typedef uint16_t u16;  typedef int16_t i16;
typedef uint32_t u32;  typedef int32_t i32;
typedef uint64_t u64;  typedef int64_t i64;

#ifndef countof
#define countof(a) ((sizeof(a)/sizeof(0[a])) / ((size_t)(!(sizeof(a)%sizeof(0[a])))))
#endif

#define align_up(x,a)   ( ((x) + ((a)-1)) & ~((a)-1) )
#define align_down(x,a) ( (x) & ~((a)-1) )
#define min(a,b)        ((a)<(b)?(a):(b))
#define max(a,b)        ((a)>(b)?(a):(b))
#define clamp(x,lo,hi)  ( (x)<(lo)?(lo):((x)>(hi)?(hi):(x)) )

static inline u16 bswap16(u16 v){ return (u16)((v>>8)|(v<<8)); }
static inline u32 bswap32(u32 v){ return (v>>24)|((v>>8)&0x0000FF00u)|((v<<8)&0x00FF0000u)|(v<<24); }
static inline u64 bswap64(u64 v){ return ((u64)bswap32((u32)v)<<32) | bswap32((u32)(v>>32)); }

static inline void memzero(void* p, size_t n){ memset(p, 0, n); }

/* hexdump compact: 16 bytes/ligne */
BASE_API void hexdump(const void* buf, size_t n, FILE* out){
    const u8* p = (const u8*)buf; if (!out) out = stdout;
    for (size_t i=0;i<n;i+=16){
        fprintf(out, "%08zx  ", i);
        for (size_t j=0;j<16;j++){
            if (i+j<n) fprintf(out, "%02x ", p[i+j]); else fputs("   ", out);
            if (j==7) fputc(' ', out);
        }
        fputs(" |", out);
        for (size_t j=0;j<16 && i+j<n;j++){
            unsigned c = p[i+j]; fputc((c>=32 && c<127)? c : '.', out);
        }
        fputs("|\n", out);
    }
}

/* ======================= String View (sv) ======================= */

typedef struct { const char* p; size_t n; } sv;

static inline sv sv_from(const char* s){ return (sv){ s, s?strlen(s):0 }; }
static inline sv sv_make(const char* p,size_t n){ return (sv){p,n}; }
static inline int sv_eq(sv a, sv b){ return a.n==b.n && (a.n==0 || memcmp(a.p,b.p,a.n)==0); }

static sv sv_ltrim(sv s){ size_t i=0; while(i<s.n && (unsigned char)s.p[i]<=0x20) i++; return (sv){s.p+i,s.n-i}; }
static sv sv_rtrim(sv s){ size_t n=s.n; while(n && (unsigned char)s.p[n-1]<=0x20) n--; return (sv){s.p,n}; }
static sv sv_trim(sv s){ return sv_rtrim(sv_ltrim(s)); }

static long sv_find(sv s, sv needle){
    if (needle.n==0) return 0; if (needle.n>s.n) return -1;
    const char* end = s.p + s.n - needle.n + 1;
    for (const char* p=s.p; p<end; p++){
        if (*p==*needle.p && memcmp(p,needle.p,needle.n)==0) return (long)(p - s.p);
    }
    return -1;
}
static int sv_starts(sv s, sv pre){ return s.n>=pre.n && memcmp(s.p,pre.p,pre.n)==0; }
static int sv_ends  (sv s, sv suf){ return s.n>=suf.n && memcmp(s.p+s.n-suf.n, suf.p, suf.n)==0; }

static int sv_split_once(sv s, char sep, sv* left, sv* right){
    for (size_t i=0;i<s.n;i++) if (s.p[i]==sep){
        if(left) *left=(sv){s.p,i};
        if(right)*right=(sv){s.p+i+1,s.n-i-1};
        return 1;
    }
    if(left) *left=s; if(right) right->p=NULL,right->n=0; return 0;
}

static int sv_to_u64(sv s, u64* out){
    s = sv_trim(s); if (!s.n) return -1;
    char buf[64]; size_t m=min(s.n, sizeof(buf)-1); memcpy(buf,s.p,m); buf[m]=0;
    char* e=NULL; errno=0; unsigned long long v=strtoull(buf,&e,0);
    if (errno || e==buf) return -1; *out=(u64)v; return 0;
}
static int sv_to_i64(sv s, i64* out){
    s = sv_trim(s); if (!s.n) return -1;
    char buf[64]; size_t m=min(s.n, sizeof(buf)-1); memcpy(buf,s.p,m); buf[m]=0;
    char* e=NULL; errno=0; long long v=strtoll(buf,&e,0);
    if (errno || e==buf) return -1; *out=(i64)v; return 0;
}
static int sv_to_double(sv s, double* out){
    s = sv_trim(s); if (!s.n) return -1;
    char buf[64]; size_t m=min(s.n, sizeof(buf)-1); memcpy(buf,s.p,m); buf[m]=0;
    char* e=NULL; errno=0; double v=strtod(buf,&e);
    if (errno || e==buf) return -1; *out=v; return 0;
}

/* ======================= Arena Allocator ======================= */

typedef struct { u8* buf; size_t len, cap; } arena;

BASE_API void  arena_init(arena* a, size_t reserve){
    a->len=0; a->cap = reserve? reserve : 64*1024;
    a->buf = (u8*)malloc(a->cap); if (!a->buf){ perror("arena"); exit(1); }
}
BASE_API void  arena_free(arena* a){ free(a->buf); a->buf=NULL; a->len=a->cap=0; }
BASE_API size_t arena_mark(const arena* a){ return a->len; }
BASE_API void  arena_reset(arena* a, size_t mark){ if (mark<=a->len) a->len=mark; }
static int  arena_grow(arena* a, size_t need){
    if (need <= a->cap) return 0; size_t ncap=a->cap; while(ncap<need){ if(ncap>SIZE_MAX/2){ncap=need;break;} ncap*=2; }
    void* p=realloc(a->buf,ncap); if(!p) return -1; a->buf=(u8*)p; a->cap=ncap; return 0;
}
BASE_API void* arena_alloc(arena* a, size_t size, size_t align){
    size_t off = align? align_up(a->len, align) : a->len;
    if (size > SIZE_MAX - off) return NULL; size_t end = off + size;
    if (arena_grow(a,end)) return NULL; void* p=a->buf+off; a->len=end; return p;
}
BASE_API char* arena_strndup(arena* a, const char* s, size_t n){
    char* p=(char*)arena_alloc(a,n+1,1); if(!p) return NULL; memcpy(p,s,n); p[n]=0; return p;
}
BASE_API char* arena_svdup(arena* a, sv s){ return arena_strndup(a, s.p, s.n); }

/* ======================= Stretchy Buffer (sb_*) ======================= */

typedef struct { size_t len, cap; } _sb_hdr;
#define _sb_hdr(a)   ((_sb_hdr*)((char*)(a) - sizeof(_sb_hdr)))
#define sb_len(a)    ((a)? _sb_hdr(a)->len : 0u)
#define sb_cap(a)    ((a)? _sb_hdr(a)->cap : 0u)
#define sb_free(a)   ((a)? (free(_sb_hdr(a)), (a)=NULL) : (void)0)
#define sb_fit(a,n)  ( (n) <= sb_cap(a) ? 0 : ((a)=_sb_grow((a),(n),sizeof(*(a))),0) )
#define sb_push(a,v) ( sb_fit((a), sb_len(a)+1), (a)[_sb_hdr(a)->len++] = (v) )

static void* _sb_grow(void* a, size_t n, size_t elemsz){
    size_t cap = sb_cap(a); size_t nc = cap ? cap*2 : 8; if (nc < n) nc = n;
    size_t bytes = sizeof(_sb_hdr) + nc*elemsz;
    _sb_hdr* h = a ? _sb_hdr(a) : NULL;
    h = (_sb_hdr*)realloc(h, bytes); if (!h){ perror("sb"); exit(1); }
    if (!a) h->len = 0; h->cap = nc;
    return (char*)h + sizeof(_sb_hdr);
}

/* ======================= HashMap string → void* ======================= */

typedef struct { char* key; void* val; u64 hash; bool used; } hentry;
typedef struct { hentry* tab; size_t n, cap; } hmap;

static u64 fnv1a64(const void* p, size_t n){
    const u8* s=(const u8*)p; u64 h=1469598103934665603ull;
    for (size_t i=0;i<n;i++){ h ^= s[i]; h *= 1099511628211ull; } return h;
}
static size_t _hmap_probe(u64 h, size_t i, size_t cap){ return ( (h + i + i*i) & (cap-1) ); } // quad
static int hmap_rehash(hmap* m, size_t ncap){
    hentry* old=m->tab; size_t oc=m->cap;
    m->tab=(hentry*)calloc(ncap,sizeof(hentry)); if(!m->tab) return -1;
    m->cap=ncap; m->n=0;
    for(size_t i=0;i<oc;i++) if (old[i].used){
        size_t j=0, idx; for(;;){ idx=_hmap_probe(old[i].hash,j++,ncap);
            if(!m->tab[idx].used){ m->tab[idx]=old[i]; m->tab[idx].used=true; m->n++; break; }
        }
    }
    free(old); return 0;
}
BASE_API void hmap_init(hmap* m){ m->tab=NULL; m->n=0; m->cap=0; }
BASE_API void hmap_free(hmap* m){
    if (!m->tab) return;
    for (size_t i=0;i<m->cap;i++) if (m->tab[i].used) free(m->tab[i].key);
    free(m->tab); m->tab=NULL; m->n=m->cap=0;
}
BASE_API int hmap_put(hmap* m, sv key, void* val){
    if (m->cap==0) if (hmap_rehash(m, 16)) return -1;
    if ((m->n+1)*4 >= m->cap*3) if (hmap_rehash(m, m->cap*2)) return -1; // LF ~0.75
    u64 h = fnv1a64(key.p, key.n); size_t j=0, idx;
    for(;;){
        idx = _hmap_probe(h, j++, m->cap); hentry* e=&m->tab[idx];
        if (!e->used){
            e->key = (char*)malloc(key.n+1); if(!e->key) return -1; memcpy(e->key,key.p,key.n); e->key[key.n]=0;
            e->val=val; e->hash=h; e->used=true; m->n++; return 0;
        }
        if (e->hash==h && strncmp(e->key, key.p, key.n)==0 && e->key[key.n]==0){ e->val=val; return 0; }
    }
}
BASE_API void* hmap_get(const hmap* m, sv key){
    if (!m->tab) return NULL; u64 h=fnv1a64(key.p,key.n); size_t j=0, idx;
    for(;;){
        idx=_hmap_probe(h,j++,m->cap); const hentry* e=&m->tab[idx];
        if (!e->used) return NULL;
        if (e->hash==h && strncmp(e->key,key.p,key.n)==0 && e->key[key.n]==0) return e->val;
    }
}
BASE_API int hmap_del(hmap* m, sv key){
    if (!m->tab) return -1; u64 h=fnv1a64(key.p,key.n); size_t j=0, idx;
    for(;;){
        idx=_hmap_probe(h,j++,m->cap); hentry* e=&m->tab[idx];
        if (!e->used) return -1;
        if (e->hash==h && strncmp(e->key,key.p,key.n)==0 && e->key[key.n]==0){
            free(e->key); e->key=NULL; e->val=NULL; e->hash=0; e->used=false;
            size_t k = (idx+1)&(m->cap-1);
            while (m->tab[k].used){
                hentry tmp=m->tab[k]; m->tab[k].used=false;
                size_t j2=0, i2;
                for(;;){
                    i2=_hmap_probe(tmp.hash,j2++,m->cap);
                    if (!m->tab[i2].used){ m->tab[i2]=tmp; break; }
                }
                k=(k+1)&(m->cap-1);
            }
            m->n--; return 0;
        }
    }
}
#define hmap_foreach(m, var_e) for(size_t _i=0; _i<(m).cap; _i++) if ((m).tab && (m).tab[_i].used) for(hentry* var_e=&(m).tab[_i]; var_e; var_e=NULL)

/* ======================= Fichiers & Chemins ======================= */

BASE_API int file_exists(const char* path){ return access(path, F_OK)==0; }

BASE_API int file_size(const char* path, long long* out){
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    ULARGE_INTEGER u; u.HighPart=fad.nFileSizeHigh; u.LowPart=fad.nFileSizeLow; *out=(long long)u.QuadPart; return 0;
#else
    struct stat st; if (stat(path,&st)) return -1; *out=(long long)st.st_size; return 0;
#endif
}
BASE_API int file_remove(const char* path){
#if defined(_WIN32)
    DWORD attr=GetFileAttributesA(path);
    if (attr!=INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return _rmdir(path);
    return remove(path);
#else
    return remove(path);
#endif
}
BASE_API int file_copy(const char* src, const char* dst){
    FILE* in=fopen(src,"rb"); if(!in) return -1;
    FILE* out=fopen(dst,"wb"); if(!out){ fclose(in); return -1; }
    u8 buf[1<<15]; size_t r; int ok=0;
    while ((r=fread(buf,1,sizeof buf,in))>0){ if (fwrite(buf,1,r,out)!=r){ ok=-1; break; } }
    if (ferror(in)) ok=-1;
    fclose(in); fclose(out); return ok;
}
BASE_API int file_move(const char* src, const char* dst){
#if defined(_WIN32)
    if (MoveFileExA(src,dst,MOVEFILE_REPLACE_EXISTING)) return 0;
    if (GetLastError()==ERROR_NOT_SAME_DEVICE){ if (file_copy(src,dst)==0){ file_remove(src); return 0; } }
    return -1;
#else
    if (rename(src,dst)==0) return 0;
    if (errno==EXDEV){ if (file_copy(src,dst)==0){ file_remove(src); return 0; } }
    return -1;
#endif
}

BASE_API int read_all(const char* path, void** out, size_t* outlen){
    FILE* f = fopen(path,"rb"); if(!f) return -1;
    fseek(f,0,SEEK_END); long n = ftell(f); if (n<0){ fclose(f); return -1; }
    rewind(f);
    void* buf = malloc((size_t)n+1); if(!buf){ fclose(f); return -1; }
    size_t r = fread(buf,1,(size_t)n,f); ((u8*)buf)[r]=0; fclose(f);
    *out=buf; if(outlen)*outlen=r; return 0;
}
BASE_API int write_all(const char* path, const void* buf, size_t n){
    FILE* f = fopen(path,"wb"); if(!f) return -1;
    int ok = (fwrite(buf,1,n,f)==n)?0:-1; fclose(f); return ok;
}

/* Chemins */
BASE_API size_t path_join(char* out, size_t cap, const char* a, const char* b){
    size_t na=strlen(a), nb=strlen(b);
    int need_sep = na && a[na-1]!=PATH_SEP;
    size_t n = na + (need_sep?1:0) + nb;
    if (cap){ size_t i=0;
        for (; i<na && i<cap-1; i++) out[i]=a[i];
        if (need_sep && i<cap-1) out[i++]=PATH_SEP;
        for (size_t j=0; j<nb && i<cap-1; j++) out[i++]=b[j];
        out[i]=0;
    }
    return n;
}
BASE_API sv path_dirname(sv p){
    if (!p.n) return sv_make("",0);
    size_t i=p.n; while(i && p.p[i-1]!=PATH_SEP) i--;
    if (i==0) return sv_make("",0);
    return sv_make(p.p, i-1);
}
BASE_API sv path_basename(sv p){
    size_t i=p.n; while(i && p.p[i-1]!=PATH_SEP) i--;
    return sv_make(p.p+i, p.n-i);
}

/* Normalize partielle: supprime ./, //, résout .. simple */
BASE_API char* path_normalize(const char* in, char* out, size_t cap){
    if (!out || cap==0) return NULL;
    size_t n=strlen(in); size_t w=0;
    for (size_t i=0;i<=n;i++){
        char c = in[i]; if (c=='\\' || c=='/') c=PATH_SEP;
        out[w++] = c;
        if (w==cap){ out[cap-1]=0; return out; }
    }
    // collapse //
    size_t r=0; w=0;
    while (out[r]){
        out[w++] = out[r];
        if (out[r]==PATH_SEP){ while(out[r+1]==PATH_SEP) r++; }
        r++;
    }
    out[w]=0;
    // remove /./
    char* s=out;
    for (;;){
        char* p=strstr(s, "/./"); if(!p) break;
        memmove(p, p+2, strlen(p+2)+1); s=p;
    }
    // resolve /a/../
    for(;;){
        char* p=strstr(out, "/../"); if(!p) break;
        char* q=p; if (q==out) { memmove(out, p+3, strlen(p+3)+1); continue; }
        q--; while(q>out && *q!=PATH_SEP) q--;
        memmove(q, p+3, strlen(p+3)+1);
    }
    return out;
}

/* Dossiers */
BASE_API int mkdir_p(const char* path){
    if (!path || !*path) return -1;
    char tmp[4096]; size_t n=strlen(path); if (n>=sizeof tmp) return -1;
    memcpy(tmp,path,n+1);
    for (size_t i=1;i<n;i++){
        if (tmp[i]==PATH_SEP){
            tmp[i]=0;
#if defined(_WIN32)
            if (tmp[0] && GetFileAttributesA(tmp)==INVALID_FILE_ATTRIBUTES){ if (_mkdir(tmp)!=0 && errno!=EEXIST) return -1; }
#else
            struct stat st; if (stat(tmp,&st)!=0){ if (mkdir(tmp, 0755)!=0 && errno!=EEXIST) return -1; }
#endif
            tmp[i]=PATH_SEP;
        }
    }
#if defined(_WIN32)
    if (_mkdir(tmp)!=0 && errno!=EEXIST) return -1;
#else
    if (mkdir(tmp,0755)!=0 && errno!=EEXIST) return -1;
#endif
    return 0;
}

typedef int (*listdir_cb)(const char* path, const char* name, int is_dir, void* user);
BASE_API int listdir(const char* dir, listdir_cb cb, void* user){
#if defined(_WIN32)
    char pat[4096]; snprintf(pat,sizeof pat,"%s\\*",&dir[0]?dir:"");
    WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA(pat,&fd);
    if (h==INVALID_HANDLE_VALUE) return -1;
    int rc=0;
    do{
        const char* name=fd.cFileName;
        if (strcmp(name,".")==0 || strcmp(name,"..")==0) continue;
        int isd = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)?1:0;
        rc = cb(dir, name, isd, user);
        if (rc) break;
    }while(FindNextFileA(h,&fd));
    FindClose(h); return rc;
#else
    DIR* d = opendir(dir); if(!d) return -1;
    struct dirent* ent; int rc=0;
    while ((ent=readdir(d))){
        const char* name=ent->d_name;
        if (strcmp(name,".")==0 || strcmp(name,"..")==0) continue;
        int isd = (ent->d_type==DT_DIR) ? 1 : 0;
        rc = cb(dir, name, isd, user);
        if (rc) break;
    }
    closedir(d); return rc;
#endif
}

/* rm -rf (simple) */
static int _rmrf_cb(const char* dir, const char* name, int is_dir, void* user){
    char path[4096]; (void)user;
    size_t n = path_join(path,sizeof path,dir,name);
    (void)n;
    if (is_dir){
        rm_rf(path);
    } else {
        file_remove(path);
    }
    return 0;
}
BASE_API int rm_rf(const char* path){
#if defined(_WIN32)
    DWORD attr=GetFileAttributesA(path);
    if (attr==INVALID_FILE_ATTRIBUTES) return 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY){
        listdir(path, _rmrf_cb, NULL);
        return _rmdir(path);
    } else {
        return remove(path);
    }
#else
    struct stat st; if (lstat(path,&st)!=0) return 0;
    if (S_ISDIR(st.st_mode)){
        listdir(path, _rmrf_cb, NULL);
        return rmdir(path);
    } else {
        return remove(path);
    }
#endif
}

/* ======================= Temps & RNG ======================= */

BASE_API u64 now_ns(void){
#if defined(_WIN32)
    static LARGE_INTEGER f={0}; LARGE_INTEGER c;
    if (!f.QuadPart){ QueryPerformanceFrequency(&f); }
    QueryPerformanceCounter(&c);
    return (u64)((c.QuadPart*1000000000ull)/f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec*1000000000ull + (u64)ts.tv_nsec;
#endif
}
BASE_API void sleep_ms(u32 ms){
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { ms/1000u, (long)(ms%1000u)*1000000L };
    nanosleep(&ts,NULL);
#endif
}
BASE_API u64 cpu_time_ns(void){
#if defined(_WIN32)
    return now_ns();
#else
    struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (u64)ts.tv_sec*1000000000ull + (u64)ts.tv_nsec;
#endif
}
BASE_API void iso8601_utc(char out[25]){
    time_t t = time(NULL); struct tm tm; 
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    snprintf(out,25,"%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* xoroshiro128** */
typedef struct { u64 s[2]; } base_rng;
static inline u64 rotl64(u64 x, int k){ return (x<<k) | (x>>(64-k)); }
static u64 rng_next(base_rng* r){
    u64 s0=r->s[0], s1=r->s[1];
    u64 res = rotl64(s0 * 5u, 7) * 9u;
    s1 ^= s0; r->s[0] = rotl64(s0, 24) ^ s1 ^ (s1<<16); r->s[1] = rotl64(s1, 37);
    return res;
}
static u64 splitmix64(u64* x){ u64 z = (*x += 0x9E3779B97F4A7C15ull); z=(z^(z>>30))*0xBF58476D1CE4E5B9ull; z=(z^(z>>27))*0x94D049BB133111EBull; return z^(z>>31); }

#if defined(_MSC_VER)
__declspec(thread) static base_rng g_rng;
#else
__thread static base_rng g_rng;
#endif

BASE_API void rng_seed(u64 a, u64 b){
    if (a==0 && b==0){ a=0x1234567812345678ull; b=0x9ABCDEF09ABCDEF0ull; }
    g_rng.s[0]=a? a : 1; g_rng.s[1]=b? b : 2;
}
BASE_API void rng_seed_time(void){
    u64 t = now_ns(); u64 x=(u64)(uintptr_t)&t ^ t; u64 sm=0xA5A5A5A5A5A5A5A5ull ^ t;
    rng_seed(splitmix64(&x), splitmix64(&sm));
}
BASE_API u64 rng_u64(void){ return rng_next(&g_rng); }
BASE_API void rng_bytes(void* out, size_t n){
    u8* p=(u8*)out; size_t i=0; while (i<n){ u64 x=rng_u64(); size_t k=min((size_t)8, n-i); memcpy(p+i,&x,k); i+=k; }
}
BASE_API u64 rng_range(u64 lo, u64 hi){ if (hi<=lo) return lo; u64 d=hi-lo; u64 x=rng_u64(); return lo + (u64)((__uint128_t)x * (d+1) >> 64); }
BASE_API void uuid_v4(char out[36]){
    u8 b[16]; rng_bytes(b,16);
    b[6]=(b[6]&0x0F)|0x40; b[8]=(b[8]&0x3F)|0x80;
    static const char h[]="0123456789abcdef"; int j=0;
    for(int i=0;i<16;i++){ if (i==4||i==6||i==8||i==10) out[j++]='-'; out[j++]=h[b[i]>>4]; out[j++]=h[b[i]&15]; }
}

/* ======================= Environnement & chemins spéciaux ======================= */

BASE_API const char* env_get(const char* key){ return getenv(key); }

BASE_API size_t home_dir(char* out, size_t cap){
#if defined(_WIN32)
    const char* u = getenv("USERPROFILE"); if (!u) u = getenv("HOMEDRIVE");
    if (!u){ if (cap) out[0]=0; return 0; }
    size_t n=strlen(u); if (cap){ size_t m=min(n,cap-1); memcpy(out,u,m); out[m]=0; } return n;
#else
    const char* h = getenv("HOME");
    if (!h){ if (cap) out[0]=0; return 0; }
    size_t n=strlen(h); if (cap){ size_t m=min(n,cap-1); memcpy(out,h,m); out[m]=0; } return n;
#endif
}
BASE_API size_t temp_dir(char* out, size_t cap){
#if defined(_WIN32)
    DWORD n=GetTempPathA((DWORD)cap, out); if (n==0) { if(cap) out[0]=0; return 0; } return (size_t)n;
#else
    const char* t = getenv("TMPDIR"); if (!t) t="/tmp";
    size_t n=strlen(t); if (cap){ size_t m=min(n,cap-1); memcpy(out,t,m); out[m]=0; } return n;
#endif
}
BASE_API size_t exe_path(char* out, size_t cap){
#if defined(_WIN32)
    DWORD n=GetModuleFileNameA(NULL,out,(DWORD)cap); if(n==0){ if(cap) out[0]=0; return 0; } return (size_t)n;
#else
    char link[64]; snprintf(link,sizeof link,"/proc/%ld/exe",(long)getpid());
    ssize_t r=readlink(link,out,cap?cap-1:0); if (r<0){ if(cap) out[0]=0; return 0; } out[r]=0; return (size_t)r;
#endif
}

/* ======================= Logging ======================= */

typedef enum { LOG_DEBUG=0, LOG_INFO=1, LOG_WARN=2, LOG_ERROR=3 } log_level;
static int g_log_level = LOG_INFO;
static int g_log_color = 1;       /* auto-color on TTY */
static FILE* g_log_out  = NULL;   /* NULL => stdout/stderr */

BASE_API void log_set_level(int lvl){ g_log_level = clamp(lvl, LOG_DEBUG, LOG_ERROR); }
BASE_API void log_set_color(int on){ g_log_color = on?1:0; }
BASE_API void log_set_stream(FILE* f){ g_log_out = f; }

static int is_tty(FILE* f){
#if defined(_WIN32)
    return _isatty(_fileno(f));
#else
    return isatty(fileno(f));
#endif
}
static void log_emit(int lvl, const char* tag, const char* fmt, va_list ap){
    if (lvl < g_log_level) return;
    char ts[25]; iso8601_utc(ts);
    FILE* out = g_log_out? g_log_out : (lvl>=LOG_WARN? stderr : stdout);
    int use_color = g_log_color && is_tty(out);
    const char* c = "";
    if (use_color){
        c = (lvl==LOG_DEBUG) ? "\x1b[90m" :
            (lvl==LOG_INFO ) ? "\x1b[36m" :
            (lvl==LOG_WARN ) ? "\x1b[33m" : "\x1b[31m";
        fprintf(out, "%s", c);
    }
    fprintf(out, "[%s] %s: ", ts, tag);
    vfprintf(out, fmt, ap);
    if (use_color) fprintf(out, "\x1b[0m");
    fputc('\n', out);
}
BASE_API void log_debug(const char* fmt, ...){ va_list ap; va_start(ap,fmt); log_emit(LOG_DEBUG,"DEBUG",fmt,ap); va_end(ap); }
BASE_API void log_info (const char* fmt, ...){ va_list ap; va_start(ap,fmt); log_emit(LOG_INFO ,"INFO ",fmt,ap); va_end(ap); }
BASE_API void log_warn (const char* fmt, ...){ va_list ap; va_start(ap,fmt); log_emit(LOG_WARN ,"WARN ",fmt,ap); va_end(ap); }
BASE_API void log_error(const char* fmt, ...){ va_list ap; va_start(ap,fmt); log_emit(LOG_ERROR,"ERROR",fmt,ap); va_end(ap); }

/* ======================= Process (léger) ======================= */

BASE_API int run_capture(const char* cmd, char** out, size_t* outlen){
    if (!cmd || !out) return -1;
#if defined(_WIN32)
    FILE* p = _popen(cmd, "rb");
    if (!p) return -1;
#else
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
#endif
    char* buf=NULL; size_t n=0;
    for(;;){
        char tmp[4096]; size_t r=fread(tmp,1,sizeof tmp,p);
        if (r) { sb_fit(buf, n + r + 1); memcpy(buf + n, tmp, r); n+=r; }
        if (r < sizeof tmp) { if (feof(p)) break; if (ferror(p)) { sb_free(buf); buf=NULL; n=0; break; } }
    }
    if (buf){ buf[n]=0; }
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    *out = buf; if (outlen) *outlen = n; return buf?0:-1;
}

/* ======================= Tests optionnels ======================= */
#ifdef BASE_TEST
static int print_cb(const char* path, const char* name, int is_dir, void* u){
    (void)u; printf("%s%c%s%s\n", path, PATH_SEP, name, is_dir?"/":""); return 0;
}
int main(void){
    log_set_level(LOG_DEBUG);
    log_info("Base test start");

    // SV
    sv t = sv_from("  foo/bar  "); sv tr=sv_trim(t);
    log_debug("trim='%.*s' starts(foo)=%d ends(bar)=%d", (int)tr.n,tr.p, sv_starts(tr,sv_from("foo")), sv_ends(tr,sv_from("bar")));

    // Arena + sb
    arena ar; arena_init(&ar, 0);
    int* v = NULL; for (int i=0;i<16;i++) sb_push(v, i*i);
    log_info("sb_len=%zu cap=%zu v[5]=%d", sb_len(v), sb_cap(v), v[5]);

    // HashMap
    hmap m; hmap_init(&m); hmap_put(&m, sv_from("k"), (void*)(uintptr_t)123);
    log_info("hmap get=%zu", (size_t)hmap_get(&m, sv_from("k")));
    hentry* e; hmap_foreach(m, e){ log_debug("map: %s=%zu", e->key,(size_t)e->val); }
    hmap_free(&m);

    // Files & dirs
    mkdir_p("tmp/a/b");
    write_all("tmp/a/b/file.txt", "hello", 5);
    listdir("tmp", print_cb, NULL);
    char* data=NULL; size_t dn=0; read_all("tmp/a/b/file.txt", (void**)&data, &dn);
    hexdump(data,dn,stdout); free(data);
    file_copy("tmp/a/b/file.txt", "tmp/a/copy.txt");
    file_move("tmp/a/copy.txt", "tmp/a/moved.txt");

    // Normalize
    char norm[256]; path_normalize("tmp//a/./b/../moved.txt", norm, sizeof norm);
    log_debug("normalized=%s", norm);

    // Time + RNG + UUID
    char ts[25]; iso8601_utc(ts); log_info("now=%s", ts);
    rng_seed_time(); char id[36]; uuid_v4(id); log_info("uuid=%.*s", 36, id);

    // Env
    char home[512]; home_dir(home,sizeof home); log_info("home=%s", home);
    char tmpd[512]; temp_dir(tmpd,sizeof tmpd); log_info("tmp=%s", tmpd);
    char exe[1024]; exe_path(exe,sizeof exe); log_info("exe=%s", exe);

    // Process
    char* out=NULL; size_t on=0; if (run_capture("echo VitteLight", &out, &on)==0){ log_info("run: %.*s", (int)on, out); free(out); }

    rm_rf("tmp");
    sb_free(v); arena_free(&ar);
    log_info("OK");
    return 0;
}
#endif