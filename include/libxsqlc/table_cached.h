// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// table_cached.h — the cached virtual table: rows are materialized once into a
// contiguous cache (fixed-size records of `row_size` bytes) and reused across
// queries, with lazy hash indexes, constraint/prefix pushdown, a COUNT(*) fast
// path, and full UPDATE/DELETE/INSERT with rowid/positional reconstruction. Rows
// are opaque:
// getters take `const void *row`, setters `void *row` (cast to your record type);
// any indirect data (strings, blobs) is consumer-owned (e.g. in a stable arena),
// since the cache copies `row_size` bytes shallowly. The definition is
// CALLER-OWNED and must outlive every db it is registered into.

#ifndef XSQLC_TABLE_CACHED_H
#define XSQLC_TABLE_CACHED_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>
#include <libxsqlc/vtab.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward-declared so this header never pulls in <libxsqlc/database.h> or
// <sqlite3.h>.
struct xsql_db;
struct sqlite3;

#ifdef __cplusplus
extern "C" {
#endif

// Opaque cached-table definition (built, then registered; must outlive its dbs).
typedef struct xsql_cached_table xsql_cached_table;

// Opaque cache-fill handle passed to a cache builder. Append one record per row.
typedef struct xsql_rowset xsql_rowset;

// Copy `row` (row_size bytes) as the next cache row. Returns false on OOM.
XSQL_API bool xsql_rowset_push(xsql_rowset *rs, const void *row);

// ---------------------------------------------------------------------------
// Callback signatures (each takes the per-registration `user` pointer last).
// ---------------------------------------------------------------------------

// Materialize all rows into the cache via xsql_rowset_push.
typedef void (*xsql_cache_builder_fn)(xsql_rowset *rs, void *user);
// Connection-state-aware variant. `transaction_state` is the state created by
// xsql_transaction_hooks::state_new for this registration. Installing this
// builder automatically selects a query-scoped (non-shared) cache.
typedef void (*xsql_stateful_cache_builder_fn)(xsql_rowset *rs,
                                               void *transaction_state,
                                               void *user);
// Projection-aware variant (non-shared tables only): `col_used` is SQLite's
// colUsed bitmask (bit i => column i is read; bit 63 => >=63 or unknown).
typedef void (*xsql_projection_cache_builder_fn)(xsql_rowset *rs,
                                                 uint64_t col_used, void *user);
// Modification hooks: `op` is a short verb phrase (e.g. "UPDATE t").
typedef void (*xsql_cached_modify_fn)(const char *op, void *user);

// Typed read getters over a cached record.
typedef int64_t (*xsql_crow_i64_fn)(const void *row, void *user);
typedef int (*xsql_crow_int_fn)(const void *row, void *user);
typedef double (*xsql_crow_double_fn)(const void *row, void *user);
// Raw ctx-emitting getter (text/blob/nullable — call xsql_result_text/blob/null).
typedef void (*xsql_crow_emit_fn)(xsql_context *ctx, const void *row, void *user);

// Typed write setters (return true, or false after xsql_vtab_set_error()).
typedef bool (*xsql_crow_set_i64_fn)(void *row, int64_t val, void *user);
typedef bool (*xsql_crow_set_int_fn)(void *row, int val, void *user);
typedef bool (*xsql_crow_set_text_fn)(void *row, const char *val, void *user);
// Value-arg setter (receives the raw value — detect SQL NULL, blobs, etc.).
typedef bool (*xsql_crow_set_value_fn)(void *row, xsql_value *val, void *user);

// Write handlers over a cached record.
typedef bool (*xsql_crow_delete_fn)(void *row, void *user);
typedef bool (*xsql_cached_insert_fn)(int argc, xsql_value **argv, void *user);
// Fill `row` from INSERT/UPDATE argv (argv[0]=rowid, column values at argv[2..]).
typedef void (*xsql_row_populator_fn)(void *row, int argc, xsql_value **argv,
                                      void *user);
// Resolve `row` from a rowid; return true if found.
typedef bool (*xsql_row_lookup_fn)(void *row, int64_t rowid, void *user);
// Stable rowid for a cached record (used by full-scan/index cursors).
typedef int64_t (*xsql_cached_rowid_fn)(const void *row, void *user);
// Extract the int64 index key from a cached record (for index_on).
typedef int64_t (*xsql_key_extractor_fn)(const void *row, void *user);
// Constraint-filter factory (xsql_cached_constraint_filter): the matched
// constraints arrive in spec order, each with its column and operator. Fill
// `*out` with an iterator over EXACTLY the rows satisfying all of them (SQLite
// does not re-check consumed constraints); return false for no rows.
typedef bool (*xsql_cached_constraint_fn)(int argc,
                                          const xsql_constraint_arg *args,
                                          xsql_row_iter *out, void *user);

// ---------------------------------------------------------------------------
// Lifecycle + table configuration
// ---------------------------------------------------------------------------

// Create a cached table whose records are `row_size` bytes each.
XSQL_API xsql_cached_table *xsql_cached_table_new(const char *name,
                                                  size_t row_size);
XSQL_API void xsql_cached_table_free(xsql_cached_table *c);

XSQL_API void xsql_cached_estimate_rows(xsql_cached_table *c,
                                        xsql_estimate_rows_fn fn, void *user);
XSQL_API void xsql_cached_count(xsql_cached_table *c, xsql_row_count_fn fn,
                                void *user);
XSQL_API void xsql_cached_cache_builder(xsql_cached_table *c,
                                        xsql_cache_builder_fn fn, void *user);
XSQL_API void xsql_cached_stateful_cache_builder(
    xsql_cached_table *c, xsql_stateful_cache_builder_fn fn, void *user);
XSQL_API void xsql_cached_projection_cache_builder(
    xsql_cached_table *c, xsql_projection_cache_builder_fn fn, void *user);
// Opt out of the persistent shared cache (rebuild a query-scoped cache instead).
XSQL_API void xsql_cached_no_shared_cache(xsql_cached_table *c);
XSQL_API void xsql_cached_on_modify(xsql_cached_table *c, xsql_cached_modify_fn fn,
                                    void *user);
XSQL_API void xsql_cached_after_modify(xsql_cached_table *c,
                                       xsql_cached_modify_fn fn, void *user);
// Install the full transaction lifecycle. The hooks are copied; `user` is
// borrowed and must outlive every database using this definition. Passing NULL
// clears the lifecycle. Returns XSQL_OK or XSQL_MISUSE.
XSQL_API int xsql_cached_transaction_hooks(
    xsql_cached_table *c, const xsql_transaction_hooks *hooks, void *user);
// Whether this table's row writes bump xsql_db_vtab_write_count() (default:
// true); a session/bookkeeping table passes false.
XSQL_API void xsql_cached_counts_as_write(xsql_cached_table *c, bool value);

// ---------------------------------------------------------------------------
// Columns (declared in order). Each returns XSQL_OK / XSQL_NO_MEMORY.
// ---------------------------------------------------------------------------

// Raw ctx-emitting read-only column of an explicit type.
XSQL_API int xsql_cached_col(xsql_cached_table *c, const char *name,
                             xsql_column_type type, xsql_crow_emit_fn get,
                             void *user);
XSQL_API int xsql_cached_col_int(xsql_cached_table *c, const char *name,
                                 xsql_crow_int_fn get, void *user);
XSQL_API int xsql_cached_col_i64(xsql_cached_table *c, const char *name,
                                 xsql_crow_i64_fn get, void *user);
XSQL_API int xsql_cached_col_double(xsql_cached_table *c, const char *name,
                                    xsql_crow_double_fn get, void *user);
XSQL_API int xsql_cached_col_text(xsql_cached_table *c, const char *name,
                                  xsql_crow_emit_fn get, void *user);
XSQL_API int xsql_cached_col_blob(xsql_cached_table *c, const char *name,
                                  xsql_crow_emit_fn get, void *user);
// Read-only nullable integer column: the emit getter may emit an integer or SQL
// NULL. Read-only sibling of xsql_cached_col_int_nullable_rw (getter only).
XSQL_API int xsql_cached_col_int_nullable(xsql_cached_table *c, const char *name,
                                          xsql_crow_emit_fn get, void *user);
// Read-only nullable text column: the emit getter may emit text or SQL NULL.
// Read-only sibling of xsql_cached_col_text_nullable_rw (getter only).
XSQL_API int xsql_cached_col_text_nullable(xsql_cached_table *c, const char *name,
                                           xsql_crow_emit_fn get, void *user);

// A typed integer setter (int / i64 *_rw) is called only with the integer the
// written value equals under SQLite's INTEGER comparison: 16, 16.0 and '16'
// all arrive as 16. Anything else -- NULL, a blob, 16.5, '0x10', 'abc', or for
// the int variant an integer outside int32 -- refuses the write with
// `column "<name>": expected an integer, got ...` (or `... does not fit a
// 32-bit integer`) and the setter does not run. A raw-value variant receives
// the value as written.
XSQL_API int xsql_cached_col_int_rw(xsql_cached_table *c, const char *name,
                                    xsql_crow_int_fn get,
                                    xsql_crow_set_int_fn set, void *user);
XSQL_API int xsql_cached_col_i64_rw(xsql_cached_table *c, const char *name,
                                    xsql_crow_i64_fn get,
                                    xsql_crow_set_i64_fn set, void *user);
XSQL_API int xsql_cached_col_text_rw(xsql_cached_table *c, const char *name,
                                     xsql_crow_emit_fn get,
                                     xsql_crow_set_text_fn set, void *user);
// Nullable text column whose setter receives the raw value (to detect NULL).
XSQL_API int xsql_cached_col_text_nullable_rw(xsql_cached_table *c,
                                              const char *name,
                                              xsql_crow_emit_fn get,
                                              xsql_crow_set_value_fn set,
                                              void *user);
// Nullable integer column: the emit getter may emit an integer or SQL NULL, and
// the setter receives the raw value (to detect NULL). Mirrors
// xsql_cached_col_text_nullable_rw for the integer type — used where a
// mapped-but-unknown value must read back as NULL rather than 0.
XSQL_API int xsql_cached_col_int_nullable_rw(xsql_cached_table *c,
                                             const char *name,
                                             xsql_crow_emit_fn get,
                                             xsql_crow_set_value_fn set,
                                             void *user);
// Value-arg writable overloads (setter receives the raw value).
XSQL_API int xsql_cached_col_int_rw_value(xsql_cached_table *c, const char *name,
                                          xsql_crow_int_fn get,
                                          xsql_crow_set_value_fn set, void *user);
XSQL_API int xsql_cached_col_i64_rw_value(xsql_cached_table *c, const char *name,
                                          xsql_crow_i64_fn get,
                                          xsql_crow_set_value_fn set, void *user);
XSQL_API int xsql_cached_col_text_rw_value(xsql_cached_table *c, const char *name,
                                           xsql_crow_emit_fn get,
                                           xsql_crow_set_value_fn set,
                                           void *user);

// ---------------------------------------------------------------------------
// Pushdown, indexes, reconstruction, write capability
// ---------------------------------------------------------------------------

// Equality pushdown (factory reads `arg` as int / text respectively). The
// constraint is consumed, so the value is compared the way SQLite compares the
// column: the int factory is called only for a value an integer can equal
// (never 10.5, 'a', NULL or a blob, which yield no rows), the text factory
// never for NULL or a blob.
XSQL_API int xsql_cached_filter_eq(xsql_cached_table *c, const char *column,
                                   double cost, double est_rows,
                                   xsql_filter_factory_fn factory, void *user);
XSQL_API int xsql_cached_filter_eq_text(xsql_cached_table *c, const char *column,
                                        double cost, double est_rows,
                                        xsql_filter_factory_fn factory,
                                        void *user);
// Prefix (LIKE) pushdown: the iterator returns a superset; SQLite re-checks the
// pattern. Only claimed when the pattern has a usable literal prefix.
XSQL_API int xsql_cached_filter_prefix(xsql_cached_table *c, const char *column,
                                       double cost, double est_rows,
                                       xsql_filter_factory_fn factory,
                                       void *user);
// Lazy hash index on an int64 column (built with the shared cache on first use).
// Also serves range (>=, >, <=, <, BETWEEN) pushdown and supplies the comparison
// value for xsql_cached_constraint_cache_filter. At most 1000 per table (the
// plan encoding's space); returns XSQL_MISUSE beyond that.
XSQL_API int xsql_cached_index_on(xsql_cached_table *c, const char *column,
                                  xsql_key_extractor_fn key, void *user);

// Multi-column / multi-operator pushdown, for predicates a single-column filter
// cannot express, e.g. interval containment (`p.v >= i.start AND p.v < i.end`),
// which SQLite would otherwise run as a cross product. Build `reqs` with the
// xsql_required_* / xsql_optional_* helpers from <libxsqlc/vtab.h>. xBestIndex
// picks the cheapest filter whose required specs are all present (ties go to
// the one consuming more constraints); it must also be strictly cheaper than
// the best equality filter or shared-cache index lookup for the same query. A
// missing required spec is not an error: another plan is used. Consumed
// constraints are omitted from SQLite's re-check, so the result must be exact.
//
// Factory-backed: the factory resolves the predicate against its source and
// streams only the matching rows; nothing is materialized. Returns XSQL_OK,
// XSQL_MISUSE (NULL table/factory, no specs), XSQL_NOT_FOUND (unknown column),
// or XSQL_NO_MEMORY.
XSQL_API int xsql_cached_constraint_filter(xsql_cached_table *c,
                                           const xsql_constraint_req *reqs,
                                           size_t nreqs, double cost,
                                           double est_rows,
                                           xsql_cached_constraint_fn factory,
                                           void *user);
// Cache-backed: the predicate is evaluated in memory over the table's own rows,
// the engine-lifetime cache for a shared table or a cursor-lived build for a
// query-scoped one. Every referenced column must already have an index_on(),
// whose key supplies the comparison value; comparisons follow SQLite ordering
// (a NULL value never matches, a numeric string compares as its number, other
// text and blobs sort after every integer). Returns XSQL_OK, XSQL_MISUSE (NULL
// table, no specs, a column without index_on, or a LIKE spec, which an integer
// key cannot evaluate), XSQL_NOT_FOUND (unknown column), or XSQL_NO_MEMORY. The
// plan is only offered for tables that have rows to evaluate (a shared cache or
// a cache builder).
XSQL_API int xsql_cached_constraint_cache_filter(xsql_cached_table *c,
                                                 const xsql_constraint_req *reqs,
                                                 size_t nreqs, double cost,
                                                 double est_rows);

// Row identity. A rowid is either a KEY (reported by filter iterators, and by
// full and index scans when a rowid fn is set) or, for a table with neither
// rowid fn nor row lookup, a cache POSITION.
//
// Stable rowid (a KEY) for full-scan and index cursors, so they report the same
// rowid the filter iterators do; `WHERE rowid = ?` then matches it by key.
// Mandatory with xsql_cached_row_lookup: it must return the key the filter
// iterators report and the row lookup resolves.
XSQL_API void xsql_cached_rowid(xsql_cached_table *c, xsql_cached_rowid_fn fn,
                                void *user);
// Resolve a rowid (a KEY, never a cache position) back to its row. This becomes
// the SOLE authority for UPDATE, DELETE and `WHERE rowid = ?`: a rowid it cannot
// resolve (e.g. a row gone mid-statement) fails the write with "row <id> not
// found in <table>" (or the reason the lookup set via xsql_vtab_set_error) and
// matches no row on read -- never another row by position, never the populator.
// Negative rowids are passed through (keys may be negative).
// REQUIRES xsql_cached_rowid: registration of a table with a row lookup but no
// rowid fn fails with XSQL_MISUSE and says why in xsql_db_errmsg.
XSQL_API void xsql_cached_row_lookup(xsql_cached_table *c, xsql_row_lookup_fn fn,
                                     void *user);
// Reconstruct an UPDATE's row from argv. With a row lookup set it is used only
// by an opted-out table (xsql_cached_update_from_column_values).
XSQL_API void xsql_cached_row_populator(xsql_cached_table *c,
                                        xsql_row_populator_fn fn, void *user);
// Opt out of rowid-based UPDATE reconstruction + the NOCHANGE optimization: the
// row is reconstructed from the real column values in argv (the populator).
// DELETE still resolves through the row lookup when the table has one.
XSQL_API void xsql_cached_update_from_column_values(xsql_cached_table *c,
                                                    bool value);
XSQL_API void xsql_cached_deletable(xsql_cached_table *c, xsql_crow_delete_fn fn,
                                    void *user);
XSQL_API void xsql_cached_insertable(xsql_cached_table *c,
                                     xsql_cached_insert_fn fn, void *user);

// ---------------------------------------------------------------------------
// Registration (the def is not copied; it must outlive `db`). Each registrar
// returns XSQL_MISUSE for a definition that breaks the row-identity rule (a row
// lookup without a rowid fn; see xsql_cached_row_lookup), with the reason in
// xsql_db_errmsg.
// ---------------------------------------------------------------------------

XSQL_API int xsql_cached_table_register(struct xsql_db *db, xsql_cached_table *c);
XSQL_API int xsql_cached_table_register_as(struct xsql_db *db,
                                           xsql_cached_table *c,
                                           const char *table_name);
XSQL_API int xsql_cached_table_register_module(struct xsql_db *db,
                                               xsql_cached_table *c);

// ---------------------------------------------------------------------------
// Invalidation
//
// A query-scoped table keeps its rows for the life of one cursor (a JOIN's
// inner loop re-filters the same cursor per outer row), and a shared-cache
// table keeps them across statements; both rebuild only after a write THROUGH
// the table. Code that changes the source behind the rows by other means (an
// engine rename, a re-analysis, a type redefinition) must say so. Each call
// below does exactly what such a write does: a scan already in flight keeps
// its rows as a snapshot, and every later probe or statement rebuilds from the
// live source.
// ---------------------------------------------------------------------------

// Invalidate one definition, wherever it is registered.
XSQL_API void xsql_cached_invalidate(xsql_cached_table *c);

// Invalidate every cached table registered on `db` (including the built-in
// runtime_settings / sql_capabilities tables). For a caller that mutates the
// engine outside SQL; a no-op on NULL or a closed db.
XSQL_API void xsql_db_invalidate_cached_tables(struct xsql_db *db);

// The same, addressed by the raw connection (see xsql_db_handle). A no-op for
// a connection not opened through xsql_open.
XSQL_API void xsql_invalidate_cached_tables(struct sqlite3 *db);

// The same, from inside a scalar function: invalidates every cached table on
// the connection `ctx` belongs to. Call it from a function that mutates the
// engine state the tables read; it holds no handle to those tables.
XSQL_API void xsql_context_invalidate_cached_tables(xsql_context *ctx);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_TABLE_CACHED_H
