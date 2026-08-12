// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// http_server.h — the consolidated thin-client HTTP query server. Serves the
// standard 6 endpoints (GET /, GET /help, POST /query, GET /status,
// POST /cancel, POST /shutdown) over the vendored civetweb transport, which is an internal
// implementation detail — no civetweb type appears in this header.
//
// Feature-gated: real behavior when the library is built with XSQL_HAS_THINCLIENT
// (CMake -DXSQLC_WITH_THINCLIENT=ON). When OFF, the same consumer code still
// compiles: the types below stay declared and every function becomes a
// static-inline no-op (a _new returns XSQL_ERROR, "thin client not enabled").
//
// Callbacks follow the library idiom: a function pointer plus a trailing
// `void *user` context; anything a callback would return by value is instead
// emitted through a borrowed out-sink handle (never a raw char* crossing
// the xsql_free allocator boundary).

#ifndef XSQLC_HTTP_SERVER_H
#define XSQLC_HTTP_SERVER_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>
#include <libxsqlc/script.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opaque handles (declared unconditionally so consumer code compiles ON and OFF)
// ---------------------------------------------------------------------------

// The server instance. Create with xsql_http_server_new; release with
// xsql_http_server_free (which stops it first).
typedef struct xsql_http_server xsql_http_server;

// Response out-sink, borrowed inside a query_fn / route handler. Fill it with the
// setters below; do not retain it past the callback's return.
typedef struct xsql_http_response xsql_http_response;

// Status out-sink, borrowed inside a status_fn. Emit exactly one JSON object.
typedef struct xsql_http_status_out xsql_http_status_out;

// ---------------------------------------------------------------------------
// Callback typedefs
//
// Fault containment (the C analog of a catch/unwind boundary): a callback
// MUST return normally — it must not longjmp / throw / unwind across the boundary
// and must not retain a borrowed out-sink past its return. The server contains a
// well-behaved-but-failing callback structurally (no lock is held across a
// callback; a failed request never breaks the accept loop) and, when a callback
// signals failure without a message, synthesizes the standard error envelope.
// A genuine process-fatal fault (SIGSEGV/abort) inside a callback is the caller's
// bug.
// ---------------------------------------------------------------------------

// Legacy path: "SQL string in, JSON string out". Emit the JSON result with
// xsql_http_response_set_body(out, json, len, NULL). Lowest precedence.
typedef void (*xsql_http_query_fn)(const char *sql, xsql_http_response *out,
                                   void *user);

// Preferred queued/main-thread path: a single-statement executor. IDENTICAL to
// xsql_script_executor_fn — the server owns multi-statement orchestration
// (xsql_run_script), option parsing, and formatting. Middle precedence.
// (config field type is xsql_script_executor_fn.)

// Whole-script executor: the server parses options + format and hands the entire
// script to this callback, which owns orchestration and TRANSFERS an envelope
// through *out (the server frees it with xsql_script_result_free). Return XSQL_OK
// on success. Highest precedence; incompatible with use_queue (rejected at _new).
typedef int (*xsql_http_script_executor_fn)(const char *script,
                                            const xsql_script_opts *opts,
                                            xsql_script_result **out, void *user);

// GET /status extra fields: emit one JSON object; the server RFC 7386
// merge-patches it onto {success,status:"ok",tool}.
typedef void (*xsql_http_status_fn)(xsql_http_status_out *out, void *user);

// A custom route handler (see xsql_http_route). Receives the request body; fills
// the response sink (body + content_type + status).
typedef void (*xsql_http_route_fn)(const char *body, xsql_http_response *out,
                                   void *user);

// Dynamic overrides consulted per request (typically read an xsql_settings via
// `user`). Return the current admission timeout (ms) / max queue depth.
typedef int    (*xsql_http_timeout_fn)(void *user);
typedef size_t (*xsql_http_max_queue_fn)(void *user);

// Interrupt check polled by run_until_stopped; return true to stop the server.
typedef bool (*xsql_http_interrupt_fn)(void *user);

// ---------------------------------------------------------------------------
// Extra routes (transport-neutral; declarative)
//
// A declarative alternative to handing out a raw underlying server object:
// reproducing that in C would force the transport's header (civetweb.h) into
// this public header, so custom routes are registered as data instead.
// ---------------------------------------------------------------------------

typedef struct xsql_http_route {
    const char        *method;        // "GET" / "POST" (borrowed; copied at _new)
    const char        *path;          // "/decompile"   (borrowed; copied at _new)
    const char        *content_type;  // default content-type (borrowed; copied)
    xsql_http_route_fn handler;
    void              *user;
} xsql_http_route;

// A static string field merged into GET /status (see status_fields). Both strings
// are borrowed and deep-copied at xsql_http_server_new.
typedef struct xsql_http_status_field {
    const char *key;
    const char *value;
} xsql_http_status_field;

// GET /status authentication policy. A zero-initialized or legacy-size config
// uses the secure default: require the configured token. PUBLIC is the only
// explicit opt-out (and has no effect when no token is configured).
#define XSQL_HTTP_STATUS_AUTH_DEFAULT  0u
#define XSQL_HTTP_STATUS_AUTH_REQUIRED 1u
#define XSQL_HTTP_STATUS_AUTH_PUBLIC   2u

// ---------------------------------------------------------------------------
// Server configuration
//
// Designed for designated initializers; set struct_size to sizeof(...) (or 0 for
// all defaults). All borrowed strings and the extra_routes array are DEEP-COPIED
// at xsql_http_server_new, so the caller need not keep them alive.
// ---------------------------------------------------------------------------

typedef struct xsql_http_server_config {
    size_t struct_size;                 // sizeof(xsql_http_server_config), or 0

    const char *tool_name;              // shown in responses; uppercased for GET /
    const char *help_text;              // GET /help body
    const char *bind_address;           // NULL => "127.0.0.1"
    int         port;                   // 0 => [8100,8999], then OS ephemeral
    const char *auth_token;             // NULL/"" => no auth required
    bool        allow_insecure_no_auth; // permit a non-loopback bind without a token

    // Executor precedence: script_executor > statement_executor > query_fn.
    xsql_http_script_executor_fn script_executor;
    void                        *script_executor_user;
    xsql_script_executor_fn      statement_executor;
    void                        *statement_executor_user;
    xsql_http_query_fn           query_fn;
    void                        *query_fn_user;

    // GET /status extra fields. Static string fields (status_fields) are merged
    // onto the base {success,status,tool} object first; status_fn's JSON object
    // is then RFC 7386 merge-patched on top (so status_fn overrides status_fields).
    const xsql_http_status_field *status_fields;  // array (copied at _new)
    size_t                        status_fields_count;
    xsql_http_status_fn status_fn;
    void               *status_fn_user;

    const xsql_http_route *extra_routes;        // array (copied at _new)
    size_t                 extra_routes_count;

    int    queue_admission_timeout_ms;  // 0 => wait forever (default 60000)
    size_t max_queue;                   // 0 => unbounded
    xsql_http_timeout_fn   queue_admission_timeout_fn;
    void                  *queue_admission_timeout_fn_user;
    xsql_http_max_queue_fn max_queue_fn;
    void                  *max_queue_fn_user;

    bool use_queue;           // owner-thread affinity via the request queue
    bool serialize_requests;  // one-at-a-time gate (ignored when use_queue)

    // ABI-growth field using XSQL_HTTP_STATUS_AUTH_* values. Zero and REQUIRED
    // require the configured token; only PUBLIC exposes status as a liveness
    // endpoint. This size_t starts beyond the complete legacy structure rather
    // than reusing its tail padding.
    size_t status_requires_auth;

    // ABI-growth field: maximum accepted request-body size in bytes. A request
    // whose declared Content-Length (or streamed length) exceeds this is rejected
    // with 413 before the body is read. 0 => the built-in default (64 MiB),
    // matching the C++ http_query_server_config::max_request_body_bytes.
    size_t max_request_body_bytes;
} xsql_http_server_config;

// ---------------------------------------------------------------------------
// Out-sink setters (valid only inside the owning callback; contents are copied
// into library memory)
// ---------------------------------------------------------------------------

#ifdef XSQL_HAS_THINCLIENT

// Set the response body (`len` bytes; embedded NUL safe) and content type
// (NULL => "text/plain"). Replaces any previous body.
XSQL_API void xsql_http_response_set_body(xsql_http_response *out,
                                          const char *body, size_t len,
                                          const char *content_type);
// Override the HTTP status code (default 200).
XSQL_API void xsql_http_response_set_status(xsql_http_response *out, int status);
// Provide the status-fn JSON object (a "{...}" string) to merge-patch.
XSQL_API void xsql_http_status_out_set_json(xsql_http_status_out *out,
                                            const char *json_object);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Validate `cfg` and construct a server. Deep-copies strings/routes. Rejects the
// script_executor + use_queue combination with XSQL_MISUSE (the whole-script
// executor has no main-thread queue path). Returns XSQL_OK (with *out set),
// XSQL_MISUSE, or XSQL_NO_MEMORY; *out is NULL on any failure.
XSQL_API int  xsql_http_server_new(const xsql_http_server_config *cfg,
                                   xsql_http_server **out);
// Stop (if running) and free. Passing NULL is a no-op.
XSQL_API void xsql_http_server_free(xsql_http_server *s);

// Start serving. Returns the bound port ([8100,8999], with an OS-assigned
// ephemeral fallback, when config.port was 0), or -1 on failure. Idempotent: a
// second call on a running server returns the current port.
XSQL_API int  xsql_http_server_start(xsql_http_server *s);
// Stop gracefully. Idempotent and safe to call concurrently.
XSQL_API void xsql_http_server_stop(xsql_http_server *s);
XSQL_API bool xsql_http_server_is_running(const xsql_http_server *s);
XSQL_API int  xsql_http_server_port(const xsql_http_server *s);
// "http://<bind>:<port>" — heap; free with xsql_free(); NULL on allocation fail.
XSQL_API char *xsql_http_server_url(const xsql_http_server *s);

// Queue mode (use_queue=true): drive queued commands on the owner thread.
XSQL_API void xsql_http_server_run_until_stopped(xsql_http_server *s);
// Process one queued command if available; returns true if one ran.
XSQL_API bool xsql_http_server_process_one_command(xsql_http_server *s);
// Set an interrupt check polled during run_until_stopped (NULL clears it).
XSQL_API void xsql_http_server_set_interrupt_check(xsql_http_server *s,
                                                   xsql_http_interrupt_fn fn,
                                                   void *user);

// ---------------------------------------------------------------------------
// Banner formatters (standalone; no server handle needed) — heap strings freed
// with xsql_free().
// ---------------------------------------------------------------------------

// A multi-line "HTTP server started on port N / URL / endpoints / example / hint"
// banner. bind_address NULL => "127.0.0.1"; stop_hint NULL =>
// "Press Ctrl+C to stop and return to REPL.". NULL only on allocation failure.
XSQL_API char *xsql_format_http_info(int port, const char *bind_address,
                                     const char *stop_hint);

// A short running/not-running status banner. bind_address NULL => "127.0.0.1".
XSQL_API char *xsql_format_http_status(int port, bool running,
                                       const char *bind_address);

#else  // !XSQL_HAS_THINCLIENT — no-op stubs so consumer code still compiles

static inline void xsql_http_response_set_body(xsql_http_response *out,
                                               const char *body, size_t len,
                                               const char *content_type) {
    (void)out; (void)body; (void)len; (void)content_type;
}
static inline void xsql_http_response_set_status(xsql_http_response *out,
                                                 int status) {
    (void)out; (void)status;
}
static inline void xsql_http_status_out_set_json(xsql_http_status_out *out,
                                                 const char *json_object) {
    (void)out; (void)json_object;
}
static inline int xsql_http_server_new(const xsql_http_server_config *cfg,
                                       xsql_http_server **out) {
    (void)cfg;
    if (out) {
        *out = NULL;
    }
    return XSQL_ERROR;  // thin client not enabled
}
static inline void xsql_http_server_free(xsql_http_server *s) { (void)s; }
static inline int  xsql_http_server_start(xsql_http_server *s) { (void)s; return -1; }
static inline void xsql_http_server_stop(xsql_http_server *s) { (void)s; }
static inline bool xsql_http_server_is_running(const xsql_http_server *s) {
    (void)s;
    return false;
}
static inline int  xsql_http_server_port(const xsql_http_server *s) { (void)s; return 0; }
static inline char *xsql_http_server_url(const xsql_http_server *s) { (void)s; return NULL; }
static inline void xsql_http_server_run_until_stopped(xsql_http_server *s) { (void)s; }
static inline bool xsql_http_server_process_one_command(xsql_http_server *s) {
    (void)s;
    return false;
}
static inline void xsql_http_server_set_interrupt_check(xsql_http_server *s,
                                                        xsql_http_interrupt_fn fn,
                                                        void *user) {
    (void)s; (void)fn; (void)user;
}
static inline char *xsql_format_http_info(int port, const char *bind_address,
                                          const char *stop_hint) {
    (void)port; (void)bind_address; (void)stop_hint;
    return NULL;
}
static inline char *xsql_format_http_status(int port, bool running,
                                            const char *bind_address) {
    (void)port; (void)running; (void)bind_address;
    return NULL;
}

#endif  // XSQL_HAS_THINCLIENT

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_HTTP_SERVER_H
