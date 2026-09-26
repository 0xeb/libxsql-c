// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// json.h — a read-only JSON DOM. Parse a document, walk it with typed getters,
// free it once.
//
// This exists because every tool built on libxsql-c that talks to an external
// engine has to read that engine's JSON output, and until now the parser was
// private to the library. It is CORE, not thin-client-gated: parsing a command's
// output must not require linking an HTTP server.
//
// EXACT INTEGERS. A JSON integer beyond 2^53 cannot survive a double, and that
// is not a corner case for the consumers this serves — 64-bit addresses are
// routine. xsql_json_i64() returns the exact value for any literal that was
// integral in form and fits int64_t; xsql_json_is_int() reports whether that
// held. xsql_json_double() is always available and always means what it did
// before.
//
// OWNERSHIP. xsql_json_parse() returns a tree the caller frees with a single
// xsql_json_free() on the root. Every other pointer this header returns —
// values, keys, strings — is BORROWED and valid only while that root lives.
// Nothing here allocates on the caller's behalf.
//
// THREADING. A parsed tree is immutable, so concurrent readers are safe.
// Parsing and freeing are not synchronized.

#ifndef XSQLC_JSON_H
#define XSQLC_JSON_H

#include <libxsqlc/common.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque DOM node. Both a whole document and any value inside it are one.
typedef struct xsql_json xsql_json;

typedef enum xsql_json_type {
    XSQL_JSON_NULL   = 0,
    XSQL_JSON_BOOL   = 1,
    XSQL_JSON_NUMBER = 2,
    XSQL_JSON_STRING = 3,
    XSQL_JSON_ARRAY  = 4,
    XSQL_JSON_OBJECT = 5
} xsql_json_type;

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

// Parse a NUL-terminated UTF-8 document. Returns NULL on any syntax error;
// leading/trailing whitespace is tolerated, malformed structure is not.
// Free the result with xsql_json_free().
XSQL_API xsql_json *xsql_json_parse(const char *text);

// Free a tree returned by xsql_json_parse(). NULL is a no-op. Never call this
// on a borrowed interior node.
XSQL_API void xsql_json_free(xsql_json *root);

// ---------------------------------------------------------------------------
// Type inspection. Every one of these accepts NULL and answers for it, so a
// lookup miss can be chained without an intervening check.
// ---------------------------------------------------------------------------

XSQL_API xsql_json_type xsql_json_type_of(const xsql_json *v);  // NULL -> XSQL_JSON_NULL
XSQL_API bool xsql_json_is_null(const xsql_json *v);            // NULL or a JSON null
XSQL_API bool xsql_json_is(const xsql_json *v, xsql_json_type t);

// ---------------------------------------------------------------------------
// Scalars. Each takes the default to return when `v` is NULL or the wrong type,
// so callers do not have to branch.
// ---------------------------------------------------------------------------

XSQL_API bool   xsql_json_bool(const xsql_json *v, bool dflt);
XSQL_API double xsql_json_double(const xsql_json *v, double dflt);

// True when this number carried an exact integer (integral literal that fit
// int64_t). False for `2.0`, `1e3`, a value past int64 range, and non-numbers.
XSQL_API bool xsql_json_is_int(const xsql_json *v);

// Exact integer value. When xsql_json_is_int() is false but `v` is a number,
// the double is truncated toward zero; otherwise `dflt` is returned. Prefer
// checking xsql_json_is_int() when exactness matters.
XSQL_API int64_t xsql_json_i64(const xsql_json *v, int64_t dflt);

// Borrowed string bytes. Returns NULL when `v` is not a string. The buffer is
// NUL-terminated AND length-carrying, so embedded NULs survive: pass `len` to
// recover the true byte count (it may be NULL if you do not care).
XSQL_API const char *xsql_json_string(const xsql_json *v, size_t *len);

// ---------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------

// Element count; 0 for anything that is not an array.
XSQL_API size_t xsql_json_array_size(const xsql_json *v);

// Borrowed element, or NULL if `v` is not an array or `i` is out of range.
XSQL_API const xsql_json *xsql_json_array_at(const xsql_json *v, size_t i);

// ---------------------------------------------------------------------------
// Objects. Member order is preserved as it appeared in the document, so
// iteration is reproducible.
// ---------------------------------------------------------------------------

// Member count; 0 for anything that is not an object.
XSQL_API size_t xsql_json_object_size(const xsql_json *v);

// Borrowed key at position `i` (NULL if out of range or not an object). Keys
// are NUL-terminated; `len` is optional and carries the true byte count.
XSQL_API const char *xsql_json_object_key(const xsql_json *v, size_t i, size_t *len);

// Borrowed value at position `i` (NULL if out of range or not an object).
XSQL_API const xsql_json *xsql_json_object_at(const xsql_json *v, size_t i);

// Borrowed value for `key`, or NULL if absent (or `v` is not an object). On a
// duplicate key the FIRST occurrence wins, matching document order.
XSQL_API const xsql_json *xsql_json_member(const xsql_json *v, const char *key);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XSQLC_JSON_H
