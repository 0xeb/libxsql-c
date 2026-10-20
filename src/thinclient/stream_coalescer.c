// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// stream_coalescer.c — batches a streaming executor's per-row writes into
// threshold-sized physical writes (the C port of the C++
// xsql::thinclient::StreamWriteCoalescer). The streaming JSON/NDJSON emitters
// call their sink once per row; forwarding each call straight to the transport
// is one chunk frame and one socket write per row. The bytes on the wire are
// identical either way; only the write count changes.

#include "libxsqlc/http_server.h"

#include "internal/internal.h"
#include "internal/strbuf.h"

#include <string.h>

struct xsql_stream_coalescer {
    xsql_stream_sink_fn real_write;
    void               *real_user;
    size_t              threshold;
    xsqlc_strbuf        buf;
    bool                failed;       // sticky once a real write (or OOM) failed
    size_t              real_writes;  // real_write invocations (for tests)
};

int xsql_stream_coalescer_new(xsql_stream_sink_fn real_write, void *real_user,
                              size_t flush_threshold_bytes,
                              xsql_stream_coalescer **out) {
    if (out) {
        *out = NULL;
    }
    if (!out || !real_write || flush_threshold_bytes == 0) {
        return XSQL_MISUSE;
    }
    xsql_stream_coalescer *c =
        (xsql_stream_coalescer *)xsqlc_malloc(sizeof(*c));
    if (!c) {
        return XSQL_NO_MEMORY;
    }
    memset(c, 0, sizeof(*c));
    c->real_write = real_write;
    c->real_user = real_user;
    c->threshold = flush_threshold_bytes;
    xsqlc_sb_init(&c->buf);
    *out = c;
    return XSQL_OK;
}

void xsql_stream_coalescer_free(xsql_stream_coalescer *c) {
    if (!c) {
        return;
    }
    xsqlc_sb_free(&c->buf);
    xsqlc_free(c);
}

bool xsql_stream_coalescer_flush(xsql_stream_coalescer *c) {
    if (!c) {
        return false;
    }
    if (c->failed || c->buf.len == 0) {
        return !c->failed;
    }
    ++c->real_writes;
    if (!c->real_write(c->buf.data, c->buf.len, c->real_user)) {
        c->failed = true;
    }
    c->buf.len = 0;
    c->buf.data[0] = '\0';
    return !c->failed;
}

bool xsql_stream_coalescer_write(xsql_stream_coalescer *c, const char *data,
                                 size_t len) {
    if (!c || c->failed) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (!data) {
        return false;
    }
    xsqlc_sb_append_len(&c->buf, data, len);
    if (xsqlc_sb_oom(&c->buf)) {
        // A dropped row would corrupt the stream; treat it like a failed write.
        c->failed = true;
        return false;
    }
    if (c->buf.len >= c->threshold) {
        xsql_stream_coalescer_flush(c);
    }
    return !c->failed;
}

bool xsql_stream_coalescer_sink(const char *data, size_t len, void *coalescer) {
    return xsql_stream_coalescer_write((xsql_stream_coalescer *)coalescer, data,
                                       len);
}

bool xsql_stream_coalescer_failed(const xsql_stream_coalescer *c) {
    return !c || c->failed;
}

size_t xsql_stream_coalescer_real_write_count(const xsql_stream_coalescer *c) {
    return c ? c->real_writes : 0;
}
