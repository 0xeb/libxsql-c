// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// transaction.h — shared virtual-table transaction lifecycle plumbing.
// PRIVATE: never installed.

#ifndef XSQLC_INTERNAL_TRANSACTION_H
#define XSQLC_INTERNAL_TRANSACTION_H

#include <libxsqlc/vtab.h>

#include <sqlite3.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct xsqlc_transaction_snapshot {
    int  savepoint;
    bool touched;
    bool wrote;
    bool prepared;
} xsqlc_transaction_snapshot;

typedef struct xsqlc_transaction {
    xsql_transaction_hooks hooks;
    void                  *hooks_user;
    void                  *state;
    bool                   touched;
    bool                   wrote;
    bool                   prepared;
    xsqlc_transaction_snapshot *savepoints;
    size_t                      savepoint_count;
    size_t                      savepoint_cap;
} xsqlc_transaction;

int  xsqlc_transaction_init(xsqlc_transaction *transaction,
                            const xsql_transaction_hooks *hooks, void *user);
void xsqlc_transaction_destroy(xsqlc_transaction *transaction);
void xsqlc_transaction_touch(xsqlc_transaction *transaction);
void xsqlc_transaction_write(xsqlc_transaction *transaction);
void *xsqlc_transaction_state(xsqlc_transaction *transaction);

int xsqlc_transaction_begin(sqlite3_vtab *vtab,
                            xsqlc_transaction *transaction);
int xsqlc_transaction_prepare(sqlite3_vtab *vtab,
                              xsqlc_transaction *transaction);
int xsqlc_transaction_commit(sqlite3_vtab *vtab,
                             xsqlc_transaction *transaction);
int xsqlc_transaction_rollback(sqlite3_vtab *vtab,
                               xsqlc_transaction *transaction);
int xsqlc_transaction_savepoint(sqlite3_vtab *vtab,
                                xsqlc_transaction *transaction,
                                int savepoint);
int xsqlc_transaction_release(sqlite3_vtab *vtab,
                              xsqlc_transaction *transaction,
                              int savepoint);
int xsqlc_transaction_rollback_to(sqlite3_vtab *vtab,
                                  xsqlc_transaction *transaction,
                                  int savepoint);

#endif  // XSQLC_INTERNAL_TRANSACTION_H
