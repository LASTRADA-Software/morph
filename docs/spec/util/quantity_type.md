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

The first two points — an exact value, in a known unit, that may be empty — are
the **core contract** and the reason `Quantity` exists rather than a plain
`double`: domain math must be exact, units must not mix by accident, and "not
entered" must be a first-class state rather than a sentinel. That core is always
present, whatever the build.

The third point — **provenance** — is an *opt-in feature layered on top of the
core*. It is on by default but compiled out entirely by
`MORPH_QUANTITY_PROVENANCE=0` (see *Provenance*), and it is what makes a domain
application reach for `Quantity` over a bare exact number: the answer alone is
often not enough — you have to show the working: *why* is the density
`0.83 kg/m³`, *which* inputs fed the surplus, *what* was 3 % of what. A traced
`Quantity` carries that explanation with the value instead of reconstructing it
after the fact.

> **Reading this spec.** Much of this document describes provenance — the larger
> surface, but the optional layer. If you only need the core, read *Units are
> types*, *Exact value and precision*, *How a value prints*, and *Wire and
> schema*; everything under *Provenance*, *Named symbols*, and the `equation()`
> rules is skippable when tracing is off.

## Contents

- [Units are types](#units-are-types)
- [Exact value and precision](#exact-value-and-precision)
  ([Empty state](#empty-state--stdnullopt-behavior))
- [How a value prints](#how-a-value-prints)
- [Provenance — a build-time toggle](#provenance--a-build-time-toggle-on-by-default)
- [Named symbols](#named-symbols)
- [Wire and schema](#wire-and-schema)
- [Unit conversion — `UnitRelation` and `convert`](#unit-conversion--unitrelation-and-convert)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Usage example](#usage-example)
- [Cross-references](#cross-references)
- [Limitations](#limitations)
- [Out of scope](#out-of-scope)

## Units are types

`Quantity<Unit::kg>` and `Quantity<Unit::m3>` are distinct types and cannot be
mixed accidentally. The application supplies:

- a `UnitTraits<Enum>` specialisation — id, display text, default decimal
  places, and a flat list of **peer-to-peer `UnitRelation` entries** that
  declare exact conversion ratios between same-dimension units with a
  `morph::math::Rational`. Each entry
  `{Unit::g, Unit::kg, Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}}`
  generates a `convert` in both directions; a `UnitTraits<E>::convert` static
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

Addition and subtraction are **evaluated in a single unit**: `kW + kW` combines
directly, while a mixed *convertible* pair (`kW + W`) first converts the right
operand to the left operand's unit and then combines. A non-convertible pair does
not compile. Negation and scaling by a dimensionless `Rational` are likewise
provided directly. The automatic operand conversion is what `convert` /
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

**Formatting.** `std::format("{}", emptyQ)` renders `N/A` suffixed with the
unit's display text — `"N/A"` (scalar, empty display), `"N/AkW"`, `"N/A%"`.
The `"N/A"` marker is a **fixed literal in the formatter**, *not* a field of
`UnitTraits` (`UnitMeta` carries only `id`, `display`, `defaultDecimals`); the
unit contributes only the trailing `display` string, appended with no
separating space. The formatting path never attempts to access the absent
`Rational`.

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

**Wire serialization.** On the morph JSON wire a quantity *is* its
`std::optional<Rational>` payload (`glz::meta` maps the instance to `payload`
alone), so an empty quantity's `std::nullopt` is a JSON `null` and a null token
read back clears the field. As a **struct member** an empty quantity is written
according to glaze's default optional handling — the null field is **omitted
from the serialized object** rather than emitted as `"field":null` — while the
same field still *accepts* an explicit `null` on read (which clears it). The
unit metadata travels only in the schema, never in the instance.

## How a value prints

Every printed form of a `Quantity` — `std::format`, and every number inside
`equation()` — goes through **one** helper, `detail::formatRationalDecimal`, so
a value reads identically everywhere. There is a single formatting path and
**no `operator<<`** (streaming is done by formatting to a `std::string` first).

**The decimal form.** `formatRationalDecimal` renders the exact `Rational` as a
fixed decimal at its **runtime `DecimalPlaces`** and then trims trailing zeros
(and a bare trailing point). So a whole value prints with no point (`2`), and
`0.30` / `1.360` print as `0.3` / `1.36`. This is deliberately **not**
`Rational`'s own `std::format("{}", r)`, which prints an integer numerator or a
`num/den` *fraction* — `Quantity` always prints a trimmed decimal. The runtime
`DecimalPlaces` tag is the single source of truth for how many decimals appear;
no field's *declared* precision is consulted at print time.

**The decimal is exact — the value never passes through `double`.**
`formatRationalDecimal` computes the fixed-point string by **integer long
division** of the canonical `numerator/denominator`: the integer part is
`|numerator| / denominator`, and each fractional digit is the next digit of the
exact quotient, carrying the exact integer remainder forward. Nothing is routed
through `Rational::toDouble` or `std::formatter<double>`, so the display no
longer inherits floating-point error. Two cases the old double path got wrong
now render correctly: large exact integers beyond the 2^53 double mantissa
(`9007199254740993` prints as itself, not `…992`), and long non-terminating
quotients (`1/3` at 18 places prints eighteen `3`s, not `…315`). The wire and
journal codecs were already exact (they serialise the int64 `num`/`den`/`dp`
directly); this change brings **display** to the same exactness the rest of the
type already had.

**Rounding rule.** A non-terminating quotient (or one whose denominator does not
divide `10^DecimalPlaces`) is rounded **half away from zero** at the runtime
`DecimalPlaces` — the same rule `Rational::toDouble`'s `std::round` uses, so
display and `toDouble` agree on the last digit. The test is exact integer math:
after producing `DecimalPlaces` digits, the truncated result rounds up when
`2 · remainder ≥ denominator`. The rounding carry propagates through the
fractional digits and, when it reaches the top, into the integer part
(`0.9999` at 3 places rounds to `1`). Rounding is symmetric in sign, and a
magnitude that rounds down to zero prints as `0`, never `-0`. Examples: `2/3`
at 4 places → `0.6667`; `1/6` at 3 places → `0.167`; `-2/3` at 4 places →
`-0.6667`.

**Overflow safety.** `numerator` and `denominator` are `int64`, and a naive
fractional digit step would form `remainder · 10`, which can need up to 67 bits
when the denominator is near `INT64_MAX`. The formatter never forms that product.
Because the running `remainder` is always `< denominator`, each digit
`q = floor(remainder · 10 / denominator)` lies in `0..9`, so `mulTenDivMod`
computes it by adding `remainder` to an accumulator ten times and reducing modulo
`denominator` as it goes. The accumulator stays below `denominator` before each
add and below `2·denominator ≤ 2^64` after, so every intermediate is exact in
64-bit — no 128-bit type, no `__int128` `/` or `%`, and no dependency on the
`__udivti3` 128-bit divide helper that clang-cl's MSVC runtime lacks. The result
is identical under MSVC and clang-cl.

**With the unit.** The `Quantity` formatter (`std::formatter<Quantity<...>>`)
appends the unit's `display` text to the number with no separating space:
`5.2kW`, `6kWh`, `0.3` (empty display for a scalar). `NamedQuantity`'s formatter
forwards to it.

**Empty prints `N/A`.** An empty quantity renders as the fixed literal `"N/A"`
suffixed with the unit's `display` — `"N/A"`, `"N/AkW"`, `"N/A%"`. The `"N/A"`
marker lives in the formatter, **not** in `UnitMeta` (which carries only `id`,
`display`, `defaultDecimals`); the format path never touches the absent
`Rational`.

**The `DecimalPlaces` flow, end to end.** The runtime tag that fixes the printed
form flows through a calculation deterministically:

1. **Origin.** `fromDouble(d)` stamps the new leaf's `Rational` with the field's
   *declared* precision — the one moment declared precision touches a value. A
   `Rational` built directly carries whatever tag it was given.
2. **Arithmetic.** `+`, `-`, `*`, `/` set the result's tag to the **maximum** of
   the engaged operands' tags (max-propagation — see *Exact value and
   precision*).
3. **Conversion.** A `convert` step multiplies by the exact ratio and carries the
   operand's tag through unchanged — scaling a value does not change how many
   decimals it is specified to.
4. **Formatting.** `formatRationalDecimal` renders at that tag, as above; nothing
   else in `Quantity` re-implements number formatting.

`atDeclaredPrecision()` / `withDecimalPlaces()` retag a value between steps if a
caller wants a specific width; they change only the tag, never the exact value.

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
  handles onto the nodes that fed this one. The struct is a plain aggregate, but
  nodes are **never copied** in practice: sharing is done through the `shared_ptr`,
  never by duplicating a node — every operation allocates a fresh `ASTNode` and
  links the (shared) prior ones.
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
   appearance; the first line begins `where `, later lines align under it. A
   placeholder exists only for a **reused** value; its exact legend form
   (leaf/conversion vs computed) is given in *How each node renders* below. A
   value used exactly once never reaches the legend — it is inlined at its one
   point of use instead.

**Degenerate roots return a single element.** When the value has no expandable
derivation, `equation()` returns a one-element vector (formula only, no
substitution/result/legend lines):

- an **empty** quantity, or any quantity with tracing compiled out → the
  formatted value alone (the `N/A` text when empty);
- a **bare unnamed leaf** (a raw input never operated on) → its formatted value;
- an **engaged value with no recorded derivation node** — one materialised
  directly (the wire codec writing `payload`, or direct assignment to the public
  `payload` member), bypassing the value constructors that call `recordLeaf()` →
  its formatted value;
- a value whose **root is a bare unit conversion** (a `static_cast` / implicit
  conversion never further operated on, and left unnamed) → its formatted
  (converted) value — a lone `convert` node is treated as an atom;
- a value whose **root is named** (e.g. a `NamedQuantity`, or the result of
  `.named(...)`) → just `"the name"`, since a name is opaque and its derivation
  is deliberately not expanded. Introspect the *unnamed* computed value if you
  want the formula (see the calculation example).

So the symbols (the shape you'd write on paper) and the numbers (the audit of
what was actually computed) live in one coherent artifact rather than in two
methods that echo each other.

**Numbers print exactly as `std::format` prints them.** `equation()` carries no
precision of its own and never consults any field's *declared* precision: each
substituted value, the result, and each `where` value runs through the same
`detail::formatRationalDecimal` helper described in *How a value prints*, at the
node's own runtime `DecimalPlaces`. So a value reads identically in `equation()`
and in `std::format("{}", q)`. The unit is **not** shown (the tree is
unit-erased); pair the lines with `std::format("{}", q)` for the united answer.

**How each node renders.** Two properties of a sub-value decide its appearance:
whether it is **named**, and whether it is **reused** (appears more than once
along the displayed paths). These compose into one rule for leaves, conversions,
and computed values alike — **reuse, not leaf-vs-computed, is what mints a
placeholder**:

| Node | Formula (`[0]`) | Substitution (`[1]`) | `where` legend (`[3…]`) |
|---|---|---|---|
| **Named** (any kind) | its name `"x"`, not expanded | its value | never — a name is opaque and is not counted for reuse |
| **Unnamed leaf**, used once | its number (`3`) | its number | — (inlined) |
| **Unnamed leaf**, reused | placeholder `cN` | its value | `cN = <value>` |
| **Unnamed conversion**, used once | its converted value (an atom) | its value | — (inlined) |
| **Unnamed conversion**, reused | placeholder `cN` | its value | `cN = <value>` |
| **Unnamed computed**, used once | its inlined expression (`"a" - "b"`) | the substituted expression | — (inlined) |
| **Unnamed computed**, reused | placeholder `cN` | its value | `cN = <symbolic> = <substituted> = <value>` |

A value used exactly once is inlined at its one point of use and never reaches
the legend; only a *reused* value earns a `cN` placeholder, so shared work is
written once instead of duplicated at each use. Naming stops both expansion and
reuse-counting at that node — a conversion behind `"heater"` folds into the name.
Leave a value unnamed precisely when you *do* want its arithmetic written out.

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
value. The compile-time symbol is a `detail::FixedString` non-type template
argument (a structural fixed-capacity buffer built from a string literal), which
is what lets the name live in the type. `morph::units::detail::FixedString` is an
alias for the single shared `morph::detail::FixedString`
(`include/morph/detail/fixed_string.hpp`) — the *same* type the forms layer
exposes as `morph::forms::FixedString` for `Choice` (see [choice.md](../forms/choice.md));
there is one definition, not two. `NamedQuantity` publicly derives from
`Quantity<U>` (declared-precision default) and offers four ways in — a **default
constructor** (empty, then named), an **`optional<Rational>` constructor**, a
**`Quantity<U>` (`Base`) constructor**, and a **`static fromDouble(double)`** —
each of which constructs the plain value first and then applies `named(Name)`,
so every path funnels through the same naming node. Naming an *empty* value is a
no-op (see `named()`), so a default-constructed `NamedQuantity` is simply empty
and unnamed until a value arrives; that empty name never surfaces because
`equation()` and formatting both short-circuit on empty.

## Wire and schema

On the morph JSON wire a quantity is just its nullable `Rational` payload —
neither the unit, the declared precision, nor the derivation travels. The unit
surfaces only in generated JSON Schemas (as `ExtUnits`, from `UnitTraits`) and
in C++ types; the declared precision surfaces as `x-decimalPlaces`. A client
cannot send a mismatched unit, and provenance stays a local, in-process concern.

**Display units come from `relations`, not a second list.** `UnitRelation` is
the *single* source of within-dimension conversion. A renderer's display/entry
unit selector — surfaced in the schema as `x-unitAlternatives` — is **derived
from the same `relations`**: the alternatives for a field's unit are its
**direct relation neighbours** — every `UnitRelation` edge that touches the
unit, in either direction — each with the exact ratio that edge declares. There
is no separate `alternatives` declaration to keep in sync; declaring a
`UnitRelation` both enables `convert` and offers the unit in the selector.

Note the scope difference: the alternatives list is the **direct edges only**,
*not* the full transitive closure. `convert` itself chains through intermediate
units (see *Unit conversion*), so a value can convert to more units than the
selector lists; the selector deliberately offers only the one-hop neighbours the
`relations` array names directly. (In the lab system `kg` lists both `g` and `t`
as alternatives, but `g` lists only `kg` — even though `g → t` converts by
chaining through `kg`.)

### Pre-decode wire validation — declared bounds

`setWire` (above) is deliberately permissive: it silently clamps a hostile
`dp` and normalises a non-canonical numerator/denominator rather than
rejecting them, so decoding itself never throws. That leaves a gap for a
value that decodes *successfully* but is still physically or contractually
impossible for its unit — a percentage above 100, a mass below zero — with no
seam to reject it before an action's own `validate()` (a business-rule check,
not a decode-level one) runs.

`Quantity<U, Dec>::withinDeclaredBounds() -> bool` closes that gap, driven by
an **optional** customisation point:

```cpp
template <>
struct morph::units::UnitTraits<Unit> {
    static constexpr UnitMeta meta(Unit u) noexcept { /* ... */ }

    // Optional: declares [min, max] for units that have a physical/contract range.
    static constexpr morph::units::QuantityBounds bounds(Unit u) noexcept {
        switch (u) {
            case Unit::percent:
                return {.min = Rational{0, DecimalPlaces{1}}, .max = Rational{100, DecimalPlaces{1}}};
            default:
                return {.min = Rational{Numerator{std::numeric_limits<std::int64_t>::min()}, Denominator{1}, DecimalPlaces{1}},
                        .max = Rational{Numerator{std::numeric_limits<std::int64_t>::max()}, Denominator{1}, DecimalPlaces{1}}};
        }
    }
};
```

- **`QuantityBounds { Rational min; Rational max; }`** — an inclusive range.
- **`HasUnitBounds<E>`** — `true` when `UnitTraits<E>` declares `bounds(E)`.
  A unit enum with no `bounds()` declares none: every value its precision
  allows is accepted, byte-for-byte the same as before this feature existed
  — this is an opt-in check, not a new default restriction.
- **`withinDeclaredBounds()`** — `true` when the payload is empty (an
  unengaged field has nothing to be out of bounds), when the unit declares no
  `bounds()`, or when the engaged value satisfies `min <= value <= max`.
  Comparison is on the exact `Rational` (via `operator<=>`), never a lossy
  `double`.

**The forms-layer seam.** `morph::forms::checkQuantityBounds<A>(action)`
(`forms.hpp`) walks every reflected `Quantity` member of an action the same
way `reconcileDeclaredPrecision` does, and returns the wire name of the first
member failing `withinDeclaredBounds()` (or `std::nullopt`).
`morph::forms::enforceQuantityBounds<A>(action)` throws
`morph::forms::QuantityDecodeError` naming that field. Both dispatch runners
that decode wire JSON into an action — `ActionDispatcher::registerAction`'s
server-side runner and `ActionExecuteRegistry::registerAction`'s client
bridge runner (`registry.hpp`/`bridge.hpp`) — call `enforceQuantityBounds`
immediately after `reconcileDeclaredPrecision` and before `recomputeAll`/the
`ActionValidator::ready` check, so an out-of-bounds wire value is rejected
before an action's own `validate()` ever sees it. `QuantityDecodeError` is
deliberately **not** `morph::model::ValidationError` — the two stay distinct
so a caller (or a test) can tell "the wire payload itself was impossible"
from "the decoded action failed its own business rule". The in-process
`localOp` execution path (`bridge.hpp`) is unaffected, for the same reason it
skips `reconcileDeclaredPrecision`: no JSON decode happens there, so there is
nothing to validate at that seam — a `Quantity` constructed directly by
calling code carries whatever bounds the caller gave it.

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
        {Unit::g, Unit::kg, Rational{Numerator{1},    Denominator{1000}, DecimalPlaces{3}}},
        {Unit::t, Unit::kg, Rational{Numerator{1000}, Denominator{1},    DecimalPlaces{3}}},
    }};
};
```

`{Unit::g, Unit::kg, Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}}`
means **1 g = 1/1000 kg**.
The framework inverts the ratio for the reverse direction:
- `g → kg`: multiply value by `1/1000`
- `kg → g`: multiply value by `1000/1`

`fromTo` must be **strictly positive** (numerator > 0 and denominator > 0), and
`from` / `to` must be distinct units.

**Positivity is now a compile-time guard, not just a caller contract.** Every
`relations` array is consumed by the `consteval` `detail::conversionRatio`
breadth-first search, which calls `detail::requirePositiveRatio(fromTo)` on each
edge it touches (both the forward `fromTo` and, via `detail::reciprocal`, the
reverse). `requirePositiveRatio` is `consteval` and `throw`s on a non-positive
ratio, so a zero or negative `fromTo` is a **compile error** at the
relation-consuming call site rather than silent runtime corruption. This is the
fix for the old failure mode: a zero `fromTo` used to make the reverse direction
(`reciprocal`, a bare numerator/denominator swap) produce a degenerate `den:0`
ratio — which `Rational` clamps to `1` — silently corrupting conversions and
leaking `den:0` into `x-unitAlternatives`; a negative ratio silently flipped the
conversion direction. Both are now rejected before the program can be built.

`from == to` is separately excluded from *use* by the `SameEnumDistinct`
constraint on `convert`; a self-edge in `relations` is still not rejected at
declaration (only non-positive ratios are).

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
`kg→t`), composed inside `convert` before a single scaling multiply. Provenance
records the conversion as **one endpoint-to-endpoint step** — a single
`convert <fromId> -> <toId>` node whose operation names only the source and
target ids (e.g. `convert g -> t`). The intermediate units the search passed
through are **not** recorded as separate hops; the composed ratio is applied in
one multiplication and one node.

Only ratio-based `UnitRelation` edges take part in composition. A user-provided
`convert` (a non-ratio conversion such as °C ↔ °F) is a **direct edge only** — it
is never composed *through*, because arbitrary functions have no ratio to
multiply into a chain. The application is responsible for keeping declared ratios
mutually consistent; the framework composes whatever shortest path it finds and
does not cross-check that two different paths agree.

This coexists with the `consteval` unit algebra: the algebra combines *different
dimensions* (`kg * m3`, `kg / m3`, same-unit `→ scalar`), while `convert`
*scales within one dimension*.

### 5. Conversion edge cases (failure modes)

- **Chained composition overflows at *compile* time, not run time.**
  `conversionRatio` is `consteval` and multiplies the edge ratios *during
  constant evaluation*, so composing a path whose product exceeds the `int64`
  numerator/denominator range is a **hard compile error** at the call site that
  requested the conversion — never a silent runtime overflow. (A *direct*
  single-edge ratio is likewise composed at compile time.) The runtime scaling
  multiply that `convert` then applies to the value can still overflow per the
  usual `Rational` envelope — that part is the documented magnitude limitation,
  distinct from this compile-time guard.
- **A non-positive `fromTo` is a compile error.** The BFS calls
  `detail::requirePositiveRatio` on every edge it touches; that `consteval`
  guard `throw`s on a numerator ≤ 0 or denominator ≤ 0, so a zero/negative ratio
  declared in `relations` fails the build instead of producing a degenerate
  `den:0` (clamped to `1`) or sign-flipped conversion at run time. See
  [1. `UnitRelation` — declaring peer-to-peer ratios](#1-unitrelation--declaring-peer-to-peer-ratios).
- **The search always terminates.** The BFS runs over a fixed-capacity queue
  (`capacity = 2 * relationCount + 1`) and refuses to revisit a unit already
  enqueued (the `seen` scan over visited nodes), so it visits each reachable
  unit at most once and cannot loop, whatever the shape of the `relations`
  graph.
- **Ties resolve by declaration order, and paths are *not* cross-checked.**
  When two equal-length paths reach the target, the BFS takes whichever edge
  appears **first in the `relations` array**. The framework composes that one
  path and does **not** verify that a different path would yield the same
  factor. If an application's declared ratios are mutually inconsistent (a
  redundant edge that disagrees with the rest of the graph), **reordering
  `relations` can silently change a chained conversion factor**. Keeping the
  declared ratios mutually consistent is the application's responsibility (the
  framework composes whatever shortest path it finds).

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
| `UnitTraits<E>` | class template | **Customisation point.** The application specialises it for its unit enum `E`: a required `static constexpr UnitMeta meta(E)` (satisfies `UnitEnum`) and an *optional* `static constexpr std::array<UnitRelation<E>, N> relations` (detected by `HasUnitRelations`; a unit system without it simply offers no `convert`/alternatives). |
| `UnitRelation<E> { from, to, fromTo }` | struct | One exact peer-to-peer ratio (`1 from == fromTo·to`, a positive `Rational`). Entries drive the auto-generated `convert`, conversion chaining, **and** the display-unit selector. |
| `UnitAlternative<E> { unit, num, den }` | struct | A *derived* per-unit view (computed from `relations`, not declared): one direct-neighbour display/entry unit and its exact alternative-to-canonical ratio (`num`/`den`, `std::int64_t`). Returned by `Quantity::unitAlternatives()`; feeds `x-unitAlternatives`. |
| `UnitEnum<E>` | concept | Satisfied by an enum with a `UnitTraits<E>::meta`. Constrains `Quantity`. |
| `isQuantity<T>` | `inline constexpr bool` variable template | Compile-time test: `true` when `T` is a `Quantity<...>`. |
| `operator*`, `operator/` (on `E`) | `consteval` | Application-supplied unit algebra deducing cross-dimension result units; an unsupported combination fails to compile. |
| `QuantityBounds { min, max }` | struct | Inclusive `Rational` range returned by the optional `UnitTraits<E>::bounds(E)` customisation point — the pre-decode wire validation seam (see "Pre-decode wire validation" above). |
| `HasUnitBounds<E>` | concept | `true` when `UnitTraits<E>` declares `bounds(E)`. A unit enum without it declares no bounds: every value its precision allows is accepted. |

### `Quantity<U, Dec>` — compile-time members

| Member | Signature | Returns |
|---|---|---|
| `unit` | `static constexpr auto` | The unit enumerator `U`. |
| `declaredDecimals` | `static constexpr std::uint32_t` | The field's declared decimal count. |
| `declaredPrecision()` | `static constexpr DecimalPlaces` | `declaredDecimals` as the strong type. |
| `unitMeta()` | `static constexpr UnitMeta` | The unit's `UnitMeta`. |
| `unitAlternatives()` | `static constexpr std::span<const UnitAlternative<E>>` | The **direct-neighbour** display/entry units for this field's unit — every `UnitRelation` edge touching the unit, **derived from `UnitTraits::relations`** (empty when none; not the transitive closure). Drives the schema's `x-unitAlternatives`. |

### `Quantity<U, Dec>` — construction

| Member | Signature | Notes |
|---|---|---|
| default ctor | `constexpr Quantity() noexcept` | The **empty** state. |
| value ctor | `Quantity(Rational engaged)` | Engages, keeping the value's own runtime precision. |
| optional ctor | `Quantity(std::optional<Rational>)` | Adopts a payload as-is. |
| cross-precision ctor | `Quantity(Quantity<U, Other>)` | Same unit, different declared precision; value carries over unchanged. |
| `fromDouble` | `static Quantity fromDouble(double raw)` | Tags the leaf at `declaredPrecision()`; **never empty from a finite value** (empty only when `raw` is non-finite / doesn't fit). |
| `fromOptional` | `static Quantity fromOptional(std::optional<Rational>)` | Empty in → empty out, preserving the declared-precision arg. |

> The declared-decimals template argument `Dec` is constrained to `[0, kMaxDecimalPlaces]` (18).
> Values outside that range cause a `static_assert` failure at compile time. `Dec == 0` is a
> legal, first-class declared precision — it is what a zero-decimal currency (JPY, KRW) or a
> plain integer count declares, and it formats and parses with no fractional digit or decimal
> point at all (see *How a value prints*).

### `Quantity<U, Dec>` — access, precision, provenance

| Member | Signature | Notes |
|---|---|---|
| `hasValue()` | `constexpr bool hasValue() const noexcept` | Engaged? No implicit `bool` conversion. |
| `withinDeclaredBounds()` | `constexpr bool withinDeclaredBounds() const noexcept` | The pre-decode wire validation seam: `true` when empty, when the unit declares no `bounds()`, or when the engaged value satisfies `min <= value <= max` (exact `Rational` comparison). See "Pre-decode wire validation" above. |
| `value()` | `const std::optional<Rational>& value() const noexcept` | The payload; pattern-match or `->` it. |
| `value_or(fallback)` | `Rational value_or(Rational const&) const` | Payload if engaged, else the fallback. |
| `operator*` | `const Rational& operator*() const` | Unchecked access to the engaged value (UB when empty, like `std::optional`). |
| `withDecimalPlaces(p)` | `Quantity withDecimalPlaces(DecimalPlaces) const` | Retags actual precision (silently clamped to `[0, kMaxDecimalPlaces]`); no-op on empty. Value unchanged. |
| `atDeclaredPrecision()` | `Quantity atDeclaredPrecision() const` | Retags actual precision to the declared one; no-op on empty. |
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
| `NamedQuantity<Name, U>` | class template | `Quantity<U>` (declared precision defaulted) that names itself `Name` on construction; slices losslessly to a plain `Quantity<U>`. `Name` is a `detail::FixedString` NTTP. Constructors: default (empty), `optional<Rational>`, and from a plain `Quantity<U>` — each names the value after building it; plus `static fromDouble(double)`. The name lives in the shared history, not as extra data. |
| `std::formatter<Quantity<U, Dec>>` | specialisation | Renders value + unit (`5.2kW`, `N/A%`). No `operator<<`. |
| `std::formatter<NamedQuantity<Name, U>>` | specialisation | Forwards to the `Quantity` formatter. |

On the wire, `glz::meta<Quantity>` reduces the instance to its nullable
`Rational` `payload`. `to_json_schema<Quantity>` stamps the unit onto the
schema as `ExtUnits{ unitAscii = meta.id, unitUnicode = meta.display }` — that
is the *only* thing the quantity's own schema hook adds. The precision and
alternatives keys (`x-decimalPlaces`, `x-unitAlternatives`) are **not** emitted
by `to_json_schema`; the forms schema-merge layer (`morph::forms`'
`mergeSchemaExtras`) adds them per `Quantity` member from `declaredDecimals`
and `unitAlternatives()`.

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
| Formatting | **`std::formatter` only, delegating to the shared `formatRationalDecimal` renderer** | Single formatting path; no `operator<<`; the runtime `DecimalPlaces` tag is the sole authority on printed decimals. |
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
        {Unit::watt, Unit::kilowatt,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, morph::math::DecimalPlaces{3}}},
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

## Cross-references

- **[`rational.md`](rational.md)** — the payload type, and the one to read
  first for anything about values, precision, or overflow. `Quantity` inherits
  *all* of its numeric behaviour from `Rational`: the `int64` overflow envelope
  (silent UB in the no-error-channel operators), the `DecimalPlaces` precision
  tag and its `std::max` propagation, the half-away-from-zero rounding of
  `fromFloat` (via `llround`) that `Quantity::fromDouble` relies on. The decimal
  formatter (`formatRationalDecimal`) is a separate exact-integer long division
  over the canonical `num/den` at the runtime `DecimalPlaces`; it does **not**
  route through `Rational`'s own `std::formatter` or `toDouble`. This spec does
  not restate the `Rational` arithmetic rules.
- **[`forms.md`](../forms/forms.md)** — how a `Quantity` field reaches a generated form:
  `ExtUnits` (unit id/display, emitted by `to_json_schema`) plus the
  schema-merge keys `x-decimalPlaces` (from `declaredDecimals`) and
  `x-unitAlternatives` (from `unitAlternatives()`), added per member by
  `morph::forms`' `mergeSchemaExtras`.
- **[`choice.md`](../forms/choice.md)** and **[`datetime.md`](datetime.md)** —
  one-kind-of-empty siblings. `Choice<T>` and `Timestamp` share `Quantity`'s
  `std::optional`-backed single empty state and total `==` (empty == empty).
  **The family is deliberately *not* uniform on ordering — do not assume one
  behaviour across all three:**

  | Type | Ordering behaviour |
  |---|---|
  | `Quantity<U>` | `operator<=>` is defined but **throws `std::logic_error`** if either operand is empty (an empty quantity has no position on the number line). |
  | `Timestamp` | `operator<=>` is **total** — defaulted over the underlying `std::optional`, so empty sorts *before* any engaged instant; never throws. |
  | `Choice<T>` | **No `operator<=>` at all** — only `==`. A `Choice` is a selected option, not an orderable scalar, so relational comparison is intentionally absent (using `<` on one is a compile error). |

  Equality (`==`) *is* uniform across the three; ordering is not. Code that
  templates over "empty-capable field" types must therefore rely only on `==`
  and `hasValue()`, never on a common `<`/`<=>`.

## Limitations

Honest constraints of the current design, distinct from *Out of scope* (things
deliberately not attempted):

- **The unit algebra does not scale.** `operator*` / `operator/` on the unit
  enum are hand-written, O(pairs) `consteval` tables the application maintains
  by listing every supported combination. There is **no dimensional-analysis
  engine**: past roughly a dozen units the table grows unwieldy, and a
  *missing* rule surfaces as a compile error at a **distant call site** (the
  arithmetic expression that attempted the combination), not at the trait
  declaration. The type system catches the mistake, but the diagnostic points
  away from the fix.
- **Provenance is on by default and allocates eagerly.** With
  `MORPH_QUANTITY_PROVENANCE=1` (the default), **every** construction,
  arithmetic op, conversion, and `named()` heap-allocates a fresh `ASTNode`
  (`recordLeaf` and the `MORPH_Q_BUILD` macro) — even though the derivation
  never crosses the wire and has a **single consumer**, `equation()`. For hot
  paths that never call `equation()`, build with `MORPH_QUANTITY_PROVENANCE=0`:
  the API stays callable and no nodes are allocated.
- **`int64` ratio overflow for wide-range unit systems.** Conversion ratios are
  exact `Rational`s of 64-bit integers. A unit system spanning many orders of
  magnitude (pico- to tera-, say) risks overflowing a composed chained ratio —
  which, per *Conversion edge cases*, is a compile error — or the runtime
  scaling multiply. Keep within-dimension ratios inside the `int64` envelope.
- **`named()` / `equation()` output differs materially between build flags.**
  With tracing off, `equation()` collapses to the bare value and `named()` is a
  no-op that discards the name. Code that depends on the *content* of those
  outputs (tests, generated reports) behaves differently across the two builds —
  the toggle changes observable behaviour, not just performance.

## Out of scope

- Serializing history on the wire — only the `Rational` payload travels.
- Auto-deriving conversions the application hasn't declared — a pair is
  convertible only when a `UnitRelation` entry exists or a user-provided
  `convert(From, To&)` overload exists for it.
