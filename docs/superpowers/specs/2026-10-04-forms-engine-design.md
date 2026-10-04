# The forms engine: schema-driven forms through the view tree — design (spec 2)

morph's schema-driven forms render today only through the shipped QML module: about 4,000 lines of
QML/JS (`DynamicForm.qml`, `CollectionView.qml`, `WizardView.qml`, `SlotRegistry.qml`,
`DateTimePicker.qml`, `JsonExact.js`) that read the schemas C++ emits, keep the drafts, encode
values exactly, evaluate the rules, fetch Choice options, lay fields out and sequence wizards. C++
has the schema emitters and the typed validators, but no runtime model of a form. This spec moves
that runtime into C++ as one engine whose output is a `ui::Node` (spec 1), so the same form renders
on the TUI and on Qt Quick, and retires the QML module once every example has moved (spec 4).

Two ways in, one engine: a form can be **built at runtime purely from schema JSON**, for
applications that assemble UIs from data, and **statically typed from an action type**, for
applications that want the compiler's help. Both produce the same runtime `FormModel`, so rules
are evaluated in exactly one place.

## Contents

[1 Shape](#1-shape) · [2 The field model](#2-the-field-model) · [3 Rules](#3-rules) ·
[4 Values and exactness](#4-values-and-exactness) · [5 FormSession](#5-formsession) ·
[6 Typed forms](#6-typed-forms-formltagt) · [7 Rendering](#7-rendering) ·
[8 Collections, wizards, app shell](#8-collections-wizards-app-shell) · [9 Packaging](#9-packaging) ·
[10 Parity and tests](#10-parity-and-tests) · [11 Retiring the QML renderer](#11-retiring-the-qml-renderer) ·
[12 Docs](#12-docs) · [13 Risks](#13-risks)

## 1. Shape

```
schema JSON ──FormModel::fromSchema──┐
                                     ├─► FormModel ─► FormSession (reactive) ─► formView() ─► ui::Node
schemaJson<A>() ──Form<A>────────────┘                    │
                                                          └─ Submitter / ChoiceFetcher (bridge)
```

- **`FormModel`** — immutable, runtime: the fields, their kinds, constraints, layout and rules.
- **`FormSession`** — reactive state over a model: drafts, encoded values, readiness, presentation
  rules, Choice options, submission. It is a controller in spec 1's sense (no toolkit, testable
  headless).
- **`formView(session, options)`** — the view: `ui` nodes bound to the session.
- **`Form<A>`** — a typed facade that builds the model from `schemaJson<A>()` and adds typed
  access. It owns a `FormSession`.

The schema JSON that `schemaJson<A>()` emits is the contract between the two entrances; it is
already pinned by the conformance and rule corpora, and nothing about it changes here.

## 2. The field model

`FormModel::fromSchema(std::string_view actionType, std::string_view schemaJson)
-> std::expected<FormModel, SchemaError>` reads the emitted keywords (glaze's generic JSON DOM),
resolving `$ref` and nullable `anyOf`/`oneOf`, and produces one `FieldSpec` per property in
`x-order`.

| `FieldKind` | From | Wire encoding |
|---|---|---|
| `Text`, `Multiline` | `string`; `x-widget: textarea` | string |
| `Integer` | `integer` | int64, digits preserved beyond 2^53 |
| `Number` | `number` (with `x-displayDecimals`) | JSON number |
| `Decimal` | `x-decimalPlaces` on a `Rational` | `{num, den, dp}` |
| `Quantity` | `ExtUnits` (+ `x-unitAlternatives`) | `{num, den, dp}` in the canonical unit |
| `Boolean` | `boolean` | `true`/`false` |
| `Enum` | `enum` / `const` branches (`x-widget: radio` for radio) | the enum value |
| `Choice` | `x-optionsAction`, `x-optionValue`, `x-optionLabel`, `x-optionsDependsOn` | the option's value |
| `DateTime`, `Date` | `format: date-time` / `date` | ISO 8601 UTC |
| `Slider` | `x-widget: slider` with `x-min/max/step` | integer |
| `Array` | array of scalars | JSON array |
| `Object` | nested object (recursion, cycle-safe) | nested object |
| `ObjectArray` | array of objects | array of nested objects |

A `FieldSpec` carries: wire name and path; label, help, placeholder and their i18n keys
(`forms/i18n.hpp`); required (`required` and `optionalFields`), `x-blankAs`; `x-readonly`,
`x-hidden`, `x-computed`; exact bounds (`minimum`/`maximum`/`multipleOf`, `x-exactMinimum`/
`x-exactMaximum`, instance `x-minimum`/`x-maximum` from `InstanceConstraints::decorate`), decimal
places, display decimals; unit and alternatives with their exact factors; enum values; the Choice
descriptor; layout (`x-section`, group kind Section/Tab/Accordion, `x-colspan`); the widget hint;
children for `Object`/`ObjectArray`. The model also carries the action type, title, groups, the
parsed rules (§3) and the submit mode (`x-submitMode`: automatic or explicit).

**Nothing is unrepresentable.** Nested objects render as sub-panels and object arrays as rows with
Add/Remove (§7), so the "unrepresentable unless a slot claims it" state of the QML renderer goes
away.

## 3. Rules

`x-rules` is parsed into a `RuleExpr` tree mirroring the 16 rule kinds (`engaged`, `notEngaged`,
`greater`, `greaterOrEqual`, `less`, `lessOrEqual`, `equals`, `requiredWhen`, `exactlyOneOf`,
`atLeastOneOf`, `mutuallyExclusive`, `visibleWhen`, `readonlyWhen`, `andOf`, `orOf`, `notOf`).

- Evaluation is over the session's encoded values and is **three-valued** — `True`, `False`,
  `Unknown` (a field that does not encode yet) — because a half-typed form has fields with no
  value; `Unknown` propagates through `andOf`/`orOf`/`notOf` by Kleene logic.
- **Gating rules** decide readiness: a gating rule blocks readiness while it is `False`; `Unknown` does not
  block on its own (the fields it waits on are required, or do not encode, and block through readiness).
  **Presentation queries** — `visible(field)`, `readonly(field)`, `dynamicallyRequired(field)` —
  come from `visibleWhen`, `readonlyWhen` and `requiredWhen`; `Unknown` presents as visible,
  editable and not required.
- Comparisons are exact: integers as digit strings, decimals and quantities as `Rational`, booleans
  as booleans — the two divergences the agreement tests were written against (`equals` against a
  bool, and against an int64 above 2^53) cannot arise, because nothing passes through a double.
- This is the **only** rule evaluator on the client. The typed rule nodes (`allRulesSatisfied<A>`)
  stay the server-side check; the agreement corpus (§10) now compares two C++ evaluators in one
  test binary.

## 4. Values and exactness

Per field the draft is what the user typed (text), the selected unit, and whether a blank-capable
field is engaged. Encoding a draft yields `std::expected<WireValue, FieldError>`:

- **`morph::math::parseDecimal(std::string_view, NumericLocale const&) -> std::optional<Rational>`**
  — new, beside `Rational` in `util/rational.hpp`: locale input normalised by
  `render::normalizeLocaleNumber`, then digits → `Rational` with the declared decimal places,
  rejecting more places than declared. C++ has no decimal parser today; this is the one the forms
  engine and any app share.
- Quantity unit conversion is exact `Rational` arithmetic with the factors `x-unitAlternatives`
  carries; integers keep every digit; date-times convert from the display offset to UTC.
- Bounds, `multipleOf`, enum membership and instance bounds are checked exactly; each failure is a
  `FieldError` with a stable code and the i18n'd message.
- `x-blankAs: Omit` leaves a blank field out of the body; `Empty` sends the kind's empty value.
- The **body** is assembled as JSON text in `x-order`; it is the byte-for-byte contract the parity
  tests compare. Prefill decodes a JSON body back into drafts (the reverse of every encoder).
- Display uses `render::formatCanonicalNumber` and `render::resolveText` with the session's
  `TranslationProvider` and locale — both headers exist and gain their first production caller.

## 5. FormSession

```cpp
class FormSession {
public:
    FormSession(reactive::Runtime&, FormModel, Submitter, ChoiceFetcher, FormSessionOptions = {});
    FieldState& field(std::string_view path);          // per-field signals and computeds
    bool ready() const;                                // tracked
    std::optional<std::string> body() const;          // tracked; nullopt until encodable
    void submit();                                     // explicit mode, or forced
    void prefill(std::string_view bodyJson);           // programmatic; never auto-submits
    void reset();
    // tracked: pending(), lastReply() (ok + payload text), lastError()
};
using Submitter = std::function<async::Completion<std::string>(std::string_view actionType, std::string bodyJson)>;
using ChoiceFetcher = std::function<async::Completion<std::string>(std::string_view optionsAction, std::string bodyJson)>;
```

- `FieldState`: `text`, `unit`, `engaged` signals; `encoded`, `error`, `visible`, `readonly`,
  `required` computeds; for a Choice, `options` (a `reactive::Query` keyed on its dependency
  fields' encoded values, so a parent change refetches and a superseded reply is dropped).
- **Readiness:** every required field encodes, no field has an error, no gating rule is `False`. A form that is
  already ready when it is built submits once in automatic mode, as the QML renderer did.
- **Submission** is a `reactive::Mutation` over the `Submitter`. In automatic mode an Effect submits
  whenever the form becomes ready with a changed body — the behaviour `DynamicForm` has today —
  except while a programmatic change (prefill, reset) is applying; in explicit mode only `submit()`
  does.
- **Choice options:** the rows are the result if it is an array, else its first array member; a
  selection no longer among the options is cleared.
- **Submitters:** `forms::bridgeSubmitter(Bridge&, exec::IExecutor&)` routes an action type to the
  model that registers it (the registry learns which models serve an action) and executes the JSON body
  through a `BridgeHandler`'s `executeJson` that it keeps per model — right for database-backed models.
  `forms::handlerSubmitter(executor, handlers...)` routes through the application's **own** handlers
  instead, for in-memory models and shared instances, where a separate handler would be a separate model
  instance; `bridgeChoiceFetcher` / `handlerChoiceFetcher` are the same for options. Tests pass a fake.

## 6. Typed forms: `Form<A>`

```cpp
template <class A, class M, class S>
class Form {
public:
    Form(reactive::Runtime&, bridge::BridgeHandler<M, S>&, FormSessionOptions = {});
    FormSession& session();
    template <auto Member> void set(MemberType<Member> value);   // programmatic, typed
    std::optional<A> value() const;                              // tracked: the body decoded into A
    bool ready() const;          // session readiness && ActionValidator<A>::ready(*value())
    // tracked: lastResult() -> std::optional<ActionTraits<A>::Result>, pending(), error()
};
```

- The model is `FormModel::forAction<A>()` = `fromSchema(ActionTraits<A>::typeId(), schemaJson<A>())` —
  the same reader, so a typed form behaves exactly like the runtime one for the same action.
- Submission decodes the body into `A` (glaze) and calls `handler.execute(a)`, so the result is
  typed. `ActionValidator<A>` adds the action's own `validate()` to readiness.
- `set<&A::field>(v)` encodes `v` with the field's encoder and applies it as a programmatic change.

## 7. Rendering

`forms::formView(FormSession&, FormViewOptions) -> ui::Node`; `FormViewOptions` carries the
`Overrides`, the grid column count, and chrome strings.

| Kind | Node |
|---|---|
| Text / Multiline | `TextInput` (SingleLine / Multiline) |
| Integer, Number, Decimal | `TextInput` |
| Quantity | `Row{TextInput, Select(unit)}` (`Select` only with alternatives) |
| Boolean | `Checkbox` |
| Enum | `Select` (Dropdown, or Radio for `x-widget: radio`) |
| Choice | `Select` bound to the options query, `Busy` while it loads |
| DateTime / Date | `DateTimeInput` |
| Slider | `Slider` |
| Array | `TextInput`, comma-separated |
| Object | `Panel` with the child fields |
| ObjectArray | a `ForEach` of child `Panel`s with Remove, and an Add button |

- Every field: label, the input, help text, the error (`TextRole::Error`), bound `visible` and
  `enabled` (readonly). Layout: sections are `Panel`s, tab groups (consecutive runs merged) are
  `Tabs`, accordions are collapsible `Panel`s, `x-colspan` places fields in a `Grid`.
- Explicit mode adds a Submit `Button` bound to readiness; every form shows the last reply or error.
- **Overrides** (the replacement for `SlotRegistry`): `forms::Overrides` maps by field, widget hint,
  unit and kind — resolved in that order, then the default — to
  `std::function<ui::Node(FieldView&)>`, where `FieldView` exposes the field's spec and state.

## 8. Collections, wizards, app shell

- **`CollectionModel::fromSchema(viewJson)`** (the `v-*` keywords; typed via `viewSchemaJson<V>()`),
  **`CollectionSession`**: the list is a `Query` on `v-query`; row and collection actions are
  `Mutation`s whose bodies are built from `bind` entries with exact ids; an action with `confirm`
  opens a `Dialog`; a row's edit action opens an editor `FormSession` prefilled from the row; a
  success refetches the list. **`collectionView`** renders a `Table` (columns from `v-columns`,
  cells formatted exactly — not through a double as today) plus the action buttons, or master-detail
  with a side form.
- **`WizardModel`** (`w-*`), **`WizardSession`**: one `FormSession` per step, kept alive; a step is
  done when its submission succeeded; Back/Next; entering a step applies its prefill from the
  resolved values (draft first, then the reply, the reply winning on a name collision — the rule
  `FlowSession` documents) as JSON values, not as re-encoded text. **`wizardView`** renders the step
  title, the step's form and the navigation.
- **`AppShellModel`** (`app-*`), **`appShellView`**: a `Menu` from `app-menu` beside a `Switch` over
  `app-screens`, kinds `form`, `wizard` and `view` — `forms/app.hpp` gains `ViewScreen<Id, View>`
  (kind `"view"`), which the forms demo needs.

Sections (`SectionSet`) keep their typed session and gain no renderer here.

## 9. Packaging

Header-only, in the base `morph` target, under `include/morph/forms/engine/`: `field_model.hpp`,
`rules.hpp`, `form_session.hpp`, `typed_form.hpp`, `form_view.hpp`, `overrides.hpp`,
`collection.hpp`, `wizard.hpp`, `app_shell.hpp`, `bridge_submitter.hpp`; `parseDecimal` in
`util/rational.hpp`. It depends on `morph::reactive` and `morph::ui`, never on a frontend.

## 10. Parity and tests

Parity with the QML renderer is measured, not asserted:

- **Rule corpus:** `rule_corpus.json` moves to `tests/data/`; all 46 rows' `ready`, `visible` and
  `readonly` verdicts from the runtime evaluator, and the typed-vs-runtime agreement over the same
  rows.
- **Bodies:** byte-identical bodies for the typed-text states the QuickTests drive — exact
  `{num, den, dp}`, int64 beyond 2^53, unit conversion, blank-as, enums, booleans, date-time zones.
- **Conformance, instance-bounds and locale corpora:** every fixture and row, read from the shared
  data files (the hand-mirrored QML copies go away with the QML).
- **Behaviour:** each of the 35 QuickTest files' claims is re-expressed against `FormSession` and
  `formView` on the `RecordingBackend` (prefill round-trip, dependent Choice refresh, automatic vs
  explicit submission, collection populate/refresh/confirm, wizard gating and prefill, i18n
  resolution, layout runs); the plan maps every file to its replacement.
- **Frontends:** `formView` output passes spec 1's backend-conformance cases on the TUI and Qt Quick.

## 11. Retiring the QML renderer

The last commit of the program (spec 4), once no example uses it: `src/qt/forms/` (QML, JS,
`I18nCatalog`, the QuickTest suite), `include/morph/qt/forms/` (`FormsControllerCore`,
`MultiModelFormsControllerCore`, `GenericModelBridgeCore`, `MultiModelBridgeCore`), the
`morph_qt_forms` target and the MorphForms module, `MORPH_BUILD_FORMS_QML`, the `qt_forms`,
`forms_qml` and `forms_qmlplugin` install components and their `morphConfig.cmake.in` blocks,
`scripts/check_forms_qml_install.sh` and its CI job. Headers the `morph::qt` component also uses
stay with it. CHANGELOG → Removed, naming the replacement.

The forms demo's self-contained HTML/JS renderer (`examples/forms --emit-html`) stays: it is an
example of consuming the schemas from a browser, not a renderer morph ships.

## 12. Docs

`docs/spec/forms/engine.md` (new: model, rules, values, sessions, rendering, overrides);
`forms.md`, `views.md`, `workflows_navigation.md` and `widget_hints.md` lose their QML-renderer
sections and point at the engine; `docs/spec/README.md` map; `ARCHITECTURE.md`; CHANGELOG
`[Unreleased]` → Added (the engine, `parseDecimal`, `ViewScreen`).

## 13. Risks

- **Size.** The engine re-implements ~4,000 lines of QML/JS in C++; header-only keeps morph's
  packaging but costs compile time in every TU that includes `form_view.hpp`. The headers are split
  so a controller that only needs `FormSession` does not pull in rendering.
- **Automatic submission on every ready change** is kept for parity; an app that wants explicit
  submission says so with `x-submitMode`, as today.
- **Parity is only as good as the claims re-expressed**; the plan lists each QuickTest file's
  replacement so a dropped claim is visible in review.
