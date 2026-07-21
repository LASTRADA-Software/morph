# `morph::forms` — schema generation & readiness for action types

Given a plain-aggregate action type `A` (registered with `BRIDGE_REGISTER_ACTION`),
`morph::forms` produces a standard JSON Schema a client can render a form from,
and provides a compile-time `validate()` body that gates submission until every
required empty-capable field is filled in. It builds on glaze's
`write_json_schema<A>` (which already contributes types, `$defs`, per-field
metadata from `glz::json_schema<A>`, and `ExtUnits` from
`morph::units::Quantity`) and closes the gaps glaze leaves open.

## Contents

- [Empty state — `EmptyCapableField` concept](#empty-state--emptycapablefield-concept)
- [`Choice` — server-sourced picklist](#choice--server-sourced-picklist)
- [`FixedString` — NTTP compile-time string](#fixedstring--nttp-compile-time-string)
- [`schemaJson<A>()` — schema generation](#schemajsona--schema-generation)
- [Field metadata — `FieldMeta`](#field-metadata--fieldmeta)
- [Renderer contract: the schema key vocabulary](#renderer-contract-the-schema-key-vocabulary)
- [`allRequiredEngaged<A>()` — readiness check](#allrequiredengageda--readiness-check)
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
module needs to know about a field to decide whether it counts as "engaged".

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
          FixedString ValueField = "id", FixedString LabelField = "name">
struct Choice {
    std::optional<T> value;
    // ...
};
```

- `T` — the value type submitted on the wire (`int64_t` for ids, `string` for codes).
- `OptionsAction` — the registered action type id whose result provides options
  (executed with an empty body, returns `{valueField, labelField, ...}` rows).
- `ValueField` / `LabelField` — which result-row fields carry the submitted value
  and the display label; both default to `"id"` / `"name"`.

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

## `schemaJson<A>()` — schema generation

Produces a complete JSON Schema string for action type `A`, post-processing the
output of `glz::write_json_schema<A>()` to add five annotation groups:

| Annotation | Scope | Contents |
|---|---|---|
| `required` | Top-level | Array of field names that are **not** `std::optional<...>` and not listed in `A::optionalFields`. |
| `x-order` | Every property | The member's declaration index (0‑based), so a renderer lays fields out in declaration order regardless of JSON key ordering. |
| `x-decimalPlaces` | `Quantity` properties | The field's declared precision (`Quantity<U, Dec>::declaredDecimals`). |
| `x-unitAlternatives` | `Quantity` properties | Convertible display/entry units derived from `UnitTraits::relations`, each with `{id, display, decimals, num, den}` — `id`/`display`/`decimals` come from the alternative unit's `UnitMeta`, and `num`/`den` are the exact alternative-to-canonical ratio. Omitted entirely when the field's unit declares no convertible units. |
| `x-optionsAction` / `x-optionValue` / `x-optionLabel` | `Choice` properties | The action that serves the options and which result fields to use. |

The result is **computed once per type and cached** in a `static const std::string`
inside `schemaJson<A>()`. On internal failure (malformed intermediate JSON,
etc.) the unmerged glaze schema is returned — or an empty string when even
glaze's own `write_json_schema<A>()` failed, since `schemaJson` feeds
`mergeSchemaExtras` with `write_json_schema<A>().value_or(std::string{})`.
Schema generation never throws.

### Required-ness rule

Required is the default. A member is *optional* (and therefore not added to
`required`) when either:
1. Its type is `std::optional<...>`, or
2. Its name appears in `A::optionalFields` — a `static constexpr` iterable of
   `std::string_view` that the action declares:

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
    std::string_view widget{};              // reserved for the widget-hints spec
    bool readOnly{false};
    bool hidden{false};
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

All five keys are additive and non-breaking, extending the renderer-contract
table below without renaming or retyping any existing key, per this program's
versioning stance ([gui_overview.md](../../planned/gui_overview.md)). A
renderer that ignores them falls back to today's behavior exactly: it shows
the raw wire key as the caption, no helper/placeholder text, and every field
editable and visible.

## Renderer contract: the schema key vocabulary

This is the **normative** list of every key a renderer must understand to build
a form from a morph action schema. Standard JSON-Schema keywords (`type`,
`properties`, `$defs`, `$ref`, numeric bounds, …) are emitted by
glaze and behave per the JSON-Schema 2020-12 spec; the table below covers the
keys morph either **synthesises** (`required`, `title`, `description` when a
`FieldMeta::help` is declared, the `x-*` extensions) or **relies on glaze to
stamp** (`format`, `ExtUnits`, `description` when no `FieldMeta::help` overrides
it). A renderer that ignores an `x-*` key
still produces a usable form — it just loses the affordance that key carries
(unit selector, field order, combo box, decimal step).

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
  `x-optionValue` / `x-optionLabel` are siblings of the `$ref` on the property**
  — `mergeSchemaExtras` patches `dom["properties"][name]`, which is the property
  node holding the `$ref`, never the referenced `$def`.

The **"Where"** column below names the node each key is written to. A renderer
resolves the `$ref` into `$defs`, then merges: per-property `x-*` keys (from the
property node) win, and `ExtUnits` (plus glaze's `type`/bounds/`description`) come
from the resolved def. The `examples/forms` QML renderer's `resolveProp` does
exactly this dual read.

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `required` | top-level (object) | array of strings | Names of members that must be engaged before submit. A member is listed unless it is a `std::optional<...>` or appears in `A::optionalFields`. Always emitted (an explicit `[]` when nothing is required). The renderer blocks submission until every listed field has a value. |
| `x-order` | property node (sibling of `$ref`) | non-negative integer | The member's 0-based **declaration index**. Renderers lay fields out in ascending `x-order`, not in JSON key order (object key order is not preserved across DOMs). |
| `x-decimalPlaces` | property node (sibling of `$ref`) | non-negative integer | The field's *declared* precision (`Quantity<U, Dec>::declaredDecimals`, unit default unless the type overrides it). The numeric input step / rounding granularity for entry in the canonical unit. **Enforced, not merely advisory:** the request/reply dispatch path retags each submitted `Quantity` to this precision before storing it (see [Advertised precision is enforced on dispatch](#advertised-precision-is-enforced-on-dispatch)). |
| `x-unitAlternatives` | property node (sibling of `$ref`) | array of objects | Convertible display/entry units for the field, derived from `UnitTraits<E>::relations`. **Omitted entirely** when the unit declares no convertible peers. Each element has the five subfields below. The renderer offers these as a unit selector and recomputes the entered value *exactly* on switch; the submitted payload is always in the canonical unit (the one named by `ExtUnits`). |
| ↳ `id` | alternative entry | string | Stable ascii id of the alternative unit (`UnitMeta::id`). |
| ↳ `display` | alternative entry | string | Human display text of the alternative unit (`UnitMeta::display`). |
| ↳ `decimals` | alternative entry | non-negative integer | The alternative unit's own default decimals (`UnitMeta::defaultDecimals`) — the input step to use while that unit is selected. |
| ↳ `num` | alternative entry | signed integer | Numerator of the exact **alternative→canonical** ratio. |
| ↳ `den` | alternative entry | signed integer | Denominator of that ratio. `value_in_canonical = value_in_alternative · num / den`; `num`/`den` are the `Rational` numerator/denominator of the composed relation, so the recompute is exact (no floating-point drift). |
| `x-optionsAction` | property node (sibling of `$ref`) | string | Type id of the registered action whose result rows populate this field's combo box (executed with an empty body). |
| `x-optionValue` | property node (sibling of `$ref`) | string | Which result-row field carries the value submitted on the wire (default `"id"`). |
| `x-optionLabel` | property node (sibling of `$ref`) | string | Which result-row field carries the display label (default `"name"`). |
| `title` | property node (sibling of `$ref`) | string | The field's display label — an explicit `FieldMeta::label`, else a title-cased member name (`dryMassPct` → "Dry Mass Pct"). Standard JSON-Schema vocabulary, not an `x-*` key. **Always emitted.** See "Field metadata" above. |
| `x-placeholder` | property node (sibling of `$ref`) | string | In-control placeholder/hint shown while the field is empty, from `FieldMeta::placeholder`. Omitted when empty; never submitted. |
| `x-readonly` | property node (sibling of `$ref`) | boolean | `true` when the field should be displayed but not editable. Emitted only when `true`. Not a security control — see "Field metadata is not a security control" above. |
| `x-hidden` | property node (sibling of `$ref`) | boolean | `true` when the field should not be shown at all; the field remains part of the action payload. Emitted only when `true`. Not a security control. |
| `format` | `Timestamp` property (or its `$def`) | string, value `"date-time"` | Standard JSON-Schema vocabulary (stamped by glaze, not by morph). The renderer shows a date-time input; the wire value is the ISO-8601 string `Timestamp` serialises to. No `x-*` extension is used for timestamps. |
| `ExtUnits` | `$def` of the `Quantity`'s unit type (reached via the property's `$ref`) | object | Glaze-stamped block describing the field's **canonical** unit. Two fields: `unitAscii` (the stable ascii id, e.g. `"kg_per_m3"` — sourced from `UnitMeta::id`) and `unitUnicode` (the human display text, e.g. `"kg/m³"` — from `UnitMeta::display`). This is the unit a payload value is always denominated in, and the reference point the `num`/`den` of every `x-unitAlternatives` entry converts *to*. A renderer resolves the property's `$ref` into `$defs` to read `ExtUnits.unitAscii`/`unitUnicode` (it is **not** on the property node next to the `x-*` keys) to label the field and anchor the unit selector. |

### Versioning stance

The emitted schema is **unversioned**. There is no `$id`, `$schema` version
marker, or morph-specific version field anywhere in the output — a renderer
cannot detect at runtime which revision of this vocabulary a schema was produced
against. The vocabulary is therefore treated as a stable framework contract:
**changing the semantics of any key above (renaming it, changing its type, or
altering how a value is interpreted) is a breaking change and ships only in a
breaking framework release.** Adding a new, optional `x-*` key that older
renderers can safely ignore is not breaking.

## `allRequiredEngaged<A>()` — readiness check

```cpp
template <typename A>
[[nodiscard]] constexpr bool allRequiredEngaged(const A& action) noexcept;
```

Returns `true` when every **required** empty-capable member of `action` has
`hasValue() == true`. Required means: not `std::optional<...>` and not listed
in `A::optionalFields`. Non-empty-capable members (plain ints, strings, etc.)
are skipped — they cannot express "not filled in". Intended as the body of the
action's `validate()` (the `ActionValidator` machinery picks it up
automatically).

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
**own top-level members** — the same flat-actions-only scope as schema
generation ([Scope: flat actions only](#scope-flat-actions-only)); it does not
recurse into nested aggregates.

## Support traits and helpers

| Symbol | Kind | Purpose |
|---|---|---|
| `detail::IsStdOptional<T>` | trait | `true` when `T` is a `std::optional<...>`. |
| `detail::isStdOptional<T>` | variable template | cvref-stripped alias of the trait. |
| `detail::HasOptionalFields<A>` | concept | `true` when `A` has a `static constexpr` iterable `optionalFields`. |
| `detail::declaredOptional<A>(name)` | constexpr function | `true` when `name` appears in `A::optionalFields`. |
| `detail::forEachNamedMember(action, visitor)` | function template | Calls `visitor.operator()<I>(name, member)` for every reflected member of `action` (uses glaze pure reflection). |
| `detail::mergeSchemaExtras<A>(raw)` | function | Post-processes a glaze-generated schema to inject `required`, `x-decimalPlaces`, `x-order`, `x-unitAlternatives`, `x-optionsAction`, `title`, `description`/`x-placeholder`/`x-readonly`/`x-hidden` etc. onto the property nodes. Called by `schemaJson<A>()`. |
| `reconcileDeclaredPrecision<A>(action)` | function | Retags every `Quantity` member of `action` in place to its declared precision (`atDeclaredPrecision()`), so a decoded wire value matches the schema's advertised `x-decimalPlaces`. No-op for non-`Quantity` members and for action types glaze cannot reflect. Called on the `executeJson` dispatch path (`bridge.hpp`). |
| `FieldMeta` | struct | Per-field presentation descriptor: `field`, `label`, `help`, `placeholder`, `widget` (reserved), `readOnly`, `hidden`, plus `withPlaceholder`/`withReadOnly`/`withHidden` fluent copies. See "Field metadata" above. |
| `detail::HasFieldMetadata<A>` | concept | `true` when `A` has a `static constexpr`/`static const` iterable `fieldMetadata`. |
| `detail::findFieldMeta<A>(name)` | function | Returns the `FieldMeta` entry naming `name`, or `nullptr`. |
| `detail::inferTitle(name)` | function | Title-cases a wire key on camelCase/underscore boundaries. |
| `describe<MemberPtr>(label, help)` | function template | Builds a `FieldMeta` whose `field` is resolved from the pointer-to-member `MemberPtr` at runtime. Not `constexpr` — see "Field metadata" above for why, and for the out-of-line declaration a `describe<>()`-based `fieldMetadata` array needs. |

## API reference

### `schemaJson<A>()`

| Signature | Returns |
|---|---|
| `template <typename A> std::string schemaJson()` | The merged schema JSON. Cached per type. Never throws. On internal failure returns the raw glaze schema, or an empty string if glaze's own schema generation failed. |

### `allRequiredEngaged<A>()`

| Signature | Returns |
|---|---|
| `template <typename A> bool allRequiredEngaged(A const&)` | `true` when every required empty-capable field is engaged. |

### `EmptyCapableField<T>` concept

| Signature | Checks |
|---|---|
| `template <typename T> concept EmptyCapableField` | `const T&` has a `noexcept` `.hasValue()` returning convertible-to-`bool`. |

### `Choice<T, OptionsAction, ValueField, LabelField>` and `FixedString<N>`

Both types are **owned by `choice.hpp` and specified in full in
[choice.md](choice.md)** — this spec does not restate their member-by-member API,
to avoid two copies drifting apart. In brief: `Choice<T, "Action", "value",
"label">` is an optionally-empty value (`std::optional<T>` payload, `hasValue()`,
unchecked `operator*`, defaulted `operator==`) whose options come from executing
a named registered action; `optionsAction()`/`valueField()`/`labelField()`
expose the compile-time metadata that `mergeSchemaExtras` reads to emit
`x-optionsAction`/`x-optionValue`/`x-optionLabel`. `FixedString<N>` is the
`consteval` NTTP string that carries those names inside the `Choice` type. The
`isChoice<T>` trait (`true` for any cvref-stripped `Choice`) is what
`mergeSchemaExtras` and `allRequiredEngaged` branch on. See [choice.md](choice.md)
for the exhaustive tables and design rationale.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Required default | **All members required unless explicitly opted out** | The safer default for domain forms — forgetting to mark a field optional would leak data, not lose it. Opt out via `std::optional` or `optionalFields` list. |
| Optional mechanism | **Two orthogonal opt-outs** | `std::optional<T>` handles library types (glaze already knows how to serialise them); `optionalFields` handles custom types like `Quantity` whose emptiness is not expressed through `optional`. |
| Schema caching | **`static const std::string` inside the template** | Same schema for the same type in every translation unit. No synchronisation needed — schema generation does not mutate anything. |
| Failure mode | **Returns raw glaze schema (or empty) rather than throwing** | Schema generation is a description facility; crashing a server over a malformed schema would be wrong. |
| `Choice` metadata | **In the type, not the payload** | The set of options for a field is a compile-time property of the action, not a runtime property of each submission. The generated schema communicates it to the client; payloads carry only the selected value. |
| Wire serialisation | **Glaze `meta` reflects `value` directly** | `Choice<T, ...>` serialises as `T \| null` — the options metadata never travels. |
| Options action | **A registered action type id** | The same action dispatch mechanism handles queries for picklist data, so no separate protocol or endpoint is needed. |
| `x-order` | **Always emitted, on every property** | JSON object key order is not reliable across DOM implementations; the explicit index gives renderers a deterministic layout. |
| `x-unitAlternatives` | **Derived from `UnitTraits::relations`** | The same `UnitRelation` entries that drive `convert` also drive the display-unit selector — no separate declaration to keep in sync. |
| `Timestamp` | **Uses standard `"format": "date-time"`** | No extension annotation needed; standard JSON-Schema vocabulary is sufficient. |

## Failure modes

### Scope: flat actions only

Annotation and `required`-derivation operate **exclusively on the action's
top-level members**. `mergeSchemaExtras` reflects `A`'s members with
`forEachNamedMember(probe, …)` and patches `dom["properties"][name]` for each —
it never descends into member types. A member that is itself an aggregate is
emitted by glaze into `$defs` and referenced by `$ref`; the generator does not
recurse into that definition, so its sub-members receive **none** of the `x-*`
annotations and are **not** part of any synthesised `required` array (the nested
`$def` gets no `required` at all). Actions meant to drive a generated form must
therefore be **flat**: every field the renderer should understand has to be a
direct member of the action type. Nesting is not a documented form-generation
path.

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
Schema generation never throws, so the empty string is the only failure signal a
caller receives.

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

### Advertised precision is enforced on dispatch

`x-decimalPlaces` advertises a field's **declared** precision
(`Quantity<U, Dec>::declaredDecimals`), but a `Quantity` on the wire carries its
own runtime `dp`, which a client may set to anything. On the client bridge
dispatch path (`executeJson` → `ActionExecuteRegistry`) these are **reconciled**:
after decoding and before dispatch, `morph::forms::reconcileDeclaredPrecision`
retags every `Quantity` member of the action to `declaredPrecision()` (an exact
`Rational` re-rounding — an empty `Quantity` stays empty), so the value the
handler stores is at the precision the schema advertised, not at the client's
submitted `dp`. This makes `x-decimalPlaces` an enforced contract on that path
rather than an advisory hint. The reconciliation is a no-op for actions with no
`Quantity` members and for action types glaze cannot reflect.
`ActionDispatcher::registerAction`'s runner performs the same reconciliation on
the server-side wire path (see [registry.md](../core/registry.md)), so
`x-decimalPlaces` is now an enforced contract on every dispatch path — local,
client-bridge, and remote wire.

### One cached schema per type — no localisation

Each type's schema is memoised in a function-local `static const std::string`
(`schemaJson<A>()`), computed by the first caller and shared process-wide
thereafter (first-caller-wins; no synchronisation, since generation mutates
nothing). This baking-in **precludes localised / i18n schemas**: the human
display strings that land in the schema (unit `display`/`unitUnicode`, and any
`description` text) are fixed at first call. There is no per-request or
per-locale schema variant — a translated form would need a different mechanism
entirely.

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

A change to any of these in glaze could make the generator silently produce a
wrong or un-merged schema rather than fail loudly.

## Cross-references

| Spec | Why |
|---|---|
| [choice.md](choice.md) | Full `Choice` API and design (this spec cross-refs rather than duplicates it). |
| [quantity_type.md](../util/quantity_type.md) | `Quantity`, its unit tags, `UnitTraits::relations`, and `convert` — the source of `x-decimalPlaces`, `x-unitAlternatives`, and `ExtUnits`. |
| [datetime.md](../util/datetime.md) | `DateTime` / `Timestamp`, the ISO-8601 wire format, and the `"format": "date-time"` schema annotation. |
| [rational.md](../util/rational.md) | Exact `Rational` values; the `num`/`den` in each `x-unitAlternatives` entry are a `Rational` numerator/denominator, which is why unit switches recompute exactly. |
| [security.md](../security.md) | The dispatcher's trust boundary — why `required` gates only the client and handlers must re-validate. |

## Out of scope

- Generating schemas for non‑action types (the module assumes glaze reflection is
  available on `A`).
- Validating payloads against the schema — the schema is for the *client*.
- Executing the options action — `choices` metadata tells the client *which*
  action to call, but the forms module does not invoke it.
- `morph::time::Timestamp` definition — it is only consumed here via
  `EmptyCapableField`.