#
# Copyright (c) 2024-2026 Elias Bachaalany
# SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
#
# This file is licensed under the Human-Origin Source License v1.0.
# See LICENSE.
#

cmake_minimum_required(VERSION 3.20)

# Enable CMAKE_MSVC_RUNTIME_LIBRARY support (required for e.g. IDA SDK /MT hosts).
cmake_policy(SET CMP0091 NEW)

project(libxsqlc VERSION 0.0.3 LANGUAGES C)

# Detect standalone vs. embedded (vendored subdirectory / larger project).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(XSQLC_STANDALONE ON)
else()
    set(XSQLC_STANDALONE OFF)
endif()

option(XSQLC_WITH_THINCLIENT "Build thin client support (HTTP server/client)" OFF)
# gzip response compression (vendored miniz). Defaults ON with the thin client,
# which is the only thing that uses it; forced OFF without it.
include(CMakeDependentOption)
cmake_dependent_option(XSQLC_WITH_GZIP
    "Compress thin-client HTTP responses with gzip when the client accepts it (vendored miniz)"
    ON "XSQLC_WITH_THINCLIENT" OFF)
option(XSQLC_BUILD_EXAMPLES  "Build libxsql-c examples" ${XSQLC_STANDALONE})
option(XSQLC_SHARED          "Build libxsql-c as a shared library (DLL/.so)" OFF)
option(XSQLC_TEST_ALLOC_HOOKS "Build private allocator failure hooks for tests" OFF)

if(XSQLC_SHARED)
    set(XSQLC_LIB_TYPE SHARED)
else()
    set(XSQLC_LIB_TYPE STATIC)
endif()

# ============================================================================
# SQLite (vendored amalgamation)
#
# Honor an outer project that already provides a `sqlite3` target so a consumer
# can vendor both libxsql (C++) and libxsql-c without a duplicate target.
# ============================================================================

if(NOT TARGET sqlite3)
    add_subdirectory(external/sqlite)
endif()

# ============================================================================
# libxsql-c
# ============================================================================

add_library(xsqlc ${XSQLC_LIB_TYPE}
    src/status.c
    src/value.c
    src/graph.c
    src/statement.c
    src/database.c
    src/result.c
    src/settings.c
    src/runtime_settings_table.c
    src/capabilities.c
    src/function.c
    src/aggregate.c
    src/script.c
    src/script_stream.c
    src/format.c
    src/printer.c
    src/export.c
    src/json_emit.c
    src/json_public.c
    src/regex.c
    src/vtab_core.c
    src/transaction.c
    src/table_index.c
    src/table_cached.c
    src/table_generator.c
    src/shared_cache.c
    src/internal/strbuf.c
    src/internal/hashmap.c
)
add_library(xsqlc::xsqlc ALIAS xsqlc)

target_include_directories(xsqlc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# sqlite3 is an implementation detail: PRIVATE so sqlite3.h never leaks to
# consumers (public headers use forward declarations for raw-handle hatches).
target_link_libraries(xsqlc PRIVATE sqlite3)

# Strict C11 everywhere (no compiler extensions).
set_target_properties(xsqlc PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON   # linkable into shared objects / plugins
)

# Runtime version string for internal sanity checks (the public version.h is
# self-contained and does not depend on this).
target_compile_definitions(xsqlc PRIVATE XSQL_C_BUILD_VERSION="${PROJECT_VERSION}")
if(XSQLC_TEST_ALLOC_HOOKS)
    target_compile_definitions(xsqlc PRIVATE XSQLC_TEST_ALLOC_HOOKS)
endif()

# Shared-library export wiring. The XSQL_API macro (common.h) becomes
# dllexport/import (Windows) or default-visibility (ELF) when XSQL_SHARED is
# defined; XSQL_BUILD (PRIVATE) selects export while building the library. Hidden
# default visibility keeps the vendored sqlite3/civetweb symbols (linked PRIVATE)
# out of the .so/.dll export table.
if(XSQLC_SHARED)
    target_compile_definitions(xsqlc PUBLIC XSQL_SHARED PRIVATE XSQL_BUILD)
    set_target_properties(xsqlc PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # C_VISIBILITY_PRESET only governs xsqlc's own objects; the statically-linked
    # sqlite3/civetweb/miniz keep default (public) visibility and would be re-exported.
    # On GNU-ld/gold, hide every symbol coming from a static archive so only the
    # XSQL_API surface is exported (Windows already exports only dllexport'd
    # symbols, so it needs no equivalent).
    if(NOT WIN32 AND NOT APPLE)
        target_link_options(xsqlc PRIVATE -Wl,--exclude-libs,ALL)
    endif()
endif()

# Warnings-as-errors on our code only (NOT the vendored sqlite3 target).
if(MSVC)
    target_compile_options(xsqlc PRIVATE /W4 /WX)
else()
    target_compile_options(xsqlc PRIVATE
        -Wall -Wextra -Wpedantic -Werror -pedantic-errors)
endif()

# ============================================================================
# Thin client (optional HTTP server/client) — feature-gated.
#
# When ON, compile the thin-client sources + the mutex/cond thread shim and link
# the vendored civetweb (MIT) transport PRIVATE so civetweb.h never reaches
# consumers. When OFF, the public headers ship static-inline no-op stubs, so
# consumer code compiles unchanged (no sources needed).
# ============================================================================

if(XSQLC_WITH_THINCLIENT)
    if(NOT TARGET civetweb)
        add_subdirectory(external/civetweb)
    endif()

    target_sources(xsqlc PRIVATE
        src/thinclient/http_server.c
        src/thinclient/stream_coalescer.c
        src/thinclient/client.c
        src/thinclient/cli.c
        src/thinclient/json_parse.c
        src/thinclient/json_helpers.c
        src/thinclient/merge_patch.c
        src/thinclient/banner.c
        src/thinclient/clipboard.c
        src/internal/thread_shim.c
    )
    target_link_libraries(xsqlc PRIVATE civetweb)
    if(WIN32)
        target_link_libraries(xsqlc PRIVATE ws2_32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(xsqlc PRIVATE Threads::Threads)
    endif()

    target_compile_definitions(xsqlc PUBLIC XSQL_HAS_THINCLIENT)

    # gzip responses: raw deflate from the vendored miniz (MIT), with the gzip
    # framing in src/thinclient/gzip.c. PRIVATE like civetweb; the define is
    # private too, because no public API changes with it.
    if(XSQLC_WITH_GZIP)
        if(NOT TARGET xsqlc_miniz)
            add_subdirectory(external/miniz)
        endif()
        target_sources(xsqlc PRIVATE src/thinclient/gzip.c)
        target_link_libraries(xsqlc PRIVATE xsqlc_miniz)
        target_compile_definitions(xsqlc PRIVATE XSQLC_HAS_GZIP)
    endif()
endif()

# ============================================================================
# Examples (optional)
# ============================================================================

if(XSQLC_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

# ============================================================================
# Install / export (standalone builds only)
# ============================================================================

if(XSQLC_STANDALONE)
    include(GNUInstallDirs)

    set(XSQLC_INSTALL_TARGETS xsqlc)
    # A STATIC xsqlc does not absorb its PRIVATE sqlite3/civetweb deps, so a
    # consumer of the installed archive must link them too — export them. A SHARED
    # xsqlc absorbs (and hides) them, so they must NOT be exported.
    if(NOT XSQLC_SHARED)
        # Only install/export sqlite3 when this project owns it (added above).
        if(TARGET sqlite3)
            get_target_property(_xsqlc_sqlite_alias sqlite3 ALIASED_TARGET)
            if(NOT _xsqlc_sqlite_alias)
                list(APPEND XSQLC_INSTALL_TARGETS sqlite3)
            endif()
        endif()
        # civetweb exists only when the thin client was enabled.
        if(TARGET civetweb)
            get_target_property(_xsqlc_civetweb_alias civetweb ALIASED_TARGET)
            if(NOT _xsqlc_civetweb_alias)
                list(APPEND XSQLC_INSTALL_TARGETS civetweb)
            endif()
        endif()
        # xsqlc_miniz exists only when gzip was enabled.
        if(TARGET xsqlc_miniz)
            list(APPEND XSQLC_INSTALL_TARGETS xsqlc_miniz)
        endif()
    endif()

    install(TARGETS ${XSQLC_INSTALL_TARGETS}
        EXPORT xsqlcTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(EXPORT xsqlcTargets
        FILE xsqlcConfig.cmake
        NAMESPACE xsqlc::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xsqlc
    )
endif()
