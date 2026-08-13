---
id: 029
title: "`-Wthread-safety-negative` fires on plain, unannotated `std::mutex` use in `core/executor.hpp`/`core/completion.hpp` under Clang 22 (Homebrew, macOS libc++), independent of any rung"
subsystem: core
severity: major
source: rung 2 (bookmarks) task 13 — CMakeLists.txt completing the buildable rung skeleton
disposition: open
test: spec-cited (repro below is a real `cmake --build` under `-DMORPH_ENABLE_STRICT_COMPILATION=ON`)
issue: https://github.com/LASTRADA-Software/morph/issues/64
---

Building any target that includes `include/morph/core/executor.hpp` or
`include/morph/core/completion.hpp` under `-DMORPH_ENABLE_STRICT_COMPILATION=ON`
with the local Clang 22 toolchain (`/opt/homebrew/opt/llvm@22`, its bundled
libc++) fails with:

```
include/morph/core/executor.hpp:87:32: error: acquiring mutex '_m' requires
  negative capability '!_m' [-Werror,-Wthread-safety-negative]
        std::scoped_lock const lock{_m};
                               ^
```

Neither `ThreadPoolExecutor::_m` (`executor.hpp`) nor
`CompletionState::mtx` (`completion.hpp`) carries any
`GUARDED_BY`/`ACQUIRE`/thread-safety attribute — they are plain
`std::mutex` members locked with plain `std::scoped_lock`/`std::unique_lock`.
Clang's thread-safety analysis normally only fires on code that opts in via
annotations; this Clang/libc++ pairing appears to have grown thread-safety
annotations on `std::mutex` itself (a recent LLVM libc++ change), so *every*
plain, unannotated use of `std::mutex` project-wide now trips
`-Wthread-safety-negative` once `-Weverything -Werror` is both active — which
they are unconditionally the moment `MORPH_ENABLE_STRICT_COMPILATION=ON` is
set (`-Weverything` itself is always on via `apply_warnings()`; strict mode
only adds `-Werror`).

## Scope

Not rung-specific — `core/executor.hpp` and `core/completion.hpp` are
included transitively by nearly every morph target. Confirmed by building
`ladder_bookmarks_tests` under strict mode: this is the *first* class of
error encountered, before Lightweight's own headers are even reached (see
finding 028). Whether CI's pinned `clang-22` (via `apt.llvm.org` on Ubuntu,
paired with a different libc++/libstdc++) reproduces this is unconfirmed from
this rung — it may be macOS/Homebrew-libc++-specific, in which case CI is
unaffected and this finding is a local-toolchain-only concern; if CI does use
the same libc++ that ships these annotations, `MORPH_ENABLE_STRICT_COMPILATION=ON`
(CI's stated default) would fail on framework code alone, on every target,
independent of any rung.

## What should happen instead

Either annotate the affected mutexes properly (`GUARDED_BY`, etc.) so the
analysis has real capability information to reason about, or suppress
`-Wthread-safety-negative` specifically (with a comment citing this finding)
in `cmake/compiler_options.cmake`'s Clang suppression block alongside the
other named exceptions already there. Not a rung's file to change — shared,
used by every target in the repo.

## Consequence for rung 2 while this is open

Task 13's strict-compilation verification of the bookmarks rung's own test
files (43 designated-field-initializer fixes) was done with
`-Wno-thread-safety-negative` added to the per-translation-unit check, to
isolate the verification to code this task actually owns. See finding 028
for the second, larger obstacle (Lightweight/unixodbc headers) hit on the
same path.
