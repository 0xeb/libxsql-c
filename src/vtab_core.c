// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// vtab_core.c — the shared virtual-table scaffolding: the thread-local error
// channel and cooperative-cancellation hooks (public xsql_vtab_* surface), plus
// the internal identifier validation and CREATE VIRTUAL TABLE helper reused by
// every table module. Shared virtual-table support includes set/get/clear/
// return_vtab_error, the interrupt checker, is_valid_sql_identifier,
// create_vtable_sqlite).

#include <libxsqlc/vtab.h>

#include <libxsqlc/status.h>

#include "internal/thread_shim.h"
#include "internal/vtab_core.h"

#include <sqlite3.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Thread-local error channel. A fixed buffer keeps the channel allocation-free
// on the hot path (error messages are short; longer ones truncate cleanly).
// ---------------------------------------------------------------------------

static XSQL_THREAD_LOCAL char g_vtab_error[512];
static XSQL_THREAD_LOCAL bool g_vtab_error_set;

void xsql_vtab_set_error(const char *fmt, ...) {
    if (!fmt) {
        xsqlc_vtab_clear_error();
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_vtab_error, sizeof(g_vtab_error), fmt, ap);
    va_end(ap);
    if (n < 0) {
        g_vtab_error[0] = '\0';
    }
    g_vtab_error_set = true;
}

void xsqlc_vtab_clear_error(void) {
    g_vtab_error[0] = '\0';
    g_vtab_error_set = false;
}

const char *xsqlc_vtab_get_error(void) {
    return g_vtab_error_set ? g_vtab_error : "";
}

bool xsqlc_vtab_has_error(void) {
    return g_vtab_error_set && g_vtab_error[0] != '\0';
}

int xsqlc_vtab_return_error(struct sqlite3_vtab *pVtab) {
    if (xsqlc_vtab_has_error() && pVtab) {
        // SQLite frees zErrMsg only when it consumes it; free a message an
        // earlier assignment left so a second one does not leak it.
        sqlite3_free(pVtab->zErrMsg);
        pVtab->zErrMsg = sqlite3_mprintf("%s", g_vtab_error);
    }
    xsqlc_vtab_clear_error();
    return SQLITE_ERROR;
}

int xsqlc_vtab_return_refusal(struct sqlite3_vtab *pVtab) {
    xsqlc_vtab_return_error(pVtab);
    return SQLITE_CONSTRAINT_VTAB;
}

// --- Prepare-time authorizer denial channel (thread-local) -------------------

static XSQL_THREAD_LOCAL char g_authz_denial[512];
static XSQL_THREAD_LOCAL bool g_authz_denial_set;

void xsqlc_set_authz_denial(const char *msg) {
    if (!msg) {
        xsqlc_clear_authz_denial();
        return;
    }
    // Truncates cleanly into the fixed buffer (messages are short).
    size_t n = 0;
    for (; msg[n] && n + 1 < sizeof(g_authz_denial); ++n) {
        g_authz_denial[n] = msg[n];
    }
    g_authz_denial[n] = '\0';
    g_authz_denial_set = true;
}

void xsqlc_clear_authz_denial(void) {
    g_authz_denial[0] = '\0';
    g_authz_denial_set = false;
}

const char *xsqlc_authz_denial(void) {
    return g_authz_denial_set ? g_authz_denial : "";
}

bool xsqlc_authz_denial_set(void) {
    return g_authz_denial_set && g_authz_denial[0] != '\0';
}

int xsqlc_unsupported_write(struct sqlite3_vtab *pVtab, const char *table, char op) {
    const char *t = table ? table : "?";
    switch (op) {
        case 'I':
            xsql_vtab_set_error(
                "INSERT INTO %s is not supported (capability mutation.%s.insert is unavailable)",
                t, t);
            break;
        case 'D':
            xsql_vtab_set_error(
                "DELETE FROM %s is not supported (capability mutation.%s.delete is unavailable)",
                t, t);
            break;
        case 'U':
        default:
            xsql_vtab_set_error(
                "UPDATE %s is not supported (capability mutation.%s.* is unavailable)",
                t, t);
            break;
    }
    return xsqlc_vtab_return_error(pVtab);
}

// ---------------------------------------------------------------------------
// Shared UPDATE column-apply (read-only rejection + partial-failure surfacing).
// See xsqlc_apply_update_columns in internal/vtab_core.h. Allocation-free: the
// applied-column list uses a fixed buffer that truncates cleanly (the final error
// goes through xsql_vtab_set_error's 512-byte channel, which also truncates).
// ---------------------------------------------------------------------------

bool xsqlc_apply_update_columns(int argc, sqlite3_value **argv, size_t ncols,
                                bool nochange_eligible,
                                const xsqlc_update_ops *ops) {
    bool has_writable = false;
    for (size_t c = 0; c < ncols; ++c) {
        if (ops->writable(ops->ctx, c)) {
            has_writable = true;
            break;
        }
    }

    // Reject a genuine write to a read-only column before mutating anything.
    // Skipped on reconstruct-by-argv paths (!nochange_eligible), where unchanged
    // read-only identity columns arrive as real values rather than NOCHANGE.
    if (nochange_eligible && has_writable) {
        for (int i = 2; i < argc && (size_t)(i - 2) < ncols; ++i) {
            if (sqlite3_value_nochange(argv[i])) {
                continue;
            }
            size_t c = (size_t)(i - 2);
            if (!ops->writable(ops->ctx, c)) {
                xsql_vtab_set_error("column \"%s\" is read-only",
                                    ops->name(ops->ctx, c));
                return false;
            }
        }
    }

    // Apply writable setters in order, tracking which applied so a later failure
    // can name the columns that were already committed.
    char applied[256];
    applied[0] = '\0';
    size_t applied_len = 0;
    for (int i = 2; i < argc && (size_t)(i - 2) < ncols; ++i) {
        if (sqlite3_value_nochange(argv[i])) {
            continue;
        }
        size_t c = (size_t)(i - 2);
        if (!ops->writable(ops->ctx, c)) {
            continue;  // read-only on a non-eligible path: skip (historical behavior)
        }
        xsqlc_vtab_clear_error();
        if (!ops->apply(ops->ctx, c, argv[i])) {
            char setter_err[512];
            const char *e = xsqlc_vtab_get_error();
            if (e && e[0]) {
                snprintf(setter_err, sizeof(setter_err), "%s", e);
            } else {
                snprintf(setter_err, sizeof(setter_err),
                         "UPDATE setter failed for column \"%s\"",
                         ops->name(ops->ctx, c));
            }
            xsqlc_vtab_clear_error();
            if (applied[0]) {
                xsql_vtab_set_error(
                    "%s (partial UPDATE: column(s) [%s] were already applied and "
                    "cannot be rolled back)",
                    setter_err, applied);
            } else {
                xsql_vtab_set_error("%s", setter_err);
            }
            return false;
        }
        const char *nm = ops->name(ops->ctx, c);
        int w = snprintf(applied + applied_len, sizeof(applied) - applied_len,
                         "%s%s", applied_len ? ", " : "", nm ? nm : "?");
        if (w > 0 && (size_t)w < sizeof(applied) - applied_len) {
            applied_len += (size_t)w;
        } else {
            applied_len = sizeof(applied) - 1;  // truncated; stop growing
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Cooperative cancellation
// ---------------------------------------------------------------------------

static XSQL_THREAD_LOCAL xsql_interrupt_fn g_interrupt_fn;
static XSQL_THREAD_LOCAL void             *g_interrupt_user;
static XSQL_THREAD_LOCAL xsqlc_progress_guard *g_progress_guards;

void xsql_vtab_set_interrupt_checker(xsql_interrupt_fn fn, void *user) {
    g_interrupt_fn = fn;
    g_interrupt_user = user;
}

void xsql_vtab_clear_interrupt_checker(void) {
    g_interrupt_fn = NULL;
    g_interrupt_user = NULL;
}

bool xsql_vtab_interrupted(void) {
    return g_interrupt_fn && g_interrupt_fn(g_interrupt_user);
}

void xsqlc_interrupt_guard_enter(
    xsqlc_interrupt_guard *guard, bool (*fn)(void *user), void *user) {
    if (!guard) return;
    guard->previous_fn = g_interrupt_fn;
    guard->previous_user = g_interrupt_user;
    guard->active = true;
    xsql_vtab_set_interrupt_checker(fn, user);
}

void xsqlc_interrupt_guard_leave(xsqlc_interrupt_guard *guard) {
    if (!guard || !guard->active) return;
    g_interrupt_fn = guard->previous_fn;
    g_interrupt_user = guard->previous_user;
    guard->active = false;
}

void xsqlc_progress_guard_enter(
    xsqlc_progress_guard *guard, sqlite3 *db, int steps,
    int (*callback)(void *user), void *user) {
    if (!guard) return;
    memset(guard, 0, sizeof(*guard));
    if (!db || steps <= 0 || !callback) return;
    guard->db = db;
    guard->steps = steps;
    guard->callback = callback;
    guard->user = user;
    guard->previous_tls = g_progress_guards;
    for (xsqlc_progress_guard *prior = g_progress_guards; prior;
         prior = prior->previous_tls) {
        if (prior->active && prior->db == db) {
            guard->previous_same_db = prior;
            break;
        }
    }
    guard->active = true;
    g_progress_guards = guard;
    sqlite3_progress_handler(db, steps, callback, user);
}

void xsqlc_progress_guard_leave(xsqlc_progress_guard *guard) {
    if (!guard || !guard->active) return;

    // Query scopes are LIFO. Unlink defensively so an early-error cleanup does
    // not leave a dangling stack pointer even if an embedder violates nesting.
    xsqlc_progress_guard **link = &g_progress_guards;
    while (*link && *link != guard) {
        link = &(*link)->previous_tls;
    }
    if (*link == guard) {
        *link = guard->previous_tls;
    }

    xsqlc_progress_guard *prior = guard->previous_same_db;
    if (prior && prior->active) {
        sqlite3_progress_handler(
            guard->db, prior->steps, prior->callback, prior->user);
    } else {
        sqlite3_progress_handler(guard->db, 0, NULL, NULL);
    }
    guard->active = false;
}

// ---------------------------------------------------------------------------
// Identifier validation + CREATE VIRTUAL TABLE
// ---------------------------------------------------------------------------

bool xsqlc_is_valid_sql_identifier(const char *name) {
    if (!name || !*name) {
        return false;
    }
    for (const char *p = name; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            return false;
        }
    }
    return true;
}

int xsqlc_create_vtable_sqlite(struct sqlite3 *db, const char *table_name,
                               const char *module_name) {
    if (!db || !xsqlc_is_valid_sql_identifier(table_name) ||
        !xsqlc_is_valid_sql_identifier(module_name)) {
        return XSQL_MISUSE;
    }
    // Both identifiers are validated to [A-Za-z0-9_]; safe to inline.
    char *sql = sqlite3_mprintf("CREATE VIRTUAL TABLE %s USING %s;", table_name,
                                module_name);
    if (!sql) {
        return XSQL_NO_MEMORY;
    }
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) {
        sqlite3_free(err);
    }
    sqlite3_free(sql);
    return rc;
}

bool xsqlc_resolve_cmp_value(sqlite3_value *v, xsqlc_cmp_value *out) {
    memset(out, 0, sizeof(*out));
    int type = sqlite3_value_type(v);
    if (type == SQLITE_TEXT) {
        // Numeric affinity on a private copy: applying it to argv itself would
        // change the type of the caller's value (e.g. an outer JOIN column).
        sqlite3_value *copy = sqlite3_value_dup(v);
        if (!copy) {
            return false;
        }
        type = sqlite3_value_numeric_type(copy);
        out->i = sqlite3_value_int64(copy);
        out->d = sqlite3_value_double(copy);
        sqlite3_value_free(copy);
    } else {
        out->i = sqlite3_value_int64(v);
        out->d = sqlite3_value_double(v);
    }
    out->kind = type == SQLITE_BLOB ? SQLITE_TEXT : type;
    if (out->kind == SQLITE_FLOAT && out->d != out->d) {
        out->kind = SQLITE_NULL;  // NaN compares like NULL
    }
    return true;
}

bool xsqlc_cmp_exact_key(const xsqlc_cmp_value *cv, int64_t *key) {
    if (cv->kind == SQLITE_INTEGER) {
        *key = cv->i;
        return true;
    }
    if (cv->kind == SQLITE_FLOAT && cv->d >= -9223372036854775808.0 &&
        cv->d < 9223372036854775808.0 && (double)(int64_t)cv->d == cv->d) {
        *key = (int64_t)cv->d;
        return true;
    }
    return false;
}

bool xsqlc_exact_setter_int64(sqlite3_value *v, const char *column,
                              int64_t *out) {
    const char *got = "NULL";
    switch (sqlite3_value_type(v)) {
        case SQLITE_INTEGER:
            *out = sqlite3_value_int64(v);
            return true;
        case SQLITE_FLOAT:
        case SQLITE_TEXT: {
            xsqlc_cmp_value cv;
            if (!xsqlc_resolve_cmp_value(v, &cv)) {
                xsql_vtab_set_error("out of memory");
                return false;
            }
            if (xsqlc_cmp_exact_key(&cv, out)) {
                return true;
            }
            got = sqlite3_value_type(v) == SQLITE_FLOAT
                      ? "a non-integral or out-of-range real"
                      : "non-integer text";
            break;
        }
        case SQLITE_BLOB:
            got = "a blob";
            break;
        default:
            break;
    }
    xsql_vtab_set_error("column \"%s\": expected an integer, got %s", column, got);
    return false;
}

bool xsqlc_exact_setter_int(sqlite3_value *v, const char *column, int *out) {
    int64_t wide = 0;
    if (!xsqlc_exact_setter_int64(v, column, &wide)) {
        return false;
    }
    if (wide < INT32_MIN || wide > INT32_MAX) {
        xsql_vtab_set_error("column \"%s\": %lld does not fit a 32-bit integer",
                            column, (long long)wide);
        return false;
    }
    *out = (int)wide;
    return true;
}

int xsqlc_cmp_bound(const xsqlc_cmp_value *cv, bool is_low, bool *strict,
                    int64_t *bound) {
    switch (cv->kind) {
        case SQLITE_NULL:
            return XSQLC_BOUND_EMPTY;
        case SQLITE_INTEGER:
            *bound = cv->i;
            return XSQLC_BOUND_SET;
        case SQLITE_FLOAT: {
            const double d = cv->d;
            if (d >= 9223372036854775808.0) {
                return is_low ? XSQLC_BOUND_EMPTY : XSQLC_BOUND_NONE;
            }
            if (d < -9223372036854775808.0) {
                return is_low ? XSQLC_BOUND_NONE : XSQLC_BOUND_EMPTY;
            }
            const int64_t t = (int64_t)d;  // truncated toward zero
            if ((double)t == d) {
                *bound = t;
                return XSQLC_BOUND_SET;
            }
            // Non-integral: key > d <=> key >= ceil(d); key < d <=> key <= floor(d).
            // Strictness no longer matters once the bound is the nearest integer.
            *strict = false;
            if (is_low) {
                *bound = d > 0 ? t + 1 : t;  // ceil
            } else {
                *bound = d > 0 ? t : t - 1;  // floor
            }
            return XSQLC_BOUND_SET;
        }
        default:
            // Text and blobs sort after every number.
            return is_low ? XSQLC_BOUND_EMPTY : XSQLC_BOUND_NONE;
    }
}

// ---------------------------------------------------------------------------
// Public factory helpers (xsql_exact_int64 / xsql_exact_text /
// xsql_int64_key_range): the comparison the built-in omit=1 plans use.
// ---------------------------------------------------------------------------

bool xsql_exact_int64(xsql_value *value, int64_t *out) {
    xsqlc_cmp_value cv;
    if (!value || !out) {
        return false;
    }
    if (!xsqlc_resolve_cmp_value((sqlite3_value *)value, &cv)) {
        xsql_vtab_set_error("out of memory");
        return false;
    }
    return xsqlc_cmp_exact_key(&cv, out);
}

const char *xsql_exact_text(xsql_value *value) {
    if (!value) {
        return NULL;
    }
    const int type = sqlite3_value_type((sqlite3_value *)value);
    if (type == SQLITE_NULL || type == SQLITE_BLOB) {
        return NULL;  // NULL equals nothing; a blob never equals text
    }
    const char *text = (const char *)sqlite3_value_text((sqlite3_value *)value);
    if (!text) {
        xsql_vtab_set_error("out of memory");  // a number's rendering failed
    }
    return text;
}

void xsql_int64_key_range_init(xsql_int64_key_range *range) {
    if (!range) {
        return;
    }
    range->lower = INT64_MIN;
    range->upper = INT64_MAX;
    range->empty = false;
}

static bool key_range_set_empty(xsql_int64_key_range *range) {
    range->empty = true;
    return false;
}

static bool key_range_apply(xsql_int64_key_range *range, xsql_constraint_op op,
                            const xsqlc_cmp_value *cv) {
    int64_t b = 0;
    bool strict = op == XSQL_CONSTRAINT_GT || op == XSQL_CONSTRAINT_LT;
    switch (op) {
        case XSQL_CONSTRAINT_EQ:
            if (!xsqlc_cmp_exact_key(cv, &b)) {
                return key_range_set_empty(range);
            }
            if (b > range->lower) range->lower = b;
            if (b < range->upper) range->upper = b;
            break;
        case XSQL_CONSTRAINT_GT:
        case XSQL_CONSTRAINT_GE:
            switch (xsqlc_cmp_bound(cv, true, &strict, &b)) {
                case XSQLC_BOUND_EMPTY:
                    return key_range_set_empty(range);
                case XSQLC_BOUND_NONE:
                    break;
                default:
                    if (strict) {
                        if (b == INT64_MAX) {
                            return key_range_set_empty(range);
                        }
                        b += 1;
                    }
                    if (b > range->lower) range->lower = b;
                    break;
            }
            break;
        case XSQL_CONSTRAINT_LT:
        case XSQL_CONSTRAINT_LE:
            switch (xsqlc_cmp_bound(cv, false, &strict, &b)) {
                case XSQLC_BOUND_EMPTY:
                    return key_range_set_empty(range);
                case XSQLC_BOUND_NONE:
                    break;
                default:
                    if (strict) {
                        if (b == INT64_MIN) {
                            return key_range_set_empty(range);
                        }
                        b -= 1;
                    }
                    if (b < range->upper) range->upper = b;
                    break;
            }
            break;
        case XSQL_CONSTRAINT_LIKE:
        default:
            xsql_vtab_set_error("LIKE is not an integer key comparison");
            return key_range_set_empty(range);
    }
    if (range->lower > range->upper) {
        range->empty = true;
    }
    return !range->empty;
}

bool xsql_int64_key_range_constrain(xsql_int64_key_range *range,
                                    xsql_constraint_op op, xsql_value *value) {
    if (!range) {
        return false;
    }
    xsqlc_cmp_value cv;
    if (!value) {
        memset(&cv, 0, sizeof(cv));
        cv.kind = SQLITE_NULL;
    } else if (!xsqlc_resolve_cmp_value((sqlite3_value *)value, &cv)) {
        xsql_vtab_set_error("out of memory");
        return key_range_set_empty(range);
    }
    return key_range_apply(range, op, &cv);
}

bool xsql_int64_key_range_constrain_int64(xsql_int64_key_range *range,
                                          xsql_constraint_op op, int64_t value) {
    if (!range) {
        return false;
    }
    xsqlc_cmp_value cv;
    memset(&cv, 0, sizeof(cv));
    cv.kind = SQLITE_INTEGER;
    cv.i = value;
    cv.d = (double)value;
    return key_range_apply(range, op, &cv);
}

bool xsql_int64_key_range_empty(const xsql_int64_key_range *range) {
    return !range || range->empty;
}

bool xsql_int64_key_range_contains(const xsql_int64_key_range *range,
                                   int64_t key) {
    return range && !range->empty && key >= range->lower && key <= range->upper;
}

int xsql_int64_key_range_unsigned_spans(const xsql_int64_key_range *range,
                                        int descending, xsql_u64_span out[2]) {
    if (!range || !out || range->empty || range->lower > range->upper) return 0;
    const uint64_t lo = (uint64_t)range->lower;
    const uint64_t hi = (uint64_t)range->upper;
    if (range->lower < 0 && range->upper >= 0) {
        const xsql_u64_span high = {lo, UINT64_MAX};
        const xsql_u64_span low = {0, hi};
        out[0] = descending ? low : high;
        out[1] = descending ? high : low;
        return 2;
    }
    out[0].lo = lo;
    out[0].hi = hi;
    return 1;
}

xsqlc_eq_arg xsqlc_eq_filter_arg(sqlite3_value *v, xsqlc_key_kind kind,
                                 sqlite3_value **copy) {
    *copy = NULL;
    const int type = sqlite3_value_type(v);
    if (kind == XSQLC_KEY_TEXT) {
        // NULL equals nothing and a blob never equals text; a number gets the
        // column's TEXT affinity, which is exactly xsql_value_text's rendering.
        return type == SQLITE_NULL || type == SQLITE_BLOB ? XSQLC_EQ_ARG_NONE
                                                          : XSQLC_EQ_ARG_PASS;
    }
    if (kind != XSQLC_KEY_INT) {
        return XSQLC_EQ_ARG_PASS;
    }
    xsqlc_cmp_value cv;
    int64_t key = 0;
    if (!xsqlc_resolve_cmp_value(v, &cv)) {
        return XSQLC_EQ_ARG_OOM;
    }
    if (!xsqlc_cmp_exact_key(&cv, &key)) {
        return XSQLC_EQ_ARG_NONE;
    }
    if (type != SQLITE_TEXT) {
        // An integer, or an integral real: sqlite3_value_int64 is exact.
        return XSQLC_EQ_ARG_PASS;
    }
    // Numeric text: sqlite3_value_int64 would stop at the first non-digit
    // ('1e1' reads as 1), so hand over a numeric copy instead.
    *copy = sqlite3_value_dup(v);
    if (!*copy) {
        return XSQLC_EQ_ARG_OOM;
    }
    (void)sqlite3_value_numeric_type(*copy);
    return XSQLC_EQ_ARG_COPY;
}

bool xsqlc_like_constraint_has_usable_prefix(sqlite3_index_info *info, int i) {
    sqlite3_value *rhs = NULL;
    int rc = sqlite3_vtab_rhs_value(info, i, &rhs);
    if (rc != SQLITE_OK || !rhs) {
        return true;  // unknown at plan time: claim; the filter stays a superset
    }
    const unsigned char *text = sqlite3_value_text(rhs);
    if (!text) {
        return false;
    }
    char c0 = (char)text[0];
    return c0 != '\0' && c0 != '%' && c0 != '_' && c0 != '\\';
}

int xsqlc_constraint_sqlite_op(xsql_constraint_op op) {
    switch (op) {
        case XSQL_CONSTRAINT_GT:   return SQLITE_INDEX_CONSTRAINT_GT;
        case XSQL_CONSTRAINT_LE:   return SQLITE_INDEX_CONSTRAINT_LE;
        case XSQL_CONSTRAINT_LT:   return SQLITE_INDEX_CONSTRAINT_LT;
        case XSQL_CONSTRAINT_GE:   return SQLITE_INDEX_CONSTRAINT_GE;
        case XSQL_CONSTRAINT_LIKE: return SQLITE_INDEX_CONSTRAINT_LIKE;
        case XSQL_CONSTRAINT_EQ:
        default:                   return SQLITE_INDEX_CONSTRAINT_EQ;
    }
}

// ---------------------------------------------------------------------------
// Constraint-request helpers (declared in <libxsqlc/vtab.h>)
// ---------------------------------------------------------------------------

static xsql_constraint_req make_req(const char *column, xsql_constraint_op op,
                                    bool required, const char *missing_error) {
    xsql_constraint_req r;
    r.column = column;
    r.op = op;
    r.required = required;
    r.missing_error = missing_error;
    return r;
}

xsql_constraint_req xsql_required_eq(const char *column,
                                     const char *missing_error) {
    return make_req(column, XSQL_CONSTRAINT_EQ, true, missing_error);
}
xsql_constraint_req xsql_required_gt(const char *column,
                                     const char *missing_error) {
    return make_req(column, XSQL_CONSTRAINT_GT, true, missing_error);
}
xsql_constraint_req xsql_required_le(const char *column,
                                     const char *missing_error) {
    return make_req(column, XSQL_CONSTRAINT_LE, true, missing_error);
}
xsql_constraint_req xsql_required_like(const char *column,
                                       const char *missing_error) {
    return make_req(column, XSQL_CONSTRAINT_LIKE, true, missing_error);
}
xsql_constraint_req xsql_optional_eq(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_EQ, false, NULL);
}
xsql_constraint_req xsql_optional_like(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_LIKE, false, NULL);
}
xsql_constraint_req xsql_optional_gt(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_GT, false, NULL);
}
xsql_constraint_req xsql_optional_ge(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_GE, false, NULL);
}
xsql_constraint_req xsql_optional_lt(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_LT, false, NULL);
}
xsql_constraint_req xsql_optional_le(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_LE, false, NULL);
}

int xsqlc_binary_constraint(const sqlite3_index_info *info, int index) {
    const char *name = sqlite3_vtab_collation((sqlite3_index_info *)info, index);
    return name && sqlite3_stricmp(name, "BINARY") == 0;
}
