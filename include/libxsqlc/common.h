// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

// common.h — shared export macro, basic typedefs, and the single allocator
// boundary (xsql_free). Every public header includes this and nothing heavier.

#ifndef XSQLC_COMMON_H
#define XSQLC_COMMON_H

#include <stddef.h>

// ---------------------------------------------------------------------------
// Export / visibility macro
//
// libxsql-c ships as a static library by default (XSQL_API expands to nothing).
// The macro is in place so a future shared-library build can define XSQL_SHARED
// (and, while building the library itself, XSQL_BUILD) without touching every
// declaration. Consumers never define anything.
// ---------------------------------------------------------------------------
#if defined(XSQL_SHARED)
#  if defined(_WIN32)
#    if defined(XSQL_BUILD)
#      define XSQL_API __declspec(dllexport)
#    else
#      define XSQL_API __declspec(dllimport)
#    endif
#  else
#    if defined(XSQL_BUILD)
#      define XSQL_API __attribute__((visibility("default")))
#    else
#      define XSQL_API
#    endif
#  endif
#else
#  define XSQL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Release memory returned by a libxsql-c API that documents its result as
// "heap; free with xsql_free". Never call the C runtime free() on such pointers
// directly — the library's allocator may differ across a DLL boundary. Passing
// NULL is a no-op.
XSQL_API void xsql_free(void *ptr);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XSQLC_COMMON_H
