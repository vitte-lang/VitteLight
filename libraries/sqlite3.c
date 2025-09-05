// SPDX-License-Identifier: GPL-3.0-or-later
//
// sqlite3.c — SQLite3 bindings pour Vitte Light (C17, complet)
// Namespace: "sqlite"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_SQLITE3 -c sqlite3.c
//   cc ... sqlite3.o -lsqlite3
//
// Portabilité:
//   - Si VL_HAVE_SQLITE3 défini et <sqlite3.h> présent: implémentation réelle.
//   - Sinon: stubs -> (nil,"ENOSYS").
//
// API (handle par identifiants entiers, pas d'userdata VM):
//   -- Connexions
//     sqlite.open(path[, flags])           -> dbid | (nil, errmsg)   // flags
//     int SQLite (ex: 0, 1==READONLY, etc.) sqlite.open_ex(path, flags, [vfs])
//     -> dbid | (nil, errmsg)   // sqlite3_open_v2 sqlite.close(db) -> true |
//     (nil, errmsg) sqlite.busy_timeout(db, ms)          -> true | (nil,
//     errmsg) sqlite.errcode(db)                   -> int sqlite.errmsg(db) ->
//     string sqlite.changes(db)                   -> int
//     sqlite.total_changes(db)             -> int
//     sqlite.last_insert_rowid(db)         -> int64
//
//   -- Exécution directe
//     sqlite.exec(db, sql)                 -> true | (nil, errmsg)
//
//   -- Préparés
//     sqlite.prepare(db, sql)              -> stmtid | (nil, errmsg)
//     sqlite.finalize(stmt)                -> true | (nil, errmsg)
//     sqlite.reset(stmt)                   -> true | (nil, errmsg)
//     sqlite.clear_bindings(stmt)          -> true | (nil, errmsg)
//     sqlite.step(stmt)                    -> rc:int                  //
//     100=ROW, 101=DONE; autres = code SQLite
//
//   -- Bind (1-based)
//     sqlite.bind_null(stmt, i)            -> true | (nil, errmsg)
//     sqlite.bind_int(stmt, i, v:int64)    -> true | (nil, errmsg)
//     sqlite.bind_float(stmt, i, v:float)  -> true | (nil, errmsg)
//     sqlite.bind_text(stmt, i, s:string)  -> true | (nil, errmsg)
//     sqlite.bind_blob(stmt, i, s:string)  -> true | (nil, errmsg)
//
//   -- Columns (0-based)
//     sqlite.column_count(stmt)            -> int
//     sqlite.column_type(stmt, i)          -> int                     //
//     1=int,2=float,3=text,4=blob,5=null sqlite.column_name(stmt, i) -> string
//     | (nil, errmsg) sqlite.column_int(stmt, i)           -> int64
//     sqlite.column_float(stmt, i)         -> float
//     sqlite.column_text(stmt, i)          -> string
//     sqlite.column_blob(stmt, i)          -> string
//
//   -- Transactions
//     sqlite.txn_begin(db[, immediate=true]) -> true | (nil, errmsg)
//     sqlite.txn_commit(db)                  -> true | (nil, errmsg)
//     sqlite.txn_rollback(db)                -> true | (nil, errmsg)
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef VL_HAVE_SQLITE3
#include <sqlite3.h>
#endif

// ---------------------------------------------------------------------
// Aides VM
// ---------------------------------------------------------------------

static const char *db_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t db_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static double db_check_num(VL_State *S, int idx) {
  VL_Value *v = vl_get(S, idx);
  if (!v) {
    vl_errorf(S, "argument #%d: number expected", idx);
    return vl_error(S);
  }
  return vl_tonumber(S, v);
}
static int db_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int db_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)db_check_int(S, idx);
  return defv;
}

// ---------------------------------------------------------------------
// Stubs si SQLite absent
// ---------------------------------------------------------------------
#ifndef VL_HAVE_SQLITE3

#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)
static int vlsql_open(VL_State *S) {
  (void)db_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_open_ex(VL_State *S) {
  (void)db_check_str(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_close(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_busy_timeout(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_errcode(VL_State *S) {
  vl_push_int(S, 0);
  return 1;
}
static int vlsql_errmsg(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_changes(VL_State *S) {
  vl_push_int(S, 0);
  return 1;
}
static int vlsql_total_changes(VL_State *S) {
  vl_push_int(S, 0);
  return 1;
}
static int vlsql_last_insert_rowid(VL_State *S) {
  vl_push_int(S, 0);
  return 1;
}
static int vlsql_exec(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_str(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_prepare(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_str(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_finalize(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_reset(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_clear_bindings(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_step(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_bind_null(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_bind_int(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  (void)db_check_int(S, 3);
  NOSYS_PAIR(S);
}
static int vlsql_bind_float(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  (void)db_check_num(S, 3);
  NOSYS_PAIR(S);
}
static int vlsql_bind_text(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  (void)db_check_str(S, 3);
  NOSYS_PAIR(S);
}
static int vlsql_bind_blob(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  (void)db_check_str(S, 3);
  NOSYS_PAIR(S);
}
static int vlsql_column_count(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_column_type(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_column_name(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_column_int(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_column_float(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_column_text(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_column_blob(VL_State *S) {
  (void)db_check_int(S, 1);
  (void)db_check_int(S, 2);
  NOSYS_PAIR(S);
}
static int vlsql_txn_begin(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_txn_commit(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vlsql_txn_rollback(VL_State *S) {
  (void)db_check_int(S, 1);
  NOSYS_PAIR(S);
}

#else
// ---------------------------------------------------------------------
// Implémentation réelle SQLite
// ---------------------------------------------------------------------

typedef struct DbEntry {
  int used;
  sqlite3 *db;
} DbEntry;
typedef struct StEntry {
  int used;
  sqlite3_stmt *st;
  int dbid;
} StEntry;

static DbEntry *g_db = NULL;
static int g_db_cap = 0;
static StEntry *g_st = NULL;
static int g_st_cap = 0;

static int ensure_db_cap(int need) {
  if (need <= g_db_cap) return 1;
  int ncap = g_db_cap ? g_db_cap : 8;
  while (ncap < need) ncap <<= 1;
  DbEntry *nd = (DbEntry *)realloc(g_db, (size_t)ncap * sizeof *nd);
  if (!nd) return 0;
  for (int i = g_db_cap; i < ncap; i++) {
    nd[i].used = 0;
    nd[i].db = NULL;
  }
  g_db = nd;
  g_db_cap = ncap;
  return 1;
}
static int ensure_st_cap(int need) {
  if (need <= g_st_cap) return 1;
  int ncap = g_st_cap ? g_st_cap : 16;
  while (ncap < need) ncap <<= 1;
  StEntry *ns = (StEntry *)realloc(g_st, (size_t)ncap * sizeof *ns);
  if (!ns) return 0;
  for (int i = g_st_cap; i < ncap; i++) {
    ns[i].used = 0;
    ns[i].st = NULL;
    ns[i].dbid = 0;
  }
  g_st = ns;
  g_st_cap = ncap;
  return 1;
}
static int alloc_db_slot(void) {
  for (int i = 1; i < g_db_cap; i++)
    if (!g_db[i].used) return i;
  if (!ensure_db_cap(g_db_cap ? g_db_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_db_cap; i++)
    if (!g_db[i].used) return i;
  return 0;
}
static int alloc_st_slot(void) {
  for (int i = 1; i < g_st_cap; i++)
    if (!g_st[i].used) return i;
  if (!ensure_st_cap(g_st_cap ? g_st_cap * 2 : 16)) return 0;
  for (int i = 1; i < g_st_cap; i++)
    if (!g_st[i].used) return i;
  return 0;
}
static int check_dbid(int id) {
  return id > 0 && id < g_db_cap && g_db[id].used && g_db[id].db;
}
static int check_stid(int id) {
  return id > 0 && id < g_st_cap && g_st[id].used && g_st[id].st;
}

static int push_sqlite_err(VL_State *S, sqlite3 *db) {
  const char *e = db ? sqlite3_errmsg(db) : "EIO";
  vl_push_nil(S);
  vl_push_string(S, e && *e ? e : "EIO");
  return 2;
}

// -- Connexions

static int vlsql_open(VL_State *S) {
  const char *path = db_check_str(S, 1);
  int flags = db_opt_int(S, 2, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(path, &db, flags, NULL);
  if (rc != SQLITE_OK) {
    int ret = push_sqlite_err(S, db);
    if (db) sqlite3_close(db);
    return ret;
  }
  int id = alloc_db_slot();
  if (!id) {
    sqlite3_close(db);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_db[id].used = 1;
  g_db[id].db = db;
  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlsql_open_ex(VL_State *S) {
  const char *path = db_check_str(S, 1);
  int flags = (int)db_check_int(S, 2);
  const char *vfs = NULL;
  if (vl_get(S, 3) && vl_isstring(S, 3)) vfs = db_check_str(S, 3);
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(path, &db, flags, vfs);
  if (rc != SQLITE_OK) {
    int ret = push_sqlite_err(S, db);
    if (db) sqlite3_close(db);
    return ret;
  }
  int id = alloc_db_slot();
  if (!id) {
    sqlite3_close(db);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_db[id].used = 1;
  g_db[id].db = db;
  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlsql_close(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  if (!check_dbid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  // Finalize statements appartenant à ce db
  for (int i = 1; i < g_st_cap; i++) {
    if (g_st[i].used && g_st[i].dbid == id && g_st[i].st) {
      sqlite3_finalize(g_st[i].st);
      g_st[i].st = NULL;
      g_st[i].used = 0;
      g_st[i].dbid = 0;
    }
  }
  int rc = sqlite3_close(g_db[id].db);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[id].db);
  g_db[id].db = NULL;
  g_db[id].used = 0;
  vl_push_bool(S, 1);
  return 1;
}

static int vlsql_busy_timeout(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  int ms = (int)db_check_int(S, 2);
  if (!check_dbid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_busy_timeout(g_db[id].db, ms < 0 ? 0 : ms);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[id].db);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsql_errcode(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  if (!check_dbid(id)) {
    vl_push_int(S, 0);
    return 1;
  }
  vl_push_int(S, (int64_t)sqlite3_errcode(g_db[id].db));
  return 1;
}
static int vlsql_errmsg(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  if (!check_dbid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  const char *e = sqlite3_errmsg(g_db[id].db);
  vl_push_string(S, e ? e : "");
  return 1;
}
static int vlsql_changes(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  if (!check_dbid(id)) {
    vl_push_int(S, 0);
    return 1;
  }
  vl_push_int(S, (int64_t)sqlite3_changes(g_db[id].db));
  return 1;
}
static int vlsql_total_changes(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  if (!check_dbid(id)) {
    vl_push_int(S, 0);
    return 1;
  }
  vl_push_int(S, (int64_t)sqlite3_total_changes(g_db[id].db));
  return 1;
}
static int vlsql_last_insert_rowid(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  if (!check_dbid(id)) {
    vl_push_int(S, 0);
    return 1;
  }
  vl_push_int(S, (int64_t)sqlite3_last_insert_rowid(g_db[id].db));
  return 1;
}

// -- Exec

static int vlsql_exec(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  const char *sql = db_check_str(S, 2);
  if (!check_dbid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  char *err = NULL;
  int rc = sqlite3_exec(g_db[id].db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    const char *msg = err ? err : sqlite3_errmsg(g_db[id].db);
    vl_push_nil(S);
    vl_push_string(S, msg ? msg : "EIO");
    if (err) sqlite3_free(err);
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

// -- Préparés

static int vlsql_prepare(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  const char *sql = db_check_str(S, 2);
  if (!check_dbid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  sqlite3_stmt *st = NULL;
  int rc = sqlite3_prepare_v2(g_db[id].db, sql, -1, &st, NULL);
  if (rc != SQLITE_OK || !st) return push_sqlite_err(S, g_db[id].db);
  int sid = alloc_st_slot();
  if (!sid) {
    sqlite3_finalize(st);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_st[sid].used = 1;
  g_st[sid].st = st;
  g_st[sid].dbid = id;
  vl_push_int(S, (int64_t)sid);
  return 1;
}

static int vlsql_finalize(VL_State *S) {
  int sid = (int)db_check_int(S, 1);
  if (!check_stid(sid)) {
    vl_push_bool(S, 1);
    return 1;
  }
  sqlite3_stmt *st = g_st[sid].st;
  g_st[sid].st = NULL;
  g_st[sid].used = 0;
  g_st[sid].dbid = 0;
  int rc = sqlite3_finalize(st);
  if (rc != SQLITE_OK) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_bool(S, 1);
  return 1;
}

static int vlsql_reset(VL_State *S) {
  int sid = (int)db_check_int(S, 1);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_reset(g_st[sid].st);
  if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
    return push_sqlite_err(S, g_db[g_st[sid].dbid].db);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsql_clear_bindings(VL_State *S) {
  int sid = (int)db_check_int(S, 1);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_clear_bindings(g_st[sid].st);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[g_st[sid].dbid].db);
  vl_push_bool(S, 1);
  return 1;
}

static int vlsql_step(VL_State *S) {
  int sid = (int)db_check_int(S, 1);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_step(g_st[sid].st);
  vl_push_int(S, (int64_t)rc);  // 100=ROW, 101=DONE
  return 1;
}

// -- Bind

static int vlsql_bind_null(VL_State *S) {
  int sid = (int)db_check_int(S, 1), idx = (int)db_check_int(S, 2);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_bind_null(g_st[sid].st, idx);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[g_st[sid].dbid].db);
  vl_push_bool(S, 1);
  return 1;
}
static int vlsql_bind_int(VL_State *S) {
  int sid = (int)db_check_int(S, 1), idx = (int)db_check_int(S, 2);
  int64_t v = db_check_int(S, 3);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_bind_int64(g_st[sid].st, idx, (sqlite3_int64)v);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[g_st[sid].dbid].db);
  vl_push_bool(S, 1);
  return 1;
}
static int vlsql_bind_float(VL_State *S) {
  int sid = (int)db_check_int(S, 1), idx = (int)db_check_int(S, 2);
  double v = db_check_num(S, 3);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_bind_double(g_st[sid].st, idx, v);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[g_st[sid].dbid].db);
  vl_push_bool(S, 1);
  return 1;
}
static int vlsql_bind_text(VL_State *S) {
  int sid = (int)db_check_int(S, 1), idx = (int)db_check_int(S, 2);
  const char *s = db_check_str(S, 3);
  size_t n = strlen(s);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_bind_text(g_st[sid].st, idx, s, (int)n, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[g_st[sid].dbid].db);
  vl_push_bool(S, 1);
  return 1;
}
static int vlsql_bind_blob(VL_State *S) {
  int sid = (int)db_check_int(S, 1), idx = (int)db_check_int(S, 2);
  const char *s = db_check_str(S, 3);
  size_t n = strlen(s);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_bind_blob(g_st[sid].st, idx, s, (int)n, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[g_st[sid].dbid].db);
  vl_push_bool(S, 1);
  return 1;
}

// -- Columns

static int vlsql_column_count(VL_State *S) {
  int sid = (int)db_check_int(S, 1);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_int(S, (int64_t)sqlite3_column_count(g_st[sid].st));
  return 1;
}
static int vlsql_column_type(VL_State *S) {
  int sid = (int)db_check_int(S, 1), i = (int)db_check_int(S, 2);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_int(S, (int64_t)sqlite3_column_type(g_st[sid].st, i));
  return 1;
}
static int vlsql_column_name(VL_State *S) {
  int sid = (int)db_check_int(S, 1), i = (int)db_check_int(S, 2);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  const char *n = sqlite3_column_name(g_st[sid].st, i);
  if (!n) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_string(S, n);
  return 1;
}
static int vlsql_column_int(VL_State *S) {
  int sid = (int)db_check_int(S, 1), i = (int)db_check_int(S, 2);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_int(S, (int64_t)sqlite3_column_int64(g_st[sid].st, i));
  return 1;
}
static int vlsql_column_float(VL_State *S) {
  int sid = (int)db_check_int(S, 1), i = (int)db_check_int(S, 2);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_float(S, sqlite3_column_double(g_st[sid].st, i));
  return 1;
}
static int vlsql_column_text(VL_State *S) {
  int sid = (int)db_check_int(S, 1), i = (int)db_check_int(S, 2);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  const unsigned char *p = sqlite3_column_text(g_st[sid].st, i);
  int n = sqlite3_column_bytes(g_st[sid].st, i);
  if (!p) {
    vl_push_string(S, "");
    return 1;
  }
  vl_push_lstring(S, (const char *)p, n);
  return 1;
}
static int vlsql_column_blob(VL_State *S) {
  int sid = (int)db_check_int(S, 1), i = (int)db_check_int(S, 2);
  if (!check_stid(sid)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  const void *p = sqlite3_column_blob(g_st[sid].st, i);
  int n = sqlite3_column_bytes(g_st[sid].st, i);
  if (!p) {
    vl_push_lstring(S, "", 0);
    return 1;
  }
  vl_push_lstring(S, (const char *)p, n);
  return 1;
}

// -- Transactions

static int vlsql_txn_begin(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  int imm = db_opt_bool(S, 2, 1);
  if (!check_dbid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  const char *sql = imm ? "BEGIN IMMEDIATE" : "BEGIN";
  int rc = sqlite3_exec(g_db[id].db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[id].db);
  vl_push_bool(S, 1);
  return 1;
}
static int vlsql_txn_commit(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  if (!check_dbid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_exec(g_db[id].db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[id].db);
  vl_push_bool(S, 1);
  return 1;
}
static int vlsql_txn_rollback(VL_State *S) {
  int id = (int)db_check_int(S, 1);
  if (!check_dbid(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int rc = sqlite3_exec(g_db[id].db, "ROLLBACK", NULL, NULL, NULL);
  if (rc != SQLITE_OK) return push_sqlite_err(S, g_db[id].db);
  vl_push_bool(S, 1);
  return 1;
}

#endif  // VL_HAVE_SQLITE3

// ---------------------------------------------------------------------
// Enregistrement VM
// ---------------------------------------------------------------------

static const VL_Reg sqlitelib[] = {
    // Connexions
    {"open", vlsql_open},
    {"open_ex", vlsql_open_ex},
    {"close", vlsql_close},
    {"busy_timeout", vlsql_busy_timeout},
    {"errcode", vlsql_errcode},
    {"errmsg", vlsql_errmsg},
    {"changes", vlsql_changes},
    {"total_changes", vlsql_total_changes},
    {"last_insert_rowid", vlsql_last_insert_rowid},

    // Exec / préparés
    {"exec", vlsql_exec},
    {"prepare", vlsql_prepare},
    {"finalize", vlsql_finalize},
    {"reset", vlsql_reset},
    {"clear_bindings", vlsql_clear_bindings},
    {"step", vlsql_step},

    // Bind
    {"bind_null", vlsql_bind_null},
    {"bind_int", vlsql_bind_int},
    {"bind_float", vlsql_bind_float},
    {"bind_text", vlsql_bind_text},
    {"bind_blob", vlsql_bind_blob},

    // Columns
    {"column_count", vlsql_column_count},
    {"column_type", vlsql_column_type},
    {"column_name", vlsql_column_name},
    {"column_int", vlsql_column_int},
    {"column_float", vlsql_column_float},
    {"column_text", vlsql_column_text},
    {"column_blob", vlsql_column_blob},

    // Transactions
    {"txn_begin", vlsql_txn_begin},
    {"txn_commit", vlsql_txn_commit},
    {"txn_rollback", vlsql_txn_rollback},

    {NULL, NULL}};

void vl_open_sqlitelib(VL_State *S) { vl_register_lib(S, "sqlite", sqlitelib); }
