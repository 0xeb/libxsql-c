// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// regex.h — a small backtracking regular-expression engine.
//
// WHY THIS EXISTS. Consumers expose SQLite's REGEXP operator, which SQLite
// itself does not implement. The C++ sibling backs it with std::regex in
// ECMAScript mode. C11 has no regex at all, and POSIX <regex.h> is absent on
// MSVC, so there is nothing to bind to — hence an engine here.
//
// ---------------------------------------------------------------------------
// SUPPORTED — a documented SUBSET of ECMAScript, not the whole grammar
// ---------------------------------------------------------------------------
//   anchors            ^ $          (LINE anchors -- see the note below)
//   any                .            (any byte except \n and \r)
//   literals           and \-escapes of metacharacters
//   escape classes     \d \D \w \W \s \S
//   assertions         \b \B
//   control escapes    \n \r \t \f \v \0, \xHH, \uHHHH (BMP -> UTF-8)
//   classes            [abc] [^abc] [a-z] with escapes inside
//   groups             ( ... ) and (?: ... )   -- capture positions are NOT
//                                                 reported; there is no
//                                                 sub-match API
//   alternation        |
//   quantifiers        * + ? {n} {n,} {n,m}, each greedy or lazy (trailing ?)
//
// ---------------------------------------------------------------------------
// NOT SUPPORTED — these fail to COMPILE rather than silently mismatching
// ---------------------------------------------------------------------------
//   backreferences     \1 .. \9
//   lookaround         (?= (?! (?<= (?<!
//   named groups       (?<name> ...)
//   unicode property   \p{...} \P{...}
//   inline flags       (?i) etc.  -- matching is always case-SENSITIVE
//
// A caller that must not diverge from ECMAScript should treat a compile
// failure as "this pattern is outside the supported subset" and say so, rather
// than treating it as "no match". (The REGEXP operator's own contract is that
// an invalid pattern yields 0, which is what the C++ side does on a throw.)
//
// ^ AND $ ARE LINE ANCHORS. `^` matches at the start of the subject and just
// after a \n or \r; `$` matches at the end and just before one. This is not a
// guess about ECMA-262 — it is what std::regex actually does, established by
// the differential suite (`^.$` matches "a\nb"). Matching the oracle is the
// point; an idealized reading that disagreed with it would be worse.
//
// BYTES, NOT CODE POINTS. Matching is byte-oriented. `.`, classes and
// quantifiers operate on bytes, so they can split a multi-byte UTF-8 sequence.
// Literal UTF-8 in a pattern still matches literal UTF-8 in the subject,
// because both are compared bytewise.
//
// THREADING. A compiled xsql_regex is immutable; concurrent search/match on
// one handle is safe. Compiling and freeing are not synchronized.

#ifndef XSQLC_REGEX_H
#define XSQLC_REGEX_H

#include <libxsqlc/common.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque compiled pattern.
typedef struct xsql_regex xsql_regex;

// Compile `pattern`. Returns NULL on a syntax error or an unsupported
// construct; when `errbuf`/`errbuf_len` are given, a short human-readable
// reason is written there (always NUL-terminated). Free with xsql_regex_free().
XSQL_API xsql_regex *xsql_regex_compile(const char *pattern, char *errbuf,
                                        size_t errbuf_len);

XSQL_API void xsql_regex_free(xsql_regex *re);

// std::regex_search semantics: true when the pattern matches ANYWHERE in
// `text`. This is what the SQL REGEXP operator uses. `text` is NUL-terminated;
// a NULL `re` or `text` is false.
XSQL_API bool xsql_regex_search(const xsql_regex *re, const char *text);

// std::regex_match semantics: true only when the pattern matches the ENTIRE
// text.
XSQL_API bool xsql_regex_match(const xsql_regex *re, const char *text);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XSQLC_REGEX_H
