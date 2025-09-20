// SPDX-License-Identifier: GPL-3.0-or-later
//
// time.c — Outils temps/horodatage portables, “plus complet” (C17)
// Namespace: "tm"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c time.c
//
// Fournit (portables Windows/POSIX):
//   Horloges:
//     double   tm_now_s(void);
//     uint64_t tm_now_ms(void);
//     uint64_t tm_now_ns(void);
//     double   tm_mono_s(void);
//     uint64_t tm_mono_ms(void);
//     uint64_t tm_mono_ns(void);
//     void     tm_sleep_ms(unsigned ms);
//     void     tm_sleep_until_mono_ms(uint64_t deadline_ms);
//   ISO8601 / RFC3339:
//     int  tm_format_iso8601(time_t t, int utc, char* out, size_t cap);
//     int  tm_format_iso8601_frac(time_t t, int utc, int frac_digits, char* out, size_t cap);
//     int  tm_iso8601_now(char* out, size_t cap, int utc);
//     int  tm_parse_iso8601(const char* s, time_t* out);            // YYYY-MM-DD[Thh:mm[:ss][.frac]][Z|±HH:MM]
//   Décalages / fuseaux:
//     int  tm_local_utc_offset_sec(time_t t, int* out_sec);         // offset local - UTC
//     int  tm_is_dst_local(time_t t);                                // 1 oui, 0 non, -1 inconnu
//   Conversions:
//     int  tm_from_utc_components(int Y,int M,int D,int h,int m,int s,time_t* out);
//     int  tm_from_local_components(int Y,int M,int D,int h,int m,int s,time_t* out);
//   Timespec helpers:
//     typedef struct timespec tm_timespec;
//     tm_timespec tm_timespec_add(tm_timespec a, tm_timespec b);
//     tm_timespec tm_timespec_sub(tm_timespec a, tm_timespec b);
//     uint64_t    tm_timespec_ms(tm_timespec ts);
//     uint64_t    tm_diff_ms_mono(uint64_t t0_ms, uint64_t t1_ms);
//   Date utils (local):
//     time_t tm_start_of_day_local(time_t t);
//     time_t tm_end_of_day_local(time_t t);
//     time_t tm_add_days_local(time_t t, int days);
//   Chrono simple (monotone):
//     typedef struct tm_stopwatch { uint64_t t0_ms; } tm_stopwatch;
//     void     tm_sw_start(tm_stopwatch* sw);
//     uint64_t tm_sw_elapsed_ms(const tm_stopwatch* sw);
//     uint64_t tm_sw_lap_ms(tm_stopwatch* sw);  // renvoie écoulé puis redémarre
//
// Notes:
//   - timegm n’existe pas sur Windows; des chemins de repli sont fournis.
//   - Les fractions d’ISO8601 sont tronquées/zero-pad jusqu’à 9 chiffres.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <sysinfoapi.h>
#else
  #include <unistd.h>
  #include <sys/time.h>
  #include <errno.h>
  #include <sys/timeb.h>
#endif

#ifndef TM_API
#define TM_API
#endif

/* ===================== Horloges ===================== */

TM_API double tm_now_s(void){
#if defined(_WIN32)
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    const double EPOCH_DIFF = 11644473600.0; /* seconds 1601->1970 */
    return (double)u.QuadPart*1e-7 - EPOCH_DIFF;
#elif defined(CLOCK_REALTIME)
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec*1e-9;
#else
    struct timeval tv; gettimeofday(&tv,NULL);
    return (double)tv.tv_sec + tv.tv_usec*1e-6;
#endif
}
TM_API uint64_t tm_now_ms(void){ return (uint64_t)(tm_now_s()*1000.0 + 0.5); }

TM_API uint64_t tm_now_ns(void){
#if defined(_WIN32)
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    const uint64_t EPOCH_100NS = 11644473600ULL * 10000000ULL;
    uint64_t t100 = u.QuadPart - EPOCH_100NS;
    return t100 * 100ULL;
#elif defined(CLOCK_REALTIME)
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
#else
    struct timeval tv; gettimeofday(&tv,NULL);
    return (uint64_t)tv.tv_sec*1000000000ull + (uint64_t)tv.tv_usec*1000ull;
#endif
}

TM_API double tm_mono_s(void){
#if defined(_WIN32)
    static LARGE_INTEGER f = {{0}};
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart/(double)f.QuadPart;
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec*1e-9;
#else
    return tm_now_s();
#endif
}
TM_API uint64_t tm_mono_ms(void){ return (uint64_t)(tm_mono_s()*1000.0 + 0.5); }
TM_API uint64_t tm_mono_ns(void){
#if defined(_WIN32)
    static LARGE_INTEGER f = {{0}};
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((__int128)c.QuadPart * 1000000000ull / (uint64_t)f.QuadPart);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return tm_now_ns();
#endif
}

TM_API void tm_sleep_ms(unsigned ms){
#if defined(_WIN32)
    Sleep(ms);
#elif defined(_POSIX_C_SOURCE)
    struct timespec ts; ts.tv_sec = ms/1000u; ts.tv_nsec = (long)(ms%1000u)*1000000L;
    while (nanosleep(&ts,&ts)==-1 && errno==EINTR) {}
#else
    usleep(ms*1000u);
#endif
}
TM_API void tm_sleep_until_mono_ms(uint64_t deadline_ms){
    for(;;){
        uint64_t now = tm_mono_ms();
        if (now >= deadline_ms) break;
        uint64_t left = deadline_ms - now;
        tm_sleep_ms(left > 50 ? 50u : (unsigned)left);
    }
}

/* ===================== Helpers sûrs tm ===================== */

static int tm_gmtime_safe(const time_t* t, struct tm* out){
#if defined(_WIN32)
    return gmtime_s(out, t)==0 ? 0 : -1;
#else
    return gmtime_r(t, out) ? 0 : -1;
#endif
}
static int tm_localtime_safe(const time_t* t, struct tm* out){
#if defined(_WIN32)
    return localtime_s(out, t)==0 ? 0 : -1;
#else
    return localtime_r(t, out) ? 0 : -1;
#endif
}

/* ===================== ISO8601 / RFC3339 ===================== */

static int two_to_str(int x, char* out){ out[0] = (char)('0'+x/10); out[1] = (char)('0'+x%10); return 0; }

TM_API int tm_format_iso8601(time_t t, int utc, char* out, size_t cap){
    return tm_format_iso8601_frac(t, utc, 0, out, cap);
}

/* frac_digits ∈ [0..9]. utc=1 => suffixe 'Z', sinon ajout offset local ±HH:MM si dispo. */
TM_API int tm_format_iso8601_frac(time_t t, int utc, int frac_digits, char* out, size_t cap){
    if (!out || cap<20) return -1;
    if (frac_digits<0) frac_digits=0; if (frac_digits>9) frac_digits=9;

    struct tm tmv;
    if (utc){ if (tm_gmtime_safe(&t,&tmv)!=0) return -1; }
    else    { if (tm_localtime_safe(&t,&tmv)!=0) return -1; }

    char buf[64]; size_t n=0;
    int k = snprintf(buf,sizeof buf,"%04d-%02d-%02dT%02d:%02d:%02d",
                     tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    if (k<0) return -1; n=(size_t)k;

    /* fraction: pas de source sous-seconde pour un time_t; on met 0...0 si demandé */
    if (frac_digits>0){
        if (n+1+(size_t)frac_digits >= sizeof buf) return -1;
        buf[n++]='.';
        for (int i=0;i<frac_digits;i++) buf[n++]='0';
    }

    if (utc){
        if (n+1 >= sizeof buf) return -1;
        buf[n++]='Z';
        buf[n]=0;
    } else {
        int off=0;
        if (tm_local_utc_offset_sec(t, &off)==0){
            int sign = off>=0? +1 : -1; int a = off*sign;
            int oh=a/3600, om=(a%3600)/60;
            if (n+6 >= sizeof buf) return -1;
            buf[n++] = sign>0?'+':'-';
            two_to_str(oh, &buf[n]); n+=2;
            buf[n++]=':';
            two_to_str(om, &buf[n]); n+=2;
            buf[n]=0;
        } else {
            buf[n]=0; /* sans offset si indisponible */
        }
    }
    if (n+1>cap) return -1;
    memcpy(out,buf,n+1);
    return (int)n;
}

TM_API int tm_iso8601_now(char* out, size_t cap, int utc){
    time_t t = (time_t)tm_now_s();
    return tm_format_iso8601(t, utc, out, cap);
}

/* Parseur tolérant: YYYY-MM-DD[Thh:mm[:ss][.frac]][Z|±HH[:MM]] */
static int dig2(const char* p){ return isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]); }
static int rd2(const char* p){ return (p[0]-'0')*10 + (p[1]-'0'); }
static int rdn(const char* s, int n, int* out){
    int v=0; for(int i=0;i<n;i++){ if(!isdigit((unsigned char)s[i])) return -1; v = v*10 + (s[i]-'0'); }
    *out=v; return 0;
}
TM_API int tm_parse_iso8601(const char* s, time_t* out){
    if (!s||!out) return -1;
    size_t L=strlen(s); if (L<10) return -1;
    if (!(isdigit((unsigned char)s[0])&&isdigit((unsigned char)s[1])&&
          isdigit((unsigned char)s[2])&&isdigit((unsigned char)s[3])&&
          s[4]=='-' && dig2(&s[5]) && s[7]=='-' && dig2(&s[8]))) return -1;

    int Y,M,D,h=0,m=0,sec=0;
    if (rdn(&s[0],4,&Y)<0) return -1;
    M=rd2(&s[5]); D=rd2(&s[8]);
    size_t i=10;
    long tz_off_sec = 0; int have_tz=0;
    if (i<L && (s[i]=='T' || s[i]=='t' || s[i]==' ')){
        i++;
        if (!(i+1<L && dig2(&s[i]))) return -1; h=rd2(&s[i]); i+=2;
        if (i<L && s[i]==':'){ i++; if (!(i+1<L && dig2(&s[i]))) return -1; m=rd2(&s[i]); i+=2; }
        if (i<L && s[i]==':'){ i++; if (!(i+1<L && dig2(&s[i]))) return -1; sec=rd2(&s[i]); i+=2; }
        if (i<L && s[i]=='.'){ /* ignore fraction for time_t */
            i++; while (i<L && isdigit((unsigned char)s[i])) i++;
        }
        if (i<L){
            if (s[i]=='Z' || s[i]=='z'){ have_tz=1; tz_off_sec=0; i++; }
            else if (s[i]=='+' || s[i]=='-'){
                int sign = (s[i]=='+')? +1 : -1; i++;
                if (!(i+1<L && dig2(&s[i]))) return -1;
                int th=rd2(&s[i]); i+=2;
                int tmn=0;
                if (i<L && s[i]==':'){ i++; if (!(i+1<L && dig2(&s[i]))) return -1; tmn=rd2(&s[i]); i+=2; }
                else if (i+1<L && isdigit((unsigned char)s[i]) && isdigit((unsigned char)s[i+1])){ tmn=rd2(&s[i]); i+=2; }
                tz_off_sec = sign*(th*3600 + tmn*60);
                have_tz=1;
            }
        }
    }

    struct tm tmv; memset(&tmv,0,sizeof tmv);
    tmv.tm_year = Y-1900;
    tmv.tm_mon  = M-1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min  = m;
    tmv.tm_sec  = sec;

#if defined(_WIN32)
    /* Interpréter comme UTC: pas de timegm -> utiliser _mkgmtime si dispo, sinon ajuster via _timezone */
    #if defined(_MSC_VER) || defined(__MINGW32__)
      time_t tutc = _mkgmtime(&tmv);
      if (tutc==(time_t)-1) return -1;
    #else
      time_t tloc = mktime(&tmv); if (tloc==(time_t)-1) return -1;
      long tzs=0; _get_timezone(&tzs); time_t tutc = tloc - tzs;
    #endif
#else
    time_t tutc = timegm(&tmv); if (tutc==(time_t)-1) return -1;
#endif

    if (have_tz) tutc -= tz_off_sec; /* 10:00+02:00 -> 08:00Z */
    *out = tutc;
    return 0;
}

/* ===================== Offset / DST ===================== */

TM_API int tm_local_utc_offset_sec(time_t t, int* out_sec){
    if (!out_sec) return -1;
    struct tm g,l;
    if (tm_gmtime_safe(&t,&g)!=0) return -1;
    if (tm_localtime_safe(&t,&l)!=0) return -1;

#if defined(_WIN32)
    /* Convertir vers secondes pour comparer */
    #if defined(_MSC_VER) || defined(__MINGW32__)
      time_t ug = _mkgmtime(&g); if (ug==(time_t)-1) return -1;
    #else
      time_t ug = t; /* approximation */
    #endif
    time_t ul = mktime(&l); if (ul==(time_t)-1) return -1;
    long off = (long)difftime(ul,ug);
#else
    time_t ug = timegm(&g); if (ug==(time_t)-1) return -1;
    time_t ul = mktime(&l); if (ul==(time_t)-1) return -1;
    long off = (long)difftime(ul,ug);
#endif
    *out_sec = (int)off;
    return 0;
}

TM_API int tm_is_dst_local(time_t t){
    struct tm l;
    if (tm_localtime_safe(&t,&l)!=0) return -1;
    if (l.tm_isdst>0) return 1;
    if (l.tm_isdst==0) return 0;
    return -1;
}

/* ===================== Conversions ===================== */

TM_API int tm_from_utc_components(int Y,int M,int D,int h,int m,int s,time_t* out){
    if (!out) return -1;
    struct tm tmv; memset(&tmv,0,sizeof tmv);
    tmv.tm_year=Y-1900; tmv.tm_mon=M-1; tmv.tm_mday=D; tmv.tm_hour=h; tmv.tm_min=m; tmv.tm_sec=s;
#if defined(_WIN32)
  #if defined(_MSC_VER) || defined(__MINGW32__)
    time_t tutc = _mkgmtime(&tmv);
  #else
    time_t tloc = mktime(&tmv); long tzs=0; _get_timezone(&tzs); time_t tutc = tloc - tzs;
  #endif
#else
    time_t tutc = timegm(&tmv);
#endif
    if (tutc==(time_t)-1) return -1;
    *out = tutc; return 0;
}

TM_API int tm_from_local_components(int Y,int M,int D,int h,int m,int s,time_t* out){
    if (!out) return -1;
    struct tm tmv; memset(&tmv,0,sizeof tmv);
    tmv.tm_year=Y-1900; tmv.tm_mon=M-1; tmv.tm_mday=D; tmv.tm_hour=h; tmv.tm_min=m; tmv.tm_sec=s;
    time_t tloc = mktime(&tmv);
    if (tloc==(time_t)-1) return -1;
    *out = tloc; return 0;
}

/* ===================== Timespec helpers ===================== */

typedef struct timespec tm_timespec;

TM_API tm_timespec tm_timespec_add(tm_timespec a, tm_timespec b){
    tm_timespec r; r.tv_sec = a.tv_sec + b.tv_sec; r.tv_nsec = a.tv_nsec + b.tv_nsec;
    if (r.tv_nsec >= 1000000000L){ r.tv_sec++; r.tv_nsec -= 1000000000L; }
    return r;
}
TM_API tm_timespec tm_timespec_sub(tm_timespec a, tm_timespec b){
    tm_timespec r; r.tv_sec = a.tv_sec - b.tv_sec; r.tv_nsec = a.tv_nsec - b.tv_nsec;
    if (r.tv_nsec < 0){ r.tv_sec--; r.tv_nsec += 1000000000L; }
    return r;
}
TM_API uint64_t tm_timespec_ms(tm_timespec ts){
    return (uint64_t)ts.tv_sec*1000ull + (uint64_t)(ts.tv_nsec/1000000L);
}
TM_API uint64_t tm_diff_ms_mono(uint64_t t0_ms, uint64_t t1_ms){
    return (t1_ms>=t0_ms)? (t1_ms - t0_ms) : 0;
}

/* ===================== Date utils (local) ===================== */

TM_API time_t tm_start_of_day_local(time_t t){
    struct tm l; if (tm_localtime_safe(&t,&l)!=0) return (time_t)-1;
    l.tm_hour=0; l.tm_min=0; l.tm_sec=0; return mktime(&l);
}
TM_API time_t tm_end_of_day_local(time_t t){
    struct tm l; if (tm_localtime_safe(&t,&l)!=0) return (time_t)-1;
    l.tm_hour=23; l.tm_min=59; l.tm_sec=59; return mktime(&l);
}
TM_API time_t tm_add_days_local(time_t t, int days){
    struct tm l; if (tm_localtime_safe(&t,&l)!=0) return (time_t)-1;
    l.tm_mday += days; return mktime(&l);
}

/* ===================== Chrono simple ===================== */

typedef struct tm_stopwatch { uint64_t t0_ms; } tm_stopwatch;

TM_API void tm_sw_start(tm_stopwatch* sw){ if(!sw) return; sw->t0_ms = tm_mono_ms(); }
TM_API uint64_t tm_sw_elapsed_ms(const tm_stopwatch* sw){ if(!sw) return 0; return tm_diff_ms_mono(sw->t0_ms, tm_mono_ms()); }
TM_API uint64_t tm_sw_lap_ms(tm_stopwatch* sw){ if(!sw) return 0; uint64_t now=tm_mono_ms(); uint64_t d=tm_diff_ms_mono(sw->t0_ms, now); sw->t0_ms=now; return d; }

/* ===================== Tests ===================== */
#ifdef TM_TEST
int main(void){
    char buf[64];
    tm_iso8601_now(buf,sizeof buf,1); printf("now UTC: %s\n", buf);
    tm_format_iso8601_frac((time_t)tm_now_s(),0,3,buf,sizeof buf); printf("now LOC: %s\n", buf);

    int off=0; if (tm_local_utc_offset_sec(time(NULL),&off)==0) printf("offset=%d sec\n", off);
    printf("DST=%d\n", tm_is_dst_local(time(NULL)));

    const char* in[]={
        "2024-02-29",
        "2024-02-29T12:34",
        "2024-02-29T12:34:56Z",
        "2024-02-29T12:34:56.123456789+01:30",
        "2024-02-29 12:34:56-0330",
        NULL
    };
    for (int i=0; in[i]; i++){
        time_t t; int rc=tm_parse_iso8601(in[i],&t);
        if (!rc){ tm_format_iso8601(t,1,buf,sizeof buf); printf("%-35s -> %s\n", in[i], buf); }
        else    { printf("%-35s -> parse error\n", in[i]); }
    }

    uint64_t t0 = tm_mono_ms(); tm_sleep_ms(120); uint64_t t1 = tm_mono_ms();
    printf("slept ~%llums\n", (unsigned long long)tm_diff_ms_mono(t0,t1));

    tm_stopwatch sw; tm_sw_start(&sw); tm_sleep_ms(50);
    printf("sw elapsed=%llums\n", (unsigned long long)tm_sw_elapsed_ms(&sw));
    printf("sw lap=%llums\n", (unsigned long long)tm_sw_lap_ms(&sw));

    time_t d0 = time(NULL);
    time_t sod = tm_start_of_day_local(d0), eod = tm_end_of_day_local(d0);
    tm_format_iso8601(sod,0,buf,sizeof buf); printf("start of day: %s\n", buf);
    tm_format_iso8601(eod,0,buf,sizeof buf); printf("end   of day: %s\n", buf);
    time_t next = tm_add_days_local(d0, 1); tm_format_iso8601(next,0,buf,sizeof buf); printf("tomorrow: %s\n", buf);

    uint64_t deadline = tm_mono_ms() + 200; tm_sleep_until_mono_ms(deadline);
    printf("deadline reached\n");
    return 0;
}
#endif