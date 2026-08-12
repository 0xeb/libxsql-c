// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// script_internal.h — concrete layout of the script envelope handles
// (xsql_script_result / xsql_script_statement). Shared privately between
// script.c (build + accessors) and format.c (renderers).
//
// PRIVATE: under src/internal, never installed, never in a public header.

#ifndef XSQLC_INTERNAL_SCRIPT_INTERNAL_H
#define XSQLC_INTERNAL_SCRIPT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

// One statement's materialized result. Rows are rectangular (ncols cells each,
// row-major). The SQL-NULL model uses a
// parallel "was a mask entry provided?" flag: when a cell has no provided mask
// entry, is_null_cell falls back to the legacy `cell text == "NULL"` sentinel.
struct xsql_script_statement {
    size_t         statement_index;
    bool           success;

    char         **columns;        // ncols column names (xsqlc heap)
    size_t         ncols;

    char         **cells;          // nrows*ncols (row-major); "" placeholder
    size_t        *cell_len;       // nrows*ncols byte lengths (embedded-NUL safe)
    unsigned char *cell_null;      // nrows*ncols resolved mask (1 == SQL NULL)
    unsigned char *cell_has_mask;  // nrows*ncols: a mask entry was provided
    size_t         nrows;
    size_t         rows_cap;       // row capacity of the flat arrays above

    double         elapsed_ms;
    char          *error;          // NULL/"" when success
    char          *sql;            // NULL/"" unless include_sql

    bool           timed_out;      // hit the ScriptOptions timeout deadline
    bool           partial;        // rows are partial (a timeout kept them)
    char         **warnings;       // warning_count strings (xsqlc heap)
    size_t         warning_count;
};

// The aggregate envelope.
struct xsql_script_result {
    bool    success;
    size_t  statement_count;
    struct xsql_script_statement *results;  // `count` records
    size_t  count;
    size_t  results_cap;
    size_t  row_count_total;
    double  elapsed_ms_total;
    bool    has_first_error;
    size_t  first_error_index;
    char   *parse_error;  // NULL/"" unless the splitter/JSON parse failed
};

// is_null_cell — reports whether the cell at (row, col) is a SQL NULL.
bool xsqlc_stmt_is_null_cell(const struct xsql_script_statement *s, size_t row,
                             size_t col);

// Release one statement's owned storage (does not free `s` itself).
void xsqlc_stmt_dispose(struct xsql_script_statement *s);

#endif  // XSQLC_INTERNAL_SCRIPT_INTERNAL_H
