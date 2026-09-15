# Quiet by default — design

`morph::log`'s default minimum level stops being `debug` and becomes `warn`,
unconditionally — the same in every build configuration. The Catch2 test
binaries go further and default to `off`, because expected error-path output is
still noise in a test run, and they take a `--log-level` switch for the times a
developer wants it back. CI sets `MORPH_TEST_LOG_LEVEL=debug` workflow-wide, so
the full stream is in the CI log when something needs diagnosing after the
fact, and out of the way locally.

## Contents

- [The noise](#the-noise)
- [Why a library default alone is not enough](#why-a-library-default-alone-is-not-enough)
- [The library default](#the-library-default)
- [The test gate](#the-test-gate)
- [Linking the gate](#linking-the-gate)
- [CI turns it back on](#ci-turns-it-back-on)
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
| stdout | 23 | Catch2 banner, the `[!shouldfail]` control, the summary |
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
exercising error paths (`[orphan] unhandled exception: silent`,
`default-sink-coverage`, a torn-record recovery warning), which is why the run
exits 0.

## Why a library default alone is not enough

`warn` fixes the library's manners: an application that links morph no longer
has its `stderr` taken over by dispatch tracing. It does not make a test run
quiet. The 54 `[ERROR]`/`[WARN ]` lines above survive a `warn` default, and
they are the lines most likely to be misread — a developer scanning output for
why something broke finds `[ERROR] [orphan] unhandled exception: silent` and
has to go read `test_completion_branches.cpp` to learn it was supposed to
happen.

So the library default and the test gate answer different questions. The
default decides what an *application* hears from morph. The gate decides what a
*test run* prints, and the right answer there is nothing, because Catch2
assertions — not log records — are how a test reports a problem.

Keeping the default unconditional, rather than varying it on `NDEBUG`, is a
deliberate reversal of an earlier revision of this design. A build-type-
dependent default meant `LogState`'s member initializer differed between
translation units compiled with different settings — two definitions of one
type and one inline function, an ODR violation the linker resolves silently and
arbitrarily. One default in every configuration removes that whole class of
problem, and costs only that a debug build no longer turns on debug logging by
itself. `setLogLevel(LogLevel::debug)` is one line, and the test binaries and CI
both have their own explicit controls below.

## The library default

```cpp
// include/morph/core/logger.hpp:113
-    std::atomic<LogLevel> minLevel{LogLevel::debug};
+    std::atomic<LogLevel> minLevel{LogLevel::warn};
```

That is the whole library change. No macro, no preprocessor branch, no build
flag — `setLogLevel()` is already public, already documented, and already the
designed control surface; this changes which side of it is the default. An
earlier revision added a `MORPH_DEFAULT_LOG_LEVEL` escape-hatch macro; it
existed only to let a consumer defuse the ODR hazard, and with the hazard gone
it is a second way to do what `setLogLevel()` already does. Dropped.

`detail::log` compares `level < minLevel` and returns early, so `debug` and
`info` records are now rejected on the lock-free fast path — no mutex, no
`std::format`, no allocation. Suppressed logging gets cheaper, not more
expensive.

`ScopedLoggerOverride`'s explicit constructor keeps its `LogLevel::debug`
default parameter. That constructor is the fixture most logger-sensitive tests
already use, and it sets the level itself, so those tests are unaffected by the
ambient default in either direction.

The eleven example entry points (`examples/*/src/*/main.cpp`, `src/main.cpp`)
need **no edit**: they call `logError`, and `warn` lets errors through. An
earlier revision added a `setLogLevel` line to each; under an unconditional
`warn` that is redundant, and it is dropped.

## The test gate

Catch2 event listeners cannot add command-line options — the parser is built
before listeners exist. Taking the level from the CLI therefore means owning
`main` and driving `Catch::Session` directly, which is supported and verified
against the pinned Catch2 3.15.3: `Catch::Session` exposes `cli()` /
`cli(Parser const&)` and `applyCommandLine(argc, argv)`
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
$ ./morph_tests                              # quiet
$ ./morph_tests --log-level=debug            # today's full output
$ ./morph_tests --log-level debug            # Clara accepts both spellings
$ ./morph_tests -? | grep log-level          # documented in the built-in help
```

The option appears in `--help` alongside Catch2's own, which is the main reason
to prefer it to an environment variable: a developer looking for the switch
finds it where they already look.

An unrecognised value on either path is a mistake the developer wants to know
about, not a silent fallback: `applyLogLevel` writes one line naming the switch
and the accepted values, and `main` returns 1 — so a typo fails loudly instead
of running the whole suite at the wrong level.

The gate sets the level only. It does not replace the sink, so a test that
installs its own sink is unaffected, and the default `stderr` sink is still
what a developer sees when they opt back in.

## Linking the gate

Test targets stop linking `Catch2::Catch2WithMain` and link `Catch2::Catch2`
plus a new `morph_test_main` static library carrying the `main` above. An
archive is safe here — unlike a Catch2 listener, which registers through a
static initializer the linker may drop with no undefined symbol to pull it in,
`main` is always an undefined symbol, so the object is always extracted.

```cmake
add_library(morph_test_main STATIC tests/test_main.cpp)
target_link_libraries(morph_test_main PUBLIC morph::morph Catch2::Catch2)
```

Ten targets link `Catch2::Catch2WithMain` today and move to `morph_test_main`:

```
morph_tests   morph_net_tests   morph_offline_sqlite_tests   morph_bench
morph_soak    bank_tests        bank_gui_tests               morph_concepts_tests
morph_vetted_hmac_{libsodium,openssl}_tests
```

**Four already define their own `main`** and cannot link a second one — an
earlier revision of this design listed them as `Catch2WithMain` consumers,
which was wrong:

```
morph_qt_tests   morph_net_qt_interop_tests
ladder_common_tests   morph_forms_controller_core_tests
```

Each owns a `QCoreApplication` whose lifetime must bracket the run, so each
keeps its `main` and calls `morph::testkit::configureSession()` from inside it.
They link `morph_test_log_level`, an INTERFACE target carrying the header and
its include path, which `morph_test_main` also links — so all fourteen binaries
share one implementation of the option and cannot drift.

`catch_discover_tests` keeps working: it invokes the binary with `--list-tests`
and friends, which `session.applyCommandLine` handles exactly as Catch2's own
`main` does. That it still enumerates 1,437 cases is part of the verification
below, because a broken `main` shows up as an empty test list — a silent pass,
not a failure.

## CI turns it back on

CI runs every suite through `ctest --preset …` (`ci.yml:113`, `:291`, `:429`,
`:538`, `:726`, `:816`, `:1161`, `:1414`, `:1616`), and `catch_discover_tests`
registers each test case as its own ctest invocation — so `--log-level` cannot
reach the binaries that way. `MORPH_TEST_LOG_LEVEL` is the input that can, and
it goes in the workflow-level `env:` block that already exists at `ci.yml:29`:

```yaml
env:
  MORPH_TEST_LOG_LEVEL: debug
```

One line, inherited by every job and every leg, including the Valgrind job
(`ci.yml:1718`) which invokes `morph_tests`, `morph_net_tests` and
`morph_offline_sqlite_tests` directly rather than through ctest.

This is a deliberate asymmetry, not an oversight: **local runs are quiet, CI
runs are verbose.** A developer at a terminal wants the signal; a CI log is
read after the fact, by someone diagnosing a failure that has already happened,
and the full stream is what makes that possible. The cost is real — roughly
2,949 extra lines per suite per leg, and the Valgrind job pays it three times
over — and is accepted for that reason. `--log-level` takes precedence over the
variable, so a developer who has exported it for another purpose is never
overridden by it locally.

## Tests that depended on the default

Thirteen sites install a sink with a bare `setLogger()` after a snapshot-only
`ScopedLoggerOverride`, and never set a level — so they inherit whatever is
ambient. With the gate forcing `off`, the nine that **capture and assert**
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
| `tests/test_logger.cpp:46` | the level and message the sink received |
| `tests/test_logger.cpp:97` | nothing — see below |

The last two were missed by the first audit, which excluded `test_logger.cpp`
on the strength of its aggregate counts (20 `setLogger` calls, 27 level-setting
calls) rather than checking each case. `:46` failed outright once the gate
landed. `:97` is the more instructive one: it installs a **null** sink to prove
a null sink does not crash, and asserts only `REQUIRE(true)`. Under `off`,
`detail::log` returns on the level check *before* reaching the `if
(state.sink)` branch the test exists to exercise — so it would have kept
passing while measuring nothing, which is exactly the failure mode
`AGENTS.md` names. It now installs the null sink at `debug` and asserts the
level it is running at.

The other four (`tests/test_sync_worker.cpp:279`, `:526`, `:546`,
`tests/test_coverage_push95.cpp:507`) install a no-op sink purely for silence
and do not care about the level.

All eleven move to `ScopedLoggerOverride`'s explicit constructor, which installs
the sink **and** sets the level under one acquisition of the global mutex:

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

- **One locked operation instead of two.** The snapshot and install happen under
  a single `scoped_lock`, which is what the class's own comments say the pairing
  is for — a snapshot straddling separate `setLogger` and `setLogLevel` calls
  can restore a combination that never existed.
- **The level is stated at the site**, so a future change to the ambient level
  cannot silently break these tests again. This design is itself such a change;
  had the sites been written this way, none would have needed touching.
- **The sink cannot leak** past the test: install and restore are one object's
  lifetime.

**Declaration order matters.** The captured object (`logged`, `orphanCount`,
`sawNull`) must be declared *before* the guard. Destruction runs in reverse
order, so the guard uninstalls the sink first and the captured object dies
second; the reverse leaves a sink holding a dangling reference during teardown.
The existing code declares the guard first, so each conversion swaps two lines.

## Pinning both defaults

Two things can silently regress, so each gets its own check.

**The library default.** The gate mutates the global level at run start, so a
test cannot read `getLogLevel()` to learn what the library would have chosen. It
constructs a fresh state instead, which is order-independent and immune to the
gate:

```cpp
morph::log::detail::LogState fresh;
REQUIRE(fresh.minLevel.load() == morph::log::LogLevel::warn);
```

**The gate itself.** A test asserts the ambient level is `off` whenever neither
`--log-level` nor `MORPH_TEST_LOG_LEVEL` was given. This has teeth: if `main`
failed to apply the level, the ambient value would be the library default,
`warn`, which differs from `off`. The test must consult the same two inputs the
gate does, so a developer running `--log-level=debug` — or anyone running under
CI, where the variable is set to `debug` — does not see a spurious failure.
Since `main` owns the resolved level, it records it in a small accessor the test
reads, rather than both re-deriving it and drifting apart.

Note the consequence for CI: under `MORPH_TEST_LOG_LEVEL=debug` this test
asserts the level is `debug`, not `off`. It therefore verifies the *mechanism*
in CI and the *default* locally, and only a run with the variable unset proves
the quiet default. That is what the local verification below is for.

## Versioning

`docs/spec/core/logger.md:56` documents `Default: debug (everything passes)`.
Per `AGENTS.md` the spec is the contract, so it changes with the code; per
`docs/spec/VERSIONING.md` a change to documented behaviour of the stable surface
is normally a major release with a deprecation window.

The project is at **0.1.0** — pre-1.0, so no deprecation marker and no window
are required, and the CHANGELOG entry carries the notice. `logger.md:142`
(`ScopedLoggerOverride`'s explicit constructor defaults to `debug`) stays
correct and is untouched.

## Testing

- **The measurement that motivated this, repeated.** With `MORPH_TEST_LOG_LEVEL`
  unset, `morph_tests 2>&1 | wc -l` must reach 23, down from 2,972. Run it in
  `gcc-debug` as well as `gcc-release` — the behaviour is now
  configuration-independent, and that claim is worth one command rather than an
  assumption.
- **The gate is proven to run, not assumed to link.** The pin test above fails
  if `main` never applied the level. Additionally, stub out the `applyLogLevel`
  call once and confirm the test fails — a check never observed failing is not a
  check.
- **`catch_discover_tests` still enumerates every case.** A custom `main` that
  mishandles `--list-tests` makes ctest register nothing, which is a silent
  pass. Confirm `ctest -N` still lists 1,437 cases.
- **`--log-level` is discoverable and validated.** `morph_tests -?` shows the
  option; `--log-level=nonsense` exits non-zero naming the accepted values.
- **Both inputs restore the old output, and CLI wins.** `--log-level=debug` and
  `MORPH_TEST_LOG_LEVEL=debug` each produce 2,949 stderr lines; with
  `MORPH_TEST_LOG_LEVEL=debug ./morph_tests --log-level=off`, the run is quiet.
- **The library-default pin fails on revert.** Restore
  `minLevel{LogLevel::debug}` and confirm it fails.
- **Full suite green** under `gcc-debug`, `gcc-release` and `clang-asan`. The
  eleven converted sites are where a mistake lands, and a botched
  declaration-order swap is a use-after-free that only the sanitizer build
  catches reliably.

## What a quiet run still prints

A default run is **24 lines**, not zero, and neither remaining piece is
suppressible by a log level.

**One `[ERROR]` line on stderr.** `tests/test_coverage_gaps.cpp:344` covers the
default sink's lambda body, and the default sink's only observable behaviour
*is* writing to `stderr` — there is no seam to watch it through, so exercising
it means letting it write. The message says so in place
(`"default-sink-coverage: expected, proves the default sink body ran"`), since
it is the sole log record a quiet run emits and would otherwise read as a stray
error.

**Thirteen lines of `FAILED:` block on stdout,** which is not a log record:

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
either scope or the control's legibility. This section exists so the next person
to run a quiet suite and see `FAILED:` knows in one search that it is the point
rather than a regression.

## Out of scope

- **Changing what any call site logs, or at what level.** The 2,895 `[DEBUG]`
  `dispatchMessage` records are not reclassified or removed; they stay exactly
  as useful to anyone who turns debug back on.
- **A CMake option for the test level.** `--log-level` and `MORPH_TEST_LOG_LEVEL`
  are runtime inputs, changeable without reconfiguring; a configure-time twin
  would add a third place to look when output is missing.
- **Auditing other test binaries' output.** The rest inherit the gate; none is
  examined here for noise of its own beyond what the level fixes.
- **`docs/spec/testing_charter.md`.** The gate changes what a test run prints,
  not what the suite guarantees.
- **The `[!shouldfail]` negative control's output.** Explained above and
  deliberately left alone.
