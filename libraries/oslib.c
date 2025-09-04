// vitte-light/libraries/oslib.c
// OS library for VitteLight: env, cwd, time, system, CPU count, temp, exec.
// C99. Cross‑platform (POSIX and Windows).
//
// Public entry:
//   void vl_register_oslib(struct VL_Context *ctx);
//
// Natives:
//   os_getenv(name)             -> str|nil
//   os_setenv(name, val[,ow=1]) -> bool
//   os_unsetenv(name)           -> bool
//   os_environ()                -> str   ("KEY=VAL\n...")
//   os_cwd()                    -> str
//   os_chdir(path)              -> bool
//   os_mkdir_p(path)            -> bool
//   os_tempdir()                -> str
//   os_hostname()               -> str
//   os_pid()                    -> int
//   os_cpu_count()              -> int
//   os_wall_time_ns()           -> int
//   os_mono_time_ns()           -> int
//   os_sleep_ms(ms)             -> nil
//   os_system(cmd)              -> int   (exit status)
//   os_exec(cmd)                -> str   (stdout capture)
//   os_isatty(fd)               -> bool
//   os_uname()                  -> str   (summary)
//   os_chmod(path, mode)        -> bool  (POSIX only; Windows returns false)
//   os_umask([mask])            -> int   (POSIX only; get or set)
//
// Build:
//   cc -std=c99 -O2 -Wall -Wextra -pedantic -Icore -c libraries/oslib.c

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "api.h"
#include "ctype.h"
#include "tm.h"      // vl_mono_time_ns, vl_sleep_ms
#include "string.h"  // VL_String, vl_make_strn

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define getcwd _getcwd
#  define chdir  _chdir
#  define isatty _isatty
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/utsname.h>
#endif

// Optional functions provided by other libs in this repo
int  vl_mkdir_p(const char *path);                 // auxlib/iolib
int  vl_exec_capture(const char *cmd, char **out_text, size_t *out_len, int *out_status); // iolib
int  vl_rand_bytes(void *buf, size_t n);           // auxlib

// ───────────────────────── VM glue ─────────────────────────
#define RET_NIL()      do{ if(ret) *(ret)=vlv_nil(); return VL_OK; }while(0)
#define RET_INT(v)     do{ if(ret) *(ret)=vlv_int((int64_t)(v)); return VL_OK; }while(0)
#define RET_BOOL(v)    do{ if(ret) *(ret)=vlv_bool((v)!=0); return VL_OK; }while(0)
#define RET_STR(p,n)   do{ VL_Value __s = vl_make_strn(ctx,(const char*)(p),(uint32_t)(n)); if(__s.type!=VT_STR) return VL_ERR_OOM; if(ret) *ret=__s; return VL_OK; }while(0)

static int need_str(const VL_Value *v){ return v && v->type==VT_STR && v->as.s; }
static int64_t want_int(const VL_Value *v, int *ok){ int64_t x=0; *ok = vl_value_as_int(v,&x); return x; }

// ───────────────────────── Helpers ─────────────────────────
static size_t strlcpy_c(char *dst, const char *src, size_t n){ size_t L=src?strlen(src):0; if(n){ size_t m = (L>=n)? n-1: L; if(m) memcpy(dst,src,m); dst[m]='\0'; } return L; }

static VL_Value make_cstr(struct VL_Context *ctx, const char *s){ return vl_make_strn(ctx, s? s: "", (uint32_t)(s? strlen(s):0)); }

// ───────────────────────── env ─────────────────────────
static VL_Status os_getenv_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ud; if(c<1||!need_str(&a[0])) return VL_ERR_TYPE; const char *v = getenv(a[0].as.s->data); if(!v) RET_NIL(); VL_Value s = make_cstr(ctx,v); if(s.type!=VT_STR) return VL_ERR_OOM; if(ret) *ret=s; return VL_OK; }

static VL_Status os_setenv_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ctx;(void)ud; if(c<2||!need_str(&a[0])||!need_str(&a[1])) return VL_ERR_TYPE; int ow=1, ok=1; if(c>=3) { int64_t x=want_int(&a[2], &ok); if(!ok) return VL_ERR_TYPE; ow = (int)(x!=0); }
#if defined(_WIN32)
    // Windows _putenv expects "KEY=VAL" or "KEY=" to delete
    size_t L = strlen(a[0].as.s->data) + 1 + strlen(a[1].as.s->data);
    char *kv = (char*)malloc(L+1); if(!kv) return VL_ERR_OOM;
    strcpy(kv, a[0].as.s->data); strcat(kv, "="); strcat(kv, a[1].as.s->data);
    int rc = _putenv(kv); free(kv); RET_BOOL(rc==0);
#else
    int rc;
    if(!ow){ const char *cur = getenv(a[0].as.s->data); if(cur) { RET_BOOL(1); } }
    rc = setenv(a[0].as.s->data, a[1].as.s->data, 1);
    RET_BOOL(rc==0);
#endif
}

static VL_Status os_unsetenv_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ctx;(void)ud; if(c<1||!need_str(&a[0])) return VL_ERR_TYPE;
#if defined(_WIN32)
    size_t L = strlen(a[0].as.s->data) + 1; char *kv=(char*)malloc(L+1); if(!kv) return VL_ERR_OOM; strcpy(kv, a[0].as.s->data); strcat(kv, "="); int rc=_putenv(kv); free(kv); RET_BOOL(rc==0);
#else
    int rc = unsetenv(a[0].as.s->data); RET_BOOL(rc==0);
#endif
}

static VL_Status os_environ_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ud;(void)a;(void)c;
#if defined(_WIN32)
    LPCH env = GetEnvironmentStringsA(); if(!env) return VL_ERR_IO; const char *p=(const char*)env; size_t cap=4096,n=0; char *buf=(char*)malloc(cap); if(!buf){ FreeEnvironmentStringsA(env); return VL_ERR_OOM; } while(*p){ size_t L=strlen(p); if(n+L+1>=cap){ size_t nc=cap*2; char *nb=(char*)realloc(buf,nc); if(!nb){ free(buf); FreeEnvironmentStringsA(env); return VL_ERR_OOM; } buf=nb; cap=nc; } memcpy(buf+n,p,L); n+=L; buf[n++]='\n'; p += L+1; } VL_Value s = vl_make_strn(ctx, buf, (uint32_t)n); free(buf); FreeEnvironmentStringsA(env); if(s.type!=VT_STR) return VL_ERR_OOM; if(ret)*ret=s; return VL_OK;
#else
    extern char **environ; size_t cap=4096,n=0; char *buf=(char*)malloc(cap); if(!buf) return VL_ERR_OOM; buf[0]='\0'; for(char **e=environ; e && *e; ++e){ size_t L=strlen(*e); if(n+L+1>=cap){ size_t nc=cap*2; char *nb=(char*)realloc(buf,nc); if(!nb){ free(buf); return VL_ERR_OOM; } buf=nb; cap=nc; } memcpy(buf+n,*e,L); n+=L; buf[n++]='\n'; } VL_Value s = vl_make_strn(ctx, buf, (uint32_t)n); free(buf); if(s.type!=VT_STR) return VL_ERR_OOM; if(ret)*ret=s; return VL_OK;
#endif
}

// ───────────────────────── cwd / paths ─────────────────────────
static VL_Status os_cwd_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)a;(void)c;(void)ud; char tmp[4096]; if(!getcwd(tmp,sizeof(tmp))) return VL_ERR_IO; RET_STR(tmp, strlen(tmp)); }
static VL_Status os_chdir_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ctx;(void)ud; if(c<1||!need_str(&a[0])) return VL_ERR_TYPE; int rc = chdir(a[0].as.s->data); RET_BOOL(rc==0); }
static VL_Status os_mkdir_p_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ud; if(c<1||!need_str(&a[0])) return VL_ERR_TYPE; RET_BOOL(vl_mkdir_p(a[0].as.s->data)); }

// ───────────────────────── system info ─────────────────────────
static VL_Status os_hostname_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)a;(void)c;(void)ud; char buf[256]; buf[0]='\0';
#if defined(_WIN32)
    DWORD sz = (DWORD)sizeof(buf); if(!GetComputerNameA(buf, &sz)) return VL_ERR_IO; RET_STR(buf, strlen(buf));
#else
    if(gethostname(buf,sizeof(buf))!=0) return VL_ERR_IO; RET_STR(buf, strlen(buf));
#endif
}

static VL_Status os_pid_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)a;(void)c;(void)ud;
#if defined(_WIN32)
    RET_INT((int64_t)GetCurrentProcessId());
#else
    RET_INT((int64_t)getpid());
#endif
}

static VL_Status os_cpu_count_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)a;(void)c;(void)ud;
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si); RET_INT((int64_t)(si.dwNumberOfProcessors? si.dwNumberOfProcessors:1));
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN); if(n<1) n=1; RET_INT((int64_t)n);
#endif
}

static VL_Status os_uname_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)a;(void)c;(void)ud; char line[256];
#if defined(_WIN32)
    SYSTEM_INFO si; GetNativeSystemInfo(&si); const char *arch = (si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64?"x86_64": si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_INTEL?"x86": si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_ARM64?"arm64":"unknown");
    snprintf(line,sizeof(line),"Windows arch=%s", arch);
    RET_STR(line, strlen(line));
#else
    struct utsname u; if(uname(&u)!=0) return VL_ERR_IO; snprintf(line,sizeof(line),"%s %s %s %s", u.sysname,u.release,u.version,u.machine); RET_STR(line, strlen(line));
#endif
}

// ───────────────────────── time / sleep ─────────────────────────
static VL_Status os_wall_time_ns_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)a;(void)c;(void)ud;
#if defined(_WIN32)
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft); ULARGE_INTEGER u; u.HighPart=ft.dwHighDateTime; u.LowPart=ft.dwLowDateTime; // 100ns since 1601
    uint64_t ns = (uint64_t)u.QuadPart * 100ull; RET_INT((int64_t)ns);
#else
#  if defined(CLOCK_REALTIME)
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); uint64_t ns=(uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec; RET_INT((int64_t)ns);
#  else
    struct timeval tv; gettimeofday(&tv,NULL); uint64_t ns=(uint64_t)tv.tv_sec*1000000000ull + (uint64_t)tv.tv_usec*1000ull; RET_INT((int64_t)ns);
#  endif
#endif
}

static VL_Status os_mono_time_ns_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)a;(void)c;(void)ud; RET_INT((int64_t)vl_mono_time_ns()); }
static VL_Status os_sleep_ms_fn     (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ud; if(c<1) return VL_ERR_INVAL; int ok=1; int64_t ms = want_int(&a[0], &ok); if(!ok||ms<0) return VL_ERR_TYPE; vl_sleep_ms((uint32_t)ms); RET_NIL(); }

// ───────────────────────── system / exec ─────────────────────────
static VL_Status os_system_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ctx;(void)ud; if(c<1||!need_str(&a[0])) return VL_ERR_TYPE; int rc = system(a[0].as.s->data); RET_INT((int64_t)rc); }
static VL_Status os_exec_fn  (struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ud; if(c<1||!need_str(&a[0])) return VL_ERR_TYPE; char *out=NULL; size_t n=0; int st=0; if(!vl_exec_capture(a[0].as.s->data, &out, &n, &st)) return VL_ERR_IO; VL_Value s = vl_make_strn(ctx, out, (uint32_t)n); free(out); if(s.type!=VT_STR) return VL_ERR_OOM; if(ret) *ret=s; return VL_OK; }

// ───────────────────────── temp / isatty ─────────────────────────
static VL_Status os_tempdir_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)a;(void)c;(void)ud; const char *td=NULL;
#if defined(_WIN32)
    static char buf[MAX_PATH]; DWORD m=GetTempPathA((DWORD)sizeof(buf), buf); if(m==0 || m>=sizeof(buf)) return VL_ERR_IO; td=buf; RET_STR(td, strlen(td));
#else
    td = getenv("TMPDIR"); if(!td||!*td) td="/tmp"; RET_STR(td, strlen(td));
#endif
}

static VL_Status os_isatty_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ud; int fd=1, ok=1; if(c>=1){ int64_t x=want_int(&a[0],&ok); if(!ok) return VL_ERR_TYPE; fd=(int)x; } RET_BOOL(isatty(fd)); }

// ───────────────────────── chmod / umask (POSIX) ─────────────────────────
static VL_Status os_chmod_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ud; if(c<2||!need_str(&a[0])) return VL_ERR_TYPE; int ok=1; int64_t mode = want_int(&a[1], &ok); if(!ok) return VL_ERR_TYPE;
#if defined(_WIN32)
    (void)mode; RET_BOOL(0);
#else
    int rc = chmod(a[0].as.s->data, (mode_t)mode); RET_BOOL(rc==0);
#endif
}

static VL_Status os_umask_fn(struct VL_Context *ctx, const VL_Value *a, uint8_t c, VL_Value *ret, void *ud){ (void)ctx;(void)ud;
#if defined(_WIN32)
    RET_INT(0);
#else
    if(c<1 || a[0].type==VT_NIL){ mode_t cur = umask(0); umask(cur); RET_INT((int64_t)cur); }
    int ok=1; int64_t m = want_int(&a[0], &ok); if(!ok) return VL_ERR_TYPE; mode_t old = umask((mode_t)m); RET_INT((int64_t)old);
#endif
}

// ───────────────────────── Registration ─────────────────────────
void vl_register_oslib(struct VL_Context *ctx){ if(!ctx) return;
    vl_register_native(ctx, "os_getenv",       os_getenv_fn,       NULL);
    vl_register_native(ctx, "os_setenv",       os_setenv_fn,       NULL);
    vl_register_native(ctx, "os_unsetenv",     os_unsetenv_fn,     NULL);
    vl_register_native(ctx, "os_environ",      os_environ_fn,      NULL);

    vl_register_native(ctx, "os_cwd",          os_cwd_fn,          NULL);
    vl_register_native(ctx, "os_chdir",        os_chdir_fn,        NULL);
    vl_register_native(ctx, "os_mkdir_p",      os_mkdir_p_fn,      NULL);

    vl_register_native(ctx, "os_tempdir",      os_tempdir_fn,      NULL);
    vl_register_native(ctx, "os_hostname",     os_hostname_fn,     NULL);
    vl_register_native(ctx, "os_pid",          os_pid_fn,          NULL);
    vl_register_native(ctx, "os_cpu_count",    os_cpu_count_fn,    NULL);
    vl_register_native(ctx, "os_uname",        os_uname_fn,        NULL);

    vl_register_native(ctx, "os_wall_time_ns", os_wall_time_ns_fn, NULL);
    vl_register_native(ctx, "os_mono_time_ns", os_mono_time_ns_fn, NULL);
    vl_register_native(ctx, "os_sleep_ms",     os_sleep_ms_fn,     NULL);

    vl_register_native(ctx, "os_system",       os_system_fn,       NULL);
    vl_register_native(ctx, "os_exec",         os_exec_fn,         NULL);
    vl_register_native(ctx, "os_isatty",       os_isatty_fn,       NULL);

    vl_register_native(ctx, "os_chmod",        os_chmod_fn,        NULL);
    vl_register_native(ctx, "os_umask",        os_umask_fn,        NULL);
}

// ───────────────────────── Self‑test (optional) ─────────────────────────
#ifdef VL_OSLIB_TEST_MAIN
int main(void){
#if !defined(_WIN32)
    printf("uname: "); struct utsname u; if(uname(&u)==0) printf("%s %s %s %s\n", u.sysname,u.release,u.version,u.machine);
#endif
    printf("pid=%d cpus=", (int)
#if defined(_WIN32)
           GetCurrentProcessId()
#else
           getpid()
#endif
    );
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si); printf("%lu\n", (unsigned long)si.dwNumberOfProcessors);
#else
    long n=sysconf(_SC_NPROCESSORS_ONLN); printf("%ld\n", n);
#endif
    return 0; }
#endif
