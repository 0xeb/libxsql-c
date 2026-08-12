// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// thinclient_cli.h — argument parsing for direct/serve/client CLI front-ends.
// The parser fills a heap-owned args struct; the caller renders help/version and
// drives the chosen mode. Feature-gated with the rest of the thin client.

#ifndef XSQLC_THINCLIENT_CLI_H
#define XSQLC_THINCLIENT_CLI_H

#include <libxsqlc/common.h>
#include <libxsqlc/status.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum xsql_cli_mode {
    XSQL_CLI_DIRECT,  // -s db -c query (open, query, close)
    XSQL_CLI_SERVE,   // -s db --serve --port N (open, listen)
    XSQL_CLI_CLIENT   // --port N -c query (connect to a running server)
} xsql_cli_mode;

// Parsed arguments. All strings are heap (xsqlc); release the whole struct with
// xsql_cli_args_free.
typedef struct xsql_cli_args {
    xsql_cli_mode mode;
    char *database;       // -s / --source (or a positional)
    char *command;        // -c / --command
    char *file;           // -f / --file
    char *output_format;  // -o / --output (default "csv")
    char *bind_address;   // --bind (default "127.0.0.1")
    int   port;           // --port (default 5555)
    bool  serve;          // --serve
    bool  help;           // -h / --help
    bool  version;        // --version
} xsql_cli_args;

#ifdef XSQL_HAS_THINCLIENT

// Parse argv[1..argc-1]. On --help/--version, returns XSQL_OK with the matching
// flag set (no mode validation). Otherwise detects the mode and validates it;
// on a bad option or an invalid combination returns XSQL_ERROR and, when `err`
// is non-NULL, stores a heap diagnostic in *err (free with xsql_free). On success
// returns XSQL_OK with *out set (free with xsql_cli_args_free); *out is NULL on
// any failure.
XSQL_API int  xsql_parse_cli_args(int argc, char *const *argv,
                                  xsql_cli_args **out, char **err);
// Release parsed args. Passing NULL is a no-op.
XSQL_API void xsql_cli_args_free(xsql_cli_args *a);
// Resolve the SQL to execute: the -c command, else the contents of the -f file.
// On success returns XSQL_OK with a heap string in *out (NULL when neither is
// set). Returns XSQL_CANT_OPEN if the file cannot be read.
XSQL_API int  xsql_cli_args_sql(const xsql_cli_args *a, char **out);

#else  // !XSQL_HAS_THINCLIENT

static inline int xsql_parse_cli_args(int argc, char *const *argv,
                                      xsql_cli_args **out, char **err) {
    (void)argc; (void)argv;
    if (out) {
        *out = NULL;
    }
    if (err) {
        *err = NULL;
    }
    return XSQL_ERROR;
}
static inline void xsql_cli_args_free(xsql_cli_args *a) { (void)a; }
static inline int  xsql_cli_args_sql(const xsql_cli_args *a, char **out) {
    (void)a;
    if (out) {
        *out = NULL;
    }
    return XSQL_ERROR;
}

#endif  // XSQL_HAS_THINCLIENT

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_THINCLIENT_CLI_H
