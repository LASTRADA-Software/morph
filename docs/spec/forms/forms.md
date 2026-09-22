# `morph::forms` — schema generation & readiness for action types

Given a plain-aggregate action type `A` (registered with
[`BRIDGE_REGISTER_ACTION`](../core/registry.md#bridge_register_action)),
`morph::forms` produces a standard JSON Schema a client can render a form from,
and provides a compile-time `validate()` body that gates submission until every
required empty-capable field is filled in. It builds on glaze's
`write_json_schema<A>` (which already contributes types, `$defs`, per-field
metadata from `glz::json_schema<A>`, and `ExtUnits` — glaze's per-unit metadata
block, detailed under [Renderer contract](#renderer-contract-the-schema-key-vocabulary)
below — from `morph::units::Quantity`) and closes the gaps glaze leaves open.

## Design principle: infer by default, declare to override

Every feature below obeys one rule, which is what reconciles "rapid GUI
development" with "flexible when the generated form isn't enough":

1. **Infer from the type where possible.** A `Quantity` field already knows its
   unit and precision; a `Choice` already knows its options action; a
   `std::optional` already means "not required." The renderer gets as far as it
   can from types alone, with zero extra user declaration.
2. **Declare to override.** When inference is ambiguous or insufficient (a
   label, a layout group, a widget choice, a cross-field rule), the user adds a
   *typed, compile-time* declaration — a `static constexpr` member or a small
   registration macro on the action (these macros are hand-aligned behind
   `// clang-format off`; the rationale lives once in `CONTRIBUTING.md` under
   *Formatting/linting*, and each site carries a pointer rather than a copy).
   Never mandatory; absence falls back to a
   sensible convention.
3. **Escape hatch always available.** The schema below is a documented, stable
   contract (see "Renderer contract"). Anything the generated GUI cannot
   express, an app builds by consuming the schema directly or overriding one
   field's widget (see "Theming / component-override registry").

This is why the constraints placed on a model's action types are light: flat,
default-constructible, reflectable aggregates whose fields come from the known
palette (`Quantity`, `Choice`, `Timestamp`, primitives, or a user type exposing
`hasValue()`), plus *optional* typed declarations. Convention buys rapid;
override + direct-schema-consumption buys flexible.

Every schema key this module and its siblings ([choice.md](choice.md),
[views.md](views.md), [workflows_navigation.md](workflows_navigation.md))
introduce is **additive and optional** — the emitted schema stays unversioned,
and a renderer that doesn't recognize a new `x-*` key, or a new top-level
view/wizard/app document, ignores it harmlessly. Renaming, retyping, or
changing the meaning of an existing key is the only kind of change reserved for
a major release.

The **Qt/QML client** (`src/qt/forms`) is the reference renderer these specs
write concrete examples against, because it already consumes the schema
contract; the **schema contract itself stays renderer-agnostic** — every
`x-*` key and view/wizard/app-schema document is specified in platform-neutral
terms so a web, ImGui, or other renderer can implement the same contract.

## Contents

- [Design principle: infer by default, declare to override](#design-principle-infer-by-default-declare-to-override)
- [Empty state — `EmptyCapableField` concept](#empty-state--emptycapablefield-concept)
- [`Choice` — server-sourced picklist](#choice--server-sourced-picklist)
- [`FixedString` — NTTP compile-time string](#fixedstring--nttp-compile-time-string)
- [Widget hints — `Multiline` / `Ranged`](#widget-hints--multiline--ranged)
- [`schemaJson<A>()` — schema generation](#schemajsona--schema-generation)
- [Field metadata — `FieldMeta`](#field-metadata--fieldmeta)
- [Layout & grouping — sections, tabs, spans](#layout--grouping--sections-tabs-spans)
- [Renderer contract: the schema key vocabulary](#renderer-contract-the-schema-key-vocabulary)
- [Shipped Qt/QML reference renderer](#shipped-qtqml-reference-renderer)
- [Renderer conformance kit](#renderer-conformance-kit)
- [Theming / component-override registry](#theming--component-override-registry)
- [Localisation — message keys and the catalog seam](#localisation--message-keys-and-the-catalog-seam)
- [`allRequiredEngaged<A>()` — readiness check](#allrequiredengageda--readiness-check)
- [Cross-field rules — the `x-rules` vocabulary](#cross-field-rules--the-x-rules-vocabulary)
- [Computed fields](#computed-fields)
- [Per-instance constraints — values that live in data](#per-instance-constraints--values-that-live-in-data)
- [Support traits and helpers](#support-traits-and-helpers)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Cross-references](#cross-references)
- [Out of scope](#out-of-scope)

## Empty state — `EmptyCapableField` concept

A field type that has an internal blank state (nothing entered / nothing
selected) exposes `hasValue() -> bool`. This is the **only** thing the forms
module needs to know about a field to decide whether it counts as "engaged" —
**a field is engaged exactly when `hasValue()` returns `true`.** Every later
use of "engaged" in this document (required-ness, cross-field rules, computed
fields) means precisely this.

```cpp
template <typename T>
concept EmptyCapableField = requires(const T& field) {
    { field.hasValue() } noexcept -> std::convertible_to<bool>;
};
```

The `noexcept` requirement is load-bearing: `allRequiredEngaged` is itself
`noexcept`, so a `hasValue()` that can throw must not cross that boundary. A type
whose `hasValue()` is *not* `noexcept` silently fails the concept and is treated
as a non-empty-capable field (always engaged), so it never gates submission —
the `noexcept` clause is what surfaces that mistake at compile time.

Satisfied by:
- `morph::units::Quantity<U, Dec>` — `hasValue()` returns `true` when the
  `Rational` payload is present.
- `morph::forms::Choice<T, ...>` — `hasValue()` returns `true` when its
  `std::optional<T>` is engaged.
- `morph::time::Timestamp` — `hasValue()` returns `true` when its `DateTime`
  payload is present.
- `morph::util::Tagged<T, Tag>` — `hasValue()` always returns `true`: it wraps
  a *required* protocol scalar, not an optionally-empty one, so it opts into
  this concept the same way the others do but never gates submission (see
  [`tagged.md`](../util/tagged.md)).
- Any user type that exposes `bool hasValue() const noexcept`.

A **non**-empty-capable field (plain `int64_t`, `std::string`, …) is always
considered engaged — forms cannot know whether it has been "filled in" without
application-specific logic, so `allRequiredEngaged` simply skips it.

## `Choice` — server-sourced picklist

A field whose value is chosen from options served by another registered action.
Options are not hardcoded on the client — they come from executing the named
action over the same wire, and the result rows are mapped to a combo box.

```cpp
template <typename T, FixedString OptionsAction,
          FixedString ValueField = "id", FixedString LabelField = "name",
          FixedString... DependsOn>
struct Choice {
    std::optional<T> value;
    // ...
};
```

- `T` — the value type submitted **on the wire** (the JSON payload exchanged
  between client and server over the transport — see [wire.md](../core/wire.md)
  for the envelope this travels inside; `int64_t` for ids, `string` for codes).
- `OptionsAction` — the registered action type id whose result provides options
  (executed with an empty body when `DependsOn` is empty, or with
  `{name: value, ...}` built from the `DependsOn` names otherwise; returns
  `{valueField, labelField, ...}` rows either way).
- `ValueField` / `LabelField` — which result-row fields carry the submitted value
  and the display label; both default to `"id"` / `"name"`.
- `DependsOn` — an optional trailing pack of sibling wire field names whose
  current values parameterise the options action (a cascading picklist);
  empty by default. See [choice.md](choice.md) for the full design.

On the wire a `Choice` is just its nullable `T` — the options metadata lives in
the C++ type and the generated schema only, never in payloads. The
`glz::meta<Choice<...>>` specialisation reflects `value` directly, so glaze
serialises it as `T | null`.

## `FixedString` — NTTP compile-time string

A structural type that lets string literals be used as non-type template
parameters (C++20 NTTP):

```cpp
template <std::size_t N>
struct FixedString {
    std::array<char, N> data{};
    consteval FixedString(const char (&literal)[N]) noexcept;
    constexpr std::string_view view() const noexcept;
};
```

Used by `Choice` to embed the options-action name, value field, and label field
in the type itself.

## Widget hints — `Multiline` / `Ranged`

Two more thin wrappers carry rendering *control* intent in the type, in the
same spirit as `Choice`: a `Multiline` field is a `std::string` that should be
edited as a text area, and a `Ranged<Min, Max, Step>` field is a bounded
numeric that should be edited as a slider.

```cpp
struct Multiline {
    std::string value;
    static constexpr std::string_view widget() noexcept { return "textarea"; }
};

template <auto Min, auto Max, auto Step = 1>
struct Ranged {
    std::optional<decltype(Min)> value;
    bool hasValue() const noexcept { return value.has_value(); }
    static constexpr auto min() noexcept { return Min; }
    static constexpr auto max() noexcept { return Max; }
    static constexpr auto step() noexcept { return Step; }
    static constexpr std::string_view widget() noexcept { return "slider"; }
};
```

Both serialise through `glz::meta` as their bare payload — `Multiline` as a
plain JSON string, `Ranged` as a nullable number — so the wire is unchanged.
Neither type is `std::optional` itself, so both are *required* by the
[Required-ness rule](#required-ness-rule) unless opted out via
`optionalFields`. `Ranged` additionally satisfies `EmptyCapableField`
(`hasValue()` is `noexcept`), so it gates `allRequiredEngaged` exactly like
`Choice`; `Multiline` does not (a plain `std::string` payload has no
distinguishable "empty" state the forms module tracks) and so is always
considered engaged, same as an unwrapped `std::string` member.

`mergeSchemaExtras` emits `x-widget` on any property whose field type declares
a `noexcept static constexpr widget()` — the shape both types above expose —
and `x-min` / `x-max` / `x-step` on any property whose field type additionally
declares `min()` / `max()` / `step()` (the `Ranged` shape). An action may also
override the widget for *any* field — wrapped or plain — by naming it in the
same `static constexpr fieldMetadata` array the [field-metadata
feature](#field-metadata--fieldmeta) uses, as long as its entries expose
`.field` and a non-empty `.widget` (both string-view-convertible); this is
read structurally (duck-typed), so this header does not gain a named
dependency on `FieldMeta`'s declaration — any type shaped that way is
honoured, and the override always wins over a type's own derived `widget()`.
Full API, the `$defs`-collapse caveat shared with `Choice`, and design
rationale are in [widget_hints.md](widget_hints.md).

## `schemaJson<A>()` — schema generation

Produces a complete JSON Schema string for action type `A`, post-processing the
output of `glz::write_json_schema<A>()` to add seven annotation groups:

| Annotation | Scope | Contents |
|---|---|---|
| `required` | Top-level, and every nested-aggregate object schema (see [Nested aggregates (recursive, depth-bounded)](#nested-aggregates-recursive-depth-bounded)) | Array of field names that are **not** `std::optional<...>` and not listed in `A::optionalFields`. Always written, overwriting whatever glaze produced: glaze never derives `required` from member types — it emits one only where a type declares `meta<V>::required` (and for a tagged variant's discriminator) — so morph does not rely on its absence. |
| `x-order` | Every property | The member's declaration index (0‑based), so a renderer lays fields out in declaration order regardless of JSON key ordering. |
| `x-decimalPlaces` | `Quantity` properties | The field's declared precision (`Quantity<U, Dec>::declaredDecimals`). |
| `x-unitAlternatives` | `Quantity` properties | Convertible display/entry units derived from `UnitTraits::relations`, each with `{id, display, decimals, num, den}` — `id`/`display`/`decimals` come from the alternative unit's `UnitMeta`, and `num`/`den` are the exact alternative-to-canonical ratio. Omitted entirely when the field's unit declares no convertible units. |
| `x-optionsAction` / `x-optionValue` / `x-optionLabel` | `Choice` properties | The action that serves the options and which result fields to use. |
| `x-optionsDependsOn` | `Choice` properties whose options depend on sibling fields | Wire names of the sibling fields that parameterise the options action; omitted when the `Choice` declares no dependency. |
| `x-widget` / `x-min` / `x-max` / `x-step` | Properties whose field type declares `widget()` (optionally `min()`/`max()`/`step()`), or any field named in a `fieldMetadata`-shaped override | The preferred control id, and (for a bounded numeric) the slider's track bounds and increment ([widget_hints.md](widget_hints.md)). |

The result is **computed once per type and cached** in a `static const std::string`
inside `schemaJson<A>()`. On internal failure (malformed intermediate JSON,
etc.) the unmerged glaze schema is returned — or an empty string when even
glaze's own `write_json_schema<A>()` failed, since `schemaJson` feeds
`mergeSchemaExtras` with `write_json_schema<A>().value_or(std::string{})`.

Schema generation throws in exactly **one** case, and never for malformed
input: an `A::formRules` declaration that contradicts `A`'s own derived
`required` array so completely that no submission could satisfy both — see
[Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules).

### Required-ness rule

Required is the default — the safer choice for domain forms, since forgetting
to mark a field optional loses data rather than silently accepting a gap (see
[Design decisions](#design-decisions) for the full rationale). A member is
*optional* (and therefore not added to `required`) when any of:
1. Its type is `std::optional<...>`, or
2. Its name appears in `A::optionalFields` — a `static constexpr` iterable of
   `std::string_view` that the action declares, or
3. It is the destination of an `A::computedFields` entry — a derived,
   read-only field is never something the user must fill in; see
   [Computed fields](#computed-fields).

Required-ness is derived here, and `x-rules` is derived from `A::formRules`,
**independently**. They can therefore disagree; the one disagreement that makes
the form unsubmittable is rejected at generation, see
[Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules).

```cpp
struct RecordMeasurement {
    std::int64_t sampleId = 0;
    Density density{};
    Moisture moisture{};   // optional

    static constexpr std::array optionalFields{std::string_view{"moisture"}};
    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};
```

### `mergeSchemaExtras` — DOM post-processing

The actual workhorse behind `schemaJson`. It parses the glaze schema into a
`glz::generic_u64` DOM (preserving `int64`/`uint64` bounds in `$defs`),
iterates reflected members via `forEachNamedMember`, and patches the DOM in
place. If the input schema is not valid JSON the raw string passes through
unchanged.

#### Reading the DOM with `findMember`

Patching the DOM in place means the walkers do two different things through the
same syntax, and only one of them is safe to leave unchecked:

- **A write** — `property["x-order"] = I`, `dom["required"] = names` — *means*
  to create the member. `operator[]`'s insert-on-missing is the behaviour
  wanted, and these sites keep it.
- **A read** — "is there an `items` node?", "does `$defs` hold this key?" —
  must not create anything. A read that inserts adds a null member to the
  schema being emitted, and (because `glz::generic_u64`'s object storage
  reallocates on insert) can invalidate a node reference an enclosing frame of
  the mutually recursive walk still holds.

`detail::findMember(node, key)` is the read. It returns a pointer to the
member, or `nullptr` when `node` is not an object or holds no such key; the
caller branches on that instead of subscripting. It replaces a
`contains(key)` + `operator[](key)` pair, which probed the same map twice, at
every read site in `forms.hpp` and `instance_constraints.hpp`. The pointer is
into `node`'s own storage, so a caller may write through it — and, exactly like
the reference `operator[]` returns, it is invalidated by any insertion into
`node`.

`Node` is deduced, so a `const` DOM yields a `const` member and both
constnesses share one implementation with no `const_cast` and no copy.

**Why this is morph's own rather than glaze's.**
`cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` fires on every
one of these subscripts and advises a "bounds-safe alternative". On
`glz::generic_json` there is none. `at(key)` is defined as
`{ return operator[](key); }` for *both* overloads (glaze v7.4.0,
`glaze/json/generic.hpp:320` and `:322`), and the non-const `operator[]` it
forwards to inserts a default-constructed member for a missing key
(`generic.hpp:201-211`):

| spelling | non-const DOM | const DOM |
|---|---|---|
| `node[key]` | inserts a null member and returns it | `glaze_error("Key not found.")` — throws |
| `node.at(key)` | identical: it *is* `operator[]` | identical: it *is* the const `operator[]` |
| `findMember(node, key)` | `nullptr`, DOM unchanged | `nullptr`, DOM unchanged |

So the checking depends on the constness of the DOM, not on the spelling, and
these walkers are mutating by construction. A mechanical `operator[]` → `at()`
sweep over them would silence ~70 findings while changing a read into a write
on exactly the inputs the check warns about (morph#706).
`tests/test_forms_dom_access.cpp` asserts both halves — that `findMember` leaves
the document byte-identical on a miss, and that `at()` on the pinned glaze does
not — so a glaze release that gives `at()` real checked semantics turns that
file red rather than leaving this rationale quietly stale.

The sites that still carry a standing
`NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)` are the
writes, and the directive now says so.

## Field metadata — `FieldMeta`

An action declares per-field presentation — label, help, placeholder,
read-only, hidden — with a `static constexpr std::array<FieldMeta, N>` (or,
for the `describe<>()` sugar, a `static const` array defined out-of-line —
see below) named `fieldMetadata`, mirroring the `optionalFields` convention
above: a compile-time declaration on the action type, surfaced through the
schema.

```cpp
struct FieldMeta {
    std::string_view field;                 // wire key of the member
    std::string_view label{};               // "" = infer from name
    std::string_view help{};                // "" = omit description
    std::string_view placeholder{};         // "" = omit x-placeholder
    std::string_view widget{};              // widget-selection override (see widget_hints.md)
    bool readOnly{false};
    bool hidden{false};
    std::string_view i18nKey{};             // "" = derive the key stem; see below
    std::optional<math::Rational> minimum{};     // disengaged = no floor
    std::optional<math::Rational> maximum{};     // disengaged = no ceiling
    std::optional<math::Rational> multipleOf{};  // disengaged = any value
};

struct RecordMeasurement {
    Choice<std::int64_t, "ListSamples"> sampleId;
    Density density{};
    Moisture moisture{};

    static constexpr std::array fieldMetadata{
        FieldMeta{.field = "sampleId", .label = "Sample",
                  .help = "Which logged sample this measurement belongs to."},
        FieldMeta{.field = "density",  .placeholder = "e.g. 1050"},
        FieldMeta{.field = "moisture", .readOnly = true},
    };
};
```

Absence of `fieldMetadata` leaves every field at its inferred default: a
`title` derived from the member name, nothing else. `mergeSchemaExtras`
looks up (via `detail::findFieldMeta<A>`) the entry, if any, whose `field`
matches each reflected member and patches the property node — the same
property node that already carries `x-order` and the `Choice`/`Quantity`
keys (see "Where the keys physically land" below). An entry naming a field
that does not exist on the action is silently ignored: no crash, no stray
property.

### Label inference

When no descriptor overrides a field's label, `detail::inferTitle` derives a
title from the wire key: split on camelCase and underscore boundaries,
capitalise each word — `dryMassPct` → `"Dry Mass Pct"`, `sample_id` →
`"Sample Id"`, a single-word `notes` → `"Notes"`. This is a pure function of
the member name, so it costs nothing per action and needs no declaration. A
descriptor's non-empty `label` always wins over the inferred title, and
`title` is **always emitted** — an unannotated action gains only this key,
otherwise unchanged.

### `describe<&Action::field>(...)` — deriving the field name from the member

`describe<MemberPtr>(label, help)` builds a `FieldMeta` whose `field` is
resolved from the pointer-to-member itself (`detail::memberWireName`), so the
wire key is never restated as a string:

```cpp
static const std::array<morph::forms::FieldMeta, 2> fieldMetadata;
// ... after the class's closing brace:
inline const std::array<morph::forms::FieldMeta, 2> RecordMeasurement::fieldMetadata{
    morph::forms::describe<&RecordMeasurement::sampleId>("Sample", "Which logged sample…"),
    morph::forms::describe<&RecordMeasurement::moisture>().withReadOnly(),
};
```

`FieldMeta::withPlaceholder(text)`, `::withReadOnly()`, and `::withHidden()`
each return a modified copy, so `describe<>()`'s result can be extended
fluently as shown above. `describe<>()` produces the exact same property
annotations as the equivalent hand-written `FieldMeta{.field = "…", ...}`
literal.

`describe<>()` is deliberately **not** `constexpr`/`consteval`, and a
`fieldMetadata` array built from it must be **declared inside the class and
defined just after its closing brace** rather than as a single in-class
initializer, for two reasons verified while implementing this feature:

1. **Incomplete-type self-reference.** A static data member's in-class
   initializer is evaluated while the enclosing class is still incomplete
   (unlike a member function body or a default member initializer, neither
   of which this is); resolving `&RecordMeasurement::sampleId`'s wire name
   requires constructing a probe `RecordMeasurement` instance, which an
   incomplete type cannot do.
2. **glaze's reflection is not `constexpr` for reflectable aggregates.**
   `glz::get_member`, which `detail::forEachNamedMember` calls, is an
   ordinary runtime function — so even resolving the name outside the class
   cannot happen inside a `constexpr`/`consteval` function.

The plain `FieldMeta{.field = "sampleId", ...}` literal form is unaffected by
either restriction (it never references the enclosing class) and stays a
single in-class `static constexpr` array.

### Per-field scalar bounds — `minimum` / `maximum` / `multipleOf`

The three numeric members are the one part of `FieldMeta` that is **not**
presentation. They declare a bound on one field's value, and one declaration
drives both halves of it: `schemaJson<A>()` serves them as the standard
JSON-Schema keys of the same names, and `allFieldBoundsSatisfied<A>(action)`
evaluates them in C++ so an action's `validate()` enforces the identical
numbers the client was shown.

```cpp
struct CreatePaste {
    Reads burnAfterReads;                  // Quantity<Unit::count, 1>; empty = no burn limit

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "burnAfterReads",
                                .minimum    = math::Rational{1, math::DecimalPlaces{1}},
                                .multipleOf = math::Rational{1, math::DecimalPlaces{1}}},
    };

    [[nodiscard]] bool validate() const noexcept {
        return morph::forms::allFieldBoundsSatisfied(*this);
    }
};
```

`FieldMeta::withMinimum(r)` / `::withMaximum(r)` / `::withMultipleOf(r)` return
modified copies, so a `describe<>()`-built entry can carry a bound too.

**Why this is not part of the `x-rules` vocabulary.** Every comparison node
there — `greater`, `greaterOrEqual`, `less`, `lessOrEqual` — takes **two member
pointers of the same action**; only `equals` accepts a literal, and it
expresses equality alone. So "at least 1" had no right-hand operand to name,
and integrality had no spelling at all. `x-rules` is also, by its own title,
the *cross-field* vocabulary: a bound on a single field's value is a property
of that field, and belongs on its property node beside `title` and
`x-decimalPlaces` rather than in a rule node whose `fields` array would hold
one entry.

**Why not `UnitTraits::bounds`.** That is a decode-side check keyed by
**unit**, not by field ([`checkQuantityBounds`](#pre-decode-wire-validation--checkquantitybounds)),
so a floor declared for `CreatePaste::burnAfterReads` would equally constrain
`PasteView::readCount` — the same `Quantity` type over the same unit, which
legitimately starts at 0. It also has no integrality vocabulary, and never
touches `schemaJson<A>()`.

**Where the keys land, and why it matters.** On the **property node**, beside
the `$ref` — never in the `$def` the `$ref` points at. Two members of the same
`Quantity` type share one `$def`, so a bound written there would leak onto both
and reintroduce exactly the per-unit behaviour above. The shipped renderer's
`resolveRef` merges the property node **over** the resolved definition, so it
reads a per-field bound with no special case (see
[Where the keys physically land](#where-the-keys-physically-land--ref-resolution-is-mandatory)).

For a `Quantity` member the bound is on **the scalar value the field denotes,
in the canonical unit** — not on the `{num,den,dp}` object the member
serialises as. That is the reading both the shipped renderer (which compares
the entered value only while the canonical unit is selected) and
`allFieldBoundsSatisfied` (which compares the engaged `math::Rational`) apply.

**What the C++ predicate checks.** `allFieldBoundsSatisfied` reads bounds from
three member kinds and no others: a `Quantity` (its engaged `Rational`), a bare
`math::Rational`, and an integral member every value of which is exactly
representable as a `Rational` numerator — every signed integer type, plus every
unsigned one narrower than 64 bits. A declaration on any other member — a
`std::string`, a `bool`, a `std::uint64_t` — is inert in C++; the schema still
advertises it. An **unengaged** `EmptyCapableField` is vacuously satisfied,
exactly as the `x-rules` comparison kinds are: a form still being filled in
must not fail a bound on a field that has no value yet, and whether the field
must be filled at all is `required`/`allRequiredEngaged`'s question.

**Exactness.** The C++ comparisons run on the exact `Rational`, never on a
`double`; `multipleOf` uses `math::checkedDiv`, so a quotient too large to
represent is refused rather than silently saturating into an integer. The
*served* number is a JSON number and therefore an approximation for a
non-integral bound — an integral bound is emitted as an integer and picks up
the usual [`x-exactMinimum`/`x-exactMaximum`](#exact-numeric-bounds--x-exactminimum--x-exactmaximum)
companion above 2^53. As everywhere else in this contract, the live client gate
is an approximation and the model is the floor.

`multipleOf` must be **strictly positive**, as JSON Schema requires: a zero
divisor has no meaning and a negative one divides the same set of values as its
magnitude. A non-positive declaration is ignored — neither emitted nor checked.

A per-*instance* `x-minimum`/`x-maximum` written by
[`InstanceConstraints`](#per-instance-constraints--values-that-live-in-data)
composes with a compiled bound rather than replacing it: the renderer checks
both, so an instance range narrows the declared one and never widens it.

### Field metadata is not a security control

`x-readonly` and `x-hidden` are presentation only. The field still travels in
the payload — a hand-built wire envelope can set it freely regardless of
either flag. Enforcement of anything security-sensitive stays server-side
(see [security.md](../security.md)); a truly secret field must not be a
member of the action at all.

### Emitted keys

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `title` | property node (sibling of `$ref`) | string | The field's display label — an explicit `FieldMeta::label`, else the inferred title-cased member name. **Always emitted.** |
| `description` | property node (sibling of `$ref`) | string | Help text, from `FieldMeta::help`. Omitted when empty. A non-empty `help` overrides any `description` glaze stamped from a `glz::json_schema<A>` block; an empty `help` leaves an existing glaze-authored `description` untouched. |
| `x-placeholder` | property node (sibling of `$ref`) | string | In-control placeholder/hint shown while the field is empty, from `FieldMeta::placeholder`. Omitted when empty. Never submitted. |
| `x-readonly` | property node (sibling of `$ref`) | boolean | `true` when the field should be displayed but not editable. Emitted only when `true`. |
| `x-hidden` | property node (sibling of `$ref`) | boolean | `true` when the field should not be shown at all; the field remains part of the action payload. Emitted only when `true`. |
| `x-i18nKey` | property node (sibling of `$ref`) | string | An explicit message-key **stem** override, from `FieldMeta::i18nKey`. Omitted when empty. Not a complete key by itself — see [Localisation — message keys and the catalog seam](#localisation--message-keys-and-the-catalog-seam) for how a renderer expands it per text slot. |
| `x-widget` | property node (sibling of `$ref`) | string | Control-selection override, from `FieldMeta::widget`. Omitted when empty. Full mechanism, precedence over a type's own derived `widget()`, and design rationale are in [Widget hints](#widget-hints--multiline--ranged). |
| `minimum` | property node (sibling of `$ref`) | number | Inclusive lower bound on the field's value, from `FieldMeta::minimum`. Omitted when not declared. Standard JSON-Schema vocabulary, not an `x-*` key — see [Per-field scalar bounds](#per-field-scalar-bounds--minimum--maximum--multipleof). |
| `maximum` | property node (sibling of `$ref`) | number | Inclusive upper bound, from `FieldMeta::maximum`. Omitted when not declared. |
| `multipleOf` | property node (sibling of `$ref`) | number | The field's value must be an exact integer multiple of this, from `FieldMeta::multipleOf`. `1` is how "whole number" is spelled. Omitted when not declared, or when the declared value is not strictly positive. |

All ten keys are additive and non-breaking, extending the renderer-contract
table below without renaming or retyping any existing key, per this program's
versioning stance (see "Design principle" above). A
renderer that ignores them falls back to today's behavior exactly: it shows
the raw wire key as the caption, no helper/placeholder text, every field
editable and visible, and no scalar bound gated client-side (the model still
refuses an out-of-bounds value).

## Layout & grouping — sections, tabs, spans

An action may declare visual structure over its flat field list: a
`static constexpr` `formLayout` groups fields into titled sections, tabs, or
an accordion panel, and a parallel `static constexpr` `fieldSpans` widens
individual fields in a grid renderer. Both mirror the `optionalFields`
convention above — a `static constexpr` list `mergeSchemaExtras` looks for by
name, present only when an action opts in. Absent either, `schemaJson<A>()`'s
output is unchanged: no `x-layout`, `x-group`, `x-section`, or `x-colspan` key
is emitted, and a renderer lays every field out exactly as it always has
(flat, `x-order` order).

`groupKindName(GroupKind)` (`forms/layout.hpp`) is the one place the three
enumerators are spelled for the wire — `"section"`, `"tab"`, `"accordion"`, in
`x-layout.groups[].kind`. It is also the downgrade path: an out-of-range value
names itself `"section"`, matching the renderer's documented fallback for a
group kind it does not implement.

```cpp
// morph::forms::FieldGroup / FieldSpan / GroupKind — forms/layout.hpp.
enum class GroupKind { Section, Tab, Accordion };

struct FieldGroup {
    std::string_view title;                    // section / tab / panel heading
    GroupKind kind{GroupKind::Section};
    std::span<const std::string_view> fields;  // member wire keys (membership;
                                               // intra-group order is x-order)
};

struct FieldSpan {
    std::string_view field;   // wire key
    int colspan{1};           // grid columns this field spans
};

struct RecordMeasurement {
    Choice<std::int64_t, "ListSamples"> sampleId;
    Density  density{};
    Moisture moisture{};
    std::string notes;

    static constexpr std::array kIdent{std::string_view{"sampleId"}};
    static constexpr std::array kMeas{std::string_view{"density"},
                                      std::string_view{"moisture"}};
    static constexpr std::array kNote{std::string_view{"notes"}};

    static constexpr std::array formLayout{
        FieldGroup{.title = "Identity",    .fields = kIdent},
        FieldGroup{.title = "Measurement", .fields = kMeas},
        FieldGroup{.title = "Notes", .kind = GroupKind::Accordion, .fields = kNote},
    };
    static constexpr std::array fieldSpans{
        FieldSpan{.field = "notes", .colspan = 2},
    };
};
```

Each group carries its own `kind`: `Section` (the default) renders as a
titled fieldset stacked vertically, consecutive `Tab` groups render as panes
of one shared tab bar, and `Accordion` renders as a collapsible panel.
`x-order` remains the sole authority on **intra-group** ordering —
`formLayout`'s array order gives the cross-group order and a group's
`fields` list gives membership only. A field named in no group falls into an
implicit trailing default group, in `x-order` order, so declaring a group for
*some* fields never hides the rest — the no-`formLayout` case is simply one
implicit group containing every field, which is exactly today's flat form.

`mergeSchemaExtras<A>` (see above) stamps this onto the DOM in a pass that
runs only when `A::formLayout` / `A::fieldSpans` exist
(`detail::HasFormLayout<A>` / `detail::HasFieldSpans<A>`, `forms/layout.hpp`):
the ordered group list becomes a single top-level `x-layout` object, and each
reflected member (via `forEachNamedMember`) gets `x-group`/`x-section` (if it
is named in a group) and `x-colspan` (if its declared span exceeds `1`). A
group naming a field the action does not have is silently ignored (schema
generation never throws); a field claimed by two groups keeps the **first**
one that names it.

The reference QML renderer (`examples/forms/gui_qml/qml/DynamicForm.qml`)
buckets its flat `fields` array into `sections` keyed on each field's
`x-section`, merges consecutive `"tab"`-kind sections into one shared tab
bar (`renderRuns`), and lays each section's fields out in a 2-column grid
honoring `x-colspan` — falling back to a single implicit flat section
(one column, no chrome) when the schema carries no `x-layout` at all.

**Tab switching destroys and rebuilds controls, so they re-seed from
`fieldValues`.** The tab bar drives its `Repeater` off
`sections[currentTab].fields`, so leaving a tab destroys that tab's field
delegates and returning to it creates new ones. A control's `text` otherwise
flows only *outward* (via `onTextChanged` into `fieldValues`) and never back,
so a returned-to tab showed empty controls while the form went on
auto-submitting the values it still held — sending data the user could not see.
Each text-bearing control therefore re-seeds itself from `fieldValues` on
`Component.onCompleted`, under the `programmaticEdit` suppression so
re-creating a control never fires the action. A `text:` binding would not work
here: prefill assigns `text` imperatively, which would break it.

## Renderer contract: the schema key vocabulary

This is the **normative** list of every key a renderer must understand to build
a form from a morph action schema. Standard JSON-Schema keywords (`type`,
`properties`, `$defs`, `$ref`, numeric bounds, …) are emitted by
glaze and behave per the JSON-Schema 2020-12 spec; the table below covers the
keys morph either **synthesises** (`required`, `title`, `description` when a
`FieldMeta::help` is declared, `minimum`/`maximum`/`multipleOf` when a
`FieldMeta` declares them, the `x-*` extensions) or **relies on glaze to
stamp** (`format`, `ExtUnits`, `description` when no `FieldMeta::help` overrides
it, `minimum`/`maximum` for a scalar member's own type range). A renderer that
ignores an `x-*` key
still produces a usable form — it just loses the affordance that key carries
(unit selector, field order, combo box, decimal step).

This table is the complete reference for every key; two rows (`x-rules`,
`x-computed`/`inputs`) name concepts — cross-field rules and computed fields —
that get their own full explanation later in this document
([Cross-field rules](#cross-field-rules--the-x-rules-vocabulary),
[Computed fields](#computed-fields)). Skip ahead to those sections first if
the two rows below aren't self-explanatory on a first read.

### Where the keys physically land — `$ref` resolution is mandatory

A `Quantity` (or any aggregate) member is **not** inlined into its property.
glaze emits the member's type once into top-level `$defs` and the property node
carries only a `$ref` pointing at it, e.g.:

```json
"$defs": {
  "quantity_kg_per_m3": { "type": "object", "ExtUnits": { "unitAscii": "kg_per_m3", "unitUnicode": "kg/m³" }, ... }
},
"properties": {
  "density": { "$ref": "#/$defs/quantity_kg_per_m3", "x-order": 2, "x-decimalPlaces": 1, "x-unitAlternatives": [ ... ] }
}
```

The two kinds of annotation therefore live in **different nodes**, and a renderer
must resolve the `$ref` to see both:

- **`ExtUnits` lives in the `$def` of the unit type** — glaze stamps it onto the
  `Quantity`'s type definition, not onto the property. Many properties of the
  same unit type share one `$def` and therefore one `ExtUnits`.
- **`x-order`, `x-decimalPlaces`, `x-unitAlternatives`, `x-optionsAction` /
  `x-optionValue` / `x-optionLabel` / `x-optionsDependsOn` are siblings of the
  `$ref` on *this* property** — `mergeSchemaExtras` patches
  `dom["properties"][name]`, which is the property node holding the `$ref`.
  This is still true for a `Quantity`/`Choice` property's own `$def` (the
  `quantity_kg_per_m3`-style def shown above never gets `x-order`/`required`/
  title — only `ExtUnits` and glaze's own `type`/bounds/`description` live
  there). It is **not** true for a *nested-aggregate* member's `$def`: see
  [Nested aggregates (recursive, depth-bounded)](#nested-aggregates-recursive-depth-bounded)
  below — that `$def` **does** get `required`/`x-order`/title/etc. patched
  directly into it, the same as any other object schema.

The **"Where"** column below names the node each key is written to. A renderer
resolves the `$ref` into `$defs`, then merges: per-property `x-*` keys (from the
property node) win, and `ExtUnits` (plus glaze's `type`/bounds/`description`) come
from the resolved def. The shipped `MorphForms` QML renderer's (`src/qt/forms`,
below) `DynamicForm.qml`'s `resolveProp` does exactly this dual read.

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `required` | top-level (object), and every nested-aggregate object schema (inlined property or `$defs` entry) — see [Nested aggregates (recursive, depth-bounded)](#nested-aggregates-recursive-depth-bounded) | array of strings | Names of members that must be engaged before submit. A member is listed unless it is a `std::optional<...>`, appears in `A::optionalFields`, or is a `computedFields` destination (see the [Required-ness rule](#required-ness-rule)). Always emitted (an explicit `[]` when nothing is required). The renderer blocks submission until every listed field has a value. |
| `x-order` | property node (sibling of `$ref`) | non-negative integer | The member's 0-based **declaration index**. Renderers lay fields out in ascending `x-order`, not in JSON key order (object key order is not preserved across DOMs). |
| `x-decimalPlaces` | property node (sibling of `$ref`) | non-negative integer | The field's *declared* precision (`Quantity<U, Dec>::declaredDecimals`, unit default unless the type overrides it). The numeric input step / rounding granularity for entry in the canonical unit. **Enforced, not merely advisory:** the request/reply dispatch path *rounds* each submitted `Quantity` to this precision before storing it — the stored value, not just its tag, is reduced (see [Advertised precision is enforced on dispatch](#advertised-precision-is-enforced-on-dispatch)). A model serving one *instance* of an action may overwrite this with a value from data — see [Per-instance constraints](#per-instance-constraints--values-that-live-in-data); `x-instanceConstraints` (below) says when it did. |
| `x-unitAlternatives` | property node (sibling of `$ref`) | array of objects | Convertible display/entry units for the field, derived from `UnitTraits<E>::relations`. **Omitted entirely** when the unit declares no convertible peers. Each element has the five subfields below. The renderer offers these as a unit selector and recomputes the entered value *exactly* on switch; the submitted payload is always in the canonical unit (the one named by `ExtUnits`). |
| ↳ `id` | alternative entry | string | Stable ascii id of the alternative unit (`UnitMeta::id`). |
| ↳ `display` | alternative entry | string | Human display text of the alternative unit (`UnitMeta::display`). |
| ↳ `decimals` | alternative entry | non-negative integer | The alternative unit's own default decimals (`UnitMeta::defaultDecimals`) — the input step to use while that unit is selected. |
| ↳ `num` | alternative entry | signed integer | Numerator of the exact **alternative→canonical** ratio. |
| ↳ `den` | alternative entry | signed integer | Denominator of that ratio. `value_in_canonical = value_in_alternative · num / den`; `num`/`den` are the `Rational` numerator/denominator of the composed relation, so the recompute is exact (no floating-point drift). |
| `x-optionsAction` | property node (sibling of `$ref`) | string | Type id of the registered action whose result rows populate this field's combo box. Executed with an empty body, unless the property also carries `x-optionsDependsOn` (below), in which case the request body is `{parentField: value, ...}` built from the named sibling fields' current values. |
| `x-optionValue` | property node (sibling of `$ref`) | string | Which result-row field carries the value submitted on the wire (default `"id"`). |
| `x-optionLabel` | property node (sibling of `$ref`) | string | Which result-row field carries the display label (default `"name"`). |
| `x-optionsDependsOn` | property node (sibling of `$ref`) | array of strings | Wire field names of sibling fields whose current values parameterise this field's options action (a cascading picklist). The renderer sends `{name: value, …}` as the options-action request body instead of an empty one, and re-fetches — clearing a now-invalid selection — whenever any listed field changes. **Omitted entirely** when the `Choice` declares no dependency. |
| `title` | property node (sibling of `$ref`) | string | The field's display label — an explicit `FieldMeta::label`, else a title-cased member name (`dryMassPct` → "Dry Mass Pct"). Standard JSON-Schema vocabulary, not an `x-*` key. **Always emitted.** See "Field metadata" above. |
| `x-placeholder` | property node (sibling of `$ref`) | string | In-control placeholder/hint shown while the field is empty, from `FieldMeta::placeholder`. Omitted when empty; never submitted. |
| `x-readonly` | property node (sibling of `$ref`) | boolean | `true` when the field should be displayed but not editable. Emitted only when `true` — including on every `computedFields` destination (see [Computed fields](#computed-fields)). Not a security control — see "Field metadata is not a security control" above. |
| `x-computed` | property node (sibling of `$ref`) | object | Marks the field as derived. Present when the action declares it as a `computed(...)` destination ([Computed fields](#computed-fields)); absent otherwise. |
| ↳ `inputs` | `x-computed` object | array of strings | Wire field names of the sibling fields the value derives from, in declaration order. Advisory to the renderer; **authoritative computation is the server's** — see [Where the value is authoritative](#where-the-value-is-authoritative). |
| `x-hidden` | property node (sibling of `$ref`) | boolean | `true` when the field should not be shown at all; the field remains part of the action payload. Emitted only when `true`. Not a security control. |
| `x-widget` | property node (sibling of `$ref`) | string | The preferred control id: `"textarea"`, `"slider"`, `"radio"`, `"combo"`, `"password"`, `"checkbox"`, … A `fieldMetadata`-shaped override (a `.field`/`.widget` entry, read structurally — see [widget_hints.md](widget_hints.md)) wins; else the field type's own `widget()` (`Multiline`, `Ranged`). **Advisory** — a renderer that lacks the named control falls back to the type-default control (text area → text field, slider → numeric input, radio → combo). Omitted when neither a wrapper type nor an override supplies one. |
| `x-min` | property node (sibling of `$ref`) | number | Slider lower bound, from `Ranged::min()`. Emitted only for a `Ranged` field. Distinct from glaze's schema `minimum` (a *validation* bound, when present) — `x-min` is the *control track* start and is never enforced. |
| `x-max` | property node (sibling of `$ref`) | number | Slider upper bound, from `Ranged::max()`. Emitted only for a `Ranged` field. |
| `minimum` | `$def` (glaze's own bound for the member's scalar type), **or** the property node when the field declares `FieldMeta::minimum` | number | Inclusive lower bound the renderer must refuse values below. The property node wins over the `$def` on merge, which is what makes a declared bound per-*field* rather than per-type — see [Per-field scalar bounds](#per-field-scalar-bounds--minimum--maximum--multipleof). For a `Quantity` property it bounds the scalar value in the **canonical** unit, not the `{num,den,dp}` object. |
| `maximum` | as `minimum` | number | Inclusive upper bound, same sources and same reading. |
| `multipleOf` | property node (sibling of `$ref`) | number, strictly positive | The value must be an exact integer multiple of this; `1` means "whole number". Emitted only from `FieldMeta::multipleOf` — glaze never stamps it. A renderer that ignores it loses the client-side gate only; the model still refuses. |
| `x-exactMinimum` | wherever `minimum` sits (property node, or the `$def` reached through its `$ref`) | string | Exact decimal spelling of `minimum`, emitted **only** when the bound's magnitude exceeds 2^53 — i.e. when an IEEE-754 double cannot hold it. See [Exact numeric bounds](#exact-numeric-bounds--x-minimumtext--x-maximumtext). |
| `x-exactMaximum` | wherever `maximum` sits | string | Exact decimal spelling of `maximum`, under the same condition. |
| `x-step` | property node (sibling of `$ref`) | number | Slider / numeric increment, from `Ranged::step()`. Emitted only for a `Ranged` field. For a `Quantity` the entry granularity remains `x-decimalPlaces` (above); `x-step` is not emitted for `Quantity`. |
| `x-minimum` | property node (sibling of `$ref`) | object `{num,den,dp}` | Inclusive lower bound for the field's value, from a model's `InstanceConstraints` — an exact `Rational` in the same wire shape as the value it bounds, never a `double`. Emitted only for a decorated instance schema ([Per-instance constraints](#per-instance-constraints--values-that-live-in-data)); never by `schemaJson<A>()`. Distinct from `x-min` (a *slider track* start, which is never checked). |
| `x-maximum` | property node (sibling of `$ref`) | object `{num,den,dp}` | Inclusive upper bound, same source and shape as `x-minimum`. |
| `x-instanceConstraints` | top-level (object) | array of strings | Wire field names whose keys were written from *instance* data rather than derived from the compiled action type. Present only on a decorated schema. A renderer needing to know whether an `x-decimalPlaces`/`x-minimum`/`x-maximum` is instance-sourced checks membership here rather than guessing. |
| `format` | `Timestamp` property (or its `$def`) | string, value `"date-time"` | Standard JSON-Schema vocabulary (stamped by glaze, not by morph). The renderer shows a date-time input; the wire value is the ISO-8601 string `Timestamp` serialises to. No `x-*` extension is used for timestamps. |
| `ExtUnits` | `$def` of the `Quantity`'s unit type (reached via the property's `$ref`) | object | Glaze-stamped block describing the field's **canonical** unit. Two fields: `unitAscii` (the stable ascii id, e.g. `"kg_per_m3"` — sourced from `UnitMeta::id`) and `unitUnicode` (the human display text, e.g. `"kg/m³"` — from `UnitMeta::display`). This is the unit a payload value is always denominated in, and the reference point the `num`/`den` of every `x-unitAlternatives` entry converts *to*. A renderer resolves the property's `$ref` into `$defs` to read `ExtUnits.unitAscii`/`unitUnicode` (it is **not** on the property node next to the `x-*` keys) to label the field and anchor the unit selector. |
| `x-layout` | top-level (object) | object | The form's group structure: `{ "groups": [ { "title": string, "kind": "section"\|"tab"\|"accordion", "fields": [wire-key,…] }, … ] }`, in `A::formLayout` declaration order. Emitted only when the action declares `formLayout`. The renderer builds the named containers in array order and places each field in its group; fields absent from every group go in a trailing default group. |
| `x-group` | property node (sibling of `$ref`) | string | The title of the group this field belongs to. Omitted for a field in the implicit default group, or when `x-layout` is absent. |
| `x-section` | property node (sibling of `$ref`) | non-negative integer | The 0-based index of this field's group in `x-layout.groups`. Omitted under the same conditions as `x-group`. |
| `x-colspan` | property node (sibling of `$ref`) | positive integer | Number of grid columns the field should span, from `FieldSpan::colspan`. Emitted only when greater than `1` (the default, single-column width). A renderer laying fields out in a grid widens the control; a single-column renderer ignores it. |
| `x-rules` | top-level (object) | array of rule objects | Cross-field rules the renderer must satisfy before enabling submit, and should surface live as inline errors. Emitted only when the action declares `formRules`; absent otherwise. A renderer that ignores it falls back to per-field `required` only. |
| ↳ `kind` | rule / condition object | string | One of the closed vocabulary ids in the "Cross-field rules" section's table above (or a condition id: `engaged`, `notEngaged`, `equals`, `and`, `or`, `not`). An unrecognised `kind` — a rule *or* a nested condition — must be treated as "cannot evaluate": a third answer, distinct from both true and false. The renderer neither claims the rule is satisfied nor blocks submission on it; the payload reaches the server, which runs the compiled rule list and has no unrecognised-kind case. See [Renderer fallback](#renderer-fallback) for the full contract and why it is *defer*, not *block*. |
| ↳ `fields` | rule / condition object | array of strings | Wire field names the rule ranges over, in declaration order (operand order is significant for `greater`/`less`). Absent on `and`/`or`/`not`, which range over nested conditions (`conditions`/`condition` below) instead of fields directly. |
| ↳ `when` | `requiredWhen` / `visibleWhen` / `readonlyWhen` object | rule/condition object | The nested condition the rule keys on. Present only on these condition-bearing kinds. May itself be an `and`/`or`/`not` node (a compound condition), nested to any depth — see [Compound conditions](#compound-conditions--andof--orof--notof). |
| ↳ `value` | `equals` condition object | scalar / `{num,den}` | The literal an `equals` condition compares against; a numeric literal is the exact `Rational` `{num, den}`, never a `double`. A `bool` literal is emitted as a JSON **boolean**, so a renderer must compare a boolean field as a boolean, not as its display text. |
| ↳ `valueText` | `equals` condition object | string | Exact decimal spelling of an **integral** `value` whose magnitude exceeds 2^53, emitted only in that case — the number itself does not survive `JSON.parse`, so comparing it would collapse literals the compiled evaluator keeps distinct. A renderer that evaluates `equals` must prefer `valueText` when present and compare digits. Same remedy as [`x-exactMinimum`/`x-exactMaximum`](#exact-numeric-bounds--x-exactminimum--x-exactmaximum) for bounds. |
| ↳ `conditions` | `and` / `or` condition object | array of condition objects | The nested conditions combined by boolean AND / OR, in declaration order; each element is itself a full condition/rule object (any `kind`, including a nested `and`/`or`/`not`) — see [Compound conditions](#compound-conditions--andof--orof--notof). |
| ↳ `condition` | `not` condition object | condition object | The single nested condition negated by boolean NOT (singular key, since `not` wraps exactly one child). |
| `x-submitMode` | top-level (object) | string | `"explicit"` opts a side-effectful (non-query) action out of the shipped renderer's default auto-submit-on-validity behavior — see [Explicit submit mode](#explicit-submit-mode--x-submitmode). Absent, or any value other than `"explicit"`, keeps the default. Emitted by `schemaJson<A>()` when the action declares `static constexpr bool explicitSubmit = true`, the same way `x-layout` is emitted from `formLayout`. An action that declares nothing — or declares it `false` — emits no key at all. |

### Explicit submit mode — `x-submitMode`

The shipped `DynamicForm.qml` renderer's default behavior is to call
`controller.submitIfValid(actionType, bodyJson)` the instant every field and
rule is satisfied — safe for a read-only query action, but unsafe for any
side-effectful (mutating) action: a mutation would fire on every keystroke
that happens to leave the form momentarily valid, with no user confirmation.

Setting the top-level `"x-submitMode": "explicit"` schema key opts a form out
of that default:

- `revalidate()` still recomputes `ready`/`previewLine` live (so `x-rules`,
  `required`, and every other live-validation affordance are unaffected) but
  never calls `submitIfValid` on its own.
- The renderer instead shows an explicit **Submit** button (`objectName:
  "submitButton"`), enabled only while `ready` — matching the existing
  `x-order`/required-asterisk convention of gating on the same readiness
  state the auto-submit label already reflected. Clicking it is the sole
  trigger; `DynamicForm.submit()` is the function it calls, itself a no-op
  unless the form is currently ready.
- The button is loaded (via a `Loader`, `active: explicitSubmitMode`) only
  when the schema opts in — a default (auto-submit) form has no such control
  anywhere in its item tree, not merely a hidden one.

Any schema describing a side-effectful action should carry this flag before
being safely rendered by the shipped renderer; a schema that omits it (every
existing schema, and any read-only query action) renders exactly as before —
zero behavior change.

#### Declaring it from C++

An action opts in with a `static constexpr bool`:

```cpp
struct CreatePaste {
    std::string title;
    std::string body;

    // Without this, the shipped renderer auto-submits on validity — which for
    // a mutation means one stored paste per typed character.
    static constexpr bool explicitSubmit = true;
};
```

`schemaJson<A>()` then emits the top-level `"x-submitMode": "explicit"`. This
is **opt-in**, exactly like `formLayout`, `fieldSpans`, and `formRules`: an
action that says nothing keeps the auto-submit default and its generated schema
is byte-for-byte unchanged, so adding the emitter changed no shipped schema.
Declaring `explicitSubmit = false` is the same statement as not declaring it.

The detection is the `HasExplicitSubmit<A>` concept — `true` when `A` declares
an `explicitSubmit` member convertible to `bool`, the same shape as
`HasFormRules<A>`. It answers only *whether the member is declared*; the
emitter reads its value afterwards, which is why `= false` and declaring
nothing produce the same schema.

Deriving the flag instead — from some "does this action mutate" predicate — is
deliberately **not** done. No such predicate exists in `forms.hpp`, and adding
one would flip the rendering of every existing generated form at once, which is
a shipped-behavior change rather than an emitter addition. Opting in per action
keeps the decision with the author who knows whether the action has effects.

### Array fields — `type: "array"`

glaze emits `{"type": "array", "items": {...}}` for a `std::vector<T>`
member, standard JSON-Schema vocabulary rather than an `x-*` extension. The
shipped `DynamicForm.qml` renderer gives it a dedicated
comma-separated-with-validation `TextField` control (`objectName: "field_" +
name`, exactly like a scalar field's control — the two are mutually
exclusive per field, so exactly one claims that name) instead of falling
through to the plain-text control, whose fallback (`JSON.stringify(text)`)
would wrap the typed text as a JSON *string*, not an array — a body the
server's schema validation always rejects.

Typed text is split on comma, each entry trimmed of surrounding whitespace,
and empty entries dropped: `"red, green, blue"` → `["red","green","blue"]`,
`"  red ,, green ,"` → `["red","green"]`. A field with today's scope —
array-of-string — is fully supported; an `items` type other than `"string"`
still renders this control and still encodes each comma-separated entry as a
JSON string (not, e.g., a JSON number), so a `std::vector<int>` field is
usable but not yet type-checked per element the way a scalar `Quantity`/
integer field is. The submitted literal for a **fully-blank** array field
follows the same blank-means-unengaged convention as every other field
(`fieldJsonLiteral` returns `null` for empty/whitespace-only text), so an
optional, untouched array field is omitted from the request body entirely
rather than submitted as `[]`. Once the field holds *any* non-whitespace
text, though — including a comma-only entry like `" , , "`, which is not
blank by that check even though every individual entry is dropped — it
encodes to a genuine empty array `[]`, not `null`; a `required` array field
is satisfied by engagement (non-blank text), not by having at least one
surviving entry.

### Boolean fields — `type: "boolean"`

glaze emits `{"type": "boolean"}` for a `bool` member, and
`{"type": ["boolean", "null"]}` for a `std::optional<bool>`. The shipped
`DynamicForm.qml` renderer gives both a `CheckBox` (`objectName: "field_" +
name`, mutually exclusive per field with the scalar and array controls, so
exactly one claims that name) rather than letting them fall through to the
plain-text control. The fallback there (`JSON.stringify(text)`) wrapped the
value as a JSON *string* — `{"flag":"true"}` — and, because a `TextField`
applies no validation of its own, accepted literally any text, so
`{"flag":"banana"}` was submitted just as readily. glaze rejects both with
`expected_true_or_false`; it does not coerce.

The control emits a bare `true` or `false`, never quoted. A `bool` member is
**required** (it has no null branch), and a checkbox always displays a definite
state, so a required boolean with no retained value is seeded `false` at
delegate creation rather than left blank — otherwise the form would show an
unchecked box while the required-field gate silently withheld submission, with
nothing on screen indicating what was missing. An *optional* boolean is left
unseeded and is omitted from the request body until the user touches it, which
is what distinguishes "not answered" from an explicit `false` for a
`std::optional<bool>` member.

### Nullable fields whose type is a `$ref` — `anyOf`

A nullable member whose underlying type is emitted as a definition rather than
inline — `std::optional<std::int64_t>`, or a `std::optional<T>` over a strong id
— produces neither a `type` key nor a top-level `$ref`:

```json
"optI64": {"anyOf": [{"$ref": "#/$defs/int64_t"}, {"type": "null"}]}
```

A renderer that resolves only a *top-level* `$ref` sees no type at all here, so
every field-kind flag is false and the value takes the plain-text path — a
quoted string the server rejects with `parse_number_failure`. `DynamicForm.qml`
therefore resolves through `anyOf` (and through `oneOf`, which takes the same
shape when a hand-written or evolved schema spells nullability that way): it
takes the first branch whose type is not `"null"`, follows a `$ref` inside it,
and merges the result under the property's own keys, so the field is typed by
`T` and picks up `T`'s constraints such as `minimum`/`maximum`. Integers on this
path are emitted as bare, exact numbers — the payload is assembled as JSON
*text*, never round-tripped through `JSON.parse`, so values beyond 2^53
(including `INT64_MAX`) survive intact.

This collapse applies **only** to branches that differ in nullability. A
`oneOf`/`anyOf` whose branches differ in *value* is a closed set, not a nullable
type, and is described next — collapsing one to its first branch would both
discard the alternatives and leave that branch's `const` masquerading as the
field's own pinned value.

### Closed sets — a reflected `enum class`

A C++ `enum class` member that declares a `glz::meta` with `glz::enumerate` is
fully described by the schema. That declaration is the same one that makes the
enum travel as its enumerator **name** rather than as its underlying integer, so
in practice every enum a form can meaningfully render carries it. glaze emits
such a member as a `oneOf` of `const` alternatives, each carrying its own
`title` (the enumerator name, which is also the name its reader accepts):

```json
"role": {"type": "string",
         "oneOf": [{"title": "Viewer",  "const": "Viewer"},
                   {"title": "Member",  "const": "Member"},
                   {"title": "Manager", "const": "Manager"}],
         "x-order": 2, "title": "Role"}
```

This is standard JSON-Schema vocabulary, not an `x-*` extension: no morph key
declares it and none is needed. A renderer recognises the shape by the property
holding a `oneOf`/`anyOf` in which **every** branch bar `{"type": "null"}`
carries a `const`. One branch without a `const` and it is not a closed set —
that is the nullability shape above, and a partial list would be worse than no
list at all. The bare JSON-Schema `enum` keyword (`{"enum": ["a", "b"]}`), which
glaze does not emit but a hand-written schema may, states the same thing and is
read the same way.

Two obligations follow, and `DynamicForm.qml` meets both:

- **Draw a selection control**, not a text field. The alternatives' `title`s are
  already the human labels, and the set is closed, so this is the same combo box
  a [`Choice`](#choice--server-sourced-picklist) draws — the only difference is
  that the options are in the schema rather than behind `x-optionsAction`, so
  there is no options fetch and no round trip. A `null` branch is not offered as
  a choosable value: leaving the field blank is how an optional member is
  declined.
- **Refuse a value outside the set.** Membership is decidable on the client
  here — the schema states the whole set — so a value outside it must leave the
  form not ready, exactly as a `boolean` field refuses anything but `true`/
  `false`. This is what distinguishes a closed set from a `Choice`, whose option
  list is a server snapshot that may already be stale and whose membership
  verdict therefore belongs to the server (see
  [choice.md](choice.md), "Validation & staleness").

The value on the wire is the enumerator name as a JSON string — `"role":"Manager"`
— which is what glaze's enum reader accepts; it refuses any other name with a
parse error, so a renderer that submitted free text was relying on the server to
say what the client already knew.

An enum **without** a `glz::meta` is refused at compile time. Without the
declaration, glaze would emit a `$ref` to a `$defs` entry that is the six-way
wildcard `{"type": ["number", "string", "boolean", "object", "array", "null"]}`,
naming neither the enumerators nor even a single type — the shipped
`DynamicForm` drew that wildcard as a checkbox, reporting the form ready for a
value nobody chose (morph#392). `schemaJson<A>()` now `static_assert`s on
`glz::glaze_enum_t` for every `enum class` member it reaches, so a rung that
declares one without `glz::meta`/`glz::enumerate` fails to build rather than
shipping a form that lies about being ready.
### Exact numeric bounds — `x-exactMinimum` / `x-exactMaximum`

`minimum` and `maximum` are standard JSON-Schema vocabulary, stamped by glaze.
They are JSON *numbers*, and a renderer reaches them by parsing the schema —
every shipped app does `JSON.parse(controller.schemasJson)`. JavaScript numbers
are IEEE-754 doubles, so any bound above 2^53 loses precision at that moment:

```
schema maximum for an int64_t field: 9223372036854775807
       after JSON.parse into a JS number: 9223372036854775808   (rounded up)
```

That breaks the client-side gate at exactly the value it is closest to failing
on. `INT64_MAX + 1` compared against a maximum rounded *up* to
`9223372036854775808` is judged **equal, not greater**, so the renderer's own
validation admits an out-of-range value. Nothing is corrupted — the payload is
assembled as JSON text and keeps the exact digits, and the server rejects it
with `parse_number_failure` — but the client claimed a value was valid that
never was.

`schemaJson<A>()` therefore also emits the bound as an exact decimal **string**,
which `JSON.parse` cannot round. A renderer that validates integer input should
prefer `x-exactMinimum`/`x-exactMaximum` when present and fall back to the numeric
`minimum`/`maximum` otherwise. The shipped `DynamicForm.qml` compares digits
directly in that case, since no JS number can hold the bound.

Two deliberate limits:

- **Emitted only above 2^53.** An ordinary bound (`int32_t`, a `Ranged` slider,
  a hand-written `maximum: 10`) loses nothing to a double, so its schema is
  byte-for-byte what it was before this key existed. Only the definitions that
  genuinely need it — `$defs/int64_t`, `$defs/uint64_t` — carry the companion.
- **The numeric bound stays.** The companion is additive: `minimum`/`maximum`
  remain exactly as glaze emitted them, so a renderer that ignores the new keys
  behaves precisely as it did before, per the versioning stance below.

Note the companion sits **wherever the bound sits**. For a `std::int64_t`
member that is the `$defs` entry the property's `$ref` points at, not the
property node — a renderer reads it from the merged node after resolving the
`$ref` (or the non-null `anyOf` branch), the same way it reads `type`.

### Versioning stance

The emitted schema is **unversioned**. There is no `$id`, `$schema` version
marker, or morph-specific version field anywhere in the output — a renderer
cannot detect at runtime which revision of this vocabulary a schema was produced
against. The vocabulary is therefore treated as a stable framework contract:
**changing the semantics of any key above (renaming it, changing its type, or
altering how a value is interpreted) is a breaking change and ships only in a
breaking framework release.** Adding a new, optional `x-*` key that older
renderers can safely ignore is not breaking.

## Shipped Qt/QML reference renderer

The schema contract above is renderer-agnostic; morph ships one reference
renderer for it, Qt/QML, as a reusable component rather than example code.

- **`src/qt/forms`** builds the QML module `MorphForms` (CMake target
  `morph_forms_module`, `qt_add_qml_module(... URI MorphForms VERSION 1.0)`):
  `DynamicForm.qml` (the `Repeater`-over-`fields` form renderer: `$ref`
  and `anyOf` resolution/dual-read, the closed-set selection control (see
  [Closed sets](#closed-sets--a-reflected-enum-class)),
  the exact rational digit arithmetic, the unit
  selector, the required-field submit gate, the options-fetch, layout/
  grouping into sections/tabs, the widget-hint controls — textarea, slider,
  radio group — the comma-separated-with-validation `"array"`-typed field
  control (see [Array fields](#array-fields--type-array)), the explicit
  submit mode (see [Explicit submit mode](#explicit-submit-mode--x-submitmode)),
  and the localisation dual-read), `DateTimePicker.qml` (manual
  ISO-8601 entry plus a calendar/time popup), `SlotRegistry.qml` (below), and
  `I18nCatalog.hpp`/`.cpp` (a `QObject`/`QML_ELEMENT` in-memory
  `TranslationProvider` realization — see
  [Localisation](#localisation--message-keys-and-the-catalog-seam) — shipped
  alongside the renderer rather than left in a demo, since it is
  model-agnostic and `DynamicForm`'s `catalog` property consumes it
  structurally, not by name). It builds whenever `-DMORPH_BUILD_FORMS_QML=ON`,
  independent of `MORPH_BUILD_EXAMPLES` — an app depends on it directly
  (`target_link_libraries(... morph_forms_moduleplugin)` plus `import
  MorphForms` in its own QML) instead of copying or forking it.
- **`include/morph/qt/forms/forms_controller_core.hpp`** ships
  `morph::qt::forms::FormsControllerCore<Model>`, a header-only, model-agnostic
  template (no `Q_OBJECT` — Qt cannot register a class *template* for QML) that
  owns or composes over the `Bridge`/`BridgeHandler<Model>`/`QtExecutor`
  wiring an app's own `QObject`/`QML_ELEMENT` controller subclass forwards to.
  Two constructors decide who owns the `Bridge`:
  - `FormsControllerCore(schemasJson)` builds and owns a private
    `ThreadPoolExecutor` + `QtExecutor` + `Bridge` over a `LocalBackend` —
    the convenient default for a demo or an app with no `Bridge` of its own.
  - `FormsControllerCore(Bridge& bridge, IExecutor* guiExec, schemasJson)`
    composes over a caller-supplied `Bridge`/executor instead of building a
    second, always-local one — the caller decides the deployment mode
    (`LocalBackend`, `SimulatedRemoteBackend`, `QtWebSocketBackend`, ...), and
    a later `bridge.switchBackend(...)` on that same `Bridge` is still
    reachable through this core's handler (the handler re-registers itself
    automatically, exactly like any other `BridgeHandler`). `bridge` and
    `guiExec` must outlive the core.

  It exposes `schemasJson()`, `submitIfValid(actionType, bodyJson, onReply,
  onError)`, and `fetchOptions(optionsAction, bodyJson, onReply, onError)` —
  both operations dispatch generically via `BridgeHandler::executeJson`, so an
  app's controller never hardcodes one action, and `fetchOptions`'s `bodyJson`
  is a true pass-through (`"{}"` for an independent `Choice`, or
  `{parentField: value, ...}` for a dependent one — see [Choice —
  server-sourced picklist](#choice--server-sourced-picklist)) rather than
  always empty; `examples/forms/gui_qml/FormsController.hpp` is the ~20-line
  reference wrapper (naming its own model type, since Qt cannot register the
  template itself), still using the owning constructor since the demo has no
  pre-existing `Bridge` to compose over.
- **`examples/forms/gui_qml`** is a *consumer* of the shipped module, not its
  home: its own `LabFormsDemo` QML module carries only `Main.qml` and the
  `FormsController` subclass naming `lab::LabModel`; `Main.qml` imports
  `MorphForms` for `DynamicForm`/`I18nCatalog` like any other consumer would.

This is packaging and factoring only: no `x-*` key changed, and a plain
single-action form renders identically to before the renderer was extracted.

`DynamicForm` connects to the controller through **two** `Connections` blocks,
not one, because only one of the two signals is universal:

- **`replyReceived(actionType, ok, payload)` is required** of every controller.
  Its block is strict, so a handler there that matches no signal on the target
  is a misspelling and the engine reports it.
- **`optionsReceived(optionsAction, ok, payload)` is optional.** It exists only
  on a controller that serves a `Choice` field; a controller that serves none
  deliberately declares neither it nor `fetchOptions()`
  (`bookmarks::gui::BookmarkFormsController` and
  `pastebin::gui::PasteFormsController` each carry the reasoning: an unused
  `fetchOptions()` would be a stub with nothing to call it). Its block gates its
  **target** on the signal being declared — `form.controller.optionsReceived
  !== undefined`, else `null` — so a controller that omits it is never connected
  to and the absence is not a warning. Without the split, every form instance
  warned once about `onOptionsReceived` as soon as a conforming choiceless
  controller was attached (morph#387), which forced any GUI test asserting "no
  QML warnings" to tolerate that exact text.

  The gate is what makes the block optional, **not** `ignoreUnknownSignals`. A
  controller that does declare `optionsReceived` is connected to strictly, so a
  misspelling of the handler is still reported. `ignoreUnknownSignals: true`
  would silence that too, and the silence is expensive: the options never
  arrive, every `Choice` combo box stays empty, the form never reaches `ready`,
  and nothing is logged.

`src/qt/forms/tests/tst_DynamicFormChoicelessController.qml` pins both halves:
a choiceless controller loads with no warning, a `Choice`-serving one still
receives its options, and a target missing `replyReceived` is still reported.

`DynamicForm.schema` takes the parsed schema **however it is supplied** — a
declarative QML binding (`schema: controller.schemas[actionType]`), an initial
property, a `setProperty` from C++, or `createTemporaryObject(component,
parent, {schema: ...})`. An assigned value round-trips through `QVariant`,
which turns each of the schema's arrays into a `QVariantList` rather than a JS
array; the renderer re-reads the schema as plain JSON once, at the property, so
an array-valued `type` (`["integer","null"]`), the `anyOf`-over-`$ref` collapse
and closed-set recognition all read the same either way. The same schema
supplied both ways yields the same field descriptors and the same submitted
body — asserted in `src/qt/forms/tests/tst_DynamicFormSchemaAsVariant.qml`,
which builds one form each way and compares them against each other.

The one thing that does **not** survive the `QVariant` boundary is JSON key
order: the map it converts through is sorted, and the declaration order is
gone before the renderer is reached, so no renderer can recover it. This is
the general rule `x-order` already exists for — "renderers lay fields out in
ascending `x-order`, not in JSON key order", above — and `schemaJson<A>()`
emits `x-order` on every property, so a generated schema is unaffected. A
**hand-written** schema that omits `x-order` leaves its fields tied, and the
sort that orders them is `Array.prototype.sort`, which QML's engine does not
guarantee to be stable: measured on Qt 6.11.2, four all-equal elements come
back reordered. So a tied schema lays out in an order that is neither
declaration order nor key order, bound or assigned. Give every property an
`x-order`.

One other value JSON cannot carry: a **non-finite** `minimum`/`maximum`. Only a
hand-authored QML object literal can declare one — `schemaJson<A>()` never
emits it, and no JSON text can spell it — and the re-read turns it into `null`.
The renderer reads a bound that is not a finite number as **no bound declared**,
which is what `maximum: Infinity` already meant, and matches JSON Schema giving
a null numeric keyword no meaning. Without that, a null bound would read as the
bound `0` and reject every positive value.

## Renderer conformance kit

A renderer proves it honors the contract above by consuming a **schema
corpus** and satisfying a set of **expected-behavior assertions** — the
executable form of this document's "normative" claim.

- **C++ fixture corpus and drift guard**
  (`tests/test_forms_conformance_corpus.cpp`): five fixture action types —
  plain scalars + `required` (`CFScalarsAndRequired`), a `Quantity` with
  convertible alternatives (`CFQuantityAlternatives`), a `Choice`
  (`CFChoiceField`), a `Timestamp` (`CFTimestampField`), and two members of the
  same `Quantity` type sharing one `$def` (`CFSharedDefFields`) — each
  asserted against the **real**, generated `schemaJson<A>()` output (never
  hand-authored), so a change to `mergeSchemaExtras`/`schemaJson` that alters
  `x-order`, `required`, `x-decimalPlaces`, `x-unitAlternatives`,
  `x-optionsAction`/`x-optionValue`/`x-optionLabel`, `format`, or `ExtUnits`
  is caught here as a failing assertion (the corpus "drift guard").
- **QML functional assertions** (`src/qt/forms/tests/tst_conformance.qml`)
  hand-author schemas mirroring each C++ fixture by name and run them through
  the shipped `DynamicForm`: fields render in `x-order`; submission is blocked
  until every `required` field is engaged and enabled once they are; a
  `Quantity` payload is `{num,den,dp}` exact and a unit switch recomputes it
  exactly (no float drift); a `Choice` descriptor carries its declared
  `x-optionsAction`/`x-optionValue`/`x-optionLabel`; a `Timestamp` renders as a
  date-time control and gates on ISO-8601; two properties sharing one `$def`
  each keep their own `x-order` while resolving the same `ExtUnits`. The
  options-fetch itself — an independent `Choice` executing its options action
  with an empty body, and a dependent one (`x-optionsDependsOn`) with
  `{parentField: value, ...}` — is asserted separately, in
  `src/qt/forms/tests/test_forms_controller_core.cpp` (a Catch2 + Qt
  executable covering `FormsControllerCore<Model>` directly), since
  `DynamicForm` never calls the options action directly — its controller
  does.
- **Accessibility slice** (`src/qt/forms/tests/tst_conformance_accessibility.qml`):
  every control exposes an accessible name (the wire key — `title` from
  [Field metadata](#field-metadata--fieldmeta), when declared, is the visible
  label but the accessible-name fallback is always the wire key), a required field's
  accessible description announces it, focus order follows `x-order`, and
  every control (choice combo, radio group, date/time picker, text field,
  multiline text area, slider, unit selector, and the calendar popup, which
  gained arrow-key day navigation plus Enter/Escape for exactly this) is
  keyboard-operable.
- **Negative assertions** (`src/qt/forms/tests/tst_conformance_negative.qml`,
  with test-only doubles `BrokenOrderForm.qml`/`BrokenQuantityForm.qml`, never
  shipped in the `MorphForms` module): a renderer that ignores `x-order` fails
  exactly the field-order assertion and no others; a renderer that silently
  rounds an over-precise `Quantity` entry instead of rejecting it fails
  exactly the exact-payload assertion and no others — proving the kit's
  assertions are specific, not all-or-nothing.

**Scope note.** The corpus above covers exactly the keys this document's
renderer contract currently defines, plus the `x-widget`/`SlotRegistry` keys
below — informally, "**Tier-1**": the per-action `x-*` schema vocabulary this
document specifies. Two numeric-bound families sit in the contract table but
outside the five-fixture corpus, each carrying its own matched C++/QML pair
instead: `x-exactMinimum`/`x-exactMaximum`
(`tests/test_forms_exact_bounds.cpp` + `tst_DynamicFormExactBounds.qml`) and
the declared `minimum`/`maximum`/`multipleOf`
(`tests/test_forms_field_bounds.cpp` + `tst_DynamicFormFieldBounds.qml`). Both
are additive per-field keys that no corpus fixture declares, so adding one
changes no fixture's generated schema — which is why the drift guard has
nothing to say about them and a dedicated pair does. It does **not** include a wizard/app-shell fixture
(`w-*`/`app-*`): although the emitters for those "**Tier-2**" keys (the
wizard/app-shell layer built atop Tier-1, one level up the composition —
[workflows_navigation.md](workflows_navigation.md)) now exist
(`morph::flows::wizardSchemaJson`/`morph::app::appSchemaJson`, see
[workflows_navigation.md](workflows_navigation.md)), no conformance-kit
fixture exercises them yet — that coverage is deferred to future work,
exactly as this corpus already treats views (below) separately rather than
as a sixth `CF*` fixture. The `v-*` view-schema layer
(`morph::views::viewSchemaJson`, [views.md](views.md)) **is** implemented;
its own renderer-behavior coverage lives in
`src/qt/forms/tests/tst_collectionview.qml` rather than this five-fixture
corpus (a view composes existing action schemas rather than introducing new
per-field schema keys, so it does not need a sixth `CF*` fixture type here).

## Theming / component-override registry

A field's control is chosen by the renderer's built-in logic (`isChoice` → combo
or radio group, `isQuantity` → number + unit selector, `format: date-time` →
date/time picker, `x-widget: "textarea"`/`"slider"` → multiline/ranged
controls). An app that wants a different control for one field, one unit, one
`x-widget`, or one JSON type does so through a client-side registry, without
forking the renderer:

- **`x-widget` (optional property-level key).** A hint naming a control
  variant when the type alone is ambiguous — e.g. `"textarea"`, `"slider"`,
  `"radio"` (already dispatched on by the renderer's own widget-hint controls,
  see [Widget hints](#widget-hints--multiline--ranged)), or an app-defined id
  such as `"slider"`/`"rating"` a registered `SlotRegistry` slot recognises.
  It is read with the same dual-read as every other property-level key
  (`opt(raw["x-widget"], p["x-widget"])`). Absent, it resolves to `""` and
  never matches `SlotRegistry`'s `byWidget` tier — purely additive and
  ignorable.
- **`SlotRegistry` (QML type, module `MorphForms`, entirely client-side).** A
  lookup a host app populates at startup: `byField(action, field, component)`,
  `byWidget(xWidget, component)`, `byUnit(unitAscii, component)`,
  `byType(jsonType, component)`, and `resolve(action, field, xWidget,
  unitAscii, jsonType)`, which returns the highest-priority match or `null`.
  Resolution order is **field → `x-widget` → unit → type → built-in default**.
  `DynamicForm` gains a `slotRegistry` property (`null` by default — no
  behavior change for an app that never sets it); when a field resolves to a
  registered `Component`, `DynamicForm` loads it via a `Loader` and hides its
  own built-in control for that field (every built-in control — combo, radio
  group, date/time picker, text field, text area, slider, unit selector — is
  gated on the `Loader`'s `sourceComponent` being `null`). A registered slot
  `Component` implements one small contract: it declares `property var field`
  and `property var setValue`, both assigned by `DynamicForm`'s
  `Loader.onLoaded` — `field` is the resolved, merged def+property descriptor,
  and `setValue(text)` is the same set-value path (`setFieldValue`) the
  built-in controls use, so an override participates in the required-gate and
  auto-fire without special-casing. `SlotRegistry.revision` is bumped on every
  `by*()` call and read inside `resolve()`, for the same reason
  `I18nCatalog.revision` exists: `_byField`/`_byWidget`/`_byUnit`/`_byType` are
  plain objects mutated in place, which does not by itself notify a binding
  that already read them.

The registry never appears in the schema or on the wire — two renderers of the
same schema may register different slots. This is the "escape hatch always
available" design principle ([above](#design-principle-infer-by-default-declare-to-override))
in practice: swap one control without forking the renderer.

## Localisation — message keys and the catalog seam

The schema stays one cached, un-localised instance per type (see
[One cached schema per type — no localisation](#one-cached-schema-per-type--no-localisation)):
translation is a renderer-side catalog lookup over **stable, mechanically
derived message keys**, never a per-locale schema variant. Two small
header-only libraries carry this:

- **`morph::forms::i18n`** (`include/morph/forms/i18n.hpp`) — the key
  derivation vocabulary.
- **`morph::render`** (`include/morph/render/i18n.hpp`,
  `include/morph/render/locale_format.hpp`) — the renderer-side catalog seam
  and the locale numeric-entry contract. `morph::render` is client-side only
  and never appears on the wire.

### Message-key derivation

A key is derived from identifiers the schema (or the `actionType` label a
renderer already has) already carries — no declaration needed in the common
case:

| Text slot | Derived key | Function |
|---|---|---|
| field label / help / placeholder | `<actionTypeId>.<wireField>.label` / `.help` / `.placeholder` | `morph::forms::i18n::fieldKey(actionTypeId, wireField, FieldSlot)` |
| layout group title | `<actionTypeId>.group.<index>` | `groupKey(actionTypeId, groupIndex)` |
| cross-field rule message | `<actionTypeId>.rule.<index>` | `ruleKey(actionTypeId, ruleIndex)` |
| wizard title / step title | `<wizardId>.title` / `<wizardId>.step.<index>.title` | `wizardTitleKey(wizardId)` / `wizardStepTitleKey(wizardId, stepIndex)` |
| app title / menu label | `<appId>.title` / `<appId>.menu.<index>.label` | `appTitleKey(appId)` / `appMenuLabelKey(appId, menuIndex)` |

`actionTypeId` is `ActionTraits<A>::typeId()`; `wireField` is the member's
reflected wire key (the same name `mergeSchemaExtras` iterates via
`forEachNamedMember`); group/rule/step/menu indexes are the 0-based position
in their respective schema arrays.

The field key is assembled from three named pieces rather than formatted in
one place, and a renderer reproducing this scheme in another language needs
all three: `fieldKeyStem(actionTypeId, wireField)` builds the
`<actionTypeId>.<wireField>` stem, `fieldSlotName(FieldSlot)` spells the slot
suffix (`"label"`, `"help"`, `"placeholder"`), and `withSlot(stem, slot)`
joins them with a `.`. `fieldKey()` is `withSlot(fieldKeyStem(...), slot)` and
`explicitFieldKey()` is `withSlot(i18nKeyOverride, slot)` — which is why an
override replaces the stem and nothing else. None of these keys are written into the
schema — a renderer derives them itself from data it already has (the schema
plus the `actionType` label it is rendering under), so declaring nothing
changes zero bytes of any schema.

**Declare to override.** A field declares an explicit key stem via
`FieldMeta::i18nKey` (see [Field metadata](#field-metadata--fieldmeta)),
emitted as `x-i18nKey` on its schema node; group, rule, wizard step, and menu
descriptors gain the same optional `i18nKey` member on their own types, owned
by each descriptor's own spec. For a **field**, the override replaces only
the `<actionTypeId>.<wireField>` stem; each of the three per-field suffixes
(`.label` / `.help` / `.placeholder`) still applies on top of it —
`morph::forms::i18n::explicitFieldKey(i18nKeyOverride, slot)` computes
`"<i18nKeyOverride>.<slot>"`. For a group, rule, wizard step, or menu entry —
each of which carries exactly one piece of text — the override *is* the
complete key, used in place of the derived one.

### The catalog seam

```cpp
// namespace morph::render — client-side only; never on the wire.
using TranslationProvider =
    std::function<std::optional<std::string>(std::string_view key, std::string_view bcp47Locale)>;
```

`morph::render::resolveText(provider, bcp47Locale, explicitKey, derivedKey,
schemaLiteral)` resolves one display slot's text, most specific first: the
explicit key (when declared) is tried first, then the derived key, and a
miss at both falls back to `schemaLiteral` — the schema's authored `title` /
`description` / `x-placeholder` / group or step title, unchanged. A
default-constructed (empty) `provider` — no catalog installed — skips
straight to `schemaLiteral`, so an unconfigured renderer behaves exactly as
it did before this spec. morph ships the seam and this resolution algorithm
only; it defines no translation storage format — a host adapts whatever
catalog it already owns (Qt `QTranslator`/`.qm`, a JSON bundle, a database)
into the one `TranslationProvider` signature.

The `examples/forms/gui_qml` reference renderer hosts a concrete, minimal
realization: `I18nCatalog` (`examples/forms/gui_qml/I18nCatalog.hpp`), an
in-memory `QObject` catalog (QML cannot hold a `std::function` directly),
wired into `DynamicForm.qml`'s `resolveText`/`i18nFieldKey` JS mirrors of the
functions above. It currently resolves only the field label/help/placeholder
slot — group-title i18n wiring for the already-implemented
[Layout & grouping](#layout--grouping--sections-tabs-spans) feature remains
future work. The wizard/app-shell layer
([workflows_navigation.md](workflows_navigation.md)) is implemented, but its
QML renderer (`WizardView.qml`/`AppShell.qml`) does not yet accept an
`I18nCatalog` either, matching `CollectionView.qml`'s own gap (see
[views.md](views.md), "Limitations") — wizard/app-menu i18n wiring remains
future work. Cross-field rules (above) are implemented
but carry no translatable message text of their own — the `x-rules`
vocabulary is structural (`kind`/`fields`/`when`/`value`) only, so a renderer
builds any rule-violation message from that structure (or its own catalog
entry, per "Rule messages come from the catalog, not the wire" below), never
from a wire string.

**Group membership is matched by index, never by translated text.** A
field's `x-section` is the stable numeric handle into `x-layout.groups`; a
renderer translates a group's *displayed* title but places fields by index.

**Rule messages come from the catalog, not the wire.** For a rule the client
can evaluate, the renderer shows its catalog message (falling back to a
renderer-built neutral message from the rule's structure); canonical
server-side error strings ([error_handling.md](../error_handling.md)) stay
untranslated protocol vocabulary, surfaced only for conditions the client
could not pre-empt.

### Locale data formatting

Display formatting is the renderer's duty; the wire stays canonical:

- **Numbers.** `morph::render::normalizeLocaleNumber(text, loc)`
  (`include/morph/render/locale_format.hpp`) converts a locale-formatted entry
  (`"1.050,25"`) to the canonical `.`-decimal text `Quantity`'s exact digit
  routines already consume (`"1050.25"`); malformed input yields `std::nullopt`
  rather than a best-effort guess. `formatCanonicalNumber(canonicalText, loc)`
  is the display-direction inverse, with display-only thousands grouping. The
  exact `Rational`/`Quantity` digit arithmetic
  ([rational.md](../util/rational.md)) never sees a locale-formatted string
  — the conversion happens at the control edge only.

  **The locale facts travel as one aggregate, not as a row of positional
  views** (morph#591):

  ```cpp
  struct NumericLocale {
      std::string_view decimalSeparator = ".";
      std::string_view groupSeparator   = "";
      std::string_view negativeSign     = "-";
      std::string_view positiveSign     = "+";
      std::string_view zeroDigit        = "0";
  };
  ```

  Both edges take it, and a call site names each fact with a designated
  initialiser: `normalizeLocaleNumber(text, {.decimalSeparator = ",",
  .groupSeparator = "."})`. Three things this is for, and the first is not
  tidiness. The five facts were five adjacent `std::string_view` parameters,
  every one silently swappable with its neighbours, and the header carried a
  clang-tidy suppression for `bugprone-easily-swappable-parameters` — on each
  of the two functions and on the sign helper — with a paragraph of
  justification each. A sixth would have made that argument weaker, not
  stronger; the aggregate deleted all three suppressions instead, and the header
  is clean under `bugprone-*` with no suppression of that check anywhere in it.
  Second, the next locale fact (a percent sign, an exponent separator) is a new
  defaulted member rather than a seventh parameter. Third, and the reason it is
  worth the churn: **the two edges take the same type**, so "these two must
  agree" is structural rather than a convention a caller can get half right —
  which is the drift morph#591 and morph#599 both came from. There is
  deliberately no back-compatible positional overload: two spellings of one call
  is how the edges drifted apart to begin with. The QML mirror takes the
  parallel shape, an object literal with the same member names, so the two
  mirrors stay structurally identical.

  Every member is defaulted to its `"C"`-locale spelling, so a caller that names
  none of them gets the identity transform in both directions.

  **The digits are locale data too, carried as a base** (morph#591). A Unicode
  decimal digit set *is* ten contiguous code points — UAX #44 assigns `Nd`
  with `Numeric_Value` 0 through 9 in code point order — so a single
  `zeroDigit` is sufficient and a ten-element table is not needed. Measured with
  `QLocale::matchingLocales` under Qt 6.11.2, over the same 711 locales:

  | `zeroDigit` | locales | e.g. |
  | --- | ---: | --- |
  | U+0030 | 604 | `C` |
  | U+0660 | 26 | `ar_BH` |
  | U+06F0 | 19 | `fa_IR` |
  | U+1E950 | 12 | `ff_Adlm_BF` |
  | U+0966 | 8 | `bgc_IN` |
  | U+09E6 | 4 | `as_IN` |
  | U+11136 | 2 | `ccp_BD` |
  | U+07C0 | 1 | `nqo_GN` |
  | U+0F20 | 1 | `dz_BT` |
  | U+1040 | 1 | `my_MM` |
  | U+1C50 | 1 | `sat_IN` |
  | U+ABF0 | 1 | `mni_IN` |

  76 of 711, across eleven distinct sets. Two of them (Chakma U+11136, Adlam
  U+1E950) are outside the BMP, so one digit is four UTF-8 bytes on the C++
  edge and two UTF-16 units on the QML edge — both scans therefore decode a
  code *point* rather than comparing a unit. The figure the issue could defend
  before this was 24, and it said so plainly: 24 was a count of locales whose
  *negative sign* is `U+061C U+002D`, not a digit-set count. 76 is the measured
  one.

  Before this, `normalizeLocaleNumber` compared one byte against `['0','9']`,
  so a user of any of those 76 locales could not enter a number at all — a flat
  rejection, not a wrong value.

  **Both edges move or neither does, and the round trip is what says so.**
  `formatCanonicalNumber` used to copy the canonical ASCII digits out unchanged.
  That is *why* the pair was self-consistent and the defect invisible from
  either side alone: display emitted the locale's separators and sign around
  ASCII digits, and entry accepted exactly that. Teaching entry to accept
  U+0665 while display kept emitting `'5'` satisfies a naive reading of the bug
  report and breaks the round trip this section requires. So the display edge
  emits the digit that far above `zeroDigit`, and the property is stated in
  those terms:

  > for every canonical `-?[0-9]+(\.[0-9]+)?` text and every `NumericLocale`,
  > `normalizeLocaleNumber(formatCanonicalNumber(canonical, loc), loc) ==
  > canonical`, byte for byte, with every digit of the display text drawn from
  > `[loc.zeroDigit, loc.zeroDigit + 9]`.

  The second half of that sentence is load-bearing. The round trip *alone* does
  not fail if only the entry edge moved, because entry accepts ASCII digits too
  — so the test that pins this asserts the display text contains no ASCII
  digit as well as asserting the round trip, and it is that assertion which
  fails for the half-fix. Pinned over all eleven digit sets on both edges:
  `[morph591]` in `tests/test_render_locale_format.cpp`, and the
  `test_thePairRoundTripsThroughEveryMeasuredDigitSet` function in
  `src/qt/forms/tests/tst_i18n.qml`.

  **Entry accepts the locale's digits *and* ASCII ones; display emits only the
  locale's.** That asymmetry is the rule morph#596 already set for signs,
  applied to digits: an ASCII `'+'` is accepted in every locale because the
  locale's own spelling is on no keyboard, and an ASCII `'5'` is accepted in an
  `ar_EG` locale for the same reason — a user with an ASCII keyboard has to be
  able to type a number. It costs nothing, because the canonical output spells
  every digit in ASCII whatever the input spelled it, so no two accepted
  spellings can produce different *values*.

  **An entry may not mix the two digit families.** `"\u06655"` — one
  Arabic-Indic digit and one ASCII digit — is malformed, not `"55"`. This is a
  decision rather than a consequence, so it is written here and pinned by a test
  on both edges: neither a keyboard nor a display edge produces an
  interleaving, and rejecting it matches the existing strictness about a sign
  anywhere but the leading position. When `zeroDigit` is the ASCII `"0"` the two
  families are the same set, so nothing can mix and the rule is invisible —
  which is why it costs no existing caller anything.

  **An empty or undecodable `zeroDigit` reads as ASCII `"0"`**, the reading an
  empty `negativeSign` gets and for the same reason: there is no locale without
  digits, so empty cannot mean absence, and a base of "nothing" would reject
  every entry the locale can produce. Because `zeroDigit` defaults to `"0"`,
  every caller that does not name it is byte-identical to the
  five-positional-parameter version — asserted rather than assumed: a 1680-case
  sweep (14 locale configurations × 60 entries × both edges) run against the
  pre-morph#591 header and against this one produced identical output.

  All the locale facts are `std::string_view`, not `char`, because a real
  locale's
  separator is not always one byte: fr-FR groups with U+202F (narrow no-break
  space, 3 bytes in UTF-8) and several locales use U+00A0 (2 bytes). Typed as
  `char`, neither could be expressed at all — a caller could only pass some
  single byte that never matched, so a perfectly valid `"1 050,25"` typed by a
  French user normalised to `std::nullopt` and the control reported it
  malformed. An empty view means "this locale has no such separator".

  **So is the negative sign** (morph#583). The same argument applies to the
  sign, and was missing here: both edges read `NumericLocale::negativeSign`,
  matched and emitted as a whole string the way the separators are. Of the 711
  locales `QLocale::matchingLocales`
  reports under Qt 6.11.2, 77 spell it as something other than a bare ASCII
  `'-'`:

  | `negativeSign` | locales | e.g. |
  | --- | ---: | --- |
  | U+002D | 634 | `C` |
  | U+061C U+002D | 24 | `ar_EG` |
  | U+200E U+002D | 9 | `ar_DZ` |
  | U+200E U+002D U+200E | 17 | `az_IR` |
  | U+200E U+2212 | 2 | `fa_IR` |
  | U+200F U+002D | 2 | `ckb_IQ` |
  | U+2212 | 23 | `eu_ES` |

  Matched as the literal byte `'-'`, none of the 77 round-tripped: the display
  direction emitted a sign the entry direction then rejected, so the pair was
  not inverse for any of them. Note `ar_DZ`, whose sign *is* the ordinary
  hyphen — it failed on the U+200E in front of it, so this was never only "the
  U+2212 locales", and a wider `char` would not have fixed it. Whole-string
  matching is what covers the 2–3 code point bidi forms, which is why the sign
  is typed like the separators rather than widened.

  **ASCII `'-'` stays accepted whatever the locale.** A bare `'-'` is taken in
  the leading position *in addition to* `negativeSign`. U+2212 and the bidi
  marks are on no keyboard, so matching only the locale's own spelling would
  reject the sign the user can actually type and leave them no way to enter a
  negative number at all. This is not the kind of guess the grouping rule
  forbids: that rule is about producing a wrong *value*, and a hyphen in a
  numeric entry has no second reading. The canonical output always spells the
  sign `'-'`, whatever the input spelled it.

  **An empty `negativeSign` means the ASCII default, not "no sign".** Unlike a
  group separator there is no locale without a negative sign, so empty cannot
  mean absence — and on the display edge it must not, because a sign that
  formatted to nothing would turn `-5` into `5`: a valid number of the wrong
  sign, which is the morph#574 failure mode rather than a rejection.

  The renderer passes the locale's own sign: `DynamicForm.qml` already binds
  `qtLocale: Qt.locale(displayLocale)` and forwards
  `qtLocale.decimalPoint`/`qtLocale.groupSeparator`, and now forwards
  `qtLocale.negativeSign` from the same object at all three call sites. The
  member is defaulted, so a caller that names only the separators is unchanged.

  **A leading positive sign is accepted on entry and never emitted on
  display** (morph#596). `normalizeLocaleNumber` reads
  `NumericLocale::positiveSign`, matched exactly as `negativeSign` is —
  the locale's own spelling as a whole string, plus a bare ASCII `'+'` in every
  locale — and **drops** what it matches: `"+5"` normalises to `"5"`, not to
  `"+5"`. Measured over `QLocale::positiveSign` for the same 711 locales under
  Qt 6.11.2:

  | `positiveSign` | locales | e.g. |
  | --- | ---: | --- |
  | U+002B | 657 | `C` |
  | U+061C U+002B | 24 | `ar_EG` |
  | U+200E U+002B | 11 | `ar_DZ` |
  | U+200E U+002B U+200E | 17 | `az_IR` |
  | U+200F U+002B | 2 | `ckb_IQ` |

  54 of 711 are more than one code point. Unlike the negative side there is no
  U+2212 analogue, so *every* non-ASCII spelling here is multi-code-point and
  whole-string matching is the only thing that can match any of them. Before
  this, a leading `'+'` fell through to the "any other character is malformed"
  arm and an explicitly-positive entry was rejected in every locale, `"C"`
  included.

  **The two functions are deliberately not inverse across a positive sign.**
  `formatCanonicalNumber` ignores `NumericLocale::positiveSign` entirely and
  never emits a positive sign, in any locale. This breaks the strict inverse relationship the
  pair otherwise holds, on purpose, and it is written down here rather than left
  for the next reader to infer from a missing parameter. The reason is the
  asymmetry in what each direction can get wrong. Canonical text is
  `-?[0-9]+(\.[0-9]+)?` — there is no `'+'` in it — so the entry edge has
  somewhere to put an accepted `'+'`: nowhere, which costs nothing. The display
  edge has no such option: `positiveSign` is `'+'` in 657 of the 711 locales, so
  emitting it would turn every positive number in every form from `5` into `+5`,
  a visible change to the product with no reported need behind it. morph#583 had
  a forced hand — the display edge emitted a sign the entry edge rejected, so
  the pair *was* broken and something had to give. Here nothing is broken: this
  is new acceptance, which is why it is an enhancement and why it stops at the
  one edge where acceptance is free. Rejecting text the display edge produced is
  a defect; accepting text no display edge produces is not.

  **Grouping is validated, never merely stripped.** A group separator is
  dropped only where a group separator can legally be: preceded by one to three
  digits, followed by exactly three more, and never after the decimal
  separator. `"1.050,25"`, `"1.000.000,25"` and an ungrouped `"1050,25"` all
  normalise; `"1.5"`, `"1.50"`, `"1.05"` and `"1.2.3.4"` in a de-DE locale are
  malformed, and so is the en-US mirror image `"1,5"`. This is not
  strictness for its own sake: dropping every occurrence unconditionally, as
  both control edges used to, turns a de-DE user's US-style `"1.5"` into `15` —
  a perfectly valid number, ten times too large, that no downstream check can
  recognise as wrong, so the user is charged ten times with no diagnostic
  anywhere (morph#574). The field's job at this edge is to report a fact to the
  layer that owns the policy, not to produce a number at any price.

  **The two separators must differ.** A non-empty `groupSeparator` equal to
  `decimalSeparator` is rejected like any other malformed entry — with one
  string in both roles there is no reading of `"1.5"` the function could
  defend, and the old code silently ate the decimal. It is reported through the
  return value rather than an assertion, deliberately: an assertion would make
  a control edge behave differently in Debug and Release, and would be
  untestable in the configuration where it fires.

  **Both edges, or neither.** `src/qt/forms/qml/DynamicForm.qml` carries a
  JavaScript mirror of this function, and a divergence between them is a
  divergence in what the product accepts. The mirror produced byte-identical
  wrong answers on all of the cases above and carries byte-identical
  validation now; changing one without the other is the defect, not the fix.
  The mirror compares one UTF-16 code unit at a time, so the whole-string sign
  match is spelled `text.startsWith(sign, i)` there rather than `ch === sign` —
  a one-unit comparison could not match the 2–3 code point forms at all. The
  mirror's `normalizeLocaleNumber` takes `positiveSign` the same way and drops
  it the same way, and its `formatCanonicalNumber` takes none, for the reason
  above; the renderer forwards `qtLocale.positiveSign` at the two entry call
  sites that already forward `qtLocale.negativeSign`, and nothing changes at the
  display call site. All three call sites now also forward
  `qtLocale.zeroDigit`, the display one included — that is what "both edges, or
  neither" costs for morph#591.

  **The two separators are matched the same way, for consistency rather than
  for a locale** (morph#599). Both sign conversions above left the mirror's
  *separator* branches spelled `ch === groupSeparator` and
  `ch === decimalSeparator` — a one-code-unit comparison, a few lines from the
  whole-string sign match, with nothing saying why. They are now
  `text.startsWith(sep, i)` as well, advancing the index by the separator's
  length the way the sign branches already do.

  Unlike the signs, **no locale reaches this**, and the rule rather than a user
  report is the reason to fix it. Measured with `QLocale::matchingLocales`
  under Qt 6.11.2, over the same 711 locales:

  | field | spellings longer than one UTF-16 code unit |
  | --- | ---: |
  | `decimalPoint` | 0 of 711 |
  | `groupSeparator` | 0 of 711 |
  | `negativeSign` (control) | 54 of 711 |
  | `positiveSign` (control) | 54 of 711 |

  The two sign rows are the control: this is not a measurement that returns
  zero for any locale field it is pointed at. Every one of the nine distinct
  `groupSeparator` spellings (U+0027, U+002C, U+002E, U+00A0, U+060C, U+066C,
  U+12C8, U+202F, U+2E41) and all three `decimalPoint` spellings (U+002C,
  U+002E, U+066B) is a single code unit, so entry behaviour is byte-identical
  before and after for every locale Qt knows. The change is to the *rule* this
  paragraph states, not to the product.

  That has a consequence for how it can be tested, and it is the reason this is
  written down: **a test driven by a real `Qt.locale(...)` cannot distinguish
  the fixed mirror from the broken one.** With a one-unit separator,
  `ch === sep` and `startsWith(sep, i)` agree on all 711, so such a test passes
  whatever the code does. The corpus that pins this is therefore synthetic —
  separators of two or more code units that no locale uses, passed straight to
  both functions — and it is pinned identically on both edges
  (`[morph599]` in `tests/test_render_locale_format.cpp`, and the
  `test_aMultiUnitSeparator*` functions in
  `src/qt/forms/tests/tst_i18n.qml`). One of the synthetic separators is a
  surrogate pair: a single code *point* that is two code *units*, which a
  per-code-point mirror would still get wrong.
- **Timestamps.** The wire value is strict UTC ISO-8601
  ([datetime.md](../util/datetime.md)); a renderer displays and edits in the
  user's zone by shifting a `morph::time::DateTime` with its existing
  duration-arithmetic operators (`dt + std::chrono::minutes{offset}` for
  display, `dt - std::chrono::minutes{offset}` back to canonical UTC before
  submission) — no new arithmetic is needed, only the offset the renderer
  chooses to display in. A locale-formatted entry must round-trip to the
  identical canonical wire value.
- **Choice option labels are data, not chrome.** Option rows come from
  executing the options action ([choice.md](choice.md)); the catalog never
  sees them. A model that wants localised rows reads
  `session::current()->locale` server-side
  ([session.md](../session/session.md)) — the one place server-side locale
  participates.

### Non-goals

- No per-locale schema variants — `schemaJson<A>()` keeps its one cached,
  un-localised schema.
- No translation storage format — the `TranslationProvider` signature is the
  whole contract.
- No server-side message localisation — canonical error strings stay
  diagnostic/protocol vocabulary.
- No RTL / layout mirroring engine.
- Not machine translation, locale negotiation, or plural rules — the catalog
  is a lookup; anything richer lives inside the host's provider
  implementation.

## `allRequiredEngaged<A>()` — readiness check

```cpp
template <typename A>
[[nodiscard]] constexpr bool allRequiredEngaged(const A& action) noexcept;
```

Returns `true` when every **required** empty-capable member of `action` has
`hasValue() == true`. Required has the same meaning as in the
[Required-ness rule](#required-ness-rule): not `std::optional<...>`, not
listed in `A::optionalFields`, and not a `computedFields` destination.
Non-empty-capable members (plain ints, strings, etc.) are skipped — they
cannot express "not filled in". Intended as the body of the action's
`validate()` (the `ActionValidator` machinery picks it up automatically).

The two exclusions are enforced by **different mechanisms**, and only one is an
explicit test. A member is inspected at all only when it satisfies
`EmptyCapableField` (`.hasValue()` exists); the sole explicit check inside the
loop is `!declaredOptional<A>(name)` against `A::optionalFields`. A
`std::optional<...>` member is *not* excluded by an `isStdOptional` test here —
it is skipped because `std::optional` exposes `has_value()`, not `hasValue()`,
so it never satisfies `EmptyCapableField` in the first place. (This differs from
the `required`-array derivation in `mergeSchemaExtras`, which checks
`isStdOptional` **explicitly** — see [Required-ness rule](#required-ness-rule).)
The predicate is `noexcept` and `constexpr`, and it inspects only the action's
**own top-level members**; unlike `schemaJson<A>()`'s schema generation (see
[Nested aggregates (recursive, depth-bounded)](#nested-aggregates-recursive-depth-bounded)),
it does **not** recurse into a nested aggregate member's own fields.

## Cross-field rules — the `x-rules` vocabulary

`allRequiredEngaged` is per-field and membership-blind by design. A condition
spanning **two or more fields** — "end date must be after start date", "supply
either an email or a phone but not both", "discount is required only when a
promo code is entered" — is expressed with a **closed, typed rule vocabulary**
declared once as an action's `static constexpr formRules` member, built with
`morph::forms::ruleList(...)`:

```cpp
struct BookRoom {
    morph::time::Timestamp checkIn;
    morph::time::Timestamp checkOut;
    std::optional<std::string> email;
    std::optional<std::string> phone;
    Quantity<Unit::money> promo;
    Quantity<Unit::money> discount;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::greater(&BookRoom::checkOut, &BookRoom::checkIn),
        morph::forms::exactlyOneOf(&BookRoom::email, &BookRoom::phone),
        morph::forms::requiredWhen(&BookRoom::discount, morph::forms::engaged(&BookRoom::promo)),
        morph::forms::visibleWhen(&BookRoom::discount, morph::forms::engaged(&BookRoom::promo)));

    [[nodiscard]] bool validate() const {
        return morph::forms::allRulesSatisfied(*this) && morph::forms::allRequiredEngaged(*this);
    }
};
```

One declaration drives three consumers: `schemaJson<A>()` emits it as a
top-level `x-rules` array (alongside `required`); `allRulesSatisfied<A>(action)`
evaluates it as the shared C++ predicate; and because `validate()` calls
`allRulesSatisfied`, `ActionValidator<A>::ready` ([registry.md](../core/registry.md))
picks it up automatically on every dispatch path that already enforces
`ready()` — `morph::flows::FlowSession::set<>`'s gate, the client
request/reply gate, and the server dispatch runner
([registry.md](../core/registry.md)) — with no extra
code anywhere. The vocabulary is deliberately closed: adding a new rule kind is
a framework change, never an application-supplied lambda, which is what lets
the client and the server evaluate identically from the same serialized form.

### The rule and condition kinds

Every row names all three spellings of the same kind: the factory a C++ author
calls, the `kind` string the schema carries, and the `RuleKind` enumerator the
framework switches on. They are listed together because a reader emitting JSON
and a reader writing C++ read the same table, and the C++ capitalisation is not
derivable from the wire spelling by any rule stated anywhere.
`scripts/check_spec_citations.sh` (check 6) reads `ruleKindName()`'s switch and
requires each enumerator and its wire spelling to appear in one row here, so a
kind added to the enum cannot reach the wire undocumented.

| Factory | Meaning | `x-rules` `kind` | `RuleKind` enumerator | Also valid as a condition? |
|---|---|---|---|---|
| `requiredWhen(field, cond)` | `field` must be engaged when `cond` holds. | `"requiredWhen"` | `RuleKind::RequiredWhen` | no (only ranges over conditions itself) |
| `greater(a, b)` / `greaterOrEqual(a, b)` | `*a > *b` / `*a >= *b`. | `"greater"` / `"greaterOrEqual"` | `RuleKind::Greater` / `RuleKind::GreaterOrEqual` | yes |
| `less(a, b)` / `lessOrEqual(a, b)` | `*a < *b` / `*a <= *b`. | `"less"` / `"lessOrEqual"` | `RuleKind::Less` / `RuleKind::LessOrEqual` | yes |
| `exactlyOneOf(f1, f2, ...)` | Exactly one listed field is engaged. | `"exactlyOneOf"` | `RuleKind::ExactlyOneOf` | no |
| `atLeastOneOf(f1, f2, ...)` | At least one listed field is engaged. | `"atLeastOneOf"` | `RuleKind::AtLeastOneOf` | no |
| `mutuallyExclusive(f1, f2, ...)` | At most one listed field is engaged. | `"mutuallyExclusive"` | `RuleKind::MutuallyExclusive` | no |
| `visibleWhen(field, cond)` | **Presentation:** `field` is shown only while `cond` holds. | `"visibleWhen"` | `RuleKind::VisibleWhen` | no |
| `readonlyWhen(field, cond)` | **Presentation:** `field` is editable only while `cond` does **not** hold. | `"readonlyWhen"` | `RuleKind::ReadonlyWhen` | no |
| `engaged(field)` / `notEngaged(field)` | `field` is / is not engaged. | `"engaged"` / `"notEngaged"` | `RuleKind::Engaged` / `RuleKind::NotEngaged` | yes (condition-only) |
| `equals(field, literal)` | `field`'s engaged value equals `literal`. | `"equals"` | `RuleKind::Equals` | yes (condition-only) |
| `andOf(cond1, cond2, ...)` | Every listed condition holds (boolean AND). | `"and"` | `RuleKind::And` | yes — also usable directly as a top-level rule |
| `orOf(cond1, cond2, ...)` | At least one listed condition holds (boolean OR). | `"or"` | `RuleKind::Or` | yes — also usable directly as a top-level rule |
| `notOf(cond)` | The nested condition does **not** hold (boolean NOT). | `"not"` | `RuleKind::Not` | yes — also usable directly as a top-level rule |

`engaged`/`notEngaged`/`requiredWhen`/the membership rules accept any
`EngageableField` — an `EmptyCapableField` (`Quantity`/`Choice`/`Timestamp`) or
a plain `std::optional<T>` (which does **not** satisfy `EmptyCapableField` —
see "two exclusions" above — but does count as engageable for rule purposes).
`greater`/`greaterOrEqual`/`less`/`lessOrEqual` are narrower: both operands
must be the **same** `EmptyCapableField` type whose engaged value
(`operator*()`) is three-way-comparable — `Quantity<U, Dec>` (compares the
exact `math::Rational` payload, never a `double`) or `morph::time::Timestamp`
(compares `DateTime`). An unengaged operand makes a comparison **vacuously
satisfied** (`true`) — both as a top-level rule and when reused as a nested
condition — so a form still being filled in never fails a comparison
prematurely; the required-ness of the operand itself is a separate
`required`/`requiredWhen` concern. `equals`, by contrast, is **not** vacuous:
an unengaged field cannot equal anything, so it returns `false` until the
field is engaged. A literal passed to `equals` is one of `std::int64_t`,
`bool`, `std::string`, the exact `math::Rational` (never a `double`), or a
captured string literal, so it serialises losslessly into `x-rules`.

**Comparing a field to a literal is not this vocabulary's job.** `equals` is
the only node that takes one, and only for equality — there is deliberately no
`greaterOrEqual(&A::field, 1)`. A bound on a single field's value is a property
of that field, not a relation between two of them, and is declared on its
`FieldMeta` instead: see
[Per-field scalar bounds](#per-field-scalar-bounds--minimum--maximum--multipleof),
which also covers integrality (`multipleOf`), a constraint `x-rules` cannot
express in any form.

A bare string literal — `equals(&A::code, "URGENT")` — is captured **inline**
as a `detail::LiteralString` (an alias for the project's shared
`morph::detail::FixedString`), not copied into a `std::string`. That is what
keeps the documented
`static constexpr auto formRules = ruleList(...)` declaration working for a
literal of any length: a rule node has to be a literal type, and a `std::string`
holding more characters than the standard library's small-string buffer (15 on
libstdc++) allocates, so the declaration fails with "refers to a result of
`operator new`". The limit was invisible in the source — the same code compiled
or did not depending only on how long the literal was, and on which standard
library was in use. Serialisation is unaffected: `emitNode()` emits the same
JSON string either way. Passing an explicit `std::string` still stores a
`std::string` and still cannot be `constexpr` when it allocates; that is
inherent to the type the caller chose.

### Compound conditions — `andOf` / `orOf` / `notOf`

The single-node conditions above (`engaged`, `notEngaged`, `equals`, and the
comparison kinds reused as booleans) compose into a **recursive condition
tree** via three more factories:

```cpp
struct BookRoom {
    // ...
    static constexpr auto formRules = morph::forms::ruleList(
        // discount required only when BOTH promo and a loyalty code are engaged
        morph::forms::requiredWhen(
            &BookRoom::discount,
            morph::forms::andOf(morph::forms::engaged(&BookRoom::promo),
                                morph::forms::engaged(&BookRoom::loyaltyCode))));
};
```

- **`andOf(cond1, cond2, ...)`** — holds when every listed condition holds
  (at least two conditions; `test()` short-circuits left to right).
- **`orOf(cond1, cond2, ...)`** — holds when at least one listed condition
  holds (at least two conditions; `test()` short-circuits left to right).
- **`notOf(cond)`** — holds when the single nested condition does **not**
  hold.

Each factory accepts any node satisfying the `morph::forms::Condition` concept
as a child — a leaf (`engaged`, `equals`, `greater`, …), a membership rule, or
another `andOf`/`orOf`/`notOf` — so a tree nests to any depth:
`orOf(notOf(engaged(&A::x)), andOf(engaged(&A::y), engaged(&A::z)))` is a valid
`when` clause. All three nodes share this uniform shape with every existing
rule/condition node (`kind`, `test(const A&) const noexcept`, `emitNode()`),
which is what makes them substitutable everywhere an existing single-node
condition already worked:

- **Nested inside a `when` clause** — `requiredWhen`/`visibleWhen`/`readonlyWhen`
  accept a compound condition in the same `when` position a leaf condition
  occupies, with no change to those three rule kinds themselves.
- **Directly as a top-level `formRules` entry** — `andOf`/`orOf`/`notOf`
  declare `isPresentation = false` and a `test()`, so `ruleList(andOf(...))`
  is itself a valid, directly-gating rule — "a single rule with a compound
  condition tree", not only a condition factored inside another rule.

`andOf`/`orOf`/`notOf` add no new closed-vocabulary *rule* kinds — they are
closed-vocabulary *conditions*, matching the existing "closed, typed" design
of every other node in this table: an application still cannot supply an
arbitrary lambda, only compose the existing typed primitives into a tree.

#### What may be a condition — the `Condition` concept

The three combinators and the three `when`-bearing rules constrain their
condition operands on `morph::forms::Condition`, which a node opts into with
`static constexpr bool isCondition = true`. **`visibleWhen`, `readonlyWhen` and
`requiredWhen` do not opt in**, and the reason is worth stating because they
have the same shape as everything that does.

`VisibleWhen::test()` and `ReadonlyWhen::test()` return `true`
*unconditionally, by design*: they are presentation rules, they never gate
submission, and a renderer reads their `when` clause rather than calling them
(see [the rule list](#the-rule-and-condition-kinds)). Nested as a condition,
such a node therefore contributes a **constant** — `andOf(visibleWhen(…), c)`
collapses to `c` — while looking exactly like a condition that says something.
The author wrote "while this field is visible" and got "true". `requiredWhen`
is excluded for the neighbouring reason: it is a rule *about* a condition
rather than a condition, and nesting one emits a `"requiredWhen"` node in a
`when` position no renderer's condition vocabulary has a case for. All six
spellings compiled before this was enforced, because "exposes
`test(const A&) const noexcept`" is a test every rule node passes.

The membership rules (`exactlyOneOf` / `atLeastOneOf` / `mutuallyExclusive`)
**do** opt in: their `test()` genuinely ranges over the action, so composing
one into a tree changes what the tree evaluates to. A renderer that does not
recognise them in a `when` position treats them as "cannot evaluate" and defers
to the server, which is the sanctioned fallback (see
[Renderer fallback](#renderer-fallback)) rather than a disagreement between the
two evaluators. The "Also valid as a condition?" column above records which
kinds the *shipped renderer's* condition vocabulary evaluates directly; it is
not the same question as which nodes the C++ factories accept.

The marker is declared per node rather than derived from `isPresentation`
(which would wrongly admit `RequiredWhen`) or from the presence of `test()`
(which admits everything), so adding a kind to the condition vocabulary is one
line on the node itself.

The same constraints carry two diagnostics. `andOf`/`orOf` recover the action
type from their first operand, and now require every other operand to agree,
so `andOf(engaged(&C::x), engaged(&B::y))` is an error at the call site rather
than 153 lines whose first line is inside `<type_traits>`. And `equals` requires
the field to be comparable against the literal at all, so
`equals(&A::someQuantity, "URGENT")` names `equals` and the caller's own line
instead of reporting `no match for 'operator=='` from inside `forms.hpp`.

#### Schema emission — nested `conditions` / `condition`

`andOf`/`orOf` emit a `"conditions"` array of nested condition nodes;
`notOf` emits a single nested `"condition"` object (singular, since it wraps
exactly one child):

```json
{ "kind": "requiredWhen", "fields": ["discount"],
  "when": { "kind": "and", "conditions": [
    { "kind": "engaged", "fields": ["promo"] },
    { "kind": "engaged", "fields": ["loyaltyCode"] }
  ]}
}
```

```json
{ "kind": "or", "conditions": [
  { "kind": "not", "condition": { "kind": "engaged", "fields": ["promo"] } },
  { "kind": "and", "conditions": [
    { "kind": "engaged", "fields": ["email"] },
    { "kind": "engaged", "fields": ["phone"] }
  ]}
]}
```

A renderer that does not recognise `"and"`/`"or"`/`"not"` treats them as an
unrecognised `kind` (see "Renderer fallback" below) — it defers enforcement to
the server rather than guessing at the nested structure, exactly like any
other unrecognised `kind`. A renderer that recognises them but not one of
their *children* has the same answer available: "cannot evaluate" propagates
up through `and`/`or`/`not` rather than collapsing to false.

### Presentation rules never gate

`visibleWhen`/`readonlyWhen` are the only two **presentation** kinds: they
never participate in `allRulesSatisfied` (skipped by construction, via each
node's `isPresentation` flag), only in what a renderer shows/enables. While a
field is hidden by `visibleWhen`, its current draft value still travels in the
payload — hiding never clears it, exactly like a static `x-hidden` field. An
author who wants "hidden ⇒ also not required" pairs `visibleWhen(f, c)` with
`requiredWhen(f, c)` explicitly; neither implies the other.

### The `x-rules` schema emission

`mergeSchemaExtras` walks `A::formRules` (when declared) and emits a
**top-level** `x-rules` array, alongside `required`. Each element is
self-describing JSON a renderer (or the server) can evaluate without any C++
type information:

```json
"x-rules": [
  { "kind": "greater", "fields": ["checkOut", "checkIn"] },
  { "kind": "exactlyOneOf", "fields": ["email", "phone"] },
  { "kind": "requiredWhen", "fields": ["discount"],
    "when": { "kind": "engaged", "fields": ["promo"] } },
  { "kind": "visibleWhen", "fields": ["discount"],
    "when": { "kind": "engaged", "fields": ["promo"] } }
]
```

Field names are the **wire (JSON) field names**, resolved from the
pointer-to-member the same way `x-order` is derived: a fresh probe instance of
the action is walked and each rule's stored member pointer is matched against
the probe's members by address. An action with no `formRules` emits no
`x-rules` key at all — byte-identical to a version of the schema generated
before this feature existed.

### Unsatisfiable declarations — `required` contradicting `x-rules`

`required` is derived from field required-ness ([Required-ness rule](#required-ness-rule));
`x-rules` is derived from `A::formRules`. Nothing links the two derivations, so
an action can declare both halves sensibly on their own and still describe a
form **no submission can satisfy**:

```cpp
struct CaptureConcentration {
    Concentration value;        // EmptyCapableField -> required by default
    QualifierChoice qualifier;  // EmptyCapableField -> required by default

    // `required` demands both. `exactlyOneOf` permits exactly one.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::exactlyOneOf(&CaptureConcentration::value,
                                   &CaptureConcentration::qualifier));
};
```

A renderer honouring `required` demands both fields; a payload meeting that
demand then fails `exactlyOneOf` on the server. The form is dead on arrival,
and both halves of the served schema look entirely reasonable in isolation.

`schemaJson<A>()` **rejects this at generation** by throwing
`morph::forms::UnsatisfiableFormError`. The check lives in
`detail::rejectUnsatisfiableRules`, called from `mergeSchemaExtras` — the one
place both halves are in hand — and reads the *emitted* rule nodes against the
*emitted* `required` array, so it matches on the same wire names a renderer
would.

**What counts as a contradiction.** A rule kind that **caps** how many of the
fields it ranges over may be engaged at once, ranging over **two or more**
fields that are also in `required`:

| Rule kind | Caps engagement? | Why |
|---|---|---|
| `exactlyOneOf` | yes — ceiling of one | Two required fields cannot both be engaged and still be "exactly one". |
| `mutuallyExclusive` | yes — ceiling of one | Same ceiling; "at most one" and "both required" cannot hold together. |
| `atLeastOneOf` | **no** — it is a *floor* | Satisfied by engaging every field it names, so it can never contradict `required`. Rejecting it would be a false positive. |
| `requiredWhen` | **no** | Only ever *adds* required-ness; it cannot cap anything. |
| everything else | no | Comparison, presentation, and compound kinds impose no engagement ceiling. |

`detail::capsEngagedCount(kind)` is the single place the capping kinds are
named. A future rule kind carrying a ceiling ("at most two of these") joins
that list and is covered with no other change.

#### A capping rule wrapped in `andOf` is caught too — and only `andOf`

The check reads a list of rule nodes as a **conjunction**: every element has to
hold, so a contradiction in any one element is a contradiction of the whole.
The top-level `x-rules` array is such a conjunction — `allRulesSatisfied` folds
it with `&&` — and so is an `and` node's `conditions`. The check therefore
descends into `and`, at any depth:

```cpp
// Both of these are rejected. They are the same contradiction.
ruleList(exactlyOneOf(&A::a, &A::b))
ruleList(andOf(exactlyOneOf(&A::a, &A::b), engaged(&A::c)))
```

The second spelling used to ship silently, because the check skipped any node
with no `fields` key and `and`/`or`/`not` emit `conditions`/`condition`
instead. The same contradiction being a hard build failure in one spelling and
an unsubmittable form in the other is worse than not checking at all: the
check's *existence* is what an author trusts.

`or` and `not` are **not** descended, and that is a property of the operators
rather than an omission:

- Under **`or`**, a contradictory operand only makes that branch dead. The
  other branch can still satisfy the rule, so
  `ruleList(orOf(exactlyOneOf(&A::a, &A::b), engaged(&A::c)))` generates.
- Under **`not`**, the contradiction inverts into a requirement. With `a` and
  `b` both `required`, `ruleList(notOf(exactlyOneOf(&A::a, &A::b)))` asks for
  *not* exactly one of them engaged — which engaging both, precisely what
  `required` already demands, satisfies. So it generates too.

Rejecting either would be a false positive, and a false positive here is a hard
build failure on a form that works.

**Boundaries that deliberately do *not* throw:**

- **Exactly one required field inside a capping rule.** Satisfiable: engage
  that field, leave the rest empty. Only two or more conflict.
- **`std::optional` members.** `detail::isStdOptional` keeps them out of
  `required` on sight, so a rule over `std::optional` fields can never reach
  the contradiction — with or without this check. The reachable case is an
  `EmptyCapableField` (a `Quantity`, a `Choice`, a strong id): required by
  default, and rangeable by a membership rule.
- **Required fields the rule does not name.** Only the intersection of the
  rule's `fields` and `required` is counted.

**How an author fixes it.** Name the rule's fields in `A::optionalFields`. The
rule then becomes the *only* gate on them, which is what the multi-field
sum-type encoding ([Sum types not in the forms palette](#sum-types-not-in-the-forms-palette--multi-field-encoding-by-design))
actually means. Alternatively, drop the rule.

**Why this one exception to permissive generation.** Everywhere else, schema
generation tolerates an author's declaration mistake silently: a `formLayout`
entry naming a field the action does not have is ignored, a field claimed by
two groups keeps the first. That is right, because a tolerated mistake still
yields a **working form** — the author loses a layout hint, not the form. This
case is different in kind: the result is a form **nobody can submit**, on any
client, with no error naming the reason. The failure is already certain at
generation time and belongs to the author's own build, so it is raised there
rather than left to surface as a user who cannot press Save. A `static_assert`
would be better still, but `detail::resolveFieldName` is not `constexpr` (it
matches member addresses against a runtime probe instance), so a
generation-time throw is the achievable form today.

**Interaction with the schema cache.** `schemaJson<A>()` memoises into a
function-local `static const std::string`. A throw during that static's
initialisation leaves it uninitialised, so a later call re-runs the check and
throws again, rather than serving a half-built or empty schema.

### Server-side: the same list, evaluated in the dispatcher

The server never trusts the client's evaluation of `x-rules`; it re-runs
`A::formRules` itself. Because an action's `validate()` calls
`allRulesSatisfied(*this)`, and `ActionValidator<A>::ready` auto-detects
`validate()` via `HasValidate` ([registry.md](../core/registry.md)), the
server dispatch runner evaluates the **exact same rule list** the client did —
the same typed nodes over the same values — with zero extra server code. A
hand-built envelope that violates a rule is rejected with
`morph::model::ValidationError` ([registry.md](../core/registry.md)) on every
dispatch path (local, simulated-remote, Qt WebSocket), before `Model::execute`
runs.

**Read "the same rule list", not "the same evaluation".** The server walks
`A::formRules` — typed nodes over decoded members. A client walks the *emitted*
`x-rules` JSON over widget state, which is a different representation of the
same declaration: a `Quantity` is exact `Rational` arithmetic on one side and
entered text on the other, and a client is free to be an approximation of the
server, never the reverse. Two consequences follow, and both are load-bearing
rather than caveats:

- a client verdict is a **convenience**, and the correctness floor is the
  server's — no client rounding, no unrecognised kind, and no missing key can
  let an action past `validate()`;
- because the two are different code, "they agree" is a property that has to
  be *tested*, not assumed. See [Two evaluators, one corpus](#two-evaluators-one-corpus).

### Renderer fallback

Every key here is additive and optional, consistent with the unversioned
schema stance below. An action declaring no `formRules` emits no `x-rules` and
behaves exactly as before this feature existed. A renderer that does not
understand `x-rules` still produces a usable form: it honours the per-field
`required` array and lets the **server** reject any cross-field violation —
the correctness floor never depends on the client understanding the key. An
unrecognised `kind` (a rule *or* a nested condition) must be treated as
"cannot evaluate" by a client renderer, which defers enforcement to the
server rather than passing the rule — the server, running the compiled C++
rule list directly, has no such "unrecognised kind" case.

#### "Cannot evaluate" means defer, not block

"Cannot evaluate" is a **third** answer, alongside true and false, and the two
shipped clients of this sentence once read it in opposite directions — one
blocked submission on an unknown `kind`, the other deferred (morph#176). The
contract is *defer*:

| Question a renderer asks | Answer when the condition cannot be evaluated |
|---|---|
| Does this rule block submission? | **No.** Hand the payload to the server. |
| Is this `requiredWhen` field required right now? | **No.** Only a definitely-true condition makes a field required. |
| Is this `visibleWhen` field shown? | **Yes.** Never hide a field over a condition you could not judge. |
| Is this `readonlyWhen` field frozen? | **No.** Leave it editable. |
| What is `and`/`or`/`not` of it? | "Cannot evaluate" **propagates**: `and` is false if any child is false, unevaluable if none is false but some is unevaluable, true otherwise; `or` is true if any child is true, unevaluable if none is true but some is unevaluable, false otherwise; `not` of unevaluable is unevaluable. |

The reason is forward compatibility, and it is the whole point of a vocabulary
that is closed but extensible. Every key here is additive: a server that gains
a seventeenth rule kind must not thereby brick every renderer already
deployed. A blocking client turns each such addition into a breaking change —
the operator sees a form that can never be satisfied, with no error naming
why, and no action of theirs can fix it. A deferring client submits, and the
server answers with the one verdict that was ever authoritative.

Nothing is lost on the safety side, because nothing was ever gained there: the
correctness floor is the server's `validate()`, which evaluates the compiled
rule list and cannot fail to recognise a kind. "Fail closed" is the right
instinct for a *decision*, but a client gate is not a decision — it is a
prediction of one, and a prediction that refuses to be made must not be
allowed to veto the decision.

The last row above is why the three-valued reading matters even for a renderer
that understands every top-level kind it is sent. Collapsing "cannot evaluate"
into `false` makes `not` of an unknown child come out **true**, so a
`requiredWhen` keyed on it starts demanding a field for a reason the renderer
has just admitted it cannot judge — blocking through the back door.

### Two evaluators, one corpus

`x-rules` is evaluated twice in this repository, and a reader should know that
before trusting either:

| Evaluator | Where | Over what |
|---|---|---|
| Compiled | `morph::forms::allRulesSatisfied` (`forms/forms.hpp`) | `A::formRules`, typed nodes over decoded members |
| Client | `testRule`/`testCondition` in `src/qt/forms/qml/DynamicForm.qml` | the emitted `x-rules` JSON over widget text |

They are different code over different representations, so agreement is a
property to be measured. It is measured by a single shared artifact —
`src/qt/forms/tests/data/rule_corpus.json`, **one file with two readers**:

- `tests/test_forms_rule_corpus.cpp` drives every row through
  `allRulesSatisfied`;
- `src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml` drives the same rows
  through a real `DynamicForm`.

Each row is `(schema, field state, expected verdict)`. The schemas are stored
as **text** and parsed by the renderer exactly as an application parses
`controller.schemasJson`, because parsing is what rounds an integer literal
past 2^53 — a fixture built as an inline JSON object could not express that
case at all.

Three assertions are what make this a pin rather than a pair of samples, and a
change to `x-rules` is expected to keep all three true:

1. every corpus schema equals the current `schemaJson<A>()` **byte for byte**;
2. every `kind` `detail::ruleKindName` names appears somewhere in the corpus,
   so a seventeenth rule kind cannot join the vocabulary without rows;
3. every kind carries **both** verdicts — a corpus whose rows all said "allow"
   would pass against a client that never blocks anything.

`visibleWhen`/`readonlyWhen` never gate submission, so their rows carry a
presentation expectation instead of a submit verdict; only the renderer can
assert it, since a `VisibleWhen` node's `test()` returns `true` unconditionally
by construction.

## Computed fields

Some fields are not entered by the user at all — they are a **pure function
of other fields on the same action**: `total = qty * price`, `vatDue = net *
rate`. An action declares one with a `static constexpr` map from a
destination member to its declared input members and a pure derivation,
next to `optionalFields`/`formRules`:

```cpp
struct LineItem {
    Quantity<Units, 2> qty;
    Quantity<Units, 2> price;
    Quantity<Units, 2> total;  // computed -- not user-entered

    // A generic (auto) lambda parameter, not `const LineItem&`: this
    // initializer runs while LineItem is still an incomplete type (a static
    // data member initializer is not a complete-class context the way a
    // member function body or a non-static default member initializer is
    // -- see "Incomplete-type self-reference" above), so the body's member
    // access must stay dependent until first use, after the class is complete.
    static constexpr auto computedFields = morph::forms::computeList(
        morph::forms::computed<&LineItem::total, &LineItem::qty, &LineItem::price>(
            [](const auto& s) { return s.qty * s.price; }));

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};
```

- **`computed<Dst, Inputs...>(fn)`** binds a destination member, its ordered
  input members, and a pure derivation `fn(const A&) -> ValueOfDst`. `Dst` and
  `Inputs...` are pointer-to-data-member NTTPs (trailing template arguments,
  not a braced-list runtime parameter), so a renamed or deleted field is a
  compile error and the input list is type-checked.
- **`computeList(...)`** composes one or more `computed(...)` declarations
  into a `detail::ComputeList<...>` value assigned to `static constexpr auto
  computedFields`. The framework detects it via the `detail::HasComputedFields<A>`
  concept, mirroring `detail::HasOptionalFields<A>`/`HasFormRules<A>`.
- **`recomputeAll<A>(action)`** is the single evaluator: it walks
  `A::computedFields` and, for each entry, overwrites the destination member
  with `fn(action)` — or, if any declared input is unengaged (`hasValue() ==
  false`, for an input satisfying `EmptyCapableField`; a non-empty-capable
  input is always considered engaged), resets the destination to its
  default-constructed (empty) value instead of computing from a missing
  operand. For a `Quantity` destination the result is converted to the
  destination's own type and **rounded** to its declared precision
  (`Quantity::atDeclaredPrecision()`), so the stored value matches
  `x-decimalPlaces` regardless of what declared precision `fn`'s return type
  happened to carry, and regardless of how many decimals the derivation itself
  produced — a product of two 2-decimal operands is exact to 4.
- `fn` must be **pure** — a function of the action's own fields only, no side
  effects, no external state. The framework cannot check this; it is the
  author's contract. Anything impure (model state, a database lookup, the
  current time) belongs in the model's `execute`, not a computed field.

### Schema emission

`mergeSchemaExtras` patches each computed destination's property node with
`x-readonly: true` and `x-computed: { "inputs": [...] }` (wire field names, in
declaration order, resolved from the pointer-to-member the same way
`x-order` is derived), and **excludes it from the synthesised `required`
array** (see [Required-ness rule](#required-ness-rule)) — a computed field is
never something the user must fill. `x-computed`/`x-readonly` are additive,
optional `x-*` keys (see the [renderer contract](#renderer-contract-the-schema-key-vocabulary)
table below); an action that declares no `computedFields` emits neither key.

### Where the value is authoritative

`recomputeAll` runs at three call sites, all authoritative:

1. `ActionExecuteRegistry::registerAction`'s executor (the client-bridge JSON
   dispatch path behind `BridgeHandler::executeJson`, [bridge.md](../core/bridge.md)).
2. `Bridge::executeVia`'s `localOp` (the in-process execution path `LocalBackend`
   uses for every `execute<Action>()`/`executeJson` call, [bridge.md](../core/bridge.md)).
3. `ActionDispatcher::registerAction`'s runner (the server-side execution path
   `RemoteServer` uses for `SimulatedRemoteBackend` and the Qt WebSocket
   transport, [registry.md](../core/registry.md)).

Sites 2–4 run **after** decode and **before** `Model::execute`, so a computed
value arriving on the wire is always discarded and replaced with the
authoritative recomputation — a hostile or buggy client cannot influence the
stored value by tampering with a computed field. On every site that also
decodes JSON (2 and 4; `localOp` never does — it dispatches an already-typed
`Action`), `recomputeAll` runs immediately after `reconcileDeclaredPrecision`
and **before** the `ActionValidator::ready` check, so a validator that
inspects a computed field sees the authoritative, server-derived value rather
than whatever arrived on the wire. Because every site calls the identical
`recomputeAll` over inputs reconciled to declared precision
(`reconcileDeclaredPrecision`, [above](#advertised-precision-is-enforced-on-dispatch)),
the client's displayed value and the server's stored value are identical to
the last digit. It is a **no-op** for actions with no `computedFields` — zero
behaviour change, backward compatible — mirroring how `reconcileDeclaredPrecision`
no-ops for actions with no `Quantity` members.

A cross-field rule ([Cross-field rules](#cross-field-rules--the-x-rules-vocabulary))
that references a computed field evaluates on the server's authoritative
recomputed value, not the client's, since `recomputeAll` runs before the
validator check on every dispatch path.

## Per-instance constraints — values that live in data

Everything above derives a schema from the compiled action type. When a form
*definition* is itself data — a versioned analysis catalogue, a per-tenant
configuration — the values of some framework-meaningful keys belong to a
database row rather than to a template parameter, and no amount of reflection
over `A` can reach them.

`morph::forms::InstanceConstraints` (`forms/instance_constraints.hpp`) is the
seam for exactly that, and no more than that: **an instance varies the values
of existing keys; it never varies the form's shape.** One declaration both
decorates the served schema (`x-decimalPlaces`, `x-minimum`, `x-maximum`, plus
the document-level `x-instanceConstraints` stamp) and checks a submitted value
against the same numbers, so the two cannot drift apart — which is what an
application patching a private key beside the framework's could never promise.

The framework reports violations and the model applies policy; the dispatch
runners do **not** apply instance constraints, because they have no instance to
read one from. See [instance_constraints.md](instance_constraints.md) for the
API, the emitted keys, and the reasoning behind both of those decisions.

**The shipped renderer honours the decorated values.** That is the half that
makes decoration worth doing rather than a second opinion nobody reads:
`DynamicForm.qml` takes `x-decimalPlaces` as the entry granularity whether it
came from the compiled type or from a row, and refuses a `Quantity` outside
`x-minimum`/`x-maximum` in the canonical unit, alongside the compiled
`minimum`/`maximum` — an instance range narrows the type's, it never widens it.
Two boundaries follow the framework's own:

- **`Quantity` fields only**, matching `checkAction`. A client that gated a key
  the model does not check would be a new divergence, not a repair of one.
- **The client comparison is a `double` quotient of the bound's `{num,den}`**,
  exactly like the compiled `minimum`/`maximum` beside it. The exact comparison
  is `checkValue`'s, against a `Rational`; as everywhere else in the renderer,
  the live gate is an approximation and the model is the floor.

## Support traits and helpers

| Symbol | Kind | Purpose |
|---|---|---|
| `detail::IsStdOptional<T>` | trait | `true` when `T` is a `std::optional<...>`. |
| `detail::isStdOptional<T>` | variable template | cvref-stripped alias of the trait. |
| `detail::HasOptionalFields<A>` | concept | `true` when `A` has a `static constexpr` iterable `optionalFields`. |
| `detail::declaredOptional<A>(name)` | constexpr function | `true` when `name` appears in `A::optionalFields`. |
| `detail::forEachNamedMember(action, visitor)` | function template | Calls `visitor.operator()<I>(name, member)` for every reflected member of `action` (uses glaze pure reflection). |
| `detail::findMember(node, key)` | function template | The checked read over a `glz::generic_u64` object node: a pointer to the member, or `nullptr` when `node` is not an object or has no such key. Never inserts, never throws; `const`-preserving. See [Reading the DOM with `findMember`](#reading-the-dom-with-findmember). |
| `detail::mergeSchemaExtras<A>(raw)` | function | Post-processes a glaze-generated schema to inject `required`, `x-decimalPlaces`, `x-order`, `x-unitAlternatives`, `x-optionsAction`, `title`, `description`/`x-placeholder`/`x-readonly`/`x-hidden` etc. onto the property nodes. Called by `schemaJson<A>()`. |
| `reconcileDeclaredPrecision<A>(action)` | function | **Rounds** every `Quantity` member of `action` in place to its declared precision (`atDeclaredPrecision()`, an exact `Rational` re-rounding — not a retag), so a decoded wire value *equals* the schema's advertised `x-decimalPlaces`, not merely displays at it. Empty members stay empty. No-op for non-`Quantity` members and for action types glaze cannot reflect. Called on both wire dispatch paths (`bridge.hpp`, `registry.hpp`); not on the in-process `localOp` path, which decodes no JSON. |
| `FieldMeta` | struct | Per-field descriptor: `field`, `label`, `help`, `placeholder`, `widget` (control-selection override, see [Widget hints](#widget-hints--multiline--ranged)), `readOnly`, `hidden`, `i18nKey`, plus the scalar bounds `minimum`/`maximum`/`multipleOf`, and the `withPlaceholder`/`withReadOnly`/`withHidden`/`withMinimum`/`withMaximum`/`withMultipleOf` fluent copies. See "Field metadata" above. |
| `detail::HasFieldMetadata<A>` | concept | `true` when `A` has a `static constexpr`/`static const` iterable `fieldMetadata`. |
| `detail::findFieldMeta<A>(name)` | function | Returns the `FieldMeta` entry naming `name`, or `nullptr`. |
| `detail::BoundCheckableInteger<T>` | concept | `true` for an integral `T` (never `bool`) every value of which is exactly representable as a `Rational` numerator — every signed type, plus every unsigned type narrower than 64 bits. Decides which integral members `allFieldBoundsSatisfied` checks. |
| `detail::satisfiesDeclaredBounds(meta, value)` | constexpr function | `true` when an exact `Rational` is within `meta`'s declared `minimum`/`maximum` and an exact multiple of its `multipleOf`. The single implementation of the bound semantics. |
| `detail::annotateDeclaredBounds(property, meta)` | function | Stamps whichever of `minimum`/`maximum`/`multipleOf` `meta` declares onto one property node. Called from `annotateBasicMemberProperty`, so nested aggregates get the same treatment. |
| `detail::inferTitle(name)` | function | Title-cases a wire key on camelCase/underscore boundaries. |
| `describe<MemberPtr>(label, help)` | function template | Builds a `FieldMeta` whose `field` is resolved from the pointer-to-member `MemberPtr` at runtime. Not `constexpr` — see "Field metadata" above for why, and for the out-of-line declaration a `describe<>()`-based `fieldMetadata` array needs. |

## API reference

### `schemaJson<A>()`

| Signature | Returns |
|---|---|
| `template <typename A> const std::string& schemaJson()` | The merged schema JSON. Cached per type and returned **by reference** (the same shape `model::payloadFingerprint<A>()` and `model::payloadShapeString<A>()` use): the cache is built once per type per process, is never mutated afterwards, and lives until the process exits, so the reference stays valid for as long as any caller could hold it. A caller that needs its own mutable copy asks for one (`std::string mine = schemaJson<A>();`); an *explicit specialisation* of this template must likewise return a reference to something that outlives the call, not to a temporary. On internal failure returns the raw glaze schema, or an empty string if glaze's own schema generation failed — it never throws over malformed *input*. Throws `UnsatisfiableFormError` for a self-contradicting *declaration* ([Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules)). |

### `allRequiredEngaged<A>()`

| Signature | Returns |
|---|---|
| `template <typename A> bool allRequiredEngaged(A const&)` | `true` when every required empty-capable field is engaged. |

### `allFieldBoundsSatisfied<A>()`

| Signature | Returns |
|---|---|
| `template <typename A> bool allFieldBoundsSatisfied(A const&)` | `true` when no `A::fieldMetadata` bound (`minimum`/`maximum`/`multipleOf`) is violated. Trivially `true` for an action declaring no `fieldMetadata`. An unengaged empty-capable field is vacuously satisfied. `noexcept`. See [Per-field scalar bounds](#per-field-scalar-bounds--minimum--maximum--multipleof). |

### `EmptyCapableField<T>` concept

| Signature | Checks |
|---|---|
| `template <typename T> concept EmptyCapableField` | `const T&` has a `noexcept` `.hasValue()` returning convertible-to-`bool`. |

### `Choice<T, OptionsAction, ValueField, LabelField, DependsOn...>` and `FixedString<N>`

Both types are **owned by `choice.hpp` and specified in full in
[choice.md](choice.md)** — this spec does not restate their member-by-member API,
to avoid two copies drifting apart. In brief: `Choice<T, "Action", "value",
"label">` is an optionally-empty value (`std::optional<T>` payload, `hasValue()`,
unchecked `operator*`, defaulted `operator==`) whose options come from executing
a named registered action; `optionsAction()`/`valueField()`/`labelField()`
expose the compile-time metadata that `mergeSchemaExtras` reads to emit
`x-optionsAction`/`x-optionValue`/`x-optionLabel`. An optional trailing
`DependsOn` pack names sibling fields whose current values parameterise the
options action (a cascading picklist); `optionsDependsOn()` exposes it, and
`mergeSchemaExtras` emits `x-optionsDependsOn` only when it is non-empty — an
independent `Choice` (the default) is unaffected. `FixedString<N>` is the
`consteval` NTTP string that carries those names inside the `Choice` type. The
`isChoice<T>` trait (`true` for any cvref-stripped `Choice`) is what
`mergeSchemaExtras` and `allRequiredEngaged` branch on. See [choice.md](choice.md)
for the exhaustive tables and design rationale.

### Cross-field rules

| Symbol | Kind | Purpose |
|---|---|---|
| `EngageableField<T>` | concept | `EmptyCapableField<T>` or `std::optional<...>` — the broader "has an empty state" test the rule vocabulary uses. |
| `Condition<Cond>` | concept | `true` when `Cond` declares `static constexpr bool isCondition = true` — the admission test for a nested condition. `VisibleWhen`/`ReadonlyWhen`/`RequiredWhen` deliberately do not declare it; see [What may be a condition](#what-may-be-a-condition--the-condition-concept). |
| `ComparableAgainstLiteral<V, L>` | concept | `true` when a field of type `V` can be compared against a literal of type `L` at all. Constrains both `equals` overloads, so an incomparable pairing is an error at the call site. |
| `RuleLiteral<L>` | concept | The closed set of literal types `equals` accepts: `std::int64_t`, `bool`, `std::string`, `math::Rational`, and a `FixedString` captured inline. It is what makes a literal serialise losslessly into `x-rules`; a type outside it is rejected at the call site rather than at schema-emission time. |
| `RuleList<Rules...>` | class template | Holds an action's declared rules, in declaration order. Built by `ruleList(...)`; never constructed directly. |
| `ruleList(rules...)` | function template | Composes rule/condition nodes into the `RuleList` an action assigns to `formRules`. |
| `HasFormRules<A>` | concept | `true` when `A` declares a `static constexpr formRules` member. |
| `allRulesSatisfied<A>(action)` | function template | `true` when every **validation** rule in `A::formRules` holds (or there are none); skips presentation rules. `noexcept`. |
| `engaged`/`notEngaged`/`equals`/`greater`/`greaterOrEqual`/`less`/`lessOrEqual`/`requiredWhen`/`exactlyOneOf`/`atLeastOneOf`/`mutuallyExclusive`/`visibleWhen`/`readonlyWhen`/`andOf`/`orOf`/`notOf` | function templates | Factories building one typed rule/condition node each; see the kind table above. |
| `UnsatisfiableFormError` | struct (`std::logic_error`) | Thrown by `schemaJson<A>()` when a capping rule ranges over two or more fields `A` also makes `required`. Its `what()` names the action type, the rule kind, and the offending fields. |
| `detail::capsEngagedCount(kind)` | function | `true` for the emitted rule kinds that impose a ceiling on how many of their fields may be engaged (`"exactlyOneOf"`, `"mutuallyExclusive"`). The single place those kinds are named. |
| `detail::findUnsatisfiableConjunct(nodes, requiredNames)` | function | The first capping node in a *conjunction* of emitted nodes that ranges over two or more names in `requiredNames`, as `(kind, offenders)`. Recurses into an `and` node's `conditions`; deliberately not into `or` or `not`. |
| `detail::rejectUnsatisfiableRules<A>(xRules, requiredNames)` | function template | Throws `UnsatisfiableFormError` for whatever `findUnsatisfiableConjunct` returns over the emitted `x-rules` array. Called from `mergeSchemaExtras`. |
| `detail::ConditionActionType<Cond>` | alias template | The action type `A` a condition/rule node's `test(const A&) const noexcept` ranges over, deduced from `&Cond::test`'s member-function-pointer type. Used by `andOf`/`orOf`/`notOf` to recover `A` without every leaf node separately naming it — and, since the recovery reads the *first* operand only, to require every other operand to agree. |

### `computed<Dst, Inputs...>()` / `computeList()` / `recomputeAll<A>()`

| Signature | Returns |
|---|---|
| `template <auto Dst, auto... Inputs, typename Fn> auto computed(Fn fn)` | A `detail::ComputedField<Dst, Fn, Inputs...>` value. |
| `template <typename... Fields> auto computeList(Fields... fields)` | A `detail::ComputeList<Fields...>` value — assign to `static constexpr auto computedFields`. |
| `template <typename A> void recomputeAll(A& action)` | Overwrites every `A::computedFields` destination in place; a no-op when `A` declares none. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Required default | **All members required unless explicitly opted out** | The safer default for domain forms — forgetting to mark a field optional would leak data, not lose it. Opt out via `std::optional` or `optionalFields` list. |
| Optional mechanism | **Two orthogonal opt-outs** | `std::optional<T>` handles library types (glaze already knows how to serialise them); `optionalFields` handles custom types like `Quantity` whose emptiness is not expressed through `optional`. |
| Schema caching | **`static const std::string` inside the template** | Same schema for the same type in every translation unit. No synchronisation needed — schema generation does not mutate anything. |
| Failure mode | **Returns raw glaze schema (or empty) rather than throwing** | Schema generation is a description facility; crashing a server over a malformed schema would be wrong. |
| Unsatisfiable declaration | **Rejected at generation with `UnsatisfiableFormError`, the one exception to the row above** | A capping rule (`exactlyOneOf`/`mutuallyExclusive`) over two or more `required` fields yields a form *nobody can submit*, not a form missing a hint — the failure is certain at generation time and belongs to the author's build, not to a user who cannot press Save. See [Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules). |
| `Choice` metadata | **In the type, not the payload** | The set of options for a field is a compile-time property of the action, not a runtime property of each submission. The generated schema communicates it to the client; payloads carry only the selected value. |
| Wire serialisation | **Glaze `meta` reflects `value` directly** | `Choice<T, ...>` serialises as `T \| null` — the options metadata never travels. |
| Options action | **A registered action type id** | The same action dispatch mechanism handles queries for picklist data, so no separate protocol or endpoint is needed. |
| Dependent `Choice` options | **Sibling values as the options-action request body, not a new dispatch mechanism** | `Choice`'s `DependsOn` pack only changes what body a renderer sends; the options action stays an ordinary registered action reached through the same `executeJson`/`ActionDispatcher` seam as every other action, so multi-parent cascades and independent `Choice`s coexist with no new framework surface. |
| `x-order` | **Always emitted, on every property** | JSON object key order is not reliable across DOM implementations; the explicit index gives renderers a deterministic layout. |
| Cross-field rules | **Closed, typed vocabulary, one declaration → schema + client + server** | Client and server must evaluate cross-field conditions identically; a closed set of framework-owned node types (not application lambdas) is what makes that possible. Arbitrary logic that does not fit stays in `validate()`/`execute`, unreflected into `x-rules`, exactly as `allRequiredEngaged` already draws the line for per-field required-ness. |
| `x-unitAlternatives` | **Derived from `UnitTraits::relations`** | The same `UnitRelation` entries that drive `convert` also drive the display-unit selector — no separate declaration to keep in sync. |
| `Timestamp` | **Uses standard `"format": "date-time"`** | No extension annotation needed; standard JSON-Schema vocabulary is sufficient. |
| Layout declaration | **`static constexpr formLayout` / `fieldSpans`, mirroring `optionalFields`** | Visual structure is a compile-time property of the action, exactly like the existing opt-out list; a renderer that ignores it degrades to the flat `x-order` form with no missing fields. |
| Widget selection | **Type-derived by default (`Multiline`/`Ranged`), `fieldMetadata`-shaped override wins** | Mirrors the `Choice`/`Quantity` pattern: the control is a compile-time property of the type; the escape hatch is a typed declaration, not a schema-only knob. |
| Widget override lookup | **Duck-typed on `.field`/`.widget`, not a named type** | Keeps `forms.hpp`'s widget lookup free of a hard dependency on any one field-metadata descriptor type declaration; any shape exposing those two members is honoured, `FieldMeta` ([above](#field-metadata--fieldmeta)) included. |
| Computed fields | **One declaration (`computed`/`computeList`) drives schema + client + server via a single shared `recomputeAll`** | The same evaluator runs on the client dispatch paths (`executeJson`, `Bridge::executeVia`) and on every server dispatch path, so the displayed value and the stored value are derived identically — a computed field can never drift, and the server never trusts a client-submitted derivation. |

## Failure modes

### Nested aggregates (recursive, depth-bounded)

A member whose type is itself a reflectable aggregate — a plain nested
struct, or `std::vector<Sub>` (a repeated aggregate) — gets its **own**
members annotated too: `x-order`, `title`/`FieldMeta`, `required`, and the
`Quantity`/`Choice`/widget/ranged-bounds rules the top level already applies.
Unlike the top level, this recurses **into the type graph** — a nested
aggregate's own nested-aggregate member is annotated in turn, and so on, down
to `morph::forms::detail::kMaxNestDepth` (16) levels below the action type —
rather than stopping after one level. This
closes the gap a flat-only generator has for domains that are naturally
nested (a measurement with a repeated specimen sub-record, a document with a
nested address, a category tree), including domains nested more than one
level deep (an address with a nested geo-coordinate sub-record, say).

Two schema shapes exist for a nested aggregate, and both are recursed into:

- **Deduplicated (`$ref`/`$defs`)** — glaze shares one `$defs` entry, `$ref`'d
  from every property, when the nested type is used **two or more times**
  anywhere in the schema. The shared `$defs` entry is annotated once; every
  property that `$ref`s it sees the same annotations.
- **Inlined** — glaze writes the object schema directly into the property
  itself (no `$ref`/`$defs` at all) when the nested type is used **exactly
  once**. The property node itself is annotated in place.

`mergeSchemaExtras` resolves whichever form applies (`annotateNestedAggregateRef`,
`forms.hpp`) and hands the resolved node to the same per-member annotation
logic the top level uses (`annotateBasicMemberProperty`), applied against the
nested type's own reflection. Each recursive step passes along two things: a
**depth counter** as a non-type template parameter, which is what bounds the
recursion, and a **runtime set of the `$defs` keys already annotated**, which
is what keeps a shared nested type from being annotated once per route to it.

Each step also carries the **whole DOM** alongside the node it is annotating,
because resolving a `$ref` means looking its key up under the DOM's `$defs`.
Those two were both plain `glz::generic_u64&` and adjacent in the parameter
list, so transposing them at a call site compiled silently and annotated
against the wrong root — a defect with no diagnostic of any kind. The DOM is
therefore passed as `detail::SchemaDomRef`, a non-owning handle whose only job
is to be a *different type* from a node, which turns that transposition into a
compile error. It is the remedy for what `bugprone-easily-swappable-parameters`
reports on this signature, rather than a suppression of the report.

**Instantiations are per (type, depth), not per route.** The recursion
originally carried the ancestor *chain* as a variadic template parameter pack,
which made `annotateNestedAggregate<Leaf, Ancestors...>` a distinct
instantiation for every distinct root-to-node route through the type graph. A
domain model shaped like a tree has one route per node; a model shaped like a
DAG — an `Address` under both a `Customer` and a `Supplier`, a `Money`
everywhere — has as many as it has paths, and that count grows exponentially in
the graph's depth. A depth counter collapses that to one instantiation per
(type, depth) pair. Measured on a fixture with 27 types over 8 levels, where
6,561 routes reach the deepest node (`tests/compile_checks/forms_dag_probe.cpp`,
g++ 16.2.1, `-std=c++23 -fsyntax-only`, CPU seconds): 26.8 s with the ancestor
chain against a 2.7 s control that has one route per node, and 3.0 s against
the same control with the depth counter. `tests/compile_checks/forms_dag_budget.cmake`
is the ctest guard that keeps it that way, asserting the DAG fixture costs no
more than three times its one-route control (morph#573, Part B).

The `$defs` set is the runtime half of the same observation: a nested
aggregate's annotations are a function of its own type alone, so a shared
`$defs` entry reachable by several routes used to be rewritten with
byte-identical content once per route. It is now annotated by the first route
to reach it, and later routes return immediately. The emitted schema is
unchanged either way — that is what makes the skip safe.

**Nesting past `kMaxNestDepth` is a compile error, not infinite recursion.**
The recursion is driven by the member types themselves, so a cyclic type graph
— a self-referential type such as `struct Node { std::vector<Node> children; };`,
or a mutual reference between two distinct types — would re-enter it forever.
`kMaxNestDepth` (16, in `forms.hpp`) bounds it: the seventeenth level down trips
a `static_assert` at the point that specific instantiation would occur, and the
`if constexpr` around that assert is what stops the deeper instantiation from
being created at all. The message names both possible causes, because a depth
counter cannot tell them apart: a cycle, which cannot be supported at any limit
(the schema it describes has no bottom), or a genuinely acyclic graph that is
simply nested deeper than 16. For a cycle, restructure the domain type — flatten
the self-reference, or represent the recursive edge as an opaque id instead of a
nested value; there is no runtime opt-out. For a graph that really is that deep,
raise `kMaxNestDepth`: it is there to turn a runaway instantiation into a
diagnostic, not to cap legitimate nesting, and raising it costs nothing that is
not actually reached, because instantiations are only created for the (type,
depth) pairs the graph really has.

A "diamond" is not affected — the same type reused from two unrelated places in
the schema, e.g. an `Address` nested under both a `Company` and a `Person`
member of the same action, is at whatever depth each of those members puts it
and recurses normally into both (and, per the `$defs` set above, is annotated
once rather than twice). The `static_assert` only fires where the offending type
is actually reached as a nested-aggregate member of some `schemaJson<A>()` (or
`mergeSchemaExtras<A>()`) instantiation — a self-referential type that is never
nested under an action this way compiles and works fine on its own.

*History:* before morph#573, the bound was the ancestor chain rather than a
depth counter, so the error fired on the cycle itself and named it as one, and
depth was otherwise unlimited. The exchange is deliberate: the ancestor chain
made the instantiation count the *route* count (see above), and no in-tree or
example type nests anywhere near 16 deep — the deepest in this repository is
two.

Computed fields, `formLayout`/`fieldSpans`, and `formRules` remain **top-level
only** regardless of nesting depth: a nested aggregate declaring any of those
has no effect on the generated schema. This keeps the generator focused on
what a nested-aggregate schema actually needs (per-field annotations) rather
than becoming a general recursive-descent schema compiler that also
re-derives layout/rules/computed-field semantics at every level.

**Purely additive, with one source-compatibility exception.** An action with
no nested-aggregate member has nothing here to trigger on, so its generated
schema is byte-for-byte unchanged. A pre-existing action that *does* have a
nested-aggregate member sees its schema gain annotations it previously
lacked — the whole point of this feature — with no change to any of its flat
top-level members. The one exception: an action nested more than
`kMaxNestDepth` levels deep, or with a self- or mutually-referential
nested-aggregate member (see the depth-bound paragraph above), fails to
*compile*. No such action exists in this repo today.

Every nested-aggregate type in the chain must be **default-constructible**,
exactly like the top-level action type (see below): the recursion builds its
own probe instance purely to enumerate its members via reflection.

### Scope: flat actions only (form layout, computed fields, and rules)

`formLayout`/`fieldSpans` ([Layout & grouping](#layout--grouping)) and
`formRules` ([Cross-field rules](#cross-field-rules--the-x-rules-vocabulary)) are read only
from the top-level action type — they are not consulted on a nested
aggregate, no matter how deep `mergeSchemaExtras` otherwise recurses (see
[Nested aggregates (recursive, depth-bounded)](#nested-aggregates-recursive-depth-bounded)
above). Computed fields (`computedFields`) are likewise top-level only.

The action type must also be **default-constructible**: `mergeSchemaExtras`
builds a probe instance (`A probe{}`) purely to enumerate member names and types
via reflection. A type with no accessible default constructor will not compile
`schemaJson<A>()`.

### Total schema failure yields an empty string

`schemaJson<A>()` calls `mergeSchemaExtras<A>(glz::write_json_schema<A>().value_or(std::string{}))`.
When glaze's own schema writer fails, `value_or` hands `mergeSchemaExtras` an
**empty string**; `read_json` then fails on it and the function returns that
same empty string. So a total failure surfaces as `""` — **not** valid JSON, not
an empty JSON object `{}`. Renderers must treat an empty string as "schema
unavailable" and refuse to build a form from it. Because the fallback path
carries no diagnostic, an empty result is **indistinguishable** from any other
failure mode (there is no error code, message, or partial schema to inspect).
The empty string is the only failure signal a caller receives on this path; the
one case in which schema generation throws instead is a self-contradicting
declaration ([Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules)),
which is an author error in the action type, not a failure of the input schema.

## Limitations

### Security / trust boundary

Validation is enforced on **every** dispatch path — client bridge, local
in-process, and the server-side wire dispatcher:

- **`BridgeHandler::executeJson` → `ActionExecuteRegistry`** (the local /
  client-side path a schema-driven GUI uses). Enforces
  `ActionValidator<Action>::ready` after decoding and before invoking the handler
  (see [bridge.md](../core/bridge.md)): an action that fails `validate()` is rejected with
  an error, never executed. It also retags `Quantity` fields to their declared
  precision (below).
- **`Bridge::executeVia`'s `localOp`** (`LocalBackend`, reached by any
  hand-built `Action` passed to `BridgeHandler<Model>::execute<Action>()`
  directly). Enforces the same `ready()` check before `Model::execute`,
  rejecting via `onError` with a `morph::model::ValidationError` (see
  [bridge.md](../core/bridge.md)).
- **`RemoteServer` / `ActionDispatcher`** (the server-side wire path, remote
  mode). `ActionDispatcher::registerAction`'s runner reconciles declared
  `Quantity` precision and enforces `ActionValidator<Action>::ready` before
  `Model::execute` runs, throwing `morph::model::ValidationError` (a
  `std::runtime_error` subclass caught by `RemoteServer`'s strand and turned
  into an `err` reply) when it returns `false` (see [registry.md](../core/registry.md)).

So the schema's `required` array and `allRequiredEngaged` are enforced
consistently on every dispatch path — schema, form, local execution, and the
remote wire path all agree. Validation is **not** authorization, however: a
validated action can still be rejected by `IAuthorizer`, and vice versa — see
[security.md](../security.md) for that separate seam. A model may also still
enforce deeper business rules `validate()` cannot express (cross-entity
constraints, balance checks); `validate()` only covers field-level readiness,
which is why the `examples/forms` model additionally calls
`action.validate()` itself inside `execute(RecordMeasurement)` and throws
`std::invalid_argument` on failure as a defense-in-depth model-level check.

Because `allRulesSatisfied` (above) is typically one of the two conjuncts of
`validate()`, a `formRules` declaration is enforced on exactly the same paths
`ActionValidator<A>::ready` already is — no separate enforcement seam.

### Advertised precision is enforced on dispatch

`x-decimalPlaces` advertises a field's **declared** precision
(`Quantity<U, Dec>::declaredDecimals`), but a `Quantity` on the wire carries its
own runtime `dp`, which a client may set to anything. On the client bridge
dispatch path (`executeJson` → `ActionExecuteRegistry`) these are **reconciled**:
after decoding and before dispatch, `morph::forms::reconcileDeclaredPrecision`
**rounds** every `Quantity` member of the action to `declaredPrecision()`, so the
value the handler stores is at the precision the schema advertised, not at the
client's submitted `dp`. An empty `Quantity` stays empty. The reconciliation is a
no-op for actions with no `Quantity` members and for action types glaze cannot
reflect. `ActionDispatcher::registerAction`'s runner performs the same
reconciliation on the server-side wire path (see
[registry.md](../core/registry.md)), so `x-decimalPlaces` is an enforced contract
on **both wire paths** — client-bridge and remote.

**Rounding, not retagging — and why the difference is the whole point.**
`Quantity::atDeclaredPrecision()` performs an exact `Rational` re-rounding
(`math::roundToDecimalPlaces`, half away from zero, matching the decimal
formatter — see
[rational.md](../util/rational.md#roundtodecimalplaces--the-one-helper-that-stays-in-the-domain)).
It does **not** merely move the `DecimalPlaces` tag. A tag-only change would
leave a field declaring `dp = 1` holding exactly `1.23456` while rendering
`1.2`: the report says one number and the database another, and an audit trail
cannot say which one the operator saw when they signed off. For a framework whose
premise is exact values for financial and lab data, that failure mode is worse
than not enforcing at all — the value *looks* compliant. Enforcement therefore
means the submitted precision beyond the declared amount is **discarded, not
hidden**, and the reconciled value is by construction the value the form was
already displaying.

The operation normalises rather than rejecting an over-precise submission. Two
reasons. A wire `dp` finer than the declared one is not by itself a protocol
violation — `{"num":6,"den":5,"dp":5}` is exactly `1.2`, perfectly representable
at `dp = 1`, and rejecting it would fail a payload with nothing wrong in it;
detecting the genuinely over-precise case means testing the *value*, not the tag.
More decisively, the same `atDeclaredPrecision()` call is what `recomputeOne`
applies to **server-derived** values (see [Computed fields](#computed-fields)),
which routinely carry more decimals than the destination field declares — a
product of two 2-decimal operands is exact to 4. A reject-shaped contract would
have the server reject its own arithmetic. Normalising is the only rule both call
sites can share.

**The in-process path is deliberately not reconciled.** `Bridge::executeVia`'s
`localOp` decodes no JSON — it dispatches an already-typed `Action` a caller
constructed in C++ — so there is no client-supplied `dp` to reconcile and no
reconciliation step there (`bridge.hpp` says so at the call site; cross-ref
[quantity_type.md](../util/quantity_type.md), which describes the same asymmetry
for `enforceQuantityBounds`). A `Quantity` built by calling code keeps whatever
precision the caller gave it. Computed fields *are* still normalised on that
path, since `recomputeAll` runs there and `recomputeOne` rounds to the
destination's declared precision.

### Pre-decode wire validation — `checkQuantityBounds`

Reconciling declared precision (above) still leaves a wire payload that is
merely *representable*, not necessarily *physically or contractually
sensible* — a percentage of `250`, a mass of `-5`. `morph::forms::
checkQuantityBounds<A>(action)` closes that gap: it walks every reflected
`Quantity` member of a decoded action and checks
`Quantity::withinDeclaredBounds()` (see
[quantity_type.md, "Pre-decode wire validation"](../util/quantity_type.md#pre-decode-wire-validation--declared-bounds))
against the optional `UnitTraits<E>::bounds(E)` a unit may declare. It returns
the wire name of the first offending field, or `std::nullopt` — a no-op for
actions with no `Quantity` members, or whose units declare no bounds.
`morph::forms::enforceQuantityBounds<A>(action)` is the throwing counterpart,
raising `morph::forms::QuantityDecodeError` naming that field.

Both wire-decoding dispatch runners — `ActionDispatcher::registerAction`'s
server-side runner and `ActionExecuteRegistry::registerAction`'s client
bridge runner (see [registry.md](../core/registry.md)/[bridge.md](../core/bridge.md))
— call `enforceQuantityBounds` immediately after `reconcileDeclaredPrecision`
and before `recomputeAll`/`ActionValidator<A>::ready`, so an out-of-bounds
wire value is rejected **before an action's own `validate()` ever sees it**.
`QuantityDecodeError` is deliberately **not** `morph::model::ValidationError`
— the two error types stay distinct so a caller can tell "the wire payload
itself was impossible" (a decode-level, framework-enforced constraint) from
"the decoded action failed its own business rule" (a `validate()`-level
rejection an action author wrote). The in-process `Bridge::executeVia`
`localOp` path is unaffected — no JSON decode happens there (see "Advertised
precision is enforced on dispatch" above for why `reconcileDeclaredPrecision`
is likewise skipped on that path), so a `Quantity` a caller constructs
directly carries whatever value the caller gave it, unchecked at this seam.

### Sum types not in the forms palette — multi-field encoding by design

The forms vocabulary provides no native sum-type (tagged union, discriminated union) support. When an action field must express *one of several alternatives* (e.g. a measurement that is "a quantity, or below limit-of-detection, or above upper detection limit"), encode it as a **multi-field structure glued by cross-field rules**: one field for the quantity, one boolean or enum for the state (measured/below/above), and a `RequiredWhen`/`VisibleWhen` rule that gates each based on the others. This is by design: sum types are rare in domain models that already use `hasValue()` optionality and `Choice` enums, and the rule-based multi-field encoding is expressive enough for the rungs' needs while keeping the schema and validation machinery focused.

The encoding carries one obligation: every field the capping rule ranges over must be named in `A::optionalFields`, so the rule is the *only* gate on them. Omitting that leaves `required` demanding every alternative at once, which contradicts the rule — `schemaJson<A>()` rejects it rather than serving an unsubmittable form ([Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules)).

### Per-instance variation is limited to key *values*

`schemaJson<A>()` derives every key from the compiled type, so two rows that
describe the same action differently cannot be told apart by it.
`InstanceConstraints` (above) lifts that for the keys it covers —
`x-decimalPlaces` and the exact bounds — but only for their *values*. Which
fields exist, the `required` array and the `x-rules` list remain functions of
`A`, so a definition wanting a different *shape* still needs a recompile, and
an application with per-definition shapes needs one compiled action per family
of shapes rather than one per definition. Applying an instance constraint also
stays the model's job rather than the dispatch runner's; see
[instance_constraints.md](instance_constraints.md), "Why dispatch cannot apply
these automatically", for why neither of those is an oversight.

### One cached schema per type — no localisation

Each type's schema is memoised in a function-local `static const std::string`
(`schemaJson<A>()`), computed by the first caller and shared process-wide
thereafter (first-caller-wins; no synchronisation, since generation mutates
nothing). This baking-in **precludes localised / i18n schemas**: the human
display strings that land in the schema (unit `display`/`unitUnicode`, and any
`description` text) are fixed at first call. There is no per-request or
per-locale schema variant — a translated form would need a different mechanism
entirely. See [Localisation — message keys and the catalog seam](#localisation--message-keys-and-the-catalog-seam)
for that mechanism.

### Load-bearing assumptions about glaze's schema shape

The generator is coupled to concrete details of glaze's schema **output shape**,
not just its public API:

- **A top-level `properties` object.** `mergeSchemaExtras` indexes
  `dom["properties"][name]` directly. If glaze stopped emitting a `properties`
  object at the root (or nested it), the merge would create the wrong structure.
- **`$defs` numeric-bound preservation via `generic_u64`.** The DOM is parsed in
  u64 number mode specifically because `$defs` carries `int64`/`uint64` bounds
  that the default double-only DOM would silently round. This depends on glaze
  emitting those bounds as integers in a place the round-trip preserves.
- **Reflection key order feeding `x-order`.** `x-order` is the index `I` from
  `glz::reflect<A>::keys`, and it is trusted to equal source **declaration
  order**. If glaze's reflection reordered keys, `x-order` would misdescribe the
  layout while still looking well-formed.
- **A `glz::enumerate`d `enum class` as a `oneOf` of `const`s.** A renderer
  recognises a closed set by that shape alone (see
  [Closed sets](#closed-sets--a-reflected-enum-class)); morph stamps no key of
  its own to mark it. If glaze emitted such an enum some other way — a bare
  `enum` array is read too, but a third spelling would not be — the member
  would silently fall back to a free-text field.

A change to any of these in glaze could make the generator silently produce a
wrong or un-merged schema rather than fail loudly.

## Cross-references

| Spec | Why |
|---|---|
| [choice.md](choice.md) | Full `Choice` API and design (this spec cross-refs rather than duplicates it). |
| [instance_constraints.md](instance_constraints.md) | `InstanceConstraints` — serving and checking the framework-meaningful keys whose values live in data rather than in the compiled type. |
| [workflows_navigation.md](workflows_navigation.md) | The wizard/app-shell layer built on this schema — one wizard step or one `kind: "form"` app screen still renders as an ordinary action form. |
| [views.md](views.md) | The view-schema layer (`morph::views`) that composes query+edit+delete action *sets* into list/table and master-detail screens; reuses `schemaJson<Row>()` unmodified to derive each column's `ExtUnits`/`x-decimalPlaces`. |
| [widget_hints.md](widget_hints.md) | Full `Multiline`/`Ranged` API and design (this spec cross-refs rather than duplicates it). |
| [quantity_type.md](../util/quantity_type.md) | `Quantity`, its unit tags, `UnitTraits::relations`, and `convert` — the source of `x-decimalPlaces`, `x-unitAlternatives`, and `ExtUnits`. |
| [datetime.md](../util/datetime.md) | `DateTime` / `Timestamp`, the ISO-8601 wire format, and the `"format": "date-time"` schema annotation. |
| [rational.md](../util/rational.md) | Exact `Rational` values; the `num`/`den` in each `x-unitAlternatives` entry are a `Rational` numerator/denominator, which is why unit switches recompute exactly. Also the comparison/equality `greater`/`greaterOrEqual`/`less`/`lessOrEqual`/`equals` use for numeric fields, so client and server compare identical values. |
| [security.md](../security.md) | The dispatcher's trust boundary — why `required` gates only the client and handlers must re-validate. |
| [session.md](../session/session.md) | `Context::locale`, the server-side hook for data (not chrome) localisation — the one place `session::current()->locale` participates, for `Choice` option-row labels. |
| [bridge.md](../core/bridge.md) | The `ActionExecuteRegistry`/`executeVia` authoritative recompute sites. |
| [registry.md](../core/registry.md) | `ActionDispatcher::registerAction`'s runner — the server-side authoritative recompute site. |

## Out of scope

- Generating schemas for non‑action types (the module assumes glaze reflection is
  available on `A`).
- Validating payloads against the schema — the schema is for the *client*.
- Executing the options action — `choices` metadata tells the client *which*
  action to call, but the forms module does not invoke it.
- `morph::time::Timestamp` definition — it is only consumed here via
  `EmptyCapableField`.