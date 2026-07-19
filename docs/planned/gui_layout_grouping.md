# GUI layout & grouping — sections, tabs, spans (planned)

> **Status: planned — not yet implemented.** This spec extends the GUI program
> umbrella ([gui_overview.md](gui_overview.md)) and the schema-generation spine
> ([forms.md](../spec/forms/forms.md)). It is a Tier-1 richer-forms feature: visual
> structure layered over the existing flat field list, additive and opt-in. See
> [todo.md](../todo.md).

## The gap

`morph::forms::schemaJson<A>()` emits a **flat** form. The only layout signal is
`x-order` — the member's 0-based declaration index — which a renderer uses to lay
fields out top-to-bottom in declaration order ([forms.md](../spec/forms/forms.md),
"Renderer contract"). There is no way to express structure *over* that flat list:

- **No sections / fieldsets.** Related fields (all the address fields, all the
  measurement fields) cannot be visually grouped under a heading.
- **No tabs or accordions.** A long action cannot be split into named panes; the
  renderer must show every field in one scroll.
- **No column spans / grid.** Every field occupies a full row; a short field
  (a code, a checkbox) cannot share a row with another, and a wide field (notes)
  cannot span the whole width deliberately.

Grouping is a compile-time property of the action — which fields belong together
is known when the struct is written — so, per [gui_overview.md](gui_overview.md),
it belongs in a `static constexpr` declaration surfaced through `x-*` keys, the
same shape as the existing `optionalFields` convention the generator already
reads (verified in `forms.hpp`) and the planned `fieldMetadata` descriptor
([gui_field_metadata.md](gui_field_metadata.md)).

## Goal

Let an action declare visual structure — sections, an optional tab/accordion
container, and per-field column spans — with **infer by default, declare to
override**: absent any declaration the form is the flat, `x-order`-ordered list
of today. When a layout descriptor is present, the renderer arranges fields into
the declared groups; a renderer that does not support grouping still renders
every field, flat, losing only the visual chrome.

## Design

### A typed layout descriptor (NEW)

The action exposes a `static constexpr` `formLayout` — a list of **groups**, each
mapping a section title (and an optional container kind) to an ordered list of
member wire keys. Per-field spans are a parallel `static constexpr` list, keyed
by wire key, so spans compose with (rather than duplicate) the
[gui_field_metadata.md](gui_field_metadata.md) descriptor:

```cpp
// namespace morph::forms — NEW.
enum class GroupKind { Section, Tab, Accordion };

struct FieldGroup {
    std::string_view title;                     // section / tab / panel heading
    GroupKind kind{GroupKind::Section};
    std::span<const std::string_view> fields;   // member wire keys (membership;
                                                // intra-group order is x-order)
};

struct FieldSpan {
    std::string_view field;                     // wire key
    int colspan{1};                             // grid columns this field spans
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

Each group carries its own `kind`: the default `Section` renders as a titled
fieldset stacked vertically, consecutive `Tab` groups render as panes of one
shared tab bar, and an `Accordion` group renders as a collapsible panel. Mixing
kinds is allowed (as above: two sections plus an accordion) but a renderer may
downgrade an unsupported kind to a plain section (the fallback).

### Interaction with `x-order`

`x-order` is unchanged and remains the authority on **intra-group** ordering. A
group's `fields` list gives the *membership* only, and the `formLayout` array
order gives the *cross-group order*; within a group the renderer still lays
fields out by ascending `x-order`. A field not
named in any group falls into an implicit trailing "ungrouped" section in
`x-order` order — so adding a group for *some* fields never hides the rest. This
keeps the flat default a special case: no `formLayout` ⇒ one implicit group
containing every field ⇒ today's flat, `x-order`-ordered form.

### Emitted keys

`mergeSchemaExtras<A>` gains a pass that, for each reflected member (via
`forEachNamedMember`, verified in `forms.hpp`), stamps its group identity and
span onto the property node, and writes the ordered group list to a single
top-level `x-layout` object so a renderer learns section titles, order, and
container kind without reconstructing them from per-field tags:

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `x-layout` | top-level (object) | object | The form's group structure: `{ "groups": [ { "title": string, "kind": "section"\|"tab"\|"accordion", "fields": [wire-key,…] }, … ] }` — each group carries its own `kind`, mirroring `FieldGroup::kind`. Emitted only when the action declares `formLayout`. The renderer builds the named containers in array order and places each field in its group; fields absent from every group go in a trailing default group. |
| `x-group` | property node (sibling of `$ref`) | string | The title of the group this field belongs to (redundant with `x-layout` for renderers that prefer a per-field lookup; both describe the same membership). Omitted for a field in the implicit default group. |
| `x-section` | property node (sibling of `$ref`) | non-negative integer | The 0-based index of this field's group in `x-layout.groups` — a stable numeric handle a renderer can sort/switch on without string comparison. Omitted when `x-layout` is absent, and (like `x-group`) for a field in the implicit default group. |
| `x-colspan` | property node (sibling of `$ref`) | positive integer | Number of grid columns the field should span, from `FieldSpan::colspan`. Emitted only when > 1. A renderer laying fields in a grid widens the control; a single-column renderer ignores it (field still shows full width). |

All four keys are **additive and non-breaking**: they extend the
[forms.md](../spec/forms/forms.md) contract table without touching any existing key,
per [gui_overview.md](gui_overview.md)'s versioning stance. A renderer that
ignores them **falls back to exactly today's flat form** — it drops `x-layout`,
`x-group`, `x-section`, and `x-colspan` and lays every field out top-to-bottom by
`x-order`. Grouping is chrome over the flat list, never a precondition for it.

### Illustrative renderer read (non-normative)

```qml
// x-layout drives the container; each field placed by its x-section index.
TabBar { Repeater { model: layout.groups; TabButton { text: modelData.title } } }
GridLayout {
    columns: 2
    Repeater {
        model: propsInSection(currentTab)
        delegate: FieldControl { Layout.columnSpan: prop["x-colspan"] ?? 1 }
    }
}
```

Illustrative of *one* renderer; the contract stays renderer-agnostic.

## Non-goals

- **No nested / recursive groups.** Groups are a single flat level over the flat
  field list — no group-within-group. This matches [forms.md](../spec/forms/forms.md)'s
  "flat actions only" scope; deep hierarchy would need nested action types the
  form generator does not descend into.
- **No responsive breakpoints.** `x-colspan` is a fixed column count, not a
  media-query grid. How a renderer reflows on a narrow viewport is the renderer's
  concern, not the schema's.
- **No per-field labels / help.** Titles here are *group* headings;
  per-**field** label, help, placeholder, read-only, hidden are
  [gui_field_metadata.md](gui_field_metadata.md).
- **No control selection.** Which widget fills each cell is
  [gui_widget_hints.md](gui_widget_hints.md); layout only positions controls.
- **No multi-action screens.** Composing several action-forms into a wizard or
  master-detail screen is Tier 2
  ([gui_workflows_navigation.md](gui_workflows_navigation.md),
  [gui_collections_views.md](gui_collections_views.md)); this spec structures a
  *single* action's form only.

## Testing (planned)

- An action with no `formLayout` emits no `x-layout` / `x-group` / `x-section`
  and a schema byte-identical to today (regression guard); the flat `x-order`
  form is unchanged.
- A `formLayout` with three `Section` groups emits an `x-layout` listing them in
  order, and every field carries the correct `x-group` / `x-section`.
- Fields omitted from all groups appear in a trailing default group; adding a
  group for some fields never drops the ungrouped ones.
- Intra-group order follows `x-order`, not the order of the group's `fields`
  list, when the two disagree.
- `Tab` / `Accordion` on a group sets that group's `kind` in `x-layout.groups`
  accordingly (other groups keep their own kinds); a `FieldSpan{colspan>1}`
  emits `x-colspan`, and `colspan == 1` emits nothing.
- A group naming a nonexistent field is ignored without crashing (schema
  generation never throws).

## Cross-references

- [gui_overview.md](gui_overview.md) — infer-by-default / declare-to-override and
  the additive-`x-*` versioning stance this feature obeys.
- [forms.md](../spec/forms/forms.md) — `schemaJson<A>()`, `mergeSchemaExtras`,
  `forEachNamedMember`, `x-order` (the intra-group ordering authority), the
  property-node placement rule, and the renderer-contract table these keys extend.
- [gui_field_metadata.md](gui_field_metadata.md) — per-field labels/help that
  decorate the controls this spec positions.
- [gui_widget_hints.md](gui_widget_hints.md) — control selection for the fields
  laid out here.
- [gui_workflows_navigation.md](gui_workflows_navigation.md),
  [gui_collections_views.md](gui_collections_views.md) — Tier-2 multi-action
  screen composition, above this single-action layout layer.
