// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// format.h — renderers over the script envelope: the canonical JSON wire format
// plus human/pipe formats (text, CSV, TSV) and a standalone ASCII table printer.
// Every renderer returns heap text released with xsql_free().

#ifndef XSQLC_FORMAT_H
#define XSQLC_FORMAT_H

#include <libxsqlc/common.h>
#include <libxsqlc/script.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Envelope renderers (heap; free with xsql_free(); NULL only on allocation failure)
// ---------------------------------------------------------------------------

// Canonical JSON envelope, including the integer-when-whole elapsed_ms rule.
// `include_sql` echoes each
// statement's SQL (only when the envelope captured it).
XSQL_API char *xsql_script_result_to_json(const xsql_script_result *r,
                                          bool include_sql);

// Human-readable text: per-statement header + an ASCII table (or ERROR line).
XSQL_API char *xsql_script_result_to_text(const xsql_script_result *r);

// Delimited output for terminals / unix pipes. A real SQL NULL renders as an empty
// field; CSV follows RFC 4180 quoting, TSV neutralizes embedded tab/newline.
XSQL_API char *xsql_script_result_to_csv(const xsql_script_result *r);
XSQL_API char *xsql_script_result_to_tsv(const xsql_script_result *r);

// JSON Lines / NDJSON: one self-describing JSON object per row, one per line —
// keyed by column NAME (the "records" shape a bulk consumer wants). Cell values are
// emitted as JSON strings (SQL NULL => null), matching the canonical envelope's cell
// treatment (xsql_script_result_to_json) so a numeric column like `rva` is "4096"
// exactly as it is there. A failed statement emits one
// `{"statement_index":I,"error":"…"}` line instead of rows; a column-less statement
// (a successful write) emits nothing; a parse error emits `{"error":"…"}`. No
// enclosing array/envelope, so the output streams and appends cleanly.
XSQL_API char *xsql_script_result_to_jsonl(const xsql_script_result *r);

// ---------------------------------------------------------------------------
// Single-statement renderers
// ---------------------------------------------------------------------------

// Render one statement as an ASCII table (SQL NULL shown as the text "NULL").
XSQL_API char *xsql_render_statement_table(const xsql_script_statement *s);

// Render one statement as a delimited block (`csv` selects RFC-4180 vs TSV rules).
XSQL_API char *xsql_render_statement_delimited(const xsql_script_statement *s,
                                               char delim, bool csv);

// ---------------------------------------------------------------------------
// Standalone ASCII table printer
// ---------------------------------------------------------------------------

// Table rendering style. `borderless` is the historical output (two-space
// columns, dashed rule); `boxed` is the classic +---+ / | cell | box that the
// CLIs (pdbsql, idasql) hand-rolled, now shareable.
typedef enum xsql_table_style {
    XSQL_TABLE_BORDERLESS = 0,
    XSQL_TABLE_BOXED = 1,
} xsql_table_style;

// Options for xsql_print_table. Designed for designated initializers; an all-zero
// struct (or NULL) selects the defaults ("(no result)" with no trailing newline).
typedef struct xsql_table_print_opts {
    size_t      struct_size;              // sizeof(xsql_table_print_opts), or 0
    const char *no_result;                // text for an empty (0-column) table
    bool        newline_after_no_result;  // append '\n' after the no-result text
    // Appended (read only when struct_size covers them, honoring the ABI-growth
    // idiom). A zero-init struct selects borderless; a bare boxed style shows the
    // "N row(s)" footer (the classic box), so the footer flag is inverted (a
    // SUPPRESS flag) — zero-init keeps the footer, matching the C++/Rust default.
    xsql_table_style style;                  // borderless (0) or boxed
    bool        boxed_no_row_count_footer;   // boxed only: omit the "N row(s)\n" footer
} xsql_table_print_opts;

// Print a table from `ncols` column headers and a row-major array of `nrows*ncols`
// cell strings. Columns are left-aligned, 2-space separated, with a dashed rule
// row. `opts` may be NULL.
XSQL_API char *xsql_print_table(const char *const *columns, size_t ncols,
                                const char *const *cells, size_t nrows,
                                const xsql_table_print_opts *opts);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_FORMAT_H
