// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// http_internal.h — cross-TU declarations shared by the thin-client sources
// (http_server.c, json_parse.c, merge_patch.c). PRIVATE: under src/thinclient,
// included only by those .c files, never installed, never in a public header.
// It deliberately does NOT pull in civetweb.h — only http_server.c and client.c
// touch the transport.

#ifndef XSQLC_THINCLIENT_HTTP_INTERNAL_H
#define XSQLC_THINCLIENT_HTTP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// json_parse.c — POST /query request-body resolution (fixed schema)
// ---------------------------------------------------------------------------

typedef enum xsqlc_qbody_status {
    XSQLC_QBODY_OK = 0,       // resolved (raw SQL or a valid {"sql":...} object)
    XSQLC_QBODY_MALFORMED,    // Content-Type: application/json but unparseable
    XSQLC_QBODY_MISSING_SQL   // declared JSON but not an object with string "sql"
} xsqlc_qbody_status;

// Resolve a request body per the two accepted shapes (raw SQL, or a JSON object
// {"sql","continue_on_error","include_sql"}). `declared_json` is true when the
// request declared Content-Type: application/json. On XSQLC_QBODY_OK, *sql_out is
// a heap copy of the SQL (free with xsql_free; NULL only on allocation failure)
// and the flag outs are filled from the JSON body (false for the raw path). On an
// error status, *sql_out is NULL. Pointer outs may not be NULL.
xsqlc_qbody_status xsqlc_resolve_query_body(const char *body, bool declared_json,
                                            char **sql_out,
                                            bool *continue_on_error_out,
                                            bool *include_sql_out);

// ---------------------------------------------------------------------------
// merge_patch.c — RFC 7386 status merge (A5)
// ---------------------------------------------------------------------------

// Deep-merge the JSON `patch` onto the JSON `base` (RFC 7386: recursive object
// merge, a null member deletes the key, a non-object patch replaces the target).
// On success stores a heap merged JSON string in *out (free with xsql_free) and
// returns XSQL_OK. Returns XSQL_ERROR (*out = NULL) when `patch` is unparseable,
// XSQL_NO_MEMORY on allocation failure.
int xsqlc_status_merge_patch(const char *base, const char *patch, char **out);

#endif  // XSQLC_THINCLIENT_HTTP_INTERNAL_H
