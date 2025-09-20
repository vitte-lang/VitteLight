// SPDX-License-Identifier: GPL-3.0-or-later
//
// csv.c — Tiny CSV reader/writer for Vitte Light (C17, portable)
// Namespace: "csv"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c csv.c
//
// CSV rules supported (RFC4180-ish):
//   - Separator: comma ',' by default (configurable).
//   - Newline: \n, \r\n accepted when reading. Writer uses \n.
//   - Quotes: fields optionally in double quotes. "" escapes a quote inside.
//   - Empty field → empty string. Missing trailing fields → empty.
//   - No type inference. All strings are UTF-8 bytes.
//
// API:
//   typedef struct { char** v; size_t n, cap; } csv_row;    // fields
//   typedef struct { csv_row* r; size_t n, cap; } csv_table; // rows
//
//   // Parse a single CSV record from a memory buffer. Stops at newline or end.
//   // Returns number of bytes consumed (>=0), or -1 on error. Populates out row.
//   long  csv_parse_record(const char* buf, size_t len, char sep, csv_row* out);
//
//   // Parse whole buffer into table (splits by records). Returns 0/-1.
//   int   csv_parse_buffer(const char* buf, size_t len, char sep, csv_table* out);
//
//   // I/O helpers using FILE*
//   int   csv_read_file(FILE* f, char sep, csv_table* out);     // 0/-1
//   int   csv_write_file(FILE* f, char sep, const csv_table* t); // 0/-1
//
//   // Builders and memory
//   void  csv_row_init(csv_row* r);
//   void  csv_row_free(csv_row* r);
//   void  csv_table_init(csv_table* t);
//   void  csv_table_free(csv_table* t);
//   int   csv_row_push(csv_row* r, const char* s);              // copies
//   int   csv_table_push(csv_table* t, const csv_row* r);       // deep copy
//
// Notes:
//   - All strings are malloc’d and owned by the row/table. Free with csv_*_free().
//   - Writer quotes a field when needed (contains sep, quote, or newline).
//   - Errors set errno; typical values: EINVAL, ENOMEM, EIO.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// ---------- Vector helpers ----------
typedef struct { char** v; size_t n, cap; } csv_row;
typedef struct { csv_row* r; size_t n, cap; } csv_table;

static void* xrealloc(void* p, size_t n){ void* q = realloc(p, n? n:1); if(!q){ errno=ENOMEM; } return q; }
static char* xstrndup(const char* s, size_t n){ char* p = (char*)malloc(n+1); if(!p){ errno=ENOMEM; return NULL; } memcpy(p,s,n); p[n]='\0'; return p; }

void csv_row_init(csv_row* r){ r->v=NULL; r->n=0; r->cap=0; }
void csv_table_init(csv_table* t){ t->r=NULL; t->n=0; t->cap=0; }

void csv_row_free(csv_row* r){
    if(!r) return;
    for(size_t i=0;i<r->n;i++) free(r->v[i]);
    free(r->v); r->v=NULL; r->n=r->cap=0;
}

void csv_table_free(csv_table* t){
    if(!t) return;
    for(size_t i=0;i<t->n;i++) csv_row_free(&t->r[i]);
    free(t->r); t->r=NULL; t->n=t->cap=0;
}

int csv_row_push(csv_row* r, const char* s){
    if(!r){ errno=EINVAL; return -1; }
    if(r->n==r->cap){
        size_t nc = r->cap? r->cap*2:8;
        char** nv = (char**)xrealloc(r->v, nc*sizeof(*nv));
        if(!nv) return -1;
        r->v=nv; r->cap=nc;
    }
    char* dup = xstrndup(s? s:"", s? strlen(s):0);
    if(!dup) return -1;
    r->v[r->n++] = dup;
    return 0;
}

static int csv_row_push_n(csv_row* r, const char* s, size_t n){
    if(!r){ errno=EINVAL; return -1; }
    if(r->n==r->cap){
        size_t nc = r->cap? r->cap*2:8;
        char** nv = (char**)xrealloc(r->v, nc*sizeof(*nv));
        if(!nv) return -1;
        r->v=nv; r->cap=nc;
    }
    char* dup = xstrndup(s? s:"", s? n:0);
    if(!dup) return -1;
    r->v[r->n++] = dup;
    return 0;
}

int csv_table_push(csv_table* t, const csv_row* r){
    if(!t||!r){ errno=EINVAL; return -1; }
    if(t->n==t->cap){
        size_t nc = t->cap? t->cap*2:8;
        csv_row* nr = (csv_row*)xrealloc(t->r, nc*sizeof(*nr));
        if(!nr) return -1;
        t->r=nr; t->cap=nc;
    }
    csv_row dst; csv_row_init(&dst);
    for(size_t i=0;i<r->n;i++){
        if(csv_row_push(&dst, r->v[i])!=0){ csv_row_free(&dst); return -1; }
    }
    t->r[t->n++] = dst;
    return 0;
}

// ---------- Parser: one record ----------
long csv_parse_record(const char* buf, size_t len, char sep, csv_row* out){
    if(!buf || !out){ errno=EINVAL; return -1; }
    csv_row_init(out);

    const char* p = buf;
    const char* end = buf + len;

    while (p <= end) {
        // End of input and last empty field
        if (p == end) { if (csv_row_push(out, "")!=0) goto fail; break; }

        int quoted = 0;
        const char* field_start = p;
        size_t accum_sz = 0;
        char* accum = NULL;

        if (*p == '"') { // quoted field
            quoted = 1;
            p++; // skip initial quote
            while (p < end) {
                if (*p == '"') {
                    if (p+1 < end && p[1] == '"') {
                        // escaped quote -> append one '"'
                        if (accum_sz+1 >= (size_t)accum_sz+1){ /* avoid warning */ }
                        char* np = (char*)xrealloc(accum, accum_sz+1+1);
                        if(!np){ free(accum); goto fail; }
                        accum = np;
                        accum[accum_sz++] = '"';
                        accum[accum_sz] = '\0';
                        p += 2;
                    } else {
                        // closing quote
                        p++;
                        break;
                    }
                } else {
                    // append a chunk until next quote or end
                    const char* q = memchr(p, '"', (size_t)(end-p));
                    size_t chunk = q ? (size_t)(q - p) : (size_t)(end - p);
                    char* np = (char*)xrealloc(accum, accum_sz + chunk + 1);
                    if(!np){ free(accum); goto fail; }
                    memcpy(np + accum_sz, p, chunk);
                    accum = np;
                    accum_sz += chunk;
                    accum[accum_sz] = '\0';
                    p += chunk;
                }
            }
            // after closing quote, next must be sep or newline or end
            if (p > end) { free(accum); goto fail; }
            // consume until separator/newline/end
            if (p < end && *p != sep && *p != '\n' && *p != '\r') { free(accum); goto fail; }
        } else {
            // unquoted field: read until sep or newline
            const char* q = p;
            while (q < end && *q != sep && *q != '\n' && *q != '\r') q++;
            if (csv_row_push_n(out, p, (size_t)(q - p))!=0) goto fail;
            p = q;
            goto end_field;
        }

        // push quoted field (accum may be NULL meaning empty)
        if (csv_row_push_n(out, accum ? accum : "", accum ? accum_sz : 0)!=0){ free(accum); goto fail; }
        free(accum);

end_field:
        // Handle record terminator or field separator
        if (p == end) break;
        if (*p == sep) { p++; continue; }
        if (*p == '\r') { // \r\n or lone \r
            p++;
            if (p < end && *p == '\n') p++;
            break;
        }
        if (*p == '\n') { p++; break; }
        // If none of the above and not end, it's an error.
        if (*p != sep) continue;
    }
    return (long)(p - buf);

fail:
    csv_row_free(out);
    errno = EINVAL;
    return -1;
}

// ---------- Parse full buffer ----------
int csv_parse_buffer(const char* buf, size_t len, char sep, csv_table* out){
    if(!buf || !out){ errno=EINVAL; return -1; }
    csv_table_init(out);
    const char* p = buf;
    const char* end = buf + len;

    while (p < end) {
        // Skip lone newlines between records
        if (*p == '\n') { p++; continue; }
        if (*p == '\r') { p++; if (p<end && *p=='\n') p++; continue; }

        csv_row row; long used = csv_parse_record(p, (size_t)(end - p), sep, &row);
        if (used < 0) { csv_table_free(out); return -1; }
        if (csv_table_push(out, &row)!=0){ csv_row_free(&row); csv_table_free(out); return -1; }
        csv_row_free(&row);
        p += used;
    }
    return 0;
}

// ---------- I/O ----------
static int read_stream_into_mem(FILE* f, char** out, size_t* out_len){
    *out=NULL; if(out_len) *out_len=0;
    if(!f){ errno=EINVAL; return -1; }
    if (fseek(f, 0, SEEK_END)==0){
        long L = ftell(f);
        if (L < 0) { rewind(f); }
        else {
            rewind(f);
            char* b = (char*)malloc((size_t)L+1);
            if(!b){ errno=ENOMEM; return -1; }
            size_t rd = fread(b,1,(size_t)L,f);
            b[rd] = '\0';
            *out = b; if(out_len) *out_len = rd;
            return 0;
        }
    }
    // Fallback: chunked read
    size_t cap=0, n=0; char* b=NULL;
    char tmp[1<<14];
    for(;;){
        size_t rd = fread(tmp,1,sizeof(tmp),f);
        if (rd){
            if (n+rd+1 > cap){
                size_t nc = cap? cap*2:65536;
                while (nc < n+rd+1) nc <<= 1;
                char* nb = (char*)realloc(b, nc);
                if(!nb){ free(b); errno=ENOMEM; return -1; }
                b=nb; cap=nc;
            }
            memcpy(b+n, tmp, rd); n+=rd; b[n]='\0';
        }
        if (rd < sizeof(tmp)){
            if (ferror(f)){ free(b); errno=EIO; return -1; }
            break;
        }
    }
    *out=b; if(out_len)*out_len=n; return 0;
}

int csv_read_file(FILE* f, char sep, csv_table* out){
    if(!f || !out){ errno=EINVAL; return -1; }
    char* buf=NULL; size_t n=0;
    if (read_stream_into_mem(f, &buf, &n)!=0) return -1;
    int rc = csv_parse_buffer(buf, n, sep, out);
    free(buf);
    return rc;
}

// Quote when needed and write one field
static int write_field(FILE* f, char sep, const char* s){
    if(!s) s="";
    int needs_q = 0;
    for (const unsigned char* p=(const unsigned char*)s; *p; ++p){
        if (*p==(unsigned char)sep || *p=='"' || *p=='\n' || *p=='\r'){ needs_q=1; break; }
    }
    if (!needs_q){
        if (fputs(s, f) < 0) return -1;
        return 0;
    }
    if (fputc('"', f) == EOF) return -1;
    for (const unsigned char* p=(const unsigned char*)s; *p; ++p){
        if (*p=='"'){ if (fputc('"',f)==EOF || fputc('"',f)==EOF) return -1; }
        else if (fputc(*p, f) == EOF) return -1;
    }
    if (fputc('"', f) == EOF) return -1;
    return 0;
}

int csv_write_file(FILE* f, char sep, const csv_table* t){
    if(!f || !t){ errno=EINVAL; return -1; }
    for (size_t r=0; r<t->n; ++r){
        for (size_t c=0; c<t->r[r].n; ++c){
            if (c>0 && fputc(sep, f)==EOF) return -1;
            if (write_field(f, sep, t->r[r].v[c])!=0) return -1;
        }
        if (fputc('\n', f)==EOF) return -1;
    }
    if (fflush(f)!=0) return -1;
    return 0;
}

// ---------- Optional demo ----------
#ifdef CSV_DEMO
#include <stdio.h>
int main(void){
    const char* sample = "a,b,c\n\"x,y\",2,\"he said \"\"hi\"\"\"\r\n,\n";
    csv_table t;
    if (csv_parse_buffer(sample, strlen(sample), ',', &t)!=0){ perror("parse"); return 1; }
    csv_write_file(stdout, ',', &t);
    csv_table_free(&t);
    return 0;
}
#endif