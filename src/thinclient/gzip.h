// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// gzip.h — gzip (RFC 1952) response compression for the thin-client HTTP
// server, over the vendored miniz raw-deflate compressor. PRIVATE: included only
// by src/thinclient .c files, and compiled only with XSQLC_WITH_GZIP (the
// XSQLC_HAS_GZIP define). No miniz type appears here.
//
// Mirrors the C++ server's vendored httplib: compress only when the client's
// Accept-Encoding mentions gzip and the response's media type is on the
// allowlist, at the fastest level (zlib Z_BEST_SPEED, windowBits 31).

#ifndef XSQLC_THINCLIENT_GZIP_H
#define XSQLC_THINCLIENT_GZIP_H

#include <stdbool.h>
#include <stddef.h>

// True when the client's Accept-Encoding value asks for gzip. Like the C++
// server this is a substring test: a "gzip;q=0" opt-out is not honored.
bool xsqlc_http_accepts_gzip(const char *accept_encoding);

// True when a response of this Content-Type may be compressed. Decided on the
// media type alone (any ";params" are ignored): JSON, NDJSON, XML, JavaScript,
// protobuf, SVG, XHTML, and every text/* except text/event-stream (a compressed
// SSE stream is held back until the compressor fills, so events never arrive).
bool xsqlc_http_can_compress(const char *content_type);

// Streaming gzip encoder. Output (header, deflate blocks, trailer) goes to
// `out(data, len, user)` as it becomes available; a false return latches the
// encoder failed.
typedef bool (*xsqlc_gzip_out_fn)(const char *data, size_t len, void *user);
typedef struct xsqlc_gzip xsqlc_gzip;

// XSQL_OK, XSQL_MISUSE (NULL out/g), or XSQL_NO_MEMORY.
int  xsqlc_gzip_new(xsqlc_gzip_out_fn out, void *user, xsqlc_gzip **g);
// Compress `len` more bytes. Returns false once the encoder has failed.
bool xsqlc_gzip_write(xsqlc_gzip *g, const char *data, size_t len);
// Flush the final deflate block and write the CRC-32 / size trailer. Returns
// false when the encoder failed at any point.
bool xsqlc_gzip_finish(xsqlc_gzip *g);
// Adapter matching xsql_stream_sink_fn (user = the encoder).
bool xsqlc_gzip_sink(const char *data, size_t len, void *g);
void xsqlc_gzip_free(xsqlc_gzip *g);

// One-shot: gzip `len` bytes into a heap buffer (free with xsqlc_free).
// Returns NULL on allocation failure.
char *xsqlc_gzip_buffer(const char *data, size_t len, size_t *out_len);

#endif  // XSQLC_THINCLIENT_GZIP_H
