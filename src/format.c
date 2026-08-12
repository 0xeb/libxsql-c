// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// format.c — renderers over the script envelope: the canonical JSON wire format
// and the human/pipe text/CSV/TSV formats (including the A6 integer-when-whole
// elapsed_ms rule via json_emit). The ASCII table core lives in printer.c.

#include <libxsqlc/format.h>

#include "internal/internal.h"
#include "internal/json.h"
#include "internal/printer.h"
#include "internal/script_internal.h"
#include "internal/strbuf.h"

#include <string.h>

// Byte length of a statement cell (embedded-NUL safe).
static size_t cell_len(const struct xsql_script_statement *s, size_t row,
                       size_t col) {
    return s->cell_len ? s->cell_len[row * s->ncols + col] : 0;
}
static const char *cell_ptr(const struct xsql_script_statement *s, size_t row,
                            size_t col) {
    return s->cells[row * s->ncols + col];
}

// ===========================================================================
// JSON
// ===========================================================================

static void append_statement_json(xsqlc_strbuf *sb,
                                   const struct xsql_script_statement *s,
                                   bool include_sql) {
    xsqlc_sb_putc(sb, '{');
    xsqlc_sb_append(sb, "\"statement_index\":");
    xsqlc_sb_append_size(sb, s->statement_index);

    xsqlc_sb_append(sb, ",\"success\":");
    xsqlc_sb_append(sb, s->success ? "true" : "false");

    if (include_sql && s->sql && s->sql[0]) {
        xsqlc_sb_append(sb, ",\"sql\":");
        xsqlc_json_append_string(sb, s->sql, strlen(s->sql));
    }

    xsqlc_sb_append(sb, ",\"columns\":[");
    for (size_t i = 0; i < s->ncols; ++i) {
        if (i) {
            xsqlc_sb_putc(sb, ',');
        }
        xsqlc_json_append_string(sb, s->columns[i], strlen(s->columns[i]));
    }
    xsqlc_sb_putc(sb, ']');

    xsqlc_sb_append(sb, ",\"rows\":[");
    for (size_t r = 0; r < s->nrows; ++r) {
        if (r) {
            xsqlc_sb_putc(sb, ',');
        }
        xsqlc_sb_putc(sb, '[');
        for (size_t c = 0; c < s->ncols; ++c) {
            if (c) {
                xsqlc_sb_putc(sb, ',');
            }
            if (xsqlc_stmt_is_null_cell(s, r, c)) {
                xsqlc_sb_append(sb, "null");
            } else {
                xsqlc_json_append_string(sb, cell_ptr(s, r, c), cell_len(s, r, c));
            }
        }
        xsqlc_sb_putc(sb, ']');
    }
    xsqlc_sb_putc(sb, ']');

    xsqlc_sb_append(sb, ",\"row_count\":");
    xsqlc_sb_append_size(sb, s->nrows);

    xsqlc_sb_append(sb, ",\"elapsed_ms\":");
    xsqlc_json_append_number(sb, s->elapsed_ms);

    xsqlc_sb_append(sb, ",\"error\":");
    if (!s->error || s->error[0] == '\0') {
        xsqlc_sb_append(sb, "null");
    } else {
        xsqlc_json_append_string(sb, s->error, strlen(s->error));
    }

    if (s->timed_out) {
        xsqlc_sb_append(sb, ",\"timed_out\":true");
    }
    if (s->partial) {
        xsqlc_sb_append(sb, ",\"partial\":true");
    }
    if (s->warning_count > 0) {
        xsqlc_sb_append(sb, ",\"warnings\":[");
        for (size_t i = 0; i < s->warning_count; ++i) {
            if (i) {
                xsqlc_sb_putc(sb, ',');
            }
            const char *warning = s->warnings[i] ? s->warnings[i] : "";
            xsqlc_json_append_string(sb, warning, strlen(warning));
        }
        xsqlc_sb_putc(sb, ']');
    }

    xsqlc_sb_putc(sb, '}');
}

char *xsql_script_result_to_json(const xsql_script_result *r, bool include_sql) {
    if (!r) {
        return NULL;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);

    if (r->parse_error && r->parse_error[0]) {
        xsqlc_sb_append(&sb, "{\"success\":false");
        xsqlc_sb_append(&sb, ",\"statement_count\":0");
        xsqlc_sb_append(&sb, ",\"results\":[]");
        xsqlc_sb_append(&sb, ",\"row_count_total\":0");
        xsqlc_sb_append(&sb, ",\"elapsed_ms_total\":0");
        xsqlc_sb_append(&sb, ",\"first_error_index\":null");
        xsqlc_sb_append(&sb, ",\"parse_error\":");
        xsqlc_json_append_string(&sb, r->parse_error, strlen(r->parse_error));
        xsqlc_sb_putc(&sb, '}');
        return xsqlc_sb_detach(&sb);
    }

    xsqlc_sb_append(&sb, "{\"success\":");
    xsqlc_sb_append(&sb, r->success ? "true" : "false");

    xsqlc_sb_append(&sb, ",\"statement_count\":");
    xsqlc_sb_append_size(&sb, r->statement_count);

    xsqlc_sb_append(&sb, ",\"results\":[");
    for (size_t i = 0; i < r->count; ++i) {
        if (i) {
            xsqlc_sb_putc(&sb, ',');
        }
        append_statement_json(&sb, &r->results[i], include_sql);
    }
    xsqlc_sb_putc(&sb, ']');

    xsqlc_sb_append(&sb, ",\"row_count_total\":");
    xsqlc_sb_append_size(&sb, r->row_count_total);

    xsqlc_sb_append(&sb, ",\"elapsed_ms_total\":");
    xsqlc_json_append_number(&sb, r->elapsed_ms_total);

    xsqlc_sb_append(&sb, ",\"first_error_index\":");
    if (r->has_first_error) {
        xsqlc_sb_append_size(&sb, r->first_error_index);
    } else {
        xsqlc_sb_append(&sb, "null");
    }

    xsqlc_sb_putc(&sb, '}');
    return xsqlc_sb_detach(&sb);
}

// ===========================================================================
// Text (ASCII table)
// ===========================================================================

// Display accessor: a SQL NULL renders as the literal text "NULL".
static const char *stmt_display_cell(void *ctx, size_t row, size_t col) {
    const struct xsql_script_statement *s =
        (const struct xsql_script_statement *)ctx;
    if (xsqlc_stmt_is_null_cell(s, row, col)) {
        return "NULL";
    }
    return cell_ptr(s, row, col);
}

char *xsql_render_statement_table(const xsql_script_statement *s) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    if (s) {
        xsqlc_render_table(&sb, (const char *const *)s->columns, s->ncols,
                           s->nrows, stmt_display_cell, (void *)s, "(no result)",
                           false, false, false);
    }
    return xsqlc_sb_detach(&sb);
}

char *xsql_script_result_to_text(const xsql_script_result *r) {
    if (!r) {
        return NULL;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);

    if (r->parse_error && r->parse_error[0]) {
        xsqlc_sb_append(&sb, "PARSE ERROR: ");
        xsqlc_sb_append(&sb, r->parse_error);
        return xsqlc_sb_detach(&sb);
    }

    for (size_t i = 0; i < r->count; ++i) {
        const struct xsql_script_statement *s = &r->results[i];
        xsqlc_sb_append(&sb, "-- statement ");
        xsqlc_sb_append_size(&sb, s->statement_index + 1);
        xsqlc_sb_putc(&sb, '/');
        xsqlc_sb_append_size(&sb, r->statement_count);
        xsqlc_sb_append(&sb, " (");
        xsqlc_sb_printf(&sb, "%g", s->elapsed_ms);  // ostream default, not the JSON rule
        xsqlc_sb_append(&sb, " ms, ");
        xsqlc_sb_append_size(&sb, s->nrows);
        xsqlc_sb_append(&sb, s->nrows == 1 ? " row)" : " rows)");
        xsqlc_sb_putc(&sb, '\n');
        if (s->success) {
            xsqlc_render_table(&sb, (const char *const *)s->columns, s->ncols,
                               s->nrows, stmt_display_cell, (void *)s,
                               "(no result)", false, false, false);
        } else {
            xsqlc_sb_append(&sb, "ERROR: ");
            xsqlc_sb_append(&sb, s->error ? s->error : "");
            xsqlc_sb_putc(&sb, '\n');
        }
        // Surface the timeout / partial-result signalling — emitted ONLY when set,
        // so a normal result is byte-identical. `--`-prefixed to match the
        // `-- statement` convention. Mirrors the C++ / Rust reference.
        for (size_t wi = 0; wi < s->warning_count; ++wi) {
            xsqlc_sb_append(&sb, "-- warning: ");
            xsqlc_sb_append(&sb, s->warnings[wi] ? s->warnings[wi] : "");
            xsqlc_sb_putc(&sb, '\n');
        }
        if (s->timed_out) {
            xsqlc_sb_append(&sb, "-- query timed out; results are partial\n");
        } else if (s->partial) {
            xsqlc_sb_append(&sb, "-- results are partial\n");
        }
        if (i + 1 < r->count) {
            xsqlc_sb_putc(&sb, '\n');
        }
    }
    return xsqlc_sb_detach(&sb);
}

// ===========================================================================
// CSV / TSV
// ===========================================================================

static void append_delimited_field(xsqlc_strbuf *sb, const char *v, size_t len,
                                   bool csv, bool is_null) {
    if (is_null) {
        return;  // real SQL NULL -> empty field
    }
    if (csv) {
        bool needs_quote = false;
        for (size_t i = 0; i < len; ++i) {
            char c = v[i];
            if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                needs_quote = true;
                break;
            }
        }
        if (!needs_quote) {
            xsqlc_sb_append_len(sb, v, len);
            return;
        }
        xsqlc_sb_putc(sb, '"');
        for (size_t i = 0; i < len; ++i) {
            if (v[i] == '"') {
                xsqlc_sb_append_len(sb, "\"\"", 2);
            } else {
                xsqlc_sb_putc(sb, v[i]);
            }
        }
        xsqlc_sb_putc(sb, '"');
    } else {
        for (size_t i = 0; i < len; ++i) {
            char c = v[i];
            xsqlc_sb_putc(sb, (c == '\t' || c == '\n' || c == '\r') ? ' ' : c);
        }
    }
}

static void render_statement_delimited_into(xsqlc_strbuf *sb,
                                            const struct xsql_script_statement *s,
                                            char delim, bool csv) {
    if (s->ncols == 0) {
        xsqlc_sb_append(sb, "(no result)\n");
        return;
    }
    for (size_t i = 0; i < s->ncols; ++i) {
        if (i) {
            xsqlc_sb_putc(sb, delim);
        }
        append_delimited_field(sb, s->columns[i], strlen(s->columns[i]), csv,
                               false);
    }
    xsqlc_sb_putc(sb, '\n');
    for (size_t r = 0; r < s->nrows; ++r) {
        for (size_t c = 0; c < s->ncols; ++c) {
            if (c) {
                xsqlc_sb_putc(sb, delim);
            }
            append_delimited_field(sb, cell_ptr(s, r, c), cell_len(s, r, c), csv,
                                   xsqlc_stmt_is_null_cell(s, r, c));
        }
        xsqlc_sb_putc(sb, '\n');
    }
}

char *xsql_render_statement_delimited(const xsql_script_statement *s, char delim,
                                      bool csv) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    if (s) {
        render_statement_delimited_into(&sb, s, delim, csv);
    }
    return xsqlc_sb_detach(&sb);
}

static char *script_result_to_delimited(const xsql_script_result *r, char delim,
                                        bool csv) {
    if (!r) {
        return NULL;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);

    if (r->parse_error && r->parse_error[0]) {
        xsqlc_sb_append(&sb, "# PARSE ERROR: ");
        xsqlc_sb_append(&sb, r->parse_error);
        xsqlc_sb_putc(&sb, '\n');
        return xsqlc_sb_detach(&sb);
    }

    bool multi = r->count > 1;
    for (size_t i = 0; i < r->count; ++i) {
        const struct xsql_script_statement *s = &r->results[i];
        if (multi) {
            xsqlc_sb_append(&sb, "# statement ");
            xsqlc_sb_append_size(&sb, s->statement_index + 1);
            xsqlc_sb_putc(&sb, '/');
            xsqlc_sb_append_size(&sb, r->statement_count);
            xsqlc_sb_putc(&sb, '\n');
        }
        if (s->success) {
            render_statement_delimited_into(&sb, s, delim, csv);
        } else {
            xsqlc_sb_append(&sb, "# ERROR: ");
            xsqlc_sb_append(&sb, s->error ? s->error : "");
            xsqlc_sb_putc(&sb, '\n');
        }
        if (multi && i + 1 < r->count) {
            xsqlc_sb_putc(&sb, '\n');
        }
    }
    return xsqlc_sb_detach(&sb);
}

char *xsql_script_result_to_csv(const xsql_script_result *r) {
    return script_result_to_delimited(r, ',', true);
}

char *xsql_script_result_to_tsv(const xsql_script_result *r) {
    return script_result_to_delimited(r, '\t', false);
}

// ===========================================================================
// JSON Lines / NDJSON
// ===========================================================================

static bool jsonl_key_used(char *const *keys, size_t count,
                           const char *candidate) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(keys[i], candidate) == 0) {
            return true;
        }
    }
    return false;
}

// JSON objects cannot represent duplicate keys without losing data in ordinary
// parsers. Preserve the first label and suffix later collisions as "#2", "#3",
// and so on, also avoiding collisions with labels already emitted.
static char **jsonl_unique_keys(const struct xsql_script_statement *s) {
    if (s->ncols == 0) {
        return NULL;
    }
    size_t bytes = 0;
    if (!xsqlc_mul_ok(s->ncols, sizeof(char *), &bytes)) {
        return NULL;
    }
    char **keys = (char **)xsqlc_malloc(bytes);
    if (!keys) {
        return NULL;
    }
    memset(keys, 0, bytes);
    for (size_t c = 0; c < s->ncols; ++c) {
        const char *base = s->columns[c] ? s->columns[c] : "";
        keys[c] = xsqlc_strdup(base);
        if (!keys[c]) {
            goto oom;
        }
        for (size_t suffix = 2; jsonl_key_used(keys, c, keys[c]); ++suffix) {
            xsqlc_free(keys[c]);
            xsqlc_strbuf candidate;
            xsqlc_sb_init(&candidate);
            xsqlc_sb_append(&candidate, base);
            xsqlc_sb_putc(&candidate, '#');
            xsqlc_sb_append_size(&candidate, suffix);
            keys[c] = xsqlc_sb_detach(&candidate);
            if (!keys[c]) {
                goto oom;
            }
            if (suffix == (size_t)-1) {
                goto oom;
            }
        }
    }
    return keys;

oom:
    for (size_t i = 0; i < s->ncols; ++i) {
        xsqlc_free(keys[i]);
    }
    xsqlc_free(keys);
    return NULL;
}

static void jsonl_free_keys(char **keys, size_t count) {
    if (!keys) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        xsqlc_free(keys[i]);
    }
    xsqlc_free(keys);
}

static void append_jsonl_metadata(xsqlc_strbuf *sb,
                                  const struct xsql_script_statement *s) {
    if (!s->timed_out && !s->partial && s->warning_count == 0) {
        return;
    }
    xsqlc_sb_append(sb, "{\"statement_index\":");
    xsqlc_sb_append_size(sb, s->statement_index);
    if (s->timed_out) {
        xsqlc_sb_append(sb, ",\"timed_out\":true");
    }
    if (s->partial) {
        xsqlc_sb_append(sb, ",\"partial\":true");
    }
    if (s->warning_count > 0) {
        xsqlc_sb_append(sb, ",\"warnings\":[");
        for (size_t i = 0; i < s->warning_count; ++i) {
            if (i) {
                xsqlc_sb_putc(sb, ',');
            }
            const char *warning = s->warnings[i] ? s->warnings[i] : "";
            xsqlc_json_append_string(sb, warning, strlen(warning));
        }
        xsqlc_sb_putc(sb, ']');
    }
    xsqlc_sb_append(sb, "}\n");
}

char *xsql_script_result_to_jsonl(const xsql_script_result *r) {
    if (!r) {
        return NULL;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);

    if (r->parse_error && r->parse_error[0]) {
        xsqlc_sb_append(&sb, "{\"error\":");
        xsqlc_json_append_string(&sb, r->parse_error, strlen(r->parse_error));
        xsqlc_sb_append(&sb, "}\n");
        return xsqlc_sb_detach(&sb);
    }

    for (size_t i = 0; i < r->count; ++i) {
        const struct xsql_script_statement *s = &r->results[i];
        // A failed statement emits one error object line instead of records.
        if (!s->success) {
            xsqlc_sb_append(&sb, "{\"statement_index\":");
            xsqlc_sb_append_size(&sb, s->statement_index);
            xsqlc_sb_append(&sb, ",\"error\":");
            xsqlc_json_append_string(&sb, s->error ? s->error : "",
                                     s->error ? strlen(s->error) : 0);
            xsqlc_sb_append(&sb, "}\n");
            continue;
        }
        // A column-less statement (a successful write) emits nothing.
        char **keys = jsonl_unique_keys(s);
        if (s->ncols > 0 && !keys) {
            xsqlc_sb_free(&sb);
            return NULL;
        }
        for (size_t row = 0; row < s->nrows; ++row) {
            xsqlc_sb_putc(&sb, '{');
            for (size_t c = 0; c < s->ncols; ++c) {
                if (c) {
                    xsqlc_sb_putc(&sb, ',');
                }
                xsqlc_json_append_string(&sb, keys[c], strlen(keys[c]));
                xsqlc_sb_putc(&sb, ':');
                if (xsqlc_stmt_is_null_cell(s, row, c)) {
                    xsqlc_sb_append(&sb, "null");
                } else {
                    xsqlc_json_append_string(&sb, cell_ptr(s, row, c),
                                             cell_len(s, row, c));
                }
            }
            xsqlc_sb_append(&sb, "}\n");
        }
        jsonl_free_keys(keys, s->ncols);
        append_jsonl_metadata(&sb, s);
    }
    return xsqlc_sb_detach(&sb);
}
