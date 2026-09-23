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

  **Running the `clang-tidy-diff` gate locally — use this diff base, and check
  the file count.** The CI job analyses *changed lines only*, computed against
  the pull request's base commit. The local equivalent is the three-dot form:

  ```sh
  cmake --preset clang-debug -DMORPH_BUILD_NET=ON -DMORPH_BUILD_QT=ON \
        -DMORPH_BUILD_LADDER=ON -DMORPH_BUILD_BANK_EXAMPLE=ON   # see ci.yml for the full set
  git fetch origin master
  git diff -U0 origin/master...HEAD > /tmp/changed.diff

  # A gate that analysed nothing must say so rather than exit 0.
  files=$(grep -c '^+++ ' /tmp/changed.diff)
  test "$files" -gt 0 || { echo "no changed files -- wrong diff base?"; exit 1; }
  echo "clang-tidy-diff over $files changed file(s)"

  python3 "$(find /usr/lib/llvm-*/share/clang /usr/share/clang \
                  -name clang-tidy-diff.py | head -1)" \
      -p1 -path build/clang-debug -j "$(nproc)" -quiet \
      -extra-arg=-std=c++23 -extra-arg=-Wno-missing-include-dirs \
      < /tmp/changed.diff
  ```

  `origin/master...HEAD` — **three** dots — diffs from the merge base, which is
  what `github.event.pull_request.base.sha` names and therefore the same file
  set CI analyses. `git diff -U0 HEAD`, the form that suggests itself, compares
  the *working tree* to the current commit: after you commit it is **empty**,
  `clang-tidy-diff.py` is handed no files, analyses nothing and exits 0. Five
  lanes ran that command and read the exit code as a green gate (morph#776);
  one of them had 345 changed lines against CI and zero locally.

  That is also why the file count is printed and asserted rather than assumed.
  Checking the gate by injecting a deliberate finding does **not** catch this:
  an injected edit is uncommitted, so it appears in `git diff HEAD`, the gate
  dutifully reports it and exits 1, and the non-vacuity check passes while the
  real check covers nothing. **An injection test validates the plumbing, not
  the input.** Count the files.

  A green local run still is not CI's run, for a reason that has nothing to do
  with the diff base: see `scripts/check_catch2_pin.sh`'s header for which
  findings can differ and why.

  **Public macro definitions are exempt, by `// clang-format off`.** They are
  the framework's documented API and contributors read them as reference, so
  the continuation backslashes are hand-aligned and the body stays legible as
  a block; leaving them to the formatter means any unrelated edit nearby
  re-wraps the whole definition, and in one case it broke a token-paste
  (`##`) invocation apart. Freeze them, and realign by hand if a body changes.
  The sites carry a one-line pointer back here rather than repeating this
  paragraph — it used to be copy-pasted at all seventeen of them, and two of
  those copies had drifted into describing code that was not there.
- **Do not trust an absolute path in a test failure when several worktrees
  share one compiler cache.** `__FILE__` is expanded at compile time and baked
  into the object; Catch2 records it per `TEST_CASE`. `fastcache-cc` serves
  entries across checkouts — that cross-worktree hit rate is why
  `cmake/CompileCache.cmake` prefers it to `ccache` — so a cache hit hands you
  an object carrying **the path of whichever worktree compiled it**. The
  failure report then names a directory that may hold a different revision of
  that file, or that a landed lane has already pruned. Nothing in the output
  says so; the path looks plausible.

  Reproduced deliberately: two directories with byte-identical sources, the
  second compile served from the cache, and the object it received baked in
  the *first* directory's path. CI is unaffected — one checkout per runner.

  `-ffile-prefix-map` is not the fix, and that is measured rather than
  assumed: it does normalise the path, and it takes the cross-worktree hit
  rate to zero doing it, because the flag carries the absolute source root
  into the cache key. The numbers are in `cmake/compiler_options.cmake` beside
  the same trade for coverage (morph#426, morph#775). So: read the line number,
  not the directory, and re-run in your own worktree before believing where a
  failure points.
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

- **`TSAN_OPTIONS` is wired into the test preset, absolutely, and it carries
  two settings rather than one.** The authoritative value is the one in
  `CMakePresets.json` on the `clang-tsan` **test** preset — read it there
  rather than from a copy, because this paragraph quoting a *prefix* of it is
  the mistake that produced morph#783. What the two settings are for:

  - `suppressions=${sourceDir}/cmake/tsan.supp` — the known-false-positive
    entries for libstdc++'s refcounted exception teardown (morph#476); the
    file itself carries the evidence for each. `${sourceDir}` makes the path
    absolute, so `ctest --preset clang-tsan` resolves it from any working
    directory.
  - `second_deadlock_stack=1` — without it a `lock-order-inversion` report
    names only where each mutex was acquired *in the inverting thread*. With
    it, TSan also prints a `Mutex Mn previously acquired by the same thread
    here:` stack for each **already-held** mutex, which is what names the
    fixture or short-lived object holding it. Measured on a two-mutex
    inversion under clang 22.1.8: the report goes from 36 lines to 55, the
    extra 19 being those two stacks (morph#736). Without the flag TSan prints
    only `Hint: use TSAN_OPTIONS=second_deadlock_stack=1 to get more
    informative warning message` — advice nobody can act on after the fact,
    because morph#578 and morph#717 are intermittent and the run that fires is
    the only evidence there will ever be. CI's two TSan legs pass it, so a
    local reproduction that did not would be less informative than the run it
    is reproducing.

  Both live in **one** `TSAN_OPTIONS` value, colon-separated, and that is
  load-bearing: the variable is a single string, so a second `TSAN_OPTIONS`
  assignment **replaces** the first rather than extending it. Exporting your
  own `TSAN_OPTIONS=suppressions=/some/other.supp` therefore drops
  `second_deadlock_stack=1` silently — you get a weaker TSan than CI runs with
  nothing saying so. If you must add a setting, append it to the preset's
  value; do not set the variable alongside it.

  Spelling the suppressions path relatively —
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
