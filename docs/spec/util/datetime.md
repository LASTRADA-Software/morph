# The `DateTime` / `Timestamp` types — design

Two types in namespace `morph::time`, mirroring the `Rational` / `Quantity`
split:

- **`morph::time::DateTime`** — the *value*: a UTC instant with millisecond
  precision over `std::chrono::sys_time<std::chrono::milliseconds>` (adapted
  from LASTRADA `Toolbox/Chrono.hpp`). Cheap to copy, ordered, duration
  arithmetic.
- **`morph::time::Timestamp`** — the *field*: an optionally-empty `DateTime`
  with the same one-kind-of-empty semantics as `Quantity` (a form draft starts
  blank; required-ness is derived by `morph::forms`).

## Contents

- [Wire format](#wire-format)
- [Schema](#schema)
- [`DateTime`](#datetime)
  - [Construction](#datetime-construction)
  - [Access and inspection](#datetime-access-and-inspection)
  - [Arithmetic and ordering](#datetime-arithmetic-and-ordering)
- [`Timestamp`](#timestamp)
  - [Free functions (namespace scope)](#free-functions-namespace-scope)
  - [Empty-state semantics](#empty-state-semantics)
- [The `now()` override seam](#the-now-override-seam)
- [Glaze codec](#glaze-codec)
- [`std::format` support](#stdformat-support)
- [Design decisions](#design-decisions)
- [Range & precision](#range--precision)
- [Failure modes — what is rejected](#failure-modes--what-is-rejected)
- [Limitations](#limitations)
- [Cross-references](#cross-references)
- [Out of scope](#out-of-scope)

## Wire format

A `DateTime` travels as the ISO-8601 string `YYYY-MM-DDTHH:MM:SS[.mmm]Z`
(UTC only). On output the fractional part and trailing `Z` are **always**
written; on input both are optional. The parser is a strict, hand-rolled
routine — no locale, no `std::chrono::parse` dependency — so behaviour is
identical across libstdc++, libc++ (WASM), and MSVC.

Unlike the `Rational` codec, which clamps hostile input into a valid value, a
malformed timestamp has no meaningful clamp: the codec rejects it as a JSON
**read error**.

## Schema

A `Timestamp` field renders as `{"type": ["string","null"],
"format": "date-time"}` — the standard JSON-Schema vocabulary form renderers
key on (the demo clients render a date-time input for it).

## `DateTime`

The underlying instant is `std::chrono::sys_time<std::chrono::milliseconds>` —
Unix time with leap seconds ignored.

### `DateTime` — construction

| Member | Signature | Notes |
|---|---|---|---|
| `value` | `std::chrono::sys_time<std::chrono::milliseconds>` | The underlying UTC instant (public data member). |
| default ctor | `constexpr DateTime() noexcept` | The Unix epoch (1970-01-01T00:00:00.000Z). |
| sys_time ctor | `constexpr DateTime(sys_time<milliseconds>) noexcept` | Wraps an existing UTC instant. |
| calendar ctor | `constexpr DateTime(std::chrono::year, std::chrono::month, std::chrono::day, std::chrono::hours, std::chrono::minutes, std::chrono::seconds, std::chrono::milliseconds = milliseconds{0}) noexcept` | Composes from calendar/clock components. Performs **no** validation (unlike `fromIso8601`): a valid `year_month_day` is a caller precondition, and out-of-range components yield an unspecified instant. |
| `now()` | `static DateTime now() noexcept` | The current UTC instant, truncated to milliseconds — or the installed override's instant, if one is active. See [The `now()` override seam](#the-now-override-seam). |
| `fromIso8601(text)` | `static std::optional<DateTime> fromIso8601(string_view) noexcept` | Strict parser (see [Wire format](#wire-format)); returns `nullopt` when malformed. |

### `DateTime` — access and inspection

| Member | Signature | Returns |
|---|---|---|
| `toIso8601()` | `std::string toIso8601() const` | `YYYY-MM-DDTHH:MM:SS.mmmZ` via `std::format`. |

### `DateTime` — arithmetic and ordering

| Member | Signature | Notes |
|---|---|---|
| `operator<=>` | `constexpr std::strong_ordering operator<=>(DateTime const&) const noexcept = default` | Delegates to the underlying `sys_time`. |
| `operator+=` | `template<Rep, Period> constexpr DateTime& operator+=(duration<Rep, Period>) noexcept` | Shifts by any chrono duration (`duration_cast` to ms). |
| `operator-=` | `template<Rep, Period> constexpr DateTime& operator-=(duration<Rep, Period>) noexcept` | Shifts backwards. |
| `operator+(lhs, delta)` | `template<Rep, Period> friend constexpr DateTime operator+(DateTime, duration<Rep, Period>) noexcept` | Returns a new shifted instant. |
| `operator-(lhs, delta)` | `template<Rep, Period> friend constexpr DateTime operator-(DateTime, duration<Rep, Period>) noexcept` | Returns a new shifted instant. |
| `operator-(lhs, rhs)` | `friend constexpr auto operator-(DateTime const&, DateTime const&) noexcept` | Returns `lhs.value - rhs.value` as a chrono duration. |

## `Timestamp`

The blank state ("not entered / not recorded") lives inside the struct, exactly
like `Quantity`: action structs never wrap a `Timestamp` in `std::optional`,
and a non-optional `Timestamp` member is *required* by `morph::forms` rules.

| Member | Signature | Notes |
|---|---|---|---|
| `value` | `std::optional<DateTime>` | The payload; `std::nullopt` means empty (public data member). |
| default ctor | `constexpr Timestamp() noexcept` | The **empty** state. |
| DateTime ctor | `constexpr Timestamp(DateTime) noexcept` | Engages with the given instant. |
| optional ctor | `constexpr Timestamp(std::optional<DateTime>) noexcept` | Adopts an optional payload as-is. |
| `now()` | `static Timestamp now() noexcept` | `Timestamp{DateTime::now()}` — delegates to `DateTime::now()`, so it observes the same override. |
| `hasValue()` | `constexpr bool hasValue() const noexcept` | Engaged? No implicit `bool` conversion. |
| `operator*` | `constexpr DateTime const& operator*() const noexcept` | Unchecked access to the engaged value (UB when empty, like `std::optional`). |
| `operator<=>` | `constexpr auto operator<=>(Timestamp const&) const noexcept = default` | Default ordering on the optional payload; empty sorts before engaged. |

### Free functions (namespace scope)

| Symbol | Signature | Notes |
|---|---|---|
| `operator-(lhs, rhs)` | `constexpr std::optional<std::chrono::milliseconds> operator-(Timestamp const&, Timestamp const&) noexcept` | The signed duration between two engaged timestamps; `nullopt` when either is empty. |

### Empty-state semantics

- **Default construction.** `Timestamp{}` is empty.
- **Querying.** `hasValue()` returns `false` when empty; no implicit `bool`.
- **Comparison.** `operator<=>` is total — empty compares less than any engaged
  timestamp; two empties compare equal (`std::optional` default ordering).
- **Wire.** An empty `Timestamp` serializes as JSON `null` (or, as a struct
  member, is omitted — the glaze `meta` delegates to `std::optional<DateTime>`).
- **Difference.** `lhs - rhs` returns `nullopt` if either operand is empty.

## The `now()` override seam

`DateTime::now()` (and therefore `Timestamp::now()`, which delegates to it) is
not a bare `system_clock::now()` call — it first consults a process-wide,
mutex-guarded override slot, falling back to real wall-clock time only when
no override is installed. This is the injection point for deterministic
testing of time-dependent behavior in models that are **registry-constructed**
(`morph::model::detail::ModelRegistryFactory::create`, `docs/spec/core/registry.md`)
and therefore have no constructor parameter through which a test could hand
them a mock clock: a test fixes "now" once, for a scope, and every
`DateTime::now()`/`Timestamp::now()` call anywhere in that scope — including
deep inside a model's `execute()` — observes the fixed instant, with **zero
change to production call sites**.

```cpp
namespace morph::time {
    void setNowOverride(std::function<DateTime()> clock);  // nullptr/empty clears it

    class ScopedNowOverride {
    public:
        explicit ScopedNowOverride(DateTime fixedInstant);       // constant "now"
        explicit ScopedNowOverride(std::function<DateTime()> clock);  // custom callable
        ~ScopedNowOverride();  // restores the previous override
        // move/copy disabled
    };
}
```

- **`setNowOverride(clock)`** installs @p clock as the override; every
  subsequent `DateTime::now()` call invokes it instead of
  `system_clock::now()`. Passing an empty `std::function` (or `nullptr`)
  clears the override and restores real wall-clock time. Thread-safe (guarded
  by the same mutex `now()` reads through).
- **`ScopedNowOverride`** is the RAII installer applications and tests should
  reach for instead of calling `setNowOverride` directly — it mirrors
  `morph::journal::ScopedActionLog` and `morph::log::ScopedLoggerOverride`
  (the same scoped-install-then-restore shape used for the codebase's other
  process-wide override seams): the constructor saves whatever override was
  active before (possibly none), installs the new one, and the destructor
  restores exactly what it saved — so nested guards unwind correctly and one
  test case's fixed "now" never leaks into the next. Non-copyable,
  non-movable (an RAII guard with no meaningful transfer semantics, matching
  `ScopedActionLog`).
- **Two constructors**: `ScopedNowOverride{someDateTime}` fixes "now" to a
  constant instant — the common case ("this record expires 24 hours after
  `2026-01-01T00:00:00Z`"); `ScopedNowOverride{someCallable}` installs an
  arbitrary `DateTime()`-returning callable for scenarios that need "now" to
  advance across calls within the same test (a fake clock that ticks).

**The override callable must not throw and must not be reentrant.**
`DateTime::now()` is `noexcept` and calls the installed override directly — an
exception escaping the callable therefore escapes a `noexcept` function and
calls `std::terminate()`. The callable also runs *while `now()`'s internal
mutex is held*, so it must not itself call `DateTime::now()`/`Timestamp::now()`,
`setNowOverride`, or construct a `ScopedNowOverride`: the mutex is
non-recursive, and any of those self-deadlocks on the calling thread. This
mirrors the identical constraint on `morph::log`'s sink callback (`logger.hpp`:
*"A sink must therefore not call back into `morph::log`... `std::mutex` is
non-recursive and that would self-deadlock"*) — the same hazard class, for the
same reason.

```cpp
TEST_CASE("expiry logic") {
    morph::time::ScopedNowOverride guard{morph::time::DateTime::fromIso8601("2026-01-01T00:00:00Z").value()};
    // Anywhere in this scope, including inside a registry-constructed model's
    // execute(), DateTime::now()/Timestamp::now() return the fixed instant --
    // no model constructor parameter needed.
    auto holder = morph::model::detail::ModelRegistryFactory::instance().create("SubscriptionModel");
    // ... exercise time-dependent behavior deterministically ...
}
```

The seam is process-wide, not per-instance: it is intended for test scopes
(one `ScopedNowOverride` per test case, or per `SECTION`) and for tools that
need to pin "now" globally (a replay/import job re-processing historical
data), not as a way to give two concurrently-running models two different
simulated clocks — see [Limitations](#limitations).

## Glaze codec

Specialisations in `glz` (and `glz::detail` for schema):

- `from<JSON, DateTime>` — reads a JSON string and calls
  `DateTime::fromIso8601`; malformed input sets `error_code::syntax_error`.
- `to<JSON, DateTime>` — writes `DateTime::toIso8601()` as a JSON string.
- `meta<Timestamp>` — declares the wire layout as the underlying
  `std::optional<DateTime>` (nullable on the wire, null = empty).
- `to_json_schema<DateTime>` — produces a JSON-Schema string with
  `"format": "date-time"`.

## `std::format` support

`std::formatter<DateTime>` renders the ISO-8601 UTC string.
An empty format spec `{}` or `{:}` is accepted; any non-empty spec throws
`std::format_error`. No `operator<<` is provided.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Precision | **Milliseconds** | Sufficient for domain timestamps without the cost/scope of microsecond/nanosecond. |
| Default state | **Unix epoch, not empty** | `DateTime` is a pure value wrapper; the empty/optional distinction lives in `Timestamp`. |
| Parser | **Hand-rolled, strict** | Identical behaviour across all standard libraries (no `std::chrono::parse` dependency, no locale). `from_chars` for each numeric field, with a leading-sign guard on the fixed-width fields (only the year may carry `-`); `year_month_day.ok()` for calendar validity. |
| Malformed input | **Read error, not clamped** | Unlike `Rational`, there is no meaningful fallback for "yesterday" — reject at the wire boundary. |
| Wire output | **Always `.mmmZ`** | The canonical form is the most precise, unambiguous representation; optional precision on input is a leniency, not the output contract. |
| Timestamp empty state | **Inside the struct**, not `std::optional<Timestamp>` | Same one-kind-of-empty design as `Quantity`; required-ness is a `morph::forms` property, not a type property. |
| Formatting | **`std::formatter` only** | Single formatting path; no `operator<<`. |
| Time zone support | **UTC only** | All application timestamps are UTC; no time zone offset parsing, no local time storage. A non-UTC input (`+02:00`) is rejected as malformed. |
| `now()` injection | **Process-wide mutex-guarded override slot**, not a constructor parameter or thread-local | Registry-constructed models (`ModelRegistryFactory::create`) have no constructor parameter a test could use to inject a clock — see `docs/spec/core/registry.md`. A global override consulted by `DateTime::now()` itself needs no change to any call site or model constructor; `ScopedNowOverride` bounds its lifetime to a test/tool scope. Mirrors the existing `setActionLog`/`ScopedActionLog` and logger-override seams rather than inventing a new pattern. |

## Range & precision

The *storage* — `std::chrono::sys_time<std::chrono::milliseconds>` — spans far
more than a millennium in each direction (the `sys_days` count alone reaches
well beyond `year{-32767}`/`year{32767}`). The *wire codec*, however, is
narrower than the value it carries:

- **Output** (`toIso8601`) formats the year with `{:04}` — a minimum-width-4,
  zero-padded field, *not* a fixed-width one. Years 1–9999 emit exactly four
  digits; year `10000` emits five (`"10000-…"`), a negative year in
  −1…−999 emits a `-` and three digits (`"-001-…"`, `"-999-…"`, four
  characters total), and a year ≤ −1000 emits a `-` and four-or-more digits
  (`"-1000-…"`, five-or-more characters).
- **Input** (`fromIso8601`) reads the year as the *first four characters*
  (`number(0, 4, allowSign=true)`) and then requires a `-` at offset 4. The
  year field is the **one place a leading `-` is legitimate**: for a year in
  −1…−999 the four characters are `-` plus three digits, `std::from_chars`
  reads the negative value, and the separator lands exactly at offset 4, so it
  round-trips. A five-or-more-digit magnitude (year `10000`, year ≤ −1000)
  pushes the `-` separator past offset 4 and **fails the separator check**.

The consequence: **years −999…9999 round-trip.** (Year 0 is the astronomical
year zero, emitted as `"0000"`; the Gregorian proleptic calendar `chrono`
uses has no year 0 gap, so it round-trips like any other.) Anything outside
that band — year ≤ −1000 or ≥ 10000 — is representable as a `DateTime` value
(and can be constructed, compared, and arithmetic-shifted in memory) but
cannot survive a serialize→parse cycle: the serialized text won't re-parse
(the widened year shifts the `-` separator off offset 4). This is treated as a
**producer precondition**: application timestamps live inside the round-tripping
band, and feeding an out-of-band instant to the wire codec is a lossy operation
the codec does not guard against. Millisecond precision is the other axis of the
same contract — a 4th fractional digit is not silently dropped (see
[Failure modes](#failure-modes--what-is-rejected)).

The pinned boundary is exercised by `DateTime::Iso8601::NegativeYearRoundTrip`
in `tests/test_datetime.cpp` (−1 and −999 round-trip; −1000 does not).

## Failure modes — what is rejected

`fromIso8601` is deliberately strict. RFC 3339 permits several forms this parser
rejects, and the rejections are load-bearing (they keep the wire form canonical
and cross-stdlib-identical), so they are enumerated here.

| Input | Result | Why |
|---|---|---|
| `2026-07-05T14:30:15.123456Z` | **read error** | A 4th (or later) fractional digit is *not* truncated — the loop consumes at most three digits, then the leftover `456Z` is trailing input and `cursor != text.size()` fails. Precision loss is a parse failure, never a silent round-down. |
| `2026-07-05T14:60:00Z` | **read error** | Leap second (`:60`). RFC 3339 *allows* `:60`; this parser bounds seconds at `> 59`. |
| `2026-07-05t14:30:15z` | **read error** | Lowercase `t`/`z`. RFC 3339 *allows* both cases; this parser matches only the uppercase literals `'T'` and `'Z'`. |
| `2026-07-05T14:30:15+02:00` | **read error** | Non-UTC zone offset — trailing input after the seconds field. UTC only. |
| `2026-07-05T14:30:15.` | **read error** | A `.` with no following digit (`digits == 0`). |
| `2026-02-30T10:00:00Z` | **read error** | Calendar date that does not exist (`year_month_day::ok()` is false). |
| `2026-07-05T-5:30:15` | **read error** | Sign injection. `std::from_chars` accepts a leading `-`, so without a guard this would read `hour = -5` — passing the one-sided `hour > 23` range check — and silently resolve to a *different* valid instant (`2026-07-04T19:30:15Z`). The parser rejects a leading `-`/`+` in every fixed-width date/clock field (month, day, hour, minute, second) and in the fraction. |
| `2026-07-05T14:-5:15`, `2026-07-05T14:30:-5` | **read error** | Same sign injection in the minute / second field. |
| `2026-07-05T+5:30:15`, `2026-+7-05T14:30:15` | **read error** | A leading `+` is rejected in the fixed-width fields for the same reason. |
| `2026-07-05T14:30:15.-5Z` | **read error** | A signed fraction: the `-` is not a digit, so the fraction loop consumes nothing (`digits == 0`). |
| **`2026-07-05T14:30:15`** (no `Z`) | **accepted, interpreted as UTC** | The trailing `Z` is *optional* on input. |

**The silent-UTC asymmetry.** A zone offset (`+02:00`) is rejected, yet a
zone-*less* string is silently accepted and assumed UTC. So the parser refuses
to *convert* a stated non-UTC time but happily *assumes* UTC for a string that
stated no zone at all — a string that names the wrong zone is safer (it fails
loudly) than one that names none (it is taken at face value). Producers that
cannot guarantee UTC should append the explicit `Z` so the intent is on the
wire, even though the parser does not require it.

**Diagnostics are coarse.** `fromIso8601` returns a bare `std::optional` — every
failure above collapses to `std::nullopt`, discarding *which* field or check
failed. The glaze `from<JSON, DateTime>` adapter then maps that single
`nullopt` to a single `error_code::syntax_error`. There is no "bad month" vs.
"bad fraction" vs. "trailing junk" distinction at the wire boundary; the caller
learns only that the string was not a valid canonical UTC timestamp.

## Limitations

- **No date-only, time-of-day, or duration types.** There is one temporal field,
  `Timestamp`, and it always carries a full instant. A birthday, a due-date, or
  an opening time all drag a `HH:MM:SS.mmm` component whether the domain wants
  one or not, and the schema always emits `format: date-time` — a renderer has
  **no signal** to offer a bare date picker (there is no `format: date` /
  `format: time` / duration emission). Callers encode "midnight UTC" or similar
  conventions by hand.
- **The cross-stdlib-identical claim is test-backed, not proven.** The parser is
  hand-rolled precisely so that libstdc++, libc++ (WASM), and MSVC agree; that
  agreement rests on the round-trip and boundary tests
  (`DateTime::Iso8601::RoundTrip`, `DateTime::Iso8601::RejectsMalformedInput`,
  `DateTime::Iso8601::RejectsSignInjection`, and
  `DateTime::Iso8601::NegativeYearRoundTrip` in `tests/test_datetime.cpp`)
  exercising the same inputs everywhere, not on a formal argument. A stdlib-specific `year_month_day::ok()` or `from_chars`
  discrepancy would only surface as a test failure on that platform.
- **An empty `Timestamp` sorts *before* every real instant.** `Timestamp`'s
  defaulted `operator<=>` delegates to `std::optional<DateTime>`, whose ordering
  puts a disengaged optional below every engaged one. So a blank/not-yet-entered
  timestamp is the *smallest* value — sorting a list of records by a
  `Timestamp` column floats the un-entered ones to the top, which is frequently
  the opposite of the domain intent ("undated = unknown = should sort last").
  This is a `std::optional` artifact, not a deliberate temporal choice; callers
  that need "empty sorts last" must special-case it (mirroring the
  `hasValue()`-before-ordering guidance in `quantity_type.md`).
- **Several converting constructors are implicit.** `DateTime(sys_time<ms>)`,
  `Timestamp(DateTime)`, and `Timestamp(std::optional<DateTime>)` are all
  non-`explicit`. This is convenient (`Timestamp t = DateTime::now();`, brace-init
  of action members) but means a stray `sys_time`, `DateTime`, or
  `optional<DateTime>` converts silently at call sites and in overload
  resolution.
- **The `now()` override is a single process-wide slot, not per-thread or
  per-instance.** `ScopedNowOverride` swaps one global override in and back
  out; it does not give two concurrently-running models (or two threads)
  independent simulated clocks, and a `ScopedNowOverride` installed on one
  thread affects `DateTime::now()` calls made concurrently on every other
  thread for its lifetime. This is adequate for the intended use (one test
  case at a time fixing "now" for whatever it exercises in-process) but is not
  a per-context clock injection mechanism — tests that run cases in parallel
  within one binary, or that need two different simulated instants live at
  once, are outside what this seam provides.

## Cross-references

- **`forms.md`** — `Timestamp` satisfies the `EmptyCapableField` concept via
  `hasValue()`, and `allRequiredEngaged<A>()` treats a non-optional `Timestamp`
  member as a **required-field gate**: the action is not "ready" until that
  timestamp is engaged. This is the whole reason the empty state lives *inside*
  `Timestamp` rather than in a `std::optional<Timestamp>` wrapper.
- **`quantity_type.md`** and **`choice.md`** — the same *one-kind-of-empty*
  pattern: exactly one representation of "not entered" (`value == nullopt` /
  the disengaged variant), a `hasValue()` query, no implicit `bool`, and an
  unchecked `operator*`. `Timestamp` is the third member of this family.
- **`rational.md`** — the instructive **contrast**. `Rational`'s codec *clamps*
  hostile input into a valid value (there is a meaningful fallback); `DateTime`'s
  codec *rejects* it as a read error (there is no meaningful "nearest valid
  timestamp"). Same framework, opposite boundary policy, chosen per type.
- **`../core/registry.md`** — `ModelRegistryFactory::create` constructs
  registry-registered models with no constructor parameter, which is exactly
  why `DateTime::now()`/`Timestamp::now()` need a seam that does not go
  through a constructor: `ScopedNowOverride`, consulted by `now()` itself,
  reaches a model's `execute()` regardless of how the model was constructed.
- **`../journal/journal.md`** — `journal::setActionLog`/`ScopedActionLog`, the
  action-log override this seam's shape (mutex-guarded slot + RAII
  scoped-install-and-restore helper) is modeled on.

### Worked example — a required `Timestamp` field

An action struct declares the timestamp as a plain, non-optional member. That
non-optionality is what makes it *required*:

```cpp
struct RecordMeterReading {
    morph::time::Timestamp observedAt;  // required: not std::optional, not defaulted-away
    std::string            meterId;
};

RecordMeterReading draft{};                       // observedAt is empty (blank form)
morph::forms::allRequiredEngaged(draft);          // false — observedAt not engaged
draft.observedAt = morph::time::DateTime::now();  // implicit DateTime -> Timestamp
morph::forms::allRequiredEngaged(draft);          // observedAt gate now satisfied
```

The schema fragment generated for the field (the nullable string + `date-time`
format the demo renderers key on):

```json
"observedAt": { "type": ["string", "null"], "format": "date-time" }
```

An empty `observedAt` serializes as `null` (or is omitted as a struct member);
a `null` on read means "not entered", not "epoch".

## Out of scope

- Sub-millisecond precision — the storage is `sys_time<milliseconds>`.
- Time zone conversion or zone-aware arithmetic — UTC exclusively.
- `std::chrono::parse`-based parsing — the hand-rolled parser is deliberate.
