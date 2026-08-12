// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// vec.h — the internal growable-array helper backing the small owned lists in
// the vtable builders (columns, filters, index defs). A single geometric-growth
// reserve over the xsqlc allocator; callers keep their own count. Header-only.
//
// PRIVATE: under src/internal, never installed, never in a public header.

#ifndef XSQLC_INTERNAL_VEC_H
#define XSQLC_INTERNAL_VEC_H

#include <stdbool.h>
#include <stddef.h>

#include "internal/internal.h"

// Ensure `*arr` has room for at least `need` elements of `elem` bytes, growing
// geometrically (doubling from a small floor). Returns false on allocation
// failure, leaving `*arr`/`*cap` untouched. `*arr` may be NULL / `*cap` 0.
static inline bool xsqlc_vec_reserve(void **arr, size_t *cap, size_t need,
                                     size_t elem) {
    if (need <= *cap) {
        return true;
    }
    size_t new_cap = *cap ? *cap : 4;
    while (new_cap < need) {
        if (new_cap > ((size_t)-1) / 2) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }
    size_t bytes;
    if (!xsqlc_mul_ok(new_cap, elem, &bytes)) {
        return false;
    }
    void *grown = xsqlc_realloc(*arr, bytes);
    if (!grown) {
        return false;
    }
    *arr = grown;
    *cap = new_cap;
    return true;
}

#endif  // XSQLC_INTERNAL_VEC_H
