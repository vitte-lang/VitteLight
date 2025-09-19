// SPDX-License-Identifier: GPL-3.0-or-later
//
// metrics.c — Petites métriques portables (C17, sans dépendances)
// Namespace: "met"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c metrics.c
//
// Fournit :
//   Horloge: met_now_ns(), met_now_s(), met_sleep_ms()
//   Stopwatch: met_sw_start/stop/reset/elapsed_ns
//   Welford: met_welford_init/update/merge/mean/var/stdev/min/max/count
//   EWMA: met_ewma_init/update/value (alpha par période)
//   SMA (fenêtre fixe): met_sma_init/update/value
//   Compteur + taux: met_meter_init/mark/rates_1_5_15
//   Histogramme fixe (bornes linéaires ou log): met_hist_* + percentiles
//   Estimateur de quantiles P²: met_p2_init/update/quantile
//
// Notes:
//   - Tout est thread-unsafe par défaut. Protégez si nécessaire.
//   - Temps monotone: QPC (Windows) / clock_gettime(CLOCK_MONOTONIC) (POSIX).
//   - Percentiles histogramme: interpolation linéaire intra-bucket.
//   - P²: quantiles indépendants (même série d'échantillons requise pour comparer).

#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <processthreadsapi.h>
  #include <synchapi.h>
#else
  #include <time.h>
  #include <unistd.h>
#endif

#ifndef MET_API
#define MET_API
#endif

/* ======================= Horloge ======================= */

MET_API uint64_t met_now_ns(void){
#if defined(_WIN32)
    static LARGE_INTEGER f = {0};
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    long double s = (long double)c.QuadPart / (long double)f.QuadPart;
    return (uint64_t)(s * 1e9L);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
MET_API double met_now_s(void){ return (double)met_now_ns() * 1e-9; }

MET_API void met_sleep_ms(uint32_t ms){
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts; ts.tv_sec = ms/1000u; ts.tv_nsec = (long)(ms%1000u)*1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ======================= Stopwatch ======================= */

typedef struct { uint64_t t0, acc; int running; } met_stopwatch;

MET_API void met_sw_reset(met_stopwatch* s){ s->t0=0; s->acc=0; s->running=0; }
MET_API void met_sw_start(met_stopwatch* s){ if (!s->running){ s->running=1; s->t0=met_now_ns(); } }
MET_API void met_sw_stop (met_stopwatch* s){ if (s->running){ s->acc += met_now_ns()-s->t0; s->running=0; } }
MET_API uint64_t met_sw_elapsed_ns(const met_stopwatch* s){
    if (s->running){ uint64_t now=met_now_ns(); return s->acc + (now - s->t0); }
    return s->acc;
}

/* ======================= Welford (stats en ligne) ======================= */

typedef struct {
    uint64_t n;
    long double mean;
    long double m2;
    double min_v, max_v;
} met_welford;

MET_API void met_welford_init(met_welford* w){
    memset(w,0,sizeof *w);
    w->min_v = INFINITY; w->max_v = -INFINITY;
}
MET_API void met_welford_update(met_welford* w, double x){
    w->n++;
    long double dx = (long double)x - w->mean;
    w->mean += dx / (long double)w->n;
    w->m2   += dx * ((long double)x - w->mean);
    if (x < w->min_v) w->min_v = x;
    if (x > w->max_v) w->max_v = x;
}
MET_API void met_welford_merge(met_welford* a, const met_welford* b){
    if (b->n==0) return;
    if (a->n==0){ *a = *b; return; }
    long double n = (long double)a->n;
    long double m = (long double)b->n;
    long double delta = b->mean - a->mean;
    a->mean = (n*a->mean + m*b->mean) / (n+m);
    a->m2   = a->m2 + b->m2 + delta*delta*(n*m)/(n+m);
    a->n   += b->n;
    if (b->min_v < a->min_v) a->min_v=b->min_v;
    if (b->max_v > a->max_v) a->max_v=b->max_v;
}
MET_API uint64_t met_welford_count (const met_welford* w){ return w->n; }
MET_API double    met_welford_mean  (const met_welford* w){ return w->n? (double)w->mean : NAN; }
MET_API double    met_welford_var   (const met_welford* w){ return (w->n>1)? (double)(w->m2/((long double)(w->n-1))) : NAN; }
MET_API double    met_welford_stdev (const met_welford* w){ double v = met_welford_var(w); return isnan(v)?NAN:sqrt(v); }
MET_API double    met_welford_min   (const met_welford* w){ return w->n? w->min_v : NAN; }
MET_API double    met_welford_max   (const met_welford* w){ return w->n? w->max_v : NAN; }

/* ======================= EWMA (alpha par période) ======================= */

typedef struct {
    double alpha;     /* 0..1 */
    double value;     /* état courant */
    int initialized;
} met_ewma;

MET_API void met_ewma_init(met_ewma* e, double alpha){ e->alpha=alpha; e->value=0.0; e->initialized=0; }
MET_API void met_ewma_update(met_ewma* e, double x){
    if (!e->initialized){ e->value = x; e->initialized=1; }
    else e->value = e->alpha * x + (1.0 - e->alpha) * e->value;
}
MET_API double met_ewma_value(const met_ewma* e){ return e->initialized? e->value : NAN; }

/* ======================= SMA (fenêtre fixe) ======================= */

typedef struct {
    double* buf;
    size_t cap, n, i;
    double sum;
} met_sma;

MET_API int met_sma_init(met_sma* s, double* storage, size_t window){
    if (!storage || window==0) return -1;
    s->buf=storage; s->cap=window; s->n=0; s->i=0; s->sum=0.0;
    for (size_t k=0;k<window;k++) s->buf[k]=0.0;
    return 0;
}
MET_API void met_sma_update(met_sma* s, double x){
    if (s->n < s->cap){ s->buf[s->i]=x; s->sum+=x; s->n++; s->i=(s->i+1)%s->cap; }
    else { s->sum -= s->buf[s->i]; s->buf[s->i]=x; s->sum += x; s->i=(s->i+1)%s->cap; }
}
MET_API double met_sma_value(const met_sma* s){ return s->n? s->sum / (double)s->n : NAN; }

/* ======================= Meter (rates 1/5/15 min) ======================= */

typedef struct {
    uint64_t count;
    uint64_t last_ns;
    met_ewma m1, m5, m15;     /* EWMA par période de 1s: alpha = 1 - exp(-1/τ) avec τ = 60, 300, 900 s */
} met_meter;

static inline double _alpha_seconds(double seconds, double tau){ return 1.0 - exp(-seconds/tau); }

MET_API void met_meter_init(met_meter* m){
    m->count=0; m->last_ns=met_now_ns();
    met_ewma_init(&m->m1,  _alpha_seconds(1.0, 60.0));
    met_ewma_init(&m->m5,  _alpha_seconds(1.0, 300.0));
    met_ewma_init(&m->m15, _alpha_seconds(1.0, 900.0));
}
MET_API void met_meter_mark(met_meter* m, uint64_t n){
    uint64_t now = met_now_ns();
    double delta_s = (double)(now - m->last_ns) * 1e-9;
    if (delta_s <= 0.0) delta_s = 1e-9;
    double rate = (double)n / delta_s;
    met_ewma_update(&m->m1 , rate);
    met_ewma_update(&m->m5 , rate);
    met_ewma_update(&m->m15, rate);
    m->count += n;
    m->last_ns = now;
}
MET_API uint64_t met_meter_count(const met_meter* m){ return m->count; }
MET_API double met_meter_rate1 (const met_meter* m){ return met_ewma_value(&m->m1 ); }
MET_API double met_meter_rate5 (const met_meter* m){ return met_ewma_value(&m->m5 ); }
MET_API double met_meter_rate15(const met_meter* m){ return met_ewma_value(&m->m15); }

/* ======================= Histogramme fixe ======================= */

typedef struct {
    double min_edge, max_edge;
    int     nb;        /* nb buckets */
    int     under, over;
    int*    counts;    /* taille nb */
    int     logscale;  /* 0 linéaire, 1 log10 */
    uint64_t total;
} met_hist;

MET_API int met_hist_init_linear(met_hist* h, int* storage_counts, int nb, double min_edge, double max_edge){
    if (!h || !storage_counts || nb<=0 || !(max_edge>min_edge)) return -1;
    h->nb=nb; h->counts=storage_counts; h->min_edge=min_edge; h->max_edge=max_edge;
    h->logscale=0; h->under=0; h->over=0; h->total=0;
    for (int i=0;i<nb;i++) h->counts[i]=0;
    return 0;
}
MET_API int met_hist_init_log10(met_hist* h, int* storage_counts, int nb, double min_edge, double max_edge){
    if (min_edge<=0 || max_edge<=min_edge) return -1;
    int rc = met_hist_init_linear(h, storage_counts, nb, log10(min_edge), log10(max_edge));
    if (rc==0) h->logscale=1;
    return rc;
}
MET_API void met_hist_add(met_hist* h, double x){
    double v = h->logscale? log10((x<=0)?1e-30:x) : x;
    if (v < h->min_edge){ h->under++; h->total++; return; }
    if (v >= h->max_edge){ h->over++; h->total++; return; }
    double w = (h->max_edge - h->min_edge) / (double)h->nb;
    int idx = (int)((v - h->min_edge) / w);
    if (idx<0) idx=0; if (idx>=h->nb) idx=h->nb-1;
    h->counts[idx]++; h->total++;
}
MET_API uint64_t met_hist_total(const met_hist* h){ return h->total; }

/* Percentile par histogramme (approx). q in [0,1]. */
MET_API double met_hist_percentile(const met_hist* h, double q){
    if (h->total==0 || q<0.0 || q>1.0) return NAN;
    uint64_t rank = (uint64_t)ceil(q*(double)h->total);
    uint64_t acc = h->under;
    double w = (h->max_edge - h->min_edge) / (double)h->nb;
    for (int i=0;i<h->nb;i++){
        uint64_t next = acc + (uint64_t)h->counts[i];
        if (rank <= next){
            /* interpolation dans le bucket */
            double frac = (h->counts[i]>0) ? ((double)(rank - acc) / (double)h->counts[i]) : 0.0;
            double edge = h->min_edge + i*w + frac*w;
            return h->logscale? pow(10.0, edge) : edge;
        }
        acc = next;
    }
    return h->logscale? pow(10.0, h->max_edge) : h->max_edge;
}

/* ======================= P² quantiles (estimateur online) ======================= */
/* Implémente le schéma P² de Jain & Chlamtac pour un quantile p.
   Stocke 5 marqueurs; bonne précision sans garder l'historique. */

typedef struct {
    int initialized;
    double p;          /* quantile cible (0..1) */
    double q[5];       /* hauteurs */
    double n[5];       /* positions actuelles */
    double np[5];      /* positions désirées */
    double dn[5];      /* incréments désirés */
    uint64_t seen;
} met_p2;

MET_API int met_p2_init(met_p2* s, double p){
    if (!s || p<=0.0 || p>=1.0) return -1;
    memset(s,0,sizeof *s); s->p=p; return 0;
}
static inline double _p2_parabolic(double q0,double q1,double q2,double n0,double n1,double n2){
    double d1 = (n1 - n0), d2 = (n2 - n1);
    if (d1==0 || d2==0) return q1;
    return q1 + (( (n1 - n0 + (q2 - q1)/(q2 - q0) * (n2 - n1)) * (q1 - q0) ) / (n2 - n0));
}
static inline double _p2_linear(double qlo,double qhi,double nlo,double nhi){
    if (nhi==nlo) return qlo;
    return qlo + (qhi - qlo) / (nhi - nlo);
}
MET_API void met_p2_update(met_p2* s, double x){
    if (!s->initialized){
        /* collecter 5 premiers points triés */
        s->q[s->seen++] = x;
        if (s->seen<5) return;
        /* tri */
        for (int i=0;i<5;i++) for (int j=i+1;j<5;j++) if (s->q[j] < s->q[i]){ double t=s->q[i]; s->q[i]=s->q[j]; s->q[j]=t; }
        for (int i=0;i<5;i++) s->n[i]=i+1;
        s->np[0]=1; s->np[1]=1 + 2*s->p; s->np[2]=1 + 4*s->p; s->np[3]=3 + 2*s->p; s->np[4]=5;
        s->dn[0]=0; s->dn[1]=s->p/2; s->dn[2]=s->p; s->dn[3]=(1.0+s->p)/2; s->dn[4]=1;
        s->initialized=1; return;
    }
    /* mise à jour */
    int k;
    if (x < s->q[0]){ s->q[0]=x; k=0; }
    else if (x < s->q[1]) k=0;
    else if (x < s->q[2]) k=1;
    else if (x < s->q[3]) k=2;
    else if (x <= s->q[4]) k=3;
    else { s->q[4]=x; k=3; }
    for (int i=k+1;i<5;i++) s->n[i] += 1.0;
    for (int i=0;i<5;i++)  s->np[i] += s->dn[i];

    for (int i=1;i<=3;i++){
        double d = s->np[i] - s->n[i];
        if ((d >= 1.0 && s->n[i+1]-s->n[i] > 1.0) || (d <= -1.0 && s->n[i-1]-s->n[i] < -1.0)){
            double sign = (d>0)? 1.0 : -1.0;
            double qn = _p2_parabolic(s->q[i-1], s->q[i], s->q[i+1], s->n[i-1], s->n[i], s->n[i+1]);
            if (s->q[i-1] < qn && qn < s->q[i+1]) s->q[i] = qn;
            else s->q[i] = _p2_linear(s->q[i-(int)sign], s->q[i+(int)sign], s->n[i-(int)sign], s->n[i+(int)sign]);
            s->n[i] += sign;
        }
    }
    s->seen++;
}
MET_API double met_p2_quantile(const met_p2* s){
    if (!s->initialized) return NAN;
    return s->q[2];
}

/* ======================= Utilitaires texte ======================= */

MET_API void met_hist_dump(const met_hist* h, FILE* out){
    if (!out || !h) return;
    double w = (h->max_edge - h->min_edge) / (double)h->nb;
    fprintf(out,"# total=%llu under=%d over=%d nb=%d %s\n",
        (unsigned long long)h->total, h->under, h->over, h->nb, h->logscale?"log10":"linear");
    for (int i=0;i<h->nb;i++){
        double a=h->min_edge + i*w, b=a+w;
        if (h->logscale){ a=pow(10.0,a); b=pow(10.0,b); }
        fprintf(out,"[%g,%g)%*s%d\n", a,b, (int)(20 - (i<10?1:2)), "", h->counts[i]);
    }
}

/* ======================= Test ======================= */
#ifdef MET_TEST
int main(void){
    /* Stopwatch */
    met_stopwatch sw; met_sw_reset(&sw); met_sw_start(&sw); met_sleep_ms(50); met_sw_stop(&sw);
    printf("stopwatch_ns=%llu\n", (unsigned long long)met_sw_elapsed_ns(&sw));

    /* Welford */
    met_welford w; met_welford_init(&w);
    for (int i=1;i<=5;i++) met_welford_update(&w, (double)i);
    printf("n=%llu mean=%.3f stdev=%.3f min=%.0f max=%.0f\n",
        (unsigned long long)met_welford_count(&w), met_welford_mean(&w), met_welford_stdev(&w), met_welford_min(&w), met_welford_max(&w));

    /* EWMA & meter */
    met_meter m; met_meter_init(&m);
    for (int k=0;k<5;k++){ met_sleep_ms(200); met_meter_mark(&m, 100); }
    printf("count=%llu r1=%.2f r5=%.2f r15=%.2f\n",
        (unsigned long long)met_meter_count(&m), met_meter_rate1(&m), met_meter_rate5(&m), met_meter_rate15(&m));

    /* Histogram */
    int buckets[20]; met_hist h; met_hist_init_linear(&h, buckets, 20, 0.0, 100.0);
    for (int i=0;i<1000;i++) met_hist_add(&h, (double)(i%100));
    printf("p50=%.2f p95=%.2f p99=%.2f\n", met_hist_percentile(&h,0.5), met_hist_percentile(&h,0.95), met_hist_percentile(&h,0.99));

    /* P² quantile */
    met_p2 q95; met_p2_init(&q95, 0.95);
    for (int i=1;i<=1000;i++) met_p2_update(&q95, (double)(i%100));
    printf("p2_95=%.2f\n", met_p2_quantile(&q95));
    return 0;
}
#endif