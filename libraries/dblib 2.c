// SPDX-License-Identifier: GPL-3.0-or-later
//
// dblib.c — Minimal SQLite wrapper for Vitte Light (C17, portable)
// Namespace: "db"
//
// Build:
//   With SQLite (recommended):
//     cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_SQLITE3 dblib.c -lsqlite3 -c
//
//   Without SQLite:
//     cc -std=c17 -O2 -Wall -Wextra -pedantic -c dblib.c
//     // All functions return ENOSYS and fail cleanly.
//
// Provides:
//   Opaque handle and error API:
//     typedef struct db db_t;
//     const char* db_errmsg(db_t*);         // last error, thread-local if db==NULL
//
//   Open/close and pragmas:
//     db_t* db_open(const char* path, int flags); // flags bitmask below
//     void  db_close(db_t*);
//     int   db_exec(db_t*, const char* sql);      // convenience exec (no results)
//     int   db_begin(db_t*);                      // BEGIN IMMEDIATE
//     int   db_commit(db_t*);
//     int   db_rollback(db_t*);
//
//   Rowset query (small/medium results):
//     typedef struct {
//       int cols, rows;
//       char** colname;   // cols strings
//       char** cell;      // rows*cols strings (row-major). NULL allowed for SQL NULL.
//     } db_rowset;
//     void  db_rowset_free(db_rowset*);
//     int   db_query(db_t*, const char* sql, db_rowset* out); // fills out on success
//
//   Scalar helpers:
//     int   db_query_i64(db_t*, const char* sql, long long* out);
//     int   db_query_f64(db_t*, const char* sql, double* out);
//     int   db_query_str(db_t*, const char* sql, char** out); // malloc'd string
//
//   Meta / changes:
//     long long db_last_rowid(db_t*);
//     int        db_changes(db_t*);
//
// Flags:
//   DB_OPEN_RW   = 1  (create if not exists)
//   DB_OPEN_RO   = 2
//   DB_OPEN_MEM  = 4  (":memory:" forced; path ignored)
//
// Notes:
//   - Strings are UTF-8. All returned strings are heap-allocated with malloc and must be freed by caller via db_rowset_free()/free().
//   - This wrapper is synchronous and single-connection per handle. No implicit threads.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define DB_OPEN_RW  1
#define DB_OPEN_RO  2
#define DB_OPEN_MEM 4

typedef struct db db_t;

#if defined(_WIN32)
  #define DB_THREAD_LOCAL __declspec(thread)
#else
  #define DB_THREAD_LOCAL __thread
#endif

static DB_THREAD_LOCAL char g_err[256];

static void set_err(const char* s) {
    if (!s) { g_err[0] = '\0'; return; }
    snprintf(g_err, sizeof(g_err), "%s", s);
}

const char* db_errmsg(db_t* db) {
    (void)db;
    return g_err[0] ? g_err : NULL;
}

// ---------- Rowset ----------
typedef struct {
    int cols, rows;
    char** colname; // [cols]
    char** cell;    // [rows * cols]
} db_rowset;

static void* xmalloc(size_t n) { void* p = malloc(n ? n : 1); if (!p) { perror("oom"); abort(); } return p; }
static char* xstrdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void db_rowset_free(db_rowset* rs) {
    if (!rs) return;
    if (rs->colname) {
        for (int i=0;i<rs->cols;i++) free(rs->colname[i]);
        free(rs->colname);
    }
    if (rs->cell) {
        int n = rs->rows * rs->cols;
        for (int i=0;i<n;i++) free(rs->cell[i]);
        free(rs->cell);
    }
    memset(rs, 0, sizeof(*rs));
}

// ---------- SQLite-backed implementation ----------
#if defined(VL_HAVE_SQLITE3)
  #include <sqlite3.h>

struct db {
    sqlite3* h;
};

static int sql_err(db_t* db, int rc) {
    if (db && db->h) {
        const char* e = sqlite3_errmsg(db->h);
        set_err(e ? e : "sqlite error");
    } else {
        set_err("sqlite error");
    }
    errno = EIO;
    return rc;
}

db_t* db_open(const char* path, int flags) {
    set_err(NULL);
    sqlite3* h = NULL;

    int of = 0;
    if (flags & DB_OPEN_MEM) {
        path = ":memory:";
        of = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    } else if (flags & DB_OPEN_RO) {
        of = SQLITE_OPEN_READONLY;
    } else { // default RW
        of = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }

#if SQLITE_VERSION_NUMBER >= 3005000
    int rc = sqlite3_open_v2(path ? path : ":memory:", &h, of, NULL);
#else
    int rc = sqlite3_open(path ? path : ":memory:", &h);
#endif
    if (rc != SQLITE_OK) {
        set_err(h ? sqlite3_errmsg(h) : "sqlite open failed");
        if (h) sqlite3_close(h);
        errno = EIO;
        return NULL;
    }

    // Pragmas for safer defaults
    sqlite3_exec(h, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    sqlite3_exec(h, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(h, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    db_t* db = (db_t*)xmalloc(sizeof(*db));
    db->h = h;
    return db;
}

void db_close(db_t* db) {
    if (!db) return;
    if (db->h) sqlite3_close(db->h);
    free(db);
}

int db_exec(db_t* db, const char* sql) {
    if (!db || !db->h || !sql) { errno = EINVAL; return -1; }
    set_err(NULL);
    char* em = NULL;
    int rc = sqlite3_exec(db->h, sql, NULL, NULL, &em);
    if (rc != SQLITE_OK) {
        set_err(em ? em : sqlite3_errmsg(db->h));
        if (em) sqlite3_free(em);
        errno = EIO;
        return -1;
    }
    if (em) sqlite3_free(em);
    return 0;
}

int db_begin(db_t* db)    { return db_exec(db, "BEGIN IMMEDIATE;"); }
int db_commit(db_t* db)   { return db_exec(db, "COMMIT;"); }
int db_rollback(db_t* db) { return db_exec(db, "ROLLBACK;"); }

static int cb_collect(void* u, int ncol, char** vals, char** names) {
    db_rowset* rs = (db_rowset*)u;
    if (rs->cols == 0) {
        rs->cols = ncol;
        rs->colname = (char**)xmalloc((size_t)ncol * sizeof(char*));
        for (int i=0;i<ncol;i++) rs->colname[i] = xstrdup(names[i] ? names[i] : "");
    } else if (rs->cols != ncol) {
        // inconsistent; bail
        return 1;
    }
    // push one row
    int old = rs->rows;
    rs->rows++;
    rs->cell = (char**)realloc(rs->cell, (size_t)rs->rows * (size_t)rs->cols * sizeof(char*));
    if (!rs->cell) return 1;
    for (int i=0;i<ncol;i++) {
        char* v = vals && vals[i] ? xstrdup(vals[i]) : NULL;
        rs->cell[old * rs->cols + i] = v;
    }
    return 0;
}

int db_query(db_t* db, const char* sql, db_rowset* out) {
    if (!db || !db->h || !sql || !out) { errno = EINVAL; return -1; }
    set_err(NULL);
    memset(out, 0, sizeof(*out));
    char* em = NULL;
    int rc = sqlite3_exec(db->h, sql, cb_collect, out, &em);
    if (rc != SQLITE_OK) {
        if (em) { set_err(em); sqlite3_free(em); }
        else set_err(sqlite3_errmsg(db->h));
        db_rowset_free(out);
        errno = EIO;
        return -1;
    }
    if (em) sqlite3_free(em);
    return 0;
}

int db_query_i64(db_t* db, const char* sql, long long* out) {
    if (!out) { errno = EINVAL; return -1; }
    db_rowset rs; if (db_query(db, sql, &rs) != 0) return -1;
    int rc = -1;
    if (rs.rows > 0 && rs.cols > 0 && rs.cell[0]) {
        char* end = NULL;
        long long v = strtoll(rs.cell[0], &end, 10);
        if (end && *end == '\0') { *out = v; rc = 0; }
        else { set_err("not an integer"); errno = EINVAL; }
    } else { set_err("no rows"); errno = ENOENT; }
    db_rowset_free(&rs);
    return rc;
}

int db_query_f64(db_t* db, const char* sql, double* out) {
    if (!out) { errno = EINVAL; return -1; }
    db_rowset rs; if (db_query(db, sql, &rs) != 0) return -1;
    int rc = -1;
    if (rs.rows > 0 && rs.cols > 0 && rs.cell[0]) {
        char* end = NULL;
        double v = strtod(rs.cell[0], &end);
        if (end && *end == '\0') { *out = v; rc = 0; }
        else { set_err("not a float"); errno = EINVAL; }
    } else { set_err("no rows"); errno = ENOENT; }
    db_rowset_free(&rs);
    return rc;
}

int db_query_str(db_t* db, const char* sql, char** out) {
    if (!out) { errno = EINVAL; return -1; }
    *out = NULL;
    db_rowset rs; if (db_query(db, sql, &rs) != 0) return -1;
    int rc = -1;
    if (rs.rows > 0 && rs.cols > 0) {
        *out = rs.cell[0] ? xstrdup(rs.cell[0]) : NULL;
        rc = 0;
    } else { set_err("no rows"); errno = ENOENT; }
    db_rowset_free(&rs);
    return rc;
}

long long db_last_rowid(db_t* db) { return (db && db->h) ? (long long)sqlite3_last_insert_rowid(db->h) : 0; }
int        db_changes(db_t* db)   { return (db && db->h) ? sqlite3_changes(db->h) : 0; }

#else  // -------- Stubs when SQLite not available --------

struct db { int _; };

db_t* db_open(const char* path, int flags) {
    (void)path; (void)flags;
    set_err("SQLite support not built (define VL_HAVE_SQLITE3)");
    errno = ENOSYS; return NULL;
}
void db_close(db_t* db) { (void)db; }
int  db_exec(db_t* db, const char* sql) { (void)db; (void)sql; set_err("ENOSYS"); errno=ENOSYS; return -1; }
int  db_begin(db_t* db){ (void)db; set_err("ENOSYS"); errno=ENOSYS; return -1; }
int  db_commit(db_t* db){ (void)db; set_err("ENOSYS"); errno=ENOSYS; return -1; }
int  db_rollback(db_t* db){ (void)db; set_err("ENOSYS"); errno=ENOSYS; return -1; }

int  db_query(db_t* db, const char* sql, db_rowset* out){ (void)db; (void)sql; (void)out; set_err("ENOSYS"); errno=ENOSYS; return -1; }
int  db_query_i64(db_t* db, const char* sql, long long* out){ (void)db;(void)sql;(void)out; set_err("ENOSYS"); errno=ENOSYS; return -1; }
int  db_query_f64(db_t* db, const char* sql, double* out){ (void)db;(void)sql;(void)out; set_err("ENOSYS"); errno=ENOSYS; return -1; }
int  db_query_str(db_t* db, const char* sql, char** out){ (void)db;(void)sql;(void)out; set_err("ENOSYS"); errno=ENOSYS; return -1; }
long long db_last_rowid(db_t* db){ (void)db; return 0; }
int        db_changes(db_t* db){ (void)db; return 0; }

#endif

// ---------- Optional demo ----------
#ifdef DBLIB_DEMO
#include <stdio.h>
int main(void) {
#if !defined(VL_HAVE_SQLITE3)
    puts("No SQLite compiled in.");
    return 0;
#else
    db_t* db = db_open(":memory:", DB_OPEN_MEM);
    if (!db) { fprintf(stderr, "open: %s\n", db_errmsg(NULL)); return 1; }
    if (db_exec(db, "create table t(id integer primary key, name text);") != 0)
        fprintf(stderr, "exec: %s\n", db_errmsg(db));
    db_begin(db);
    db_exec(db, "insert into t(name) values ('alice'),('bob'),('carol');");
    db_commit(db);

    db_rowset rs;
    if (db_query(db, "select id,name from t order by id;", &rs)==0) {
        for (int r=0;r<rs.rows;r++) {
            printf("%s=%s, %s=%s\n",
                rs.colname[0], rs.cell[r*rs.cols+0]?rs.cell[r*rs.cols+0]:"NULL",
                rs.colname[1], rs.cell[r*rs.cols+1]?rs.cell[r*rs.cols+1]:"NULL");
        }
        db_rowset_free(&rs);
    } else {
        fprintf(stderr, "query: %s\n", db_errmsg(db));
    }
    long long n=0; if (db_query_i64(db, "select count(*) from t;", &n)==0) printf("count=%lld\n", n);
    db_close(db);
    return 0;
#endif
}
#endif
