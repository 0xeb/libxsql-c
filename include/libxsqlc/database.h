// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// database.h — the connection handle: open/close, exec, prepare, the scalar
// convenience, and scalar/aggregate function registration.

#ifndef XSQLC_DATABASE_H
#define XSQLC_DATABASE_H

#include <libxsqlc/common.h>
#include <libxsqlc/result.h>
#include <libxsqlc/status.h>
#include <libxsqlc/statement.h>
#include <libxsqlc/value.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Raw escape-hatch handle: forward-declared so this header never pulls in
// sqlite3.h.
struct sqlite3;

#ifdef __cplusplus
extern "C" {
#endif

// Opaque connection handle. Created by xsql_open()/xsql_open_memory(); released
// with xsql_close().
typedef struct xsql_db xsql_db;

// Callback types.
//
// A scalar function receives its arguments as an array of borrowed values and
// writes its result (or an error) through the context.
typedef void (*xsql_scalar_fn)(xsql_context *ctx, int argc, xsql_value **argv,
                               void *user);
// An aggregate steps once per input row, then finalizes once. Per-aggregation
// state lives in xsql_aggregate_state(ctx).
typedef void (*xsql_aggregate_step_fn)(xsql_context *ctx, int argc,
                                       xsql_value **argv, void *user);
typedef void (*xsql_aggregate_final_fn)(xsql_context *ctx, void *user);
// sqlite3_exec-style row callback: return nonzero to abort the exec.
typedef int (*xsql_exec_callback)(void *user, int ncol, char **values,
                                  char **names);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Open a database at `path` (a filename, ":memory:", or "" for a private temp
// db). The built-in blob_concat aggregate is auto-registered on success. On
// return *out holds a handle to close with xsql_close() even on failure (so the
// error text is retrievable) — check the returned status. Returns an
// xsql_status.
XSQL_API int xsql_open(const char *path, xsql_db **out);

// Convenience: open a fresh private in-memory database (":memory:").
XSQL_API int xsql_open_memory(xsql_db **out);

// Close and free the handle. Passing NULL is a no-op.
XSQL_API void xsql_close(xsql_db *db);

// True while a connection is open.
XSQL_API bool xsql_db_is_open(const xsql_db *db);

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

// Execute one or more semicolon-separated statements, discarding any rows.
XSQL_API int xsql_db_exec(xsql_db *db, const char *sql);

// Execute with a per-row callback (sqlite3_exec semantics). Returns the raw
// result code (XSQL_OK, XSQL_ABORT if the callback returned nonzero, or an
// error).
XSQL_API int xsql_db_exec_callback(xsql_db *db, const char *sql,
                                   xsql_exec_callback cb, void *user);

// Prepare a single statement. On success *out receives a statement to finalize
// with xsql_stmt_finalize(); on failure *out is set to NULL. Returns an
// xsql_status.
XSQL_API int xsql_db_prepare(xsql_db *db, const char *sql, xsql_stmt **out);

// True if `sql` prepares to a read-only statement (false on a prepare error).
XSQL_API bool xsql_db_is_readonly_sql(xsql_db *db, const char *sql);

// Run `sql` and return the first column of the first row as heap text (free
// with xsql_free()). Returns an empty string when there are no rows, the cell
// is NULL, or the query errors (it runs the
// same xsql_db_query path). Returns NULL only on allocation failure.
XSQL_API char *xsql_db_scalar(xsql_db *db, const char *sql);

// ---------------------------------------------------------------------------
// Queries — the materialized xsql_result envelope (see result.h)
// ---------------------------------------------------------------------------

// Caller-filled query options. Designed for designated initializers; an all-zero
// struct (or a NULL pointer to xsql_db_query_opts) selects defaults. `struct_size`
// lets the ABI grow compatibly — set it to sizeof(xsql_query_opts), or leave the
// whole struct zeroed.
//
// `timeout_ms` sets a real query deadline (0 = no timeout, the default). When it
// trips, a read-only result statement keeps the rows gathered so far and reports
// xsql_result_timed_out()==true / xsql_result_partial()==true with a warning; a
// mutation (including DML with RETURNING) reports a hard timeout error and its
// provisional rows are discarded.
// `progress_steps` is the VDBE-opcode interval between deadline checks (<=0 uses
// the default of 1000). The deadline is monotonic (steady-clock based).
//
// `should_cancel` (+ `should_cancel_user`) is an optional cooperative-cancellation
// predicate — the C-native closure idiom (function pointer + void* user-data) for
// the C++ QueryOptions::should_cancel std::function. When set and it returns
// nonzero, an in-flight query stops ASAP: a read-only result statement keeps the
// rows gathered so far (xsql_result_partial()==true) with a "query cancelled"
// warning and is NOT flagged timed_out; a mutation reports a hard "Query
// cancelled" error. It works even when timeout_ms == 0, so a server
// can abort a runaway query (POST /cancel or a client disconnect). These are
// ABI-growth fields (read only when struct_size covers them). NULL => never
// cancelled.
typedef struct xsql_query_opts {
    size_t struct_size;     // sizeof(xsql_query_opts), or 0 for all defaults
    int    timeout_ms;      // query deadline in milliseconds (0 = no timeout)
    int    progress_steps;  // VDBE opcode interval between checks (<=0 => 1000)
    // ABI-growth: cooperative cancellation (function pointer + user-data). A nonzero
    // return cancels the query (partial rows only for read-only SQL).
    int  (*should_cancel)(void *user);
    void  *should_cancel_user;
} xsql_query_opts;

// Run `sql` and gather the whole result set into a xsql_result (release with
// xsql_result_free()). On success *out receives a result whose xsql_result_ok()
// is true; on a prepare/step error *out still receives a valid, freeable result
// carrying the error text (xsql_result_error()) — errors are reported through
// the envelope, never thrown. Returns XSQL_OK when the query succeeded, else the
// underlying error status. Returns XSQL_NO_MEMORY with *out set to NULL only on
// allocation failure, and XSQL_MISUSE if `out` is NULL.
XSQL_API int xsql_db_query(xsql_db *db, const char *sql, xsql_result **out);

// As xsql_db_query() with explicit options. `opts` may be NULL (defaults). A
// nonzero opts->timeout_ms installs a real deadline (see xsql_query_opts): a
// partial read timeout returns XSQL_OK with xsql_result_partial() true; a
// mutation timeout returns an error status with the timeout error text.
XSQL_API int xsql_db_query_opts(xsql_db *db, const char *sql,
                                const xsql_query_opts *opts, xsql_result **out);

// ---------------------------------------------------------------------------
// Function registration
//
// `destroy` (may be NULL) is invoked with `user` when the function is
// unregistered or the connection closes — and, per SQLite semantics, even if
// registration itself fails, so the library never double-frees `user`.
// ---------------------------------------------------------------------------

XSQL_API int xsql_db_create_function(xsql_db *db, const char *name, int argc,
                                     xsql_scalar_fn fn, void *user,
                                     void (*destroy)(void *));

XSQL_API int xsql_db_create_aggregate(xsql_db *db, const char *name, int argc,
                                      xsql_aggregate_step_fn step,
                                      xsql_aggregate_final_fn final, void *user,
                                      void (*destroy)(void *));

// ---------------------------------------------------------------------------
// Introspection / escape hatch
// ---------------------------------------------------------------------------

XSQL_API int64_t xsql_db_last_insert_rowid(const xsql_db *db);
XSQL_API int     xsql_db_changes(const xsql_db *db);

// Last error text for this connection (borrowed; valid until the next API call
// on the same db). Never NULL.
XSQL_API const char *xsql_db_errmsg(const xsql_db *db);

// Raw escape hatch: the underlying sqlite3* (borrowed). NULL if closed.
XSQL_API struct sqlite3 *xsql_db_handle(xsql_db *db);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_DATABASE_H
