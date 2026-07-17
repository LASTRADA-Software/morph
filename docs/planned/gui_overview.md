# GUI enhancement program — overview (planned)

> **Status: planned — not yet implemented.** This is the umbrella spec for the
> GUI-generation enhancement program. It frames the goal, the guiding principle,
> and how the individual GUI specs (`gui_*.md`) layer on top of the existing
> schema-driven forms surface ([forms.md](../spec/forms.md), [choice.md](../spec/choice.md)).
> The individual features each have their own spec; this file is the map. See
> [todo.md](../todo.md).

## The goal

Make GUI development against morph **rapid by default and flexible when needed**.
The user supplies a model and plain-aggregate action types; from those alone the
framework should generate a usable, good-looking GUI with as little extra
declaration as possible — and every generated affordance should be overridable
without forking the renderer.

Today the spine already exists: an action struct is turned into a JSON Schema by
`morph::forms::schemaJson<A>()`, enriched with `x-*` extension keys, and a
renderer builds a form from it ([forms.md](../spec/forms.md)). The current surface
is **one action → one flat form**. This program extends that spine along two
tiers plus an ecosystem layer.

## The guiding principle: infer by default, declare to override

Every feature in this program obeys one rule, which is what reconciles "rapid"
with "flexible":

1. **Infer from the type where possible.** A `Quantity` field already knows its
   unit and precision; a `Choice` already knows its options action; a
   `std::optional` already means "not required." The renderer should get as far
   as it can from types alone, with zero extra user declaration.
2. **Declare to override.** When inference is ambiguous or insufficient (a label,
   a layout group, a widget choice, a cross-field rule), the user adds a
   *typed, compile-time* declaration — a `static constexpr` member or a small
   registration macro on the action. Never mandatory; absence falls back to a
   sensible convention.
3. **Escape hatch always available.** The schema is a documented, stable
   contract ([forms.md](../spec/forms.md) "Renderer contract"). Anything the
   generated GUI cannot express, an app builds by consuming the schema directly
   or overriding one field's widget (see [gui_renderer_toolkit.md](gui_renderer_toolkit.md)).

This is why the constraints the program puts on the user's model are light: flat,
default-constructible, reflectable aggregates whose fields come from the known
palette (`Quantity`, `Choice`, `Timestamp`, primitives, or a user type exposing
`hasValue()`), plus *optional* typed declarations. Convention buys rapid;
override + direct-schema-consumption buys flexible.

## The two tiers and the ecosystem layer

### Tier 1 — richer forms (polish a single action's form until it looks bespoke)

Purely additive metadata and logic on top of the existing single-action form.
Each emits new additive schema keys — `x-*` extensions or standard JSON-Schema
annotations (`title`, `description`) — that a renderer ignores harmlessly if
unsupported.

| Spec | Adds |
|---|---|
| [gui_field_metadata.md](gui_field_metadata.md) | Labels, help text, placeholders, read-only, hidden — per-field presentation. |
| [gui_layout_grouping.md](gui_layout_grouping.md) | Sections, tabs, accordions, column spans — visual structure over the flat field list. |
| [gui_widget_hints.md](gui_widget_hints.md) | Control selection (multiline, slider, radio-vs-combo) — derived from type, annotated when ambiguous. |
| [gui_cross_field_rules.md](gui_cross_field_rules.md) | A typed rule vocabulary (required-when, comparisons, one-of) evaluated on **both** client and server. |
| [gui_computed_fields.md](gui_computed_fields.md) | Derived read-only fields recomputed live client-side and authoritatively server-side. |
| [gui_dependent_choices.md](gui_dependent_choices.md) | `Choice` options parameterised by sibling field values (cascading picklists). |
| [gui_i18n.md](gui_i18n.md) | Localised display text — stable message keys derived from the schema, a renderer-side catalog seam, and locale formatting duties. Cross-cutting: fixes the key scheme the other Tier-1 declarations translate through. |

### Tier 2 — app generation (climb from "form" to "screens")

Introduces a **view/app schema layer above the action schema**: descriptors that
compose existing action-forms into lists, master-detail screens, wizards, and a
navigable app shell. Larger architectural commitment; each screen is still built
from Tier-1 action-forms.

| Spec | Adds |
|---|---|
| [gui_collections_views.md](gui_collections_views.md) | List/table + master-detail views generated from query + edit + delete action *sets*. |
| [gui_workflows_navigation.md](gui_workflows_navigation.md) | Multi-step wizards (shared draft across actions) and an app-shell/route descriptor (menu → screens). |

### Ecosystem — make renderers cheap to own

| Spec | Adds |
|---|---|
| [gui_renderer_toolkit.md](gui_renderer_toolkit.md) | A reusable reference renderer (Qt/QML first), a renderer conformance test kit, and per-field widget-override / theming slots. |

## Versioning stance (unchanged, deliberately)

Per [forms.md](../spec/forms.md), the emitted schema is **unversioned**, and the
program keeps it that way: **every new key each `gui_*.md` spec introduces is
additive and optional — an `x-*` extension, a standard JSON-Schema annotation
(`title`, `description`), or a new top-level view-schema document — and an older
renderer can safely ignore it.** Adding such a key is explicitly *not* a breaking
change. Renaming, retyping, or changing the meaning of an existing key remains a
breaking change reserved for a major release. Each spec must state, in its
design, that its keys are additive and name the fallback a renderer that ignores
them exhibits.

(If a negotiated version is ever wanted, [protocol_versioning.md](protocol_versioning.md)
is the mechanism to build on — but this program does not require it.)

## Reference render target

The **Qt/QML client** (`examples/forms/gui_qml`) is the reference renderer the
specs write concrete examples against, because it already consumes the schema
contract. The **schema contract itself is renderer-agnostic**: every `x-*` key
and view-schema document is specified in platform-neutral terms so a web, ImGui,
or other renderer can implement the same contract. Where a spec shows a QML
snippet it is illustrative of *one* conformant renderer, never normative for the
contract.

## How the specs relate to existing work

- **Builds directly on:** [forms.md](../spec/forms.md) (schema generation, the
  `x-*` vocabulary, `allRequiredEngaged`), [choice.md](../spec/choice.md)
  (`Choice`/`FixedString`), [quantity_type.md](../spec/quantity_type.md),
  [datetime.md](../spec/datetime.md).
- **Cross-field rules tie into** [validation.md](validation.md): one rule
  declaration should drive the schema's `required`/`x-rules`, the client submit
  gate, *and* the planned server-side validator — no drift between them.
- **Reactive forms build on** the `subscribe`/`set<>` draft mechanism in
  [bridge.md](../spec/bridge.md); computed fields and wizards extend it.
- **Nothing here changes the wire or dispatch semantics** — the GUI program is an
  opt-in layer over registration, schema generation, and the existing dispatch
  paths, exactly as the forms layer is today.

## Cross-references

- [forms.md](../spec/forms.md) — the schema-generation spine this program extends;
  the normative `x-*` key vocabulary and renderer contract.
- [choice.md](../spec/choice.md) — `Choice`/`FixedString`, the pattern the
  dependent-choices and view specs generalise.
- [bridge.md](../spec/bridge.md) — the reactive `subscribe`/`set<>` draft path the
  computed-field and wizard specs build on.
- [validation.md](validation.md) — the planned server-side validation the
  cross-field rules must share a declaration with.
- [todo.md](../todo.md) — the program's execution order and where it sits among
  the other planned work.
