// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// export.c — xsql_export_tables: dump a database (or a chosen set of tables) as a
// self-contained SQL script, including the %.16g REAL literal rule and the
// uppercase X'..' blob-hex form.

#include <libxsqlc/script.h>

#include <libxsqlc/database.h>
#include <libxsqlc/result.h>
#include <libxsqlc/statement.h>
#include <libxsqlc/status.h>

#include "internal/internal.h"
#include "internal/strbuf.h"

#include <sqlite3.h>

#include <stdio.h>

// Append a SQL string literal: '...' with embedded ' doubled (over `len` bytes).
static void export_append_text_literal(xsqlc_strbuf *sb, const char *text,
                                       size_t len) {
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

// Emit one table's DDL + data into `sb`. Returns an xsql_status.
static int export_one_table(struct xsql_db *db, const char *table,
                            xsqlc_strbuf *sb) {
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

    // Data rows.
    xsqlc_strbuf select;
    xsqlc_sb_init(&select);
    xsqlc_sb_append(&select, "SELECT * FROM ");
    xsqlc_sb_append(&select, quoted);
    xsqlc_sb_putc(&select, ';');
    xsql_stmt *dst = NULL;
    rc = xsql_db_prepare(db, xsqlc_sb_cstr(&select), &dst);
    xsqlc_sb_free(&select);
    if (rc != XSQL_OK) {
        xsqlc_free(quoted);
        return rc;
    }

    size_t row_count = 0;
    int step;
    while ((step = xsql_stmt_step(dst)) == XSQL_ROW) {
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
                    // A3: %.16g == std::setprecision(digits10+1) default float.
                    xsqlc_sb_printf(sb, "%.16g", xsql_stmt_col_double(dst, i));
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
    }
    xsql_stmt_finalize(dst);
    if (step != XSQL_DONE) {
        xsqlc_free(quoted);
        return XSQL_ERROR;
    }

    xsqlc_sb_append(sb, "-- ");
    xsqlc_sb_append_size(sb, row_count);
    xsqlc_sb_append_len(sb, " rows exported\n\n", 16);

    xsqlc_free(quoted);
    return XSQL_OK;
}

int xsql_export_tables(struct xsql_db *db, const char *const *names, size_t count,
                       const char *path) {
    if (!db || !path) {
        return XSQL_MISUSE;
    }

    char **owned_names = NULL;
    size_t ntables = 0;
    const char *const *tables = names;
    if (!names || count == 0) {
        int rc = export_collect_table_names(db, &owned_names, &ntables);
        if (rc != XSQL_OK) {
            return rc;
        }
        tables = (const char *const *)owned_names;
    } else {
        ntables = count;
    }

    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "-- SQL Export\n");
    xsqlc_sb_append(&sb, "-- Tables: ");
    xsqlc_sb_append_size(&sb, ntables);
    xsqlc_sb_append_len(&sb, "\n\n", 2);

    int rc = XSQL_OK;
    for (size_t i = 0; i < ntables; ++i) {
        rc = export_one_table(db, tables[i], &sb);
        if (rc != XSQL_OK) {
            break;
        }
    }
    if (owned_names) {
        xsql_free_strings(owned_names, ntables);
    }

    if (rc != XSQL_OK || xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        return (rc != XSQL_OK) ? rc : XSQL_NO_MEMORY;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        xsqlc_sb_free(&sb);
        return XSQL_CANT_OPEN;
    }
    size_t written = fwrite(sb.data ? sb.data : "", 1, sb.len, f);
    int close_rc = fclose(f);
    int result = (written == sb.len && close_rc == 0) ? XSQL_OK : XSQL_IO_ERROR;
    xsqlc_sb_free(&sb);
    return result;
}
