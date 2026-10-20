// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// result.h — the materialized query envelope. A xsql_result is a whole result set
// gathered into memory, with column names, row-major cells, per-cell SQL-NULL
// flags, an error string, warnings, and timing/timeout metadata. Produced by
// xsql_db_query() / xsql_db_query_opts() (database.h) and released with
// xsql_result_free().

#ifndef XSQLC_RESULT_H
#define XSQLC_RESULT_H

#include <libxsqlc/common.h>
#include <libxsqlc/value.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle. Created by xsql_db_query()/xsql_db_query_opts(); released with
// xsql_result_free(). Every accessor below returns a borrowed view valid only
// while the result lives.
typedef struct xsql_result xsql_result;

// ---------------------------------------------------------------------------
// Columns
// ---------------------------------------------------------------------------

// Number of output columns.
XSQL_API size_t xsql_result_column_count(const xsql_result *r);

// Name of column `col` (borrowed; NUL-terminated). NULL if `col` is out of
// range.
XSQL_API const char *xsql_result_column_name(const xsql_result *r, size_t col);

// ---------------------------------------------------------------------------
// Rows and cells
// ---------------------------------------------------------------------------

// Number of result rows.
XSQL_API size_t xsql_result_row_count(const xsql_result *r);

// Cell text at (row, col): borrowed, NUL-terminated. A SQL NULL reads as ""
// (indistinguishable from a genuine empty string by value alone — use
// xsql_result_cell_is_null() to tell them apart). Numeric cells are coerced to
// text; a BLOB's raw bytes are returned.
// Read as a C string it stops at the first embedded NUL; use
// xsql_result_cell_bytes() for the true byte length. Returns NULL if (row, col)
// is out of range.
XSQL_API const char *xsql_result_cell(const xsql_result *r, size_t row,
                                      size_t col);

// True byte length of the cell at (row, col) — never truncated at an embedded
// NUL (BLOB/embedded-NUL safe; pairs with xsql_result_cell()). 0 for a SQL NULL
// or an out-of-range index.
XSQL_API int xsql_result_cell_bytes(const xsql_result *r, size_t row,
                                    size_t col);

// True if the cell at (row, col) was a SQL NULL. The only way to distinguish a
// SQL NULL from an empty-string value. False for an out-of-range index.
XSQL_API bool xsql_result_cell_is_null(const xsql_result *r, size_t row,
                                       size_t col);

// SQLite storage class of the cell at (row, col), read before the cell was
// converted to text: 1 INTEGER, 2 FLOAT, 3 TEXT, 4 BLOB, 5 NULL (the codes
// xsql_value_type() returns). Lets an embedder hand an integer back as an
// integer, not its decimal text. 0 for an out-of-range index.
XSQL_API int xsql_result_cell_type(const xsql_result *r, size_t row,
                                   size_t col);

// ---------------------------------------------------------------------------
// Status, warnings, timing
// ---------------------------------------------------------------------------

// True when the query succeeded (the error text is empty).
XSQL_API bool xsql_result_ok(const xsql_result *r);

// Error text (borrowed; never NULL — "" when ok). A failed query populates this
// rather than throwing/aborting. Named _errmsg (not _error) so it does not
// collide with the scalar-function error sink xsql_result_error() in value.h.
XSQL_API const char *xsql_result_errmsg(const xsql_result *r);

// Number of non-fatal warnings.
XSQL_API size_t xsql_result_warning_count(const xsql_result *r);

// Warning `i` (borrowed; NUL-terminated). NULL if `i` is out of range.
XSQL_API const char *xsql_result_warning(const xsql_result *r, size_t i);

// True if the query's deadline (xsql_query_opts::timeout_ms) tripped. A
// read-only result statement then keeps the rows gathered so far (see
// xsql_result_partial); a mutation (including DML with RETURNING) carries a
// timeout error and its provisional rows are discarded.
XSQL_API bool xsql_result_timed_out(const xsql_result *r);

// True if a timeout/cancellation/late read error kept the rows gathered so far.
// Interrupted mutations are never partial successes.
XSQL_API bool xsql_result_partial(const xsql_result *r);

// Monotonic elapsed time of the step loop in whole milliseconds.
XSQL_API int xsql_result_elapsed_ms(const xsql_result *r);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Release a result. Passing NULL is a no-op.
XSQL_API void xsql_result_free(xsql_result *r);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_RESULT_H
