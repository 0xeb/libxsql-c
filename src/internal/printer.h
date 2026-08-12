// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// printer.h — the shared ASCII table core. Used by the
// public xsql_print_table (printer.c) and the per-statement table renderer
// (format.c). PRIVATE.

#ifndef XSQLC_INTERNAL_PRINTER_H
#define XSQLC_INTERNAL_PRINTER_H

#include <stdbool.h>
#include <stddef.h>

#include "internal/strbuf.h"

// Cell accessor: return the (row, col) cell as a borrowed NUL-terminated string.
typedef const char *(*xsqlc_cell_fn)(void *ctx, size_t row, size_t col);

// Render a table into `sb`. When `boxed` is false (default), columns are
// left-aligned, 2-space separated, with a dashed rule row, and a zero-column
// table renders `no_result` (optionally followed by a newline). When `boxed` is
// true, renders the classic +---+ / | cell | box; a zero-column table renders
// nothing, and `boxed_row_count_footer` appends a "N row(s)\n" footer.
void xsqlc_render_table(xsqlc_strbuf *sb, const char *const *columns,
                        size_t ncols, size_t nrows, xsqlc_cell_fn cell, void *ctx,
                        const char *no_result, bool newline_after_no_result,
                        bool boxed, bool boxed_row_count_footer);

#endif  // XSQLC_INTERNAL_PRINTER_H
