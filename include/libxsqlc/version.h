// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// version.h — compile-time library version.
//
// The three integer components are the single source of truth for the version
// and are hardcoded here so this header compiles standalone (it has no
// dependency on any build-system-injected define). Keep them in lockstep with
// the CMake `project(libxsqlc VERSION ...)` line whenever the version is bumped.

#ifndef XSQLC_VERSION_H
#define XSQLC_VERSION_H

#define XSQL_C_VERSION_MAJOR 0
#define XSQL_C_VERSION_MINOR 0
#define XSQL_C_VERSION_PATCH 1

#define XSQL_C_VERSION_STRINGIZE_(x) #x
#define XSQL_C_VERSION_STRINGIZE(x)  XSQL_C_VERSION_STRINGIZE_(x)

#define XSQL_C_VERSION_STRING              \
    XSQL_C_VERSION_STRINGIZE(XSQL_C_VERSION_MAJOR) "." \
    XSQL_C_VERSION_STRINGIZE(XSQL_C_VERSION_MINOR) "." \
    XSQL_C_VERSION_STRINGIZE(XSQL_C_VERSION_PATCH)

// Single monotonically-comparable integer, e.g. 0.0.1 -> 1.
#define XSQL_C_VERSION_NUMBER \
    (XSQL_C_VERSION_MAJOR * 10000 + XSQL_C_VERSION_MINOR * 100 + XSQL_C_VERSION_PATCH)

#endif // XSQLC_VERSION_H
