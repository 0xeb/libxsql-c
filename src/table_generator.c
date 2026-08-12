// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// table_generator.c — the generator virtual-table module: the sqlite3_module
// trampolines for a STREAMING (never-materialized) source, xBestIndex (constraint
// / parametric / single-column / missing-required / full-scan priority), xFilter
// (build the generator or iterator and prime it once), xColumn (the A8
// unconditional NOCHANGE early-return), xRowid, and the row_lookup-based
// UPDATE/DELETE/INSERT path, plus the builder + registrars. Shared vtab helpers
// live in vtab_core.c.

#include <libxsqlc/table_generator.h>

#include <libxsqlc/value.h>
#include <libxsqlc/vtab.h>

#include "internal/generator_internal.h"
#include "internal/internal.h"
#include "internal/strbuf.h"
#include "internal/transaction.h"
#include "internal/vec.h"
#include "internal/vtab_core.h"

#include <sqlite3.h>

#include <stdlib.h>
#include <string.h>

// Plan sentinels. MISSING_REQUIRED_CONSTRAINT is -3. Single-column filters get ids
// 1..n; parametric 500+n; constraint 2000+n.
#define GEN_FILTER_NONE 0
#define GEN_MISSING_REQUIRED_CONSTRAINT (-3)
#define GEN_COUNT_ONLY_SCAN (-4)
#define GEN_PARAMETRIC_FILTER_BASE 500
#define GEN_CONSTRAINT_FILTER_BASE 2000

// ---------------------------------------------------------------------------
// Wrappers
// ---------------------------------------------------------------------------

typedef struct {
    sqlite3_vtab                base;
    const xsql_generator_table *def;
    xsqlc_transaction transaction;
    struct xsql_db *owner;
    char           *schema_name;
    char           *table_name;
} GeneratorVtab;

typedef struct {
    sqlite3_vtab_cursor         base;
    const xsql_generator_table *def;

    xsql_generator gen;
    bool           has_gen, gen_eof;

    xsql_row_iter iter;
    bool          has_iter, using_iterator, iterator_eof;

    bool   count_only;
    size_t count_only_total;
    size_t count_only_position;
} GeneratorCursor;

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

static int find_column(const xsql_generator_table *t, const char *name) {
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

static int sqlite_constraint_op(xsql_constraint_op op) {
    switch (op) {
        case XSQL_CONSTRAINT_GT:   return SQLITE_INDEX_CONSTRAINT_GT;
        case XSQL_CONSTRAINT_LE:   return SQLITE_INDEX_CONSTRAINT_LE;
        case XSQL_CONSTRAINT_LT:   return SQLITE_INDEX_CONSTRAINT_LT;
        case XSQL_CONSTRAINT_GE:   return SQLITE_INDEX_CONSTRAINT_GE;
        case XSQL_CONSTRAINT_LIKE: return SQLITE_INDEX_CONSTRAINT_LIKE;
        case XSQL_CONSTRAINT_EQ:
        default:                   return SQLITE_INDEX_CONSTRAINT_EQ;
    }
}

static void column_emit(const gen_column *c, xsql_context *ctx, const void *row) {
    switch (c->get_kind) {
        case GGET_I64:    xsql_result_i64(ctx, c->get.i64(row, c->user)); break;
        case GGET_INT:    xsql_result_int(ctx, c->get.i(row, c->user)); break;
        case GGET_DOUBLE: xsql_result_double(ctx, c->get.d(row, c->user)); break;
        case GGET_EMIT:   c->get.emit(ctx, row, c->user); break;
        case GGET_NULL:
        default:          xsql_result_null(ctx); break;
    }
}

static bool column_set(const gen_column *c, void *row, xsql_value *val) {
    switch (c->set_kind) {
        case GSET_I64: return c->set.i64(row, xsql_value_i64(val), c->user);
        case GSET_INT: return c->set.i(row, xsql_value_int(val), c->user);
        case GSET_TEXT: {
            const char *tx = xsql_value_text(val);
            return c->set.text(row, tx ? tx : "", c->user);
        }
        case GSET_VALUE: return c->set.value(row, val, c->user);
        default:         return false;
    }
}

static void fire_modify(xsql_generator_modify_fn fn, void *user, const char *verb,
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

static bool table_has_writable(const xsql_generator_table *def) {
    for (size_t i = 0; i < def->ncols; ++i) {
        if (def->columns[i].writable && def->columns[i].set_kind != GSET_NONE) {
            return true;
        }
    }
    return false;
}

static char *build_schema(const xsql_generator_table *t) {
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
        if (t->columns[i].hidden) {
            xsqlc_sb_append(&sb, " HIDDEN");
        }
    }
    xsqlc_sb_putc(&sb, ')');
    return xsqlc_sb_detach(&sb);
}

// ---------------------------------------------------------------------------
// Cursor source lifecycle
// ---------------------------------------------------------------------------

static void cursor_release_iter(GeneratorCursor *c) {
    if (c->has_iter && c->iter.destroy) {
        c->iter.destroy(c->iter.self);
    }
    memset(&c->iter, 0, sizeof(c->iter));
    c->has_iter = false;
}

static void cursor_release_gen(GeneratorCursor *c) {
    if (c->has_gen && c->gen.destroy) {
        c->gen.destroy(c->gen.self);
    }
    memset(&c->gen, 0, sizeof(c->gen));
    c->has_gen = false;
}

static void generator_reset_cursor(GeneratorCursor *cur) {
    cursor_release_iter(cur);
    cursor_release_gen(cur);
    cur->using_iterator = false;
    cur->iterator_eof = false;
    cur->gen_eof = false;
    cur->count_only = false;
    cur->count_only_total = 0;
    cur->count_only_position = 0;
}

// ---------------------------------------------------------------------------
// Trampolines
// ---------------------------------------------------------------------------

static int generator_connect(sqlite3 *db, void *pAux, int argc,
                             const char *const *argv, sqlite3_vtab **ppVtab,
                             char **pzErr) {
    (void)pzErr;
    const xsql_generator_table *def = (const xsql_generator_table *)pAux;
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
    GeneratorVtab *vtab = (GeneratorVtab *)xsqlc_malloc(sizeof(*vtab));
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
    const bool has_lookup = def->lookup_fn != NULL;
    rc = xsqlc_db_record_write_surface(
        vtab->owner, vtab->schema_name, vtab->table_name,
        def->supports_insert && def->insert_fn != NULL,
        def->supports_delete && def->delete_fn != NULL && has_lookup);
    if (rc == XSQL_OK && has_lookup) {
        for (size_t i = 0; i < def->ncols; ++i) {
            if (def->columns[i].writable &&
                def->columns[i].set_kind != GSET_NONE) {
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

static int generator_disconnect(sqlite3_vtab *pVtab) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    xsqlc_transaction_destroy(&vtab->transaction);
    xsqlc_free(vtab->schema_name);
    xsqlc_free(vtab->table_name);
    xsqlc_free(vtab);
    return SQLITE_OK;
}

static int generator_destroy(sqlite3_vtab *pVtab) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    xsqlc_db_remove_write_surface(
        vtab->owner, vtab->schema_name, vtab->table_name);
    return generator_disconnect(pVtab);
}

static int generator_open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    GeneratorCursor *cur = (GeneratorCursor *)xsqlc_malloc(sizeof(*cur));
    if (!cur) {
        return SQLITE_NOMEM;
    }
    memset(cur, 0, sizeof(*cur));
    cur->def = vtab->def;
    *ppCursor = &cur->base;
    return SQLITE_OK;
}

static int generator_close(sqlite3_vtab_cursor *pCursor) {
    GeneratorCursor *cur = (GeneratorCursor *)pCursor;
    cursor_release_iter(cur);
    cursor_release_gen(cur);
    xsqlc_free(cur);
    return SQLITE_OK;
}

static int generator_next(sqlite3_vtab_cursor *pCursor) {
    // Cooperative cancellation: a streaming source can iterate without bound, so
    // poll the deadline the query installed (Phase 7) and bail before advancing.
    if (xsql_vtab_interrupted()) {
        xsql_vtab_set_error("query interrupted: deadline exceeded");
        return xsqlc_vtab_return_error(pCursor->pVtab);
    }
    GeneratorCursor *cur = (GeneratorCursor *)pCursor;
    if (cur->count_only) {
        if (cur->count_only_position < cur->count_only_total) {
            ++cur->count_only_position;
        }
        return SQLITE_OK;
    }
    if (cur->using_iterator && cur->has_iter) {
        xsqlc_vtab_clear_error();
        if (!cur->iter.next(cur->iter.self)) {
            cur->iterator_eof = true;
        }
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
    } else if (cur->has_gen) {
        xsqlc_vtab_clear_error();
        if (!cur->gen.next(cur->gen.self)) {
            cur->gen_eof = true;
        }
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
    }
    return SQLITE_OK;
}

static int generator_eof(sqlite3_vtab_cursor *pCursor) {
    GeneratorCursor *cur = (GeneratorCursor *)pCursor;
    if (cur->count_only) {
        return cur->count_only_position >= cur->count_only_total ? 1 : 0;
    }
    if (cur->using_iterator) {
        if (!cur->has_iter || cur->iterator_eof) {
            return 1;
        }
        return cur->iter.eof(cur->iter.self) ? 1 : 0;
    }
    if (cur->has_gen) {
        return cur->gen_eof ? 1 : 0;
    }
    return 1;
}

static void prime_generator(GeneratorCursor *cur, xsql_generator *gen,
                            bool built);

// SQLite uses colUsed == 0 for both COUNT(*) and rowid-only scans. The cheap
// count plan is safe for COUNT(*) because SQLite never asks for a row value,
// but xRowid/xColumn must observe the real generator. Materialize lazily at the
// current phantom position when either callback is requested.
static int materialize_count_only_generator(GeneratorCursor *cur,
                                            uint64_t col_used) {
    if (!cur || !cur->count_only) {
        return SQLITE_OK;
    }

    const xsql_generator_table *def = cur->def;
    xsqlc_vtab_clear_error();
    xsql_generator gen;
    memset(&gen, 0, sizeof(gen));
    bool built = false;
    if (def->projection_generator_fn) {
        built = def->projection_generator_fn(
            col_used, &gen, def->projection_generator_user);
    } else if (def->generator_fn) {
        built = def->generator_fn(&gen, def->generator_user);
    }
    prime_generator(cur, &gen, built);
    if (xsqlc_vtab_has_error()) {
        cur->gen_eof = true;
        return xsqlc_vtab_return_error(cur->base.pVtab);
    }

    for (size_t row = 0;
         cur->has_gen && !cur->gen_eof &&
         row < cur->count_only_position;
         ++row) {
        xsqlc_vtab_clear_error();
        if (!cur->gen.next(cur->gen.self)) {
            cur->gen_eof = true;
        }
        if (xsqlc_vtab_has_error()) {
            cur->gen_eof = true;
            return xsqlc_vtab_return_error(cur->base.pVtab);
        }
    }

    cur->count_only = false;
    if (!cur->has_gen || cur->gen_eof) {
        xsql_vtab_set_error(
            "generator row_count() exceeded the rows produced by generator()");
        return xsqlc_vtab_return_error(cur->base.pVtab);
    }
    return SQLITE_OK;
}

static int generator_xcolumn(sqlite3_vtab_cursor *pCursor, sqlite3_context *ctx,
                             int col) {
    GeneratorCursor *cur = (GeneratorCursor *)pCursor;

    // A8: the generator reconstructs UPDATE targets from the rowid alone (via
    // row_lookup), so an unchanged column never needs a real value — report
    // NOCHANGE unconditionally (unlike the cached table's gated A7 path).
    // Returning without a value marks the column SQLITE_NOCHANGE in xUpdate.
    if (sqlite3_vtab_nochange(ctx)) {
        return SQLITE_OK;
    }

    const xsql_generator_table *def = cur->def;
    if (cur->count_only) {
        uint64_t col_used =
            col >= 0 && col < 63 ? (UINT64_C(1) << (unsigned)col)
                                 : (UINT64_C(1) << 63);
        int rc = materialize_count_only_generator(cur, col_used);
        if (rc != SQLITE_OK) return rc;
    }
    if (col < 0 || (size_t)col >= def->ncols) {
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
    } else if (cur->has_gen && !cur->gen_eof) {
        const void *row = cur->gen.current(cur->gen.self);
        if (row) {
            column_emit(&def->columns[col], xctx, row);
        } else {
            sqlite3_result_null(ctx);
        }
    } else {
        sqlite3_result_null(ctx);
    }
    return SQLITE_OK;
}

static int generator_rowid(sqlite3_vtab_cursor *pCursor, sqlite3_int64 *pRowid) {
    GeneratorCursor *cur = (GeneratorCursor *)pCursor;
    if (cur->count_only) {
        int rc = materialize_count_only_generator(cur, UINT64_C(0));
        if (rc != SQLITE_OK) return rc;
    }
    if (cur->using_iterator && cur->has_iter) {
        *pRowid = cur->iterator_eof ? 0 : cur->iter.rowid(cur->iter.self);
    } else if (cur->has_gen && !cur->gen_eof) {
        *pRowid = cur->gen.rowid(cur->gen.self);
    } else {
        *pRowid = 0;
    }
    return SQLITE_OK;
}

// ---------------------------------------------------------------------------
// xBestIndex
// ---------------------------------------------------------------------------

static int find_usable_constraint(sqlite3_index_info *info, int column_index,
                                  int op_code) {
    for (int i = 0; i < info->nConstraint; ++i) {
        const struct sqlite3_index_constraint *c = &info->aConstraint[i];
        if (c->usable && c->iColumn == column_index && c->op == op_code) {
            return i;
        }
    }
    return -1;
}

// Does the single ORDER BY term match this filter's declared order?
static bool consumes_order(const gen_constraint_filter *cf,
                           sqlite3_index_info *info) {
    if (!cf->has_order || info->nOrderBy != 1) {
        return false;
    }
    return info->aOrderBy[0].iColumn == cf->ordered_column &&
           ((bool)info->aOrderBy[0].desc == cf->ordered_desc);
}

// Count how many of a constraint filter's specs are satisfiable, and whether all
// required specs are matched. On an unmatched required spec, capture its
// non-empty missing_error into *missing_out (first wins).
static void probe_constraint_filter(const gen_constraint_filter *cf,
                                    sqlite3_index_info *info,
                                    int *matched_count, bool *required_ok,
                                    const char **missing_out) {
    int matched = 0;
    bool req_ok = true;
    for (size_t s = 0; s < cf->nspecs; ++s) {
        const gen_constraint_spec *spec = &cf->specs[s];
        int ci = find_usable_constraint(info, spec->column_index,
                                        sqlite_constraint_op(spec->op));
        if (ci >= 0) {
            matched++;
        } else if (spec->required) {
            req_ok = false;
            if (spec->missing_error && *spec->missing_error && missing_out &&
                !*missing_out) {
                *missing_out = spec->missing_error;
            }
        }
    }
    *matched_count = matched;
    *required_ok = req_ok;
}

static int generator_best_index(sqlite3_vtab *pVtab, sqlite3_index_info *info) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    const xsql_generator_table *def = vtab->def;
    const char *missing_required = NULL;

    // A bare COUNT(*) has no projected columns, constraints, or ordering. Only
    // select this plan when an exact count callback exists; otherwise SQLite
    // performs the normal streaming scan.
    if (def->count_fn && info->colUsed == 0 && info->nConstraint == 0 &&
        info->nOrderBy == 0) {
        info->idxNum = GEN_COUNT_ONLY_SCAN;
        info->estimatedCost = 1.0;
        info->estimatedRows = 1;
        return SQLITE_OK;
    }

    // 1. Constraint filters — pick the best fully-satisfied one (lower cost, then
    //    order-consumption, then more matched specs).
    const gen_constraint_filter *best_cf = NULL;
    bool                         best_cf_consumes = false;
    int                          best_cf_matched = 0;
    for (size_t f = 0; f < def->nconstr; ++f) {
        const gen_constraint_filter *cf = &def->constraints[f];
        int matched = 0;
        bool req_ok = true;
        probe_constraint_filter(cf, info, &matched, &req_ok, &missing_required);
        if (!req_ok || matched == 0) {
            continue;
        }
        bool cons = consumes_order(cf, info);
        bool better;
        if (!best_cf) {
            better = true;
        } else if (cf->cost < best_cf->cost) {
            better = true;
        } else if (cf->cost > best_cf->cost) {
            better = false;
        } else if (cons != best_cf_consumes) {
            better = cons;
        } else {
            better = matched > best_cf_matched;
        }
        if (better) {
            best_cf = cf;
            best_cf_consumes = cons;
            best_cf_matched = matched;
        }
    }
    if (best_cf) {
        // Assign argvIndex (1-based, spec order) and build idxStr = matched spec
        // indices, so xFilter can pair each argv with its column + operator.
        xsqlc_strbuf sb;
        xsqlc_sb_init(&sb);
        int argv_idx = 0;
        for (size_t s = 0; s < best_cf->nspecs; ++s) {
            const gen_constraint_spec *spec = &best_cf->specs[s];
            int ci = find_usable_constraint(info, spec->column_index,
                                            sqlite_constraint_op(spec->op));
            if (ci < 0) {
                continue;
            }
            argv_idx++;
            info->aConstraintUsage[ci].argvIndex = argv_idx;
            info->aConstraintUsage[ci].omit = 1;
            if (argv_idx > 1) {
                xsqlc_sb_putc(&sb, ',');
            }
            xsqlc_sb_append_size(&sb, s);
        }
        char *idx = xsqlc_sb_detach(&sb);
        if (!idx) {
            return SQLITE_NOMEM;
        }
        info->idxStr = sqlite3_mprintf("%s", idx);
        xsql_free(idx);
        if (!info->idxStr) {
            return SQLITE_NOMEM;
        }
        info->needToFreeIdxStr = 1;
        info->idxNum = best_cf->filter_id;
        info->estimatedCost = best_cf->cost;
        info->estimatedRows = (sqlite3_int64)best_cf->est_rows;
        if (best_cf_consumes) {
            info->orderByConsumed = 1;
        }
        return SQLITE_OK;
    }

    // 2. Parametric filters — fire only when EVERY column has a usable EQ.
    for (size_t p = 0; p < def->nparam; ++p) {
        const gen_parametric_filter *pf = &def->parametric[p];
        bool all = pf->ncolumns > 0;
        for (size_t k = 0; k < pf->ncolumns && all; ++k) {
            if (find_usable_constraint(info, pf->column_indices[k],
                                       SQLITE_INDEX_CONSTRAINT_EQ) < 0) {
                all = false;
            }
        }
        if (!all) {
            continue;
        }
        for (size_t k = 0; k < pf->ncolumns; ++k) {
            int ci = find_usable_constraint(info, pf->column_indices[k],
                                            SQLITE_INDEX_CONSTRAINT_EQ);
            info->aConstraintUsage[ci].argvIndex = (int)k + 1;
            info->aConstraintUsage[ci].omit = 1;
        }
        info->idxNum = pf->filter_id;
        info->estimatedCost = pf->cost;
        info->estimatedRows = (sqlite3_int64)pf->est_rows;
        return SQLITE_OK;
    }

    // 3. Single-column equality filters — cheapest matching one.
    const gen_filter *best_filter = NULL;
    int               best_filter_ci = -1;
    double            best_cost = 1e9;
    for (size_t i = 0; i < def->nfilters; ++i) {
        const gen_filter *fl = &def->filters[i];
        int ci = find_usable_constraint(info, fl->column_index,
                                        SQLITE_INDEX_CONSTRAINT_EQ);
        if (ci >= 0 && fl->cost < best_cost) {
            best_filter = fl;
            best_filter_ci = ci;
            best_cost = fl->cost;
        }
    }
    if (best_filter && best_filter_ci >= 0) {
        info->aConstraintUsage[best_filter_ci].argvIndex = 1;
        info->aConstraintUsage[best_filter_ci].omit = 1;
        info->idxNum = best_filter->filter_id;
        info->estimatedCost = best_filter->cost;
        info->estimatedRows = (sqlite3_int64)best_filter->est_rows;
        return SQLITE_OK;
    }

    size_t est = def->estimate_fn ? def->estimate_fn(def->estimate_user) : 1000;

    // 4. A required constraint was unmatched with a message: refuse the query.
    if (missing_required) {
        info->idxNum = GEN_MISSING_REQUIRED_CONSTRAINT;
        info->idxStr = sqlite3_mprintf("%s", missing_required);
        info->needToFreeIdxStr = 1;
        info->estimatedCost = 1e9;
        info->estimatedRows = 1;
        return SQLITE_OK;
    }

    // 5. Full scan.
    info->idxNum = GEN_FILTER_NONE;
    info->estimatedCost = (double)est;
    info->estimatedRows = (sqlite3_int64)est;
    // Carry colUsed to xFilter via idxStr (like the cached table) so a projection-aware
    // generator can skip expensive unused columns. Only the GEN_FILTER_NONE plan reaches
    // xFilter's full-scan branch and uses idxStr (filter/constraint plans returned
    // above), so there is no collision. Only allocated for projection-aware tables.
    if (def->projection_generator_fn) {
        char *s = sqlite3_mprintf("%llu", (unsigned long long)info->colUsed);
        if (s) {
            info->idxStr = s;
            info->needToFreeIdxStr = 1;
        }
    }
    return SQLITE_OK;
}

// ---------------------------------------------------------------------------
// xFilter
// ---------------------------------------------------------------------------

static const gen_constraint_filter *find_constraint_filter(
    const xsql_generator_table *def, int idxNum) {
    for (size_t i = 0; i < def->nconstr; ++i) {
        if (def->constraints[i].filter_id == idxNum) {
            return &def->constraints[i];
        }
    }
    return NULL;
}

static const gen_parametric_filter *find_parametric_filter(
    const xsql_generator_table *def, int idxNum) {
    for (size_t i = 0; i < def->nparam; ++i) {
        if (def->parametric[i].filter_id == idxNum) {
            return &def->parametric[i];
        }
    }
    return NULL;
}

// Prime a freshly-built generator with a single next() (the pull-model contract).
static void prime_generator(GeneratorCursor *cur, xsql_generator *gen,
                            bool built) {
    if (built) {
        cur->gen = *gen;
        cur->has_gen = true;
        cur->gen_eof = !cur->gen.next(cur->gen.self);
    } else {
        cur->has_gen = false;
        cur->gen_eof = true;
    }
}

static int generator_run_constraint(GeneratorCursor *cur,
                                    const gen_constraint_filter *cf,
                                    const char *idxStr, int argc,
                                    sqlite3_value **argv) {
    xsql_constraint_arg *args = NULL;
    if (argc > 0) {
        args = (xsql_constraint_arg *)xsqlc_malloc(sizeof(*args) * (size_t)argc);
        if (!args) {
            return SQLITE_NOMEM;
        }
        int n = 0;
        const char *p = idxStr;
        while (p && *p && n < argc) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            size_t spec = (size_t)(v >= 0 ? v : 0);
            if (spec < cf->nspecs) {
                args[n].column_index = cf->specs[spec].column_index;
                args[n].op = cf->specs[spec].op;
            } else {
                args[n].column_index = -1;
                args[n].op = XSQL_CONSTRAINT_EQ;
            }
            args[n].value = (xsql_value *)argv[n];
            n++;
            p = (*end == ',') ? end + 1 : end;
        }
        if (n != argc || (p && *p)) {
            xsqlc_free(args);
            xsql_vtab_set_error("internal generator constraint plan mismatch");
            return SQLITE_ERROR;
        }
    }
    xsqlc_vtab_clear_error();
    xsql_generator gen;
    memset(&gen, 0, sizeof(gen));
    bool built = cf->factory(argc, args, &gen, cf->user);
    xsqlc_free(args);
    prime_generator(cur, &gen, built);
    return SQLITE_OK;
}

static int generator_xfilter(sqlite3_vtab_cursor *pCursor, int idxNum,
                             const char *idxStr, int argc,
                             sqlite3_value **argv) {
    GeneratorCursor *cur = (GeneratorCursor *)pCursor;
    const xsql_generator_table *def = cur->def;
    generator_reset_cursor(cur);

    if (idxNum == GEN_COUNT_ONLY_SCAN) {
        cur->count_only = true;
        cur->count_only_total = def->count_fn(def->count_user);
        cur->count_only_position = 0;
        return SQLITE_OK;
    }

    if (idxNum == GEN_MISSING_REQUIRED_CONSTRAINT) {
        xsql_vtab_set_error("%s",
                            (idxStr && *idxStr) ? idxStr
                                                : "required constraint missing");
        return xsqlc_vtab_return_error(pCursor->pVtab);
    }

    if (idxNum >= GEN_CONSTRAINT_FILTER_BASE) {
        const gen_constraint_filter *cf = find_constraint_filter(def, idxNum);
        if (cf) {
            int rc = generator_run_constraint(cur, cf, idxStr, argc, argv);
            if (rc != SQLITE_OK) {
                if (xsqlc_vtab_has_error()) {
                    return xsqlc_vtab_return_error(pCursor->pVtab);
                }
                return rc;
            }
            if (xsqlc_vtab_has_error()) {
                return xsqlc_vtab_return_error(pCursor->pVtab);
            }
            return SQLITE_OK;
        }
    } else if (idxNum >= GEN_PARAMETRIC_FILTER_BASE) {
        const gen_parametric_filter *pf = find_parametric_filter(def, idxNum);
        if (pf) {
            xsqlc_vtab_clear_error();
            xsql_generator gen;
            memset(&gen, 0, sizeof(gen));
            bool built = pf->factory(argc, (xsql_value **)argv, &gen, pf->user);
            prime_generator(cur, &gen, built);
            if (xsqlc_vtab_has_error()) {
                return xsqlc_vtab_return_error(pCursor->pVtab);
            }
            return SQLITE_OK;
        }
    } else if (idxNum != GEN_FILTER_NONE && argc > 0) {
        for (size_t i = 0; i < def->nfilters; ++i) {
            const gen_filter *fl = &def->filters[i];
            if (fl->filter_id != idxNum) {
                continue;
            }
            xsqlc_vtab_clear_error();
            cur->using_iterator = true;
            cur->iterator_eof = true;
            xsql_row_iter it;
            memset(&it, 0, sizeof(it));
            if (fl->factory((xsql_value *)argv[0], &it, fl->user)) {
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
    if (def->full_scan_error) {
        xsql_vtab_set_error("%s", def->full_scan_error);
        return xsqlc_vtab_return_error(pCursor->pVtab);
    }
    if (def->projection_generator_fn) {
        // Projection-aware full scan: decode colUsed (set on the GEN_FILTER_NONE plan).
        uint64_t col_used = ~(uint64_t)0;  // absent/unparseable => all columns (safe)
        if (idxStr && *idxStr) {
            col_used = (uint64_t)strtoull(idxStr, NULL, 10);
        }
        xsqlc_vtab_clear_error();
        xsql_generator gen;
        memset(&gen, 0, sizeof(gen));
        bool built = def->projection_generator_fn(col_used, &gen, def->projection_generator_user);
        prime_generator(cur, &gen, built);
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
    } else if (def->generator_fn) {
        xsqlc_vtab_clear_error();
        xsql_generator gen;
        memset(&gen, 0, sizeof(gen));
        bool built = def->generator_fn(&gen, def->generator_user);
        prime_generator(cur, &gen, built);
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(pCursor->pVtab);
        }
    }
    return SQLITE_OK;
}

// ---------------------------------------------------------------------------
// xUpdate — reconstruct the old row by rowid via row_lookup, then apply setters.
// ---------------------------------------------------------------------------

static int generator_delete(GeneratorVtab *vtab, sqlite3_value **argv) {
    const xsql_generator_table *def = vtab->def;
    if (!def->supports_delete || !def->delete_fn || !def->lookup_fn) {
        return xsqlc_unsupported_write(&vtab->base, def->name, 'D');
    }
    int64_t raw = sqlite3_value_int64(argv[0]);
    void *temp = xsqlc_malloc(def->row_size);
    if (!temp) {
        return SQLITE_NOMEM;
    }
    xsqlc_vtab_clear_error();
    if (!def->lookup_fn(temp, raw, def->lookup_user)) {
        xsqlc_free(temp);
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(&vtab->base);
        }
        return SQLITE_ERROR;
    }
    xsqlc_transaction_touch(&vtab->transaction);
    fire_modify(def->before_modify_fn, def->before_user, "DELETE FROM ",
                def->name);
    xsqlc_vtab_clear_error();
    bool ok = def->delete_fn(temp, def->delete_user);
    xsqlc_free(temp);
    if (!ok) {
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(&vtab->base);
        }
        return SQLITE_ERROR;
    }
    fire_modify(def->after_modify_fn, def->after_user, "DELETE FROM ", def->name);
    return SQLITE_OK;
}

// Per-column accessors for the shared UPDATE apply path (xsqlc_apply_update_columns).
typedef struct {
    const xsql_generator_table *def;
    void                       *row;
} generator_update_ctx;
static bool generator_update_writable(void *ctx, size_t c) {
    const gen_column *col = &((const generator_update_ctx *)ctx)->def->columns[c];
    return col->writable && col->set_kind != GSET_NONE;
}
static const char *generator_update_name(void *ctx, size_t c) {
    return ((const generator_update_ctx *)ctx)->def->columns[c].name;
}
static bool generator_update_apply(void *ctx, size_t c, sqlite3_value *val) {
    const generator_update_ctx *x = (const generator_update_ctx *)ctx;
    return column_set(&x->def->columns[c], x->row, (xsql_value *)val);
}

static int generator_update(GeneratorVtab *vtab, int argc,
                            sqlite3_value **argv) {
    const xsql_generator_table *def = vtab->def;
    if (!def->lookup_fn || !table_has_writable(def)) {
        return xsqlc_unsupported_write(&vtab->base, def->name, 'U');
    }
    int64_t raw = sqlite3_value_int64(argv[0]);
    void *temp = xsqlc_malloc(def->row_size);
    if (!temp) {
        return SQLITE_NOMEM;
    }
    xsqlc_vtab_clear_error();
    if (!def->lookup_fn(temp, raw, def->lookup_user)) {
        xsqlc_free(temp);
        if (xsqlc_vtab_has_error()) {
            return xsqlc_vtab_return_error(&vtab->base);
        }
        return SQLITE_READONLY;
    }
    xsqlc_transaction_touch(&vtab->transaction);
    fire_modify(def->before_modify_fn, def->before_user, "UPDATE ", def->name);
    // lookup_fn resolves the row by rowid, so NOCHANGE is always honored here.
    generator_update_ctx uctx = {def, temp};
    xsqlc_update_ops ops = {generator_update_writable, generator_update_name,
                            generator_update_apply, &uctx};
    if (!xsqlc_apply_update_columns(argc, argv, def->ncols, /*nochange_eligible=*/true,
                                    &ops)) {
        xsqlc_free(temp);
        return xsqlc_vtab_return_error(&vtab->base);
    }
    xsqlc_free(temp);
    fire_modify(def->after_modify_fn, def->after_user, "UPDATE ", def->name);
    return SQLITE_OK;
}

static int generator_insert(GeneratorVtab *vtab, int argc,
                            sqlite3_value **argv) {
    const xsql_generator_table *def = vtab->def;
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
    fire_modify(def->after_modify_fn, def->after_user, "INSERT INTO ", def->name);
    return SQLITE_OK;
}

static int generator_update_dispatch(sqlite3_vtab *pVtab, int argc,
                                     sqlite3_value **argv,
                                     sqlite3_int64 *pRowid) {
    (void)pRowid;
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    int rc;
    if (argc == 1 && sqlite3_value_type(argv[0]) != SQLITE_NULL) {
        rc = generator_delete(vtab, argv);
    } else if (argc > 1 && sqlite3_value_type(argv[0]) != SQLITE_NULL) {
        rc = generator_update(vtab, argc, argv);
    } else if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        rc = generator_insert(vtab, argc, argv);
    } else {
        rc = SQLITE_READONLY;
    }
    if (rc == SQLITE_OK) {
        xsqlc_transaction_write(&vtab->transaction);
    }
    return rc;
}

static int generator_begin(sqlite3_vtab *pVtab) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    return xsqlc_transaction_begin(pVtab, &vtab->transaction);
}
static int generator_prepare(sqlite3_vtab *pVtab) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    return xsqlc_transaction_prepare(pVtab, &vtab->transaction);
}
static int generator_commit(sqlite3_vtab *pVtab) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    return xsqlc_transaction_commit(pVtab, &vtab->transaction);
}
static int generator_rollback(sqlite3_vtab *pVtab) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    return xsqlc_transaction_rollback(pVtab, &vtab->transaction);
}
static int generator_savepoint(sqlite3_vtab *pVtab, int savepoint) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    return xsqlc_transaction_savepoint(
        pVtab, &vtab->transaction, savepoint);
}
static int generator_release(sqlite3_vtab *pVtab, int savepoint) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    return xsqlc_transaction_release(
        pVtab, &vtab->transaction, savepoint);
}
static int generator_rollback_to(sqlite3_vtab *pVtab, int savepoint) {
    GeneratorVtab *vtab = (GeneratorVtab *)pVtab;
    return xsqlc_transaction_rollback_to(
        pVtab, &vtab->transaction, savepoint);
}

static sqlite3_module g_generator_module = {
    .iVersion = 3,
    .xCreate = generator_connect,
    .xConnect = generator_connect,
    .xBestIndex = generator_best_index,
    .xDisconnect = generator_disconnect,
    .xDestroy = generator_destroy,
    .xOpen = generator_open,
    .xClose = generator_close,
    .xFilter = generator_xfilter,
    .xNext = generator_next,
    .xEof = generator_eof,
    .xColumn = generator_xcolumn,
    .xRowid = generator_rowid,
    .xUpdate = generator_update_dispatch,
    .xBegin = generator_begin,
    .xSync = generator_prepare,
    .xCommit = generator_commit,
    .xRollback = generator_rollback,
    .xSavepoint = generator_savepoint,
    .xRelease = generator_release,
    .xRollbackTo = generator_rollback_to,
};

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

xsql_generator_table *xsql_generator_table_new(const char *name,
                                               size_t row_size) {
    xsql_generator_table *t =
        (xsql_generator_table *)xsqlc_malloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->row_size = row_size;
    t->name = xsqlc_strdup(name ? name : "");
    if (!t->name) {
        xsqlc_free(t);
        return NULL;
    }
    return t;
}

void xsql_generator_table_free(xsql_generator_table *g) {
    if (!g) {
        return;
    }
    for (size_t i = 0; i < g->ncols; ++i) {
        xsqlc_free(g->columns[i].name);
    }
    xsqlc_free(g->columns);
    xsqlc_free(g->filters);
    for (size_t i = 0; i < g->nparam; ++i) {
        xsqlc_free(g->parametric[i].column_indices);
    }
    xsqlc_free(g->parametric);
    for (size_t i = 0; i < g->nconstr; ++i) {
        for (size_t s = 0; s < g->constraints[i].nspecs; ++s) {
            xsqlc_free(g->constraints[i].specs[s].missing_error);
        }
        xsqlc_free(g->constraints[i].specs);
    }
    xsqlc_free(g->constraints);
    xsqlc_free(g->full_scan_error);
    xsqlc_free(g->name);
    xsqlc_free(g);
}

void xsql_generator_table_generator(xsql_generator_table *g,
                                    xsql_generator_make_fn fn, void *user) {
    if (g) {
        g->generator_fn = fn;
        g->generator_user = user;
    }
}
void xsql_generator_table_projection_generator(xsql_generator_table *g,
                                               xsql_generator_make_projection_fn fn,
                                               void *user) {
    if (g) {
        g->projection_generator_fn = fn;
        g->projection_generator_user = user;
    }
}
void xsql_generator_estimate_rows(xsql_generator_table *g,
                                  xsql_estimate_rows_fn fn, void *user) {
    if (g) {
        g->estimate_fn = fn;
        g->estimate_user = user;
    }
}
void xsql_generator_count(xsql_generator_table *g, xsql_row_count_fn fn,
                          void *user) {
    if (g) {
        g->count_fn = fn;
        g->count_user = user;
    }
}
void xsql_generator_full_scan_error(xsql_generator_table *g,
                                    const char *message) {
    if (g) {
        xsqlc_free(g->full_scan_error);
        g->full_scan_error = xsqlc_strdup(message ? message : "");
    }
}
void xsql_generator_on_modify(xsql_generator_table *g,
                              xsql_generator_modify_fn fn, void *user) {
    if (g) {
        g->before_modify_fn = fn;
        g->before_user = user;
    }
}
void xsql_generator_after_modify(xsql_generator_table *g,
                                 xsql_generator_modify_fn fn, void *user) {
    if (g) {
        g->after_modify_fn = fn;
        g->after_user = user;
    }
}
int xsql_generator_transaction_hooks(
    xsql_generator_table *g, const xsql_transaction_hooks *hooks, void *user) {
    if (!g || (hooks && hooks->struct_size != sizeof(*hooks))) {
        return XSQL_MISUSE;
    }
    memset(&g->transaction_hooks, 0, sizeof(g->transaction_hooks));
    g->transaction_user = NULL;
    if (hooks) {
        g->transaction_hooks = *hooks;
        g->transaction_user = user;
    }
    return XSQL_OK;
}

static gen_column *push_column(xsql_generator_table *t, const char *name,
                               xsql_column_type type) {
    if (!xsqlc_vec_reserve((void **)&t->columns, &t->cols_cap, t->ncols + 1,
                           sizeof(gen_column))) {
        return NULL;
    }
    gen_column *c = &t->columns[t->ncols];
    memset(c, 0, sizeof(*c));
    c->name = xsqlc_strdup(name ? name : "");
    if (!c->name) {
        return NULL;
    }
    c->type = type;
    c->set_kind = GSET_NONE;
    t->ncols++;
    return c;
}

int xsql_generator_col(xsql_generator_table *g, const char *name,
                       xsql_column_type type, xsql_grow_emit_fn get,
                       void *user) {
    if (!g) {
        return XSQL_MISUSE;
    }
    gen_column *col = push_column(g, name, type);
    if (!col) {
        return XSQL_NO_MEMORY;
    }
    col->get_kind = GGET_EMIT;
    col->get.emit = get;
    col->user = user;
    return XSQL_OK;
}

#define GEN_COL_TYPED(fnname, coltype, getkind, getfield, gettype)             \
    int fnname(xsql_generator_table *g, const char *name, gettype get,        \
               void *user) {                                                  \
        if (!g) {                                                             \
            return XSQL_MISUSE;                                               \
        }                                                                     \
        gen_column *col = push_column(g, name, coltype);                      \
        if (!col) {                                                           \
            return XSQL_NO_MEMORY;                                            \
        }                                                                     \
        col->get_kind = getkind;                                             \
        col->get.getfield = get;                                             \
        col->user = user;                                                    \
        return XSQL_OK;                                                      \
    }

GEN_COL_TYPED(xsql_generator_col_int, XSQL_COL_INTEGER, GGET_INT, i,
              xsql_grow_int_fn)
GEN_COL_TYPED(xsql_generator_col_i64, XSQL_COL_INTEGER, GGET_I64, i64,
              xsql_grow_i64_fn)
GEN_COL_TYPED(xsql_generator_col_double, XSQL_COL_REAL, GGET_DOUBLE, d,
              xsql_grow_double_fn)
GEN_COL_TYPED(xsql_generator_col_text, XSQL_COL_TEXT, GGET_EMIT, emit,
              xsql_grow_emit_fn)
GEN_COL_TYPED(xsql_generator_col_blob, XSQL_COL_BLOB, GGET_EMIT, emit,
              xsql_grow_emit_fn)

static gen_column *push_rw_column(xsql_generator_table *g, const char *name,
                                  xsql_column_type type, int get_kind,
                                  void *user) {
    if (!g) {
        return NULL;
    }
    gen_column *col = push_column(g, name, type);
    if (!col) {
        return NULL;
    }
    col->get_kind = get_kind;
    col->writable = true;
    col->user = user;
    return col;
}

int xsql_generator_col_rw(xsql_generator_table *g, const char *name,
                          xsql_column_type type, xsql_grow_emit_fn get,
                          xsql_grow_set_value_fn set, void *user) {
    gen_column *col = push_rw_column(g, name, type, GGET_EMIT, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = GSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_generator_col_int_rw(xsql_generator_table *g, const char *name,
                              xsql_grow_int_fn get, xsql_grow_set_int_fn set,
                              void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_INTEGER, GGET_INT, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.i = get;
    col->set_kind = GSET_INT;
    col->set.i = set;
    return XSQL_OK;
}
int xsql_generator_col_i64_rw(xsql_generator_table *g, const char *name,
                              xsql_grow_i64_fn get, xsql_grow_set_i64_fn set,
                              void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_INTEGER, GGET_I64, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.i64 = get;
    col->set_kind = GSET_I64;
    col->set.i64 = set;
    return XSQL_OK;
}
int xsql_generator_col_text_rw(xsql_generator_table *g, const char *name,
                               xsql_grow_emit_fn get, xsql_grow_set_text_fn set,
                               void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_TEXT, GGET_EMIT, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = GSET_TEXT;
    col->set.text = set;
    return XSQL_OK;
}
int xsql_generator_col_text_nullable_rw(xsql_generator_table *g,
                                        const char *name, xsql_grow_emit_fn get,
                                        xsql_grow_set_value_fn set, void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_TEXT, GGET_EMIT, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = GSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_generator_col_double_rw(xsql_generator_table *g, const char *name,
                                 xsql_grow_double_fn get,
                                 xsql_grow_set_value_fn set, void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_REAL, GGET_DOUBLE, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.d = get;
    col->set_kind = GSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_generator_col_blob_rw(xsql_generator_table *g, const char *name,
                               xsql_grow_emit_fn get, xsql_grow_set_value_fn set,
                               void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_BLOB, GGET_EMIT, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = GSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_generator_col_int_rw_value(xsql_generator_table *g, const char *name,
                                    xsql_grow_int_fn get,
                                    xsql_grow_set_value_fn set, void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_INTEGER, GGET_INT, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.i = get;
    col->set_kind = GSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_generator_col_i64_rw_value(xsql_generator_table *g, const char *name,
                                    xsql_grow_i64_fn get,
                                    xsql_grow_set_value_fn set, void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_INTEGER, GGET_I64, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.i64 = get;
    col->set_kind = GSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}
int xsql_generator_col_text_rw_value(xsql_generator_table *g, const char *name,
                                     xsql_grow_emit_fn get,
                                     xsql_grow_set_value_fn set, void *user) {
    gen_column *col = push_rw_column(g, name, XSQL_COL_TEXT, GGET_EMIT, user);
    if (!col) {
        return g ? XSQL_NO_MEMORY : XSQL_MISUSE;
    }
    col->get.emit = get;
    col->set_kind = GSET_VALUE;
    col->set.value = set;
    return XSQL_OK;
}

static int add_hidden_column(xsql_generator_table *g, const char *name,
                             xsql_column_type type) {
    if (!g) {
        return XSQL_MISUSE;
    }
    gen_column *col = push_column(g, name, type);
    if (!col) {
        return XSQL_NO_MEMORY;
    }
    col->get_kind = GGET_NULL;
    col->hidden = true;
    return XSQL_OK;
}

int xsql_generator_hidden_col_i64(xsql_generator_table *g, const char *name) {
    return add_hidden_column(g, name, XSQL_COL_INTEGER);
}
int xsql_generator_hidden_col_int(xsql_generator_table *g, const char *name) {
    return add_hidden_column(g, name, XSQL_COL_INTEGER);
}
int xsql_generator_hidden_col_text(xsql_generator_table *g, const char *name) {
    return add_hidden_column(g, name, XSQL_COL_TEXT);
}

static int add_filter(xsql_generator_table *g, const char *column, double cost,
                      double est_rows, xsql_filter_factory_fn factory,
                      void *user) {
    if (!g) {
        return XSQL_MISUSE;
    }
    int col = find_column(g, column);
    if (col < 0) {
        return XSQL_NOT_FOUND;
    }
    if (!xsqlc_vec_reserve((void **)&g->filters, &g->filters_cap, g->nfilters + 1,
                           sizeof(gen_filter))) {
        return XSQL_NO_MEMORY;
    }
    gen_filter *f = &g->filters[g->nfilters];
    f->column_index = col;
    f->filter_id = (int)g->nfilters + 1;
    f->cost = cost;
    f->est_rows = est_rows;
    f->op = XSQL_FILTER_EQ;
    f->factory = factory;
    f->user = user;
    g->nfilters++;
    return XSQL_OK;
}

int xsql_generator_filter_eq(xsql_generator_table *g, const char *column,
                             double cost, double est_rows,
                             xsql_filter_factory_fn factory, void *user) {
    return add_filter(g, column, cost, est_rows, factory, user);
}
int xsql_generator_filter_eq_text(xsql_generator_table *g, const char *column,
                                  double cost, double est_rows,
                                  xsql_filter_factory_fn factory, void *user) {
    return add_filter(g, column, cost, est_rows, factory, user);
}

int xsql_generator_parametric_filter(xsql_generator_table *g,
                                     const char *const *columns, size_t ncolumns,
                                     double cost, double est_rows,
                                     xsql_generator_parametric_fn factory,
                                     void *user) {
    if (!g) {
        return XSQL_MISUSE;
    }
    if (ncolumns == 0) {
        return XSQL_MISUSE;
    }
    int *indices = (int *)xsqlc_malloc(sizeof(int) * ncolumns);
    if (!indices) {
        return XSQL_NO_MEMORY;
    }
    for (size_t k = 0; k < ncolumns; ++k) {
        int col = find_column(g, columns[k]);
        if (col < 0) {
            xsqlc_free(indices);
            return XSQL_NOT_FOUND;
        }
        indices[k] = col;
    }
    if (!xsqlc_vec_reserve((void **)&g->parametric, &g->param_cap, g->nparam + 1,
                           sizeof(gen_parametric_filter))) {
        xsqlc_free(indices);
        return XSQL_NO_MEMORY;
    }
    gen_parametric_filter *pf = &g->parametric[g->nparam];
    memset(pf, 0, sizeof(*pf));
    pf->column_indices = indices;
    pf->ncolumns = ncolumns;
    pf->filter_id = GEN_PARAMETRIC_FILTER_BASE + (int)g->nparam;
    pf->cost = cost;
    pf->est_rows = est_rows;
    pf->factory = factory;
    pf->user = user;
    g->nparam++;
    return XSQL_OK;
}

int xsql_generator_constraint_filter(xsql_generator_table *g,
                                     const xsql_constraint_req *reqs,
                                     size_t nreqs, double cost, double est_rows,
                                     xsql_generator_constraint_fn factory,
                                     void *user) {
    if (!g) {
        return XSQL_MISUSE;
    }
    if (nreqs == 0) {
        return XSQL_MISUSE;
    }
    gen_constraint_spec *specs =
        (gen_constraint_spec *)xsqlc_malloc(sizeof(*specs) * nreqs);
    if (!specs) {
        return XSQL_NO_MEMORY;
    }
    memset(specs, 0, sizeof(*specs) * nreqs);
    for (size_t s = 0; s < nreqs; ++s) {
        int col = find_column(g, reqs[s].column);
        if (col < 0) {
            for (size_t j = 0; j < s; ++j) {
                xsqlc_free(specs[j].missing_error);
            }
            xsqlc_free(specs);
            return XSQL_NOT_FOUND;
        }
        specs[s].column_index = col;
        specs[s].op = reqs[s].op;
        specs[s].required = reqs[s].required;
        if (reqs[s].required && reqs[s].missing_error) {
            specs[s].missing_error = xsqlc_strdup(reqs[s].missing_error);
            if (!specs[s].missing_error) {
                for (size_t j = 0; j <= s; ++j) {
                    xsqlc_free(specs[j].missing_error);
                }
                xsqlc_free(specs);
                return XSQL_NO_MEMORY;
            }
        }
    }
    if (!xsqlc_vec_reserve((void **)&g->constraints, &g->constr_cap,
                           g->nconstr + 1, sizeof(gen_constraint_filter))) {
        for (size_t s = 0; s < nreqs; ++s) {
            xsqlc_free(specs[s].missing_error);
        }
        xsqlc_free(specs);
        return XSQL_NO_MEMORY;
    }
    gen_constraint_filter *cf = &g->constraints[g->nconstr];
    memset(cf, 0, sizeof(*cf));
    cf->specs = specs;
    cf->nspecs = nreqs;
    cf->filter_id = GEN_CONSTRAINT_FILTER_BASE + (int)g->nconstr;
    cf->cost = cost;
    cf->est_rows = est_rows;
    cf->factory = factory;
    cf->user = user;
    g->nconstr++;
    return XSQL_OK;
}

static xsql_constraint_req make_req(const char *column, xsql_constraint_op op,
                                    bool required, const char *missing_error) {
    xsql_constraint_req r;
    r.column = column;
    r.op = op;
    r.required = required;
    r.missing_error = missing_error;
    return r;
}

xsql_constraint_req xsql_required_eq(const char *column,
                                     const char *missing_error) {
    return make_req(column, XSQL_CONSTRAINT_EQ, true, missing_error);
}
xsql_constraint_req xsql_required_like(const char *column,
                                       const char *missing_error) {
    return make_req(column, XSQL_CONSTRAINT_LIKE, true, missing_error);
}
xsql_constraint_req xsql_optional_eq(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_EQ, false, NULL);
}
xsql_constraint_req xsql_optional_like(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_LIKE, false, NULL);
}
xsql_constraint_req xsql_optional_gt(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_GT, false, NULL);
}
xsql_constraint_req xsql_optional_ge(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_GE, false, NULL);
}
xsql_constraint_req xsql_optional_lt(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_LT, false, NULL);
}
xsql_constraint_req xsql_optional_le(const char *column) {
    return make_req(column, XSQL_CONSTRAINT_LE, false, NULL);
}

void xsql_generator_order_by_consumed(xsql_generator_table *g,
                                      const char *column, bool desc) {
    if (!g || g->nconstr == 0) {
        return;
    }
    int col = find_column(g, column);
    if (col < 0) {
        return;
    }
    gen_constraint_filter *cf = &g->constraints[g->nconstr - 1];
    cf->has_order = true;
    cf->ordered_column = col;
    cf->ordered_desc = desc;
}

void xsql_generator_row_lookup(xsql_generator_table *g,
                               xsql_generator_row_lookup_fn fn, void *user) {
    if (g) {
        g->lookup_fn = fn;
        g->lookup_user = user;
    }
}
void xsql_generator_deletable(xsql_generator_table *g,
                              xsql_generator_delete_fn fn, void *user) {
    if (g) {
        g->supports_delete = true;
        g->delete_fn = fn;
        g->delete_user = user;
    }
}
void xsql_generator_insertable(xsql_generator_table *g,
                               xsql_generator_insert_fn fn, void *user) {
    if (g) {
        g->supports_insert = true;
        g->insert_fn = fn;
        g->insert_user = user;
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

int xsql_generator_table_register_module(struct xsql_db *db,
                                         xsql_generator_table *g) {
    if (!db || !db->db || !g || !xsqlc_is_valid_sql_identifier(g->name)) {
        return XSQL_MISUSE;
    }
    return sqlite3_create_module_v2(db->db, g->name, &g_generator_module, g,
                                    NULL);
}

int xsql_generator_table_register_as(struct xsql_db *db, xsql_generator_table *g,
                                     const char *table_name) {
    if (!db || !db->db || !g || !xsqlc_is_valid_sql_identifier(table_name)) {
        return XSQL_MISUSE;
    }
    int rc = sqlite3_create_module_v2(db->db, table_name, &g_generator_module, g,
                                      NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    rc = xsqlc_create_vtable_sqlite(db->db, table_name, table_name);
    return rc;
}

int xsql_generator_table_register(struct xsql_db *db, xsql_generator_table *g) {
    if (!db || !g) {
        return XSQL_MISUSE;
    }
    return xsql_generator_table_register_as(db, g, g->name);
}
