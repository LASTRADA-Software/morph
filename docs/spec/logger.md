# The `morph::log` logging system — design

`morph::log` is a lightweight, thread-safe logging facility built on C++23
`<print>` and `<format>`. It provides a global log sink, runtime level
filtering, and a RAII guard for test isolation.

There is no logger object to construct — the API is a set of namespace-scope
free functions operating on a hidden `LogState` singleton.

## Contents

- [Log levels](#log-levels)
- [Global state](#global-state)
- [Log-injection sanitisation](#log-injection-sanitisation)
- [Level helpers](#level-helpers)
- [Scoped override](#scoped-override)
- [Thread safety](#thread-safety)
- [Failure modes](#failure-modes)
- [Lifetime](#lifetime)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Log levels

Five severity levels, ordered from most to least verbose:

| Enumerator | Value | Meaning |
|---|---|---|
| `debug` | 0 | Fine-grained diagnostic output |
| `info` | 1 | General informational messages |
| `warn` | 2 | Recoverable conditions worth noting |
| `error` | 3 | Errors that should be investigated |
| `off` | 4 | Suppresses all output when set as minimum level |

The ordering is used for runtime filtering: a message is emitted only when
the sink is non-null **and** `level >= logState().minLevel` (the `uint8_t`
value comparison). Setting `minLevel` to `off` silences everything, and
installing a null sink (`setLogger(nullptr)`) suppresses all output without
crashing.

The level comparison is the hot path and is deliberately made cheap: `minLevel`
is a `std::atomic<LogLevel>`, so the reject decision is a single relaxed atomic
load with **no mutex acquisition** (see [Thread safety](#thread-safety)).

## Global state

A single `detail::LogState` singleton holds:

- **`sink`** — a `std::function<void(LogLevel, std::string_view)>` called for
  every message that passes the level filter. Default: writes
  `[LEVEL] sanitizeLogLine(msg)` to `stderr` (newline/control-char sanitisation,
  see [Log-injection sanitisation](#log-injection-sanitisation)). Guarded by `mtx`.
- **`minLevel`** — the minimum `LogLevel` to emit, as a
  `std::atomic<LogLevel>`. Default: `debug` (everything passes). Read/written
  lock-free with relaxed ordering; **not** guarded by `mtx`.
- **`mtx`** — a `std::mutex` protecting `sink` and serialising sink invocation.

The two fields have different concurrency disciplines:

- **`minLevel` is lock-free.** `setLogLevel` is a relaxed atomic `store` and
  `getLogLevel` is a relaxed atomic `load` — neither touches `mtx`. This makes
  the level a cheap, contention-free fast path that both `detail::log` and
  `detail::logFormat` consult before doing any expensive work.
- **`sink` is mutex-guarded.** `setLogger` acquires `mtx` to swap the sink, and
  `detail::log` holds `mtx` for the duration of the sink call, so sink
  invocations are serialised with respect to each other and to `setLogger` /
  `ScopedLoggerOverride`.

The relaxed memory order is intentional: level filtering is advisory (a message
racing a concurrent `setLogLevel` may be emitted or dropped depending on
timing), so no cross-thread happens-before relationship on the level is needed,
and relaxed is the cheapest correct choice.

## Log-injection sanitisation

A log message is a single logical record, and the default sink emits it as one
physical line (`[LEVEL] msg\n`). User-controlled text containing a newline could
otherwise **forge a log line** — a message like `"login ok\n[ERROR] breach"`
would appear as two lines, the second a fabricated `[ERROR]` record — or corrupt
a line-oriented log parser with embedded control bytes.

The default sink defends against this by passing every message through
`detail::sanitizeLogLine` before writing:

- `\n`, `\r`, `\t` become the C-style escapes `\n`, `\r`, `\t`.
- Any other control byte (`< 0x20`, or `0x7f` DEL) becomes a `\xHH` escape.
- Printable text — including non-ASCII UTF-8 continuation bytes (`>= 0x80`) —
  passes through untouched.

It is a single cheap pass that does **no allocation on the common (already-clean)
path**: it scans for the first byte needing escaping and returns a plain copy if
there is none. The escaped result is always a single line, so a forged `\n`
cannot splice a fake record into the stream.

**Sanitisation lives in the *default sink*, not in the emit path.** `detail::log`
still hands the sink the raw `std::string_view`; only the shipped `stderr` sink
sanitises. A custom sink installed via `setLogger` receives the unsanitised
message and is responsible for its own escaping (or for routing to a structured
backend where raw bytes are safe) — `sanitizeLogLine` is a public
`detail`-namespace helper a custom sink can reuse. This keeps the fast path free
of forced escaping for sinks that do not need it, while the out-of-the-box
behaviour is safe.

## Level helpers

Eight public free functions — four taking a plain `std::string_view`, and four
overloaded on `std::format_string` + variadic args — each hardcoding one level:

```cpp
logDebug("message");                     // plain string
logInfo("count: {}", n);                 // format string
logWarn("timeout after {}ms", ms);
logError("{}: {}", err.what(), code);
```

The format variants delegate to `detail::logFormat`, which checks the level
against `minLevel` **before** calling `std::format` and returns early if the
message is suppressed — so a call like `logDebug("… {} …", expensiveArgs)`
under a higher `minLevel` pays no `std::format` and no string-allocation cost;
only the (already-evaluated) argument expressions and the atomic load are paid.
When the level passes, `logFormat` formats and forwards to `detail::log`, which
re-checks the level (again lock-free) before taking the mutex.

The plain (`std::string_view`) variants call `detail::log` directly, which
performs the same lock-free level reject before locking.

## Scoped override

`ScopedLoggerOverride` is a RAII guard that swaps the global sink and level
for the lifetime of the object, restoring them in the destructor. Designed for
tests that want to capture log output without leaking the custom sink into
other tests.

Two constructors:

- **Default** — snapshots the current sink and level without changing them.
  Use when test code will install its own sink mid-test via `setLogger()` /
  `setLogLevel()` and just wants automatic restoration.
- **Explicit** — takes a new sink and optional level (defaults to `debug`).
  Installs them immediately; restores the previous values on destruction.

Copy and move are deleted — the guard is not copyable.

## Thread safety

The design splits work between a lock-free fast path and a mutex-guarded slow
path:

- **Level check — lock-free.** Both `detail::log` and `detail::logFormat`
  compare `level` against `minLevel.load(relaxed)` and bail out before touching
  the mutex or doing any formatting. `setLogLevel` / `getLogLevel` are a relaxed
  atomic `store` / `load`. Reading or changing the level never contends with
  logging on another thread.
- **Sink invocation — serialised under `mtx`.** When a message passes the
  level check, `detail::log` takes `state.mtx` and calls the sink **while
  holding the lock**. This means at most one sink call runs at a time, and it
  cannot race a concurrent `setLogger` or a `ScopedLoggerOverride`
  construction/destruction (all of which also take `mtx`). The default
  `stderr` sink is independently thread-safe, but serialising also keeps
  interleaved messages from arbitrary user sinks intact.

Because the sink runs under the global, **non-recursive** `std::mutex`, a sink
**MUST NOT** call back into `morph::log` from within its own invocation:

- calling any `logDebug`/`logInfo`/`logWarn`/`logError` (or `detail::log`
  directly) — the nested `detail::log` would try to re-lock `mtx`;
- calling `setLogger` / `setLogLevel`-via-sink paths that lock `mtx`;
- constructing or destroying a `ScopedLoggerOverride`.

Any of these re-enters the held mutex and **self-deadlocks** (`std::mutex` is
not recursive — re-locking from the same thread is undefined behaviour, in
practice a hang). Note `setLogLevel` / `getLogLevel` alone are safe to call
from a sink because they do not take `mtx` — but there is no legitimate reason
to, and doing so is still poor form.

A sink should also **not block for long**: it stalls every other thread trying
to log (they queue on `mtx`) for the full duration of the call. Do expensive or
blocking work (network, disk with fsync, cross-thread handoff) off the logging
thread.

## Failure modes

- **A single string argument is never a format string.** `logInfo(s)` for a
  `std::string`/`std::string_view`/`const char*` `s` binds the plain
  `std::string_view` overload, not the `std::format_string` template. Any `{}`
  in `s` is therefore emitted **literally** and cannot throw
  `std::format_error` — the text is passed straight through to the sink. Use
  the variadic overload (`logInfo("{}", s)`) only when you actually want `s`
  treated as a format string with arguments. This is why untrusted or
  brace-containing strings are safe to pass as the sole argument.
- **A throwing sink propagates.** Exceptions from the sink are **not** caught.
  They unwind out of `detail::log` (releasing `mtx` via the `scoped_lock`
  destructor as the stack unwinds) and out of the originating
  `logDebug`/`logInfo`/… call into the caller. The logging layer adds no
  `try/catch`; a sink that must not disrupt its caller has to swallow its own
  exceptions internally.
- **Format-time errors (variadic overload).** Argument formatting is checked at
  compile time by `std::format_string`, so mismatched placeholders are a build
  error, not a runtime one. A runtime `std::format_error` is only possible from
  a dynamically-constructed format string, which this API does not accept.

## Lifetime

`detail::logState()` returns a reference to a function-local
`static LogState` — a Meyers singleton. First-use initialisation is
thread-safe (the C++ runtime guards the static's construction), and the object
lives until static-destruction at program exit.

**Do not log from static-destruction paths.** Once `LogState` itself is
destroyed, any later `morph::log` call (e.g. from another static object's
destructor, or an `atexit` handler ordered after `LogState`) touches a
destroyed `std::mutex`/`std::function`/`std::atomic` — a use-after-free with
undefined behaviour. There is no destruction-order guarantee between `LogState`
and other translation-unit statics, so avoid logging from any code that can run
during static teardown.

## API reference

### `LogLevel`

| Member | Signature | Notes |
|---|---|---|
| `debug` | `enum class LogLevel : std::uint8_t` | Value 0 |
| `info` | | Value 1 |
| `warn` | | Value 2 |
| `error` | | Value 3 |
| `off` | | Value 4 |

### Configuration functions

| Symbol | Signature | Notes |
|---|---|---|
| `setLogger` | `void setLogger(std::function<void(LogLevel, std::string_view)>)` | Replaces the global sink. Thread-safe: **takes `mtx`**. Pass a no-op lambda (or `nullptr`) to silence all output — a null sink is skipped by the emit path. |
| `setLogLevel` | `void setLogLevel(LogLevel)` | Sets the minimum level. Messages below this level are silently dropped. Thread-safe and **lock-free** — a relaxed atomic `store`, does not take `mtx`. |
| `getLogLevel` | `LogLevel getLogLevel()` | Returns the current minimum level. Thread-safe and **lock-free** — a relaxed atomic `load`, does not take `mtx`. |

### Level helpers

| Symbol | Overloads | Notes |
|---|---|---|
| `logDebug` | `(std::string_view)` / `(std::format_string<Args...>, Args&&...)` | Emits at `LogLevel::debug` |
| `logInfo` | `(std::string_view)` / `(std::format_string<Args...>, Args&&...)` | Emits at `LogLevel::info` |
| `logWarn` | `(std::string_view)` / `(std::format_string<Args...>, Args&&...)` | Emits at `LogLevel::warn` |
| `logError` | `(std::string_view)` / `(std::format_string<Args...>, Args&&...)` | Emits at `LogLevel::error` |

### `ScopedLoggerOverride`

| Member | Signature | Notes |
|---|---|---|
| default ctor | `ScopedLoggerOverride()` | Snapshots current sink + level; does not change them. The **sink** snapshot/restore is serialised under the global mutex against `setLogger` and `detail::log`. The **level** is the lock-free atomic `minLevel`: it is read/written inside the same critical section but that does **not** serialise it against `setLogLevel`/`getLogLevel` (which never take the mutex), so a concurrent `setLogLevel` racing an override's save/restore is a last-writer-wins atomic race, not a mutually-exclusive one. Keep level changes on one thread while an override is live. |
| explicit ctor | `ScopedLoggerOverride(std::function<void(LogLevel, std::string_view)>, LogLevel = debug)` | Installs the given sink and level; saves previous values. Same guarantee as above: the sink swap is mutex-serialised; the level swap is an atomic store not serialised against lock-free `setLogLevel`. |
| dtor | `~ScopedLoggerOverride()` | Restores saved sink and level. Thread-safe. |
| copy/move | deleted | Not copyable or movable. |

### Internal detail (not for direct use)

| Symbol | Signature | Notes |
|---|---|---|
| `detail::levelName` | `constexpr std::string_view levelName(LogLevel) noexcept` | Returns `"DEBUG"`, `"INFO "`, `"WARN "`, `"ERROR"`, `"OFF  "` (padded to 5 chars). Falls back to `"?    "` for any out-of-range enum value. |
| `detail::Logger` | `using Logger = std::function<void(LogLevel, std::string_view)>` | Sink function type. |
| `detail::sanitizeLogLine` | `std::string sanitizeLogLine(std::string_view)` | Escapes newline/CR/TAB (as `\n`/`\r`/`\t`) and other control bytes (`< 0x20`, `0x7f`) as `\xHH`, leaving printable/UTF-8 text intact. Used by the default sink to prevent log-injection; allocation-free when the input is already clean. Reusable by custom sinks. |
| `detail::log` | `void log(LogLevel, std::string_view)` | Internal emit entry point. First does a **lock-free** `level >= minLevel` check (relaxed atomic load) and returns early if suppressed; only then acquires `mtx` and calls the sink if it is non-null. The sink runs **while `mtx` is held** (see [Thread safety](#thread-safety)). |
| `detail::logFormat` | `template<typename... Args> void logFormat(LogLevel, std::format_string<Args...>, Args&&...)` | Checks `level >= minLevel` **before** calling `std::format` (lock-free) and returns early if suppressed, so a filtered formatted call pays no formatting/allocation cost. When the level passes, formats via `std::format` and forwards to `detail::log`. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| API surface | **Namespace-scope free functions, no logger object** | The simplest possible integration — no dependency injection, no global instance to manage. A single hidden `LogState` singleton is sufficient for application-level logging. |
| Level filtering | **Runtime `uint8_t` comparison, not template-based** | Levels are known at runtime from configuration; a virtual dispatch or template per level adds complexity with no benefit. The `>=` comparison on the enum's underlying value is branch-predictable and cheap. |
| Sink type | **`std::function<void(LogLevel, std::string_view)>`** | Erases any callable (lambda, function pointer, callable object) without requiring the user to subclass an interface. The cost of a `std::function` invocation is negligible relative to I/O. |
| Thread safety | **Split: lock-free `std::atomic<LogLevel>` for the level, `std::mutex` for the sink** | The level is the per-call hot path, read on every log call; making it an atomic keeps filtering contention-free and lets a suppressed formatted call reject before `std::format`. The sink is a `std::function` that cannot be swapped atomically, so it keeps the mutex — which also serialises sink calls. The default `stderr` sink is independently thread-safe, but serialising keeps arbitrary user sinks' output from interleaving. Consequence: the sink runs under a non-recursive mutex and must not re-enter `morph::log` (see [Thread safety](#thread-safety)). |
| Default sink | **`std::println(stderr, "[{}] {}", levelName(lvl), sanitizeLogLine(msg))`** | C++23 `std::println` is the modern replacement for `fprintf` — type-safe, no format string mismatch, and it appends a newline automatically. The `[LEVEL]` prefix is concise and grep-friendly. The message is run through `sanitizeLogLine` so a newline/control byte in user-controlled text cannot forge a second log line (see [Log-injection sanitisation](#log-injection-sanitisation)). |
| Sanitise in the sink, not the emit path | **Only the default sink escapes; `detail::log` passes the raw view** | Keeps the fast path allocation-free for custom sinks that route to structured backends where raw bytes are safe; the out-of-the-box `stderr` sink is still safe by default, and custom sinks can reuse the public `detail::sanitizeLogLine`. |
| Format support | **Two overloads per level: `std::string_view` and `std::format_string` + variadic args** | The plain overload avoids forcing `std::format` on callers with a raw string; the variadic overload provides type-safe formatting. SFINAE on `std::format_string` ensures the format args are checked at compile time. |
| `ScopedLoggerOverride` | **RAII guard with two constructors (snapshot-only and install), deleted copy/move** | The default constructor enables a pattern where test fixtures set up their own logger mid-test and still get automatic restoration. The explicit constructor is the common case. Deleted copy/move prevents double-restore. |
| No `operator<<` | **`std::format` / `std::print` only** | Consistent with C++23 direction — no iostreams dependency, no ADL pitfalls, no locale contamination. |
| No compile-time level filtering | **All levels compiled in; filtering is runtime only** | The system is small enough that the dead code from unused levels is negligible. A compile-time toggle would require `if constexpr` at every call site or a macro-based approach, neither of which is justified here. |

## Limitations

These are deliberate scope choices, not defects — but they bound where this
logger is the right tool:

- **One global sink, one serialised call.** There is a single process-wide
  sink invoked under `mtx`, so all logging from all threads funnels through one
  serialised call. A slow or blocking sink stalls every other thread's logging
  for the duration of each call. There is no fan-out to multiple sinks and no
  async/queued handoff — if you need those, wrap them inside your own sink
  (and keep the part that runs under `mtx` short).
- **Sink signature is only `(LogLevel, std::string_view)`.** The sink receives
  a level and a fully-formatted message and nothing else — no source location,
  no category/logger name, no timestamp, no thread id. Structured routing
  (per-category filtering, per-field indexing, correlation ids) is not possible
  without smuggling that data into the message string. Callers that need it
  must encode it into the text and parse it back out in the sink.
- **No compile-time level elision.** Every call site is compiled in and reaches
  at least the atomic level check at runtime; the argument expressions of a
  formatted call are always evaluated even when the message is suppressed
  (only the `std::format` step is skipped). There is no macro or `if constexpr`
  that removes suppressed call sites from the binary.
- **Only the default sink sanitises.** Newline/control-char escaping
  (`sanitizeLogLine`) is applied by the shipped `stderr` sink, not by
  `detail::log`. A custom sink installed via `setLogger` receives the raw
  message and must sanitise itself (it may reuse `detail::sanitizeLogLine`) if it
  writes to a line-oriented target — otherwise user-controlled text can inject
  forged lines through *that* sink. See
  [Log-injection sanitisation](#log-injection-sanitisation).
- **`ScopedLoggerOverride` swaps *global* state.** The override replaces the one
  global sink/level, so it isolates only tests that run **serially**. Two tests
  running concurrently (or code on a background thread) share the same override
  window and will see each other's sink. It is a serial-test-isolation helper,
  not per-thread or per-scope logging.

## Cross-references

- **`completion.md`** and ARCHITECTURE.md's *Error propagation* section — the
  error-handling path is a primary user of this logger: an abandoned
  `Completion` with no `.onError` handler logs the pending exception through the
  configured sink from its destructor (non-`std::exception` types are logged as
  `"unknown exception"`). This is exactly the kind of destructor-time logging
  the [Lifetime](#lifetime) note warns about — safe during normal execution,
  but not from static-destruction ordering after `LogState` is gone.
- **`ScopedLoggerOverride` usage** — the standard pattern for capturing log
  output in tests without leaking a sink across cases; it mirrors
  `morph::journal::ScopedActionLog` (see `journal.md`), which applies the same
  RAII save/restore idiom to the action-log sink. Because both swap global
  state, tests using them must run serially (see [Limitations](#limitations)).
- ARCHITECTURE.md *Logger* section — the one-paragraph framework-level summary:
  all framework internals route through `morph::log::detail::log`, and
  applications call `setLogger` once at startup to redirect to spdlog, Qt
  logging, or a test spy.