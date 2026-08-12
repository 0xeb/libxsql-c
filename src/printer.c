// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// printer.c — the ASCII table core (internal xsqlc_render_table) and the public
// xsql_print_table.

#include <libxsqlc/format.h>

#include "internal/internal.h"
#include "internal/printer.h"
#include "internal/strbuf.h"

#include <string.h>

// Append one already-materialized text line with column padding. `w` holds the
// per-column widths; only non-last columns shorter than their width are padded.
static void render_line_texts(xsqlc_strbuf *sb, const char *const *texts,
                              size_t ncols, const size_t *w) {
    for (size_t i = 0; i < ncols; ++i) {
        if (i) {
            xsqlc_sb_append_len(sb, "  ", 2);
        }
        const char *t = texts[i] ? texts[i] : "";
        size_t len = strlen(t);
        xsqlc_sb_append_len(sb, t, len);
        if (i + 1 < ncols && len < w[i]) {
            for (size_t k = len; k < w[i]; ++k) {
                xsqlc_sb_putc(sb, ' ');
            }
        }
    }
    xsqlc_sb_putc(sb, '\n');
}

// `+` + (width+2) dashes per column + `+`. The +2 covers the space around each cell.
static void render_boxed_rule(xsqlc_strbuf *sb, size_t ncols, const size_t *w) {
    xsqlc_sb_putc(sb, '+');
    for (size_t i = 0; i < ncols; ++i) {
        for (size_t k = 0; k < w[i] + 2; ++k) {
            xsqlc_sb_putc(sb, '-');
        }
        xsqlc_sb_putc(sb, '+');
    }
    xsqlc_sb_putc(sb, '\n');
}

// `| ` then, per column, the value left-padded to width + ` | ` — the trailing
// space after the final `|` is load-bearing (matches the historical box).
static void render_boxed_line(xsqlc_strbuf *sb, const char *const *texts,
                              size_t ncols, const size_t *w) {
    xsqlc_sb_append_len(sb, "| ", 2);
    for (size_t i = 0; i < ncols; ++i) {
        const char *t = texts[i] ? texts[i] : "";
        size_t len = strlen(t);
        xsqlc_sb_append_len(sb, t, len);
        for (size_t k = len; k < w[i]; ++k) {
            xsqlc_sb_putc(sb, ' ');
        }
        xsqlc_sb_append_len(sb, " | ", 3);
    }
    xsqlc_sb_putc(sb, '\n');
}

void xsqlc_render_table(xsqlc_strbuf *sb, const char *const *columns,
                        size_t ncols, size_t nrows, xsqlc_cell_fn cell, void *ctx,
                        const char *no_result, bool newline_after_no_result,
                        bool boxed, bool boxed_row_count_footer) {
    if (ncols == 0) {
        if (!boxed) {  // boxed: a column-less result prints NOTHING
            xsqlc_sb_append(sb, no_result ? no_result : "(no result)");
            if (newline_after_no_result) {
                xsqlc_sb_putc(sb, '\n');
            }
        }
        return;
    }

    size_t *w = (size_t *)xsqlc_malloc(ncols * sizeof(*w));
    if (!w) {
        sb->oom = true;  // propagate as a build failure the caller checks once
        return;
    }
    for (size_t i = 0; i < ncols; ++i) {
        w[i] = columns[i] ? strlen(columns[i]) : 0;
    }
    for (size_t r = 0; r < nrows; ++r) {
        for (size_t c = 0; c < ncols; ++c) {
            const char *t = cell(ctx, r, c);
            size_t len = t ? strlen(t) : 0;
            if (len > w[c]) {
                w[c] = len;
            }
        }
    }

    const char **line = (const char **)xsqlc_malloc(ncols * sizeof(*line));
    if (!line) {
        xsqlc_free(w);
        sb->oom = true;
        return;
    }

    if (boxed) {
        render_boxed_rule(sb, ncols, w);
        render_boxed_line(sb, columns, ncols, w);
        render_boxed_rule(sb, ncols, w);
        for (size_t r = 0; r < nrows; ++r) {
            for (size_t c = 0; c < ncols; ++c) {
                line[c] = cell(ctx, r, c);
            }
            render_boxed_line(sb, line, ncols, w);
        }
        render_boxed_rule(sb, ncols, w);
        if (boxed_row_count_footer) {
            xsqlc_sb_append_size(sb, nrows);
            xsqlc_sb_append(sb, " row(s)\n");
        }
        xsqlc_free(line);
        xsqlc_free(w);
        return;
    }

    // Borderless (default): header + dashed rule, then data rows.
    render_line_texts(sb, columns, ncols, w);
    for (size_t i = 0; i < ncols; ++i) {
        if (i) {
            xsqlc_sb_append_len(sb, "  ", 2);
        }
        for (size_t k = 0; k < w[i]; ++k) {
            xsqlc_sb_putc(sb, '-');
        }
    }
    xsqlc_sb_putc(sb, '\n');

    for (size_t r = 0; r < nrows; ++r) {
        for (size_t c = 0; c < ncols; ++c) {
            line[c] = cell(ctx, r, c);
        }
        render_line_texts(sb, line, ncols, w);
    }

    xsqlc_free(line);
    xsqlc_free(w);
}

// --- Public standalone printer ---------------------------------------------

// Flat row-major cell accessor for xsql_print_table.
typedef struct {
    const char *const *cells;
    size_t ncols;
} flat_cells_ctx;

static const char *flat_cell(void *ctx, size_t row, size_t col) {
    flat_cells_ctx *fc = (flat_cells_ctx *)ctx;
    const char *v = fc->cells[row * fc->ncols + col];
    return v ? v : "";
}

char *xsql_print_table(const char *const *columns, size_t ncols,
                       const char *const *cells, size_t nrows,
                       const xsql_table_print_opts *opts) {
    const char *no_result = "(no result)";
    bool nl_after = false;
    bool boxed = false;
    bool boxed_footer = true;
    if (opts) {
        if (opts->no_result) {
            no_result = opts->no_result;
        }
        nl_after = opts->newline_after_no_result;
        // The style/footer fields are ABI-growth additions: read them only when
        // the caller's struct_size covers them (an older/smaller struct stays
        // borderless). The footer flag is inverted (a SUPPRESS flag), so a
        // zero-init boxed struct keeps the footer — matching the C++/Rust default.
        if (opts->struct_size >=
            offsetof(xsql_table_print_opts, boxed_no_row_count_footer) +
                sizeof(opts->boxed_no_row_count_footer)) {
            boxed = opts->style == XSQL_TABLE_BOXED;
            boxed_footer = !opts->boxed_no_row_count_footer;
        }
    }

    flat_cells_ctx fc = {cells, ncols};
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_render_table(&sb, columns, ncols, (cells ? nrows : 0), flat_cell, &fc,
                       no_result, nl_after, boxed, boxed_footer);
    return xsqlc_sb_detach(&sb);
}
