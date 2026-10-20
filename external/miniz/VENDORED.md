# Vendored miniz

- **Upstream:** https://github.com/richgel999/miniz
- **Version:** 3.1.2 (`MZ_VERSION "11.3.2"`), from the release asset `miniz-3.1.2.zip`
- **License:** MIT (see `LICENSE`)
- **Retrieved:** 2026-09-28
- **SHA-256:** `miniz.c` `e2c1aeb66eef9191d8c3feb164db2def2335a61d039bf04ed849f6b042433b30`,
  `miniz.h` `b53b62ed122e559b8f679e3cb787a0b0035fe87a58f909da0e44931678f4e85f`,
  `LICENSE` `0115478d567121238cf6cc1c0c361926cf07a49d9e4c9e66da97fac6a01646b3`

## Why miniz

The thin-client HTTP server gzip-compresses a response when the client sends
`Accept-Encoding: gzip`, like the C++ libxsql server does through zlib. libxsql-c
is pure C and static-links into closed-source plugin hosts, so the codec has to
be permissively licensed, vendorable as plain source, and MSVC-clean. miniz is a
single `.c`/`.h` pair under MIT with a raw-deflate compressor (`tdefl`), an
inflater (`tinfl`) and `mz_crc32`, which is everything gzip framing needs. zlib
itself would add a multi-file build for the same result.

miniz's zlib-style `mz_deflateInit2` does not accept windowBits 31 (the gzip
wrapper), so the library writes the RFC 1952 header and trailer itself in
`src/thinclient/gzip.c` and uses `tdefl` for the raw deflate body at level 1.

## What is vendored

| File | Role |
|---|---|
| `miniz.c` | the amalgamated implementation, as released |
| `miniz.h` | its header, as released |
| `LICENSE` | upstream license |

All three are pristine copies of the release zip; nothing is patched. The
release's `examples/`, `readme.md` and `ChangeLog.md` are not carried.

## Build

`CMakeLists.txt` builds the `xsqlc_miniz` static target, only when
`XSQLC_WITH_GZIP=ON` (the default whenever `XSQLC_WITH_THINCLIENT=ON`). It keeps
miniz's own warning level, like the vendored sqlite3 and civetweb targets. These
PUBLIC defines trim it to the in-memory codec:

| Define | Effect |
|---|---|
| `MINIZ_NO_STDIO` | no file I/O |
| `MINIZ_NO_TIME` | no time functions |
| `MINIZ_NO_ARCHIVE_APIS` | no ZIP reader/writer |
| `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` | no `deflate`/`crc32`/... macros, so a host that also links zlib sees no clash |

The exported symbols keep miniz's own `mz_*` / `tdefl_*` / `tinfl_*` names. A
host that statically links a second copy of miniz should build libxsql-c with
`-DXSQLC_WITH_GZIP=OFF` (responses are then never compressed), or provide its
own `xsqlc_miniz` target before adding this directory. A shared libxsql-c build
hides these symbols (`--exclude-libs,ALL` on ELF; Windows exports only
`XSQL_API` symbols).

miniz picks its unaligned-access strategy from the target CPU. On x86/x64 the
3.1.2 default is `MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0` with memcpy-based
loads, which keeps UBSan's alignment check quiet.

## Updating

1. Download the target release zip from the upstream Releases page.
2. Replace `miniz.c`, `miniz.h` and `LICENSE` from it, and update the version,
   date and hashes above.
3. Check that the defines above still exist and still mean the same thing
   (grep `miniz.h` for `MINIZ_NO_`), and that `tdefl_compress_buffer`,
   `tdefl_create_comp_flags_from_zip_params`, `mz_crc32` and
   `tinfl_decompress_mem_to_heap` keep their signatures.
4. Rebuild with `-DXSQLC_WITH_THINCLIENT=ON` and run the functional suite plus
   the ASan/UBSan (gcc and clang) suite.
