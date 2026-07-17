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
    { field.hasValue() } -> std::convertible_to<bool>;
};
```

Satisfied by:
- `morph::units::Quantity<U, Dec>` — `hasValue()` returns `true` when the
  `Rational` payload is present.
- `morph::forms::Choice<T, ...>` — `hasValue()` returns `true` when its
  `std::optional<T>` is engaged.
- `morph::time::Timestamp` — `hasValue()` returns `true` when its `DateTime`
  payload is present.
- Any user type that exposes `bool hasValue() const`.

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

## Renderer contract: the schema key vocabulary

This is the **normative** list of every key a renderer must understand to build
a form from a morph action schema. Standard JSON-Schema keywords (`type`,
`properties`, `$defs`, `$ref`, numeric bounds, `description`, …) are emitted by
glaze and behave per the JSON-Schema 2020-12 spec; the table below covers the
keys morph either **synthesises** (`required`, the `x-*` extensions) or **relies
on glaze to stamp** (`format`, `ExtUnits`). A renderer that ignores an `x-*` key
still produces a usable form — it just loses the affordance that key carries
(unit selector, field order, combo box, decimal step).

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `required` | top-level (object) | array of strings | Names of members that must be engaged before submit. A member is listed unless it is a `std::optional<...>` or appears in `A::optionalFields`. Always emitted (an explicit `[]` when nothing is required). The renderer blocks submission until every listed field has a value. |
| `x-order` | every property | non-negative integer | The member's 0-based **declaration index**. Renderers lay fields out in ascending `x-order`, not in JSON key order (object key order is not preserved across DOMs). |
| `x-decimalPlaces` | `Quantity` property | non-negative integer | The field's *declared* precision (`Quantity<U, Dec>::declaredDecimals`, unit default unless the type overrides it). The numeric input step / rounding granularity for entry in the canonical unit. |
| `x-unitAlternatives` | `Quantity` property | array of objects | Convertible display/entry units for the field, derived from `UnitTraits<E>::relations`. **Omitted entirely** when the unit declares no convertible peers. Each element has the five subfields below. The renderer offers these as a unit selector and recomputes the entered value *exactly* on switch; the submitted payload is always in the canonical unit (the one named by `ExtUnits`). |
| ↳ `id` | alternative entry | string | Stable ascii id of the alternative unit (`UnitMeta::id`). |
| ↳ `display` | alternative entry | string | Human display text of the alternative unit (`UnitMeta::display`). |
| ↳ `decimals` | alternative entry | non-negative integer | The alternative unit's own default decimals (`UnitMeta::defaultDecimals`) — the input step to use while that unit is selected. |
| ↳ `num` | alternative entry | signed integer | Numerator of the exact **alternative→canonical** ratio. |
| ↳ `den` | alternative entry | signed integer | Denominator of that ratio. `value_in_canonical = value_in_alternative · num / den`; `num`/`den` are the `Rational` numerator/denominator of the composed relation, so the recompute is exact (no floating-point drift). |
| `x-optionsAction` | `Choice` property | string | Type id of the registered action whose result rows populate this field's combo box (executed with an empty body). |
| `x-optionValue` | `Choice` property | string | Which result-row field carries the value submitted on the wire (default `"id"`). |
| `x-optionLabel` | `Choice` property | string | Which result-row field carries the display label (default `"name"`). |
| `format` | `Timestamp` property | string, value `"date-time"` | Standard JSON-Schema vocabulary (stamped by glaze, not by morph). The renderer shows a date-time input; the wire value is the ISO-8601 string `Timestamp` serialises to. No `x-*` extension is used for timestamps. |
| `ExtUnits` | `Quantity` property | object | Glaze-stamped block describing the field's **canonical** unit. Two fields: `unitAscii` (the stable ascii id, e.g. `"kg_per_m3"` — sourced from `UnitMeta::id`) and `unitUnicode` (the human display text, e.g. `"kg/m³"` — from `UnitMeta::display`). This is the unit a payload value is always denominated in, and the reference point the `num`/`den` of every `x-unitAlternatives` entry converts *to*. A renderer needs `ExtUnits.unitAscii`/`unitUnicode` to label the field and to anchor the unit selector. |

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

## Support traits and helpers

| Symbol | Kind | Purpose |
|---|---|---|
| `detail::IsStdOptional<T>` | trait | `true` when `T` is a `std::optional<...>`. |
| `detail::isStdOptional<T>` | variable template | cvref-stripped alias of the trait. |
| `detail::HasOptionalFields<A>` | concept | `true` when `A` has a `static constexpr` iterable `optionalFields`. |
| `detail::declaredOptional<A>(name)` | constexpr function | `true` when `name` appears in `A::optionalFields`. |
| `detail::forEachNamedMember(action, visitor)` | function template | Calls `visitor.operator()<I>(name, member)` for every reflected member of `action` (uses glaze pure reflection). |
| `detail::mergeSchemaExtras<A>(raw)` | function | Post-processes a glaze-generated schema to inject `required`, `x-decimalPlaces`, `x-order`, `x-unitAlternatives`, `x-optionsAction` etc. Called by `schemaJson<A>()`. |

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
| `template <typename T> concept EmptyCapableField` | `const T&` has `.hasValue()` returning convertible-to-`bool`. |

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

`required`-ness and `allRequiredEngaged` gate the **client only**. The morph
dispatcher runs **no server-side validators** before invoking a handler — it
does not consult the schema's `required` array and does not call `validate()`.
Consequently a hand-crafted wire payload that omits or blanks a "required" field
is accepted by the wire/dispatch layer and reaches the handler unmodified: the
`required` array and `allRequiredEngaged` are **UX affordances, not a security or
integrity boundary**. Model authors must therefore **re-check required
quantities inside the handler**. The `examples/forms` model does exactly this —
its `execute(RecordMeasurement)` calls `action.validate()` (the same
`allRequiredEngaged` predicate the client gates on) and throws
`std::invalid_argument` if it fails, so schema, form, and server agree by
construction rather than by trust. See [security.md](security.md) for the
dispatcher's validation stance.

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
| [choice.md](choice.md) | Full `Choice` / `FixedString` API and design (this spec cross-refs rather than duplicates them). |
| [quantity_type.md](quantity_type.md) | `Quantity`, its unit tags, `UnitTraits::relations`, and `convert` — the source of `x-decimalPlaces`, `x-unitAlternatives`, and `ExtUnits`. |
| [datetime.md](datetime.md) | `DateTime` / `Timestamp`, the ISO-8601 wire format, and the `"format": "date-time"` schema annotation. |
| [rational.md](rational.md) | Exact `Rational` values; the `num`/`den` in each `x-unitAlternatives` entry are a `Rational` numerator/denominator, which is why unit switches recompute exactly. |
| [security.md](security.md) | The dispatcher's trust boundary — why `required` gates only the client and handlers must re-validate. |

## Out of scope

- Generating schemas for non‑action types (the module assumes glaze reflection is
  available on `A`).
- Validating payloads against the schema — the schema is for the *client*.
- Executing the options action — `choices` metadata tells the client *which*
  action to call, but the forms module does not invoke it.
- `morph::time::Timestamp` definition — it is only consumed here via
  `EmptyCapableField`.