// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// thinclient_json.h — small string-based JSON helpers for building thin-client
// responses. These are
// the lightweight "build a fixed envelope" helpers a consumer uses inside its own
// query_fn / route handler; the full canonical script envelope is rendered by
// xsql_script_result_to_json() (format.h).
//
// Feature-gated like the rest of the thin client (built under XSQL_HAS_THINCLIENT);
// no-op stubs otherwise.

#ifndef XSQLC_THINCLIENT_JSON_H
#define XSQLC_THINCLIENT_JSON_H

#include <libxsqlc/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XSQL_HAS_THINCLIENT

// Escape `s` for embedding inside a JSON string literal, WITHOUT the surrounding
// quotes. Specials " \ \n \r \t are backslash-escaped; every other byte < 0x20 is
// \u00xx (lowercase hex). Heap; free with xsql_free (NULL only on allocation
// failure). A NULL input escapes to "".
XSQL_API char *xsql_json_escape(const char *s);

// {"success":false,"error":"<escaped error>"}. Heap; free with xsql_free.
XSQL_API char *xsql_make_error_json(const char *error);

// {"success":true} when message is NULL/empty, else
// {"success":true,"message":"<escaped message>"}. Heap; free with xsql_free.
XSQL_API char *xsql_make_success_json(const char *message);

// {"success":true,"status":"ok","tool":"<escaped tool>"} with `extra_json`
// (a raw "k":v,... fragment WITHOUT braces, may be NULL/empty) spliced in before
// the closing brace. Heap; free with xsql_free.
XSQL_API char *xsql_make_status_json(const char *tool, const char *extra_json);

#else  // !XSQL_HAS_THINCLIENT

static inline char *xsql_json_escape(const char *s) {
    (void)s;
    return NULL;
}
static inline char *xsql_make_error_json(const char *error) {
    (void)error;
    return NULL;
}
static inline char *xsql_make_success_json(const char *message) {
    (void)message;
    return NULL;
}
static inline char *xsql_make_status_json(const char *tool,
                                          const char *extra_json) {
    (void)tool; (void)extra_json;
    return NULL;
}

#endif  // XSQL_HAS_THINCLIENT

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_THINCLIENT_JSON_H
