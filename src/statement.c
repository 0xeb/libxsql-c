// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// statement.c — prepared-statement wrapper: bind, step, and typed column reads.

#include <libxsqlc/statement.h>
#include <libxsqlc/status.h>

#include "internal/internal.h"

#include <sqlite3.h>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

struct xsql_stmt *xsqlc_stmt_wrap(struct sqlite3_stmt *stmt,
                                  struct sqlite3 *db) {
    struct xsql_stmt *s = (struct xsql_stmt *)xsqlc_malloc(sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->stmt = stmt;
    s->db = db;
    s->last_error = NULL;
    return s;
}

void xsql_stmt_finalize(xsql_stmt *s) {
    if (!s) {
        return;
    }
    if (s->stmt) {
        sqlite3_finalize(s->stmt);
        s->stmt = NULL;
    }
    xsqlc_free(s->last_error);
    xsqlc_free(s);
}

// ---------------------------------------------------------------------------
// Validity / error text
// ---------------------------------------------------------------------------

bool xsql_stmt_valid(const xsql_stmt *s) {
    return s != NULL && s->stmt != NULL;
}

const char *xsql_stmt_errmsg(const xsql_stmt *s) {
    if (!s || !s->last_error) {
        return "";
    }
    return s->last_error;
}

bool xsql_stmt_is_readonly(const xsql_stmt *s) {
    return xsql_stmt_valid(s) && sqlite3_stmt_readonly(s->stmt) != 0;
}

// Record (or clear) the last error from the owning connection after an op.
static int stmt_finish(xsql_stmt *s, int rc) {
    if (rc == SQLITE_OK) {
        xsqlc_str_set(&s->last_error, NULL);
    } else {
        xsqlc_str_set(&s->last_error, s->db ? sqlite3_errmsg(s->db) : "error");
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Reset / bind
// ---------------------------------------------------------------------------

int xsql_stmt_reset(xsql_stmt *s) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    return stmt_finish(s, sqlite3_reset(s->stmt));
}

int xsql_stmt_clear_bindings(xsql_stmt *s) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    return stmt_finish(s, sqlite3_clear_bindings(s->stmt));
}

int xsql_stmt_bind_null(xsql_stmt *s, int index) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    return stmt_finish(s, sqlite3_bind_null(s->stmt, index));
}

int xsql_stmt_bind_int(xsql_stmt *s, int index, int value) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    return stmt_finish(s, sqlite3_bind_int(s->stmt, index, value));
}

int xsql_stmt_bind_i64(xsql_stmt *s, int index, int64_t value) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    return stmt_finish(s, sqlite3_bind_int64(s->stmt, index, value));
}

int xsql_stmt_bind_double(xsql_stmt *s, int index, double value) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    return stmt_finish(s, sqlite3_bind_double(s->stmt, index, value));
}

int xsql_stmt_bind_text(xsql_stmt *s, int index, const char *value) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    return stmt_finish(s, sqlite3_bind_text(s->stmt, index, value, -1,
                                            SQLITE_TRANSIENT));
}

int xsql_stmt_bind_text_len(xsql_stmt *s, int index, const char *value,
                            size_t len) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    // By length: an embedded NUL is kept. A NULL pointer with len 0 is "".
    return stmt_finish(s, sqlite3_bind_text64(s->stmt, index, value ? value : "",
                                              (sqlite3_uint64)len,
                                              SQLITE_TRANSIENT, SQLITE_UTF8));
}

int xsql_stmt_bind_blob(xsql_stmt *s, int index, const void *data,
                        size_t size) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_MISUSE;
    }
    // An empty blob is still a blob: sqlite3_bind_blob with a NULL pointer
    // would bind SQL NULL instead. A large one binds whole (blob64), never
    // silently truncated.
    if (size == 0) {
        return stmt_finish(s, sqlite3_bind_zeroblob(s->stmt, index, 0));
    }
    return stmt_finish(s, sqlite3_bind_blob64(s->stmt, index, data,
                                              (sqlite3_uint64)size,
                                              SQLITE_TRANSIENT));
}

int xsql_stmt_parameter_count(const xsql_stmt *s) {
    return xsql_stmt_valid(s) ? sqlite3_bind_parameter_count(s->stmt) : 0;
}

// ---------------------------------------------------------------------------
// Step
// ---------------------------------------------------------------------------

int xsql_stmt_step(xsql_stmt *s) {
    if (!xsql_stmt_valid(s)) {
        return XSQL_ERROR;
    }

    int rc = sqlite3_step(s->stmt);
    if (rc == SQLITE_ROW) {
        xsqlc_str_set(&s->last_error, NULL);
        return XSQL_ROW;
    }
    if (rc == SQLITE_DONE) {
        xsqlc_str_set(&s->last_error, NULL);
        return XSQL_DONE;
    }
    xsqlc_str_set(&s->last_error, s->db ? sqlite3_errmsg(s->db) : "error");
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        return XSQL_BUSY;
    }
    return XSQL_ERROR;
}

// ---------------------------------------------------------------------------
// Column metadata / typed reads (0-based)
// ---------------------------------------------------------------------------

int xsql_stmt_column_count(const xsql_stmt *s) {
    return xsql_stmt_valid(s) ? sqlite3_column_count(s->stmt) : 0;
}

const char *xsql_stmt_column_name(const xsql_stmt *s, int col) {
    if (!xsql_stmt_valid(s)) {
        return "";
    }
    const char *name = sqlite3_column_name(s->stmt, col);
    return name ? name : "";
}

int xsql_stmt_column_type(const xsql_stmt *s, int col) {
    return xsql_stmt_valid(s) ? sqlite3_column_type(s->stmt, col) : SQLITE_NULL;
}

bool xsql_stmt_column_is_null(const xsql_stmt *s, int col) {
    return !xsql_stmt_valid(s) ||
           sqlite3_column_type(s->stmt, col) == SQLITE_NULL;
}

int xsql_stmt_col_int(const xsql_stmt *s, int col) {
    return xsql_stmt_valid(s) ? sqlite3_column_int(s->stmt, col) : 0;
}

int64_t xsql_stmt_col_i64(const xsql_stmt *s, int col) {
    return xsql_stmt_valid(s) ? sqlite3_column_int64(s->stmt, col) : 0;
}

double xsql_stmt_col_double(const xsql_stmt *s, int col) {
    return xsql_stmt_valid(s) ? sqlite3_column_double(s->stmt, col) : 0.0;
}

const char *xsql_stmt_col_text(const xsql_stmt *s, int col) {
    if (!xsql_stmt_valid(s)) {
        return NULL;
    }
    return (const char *)sqlite3_column_text(s->stmt, col);
}

const void *xsql_stmt_col_blob(const xsql_stmt *s, int col) {
    if (!xsql_stmt_valid(s)) {
        return NULL;
    }
    return sqlite3_column_blob(s->stmt, col);
}

int xsql_stmt_col_bytes(const xsql_stmt *s, int col) {
    return xsql_stmt_valid(s) ? sqlite3_column_bytes(s->stmt, col) : 0;
}

// ---------------------------------------------------------------------------
// Escape hatch
// ---------------------------------------------------------------------------

struct sqlite3_stmt *xsql_stmt_handle(xsql_stmt *s) {
    return s ? s->stmt : NULL;
}
