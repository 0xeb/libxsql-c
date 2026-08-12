// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// json_helpers.c — the string-based thin-client JSON helpers. These build small
// fixed envelopes; the escaping emits no surrounding quotes and uses literal
// control bytes for backspace/form-feed, which is deliberately NOT the canonical
// envelope emitter (xsqlc_json_append_string wraps in quotes and emits \b/\f).

#include "libxsqlc/thinclient_json.h"

#include "internal/internal.h"
#include "internal/strbuf.h"

#include <stdio.h>
#include <string.h>

static char *sb_finish(xsqlc_strbuf *sb) {
    if (xsqlc_sb_oom(sb)) {
        xsqlc_sb_free(sb);
        return NULL;
    }
    return xsqlc_sb_detach(sb);
}

// Append the JSON-escaped bytes of `s` (no surrounding quotes) to `sb`.
static void escape_into(xsqlc_strbuf *sb, const char *s) {
    if (!s) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '"':  xsqlc_sb_append_len(sb, "\\\"", 2); break;
            case '\\': xsqlc_sb_append_len(sb, "\\\\", 2); break;
            case '\n': xsqlc_sb_append_len(sb, "\\n", 2);  break;
            case '\r': xsqlc_sb_append_len(sb, "\\r", 2);  break;
            case '\t': xsqlc_sb_append_len(sb, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
                    xsqlc_sb_append(sb, buf);
                } else {
                    xsqlc_sb_putc(sb, (char)c);
                }
        }
    }
}

char *xsql_json_escape(const char *s) {
    // A NULL/empty input escapes to a heap "" (escaping a non-empty string always
    // yields at least one byte, so the strbuf path never detaches an empty buffer).
    if (!s || !s[0]) {
        return xsqlc_strdup("");
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    escape_into(&sb, s);
    return sb_finish(&sb);
}

char *xsql_make_error_json(const char *error) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "{\"success\":false,\"error\":\"");
    escape_into(&sb, error);
    xsqlc_sb_append(&sb, "\"}");
    return sb_finish(&sb);
}

char *xsql_make_success_json(const char *message) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    if (message && message[0]) {
        xsqlc_sb_append(&sb, "{\"success\":true,\"message\":\"");
        escape_into(&sb, message);
        xsqlc_sb_append(&sb, "\"}");
    } else {
        xsqlc_sb_append(&sb, "{\"success\":true}");
    }
    return sb_finish(&sb);
}

char *xsql_make_status_json(const char *tool, const char *extra_json) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "{\"success\":true,\"status\":\"ok\",\"tool\":\"");
    escape_into(&sb, tool);
    xsqlc_sb_putc(&sb, '"');
    if (extra_json && extra_json[0]) {
        xsqlc_sb_putc(&sb, ',');
        xsqlc_sb_append(&sb, extra_json);
    }
    xsqlc_sb_putc(&sb, '}');
    return sb_finish(&sb);
}
