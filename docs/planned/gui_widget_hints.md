# GUI widget hints — control selection (planned)

> **Status: planned — not yet implemented.** This spec extends the GUI program
> umbrella ([gui_overview.md](gui_overview.md)) and the schema-generation spine
> ([forms.md](../spec/forms.md)), following the type-derived-metadata pattern of
> [choice.md](../spec/choice.md). It is a Tier-1 richer-forms feature: control
> selection for a single action's fields, additive and opt-in. See
> [todo.md](../todo.md).

## The gap

A renderer today picks a control almost entirely from the JSON-Schema `type` and
morph's existing `x-*` keys: a `Choice` (`x-optionsAction`) becomes a combo box,
a `Quantity` becomes a numeric input with a unit selector, a `Timestamp`
(`"format": "date-time"`) becomes a date-time picker, a `bool` a checkbox, a
`string` a single-line text field ([forms.md](../spec/forms.md), "Renderer
contract"). That covers the typed field palette but leaves real control choices
unexpressed:

- **Multiline vs single-line text.** Every `std::string` becomes a one-line field;
  a `notes` / `description` field has no way to ask for a text area.
- **Slider vs spin box for bounded numerics.** glaze emits numeric `minimum` /
  `maximum` bounds, but nothing says "this bounded range is better as a slider."
- **Radio group vs combo for a small `Choice`.** A `Choice` is always a combo box
  even when it has three fixed options that would read better as radio buttons —
  and the option count is not even known at schema-generation time (it comes from
  running the options action).

The umbrella principle ([gui_overview.md](gui_overview.md)) says the control
should come from the **type** wherever possible, exactly as `Quantity` and
`Choice` already carry their own rendering intent in the type, and an explicit
`x-widget` hint is added only when the type is genuinely ambiguous.

## Goal

Derive the control from the field **type** by default, extending the small typed
wrapper family morph already has (`Quantity`, `Choice`), and fall back to an
explicit `x-widget` override only where the type cannot say. Concretely:

1. **New typed wrappers** carry the control intent so it is inferred, not
   declared — e.g. `Multiline<>` for a text area, `Ranged<...>` for a slider.
2. **`x-widget` override** for the residual ambiguous cases (radio-vs-combo,
   forcing a specific control on a plain type) via the
   [gui_field_metadata.md](gui_field_metadata.md) descriptor, so a plain type
   need not be replaced just to change its control.

Unannotated plain types keep their current controls unchanged.

## Design

### Type-derived control: new wrapper types (NEW)

In the spirit of `Choice`/`Quantity` — a thin wrapper whose *wire form is its
payload* and whose rendering intent lives in the C++ type — the following NEW
wrappers are proposed. Each serialises through glaze `meta` as its bare payload
(the `Choice` pattern, [choice.md](../spec/choice.md)) so the wire is unchanged;
each carries `hasValue()` where it can be empty so it participates in
`EmptyCapableField` / `required` derivation (verified in `forms.hpp`) exactly
like `Choice`:

```cpp
// namespace morph::forms — NEW.

// A string edited as a text area. Wire form: plain string.
struct Multiline {
    std::string value;
    static constexpr std::string_view widget() noexcept { return "textarea"; }
    // glz::meta reflects `value`; serialises as a plain JSON string.
};

// A bounded numeric edited as a slider. Wire form: plain number.
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

These follow the same three properties `Choice` established
([choice.md](../spec/choice.md)): the control intent is a compile-time property
of the type, the wire carries only the value, and the schema bridges the gap via
a property-level `x-*` key. A renderer needs no new wire handling — only to
honour the emitted `x-widget`.

### Explicit override: `x-widget` on the field descriptor (NEW)

For the cases a type cannot express — chiefly **radio-vs-combo for a small
`Choice`**, where the control preference is orthogonal to the value type and the
option count is unknown at compile time — the author sets `widget` on the
[gui_field_metadata.md](gui_field_metadata.md) `FieldMeta` descriptor:

```cpp
static constexpr std::array fieldMetadata{
    FieldMeta{.field = "status", .widget = "radio"},   // a small Choice as radios
    FieldMeta{.field = "code",   .widget = "password"},// a plain string, masked
};
```

`widget` on `FieldMeta` is the single override channel; the derived widget (from
a wrapper type) is the default, and an explicit `FieldMeta::widget` **wins** over
it. This keeps one declaration surface for per-field presentation across
[gui_field_metadata.md](gui_field_metadata.md) and this spec.

### Emitted keys

`mergeSchemaExtras<A>` (verified in `forms.hpp`), iterating members via
`forEachNamedMember` (verified), emits `x-widget` on a property when the field's
type declares a `widget()` **or** a `FieldMeta::widget` overrides it, and the
range subfields for a `Ranged`:

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `x-widget` | property node (sibling of `$ref`) | string | The preferred control id: `"textarea"`, `"slider"`, `"radio"`, `"combo"`, `"password"`, `"checkbox"`, … A `FieldMeta::widget` override wins; else the field type's `widget()`. **Advisory** — a renderer that lacks the named control falls back to the type-default control (text area → text field, slider → numeric input, radio → combo). Omitted when neither a wrapper nor an override supplies one. |
| `x-min` | property node (sibling of `$ref`) | number | Slider lower bound, from `Ranged::min()`. Emitted only for a `Ranged` field. Distinct from glaze's schema `minimum` (a *validation* bound); `x-min` is the *control track* start. A renderer without a slider ignores it. |
| `x-max` | property node (sibling of `$ref`) | number | Slider upper bound, from `Ranged::max()`. Emitted only for a `Ranged` field. |
| `x-step` | property node (sibling of `$ref`) | number | Slider / numeric increment, from `Ranged::step()`. Emitted only for a `Ranged` field. For a `Quantity` the entry granularity remains `x-decimalPlaces` ([forms.md](../spec/forms.md)); `x-step` is not emitted for `Quantity`. |

All keys are **additive and non-breaking**: they extend the
[forms.md](../spec/forms.md) contract table with no change to any existing key,
per [gui_overview.md](gui_overview.md)'s versioning stance. Because `x-widget` is
**advisory**, a renderer that ignores it **falls back to exactly today's
behaviour** — it selects the control from `type` and the existing keys, so a
`Multiline` renders as an ordinary text field, a `Ranged` as an ordinary numeric
input, and a `Choice` as a combo box. No form becomes unrenderable for lack of a
named control; the hint only upgrades the control when the renderer supports it.

### Illustrative renderer read (non-normative)

```qml
// x-widget chooses the control; unknown ids fall back to the type default.
Loader {
    sourceComponent: {
        switch (prop["x-widget"]) {
        case "textarea": return textAreaCtl;
        case "slider":   return sliderCtl;   // uses x-min / x-max / x-step
        case "radio":    return radioGroupCtl;
        default:         return defaultForType(prop);   // today's selection
        }
    }
}
```

Illustrative of *one* renderer; the contract stays renderer-agnostic.

## Non-goals

- **No new wire types.** Every wrapper serialises as its bare payload (string /
  number), so `x-widget` never changes what travels — a `Multiline` is a
  `string` on the wire, a `Ranged` a number. The wire and dispatch semantics are
  untouched ([gui_overview.md](gui_overview.md)).
- **`x-widget` is not validation.** A slider's `x-min` / `x-max` are a *control
  track*, not an enforced range. Value validation stays with glaze's `minimum` /
  `maximum` and server-side checks ([validation.md](validation.md)); a renderer
  may present a wider track than the validation bound.
- **No arbitrary custom widgets.** `x-widget` is a small closed vocabulary of
  well-known control ids, not a plugin hook. App-specific controls are a
  renderer-toolkit per-field override
  ([gui_renderer_toolkit.md](gui_renderer_toolkit.md)), not a schema key.
- **No label / help / layout.** Captions and help are
  [gui_field_metadata.md](gui_field_metadata.md); sections/tabs/spans are
  [gui_layout_grouping.md](gui_layout_grouping.md). This spec chooses the control
  only.
- **No option-count-driven auto radio/combo.** Whether a small `Choice` renders
  as radios is an explicit `x-widget` decision, not inferred from a live option
  count — the option count is a runtime property of the options action
  ([choice.md](../spec/choice.md)), not known at schema-generation time.

## Testing (planned)

- A plain `std::string` / `int64_t` / `bool` field with no wrapper and no
  override emits no `x-widget` and renders with today's type-default control
  (regression guard).
- A `Multiline` field emits `x-widget = "textarea"` and serialises as a plain
  JSON string on the wire (wire-unchanged guard).
- A `Ranged<0, 100, 5>` field emits `x-widget = "slider"` with `x-min = 0`,
  `x-max = 100`, `x-step = 5`, and serialises as a plain number.
- A `FieldMeta::widget` override wins over a wrapper's derived `widget()` for the
  same field; a `FieldMeta::widget` on a plain type emits `x-widget` with no
  wrapper present.
- A `Ranged` participates in `required` derivation and `allRequiredEngaged` via
  `hasValue()` exactly like `Choice` (engaged/empty behaviour).
- A renderer ignoring `x-widget` produces a usable form for every field (no
  control id is load-bearing).

## Cross-references

- [gui_overview.md](gui_overview.md) — the derive-from-type-first principle, the
  wrapper-type family (`Quantity`/`Choice`) this extends, and the additive-`x-*`
  versioning stance.
- [forms.md](../spec/forms.md) — `schemaJson<A>()`, `mergeSchemaExtras`,
  `forEachNamedMember`, `EmptyCapableField` / `required` derivation the new
  wrappers plug into, the `x-decimalPlaces` entry-granularity precedent, and the
  renderer-contract table these keys extend.
- [choice.md](../spec/choice.md) — the type-carries-intent / wire-carries-value
  pattern (`glz::meta` reflecting the payload) the new wrappers follow.
- [gui_field_metadata.md](gui_field_metadata.md) — the `FieldMeta` descriptor
  that also carries the `x-widget` override, keeping one per-field surface.
- [gui_layout_grouping.md](gui_layout_grouping.md) — where the selected controls
  are positioned.
- [gui_renderer_toolkit.md](gui_renderer_toolkit.md) — per-field widget-override
  slots for app-specific controls beyond the `x-widget` vocabulary.
