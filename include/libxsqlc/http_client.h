// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// http_client.h — the thin-client HTTP client, over the vendored civetweb client
// transport (an internal detail — no civetweb type appears here). Response bodies
// are heap strings allocated by the library; free them with xsql_free().
//
// Feature-gated like http_server.h: the types stay declared when the library is
// built without XSQL_HAS_THINCLIENT, and every function becomes a no-op stub.

#ifndef XSQLC_HTTP_CLIENT_H
#define XSQLC_HTTP_CLIENT_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque client handle.
typedef struct xsql_http_client xsql_http_client;

// Client configuration. Designated-initializer friendly; struct_size 0 selects
// defaults. Strings are copied at _new.
typedef struct xsql_http_client_config {
    size_t      struct_size;   // sizeof(xsql_http_client_config), or 0
    const char *host;          // NULL => "127.0.0.1"
    int         port;          // default 5555 (A13: the client keeps 5555)
    int         timeout_sec;   // default 30
    const char *auth_token;    // optional; sent as the X-XSQL-Token header
} xsql_http_client_config;

#ifdef XSQL_HAS_THINCLIENT

// Create a client. `cfg` may be NULL (all defaults). Returns XSQL_OK (*out set),
// XSQL_NO_MEMORY on failure; *out is NULL on failure.
XSQL_API int  xsql_http_client_new(const xsql_http_client_config *cfg,
                                   xsql_http_client **out);
// Release a client. Passing NULL is a no-op.
XSQL_API void xsql_http_client_free(xsql_http_client *c);

// POST /query with the SQL as a text/plain body. On success (HTTP 200) returns
// XSQL_OK with the response body in *out (heap; free with xsql_free). On an HTTP
// error returns XSQL_ERROR with the server's error body in *out when available.
// On a connection failure returns XSQL_IO_ERROR with a diagnostic in *out.
XSQL_API int  xsql_http_client_query(xsql_http_client *c, const char *sql,
                                     char **out);
// GET /status. Same return contract as query.
XSQL_API int  xsql_http_client_status(xsql_http_client *c, char **out);
// POST /cancel. Returns XSQL_OK when cancellation was accepted, XSQL_ERROR when
// the configured server executor cannot cancel, or XSQL_IO_ERROR on transport
// failure.
XSQL_API int  xsql_http_client_cancel(xsql_http_client *c);
// POST /shutdown (best-effort; the server may close the connection first).
XSQL_API int  xsql_http_client_shutdown(xsql_http_client *c);
// True if GET /status returns HTTP 200.
XSQL_API bool xsql_http_client_ping(xsql_http_client *c);

#else  // !XSQL_HAS_THINCLIENT

static inline int xsql_http_client_new(const xsql_http_client_config *cfg,
                                       xsql_http_client **out) {
    (void)cfg;
    if (out) {
        *out = NULL;
    }
    return XSQL_ERROR;
}
static inline void xsql_http_client_free(xsql_http_client *c) { (void)c; }
static inline int  xsql_http_client_query(xsql_http_client *c, const char *sql,
                                          char **out) {
    (void)c; (void)sql;
    if (out) {
        *out = NULL;
    }
    return XSQL_ERROR;
}
static inline int  xsql_http_client_status(xsql_http_client *c, char **out) {
    (void)c;
    if (out) {
        *out = NULL;
    }
    return XSQL_ERROR;
}
static inline int  xsql_http_client_cancel(xsql_http_client *c) {
    (void)c;
    return XSQL_ERROR;
}
static inline int  xsql_http_client_shutdown(xsql_http_client *c) {
    (void)c;
    return XSQL_ERROR;
}
static inline bool xsql_http_client_ping(xsql_http_client *c) {
    (void)c;
    return false;
}

#endif  // XSQL_HAS_THINCLIENT

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_HTTP_CLIENT_H
