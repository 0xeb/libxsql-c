// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// clipboard.h — thin-client clipboard/URL/payload helpers: host normalization,
// HTTP/MCP clipboard payload builders, server-output parsing, and a Windows
// clipboard copy (no-op elsewhere; macOS is intentionally deferred). All heap
// results are released with xsql_free().
//
// Feature-gated with the rest of the thin client (built under
// XSQL_HAS_THINCLIENT); no-op stubs otherwise.

#ifndef XSQLC_CLIPBOARD_H
#define XSQLC_CLIPBOARD_H

#include <libxsqlc/common.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XSQL_HAS_THINCLIENT

// Empty/NULL host => "127.0.0.1". Heap; free with xsql_free (NULL on OOM).
XSQL_API char *xsql_normalize_clipboard_host(const char *host);
// Normalize then bracket a bare IPv6 literal ("::1" => "[::1]"). Heap.
XSQL_API char *xsql_format_url_host(const char *host);
// "Use <tool> ...\ncurl -X POST http://<host>:<port>/query -d \"<sql>\"".
// sample_sql NULL => "SELECT 1". Heap.
XSQL_API char *xsql_build_http_clipboard_payload(const char *tool,
                                                 const char *host, int port,
                                                 const char *sample_sql);
// The mcpServers JSON block pointing at http://<host>:<port>/sse. Heap.
XSQL_API char *xsql_build_mcp_clipboard_payload(const char *server,
                                                const char *host, int port);
// If `output` begins with `prefix` followed by digits, parse the port (>0) into
// *out_port and return true; else false.
XSQL_API bool xsql_parse_started_port(const char *output, const char *prefix,
                                      int *out_port);
// Extract the host from the URL on the first line containing `prefix`. On success
// stores a heap host in *out_host (free with xsql_free) and returns true.
XSQL_API bool xsql_extract_url_host_from_output_line(const char *output,
                                                     const char *prefix,
                                                     char **out_host);
// Parse an "HTTP server started on port <N>" line: the port into *out_port and
// the host from the "URL: " line into *out_host (heap; falls back to "127.0.0.1"
// when absent). Returns false only if the port line is missing.
XSQL_API bool xsql_extract_http_start_endpoint(const char *output,
                                               char **out_host, int *out_port);
// As above for an "MCP server started on port <N>" line + an "SSE endpoint: " URL.
XSQL_API bool xsql_extract_mcp_start_endpoint(const char *output,
                                              char **out_host, int *out_port);
// Copy UTF-8 `text` to the Windows clipboard. Returns false on non-Windows, on
// empty text, or on any Win32 failure. A test override (below) intercepts it.
XSQL_API bool xsql_copy_text_to_clipboard(const char *text);
// Install a test override for xsql_copy_text_to_clipboard (NULL clears it).
XSQL_API void xsql_set_clipboard_copy_override(bool (*fn)(const char *text,
                                                          void *user),
                                               void *user);

#else  // !XSQL_HAS_THINCLIENT

static inline char *xsql_normalize_clipboard_host(const char *host) {
    (void)host;
    return NULL;
}
static inline char *xsql_format_url_host(const char *host) {
    (void)host;
    return NULL;
}
static inline char *xsql_build_http_clipboard_payload(const char *tool,
                                                      const char *host, int port,
                                                      const char *sample_sql) {
    (void)tool; (void)host; (void)port; (void)sample_sql;
    return NULL;
}
static inline char *xsql_build_mcp_clipboard_payload(const char *server,
                                                     const char *host, int port) {
    (void)server; (void)host; (void)port;
    return NULL;
}
static inline bool xsql_parse_started_port(const char *output,
                                           const char *prefix, int *out_port) {
    (void)output; (void)prefix; (void)out_port;
    return false;
}
static inline bool xsql_extract_url_host_from_output_line(const char *output,
                                                          const char *prefix,
                                                          char **out_host) {
    (void)output; (void)prefix;
    if (out_host) {
        *out_host = NULL;
    }
    return false;
}
static inline bool xsql_extract_http_start_endpoint(const char *output,
                                                    char **out_host,
                                                    int *out_port) {
    (void)output;
    if (out_host) {
        *out_host = NULL;
    }
    (void)out_port;
    return false;
}
static inline bool xsql_extract_mcp_start_endpoint(const char *output,
                                                   char **out_host,
                                                   int *out_port) {
    (void)output;
    if (out_host) {
        *out_host = NULL;
    }
    (void)out_port;
    return false;
}
static inline bool xsql_copy_text_to_clipboard(const char *text) {
    (void)text;
    return false;
}
static inline void xsql_set_clipboard_copy_override(bool (*fn)(const char *text,
                                                              void *user),
                                                    void *user) {
    (void)fn; (void)user;
}

#endif  // XSQL_HAS_THINCLIENT

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_CLIPBOARD_H
