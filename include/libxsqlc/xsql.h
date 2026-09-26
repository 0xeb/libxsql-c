// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// xsql.h — convenience umbrella that pulls in the whole public surface.
//
// This header is a courtesy for consumers who want everything with one include.
// It is NEVER required and is NEVER included by any other library header or
// compilation unit — the library is include-what-you-use internally. Include
// the specific <libxsqlc/*.h> you need for the fastest compiles.

#ifndef XSQLC_XSQL_H
#define XSQLC_XSQL_H

#include <libxsqlc/common.h>
#include <libxsqlc/version.h>
#include <libxsqlc/status.h>
#include <libxsqlc/value.h>
#include <libxsqlc/statement.h>
#include <libxsqlc/result.h>
#include <libxsqlc/database.h>
#include <libxsqlc/settings.h>
#include <libxsqlc/capabilities.h>
#include <libxsqlc/script.h>
#include <libxsqlc/format.h>
#include <libxsqlc/graph.h>
#include <libxsqlc/json.h>
#include <libxsqlc/regex.h>
#include <libxsqlc/vtab.h>
#include <libxsqlc/table_index.h>
#include <libxsqlc/table_cached.h>
#include <libxsqlc/table_generator.h>

#endif // XSQLC_XSQL_H
