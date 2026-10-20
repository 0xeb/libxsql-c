// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// database.c — the connection handle: open/close, exec, prepare, the scalar
// convenience, and introspection. Function/aggregate registration lives in
// function.c / aggregate.c; the built-in blob_concat is auto-registered here on
// open.

#include <libxsqlc/database.h>
#include <libxsqlc/statement.h>

#include "internal/internal.h"
#include "internal/vtab_core.h"

#include <sqlite3.h>

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Write-surface authorizer (unsupported-write-surface handling)
//
// Fires once per DML action at prepare time, INDEPENDENT of match count — so a
// 0-row DELETE/UPDATE on an unwritable surface is denied consistently instead of
// silently "succeeding" (xUpdate is never called when nothing matches). The
// composed message is byte-identical to xsqlc_unsupported_write so the 0-row and
// matched-row paths report the same actionable, capability-scoped error.
// ---------------------------------------------------------------------------

static struct xsql_db *g_open_databases;

static sqlite3_mutex *database_registry_mutex(void) {
    return sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_APP1);
}

static void register_open_database(struct xsql_db *db) {
    sqlite3_mutex *mutex = database_registry_mutex();
    if (mutex) sqlite3_mutex_enter(mutex);
    db->registry_next = g_open_databases;
    g_open_databases = db;
    if (mutex) sqlite3_mutex_leave(mutex);
}

static void unregister_open_database(struct xsql_db *db) {
    sqlite3_mutex *mutex = database_registry_mutex();
    if (mutex) sqlite3_mutex_enter(mutex);
    struct xsql_db **link = &g_open_databases;
    while (*link) {
        if (*link == db) {
            *link = db->registry_next;
            break;
        }
        link = &(*link)->registry_next;
    }
    db->registry_next = NULL;
    if (mutex) sqlite3_mutex_leave(mutex);
}

struct xsql_db *xsqlc_db_from_sqlite(struct sqlite3 *sqlite) {
    struct xsql_db *found = NULL;
    sqlite3_mutex *mutex = database_registry_mutex();
    if (mutex) sqlite3_mutex_enter(mutex);
    for (struct xsql_db *db = g_open_databases; db; db = db->registry_next) {
        if (db->db == sqlite) {
            found = db;
            break;
        }
    }
    if (mutex) sqlite3_mutex_leave(mutex);
    return found;
}

static void free_write_surface(xsqlc_write_surface *surface) {
    if (!surface) return;
    xsqlc_free(surface->schema);
    xsqlc_free(surface->table);
    for (size_t i = 0; i < surface->writable_column_count; ++i) {
        xsqlc_free(surface->writable_columns[i]);
    }
    xsqlc_free(surface->writable_columns);
    memset(surface, 0, sizeof(*surface));
}

static xsqlc_write_surface *find_write_surface(
    struct xsql_db *db, const char *schema, const char *table,
    size_t *index_out) {
    if (!db || !table) return NULL;
    const char *effective_schema =
        schema && schema[0] != '\0' ? schema : "main";
    for (size_t i = 0; i < db->write_surface_count; ++i) {
        xsqlc_write_surface *surface = &db->write_surfaces[i];
        if (surface->schema && surface->table &&
            sqlite3_stricmp(surface->schema, effective_schema) == 0 &&
            sqlite3_stricmp(surface->table, table) == 0) {
            if (index_out) *index_out = i;
            return surface;
        }
    }
    return NULL;
}

static int xsqlc_write_surface_authorizer(void *pArg, int action, const char *a1,
                                          const char *a2, const char *db_name,
                                          const char *trigger) {
    (void)trigger;
    struct xsql_db *db = (struct xsql_db *)pArg;
    if (!db || !a1) {
        return SQLITE_OK;
    }
    const char *schema = (db_name && db_name[0] != '\0') ? db_name : "main";
    size_t surface_index = 0;
    xsqlc_write_surface *surface =
        find_write_surface(db, schema, a1, &surface_index);
    if (action == SQLITE_DROP_VTABLE || action == SQLITE_DROP_TABLE) {
        db->preparing_drop = surface != NULL;
        db->preparing_drop_index = surface_index;
        return SQLITE_OK;
    }
    if (action == SQLITE_DELETE && db->preparing_drop &&
        db->preparing_drop_index < db->write_surface_count &&
        surface == &db->write_surfaces[db->preparing_drop_index]) {
        db->preparing_drop = false;
        return SQLITE_OK;
    }
    db->preparing_drop = false;
    if (action != SQLITE_INSERT && action != SQLITE_DELETE &&
        action != SQLITE_UPDATE) {
        return SQLITE_OK;
    }
    if (!surface) {
        return SQLITE_OK;  // not one of our registered vtables
    }
    const char *verb = NULL;
    const char *leaf = NULL;
    switch (action) {
        case SQLITE_INSERT:
            if (surface->insertable) return SQLITE_OK;
            verb = "INSERT INTO";
            leaf = "insert";
            break;
        case SQLITE_DELETE:
            if (surface->deletable) return SQLITE_OK;
            verb = "DELETE FROM";
            leaf = "delete";
            break;
        case SQLITE_UPDATE:
            for (size_t i = 0; i < surface->writable_column_count; ++i) {
                if (a2 &&
                    sqlite3_stricmp(surface->writable_columns[i], a2) == 0) {
                    return SQLITE_OK;
                }
            }
            verb = "UPDATE";
            leaf = a2 ? a2 : "*";
            break;
        default:
            return SQLITE_OK;
    }
    char msg[512];
    if (action == SQLITE_UPDATE && a2) {
        snprintf(
            msg, sizeof(msg),
            "column \"%s\" is read-only "
            "(capability mutation.%s.%s is unavailable)",
            a2, a1, leaf);
    } else {
        snprintf(
            msg, sizeof(msg),
            "%s %s is not supported "
            "(capability mutation.%s.%s is unavailable)",
            verb, a1, a1, leaf);
    }
    xsqlc_set_authz_denial(msg);
    return SQLITE_DENY;
}

int xsqlc_db_record_write_surface(struct xsql_db *db, const char *schema,
                                  const char *table, bool insertable,
                                  bool deletable) {
    if (!db || !table) return XSQL_MISUSE;
    xsqlc_write_surface *surface =
        find_write_surface(db, schema, table, NULL);
    if (surface) {
        for (size_t i = 0; i < surface->writable_column_count; ++i) {
            xsqlc_free(surface->writable_columns[i]);
        }
        xsqlc_free(surface->writable_columns);
        surface->writable_columns = NULL;
        surface->writable_column_count = 0;
        surface->writable_column_cap = 0;
        surface->insertable = insertable;
        surface->deletable = deletable;
        return XSQL_OK;
    }
    if (db->write_surface_count == db->write_surface_cap) {
        size_t ncap = db->write_surface_cap ? db->write_surface_cap * 2 : 8;
        xsqlc_write_surface *grown = (xsqlc_write_surface *)xsqlc_realloc(
            db->write_surfaces, ncap * sizeof(*grown));
        if (!grown) {
            return XSQL_NO_MEMORY;
        }
        db->write_surfaces = grown;
        db->write_surface_cap = ncap;
    }
    char *schema_dup = xsqlc_strdup(
        schema && schema[0] != '\0' ? schema : "main");
    char *table_dup = xsqlc_strdup(table);
    if (!schema_dup || !table_dup) {
        xsqlc_free(schema_dup);
        xsqlc_free(table_dup);
        return XSQL_NO_MEMORY;
    }
    surface = &db->write_surfaces[db->write_surface_count];
    memset(surface, 0, sizeof(*surface));
    surface->schema = schema_dup;
    surface->table = table_dup;
    surface->insertable = insertable;
    surface->deletable = deletable;
    db->write_surface_count++;
    return XSQL_OK;
}

int xsqlc_db_record_writable_column(struct xsql_db *db, const char *schema,
                                    const char *table, const char *column) {
    if (!column) return XSQL_MISUSE;
    xsqlc_write_surface *surface =
        find_write_surface(db, schema, table, NULL);
    if (!surface) return XSQL_MISUSE;
    for (size_t i = 0; i < surface->writable_column_count; ++i) {
        if (sqlite3_stricmp(surface->writable_columns[i], column) == 0) {
            return XSQL_OK;
        }
    }
    if (surface->writable_column_count == surface->writable_column_cap) {
        size_t cap =
            surface->writable_column_cap ? surface->writable_column_cap * 2 : 4;
        char **grown = (char **)xsqlc_realloc(
            surface->writable_columns, cap * sizeof(*grown));
        if (!grown) return XSQL_NO_MEMORY;
        surface->writable_columns = grown;
        surface->writable_column_cap = cap;
    }
    char *copy = xsqlc_strdup(column);
    if (!copy) return XSQL_NO_MEMORY;
    surface->writable_columns[surface->writable_column_count++] = copy;
    return XSQL_OK;
}

void xsqlc_db_remove_write_surface(struct xsql_db *db, const char *schema,
                                   const char *table) {
    size_t index = 0;
    xsqlc_write_surface *surface =
        find_write_surface(db, schema, table, &index);
    if (!surface) return;
    free_write_surface(surface);
    if (index + 1 < db->write_surface_count) {
        memmove(surface, surface + 1,
                (db->write_surface_count - index - 1) * sizeof(*surface));
    }
    db->write_surface_count--;
    db->preparing_drop = false;
}

// ---------------------------------------------------------------------------
// Cached-table registrations (connection-wide invalidation)
// ---------------------------------------------------------------------------

int xsqlc_db_add_cached_binding(struct xsql_db *db,
                                xsqlc_cached_binding *binding) {
    if (!db || !db->db || !binding) return XSQL_MISUSE;
    sqlite3_mutex *mutex = sqlite3_db_mutex(db->db);
    sqlite3_mutex_enter(mutex);
    int rc = XSQL_OK;
    if (db->cached_binding_count == db->cached_binding_cap) {
        const size_t cap = db->cached_binding_cap ? db->cached_binding_cap * 2 : 8;
        xsqlc_cached_binding **grown = (xsqlc_cached_binding **)xsqlc_realloc(
            db->cached_bindings, cap * sizeof(*grown));
        if (!grown) {
            rc = XSQL_NO_MEMORY;
        } else {
            db->cached_bindings = grown;
            db->cached_binding_cap = cap;
        }
    }
    if (rc == XSQL_OK) {
        db->cached_bindings[db->cached_binding_count++] = binding;
    }
    sqlite3_mutex_leave(mutex);
    return rc;
}

void xsqlc_db_remove_cached_binding(struct xsql_db *db,
                                    const xsqlc_cached_binding *binding) {
    if (!db || !binding) return;
    // Only the module destructor calls this, and SQLite runs it with the
    // connection mutex already held (inside create_module or close). Taking it
    // again here would call sqlite3_db_mutex on a connection mid-close.
    for (size_t i = 0; i < db->cached_binding_count; ++i) {
        if (db->cached_bindings[i] == binding) {
            memmove(&db->cached_bindings[i], &db->cached_bindings[i + 1],
                    (db->cached_binding_count - i - 1) *
                        sizeof(*db->cached_bindings));
            db->cached_binding_count--;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int xsql_open(const char *path, xsql_db **out) {
    if (!out) {
        return XSQL_MISUSE;
    }
    *out = NULL;

    xsql_db *db = (xsql_db *)xsqlc_malloc(sizeof(*db));
    if (!db) {
        return XSQL_NO_MEMORY;
    }
    db->db = NULL;
    db->last_error = NULL;
    db->write_surfaces = NULL;
    db->write_surface_count = 0;
    db->write_surface_cap = 0;
    db->preparing_drop = false;
    db->preparing_drop_index = 0;
    db->registry_next = NULL;
    db->cached_bindings = NULL;
    db->cached_binding_count = 0;
    db->cached_binding_cap = 0;
    db->active_query = NULL;
    db->vtab_write_count = 0;

    int rc = sqlite3_open(path, &db->db);
    if (rc != SQLITE_OK) {
        xsqlc_str_set(&db->last_error,
                      db->db ? sqlite3_errmsg(db->db)
                             : "Failed to allocate database");
        if (db->db) {
            sqlite3_close(db->db);
            db->db = NULL;
        }
        // Hand back the closed handle so the caller can read the error and must
        // still xsql_close() it.
        *out = db;
        return rc;
    }

    xsqlc_str_set(&db->last_error, NULL);
    // Module xCreate/xConnect callbacks receive only sqlite3*. Register the
    // wrapper before any module can connect so they can attach concrete
    // schema/table names to this connection's capability registry.
    register_open_database(db);
    // Install the write-surface authorizer: it reads db->write_surfaces live at
    // prepare time (populated as tables register below), denying an unsupported
    // write — including the 0-row case — at prepare.
    sqlite3_set_authorizer(db->db, xsqlc_write_surface_authorizer, db);
    xsqlc_register_builtin_aggregates(db);
    *out = db;
    return XSQL_OK;
}

int xsql_open_memory(xsql_db **out) {
    return xsql_open(":memory:", out);
}

void xsql_close(xsql_db *db) {
    if (!db) {
        return;
    }
    if (db->db) {
        sqlite3_close(db->db);
        db->db = NULL;
    }
    unregister_open_database(db);
    // sqlite3_close ran every module destructor, and each removed its own
    // binding. Any left over belongs to a connection SQLite refused to close
    // (unfinalized statements): orphan them so a late destructor never touches
    // this freed wrapper.
    for (size_t i = 0; i < db->cached_binding_count; ++i) {
        db->cached_bindings[i]->owner = NULL;
    }
    xsqlc_free(db->cached_bindings);
    for (size_t i = 0; i < db->write_surface_count; ++i) {
        free_write_surface(&db->write_surfaces[i]);
    }
    xsqlc_free(db->write_surfaces);
    xsqlc_free(db->last_error);
    xsqlc_free(db);
}

bool xsql_db_is_open(const xsql_db *db) {
    return db != NULL && db->db != NULL;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

int xsql_db_exec(xsql_db *db, const char *sql) {
    if (!db || !db->db) {
        if (db) {
            xsqlc_str_set(&db->last_error, "Database not open");
        }
        return XSQL_ERROR;
    }

    // sqlite3_exec prepares internally, so clear the denial slot first and, on an
    // authorizer denial, surface the capability-scoped message rather than
    // SQLite's fixed "not authorized" (unsupported-write-surface handling).
    xsqlc_clear_authz_denial();
    char *err = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &err);
    if (rc == SQLITE_AUTH && xsqlc_authz_denial_set()) {
        xsqlc_str_set(&db->last_error, xsqlc_authz_denial());
        if (err) sqlite3_free(err);
    } else if (err) {
        xsqlc_str_set(&db->last_error, err);
        sqlite3_free(err);
    } else if (rc != SQLITE_OK) {
        xsqlc_str_set(&db->last_error, sqlite3_errmsg(db->db));
    } else {
        xsqlc_str_set(&db->last_error, NULL);
    }
    return rc;
}

int xsql_db_exec_callback(xsql_db *db, const char *sql, xsql_exec_callback cb,
                          void *user) {
    if (!db || !db->db) {
        if (db) {
            xsqlc_str_set(&db->last_error, "Database not open");
        }
        return XSQL_ERROR;
    }

    xsqlc_clear_authz_denial();
    char *err = NULL;
    // xsql_exec_callback has the same signature as sqlite3_exec's callback.
    int rc = sqlite3_exec(db->db, sql, cb, user, &err);
    if (rc == SQLITE_AUTH && xsqlc_authz_denial_set()) {
        xsqlc_str_set(&db->last_error, xsqlc_authz_denial());
        if (err) sqlite3_free(err);
    } else if (err) {
        xsqlc_str_set(&db->last_error, err);
        sqlite3_free(err);
    } else if (rc != SQLITE_OK) {
        xsqlc_str_set(&db->last_error, sqlite3_errmsg(db->db));
    } else {
        xsqlc_str_set(&db->last_error, NULL);
    }
    return rc;
}

int xsql_db_prepare(xsql_db *db, const char *sql, xsql_stmt **out) {
    if (out) {
        *out = NULL;
    }
    if (!db || !db->db) {
        if (db) {
            xsqlc_str_set(&db->last_error, "Database not open");
        }
        return XSQL_ERROR;
    }

    xsqlc_clear_authz_denial();
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (rc == SQLITE_AUTH && xsqlc_authz_denial_set()) {
            xsqlc_str_set(&db->last_error, xsqlc_authz_denial());
        } else {
            xsqlc_str_set(&db->last_error, sqlite3_errmsg(db->db));
        }
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        return rc;
    }

    xsql_stmt *wrapped = xsqlc_stmt_wrap(stmt, db->db);
    if (!wrapped) {
        sqlite3_finalize(stmt);
        xsqlc_str_set(&db->last_error, "out of memory");
        return XSQL_NO_MEMORY;
    }
    xsqlc_str_set(&db->last_error, NULL);
    if (out) {
        *out = wrapped;
    } else {
        xsql_stmt_finalize(wrapped);
    }
    return XSQL_OK;
}

bool xsql_db_is_readonly_sql(xsql_db *db, const char *sql) {
    if (!db || !db->db) {
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        xsqlc_str_set(&db->last_error, sqlite3_errmsg(db->db));
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        return false;
    }
    bool readonly = sqlite3_stmt_readonly(stmt) != 0;
    sqlite3_finalize(stmt);
    return readonly;
}

char *xsql_db_scalar(xsql_db *db, const char *sql) {
    // Run the query and return the first cell of
    // the first row, else "". A SQL NULL and an empty-string first cell both
    // read as "" here (scalar does not consult the null flag). Errors and
    // no-rows also yield "".
    xsql_result *r = NULL;
    int rc = xsql_db_query(db, sql, &r);
    if (rc == XSQL_NO_MEMORY || !r) {
        return NULL;  // allocation failure — the only NULL return
    }

    char *out;
    if (xsql_result_ok(r) && xsql_result_row_count(r) > 0) {
        out = xsqlc_strdup(xsql_result_cell(r, 0, 0));
    } else {
        out = xsqlc_strdup("");
    }
    xsql_result_free(r);
    return out;  // NULL only on allocation failure
}

// ---------------------------------------------------------------------------
// Introspection / escape hatch
// ---------------------------------------------------------------------------

int64_t xsql_db_last_insert_rowid(const xsql_db *db) {
    return (db && db->db) ? sqlite3_last_insert_rowid(db->db) : 0;
}

int xsql_db_changes(const xsql_db *db) {
    return (db && db->db) ? sqlite3_changes(db->db) : 0;
}

int64_t xsql_db_total_changes(const xsql_db *db) {
    return (db && db->db) ? (int64_t)sqlite3_total_changes64(db->db) : 0;
}

uint64_t xsql_db_vtab_write_count(const xsql_db *db) {
    if (!db || !db->db) {
        return 0;
    }
    // Writes bump the count inside sqlite3_step, which holds the connection
    // mutex; reading under the same mutex keeps a cross-thread read whole.
    sqlite3_mutex *mutex = sqlite3_db_mutex(db->db);
    if (mutex) sqlite3_mutex_enter(mutex);
    const uint64_t count = db->vtab_write_count;
    if (mutex) sqlite3_mutex_leave(mutex);
    return count;
}

void xsqlc_note_vtab_write(struct xsql_db *owner, bool counts_as_write) {
    if (!counts_as_write || !owner || !owner->db) {
        return;
    }
    sqlite3_mutex *mutex = sqlite3_db_mutex(owner->db);
    if (mutex) sqlite3_mutex_enter(mutex);
    owner->vtab_write_count++;
    if (mutex) sqlite3_mutex_leave(mutex);
}

const char *xsql_db_errmsg(const xsql_db *db) {
    if (!db || !db->last_error) {
        return "";
    }
    return db->last_error;
}

struct sqlite3 *xsql_db_handle(xsql_db *db) {
    return db ? db->db : NULL;
}
