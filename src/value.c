// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// value.c — function argument reads, the result/error sink, and the
// per-aggregation state slot. The opaque xsql_value / xsql_context /
// xsql_query_row handles alias sqlite3_value* / sqlite3_context* /
// sqlite3_stmt* directly (single-pointer views, no wrapper allocation), so the
// accessors are thin casts over the SQLite C API.

#include <libxsqlc/value.h>
#include <libxsqlc/status.h>

#include "internal/internal.h"

#include <sqlite3.h>

// ---------------------------------------------------------------------------
// Cast helpers (opaque handle <-> raw sqlite type)
// ---------------------------------------------------------------------------

static sqlite3_value *as_value(xsql_value *v) {
    return (sqlite3_value *)v;
}

static sqlite3_context *as_context(xsql_context *ctx) {
    return (sqlite3_context *)ctx;
}

static sqlite3_stmt *as_row_stmt(const xsql_query_row *row) {
    return (sqlite3_stmt *)row;
}

// ---------------------------------------------------------------------------
// Reading a function argument
// ---------------------------------------------------------------------------

int64_t xsql_value_i64(xsql_value *v) {
    return sqlite3_value_int64(as_value(v));
}

int xsql_value_int(xsql_value *v) {
    return sqlite3_value_int(as_value(v));
}

double xsql_value_double(xsql_value *v) {
    return sqlite3_value_double(as_value(v));
}

const char *xsql_value_text(xsql_value *v) {
    // Borrowed, NUL-terminated pointer. Read as a C string it stops at the
    // first embedded NUL; the true
    // byte length is xsql_value_bytes().
    return (const char *)sqlite3_value_text(as_value(v));
}

const void *xsql_value_blob(xsql_value *v) {
    return sqlite3_value_blob(as_value(v));
}

int xsql_value_bytes(xsql_value *v) {
    // Full byte length; never truncated at an embedded NUL.
    return sqlite3_value_bytes(as_value(v));
}

int xsql_value_type(xsql_value *v) {
    return sqlite3_value_type(as_value(v));
}

bool xsql_value_is_null(xsql_value *v) {
    return sqlite3_value_type(as_value(v)) == SQLITE_NULL;
}

bool xsql_value_is_nochange(xsql_value *v) {
    return sqlite3_value_nochange(as_value(v)) != 0;
}

// ---------------------------------------------------------------------------
// Setting a scalar/aggregate result
// ---------------------------------------------------------------------------

void xsql_result_int(xsql_context *ctx, int val) {
    sqlite3_result_int(as_context(ctx), val);
}

void xsql_result_i64(xsql_context *ctx, int64_t val) {
    sqlite3_result_int64(as_context(ctx), val);
}

void xsql_result_double(xsql_context *ctx, double val) {
    sqlite3_result_double(as_context(ctx), val);
}

void xsql_result_text(xsql_context *ctx, const char *s, int len,
                      xsql_text_copy copy) {
    // XSQL_TRANSIENT -> SQLITE_TRANSIENT (library copies), else SQLITE_STATIC.
    // A negative len means "read up to the first NUL": an embedded NUL
    // truncates the value rather than being an error.
    sqlite3_destructor_type dtor =
        (copy == XSQL_TRANSIENT) ? SQLITE_TRANSIENT : SQLITE_STATIC;
    sqlite3_result_text(as_context(ctx), s, len, dtor);
}

void xsql_result_text_static(xsql_context *ctx, const char *s) {
    sqlite3_result_text(as_context(ctx), s, -1, SQLITE_STATIC);
}

void xsql_result_blob(xsql_context *ctx, const void *data, size_t len) {
    int n = (len > (size_t)0x7fffffff) ? 0x7fffffff : (int)len;
    sqlite3_result_blob(as_context(ctx), data, n, SQLITE_TRANSIENT);
}

void xsql_result_null(xsql_context *ctx) {
    sqlite3_result_null(as_context(ctx));
}

void xsql_result_error(xsql_context *ctx, const char *msg) {
    sqlite3_result_error(as_context(ctx), msg ? msg : "", -1);
}

// ---------------------------------------------------------------------------
// Aggregate state and context helpers
// ---------------------------------------------------------------------------

void **xsql_aggregate_state(xsql_context *ctx) {
    // One pointer-sized slot owned by SQLite, zero-initialized on first access
    // and stable for the aggregation's duration.
    return (void **)sqlite3_aggregate_context(as_context(ctx), (int)sizeof(void *));
}

const char *xsql_context_db_errmsg(xsql_context *ctx) {
    sqlite3 *db = sqlite3_context_db_handle(as_context(ctx));
    return db ? sqlite3_errmsg(db) : "No database handle";
}

int xsql_context_query_each(xsql_context *ctx, const char *sql,
                            xsql_query_row_cb row_cb, void *user) {
    sqlite3 *db = sqlite3_context_db_handle(as_context(ctx));
    if (!db) {
        return XSQL_MISUSE;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        return rc;
    }

    int result = XSQL_OK;
    for (;;) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            if (row_cb && row_cb((const xsql_query_row *)stmt, user) != 0) {
                result = XSQL_ABORT;
                break;
            }
            continue;
        }
        if (rc == SQLITE_DONE) {
            result = XSQL_OK;
            break;
        }
        result = rc;  // an error code
        break;
    }

    sqlite3_finalize(stmt);
    return result;
}

const char *xsql_query_row_text(const xsql_query_row *row, int col) {
    return (const char *)sqlite3_column_text(as_row_stmt(row), col);
}

int xsql_query_row_int(const xsql_query_row *row, int col) {
    return sqlite3_column_int(as_row_stmt(row), col);
}

int64_t xsql_query_row_i64(const xsql_query_row *row, int col) {
    return sqlite3_column_int64(as_row_stmt(row), col);
}

double xsql_query_row_double(const xsql_query_row *row, int col) {
    return sqlite3_column_double(as_row_stmt(row), col);
}

bool xsql_query_row_is_null(const xsql_query_row *row, int col) {
    return sqlite3_column_type(as_row_stmt(row), col) == SQLITE_NULL;
}
