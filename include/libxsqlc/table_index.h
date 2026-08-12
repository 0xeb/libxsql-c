// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// table_index.h — the index (live) virtual table: a table whose rows are read
// fresh on every query by 0-based index, with typed columns, optional writable
// columns / INSERT / DELETE, and equality constraint pushdown via row iterators.
// Register the resulting table
// into a connection; the definition is CALLER-OWNED and must outlive every db it
// is registered into (it is never copied).

#ifndef XSQLC_TABLE_INDEX_H
#define XSQLC_TABLE_INDEX_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>
#include <libxsqlc/vtab.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward-declared so this header never pulls in <libxsqlc/database.h>.
struct xsql_db;

#ifdef __cplusplus
extern "C" {
#endif

// Opaque table definition. Built with xsql_table_new(); released with
// xsql_table_free(); must outlive every connection it is registered into.
typedef struct xsql_table xsql_table;

// ---------------------------------------------------------------------------
// Callback signatures. Each takes the per-registration `user` pointer last.
// ---------------------------------------------------------------------------

// xsql_row_count_fn / xsql_estimate_rows_fn are declared in <libxsqlc/vtab.h>
// (shared across table flavors).
//
// Modification hook: `op` is a short verb phrase (e.g. "INSERT INTO t").
typedef void (*xsql_on_modify_fn)(const char *op, void *user);

// Typed read getters: return the cell value for row index `row`.
typedef int64_t (*xsql_col_i64_fn)(size_t row, void *user);
typedef int (*xsql_col_int_fn)(size_t row, void *user);
typedef double (*xsql_col_double_fn)(size_t row, void *user);
// Text/blob getters emit through the context so they can produce SQL NULL and
// choose static/transient copy semantics (call xsql_result_text/blob/null).
typedef void (*xsql_col_emit_fn)(xsql_context *ctx, size_t row, void *user);

// Typed write setters: apply a new value at row index `row`; return true on
// success, or false after calling xsql_vtab_set_error() to report why.
typedef bool (*xsql_col_set_i64_fn)(size_t row, int64_t val, void *user);
typedef bool (*xsql_col_set_int_fn)(size_t row, int val, void *user);
typedef bool (*xsql_col_set_text_fn)(size_t row, const char *val, void *user);
// Nullable setter receives the raw value so it can detect a SQL NULL assignment.
typedef bool (*xsql_col_set_value_fn)(size_t row, xsql_value *val, void *user);

// DELETE handler: remove row index `row`.
typedef bool (*xsql_delete_fn)(size_t row, void *user);
// INSERT handler: create a row from `argc` column values.
typedef bool (*xsql_insert_fn)(int argc, xsql_value **argv, void *user);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

XSQL_API xsql_table *xsql_table_new(const char *name);
XSQL_API void xsql_table_free(xsql_table *t);

// ---------------------------------------------------------------------------
// Table-level configuration
// ---------------------------------------------------------------------------

XSQL_API void xsql_table_count(xsql_table *t, xsql_row_count_fn fn, void *user);
XSQL_API void xsql_table_estimate_rows(xsql_table *t, xsql_estimate_rows_fn fn,
                                       void *user);
XSQL_API void xsql_table_on_modify(xsql_table *t, xsql_on_modify_fn fn, void *user);
// Install the full transaction lifecycle. The hooks are copied; `user` is
// borrowed and must outlive every database using this definition. Passing NULL
// clears the lifecycle. Returns XSQL_OK or XSQL_MISUSE.
XSQL_API int xsql_table_transaction_hooks(
    xsql_table *t, const xsql_transaction_hooks *hooks, void *user);

// ---------------------------------------------------------------------------
// Columns (declared in order; the column index is its position). Read-only
// registrars add a column; the *_rw variants also make it writable. Each
// returns XSQL_OK, or XSQL_NO_MEMORY on allocation failure.
// ---------------------------------------------------------------------------

XSQL_API int xsql_table_col_int(xsql_table *t, const char *name,
                                xsql_col_int_fn get, void *user);
XSQL_API int xsql_table_col_i64(xsql_table *t, const char *name,
                                xsql_col_i64_fn get, void *user);
XSQL_API int xsql_table_col_double(xsql_table *t, const char *name,
                                   xsql_col_double_fn get, void *user);
XSQL_API int xsql_table_col_text(xsql_table *t, const char *name,
                                 xsql_col_emit_fn get, void *user);
XSQL_API int xsql_table_col_blob(xsql_table *t, const char *name,
                                 xsql_col_emit_fn get, void *user);

XSQL_API int xsql_table_col_int_rw(xsql_table *t, const char *name,
                                   xsql_col_int_fn get, xsql_col_set_int_fn set,
                                   void *user);
XSQL_API int xsql_table_col_i64_rw(xsql_table *t, const char *name,
                                   xsql_col_i64_fn get, xsql_col_set_i64_fn set,
                                   void *user);
XSQL_API int xsql_table_col_text_rw(xsql_table *t, const char *name,
                                    xsql_col_emit_fn get,
                                    xsql_col_set_text_fn set, void *user);
// Nullable text column whose setter receives the raw value (to detect NULL).
XSQL_API int xsql_table_col_text_nullable_rw(xsql_table *t, const char *name,
                                             xsql_col_emit_fn get,
                                             xsql_col_set_value_fn set,
                                             void *user);

// ---------------------------------------------------------------------------
// Write capability
// ---------------------------------------------------------------------------

XSQL_API void xsql_table_deletable(xsql_table *t, xsql_delete_fn fn, void *user);
XSQL_API void xsql_table_insertable(xsql_table *t, xsql_insert_fn fn, void *user);

// ---------------------------------------------------------------------------
// Constraint pushdown. Register an equality filter on a column: a WHERE
// column = ? uses the factory's iterator instead of a full scan. `filter_eq`
// and `filter_eq_text` are the same EQ pushdown — the factory reads `arg` as an
// integer or as text respectively. Returns XSQL_OK, XSQL_NO_MEMORY, or
// XSQL_NOT_FOUND if `column` is not a declared column.
// ---------------------------------------------------------------------------

XSQL_API int xsql_table_filter_eq(xsql_table *t, const char *column, double cost,
                                  double est_rows, xsql_filter_factory_fn factory,
                                  void *user);
XSQL_API int xsql_table_filter_eq_text(xsql_table *t, const char *column,
                                       double cost, double est_rows,
                                       xsql_filter_factory_fn factory,
                                       void *user);

// ---------------------------------------------------------------------------
// Registration into a connection. The table definition is not copied; it must
// outlive `db`. `xsql_table_register` registers a module named after the table
// and creates the virtual table under the same name; `_register_as` creates it
// under `table_name` (using a module of the same name). Lower-level:
// `xsql_table_register_module` registers only the module; `xsql_db_create_table`
// issues the CREATE VIRTUAL TABLE. All return an xsql_status.
// ---------------------------------------------------------------------------

XSQL_API int xsql_table_register(struct xsql_db *db, xsql_table *t);
XSQL_API int xsql_table_register_as(struct xsql_db *db, xsql_table *t,
                                    const char *table_name);
XSQL_API int xsql_table_register_module(struct xsql_db *db, xsql_table *t);
XSQL_API int xsql_db_create_table(struct xsql_db *db, const char *table_name,
                                  const char *module_name);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_TABLE_INDEX_H
