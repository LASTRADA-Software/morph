# Quiet by default — design

`morph::log`'s default minimum level stops being `debug`. A release build
defaults to `warn`; a debug build defaults to `debug`, so a developer who
built to debug still gets the diagnostics without asking. The Catch2 test
binaries override both: they default to `off` and take a `--log-level` switch,
because a test run should be quiet in every configuration — including the
Debug ones that CI, the three sanitizers and the coverage job all use. CI
turns the stream back on for a leg that wants it through
`MORPH_TEST_LOG_LEVEL`, since its suites run under `ctest` where a command-line
flag cannot reach them.

## Contents

- [The noise](#the-noise)
- [Why the build-type default is not enough on its own](#why-the-build-type-default-is-not-enough-on-its-own)
- [The library default](#the-library-default)
- [The ODR hazard, and the escape hatch](#the-odr-hazard-and-the-escape-hatch)
- [The test gate](#the-test-gate)
- [Linking the gate](#linking-the-gate)
- [Tests that depended on the default](#tests-that-depended-on-the-default)
- [Pinning both defaults](#pinning-both-defaults)
- [Versioning](#versioning)
- [Testing](#testing)
- [What a quiet run still prints](#what-a-quiet-run-still-prints)
- [Out of scope](#out-of-scope)

## The noise

A default `morph_tests` run (`build/gcc-release`, 1,437 cases, 22,078
assertions) writes **2,972 lines** to the terminal:

| Stream | Lines | Content |
|---|---|---|
| stdout | 23 | Catch2 banner, the one expected failure, the summary |
| stderr | 2,895 | `[DEBUG]` |
| stderr | 44 | `[ERROR]` |
| stderr | 10 | `[WARN ]` |

The stdout half is Catch2 doing its job. The 2,949 stderr lines are
`morph::log`'s default sink, overwhelmingly one message:

```
[DEBUG] [dispatchMessage] connection 0: kind=attach callId=0 typeId=SHI_CounterModel modelId=0 modelType= actionType= bodyBytes=0
```

Nothing in the test binary configures the logger, so the level is whatever
`LogState` is constructed with:

```cpp
// include/morph/core/logger.hpp:113
std::atomic<LogLevel> minLevel{LogLevel::debug};
```

`debug` is level 0, the lowest, so every record passes the filter. The
`[ERROR]` and `[WARN ]` records are not failures — they are tests deliberately
exercising error paths, which is why the run exits 0.

## Why the build-type default is not enough on its own

A `warn`-in-release / `debug`-in-debug default is the right *library* policy,
but on its own it would not fix the reported problem, because this project
builds Debug almost everywhere. Nine of the fourteen configure presets set
`CMAKE_BUILD_TYPE: Debug`:

```
gcc-debug  clang-debug  cl-debug  cl-qt-debug  clangcl-debug
clang-asan  clang-tsan  clang-ubsan  clang-coverage
```

and the CI workflows reference Debug ten times and Release zero. The 2,972-line
measurement above was taken on `gcc-release` — the quiet half. Left to the
library default alone, `morph_tests` would stay at full volume in CI, under all
three sanitizers, under coverage, and in `gcc-debug`/`clang-debug` locally:
every configuration whose logs anyone actually reads.

So the build-type default and the test gate are not alternatives. The default
decides what an *application* gets; the gate decides what a *test run* gets,
and a test run should be quiet regardless of how it was compiled.

## The library default

```cpp
// include/morph/core/logger.hpp — above LogState
#ifndef MORPH_DEFAULT_LOG_LEVEL
#  ifdef NDEBUG
#    define MORPH_DEFAULT_LOG_LEVEL ::morph::log::LogLevel::warn
#  else
#    define MORPH_DEFAULT_LOG_LEVEL ::morph::log::LogLevel::debug
#  endif
#endif

// in LogState
std::atomic<LogLevel> minLevel{MORPH_DEFAULT_LOG_LEVEL};
```

`NDEBUG` rather than `CMAKE_BUILD_TYPE`: a header cannot see a CMake variable,
and `NDEBUG` is the standard, portable signal that every generator and
toolchain already sets for release configurations.

Consequences for a consumer who never calls `setLogLevel()`: a release build
emits warnings and errors and nothing else; a debug build emits everything, as
today. Both remain overridable at runtime by the existing public
`setLogLevel()`, which is unchanged.

This also means the eleven example entry points (`examples/*/src/*/main.cpp`,
`src/main.cpp`) need **no edit**. They call `logError`, and `warn` lets errors
through in release while debug builds stay verbose — which is what a demo
should do. An earlier revision of this design added a `setLogLevel` line to
each; the build-type default makes that redundant, and it is dropped.

## The ODR hazard, and the escape hatch

`LogState` is a struct defined in a header and reached through
`inline LogState& logState()`. Making its default member initializer depend on
`NDEBUG` means two translation units compiled with different settings see
**different definitions of the same type and the same inline function** — an
ODR violation, where the linker silently keeps one and discards the other. The
surviving definition is unspecified, so the observed default becomes a
link-order accident.

morph is header-only and every consumer recompiles the headers, so within one
consistently-configured project this cannot fire. It fires on mixed-config
links: a release application pulling in a debug-built helper library, a vcpkg
tree mixing configurations, an IDE that builds one target Debug and another
Release.

This is accepted rather than solved — any config-dependent value in a header
has the same shape, and the alternative (an unconditional default) was
considered and rejected. It is mitigated two ways:

- **`MORPH_DEFAULT_LOG_LEVEL` is the escape hatch.** Because the `#ifndef`
  guard comes first, a consumer who defines it project-wide
  (`-DMORPH_DEFAULT_LOG_LEVEL=::morph::log::LogLevel::warn`) gets one identical
  definition in every TU regardless of each one's `NDEBUG`, which removes the
  hazard entirely.
- **It is documented as a hazard,** in `docs/spec/core/logger.md`, not left for
  someone to discover from a wrong log level.

## The test gate

Catch2 event listeners cannot add command-line options — the parser is built
before listeners exist. Taking the level from the CLI therefore means owning
`main` and driving `Catch::Session` directly, which is supported and verified
against the pinned Catch2 3.15.3: `Catch::Session` exposes
`cli()` / `cli(Parser const&)` and `applyCommandLine(argc, argv)`
(`<catch2/catch_session.hpp>`), and `Catch::Clara::Opt(T& ref, StringRef hint)`
is the option type (`<catch2/internal/catch_clara.hpp>`).

A new `tests/test_main.cpp` provides that `main` for every Catch2 test binary:

```cpp
int main(int argc, char* argv[]) {
    Catch::Session session;
    std::string level;  // empty -> fall back to the environment

    session.cli(session.cli()
        | Catch::Clara::Opt(level, "level")["--log-level"]
              ("morph log level: debug|info|warn|error|off (default: off)"));

    if (const int rc = session.applyCommandLine(argc, argv); rc != 0) {
        return rc;
    }
    if (!applyLogLevel(level)) {  // prints the accepted values on a bad value
        return 1;
    }
    return session.run();
}
```

```
$ ./morph_tests                              # quiet, Debug and Release alike
$ ./morph_tests --log-level=debug            # today's full output
$ ./morph_tests --log-level debug            # Clara accepts both spellings
$ ./morph_tests -? | grep log-level          # documented in the built-in help
```

The option appears in `--help` alongside Catch2's own, which is the main reason
to prefer it to an environment variable: a developer looking for the switch
finds it where they already look.

**`MORPH_TEST_LOG_LEVEL` is the CI half, not a fallback for its own sake.**
Every CI leg runs the suites through `ctest --preset …`
(`ci.yml:113`, `:291`, `:429`, `:538`, `:726`, `:816`, `:1161`, `:1414`,
`:1616`), and `catch_discover_tests` registers each test case as its own ctest
invocation — so `--log-level` cannot reach the binaries that way. An
environment variable is the only input that reaches all of them at once:

```yaml
env:
  MORPH_TEST_LOG_LEVEL: debug     # one line, job- or workflow-scoped
```

So the two inputs divide cleanly by audience. `--log-level` is the local
switch, discoverable in `-?` where a developer already looks.
`MORPH_TEST_LOG_LEVEL` is how CI turns the full stream back on for a leg that
wants it. The CLI wins when both are set, so a developer debugging locally is
never overridden by an exported variable they forgot about.

One CI leg does invoke the binaries directly — the Valgrind job
(`ci.yml:1718`) loops over `morph_tests`, `morph_net_tests` and
`morph_offline_sqlite_tests`. It can use either form, and is the leg that
benefits most from the quiet default: three suites at 2,949 stderr lines each,
under memcheck, is the noisiest log in the matrix.

An unrecognised value on either path is a mistake the developer wants to know
about, not a silent fallback: `applyLogLevel` writes one line naming the
switch and the accepted values and `main` returns 1, so a typo fails loudly
instead of running the suite at the wrong level.

The gate sets the level only. It does not replace the sink, so a test that
installs its own sink is unaffected, and the default `stderr` sink is still
what a developer sees when they opt back in.

## Linking the gate

Test targets stop linking `Catch2::Catch2WithMain` and link
`Catch2::Catch2` plus a new `morph_test_main` static library carrying the
`main` above. An archive is safe here — unlike a Catch2 listener, which
registers through a static initializer that the linker may drop with no
undefined symbol to pull it in, `main` is always an undefined symbol, so the
object is always extracted.

```cmake
add_library(morph_test_main STATIC tests/test_main.cpp)
target_link_libraries(morph_test_main PUBLIC morph::morph Catch2::Catch2)
```

Consumers, each of which links `Catch2::Catch2WithMain` today:

```
morph_tests            morph_net_tests           morph_qt_tests
morph_offline_sqlite_tests   morph_net_qt_interop_tests
bank_tests             ladder_common_tests       morph_concepts_tests
morph_forms_controller_core_tests
morph_vetted_hmac_{libsodium,openssl}_tests
```

`catch_discover_tests` keeps working: it invokes the binary with
`--list-tests` and friends, which `session.applyCommandLine` handles exactly
as Catch2's own `main` does. That it still enumerates 1,437 cases is part of
the verification below, since a broken `main` would show up as an empty test
list rather than as a failure.

## Tests that depended on the default

Eleven sites install a sink with a bare `setLogger()` after a snapshot-only
`ScopedLoggerOverride`, and never set a level — so they inherit whatever is
ambient. With the gate forcing `off`, the seven that **capture and assert**
would capture nothing:

| Site | What it captures |
|---|---|
| `tests/test_sync_worker.cpp:180` | `logged` vector, asserted on |
| `tests/test_sync_worker.cpp:208` | `logged` vector, asserted on |
| `tests/test_sync_worker.cpp:248` | `logged` vector, asserted on |
| `tests/test_concurrency_invariants.cpp:202` | counts `[orphan]` `[ERROR]` records |
| `tests/test_coverage_push95.cpp:448` | `"null Deps member"` message |
| `tests/test_coverage_gaps.cpp:307` | throwing sink covering a `catch(...)` arm |
| `tests/test_completion_extra.cpp:305` | throwing sink covering a `catch(...)` arm |

The other four (`tests/test_sync_worker.cpp:279`, `:526`, `:546`,
`tests/test_coverage_push95.cpp:507`) install a no-op sink purely for silence
and do not care about the level.

All eleven move to `ScopedLoggerOverride`'s explicit constructor, which
installs the sink **and** sets the level under one acquisition of the global
mutex:

```cpp
// before — sink installed bare, level inherited from whatever is ambient
morph::log::ScopedLoggerOverride guard;
std::vector<std::string> logged;
morph::log::setLogger([&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); });

// after — the guard installs the sink and sets the level; `logged` outlives it
std::vector<std::string> logged;
morph::log::ScopedLoggerOverride const guard{
    [&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); }};
```

Better than adding a `setLogLevel(debug)` call beside each `setLogger()`:

- **One locked operation instead of two.** The snapshot and install happen
  under a single `scoped_lock`, which is what the class's own comments say the
  pairing is for — a snapshot straddling separate `setLogger` and
  `setLogLevel` calls can restore a combination that never existed.
- **The level is stated at the site**, so a future change to the ambient level
  cannot silently break these tests again. This design is itself the second
  such change; the first would have been caught by this.
- **The sink cannot leak** past the test: install and restore are one object's
  lifetime.

**Declaration order matters.** The captured object (`logged`, `orphanCount`,
`sawNull`) must be declared *before* the guard. Destruction runs in reverse
order, so the guard uninstalls the sink first and the captured object dies
second; the reverse leaves a sink holding a dangling reference during
teardown. The existing code declares the guard first, so each conversion swaps
two lines.

## Pinning both defaults

Two separate things can silently regress, so each gets its own check.

**The library default.** The gate mutates the global level at run start, so a
test cannot read `getLogLevel()` to learn what the library would have chosen.
It constructs a fresh state instead, which is order-independent and immune to
the gate:

```cpp
morph::log::detail::LogState fresh;
#ifdef NDEBUG
    REQUIRE(fresh.minLevel.load() == morph::log::LogLevel::warn);
#else
    REQUIRE(fresh.minLevel.load() == morph::log::LogLevel::debug);
#endif
```

**The gate itself.** A test asserts the ambient level is `off` whenever
neither `--log-level` nor `MORPH_TEST_LOG_LEVEL` was given. This has teeth: if
`main` failed to apply the level, the ambient value would be the library
default — `warn` under Release, `debug` under Debug — and both differ from
`off`. The test must consult the same two inputs the gate does, so that a
developer running `--log-level=debug` does not see a spurious failure. Since
`main` owns the resolved level, it records it in a small accessor the test
reads, rather than each re-parsing the inputs and drifting apart.

## Versioning

`docs/spec/core/logger.md:56` documents `Default: debug (everything passes)`.
Per `AGENTS.md` the spec is the contract, so it changes with the code; per
`docs/spec/VERSIONING.md` a change to documented behaviour of the stable
surface is normally a major release with a deprecation window.

The project is at **0.1.0** — pre-1.0, so no deprecation marker and no window
are required, and the CHANGELOG entry carries the notice.
`logger.md:142` (`ScopedLoggerOverride`'s explicit constructor defaults to
`debug`) stays correct and is untouched. `logger.md` also gains the
`MORPH_DEFAULT_LOG_LEVEL` macro and the ODR hazard.

## Testing

- **The measurement that motivated this, repeated in both configurations.**
  `morph_tests 2>&1 | wc -l` must reach 23 in `gcc-release` *and* in
  `gcc-debug`. Release alone proves nothing here — Debug is the configuration
  that was never quiet.
- **The gate is proven to run, not assumed to link.** The pin test above fails
  if `main` never applied the level. Additionally, stub out the `applyLogLevel`
  call once and confirm the test fails — a check never observed failing is not
  a check.
- **`catch_discover_tests` still enumerates every case.** A custom `main` that
  mishandles `--list-tests` shows up as ctest registering nothing, which is a
  silent pass, not a failure. Confirm `ctest -N` still lists 1,437 cases.
- **`--log-level` is discoverable and validated.** `morph_tests -?` shows the
  option; `--log-level=nonsense` exits non-zero naming the accepted values.
- **Both pins fail on revert.** Restore `minLevel{LogLevel::debug}` and confirm
  the library-default pin fails in the Release configuration.
- **`--log-level=debug` restores today's output** — 2,949 stderr lines,
  confirming the gate suppresses rather than removes. Same for the
  `MORPH_TEST_LOG_LEVEL=debug` fallback, and CLI wins when both are set.
- **Full suite green** under `gcc-debug`, `gcc-release`, and `clang-asan`. The
  eleven converted sites are where a mistake lands, and a botched
  declaration-order swap is a use-after-free that only the sanitizer build
  reliably catches.

## What a quiet run still prints

The 23 surviving lines are not all banner and summary. Thirteen of them are a
`FAILED:` block, and no log level can suppress it, because it is not a log
record:

```
-------------------------------------------------------------------------------
morph::test::checkReplayLedgerContract rejects a no-op ledger
-------------------------------------------------------------------------------
/home/yaraslau/repo/morph/tests/test_replay_ledger.cpp:44
...............................................................................

/home/yaraslau/repo/morph/tests/replay_ledger_conformance.hpp:52: FAILED:
  REQUIRE( hit.has_value() )
with expansion:
  false
with message:
  implementation under test: NoOpReplayLedger
```

This is `tests/test_replay_ledger.cpp:44`, tagged `[!shouldfail]`: a negative
control that feeds a deliberately inert `NoOpReplayLedger` to the replay-ledger
conformance suite. Catch2 prints the failing assertion and then counts the case
as passing — `1 failed as expected` in the summary — and turns the run red if
the stub ever *satisfies* the suite, which would mean the suite had weakened
into checking nothing.

It is left exactly as it is. Suppressing it would remove the visible half of a
control whose entire purpose is to be noticed when it stops failing, and the
alternatives (a separate target, a custom reporter) buy tidiness at the cost of
either scope or the control's legibility. This section exists so the next
person to run a quiet suite and see `FAILED:` knows in one search that it is
the point rather than a regression.

## Out of scope

- **Changing what any call site logs, or at what level.** The 2,895 `[DEBUG]`
  `dispatchMessage` records are not reclassified or removed; they stay exactly
  as useful to anyone who turns debug back on.
- **A CMake option for the test level.** `--log-level` and its environment
  fallback are runtime inputs, changeable without reconfiguring; a
  configure-time twin would add a third place to look when output is missing.
- **Auditing other test binaries' output.** The rest inherit the gate; none is
  examined here for noise of its own beyond what the level fixes.
- **`docs/spec/testing_charter.md`.** The gate changes what a test run prints,
  not what the suite guarantees.
- **The `[!shouldfail]` negative control's output.** Explained in
  [What a quiet run still prints](#what-a-quiet-run-still-prints) and
  deliberately left alone.
