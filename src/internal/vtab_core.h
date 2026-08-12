// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// vtab_core.h — shared virtual-table scaffolding used by every table module
// (index / cached / generator): the thread-local error channel, identifier
// validation, and the CREATE VIRTUAL TABLE helper. The public error/interrupt
// entry points (xsql_vtab_set_error / _interrupted / _set_interrupt_checker)
// live alongside these in vtab_core.c. PRIVATE.

#ifndef XSQLC_INTERNAL_VTAB_CORE_H
#define XSQLC_INTERNAL_VTAB_CORE_H

#include <stdbool.h>
#include <stddef.h>

struct sqlite3;
struct sqlite3_vtab;

// Scoped interruption helpers used by query/streaming paths. They restore an
// outer libxsql-c checker and any outer libxsql-c progress handler on the same
// connection instead of unconditionally clearing process state.
typedef struct {
    bool (*previous_fn)(void *user);
    void *previous_user;
    bool active;
} xsqlc_interrupt_guard;

typedef struct xsqlc_progress_guard {
    struct sqlite3 *db;
    int steps;
    int (*callback)(void *user);
    void *user;
    struct xsqlc_progress_guard *previous_tls;
    struct xsqlc_progress_guard *previous_same_db;
    bool active;
} xsqlc_progress_guard;

void xsqlc_interrupt_guard_enter(
    xsqlc_interrupt_guard *guard, bool (*fn)(void *user), void *user);
void xsqlc_interrupt_guard_leave(xsqlc_interrupt_guard *guard);
void xsqlc_progress_guard_enter(
    xsqlc_progress_guard *guard, struct sqlite3 *db, int steps,
    int (*callback)(void *user), void *user);
void xsqlc_progress_guard_leave(xsqlc_progress_guard *guard);

// Thread-local vtab error channel (C analog of clear/get/return_vtab_error).
void        xsqlc_vtab_clear_error(void);
const char *xsqlc_vtab_get_error(void);  // "" when none
bool        xsqlc_vtab_has_error(void);

// Copy the pending TLS error (if any) into pVtab->zErrMsg via sqlite3_mprintf,
// clear the channel, and return SQLITE_ERROR — the standard callback-failure tail.
int xsqlc_vtab_return_error(struct sqlite3_vtab *pVtab);

// Set a capability-scoped, actionable error for an unsupported write surface and
// return SQLITE_ERROR (not SQLITE_READONLY, which SQLite renders as the misleading
// generic "attempt to write a readonly database"). `op` selects the verb:
// 'I' INSERT, 'D' DELETE, 'U' UPDATE. Message is byte-identical to the C++ core's
// detail::unsupported_* so the ports report the same text
// (unsupported-write-surface handling).
int xsqlc_unsupported_write(struct sqlite3_vtab *pVtab, const char *table, char op);

// Prepare-time authorizer denial message (thread-local). The write-surface
// authorizer stashes a capability-scoped message here and returns SQLITE_DENY;
// the exec/prepare paths substitute it for SQLite's fixed "not authorized" on an
// SQLITE_AUTH failure, so the 0-row and matched-row cases report identically.
void        xsqlc_set_authz_denial(const char *msg);
void        xsqlc_clear_authz_denial(void);
const char *xsqlc_authz_denial(void);
bool        xsqlc_authz_denial_set(void);

struct sqlite3_value;

// Per-column accessors for the shared UPDATE apply path; `col` is the 0-based
// column index (value at argv[col + 2]), backed by each module's column array.
typedef struct {
    bool        (*writable)(void *ctx, size_t col);         // has a usable setter
    const char *(*name)(void *ctx, size_t col);             // column name
    bool        (*apply)(void *ctx, size_t col,             // invoke the setter
                         struct sqlite3_value *val);
    void        *ctx;
} xsqlc_update_ops;

// Apply an UPDATE's column setters, shared by the index / cached / generator
// modules. A write to a read-only column is rejected (`column "<name>" is
// read-only`) rather than silently dropped — SQLite marks unchanged columns
// NOCHANGE, so a non-NOCHANGE value on a non-writable column is a real write. The
// rejection is atomic (nothing applies if any target is read-only).
// `nochange_eligible` MUST be false on reconstruct-by-argv paths, where unchanged
// read-only identity columns carry their real value in argv and are skipped, not
// rejected. Setters apply in order and can't be rolled back, so a later failure
// names the columns already applied. Returns true on success; on failure sets the
// vtab error and returns false (caller then does cleanup + xsqlc_vtab_return_error).
bool xsqlc_apply_update_columns(int argc, struct sqlite3_value **argv, size_t ncols,
                                bool nochange_eligible,
                                const xsqlc_update_ops *ops);

// True iff `name` is a non-empty run of [A-Za-z0-9_] (SQL-injection guard).
bool xsqlc_is_valid_sql_identifier(const char *name);

// Issue `CREATE VIRTUAL TABLE <table_name> USING <module_name>;` on `db` after
// validating both identifiers. Returns an xsql_status.
int xsqlc_create_vtable_sqlite(struct sqlite3 *db, const char *table_name,
                               const char *module_name);

#endif  // XSQLC_INTERNAL_VTAB_CORE_H
