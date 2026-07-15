# The `Quantity` type — design

`morph::units::Quantity` is a domain number that knows three things about
itself:

1. **Its unit** — carried as a compile-time template tag, so `kg` and `m³`
   cannot be mixed by accident and combinations are checked when the code is
   compiled, not when it runs.
2. **Its exactness** — the payload is an exact `morph::math::Rational`, never a
   `double`, with a declared and a runtime notion of precision. Empty is a real
   state (`std::nullopt` = "not entered / not measured").
3. **How it was computed** — its *derivation*. A computed quantity can render
   itself as an equation with named variables, shown symbolically and evaluated.

The third point is the reason the type exists rather than a plain `double`. In
a domain application (financial, engineering, metering) the answer alone is not
enough — you have to be able to show the working: *why* is the density
`0.83 kg/m³`, *which* inputs fed the surplus, *what* was 3 % of what. A
`Quantity` carries that explanation with the value instead of reconstructing it
after the fact.

## Contents

- [Units are types](#units-are-types)
- [Exact value and precision](#exact-value-and-precision)
  ([Empty state](#empty-state--stdnullopt-behavior))
- [Provenance — a build-time toggle](#provenance--a-build-time-toggle-on-by-default)
- [Named symbols](#named-symbols)
- [Formatting](#formatting--stdformatter-only)
- [Wire and schema](#wire-and-schema)
- [Unit conversion — `UnitRelation` and `convert`](#unit-conversion--unitrelation-and-convert)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Usage example](#usage-example)
- [Out of scope](#out-of-scope)

## Units are types

`Quantity<Unit::kg>` and `Quantity<Unit::m3>` are distinct types and cannot be
mixed accidentally. The application supplies:

- a `UnitTraits<Enum>` specialisation — id, display text, default decimal
  places, and a flat list of **peer-to-peer `UnitRelation` entries** that
  declare exact conversion ratios between same-dimension units with a
  `morph::math::Rational`. Each entry `{Unit::g, Unit::kg, Rational{1, 1000}}`
  generate a `convert` in both directions; a `UnitTraits<E>::convert` static
  takes precedence when the conversion is not a simple ratio (e.g. Celsius ↔
  Fahrenheit).
- a `consteval` unit algebra (`operator*` / `operator/` on the enum) so
  cross-unit products and quotients deduce their result unit at compile time.
  `Mass / Volume → Quantity<Unit::kg_per_m3>`. An unsupported combination does
  not compile at the call site that attempted it.

There is **no library-provided dimensionless unit**. The result of dividing two
same-unit quantities is whatever the application's own `operator/` returns for
`a / a` — by convention a `scalar` / `ratio` enumerator the application defines
(and typically formats with an empty display string). The framework does not
inject one; a unit system that wants a dimensionless result must declare the
enumerator and the algebra rule for it.

Addition and subtraction are defined **only between identical units** —
`kW + kW`, not `kW + W`. Negation and scaling by a dimensionless `Rational` are
likewise provided directly. To add or subtract values of *different but
convertible* units (`kW + W`), the right operand is converted to the left
operand's unit first; that automatic conversion is what `convert` /
`operator Quantity<To>()` exist for (see *Unit conversion*).

## Exact value and precision

Two precisions exist by design and must not be conflated:

- **Declared precision** (a template argument) — a property of the *field*: how
  many decimals this slot is specified to hold. Defaults from the unit's
  `UnitTraits` metadata, overridable per field (`Quantity<Unit::m3, 4>`). It
  feeds the generated schema (`x-decimalPlaces`), `fromDouble`, and
  `atDeclaredPrecision`.
- **Actual precision** — a property of the *value*: the runtime `DecimalPlaces`
  tag inside the `Rational`. It max-propagates through arithmetic and can be
  retagged at run time (`withDecimalPlaces`, `atDeclaredPrecision`).

**Max-propagation, precisely.** During a calculation the actual precision of a
result is the **maximum `DecimalPlaces` among the engaged (non-empty) operands** —
the widest declared value wins, so a computation is never *less* precise than its
most-precise input. (Empty operands make the whole result empty and so
contribute no precision.) A computed *temporary* has no field of its own to draw
a declared precision from, so its **declared** precision is the deduced result
unit's `UnitTraits` default; call `atDeclaredPrecision()` to retag the actual
precision back to that default before display when max-propagation has widened
it past what the field is specified to show.

**Magnitude is bounded by `int64`.** The exact payload is a ratio of 64-bit
integers, so `+`, `-`, `*`, and ratio composition can overflow when a reduced
numerator or denominator exceeds `int64` range (roughly `9.2·10¹⁸`). This is a
**documented limitation, not a handled case**: `Quantity` does not turn overflow
into empty — it inherits `Rational`'s behaviour. Division by zero *is* handled
(it yields empty); overflow is not. Domain values are expected to stay well
within that range; a computation that could approach it should be scaled or
reworked, not relied on to saturate or report.

### Empty state — `std::nullopt` behavior

The payload is `std::optional<Rational>`. An empty quantity is one whose
payload is `std::nullopt`. Every API surface member has specified behaviour in
the empty state — with one deliberate exception: **relational ordering carries
a precondition that both operands are engaged** (see *Comparison with empty*).

**How emptiness arises.**

- **Default construction.** `Quantity<U>{}` is empty — no value has been
  entered.
- **Division by zero.** Any division whose divisor evaluates to zero yields
  empty, regardless of the dividend's state.
- **`fromOptional`.** `Quantity::fromOptional(std::nullopt)` produces an empty
  quantity in the given unit, preserving the declared-precision template arg.
- **Propagation from operands.** Every arithmetic and conversion operation
  propagates emptiness (see below).
- **`fromDouble` on a non-representable input.** `fromDouble` is empty **only**
  when `raw` is not finite (`NaN`, `±∞`) or its scaled magnitude overflows
  `int64` — i.e. it never turns a *finite, in-range* reading into empty. It
  rounds `raw` to the field's declared precision (half-away-from-zero, per
  `Rational::fromFloat`). To map an *optional* `double` to a `Quantity`, use
  `opt.map(Quantity<U>::fromDouble).value_or(Quantity<U>{})`.

**Querying emptiness.** `hasValue()` returns `true` when the payload is
present, `false` when empty. There is no implicit boolean conversion — call
`hasValue()` explicitly.

**`value()` and `value_or()`.** `value()` returns `const
std::optional<Rational>&` — callers pattern-match on it directly.
`value_or(Rational const& fallback)` returns the payload if present, otherwise
the caller-supplied fallback.

**Arithmetic propagates emptiness.** Every operator (`+`, `-`, `*`, `/`,
unary `-`) yields an empty result when any operand is empty. Division by a
non-empty zero also yields empty. This is spreadsheet / SQL-NULL semantics:
empty + 5 → empty, empty * empty → empty, 3 / 0 → empty. The framework does
not distinguish "no data" from "no result"; validate before dividing if the
distinction matters.

**Conversion propagates emptiness.** `operator Quantity<To>()` on an empty
quantity produces an empty quantity in the target unit — no `convert` function
is called, no history node is recorded. A `convert` (ratio or user override) is
only invoked when the operand carries a value.

**Comparison with empty.** Equality is total: `operator==` returns `true` when
both sides are empty, `false` when one is empty and the other is not.
Relational ordering is **not** total. Emptiness is a *runtime* property of the
value, so whether an operand is engaged cannot be known when the code is
compiled — `operator<=>` and the four relational operators (`<`, `<=`, `>`,
`>=`) therefore carry a **precondition that both operands are engaged**.
Violating it **throws `std::logic_error`** — a defined diagnostic, never a
silent wrong answer and never undefined behaviour. Callers must
`hasValue()`-check before ordering. (Ordering is a
precondition rather than a compile-time deletion precisely because the type
*does* need to be ordered whenever its values are present — sorting a column of
engaged quantities, say.)

Comparison — both `==` and ordering — is defined **only between the same unit**
(`Quantity<U>` vs `Quantity<U>`, across any declared precisions). Unlike `+` /
`-`, comparison does **not** auto-convert: `kg < g` does not compile; convert one
side first if you mean to compare across units. Comparison is on the exact value
alone and **ignores the runtime `DecimalPlaces` tag** — two engaged quantities
with equal value but different precision compare equal.

**Formatting.** `std::format("{}", emptyQ)` renders the unit's `N/A` text
(defined in `UnitTraits`) alone — `"N/A"`, `"N/A kW"`, etc. The formatting
path never attempts to access the absent `Rational`.

**`equation()`.** An empty quantity has no derivation. `equation()` returns a
single-element vector containing the same `N/A` text that `std::format` would
produce for the value part — no formula, no substitution, no `where` legend,
just the formatted placeholder.

**`named()`.** Calling `named("label")` on an empty quantity returns an empty
quantity of the same unit — no history node is created, and the name is
discarded. (With tracing compiled out this is the normal path for all
quantities; with tracing enabled it is the empty-specific path.)

**`withDecimalPlaces()` and `atDeclaredPrecision()`.** Applied to an empty
quantity, both are no-ops that return the same empty quantity unchanged.
There is no `Rational` to retag.

**Wire serialization.** On the morph JSON wire an empty quantity serializes as
JSON `null` — the payload's `std::nullopt` maps directly to a null JSON token,
and the unit metadata travels only in the schema, never in the instance.

## Provenance — a build-time toggle, on by default

Whether a quantity carries its derivation is a **build-wide decision, selected
by the preprocessor macro `MORPH_QUANTITY_PROVENANCE`** — *not* a template
argument. The macro defaults to `1` (tracing on); defining it to `0` before
including the header (or `-DMORPH_QUANTITY_PROVENANCE=0` build-wide) compiles
tracing out. `Quantity<U>` is the **same type** whether or not tracing is
compiled in, so a single build can never hold both a traced and an untraced
flavour of the same unit, and nothing has to interoperate across that boundary.
**The default is on: every `Quantity` is traceable** unless the macro is set
to `0`.

With the macro set to `0`, no `ASTNode`s are ever allocated — no per-leaf,
per-operation, or per-copy bookkeeping — and the provenance surface stays
callable but returns a meaningful *empty* result rather than disappearing:

- `equation()` returns just the value's own formatted number (the result line,
  no symbolic formula and no `where` legend) — there is no recorded derivation
  to expand;
- `named()` and `NamedQuantity` retain no name (a name has nowhere to live
  without a history node), so they are no-ops that slice straight to a plain
  value;
- copying is an ordinary value copy, not a shared-DAG refcount bump.

Callers therefore compile and run unchanged in either build; only the richness
of `equation()` differs. Turning tracing back on is a rebuild, not a type
change.

When tracing is on, the derivation is a **shared DAG** built from three concrete
types:

- **`ASTUnit`** — one step of work: an `operation` string plus the `lhs`, `rhs`,
  and `result` values (each an `optional<Rational>`; `rhs` is empty for
  single-operand steps such as a unit conversion). The `result` is stored, not
  recomputed, so a step can be printed on its own.
- **`ASTNode`** — a node in the DAG: its own `ASTUnit current` step, an optional
  symbol `name` (set by `named()` / `NamedQuantity`, which makes the node opaque
  and stops `equation()` expanding it), and `shared_ptr<ASTNode> left` / `right`
  handles onto the nodes that fed this one. An `ASTNode` is **move-only**: sharing
  is done through the `shared_ptr`, never by copying the node.
- **`Context`** — the per-`Quantity` handle: a single `shared_ptr<ASTNode> node`
  pointing at the root of that value's derivation. Copying a `Quantity` copies its
  `Context`, which just bumps the node's refcount — the tree itself is never
  cloned.

So copying a quantity bumps a refcount instead of cloning the tree, and a
sub-value reused in several places (the `x` in `(x - y) / x`) appears once,
deduplicated by node identity rather than repeated per reference. Each
constructed leaf gets its own node — even a raw input — which is exactly what
lets `equation()` recognise "the same value, referenced more than once" and give
it a single shared placeholder. The tree is deliberately **unit-erased** so it
can be shared across differently typed quantities. It is *not* precision-erased:
every value stored in a node is the exact `Rational` as computed, carrying its
own runtime `DecimalPlaces` tag.

**Nodes are immutable once built.** No operation ever mutates an existing
`ASTNode` — arithmetic, conversion, and `named()` each allocate a *new* node
that points at the (unchanged) prior ones. Immutability is what makes sharing
safe: a subtree held by several quantities can never be altered out from under
one of them. Because the nodes are immutable and `shared_ptr`'s refcount is
atomic, a completed derivation can be read (formatted, `equation()`-ed) from
multiple threads concurrently; building new quantities from a shared subtree on
different threads is likewise safe.

The derivation is exposed through **one method, `equation()`** — a single
worked formula, not two parallel views. It returns a `std::vector<std::string>`
of **print-ready lines** (each already carrying whatever indentation it needs,
so a caller emits them verbatim), in this fixed order:

1. **Formula** — element `[0]` — the whole computation as a **symbolic
   expression**: named values by name, a placeholder (`c1`, `c2`, …) for each
   unnamed value that is **reused** (appears more than once), and every other
   unnamed sub-value inlined — a computed one down to its leaf terms, a leaf as
   its own number — e.g. `"heater" * 3 * "tariff" - "solar" * "tariff" +
   "standing"`. Parenthesised only where needed to preserve meaning — driven by
   both **precedence** (a sum under a `*`/`/`) and **associativity** (the
   right side of the non-associative `-` and `/`: `a - (b + c)`, `a / (b * c)`
   stay parenthesised, while the left-associative `a - b - c` and `a * b * c`
   do not). No leading indent and no `=`.
2. **Substitution** — element `[1]` — the same expression with every symbol and
   placeholder replaced by its value — `2 * 3 * 0.3 - 1.8 * 0.3 + 0.1`. Returned
   as a continuation line: indented and prefixed `= `.
3. **Result** — element `[2]` — the overall result value — `1.36` — in the same
   indented `= ` continuation form.
4. **`where` legend** — elements `[3]…`, one per placeholder, in order of first
   appearance; the first begins `where `, any further lines align under it. A
   placeholder exists only for a reused value, and its legend form depends on
   what that value is:
   - a **reused unnamed leaf** (a raw input used more than once): `c1 = <value>` —
     e.g. `where c1 = 3`;
   - a **reused unnamed computed value**: its work written once, as
     `c1 = <symbolic> = <substituted> = <value>`.
   A value used exactly once never reaches the legend — it is inlined at its one
   point of use instead.

**Degenerate roots return a single element.** When the value has no expandable
derivation, `equation()` returns a one-element vector (formula only, no
substitution/result/legend lines):

- an **empty** quantity, or any quantity with tracing compiled out → the
  formatted value alone (the `N/A` text when empty);
- a **bare unnamed leaf** (a raw input never operated on) → its formatted value;
- a value whose **root is named** (e.g. a `NamedQuantity`, or the result of
  `.named(...)`) → just `"the name"`, since a name is opaque and its derivation
  is deliberately not expanded. Introspect the *unnamed* computed value if you
  want the formula (see the calculation example).

So the symbols (the shape you'd write on paper) and the numbers (the audit of
what was actually computed) live in one coherent artifact rather than in two
methods that echo each other.

**How the numbers are rendered.** `equation()` carries no precision of its own
and never consults any field's *declared* precision. Every number it prints —
each substituted value, the result, and each `where` value — is produced by
running that node's stored `Rational` through the ordinary `Rational` formatter.
The decimals therefore come from the value itself: the `Rational`'s own runtime
`DecimalPlaces` tag, the single source of truth for how a value prints, exactly
as `std::format("{}", someRational)` would render it elsewhere. The unit is not
shown (the tree is unit-erased); pair the lines with `std::format("{}", q)` for
the united answer.

**The `DecimalPlaces` flow, end to end.** How a number prints is fixed entirely
by the runtime `DecimalPlaces` tag it carries, and that tag flows through the
calculation deterministically:

1. **Origin.** `fromDouble(d)` stamps the new leaf's `Rational` with the field's
   *declared* precision — the one moment declared precision touches a value. A
   `Rational` built directly carries whatever tag it was given.
2. **Arithmetic.** `+`, `-`, `*`, `/` set the result's tag to the **maximum** of
   the operands' tags (max-propagation, above).
3. **Conversion.** A `convert` step multiplies by the (exact) ratio and carries
   the operand's tag through unchanged — scaling a value does not change how many
   decimals it is specified to.
4. **Formatting.** `std::format` and `equation()` both defer to the `Rational`
   formatter, which is the single authority on the printed form: the default
   (no format spec) prints an exact integer when the denominator is `1`
   (so `2/1` → `2`) and otherwise renders at the value's `DecimalPlaces`. Nothing
   in `Quantity` re-implements number formatting.

`atDeclaredPrecision()` / `withDecimalPlaces()` retag a value between steps if a
caller wants a specific width; they change only the tag, never the exact value.

Two things decide how any sub-value reads: whether it is **named**, and whether
it is **reused**. These compose into one uniform rule that applies to leaves and
computed values alike:

- **Named** → appears as its name and is not expanded; the name summarises its
  own sub-derivation (a conversion behind `"heater"`, say, folds into the name).
- **Unnamed, used once** → *inlined* at its point of use: a computed value as its
  arithmetic (`"a" - "b"`), a leaf as its own number (`3`). Leave a value unnamed
  precisely when you *do* want its arithmetic shown.
- **Unnamed, reused** (appears more than once) → earns a single placeholder
  (`c1`, `c2`, …) so the shared work is written once, in the `where` legend,
  rather than duplicated at each use — whether the reused value is a computed
  expression or a raw leaf.

The distinction between a leaf and a computed value is therefore *not* what
triggers a placeholder — reuse is. A once-used leaf is just its number in the
formula; a placeholder appears only to avoid repeating a shared value.

## Named symbols

`named("load")` labels a value as a symbol: in `equation()` it appears by that
name and its own sub-derivation is not expanded ("a name is a promise you don't
need to see how this was made"). This is the knob that controls formula depth —
name the inputs you want treated as given, leave unnamed the values whose
arithmetic you want written out. Naming builds a fresh history node rather than
mutating the shared one, so it never renames a value some other quantity is also
holding. (With tracing compiled out there is no node to build, so naming is a
no-op that slices to a plain value — see Provenance.)

A compile-time convenience wrapper, `NamedQuantity<"load", Unit::kW>`, names a
value on construction and slices losslessly back to a plain `Quantity` wherever
one is expected — the name lives in the shared history, not as extra data on the
value.

## Formatting — `std::formatter` only

A `std::formatter<Quantity<...>>` renders value + unit (`5.2kW`, `N/A%`),
driving off the `UnitTraits` display text and the `Rational` formatter.
`NamedQuantity` forwards to it. **No `operator<<` is provided** — formatting
goes through `std::format` exclusively.

## Wire and schema

On the morph JSON wire a quantity is just its nullable `Rational` payload —
neither the unit, the declared precision, nor the derivation travels. The unit
surfaces only in generated JSON Schemas (as `ExtUnits`, from `UnitTraits`) and
in C++ types; the declared precision surfaces as `x-decimalPlaces`. A client
cannot send a mismatched unit, and provenance stays a local, in-process concern.

**Display units come from `relations`, not a second list.** `UnitRelation` is
the *single* source of within-dimension conversion. A renderer's display/entry
unit selector — surfaced in the schema as `x-unitAlternatives` — is **derived
from the same `relations`**: the alternatives for a field's unit are the units
it can convert to (directly or by chaining), each with the exact ratio the
conversion uses. There is no separate `alternatives` declaration to keep in
sync; declaring a `UnitRelation` both enables `convert` and offers the unit in
the selector.

## Unit conversion — `UnitRelation` and `convert`

Arithmetic works only on values of the *same* unit, but the same physical
quantity is often held in different units (a heater rated in `W`, a tariff
computed in `kW`). Conversion bridges that gap. It is exposed two ways that share
one implementation:

- **`operator Quantity<To>()`** — the *implicit* conversion the type surfaces, so
  a `Quantity<From>` can appear wherever a `Quantity<To>` is expected (and
  `static_cast<Quantity<To>>(q)` reads explicitly).
- **`convert`** — the routine that does the actual value math: the generated
  ratio `convert(Quantity<From>, Quantity<To>&)` template, or a
  `UnitTraits<E>::convert` static override for the pair. The conversion operator
  drives whichever applies; mixed-unit `+` / `-` route through the same operator
  on the right operand. The operator is the single place the
  arithmetic-to-value-math bridge lives — it wraps the value math with provenance
  and empty-propagation, so neither `convert` nor the arithmetic path re-implements
  any of that.

### 1. `UnitRelation` — declaring peer-to-peer ratios

Each unit system declares its within-dimension relationships as a flat list of
`UnitRelation` entries in `UnitTraits`:

```cpp
template <typename E>
struct UnitRelation {
    E from;
    E to;
    morph::math::Rational fromTo;  // 1 unit of `from` = `fromTo` units of `to`
};
```

Example:

```cpp
template <>
struct morph::units::UnitTraits<Unit> {
    static constexpr UnitMeta meta(Unit u) noexcept { /* ... */ }

    static constexpr std::array<UnitRelation<Unit>, 2> relations{{
        {Unit::g,   Unit::kg, Rational{  1, 1000}},
        {Unit::t,   Unit::kg, Rational{1000,    1}},
    }};
};
```

`{Unit::g, Unit::kg, Rational{1, 1000}}` means **1 g = 1/1000 kg**.
The framework inverts the ratio for the reverse direction:
- `g → kg`: multiply value by `1/1000`
- `kg → g`: multiply value by `1000/1`

`fromTo` must be **strictly positive** — the reverse direction divides by it, so
zero is rejected, and a negative ratio has no physical meaning for a
within-dimension scale. `from` and `to` must be distinct units.

### 2. Auto-generated `convert` and user override

For each entry the framework generates a within-dimension conversion as a
**constrained function template** (`template <auto From, auto To> requires
RatioConvertible<From, To> void convert(...)`) at namespace scope. A pair the
ratios can reach (directly or by chaining) is convertible; the conversion
operator (`operator Quantity<To>()`) drives it, propagates empty, and records
the provenance step.

**User override — a `UnitTraits<E>::convert` static.** For a conversion that is
*not* a ratio (Celsius ↔ Fahrenheit, a currency rate), the application adds a
`convert` static to its `UnitTraits<E>` specialisation. The framework prefers it
over the generated ratio path for the pairs it names (`if constexpr` picks the
user's static when present, else the ratio template). It is a *static* rather
than a free function found by ADL because the unit is a **non-type** template
argument of `Quantity`, so ADL never reaches the enum's namespace — the
customisation therefore lives with `meta` and `relations`, called by qualified
name. Template it on the declared-decimals parameters so its signature needs no
`meta()` lookup mid-class:

```cpp
template <>
struct morph::units::UnitTraits<Unit> {
    static constexpr UnitMeta meta(Unit u) noexcept { /* ... */ }
    // ... relations ...

    // Takes priority over any ratio path for this pair.
    template <std::uint32_t DIn, std::uint32_t DOut>
    static void convert(Quantity<Unit::celsius, DIn> const& in, Quantity<Unit::fahrenheit, DOut>& out) {
        out = Quantity<Unit::fahrenheit, DOut>::fromDouble(in.value()->toDouble() * 9.0 / 5.0 + 32.0);
    }
};
```

The user's `convert` performs the pure value math on an engaged operand; the
framework wraps it with the provenance step (a `convert From → To` node) and
handles empty propagation (it is never called on an empty operand).

A pair without a `UnitRelation` path *and* without a `UnitTraits::convert`
static simply does not convert — the attempt does not compile.

### 3. Mixed-unit `+` / `-`

`a + b` with differing units converts the **right operand** to the left
operand's unit first (via `convert(RightUnit, LeftUnit&)`) and the result
takes the left operand's unit. This only compiles when the corresponding
`convert` exists (auto-generated or user-provided).

### 4. Chaining

The `relations` list defines an **undirected weighted graph**: each entry is one
edge, usable in either direction (forward with `fromTo`, backward with its
reciprocal). When no direct entry exists for a requested `From → To`, the
framework searches this graph for a connecting path and composes the conversion
from it.

The search is a **compile-time breadth-first search** over `relations`, so it
finds a path with the **fewest hops**; ties are broken by the entries'
declaration order in the array. The conversion factor is the **product of the
edge ratios** along the chosen path (`g → t` via `kg` composes `g→kg` and
`kg→t`). Provenance records **each hop as its own step**, so the derivation shows
the units it passed through, not just the endpoints.

Only ratio-based `UnitRelation` edges take part in composition. A user-provided
`convert` (a non-ratio conversion such as °C ↔ °F) is a **direct edge only** — it
is never composed *through*, because arbitrary functions have no ratio to
multiply into a chain. The application is responsible for keeping declared ratios
mutually consistent; the framework composes whatever shortest path it finds and
does not cross-check that two different paths agree.

This coexists with the `consteval` unit algebra: the algebra combines *different
dimensions* (`kg * m3`, `kg / m3`, same-unit `→ scalar`), while `convert`
*scales within one dimension*.

## API reference

The public surface, grouped by role. `U` / `From` / `To` are enumerators of an
application unit enum satisfying `UnitEnum`; `Dec` is a declared-decimals
template argument that defaults from `UnitTraits`. Signatures are shown in
essential form (the cross-precision template parameters that let same-unit
operands of different declared precisions interoperate are elided for
readability).

### Support types and customisation points

| Symbol | Kind | Purpose |
|---|---|---|
| `UnitMeta { id, display, defaultDecimals }` | struct | Static per-unit description returned by `UnitTraits::meta`. `id` is wire/schema vocabulary, `display` is human text, `defaultDecimals` seeds declared precision. |
| `UnitTraits<E>` | class template | **Customisation point.** The application specialises it for its unit enum `E`: `static constexpr UnitMeta meta(E)` and `static constexpr std::array<UnitRelation<E>, N> relations`. |
| `UnitRelation<E> { from, to, fromTo }` | struct | One exact peer-to-peer ratio (`1 from == fromTo·to`, a positive `Rational`). Entries drive the auto-generated `convert`, conversion chaining, **and** the display-unit selector. |
| `UnitAlternative<E> { unit, num, den }` | struct | A *derived* per-unit view (computed from `relations`, not declared): one convertible display/entry unit and its exact ratio. Returned by `Quantity::unitAlternatives()`; feeds `x-unitAlternatives`. |
| `UnitEnum<E>` | concept | Satisfied by an enum with a `UnitTraits<E>::meta`. Constrains `Quantity`. |
| `operator*`, `operator/` (on `E`) | `consteval` | Application-supplied unit algebra deducing cross-dimension result units; an unsupported combination fails to compile. |

### `Quantity<U, Dec>` — compile-time members

| Member | Signature | Returns |
|---|---|---|
| `unit` | `static constexpr auto` | The unit enumerator `U`. |
| `declaredDecimals` | `static constexpr std::uint32_t` | The field's declared decimal count. |
| `declaredPrecision()` | `static constexpr DecimalPlaces` | `declaredDecimals` as the strong type. |
| `unitMeta()` | `static constexpr UnitMeta` | The unit's `UnitMeta`. |
| `unitAlternatives()` | `static constexpr std::span<const UnitAlternative<E>>` | The convertible display/entry units for this field's unit, **derived from `UnitTraits::relations`** (empty when none). Drives the schema's `x-unitAlternatives`. |

### `Quantity<U, Dec>` — construction

| Member | Signature | Notes |
|---|---|---|
| default ctor | `constexpr Quantity() noexcept` | The **empty** state. |
| value ctor | `constexpr Quantity(Rational engaged) noexcept` | Engages, keeping the value's own runtime precision. |
| optional ctor | `constexpr Quantity(std::optional<Rational>) noexcept` | Adopts a payload as-is. |
| cross-precision ctor | `constexpr Quantity(Quantity<U, Other>) noexcept` | Same unit, different declared precision; value carries over unchanged. |
| `fromDouble` | `static Quantity fromDouble(double raw) noexcept` | Tags the leaf at `declaredPrecision()`; **never empty from a finite value** (empty only when `raw` is non-finite / doesn't fit). |
| `fromOptional` | `static Quantity fromOptional(std::optional<Rational>) noexcept` | Empty in → empty out, preserving the declared-precision arg. |

### `Quantity<U, Dec>` — access, precision, provenance

| Member | Signature | Notes |
|---|---|---|
| `hasValue()` | `constexpr bool hasValue() const noexcept` | Engaged? No implicit `bool` conversion. |
| `value()` | `const std::optional<Rational>& value() const noexcept` | The payload; pattern-match or `->` it. |
| `value_or(fallback)` | `Rational value_or(Rational const&) const` | Payload if engaged, else the fallback. |
| `operator*` | `const Rational& operator*() const` | Unchecked access to the engaged value (UB when empty, like `std::optional`). |
| `withDecimalPlaces(p)` | `constexpr Quantity withDecimalPlaces(DecimalPlaces) const noexcept` | Retags actual precision; no-op on empty. Value unchanged. |
| `atDeclaredPrecision()` | `constexpr Quantity atDeclaredPrecision() const noexcept` | Retags actual precision to the declared one; no-op on empty. |
| `named(label)` | `Quantity named(std::string label) const` | Returns a same-unit quantity marked as the symbol `label`; builds a fresh history node (no-op returning empty on empty, or with tracing off). |
| `equation()` | `std::vector<std::string> equation() const` | The worked formula as print-ready lines (see *Provenance*). Single-element (the formatted value) when empty or tracing off. |
| `operator Quantity<To>()` | `operator Quantity<To>() const` | Implicit same-dimension conversion; delegates to `convert`, propagates empty, records a provenance step. |

### Free functions and operators (namespace scope)

| Symbol | Signature (essential) | Notes |
|---|---|---|
| `convert` | `void convert(Quantity<From> const&, Quantity<To>&)` | Auto-generated **constrained template** per `UnitRelation`; a `UnitTraits<E>::convert` static wins for its pair. Does the value math only, on an engaged operand. |
| `operator+`, `operator-` | `Quantity<U> operator±(Quantity<U>, Quantity<U>)` | Same-unit; mixed convertible units convert the right operand to the left's unit first. Result carries the unit's default declared precision. |
| `operator-` (unary) | `Quantity<U, Dec> operator-(Quantity<U, Dec>)` | Negation; keeps declared precision. |
| `operator*`, `operator/` (cross-unit) | `Quantity<A*B> / Quantity<A/B>` | Result unit from the `consteval` algebra; SFINAE'd out when the algebra rejects the pair. |
| `operator*`, `operator/` (scalar) | `Quantity<U, Dec> op(Quantity<U, Dec>, Rational)` | Scale/divide by a dimensionless `Rational`; unit and declared precision unchanged. |
| `operator==` | `bool operator==(Quantity<U>, Quantity<U>)` | **Total**; empty==empty is `true`. |
| `operator<=>` (`<`,`<=`,`>`,`>=`) | `std::strong_ordering operator<=>(Quantity<U>, Quantity<U>)` | Ordering; **throws `std::logic_error`** if either operand is empty. |

All arithmetic and conversion **propagate empty**; division by a non-empty zero
yields empty.

### `NamedQuantity<Name, U>` and formatting

| Symbol | Kind | Notes |
|---|---|---|
| `NamedQuantity<Name, U>` | class template | `Quantity<U>` that names itself `Name` on construction; slices losslessly to a plain `Quantity<U>`. The name lives in the shared history, not as extra data. |
| `std::formatter<Quantity<U, Dec>>` | specialisation | Renders value + unit (`5.2kW`, `N/A%`). No `operator<<`. |
| `std::formatter<NamedQuantity<Name, U>>` | specialisation | Forwards to the `Quantity` formatter. |

On the wire (`glz::meta` / `to_json_schema`) a quantity is its nullable
`Rational` payload; the unit surfaces only in the generated JSON Schema
(`ExtUnits`) and the declared precision as `x-decimalPlaces`.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Provenance | **`MORPH_QUANTITY_PROVENANCE` macro, default `1`** | A build-wide toggle, not a template arg — `Quantity<U>` is the identical type either way; set to `0` and no nodes are allocated and the provenance API returns empty results (`equation()` → just the value, `named()` → no-op). |
| Payload | **Exact `Rational`** | Exactness is non-negotiable for domain math. |
| Magnitude | **Bounded by `int64`; overflow is a limitation, not empty** | `+`/`-`/`*`/ratio composition can overflow `int64` numerators/denominators; only division-by-zero is turned into empty. Domain values stay well within range. |
| Display units | **Derived from `UnitRelation`** | The schema's `x-unitAlternatives` selector comes from the same `relations` that drive `convert` — one source, no second list to keep in sync. |
| History structure | **Shared, unit-erased DAG (`ASTUnit` / `ASTNode` / `Context`)** | `ASTNode` (step + optional name + `shared_ptr` children) linked into a DAG; each `Quantity` holds a `Context` (a `shared_ptr<ASTNode>` root). Cheap copies; reused subexpressions deduped by node identity; shareable across units. |
| Placeholders | **Reuse, not leaf-vs-computed, mints a `cN`** | A value used once inlines (leaf → its number, computed → its expression); only a value reused across the expression earns one shared placeholder, so shared work is written once. |
| Precision | **Actual = max of engaged operands; declared from `UnitTraits`** | Max-propagation keeps a result no less precise than its widest input; the declared tag stays a field property (`fromDouble` origin, `atDeclaredPrecision` to reset). |
| Formatting | **`std::formatter` only, delegating to the `Rational` formatter** | Single formatting path; no `operator<<`; the runtime `DecimalPlaces` tag is the sole authority on printed decimals. |
| Wire | **Payload only** | Units and history never travel; the wire stays a nullable `Rational`. |
| Empty ordering | **Throws, not a compile error** | Emptiness is a runtime `optional` state, so ordering an empty operand throws `std::logic_error` (a testable defined diagnostic); `==` stays total. |
| Conversion | **`UnitRelation` entries + auto-generated constrained-template `convert`; `UnitTraits::convert` static override** | Application declares exact peer-to-peer ratios in `UnitTraits::relations`; framework auto-generates a constrained `convert(From, To&)` template and records the provenance step. A `UnitTraits<E>::convert` static wins (`if constexpr`) for non-ratio conversions (C↔F, currency) — a static, not an ADL free function, because the unit is a non-type template arg so ADL can't reach the enum's namespace. Chaining composes ratio edges over the relation graph. |

## Usage example

A small energy-billing scenario that exercises the whole surface: conversion,
cross-unit algebra that *deduces* result units (kW·h → kWh → €), same-unit
arithmetic, a dimensionless ratio, and a computed value explaining itself.

### The unit system (application side)

```cpp
#include <morph/quantity.hpp>
#include <format>

using morph::units::Quantity;

enum class Unit : std::uint16_t {
    scalar, watt, kilowatt, hour, kilowatt_hour, euro, euro_per_kwh
};

template <>
struct morph::units::UnitTraits<Unit> {
    static constexpr morph::units::UnitMeta meta(Unit u) noexcept {
        switch (u) {
            case Unit::scalar:        return {"scalar", "", 3};
            case Unit::watt:          return {"watt", "W", 1};
            case Unit::kilowatt:      return {"kilowatt", "kW", 3};
            case Unit::hour:          return {"hour", "h", 2};
            case Unit::kilowatt_hour: return {"kwh", "kWh", 3};
            case Unit::euro:          return {"euro", "€", 2};
            case Unit::euro_per_kwh:  return {"eur_per_kwh", "€/kWh", 4};
        }
        return {"?", "?", 3};
    }

    // Peer-to-peer unit relations: auto-generate convert(Watt↔Kilowatt).
    static constexpr std::array<morph::units::UnitRelation<Unit>, 1> relations{{
        {Unit::watt, Unit::kilowatt, morph::math::Rational{1, 1000}},
    }};
};

// The unit algebra — consteval, so an unsupported combination is a compile
// error at the offending call site, not a runtime surprise.
consteval Unit operator*(Unit a, Unit b) {
    if (a == Unit::scalar) return b;
    if (b == Unit::scalar) return a;
    if (a == Unit::kilowatt && b == Unit::hour) return Unit::kilowatt_hour;
    if (a == Unit::hour && b == Unit::kilowatt) return Unit::kilowatt_hour;
    if (a == Unit::kilowatt_hour && b == Unit::euro_per_kwh) return Unit::euro;
    if (a == Unit::euro_per_kwh && b == Unit::kilowatt_hour) return Unit::euro;
    throw "unsupported unit product";
}
consteval Unit operator/(Unit a, Unit b) {
    if (b == Unit::scalar) return a;
    if (a == b)            return Unit::scalar;
    if (a == Unit::euro          && b == Unit::kilowatt_hour) return Unit::euro_per_kwh;
    if (a == Unit::kilowatt_hour && b == Unit::hour)          return Unit::kilowatt;
    throw "unsupported unit quotient";
}

// A same-dimension conversion: watt ↔ kilowatt. Declared as a UnitRelation
// entry, the framework auto-generates convert(Watt, Kilowatt&) and
// convert(Kilowatt, Watt&). The framework also records the conversion step
// in the derivation when tracing is on.
// (For a non-ratio conversion like Celsius ↔ Fahrenheit, add a templated
// `convert` static to this UnitTraits specialisation; it wins over the
// generated ratio path for the pairs it names.)
```

### The calculation

```cpp
using morph::units::NamedQuantity;

using Watt         = Quantity<Unit::watt>;
using Kilowatt     = Quantity<Unit::kilowatt>;
using Hours        = Quantity<Unit::hour>;
using KilowattHour = Quantity<Unit::kilowatt_hour>;
using Euro         = Quantity<Unit::euro>;                        // plain result unit

// Type-level naming: every value of these types is named on construction, so
// the name comes from the type rather than a call-site .named(...).
using Tariff         = NamedQuantity<"tariff",   Unit::euro_per_kwh>;
using StandingCharge = NamedQuantity<"standing", Unit::euro>;

// Inputs. Two ways to name: call-site .named(...) and the type itself
// (Tariff / StandingCharge). The heater is rated in watts and converted to kW
// (the conversion is recorded, and folds behind the "heater" name).
auto heater  = static_cast<Kilowatt>(Watt::fromDouble(2000.0)).named("heater");
auto runtime = Hours::fromDouble(3.0);                         // left UNNAMED on purpose
auto solar   = KilowattHour::fromDouble(1.8).named("solar");   // covered by panels
Tariff         tariff{Tariff::fromDouble(0.30)};               // named "tariff"
StandingCharge standing{StandingCharge::fromDouble(0.10)};     // named "standing"

// Cross-unit algebra deduces each result unit at compile time. Results are
// left unnamed — the value we introspect must have an unnamed root, or
// equation() would collapse to just its name.
auto consumption = heater * runtime;             // kW * h      -> kWh    (6 kWh)
auto grid_cost   = consumption * tariff;         // kWh * €/kWh -> €      (1.80 €)
auto savings     = solar * tariff;               // kWh * €/kWh -> €      (0.54 €)
auto net_cost    = grid_cost - savings + standing;  // € arithmetic -> €  (1.36 €)

// Same-unit division cancels to the scalar unit — the fraction the panels cover:
auto solar_share = solar / consumption;          // kWh / kWh   -> scalar (0.3)

// heater * tariff would be a compile error: kW * €/kWh is not in the algebra.
```

### Reading the result

`std::format` renders value + unit; `equation()` returns the worked formula as
lines of text. (There is no `operator<<`; streaming a preformatted
`std::string` is ordinary `std::ostream`, not an overload on `Quantity`.)

```cpp
// Formatted with its unit:
std::format("{}", net_cost);      // -> "1.36€"
std::format("{}", consumption);   // -> "6kWh"
std::format("{}", solar_share);   // -> "0.3"   (scalar unit renders empty)

// Print a value with its equation.
auto report = [](std::string_view label, auto const& q) {
    std::cout << label << " = " << std::format("{}", q) << '\n';
    for (auto const& line : q.equation()) std::cout << "  " << line << '\n';
};

report("net_cost", net_cost);
```

This prints:

```text
net_cost = 1.36€
  "heater" * 3 * "tariff" - "solar" * "tariff" + "standing"
      = 2 * 3 * 0.3 - 1.8 * 0.3 + 0.1
      = 1.36
```

One worked artifact: the symbolic formula, the same formula with values
substituted, and the result. Nothing here is reused, so there is no placeholder
and no `where` legend — every value appears inline.

- **Named values are symbols and are not expanded** — `"heater"`, `"solar"`
  (named at the call site) and `"tariff"`, `"standing"` (named by their types
  `Tariff` / `StandingCharge`) read identically; the two naming mechanisms are
  indistinguishable in the output. Because `heater` is named, its watt → kW
  conversion is summarised behind the name and does not appear; leave `heater`
  unnamed if you want the conversion written into the formula.
- **The unnamed `runtime` inlines as its value `3`.** It is a raw leaf used
  exactly once, so it is written directly into the formula rather than pushed to a
  placeholder — placeholders exist only for *reused* values (see below).
- **Unnamed computed values inline too.** `consumption`, `grid_cost`, `savings`,
  and the subtotal have arithmetic structure and are each used once, so they fold
  directly into the one-line formula. The same "used once ⇒ inline" rule governs
  leaves (`runtime`) and computed values alike; only reuse changes it.

The result value carries its unit; the formula lines are unit-erased numbers, so
pair them with `std::format("{}", net_cost)` (the `net_cost = 1.36€` header) for
the fully-united answer.

### Reused values share one placeholder

The example above inlines every unnamed value — leaf and computed alike —
because each is used exactly once. When an unnamed value instead feeds the
**same expression more than once**, it is not re-expanded at each use: it earns a
single placeholder, and its work is written once in the `where` legend.

```cpp
auto a = KilowattHour::fromDouble(6.0).named("a");
auto b = KilowattHour::fromDouble(1.8).named("b");

// `diff` is unnamed and computed, and it appears twice in `share`.
auto diff  = a - b;                 // kWh, unnamed computed          (4.2)
auto share = diff / (diff + b);     // kWh / kWh -> scalar            (0.7)

report("share", share);
```

This prints:

```text
share = 0.7
  c1 / (c1 + "b")
      = 4.2 / (4.2 + 1.8)
      = 0.7
  where c1 = "a" - "b" = 6 - 1.8 = 4.2
```

`diff` occurs twice in the formula but as the single placeholder `c1`, and the
`where` line spells out its derivation exactly once — `c1 = <symbolic> =
<substituted> = <value>`, the reused-computed form of the legend. The
parentheses appear only because `/` over a sum requires them. Had `diff` been
used once, it would have inlined as `"a" - "b"` in place, with no `c1` at all —
the placeholder exists precisely to avoid writing shared work twice.

## Out of scope

- Serializing history on the wire — only the `Rational` payload travels.
- Auto-deriving conversions the application hasn't declared — a pair is
  convertible only when a `UnitRelation` entry exists or a user-provided
  `convert(From, To&)` overload exists for it.
