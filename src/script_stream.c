// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <libxsqlc/script.h>

#include <libxsqlc/database.h>
#include <libxsqlc/vtab.h>

#include "internal/internal.h"
#include "internal/json.h"
#include "internal/strbuf.h"
#include "internal/vtab_core.h"

#include <sqlite3.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

typedef struct stream_interrupt {
    long long started_ms;
    int timeout_ms;
    int (*cancel)(void *user);
    void *cancel_user;
    bool timed_out;
    bool cancelled;
} stream_interrupt;

typedef struct stream_output {
    xsql_stream_sink_fn sink;
    void *user;
    bool aborted;
} stream_output;

static long long stream_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0) {
        return 0;
    }
    QueryPerformanceCounter(&counter);
    long long whole = counter.QuadPart / frequency.QuadPart;
    long long fraction = counter.QuadPart % frequency.QuadPart;
    return whole * 1000 +
           (fraction * 1000) / frequency.QuadPart;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
#endif
}

static bool stream_poll(stream_interrupt *interrupt) {
    if (interrupt->cancelled || interrupt->timed_out) {
        return true;
    }
    if (interrupt->cancel &&
        interrupt->cancel(interrupt->cancel_user)) {
        interrupt->cancelled = true;
        return true;
    }
    if (interrupt->timeout_ms > 0 &&
        stream_now_ms() - interrupt->started_ms >=
            interrupt->timeout_ms) {
        interrupt->timed_out = true;
        return true;
    }
    return false;
}

static int stream_progress(void *user) {
    return stream_poll((stream_interrupt *)user) ? 1 : 0;
}

static bool stream_vtab_interrupt(void *user) {
    return stream_poll((stream_interrupt *)user);
}

static int emit_bytes(stream_output *output, const char *data, size_t size) {
    if (output->aborted) {
        return XSQL_ABORT;
    }
    if (size != 0 && !output->sink(data, size, output->user)) {
        output->aborted = true;
        return XSQL_ABORT;
    }
    return XSQL_OK;
}

static int emit_buffer(stream_output *output, xsqlc_strbuf *buffer) {
    if (xsqlc_sb_oom(buffer)) {
        return XSQL_NO_MEMORY;
    }
    return emit_bytes(output, xsqlc_sb_cstr(buffer), buffer->len);
}

static void append_sqlite_cell(xsqlc_strbuf *buffer, sqlite3_stmt *statement,
                               int column) {
    int type = sqlite3_column_type(statement, column);
    if (type == SQLITE_NULL) {
        xsqlc_sb_append(buffer, "null");
        return;
    }
    const void *value;
    if (type == SQLITE_BLOB) {
        value = sqlite3_column_blob(statement, column);
    } else {
        value = sqlite3_column_text(statement, column);
    }
    int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes < 0) {
        value = "";
        bytes = 0;
    }
    xsqlc_json_append_string(
        buffer, (const char *)value, (size_t)bytes);
}

static int option_timeout(const xsql_script_opts *options) {
    if (!options ||
        options->struct_size <
            offsetof(xsql_script_opts, timeout_ms) +
                sizeof(options->timeout_ms)) {
        return 0;
    }
    return options->timeout_ms > 0 ? options->timeout_ms : 0;
}

static int (*option_cancel(const xsql_script_opts *options))(void *) {
    if (!options ||
        options->struct_size <
            offsetof(xsql_script_opts, should_cancel_user) +
                sizeof(options->should_cancel_user)) {
        return NULL;
    }
    return options->should_cancel;
}

static void *option_cancel_user(const xsql_script_opts *options) {
    if (!options ||
        options->struct_size <
            offsetof(xsql_script_opts, should_cancel_user) +
                sizeof(options->should_cancel_user)) {
        return NULL;
    }
    return options->should_cancel_user;
}

static bool option_continue(const xsql_script_opts *options) {
    return options && options->continue_on_error;
}

static bool option_include_sql(const xsql_script_opts *options) {
    return options && options->include_sql;
}

static void append_statement_error(
    xsqlc_strbuf *buffer, size_t index, const char *sql,
    bool include_sql, const char *message, long long elapsed_ms,
    bool timed_out, bool ndjson) {
    xsqlc_sb_append(buffer, "{\"statement_index\":");
    xsqlc_sb_append_size(buffer, index);
    if (!ndjson && include_sql) {
        xsqlc_sb_append(buffer, ",\"sql\":");
        xsqlc_json_append_string(buffer, sql, strlen(sql));
    }
    if (!ndjson) {
        xsqlc_sb_append(
            buffer,
            ",\"columns\":[],\"rows\":[],\"row_count\":0,\"elapsed_ms\":");
        xsqlc_sb_append_i64(buffer, elapsed_ms);
        xsqlc_sb_append(buffer, ",\"success\":false");
    }
    xsqlc_sb_append(buffer, ",\"error\":");
    xsqlc_json_append_string(
        buffer, message ? message : "", strlen(message ? message : ""));
    if (timed_out) {
        xsqlc_sb_append(buffer, ",\"timed_out\":true");
    }
    xsqlc_sb_append(buffer, ndjson ? "}\n" : "}");
}

static char **make_unique_keys(sqlite3_stmt *statement, int count) {
    if (count <= 0) {
        return NULL;
    }
    char **keys = (char **)xsqlc_malloc((size_t)count * sizeof(*keys));
    if (!keys) {
        return NULL;
    }
    memset(keys, 0, (size_t)count * sizeof(*keys));
    for (int column = 0; column < count; ++column) {
        const char *base = sqlite3_column_name(statement, column);
        if (!base) {
            base = "";
        }
        size_t suffix = 1;
        for (;;) {
            xsqlc_strbuf candidate;
            xsqlc_sb_init(&candidate);
            xsqlc_sb_append(&candidate, base);
            if (suffix > 1) {
                xsqlc_sb_putc(&candidate, '#');
                xsqlc_sb_append_size(&candidate, suffix);
            }
            char *value = xsqlc_sb_detach(&candidate);
            if (!value) {
                for (int i = 0; i < column; ++i) {
                    xsqlc_free(keys[i]);
                }
                xsqlc_free(keys);
                return NULL;
            }
            bool used = false;
            for (int prior = 0; prior < column; ++prior) {
                if (strcmp(keys[prior], value) == 0) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                keys[column] = value;
                break;
            }
            xsqlc_free(value);
            ++suffix;
        }
    }
    return keys;
}

static void free_keys(char **keys, int count) {
    if (!keys) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        xsqlc_free(keys[i]);
    }
    xsqlc_free(keys);
}

static int stream_script(xsql_db *db, const char *script,
                         const xsql_script_opts *options,
                         xsql_stream_sink_fn sink, void *user,
                         bool ndjson) {
    if (!db || !db->db || !script || !sink) {
        return XSQL_MISUSE;
    }
    char **statements = NULL;
    size_t statement_count = 0;
    int status =
        xsql_script_split(script, &statements, &statement_count);
    if (status != XSQL_OK) {
        return status;
    }

    stream_output output = {sink, user, false};
    if (!ndjson) {
        xsqlc_strbuf header;
        xsqlc_sb_init(&header);
        xsqlc_sb_append(&header, "{\"statement_count\":");
        xsqlc_sb_append_size(&header, statement_count);
        xsqlc_sb_append(&header, ",\"results\":[");
        status = emit_buffer(&output, &header);
        xsqlc_sb_free(&header);
        if (status != XSQL_OK) {
            xsql_free_strings(statements, statement_count);
            return status;
        }
    }

    size_t emitted_statements = 0;
    size_t row_count_total = 0;
    long long elapsed_total = 0;
    bool has_error = false;
    size_t first_error = 0;
    const bool include_sql = option_include_sql(options);
    bool request_cancelled = false;

    for (size_t index = 0; index < statement_count; ++index) {
        const char *sql = statements[index];
        long long started_ms = stream_now_ms();
        stream_interrupt interrupt = {
            started_ms, option_timeout(options),
            option_cancel(options), option_cancel_user(options),
            false, false};
        xsqlc_progress_guard progress_guard = {0};
        xsqlc_interrupt_guard interrupt_guard = {0};
        const bool guard_enabled =
            interrupt.timeout_ms > 0 || interrupt.cancel;
        if (guard_enabled) {
            xsqlc_progress_guard_enter(
                &progress_guard, db->db, 1000, stream_progress, &interrupt);
            xsqlc_interrupt_guard_enter(
                &interrupt_guard, stream_vtab_interrupt, &interrupt);
        }

        // Cancellation is request-terminal. Poll before prepare so a cancelled
        // script cannot run DDL/DML or planner callbacks for the next statement.
        if (request_cancelled || stream_poll(&interrupt)) {
            interrupt.cancelled = true;
            request_cancelled = true;
            if (guard_enabled) {
                xsqlc_interrupt_guard_leave(&interrupt_guard);
                xsqlc_progress_guard_leave(&progress_guard);
            }
            long long elapsed = stream_now_ms() - started_ms;
            if (elapsed < 0) elapsed = 0;
            xsqlc_strbuf error;
            xsqlc_sb_init(&error);
            if (!ndjson && emitted_statements != 0) {
                xsqlc_sb_putc(&error, ',');
            }
            append_statement_error(
                &error, index, sql, include_sql, "query cancelled",
                elapsed, false, ndjson);
            status = emit_buffer(&output, &error);
            xsqlc_sb_free(&error);
            if (status == XSQL_OK) {
                ++emitted_statements;
                elapsed_total += elapsed;
                if (!has_error) {
                    has_error = true;
                    first_error = index;
                }
            }
            break;
        }

        sqlite3_stmt *statement = NULL;
        int sqlite_status =
            sqlite3_prepare_v2(db->db, sql, -1, &statement, NULL);
        if (sqlite_status != SQLITE_OK) {
            if (guard_enabled) {
                xsqlc_interrupt_guard_leave(&interrupt_guard);
                xsqlc_progress_guard_leave(&progress_guard);
            }
            long long elapsed = stream_now_ms() - started_ms;
            if (elapsed < 0) elapsed = 0;
            const char *prepare_error = sqlite3_errmsg(db->db);
            if (interrupt.cancelled) {
                prepare_error = "Query cancelled";
                request_cancelled = true;
            } else if (interrupt.timed_out) {
                prepare_error = "Query timed out";
            }
            xsqlc_strbuf error;
            xsqlc_sb_init(&error);
            if (!ndjson && emitted_statements != 0) {
                xsqlc_sb_putc(&error, ',');
            }
            append_statement_error(
                &error, index, sql, include_sql,
                prepare_error, elapsed, interrupt.timed_out, ndjson);
            status = emit_buffer(&output, &error);
            xsqlc_sb_free(&error);
            sqlite3_finalize(statement);
            if (status != XSQL_OK) {
                break;
            }
            ++emitted_statements;
            elapsed_total += elapsed;
            if (!has_error) {
                has_error = true;
                first_error = index;
            }
            if (request_cancelled || !option_continue(options)) {
                break;
            }
            continue;
        }

        int columns = sqlite3_column_count(statement);
        const bool readonly = sqlite3_stmt_readonly(statement) != 0;
        const bool partial_capable = columns > 0 && readonly;
        const bool defer_rows = columns > 0 && !readonly;
        char **keys = ndjson ? make_unique_keys(statement, columns) : NULL;
        if (ndjson && columns > 0 && !keys) {
            if (guard_enabled) {
                xsqlc_interrupt_guard_leave(&interrupt_guard);
                xsqlc_progress_guard_leave(&progress_guard);
            }
            sqlite3_finalize(statement);
            status = XSQL_NO_MEMORY;
            break;
        }
        xsqlc_strbuf deferred_rows;
        xsqlc_sb_init(&deferred_rows);

        if (!ndjson) {
            xsqlc_strbuf head;
            xsqlc_sb_init(&head);
            if (emitted_statements != 0) {
                xsqlc_sb_putc(&head, ',');
            }
            xsqlc_sb_append(&head, "{\"statement_index\":");
            xsqlc_sb_append_size(&head, index);
            if (include_sql) {
                xsqlc_sb_append(&head, ",\"sql\":");
                xsqlc_json_append_string(&head, sql, strlen(sql));
            }
            xsqlc_sb_append(&head, ",\"columns\":[");
            for (int column = 0; column < columns; ++column) {
                if (column != 0) {
                    xsqlc_sb_putc(&head, ',');
                }
                const char *name = sqlite3_column_name(statement, column);
                xsqlc_json_append_string(
                    &head, name ? name : "", strlen(name ? name : ""));
            }
            xsqlc_sb_append(&head, "],\"rows\":[");
            status = emit_buffer(&output, &head);
            xsqlc_sb_free(&head);
            if (status != XSQL_OK) {
                if (guard_enabled) {
                    xsqlc_interrupt_guard_leave(&interrupt_guard);
                    xsqlc_progress_guard_leave(&progress_guard);
                }
                free_keys(keys, columns);
                xsqlc_sb_free(&deferred_rows);
                sqlite3_finalize(statement);
                break;
            }
        }

        size_t rows = 0;
        bool failed = false;
        const char *error_message = NULL;
        for (;;) {
            if (stream_poll(&interrupt)) {
                break;
            }
            sqlite_status = sqlite3_step(statement);
            if (sqlite_status == SQLITE_ROW) {
                xsqlc_strbuf row;
                xsqlc_sb_init(&row);
                if (ndjson) {
                    xsqlc_sb_putc(&row, '{');
                    for (int column = 0; column < columns; ++column) {
                        if (column != 0) {
                            xsqlc_sb_putc(&row, ',');
                        }
                        xsqlc_json_append_string(
                            &row, keys[column], strlen(keys[column]));
                        xsqlc_sb_putc(&row, ':');
                        append_sqlite_cell(&row, statement, column);
                    }
                    xsqlc_sb_append(&row, "}\n");
                } else {
                    if (rows != 0) {
                        xsqlc_sb_putc(&row, ',');
                    }
                    xsqlc_sb_putc(&row, '[');
                    for (int column = 0; column < columns; ++column) {
                        if (column != 0) {
                            xsqlc_sb_putc(&row, ',');
                        }
                        append_sqlite_cell(&row, statement, column);
                    }
                    xsqlc_sb_putc(&row, ']');
                }
                if (xsqlc_sb_oom(&row)) {
                    status = XSQL_NO_MEMORY;
                } else if (defer_rows) {
                    xsqlc_sb_append_len(
                        &deferred_rows, xsqlc_sb_cstr(&row), row.len);
                    if (xsqlc_sb_oom(&deferred_rows)) {
                        status = XSQL_NO_MEMORY;
                    }
                } else {
                    status = emit_buffer(&output, &row);
                }
                xsqlc_sb_free(&row);
                if (status != XSQL_OK) {
                    break;
                }
                ++rows;
                if (stream_poll(&interrupt)) {
                    break;
                }
            } else if (sqlite_status == SQLITE_DONE) {
                break;
            } else {
                // The pre-step/progress/vtable checks latch timeout/cancel before
                // returning an interrupt. A fresh poll here can mislabel an
                // unrelated SQLite error after the statement has stopped.
                if (!interrupt.timed_out && !interrupt.cancelled) {
                    failed = true;
                    error_message = sqlite3_errmsg(db->db);
                }
                break;
            }
        }
        if (guard_enabled) {
            xsqlc_interrupt_guard_leave(&interrupt_guard);
            xsqlc_progress_guard_leave(&progress_guard);
        }
        if (status != XSQL_OK) {
            xsqlc_sb_free(&deferred_rows);
            free_keys(keys, columns);
            sqlite3_finalize(statement);
            break;
        }

        if (interrupt.cancelled) {
            request_cancelled = true;
        }
        if (interrupt.timed_out && !partial_capable) {
            failed = true;
            error_message = "Query timed out";
        } else if (interrupt.cancelled && !partial_capable) {
            failed = true;
            error_message = "Query cancelled";
        }
        if (defer_rows) {
            if (failed) {
                rows = 0;
            } else {
                status = emit_buffer(&output, &deferred_rows);
            }
        }
        xsqlc_sb_free(&deferred_rows);
        if (status != XSQL_OK) {
            free_keys(keys, columns);
            sqlite3_finalize(statement);
            break;
        }
        long long elapsed = stream_now_ms() - started_ms;
        if (elapsed < 0) {
            elapsed = 0;
        }

        xsqlc_strbuf tail;
        xsqlc_sb_init(&tail);
        if (ndjson) {
            if (failed) {
                xsqlc_sb_append(&tail, "{\"statement_index\":");
                xsqlc_sb_append_size(&tail, index);
                xsqlc_sb_append(&tail, ",\"error\":");
                xsqlc_json_append_string(
                    &tail, error_message ? error_message : "",
                    strlen(error_message ? error_message : ""));
                if (interrupt.timed_out) {
                    xsqlc_sb_append(&tail, ",\"timed_out\":true");
                }
                if (partial_capable && !interrupt.timed_out &&
                    !interrupt.cancelled) {
                    xsqlc_sb_append(
                        &tail, ",\"partial\":true,\"warnings\":[");
                    const char *warning =
                        "query failed; returning partial rows";
                    xsqlc_json_append_string(
                        &tail, warning, strlen(warning));
                    xsqlc_sb_putc(&tail, ']');
                }
                xsqlc_sb_append(&tail, "}\n");
            } else if (
                partial_capable &&
                (interrupt.timed_out || interrupt.cancelled)) {
                xsqlc_sb_append(&tail, "{\"statement_index\":");
                xsqlc_sb_append_size(&tail, index);
                if (interrupt.timed_out) {
                    xsqlc_sb_append(&tail, ",\"timed_out\":true");
                }
                xsqlc_sb_append(
                    &tail, ",\"partial\":true,\"warnings\":[");
                const char *warning = interrupt.timed_out
                    ? "query timed out; returning partial rows"
                    : "query cancelled; returning partial rows";
                xsqlc_json_append_string(
                    &tail, warning, strlen(warning));
                xsqlc_sb_append(&tail, "]}\n");
            }
        } else {
            xsqlc_sb_append(&tail, "],\"row_count\":");
            xsqlc_sb_append_size(&tail, rows);
            xsqlc_sb_append(&tail, ",\"elapsed_ms\":");
            xsqlc_sb_append_i64(&tail, elapsed);
            xsqlc_sb_append(&tail, ",\"success\":");
            xsqlc_sb_append(&tail, failed ? "false" : "true");
            xsqlc_sb_append(&tail, ",\"error\":");
            if (failed) {
                xsqlc_json_append_string(
                    &tail, error_message ? error_message : "",
                    strlen(error_message ? error_message : ""));
            } else {
                xsqlc_sb_append(&tail, "null");
            }
            if (interrupt.timed_out) {
                xsqlc_sb_append(&tail, ",\"timed_out\":true");
            }
            if (!failed && partial_capable &&
                (interrupt.timed_out || interrupt.cancelled)) {
                xsqlc_sb_append(
                    &tail, ",\"partial\":true,\"warnings\":[");
                const char *warning = interrupt.timed_out
                    ? "query timed out; returning partial rows"
                    : "query cancelled; returning partial rows";
                xsqlc_json_append_string(
                    &tail, warning, strlen(warning));
                xsqlc_sb_putc(&tail, ']');
            } else if (
                failed && partial_capable && !interrupt.timed_out &&
                !interrupt.cancelled) {
                xsqlc_sb_append(
                    &tail, ",\"partial\":true,\"warnings\":[");
                const char *warning =
                    "query failed; returning partial rows";
                xsqlc_json_append_string(
                    &tail, warning, strlen(warning));
                xsqlc_sb_putc(&tail, ']');
            }
            xsqlc_sb_putc(&tail, '}');
        }
        status = emit_buffer(&output, &tail);
        xsqlc_sb_free(&tail);
        free_keys(keys, columns);
        sqlite3_finalize(statement);
        if (status != XSQL_OK) {
            break;
        }

        ++emitted_statements;
        row_count_total += rows;
        elapsed_total += elapsed;
        if (failed) {
            if (!has_error) {
                has_error = true;
                first_error = index;
            }
            if (request_cancelled || !option_continue(options)) {
                break;
            }
        }
    }

    if (status == XSQL_OK && !ndjson) {
        xsqlc_strbuf footer;
        xsqlc_sb_init(&footer);
        xsqlc_sb_append(&footer, "],\"row_count_total\":");
        xsqlc_sb_append_size(&footer, row_count_total);
        xsqlc_sb_append(&footer, ",\"elapsed_ms_total\":");
        xsqlc_sb_append_i64(&footer, elapsed_total);
        xsqlc_sb_append(&footer, ",\"first_error_index\":");
        if (has_error) {
            xsqlc_sb_append_size(&footer, first_error);
        } else {
            xsqlc_sb_append(&footer, "null");
        }
        xsqlc_sb_append(&footer, ",\"success\":");
        xsqlc_sb_append(&footer, has_error ? "false}" : "true}");
        status = emit_buffer(&output, &footer);
        xsqlc_sb_free(&footer);
    }

    xsql_free_strings(statements, statement_count);
    return status;
}

int xsql_db_stream_script_json(xsql_db *db, const char *script,
                               const xsql_script_opts *options,
                               xsql_stream_sink_fn sink, void *user) {
    return stream_script(db, script, options, sink, user, false);
}

int xsql_db_stream_script_ndjson(xsql_db *db, const char *script,
                                 const xsql_script_opts *options,
                                 xsql_stream_sink_fn sink, void *user) {
    return stream_script(db, script, options, sink, user, true);
}
