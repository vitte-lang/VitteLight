// SPDX-License-Identifier: GPL-3.0-or-later
//
// dblib.c — SQLite wrapper C17 portable for Vitte Light
//
// Capacités:
//   - Ouverture/fermeture de bases (fichier, :memory:)
//   - Exec direct (DDL/DML), erreurs détaillées
//   - Requêtes avec callback ligne par ligne
//   - Statements préparés: bind (nil/int64/double/text/blob), step, getters,
//   finalize
//   - Transactions: BEGIN [IMMEDIATE|EXCLUSIVE], COMMIT, ROLLBACK
//   - Helpers: last_insert_rowid, changes
//
// Dépendances:
//   - includes/auxlib.h (AuxStatus + utils)
//   - SQLite3 (optionnel). Si non présent, retourne AUX_ENOSYS.
//
// Build exemple:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_SQLITE3 -c dblib.c
//   cc ... dblib.o -lsqlite3
//
// En-tête suggéré: includes/dblib.h (facultatif; API minimale redéclarée
// ci-dessous).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"

#ifdef VL_HAVE_SQLITE3
#include <sqlite3.h>
#endif

#ifndef VITTE_LIGHT_INCLUDES_DBLIB_H
#define VITTE_LIGHT_INCLUDES_DBLIB_H 1

typedef struct VlDb VlDb;
typedef struct VlDbStmt VlDbStmt;

typedef enum {
  VL_DB_OPEN_RO = 1 << 0,      // read-only
  VL_DB_OPEN_RW = 1 << 1,      // read-write
  VL_DB_OPEN_CREATE = 1 << 2,  // create if missing
  VL_DB_OPEN_MEM = 1 << 3      // open :memory:
} VlDbOpenFlags;

typedef enum {
  VL_DB_STEP_ROW = 1,
  VL_DB_STEP_DONE = 0,
  VL_DB_STEP_ERR = -1
} VlDbStepRc;

typedef int (*VlDbRowCb)(int ncol, const char *const *colnames,
                         const char *const *values, void *ud);

// Core
AuxStatus vl_db_open(const char *path, int flags, VlDb **out_db);
void vl_db_close(VlDb *db);

// Exec and queries
AuxStatus vl_db_exec(VlDb *db, const char *sql);
AuxStatus vl_db_exec_query(VlDb *db, const char *sql, VlDbRowCb cb, void *ud);

// Prepared statements
AuxStatus vl_db_prepare(VlDb *db, const char *sql, VlDbStmt **out_stmt,
                        const char **tail);
AuxStatus vl_db_bind_null(VlDbStmt *st, int idx);
AuxStatus vl_db_bind_int64(VlDbStmt *st, int idx, int64_t v);
AuxStatus vl_db_bind_double(VlDbStmt *st, int idx, double v);
AuxStatus vl_db_bind_text(VlDbStmt *st, int idx, const char *s);
AuxStatus vl_db_bind_blob(VlDbStmt *st, int idx, const void *p, size_t n);
AuxStatus vl_db_clear_bindings(VlDbStmt *st);
AuxStatus vl_db_reset(VlDbStmt *st);
int vl_db_bind_count(VlDbStmt *st);
VlDbStepRc vl_db_step(VlDbStmt *st);
int vl_db_column_count(VlDbStmt *st);
const char *vl_db_column_name(VlDbStmt *st, int i);
int vl_db_column_type(
    VlDbStmt *st, int i);  // 1=NULL,2=INT,3=FLOAT,4=TEXT,5=BLOB (aligné sqlite)
int64_t vl_db_column_int64(VlDbStmt *st, int i);
double vl_db_column_double(VlDbStmt *st, int i);
const char *vl_db_column_text(VlDbStmt *st, int i);
const void *vl_db_column_blob(VlDbStmt *st, int i, int *out_size);
AuxStatus vl_db_finalize(VlDbStmt *st);

// Transactions + meta
AuxStatus vl_db_begin(
    VlDb *db,
    const char *mode);  // mode: NULL|"DEFERRED"|"IMMEDIATE"|"EXCLUSIVE"
AuxStatus vl_db_commit(VlDb *db);
AuxStatus vl_db_rollback(VlDb *db);
int64_t vl_db_last_insert_rowid(VlDb *db);
int vl_db_changes(VlDb *db);

// Error string (optionnel)
const char *vl_db_errstr(VlDb *db);

#endif  // VITTE_LIGHT_INCLUDES_DBLIB_H

// ======================================================================
// Implémentation
// ======================================================================

#ifdef VL_HAVE_SQLITE3

struct VlDb {
  sqlite3 *h;
};
struct VlDbStmt {
  sqlite3_stmt *h;
};

static AuxStatus map_sqlite_err(int rc) {
  if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) return AUX_OK;
  switch (rc) {
    case SQLITE_NOMEM:
      return AUX_ENOMEM;
    case SQLITE_RANGE:
      return AUX_ERANGE;
    case SQLITE_MISUSE:
      return AUX_EINVAL;
    default:
      return AUX_EIO;
  }
}

AuxStatus vl_db_open(const char *path, int flags, VlDb **out_db) {
  if (!out_db) return AUX_EINVAL;
  *out_db = NULL;

  int oflags = 0;
  if (flags & VL_DB_OPEN_MEM) path = ":memory:";
  if (flags & VL_DB_OPEN_RO) oflags |= SQLITE_OPEN_READONLY;
  if (flags & VL_DB_OPEN_RW) oflags |= SQLITE_OPEN_READWRITE;
  if (flags & VL_DB_OPEN_CREATE) oflags |= SQLITE_OPEN_CREATE;
  if (!oflags) oflags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;

  sqlite3 *h = NULL;
  int rc = sqlite3_open_v2(path ? path : ":memory:", &h, oflags, NULL);
  if (rc != SQLITE_OK) {
    if (h) {
      sqlite3_close(h);
    }
    return map_sqlite_err(rc);
  }
  // Tuning raisonnable
  sqlite3_exec(h, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
  sqlite3_exec(h, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
  sqlite3_exec(h, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

  VlDb *db = (VlDb *)calloc(1, sizeof *db);
  if (!db) {
    sqlite3_close(h);
    return AUX_ENOMEM;
  }
  db->h = h;
  *out_db = db;
  return AUX_OK;
}

void vl_db_close(VlDb *db) {
  if (!db) return;
  if (db->h) sqlite3_close(db->h);
  free(db);
}

const char *vl_db_errstr(VlDb *db) {
  return db && db->h ? sqlite3_errmsg(db->h) : "db=NULL";
}

AuxStatus vl_db_exec(VlDb *db, const char *sql) {
  if (!db || !db->h || !sql) return AUX_EINVAL;
  char *err = NULL;
  int rc = sqlite3_exec(db->h, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    if (err) {
      AUX_LOG_ERROR("sqlite exec: %s", err);
      sqlite3_free(err);
    }
    return map_sqlite_err(rc);
  }
  return AUX_OK;
}

static int row_tramp(void *ud, int ncol, char **vals, char **cols) {
  VlDbRowCb cb = (VlDbRowCb)ud;
  if (!cb) return 0;
  // cast to const
  const char *const *cvals = (const char *const *)vals;
  const char *const *cnames = (const char *const *)cols;
  return cb(ncol, cnames, cvals, NULL);
}

AuxStatus vl_db_exec_query(VlDb *db, const char *sql, VlDbRowCb cb, void *ud) {
  if (!db || !db->h || !sql) return AUX_EINVAL;
  (void)ud;  // cb n’a pas d’userdata param dans cette signature; si besoin,
             // changez l’API.
  char *err = NULL;
  int rc = sqlite3_exec(db->h, sql, cb ? row_tramp : NULL, (void *)cb, &err);
  if (rc != SQLITE_OK) {
    if (err) {
      AUX_LOG_ERROR("sqlite query: %s", err);
      sqlite3_free(err);
    }
    return map_sqlite_err(rc);
  }
  return AUX_OK;
}

// Prepared statements
AuxStatus vl_db_prepare(VlDb *db, const char *sql, VlDbStmt **out_stmt,
                        const char **tail) {
  if (!db || !db->h || !sql || !out_stmt) return AUX_EINVAL;
  *out_stmt = NULL;
  sqlite3_stmt *st = NULL;
  const char *ztail = NULL;
  int rc = sqlite3_prepare_v2(db->h, sql, -1, &st, &ztail);
  if (rc != SQLITE_OK) return map_sqlite_err(rc);
  VlDbStmt *S = (VlDbStmt *)calloc(1, sizeof *S);
  if (!S) {
    sqlite3_finalize(st);
    return AUX_ENOMEM;
  }
  S->h = st;
  *out_stmt = S;
  if (tail) *tail = ztail;
  return AUX_OK;
}

AuxStatus vl_db_bind_null(VlDbStmt *st, int idx) {
  if (!st || !st->h) return AUX_EINVAL;
  return map_sqlite_err(sqlite3_bind_null(st->h, idx));
}

AuxStatus vl_db_bind_int64(VlDbStmt *st, int idx, int64_t v) {
  if (!st || !st->h) return AUX_EINVAL;
  return map_sqlite_err(sqlite3_bind_int64(st->h, idx, (sqlite3_int64)v));
}

AuxStatus vl_db_bind_double(VlDbStmt *st, int idx, double v) {
  if (!st || !st->h) return AUX_EINVAL;
  return map_sqlite_err(sqlite3_bind_double(st->h, idx, v));
}

AuxStatus vl_db_bind_text(VlDbStmt *st, int idx, const char *s) {
  if (!st || !st->h || !s) return AUX_EINVAL;
  return map_sqlite_err(sqlite3_bind_text(st->h, idx, s, -1, SQLITE_TRANSIENT));
}

AuxStatus vl_db_bind_blob(VlDbStmt *st, int idx, const void *p, size_t n) {
  if (!st || !st->h || (!p && n)) return AUX_EINVAL;
  return map_sqlite_err(
      sqlite3_bind_blob(st->h, idx, p, (int)n, SQLITE_TRANSIENT));
}

AuxStatus vl_db_clear_bindings(VlDbStmt *st) {
  if (!st || !st->h) return AUX_EINVAL;
  return map_sqlite_err(sqlite3_clear_bindings(st->h));
}

AuxStatus vl_db_reset(VlDbStmt *st) {
  if (!st || !st->h) return AUX_EINVAL;
  return map_sqlite_err(sqlite3_reset(st->h));
}

int vl_db_bind_count(VlDbStmt *st) {
  if (!st || !st->h) return 0;
  return sqlite3_bind_parameter_count(st->h);
}

VlDbStepRc vl_db_step(VlDbStmt *st) {
  if (!st || !st->h) return VL_DB_STEP_ERR;
  int rc = sqlite3_step(st->h);
  if (rc == SQLITE_ROW) return VL_DB_STEP_ROW;
  if (rc == SQLITE_DONE) return VL_DB_STEP_DONE;
  return VL_DB_STEP_ERR;
}

int vl_db_column_count(VlDbStmt *st) {
  if (!st || !st->h) return 0;
  return sqlite3_column_count(st->h);
}

const char *vl_db_column_name(VlDbStmt *st, int i) {
  if (!st || !st->h) return NULL;
  return sqlite3_column_name(st->h, i);
}

int vl_db_column_type(VlDbStmt *st, int i) {
  if (!st || !st->h) return 0;
  return sqlite3_column_type(st->h, i);  // 1..5 comme SQLite
}

int64_t vl_db_column_int64(VlDbStmt *st, int i) {
  if (!st || !st->h) return 0;
  return (int64_t)sqlite3_column_int64(st->h, i);
}

double vl_db_column_double(VlDbStmt *st, int i) {
  if (!st || !st->h) return 0.0;
  return sqlite3_column_double(st->h, i);
}

const char *vl_db_column_text(VlDbStmt *st, int i) {
  if (!st || !st->h) return NULL;
  return (const char *)sqlite3_column_text(st->h, i);
}

const void *vl_db_column_blob(VlDbStmt *st, int i, int *out_size) {
  if (!st || !st->h) return NULL;
  if (out_size) *out_size = sqlite3_column_bytes(st->h, i);
  return sqlite3_column_blob(st->h, i);
}

AuxStatus vl_db_finalize(VlDbStmt *st) {
  if (!st) return AUX_OK;
  if (st->h) {
    int rc = sqlite3_finalize(st->h);
    free(st);
    return map_sqlite_err(rc);
  }
  free(st);
  return AUX_OK;
}

// Transactions + meta
AuxStatus vl_db_begin(VlDb *db, const char *mode) {
  if (!db || !db->h) return AUX_EINVAL;
  const char *m = (mode && *mode) ? mode : "DEFERRED";
  char sql[64];
  snprintf(sql, sizeof sql, "BEGIN %s TRANSACTION;", m);
  return vl_db_exec(db, sql);
}

AuxStatus vl_db_commit(VlDb *db) {
  if (!db || !db->h) return AUX_EINVAL;
  return vl_db_exec(db, "COMMIT;");
}

AuxStatus vl_db_rollback(VlDb *db) {
  if (!db || !db->h) return AUX_EINVAL;
  return vl_db_exec(db, "ROLLBACK;");
}

int64_t vl_db_last_insert_rowid(VlDb *db) {
  if (!db || !db->h) return 0;
  return (int64_t)sqlite3_last_insert_rowid(db->h);
}

int vl_db_changes(VlDb *db) {
  if (!db || !db->h) return 0;
  return sqlite3_changes(db->h);
}

#else  // VL_HAVE_SQLITE3

// Stubs si SQLite absent
struct VlDb {
  int _;
};
struct VlDbStmt {
  int _;
};

AuxStatus vl_db_open(const char *path, int flags, VlDb **out_db) {
  (void)path;
  (void)flags;
  (void)out_db;
  return AUX_ENOSYS;
}
void vl_db_close(VlDb *db) { (void)db; }
const char *vl_db_errstr(VlDb *db) {
  (void)db;
  return "sqlite3 not available";
}
AuxStatus vl_db_exec(VlDb *db, const char *sql) {
  (void)db;
  (void)sql;
  return AUX_ENOSYS;
}
AuxStatus vl_db_exec_query(VlDb *db, const char *sql, VlDbRowCb cb, void *ud) {
  (void)db;
  (void)sql;
  (void)cb;
  (void)ud;
  return AUX_ENOSYS;
}
AuxStatus vl_db_prepare(VlDb *db, const char *sql, VlDbStmt **out_stmt,
                        const char **tail) {
  (void)db;
  (void)sql;
  (void)out_stmt;
  (void)tail;
  return AUX_ENOSYS;
}
AuxStatus vl_db_bind_null(VlDbStmt *st, int idx) {
  (void)st;
  (void)idx;
  return AUX_ENOSYS;
}
AuxStatus vl_db_bind_int64(VlDbStmt *st, int idx, int64_t v) {
  (void)st;
  (void)idx;
  (void)v;
  return AUX_ENOSYS;
}
AuxStatus vl_db_bind_double(VlDbStmt *st, int idx, double v) {
  (void)st;
  (void)idx;
  (void)v;
  return AUX_ENOSYS;
}
AuxStatus vl_db_bind_text(VlDbStmt *st, int idx, const char *s) {
  (void)st;
  (void)idx;
  (void)s;
  return AUX_ENOSYS;
}
AuxStatus vl_db_bind_blob(VlDbStmt *st, int idx, const void *p, size_t n) {
  (void)st;
  (void)idx;
  (void)p;
  (void)n;
  return AUX_ENOSYS;
}
AuxStatus vl_db_clear_bindings(VlDbStmt *st) {
  (void)st;
  return AUX_ENOSYS;
}
AuxStatus vl_db_reset(VlDbStmt *st) {
  (void)st;
  return AUX_ENOSYS;
}
int vl_db_bind_count(VlDbStmt *st) {
  (void)st;
  return 0;
}
VlDbStepRc vl_db_step(VlDbStmt *st) {
  (void)st;
  return VL_DB_STEP_ERR;
}
int vl_db_column_count(VlDbStmt *st) {
  (void)st;
  return 0;
}
const char *vl_db_column_name(VlDbStmt *st, int i) {
  (void)st;
  (void)i;
  return NULL;
}
int vl_db_column_type(VlDbStmt *st, int i) {
  (void)st;
  (void)i;
  return 0;
}
int64_t vl_db_column_int64(VlDbStmt *st, int i) {
  (void)st;
  (void)i;
  return 0;
}
double vl_db_column_double(VlDbStmt *st, int i) {
  (void)st;
  (void)i;
  return 0.0;
}
const char *vl_db_column_text(VlDbStmt *st, int i) {
  (void)st;
  (void)i;
  return NULL;
}
const void *vl_db_column_blob(VlDbStmt *st, int i, int *out_size) {
  (void)st;
  (void)i;
  (void)out_size;
  return NULL;
}
AuxStatus vl_db_finalize(VlDbStmt *st) {
  (void)st;
  return AUX_ENOSYS;
}
AuxStatus vl_db_begin(VlDb *db, const char *mode) {
  (void)db;
  (void)mode;
  return AUX_ENOSYS;
}
AuxStatus vl_db_commit(VlDb *db) {
  (void)db;
  return AUX_ENOSYS;
}
AuxStatus vl_db_rollback(VlDb *db) {
  (void)db;
  return AUX_ENOSYS;
}
int64_t vl_db_last_insert_rowid(VlDb *db) {
  (void)db;
  return 0;
}
int vl_db_changes(VlDb *db) {
  (void)db;
  return 0;
}

#endif  // VL_HAVE_SQLITE3
