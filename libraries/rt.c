// SPDX-License-Identifier: GPL-3.0-or-later
//
// rt.c — Outils temps/horloge/tempo portables, “plus complet” (C17)
// Namespace: "rt"
//
// Fournit:
//   Horloges:
//     - rt_mono_ns()/ms()/us         : monotonic haute résolution
//     - rt_real_ns()/ms()/us         : temps réel (Unix epoch)
//     - rt_tsc_supported(), rt_tsc(), rt_tsc_hz()  (x86/x64, sinon 0)
//   Sommeil:
//     - rt_sleep_ms(ms), rt_sleep_us(us)
//     - rt_sleep_until_mono_ns(t_ns) : deadline monotonic
//   Deadline:
//     - rt_deadline_init(ms)
//     - rt_deadline_left_ms(), rt_deadline_expired()
//   Stopwatch:
//     - rt_sw_reset/start/stop/lap_ms/elapsed_ns/ms
//   Rate limiter (token bucket):
//     - rt_rl_init(rate_per_s, burst)
//     - rt_rl_allow(cost), rt_rl_wait_time_ms(cost)
//   Backoff exponentiel avec jitter:
//     - rt_backoff_init(base, cap, seed)
//     - rt_backoff_next_ms(), rt_backoff_reset()
//   Formatage / parse:
//     - rt_fmt_duration_ns(ns, buf, cap) -> "1h23m45.678s"
//     - rt_fmt_iso8601_real(buf, cap)    -> "YYYY-MM-DDTHH:MM:SS.sssZ"
//     - rt_parse_iso8601_real(str, *out_ns)  (fraction ms, ‘Z’ ou offset ±HH:MM)
//   Utilitaires timespec:
//     - rt_ns_to_timespec(ns, *ts), rt_timespec_to_ns(*ts)
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c rt.c
//
// Démo (RT_TEST):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DRT_TEST rt.c && ./a.out

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <mmsystem.h>
  #pragma comment(lib,"winmm.lib")
#else
  #include <time.h>
  #include <unistd.h>
  #include <sys/time.h>
#endif

#ifndef RT_API
#define RT_API
#endif

/* ====================== Horloges ====================== */

#if defined(_WIN32)
static uint64_t _qpf_hz(void){
    static LARGE_INTEGER f = {0};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    return (uint64_t)f.QuadPart;
}
static uint64_t _qpc_ticks(void){
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}
#endif

RT_API uint64_t rt_mono_ns(void){
#if defined(_WIN32)
    uint64_t t = _qpc_ticks();
    uint64_t f = _qpf_hz();
    long double ns = ((long double)t * 1.0e9L) / (long double)f;
    return (uint64_t)ns;
#else
  #if defined(CLOCK_MONOTONIC)
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  #else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  #endif
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
RT_API uint64_t rt_mono_ms(void){ return rt_mono_ns()/1000000ull; }
RT_API uint64_t rt_mono_us(void){ return rt_mono_ns()/1000ull; }

RT_API uint64_t rt_real_ns(void){
#if defined(_WIN32)
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t100 = ((uint64_t)ft.dwHighDateTime<<32) | ft.dwLowDateTime; /* 100ns since 1601 */
    uint64_t unix_1601_1970_100ns = 116444736000000000ull;
    uint64_t t_unix_100 = (t100 - unix_1601_1970_100ns);
    return t_unix_100 * 100ull;
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
RT_API uint64_t rt_real_ms(void){ return rt_real_ns()/1000000ull; }
RT_API uint64_t rt_real_us(void){ return rt_real_ns()/1000ull; }

/* ====================== TSC (best effort) ====================== */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
  #if defined(_MSC_VER)
    #include <intrin.h>
    static inline uint64_t _rdtsc64(void){ return __rdtsc(); }
  #else
    static inline uint64_t _rdtsc64(void){ unsigned int lo,hi; __asm__ __volatile__("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }
  #endif
  static uint64_t _tsc_hz_cached=0;
#endif

RT_API int rt_tsc_supported(void){
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
    return 1;
#else
    return 0;
#endif
}
RT_API uint64_t rt_tsc(void){
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
    return _rdtsc64();
#else
    return 0;
#endif
}
/* Calibre en ~50ms via QPC/clock_gettime. */
RT_API uint64_t rt_tsc_hz(void){
#if !(defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86))
    return 0;
#else
    if (_tsc_hz_cached) return _tsc_hz_cached;
    uint64_t t0 = rt_mono_ns(), c0 = _rdtsc64();
    /* attendre ~50ms */
#if defined(_WIN32)
    Sleep(50);
#else
    struct timespec ts={0,50*1000000L}; nanosleep(&ts,NULL);
#endif
    uint64_t t1 = rt_mono_ns(), c1 = _rdtsc64();
    uint64_t dt_ns = (t1>t0)? (t1-t0) : 1;
    uint64_t dc = (c1>c0)? (c1-c0) : 1;
    long double hz = ((long double)dc * 1.0e9L) / (long double)dt_ns;
    _tsc_hz_cached = (uint64_t)(hz+0.5L);
    return _tsc_hz_cached;
#endif
}

/* ====================== Sleep ====================== */

RT_API void rt_sleep_ms(uint32_t ms){
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts; ts.tv_sec=ms/1000u; ts.tv_nsec=(long)(ms%1000u)*1000000L;
    while (nanosleep(&ts,&ts)==-1) {}
#endif
}
RT_API void rt_sleep_us(uint32_t us){
#if defined(_WIN32)
    /* Windows: résolution ~1ms. Busy-wait court pour <1ms. */
    if (us<=1000){ uint64_t end=rt_mono_ns()+ (uint64_t)us*1000ull; while (rt_mono_ns()<end) { /* spin */ } return; }
    Sleep((us+999)/1000);
#else
    struct timespec ts; ts.tv_sec=us/1000000u; ts.tv_nsec=(long)(us%1000000u)*1000L;
    while (nanosleep(&ts,&ts)==-1) {}
#endif
}
/* dort jusqu’à t_ns (monotonic). Retour 0 si atteint, >0 ms restants si réveil anticipé. */
RT_API uint32_t rt_sleep_until_mono_ns(uint64_t t_ns){
    while (1){
        uint64_t now = rt_mono_ns();
        if (now>=t_ns) return 0;
        uint64_t left_ns = t_ns - now;
        if (left_ns > 2e6) rt_sleep_us((uint32_t)(left_ns/1000ull) - 1000u); /* garder une marge */
        else { while (rt_mono_ns()<t_ns) {} return 0; }
    }
}

/* ====================== Deadline ====================== */

typedef struct { uint64_t end_ns; } rt_deadline;

RT_API void rt_deadline_init(rt_deadline* d, uint32_t timeout_ms){
    if (!d) return; d->end_ns = rt_mono_ns() + (uint64_t)timeout_ms*1000000ull;
}
RT_API int rt_deadline_expired(const rt_deadline* d){
    if (!d) return 1; return rt_mono_ns() >= d->end_ns;
}
RT_API uint32_t rt_deadline_left_ms(const rt_deadline* d){
    if (!d) return 0;
    uint64_t now=rt_mono_ns();
    if (now>=d->end_ns) return 0;
    uint64_t ms=(d->end_ns-now)/1000000ull;
    return (ms>0xFFFFFFFFull)?0xFFFFFFFFu:(uint32_t)ms;
}

/* ====================== Stopwatch ====================== */

typedef struct { uint64_t t0_ns, acc_ns; int running; } rt_sw;
RT_API void rt_sw_reset(rt_sw* s){ if(!s) return; s->t0_ns=0; s->acc_ns=0; s->running=0; }
RT_API void rt_sw_start(rt_sw* s){ if(!s) return; if(!s->running){ s->t0_ns=rt_mono_ns(); s->running=1; } }
RT_API void rt_sw_stop (rt_sw* s){ if(!s) return; if(s->running){ s->acc_ns += rt_mono_ns()-s->t0_ns; s->running=0; } }
RT_API uint64_t rt_sw_elapsed_ns(const rt_sw* s){ if(!s) return 0; return s->running? s->acc_ns + (rt_mono_ns()-s->t0_ns) : s->acc_ns; }
RT_API uint64_t rt_sw_elapsed_ms(const rt_sw* s){ return rt_sw_elapsed_ns(s)/1000000ull; }
RT_API uint64_t rt_sw_lap_ms(rt_sw* s){
    if(!s) return 0; if(!s->running){ rt_sw_start(s); return 0; }
    uint64_t now=rt_mono_ns(); uint64_t lap=now - s->t0_ns; s->t0_ns=now; return lap/1000000ull;
}

/* ====================== Rate limiter ====================== */

typedef struct {
    double rate_per_s, burst, tokens;
    uint64_t last_ns;
} rt_rl;

RT_API void rt_rl_init(rt_rl* rl, double rate_per_s, double burst){
    if(!rl) return;
    if (rate_per_s<0) rate_per_s=0;
    if (burst<1) burst=1;
    rl->rate_per_s=rate_per_s; rl->burst=burst; rl->tokens=burst; rl->last_ns=rt_mono_ns();
}
RT_API int rt_rl_allow(rt_rl* rl, double cost){
    if(!rl) return 0;
    uint64_t now=rt_mono_ns();
    double dt=(double)(now-rl->last_ns)/1e9; rl->last_ns=now;
    rl->tokens += rl->rate_per_s * dt; if (rl->tokens>rl->burst) rl->tokens=rl->burst;
    if (rl->tokens >= cost){ rl->tokens -= cost; return 1; }
    return 0;
}
RT_API uint32_t rt_rl_wait_time_ms(const rt_rl* rl, double cost){
    if(!rl) return 0xFFFFFFFFu;
    if (rl->tokens >= cost) return 0;
    double need = cost - rl->tokens;
    if (rl->rate_per_s <= 0) return 0xFFFFFFFFu;
    double ms = (need / rl->rate_per_s) * 1000.0;
    if (ms<0) ms=0; if (ms>4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)(ms+0.5);
}

/* ====================== Backoff exponentiel ====================== */

typedef struct {
    uint32_t base_ms, cap_ms, cur_ms;
    uint64_t seed;
} rt_backoff;

static uint64_t _xs64(uint64_t* s){ uint64_t x=*s; x^=x<<13; x^=x>>7; x^=x<<17; *s=x; return x; }

RT_API void rt_backoff_init(rt_backoff* b, uint32_t base_ms, uint32_t cap_ms, uint64_t seed){
    if(!b) return; if(!base_ms) base_ms=1; if (cap_ms<base_ms) cap_ms=base_ms;
    b->base_ms=base_ms; b->cap_ms=cap_ms; b->cur_ms=base_ms;
    b->seed = seed?seed:(rt_mono_ns()^0x9E3779B97F4A7C15ull);
}
RT_API uint32_t rt_backoff_next_ms(rt_backoff* b){
    if(!b) return 0;
    uint64_t r=_xs64(&b->seed); double jitter=0.5 + (double)(r&0xFFFF)/65535.0*0.5;
    uint32_t d=(uint32_t)((double)b->cur_ms*jitter + 0.5);
    uint64_t next=(uint64_t)b->cur_ms*2u; if (next>b->cap_ms) next=b->cap_ms; b->cur_ms=(uint32_t)next;
    return d;
}
RT_API void rt_backoff_reset(rt_backoff* b){ if(!b) return; b->cur_ms=b->base_ms; }

/* ====================== Format / Parse ====================== */

RT_API void rt_ns_to_timespec(uint64_t ns, struct timespec* ts){
#if defined(_WIN32)
    /* struct timespec existe en C17 mais non utilisée par Win32 API; ok pour outils */
#endif
    if (!ts) return; ts->tv_sec = (time_t)(ns/1000000000ull); ts->tv_nsec = (long)(ns%1000000000ull);
}
RT_API uint64_t rt_timespec_to_ns(const struct timespec* ts){
    if (!ts) return 0; return (uint64_t)ts->tv_sec*1000000000ull + (uint64_t)ts->tv_nsec;
}

/* "1h23m45.678s" ou "123ms" selon la grandeur. */
RT_API int rt_fmt_duration_ns(uint64_t ns, char* out, size_t cap){
    if (!out||cap==0) return -1;
    uint64_t s = ns/1000000000ull; uint64_t rem_ns = ns%1000000000ull;
    uint64_t h=s/3600ull, m=(s%3600ull)/60ull, sec=s%60ull;
    if (h){ return snprintf(out,cap,"%lluh%02llum%02llu.%03llus",
            (unsigned long long)h,(unsigned long long)m,(unsigned long long)sec,(unsigned long long)(rem_ns/1000000ull)); }
    if (m){ return snprintf(out,cap,"%llum%02llu.%03llus",
            (unsigned long long)m,(unsigned long long)sec,(unsigned long long)(rem_ns/1000000ull)); }
    if (s){ return snprintf(out,cap,"%llu.%03llus",(unsigned long long)sec,(unsigned long long)(rem_ns/1000000ull)); }
    if (ns>=1000000ull) return snprintf(out,cap,"%llums",(unsigned long long)(ns/1000000ull));
    if (ns>=1000ull)    return snprintf(out,cap,"%lluus",(unsigned long long)(ns/1000ull));
    return snprintf(out,cap,"%lluns",(unsigned long long)ns);
}

/* ISO8601 UTC pour temps réel courant. Exemple: 2025-09-19T12:34:56.789Z */
RT_API int rt_fmt_iso8601_real(char* out, size_t cap){
    if (!out||cap<25) return -1;
    uint64_t ns = rt_real_ns(); uint64_t ms = ns/1000000ull;
    time_t sec = (time_t)(ms/1000ull); int msec = (int)(ms%1000ull);
#if defined(_WIN32)
    SYSTEMTIME st; FILETIME ft; ULARGE_INTEGER li;
    /* reconvertir sec->SYSTEMTIME local est coûteux; utiliser gmtime_r-like portable */
#endif
    struct tm t; 
#if defined(_WIN32)
    gmtime_s(&t, &sec);
#else
    gmtime_r(&sec, &t);
#endif
    return snprintf(out,cap,"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, msec);
}

/* Parse ISO8601 restreint: "YYYY-MM-DDTHH:MM:SS(.sss)?(Z|±HH:MM)". Retour 0 ok, -1 sinon. */
RT_API int rt_parse_iso8601_real(const char* s, uint64_t* out_ns){
    if (!s||!out_ns) return -1;
    int Y,M,D,h,m; int sec; int ms=0; int off_sign=0, off_h=0, off_min=0; char z=0;
    /* scan basique */
    if (sscanf(s,"%4d-%2d-%2dT%2d:%2d:%2d", &Y,&M,&D,&h,&m,&sec)!=6) return -1;
    const char* p = s; int dots=0;
    const char* dot = strchr(s,'T'); if (dot) dot=strchr(dot, '.');
    if (dot){ if (sscanf(dot, ".%3d", &ms)!=1) ms=0; }
    const char* tz = strrchr(s,'Z'); if (tz && tz[1]==0) z='Z';
    if (!z){
        const char* pm = strrchr(s,'+'); if (!pm) pm = strrchr(s,'-');
        if (pm){
            off_sign = (*pm=='-')? -1 : 1;
            if (sscanf(pm+1,"%2d:%2d",&off_h,&off_min)!=2) return -1;
        }
    }
    /* convertir YMDhms -> epoch (UTC) */
    struct tm t={0}; t.tm_year=Y-1900; t.tm_mon=M-1; t.tm_mday=D; t.tm_hour=h; t.tm_min=m; t.tm_sec=sec;
#if defined(_WIN32)
    /* timegm portable */
    time_t epoch = _mkgmtime(&t);
#else
    time_t epoch = timegm(&t);
#endif
    if (epoch==(time_t)-1) return -1;
    int total_off = (z?0:(off_sign*(off_h*60+off_min)));
    int64_t adj = (int64_t)epoch - (int64_t)total_off*60; /* soustraire offset pour UTC */
    if (adj<0) return -1;
    *out_ns = (uint64_t)adj*1000000000ull + (uint64_t)ms*1000000ull;
    return 0;
}

/* ====================== Test ====================== */
#ifdef RT_TEST
int main(void){
    char buf[64];
    printf("mono=%llu ms real=%llu ms\n",
        (unsigned long long)rt_mono_ms(), (unsigned long long)rt_real_ms());
    rt_sleep_ms(20);
    rt_sw sw; rt_sw_reset(&sw); rt_sw_start(&sw); rt_sleep_ms(50); printf("lap=%llums\n",(unsigned long long)rt_sw_lap_ms(&sw)); rt_sleep_ms(30); rt_sw_stop(&sw);
    printf("elapsed=%llums\n",(unsigned long long)rt_sw_elapsed_ms(&sw));

    rt_deadline d; rt_deadline_init(&d, 120);
    while(!rt_deadline_expired(&d)){ printf("left~%u ms\n", rt_deadline_left_ms(&d)); rt_sleep_ms(30); }

    rt_rl rl; rt_rl_init(&rl, 5.0, 5.0); int ok=0, deny=0; for(int i=0;i<20;i++){ if(rt_rl_allow(&rl,1.0)) ok++; else deny++; rt_sleep_ms(50); }
    printf("rate allow=%d deny=%d\n", ok, deny);

    rt_backoff bo; rt_backoff_init(&bo, 10, 200, 0); for(int i=0;i<6;i++){ printf("backoff=%u ms\n", rt_backoff_next_ms(&bo)); }

    printf("dur=%s\n", (rt_fmt_duration_ns(5023678000000ull, buf, sizeof buf), buf));
    printf("iso now=%s\n", (rt_fmt_iso8601_real(buf,sizeof buf), buf));
    uint64_t ns=0; if (rt_parse_iso8601_real("2025-09-19T12:34:56.789Z",&ns)==0) printf("parsed ns=%llu\n",(unsigned long long)ns);

#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86))
    printf("tsc_supported=%d hz=%llu\n", rt_tsc_supported(), (unsigned long long)rt_tsc_hz());
#endif
    return 0;
}
#endif