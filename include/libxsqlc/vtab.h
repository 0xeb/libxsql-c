// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// vtab.h — the common virtual-table surface shared by every table flavor
// (index / cached / generator): column types, the constraint-pushdown row
// iterator and filter descriptor, the thread-local error channel, and
// cooperative-cancellation hooks. Include a specific table header
// (<libxsqlc/table_index.h>, …) to
// build and register a table.

#ifndef XSQLC_VTAB_H
#define XSQLC_VTAB_H

#include <libxsqlc/common.h>
#include <libxsqlc/value.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Declared column storage class (drives the synthesized CREATE TABLE DDL).
typedef enum xsql_column_type {
    XSQL_COL_INTEGER = 0,
    XSQL_COL_TEXT    = 1,
    XSQL_COL_REAL    = 2,
    XSQL_COL_BLOB    = 3
} xsql_column_type;

// Constraint operator a filter matches. EQ filters are applied with omit=1
// (SQLite trusts the iterator); LIKE/GLOB are best-effort supersets applied with
// omit=0 (SQLite re-checks the real pattern) and are used by the cached
// prefix filter in a later phase.
typedef enum xsql_filter_op {
    XSQL_FILTER_EQ   = 0,
    XSQL_FILTER_LIKE = 1,
    XSQL_FILTER_GLOB = 2
} xsql_filter_op;

// ---------------------------------------------------------------------------
// Row iterator — the constraint-pushdown fast path (C analog of RowIterator)
// ---------------------------------------------------------------------------
//
// A filter factory fills one of these to iterate only the rows matching a
// constraint (e.g. WHERE key = ?), instead of a full scan. The framework primes
// the iterator with one next() call, then drives it via next/eof/column/rowid,
// and finally calls destroy() (if set) to release `self`.
typedef struct xsql_row_iter {
    // Advance to the next row; return true if a row is now current, false when
    // exhausted. Must be usable as the priming call before the first access.
    bool (*next)(void *self);
    // True if the iterator is exhausted (no current row).
    bool (*eof)(void *self);
    // Emit the current row's column `col` through `ctx`.
    void (*column)(void *self, xsql_context *ctx, int col);
    // Current row's rowid.
    int64_t (*rowid)(void *self);
    // Release `self` (may be NULL if there is nothing to free).
    void (*destroy)(void *self);
    // Opaque iterator state, passed to every callback above.
    void *self;
} xsql_row_iter;

// Filter factory: given the constraint value `arg`, fill `*out` with an iterator
// and return true; return false to mean "no matching rows" (an empty result).
typedef bool (*xsql_filter_factory_fn)(xsql_value *arg, xsql_row_iter *out,
                                       void *user);

// Shared table callbacks (used by every table flavor). Each takes the
// per-registration `user` pointer last.
typedef size_t (*xsql_row_count_fn)(void *user);       // live row count
typedef size_t (*xsql_estimate_rows_fn)(void *user);   // cheap planning estimate

// ---------------------------------------------------------------------------
// Transaction lifecycle
// ---------------------------------------------------------------------------
//
// SQLite propagates xSync failures but ignores xCommit/xRollback failures.
// Accordingly, prepare_commit/savepoint/release/rollback_to return an xsql_status
// while commit/rollback are deliberately infallible finish callbacks.
//
// A table definition can be registered on multiple connections. state_new runs
// once per registration and returns connection-local state; state_free releases
// it when that virtual-table registration is disconnected. Every other callback
// receives that state plus the registration-time `user` pointer supplied to the
// table-specific *_transaction_hooks() function.
typedef int (*xsql_transaction_state_new_fn)(void **state_out, void *user);
typedef void (*xsql_transaction_state_free_fn)(void *state, void *user);
typedef int (*xsql_transaction_prepare_fn)(void *state, void *user);
typedef void (*xsql_transaction_finish_fn)(void *state, void *user);
typedef int (*xsql_transaction_savepoint_fn)(void *state, int savepoint,
                                             void *user);

typedef struct xsql_transaction_hooks {
    size_t struct_size;  // sizeof(xsql_transaction_hooks)
    xsql_transaction_state_new_fn  state_new;
    xsql_transaction_state_free_fn state_free;
    xsql_transaction_prepare_fn    prepare_commit;
    xsql_transaction_finish_fn     commit;
    xsql_transaction_finish_fn     rollback;
    xsql_transaction_savepoint_fn  savepoint;
    xsql_transaction_savepoint_fn  release;
    xsql_transaction_savepoint_fn  rollback_to;
} xsql_transaction_hooks;

// Filter descriptor (C analog of FilterDef). Designed for designated
// initializers; `struct_size` lets the ABI grow. `column_index` is resolved by
// the name-based registrars, so callers rarely fill this directly.
typedef struct xsql_filter_spec {
    size_t         struct_size;   // sizeof(xsql_filter_spec), or 0 for defaults
    int            column_index;  // column the filter constrains
    xsql_filter_op op;            // constraint operator (default EQ)
    double         cost;          // query-planner cost estimate
    double         est_rows;      // estimated matching rows
} xsql_filter_spec;

// ---------------------------------------------------------------------------
// Virtual-table error channel (thread-local; cleared by the framework around
// each callback). Call from inside a column setter / insert / delete callback
// to report a message that surfaces as the statement's SQLite error text.
// ---------------------------------------------------------------------------
XSQL_API void xsql_vtab_set_error(const char *fmt, ...);

// ---------------------------------------------------------------------------
// Cooperative cancellation. A long cache-builder / iterator loop that never
// yields to the VDBE can poll xsql_vtab_interrupted() and bail out (via
// xsql_vtab_set_error) when the embedder-installed predicate reports the query
// has exceeded its deadline. Both the predicate and the poll are thread-local.
// ---------------------------------------------------------------------------
typedef bool (*xsql_interrupt_fn)(void *user);

XSQL_API void xsql_vtab_set_interrupt_checker(xsql_interrupt_fn fn, void *user);
XSQL_API void xsql_vtab_clear_interrupt_checker(void);
XSQL_API bool xsql_vtab_interrupted(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_VTAB_H
