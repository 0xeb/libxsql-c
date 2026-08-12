// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.
//
// thinclient.h — umbrella header pulling in the whole thin-client surface (HTTP
// server + client, CLI parsing, clipboard helpers). Each sub-header is
// self-contained and
// feature-gates itself on XSQL_HAS_THINCLIENT, so this compiles either way.

#ifndef XSQLC_THINCLIENT_H
#define XSQLC_THINCLIENT_H

#include <libxsqlc/http_server.h>
#include <libxsqlc/http_client.h>
#include <libxsqlc/thinclient_cli.h>
#include <libxsqlc/thinclient_json.h>
#include <libxsqlc/clipboard.h>

#endif // XSQLC_THINCLIENT_H
