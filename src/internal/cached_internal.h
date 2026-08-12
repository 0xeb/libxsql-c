// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// cached_internal.h — concrete layout of the cached-table definition and its
// shared cache, shared privately between table_cached.c (module + builder) and
// shared_cache.c (cache lifecycle). Rows are type-erased (`row_size` bytes,
// contiguous). PRIVATE.

#ifndef XSQLC_INTERNAL_CACHED_INTERNAL_H
#define XSQLC_INTERNAL_CACHED_INTERNAL_H

#include <libxsqlc/table_cached.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/hashmap.h"

struct sqlite3_mutex;  // forward-declared; the shared cache uses sqlite3's mutex

// Contiguous row storage: `count` records of `row_size` bytes each.
typedef struct {
    void  *data;
    size_t count;
    size_t cap;       // capacity in rows
    size_t row_size;
} rowbuf;

// The public cache-fill handle is a thin view over the rowbuf being populated.
struct xsql_rowset {
    rowbuf *rb;
};

// Column get/set dispatch kinds (no closures in C).
enum { CGET_I64, CGET_INT, CGET_DOUBLE, CGET_EMIT };
enum { CSET_NONE, CSET_I64, CSET_INT, CSET_TEXT, CSET_VALUE };

typedef struct {
    char            *name;
    xsql_column_type type;
    bool             writable;

    int get_kind;
    union {
        xsql_crow_i64_fn    i64;
        xsql_crow_int_fn    i;
        xsql_crow_double_fn d;
        xsql_crow_emit_fn   emit;
    } get;

    int set_kind;
    union {
        xsql_crow_set_i64_fn   i64;
        xsql_crow_set_int_fn   i;
        xsql_crow_set_text_fn  text;
        xsql_crow_set_value_fn value;
    } set;

    void *user;
} cached_column;

typedef struct {
    int                    column_index;
    int                    filter_id;
    double                 cost;
    double                 est_rows;
    xsql_filter_op         op;
    xsql_filter_factory_fn factory;
    void                  *user;
} cached_filter;

typedef struct {
    int                   column_index;
    xsql_key_extractor_fn key;
    void                 *user;
} cached_index_def;

// One (key, row-position) pair of a sorted index. Sorted ascending by key; the
// row order for equal keys is preserved (stable), so a duplicate key is a
// contiguous run and the window a range carves is reproducible.
typedef struct {
    int64_t key;
    size_t  row;
} xsqlc_sorted_entry;

// A per-index sorted view used ONLY for range (>=, >, <=, <, BETWEEN) pushdown.
// The hash `indexes` stays the sole structure for the hot equality path; this
// parallel structure is built LAZILY on the first range query for an index and
// then cached alongside the hash index (invalidated on the same rebuilds).
// `built` gates a one-time construction.
typedef struct {
    xsqlc_sorted_entry *entries;  // sorted by key ascending
    size_t              count;
    size_t              cap;
    bool                built;
} xsqlc_sorted_index;

// The shared, lazily-built cache.
//
// CONCURRENCY CONTRACT. A shared cache (use_shared_cache = true) is engine-
// lifetime and shared across every connection to the same table def, so its
// mutation of shared state must be serialized. `mutex` (a sqlite3 fast mutex)
// protects EVERY writer of the shared state:
//   - the lazy full BUILD of `data` + hash `indexes` + sizing of `sorted_indexes`
//     (xsqlc_cache_ensure_built),
//   - INVALIDATION / post-mutation invalidation and the `built` /
//     `mutation_snapshot` flag flips (xsqlc_cache_invalidate[_after_mutation]),
//   - the LAZY sorted-view build for a range query, i.e. the first
//     xsqlc_sorted_index_build into `sorted_indexes[i]` AND the paired
//     xsqlc_sorted_index_collect out of it (both under `mutex` in xFilter — a
//     bare build/collect there would race writer vs reader on the same array),
//   - teardown via xsqlc_cache_free_indexes (caller must already hold `mutex`).
// The cursor READ side reads the publish state under the SAME `mutex`: the
// `built` / `mutation_snapshot` flag pair and `data.count` are read via
// shared_view_usable() (a brief mutex-guarded consistent snapshot) in
// full_scan_row / cached_eof, because another CONNECTION to the same shared def
// can flip those flags / swap the `data` rowbuf under `mutex` while this
// connection scans — an unlocked read there is a torn/stale read that could index
// past a shrunk buffer. Borrowing an `indexes[i]` position list stays valid
// because a rebuild that would reallocate it is itself mutex-guarded and only
// runs when the cache is invalidated (never mid-scan for a live cursor). Do NOT
// add an unlocked reader of the flag pair, and do NOT add an unlocked writer to
// the shared state.
//
// For a query-scoped cache (use_shared_cache = false) the `data`/index storage
// is effectively per-statement (the mutation snapshot) or per-cursor, so the
// same `mutex` calls are cheap no-op serialization on an uncontended lock.
typedef struct shared_cache {
    rowbuf                data;
    xsqlc_i64map         *indexes;   // one map per index_def (int64 key -> rows)
    size_t                nindexes;
    // Parallel sorted views for range pushdown, one slot per hash index. Built on
    // demand (first range query) under `mutex`, cleared whenever `indexes` is.
    xsqlc_sorted_index   *sorted_indexes;
    size_t                nsorted;
    bool                  built;
    bool                  mutation_snapshot;  // keep data alive across a scan-driven write
    struct sqlite3_mutex *mutex;
} shared_cache;

// Range-flag bits (see the idxNum layout in table_cached.c).
#define XSQLC_RANGE_HAS_LOW    0x1
#define XSQLC_RANGE_LOW_STRICT 0x2
#define XSQLC_RANGE_HAS_HIGH   0x4
#define XSQLC_RANGE_HIGH_STRICT 0x8

// Build (once) the sorted (key,row) view from a data rowbuf + its key extractor.
// Entries are sorted ascending by key; ties keep data order (stable). Returns
// false on OOM (leaves `sorted` empty + unbuilt).
bool xsqlc_sorted_index_build(xsqlc_sorted_index *sorted, const rowbuf *data,
                              xsql_key_extractor_fn key, void *user);

// Free a sorted index's entries and reset it to empty + unbuilt.
void xsqlc_sorted_index_free(xsqlc_sorted_index *sorted);

// Carve the matching window from a built sorted index honoring `range_flags`
// (XSQLC_RANGE_*) + bounds and append the matching row positions to `out`.
// `low`/`high` are only read when the corresponding HAS_* flag is set. Returns
// false on OOM while appending to `out`.
bool xsqlc_sorted_index_collect(const xsqlc_sorted_index *sorted, int range_flags,
                                int64_t low, int64_t high, xsqlc_poslist *out);

struct xsql_cached_table {
    char  *name;
    size_t row_size;

    xsql_estimate_rows_fn           estimate_fn;
    void                           *estimate_user;
    xsql_row_count_fn               count_fn;
    void                           *count_user;
    xsql_cache_builder_fn           cache_builder_fn;
    void                           *cache_builder_user;
    xsql_stateful_cache_builder_fn  stateful_cache_builder_fn;
    void                           *stateful_cache_builder_user;
    xsql_projection_cache_builder_fn projection_fn;
    void                           *projection_user;

    cached_column    *columns;
    size_t            ncols, cols_cap;
    cached_filter    *filters;
    size_t            nfilters, filters_cap;
    cached_index_def *index_defs;
    size_t            nindexes, index_cap;

    xsql_crow_delete_fn   delete_fn;
    void                 *delete_user;
    bool                  supports_delete;
    xsql_cached_insert_fn insert_fn;
    void                 *insert_user;
    bool                  supports_insert;
    xsql_cached_modify_fn before_modify_fn;
    void                 *before_user;
    xsql_cached_modify_fn after_modify_fn;
    void                 *after_user;
    xsql_transaction_hooks transaction_hooks;
    void                  *transaction_user;
    xsql_row_populator_fn populator_fn;
    void                 *populator_user;
    xsql_row_lookup_fn    lookup_fn;
    void                 *lookup_user;
    xsql_cached_rowid_fn  rowid_fn;
    void                 *rowid_user;

    bool          update_from_column_values;
    bool          use_shared_cache;
    shared_cache *shared;  // always allocated at _new (unused when non-shared)
};

// ---------------------------------------------------------------------------
// rowbuf + shared-cache lifecycle (shared_cache.c)
// ---------------------------------------------------------------------------

bool  xsqlc_rowbuf_push(rowbuf *rb, const void *row);
void *xsqlc_rowbuf_at(const rowbuf *rb, size_t i);
void  xsqlc_rowbuf_clear(rowbuf *rb);  // count = 0, capacity retained
void  xsqlc_rowbuf_free(rowbuf *rb);

// Allocate/free the shared cache struct (with its mutex).
shared_cache *xsqlc_shared_cache_new(size_t row_size);
void          xsqlc_shared_cache_free(shared_cache *sc);

// Tear down BOTH the hash indexes and the parallel sorted range views of a cache
// (freeing each and resetting nindexes/nsorted to 0). Caller must hold sc->mutex.
// This is the single teardown for the index pair — never free one half by hand.
void xsqlc_cache_free_indexes(shared_cache *sc);

// Ensure the shared cache is built (lazy, mutex-guarded). Returns true if the
// cache is usable; false when the builder raised an error (left in the vtab error
// channel for the caller to surface). No-op (returns true) for non-shared tables.
bool xsqlc_cache_ensure_built(const struct xsql_cached_table *def);

// Full clear (INSERT / explicit invalidation): drop data + indexes, built=false.
void xsqlc_cache_invalidate(const struct xsql_cached_table *def);

// Post-mutation invalidation (UPDATE/DELETE): clear indexes, built=false, but keep
// `data` alive as a mutation snapshot so an in-flight scan-driven write still
// visits every row.
void xsqlc_cache_invalidate_after_mutation(const struct xsql_cached_table *def);

#endif  // XSQLC_INTERNAL_CACHED_INTERNAL_H
