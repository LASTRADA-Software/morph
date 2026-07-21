# `Multiline` / `Ranged` — widget hints for control selection

`morph::forms::Multiline` and `morph::forms::Ranged<Min, Max, Step>` are thin
field-type wrappers, in the same family as `Choice` ([choice.md](choice.md)):
each carries a rendering **control** preference in the C++ type itself, and
the schema surfaces that preference as an `x-*` annotation a renderer can act
on. Neither wrapper changes what travels on the wire — `Multiline` is a plain
string, `Ranged` a nullable number, exactly as if the field had been declared
unwrapped.

For the residual cases a type cannot express — chiefly forcing a specific
control on a plain type, or choosing radio buttons over a combo box for a
`Choice` — an action names the field in the same `static constexpr
fieldMetadata` array the field-metadata feature uses for labels, help text,
and the rest ([forms.md, "Field metadata"](forms.md#field-metadata--fieldmeta)),
whose entry carries a non-empty `widget`; `morph::forms` reads that override
**structurally** (duck-typed on `.field` / `.widget`), so this mechanism works
with any matching descriptor type without this header naming or including it.

## Contents

- [`Multiline` — structure](#multiline--structure)
- [`Ranged<Min, Max, Step>` — structure](#rangedmin-max-step--structure)
- [Empty state](#empty-state)
- [Wire and schema](#wire-and-schema)
- [Schema representation](#schema-representation)
- [Widget override: `fieldMetadata`](#widget-override-fieldmetadata)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Usage example](#usage-example)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## `Multiline` — structure

```cpp
struct Multiline {
    std::string value;
    // ...
};
```

A `Multiline` field is a `std::string` that should be **edited as a
multi-line text area** rather than the single-line field every unwrapped
`std::string` gets by default. The struct carries exactly one payload member,
`value`; the `glz::meta` specialisation reflects it directly, so the type
serialises as a plain JSON string, indistinguishable on the wire from an
unwrapped `std::string`.

## `Ranged<Min, Max, Step>` — structure

```cpp
template <auto Min, auto Max, auto Step = 1>
struct Ranged {
    std::optional<decltype(Min)> value;
    // ...
};
```

| Template parameter | Purpose |
|---|---|
| `Min` | Inclusive lower bound of the slider's control track. |
| `Max` | Inclusive upper bound of the control track (must be greater than `Min`). |
| `Step` | Track increment (defaults to `1`; must be strictly positive). |

`Min`, `Max`, and `Step` must be the same arithmetic type (all `int`, all
`double`, …) — a `Ranged` mixing an `int` `Min`/`Max` with the default `Step`
(itself an `int` literal, `1`) compiles; mixing an `int` `Min`/`Max` with a
`double` `Step` (or vice versa) does not, so a floating-point `Ranged` must
name its `Step` explicitly (e.g. `Ranged<0.5, 2.5, 0.5>`, never
`Ranged<0.5, 2.5>`).

A `Ranged` field is a **bounded numeric edited as a slider**, with the payload
a nullable `decltype(Min)` — the same "one optional value" shape `Choice`
uses, so the type participates in `allRequiredEngaged` the same way.

## Empty state

`Multiline` has no distinguishable empty state: its payload is a plain
`std::string`, and (like an unwrapped `std::string` member) the forms module
cannot tell "not filled in" apart from an intentionally blank string. It does
not define `hasValue()` and therefore does not satisfy `EmptyCapableField`
([forms.md](forms.md), "Empty state") — it is always considered *engaged*.

`Ranged`'s payload is `std::optional<decltype(Min)>`; a default-constructed
`Ranged` is empty. `hasValue()` reports engagement, `operator*` gives
unchecked access (UB when empty, exactly like `Choice`/`std::optional`).

## Wire and schema

Both types serialise through `glz::meta` as their bare payload:

```cpp
template <>
struct glz::meta<morph::forms::Multiline> {
    static constexpr auto value = &morph::forms::Multiline::value;
    static constexpr std::string_view name = "Multiline";
};

template <auto Min, auto Max, auto Step>
struct glz::meta<morph::forms::Ranged<Min, Max, Step>> {
    static constexpr auto value = &morph::forms::Ranged<Min, Max, Step>::value;
    static constexpr std::string_view name = "Ranged";
};
```

`Multiline` therefore serialises as a plain JSON string; `Ranged` as
`decltype(Min) | null`. Neither the widget preference nor (for `Ranged`) the
slider bounds ever travel with a payload — they live only in the C++ type and
the generated schema, exactly like `Choice`'s options metadata
([choice.md](choice.md)).

In the generated schema (`morph::forms::schemaJson`) a `Multiline` property
gets `x-widget: "textarea"`; a `Ranged` property gets `x-widget: "slider"`
plus `x-min` / `x-max` / `x-step`. Neither type is `std::optional<...>`
itself, so both are **required** by forms.md's Required-ness rule
([forms.md](forms.md#required-ness-rule)) unless the action opts the field
out via `optionalFields`.

## Schema representation

Like `Choice`, the `glz::meta` specialisation sets one fixed `name` for
*every* instantiation — `"Multiline"` has no template parameters so this is
moot for it, but `Ranged<Min, Max, Step>` sets `name = "Ranged"` regardless of
`Min`/`Max`/`Step`. glaze therefore collapses every `Ranged<...>`
instantiation in an action to the **same** `$defs/Ranged` entry, describing
only the common shape (a nullable `decltype(Min)`) — exactly the consequence
[choice.md](choice.md#schema-representation) documents for `Choice`. The
collision is equally benign here: a field's own bounds live in its
**property-level** `x-min` / `x-max` / `x-step`, not in the shared `$defs`
entry, so a renderer reading the property node (not the `$def`) never depends
on `$defs/Ranged` to tell two differently-bounded `Ranged` fields apart. The
same caveat as `Choice` applies: when `decltype(Min)` differs across `Ranged`
fields in one action (an `int` slider and a `double` slider, say), the single
`$defs/Ranged` payload type cannot be correct for both; renderers that submit
the raw nullable value observe no problem, since the wire value is validated
by the action, not by the schema.

## Widget override: `fieldMetadata`

`mergeSchemaExtras` (verified in `forms.hpp`) computes each property's
`x-widget` in two steps, for **every** reflected member (not only `Multiline`
/ `Ranged` fields):

1. **Type-derived default.** If the member's type exposes a `noexcept static
   constexpr widget()` returning something convertible to `std::string_view`
   (the shape `Multiline` and `Ranged` both have — `detail::DeclaresWidget` in
   `forms.hpp`), that string is the field's default widget hint.
2. **Override.** If the action declares a `static constexpr` iterable
   `fieldMetadata` (mirroring the `optionalFields` convention) whose element
   type exposes `.field` and `.widget`, both convertible to
   `std::string_view` (`detail::HasFieldMetadataWidgets<A>` in `forms.hpp`),
   and one entry's `.field` equals the member's wire name with a non-empty
   `.widget`, that string **replaces** the type-derived default.

Both checks are **structural** (duck-typed): `forms.hpp` never names or
includes a `FieldMeta` type in this lookup. In practice, every action that
declares `fieldMetadata` today uses `morph::forms::FieldMeta`
([forms.md, "Field metadata"](forms.md#field-metadata--fieldmeta)) — which
already carries `.field` and `.widget` — so the override mechanism is
exercised through that one concrete type; the structural check simply means
`forms.hpp` would honour any other descriptor array shaped the same way, with
no header dependency of its own on `FieldMeta`'s declaration.

`x-min` / `x-max` / `x-step` are **not** overridable through `fieldMetadata` —
they come solely from a `Ranged` field's own `min()` / `max()` / `step()` (via
`detail::DeclaresRangedBounds<Member>`), independent of whatever `x-widget`
ends up on the property. This is deliberate: overriding *which* control
renders (`x-widget`) is orthogonal to the numeric bounds a slider (if
rendered) would use — a `fieldMetadata` override that turns a `Ranged` field
into e.g. `"combo"` still leaves `x-min`/`x-max`/`x-step` on the property,
harmless for a renderer that ignores them.

## API reference

### `Multiline`

| Member | Signature | Notes |
|---|---|---|
| `value` | `std::string value` | Public data member; the payload. |
| default ctor | `constexpr Multiline() noexcept` | Empty string. |
| value ctor | `Multiline(std::string text) noexcept(...)` | Implicit; engages, moving from `text`. |
| `widget()` | `static constexpr std::string_view widget() noexcept` | Always `"textarea"`. |
| `operator==` | `constexpr bool operator==(const Multiline&) const` | Defaulted; compares `value`. |

### `Ranged<Min, Max, Step>`

| Member | Signature | Notes |
|---|---|---|
| `value` | `std::optional<decltype(Min)> value` | Public data member; the payload. |
| default ctor | `constexpr Ranged() noexcept` | Empty state. |
| value ctor | `constexpr Ranged(decltype(Min) selected) noexcept` | Implicit; engages. |
| optional ctor | `constexpr Ranged(std::optional<decltype(Min)> payload) noexcept` | Implicit; adopts as-is. |
| `hasValue()` | `constexpr bool hasValue() const noexcept` | Engaged? |
| `operator*` | `constexpr decltype(Min) operator*() const noexcept` | Unchecked (UB when empty). |
| `min()` | `static constexpr auto min() noexcept` | Returns `Min`. |
| `max()` | `static constexpr auto max() noexcept` | Returns `Max`. |
| `step()` | `static constexpr auto step() noexcept` | Returns `Step`. |
| `widget()` | `static constexpr std::string_view widget() noexcept` | Always `"slider"`. |
| `operator==` | `constexpr bool operator==(const Ranged&) const` | Defaulted; empty equals only empty. |

### Traits (in `morph::forms::detail`, `forms.hpp`)

| Symbol | Kind | Notes |
|---|---|---|
| `DeclaresWidget<T>` | concept | `true` when `T` exposes a `noexcept static constexpr widget()`. |
| `DeclaresRangedBounds<T>` | concept | `true` when `T` exposes `noexcept` `min()`/`max()`/`step()`. |
| `HasFieldMetadataEntries<A>` | concept | `true` when `A::fieldMetadata` is iterable. |
| `HasFieldMetadataWidgets<A>` | concept | `true` when `A::fieldMetadata`'s entries expose `.field`/`.widget`. |
| `widgetOverride<A>(name)` | constexpr function | The matching entry's `.widget`, or `""` when none matches or `A` has no `fieldMetadata`. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Wire representation | **Bare payload, no wrapper metadata** | Same rule as `Choice`/`Quantity`: rendering intent lives in the type and the schema, never the payload. |
| Empty state | **`Multiline`: none; `Ranged`: `std::optional`** | `Multiline` wraps a type (`std::string`) with no framework-recognised empty state of its own; `Ranged` wraps a scalar, which needs an explicit optional to have one. |
| Widget default | **Type-derived, via a `widget()` static function** | Matches the umbrella program's "infer from the type" principle; any user type can opt in the same way `Multiline`/`Ranged` do, with no registration step. |
| Widget override | **Duck-typed on `fieldMetadata`'s `.field`/`.widget`** | Lets the override live in a descriptor whose canonical definition is owned by a different feature ([forms.md, "Field metadata"](forms.md#field-metadata--fieldmeta)) without `forms.hpp` gaining a named dependency on that type. |
| Range subfields | **Not overridable via `fieldMetadata`** | `x-min`/`x-max`/`x-step` describe the *type's* declared bounds; overriding the widget choice must not silently change what those bounds mean. |
| `$defs` naming | **Fixed `name` per wrapper (`"Multiline"`, `"Ranged"`)** | Same trade-off as `Choice`: instantiations collapse into one shared `$def`, harmless because the differentiating data (bounds, widget) is property-level. |

## Usage example

```cpp
#include <morph/forms/widget_hints.hpp>
#include <morph/forms/forms.hpp>

struct SubmitFeedback {
    morph::forms::Multiline comments;
    morph::forms::Ranged<1, 5, 1> rating;
    std::string category;

    // "category" gets a widget purely from the override; "rating" keeps its
    // Ranged-derived "slider" (no entry names it here).
    static constexpr std::array fieldMetadata{
        morph::forms::FieldMeta{.field = "category", .widget = "radio"},
    };

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};
```

The generated schema surfaces `comments` with `x-widget: "textarea"`,
`rating` with `x-widget: "slider"` plus `x-min: 1`, `x-max: 5`, `x-step: 1`,
and `category` with `x-widget: "radio"` (a plain `std::string`, annotated
purely through the override). All three are listed in `required` (none is
`std::optional`, none is in `optionalFields`); `allRequiredEngaged` gates on
`rating.hasValue()` but not on `comments` or `category` (neither is
empty-capable).

## Failure modes

### A `fieldMetadata` entry naming a nonexistent field is silently ignored

`widgetOverride<A>` only ever compares `.field` against the wire names
`forEachNamedMember` actually visits; an entry naming a field that does not
exist on `A` (a typo, a renamed member) never matches anything and is
dropped — consistent with `optionalFields`'s existing behaviour
([forms.md](forms.md)) and with schema generation never throwing.

### A non-conforming `fieldMetadata` disqualifies the whole action

`HasFieldMetadataWidgets<A>` requires **every** element of `A::fieldMetadata`
to expose `.field` and `.widget` convertible to `std::string_view`. If an
action's descriptor array element type is missing either member, the concept
is simply not satisfied and **no** override applies for **any** field on that
action — not a partial application. Widget hints then fall back entirely to
each field's own type-derived `widget()` (or none). This is independent of
(and additionally constrained versus) `detail::HasFieldMetadata<A>` — the
label/help/placeholder lookup — which requires `A::fieldMetadata`'s elements
to be convertible to `morph::forms::FieldMeta` specifically, so that
`detail::findFieldMeta` can hand back a typed pointer; a `fieldMetadata` array
of some other shape satisfies the widget-override concept without satisfying
that one, and vice versa is not possible since `FieldMeta` itself satisfies
both.

## Limitations

- **No option-count-driven radio/combo.** Whether a `Choice` renders as radio
  buttons is always an explicit `fieldMetadata` override — never inferred
  from how many options the options action happens to return, which is not
  known at schema-generation time ([choice.md](choice.md)).
- **`x-widget` is advisory, never validation.** A slider's `x-min`/`x-max` is
  a control track only; value-bounds enforcement stays with glaze's own
  `minimum`/`maximum` (when declared) and server-side checks — a renderer may
  legitimately present a wider or narrower track than any validation bound.
- **The control-id vocabulary is not enumerated here.** `x-widget` carries
  whatever string the type or override supplies; this spec does not
  enumerate every id a renderer must recognise — only `"textarea"` (from
  `Multiline`) and `"slider"` (from `Ranged`) are type-derived, and any other
  id (`"radio"`, `"combo"`, `"password"`, …) is meaningful only by convention
  between the action's author and the target renderer.

## Cross-references

- **[forms.md](forms.md)** — `schemaJson<A>()`, `mergeSchemaExtras`,
  `forEachNamedMember`, `EmptyCapableField` / `required` derivation these
  wrappers plug into; `FieldMeta` and its `.widget` member
  ([forms.md, "Field metadata"](forms.md#field-metadata--fieldmeta)), the
  concrete descriptor type most actions use to supply the override this spec
  describes; and the renderer-contract table `x-widget`/`x-min`/`x-max`/
  `x-step` extend.
- **[choice.md](choice.md)** — the type-carries-intent / wire-carries-value
  pattern and the `$defs`-collapse consequence both wrappers follow.
- **[quantity_type.md](../util/quantity_type.md)** — `Quantity`'s own
  `x-decimalPlaces` entry-granularity annotation, the analogous (but
  distinct) numeric-precision contract `x-step` does not replace.
- **[forms.md](forms.md#design-principle-infer-by-default-declare-to-override)** —
  the infer-by-default / declare-to-override principle and the additive-`x-*`
  versioning stance this feature obeys.
