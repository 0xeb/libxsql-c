// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// settings_internal.h — atomic staged-settings helpers for the SQL table.
// PRIVATE: never installed.

#ifndef XSQLC_INTERNAL_SETTINGS_INTERNAL_H
#define XSQLC_INTERNAL_SETTINGS_INTERNAL_H

#include <libxsqlc/settings.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct xsqlc_setting_update {
    char key[64];
    char value[128];
} xsqlc_setting_update;

bool xsqlc_settings_begin_staged_query_timeout(xsql_settings *settings);
void xsqlc_settings_discard_staged_query_timeout(xsql_settings *settings);
void xsqlc_settings_commit_staged(
    xsql_settings *settings, const xsqlc_setting_update *updates,
    size_t count, bool owns_staged_query_timeout);

#endif  // XSQLC_INTERNAL_SETTINGS_INTERNAL_H
