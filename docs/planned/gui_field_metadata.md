# GUI field metadata — labels, help, placeholders, read-only, hidden (planned)

> **Status: planned — not yet implemented.** This spec extends the GUI program
> umbrella ([gui_overview.md](gui_overview.md)) and the schema-generation spine
> ([forms.md](../spec/forms.md)). It is a Tier-1 richer-forms feature: purely
> additive presentation metadata on a single action's flat form. See
> [todo.md](../todo.md).

## The gap

Today `morph::forms::schemaJson<A>()` emits enough for a renderer to *place* and
*type* a field — `x-order` for layout order, `x-decimalPlaces` / `x-unitAlternatives`
for `Quantity`, `x-optionsAction` / `x-optionValue` / `x-optionLabel` for `Choice`,
plus glaze's `type`/bounds/`format` and any `description` from a `glz::json_schema<A>`
specialisation ([forms.md](../spec/forms.md), "Renderer contract"). What it does
**not** provide, short of hand-authoring a `glz::json_schema<A>` block, is
per-field *presentation* metadata:

- **No label.** A renderer has only the raw wire key (`sampleId`, `dryMassPct`)
  to display; there is no human title distinct from the member name.
- **No help / placeholder text.** `description` exists (glaze) but there is no
  distinction between descriptive help and an in-control placeholder hint, and
  nothing derives either from the field name.
- **No read-only or hidden signal.** Every emitted property is an editable,
  visible control. A field that should be displayed-but-not-edited, or carried in
  the payload but never shown, cannot be expressed without dropping it from the
  action type entirely.

The forms layer already establishes the pattern for the fix: metadata that is a
compile-time property of the action belongs *in the type or a `static constexpr`
declaration*, surfaced through the schema as `x-*` keys — exactly how `Choice`
carries its options source ([choice.md](../spec/choice.md)) and how
`optionalFields` (verified in `forms.hpp`) opts a field out of `required`.

## Goal

Let an action declare per-field presentation — label, help, placeholder,
read-only, hidden — with the umbrella's **infer by default, declare to override**
discipline ([gui_overview.md](gui_overview.md)):

1. **Infer a label from the member name** (`dryMassPct` → "Dry Mass Pct" by a
   title-case split) so the common case needs *zero* declaration.
2. **Declare to override** the inferred label and to add help / placeholder /
   read-only / hidden, via a typed `static constexpr` descriptor on the action.

Flat, unannotated actions render exactly as today.

## Design

### A typed field-descriptor declaration (NEW)

An action opts in by exposing a `static constexpr` array of field descriptors,
`fieldMetadata`, mirroring the existing `optionalFields` convention (a
`static constexpr` iterable the generator already looks for — verified in
`forms.hpp`). Each entry names a member by its wire key and carries the
overrides for it:

```cpp
// namespace morph::forms — NEW.
struct FieldMeta {
    std::string_view field;                 // wire key of the member
    std::string_view label{};               // "" = infer from name
    std::string_view help{};                // "" = omit x-help / description
    std::string_view placeholder{};         // "" = omit x-placeholder
    bool readOnly{false};
    bool hidden{false};
};

struct RecordMeasurement {
    Choice<std::int64_t, "ListSamples"> sampleId;
    Density  density{};
    Moisture moisture{};

    static constexpr std::array fieldMetadata{
        FieldMeta{.field = "sampleId", .label = "Sample",
                  .help = "Which logged sample this measurement belongs to."},
        FieldMeta{.field = "density",  .placeholder = "e.g. 1050"},
        FieldMeta{.field = "moisture", .readOnly = true},
    };
};
```

A single-field convenience helper, `describe<&Action::field>(...)`, is proposed
as **NEW** sugar that produces one `FieldMeta` with the member's name filled in
from the pointer-to-member, so the field name is never restated as a string:

```cpp
static constexpr std::array fieldMetadata{
    describe<&RecordMeasurement::sampleId>("Sample", "Which logged sample…"),
    describe<&RecordMeasurement::moisture>().readOnly(),
};
```

Both forms compile to the same `std::array<FieldMeta, N>`; an action may use
either. Absence of `fieldMetadata` leaves every field at its inferred defaults.

### Label inference (NEW)

When no descriptor overrides a field's label, the generator synthesises a
`title` from the wire key: split on camel-case and underscore boundaries,
capitalise each word (`dryMassPct` → "Dry Mass Pct", `sample_id` → "Sample Id").
This is a pure function of the member name known at schema-generation time, so it
costs nothing per action. A descriptor's non-empty `label` always wins over the
inferred title.

### Emitted keys

`mergeSchemaExtras<A>` (verified in `forms.hpp`) gains a pass that, for each
reflected member (via `forEachNamedMember`, verified), looks up any matching
`FieldMeta` and patches the property node — the same property node that already
carries `x-order` and the `Choice`/`Quantity` keys ([forms.md](../spec/forms.md),
"Where the keys physically land"). Label maps onto the standard JSON-Schema
`title`; help maps onto standard `description` (so a renderer that already reads
glaze's `description` needs no change); the rest are `x-*` extensions:

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `title` | property node (sibling of `$ref`) | string | The field's display label — an explicit `label`, else the inferred title-cased member name. Always emitted. The renderer uses it as the control's caption instead of the raw wire key. Standard JSON-Schema vocabulary, not an `x-*` key. |
| `description` | property node (sibling of `$ref`) | string | Help text for the field, from `FieldMeta::help`. Omitted when empty. Standard JSON-Schema vocabulary; a renderer shows it as helper/tooltip text. A `FieldMeta::help` overrides any `description` glaze stamped from a `glz::json_schema<A>` block. |
| `x-placeholder` | property node (sibling of `$ref`) | string | In-control placeholder / hint text shown while the field is empty. Omitted when empty. Advisory: the renderer shows it inside the empty control; it is never submitted. |
| `x-readonly` | property node (sibling of `$ref`) | boolean | `true` when the field should be displayed but not editable. Emitted only when `true`. The renderer disables entry; the field still appears in the payload with its default/current value. Not a security control (see Non-goals). |
| `x-hidden` | property node (sibling of `$ref`) | boolean | `true` when the field should not be shown at all. Emitted only when `true`. The renderer omits the control but the field remains part of the action payload (submitted at its default/current value). |

All five keys are **additive and non-breaking**: they extend the
[forms.md](../spec/forms.md) contract table without renaming or retyping any
existing key, exactly as [gui_overview.md](gui_overview.md)'s versioning stance
requires. A renderer that ignores them **falls back to today's behaviour** — it
shows the raw wire key as the caption, no helper/placeholder text, and every
field editable and visible. No affordance is lost that exists today; only the
new polish is skipped.

### Illustrative renderer read (non-normative)

One conformant QML renderer might, after resolving the property `$ref`:

```qml
// title/description/x-placeholder/x-readonly/x-hidden read from the property node
Label   { text: prop["title"] }
TextField {
    placeholderText: prop["x-placeholder"] ?? ""
    enabled: !(prop["x-readonly"] ?? false)
    visible: !(prop["x-hidden"] ?? false)
    ToolTip.text: prop["description"] ?? ""
}
```

This is illustrative of *one* renderer; the contract stays renderer-agnostic.

## Non-goals

- **`x-hidden` / `x-readonly` are not security controls.** Both keys are
  presentation only — the field still travels in the payload and a hand-built
  wire envelope can set it freely. Enforcement stays server-side
  ([validation.md](validation.md), [forms.md](../spec/forms.md)'s trust
  boundary). A truly secret field must not be a member of the action at all.
- **No i18n.** Labels and help are baked into the one cached schema per type
  ([forms.md](../spec/forms.md), "no localisation"). Translated captions need the
  separate mechanism forms.md defers, not this spec.
- **No widget selection.** *Which* control renders a field is
  [gui_widget_hints.md](gui_widget_hints.md); this spec only labels and annotates
  whatever control that spec (or the field type) selects.
- **No layout / grouping.** Sections, tabs, and column spans are
  [gui_layout_grouping.md](gui_layout_grouping.md); `fieldMetadata` never affects
  placement beyond `x-order` (unchanged).
- **No cross-field logic.** Conditional read-only / hidden ("read-only *when*
  another field is X") is [gui_cross_field_rules.md](gui_cross_field_rules.md);
  `x-readonly` / `x-hidden` here are static, unconditional booleans.

## Testing (planned)

- An action with no `fieldMetadata` emits `title` inferred from each member name
  and no `x-placeholder` / `x-readonly` / `x-hidden`; the rest of the schema is
  byte-identical to today (regression guard).
- A `FieldMeta` with a `label` overrides the inferred `title`; a `help` lands as
  `description` and overrides any glaze-stamped `description`; a `placeholder`
  lands as `x-placeholder`.
- `readOnly` / `hidden` emit `x-readonly` / `x-hidden` **only** when `true`
  (absent otherwise, keeping the schema minimal).
- `describe<&A::field>(…)` and the explicit `FieldMeta{.field="…"}` form produce
  identical property annotations for the same field.
- Label inference: `dryMassPct` → "Dry Mass Pct", `sample_id` → "Sample Id",
  a single-word `notes` → "Notes".
- A `FieldMeta` naming a field that does not exist on the action is ignored (no
  crash, no stray property) — consistent with schema generation never throwing.

## Cross-references

- [gui_overview.md](gui_overview.md) — the infer-by-default / declare-to-override
  principle and the additive-`x-*` versioning stance this feature obeys.
- [forms.md](../spec/forms.md) — `schemaJson<A>()`, `mergeSchemaExtras`,
  `forEachNamedMember`, the `optionalFields` convention this mirrors, the
  property-node vs `$def` placement rule, and the renderer-contract table these
  keys extend.
- [choice.md](../spec/choice.md) — the pattern of carrying field metadata in the
  type / declaration and surfacing it through property-level `x-*` keys.
- [gui_layout_grouping.md](gui_layout_grouping.md) — visual structure (sections,
  tabs, spans) that composes with these per-field labels.
- [gui_widget_hints.md](gui_widget_hints.md) — control selection, which this
  spec's labels and help decorate.
- [validation.md](validation.md) — why `x-hidden` / `x-readonly` are not a
  server-side boundary.
