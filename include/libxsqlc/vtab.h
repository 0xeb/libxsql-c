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

// ---------------------------------------------------------------------------
// Multi-column / multi-operator constraints (constraint filters)
// ---------------------------------------------------------------------------

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
// below and hand to a constraint-filter registrar: xsql_generator_constraint_
// filter[_projection], xsql_cached_constraint_filter, xsql_cached_constraint_
// cache_filter). The strings are borrowed only for the duration of that call.
typedef struct xsql_constraint_req {
    const char        *column;         // constrained column name
    xsql_constraint_op op;             // operator to match
    bool               required;       // the filter matches only when present
    // Generator tables only: the query fails with this text when a required
    // spec is absent (cached tables fall back to another plan instead).
    const char        *missing_error;
} xsql_constraint_req;

// One pushed-down constraint value handed to a constraint-filter factory. The
// factory sees which column and operator each value belongs to (needed to build
// range predicates). `value` is borrowed for the duration of the factory call.
typedef struct xsql_constraint_arg {
    int                column_index;
    xsql_constraint_op op;
    xsql_value        *value;
} xsql_constraint_arg;

// Constraint-request helpers (fill an xsql_constraint_req by value).
XSQL_API xsql_constraint_req xsql_required_eq(const char *column,
                                              const char *missing_error);
XSQL_API xsql_constraint_req xsql_required_gt(const char *column,
                                              const char *missing_error);
XSQL_API xsql_constraint_req xsql_required_le(const char *column,
                                              const char *missing_error);
XSQL_API xsql_constraint_req xsql_required_like(const char *column,
                                                const char *missing_error);
XSQL_API xsql_constraint_req xsql_optional_eq(const char *column);
XSQL_API xsql_constraint_req xsql_optional_like(const char *column);
XSQL_API xsql_constraint_req xsql_optional_gt(const char *column);
XSQL_API xsql_constraint_req xsql_optional_ge(const char *column);
XSQL_API xsql_constraint_req xsql_optional_lt(const char *column);
XSQL_API xsql_constraint_req xsql_optional_le(const char *column);

// ---------------------------------------------------------------------------
// Constraint values in factories
// ---------------------------------------------------------------------------
//
// A constraint-filter / parametric-filter factory (and a raw filter factory)
// receives each constraint value exactly as SQLite handed it to xFilter: no
// column affinity, so `k = '15'` is text, `k = 10.5` a real and `k = NULL`
// NULL. Those plans are omit=1 -- SQLite does not re-check the rows -- so the
// factory must answer what SQLite would answer for a native column. Reading
// the value with xsql_value_i64() does not: 10.5 truncates to 10, and 'a' and
// NULL become 0. These helpers are the comparison the built-in filter_eq /
// index plans use; call them instead of xsql_value_i64() / xsql_value_text()
// on a constraint value.
//
// An exact integer key (an address, an id) in an EQ constraint:
//
//     int64_t ea;
//     if (!xsql_exact_int64(arg->value, &ea)) return empty(out);  // no rows
//
// A key range from GT/GE/LT/LE (and EQ) constraints on one INTEGER column:
//
//     xsql_int64_key_range range;
//     xsql_int64_key_range_init(&range);
//     for (int i = 0; i < argc; ++i)
//         xsql_int64_key_range_constrain(&range, args[i].op, args[i].value);
//     if (xsql_int64_key_range_empty(&range)) return empty(out);
//     scan(range.lower, range.upper);  // both inclusive
//
// A parameter that deliberately accepts another spelling (a symbol name or
// '0x401000' text for an address, say) is a product surface, not a key
// comparison: parse that spelling on purpose, and use these for the rest.

// Write the int64 that equals `value` under SQLite's comparison with an
// INTEGER column to *out and return true, or return false when no integer
// does (the factory then yields no rows). An integer is itself; a real only
// when integral and in int64 range (10.0 -> 10, 10.5 -> none, exact past
// 2^53); text only when SQLite's INTEGER affinity makes it such a number
// ('10', ' 10 ', '1e1' -> 10; '0x10' and 'a' -> none); NULL, NaN and blobs
// never. On allocation failure the vtab error is set as well.
XSQL_API bool xsql_exact_int64(xsql_value *value, int64_t *out);

// The text that equals `value` under SQLite's comparison with a TEXT column,
// or NULL when no text does (the factory then yields no rows). Text is
// itself; a number gets TEXT affinity, which is its rendering (10 -> "10",
// 10.5 -> "10.5"); NULL and blobs never equal text. Borrowed like
// xsql_value_text().
XSQL_API const char *xsql_exact_text(xsql_value *value);

// The inclusive window of int64 keys that satisfies every comparison applied
// to it, each compared the way SQLite compares an INTEGER column: GE 14.5
// keeps keys >= 15, LT 'a' keeps every key (text sorts after all numbers), a
// NULL operand or an EQ value no integer can equal empties the window. Read
// `lower` / `upper` (inclusive; INT64_MIN / INT64_MAX when unbounded) only
// when not empty; mutate it only through the functions below.
typedef struct xsql_int64_key_range {
    int64_t lower;
    int64_t upper;
    bool    empty;
} xsql_int64_key_range;

// Start with every int64.
XSQL_API void xsql_int64_key_range_init(xsql_int64_key_range *range);

// Narrow the window to keys satisfying `key <op> value`. Returns false once
// the window is empty. LIKE is not a key comparison: it empties the window and
// sets the vtab error, so a mis-routed constraint fails loudly. On allocation
// failure the vtab error is set and the window empties.
XSQL_API bool xsql_int64_key_range_constrain(xsql_int64_key_range *range,
                                             xsql_constraint_op op,
                                             xsql_value *value);

// The same for an integer the caller resolved itself (e.g. a symbol name
// parsed to an address).
XSQL_API bool xsql_int64_key_range_constrain_int64(xsql_int64_key_range *range,
                                                   xsql_constraint_op op,
                                                   int64_t value);

// True when no key satisfies the comparisons applied so far.
XSQL_API bool xsql_int64_key_range_empty(const xsql_int64_key_range *range);

// Whether `key` satisfies every comparison applied so far.
XSQL_API bool xsql_int64_key_range_contains(const xsql_int64_key_range *range,
                                            int64_t key);

// An inclusive span of unsigned 64-bit keys, lo <= hi. Produced by
// xsql_int64_key_range_unsigned_spans().
typedef struct xsql_u64_span {
    uint64_t lo;
    uint64_t hi;
} xsql_u64_span;

// The window as unsigned 64-bit spans, in the order SQL sorts the keys; writes
// them to out[0..] and returns how many (0, 1 or 2).
//
// For tables whose key is really an unsigned 64-bit value (an address) stored
// as a bit-cast int64. Keys >= 2^63 reach SQL as negative integers, so SQL
// order is the high half first (most negative first), then 0, then the low
// half, while the engine walks unsigned. A walker that claims ORDER BY key, or
// omits the constraint, must visit the spans in this order:
//
//   empty window     -> no spans
//   lo, hi >= 0      -> [lo, hi]
//   lo, hi < 0       -> [u(lo), u(hi)]
//   lo < 0 <= hi     -> [u(lo), UINT64_MAX], then [0, hi]
//
// where u() is the bit-cast to uint64. A nonzero `descending` reverses the
// order of the spans; each span stays lo <= hi, and the walker decides how to
// walk it (downward, for a descending walk). A span only says what SQL order
// requires: it does not clamp to anything the engine can address.
//
// Gotcha for anyone writing such queries: SQLite reads a decimal literal
// >= 2^63 (18446744073709551613) as a REAL, which no INTEGER key equals and
// which bounds the window past every key. Write a hex literal
// (0xFFFFFFFFFFFFFFFD, read as 64-bit two's complement) or the negative decimal
// (-3). And key >= 0xFFFFF80000000000 also includes every low-half key;
// key BETWEEN 0xFFFFF80000000000 AND -1 is the high half.
XSQL_API int xsql_int64_key_range_unsigned_spans(const xsql_int64_key_range *range,
                                                 int descending,
                                                 xsql_u64_span out[2]);

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
