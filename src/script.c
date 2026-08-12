// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// script.c — the multi-statement orchestrator, statement splitter, canonical
// envelope build/accessors, JSON round-trip inverse, SQL export, and identifier
// quoting (the JSON / text / delimited renderers live in format.c). Errors are
// reported through the envelope, never thrown.

#include <libxsqlc/script.h>

#include <libxsqlc/database.h>
#include <libxsqlc/result.h>
#include <libxsqlc/status.h>

#include "internal/internal.h"
#include "internal/json.h"
#include "internal/script_internal.h"
#include "internal/strbuf.h"

#include <sqlite3.h>

#include <ctype.h>
#include <stddef.h>
#include <string.h>

// ===========================================================================
// Statement storage: build helpers, is_null_cell, dispose
// ===========================================================================

bool xsqlc_stmt_is_null_cell(const struct xsql_script_statement *s, size_t row,
                             size_t col) {
    // Reports whether the cell at (row, col) is a SQL NULL.
    if (!s || row >= s->nrows || col >= s->ncols) {
        return false;
    }
    size_t i = row * s->ncols + col;
    if (s->cell_has_mask && s->cell_has_mask[i]) {
        return s->cell_null[i] != 0;
    }
    // Legacy fallback: a cell whose text is exactly "NULL" reads as SQL NULL.
    // Guard the backing store the same way xsql_script_statement_cell() does: a
    // statement with nrows/ncols but no cell array (or a NULL element) must read
    // as "not null" rather than dereference a NULL pointer.
    if (!s->cells || !s->cells[i]) {
        return false;
    }
    return strcmp(s->cells[i], "NULL") == 0;
}

void xsqlc_stmt_dispose(struct xsql_script_statement *s) {
    if (!s) {
        return;
    }
    if (s->columns) {
        for (size_t i = 0; i < s->ncols; ++i) {
            xsqlc_free(s->columns[i]);
        }
        xsqlc_free(s->columns);
    }
    if (s->cells) {
        size_t total = s->nrows * s->ncols;
        for (size_t i = 0; i < total; ++i) {
            xsqlc_free(s->cells[i]);
        }
        xsqlc_free(s->cells);
    }
    xsqlc_free(s->cell_len);
    xsqlc_free(s->cell_null);
    xsqlc_free(s->cell_has_mask);
    xsqlc_free(s->error);
    xsqlc_free(s->sql);
    if (s->warnings) {
        for (size_t i = 0; i < s->warning_count; ++i) {
            xsqlc_free(s->warnings[i]);
        }
        xsqlc_free(s->warnings);
    }
    memset(s, 0, sizeof(*s));
}

// Grow the flat cell arrays to hold one more row. Returns false on OOM.
static bool stmt_reserve_row(struct xsql_script_statement *s) {
    if (s->nrows < s->rows_cap) {
        return true;
    }
    size_t new_cap = s->rows_cap ? s->rows_cap * 2 : 8;
    if (s->ncols == 0) {
        s->rows_cap = new_cap;  // row counter still advances; no per-cell storage
        return true;
    }
    // n = new_cap * ncols cells; guard the product and each per-array byte
    // multiply so a huge cap/ncols cannot under-allocate.
    size_t n;
    if (!xsqlc_mul_ok(new_cap, s->ncols, &n)) {
        return false;
    }
    size_t cell_bytes, len_bytes;
    if (!xsqlc_mul_ok(n, sizeof(char *), &cell_bytes) ||
        !xsqlc_mul_ok(n, sizeof(size_t), &len_bytes)) {
        return false;
    }

    char **cells = (char **)xsqlc_realloc(s->cells, cell_bytes);
    if (!cells) {
        return false;
    }
    s->cells = cells;
    size_t *lens = (size_t *)xsqlc_realloc(s->cell_len, len_bytes);
    if (!lens) {
        return false;
    }
    s->cell_len = lens;
    unsigned char *nul = (unsigned char *)xsqlc_realloc(s->cell_null, n);
    if (!nul) {
        return false;
    }
    s->cell_null = nul;
    unsigned char *hm = (unsigned char *)xsqlc_realloc(s->cell_has_mask, n);
    if (!hm) {
        return false;
    }
    s->cell_has_mask = hm;

    s->rows_cap = new_cap;
    return true;
}

// Append one row from explicit byte-length cells (embedded-NUL safe). The mask
// covers the first `nnulls` cells; cells past it fall back to the "NULL" sentinel.
// `lens` may be NULL (each cell measured with strlen). Internal — used by the
// public C-string add_row and the database/JSON builders.
static int stmt_add_row_bytes(struct xsql_script_statement *s,
                              const char *const *cells, const size_t *lens,
                              const unsigned char *nulls, size_t ncells,
                              size_t nnulls) {
    if (!s) {
        return XSQL_MISUSE;
    }
    if (!stmt_reserve_row(s)) {
        return XSQL_NO_MEMORY;
    }
    size_t base = s->nrows * s->ncols;
    for (size_t c = 0; c < s->ncols; ++c) {
        const char *txt = (c < ncells && cells && cells[c]) ? cells[c] : "";
        size_t len = (lens && c < ncells) ? lens[c] : strlen(txt);
        char *dup = (char *)xsqlc_malloc(len + 1);
        if (!dup) {
            for (size_t k = 0; k < c; ++k) {
                xsqlc_free(s->cells[base + k]);
            }
            return XSQL_NO_MEMORY;
        }
        if (len > 0) {
            memcpy(dup, txt, len);
        }
        dup[len] = '\0';
        s->cells[base + c] = dup;
        s->cell_len[base + c] = len;
        if (nulls && c < nnulls) {
            s->cell_has_mask[base + c] = 1;
            s->cell_null[base + c] = (unsigned char)(nulls[c] ? 1 : 0);
        } else {
            s->cell_has_mask[base + c] = 0;
            s->cell_null[base + c] = 0;
        }
    }
    s->nrows++;
    return XSQL_OK;
}

// ===========================================================================
// Executor-side setters (xsql_stmt_out_*)
// ===========================================================================

void xsql_stmt_out_set_success(struct xsql_script_statement *out, bool success) {
    if (out) {
        out->success = success;
    }
}

void xsql_stmt_out_set_error(struct xsql_script_statement *out, const char *error) {
    if (out) {
        xsqlc_str_set(&out->error, error);
    }
}

void xsql_stmt_out_set_elapsed_ms(struct xsql_script_statement *out, double ms) {
    if (out) {
        out->elapsed_ms = ms;
    }
}

void xsql_stmt_out_set_timed_out(struct xsql_script_statement *out,
                                 bool timed_out) {
    if (out) {
        out->timed_out = timed_out;
    }
}

void xsql_stmt_out_set_partial(struct xsql_script_statement *out, bool partial) {
    if (out) {
        out->partial = partial;
    }
}

int xsql_stmt_out_add_warning(struct xsql_script_statement *out,
                              const char *warning) {
    if (!out) {
        return XSQL_MISUSE;
    }
    if (out->warning_count == (size_t)-1) {
        return XSQL_NO_MEMORY;
    }
    size_t bytes = 0;
    if (!xsqlc_mul_ok(out->warning_count + 1, sizeof(*out->warnings), &bytes)) {
        return XSQL_NO_MEMORY;
    }
    char **grown = (char **)xsqlc_realloc(out->warnings, bytes);
    if (!grown) {
        return XSQL_NO_MEMORY;
    }
    out->warnings = grown;
    char *copy = xsqlc_strdup(warning ? warning : "");
    if (!copy) {
        return XSQL_NO_MEMORY;
    }
    out->warnings[out->warning_count++] = copy;
    return XSQL_OK;
}

int xsql_stmt_out_add_column(struct xsql_script_statement *out, const char *name) {
    if (!out) {
        return XSQL_MISUSE;
    }
    char **grown =
        (char **)xsqlc_realloc(out->columns, (out->ncols + 1) * sizeof(*grown));
    if (!grown) {
        return XSQL_NO_MEMORY;
    }
    out->columns = grown;
    char *dup = xsqlc_strdup(name ? name : "");
    if (!dup) {
        return XSQL_NO_MEMORY;
    }
    out->columns[out->ncols++] = dup;
    return XSQL_OK;
}

int xsql_stmt_out_add_row(struct xsql_script_statement *out,
                          const char *const *cells, size_t ncells,
                          const unsigned char *nulls, size_t nnulls) {
    return stmt_add_row_bytes(out, cells, NULL, nulls, ncells, nnulls);
}

// ===========================================================================
// Statement splitter
// ===========================================================================

// Trim leading/trailing whitespace and, if non-empty, push a copy onto the
// growable array. Returns false only on allocation failure.
static bool split_push_trimmed(char ***arr, size_t *n, size_t *cap,
                               const char *data, size_t len) {
    size_t start = 0;
    while (start < len && isspace((unsigned char)data[start])) {
        ++start;
    }
    size_t end = len;
    while (end > start && isspace((unsigned char)data[end - 1])) {
        --end;
    }
    if (end == start) {
        return true;  // empty after trim; nothing to push
    }
    if (*n == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 8;
        char **grown = (char **)xsqlc_realloc(*arr, new_cap * sizeof(*grown));
        if (!grown) {
            return false;
        }
        *arr = grown;
        *cap = new_cap;
    }
    size_t sub = end - start;
    char *copy = (char *)xsqlc_malloc(sub + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, data + start, sub);
    copy[sub] = '\0';
    (*arr)[(*n)++] = copy;
    return true;
}

int xsql_script_split(const char *script, char ***out, size_t *count) {
    if (!out || !count) {
        return XSQL_MISUSE;
    }
    *out = NULL;
    *count = 0;
    if (!script) {
        return XSQL_OK;
    }

    char **stmts = NULL;
    size_t n = 0, cap = 0;
    xsqlc_strbuf cur;
    xsqlc_sb_init(&cur);
    int rc = XSQL_OK;

    for (const char *p = script; *p; ++p) {
        xsqlc_sb_putc(&cur, *p);
        if (xsqlc_sb_oom(&cur)) {
            rc = XSQL_NO_MEMORY;
            goto done;
        }
        if (sqlite3_complete(cur.data)) {
            if (!split_push_trimmed(&stmts, &n, &cap, cur.data, cur.len)) {
                rc = XSQL_NO_MEMORY;
                goto done;
            }
            xsqlc_sb_free(&cur);
            xsqlc_sb_init(&cur);
        }
    }
    // Trailing statement without a terminating ';'.
    if (cur.data && !split_push_trimmed(&stmts, &n, &cap, cur.data, cur.len)) {
        rc = XSQL_NO_MEMORY;
        goto done;
    }

done:
    xsqlc_sb_free(&cur);
    if (rc != XSQL_OK) {
        xsql_free_strings(stmts, n);
        return rc;
    }
    *out = stmts;
    *count = n;
    return XSQL_OK;
}

void xsql_free_strings(char **strings, size_t count) {
    if (!strings) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        xsqlc_free(strings[i]);
    }
    xsqlc_free(strings);
}

// ===========================================================================
// Orchestrator
// ===========================================================================

static bool opt_continue_on_error(const xsql_script_opts *opts) {
    return opts && opts->continue_on_error;
}
static bool opt_include_sql(const xsql_script_opts *opts) {
    return opts && opts->include_sql;
}
static int opt_timeout_ms(const xsql_script_opts *opts) {
    // timeout_ms is an ABI-growth field: read only when struct_size covers it.
    if (opts && opts->struct_size >=
                    offsetof(xsql_script_opts, timeout_ms) + sizeof(opts->timeout_ms)) {
        return opts->timeout_ms;
    }
    return 0;
}
// should_cancel/_user are ABI-growth fields: read only when struct_size covers the
// pair (a smaller/zeroed opts leaves cooperative cancellation off).
static bool opts_have_cancel(const xsql_script_opts *opts) {
    return opts && opts->struct_size >=
                       offsetof(xsql_script_opts, should_cancel_user) +
                           sizeof(opts->should_cancel_user);
}
static int (*opt_should_cancel(const xsql_script_opts *opts))(void *) {
    return opts_have_cancel(opts) ? opts->should_cancel : NULL;
}
static void *opt_should_cancel_user(const xsql_script_opts *opts) {
    return opts_have_cancel(opts) ? opts->should_cancel_user : NULL;
}

static bool statement_reports_cancellation(
    const struct xsql_script_statement *statement) {
    if (!statement) return false;
    if (statement->error && strstr(statement->error, "cancelled")) {
        return true;
    }
    for (size_t i = 0; i < statement->warning_count; ++i) {
        if (statement->warnings[i] &&
            strstr(statement->warnings[i], "cancelled")) {
            return true;
        }
    }
    return false;
}

static int run_script_core(const char *script, xsql_script_executor_fn exec,
                           void *user, const xsql_script_opts *opts,
                           xsql_script_result **out) {
    if (!out) {
        return XSQL_MISUSE;
    }
    *out = NULL;

    xsql_script_result *r =
        (xsql_script_result *)xsqlc_malloc(sizeof(*r));
    if (!r) {
        return XSQL_NO_MEMORY;
    }
    memset(r, 0, sizeof(*r));

    char **stmts = NULL;
    size_t nstmts = 0;
    int split_rc = xsql_script_split(script, &stmts, &nstmts);
    if (split_rc != XSQL_OK) {
        // The splitter does not parse — its only non-OK outcome is allocation
        // pressure (XSQL_NO_MEMORY). Propagate OOM distinctly (no misleading
        // "parse error" envelope) so callers can tell it apart from real SQL
        // errors, matching the port's propagate-OOM convention.
        xsqlc_free(r);
        return split_rc == XSQL_NO_MEMORY ? XSQL_NO_MEMORY : split_rc;
    }

    r->statement_count = nstmts;
    if (nstmts > 0) {
        r->results = (struct xsql_script_statement *)xsqlc_malloc(
            nstmts * sizeof(*r->results));
        if (!r->results) {
            xsql_free_strings(stmts, nstmts);
            xsqlc_free(r);
            return XSQL_NO_MEMORY;
        }
        memset(r->results, 0, nstmts * sizeof(*r->results));
        r->results_cap = nstmts;
    }

    int (*should_cancel)(void *) = opt_should_cancel(opts);
    void *should_cancel_user = opt_should_cancel_user(opts);

    bool stop = false;
    bool cancellation_latched = false;
    for (size_t i = 0; i < nstmts; ++i) {
        if (stop) {
            break;
        }
        // Cooperative cancellation between statements (e.g. an HTTP POST /cancel):
        // don't start a new statement once cancel is requested. Mid-statement
        // cancellation is the executor's job (the database executor threads
        // should_cancel into xsql_db_query_opts). Mirrors C++ run_script.
        if (cancellation_latched ||
            (should_cancel && should_cancel(should_cancel_user))) {
            cancellation_latched = true;
            struct xsql_script_statement *st = &r->results[r->count];
            st->statement_index = i;
            st->success = false;
            st->error = xsqlc_strdup("query cancelled");
            if (!st->error) {
                xsql_free_strings(stmts, nstmts);
                xsql_script_result_free(r);
                return XSQL_NO_MEMORY;
            }
            if (opt_include_sql(opts)) {
                st->sql = xsqlc_strdup(stmts[i]);
                if (!st->sql) {
                    xsqlc_stmt_dispose(st);
                    xsql_free_strings(stmts, nstmts);
                    xsql_script_result_free(r);
                    return XSQL_NO_MEMORY;
                }
            }
            r->has_first_error = true;
            r->first_error_index = i;
            r->count++;
            break;
        }

        struct xsql_script_statement *st = &r->results[r->count];
        st->statement_index = i;
        st->success = false;  // executor must opt in to success

        exec(stmts[i], st, user);
        if (statement_reports_cancellation(st)) {
            cancellation_latched = true;
        }

        if (opt_include_sql(opts)) {
            xsqlc_str_set(&st->sql, stmts[i]);
        }

        if (!st->success) {
            if (!r->has_first_error) {
                r->has_first_error = true;
                r->first_error_index = i;
            }
            if (!opt_continue_on_error(opts)) {
                stop = true;
            }
        }

        r->row_count_total += st->nrows;
        r->elapsed_ms_total += st->elapsed_ms;
        r->count++;
    }

    r->success = !r->has_first_error;
    xsql_free_strings(stmts, nstmts);
    *out = r;
    return r->success ? XSQL_OK : XSQL_ERROR;
}

int xsql_run_script(const char *script, xsql_script_executor_fn exec, void *user,
                    const xsql_script_opts *opts, xsql_script_result **out) {
    if (!exec) {
        return XSQL_MISUSE;
    }
    return run_script_core(script, exec, user, opts, out);
}

// --- Database-backed executor ----------------------------------------------

// Context threaded through the database executor: the connection plus the
// per-statement query deadline and cooperative-cancel predicate read from the
// script options.
struct db_script_ctx {
    struct xsql_db *db;
    int             timeout_ms;
    int           (*should_cancel)(void *user);
    void           *should_cancel_user;
};

static void db_script_exec(const char *sql, struct xsql_script_statement *out,
                           void *user) {
    struct db_script_ctx *ctx = (struct db_script_ctx *)user;
    xsql_result *res = NULL;
    xsql_query_opts qo = {0};
    qo.struct_size = sizeof(qo);
    qo.timeout_ms = ctx->timeout_ms;
    // Thread cooperative cancellation into the query so an in-flight statement
    // stops ASAP (partial rows only for read-only statements).
    qo.should_cancel = ctx->should_cancel;
    qo.should_cancel_user = ctx->should_cancel_user;
    xsql_db_query_opts(ctx->db, sql, &qo, &res);
    if (!res) {
        out->success = false;
        xsqlc_str_set(&out->error, "out of memory");
        return;
    }

    bool ok = xsql_result_ok(res);
    size_t nc = xsql_result_column_count(res);
    bool build_ok = true;

    for (size_t c = 0; c < nc && build_ok; ++c) {
        if (xsql_stmt_out_add_column(out, xsql_result_column_name(res, c)) !=
            XSQL_OK) {
            build_ok = false;
        }
    }

    size_t nr = xsql_result_row_count(res);
    if (build_ok && nc > 0) {
        const char **cells =
            (const char **)xsqlc_malloc(nc * sizeof(*cells));
        size_t *lens = (size_t *)xsqlc_malloc(nc * sizeof(*lens));
        unsigned char *nulls = (unsigned char *)xsqlc_malloc(nc);
        if (!cells || !lens || !nulls) {
            build_ok = false;
        } else {
            for (size_t row = 0; row < nr && build_ok; ++row) {
                for (size_t c = 0; c < nc; ++c) {
                    cells[c] = xsql_result_cell(res, row, c);
                    lens[c] = (size_t)xsql_result_cell_bytes(res, row, c);
                    nulls[c] =
                        (unsigned char)(xsql_result_cell_is_null(res, row, c) ? 1
                                                                              : 0);
                }
                if (stmt_add_row_bytes(out, cells, lens, nulls, nc, nc) !=
                    XSQL_OK) {
                    build_ok = false;
                }
            }
        }
        xsqlc_free(cells);
        xsqlc_free(lens);
        xsqlc_free(nulls);
    }

    out->elapsed_ms = (double)xsql_result_elapsed_ms(res);
    if (!build_ok) {
        out->success = false;
        xsqlc_str_set(&out->error, "out of memory");
    } else {
        out->success = ok;
        if (!ok) {
            xsqlc_str_set(&out->error, xsql_result_errmsg(res));
        }
        // Round-trip the timeout / partial-result signalling (mirrors C++/Rust):
        // a partial timeout is a success with partial rows + a warning.
        xsql_stmt_out_set_timed_out(out, xsql_result_timed_out(res));
        xsql_stmt_out_set_partial(out, xsql_result_partial(res));
        size_t wc = xsql_result_warning_count(res);
        for (size_t i = 0; i < wc; ++i) {
            if (xsql_stmt_out_add_warning(out, xsql_result_warning(res, i)) !=
                XSQL_OK) {
                out->success = false;
                xsqlc_str_set(&out->error, "out of memory");
                break;
            }
        }
    }
    xsql_result_free(res);
}

int xsql_db_run_script(struct xsql_db *db, const char *script,
                       const xsql_script_opts *opts, xsql_script_result **out) {
    struct db_script_ctx ctx;
    ctx.db = db;
    ctx.timeout_ms = opt_timeout_ms(opts);
    ctx.should_cancel = opt_should_cancel(opts);
    ctx.should_cancel_user = opt_should_cancel_user(opts);
    return run_script_core(script, db_script_exec, &ctx, opts, out);
}

int xsql_execute_script(struct xsql_db *db, const char *script,
                        const xsql_script_opts *opts, xsql_script_result **out) {
    return xsql_db_run_script(db, script, opts, out);
}

// ===========================================================================
// Envelope accessors
// ===========================================================================

bool xsql_script_result_ok(const xsql_script_result *r) {
    return r ? r->success : false;
}
size_t xsql_script_result_statement_count(const xsql_script_result *r) {
    return r ? r->statement_count : 0;
}
size_t xsql_script_result_count(const xsql_script_result *r) {
    return r ? r->count : 0;
}
size_t xsql_script_result_row_count_total(const xsql_script_result *r) {
    return r ? r->row_count_total : 0;
}
double xsql_script_result_elapsed_ms_total(const xsql_script_result *r) {
    return r ? r->elapsed_ms_total : 0.0;
}
bool xsql_script_result_has_first_error(const xsql_script_result *r) {
    return r ? r->has_first_error : false;
}
size_t xsql_script_result_first_error_index(const xsql_script_result *r) {
    return r ? r->first_error_index : 0;
}
const char *xsql_script_result_parse_error(const xsql_script_result *r) {
    if (!r || !r->parse_error) {
        return "";
    }
    return r->parse_error;
}

const xsql_script_statement *
xsql_script_result_statement(const xsql_script_result *r, size_t i) {
    if (!r || i >= r->count) {
        return NULL;
    }
    return &r->results[i];
}

size_t xsql_script_statement_index(const xsql_script_statement *s) {
    return s ? s->statement_index : 0;
}
bool xsql_script_statement_ok(const xsql_script_statement *s) {
    return s ? s->success : false;
}
const char *xsql_script_statement_error(const xsql_script_statement *s) {
    if (!s || !s->error) {
        return "";
    }
    return s->error;
}
const char *xsql_script_statement_sql(const xsql_script_statement *s) {
    if (!s || !s->sql) {
        return "";
    }
    return s->sql;
}
double xsql_script_statement_elapsed_ms(const xsql_script_statement *s) {
    return s ? s->elapsed_ms : 0.0;
}
size_t xsql_script_statement_row_count(const xsql_script_statement *s) {
    return s ? s->nrows : 0;
}
bool xsql_script_statement_timed_out(const xsql_script_statement *s) {
    return s ? s->timed_out : false;
}
bool xsql_script_statement_partial(const xsql_script_statement *s) {
    return s ? s->partial : false;
}
size_t xsql_script_statement_warning_count(const xsql_script_statement *s) {
    return s ? s->warning_count : 0;
}
const char *xsql_script_statement_warning(const xsql_script_statement *s, size_t i) {
    if (!s || i >= s->warning_count || !s->warnings) {
        return NULL;
    }
    return s->warnings[i];
}
size_t xsql_script_statement_column_count(const xsql_script_statement *s) {
    return s ? s->ncols : 0;
}
const char *xsql_script_statement_column_name(const xsql_script_statement *s,
                                              size_t col) {
    if (!s || col >= s->ncols) {
        return NULL;
    }
    return s->columns[col];
}
const char *xsql_script_statement_cell(const xsql_script_statement *s, size_t row,
                                       size_t col) {
    if (!s || row >= s->nrows || col >= s->ncols || !s->cells) {
        return NULL;
    }
    return s->cells[row * s->ncols + col];
}
bool xsql_script_statement_cell_is_null(const xsql_script_statement *s, size_t row,
                                        size_t col) {
    return xsqlc_stmt_is_null_cell(s, row, col);
}

void xsql_script_result_free(xsql_script_result *r) {
    if (!r) {
        return;
    }
    if (r->results) {
        for (size_t i = 0; i < r->count; ++i) {
            xsqlc_stmt_dispose(&r->results[i]);
        }
        xsqlc_free(r->results);
    }
    xsqlc_free(r->parse_error);
    xsqlc_free(r);
}

// ===========================================================================
// JSON inverse (xsql_script_result_from_json)
// ===========================================================================

// Stringify one JSON cell value. Sets *len (byte length) and *is_null. Returns a
// heap copy (xsqlc); NULL on OOM.
static char *json_cell_text(const xsqlc_jval *cell, size_t *len,
                            unsigned char *is_null) {
    *is_null = 0;
    *len = 0;
    if (!cell || cell->type == XSQLC_JNULL) {
        *is_null = 1;
        return xsqlc_strdup("");
    }
    if (cell->type == XSQLC_JSTR) {
        *len = cell->strlen;
        char *d = (char *)xsqlc_malloc(cell->strlen + 1);
        if (d) {
            if (cell->strlen > 0) {
                memcpy(d, cell->str, cell->strlen);
            }
            d[cell->strlen] = '\0';
        }
        return d;
    }
    if (cell->type == XSQLC_JBOOL) {
        const char *s = cell->bval ? "true" : "false";
        *len = strlen(s);
        return xsqlc_strdup(s);
    }
    if (cell->type == XSQLC_JNUM) {
        xsqlc_strbuf sb;
        xsqlc_sb_init(&sb);
        xsqlc_json_append_number(&sb, cell->num);
        *len = sb.len;
        return xsqlc_sb_detach(&sb);
    }
    return xsqlc_strdup("");  // arrays/objects: not expected in a cell
}

static bool build_statement_from_json(struct xsql_script_statement *st,
                                      const xsqlc_jval *js) {
    st->statement_index = xsqlc_json_size(xsqlc_json_member(js, "statement_index"), 0);
    st->success = xsqlc_json_bool(xsqlc_json_member(js, "success"), false);
    st->elapsed_ms = xsqlc_json_num(xsqlc_json_member(js, "elapsed_ms"), 0.0);
    st->timed_out = xsqlc_json_bool(xsqlc_json_member(js, "timed_out"), false);
    st->partial = xsqlc_json_bool(xsqlc_json_member(js, "partial"), false);

    const xsqlc_jval *err = xsqlc_json_member(js, "error");
    if (xsqlc_json_is(err, XSQLC_JSTR)) {
        st->error = xsqlc_strdup(xsqlc_json_str(err));
        if (!st->error) {
            return false;
        }
    }
    const xsqlc_jval *sql = xsqlc_json_member(js, "sql");
    if (xsqlc_json_is(sql, XSQLC_JSTR)) {
        st->sql = xsqlc_strdup(xsqlc_json_str(sql));
        if (!st->sql) {
            return false;
        }
    }
    const xsqlc_jval *warnings = xsqlc_json_member(js, "warnings");
    if (xsqlc_json_is(warnings, XSQLC_JARR)) {
        for (size_t i = 0; i < warnings->nitems; ++i) {
            const xsqlc_jval *warning = warnings->items[i];
            if (!xsqlc_json_is(warning, XSQLC_JSTR)) {
                continue;
            }
            if (xsql_stmt_out_add_warning(st, xsqlc_json_str(warning)) !=
                XSQL_OK) {
                return false;
            }
        }
    }

    const xsqlc_jval *cols = xsqlc_json_member(js, "columns");
    if (xsqlc_json_is(cols, XSQLC_JARR)) {
        for (size_t i = 0; i < cols->nitems; ++i) {
            const xsqlc_jval *c = cols->items[i];
            const char *name = xsqlc_json_is(c, XSQLC_JSTR) ? xsqlc_json_str(c) : "";
            if (xsql_stmt_out_add_column(st, name) != XSQL_OK) {
                return false;
            }
        }
    }

    const xsqlc_jval *rows = xsqlc_json_member(js, "rows");
    if (xsqlc_json_is(rows, XSQLC_JARR)) {
        for (size_t ri = 0; ri < rows->nitems; ++ri) {
            const xsqlc_jval *jr = rows->items[ri];
            size_t ncells = xsqlc_json_is(jr, XSQLC_JARR) ? jr->nitems : 0;
            const char **cells =
                (const char **)xsqlc_malloc((ncells ? ncells : 1) * sizeof(*cells));
            size_t *lens = (size_t *)xsqlc_malloc((ncells ? ncells : 1) * sizeof(*lens));
            unsigned char *nulls =
                (unsigned char *)xsqlc_malloc(ncells ? ncells : 1);
            char **owned =
                (char **)xsqlc_malloc((ncells ? ncells : 1) * sizeof(*owned));
            if (!cells || !lens || !nulls || !owned) {
                xsqlc_free(cells);
                xsqlc_free(lens);
                xsqlc_free(nulls);
                xsqlc_free(owned);
                return false;
            }
            bool cell_ok = true;
            size_t built = 0;
            for (; built < ncells; ++built) {
                size_t len = 0;
                unsigned char is_null = 0;
                char *txt = json_cell_text(jr->items[built], &len, &is_null);
                if (!txt) {
                    cell_ok = false;
                    break;
                }
                owned[built] = txt;
                cells[built] = txt;
                lens[built] = len;
                nulls[built] = is_null;
            }
            if (cell_ok) {
                if (stmt_add_row_bytes(st, cells, lens, nulls, ncells, ncells) !=
                    XSQL_OK) {
                    cell_ok = false;
                }
            }
            for (size_t k = 0; k < built; ++k) {
                xsqlc_free(owned[k]);
            }
            xsqlc_free(cells);
            xsqlc_free(lens);
            xsqlc_free(nulls);
            xsqlc_free(owned);
            if (!cell_ok) {
                return false;
            }
        }
    }
    return true;
}

int xsql_script_result_from_json(const char *json, xsql_script_result **out) {
    if (!out) {
        return XSQL_MISUSE;
    }
    *out = NULL;

    xsql_script_result *r = (xsql_script_result *)xsqlc_malloc(sizeof(*r));
    if (!r) {
        return XSQL_NO_MEMORY;
    }
    memset(r, 0, sizeof(*r));

    xsqlc_jval *j = xsqlc_json_parse(json);
    if (!j || j->type != XSQLC_JOBJ) {
        xsqlc_json_free(j);
        r->success = false;
        r->parse_error =
            xsqlc_strdup("json_to_script_result: could not parse result JSON");
        *out = r;
        return XSQL_OK;
    }

    r->success = xsqlc_json_bool(xsqlc_json_member(j, "success"), false);
    r->statement_count = xsqlc_json_size(xsqlc_json_member(j, "statement_count"), 0);
    r->row_count_total = xsqlc_json_size(xsqlc_json_member(j, "row_count_total"), 0);
    r->elapsed_ms_total = xsqlc_json_num(xsqlc_json_member(j, "elapsed_ms_total"), 0.0);

    const xsqlc_jval *perr = xsqlc_json_member(j, "parse_error");
    if (xsqlc_json_is(perr, XSQLC_JSTR)) {
        r->parse_error = xsqlc_strdup(xsqlc_json_str(perr));
    }
    // A request-level failure envelope (top-level string "error", no results/
    // parse_error) is surfaced as parse_error so the text/csv/tsv formatters emit
    // the message. An explicit parse_error wins; a success envelope ignores it.
    if ((!r->parse_error || r->parse_error[0] == '\0') && !r->success) {
        const xsqlc_jval *terr = xsqlc_json_member(j, "error");
        if (xsqlc_json_is(terr, XSQLC_JSTR)) {
            xsqlc_str_set(&r->parse_error, xsqlc_json_str(terr));
        }
    }

    const xsqlc_jval *fei = xsqlc_json_member(j, "first_error_index");
    if (xsqlc_json_is(fei, XSQLC_JNUM) && fei->num >= 0) {
        r->has_first_error = true;
        r->first_error_index = (size_t)fei->num;
    }

    const xsqlc_jval *results = xsqlc_json_member(j, "results");
    if (xsqlc_json_is(results, XSQLC_JARR) && results->nitems > 0) {
        r->results = (struct xsql_script_statement *)xsqlc_malloc(
            results->nitems * sizeof(*r->results));
        if (!r->results) {
            xsqlc_json_free(j);
            xsql_script_result_free(r);
            return XSQL_NO_MEMORY;
        }
        memset(r->results, 0, results->nitems * sizeof(*r->results));
        r->results_cap = results->nitems;
        for (size_t i = 0; i < results->nitems; ++i) {
            const xsqlc_jval *js = results->items[i];
            if (!xsqlc_json_is(js, XSQLC_JOBJ)) {
                continue;
            }
            struct xsql_script_statement *st = &r->results[r->count];
            if (!build_statement_from_json(st, js)) {
                xsqlc_json_free(j);
                xsql_script_result_free(r);
                return XSQL_NO_MEMORY;
            }
            r->count++;
        }
    }

    xsqlc_json_free(j);
    *out = r;
    return XSQL_OK;
}

// ===========================================================================
// Identifier quoting  (SQL export lives in export.c)
// ===========================================================================

char *xsql_quote_identifier(const char *name) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_putc(&sb, '"');
    if (name) {
        for (const char *p = name; *p; ++p) {
            if (*p == '"') {
                xsqlc_sb_putc(&sb, '"');
            }
            xsqlc_sb_putc(&sb, *p);
        }
    }
    xsqlc_sb_putc(&sb, '"');
    return xsqlc_sb_detach(&sb);
}
