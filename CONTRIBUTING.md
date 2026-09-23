# Contributing to morph

## Toolchain

morph is a header-only C++23 library. You need a C++23 compiler, CMake with
Ninja, and the dependencies declared in `vcpkg.json` (Glaze, Catch2; Qt 6 only
when building the optional Qt integration, `-DMORPH_BUILD_QT=ON`). CMake
presets are provided — `cmake --list-presets` shows the configured matrix; the
README documents the full set of build options
(`MORPH_BUILD_TESTS`, `MORPH_BUILD_EXAMPLES`, `MORPH_BUILD_QT`,
`MORPH_BUILD_FORMS_QML`, …).

A plain configure/build/test loop:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## The spec-first workflow (please read this one section)

`docs/spec/` is the **authoritative design reference** — one file per public
type or subsystem. The rules, from `AGENTS.md`:

- **Before changing any public type or subsystem, read its spec.** The spec
  carries the invariants and reasoning the code alone does not.
- **If a change invalidates any part of a spec, update the spec in the same
  change** — never the other way around. Where `docs/ARCHITECTURE.md` (the
  cross-cutting map) and a spec disagree, the spec wins.
- **Planned work** lives in `docs/planned/`, one spec per feature, each with a
  `Status: planned — not yet implemented` banner; `docs/todo.md` is the
  prioritised index. When you implement one: build against the spec, then flip
  its banner and rewrite it to present tense, and update `ARCHITECTURE.md`.
- **Feature docs** (`docs/superpowers/`) are compressed reference docs — one
  file per feature, under 500 lines, present tense only, no changelogs or
  migration notes (git history covers that).

## The changelog

`CHANGELOG.md` tracks notable changes to **morph** — the library, its specs and
its tooling. Its scope is what a consumer of the framework sees: the same
surface `docs/spec/VERSIONING.md` defines.

- **A change confined to `examples/` gets no changelog entry.** The application
  ladder is demonstration code. Adding a rung action, fixing a rung's model,
  extending a rung's tests, or adding a scenario under
  `scripts/scenario/scenarios/` changes nothing a morph consumer depends on —
  git history and the rung's own README carry that record.
- **A rung change that moves the framework does get one, for the framework
  part**: the new `include/morph/` seam, the spec that changed, the behaviour a
  consumer can now rely on. Describe that, not the rung that motivated it.
- **An entry describes its own change and then stops moving.** Do not re-count
  or re-word a published entry because later work changed a total it quotes —
  it remains a true statement about the change it documents.

Practical reason for the first rule: every rung PR appending to the same two
list heads made `CHANGELOG.md` the only file rung work ever conflicted in,
serialising independent rungs behind one file.

## Quality gates

- **Tests:** behavior changes come with Catch2 tests under `tests/`. For what
  this repository's test instruments collectively guarantee — the coverage
  floor CI enforces, which SQLite-style techniques are adopted or declined and
  why, and each instrument's known reach limits (`oom_injector` disabled under
  sanitizers, `fault_proxy` is ladder-only, no I/O-failure seam for
  `include/morph/net`) — see
  [`docs/spec/testing_strategy.md`](docs/spec/testing_strategy.md) before
  deciding what a change needs beyond an ordinary Catch2 case.
- **Sanitizers:** the `clang-asan`/`clang-tsan`/`clang-ubsan` presets are what
  CI's sanitizer matrix runs. Running them locally has two traps that cost more
  to rediscover than to read about — see
  [Running the sanitizer presets locally](#running-the-sanitizer-presets-locally).
- **Doxygen is strict:** the Docs CI job runs with
  `WARN_AS_ERROR = FAIL_ON_WARNINGS` — every public symbol needs complete
  `@param`/`@tparam`/`@return` docs. Reproduce locally with
  `cmake -S . -B build -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON
  -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF` and
  `cmake --build build --target doc`.
- **Warnings are errors:** `cmake/compiler_options.cmake` turns on
  `-Weverything` (Clang, AppleClang and clang-cl), the widest practical GCC
  set, or `/W4` (MSVC), plus `-Werror`/`/WX` while
  `MORPH_ENABLE_STRICT_COMPILATION` is on — which it is by default, locally as
  well as in CI. Configuring prints which set was chosen, e.g.

  ```text
  -- morph: warnings: AppleClang 17.0.0.17000603 -> Clang set applied (29 flags, -Weverything, strict=ON)
  -- morph: warnings: '-Weverything' verified on 4 target(s)
  ```

  If those two lines are missing, or the first names a compiler family you did
  not expect, your local build is not enforcing what CI enforces — say so on
  the issue rather than working around it. A compiler id the file does not
  recognise now fails the configure outright instead of quietly building with
  no diagnostics.
- **Formatting/linting:** `.clang-format` and `.clang-tidy` govern C++;
  markdown follows `.markdownlint.yaml` (119-column limit; code blocks and
  tables exempt). `pre-commit run --all-files` runs the configured hooks.

  **Public macro definitions are exempt, by `// clang-format off`.** They are
  the framework's documented API and contributors read them as reference, so
  the continuation backslashes are hand-aligned and the body stays legible as
  a block; leaving them to the formatter means any unrelated edit nearby
  re-wraps the whole definition, and in one case it broke a token-paste
  (`##`) invocation apart. Freeze them, and realign by hand if a body changes.
  The sites carry a one-line pointer back here rather than repeating this
  paragraph — it used to be copy-pasted at all seventeen of them, and two of
  those copies had drifted into describing code that was not there.
- **Keep mechanical facts honest:** `docs/spec/pinned_facts.toml` pins the
  mechanical facts that recur across specs — enum cardinalities, key
  constants (`kMaxEnvelopeBytes`, `kMaxDecimalPlaces`, `kClockSkewMs`),
  canonical error/reply strings, and glaze parsing behavior
  (`error_on_unknown_keys = false`, duplicate-key last-wins). Two CI checks
  enforce it:
  - `tests/test_pinned_facts.cpp` asserts the real code symbols against a
    header `cmake/pinned_facts.cmake` generates from the manifest at
    configure time — a `static_assert`/exhaustive-`switch` compile-time gate
    where the type system allows it, a Catch2 runtime `REQUIRE` where it
    does not (an exception's `what()`, a `RemoteServer` reply string, a
    glaze option that has no reachable symbol). It runs as part of the
    normal `morph_tests` target, so it is checked under every compiler in
    the CI matrix.
  - a gate removed on 2026-09-23 (the "Drift guard" workflow) asserts
    every pinned value is still cited in the spec file that documents it,
    and that no banned, superseded terminology (e.g. the pipe-delimited-era
    "*N*-part protocol" wording) has crept back into `docs/spec/`,
    `docs/ARCHITECTURE.md`, or `include/`.

  If you change a pinned constant, enum cardinality, or canonical error
  string, update the code, `docs/spec/pinned_facts.toml`, and the spec prose
  that cites it together in the same commit.

## Running the sanitizer presets locally

Three presets build the tree under a sanitizer: `clang-asan` (ASan + UBSan),
`clang-tsan` (TSan) and `clang-ubsan` (UBSan standalone). The whole recipe, for
TSan — nothing else needs setting, and in particular **do not set
`TSAN_OPTIONS` by hand**:

```sh
cmake --preset clang-tsan
cmake --build --preset clang-tsan
bash scripts/check_sanitizer_instrumentation.sh build/clang-tsan tsan
ctest --preset clang-tsan
```

Substitute `clang-asan`/`asan` or `clang-ubsan`/`ubsan` throughout for the
other two. Two things that recipe hides, both of which used to have to be
rediscovered:

- **The suppressions file is wired into the test preset, absolutely.**
  `cmake/tsan.supp` holds the known-false-positive entries for libstdc++'s
  refcounted exception teardown (morph#476); the file itself carries the
  evidence for each. The `clang-tsan` **test** preset sets
  `TSAN_OPTIONS=suppressions=${sourceDir}/cmake/tsan.supp`, so `ctest --preset
  clang-tsan` resolves it from any working directory. Spelling it relatively —
  `TSAN_OPTIONS=suppressions=cmake/tsan.supp`, which is what copying CI's line
  by hand tends to produce — breaks test *discovery*, not the tests:
  `catch_discover_tests` runs each binary with its own build directory as the
  working directory, TSan cannot open the file, and it exits with its default
  `exitcode=66` before Catch2 lists a single test. Catch2's
  `CatchAddTests.cmake` then reports

  ```text
  CMake Error at .../CatchAddTests.cmake:307 (message):
    Error listing tests from executable '.../examples/concepts/morph_concepts_tests':

      Result: 66
      Output:
  ```

  `Output:` is empty for *every* discovery failure — the listing is written to
  a file via `--out`, so there is never anything on stdout to report — and the
  binary named is simply the first one ctest enumerated, not the one you were
  working on. The one real clue is a line of TSan's own, on stderr, printed
  just above that error and not part of it:

  ```text
  ThreadSanitizer: failed to read suppressions file
  '.../build/clang-tsan/examples/concepts/cmake/tsan.supp'
  ```

  — a path under the *build* tree that nobody wrote, which is the relative one
  resolved against the discovery working directory. If you see any of this,
  check your environment for a relative `TSAN_OPTIONS` rather than the named
  binary. Note that a preset cannot
  protect you here: a test preset's `environment` reaches the test processes,
  not ctest's own process, so a bad `TSAN_OPTIONS` inherited from your shell
  still reaches the discovery step.
- **The OomInjector tests are excluded on the `clang-asan` and `clang-tsan`
  presets.** `tests/oom_injector.cpp` overrides the process-wide `operator
  new`/`delete`, which the ASan and TSan runtimes also define, so the file
  compiles its overrides out under `__SANITIZE_ADDRESS__`/`__SANITIZE_THREAD__`
  and every test that arms the injector then throws `OomInjector: unusable
  under ASan/TSan`. Measured on both presets: five `OomInjector` cases plus the
  morph#108 allocation-failure case. Both test presets carry
  `filter.exclude.name` = `OomInjector|morph#108` so the plain `ctest --preset`
  above is green; CI passes the same regex as `-E` explicitly, which overrides
  the preset field with the same value. Every non-sanitizer leg runs those
  tests normally.

The `check_sanitizer_instrumentation.sh` line is not a formality. A sanitizer
run over binaries that were never instrumented passes the whole suite and
reports nothing, which reads exactly like a clean run (morph#542, morph#679).
The sweep above walks every binary ctest will run and asserts each carries the
mode's symbols:

```text
check_sanitizer_instrumentation: 4 ctest binaries all carry __tsan_ symbols (0 allowlisted).
```

If you built a single target rather than the whole preset, the sweep refuses
(too few binaries for its floor to mean anything); ask the narrower question
instead, which applies no floor and says so:

```sh
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/tests/morph_tests tsan
```

## Security

Suspected vulnerabilities go through private reporting — see
[SECURITY.md](SECURITY.md), not the public issue tracker.

## License

morph is Apache-2.0 (see [LICENSE](LICENSE)); contributions are accepted under
the same license.
