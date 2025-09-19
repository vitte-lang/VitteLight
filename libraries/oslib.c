// SPDX-License-Identifier: GPL-3.0-or-later
//
// oslib.c — Utilitaires OS portables (C17)
// Namespace: "os"
//
// Plateformes: Linux/macOS (POSIX), Windows (Win32)
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c oslib.c
//
// Fournit:
//   Chemins   : os_path_sep, os_join, os_getcwd, os_homedir, os_tmpdir
//   Fichiers  : os_exists, os_isdir, os_filesize, os_mkdirs, os_read_file, os_write_file, os_write_file_atomic
//   Listing   : os_listdir(path, cb, user)   // cb(name, is_dir, user) -> int (0=continuer,!=0=stop)
//   Env       : os_env_get, os_env_set
//   Proc      : os_exec_capture(cmdline, out, cap, out_len, exit_code)
//   Système   : os_sleep_ms, os_cpu_count, os_time_now_ms
//
// Notes:
//   - Toutes les fonctions retournent 0 si succès, -1 si erreur (sauf si indiqué).
//   - os_read_file alloue *buf via malloc; à l’appelant de free().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <process.h>
  #define OS_PATH_SEP '\\'
  #define os_access _access
  #define os_mkdir_one(p,mode) _mkdir(p)
  #ifndef PATH_MAX
    #define PATH_MAX MAX_PATH
  #endif
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <dirent.h>
  #include <sys/wait.h>
  #define OS_PATH_SEP '/'
  #define os_access access
  #define os_mkdir_one(p,mode) mkdir(p, (mode_t)(mode))
  #ifndef PATH_MAX
    #define PATH_MAX 4096
  #endif
#endif

#ifndef OS_API
#define OS_API
#endif

/* ========================= Temps / Système ========================= */

OS_API void os_sleep_ms(unsigned ms){
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts; ts.tv_sec = ms/1000u; ts.tv_nsec = (long)(ms%1000u)*1000000L;
    nanosleep(&ts,NULL);
#endif
}

OS_API uint64_t os_time_now_ms(void){
#if defined(_WIN32)
    static LARGE_INTEGER f={0}; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart*1000ULL)/ (uint64_t)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000ULL + (uint64_t)(ts.tv_nsec/1000000ULL);
#endif
}

OS_API int os_cpu_count(void){
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n>0)? (int)n : 1;
#endif
}

/* ========================= Chemins ========================= */

OS_API char os_path_sep(void){ return OS_PATH_SEP; }

/* Concatène a + sep + b dans out. Gère les séparateurs en double. */
OS_API int os_join(char* out, size_t cap, const char* a, const char* b){
    if (!out || cap==0) return -1;
    out[0]=0;
    const char* A=a?a:"";
    const char* B=b?b:"";
    size_t na=strlen(A), nb=strlen(B);
    int need_sep = 0;
    if (na && nb){
        char ca = A[na-1], cb = B[0];
        if (ca!=OS_PATH_SEP && cb!=OS_PATH_SEP) need_sep=1;
        if (ca==OS_PATH_SEP || cb==OS_PATH_SEP) need_sep=0;
    }
    size_t need = na + nb + (need_sep?1:0) + 1;
    if (need > cap) return -1;
    memcpy(out, A, na);
    if (need_sep){ out[na]=OS_PATH_SEP; na++; }
    memcpy(out+na, B, nb);
    out[na+nb]=0;
    return 0;
}

OS_API int os_getcwd(char* out, size_t cap){
#if defined(_WIN32)
    DWORD n = GetCurrentDirectoryA((DWORD)cap, out);
    return (n>0 && n<cap)? 0 : -1;
#else
    return getcwd(out, cap)? 0 : -1;
#endif
}

OS_API int os_homedir(char* out, size_t cap){
#if defined(_WIN32)
    const char* h = getenv("USERPROFILE");
    if (!h) h = getenv("HOMEDRIVE");
    const char* p = getenv("HOMEPATH");
    if (h && p){
        char tmp[PATH_MAX];
        if (os_join(tmp,sizeof tmp,h,p)!=0) return -1;
        if (strlen(tmp)+1>cap) return -1;
        strcpy(out,tmp); return 0;
    }
    if (h){ if (strlen(h)+1>cap) return -1; strcpy(out,h); return 0; }
    return -1;
#else
    const char* h = getenv("HOME");
    if (!h) return -1;
    if (strlen(h)+1>cap) return -1;
    strcpy(out,h); return 0;
#endif
}

OS_API int os_tmpdir(char* out, size_t cap){
#if defined(_WIN32)
    DWORD n = GetTempPathA((DWORD)cap, out);
    return (n>0 && n<cap)? 0 : -1;
#else
    const char* t = getenv("TMPDIR"); if (!t) t="/tmp";
    size_t n = strlen(t); if (n+1>cap) return -1;
    strcpy(out,t); return 0;
#endif
}

/* ========================= Fichiers ========================= */

OS_API int os_exists(const char* path){
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return (a==INVALID_FILE_ATTRIBUTES)? 0 : 1;
#else
    return access(path, F_OK)==0;
#endif
}

OS_API int os_isdir(const char* path){
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return (a!=INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))? 1:0;
#else
    struct stat st; if (stat(path,&st)!=0) return 0;
    return S_ISDIR(st.st_mode)?1:0;
#endif
}

OS_API int os_filesize(const char* path, uint64_t* out){
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    ULARGE_INTEGER u; u.HighPart=fad.nFileSizeHigh; u.LowPart=fad.nFileSizeLow;
    if (out) *out = (uint64_t)u.QuadPart;
    return 0;
#else
    struct stat st; if (stat(path,&st)!=0) return -1;
    if (out) *out = (uint64_t)st.st_size;
    return 0;
#endif
}

/* mkpath récursif. mode ignoré sous Windows. */
OS_API int os_mkdirs(const char* path, unsigned mode){
    if (!path || !*path) return -1;
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n>=sizeof tmp) return -1;
    memcpy(tmp,path,n+1);
    for (size_t i=1;i<n;i++){
        if (tmp[i]==OS_PATH_SEP){
            tmp[i]=0;
            if (*tmp && !os_isdir(tmp)){
                if (os_mkdir_one(tmp, mode)!=0 && !os_isdir(tmp)) return -1;
            }
            tmp[i]=OS_PATH_SEP;
        }
    }
    if (!os_isdir(tmp)){
        if (os_mkdir_one(tmp, mode)!=0 && !os_isdir(tmp)) return -1;
    }
    return 0;
}

/* Lit tout le fichier. Alloue *buf. */
OS_API int os_read_file(const char* path, void** buf, size_t* len){
    if (!buf || !len) return -1;
    *buf=NULL; *len=0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f,0,SEEK_END)!=0){ fclose(f); return -1; }
    long sz = ftell(f);
    if (sz<0){ fclose(f); return -1; }
    rewind(f);
    void* p = malloc((size_t)sz + 1);
    if (!p){ fclose(f); return -1; }
    size_t rd = fread(p,1,(size_t)sz,f);
    fclose(f);
    if (rd!=(size_t)sz){ free(p); return -1; }
    ((char*)p)[sz]=0;
    *buf=p; *len=(size_t)sz; return 0;
}

OS_API int os_write_file(const char* path, const void* data, size_t len){
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t wr = data && len? fwrite(data,1,len,f) : 0;
    int rc = (wr==len)? 0 : -1;
    if (fclose(f)!=0) rc = -1;
    return rc;
}

/* Écriture atomique: écrit dans un fichier temporaire dans le même dossier puis rename. */
OS_API int os_write_file_atomic(const char* path, const void* data, size_t len){
    char dir[PATH_MAX], base[PATH_MAX];
    const char* slash = strrchr(path, OS_PATH_SEP);
    if (slash){
        size_t dn=(size_t)(slash - path);
        if (dn>=sizeof dir) return -1;
        memcpy(dir,path,dn); dir[dn]=0;
        snprintf(base,sizeof base,"%s", slash+1);
    } else {
        snprintf(dir,sizeof dir,".");
        snprintf(base,sizeof base,"%s", path);
    }
    if (os_mkdirs(dir, 0777)!=0) return -1;

    char tmp[PATH_MAX];
#if defined(_WIN32)
    snprintf(tmp,sizeof tmp,"%s%c.%s.%u.tmp", dir, OS_PATH_SEP, base, (unsigned)GetCurrentProcessId());
#else
    snprintf(tmp,sizeof tmp,"%s%c.%s.%u.tmp", dir, OS_PATH_SEP, base, (unsigned)getpid());
#endif
    if (os_write_file(tmp, data, len)!=0) return -1;

#if defined(_WIN32)
    /* Remplacement atomique si possible */
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)){
        DeleteFileA(tmp);
        return -1;
    }
#else
    if (rename(tmp, path)!=0){
        remove(tmp);
        return -1;
    }
#endif
    return 0;
}

/* ========================= Listing ========================= */

typedef int (*os_dir_cb)(const char* name, int is_dir, void* user);

OS_API int os_listdir(const char* path, os_dir_cb cb, void* u){
    if (!cb) return -1;
#if defined(_WIN32)
    char pat[PATH_MAX];
    if (os_join(pat,sizeof pat,path,"*")!=0) return -1;
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h==INVALID_HANDLE_VALUE) return -1;
    do{
        const char* n = fd.cFileName;
        if (strcmp(n,".")==0 || strcmp(n,"..")==0) continue;
        int isd = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)?1:0;
        if (cb(n, isd, u)) { FindClose(h); return 1; }
    }while(FindNextFileA(h,&fd));
    FindClose(h);
    return 0;
#else
    DIR* d = opendir(path?path:"."); if (!d) return -1;
    struct dirent* e;
    while ((e=readdir(d))!=NULL){
        const char* n = e->d_name;
        if (strcmp(n,".")==0 || strcmp(n,"..")==0) continue;
        int isd=0;
#if defined(_DIRENT_HAVE_D_TYPE)
        if (e->d_type==DT_DIR) isd=1;
        else if (e->d_type==DT_UNKNOWN){
            char full[PATH_MAX];
            if (os_join(full,sizeof full,path,n)==0) isd = os_isdir(full);
        } else isd=0;
#else
        char full[PATH_MAX]; if (os_join(full,sizeof full,path,n)==0) isd = os_isdir(full);
#endif
        if (cb(n, isd, u)){ closedir(d); return 1; }
    }
    closedir(d);
    return 0;
#endif
}

/* ========================= Environnement ========================= */

OS_API const char* os_env_get(const char* key, const char* defv){
#if defined(_WIN32)
    static char buf[32768];
    DWORD n = GetEnvironmentVariableA(key, buf, (DWORD)sizeof buf);
    if (n>0 && n<sizeof buf) return buf;
    (void)defv; return NULL;
#else
    const char* v = getenv(key);
    return v? v : defv;
#endif
}

OS_API int os_env_set(const char* key, const char* val){
#if defined(_WIN32)
    return SetEnvironmentVariableA(key, val)? 0 : -1;
#else
    return setenv(key, val?val:"", 1)==0? 0 : -1;
#endif
}

/* ========================= Processus ========================= */
/* Exécute une commande système, capture stdout (texte binaire). */
OS_API int os_exec_capture(const char* cmdline, char* out, size_t cap, size_t* out_len, int* exit_code){
    if (out_len) *out_len=0;
    if (exit_code) *exit_code=-1;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa={0}; sa.nLength=sizeof sa; sa.bInheritHandle=TRUE;
    HANDLE r=0,w=0;
    if (!CreatePipe(&r,&w,&sa,0)) return -1;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si; PROCESS_INFORMATION pi;
    ZeroMemory(&si,sizeof si); ZeroMemory(&pi,sizeof pi);
    si.cb=sizeof si; si.dwFlags=STARTF_USESTDHANDLES; si.hStdOutput=w; si.hStdError=w; si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);

    char* cl = _strdup(cmdline?cmdline:"");
    BOOL ok = CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(w);
    if (!ok){ CloseHandle(r); free(cl); return -1; }
    free(cl);

    size_t off=0;
    while (1){
        DWORD k=0; char buf[4096];
        if (!ReadFile(r, buf, sizeof buf, &k, NULL) || k==0) break;
        size_t can = (off + k <= cap)? k : (cap - off);
        if (can){ memcpy(out+off, buf, can); off += can; }
    }
    CloseHandle(r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec=0; GetExitCodeProcess(pi.hProcess,&ec);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (out_len) *out_len=off;
    if (cap) out[(off<cap)?off:cap-1]=0;
    if (exit_code) *exit_code=(int)ec;
    return 0;
#else
    int pfd[2]; if (pipe(pfd)!=0) return -1;
    pid_t pid = fork();
    if (pid<0){ close(pfd[0]); close(pfd[1]); return -1; }
    if (pid==0){
        /* child */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execl("/bin/sh","sh","-c", cmdline?cmdline:"", (char*)NULL);
        _exit(127);
    }
    close(pfd[1]);
    size_t off=0; char buf[4096];
    while (1){
        ssize_t k = read(pfd[0], buf, sizeof buf);
        if (k<=0) break;
        size_t can = (off + (size_t)k <= cap)? (size_t)k : (cap - off);
        if (can){ memcpy(out+off, buf, can); off += can; }
    }
    close(pfd[0]);
    int st=0; waitpid(pid,&st,0);
    if (out_len) *out_len=off;
    if (cap) out[(off<cap)?off:cap-1]=0;
    if (exit_code) *exit_code = WIFEXITED(st)? WEXITSTATUS(st): -1;
    return 0;
#endif
}

/* ========================= Test ========================= */
#ifdef OS_TEST
static int print_cb(const char* n, int isd, void* u){ (void)u; printf("%s%s\n", n, isd?"/":""); return 0; }
int main(void){
    char cwd[PATH_MAX]; os_getcwd(cwd,sizeof cwd); printf("cwd: %s\n", cwd);
    char home[PATH_MAX]; if (os_homedir(home,sizeof home)==0) printf("home: %s\n", home);
    char tmpd[PATH_MAX]; os_tmpdir(tmpd,sizeof tmpd); printf("tmp: %s\n", tmpd);

    char p[PATH_MAX]; os_join(p,sizeof p,tmpd,"vitte-test");
    os_mkdirs(p,0777);
    const char* msg="hello\n";
    os_write_file_atomic(os_join(p,sizeof p,p,"a.txt")==0?p:"/tmp/a.txt", msg, strlen(msg));

    puts("[list tmp]");
    os_listdir(tmpd, print_cb, NULL);

    char out[8192]; size_t n=0; int ec=0;
    os_exec_capture(
#if defined(_WIN32)
        "cmd /c echo hi",
#else
        "echo hi",
#endif
        out,sizeof out,&n,&ec);
    printf("exec(%d): %.*s", ec, (int)n, out);

    printf("cpus=%d now=%llu ms\n", os_cpu_count(), (unsigned long long)os_time_now_ms());
    return 0;
}
#endif