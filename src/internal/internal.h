// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// internal.h — shared declarations for libxsql-c compilation units.
//
// PRIVATE: this header lives under src/internal, is never installed, and is
// included only by .c files in this library. It centralizes the allocator so
// that everything a consumer releases with xsql_free() was allocated by the
// matching internal allocator (single allocator boundary — important across a
// future DLL split), plus the small shared string/error helpers and the two
// handle-wrapper layouts.

#ifndef XSQLC_INTERNAL_H
#define XSQLC_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

// Forward declarations so this header never forces <sqlite3.h> on its includers.
struct sqlite3;
struct sqlite3_stmt;

// Overflow-safe `count * size`. Writes the product to *out and returns true when
// it fits a size_t; returns false (leaving *out untouched) when it would wrap.
// Use at every allocation-sizing multiply where either factor is data/engine-
// driven, so a pathological count/size can never silently under-allocate.
static inline bool xsqlc_mul_ok(size_t count, size_t size, size_t *out) {
    if (size != 0 && count > ((size_t)-1) / size) {
        return false;
    }
    *out = count * size;
    return true;
}

// Allocator wrappers behind the public xsql_free() boundary. Backed by the
// vendored SQLite allocator (sqlite3_malloc/realloc/free) so allocations and
// frees always route through one heap, regardless of the C runtime a consumer
// links. Any pointer a public API documents as "free with xsql_free" MUST be
// produced by xsqlc_malloc/xsqlc_realloc.
void *xsqlc_malloc(size_t size);
void *xsqlc_realloc(void *ptr, size_t size);
void  xsqlc_free(void *ptr);

#if defined(XSQLC_TEST_ALLOC_HOOKS)
// Private test-only allocation fault injection. `countdown == 0` fails the next
// xsqlc_malloc/xsqlc_realloc call; positive values fail after that many
// successful checked allocations. Not declared in any installed header.
void xsqlc_test_alloc_fail_after(long long countdown);
void xsqlc_test_alloc_fail_reset(void);
#endif

// Duplicate a NUL-terminated string with xsqlc_malloc (NULL in -> NULL out;
// returns NULL on allocation failure). Free with xsqlc_free / xsql_free.
char *xsqlc_strdup(const char *s);

// Replace a heap-owned error slot: frees the current *slot and stores a fresh
// copy of `s` (or NULL when `s` is NULL). On allocation failure the slot is
// cleared to NULL rather than left dangling. Callers read the slot as
// `*slot ? *slot : ""`.
void xsqlc_str_set(char **slot, const char *s);

// ---------------------------------------------------------------------------
// Handle layouts (shared between database.c / statement.c / function.c /
// aggregate.c). Kept here rather than pimpl'd because the whole library is one
// static archive and several TUs cooperate on the same handles.
// ---------------------------------------------------------------------------

// One registered vtable's write capabilities, keyed by schema + SQL table name.
// Consulted by the prepare-time write-surface authorizer (unsupported-write-surface handling).
typedef struct {
    char *schema;      // heap (xsqlc); canonical SQLite schema name
    char *table;       // heap (xsqlc); SQL table name the authorizer sees
    bool  insertable;
    bool  deletable;
    char **writable_columns;
    size_t writable_column_count;
    size_t writable_column_cap;
} xsqlc_write_surface;

struct xsql_db {
    struct sqlite3 *db;          // NULL once closed
    char           *last_error;  // heap (xsqlc); NULL means "" (no error)
    // Growable write-surface registry (see xsqlc_db_record_write_surface). The
    // authorizer, installed on `db`, denies an unsupported INSERT/UPDATE/DELETE at
    // prepare time — including the 0-row case that never reaches xUpdate.
    xsqlc_write_surface *write_surfaces;
    size_t               write_surface_count;
    size_t               write_surface_cap;
    bool                 preparing_drop;
    size_t               preparing_drop_index;
    struct xsql_db      *registry_next;
};

// Concrete write surfaces are tracked by successful xCreate/xConnect/xDestroy
// callbacks, so raw/qualified SQL and persistent reconnects are covered too.
struct xsql_db *xsqlc_db_from_sqlite(struct sqlite3 *sqlite);
int xsqlc_db_record_write_surface(struct xsql_db *db, const char *schema,
                                  const char *table, bool insertable,
                                  bool deletable);
int xsqlc_db_record_writable_column(struct xsql_db *db, const char *schema,
                                    const char *table, const char *column);
void xsqlc_db_remove_write_surface(struct xsql_db *db, const char *schema,
                                   const char *table);

struct xsql_stmt {
    struct sqlite3_stmt *stmt;       // NULL once finalized
    struct sqlite3      *db;         // borrowed, for error text
    char                *last_error; // heap (xsqlc); NULL means ""
};

// Wrap a freshly-prepared sqlite3_stmt in an xsql_stmt (heap; free via
// xsql_stmt_finalize). Returns NULL on allocation failure. Defined in
// statement.c so the wrapper layout stays owned there.
struct xsql_stmt *xsqlc_stmt_wrap(struct sqlite3_stmt *stmt,
                                  struct sqlite3 *db);

// Register the built-in aggregates (currently blob_concat) on a freshly-opened
// connection. Defined in aggregate.c; called from xsql_open. Returns an
// xsql_status.
int xsqlc_register_builtin_aggregates(struct xsql_db *db);

#endif // XSQLC_INTERNAL_H
