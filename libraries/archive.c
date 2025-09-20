// SPDX-License-Identifier: GPL-3.0-or-later
//
// archive.c — Outils TAR (POSIX ustar) portables pour Vitte Light (C17, sans dépendances)
// Namespace: "arch"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c archive.c
//
// Fournit :
//   - Écriture TAR :
//       arch_tar_open_write(path)
//       arch_tar_add_file(tw, src_path, arc_path, mode)     // fichier régulier
//       arch_tar_add_dir (tw, arc_path, mode)               // entrée répertoire
//       arch_tar_close_write(tw)
//   - Lecture / Listing / Extraction :
//       arch_tar_list(path, cb, user)
//       arch_tar_extract_all(path, dest_root)
//   - Limitations intentionnelles : fichiers et dossiers (pas de liens spéciaux/char/dev).
//   - Compatible ustar. Champs numériques en octal, padding 512.
//
// Notes :
//   - Pas de compression intégrée. Pour .tar.gz, compressez/décompressez en amont/aval.
//   - Gère Windows + POSIX. Normalise les chemins d’archive en '/'.
//   - Les permissions sont appliquées au mieux (Windows ignore POSIX mode).
//
// Exemple rapide :
//   arch_tar_writer* w = arch_tar_open_write("out.tar");
//   arch_tar_add_dir(w, "folder/", 0755);
//   arch_tar_add_file(w, "local.bin", "folder/data.bin", 0644);
//   arch_tar_close_write(w);
//
//   arch_tar_extract_all("out.tar", "dest");
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <sys/stat.h>
  #define PATH_SEP '\\'
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif
#else
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #include <utime.h>
  #define PATH_SEP '/'
#endif

#ifndef ARCH_API
#define ARCH_API
#endif

/* ========================= Helpers ========================= */

static void *xmalloc(size_t n){ void* p = malloc(n); if(!p){ perror("malloc"); exit(1);} return p; }
static size_t strlcpy0(char* dst, const char* src, size_t cap){
    if (!cap) return 0; size_t n=0; while (n+1<cap && src && src[n]) { dst[n]=src[n]; n++; }
    dst[n]=0; return n;
}
static void path_to_tar(char* s){ for (; *s; ++s) if (*s=='\\') *s='/'; }
static int mk_dirs_p(const char* path){
    char tmp[4096]; strlcpy0(tmp, path, sizeof tmp);
    size_t n = strlen(tmp);
    if (n==0) return 0;
    /* skip leading slashes */
    size_t i = 0; while (tmp[i]=='/' || tmp[i]=='\\') i++;
    for (; i<n; i++){
        if (tmp[i]=='/' || tmp[i]=='\\'){
            tmp[i] = 0;
#if defined(_WIN32)
            if (tmp[0] && _mkdir(tmp)!=0 && errno!=EEXIST) return -1;
#else
            if (tmp[0] && mkdir(tmp, 0755)!=0 && errno!=EEXIST) return -1;
#endif
            tmp[i] = PATH_SEP;
        }
    }
#if defined(_WIN32)
    if (_mkdir(tmp)!=0 && errno!=EEXIST) return -1;
#else
    if (mkdir(tmp,0755)!=0 && errno!=EEXIST) return -1;
#endif
    return 0;
}
static void join_path(char* out, size_t cap, const char* a, const char* b){
    size_t na = a?strlen(a):0, nb = b?strlen(b):0;
    if (!cap) return;
    size_t i=0;
    for (; i<na && i<cap-1; i++) out[i]=a[i];
    if (i && out[i-1]!=PATH_SEP && i<cap-1) out[i++]=PATH_SEP;
    for (size_t j=0;j<nb && i<cap-1;j++) out[i++]=b[j];
    out[i]=0;
}

/* ========================= TAR structures ========================= */

#pragma pack(push,1)
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;         /* '0' file, '5' dir */
    char linkname[100];
    char magic[6];         /* "ustar\0" */
    char version[2];       /* "00" */
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} tar_hdr;
#pragma pack(pop)

static void octal_write(char* dst, size_t n, uint64_t v){
    /* Écrit v en octal ASCII, right-justified, N-1 chars + '\0' final.
       Les champs TAR traditionnels demandent un espace/NULL final. */
    char buf[32]; size_t i=0;
    do { buf[i++] = (char)('0' + (v & 7)); v >>= 3; } while (v && i<sizeof buf);
    /* Remplir de '0' */
    size_t pos = n-2; /* laisser place pour '\0' */
    for (size_t k=0;k<n;k++) dst[k]='0';
    for (size_t k=0;k<i && pos<(n-1);k++) dst[pos--] = buf[k];
    dst[n-1] = '\0';
}

static unsigned hdr_checksum(const tar_hdr* h){
    unsigned sum=0;
    const unsigned char* p=(const unsigned char*)h;
    for (size_t i=0;i<sizeof *h;i++){
        if (i>=148 && i<156) sum += 0x20; else sum += p[i];
    }
    return sum;
}

static void hdr_set_name(tar_hdr* h, const char* arc_path){
    /* Split prefix/name if >100 */
    char tmp[512]; strlcpy0(tmp, arc_path?arc_path:"", sizeof tmp);
    path_to_tar(tmp);
    size_t n = strlen(tmp);
    if (n <= 100){
        strlcpy0(h->name, tmp, sizeof h->name);
        h->prefix[0]=0;
        return;
    }
    /* Try to split at last '/' so that name<=100 and prefix<=155 */
    const char* slash = tmp + n;
    while (slash>tmp && *slash!='/') slash--;
    if (slash==tmp){ /* no slash to split, truncate */
        strlcpy0(h->name, tmp + (n>100?(n-100):0), sizeof h->name);
        h->prefix[0]=0;
        return;
    }
    size_t name_len = (size_t)(tmp + n - (slash+1));
    size_t pre_len  = (size_t)(slash - tmp);
    if (name_len>100) name_len=100; /* still too big: hard truncate */
    if (pre_len>155) { /* try to drop leading components */
        const char* p = tmp + (pre_len - 155);
        strlcpy0(h->prefix, p, sizeof h->prefix);
    } else {
        memcpy(h->prefix, tmp, pre_len);
        h->prefix[pre_len]=0;
    }
    memcpy(h->name, slash+1, name_len);
    h->name[name_len]=0;
}

static void hdr_fill_common(tar_hdr* h, const char* arc_path, uint64_t size, uint32_t mode, uint64_t mtime, char typeflag){
    memset(h, 0, sizeof *h);
    hdr_set_name(h, arc_path);
    octal_write(h->mode,   sizeof h->mode,   mode ? mode & 07777u : (typeflag=='5'? 0755u : 0644u));
    octal_write(h->uid,    sizeof h->uid,    0);
    octal_write(h->gid,    sizeof h->gid,    0);
    octal_write(h->size,   sizeof h->size,   (typeflag=='5')? 0 : size);
    octal_write(h->mtime,  sizeof h->mtime,  mtime);
    memset(h->chksum, ' ', sizeof h->chksum);
    h->typeflag = typeflag;
    h->magic[0]='u'; h->magic[1]='s'; h->magic[2]='t'; h->magic[3]='a'; h->magic[4]='r'; h->magic[5]=0;
    h->version[0]='0'; h->version[1]='0';
    /* uname/gname facultatifs */
    strlcpy0(h->uname, "user", sizeof h->uname);
    strlcpy0(h->gname, "group", sizeof h->gname);
    unsigned sum = hdr_checksum(h);
    octal_write(h->chksum, sizeof h->chksum, sum);
}

/* ========================= Writer ========================= */

typedef struct { FILE* f; } arch_tar_writer;

ARCH_API arch_tar_writer* arch_tar_open_write(const char* tar_path){
    FILE* f = fopen(tar_path, "wb");
    if (!f) return NULL;
    arch_tar_writer* w = (arch_tar_writer*)xmalloc(sizeof *w);
    w->f = f;
    return w;
}

static int write_zeros(FILE* f, size_t n){
    static const unsigned char z[512]={0};
    while (n){
        size_t k = n>512?512:n;
        if (fwrite(z,1,k,f)!=k) return -1;
        n -= k;
    }
    return 0;
}

ARCH_API int arch_tar_add_dir(arch_tar_writer* w, const char* arc_path, uint32_t mode){
    if (!w || !w->f || !arc_path) return -1;
    /* Ensure trailing slash in archive entry */
    char ap[512]; strlcpy0(ap, arc_path, sizeof ap);
    path_to_tar(ap);
    size_t len = strlen(ap);
    if (!len || ap[len-1]!='/'){ if (len+2>=sizeof ap) return -1; ap[len]='/'; ap[len+1]=0; }
    tar_hdr h; hdr_fill_common(&h, ap, 0, mode, (uint64_t)time(NULL), '5');
    if (fwrite(&h,1,sizeof h,w->f)!=sizeof h) return -1;
    return 0;
}

ARCH_API int arch_tar_add_file(arch_tar_writer* w, const char* src_path, const char* arc_path, uint32_t mode){
    if (!w || !w->f || !src_path || !arc_path) return -1;
    /* stat size + mtime */
#if defined(_WIN32)
    struct _stat64 st; if (_stat64(src_path, &st)!=0 || !S_ISREG(st.st_mode)) return -1;
    uint64_t fsz = (uint64_t)st.st_size; uint64_t mt = (uint64_t)st.st_mtime;
#else
    struct stat st; if (stat(src_path, &st)!=0 || !S_ISREG(st.st_mode)) return -1;
    uint64_t fsz = (uint64_t)st.st_size; uint64_t mt = (uint64_t)st.st_mtime;
#endif
    FILE* in = fopen(src_path, "rb"); if (!in) return -1;

    tar_hdr h; hdr_fill_common(&h, arc_path, fsz, mode, mt, '0');
    if (fwrite(&h,1,sizeof h,w->f)!=sizeof h){ fclose(in); return -1; }

    /* copy with 512-block padding */
    unsigned char buf[1<<15];
    uint64_t left = fsz;
    while (left){
        size_t r = (size_t)((left > sizeof buf) ? sizeof buf : left);
        r = fread(buf,1,r,in);
        if (r==0 && ferror(in)){ fclose(in); return -1; }
        if (r){
            if (fwrite(buf,1,r,w->f)!=r){ fclose(in); return -1; }
            left -= r;
        } else break;
    }
    fclose(in);
    /* pad to 512 */
    size_t pad = (size_t)((512 - (fsz % 512)) % 512);
    if (pad && write_zeros(w->f, pad)!=0) return -1;
    return 0;
}

ARCH_API int arch_tar_close_write(arch_tar_writer* w){
    if (!w) return -1;
    int rc = 0;
    if (w->f){
        if (write_zeros(w->f, 1024)!=0) rc=-1; /* deux blocs vides */
        if (fclose(w->f)!=0) rc=-1;
    }
    free(w);
    return rc;
}

/* ========================= Reader / Extract ========================= */

typedef struct {
    char name[256];
    uint64_t size;
    char type;      /* '0' file, '5' dir, autres ignorés */
    uint32_t mode;
    uint64_t mtime;
} arch_tar_entry;

typedef int (*arch_list_cb)(const arch_tar_entry* e, void* user);

static uint64_t octal_read(const char* s, size_t n){
    uint64_t v=0;
    for (size_t i=0;i<n && s[i]; i++){
        if (s[i]==' ' || s[i]=='\0') break;
        if (s[i]<'0' || s[i]>'7') break;
        v = (v<<3) + (uint64_t)(s[i]-'0');
    }
    return v;
}

static int parse_hdr(const tar_hdr* h, arch_tar_entry* e){
    /* validate magic optionally */
    if (!(h->magic[0]=='u' && h->magic[1]=='s' && h->magic[2]=='t' && h->magic[3]=='a' && h->magic[4]=='r'))
        ; /* we still try to read, many tars omit magic */
    unsigned stored = (unsigned)octal_read(h->chksum, sizeof h->chksum);
    unsigned calc = hdr_checksum(h);
    if (stored!=0 && stored!=calc){
        /* could be non-ustar without checksum strict; accept zero blocks detection outside */
        ;
    }
    /* join prefix/name */
    char path[256]; path[0]=0;
    if (h->prefix[0]){
        strlcpy0(path, h->prefix, sizeof path);
        size_t n=strlen(path);
        if (n+1<sizeof path){ path[n]='/'; path[n+1]=0; }
        strlcpy0(path+strlen(path), h->name, sizeof path - strlen(path));
    } else {
        strlcpy0(path, h->name, sizeof path);
    }
    /* normalize */
    for (char* p=path; *p; ++p) if (*p=='\\') *p='/';

    e->type  = h->typeflag ? h->typeflag : '0';
    e->size  = octal_read(h->size, sizeof h->size);
    e->mode  = (uint32_t)octal_read(h->mode, sizeof h->mode);
    e->mtime = octal_read(h->mtime, sizeof h->mtime);
    strlcpy0(e->name, path, sizeof e->name);
    return 0;
}

static int is_zero_block(const unsigned char* b){
    for (int i=0;i<512;i++) if (b[i]!=0) return 0; return 1;
}

ARCH_API int arch_tar_list(const char* tar_path, arch_list_cb cb, void* user){
    FILE* f = fopen(tar_path, "rb"); if (!f) return -1;
    unsigned char block[512];
    while (fread(block,1,512,f)==512){
        if (is_zero_block(block)){
            /* check second zero block */
            if (fread(block,1,512,f)==512 && is_zero_block(block)) break;
            else { fseek(f,-512,SEEK_CUR); continue; }
        }
        tar_hdr h; memcpy(&h, block, 512);
        arch_tar_entry e; parse_hdr(&h, &e);
        if (cb){
            int rc = cb(&e, user);
            if (rc) { fclose(f); return rc; }
        }
        /* skip payload */
        uint64_t skip = e.size;
        if (e.type=='5') skip = 0;
        if (skip){
            uint64_t pad = (512 - (skip % 512)) % 512;
            if (fseek(f, (long)(skip + pad), SEEK_CUR)!=0){ fclose(f); return -1; }
        }
    }
    fclose(f);
    return 0;
}

ARCH_API int arch_tar_extract_all(const char* tar_path, const char* dest_root){
    FILE* f = fopen(tar_path, "rb"); if (!f) return -1;
    unsigned char block[512];
    char outpath[4096];
    while (fread(block,1,512,f)==512){
        if (is_zero_block(block)){
            if (fread(block,1,512,f)==512 && is_zero_block(block)) break;
            else { fseek(f,-512,SEEK_CUR); continue; }
        }
        tar_hdr h; memcpy(&h, block, 512);
        arch_tar_entry e; parse_hdr(&h, &e);

        /* construire chemin */
        if (dest_root && *dest_root) join_path(outpath, sizeof outpath, dest_root, e.name);
        else strlcpy0(outpath, e.name, sizeof outpath);

        /* sécurité basique: empêcher sortie du dest_root via .. */
        if (strstr(e.name, "..")) { /* on peut raffiner si besoin */
            /* skip entry */
            uint64_t skip = e.size;
            uint64_t pad = (512 - (skip % 512)) % 512;
            if (fseek(f, (long)(skip + pad), SEEK_CUR)!=0){ fclose(f); return -1; }
            continue;
        }

        if (e.type=='5'){ /* dir */
            if (mk_dirs_p(outpath)!=0){ fclose(f); return -1; }
#if !defined(_WIN32)
            chmod(outpath, e.mode? e.mode : 0755);
#endif
            continue;
        }

        /* ensure parent directory exists */
        {
            char dirbuf[4096]; strlcpy0(dirbuf, outpath, sizeof dirbuf);
            char* last = NULL; for (char* p=dirbuf; *p; ++p) if (*p=='/'||*p=='\\') last=p;
            if (last){ *last=0; if (dirbuf[0]) mk_dirs_p(dirbuf); }
        }

        /* write file */
        FILE* out = fopen(outpath, "wb");
        if (!out){ fclose(f); return -1; }

        uint64_t left = e.size;
        while (left){
            size_t chunk = (size_t)(left > sizeof block ? sizeof block : left);
            size_t r = fread(block,1,chunk,f);
            if (r!=chunk){ fclose(out); fclose(f); return -1; }
            if (fwrite(block,1,r,out)!=r){ fclose(out); fclose(f); return -1; }
            left -= r;
        }
        fclose(out);

        /* pad */
        if (e.size % 512){
            uint64_t pad = 512 - (e.size % 512);
            if (fseek(f, (long)pad, SEEK_CUR)!=0){ fclose(f); return -1; }
        }

#if !defined(_WIN32)
        chmod(outpath, e.mode? e.mode : 0644);
        struct utimbuf tb; tb.actime=(time_t)e.mtime; tb.modtime=(time_t)e.mtime;
        utime(outpath, &tb);
#endif
    }
    fclose(f);
    return 0;
}

/* ========================= Test optionnel ========================= */
#ifdef ARCH_TEST
static int list_cb(const arch_tar_entry* e, void* u){
    (void)u;
    printf("%c %08o %10llu %s\n",
           e->type, (unsigned)e->mode,
           (unsigned long long)e->size, e->name);
    return 0;
}
int main(void){
    arch_tar_writer* w = arch_tar_open_write("demo.tar");
    arch_tar_add_dir(w, "folder/", 0755);
    FILE* t=fopen("hello.txt","wb"); fputs("Hello TAR\n",t); fclose(t);
    arch_tar_add_file(w, "hello.txt", "folder/hello.txt", 0644);
    arch_tar_close_write(w);

    puts("== LIST ==");
    arch_tar_list("demo.tar", list_cb, NULL);

    puts("== EXTRACT ==");
    arch_tar_extract_all("demo.tar", "out");

    return 0;
}
#endif