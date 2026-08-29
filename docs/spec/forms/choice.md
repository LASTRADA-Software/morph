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

- [Choice — structure](#choice--structure)
- [Empty state](#empty-state)
- [Dependent (cascading) options — `DependsOn`](#dependent-cascading-options--dependson)
- [Wire and schema](#wire-and-schema)
- [Schema representation](#schema-representation)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Usage example](#usage-example)
- [Author's obligations](#authors-obligations)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Cross-references](#cross-references)
  
## Choice — structure

```cpp
template <typename T, FixedString OptionsAction,
          FixedString ValueField = "id", FixedString LabelField = "name",
          FixedString... DependsOn>
struct Choice {
    std::optional<T> value;
    // ...
};
```

| Template parameter | Purpose |
|---|---|
| `T` | Underlying value type submitted on the wire (e.g. `std::int64_t` for ids, `std::string` for codes). |
| `OptionsAction` | Type id of the registered action whose result provides the options. Executed with an empty body when `DependsOn` is empty; otherwise with `{name: value, ...}` built from the named sibling fields' current values. |
| `ValueField` | Field of each result row submitted as the value. |
| `LabelField` | Field of each result row shown to the user. |
| `DependsOn` | Optional trailing pack of wire (JSON) field names of sibling fields whose current values parameterise the options action — a cascading picklist. Defaults to empty (no dependency), which is today's independent `Choice`, unchanged. |

The four template parameters — the type parameter `T` plus the three
[`FixedString`](forms.md#fixedstring--nttp-compile-time-string) NTTPs (a
structural type letting string literals act as non-type template parameters —
see the link for the full definition) — make every `Choice<T, "ListPayees">` a distinct type
whose options source is known at compile time. The same action name can appear
in multiple `Choice` fields across different form types. `DependsOn` is a
fifth, trailing, defaulted-empty pack: it adds no new distinctness rule beyond
what the pack's own contents already produce (two `Choice`s differing only in
`DependsOn` are already distinct types, same as differing in any other NTTP).

## Empty state

The payload is `std::optional<T>`. A default-constructed `Choice` is empty — no
value has been selected. `hasValue()` queries the state; `operator*` provides
unchecked access (UB when empty, exactly like `std::optional`).

Equality is total: `operator==` returns `true` when both are empty or both hold
equal values.

## Dependent (cascading) options — `DependsOn`

`Choice` accepts an optional trailing pack of `FixedString` names, `DependsOn`,
naming sibling fields of the same enclosing action whose **current values**
parameterise the options action — a cascading picklist (the list of cities
depends on the selected country, sub-accounts depend on the selected account,
…). The pack defaults to empty, which is today's independent `Choice`
unchanged: `optionsDependsOn()` returns an empty array, `OptionsAction` is
still called with an empty body, and the emitted schema carries no
`x-optionsDependsOn` key.

When `DependsOn` is non-empty, the options action's request body is no longer
empty: a client sends `{name: value, …}` — one entry per `DependsOn` name, set
to that sibling's current engaged value — instead of `{}`. The options action
is still an ordinary registered action; only the body it receives changes.
The options action's own `schemaJson` describes that body (a `ListCities`
action with a `country` field expects exactly that key), so the `DependsOn`
names must match the options action's actual input field names — the same
class of runtime-checked convention `ValueField`/`LabelField` already are
against the options action's *result* fields.

```cpp
struct ShippingAddress {
    morph::forms::Choice<std::int64_t, "ListCountries"> country;
    morph::forms::Choice<std::int64_t, "ListCities", "id", "name", "country">
        city;   // options depend on the sibling "country" value
};
```

`optionsDependsOn()` returns the declared names as
`std::array<std::string_view, N>`, in declaration order — the accessor
`mergeSchemaExtras` ([forms.md#renderer-contract-the-schema-key-vocabulary](forms.md#renderer-contract-the-schema-key-vocabulary))
reads to emit `x-optionsDependsOn`
on the property, and the same accessor a renderer reads to know which sibling
fields to watch. *Fetching* on that dependency (waiting for every parent to be
engaged, re-fetching on change, clearing a stale child selection) is a
renderer concern documented in
[forms.md's renderer contract](forms.md#renderer-contract-the-schema-key-vocabulary), not
part of the `Choice` type itself — `Choice` only carries the declaration.

## Wire and schema

On the morph JSON wire a `Choice` is its nullable underlying value — the
options metadata never travels with payloads. The glaze `meta` specialisation
maps the type name to `"Choice"` and serialises through the `value` member
directly:

```cpp
template <typename T, FixedString OA, FixedString VF, FixedString LF, FixedString... DependsOn>
struct glz::meta<morph::forms::Choice<T, OA, VF, LF, DependsOn...>> {
    static constexpr auto value = &morph::forms::Choice<T, OA, VF, LF, DependsOn...>::value;
    static constexpr std::string_view name = "Choice";
};
```

In the generated JSON Schema (`morph::forms::schemaJson`) the property receives
`x-optionsAction`, `x-optionValue`, and `x-optionLabel` annotations so a client
knows which action to call and which result fields to use — plus
`x-optionsDependsOn` when the field declares a `DependsOn` pack (see
[Dependent (cascading) options](#dependent-cascading-options--dependson)
above). A non-optional `Choice` member is required by the `morph::forms`
rules.

## Schema representation

The `glz::meta` specialisation sets `name = "Choice"` for *every*
instantiation, regardless of `T`, `OptionsAction`, `ValueField`, `LabelField`,
or `DependsOn`. glaze uses that name as the type's key in the schema's `$defs`
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
which action to call, which result fields to read, and (for a dependent
`Choice`) which sibling fields parameterise it — are emitted by
`mergeSchemaExtras` as **property-level** `x-optionsAction` / `x-optionValue` /
`x-optionLabel` / `x-optionsDependsOn` annotations, one set per property,
alongside `x-order` and the derived `required` array. A renderer reads those
from the property, not from `$defs`, so it never depends on the shared
`$defs/Choice` node to tell two `Choice` fields apart. The one caveat, when `T`
varies across `Choice` fields in the same action, is that the single
`$defs/Choice` payload type cannot be correct for all of them; renderers that
submit the raw nullable value observe no problem, since the wire value is
validated by the action, not by the schema.

## API reference

### `Choice<T, OptionsAction, ValueField, LabelField, DependsOn...>`

| Member | Signature | Notes |
|---|---|---|
| `value` | `std::optional<T> value` | Public data member; the payload. |
| default ctor | `constexpr Choice() noexcept` | Empty state — `std::nullopt`. |
| value ctor | `constexpr Choice(T selected) noexcept(std::is_nothrow_move_constructible_v<T>)` | Implicit; engages, moving from `selected`. |
| optional ctor | `constexpr Choice(std::optional<T> payload) noexcept(std::is_nothrow_move_constructible_v<T>)` | Implicit; adopts an optional payload as-is. |
| `optionsAction()` | `static constexpr std::string_view optionsAction() noexcept` | The action name from the type. |
| `valueField()` | `static constexpr std::string_view valueField() noexcept` | The result-row field submitted as the value. |
| `labelField()` | `static constexpr std::string_view labelField() noexcept` | The result-row field shown to the user. |
| `optionsDependsOn()` | `static constexpr std::array<std::string_view, N> optionsDependsOn() noexcept` | Wire names of the sibling fields this field's options depend on; empty for an independent `Choice`. |
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
| Sibling dependency | **Optional trailing `FixedString... DependsOn` NTTP pack** | Cascading picklists need the parent's current value in the options-action request; `Choice` doesn't know its enclosing action type, so parent names are opaque wire-name strings — the same NTTP vehicle `OptionsAction`/`ValueField`/`LabelField` already use. Defaults to empty, so every pre-existing `Choice<...>` is source- and schema-compatible unchanged. |
| Blank state | **`std::nullopt` in the payload** | Same pattern as `Quantity` and `Timestamp`; a non-optional `Choice` member is required by the forms rules. |
| Glaze serialisation | **Through the `value` member directly** | The glaze `meta` specialisation maps `value` so the type serialises as its nullable payload, with the type name `"Choice"`. |
| Equality | **Total, through `std::optional`'s semantics** | Empty equals empty; engaged values compare by `T`'s equality. |
| Converting constructors | **Both the `T` and `std::optional<T>` ctors are implicit** | Lets a `Choice` field be assigned directly from a bare value or an optional — `draft.slot = 4` and `slot = std::optional<int64_t>{}` both work — so authors need not name the field type at every assignment. `Choice` has no member the two ctors could ambiguate on, so an implicit `T` and an implicit `std::optional<T>` coexist without a conversion clash. |

## Usage example

```cpp
#include <morph/forms/choice.hpp>
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

Declaring a `Choice` places six obligations on the author that the type
system cannot check, because the strings are opaque NTTPs:

- **The `OptionsAction` must be a registered action.** The name (`"ListSamples"`
  above) has to resolve to an action the executor knows, and that action must
  succeed **when invoked with an empty body** (an independent `Choice`, no
  `DependsOn`) **or with `{name: value, ...}` built from the `DependsOn`
  names** (a dependent `Choice`) — the renderer calls it with no other
  arguments to populate the combo box. An action that requires input fields
  beyond its declared `DependsOn` cannot serve as an options source.
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
- **Each `DependsOn` name must be a real sibling field's wire name** in the
  enclosing action. The renderer reads that sibling's current value to build
  the options request; a name matching no property yields a missing key in
  the request body.
- **The options action must accept those names as input fields.** An options
  action that ignores the body silently returns the unfiltered list — no
  error, just a non-cascading picklist.
- **The parent's value type must match the options action's input field
  type.** The renderer forwards the parent `Choice`'s underlying `T` as-is; a
  mismatch surfaces only as a decode failure or empty result at fetch time.

## Failure modes

### Option ids larger than 2^53

An option id is carried exactly, whatever its magnitude. The renderer parses an
`OptionsAction` reply with `JsonExact.parse` (`src/qt/forms/qml/JsonExact.js`),
which keeps an integer literal a double cannot represent as its exact digits,
and emits those digits verbatim into the submitted body.

This is a guarantee, not an implementation note, because the alternative is
silent: JavaScript numbers are IEEE-754 doubles, so a plain `JSON.parse` rounds
an id above 2^53 on the way in, and re-serialising the rounded number submits a
*different* id than the app sent. Doubles round to even in that range, so a
dense id sequence collapses pairwise — two option rows reduce to the same
`valueJson`, the combo box shows two entries the UI cannot tell apart, and the
staleness guard matches happily against either. A sparse id usually rounds to a
value naming no row at all, which either throws in the model or is stored as
garbage, depending on whether the action looks the id up (morph#190).

Values a double *does* hold exactly — the overwhelmingly common case — remain
ordinary JSON numbers, so nothing about the wire shape changes for them.

### Validation & staleness

`Choice` participates in `morph::forms` required-field validation only through
`hasValue()`, which reports **engagement**
([defined in forms.md](forms.md#empty-state--emptycapablefield-concept)) — whether *some* value is selected —
and nothing more. That leaves gaps neither the client nor the server closes:

- **A required `Choice` with an empty options list is permanently
  unsubmittable.** If the `OptionsAction` returns zero rows, there is nothing to
  select, so `hasValue()` can never become `true`, so
  [`allRequiredEngaged`](forms.md#allrequiredengageda--readiness-check)
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
- **`DependsOn` names are unchecked strings, same as `OptionsAction`.** Each
  name is resolved at runtime against the enclosing action's sibling fields
  and against the options action's input fields; a typo, a renamed sibling,
  or an options action that doesn't accept the named input all compile
  cleanly and fail only when a client fetches.

## Lifetime annotations

`operator*` returns a reference into the `Choice` and marks its implicit object
parameter `MORPH_LIFETIMEBOUND` (`morph/attributes.hpp`), so dereferencing a
temporary and keeping the result is a diagnostic rather than a dangling read. See
[concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#morph_lifetimebound--the-must-outlive-rules-told-to-the-compiler).

## Cross-references

- **[forms.md](forms.md)** — how a `Choice` member becomes *required* (the
  [`EmptyCapableField` concept](forms.md#empty-state--emptycapablefield-concept)
  plus the [not-`std::optional`/not-`optionalFields` rule](forms.md#required-ness-rule)),
  and where `mergeSchemaExtras` emits the `x-optionsAction` /
  `x-optionValue` / `x-optionLabel` / `x-optionsDependsOn` property
  annotations ([renderer contract](forms.md#renderer-contract-the-schema-key-vocabulary)).
- **[quantity_type.md](../util/quantity_type.md)** and **[datetime.md](../util/datetime.md)** —
  `Quantity` and `Timestamp` share the *one kind of empty* pattern: the blank
  state lives inside the value as `std::optional`, `hasValue()` reports
  engagement, and a non-optional member is required. `Choice` is the third
  member of that family.
- **[security.md](../security.md)** — the options metadata and any
  membership expectation are enforced (if at all) only on the client; the
  server sees a bare nullable value. This is the client-only-validation trust
  boundary — never trust a submitted `Choice` value to be a current, valid
  option without server-side re-checking.
