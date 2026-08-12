// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// result.c — the materialized query envelope. A whole result set is gathered
// into memory: column names, a row-major flat array of NUL-terminated cell
// buffers (length-preserving, so BLOBs and embedded NULs survive), a parallel
// per-cell SQL-NULL flag array, plus error/warnings/timing metadata.
//
// Errors are reported through the result (error text), never thrown: every
// failure path still hands back a valid, freeable result
// object. The xsql_query_opts timeout fields drive a real deadline here (Phase
// 7): a SQLite progress handler aborts VDBE work once the deadline passes, and a
// thread-local interrupt checker lets long virtual-table loops bail at the same
// deadline. A read-only result statement keeps the rows gathered so far
// (partial); a mutation, including DML with RETURNING, reports a hard error
// because SQLite rolls an interrupted statement back.

// Monotonic deadline math uses clock_gettime(CLOCK_MONOTONIC) on POSIX, which is
// exposed only when _POSIX_C_SOURCE is defined under strict -std=c11; Windows
// uses QueryPerformanceCounter instead. This must precede every #include.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <libxsqlc/result.h>

#include <libxsqlc/database.h>
#include <libxsqlc/status.h>
#include <libxsqlc/vtab.h>

#include "internal/internal.h"
#include "internal/vtab_core.h"

#include <sqlite3.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

struct xsql_result {
    char          **columns;     // ncols names (xsqlc heap)
    size_t          ncols;

    // Row-major cells: nrows*ncols entries. `cells[i]` is a NUL-terminated
    // xsqlc buffer of `cell_bytes[i]` bytes (+1 for the terminator); a SQL NULL
    // is stored as "" with cell_null[i]==1.
    char          **cells;
    int            *cell_bytes;
    unsigned char  *cell_null;
    size_t          nrows;
    size_t          rows_cap;    // rows worth of capacity in the arrays above

    char          **warnings;    // nwarn entries (xsqlc heap)
    size_t          nwarn;

    char           *error;       // NULL/"" means ok
    bool            timed_out;
    bool            partial;
    int             elapsed_ms;
};

// ---------------------------------------------------------------------------
// Build helpers
// ---------------------------------------------------------------------------

// Grow the three parallel cell arrays to hold at least one more row. Returns
// false on allocation failure (leaving the existing arrays intact).
static bool result_reserve_row(xsql_result *r) {
    if (r->nrows < r->rows_cap) {
        return true;
    }
    size_t new_cap = r->rows_cap ? r->rows_cap * 2 : 8;

    // ncols == 0 (a rows-but-no-columns statement) still needs the row counter
    // to advance; there are no per-cell arrays to grow in that case.
    if (r->ncols == 0) {
        r->rows_cap = new_cap;
        return true;
    }

    // n = new_cap * ncols cells; guard both the cell-count product and each
    // per-array byte multiply so a huge cap/ncols cannot under-allocate.
    size_t n;
    if (!xsqlc_mul_ok(new_cap, r->ncols, &n)) {
        return false;
    }
    size_t cell_bytes, byte_bytes, null_bytes;
    if (!xsqlc_mul_ok(n, sizeof(char *), &cell_bytes) ||
        !xsqlc_mul_ok(n, sizeof(int), &byte_bytes) ||
        !xsqlc_mul_ok(n, sizeof(unsigned char), &null_bytes)) {
        return false;
    }

    char **cells = (char **)xsqlc_realloc(r->cells, cell_bytes);
    if (!cells) {
        return false;
    }
    r->cells = cells;

    int *bytes = (int *)xsqlc_realloc(r->cell_bytes, byte_bytes);
    if (!bytes) {
        return false;
    }
    r->cell_bytes = bytes;

    unsigned char *nulls =
        (unsigned char *)xsqlc_realloc(r->cell_null, null_bytes);
    if (!nulls) {
        return false;
    }
    r->cell_null = nulls;

    r->rows_cap = new_cap;
    return true;
}

// Append one stepped row's cells. Returns false on allocation failure.
static bool result_append_row(xsql_result *r, sqlite3_stmt *stmt) {
    if (!result_reserve_row(r)) {
        return false;
    }
    size_t base = r->nrows * r->ncols;
    for (size_t c = 0; c < r->ncols; ++c) {
        char *cell;
        int bytes;
        unsigned char is_null;
        int coltype = sqlite3_column_type(stmt, (int)c);
        if (coltype == SQLITE_NULL) {
            cell = xsqlc_strdup("");  // SQL NULL -> "" + null flag
            bytes = 0;
            is_null = 1;
        } else {
            const void *src;
            int n;
            if (coltype == SQLITE_BLOB) {
                // BLOBs survive verbatim: pull the raw bytes (not the text form,
                // which stops at the first NUL) so embedded NULs round-trip.
                // blob() before bytes() so the count matches the raw payload.
                src = sqlite3_column_blob(stmt, (int)c);
                n = sqlite3_column_bytes(stmt, (int)c);
            } else {
                // text() before bytes() so the byte count matches the UTF-8 text.
                src = sqlite3_column_text(stmt, (int)c);
                n = sqlite3_column_bytes(stmt, (int)c);
            }
            if (!src || n < 0) {
                n = 0;
            }
            cell = (char *)xsqlc_malloc((size_t)n + 1);
            if (cell) {
                if (n > 0) {
                    memcpy(cell, src, (size_t)n);
                }
                cell[n] = '\0';
            }
            bytes = n;
            is_null = 0;
        }
        if (!cell) {
            // Roll back the cells already written for this row so free() over
            // nrows*ncols never touches an uninitialized slot.
            for (size_t k = 0; k < c; ++k) {
                xsqlc_free(r->cells[base + k]);
            }
            return false;
        }
        r->cells[base + c] = cell;
        r->cell_bytes[base + c] = bytes;
        r->cell_null[base + c] = is_null;
    }
    r->nrows++;
    return true;
}

// Set the result's error text and mirror it into the db's last_error.
static void result_set_error(xsql_result *r, xsql_db *db, const char *msg) {
    xsqlc_str_set(&r->error, msg);
    if (db) {
        xsqlc_str_set(&db->last_error, msg);
    }
}

static void result_clear_rows(xsql_result *r) {
    if (!r) return;
    size_t cell_count = r->nrows * r->ncols;
    for (size_t i = 0; i < cell_count; ++i) {
        xsqlc_free(r->cells[i]);
    }
    r->nrows = 0;
}

// Monotonic millisecond clock for deadline + elapsed math. Steady (unaffected by
// wall-clock adjustments).
static long long mono_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, ctr;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
        return 0;
    }
    QueryPerformanceCounter(&ctr);
    // Split the conversion so a long-uptime counter cannot overflow int64.
    long long whole = ctr.QuadPart / freq.QuadPart;
    long long frac = ctr.QuadPart % freq.QuadPart;
    return whole * 1000 + (frac * 1000) / freq.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// Whole-millisecond elapsed since a monotonic start stamp (never negative).
static int elapsed_ms_since(long long start_ms) {
    long long ms = mono_now_ms() - start_ms;
    if (ms < 0) {
        ms = 0;
    }
    return (int)ms;
}

// Append a non-fatal warning (used on a partial timeout). A lost advisory never
// fails a query, so an allocation failure here is silently dropped.
static void result_add_warning(xsql_result *r, const char *msg) {
    char **grown =
        (char **)xsqlc_realloc(r->warnings, (r->nwarn + 1) * sizeof(*grown));
    if (!grown) {
        return;
    }
    r->warnings = grown;
    char *copy = xsqlc_strdup(msg);
    if (!copy) {
        return;  // array grew but nwarn unchanged — spare capacity, not a leak
    }
    r->warnings[r->nwarn++] = copy;
}

// ---------------------------------------------------------------------------
// Deadline + cancellation plumbing. The progress handler aborts VDBE work between
// opcodes; the interrupt checker lets a long native virtual-table loop (which never
// yields to the VDBE) poll the same deadline/cancel via xsql_vtab_interrupted() and
// bail. Both consult one monotonic start stamp plus the optional cooperative-cancel
// predicate — which is honored even when timeout_ms == 0.
// ---------------------------------------------------------------------------

struct timeout_state {
    long long   started_ms;
    int         timeout_ms;
    bool        timed_out;
    // Optional cooperative-cancellation predicate (function pointer + user-data).
    // Checked ahead of the deadline and effective even when timeout_ms == 0.
    int       (*cancel)(void *user);
    void       *cancel_user;
    bool        cancelled;
};

static bool deadline_passed(const struct timeout_state *st) {
    return st && st->timeout_ms > 0 &&
           mono_now_ms() - st->started_ms >= st->timeout_ms;
}

// True once the caller-supplied cancel predicate requests cancellation. Latches
// st->cancelled so the classify step downstream can tell cancel from timeout.
static bool cancel_requested(struct timeout_state *st) {
    if (st && st->cancel && st->cancel(st->cancel_user)) {
        st->cancelled = true;
        return true;
    }
    return false;
}

// sqlite3_progress_handler callback: return nonzero to abort the statement.
// Cancellation takes precedence over the deadline (and works under timeout_ms == 0).
static int progress_cb(void *user) {
    struct timeout_state *st = (struct timeout_state *)user;
    if (cancel_requested(st)) {
        return 1;
    }
    if (deadline_passed(st)) {
        st->timed_out = true;
        return 1;
    }
    return 0;
}

// xsql_vtab interrupt checker: true once cancellation was requested or the deadline
// has passed (so a long native vtable loop bails on either).
static bool deadline_reached(void *user) {
    struct timeout_state *st = (struct timeout_state *)user;
    if (cancel_requested(st)) {
        return true;
    }
    if (deadline_passed(st)) {
        st->timed_out = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

int xsql_db_query_opts(xsql_db *db, const char *sql, const xsql_query_opts *opts,
                       xsql_result **out) {
    int timeout_ms = 0;
    int progress_steps = 1000;
    int (*should_cancel)(void *) = NULL;
    void *should_cancel_user = NULL;
    if (opts) {
        timeout_ms = opts->timeout_ms;
        if (opts->progress_steps > 0) {
            progress_steps = opts->progress_steps;
        }
        // should_cancel/_user are ABI-growth fields: read only when struct_size
        // covers them (a smaller/zeroed opts leaves cancellation off).
        if (opts->struct_size >=
            offsetof(xsql_query_opts, should_cancel_user) +
                sizeof(opts->should_cancel_user)) {
            should_cancel = opts->should_cancel;
            should_cancel_user = opts->should_cancel_user;
        }
    }
    const bool timeout_enabled = timeout_ms > 0;
    const bool cancel_enabled = should_cancel != NULL;
    // The progress-handler + interrupt-checker machinery is armed whenever EITHER a
    // deadline OR a cancel predicate is in play (cancel must work under timeout_ms==0).
    const bool guard_enabled = timeout_enabled || cancel_enabled;

    if (!out) {
        return XSQL_MISUSE;
    }
    *out = NULL;

    xsql_result *r = (xsql_result *)xsqlc_malloc(sizeof(*r));
    if (!r) {
        return XSQL_NO_MEMORY;
    }
    memset(r, 0, sizeof(*r));

    if (!db || !db->db) {
        result_set_error(r, db, "Database not open");
        *out = r;
        return XSQL_ERROR;
    }

    const long long started_ms = mono_now_ms();
    struct timeout_state ts = {started_ms,    timeout_ms,         false,
                               should_cancel, should_cancel_user, false};
    xsqlc_progress_guard progress_guard = {0};
    xsqlc_interrupt_guard interrupt_guard = {0};
    if (guard_enabled) {
        // Cover prepare-time xBestIndex and other module callbacks as part of
        // the same per-statement deadline.
        xsqlc_progress_guard_enter(
            &progress_guard, db->db, progress_steps, progress_cb, &ts);
        xsqlc_interrupt_guard_enter(
            &interrupt_guard, deadline_reached, &ts);
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        r->elapsed_ms = elapsed_ms_since(started_ms);
        if (guard_enabled) {
            xsqlc_interrupt_guard_leave(&interrupt_guard);
            xsqlc_progress_guard_leave(&progress_guard);
        }
        if (cancel_enabled && ts.cancelled) {
            result_set_error(r, db, "Query cancelled");
            rc = SQLITE_ERROR;
        } else if (timeout_enabled && ts.timed_out) {
            r->timed_out = true;
            result_set_error(r, db, "Query timed out");
            rc = SQLITE_ERROR;
        } else {
            result_set_error(r, db, sqlite3_errmsg(db->db));
        }
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        *out = r;
        return rc;
    }

    // Collect column names up front (before the step loop).
    int col_count = sqlite3_column_count(stmt);
    const bool partial_capable =
        col_count > 0 && sqlite3_stmt_readonly(stmt) != 0;
    if (col_count > 0) {
        r->columns = (char **)xsqlc_malloc((size_t)col_count * sizeof(char *));
        if (!r->columns) {
            if (guard_enabled) {
                xsqlc_interrupt_guard_leave(&interrupt_guard);
                xsqlc_progress_guard_leave(&progress_guard);
            }
            sqlite3_finalize(stmt);
            xsql_result_free(r);
            return XSQL_NO_MEMORY;
        }
        for (int i = 0; i < col_count; ++i) {
            r->columns[i] = NULL;  // keep free() safe if a later dup fails
        }
        for (int i = 0; i < col_count; ++i) {
            const char *name = sqlite3_column_name(stmt, i);
            r->columns[i] = xsqlc_strdup(name ? name : "");
            if (!r->columns[i]) {
                r->ncols = (size_t)col_count;  // so free() releases 0..i-1
                if (guard_enabled) {
                    xsqlc_interrupt_guard_leave(&interrupt_guard);
                    xsqlc_progress_guard_leave(&progress_guard);
                }
                sqlite3_finalize(stmt);
                xsql_result_free(r);
                return XSQL_NO_MEMORY;
            }
        }
    }
    r->ncols = (size_t)col_count;

    for (;;) {
        // Poll once before every step. SQLite's progress callback may not run
        // for a short mutating statement, so an already-cancelled request must
        // not execute it at all.
        if (guard_enabled && deadline_reached(&ts)) {
            rc = SQLITE_INTERRUPT;
            break;
        }
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            if (!result_append_row(r, stmt)) {
                if (guard_enabled) {
                    xsqlc_interrupt_guard_leave(&interrupt_guard);
                    xsqlc_progress_guard_leave(&progress_guard);
                }
                sqlite3_finalize(stmt);
                xsql_result_free(r);
                return XSQL_NO_MEMORY;
            }
            continue;
        }
        break;  // DONE, an error, a progress/cancel abort, or a cooperative vtab bail
    }

    r->elapsed_ms = elapsed_ms_since(started_ms);
    if (guard_enabled) {
        xsqlc_interrupt_guard_leave(&interrupt_guard);
        xsqlc_progress_guard_leave(&progress_guard);
    }

    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    } else if (cancel_enabled && ts.cancelled) {
        // Cancellation takes precedence over the deadline and works even when
        // timeout_ms == 0. The pre-step check, progress handler, and cooperative
        // vtable checker all latch ts.cancelled before SQLite returns an error.
        // Do not poll here: the statement may already have completed.
        if (partial_capable && r->nrows > 0) {
            // Read-only result with a non-empty prefix: keep the rows gathered
            // so far (partial success, ok()==true) and record an advisory warning.
            r->partial = true;
            result_add_warning(r, "query cancelled; returning partial rows");
            rc = SQLITE_OK;
        } else {
            // A mutation/non-result, OR a zero-row cancel: an interrupted write is
            // rolled back, and an empty "partial" would read as a valid truncation
            // of the real result, so a zero-row cancel is a hard error too.
            result_clear_rows(r);
            result_set_error(r, db, "Query cancelled");
            rc = SQLITE_ERROR;
        }
    } else if (timeout_enabled && ts.timed_out) {
        // A progress-handler abort flips ts.timed_out and makes step return
        // SQLITE_INTERRUPT; the pre-step and cooperative-vtable checks latch the
        // same flag. Do not sample the clock after completion.
        r->timed_out = true;
        if (partial_capable && r->nrows > 0) {
            // Read-only result with a non-empty prefix: keep the rows gathered
            // so far (partial success, ok()==true) and record an advisory warning.
            r->partial = true;
            result_add_warning(r, "query timed out; returning partial rows");
            rc = SQLITE_OK;
        } else {
            // A zero-row timeout is a hard error (an empty "partial" would read as
            // a valid truncation of the real result); r->timed_out stays set.
            result_clear_rows(r);
            result_set_error(r, db, "Query timed out");
            rc = SQLITE_ERROR;
        }
    } else {
        // A read-only statement may have yielded a useful prefix before a late
        // source error. Mutation RETURNING rows are provisional and must be
        // discarded when the statement fails.
        if (partial_capable && r->nrows > 0) {
            r->partial = true;
            result_add_warning(r, "query failed; returning partial rows");
        } else {
            result_clear_rows(r);
        }
        result_set_error(r, db, sqlite3_errmsg(db->db));
    }

    sqlite3_finalize(stmt);

    if (rc == SQLITE_OK) {
        xsqlc_str_set(&db->last_error, NULL);
    }
    *out = r;
    return rc;
}

int xsql_db_query(xsql_db *db, const char *sql, xsql_result **out) {
    return xsql_db_query_opts(db, sql, NULL, out);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

size_t xsql_result_column_count(const xsql_result *r) {
    return r ? r->ncols : 0;
}

const char *xsql_result_column_name(const xsql_result *r, size_t col) {
    if (!r || col >= r->ncols || !r->columns) {
        return NULL;
    }
    return r->columns[col];
}

size_t xsql_result_row_count(const xsql_result *r) {
    return r ? r->nrows : 0;
}

// Flat index of (row, col), or SIZE_MAX if out of range / no cell storage.
static size_t cell_index(const xsql_result *r, size_t row, size_t col) {
    if (!r || row >= r->nrows || col >= r->ncols || !r->cells) {
        return (size_t)-1;
    }
    return row * r->ncols + col;
}

const char *xsql_result_cell(const xsql_result *r, size_t row, size_t col) {
    size_t i = cell_index(r, row, col);
    return (i == (size_t)-1) ? NULL : r->cells[i];
}

int xsql_result_cell_bytes(const xsql_result *r, size_t row, size_t col) {
    size_t i = cell_index(r, row, col);
    return (i == (size_t)-1) ? 0 : r->cell_bytes[i];
}

bool xsql_result_cell_is_null(const xsql_result *r, size_t row, size_t col) {
    size_t i = cell_index(r, row, col);
    return (i == (size_t)-1) ? false : (r->cell_null[i] != 0);
}

bool xsql_result_ok(const xsql_result *r) {
    return r && (!r->error || r->error[0] == '\0');
}

const char *xsql_result_errmsg(const xsql_result *r) {
    if (!r || !r->error) {
        return "";
    }
    return r->error;
}

size_t xsql_result_warning_count(const xsql_result *r) {
    return r ? r->nwarn : 0;
}

const char *xsql_result_warning(const xsql_result *r, size_t i) {
    if (!r || i >= r->nwarn || !r->warnings) {
        return NULL;
    }
    return r->warnings[i];
}

bool xsql_result_timed_out(const xsql_result *r) {
    return r ? r->timed_out : false;
}

bool xsql_result_partial(const xsql_result *r) {
    return r ? r->partial : false;
}

int xsql_result_elapsed_ms(const xsql_result *r) {
    return r ? r->elapsed_ms : 0;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void xsql_result_free(xsql_result *r) {
    if (!r) {
        return;
    }
    if (r->columns) {
        for (size_t i = 0; i < r->ncols; ++i) {
            xsqlc_free(r->columns[i]);
        }
        xsqlc_free(r->columns);
    }
    if (r->cells) {
        size_t total = r->nrows * r->ncols;
        for (size_t i = 0; i < total; ++i) {
            xsqlc_free(r->cells[i]);
        }
        xsqlc_free(r->cells);
    }
    xsqlc_free(r->cell_bytes);
    xsqlc_free(r->cell_null);
    if (r->warnings) {
        for (size_t i = 0; i < r->nwarn; ++i) {
            xsqlc_free(r->warnings[i]);
        }
        xsqlc_free(r->warnings);
    }
    xsqlc_free(r->error);
    xsqlc_free(r);
}
