// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// hashmap.c — see hashmap.h. Open addressing with linear probing and a
// power-of-two capacity; grows at a 0.7 load factor. Keys are int64 mixed with a
// SplitMix64-style finalizer for a good probe distribution.

#include "internal/hashmap.h"

#include "internal/internal.h"

#include <string.h>

void xsqlc_i64map_init(xsqlc_i64map *m) {
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
}

void xsqlc_i64map_free(xsqlc_i64map *m) {
    if (m->slots) {
        for (size_t i = 0; i < m->cap; ++i) {
            if (m->slots[i].used) {
                xsqlc_free(m->slots[i].list.items);
            }
        }
        xsqlc_free(m->slots);
    }
    xsqlc_i64map_init(m);
}

static size_t mix_key(int64_t key) {
    uint64_t z = (uint64_t)key + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (size_t)z;
}

bool xsqlc_poslist_push(xsqlc_poslist *pl, size_t pos) {
    if (pl->count == pl->cap) {
        size_t new_cap = pl->cap ? pl->cap * 2 : 4;
        size_t bytes;
        if (!xsqlc_mul_ok(new_cap, sizeof(*pl->items), &bytes)) {
            return false;
        }
        size_t *grown = (size_t *)xsqlc_realloc(pl->items, bytes);
        if (!grown) {
            return false;
        }
        pl->items = grown;
        pl->cap = new_cap;
    }
    pl->items[pl->count++] = pos;
    return true;
}

void xsqlc_poslist_clear(xsqlc_poslist *pl) { pl->count = 0; }

void xsqlc_poslist_free(xsqlc_poslist *pl) {
    xsqlc_free(pl->items);
    pl->items = NULL;
    pl->count = 0;
    pl->cap = 0;
}

// Internal shorthand kept for the existing i64map_add call sites.
#define poslist_push xsqlc_poslist_push

// Find the slot for `key` (returns its index if present, else the first free
// slot to insert into). Requires cap > 0.
static size_t probe(const xsqlc_i64map_slot *slots, size_t cap, int64_t key,
                    bool *found) {
    size_t mask = cap - 1;
    size_t i = mix_key(key) & mask;
    for (;;) {
        if (!slots[i].used) {
            *found = false;
            return i;
        }
        if (slots[i].key == key) {
            *found = true;
            return i;
        }
        i = (i + 1) & mask;
    }
}

static bool grow(xsqlc_i64map *m) {
    size_t new_cap = m->cap ? m->cap * 2 : 16;
    size_t bytes;
    if (!xsqlc_mul_ok(new_cap, sizeof(xsqlc_i64map_slot), &bytes)) {
        return false;
    }
    xsqlc_i64map_slot *ns = (xsqlc_i64map_slot *)xsqlc_malloc(bytes);
    if (!ns) {
        return false;
    }
    memset(ns, 0, bytes);
    for (size_t i = 0; i < m->cap; ++i) {
        if (!m->slots[i].used) {
            continue;
        }
        bool found = false;
        size_t j = probe(ns, new_cap, m->slots[i].key, &found);
        ns[j] = m->slots[i];  // move slot (incl. its list pointer)
    }
    xsqlc_free(m->slots);
    m->slots = ns;
    m->cap = new_cap;
    return true;
}

bool xsqlc_i64map_add(xsqlc_i64map *m, int64_t key, size_t pos) {
    // Grow at ~0.7 load (also handles the empty-map case).
    if (m->cap == 0 || (m->count + 1) * 10 >= m->cap * 7) {
        if (!grow(m)) {
            return false;
        }
    }
    bool found = false;
    size_t i = probe(m->slots, m->cap, key, &found);
    if (!found) {
        m->slots[i].used = true;
        m->slots[i].key = key;
        m->slots[i].list.items = NULL;
        m->slots[i].list.count = 0;
        m->slots[i].list.cap = 0;
        m->count++;
    }
    if (!poslist_push(&m->slots[i].list, pos)) {
        if (!found) {
            // The key was freshly inserted at the tail of its probe chain (the
            // first free slot). Its poslist is empty and no later insert probed
            // past it, so clearing `used` here does not break any probe chain —
            // leaving the phantom key would corrupt lookups for a colliding key.
            xsqlc_poslist_free(&m->slots[i].list);
            m->slots[i].used = false;
            m->count--;
        }
        return false;
    }
    return true;
}

const xsqlc_poslist *xsqlc_i64map_get(const xsqlc_i64map *m, int64_t key) {
    if (m->cap == 0) {
        return NULL;
    }
    bool found = false;
    size_t i = probe(m->slots, m->cap, key, &found);
    return found ? &m->slots[i].list : NULL;
}
