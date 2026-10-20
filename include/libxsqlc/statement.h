// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// statement.h — prepared statement: bind, step, and typed column reads.

#ifndef XSQLC_STATEMENT_H
#define XSQLC_STATEMENT_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Raw escape-hatch handle: forward-declared so this header never pulls in
// sqlite3.h. Consumers that need the underlying statement include sqlite3.h
// themselves.
struct sqlite3_stmt;

#ifdef __cplusplus
extern "C" {
#endif

// Opaque prepared statement. Created by xsql_db_prepare(); released with
// xsql_stmt_finalize().
typedef struct xsql_stmt xsql_stmt;

// Validity + last error (borrowed, valid until the next call on this stmt).
XSQL_API bool        xsql_stmt_valid(const xsql_stmt *s);
XSQL_API const char *xsql_stmt_errmsg(const xsql_stmt *s);

// True if the statement makes no direct changes to the database.
XSQL_API bool xsql_stmt_is_readonly(const xsql_stmt *s);

// Reset for re-execution / drop all bound parameter values. Return an
// xsql_status (XSQL_OK on success).
XSQL_API int xsql_stmt_reset(xsql_stmt *s);
XSQL_API int xsql_stmt_clear_bindings(xsql_stmt *s);

// Bind a value to a parameter. Parameter indices are 1-based (SQLite
// convention). Text/blob bytes are copied (transient). Return an xsql_status.
XSQL_API int xsql_stmt_bind_null(xsql_stmt *s, int index);
XSQL_API int xsql_stmt_bind_int(xsql_stmt *s, int index, int value);
XSQL_API int xsql_stmt_bind_i64(xsql_stmt *s, int index, int64_t value);
XSQL_API int xsql_stmt_bind_double(xsql_stmt *s, int index, double value);
// NUL-terminated text: it stops at the first NUL. Use xsql_stmt_bind_text_len
// for text that may carry an embedded NUL.
XSQL_API int xsql_stmt_bind_text(xsql_stmt *s, int index, const char *value);
// Exactly `len` bytes of UTF-8 text (an embedded NUL is kept).
XSQL_API int xsql_stmt_bind_text_len(xsql_stmt *s, int index, const char *value,
                                     size_t len);
// A zero-length blob is bound as a blob (not SQL NULL), even with data == NULL.
XSQL_API int xsql_stmt_bind_blob(xsql_stmt *s, int index, const void *data,
                                 size_t size);
// Number of placeholders in the statement (sqlite3_bind_parameter_count); 0
// for an invalid statement.
XSQL_API int xsql_stmt_parameter_count(const xsql_stmt *s);

// Advance the statement one step. Returns XSQL_ROW (a row is available),
// XSQL_DONE (finished), XSQL_BUSY (retryable lock), or XSQL_ERROR.
XSQL_API int xsql_stmt_step(xsql_stmt *s);

// Result-column metadata (0-based columns).
XSQL_API int         xsql_stmt_column_count(const xsql_stmt *s);
XSQL_API const char *xsql_stmt_column_name(const xsql_stmt *s, int col);
XSQL_API int         xsql_stmt_column_type(const xsql_stmt *s, int col);
XSQL_API bool        xsql_stmt_column_is_null(const xsql_stmt *s, int col);

// Typed column reads (0-based). Text/blob pointers are borrowed and valid only
// until the next step/reset/finalize. xsql_stmt_col_text returns a borrowed,
// NUL-terminated pointer; xsql_stmt_col_bytes gives the full byte length.
XSQL_API int         xsql_stmt_col_int(const xsql_stmt *s, int col);
XSQL_API int64_t     xsql_stmt_col_i64(const xsql_stmt *s, int col);
XSQL_API double      xsql_stmt_col_double(const xsql_stmt *s, int col);
XSQL_API const char *xsql_stmt_col_text(const xsql_stmt *s, int col);
XSQL_API const void *xsql_stmt_col_blob(const xsql_stmt *s, int col);
XSQL_API int         xsql_stmt_col_bytes(const xsql_stmt *s, int col);

// Finalize and free the statement. Passing NULL is a no-op.
XSQL_API void xsql_stmt_finalize(xsql_stmt *s);

// Raw escape hatch: the underlying sqlite3_stmt* (borrowed). NULL if invalid.
XSQL_API struct sqlite3_stmt *xsql_stmt_handle(xsql_stmt *s);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_STATEMENT_H
