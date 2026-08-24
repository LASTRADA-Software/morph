# `morph::forms` — schema generation & readiness for action types

Given a plain-aggregate action type `A` (registered with
[`BRIDGE_REGISTER_ACTION`](../core/registry.md#bridge_register_action)),
`morph::forms` produces a standard JSON Schema a client can render a form from,
and provides a compile-time `validate()` body that gates submission until every
required empty-capable field is filled in. It builds on glaze's
`write_json_schema<A>` (which already contributes types, `$defs`, per-field
metadata from `glz::json_schema<A>`, and `ExtUnits` — glaze's per-unit metadata
block, detailed under [Renderer contract](#renderer-contract-the-schema-key-vocabulary)
below — from `morph::units::Quantity`) and closes the gaps glaze leaves open.

## Design principle: infer by default, declare to override

Every feature below obeys one rule, which is what reconciles "rapid GUI
development" with "flexible when the generated form isn't enough":

1. **Infer from the type where possible.** A `Quantity` field already knows its
   unit and precision; a `Choice` already knows its options action; a
   `std::optional` already means "not required." The renderer gets as far as it
   can from types alone, with zero extra user declaration.
2. **Declare to override.** When inference is ambiguous or insufficient (a
   label, a layout group, a widget choice, a cross-field rule), the user adds a
   *typed, compile-time* declaration — a `static constexpr` member or a small
   registration macro on the action. Never mandatory; absence falls back to a
   sensible convention.
3. **Escape hatch always available.** The schema below is a documented, stable
   contract (see "Renderer contract"). Anything the generated GUI cannot
   express, an app builds by consuming the schema directly or overriding one
   field's widget (see "Theming / component-override registry").

This is why the constraints placed on a model's action types are light: flat,
default-constructible, reflectable aggregates whose fields come from the known
palette (`Quantity`, `Choice`, `Timestamp`, primitives, or a user type exposing
`hasValue()`), plus *optional* typed declarations. Convention buys rapid;
override + direct-schema-consumption buys flexible.

Every schema key this module and its siblings ([choice.md](choice.md),
[views.md](views.md), [workflows_navigation.md](workflows_navigation.md))
introduce is **additive and optional** — the emitted schema stays unversioned,
and a renderer that doesn't recognize a new `x-*` key, or a new top-level
view/wizard/app document, ignores it harmlessly. Renaming, retyping, or
changing the meaning of an existing key is the only kind of change reserved for
a major release.

The **Qt/QML client** (`src/qt/forms`) is the reference renderer these specs
write concrete examples against, because it already consumes the schema
contract; the **schema contract itself stays renderer-agnostic** — every
`x-*` key and view/wizard/app-schema document is specified in platform-neutral
terms so a web, ImGui, or other renderer can implement the same contract.

## Contents

- [Design principle: infer by default, declare to override](#design-principle-infer-by-default-declare-to-override)
- [Empty state — `EmptyCapableField` concept](#empty-state--emptycapablefield-concept)
- [`Choice` — server-sourced picklist](#choice--server-sourced-picklist)
- [`FixedString` — NTTP compile-time string](#fixedstring--nttp-compile-time-string)
- [Widget hints — `Multiline` / `Ranged`](#widget-hints--multiline--ranged)
- [`schemaJson<A>()` — schema generation](#schemajsona--schema-generation)
- [Field metadata — `FieldMeta`](#field-metadata--fieldmeta)
- [Layout & grouping — sections, tabs, spans](#layout--grouping--sections-tabs-spans)
- [Renderer contract: the schema key vocabulary](#renderer-contract-the-schema-key-vocabulary)
- [Shipped Qt/QML reference renderer](#shipped-qtqml-reference-renderer)
- [Renderer conformance kit](#renderer-conformance-kit)
- [Theming / component-override registry](#theming--component-override-registry)
- [Localisation — message keys and the catalog seam](#localisation--message-keys-and-the-catalog-seam)
- [`allRequiredEngaged<A>()` — readiness check](#allrequiredengageda--readiness-check)
- [Cross-field rules — the `x-rules` vocabulary](#cross-field-rules--the-x-rules-vocabulary)
- [Computed fields](#computed-fields)
- [Per-instance constraints — values that live in data](#per-instance-constraints--values-that-live-in-data)
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
module needs to know about a field to decide whether it counts as "engaged" —
**a field is engaged exactly when `hasValue()` returns `true`.** Every later
use of "engaged" in this document (required-ness, cross-field rules, computed
fields) means precisely this.

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
- `morph::util::Tagged<T, Tag>` — `hasValue()` always returns `true`: it wraps
  a *required* protocol scalar, not an optionally-empty one, so it opts into
  this concept the same way the others do but never gates submission (see
  [`tagged.md`](../util/tagged.md)).
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
          FixedString ValueField = "id", FixedString LabelField = "name",
          FixedString... DependsOn>
struct Choice {
    std::optional<T> value;
    // ...
};
```

- `T` — the value type submitted **on the wire** (the JSON payload exchanged
  between client and server over the transport — see [wire.md](../core/wire.md)
  for the envelope this travels inside; `int64_t` for ids, `string` for codes).
- `OptionsAction` — the registered action type id whose result provides options
  (executed with an empty body when `DependsOn` is empty, or with
  `{name: value, ...}` built from the `DependsOn` names otherwise; returns
  `{valueField, labelField, ...}` rows either way).
- `ValueField` / `LabelField` — which result-row fields carry the submitted value
  and the display label; both default to `"id"` / `"name"`.
- `DependsOn` — an optional trailing pack of sibling wire field names whose
  current values parameterise the options action (a cascading picklist);
  empty by default. See [choice.md](choice.md) for the full design.

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

## Widget hints — `Multiline` / `Ranged`

Two more thin wrappers carry rendering *control* intent in the type, in the
same spirit as `Choice`: a `Multiline` field is a `std::string` that should be
edited as a text area, and a `Ranged<Min, Max, Step>` field is a bounded
numeric that should be edited as a slider.

```cpp
struct Multiline {
    std::string value;
    static constexpr std::string_view widget() noexcept { return "textarea"; }
};

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

Both serialise through `glz::meta` as their bare payload — `Multiline` as a
plain JSON string, `Ranged` as a nullable number — so the wire is unchanged.
Neither type is `std::optional` itself, so both are *required* by the
[Required-ness rule](#required-ness-rule) unless opted out via
`optionalFields`. `Ranged` additionally satisfies `EmptyCapableField`
(`hasValue()` is `noexcept`), so it gates `allRequiredEngaged` exactly like
`Choice`; `Multiline` does not (a plain `std::string` payload has no
distinguishable "empty" state the forms module tracks) and so is always
considered engaged, same as an unwrapped `std::string` member.

`mergeSchemaExtras` emits `x-widget` on any property whose field type declares
a `noexcept static constexpr widget()` — the shape both types above expose —
and `x-min` / `x-max` / `x-step` on any property whose field type additionally
declares `min()` / `max()` / `step()` (the `Ranged` shape). An action may also
override the widget for *any* field — wrapped or plain — by naming it in the
same `static constexpr fieldMetadata` array the [field-metadata
feature](#field-metadata--fieldmeta) uses, as long as its entries expose
`.field` and a non-empty `.widget` (both string-view-convertible); this is
read structurally (duck-typed), so this header does not gain a named
dependency on `FieldMeta`'s declaration — any type shaped that way is
honoured, and the override always wins over a type's own derived `widget()`.
Full API, the `$defs`-collapse caveat shared with `Choice`, and design
rationale are in [widget_hints.md](widget_hints.md).

## `schemaJson<A>()` — schema generation

Produces a complete JSON Schema string for action type `A`, post-processing the
output of `glz::write_json_schema<A>()` to add seven annotation groups:

| Annotation | Scope | Contents |
|---|---|---|
| `required` | Top-level, and every nested-aggregate object schema (see [Nested aggregates (recursive, cycle-guarded)](#nested-aggregates-recursive-cycle-guarded)) | Array of field names that are **not** `std::optional<...>` and not listed in `A::optionalFields`. |
| `x-order` | Every property | The member's declaration index (0‑based), so a renderer lays fields out in declaration order regardless of JSON key ordering. |
| `x-decimalPlaces` | `Quantity` properties | The field's declared precision (`Quantity<U, Dec>::declaredDecimals`). |
| `x-unitAlternatives` | `Quantity` properties | Convertible display/entry units derived from `UnitTraits::relations`, each with `{id, display, decimals, num, den}` — `id`/`display`/`decimals` come from the alternative unit's `UnitMeta`, and `num`/`den` are the exact alternative-to-canonical ratio. Omitted entirely when the field's unit declares no convertible units. |
| `x-optionsAction` / `x-optionValue` / `x-optionLabel` | `Choice` properties | The action that serves the options and which result fields to use. |
| `x-optionsDependsOn` | `Choice` properties whose options depend on sibling fields | Wire names of the sibling fields that parameterise the options action; omitted when the `Choice` declares no dependency. |
| `x-widget` / `x-min` / `x-max` / `x-step` | Properties whose field type declares `widget()` (optionally `min()`/`max()`/`step()`), or any field named in a `fieldMetadata`-shaped override | The preferred control id, and (for a bounded numeric) the slider's track bounds and increment ([widget_hints.md](widget_hints.md)). |

The result is **computed once per type and cached** in a `static const std::string`
inside `schemaJson<A>()`. On internal failure (malformed intermediate JSON,
etc.) the unmerged glaze schema is returned — or an empty string when even
glaze's own `write_json_schema<A>()` failed, since `schemaJson` feeds
`mergeSchemaExtras` with `write_json_schema<A>().value_or(std::string{})`.

Schema generation throws in exactly **one** case, and never for malformed
input: an `A::formRules` declaration that contradicts `A`'s own derived
`required` array so completely that no submission could satisfy both — see
[Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules).

### Required-ness rule

Required is the default — the safer choice for domain forms, since forgetting
to mark a field optional loses data rather than silently accepting a gap (see
[Design decisions](#design-decisions) for the full rationale). A member is
*optional* (and therefore not added to `required`) when any of:
1. Its type is `std::optional<...>`, or
2. Its name appears in `A::optionalFields` — a `static constexpr` iterable of
   `std::string_view` that the action declares, or
3. It is the destination of an `A::computedFields` entry — a derived,
   read-only field is never something the user must fill in; see
   [Computed fields](#computed-fields).

Required-ness is derived here, and `x-rules` is derived from `A::formRules`,
**independently**. They can therefore disagree; the one disagreement that makes
the form unsubmittable is rejected at generation, see
[Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules).

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
    std::string_view widget{};              // widget-selection override (see widget_hints.md)
    bool readOnly{false};
    bool hidden{false};
    std::string_view i18nKey{};             // "" = derive the key stem; see below
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
| `x-i18nKey` | property node (sibling of `$ref`) | string | An explicit message-key **stem** override, from `FieldMeta::i18nKey`. Omitted when empty. Not a complete key by itself — see [Localisation — message keys and the catalog seam](#localisation--message-keys-and-the-catalog-seam) for how a renderer expands it per text slot. |
| `x-widget` | property node (sibling of `$ref`) | string | Control-selection override, from `FieldMeta::widget`. Omitted when empty. Full mechanism, precedence over a type's own derived `widget()`, and design rationale are in [Widget hints](#widget-hints--multiline--ranged). |

All seven keys are additive and non-breaking, extending the renderer-contract
table below without renaming or retyping any existing key, per this program's
versioning stance (see "Design principle" above). A
renderer that ignores them falls back to today's behavior exactly: it shows
the raw wire key as the caption, no helper/placeholder text, and every field
editable and visible.

## Layout & grouping — sections, tabs, spans

An action may declare visual structure over its flat field list: a
`static constexpr` `formLayout` groups fields into titled sections, tabs, or
an accordion panel, and a parallel `static constexpr` `fieldSpans` widens
individual fields in a grid renderer. Both mirror the `optionalFields`
convention above — a `static constexpr` list `mergeSchemaExtras` looks for by
name, present only when an action opts in. Absent either, `schemaJson<A>()`'s
output is unchanged: no `x-layout`, `x-group`, `x-section`, or `x-colspan` key
is emitted, and a renderer lays every field out exactly as it always has
(flat, `x-order` order).

```cpp
// morph::forms::FieldGroup / FieldSpan / GroupKind — forms/layout.hpp.
enum class GroupKind { Section, Tab, Accordion };

struct FieldGroup {
    std::string_view title;                    // section / tab / panel heading
    GroupKind kind{GroupKind::Section};
    std::span<const std::string_view> fields;  // member wire keys (membership;
                                               // intra-group order is x-order)
};

struct FieldSpan {
    std::string_view field;   // wire key
    int colspan{1};           // grid columns this field spans
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

Each group carries its own `kind`: `Section` (the default) renders as a
titled fieldset stacked vertically, consecutive `Tab` groups render as panes
of one shared tab bar, and `Accordion` renders as a collapsible panel.
`x-order` remains the sole authority on **intra-group** ordering —
`formLayout`'s array order gives the cross-group order and a group's
`fields` list gives membership only. A field named in no group falls into an
implicit trailing default group, in `x-order` order, so declaring a group for
*some* fields never hides the rest — the no-`formLayout` case is simply one
implicit group containing every field, which is exactly today's flat form.

`mergeSchemaExtras<A>` (see above) stamps this onto the DOM in a pass that
runs only when `A::formLayout` / `A::fieldSpans` exist
(`detail::HasFormLayout<A>` / `detail::HasFieldSpans<A>`, `forms/layout.hpp`):
the ordered group list becomes a single top-level `x-layout` object, and each
reflected member (via `forEachNamedMember`) gets `x-group`/`x-section` (if it
is named in a group) and `x-colspan` (if its declared span exceeds `1`). A
group naming a field the action does not have is silently ignored (schema
generation never throws); a field claimed by two groups keeps the **first**
one that names it.

The reference QML renderer (`examples/forms/gui_qml/qml/DynamicForm.qml`)
buckets its flat `fields` array into `sections` keyed on each field's
`x-section`, merges consecutive `"tab"`-kind sections into one shared tab
bar (`renderRuns`), and lays each section's fields out in a 2-column grid
honoring `x-colspan` — falling back to a single implicit flat section
(one column, no chrome) when the schema carries no `x-layout` at all.

**Tab switching destroys and rebuilds controls, so they re-seed from
`fieldValues`.** The tab bar drives its `Repeater` off
`sections[currentTab].fields`, so leaving a tab destroys that tab's field
delegates and returning to it creates new ones. A control's `text` otherwise
flows only *outward* (via `onTextChanged` into `fieldValues`) and never back,
so a returned-to tab showed empty controls while the form went on
auto-submitting the values it still held — sending data the user could not see.
Each text-bearing control therefore re-seeds itself from `fieldValues` on
`Component.onCompleted`, under the `programmaticEdit` suppression so
re-creating a control never fires the action. A `text:` binding would not work
here: prefill assigns `text` imperatively, which would break it.

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

This table is the complete reference for every key; two rows (`x-rules`,
`x-computed`/`inputs`) name concepts — cross-field rules and computed fields —
that get their own full explanation later in this document
([Cross-field rules](#cross-field-rules--the-x-rules-vocabulary),
[Computed fields](#computed-fields)). Skip ahead to those sections first if
the two rows below aren't self-explanatory on a first read.

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
  `x-optionValue` / `x-optionLabel` / `x-optionsDependsOn` are siblings of the
  `$ref` on *this* property** — `mergeSchemaExtras` patches
  `dom["properties"][name]`, which is the property node holding the `$ref`.
  This is still true for a `Quantity`/`Choice` property's own `$def` (the
  `quantity_kg_per_m3`-style def shown above never gets `x-order`/`required`/
  title — only `ExtUnits` and glaze's own `type`/bounds/`description` live
  there). It is **not** true for a *nested-aggregate* member's `$def`: see
  [Nested aggregates (recursive, cycle-guarded)](#nested-aggregates-recursive-cycle-guarded)
  below — that `$def` **does** get `required`/`x-order`/title/etc. patched
  directly into it, the same as any other object schema.

The **"Where"** column below names the node each key is written to. A renderer
resolves the `$ref` into `$defs`, then merges: per-property `x-*` keys (from the
property node) win, and `ExtUnits` (plus glaze's `type`/bounds/`description`) come
from the resolved def. The shipped `MorphForms` QML renderer's (`src/qt/forms`,
below) `DynamicForm.qml`'s `resolveProp` does exactly this dual read.

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `required` | top-level (object), and every nested-aggregate object schema (inlined property or `$defs` entry) — see [Nested aggregates (recursive, cycle-guarded)](#nested-aggregates-recursive-cycle-guarded) | array of strings | Names of members that must be engaged before submit. A member is listed unless it is a `std::optional<...>`, appears in `A::optionalFields`, or is a `computedFields` destination (see the [Required-ness rule](#required-ness-rule)). Always emitted (an explicit `[]` when nothing is required). The renderer blocks submission until every listed field has a value. |
| `x-order` | property node (sibling of `$ref`) | non-negative integer | The member's 0-based **declaration index**. Renderers lay fields out in ascending `x-order`, not in JSON key order (object key order is not preserved across DOMs). |
| `x-decimalPlaces` | property node (sibling of `$ref`) | non-negative integer | The field's *declared* precision (`Quantity<U, Dec>::declaredDecimals`, unit default unless the type overrides it). The numeric input step / rounding granularity for entry in the canonical unit. **Enforced, not merely advisory:** the request/reply dispatch path *rounds* each submitted `Quantity` to this precision before storing it — the stored value, not just its tag, is reduced (see [Advertised precision is enforced on dispatch](#advertised-precision-is-enforced-on-dispatch)). A model serving one *instance* of an action may overwrite this with a value from data — see [Per-instance constraints](#per-instance-constraints--values-that-live-in-data); `x-instanceConstraints` (below) says when it did. |
| `x-unitAlternatives` | property node (sibling of `$ref`) | array of objects | Convertible display/entry units for the field, derived from `UnitTraits<E>::relations`. **Omitted entirely** when the unit declares no convertible peers. Each element has the five subfields below. The renderer offers these as a unit selector and recomputes the entered value *exactly* on switch; the submitted payload is always in the canonical unit (the one named by `ExtUnits`). |
| ↳ `id` | alternative entry | string | Stable ascii id of the alternative unit (`UnitMeta::id`). |
| ↳ `display` | alternative entry | string | Human display text of the alternative unit (`UnitMeta::display`). |
| ↳ `decimals` | alternative entry | non-negative integer | The alternative unit's own default decimals (`UnitMeta::defaultDecimals`) — the input step to use while that unit is selected. |
| ↳ `num` | alternative entry | signed integer | Numerator of the exact **alternative→canonical** ratio. |
| ↳ `den` | alternative entry | signed integer | Denominator of that ratio. `value_in_canonical = value_in_alternative · num / den`; `num`/`den` are the `Rational` numerator/denominator of the composed relation, so the recompute is exact (no floating-point drift). |
| `x-optionsAction` | property node (sibling of `$ref`) | string | Type id of the registered action whose result rows populate this field's combo box. Executed with an empty body, unless the property also carries `x-optionsDependsOn` (below), in which case the request body is `{parentField: value, ...}` built from the named sibling fields' current values. |
| `x-optionValue` | property node (sibling of `$ref`) | string | Which result-row field carries the value submitted on the wire (default `"id"`). |
| `x-optionLabel` | property node (sibling of `$ref`) | string | Which result-row field carries the display label (default `"name"`). |
| `x-optionsDependsOn` | property node (sibling of `$ref`) | array of strings | Wire field names of sibling fields whose current values parameterise this field's options action (a cascading picklist). The renderer sends `{name: value, …}` as the options-action request body instead of an empty one, and re-fetches — clearing a now-invalid selection — whenever any listed field changes. **Omitted entirely** when the `Choice` declares no dependency. |
| `title` | property node (sibling of `$ref`) | string | The field's display label — an explicit `FieldMeta::label`, else a title-cased member name (`dryMassPct` → "Dry Mass Pct"). Standard JSON-Schema vocabulary, not an `x-*` key. **Always emitted.** See "Field metadata" above. |
| `x-placeholder` | property node (sibling of `$ref`) | string | In-control placeholder/hint shown while the field is empty, from `FieldMeta::placeholder`. Omitted when empty; never submitted. |
| `x-readonly` | property node (sibling of `$ref`) | boolean | `true` when the field should be displayed but not editable. Emitted only when `true` — including on every `computedFields` destination (see [Computed fields](#computed-fields)). Not a security control — see "Field metadata is not a security control" above. |
| `x-computed` | property node (sibling of `$ref`) | object | Marks the field as derived. Present when the action declares it as a `computed(...)` destination ([Computed fields](#computed-fields)); absent otherwise. |
| ↳ `inputs` | `x-computed` object | array of strings | Wire field names of the sibling fields the value derives from, in declaration order. Advisory to the renderer; **authoritative computation is the server's** — see [Where the value is authoritative](#where-the-value-is-authoritative). |
| `x-hidden` | property node (sibling of `$ref`) | boolean | `true` when the field should not be shown at all; the field remains part of the action payload. Emitted only when `true`. Not a security control. |
| `x-widget` | property node (sibling of `$ref`) | string | The preferred control id: `"textarea"`, `"slider"`, `"radio"`, `"combo"`, `"password"`, `"checkbox"`, … A `fieldMetadata`-shaped override (a `.field`/`.widget` entry, read structurally — see [widget_hints.md](widget_hints.md)) wins; else the field type's own `widget()` (`Multiline`, `Ranged`). **Advisory** — a renderer that lacks the named control falls back to the type-default control (text area → text field, slider → numeric input, radio → combo). Omitted when neither a wrapper type nor an override supplies one. |
| `x-min` | property node (sibling of `$ref`) | number | Slider lower bound, from `Ranged::min()`. Emitted only for a `Ranged` field. Distinct from glaze's schema `minimum` (a *validation* bound, when present) — `x-min` is the *control track* start and is never enforced. |
| `x-max` | property node (sibling of `$ref`) | number | Slider upper bound, from `Ranged::max()`. Emitted only for a `Ranged` field. |
| `x-exactMinimum` | wherever `minimum` sits (property node, or the `$def` reached through its `$ref`) | string | Exact decimal spelling of `minimum`, emitted **only** when the bound's magnitude exceeds 2^53 — i.e. when an IEEE-754 double cannot hold it. See [Exact numeric bounds](#exact-numeric-bounds--x-minimumtext--x-maximumtext). |
| `x-exactMaximum` | wherever `maximum` sits | string | Exact decimal spelling of `maximum`, under the same condition. |
| `x-step` | property node (sibling of `$ref`) | number | Slider / numeric increment, from `Ranged::step()`. Emitted only for a `Ranged` field. For a `Quantity` the entry granularity remains `x-decimalPlaces` (above); `x-step` is not emitted for `Quantity`. |
| `x-minimum` | property node (sibling of `$ref`) | object `{num,den,dp}` | Inclusive lower bound for the field's value, from a model's `InstanceConstraints` — an exact `Rational` in the same wire shape as the value it bounds, never a `double`. Emitted only for a decorated instance schema ([Per-instance constraints](#per-instance-constraints--values-that-live-in-data)); never by `schemaJson<A>()`. Distinct from `x-min` (a *slider track* start, which is never checked). |
| `x-maximum` | property node (sibling of `$ref`) | object `{num,den,dp}` | Inclusive upper bound, same source and shape as `x-minimum`. |
| `x-instanceConstraints` | top-level (object) | array of strings | Wire field names whose keys were written from *instance* data rather than derived from the compiled action type. Present only on a decorated schema. A renderer needing to know whether an `x-decimalPlaces`/`x-minimum`/`x-maximum` is instance-sourced checks membership here rather than guessing. |
| `format` | `Timestamp` property (or its `$def`) | string, value `"date-time"` | Standard JSON-Schema vocabulary (stamped by glaze, not by morph). The renderer shows a date-time input; the wire value is the ISO-8601 string `Timestamp` serialises to. No `x-*` extension is used for timestamps. |
| `ExtUnits` | `$def` of the `Quantity`'s unit type (reached via the property's `$ref`) | object | Glaze-stamped block describing the field's **canonical** unit. Two fields: `unitAscii` (the stable ascii id, e.g. `"kg_per_m3"` — sourced from `UnitMeta::id`) and `unitUnicode` (the human display text, e.g. `"kg/m³"` — from `UnitMeta::display`). This is the unit a payload value is always denominated in, and the reference point the `num`/`den` of every `x-unitAlternatives` entry converts *to*. A renderer resolves the property's `$ref` into `$defs` to read `ExtUnits.unitAscii`/`unitUnicode` (it is **not** on the property node next to the `x-*` keys) to label the field and anchor the unit selector. |
| `x-layout` | top-level (object) | object | The form's group structure: `{ "groups": [ { "title": string, "kind": "section"\|"tab"\|"accordion", "fields": [wire-key,…] }, … ] }`, in `A::formLayout` declaration order. Emitted only when the action declares `formLayout`. The renderer builds the named containers in array order and places each field in its group; fields absent from every group go in a trailing default group. |
| `x-group` | property node (sibling of `$ref`) | string | The title of the group this field belongs to. Omitted for a field in the implicit default group, or when `x-layout` is absent. |
| `x-section` | property node (sibling of `$ref`) | non-negative integer | The 0-based index of this field's group in `x-layout.groups`. Omitted under the same conditions as `x-group`. |
| `x-colspan` | property node (sibling of `$ref`) | positive integer | Number of grid columns the field should span, from `FieldSpan::colspan`. Emitted only when greater than `1` (the default, single-column width). A renderer laying fields out in a grid widens the control; a single-column renderer ignores it. |
| `x-rules` | top-level (object) | array of rule objects | Cross-field rules the renderer must satisfy before enabling submit, and should surface live as inline errors. Emitted only when the action declares `formRules`; absent otherwise. A renderer that ignores it falls back to per-field `required` only. |
| ↳ `kind` | rule / condition object | string | One of the closed vocabulary ids in the "Cross-field rules" section's table above (or a condition id: `engaged`, `notEngaged`, `equals`, `and`, `or`, `not`). An unrecognised `kind` must be treated as "cannot evaluate" — the renderer leaves the gate to the server rather than passing the rule (fail-closed). |
| ↳ `fields` | rule / condition object | array of strings | Wire field names the rule ranges over, in declaration order (operand order is significant for `greater`/`less`). Absent on `and`/`or`/`not`, which range over nested conditions (`conditions`/`condition` below) instead of fields directly. |
| ↳ `when` | `requiredWhen` / `visibleWhen` / `readonlyWhen` object | rule/condition object | The nested condition the rule keys on. Present only on these condition-bearing kinds. May itself be an `and`/`or`/`not` node (a compound condition), nested to any depth — see [Compound conditions](#compound-conditions--andof--orof--notof). |
| ↳ `value` | `equals` condition object | scalar / `{num,den}` | The literal an `equals` condition compares against; a numeric literal is the exact `Rational` `{num, den}`, never a `double`. |
| ↳ `conditions` | `and` / `or` condition object | array of condition objects | The nested conditions combined by boolean AND / OR, in declaration order; each element is itself a full condition/rule object (any `kind`, including a nested `and`/`or`/`not`) — see [Compound conditions](#compound-conditions--andof--orof--notof). |
| ↳ `condition` | `not` condition object | condition object | The single nested condition negated by boolean NOT (singular key, since `not` wraps exactly one child). |
| `x-submitMode` | top-level (object) | string | `"explicit"` opts a side-effectful (non-query) action out of the shipped renderer's default auto-submit-on-validity behavior — see [Explicit submit mode](#explicit-submit-mode--x-submitmode). Absent, or any value other than `"explicit"`, keeps the default. Not emitted by `schemaJson<A>()`; a schema author sets it by hand (or a hand-authored schema fixture/example does), the same way `x-layout`/`x-widget` overrides are authored today. |

### Explicit submit mode — `x-submitMode`

The shipped `DynamicForm.qml` renderer's default behavior is to call
`controller.submitIfValid(actionType, bodyJson)` the instant every field and
rule is satisfied — safe for a read-only query action, but unsafe for any
side-effectful (mutating) action: a mutation would fire on every keystroke
that happens to leave the form momentarily valid, with no user confirmation.

Setting the top-level `"x-submitMode": "explicit"` schema key opts a form out
of that default:

- `revalidate()` still recomputes `ready`/`previewLine` live (so `x-rules`,
  `required`, and every other live-validation affordance are unaffected) but
  never calls `submitIfValid` on its own.
- The renderer instead shows an explicit **Submit** button (`objectName:
  "submitButton"`), enabled only while `ready` — matching the existing
  `x-order`/required-asterisk convention of gating on the same readiness
  state the auto-submit label already reflected. Clicking it is the sole
  trigger; `DynamicForm.submit()` is the function it calls, itself a no-op
  unless the form is currently ready.
- The button is loaded (via a `Loader`, `active: explicitSubmitMode`) only
  when the schema opts in — a default (auto-submit) form has no such control
  anywhere in its item tree, not merely a hidden one.

Any schema describing a side-effectful action should carry this flag before
being safely rendered by the shipped renderer; a schema that omits it (every
existing schema, and any read-only query action) renders exactly as before —
zero behavior change.

### Array fields — `type: "array"`

glaze emits `{"type": "array", "items": {...}}` for a `std::vector<T>`
member, standard JSON-Schema vocabulary rather than an `x-*` extension. The
shipped `DynamicForm.qml` renderer gives it a dedicated
comma-separated-with-validation `TextField` control (`objectName: "field_" +
name`, exactly like a scalar field's control — the two are mutually
exclusive per field, so exactly one claims that name) instead of falling
through to the plain-text control, whose fallback (`JSON.stringify(text)`)
would wrap the typed text as a JSON *string*, not an array — a body the
server's schema validation always rejects.

Typed text is split on comma, each entry trimmed of surrounding whitespace,
and empty entries dropped: `"red, green, blue"` → `["red","green","blue"]`,
`"  red ,, green ,"` → `["red","green"]`. A field with today's scope —
array-of-string — is fully supported; an `items` type other than `"string"`
still renders this control and still encodes each comma-separated entry as a
JSON string (not, e.g., a JSON number), so a `std::vector<int>` field is
usable but not yet type-checked per element the way a scalar `Quantity`/
integer field is. The submitted literal for a **fully-blank** array field
follows the same blank-means-unengaged convention as every other field
(`fieldJsonLiteral` returns `null` for empty/whitespace-only text), so an
optional, untouched array field is omitted from the request body entirely
rather than submitted as `[]`. Once the field holds *any* non-whitespace
text, though — including a comma-only entry like `" , , "`, which is not
blank by that check even though every individual entry is dropped — it
encodes to a genuine empty array `[]`, not `null`; a `required` array field
is satisfied by engagement (non-blank text), not by having at least one
surviving entry.

### Boolean fields — `type: "boolean"`

glaze emits `{"type": "boolean"}` for a `bool` member, and
`{"type": ["boolean", "null"]}` for a `std::optional<bool>`. The shipped
`DynamicForm.qml` renderer gives both a `CheckBox` (`objectName: "field_" +
name`, mutually exclusive per field with the scalar and array controls, so
exactly one claims that name) rather than letting them fall through to the
plain-text control. The fallback there (`JSON.stringify(text)`) wrapped the
value as a JSON *string* — `{"flag":"true"}` — and, because a `TextField`
applies no validation of its own, accepted literally any text, so
`{"flag":"banana"}` was submitted just as readily. glaze rejects both with
`expected_true_or_false`; it does not coerce.

The control emits a bare `true` or `false`, never quoted. A `bool` member is
**required** (it has no null branch), and a checkbox always displays a definite
state, so a required boolean with no retained value is seeded `false` at
delegate creation rather than left blank — otherwise the form would show an
unchecked box while the required-field gate silently withheld submission, with
nothing on screen indicating what was missing. An *optional* boolean is left
unseeded and is omitted from the request body until the user touches it, which
is what distinguishes "not answered" from an explicit `false` for a
`std::optional<bool>` member.

### Nullable fields whose type is a `$ref` — `anyOf`

A nullable member whose underlying type is emitted as a definition rather than
inline — `std::optional<std::int64_t>`, or a `std::optional<T>` over a strong id
— produces neither a `type` key nor a top-level `$ref`:

```json
"optI64": {"anyOf": [{"$ref": "#/$defs/int64_t"}, {"type": "null"}]}
```

A renderer that resolves only a *top-level* `$ref` sees no type at all here, so
every field-kind flag is false and the value takes the plain-text path — a
quoted string the server rejects with `parse_number_failure`. `DynamicForm.qml`
therefore resolves through `anyOf` (and `oneOf`, which glaze does not currently
emit but a hand-written or evolved schema may): it takes the first branch whose
type is not `"null"`, follows a `$ref` inside it, and merges the result under
the property's own keys, so the field is typed by `T` and picks up `T`'s
constraints such as `minimum`/`maximum`. Integers on this path are emitted as
bare, exact numbers — the payload is assembled as JSON *text*, never round-tripped
through `JSON.parse`, so values beyond 2^53 (including `INT64_MAX`) survive
intact.
### Exact numeric bounds — `x-exactMinimum` / `x-exactMaximum`

`minimum` and `maximum` are standard JSON-Schema vocabulary, stamped by glaze.
They are JSON *numbers*, and a renderer reaches them by parsing the schema —
every shipped app does `JSON.parse(controller.schemasJson)`. JavaScript numbers
are IEEE-754 doubles, so any bound above 2^53 loses precision at that moment:

```
schema maximum for an int64_t field: 9223372036854775807
       after JSON.parse into a JS number: 9223372036854775808   (rounded up)
```

That breaks the client-side gate at exactly the value it is closest to failing
on. `INT64_MAX + 1` compared against a maximum rounded *up* to
`9223372036854775808` is judged **equal, not greater**, so the renderer's own
validation admits an out-of-range value. Nothing is corrupted — the payload is
assembled as JSON text and keeps the exact digits, and the server rejects it
with `parse_number_failure` — but the client claimed a value was valid that
never was.

`schemaJson<A>()` therefore also emits the bound as an exact decimal **string**,
which `JSON.parse` cannot round. A renderer that validates integer input should
prefer `x-exactMinimum`/`x-exactMaximum` when present and fall back to the numeric
`minimum`/`maximum` otherwise. The shipped `DynamicForm.qml` compares digits
directly in that case, since no JS number can hold the bound.

Two deliberate limits:

- **Emitted only above 2^53.** An ordinary bound (`int32_t`, a `Ranged` slider,
  a hand-written `maximum: 10`) loses nothing to a double, so its schema is
  byte-for-byte what it was before this key existed. Only the definitions that
  genuinely need it — `$defs/int64_t`, `$defs/uint64_t` — carry the companion.
- **The numeric bound stays.** The companion is additive: `minimum`/`maximum`
  remain exactly as glaze emitted them, so a renderer that ignores the new keys
  behaves precisely as it did before, per the versioning stance below.

Note the companion sits **wherever the bound sits**. For a `std::int64_t`
member that is the `$defs` entry the property's `$ref` points at, not the
property node — a renderer reads it from the merged node after resolving the
`$ref` (or the non-null `anyOf` branch), the same way it reads `type`.

### Versioning stance

The emitted schema is **unversioned**. There is no `$id`, `$schema` version
marker, or morph-specific version field anywhere in the output — a renderer
cannot detect at runtime which revision of this vocabulary a schema was produced
against. The vocabulary is therefore treated as a stable framework contract:
**changing the semantics of any key above (renaming it, changing its type, or
altering how a value is interpreted) is a breaking change and ships only in a
breaking framework release.** Adding a new, optional `x-*` key that older
renderers can safely ignore is not breaking.

## Shipped Qt/QML reference renderer

The schema contract above is renderer-agnostic; morph ships one reference
renderer for it, Qt/QML, as a reusable component rather than example code.

- **`src/qt/forms`** builds the QML module `MorphForms` (CMake target
  `morph_forms_module`, `qt_add_qml_module(... URI MorphForms VERSION 1.0)`):
  `DynamicForm.qml` (the `Repeater`-over-`fields` form renderer: `$ref`
  and `anyOf` resolution/dual-read, the exact rational digit arithmetic, the unit
  selector, the required-field submit gate, the options-fetch, layout/
  grouping into sections/tabs, the widget-hint controls — textarea, slider,
  radio group — the comma-separated-with-validation `"array"`-typed field
  control (see [Array fields](#array-fields--type-array)), the explicit
  submit mode (see [Explicit submit mode](#explicit-submit-mode--x-submitmode)),
  and the localisation dual-read), `DateTimePicker.qml` (manual
  ISO-8601 entry plus a calendar/time popup), `SlotRegistry.qml` (below), and
  `I18nCatalog.hpp`/`.cpp` (a `QObject`/`QML_ELEMENT` in-memory
  `TranslationProvider` realization — see
  [Localisation](#localisation--message-keys-and-the-catalog-seam) — shipped
  alongside the renderer rather than left in a demo, since it is
  model-agnostic and `DynamicForm`'s `catalog` property consumes it
  structurally, not by name). It builds whenever `-DMORPH_BUILD_FORMS_QML=ON`,
  independent of `MORPH_BUILD_EXAMPLES` — an app depends on it directly
  (`target_link_libraries(... morph_forms_moduleplugin)` plus `import
  MorphForms` in its own QML) instead of copying or forking it.
- **`include/morph/qt/forms/forms_controller_core.hpp`** ships
  `morph::qt::forms::FormsControllerCore<Model>`, a header-only, model-agnostic
  template (no `Q_OBJECT` — Qt cannot register a class *template* for QML) that
  owns or composes over the `Bridge`/`BridgeHandler<Model>`/`QtExecutor`
  wiring an app's own `QObject`/`QML_ELEMENT` controller subclass forwards to.
  Two constructors decide who owns the `Bridge`:
  - `FormsControllerCore(schemasJson)` builds and owns a private
    `ThreadPoolExecutor` + `QtExecutor` + `Bridge` over a `LocalBackend` —
    the convenient default for a demo or an app with no `Bridge` of its own.
  - `FormsControllerCore(Bridge& bridge, IExecutor* guiExec, schemasJson)`
    composes over a caller-supplied `Bridge`/executor instead of building a
    second, always-local one — the caller decides the deployment mode
    (`LocalBackend`, `SimulatedRemoteBackend`, `QtWebSocketBackend`, ...), and
    a later `bridge.switchBackend(...)` on that same `Bridge` is still
    reachable through this core's handler (the handler re-registers itself
    automatically, exactly like any other `BridgeHandler`). `bridge` and
    `guiExec` must outlive the core.

  It exposes `schemasJson()`, `submitIfValid(actionType, bodyJson, onReply,
  onError)`, and `fetchOptions(optionsAction, bodyJson, onReply, onError)` —
  both operations dispatch generically via `BridgeHandler::executeJson`, so an
  app's controller never hardcodes one action, and `fetchOptions`'s `bodyJson`
  is a true pass-through (`"{}"` for an independent `Choice`, or
  `{parentField: value, ...}` for a dependent one — see [Choice —
  server-sourced picklist](#choice--server-sourced-picklist)) rather than
  always empty; `examples/forms/gui_qml/FormsController.hpp` is the ~20-line
  reference wrapper (naming its own model type, since Qt cannot register the
  template itself), still using the owning constructor since the demo has no
  pre-existing `Bridge` to compose over.
- **`examples/forms/gui_qml`** is a *consumer* of the shipped module, not its
  home: its own `LabFormsDemo` QML module carries only `Main.qml` and the
  `FormsController` subclass naming `lab::LabModel`; `Main.qml` imports
  `MorphForms` for `DynamicForm`/`I18nCatalog` like any other consumer would.

This is packaging and factoring only: no `x-*` key changed, and a plain
single-action form renders identically to before the renderer was extracted.

## Renderer conformance kit

A renderer proves it honors the contract above by consuming a **schema
corpus** and satisfying a set of **expected-behavior assertions** — the
executable form of this document's "normative" claim.

- **C++ fixture corpus and drift guard**
  (`tests/test_forms_conformance_corpus.cpp`): five fixture action types —
  plain scalars + `required` (`CFScalarsAndRequired`), a `Quantity` with
  convertible alternatives (`CFQuantityAlternatives`), a `Choice`
  (`CFChoiceField`), a `Timestamp` (`CFTimestampField`), and two members of the
  same `Quantity` type sharing one `$def` (`CFSharedDefFields`) — each
  asserted against the **real**, generated `schemaJson<A>()` output (never
  hand-authored), so a change to `mergeSchemaExtras`/`schemaJson` that alters
  `x-order`, `required`, `x-decimalPlaces`, `x-unitAlternatives`,
  `x-optionsAction`/`x-optionValue`/`x-optionLabel`, `format`, or `ExtUnits`
  is caught here as a failing assertion (the corpus "drift guard").
- **QML functional assertions** (`src/qt/forms/tests/tst_conformance.qml`)
  hand-author schemas mirroring each C++ fixture by name and run them through
  the shipped `DynamicForm`: fields render in `x-order`; submission is blocked
  until every `required` field is engaged and enabled once they are; a
  `Quantity` payload is `{num,den,dp}` exact and a unit switch recomputes it
  exactly (no float drift); a `Choice` descriptor carries its declared
  `x-optionsAction`/`x-optionValue`/`x-optionLabel`; a `Timestamp` renders as a
  date-time control and gates on ISO-8601; two properties sharing one `$def`
  each keep their own `x-order` while resolving the same `ExtUnits`. The
  options-fetch itself — an independent `Choice` executing its options action
  with an empty body, and a dependent one (`x-optionsDependsOn`) with
  `{parentField: value, ...}` — is asserted separately, in
  `src/qt/forms/tests/test_forms_controller_core.cpp` (a Catch2 + Qt
  executable covering `FormsControllerCore<Model>` directly), since
  `DynamicForm` never calls the options action directly — its controller
  does.
- **Accessibility slice** (`src/qt/forms/tests/tst_conformance_accessibility.qml`):
  every control exposes an accessible name (the wire key — `title` from
  [Field metadata](#field-metadata--fieldmeta), when declared, is the visible
  label but the accessible-name fallback is always the wire key), a required field's
  accessible description announces it, focus order follows `x-order`, and
  every control (choice combo, radio group, date/time picker, text field,
  multiline text area, slider, unit selector, and the calendar popup, which
  gained arrow-key day navigation plus Enter/Escape for exactly this) is
  keyboard-operable.
- **Negative assertions** (`src/qt/forms/tests/tst_conformance_negative.qml`,
  with test-only doubles `BrokenOrderForm.qml`/`BrokenQuantityForm.qml`, never
  shipped in the `MorphForms` module): a renderer that ignores `x-order` fails
  exactly the field-order assertion and no others; a renderer that silently
  rounds an over-precise `Quantity` entry instead of rejecting it fails
  exactly the exact-payload assertion and no others — proving the kit's
  assertions are specific, not all-or-nothing.

**Scope note.** The corpus above covers exactly the keys this document's
renderer contract currently defines, plus the `x-widget`/`SlotRegistry` keys
below — informally, "**Tier-1**": the per-action `x-*` schema vocabulary this
document specifies. It does **not** include a wizard/app-shell fixture
(`w-*`/`app-*`): although the emitters for those "**Tier-2**" keys (the
wizard/app-shell layer built atop Tier-1, one level up the composition —
[workflows_navigation.md](workflows_navigation.md)) now exist
(`morph::flows::wizardSchemaJson`/`morph::app::appSchemaJson`, see
[workflows_navigation.md](workflows_navigation.md)), no conformance-kit
fixture exercises them yet — that coverage is deferred to future work,
exactly as this corpus already treats views (below) separately rather than
as a sixth `CF*` fixture. The `v-*` view-schema layer
(`morph::views::viewSchemaJson`, [views.md](views.md)) **is** implemented;
its own renderer-behavior coverage lives in
`src/qt/forms/tests/tst_collectionview.qml` rather than this five-fixture
corpus (a view composes existing action schemas rather than introducing new
per-field schema keys, so it does not need a sixth `CF*` fixture type here).

## Theming / component-override registry

A field's control is chosen by the renderer's built-in logic (`isChoice` → combo
or radio group, `isQuantity` → number + unit selector, `format: date-time` →
date/time picker, `x-widget: "textarea"`/`"slider"` → multiline/ranged
controls). An app that wants a different control for one field, one unit, one
`x-widget`, or one JSON type does so through a client-side registry, without
forking the renderer:

- **`x-widget` (optional property-level key).** A hint naming a control
  variant when the type alone is ambiguous — e.g. `"textarea"`, `"slider"`,
  `"radio"` (already dispatched on by the renderer's own widget-hint controls,
  see [Widget hints](#widget-hints--multiline--ranged)), or an app-defined id
  such as `"slider"`/`"rating"` a registered `SlotRegistry` slot recognises.
  It is read with the same dual-read as every other property-level key
  (`opt(raw["x-widget"], p["x-widget"])`). Absent, it resolves to `""` and
  never matches `SlotRegistry`'s `byWidget` tier — purely additive and
  ignorable.
- **`SlotRegistry` (QML type, module `MorphForms`, entirely client-side).** A
  lookup a host app populates at startup: `byField(action, field, component)`,
  `byWidget(xWidget, component)`, `byUnit(unitAscii, component)`,
  `byType(jsonType, component)`, and `resolve(action, field, xWidget,
  unitAscii, jsonType)`, which returns the highest-priority match or `null`.
  Resolution order is **field → `x-widget` → unit → type → built-in default**.
  `DynamicForm` gains a `slotRegistry` property (`null` by default — no
  behavior change for an app that never sets it); when a field resolves to a
  registered `Component`, `DynamicForm` loads it via a `Loader` and hides its
  own built-in control for that field (every built-in control — combo, radio
  group, date/time picker, text field, text area, slider, unit selector — is
  gated on the `Loader`'s `sourceComponent` being `null`). A registered slot
  `Component` implements one small contract: it declares `property var field`
  and `property var setValue`, both assigned by `DynamicForm`'s
  `Loader.onLoaded` — `field` is the resolved, merged def+property descriptor,
  and `setValue(text)` is the same set-value path (`setFieldValue`) the
  built-in controls use, so an override participates in the required-gate and
  auto-fire without special-casing. `SlotRegistry.revision` is bumped on every
  `by*()` call and read inside `resolve()`, for the same reason
  `I18nCatalog.revision` exists: `_byField`/`_byWidget`/`_byUnit`/`_byType` are
  plain objects mutated in place, which does not by itself notify a binding
  that already read them.

The registry never appears in the schema or on the wire — two renderers of the
same schema may register different slots. This is the "escape hatch always
available" design principle ([above](#design-principle-infer-by-default-declare-to-override))
in practice: swap one control without forking the renderer.

## Localisation — message keys and the catalog seam

The schema stays one cached, un-localised instance per type (see
[One cached schema per type — no localisation](#one-cached-schema-per-type--no-localisation)):
translation is a renderer-side catalog lookup over **stable, mechanically
derived message keys**, never a per-locale schema variant. Two small
header-only libraries carry this:

- **`morph::forms::i18n`** (`include/morph/forms/i18n.hpp`) — the key
  derivation vocabulary.
- **`morph::render`** (`include/morph/render/i18n.hpp`,
  `include/morph/render/locale_format.hpp`) — the renderer-side catalog seam
  and the locale numeric-entry contract. `morph::render` is client-side only
  and never appears on the wire.

### Message-key derivation

A key is derived from identifiers the schema (or the `actionType` label a
renderer already has) already carries — no declaration needed in the common
case:

| Text slot | Derived key | Function |
|---|---|---|
| field label / help / placeholder | `<actionTypeId>.<wireField>.label` / `.help` / `.placeholder` | `morph::forms::i18n::fieldKey(actionTypeId, wireField, FieldSlot)` |
| layout group title | `<actionTypeId>.group.<index>` | `groupKey(actionTypeId, groupIndex)` |
| cross-field rule message | `<actionTypeId>.rule.<index>` | `ruleKey(actionTypeId, ruleIndex)` |
| wizard title / step title | `<wizardId>.title` / `<wizardId>.step.<index>.title` | `wizardTitleKey(wizardId)` / `wizardStepTitleKey(wizardId, stepIndex)` |
| app title / menu label | `<appId>.title` / `<appId>.menu.<index>.label` | `appTitleKey(appId)` / `appMenuLabelKey(appId, menuIndex)` |

`actionTypeId` is `ActionTraits<A>::typeId()`; `wireField` is the member's
reflected wire key (the same name `mergeSchemaExtras` iterates via
`forEachNamedMember`); group/rule/step/menu indexes are the 0-based position
in their respective schema arrays. None of these keys are written into the
schema — a renderer derives them itself from data it already has (the schema
plus the `actionType` label it is rendering under), so declaring nothing
changes zero bytes of any schema.

**Declare to override.** A field declares an explicit key stem via
`FieldMeta::i18nKey` (see [Field metadata](#field-metadata--fieldmeta)),
emitted as `x-i18nKey` on its schema node; group, rule, wizard step, and menu
descriptors gain the same optional `i18nKey` member on their own types, owned
by each descriptor's own spec. For a **field**, the override replaces only
the `<actionTypeId>.<wireField>` stem; each of the three per-field suffixes
(`.label` / `.help` / `.placeholder`) still applies on top of it —
`morph::forms::i18n::explicitFieldKey(i18nKeyOverride, slot)` computes
`"<i18nKeyOverride>.<slot>"`. For a group, rule, wizard step, or menu entry —
each of which carries exactly one piece of text — the override *is* the
complete key, used in place of the derived one.

### The catalog seam

```cpp
// namespace morph::render — client-side only; never on the wire.
using TranslationProvider =
    std::function<std::optional<std::string>(std::string_view key, std::string_view bcp47Locale)>;
```

`morph::render::resolveText(provider, bcp47Locale, explicitKey, derivedKey,
schemaLiteral)` resolves one display slot's text, most specific first: the
explicit key (when declared) is tried first, then the derived key, and a
miss at both falls back to `schemaLiteral` — the schema's authored `title` /
`description` / `x-placeholder` / group or step title, unchanged. A
default-constructed (empty) `provider` — no catalog installed — skips
straight to `schemaLiteral`, so an unconfigured renderer behaves exactly as
it did before this spec. morph ships the seam and this resolution algorithm
only; it defines no translation storage format — a host adapts whatever
catalog it already owns (Qt `QTranslator`/`.qm`, a JSON bundle, a database)
into the one `TranslationProvider` signature.

The `examples/forms/gui_qml` reference renderer hosts a concrete, minimal
realization: `I18nCatalog` (`examples/forms/gui_qml/I18nCatalog.hpp`), an
in-memory `QObject` catalog (QML cannot hold a `std::function` directly),
wired into `DynamicForm.qml`'s `resolveText`/`i18nFieldKey` JS mirrors of the
functions above. It currently resolves only the field label/help/placeholder
slot — group-title i18n wiring for the already-implemented
[Layout & grouping](#layout--grouping--sections-tabs-spans) feature remains
future work. The wizard/app-shell layer
([workflows_navigation.md](workflows_navigation.md)) is implemented, but its
QML renderer (`WizardView.qml`/`AppShell.qml`) does not yet accept an
`I18nCatalog` either, matching `CollectionView.qml`'s own gap (see
[views.md](views.md), "Limitations") — wizard/app-menu i18n wiring remains
future work. Cross-field rules (above) are implemented
but carry no translatable message text of their own — the `x-rules`
vocabulary is structural (`kind`/`fields`/`when`/`value`) only, so a renderer
builds any rule-violation message from that structure (or its own catalog
entry, per "Rule messages come from the catalog, not the wire" below), never
from a wire string.

**Group membership is matched by index, never by translated text.** A
field's `x-section` is the stable numeric handle into `x-layout.groups`; a
renderer translates a group's *displayed* title but places fields by index.

**Rule messages come from the catalog, not the wire.** For a rule the client
can evaluate, the renderer shows its catalog message (falling back to a
renderer-built neutral message from the rule's structure); canonical
server-side error strings ([error_handling.md](../error_handling.md)) stay
untranslated protocol vocabulary, surfaced only for conditions the client
could not pre-empt.

### Locale data formatting

Display formatting is the renderer's duty; the wire stays canonical:

- **Numbers.** `morph::render::normalizeLocaleNumber(text, decimalSeparator,
  groupSeparator)` (`include/morph/render/locale_format.hpp`) converts a
  locale-formatted entry (`"1.050,25"`) to the canonical `.`-decimal text
  `Quantity`'s exact digit routines already consume (`"1050.25"`); malformed
  input yields `std::nullopt` rather than a best-effort guess.
  `formatCanonicalNumber` is the display-direction inverse, with
  display-only thousands grouping. The exact `Rational`/`Quantity` digit
  arithmetic ([rational.md](../util/rational.md)) never sees a
  locale-formatted string — the conversion happens at the control edge only.

  Both separators are `std::string_view`, not `char`, because a real locale's
  separator is not always one byte: fr-FR groups with U+202F (narrow no-break
  space, 3 bytes in UTF-8) and several locales use U+00A0 (2 bytes). Typed as
  `char`, neither could be expressed at all — a caller could only pass some
  single byte that never matched, so a perfectly valid `"1 050,25"` typed by a
  French user normalised to `std::nullopt` and the control reported it
  malformed. An empty view means "this locale has no such separator".
- **Timestamps.** The wire value is strict UTC ISO-8601
  ([datetime.md](../util/datetime.md)); a renderer displays and edits in the
  user's zone by shifting a `morph::time::DateTime` with its existing
  duration-arithmetic operators (`dt + std::chrono::minutes{offset}` for
  display, `dt - std::chrono::minutes{offset}` back to canonical UTC before
  submission) — no new arithmetic is needed, only the offset the renderer
  chooses to display in. A locale-formatted entry must round-trip to the
  identical canonical wire value.
- **Choice option labels are data, not chrome.** Option rows come from
  executing the options action ([choice.md](choice.md)); the catalog never
  sees them. A model that wants localised rows reads
  `session::current()->locale` server-side
  ([session.md](../session/session.md)) — the one place server-side locale
  participates.

### Non-goals

- No per-locale schema variants — `schemaJson<A>()` keeps its one cached,
  un-localised schema.
- No translation storage format — the `TranslationProvider` signature is the
  whole contract.
- No server-side message localisation — canonical error strings stay
  diagnostic/protocol vocabulary.
- No RTL / layout mirroring engine.
- Not machine translation, locale negotiation, or plural rules — the catalog
  is a lookup; anything richer lives inside the host's provider
  implementation.

## `allRequiredEngaged<A>()` — readiness check

```cpp
template <typename A>
[[nodiscard]] constexpr bool allRequiredEngaged(const A& action) noexcept;
```

Returns `true` when every **required** empty-capable member of `action` has
`hasValue() == true`. Required has the same meaning as in the
[Required-ness rule](#required-ness-rule): not `std::optional<...>`, not
listed in `A::optionalFields`, and not a `computedFields` destination.
Non-empty-capable members (plain ints, strings, etc.) are skipped — they
cannot express "not filled in". Intended as the body of the action's
`validate()` (the `ActionValidator` machinery picks it up automatically).

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
**own top-level members**; unlike `schemaJson<A>()`'s schema generation (see
[Nested aggregates (recursive, cycle-guarded)](#nested-aggregates-recursive-cycle-guarded)),
it does **not** recurse into a nested aggregate member's own fields.

## Cross-field rules — the `x-rules` vocabulary

`allRequiredEngaged` is per-field and membership-blind by design. A condition
spanning **two or more fields** — "end date must be after start date", "supply
either an email or a phone but not both", "discount is required only when a
promo code is entered" — is expressed with a **closed, typed rule vocabulary**
declared once as an action's `static constexpr formRules` member, built with
`morph::forms::ruleList(...)`:

```cpp
struct BookRoom {
    morph::time::Timestamp checkIn;
    morph::time::Timestamp checkOut;
    std::optional<std::string> email;
    std::optional<std::string> phone;
    Quantity<Unit::money> promo;
    Quantity<Unit::money> discount;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::greater(&BookRoom::checkOut, &BookRoom::checkIn),
        morph::forms::exactlyOneOf(&BookRoom::email, &BookRoom::phone),
        morph::forms::requiredWhen(&BookRoom::discount, morph::forms::engaged(&BookRoom::promo)),
        morph::forms::visibleWhen(&BookRoom::discount, morph::forms::engaged(&BookRoom::promo)));

    [[nodiscard]] bool validate() const {
        return morph::forms::allRulesSatisfied(*this) && morph::forms::allRequiredEngaged(*this);
    }
};
```

One declaration drives three consumers: `schemaJson<A>()` emits it as a
top-level `x-rules` array (alongside `required`); `allRulesSatisfied<A>(action)`
evaluates it as the shared C++ predicate; and because `validate()` calls
`allRulesSatisfied`, `ActionValidator<A>::ready` ([registry.md](../core/registry.md))
picks it up automatically on every dispatch path that already enforces
`ready()` — the reactive `set<>` gate, the client request/reply gate, and the
server dispatch runner ([registry.md](../core/registry.md)) — with no extra
code anywhere. The vocabulary is deliberately closed: adding a new rule kind is
a framework change, never an application-supplied lambda, which is what lets
the client and the server evaluate identically from the same serialized form.

### The rule and condition kinds

| Factory | Meaning | `x-rules` `kind` | Also valid as a condition? |
|---|---|---|---|
| `requiredWhen(field, cond)` | `field` must be engaged when `cond` holds. | `"requiredWhen"` | no (only ranges over conditions itself) |
| `greater(a, b)` / `greaterOrEqual(a, b)` | `*a > *b` / `*a >= *b`. | `"greater"` / `"greaterOrEqual"` | yes |
| `less(a, b)` / `lessOrEqual(a, b)` | `*a < *b` / `*a <= *b`. | `"less"` / `"lessOrEqual"` | yes |
| `exactlyOneOf(f1, f2, ...)` | Exactly one listed field is engaged. | `"exactlyOneOf"` | no |
| `atLeastOneOf(f1, f2, ...)` | At least one listed field is engaged. | `"atLeastOneOf"` | no |
| `mutuallyExclusive(f1, f2, ...)` | At most one listed field is engaged. | `"mutuallyExclusive"` | no |
| `visibleWhen(field, cond)` | **Presentation:** `field` is shown only while `cond` holds. | `"visibleWhen"` | no |
| `readonlyWhen(field, cond)` | **Presentation:** `field` is editable only while `cond` does **not** hold. | `"readonlyWhen"` | no |
| `engaged(field)` / `notEngaged(field)` | `field` is / is not engaged. | `"engaged"` / `"notEngaged"` | yes (condition-only) |
| `equals(field, literal)` | `field`'s engaged value equals `literal`. | `"equals"` | yes (condition-only) |
| `andOf(cond1, cond2, ...)` | Every listed condition holds (boolean AND). | `"and"` | yes — also usable directly as a top-level rule |
| `orOf(cond1, cond2, ...)` | At least one listed condition holds (boolean OR). | `"or"` | yes — also usable directly as a top-level rule |
| `notOf(cond)` | The nested condition does **not** hold (boolean NOT). | `"not"` | yes — also usable directly as a top-level rule |

`engaged`/`notEngaged`/`requiredWhen`/the membership rules accept any
`EngageableField` — an `EmptyCapableField` (`Quantity`/`Choice`/`Timestamp`) or
a plain `std::optional<T>` (which does **not** satisfy `EmptyCapableField` —
see "two exclusions" above — but does count as engageable for rule purposes).
`greater`/`greaterOrEqual`/`less`/`lessOrEqual` are narrower: both operands
must be the **same** `EmptyCapableField` type whose engaged value
(`operator*()`) is three-way-comparable — `Quantity<U, Dec>` (compares the
exact `math::Rational` payload, never a `double`) or `morph::time::Timestamp`
(compares `DateTime`). An unengaged operand makes a comparison **vacuously
satisfied** (`true`) — both as a top-level rule and when reused as a nested
condition — so a form still being filled in never fails a comparison
prematurely; the required-ness of the operand itself is a separate
`required`/`requiredWhen` concern. `equals`, by contrast, is **not** vacuous:
an unengaged field cannot equal anything, so it returns `false` until the
field is engaged. A literal passed to `equals` is one of `std::int64_t`,
`bool`, `std::string`, the exact `math::Rational` (never a `double`), or a
captured string literal, so it serialises losslessly into `x-rules`.

A bare string literal — `equals(&A::code, "URGENT")` — is captured **inline**
as a `detail::LiteralString` (an alias for the project's shared
`morph::detail::FixedString`), not copied into a `std::string`. That is what
keeps the documented
`static constexpr auto formRules = ruleList(...)` declaration working for a
literal of any length: a rule node has to be a literal type, and a `std::string`
holding more characters than the standard library's small-string buffer (15 on
libstdc++) allocates, so the declaration fails with "refers to a result of
`operator new`". The limit was invisible in the source — the same code compiled
or did not depending only on how long the literal was, and on which standard
library was in use. Serialisation is unaffected: `emitNode()` emits the same
JSON string either way. Passing an explicit `std::string` still stores a
`std::string` and still cannot be `constexpr` when it allocates; that is
inherent to the type the caller chose.

### Compound conditions — `andOf` / `orOf` / `notOf`

The single-node conditions above (`engaged`, `notEngaged`, `equals`, and the
comparison kinds reused as booleans) compose into a **recursive condition
tree** via three more factories:

```cpp
struct BookRoom {
    // ...
    static constexpr auto formRules = morph::forms::ruleList(
        // discount required only when BOTH promo and a loyalty code are engaged
        morph::forms::requiredWhen(
            &BookRoom::discount,
            morph::forms::andOf(morph::forms::engaged(&BookRoom::promo),
                                morph::forms::engaged(&BookRoom::loyaltyCode))));
};
```

- **`andOf(cond1, cond2, ...)`** — holds when every listed condition holds
  (at least two conditions; `test()` short-circuits left to right).
- **`orOf(cond1, cond2, ...)`** — holds when at least one listed condition
  holds (at least two conditions; `test()` short-circuits left to right).
- **`notOf(cond)`** — holds when the single nested condition does **not**
  hold.

Each factory accepts **any** condition or rule node as a child — a leaf
(`engaged`, `equals`, `greater`, …) or another `andOf`/`orOf`/`notOf` — so a
tree nests to any depth: `orOf(notOf(engaged(&A::x)), andOf(engaged(&A::y),
engaged(&A::z)))` is a valid `when` clause. All three nodes share this
uniform shape with every existing rule/condition node (`kind`, `test(const
A&) const noexcept`, `emitNode()`), which is what makes them substitutable
everywhere an existing single-node condition already worked:

- **Nested inside a `when` clause** — `requiredWhen`/`visibleWhen`/`readonlyWhen`
  accept a compound condition in the same `when` position a leaf condition
  occupies, with no change to those three rule kinds themselves.
- **Directly as a top-level `formRules` entry** — `andOf`/`orOf`/`notOf`
  declare `isPresentation = false` and a `test()`, so `ruleList(andOf(...))`
  is itself a valid, directly-gating rule — "a single rule with a compound
  condition tree", not only a condition factored inside another rule.

`andOf`/`orOf`/`notOf` add no new closed-vocabulary *rule* kinds — they are
closed-vocabulary *conditions*, matching the existing "closed, typed" design
of every other node in this table: an application still cannot supply an
arbitrary lambda, only compose the existing typed primitives into a tree.

#### Schema emission — nested `conditions` / `condition`

`andOf`/`orOf` emit a `"conditions"` array of nested condition nodes;
`notOf` emits a single nested `"condition"` object (singular, since it wraps
exactly one child):

```json
{ "kind": "requiredWhen", "fields": ["discount"],
  "when": { "kind": "and", "conditions": [
    { "kind": "engaged", "fields": ["promo"] },
    { "kind": "engaged", "fields": ["loyaltyCode"] }
  ]}
}
```

```json
{ "kind": "or", "conditions": [
  { "kind": "not", "condition": { "kind": "engaged", "fields": ["promo"] } },
  { "kind": "and", "conditions": [
    { "kind": "engaged", "fields": ["email"] },
    { "kind": "engaged", "fields": ["phone"] }
  ]}
]}
```

A renderer that does not recognise `"and"`/`"or"`/`"not"` treats them as an
unrecognised `kind` per the existing fail-closed rule (see "Renderer
fallback" below) — it defers enforcement to the server rather than guessing
at the nested structure, exactly like any other unrecognised `kind`.

### Presentation rules never gate

`visibleWhen`/`readonlyWhen` are the only two **presentation** kinds: they
never participate in `allRulesSatisfied` (skipped by construction, via each
node's `isPresentation` flag), only in what a renderer shows/enables. While a
field is hidden by `visibleWhen`, its current draft value still travels in the
payload — hiding never clears it, exactly like a static `x-hidden` field. An
author who wants "hidden ⇒ also not required" pairs `visibleWhen(f, c)` with
`requiredWhen(f, c)` explicitly; neither implies the other.

### The `x-rules` schema emission

`mergeSchemaExtras` walks `A::formRules` (when declared) and emits a
**top-level** `x-rules` array, alongside `required`. Each element is
self-describing JSON a renderer (or the server) can evaluate without any C++
type information:

```json
"x-rules": [
  { "kind": "greater", "fields": ["checkOut", "checkIn"] },
  { "kind": "exactlyOneOf", "fields": ["email", "phone"] },
  { "kind": "requiredWhen", "fields": ["discount"],
    "when": { "kind": "engaged", "fields": ["promo"] } },
  { "kind": "visibleWhen", "fields": ["discount"],
    "when": { "kind": "engaged", "fields": ["promo"] } }
]
```

Field names are the **wire (JSON) field names**, resolved from the
pointer-to-member the same way `x-order` is derived: a fresh probe instance of
the action is walked and each rule's stored member pointer is matched against
the probe's members by address. An action with no `formRules` emits no
`x-rules` key at all — byte-identical to a version of the schema generated
before this feature existed.

### Unsatisfiable declarations — `required` contradicting `x-rules`

`required` is derived from field required-ness ([Required-ness rule](#required-ness-rule));
`x-rules` is derived from `A::formRules`. Nothing links the two derivations, so
an action can declare both halves sensibly on their own and still describe a
form **no submission can satisfy**:

```cpp
struct CaptureConcentration {
    Concentration value;        // EmptyCapableField -> required by default
    QualifierChoice qualifier;  // EmptyCapableField -> required by default

    // `required` demands both. `exactlyOneOf` permits exactly one.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::exactlyOneOf(&CaptureConcentration::value,
                                   &CaptureConcentration::qualifier));
};
```

A renderer honouring `required` demands both fields; a payload meeting that
demand then fails `exactlyOneOf` on the server. The form is dead on arrival,
and both halves of the served schema look entirely reasonable in isolation.

`schemaJson<A>()` **rejects this at generation** by throwing
`morph::forms::UnsatisfiableFormError`. The check lives in
`detail::rejectUnsatisfiableRules`, called from `mergeSchemaExtras` — the one
place both halves are in hand — and reads the *emitted* rule nodes against the
*emitted* `required` array, so it matches on the same wire names a renderer
would.

**What counts as a contradiction.** A rule kind that **caps** how many of the
fields it ranges over may be engaged at once, ranging over **two or more**
fields that are also in `required`:

| Rule kind | Caps engagement? | Why |
|---|---|---|
| `exactlyOneOf` | yes — ceiling of one | Two required fields cannot both be engaged and still be "exactly one". |
| `mutuallyExclusive` | yes — ceiling of one | Same ceiling; "at most one" and "both required" cannot hold together. |
| `atLeastOneOf` | **no** — it is a *floor* | Satisfied by engaging every field it names, so it can never contradict `required`. Rejecting it would be a false positive. |
| `requiredWhen` | **no** | Only ever *adds* required-ness; it cannot cap anything. |
| everything else | no | Comparison, presentation, and compound kinds impose no engagement ceiling. |

`detail::capsEngagedCount(kind)` is the single place the capping kinds are
named. A future rule kind carrying a ceiling ("at most two of these") joins
that list and is covered with no other change.

**Boundaries that deliberately do *not* throw:**

- **Exactly one required field inside a capping rule.** Satisfiable: engage
  that field, leave the rest empty. Only two or more conflict.
- **`std::optional` members.** `detail::isStdOptional` keeps them out of
  `required` on sight, so a rule over `std::optional` fields can never reach
  the contradiction — with or without this check. The reachable case is an
  `EmptyCapableField` (a `Quantity`, a `Choice`, a strong id): required by
  default, and rangeable by a membership rule.
- **Required fields the rule does not name.** Only the intersection of the
  rule's `fields` and `required` is counted.

**How an author fixes it.** Name the rule's fields in `A::optionalFields`. The
rule then becomes the *only* gate on them, which is what the multi-field
sum-type encoding ([Sum types not in the forms palette](#sum-types-not-in-the-forms-palette--multi-field-encoding-by-design))
actually means. Alternatively, drop the rule.

**Why this one exception to permissive generation.** Everywhere else, schema
generation tolerates an author's declaration mistake silently: a `formLayout`
entry naming a field the action does not have is ignored, a field claimed by
two groups keeps the first. That is right, because a tolerated mistake still
yields a **working form** — the author loses a layout hint, not the form. This
case is different in kind: the result is a form **nobody can submit**, on any
client, with no error naming the reason. The failure is already certain at
generation time and belongs to the author's own build, so it is raised there
rather than left to surface as a user who cannot press Save. A `static_assert`
would be better still, but `detail::resolveFieldName` is not `constexpr` (it
matches member addresses against a runtime probe instance), so a
generation-time throw is the achievable form today.

**Interaction with the schema cache.** `schemaJson<A>()` memoises into a
function-local `static const std::string`. A throw during that static's
initialisation leaves it uninitialised, so a later call re-runs the check and
throws again, rather than serving a half-built or empty schema.

### Server-side: the same list, evaluated in the dispatcher

The server never trusts the client's evaluation of `x-rules`; it re-runs
`A::formRules` itself. Because an action's `validate()` calls
`allRulesSatisfied(*this)`, and `ActionValidator<A>::ready` auto-detects
`validate()` via `HasValidate` ([registry.md](../core/registry.md)), the
server dispatch runner evaluates the **exact same rule list** the client did —
the same typed nodes over the same values — with zero extra server code. A
hand-built envelope that violates a rule is rejected with
`morph::model::ValidationError` ([registry.md](../core/registry.md)) on every
dispatch path (local, simulated-remote, Qt WebSocket), before `Model::execute`
runs.

### Renderer fallback

Every key here is additive and optional, consistent with the unversioned
schema stance below. An action declaring no `formRules` emits no `x-rules` and
behaves exactly as before this feature existed. A renderer that does not
understand `x-rules` still produces a usable form: it honours the per-field
`required` array and lets the **server** reject any cross-field violation —
the correctness floor never depends on the client understanding the key. An
unrecognised `kind` (a rule *or* a nested condition) must be treated as
"cannot evaluate" by a client renderer, which defers enforcement to the
server rather than passing the rule — the server, running the compiled C++
rule list directly, has no such "unrecognised kind" case.

## Computed fields

Some fields are not entered by the user at all — they are a **pure function
of other fields on the same action**: `total = qty * price`, `vatDue = net *
rate`. An action declares one with a `static constexpr` map from a
destination member to its declared input members and a pure derivation,
next to `optionalFields`/`formRules`:

```cpp
struct LineItem {
    Quantity<Units, 2> qty;
    Quantity<Units, 2> price;
    Quantity<Units, 2> total;  // computed -- not user-entered

    // A generic (auto) lambda parameter, not `const LineItem&`: this
    // initializer runs while LineItem is still an incomplete type (a static
    // data member initializer is not a complete-class context the way a
    // member function body or a non-static default member initializer is
    // -- see "Incomplete-type self-reference" above), so the body's member
    // access must stay dependent until first use, after the class is complete.
    static constexpr auto computedFields = morph::forms::computeList(
        morph::forms::computed<&LineItem::total, &LineItem::qty, &LineItem::price>(
            [](const auto& s) { return s.qty * s.price; }));

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};
```

- **`computed<Dst, Inputs...>(fn)`** binds a destination member, its ordered
  input members, and a pure derivation `fn(const A&) -> ValueOfDst`. `Dst` and
  `Inputs...` are pointer-to-data-member NTTPs (trailing template arguments,
  not a braced-list runtime parameter), so a renamed or deleted field is a
  compile error and the input list is type-checked.
- **`computeList(...)`** composes one or more `computed(...)` declarations
  into a `detail::ComputeList<...>` value assigned to `static constexpr auto
  computedFields`. The framework detects it via the `detail::HasComputedFields<A>`
  concept, mirroring `detail::HasOptionalFields<A>`/`HasFormRules<A>`.
- **`recomputeAll<A>(action)`** is the single evaluator: it walks
  `A::computedFields` and, for each entry, overwrites the destination member
  with `fn(action)` — or, if any declared input is unengaged (`hasValue() ==
  false`, for an input satisfying `EmptyCapableField`; a non-empty-capable
  input is always considered engaged), resets the destination to its
  default-constructed (empty) value instead of computing from a missing
  operand. For a `Quantity` destination the result is converted to the
  destination's own type and **rounded** to its declared precision
  (`Quantity::atDeclaredPrecision()`), so the stored value matches
  `x-decimalPlaces` regardless of what declared precision `fn`'s return type
  happened to carry, and regardless of how many decimals the derivation itself
  produced — a product of two 2-decimal operands is exact to 4.
- `fn` must be **pure** — a function of the action's own fields only, no side
  effects, no external state. The framework cannot check this; it is the
  author's contract. Anything impure (model state, a database lookup, the
  current time) belongs in the model's `execute`, not a computed field.

### Schema emission

`mergeSchemaExtras` patches each computed destination's property node with
`x-readonly: true` and `x-computed: { "inputs": [...] }` (wire field names, in
declaration order, resolved from the pointer-to-member the same way
`x-order` is derived), and **excludes it from the synthesised `required`
array** (see [Required-ness rule](#required-ness-rule)) — a computed field is
never something the user must fill. `x-computed`/`x-readonly` are additive,
optional `x-*` keys (see the [renderer contract](#renderer-contract-the-schema-key-vocabulary)
table below); an action that declares no `computedFields` emits neither key.

### Where the value is authoritative

`recomputeAll` runs at three call sites, all authoritative:

1. `ActionExecuteRegistry::registerAction`'s executor (the client-bridge JSON
   dispatch path behind `BridgeHandler::executeJson`, [bridge.md](../core/bridge.md)).
2. `Bridge::executeVia`'s `localOp` (the in-process execution path `LocalBackend`
   uses for every `execute<Action>()`/`executeJson` call, [bridge.md](../core/bridge.md)).
3. `ActionDispatcher::registerAction`'s runner (the server-side execution path
   `RemoteServer` uses for `SimulatedRemoteBackend` and the Qt WebSocket
   transport, [registry.md](../core/registry.md)).

Sites 2–4 run **after** decode and **before** `Model::execute`, so a computed
value arriving on the wire is always discarded and replaced with the
authoritative recomputation — a hostile or buggy client cannot influence the
stored value by tampering with a computed field. On every site that also
decodes JSON (2 and 4; `localOp` never does — it dispatches an already-typed
`Action`), `recomputeAll` runs immediately after `reconcileDeclaredPrecision`
and **before** the `ActionValidator::ready` check, so a validator that
inspects a computed field sees the authoritative, server-derived value rather
than whatever arrived on the wire. Because every site calls the identical
`recomputeAll` over inputs reconciled to declared precision
(`reconcileDeclaredPrecision`, [above](#advertised-precision-is-enforced-on-dispatch)),
the client's displayed value and the server's stored value are identical to
the last digit. It is a **no-op** for actions with no `computedFields` — zero
behaviour change, backward compatible — mirroring how `reconcileDeclaredPrecision`
no-ops for actions with no `Quantity` members.

A cross-field rule ([Cross-field rules](#cross-field-rules--the-x-rules-vocabulary))
that references a computed field evaluates on the server's authoritative
recomputed value, not the client's, since `recomputeAll` runs before the
validator check on every dispatch path.

## Per-instance constraints — values that live in data

Everything above derives a schema from the compiled action type. When a form
*definition* is itself data — a versioned analysis catalogue, a per-tenant
configuration — the values of some framework-meaningful keys belong to a
database row rather than to a template parameter, and no amount of reflection
over `A` can reach them.

`morph::forms::InstanceConstraints` (`forms/instance_constraints.hpp`) is the
seam for exactly that, and no more than that: **an instance varies the values
of existing keys; it never varies the form's shape.** One declaration both
decorates the served schema (`x-decimalPlaces`, `x-minimum`, `x-maximum`, plus
the document-level `x-instanceConstraints` stamp) and checks a submitted value
against the same numbers, so the two cannot drift apart — which is what an
application patching a private key beside the framework's could never promise.

The framework reports violations and the model applies policy; the dispatch
runners do **not** apply instance constraints, because they have no instance to
read one from. See [instance_constraints.md](instance_constraints.md) for the
API, the emitted keys, and the reasoning behind both of those decisions.

## Support traits and helpers

| Symbol | Kind | Purpose |
|---|---|---|
| `detail::IsStdOptional<T>` | trait | `true` when `T` is a `std::optional<...>`. |
| `detail::isStdOptional<T>` | variable template | cvref-stripped alias of the trait. |
| `detail::HasOptionalFields<A>` | concept | `true` when `A` has a `static constexpr` iterable `optionalFields`. |
| `detail::declaredOptional<A>(name)` | constexpr function | `true` when `name` appears in `A::optionalFields`. |
| `detail::forEachNamedMember(action, visitor)` | function template | Calls `visitor.operator()<I>(name, member)` for every reflected member of `action` (uses glaze pure reflection). |
| `detail::mergeSchemaExtras<A>(raw)` | function | Post-processes a glaze-generated schema to inject `required`, `x-decimalPlaces`, `x-order`, `x-unitAlternatives`, `x-optionsAction`, `title`, `description`/`x-placeholder`/`x-readonly`/`x-hidden` etc. onto the property nodes. Called by `schemaJson<A>()`. |
| `reconcileDeclaredPrecision<A>(action)` | function | **Rounds** every `Quantity` member of `action` in place to its declared precision (`atDeclaredPrecision()`, an exact `Rational` re-rounding — not a retag), so a decoded wire value *equals* the schema's advertised `x-decimalPlaces`, not merely displays at it. Empty members stay empty. No-op for non-`Quantity` members and for action types glaze cannot reflect. Called on both wire dispatch paths (`bridge.hpp`, `registry.hpp`); not on the in-process `localOp` path, which decodes no JSON. |
| `FieldMeta` | struct | Per-field presentation descriptor: `field`, `label`, `help`, `placeholder`, `widget` (control-selection override, see [Widget hints](#widget-hints--multiline--ranged)), `readOnly`, `hidden`, plus `withPlaceholder`/`withReadOnly`/`withHidden` fluent copies. See "Field metadata" above. |
| `detail::HasFieldMetadata<A>` | concept | `true` when `A` has a `static constexpr`/`static const` iterable `fieldMetadata`. |
| `detail::findFieldMeta<A>(name)` | function | Returns the `FieldMeta` entry naming `name`, or `nullptr`. |
| `detail::inferTitle(name)` | function | Title-cases a wire key on camelCase/underscore boundaries. |
| `describe<MemberPtr>(label, help)` | function template | Builds a `FieldMeta` whose `field` is resolved from the pointer-to-member `MemberPtr` at runtime. Not `constexpr` — see "Field metadata" above for why, and for the out-of-line declaration a `describe<>()`-based `fieldMetadata` array needs. |

## API reference

### `schemaJson<A>()`

| Signature | Returns |
|---|---|
| `template <typename A> std::string schemaJson()` | The merged schema JSON. Cached per type. On internal failure returns the raw glaze schema, or an empty string if glaze's own schema generation failed — it never throws over malformed *input*. Throws `UnsatisfiableFormError` for a self-contradicting *declaration* ([Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules)). |

### `allRequiredEngaged<A>()`

| Signature | Returns |
|---|---|
| `template <typename A> bool allRequiredEngaged(A const&)` | `true` when every required empty-capable field is engaged. |

### `EmptyCapableField<T>` concept

| Signature | Checks |
|---|---|
| `template <typename T> concept EmptyCapableField` | `const T&` has a `noexcept` `.hasValue()` returning convertible-to-`bool`. |

### `Choice<T, OptionsAction, ValueField, LabelField, DependsOn...>` and `FixedString<N>`

Both types are **owned by `choice.hpp` and specified in full in
[choice.md](choice.md)** — this spec does not restate their member-by-member API,
to avoid two copies drifting apart. In brief: `Choice<T, "Action", "value",
"label">` is an optionally-empty value (`std::optional<T>` payload, `hasValue()`,
unchecked `operator*`, defaulted `operator==`) whose options come from executing
a named registered action; `optionsAction()`/`valueField()`/`labelField()`
expose the compile-time metadata that `mergeSchemaExtras` reads to emit
`x-optionsAction`/`x-optionValue`/`x-optionLabel`. An optional trailing
`DependsOn` pack names sibling fields whose current values parameterise the
options action (a cascading picklist); `optionsDependsOn()` exposes it, and
`mergeSchemaExtras` emits `x-optionsDependsOn` only when it is non-empty — an
independent `Choice` (the default) is unaffected. `FixedString<N>` is the
`consteval` NTTP string that carries those names inside the `Choice` type. The
`isChoice<T>` trait (`true` for any cvref-stripped `Choice`) is what
`mergeSchemaExtras` and `allRequiredEngaged` branch on. See [choice.md](choice.md)
for the exhaustive tables and design rationale.

### Cross-field rules

| Symbol | Kind | Purpose |
|---|---|---|
| `EngageableField<T>` | concept | `EmptyCapableField<T>` or `std::optional<...>` — the broader "has an empty state" test the rule vocabulary uses. |
| `RuleList<Rules...>` | class template | Holds an action's declared rules, in declaration order. Built by `ruleList(...)`; never constructed directly. |
| `ruleList(rules...)` | function template | Composes rule/condition nodes into the `RuleList` an action assigns to `formRules`. |
| `HasFormRules<A>` | concept | `true` when `A` declares a `static constexpr formRules` member. |
| `allRulesSatisfied<A>(action)` | function template | `true` when every **validation** rule in `A::formRules` holds (or there are none); skips presentation rules. `noexcept`. |
| `engaged`/`notEngaged`/`equals`/`greater`/`greaterOrEqual`/`less`/`lessOrEqual`/`requiredWhen`/`exactlyOneOf`/`atLeastOneOf`/`mutuallyExclusive`/`visibleWhen`/`readonlyWhen`/`andOf`/`orOf`/`notOf` | function templates | Factories building one typed rule/condition node each; see the kind table above. |
| `UnsatisfiableFormError` | struct (`std::logic_error`) | Thrown by `schemaJson<A>()` when a capping rule ranges over two or more fields `A` also makes `required`. Its `what()` names the action type, the rule kind, and the offending fields. |
| `detail::capsEngagedCount(kind)` | function | `true` for the emitted rule kinds that impose a ceiling on how many of their fields may be engaged (`"exactlyOneOf"`, `"mutuallyExclusive"`). The single place those kinds are named. |
| `detail::rejectUnsatisfiableRules<A>(xRules, requiredNames)` | function template | Throws `UnsatisfiableFormError` when a capping rule node names two or more fields present in `requiredNames`. Called from `mergeSchemaExtras`. |
| `detail::ConditionActionType<Cond>` | alias template | The action type `A` a condition/rule node's `test(const A&) const noexcept` ranges over, deduced from `&Cond::test`'s member-function-pointer type. Used internally by `andOf`/`orOf`/`notOf` to recover `A` without every leaf node separately naming it. |

### `computed<Dst, Inputs...>()` / `computeList()` / `recomputeAll<A>()`

| Signature | Returns |
|---|---|
| `template <auto Dst, auto... Inputs, typename Fn> auto computed(Fn fn)` | A `detail::ComputedField<Dst, Fn, Inputs...>` value. |
| `template <typename... Fields> auto computeList(Fields... fields)` | A `detail::ComputeList<Fields...>` value — assign to `static constexpr auto computedFields`. |
| `template <typename A> void recomputeAll(A& action)` | Overwrites every `A::computedFields` destination in place; a no-op when `A` declares none. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Required default | **All members required unless explicitly opted out** | The safer default for domain forms — forgetting to mark a field optional would leak data, not lose it. Opt out via `std::optional` or `optionalFields` list. |
| Optional mechanism | **Two orthogonal opt-outs** | `std::optional<T>` handles library types (glaze already knows how to serialise them); `optionalFields` handles custom types like `Quantity` whose emptiness is not expressed through `optional`. |
| Schema caching | **`static const std::string` inside the template** | Same schema for the same type in every translation unit. No synchronisation needed — schema generation does not mutate anything. |
| Failure mode | **Returns raw glaze schema (or empty) rather than throwing** | Schema generation is a description facility; crashing a server over a malformed schema would be wrong. |
| Unsatisfiable declaration | **Rejected at generation with `UnsatisfiableFormError`, the one exception to the row above** | A capping rule (`exactlyOneOf`/`mutuallyExclusive`) over two or more `required` fields yields a form *nobody can submit*, not a form missing a hint — the failure is certain at generation time and belongs to the author's build, not to a user who cannot press Save. See [Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules). |
| `Choice` metadata | **In the type, not the payload** | The set of options for a field is a compile-time property of the action, not a runtime property of each submission. The generated schema communicates it to the client; payloads carry only the selected value. |
| Wire serialisation | **Glaze `meta` reflects `value` directly** | `Choice<T, ...>` serialises as `T \| null` — the options metadata never travels. |
| Options action | **A registered action type id** | The same action dispatch mechanism handles queries for picklist data, so no separate protocol or endpoint is needed. |
| Dependent `Choice` options | **Sibling values as the options-action request body, not a new dispatch mechanism** | `Choice`'s `DependsOn` pack only changes what body a renderer sends; the options action stays an ordinary registered action reached through the same `executeJson`/`ActionDispatcher` seam as every other action, so multi-parent cascades and independent `Choice`s coexist with no new framework surface. |
| `x-order` | **Always emitted, on every property** | JSON object key order is not reliable across DOM implementations; the explicit index gives renderers a deterministic layout. |
| Cross-field rules | **Closed, typed vocabulary, one declaration → schema + client + server** | Client and server must evaluate cross-field conditions identically; a closed set of framework-owned node types (not application lambdas) is what makes that possible. Arbitrary logic that does not fit stays in `validate()`/`execute`, unreflected into `x-rules`, exactly as `allRequiredEngaged` already draws the line for per-field required-ness. |
| `x-unitAlternatives` | **Derived from `UnitTraits::relations`** | The same `UnitRelation` entries that drive `convert` also drive the display-unit selector — no separate declaration to keep in sync. |
| `Timestamp` | **Uses standard `"format": "date-time"`** | No extension annotation needed; standard JSON-Schema vocabulary is sufficient. |
| Layout declaration | **`static constexpr formLayout` / `fieldSpans`, mirroring `optionalFields`** | Visual structure is a compile-time property of the action, exactly like the existing opt-out list; a renderer that ignores it degrades to the flat `x-order` form with no missing fields. |
| Widget selection | **Type-derived by default (`Multiline`/`Ranged`), `fieldMetadata`-shaped override wins** | Mirrors the `Choice`/`Quantity` pattern: the control is a compile-time property of the type; the escape hatch is a typed declaration, not a schema-only knob. |
| Widget override lookup | **Duck-typed on `.field`/`.widget`, not a named type** | Keeps `forms.hpp`'s widget lookup free of a hard dependency on any one field-metadata descriptor type declaration; any shape exposing those two members is honoured, `FieldMeta` ([above](#field-metadata--fieldmeta)) included. |
| Computed fields | **One declaration (`computed`/`computeList`) drives schema + client + server via a single shared `recomputeAll`** | The same evaluator runs on the reactive client path and on every server dispatch path, so the displayed value and the stored value are derived identically — a computed field can never drift, and the server never trusts a client-submitted derivation. |

## Failure modes

### Nested aggregates (recursive, cycle-guarded)

A member whose type is itself a reflectable aggregate — a plain nested
struct, or `std::vector<Sub>` (a repeated aggregate) — gets its **own**
members annotated too: `x-order`, `title`/`FieldMeta`, `required`, and the
`Quantity`/`Choice`/widget/ranged-bounds rules the top level already applies.
Unlike the top level, this recurses to **whatever depth the type graph
actually has** — a nested aggregate's own nested-aggregate member is
annotated in turn, and so on — rather than stopping after one level. This
closes the gap a flat-only generator has for domains that are naturally
nested (a measurement with a repeated specimen sub-record, a document with a
nested address, a category tree), including domains nested more than one
level deep (an address with a nested geo-coordinate sub-record, say).

Two schema shapes exist for a nested aggregate, and both are recursed into:

- **Deduplicated (`$ref`/`$defs`)** — glaze shares one `$defs` entry, `$ref`'d
  from every property, when the nested type is used **two or more times**
  anywhere in the schema. The shared `$defs` entry is annotated once; every
  property that `$ref`s it sees the same annotations.
- **Inlined** — glaze writes the object schema directly into the property
  itself (no `$ref`/`$defs` at all) when the nested type is used **exactly
  once**. The property node itself is annotated in place.

`mergeSchemaExtras` resolves whichever form applies (`annotateNestedAggregateRef`,
`forms.hpp`) and hands the resolved node to the same per-member annotation
logic the top level uses (`annotateBasicMemberProperty`), applied against the
nested type's own reflection. Each recursive step passes along the **ancestor
chain** — the action type `A`, followed by every nested-aggregate type
visited since, in order, ending with the type currently being annotated — as
a variadic template parameter pack, so a deeper call can tell whether a
member's type is already somewhere on that chain.

**Cyclic nested aggregates are a compile error, not infinite recursion.** A
member whose type (or, for a `std::vector<Sub>` member, `Sub` itself) matches
any type already on the ancestor chain — the action type, the
nested-aggregate type currently being annotated, or anything annotated in
between (a self-referential type such as `struct Node { std::vector<Node>
children; };`, or a mutual reference between two distinct types) — trips a
`static_assert` at the point that specific recursive instantiation would
occur, instead of recursing forever. This only rejects genuine cycles: a
"diamond" — the same type reused from two unrelated places in the schema,
e.g. an `Address` nested under both a `Company` and a `Person` member of the
same action — is not a cycle (neither `Address` nor any of its members is its
own ancestor) and recurses normally into both. Restructure a domain type
that trips this (e.g. flatten the self-reference, or represent the recursive
edge as an opaque id instead of a nested value) if you need one; there is no
runtime opt-out. The `static_assert` only fires where the offending type is
actually reached as a nested-aggregate member of some `schemaJson<A>()` (or
`mergeSchemaExtras<A>()`) instantiation — a self-referential type that is
never nested under an action this way compiles and works fine on its own.

Computed fields, `formLayout`/`fieldSpans`, and `formRules` remain **top-level
only** regardless of nesting depth: a nested aggregate declaring any of those
has no effect on the generated schema. This keeps the generator focused on
what a nested-aggregate schema actually needs (per-field annotations) rather
than becoming a general recursive-descent schema compiler that also
re-derives layout/rules/computed-field semantics at every level.

**Purely additive, with one source-compatibility exception.** An action with
no nested-aggregate member has nothing here to trigger on, so its generated
schema is byte-for-byte unchanged. A pre-existing action that *does* have a
nested-aggregate member sees its schema gain annotations it previously
lacked — the whole point of this feature — with no change to any of its flat
top-level members. The one exception: an action with a self- or
mutually-referential nested-aggregate member (see the cycle-guard paragraph
above) now fails to *compile*, where it previously compiled (recursion used
to stop before reaching the cycle). No such action exists in this repo today.

Every nested-aggregate type in the chain must be **default-constructible**,
exactly like the top-level action type (see below): the recursion builds its
own probe instance purely to enumerate its members via reflection.

### Scope: flat actions only (form layout, computed fields, and rules)

`formLayout`/`fieldSpans` ([Layout & grouping](#layout--grouping)) and
`formRules` ([Cross-field rules](#cross-field-rules--the-x-rules-vocabulary)) are read only
from the top-level action type — they are not consulted on a nested
aggregate, no matter how deep `mergeSchemaExtras` otherwise recurses (see
[Nested aggregates (recursive, cycle-guarded)](#nested-aggregates-recursive-cycle-guarded)
above). Computed fields (`computedFields`) are likewise top-level only.

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
The empty string is the only failure signal a caller receives on this path; the
one case in which schema generation throws instead is a self-contradicting
declaration ([Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules)),
which is an author error in the action type, not a failure of the input schema.

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

Because `allRulesSatisfied` (above) is typically one of the two conjuncts of
`validate()`, a `formRules` declaration is enforced on exactly the same paths
`ActionValidator<A>::ready` already is — no separate enforcement seam.

### Advertised precision is enforced on dispatch

`x-decimalPlaces` advertises a field's **declared** precision
(`Quantity<U, Dec>::declaredDecimals`), but a `Quantity` on the wire carries its
own runtime `dp`, which a client may set to anything. On the client bridge
dispatch path (`executeJson` → `ActionExecuteRegistry`) these are **reconciled**:
after decoding and before dispatch, `morph::forms::reconcileDeclaredPrecision`
**rounds** every `Quantity` member of the action to `declaredPrecision()`, so the
value the handler stores is at the precision the schema advertised, not at the
client's submitted `dp`. An empty `Quantity` stays empty. The reconciliation is a
no-op for actions with no `Quantity` members and for action types glaze cannot
reflect. `ActionDispatcher::registerAction`'s runner performs the same
reconciliation on the server-side wire path (see
[registry.md](../core/registry.md)), so `x-decimalPlaces` is an enforced contract
on **both wire paths** — client-bridge and remote.

**Rounding, not retagging — and why the difference is the whole point.**
`Quantity::atDeclaredPrecision()` performs an exact `Rational` re-rounding
(`math::roundToDecimalPlaces`, half away from zero, matching the decimal
formatter — see
[rational.md](../util/rational.md#roundtodecimalplaces--the-one-helper-that-stays-in-the-domain)).
It does **not** merely move the `DecimalPlaces` tag. A tag-only change would
leave a field declaring `dp = 1` holding exactly `1.23456` while rendering
`1.2`: the report says one number and the database another, and an audit trail
cannot say which one the operator saw when they signed off. For a framework whose
premise is exact values for financial and lab data, that failure mode is worse
than not enforcing at all — the value *looks* compliant. Enforcement therefore
means the submitted precision beyond the declared amount is **discarded, not
hidden**, and the reconciled value is by construction the value the form was
already displaying.

The operation normalises rather than rejecting an over-precise submission. Two
reasons. A wire `dp` finer than the declared one is not by itself a protocol
violation — `{"num":6,"den":5,"dp":5}` is exactly `1.2`, perfectly representable
at `dp = 1`, and rejecting it would fail a payload with nothing wrong in it;
detecting the genuinely over-precise case means testing the *value*, not the tag.
More decisively, the same `atDeclaredPrecision()` call is what `recomputeOne`
applies to **server-derived** values (see [Computed fields](#computed-fields)),
which routinely carry more decimals than the destination field declares — a
product of two 2-decimal operands is exact to 4. A reject-shaped contract would
have the server reject its own arithmetic. Normalising is the only rule both call
sites can share.

**The in-process path is deliberately not reconciled.** `Bridge::executeVia`'s
`localOp` decodes no JSON — it dispatches an already-typed `Action` a caller
constructed in C++ — so there is no client-supplied `dp` to reconcile and no
reconciliation step there (`bridge.hpp` says so at the call site; cross-ref
[quantity_type.md](../util/quantity_type.md), which describes the same asymmetry
for `enforceQuantityBounds`). A `Quantity` built by calling code keeps whatever
precision the caller gave it. Computed fields *are* still normalised on that
path, since `recomputeAll` runs there and `recomputeOne` rounds to the
destination's declared precision.

### Pre-decode wire validation — `checkQuantityBounds`

Reconciling declared precision (above) still leaves a wire payload that is
merely *representable*, not necessarily *physically or contractually
sensible* — a percentage of `250`, a mass of `-5`. `morph::forms::
checkQuantityBounds<A>(action)` closes that gap: it walks every reflected
`Quantity` member of a decoded action and checks
`Quantity::withinDeclaredBounds()` (see
[quantity_type.md, "Pre-decode wire validation"](../util/quantity_type.md#pre-decode-wire-validation--declared-bounds))
against the optional `UnitTraits<E>::bounds(E)` a unit may declare. It returns
the wire name of the first offending field, or `std::nullopt` — a no-op for
actions with no `Quantity` members, or whose units declare no bounds.
`morph::forms::enforceQuantityBounds<A>(action)` is the throwing counterpart,
raising `morph::forms::QuantityDecodeError` naming that field.

Both wire-decoding dispatch runners — `ActionDispatcher::registerAction`'s
server-side runner and `ActionExecuteRegistry::registerAction`'s client
bridge runner (see [registry.md](../core/registry.md)/[bridge.md](../core/bridge.md))
— call `enforceQuantityBounds` immediately after `reconcileDeclaredPrecision`
and before `recomputeAll`/`ActionValidator<A>::ready`, so an out-of-bounds
wire value is rejected **before an action's own `validate()` ever sees it**.
`QuantityDecodeError` is deliberately **not** `morph::model::ValidationError`
— the two error types stay distinct so a caller can tell "the wire payload
itself was impossible" (a decode-level, framework-enforced constraint) from
"the decoded action failed its own business rule" (a `validate()`-level
rejection an action author wrote). The in-process `Bridge::executeVia`
`localOp` path is unaffected — no JSON decode happens there (see "Advertised
precision is enforced on dispatch" above for why `reconcileDeclaredPrecision`
is likewise skipped on that path), so a `Quantity` a caller constructs
directly carries whatever value the caller gave it, unchecked at this seam.

### Sum types not in the forms palette — multi-field encoding by design

The forms vocabulary provides no native sum-type (tagged union, discriminated union) support. When an action field must express *one of several alternatives* (e.g. a measurement that is "a quantity, or below limit-of-detection, or above upper detection limit"), encode it as a **multi-field structure glued by cross-field rules**: one field for the quantity, one boolean or enum for the state (measured/below/above), and a `RequiredWhen`/`VisibleWhen` rule that gates each based on the others. This is by design: sum types are rare in domain models that already use `hasValue()` optionality and `Choice` enums, and the rule-based multi-field encoding is expressive enough for the rungs' needs while keeping the schema and validation machinery focused.

The encoding carries one obligation: every field the capping rule ranges over must be named in `A::optionalFields`, so the rule is the *only* gate on them. Omitting that leaves `required` demanding every alternative at once, which contradicts the rule — `schemaJson<A>()` rejects it rather than serving an unsubmittable form ([Unsatisfiable declarations](#unsatisfiable-declarations--required-contradicting-x-rules)).

### Per-instance variation is limited to key *values*

`schemaJson<A>()` derives every key from the compiled type, so two rows that
describe the same action differently cannot be told apart by it.
`InstanceConstraints` (above) lifts that for the keys it covers —
`x-decimalPlaces` and the exact bounds — but only for their *values*. Which
fields exist, the `required` array and the `x-rules` list remain functions of
`A`, so a definition wanting a different *shape* still needs a recompile, and
an application with per-definition shapes needs one compiled action per family
of shapes rather than one per definition. Applying an instance constraint also
stays the model's job rather than the dispatch runner's; see
[instance_constraints.md](instance_constraints.md), "Why dispatch cannot apply
these automatically", for why neither of those is an oversight.

### One cached schema per type — no localisation

Each type's schema is memoised in a function-local `static const std::string`
(`schemaJson<A>()`), computed by the first caller and shared process-wide
thereafter (first-caller-wins; no synchronisation, since generation mutates
nothing). This baking-in **precludes localised / i18n schemas**: the human
display strings that land in the schema (unit `display`/`unitUnicode`, and any
`description` text) are fixed at first call. There is no per-request or
per-locale schema variant — a translated form would need a different mechanism
entirely. See [Localisation — message keys and the catalog seam](#localisation--message-keys-and-the-catalog-seam)
for that mechanism.

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
| [instance_constraints.md](instance_constraints.md) | `InstanceConstraints` — serving and checking the framework-meaningful keys whose values live in data rather than in the compiled type. |
| [workflows_navigation.md](workflows_navigation.md) | The wizard/app-shell layer built on this schema — one wizard step or one `kind: "form"` app screen still renders as an ordinary action form. |
| [views.md](views.md) | The view-schema layer (`morph::views`) that composes query+edit+delete action *sets* into list/table and master-detail screens; reuses `schemaJson<Row>()` unmodified to derive each column's `ExtUnits`/`x-decimalPlaces`. |
| [widget_hints.md](widget_hints.md) | Full `Multiline`/`Ranged` API and design (this spec cross-refs rather than duplicates it). |
| [quantity_type.md](../util/quantity_type.md) | `Quantity`, its unit tags, `UnitTraits::relations`, and `convert` — the source of `x-decimalPlaces`, `x-unitAlternatives`, and `ExtUnits`. |
| [datetime.md](../util/datetime.md) | `DateTime` / `Timestamp`, the ISO-8601 wire format, and the `"format": "date-time"` schema annotation. |
| [rational.md](../util/rational.md) | Exact `Rational` values; the `num`/`den` in each `x-unitAlternatives` entry are a `Rational` numerator/denominator, which is why unit switches recompute exactly. Also the comparison/equality `greater`/`greaterOrEqual`/`less`/`lessOrEqual`/`equals` use for numeric fields, so client and server compare identical values. |
| [security.md](../security.md) | The dispatcher's trust boundary — why `required` gates only the client and handlers must re-validate. |
| [session.md](../session/session.md) | `Context::locale`, the server-side hook for data (not chrome) localisation — the one place `session::current()->locale` participates, for `Choice` option-row labels. |
| [bridge.md](../core/bridge.md) | The `ActionExecuteRegistry`/`executeVia` authoritative recompute sites. |
| [registry.md](../core/registry.md) | `ActionDispatcher::registerAction`'s runner — the server-side authoritative recompute site. |

## Out of scope

- Generating schemas for non‑action types (the module assumes glaze reflection is
  available on `A`).
- Validating payloads against the schema — the schema is for the *client*.
- Executing the options action — `choices` metadata tells the client *which*
  action to call, but the forms module does not invoke it.
- `morph::time::Timestamp` definition — it is only consumed here via
  `EmptyCapableField`.