// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// function.c — scalar function registration and the sqlite3 -> user-callback
// trampoline.

#include <libxsqlc/database.h>

#include "internal/internal.h"

#include <sqlite3.h>

// User callback + its opaque data, allocated per registration and owned by
// SQLite (freed via the xDestroy hook below).
typedef struct scalar_wrapper {
    xsql_scalar_fn fn;
    void          *user;
    void         (*destroy)(void *);
} scalar_wrapper;

// sqlite3 xFunc: unwrap and dispatch. xsql_context / xsql_value alias the raw
// sqlite3 objects, so the casts are views, not copies.
static void scalar_trampoline(sqlite3_context *ctx, int argc,
                              sqlite3_value **argv) {
    scalar_wrapper *w = (scalar_wrapper *)sqlite3_user_data(ctx);
    if (!w || !w->fn) {
        return;
    }
    w->fn((xsql_context *)ctx, argc, (xsql_value **)argv, w->user);
}

// sqlite3 xDestroy: invoked when the function is removed, the connection
// closes, OR registration itself fails (SQLite guarantees this) — so the user
// data is released exactly once regardless of outcome.
static void scalar_wrapper_destroy(void *p) {
    scalar_wrapper *w = (scalar_wrapper *)p;
    if (!w) {
        return;
    }
    if (w->destroy) {
        w->destroy(w->user);
    }
    xsqlc_free(w);
}

int xsql_db_create_function(xsql_db *db, const char *name, int argc,
                            unsigned flags, xsql_scalar_fn fn, void *user,
                            void (*destroy)(void *)) {
    if (!db || !db->db) {
        if (db) {
            xsqlc_str_set(&db->last_error, "Database not open");
        }
        // SQLite never took ownership; release the user data ourselves.
        if (destroy) {
            destroy(user);
        }
        return XSQL_ERROR;
    }

    scalar_wrapper *w = (scalar_wrapper *)xsqlc_malloc(sizeof(*w));
    if (!w) {
        if (destroy) {
            destroy(user);
        }
        xsqlc_str_set(&db->last_error, "out of memory");
        return XSQL_NO_MEMORY;
    }
    w->fn = fn;
    w->user = user;
    w->destroy = destroy;

    // Not deterministic unless the caller says so (SQLite's own default): a
    // deterministic constant-argument call may be evaluated once and reused.
    const int text_rep =
        SQLITE_UTF8 |
        ((flags & (unsigned)XSQL_FUNCTION_DETERMINISTIC) ? SQLITE_DETERMINISTIC
                                                          : 0);
    int rc = sqlite3_create_function_v2(db->db, name, argc, text_rep, w,
                                        scalar_trampoline, NULL, NULL,
                                        scalar_wrapper_destroy);
    if (rc != SQLITE_OK) {
        // Do NOT free `w` here: sqlite3_create_function_v2 invokes the xDestroy
        // callback (scalar_wrapper_destroy) even when registration fails, which
        // already freed `w` and ran the user destructor. A manual free would be
        // a double-free.
        xsqlc_str_set(&db->last_error, sqlite3_errmsg(db->db));
    } else {
        xsqlc_str_set(&db->last_error, NULL);
    }
    return rc;
}
