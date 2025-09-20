// SPDX-License-Identifier: GPL-3.0-or-later
//
// proc.c — Lancement de processus portable (C17)
// Namespace: "proc"
//
// Objectif:
//   - Créer un processus enfant avec pipes stdin/stdout (+stderr optionnel).
//   - Écrire sur stdin, lire stdout (blocant avec timeout).
//   - Attendre la fin, récupérer le code de retour.
//   - Tuer et nettoyer.
//
// Limitations:
//   - Pas de tableau argv/env détaillé: on passe une cmdline.
//   - Env hérité. CWD optionnel.
//   - Flux texte/binaires non transformés.
//   - Lecture stdout+stderr combinés si merge_err!=0.
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c proc.c
//
// Test (POSIX):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DPROC_TEST proc.c && ./a.out

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
#else
  #include <unistd.h>
  #include <errno.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <signal.h>
  #include <fcntl.h>
  #include <time.h>
  #include <sys/select.h>
#endif

#ifndef PROC_API
#define PROC_API
#endif

typedef struct {
#if defined(_WIN32)
    PROCESS_INFORMATION pi;
    HANDLE hin, hout; /* parent-side handles: write to hin, read from hout */
#else
    pid_t pid;
    int   in_w;   /* parent -> child stdin */
    int   out_r;  /* parent <- child stdout (+stderr si fusion) */
#endif
    int merge_err;
} proc_t;

/* ====================== Utils ====================== */

static void ms_sleep(int ms){
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts={ms/1000,(ms%1000)*1000000L};
    nanosleep(&ts,NULL);
#endif
}

/* ====================== API ====================== */

/* Lance un processus.
   cmdline : commande brute, interprétée par le shell système.
   cwd     : répertoire de travail ou NULL.
   merge_err : si !=0, redirige stderr vers stdout.
   Retour: 0 ok, -1 échec. Remplit *p. */
PROC_API int proc_spawn(proc_t* p, const char* cmdline, const char* cwd, int merge_err){
    if (!p || !cmdline) return -1;
    memset(p,0,sizeof *p);
    p->merge_err = merge_err?1:0;

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa={0}; sa.nLength=sizeof sa; sa.bInheritHandle=TRUE;

    HANDLE rOut=0,wOut=0, rIn=0,wIn=0;
    if (!CreatePipe(&rOut,&wOut,&sa,0)) return -1;
    if (!SetHandleInformation(rOut, HANDLE_FLAG_INHERIT, 0)){ CloseHandle(rOut); CloseHandle(wOut); return -1; }
    if (!CreatePipe(&rIn,&wIn,&sa,0)){ CloseHandle(rOut); CloseHandle(wOut); return -1; }
    if (!SetHandleInformation(wIn, HANDLE_FLAG_INHERIT, 0)){ CloseHandle(rOut); CloseHandle(wOut); CloseHandle(rIn); CloseHandle(wIn); return -1; }

    STARTUPINFOA si; ZeroMemory(&si,sizeof si); si.cb=sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = rIn;
    si.hStdOutput = wOut;
    si.hStdError  = merge_err ? wOut : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi; ZeroMemory(&pi,sizeof pi);

    /* Dupliquer cmdline car CreateProcessA peut la modifier */
    char* cl = _strdup(cmdline);
    BOOL ok = CreateProcessA(
        NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
        cwd ? cwd : NULL, &si, &pi);
    free(cl);

    /* Handles côté parent */
    CloseHandle(rIn);  /* parent écrit via wIn */
    CloseHandle(wOut); /* parent lit via rOut */

    if (!ok){
        CloseHandle(rOut); CloseHandle(wIn);
        return -1;
    }

    p->pi = pi;
    p->hout = rOut;
    p->hin  = wIn;

    /* Mode non bloquant pour lecture (optionnel sous Win, on garde blocant + timeouts gérés par ReadFile avec Peek) */
    return 0;

#else
    int in_pipe[2]={-1,-1};
    int out_pipe[2]={-1,-1};
    if (pipe(in_pipe)!=0) return -1;
    if (pipe(out_pipe)!=0){ close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid<0){
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    if (pid==0){
        /* Enfant */
        /* stdin <- in_pipe[0] ; stdout -> out_pipe[1] ; stderr -> out_pipe[1] si merge */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        if (merge_err) dup2(out_pipe[1], STDERR_FILENO);

        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        if (cwd && *cwd) chdir(cwd);

        execl("/bin/sh","sh","-c", cmdline, (char*)NULL);
        _exit(127);
    }
    /* Parent */
    close(in_pipe[0]);  /* garde in_pipe[1] pour écrire */
    close(out_pipe[1]); /* garde out_pipe[0] pour lire */

    /* mettre lecture non bloquante pour select */
    int fl = fcntl(out_pipe[0], F_GETFL, 0); if (fl>=0) fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);

    p->pid  = pid;
    p->in_w = in_pipe[1];
    p->out_r= out_pipe[0];
    return 0;
#endif
}

/* Écrit dans stdin du processus. Retourne octets écrits, -1 sinon. */
PROC_API int proc_write(proc_t* p, const void* buf, size_t n){
    if (!p || !buf || n==0) return 0;
#if defined(_WIN32)
    DWORD k=0;
    if (!WriteFile(p->hin, buf, (DWORD)n, &k, NULL)) return -1;
    return (int)k;
#else
    ssize_t k = write(p->in_w, buf, n);
    return (k<0)? -1 : (int)k;
#endif
}

/* Ferme stdin du processus (signal EOF). */
PROC_API void proc_close_stdin(proc_t* p){
    if (!p) return;
#if defined(_WIN32)
    if (p->hin){ CloseHandle(p->hin); p->hin=NULL; }
#else
    if (p->in_w>=0){ close(p->in_w); p->in_w=-1; }
#endif
}

/* Lit stdout (et stderr si fusion) avec timeout_ms.
   Retour: >0 octets lus, 0 si timeout, -1 si erreur/EOF fermé et rien lu. */
PROC_API int proc_read(proc_t* p, void* buf, size_t cap, int timeout_ms){
    if (!p || !buf || cap==0) return -1;
#if defined(_WIN32)
    DWORD avail=0;
    /* Sonde disponibilité, sinon attend petit pas jusqu'au timeout */
    int waited=0;
    while (1){
        if (PeekNamedPipe(p->hout, NULL, 0, NULL, &avail, NULL)){
            if (avail>0){
                DWORD k=0; if (!ReadFile(p->hout, buf, (DWORD)((cap<avail)?cap:avail), &k, NULL)) return -1;
                return (int)k;
            }
        } else {
            /* pipe fermé ou erreur */
            return -1;
        }
        if (timeout_ms==0) return 0;
        if (timeout_ms>0 && waited>=timeout_ms) return 0;
        ms_sleep(10); waited += 10;
    }
#else
    fd_set rf; FD_ZERO(&rf); FD_SET(p->out_r,&rf);
    struct timeval tv, *ptv=NULL;
    if (timeout_ms>=0){ tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000; ptv=&tv; }
    int rc = select(p->out_r+1, &rf, NULL, NULL, ptv);
    if (rc<0){
        if (errno==EINTR) return 0;
        return -1;
    }
    if (rc==0) return 0; /* timeout */
    ssize_t k = read(p->out_r, buf, cap);
    if (k<=0) return -1;
    return (int)k;
#endif
}

/* Attente de fin. timeout_ms<0 = infini. Retour:
   0 si terminé (exit_code rempli), 1 si timeout, -1 si erreur. */
PROC_API int proc_wait(proc_t* p, int timeout_ms, int* exit_code){
    if (!p) return -1;
#if defined(_WIN32)
    DWORD rc = WaitForSingleObject(p->pi.hProcess, (timeout_ms<0)?INFINITE:(DWORD)timeout_ms);
    if (rc==WAIT_TIMEOUT) return 1;
    if (rc!=WAIT_OBJECT_0) return -1;
    DWORD ec=0; GetExitCodeProcess(p->pi.hProcess, &ec);
    if (exit_code) *exit_code=(int)ec;
    return 0;
#else
    int elapsed=0;
    while (1){
        int st=0;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r==p->pid){
            if (exit_code){
                if (WIFEXITED(st)) *exit_code = WEXITSTATUS(st);
                else if (WIFSIGNALED(st)) *exit_code = 128 + WTERMSIG(st);
                else *exit_code = -1;
            }
            return 0;
        }
        if (r<0){
            if (errno==EINTR) continue;
            return -1;
        }
        if (timeout_ms==0) return 1;
        if (timeout_ms>0 && elapsed>=timeout_ms) return 1;
        ms_sleep(10); elapsed+=10;
    }
#endif
}

/* Tue le processus (SIGKILL / TerminateProcess). */
PROC_API int proc_kill(proc_t* p){
    if (!p) return -1;
#if defined(_WIN32)
    return TerminateProcess(p->pi.hProcess, 137) ? 0 : -1;
#else
    return kill(p->pid, SIGKILL)==0 ? 0 : -1;
#endif
}

/* Libère ressources. À appeler après fin du processus. */
PROC_API void proc_close(proc_t* p){
    if (!p) return;
#if defined(_WIN32)
    if (p->hin)  CloseHandle(p->hin),  p->hin=NULL;
    if (p->hout) CloseHandle(p->hout), p->hout=NULL;
    if (p->pi.hThread)  CloseHandle(p->pi.hThread), p->pi.hThread=NULL;
    if (p->pi.hProcess) CloseHandle(p->pi.hProcess),p->pi.hProcess=NULL;
#else
    if (p->in_w>=0)  close(p->in_w),  p->in_w=-1;
    if (p->out_r>=0) close(p->out_r), p->out_r=-1;
#endif
}

/* ====================== Test ====================== */
#ifdef PROC_TEST
int main(void){
    proc_t pr;
#if defined(_WIN32)
    const char* cmd = "cmd /c for /L %i in (1,1,5) do @echo line %i& @ping -n 2 127.0.0.1 >nul";
#else
    const char* cmd = "for i in 1 2 3 4 5; do echo line $i; sleep 0.2; done";
#endif
    if (proc_spawn(&pr, cmd, NULL, 1)!=0){ fprintf(stderr,"spawn fail\n"); return 1; }

    char buf[256];
    for (;;){
        int k = proc_read(&pr, buf, sizeof buf-1, 500);
        if (k<0) break;        /* EOF */
        if (k==0) {
            int ec;
            int w = proc_wait(&pr, 0, &ec);
            if (w==0){ printf("[exit %d]\n", ec); break; }
            continue;          /* timeout, boucle */
        }
        buf[k]=0; fputs(buf, stdout);
    }

    proc_close(&pr);
    return 0;
}
#endif