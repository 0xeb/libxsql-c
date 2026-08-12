// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// json_parse.c — POST /query request-body resolution: resolve the SQL text +
// continue_on_error / include_sql flags from either a raw-SQL body or a JSON
// object. Only JSON
// *parsing* lives in the thin client; emission is the house pattern in
// json_emit.c. This is a thin adapter over the internal tolerant DOM
// (internal/json.h) — the fixed schema is exactly the object it recognizes.

#include "thinclient/http_internal.h"

#include "internal/internal.h"
#include "internal/json.h"

#include <string.h>

// bool | number!=0 | "1" | "true".
static bool read_flag(const xsqlc_jval *obj, const char *key) {
    const xsqlc_jval *v = xsqlc_json_member(obj, key);
    if (!v) {
        return false;
    }
    if (v->type == XSQLC_JBOOL) {
        return v->bval;
    }
    if (v->type == XSQLC_JNUM) {
        return v->num != 0;
    }
    if (v->type == XSQLC_JSTR && v->str) {
        return strcmp(v->str, "1") == 0 || strcmp(v->str, "true") == 0;
    }
    return false;
}

// Copy `n` bytes as a NUL-terminated heap string (xsqlc allocator).
static char *dup_n(const char *s, size_t n) {
    char *out = (char *)xsqlc_malloc(n + 1);
    if (!out) {
        return NULL;
    }
    if (n) {
        memcpy(out, s, n);
    }
    out[n] = '\0';
    return out;
}

xsqlc_qbody_status xsqlc_resolve_query_body(const char *body, bool declared_json,
                                            char **sql_out,
                                            bool *continue_on_error_out,
                                            bool *include_sql_out) {
    *sql_out = NULL;
    *continue_on_error_out = false;
    *include_sql_out = false;

    if (!body) {
        return XSQLC_QBODY_OK;  // caller treats NULL sql as "Empty query"
    }

    // Raw SQL unless the body declares/looks like JSON (declared
    // application/json OR a leading '{').
    const bool looks_json = declared_json || body[0] == '{';
    if (!looks_json) {
        *sql_out = xsqlc_strdup(body);
        return XSQLC_QBODY_OK;
    }

    xsqlc_jval *j = xsqlc_json_parse(body);
    const bool parsed = (j != NULL);
    const xsqlc_jval *sqlv = xsqlc_json_member(j, "sql");
    const bool has_sql = sqlv && sqlv->type == XSQLC_JSTR;

    if (has_sql) {
        *sql_out = dup_n(sqlv->str ? sqlv->str : "", sqlv->strlen);
        *continue_on_error_out = read_flag(j, "continue_on_error");
        *include_sql_out = read_flag(j, "include_sql");
        xsqlc_json_free(j);
        return XSQLC_QBODY_OK;
    }

    if (declared_json) {
        // Declared JSON but not a usable {"sql": "..."} object -> reject.
        xsqlc_json_free(j);
        return parsed ? XSQLC_QBODY_MISSING_SQL : XSQLC_QBODY_MALFORMED;
    }

    // Undeclared leading-'{' that isn't valid JSON-with-sql -> treat as raw SQL.
    xsqlc_json_free(j);
    *sql_out = xsqlc_strdup(body);
    return XSQLC_QBODY_OK;
}
