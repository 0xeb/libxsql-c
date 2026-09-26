// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// runtime_settings_table.c — canonical transactional runtime_settings table.

#include <libxsqlc/settings.h>

#include <libxsqlc/database.h>
#include <libxsqlc/status.h>
#include <libxsqlc/table_cached.h>
#include <libxsqlc/value.h>
#include <libxsqlc/vtab.h>

#include "internal/internal.h"
#include "internal/settings_internal.h"
#include "internal/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct runtime_row {
    xsql_runtime_setting_entry_v2 entry;
    void *transaction_state;
} runtime_row;

typedef struct runtime_savepoint {
    int savepoint;
    xsqlc_setting_update *updates;
    size_t count;
} runtime_savepoint;

typedef struct runtime_transaction {
    xsqlc_setting_update *updates;
    size_t update_count;
    size_t update_cap;
    runtime_savepoint *savepoints;
    size_t savepoint_count;
    size_t savepoint_cap;
    bool owns_staged_query_timeout;
} runtime_transaction;

struct xsql_runtime_settings_table {
    xsql_settings *settings;
    char product_prefix[64];
    xsql_cached_table *definition;
};

static void clear_savepoints(runtime_transaction *transaction) {
    for (size_t i = 0; i < transaction->savepoint_count; ++i) {
        xsqlc_free(transaction->savepoints[i].updates);
        transaction->savepoints[i].updates = NULL;
        transaction->savepoints[i].count = 0;
    }
    transaction->savepoint_count = 0;
}

static int runtime_state_new(void **state_out, void *user) {
    (void)user;
    if (!state_out) {
        return XSQL_MISUSE;
    }
    *state_out = xsqlc_malloc(sizeof(runtime_transaction));
    if (!*state_out) {
        return XSQL_NO_MEMORY;
    }
    memset(*state_out, 0, sizeof(runtime_transaction));
    return XSQL_OK;
}

static void runtime_state_free(void *state, void *user) {
    runtime_transaction *transaction =
        (runtime_transaction *)state;
    xsql_runtime_settings_table *table =
        (xsql_runtime_settings_table *)user;
    if (!transaction) {
        return;
    }
    if (transaction->owns_staged_query_timeout && table) {
        xsqlc_settings_discard_staged_query_timeout(
            table->settings);
    }
    clear_savepoints(transaction);
    xsqlc_free(transaction->savepoints);
    xsqlc_free(transaction->updates);
    xsqlc_free(transaction);
}

static void finish_transaction(
    runtime_transaction *transaction,
    xsql_runtime_settings_table *table, bool commit) {
    if (!transaction || !table) {
        return;
    }
    if (commit) {
        xsqlc_settings_commit_staged(
            table->settings, transaction->updates,
            transaction->update_count,
            transaction->owns_staged_query_timeout);
    } else if (transaction->owns_staged_query_timeout) {
        xsqlc_settings_discard_staged_query_timeout(
            table->settings);
    }
    transaction->update_count = 0;
    transaction->owns_staged_query_timeout = false;
    clear_savepoints(transaction);
}

static void runtime_commit(void *state, void *user) {
    finish_transaction(
        (runtime_transaction *)state,
        (xsql_runtime_settings_table *)user, true);
}

static void runtime_rollback(void *state, void *user) {
    finish_transaction(
        (runtime_transaction *)state,
        (xsql_runtime_settings_table *)user, false);
}

static bool clone_updates(
    const xsqlc_setting_update *source, size_t count,
    xsqlc_setting_update **target) {
    *target = NULL;
    if (count == 0) {
        return true;
    }
    size_t bytes = 0;
    if (!xsqlc_mul_ok(count, sizeof(**target), &bytes)) {
        return false;
    }
    *target = (xsqlc_setting_update *)xsqlc_malloc(bytes);
    if (!*target) {
        return false;
    }
    memcpy(*target, source, bytes);
    return true;
}

static int runtime_savepoint_hook(
    void *state, int savepoint, void *user) {
    (void)user;
    runtime_transaction *transaction =
        (runtime_transaction *)state;
    if (!transaction) {
        return XSQL_MISUSE;
    }
    xsqlc_setting_update *copy = NULL;
    if (!clone_updates(
            transaction->updates, transaction->update_count,
            &copy)) {
        xsql_vtab_set_error(
            "runtime_settings: out of memory while saving a savepoint");
        return XSQL_NO_MEMORY;
    }
    if (!xsqlc_vec_reserve(
            (void **)&transaction->savepoints,
            &transaction->savepoint_cap,
            transaction->savepoint_count + 1,
            sizeof(*transaction->savepoints))) {
        xsqlc_free(copy);
        xsql_vtab_set_error(
            "runtime_settings: out of memory while saving a savepoint");
        return XSQL_NO_MEMORY;
    }
    runtime_savepoint *snapshot =
        &transaction->savepoints[transaction->savepoint_count++];
    snapshot->savepoint = savepoint;
    snapshot->updates = copy;
    snapshot->count = transaction->update_count;
    return XSQL_OK;
}

static int runtime_release_hook(
    void *state, int savepoint, void *user) {
    (void)user;
    runtime_transaction *transaction =
        (runtime_transaction *)state;
    if (!transaction) {
        return XSQL_MISUSE;
    }
    size_t write = 0;
    for (size_t i = 0;
         i < transaction->savepoint_count; ++i) {
        runtime_savepoint snapshot = transaction->savepoints[i];
        if (snapshot.savepoint >= savepoint) {
            xsqlc_free(snapshot.updates);
        } else {
            transaction->savepoints[write++] = snapshot;
        }
    }
    transaction->savepoint_count = write;
    return XSQL_OK;
}

static bool updates_contain(
    const xsqlc_setting_update *updates, size_t count,
    const char *key) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(updates[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

static int runtime_rollback_to_hook(
    void *state, int savepoint, void *user) {
    runtime_transaction *transaction =
        (runtime_transaction *)state;
    xsql_runtime_settings_table *table =
        (xsql_runtime_settings_table *)user;
    if (!transaction || !table) {
        return XSQL_MISUSE;
    }
    const runtime_savepoint *target = NULL;
    for (size_t i = 0;
         i < transaction->savepoint_count; ++i) {
        if (transaction->savepoints[i].savepoint == savepoint) {
            target = &transaction->savepoints[i];
            break;
        }
    }
    size_t target_count = target ? target->count : 0;
    if (target_count > transaction->update_cap &&
        !xsqlc_vec_reserve(
            (void **)&transaction->updates,
            &transaction->update_cap, target_count,
            sizeof(*transaction->updates))) {
        xsql_vtab_set_error(
            "runtime_settings: out of memory while rolling back "
            "to a savepoint");
        return XSQL_NO_MEMORY;
    }
    bool target_has_timeout =
        target && updates_contain(
                      target->updates, target->count,
                      "query_timeout_ms");
    bool current_has_timeout =
        transaction->owns_staged_query_timeout;
    if (!current_has_timeout && target_has_timeout &&
        !xsqlc_settings_begin_staged_query_timeout(
            table->settings)) {
        xsql_vtab_set_error(
            "runtime_settings: cannot restore staged "
            "query_timeout_ms while the timeout stack is active");
        return XSQL_ERROR;
    }
    if (current_has_timeout && !target_has_timeout) {
        xsqlc_settings_discard_staged_query_timeout(
            table->settings);
    }
    if (target_count != 0) {
        memcpy(transaction->updates, target->updates,
               target_count * sizeof(*transaction->updates));
    }
    transaction->update_count = target_count;
    transaction->owns_staged_query_timeout =
        target_has_timeout;

    size_t write = 0;
    for (size_t i = 0;
         i < transaction->savepoint_count; ++i) {
        runtime_savepoint snapshot = transaction->savepoints[i];
        if (snapshot.savepoint > savepoint) {
            xsqlc_free(snapshot.updates);
        } else {
            transaction->savepoints[write++] = snapshot;
        }
    }
    transaction->savepoint_count = write;
    return XSQL_OK;
}

static size_t runtime_estimate(void *user) {
    xsql_runtime_settings_table *table =
        (xsql_runtime_settings_table *)user;
    return table ? xsql_settings_count(table->settings) : 0;
}

static const xsqlc_setting_update *find_staged(
    const runtime_transaction *transaction, const char *key) {
    if (!transaction) {
        return NULL;
    }
    for (size_t i = 0; i < transaction->update_count; ++i) {
        if (strcmp(transaction->updates[i].key, key) == 0) {
            return &transaction->updates[i];
        }
    }
    return NULL;
}

static void runtime_build(
    xsql_rowset *rows, void *transaction_state, void *user) {
    xsql_runtime_settings_table *table =
        (xsql_runtime_settings_table *)user;
    runtime_transaction *transaction =
        (runtime_transaction *)transaction_state;
    if (!rows || !table || !transaction) {
        xsql_vtab_set_error(
            "runtime_settings: missing connection-local "
            "transaction state");
        return;
    }
    size_t count = xsql_settings_count(table->settings);
    if (count == 0) {
        return;
    }
    size_t bytes = 0;
    if (!xsqlc_mul_ok(
            count, sizeof(xsql_runtime_setting_entry_v2), &bytes)) {
        xsql_vtab_set_error("runtime_settings: out of memory");
        return;
    }
    xsql_runtime_setting_entry_v2 *entries =
        (xsql_runtime_setting_entry_v2 *)xsqlc_malloc(bytes);
    if (!entries) {
        xsql_vtab_set_error("runtime_settings: out of memory");
        return;
    }
    size_t written = xsql_settings_enumerate_v2(
        table->settings, entries, count);
    for (size_t i = 0; i < written; ++i) {
        runtime_row row;
        memset(&row, 0, sizeof(row));
        row.entry = entries[i];
        row.transaction_state = transaction;
        const xsqlc_setting_update *staged =
            find_staged(transaction, row.entry.key);
        if (staged) {
            snprintf(row.entry.value, sizeof(row.entry.value),
                     "%s", staged->value);
        }
        if (!xsql_rowset_push(rows, &row)) {
            xsql_vtab_set_error("runtime_settings: out of memory");
            break;
        }
    }
    xsqlc_free(entries);
}

static void emit_key(
    xsql_context *context, const void *row, void *user) {
    (void)user;
    const runtime_row *runtime = (const runtime_row *)row;
    xsql_result_text_static(context, runtime->entry.key);
}

static void emit_value(
    xsql_context *context, const void *row, void *user) {
    (void)user;
    const runtime_row *runtime = (const runtime_row *)row;
    xsql_result_text_static(context, runtime->entry.value);
}

static void emit_type(
    xsql_context *context, const void *row, void *user) {
    (void)user;
    const runtime_row *runtime = (const runtime_row *)row;
    xsql_result_text_static(context, runtime->entry.type);
}

static void emit_scope(
    xsql_context *context, const void *row, void *user) {
    (void)user;
    const runtime_row *runtime = (const runtime_row *)row;
    xsql_result_text_static(context, runtime->entry.scope);
}

static void emit_kind(
    xsql_context *context, const void *row, void *user) {
    (void)user;
    const runtime_row *runtime = (const runtime_row *)row;
    xsql_result_text_static(
        context,
        strcmp(runtime->entry.scope, "action") == 0 ? "action" : "value");
}

// xsql_cached_col_int takes a value-returning getter, not an emitter.
static int emit_settable(const void *row, void *user) {
    const runtime_row *runtime = (const runtime_row *)row;
    xsql_runtime_settings_table *table =
        (xsql_runtime_settings_table *)user;
    return xsql_settings_is_settable(table->settings, runtime->entry.key) ? 1 : 0;
}

static bool stage_value(
    void *row, xsql_value *value, void *user) {
    runtime_row *runtime = (runtime_row *)row;
    xsql_runtime_settings_table *table =
        (xsql_runtime_settings_table *)user;
    if (xsql_value_is_nochange(value)) {
        return true;
    }
    if (xsql_value_is_null(value)) {
        xsql_vtab_set_error(
            "runtime_settings: '%s' cannot be set to NULL",
            runtime->entry.key);
        return false;
    }
    if (strcmp(runtime->entry.scope, "action") == 0) {
        xsql_vtab_set_error(
            "runtime_settings: '%s' is a PRAGMA verb, "
            "not a settable value",
            runtime->entry.key);
        return false;
    }
    const char *text = xsql_value_text(value);
    xsql_runtime_pragma_reply_v2 normalized;
    if (!xsql_settings_normalize_v2(
            table->settings, runtime->entry.key,
            text ? text : "", table->product_prefix,
            &normalized)) {
        xsql_vtab_set_error(
            "runtime_settings: unknown key '%s'",
            runtime->entry.key);
        return false;
    }
    if (!normalized.success) {
        xsql_vtab_set_error("%s", normalized.error);
        return false;
    }
    runtime_transaction *transaction =
        (runtime_transaction *)runtime->transaction_state;
    if (!transaction) {
        xsql_vtab_set_error(
            "runtime_settings: missing connection-local "
            "transaction state");
        return false;
    }
    for (size_t i = 0; i < transaction->update_count; ++i) {
        if (strcmp(
                transaction->updates[i].key,
                runtime->entry.key) == 0) {
            snprintf(
                transaction->updates[i].value,
                sizeof(transaction->updates[i].value), "%s",
                normalized.value);
            snprintf(runtime->entry.value,
                     sizeof(runtime->entry.value), "%s",
                     normalized.value);
            return true;
        }
    }
    if (!xsqlc_vec_reserve(
            (void **)&transaction->updates,
            &transaction->update_cap,
            transaction->update_count + 1,
            sizeof(*transaction->updates))) {
        xsql_vtab_set_error(
            "runtime_settings: out of memory while staging '%s'",
            runtime->entry.key);
        return false;
    }
    bool first_timeout =
        strcmp(runtime->entry.key, "query_timeout_ms") == 0;
    if (first_timeout &&
        !xsqlc_settings_begin_staged_query_timeout(
            table->settings)) {
        xsql_vtab_set_error(
            "runtime_settings: query_timeout_ms cannot be staged "
            "while the timeout stack is active");
        return false;
    }
    xsqlc_setting_update *update =
        &transaction->updates[transaction->update_count++];
    memset(update, 0, sizeof(*update));
    snprintf(update->key, sizeof(update->key), "%s",
             runtime->entry.key);
    snprintf(update->value, sizeof(update->value), "%s",
             normalized.value);
    if (first_timeout) {
        transaction->owns_staged_query_timeout = true;
    }
    snprintf(runtime->entry.value,
             sizeof(runtime->entry.value), "%s",
             normalized.value);
    return true;
}

xsql_runtime_settings_table *xsql_runtime_settings_table_new(
    xsql_settings *settings, const char *product_prefix) {
    if (!settings || !product_prefix ||
        product_prefix[0] == '\0' ||
        strlen(product_prefix) >=
            sizeof(((xsql_runtime_settings_table *)0)
                       ->product_prefix)) {
        return NULL;
    }
    xsql_runtime_settings_table *table =
        (xsql_runtime_settings_table *)xsqlc_malloc(
            sizeof(*table));
    if (!table) {
        return NULL;
    }
    memset(table, 0, sizeof(*table));
    table->settings = settings;
    snprintf(table->product_prefix,
             sizeof(table->product_prefix), "%s",
             product_prefix);
    table->definition = xsql_cached_table_new(
        "runtime_settings", sizeof(runtime_row));
    if (!table->definition) {
        xsql_runtime_settings_table_free(table);
        return NULL;
    }
    xsql_cached_no_shared_cache(table->definition);
    xsql_cached_estimate_rows(
        table->definition, runtime_estimate, table);
    xsql_cached_stateful_cache_builder(
        table->definition, runtime_build, table);

    xsql_transaction_hooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.struct_size = sizeof(hooks);
    hooks.state_new = runtime_state_new;
    hooks.state_free = runtime_state_free;
    hooks.commit = runtime_commit;
    hooks.rollback = runtime_rollback;
    hooks.savepoint = runtime_savepoint_hook;
    hooks.release = runtime_release_hook;
    hooks.rollback_to = runtime_rollback_to_hook;
    if (xsql_cached_transaction_hooks(
            table->definition, &hooks, table) != XSQL_OK ||
        xsql_cached_col_text(
            table->definition, "key", emit_key, table) != XSQL_OK ||
        xsql_cached_col_text_nullable_rw(
            table->definition, "value", emit_value,
            stage_value, table) != XSQL_OK ||
        xsql_cached_col_text(
            table->definition, "type", emit_type, table) != XSQL_OK ||
        xsql_cached_col_text(
            table->definition, "scope", emit_scope, table) != XSQL_OK ||
        xsql_cached_col_text(
            table->definition, "kind", emit_kind, table) != XSQL_OK ||
        xsql_cached_col_int(
            table->definition, "settable", emit_settable, table) != XSQL_OK) {
        xsql_runtime_settings_table_free(table);
        return NULL;
    }
    return table;
}

void xsql_runtime_settings_table_free(
    xsql_runtime_settings_table *table) {
    if (!table) {
        return;
    }
    xsql_cached_table_free(table->definition);
    xsqlc_free(table);
}

int xsql_runtime_settings_table_register(
    xsql_db *db, xsql_runtime_settings_table *table) {
    if (!db || !table || !table->definition) {
        return XSQL_MISUSE;
    }
    return xsql_cached_table_register(db, table->definition);
}
