# GUI renderer toolkit — reference renderer, conformance kit, theming slots (planned)

> **Status: planned — not yet implemented.** This is the **ecosystem** layer of
> the GUI program ([gui_overview.md](gui_overview.md)). It turns the reference
> renderer from an *example* into a *shipped, reusable* component, adds a
> **conformance test kit** that formalises the "normative" renderer contract of
> [forms.md](../spec/forms/forms.md), and defines a **theming / component-override**
> registry so an app swaps one field's control without forking the renderer. It
> consumes — and does not change — the `x-*` action schema
> ([forms.md](../spec/forms/forms.md)) and the `v-*` / `w-*` / `app-*` view schemas
> ([gui_collections_views.md](gui_collections_views.md),
> [gui_workflows_navigation.md](gui_workflows_navigation.md)). See
> [todo.md](../todo.md).

## The gap

The only renderer is `examples/forms/gui_qml` — a ~425-line `DynamicForm.qml`
plus `Main.qml` ([forms.md](../spec/forms/forms.md)). It is an **example**, not a
component: an app that wants schema-driven forms copies and forks it. Three
things are missing:

- **No shipped renderer.** The QML that consumes `schemaJson<A>()` (the
  `resolveProp` dual-read, the exact rational digit arithmetic, the unit selector,
  the options fetch) lives in an example directory, so every consumer
  re-implements or vendors it. It is a reference implementation trapped as demo
  code.
- **No conformance kit.** [forms.md](../spec/forms/forms.md) calls its `x-*` vocabulary
  **normative** ("the normative list of every key a renderer must understand"),
  but nothing lets a *new* renderer (web, ImGui) prove it honors that contract.
  Correctness of a renderer is currently "read the prose and hope."
- **No override seam.** A field's control is chosen by the renderer's built-in
  logic (`isChoice` → combo, `isQuantity` → number + unit selector). An app that
  wants a slider for one field, or its own date picker, must fork `DynamicForm`.
  [gui_overview.md](gui_overview.md) promises "every generated affordance
  overridable without forking the renderer" — that seam does not exist.

## Goal

Three deliverables, each additive and opt-in:

1. A **reusable reference renderer** shipped as a library (Qt/QML **first**,
   seeded by the existing `DynamicForm`), with the schema-consuming logic factored
   out of the example so an app depends on it directly.
2. A **conformance test kit** — a schema corpus plus expected-behavior assertions
   — a new renderer runs to prove it honors the `x-*` (and `v-*` / `w-*` /
   `app-*`) contract.
3. A **theming / component-override registry**, keyed by `x-widget` / unit / type,
   so an app substitutes one field's control while the rest of the renderer is
   untouched.

The **schema contract stays renderer-agnostic** ([gui_overview.md](gui_overview.md)):
QML is the reference, every deliverable is specified in platform-neutral terms so
a web / ImGui renderer implements the same contract, and QML snippets here are
illustrative of *one* conformant renderer, never normative.

## Design

### 1. The reference renderer as a shipped component

Factor the schema-consuming logic — today embedded in `DynamicForm.qml` — into a
distributed QML module (proposed `MorphForms` module, promoted from the example's
`FormsController`/`DynamicForm` seed) that an app imports rather than copies. The
factoring keeps the renderer-agnostic core distinct from the QML surface:

- **Renderer-agnostic core (documented behavior, any language).** The algorithms
  the contract *requires*, independent of Qt: `$ref` resolution merging def +
  property (`resolveProp` — property `x-*` wins, `ExtUnits` comes from the def);
  field ordering by `x-order`; the **exact** rational assembly and unit conversion
  (`scaledDigits` / `mulDigits` / `divRoundDigits` / `rationalJson` /
  `convertText` — payloads stay canonical, unit switches recompute exactly via
  the `num`/`den` ratio); the required-field submit gate; and the options-fetch /
  `optionRows` extraction. These are specified as the **behavioral contract** a
  conformant renderer implements, and are exactly what the kit (below) tests.
- **QML surface (the reference).** The `Repeater`-over-`fields` layout, the
  `TextField`/`ComboBox`/`DateTimePicker` controls, and the auto-fire-on-ready
  wiring stay QML but move behind the module boundary so they are reused, not
  forked. `Main.qml`'s "enumerate schemas" role is subsumed by the `app-*` shell
  ([gui_workflows_navigation.md](gui_workflows_navigation.md)) when present.

QML **first** because it already consumes the contract; the module boundary is
drawn so the agnostic core is documented for a web/ImGui port to reimplement.
This is a packaging and factoring change — **no `x-*` key changes**, and a plain
single-action form renders identically to today.

### 2. The conformance test kit

A renderer proves it honors the contract by consuming a **schema corpus** and
satisfying a set of **expected-behavior assertions**. Both ship with morph and
are renderer-agnostic (the corpus is JSON; the assertions are described in
platform-neutral terms a harness in any language checks).

**Corpus** — one schema per contract feature, generated by `schemaJson<A>()` /
the view-schema emitters from fixture action/view types so the corpus never
drifts from what the framework actually emits:

| Fixture | Exercises |
|---|---|
| plain scalars + `required` | field order (`x-order`), required-gate, integer bounds |
| `Quantity` with alternatives | `ExtUnits` def read, `x-decimalPlaces` step, `x-unitAlternatives` exact recompute |
| `Choice` | `x-optionsAction` fetch, `x-optionValue`/`x-optionLabel` mapping, empty-body query |
| `Timestamp` | `format: "date-time"` control + ISO-8601 wire value |
| `$ref` to shared `$def` | mandatory dual-read (property `x-*` beats def) |
| collection view | `v-query` populate, `v-columns` derivation, `v-rowAction` bind |
| wizard / app | `w-steps` prefill threading, `app-menu` routing |

**Assertions** — the observable, renderer-independent behaviors each fixture
must produce, proposed as a `morph::conformance` description the harness drives:

- Fields render in `x-order`, not JSON key order.
- Submission is blocked until every `required` field is engaged, and enabled once
  they are (`allRequiredEngaged` parity, [forms.md](../spec/forms/forms.md)).
- A `Quantity` payload is emitted as `{num,den,dp}` **exactly** (the corpus pins
  expected `num`/`den` for a given typed decimal), and a unit switch recomputes
  the entry exactly with no float drift.
- A `Choice` executes its options action with an empty body and submits
  `valueField`, displays `labelField`.
- The wire body a renderer produces for a filled fixture **byte-matches** (modulo
  key order) the expected canonical body the corpus pins.

**Accessibility assertions.** Generated forms must be *operable*, not just
visible, so the kit carries an accessibility slice alongside the functional
one:

- every control exposes an **accessible name** — the property's `title`
  (falling back to the wire key), so schema-driven labels reach the platform
  accessibility tree;
- **focus order follows `x-order`** (and group order under `x-layout`), never
  visual coincidence;
- every control class is **keyboard-operable** — combo, date, unit selector
  and slider included; no pointer-only affordance;
- a violated rule / blocked submit is **announced**, not merely tinted: the
  message the renderer shows is exposed as the control's accessible
  description.

Like every other assertion these are platform-neutral behaviors; the QML
reference implements them with Qt's `Accessible` attached properties, a web
renderer with ARIA. Locale fixtures ([gui_i18n.md](gui_i18n.md)) run the same
assertions under translation — the accessible name is the *translated* title
when a catalog is installed.

The kit is the executable form of forms.md's "normative" claim: a renderer that
passes it demonstrably honors the vocabulary; a renderer that ignores an `x-*`
key fails the specific assertion for that affordance while still passing the
baseline form assertions (the documented graceful-degradation fallback).

### 3. Theming / component-override registry

A **NEW**, optional client-side registry lets an app override *which control*
renders a field, keyed by the schema attributes a renderer already reads, without
touching the renderer. It introduces **one new optional schema key** and a
client-side registry:

- **`x-widget` (NEW optional action-schema key).** A hint naming a control
  variant when the type alone is ambiguous — e.g. `"slider"` vs the default
  number field, `"radio"` vs the default combo. It is a Tier-1 widget-hints key
  (its full definition belongs to [gui_widget_hints.md](gui_widget_hints.md),
  [gui_overview.md](gui_overview.md)); the toolkit only defines how the renderer
  *dispatches* on it. Absent `x-widget`, the renderer picks the control from the
  type exactly as today — so `x-widget` is purely additive and ignorable.
- **The override registry (client-side, no schema involvement).** A lookup a
  host populates at startup, resolved per field in priority order:

  ```cpp
  // namespace morph::render — NEW. Client-side only; never on the wire.
  struct SlotRegistry {
      // Most specific wins: exact field id, then x-widget, then unit id,
      // then JSON type. A miss falls back to the built-in control.
      void byField (std::string_view action, std::string_view field, Slot);
      void byWidget(std::string_view xWidget, Slot);   // e.g. "slider"
      void byUnit  (std::string_view unitAscii, Slot); // e.g. "kg_per_m3"
      void byType  (std::string_view jsonType, Slot);  // e.g. "integer"
      // Slot is a renderer-native factory (a QML Component in the QML renderer).
  };
  ```

  Resolution order is **field → `x-widget` → unit → type → built-in default**, so
  an app overrides exactly one field, or every field of a unit, or a whole type,
  at the granularity it needs. A slot receives the resolved field descriptor (the
  merged def+property node) and the same set-value callback the built-in controls
  use, so an override participates in the required-gate and auto-fire without
  special-casing.

The registry is **entirely client-side** — it never appears in the schema or on
the wire; two renderers of the same schema may register different slots. This is
[gui_overview.md](gui_overview.md)'s "escape hatch always available" made
concrete: swap one control without forking, and, at the extreme, consume the
schema directly.

## Non-goals

- **Not a UI toolkit or widget library.** morph ships the *schema-consuming
  renderer* and the *override seam*, not a styled component set. Visual theming
  (colors, spacing, fonts) is the host toolkit's job; the "theming" here is
  **component substitution**, not a design system.
- **No schema/contract change for the reference renderer.** Shipping the renderer
  as a library is packaging; the only new *schema* key is the optional `x-widget`
  hint (owned by widget-hints), and it is additive and ignorable.
- **Not a cross-renderer runtime.** The conformance kit checks *behavior against
  the contract*; it does not make renderers interchangeable at runtime or define a
  plugin ABI. Each renderer is its own program that passes the same assertions.
- **QML is first, not exclusive — but morph ships one.** The agnostic core is
  documented so a web/ImGui renderer can be built and run against the kit; morph
  itself ships and maintains the QML reference, not a web or ImGui renderer.
- **No server involvement.** The renderer, kit, and slot registry are client-side.
  Nothing here touches dispatch, the wire, or `RemoteServer`
  ([backend.md](../spec/core/backend.md)); the server still only sees action payloads.
- **The kit does not test look.** It asserts *contract behavior* (order, gating,
  exact payloads, options fetch), never pixels or styling — those are
  renderer-specific and out of scope.

## Testing (planned)

- The extracted `MorphForms` module renders every `examples/forms` fixture
  identically to the current in-example `DynamicForm` (regression guard that
  factoring changed no behavior).
- The QML reference renderer runs the conformance kit and passes every assertion;
  a deliberately broken renderer (ignores `x-order`, or rounds a `Quantity`)
  fails exactly the corresponding assertion and no others.
- The corpus is regenerated from fixture types via `schemaJson<A>()` / the
  view-schema emitters and diffed, so a change to an emitter that alters the
  contract is caught as a corpus diff (drift guard).
- Slot resolution honors priority: a `byField` override beats a `byWidget` beats a
  `byUnit` beats a `byType`, and a miss falls back to the built-in control; an
  override participates in the required-gate and auto-fire like a built-in.
- An `x-widget: "slider"` field with no registered slot renders the default
  number control (additive/ignorable key), and with a registered `byWidget`
  slider renders the override.
- The QML reference passes the accessibility slice: accessible names match
  `title`, focus traversal matches `x-order`, every fixture control operates
  keyboard-only, and rule violations surface as accessible descriptions.

## Cross-references

- [gui_overview.md](gui_overview.md) — the ecosystem layer this document
  occupies; the "renderer-agnostic contract, QML reference" and
  "escape-hatch/override" principles the three deliverables realise.
- [forms.md](../spec/forms/forms.md) — the **normative** `x-*` vocabulary and renderer
  contract (the `$ref` dual-read, `x-order`, `ExtUnits`, `x-decimalPlaces`,
  `x-unitAlternatives`, `x-optionsAction`) the reference renderer consumes and the
  conformance kit formalises into executable assertions.
- [choice.md](../spec/forms/choice.md) — the options-fetch / `optionRows` behavior the
  renderer implements and the kit asserts.
- [gui_i18n.md](gui_i18n.md) — the `TranslationProvider` seam the shipped
  renderer hosts, and the locale fixtures the kit runs (including under the
  accessibility assertions).
- [gui_collections_views.md](gui_collections_views.md) /
  [gui_workflows_navigation.md](gui_workflows_navigation.md) — the `v-*` / `w-*` /
  `app-*` view schemas the shipped renderer also consumes and the kit's collection
  / wizard / app fixtures exercise.
- [bridge.md](../spec/core/bridge.md) — the `set<>` / auto-fire path a slot override
  participates in, unchanged.
- [backend.md](../spec/core/backend.md) — unchanged; the renderer, kit, and registry
  are client-side and never touch dispatch or the wire.
