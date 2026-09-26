// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// table_cached.c — the cached virtual-table module: the sqlite3_module
// trampolines, xBestIndex (count-only / rowid / index / filter / full-scan
// priority), xFilter, xColumn (NOCHANGE gate), xRowid, and the UPDATE/DELETE
// reconstruction decision tree, plus the builder + registrars. The shared-cache
// lifecycle lives in shared_cache.c; shared vtab helpers in vtab_core.c.

#include <libxsqlc/table_cached.h>

#include <libxsqlc/value.h>
#include <libxsqlc/vtab.h>

#include "internal/cached_internal.h"
#include "internal/internal.h"
#include "internal/strbuf.h"
#include "internal/transaction.h"
#include "internal/vec.h"
#include "internal/vtab_core.h"

#include <sqlite3.h>

#include <stdlib.h>
#include <string.h>

// Filter-id sentinels.
#define CT_FILTER_NONE 0
#define CT_ROWID_FILTER (-1)
#define CT_COUNT_ONLY_SCAN (-2)
// An equality index lookup uses idxNum = CT_INDEX_BASE + index_pos.
#define CT_INDEX_BASE 1000

// Range (>=, >, <=, <, BETWEEN) pushdown on an indexed column. Encoded as
//   idxNum = CT_RANGE_BASE + index_pos * CT_RANGE_STRIDE + range_flags
// where range_flags is the 4-bit XSQLC_RANGE_* mask. CT_RANGE_BASE sits ABOVE
// the equality index space (CT_INDEX_BASE + pos) so the two never collide, and
// CT_RANGE_STRIDE reserves 16 flag values per index. A range plan always sets
// idxNum >= CT_RANGE_BASE > 0, so "VIRTUAL TABLE INDEX 0 = full scan" stays a
// valid discriminator. argv order is [low?, high?] (low first when both present).
#define CT_RANGE_BASE 2000
#define CT_RANGE_STRIDE 16
#define CT_RANGE_FLAG_MASK                                                     \
    (XSQLC_RANGE_HAS_LOW | XSQLC_RANGE_LOW_STRICT | XSQLC_RANGE_HAS_HIGH |      \
     XSQLC_RANGE_HIGH_STRICT)

// ---------------------------------------------------------------------------
// Wrappers
// ---------------------------------------------------------------------------

typedef struct {
    sqlite3_vtab             base;
    const xsql_cached_table *def;
    xsqlc_transaction transaction;
    // Query-scoped write reconstruction belongs to this SQLite connection.
    // Keeping it on the shared table definition lets another connection's
    // xFilter free rows while this connection is inside xUpdate.
    rowbuf mutation_snapshot;
    bool   mutation_snapshot_valid;
    struct xsql_db *owner;
    char           *schema_name;
    char           *table_name;
} CachedVtab;

typedef struct {
    sqlite3_vtab_cursor      base;
    const xsql_cached_table *def;

    rowbuf cache;        // non-shared fallback
    bool   cache_built;
    size_t current_row;

    xsql_row_iter iter;
    bool          has_iter, using_iterator, iterator_eof;

    bool    using_rowid_lookup, rowid_lookup_eof;
    void   *rowid_lookup_row;  // row_size buffer
    int64_t rowid_lookup_id;

    bool                 using_index;
    const xsqlc_poslist *index_matches;  // into shared->indexes or cursor_indexes
    size_t               index_pos;
    // Range (>=, >, <=, <, BETWEEN) window: a cursor-owned list of row positions
    // carved from the sorted index. When a range plan runs, index_matches points
    // at this list so xNext/xColumn/xRowid reuse the existing index machinery.
    xsqlc_poslist        range_matches;
    // Row data the index positions point into: shared->data (shared tables) or
    // &index_cache (query-scoped). Set per xFilter.
    const rowbuf        *index_data_source;
    // Cursor-owned copy of a shared-cache EQ lookup's matched rows (filled under the
    // cache mutex), so the cursor doesn't dangle if the shared cache is invalidated.
    rowbuf               index_row_copy;
    // Per-cursor index (query-scoped tables): built once on the first index xFilter
    // and reused across this cursor's xFilter calls (a JOIN's inner loop), so a
    // query-scoped table stays fast in a JOIN without an engine-lifetime cache.
    rowbuf               index_cache;
    xsqlc_i64map        *cursor_indexes;
    size_t               n_cursor_indexes;
    bool                 cursor_index_built;
    // Parallel sorted views for range pushdown on a query-scoped table, one slot
    // per index, built lazily on the first range query against this cursor and
    // reused thereafter (like cursor_indexes). Dropped with the cursor.
    xsqlc_sorted_index  *cursor_sorted_indexes;
    size_t               n_cursor_sorted;

    bool   using_count_only;
    size_t count_only_total;
} CachedCursor;

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

static int find_column(const xsql_cached_table *t, const char *name) {
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

static int sqlite_op(xsql_filter_op op) {
    switch (op) {
        case XSQL_FILTER_LIKE: return SQLITE_INDEX_CONSTRAINT_LIKE;
        case XSQL_FILTER_GLOB: return SQLITE_INDEX_CONSTRAINT_GLOB;
        case XSQL_FILTER_EQ:
        default:               return SQLITE_INDEX_CONSTRAINT_EQ;
    }
}

// Find a filter by (column, sqlite constraint op).
static const cached_filter *find_filter(const xsql_cached_table *t, int col,
                                        int op) {
    for (size_t i = 0; i < t->nfilters; ++i) {
        if (t->filters[i].column_index == col &&
            sqlite_op(t->filters[i].op) == op) {
            return &t->filters[i];
        }
    }
    return NULL;
}

static int find_index(const xsql_cached_table *t, int col) {
    for (size_t i = 0; i < t->nindexes; ++i) {
        if (t->index_defs[i].column_index == col) {
            return (int)i;
        }
    }
    return -1;
}

static bool has_scan_driven_mutation(const xsql_cached_table *def) {
    if (def->supports_delete) {
        return true;
    }
    for (size_t i = 0; i < def->ncols; ++i) {
        if (def->columns[i].writable && def->columns[i].set_kind != CSET_NONE) {
            return true;
        }
    }
    return false;
}

static bool supports_count_only_scan(const xsql_cached_table *def) {
    return def->count_fn && !def->rowid_fn && !has_scan_driven_mutation(def);
}

// Query-scoped analog of the shared-cache mutation snapshot: a NON-shared,
// full-scan (no filter iterator), positional table (no lookup_fn / rowid_fn) whose
// scan-driven UPDATE/DELETE needs the pre-mutation cache preserved for the whole
// statement (positional rowids stay valid). Filter tables are excluded: a filter
// iterator's rowid may be iterator-local, not a global cache position.
static bool has_cache_builder(const xsql_cached_table *def) {
    return def->cache_builder_fn || def->stateful_cache_builder_fn;
}

static bool query_scoped_uses_mutation_snapshot(const xsql_cached_table *def) {
    return !def->use_shared_cache && !def->lookup_fn && !def->rowid_fn &&
           def->nfilters == 0 && has_cache_builder(def) &&
           has_scan_driven_mutation(def);
}

static bool reconstruct_by_rowid(const xsql_cached_table *def) {
    return !def->update_from_column_values &&
           (((!def->rowid_fn) && def->use_shared_cache) || def->lookup_fn != NULL ||
            query_scoped_uses_mutation_snapshot(def));
}

static void invoke_cache_builder(const xsql_cached_table *def,
                                 CachedVtab *vtab, xsql_rowset *rows) {
    if (def->stateful_cache_builder_fn) {
        def->stateful_cache_builder_fn(
            rows, xsqlc_transaction_state(&vtab->transaction),
            def->stateful_cache_builder_user);
    } else if (def->cache_builder_fn) {
        def->cache_builder_fn(rows, def->cache_builder_user);
    }
}

static void column_emit(const cached_column *c, xsql_context *ctx,
                        const void *row) {
    switch (c->get_kind) {
        case CGET_I64:    xsql_result_i64(ctx, c->get.i64(row, c->user)); break;
        case CGET_INT:    xsql_result_int(ctx, c->get.i(row, c->user)); break;
        case CGET_DOUBLE: xsql_result_double(ctx, c->get.d(row, c->user)); break;
        case CGET_EMIT:   c->get.emit(ctx, row, c->user); break;
        default:          xsql_result_null(ctx); break;
    }
}

static bool column_set(const cached_column *c, void *row, xsql_value *val) {
    switch (c->set_kind) {
        case CSET_I64:  return c->set.i64(row, xsql_value_i64(val), c->user);
        case CSET_INT:  return c->set.i(row, xsql_value_int(val), c->user);
        case CSET_TEXT: {
            const char *tx = xsql_value_text(val);
            return c->set.text(row, tx ? tx : "", c->user);
        }
        case CSET_VALUE: return c->set.value(row, val, c->user);
        default:         return false;
    }
}

static void fire_modify(xsql_cached_modify_fn fn, void *user, const char *verb,
                        const char *name) {
    if (!fn) {
        return;
    }
    char *msg = sqlite3_mprintf("%s%s", verb, name);
    if (msg) {
        fn(msg, user);
        sqlite3_free(msg);
    }
}

static char *build_schema(const xsql_cached_table *t) {
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

// Consistent-snapshot read of the shared cache's publish state. The writers
// (xsqlc_cache_ensure_built / invalidate / invalidate_after_mutation) flip
// `built` / `mutation_snapshot` and swap the `data` rowbuf under `shared->mutex`;
// another connection to the same shared definition can be doing so while this
// connection scans. Reading the flag pair + row count under the same mutex avoids
// a torn/reordered read (and a stale count that would index past a shrunk buffer).
// Returns whether the shared data is usable and, when it is, the current row
// count. Callers must NOT already hold `mutex`.
static bool shared_view_usable(shared_cache *shared, size_t *count) {
    sqlite3_mutex_enter(shared->mutex);
    bool usable = shared->built || shared->mutation_snapshot;
    size_t n = shared->data.count;
    sqlite3_mutex_leave(shared->mutex);
    if (count) {
        *count = n;
    }
    return usable;
}

// Route the current full-scan / index row to `column_emit`, using the shared
// cache when it holds usable data, else the cursor-local cache.
static const void *full_scan_row(const CachedCursor *cur, size_t row) {
    const xsql_cached_table *def = cur->def;
    shared_cache *shared = def->shared;
    if (def->use_shared_cache) {
        size_t count = 0;
        if (shared_view_usable(shared, &count) && row < count) {
            return xsqlc_rowbuf_at(&shared->data, row);
        }
    }
    if (row < cur->cache.count) {
        return xsqlc_rowbuf_at(&cur->cache, row);
    }
    return NULL;
}

static void cursor_release_iter(CachedCursor *c) {
    if (c->has_iter && c->iter.destroy) {
        c->iter.destroy(c->iter.self);
    }
    memset(&c->iter, 0, sizeof(c->iter));
    c->has_iter = false;
}

// ---------------------------------------------------------------------------
// Trampolines
// ---------------------------------------------------------------------------

static int cached_connect(sqlite3 *db, void *pAux, int argc,
                          const char *const *argv, sqlite3_vtab **ppVtab,
                          char **pzErr) {
    (void)pzErr;
    const xsql_cached_table *def = (const xsql_cached_table *)pAux;
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
    CachedVtab *vtab = (CachedVtab *)xsqlc_malloc(sizeof(*vtab));
    if (!vtab) {
        return SQLITE_NOMEM;
    }
    memset(vtab, 0, sizeof(*vtab));
    vtab->def = def;
    vtab->mutation_snapshot.row_size = def->row_size;
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
                def->columns[i].set_kind != CSET_NONE) {
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

static int cached_disconnect(sqlite3_vtab *pVtab) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    xsqlc_transaction_destroy(&vtab->transaction);
    xsqlc_rowbuf_free(&vtab->mutation_snapshot);
    xsqlc_free(vtab->schema_name);
    xsqlc_free(vtab->table_name);
    xsqlc_free(vtab);
    return SQLITE_OK;
}

static int cached_destroy(sqlite3_vtab *pVtab) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    xsqlc_db_remove_write_surface(
        vtab->owner, vtab->schema_name, vtab->table_name);
    return cached_disconnect(pVtab);
}

static int cached_open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    CachedCursor *cur = (CachedCursor *)xsqlc_malloc(sizeof(*cur));
    if (!cur) {
        return SQLITE_NOMEM;
    }
    memset(cur, 0, sizeof(*cur));
    cur->def = vtab->def;
    cur->cache.row_size = vtab->def->row_size;
    cur->index_cache.row_size = vtab->def->row_size;
    cur->index_row_copy.row_size = vtab->def->row_size;
    cur->rowid_lookup_row = xsqlc_malloc(vtab->def->row_size);
    if (!cur->rowid_lookup_row) {
        xsqlc_free(cur);
        return SQLITE_NOMEM;
    }
    *ppCursor = &cur->base;
    return SQLITE_OK;
}

static int cached_close(sqlite3_vtab_cursor *pCursor) {
    CachedCursor *cur = (CachedCursor *)pCursor;
    cursor_release_iter(cur);
    xsqlc_rowbuf_free(&cur->cache);
    xsqlc_rowbuf_free(&cur->index_cache);
    xsqlc_rowbuf_free(&cur->index_row_copy);
    xsqlc_poslist_free(&cur->range_matches);
    for (size_t i = 0; i < cur->n_cursor_indexes; ++i) {
        xsqlc_i64map_free(&cur->cursor_indexes[i]);
    }
    xsqlc_free(cur->cursor_indexes);
    for (size_t i = 0; i < cur->n_cursor_sorted; ++i) {
        xsqlc_sorted_index_free(&cur->cursor_sorted_indexes[i]);
    }
    xsqlc_free(cur->cursor_sorted_indexes);
    xsqlc_free(cur->rowid_lookup_row);
    xsqlc_free(cur);
    return SQLITE_OK;
}

static int cached_next(sqlite3_vtab_cursor *pCursor) {
    // Cooperative cancellation: poll the query's deadline (Phase 7) so a large
    // cached scan bails at the deadline instead of running to completion.
    if (xsql_vtab_interrupted()) {
        xsql_vtab_set_error("query interrupted: deadline exceeded");
        return xsqlc_vtab_return_error(pCursor->pVtab);
    }
    CachedCursor *cur = (CachedCursor *)pCursor;
    if (cur->using_iterator && cur->has_iter) {
        xsqlc_vtab_clear_error();
        if (!cur->iter.next(cur->iter.self)) {
            cur->iterator_eof = true;
        }
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
    } else if (cur->using_rowid_lookup) {
        cur->rowid_lookup_eof = true;
    } else if (cur->using_index) {
        cur->index_pos++;
    } else {
        cur->current_row++;
    }
    return SQLITE_OK;
}

static int cached_eof(sqlite3_vtab_cursor *pCursor) {
    CachedCursor *cur = (CachedCursor *)pCursor;
    const xsql_cached_table *def = cur->def;
    if (cur->using_iterator) {
        if (!cur->has_iter || cur->iterator_eof) {
            return 1;
        }
        return cur->iter.eof(cur->iter.self) ? 1 : 0;
    }
    if (cur->using_rowid_lookup) {
        return cur->rowid_lookup_eof ? 1 : 0;
    }
    if (cur->using_index) {
        if (!cur->index_matches) {
            return 1;
        }
        return cur->index_pos >= cur->index_matches->count ? 1 : 0;
    }
    if (cur->using_count_only) {
        return cur->current_row >= cur->count_only_total ? 1 : 0;
    }
    shared_cache *shared = def->shared;
    if (def->use_shared_cache) {
        size_t count = 0;
        if (shared_view_usable(shared, &count)) {
            return cur->current_row >= count ? 1 : 0;
        }
    }
    return cur->current_row >= cur->cache.count ? 1 : 0;
}

static int cached_xcolumn(sqlite3_vtab_cursor *pCursor, sqlite3_context *ctx,
                          int col) {
    CachedCursor *cur = (CachedCursor *)pCursor;
    const xsql_cached_table *def = cur->def;

    // NOCHANGE is honored only when the row can be reconstructed from the rowid
    // alone (must match cached_update's reconstruct_by_rowid) — A7.
    if (sqlite3_vtab_nochange(ctx) && !def->update_from_column_values &&
        (((!def->rowid_fn) && def->use_shared_cache) || def->lookup_fn ||
         query_scoped_uses_mutation_snapshot(def))) {
        return SQLITE_OK;
    }

    if (col < 0 || (size_t)col >= def->ncols) {
        sqlite3_result_null(ctx);
        return SQLITE_OK;
    }
    if (cur->using_count_only) {
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
    } else if (cur->using_rowid_lookup) {
        if (cur->rowid_lookup_eof) {
            sqlite3_result_null(ctx);
            return SQLITE_OK;
        }
        column_emit(&def->columns[col], xctx, cur->rowid_lookup_row);
    } else if (cur->using_index) {
        if (cur->index_matches && cur->index_pos < cur->index_matches->count) {
            size_t row_idx = cur->index_matches->items[cur->index_pos];
            const rowbuf *src = cur->index_data_source;
            if (src && row_idx < src->count) {
                column_emit(&def->columns[col], xctx, xsqlc_rowbuf_at(src, row_idx));
            } else {
                sqlite3_result_null(ctx);
            }
        } else {
            sqlite3_result_null(ctx);
        }
    } else {
        const void *row = full_scan_row(cur, cur->current_row);
        if (row) {
            column_emit(&def->columns[col], xctx, row);
        } else {
            sqlite3_result_null(ctx);
        }
    }
    return SQLITE_OK;
}

static int cached_rowid(sqlite3_vtab_cursor *pCursor, sqlite3_int64 *pRowid) {
    CachedCursor *cur = (CachedCursor *)pCursor;
    const xsql_cached_table *def = cur->def;
    if (cur->using_iterator && cur->has_iter) {
        *pRowid = cur->iterator_eof ? 0 : cur->iter.rowid(cur->iter.self);
    } else if (cur->using_rowid_lookup) {
        *pRowid = cur->rowid_lookup_eof ? 0 : cur->rowid_lookup_id;
    } else if (cur->using_index) {
        // Rowid honors rowid_fn; else the matched cache POSITION (row_idx), NOT
        // cur->current_row (the index path does not advance it), matching the
        // full-scan path so positional/lookup reconstruction resolves the same row.
        if (cur->index_matches && cur->index_pos < cur->index_matches->count) {
            size_t row_idx = cur->index_matches->items[cur->index_pos];
            if (def->rowid_fn) {
                const rowbuf *src = cur->index_data_source;
                if (src && row_idx < src->count) {
                    *pRowid = def->rowid_fn(xsqlc_rowbuf_at(src, row_idx),
                                            def->rowid_user);
                    return SQLITE_OK;
                }
            }
            *pRowid = (sqlite3_int64)row_idx;
        } else {
            *pRowid = (sqlite3_int64)cur->current_row;
        }
    } else if (cur->using_count_only) {
        *pRowid = (sqlite3_int64)cur->current_row;
    } else if (def->rowid_fn) {
        const void *row = full_scan_row(cur, cur->current_row);
        *pRowid = row ? def->rowid_fn(row, def->rowid_user)
                      : (sqlite3_int64)cur->current_row;
    } else {
        *pRowid = (sqlite3_int64)cur->current_row;
    }
    return SQLITE_OK;
}

static void cached_reset_cursor(CachedCursor *cur) {
    cursor_release_iter(cur);
    cur->using_iterator = false;
    cur->iterator_eof = false;
    cur->using_rowid_lookup = false;
    cur->rowid_lookup_eof = false;
    cur->rowid_lookup_id = 0;
    cur->using_index = false;
    cur->index_matches = NULL;
    cur->index_pos = 0;
    xsqlc_poslist_clear(&cur->range_matches);
    cur->using_count_only = false;
    cur->count_only_total = 0;
    xsqlc_rowbuf_clear(&cur->cache);
    cur->cache_built = false;
    cur->current_row = 0;
}

// Build (once, lazily) a connection-local snapshot of the pre-mutation rows so
// a scan-driven multi-row UPDATE/DELETE on a query-scoped table keeps stable
// positional rowids for the whole statement. The snapshot must not live in
// def->shared: the same table definition can be registered on several SQLite
// connections and another connection may start a scan while xUpdate is using it.
static bool ensure_query_scoped_mutation_snapshot(const xsql_cached_table *def,
                                                  CachedVtab *vtab) {
    if (!query_scoped_uses_mutation_snapshot(def)) {
        return true;
    }
    if (!vtab->mutation_snapshot_valid) {
        xsqlc_rowbuf_clear(&vtab->mutation_snapshot);
        struct xsql_rowset rs = {&vtab->mutation_snapshot};
        xsqlc_vtab_clear_error();
        invoke_cache_builder(def, vtab, &rs);
        if (xsqlc_vtab_has_error()) {
            xsqlc_rowbuf_clear(&vtab->mutation_snapshot);
            vtab->mutation_snapshot_valid = false;
            return false;
        }
        vtab->mutation_snapshot_valid = true;
    }
    return true;
}

// Drop any query-scoped mutation snapshot at the start of the next scan (= next
// statement), so reads rebuild fresh from the live source. No-op for shared tables.
static void clear_query_scoped_mutation_snapshot(CachedVtab *vtab) {
    if (!vtab || vtab->def->use_shared_cache) {
        return;
    }
    if (vtab->mutation_snapshot_valid) {
        vtab->mutation_snapshot_valid = false;
        xsqlc_rowbuf_clear(&vtab->mutation_snapshot);
    }
}

// Build (once) a per-cursor cache + hash indexes for a query-scoped table, reused
// across this cursor's xFilter calls (a JOIN's inner loop).
static bool build_cursor_index(CachedCursor *cur) {
    const xsql_cached_table *def = cur->def;
    if (cur->cursor_index_built) {
        return true;
    }
    xsqlc_rowbuf_clear(&cur->index_cache);
    struct xsql_rowset rs = {&cur->index_cache};
    CachedVtab *vtab = (CachedVtab *)cur->base.pVtab;
    invoke_cache_builder(def, vtab, &rs);
    if (xsqlc_vtab_has_error()) {
        xsqlc_rowbuf_clear(&cur->index_cache);
        return false;
    }
    if (def->nindexes > 0) {
        cur->cursor_indexes =
            (xsqlc_i64map *)xsqlc_malloc(def->nindexes * sizeof(xsqlc_i64map));
        if (!cur->cursor_indexes) {
            xsql_vtab_set_error("out of memory");
            return false;
        }
        cur->n_cursor_indexes = def->nindexes;
        for (size_t i = 0; i < def->nindexes; ++i) {
            xsqlc_i64map_init(&cur->cursor_indexes[i]);
        }
        for (size_t i = 0; i < def->nindexes; ++i) {
            const cached_index_def *ix = &def->index_defs[i];
            for (size_t row = 0; row < cur->index_cache.count; ++row) {
                const void *r = xsqlc_rowbuf_at(&cur->index_cache, row);
                if (!xsqlc_i64map_add(&cur->cursor_indexes[i], ix->key(r, ix->user),
                                      row)) {
                    xsql_vtab_set_error("out of memory");
                    // OOM mid-build: free the partial index so a re-entry (a JOIN
                    // re-step / reset+re-step after the error) rebuilds cleanly
                    // instead of overwriting (and leaking) cursor_indexes.
                    for (size_t j = 0; j < cur->n_cursor_indexes; ++j) {
                        xsqlc_i64map_free(&cur->cursor_indexes[j]);
                    }
                    xsqlc_free(cur->cursor_indexes);
                    cur->cursor_indexes = NULL;
                    cur->n_cursor_indexes = 0;
                    return false;
                }
            }
        }
    }
    cur->cursor_index_built = true;
    return true;
}

static int cached_xfilter(sqlite3_vtab_cursor *pCursor, int idxNum,
                          const char *idxStr, int argc, sqlite3_value **argv) {
    CachedCursor *cur = (CachedCursor *)pCursor;
    const xsql_cached_table *def = cur->def;
    cached_reset_cursor(cur);
    // A new scan begins a new statement: drop any query-scoped mutation snapshot
    // preserved from a prior scan-driven UPDATE/DELETE (no-op for shared tables).
    clear_query_scoped_mutation_snapshot((CachedVtab *)pCursor->pVtab);

    if (idxNum == CT_COUNT_ONLY_SCAN && argc == 0 &&
        supports_count_only_scan(def)) {
        xsqlc_vtab_clear_error();
        cur->using_count_only = true;
        cur->count_only_total = def->count_fn(def->count_user);
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
        return SQLITE_OK;
    }

    if (idxNum != CT_FILTER_NONE && argc > 0) {
        if (idxNum == CT_ROWID_FILTER) {
            if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
                cur->using_rowid_lookup = true;
                cur->rowid_lookup_id = 0;
                cur->rowid_lookup_eof = true;
                return SQLITE_OK;
            }
            int64_t raw = sqlite3_value_int64(argv[0]);
            cur->using_rowid_lookup = true;
            cur->rowid_lookup_id = raw;
            cur->rowid_lookup_eof = true;
            if (raw >= 0) {
                xsqlc_vtab_clear_error();
                bool found = false;
                if (def->lookup_fn) {
                    found = def->lookup_fn(cur->rowid_lookup_row, raw,
                                           def->lookup_user);
                    cur->rowid_lookup_eof = !found;
                }
                if (!found) {
                    if (def->use_shared_cache) {
                        if (!xsqlc_cache_ensure_built(def)) {
                            return xsqlc_vtab_return_error(pCursor->pVtab);
                        }
                        shared_cache *shared = def->shared;
                        size_t rid = (size_t)raw;
                        if (shared->built && rid < shared->data.count) {
                            memcpy(cur->rowid_lookup_row,
                                   xsqlc_rowbuf_at(&shared->data, rid),
                                   def->row_size);
                            cur->rowid_lookup_eof = false;
                        }
                    } else if (!def->lookup_fn && has_cache_builder(def)) {
                        rowbuf rows;
                        memset(&rows, 0, sizeof(rows));
                        rows.row_size = def->row_size;
                        struct xsql_rowset rs = {&rows};
                        CachedVtab *vtab =
                            (CachedVtab *)pCursor->pVtab;
                        invoke_cache_builder(def, vtab, &rs);
                        if (xsqlc_vtab_has_error()) {
                            xsqlc_rowbuf_free(&rows);
                            return xsqlc_vtab_return_error(pCursor->pVtab);
                        }
                        size_t rid = (size_t)raw;
                        if (rid < rows.count) {
                            memcpy(cur->rowid_lookup_row,
                                   xsqlc_rowbuf_at(&rows, rid), def->row_size);
                            cur->rowid_lookup_eof = false;
                        }
                        xsqlc_rowbuf_free(&rows);
                    }
                }
                if (xsqlc_vtab_has_error()) {
                    return xsqlc_vtab_return_error(pCursor->pVtab);
                }
            }
            return SQLITE_OK;
        }

        // Range (>=, >, <=, <, BETWEEN) plan on an indexed column. CT_RANGE_BASE
        // sits above CT_INDEX_BASE, so this MUST be tested BEFORE the equality-
        // index branch below. The sorted view is built lazily here and cached
        // (shared cache for shared tables, per-cursor for query-scoped).
        if (idxNum >= CT_RANGE_BASE) {
            int encoded = idxNum - CT_RANGE_BASE;
            int index_pos = encoded / CT_RANGE_STRIDE;
            int range_flags = encoded & CT_RANGE_FLAG_MASK;
            if (index_pos >= 0 && (size_t)index_pos < def->nindexes) {
                // Decode bounds from argv: low first (if present), then high.
                int arg = 0;
                int64_t low = 0, high = 0;
                if (range_flags & XSQLC_RANGE_HAS_LOW) {
                    if (arg < argc) low = sqlite3_value_int64(argv[arg]);
                    ++arg;
                }
                if (range_flags & XSQLC_RANGE_HAS_HIGH) {
                    if (arg < argc) high = sqlite3_value_int64(argv[arg]);
                    ++arg;
                }
                xsqlc_vtab_clear_error();
                if (def->use_shared_cache) {
                    if (!xsqlc_cache_ensure_built(def)) {
                        return xsqlc_vtab_return_error(pCursor->pVtab);
                    }
                    shared_cache *shared = def->shared;
                    // The lazy sorted-view build mutates shared->sorted_indexes
                    // (a stable sort into a shared array); collecting reads it.
                    // Both must hold shared->mutex — otherwise two connections
                    // range-querying the same index race writer vs reader on the
                    // same array (UB). Snapshot the matches into the per-cursor
                    // range_matches under the lock, then release; the data source
                    // pointer stays valid (shared->data is only rebuilt under the
                    // same mutex on invalidation).
                    sqlite3_mutex_enter(shared->mutex);
                    if (shared->built &&
                        (size_t)index_pos < shared->nsorted) {
                        xsqlc_sorted_index *sorted =
                            &shared->sorted_indexes[index_pos];
                        if (!sorted->built &&
                            !xsqlc_sorted_index_build(
                                sorted, &shared->data,
                                def->index_defs[index_pos].key,
                                def->index_defs[index_pos].user)) {
                            sqlite3_mutex_leave(shared->mutex);
                            xsql_vtab_set_error("out of memory");
                            return xsqlc_vtab_return_error(pCursor->pVtab);
                        }
                        xsqlc_poslist_clear(&cur->range_matches);
                        if (!xsqlc_sorted_index_collect(sorted, range_flags, low,
                                                        high,
                                                        &cur->range_matches)) {
                            sqlite3_mutex_leave(shared->mutex);
                            xsql_vtab_set_error("out of memory");
                            return xsqlc_vtab_return_error(pCursor->pVtab);
                        }
                        sqlite3_mutex_leave(shared->mutex);
                        cur->using_index = true;
                        cur->index_matches = &cur->range_matches;
                        cur->index_data_source = &shared->data;
                        cur->index_pos = 0;
                        return SQLITE_OK;
                    }
                    sqlite3_mutex_leave(shared->mutex);
                } else if (has_cache_builder(def)) {
                    // Query-scoped: build the per-cursor cache + hash indexes once,
                    // then the sorted view on demand (reused across this cursor).
                    if (!build_cursor_index(cur)) {
                        return xsqlc_vtab_return_error(pCursor->pVtab);
                    }
                    if (cur->n_cursor_sorted != def->nindexes) {
                        for (size_t i = 0; i < cur->n_cursor_sorted; ++i) {
                            xsqlc_sorted_index_free(&cur->cursor_sorted_indexes[i]);
                        }
                        xsqlc_free(cur->cursor_sorted_indexes);
                        cur->cursor_sorted_indexes = NULL;
                        cur->n_cursor_sorted = 0;
                        if (def->nindexes > 0) {
                            cur->cursor_sorted_indexes =
                                (xsqlc_sorted_index *)xsqlc_malloc(
                                    def->nindexes *
                                    sizeof(*cur->cursor_sorted_indexes));
                            if (!cur->cursor_sorted_indexes) {
                                xsql_vtab_set_error("out of memory");
                                return xsqlc_vtab_return_error(pCursor->pVtab);
                            }
                            memset(cur->cursor_sorted_indexes, 0,
                                   def->nindexes *
                                       sizeof(*cur->cursor_sorted_indexes));
                            cur->n_cursor_sorted = def->nindexes;
                        }
                    }
                    if ((size_t)index_pos < cur->n_cursor_sorted) {
                        xsqlc_sorted_index *sorted =
                            &cur->cursor_sorted_indexes[index_pos];
                        if (!sorted->built &&
                            !xsqlc_sorted_index_build(
                                sorted, &cur->index_cache,
                                def->index_defs[index_pos].key,
                                def->index_defs[index_pos].user)) {
                            xsql_vtab_set_error("out of memory");
                            return xsqlc_vtab_return_error(pCursor->pVtab);
                        }
                        xsqlc_poslist_clear(&cur->range_matches);
                        if (!xsqlc_sorted_index_collect(sorted, range_flags, low,
                                                        high,
                                                        &cur->range_matches)) {
                            xsql_vtab_set_error("out of memory");
                            return xsqlc_vtab_return_error(pCursor->pVtab);
                        }
                        cur->using_index = true;
                        cur->index_matches = &cur->range_matches;
                        cur->index_data_source = &cur->index_cache;
                        cur->index_pos = 0;
                        return SQLITE_OK;
                    }
                }
            }
        }

        if (idxNum >= CT_INDEX_BASE) {
            int index_pos = idxNum - CT_INDEX_BASE;
            if (index_pos >= 0 && (size_t)index_pos < def->nindexes) {
                xsqlc_vtab_clear_error();
                if (def->use_shared_cache) {
                    if (!xsqlc_cache_ensure_built(def)) {
                        return xsqlc_vtab_return_error(pCursor->pVtab);
                    }
                    shared_cache *shared = def->shared;
                    if (shared->built && (size_t)index_pos < shared->nindexes) {
                        int64_t key = sqlite3_value_int64(argv[0]);
                        // Copy the matched rows into cursor-owned storage under the
                        // cache mutex, so a concurrent invalidate can't free the
                        // shared index/data out from under the cursor mid-scan.
                        sqlite3_mutex_enter(shared->mutex);
                        if (shared->built &&
                            (size_t)index_pos < shared->nindexes) {
                            const xsqlc_poslist *matches =
                                xsqlc_i64map_get(&shared->indexes[index_pos], key);
                            xsqlc_rowbuf_clear(&cur->index_row_copy);
                            xsqlc_poslist_clear(&cur->range_matches);
                            if (matches) {
                                for (size_t j = 0; j < matches->count; ++j) {
                                    size_t pos = matches->items[j];
                                    if (pos < shared->data.count) {
                                        xsqlc_poslist_push(&cur->range_matches,
                                                           cur->index_row_copy.count);
                                        xsqlc_rowbuf_push(
                                            &cur->index_row_copy,
                                            xsqlc_rowbuf_at(&shared->data, pos));
                                    }
                                }
                            }
                            cur->using_index = true;
                            cur->index_matches = &cur->range_matches;
                            cur->index_data_source = &cur->index_row_copy;
                            cur->index_pos = 0;
                            sqlite3_mutex_leave(shared->mutex);
                            return SQLITE_OK;
                        }
                        sqlite3_mutex_leave(shared->mutex);
                    }
                } else if (has_cache_builder(def)) {
                    // Query-scoped: build the per-cursor cache + hash indexes once,
                    // then reuse across this cursor's xFilter calls (JOIN inner loop).
                    if (!build_cursor_index(cur)) {
                        return xsqlc_vtab_return_error(pCursor->pVtab);
                    }
                    if ((size_t)index_pos < cur->n_cursor_indexes) {
                        int64_t key = sqlite3_value_int64(argv[0]);
                        cur->using_index = true;
                        cur->index_matches =
                            xsqlc_i64map_get(&cur->cursor_indexes[index_pos], key);
                        cur->index_data_source = &cur->index_cache;
                        cur->index_pos = 0;
                        return SQLITE_OK;
                    }
                }
            }
        }

        for (size_t i = 0; i < def->nfilters; ++i) {
            const cached_filter *f = &def->filters[i];
            if (f->filter_id != idxNum) {
                continue;
            }
            xsqlc_vtab_clear_error();
            cur->using_iterator = true;
            cur->iterator_eof = true;
            xsql_row_iter it;
            memset(&it, 0, sizeof(it));
            if (f->factory((xsql_value *)argv[0], &it, f->user)) {
                cur->iter = it;
                cur->has_iter = true;
                cur->iterator_eof = !cur->iter.next(cur->iter.self);
            }
            if (xsqlc_vtab_has_error()) {
                return xsqlc_vtab_return_error(pCursor->pVtab);
            }
            return SQLITE_OK;
        }
    }

    // Full scan.
    if (def->use_shared_cache) {
        xsqlc_vtab_clear_error();
        if (!xsqlc_cache_ensure_built(def)) {
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
    } else if (def->projection_fn || has_cache_builder(def)) {
        xsqlc_rowbuf_clear(&cur->cache);
        xsqlc_vtab_clear_error();
        struct xsql_rowset rs = {&cur->cache};
        if (def->projection_fn) {
            uint64_t col_used = ~(uint64_t)0;
            if (idxStr && *idxStr) {
                col_used = (uint64_t)strtoull(idxStr, NULL, 10);
            }
            def->projection_fn(&rs, col_used, def->projection_user);
        } else {
            CachedVtab *vtab = (CachedVtab *)pCursor->pVtab;
            invoke_cache_builder(def, vtab, &rs);
        }
        if (xsqlc_vtab_has_error()) {
            xsqlc_rowbuf_clear(&cur->cache);
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
        cur->cache_built = true;
    }
    cur->using_iterator = false;
    cur->using_index = false;
    cur->current_row = 0;
    return SQLITE_OK;
}

// A LIKE/GLOB constraint is worth a prefix filter only if the pattern has a
// usable literal prefix; unknown-at-plan (bound param) => claim (A9).
static bool like_has_usable_prefix(sqlite3_index_info *info, int i) {
    sqlite3_value *rhs = NULL;
    int rc = sqlite3_vtab_rhs_value(info, i, &rhs);
    if (rc != SQLITE_OK || !rhs) {
        return true;
    }
    const unsigned char *text = sqlite3_value_text(rhs);
    if (!text) {
        return false;
    }
    char c0 = (char)text[0];
    return c0 != '\0' && c0 != '%' && c0 != '_' && c0 != '\\';
}

static int cached_best_index(sqlite3_vtab *pVtab, sqlite3_index_info *info) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    const xsql_cached_table *def = vtab->def;

    // Carry colUsed to xFilter per-plan (never on the shared vtab) so a
    // projection builder can skip unused columns without self-join clobbering.
    if (def->projection_fn) {
        char *s = sqlite3_mprintf("%llu", (unsigned long long)info->colUsed);
        if (s) {
            info->idxStr = s;
            info->needToFreeIdxStr = 1;
        }
    }

    const cached_filter *best_filter = NULL;
    int best_filter_constraint = -1;
    int best_index_pos = -1;
    int best_index_constraint = -1;
    int best_rowid_constraint = -1;
    double best_cost = 1e9;

    // Range-pushdown candidate: the lower/upper bound constraints that target a
    // single indexed column. SQLite delivers `x BETWEEN a AND b` as a GE + an LE
    // on the same column, so a lower and an upper bound on the SAME indexed column
    // are consumed together (two argv slots). A range plan is only chosen when NO
    // equality index/filter won (equality is strictly cheaper).
    int range_index_pos = -1;         // indexed column carrying the range
    int range_low_constraint = -1;
    int range_high_constraint = -1;
    bool range_low_strict = false;    // GT (>) vs GE (>=)
    bool range_high_strict = false;   // LT (<) vs LE (<=)
    // A range is only served off a sorted view, which needs the cache: shared
    // tables (engine-lifetime) or query-scoped tables with a cache_builder.
    const bool range_eligible = def->use_shared_cache || has_cache_builder(def);

    for (int i = 0; i < info->nConstraint; ++i) {
        const struct sqlite3_index_constraint *c = &info->aConstraint[i];
        if (!c->usable) {
            continue;
        }
        int op = c->op;
        bool is_eq = (op == SQLITE_INDEX_CONSTRAINT_EQ);
        bool is_like = (op == SQLITE_INDEX_CONSTRAINT_LIKE ||
                        op == SQLITE_INDEX_CONSTRAINT_GLOB);
        bool is_low = (op == SQLITE_INDEX_CONSTRAINT_GE ||
                       op == SQLITE_INDEX_CONSTRAINT_GT);
        bool is_high = (op == SQLITE_INDEX_CONSTRAINT_LE ||
                        op == SQLITE_INDEX_CONSTRAINT_LT);

        // Range (>=, >, <=, <) on an indexed column: record the tightest bound(s)
        // for a single indexed column. Only pursued if no equality lookup wins.
        if ((is_low || is_high) && range_eligible && c->iColumn >= 0) {
            int idx_pos = find_index(def, c->iColumn);
            if (idx_pos >= 0 &&
                (range_index_pos < 0 || range_index_pos == idx_pos)) {
                range_index_pos = idx_pos;
                if (is_low && range_low_constraint < 0) {
                    range_low_constraint = i;
                    range_low_strict = (op == SQLITE_INDEX_CONSTRAINT_GT);
                } else if (is_high && range_high_constraint < 0) {
                    range_high_constraint = i;
                    range_high_strict = (op == SQLITE_INDEX_CONSTRAINT_LT);
                }
            }
            continue;
        }

        if (!is_eq && !is_like) {
            continue;
        }
        if (c->iColumn < 0) {
            if (is_eq) {
                best_rowid_constraint = i;  // rowid via equality only
            }
            continue;
        }
        if (is_like) {
            const cached_filter *f = find_filter(def, c->iColumn, op);
            if (f && f->cost < best_cost && like_has_usable_prefix(info, i)) {
                best_filter = f;
                best_filter_constraint = i;
                best_cost = f->cost;
            }
            continue;
        }
        // Equality: filter, then index.
        const cached_filter *f =
            find_filter(def, c->iColumn, SQLITE_INDEX_CONSTRAINT_EQ);
        if (f && f->cost < best_cost) {
            best_filter = f;
            best_filter_constraint = i;
            best_cost = f->cost;
        }
        // Query-scoped tables also use the index: xFilter builds a per-cursor hash
        // index on a cache_builder-backed cache (built once, reused across a JOIN).
        int idx_pos = (def->use_shared_cache || has_cache_builder(def))
                          ? find_index(def, c->iColumn) : -1;
        if (idx_pos >= 0) {
            if (def->use_shared_cache) {
                // Hash lookup on the already-built shared cache is near-free:
                // compete by cost and displace a costlier filter.
                double index_cost = 1.0;
                if (index_cost < best_cost) {
                    best_index_pos = idx_pos;
                    best_index_constraint = i;
                    best_cost = index_cost;
                    best_filter = NULL;  // index beats costlier filter
                }
            } else if (best_index_pos < 0) {
                // Query-scoped: the index scan must first BUILD the per-cursor
                // cache (a full engine read), so it never outranks an explicit
                // filter (a targeted engine read). Recorded as the fallback that
                // beats a full scan; chosen below only when no filter matched.
                best_index_pos = idx_pos;
                best_index_constraint = i;
            }
        }
    }

    size_t est = def->estimate_fn ? def->estimate_fn(def->estimate_user) : 1000;

    if (info->nConstraint == 0 && info->colUsed == 0 &&
        supports_count_only_scan(def)) {
        info->idxNum = CT_COUNT_ONLY_SCAN;
        info->estimatedCost = (double)est;
        info->estimatedRows = (sqlite3_int64)est;
    } else if (best_rowid_constraint >= 0) {
        info->aConstraintUsage[best_rowid_constraint].argvIndex = 1;
        info->aConstraintUsage[best_rowid_constraint].omit = 1;
        info->idxNum = CT_ROWID_FILTER;
        info->estimatedCost = 1.0;
        info->estimatedRows = 1;
    } else if (best_index_pos >= 0 && best_index_constraint >= 0 &&
               !best_filter) {
        info->aConstraintUsage[best_index_constraint].argvIndex = 1;
        info->aConstraintUsage[best_index_constraint].omit = 1;
        info->idxNum = CT_INDEX_BASE + best_index_pos;
        info->estimatedCost = 1.0;
        info->estimatedRows = 5;
    } else if (best_filter && best_filter_constraint >= 0) {
        info->aConstraintUsage[best_filter_constraint].argvIndex = 1;
        // EQ filters are trusted (omit); LIKE/GLOB stay for SQLite to re-check.
        info->aConstraintUsage[best_filter_constraint].omit =
            (sqlite_op(best_filter->op) == SQLITE_INDEX_CONSTRAINT_EQ) ? 1 : 0;
        info->idxNum = best_filter->filter_id;
        info->estimatedCost = best_filter->cost;
        info->estimatedRows = (sqlite3_int64)best_filter->est_rows;
    } else if (range_index_pos >= 0 &&
               (range_low_constraint >= 0 || range_high_constraint >= 0)) {
        // Range plan on an indexed column: cheaper than a full scan (a sorted-view
        // window), pricier than an EQ point lookup. A two-bound (BETWEEN) window
        // is tighter than a one-bound half-scan, so it costs less. The bounds are
        // handled EXACTLY by the sorted-view carve (strict/inclusive honored), so
        // the consumed constraints are omitted (SQLite need not re-check them).
        int range_flags = 0;
        int argv_slot = 1;
        if (range_low_constraint >= 0) {
            range_flags |= XSQLC_RANGE_HAS_LOW;
            if (range_low_strict) range_flags |= XSQLC_RANGE_LOW_STRICT;
            info->aConstraintUsage[range_low_constraint].argvIndex = argv_slot++;
            info->aConstraintUsage[range_low_constraint].omit = 1;
        }
        if (range_high_constraint >= 0) {
            range_flags |= XSQLC_RANGE_HAS_HIGH;
            if (range_high_strict) range_flags |= XSQLC_RANGE_HIGH_STRICT;
            info->aConstraintUsage[range_high_constraint].argvIndex = argv_slot++;
            info->aConstraintUsage[range_high_constraint].omit = 1;
        }
        bool two_bound =
            (range_low_constraint >= 0 && range_high_constraint >= 0);
        // Fraction-of-scan heuristic: a two-sided window selects ~1/4 of rows, a
        // one-sided ~1/2. Always strictly below the full-scan cost so a pushed
        // range beats the scan, and above the EQ point-lookup cost of 1.0.
        double frac = two_bound ? 0.25 : 0.5;
        double full_cost = (double)est;
        double range_cost = 2.0 + full_cost * frac;
        // Keep the range strictly cheaper than a full scan even on tiny tables so
        // the planner prefers the pushed window (and the plan reports a virtual
        // index, not INDEX 0). Never dip to/below the EQ point-lookup cost (1.0).
        if (range_cost >= full_cost) {
            double floor_cost = full_cost * 0.75;
            range_cost = floor_cost > 1.5 ? floor_cost : 1.5;
        }
        info->idxNum = CT_RANGE_BASE + range_index_pos * CT_RANGE_STRIDE +
                       range_flags;
        info->estimatedCost = range_cost;
        info->estimatedRows =
            (sqlite3_int64)(est > 0 ? (size_t)((double)est * frac) + 1 : 1);
    } else {
        info->idxNum = CT_FILTER_NONE;
        info->estimatedCost = (double)est;
        info->estimatedRows = (sqlite3_int64)est;
    }
    return SQLITE_OK;
}

// ---------------------------------------------------------------------------
// xUpdate: the reconstruction decision tree (A11/A12/A15)
// ---------------------------------------------------------------------------

static int cached_delete(CachedVtab *vtab, sqlite3_value **argv) {
    const xsql_cached_table *def = vtab->def;
    if (!def->supports_delete || !def->delete_fn) {
        return xsqlc_unsupported_write(&vtab->base, def->name, 'D');
    }
    int64_t raw = sqlite3_value_int64(argv[0]);
    size_t rowid = raw >= 0 ? (size_t)raw : (size_t)-1;
    shared_cache *shared = def->shared;
    void *temp = xsqlc_malloc(def->row_size);
    if (!temp) {
        return SQLITE_NOMEM;
    }
    void *row_ptr = NULL;
    bool rbr = reconstruct_by_rowid(def);
    // Query-scoped tables: freeze the pre-mutation cache for this statement.
    if (!ensure_query_scoped_mutation_snapshot(def, vtab)) {
        xsqlc_free(temp);
        return xsqlc_vtab_return_error(&vtab->base);
    }

    if (rbr && !def->rowid_fn && def->use_shared_cache && shared->built &&
        raw >= 0 && rowid < shared->data.count) {
        row_ptr = xsqlc_rowbuf_at(&shared->data, rowid);
    } else if (rbr && !def->rowid_fn && def->use_shared_cache &&
               shared->mutation_snapshot && !def->lookup_fn && raw >= 0 &&
               rowid < shared->data.count) {
        row_ptr = xsqlc_rowbuf_at(&shared->data, rowid);
    } else if (def->lookup_fn && def->lookup_fn(temp, raw, def->lookup_user)) {
        row_ptr = temp;
    } else if (!def->use_shared_cache && vtab->mutation_snapshot_valid &&
               raw >= 0 && rowid < vtab->mutation_snapshot.count) {
        // Query-scoped snapshot: stable pre-mutation row for a multi-row DELETE.
        // INVARIANT: `rowid` here is the positional index into the snapshot that
        // xColumn/xRowid emitted for THIS statement's scan. It is only a valid
        // cache position for the scan-driven path gated by
        // query_scoped_uses_mutation_snapshot() (no lookup_fn / rowid_fn /
        // filters). INSTEAD-OF/trigger writes that supply an out-of-band rowid do
        // NOT reconstruct here — they fall through to the builder branch below or
        // to SQLITE_ERROR — so a foreign rowid can never index the snapshot.
        row_ptr = xsqlc_rowbuf_at(&vtab->mutation_snapshot, rowid);
    } else if (!def->rowid_fn && !def->use_shared_cache && has_cache_builder(def) &&
               raw >= 0) {
        rowbuf rows;
        memset(&rows, 0, sizeof(rows));
        rows.row_size = def->row_size;
        struct xsql_rowset rs = {&rows};
        xsqlc_vtab_clear_error();
        invoke_cache_builder(def, vtab, &rs);
        // Gate on a builder error BEFORE indexing `rows` — a partial rebuild is
        // divergent, so trusting its count could DELETE the wrong row. Surface the
        // real error instead of a bare SQLITE_ERROR (matches the snapshot path and
        // the shared-cache builder gate).
        if (xsqlc_vtab_has_error()) {
            xsqlc_rowbuf_free(&rows);
            xsqlc_free(temp);
            return xsqlc_vtab_return_error(&vtab->base);
        }
        if (rowid < rows.count) {
            memcpy(temp, xsqlc_rowbuf_at(&rows, rowid), def->row_size);
            row_ptr = temp;
        }
        xsqlc_rowbuf_free(&rows);
    }

    if (!row_ptr) {
        xsqlc_free(temp);
        return SQLITE_ERROR;
    }
    xsqlc_transaction_touch(&vtab->transaction);
    fire_modify(def->before_modify_fn, def->before_user, "DELETE FROM ",
                def->name);
    // Surface the callback's reason (xsql_vtab_set_error) instead of SQLite's
    // generic "SQL logic error", as the INSERT/UPDATE paths already do.
    xsqlc_vtab_clear_error();
    bool ok = def->delete_fn(row_ptr, def->delete_user);
    xsqlc_free(temp);
    if (!ok) {
        return xsqlc_vtab_return_error(&vtab->base);
    }
    if (def->use_shared_cache) {
        xsqlc_cache_invalidate_after_mutation(def);
    }
    fire_modify(def->after_modify_fn, def->after_user, "DELETE FROM ", def->name);
    return SQLITE_OK;
}

// Per-column accessors for the shared UPDATE apply path (xsqlc_apply_update_columns).
typedef struct {
    const xsql_cached_table *def;
    void                    *row;
} cached_update_ctx;
static bool cached_update_writable(void *ctx, size_t c) {
    const cached_column *col = &((const cached_update_ctx *)ctx)->def->columns[c];
    return col->writable && col->set_kind != CSET_NONE;
}
static const char *cached_update_name(void *ctx, size_t c) {
    return ((const cached_update_ctx *)ctx)->def->columns[c].name;
}
static bool cached_update_apply(void *ctx, size_t c, sqlite3_value *val) {
    const cached_update_ctx *x = (const cached_update_ctx *)ctx;
    return column_set(&x->def->columns[c], x->row, (xsql_value *)val);
}

static int cached_update(CachedVtab *vtab, int argc, sqlite3_value **argv) {
    const xsql_cached_table *def = vtab->def;
    int64_t raw = sqlite3_value_int64(argv[0]);
    size_t old_rowid = raw >= 0 ? (size_t)raw : (size_t)-1;

    bool has_writable = false;
    for (size_t i = 0; i < def->ncols; ++i) {
        if (def->columns[i].writable && def->columns[i].set_kind != CSET_NONE) {
            has_writable = true;
            break;
        }
    }
    if (!has_writable) {
        return xsqlc_unsupported_write(&vtab->base, def->name, 'U');
    }
    // Query-scoped tables: freeze the pre-mutation cache for this statement.
    // Runs before the before_modify hook (same order as DELETE) so a failed
    // snapshot aborts before any observable side effect.
    if (!ensure_query_scoped_mutation_snapshot(def, vtab)) {
        return xsqlc_vtab_return_error(&vtab->base);
    }
    xsqlc_transaction_touch(&vtab->transaction);
    fire_modify(def->before_modify_fn, def->before_user, "UPDATE ", def->name);

    shared_cache *shared = def->shared;
    void *temp = xsqlc_malloc(def->row_size);
    if (!temp) {
        return SQLITE_NOMEM;
    }
    void *row_ptr = NULL;
    bool rbr = reconstruct_by_rowid(def);

    if (rbr && !def->rowid_fn && def->use_shared_cache && shared->built &&
        raw >= 0 && old_rowid < shared->data.count) {
        row_ptr = xsqlc_rowbuf_at(&shared->data, old_rowid);
    } else if (rbr && !def->rowid_fn && def->use_shared_cache &&
               shared->mutation_snapshot && !def->lookup_fn && raw >= 0 &&
               old_rowid < shared->data.count) {
        row_ptr = xsqlc_rowbuf_at(&shared->data, old_rowid);
    } else if (rbr && def->lookup_fn &&
               def->lookup_fn(temp, raw, def->lookup_user)) {
        row_ptr = temp;
    } else if (!rbr && def->populator_fn) {
        // NOCHANGE off: unchanged identity columns carry real values in argv.
        def->populator_fn(temp, argc, (xsql_value **)argv, def->populator_user);
        row_ptr = temp;
    } else if (!def->use_shared_cache && vtab->mutation_snapshot_valid &&
               raw >= 0 && old_rowid < vtab->mutation_snapshot.count) {
        // Query-scoped snapshot: stable pre-mutation row for a multi-row UPDATE.
        // Same INVARIANT as cached_delete: `old_rowid` is the positional index
        // this statement's scan emitted into the snapshot, valid only on the
        // scan-driven path (query_scoped_uses_mutation_snapshot()). An INSTEAD-OF/
        // trigger UPDATE with a foreign rowid does not reach this branch.
        row_ptr = xsqlc_rowbuf_at(&vtab->mutation_snapshot, old_rowid);
    } else if (!def->update_from_column_values && !def->rowid_fn &&
               !def->use_shared_cache && has_cache_builder(def) && raw >= 0) {
        rowbuf rows;
        memset(&rows, 0, sizeof(rows));
        rows.row_size = def->row_size;
        struct xsql_rowset rs = {&rows};
        xsqlc_vtab_clear_error();
        invoke_cache_builder(def, vtab, &rs);
        // Gate on a builder error BEFORE indexing `rows` — a partial rebuild is
        // divergent, so trusting its count could UPDATE the wrong row. Surface the
        // real error instead of a bare SQLITE_ERROR (matches the snapshot path and
        // the shared-cache builder gate).
        if (xsqlc_vtab_has_error()) {
            xsqlc_rowbuf_free(&rows);
            xsqlc_free(temp);
            return xsqlc_vtab_return_error(&vtab->base);
        }
        if (old_rowid < rows.count) {
            memcpy(temp, xsqlc_rowbuf_at(&rows, old_rowid), def->row_size);
            row_ptr = temp;
        }
        xsqlc_rowbuf_free(&rows);
    } else if (def->populator_fn) {
        def->populator_fn(temp, argc, (xsql_value **)argv, def->populator_user);
        row_ptr = temp;
    } else if (def->lookup_fn && def->lookup_fn(temp, raw, def->lookup_user)) {
        row_ptr = temp;
    } else {
        xsqlc_free(temp);
        return SQLITE_READONLY;
    }

    // reconstruct_by_rowid gates the read-only-write rejection: on argv-reconstruct
    // tables the unchanged identity columns carry real values, so don't reject them.
    cached_update_ctx uctx = {def, row_ptr};
    xsqlc_update_ops ops = {cached_update_writable, cached_update_name,
                            cached_update_apply, &uctx};
    if (!xsqlc_apply_update_columns(argc, argv, def->ncols, rbr, &ops)) {
        xsqlc_free(temp);
        return xsqlc_vtab_return_error(&vtab->base);
    }
    xsqlc_free(temp);
    if (def->use_shared_cache) {
        xsqlc_cache_invalidate_after_mutation(def);
    }
    fire_modify(def->after_modify_fn, def->after_user, "UPDATE ", def->name);
    return SQLITE_OK;
}

static int cached_insert(CachedVtab *vtab, int argc, sqlite3_value **argv) {
    const xsql_cached_table *def = vtab->def;
    if (!def->supports_insert || !def->insert_fn) {
        return xsqlc_unsupported_write(&vtab->base, def->name, 'I');
    }
    xsqlc_transaction_touch(&vtab->transaction);
    fire_modify(def->before_modify_fn, def->before_user, "INSERT INTO ",
                def->name);
    xsqlc_vtab_clear_error();
    if (!def->insert_fn(argc - 2, (xsql_value **)&argv[2], def->insert_user)) {
        return xsqlc_vtab_return_error(&vtab->base);
    }
    xsqlc_vtab_clear_error();
    xsqlc_cache_invalidate(def);  // full clear (not a snapshot)
    fire_modify(def->after_modify_fn, def->after_user, "INSERT INTO ", def->name);
    return SQLITE_OK;
}

static int cached_update_dispatch(sqlite3_vtab *pVtab, int argc,
                                  sqlite3_value **argv, sqlite3_int64 *pRowid) {
    (void)pRowid;
    CachedVtab *vtab = (CachedVtab *)pVtab;
    int rc;
    if (argc == 1 && sqlite3_value_type(argv[0]) != SQLITE_NULL) {
        rc = cached_delete(vtab, argv);
    } else if (argc > 1 && sqlite3_value_type(argv[0]) != SQLITE_NULL) {
        rc = cached_update(vtab, argc, argv);
    } else if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        rc = cached_insert(vtab, argc, argv);
    } else {
        rc = SQLITE_READONLY;
    }
    if (rc == SQLITE_OK) {
        xsqlc_transaction_write(&vtab->transaction);
    }
    return rc;
}

static int cached_begin(sqlite3_vtab *pVtab) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    return xsqlc_transaction_begin(pVtab, &vtab->transaction);
}
static int cached_prepare(sqlite3_vtab *pVtab) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    return xsqlc_transaction_prepare(pVtab, &vtab->transaction);
}
static int cached_commit(sqlite3_vtab *pVtab) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    return xsqlc_transaction_commit(pVtab, &vtab->transaction);
}
static int cached_rollback(sqlite3_vtab *pVtab) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    return xsqlc_transaction_rollback(pVtab, &vtab->transaction);
}
static int cached_savepoint(sqlite3_vtab *pVtab, int savepoint) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    return xsqlc_transaction_savepoint(
        pVtab, &vtab->transaction, savepoint);
}
static int cached_release(sqlite3_vtab *pVtab, int savepoint) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    return xsqlc_transaction_release(
        pVtab, &vtab->transaction, savepoint);
}
static int cached_rollback_to(sqlite3_vtab *pVtab, int savepoint) {
    CachedVtab *vtab = (CachedVtab *)pVtab;
    return xsqlc_transaction_rollback_to(
        pVtab, &vtab->transaction, savepoint);
}

static sqlite3_module g_cached_module = {
    .iVersion = 3,
    .xCreate = cached_connect,
    .xConnect = cached_connect,
    .xBestIndex = cached_best_index,
    .xDisconnect = cached_disconnect,
    .xDestroy = cached_destroy,
    .xOpen = cached_open,
    .xClose = cached_close,
    .xFilter = cached_xfilter,
    .xNext = cached_next,
    .xEof = cached_eof,
    .xColumn = cached_xcolumn,
    .xRowid = cached_rowid,
    .xUpdate = cached_update_dispatch,
    .xBegin = cached_begin,
    .xSync = cached_prepare,
    .xCommit = cached_commit,
    .xRollback = cached_rollback,
    .xSavepoint = cached_savepoint,
    .xRelease = cached_release,
    .xRollbackTo = cached_rollback_to,
};

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

xsql_cached_table *xsql_cached_table_new(const char *name, size_t row_size) {
    xsql_cached_table *t = (xsql_cached_table *)xsqlc_malloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->row_size = row_size;
    t->use_shared_cache = true;
    t->name = xsqlc_strdup(name ? name : "");
    if (!t->name) {
        xsqlc_free(t);
        return NULL;
    }
    t->shared = xsqlc_shared_cache_new(row_size);
    if (!t->shared) {
        xsqlc_free(t->name);
        xsqlc_free(t);
        return NULL;
    }
    return t;
}

void xsql_cached_table_free(xsql_cached_table *c) {
    if (!c) {
        return;
    }
    for (size_t i = 0; i < c->ncols; ++i) {
        xsqlc_free(c->columns[i].name);
    }
    xsqlc_free(c->columns);
    xsqlc_free(c->filters);
    xsqlc_free(c->index_defs);
    xsqlc_shared_cache_free(c->shared);
    xsqlc_free(c->name);
    xsqlc_free(c);
}

void xsql_cached_estimate_rows(xsql_cached_table *c, xsql_estimate_rows_fn fn,
                               void *user) {
    if (c) {
        c->estimate_fn = fn;
        c->estimate_user = user;
    }
}
void xsql_cached_count(xsql_cached_table *c, xsql_row_count_fn fn, void *user) {
    if (c) {
        c->count_fn = fn;
        c->count_user = user;
    }
}
void xsql_cached_cache_builder(xsql_cached_table *c, xsql_cache_builder_fn fn,
                               void *user) {
    if (c) {
        c->cache_builder_fn = fn;
        c->cache_builder_user = user;
    }
}
void xsql_cached_stateful_cache_builder(
    xsql_cached_table *c, xsql_stateful_cache_builder_fn fn, void *user) {
    if (c) {
        c->stateful_cache_builder_fn = fn;
        c->stateful_cache_builder_user = user;
        c->use_shared_cache = false;
    }
}
void xsql_cached_projection_cache_builder(xsql_cached_table *c,
                                          xsql_projection_cache_builder_fn fn,
                                          void *user) {
    if (c) {
        c->projection_fn = fn;
        c->projection_user = user;
    }
}
void xsql_cached_no_shared_cache(xsql_cached_table *c) {
    if (c) {
        c->use_shared_cache = false;
    }
}
void xsql_cached_on_modify(xsql_cached_table *c, xsql_cached_modify_fn fn,
                           void *user) {
    if (c) {
        c->before_modify_fn = fn;
        c->before_user = user;
    }
}
void xsql_cached_after_modify(xsql_cached_table *c, xsql_cached_modify_fn fn,
                              void *user) {
    if (c) {
        c->after_modify_fn = fn;
        c->after_user = user;
    }
}
int xsql_cached_transaction_hooks(
    xsql_cached_table *c, const xsql_transaction_hooks *hooks, void *user) {
    if (!c || (hooks && hooks->struct_size != sizeof(*hooks))) {
        return XSQL_MISUSE;
    }
    memset(&c->transaction_hooks, 0, sizeof(c->transaction_hooks));
    c->transaction_user = NULL;
    if (hooks) {
        c->transaction_hooks = *hooks;
        c->transaction_user = user;
    }
    return XSQL_OK;
}

static cached_column *push_column(xsql_cached_table *t, const char *name,
                                  xsql_column_type type) {
    if (!xsqlc_vec_reserve((void **)&t->columns, &t->cols_cap, t->ncols + 1,
                           sizeof(cached_column))) {
        return NULL;
    }
    cached_column *c = &t->columns[t->ncols];
    memset(c, 0, sizeof(*c));
    c->name = xsqlc_strdup(name ? name : "");
    if (!c->name) {
        return NULL;
    }
    c->type = type;
    c->set_kind = CSET_NONE;
    t->ncols++;
    return c;
}

int xsql_cached_col(xsql_cached_table *c, const char *name, xsql_column_type type,
                    xsql_crow_emit_fn get, void *user) {
    if (!c) {
        return XSQL_MISUSE;
    }
    cached_column *col = push_column(c, name, type);
    if (!col) {
        return XSQL_NO_MEMORY;
    }
    col->get_kind = CGET_EMIT;
    col->get.emit = get;
    col->user = user;
    return XSQL_OK;
}

#define CACHED_COL_TYPED(fnname, coltype, getkind, getfield, gettype)          \
    int fnname(xsql_cached_table *c, const char *name, gettype get,            \
               void *user) {                                                   \
        if (!c) {                                                              \
            return XSQL_MISUSE;                                                \
        }                                                                      \
        cached_column *col = push_column(c, name, coltype);                    \
        if (!col) {                                                            \
            return XSQL_NO_MEMORY;                                             \
        }                                                                      \
        col->get_kind = getkind;                                              \
        col->get.getfield = get;                                              \
        col->user = user;                                                      \
        return XSQL_OK;                                                        \
    }

CACHED_COL_TYPED(xsql_cached_col_int, XSQL_COL_INTEGER, CGET_INT, i,
                 xsql_crow_int_fn)
CACHED_COL_TYPED(xsql_cached_col_i64, XSQL_COL_INTEGER, CGET_I64, i64,
                 xsql_crow_i64_fn)
CACHED_COL_TYPED(xsql_cached_col_double, XSQL_COL_REAL, CGET_DOUBLE, d,
                 xsql_crow_double_fn)
CACHED_COL_TYPED(xsql_cached_col_text, XSQL_COL_TEXT, CGET_EMIT, emit,
                 xsql_crow_emit_fn)
CACHED_COL_TYPED(xsql_cached_col_blob, XSQL_COL_BLOB, CGET_EMIT, emit,
                 xsql_crow_emit_fn)

// Read-only nullable registrars: the emit getter may emit a value or SQL NULL.
// Getter-only siblings of xsql_cached_col_{int,text}_nullable_rw.
int xsql_cached_col_int_nullable(xsql_cached_table *c, const char *name,
                                 xsql_crow_emit_fn get, void *user) {
    if (!c) {
        return XSQL_MISUSE;
    }
    cached_column *col = push_column(c, name, XSQL_COL_INTEGER);
    if (!col) {
        return XSQL_NO_MEMORY;
    }
    col->get_kind = CGET_EMIT;
    col->get.emit = get;
    col->user = user;
    return XSQL_OK;
}
int xsql_cached_col_text_nullable(xsql_cached_table *c, const char *name,
                                  xsql_crow_emit_fn get, void *user) {
    if (!c) {
        return XSQL_MISUSE;
    }
    cached_column *col = push_column(c, name, XSQL_COL_TEXT);
    if (!col) {
        return XSQL_NO_MEMORY;
    }
    col->get_kind = CGET_EMIT;
    col->get.emit = get;
    col->user = user;
    return XSQL_OK;
}

// Read/write registrars. `get_kind`/`get` set the getter; `set_kind`/`set` the
// setter. `coltype` is the declared SQL type.
static cached_column *push_rw_column(xsql_cached_table *c, const char *name,
                                     xsql_column_type type, int get_kind,
                                     void *user) {
    if (!c) {
        return NULL;
    }
    cached_column *col = push_column(c, name, type);
    if (!col) {
        return NULL;
    }
    col->get_kind = get_kind;
    col->writable = true;
    col->user = user;
    return col;
}

int xsql_cached_col_int_rw(xsql_cached_table *c, const char *name,
                           xsql_crow_int_fn get, xsql_crow_set_int_fn set,
                           void *user) {
    cached_column *col = push_rw_column(c, name, XSQL_COL_INTEGER, CGET_INT, user);
    if (!col) {
        return c ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.i = get;
    col->set_kind = CSET_INT;
    col->set.i = set;
    return XSQL_OK;
}
int xsql_cached_col_i64_rw(xsql_cached_table *c, const char *name,
                           xsql_crow_i64_fn get, xsql_crow_set_i64_fn set,
                           void *user) {
    cached_column *col = push_rw_column(c, name, XSQL_COL_INTEGER, CGET_I64, user);
    if (!col) {
        return c ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.i64 = get;
    col->set_kind = CSET_I64;
    col->set.i64 = set;
    return XSQL_OK;
}
int xsql_cached_col_text_rw(xsql_cached_table *c, const char *name,
                            xsql_crow_emit_fn get, xsql_crow_set_text_fn set,
                            void *user) {
    cached_column *col = push_rw_column(c, name, XSQL_COL_TEXT, CGET_EMIT, user);
    if (!col) {
        return c ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = CSET_TEXT;
    col->set.text = set;
    return XSQL_OK;
}
int xsql_cached_col_text_nullable_rw(xsql_cached_table *c, const char *name,
                                     xsql_crow_emit_fn get,
                                     xsql_crow_set_value_fn set, void *user) {
    cached_column *col = push_rw_column(c, name, XSQL_COL_TEXT, CGET_EMIT, user);
    if (!col) {
        return c ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = CSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_cached_col_int_nullable_rw(xsql_cached_table *c, const char *name,
                                    xsql_crow_emit_fn get,
                                    xsql_crow_set_value_fn set, void *user) {
    cached_column *col =
        push_rw_column(c, name, XSQL_COL_INTEGER, CGET_EMIT, user);
    if (!col) {
        return c ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = CSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_cached_col_int_rw_value(xsql_cached_table *c, const char *name,
                                 xsql_crow_int_fn get, xsql_crow_set_value_fn set,
                                 void *user) {
    cached_column *col = push_rw_column(c, name, XSQL_COL_INTEGER, CGET_INT, user);
    if (!col) {
        return c ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.i = get;
    col->set_kind = CSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_cached_col_i64_rw_value(xsql_cached_table *c, const char *name,
                                 xsql_crow_i64_fn get, xsql_crow_set_value_fn set,
                                 void *user) {
    cached_column *col = push_rw_column(c, name, XSQL_COL_INTEGER, CGET_I64, user);
    if (!col) {
        return c ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.i64 = get;
    col->set_kind = CSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_cached_col_text_rw_value(xsql_cached_table *c, const char *name,
                                  xsql_crow_emit_fn get,
                                  xsql_crow_set_value_fn set, void *user) {
    cached_column *col = push_rw_column(c, name, XSQL_COL_TEXT, CGET_EMIT, user);
    if (!col) {
        return c ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = CSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}

static int add_filter(xsql_cached_table *c, const char *column, double cost,
                      double est_rows, xsql_filter_op op,
                      xsql_filter_factory_fn factory, void *user) {
    if (!c) {
        return XSQL_MISUSE;
    }
    int col = find_column(c, column);
    if (col < 0) {
        return XSQL_NOT_FOUND;
    }
    if (!xsqlc_vec_reserve((void **)&c->filters, &c->filters_cap, c->nfilters + 1,
                           sizeof(cached_filter))) {
        return XSQL_NO_MEMORY;
    }
    cached_filter *f = &c->filters[c->nfilters];
    f->column_index = col;
    f->filter_id = (int)c->nfilters + 1;
    f->cost = cost;
    f->est_rows = est_rows;
    f->op = op;
    f->factory = factory;
    f->user = user;
    c->nfilters++;
    return XSQL_OK;
}

int xsql_cached_filter_eq(xsql_cached_table *c, const char *column, double cost,
                          double est_rows, xsql_filter_factory_fn factory,
                          void *user) {
    return add_filter(c, column, cost, est_rows, XSQL_FILTER_EQ, factory, user);
}
int xsql_cached_filter_eq_text(xsql_cached_table *c, const char *column,
                               double cost, double est_rows,
                               xsql_filter_factory_fn factory, void *user) {
    return add_filter(c, column, cost, est_rows, XSQL_FILTER_EQ, factory, user);
}
int xsql_cached_filter_prefix(xsql_cached_table *c, const char *column,
                              double cost, double est_rows,
                              xsql_filter_factory_fn factory, void *user) {
    return add_filter(c, column, cost, est_rows, XSQL_FILTER_LIKE, factory, user);
}

int xsql_cached_index_on(xsql_cached_table *c, const char *column,
                         xsql_key_extractor_fn key, void *user) {
    if (!c) {
        return XSQL_MISUSE;
    }
    int col = find_column(c, column);
    if (col < 0) {
        return XSQL_NOT_FOUND;
    }
    if (!xsqlc_vec_reserve((void **)&c->index_defs, &c->index_cap,
                           c->nindexes + 1, sizeof(cached_index_def))) {
        return XSQL_NO_MEMORY;
    }
    cached_index_def *ix = &c->index_defs[c->nindexes];
    ix->column_index = col;
    ix->key = key;
    ix->user = user;
    c->nindexes++;
    return XSQL_OK;
}

void xsql_cached_rowid(xsql_cached_table *c, xsql_cached_rowid_fn fn, void *user) {
    if (c) {
        c->rowid_fn = fn;
        c->rowid_user = user;
    }
}
void xsql_cached_row_lookup(xsql_cached_table *c, xsql_row_lookup_fn fn,
                            void *user) {
    if (c) {
        c->lookup_fn = fn;
        c->lookup_user = user;
    }
}
void xsql_cached_row_populator(xsql_cached_table *c, xsql_row_populator_fn fn,
                               void *user) {
    if (c) {
        c->populator_fn = fn;
        c->populator_user = user;
    }
}
void xsql_cached_update_from_column_values(xsql_cached_table *c, bool value) {
    if (c) {
        c->update_from_column_values = value;
    }
}
void xsql_cached_deletable(xsql_cached_table *c, xsql_crow_delete_fn fn,
                           void *user) {
    if (c) {
        c->supports_delete = true;
        c->delete_fn = fn;
        c->delete_user = user;
    }
}
void xsql_cached_insertable(xsql_cached_table *c, xsql_cached_insert_fn fn,
                            void *user) {
    if (c) {
        c->supports_insert = true;
        c->insert_fn = fn;
        c->insert_user = user;
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

int xsql_cached_table_register_module(struct xsql_db *db, xsql_cached_table *c) {
    if (!db || !db->db || !c || !xsqlc_is_valid_sql_identifier(c->name)) {
        return XSQL_MISUSE;
    }
    return sqlite3_create_module_v2(db->db, c->name, &g_cached_module, c, NULL);
}

int xsql_cached_table_register_as(struct xsql_db *db, xsql_cached_table *c,
                                  const char *table_name) {
    if (!db || !db->db || !c || !xsqlc_is_valid_sql_identifier(table_name)) {
        return XSQL_MISUSE;
    }
    int rc = sqlite3_create_module_v2(db->db, table_name, &g_cached_module, c,
                                      NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    rc = xsqlc_create_vtable_sqlite(db->db, table_name, table_name);
    return rc;
}

int xsql_cached_table_register(struct xsql_db *db, xsql_cached_table *c) {
    if (!db || !c) {
        return XSQL_MISUSE;
    }
    return xsql_cached_table_register_as(db, c, c->name);
}
