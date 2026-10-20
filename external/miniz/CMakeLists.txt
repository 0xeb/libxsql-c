# Vendored miniz (MIT) — deflate/inflate + CRC-32 for gzip response compression.
# Built only when the parent enables XSQLC_WITH_GZIP (itself only meaningful with
# XSQLC_WITH_THINCLIENT). Trimmed to the in-memory codec; see VENDORED.md.
#
# Kept as its own target with miniz's own warning level: the xsqlc library stays
# -Werror / /WX, but the vendored source must not, exactly like the vendored
# sqlite3 and civetweb targets. Named xsqlc_miniz so it cannot collide with a
# `miniz` target an embedding project already has.

add_library(xsqlc_miniz STATIC miniz.c)

# Position-independent so it links into shared objects / plugins alongside xsqlc.
set_property(TARGET xsqlc_miniz PROPERTY POSITION_INDEPENDENT_CODE ON)

# Respect an MSVC runtime library choice from the parent (e.g. IDA SDK /MT hosts).
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    set_property(TARGET xsqlc_miniz PROPERTY MSVC_RUNTIME_LIBRARY ${CMAKE_MSVC_RUNTIME_LIBRARY})
endif()

target_include_directories(xsqlc_miniz PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# The feature trim is PUBLIC so every TU that includes miniz.h (the library's
# gzip framing and the private test suite's decoder) sees the same
# declarations the archive was built with. MINIZ_NO_ZLIB_COMPATIBLE_NAMES keeps
# miniz's zlib-named macros (deflate, crc32, ...) out, so a host that also links
# real zlib (as the C++ libxsql does) sees no clash.
target_compile_definitions(xsqlc_miniz PUBLIC
    MINIZ_NO_STDIO
    MINIZ_NO_TIME
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
