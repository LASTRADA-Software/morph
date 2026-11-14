# A compiled library for morph's UI layer

`morph::morph` is a header-only `INTERFACE` target. Every translation unit that reaches the UI layer re-parses and
re-checks the whole interpreter, document model and service code. That cost is now the largest single item in a build,
and it is what makes CI's GitHub-hosted runners run out of memory. This design moves the non-template code of the UI
layer into compiled sources, so it is paid once per build instead of once per translation unit. It then makes the
library buildable as a shared library, as a configure-time choice.

## Why

Measured on master `4e99bac6f`, clang 23.1.4, with the `clang-coverage` preset's flags. Each row is peak front-end
memory for a file containing one `#include` and nothing else:

```
   109 MB  <string>
   338 MB  <glaze/glaze.hpp>
   632 MB  <morph/core/bridge.hpp>
   726 MB  <morph/ui/document.hpp>
  1818 MB  <morph/ui/interpreter.hpp>
  1306 MB  <morph/ui/service.hpp>
  2291 MB  <morph/client/source.hpp>
```

`examples/lims/screens/tests/test_lims_ignition_screen.cpp` peaks at 3432 MB, and its `#include` lines alone take
3106 MB. 74 of 596 objects in a `clang-coverage` build depend on `interpreter.hpp`. A hosted runner has 16 GB and
4 vCPUs. Linux legs die with exit 143 or "The hosted runner lost communication with the server", with no compiler
error. Issue #1118 records the measurements.

`interpreter.hpp` is 11,077 lines with 174 `inline` functions and 3 templates. `document.hpp` is 5,648 lines with 85
`inline` functions and no templates. Most of that code is not generic.

## Goals

- `morph::morph` becomes a compiled library. Its non-template UI code lives in `src/ui/*.cpp`.
- Including `<morph/ui/interpreter.hpp>` alone peaks well under 1 GB in the front end, measured as above.
- `test_lims_ignition_screen.cpp` peaks under 2 GB in a `clang-coverage` compile.
- Every CI leg passes on GitHub-hosted runners, gcc-release included, with no cap on build parallelism beyond one
  compile per CPU. Under `g++ -O3 -DNDEBUG` the UI test files peak at 4.7-5.7 GB each today
  (`tests/test_ui_shell_parts.cpp` 5657 MB, `test_ui_workspace.cpp` 5598 MB, `test_ui_author.cpp` 5356 MB,
  `test_ui_host.cpp` 5253 MB, `test_ui_interpreter_{commands,detail,control}.cpp` about 4675 MB). Four at once must fit
  in 16 GB, so each must come down below about 3.5 GB.
- `-DMORPH_BUILD_SHARED=ON` builds `morph` as a shared library that works on Linux and Windows (part B).
- No behaviour changes. The public API, its headers and their include paths stay as they are.

## Non-goals

- Templates stay in headers. `author.hpp` (100 templates), `forms/forms.hpp` (111), `core/bridge.hpp` (41) and the
  template parts of `mount.hpp` and `view.hpp` are not split.
- No ABI promise. A shared `morph` may only be loaded by code built against the same version, and its SONAME carries
  the full version so a mismatch fails at load.
- `morph_client`, `morph_tui`, `morph_qt_impl` and `morph_qt_quick` stay `STATIC` in both modes.

## Part A: the static library

### What may move

Code moves out of a header only when its meaning does not depend on the including translation unit. That excludes:

1. **Templates and `constexpr`/`consteval` functions.**
2. **Code whose meaning depends on a per-target macro, directly or through an include.** `MORPH_CLIENT_ONLY`,
   `MORPH_REQUIRE_VETTED_HMAC`, `MORPH_TRACY_ENABLED` and `NDEBUG` change what `core/bridge.hpp`,
   `core/registry.hpp`, `core/effects.hpp`, `core/records.hpp`, `core/profiler.hpp`,
   `core/detail/owner_probe.hpp`, `session/session_auth.hpp` and `journal/action_log_json.hpp` compile to.
   `morph_tests` undefines `MORPH_REQUIRE_VETTED_HMAC`, and the `morph_client_only_*` probes build with
   `MORPH_CLIENT_ONLY`. None of the UI headers below `#if` on these macros. Eleven of them include headers that do. A
   moved body that calls into, or depends on the layout of, macro-sensitive code is compiled once with the library's
   settings, so the lane checks each such call. Today only default arguments differ, which is harmless. A difference in
   behaviour or layout is a stop-and-report finding.
3. **Code whose types come from the includer.**

Everything else in the UI headers may move: free `inline` functions, member functions defined in a class body, and
`detail` helpers. A `detail` helper moves into the `.cpp`'s unnamed namespace only if a tree-wide search (`include`,
`src`, `examples`, `tests`) finds no other user. `theme.hpp`'s `detail::channelOf` (used by `interpreter.hpp`),
`detail::hexColor` (used by `src/tui/display_widgets.cpp`) and `detail::numberOf` (used by `message.hpp`) are known
cases. They keep a declaration in the header and get an out-of-line definition.

A function that moves keeps its declaration, and its Doxygen block, in the header, with `MORPH_API` (below) on it. A
class with any moved member carries `MORPH_API` on the class. In part A `MORPH_API` expands to nothing. Putting it on
now means part B does not touch every moved declaration a second time.

Moving bodies removes code from what includers see. A header that compiled only because a moved body pulled in an
include breaks. Each lane builds every target, Qt, Qt Quick and TUI included, after each header.

The 13 headers in scope have no function-local statics and no `inline` variables, so a static library neither drops
nor duplicates any object they define.

### Build and packaging

- `add_library(morph STATIC)` replaces `add_library(morph INTERFACE)`. Include directories, link libraries and compile
  definitions become `PUBLIC`. Both header file sets become `PUBLIC`, keep `VERIFY_INTERFACE_HEADER_SETS`, and stay in
  the existing `install(TARGETS morph EXPORT morphTargets …)` rule, which gains an `ARCHIVE` destination.
- `GenerateExportHeader` produces `morph/export.hpp` in the binary directory. It is added to `morph` through a
  `FILE_SET` whose base is that directory, so both the build and the install find it. A static build sets
  `MORPH_STATIC_DEFINE` `PUBLIC`.
- Sources are listed explicitly beside the target. Every lane adds to that list, so its conflicts are expected and
  trivial. Resolve them by keeping every line.
- **Instrumentation.** `apply_coverage(morph)` and `apply_sanitizers(morph)` are called on the target, as
  `src/client/CMakeLists.txt` does for `morph_client`. Without them `src/ui` has no coverage mapping, and the
  sanitizer legs stop instrumenting the UI layer. With them, every target linking `morph` must itself be built with the
  same sanitizer, or the link fails on `__asan_init`. The skeleton commit audits every consumer of `morph`, including
  `morph_guard_probe_deps`, the try_compile probes, examples and benches, and states the result.
- WASM's global `-fexceptions` already reaches a compiled target. Windows and WASM link the static library like any
  other. One that needs special handling is a finding to report.

### Coverage

The skeleton commit, not a later one, makes the coverage machinery see `src/ui`, so no lane ever runs a gate that
silently left its code out.

- `scripts/coverage.sh`: `SOURCES` gains `src/ui`.
- `scripts/check_branch_coverage.py` filters on `include/morph/` at line 197 and line 440. Both accept `src/ui`, and
  `src/ui` is scored together with `include/morph/ui` under the existing `include/morph/ui` floor. The script's
  self-tests cover the new prefix.
- The Codecov "framework" component, now `include/morph/**`, gains `src/ui/**`.
- `scripts/branch_partial_allowlist.json` entries for `document.hpp` and `expression.hpp` move with the code they
  point at. Hints are checked with the checker's own resolver on each commit.
- **The control is the report itself.** `check_coverage_roots.sh` and `check_coverage_objects.sh` inspect binaries and
  roots, not archives, so they cannot tell that the library went uninstrumented. Each lane shows `src/ui/*.cpp` lines
  with non-zero counts in its lcov output, and states that it checked.

### Order of work

1. **Skeleton (lane 1, first commit, pushed alone).** Target conversion, `export.hpp`, instrumentation and the
   consumer audit, all coverage changes, and one small `src/ui` source. The library builds and every test passes.
2. **Interpreter (lane 1).** Move `interpreter.hpp`'s non-template code to `src/ui/interpreter.cpp`. Then **measure**
   before anything else starts. `ScreenCore`'s and the other classes' definitions stay in the header, and it still
   includes `bridge.hpp` (632 MB alone) and `document.hpp` (726 MB). So the 1 GB goal may need include pruning, with
   forward declarations and moving includes into the `.cpp`. Lane 1 reports the figure and prunes what it can.
3. **In parallel, from the skeleton commit:**
   - **Document lane:** `document.hpp` and `expression.hpp`, with their allowlist entries.
   - **Services lane:** `service.hpp`, `service_client.hpp`, `host.hpp`, `workspace.hpp`, `shell_frame.hpp`,
     `shell_parts_model.hpp`, `backend.hpp`, `theme.hpp`, `explorer.hpp` and `navigator.hpp`.
4. **Measure again** and report against the goals. If a goal is missed, the next candidates are `core/remote.hpp`
   (3,406 lines, one template) and the non-template parts of `mount.hpp`. That decision is made on the measurement.

One commit per header, so a bad move can be dropped on its own.

### Verification

- Every target builds and every test passes, with clang and gcc locally, Qt, Qt Quick and TUI included.
- The front-end figures above are re-measured the same way, before and after.
- The lcov output shows `src/ui/*.cpp` with non-zero counts, and the `include/morph/ui` branch floor holds.
- `scripts/check_install_export.sh`'s consumer today calls only `Rational` and `client::defaultSignIn`, so it never
  links moved code. It gains a call that does, such as parsing a ui/1 document.
- **Performance.** `tests/bench_mount.cpp` includes only `mount.hpp`, which does not move, so it measures nothing
  here. Lane 1 runs a bench that drives the interpreter, or adds one, before and after. A regression over 10% is
  reported.
- clang-tidy-diff lints every moved line as changed. Lanes run clang-tidy on what they move and fix what it reports.
  That is part of the move, not a reason to skip it.
- Windows and gcc-release can only be judged by CI, and their results are reported as measured there.

### Documentation

These all say header-only, and are updated to say what is now true: `docs/ARCHITECTURE.md`, `docs/BUILDING.md`,
`docs/spec/VERSIONING.md`, `docs/spec/ui/service.md`, `docs/spec/core/profiler.md`, `docs/spec/core/bridge.md`,
`docs/spec/core/backend.md`, `docs/spec/journal/journal.md` and `docs/spec/forms/forms.md`. A lane greps `docs/` for
"header-only" rather than trusting this list. `docs/BUILDING.md`'s claim that `-I<repo>/include` alone compiles is
replaced with linking `morph::morph`.

## Part B: a shared library

Part B starts after part A's lanes land on the branch. It goes on the same branch, in its own commits.

### The option

`MORPH_BUILD_SHARED` (default `OFF`) chooses `add_library(morph SHARED)`. It is morph's own option, not
`BUILD_SHARED_LIBS`, because `BUILD_SHARED_LIBS` would also make Catch2, Lightweight and Tracy shared. Catch2 v3 is not
built to be a Windows DLL.

### Singletons must have one home

The core headers hold process-wide state as function-local statics and `inline` variables:

- `logState()` in `core/logger.hpp`
- `defaultDispatcher()` and `defaultRegistry()` in `core/registry.hpp`
- `screenRegistry()` in `ui/author.hpp`
- the owner probe in `core/detail/owner_probe.hpp`
- the effects, records and execute registries in `core/effects.hpp`, `core/records.hpp` and `core/bridge.hpp`
- thread-locals in `session/session.hpp`

With `morph` as a DLL, each module that includes these gets its own copy. With `VISIBILITY_INLINES_HIDDEN` on Linux
the same is true per shared object. The compiled UI code would then log through, and dispatch to, a different instance
than the application. So:

- Each non-template accessor moves out of line into the library, with `MORPH_API`. This touches core headers, and is
  allowed because the accessor's body does not depend on the macros listed in part A. The lane checks each one, and
  one that does depend on them is a stop-and-report finding.
- A per-type registry that is a template static stays one per module. The lane lists each one, and states whether it
  is only reached from the module that instantiates it, which is safe, or across the boundary. A crossing case is a
  finding to report with a proposed fix.
- `typeid` comparisons across the boundary (`core/registry.hpp`) and exceptions thrown from the library and caught
  outside need exported type information. A test that exercises each across the boundary is part of the work.

### Build

- The shared target uses `CXX_VISIBILITY_PRESET hidden` and `VISIBILITY_INLINES_HIDDEN ON`. A missing `MORPH_API`
  then fails to link on Linux as well as Windows.
- `install(TARGETS morph …)` gains `LIBRARY` and `RUNTIME` destinations.
- **Position-independent code.** core-cpp's modules are `STATIC`. `CMAKE_POSITION_INDEPENDENT_CODE ON` reaches them
  only when core-cpp is built in-tree. With `MORPH_BUILD_SHARED`, a `find_package(core-cpp)` hit on a prebuilt
  copy stops the configure with an error saying why, rather than failing at link.
- **Windows DLL lookup.** `CMAKE_RUNTIME_OUTPUT_DIRECTORY` puts `morph.dll` beside the executables, so ctest and
  `catch_discover_tests` (in `cmake/morph_add_rung.cmake` and `examples/common/CMakeLists.txt`) can load it.
- **MSVC.** C4251 (exported class with a member of an unexported type such as `std::string`) and C4275 (exported class
  with an unexported base, such as `RtScope : std::enable_shared_from_this` and `ScopeEnv : Environment` in
  `interpreter.hpp`) are disabled on `morph` and its users, with a comment giving the version rule as the reason.
  `dllexport` on a class instantiates every special member. A class that fails to compile because of that is fixed by
  exporting members instead of the class.

### CI

`Linux / clang-release` and `Windows / clangcl-debug` configure with `-DMORPH_BUILD_SHARED=ON`. Every other leg stays
static. `scripts/check_install_export.sh` runs once for each mode.

### Verification

- A local `-DMORPH_BUILD_SHARED=ON` build with hidden visibility links and passes every test on Linux.
- A test shows that the logger, the default dispatcher and the screen registry are the same object when reached from
  the library and from the executable.
- Windows shared is judged by CI.
