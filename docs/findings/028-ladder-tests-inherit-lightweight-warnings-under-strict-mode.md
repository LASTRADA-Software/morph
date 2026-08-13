---
id: 028
title: "`ladder_<rung>_tests` applies `-Weverything -Werror` to Lightweight/unixodbc headers it deliberately spares `ladder_<rung>_lib`, so `MORPH_ENABLE_STRICT_COMPILATION=ON` fails on any DB-touching rung, unrelated to that rung's own code"
subsystem: core
severity: blocker
source: rung 2 (bookmarks) task 13 — CMakeLists.txt completing the buildable rung skeleton
disposition: fixed
test: spec-cited (repro below is a real `cmake --build` under `-DMORPH_ENABLE_STRICT_COMPILATION=ON`)
---

`cmake/morph_add_rung.cmake` deliberately does **not** call `apply_warnings()`
on `ladder_${_rung}_lib` (line ~123-124):

```cmake
# Lightweight's headers are not -Werror clean (bank's own caveat,
# examples/bank/CMakeLists.txt) — no apply_warnings() here.
```

But `ladder_${_rung}_tests` (line 408) calls `apply_warnings()`
unconditionally, and `ladder_${_rung}_tests` PRIVATE-links
`ladder_${_rung}_lib`, which PUBLIC-links `Lightweight::Lightweight`
(line 120: `target_link_libraries(ladder_${_rung}_lib PUBLIC morph::morph
Lightweight::Lightweight Qt6::Core)`). Lightweight's own include directories
propagate into `ladder_${_rung}_tests` as plain `-I`, not `-isystem` (unlike
Qt/glaze/reflection-cpp, which the same target already gets via `-isystem` —
confirmed by inspecting the generated compile command), so the *lib* target's
carve-out is silently defeated for the *tests* target, which is exactly the
target the carve-out's own comment says needs it.

## Repro

Any test file in a DB-touching rung that transitively includes a Lightweight
header (directly, or via that rung's own `db/*_entity.hpp`) fails to compile
under strict mode with dozens of unrelated diagnostics from Lightweight's own
sources and from `<sql.h>`/`<sqltypes.h>`/`<sqlext.h>` (unixodbc):
`-Wreserved-macro-identifier`, `-Wswitch-default`, `-Wold-style-cast`,
`-Wcast-qual`, `-Wshadow`, `-Wshadow-field-in-constructor`,
`-Wmissing-variable-declarations`, and more — none of it in morph or rung
code.

```
cmake -S . -B build/strict -DMORPH_ENABLE_STRICT_COMPILATION=ON \
    -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=pastebin -DMORPH_BUILD_QT=ON
cmake --build build/strict --target ladder_pastebin_tests
# fails compiling test_paste_model.cpp on Lightweight/unixodbc header
# diagnostics before reaching a single line of pastebin's own code.
```

Confirmed on **both** `pastebin` (rung 1, merged long ago) and `bookmarks`
(rung 2, this task) — this is not new, not rung-2-specific, and would have
been present the moment rung 1's `CMakeLists.txt` landed. The local build
tree used throughout the ladder's development
(`build/clang-coverage`) has `MORPH_ENABLE_STRICT_COMPILATION=OFF`, which is
why no earlier task's real build hit it.

## What should happen instead

`Lightweight`'s (and unixodbc's) include directories reaching
`ladder_${_rung}_tests` should be marked `-isystem`, e.g.
`target_include_directories(... SYSTEM ...)` on the `Lightweight::Lightweight`
import, or an explicit `SYSTEM` re-declaration of those dirs on
`ladder_${_rung}_lib`'s PUBLIC interface — matching how Qt/glaze/reflection-cpp
are already treated in the very same target. A two-directory `cmake/` change,
not a rung's to make unilaterally (shared file, used by every rung).

## Consequence for rung 2 while this is open

Task 13's own designated-field-initializer fix (43 warnings across 5 test
files, see the task's report) is real and independently verified clean, but a
*fully* clean `-DMORPH_ENABLE_STRICT_COMPILATION=ON` build of
`ladder_bookmarks_tests` cannot be reached end-to-end via the normal
`cmake --build` flow until this is fixed — the build fails on Lightweight's
own headers first. Verification for task 13 was done per-translation-unit
with the compiler invoked directly (from the real, unmodified compile
commands) with the affected include paths remapped to `-isystem` to isolate
the check to the rung's own code, rather than by a strict-mode
`cmake --build` of the whole target.
