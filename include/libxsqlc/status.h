// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// status.h — result/status codes.
//
// Every fallible libxsql-c function returns an int carrying one of these
// values. The numeric values are identical to the corresponding SQLite result
// codes (XSQL_OK == SQLITE_OK, XSQL_ROW == SQLITE_ROW, ...), so a status can be
// compared or forwarded directly against a raw sqlite3 return code.

#ifndef XSQLC_STATUS_H
#define XSQLC_STATUS_H

#include <libxsqlc/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum xsql_status {
    XSQL_OK         = 0,
    XSQL_ERROR      = 1,
    XSQL_INTERNAL   = 2,
    XSQL_PERM       = 3,
    XSQL_ABORT      = 4,
    XSQL_BUSY       = 5,
    XSQL_LOCKED     = 6,
    XSQL_NO_MEMORY  = 7,   // SQLITE_NOMEM
    XSQL_READ_ONLY  = 8,   // SQLITE_READONLY
    XSQL_INTERRUPT  = 9,
    XSQL_IO_ERROR   = 10,  // SQLITE_IOERR
    XSQL_CORRUPT    = 11,
    XSQL_NOT_FOUND  = 12,  // SQLITE_NOTFOUND
    XSQL_FULL       = 13,
    XSQL_CANT_OPEN  = 14,
    XSQL_PROTOCOL   = 15,
    XSQL_EMPTY      = 16,
    XSQL_SCHEMA     = 17,
    XSQL_TOO_BIG    = 18,
    XSQL_CONSTRAINT = 19,
    XSQL_MISMATCH   = 20,
    XSQL_MISUSE     = 21,
    XSQL_NO_LFS     = 22,  // SQLITE_NOLFS
    XSQL_AUTH       = 23,
    XSQL_FORMAT     = 24,
    XSQL_RANGE      = 25,
    XSQL_NOT_A_DB   = 26,  // SQLITE_NOTADB
    XSQL_NOTICE     = 27,
    XSQL_WARNING    = 28,
    XSQL_ROW        = 100, // SQLITE_ROW
    XSQL_DONE       = 101  // SQLITE_DONE
} xsql_status;

// Human-readable name for a status code (borrowed static string; never NULL —
// unknown codes return "unknown"). Accepts an int so a raw sqlite3 code works.
XSQL_API const char *xsql_status_str(int status);

// Predicates. Accept an int so raw sqlite3 return codes work unchanged.
#define XSQL_IS_OK(s)   ((s) == XSQL_OK)
#define XSQL_IS_ROW(s)  ((s) == XSQL_ROW)
#define XSQL_IS_DONE(s) ((s) == XSQL_DONE)

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_STATUS_H
