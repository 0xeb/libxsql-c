// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// shared_cache.c — the cached table's contiguous row storage and shared-cache
// lifecycle: lazy build, index construction, invalidation, and the mutation
// snapshot that keeps row data alive across a scan-driven UPDATE/DELETE.
// The per-cache mutex is sqlite3's, so no platform threading code is needed here.

#include "internal/cached_internal.h"

#include "internal/internal.h"
#include "internal/vtab_core.h"

#include <sqlite3.h>

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// rowbuf
// ---------------------------------------------------------------------------

bool xsqlc_rowbuf_push(rowbuf *rb, const void *row) {
    if (rb->count == rb->cap) {
        size_t new_cap = rb->cap ? rb->cap * 2 : 8;
        size_t bytes;
        if (!xsqlc_mul_ok(new_cap, rb->row_size, &bytes)) {
            return false;
        }
        void *grown = xsqlc_realloc(rb->data, bytes);
        if (!grown) {
            return false;
        }
        rb->data = grown;
        rb->cap = new_cap;
    }
    memcpy((char *)rb->data + rb->count * rb->row_size, row, rb->row_size);
    rb->count++;
    return true;
}

void *xsqlc_rowbuf_at(const rowbuf *rb, size_t i) {
    return (char *)rb->data + i * rb->row_size;
}

void xsqlc_rowbuf_clear(rowbuf *rb) {
    rb->count = 0;
}

void xsqlc_rowbuf_free(rowbuf *rb) {
    xsqlc_free(rb->data);
    rb->data = NULL;
    rb->count = 0;
    rb->cap = 0;
}

// Public cache-fill entry point (see table_cached.h).
bool xsql_rowset_push(xsql_rowset *rs, const void *row) {
    if (!rs || !rs->rb || !row) {
        return false;
    }
    return xsqlc_rowbuf_push(rs->rb, row);
}

// ---------------------------------------------------------------------------
// Sorted index (range pushdown)
// ---------------------------------------------------------------------------

// Order by key ascending; break ties by row so equal keys keep data order (the
// entries are appended in ascending row order, so this is a stable sort — the
// per-key run stays in the original row sequence).
static int sorted_entry_cmp(const void *pa, const void *pb) {
    const xsqlc_sorted_entry *a = (const xsqlc_sorted_entry *)pa;
    const xsqlc_sorted_entry *b = (const xsqlc_sorted_entry *)pb;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    if (a->row < b->row) return -1;
    if (a->row > b->row) return 1;
    return 0;
}

bool xsqlc_sorted_index_build(xsqlc_sorted_index *sorted, const rowbuf *data,
                              xsql_key_extractor_fn key, void *user) {
    xsqlc_free(sorted->entries);
    sorted->entries = NULL;
    sorted->count = 0;
    sorted->cap = 0;
    sorted->built = false;
    if (data->count > 0) {
        size_t bytes;
        if (!xsqlc_mul_ok(data->count, sizeof(*sorted->entries), &bytes)) {
            return false;
        }
        sorted->entries = (xsqlc_sorted_entry *)xsqlc_malloc(bytes);
        if (!sorted->entries) {
            return false;
        }
        sorted->cap = data->count;
        for (size_t row = 0; row < data->count; ++row) {
            sorted->entries[row].key = key(xsqlc_rowbuf_at(data, row), user);
            sorted->entries[row].row = row;
        }
        sorted->count = data->count;
        qsort(sorted->entries, sorted->count, sizeof(*sorted->entries),
              sorted_entry_cmp);
    }
    sorted->built = true;
    return true;
}

void xsqlc_sorted_index_free(xsqlc_sorted_index *sorted) {
    xsqlc_free(sorted->entries);
    sorted->entries = NULL;
    sorted->count = 0;
    sorted->cap = 0;
    sorted->built = false;
}

// First index in [lo,hi) whose key is NOT < `k` (lower-bound bisection).
static size_t sorted_lower_bound(const xsqlc_sorted_entry *e, size_t lo,
                                 size_t hi, int64_t k) {
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e[mid].key < k) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// First index in [lo,hi) whose key is > `k` (upper-bound bisection).
static size_t sorted_upper_bound(const xsqlc_sorted_entry *e, size_t lo,
                                 size_t hi, int64_t k) {
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k < e[mid].key) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

bool xsqlc_sorted_index_collect(const xsqlc_sorted_index *sorted, int range_flags,
                                int64_t low, int64_t high, xsqlc_poslist *out) {
    const xsqlc_sorted_entry *e = sorted->entries;
    size_t begin = 0, end = sorted->count;
    if (range_flags & XSQLC_RANGE_HAS_LOW) {
        // >= low : first key not < low.  > low : first key > low (skip the run
        // equal to low).
        begin = sorted_lower_bound(e, 0, sorted->count, low);
        if (range_flags & XSQLC_RANGE_LOW_STRICT) {
            begin = sorted_upper_bound(e, begin, sorted->count, low);
        }
    }
    if (range_flags & XSQLC_RANGE_HAS_HIGH) {
        // <= high : one past the last key not > high (upper_bound of high).
        // < high  : first key not < high (lower_bound of high).
        end = (range_flags & XSQLC_RANGE_HIGH_STRICT)
                  ? sorted_lower_bound(e, begin, sorted->count, high)
                  : sorted_upper_bound(e, begin, sorted->count, high);
    }
    for (size_t i = begin; i < end; ++i) {
        if (!xsqlc_poslist_push(out, e[i].row)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shared cache struct
// ---------------------------------------------------------------------------

shared_cache *xsqlc_shared_cache_new(size_t row_size) {
    shared_cache *sc = (shared_cache *)xsqlc_malloc(sizeof(*sc));
    if (!sc) {
        return NULL;
    }
    memset(sc, 0, sizeof(*sc));
    sc->data.row_size = row_size;
    sc->mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
    return sc;
}

void xsqlc_cache_free_indexes(shared_cache *sc) {
    if (sc->indexes) {
        for (size_t i = 0; i < sc->nindexes; ++i) {
            xsqlc_i64map_free(&sc->indexes[i]);
        }
        xsqlc_free(sc->indexes);
        sc->indexes = NULL;
    }
    sc->nindexes = 0;
    // The parallel range views are invalidated together with the hash indexes:
    // every point that clears `indexes` (rebuild, INSERT invalidation, post-
    // mutation invalidation, free) also clears `sorted_indexes` — the two index
    // structures share a single lifecycle and are never freed independently.
    if (sc->sorted_indexes) {
        for (size_t i = 0; i < sc->nsorted; ++i) {
            xsqlc_sorted_index_free(&sc->sorted_indexes[i]);
        }
        xsqlc_free(sc->sorted_indexes);
        sc->sorted_indexes = NULL;
    }
    sc->nsorted = 0;
}

void xsqlc_shared_cache_free(shared_cache *sc) {
    if (!sc) {
        return;
    }
    xsqlc_cache_free_indexes(sc);
    xsqlc_rowbuf_free(&sc->data);
    sqlite3_mutex_free(sc->mutex);
    xsqlc_free(sc);
}

// ---------------------------------------------------------------------------
// Lazy build + invalidation
// ---------------------------------------------------------------------------

// Build the hash indexes over the current cache data. Returns false on OOM.
static bool build_indexes(const struct xsql_cached_table *def, shared_cache *sc) {
    if (def->nindexes == 0) {
        return true;
    }
    size_t idx_bytes;
    if (!xsqlc_mul_ok(def->nindexes, sizeof(*sc->indexes), &idx_bytes)) {
        return false;
    }
    sc->indexes = (xsqlc_i64map *)xsqlc_malloc(idx_bytes);
    if (!sc->indexes) {
        return false;
    }
    sc->nindexes = def->nindexes;
    for (size_t i = 0; i < def->nindexes; ++i) {
        xsqlc_i64map_init(&sc->indexes[i]);
    }
    // Size the parallel sorted-view slots alongside the hash indexes; entries are
    // built lazily on the first range query (see the range branch in xFilter).
    size_t sorted_bytes;
    if (!xsqlc_mul_ok(def->nindexes, sizeof(*sc->sorted_indexes), &sorted_bytes)) {
        return false;
    }
    sc->sorted_indexes = (xsqlc_sorted_index *)xsqlc_malloc(sorted_bytes);
    if (!sc->sorted_indexes) {
        return false;
    }
    sc->nsorted = def->nindexes;
    memset(sc->sorted_indexes, 0, sorted_bytes);
    for (size_t i = 0; i < def->nindexes; ++i) {
        const cached_index_def *ix = &def->index_defs[i];
        for (size_t row = 0; row < sc->data.count; ++row) {
            const void *r = xsqlc_rowbuf_at(&sc->data, row);
            int64_t key = ix->key(r, ix->user);
            if (!xsqlc_i64map_add(&sc->indexes[i], key, row)) {
                return false;
            }
        }
    }
    return true;
}

bool xsqlc_cache_ensure_built(const struct xsql_cached_table *def) {
    if (!def->use_shared_cache) {
        return true;  // non-shared: nothing to build here
    }
    shared_cache *sc = def->shared;
    sqlite3_mutex_enter(sc->mutex);
    if (sc->built) {
        sqlite3_mutex_leave(sc->mutex);
        return true;
    }

    xsqlc_rowbuf_clear(&sc->data);
    xsqlc_cache_free_indexes(sc);
    sc->mutation_snapshot = false;

    if (def->cache_builder_fn) {
        struct xsql_rowset rs = {&sc->data};
        def->cache_builder_fn(&rs, def->cache_builder_user);
        if (xsqlc_vtab_has_error()) {
            // Leave the cache empty + unbuilt; the error stays in the channel.
            xsqlc_rowbuf_clear(&sc->data);
            xsqlc_cache_free_indexes(sc);
            sc->built = false;
            sc->mutation_snapshot = false;
            sqlite3_mutex_leave(sc->mutex);
            return false;
        }
    }

    if (!build_indexes(def, sc)) {
        xsqlc_cache_free_indexes(sc);
        sqlite3_mutex_leave(sc->mutex);
        return false;
    }
    sc->built = true;
    sqlite3_mutex_leave(sc->mutex);
    return true;
}

void xsqlc_cache_invalidate(const struct xsql_cached_table *def) {
    shared_cache *sc = def->shared;
    if (!sc) {
        return;
    }
    sqlite3_mutex_enter(sc->mutex);
    xsqlc_rowbuf_clear(&sc->data);
    xsqlc_cache_free_indexes(sc);
    sc->built = false;
    sc->mutation_snapshot = false;
    ++sc->write_generation;
    sqlite3_mutex_leave(sc->mutex);
}

void xsqlc_cache_invalidate_after_mutation(const struct xsql_cached_table *def) {
    shared_cache *sc = def->shared;
    if (!sc) {
        return;
    }
    sqlite3_mutex_enter(sc->mutex);
    xsqlc_cache_free_indexes(sc);
    sc->built = false;
    // Keep `data` alive so an in-flight scan-driven UPDATE/DELETE still sees every
    // row (A1); only mark a snapshot when there is data to preserve (A16).
    sc->mutation_snapshot = sc->data.count > 0;
    ++sc->write_generation;
    sqlite3_mutex_leave(sc->mutex);
}

void xsqlc_cache_bump_write_generation(const struct xsql_cached_table *def) {
    shared_cache *sc = def->shared;
    if (!sc) {
        return;
    }
    sqlite3_mutex_enter(sc->mutex);
    ++sc->write_generation;
    sqlite3_mutex_leave(sc->mutex);
}

uint64_t xsqlc_cache_write_generation(const struct xsql_cached_table *def) {
    shared_cache *sc = def->shared;
    if (!sc) {
        return 0;
    }
    sqlite3_mutex_enter(sc->mutex);
    const uint64_t g = sc->write_generation;
    sqlite3_mutex_leave(sc->mutex);
    return g;
}
