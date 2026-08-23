# `Rational`, `DecimalPlaces`, `RationalError` — design

`morph::math::Rational` is a small, value-semantic, trivially-copyable struct
representing the exact rational number `numerator/denominator` with
`std::int64_t` components. It carries a runtime decimal-precision tag as a
strong type (`DecimalPlaces`). Arithmetic is exact — sums, differences,
products, and quotients are reduced to canonical form with no floating-point
rounding error. The precision tag affects only decimal scaling
(`Rational::fromFloat`) and rounding (`Rational::toDouble`, formatting); it
never changes a stored value.

Adapted from LASTRADA `JPMath/Rational.hpp`, with the `boxed` strong-type
dependency replaced by a self-contained `DecimalPlaces` and a Glaze wire codec
so the type round-trips through the morph JSON wire with its invariants restored
on read.

## Contents

- [Invariants](#invariants)
- [Support types](#support-types)
- [Construction](#construction)
- [Arithmetic](#arithmetic)
- [Overflow & value-range envelope](#overflow--value-range-envelope)
- [Mixed-type expressions (expected propagation)](#mixed-type-expressions-expected-propagation)
- [Conversion helpers](#conversion-helpers)
- [Rounding helpers (free functions)](#rounding-helpers-free-functions)
- [Comparison](#comparison)
- [Formatting](#formatting)
- [Wire and schema](#wire-and-schema)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Cross-references](#cross-references)
- [Limitations](#limitations)

## Invariants

Every public operation that produces a `Rational` restores:

- `denominator > 0` (strictly positive — never zero, never negative)
- `gcd(|numerator|, denominator) == 1`
- canonical zero is `0/1`
- `0 <= decimalPlaces.value <= kMaxDecimalPlaces`

All sign lives in the numerator.

**No default precision.** Every call site states the precision it intends, e.g.
`Rational{Numerator{1}, Denominator{3}, DecimalPlaces{9}}`. Precision is capped at `kMaxDecimalPlaces`
(18, the largest `k` for which `10^k` fits in `int64_t`); out-of-range values
assert in debug and clamp into `[0, kMaxDecimalPlaces]` in release. Zero decimal places is a
legal, first-class precision — a whole-number value (a zero-decimal currency such as JPY/KRW, or
a plain integer count) declares `DecimalPlaces{0}` and carries no fractional digit at all.

The struct never throws. Operations that may fail (zero divisor, non-finite
floating-point input, overflow during decimal scaling) return
`std::expected<Rational, RationalError>`.

## Support types

| Type | Role |
|---|---|
| `DecimalPlaces` | Strong type for a decimal-precision count. Prevents the precision from being confused with a numerator or denominator at a call site. Explicit construction; default-constructed `value` is `0`. |
| `Numerator` | Strong type for a Rational numerator. Prevents numerator/denominator argument swapping at construction sites. Explicit construction. |
| `Denominator` | Strong type for a Rational denominator. Explicit construction; must never be zero after canonicalisation. |
| `RationalError` | `enum class : std::uint8_t` with three values: `DivisionByZero`, `NotFinite` (non-finite float input), `Overflow` (scaled magnitude exceeds `int64_t`). |
| `kMaxDecimalPlaces` | `constexpr std::uint32_t = 18` — largest decimal precision supported. |

## Construction

| Path | Signature / example | Notes |
|---|---|---|
| Default | `Rational() noexcept` | Canonical zero (`0/1`) at precision 1. |
| Whole integer | `Rational(int64_t, DecimalPlaces) noexcept` | Stores `whole/1` at the given precision; clamped. |
| Full | `Rational(Numerator, Denominator, DecimalPlaces) noexcept` | Canonicalises: flips sign, reduces by gcd, clamps zero denominator to 1. |
| `from` | `static expected<Rational, RationalError> from(Numerator, Denominator, DecimalPlaces) noexcept` | Validating factory — rejects `denominator == 0` with `DivisionByZero` instead of clamping. |
| `fromFloat` | `static expected<Rational, RationalError> fromFloat(double/float/long double, DecimalPlaces) noexcept` | Lifts a floating-point value to a rational scaled to the requested precision. Returns `NotFinite` for NaN/Inf, `Overflow` when scaled magnitude exceeds `int64_t`. Not `constexpr`. All three overloads forward to one `fromFloatImpl` template that scales in `long double`, guards the range, then `llround`s. The result denominator is `10^dp` before canonicalisation. |
| `zero(p)` | `static constexpr Rational zero(DecimalPlaces) noexcept` | `0/1` at the given precision. |
| `one(p)` | `static constexpr Rational one(DecimalPlaces) noexcept` | `1/1` at the given precision. |

**Wire path.** The Glaze deserialisation path (`setWire`) rebuilds through the
canonicalising constructor, silently clamping what it cannot represent
(`den == 0`, out-of-range `dp`, a component whose magnitude does not fit)
instead of asserting, and counting the clamp for the decoding layer to act on.
See [Decoding cannot fail](#decoding-cannot-fail-so-the-clamp-is-reported-instead).

## Arithmetic

**Binary arithmetic propagates `std::max` of the two operands' `decimalPlaces`**
— the wider precision wins. Comparison (`<=>`, `==`) is purely value-based on
the canonical `(numerator, denominator)` pair and ignores `decimalPlaces`.

| Operation | Returns | Notes |
|---|---|---|
| `operator+`, `operator-`, `operator*` (plain `Rational` × `Rational`) | `Rational` | `noexcept`, return a bare `Rational` — no error channel. This means *representable* results never fail; it does **not** mean the operation cannot go wrong. Reduced int64 cross-terms exceeding ~2^63 are **undefined behaviour**, not a reported error (see [Overflow & value-range envelope](#overflow--value-range-envelope)). Reduce-before-multiply (Knuth 4.5.1) to extend safe int64 range. Cross-cancellation before multiplication. |
| `operator/`, `dividedBy` (plain `Rational` ÷ `Rational`) | `expected<Rational, RationalError>` | `DivisionByZero` when divisor's numerator is zero. Implemented by multiplying `*this` by the reciprocal built directly (`den/num`, sign carried onto the numerator), so it **also propagates `max` precision** — the division inherits the max-precision rule through its internal `*=` even though it returns `expected`. |
| `operator-` (unary) | `Rational` | Negates numerator. Precision preserved. **Negating `INT64_MIN` overflows.** |
| `reciprocal` | `expected<Rational, RationalError>` | Multiplicative inverse. `DivisionByZero` when the value is zero. **Precision is the operand's own `decimalPlaces`, not `max`** (it is a unary operation with no second operand to widen against). |
| `operator+=`, `-=`, `*=` (in-place) | `Rational&` | Mutate `*this`, widen precision to `max`, canonicalise. |

## Overflow & value-range envelope

`Rational` is **fixed-width `int64` arithmetic, not a bignum.** Both the
numerator and denominator are `std::int64_t`, and the additive/multiplicative
operators do their intermediate math in that same 64-bit type. This gives the
type a hard value-range envelope that the `noexcept` signatures do not advertise.

**`operator+` / `operator-` / `operator*` are `noexcept` and return a bare
`Rational` — but they can still be wrong.** The reduce-before-multiply and
cross-cancellation steps (Knuth 4.5.1) push the point at which the intermediate
products overflow, but they do not eliminate it. When a reduced cross-term
exceeds ~2^63 the signed multiplication/addition is **undefined behaviour**, not
a trapped or reported error:

- `operator+=` / `operator-=` compute `numerator * rightDenominatorScaled ±
  rhs.numerator * leftDenominatorScaled` and `denominator *
  rightDenominatorScaled`. Adding two fractions over large *coprime*
  denominators (nothing to cancel) grows the common denominator toward
  `d1 * d2`; both the scaled numerator and the product denominator can pass 2^63.
- `operator*=` cross-cancels first, but a genuinely large coprime product
  (`reducedLeftNumerator * reducedRightNumerator`, likewise the denominators)
  still overflows.

Because these operators have no error channel, an overflow here is **silent** —
the result is a garbage `Rational` (or a sanitizer trap under UBSan), never a
`RationalError`. Contrast the *only* fallible plain operator, `operator/`
(division), whose sole failure mode is a trivial divisor-is-zero check yet which
returns `std::expected`. The fallibility is inverted: the operation that almost
cannot fail is the one that reports, and the ones that carry real UB do not (see
[Limitations](#limitations)).

**`dp` → approximate maximum representable magnitude.** A value scaled to
precision `dp` (as `fromFloat` builds it) has denominator `10^dp`, so the
largest magnitude whose scaled numerator still fits `int64` is roughly
`INT64_MAX / 10^dp ≈ 9.22e18 / 10^dp`:

| `dp` | denominator `10^dp` | approx. max magnitude |
|---|---|---|
| 1 | 10 | ~9.2e17 |
| 2 | 100 | ~9.2e16 |
| 4 | 10^4 | ~9.2e14 |
| 6 | 10^6 | ~9.2e12 |
| 9 | 10^9 | ~9.2e9 (≈ 9.2 billion) |
| 12 | 10^12 | ~9.2e6 (≈ 9.2 million) |
| 15 | 10^15 | ~9223 |
| 18 | 10^18 | **≈ 9.2** |

At the maximum precision `dp = 18` the representable magnitude is only about
**±9.2** — a value tagged with 18 decimal places has essentially spent its whole
`int64` budget on the fractional part. `fromFloat` guards this edge explicitly,
but the arithmetic operators downstream do **not** re-check it, so intermediate
results that leave the envelope are UB regardless of the entry guard.

The `fromFloat` guard is **asymmetric** and half-ulp aware. After scaling in
`long double` it rejects with `Overflow` when `scaled >= 2^63 - 0.5` or
`scaled < -2^63` (exactly). The upper bound subtracts half an ulp because
`llround` rounds `[2^63 - 0.5, 2^63)` *up* to `2^63`, which overflows `int64`;
comparing against `INT64_MAX` would not help because on platforms where
`long double == double` that constant itself rounds up to `2^63`. The lower
bound is a strict `<` against `-2^63` because `INT64_MIN == -2^63` is a valid
result and `llround` maps `(-2^63 - 0.5, -2^63]` onto it. On x86's 80-bit
`long double`, `922337203685477580.75` scaled at `dp = 1` lands on exactly
`2^63 - 0.5` and is rejected by this window (it would otherwise `llround` up to
a poisoned `INT64_MIN` numerator); where `long double == double` the same
literal rounds past the bound and is rejected by the plain `2^63` check.

**`INT64_MIN` negation hazards.** `INT64_MIN` (`-2^63`) has no positive
counterpart in `int64`, so every place that negates a component is a latent UB
site when that exact value reaches it:

- **unary `operator-`** — `Rational{Numerator{-numerator}, ...}`: negating an
  `INT64_MIN` numerator overflows.
- **`from`** — guards **only** `denominator == 0`; it does not screen
  `INT64_MIN` components, so a hostile-but-nonzero `(INT64_MIN, …)` pair flows
  straight into the canonicalising constructor.
- **`reciprocal`** — negates the numerator in the `numerator < 0` branch;
  `INT64_MIN` there overflows.
- **`canonicalise`** — flips sign for a negative denominator (`numerator =
  -numerator`) and takes `absoluteNumerator = numerator < 0 ? -numerator :
  numerator`; both negate `INT64_MIN`. This is the shared sink for every
  constructor and operator, so any path that lets `INT64_MIN` reach
  canonicalisation is unsafe.

Only the wire codec (`setWire`) defends against this: it maps an `INT64_MIN`
`num`/`den` to `-INT64_MAX` *before* constructing, so untrusted input never
negates the trap value. In-code call sites get no such guard — keep operands
well inside the envelope above.

### Checked arithmetic

`operator+`/`operator-`/`operator*` are fixed-width `std::int64_t` arithmetic,
and at ledger-realistic magnitudes overflow is reachable — summing dp=2 legs of
10^9 minor units overflows at exactly `INT64_MAX / 10^9 + 1` rows.

**The operators saturate; they never overflow.** When the exact result — or any
intermediate cross-term needed to reach it — does not fit, the operator logs at
`error` and clamps to the largest representable magnitude *of the correct
sign*, rather than committing signed-overflow undefined behaviour. The sign
comes from exact comparison (`a + b` compares `a` against `-b`), not from the
operands' magnitudes, so a mixed-sign case whose cross-terms overflow still
saturates in the mathematically correct direction.

Saturation rather than an exception because these operators are used inside
strand-bound model code and in `constexpr` expressions: throwing would change
their contract for every existing caller, while leaving the overflow undefined
is what this exists to stop. A clamped value is wrong, but it is *defined*
wrong, and it is logged.

**The operators keep `noexcept`,** including when the overflow path logs, and
they need no local guard to do it: `morph::log`'s helpers are themselves
`noexcept` (`docs/spec/core/logger.md`, "Failure modes"), so an arithmetic
operator cannot begin failing because logging failed. If the record cannot be
emitted, the logging layer counts it in `morph::log::droppedLogRecords()`
rather than propagating. Both this function and `CompletionState`'s destructor
carried a local `try`/`catch` for this until morph#158 moved the guarantee to
where it belongs.

`canonicalise` is total for the same reason. It previously negated the
numerator unguarded, so a component of `INT64_MIN` was undefined behaviour —
reachable both by constructing such a value directly and by *ordinary
arithmetic landing on it exactly* (`-INT64_MAX - 1` is a legal subtraction
whose result is `INT64_MIN`). Such a component is now clamped to `-INT64_MAX`
and logged, matching what `setWire` already did for the same values arriving
off the wire.

`checkedAdd`, `checkedSub` and `checkedMul` return
`std::expected<Rational, RationalError>`, yielding `RationalError::Overflow`
rather than saturating. That is the division of labour: the operators stay
usable and defined for code that can absorb a clamped value, while the checked
forms stay exact-or-nothing for code that must not — a ledger totalling rows
needs to *stop*, not carry on with a clamped balance. They check every intermediate the
operation would form **before** forming any of it — detecting signed overflow
by performing it and inspecting the result is itself undefined, so the question
has to be answered from the operands alone.

**The intermediate cross-terms are the binding constraint, not the result.**
`checkedAdd` rejects operand pairs whose *final* value would have been
perfectly representable but which cannot be reached without an intermediate
that overflows — for instance `1/INT64_MAX + 1/(INT64_MAX-1)`, a tiny value
with an unrepresentable common denominator. This is precisely the case a caller
cannot detect by inspecting the answer, because under the unchecked operators
there is no valid answer to inspect.

`checkedMul` checks the *cross-cancelled* factors `operator*` actually
multiplies, not the raw operands: cross-cancelling is what keeps most products
in range, so checking beforehand would reject pairs that multiply perfectly
well (`INT64_MAX/2 * 2/1` reduces to `INT64_MAX/1`).

Both share one set of predicates (`addWouldOverflow`, `subWouldOverflow`,
`mulWouldOverflow`), so the operators and the checked forms cannot disagree
about what overflows.

## Mixed-type expressions (expected propagation)

Whenever an arithmetic expression contains an
`std::expected<Rational, RationalError>` sub-expression or a floating-point
operand, the whole expression evaluates to
`std::expected<Rational, RationalError>`. The float operand is lifted via
`Rational::fromFloat` — its precision is taken from the Rational operand's
`getDecimalPlaces()`. Errors short-circuit left to right.

```
auto const a = Rational{Numerator{7}, Denominator{2}, DecimalPlaces{9}};
auto const b = Rational{2, DecimalPlaces{9}};
auto const c = Rational{Numerator{1}, Denominator{2}, DecimalPlaces{9}};
auto result = a / b + c * 3.5;
// decltype(result) == std::expected<Rational, RationalError>
```

Implemented through constrained templates with `RationalLike`, `LiftableOperand`,
and `NeedsLifting` concepts. Valid operand combinations: Rational × Rational,
Rational × ExpectedRational, ExpectedRational × (anything liftable),
float × Rational/ExpectedRational. Two floats are not accepted (at least one
Rational-family operand must be present).

## Conversion helpers

| Member | Signature | Notes |
|---|---|---|
| `toDouble()` | `double toDouble() const noexcept` | Converts to `double`, rounded to this value's `decimalPlaces`. |
| `toDouble(n)` | `double toDouble(uint32_t) const noexcept` | Converts to `double`, rounded to `n` decimal places. Falls back to unrounded conversion when `n > 18`. |

**`toDouble` is a display/interop reading, never an exact path.** The
implementation computes `double(numerator) / double(denominator)` *first*, then
rounds the quotient to `n` decimal places (`std::round(raw * 10^n) / 10^n`).
The division happens in IEEE-754 `double`, whose mantissa holds only 53 bits:
any numerator or denominator beyond 2^53 (~9.0e15) is already rounded to the
nearest representable `double` **before** the decimal rounding runs, so the
result can differ from the exact rational even at magnitudes the `int64`
components represent perfectly. Treat `toDouble` as the way to *show* or hand a
`Rational` to a floating-point consumer — never as a lossless round-trip. The
exact value lives only in the `(numerator, denominator)` pair; the empty-spec
`"{}"` format (`"n/d"`) and the wire codec are the exactness-preserving
readings.

## Rounding helpers (free functions)

Free functions in `morph::math`, found by ADL:

| Function | Signature | Notes |
|---|---|---|
| `abs` | `constexpr Rational abs(Rational) noexcept` | Absolute value. Precision preserved. |
| `ceil` | `constexpr int64_t ceil(Rational) noexcept` | Rounds toward positive infinity. |
| `floor` | `constexpr int64_t floor(Rational) noexcept` | Rounds toward negative infinity. |
| `trunc` | `constexpr int64_t trunc(Rational) noexcept` | Truncates toward zero. |

## Comparison

| Operation | Notes |
|---|---|
| `operator<=>` | Three-way comparison. **Value-only: ignores `decimalPlaces`.** Exact for the full int64 range: cross-products computed in 128 bits (via `detail::mulU64`, a 64×64→128-bit product using `unsigned __int128` on GCC/Clang, portable 32-bit limbs on MSVC). Sign-checked first; zero short-circuits. |
| `operator==` | **Value-only: ignores `decimalPlaces`.** Returns `true` when canonical pairs are identical. |

## Formatting

`std::format` support is split by the supplied spec:

| Spec | Output | Example |
|---|---|---|
| empty `"{}"` | Exact rational form — `"n/d"`, or `"n"` when integer | `"7/2"`, `"3"` |
| non-empty `"{:.3f}"` | Delegated to `std::formatter<double>` on `toDouble()` | `"3.500"` |

Implemented as a `std::formatter<Rational>` specialisation with a
`delegateToDouble` flag set during `parse`.

## Wire and schema

Over the morph JSON wire a `Rational` travels as the object
`{"num":617,"den":50,"dp":2}`. Reading goes through the canonicalising
constructor, so a non-canonical payload (`1234/100`) or a hostile one
(`den == 0`, out-of-range `dp`) always lands as a valid, reduced value.

The Glaze codec (`glz::meta<Rational>`) uses `glz::custom<setWire, getWire>` to
route serialisation through the `Wire` struct. A `to_json_schema<Rational>`
specialisation preserves schema shape by delegating to `Wire`'s schema. The
`glz::meta` also fixes the schema type name to `"Rational"`.

### Decoding cannot fail, so the clamp is reported instead

`setWire` rebuilds through the canonicalising constructor, which clamps rather
than rejects. `{"num":5,"den":0,"dp":2}` therefore decodes to a plausible
`5/1`, and nothing downstream can tell the value was altered.

`Rational` reports the fact and stops there. `Wire::validate()` is the
predicate — non-canonical but representable input (`4/8`, a negative `den`) is
**valid**, since reducing and sign-normalising round-trip the same value — and
`WireClampScope` counts clamps across a decode:

```cpp
morph::math::WireClampScope clamps;
if (auto err = glz::read<opts>(action, json)) { ... }
if (clamps.clamped() != 0) { /* reject the payload */ }
```

Deciding what a clamp *means* is not this type's call. The same clamp is a
protocol violation when the bytes came off a socket and a harmless
normalisation when a local caller wrote them, and only the decoding layer
knows which. `ActionTraits<A>::fromJson` is that layer for action payloads —
`morph::wire` carries an execute envelope's `body` as an opaque string and
never parses it, so `fromJson` is the first and only place a `Rational` inside
it is decoded — and it rejects a clamped payload with `ParseError`.

**Absent fields fall back to `Wire`'s member defaults.** A payload missing a
key decodes to that field's default — `num = 0`, `den = 1`, `dp = 1` — so `"{}"`
reads as canonical zero at precision 1 (`Rational::zero(dp1)`), overwriting
whatever the destination held. Composition with `std::optional<Rational>` works
as expected: `"null"` decodes to an empty optional, and a present object decodes
through `setWire`.

## API reference

### `Rational` — data members

| Member | Type | Invariant |
|---|---|---|
| `numerator` | `int64_t` | Carries the sign of the rational value. |
| `denominator` | `int64_t` | Strictly positive. Never zero, never negative. |
| `decimalPlaces` | `DecimalPlaces` | `0 <= value <= kMaxDecimalPlaces`. |

### `Rational` — accessors

| Member | Returns |
|---|---|
| `getDecimalPlaces()` | `DecimalPlaces` — the current precision tag. |
| `isZero()` | `bool` — `numerator == 0`. |
| `isInteger()` | `bool` — `denominator == 1`. |
| `isNegative()` | `bool` — `numerator < 0`. |

### `Rational` — wire helpers

| Member | Signature |
|---|---|
| `checkedAdd(a, b)` | `constexpr expected<Rational, RationalError> noexcept` — exact sum, or `Overflow`. |
| `checkedSub(a, b)` | `constexpr expected<Rational, RationalError> noexcept` — exact difference, or `Overflow`. |
| `checkedMul(a, b)` | `constexpr expected<Rational, RationalError> noexcept` — exact product, or `Overflow`. |
| `setWire(Wire)` | `void noexcept` — rebuilds through the canonicalising constructor, clamping what it cannot represent and counting the clamp. |
| `Wire::validate()` | `constexpr bool noexcept` — whether these raw values decode without being clamped. |
| `WireClampScope` | Scoped observer: how many `Rational` values were clamped while decoding. |
| `getWire()` | `Wire noexcept` — canonical members ready for JSON encoding. |
| `struct Wire` | `{ int64_t num; int64_t den; uint32_t dp; }` — flat JSON representation. |

### `Rational` — `constexpr` non-member operators

```
Rational operator+(Rational, Rational) noexcept;
Rational operator-(Rational, Rational) noexcept;
Rational operator*(Rational, Rational) noexcept;
expected<Rational, RationalError> operator/(Rational, Rational) noexcept;
```

### `Rational` — mixed-type operator templates (namespace `morph::math`)

```
// At least one operand must be Rational-family; at least one must be
// ExpectedRational or float; both operands must be liftable. Two plain
// Rationals fail the NeedsLifting clause and take the non-template path.
template <typename Left, typename Right>
  requires (RationalLike<Left> || RationalLike<Right>)
        && (NeedsLifting<Left> || NeedsLifting<Right>)
        && (LiftableOperand<Left> && LiftableOperand<Right>)
expected<Rational, RationalError> operator+(Left const&, Right const&) noexcept;
// Same for -, *, /
```

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Precision | **Runtime tag (`DecimalPlaces`), not compile-time** | Precision is a property of the *value*, not the *type* — it is set by the data source and propagates through arithmetic dynamically. A compile-time tag would make every precision a distinct type, breaking cross-precision arithmetic without explicit conversion. |
| No default precision | **Caller must supply `DecimalPlaces` at every construction** | Prevents accidental use of a wrong or unknown precision. Every call site states what it intends. |
| Strong types for numerator/denominator | **`Numerator` / `Denominator` / `DecimalPlaces`** | Prevents argument-order mistakes (`Numerator{3}, Denominator{5}` vs `Numerator{5}, Denominator{3}`). `Numerator` / `Denominator` are `explicit`, so bare `Rational{3, 5, dp}` does not compile — the strong types must be spelled out. |
| Error handling | **`std::expected<Rational, RationalError>`** | The struct never throws. Fallible operations return an expected type; the caller decides how to handle errors. Short-circuit via mixed-type expression templates propagates failures. |
| Canonicalisation on `setWire` | **Silently clamps hostile input** | Untrusted wire data (den==0, out-of-range dp, INT64_MIN) always produces a valid reduced value rather than asserting or propagating UB. |
| Comparison ignores precision | **Value-only `<=>` and `==`** | Two values equal in magnitude should compare equal regardless of how many decimals they claim. Precision is a display/rounding concern, not a value property. |
| Max-precision propagation | **Result precision = max of operands** | A computation is never less precise than its most precise input. There is no in-place retag helper; a caller needing a different tag constructs a fresh `Rational` with the desired `DecimalPlaces`. |
| 128-bit cross-product comparison | **`detail::mulU64`** | Exact ordering over the full int64 range without overflow. Uses `unsigned __int128` when available (GCC/Clang), portable 32-bit limb decomposition on MSVC. |
| Negation limitation | **`INT64_MIN` overflows** | Documented limitation. The wire codec clamps `INT64_MIN` components away for untrusted input. |
| `fromFloat` not `constexpr` | **Uses `std::llround` / `std::isfinite`** | These standard library functions are not `constexpr`. The `fromFloat` overloads are `inline` out-of-class, `noexcept` but not `constexpr`. |

## Cross-references

| Spec | Relationship |
|---|---|
| [`quantity_type.md`](quantity_type.md) | `Rational` is the **runtime substrate** for `Quantity`. A `Quantity`'s declared precision and the forms layer's `x-decimalPlaces` schema annotation both resolve, at runtime, to a `Rational`'s `DecimalPlaces` tag — the `dp` value carried on the wire and propagated through arithmetic here is exactly the precision a `Quantity` declares. The overflow envelope and `INT64_MIN` hazards documented above therefore bound `Quantity` too. |
| [`forms.md`](../forms/forms.md) | The form generator reads `x-decimalPlaces` (and the `Rational` wire shape `{"num","den","dp"}`) to build precision-aware numeric inputs; a form value is a `Rational` under the hood, so its display uses `toDouble`/formatting and its exact value uses the wire codec. |
| [`security.md`](../security.md) | `setWire` performs the untrusted-wire **clamping** (`den == 0 → 1`, out-of-range `dp` → `[0, 18]`, `INT64_MIN` → `-INT64_MAX`). This is the boundary defence that keeps a hostile payload from reaching the UB-prone negation/overflow sites; see the clamping semantics discussion there. |
| [`datetime.md`](datetime.md) | Contrast case for wire-decode policy: the `DateTime` codec is **strict** (rejects malformed input) whereas `Rational::setWire` is **lenient/clamping** (silently repairs it). See [Limitations](#limitations) for why the difference matters. |

## Limitations

- **Fixed-width `int64`, not a bignum — with silent overflow UB in the
  "safe-looking" operators.** `+`, `-`, and `*` are `noexcept` and hand back a
  bare `Rational`, which reads as "infallible" but means the opposite for
  out-of-envelope inputs: a reduced cross-term past ~2^63 is undefined
  behaviour, produced *silently*. The fallibility is inverted — `operator/`,
  whose only failure is a trivial divisor-is-zero check, returns
  `std::expected`, while the genuinely dangerous `+`/`-`/`*` do not. See
  [Overflow & value-range envelope](#overflow--value-range-envelope) for the
  `dp` → magnitude table.
- **`setWire` clamps hostile input rather than rejecting it.** `den == 0`
  becomes `1`, an `INT64_MIN` component becomes `-INT64_MAX`, an out-of-range
  `dp` is pulled into `[0, 18]` (only the upper bound can ever fire, since `dp`
  is unsigned and `0` is itself a legal precision). The invariants are always
  restored, but a *corrupt amount silently becomes a specific wrong number* —
  e.g. a payload meant to carry `x/0` lands as `x/1`, a completely different
  value, with no error surfaced. This is deliberate (a `Rational` never
  propagates UB from the wire) but it trades detectability for robustness. It
  is the opposite policy from the strict `DateTime` codec, which rejects
  malformed input outright (cross-ref [`datetime.md`](datetime.md)); a caller
  that needs "reject, don't guess" semantics for amounts must validate before
  decode.
- **`==` and `<=>` ignore precision, so equality is not substitutability.**
  Comparison is purely value-based on the canonical `(numerator, denominator)`
  pair. Two `Rational`s can therefore satisfy `a == b` while
  `a.toDouble() != b.toDouble()`, because `toDouble()` rounds to each value's
  *own* `decimalPlaces`: e.g. `7/8` tagged `dp = 1` reads `0.9` while the same
  `7/8` tagged `dp = 2` reads `0.88`, yet the two compare equal. Equal values
  are not freely interchangeable in a floating-point context — precision is a
  display property the comparison does not see.
- **Float operands in mixed expressions are lifted at the Rational operand's
  `dp`.** In a mixed expression (`someRational * 3.5`) the float is converted
  via `fromFloat` using the precision of the Rational-family operand
  (`liftPrecision`), which **snaps the literal onto that decimal grid before the
  operation**. A coarse `dp` silently quantises the float: with a `dp = 1`
  Rational, `3.57` is lifted to `36/10` (i.e. `3.6`) before multiplying, so the
  literal you wrote is not the value used. The grid is chosen by the Rational,
  not the float.