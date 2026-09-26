// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// settings.c — thread-safe typed runtime settings and action-PRAGMA dispatch.

#include <libxsqlc/settings.h>

#include "internal/internal.h"
#include "internal/settings_internal.h"
#include "internal/vec.h"

#include <sqlite3.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XSQLC_MAX_TIMEOUT_MS 3600000LL
#define XSQLC_MAX_QUEUE_LIMIT 10000LL

typedef struct xsqlc_setting_record {
    char key[64];
    char value[128];
    char type[8];
    char scope[64];
    xsql_runtime_setting_type setting_type;
    bool writable;
    long long minimum;
    long long maximum;
} xsqlc_setting_record;

struct xsql_settings {
    sqlite3_mutex *mtx;
    xsqlc_setting_record *records;
    size_t record_count;
    size_t record_cap;
    int *stack;
    size_t stack_len;
    size_t stack_cap;
    size_t max_stack_depth;
    size_t staged_query_timeout_transactions;
};

typedef struct {
    const char *p;
    size_t n;
} sview;

static bool field_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0 || !src) {
        return false;
    }
    size_t len = strlen(src);
    if (len >= cap) {
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

static void copy_truncated(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) {
        return;
    }
    // Bounded copy (not snprintf "%s"): copying a wider source field into a
    // narrower destination is a deliberate truncation, and at -O2 gcc's
    // -Wformat-truncation flags the snprintf form even though it is safe. A
    // length-checked memcpy expresses the same truncation without the warning.
    size_t len = src ? strlen(src) : 0;
    if (len > cap - 1) {
        len = cap - 1;
    }
    if (len > 0) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static bool ci_equal(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    for (; *a && *b; ++a, ++b) {
        if (tolower((unsigned char)*a) !=
            tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static bool parse_i64_strict(const char *text, long long *out) {
    if (!text || !out || *text == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_bool_strict(const char *text, bool *out) {
    if (!out) {
        return false;
    }
    if (ci_equal(text, "1") || ci_equal(text, "on") ||
        ci_equal(text, "true") || ci_equal(text, "yes")) {
        *out = true;
        return true;
    }
    if (ci_equal(text, "0") || ci_equal(text, "off") ||
        ci_equal(text, "false") || ci_equal(text, "no")) {
        *out = false;
        return true;
    }
    return false;
}

static xsqlc_setting_record *find_record_locked(
    xsql_settings *settings, const char *key) {
    if (!settings || !key) {
        return NULL;
    }
    for (size_t i = 0; i < settings->record_count; ++i) {
        if (strcmp(settings->records[i].key, key) == 0) {
            return &settings->records[i];
        }
    }
    return NULL;
}

static const char *type_name(xsql_runtime_setting_type type) {
    switch (type) {
        case XSQL_SETTING_BOOL: return "bool";
        case XSQL_SETTING_INT: return "int";
        case XSQL_SETTING_STRING: return "string";
    }
    return "string";
}

static bool register_locked(
    xsql_settings *settings, const char *key,
    xsql_runtime_setting_type type, const char *default_value,
    long long minimum, long long maximum, const char *scope,
    bool writable) {
    if (!settings || !key || !scope || !default_value ||
        key[0] == '\0' || scope[0] == '\0' ||
        find_record_locked(settings, key)) {
        return false;
    }
    if (strlen(key) >= sizeof(settings->records[0].key) ||
        strlen(default_value) >= sizeof(settings->records[0].value) ||
        strlen(scope) >= sizeof(settings->records[0].scope)) {
        return false;
    }
    // A string setting's default must satisfy the same "not empty" bound as a
    // written value (the C++ contract validates the default at registration too).
    if (type == XSQL_SETTING_STRING && default_value[0] == '\0') {
        return false;
    }
    if (!xsqlc_vec_reserve(
            (void **)&settings->records, &settings->record_cap,
            settings->record_count + 1, sizeof(*settings->records))) {
        return false;
    }
    xsqlc_setting_record *record =
        &settings->records[settings->record_count];
    memset(record, 0, sizeof(*record));
    if (!field_copy(record->key, sizeof(record->key), key) ||
        !field_copy(record->value, sizeof(record->value), default_value) ||
        !field_copy(record->type, sizeof(record->type), type_name(type)) ||
        !field_copy(record->scope, sizeof(record->scope), scope)) {
        return false;
    }
    record->setting_type = type;
    record->writable = writable;
    record->minimum = minimum;
    record->maximum = maximum;
    ++settings->record_count;
    return true;
}

static bool register_int_locked(
    xsql_settings *settings, const char *key, long long default_value,
    long long minimum, long long maximum, const char *scope,
    bool writable) {
    if (minimum > maximum || default_value < minimum ||
        default_value > maximum) {
        return false;
    }
    char value[64];
    snprintf(value, sizeof(value), "%lld", default_value);
    return register_locked(settings, key, XSQL_SETTING_INT, value,
                           minimum, maximum, scope, writable);
}

static bool register_common(xsql_settings *settings) {
    return register_int_locked(
               settings, "query_timeout_ms", 60000, 0,
               XSQLC_MAX_TIMEOUT_MS, "common", true) &&
           register_int_locked(
               settings, "queue_admission_timeout_ms", 120000, 0,
               XSQLC_MAX_TIMEOUT_MS, "common", true) &&
           register_int_locked(
               settings, "max_queue", 64, 0, XSQLC_MAX_QUEUE_LIMIT,
               "common", true) &&
           register_locked(
               settings, "hints_enabled", XSQL_SETTING_BOOL, "1",
               LLONG_MIN, LLONG_MAX, "common", true) &&
           register_int_locked(
               settings, "timeout_stack_depth", 0, 0, LLONG_MAX,
               "common", false) &&
           register_int_locked(
               settings, "max_timeout_stack_depth",
               settings->max_stack_depth > (size_t)LLONG_MAX
                   ? LLONG_MAX
                   : (long long)settings->max_stack_depth,
               0, LLONG_MAX, "common", false) &&
           register_int_locked(
               settings, "timeout_push", 60000, 0,
               XSQLC_MAX_TIMEOUT_MS, "action", false) &&
           register_int_locked(
               settings, "timeout_pop", 60000, 0,
               XSQLC_MAX_TIMEOUT_MS, "action", false);
}

xsql_settings *xsql_settings_new(const xsql_settings_opts *opts) {
    xsql_settings *settings =
        (xsql_settings *)xsqlc_malloc(sizeof(*settings));
    if (!settings) {
        return NULL;
    }
    memset(settings, 0, sizeof(*settings));
    settings->max_stack_depth =
        opts ? opts->max_timeout_stack_depth : 64;
    settings->mtx = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
    if (!register_common(settings)) {
        xsql_settings_free(settings);
        return NULL;
    }
    return settings;
}

void xsql_settings_free(xsql_settings *settings) {
    if (!settings) {
        return;
    }
    sqlite3_mutex_free(settings->mtx);
    xsqlc_free(settings->records);
    xsqlc_free(settings->stack);
    xsqlc_free(settings);
}

static bool effective_value_locked(
    xsql_settings *settings, const xsqlc_setting_record *record,
    char *out, size_t cap) {
    if (!settings || !record || !out || cap == 0) {
        return false;
    }
    if (strcmp(record->key, "timeout_stack_depth") == 0) {
        snprintf(out, cap, "%zu", settings->stack_len);
        return true;
    }
    if (strcmp(record->key, "timeout_push") == 0 ||
        strcmp(record->key, "timeout_pop") == 0) {
        const xsqlc_setting_record *timeout =
            find_record_locked(settings, "query_timeout_ms");
        copy_truncated(out, cap, timeout ? timeout->value : "0");
        return timeout != NULL;
    }
    copy_truncated(out, cap, record->value);
    return true;
}

static long long integer_value_locked(
    xsql_settings *settings, const char *key, long long fallback) {
    xsqlc_setting_record *record = find_record_locked(settings, key);
    long long value = 0;
    if (!record || !parse_i64_strict(record->value, &value)) {
        return fallback;
    }
    return value;
}

void xsql_settings_snapshot(
    xsql_settings *settings, xsql_settings_snap *out) {
    if (!settings || !out) {
        return;
    }
    sqlite3_mutex_enter(settings->mtx);
    out->query_timeout_ms = (int)integer_value_locked(
        settings, "query_timeout_ms", 60000);
    out->queue_admission_timeout_ms = (int)integer_value_locked(
        settings, "queue_admission_timeout_ms", 120000);
    out->max_queue = (size_t)integer_value_locked(
        settings, "max_queue", 64);
    xsqlc_setting_record *hints =
        find_record_locked(settings, "hints_enabled");
    out->hints_enabled = hints && strcmp(hints->value, "1") == 0;
    out->timeout_stack_depth = settings->stack_len;
    sqlite3_mutex_leave(settings->mtx);
}

int xsql_settings_get_query_timeout_ms(xsql_settings *settings) {
    if (!settings) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    int value = (int)integer_value_locked(
        settings, "query_timeout_ms", 0);
    sqlite3_mutex_leave(settings->mtx);
    return value;
}

int xsql_settings_get_queue_admission_timeout_ms(
    xsql_settings *settings) {
    if (!settings) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    int value = (int)integer_value_locked(
        settings, "queue_admission_timeout_ms", 0);
    sqlite3_mutex_leave(settings->mtx);
    return value;
}

size_t xsql_settings_get_max_queue(xsql_settings *settings) {
    if (!settings) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    size_t value = (size_t)integer_value_locked(
        settings, "max_queue", 0);
    sqlite3_mutex_leave(settings->mtx);
    return value;
}

bool xsql_settings_get_hints_enabled(xsql_settings *settings) {
    if (!settings) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    xsqlc_setting_record *record =
        find_record_locked(settings, "hints_enabled");
    bool value = record && strcmp(record->value, "1") == 0;
    sqlite3_mutex_leave(settings->mtx);
    return value;
}

size_t xsql_settings_get_max_timeout_stack_depth(
    xsql_settings *settings) {
    if (!settings) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    size_t value = settings->max_stack_depth;
    sqlite3_mutex_leave(settings->mtx);
    return value;
}

bool xsql_settings_is_settable(xsql_settings *settings, const char *key) {
    if (!settings || !key) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    const xsqlc_setting_record *record = find_record_locked(settings, key);
    // An action row is never settable even if registered writable: timeout_push
    // and timeout_pop are imperative verbs, not values.
    bool settable = record && record->writable &&
                    strcmp(record->scope, "action") != 0;
    sqlite3_mutex_leave(settings->mtx);
    return settable;
}

bool xsql_settings_timeout_stack_active(xsql_settings *settings) {
    if (!settings) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    bool active = settings->stack_len != 0;
    sqlite3_mutex_leave(settings->mtx);
    return active;
}

bool xsql_settings_staged_query_timeout_active(
    xsql_settings *settings) {
    if (!settings) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    bool active =
        settings->staged_query_timeout_transactions != 0;
    sqlite3_mutex_leave(settings->mtx);
    return active;
}

static void reply_result_v2(
    xsql_runtime_pragma_reply_v2 *out, const char *name,
    const char *value) {
    memset(out, 0, sizeof(*out));
    out->handled = true;
    out->success = true;
    copy_truncated(out->name, sizeof(out->name), name);
    copy_truncated(out->value, sizeof(out->value), value);
}

static void reply_errorf_v2(
    xsql_runtime_pragma_reply_v2 *out, const char *fmt, ...) {
    memset(out, 0, sizeof(*out));
    out->handled = true;
    va_list args;
    va_start(args, fmt);
    vsnprintf(out->error, sizeof(out->error), fmt, args);
    va_end(args);
}

static bool normalize_locked(
    xsql_settings *settings, const char *key, const char *input,
    const char *prefix, xsql_runtime_pragma_reply_v2 *out) {
    memset(out, 0, sizeof(*out));
    xsqlc_setting_record *record =
        find_record_locked(settings, key);
    if (!record || strcmp(record->scope, "action") == 0) {
        return false;
    }
    if (!record->writable) {
        reply_errorf_v2(out, "runtime_settings: '%s' is read-only", key);
        return true;
    }

    char normalized[128];
    normalized[0] = '\0';
    if (record->setting_type == XSQL_SETTING_BOOL) {
        bool value = false;
        if (!parse_bool_strict(input, &value)) {
            reply_errorf_v2(out, "Invalid %s.%s value", prefix, key);
            return true;
        }
        copy_truncated(normalized, sizeof(normalized),
                       value ? "1" : "0");
    } else if (record->setting_type == XSQL_SETTING_INT) {
        long long value = 0;
        if (!parse_i64_strict(input, &value) ||
            value < record->minimum || value > record->maximum) {
            reply_errorf_v2(out, "Invalid %s.%s value", prefix, key);
            return true;
        }
        snprintf(normalized, sizeof(normalized), "%lld", value);
    } else {
        // Reject empty (an empty string is the same "unset" shape the writable
        // table already rejects as NULL). The upper bound is the record's fixed
        // inline buffer — a deliberate C replacement for the reference 4096-byte
        // string-setting cap.
        if (!input || input[0] == '\0') {
            reply_errorf_v2(
                out, "Invalid %s.%s value (may not be empty)", prefix, key);
            return true;
        }
        if (strlen(input) >= sizeof(normalized)) {
            reply_errorf_v2(out, "Invalid %s.%s value", prefix, key);
            return true;
        }
        copy_truncated(normalized, sizeof(normalized), input);
    }
    reply_result_v2(out, key, normalized);
    return true;
}

static void copy_reply_v2_to_v1(
    xsql_runtime_pragma_reply *out,
    const xsql_runtime_pragma_reply_v2 *wide) {
    memset(out, 0, sizeof(*out));
    out->handled = wide->handled;
    out->success = wide->success;
    copy_truncated(out->name, sizeof(out->name), wide->name);
    copy_truncated(out->value, sizeof(out->value), wide->value);
    copy_truncated(out->error, sizeof(out->error), wide->error);
}

bool xsql_settings_normalize_v2(
    xsql_settings *settings, const char *key, const char *value,
    const char *product_prefix, xsql_runtime_pragma_reply_v2 *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!settings || !key || !value || !product_prefix) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    bool handled = normalize_locked(
        settings, key, value, product_prefix, out);
    sqlite3_mutex_leave(settings->mtx);
    return handled;
}

bool xsql_settings_normalize(
    xsql_settings *settings, const char *key, const char *value,
    const char *product_prefix, xsql_runtime_pragma_reply *out) {
    if (!out) {
        return false;
    }
    xsql_runtime_pragma_reply_v2 wide;
    bool handled = xsql_settings_normalize_v2(
        settings, key, value, product_prefix, &wide);
    copy_reply_v2_to_v1(out, &wide);
    return handled;
}

bool xsql_settings_apply_v2(
    xsql_settings *settings, const char *key, const char *value,
    const char *product_prefix, xsql_runtime_pragma_reply_v2 *out) {
    xsql_runtime_pragma_reply_v2 local;
    if (!out) {
        out = &local;
    }
    memset(out, 0, sizeof(*out));
    if (!settings || !key || !value || !product_prefix) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    bool handled = normalize_locked(
        settings, key, value, product_prefix, out);
    if (handled && out->success) {
        xsqlc_setting_record *record =
            find_record_locked(settings, key);
        if (record) {
            copy_truncated(
                record->value, sizeof(record->value), out->value);
        }
    }
    sqlite3_mutex_leave(settings->mtx);
    return handled;
}

bool xsql_settings_apply(
    xsql_settings *settings, const char *key, const char *value,
    const char *product_prefix, xsql_runtime_pragma_reply *out) {
    xsql_runtime_pragma_reply_v2 wide;
    bool handled = xsql_settings_apply_v2(
        settings, key, value, product_prefix, &wide);
    if (out) {
        copy_reply_v2_to_v1(out, &wide);
    }
    return handled;
}

bool xsql_settings_set_query_timeout_ms(
    xsql_settings *settings, int value) {
    char input[32];
    snprintf(input, sizeof(input), "%d", value);
    xsql_runtime_pragma_reply_v2 reply;
    return xsql_settings_apply_v2(
               settings, "query_timeout_ms", input, "runtime", &reply) &&
           reply.success;
}

bool xsql_settings_set_queue_admission_timeout_ms(
    xsql_settings *settings, int value) {
    char input[32];
    snprintf(input, sizeof(input), "%d", value);
    xsql_runtime_pragma_reply_v2 reply;
    return xsql_settings_apply_v2(
               settings, "queue_admission_timeout_ms", input,
               "runtime", &reply) &&
           reply.success;
}

bool xsql_settings_set_max_queue(
    xsql_settings *settings, size_t value) {
    char input[32];
    snprintf(input, sizeof(input), "%zu", value);
    xsql_runtime_pragma_reply_v2 reply;
    return xsql_settings_apply_v2(
               settings, "max_queue", input, "runtime", &reply) &&
           reply.success;
}

void xsql_settings_set_hints_enabled(
    xsql_settings *settings, bool value) {
    xsql_runtime_pragma_reply_v2 reply;
    (void)xsql_settings_apply_v2(
        settings, "hints_enabled", value ? "1" : "0",
        "runtime", &reply);
}

bool xsql_settings_register_bool(
    xsql_settings *settings, const char *key, bool default_value,
    const char *scope, bool writable) {
    if (!settings) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    bool ok = register_locked(
        settings, key, XSQL_SETTING_BOOL,
        default_value ? "1" : "0", LLONG_MIN, LLONG_MAX,
        scope, writable);
    sqlite3_mutex_leave(settings->mtx);
    return ok;
}

bool xsql_settings_register_int(
    xsql_settings *settings, const char *key, long long default_value,
    long long minimum, long long maximum, const char *scope,
    bool writable) {
    if (!settings) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    bool ok = register_int_locked(
        settings, key, default_value, minimum, maximum,
        scope, writable);
    sqlite3_mutex_leave(settings->mtx);
    return ok;
}

bool xsql_settings_register_string(
    xsql_settings *settings, const char *key,
    const char *default_value, const char *scope, bool writable) {
    if (!settings || !default_value) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    bool ok = register_locked(
        settings, key, XSQL_SETTING_STRING, default_value,
        LLONG_MIN, LLONG_MAX, scope, writable);
    sqlite3_mutex_leave(settings->mtx);
    return ok;
}

bool xsql_settings_get_value(
    xsql_settings *settings, const char *key, char *out, size_t cap) {
    if (!settings || !key || !out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    sqlite3_mutex_enter(settings->mtx);
    xsqlc_setting_record *record =
        find_record_locked(settings, key);
    bool ok = effective_value_locked(settings, record, out, cap);
    sqlite3_mutex_leave(settings->mtx);
    return ok;
}

bool xsql_settings_timeout_push(
    xsql_settings *settings, int timeout_ms, int *effective) {
    if (!settings || timeout_ms < 0 ||
        timeout_ms > XSQLC_MAX_TIMEOUT_MS) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    if (settings->staged_query_timeout_transactions != 0 ||
        (settings->max_stack_depth != 0 &&
         settings->stack_len >= settings->max_stack_depth) ||
        !xsqlc_vec_reserve(
            (void **)&settings->stack, &settings->stack_cap,
            settings->stack_len + 1, sizeof(*settings->stack))) {
        sqlite3_mutex_leave(settings->mtx);
        return false;
    }
    xsqlc_setting_record *record =
        find_record_locked(settings, "query_timeout_ms");
    if (!record) {
        sqlite3_mutex_leave(settings->mtx);
        return false;
    }
    int previous = (int)integer_value_locked(
        settings, "query_timeout_ms", 0);
    settings->stack[settings->stack_len++] = previous;
    snprintf(record->value, sizeof(record->value), "%d", timeout_ms);
    if (effective) {
        *effective = timeout_ms;
    }
    sqlite3_mutex_leave(settings->mtx);
    return true;
}

bool xsql_settings_timeout_pop(
    xsql_settings *settings, int *effective) {
    if (!settings) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    if (settings->staged_query_timeout_transactions != 0 ||
        settings->stack_len == 0) {
        sqlite3_mutex_leave(settings->mtx);
        return false;
    }
    int previous = settings->stack[--settings->stack_len];
    xsqlc_setting_record *record =
        find_record_locked(settings, "query_timeout_ms");
    if (!record) {
        sqlite3_mutex_leave(settings->mtx);
        return false;
    }
    snprintf(record->value, sizeof(record->value), "%d", previous);
    if (effective) {
        *effective = previous;
    }
    sqlite3_mutex_leave(settings->mtx);
    return true;
}

size_t xsql_settings_count(xsql_settings *settings) {
    if (!settings) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    size_t count = settings->record_count;
    sqlite3_mutex_leave(settings->mtx);
    return count;
}

static void fill_entry_locked(
    xsql_settings *settings, const xsqlc_setting_record *record,
    xsql_runtime_setting_entry_v2 *entry) {
    memset(entry, 0, sizeof(*entry));
    copy_truncated(entry->key, sizeof(entry->key), record->key);
    (void)effective_value_locked(
        settings, record, entry->value, sizeof(entry->value));
    copy_truncated(entry->type, sizeof(entry->type), record->type);
    copy_truncated(entry->scope, sizeof(entry->scope), record->scope);
}

size_t xsql_settings_enumerate_v2(
    xsql_settings *settings, xsql_runtime_setting_entry_v2 *out,
    size_t cap) {
    if (!settings || !out || cap == 0) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    size_t count =
        settings->record_count < cap ? settings->record_count : cap;
    for (size_t i = 0; i < count; ++i) {
        fill_entry_locked(settings, &settings->records[i], &out[i]);
    }
    sqlite3_mutex_leave(settings->mtx);
    return count;
}

size_t xsql_settings_enumerate_common_v2(
    xsql_settings *settings, xsql_runtime_setting_entry_v2 *out,
    size_t cap) {
    if (!settings || !out || cap == 0) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    size_t written = 0;
    for (size_t i = 0;
         i < settings->record_count && written < cap; ++i) {
        const xsqlc_setting_record *record = &settings->records[i];
        if (strcmp(record->scope, "common") != 0 &&
            strcmp(record->scope, "action") != 0) {
            continue;
        }
        fill_entry_locked(settings, record, &out[written++]);
    }
    sqlite3_mutex_leave(settings->mtx);
    return written;
}

static void copy_entry_v2_to_v1(
    xsql_runtime_setting_entry *out,
    const xsql_runtime_setting_entry_v2 *wide) {
    memset(out, 0, sizeof(*out));
    copy_truncated(out->key, sizeof(out->key), wide->key);
    copy_truncated(out->value, sizeof(out->value), wide->value);
    copy_truncated(out->type, sizeof(out->type), wide->type);
    copy_truncated(out->scope, sizeof(out->scope), wide->scope);
}

size_t xsql_settings_enumerate(
    xsql_settings *settings, xsql_runtime_setting_entry *out,
    size_t cap) {
    if (!settings || !out || cap == 0) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    size_t count =
        settings->record_count < cap ? settings->record_count : cap;
    for (size_t i = 0; i < count; ++i) {
        xsql_runtime_setting_entry_v2 wide;
        fill_entry_locked(settings, &settings->records[i], &wide);
        copy_entry_v2_to_v1(&out[i], &wide);
    }
    sqlite3_mutex_leave(settings->mtx);
    return count;
}

size_t xsql_settings_enumerate_common(
    xsql_settings *settings, xsql_runtime_setting_entry *out,
    size_t cap) {
    if (!settings || !out || cap == 0) {
        return 0;
    }
    sqlite3_mutex_enter(settings->mtx);
    size_t written = 0;
    for (size_t i = 0;
         i < settings->record_count && written < cap; ++i) {
        const xsqlc_setting_record *record = &settings->records[i];
        if (strcmp(record->scope, "common") != 0 &&
            strcmp(record->scope, "action") != 0) {
            continue;
        }
        xsql_runtime_setting_entry_v2 wide;
        fill_entry_locked(settings, record, &wide);
        copy_entry_v2_to_v1(&out[written++], &wide);
    }
    sqlite3_mutex_leave(settings->mtx);
    return written;
}

bool xsqlc_settings_begin_staged_query_timeout(
    xsql_settings *settings) {
    if (!settings) {
        return false;
    }
    sqlite3_mutex_enter(settings->mtx);
    bool ok = settings->stack_len == 0;
    if (ok) {
        ++settings->staged_query_timeout_transactions;
    }
    sqlite3_mutex_leave(settings->mtx);
    return ok;
}

void xsqlc_settings_discard_staged_query_timeout(
    xsql_settings *settings) {
    if (!settings) {
        return;
    }
    sqlite3_mutex_enter(settings->mtx);
    if (settings->staged_query_timeout_transactions != 0) {
        --settings->staged_query_timeout_transactions;
    }
    sqlite3_mutex_leave(settings->mtx);
}

void xsqlc_settings_commit_staged(
    xsql_settings *settings, const xsqlc_setting_update *updates,
    size_t count, bool owns_staged_query_timeout) {
    if (!settings) {
        return;
    }
    sqlite3_mutex_enter(settings->mtx);
    for (size_t i = 0; i < count; ++i) {
        xsqlc_setting_record *record =
            find_record_locked(settings, updates[i].key);
        if (record && record->writable) {
            copy_truncated(
                record->value, sizeof(record->value),
                updates[i].value);
        }
    }
    if (owns_staged_query_timeout &&
        settings->staged_query_timeout_transactions != 0) {
        --settings->staged_query_timeout_transactions;
    }
    sqlite3_mutex_leave(settings->mtx);
}

static sview sv_trim(sview view) {
    while (view.n > 0 &&
           isspace((unsigned char)view.p[0])) {
        ++view.p;
        --view.n;
    }
    while (view.n > 0 &&
           isspace((unsigned char)view.p[view.n - 1])) {
        --view.n;
    }
    return view;
}

static sview sv_strip_quotes(sview view) {
    if (view.n >= 2) {
        char first = view.p[0];
        char last = view.p[view.n - 1];
        if ((first == '\'' && last == '\'') ||
            (first == '"' && last == '"')) {
            ++view.p;
            view.n -= 2;
        }
    }
    return view;
}

static void sv_copy(
    char *dst, size_t cap, sview view, bool lower) {
    size_t i = 0;
    for (; i + 1 < cap && i < view.n; ++i) {
        unsigned char value = (unsigned char)view.p[i];
        dst[i] = lower ? (char)tolower(value) : (char)value;
    }
    dst[i] = '\0';
}

static bool sv_starts_with(sview view, const char *prefix) {
    size_t len = strlen(prefix);
    if (view.n < len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (tolower((unsigned char)view.p[i]) !=
            tolower((unsigned char)prefix[i])) {
            return false;
        }
    }
    return true;
}

// Replace every SQL comment (line `--`, block `/* */`) with a single space,
// leaving quoted literals untouched. One-pass, quote-aware: a line comment ends
// at '\n' or a lone '\r'; a block comment runs to "*/" or end-of-input; a doubled
// quote inside a literal stays inside it. Mirrors the C++ one-pass
// strip_sql_comments so `;;`, interior comments, and lone-CR line ends all
// normalize identically. Output is bounded by `cap` (a runtime PRAGMA is short —
// only timeout_push/pop are PRAGMAs and their values are small integers) and is
// always NUL-terminated; the written length is returned.
static size_t strip_sql_comments(
    const char *src, size_t n, char *out, size_t cap) {
    size_t w = 0;
    size_t i = 0;
    while (i < n) {
        char c = src[i];
        if (c == '\'' || c == '"') {
            char quote = c;
            if (w + 1 < cap) {
                out[w++] = c;
            }
            ++i;
            while (i < n) {
                if (w + 1 < cap) {
                    out[w++] = src[i];
                }
                if (src[i] == quote) {
                    if (i + 1 < n && src[i + 1] == quote) {
                        if (w + 1 < cap) {
                            out[w++] = src[i + 1];
                        }
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (c == '-' && i + 1 < n && src[i + 1] == '-') {
            i += 2;
            while (i < n && src[i] != '\n' && src[i] != '\r') {
                ++i;
            }
            if (w + 1 < cap) {
                out[w++] = ' ';
            }
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n &&
                   !(src[j] == '*' && src[j + 1] == '/')) {
                ++j;
            }
            i = (j + 1 < n) ? j + 2 : n;
            if (w + 1 < cap) {
                out[w++] = ' ';
            }
            continue;
        }
        if (w + 1 < cap) {
            out[w++] = c;
        }
        ++i;
    }
    out[w] = '\0';
    return w;
}

bool xsql_parse_runtime_pragma(
    const char *sql, const char *product_prefix,
    xsql_runtime_pragma_request *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!sql || !product_prefix || product_prefix[0] == '\0') {
        return false;
    }
    char scratch[2048];
    size_t scratch_len =
        strip_sql_comments(sql, strlen(sql), scratch, sizeof(scratch));
    sview text = {scratch, scratch_len};
    text = sv_trim(text);
    // Statement terminators (also `;;` — comment stripping can expose more than
    // one).
    for (;;) {
        text = sv_trim(text);
        if (text.n > 0 && text.p[text.n - 1] == ';') {
            --text.n;
        } else {
            break;
        }
    }
    text = sv_trim(text);
    if (!sv_starts_with(text, "pragma")) {
        return false;
    }
    // Word boundary: `pragmatic_helper(...)` starts with "pragma" but is not the
    // PRAGMA keyword — the next byte must be whitespace.
    if (text.n <= 6 || !isspace((unsigned char)text.p[6])) {
        return false;
    }
    text.p += 6;
    text.n -= 6;
    text = sv_trim(text);

    size_t prefix_len = strlen(product_prefix);
    if (text.n < prefix_len + 1) {
        return false;
    }
    for (size_t i = 0; i < prefix_len; ++i) {
        if (tolower((unsigned char)text.p[i]) !=
            tolower((unsigned char)product_prefix[i])) {
            return false;
        }
    }
    if (text.p[prefix_len] != '.') {
        return false;
    }
    text.p += prefix_len + 1;
    text.n -= prefix_len + 1;
    text = sv_trim(text);

    sview key = text;
    sview value = {NULL, 0};
    for (size_t i = 0; i < text.n; ++i) {
        if (text.p[i] == '=') {
            out->has_value = true;
            key.n = i;
            value.p = text.p + i + 1;
            value.n = text.n - i - 1;
            break;
        }
    }
    key = sv_trim(key);
    if (out->has_value) {
        value = sv_strip_quotes(sv_trim(value));
    }
    out->matched = true;
    sv_copy(out->key, sizeof(out->key), key, true);
    sv_copy(out->value, sizeof(out->value), value, false);
    return true;
}

bool xsql_handle_common_runtime_pragma_v2(
    const xsql_runtime_pragma_request *request,
    const char *product_prefix, xsql_settings *settings,
    xsql_runtime_pragma_reply_v2 *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!request || !request->matched || !product_prefix ||
        !settings) {
        return false;
    }
    if (strcmp(request->key, "timeout_push") == 0) {
        if (!request->has_value || request->value[0] == '\0') {
            reply_errorf_v2(
                out, "%s.timeout_push requires a timeout value",
                product_prefix);
            return true;
        }
        long long parsed = 0;
        if (!parse_i64_strict(request->value, &parsed) ||
            parsed < 0 || parsed > XSQLC_MAX_TIMEOUT_MS) {
            reply_errorf_v2(
                out, "Invalid %s.timeout_push value",
                product_prefix);
            return true;
        }
        int effective = 0;
        if (!xsql_settings_timeout_push(
                settings, (int)parsed, &effective)) {
            if (xsql_settings_staged_query_timeout_active(settings)) {
                reply_errorf_v2(
                    out,
                    "%s.timeout_push is unavailable while "
                    "runtime_settings has a staged query_timeout_ms write",
                    product_prefix);
            } else {
                reply_errorf_v2(
                    out, "%s.timeout_push stack full (max %zu entries)",
                    product_prefix,
                    xsql_settings_get_max_timeout_stack_depth(settings));
            }
            return true;
        }
        char value[32];
        snprintf(value, sizeof(value), "%d", effective);
        reply_result_v2(out, "query_timeout_ms", value);
        return true;
    }
    if (strcmp(request->key, "timeout_pop") == 0) {
        int effective = 0;
        if (!xsql_settings_timeout_pop(settings, &effective)) {
            if (xsql_settings_staged_query_timeout_active(settings)) {
                reply_errorf_v2(
                    out,
                    "%s.timeout_pop is unavailable while "
                    "runtime_settings has a staged query_timeout_ms write",
                    product_prefix);
            } else {
                reply_errorf_v2(
                    out, "%s.timeout_pop stack is empty",
                    product_prefix);
            }
            return true;
        }
        char value[32];
        snprintf(value, sizeof(value), "%d", effective);
        reply_result_v2(out, "query_timeout_ms", value);
        return true;
    }
    return false;
}

bool xsql_handle_common_runtime_pragma(
    const xsql_runtime_pragma_request *request,
    const char *product_prefix, xsql_settings *settings,
    xsql_runtime_pragma_reply *out) {
    if (!out) {
        return false;
    }
    xsql_runtime_pragma_reply_v2 wide;
    bool handled = xsql_handle_common_runtime_pragma_v2(
        request, product_prefix, settings, &wide);
    copy_reply_v2_to_v1(out, &wide);
    return handled;
}
