// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

#ifndef XSQLC_CAPABILITIES_H
#define XSQLC_CAPABILITIES_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>

#include <stdbool.h>
#include <stddef.h>

struct xsql_db;

#ifdef __cplusplus
extern "C" {
#endif

// One row in the canonical capability surface. Strings are copied by
// xsql_capabilities_new(), so callers may release them after construction.
typedef struct xsql_capability {
    const char *name;
    bool        is_supported;
    const char *notes;
} xsql_capability;

// Opaque, immutable definition of:
//
//   sql_capabilities(name TEXT, is_supported INTEGER, notes TEXT)
//
// Rows are sorted by name. Empty and duplicate names are rejected.
typedef struct xsql_capabilities xsql_capabilities;

// Build a capability definition. On success, returns XSQL_OK and stores an owned
// handle in *out. Returns XSQL_MISUSE for invalid/duplicate names and
// XSQL_NO_MEMORY on allocation failure.
XSQL_API int xsql_capabilities_new(const xsql_capability *rows, size_t count,
                                   xsql_capabilities **out);
XSQL_API void xsql_capabilities_free(xsql_capabilities *capabilities);

// Register and create the canonical read-only sql_capabilities virtual table.
// The definition must outlive the database registration.
XSQL_API int xsql_capabilities_register(struct xsql_db *db,
                                        xsql_capabilities *capabilities);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_CAPABILITIES_H
