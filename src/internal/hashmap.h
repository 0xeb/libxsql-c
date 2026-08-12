// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// hashmap.h — an open-addressing (linear-probe, power-of-two) hash map from an
// int64 key to a list of row positions. Backs the cached-table `index_on` lazy
// hash index and the shared-cache bookkeeping. PRIVATE.

#ifndef XSQLC_INTERNAL_HASHMAP_H
#define XSQLC_INTERNAL_HASHMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A growable list of row positions (the map's value type).
typedef struct xsqlc_poslist {
    size_t *items;
    size_t  count;
    size_t  cap;
} xsqlc_poslist;

typedef struct xsqlc_i64map_slot {
    int64_t       key;
    bool          used;
    xsqlc_poslist list;
} xsqlc_i64map_slot;

typedef struct xsqlc_i64map {
    xsqlc_i64map_slot *slots;
    size_t             cap;    // power of two (0 until first insert)
    size_t             count;  // occupied slots (distinct keys)
} xsqlc_i64map;

// Zero-initialize (no allocation).
void xsqlc_i64map_init(xsqlc_i64map *m);

// Release all slots and their position lists; resets to empty.
void xsqlc_i64map_free(xsqlc_i64map *m);

// Append `pos` to the position list for `key` (creating the key if absent).
// Returns false on allocation failure.
bool xsqlc_i64map_add(xsqlc_i64map *m, int64_t key, size_t pos);

// Look up a key's position list, or NULL if the key is absent.
const xsqlc_poslist *xsqlc_i64map_get(const xsqlc_i64map *m, int64_t key);

// Append `pos` to a standalone position list (grows on demand). Returns false on
// allocation failure. Used by the cached-table range window (a cursor-owned list).
bool xsqlc_poslist_push(xsqlc_poslist *pl, size_t pos);

// Reset a position list to empty (count = 0), retaining its capacity.
void xsqlc_poslist_clear(xsqlc_poslist *pl);

// Release a position list's backing storage and reset it to empty.
void xsqlc_poslist_free(xsqlc_poslist *pl);

#endif  // XSQLC_INTERNAL_HASHMAP_H
