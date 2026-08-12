// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// table_index.c — the index (live) virtual-table module: the sqlite3_module
// trampolines (Vtab/Cursor first-member structs), xBestIndex/xFilter constraint
// pushdown, xColumn/xRowid, and INSERT/UPDATE/DELETE via xUpdate, plus the
// builder + registrars. Shared error-channel / identifier / create-vtable
// helpers live in vtab_core.c.

#include <libxsqlc/table_index.h>

#include <libxsqlc/value.h>
#include <libxsqlc/vtab.h>

#include "internal/internal.h"
#include "internal/strbuf.h"
#include "internal/transaction.h"
#include "internal/vec.h"
#include "internal/vtab_core.h"

#include <sqlite3.h>

#include <string.h>

// Filter id 0 is reserved for "full scan".
#define IDX_FILTER_NONE 0

// ---------------------------------------------------------------------------
// Definition layout
// ---------------------------------------------------------------------------

enum { GET_I64, GET_INT, GET_DOUBLE, GET_EMIT };
enum { SET_NONE, SET_I64, SET_INT, SET_TEXT, SET_VALUE };

typedef struct {
    char            *name;
    xsql_column_type type;
    bool             writable;

    int get_kind;
    union {
        xsql_col_i64_fn    i64;
        xsql_col_int_fn    i;
        xsql_col_double_fn d;
        xsql_col_emit_fn   emit;
    } get;

    int set_kind;
    union {
        xsql_col_set_i64_fn   i64;
        xsql_col_set_int_fn   i;
        xsql_col_set_text_fn  text;
        xsql_col_set_value_fn value;
    } set;

    void *user;
} idx_column;

typedef struct {
    int                    column_index;
    int                    filter_id;
    double                 cost;
    double                 est_rows;
    xsql_filter_op         op;
    xsql_filter_factory_fn factory;
    void                  *user;
} idx_filter;

struct xsql_table {
    char *name;

    xsql_row_count_fn     count_fn;
    void                 *count_user;
    xsql_estimate_rows_fn estimate_fn;
    void                 *estimate_user;
    xsql_on_modify_fn     on_modify_fn;
    void                 *on_modify_user;
    xsql_transaction_hooks transaction_hooks;
    void                  *transaction_user;

    idx_column *columns;
    size_t      ncols, cols_cap;
    idx_filter *filters;
    size_t      nfilters, filters_cap;

    xsql_delete_fn delete_fn;
    void          *delete_user;
    bool           supports_delete;
    xsql_insert_fn insert_fn;
    void          *insert_user;
    bool           supports_insert;
};

// ---------------------------------------------------------------------------
// Cursor / vtab wrappers (sqlite base struct first — trampolines cast directly)
// ---------------------------------------------------------------------------

typedef struct {
    sqlite3_vtab      base;
    const xsql_table *def;
    xsqlc_transaction transaction;
    struct xsql_db   *owner;
    char             *schema_name;
    char             *table_name;
} IndexVtab;

typedef struct {
    sqlite3_vtab_cursor base;
    const xsql_table   *def;
    size_t              idx, total;
    xsql_row_iter       iter;
    bool                has_iter, using_iterator, iterator_eof;
} IndexCursor;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static const char *column_type_sql(xsql_column_type t) {
    switch (t) {
        case XSQL_COL_INTEGER: return "INTEGER";
        case XSQL_COL_TEXT:    return "TEXT";
        case XSQL_COL_REAL:    return "REAL";
        case XSQL_COL_BLOB:    return "BLOB";
    }
    return "TEXT";
}

static int find_column(const xsql_table *t, const char *name) {
    if (!name) {
        return -1;
    }
    for (size_t i = 0; i < t->ncols; ++i) {
        if (strcmp(t->columns[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static const idx_filter *find_filter_by_column(const xsql_table *t, int col) {
    for (size_t i = 0; i < t->nfilters; ++i) {
        if (t->filters[i].column_index == col) {
            return &t->filters[i];
        }
    }
    return NULL;
}

static void column_emit(const idx_column *c, xsql_context *ctx, size_t row) {
    switch (c->get_kind) {
        case GET_I64:    xsql_result_i64(ctx, c->get.i64(row, c->user)); break;
        case GET_INT:    xsql_result_int(ctx, c->get.i(row, c->user)); break;
        case GET_DOUBLE: xsql_result_double(ctx, c->get.d(row, c->user)); break;
        case GET_EMIT:   c->get.emit(ctx, row, c->user); break;
        default:         xsql_result_null(ctx); break;
    }
}

static bool column_set(const idx_column *c, size_t row, xsql_value *val) {
    switch (c->set_kind) {
        case SET_I64:  return c->set.i64(row, xsql_value_i64(val), c->user);
        case SET_INT:  return c->set.i(row, xsql_value_int(val), c->user);
        case SET_TEXT: {
            const char *tx = xsql_value_text(val);
            return c->set.text(row, tx ? tx : "", c->user);
        }
        case SET_VALUE: return c->set.value(row, val, c->user);
        default:        return false;
    }
}

static void fire_on_modify(const xsql_table *def, const char *verb) {
    if (!def->on_modify_fn) {
        return;
    }
    char *msg = sqlite3_mprintf("%s%s", verb, def->name);
    if (msg) {
        def->on_modify_fn(msg, def->on_modify_user);
        sqlite3_free(msg);
    }
}

// Build the CREATE TABLE schema for sqlite3_declare_vtab. Heap (xsqlc); free with
// xsql_free. NULL on OOM.
static char *build_schema(const xsql_table *t) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "CREATE TABLE ");
    xsqlc_sb_append(&sb, t->name);
    xsqlc_sb_putc(&sb, '(');
    for (size_t i = 0; i < t->ncols; ++i) {
        if (i) {
            xsqlc_sb_append_len(&sb, ", ", 2);
        }
        xsqlc_sb_putc(&sb, '"');
        xsqlc_sb_append(&sb, t->columns[i].name);
        xsqlc_sb_append(&sb, "\" ");
        xsqlc_sb_append(&sb, column_type_sql(t->columns[i].type));
    }
    xsqlc_sb_putc(&sb, ')');
    return xsqlc_sb_detach(&sb);
}

// Release any iterator held by the cursor.
static void cursor_release_iter(IndexCursor *c) {
    if (c->has_iter && c->iter.destroy) {
        c->iter.destroy(c->iter.self);
    }
    memset(&c->iter, 0, sizeof(c->iter));
    c->has_iter = false;
}

// ---------------------------------------------------------------------------
// sqlite3_module trampolines
// ---------------------------------------------------------------------------

static int index_connect(sqlite3 *db, void *pAux, int argc,
                         const char *const *argv, sqlite3_vtab **ppVtab,
                         char **pzErr) {
    (void)pzErr;
    const xsql_table *def = (const xsql_table *)pAux;
    if (!def || argc < 3 || !argv[1] || !argv[2]) {
        return SQLITE_MISUSE;
    }

    char *schema = build_schema(def);
    if (!schema) {
        return SQLITE_NOMEM;
    }
    int rc = sqlite3_declare_vtab(db, schema);
    xsql_free(schema);
    if (rc != SQLITE_OK) {
        return rc;
    }

    IndexVtab *vtab = (IndexVtab *)xsqlc_malloc(sizeof(*vtab));
    if (!vtab) {
        return SQLITE_NOMEM;
    }
    memset(vtab, 0, sizeof(*vtab));
    vtab->def = def;
    rc = xsqlc_transaction_init(&vtab->transaction,
                                &def->transaction_hooks,
                                def->transaction_user);
    if (rc != XSQL_OK) {
        xsqlc_free(vtab);
        return rc;
    }
    vtab->owner = xsqlc_db_from_sqlite(db);
    vtab->schema_name = xsqlc_strdup(argv[1]);
    vtab->table_name = xsqlc_strdup(argv[2]);
    if (!vtab->owner || !vtab->schema_name || !vtab->table_name) {
        const int failure = vtab->owner ? SQLITE_NOMEM : SQLITE_MISUSE;
        xsqlc_free(vtab->schema_name);
        xsqlc_free(vtab->table_name);
        xsqlc_transaction_destroy(&vtab->transaction);
        xsqlc_free(vtab);
        return failure;
    }
    rc = xsqlc_db_record_write_surface(
        vtab->owner, vtab->schema_name, vtab->table_name,
        def->supports_insert && def->insert_fn != NULL,
        def->supports_delete && def->delete_fn != NULL);
    if (rc == XSQL_OK) {
        for (size_t i = 0; i < def->ncols; ++i) {
            if (def->columns[i].writable &&
                def->columns[i].set_kind != SET_NONE) {
                rc = xsqlc_db_record_writable_column(
                    vtab->owner, vtab->schema_name, vtab->table_name,
                    def->columns[i].name);
                if (rc != XSQL_OK) break;
            }
        }
    }
    if (rc != XSQL_OK) {
        xsqlc_db_remove_write_surface(
            vtab->owner, vtab->schema_name, vtab->table_name);
        xsqlc_free(vtab->schema_name);
        xsqlc_free(vtab->table_name);
        xsqlc_transaction_destroy(&vtab->transaction);
        xsqlc_free(vtab);
        return rc;
    }
    *ppVtab = &vtab->base;
    return SQLITE_OK;
}

static int index_disconnect(sqlite3_vtab *pVtab) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    xsqlc_transaction_destroy(&vtab->transaction);
    xsqlc_free(vtab->schema_name);
    xsqlc_free(vtab->table_name);
    xsqlc_free(vtab);
    return SQLITE_OK;
}

static int index_destroy(sqlite3_vtab *pVtab) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    xsqlc_db_remove_write_surface(
        vtab->owner, vtab->schema_name, vtab->table_name);
    return index_disconnect(pVtab);
}

static int index_open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    IndexCursor *cur = (IndexCursor *)xsqlc_malloc(sizeof(*cur));
    if (!cur) {
        return SQLITE_NOMEM;
    }
    memset(cur, 0, sizeof(*cur));
    cur->def = vtab->def;
    *ppCursor = &cur->base;
    return SQLITE_OK;
}

static int index_close(sqlite3_vtab_cursor *pCursor) {
    IndexCursor *cur = (IndexCursor *)pCursor;
    cursor_release_iter(cur);
    xsqlc_free(cur);
    return SQLITE_OK;
}

static int index_next(sqlite3_vtab_cursor *pCursor) {
    // Cooperative cancellation: poll the query's deadline (Phase 7) so a long
    // scan / never-terminating iterator bails instead of looping forever.
    if (xsql_vtab_interrupted()) {
        xsql_vtab_set_error("query interrupted: deadline exceeded");
        return xsqlc_vtab_return_error(pCursor->pVtab);
    }
    IndexCursor *cur = (IndexCursor *)pCursor;
    if (cur->using_iterator && cur->has_iter) {
        xsqlc_vtab_clear_error();
        if (!cur->iter.next(cur->iter.self)) {
            cur->iterator_eof = true;
        }
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
    } else {
        cur->idx++;
    }
    return SQLITE_OK;
}

static int index_eof(sqlite3_vtab_cursor *pCursor) {
    IndexCursor *cur = (IndexCursor *)pCursor;
    if (cur->using_iterator) {
        if (!cur->has_iter || cur->iterator_eof) {
            return 1;
        }
        return cur->iter.eof(cur->iter.self) ? 1 : 0;
    }
    return cur->idx >= cur->total ? 1 : 0;
}

static int index_column(sqlite3_vtab_cursor *pCursor, sqlite3_context *ctx,
                        int col) {
    IndexCursor *cur = (IndexCursor *)pCursor;

    // During UPDATE, SQLite asks for unchanged columns; returning without a
    // value marks the column SQLITE_NOCHANGE in xUpdate.
    if (sqlite3_vtab_nochange(ctx)) {
        return SQLITE_OK;
    }
    if (col < 0 || (size_t)col >= cur->def->ncols) {
        sqlite3_result_null(ctx);
        return SQLITE_OK;
    }
    xsql_context *xctx = (xsql_context *)ctx;
    if (cur->using_iterator && cur->has_iter) {
        if (cur->iterator_eof) {
            sqlite3_result_null(ctx);
            return SQLITE_OK;
        }
        cur->iter.column(cur->iter.self, xctx, col);
    } else {
        column_emit(&cur->def->columns[col], xctx, cur->idx);
    }
    return SQLITE_OK;
}

static int index_rowid(sqlite3_vtab_cursor *pCursor, sqlite3_int64 *pRowid) {
    IndexCursor *cur = (IndexCursor *)pCursor;
    if (cur->using_iterator && cur->has_iter) {
        *pRowid = cur->iterator_eof ? 0 : cur->iter.rowid(cur->iter.self);
    } else {
        *pRowid = (sqlite3_int64)cur->idx;
    }
    return SQLITE_OK;
}

static int index_filter(sqlite3_vtab_cursor *pCursor, int idxNum,
                        const char *idxStr, int argc, sqlite3_value **argv) {
    (void)idxStr;
    IndexCursor *cur = (IndexCursor *)pCursor;
    const xsql_table *def = cur->def;

    cursor_release_iter(cur);
    cur->using_iterator = false;
    cur->iterator_eof = false;
    cur->idx = 0;
    cur->total = 0;

    if (idxNum != IDX_FILTER_NONE && argc > 0) {
        for (size_t i = 0; i < def->nfilters; ++i) {
            const idx_filter *f = &def->filters[i];
            if (f->filter_id != idxNum) {
                continue;
            }
            cur->using_iterator = true;
            cur->iterator_eof = true;
            xsql_row_iter it;
            memset(&it, 0, sizeof(it));
            if (f->factory((xsql_value *)argv[0], &it, f->user)) {
                cur->iter = it;
                cur->has_iter = true;
                cur->iterator_eof = !cur->iter.next(cur->iter.self);
            }
            return SQLITE_OK;
        }
    }

    // Full scan.
    cur->total = def->count_fn ? def->count_fn(def->count_user) : 0;
    return SQLITE_OK;
}

static int index_best_index(sqlite3_vtab *pVtab, sqlite3_index_info *info) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    const xsql_table *def = vtab->def;

    const idx_filter *best = NULL;
    int best_constraint = -1;
    for (int i = 0; i < info->nConstraint; ++i) {
        const struct sqlite3_index_constraint *c = &info->aConstraint[i];
        if (!c->usable || c->op != SQLITE_INDEX_CONSTRAINT_EQ) {
            continue;
        }
        const idx_filter *f = find_filter_by_column(def, c->iColumn);
        if (f && (!best || f->cost < best->cost)) {
            best = f;
            best_constraint = i;
        }
    }

    if (best && best_constraint >= 0) {
        info->aConstraintUsage[best_constraint].argvIndex = 1;
        info->aConstraintUsage[best_constraint].omit = 1;
        info->idxNum = best->filter_id;
        info->estimatedCost = best->cost;
        info->estimatedRows = (sqlite3_int64)best->est_rows;
    } else {
        // Full scan: use the cheap estimate, never the (possibly costly) count.
        size_t full = def->estimate_fn ? def->estimate_fn(def->estimate_user)
                                       : 100000;
        info->idxNum = IDX_FILTER_NONE;
        info->estimatedCost = (double)full;
        info->estimatedRows = (sqlite3_int64)full;
    }
    return SQLITE_OK;
}

// Per-column accessors for the shared UPDATE apply path (xsqlc_apply_update_columns).
typedef struct {
    const xsql_table *def;
    size_t            rowid;
} index_update_ctx;
static bool index_update_writable(void *ctx, size_t c) {
    const idx_column *col = &((const index_update_ctx *)ctx)->def->columns[c];
    return col->writable && col->set_kind != SET_NONE;
}
static const char *index_update_name(void *ctx, size_t c) {
    return ((const index_update_ctx *)ctx)->def->columns[c].name;
}
static bool index_update_apply(void *ctx, size_t c, sqlite3_value *val) {
    const index_update_ctx *x = (const index_update_ctx *)ctx;
    return column_set(&x->def->columns[c], x->rowid, (xsql_value *)val);
}

static int index_update_impl(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv,
                             sqlite3_int64 *pRowid) {
    (void)pRowid;
    IndexVtab *vtab = (IndexVtab *)pVtab;
    const xsql_table *def = vtab->def;

    // DELETE: argc == 1, argv[0] = old rowid.
    if (argc == 1 && sqlite3_value_type(argv[0]) != SQLITE_NULL) {
        if (!def->supports_delete || !def->delete_fn) {
            return xsqlc_unsupported_write(pVtab, def->name, 'D');
        }
        // A negative rowid cannot map to a valid 0-based row index; reject it before
        // it wraps to a huge size_t handed to the host delete callback.
        int64_t raw_rowid = sqlite3_value_int64(argv[0]);
        if (raw_rowid < 0) {
            return SQLITE_ERROR;
        }
        size_t rowid = (size_t)raw_rowid;
        xsqlc_transaction_touch(&vtab->transaction);
        fire_on_modify(def, "DELETE FROM ");
        return def->delete_fn(rowid, def->delete_user) ? SQLITE_OK : SQLITE_ERROR;
    }

    // UPDATE: argc > 1, argv[0] = old rowid (non-NULL).
    if (argc > 1 && sqlite3_value_type(argv[0]) != SQLITE_NULL) {
        // Negative rowid guard (see DELETE above).
        int64_t old_raw = sqlite3_value_int64(argv[0]);
        if (old_raw < 0) {
            return SQLITE_ERROR;
        }
        size_t old_rowid = (size_t)old_raw;
        xsqlc_transaction_touch(&vtab->transaction);
        fire_on_modify(def, "UPDATE ");
        // The row is resolved positionally by rowid, so NOCHANGE is always honored.
        index_update_ctx uctx = {def, old_rowid};
        xsqlc_update_ops ops = {index_update_writable, index_update_name,
                                index_update_apply, &uctx};
        if (!xsqlc_apply_update_columns(argc, argv, def->ncols,
                                        /*nochange_eligible=*/true, &ops)) {
            return xsqlc_vtab_return_error(pVtab);
        }
        return SQLITE_OK;
    }

    // INSERT: argc > 1, argv[0] == NULL.
    if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        if (!def->supports_insert || !def->insert_fn) {
            return xsqlc_unsupported_write(pVtab, def->name, 'I');
        }
        xsqlc_transaction_touch(&vtab->transaction);
        fire_on_modify(def, "INSERT INTO ");
        xsqlc_vtab_clear_error();
        // argv[0]=NULL rowid, argv[1]=rowid; column values start at argv[2].
        if (!def->insert_fn(argc - 2, (xsql_value **)&argv[2], def->insert_user)) {
            return xsqlc_vtab_return_error(pVtab);
        }
        return SQLITE_OK;
    }

    return SQLITE_READONLY;
}

static int index_update(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv,
                        sqlite3_int64 *pRowid) {
    int rc = index_update_impl(pVtab, argc, argv, pRowid);
    if (rc == SQLITE_OK) {
        xsqlc_transaction_write(&((IndexVtab *)pVtab)->transaction);
    }
    return rc;
}

static int index_begin(sqlite3_vtab *pVtab) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    return xsqlc_transaction_begin(pVtab, &vtab->transaction);
}
static int index_prepare(sqlite3_vtab *pVtab) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    return xsqlc_transaction_prepare(pVtab, &vtab->transaction);
}
static int index_commit(sqlite3_vtab *pVtab) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    return xsqlc_transaction_commit(pVtab, &vtab->transaction);
}
static int index_rollback(sqlite3_vtab *pVtab) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    return xsqlc_transaction_rollback(pVtab, &vtab->transaction);
}
static int index_savepoint(sqlite3_vtab *pVtab, int savepoint) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    return xsqlc_transaction_savepoint(
        pVtab, &vtab->transaction, savepoint);
}
static int index_release(sqlite3_vtab *pVtab, int savepoint) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    return xsqlc_transaction_release(
        pVtab, &vtab->transaction, savepoint);
}
static int index_rollback_to(sqlite3_vtab *pVtab, int savepoint) {
    IndexVtab *vtab = (IndexVtab *)pVtab;
    return xsqlc_transaction_rollback_to(
        pVtab, &vtab->transaction, savepoint);
}

// The one shared module singleton for every index table. Designated initializers
// so any trailing sqlite3_module fields (which vary by amalgamation version, e.g.
// xIntegrity) default to NULL without tripping -Wmissing-field-initializers.
static sqlite3_module g_index_module = {
    .iVersion = 3,
    .xCreate = index_connect,
    .xConnect = index_connect,
    .xBestIndex = index_best_index,
    .xDisconnect = index_disconnect,
    .xDestroy = index_destroy,
    .xOpen = index_open,
    .xClose = index_close,
    .xFilter = index_filter,
    .xNext = index_next,
    .xEof = index_eof,
    .xColumn = index_column,
    .xRowid = index_rowid,
    .xUpdate = index_update,
    .xBegin = index_begin,
    .xSync = index_prepare,
    .xCommit = index_commit,
    .xRollback = index_rollback,
    .xSavepoint = index_savepoint,
    .xRelease = index_release,
    .xRollbackTo = index_rollback_to,
};

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

xsql_table *xsql_table_new(const char *name) {
    xsql_table *t = (xsql_table *)xsqlc_malloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->name = xsqlc_strdup(name ? name : "");
    if (!t->name) {
        xsqlc_free(t);
        return NULL;
    }
    return t;
}

void xsql_table_free(xsql_table *t) {
    if (!t) {
        return;
    }
    for (size_t i = 0; i < t->ncols; ++i) {
        xsqlc_free(t->columns[i].name);
    }
    xsqlc_free(t->columns);
    xsqlc_free(t->filters);
    xsqlc_free(t->name);
    xsqlc_free(t);
}

void xsql_table_count(xsql_table *t, xsql_row_count_fn fn, void *user) {
    if (t) {
        t->count_fn = fn;
        t->count_user = user;
    }
}

void xsql_table_estimate_rows(xsql_table *t, xsql_estimate_rows_fn fn,
                              void *user) {
    if (t) {
        t->estimate_fn = fn;
        t->estimate_user = user;
    }
}

void xsql_table_on_modify(xsql_table *t, xsql_on_modify_fn fn, void *user) {
    if (t) {
        t->on_modify_fn = fn;
        t->on_modify_user = user;
    }
}

int xsql_table_transaction_hooks(
    xsql_table *t, const xsql_transaction_hooks *hooks, void *user) {
    if (!t || (hooks && hooks->struct_size != sizeof(*hooks))) {
        return XSQL_MISUSE;
    }
    memset(&t->transaction_hooks, 0, sizeof(t->transaction_hooks));
    t->transaction_user = NULL;
    if (hooks) {
        t->transaction_hooks = *hooks;
        t->transaction_user = user;
    }
    return XSQL_OK;
}

// Reserve + append a zeroed column with a duplicated name. NULL on OOM.
static idx_column *push_column(xsql_table *t, const char *name,
                               xsql_column_type type) {
    if (!xsqlc_vec_reserve((void **)&t->columns, &t->cols_cap, t->ncols + 1,
                           sizeof(idx_column))) {
        return NULL;
    }
    idx_column *c = &t->columns[t->ncols];
    memset(c, 0, sizeof(*c));
    c->name = xsqlc_strdup(name ? name : "");
    if (!c->name) {
        return NULL;
    }
    c->type = type;
    c->set_kind = SET_NONE;
    t->ncols++;
    return c;
}

int xsql_table_col_int(xsql_table *t, const char *name, xsql_col_int_fn get,
                       void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_INTEGER);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_INT;
    c->get.i = get;
    c->user = user;
    return XSQL_OK;
}

int xsql_table_col_i64(xsql_table *t, const char *name, xsql_col_i64_fn get,
                       void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_INTEGER);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_I64;
    c->get.i64 = get;
    c->user = user;
    return XSQL_OK;
}

int xsql_table_col_double(xsql_table *t, const char *name, xsql_col_double_fn get,
                          void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_REAL);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_DOUBLE;
    c->get.d = get;
    c->user = user;
    return XSQL_OK;
}

int xsql_table_col_text(xsql_table *t, const char *name, xsql_col_emit_fn get,
                        void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_TEXT);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_EMIT;
    c->get.emit = get;
    c->user = user;
    return XSQL_OK;
}

int xsql_table_col_blob(xsql_table *t, const char *name, xsql_col_emit_fn get,
                        void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_BLOB);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_EMIT;
    c->get.emit = get;
    c->user = user;
    return XSQL_OK;
}

int xsql_table_col_int_rw(xsql_table *t, const char *name, xsql_col_int_fn get,
                          xsql_col_set_int_fn set, void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_INTEGER);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_INT;
    c->get.i = get;
    c->writable = true;
    c->set_kind = SET_INT;
    c->set.i = set;
    c->user = user;
    return XSQL_OK;
}

int xsql_table_col_i64_rw(xsql_table *t, const char *name, xsql_col_i64_fn get,
                          xsql_col_set_i64_fn set, void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_INTEGER);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_I64;
    c->get.i64 = get;
    c->writable = true;
    c->set_kind = SET_I64;
    c->set.i64 = set;
    c->user = user;
    return XSQL_OK;
}

int xsql_table_col_text_rw(xsql_table *t, const char *name, xsql_col_emit_fn get,
                           xsql_col_set_text_fn set, void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_TEXT);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_EMIT;
    c->get.emit = get;
    c->writable = true;
    c->set_kind = SET_TEXT;
    c->set.text = set;
    c->user = user;
    return XSQL_OK;
}

int xsql_table_col_text_nullable_rw(xsql_table *t, const char *name,
                                    xsql_col_emit_fn get,
                                    xsql_col_set_value_fn set, void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    idx_column *c = push_column(t, name, XSQL_COL_TEXT);
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    c->get_kind = GET_EMIT;
    c->get.emit = get;
    c->writable = true;
    c->set_kind = SET_VALUE;
    c->set.value = set;
    c->user = user;
    return XSQL_OK;
}

void xsql_table_deletable(xsql_table *t, xsql_delete_fn fn, void *user) {
    if (t) {
        t->supports_delete = true;
        t->delete_fn = fn;
        t->delete_user = user;
    }
}

void xsql_table_insertable(xsql_table *t, xsql_insert_fn fn, void *user) {
    if (t) {
        t->supports_insert = true;
        t->insert_fn = fn;
        t->insert_user = user;
    }
}

static int add_filter(xsql_table *t, const char *column, double cost,
                      double est_rows, xsql_filter_factory_fn factory,
                      void *user) {
    if (!t) {
        return XSQL_MISUSE;
    }
    int col = find_column(t, column);
    if (col < 0) {
        return XSQL_NOT_FOUND;
    }
    if (!xsqlc_vec_reserve((void **)&t->filters, &t->filters_cap, t->nfilters + 1,
                           sizeof(idx_filter))) {
        return XSQL_NO_MEMORY;
    }
    idx_filter *f = &t->filters[t->nfilters];
    f->column_index = col;
    f->filter_id = (int)t->nfilters + 1;  // ids start at 1 (0 = full scan)
    f->cost = cost;
    f->est_rows = est_rows;
    f->op = XSQL_FILTER_EQ;
    f->factory = factory;
    f->user = user;
    t->nfilters++;
    return XSQL_OK;
}

int xsql_table_filter_eq(xsql_table *t, const char *column, double cost,
                         double est_rows, xsql_filter_factory_fn factory,
                         void *user) {
    return add_filter(t, column, cost, est_rows, factory, user);
}

int xsql_table_filter_eq_text(xsql_table *t, const char *column, double cost,
                              double est_rows, xsql_filter_factory_fn factory,
                              void *user) {
    // Same EQ pushdown; the factory reads `arg` as text.
    return add_filter(t, column, cost, est_rows, factory, user);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

int xsql_table_register_module(struct xsql_db *db, xsql_table *t) {
    if (!db || !db->db || !t) {
        return XSQL_MISUSE;
    }
    if (!xsqlc_is_valid_sql_identifier(t->name)) {
        return XSQL_MISUSE;
    }
    // The def is caller-owned (no destructor): it must outlive `db`.
    return sqlite3_create_module_v2(db->db, t->name, &g_index_module, t, NULL);
}

int xsql_db_create_table(struct xsql_db *db, const char *table_name,
                         const char *module_name) {
    if (!db || !db->db) {
        return XSQL_MISUSE;
    }
    return xsqlc_create_vtable_sqlite(db->db, table_name, module_name);
}

int xsql_table_register_as(struct xsql_db *db, xsql_table *t,
                           const char *table_name) {
    if (!db || !db->db || !t || !table_name) {
        return XSQL_MISUSE;
    }
    if (!xsqlc_is_valid_sql_identifier(table_name)) {
        return XSQL_MISUSE;
    }
    // Register a module named after the target table so multiple registrations
    // of the same def under different names never collide.
    int rc = sqlite3_create_module_v2(db->db, table_name, &g_index_module, t,
                                      NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    rc = xsqlc_create_vtable_sqlite(db->db, table_name, table_name);
    return rc;
}

int xsql_table_register(struct xsql_db *db, xsql_table *t) {
    if (!db || !t) {
        return XSQL_MISUSE;
    }
    return xsql_table_register_as(db, t, t->name);
}
