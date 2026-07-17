# The `Choice` type — design

`morph::forms::Choice<T, "ListPayees">` is a form field whose options come from
executing another action at runtime. In the UI it renders as a combo box: the
client calls the named action, reads the result rows, and offers `labelField` as
display text while submitting `valueField` as the payload.

The key design property: **the options metadata lives in the C++ type, never on
the wire**. The wire carries only the nullable underlying value (`T`), exactly
like `Quantity` and `Timestamp`. The schema bridges the gap — the declaration
surfaces `x-optionsAction` / `x-optionValue` / `x-optionLabel` so a client knows
which action to call and which result fields to use without hardcoding anything.

## Contents

- [FixedString — NTTP string](#fixedstring--an-nttp-compile-time-string)
- [FixedString notes](#fixedstring-notes)
- [Choice — structure](#choice--structure)
- [Empty state](#empty-state)
- [Wire and schema](#wire-and-schema)
- [Schema representation](#schema-representation)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Usage example](#usage-example)
- [Author's obligations](#authors-obligations)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## FixedString — an NTTP compile-time string

`morph::forms::FixedString<N>` is a structural type that can appear as a
non-type template parameter. It stores `N` characters (including the terminating
null) in a `std::array<char, N>` and is constructed `consteval` from a string
literal.

It is an **alias** for the single shared definition `morph::detail::FixedString`
(`include/morph/detail/fixed_string.hpp`). The units layer's `NamedQuantity`
(see [quantity_type.md](quantity_type.md)) uses the *same* underlying type via
its own `morph::units::detail::FixedString` alias, so there is exactly one
`FixedString` definition in the codebase, not two look-alikes. `morph::forms::`
and `morph::units::detail::` are kept as names for source compatibility and
layer-local readability.

| Member | Signature | Notes |
|---|---|---|
| `data` | `std::array<char, N> data{}` | Null-terminated storage. |
| ctor | `consteval FixedString(const char (&literal)[N]) noexcept` | From a string literal of the same length. |
| `view()` | `[[nodiscard]] constexpr std::string_view view() const noexcept` | Excludes the null terminator — length is `N - 1`. |

The `consteval` constructor guarantees that `FixedString` is only ever
initialised from a compile-time literal, so every template instantiation is
visible to the compiler at the point of use.

## FixedString notes

A few subtleties matter because `FixedString` is the vehicle that carries the
options metadata into the type system:

- **`N` counts the terminating null.** The literal `"id"` is `const char[3]`,
  so `FixedString<3>`; `view()` returns `{data.data(), N - 1}` — a two-char
  view `"id"` that stops before the null. The null is stored but never part of
  the string.
- **An empty literal yields a zero-length name.** `""` is `const char[1]`, so
  `FixedString<1>` and `view()` is a length-0 `string_view`. Nothing rejects
  this — an empty action name or field name compiles and simply produces an
  empty `x-option*` annotation that no client can act on.
- **Type identity depends on structural NTTP equality.** `FixedString` is a
  *structural type* (only public, non-mutable data members: the `std::array`),
  so it is usable as a non-type template parameter and two `FixedString`
  values compare member-wise. Two separate `"id"` literals therefore produce
  the *same* `FixedString<3>` value, so `Choice<T, "A", "id", "name">` written
  in two translation units is one and the same type. This is what makes
  `Choice` type identity stable across the codebase and what lets `glz::meta`
  and `isChoice` match on the instantiation.
- **`FixedString` is public only as an NTTP vehicle.** It exists so a string
  literal can travel as a template parameter; it is not a general-purpose
  string type and is not intended for direct use in application code. The
  only supported way to produce one is a string literal in a `Choice`
  template-argument position.

## Choice — structure

```cpp
template <typename T, FixedString OptionsAction,
          FixedString ValueField = "id", FixedString LabelField = "name">
struct Choice {
    std::optional<T> value;
    // ...
};
```

| Template parameter | Purpose |
|---|---|
| `T` | Underlying value type submitted on the wire (e.g. `std::int64_t` for ids, `std::string` for codes). |
| `OptionsAction` | Type id of the registered action whose result provides the options (executed with an empty body). |
| `ValueField` | Field of each result row submitted as the value. |
| `LabelField` | Field of each result row shown to the user. |

The four template parameters — the type parameter `T` plus the three
`FixedString` NTTPs — make every `Choice<T, "ListPayees">` a distinct type
whose options source is known at compile time. The same action name can appear
in multiple `Choice` fields across different form types.

## Empty state

The payload is `std::optional<T>`. A default-constructed `Choice` is empty — no
value has been selected. `hasValue()` queries the state; `operator*` provides
unchecked access (UB when empty, exactly like `std::optional`).

Equality is total: `operator==` returns `true` when both are empty or both hold
equal values.

## Wire and schema

On the morph JSON wire a `Choice` is its nullable underlying value — the
options metadata never travels with payloads. The glaze `meta` specialisation
maps the type name to `"Choice"` and serialises through the `value` member
directly:

```cpp
template <typename T, FixedString OA, FixedString VF, FixedString LF>
struct glz::meta<morph::forms::Choice<T, OA, VF, LF>> {
    static constexpr auto value = &morph::forms::Choice<T, OA, VF, LF>::value;
    static constexpr std::string_view name = "Choice";
};
```

In the generated JSON Schema (`morph::forms::schemaJson`) the property receives
`x-optionsAction`, `x-optionValue`, and `x-optionLabel` annotations so a client
knows which action to call and which result fields to use. A non-optional
`Choice` member is required by the `morph::forms` rules.

## Schema representation

The `glz::meta` specialisation sets `name = "Choice"` for *every*
instantiation, regardless of `T`, `OptionsAction`, `ValueField`, or
`LabelField`. glaze uses that name as the type's key in the schema's `$defs`
block and as the `$ref` target for each property. Two consequences follow, both
intentional:

- **`$defs` collapses all `Choice<...>` to one entry.** `Choice<int64_t,
  "ListSamples">` and `Choice<std::string, "ListPayees", "code", "label">`
  both resolve to `$defs/Choice` and share a single `$ref`. glaze does not
  suffix the name to keep them apart, so whichever it emits describes only the
  common shape.
- **That shared shape is the bare nullable value.** Because `meta::value`
  points at the `value` member, the `$defs/Choice` entry describes only
  `std::optional<T>` — a nullable scalar/string, carrying none of the options
  metadata. Nothing that distinguishes one `Choice` field from another lives
  in `$defs`.

The collision is therefore benign: the parts that *do* differ between fields —
which action to call, which result fields to read — are emitted by
`mergeSchemaExtras` as **property-level** `x-optionsAction` / `x-optionValue` /
`x-optionLabel` annotations, one set per property, alongside `x-order` and the
derived `required` array. A renderer reads those from the property, not from
`$defs`, so it never depends on the shared `$defs/Choice` node to tell two
`Choice` fields apart. The one caveat, when `T` varies across `Choice` fields
in the same action, is that the single `$defs/Choice` payload type cannot be
correct for all of them; renderers that submit the raw nullable value observe
no problem, since the wire value is validated by the action, not by the schema.

## API reference

### `Choice<T, OptionsAction, ValueField, LabelField>`

| Member | Signature | Notes |
|---|---|---|
| `value` | `std::optional<T> value` | Public data member; the payload. |
| default ctor | `constexpr Choice() noexcept` | Empty state — `std::nullopt`. |
| value ctor | `constexpr Choice(T selected) noexcept(std::is_nothrow_move_constructible_v<T>)` | Implicit; engages, moving from `selected`. |
| optional ctor | `constexpr Choice(std::optional<T> payload) noexcept(std::is_nothrow_move_constructible_v<T>)` | Implicit; adopts an optional payload as-is. |
| `optionsAction()` | `static constexpr std::string_view optionsAction() noexcept` | The action name from the type. |
| `valueField()` | `static constexpr std::string_view valueField() noexcept` | The result-row field submitted as the value. |
| `labelField()` | `static constexpr std::string_view labelField() noexcept` | The result-row field shown to the user. |
| `hasValue()` | `constexpr bool hasValue() const noexcept` | Engaged? No implicit `bool` conversion. |
| `operator*` | `constexpr const T& operator*() const noexcept` | Unchecked access (UB when empty). |
| `operator==` | `constexpr bool operator==(const Choice&) const noexcept` | Total; empty==empty is `true`. |

### Trait

| Symbol | Kind | Notes |
|---|---|---|
| `isChoice<T>` | `constexpr bool` | `true` when `T` (cvref-stripped) is a `morph::forms::Choice`. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Options source | **NTTP `FixedString` naming an action** | The options action is known at compile time and baked into the type; the schema then tells the client which action to call without the client hardcoding any names. |
| Value / label fields | **NTTP defaults `"id"` / `"name"`** | Most list actions return rows with these conventional field names; override when the result schema differs. |
| Wire representation | **Nullable `T` only** | Options metadata never travels with payloads — it lives in the C++ type and the generated schema. Keeps the wire compact and stable. |
| Options metadata delivery | **Schema annotations (`x-optionsAction` etc.)** | Generated by `schemaJson` from the NTTP parameters so a client can discover the options source without hardcoding. |
| Blank state | **`std::nullopt` in the payload** | Same pattern as `Quantity` and `Timestamp`; a non-optional `Choice` member is required by the forms rules. |
| Glaze serialisation | **Through the `value` member directly** | The glaze `meta` specialisation maps `value` so the type serialises as its nullable payload, with the type name `"Choice"`. |
| Equality | **Total, through `std::optional`'s semantics** | Empty equals empty; engaged values compare by `T`'s equality. |
| Converting constructors | **Both the `T` and `std::optional<T>` ctors are implicit** | Lets a `Choice` field be assigned directly from a bare value or an optional — `draft.slot = 4` and `slot = std::optional<int64_t>{}` both work — so authors need not name the field type at every assignment. `Choice` has no member the two ctors could ambiguate on, so an implicit `T` and an implicit `std::optional<T>` coexist without a conversion clash. |

## Usage example

```cpp
#include <morph/choice.hpp>
#include <cstdint>

struct SampleInfo { std::int64_t id = 0; std::string name; };
struct SampleList { std::vector<SampleInfo> samples; };
struct ListSamples {};  // registered like any other action (a pure query)

struct RecordMeasurement {
    morph::forms::Choice<std::int64_t, "ListSamples"> sampleId;
    // ...
};
```

The generated schema surfaces `sampleId` as a `Choice` with `x-optionsAction =
"ListSamples"`, `x-optionValue = "id"`, `x-optionLabel = "name"`. The client
calls `ListSamples`, reads the `SampleList` result, and offers each
`SampleInfo::name` as a display option while submitting `SampleInfo::id` as the
value. On the wire the field is just a nullable integer — `null` or `42`.

## Author's obligations

Declaring a `Choice` places three obligations on the author that the type
system cannot check, because the strings are opaque NTTPs:

- **The `OptionsAction` must be a registered action.** The name (`"ListSamples"`
  above) has to resolve to an action the executor knows, and that action must
  succeed **when invoked with an empty body** — the renderer calls it with no
  arguments to populate the combo box. An action that requires input fields
  cannot serve as an options source.
- **The result rows must expose the value and label fields.** Each row the
  options action returns must have fields literally named by `ValueField` and
  `LabelField` — `id` and `name` by default. The renderer reads those two
  fields off every row: `valueField()` becomes the submitted payload,
  `labelField()` the display text.
- **Those names are WIRE (JSON) field names, not C++ member names.** They must
  match what the result row serialises as on the wire, not what the C++ member
  is called. A `glz::meta` rename (or any glaze naming customisation) on the
  result type changes the wire name, and `ValueField`/`LabelField` must track
  that renamed wire name — not the original member identifier. Defaulting to
  `"id"`/`"name"` works only when the result rows serialise with exactly those
  keys.

## Failure modes

### Validation & staleness

`Choice` participates in `morph::forms` required-field validation only through
`hasValue()`, which reports **engagement** — whether *some* value is selected —
and nothing more. That leaves gaps neither the client nor the server closes:

- **A required `Choice` with an empty options list is permanently
  unsubmittable.** If the `OptionsAction` returns zero rows, there is nothing to
  select, so `hasValue()` can never become `true`, so `allRequiredEngaged`
  never passes. The form cannot be submitted until the options action yields at
  least one row — there is no "no valid options" escape hatch for a required
  field.
- **`allRequiredEngaged` checks engagement, never membership.** It verifies the
  payload is engaged; it never re-checks that the selected value still exists
  among the *current* options. A value chosen when the list contained id `42`
  stays "valid" to `allRequiredEngaged` even after `42` disappears from the
  options action's result. A **stale id submits silently** — the draft looks
  complete and goes out with a value that no current option backs.
- **Neither side validates option membership.** The schema's `x-option*`
  annotations tell a client how to *fetch* options; they do not constrain the
  submitted value to that set. The client renderer does not re-validate the
  payload against a freshly fetched list, and the wire schema (a bare nullable
  `T`) has no enumeration to check against on the server. Membership is an
  assumption, not an enforced invariant, at both ends.

## Limitations

- **The action and field names are unchecked strings.** `OptionsAction`,
  `ValueField`, and `LabelField` are resolved *at runtime* by the client and
  executor. A typo in the action name, an action that is not registered, or a
  value/label field that does not match the result row's wire keys all
  **compile cleanly** and fail only later — when some client executes the
  options action and finds nothing, or reads a field that is not there. There
  is no compile-time link between a `Choice` and the action or result type it
  names. Renaming a result field (e.g. via `glz::meta`) without updating the
  `Choice` is the same silent failure.
- **The accessor surface is unchecked-only.** The type offers `hasValue()` and
  an *unchecked* `operator*` (UB when empty, by design). There is no
  `value_or`, no checked accessor, and no `operator bool`. Callers must guard
  every dereference with `hasValue()` themselves; the type will not do it for
  them, and "read the value if present, else a default" has to be written by
  hand.

## Cross-references

- **[forms.md](forms.md)** — how a `Choice` member becomes *required* (the
  `EmptyCapableField` concept plus the not-`std::optional`/not-`optionalFields`
  rule), and where `mergeSchemaExtras` emits the `x-optionsAction` /
  `x-optionValue` / `x-optionLabel` property annotations.
- **[quantity_type.md](quantity_type.md)** and **[datetime.md](datetime.md)** —
  `Quantity` and `Timestamp` share the *one kind of empty* pattern: the blank
  state lives inside the value as `std::optional`, `hasValue()` reports
  engagement, and a non-optional member is required. `Choice` is the third
  member of that family.
- **[security.md](security.md)** — the options metadata and any
  membership expectation are enforced (if at all) only on the client; the
  server sees a bare nullable value. This is the client-only-validation trust
  boundary — never trust a submitted `Choice` value to be a current, valid
  option without server-side re-checking.