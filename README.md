# libxsql-c

Pure-C (C11) **libxsql** — an ergonomic framework for exposing
host-application data as SQLite virtual tables, plus scalar/aggregate
functions, bounded-memory script streaming/export, query timeouts,
transactional runtime settings, capability discovery, and an optional blocking
HTTP thin client.

libxsql-c is a fresh, idiomatic-C implementation against the same vendored
SQLite substrate — not a wrapper over any C++ library and with no C++ ABI
bridge. The C symbols are prefixed `xsql_` and the public headers live under
`include/libxsqlc/` so it can coexist with the C++ library in the same build.

- **CMake target:** `xsqlc` (alias `xsqlc::xsqlc`)
- **Public headers:** `include/libxsqlc/` — include `<libxsqlc/xsql.h>` for everything,
  or the specific header you need (`<libxsqlc/database.h>`, …) for faster compiles
- **Language:** strict C11 (`-std=c11 -pedantic-errors`, MSVC `/std:c11`),
  warnings-as-errors on MSVC, GCC, and Clang
- **License:** Human-Origin Source License v1.0

## Building

libxsql-c reads as a self-contained project three ways: standalone, as a
vendored subdirectory of another project, and via `add_subdirectory()`.

### Standalone

```bash
cmake -S . -B build
cmake --build build --config Release
```

Vendored SQLite is built automatically. If your outer project already provides
a `sqlite3` CMake target, libxsql-c uses it instead of adding a duplicate.

### Vendored into another CMake project

```cmake
add_subdirectory(third_party/libxsql-c)
target_link_libraries(your_target PRIVATE xsqlc::xsqlc)
```

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `XSQLC_WITH_THINCLIENT` | `OFF` | Build the HTTP thin-client layer; headers ship no-op stubs when off |
| `XSQLC_SHARED` | `OFF` | Build libxsql-c as a shared library (DLL/.so) instead of a static archive. Only the `XSQL_API` surface is exported; vendored SQLite/civetweb symbols are hidden. Release memory returned by the library with `xsql_free()` across the boundary |
| `XSQLC_BUILD_EXAMPLES` | on when standalone | Build the bundled examples |

## Conventions

A handful of rules hold across the whole surface:

- **Status codes.** Most functions return an `int` whose values equal the
  SQLite result codes (`XSQL_OK`, `XSQL_ROW`, `XSQL_DONE`, `XSQL_ERROR`, …; see
  `<libxsqlc/status.h>`). Detail text comes from `xsql_db_errmsg(db)`.
- **Ownership.** Every heap string the library returns (scalar results, all the
  formatters, `xsql_quote_identifier`, JSON helpers) is released with the single
  allocator boundary `xsql_free()`. Opaque handles have matching
  `_new`/`_free` (or `_open`/`_close`, `_finalize`) pairs; passing `NULL` to any
  destructor is a no-op.
- **Callbacks.** The C-native closure idiom is a function pointer plus a
  `void *user` argument (and, where ownership transfers, a
  `void (*destroy)(void *)`) — the same shape as `sqlite3_create_function_v2`.
- **No `sqlite3.h` in public headers.** The raw-handle escape hatches
  (`xsql_db_handle`, `xsql_stmt_handle`) return forward-declared pointers.

## API tour

### Open, execute, query

```c
#include <libxsqlc/database.h>
#include <libxsqlc/result.h>

xsql_db *db = NULL;
if (xsql_open_memory(&db) != XSQL_OK) { /* handle */ }

xsql_db_exec(db, "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT)");
xsql_db_exec(db, "INSERT INTO items(name) VALUES('alpha'),('beta')");

xsql_result *r = NULL;
if (xsql_db_query(db, "SELECT id, name FROM items ORDER BY id", &r) == XSQL_OK) {
    for (size_t i = 0; i < xsql_result_row_count(r); ++i) {
        const char *id   = xsql_result_cell(r, i, 0);
        const char *name = xsql_result_cell(r, i, 1);
        printf("%s = %s\n", id, name);
    }
}
xsql_result_free(r);

// One-cell convenience (heap string; free with xsql_free()):
char *n = xsql_db_scalar(db, "SELECT count(*) FROM items");
printf("%s rows\n", n);
xsql_free(n);

xsql_close(db);
```

Prepared statements (`<libxsqlc/statement.h>`) are available for bind/step/column
loops: `xsql_db_prepare` → `xsql_stmt_bind_*` → `xsql_stmt_step` →
`xsql_stmt_col_*` → `xsql_stmt_finalize`.

### Scalar and aggregate functions

A scalar function reads borrowed argument values and emits its result through a
context; the built-in `blob_concat` aggregate is auto-registered on open.

```c
#include <libxsqlc/value.h>

static void add2(xsql_context *ctx, int argc, xsql_value **argv, void *user) {
    (void)user;
    if (argc == 2)
        xsql_result_i64(ctx, xsql_value_i64(argv[0]) + xsql_value_i64(argv[1]));
    else
        xsql_result_error(ctx, "add2 expects 2 arguments");
}

xsql_db_create_function(db, "add2", 2, add2, /*user*/ NULL, /*destroy*/ NULL);
// ... SELECT add2(20, 22);  -> 42
```

`xsql_db_create_aggregate` registers a step + final pair; per-aggregation state
lives in the `xsql_aggregate_state(ctx)` slot.

### Virtual tables

Three table flavors expose host data as queryable tables. Each is built as a
**caller-owned definition** that must outlive every connection it is registered
into, then registered with one call.

**Index (live) tables** read rows fresh on every query by 0-based index, with
typed columns, optional writable columns / INSERT / DELETE, and equality
pushdown:

```c
#include <libxsqlc/table_index.h>

static const char *g_names[] = {"alpha", "beta", "gamma"};
static size_t names_count(void *u) { (void)u; return 3; }
static void   names_get(xsql_context *ctx, size_t row, void *u) {
    (void)u; xsql_result_text(ctx, g_names[row], -1, XSQL_TRANSIENT);
}

xsql_table *t = xsql_table_new("names");
xsql_table_count(t, names_count, NULL);
xsql_table_col_text(t, "name", names_get, NULL);
xsql_table_register(db, t);      // CREATE VIRTUAL TABLE names ...
// ... SELECT name FROM names;
xsql_table_free(t);              // after every db it was registered into closes
```

**Cached tables** (`<libxsqlc/table_cached.h>`) materialize rows once into a
contiguous cache of fixed-size records and reuse them across queries, with lazy
hash indexes (`xsql_cached_index_on`), constraint/prefix pushdown, a COUNT(\*)
fast path, and full UPDATE/DELETE/INSERT. A cache builder pushes records via
`xsql_rowset_push`:

```c
struct Row { int64_t id; };
static void build(xsql_rowset *rs, void *u) {
    (void)u;
    for (int64_t i = 1; i <= 3; ++i) { struct Row r = {i}; xsql_rowset_push(rs, &r); }
}
static int64_t get_id(const void *row, void *u) { (void)u; return ((const struct Row *)row)->id; }

xsql_cached_table *c = xsql_cached_table_new("ids", sizeof(struct Row));
xsql_cached_cache_builder(c, build, NULL);
xsql_cached_col_i64(c, "id", get_id, NULL);
xsql_cached_table_register(db, c);
xsql_cached_table_free(c);
```

**Generator tables** (`<libxsqlc/table_generator.h>`) are a streaming source that
never materializes, so `LIMIT` stops work early — you supply an
`xsql_generator` (`next`/`current`/`rowid`/`destroy`) via a factory, plus
hidden columns, parametric/constraint filters, and ORDER BY elision.
`xsql_generator_count` adds an opt-in exact `COUNT(*)` path that does not create
or walk a generator.

All three table flavors support a full transaction lifecycle through
`xsql_transaction_hooks`: connection-local state, prepare/commit/rollback, and
savepoint/release/rollback-to callbacks. Failed mutation callbacks still arm
rollback, while read-only statements and schema creation never fire commit.

### Scripts, export, and formatters

```c
#include <libxsqlc/script.h>
#include <libxsqlc/format.h>

xsql_script_result *sr = NULL;
xsql_db_run_script(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1); SELECT * FROM t;",
                   /*opts*/ NULL, &sr);

char *json = xsql_script_result_to_json(sr, /*include_sql*/ false);  // canonical wire JSON
char *text = xsql_script_result_to_text(sr);                          // human table
char *csv  = xsql_script_result_to_csv(sr);                           // RFC-4180
xsql_free(json); xsql_free(text); xsql_free(csv);
xsql_script_result_free(sr);

xsql_export_tables(db, /*names*/ NULL, /*count*/ 0, "dump.sql");       // SQL dump
```

`xsql_script_split` splits a script into statements; `xsql_print_table` renders
a standalone ASCII table. For large results,
`xsql_db_stream_script_json` and `xsql_db_stream_script_ndjson` step and emit
one read-only row at a time through an `xsql_stream_sink_fn`, keeping library
memory bounded to O(one row) for reads. Mutation `RETURNING` rows are buffered
until successful completion so failed or interrupted writes expose no
provisional output. Returning `false` from the sink stops immediately; timeouts
and cancellation retain the same partial-result metadata as the buffered
runner.

### Query timeouts and cooperative cancellation

A nonzero `xsql_query_opts::timeout_ms` installs a real, monotonic deadline
(enforced by a SQLite progress handler). A result-bearing statement keeps the
rows gathered so far and reports a partial timeout; a non-result statement
reports a hard error.

```c
xsql_query_opts opts = { sizeof(opts), /*timeout_ms*/ 50, /*progress_steps*/ 0 };
xsql_result *r = NULL;
xsql_db_query_opts(db, "SELECT ... /* long scan */", &opts, &r);
if (xsql_result_timed_out(r)) {
    if (xsql_result_partial(r)) { /* use the rows we got */ }
    else                        { /* hard timeout: xsql_result_errmsg(r) */ }
}
xsql_result_free(r);
```

Long native virtual-table scans poll `xsql_vtab_interrupted()` so they bail at
the same deadline; embedders can install their own predicate with
`xsql_vtab_set_interrupt_checker`.

### Runtime settings

`<libxsqlc/settings.h>` provides a thread-safe typed settings registry (common
and product-specific boolean, integer, and string values), a bounded scoped
timeout stack, and the canonical transactional
`runtime_settings(key,value,type,scope)` virtual table. Value-carrying PRAGMAs
are retired; only the action verbs `timeout_push` and `timeout_pop` remain.
The original fixed-size reply/enumeration APIs remain ABI-stable; use the
corresponding `_v2` entry points for string values up to 127 bytes and product
scopes up to 63 bytes.

```c
xsql_settings *s = xsql_settings_new(NULL);          // NULL = all defaults
xsql_settings_set_query_timeout_ms(s, 30000);        // validated setter (returns bool)
xsql_settings_register_string(
    s, "product.mode", "safe", "product", /*writable*/ true);
xsql_runtime_settings_table *rt =
    xsql_runtime_settings_table_new(s, "product");
xsql_runtime_settings_table_register(db, rt);
// UPDATE runtime_settings SET value='fast' WHERE key='product.mode';
int effective = 0;
xsql_settings_timeout_push(s, 5000, &effective);     // scoped override
xsql_settings_snap snap;
xsql_settings_snapshot(s, &snap);                    // atomic value snapshot
xsql_runtime_settings_table_free(rt);                // after the db closes
xsql_settings_free(s);
```

`<libxsqlc/capabilities.h>` builds the one canonical read-only discovery table,
`sql_capabilities(name,is_supported,notes)`. Construction deep-copies and sorts
rows and rejects empty or duplicate names.

### Optional HTTP thin client

An optional HTTP thin-client layer ships behind `-DXSQLC_WITH_THINCLIENT=ON`
(off by default). When the feature is off, the thin-client headers compile to
no-op stubs so consumer code builds unchanged either way. The transport is a
vendored civetweb (MIT); no transport type appears in any public header.

`#include <libxsqlc/thinclient.h>` pulls in the whole surface:

- **`http_server.h`** — an opaque `xsql_http_server` serving six endpoints
  (`GET /`, `GET /help`, `POST /query`, `GET /status`, `POST /cancel`,
  `POST /shutdown`).
  Configure via `xsql_http_server_config` (zero-init for defaults): pick one of
  three query seams — a whole-script `script_executor`, a single-statement
  `statement_executor` (the server owns multi-statement orchestration and
  formatting), or a legacy `query_fn` — plus an optional `status_fn`, custom
  `extra_routes`, an `auth_token` (`X-XSQL-Token` or `Authorization: Bearer`),
  and either a main-thread request queue (`use_queue`, driven by
  `xsql_http_server_run_until_stopped`) or a one-at-a-time serialize gate.
  When a token is configured, `/status` requires it by default; set
  `status_requires_auth = XSQL_HTTP_STATUS_AUTH_PUBLIC` only when an
  unauthenticated liveness endpoint is intentional.
  `POST /query` accepts a raw-SQL or JSON body and a `?format=json|text|csv|tsv`
  selector. Queue admission failures use exact HTTP statuses (503 full/stopped,
  408 timeout), and the admission deadline ends once execution starts. Port `0`
  tries `[8100,8999]`, then atomically asks the kernel for an available
  ephemeral port if that band is exhausted.
- **`http_client.h`** — `xsql_http_client` with `query` / `status` / `cancel` /
  `shutdown` / `ping` (response bodies are heap strings freed with `xsql_free`).
- **`thinclient_cli.h`** — `xsql_parse_cli_args` for direct / serve / client
  command-line front-ends.
- **`clipboard.h`** — host/URL normalization, HTTP/MCP clipboard payload
  builders, server-output parsing (including the `extract_http`/`mcp_start_endpoint`
  composites), and a clipboard copy (Windows + macOS `pbcopy`; no-op elsewhere).
- **`thinclient_json.h`** — small JSON response builders (`xsql_json_escape`,
  `xsql_make_error_json`, `xsql_make_success_json`, `xsql_make_status_json`).
- Server-info banners `xsql_format_http_info` / `xsql_format_http_status`, and a
  static `status_fields` map on the server config (merged into `/status` under
  `status_fn`).

## License and Terms of Use

In short: you may read, build, evaluate, benchmark, package, and use unmodified libxsql-c, including commercially, if you preserve notices and follow the license terms. You may fork or patch it to prepare bug fixes, optimizations, features, tests, or documentation improvements for contribution back within the license's contribution-purpose rules.

You may not maintain a divergent private fork, port, rebrand, clone, API-compatible replacement, competing implementation, or use libxsql-c as AI input to recreate or improve a derivative implementation without prior written permission from Elias Bachaalany. Independent implementations that are not copied from, materially derived from, or substantially informed by libxsql-c in the license's defined sense are not prohibited.

Permission requests: open a GitHub issue at [0xeb/libxsql-c/issues](https://github.com/0xeb/libxsql-c/issues).

If libxsql-c materially informs a distributed project, preserve the human origin: credit libxsql-c and Elias Bachaalany visibly in your README/docs and in About/credits UI when applicable. The license includes an examples/FAQ section for common allowed and permission-required uses. Third-party dependencies (vendored SQLite (public domain) and civetweb (MIT)) remain under their own licenses.

See the full [Human-Origin Source License v1.0](LICENSE).
