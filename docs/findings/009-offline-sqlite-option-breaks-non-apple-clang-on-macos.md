---
id: 009
title: MORPH_BUILD_OFFLINE_SQLITE=ON breaks every TU in the target on macOS with a non-Apple clang -- FindSQLite3 resolves the SDK's /usr/include and it is injected ahead of libc++
subsystem: backend
severity: minor
source: lims rung 6, build order §7 (enabling the durable queue for the first time)
disposition: open
test: spec-cited (repro below reproduces on the repo's own morph_offline_sqlite_tests target)
---

Turning on the durable offline queue makes the target fail to compile on
macOS whenever the compiler is not Apple's clang. It is not specific to the
ladder: the repo's **own** `morph_offline_sqlite_tests` target fails
identically.

## Repro

```
cmake -S . -B build/strict -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt6 \
  -DMORPH_BUILD_QT=ON -DMORPH_BUILD_TESTS=ON -DMORPH_BUILD_OFFLINE_SQLITE=ON
cmake --build build/strict --target morph_offline_sqlite_tests
```

Homebrew clang 22.1.8, macOS 26.0 (Darwin 25.6.0), 20 errors:

```
.../MacOSX.sdk/usr/include/_types.h:46:9: error: unknown type name '__uint32_t'
.../MacOSX.sdk/usr/include/arm/_types.h:75:9: error: unknown type name 'ptrdiff_t'
.../include/c++/v1/cstddef:45:5: error: <cstddef> tried including <stddef.h>
    but didn't find libc++'s <stddef.h> header. This usually means that your
    header search paths are not configured properly. The header search paths
    should contain the C++ Standard Library headers before any C Standard
    Library ...
```

## Cause

CMake's bundled `FindSQLite3` finds `sqlite3.h` in the macOS SDK, so the
cache ends up with

```
SQLite3_INCLUDE_DIR:PATH=/Applications/Xcode.app/.../MacOSX.sdk/usr/include
```

— the SDK's *entire* C include directory, not a sqlite-private prefix. The
root `CMakeLists.txt` then puts that on `SQLite3::SQLite3`'s
`INTERFACE_INCLUDE_DIRECTORIES`, which reaches the compile line as
`-isystem`, ahead of libc++'s own headers. Every `<cstddef>`/`<cctype>`/
`<cwctype>` in the TU then resolves to the C SDK's version and libc++ refuses
to proceed. Apple clang is immune because it knows the SDK layout and orders
those paths itself.

This is a property of the header *directory*, not of sqlite: any
`find_package` whose result is a bare SDK `/usr/include` would do the same.
It is invisible in CI because no CI leg enables the option on macOS.

## Workaround

Point the cache at a private prefix instead:

```
-DSQLite3_INCLUDE_DIR=/opt/homebrew/opt/sqlite/include \
-DSQLite3_LIBRARY=/opt/homebrew/opt/sqlite/lib/libsqlite3.dylib
```

With that, `morph_offline_sqlite_tests` builds and passes (40 assertions in
10 test cases), and so does the ladder's own durable-queue leg.

## What should happen

In the `if(MORPH_BUILD_OFFLINE_SQLITE)` block of the root `CMakeLists.txt`,
drop an include directory from the imported target when it is a bare SDK/system
include root — the compiler finds `sqlite3.h` there with no help, and adding
it can only reorder the search path harmfully. Something like: skip
`INTERFACE_INCLUDE_DIRECTORIES` when `SQLite3_INCLUDE_DIRS` matches
`CMAKE_OSX_SYSROOT`/`/usr/include`. The same guard would protect any other
`find_package` in the tree that can resolve to a system include root.

Alternatively, document the two `-D` overrides above in the option's own
comment, which currently says only that SQLite3 must be present.
