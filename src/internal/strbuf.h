// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// strbuf.h — a geometric-growth byte buffer: the std::string / std::ostringstream
// replacement used by the formatters, the JSON emitter, and the SQL exporter.
//
// PRIVATE: lives under src/internal, never installed, never included from a
// public header. Backed by the xsqlc allocator so a detached buffer is a heap C
// string a consumer can release with xsql_free().
//
// Every append is length-explicit and embedded-NUL safe; the buffer always keeps
// a trailing NUL so `data` reads as a valid C string. Allocation failure latches
// a sticky OOM flag: subsequent appends become no-ops so callers can build up an
// entire document and check xsqlc_sb_oom() once at the end instead of after each
// call.

#ifndef XSQLC_INTERNAL_STRBUF_H
#define XSQLC_INTERNAL_STRBUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct xsqlc_strbuf {
    char  *data;  // NUL-terminated; NULL until the first append allocates
    size_t len;   // bytes used, excluding the trailing NUL
    size_t cap;   // bytes allocated (>= len + 1 once data != NULL)
    bool   oom;   // sticky: an allocation failed; further appends are no-ops
} xsqlc_strbuf;

// Zero-initialize (no allocation). A zeroed struct is also valid.
void xsqlc_sb_init(xsqlc_strbuf *sb);

// Pre-grow so at least `extra` more bytes (plus the NUL) fit without realloc.
// A hint only; safe to skip. No-op once OOM has latched.
void xsqlc_sb_reserve(xsqlc_strbuf *sb, size_t extra);

// Appends. All no-ops once OOM has latched.
void xsqlc_sb_putc(xsqlc_strbuf *sb, char c);
void xsqlc_sb_append(xsqlc_strbuf *sb, const char *s);  // NUL-terminated (NULL ok)
void xsqlc_sb_append_len(xsqlc_strbuf *sb, const char *s, size_t n);
void xsqlc_sb_append_size(xsqlc_strbuf *sb, size_t v);    // decimal
void xsqlc_sb_append_i64(xsqlc_strbuf *sb, long long v);  // decimal

// printf-style append for small, bounded formats (numbers, short fixed text).
// The formatted result must fit a modest internal bound; oversized output latches
// OOM rather than truncating silently.
void xsqlc_sb_vprintf(xsqlc_strbuf *sb, const char *fmt, va_list ap);
void xsqlc_sb_printf(xsqlc_strbuf *sb, const char *fmt, ...);

// True if any append hit an allocation failure. A detached/read result is only
// trustworthy when this is false.
bool xsqlc_sb_oom(const xsqlc_strbuf *sb);

// Borrow the current contents (NUL-terminated; "" when empty). Valid until the
// next mutation or free. Returns NULL after OOM.
const char *xsqlc_sb_cstr(const xsqlc_strbuf *sb);

// Detach the buffer as a heap C string (xsqlc allocator; free with xsql_free).
// Resets `sb` to empty. Returns NULL on prior OOM (and frees any partial buffer).
char *xsqlc_sb_detach(xsqlc_strbuf *sb);

// Release the buffer. Passing a zeroed/NULL-data buffer is a no-op.
void xsqlc_sb_free(xsqlc_strbuf *sb);

#endif  // XSQLC_INTERNAL_STRBUF_H
