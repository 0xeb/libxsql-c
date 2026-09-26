// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// json.h — the hand-rolled JSON surface (no nlohmann). Two halves:
//
//   * Emitter helpers used by the script formatter to produce the canonical
//     envelope. append_number implements the A6 "integer-when-whole"
//     rule so an elapsed_ms of 2.0 serializes as `2`, not `2.0`.
//   * A small tolerant DOM parser used by xsql_script_result_from_json to rebuild
//     a ScriptResult from a serialized envelope (the inverse of the emitter).
//
// PRIVATE: under src/internal, never installed, never in a public header.

#ifndef XSQLC_INTERNAL_JSON_H
#define XSQLC_INTERNAL_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/strbuf.h"

// ---------------------------------------------------------------------------
// Emitter
// ---------------------------------------------------------------------------

// Append `s` (n bytes; embedded NULs allowed) as a quoted, escaped JSON string.
// Escapes " \ \b \f \n \r \t and any control byte < 0x20 as \u00xx.
void xsqlc_json_append_string(xsqlc_strbuf *sb, const char *s, size_t n);

// Append `v` as a JSON number. A whole (finite) value is emitted as a bare
// integer (no ".0"); otherwise the default %g form.
void xsqlc_json_append_number(xsqlc_strbuf *sb, double v);

// ---------------------------------------------------------------------------
// Tolerant DOM parser
// ---------------------------------------------------------------------------

typedef enum xsqlc_jtype {
    XSQLC_JNULL,
    XSQLC_JBOOL,
    XSQLC_JNUM,
    XSQLC_JSTR,
    XSQLC_JARR,
    XSQLC_JOBJ
} xsqlc_jtype;

typedef struct xsqlc_jval xsqlc_jval;

struct xsqlc_jval {
    xsqlc_jtype type;
    bool        bval;      // XSQLC_JBOOL
    double      num;       // XSQLC_JNUM (always set)
    // Exact integer carriage. `num` alone cannot represent a JSON integer
    // beyond 2^53, and consumers do hit that: radare2 emits full 64-bit
    // addresses, so `0x7ff6e2c14a01` style values round through a double and
    // come back wrong. When the literal is integral in FORM (no '.', no
    // exponent) and fits int64_t exactly, `is_int` is true and `i64` holds the
    // exact value. `num` keeps its old meaning either way, so every existing
    // reader is unaffected.
    bool        is_int;    // XSQLC_JNUM: i64 is exact
    int64_t     i64;       // XSQLC_JNUM and is_int
    char       *str;       // XSQLC_JSTR (decoded; xsqlc heap)
    size_t      strlen;    // byte length of str (embedded-NUL safe)

    xsqlc_jval **items;    // XSQLC_JARR elements
    size_t       nitems;

    char       **keys;     // XSQLC_JOBJ member keys (decoded)
    xsqlc_jval **vals;     // XSQLC_JOBJ member values
    size_t       nmembers;
};

// Parse a whole JSON document. Returns NULL on any syntax error (tolerant only of
// leading/trailing whitespace, not of malformed structure). Free with
// xsqlc_json_free.
xsqlc_jval *xsqlc_json_parse(const char *text);

void xsqlc_json_free(xsqlc_jval *v);

// Object member lookup by key (NULL if absent or `v` is not an object).
const xsqlc_jval *xsqlc_json_member(const xsqlc_jval *v, const char *key);

// Typed convenience readers with defaults (used by the envelope rebuilder).
bool        xsqlc_json_is(const xsqlc_jval *v, xsqlc_jtype t);
bool        xsqlc_json_bool(const xsqlc_jval *v, bool dflt);
double      xsqlc_json_num(const xsqlc_jval *v, double dflt);
size_t      xsqlc_json_size(const xsqlc_jval *v, size_t dflt);
const char *xsqlc_json_str(const xsqlc_jval *v);  // "" if not a string

// Serialize a DOM value back to compact JSON, appended to `sb` (the inverse of
// xsqlc_json_parse). String bytes are re-escaped via xsqlc_json_append_string and
// numbers via xsqlc_json_append_number (integer-when-whole). Object member order
// is preserved. Used by the RFC 7386 status merge (merge_patch.c).
void xsqlc_json_dump(const xsqlc_jval *v, xsqlc_strbuf *sb);

#endif  // XSQLC_INTERNAL_JSON_H
