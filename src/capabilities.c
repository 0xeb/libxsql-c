// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

#include <libxsqlc/capabilities.h>

#include <libxsqlc/database.h>
#include <libxsqlc/table_cached.h>
#include <libxsqlc/value.h>

#include "internal/internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct capability_row {
    char *name;
    int   is_supported;
    char *notes;
} capability_row;

struct xsql_capabilities {
    capability_row    *rows;
    size_t             count;
    xsql_cached_table *definition;
};

static int compare_capabilities(const void *left, const void *right) {
    const capability_row *a = (const capability_row *)left;
    const capability_row *b = (const capability_row *)right;
    return strcmp(a->name, b->name);
}

static size_t capability_count(void *user) {
    return ((const xsql_capabilities *)user)->count;
}

static void capability_build(xsql_rowset *rowset, void *user) {
    const xsql_capabilities *capabilities =
        (const xsql_capabilities *)user;
    for (size_t i = 0; i < capabilities->count; ++i) {
        if (!xsql_rowset_push(rowset, &capabilities->rows[i])) {
            return;
        }
    }
}

static void emit_name(xsql_context *ctx, const void *row, void *user) {
    (void)user;
    const capability_row *capability = (const capability_row *)row;
    xsql_result_text(ctx, capability->name, -1, XSQL_TRANSIENT);
}

static int get_supported(const void *row, void *user) {
    (void)user;
    return ((const capability_row *)row)->is_supported;
}

static void emit_notes(xsql_context *ctx, const void *row, void *user) {
    (void)user;
    const capability_row *capability = (const capability_row *)row;
    xsql_result_text(ctx, capability->notes, -1, XSQL_TRANSIENT);
}

int xsql_capabilities_new(const xsql_capability *rows, size_t count,
                          xsql_capabilities **out) {
    if (!out || (count != 0 && !rows)) {
        return XSQL_MISUSE;
    }
    *out = NULL;
    xsql_capabilities *capabilities =
        (xsql_capabilities *)xsqlc_malloc(sizeof(*capabilities));
    if (!capabilities) {
        return XSQL_NO_MEMORY;
    }
    memset(capabilities, 0, sizeof(*capabilities));
    if (count != 0) {
        size_t bytes = 0;
        if (!xsqlc_mul_ok(count, sizeof(*capabilities->rows), &bytes)) {
            xsql_capabilities_free(capabilities);
            return XSQL_NO_MEMORY;
        }
        capabilities->rows = (capability_row *)xsqlc_malloc(bytes);
        if (!capabilities->rows) {
            xsql_capabilities_free(capabilities);
            return XSQL_NO_MEMORY;
        }
        memset(capabilities->rows, 0, bytes);
    }
    capabilities->count = count;
    for (size_t i = 0; i < count; ++i) {
        if (!rows[i].name || rows[i].name[0] == '\0') {
            xsql_capabilities_free(capabilities);
            return XSQL_MISUSE;
        }
        capabilities->rows[i].name = xsqlc_strdup(rows[i].name);
        capabilities->rows[i].notes =
            xsqlc_strdup(rows[i].notes ? rows[i].notes : "");
        capabilities->rows[i].is_supported =
            rows[i].is_supported ? 1 : 0;
        if (!capabilities->rows[i].name ||
            !capabilities->rows[i].notes) {
            xsql_capabilities_free(capabilities);
            return XSQL_NO_MEMORY;
        }
    }
    if (count > 1) {
        qsort(capabilities->rows, count, sizeof(*capabilities->rows),
              compare_capabilities);
    }
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(capabilities->rows[i - 1].name,
                   capabilities->rows[i].name) == 0) {
            xsql_capabilities_free(capabilities);
            return XSQL_MISUSE;
        }
    }

    capabilities->definition =
        xsql_cached_table_new("sql_capabilities", sizeof(capability_row));
    if (!capabilities->definition) {
        xsql_capabilities_free(capabilities);
        return XSQL_NO_MEMORY;
    }
    xsql_cached_count(capabilities->definition, capability_count,
                      capabilities);
    xsql_cached_estimate_rows(capabilities->definition, capability_count,
                              capabilities);
    xsql_cached_cache_builder(capabilities->definition, capability_build,
                              capabilities);
    if (xsql_cached_col_text(capabilities->definition, "name", emit_name,
                             capabilities) != XSQL_OK ||
        xsql_cached_col_int(capabilities->definition, "is_supported",
                            get_supported, capabilities) != XSQL_OK ||
        xsql_cached_col_text(capabilities->definition, "notes", emit_notes,
                             capabilities) != XSQL_OK) {
        xsql_capabilities_free(capabilities);
        return XSQL_NO_MEMORY;
    }
    *out = capabilities;
    return XSQL_OK;
}

void xsql_capabilities_free(xsql_capabilities *capabilities) {
    if (!capabilities) {
        return;
    }
    xsql_cached_table_free(capabilities->definition);
    for (size_t i = 0; i < capabilities->count; ++i) {
        xsqlc_free(capabilities->rows[i].name);
        xsqlc_free(capabilities->rows[i].notes);
    }
    xsqlc_free(capabilities->rows);
    xsqlc_free(capabilities);
}

int xsql_capabilities_register(xsql_db *db,
                               xsql_capabilities *capabilities) {
    if (!db || !capabilities || !capabilities->definition) {
        return XSQL_MISUSE;
    }
    return xsql_cached_table_register(db, capabilities->definition);
}
