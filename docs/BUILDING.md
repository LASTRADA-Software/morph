# Building and installing morph

## Requirements

- **A C++23 compiler and standard library.** morph is developed against recent
  Clang and GCC on Linux and MSVC/clang-cl on Windows. The default logger uses
  `std::println`, so the standard library must be C++23 as well.
- **CMake and Ninja.** `CMakePresets.json` carries the supported configurations.
- **[Glaze](https://github.com/stephenberry/glaze) 7.4** for JSON reflection,
  found with `find_package` (for example from the `vcpkg.json` manifest) or
  fetched through CPM.
- **[core-cpp](https://github.com/contour-terminal/core-cpp) 0.7**, fetched
  through CPM. morph's timers run on its event loop, coroutines build on its
  `Task<T>`, and `morph::net` takes its base64 and wakeup primitive from it.
  morph itself is header-only, but `core::base`, `core::net` and
  `core::platform` are static libraries, so a project that links
  `morph::morph` builds them too.
- **Optional:** Qt 6 (WebSocket transport, Qt Quick frontend), libunicode
  (terminal frontend, fetched if not installed), SQLite3 (durable offline
  queue), and the dependencies of individual examples.

Fetched dependencies are cached in CPM's source cache, `.cache/cpm` by default
(`CPM_SOURCE_CACHE` overrides it), so a second configure clones nothing.

## Building the tests

On Linux the presets fetch what they need; no package manager is required:

```sh
cmake --preset clang-release
cmake --build build/clang-release --target morph_tests
./build/clang-release/tests/morph_tests
```

`gcc-release`, `clang-debug` and the sanitizer presets (`clang-asan`,
`clang-tsan`, `clang-ubsan`) work the same way. `linux-everything` turns every
optional feature on. The Windows presets (`cl-*`, `clangcl-*`) use vcpkg and
need `VCPKG_ROOT` set.

## CMake options

| Option | Default | What it builds |
|---|---|---|
| `MORPH_BUILD_TESTS` | `ON` | The unit tests |
| `MORPH_BUILD_EXAMPLES` | `ON` | The dependency-free examples (forms, concepts, gallery, LIMS gallery); the generic client when a frontend is on |
| `MORPH_BUILD_CLIENT` | `ON` | `morph::client`, the generic client's source and shell |
| `MORPH_BUILD_QT` | `OFF` | The Qt 6 WebSocket transport and executor |
| `MORPH_BUILD_QT_QUICK` | `OFF` | The Qt Quick frontend (needs `MORPH_BUILD_QT`, Qt 6.5+) |
| `MORPH_BUILD_TUI` | `OFF` | The terminal frontend |
| `MORPH_BUILD_NET` | `OFF` | `morph::net`, the raw-socket WebSocket transport (POSIX only) |
| `MORPH_BUILD_OFFLINE_SQLITE` | `OFF` | The SQLite-backed durable offline queue |
| `MORPH_BUILD_LADDER` | `OFF` | The application ladder; `MORPH_LADDER_RUNGS` picks the rungs |
| `MORPH_BUILD_BANK_EXAMPLE` | `OFF` | The bank example's SQLite/Lightweight stack |
| `MORPH_BUILD_HMAC_EXAMPLES` | `OFF` | The libsodium/OpenSSL HMAC adapter examples |
| `MORPH_BUILD_FUZZERS` | `OFF` | libFuzzer harnesses (Clang only) |
| `MORPH_BUILD_LOAD_TESTS` | `OFF` | Soak and throughput/latency benchmarks |
| `MORPH_BUILD_DOCUMENTATION` | `OFF` | The Doxygen reference (`--target doc`) |
| `MORPH_ENABLE_TRACY` | `OFF` | Tracy profiler zones on morph's hot paths; `MORPH_TRACY_ON_DEMAND` (`ON`) records only while a profiler is connected |
| `MORPH_ENABLE_STRICT_COMPILATION` | `ON` | Warnings as errors, with extra diagnostics |
| `MORPH_BUILD_CLANG_TIDY` | `OFF` | clang-tidy during the build, warnings as errors |
| `MORPH_INSTALL` | top-level project | Install and export rules |

Two options change what the library itself compiles, for deployments that need
them:

- **`MORPH_CLIENT_ONLY`** (`OFF`) suppresses the registrars that construct and
  run models, so a pure remote client need not link the models' implementations.
  Never enable it in a process that hosts models: it would register nothing, and
  every call would fail at run time with "unknown model type". See
  [`spec/core/registry.md`](spec/core/registry.md), "`MORPH_CLIENT_ONLY`".
- **`MORPH_REQUIRE_VETTED_HMAC`** (`OFF`) removes the default reference HMAC from
  the token signer and verifier, so every call site must pass a vetted MAC
  function. See [`spec/security.md`](spec/security.md).

## Using morph in your own project

### Install it, then `find_package` (supported)

```sh
cmake -S . -B build-min -G Ninja -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build-min
cmake --install build-min --prefix /your/prefix
```

```cmake
find_package(morph CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE morph::morph)
```

Put the prefix on `CMAKE_PREFIX_PATH`. `morph::morph` carries the include path,
Glaze, core-cpp, Threads and morph's compile definitions. Glaze and core-cpp are
installed alongside morph when the build fetched them, and `morphConfig.cmake`
finds them with `find_dependency`, so a missing dependency fails at
`find_package` rather than at compile time. The build step must run before the
install because core-cpp's static libraries are part of it.

Optional components install only when their build option was on, and are
requested by name:

```cmake
find_package(morph CONFIG REQUIRED COMPONENTS net offline_sqlite)
target_link_libraries(your_app PRIVATE morph::net morph::offline_sqlite)
```

| Component | Build option | Notes |
|---|---|---|
| `net` | `MORPH_BUILD_NET` | POSIX only |
| `tui` | `MORPH_BUILD_TUI` | Installed only when libunicode was found rather than fetched |
| `offline_sqlite` | `MORPH_BUILD_OFFLINE_SQLITE` | |
| `qt` | `MORPH_BUILD_QT` | |
| `qt_quick` | `MORPH_BUILD_QT_QUICK` | Needs `qt` |

Asking for a component that was not installed fails at `find_package` and names
it. An install that carries `tui` needs libunicode findable by every consumer,
even one that does not ask for `tui`, because the core-cpp package installed
with it was built with its TUI: put libunicode's prefix on `CMAKE_PREFIX_PATH`
next to morph's.

### Vendor the repository

`add_subdirectory(morph)` or `FetchContent` gives you the same `morph::morph`
target. `MORPH_INSTALL` defaults to `PROJECT_IS_TOP_LEVEL`, so a vendored morph
adds nothing to your project's install.

### A bare include path (not recommended)

`-I<repo>/include` compiles, since morph is header-only, but then nothing
supplies what the interface target would: Glaze at the pinned version, core-cpp
and its static libraries, a C++23 standard library, the thread library, and each
optional subsystem's own dependencies. Prefer `find_package`.

## API reference

```sh
cmake -S . -B build-doc -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON \
      -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build-doc --target doc
```

Doxygen runs with warnings as errors: every public symbol carries complete
`@param`/`@tparam`/`@return` documentation.
