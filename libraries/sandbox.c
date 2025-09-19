// SPDX-License-Identifier: GPL-3.0-or-later
//
// sandbox.c — Exécution “sandboxée” d’une commande (POSIX, C17)
// Namespace: "sbox"
//
// Objectifs:
//   - Lancer une commande dans un sous-processus avec limites de ressources.
//   - Capturer stdout/stderr en mémoire (tailles plafonnées).
//   - Timeout mur (wall clock). Kill du groupe si dépassement.
//   - Retour du code de sortie ou du signal, métriques simples.
//
// Portabilité:
//   - POSIX uniquement (Linux/macOS/BSD). Windows: non supporté (stubs).
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c sandbox.c
//
// Démo:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DSANDBOX_TEST sandbox.c && ./a.out
//

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)

/* ===== Stubs Windows ===== */
#ifndef SBOX_API
#define SBOX_API
#endif

typedef struct {
    const char* cmdline;     /* interprété par le shell */
    const char* workdir;     /* NULL => inchangé */
    uint32_t    time_limit_ms;   /* 0 => illimité */
    uint64_t    mem_limit_bytes; /* 0 => illimité (adresse) */
    uint64_t    file_size_limit; /* 0 => illimité */
    size_t      max_stdout;      /* plafond capture */
    size_t      max_stderr;      /* plafond capture */
} sbox_opts;

typedef struct {
    int started;
    int exited;
    int exit_code;        /* si exited && !signaled */
    int signaled;         /* 1 si tué par signal (toujours 0 sous Windows) */
    int term_signal;      /* n/a */
    int timed_out;        /* 1 si timeout mur dépassé */
    uint64_t wall_ms;
    char* out; size_t out_len;
    char* err; size_t err_len;
} sbox_result;

SBOX_API int sbox_run(const sbox_opts* opts, sbox_result* res){
    (void)opts; if(res){ memset(res,0,sizeof *res); } return -1; /* non supporté */
}
SBOX_API void sbox_free_result(sbox_result* r){
    if(!r) return; free(r->out); free(r->err); r->out=r->err=NULL; r->out_len=r->err_len=0;
}

#else /* POSIX */

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#ifndef SBOX_API
#define SBOX_API
#endif

/* ===================== API ===================== */

typedef struct {
    const char* cmdline;     /* interprété par /bin/sh -c */
    const char* workdir;     /* NULL => inchangé */
    uint32_t    time_limit_ms;   /* timeout mur. 0 => illimité */
    uint64_t    mem_limit_bytes; /* RLIMIT_AS. 0 => illimité */
    uint64_t    file_size_limit; /* RLIMIT_FSIZE. 0 => illimité */
    size_t      max_stdout;      /* octets max à conserver */
    size_t      max_stderr;      /* octets max à conserver */
} sbox_opts;

typedef struct {
    int started;
    int exited;
    int exit_code;        /* si exited && !signaled */
    int signaled;         /* 1 si tué par signal */
    int term_signal;      /* signal de terminaison (ex: SIGKILL) */
    int timed_out;        /* 1 si timeout mur dépassé */
    uint64_t wall_ms;
    char* out; size_t out_len;
    char* err; size_t err_len;
} sbox_result;

/* ===================== Utils ===================== */

static uint64_t _now_ms(void){
#if defined(CLOCK_MONOTONIC)
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
#endif
    return (uint64_t)ts.tv_sec*1000ull + (uint64_t)ts.tv_nsec/1000000ull;
}

static int _set_nonblock(int fd){
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl<0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int _grow_buf(char** buf, size_t* cap, size_t need){
    if (*cap >= need) return 0;
    size_t newc = (*cap==0)? 4096 : *cap;
    while (newc < need) newc *= 2;
    char* nb = (char*)realloc(*buf, newc);
    if (!nb) return -1;
    *buf = nb; *cap = newc; return 0;
}

/* ===================== Run ===================== */

SBOX_API int sbox_run(const sbox_opts* opts, sbox_result* res){
    if (!opts || !opts->cmdline || !res) return -1;
    memset(res, 0, sizeof *res);
    res->started = 1;

    int pout[2]={-1,-1}, perr[2]={-1,-1};
    if (pipe(pout)!=0) return -1;
    if (pipe(perr)!=0){ close(pout[0]); close(pout[1]); return -1; }

    pid_t pid = fork();
    if (pid<0){
        close(pout[0]); close(pout[1]); close(perr[0]); close(perr[1]); return -1;
    }
    if (pid==0){
        /* Child */
        /* nouveau groupe pour pouvoir kill -pgid */
        setpgid(0,0);

        /* apply rlimits avant exec */
        if (opts->mem_limit_bytes){
            struct rlimit rl = { .rlim_cur = (rlim_t)opts->mem_limit_bytes, .rlim_max = (rlim_t)opts->mem_limit_bytes };
            setrlimit(RLIMIT_AS, &rl);
        }
        if (opts->file_size_limit){
            struct rlimit rl = { .rlim_cur = (rlim_t)opts->file_size_limit, .rlim_max = (rlim_t)opts->file_size_limit };
            setrlimit(RLIMIT_FSIZE, &rl);
        }
        /* Limite CPU “logicielle”: peut compléter le wall timeout */
        /* Exemple: 2x le timeout mur en secondes si défini */
        if (opts->time_limit_ms){
            rlim_t sec = (rlim_t)((opts->time_limit_ms+999)/1000)*2 + 1;
            struct rlimit rl = { .rlim_cur = sec, .rlim_max = sec };
            setrlimit(RLIMIT_CPU, &rl);
        }

        /* redirections */
        close(pout[0]); close(perr[0]);
        dup2(pout[1], STDOUT_FILENO);
        dup2(perr[1], STDERR_FILENO);
        /* stdin: /dev/null */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull>=0){ dup2(devnull, STDIN_FILENO); close(devnull); }

        /* chdir éventuel */
        if (opts->workdir && *opts->workdir) (void)chdir(opts->workdir);

        /* cloexec off sur stdio, fermer les autres */
        close(pout[1]); close(perr[1]);

        execl("/bin/sh","sh","-c", opts->cmdline, (char*)NULL);
        _exit(127);
    }

    /* Parent */
    close(pout[1]); close(perr[1]);

    _set_nonblock(pout[0]);
    _set_nonblock(perr[0]);

    char *o_buf=NULL,*e_buf=NULL; size_t o_len=0,e_len=0, o_cap=0,e_cap=0;
    const size_t o_max = opts->max_stdout?opts->max_stdout:(size_t)1<<20; /* 1 MiB par défaut */
    const size_t e_max = opts->max_stderr?opts->max_stderr:(size_t)1<<20;

    uint64_t t0 = _now_ms();
    int done = 0;
    int timed_out = 0;

    while (!done){
        /* Timeout mur */
        if (opts->time_limit_ms){
            uint64_t now = _now_ms();
            if (now - t0 >= opts->time_limit_ms){
                timed_out = 1;
                /* tuer le groupe */
                kill(-pid, SIGKILL);
            }
        }

        /* Lire tout ce qui est dispo sur stdout/stderr */
        fd_set rf; FD_ZERO(&rf);
        int nfds = 0;
        if (pout[0]>=0){ FD_SET(pout[0], &rf); if (pout[0]>nfds) nfds=pout[0]; }
        if (perr[0]>=0){ FD_SET(perr[0], &rf); if (perr[0]>nfds) nfds=perr[0]; }

        struct timeval tv; tv.tv_sec=0; tv.tv_usec=100*1000; /* 100ms */
        int sel = select(nfds+1, &rf, NULL, NULL, &tv);
        if (sel>=0){
            if (pout[0]>=0 && FD_ISSET(pout[0], &rf)){
                char tmp[4096];
                ssize_t k = read(pout[0], tmp, sizeof tmp);
                if (k>0){
                    size_t to_add=(size_t)k;
                    if (o_len + to_add > o_max) to_add = (o_max>o_len)? (o_max-o_len):0;
                    if (to_add){
                        if (_grow_buf(&o_buf, &o_cap, o_len+to_add+1)!=0) { /* OOM: stop capturing further */ }
                        else { memcpy(o_buf+o_len, tmp, to_add); o_len += to_add; o_buf[o_len]=0; }
                    }
                } else if (k==0){ close(pout[0]); pout[0]=-1; }
                else if (errno!=EAGAIN && errno!=EWOULDBLOCK){ close(pout[0]); pout[0]=-1; }
            }
            if (perr[0]>=0 && FD_ISSET(perr[0], &rf)){
                char tmp[4096];
                ssize_t k = read(perr[0], tmp, sizeof tmp);
                if (k>0){
                    size_t to_add=(size_t)k;
                    if (e_len + to_add > e_max) to_add = (e_max>e_len)? (e_max-e_len):0;
                    if (to_add){
                        if (_grow_buf(&e_buf, &e_cap, e_len+to_add+1)!=0) { /* OOM */ }
                        else { memcpy(e_buf+e_len, tmp, to_add); e_len += to_add; e_buf[e_len]=0; }
                    }
                } else if (k==0){ close(perr[0]); perr[0]=-1; }
                else if (errno!=EAGAIN && errno!=EWOULDBLOCK){ close(perr[0]); perr[0]=-1; }
            }
        }

        /* Vérifier l’état du processus */
        int st=0; pid_t r = waitpid(pid, &st, WNOHANG);
        if (r==pid){
            res->exited = 1;
            if (WIFEXITED(st)){ res->exit_code = WEXITSTATUS(st); res->signaled=0; res->term_signal=0; }
            else if (WIFSIGNALED(st)){ res->signaled=1; res->term_signal=WTERMSIG(st); }
            done = 1;
        } else if (r<0){
            if (errno!=EINTR){ done=1; }
        }

        /* Si pipes fermés et process terminé -> fin */
        if ((pout[0]<0 && perr[0]<0) && res->exited) done=1;
    }

    if (pout[0]>=0) close(pout[0]);
    if (perr[0]>=0) close(perr[0]);

    res->timed_out = timed_out;
    res->wall_ms = _now_ms() - t0;
    res->out = o_buf; res->out_len=o_len;
    res->err = e_buf; res->err_len=e_len;
    return 0;
}

SBOX_API void sbox_free_result(sbox_result* r){
    if (!r) return;
    free(r->out); free(r->err);
    r->out=r->err=NULL; r->out_len=r->err_len=0;
}

#endif /* POSIX */

/* ===================== Test ===================== */
#ifdef SANDBOX_TEST
int main(void){
#if defined(_WIN32)
    puts("sandbox is POSIX-only in this build"); return 0;
#else
    sbox_opts o = {
        .cmdline = "echo hello; echo err >&2; sleep 1; echo done",
        .workdir = NULL,
        .time_limit_ms = 1500,
        .mem_limit_bytes = 256ull<<20, /* 256 MiB */
        .file_size_limit = 1ull<<20,   /* 1 MiB */
        .max_stdout = 64<<10,
        .max_stderr = 64<<10
    };
    sbox_result r;
    if (sbox_run(&o,&r)!=0){ fprintf(stderr,"sbox_run failed\n"); return 1; }
    printf("exited=%d code=%d signaled=%d sig=%d timeout=%d wall=%llums\n",
        r.exited, r.exit_code, r.signaled, r.term_signal, r.timed_out,
        (unsigned long long)r.wall_ms);
    printf("stdout(%zu):\n%.*s", r.out_len, (int)r.out_len, r.out?r.out:"");
    printf("stderr(%zu):\n%.*s", r.err_len, (int)r.err_len, r.err?r.err:"");
    sbox_free_result(&r);

    /* Test timeout */
    o.cmdline="sleep 2; echo never";
    o.time_limit_ms=500;
    if (sbox_run(&o,&r)==0){
        printf("\n[timeout test] timeout=%d wall=%llums\n", r.timed_out, (unsigned long long)r.wall_ms);
        sbox_free_result(&r);
    }
    return 0;
#endif
}
#endif