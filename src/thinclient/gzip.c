// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// gzip.c — RFC 1952 framing over miniz's raw-deflate compressor (tdefl). miniz's
// zlib-style mz_deflate cannot emit the gzip wrapper (windowBits 31), so the
// 10-byte header and the CRC-32 + ISIZE trailer are written here and the body
// is a raw deflate stream at level 1 (zlib Z_BEST_SPEED, the C++ server's level).

#include "thinclient/gzip.h"

#include "libxsqlc/status.h"

#include "internal/internal.h"
#include "internal/strbuf.h"

#include "miniz.h"

#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Negotiation
// ---------------------------------------------------------------------------

bool xsqlc_http_accepts_gzip(const char *accept_encoding) {
    return accept_encoding && strstr(accept_encoding, "gzip") != NULL;
}

bool xsqlc_http_can_compress(const char *content_type) {
    if (!content_type) {
        return false;
    }
    size_t n = strcspn(content_type, ";");
    while (n > 0 &&
           (content_type[n - 1] == ' ' || content_type[n - 1] == '\t')) {
        --n;
    }
    static const char *const kAllow[] = {
        "image/svg+xml",        "application/javascript", "application/json",
        "application/xml",      "application/protobuf",   "application/x-ndjson",
        "application/xhtml+xml",
    };
    for (size_t i = 0; i < sizeof(kAllow) / sizeof(kAllow[0]); ++i) {
        if (strlen(kAllow[i]) == n && memcmp(content_type, kAllow[i], n) == 0) {
            return true;
        }
    }
    static const char kSse[] = "text/event-stream";
    if (n == sizeof(kSse) - 1 && memcmp(content_type, kSse, n) == 0) {
        return false;
    }
    return n >= 5 && memcmp(content_type, "text/", 5) == 0;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct xsqlc_gzip {
    tdefl_compressor  *deflate;  // ~300 KB; heap
    xsqlc_gzip_out_fn  out;
    void              *user;
    mz_ulong           crc;
    uint32_t           isize;    // input length mod 2^32 (RFC 1952 ISIZE)
    bool               started;  // header written
    bool               failed;
    bool               finished;
};

static mz_bool put_buf(const void *buf, int len, void *user) {
    xsqlc_gzip *g = (xsqlc_gzip *)user;
    if (len <= 0) {
        return MZ_TRUE;
    }
    if (!g->out((const char *)buf, (size_t)len, g->user)) {
        g->failed = true;
        return MZ_FALSE;
    }
    return MZ_TRUE;
}

static bool write_header(xsqlc_gzip *g) {
    if (g->started) {
        return true;
    }
    g->started = true;
    // ID1 ID2 CM=deflate FLG=0 MTIME=0 XFL=4 (fastest) OS=255 (unknown).
    static const unsigned char kHeader[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 4, 0xff};
    if (!g->out((const char *)kHeader, sizeof(kHeader), g->user)) {
        g->failed = true;
    }
    return !g->failed;
}

int xsqlc_gzip_new(xsqlc_gzip_out_fn out, void *user, xsqlc_gzip **g_out) {
    if (g_out) {
        *g_out = NULL;
    }
    if (!out || !g_out) {
        return XSQL_MISUSE;
    }
    xsqlc_gzip *g = (xsqlc_gzip *)xsqlc_malloc(sizeof(*g));
    if (!g) {
        return XSQL_NO_MEMORY;
    }
    memset(g, 0, sizeof(*g));
    g->deflate = (tdefl_compressor *)xsqlc_malloc(sizeof(tdefl_compressor));
    if (!g->deflate) {
        xsqlc_free(g);
        return XSQL_NO_MEMORY;
    }
    g->out = out;
    g->user = user;
    g->crc = MZ_CRC32_INIT;
    // Level 1, negative window bits => raw deflate (the gzip wrapper is ours).
    mz_uint flags = tdefl_create_comp_flags_from_zip_params(
        1, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    if (tdefl_init(g->deflate, put_buf, g, (int)flags) != TDEFL_STATUS_OKAY) {
        xsqlc_free(g->deflate);
        xsqlc_free(g);
        return XSQL_ERROR;
    }
    *g_out = g;
    return XSQL_OK;
}

void xsqlc_gzip_free(xsqlc_gzip *g) {
    if (!g) {
        return;
    }
    xsqlc_free(g->deflate);
    xsqlc_free(g);
}

bool xsqlc_gzip_write(xsqlc_gzip *g, const char *data, size_t len) {
    if (!g || g->failed || g->finished) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (!data || !write_header(g)) {
        g->failed = true;
        return false;
    }
    g->crc = mz_crc32(g->crc, (const unsigned char *)data, len);
    g->isize += (uint32_t)len;  // wraps mod 2^32 by design (RFC 1952 ISIZE)
    if (tdefl_compress_buffer(g->deflate, data, len, TDEFL_NO_FLUSH) !=
        TDEFL_STATUS_OKAY) {
        g->failed = true;
    }
    return !g->failed;
}

bool xsqlc_gzip_sink(const char *data, size_t len, void *g) {
    return xsqlc_gzip_write((xsqlc_gzip *)g, data, len);
}

bool xsqlc_gzip_finish(xsqlc_gzip *g) {
    if (!g || g->failed) {
        return false;
    }
    if (g->finished) {
        return true;
    }
    g->finished = true;
    if (!write_header(g)) {
        return false;
    }
    if (tdefl_compress_buffer(g->deflate, NULL, 0, TDEFL_FINISH) !=
        TDEFL_STATUS_DONE) {
        g->failed = true;
        return false;
    }
    unsigned char trailer[8];
    uint32_t crc = (uint32_t)g->crc;
    for (int i = 0; i < 4; ++i) {
        trailer[i] = (unsigned char)(crc >> (8 * i));
        trailer[4 + i] = (unsigned char)(g->isize >> (8 * i));
    }
    if (!g->out((const char *)trailer, sizeof(trailer), g->user)) {
        g->failed = true;
    }
    return !g->failed;
}

static bool append_out(const char *data, size_t len, void *user) {
    xsqlc_strbuf *sb = (xsqlc_strbuf *)user;
    xsqlc_sb_append_len(sb, data, len);
    return !xsqlc_sb_oom(sb);
}

char *xsqlc_gzip_buffer(const char *data, size_t len, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_gzip *g = NULL;
    if (xsqlc_gzip_new(append_out, &sb, &g) != XSQL_OK) {
        return NULL;
    }
    bool ok = xsqlc_gzip_write(g, data, len) && xsqlc_gzip_finish(g);
    xsqlc_gzip_free(g);
    if (!ok || xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        return NULL;
    }
    size_t n = sb.len;
    char *buf = xsqlc_sb_detach(&sb);
    if (buf && out_len) {
        *out_len = n;
    }
    return buf;
}
