# Vendored civetweb

- **Upstream:** https://github.com/civetweb/civetweb
- **Version:** 1.16 (`CIVETWEB_VERSION "1.16"`)
- **License:** MIT (see `LICENSE.md`)
- **Retrieved:** 2026-07-05

## Why civetweb

libxsql-c is a pure-C library that must static-link into closed-source plugin
hosts (IDA / Binary Ninja). civetweb is a mature, MSVC-clean, permissively
(MIT) licensed HTTP server **and** client in one C unit. Alternatives were
rejected for licensing or fit: mongoose (GPLv2, incompatible with static-linking
into closed-source hosts), libmicrohttpd (LGPL friction, server-only, poor
Windows vendoring), and a hand-rolled HTTP/1.1 parser (an unnecessary
network-facing security surface to own).

civetweb is compiled **only** when `XSQLC_WITH_THINCLIENT=ON`, with features
trimmed to what the thin client uses: `NO_SSL NO_CGI NO_CACHING NO_FILES`, plus
`USE_IPV6` so a server can listen on, and the client dial, an IPv6 address. We also
set `SOCKET_TIMEOUT_QUANTUM=200` (upstream default 2000ms) so `mg_stop()` returns
promptly — the macro is `#ifndef`-guarded upstream, so this is a compile-define
override, **not** a source patch. The vendored `.c`/`.h`/`.inl` files are pristine.

## What is vendored

`src/civetweb.c` `#include`s a set of `.inl` translation-unit fragments. Only
the fragments reachable in the trimmed (no SSL / CGI / Lua / Duktape / zlib /
HTTP2 / timers / external-function) configuration are carried here:

| File | Role |
|---|---|
| `civetweb.c` | the amalgamated implementation |
| `civetweb.h` | public API header (upstream `include/civetweb.h`) |
| `md5.inl` | MD5 (unconditional include) |
| `sort.inl` | sort helper (unconditional include) |
| `match.inl` | URI pattern matcher (unconditional include) |
| `response.inl` | HTTP status-line strings (unconditional include) |
| `sha1.inl` | SHA-1 (WebSocket handshake path; unconditional include) |
| `handle_form.inl` | multipart/form parsing (unconditional include) |

The Lua/Duktape/mbedtls/wolfssl/openssl-dl/zlib/http2/timer `.inl` files and the
`external_*.inl` extension hooks are behind opt-in macros we never define, so
they are intentionally not vendored.

## Updating

1. Download the target release tarball from the upstream repo.
2. Replace `civetweb.c`, `civetweb.h`, and the `.inl` files above from
   `src/` and `include/`.
3. If a new upstream version adds an **unconditional** `#include "<x>.inl"` in
   `civetweb.c` (grep `#include "` and check the surrounding `#if` guards),
   vendor that file too and add a row above.
4. Rebuild with `-DXSQLC_WITH_THINCLIENT=ON` and run `/test-libxsql-c` +
   `/test-libxsql-c-asan`.
