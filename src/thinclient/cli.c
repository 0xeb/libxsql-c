// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// cli.c — direct/serve/client argument parsing.
// Fills a heap args struct; the caller renders help/version and drives the mode.

#include "libxsqlc/thinclient_cli.h"

#include "internal/internal.h"
#include "internal/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char **err, const char *a, const char *b) {
    if (!err) {
        return;
    }
    xsqlc_strbuf sb;
    xsqlc_sb_init(&sb);
    xsqlc_sb_append(&sb, a);
    if (b) {
        xsqlc_sb_append(&sb, b);
    }
    if (xsqlc_sb_oom(&sb)) {
        xsqlc_sb_free(&sb);
        *err = NULL;
        return;
    }
    *err = xsqlc_sb_detach(&sb);
}

// Replace *slot with a fresh copy of s (freeing the old). Returns false on OOM.
static bool set_str(char **slot, const char *s) {
    char *dup = xsqlc_strdup(s);
    if (!dup) {
        return false;
    }
    xsqlc_free(*slot);
    *slot = dup;
    return true;
}

void xsql_cli_args_free(xsql_cli_args *a) {
    if (!a) {
        return;
    }
    xsqlc_free(a->database);
    xsqlc_free(a->command);
    xsqlc_free(a->file);
    xsqlc_free(a->output_format);
    xsqlc_free(a->bind_address);
    xsqlc_free(a);
}

static xsql_cli_mode detect_mode(const xsql_cli_args *a) {
    if (a->serve) {
        return XSQL_CLI_SERVE;
    }
    const bool no_db = !a->database || a->database[0] == '\0';
    const bool has_query = (a->command && a->command[0]) || (a->file && a->file[0]);
    if (no_db && a->port != 5555) {
        return XSQL_CLI_CLIENT;
    }
    if (no_db && has_query) {
        return XSQL_CLI_CLIENT;
    }
    return XSQL_CLI_DIRECT;
}

static bool validate(const xsql_cli_args *a, char **err) {
    const bool no_db = !a->database || a->database[0] == '\0';
    const bool no_query =
        (!a->command || a->command[0] == '\0') && (!a->file || a->file[0] == '\0');
    switch (a->mode) {
        case XSQL_CLI_DIRECT:
            if (no_db) {
                set_err(err, "No database specified. Use -s <database>", NULL);
                return false;
            }
            if (no_query) {
                set_err(err, "No query specified. Use -c <query> or -f <file>", NULL);
                return false;
            }
            break;
        case XSQL_CLI_SERVE:
            if (no_db) {
                set_err(err, "No database specified for serve mode. Use -s <database>",
                        NULL);
                return false;
            }
            break;
        case XSQL_CLI_CLIENT:
            if (no_query) {
                set_err(err,
                        "No query specified for client mode. Use -c <query> or -f <file>",
                        NULL);
                return false;
            }
            break;
    }
    return true;
}

int xsql_parse_cli_args(int argc, char *const *argv, xsql_cli_args **out,
                        char **err) {
    if (out) {
        *out = NULL;
    }
    if (err) {
        *err = NULL;
    }
    if (!out) {
        return XSQL_MISUSE;
    }

    xsql_cli_args *a = (xsql_cli_args *)xsqlc_malloc(sizeof(*a));
    if (!a) {
        return XSQL_NO_MEMORY;
    }
    memset(a, 0, sizeof(*a));
    a->mode = XSQL_CLI_DIRECT;
    a->port = 5555;
    // Defaults the args struct initializes.
    if (!set_str(&a->output_format, "csv") || !set_str(&a->bind_address, "127.0.0.1")) {
        xsql_cli_args_free(a);
        return XSQL_NO_MEMORY;
    }

#define NEED_ARG(flag)                                                   \
    do {                                                                 \
        if (++i >= argc) {                                               \
            set_err(err, "Missing argument for ", flag);                 \
            xsql_cli_args_free(a);                                       \
            return XSQL_ERROR;                                           \
        }                                                                \
    } while (0)
#define ASSIGN(slot)                                                     \
    do {                                                                 \
        if (!set_str(slot, argv[i])) {                                   \
            xsql_cli_args_free(a);                                       \
            return XSQL_NO_MEMORY;                                       \
        }                                                                \
    } while (0)

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            a->help = true;
        } else if (strcmp(arg, "--version") == 0) {
            a->version = true;
        } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--source") == 0) {
            NEED_ARG(arg);
            ASSIGN(&a->database);
        } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--command") == 0) {
            NEED_ARG(arg);
            ASSIGN(&a->command);
        } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--file") == 0) {
            NEED_ARG(arg);
            ASSIGN(&a->file);
        } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            NEED_ARG(arg);
            ASSIGN(&a->output_format);
        } else if (strcmp(arg, "--serve") == 0) {
            a->serve = true;
        } else if (strcmp(arg, "--port") == 0) {
            NEED_ARG(arg);
            char *endp = NULL;
            long p = strtol(argv[i], &endp, 10);
            if (endp == argv[i] || *endp != '\0' || p <= 0 || p > 65535) {
                set_err(err, "Invalid port number: ", argv[i]);
                xsql_cli_args_free(a);
                return XSQL_ERROR;
            }
            a->port = (int)p;
        } else if (strcmp(arg, "--bind") == 0) {
            NEED_ARG(arg);
            ASSIGN(&a->bind_address);
        } else if (arg[0] == '-') {
            set_err(err, "Unknown option: ", arg);
            xsql_cli_args_free(a);
            return XSQL_ERROR;
        } else {
            // Positional -> database if not yet set.
            if (!a->database || a->database[0] == '\0') {
                ASSIGN(&a->database);
            } else {
                set_err(err, "Unexpected argument: ", arg);
                xsql_cli_args_free(a);
                return XSQL_ERROR;
            }
        }
    }

#undef NEED_ARG
#undef ASSIGN

    // --help / --version short-circuit before mode detection + validation.
    if (a->help || a->version) {
        *out = a;
        return XSQL_OK;
    }

    a->mode = detect_mode(a);
    if (!validate(a, err)) {
        xsql_cli_args_free(a);
        return XSQL_ERROR;
    }

    *out = a;
    return XSQL_OK;
}

int xsql_cli_args_sql(const xsql_cli_args *a, char **out) {
    if (out) {
        *out = NULL;
    }
    if (!a || !out) {
        return XSQL_MISUSE;
    }
    if (a->command && a->command[0]) {
        char *dup = xsqlc_strdup(a->command);
        if (!dup) {
            return XSQL_NO_MEMORY;
        }
        *out = dup;
        return XSQL_OK;
    }
    if (a->file && a->file[0]) {
        FILE *f = fopen(a->file, "rb");
        if (!f) {
            return XSQL_CANT_OPEN;
        }
        xsqlc_strbuf sb;
        xsqlc_sb_init(&sb);
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            xsqlc_sb_append_len(&sb, buf, n);
        }
        fclose(f);
        if (xsqlc_sb_oom(&sb)) {
            xsqlc_sb_free(&sb);
            return XSQL_NO_MEMORY;
        }
        *out = xsqlc_sb_detach(&sb);
        return *out ? XSQL_OK : XSQL_NO_MEMORY;
    }
    return XSQL_OK;  // neither -c nor -f: no SQL (NULL)
}
