// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// banner.c — human-readable HTTP server info/status banners. Reuses the public
// xsql_format_url_host for IPv6-safe host rendering.

#include "libxsqlc/http_server.h"

#include "libxsqlc/clipboard.h"
#include "internal/internal.h"
#include "internal/strbuf.h"

static char *sb_finish(xsqlc_strbuf *sb) {
    if (xsqlc_sb_oom(sb)) {
        xsqlc_sb_free(sb);
        return NULL;
    }
    return xsqlc_sb_detach(sb);
}

char *xsql_format_http_info(int port, const char *bind_address,
                            const char *stop_hint) {
    const char *hint =
        stop_hint ? stop_hint : "Press Ctrl+C to stop and return to REPL.";
    char *host = xsql_format_url_host(bind_address);  // NULL/"" => 127.0.0.1
    if (!host) {
        return NULL;
    }
    // The example is dialed, so a wildcard bind shows its loopback there.
    char *connect_host = xsql_format_connect_url_host(bind_address);
    if (!connect_host) {
        xsqlc_free(host);
        return NULL;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "HTTP server started on port ");
    xsqlc_sb_append_i64(&sb, port);
    xsqlc_sb_append(&sb, "\nURL: http://");
    xsqlc_sb_append(&sb, host);
    xsqlc_sb_putc(&sb, ':');
    xsqlc_sb_append_i64(&sb, port);
    xsqlc_sb_append(&sb,
                    "\n\nEndpoints:\n"
                    "  GET  /help     - API documentation\n"
                    "  POST /query    - Execute SQL query\n"
                    "  GET  /status   - Health check\n"
                    "  POST /cancel   - Cancel active queries\n"
                    "  POST /shutdown - Stop server\n\n"
                    "Example:\n"
                    "  curl -X POST http://");
    xsqlc_sb_append(&sb, connect_host);
    xsqlc_sb_putc(&sb, ':');
    xsqlc_sb_append_i64(&sb, port);
    xsqlc_sb_append(&sb,
                    "/query --data-binary \"SELECT name FROM sqlite_master WHERE "
                    "type='table' LIMIT 10\"\n\n");
    xsqlc_sb_append(&sb, hint);
    xsqlc_sb_putc(&sb, '\n');
    xsqlc_free(connect_host);
    xsqlc_free(host);
    return sb_finish(&sb);
}

char *xsql_format_http_status(int port, bool running, const char *bind_address) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    if (running) {
        char *host = xsql_format_url_host(bind_address);
        if (!host) {
            xsqlc_sb_free(&sb);
            return NULL;
        }
        xsqlc_sb_append(&sb, "HTTP server running on port ");
        xsqlc_sb_append_i64(&sb, port);
        xsqlc_sb_append(&sb, "\nURL: http://");
        xsqlc_sb_append(&sb, host);
        xsqlc_sb_putc(&sb, ':');
        xsqlc_sb_append_i64(&sb, port);
        xsqlc_sb_putc(&sb, '\n');
        xsqlc_free(host);
    } else {
        xsqlc_sb_append(&sb,
                        "HTTP server not running\nUse '.http start' to start\n");
    }
    return sb_finish(&sb);
}
