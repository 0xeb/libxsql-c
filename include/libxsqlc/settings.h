// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// settings.h — thread-safe typed runtime settings, the canonical transactional
// runtime_settings SQL table, and the two imperative timeout-stack PRAGMAs.
// Every accessor is guarded by an internal mutex, so a single xsql_settings may be
// shared across threads (unlike xsql_db, which is one-thread-at-a-time).
//
// The settings object is standalone: it is NOT attached to an xsql_db. A product
// reads xsql_settings_get_query_timeout_ms() and passes it as
// xsql_query_opts::timeout_ms at its own query call sites.

#ifndef XSQLC_SETTINGS_H
#define XSQLC_SETTINGS_H

#include <libxsqlc/common.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque settings handle. Created by xsql_settings_new(); released with
// xsql_settings_free().
typedef struct xsql_settings xsql_settings;
struct xsql_db;

// Construction options. Designed for designated initializers; `struct_size` lets
// the ABI grow compatibly. Passing NULL to xsql_settings_new() selects all
// defaults. NOTE: when a struct IS provided, max_timeout_stack_depth is taken
// verbatim (there is no "unspecified" sentinel for it — 0 means unbounded, not
// "default"), so provide the depth you want.
typedef struct xsql_settings_opts {
    size_t struct_size;              // sizeof(xsql_settings_opts), or 0
    // Maximum depth of the timeout_push stack. Bounds a client from growing the
    // stack without limit (a memory-growth DoS). Taken verbatim from a provided
    // options struct; 0 means unbounded. When opts is NULL the default is 64.
    size_t max_timeout_stack_depth;
} xsql_settings_opts;

// Immutable snapshot of the settings at one instant. Filled by xsql_settings_snapshot().
typedef struct xsql_settings_snap {
    int    query_timeout_ms;             // default 60000
    int    queue_admission_timeout_ms;   // default 120000
    size_t max_queue;                    // default 64
    bool   hints_enabled;                // default true
    size_t timeout_stack_depth;          // current push/pop stack size
} xsql_settings_snap;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Create a settings object. `opts` may be NULL (all defaults). Returns NULL only
// on allocation failure.
XSQL_API xsql_settings *xsql_settings_new(const xsql_settings_opts *opts);

// Release a settings object. Passing NULL is a no-op.
XSQL_API void xsql_settings_free(xsql_settings *s);

// ---------------------------------------------------------------------------
// Snapshot + getters (each takes the internal lock)
// ---------------------------------------------------------------------------

// Fill *out with an atomic snapshot of the current settings. No-op if either
// pointer is NULL.
XSQL_API void xsql_settings_snapshot(xsql_settings *s, xsql_settings_snap *out);

XSQL_API int    xsql_settings_get_query_timeout_ms(xsql_settings *s);
XSQL_API int    xsql_settings_get_queue_admission_timeout_ms(xsql_settings *s);
XSQL_API size_t xsql_settings_get_max_queue(xsql_settings *s);
XSQL_API bool   xsql_settings_get_hints_enabled(xsql_settings *s);

// ---------------------------------------------------------------------------
// Validated setters. Return true on success; false (no mutation) if the value is
// out of range. Valid timeouts are [0, 3600000] ms (0 = no timeout); max_queue
// must be <= 10000. Setting hints always succeeds.
// ---------------------------------------------------------------------------

XSQL_API bool xsql_settings_set_query_timeout_ms(xsql_settings *s, int v);
XSQL_API bool xsql_settings_set_queue_admission_timeout_ms(xsql_settings *s, int v);
XSQL_API bool xsql_settings_set_max_queue(xsql_settings *s, size_t v);
XSQL_API void xsql_settings_set_hints_enabled(xsql_settings *s, bool v);

// ---------------------------------------------------------------------------
// Timeout stack. timeout_push saves the current query timeout and installs `ms`
// as the new effective value; timeout_pop restores the previous one. `effective`
// (may be NULL) receives the resulting effective query timeout. push returns
// false (no change) on an invalid value or when the depth cap is reached; pop
// returns false on an empty stack.
// ---------------------------------------------------------------------------

XSQL_API bool xsql_settings_timeout_push(xsql_settings *s, int ms, int *effective);
XSQL_API bool xsql_settings_timeout_pop(xsql_settings *s, int *effective);
XSQL_API size_t xsql_settings_get_max_timeout_stack_depth(xsql_settings *s);
XSQL_API bool xsql_settings_timeout_stack_active(xsql_settings *s);

// True when `UPDATE runtime_settings` accepts this key: registered writable AND
// not an imperative action verb. `scope` cannot answer this on its own --
// query_timeout_ms (tunable) and max_timeout_stack_depth (a read-only live
// counter) are both scope="common". Unknown keys report false.
XSQL_API bool xsql_settings_is_settable(xsql_settings *s, const char *key);
XSQL_API bool xsql_settings_staged_query_timeout_active(xsql_settings *s);

// ---------------------------------------------------------------------------
// Typed registry. Common settings are registered automatically; products can
// append their own bool/int/string values before constructing the SQL table.
// Keys and scopes must be non-empty and fit their public entry buffers.
// Duplicate keys and invalid defaults/bounds are rejected.
// ---------------------------------------------------------------------------

typedef enum xsql_runtime_setting_type {
    XSQL_SETTING_BOOL = 0,
    XSQL_SETTING_INT = 1,
    XSQL_SETTING_STRING = 2
} xsql_runtime_setting_type;

XSQL_API bool xsql_settings_register_bool(
    xsql_settings *s, const char *key, bool default_value,
    const char *scope, bool writable);
XSQL_API bool xsql_settings_register_int(
    xsql_settings *s, const char *key, long long default_value,
    long long minimum, long long maximum, const char *scope, bool writable);
XSQL_API bool xsql_settings_register_string(
    xsql_settings *s, const char *key, const char *default_value,
    const char *scope, bool writable);

// Copy one effective committed value to `out` (always NUL-terminated when cap
// is nonzero). Returns false for an unknown key or invalid arguments.
XSQL_API bool xsql_settings_get_value(
    xsql_settings *s, const char *key, char *out, size_t cap);

// ---------------------------------------------------------------------------
// PRAGMA parse + dispatch — `PRAGMA <product>.<key>[ = <value>]`
//
// The request/reply carry fixed inline buffers (no heap; nothing to free). Text
// longer than a buffer is truncated cleanly (runtime keys/values are short).
// ---------------------------------------------------------------------------

typedef struct xsql_runtime_pragma_request {
    bool matched;      // the statement is a `PRAGMA <product>.…` we recognize
    bool has_value;    // an `= value` clause was present
    char key[64];      // lower-cased key (e.g. "query_timeout_ms")
    char value[128];   // value with one layer of surrounding quotes stripped
} xsql_runtime_pragma_request;

typedef struct xsql_runtime_pragma_reply {
    bool handled;      // the key was a common runtime key (dispatch owned it)
    bool success;      // the get/set succeeded
    char name[64];     // canonical setting name reported back
    char value[64];    // resulting value (as text) on success
    char error[192];   // error text on failure
} xsql_runtime_pragma_reply;

// Wider reply for registered string settings. This is a distinct type and API
// so applications compiled against the original fixed-size reply cannot be
// overwritten by a newer library.
typedef struct xsql_runtime_pragma_reply_v2 {
    bool handled;
    bool success;
    char name[64];
    char value[128];
    char error[192];
} xsql_runtime_pragma_reply_v2;

// Parse `sql` as a runtime PRAGMA for `product_prefix` (case-insensitive, e.g.
// "idasql"). Fills *out and returns out->matched. A NULL argument or a
// non-matching statement yields matched==false. Never allocates.
XSQL_API bool xsql_parse_runtime_pragma(const char *sql,
                                        const char *product_prefix,
                                        xsql_runtime_pragma_request *out);

// Validate and canonicalize a registered value without mutating it. `handled`
// is false for an unknown key or an action row.
XSQL_API bool xsql_settings_normalize(
    xsql_settings *s, const char *key, const char *value,
    const char *product_prefix, xsql_runtime_pragma_reply *out);
XSQL_API bool xsql_settings_normalize_v2(
    xsql_settings *s, const char *key, const char *value,
    const char *product_prefix, xsql_runtime_pragma_reply_v2 *out);

// Validate and immediately commit a direct C-API write.
XSQL_API bool xsql_settings_apply(
    xsql_settings *s, const char *key, const char *value,
    const char *product_prefix, xsql_runtime_pragma_reply *out);
XSQL_API bool xsql_settings_apply_v2(
    xsql_settings *s, const char *key, const char *value,
    const char *product_prefix, xsql_runtime_pragma_reply_v2 *out);

// Dispatch only the imperative common PRAGMAs: timeout_push and timeout_pop.
// Value settings are changed exclusively through UPDATE runtime_settings.
XSQL_API bool xsql_handle_common_runtime_pragma(
    const xsql_runtime_pragma_request *req, const char *product_prefix,
    xsql_settings *s, xsql_runtime_pragma_reply *out);
XSQL_API bool xsql_handle_common_runtime_pragma_v2(
    const xsql_runtime_pragma_request *req, const char *product_prefix,
    xsql_settings *s, xsql_runtime_pragma_reply_v2 *out);

// ---------------------------------------------------------------------------
// Enumeration — backs `runtime_settings`. Fixed inline buffers keep it
// allocation-free for callers.
// ---------------------------------------------------------------------------

// One enumerated common runtime setting with its LIVE value.
typedef struct xsql_runtime_setting_entry {
    char key[64];      // e.g. "query_timeout_ms"
    char value[64];    // canonical text (int decimal, bool "1"/"0")
    char type[8];      // "int" | "bool" | "string"
    char scope[16];    // "common" | "action" | product scope
} xsql_runtime_setting_entry;

// Wider enumeration row for registered string values and product scopes.
typedef struct xsql_runtime_setting_entry_v2 {
    char key[64];
    char value[128];
    char type[8];
    char scope[64];
} xsql_runtime_setting_entry_v2;

// Number of common entries xsql_settings_enumerate_common() reports.
#define XSQL_RUNTIME_COMMON_COUNT 8

// Fill up to `cap` entries of `out` with the common runtime keys and their live
// values, in a stable order. Returns the number written (<= cap, and <=
// XSQL_RUNTIME_COMMON_COUNT). A product appends its own extra entries after
// these. Never allocates; a NULL `s`/`out` writes nothing and returns 0.
XSQL_API size_t xsql_settings_enumerate_common(
    xsql_settings *s, xsql_runtime_setting_entry *out, size_t cap);
XSQL_API size_t xsql_settings_enumerate_common_v2(
    xsql_settings *s, xsql_runtime_setting_entry_v2 *out, size_t cap);

// Enumerate every registered row in deterministic registration order.
XSQL_API size_t xsql_settings_count(xsql_settings *s);
XSQL_API size_t xsql_settings_enumerate(
    xsql_settings *s, xsql_runtime_setting_entry *out, size_t cap);
XSQL_API size_t xsql_settings_enumerate_v2(
    xsql_settings *s, xsql_runtime_setting_entry_v2 *out, size_t cap);

// ---------------------------------------------------------------------------
// Canonical transactional SQL table:
//   runtime_settings(key TEXT, value TEXT, type TEXT, scope TEXT)
// The handle and settings object must outlive every database registration.
// ---------------------------------------------------------------------------

typedef struct xsql_runtime_settings_table xsql_runtime_settings_table;

XSQL_API xsql_runtime_settings_table *xsql_runtime_settings_table_new(
    xsql_settings *settings, const char *product_prefix);
XSQL_API void xsql_runtime_settings_table_free(
    xsql_runtime_settings_table *table);
XSQL_API int xsql_runtime_settings_table_register(
    struct xsql_db *db, xsql_runtime_settings_table *table);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_SETTINGS_H
