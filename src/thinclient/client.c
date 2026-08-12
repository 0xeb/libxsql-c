// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// client.c — the thin-client HTTP client over civetweb's
// client API. Response bodies are heap strings owned by the library.

#include "libxsqlc/http_client.h"

#include "internal/internal.h"
#include "internal/strbuf.h"

#include <string.h>

#include "civetweb.h"

struct xsql_http_client {
    char *host;         // heap
    int   port;
    int   timeout_sec;
    char *auth_token;   // heap or NULL
};

static int client_config_has(const xsql_http_client_config *cfg,
                             size_t offset, size_t field_size) {
    return cfg && cfg->struct_size >= offset &&
           cfg->struct_size - offset >= field_size;
}

#define CLIENT_CONFIG_HAS(cfg, field)                                      \
    client_config_has((cfg), offsetof(xsql_http_client_config, field),     \
                      sizeof(((xsql_http_client_config *)0)->field))

int xsql_http_client_new(const xsql_http_client_config *cfg,
                         xsql_http_client **out) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        return XSQL_MISUSE;
    }
    xsql_http_client *c = (xsql_http_client *)xsqlc_malloc(sizeof(*c));
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    memset(c, 0, sizeof(*c));

    const char *host = "127.0.0.1";
    c->port = 5555;
    c->timeout_sec = 30;
    if (cfg && cfg->struct_size != 0) {
        if (CLIENT_CONFIG_HAS(cfg, host) && cfg->host && cfg->host[0]) {
            host = cfg->host;
        }
        if (CLIENT_CONFIG_HAS(cfg, port) && cfg->port != 0) {
            c->port = cfg->port;
        }
        if (CLIENT_CONFIG_HAS(cfg, timeout_sec) && cfg->timeout_sec != 0) {
            c->timeout_sec = cfg->timeout_sec;
        }
    }
    c->host = xsqlc_strdup(host);
    if (!c->host) {
        xsqlc_free(c);
        return XSQL_NO_MEMORY;
    }
    if (CLIENT_CONFIG_HAS(cfg, auth_token) &&
        cfg->auth_token && cfg->auth_token[0]) {
        c->auth_token = xsqlc_strdup(cfg->auth_token);
        if (!c->auth_token) {
            xsqlc_free(c->host);
            xsqlc_free(c);
            return XSQL_NO_MEMORY;
        }
    }
    *out = c;
    return XSQL_OK;
}

#undef CLIENT_CONFIG_HAS

void xsql_http_client_free(xsql_http_client *c) {
    if (!c) {
        return;
    }
    xsqlc_free(c->host);
    xsqlc_free(c->auth_token);
    xsqlc_free(c);
}

// Perform one request. On success returns XSQL_OK (HTTP 200) or XSQL_ERROR (other
// HTTP status), with the body in *body_out (heap; "" if empty) and the HTTP code
// in *http_status (0 when the connection failed). On a transport failure returns
// XSQL_IO_ERROR with a diagnostic in *body_out.
static int do_request(xsql_http_client *c, const char *method, const char *path,
                      const char *body, char **body_out, int *http_status) {
    if (body_out) {
        *body_out = NULL;
    }
    if (http_status) {
        *http_status = 0;
    }

    char ebuf[256] = {0};
    struct mg_connection *conn =
        mg_connect_client(c->host, c->port, 0 /*use_ssl*/, ebuf, sizeof(ebuf));
    if (!conn) {
        if (body_out) {
            *body_out = xsqlc_strdup(ebuf[0] ? ebuf : "connection failed");
        }
        return XSQL_IO_ERROR;
    }

    size_t blen = body ? strlen(body) : 0;
    mg_printf(conn,
              "%s %s HTTP/1.1\r\n"
              "Host: %s:%d\r\n"
              "Connection: close\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: %zu\r\n",
              method, path, c->host, c->port, blen);
    if (c->auth_token) {
        mg_printf(conn, "X-XSQL-Token: %s\r\n", c->auth_token);
    }
    mg_printf(conn, "\r\n");
    if (blen) {
        mg_write(conn, body, blen);
    }

    int timeout_ms = c->timeout_sec > 0 ? c->timeout_sec * 1000 : -1;
    int r = mg_get_response(conn, ebuf, sizeof(ebuf), timeout_ms);
    if (r < 0) {
        if (body_out) {
            *body_out = xsqlc_strdup(ebuf[0] ? ebuf : "no response");
        }
        mg_close_connection(conn);
        return XSQL_IO_ERROR;
    }

    const struct mg_response_info *ri = mg_get_response_info(conn);
    int status = ri ? ri->status_code : 0;
    if (http_status) {
        *http_status = status;
    }

    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    char buf[4096];
    int n;
    while ((n = mg_read(conn, buf, sizeof(buf))) > 0) {
        xsqlc_sb_append_len(&sb, buf, (size_t)n);
    }
    mg_close_connection(conn);

    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        return XSQL_NO_MEMORY;
    }
    if (body_out) {
        *body_out = xsqlc_sb_detach(&sb);
        if (!*body_out) {
            return XSQL_NO_MEMORY;
        }
    } else {
        xsqlc_sb_free(&sb);
    }
    return status == 200 ? XSQL_OK : XSQL_ERROR;
}

int xsql_http_client_query(xsql_http_client *c, const char *sql, char **out) {
    if (out) {
        *out = NULL;
    }
    if (!c || !out) {
        return XSQL_MISUSE;
    }
    return do_request(c, "POST", "/query", sql ? sql : "", out, NULL);
}

int xsql_http_client_status(xsql_http_client *c, char **out) {
    if (out) {
        *out = NULL;
    }
    if (!c || !out) {
        return XSQL_MISUSE;
    }
    return do_request(c, "GET", "/status", NULL, out, NULL);
}

int xsql_http_client_cancel(xsql_http_client *c) {
    if (!c) {
        return XSQL_MISUSE;
    }
    char *body = NULL;
    int rc = do_request(c, "POST", "/cancel", "", &body, NULL);
    xsql_free(body);
    return rc;
}

int xsql_http_client_shutdown(xsql_http_client *c) {
    if (!c) {
        return XSQL_MISUSE;
    }
    char *body = NULL;
    (void)do_request(c, "POST", "/shutdown", "", &body, NULL);
    xsql_free(body);
    // Best-effort: the server may close the connection before responding, so any
    // transport error here is expected and swallowed.
    return XSQL_OK;
}

bool xsql_http_client_ping(xsql_http_client *c) {
    if (!c) {
        return false;
    }
    char *body = NULL;
    int http_status = 0;
    int rc = do_request(c, "GET", "/status", NULL, &body, &http_status);
    xsql_free(body);
    return rc != XSQL_IO_ERROR && http_status == 200;
}
