// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// http_server.c — the consolidated thin-client HTTP query server over the
// vendored civetweb transport. Serves GET / (A4
// uppercased landing), GET /help, POST /query (executor precedence + queue /
// serialize modes + ?format), GET /status (A5 RFC 7386 merge), POST /cancel,
// POST /shutdown.
// A13 (port 0 -> random [8100,8999]) and A17 (callback error handling: a
// callback that returns an error / leaves no body yields a JSON error envelope
// rather than an empty or malformed response) live here too. Note: pure C has no
// exception/panic mechanism, so a callback that crashes cannot be caught — A17
// only covers a callback that returns cleanly without producing a body.
//
// The POSIX feature macro must precede every include (monotonic clock, same
// discipline as result.c / thread_shim.c).

#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "libxsqlc/http_server.h"

#include "thinclient/http_internal.h"

#include "internal/internal.h"
#include "internal/strbuf.h"
#include "internal/json.h"
#include "internal/thread_shim.h"
#include "libxsqlc/format.h"
#include "libxsqlc/script.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <time.h>
#endif

#include "civetweb.h"

// ===========================================================================
// Small utilities
// ===========================================================================

static long long mono_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    if (freq.QuadPart == 0) {
        return 0;
    }
    return (long long)(ctr.QuadPart * 1000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static bool is_loopback(const char *addr) {
    if (!addr || addr[0] == '\0') {
        return true;  // default bind is 127.0.0.1
    }
    return strncmp(addr, "127.", 4) == 0 || strcmp(addr, "::1") == 0 ||
           strcmp(addr, "localhost") == 0;
}

// {"success":false,"error":"<msg>"} as a heap string (NULL on OOM).
static char *json_err(const char *msg) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "{\"success\":false,\"error\":");
    xsqlc_json_append_string(&sb, msg, strlen(msg));
    xsqlc_sb_putc(&sb, '}');
    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        return NULL;
    }
    return xsqlc_sb_detach(&sb);
}

static char *json_err_hint(const char *msg, const char *hint) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "{\"success\":false,\"error\":");
    xsqlc_json_append_string(&sb, msg, strlen(msg));
    xsqlc_sb_append(&sb, ",\"hint\":");
    xsqlc_json_append_string(&sb, hint, strlen(hint));
    xsqlc_sb_putc(&sb, '}');
    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        return NULL;
    }
    return xsqlc_sb_detach(&sb);
}

// ===========================================================================
// Out-sinks (public setters)
// ===========================================================================

struct xsql_http_response {
    char  *body;          // heap; NULL until set
    size_t body_len;
    char  *content_type;  // heap; NULL => default
    int    status;        // default 200
};

struct xsql_http_status_out {
    char *json;  // heap; the object to merge-patch
};

void xsql_http_response_set_body(xsql_http_response *out, const char *body,
                                 size_t len, const char *content_type) {
    if (!out) {
        return;
    }
    if ((len != 0 && !body) || len == SIZE_MAX) {
        return;
    }
    char *copy = (char *)xsqlc_malloc(len + 1);
    if (!copy) {
        return;
    }
    if (len && body) {
        memcpy(copy, body, len);
    }
    copy[len] = '\0';
    xsqlc_free(out->body);
    out->body = copy;
    out->body_len = len;
    xsqlc_str_set(&out->content_type, content_type);
}

void xsql_http_response_set_status(xsql_http_response *out, int status) {
    if (out) {
        out->status = status;
    }
}

void xsql_http_status_out_set_json(xsql_http_status_out *out,
                                   const char *json_object) {
    if (out) {
        xsqlc_str_set(&out->json, json_object);
    }
}

static void response_reset(xsql_http_response *r) {
    xsqlc_free(r->body);
    xsqlc_free(r->content_type);
    memset(r, 0, sizeof(*r));
    r->status = 200;
}

// ===========================================================================
// Command queue (use_queue mode)
// ===========================================================================

struct request_cancel {
    xsql_http_server *server;
    unsigned long long epoch;
};

enum admit_result { ADMIT_OK, ADMIT_NOT_RUNNING, ADMIT_QUEUE_FULL, ADMIT_TIMEOUT };

struct pending_command {
    struct pending_command *next;
    char               *sql;            // heap
    bool                use_executor;
    xsql_script_opts    opts;
    char               *result_json;    // legacy query_fn path (heap)
    xsql_script_result *script_result;  // executor path (owned)
    bool                started;        // admission deadline ends here
    bool                completed;
    bool                cancelled;
    enum admit_result   terminal_admission;
    struct request_cancel request_cancel;
    int                 refcount;       // guarded by done_mtx
    xsqlc_mtx          *done_mtx;
    xsqlc_cond         *done_cv;
};

static struct pending_command *cmd_new(const char *sql) {
    struct pending_command *c =
        (struct pending_command *)xsqlc_malloc(sizeof(*c));
    if (!c) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->refcount = 1;
    c->sql = xsqlc_strdup(sql);
    if (!c->sql || xsqlc_mtx_init(&c->done_mtx) != XSQL_OK ||
        xsqlc_cond_init(&c->done_cv) != XSQL_OK) {
        xsqlc_free(c->sql);
        xsqlc_mtx_destroy(c->done_mtx);
        xsqlc_cond_destroy(c->done_cv);
        xsqlc_free(c);
        return NULL;
    }
    return c;
}

static void cmd_free(struct pending_command *c) {
    xsqlc_mtx_destroy(c->done_mtx);
    xsqlc_cond_destroy(c->done_cv);
    xsqlc_free(c->sql);
    xsqlc_free(c->result_json);
    xsql_script_result_free(c->script_result);
    xsqlc_free(c);
}

static void cmd_retain(struct pending_command *c) {
    xsqlc_mtx_lock(c->done_mtx);
    ++c->refcount;
    xsqlc_mtx_unlock(c->done_mtx);
}

static void cmd_release(struct pending_command *c) {
    xsqlc_mtx_lock(c->done_mtx);
    int n = --c->refcount;
    xsqlc_mtx_unlock(c->done_mtx);
    if (n == 0) {
        cmd_free(c);
    }
}

// ===========================================================================
// Server
// ===========================================================================

struct xsql_http_server {
    // Deep-copied config.
    char *tool_name;
    char *help_text;
    char *bind_address;
    char *auth_token;  // NULL/"" => no auth
    int   port_config;
    bool  allow_insecure_no_auth;
    bool  status_requires_auth;

    xsql_http_script_executor_fn script_executor;
    void                        *script_executor_user;
    xsql_script_executor_fn      statement_executor;
    void                        *statement_executor_user;
    xsql_http_query_fn           query_fn;
    void                        *query_fn_user;
    xsql_http_status_fn          status_fn;
    void                        *status_fn_user;

    xsql_http_status_field *status_fields;  // deep-copied
    size_t                  nstatus_fields;

    xsql_http_route *routes;  // deep-copied
    size_t           nroutes;

    int    queue_admission_timeout_ms;
    size_t max_queue;
    xsql_http_timeout_fn   queue_admission_timeout_fn;
    void                  *queue_admission_timeout_fn_user;
    xsql_http_max_queue_fn max_queue_fn;
    void                  *max_queue_fn_user;

    bool use_queue;
    bool serialize_requests;
    size_t max_request_body_bytes;  // 0 => XSQLC_HTTP_MAX_BODY_BYTES

    // Runtime.
    struct mg_context *ctx;      // NULL when stopped
    int                port;     // bound port (0 when stopped)
    bool               running;
    xsqlc_mtx         *state_mtx;

    // Command queue.
    xsqlc_mtx              *queue_mtx;
    xsqlc_cond             *queue_cv;
    struct pending_command *q_head;
    struct pending_command *q_tail;
    size_t                  q_len;
    bool                    processing;

    // Serialize gate.
    xsqlc_mtx  *ser_mtx;
    xsqlc_cond *ser_cv;
    bool        ser_busy;
    size_t      ser_waiting;

    // Detached /shutdown latch.
    xsqlc_mtx  *latch_mtx;
    xsqlc_cond *latch_cv;
    size_t      shutdown_inflight;

    // In-flight request-handler barrier: stop() waits (bounded) for handlers to
    // finish flushing before mg_stop() tears their sockets down, so a drained
    // queued request delivers its "HTTP server stopped" envelope rather than a
    // bare connection drop.
    xsqlc_mtx  *inflight_mtx;
    xsqlc_cond *inflight_cv;
    size_t      inflight;

    // Cooperative cancellation epoch. Every query snapshots it; POST /cancel
    // increments it, so newly queued requests cannot clear an older request's
    // cancellation state.
    xsqlc_mtx         *cancel_mtx;
    unsigned long long cancel_epoch;

    xsql_http_interrupt_fn interrupt_fn;
    void                  *interrupt_user;
};

static unsigned long long cancel_epoch_snapshot(xsql_http_server *s) {
    xsqlc_mtx_lock(s->cancel_mtx);
    unsigned long long epoch = s->cancel_epoch;
    xsqlc_mtx_unlock(s->cancel_mtx);
    return epoch;
}

static int request_should_cancel(void *user) {
    struct request_cancel *request = (struct request_cancel *)user;
    if (!request || !request->server) {
        return 0;
    }
    return cancel_epoch_snapshot(request->server) != request->epoch;
}

static bool server_running(xsql_http_server *s) {
    xsqlc_mtx_lock(s->state_mtx);
    bool r = s->running;
    xsqlc_mtx_unlock(s->state_mtx);
    return r;
}

// Build a failed script_result carrying `msg` (via the canonical error envelope).
static xsql_script_result *make_error_script_result(const char *msg) {
    char *j = json_err(msg);
    xsql_script_result *r = NULL;
    if (j) {
        xsql_script_result_from_json(j, &r);
        xsqlc_free(j);
    }
    if (!r) {
        xsql_script_result_from_json(
            "{\"success\":false,\"error\":\"error\"}", &r);
    }
    return r;
}

// Run the legacy query_fn into a heap JSON string (A17: fall back to an error
// envelope if the callback returned without setting a body).
static char *invoke_query_fn(xsql_http_server *s, const char *sql) {
    xsql_http_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = 200;
    s->query_fn(sql, &resp, s->query_fn_user);
    char *json;
    if (resp.body) {
        json = resp.body;
        resp.body = NULL;  // take ownership
    } else {
        json = json_err("thinclient query callback returned no result body");
    }
    response_reset(&resp);
    return json;
}

// ---- queue admission -------------------------------------------------------

static int admission_http_status(enum admit_result admission) {
    switch (admission) {
        case ADMIT_NOT_RUNNING:
        case ADMIT_QUEUE_FULL:
            return 503;
        case ADMIT_TIMEOUT:
            return 408;
        case ADMIT_OK:
        default:
            return 200;
    }
}

static enum admit_result admit_and_wait(xsql_http_server *s,
                                        struct pending_command *cmd) {
    if (!server_running(s)) {
        return ADMIT_NOT_RUNNING;
    }

    int timeout_ms = s->queue_admission_timeout_ms;
    if (s->queue_admission_timeout_fn) {
        timeout_ms = s->queue_admission_timeout_fn(s->queue_admission_timeout_fn_user);
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    xsqlc_mtx_lock(s->queue_mtx);
    size_t max_q = s->max_queue;
    if (s->max_queue_fn) {
        max_q = s->max_queue_fn(s->max_queue_fn_user);
    }
    size_t outstanding = s->q_len + (s->processing ? 1u : 0u);
    if (max_q > 0 && outstanding >= max_q) {
        xsqlc_mtx_unlock(s->queue_mtx);
        return ADMIT_QUEUE_FULL;
    }
    // Enqueue (queue holds a ref).
    cmd_retain(cmd);
    cmd->next = NULL;
    if (s->q_tail) {
        s->q_tail->next = cmd;
    } else {
        s->q_head = cmd;
    }
    s->q_tail = cmd;
    ++s->q_len;
    xsqlc_mtx_unlock(s->queue_mtx);
    xsqlc_cond_signal(s->queue_cv);

    // Wait only until the owner starts it under the admission deadline. Once
    // started, execution may legitimately run longer than that deadline.
    xsqlc_mtx_lock(cmd->done_mtx);
    if (timeout_ms == 0) {
        // Unbounded admission wait, but poll every 200ms so a server stop can
        // break the loop: at shutdown the owner thread stops dequeuing, and a
        // command already dequeued (so past drain_pending's reach) but never
        // completed would otherwise hang here forever. server_running() flips to
        // false under xsql_http_server_stop(), which then broadcasts done_cv-side
        // waiters via the drain path; the poll interval bounds the worst case.
        while (!cmd->completed && !cmd->started) {
            if (!server_running(s)) {
                cmd->cancelled = true;  // worker must skip an abandoned request
                xsqlc_mtx_unlock(cmd->done_mtx);
                return ADMIT_NOT_RUNNING;
            }
            xsqlc_cond_wait_for(cmd->done_cv, cmd->done_mtx, 200);
        }
    } else {
        long long deadline = mono_now_ms() + timeout_ms;
        while (!cmd->completed && !cmd->started) {
            long long rem = deadline - mono_now_ms();
            if (rem <= 0) {
                break;
            }
            xsqlc_cond_wait_for(cmd->done_cv, cmd->done_mtx, (int)rem);
        }
        if (!cmd->completed && !cmd->started) {
            cmd->cancelled = true;  // worker must skip an abandoned request
            xsqlc_mtx_unlock(cmd->done_mtx);
            return ADMIT_TIMEOUT;
        }
    }
    while (!cmd->completed) {
        xsqlc_cond_wait_for(cmd->done_cv, cmd->done_mtx, 200);
    }
    enum admit_result result = cmd->terminal_admission;
    xsqlc_mtx_unlock(cmd->done_mtx);
    return result;
}

static char *queue_and_wait(xsql_http_server *s, const char *sql,
                            enum admit_result *admission) {
    *admission = ADMIT_OK;
    struct pending_command *cmd = cmd_new(sql);
    if (!cmd) {
        return json_err("Out of memory");
    }
    char *json = NULL;
    *admission = admit_and_wait(s, cmd);
    switch (*admission) {
        case ADMIT_NOT_RUNNING:
            xsqlc_mtx_lock(cmd->done_mtx);
            json = cmd->result_json;
            cmd->result_json = NULL;
            xsqlc_mtx_unlock(cmd->done_mtx);
            if (!json) {
                json = json_err("Server not running");
            }
            break;
        case ADMIT_QUEUE_FULL:
            json = json_err_hint("Queue full",
                                 "Reduce concurrency or increase max_queue");
            break;
        case ADMIT_TIMEOUT:
            json = json_err_hint(
                "Request timed out while waiting in queue",
                "Reduce concurrency or increase queue_admission_timeout_ms");
            break;
        case ADMIT_OK:
            xsqlc_mtx_lock(cmd->done_mtx);
            json = cmd->result_json;
            cmd->result_json = NULL;
            xsqlc_mtx_unlock(cmd->done_mtx);
            break;
    }
    cmd_release(cmd);
    return json;
}

static xsql_script_result *queue_and_wait_script(xsql_http_server *s,
                                                  const char *sql,
                                                  const xsql_script_opts *opts,
                                                  enum admit_result *admission) {
    *admission = ADMIT_OK;
    struct pending_command *cmd = cmd_new(sql);
    if (!cmd) {
        return make_error_script_result("Out of memory");
    }
    cmd->use_executor = true;
    cmd->opts = *opts;
    if (opts->should_cancel == request_should_cancel &&
        opts->should_cancel_user) {
        cmd->request_cancel =
            *(const struct request_cancel *)opts->should_cancel_user;
        cmd->opts.should_cancel_user = &cmd->request_cancel;
    }
    xsql_script_result *r = NULL;
    *admission = admit_and_wait(s, cmd);
    switch (*admission) {
        case ADMIT_NOT_RUNNING:
            xsqlc_mtx_lock(cmd->done_mtx);
            r = cmd->script_result;
            cmd->script_result = NULL;
            xsqlc_mtx_unlock(cmd->done_mtx);
            if (!r) {
                r = make_error_script_result("Server not running");
            }
            break;
        case ADMIT_QUEUE_FULL:
            r = make_error_script_result("Queue full");
            break;
        case ADMIT_TIMEOUT:
            r = make_error_script_result("Request timed out while waiting in queue");
            break;
        case ADMIT_OK:
            xsqlc_mtx_lock(cmd->done_mtx);
            r = cmd->script_result;
            cmd->script_result = NULL;
            xsqlc_mtx_unlock(cmd->done_mtx);
            break;
    }
    cmd_release(cmd);
    return r;
}

static bool process_one_internal(xsql_http_server *s, int timeout_ms) {
    struct pending_command *cmd = NULL;
    xsqlc_mtx_lock(s->queue_mtx);
    if (timeout_ms > 0 && s->q_head == NULL) {
        xsqlc_cond_wait_for(s->queue_cv, s->queue_mtx, timeout_ms);
    }
    if (s->q_head) {
        cmd = s->q_head;
        s->q_head = cmd->next;
        if (!s->q_head) {
            s->q_tail = NULL;
        }
        --s->q_len;
        s->processing = true;  // count in-flight toward max_queue
    }
    xsqlc_mtx_unlock(s->queue_mtx);

    if (!cmd) {
        return false;
    }

    xsqlc_mtx_lock(cmd->done_mtx);
    cmd->started = true;
    bool skip = cmd->cancelled || cmd->completed;
    xsqlc_mtx_unlock(cmd->done_mtx);
    xsqlc_cond_broadcast(cmd->done_cv);

    if (!skip) {
        // D5 note (C++ F14): "arm the /cancel epoch at execution start" is N/A
        // here — /cancel returns 409 for the statement executor (handle_cancel),
        // and the whole-script executor (the only one /cancel drives) has no queue
        // path (xsql_http_server_new rejects script_executor + use_queue). So a
        // queued command's epoch never advances and cannot be spuriously cancelled.
        if (cmd->use_executor && s->statement_executor) {
            xsql_script_result *r = NULL;
            xsql_run_script(cmd->sql, s->statement_executor,
                            s->statement_executor_user, &cmd->opts, &r);
            cmd->script_result = r;
        } else if (s->query_fn) {
            cmd->result_json = invoke_query_fn(s, cmd->sql);
        } else {
            cmd->result_json = json_err("No query handler");
        }
        xsqlc_mtx_lock(cmd->done_mtx);
        cmd->completed = true;
        xsqlc_mtx_unlock(cmd->done_mtx);
        xsqlc_cond_signal(cmd->done_cv);
    }

    xsqlc_mtx_lock(s->queue_mtx);
    s->processing = false;
    xsqlc_mtx_unlock(s->queue_mtx);

    cmd_release(cmd);  // release the queue's ref
    return true;
}

static void drain_pending(xsql_http_server *s, const char *message) {
    struct pending_command *head;
    xsqlc_mtx_lock(s->queue_mtx);
    head = s->q_head;
    s->q_head = NULL;
    s->q_tail = NULL;
    s->q_len = 0;
    xsqlc_mtx_unlock(s->queue_mtx);

    while (head) {
        struct pending_command *cmd = head;
        head = head->next;
        xsqlc_mtx_lock(cmd->done_mtx);
        if (!cmd->completed) {
            if (cmd->use_executor) {
                cmd->script_result = make_error_script_result(message);
            } else {
                cmd->result_json = json_err(message);
            }
            cmd->terminal_admission = ADMIT_NOT_RUNNING;
            cmd->completed = true;
        }
        xsqlc_mtx_unlock(cmd->done_mtx);
        xsqlc_cond_signal(cmd->done_cv);
        cmd_release(cmd);  // release the queue's ref
    }
}

// ---- serialize gate --------------------------------------------------------

enum gate_result { GATE_OK, GATE_FULL, GATE_TIMEOUT };

static enum gate_result gate_acquire(xsql_http_server *s, int timeout_ms,
                                     size_t max_q) {
    xsqlc_mtx_lock(s->ser_mtx);
    ++s->ser_waiting;
    size_t now_pending = s->ser_waiting;
    if (max_q > 0 && now_pending > max_q) {
        --s->ser_waiting;
        xsqlc_mtx_unlock(s->ser_mtx);
        return GATE_FULL;
    }
    if (timeout_ms == 0) {
        while (s->ser_busy) {
            xsqlc_cond_wait_for(s->ser_cv, s->ser_mtx, 0);
        }
    } else {
        long long deadline = mono_now_ms() + timeout_ms;
        while (s->ser_busy) {
            long long rem = deadline - mono_now_ms();
            if (rem <= 0) {
                --s->ser_waiting;
                xsqlc_mtx_unlock(s->ser_mtx);
                return GATE_TIMEOUT;
            }
            xsqlc_cond_wait_for(s->ser_cv, s->ser_mtx, (int)rem);
        }
    }
    s->ser_busy = true;
    xsqlc_mtx_unlock(s->ser_mtx);
    return GATE_OK;
}

static void gate_release(xsql_http_server *s) {
    xsqlc_mtx_lock(s->ser_mtx);
    s->ser_busy = false;
    if (s->ser_waiting > 0) {
        --s->ser_waiting;
    }
    xsqlc_cond_signal(s->ser_cv);
    xsqlc_mtx_unlock(s->ser_mtx);
}

// ===========================================================================
// HTTP response helpers
// ===========================================================================

static void send_response(struct mg_connection *conn, int status,
                          const char *ctype, const char *body, size_t len) {
    const char *reason = mg_get_response_code_text(conn, status);
    // Frame the body strictly by Content-Length (no "Connection: close", which
    // makes some clients switch to read-until-EOF and wait out civetweb's
    // graceful-close discard-read). civetweb is configured with keep-alive off,
    // so the connection is closed after the response regardless.
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %zu\r\n"
              "\r\n",
              status, reason ? reason : "", ctype ? ctype : "text/plain", len);
    if (len && body) {
        mg_write(conn, body, len);
    }
}

static void send_json(struct mg_connection *conn, int status, const char *json) {
    send_response(conn, status, "application/json", json, json ? strlen(json) : 0);
}

static void send_json_err(struct mg_connection *conn, int status,
                          const char *msg) {
    char *j = json_err(msg);
    if (j) {
        send_json(conn, status, j);
        xsqlc_free(j);
    } else {
        send_json(conn, status, "{\"success\":false,\"error\":\"error\"}");
    }
}

static void send_formatted(struct mg_connection *conn,
                           const xsql_script_result *r, const char *format,
                           bool include_sql) {
    char *out;
    const char *ctype;
    if (strcmp(format, "text") == 0) {
        out = xsql_script_result_to_text(r);
        ctype = "text/plain";
    } else if (strcmp(format, "csv") == 0) {
        out = xsql_script_result_to_csv(r);
        ctype = "text/csv";
    } else if (strcmp(format, "tsv") == 0) {
        out = xsql_script_result_to_tsv(r);
        ctype = "text/tab-separated-values";
    } else {
        out = xsql_script_result_to_json(r, include_sql);
        ctype = "application/json";
    }
    if (out) {
        send_response(conn, 200, ctype, out, strlen(out));
        xsqlc_free(out);
    } else {
        send_json_err(conn, 500, "Out of memory");
    }
}

// ===========================================================================
// Endpoint handlers
// ===========================================================================

// Constant-time token comparison, mirroring the C++
// xsql::thinclient::detail::timing_safe_equal. strcmp short-circuits on the first
// differing byte, turning an authenticated endpoint into a remote timing oracle
// for the secret's matching prefix; accumulate over the presented token's full
// length instead. The secret's LENGTH is not concealed (a length mismatch is
// unequal), only prefix-match timing.
static bool timing_safe_equal(const char *presented, const char *expected) {
    size_t plen = strlen(presented);
    size_t elen = strlen(expected);
    unsigned char acc = (plen == elen) ? 0u : 1u;
    for (size_t i = 0; i < plen; ++i) {
        unsigned char e =
            elen ? (unsigned char)expected[i % elen] : 0u;
        acc = (unsigned char)(acc | ((unsigned char)presented[i] ^ e));
    }
    return acc == 0;
}

static bool check_auth(xsql_http_server *s, struct mg_connection *conn) {
    if (!s->auth_token || s->auth_token[0] == '\0') {
        return true;
    }
    const char *token = "";
    const char *xtok = mg_get_header(conn, "X-XSQL-Token");
    if (xtok) {
        token = xtok;
    } else {
        const char *auth = mg_get_header(conn, "Authorization");
        if (auth && strncmp(auth, "Bearer ", 7) == 0) {
            token = auth + 7;
        }
    }
    if (timing_safe_equal(token, s->auth_token)) {
        return true;
    }
    send_json(conn, 401, "{\"success\":false,\"error\":\"Unauthorized\"}");
    return false;
}

static void handle_root(xsql_http_server *s, struct mg_connection *conn) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    const char *tool = s->tool_name ? s->tool_name : "";
    for (const char *p = tool; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 'a' + 'A');
        }
        xsqlc_sb_putc(&sb, (char)c);
    }
    xsqlc_sb_append(&sb,
                    " HTTP Server\n\nEndpoints:\n"
                    "  GET  /help     - API documentation\n"
                    "  POST /query    - Execute SQL query\n"
                    "  GET  /status   - Health check\n"
                    "  POST /cancel   - Cancel active queries\n"
                    "  POST /shutdown - Stop server\n\n"
                    "Example: curl -X POST http://localhost:");
    xsqlc_sb_append_i64(&sb, s->port);
    xsqlc_sb_append(&sb,
                    "/query -d \"SELECT name FROM sqlite_master WHERE "
                    "type='table' LIMIT 10\"\n");
    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        send_response(conn, 200, "text/plain", "", 0);
        return;
    }
    send_response(conn, 200, "text/plain", xsqlc_sb_cstr(&sb), sb.len);
    xsqlc_sb_free(&sb);
}

static void handle_status(xsql_http_server *s, struct mg_connection *conn) {
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "{\"success\":true,\"status\":\"ok\",\"tool\":");
    xsqlc_json_append_string(&sb, s->tool_name ? s->tool_name : "",
                             s->tool_name ? strlen(s->tool_name) : 0);
    // Static status_fields (string values) merged onto the base first; status_fn
    // is merge-patched on top below, so it can override any of them.
    for (size_t i = 0; i < s->nstatus_fields; ++i) {
        const char *k = s->status_fields[i].key ? s->status_fields[i].key : "";
        const char *v = s->status_fields[i].value ? s->status_fields[i].value : "";
        xsqlc_sb_putc(&sb, ',');
        xsqlc_json_append_string(&sb, k, strlen(k));
        xsqlc_sb_putc(&sb, ':');
        xsqlc_json_append_string(&sb, v, strlen(v));
    }
    xsqlc_sb_putc(&sb, '}');
    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        send_json_err(conn, 500, "Out of memory");
        return;
    }
    char *base = xsqlc_sb_detach(&sb);
    if (!base) {
        send_json_err(conn, 500, "Out of memory");
        return;
    }

    char *final_json = base;
    if (s->status_fn) {
        xsql_http_status_out out;
        memset(&out, 0, sizeof(out));
        s->status_fn(&out, s->status_fn_user);
        if (out.json) {
            char *merged = NULL;
            if (xsqlc_status_merge_patch(base, out.json, &merged) == XSQL_OK &&
                merged) {
                xsqlc_free(base);
                final_json = merged;
            }
            xsqlc_free(out.json);
        }
    }
    send_json(conn, 200, final_json);
    xsqlc_free(final_json);
}

// Hard ceiling on a single request body. A client could otherwise POST an
// arbitrarily large body and force ~2x-peak growth in the accumulation buffer
// (memory-exhaustion DoS). 64 MiB is far above any legitimate SQL payload; over
// the limit the handler returns 413. Compile-time overridable for embedders that
// genuinely need larger bodies.
#ifndef XSQLC_HTTP_MAX_BODY_BYTES
#  define XSQLC_HTTP_MAX_BODY_BYTES ((size_t)64 * 1024 * 1024)
#endif

// Read the full request body into a heap buffer (caller frees with xsqlc_free).
// Returns NULL for an empty body OR any error; the caller disambiguates via the
// out-flags: *out_oom on allocation failure, *out_too_large when the body would
// exceed XSQLC_HTTP_MAX_BODY_BYTES (declared Content-Length or streamed length).
static char *read_body(struct mg_connection *conn,
                       const struct mg_request_info *ri, size_t max_bytes,
                       size_t *out_len, bool *out_oom, bool *out_too_large) {
    if (max_bytes == 0) {
        max_bytes = XSQLC_HTTP_MAX_BODY_BYTES;
    }
    *out_len = 0;
    if (out_oom) {
        *out_oom = false;
    }
    if (out_too_large) {
        *out_too_large = false;
    }
    // Fast reject on a declared Content-Length over the cap (before allocating).
    if (ri && ri->content_length > 0 &&
        (unsigned long long)ri->content_length > (unsigned long long)max_bytes) {
        if (out_too_large) {
            *out_too_large = true;
        }
        return NULL;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    char buf[8192];
    int n;
    while ((n = mg_read(conn, buf, sizeof(buf))) > 0) {
        // Guard streamed length too (chunked bodies carry no Content-Length).
        if (sb.len + (size_t)n > max_bytes) {
            xsqlc_sb_free(&sb);
            if (out_too_large) {
                *out_too_large = true;
            }
            return NULL;
        }
        xsqlc_sb_append_len(&sb, buf, (size_t)n);
    }
    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        if (out_oom) {
            *out_oom = true;
        }
        return NULL;
    }
    *out_len = sb.len;
    if (sb.len == 0) {
        xsqlc_sb_free(&sb);
        return NULL;  // empty body
    }
    return xsqlc_sb_detach(&sb);
}

static bool get_param(const struct mg_request_info *ri, const char *name,
                      char *buf, size_t buflen) {
    if (buflen) {
        buf[0] = '\0';
    }
    if (!ri->query_string) {
        return false;
    }
    int r = mg_get_var2(ri->query_string, strlen(ri->query_string), name, buf,
                        buflen, 0);
    return r >= 0;
}

static void handle_query(xsql_http_server *s, struct mg_connection *conn,
                          const struct mg_request_info *ri) {
    struct request_cancel request_cancel;
    request_cancel.server = s;
    request_cancel.epoch = cancel_epoch_snapshot(s);

    size_t body_len = 0;
    bool body_oom = false;
    bool body_too_large = false;
    char *body = read_body(conn, ri, s->max_request_body_bytes, &body_len, &body_oom, &body_too_large);
    if (body_too_large) {
        send_json_err(conn, 413, "Request body too large");
        return;
    }
    if (body_oom) {
        send_json_err(conn, 500, "Out of memory");
        return;
    }
    if (!body || body_len == 0) {
        xsqlc_free(body);
        send_json_err(conn, 400, "Empty query");
        return;
    }

    bool gate_held = false;
    if (s->serialize_requests && !s->use_queue) {
        size_t max_q = s->max_queue_fn ? s->max_queue_fn(s->max_queue_fn_user)
                                       : s->max_queue;
        int timeout_ms = s->queue_admission_timeout_fn
                             ? s->queue_admission_timeout_fn(
                                   s->queue_admission_timeout_fn_user)
                             : s->queue_admission_timeout_ms;
        if (timeout_ms < 0) {
            timeout_ms = 0;
        }
        switch (gate_acquire(s, timeout_ms, max_q)) {
            case GATE_FULL:
                xsqlc_free(body);
                send_json(conn, 503,
                          "{\"success\":false,\"error\":\"Queue full\",\"hint\":"
                          "\"Reduce concurrency or increase max_queue\"}");
                return;
            case GATE_TIMEOUT:
                xsqlc_free(body);
                send_json(conn, 408,
                          "{\"success\":false,\"error\":\"Request timed out while "
                          "waiting for serialization\",\"hint\":\"Reduce "
                          "concurrency or increase queue_admission_timeout_ms\"}");
                return;
            case GATE_OK:
                gate_held = true;
                break;
        }
    }

    // Output format (default json); an unrecognized value is a 400.
    char fbuf[32];
    const char *format = "json";
    if (get_param(ri, "format", fbuf, sizeof(fbuf)) && fbuf[0]) {
        format = fbuf;
        if (strcmp(format, "json") != 0 && strcmp(format, "text") != 0 &&
            strcmp(format, "csv") != 0 && strcmp(format, "tsv") != 0) {
            const char *msg;
            xsqlc_strbuf mb;
            xsqlc_sb_init(&mb);
            xsqlc_sb_append(&mb, "unrecognized ?format '");
            xsqlc_sb_append(&mb, format);
            xsqlc_sb_append(&mb, "' (expected json, text, csv, or tsv)");
            msg = xsqlc_sb_oom(&mb) ? NULL : xsqlc_sb_cstr(&mb);
            send_json_err(conn, 400, msg ? msg : "unrecognized ?format");
            xsqlc_sb_free(&mb);
            if (gate_held) {
                gate_release(s);
            }
            xsqlc_free(body);
            return;
        }
    }

    // Resolve SQL + JSON-body flags.
    const char *ctype_hdr = mg_get_header(conn, "Content-Type");
    bool declared_json = ctype_hdr && strstr(ctype_hdr, "application/json");
    char *sql = NULL;
    bool json_coe = false, json_incl = false;
    xsqlc_qbody_status qb =
        xsqlc_resolve_query_body(body, declared_json, &sql, &json_coe, &json_incl);
    xsqlc_free(body);
    if (qb == XSQLC_QBODY_MALFORMED) {
        send_json_err(conn, 400, "malformed JSON request body");
        if (gate_held) {
            gate_release(s);
        }
        return;
    }
    if (qb == XSQLC_QBODY_MISSING_SQL) {
        send_json_err(conn, 400,
                      "JSON request body must be an object with a string \"sql\"");
        if (gate_held) {
            gate_release(s);
        }
        return;
    }
    if (!sql || sql[0] == '\0') {
        xsqlc_free(sql);
        send_json_err(conn, 400, "Empty query");
        if (gate_held) {
            gate_release(s);
        }
        return;
    }

    // Options: query params ("1") OR JSON body flags; either enabling wins.
    xsql_script_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.should_cancel = request_should_cancel;
    opts.should_cancel_user = &request_cancel;
    char pbuf[8];
    if (get_param(ri, "continue_on_error", pbuf, sizeof(pbuf)) &&
        strcmp(pbuf, "1") == 0) {
        opts.continue_on_error = true;
    }
    if (get_param(ri, "include_sql", pbuf, sizeof(pbuf)) &&
        strcmp(pbuf, "1") == 0) {
        opts.include_sql = true;
    }
    if (json_coe) {
        opts.continue_on_error = true;
    }
    if (json_incl) {
        opts.include_sql = true;
    }

    // Executor precedence: script > statement > query_fn.
    if (s->script_executor) {
        xsql_script_result *r = NULL;
        int rc = s->script_executor(sql, &opts, &r, s->script_executor_user);
        if (rc != XSQL_OK || !r) {
            xsql_script_result_free(r);
            send_json_err(conn, 500,
                          "thinclient script executor returned an error");
        } else {
            send_formatted(conn, r, format, opts.include_sql);
            xsql_script_result_free(r);
        }
    } else if (s->statement_executor) {
        xsql_script_result *r = NULL;
        enum admit_result admission = ADMIT_OK;
        if (s->use_queue) {
            r = queue_and_wait_script(s, sql, &opts, &admission);
        } else {
            xsql_run_script(sql, s->statement_executor,
                            s->statement_executor_user, &opts, &r);
        }
        if (r) {
            if (admission == ADMIT_OK) {
                send_formatted(conn, r, format, opts.include_sql);
            } else {
                char *json =
                    xsql_script_result_to_json(r, opts.include_sql);
                if (json) {
                    send_json(conn, admission_http_status(admission), json);
                    xsql_free(json);
                } else {
                    send_json_err(conn, 500, "Out of memory");
                }
            }
            xsql_script_result_free(r);
        } else {
            send_json_err(conn, 500, "Out of memory");
        }
    } else if (s->query_fn) {
        enum admit_result admission = ADMIT_OK;
        char *result = s->use_queue
            ? queue_and_wait(s, sql, &admission)
            : invoke_query_fn(s, sql);
        if (!result) {
            send_json_err(conn, 500, "Out of memory");
        } else if (strcmp(format, "json") == 0) {
            send_json(conn, admission_http_status(admission), result);
            xsqlc_free(result);
        } else {
            if (admission != ADMIT_OK) {
                send_json(conn, admission_http_status(admission), result);
                xsqlc_free(result);
                xsqlc_free(sql);
                if (gate_held) {
                    gate_release(s);
                }
                return;
            }
            xsql_script_result *r = NULL;
            xsql_script_result_from_json(result, &r);
            xsqlc_free(result);
            if (r) {
                send_formatted(conn, r, format, false);
                xsql_script_result_free(r);
            } else {
                send_json_err(conn, 500, "Out of memory");
            }
        }
    } else {
        send_json_err(conn, 500, "Query callback not set");
    }

    xsqlc_free(sql);
    if (gate_held) {
        gate_release(s);
    }
}

static void handle_cancel(xsql_http_server *s, struct mg_connection *conn) {
    // Only the whole-script executor receives ScriptOptions and can propagate
    // cancellation into an in-flight statement. The statement and legacy
    // callbacks can at most notice it between calls, so do not claim support.
    if (!s->script_executor) {
        send_json(
            conn, 409,
            "{\"success\":false,\"error\":\"cancellation is not supported by "
            "the configured executor\"}");
        return;
    }
    xsqlc_mtx_lock(s->cancel_mtx);
    ++s->cancel_epoch;
    xsqlc_mtx_unlock(s->cancel_mtx);
    send_json(conn, 200,
              "{\"success\":true,\"message\":\"cancel requested\"}");
}

static void shutdown_worker(void *arg) {
    xsql_http_server *s = (xsql_http_server *)arg;
    sleep_ms(100);
    xsql_http_server_stop(s);
    xsqlc_mtx_lock(s->latch_mtx);
    if (s->shutdown_inflight > 0) {
        --s->shutdown_inflight;
    }
    xsqlc_cond_broadcast(s->latch_cv);
    xsqlc_mtx_unlock(s->latch_mtx);
}

static void handle_shutdown(xsql_http_server *s, struct mg_connection *conn) {
    send_json(conn, 200, "{\"success\":true,\"message\":\"Shutting down\"}");
    xsqlc_mtx_lock(s->latch_mtx);
    ++s->shutdown_inflight;
    xsqlc_mtx_unlock(s->latch_mtx);
    if (xsqlc_thread_spawn_detached(shutdown_worker, s) != XSQL_OK) {
        // The worker that owed the decrement never started; undo it so free()'s
        // latch wait cannot hang.
        xsqlc_mtx_lock(s->latch_mtx);
        if (s->shutdown_inflight > 0) {
            --s->shutdown_inflight;
        }
        xsqlc_cond_broadcast(s->latch_cv);
        xsqlc_mtx_unlock(s->latch_mtx);
    }
}

static const xsql_http_route *match_route(xsql_http_server *s, const char *method,
                                          const char *path) {
    for (size_t i = 0; i < s->nroutes; ++i) {
        const xsql_http_route *r = &s->routes[i];
        if (r->method && r->path && strcmp(r->method, method) == 0 &&
            strcmp(r->path, path) == 0) {
            return r;
        }
    }
    return NULL;
}

static int dispatch_inner(struct mg_connection *conn, xsql_http_server *s) {
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *method = ri->request_method ? ri->request_method : "";
    const char *path = ri->local_uri ? ri->local_uri : ri->request_uri;
    if (!path) {
        path = "";
    }

    if (strcmp(path, "/") == 0 && strcmp(method, "GET") == 0) {
        handle_root(s, conn);
        return 1;
    }
    if (strcmp(path, "/help") == 0 && strcmp(method, "GET") == 0) {
        send_response(conn, 200, "text/plain", s->help_text ? s->help_text : "",
                      s->help_text ? strlen(s->help_text) : 0);
        return 1;
    }
    if (strcmp(path, "/query") == 0 && strcmp(method, "POST") == 0) {
        if (check_auth(s, conn)) {
            handle_query(s, conn, ri);
        }
        return 1;
    }
    if (strcmp(path, "/status") == 0 && strcmp(method, "GET") == 0) {
        if (!s->status_requires_auth || check_auth(s, conn)) {
            handle_status(s, conn);
        }
        return 1;
    }
    if (strcmp(path, "/cancel") == 0 && strcmp(method, "POST") == 0) {
        if (check_auth(s, conn)) {
            handle_cancel(s, conn);
        }
        return 1;
    }
    if (strcmp(path, "/shutdown") == 0 && strcmp(method, "POST") == 0) {
        if (check_auth(s, conn)) {
            handle_shutdown(s, conn);
        }
        return 1;
    }

    const xsql_http_route *route = match_route(s, method, path);
    if (route && route->handler) {
        if (!check_auth(s, conn)) {
            return 1;
        }
        size_t body_len = 0;
        bool body_oom = false;
        bool body_too_large = false;
        char *body = read_body(conn, ri, s->max_request_body_bytes, &body_len, &body_oom, &body_too_large);
        if (body_too_large) {
            send_json_err(conn, 413, "Request body too large");
            return 1;
        }
        if (body_oom) {
            send_json_err(conn, 500, "Out of memory");
            return 1;
        }
        xsql_http_response resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = 200;
        route->handler(body ? body : "", &resp, route->user);
        const char *ctype = resp.content_type ? resp.content_type
                            : (route->content_type ? route->content_type
                                                   : "text/plain");
        send_response(conn, resp.status, ctype, resp.body ? resp.body : "",
                      resp.body_len);
        response_reset(&resp);
        xsqlc_free(body);
        return 1;
    }

    send_json_err(conn, 404, "Not found");
    return 1;
}

// civetweb request entry point. Brackets dispatch_inner with the in-flight
// counter so stop() can wait for every handler (including a queued one just
// woken by drain) to finish writing before mg_stop() closes its socket.
static int dispatch(struct mg_connection *conn, void *cbdata) {
    xsql_http_server *s = (xsql_http_server *)cbdata;
    xsqlc_mtx_lock(s->inflight_mtx);
    ++s->inflight;
    xsqlc_mtx_unlock(s->inflight_mtx);

    int rc = dispatch_inner(conn, s);

    xsqlc_mtx_lock(s->inflight_mtx);
    --s->inflight;
    xsqlc_cond_broadcast(s->inflight_cv);
    xsqlc_mtx_unlock(s->inflight_mtx);
    return rc;
}

// ===========================================================================
// Lifecycle
// ===========================================================================

static void free_routes(xsql_http_server *s) {
    for (size_t i = 0; i < s->nroutes; ++i) {
        xsqlc_free((char *)s->routes[i].method);
        xsqlc_free((char *)s->routes[i].path);
        xsqlc_free((char *)s->routes[i].content_type);
    }
    xsqlc_free(s->routes);
    s->routes = NULL;
    s->nroutes = 0;
}

static void free_status_fields(xsql_http_server *s) {
    for (size_t i = 0; i < s->nstatus_fields; ++i) {
        xsqlc_free((char *)s->status_fields[i].key);
        xsqlc_free((char *)s->status_fields[i].value);
    }
    xsqlc_free(s->status_fields);
    s->status_fields = NULL;
    s->nstatus_fields = 0;
}

static void server_destroy(xsql_http_server *s) {
    free_routes(s);
    free_status_fields(s);
    xsqlc_free(s->tool_name);
    xsqlc_free(s->help_text);
    xsqlc_free(s->bind_address);
    xsqlc_free(s->auth_token);
    xsqlc_mtx_destroy(s->state_mtx);
    xsqlc_mtx_destroy(s->queue_mtx);
    xsqlc_cond_destroy(s->queue_cv);
    xsqlc_mtx_destroy(s->ser_mtx);
    xsqlc_cond_destroy(s->ser_cv);
    xsqlc_mtx_destroy(s->latch_mtx);
    xsqlc_cond_destroy(s->latch_cv);
    xsqlc_mtx_destroy(s->inflight_mtx);
    xsqlc_cond_destroy(s->inflight_cv);
    xsqlc_mtx_destroy(s->cancel_mtx);
    xsqlc_free(s);
}

// Copy a borrowed config string; returns false only on OOM (with *slot left NULL).
static bool copy_opt(char **slot, const char *s) {
    if (!s) {
        *slot = NULL;
        return true;
    }
    *slot = xsqlc_strdup(s);
    return *slot != NULL;
}

int xsql_http_server_new(const xsql_http_server_config *cfg,
                         xsql_http_server **out) {
    if (out) {
        *out = NULL;
    }
    if (!cfg || !out) {
        return XSQL_MISUSE;
    }
    // A whole-script executor has no main-thread queue path.
    if (cfg->script_executor && cfg->use_queue) {
        return XSQL_MISUSE;
    }

    xsql_http_server *s = (xsql_http_server *)xsqlc_malloc(sizeof(*s));
    if (!s) {
        return XSQL_NO_MEMORY;
    }
    memset(s, 0, sizeof(*s));

    s->port_config = cfg->port;
    s->allow_insecure_no_auth = cfg->allow_insecure_no_auth;
    s->script_executor = cfg->script_executor;
    s->script_executor_user = cfg->script_executor_user;
    s->statement_executor = cfg->statement_executor;
    s->statement_executor_user = cfg->statement_executor_user;
    s->query_fn = cfg->query_fn;
    s->query_fn_user = cfg->query_fn_user;
    s->status_fn = cfg->status_fn;
    s->status_fn_user = cfg->status_fn_user;
    s->queue_admission_timeout_ms =
        cfg->queue_admission_timeout_ms != 0 ? cfg->queue_admission_timeout_ms
                                             : 60000;
    s->max_queue = cfg->max_queue;
    s->queue_admission_timeout_fn = cfg->queue_admission_timeout_fn;
    s->queue_admission_timeout_fn_user = cfg->queue_admission_timeout_fn_user;
    s->max_queue_fn = cfg->max_queue_fn;
    s->max_queue_fn_user = cfg->max_queue_fn_user;
    s->use_queue = cfg->use_queue;
    s->serialize_requests = cfg->serialize_requests;
    s->status_requires_auth =
        cfg->struct_size >= offsetof(xsql_http_server_config,
                                     status_requires_auth) +
                                sizeof(cfg->status_requires_auth)
            ? cfg->status_requires_auth != XSQL_HTTP_STATUS_AUTH_PUBLIC
            : true;
    s->max_request_body_bytes =
        cfg->struct_size >= offsetof(xsql_http_server_config,
                                     max_request_body_bytes) +
                                sizeof(cfg->max_request_body_bytes)
            ? cfg->max_request_body_bytes
            : 0;
    s->interrupt_fn = NULL;

    bool ok = copy_opt(&s->tool_name, cfg->tool_name ? cfg->tool_name : "xsql") &&
              copy_opt(&s->help_text, cfg->help_text ? cfg->help_text : "") &&
              copy_opt(&s->bind_address,
                       cfg->bind_address ? cfg->bind_address : "127.0.0.1");
    if (ok && cfg->auth_token && cfg->auth_token[0]) {
        ok = copy_opt(&s->auth_token, cfg->auth_token);
    }

    if (ok && cfg->status_fields && cfg->status_fields_count > 0) {
        size_t bytes = 0;
        if (!xsqlc_mul_ok(
                cfg->status_fields_count, sizeof(*s->status_fields),
                &bytes)) {
            ok = false;
        } else {
            s->status_fields =
                (xsql_http_status_field *)xsqlc_malloc(bytes);
        }
        if (!s->status_fields) {
            ok = false;
        } else {
            memset(s->status_fields, 0,
                   cfg->status_fields_count * sizeof(*s->status_fields));
            s->nstatus_fields = cfg->status_fields_count;
            for (size_t i = 0; ok && i < cfg->status_fields_count; ++i) {
                const xsql_http_status_field *src = &cfg->status_fields[i];
                char *k = NULL, *v = NULL;
                ok = copy_opt(&k, src->key) && copy_opt(&v, src->value);
                s->status_fields[i].key = k;
                s->status_fields[i].value = v;
            }
        }
    }

    if (ok && cfg->extra_routes && cfg->extra_routes_count > 0) {
        size_t bytes = 0;
        if (!xsqlc_mul_ok(
                cfg->extra_routes_count, sizeof(*s->routes), &bytes)) {
            ok = false;
        } else {
            s->routes = (xsql_http_route *)xsqlc_malloc(bytes);
        }
        if (!s->routes) {
            ok = false;
        } else {
            memset(s->routes, 0, cfg->extra_routes_count * sizeof(*s->routes));
            s->nroutes = cfg->extra_routes_count;
            for (size_t i = 0; ok && i < cfg->extra_routes_count; ++i) {
                const xsql_http_route *src = &cfg->extra_routes[i];
                char *m = NULL, *p = NULL, *ct = NULL;
                ok = copy_opt(&m, src->method) && copy_opt(&p, src->path) &&
                     copy_opt(&ct, src->content_type);
                s->routes[i].method = m;
                s->routes[i].path = p;
                s->routes[i].content_type = ct;
                s->routes[i].handler = src->handler;
                s->routes[i].user = src->user;
            }
        }
    }

    if (ok) {
        ok = xsqlc_mtx_init(&s->state_mtx) == XSQL_OK &&
             xsqlc_mtx_init(&s->queue_mtx) == XSQL_OK &&
             xsqlc_cond_init(&s->queue_cv) == XSQL_OK &&
             xsqlc_mtx_init(&s->ser_mtx) == XSQL_OK &&
             xsqlc_cond_init(&s->ser_cv) == XSQL_OK &&
             xsqlc_mtx_init(&s->latch_mtx) == XSQL_OK &&
             xsqlc_cond_init(&s->latch_cv) == XSQL_OK &&
              xsqlc_mtx_init(&s->inflight_mtx) == XSQL_OK &&
              xsqlc_cond_init(&s->inflight_cv) == XSQL_OK &&
              xsqlc_mtx_init(&s->cancel_mtx) == XSQL_OK;
    }

    if (!ok) {
        server_destroy(s);
        return XSQL_NO_MEMORY;
    }

    *out = s;
    return XSQL_OK;
}

int xsql_http_server_start(xsql_http_server *s) {
    if (!s) {
        return -1;
    }
    xsqlc_mtx_lock(s->state_mtx);
    if (s->running) {
        int p = s->port;
        xsqlc_mtx_unlock(s->state_mtx);
        return p;
    }
    xsqlc_mtx_unlock(s->state_mtx);

    // Refuse a non-loopback bind without a token unless explicitly allowed.
    if (!is_loopback(s->bind_address) &&
        (!s->auth_token || s->auth_token[0] == '\0') &&
        !s->allow_insecure_no_auth) {
        return -1;
    }

    // Candidate ports: a single configured port, or a random scan of [8100,8999]
    // (a wide 900-slot band clear of the tools' fixed defaults, matching libxsql).
    int candidates[901];
    int ncand;
    if (s->port_config != 0) {
        candidates[0] = s->port_config;
        ncand = 1;
    } else {
        unsigned base =
            (unsigned)((mono_now_ms() ^ (long long)(size_t)s) & 0x7fffffff);
        for (int off = 0; off < 900; ++off) {
            candidates[off] = 8100 + (int)((base + (unsigned)off) % 900u);
        }
        // If the preferred band is exhausted, ask the kernel for any available
        // ephemeral port. CivetWeb performs the actual bind, so this has no
        // probe-then-bind race.
        candidates[900] = 0;
        ncand = 901;
    }

    for (int i = 0; i < ncand; ++i) {
        char ports[128];
        int need = snprintf(ports, sizeof(ports), "%s:%d", s->bind_address,
                            candidates[i]);
        if (need < 0 || (size_t)need >= sizeof(ports)) {
            // A caller-supplied bind_address too long for the buffer would be
            // silently truncated and bind to a wrong/garbage listening spec —
            // fail the bind instead. (Address is loop-invariant, so this fails
            // once for every candidate; -1 aborts start cleanly.)
            return -1;
        }
        const char *opts[] = {"listening_ports", ports, "num_threads", "32",
                              NULL};
        struct mg_context *ctx = mg_start(NULL, NULL, opts);
        if (!ctx) {
            continue;
        }
        int bound_port = candidates[i];
        if (bound_port == 0) {
            struct mg_server_port server_port;
            memset(&server_port, 0, sizeof(server_port));
            if (mg_get_server_ports(ctx, 1, &server_port) != 1 ||
                server_port.port <= 0) {
                mg_stop(ctx);
                continue;
            }
            bound_port = server_port.port;
        }
        mg_set_request_handler(ctx, "/", dispatch, s);
        xsqlc_mtx_lock(s->state_mtx);
        s->ctx = ctx;
        s->port = bound_port;
        s->running = true;
        xsqlc_mtx_unlock(s->state_mtx);
        return bound_port;
    }
    return -1;
}

void xsql_http_server_stop(xsql_http_server *s) {
    if (!s) {
        return;
    }
    xsqlc_mtx_lock(s->state_mtx);
    s->running = false;
    struct mg_context *ctx = s->ctx;
    s->ctx = NULL;
    s->port = 0;
    xsqlc_mtx_unlock(s->state_mtx);

    xsqlc_cond_broadcast(s->queue_cv);          // wake any owner-thread waiter
    drain_pending(s, "HTTP server stopped");    // fail queued commands

    // Let in-flight handlers finish flushing before civetweb tears their sockets
    // down, so a drained queued request delivers its shutdown envelope instead of
    // a bare connection drop. Bounded: a wedged user callback must not hang here
    // (mg_stop would block on that worker's join anyway).
    long long deadline = mono_now_ms() + 5000;
    xsqlc_mtx_lock(s->inflight_mtx);
    while (s->inflight > 0) {
        long long rem = deadline - mono_now_ms();
        if (rem <= 0) {
            break;
        }
        xsqlc_cond_wait_for(s->inflight_cv, s->inflight_mtx, (int)rem);
    }
    xsqlc_mtx_unlock(s->inflight_mtx);

    if (ctx) {
        mg_stop(ctx);  // blocks until civetweb workers drain
    }
}

void xsql_http_server_free(xsql_http_server *s) {
    if (!s) {
        return;
    }
    xsql_http_server_stop(s);
    // Wait (unbounded, deliberately) for any detached /shutdown thread to finish
    // touching our members before we free them — a diagnosable hang over a UAF.
    xsqlc_mtx_lock(s->latch_mtx);
    while (s->shutdown_inflight > 0) {
        xsqlc_cond_wait_for(s->latch_cv, s->latch_mtx, 0);
    }
    xsqlc_mtx_unlock(s->latch_mtx);
    server_destroy(s);
}

bool xsql_http_server_is_running(const xsql_http_server *s) {
    if (!s) {
        return false;
    }
    xsql_http_server *m = (xsql_http_server *)s;
    return server_running(m);
}

int xsql_http_server_port(const xsql_http_server *s) {
    if (!s) {
        return 0;
    }
    xsql_http_server *m = (xsql_http_server *)s;
    xsqlc_mtx_lock(m->state_mtx);
    int p = m->port;
    xsqlc_mtx_unlock(m->state_mtx);
    return p;
}

char *xsql_http_server_url(const xsql_http_server *s) {
    if (!s) {
        return NULL;
    }
    xsql_http_server *m = (xsql_http_server *)s;
    xsqlc_mtx_lock(m->state_mtx);
    int port = m->port;
    xsqlc_mtx_unlock(m->state_mtx);
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, "http://");
    xsqlc_sb_append(&sb, m->bind_address ? m->bind_address : "127.0.0.1");
    xsqlc_sb_putc(&sb, ':');
    xsqlc_sb_append_i64(&sb, port);
    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        return NULL;
    }
    return xsqlc_sb_detach(&sb);
}

void xsql_http_server_run_until_stopped(xsql_http_server *s) {
    if (!s) {
        return;
    }
    while (server_running(s)) {
        if (s->interrupt_fn && s->interrupt_fn(s->interrupt_user)) {
            xsql_http_server_stop(s);
            break;
        }
        process_one_internal(s, 100);
    }
}

bool xsql_http_server_process_one_command(xsql_http_server *s) {
    if (!s) {
        return false;
    }
    return process_one_internal(s, 0);
}

void xsql_http_server_set_interrupt_check(xsql_http_server *s,
                                          xsql_http_interrupt_fn fn, void *user) {
    if (!s) {
        return;
    }
    s->interrupt_fn = fn;
    s->interrupt_user = user;
}
