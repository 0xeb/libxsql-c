// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// transaction.c — shared virtual-table transaction lifecycle plumbing.

#include "internal/transaction.h"

#include "internal/internal.h"
#include "internal/vtab_core.h"
#include "internal/vec.h"

#include <libxsqlc/status.h>

#include <string.h>

static void reset_transaction(xsqlc_transaction *transaction) {
    transaction->touched = false;
    transaction->wrote = false;
    transaction->prepared = false;
    transaction->savepoint_count = 0;
}

static int hook_error(sqlite3_vtab *vtab, int status, const char *name) {
    if (status == XSQL_OK) {
        return SQLITE_OK;
    }
    if (!xsqlc_vtab_has_error()) {
        xsql_vtab_set_error("%s transaction hook failed", name);
    }
    if (vtab) {
        sqlite3_free(vtab->zErrMsg);
        vtab->zErrMsg = sqlite3_mprintf("%s", xsqlc_vtab_get_error());
    }
    xsqlc_vtab_clear_error();
    return status;
}

int xsqlc_transaction_init(xsqlc_transaction *transaction,
                           const xsql_transaction_hooks *hooks, void *user) {
    if (!transaction) {
        return XSQL_MISUSE;
    }
    memset(transaction, 0, sizeof(*transaction));
    if (!hooks) {
        return XSQL_OK;
    }
    if (hooks->struct_size == 0) {
        return XSQL_OK;
    }
    if (hooks->struct_size != sizeof(*hooks)) {
        return XSQL_MISUSE;
    }
    transaction->hooks = *hooks;
    transaction->hooks_user = user;
    if (transaction->hooks.state_new) {
        int rc = transaction->hooks.state_new(&transaction->state, user);
        if (rc != XSQL_OK) {
            memset(&transaction->hooks, 0, sizeof(transaction->hooks));
            transaction->hooks_user = NULL;
            transaction->state = NULL;
            return rc;
        }
    }
    return XSQL_OK;
}

void xsqlc_transaction_destroy(xsqlc_transaction *transaction) {
    if (!transaction) {
        return;
    }
    if (transaction->hooks.state_free) {
        transaction->hooks.state_free(transaction->state,
                                      transaction->hooks_user);
    }
    xsqlc_free(transaction->savepoints);
    memset(transaction, 0, sizeof(*transaction));
}

void xsqlc_transaction_touch(xsqlc_transaction *transaction) {
    if (transaction) {
        transaction->touched = true;
    }
}

void xsqlc_transaction_write(xsqlc_transaction *transaction) {
    if (transaction) {
        transaction->wrote = true;
    }
}

void *xsqlc_transaction_state(xsqlc_transaction *transaction) {
    return transaction ? transaction->state : NULL;
}

int xsqlc_transaction_begin(sqlite3_vtab *vtab,
                            xsqlc_transaction *transaction) {
    (void)vtab;
    if (transaction) {
        reset_transaction(transaction);
    }
    return SQLITE_OK;
}

int xsqlc_transaction_prepare(sqlite3_vtab *vtab,
                              xsqlc_transaction *transaction) {
    if (!transaction || !transaction->wrote || transaction->prepared) {
        return SQLITE_OK;
    }
    if (transaction->hooks.prepare_commit) {
        xsqlc_vtab_clear_error();
        int rc = transaction->hooks.prepare_commit(
            transaction->state, transaction->hooks_user);
        if (rc != XSQL_OK) {
            return hook_error(vtab, rc, "prepare_commit");
        }
        xsqlc_vtab_clear_error();
    }
    transaction->prepared = true;
    return SQLITE_OK;
}

int xsqlc_transaction_commit(sqlite3_vtab *vtab,
                             xsqlc_transaction *transaction) {
    (void)vtab;
    if (!transaction) {
        return SQLITE_OK;
    }
    if (transaction->wrote && transaction->hooks.commit) {
        transaction->hooks.commit(transaction->state,
                                  transaction->hooks_user);
    }
    reset_transaction(transaction);
    xsqlc_vtab_clear_error();
    return SQLITE_OK;
}

int xsqlc_transaction_rollback(sqlite3_vtab *vtab,
                               xsqlc_transaction *transaction) {
    (void)vtab;
    if (!transaction) {
        return SQLITE_OK;
    }
    if (transaction->touched && transaction->hooks.rollback) {
        transaction->hooks.rollback(transaction->state,
                                    transaction->hooks_user);
    }
    reset_transaction(transaction);
    xsqlc_vtab_clear_error();
    return SQLITE_OK;
}

static int run_savepoint_hook(
    sqlite3_vtab *vtab, xsqlc_transaction *transaction,
    xsql_transaction_savepoint_fn hook, int savepoint,
    const char *name) {
    if (!transaction || !hook) {
        return SQLITE_OK;
    }
    xsqlc_vtab_clear_error();
    int rc = hook(transaction->state, savepoint,
                  transaction->hooks_user);
    if (rc != XSQL_OK) {
        return hook_error(vtab, rc, name);
    }
    xsqlc_vtab_clear_error();
    return SQLITE_OK;
}

int xsqlc_transaction_savepoint(sqlite3_vtab *vtab,
                                xsqlc_transaction *transaction,
                                int savepoint) {
    if (!transaction) {
        return SQLITE_OK;
    }
    if (!xsqlc_vec_reserve(
            (void **)&transaction->savepoints,
            &transaction->savepoint_cap,
            transaction->savepoint_count + 1,
            sizeof(*transaction->savepoints))) {
        return SQLITE_NOMEM;
    }
    xsqlc_transaction_snapshot *snapshot =
        &transaction->savepoints[transaction->savepoint_count++];
    snapshot->savepoint = savepoint;
    snapshot->touched = transaction->touched;
    snapshot->wrote = transaction->wrote;
    snapshot->prepared = transaction->prepared;
    int rc = run_savepoint_hook(
        vtab, transaction, transaction->hooks.savepoint,
        savepoint, "savepoint");
    if (rc != SQLITE_OK) {
        --transaction->savepoint_count;
    }
    return rc;
}

int xsqlc_transaction_release(sqlite3_vtab *vtab,
                              xsqlc_transaction *transaction,
                              int savepoint) {
    if (!transaction) {
        return SQLITE_OK;
    }
    int rc = run_savepoint_hook(
        vtab, transaction, transaction->hooks.release,
        savepoint, "release");
    if (rc != SQLITE_OK) {
        return rc;
    }
    size_t write = 0;
    for (size_t i = 0; i < transaction->savepoint_count; ++i) {
        if (transaction->savepoints[i].savepoint < savepoint) {
            transaction->savepoints[write++] =
                transaction->savepoints[i];
        }
    }
    transaction->savepoint_count = write;
    return SQLITE_OK;
}

int xsqlc_transaction_rollback_to(sqlite3_vtab *vtab,
                                  xsqlc_transaction *transaction,
                                  int savepoint) {
    if (!transaction) {
        return SQLITE_OK;
    }
    int rc = run_savepoint_hook(
        vtab, transaction, transaction->hooks.rollback_to,
        savepoint, "rollback_to");
    if (rc != SQLITE_OK) {
        return rc;
    }
    bool found = false;
    size_t write = 0;
    for (size_t i = 0; i < transaction->savepoint_count; ++i) {
        const xsqlc_transaction_snapshot snapshot =
            transaction->savepoints[i];
        if (snapshot.savepoint == savepoint) {
            transaction->touched = snapshot.touched;
            transaction->wrote = snapshot.wrote;
            transaction->prepared = snapshot.prepared;
            found = true;
        }
        if (snapshot.savepoint <= savepoint) {
            transaction->savepoints[write++] = snapshot;
        }
    }
    transaction->savepoint_count = write;
    if (!found) {
        transaction->touched = false;
        transaction->wrote = false;
        transaction->prepared = false;
    }
    return SQLITE_OK;
}
