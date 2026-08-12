// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// strbuf.c — see strbuf.h. Geometric growth (doubling from an 8-byte floor),
// sticky OOM, xsqlc-allocator backed.

#include "internal/strbuf.h"

#include "internal/internal.h"

#include <stdio.h>
#include <string.h>

void xsqlc_sb_init(xsqlc_strbuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->oom = false;
}

// Ensure room for `extra` more payload bytes plus the trailing NUL. Latches OOM
// on failure (leaving any existing buffer intact).
static bool sb_ensure(xsqlc_strbuf *sb, size_t extra) {
    if (sb->oom) {
        return false;
    }
    size_t need = sb->len + extra + 1;  // +1 for the NUL
    if (need <= sb->cap) {
        return true;
    }
    size_t new_cap = sb->cap ? sb->cap : 8;
    while (new_cap < need) {
        // Guard against overflow before doubling.
        if (new_cap > (size_t)-1 / 2) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }
    char *p = (char *)xsqlc_realloc(sb->data, new_cap);
    if (!p) {
        sb->oom = true;
        return false;
    }
    sb->data = p;
    sb->cap = new_cap;
    return true;
}

void xsqlc_sb_reserve(xsqlc_strbuf *sb, size_t extra) {
    (void)sb_ensure(sb, extra);
}

void xsqlc_sb_putc(xsqlc_strbuf *sb, char c) {
    if (!sb_ensure(sb, 1)) {
        return;
    }
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void xsqlc_sb_append_len(xsqlc_strbuf *sb, const char *s, size_t n) {
    if (n == 0 || !s) {
        return;
    }
    if (!sb_ensure(sb, n)) {
        return;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void xsqlc_sb_append(xsqlc_strbuf *sb, const char *s) {
    if (!s) {
        return;
    }
    xsqlc_sb_append_len(sb, s, strlen(s));
}

void xsqlc_sb_append_size(xsqlc_strbuf *sb, size_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%zu", v);
    if (n > 0) {
        xsqlc_sb_append_len(sb, buf, (size_t)n);
    }
}

void xsqlc_sb_append_i64(xsqlc_strbuf *sb, long long v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", v);
    if (n > 0) {
        xsqlc_sb_append_len(sb, buf, (size_t)n);
    }
}

void xsqlc_sb_vprintf(xsqlc_strbuf *sb, const char *fmt, va_list ap) {
    if (sb->oom) {
        return;
    }
    char buf[128];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n < 0) {
        sb->oom = true;
        return;
    }
    if ((size_t)n < sizeof(buf)) {
        xsqlc_sb_append_len(sb, buf, (size_t)n);
        return;
    }
    // Rare overflow of the stack buffer: grow and format once more.
    if (!sb_ensure(sb, (size_t)n)) {
        return;
    }
    int m = vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    if (m < 0) {
        sb->oom = true;
        return;
    }
    sb->len += (size_t)m;
    sb->data[sb->len] = '\0';
}

void xsqlc_sb_printf(xsqlc_strbuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xsqlc_sb_vprintf(sb, fmt, ap);
    va_end(ap);
}

bool xsqlc_sb_oom(const xsqlc_strbuf *sb) {
    return sb->oom;
}

const char *xsqlc_sb_cstr(const xsqlc_strbuf *sb) {
    if (sb->oom) {
        return NULL;
    }
    return sb->data ? sb->data : "";
}

char *xsqlc_sb_detach(xsqlc_strbuf *sb) {
    if (sb->oom) {
        xsqlc_free(sb->data);
        xsqlc_sb_init(sb);
        return NULL;
    }
    char *out = sb->data;
    if (!out) {
        // Never-appended buffer: hand back an empty heap string so the caller can
        // xsql_free() it unconditionally.
        out = xsqlc_strdup("");
    }
    xsqlc_sb_init(sb);
    return out;
}

void xsqlc_sb_free(xsqlc_strbuf *sb) {
    xsqlc_free(sb->data);
    xsqlc_sb_init(sb);
}
