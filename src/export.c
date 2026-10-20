// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// export.c — xsql_export_tables: dump a database (or a chosen set of tables) as a
// self-contained SQL script, including round-trip-safe REAL literals and the
// uppercase X'..' blob-hex form.
//
// The header counts the tables actually written and names, on a
// "-- Skipped (requires constraints): a, b" line, the tables an all-tables
// export skipped because they cannot be read without a WHERE; requested by
// name, such a table fails the export instead.

#include <libxsqlc/script.h>

#include <libxsqlc/database.h>
#include <libxsqlc/result.h>
#include <libxsqlc/statement.h>
#include <libxsqlc/status.h>

#include "internal/internal.h"
#include "internal/strbuf.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Append a SQL string literal: '...' with embedded ' doubled (over `len` bytes).
static void export_append_text_literal(xsqlc_strbuf *sb, const char *text,
                                       size_t len) {
    if (memchr(text, 0, len)) {
        static const char hex[] = "0123456789ABCDEF";
        xsqlc_sb_append(sb, "CAST(X'");
        for (size_t i = 0; i < len; ++i) {
            unsigned char c = (unsigned char)text[i];
            char pair[2] = {hex[c >> 4], hex[c & 15]};
            xsqlc_sb_append_len(sb, pair, 2);
        }
        xsqlc_sb_append(sb, "' AS TEXT)");
        return;
    }
    xsqlc_sb_putc(sb, '\'');
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '\'') {
            xsqlc_sb_append_len(sb, "''", 2);
        } else {
            xsqlc_sb_putc(sb, c);
        }
    }
    xsqlc_sb_putc(sb, '\'');
}

static void export_append_blob_hex(xsqlc_strbuf *sb, const unsigned char *data,
                                   size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    xsqlc_sb_append_len(sb, "X'", 2);
    for (size_t i = 0; i < len; ++i) {
        char pair[2] = {hex[data[i] >> 4], hex[data[i] & 0x0F]};
        xsqlc_sb_append_len(sb, pair, 2);
    }
    xsqlc_sb_putc(sb, '\'');
}

static bool export_append_real_literal(xsqlc_strbuf *sb, double value) {
    if (isinf(value)) {
        xsqlc_sb_append(sb, value < 0 ? "-9.0e999" : "9.0e999");
        return true;
    }

    /* SQLite formatting is independent of the process locale. */
    char literal[64];
    sqlite3_snprintf((int)sizeof(literal), literal, "%!.16e", value);
    const char *exponent = strchr(literal, 'e');
    const char *decimal = strchr(literal, '.');
    if (!exponent || !decimal) {
        return false;
    }
    xsqlc_sb_append_len(sb, literal, (size_t)(exponent - literal));
    for (size_t digits = (size_t)(exponent - decimal - 1); digits < 16; ++digits) {
        xsqlc_sb_putc(sb, '0');
    }
    xsqlc_sb_append(sb, exponent);
    return true;
}

// Collect user table names (ordered) into a fresh string array. Returns an
// xsql_status; on success *names/*count own an xsql_free_strings-able array.
static int export_collect_table_names(struct xsql_db *db, char ***names,
                                      size_t *count) {
    *names = NULL;
    *count = 0;
    xsql_result *res = NULL;
    int rc = xsql_db_query(
        db, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;",
        &res);
    if (!res) {
        return XSQL_NO_MEMORY;
    }
    if (!xsql_result_ok(res)) {
        rc = (rc == XSQL_OK) ? XSQL_ERROR : rc;
        xsql_result_free(res);
        return rc;
    }
    size_t nr = xsql_result_row_count(res);
    char **arr = (char **)xsqlc_malloc((nr ? nr : 1) * sizeof(*arr));
    if (!arr) {
        xsql_result_free(res);
        return XSQL_NO_MEMORY;
    }
    size_t n = 0;
    for (size_t i = 0; i < nr; ++i) {
        const char *name = xsql_result_cell(res, i, 0);
        arr[n] = xsqlc_strdup(name ? name : "");
        if (!arr[n]) {
            xsql_free_strings(arr, n);
            xsql_result_free(res);
            return XSQL_NO_MEMORY;
        }
        ++n;
    }
    xsql_result_free(res);
    *names = arr;
    *count = n;
    return XSQL_OK;
}

// SQLite's planner text when every plan a virtual table offered was refused
// (its xBestIndex returned SQLITE_CONSTRAINT for each one).
#define EXPORT_NO_QUERY_SOLUTION "no query solution"

// Outcome of one table, beyond its xsql_status.
typedef enum {
    EXPORT_TABLE_WRITTEN,
    EXPORT_TABLE_EMPTY,    // no columns: nothing written
    EXPORT_TABLE_REFUSED,  // needs constraints: nothing written; *reason set
} export_table_outcome;

// Emit one table's DDL + data into `sb`. Returns an xsql_status. Whether the
// table can be read is settled by the first step of its SELECT, before any of
// its text is appended, so a refused table leaves nothing behind: it reports
// EXPORT_TABLE_REFUSED with the table's own reason in `*reason` (heap; the
// caller frees it). A libxsql table refuses an unconstrained scan with
// SQLITE_CONSTRAINT_VTAB and a table whose xBestIndex rejects every plan fails to
// prepare with "no query solution". Ordinary constraint failures remain errors.
static int export_one_table(struct xsql_db *db, const char *table,
                            xsqlc_strbuf *sb, export_table_outcome *outcome,
                            char **reason) {
    *outcome = EXPORT_TABLE_EMPTY;
    *reason = NULL;
    char *quoted = xsql_quote_identifier(table);
    if (!quoted) {
        return XSQL_NO_MEMORY;
    }
    int rc = XSQL_OK;

    // Columns via PRAGMA table_info.
    xsqlc_strbuf pragma;
    xsqlc_sb_init(&pragma);
    xsqlc_sb_append(&pragma, "PRAGMA table_info(");
    xsqlc_sb_append(&pragma, quoted);
    xsqlc_sb_append(&pragma, ");");

    xsql_stmt *cst = NULL;
    rc = xsql_db_prepare(db, xsqlc_sb_cstr(&pragma), &cst);
    xsqlc_sb_free(&pragma);
    if (rc != XSQL_OK) {
        xsqlc_free(quoted);
        return rc;
    }

    // Buffer the CREATE body so we can skip a column-less table entirely.
    xsqlc_strbuf create_body;
    xsqlc_sb_init(&create_body);
    size_t ncols = 0;
    while (xsql_stmt_step(cst) == XSQL_ROW) {
        const char *cname = xsql_stmt_col_text(cst, 1);
        bool type_null = xsql_stmt_column_is_null(cst, 2);
        const char *ctype = xsql_stmt_col_text(cst, 2);
        bool notnull = xsql_stmt_col_int(cst, 3) != 0;
        bool dflt_null = xsql_stmt_column_is_null(cst, 4);
        const char *cdflt = xsql_stmt_col_text(cst, 4);
        bool pk = xsql_stmt_col_int(cst, 5) != 0;

        if (ncols > 0) {
            xsqlc_sb_append_len(&create_body, ",\n", 2);
        }
        char *cq = xsql_quote_identifier(cname ? cname : "");
        if (!cq) {
            // Do not emit an empty identifier and press on with an invalid
            // CREATE TABLE — propagate the allocation failure so the caller sees
            // a clean XSQL_NO_MEMORY instead of corrupt DDL.
            xsqlc_sb_free(&create_body);
            xsql_stmt_finalize(cst);
            xsqlc_free(quoted);
            return XSQL_NO_MEMORY;
        }
        xsqlc_sb_append(&create_body, "    ");
        xsqlc_sb_append(&create_body, cq);
        xsqlc_free(cq);
        if (!type_null && ctype && ctype[0]) {
            xsqlc_sb_putc(&create_body, ' ');
            xsqlc_sb_append(&create_body, ctype);
        }
        if (pk) {
            xsqlc_sb_append(&create_body, " PRIMARY KEY");
        }
        if (notnull) {
            xsqlc_sb_append(&create_body, " NOT NULL");
        }
        if (!dflt_null && cdflt && cdflt[0]) {
            xsqlc_sb_append(&create_body, " DEFAULT ");
            xsqlc_sb_append(&create_body, cdflt);
        }
        ++ncols;
    }
    xsql_stmt_finalize(cst);

    if (ncols == 0) {
        xsqlc_sb_free(&create_body);
        xsqlc_free(quoted);
        return XSQL_OK;  // skip a table with no columns
    }

    // Open the data and take the first step before writing anything.
    xsqlc_strbuf select;
    xsqlc_sb_init(&select);
    xsqlc_sb_append(&select, "SELECT * FROM ");
    xsqlc_sb_append(&select, quoted);
    xsqlc_sb_putc(&select, ';');
    xsql_stmt *dst = NULL;
    rc = xsql_db_prepare(db, xsqlc_sb_cstr(&select), &dst);
    xsqlc_sb_free(&select);
    if (rc != XSQL_OK) {
        if (rc == XSQL_ERROR &&
            strcmp(xsql_db_errmsg(db), EXPORT_NO_QUERY_SOLUTION) == 0) {
            *reason = xsqlc_strdup(EXPORT_NO_QUERY_SOLUTION);
            rc = *reason ? XSQL_OK : XSQL_NO_MEMORY;
            *outcome = EXPORT_TABLE_REFUSED;
        }
        xsqlc_sb_free(&create_body);
        xsqlc_free(quoted);
        return rc;
    }
    int step = xsql_stmt_step(dst);
    if (step != XSQL_ROW && step != XSQL_DONE) {
        const char *message = xsql_stmt_errmsg(dst);
        if (sqlite3_extended_errcode(db->db) == SQLITE_CONSTRAINT_VTAB) {
            *reason = xsqlc_strdup(message ? message : "");
            rc = *reason ? XSQL_OK : XSQL_NO_MEMORY;
            *outcome = EXPORT_TABLE_REFUSED;
        } else {
            xsqlc_str_set(&db->last_error, message);
            rc = XSQL_ERROR;
        }
        xsql_stmt_finalize(dst);
        xsqlc_sb_free(&create_body);
        xsqlc_free(quoted);
        return rc;
    }

    xsqlc_sb_append(sb, "-- Table: ");
    xsqlc_sb_append(sb, table);
    xsqlc_sb_putc(sb, '\n');
    xsqlc_sb_append(sb, "DROP TABLE IF EXISTS ");
    xsqlc_sb_append(sb, quoted);
    xsqlc_sb_append_len(sb, ";\n", 2);
    xsqlc_sb_append(sb, "CREATE TABLE ");
    xsqlc_sb_append(sb, quoted);
    xsqlc_sb_append_len(sb, " (\n", 3);
    xsqlc_sb_append(sb, xsqlc_sb_cstr(&create_body));
    xsqlc_sb_append_len(sb, "\n);\n\n", 5);
    xsqlc_sb_free(&create_body);

    size_t row_count = 0;
    while (step == XSQL_ROW) {
        xsqlc_sb_append(sb, "INSERT INTO ");
        xsqlc_sb_append(sb, quoted);
        xsqlc_sb_append(sb, " VALUES (");
        int col_count = xsql_stmt_column_count(dst);
        for (int i = 0; i < col_count; ++i) {
            switch (xsql_stmt_column_type(dst, i)) {
                case SQLITE_NULL:
                    xsqlc_sb_append(sb, "NULL");
                    break;
                case SQLITE_INTEGER:
                    xsqlc_sb_append_i64(sb, (long long)xsql_stmt_col_i64(dst, i));
                    break;
                case SQLITE_FLOAT:
                    // Seventeen significant digits preserve finite binary64 values.
                    if (!export_append_real_literal(
                            sb, xsql_stmt_col_double(dst, i))) {
                        xsqlc_str_set(&db->last_error, "cannot format REAL value");
                        xsql_stmt_finalize(dst);
                        xsqlc_free(quoted);
                        return XSQL_ERROR;
                    }
                    break;
                case SQLITE_BLOB: {
                    const unsigned char *b =
                        (const unsigned char *)xsql_stmt_col_blob(dst, i);
                    int n = xsql_stmt_col_bytes(dst, i);
                    export_append_blob_hex(sb, b, (n > 0) ? (size_t)n : 0);
                    break;
                }
                case SQLITE_TEXT:
                default: {
                    const char *t = xsql_stmt_col_text(dst, i);
                    int n = xsql_stmt_col_bytes(dst, i);
                    export_append_text_literal(sb, t ? t : "",
                                               (n > 0) ? (size_t)n : 0);
                    break;
                }
            }
            if (i + 1 < col_count) {
                xsqlc_sb_append(sb, ", ");
            }
        }
        xsqlc_sb_append_len(sb, ");\n", 3);
        ++row_count;
        step = xsql_stmt_step(dst);
    }
    if (step != XSQL_DONE) {
        // A failure after the table was opened is a real error, whatever its code.
        xsqlc_str_set(&db->last_error, xsql_stmt_errmsg(dst));
        xsql_stmt_finalize(dst);
        xsqlc_free(quoted);
        return XSQL_ERROR;
    }
    xsql_stmt_finalize(dst);

    xsqlc_sb_append(sb, "-- ");
    xsqlc_sb_append_size(sb, row_count);
    xsqlc_sb_append_len(sb, " rows exported\n\n", 16);

    xsqlc_free(quoted);
    *outcome = EXPORT_TABLE_WRITTEN;
    return XSQL_OK;
}

// Replace `path` with `staged` (same directory). rename() cannot replace an
// existing file on Windows, so use MoveFileEx there.
static bool export_replace_file(const char *staged, const char *path) {
#ifdef _WIN32
    return MoveFileExA(staged, path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(staged, path) == 0;
#endif
}

// Create an invocation-owned sibling file. C11's `x` mode makes creation
// exclusive; the random suffix plus retry handles concurrent exporters.
static FILE *export_open_temp(const char *path, xsqlc_strbuf *temp_path) {
    static const char hex[] = "0123456789abcdef";
    unsigned char nonce[16];
    xsqlc_sb_init(temp_path);
    for (int attempt = 0; attempt < 128; ++attempt) {
        xsqlc_sb_free(temp_path);
        xsqlc_sb_init(temp_path);
        sqlite3_randomness((int)sizeof(nonce), nonce);
        xsqlc_sb_append(temp_path, path);
        xsqlc_sb_append(temp_path, ".stage.");
        for (size_t i = 0; i < sizeof(nonce); ++i) {
            char pair[2] = {hex[nonce[i] >> 4], hex[nonce[i] & 0x0f]};
            xsqlc_sb_append_len(temp_path, pair, 2);
        }
        xsqlc_sb_append(temp_path, ".tmp");
        if (xsqlc_sb_oom(temp_path)) {
            errno = ENOMEM;
            return NULL;
        }
        FILE *file = fopen(xsqlc_sb_cstr(temp_path), "wbx");
        if (file) {
            return file;
        }
        if (errno != EEXIST) {
            return NULL;
        }
    }
    errno = EEXIST;
    return NULL;
}

// Write `header` then `body` to `path` through a sibling staged file renamed
// over it, so a failure never leaves a partial `path`. Returns an xsql_status.
static int export_write_file(struct xsql_db *db, const char *path,
                             const xsqlc_strbuf *header, const xsqlc_strbuf *body) {
    xsqlc_strbuf staged;
    xsqlc_sb_init(&staged);
    FILE *f = export_open_temp(path, &staged);
    if (!f) {
        xsqlc_str_set(&db->last_error, "cannot open output file");
        int result = errno == ENOMEM ? XSQL_NO_MEMORY : XSQL_CANT_OPEN;
        xsqlc_sb_free(&staged);
        return result;
    }
    const char *staged_path = xsqlc_sb_cstr(&staged);
    size_t written = fwrite(header->data ? header->data : "", 1, header->len, f);
    written += fwrite(body->data ? body->data : "", 1, body->len, f);
    int close_rc = fclose(f);
    int result = XSQL_OK;
    if (written != header->len + body->len || close_rc != 0 ||
        !export_replace_file(staged_path, path)) {
        remove(staged_path);
        xsqlc_str_set(&db->last_error, "cannot write output file");
        result = XSQL_IO_ERROR;
    }
    xsqlc_sb_free(&staged);
    return result;
}

int xsql_export_tables(struct xsql_db *db, const char *const *names, size_t count,
                       const char *path) {
    if (!db || !path) {
        return XSQL_MISUSE;
    }

    const bool explicit_request = names && count > 0;
    char **owned_names = NULL;
    size_t ntables = 0;
    const char *const *tables = names;
    if (!explicit_request) {
        int rc = export_collect_table_names(db, &owned_names, &ntables);
        if (rc != XSQL_OK) {
            return rc;
        }
        tables = (const char *const *)owned_names;
    } else {
        ntables = count;
    }

    xsqlc_strbuf body;
    xsqlc_sb_init(&body);
    xsqlc_strbuf skipped;  // "a, b" — the tables skipped for needing constraints
    xsqlc_sb_init(&skipped);
    size_t exported = 0;

    int rc = XSQL_OK;
    for (size_t i = 0; i < ntables && rc == XSQL_OK; ++i) {
        export_table_outcome outcome = EXPORT_TABLE_EMPTY;
        char *reason = NULL;
        rc = export_one_table(db, tables[i], &body, &outcome, &reason);
        if (rc != XSQL_OK) {
            break;
        }
        if (outcome == EXPORT_TABLE_WRITTEN) {
            ++exported;
        } else if (outcome == EXPORT_TABLE_REFUSED) {
            if (explicit_request) {
                xsqlc_strbuf message;
                xsqlc_sb_init(&message);
                xsqlc_sb_append(&message, "Table '");
                xsqlc_sb_append(&message, tables[i]);
                xsqlc_sb_append(&message,
                                "' cannot be exported: it requires constraints "
                                "(WHERE) to be read");
                if (reason && reason[0]) {
                    xsqlc_sb_append(&message, " (");
                    xsqlc_sb_append(&message, reason);
                    xsqlc_sb_putc(&message, ')');
                }
                rc = xsqlc_sb_oom(&message) ? XSQL_NO_MEMORY : XSQL_CONSTRAINT;
                if (rc == XSQL_CONSTRAINT) {
                    xsqlc_str_set(&db->last_error, xsqlc_sb_cstr(&message));
                }
                xsqlc_sb_free(&message);
            } else {
                if (skipped.len > 0) {
                    xsqlc_sb_append_len(&skipped, ", ", 2);
                }
                xsqlc_sb_append(&skipped, tables[i]);
            }
        }
        xsqlc_free(reason);
    }
    if (owned_names) {
        xsql_free_strings(owned_names, ntables);
    }

    xsqlc_strbuf header;
    xsqlc_sb_init(&header);
    if (rc == XSQL_OK) {
        xsqlc_sb_append(&header, "-- SQL Export\n");
        xsqlc_sb_append(&header, "-- Tables: ");
        xsqlc_sb_append_size(&header, exported);
        xsqlc_sb_putc(&header, '\n');
        if (skipped.len > 0) {
            xsqlc_sb_append(&header, "-- Skipped (requires constraints): ");
            xsqlc_sb_append(&header, xsqlc_sb_cstr(&skipped));
            xsqlc_sb_putc(&header, '\n');
        }
        xsqlc_sb_putc(&header, '\n');
        if (xsqlc_sb_oom(&header) || xsqlc_sb_oom(&skipped) || xsqlc_sb_oom(&body)) {
            rc = XSQL_NO_MEMORY;
        }
    }
    if (rc == XSQL_OK) {
        rc = export_write_file(db, path, &header, &body);
    }
    if (rc == XSQL_OK) {
        xsqlc_str_set(&db->last_error, NULL);
    }
    xsqlc_sb_free(&header);
    xsqlc_sb_free(&skipped);
    xsqlc_sb_free(&body);
    return rc;
}
