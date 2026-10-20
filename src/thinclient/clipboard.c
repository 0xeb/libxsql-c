// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// clipboard.c — thin-client clipboard/URL/payload helpers.
// Pure string helpers plus a Windows clipboard copy and a macOS pbcopy backend.
// <windows.h> is pulled in only here (a .c file), never a shared header.

// macOS: popen/pclose are POSIX and are hidden by <stdio.h> under -std=c11's
// __STRICT_ANSI__; opt them back in before any include. (This branch compiles
// only on Apple.)
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE 1
#endif

#include "libxsqlc/clipboard.h"

#include "internal/internal.h"
#include "internal/strbuf.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

static char *sb_finish(xsqlc_strbuf *sb) {
    if (xsqlc_sb_oom(sb)) {
        xsqlc_sb_free(sb);
        return NULL;
    }
    return xsqlc_sb_detach(sb);
}

char *xsql_normalize_clipboard_host(const char *host) {
    if (!host || host[0] == '\0') {
        return xsqlc_strdup("127.0.0.1");
    }
    return xsqlc_strdup(host);
}

char *xsql_format_url_host(const char *host) {
    char *norm = xsql_normalize_clipboard_host(host);
    if (!norm) {
        return NULL;
    }
    // Bracket a bare IPv6 literal (contains ':' and not already bracketed).
    if (norm[0] != '[' && strchr(norm, ':') != NULL) {
        xsqlc_strbuf sb;
        xsqlc_sb_init(&sb);
        xsqlc_sb_putc(&sb, '[');
        xsqlc_sb_append(&sb, norm);
        xsqlc_sb_putc(&sb, ']');
        xsqlc_free(norm);
        return sb_finish(&sb);
    }
    return norm;
}

char *xsql_format_connect_url_host(const char *bind_host) {
    if (bind_host && strcmp(bind_host, "0.0.0.0") == 0) {
        return xsqlc_strdup("127.0.0.1");
    }
    if (bind_host &&
        (strcmp(bind_host, "::") == 0 || strcmp(bind_host, "[::]") == 0)) {
        return xsqlc_strdup("[::1]");
    }
    return xsql_format_url_host(bind_host);
}

char *xsql_build_http_clipboard_payload(const char *tool, const char *host,
                                        int port, const char *sample_sql,
                                        const char *auth_token) {
    if (!sample_sql || sample_sql[0] == '\0') {
        return NULL;
    }
    const char *tool_name = tool ? tool : "";
    const bool has_token = auth_token && auth_token[0] != '\0';
    char *hosturl = xsql_format_connect_url_host(host);
    if (!hosturl) {
        return NULL;
    }
    xsqlc_strbuf base;
    xsqlc_sb_init(&base);
    xsqlc_sb_append(&base, "http://");
    xsqlc_sb_append(&base, hosturl);
    xsqlc_sb_putc(&base, ':');
    xsqlc_sb_append_i64(&base, port);
    xsqlc_free(hosturl);
    char *base_url = sb_finish(&base);
    if (!base_url) {
        return NULL;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, tool_name);
    xsqlc_sb_append(&sb, " HTTP server at ");
    xsqlc_sb_append(&sb, base_url);
    xsqlc_sb_append(&sb,
                    "\nRead GET /help first (endpoints, schema discovery), then "
                    "send SQL as the body of POST /query; responses are JSON.\n");
    if (has_token) {
        xsqlc_sb_append(&sb,
                        "Send the Authorization header shown with every "
                        "request; GET / and GET /help work without it.\n");
    }
    xsqlc_sb_append(&sb, "  curl ");
    xsqlc_sb_append(&sb, base_url);
    xsqlc_sb_append(&sb, "/help\n  curl -X POST ");
    xsqlc_sb_append(&sb, base_url);
    xsqlc_sb_append(&sb, "/query");
    if (has_token) {
        xsqlc_sb_append(&sb, " -H \"Authorization: Bearer ");
        xsqlc_sb_append(&sb, auth_token);
        xsqlc_sb_putc(&sb, '"');
    }
    xsqlc_sb_append(&sb, " --data-binary \"");
    xsqlc_sb_append(&sb, sample_sql);
    xsqlc_sb_append(&sb, "\"\n");
    xsqlc_free(base_url);
    return sb_finish(&sb);
}

char *xsql_build_mcp_clipboard_payload(const char *server, const char *host,
                                       int port) {
    const char *server_name = server ? server : "";
    char *hosturl = xsql_format_connect_url_host(host);
    if (!hosturl) {
        return NULL;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "{\n  \"mcpServers\": {\n    \"");
    xsqlc_sb_append(&sb, server_name);
    xsqlc_sb_append(&sb, "\": {\n      \"url\": \"http://");
    xsqlc_sb_append(&sb, hosturl);
    xsqlc_sb_putc(&sb, ':');
    xsqlc_sb_append_i64(&sb, port);
    xsqlc_sb_append(&sb, "/sse\"\n    }\n  }\n}\n");
    xsqlc_free(hosturl);
    return sb_finish(&sb);
}

bool xsql_parse_started_port(const char *output, const char *prefix,
                             int *out_port) {
    if (!output || !prefix || !out_port) {
        return false;
    }
    size_t plen = strlen(prefix);
    if (strncmp(output, prefix, plen) != 0) {
        return false;
    }
    const char *p = output + plen;
    const char *start = p;
    int port = 0;
    while (*p >= '0' && *p <= '9') {
        port = port * 10 + (*p - '0');
        ++p;
    }
    if (p == start || port <= 0) {
        return false;
    }
    *out_port = port;
    return true;
}

bool xsql_extract_url_host_from_output_line(const char *output,
                                            const char *prefix, char **out_host) {
    if (out_host) {
        *out_host = NULL;
    }
    if (!output || !prefix || !out_host) {
        return false;
    }
    const char *pos = strstr(output, prefix);
    if (!pos) {
        return false;
    }
    const char *url = pos + strlen(prefix);
    // URL runs to the end of line.
    size_t url_len = 0;
    while (url[url_len] && url[url_len] != '\r' && url[url_len] != '\n') {
        ++url_len;
    }
    if (url_len == 0) {
        return false;
    }

    // Skip an optional scheme "://".
    size_t host_start = 0;
    for (size_t i = 0; i + 2 < url_len; ++i) {
        if (url[i] == ':' && url[i + 1] == '/' && url[i + 2] == '/') {
            host_start = i + 3;
            break;
        }
    }
    if (host_start >= url_len) {
        return false;
    }

    size_t host_end;
    if (url[host_start] == '[') {
        // Bracketed IPv6 literal: include the closing ']'.
        size_t j = host_start + 1;
        while (j < url_len && url[j] != ']') {
            ++j;
        }
        if (j >= url_len) {
            return false;
        }
        host_end = j + 1;  // inclusive of ']'
    } else {
        // Host up to the last ':' (the port separator) after host_start.
        size_t sep = url_len;
        for (size_t j = url_len; j > host_start; --j) {
            if (url[j - 1] == ':') {
                sep = j - 1;
                break;
            }
        }
        if (sep == url_len || sep <= host_start) {
            return false;
        }
        host_end = sep;
    }

    size_t n = host_end - host_start;
    char *host = (char *)xsqlc_malloc(n + 1);
    if (!host) {
        return false;
    }
    memcpy(host, url + host_start, n);
    host[n] = '\0';
    *out_host = host;
    return true;
}

// Shared body for the two composite extractors: parse the "<...>on port N" port
// line and the URL host line, falling back to 127.0.0.1 for the host.
static bool extract_start_endpoint(const char *output, const char *port_prefix,
                                   const char *url_prefix, char **out_host,
                                   int *out_port) {
    if (out_host) {
        *out_host = NULL;
    }
    if (!xsql_parse_started_port(output, port_prefix, out_port)) {
        return false;
    }
    if (!out_host) {
        return true;
    }
    if (!xsql_extract_url_host_from_output_line(output, url_prefix, out_host)) {
        *out_host = xsqlc_strdup("127.0.0.1");
        if (!*out_host) {
            return false;  // OOM: never report success with a NULL host
        }
    }
    return true;
}

bool xsql_extract_http_start_endpoint(const char *output, char **out_host,
                                      int *out_port) {
    return extract_start_endpoint(output, "HTTP server started on port ",
                                  "URL: ", out_host, out_port);
}

bool xsql_extract_mcp_start_endpoint(const char *output, char **out_host,
                                     int *out_port) {
    return extract_start_endpoint(output, "MCP server started on port ",
                                  "SSE endpoint: ", out_host, out_port);
}

// ---------------------------------------------------------------------------
// Clipboard copy (Windows / macOS backends; no-op elsewhere) + test override
// ---------------------------------------------------------------------------

static bool (*g_clipboard_override)(const char *, void *);
static void *g_clipboard_override_user;

void xsql_set_clipboard_copy_override(bool (*fn)(const char *text, void *user),
                                      void *user) {
    g_clipboard_override = fn;
    g_clipboard_override_user = user;
}

#if defined(_WIN32)
static bool copy_text_windows(const char *text) {
    if (!text || text[0] == '\0') {
        return false;
    }
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wide_len <= 0) {
        return false;
    }
    if (!OpenClipboard(NULL)) {
        return false;
    }
    bool ok = false;
    if (EmptyClipboard()) {
        size_t bytes = (size_t)wide_len * sizeof(wchar_t);
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle) {
            void *mem = GlobalLock(handle);
            if (mem) {
                MultiByteToWideChar(CP_UTF8, 0, text, -1, (wchar_t *)mem, wide_len);
                GlobalUnlock(handle);
                if (SetClipboardData(CF_UNICODETEXT, handle)) {
                    ok = true;
                } else {
                    GlobalFree(handle);
                }
            } else {
                GlobalFree(handle);
            }
        }
    }
    CloseClipboard();
    return ok;
}
#elif defined(__APPLE__)
// Pipe the UTF-8 text to the `pbcopy` utility. No Cocoa/Obj-C dependency.
static bool copy_text_macos(const char *text) {
    if (!text || text[0] == '\0') {
        return false;
    }
    FILE *pipe = popen("pbcopy", "w");
    if (!pipe) {
        return false;
    }
    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, pipe);
    int rc = pclose(pipe);
    return written == len && rc == 0;
}
#endif

bool xsql_copy_text_to_clipboard(const char *text) {
    if (g_clipboard_override) {
        return g_clipboard_override(text, g_clipboard_override_user);
    }
#if defined(_WIN32)
    return copy_text_windows(text);
#elif defined(__APPLE__)
    return copy_text_macos(text);
#else
    (void)text;
    return false;
#endif
}
