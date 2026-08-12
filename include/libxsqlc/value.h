// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// value.h — SQL function argument values, the result/error sink, and the
// per-aggregation state slot.

#ifndef XSQLC_VALUE_H
#define XSQLC_VALUE_H

#include <libxsqlc/common.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles. All three are borrowed views the library never owns: they
// alias the underlying SQLite objects and are valid only for the duration of
// the callback (or, for a query-row, the current row) that hands them out.
typedef struct xsql_value xsql_value;          // one function argument
typedef struct xsql_context xsql_context;      // scalar/aggregate result sink
typedef struct xsql_query_row xsql_query_row;  // one row inside query_each

// Copy semantics for text/blob results. XSQL_STATIC promises the pointer
// outlives the call (no copy); XSQL_TRANSIENT tells the library to take its own
// copy before returning. These map to SQLITE_STATIC / SQLITE_TRANSIENT.
typedef enum xsql_text_copy {
    XSQL_STATIC    = 0,
    XSQL_TRANSIENT = 1
} xsql_text_copy;

// ---------------------------------------------------------------------------
// Reading a function argument (xsql_value)
// ---------------------------------------------------------------------------

// Typed reads. sqlite applies its usual coercions.
XSQL_API int64_t xsql_value_i64(xsql_value *v);
XSQL_API int     xsql_value_int(xsql_value *v);
XSQL_API double  xsql_value_double(xsql_value *v);

// Text as a borrowed, NUL-terminated C string (valid until the value changes or
// the callback returns). Read as a C string it stops at the first embedded NUL;
// use xsql_value_bytes() for the true byte length (which counts bytes past a
// NUL). Returns NULL for a SQL NULL.
XSQL_API const char *xsql_value_text(xsql_value *v);

// Blob bytes (borrowed). Returns NULL for a SQL NULL or a zero-length blob;
// pair with xsql_value_bytes() for the length.
XSQL_API const void *xsql_value_blob(xsql_value *v);

// Full byte length of the text/blob value — never truncated at an embedded NUL.
XSQL_API int xsql_value_bytes(xsql_value *v);

// Raw SQLite datatype code (SQLITE_INTEGER/FLOAT/TEXT/BLOB/NULL == 1/2/3/4/5).
XSQL_API int xsql_value_type(xsql_value *v);

// Convenience predicates.
XSQL_API bool xsql_value_is_null(xsql_value *v);
XSQL_API bool xsql_value_is_nochange(xsql_value *v);

// ---------------------------------------------------------------------------
// Setting a scalar/aggregate result (xsql_context)
// ---------------------------------------------------------------------------

XSQL_API void xsql_result_int(xsql_context *ctx, int val);
XSQL_API void xsql_result_i64(xsql_context *ctx, int64_t val);
XSQL_API void xsql_result_double(xsql_context *ctx, double val);

// Text result. len < 0 means "read up to the first NUL" (an embedded NUL
// truncates the value rather than being an error).
// A non-negative len stores exactly that many bytes. `copy` selects
// static-vs-transient ownership of `s`.
XSQL_API void xsql_result_text(xsql_context *ctx, const char *s, int len,
                               xsql_text_copy copy);

// Shorthand for a static, NUL-terminated string result (no copy).
XSQL_API void xsql_result_text_static(xsql_context *ctx, const char *s);

// Blob result. The library always copies the bytes (transient).
XSQL_API void xsql_result_blob(xsql_context *ctx, const void *data, size_t len);

XSQL_API void xsql_result_null(xsql_context *ctx);
XSQL_API void xsql_result_error(xsql_context *ctx, const char *msg);

// ---------------------------------------------------------------------------
// Aggregate state and context helpers
// ---------------------------------------------------------------------------

// One pointer-sized slot owned by SQLite, zero-initialized on first access and
// stable for the duration of the aggregation. Store a heap pointer to your
// state here (allocate on the first step, free in the final callback). Returns
// NULL only under memory pressure.
XSQL_API void **xsql_aggregate_state(xsql_context *ctx);

// Last error text on the database this context belongs to (borrowed).
XSQL_API const char *xsql_context_db_errmsg(xsql_context *ctx);

// Row callback for xsql_context_query_each: return 0 to keep iterating, nonzero
// to stop early (query_each then returns XSQL_ABORT).
typedef int (*xsql_query_row_cb)(const xsql_query_row *row, void *user);

// Run a query on the same database the context belongs to, invoking `row_cb`
// once per result row. Returns XSQL_OK when all rows were consumed, XSQL_ABORT
// if the callback asked to stop, or an error status if preparation/stepping
// failed.
XSQL_API int xsql_context_query_each(xsql_context *ctx, const char *sql,
                                     xsql_query_row_cb row_cb, void *user);

// Per-column reads inside a query_each row callback (0-based columns). Text is
// borrowed and valid only for the current row.
XSQL_API const char *xsql_query_row_text(const xsql_query_row *row, int col);
XSQL_API int         xsql_query_row_int(const xsql_query_row *row, int col);
XSQL_API int64_t     xsql_query_row_i64(const xsql_query_row *row, int col);
XSQL_API double      xsql_query_row_double(const xsql_query_row *row, int col);
XSQL_API bool        xsql_query_row_is_null(const xsql_query_row *row, int col);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_VALUE_H
