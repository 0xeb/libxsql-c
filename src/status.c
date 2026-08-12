// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// status.c — status-name lookup, the public xsql_free() boundary, and the
// internal allocator wrappers it is built on.

#include <libxsqlc/status.h>
#include <libxsqlc/common.h>

#include "internal/internal.h"

#include <sqlite3.h>

#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Allocator (single boundary — see internal.h)
// ---------------------------------------------------------------------------

#if defined(XSQLC_TEST_ALLOC_HOOKS)
static long long g_alloc_fail_countdown = -1;

void xsqlc_test_alloc_fail_after(long long countdown) {
    g_alloc_fail_countdown = countdown;
}

void xsqlc_test_alloc_fail_reset(void) {
    g_alloc_fail_countdown = -1;
}

static bool xsqlc_test_should_fail_alloc(void) {
    if (g_alloc_fail_countdown < 0) {
        return false;
    }
    if (g_alloc_fail_countdown == 0) {
        g_alloc_fail_countdown = -1;
        return true;
    }
    g_alloc_fail_countdown--;
    return false;
}
#endif

void *xsqlc_malloc(size_t size) {
    // sqlite3_malloc takes an int; guard against overflow on LP64 callers.
    if (size > (size_t)0x7fffffff) {
        return NULL;
    }
#if defined(XSQLC_TEST_ALLOC_HOOKS)
    if (xsqlc_test_should_fail_alloc()) {
        return NULL;
    }
#endif
    return sqlite3_malloc((int)size);
}

void *xsqlc_realloc(void *ptr, size_t size) {
    if (size > (size_t)0x7fffffff) {
        return NULL;
    }
#if defined(XSQLC_TEST_ALLOC_HOOKS)
    if (xsqlc_test_should_fail_alloc()) {
        return NULL;
    }
#endif
    return sqlite3_realloc(ptr, (int)size);
}

void xsqlc_free(void *ptr) {
    sqlite3_free(ptr);
}

char *xsqlc_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *copy = (char *)xsqlc_malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

void xsqlc_str_set(char **slot, const char *s) {
    char *copy = xsqlc_strdup(s);  // NULL when s is NULL or on allocation failure
    xsqlc_free(*slot);
    *slot = copy;
}

void xsql_free(void *ptr) {
    xsqlc_free(ptr);
}

// ---------------------------------------------------------------------------
// Status names
// ---------------------------------------------------------------------------

const char *xsql_status_str(int status) {
    switch (status) {
        case XSQL_OK:         return "ok";
        case XSQL_ERROR:      return "error";
        case XSQL_INTERNAL:   return "internal";
        case XSQL_PERM:       return "perm";
        case XSQL_ABORT:      return "abort";
        case XSQL_BUSY:       return "busy";
        case XSQL_LOCKED:     return "locked";
        case XSQL_NO_MEMORY:  return "no_memory";
        case XSQL_READ_ONLY:  return "read_only";
        case XSQL_INTERRUPT:  return "interrupt";
        case XSQL_IO_ERROR:   return "io_error";
        case XSQL_CORRUPT:    return "corrupt";
        case XSQL_NOT_FOUND:  return "not_found";
        case XSQL_FULL:       return "full";
        case XSQL_CANT_OPEN:  return "cant_open";
        case XSQL_PROTOCOL:   return "protocol";
        case XSQL_EMPTY:      return "empty";
        case XSQL_SCHEMA:     return "schema";
        case XSQL_TOO_BIG:    return "too_big";
        case XSQL_CONSTRAINT: return "constraint";
        case XSQL_MISMATCH:   return "mismatch";
        case XSQL_MISUSE:     return "misuse";
        case XSQL_NO_LFS:     return "no_lfs";
        case XSQL_AUTH:       return "auth";
        case XSQL_FORMAT:     return "format";
        case XSQL_RANGE:      return "range";
        case XSQL_NOT_A_DB:   return "not_a_db";
        case XSQL_NOTICE:     return "notice";
        case XSQL_WARNING:    return "warning";
        case XSQL_ROW:        return "row";
        case XSQL_DONE:       return "done";
        default:              return "unknown";
    }
}
