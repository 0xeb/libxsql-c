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

#include <libxsqlc/vtab.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sqlite3;
struct sqlite3_vtab;
struct sqlite3_value;

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

// As xsqlc_vtab_return_error, but for a table refusing to be read without
// constraints (a required constraint is missing, or a full scan is forbidden):
// returns SQLITE_CONSTRAINT_VTAB, so a caller (xsql_export_tables) can tell
// "needs a WHERE" from an ordinary backend constraint failure.
int xsqlc_vtab_return_refusal(struct sqlite3_vtab *pVtab);

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

struct sqlite3_index_info;

// Whether a LIKE/GLOB constraint `i` is worth routing to a prefix filter: true
// iff its pattern has a usable literal prefix (the first character is not a
// wildcard or the escape). A right-hand side unknown at plan time (a bound
// parameter) claims the filter: the filter is always a superset, so the worst
// case is a full walk. Shared by the cached table's LIKE/GLOB filters and the
// generator's filter_like_text (C++ detail::like_constraint_has_usable_prefix).
bool xsqlc_like_constraint_has_usable_prefix(struct sqlite3_index_info *info,
                                             int i);

// ---------------------------------------------------------------------------
// Constraint values against integer keys, the way SQLite compares them
// ---------------------------------------------------------------------------
//
// SQLite hands xFilter a constraint's value with no column affinity: k = '10'
// arrives as text, k = 10.5 as a real, k = NULL as NULL. A plan that sets
// omit=1 is trusted, so it must answer exactly what SQLite answers for an
// INTEGER column holding the same keys; sqlite3_value_int64 does not (10.5
// reads as 10, 'a' and NULL as 0, '1e1' as 1).

// A constraint value resolved the way an INTEGER column compares it: numeric
// text becomes its number (SQLite's own parser, on a private copy), other text
// and blobs are "text" (sorting after every number), NULL and NaN never match.
typedef struct {
    int     kind;  // SQLITE_NULL / SQLITE_INTEGER / SQLITE_FLOAT / SQLITE_TEXT
    int64_t i;
    double  d;
} xsqlc_cmp_value;

// False only on out-of-memory (the private copy numeric affinity needs).
bool xsqlc_resolve_cmp_value(struct sqlite3_value *v, xsqlc_cmp_value *out);

// The one integer key equal to `cv`, if any (an equality lookup).
bool xsqlc_cmp_exact_key(const xsqlc_cmp_value *cv, int64_t *key);

// A typed integer column's setter value, exactly: the int64 that equals `v`
// under SQLite's comparison with an INTEGER column (xsqlc_cmp_exact_key: 16,
// 16.0, '16', ' 16 ', '1e1' -> an integer; never NULL, a blob, a non-integral
// or out-of-range real, or non-integer text). SQLite applies no column
// affinity to the values xUpdate receives, so this is the only place that
// spelling becomes the integer. On a refusal the vtab error is set to
// `column "<column>": expected an integer, got ...` and false is returned:
// the setter must not run. The 32-bit form also refuses an exact integer
// outside int32 (`column "<column>": <value> does not fit a 32-bit integer`).
bool xsqlc_exact_setter_int64(struct sqlite3_value *v, const char *column,
                              int64_t *out);
bool xsqlc_exact_setter_int(struct sqlite3_value *v, const char *column,
                            int *out);

// One side of a range over integer keys: `cv` as a lower (`is_low`) or upper
// bound, normalized to an integer bound with the same meaning. XSQLC_BOUND_SET
// writes *bound, strict per *strict (a non-integral real clears it: the
// nearest integer is then an inclusive bound); XSQLC_BOUND_EMPTY: no key can
// satisfy it; XSQLC_BOUND_NONE: every key does (drop the bound).
enum { XSQLC_BOUND_SET = 0, XSQLC_BOUND_EMPTY = 1, XSQLC_BOUND_NONE = 2 };
int xsqlc_cmp_bound(const xsqlc_cmp_value *cv, bool is_low, bool *strict,
                    int64_t *bound);

// What an equality factory (filter_eq / filter_eq_text / filter_eq_projection)
// reads its value as. XSQLC_KEY_RAW: hand the value over untouched.
typedef enum {
    XSQLC_KEY_RAW = 0,
    XSQLC_KEY_INT,   // xsql_value_i64: called only for an exactly-equal integer
    XSQLC_KEY_TEXT   // xsql_value_text: never for NULL or a blob
} xsqlc_key_kind;

typedef enum {
    XSQLC_EQ_ARG_PASS,  // call the factory with the value as it arrived
    XSQLC_EQ_ARG_COPY,  // call it with *copy (then sqlite3_value_free it)
    XSQLC_EQ_ARG_NONE,  // no key can equal the value: no rows, no call
    XSQLC_EQ_ARG_OOM
} xsqlc_eq_arg;

// Decide how an omit=1 equality factory of `kind` sees `v`. For XSQLC_KEY_INT
// a numeric string that equals an integer arrives as a numeric copy, so the
// factory's xsql_value_i64 reads the exact key ('1e1' is 10, not 1).
xsqlc_eq_arg xsqlc_eq_filter_arg(struct sqlite3_value *v, xsqlc_key_kind kind,
                                 struct sqlite3_value **copy);

// The SQLITE_INDEX_CONSTRAINT_* code a constraint-filter spec matches.
int xsqlc_constraint_sqlite_op(xsql_constraint_op op);

// True iff `name` is a non-empty run of [A-Za-z0-9_] (SQL-injection guard).
bool xsqlc_is_valid_sql_identifier(const char *name);

// Issue `CREATE VIRTUAL TABLE <table_name> USING <module_name>;` on `db` after
// validating both identifiers. Returns an xsql_status.
int xsqlc_create_vtable_sqlite(struct sqlite3 *db, const char *table_name,
                               const char *module_name);

struct sqlite3_index_info;
/* Exact pushdown supports the declared binary collation only. */
int xsqlc_binary_constraint(const struct sqlite3_index_info *info, int index);

#endif  // XSQLC_INTERNAL_VTAB_CORE_H
