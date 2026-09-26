// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// table_generator.h — the generator virtual table: a STREAMING source for
// expensive data where a full scan must be lazy so LIMIT stops work early (the
// opposite of the cached table's materialize-once model). A caller-supplied
// generator is pulled one row at a time; nothing is buffered. Adds hidden
// columns, parametric and multi-operator constraint pushdown, ORDER BY sort
// elision (order_by_consumed), and a full-scan refusal (full_scan_error) for
// tables that require an input constraint. Rows are opaque: getters take
// `const void *row`, setters `void *row`; indirect data (strings, blobs) is
// consumer-owned (stable/arena). The definition is CALLER-OWNED and must outlive
// every db it is registered into.

#ifndef XSQLC_TABLE_GENERATOR_H
#define XSQLC_TABLE_GENERATOR_H

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

// Opaque generator-table definition (built, then registered; must outlive dbs).
typedef struct xsql_generator_table xsql_generator_table;

// ---------------------------------------------------------------------------
// The generator object — a caller-implemented pull iterator over opaque rows.
// A factory fills one of these; the framework primes it with a single next()
// call, then drives next/current/rowid, and finally destroy() (if set). LIMIT
// stops early because the framework simply stops pulling — nothing materializes.
// ---------------------------------------------------------------------------
typedef struct xsql_generator {
    // Advance to the next row; return true if a row is now current, false when
    // exhausted. Must be usable as the priming call before the first access.
    bool (*next)(void *self);
    // The current row (row_size bytes), valid only after next() returned true.
    const void *(*current)(void *self);
    // The current row's rowid.
    int64_t (*rowid)(void *self);
    // Release `self` (may be NULL if there is nothing to free).
    void (*destroy)(void *self);
    // Opaque generator state, passed to every callback above.
    void *self;
} xsql_generator;

// Constraint operator matched by a constraint-filter spec.
typedef enum xsql_constraint_op {
    XSQL_CONSTRAINT_EQ   = 0,
    XSQL_CONSTRAINT_GT   = 1,
    XSQL_CONSTRAINT_LE   = 2,
    XSQL_CONSTRAINT_LT   = 3,
    XSQL_CONSTRAINT_GE   = 4,
    XSQL_CONSTRAINT_LIKE = 5
} xsql_constraint_op;

// A required/optional constraint request (a plain POD you fill with the helpers
// below and hand to xsql_generator_constraint_filter). The strings are borrowed
// only for the duration of that call.
typedef struct xsql_constraint_req {
    const char        *column;         // constrained column name
    xsql_constraint_op op;             // operator to match
    bool               required;       // fail the query if absent (see below)
    const char        *missing_error;  // error text when a required spec is absent
} xsql_constraint_req;

// One pushed-down constraint value handed to a constraint-filter factory. The
// factory sees which column and operator each value belongs to (needed to build
// range predicates). `value` is borrowed for the duration of the factory call.
typedef struct xsql_constraint_arg {
    int                column_index;
    xsql_constraint_op op;
    xsql_value        *value;
} xsql_constraint_arg;

// ---------------------------------------------------------------------------
// Callback signatures (each takes the per-registration `user` pointer last).
// ---------------------------------------------------------------------------

// Typed read getters over a generated record.
typedef int64_t (*xsql_grow_i64_fn)(const void *row, void *user);
typedef int (*xsql_grow_int_fn)(const void *row, void *user);
typedef double (*xsql_grow_double_fn)(const void *row, void *user);
// Raw ctx-emitting getter (text/blob/nullable — call xsql_result_text/blob/null).
typedef void (*xsql_grow_emit_fn)(xsql_context *ctx, const void *row, void *user);

// Typed write setters (return true, or false after xsql_vtab_set_error()).
typedef bool (*xsql_grow_set_i64_fn)(void *row, int64_t val, void *user);
typedef bool (*xsql_grow_set_int_fn)(void *row, int val, void *user);
typedef bool (*xsql_grow_set_text_fn)(void *row, const char *val, void *user);
// Value-arg setter (receives the raw value — detect SQL NULL, blobs, doubles).
typedef bool (*xsql_grow_set_value_fn)(void *row, xsql_value *val, void *user);

// Modification hooks: `op` is a short verb phrase (e.g. "UPDATE t").
typedef void (*xsql_generator_modify_fn)(const char *op, void *user);
// Write handlers over a reconstructed record.
typedef bool (*xsql_generator_delete_fn)(void *row, void *user);
typedef bool (*xsql_generator_insert_fn)(int argc, xsql_value **argv, void *user);
// Resolve `row` (row_size bytes) from a rowid; return true if found. Required for
// UPDATE/DELETE (the generator reconstructs the old row by rowid alone).
typedef bool (*xsql_generator_row_lookup_fn)(void *row, int64_t rowid, void *user);

// Full-scan factory: fill `*out` with a generator over every row; return false to
// mean an empty result.
typedef bool (*xsql_generator_make_fn)(xsql_generator *out, void *user);
// Projection-aware full-scan factory: receives SQLite's colUsed bitmask. Bits
// 0..62 identify their matching columns; bit 63 means column 63 or any higher
// column is used (and can also represent unknown usage). This lets a factory
// skip materializing expensive unused columns. Used for the full-scan plan when
// set; otherwise the plain make fn is used.
typedef bool (*xsql_generator_make_projection_fn)(uint64_t col_used,
                                                  xsql_generator *out, void *user);
// Parametric factory: the pushed-down equality values arrive in `argv` (one per
// configured column, in registration order).
typedef bool (*xsql_generator_parametric_fn)(int argc, xsql_value **argv,
                                             xsql_generator *out, void *user);
// Projection-aware variants of the two EQ-driven factories. They receive the
// same colUsed bitmask as xsql_generator_make_projection_fn, so a filtered query
// that does not select an expensive or non-portable column can be routed to a
// cheaper producer than one that does — the pushdown and the projection lever
// compose instead of being mutually exclusive.
typedef bool (*xsql_filter_projection_factory_fn)(xsql_value *arg,
                                                  uint64_t col_used,
                                                  xsql_row_iter *out, void *user);
typedef bool (*xsql_generator_parametric_projection_fn)(int argc, xsql_value **argv,
                                                        uint64_t col_used,
                                                        xsql_generator *out,
                                                        void *user);
// Constraint factory: each matched constraint arrives with its column + operator.
typedef bool (*xsql_generator_constraint_fn)(int argc,
                                             const xsql_constraint_arg *args,
                                             xsql_generator *out, void *user);

// ---------------------------------------------------------------------------
// Lifecycle + table configuration
// ---------------------------------------------------------------------------

// Create a generator table whose records are `row_size` bytes each.
XSQL_API xsql_generator_table *xsql_generator_table_new(const char *name,
                                                        size_t row_size);
XSQL_API void xsql_generator_table_free(xsql_generator_table *g);

// The full-scan generator factory (required — a table with none never yields).
XSQL_API void xsql_generator_table_generator(xsql_generator_table *g,
                                             xsql_generator_make_fn fn,
                                             void *user);
// Projection-aware full-scan factory (see xsql_generator_make_projection_fn): skips
// expensive unused columns via colUsed. Additive — overrides the plain generator for
// the full-scan plan when set; leave unset for the plain behavior.
XSQL_API void xsql_generator_table_projection_generator(
    xsql_generator_table *g, xsql_generator_make_projection_fn fn, void *user);
XSQL_API void xsql_generator_estimate_rows(xsql_generator_table *g,
                                           xsql_estimate_rows_fn fn, void *user);
// Optional exact count used only for a bare COUNT(*) plan. It is never called
// during ordinary scans or planning, and avoids constructing a generator.
XSQL_API void xsql_generator_count(xsql_generator_table *g,
                                   xsql_row_count_fn fn, void *user);
// Refuse an unconstrained full scan with this message (a required-input table).
XSQL_API void xsql_generator_full_scan_error(xsql_generator_table *g,
                                             const char *message);
XSQL_API void xsql_generator_on_modify(xsql_generator_table *g,
                                       xsql_generator_modify_fn fn, void *user);
XSQL_API void xsql_generator_after_modify(xsql_generator_table *g,
                                          xsql_generator_modify_fn fn,
                                          void *user);
// Install the full transaction lifecycle. The hooks are copied; `user` is
// borrowed and must outlive every database using this definition. Passing NULL
// clears the lifecycle. Returns XSQL_OK or XSQL_MISUSE.
XSQL_API int xsql_generator_transaction_hooks(
    xsql_generator_table *g, const xsql_transaction_hooks *hooks, void *user);

// ---------------------------------------------------------------------------
// Columns (declared in order). Each returns XSQL_OK / XSQL_NO_MEMORY.
// ---------------------------------------------------------------------------

// Raw ctx-emitting read-only column of an explicit type.
XSQL_API int xsql_generator_col(xsql_generator_table *g, const char *name,
                                xsql_column_type type, xsql_grow_emit_fn get,
                                void *user);
XSQL_API int xsql_generator_col_int(xsql_generator_table *g, const char *name,
                                    xsql_grow_int_fn get, void *user);
XSQL_API int xsql_generator_col_i64(xsql_generator_table *g, const char *name,
                                    xsql_grow_i64_fn get, void *user);
XSQL_API int xsql_generator_col_double(xsql_generator_table *g, const char *name,
                                       xsql_grow_double_fn get, void *user);
XSQL_API int xsql_generator_col_text(xsql_generator_table *g, const char *name,
                                     xsql_grow_emit_fn get, void *user);
XSQL_API int xsql_generator_col_blob(xsql_generator_table *g, const char *name,
                                     xsql_grow_emit_fn get, void *user);

// Generic writable column (explicit type; value-arg setter).
XSQL_API int xsql_generator_col_rw(xsql_generator_table *g, const char *name,
                                   xsql_column_type type, xsql_grow_emit_fn get,
                                   xsql_grow_set_value_fn set, void *user);
XSQL_API int xsql_generator_col_int_rw(xsql_generator_table *g, const char *name,
                                       xsql_grow_int_fn get,
                                       xsql_grow_set_int_fn set, void *user);
XSQL_API int xsql_generator_col_i64_rw(xsql_generator_table *g, const char *name,
                                       xsql_grow_i64_fn get,
                                       xsql_grow_set_i64_fn set, void *user);
XSQL_API int xsql_generator_col_text_rw(xsql_generator_table *g, const char *name,
                                        xsql_grow_emit_fn get,
                                        xsql_grow_set_text_fn set, void *user);
// Nullable text whose setter receives the raw value (to detect NULL).
XSQL_API int xsql_generator_col_text_nullable_rw(xsql_generator_table *g,
                                                 const char *name,
                                                 xsql_grow_emit_fn get,
                                                 xsql_grow_set_value_fn set,
                                                 void *user);
// Double / blob writable columns (setter receives the raw value).
XSQL_API int xsql_generator_col_double_rw(xsql_generator_table *g,
                                          const char *name,
                                          xsql_grow_double_fn get,
                                          xsql_grow_set_value_fn set, void *user);
XSQL_API int xsql_generator_col_blob_rw(xsql_generator_table *g, const char *name,
                                        xsql_grow_emit_fn get,
                                        xsql_grow_set_value_fn set, void *user);
// Value-arg writable overloads (setter receives the raw value).
XSQL_API int xsql_generator_col_int_rw_value(xsql_generator_table *g,
                                             const char *name,
                                             xsql_grow_int_fn get,
                                             xsql_grow_set_value_fn set,
                                             void *user);
XSQL_API int xsql_generator_col_i64_rw_value(xsql_generator_table *g,
                                             const char *name,
                                             xsql_grow_i64_fn get,
                                             xsql_grow_set_value_fn set,
                                             void *user);
XSQL_API int xsql_generator_col_text_rw_value(xsql_generator_table *g,
                                              const char *name,
                                              xsql_grow_emit_fn get,
                                              xsql_grow_set_value_fn set,
                                              void *user);

// Hidden columns (present in the schema with HIDDEN; read as NULL; used only as
// pushed-down inputs to parametric / constraint filters).
XSQL_API int xsql_generator_hidden_col_i64(xsql_generator_table *g,
                                           const char *name);
XSQL_API int xsql_generator_hidden_col_int(xsql_generator_table *g,
                                           const char *name);
XSQL_API int xsql_generator_hidden_col_text(xsql_generator_table *g,
                                            const char *name);

// ---------------------------------------------------------------------------
// Pushdown, ordering, reconstruction, write capability
// ---------------------------------------------------------------------------

// Single-column equality pushdown (factory reads `arg` as int / text). Bypasses
// the generator entirely via an xsql_row_iter.
XSQL_API int xsql_generator_filter_eq(xsql_generator_table *g, const char *column,
                                      double cost, double est_rows,
                                      xsql_filter_factory_fn factory, void *user);
XSQL_API int xsql_generator_filter_eq_text(xsql_generator_table *g,
                                           const char *column, double cost,
                                           double est_rows,
                                           xsql_filter_factory_fn factory,
                                           void *user);

// Projection-aware single-column equality pushdown: exactly filter_eq(), except
// the factory also receives colUsed. Strictly additive — a table that never
// registers one of these plans and behaves byte-for-byte as before.
XSQL_API int xsql_generator_filter_eq_projection(
    xsql_generator_table *g, const char *column, double cost, double est_rows,
    xsql_filter_projection_factory_fn factory, void *user);

// Parametric filter: fires only when EVERY listed column has a usable equality
// constraint; the values reach the factory in `columns` order.
XSQL_API int xsql_generator_parametric_filter(xsql_generator_table *g,
                                              const char *const *columns,
                                              size_t ncolumns, double cost,
                                              double est_rows,
                                              xsql_generator_parametric_fn factory,
                                              void *user);

// Projection-aware parametric filter (see filter_eq_projection for the
// rationale). Also strictly additive.
XSQL_API int xsql_generator_parametric_filter_projection(
    xsql_generator_table *g, const char *const *columns, size_t ncolumns,
    double cost, double est_rows,
    xsql_generator_parametric_projection_fn factory, void *user);

// General multi-operator constraint filter. A required spec that is absent fails
// the query with its missing_error (or, when that is empty, falls through to a
// full scan). Build `reqs` with the helpers below.
XSQL_API int xsql_generator_constraint_filter(xsql_generator_table *g,
                                              const xsql_constraint_req *reqs,
                                              size_t nreqs, double cost,
                                              double est_rows,
                                              xsql_generator_constraint_fn factory,
                                              void *user);

// Constraint-request helpers (fill an xsql_constraint_req by value).
XSQL_API xsql_constraint_req xsql_required_eq(const char *column,
                                              const char *missing_error);
XSQL_API xsql_constraint_req xsql_required_like(const char *column,
                                                const char *missing_error);
XSQL_API xsql_constraint_req xsql_optional_eq(const char *column);
XSQL_API xsql_constraint_req xsql_optional_like(const char *column);
XSQL_API xsql_constraint_req xsql_optional_gt(const char *column);
XSQL_API xsql_constraint_req xsql_optional_ge(const char *column);
XSQL_API xsql_constraint_req xsql_optional_lt(const char *column);
XSQL_API xsql_constraint_req xsql_optional_le(const char *column);

// Tag the most-recently-added constraint filter as emitting rows already sorted
// by `column` (ascending unless `desc`), so SQLite can elide the ORDER BY.
XSQL_API void xsql_generator_order_by_consumed(xsql_generator_table *g,
                                               const char *column, bool desc);

XSQL_API void xsql_generator_row_lookup(xsql_generator_table *g,
                                        xsql_generator_row_lookup_fn fn,
                                        void *user);
XSQL_API void xsql_generator_deletable(xsql_generator_table *g,
                                       xsql_generator_delete_fn fn, void *user);
XSQL_API void xsql_generator_insertable(xsql_generator_table *g,
                                        xsql_generator_insert_fn fn, void *user);

// ---------------------------------------------------------------------------
// Registration (the def is not copied; it must outlive `db`).
// ---------------------------------------------------------------------------

XSQL_API int xsql_generator_table_register(struct xsql_db *db,
                                           xsql_generator_table *g);
XSQL_API int xsql_generator_table_register_as(struct xsql_db *db,
                                              xsql_generator_table *g,
                                              const char *table_name);
XSQL_API int xsql_generator_table_register_module(struct xsql_db *db,
                                                  xsql_generator_table *g);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_TABLE_GENERATOR_H
