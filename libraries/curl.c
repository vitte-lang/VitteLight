// SPDX-License-Identifier: GPL-3.0-or-later
//
// curl.c — Client HTTP minimal pour Vitte Light (C17, portable)
// Namespace: "http"
//
// Build:
//   Avec libcurl (recommandé):
//     cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_LIBCURL curl.c -lcurl -c
//   Sans libcurl (fallback via binaire `curl`):
//     cc -std=c17 -O2 -Wall -Wextra -pedantic -c curl.c
//
// API:
//   // Erreur thread-locale (NULL si aucune):
//   const char* http_err(void);
//
//   // GET dans mémoire (alloue, à libérer par free()):
//   int http_get(const char* url,
//                const char* headers_csv,   // ex: "Accept: application/json,Authorization: Bearer xyz"
//                long timeout_ms,           // 0 = défaut (30s)
//                char** out_data, size_t* out_size);
//
//   // POST binaire dans mémoire (Content-Type optionnel, ex: "application/json"):
//   int http_post(const char* url,
//                 const void* body, size_t body_len, const char* content_type,
//                 const char* headers_csv,
//                 long timeout_ms,
//                 char** out_data, size_t* out_size);
//
//   // Téléchargement direct vers fichier:
//   int http_download_file(const char* url, const char* out_path, long timeout_ms);
//
// Notes:
//   - User-Agent par défaut: "VitteLight/0.1".
//   - Vérification TLS activée (fallback CLI: --proto-default https, --fail, --tlsv1.2).
//   - headers_csv: liste séparée par virgules. Espaces tolérés.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
  #define HTTP_THREAD_LOCAL __declspec(thread)
  #define PATH_SEP '\\'
#else
  #define HTTP_THREAD_LOCAL __thread
  #define PATH_SEP '/'
  #include <unistd.h>
#endif

#ifndef HTTP_DEFAULT_TIMEOUT_MS
#define HTTP_DEFAULT_TIMEOUT_MS 30000L
#endif

static HTTP_THREAD_LOCAL char g_http_err[256];
static void http_set_err(const char* s){
    if (!s) { g_http_err[0] = '\0'; return; }
    snprintf(g_http_err, sizeof(g_http_err), "%s", s);
}
const char* http_err(void){ return g_http_err[0] ? g_http_err : NULL; }

// -------- util mémoire --------
typedef struct { char* p; size_t n, cap; } mem_t;
static int mem_init(mem_t* m){ m->p=NULL; m->n=0; m->cap=0; return 0; }
static int mem_reserve(mem_t* m, size_t more){
    size_t need = m->n + more + 1;
    if (need <= m->cap) return 0;
    size_t ncap = m->cap? m->cap*2 : 4096;
    while (ncap < need) ncap *= 2;
    char* np = (char*)realloc(m->p, ncap);
    if (!np) return -1;
    m->p=np; m->cap=ncap; return 0;
}
static int mem_append(mem_t* m, const void* buf, size_t len){
    if (len==0) return 0;
    if (mem_reserve(m, len)!=0) return -1;
    memcpy(m->p + m->n, buf, len);
    m->n += len; m->p[m->n] = '\0';
    return 0;
}
static void mem_move_out(mem_t* m, char** out, size_t* out_sz){
    if (out) *out = m->p; else free(m->p);
    if (out_sz) *out_sz = m->n;
    m->p=NULL; m->n=0; m->cap=0;
}

// -------- parser headers_csv simple --------
static char** split_headers(const char* csv, size_t* out_count){
    *out_count = 0;
    if (!csv || !*csv) return NULL;
    // Compter virgules
    size_t cnt = 1;
    for (const char* p=csv; *p; ++p) if (*p==',') cnt++;
    char** arr = (char**)calloc(cnt, sizeof(char*));
    if (!arr) return NULL;
    const char* s = csv;
    size_t i=0;
    while (*s && i<cnt) {
        const char* e = strchr(s, ',');
        size_t len = e? (size_t)(e - s) : strlen(s);
        // trim espaces
        while (len && (*s==' '||*s=='\t')) { s++; len--; }
        while (len && (s[len-1]==' '||s[len-1]=='\t')) len--;
        if (len) {
            arr[i] = (char*)malloc(len+1);
            if (!arr[i]) { // free partiel
                for (size_t k=0;k<i;k++) free(arr[k]);
                free(arr); return NULL;
            }
            memcpy(arr[i], s, len); arr[i][len]='\0';
            i++;
        }
        if (!e) break;
        s = e+1;
    }
    *out_count = i;
    return arr;
}
static void free_headers(char** arr, size_t n){ if(!arr) return; for(size_t i=0;i<n;i++) free(arr[i]); free(arr); }

// =====================================================================================
// = libcurl ==========================================================================
// =====================================================================================
#if defined(VL_HAVE_LIBCURL)
#include <curl/curl.h>

static size_t write_cb(void* ptr, size_t sz, size_t nm, void* userdata){
    mem_t* m = (mem_t*)userdata;
    size_t n = sz*nm;
    return mem_append(m, ptr, n)==0 ? n : 0;
}

static CURL* easy_common(const char* url, long timeout_ms, struct curl_slist** hdrs, const char* headers_csv){
    CURL* h = curl_easy_init();
    if (!h) { http_set_err("curl_easy_init failed"); return NULL; }
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "VitteLight/0.1");
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); // autorise gzip/deflate
    long to = timeout_ms > 0 ? timeout_ms : HTTP_DEFAULT_TIMEOUT_MS;
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, to);

    // headers optionnels
    if (headers_csv && *headers_csv) {
        size_t n=0; char** H = split_headers(headers_csv, &n);
        for (size_t i=0;i<n;i++) *hdrs = curl_slist_append(*hdrs, H[i]);
        free_headers(H, n);
        if (*hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, *hdrs);
    }
    return h;
}

static int perform_to_mem(CURL* h, mem_t* m){
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, m);
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        http_set_err(curl_easy_strerror(rc));
        return -1;
    }
    long code=0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    if (code >= 400) { http_set_err("HTTP error"); return -1; }
    return 0;
}

int http_get(const char* url, const char* headers_csv, long timeout_ms, char** out_data, size_t* out_size){
    if (!url || !out_data || !out_size) { errno=EINVAL; return -1; }
    http_set_err(NULL);
    mem_t m; mem_init(&m);
    struct curl_slist* sl=NULL;
    CURL* h = easy_common(url, timeout_ms, &sl, headers_csv);
    if (!h) { errno=EIO; return -1; }
    int rc = perform_to_mem(h, &m);
    curl_slist_free_all(sl);
    curl_easy_cleanup(h);
    if (rc!=0) { free(m.p); errno=EIO; return -1; }
    mem_move_out(&m, out_data, out_size);
    return 0;
}

int http_post(const char* url,
              const void* body, size_t body_len, const char* content_type,
              const char* headers_csv,
              long timeout_ms,
              char** out_data, size_t* out_size)
{
    if (!url || (!body && body_len) || !out_data || !out_size) { errno=EINVAL; return -1; }
    http_set_err(NULL);
    mem_t m; mem_init(&m);
    struct curl_slist* sl=NULL;
    CURL* h = easy_common(url, timeout_ms, &sl, headers_csv);
    if (!h) { errno=EIO; return -1; }
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body? body : "");
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body_len);
    if (content_type && *content_type) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "Content-Type: %s", content_type);
        sl = curl_slist_append(sl, tmp);
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, sl);
    }
    int rc = perform_to_mem(h, &m);
    curl_slist_free_all(sl);
    curl_easy_cleanup(h);
    if (rc!=0) { free(m.p); errno=EIO; return -1; }
    mem_move_out(&m, out_data, out_size);
    return 0;
}

int http_download_file(const char* url, const char* out_path, long timeout_ms){
    if (!url || !out_path) { errno=EINVAL; return -1; }
    http_set_err(NULL);
    FILE* f = fopen(out_path, "wb");
    if (!f) return -1;
    struct curl_slist* sl=NULL;
    CURL* h = easy_common(url, timeout_ms, &sl, NULL);
    if (!h) { fclose(f); errno=EIO; return -1; }
    curl_easy_setopt(h, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, NULL); // fwrite direct
    CURLcode rc = curl_easy_perform(h);
    long code=0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(sl);
    curl_easy_cleanup(h);
    int ok = (rc==CURLE_OK && code<400) ? 0 : -1;
    if (ok!=0) { http_set_err(rc!=CURLE_OK ? curl_easy_strerror(rc) : "HTTP error"); }
    if (f) { if (fflush(f)!=0) ok=-1; if (fclose(f)!=0) ok=-1; }
    if (ok!=0) { (void)remove(out_path); errno=EIO; }
    return ok;
}

#else
// =====================================================================================
// = Fallback via binaire `curl` =======================================================
// =====================================================================================

static int have_cli(void){
#if defined(_WIN32)
    return system("where curl >NUL 2>NUL") == 0;
#else
    return system("command -v curl >/dev/null 2>&1") == 0;
#endif
}

static void sh_quote(char* out, size_t n, const char* in){
#if defined(_WIN32)
    // "C:\path with spaces"
    snprintf(out, n, "\"%s\"", in? in:"");
#else
    if (!in) { snprintf(out,n,"''"); return; }
    size_t used=0; out[0]='\0';
    if (used+1<n) out[used++]='\'', out[used]='\0';
    for (const char* p=in; *p && used+4<n; ++p){
        if (*p=='\'') used += snprintf(out+used, n-used, "'\\''");
        else out[used++]=*p, out[used]='\0';
    }
    if (used+1<n) out[used++]='\'', out[used]='\0';
#endif
}

static int read_whole(const char* path, char** out, size_t* out_sz){
    *out=NULL; if(out_sz) *out_sz=0;
    FILE* f=fopen(path,"rb"); if(!f) return -1;
    if (fseek(f,0,SEEK_END)!=0){ fclose(f); return -1; }
    long L=ftell(f); if(L<0){ fclose(f); return -1; }
    if (fseek(f,0,SEEK_SET)!=0){ fclose(f); return -1; }
    char* buf=(char*)malloc((size_t)L+1); if(!buf){ fclose(f); return -1; }
    size_t rd=fread(buf,1,(size_t)L,f); fclose(f);
    if (rd!=(size_t)L){ free(buf); return -1; }
    buf[L]='\0'; *out=buf; if(out_sz)*out_sz=(size_t)L; return 0;
}

static int run_cmd(const char* cmd){
#if defined(_WIN32)
    int rc = system(cmd);
    return rc==0 ? 0 : -1;
#else
    int rc = system(cmd);
    if (rc==-1) return -1;
    if (WIFEXITED(rc) && WEXITSTATUS(rc)==0) return 0;
    return -1;
#endif
}

static int build_headers_args(char* dst, size_t n, const char* headers_csv){
    dst[0]='\0';
    if (!headers_csv || !*headers_csv) return 0;
    size_t count=0; char** H = split_headers(headers_csv, &count);
    for (size_t i=0;i<count;i++){
#if defined(_WIN32)
        char q[512]; sh_quote(q,sizeof(q),H[i]);
        int r = snprintf(dst+strlen(dst), n - strlen(dst), " -H %s", q);
#else
        char q[512]; sh_quote(q,sizeof(q),H[i]);
        int r = snprintf(dst+strlen(dst), n - strlen(dst), " -H %s", q);
#endif
        if (r<0 || (size_t)r >= n - strlen(dst)) { free_headers(H,count); return -1; }
    }
    free_headers(H,count);
    return 0;
}

int http_get(const char* url, const char* headers_csv, long timeout_ms, char** out_data, size_t* out_size){
    if (!url || !out_data || !out_size) { errno=EINVAL; return -1; }
    http_set_err(NULL);
    if (!have_cli()) { http_set_err("curl CLI not found"); errno=ENOENT; return -1; }

    char qurl[1024]; sh_quote(qurl,sizeof(qurl),url);
    char hdr[2048]; if (build_headers_args(hdr,sizeof(hdr),headers_csv)!=0) { errno=E2BIG; return -1; }

#if defined(_WIN32)
    char tmp[] = "curl_out_XXXXXX.tmp";
    // crude unique name: append PID
    snprintf(tmp, sizeof(tmp), "curl_out_%lu.tmp", (unsigned long)GetCurrentProcessId());
#else
    char tmp[64]; snprintf(tmp,sizeof(tmp), "/tmp/vl_curl_%d.out", getpid());
#endif

    long to = timeout_ms>0? timeout_ms : HTTP_DEFAULT_TIMEOUT_MS;
#if defined(_WIN32)
    char cmd[4096];
    snprintf(cmd,sizeof(cmd),
        "curl --proto-default https --silent --show-error --fail --location "
        "--max-time %.0f --compressed -A \"VitteLight/0.1\"%s %s > %s 2>NUL",
        (double)(to/1000.0), hdr, qurl, tmp);
#else
    char cmd[4096];
    snprintf(cmd,sizeof(cmd),
        "curl --proto-default https --silent --show-error --fail --location "
        "--max-time %.0f --compressed -A 'VitteLight/0.1'%s %s > %s 2>/dev/null",
        (double)(to/1000.0), hdr, qurl, tmp);
#endif
    if (run_cmd(cmd)!=0) { http_set_err("curl failed"); (void)remove(tmp); errno=EIO; return -1; }
    int rc = read_whole(tmp, out_data, out_size);
    (void)remove(tmp);
    if (rc!=0){ http_set_err("read failed"); errno=EIO; return -1; }
    return 0;
}

int http_post(const char* url,
              const void* body, size_t body_len, const char* content_type,
              const char* headers_csv,
              long timeout_ms,
              char** out_data, size_t* out_size)
{
    if (!url || (!body && body_len) || !out_data || !out_size) { errno=EINVAL; return -1; }
    http_set_err(NULL);
    if (!have_cli()) { http_set_err("curl CLI not found"); errno=ENOENT; return -1; }

    char qurl[1024]; sh_quote(qurl,sizeof(qurl),url);
    char hdr[2048]; if (build_headers_args(hdr,sizeof(hdr),headers_csv)!=0) { errno=E2BIG; return -1; }

#if defined(_WIN32)
    char tf[] = "curl_body_XXXXXX.tmp"; snprintf(tf,sizeof(tf), "curl_body_%lu.tmp",(unsigned long)GetCurrentProcessId());
    char of[] = "curl_out_XXXXXX.tmp"; snprintf(of,sizeof(of), "curl_out_%lu.tmp",(unsigned long)GetCurrentProcessId());
#else
    char tf[64]; snprintf(tf,sizeof(tf), "/tmp/vl_curl_%d.body", getpid());
    char of[64]; snprintf(of,sizeof(of), "/tmp/vl_curl_%d.out",  getpid());
#endif
    // écrire body
    if (body_len){
        FILE* f = fopen(tf,"wb"); if(!f){ errno=EIO; return -1; }
        if (fwrite(body,1,body_len,f) != body_len){ fclose(f); (void)remove(tf); errno=EIO; return -1; }
        if (fflush(f)!=0 || fclose(f)!=0){ (void)remove(tf); errno=EIO; return -1; }
    } else {
        // créer fichier vide
        FILE* f=fopen(tf,"wb"); if (f) fclose(f);
    }

    long to = timeout_ms>0? timeout_ms : HTTP_DEFAULT_TIMEOUT_MS;
    char ctarg[256]="";
    if (content_type && *content_type){
        char qct[128]; sh_quote(qct,sizeof(qct),content_type);
#if defined(_WIN32)
        snprintf(ctarg,sizeof(ctarg)," -H \"Content-Type: %s\"", content_type);
#else
        snprintf(ctarg,sizeof(ctarg)," -H %s%s%s", "'Content-Type: ", content_type, "'");
#endif
    }

#if defined(_WIN32)
    char qtf[1024]; sh_quote(qtf,sizeof(qtf),tf);
    char cmd[4096];
    snprintf(cmd,sizeof(cmd),
        "curl --proto-default https --silent --show-error --fail --location "
        "--max-time %.0f --compressed -A \"VitteLight/0.1\"%s%s -X POST --data-binary @%s %s > %s 2>NUL",
        (double)(to/1000.0), hdr, ctarg, qtf, qurl, of);
#else
    char qtf[1024]; sh_quote(qtf,sizeof(qtf),tf);
    char cmd[4096];
    snprintf(cmd,sizeof(cmd),
        "curl --proto-default https --silent --show-error --fail --location "
        "--max-time %.0f --compressed -A 'VitteLight/0.1'%s%s -X POST --data-binary @%s %s > %s 2>/dev/null",
        (double)(to/1000.0), hdr, ctarg, qtf, qurl, of);
#endif

    int ok = run_cmd(cmd);
    (void)remove(tf);
    if (ok!=0){ http_set_err("curl failed"); (void)remove(of); errno=EIO; return -1; }
    int rc = read_whole(of, out_data, out_size);
    (void)remove(of);
    if (rc!=0){ http_set_err("read failed"); errno=EIO; return -1; }
    return 0;
}

int http_download_file(const char* url, const char* out_path, long timeout_ms){
    if (!url || !out_path) { errno=EINVAL; return -1; }
    http_set_err(NULL);
    if (!have_cli()) { http_set_err("curl CLI not found"); errno=ENOENT; return -1; }

    char qurl[1024]; sh_quote(qurl,sizeof(qurl),url);
    char qout[1024]; sh_quote(qout,sizeof(qout),out_path);
    long to = timeout_ms>0? timeout_ms : HTTP_DEFAULT_TIMEOUT_MS;

#if defined(_WIN32)
    char cmd[4096];
    snprintf(cmd,sizeof(cmd),
        "curl --proto-default https --silent --show-error --fail --location "
        "--tlsv1.2 --max-time %.0f --compressed -A \"VitteLight/0.1\" -o %s %s 1>NUL 2>NUL",
        (double)(to/1000.0), qout, qurl);
#else
    char cmd[4096];
    snprintf(cmd,sizeof(cmd),
        "curl --proto-default https --silent --show-error --fail --location "
        "--tlsv1.2 --max-time %.0f --compressed -A 'VitteLight/0.1' -o %s %s >/dev/null 2>&1",
        (double)(to/1000.0), qout, qurl);
#endif
    if (run_cmd(cmd)!=0){ http_set_err("curl failed"); (void)remove(out_path); errno=EIO; return -1; }
    return 0;
}
#endif

// -------- Demo optionnel --------
#ifdef CURL_DEMO
#include <stdio.h>
int main(void){
    char* data=NULL; size_t n=0;
    if (http_get("https://httpbin.org/get", "Accept: application/json", 5000, &data, &n)==0){
        printf("GET %zu bytes\n%.*s\n", n, (int)(n>200?n=200:n), data);
        free(data);
    } else {
        fprintf(stderr,"err: %s\n", http_err());
    }
    return 0;
}
#endif