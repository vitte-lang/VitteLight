// SPDX-License-Identifier: GPL-3.0-or-later
//
// z.c — Compression utilitaire complète (mémoire, flux, fichiers), C17 portable
// Namespace: "z"
//
// Points clés:
//   • CRC32 (table générée à l’exécution)
//   • Mémoire: deflate/inflate (zlib si dispo, sinon passthrough)
//   • Flux   : compress/decompress FILE*↔FILE* (chunks)
//   • GZip   : lecture/écriture, détection auto (magic 1F 8B)
//   • Options: niveau, stratégie simple, tailles tampon
//
// Build (avec zlib):
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DHAVE_ZLIB z.c -lz
// Build (fallback sans zlib): compile et fonctionne, mais sans compression.
//
// Codes retour communs:
//   0=OK ; 1=OK passthrough ; -1=E/S/Mem ; -2=zlib ; -3=input invalide

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(HAVE_ZLIB)
  #include <zlib.h>
#endif

#ifndef Z_API
#define Z_API
#endif

/* ===================== CRC32 ===================== */

static uint32_t z__crc_tbl[256];
static int z__crc_init_done = 0;

static void z__crc_init(void){
    if (z__crc_init_done) return;
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for (int k=0;k<8;k++) c = (c&1)? (0xEDB88320u ^ (c>>1)) : (c>>1);
        z__crc_tbl[i]=c;
    }
    z__crc_init_done=1;
}
Z_API uint32_t z_crc32(const void* p, size_t n){
    z__crc_init();
    const uint8_t* s=(const uint8_t*)p;
    uint32_t c=~0u;
    for (size_t i=0;i<n;i++) c = z__crc_tbl[(c ^ s[i]) & 0xFFu] ^ (c>>8);
    return ~c;
}

/* ===================== Mémoire ===================== */

Z_API int z_deflate_mem(const void* in, size_t in_n, int level, void** out, size_t* out_n){
    if (!out || !out_n) return -3;
#if defined(HAVE_ZLIB)
    if (level<0) level = Z_DEFAULT_COMPRESSION;
    /* marge standard compressBound */
    uLongf cap = compressBound((uLong)in_n);
    void* buf = malloc(cap?cap:1);
    if (!buf) return -1;
    int rc = compress2((Bytef*)buf, &cap, (const Bytef*)in, (uLong)in_n, level);
    if (rc != Z_OK){ free(buf); return -2; }
    *out = buf; *out_n = (size_t)cap; return 0;
#else
    void* buf = malloc(in_n?in_n:1);
    if (!buf) return -1;
    if (in && in_n) memcpy(buf,in,in_n);
    *out=buf; *out_n=in_n; return 1;
#endif
}

Z_API int z_inflate_mem(const void* in, size_t in_n, void** out, size_t* out_n, size_t hint){
    if (!out || !out_n) return -3;
#if defined(HAVE_ZLIB)
    size_t cap = hint? hint : (in_n*3 + 64);
    if (cap<256) cap=256;
    void* buf=NULL;
    for (int tries=0; tries<8; tries++){
        void* nb = realloc(buf, cap);
        if (!nb){ free(buf); return -1; }
        buf=nb;
        uLongf outl=(uLongf)cap;
        int rc = uncompress((Bytef*)buf, &outl, (const Bytef*)in, (uLong)in_n);
        if (rc==Z_OK){ *out=buf; *out_n=(size_t)outl; return 0; }
        if (rc==Z_BUF_ERROR){ cap*=2; continue; }
        free(buf); return -2;
    }
    free(buf); return -2;
#else
    void* buf = malloc(in_n?in_n:1);
    if (!buf) return -1;
    if (in && in_n) memcpy(buf,in,in_n);
    *out=buf; *out_n=in_n; return 1;
#endif
}

/* ===================== Flux (FILE* ↔ FILE*) ===================== */

typedef struct {
    size_t in_chunk;   /* taille lecture */
    size_t out_chunk;  /* taille écriture */
    int    level;      /* deflate */
} z_stream_opts;

static void z__opts_default(z_stream_opts* o){
    o->in_chunk  = 1<<16;  /* 64 KiB */
    o->out_chunk = 1<<16;
    o->level     = -1;     /* zlib default */
}

/* Détecte un en-tête GZip (1F 8B 08) dans les 3 premiers octets. */
Z_API int z_is_gzip_header(const unsigned char* p, size_t n){
    return (n>=3 && p[0]==0x1F && p[1]==0x8B && p[2]==0x08) ? 1 : 0;
}

#if defined(HAVE_ZLIB)
/* ---- avec zlib ---- */
Z_API int z_deflate_fp(FILE* fin, FILE* fout, const z_stream_opts* _opt){
    if (!fin||!fout) return -3;
    z_stream_opts opt; if(_opt) opt=*(_opt); else z__opts_default(&opt);

    z_stream zs; memset(&zs,0,sizeof zs);
    int lvl = (opt.level<0)? Z_DEFAULT_COMPRESSION : (opt.level>9? 9: opt.level);
    if (deflateInit(&zs, lvl) != Z_OK) return -2;

    unsigned char* in  = (unsigned char*)malloc(opt.in_chunk);
    unsigned char* out = (unsigned char*)malloc(opt.out_chunk);
    if(!in||!out){ free(in); free(out); deflateEnd(&zs); return -1; }

    int rc=0;
    for(;;){
        zs.avail_in = (uInt)fread(in,1,opt.in_chunk,fin);
        zs.next_in  = in;
        int last = feof(fin);

        do{
            zs.avail_out = (uInt)opt.out_chunk; zs.next_out = out;
            int flush = last ? Z_FINISH : Z_NO_FLUSH;
            int zrc = deflate(&zs, flush);
            if (zrc==Z_STREAM_ERROR){ rc=-2; goto done; }
            size_t have = opt.out_chunk - zs.avail_out;
            if (have && fwrite(out,1,have,fout)!=have){ rc=-1; goto done; }
        } while (zs.avail_out==0);

        if (last) break;
        if (ferror(fin)){ rc=-1; goto done; }
    }

done:
    deflateEnd(&zs);
    free(in); free(out);
    return rc;
}

Z_API int z_inflate_fp(FILE* fin, FILE* fout, const z_stream_opts* _opt, int raw_gzip_auto){
    if (!fin||!fout) return -3;
    z_stream_opts opt; if(_opt) opt=*(_opt); else z__opts_default(&opt);

    /* Détecter gzip vs zlib vs raw en regardant le début. */
    unsigned char peek[4]={0}; size_t pn=fread(peek,1,3,fin);
    if (pn>0) fseek(fin, (long)pn * -1L, SEEK_CUR);
    int is_gz = z_is_gzip_header(peek,pn);

    z_stream zs; memset(&zs,0,sizeof zs);
    int winbits = is_gz? 16+MAX_WBITS : MAX_WBITS; /* 16+ pour gzip auto */
    if (raw_gzip_auto==2) winbits = MAX_WBITS;     /* zlib enrobage */
    if (raw_gzip_auto==1) winbits = -MAX_WBITS;    /* raw deflate */

    if (inflateInit2(&zs, winbits) != Z_OK) return -2;

    unsigned char* in  = (unsigned char*)malloc(opt.in_chunk);
    unsigned char* out = (unsigned char*)malloc(opt.out_chunk);
    if(!in||!out){ free(in); free(out); inflateEnd(&zs); return -1; }

    int rc=0;
    int done=0;
    while (!done){
        zs.avail_in = (uInt)fread(in,1,opt.in_chunk,fin);
        if (ferror(fin)){ rc=-1; break; }
        if (zs.avail_in==0){ /* pousser un finish */
            int zrc;
            do{
                zs.avail_out=(uInt)opt.out_chunk; zs.next_out=out;
                zrc = inflate(&zs, Z_FINISH);
                size_t have = opt.out_chunk - zs.avail_out;
                if (have && fwrite(out,1,have,fout)!=have){ rc=-1; goto out; }
            } while (zrc!=Z_STREAM_END && zs.avail_out==0);
            if (zrc==Z_STREAM_END){ rc=0; goto out; }
            if (zrc!=Z_OK && zrc!=Z_BUF_ERROR){ rc=-2; goto out; }
            goto out;
        }
        zs.next_in  = in;

        for(;;){
            zs.avail_out=(uInt)opt.out_chunk; zs.next_out=out;
            int zrc = inflate(&zs, Z_NO_FLUSH);
            if (zrc==Z_STREAM_END){ done=1; }
            else if (zrc!=Z_OK){ if(zrc==Z_BUF_ERROR && zs.avail_in==0) break; if(zrc!=Z_BUF_ERROR){ rc=-2; goto out; } }
            size_t have = opt.out_chunk - zs.avail_out;
            if (have && fwrite(out,1,have,fout)!=have){ rc=-1; goto out; }
            if (zs.avail_out!=0) break;
        }
    }

out:
    inflateEnd(&zs);
    free(in); free(out);
    return rc;
}
#else
/* ---- fallback sans zlib: copie brute ---- */
Z_API int z_deflate_fp(FILE* fin, FILE* fout, const z_stream_opts* _opt){
    (void)_opt;
    unsigned char buf[1<<16];
    size_t n;
    while ((n=fread(buf,1,sizeof buf,fin))>0){
        if (fwrite(buf,1,n,fout)!=n) return -1;
    }
    return ferror(fin)? -1 : 1;
}
Z_API int z_inflate_fp(FILE* fin, FILE* fout, const z_stream_opts* _opt, int raw_gzip_auto){
    (void)_opt; (void)raw_gzip_auto;
    unsigned char buf[1<<16];
    size_t n;
    while ((n=fread(buf,1,sizeof buf,fin))>0){
        if (fwrite(buf,1,n,fout)!=n) return -1;
    }
    return ferror(fin)? -1 : 1;
}
#endif

/* ===================== Fichiers GZip simples ===================== */

Z_API int z_gzip_file_write(const char* path, const void* buf, size_t n, int level){
    if (!path) return -3;
#if defined(HAVE_ZLIB)
    gzFile g;
    {
        char mode[8]; snprintf(mode,sizeof mode,"wb%d",(level<0||level>9)? Z_DEFAULT_COMPRESSION: level);
        g = gzopen(path, mode);
    }
    if (!g) return -1;
    size_t off=0;
    while (off<n){
        unsigned chunk = (n-off>1<<20)? (1<<20) : (unsigned)(n-off);
        int w = gzwrite(g, ((const unsigned char*)buf)+off, (unsigned)chunk);
        if (w <= 0){ gzclose(g); return -2; }
        off += (size_t)w;
    }
    if (gzclose(g) != Z_OK) return -2;
    return 0;
#else
    FILE* f=fopen(path,"wb");
    if (!f) return -1;
    size_t w = n? fwrite(buf,1,n,f):0;
    int rc = (w==n)? 1 : -1;
    fclose(f); return rc;
#endif
}

Z_API int z_gzip_file_read(const char* path, void** out, size_t* out_n){
    if (!path||!out||!out_n) return -3;
#if defined(HAVE_ZLIB)
    gzFile g = gzopen(path,"rb");
    if (!g) return -1;
    size_t cap=1<<16, len=0;
    unsigned char* buf=(unsigned char*)malloc(cap);
    if(!buf){ gzclose(g); return -1; }
    for(;;){
        if (len==cap){
            size_t nc=cap*2;
            unsigned char* nb=(unsigned char*)realloc(buf,nc);
            if(!nb){ free(buf); gzclose(g); return -1; }
            buf=nb; cap=nc;
        }
        int r = gzread(g, buf+len, (unsigned)(cap-len));
        if (r<0){ free(buf); gzclose(g); return -2; }
        if (r==0) break;
        len += (size_t)r;
    }
    gzclose(g);
    *out=buf; *out_n=len; return 0;
#else
    FILE* f=fopen(path,"rb");
    if (!f) return -1;
    if (fseek(f,0,SEEK_END)!=0){ fclose(f); return -1; }
    long L=ftell(f); if (L<0){ fclose(f); return -1; }
    if (fseek(f,0,SEEK_SET)!=0){ fclose(f); return -1; }
    void* buf=malloc((size_t)L? (size_t)L:1);
    if(!buf){ fclose(f); return -1; }
    size_t r=fread(buf,1,(size_t)L,f); fclose(f);
    if (r!=(size_t)L){ free(buf); return -1; }
    *out=buf; *out_n=(size_t)L; return 1;
#endif
}

/* ===================== Utilitaires haut niveau ===================== */

/* Compresse un fichier source vers un fichier destination en zlib "raw/zlib/gzip".
   mode: 0=zlib, 1=gzip, 2=raw(deflate). */
Z_API int z_file_compress(const char* src, const char* dst, int mode, int level){
    if (!src||!dst) return -3;
    FILE* in=fopen(src,"rb"); if(!in) return -1;
#if defined(HAVE_ZLIB)
    int rc=0;
    if (mode==1){
        /* Écrire directement en .gz via gz* API */
        /* Lire entièrement et transmettre à z_gzip_file_write pour simplicité */
        fseek(in,0,SEEK_END); long L=ftell(in); if(L<0){ fclose(in); return -1; }
        fseek(in,0,SEEK_SET);
        void* buf=malloc((size_t)L? (size_t)L:1); if(!buf){ fclose(in); return -1; }
        size_t r=fread(buf,1,(size_t)L,in); fclose(in);
        if (r!=(size_t)L){ free(buf); return -1; }
        rc = z_gzip_file_write(dst, buf, (size_t)L, level);
        free(buf); return rc;
    }
    /* zlib raw/zlib via z_(de)flate_fp + zlib enveloppe paramétrée par inflate/deflateInit2.
       Pour simplicité, on utilise deflateInit + copy, suffisant (zlib) */
    FILE* out=fopen(dst,"wb"); if(!out){ fclose(in); return -1; }
    z_stream_opts opt; z__opts_default(&opt); opt.level=level;
    rc = z_deflate_fp(in,out,&opt);
    fclose(out);
    return rc;
#else
    /* fallback copie */
    FILE* out=fopen(dst,"wb"); if(!out){ fclose(in); return -1; }
    unsigned char buf[1<<16]; size_t n; int rc=1;
    while((n=fread(buf,1,sizeof buf,in))>0){
        if (fwrite(buf,1,n,out)!=n){ rc=-1; break; }
    }
    fclose(in); fclose(out); return rc;
#endif
}

/* Décompresse un fichier source vers dest. raw_gzip_auto: 0=auto(zlib/gzip),1=raw,2=zlib. */
Z_API int z_file_decompress(const char* src, const char* dst, int raw_gzip_auto){
    if (!src||!dst) return -3;
    FILE* in=fopen(src,"rb"); if(!in) return -1;
    FILE* out=fopen(dst,"wb"); if(!out){ fclose(in); return -1; }
    int rc = z_inflate_fp(in,out,NULL,raw_gzip_auto);
    fclose(in); fclose(out);
    return rc;
}

/* Mémoire → GZip fichier, et fichier GZip → mémoire utilitaires rapides */
Z_API int z_gzip_buffer_to_file(const char* path, const void* buf, size_t n, int level){
    return z_gzip_file_write(path, buf, n, level);
}
Z_API int z_gzip_file_to_buffer(const char* path, void** out, size_t* out_n){
    return z_gzip_file_read(path, out, out_n);
}

/* ===================== Test ===================== */
#ifdef Z_TEST
#include <assert.h>
static void hexdump(const void* p, size_t n){
    const unsigned char* b=p;
    for(size_t i=0;i<n;i++){ printf("%02x%s", b[i], ((i&15)==15||i+1==n)?"\n":" "); }
}
int main(void){
    const char* msg="Vitte Light — zlib util complète";
    void* c=NULL; size_t cn=0;
    int rc = z_deflate_mem(msg, strlen(msg), 6, &c, &cn);
    printf("deflate rc=%d out=%zu\n", rc, cn);
    void* p=NULL; size_t pn=0;
    rc = z_inflate_mem(c, cn, &p, &pn, 64);
    printf("inflate rc=%d out=%zu eq=%d\n", rc, pn, (int)(pn==strlen(msg)&&memcmp(p,msg,pn)==0));
    printf("crc=%08x\n", z_crc32(msg, strlen(msg)));
#if defined(HAVE_ZLIB)
    assert(z_gzip_file_write("ztest.gz", msg, strlen(msg), 6)==0);
    void* fb=NULL; size_t fn=0; assert(z_gzip_file_read("ztest.gz",&fb,&fn)==0);
    printf("gz len=%zu eq=%d\n", fn, (int)(fn==strlen(msg)&&memcmp(fb,msg,fn)==0));
    free(fb);
    FILE* in=fopen("ztest.gz","rb"); FILE* out=fopen("ztest.out","wb");
    assert(z_inflate_fp(in,out,NULL,0)==0); fclose(in); fclose(out);
#endif
    free(c); free(p);
    return 0;
}
#endif