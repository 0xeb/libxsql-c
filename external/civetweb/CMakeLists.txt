# Vendored civetweb (MIT) — HTTP server + client for the thin client.
# Built only when the parent enables XSQLC_WITH_THINCLIENT. Features are trimmed
# to what the thin client uses (no SSL/CGI/caching/file-serving); see VENDORED.md.
#
# Kept as its own target with civetweb's own (relaxed) warning level: the xsqlc
# library stays -Werror / /WX, but the vendored amalgamation must not, exactly
# like the vendored sqlite3 target.

add_library(civetweb STATIC civetweb.c)

# Position-independent so it links into shared objects / plugins alongside xsqlc.
set_property(TARGET civetweb PROPERTY POSITION_INDEPENDENT_CODE ON)

# Respect an MSVC runtime library choice from the parent (e.g. IDA SDK /MT hosts).
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    set_property(TARGET civetweb PROPERTY MSVC_RUNTIME_LIBRARY ${CMAKE_MSVC_RUNTIME_LIBRARY})
endif()

# The header sits next to civetweb.c; expose the directory both for civetweb.c's
# own `#include "civetweb.h"` and for the thin-client .c files that use it. This
# stays PUBLIC on the civetweb target but the xsqlc link is PRIVATE, so
# civetweb.h never reaches libxsql-c consumers.
target_include_directories(civetweb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# Trim to the used feature set (mirrors the design doc + VENDORED.md).
# SOCKET_TIMEOUT_QUANTUM is civetweb's poll quantum (default 2000ms) — the master
# thread only notices a stop request between quanta, so mg_stop() can take up to
# that long. 200ms keeps server teardown snappy (it is #ifndef-guarded upstream,
# so this override needs no source patch).
target_compile_definitions(civetweb PRIVATE
    NO_SSL
    NO_CGI
    NO_CACHING
    NO_FILES
    USE_STACK_SIZE=131072
    SOCKET_TIMEOUT_QUANTUM=200)

if(WIN32)
    target_link_libraries(civetweb PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(civetweb PRIVATE Threads::Threads)
endif()
