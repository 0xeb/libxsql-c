// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// aggregate.c — aggregate function registration, the sqlite3 -> user-callback
// trampolines, and the built-in blob_concat aggregate.

#include <libxsqlc/database.h>
#include <libxsqlc/value.h>

#include "internal/internal.h"

#include <sqlite3.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Registration + trampolines
// ---------------------------------------------------------------------------

typedef struct aggregate_wrapper {
    xsql_aggregate_step_fn  step;
    xsql_aggregate_final_fn final;
    void                   *user;
    void                  (*destroy)(void *);
} aggregate_wrapper;

static void aggregate_step_trampoline(sqlite3_context *ctx, int argc,
                                      sqlite3_value **argv) {
    aggregate_wrapper *w = (aggregate_wrapper *)sqlite3_user_data(ctx);
    if (!w || !w->step) {
        return;
    }
    w->step((xsql_context *)ctx, argc, (xsql_value **)argv, w->user);
}

static void aggregate_final_trampoline(sqlite3_context *ctx) {
    aggregate_wrapper *w = (aggregate_wrapper *)sqlite3_user_data(ctx);
    if (!w || !w->final) {
        return;
    }
    w->final((xsql_context *)ctx, w->user);
}

// Invoked when the aggregate is removed, the connection closes, or
// registration fails — releasing the user data exactly once.
static void aggregate_wrapper_destroy(void *p) {
    aggregate_wrapper *w = (aggregate_wrapper *)p;
    if (!w) {
        return;
    }
    if (w->destroy) {
        w->destroy(w->user);
    }
    xsqlc_free(w);
}

int xsql_db_create_aggregate(xsql_db *db, const char *name, int argc,
                             xsql_aggregate_step_fn step,
                             xsql_aggregate_final_fn final, void *user,
                             void (*destroy)(void *)) {
    if (!db || !db->db) {
        if (db) {
            xsqlc_str_set(&db->last_error, "Database not open");
        }
        if (destroy) {
            destroy(user);
        }
        return XSQL_ERROR;
    }

    aggregate_wrapper *w = (aggregate_wrapper *)xsqlc_malloc(sizeof(*w));
    if (!w) {
        if (destroy) {
            destroy(user);
        }
        xsqlc_str_set(&db->last_error, "out of memory");
        return XSQL_NO_MEMORY;
    }
    w->step = step;
    w->final = final;
    w->user = user;
    w->destroy = destroy;

    int rc = sqlite3_create_function_v2(db->db, name, argc,
                                        SQLITE_UTF8 | SQLITE_DETERMINISTIC, w,
                                        NULL, aggregate_step_trampoline,
                                        aggregate_final_trampoline,
                                        aggregate_wrapper_destroy);
    if (rc != SQLITE_OK) {
        // Do NOT free `w`: sqlite3_create_function_v2 already ran the xDestroy
        // callback on failure (see function.c for the detailed rationale).
        xsqlc_str_set(&db->last_error, sqlite3_errmsg(db->db));
    } else {
        xsqlc_str_set(&db->last_error, NULL);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Built-in blob_concat aggregate.
//
// One arg. NULLs are skipped; a BLOB appends its raw bytes; an INTEGER in
// [0,255] appends one byte, out of range is an error; TEXT/FLOAT/other is an
// error. The final result distinguishes "no non-NULL input" (NULL) from "input
// that contributed no bytes" (an empty BLOB).
// ---------------------------------------------------------------------------

typedef struct blob_concat_state {
    uint8_t *bytes;
    size_t   len;
    size_t   cap;
    bool     any_input;  // distinguishes "no rows / all NULL" from "empty blob"
    bool     errored;
} blob_concat_state;

static bool bc_reserve(blob_concat_state *st, size_t extra) {
    // Guard the required-size addition first: a huge `extra` (e.g. an enormous
    // input BLOB) must fail cleanly rather than wrap to a tiny under-alloc.
    if (extra > (size_t)-1 - st->len) {
        return false;
    }
    size_t need = st->len + extra;
    if (need <= st->cap) {
        return true;
    }
    size_t newcap = st->cap ? st->cap : 64;
    // Cap the geometric growth at SIZE_MAX/2 before doubling so it can neither
    // wrap to 0 (infinite loop) nor overflow into an under-sized realloc.
    while (newcap < need) {
        if (newcap > ((size_t)-1) / 2) {
            newcap = need;
            break;
        }
        newcap *= 2;
    }
    uint8_t *nb = (uint8_t *)xsqlc_realloc(st->bytes, newcap);
    if (!nb) {
        return false;
    }
    st->bytes = nb;
    st->cap = newcap;
    return true;
}

static void blob_concat_step(xsql_context *ctx, int argc, xsql_value **argv,
                             void *user) {
    (void)user;
    void **slot = xsql_aggregate_state(ctx);
    if (!slot) {
        return;
    }
    blob_concat_state *st = (blob_concat_state *)*slot;
    if (!st) {
        st = (blob_concat_state *)xsqlc_malloc(sizeof(*st));
        if (!st) {
            return;  // out of memory: skip silently, like a no-op step
        }
        st->bytes = NULL;
        st->len = 0;
        st->cap = 0;
        st->any_input = false;
        st->errored = false;
        *slot = st;
    }
    if (st->errored || argc < 1) {
        return;
    }

    xsql_value *arg = argv[0];
    int type = xsql_value_type(arg);

    if (type == SQLITE_NULL) {
        // NULL inputs are skipped, like SQLite's built-in aggregates.
        return;
    }

    st->any_input = true;

    if (type == SQLITE_BLOB) {
        const void *data = xsql_value_blob(arg);
        int len = xsql_value_bytes(arg);
        if (data && len > 0) {
            if (!bc_reserve(st, (size_t)len)) {
                st->errored = true;
                xsql_result_error(ctx, "blob_concat: out of memory");
                return;
            }
            memcpy(st->bytes + st->len, data, (size_t)len);
            st->len += (size_t)len;
        }
        return;
    }

    if (type == SQLITE_INTEGER) {
        int64_t value = xsql_value_i64(arg);
        if (value < 0 || value > 255) {
            st->errored = true;
            xsql_result_error(
                ctx, "blob_concat: INTEGER input out of byte range [0, 255]");
            return;
        }
        if (!bc_reserve(st, 1)) {
            st->errored = true;
            xsql_result_error(ctx, "blob_concat: out of memory");
            return;
        }
        st->bytes[st->len++] = (uint8_t)(value & 0xFF);
        return;
    }

    // SQLITE_TEXT, SQLITE_FLOAT, or anything else: error.
    st->errored = true;
    xsql_result_error(ctx,
                      "blob_concat: input must be BLOB, INTEGER 0-255, or NULL");
}

static void blob_concat_final(xsql_context *ctx, void *user) {
    (void)user;
    void **slot = xsql_aggregate_state(ctx);
    if (!slot || *slot == NULL) {
        // No step calls happened (zero-row aggregation): emit NULL.
        xsql_result_null(ctx);
        return;
    }
    blob_concat_state *st = (blob_concat_state *)*slot;
    if (!st->errored) {
        if (!st->any_input) {
            // No non-NULL input (zero rows or all-NULL): NULL, matching
            // SQLite's built-in aggregates.
            xsql_result_null(ctx);
        } else if (st->len == 0) {
            // At least one non-NULL input, but no bytes contributed (e.g. a
            // zero-length BLOB x''): an empty BLOB, distinct from NULL.
            xsql_result_blob(ctx, "", 0);
        } else {
            xsql_result_blob(ctx, st->bytes, st->len);
        }
    }
    // If st->errored, the error was already raised in step(); don't override.
    xsqlc_free(st->bytes);
    xsqlc_free(st);
    *slot = NULL;
}

int xsqlc_register_builtin_aggregates(struct xsql_db *db) {
    return xsql_db_create_aggregate(db, "blob_concat", 1, blob_concat_step,
                                    blob_concat_final, NULL, NULL);
}
