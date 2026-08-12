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
        pVtab->zErrMsg = sqlite3_mprintf("%s", g_vtab_error);
    }
    xsqlc_vtab_clear_error();
    return SQLITE_ERROR;
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
