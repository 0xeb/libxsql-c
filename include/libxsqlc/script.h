// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// script.h — multi-statement SQL orchestration: the statement splitter, the
// canonical script envelope (xsql_script_result), the generic and database-backed
// runners, plus SQL export and identifier quoting. Render an envelope with the
// helpers in <libxsqlc/format.h>.

#ifndef XSQLC_SCRIPT_H
#define XSQLC_SCRIPT_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>

#include <stdbool.h>
#include <stddef.h>

// Forward-declared so this header never pulls in <libxsqlc/database.h>.
struct xsql_db;

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles. `xsql_script_result` is the whole envelope (release with
// xsql_script_result_free()); `xsql_script_statement` is one statement's result
// (borrowed — valid only while its owning result lives).
typedef struct xsql_script_result xsql_script_result;
typedef struct xsql_script_statement xsql_script_statement;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

// Caller-filled script options. Designed for designated initializers; an all-zero
// struct (or a NULL pointer) selects defaults (fail-fast, no echoed SQL).
// `struct_size` lets the ABI grow compatibly.
typedef struct xsql_script_opts {
    size_t struct_size;       // sizeof(xsql_script_opts), or 0 for all defaults
    bool   continue_on_error; // run every statement instead of stopping at the first error
    bool   include_sql;       // echo each statement's SQL into the envelope
    // ABI-growth: optional cooperative-cancellation predicate (the C-native closure
    // idiom for the C++ ScriptOptions::should_cancel std::function). When set and it
    // returns nonzero, the runner checks it BETWEEN statements (it does not start a
    // new statement once cancel is requested) and, on the database-backed runner,
    // threads it into each statement's query so an in-flight one stops ASAP.
    // Read-only result statements may return a partial prefix; mutations return
    // an error. Cancel is not a timeout. It works even when timeout_ms == 0,
    // letting a server abort a
    // runaway script (POST /cancel or a client disconnect). Read only when
    // struct_size covers it. NULL => never cancelled.
    int  (*should_cancel)(void *user);
    void  *should_cancel_user;
    // ABI-growth field (read only when struct_size covers it): per-statement query
    // deadline in ms (0 = no timeout). This field deliberately follows the
    // pointer pair so it starts beyond the complete legacy structure, including
    // its x64 tail padding. A statement that trips the deadline comes back with
    // xsql_script_statement timed_out/partial set and a warning.
    int    timeout_ms;
} xsql_script_opts;

// ---------------------------------------------------------------------------
// Statement splitter
// ---------------------------------------------------------------------------

// Split a SQL script into individual statements (driven by sqlite3_complete). On
// success *out receives a heap array of *count NUL-terminated statement strings —
// each retains the trailing ';' that completed it — released as a unit with
// xsql_free_strings(). Whitespace/comment-only input yields count 0 and a NULL
// array. Always succeeds; returns XSQL_MISUSE on a
// NULL out/count and XSQL_NO_MEMORY on allocation failure.
XSQL_API int xsql_script_split(const char *script, char ***out, size_t *count);

// Release an array returned by xsql_script_split (frees each string and the
// array). Passing NULL is a no-op.
XSQL_API void xsql_free_strings(char **strings, size_t count);

// ---------------------------------------------------------------------------
// Runners
// ---------------------------------------------------------------------------

// Per-statement executor for the generic runner. Receives one statement's SQL and
// an output handle to describe its result through the xsql_stmt_out_* setters
// below (success defaults to false until set). `user` is forwarded verbatim.
typedef void (*xsql_script_executor_fn)(const char *sql,
                                        xsql_script_statement *out, void *user);

// Executor-side setters (valid only inside an xsql_script_executor_fn call).
XSQL_API void xsql_stmt_out_set_success(xsql_script_statement *out, bool success);
XSQL_API void xsql_stmt_out_set_error(xsql_script_statement *out, const char *error);
XSQL_API void xsql_stmt_out_set_elapsed_ms(xsql_script_statement *out, double ms);
XSQL_API void xsql_stmt_out_set_timed_out(xsql_script_statement *out, bool timed_out);
XSQL_API void xsql_stmt_out_set_partial(xsql_script_statement *out, bool partial);
// Copy one non-fatal warning into the statement. The caller retains ownership
// of `warning`; returns XSQL_OK, XSQL_MISUSE, or XSQL_NO_MEMORY.
XSQL_API int  xsql_stmt_out_add_warning(xsql_script_statement *out,
                                        const char *warning);
XSQL_API int  xsql_stmt_out_add_column(xsql_script_statement *out, const char *name);
// Append one row of `ncells` text cells. `nulls` (may be NULL) is a per-cell
// SQL-NULL mask covering the first `nnulls` cells (nonzero => the cell is SQL
// NULL); cells past `nnulls` fall back to the legacy "NULL"-sentinel rule. Returns
// XSQL_OK or XSQL_NO_MEMORY.
XSQL_API int  xsql_stmt_out_add_row(xsql_script_statement *out,
                                    const char *const *cells, size_t ncells,
                                    const unsigned char *nulls, size_t nnulls);

// Generic multi-statement runner: split `script`, invoke `exec` per statement, and
// aggregate into *out (release with xsql_script_result_free()). `opts` may be
// NULL. Returns XSQL_OK when every statement succeeded, XSQL_ERROR when any failed
// or the split failed (the envelope still carries the details), XSQL_MISUSE on a
// NULL out, XSQL_NO_MEMORY on allocation failure.
XSQL_API int xsql_run_script(const char *script, xsql_script_executor_fn exec,
                             void *user, const xsql_script_opts *opts,
                             xsql_script_result **out);

// Database-backed canonical runner: each statement is executed via xsql_db_query,
// populating columns/rows/timing and the per-cell SQL-NULL mask. Same return
// contract as xsql_run_script.
XSQL_API int xsql_db_run_script(struct xsql_db *db, const char *script,
                                const xsql_script_opts *opts,
                                xsql_script_result **out);

// Convenience alias for xsql_db_run_script (the "execute a script's result sets"
// spelling). Identical behavior and return contract.
XSQL_API int xsql_execute_script(struct xsql_db *db, const char *script,
                                 const xsql_script_opts *opts,
                                 xsql_script_result **out);

// Row-at-a-time output sink. Return false to stop immediately (for example,
// when an HTTP client disconnects). The byte span is borrowed for the call.
typedef bool (*xsql_stream_sink_fn)(const char *data, size_t size, void *user);

// Stream the canonical script envelope as JSON without materializing read-only
// result rows. Peak library memory is O(one row) for reads. Mutation RETURNING
// rows are buffered until the statement succeeds, because rows from a failed
// or interrupted mutation are provisional and must never reach the sink. The
// envelope is structurally equivalent to xsql_script_result_to_json(); summary
// fields follow the rows because they are not known earlier.
//
// The NDJSON variant emits one keyed object per row and a metadata/error record
// when needed. Duplicate column names are made unique with #2, #3, ... suffixes.
//
// Both honor timeout/cancellation options between rows and return XSQL_ABORT
// when sink returns false. SQL errors are represented in the emitted stream and
// return XSQL_OK; misuse/allocation errors are returned directly.
XSQL_API int xsql_db_stream_script_json(struct xsql_db *db,
                                        const char *script,
                                        const xsql_script_opts *opts,
                                        xsql_stream_sink_fn sink, void *user);
XSQL_API int xsql_db_stream_script_ndjson(struct xsql_db *db,
                                          const char *script,
                                          const xsql_script_opts *opts,
                                          xsql_stream_sink_fn sink, void *user);

// ---------------------------------------------------------------------------
// Envelope accessors (all borrowed; valid while the result lives)
// ---------------------------------------------------------------------------

XSQL_API bool   xsql_script_result_ok(const xsql_script_result *r);
XSQL_API size_t xsql_script_result_statement_count(const xsql_script_result *r);
XSQL_API size_t xsql_script_result_count(const xsql_script_result *r);  // statements present
XSQL_API size_t xsql_script_result_row_count_total(const xsql_script_result *r);
XSQL_API double xsql_script_result_elapsed_ms_total(const xsql_script_result *r);
XSQL_API bool   xsql_script_result_has_first_error(const xsql_script_result *r);
XSQL_API size_t xsql_script_result_first_error_index(const xsql_script_result *r);
// Parse/split error text (borrowed; "" when none).
XSQL_API const char *xsql_script_result_parse_error(const xsql_script_result *r);

// Statement `i` (0-based), or NULL when out of range.
XSQL_API const xsql_script_statement *
xsql_script_result_statement(const xsql_script_result *r, size_t i);

XSQL_API size_t xsql_script_statement_index(const xsql_script_statement *s);
XSQL_API bool   xsql_script_statement_ok(const xsql_script_statement *s);
XSQL_API const char *xsql_script_statement_error(const xsql_script_statement *s);  // "" if none
XSQL_API const char *xsql_script_statement_sql(const xsql_script_statement *s);    // "" if not echoed
XSQL_API double xsql_script_statement_elapsed_ms(const xsql_script_statement *s);
XSQL_API size_t xsql_script_statement_row_count(const xsql_script_statement *s);
// Timeout signalling (see xsql_script_opts::timeout_ms): timed_out is true if the
// statement hit its deadline; partial is true if a timeout kept the rows gathered
// so far. `warnings` are non-fatal notices (e.g. the partial-rows timeout warning).
XSQL_API bool   xsql_script_statement_timed_out(const xsql_script_statement *s);
XSQL_API bool   xsql_script_statement_partial(const xsql_script_statement *s);
XSQL_API size_t xsql_script_statement_warning_count(const xsql_script_statement *s);
XSQL_API const char *xsql_script_statement_warning(const xsql_script_statement *s,
                                                   size_t i);  // NULL if out of range
XSQL_API size_t xsql_script_statement_column_count(const xsql_script_statement *s);
XSQL_API const char *xsql_script_statement_column_name(const xsql_script_statement *s,
                                                       size_t col);
// Cell text (borrowed, NUL-terminated; "" for a SQL NULL). NULL if out of range.
XSQL_API const char *xsql_script_statement_cell(const xsql_script_statement *s,
                                                size_t row, size_t col);
XSQL_API bool xsql_script_statement_cell_is_null(const xsql_script_statement *s,
                                                 size_t row, size_t col);

// Rebuild an envelope from the canonical JSON produced by
// xsql_script_result_to_json (the inverse). Tolerant of missing/extra fields; on
// unparseable input *out still receives a valid failed result carrying a
// parse_error. Returns XSQL_OK / XSQL_MISUSE / XSQL_NO_MEMORY.
XSQL_API int xsql_script_result_from_json(const char *json,
                                          xsql_script_result **out);

// Release an envelope. Passing NULL is a no-op.
XSQL_API void xsql_script_result_free(xsql_script_result *r);

// ---------------------------------------------------------------------------
// SQL export / identifier quoting
// ---------------------------------------------------------------------------

// Dump `count` tables (or, when names is NULL/count 0, every user table ordered by
// name) as a self-contained SQL script written to `path`, including the %.16g REAL
// formatting. Returns XSQL_OK on
// success, else an error status (the db's error text via xsql_db_errmsg).
XSQL_API int xsql_export_tables(struct xsql_db *db, const char *const *names,
                                size_t count, const char *path);

// Quote `name` as a SQL identifier (wrap in double quotes, doubling embedded ").
// Returns heap text (free with xsql_free()); NULL only on allocation failure.
XSQL_API char *xsql_quote_identifier(const char *name);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_SCRIPT_H
