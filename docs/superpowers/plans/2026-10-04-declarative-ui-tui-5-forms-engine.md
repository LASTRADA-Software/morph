# Declarative UI, Part 5 — the C++ forms engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Ship the C++ forms engine — `FormModel`, `FormSession`, `Form<A>`, `formView`, collections, wizards and
the app shell — header-only under `include/morph/forms/engine/`, carrying every behaviour the QML renderer's
QuickTest suite pins as headless Catch2 tests, plus `math::parseDecimal` and `app::ViewScreen`.

**Architecture:** `FormModel::fromSchema` reads the schema JSON `schemaJson<A>()` emits (glaze's u64 DOM, so
integers keep every digit) into an immutable field model with parsed `x-rules`. `FormSession` holds a
`reactive::Signal` per draft and derives encodings, errors, three-valued rule verdicts and readiness as
`Computed`s; submission is a `reactive::Mutation` over a `Submitter`, Choice options a `reactive::Query` per Choice
over a `ChoiceFetcher`. `formView` turns a session into `ui::Node`s bound to those signals, so one form renders on
every frontend; collections, wizards and the app shell compose sessions and views the same way.

**Tech Stack:** C++23, header-only; glaze (`glz::generic_u64`); `morph::reactive` (Part 1); `morph::ui` and
`ui::testing::RecordingBackend` (Part 2); `morph::math::Rational`, `morph::render` locale formatting; the bridge
(`BridgeHandler`, `ActionExecuteRegistry`); Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-10-04-forms-engine-design.md` (spec 2), §1–§10 and §12; §11 (retiring the QML
renderer) is Part 10's — this part removes nothing.

This is **Part 5 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first: it fixes the branch, the commit layout and the
`wip(...)` commit convention every task below follows, and this part ends by squashing its `wip(forms)` commits
into one, directly after the `qt_quick:` commit. It consumes Part 1 (`morph::reactive`) and Part 2 (`morph::ui`,
`RecordingBackend`) exactly as the interface contract
(`docs/superpowers/plans/2026-10-04-declarative-ui-tui-interfaces.md`) states them.

## Global Constraints

- Part 1's Global Constraints apply unchanged (SPDX line, `#pragma once`, naming, comments, Doxygen on every
  symbol including `detail`, `-Weverything -Werror`, clang-tidy, sign-off).
- The engine is header-only in the base `morph` target's `FILE_SET HEADERS` (`engine/detail/text.hpp` in
  `morph_detail_headers`); no new CMake option, no new dependency, no compiled source.
- `form_session.hpp` and everything it includes stay free of `morph/ui`: a controller that only needs
  `FormSession` must not pull rendering in (spec §9). Only `overrides.hpp`, `form_view.hpp`, `collection.hpp`,
  `wizard.hpp` and `app_shell.hpp` include `morph/ui`.
- No value passes through a `double`: integers compare and encode as digits, decimals and quantities as
  `Rational`s or by digit cross-multiplication. Floating `std::to_chars` appears only where a JSON double is
  printed in fixed notation (`detail::engine::numberText`).
- No `std::ranges::contains` (missing from the Emscripten libc++ the wasm job uses); use
  `detail::engine::contains`.
- No unchecked `operator[]` in a header (`cppcoreguidelines-pro-bounds-avoid-unchecked-container-access`):
  containers use `.at()`, `try_emplace` or iteration, the glaze DOM `get_object().insert_or_assign`, and a
  `std::span` (no `at()` before C++26) `detail::engine::elementAt` (Task 4).
- Every engine test carries the tag `[forms-engine]`; test files are `tests/test_forms_engine_*.cpp`.
- The rule and instance-bounds corpora move to `tests/data/` (Task 9) and both the Catch2 and the QML suites
  read them there; the copies under `src/qt/forms/tests/data/` stay, unread, until Part 10 removes the
  directory.
- The `RecordingBackend` vocabulary the view tests rely on — widget kinds (`"TextInput"`, `"Button"`, …) and
  recorded props (`"text"`, `"label"`, `"enabled"`, …) — is Part 2's (`ui/testing/recording_backend.hpp`, its Task
  2); the kinds are named once, in `tests/forms_engine_view_support.hpp`.
- Lifetime rule, stated in every session class's brief: a session outlives every view mounted on it; tests
  destroy `ui::Mounted` before the session.
- Code blocks are not hand-wrapped to the 119-column limit everywhere: the pre-commit `clang-format` hook
  formats each file on `git commit`; when it rewrites one, `git add` it and commit again.
- Nothing under `src/qt/forms` changes behaviour; only its CMake data-path definitions and comments move with the
  corpora.

## Review Focus

1. **A prefill that leaves the form ready** must not submit, and the next user edit must (Task 6 test
   "FormSession: prefilling a complete payload never submits; the next user edit does").
2. **A Choice reply for a superseded parent value** must be dropped, and a selection prefilled before its options
   arrive must survive the refetch (Task 8 test "choice: a reply for a superseded parent value is dropped, and a
   prefilled selection survives its refetch").
3. **An id beyond 2^53** must stay exact through encode, body, a rule comparison and prefill (Task 6 test
   "FormSession: an id beyond 2^53 is exact through encode, body, a rule and prefill").
4. **A rule kind the engine does not know** must never block readiness, while a False conjunct beside it still
   does (Task 3 test "RuleExpr: an unknown kind never blocks, and a false conjunct still does").
5. **A collection row removed while its view is mounted** must be torn down after its bindings, not before
   (Task 12 test "formView: a row removed while its view is mounted is torn down after its bindings").

---

## File Structure

| File | Responsibility |
|---|---|
| `include/morph/util/rational.hpp` | `math::parseDecimal` — locale entry text to an exact `Rational` |
| `include/morph/forms/engine/detail/text.hpp` | JSON quoting, exact decimal-text arithmetic, wall-clock/instant text (detail) |
| `include/morph/forms/engine/rules.hpp` | `SchemaError`, `Tri`, `Scalar`, `RuleValue`, `RuleExpr`, gating and presentation verdicts |
| `include/morph/forms/engine/field_model.hpp` | `FieldKind`, `SubmitMode`, `FieldSpec`, `FieldGroupSpec`, `FormModel::fromSchema`, `FormModel::forAction<A>` |
| `include/morph/forms/engine/values.hpp` | `FieldError`, `WireValue`, `ValueContext`, `encodeScalar`, `decodeScalar`, `convertDraft`, `parseOptions` |
| `include/morph/forms/engine/form_session.hpp` | `Submitter`, `ChoiceFetcher`, `FormSessionOptions`, `FieldState`, `RowState`, `FormSession` |
| `include/morph/core/bridge.hpp` | `ActionExecuteRegistry::modelsServing`, `makeHandler` |
| `include/morph/forms/engine/bridge_submitter.hpp` | `bridgeSubmitter`, `bridgeChoiceFetcher` |
| `include/morph/forms/engine/handler_submitter.hpp` | `handlerSubmitter`, `handlerChoiceFetcher` — forms submitting through the application's own `BridgeHandler`s |
| `include/morph/forms/engine/typed_form.hpp` | `Form<A, M, S>` |
| `include/morph/forms/engine/overrides.hpp` | `FieldView`, `Overrides` |
| `include/morph/forms/engine/form_view.hpp` | `FormViewOptions`, `formView` |
| `include/morph/forms/engine/collection.hpp` | `CollectionModel`, `CollectionSession`, `CollectionViewOptions`, `collectionView` |
| `include/morph/forms/engine/wizard.hpp` | `WizardModel`, `WizardSession`, `wizardView` |
| `include/morph/forms/app.hpp` | `app::ViewScreen<Id, View>` |
| `include/morph/forms/engine/app_shell.hpp` | `AppShellModel`, `AppShellSources`, `AppShellSession`, `appShellView` |
| `tests/forms_engine_support.hpp` | `FakeServer` (settleable submitter/fetcher) and the session `Harness` |
| `tests/forms_engine_view_support.hpp` | The node outline and the `RecordingBackend` vocabulary view tests use |
| `tests/test_rational_parse_decimal.cpp`, `tests/test_render_locale_format.cpp` | `parseDecimal` |
| `tests/test_forms_engine_{text,rules,model,values,session,nested,choice}.cpp` | Model, rules, values, session |
| `tests/test_forms_engine_{bridge,handler_submitter,typed,view,collection,wizard,app_shell}.cpp` | Submitters, typed facade, rendering, composites |
| `tests/data/rule_corpus.json`, `tests/data/instance_bounds.json` | The shared corpora, now read from one place |
| `tests/test_forms_{rule_agreement,instance_constraints,conformance_corpus}.cpp` | The corpora driven through the engine |
| `docs/spec/forms/engine.md` | The authoritative engine spec |
| `docs/spec/forms/{forms,views,workflows_navigation,widget_hints}.md`, `docs/spec/util/rational.md`, `docs/spec/core/bridge.md` | Engine sections added beside the existing ones |
| `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/qt/forms/CMakeLists.txt` | Header, test and corpus-path registration |
| `docs/spec/README.md`, `docs/ARCHITECTURE.md`, `CHANGELOG.md`, `docs/spec/pinned_facts.toml` | Maps, changelog, the pinned nesting depth |

## Build and test commands (used by every task)

```bash
cmake -S . -B build/reactive -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF   # once
cmake --build build/reactive --target morph_tests
./build/reactive/tests/morph_tests "[forms-engine]"
```

Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING, "Warnings
are errors"). The QML suite, which Task 9 points at the moved corpora, builds in `build/all` (master plan, "Build
directories"; it still configures `MORPH_BUILD_FORMS_QML=ON`): `cmake --build build/all && ctest --test-dir
build/all -R forms --output-on-failure`.

## QuickTest parity map

Spec 2 §10: every claim the 35 `src/qt/forms/tests/tst_*.qml` files make is carried by a Catch2 test of this part
(`tst_main.cpp` is the QuickTest entry point, not a test file). Each file is followed by the test file(s) and the
exact TEST_CASE names that carry it; Task 17 Step 2 checks that every name below exists. What is deliberately
not carried is listed in the next section.

- `tst_collectionview.qml` → `test_forms_engine_collection.cpp`
  - `CollectionSession: the list loads on construction; cells are exact`
  - `CollectionSession: opening a row prefills only its bound fields and fires nothing`
  - `CollectionSession: an edit after opening fires, closes the editor and reloads the list`
  - `CollectionSession: a confirmed row action fires its bound body and reloads; a cancelled one fires nothing`
  - `CollectionSession: a collection action fires with an empty body`
  - `CollectionSession: nothing typed into one row's editor carries over to the next opening`
  - `collectionView: a table of the visible columns, the actions, the editor and confirm dialogs`
- `tst_conformance.qml` → `test_forms_conformance_corpus.cpp`, `test_forms_engine_model.cpp`
  - `Conformance corpus through the engine: x-order and the required gate`
  - `Conformance corpus through the engine: exact Quantity payloads and unit switching`
  - `Conformance corpus through the engine: Choice, Timestamp and the shared $def`
  - `FormModel: key order is irrelevant, x-order is the layout, required and layout read either way`
- `tst_conformance_negative.qml` → `test_forms_conformance_corpus.cpp`, `test_forms_engine_session.cpp`
  - `Conformance corpus through the engine: x-order and the required gate`
  - `Conformance corpus through the engine: exact Quantity payloads and unit switching`
  - `FormSession: readiness gates on required fields, precision and date syntax; the body is byte-exact`
- `tst_conformance_accessibility.qml` → `test_forms_engine_view.cpp`
  - `formView: labels fall back to the wire key, follow x-order and come from translations`
  - `formView: explicit mode adds a Submit button bound to readiness; the cell shows help and a live *`
- `tst_dynamicform.qml` → `test_forms_engine_model.cpp`, `test_forms_engine_text.cpp`,
  `test_forms_engine_values.cpp`, `test_forms_engine_session.cpp`, `test_forms_engine_choice.cpp`,
  `test_forms_engine_view.cpp`
  - `FormModel: kinds, units, the Choice descriptor and flags come from the schema`
  - `FormModel: widget hints become Multiline, Slider and a radio Choice`
  - `FormModel: x-layout buckets fields by x-section in x-order, with a trailing implicit group`
  - `FormModel: a dependent Choice names its parents`
  - `engine text: digit multiplication is exact`
  - `engine text: compareDecimal orders canonical decimals at any magnitude`
  - `values: unit conversion is exact and rounds half away from zero`
  - `values: a Quantity keeps the unreduced scaled digits the QML renderer sent`
  - `values: option rows are the reply array or its first array member, ids exact`
  - `FormSession: readiness gates on required fields, precision and date syntax; the body is byte-exact`
  - `FormSession: reset clears every draft and does not submit a form that stays ready`
  - `FormSession: field() resolves paths and refuses unknown ones`
  - `choice: a dependent Choice refetches with its parents' literals when a parent changes`
  - `formView: sections, tab runs, accordions and the implicit group, each on its grid`
  - `formView: switching tabs keeps what was typed and submits nothing`
- `tst_DynamicFormArrayField.qml` → `test_forms_engine_session.cpp`, `test_forms_engine_values.cpp`,
  `test_forms_engine_view.cpp`
  - `FormSession: an array field encodes a JSON array; blank optional is omitted, a required one gates`
  - `values: booleans, enums, choices, dates and arrays encode to their wire shapes`
  - `formView: each kind draws its control`
- `tst_DynamicFormBlankAs.qml` → `test_forms_engine_session.cpp`
  - `FormSession: x-blankAs empty submits "" once the field was filled, and only then`
  - `FormSession: a stored payload round-trips, absent members start blank, the unit resets`
- `tst_DynamicFormBooleanAndAnyOf.qml` → `test_forms_engine_session.cpp`, `test_forms_engine_view.cpp`
  - `FormSession: booleans are bare literals; a required one answers false, an optional one is omitted`
  - `FormSession: an integer through anyOf keeps every digit and refuses text`
  - `formView: each kind draws its control`
- `tst_DynamicFormChoicelessController.qml` → `test_forms_engine_choice.cpp`
  - `choice: a form with no Choice needs no fetcher; one with a Choice gets its options`
- `tst_DynamicFormChrome.qml` → `test_forms_engine_view.cpp`
  - `Overrides: an override receives the field and drives the session like the built-in control`
  - `formView: explicit mode adds a Submit button bound to readiness; the cell shows help and a live *`
  - `formView: the mounted Submit button follows readiness and submits on click`
- `tst_DynamicFormDisplayUnit.qml` → `test_forms_engine_model.cpp`, `test_forms_engine_values.cpp`,
  `test_forms_engine_view.cpp`
  - `FormModel: a plain number reads its display unit and decimals; a declared precision wins`
  - `values: placeholders follow the kind and the declared precision`
  - `values: a plain number is a JSON number of the digits typed, gated by its bounds`
  - `formView: each kind draws its control`
  - `Overrides: field beats widget beats unit beats kind; a miss draws the built-in control`
- `tst_DynamicFormEnumChoice.qml` → `test_forms_engine_model.cpp`, `test_forms_engine_session.cpp`,
  `test_forms_engine_view.cpp`
  - `FormModel: a closed set is an Enum in every spelling the emitter or a hand writes`
  - `FormSession: an enum starts unselected, encodes the chosen literal and refuses anything else`
  - `formView: each kind draws its control`
- `tst_DynamicFormExactBounds.qml` → `test_forms_engine_values.cpp`, `test_forms_engine_session.cpp`
  - `values: an integer keeps every digit and prefers the exact bound`
  - `FormSession: an integer through anyOf keeps every digit and refuses text`
- `tst_DynamicFormFieldBounds.qml` → `test_forms_engine_values.cpp`
  - `values: Quantity bounds, instance bounds and multipleOf are checked exactly in the canonical unit`
  - `values: an integer keeps every digit and prefers the exact bound`
- `tst_DynamicFormGridColumns.qml` → `test_forms_engine_view.cpp`
  - `formView: a host grid widens every group's grid; spans clamp to it`
  - `formView: sections, tab runs, accordions and the implicit group, each on its grid`
- `tst_DynamicFormInstanceBounds.qml` → `test_forms_instance_constraints.cpp`, `test_forms_engine_model.cpp`
  - `Every corpus row's engine verdict is the one the corpus records`
  - `The decorated schema drives the engine's entry precision`
  - `FormModel: an instance-decorated schema reads its precision and exact range`
- `tst_DynamicFormNestedAggregate.qml` → `test_forms_engine_model.cpp`, `test_forms_engine_nested.cpp`
  - `FormModel: nested objects and collections are described recursively, and a cycle stops`
  - `nested: a self-referential type stops where it repeats, and a value loaded there is unrepresentable`
  - `nested: an acyclic aggregate is drawn and encoded; an optional one may be left out`
- `tst_DynamicFormObjectArraySlot.qml` → `test_forms_engine_nested.cpp`, `test_forms_engine_view.cpp`
  - `nested: collection rows encode with each member's encoder; an empty required collection is []`
  - `nested: loaded cells read as their text; a reset empties the rows`
  - `nested: a member that does not encode, or a blank required member, keeps the form unready`
  - `formView: an object is a panel of member cells; a collection adds and removes rows`
  - `formView: a row removed while its view is mounted is torn down after its bindings`
- `tst_DynamicFormObjectSlot.qml` → `test_forms_engine_nested.cpp`, `test_forms_engine_view.cpp`
  - `nested: an object encodes member by member with each member's own encoder`
  - `nested: two levels deep, blank optional leaves and objects are left out`
  - `nested: members read in the display locale`
  - `nested: a prefill decodes into the members and round-trips`
  - `nested: rows inside an object reuse the row encoder`
  - `nested: a required boolean member of a filled object answers false`
  - `nested: a member that does not encode, or a blank required member, keeps the form unready`
  - `formView: an object is a panel of member cells; a collection adds and removes rows`
- `tst_DynamicFormPlainNumber.qml` → `test_forms_engine_values.cpp`, `test_forms_engine_model.cpp`
  - `values: a plain number is a JSON number of the digits typed, gated by its bounds`
  - `values: a Quantity keeps the unreduced scaled digits the QML renderer sent`
  - `FormModel: a plain number reads its display unit and decimals; a declared precision wins`
- `tst_DynamicFormPrefill.qml` → `test_forms_engine_session.cpp`, `test_forms_engine_values.cpp`,
  `test_forms_engine_choice.cpp`, `test_forms_engine_view.cpp`
  - `FormSession: prefilling a complete payload never submits; the next user edit does`
  - `FormSession: a stored payload round-trips, absent members start blank, the unit resets`
  - `values: decoding is the inverse of encoding, per kind`
  - `choice: a prefilled selection is shown once its options arrive`
  - `formView: mounted controls drive the session`
- `tst_DynamicFormReactive.qml` → `test_forms_engine_session.cpp`, `test_forms_engine_choice.cpp`
  - `FormSession: automatic mode submits once when the body becomes valid, and again only when it changes`
  - `choice: only an independent Choice fetches on construction, with an empty body`
  - `choice: a dependent Choice refetches with its parents' literals when a parent changes`
  - `choice: a selection the new options no longer back is cleared, without submitting`
- `tst_DynamicFormReseedSubmit.qml` → `test_forms_engine_view.cpp`, `test_forms_engine_session.cpp`
  - `formView: switching tabs keeps what was typed and submits nothing`
  - `FormSession: reset clears every draft and does not submit a form that stays ready`
- `tst_DynamicFormRuleAgreement.qml` → `test_forms_rule_agreement.cpp`, `test_forms_engine_rules.cpp`
  - `the forms engine reaches the compiled verdicts on the emitted schema`
  - `RuleExpr: equals compares booleans as booleans and big integers on digits`
- `tst_DynamicFormRuleCorpus.qml` → `test_forms_engine_rules.cpp`
  - `rule corpus: the engine reads every corpus schema`
  - `rule corpus: every row's engine verdicts are the ones the corpus records`
  - `rule corpus: the engine and allRulesSatisfied agree on every row`
- `tst_DynamicFormRules.qml` → `test_forms_engine_session.cpp`, `test_forms_engine_rules.cpp`,
  `test_forms_engine_view.cpp`
  - `FormSession: x-rules gate and present live`
  - `FormSession: compound when-clauses require, release and negate`
  - `RuleExpr: membership counts definite operands and is Unknown only when the count is open`
  - `formView: visibility and read-only follow the rules on the mounted cells`
- `tst_DynamicFormSchemaAsVariant.qml` → `test_forms_engine_model.cpp`, `test_forms_engine_session.cpp`
  - `FormModel: key order is irrelevant, x-order is the layout, required and layout read either way`
  - `FormSession: an integer through anyOf keeps every digit and refuses text`
- `tst_DynamicFormSubmitMode.qml` → `test_forms_engine_session.cpp`, `test_forms_engine_view.cpp`
  - `FormSession: explicit mode submits only on submit(), and only while ready`
  - `formView: explicit mode adds a Submit button bound to readiness; the cell shows help and a live *`
  - `formView: the mounted Submit button follows readiness and submits on click`
- `tst_DynamicFormUnknownRuleKind.qml` → `test_forms_engine_rules.cpp`
  - `RuleExpr: an unknown kind never blocks, and a false conjunct still does`
  - `RuleExpr: and, or and not are Kleene over Unknown`
  - `RuleExpr: presentation verdicts combine every rule naming the field`
- `tst_i18n.qml` → `test_forms_engine_session.cpp`, `test_forms_engine_view.cpp`, `test_forms_engine_values.cpp`,
  `test_rational_parse_decimal.cpp`, `test_render_locale_format.cpp`, `test_forms_engine_text.cpp`
  - `FormSession: labels resolve through the translations; numbers read in the session locale`
  - `formView: labels fall back to the wire key, follow x-order and come from translations`
  - `values: an error message resolves through the translation provider`
  - `parseDecimal: locale grouping, separators and signs are read through the control edge`
  - `parseDecimal agrees with normalizeLocaleNumber on the multi-unit corpus`
  - `parseDecimal reads every measured digit set`
  - `engine text: wall-clock and instant conversion`
- `tst_LargeIdPrecision.qml` → `test_forms_engine_collection.cpp`, `test_forms_engine_choice.cpp`,
  `test_forms_engine_session.cpp`
  - `CollectionSession: ids beyond 2^53 stay distinct in keys, cells and bodies`
  - `choice: option values and labels keep ids beyond 2^53 distinct`
  - `FormSession: an id beyond 2^53 is exact through encode, body, a rule and prefill`
- `tst_slot_registry.qml` → `test_forms_engine_view.cpp`
  - `Overrides: field beats widget beats unit beats kind; a miss draws the built-in control`
  - `Overrides: an override receives the field and drives the session like the built-in control`
- `tst_SlotRegistryByKind.qml` → `test_forms_engine_model.cpp`, `test_forms_engine_view.cpp`
  - `FormModel: every renderer kind is named`
  - `Overrides: field beats widget beats unit beats kind; a miss draws the built-in control`
  - `Overrides: an override receives the field and drives the session like the built-in control`
- `tst_ViewChrome.qml` → `test_forms_engine_collection.cpp`, `test_forms_engine_wizard.cpp`
  - `collectionView: a table of the visible columns, the actions, the editor and confirm dialogs`
  - `collectionView: master-detail puts the editor beside the table`
  - `wizardView: the title with the position, the step's form, and Back/Next bound to the session`
- `tst_wizardview.qml` → `test_forms_engine_wizard.cpp`
  - `WizardSession: Next waits for the step's successful submission; Back always returns`
  - `WizardSession: a failed submission leaves the step not done`
  - `WizardSession: entering a step applies its prefill as JSON values, the reply winning`

## What the parity map does not carry

These QML claims have no engine test, each for a stated reason:

- **Qt's own locale facts** (`tst_i18n.qml`: the `qtReports…`/`qtExposes…` cases and "no locale separator needs
  more than one code unit") measure `Qt.locale`, not the engine. The measured locales they produced are the
  corpus `test_render_locale_format.cpp` already pins, and `parseDecimal` is run over it.
- **QML-only mechanics**: the `Connections` warning of `tst_DynamicFormChoicelessController.qml`, "an assigned
  schema is not a plain JS object" (`tst_DynamicFormSchemaAsVariant.qml`), the `previewLine` (the engine's preview
  is `body()`), and the QML renderer's internal indexes (`dependentsIsReverseOfDependsOn`,
  `fieldByNameIndexesByName`; `FormModel::find` and `FieldSpec::choice.dependsOn` replace them).
- **The chrome and slot registries** (`tst_DynamicFormChrome.qml`, `tst_ViewChrome.qml`, the "slot registered after
  the form is built" and "byField is scoped to its own action" cases): spec 2 §7 replaces registries with
  composition — `Overrides` belong to one `formView` call, and chrome is built from the session's signals. What
  the chrome tests proved about the form (an overridden control still drives it, Submit goes through its own
  gate) is carried by the Overrides and Submit tests above.
- **Focus order and keyboard operability** (`tst_conformance_accessibility.qml`) are properties of a frontend's
  widgets (Parts 3 and 4), not of the node tree the engine emits; what the engine owns — cells in `x-order`, a
  label falling back to the wire key, the required marker — the label and Submit tests above carry.
- **The broken renderers of `tst_conformance_negative.qml`** proved the conformance harness can fail. Here every
  task's Step 4 mutation check does that job: it breaks the engine on purpose and names the test that must fail
  (Task 9 raises `atLeastOneOf`'s threshold, and the corpus cases fail).

## Contract additions and spec deviations

Every public name beyond the Part 5 contract, and every place this plan departs from spec 2, in one list (each is
also in its task's Interfaces block and in `docs/spec/forms/engine.md`):

- **Added to `FormSessionOptions`:** `accepts` — an extra readiness predicate over the body; `Form<A>` joins
  `validate()` through it.
- **Added to `FormSession`:** `runtime()`, `fields()`, `assign(path, json)`, `groupCollapsed(i)`,
  `tabSelection(i)`, `groupTitle(i)`, `submitMode()`, `options()`. **Added to `FormModel`:** `find(name)`.
  **Added types:** `RowState`, `FieldError`, `WireValue`, `ValueContext`, `RuleValue`, `Scalar`, `DecimalText`,
  `UnitOption`, `ChoiceOption`, `ChoiceSpec`, `kMaxNestingDepth`; `FormViewOptions::flatGridColumns`; and the full
  declarations of the contract's forward-declared classes (Tasks 4, 6–8, 12–15).
- **Added to `bridge::ActionExecuteRegistry`** (`include/morph/core/bridge.hpp`): `HandlerFactory`,
  `modelsServing(actionId)`, `makeHandler(modelId, Bridge&, IExecutor*)`. Spec 2 §6 routes through "the
  bridge's JSON entry"; the bridge has no action-type-keyed JSON entry (`executeJson` is a `BridgeHandler`
  member), so `bridgeSubmitter` routes the action type to the one model registering it (Task 10).
- **Composite constructors take a `SchemaLookup`** (action type → schema JSON): `CollectionSession`,
  `WizardSession`; `AppShellSession` takes `AppShellSources{actions, wizards, views}` and builds each screen's
  session itself. `WizardModel::fromSchema(wizardId, wizardJson)` and `AppShellModel::fromSchema(appId, appJson)`
  take the registered id (the i18n key stem); `AppScreen::kind` is the enum `ScreenKind`.
- **Gating is "not False"**, not spec 2 §3's "every gating rule True": Unknown arises only from a rule kind the
  engine does not know (an Invalid operand already blocks through its field's error), and blocking on it would
  lock an older client out of a newer server's forms — the QML renderer's `tst_DynamicFormUnknownRuleKind.qml`
  behaviour, kept.
- **Schema keywords:** the emitter writes `x-optionValue` / `x-optionLabel` (spec 2 §2 says `x-optionsValue` /
  `x-optionsLabel`); the engine reads what is emitted.
- **QML parity kept where spec 2 is silent:** a form ready as constructed submits once in automatic mode; Quantity
  literals are unreduced (`{"num":2650500,"den":1000,"dp":3}`).
- **QML behaviour changed** (listed in `engine.md`, "Differences from the QML renderer"): a required
  ObjectArray with no rows encodes `[]`; Array items encode by item type rather than always as strings; Quantity
  bounds hold whatever the entry unit; a required checkbox's blank draft encodes `false`; every
  `visibleWhen`/`requiredWhen` naming a field counts, not only the first.

---

### Task 1: `morph::math::parseDecimal`

**Files:**
- Modify: `include/morph/util/rational.hpp` — includes (add `<algorithm>`, `<optional>`, `<morph/render/locale_format.hpp>`
  after `<morph/core/payload_shape_tag.hpp>`), and the new function directly before the closing
  `}  // namespace morph::math` that follows `Rational::toDouble`'s out-of-line definition
- Modify: `tests/CMakeLists.txt` — add `test_rational_parse_decimal.cpp` after `test_rational.cpp`
- Modify: `tests/test_render_locale_format.cpp` — append the locale-corpus rows driven through `parseDecimal`
- Modify: `docs/spec/util/rational.md` — a "Parsing decimal entry text" section before "## API reference"
- Test: `tests/test_rational_parse_decimal.cpp`

**Interfaces:**
- Consumes: `render::NumericLocale`, `render::normalizeLocaleNumber` (`include/morph/render/locale_format.hpp:97`,
  `:585`); `Rational`, `Numerator`, `Denominator`, `DecimalPlaces`, `kMaxDecimalPlaces`, `detail::powerOfTen`
  (`include/morph/util/rational.hpp:375`, `:140`, `:154`, `:121`, `:167`, `:1331`).
- Produces (the interface contract's Part 5 entry, exactly):
  `morph::math::parseDecimal(std::string_view text, render::NumericLocale const& locale,
  std::optional<std::uint32_t> maxDecimalPlaces = std::nullopt) -> std::optional<Rational>`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_rational_parse_decimal.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/render/locale_format.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string_view>

namespace {

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::parseDecimal;
using morph::math::Rational;
using morph::render::NumericLocale;

constexpr NumericLocale kCLocale{};
constexpr NumericLocale kGerman{.decimalSeparator = ",", .groupSeparator = "."};

[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{0}};
}

}  // namespace

TEST_CASE("parseDecimal: plain text is the exact value it spells", "[rational][parse-decimal]") {
    auto const value = parseDecimal("2650.5", kCLocale);
    REQUIRE(value.has_value());
    CHECK(*value == exact(26505, 10));
    CHECK(value->decimalPlaces.value == 1);
    CHECK(parseDecimal("-0.25", kCLocale) == exact(-1, 4));
    CHECK(parseDecimal("007.50", kCLocale) == exact(75, 10));
    CHECK(parseDecimal("0", kCLocale) == exact(0, 1));
}

TEST_CASE("parseDecimal: locale grouping, separators and signs are read through the control edge",
          "[rational][parse-decimal]") {
    CHECK(parseDecimal("1.050,25", kGerman) == exact(105025, 100));
    CHECK(parseDecimal("+1.050,25", kGerman) == exact(105025, 100));
    CHECK(parseDecimal("1.5", kGerman) == std::nullopt);  // a group that is too short, not 15
    constexpr std::string_view kMinus = "\xE2\x88\x92";   // U+2212
    NumericLocale const basque{.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kMinus};
    CHECK(parseDecimal("\xE2\x88\x92"
                       "5",
                       basque) == exact(-5, 1));
    NumericLocale const arabic{.zeroDigit = "\xD9\xA0"};  // U+0660
    CHECK(parseDecimal("\xD9\xA5", arabic) == exact(5, 1));
}

TEST_CASE("parseDecimal: a declared precision rejects more places and tags the value", "[rational][parse-decimal]") {
    CHECK(parseDecimal("12.55", kCLocale, 1U) == std::nullopt);
    auto const typed = parseDecimal("12.5", kCLocale, 1U);
    REQUIRE(typed.has_value());
    CHECK(*typed == exact(125, 10));
    CHECK(typed->decimalPlaces.value == 1);
    auto const whole = parseDecimal("12", kCLocale, 3U);
    REQUIRE(whole.has_value());
    CHECK(whole->decimalPlaces.value == 3);
    auto const wide = parseDecimal("1.5", kCLocale, 30U);
    REQUIRE(wide.has_value());
    CHECK(wide->decimalPlaces.value == morph::math::kMaxDecimalPlaces);
}

TEST_CASE("parseDecimal: malformed shapes are refused rather than guessed", "[rational][parse-decimal]") {
    for (std::string_view const text : {"", "-", "5.", ".5", "-.5", "1e5", "abc", "1-2", "3,5", " 5", "5 ", "--1"}) {
        INFO("accepted '" << text << "'");
        CHECK_FALSE(parseDecimal(text, kCLocale).has_value());
    }
}

TEST_CASE("parseDecimal: a value outside the int64 envelope is refused", "[rational][parse-decimal]") {
    CHECK(parseDecimal("9223372036854775807", kCLocale) == exact(9223372036854775807, 1));
    CHECK(parseDecimal("-9223372036854775807", kCLocale) == exact(-9223372036854775807, 1));
    CHECK_FALSE(parseDecimal("9223372036854775808", kCLocale).has_value());
    CHECK_FALSE(parseDecimal("92233720368547758.08", kCLocale).has_value());
    CHECK_FALSE(parseDecimal("0.1234567890123456789", kCLocale).has_value());  // 19 places
    CHECK(parseDecimal("0.123456789012345678", kCLocale).has_value());          // 18 places
}
```

Append to `tests/test_render_locale_format.cpp` (it already defines `kDigitSets`, `kGroup2`, `kDecimal2`,
`kGroup4`, `kDecimal4`, `kArDecimal`, `kArGroup`, `kArZero`, `kArNegative` and `joined` in anonymous namespaces
above; add `#include <morph/util/rational.hpp>` to its includes):

```cpp
// ──── The same corpora, through the parser the forms engine uses ─────────────
//
// parseDecimal is normalizeLocaleNumber plus the canonical-shape check and the
// int64 envelope, so every row above that normalises must parse to the exact
// value of its canonical text, and every row that does not must not parse.

namespace {
[[nodiscard]] std::optional<morph::math::Rational> canonicalValue(std::string_view canonical) {
    return morph::math::parseDecimal(canonical, morph::render::NumericLocale{});
}
}  // namespace

TEST_CASE("parseDecimal agrees with normalizeLocaleNumber on the multi-unit corpus", "[render][locale][parse-decimal]") {
    morph::render::NumericLocale const two{.decimalSeparator = kDecimal2, .groupSeparator = kGroup2};
    morph::render::NumericLocale const four{.decimalSeparator = kDecimal4, .groupSeparator = kGroup4};
    struct Row {
        std::string text;
        morph::render::NumericLocale locale;
    };
    std::vector<Row> const rows{
        {joined({"1", kGroup2, "050", kDecimal2, "25"}), two},
        {joined({"-1", kGroup2, "050", kDecimal2, "25"}), two},
        {joined({"+1", kGroup2, "050", kDecimal2, "25"}), two},
        {joined({"1", kGroup4, "050", kDecimal4, "25"}), four},
        {joined({"1", kGroup2, "5"}), two},
        {joined({"1", kDecimal2, "0", kDecimal2, "5"}), two},
        {joined({kDecimal2, "-5"}), two},
    };
    for (auto const& row : rows) {
        auto const normalised = morph::render::normalizeLocaleNumber(row.text, row.locale);
        auto const parsed = morph::math::parseDecimal(row.text, row.locale);
        INFO("row '" << row.text << "'");
        REQUIRE(parsed.has_value() == normalised.has_value());
        if (normalised.has_value()) {
            CHECK(*parsed == *canonicalValue(*normalised));
        }
    }
}

TEST_CASE("parseDecimal reads every measured digit set", "[render][locale][parse-decimal]") {
    for (auto const& set : kDigitSets) {
        INFO(set.name);
        auto const five = morph::math::parseDecimal(set.five, {.zeroDigit = set.zero});
        REQUIRE(five.has_value());
        CHECK(*five == *canonicalValue("5"));
        morph::render::NumericLocale const locale{.decimalSeparator = kArDecimal,
                                                  .groupSeparator = kArGroup,
                                                  .negativeSign = kArNegative,
                                                  .zeroDigit = set.zero};
        auto const display = morph::render::formatCanonicalNumber("-1050.25", locale);
        CHECK(morph::math::parseDecimal(display, locale) == canonicalValue("-1050.25"));
    }
    morph::render::NumericLocale const arabic{.zeroDigit = kArZero};
    CHECK_FALSE(morph::math::parseDecimal("\xD9\xA5"
                                          "5",
                                          arabic)
                    .has_value());  // two digit families in one entry
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: compile error `no member named 'parseDecimal' in namespace 'morph::math'`.

- [ ] **Step 3: Implement**

In `include/morph/util/rational.hpp`, add to the includes `#include <algorithm>`, `#include <optional>` and
`#include <morph/render/locale_format.hpp>` (after `#include <morph/core/payload_shape_tag.hpp>`), and directly
before the `}  // namespace morph::math` that closes the block holding `Rational::toDouble`'s definition:

```cpp
/// @brief Parses locale-formatted decimal entry text into the exact value it spells.
///
/// The text goes through `render::normalizeLocaleNumber` first, so the locale's separators,
/// signs and digits are accepted and its grouping is validated; the canonical text must then
/// be `-?[0-9]+(\.[0-9]+)?` — no exponent, no bare separator, no surrounding space. The value
/// must fit `Rational`: at most `kMaxDecimalPlaces` fraction digits, and the digits read as one
/// integer within `int64_t`. Nothing is rounded: text finer than @p maxDecimalPlaces is refused.
/// @param text             The entry, e.g. `"1.050,25"`.
/// @param locale           The locale facts the entry was typed in.
/// @param maxDecimalPlaces The declared precision, when there is one. The result carries it as
///                         its precision tag (clamped to `kMaxDecimalPlaces`); without one the
///                         tag is the number of fraction digits typed.
/// @return The exact value, or `std::nullopt` for malformed, over-precise or unrepresentable text.
[[nodiscard]] inline std::optional<Rational> parseDecimal(std::string_view text, render::NumericLocale const& locale,
                                                          std::optional<std::uint32_t> maxDecimalPlaces = std::nullopt) {
    std::optional<std::string> const canonical = render::normalizeLocaleNumber(text, locale);
    if (!canonical.has_value()) {
        return std::nullopt;
    }
    std::string_view rest{*canonical};
    bool const negative = rest.starts_with('-');
    if (negative) {
        rest.remove_prefix(1);
    }
    auto const dot = rest.find('.');
    std::string_view const whole = rest.substr(0, dot);
    std::string_view const fraction = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
    auto const allDigits = [](std::string_view part) {
        return !part.empty() && std::ranges::all_of(part, [](char chr) { return chr >= '0' && chr <= '9'; });
    };
    if (!allDigits(whole) || (dot != std::string_view::npos && !allDigits(fraction))) {
        return std::nullopt;
    }
    auto const places = static_cast<std::uint32_t>(fraction.size());
    if (places > kMaxDecimalPlaces || (maxDecimalPlaces.has_value() && places > *maxDecimalPlaces)) {
        return std::nullopt;
    }
    constexpr auto kLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    std::uint64_t magnitude = 0;
    for (std::string_view const part : {whole, fraction}) {
        for (char const chr : part) {
            auto const digit = static_cast<std::uint64_t>(chr - '0');
            if (magnitude > (kLimit - digit) / 10U) {
                return std::nullopt;
            }
            magnitude = (magnitude * 10U) + digit;
        }
    }
    auto const signedMagnitude = static_cast<std::int64_t>(magnitude);
    std::uint32_t const tag = std::min(maxDecimalPlaces.value_or(places), kMaxDecimalPlaces);
    return Rational{Numerator{negative ? -signedMagnitude : signedMagnitude}, Denominator{detail::powerOfTen(places)},
                    DecimalPlaces{tag}};
}
```

In `docs/spec/util/rational.md`, before `## API reference`, add:

```markdown
## Parsing decimal entry text — `parseDecimal`

`parseDecimal(text, locale, maxDecimalPlaces = nullopt) -> std::optional<Rational>` is the one
decimal parser morph ships; the forms engine and any application share it.

1. `render::normalizeLocaleNumber(text, locale)` reads the locale's separators, signs and digit
   set and validates its grouping (`docs/spec/forms/forms.md`, "Locale data formatting").
2. The canonical text must be `-?[0-9]+(\.[0-9]+)?`: `"5."`, `".5"` and `"1e5"` are refused.
3. More fraction digits than `maxDecimalPlaces` are refused, never rounded; more than
   `kMaxDecimalPlaces` are refused whatever was declared.
4. The digits, read as one integer, must fit `int64_t`; the result is that integer over
   `10^places`, canonicalised, tagged with `maxDecimalPlaces` (clamped to `kMaxDecimalPlaces`) or,
   without one, with the number of places typed.

Rejection is `std::nullopt` in every case: the caller is an entry edge, and "this text is not a
number this field can hold" is an ordinary answer there, not an error.
```

and add a row to the "Conversion helpers" table — or, if that section is prose, one line at its end:
`` `parseDecimal` (above) is the inverse direction: entry text to an exact value. ``

In `tests/CMakeLists.txt`, add `test_rational_parse_decimal.cpp` after `test_rational.cpp`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[parse-decimal]"`
Expected: PASS, 7 test cases.

Mutation check: delete `|| (maxDecimalPlaces.has_value() && places > *maxDecimalPlaces)` from the guard. Expected:
FAIL in "a declared precision rejects more places and tags the value" (`parseDecimal("12.55", kCLocale, 1U)` is engaged).
Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/util/rational.hpp tests/test_rational_parse_decimal.cpp tests/test_render_locale_format.cpp \
        tests/CMakeLists.txt docs/spec/util/rational.md
git commit -m "wip(forms): parseDecimal, the exact decimal entry parser

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: Exact text helpers (`forms/engine/detail/text.hpp`)

The engine never routes a value through a `double`. These helpers are the arithmetic that makes that possible:
JSON quoting that matches the body bytes the QML renderer produced, a canonical-decimal comparison that works at any
magnitude, digit multiplication for comparing a `Rational` against a decimal bound, decimal rendering of a
`Rational`, and ISO-8601 wall-clock conversion for the display zone.

**Files:**
- Create: `include/morph/forms/engine/detail/text.hpp`
- Modify: `CMakeLists.txt` — in the `FILE_SET morph_detail_headers` block, after
  `include/morph/forms/detail/schema_name.hpp`, add `include/morph/forms/engine/detail/text.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_text.cpp` after `test_forms_conformance_corpus.cpp`
- Test: `tests/test_forms_engine_text.cpp`

**Interfaces:**
- Consumes: `glz::generic_u64`, `glz::read_json`, `glz::write_json`; `forms::detail::findMember`
  (`include/morph/forms/forms.hpp:2482`); `math::Rational`, `math::roundToDecimalPlaces`, `math::RoundingMode`,
  `math::detail::powerOfTen`, `math::detail::absU64` (`include/morph/util/rational.hpp:1029`, `:190`, `:1331`,
  `:368`).
- Produces, in `morph::forms::detail::engine` (later tasks use exactly these):
  `Json` (= `glz::generic_u64`); `trim(std::string_view) -> std::string_view`; `quote(std::string_view) ->
  std::string`; `parseJson(std::string_view) -> std::optional<Json>`; `toJson(Json const&) -> std::string`;
  `emptyObject() -> Json`; `member(Json const&, std::string_view) -> Json const*`;
  `stringAt(Json const&, std::string_view) -> std::string const*`; `numberText(Json const&) ->
  std::optional<std::string>`; `displayText(Json const&) -> std::string`; `isCanonicalDecimal(std::string_view)`,
  `isCanonicalInteger(std::string_view)`; `stripLeadingZeros(std::string_view) -> std::string`;
  `compareDecimal(std::string_view, std::string_view) -> std::strong_ordering`; `multiplyDigits(std::string_view,
  std::string_view) -> std::string`; `compareRationalToDecimal(math::Rational const&, std::string_view) ->
  std::strong_ordering`; `decimalText(math::Rational const&, std::uint32_t places) -> std::string`;
  `parseWallClock(std::string_view) -> std::optional<std::chrono::sys_seconds>`; `parseInstant(std::string_view)
  -> std::optional<std::chrono::sys_seconds>`; `formatWallClock(std::chrono::sys_seconds) -> std::string`;
  `formatUtc(std::chrono::sys_seconds) -> std::string`; `isIsoDate(std::string_view) -> bool`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_text.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <compare>
#include <morph/forms/engine/detail/text.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>

namespace engine = morph::forms::detail::engine;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

TEST_CASE("engine text: quote matches JSON.stringify byte for byte", "[forms-engine][text]") {
    CHECK(engine::quote("plain") == R"("plain")");
    CHECK(engine::quote("a\"b\\c") == R"("a\"b\\c")");
    CHECK(engine::quote("tab\tnl\ncr\r") == R"("tab\tnl\ncr\r")");
    CHECK(engine::quote(std::string{"\x01"}) == R"("\u0001")");
    CHECK(engine::quote("kg/m\xC2\xB3") == "\"kg/m\xC2\xB3\"");  // non-ASCII passes through
}

TEST_CASE("engine text: numbers keep every digit, and a double is written in fixed notation",
          "[forms-engine][text]") {
    auto const dom = engine::parseJson(R"({"big":18446744073709551615,"neg":-9223372036854775808,"small":1e-7,)"
                                       R"("max":1.7976931348623157e+308,"half":0.5,"text":"x"})");
    REQUIRE(dom.has_value());
    CHECK(engine::numberText(*engine::member(*dom, "big")) == "18446744073709551615");
    CHECK(engine::numberText(*engine::member(*dom, "neg")) == "-9223372036854775808");
    CHECK(engine::numberText(*engine::member(*dom, "small")) == "0.0000001");
    CHECK(engine::numberText(*engine::member(*dom, "half")) == "0.5");
    CHECK(engine::numberText(*engine::member(*dom, "max"))->size() == 309);
    CHECK_FALSE(engine::numberText(*engine::member(*dom, "text")).has_value());
    CHECK(engine::toJson(*engine::member(*dom, "big")) == "18446744073709551615");
    CHECK(engine::displayText(*engine::member(*dom, "text")) == "x");
    CHECK_FALSE(engine::parseJson("not json").has_value());
}

TEST_CASE("engine text: compareDecimal orders canonical decimals at any magnitude", "[forms-engine][text]") {
    using std::strong_ordering;
    CHECK(engine::compareDecimal("9223372036854775808", "9223372036854775807") == strong_ordering::greater);
    CHECK(engine::compareDecimal("-9223372036854775809", "-9223372036854775808") == strong_ordering::less);
    CHECK(engine::compareDecimal("0009223372036854775807", "9223372036854775807") == strong_ordering::equal);
    CHECK(engine::compareDecimal("0.25", "0.5") == strong_ordering::less);
    CHECK(engine::compareDecimal("2.50", "2.5") == strong_ordering::equal);
    CHECK(engine::compareDecimal("-0", "0") == strong_ordering::equal);
    CHECK(engine::compareDecimal("-40.5", "-41") == strong_ordering::greater);
}

TEST_CASE("engine text: digit multiplication is exact", "[forms-engine][text]") {
    CHECK(engine::multiplyDigits("123", "45") == "5535");
    CHECK(engine::multiplyDigits("0", "999") == "0");
    CHECK(engine::multiplyDigits("9223372036854775807", "1000") == "9223372036854775807000");
}

TEST_CASE("engine text: a Rational compares exactly against a decimal", "[forms-engine][text]") {
    Rational const third{Numerator{1}, Denominator{3}, DecimalPlaces{2}};
    CHECK(engine::compareRationalToDecimal(third, "0.3333") == std::strong_ordering::greater);
    CHECK(engine::compareRationalToDecimal(third, "0.3334") == std::strong_ordering::less);
    Rational const big{Numerator{9223372036854775807}, Denominator{1}, DecimalPlaces{0}};
    CHECK(engine::compareRationalToDecimal(big, std::string(309, '9')) == std::strong_ordering::less);
    CHECK(engine::compareRationalToDecimal(-big, "-100000000000000000000") == std::strong_ordering::greater);
}

TEST_CASE("engine text: decimalText rounds half away from zero at the requested places", "[forms-engine][text]") {
    CHECK(engine::decimalText(Rational{Numerator{5}, Denominator{4}, DecimalPlaces{2}}, 2) == "1.25");
    CHECK(engine::decimalText(Rational{Numerator{-1}, Denominator{3}, DecimalPlaces{2}}, 2) == "-0.33");
    CHECK(engine::decimalText(Rational{Numerator{245050}, Denominator{100}, DecimalPlaces{2}}, 2) == "2450.50");
    CHECK(engine::decimalText(Rational{Numerator{15}, Denominator{10}, DecimalPlaces{1}}, 1) == "1.5");
    CHECK(engine::decimalText(Rational{Numerator{-1}, Denominator{1000}, DecimalPlaces{3}}, 2) == "0.00");
    CHECK(engine::decimalText(Rational{Numerator{7}, Denominator{1}, DecimalPlaces{0}}, 0) == "7");
}

TEST_CASE("engine text: wall-clock and instant conversion", "[forms-engine][text]") {
    using namespace std::chrono_literals;
    auto const wall = engine::parseWallClock("2026-07-05T14:30");
    REQUIRE(wall.has_value());
    CHECK(engine::formatUtc(*wall) == "2026-07-05T14:30:00Z");
    CHECK(engine::formatWallClock(*wall + 2h) == "2026-07-05T16:30:00");
    CHECK(engine::parseWallClock("2026-07-05T14:30:15").has_value());
    CHECK_FALSE(engine::parseWallClock("not-a-date").has_value());
    CHECK_FALSE(engine::parseWallClock("2026-02-30T10:00").has_value());
    CHECK_FALSE(engine::parseWallClock("2026-07-05T24:00").has_value());
    auto const zoned = engine::parseInstant("2026-01-01T00:30:00+01:00");
    REQUIRE(zoned.has_value());
    CHECK(engine::formatWallClock(*zoned) == "2025-12-31T23:30:00");
    CHECK(engine::parseInstant("2026-07-20T09:00:00Z").has_value());
    CHECK(engine::parseInstant("2026-07-20T09:00:00.123Z").has_value());
    CHECK(engine::parseInstant("2026-07-20T09:00:00").has_value());
    CHECK(engine::isIsoDate("2026-02-28"));
    CHECK_FALSE(engine::isIsoDate("2026-02-30"));
    CHECK_FALSE(engine::isIsoDate("2026-2-3"));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/detail/text.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/detail/text.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/detail/text.hpp
/// @brief Exact text helpers the forms engine shares: JSON quoting and reading, canonical
///        decimal comparison and digit arithmetic, decimal rendering of a `Rational`, and
///        ISO-8601 wall-clock conversion.
///
/// Specified in `docs/spec/forms/engine.md`.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "../../../util/rational.hpp"
#include "../../forms.hpp"

/// @brief Implementation details of the forms engine (`include/morph/forms/engine/`).
namespace morph::forms::detail::engine {

/// @brief The JSON DOM the engine reads: integers keep every digit, as `uint64_t` or `int64_t`.
using Json = glz::generic_u64;

/// @brief Strips ASCII whitespace from both ends.
/// @param text The text.
/// @return A view of @p text without leading or trailing spaces, tabs or line breaks.
[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept {
    constexpr std::string_view kSpace = " \t\r\n\f\v";
    auto const first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    auto const last = text.find_last_not_of(kSpace);
    return text.substr(first, last - first + 1);
}

/// @brief Quotes @p text as a JSON string exactly as `JSON.stringify` does: `"` and `\` escaped,
///        the five short control escapes, any other control character as `\u00xx`, everything
///        else verbatim. The body bytes the engine produces depend on this spelling.
/// @param text UTF-8 text.
/// @return The quoted literal.
[[nodiscard]] inline std::string quote(std::string_view text) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (char const chr : text) {
        auto const code = static_cast<unsigned char>(chr);
        switch (chr) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (code < 0x20U) {
                    out += "\\u00";
                    out += kHex.at(code >> 4U);
                    out += kHex.at(code & 0x0FU);
                } else {
                    out += chr;
                }
                break;
        }
    }
    out += '"';
    return out;
}

/// @brief Parses JSON text into the u64-mode DOM.
/// @param text JSON text.
/// @return The DOM, or `std::nullopt` when @p text is not JSON.
[[nodiscard]] inline std::optional<Json> parseJson(std::string_view text) {
    // glaze reads a null-terminated buffer; a view into the middle of a larger string is not one.
    std::string const buffer{text};
    Json dom{};
    if (glz::read_json(dom, buffer)) {
        return std::nullopt;
    }
    return dom;
}

/// @brief Writes a DOM value as compact JSON; integers keep every digit.
/// @param value The value.
/// @return Its JSON text.
[[nodiscard]] inline std::string toJson(Json const& value) { return glz::write_json(value).value_or(std::string{}); }

/// @brief A DOM node holding an empty JSON object.
/// @return `{}`.
[[nodiscard]] inline Json emptyObject() {
    Json node{};
    node.data.emplace<Json::object_t>();
    return node;
}

/// @brief The member @p key of an object node, without inserting anything.
/// @param node The node; a non-object has no members.
/// @param key  The member name.
/// @return The member, or null.
[[nodiscard]] inline Json const* member(Json const& node, std::string_view key) {
    return ::morph::forms::detail::findMember(node, key);
}

/// @brief The string member @p key of an object node.
/// @param node The node.
/// @param key  The member name.
/// @return The string, or null when absent or not a string.
[[nodiscard]] inline std::string const* stringAt(Json const& node, std::string_view key) {
    Json const* const found = member(node, key);
    return found == nullptr ? nullptr : found->get_if<std::string>();
}

/// @brief A JSON number as canonical decimal text, `-?[0-9]+(\.[0-9]+)?`.
///
/// Integers keep every digit; a double is written in fixed notation with the shortest digits
/// that read back as the same double, so `1e-7` is `"0.0000001"` and no text has an exponent.
/// @param value The value.
/// @return The text, or `std::nullopt` when @p value is not a finite number.
[[nodiscard]] inline std::optional<std::string> numberText(Json const& value) {
    if (auto const* const unsignedValue = value.get_if<std::uint64_t>()) {
        return std::to_string(*unsignedValue);
    }
    if (auto const* const signedValue = value.get_if<std::int64_t>()) {
        return std::to_string(*signedValue);
    }
    if (auto const* const doubleValue = value.get_if<double>()) {
        if (!std::isfinite(*doubleValue)) {
            return std::nullopt;
        }
        // 309 integer digits for DBL_MAX, a sign, and the longest shortest fraction.
        std::array<char, 400> buffer{};
        auto const result =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), *doubleValue, std::chars_format::fixed);
        if (result.ec != std::errc{}) {
            return std::nullopt;
        }
        std::string text{buffer.data(), result.ptr};
        if (text == "-0") {
            text = "0";
        }
        return text;
    }
    return std::nullopt;
}

/// @brief A scalar as display text: a string as itself, a number as `numberText`, a boolean
///        as `true`/`false`, null as empty, anything else as compact JSON.
/// @param value The value.
/// @return The text.
[[nodiscard]] inline std::string displayText(Json const& value) {
    if (auto const* const text = value.get_if<std::string>()) {
        return *text;
    }
    if (auto number = numberText(value)) {
        return *std::move(number);
    }
    if (auto const* const flag = value.get_if<bool>()) {
        return *flag ? "true" : "false";
    }
    if (value.is_null()) {
        return {};
    }
    return toJson(value);
}

/// @brief Whether @p text is `-?[0-9]+(\.[0-9]+)?`.
/// @param text The text.
/// @return `true` for canonical decimal text.
[[nodiscard]] inline bool isCanonicalDecimal(std::string_view text) noexcept {
    if (text.starts_with('-')) {
        text.remove_prefix(1);
    }
    auto const dot = text.find('.');
    auto const digits = [](std::string_view part) {
        return !part.empty() && std::ranges::all_of(part, [](char chr) { return chr >= '0' && chr <= '9'; });
    };
    if (dot == std::string_view::npos) {
        return digits(text);
    }
    return digits(text.substr(0, dot)) && digits(text.substr(dot + 1));
}

/// @brief Whether @p text is `-?[0-9]+`.
/// @param text The text.
/// @return `true` for canonical integer text.
[[nodiscard]] inline bool isCanonicalInteger(std::string_view text) noexcept {
    return isCanonicalDecimal(text) && text.find('.') == std::string_view::npos;
}

/// @brief Drops a leading run of zeros that JSON forbids: `"007.50"` → `"7.50"`, `"-00"` → `"-0"`.
/// @param canonical Canonical decimal text.
/// @return The text with at most one leading zero before the point.
[[nodiscard]] inline std::string stripLeadingZeros(std::string_view canonical) {
    std::string out;
    if (canonical.starts_with('-')) {
        out += '-';
        canonical.remove_prefix(1);
    }
    while (canonical.size() > 1 && canonical.front() == '0' && canonical.at(1) != '.') {
        canonical.remove_prefix(1);
    }
    out += canonical;
    return out;
}

/// @brief Orders two canonical decimals exactly, at any magnitude; `-0` equals `0`.
/// @param lhs Canonical decimal text (leading zeros allowed).
/// @param rhs Canonical decimal text (leading zeros allowed).
/// @return The ordering of the two values.
[[nodiscard]] inline std::strong_ordering compareDecimal(std::string_view lhs, std::string_view rhs) noexcept {
    struct Parts {
        bool negative = false;
        std::string_view whole;
        std::string_view fraction;
    };
    auto const split = [](std::string_view text) {
        Parts parts{};
        parts.negative = text.starts_with('-');
        if (parts.negative) {
            text.remove_prefix(1);
        }
        auto const dot = text.find('.');
        parts.whole = text.substr(0, dot);
        parts.fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
        while (parts.whole.size() > 1 && parts.whole.front() == '0') {
            parts.whole.remove_prefix(1);
        }
        while (!parts.fraction.empty() && parts.fraction.back() == '0') {
            parts.fraction.remove_suffix(1);
        }
        if ((parts.whole.empty() || parts.whole == "0") && parts.fraction.empty()) {
            parts.negative = false;
        }
        return parts;
    };
    auto const magnitude = [](Parts const& left, Parts const& right) {
        if (left.whole.size() != right.whole.size()) {
            return left.whole.size() <=> right.whole.size();
        }
        if (auto const order = left.whole <=> right.whole; order != 0) {
            return order;
        }
        return left.fraction <=> right.fraction;
    };
    Parts const left = split(lhs);
    Parts const right = split(rhs);
    if (left.negative != right.negative) {
        return left.negative ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    auto const order = magnitude(left, right);
    if (!left.negative) {
        return order;
    }
    return 0 <=> order;
}

/// @brief Multiplies two non-negative digit strings exactly.
/// @param lhs Decimal digits.
/// @param rhs Decimal digits.
/// @return The product's digits, without leading zeros.
[[nodiscard]] inline std::string multiplyDigits(std::string_view lhs, std::string_view rhs) {
    std::vector<std::uint32_t> product(lhs.size() + rhs.size(), 0U);
    for (std::size_t i = lhs.size(); i-- > 0;) {
        for (std::size_t j = rhs.size(); j-- > 0;) {
            auto const left = static_cast<std::uint32_t>(lhs.at(i) - '0');
            auto const right = static_cast<std::uint32_t>(rhs.at(j) - '0');
            std::uint32_t& low = product.at(i + j + 1);
            std::uint32_t const sum = (left * right) + low;
            low = sum % 10U;
            product.at(i + j) += sum / 10U;
        }
    }
    std::string out;
    for (std::uint32_t const digit : product) {
        if (out.empty() && digit == 0U) {
            continue;
        }
        out += static_cast<char>('0' + digit);
    }
    return out.empty() ? std::string{"0"} : out;
}

/// @brief Orders an exact rational against a canonical decimal, exactly, at any magnitude.
///
/// Cross-multiplies on digits: `|num| * 10^places` against `|digits| * den`, so neither side is
/// ever narrowed to a `Rational` or a `double`.
/// @param value   The rational.
/// @param decimal Canonical decimal text.
/// @return The ordering of @p value against @p decimal.
[[nodiscard]] inline std::strong_ordering compareRationalToDecimal(math::Rational const& value,
                                                                   std::string_view decimal) {
    bool const decimalNegative = decimal.starts_with('-');
    std::string_view magnitude = decimalNegative ? decimal.substr(1) : decimal;
    auto const dot = magnitude.find('.');
    std::string digits{magnitude.substr(0, dot)};
    std::size_t places = 0;
    if (dot != std::string_view::npos) {
        digits += magnitude.substr(dot + 1);
        places = magnitude.size() - dot - 1;
    }
    bool const decimalZero = std::ranges::all_of(digits, [](char chr) { return chr == '0'; });
    int const decimalSign = decimalZero ? 0 : (decimalNegative ? -1 : 1);
    int const valueSign = value.numerator == 0 ? 0 : (value.numerator < 0 ? -1 : 1);
    if (valueSign != decimalSign) {
        return valueSign <=> decimalSign;
    }
    if (valueSign == 0) {
        return std::strong_ordering::equal;
    }
    std::string const scaledValue = std::to_string(math::detail::absU64(value.numerator)) + std::string(places, '0');
    std::string const scaledDecimal = multiplyDigits(digits, std::to_string(value.denominator));
    auto const order = compareDecimal(scaledValue, scaledDecimal);
    return valueSign > 0 ? order : 0 <=> order;
}

/// @brief Renders @p value as canonical decimal text, rounded half away from zero to @p places.
/// @param value  The value.
/// @param places Fraction digits; clamped to `math::kMaxDecimalPlaces`.
/// @return E.g. `"-0.33"` for `-1/3` at two places; a value that rounds to zero has no sign.
[[nodiscard]] inline std::string decimalText(math::Rational const& value, std::uint32_t places) {
    std::uint32_t const wanted = std::min(places, math::kMaxDecimalPlaces);
    math::Rational const rounded =
        math::roundToDecimalPlaces(value, math::DecimalPlaces{wanted}, math::RoundingMode::HalfAwayFromZero);
    std::int64_t const scale = math::detail::powerOfTen(wanted);
    std::string digits = multiplyDigits(std::to_string(math::detail::absU64(rounded.numerator)),
                                        std::to_string(scale / rounded.denominator));
    if (digits.size() <= wanted) {
        digits.insert(0, wanted + 1 - digits.size(), '0');
    }
    std::string out = rounded.numerator < 0 ? "-" : "";
    out += std::string_view{digits}.substr(0, digits.size() - wanted);
    if (wanted > 0) {
        out += '.';
        out += std::string_view{digits}.substr(digits.size() - wanted);
    }
    return out;
}

/// @brief Reads `count` ASCII digits at @p offset of @p text.
/// @param text   The text.
/// @param offset Where the digits start.
/// @param count  How many digits.
/// @return The value, or `std::nullopt` when a character there is not a digit.
[[nodiscard]] inline std::optional<int> digitsAt(std::string_view text, std::size_t offset, std::size_t count) {
    if (offset + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = offset; i < offset + count; ++i) {
        char const chr = text.at(i);
        if (chr < '0' || chr > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (chr - '0');
    }
    return value;
}

/// @brief Reads `YYYY-MM-DDTHH:MM[:SS]` as a calendar instant, validating every component.
///
/// The result is the reading as if it were UTC; the caller shifts it by its display zone.
/// @param text The wall-clock text.
/// @return The instant, or `std::nullopt` for malformed or impossible text.
[[nodiscard]] inline std::optional<std::chrono::sys_seconds> parseWallClock(std::string_view text) {
    if (text.size() != 16 && text.size() != 19) {
        return std::nullopt;
    }
    if (text.at(4) != '-' || text.at(7) != '-' || text.at(10) != 'T' || text.at(13) != ':' ||
        (text.size() == 19 && text.at(16) != ':')) {
        return std::nullopt;
    }
    auto const year = digitsAt(text, 0, 4);
    auto const month = digitsAt(text, 5, 2);
    auto const day = digitsAt(text, 8, 2);
    auto const hour = digitsAt(text, 11, 2);
    auto const minute = digitsAt(text, 14, 2);
    auto const second = text.size() == 19 ? digitsAt(text, 17, 2) : std::optional<int>{0};
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    std::chrono::year_month_day const date{std::chrono::year{*year},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
           std::chrono::seconds{*second};
}

/// @brief Reads an ISO-8601 instant: `YYYY-MM-DDTHH:MM[:SS[.fraction]][Z|±HH:MM|±HHMM]`.
///
/// No zone designator reads as UTC, which is what a `Timestamp` serialises to; a fraction is
/// dropped, since every engine display is to the second.
/// @param text The instant text.
/// @return The UTC instant, or `std::nullopt` when malformed.
[[nodiscard]] inline std::optional<std::chrono::sys_seconds> parseInstant(std::string_view text) {
    std::size_t clockEnd = 19;
    if (text.size() < 19 || text.at(16) != ':') {
        clockEnd = 16;
    }
    auto local = parseWallClock(text.substr(0, clockEnd));
    if (!local) {
        return std::nullopt;
    }
    std::string_view rest = text.substr(clockEnd);
    if (rest.starts_with('.')) {
        rest.remove_prefix(1);
        while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9') {
            rest.remove_prefix(1);
        }
    }
    if (rest.empty() || rest == "Z") {
        return local;
    }
    if (rest.front() != '+' && rest.front() != '-') {
        return std::nullopt;
    }
    int const sign = rest.front() == '-' ? -1 : 1;
    rest.remove_prefix(1);
    std::string compact{rest};
    if (compact.size() == 5 && compact.at(2) == ':') {
        compact.erase(2, 1);
    }
    auto const hours = digitsAt(compact, 0, 2);
    auto const minutes = digitsAt(compact, 2, 2);
    if (compact.size() != 4 || !hours || !minutes) {
        return std::nullopt;
    }
    return *local - (sign * (std::chrono::hours{*hours} + std::chrono::minutes{*minutes}));
}

/// @brief Writes an instant as `YYYY-MM-DDTHH:MM:SS`.
/// @param instant The instant.
/// @return The wall-clock text.
[[nodiscard]] inline std::string formatWallClock(std::chrono::sys_seconds instant) {
    auto const days = std::chrono::floor<std::chrono::days>(instant);
    std::chrono::year_month_day const date{days};
    std::chrono::hh_mm_ss const clock{instant - days};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()), clock.hours().count(),
                       clock.minutes().count(), clock.seconds().count());
}

/// @brief Writes an instant as ISO-8601 UTC, `YYYY-MM-DDTHH:MM:SSZ`.
/// @param instant The instant.
/// @return The text.
[[nodiscard]] inline std::string formatUtc(std::chrono::sys_seconds instant) { return formatWallClock(instant) + "Z"; }

/// @brief Whether @p text is a valid calendar date `YYYY-MM-DD`.
/// @param text The text.
/// @return `true` for a well-formed, existing date.
[[nodiscard]] inline bool isIsoDate(std::string_view text) {
    if (text.size() != 10 || text.at(4) != '-' || text.at(7) != '-') {
        return false;
    }
    auto const year = digitsAt(text, 0, 4);
    auto const month = digitsAt(text, 5, 2);
    auto const day = digitsAt(text, 8, 2);
    if (!year || !month || !day) {
        return false;
    }
    return std::chrono::year_month_day{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                                       std::chrono::day{static_cast<unsigned>(*day)}}
        .ok();
}

}  // namespace morph::forms::detail::engine
```

Add `#include <vector>` to the include list (used by `multiplyDigits`). Register the header and the test file as
listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][text]"`
Expected: PASS, 7 test cases.

Mutation check: in `compareDecimal`, delete the `while (!parts.fraction.empty() && parts.fraction.back() == '0')`
loop. Expected: FAIL in "compareDecimal orders canonical decimals at any magnitude" (`"2.50"` vs `"2.5"` is no longer
equal). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/detail/text.hpp tests/test_forms_engine_text.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): exact text helpers for the forms engine

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `RuleExpr` — `x-rules` parsed, evaluated three-valued and exactly

**Files:**
- Create: `include/morph/forms/engine/rules.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS` block, after `include/morph/forms/instance_constraints.hpp`, add
  `include/morph/forms/engine/rules.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_rules.cpp` after `test_forms_engine_text.cpp`
- Test: `tests/test_forms_engine_rules.cpp`

**Interfaces:**
- Consumes: Task 2's `detail::engine::{Json, member, stringAt, numberText, isCanonicalInteger, compareDecimal,
  compareRationalToDecimal}`; `forms::detail::RuleKind`, `forms::detail::ruleKindName`
  (`include/morph/forms/forms.hpp:675`, `:696`) — only in the test, to prove every kind the emitter can write
  parses.
- Produces (`morph::forms`): `SchemaError{path, message}`; `Tri{False, True, Unknown}`, `triNot(Tri)`,
  `triFromBool(bool)`; `DecimalText{canonical}`; `Scalar = std::variant<std::monostate, bool, std::string,
  DecimalText, math::Rational>`; `compareScalars(Scalar const&, Scalar const&) -> std::optional<std::strong_ordering>`;
  `RuleValue{State{Blank, Invalid, Valid} state; Scalar scalar}`; `RuleLookup =
  std::function<RuleValue(std::string_view)>`; `RuleExpr` with `Kind`, `parse(Json const&, std::string const& path)
  -> std::expected<RuleExpr, SchemaError>`, `kind()`, `kindName()`, `fields()`, `when()`, `condition()`,
  `conditions()`, `literal()`, `gates()`, `holds(RuleLookup const&) -> Tri`, `test(RuleLookup const&) -> Tri`;
  `gatingVerdict`, `visibleVerdict`, `readonlyVerdict`, `requiredVerdict` (each `(std::span<RuleExpr const>, …,
  RuleLookup const&) -> Tri`).

The three-valued reading, which the corpus rows in Task 9 pin end to end:

| Node | Blank operand | Invalid operand (typed, does not encode) | Valid operands |
|---|---|---|---|
| `engaged` / `notEngaged` | False / True | Unknown | True / False |
| `equals` | False | Unknown | exact equality; mismatched types are False |
| `greater` … `lessOrEqual` | True (vacuous) | Unknown | exact ordering; incomparable types are Unknown |
| membership kinds | counts as not engaged | Unknown unless the definite count already decides | counted |
| `requiredWhen` (as a rule) | — | — | True unless `when` is True, then `engaged(field)` |
| `visibleWhen` / `readonlyWhen` (as a rule) | — | — | True: they never gate |
| unrecognised kind | — | — | Unknown |

`and`/`or`/`not` are Kleene. A gating verdict blocks readiness only when it is **False**: the only Unknown that can
reach readiness comes from a kind this engine does not know (an operand that does not encode already blocks through
its own field error), and such a rule is the server's to judge — blocking on it would make every additive extension
of the vocabulary a breaking change for every deployed client.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_rules.cpp` (Task 9 appends the corpus cases to this file):

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <morph/forms/engine/rules.hpp>
#include <morph/forms/forms.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using morph::forms::DecimalText;
using morph::forms::RuleExpr;
using morph::forms::RuleLookup;
using morph::forms::RuleValue;
using morph::forms::Scalar;
using morph::forms::Tri;
namespace engine = morph::forms::detail::engine;

[[nodiscard]] RuleExpr rule(std::string_view json) {
    auto const dom = engine::parseJson(json);
    REQUIRE(dom.has_value());
    auto parsed = RuleExpr::parse(*dom, "x-rules[0]");
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

[[nodiscard]] RuleValue valid(Scalar scalar) { return RuleValue{RuleValue::State::Valid, std::move(scalar)}; }
[[nodiscard]] RuleValue invalid() { return RuleValue{RuleValue::State::Invalid, {}}; }

/// The fields a rule sees; an absent name is blank.
struct Fields {
    std::map<std::string, RuleValue, std::less<>> values{};

    [[nodiscard]] RuleLookup lookup() const {
        return [this](std::string_view name) {
            auto const found = values.find(name);
            return found == values.end() ? RuleValue{} : found->second;
        };
    }
};

}  // namespace

TEST_CASE("RuleExpr: every kind the emitter writes parses to a known kind", "[forms-engine][rules]") {
    for (std::uint8_t ordinal = 0; ordinal < 16; ++ordinal) {
        auto const name = morph::forms::detail::ruleKindName(static_cast<morph::forms::detail::RuleKind>(ordinal));
        REQUIRE_FALSE(name.empty());
        std::string json = R"({"kind":")" + std::string{name} + R"(","fields":["a","b"],)";
        json += R"("when":{"kind":"engaged","fields":["a"]},"condition":{"kind":"engaged","fields":["a"]},)";
        json += R"("conditions":[{"kind":"engaged","fields":["a"]}],"value":1})";
        RuleExpr const parsed = rule(json);
        INFO(name);
        CHECK(parsed.kind() != RuleExpr::Kind::Unknown);
        CHECK(parsed.kindName() == name);
    }
}

TEST_CASE("RuleExpr: a malformed rule is a SchemaError naming where it is", "[forms-engine][rules]") {
    auto const reject = [](std::string_view json) {
        auto const dom = engine::parseJson(json);
        REQUIRE(dom.has_value());
        return RuleExpr::parse(*dom, "x-rules[3]");
    };
    auto const noKind = reject(R"({"fields":["a"]})");
    REQUIRE_FALSE(noKind.has_value());
    CHECK(noKind.error().path == "x-rules[3]");
    CHECK_FALSE(reject(R"({"kind":"engaged","fields":"a"})").has_value());
    CHECK_FALSE(reject(R"({"kind":"requiredWhen","fields":["a"]})").has_value());
    CHECK_FALSE(reject(R"({"kind":"greater","fields":["a"]})").has_value());
    auto const nested = reject(R"({"kind":"and","conditions":[{"kind":"engaged"}]})");
    REQUIRE_FALSE(nested.has_value());
    CHECK(nested.error().path == "x-rules[3].conditions[0]");
    CHECK(reject(R"({"kind":"quantumEntangled"})").has_value());  // unknown is not malformed
}

TEST_CASE("RuleExpr: and, or and not are Kleene over Unknown", "[forms-engine][rules]") {
    Fields const fields{{{"yes", valid(std::string{"x"})}, {"bad", invalid()}}};
    auto const lookup = fields.lookup();
    auto const verdict = [&](std::string_view json) { return rule(json).test(lookup); };
    CHECK(verdict(R"({"kind":"and","conditions":[{"kind":"engaged","fields":["no"]},{"kind":"engaged","fields":["bad"]}]})") == Tri::False);
    CHECK(verdict(R"({"kind":"and","conditions":[{"kind":"engaged","fields":["yes"]},{"kind":"engaged","fields":["bad"]}]})") == Tri::Unknown);
    CHECK(verdict(R"({"kind":"or","conditions":[{"kind":"engaged","fields":["yes"]},{"kind":"engaged","fields":["bad"]}]})") == Tri::True);
    CHECK(verdict(R"({"kind":"or","conditions":[{"kind":"engaged","fields":["no"]},{"kind":"engaged","fields":["bad"]}]})") == Tri::Unknown);
    CHECK(verdict(R"({"kind":"not","condition":{"kind":"engaged","fields":["bad"]}})") == Tri::Unknown);
    CHECK(verdict(R"({"kind":"not","condition":{"kind":"engaged","fields":["no"]}})") == Tri::True);
}

// Review Focus 4.
TEST_CASE("RuleExpr: an unknown kind never blocks, and a false conjunct still does", "[forms-engine][rules]") {
    Fields fields{};
    auto const lookup = fields.lookup();
    std::vector<RuleExpr> const unknownTop{rule(R"({"kind":"quantumEntangled","fields":["email","note"]})")};
    CHECK(morph::forms::gatingVerdict(unknownTop, lookup) == Tri::Unknown);

    std::vector<RuleExpr> const insideNot{
        rule(R"({"kind":"requiredWhen","fields":["note"],"when":{"kind":"not","condition":{"kind":"quantumEntangled","fields":["email"]}}})")};
    CHECK(morph::forms::gatingVerdict(insideNot, lookup) == Tri::True);
    CHECK(morph::forms::requiredVerdict(insideNot, "note", lookup) == Tri::Unknown);

    std::vector<RuleExpr> const falseConjunct{rule(
        R"({"kind":"and","conditions":[{"kind":"engaged","fields":["email"]},{"kind":"quantumEntangled","fields":["note"]}]})")};
    CHECK(morph::forms::gatingVerdict(falseConjunct, lookup) == Tri::False);
    fields.values.emplace("email", valid(std::string{"a@b"}));
    CHECK(morph::forms::gatingVerdict(falseConjunct, lookup) == Tri::Unknown);

    std::vector<RuleExpr> const presentation{
        rule(R"({"kind":"visibleWhen","fields":["note"],"when":{"kind":"quantumEntangled","fields":["email"]}})"),
        rule(R"({"kind":"readonlyWhen","fields":["note"],"when":{"kind":"quantumEntangled","fields":["email"]}})")};
    CHECK(morph::forms::visibleVerdict(presentation, "note", lookup) == Tri::Unknown);   // presents as visible
    CHECK(morph::forms::readonlyVerdict(presentation, "note", lookup) == Tri::Unknown);  // presents as editable
    CHECK(morph::forms::gatingVerdict(presentation, lookup) == Tri::True);
}

TEST_CASE("RuleExpr: comparisons are vacuous on a blank operand and exact on valid ones", "[forms-engine][rules]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;
    Fields fields{};
    RuleExpr const greater = rule(R"({"kind":"greater","fields":["high","low"]})");
    CHECK(greater.test(fields.lookup()) == Tri::True);
    fields.values["high"] = valid(Rational{Numerator{2}, Denominator{1}, DecimalPlaces{2}});
    CHECK(greater.test(fields.lookup()) == Tri::True);
    fields.values["low"] = valid(Rational{Numerator{200}, Denominator{100}, DecimalPlaces{2}});
    CHECK(greater.test(fields.lookup()) == Tri::False);
    fields.values["low"] = valid(Rational{Numerator{199}, Denominator{100}, DecimalPlaces{2}});
    CHECK(greater.test(fields.lookup()) == Tri::True);
    fields.values["low"] = invalid();
    CHECK(greater.test(fields.lookup()) == Tri::Unknown);
    fields.values["low"] = valid(std::string{"text"});
    CHECK(greater.test(fields.lookup()) == Tri::Unknown);  // a number and a string do not order
    fields.values["high"] = valid(DecimalText{"9223372036854775808"});
    fields.values["low"] = valid(DecimalText{"9223372036854775807"});
    CHECK(greater.test(fields.lookup()) == Tri::True);
}

TEST_CASE("RuleExpr: equals compares booleans as booleans and big integers on digits", "[forms-engine][rules]") {
    Fields fields{};
    RuleExpr const flag = rule(R"({"kind":"equals","fields":["flag"],"value":true})");
    CHECK(flag.holds(fields.lookup()) == Tri::False);
    fields.values["flag"] = valid(true);
    CHECK(flag.holds(fields.lookup()) == Tri::True);
    fields.values["flag"] = valid(std::string{"true"});
    CHECK(flag.holds(fields.lookup()) == Tri::False);  // a string is not the boolean

    RuleExpr const big =
        rule(R"({"kind":"equals","fields":["id"],"value":9007199254740993,"valueText":"9007199254740993"})");
    fields.values["id"] = valid(DecimalText{"9007199254740992"});
    CHECK(big.holds(fields.lookup()) == Tri::False);
    fields.values["id"] = valid(DecimalText{"9007199254740993"});
    CHECK(big.holds(fields.lookup()) == Tri::True);

    RuleExpr const exactValue = rule(R"({"kind":"equals","fields":["amount"],"value":{"num":5,"den":2}})");
    fields.values["amount"] = valid(DecimalText{"2.50"});
    CHECK(exactValue.holds(fields.lookup()) == Tri::True);
}

TEST_CASE("RuleExpr: membership counts definite operands and is Unknown only when the count is open",
          "[forms-engine][rules]") {
    Fields fields{};
    RuleExpr const one = rule(R"({"kind":"exactlyOneOf","fields":["a","b","c"]})");
    RuleExpr const least = rule(R"({"kind":"atLeastOneOf","fields":["a","b","c"]})");
    RuleExpr const most = rule(R"({"kind":"mutuallyExclusive","fields":["a","b","c"]})");
    CHECK(one.test(fields.lookup()) == Tri::False);
    CHECK(least.test(fields.lookup()) == Tri::False);
    CHECK(most.test(fields.lookup()) == Tri::True);
    fields.values["a"] = invalid();
    CHECK(one.test(fields.lookup()) == Tri::Unknown);
    CHECK(least.test(fields.lookup()) == Tri::Unknown);
    CHECK(most.test(fields.lookup()) == Tri::True);
    fields.values["b"] = valid(std::string{"x"});
    fields.values["c"] = valid(std::string{"y"});
    CHECK(one.test(fields.lookup()) == Tri::False);
    CHECK(least.test(fields.lookup()) == Tri::True);
    CHECK(most.test(fields.lookup()) == Tri::False);
}

TEST_CASE("RuleExpr: presentation verdicts combine every rule naming the field", "[forms-engine][rules]") {
    Fields fields{};
    std::vector<RuleExpr> const rules{
        rule(R"({"kind":"visibleWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}})"),
        rule(R"({"kind":"requiredWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}})"),
        rule(R"({"kind":"readonlyWhen","fields":["total"],"when":{"kind":"engaged","fields":["promo"]}})")};
    CHECK(morph::forms::visibleVerdict(rules, "discount", fields.lookup()) == Tri::False);
    CHECK(morph::forms::visibleVerdict(rules, "total", fields.lookup()) == Tri::True);
    CHECK(morph::forms::requiredVerdict(rules, "discount", fields.lookup()) == Tri::False);
    CHECK(morph::forms::readonlyVerdict(rules, "total", fields.lookup()) == Tri::False);
    fields.values["promo"] = valid(DecimalText{"5"});
    CHECK(morph::forms::visibleVerdict(rules, "discount", fields.lookup()) == Tri::True);
    CHECK(morph::forms::requiredVerdict(rules, "discount", fields.lookup()) == Tri::True);
    CHECK(morph::forms::readonlyVerdict(rules, "total", fields.lookup()) == Tri::True);
    CHECK(morph::forms::gatingVerdict(rules, fields.lookup()) == Tri::False);  // discount now required, blank
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/rules.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/rules.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/rules.hpp
/// @brief `x-rules` as a parsed `RuleExpr` tree: three-valued, exact evaluation over a form's
///        encoded values, and the presentation queries the view binds to.
///
/// Specified in `docs/spec/forms/engine.md`, "Rules".

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../../util/rational.hpp"
#include "detail/text.hpp"

namespace morph::forms {

/// @brief Why a schema document could not be read.
struct SchemaError {
    /// @brief Where in the document: empty for the root, else e.g. `properties.mass` or `x-rules[2].when`.
    std::string path{};
    /// @brief What is wrong there.
    std::string message{};
    /// @brief Member-wise equality.
    /// @param other The other error.
    /// @return `true` when path and message match.
    bool operator==(SchemaError const& other) const = default;
};

/// @brief A three-valued truth value: a rule over a half-typed form can be undecidable.
enum class Tri : std::uint8_t {
    False,   ///< Definitely does not hold.
    True,    ///< Definitely holds.
    Unknown  ///< Cannot be decided: an operand does not encode, or the kind is not one this engine knows.
};

/// @brief Kleene negation.
/// @param value The operand.
/// @return `True` for `False`, `False` for `True`, `Unknown` for `Unknown`.
[[nodiscard]] constexpr Tri triNot(Tri value) noexcept {
    switch (value) {
        case Tri::False:
            return Tri::True;
        case Tri::True:
            return Tri::False;
        default:
            return Tri::Unknown;
    }
}

/// @brief Lifts a definite boolean.
/// @param value The boolean.
/// @return `True` or `False`.
[[nodiscard]] constexpr Tri triFromBool(bool value) noexcept { return value ? Tri::True : Tri::False; }

/// @brief A canonical decimal (`-?[0-9]+(\.[0-9]+)?`) compared on its digits, never through a double.
struct DecimalText {
    /// @brief The canonical text.
    std::string canonical{};
    /// @brief Textual equality; numeric equality is `compareScalars`.
    /// @param other The other decimal.
    /// @return `true` when the texts match.
    bool operator==(DecimalText const& other) const = default;
};

/// @brief An exact scalar a rule compares: none, a boolean, a string, a decimal, or a rational.
using Scalar = std::variant<std::monostate, bool, std::string, DecimalText, math::Rational>;

/// @brief Orders two scalars exactly, when they are of comparable kinds.
///
/// Booleans order `false < true`; strings byte-wise (ISO-8601 instants therefore in time order);
/// decimals and rationals numerically, against each other too. Any other pairing has no order.
/// @param lhs Left scalar.
/// @param rhs Right scalar.
/// @return The ordering, or `std::nullopt` when the two do not compare.
[[nodiscard]] inline std::optional<std::strong_ordering> compareScalars(Scalar const& lhs, Scalar const& rhs) {
    if (auto const* const left = std::get_if<bool>(&lhs)) {
        if (auto const* const right = std::get_if<bool>(&rhs)) {
            return *left <=> *right;
        }
        return std::nullopt;
    }
    if (auto const* const left = std::get_if<std::string>(&lhs)) {
        if (auto const* const right = std::get_if<std::string>(&rhs)) {
            return *left <=> *right;
        }
        return std::nullopt;
    }
    auto const* const leftDecimal = std::get_if<DecimalText>(&lhs);
    auto const* const rightDecimal = std::get_if<DecimalText>(&rhs);
    auto const* const leftRational = std::get_if<math::Rational>(&lhs);
    auto const* const rightRational = std::get_if<math::Rational>(&rhs);
    if (leftDecimal != nullptr && rightDecimal != nullptr) {
        return detail::engine::compareDecimal(leftDecimal->canonical, rightDecimal->canonical);
    }
    if (leftRational != nullptr && rightRational != nullptr) {
        return *leftRational <=> *rightRational;
    }
    if (leftRational != nullptr && rightDecimal != nullptr) {
        return detail::engine::compareRationalToDecimal(*leftRational, rightDecimal->canonical);
    }
    if (leftDecimal != nullptr && rightRational != nullptr) {
        return 0 <=> detail::engine::compareRationalToDecimal(*rightRational, leftDecimal->canonical);
    }
    return std::nullopt;
}

/// @brief What a rule sees of one field.
struct RuleValue {
    /// @brief The field's draft, as far as rules are concerned.
    enum class State : std::uint8_t {
        Blank,    ///< Nothing entered.
        Invalid,  ///< Something entered that does not encode.
        Valid     ///< Entered and encodes; `scalar` holds the exact value.
    };
    /// @brief The draft's state.
    State state = State::Blank;
    /// @brief The exact value when `state` is `Valid`.
    Scalar scalar{};
};

/// @brief Looks a top-level field up by wire name; an unknown name reads as blank.
using RuleLookup = std::function<RuleValue(std::string_view field)>;

/// @brief One parsed `x-rules` node: a rule, or a condition nested inside one.
class RuleExpr {
public:
    /// @brief The node kinds `forms.hpp` emits, plus `Unknown` for any other `"kind"`.
    enum class Kind : std::uint8_t {
        Engaged,
        NotEngaged,
        Equals,
        Greater,
        GreaterOrEqual,
        Less,
        LessOrEqual,
        RequiredWhen,
        ExactlyOneOf,
        AtLeastOneOf,
        MutuallyExclusive,
        VisibleWhen,
        ReadonlyWhen,
        And,
        Or,
        Not,
        Unknown
    };

    /// @brief Parses one node.
    ///
    /// An unrecognised `"kind"` is kept as `Kind::Unknown`, not refused: it is a rule this engine
    /// cannot judge, and the server, which knows it, still does. A recognised kind missing what
    /// it needs (`fields`, `when`, `condition`, `conditions`) is malformed.
    /// @param node The node.
    /// @param path Where it is, for the error (e.g. `x-rules[2]`).
    /// @return The node, or the error naming where the document is malformed.
    // NOLINTNEXTLINE(misc-no-recursion) -- a condition tree is parsed by the same function, node by node
    [[nodiscard]] static std::expected<RuleExpr, SchemaError> parse(detail::engine::Json const& node,
                                                                    std::string const& path) {
        using detail::engine::Json;
        if (!node.is_object()) {
            return std::unexpected(SchemaError{path, "a rule must be a JSON object"});
        }
        auto const* const kind = detail::engine::stringAt(node, "kind");
        if (kind == nullptr) {
            return std::unexpected(SchemaError{path, "a rule needs a string \"kind\""});
        }
        RuleExpr expr{};
        expr._kindName = *kind;
        expr._kind = kindOf(*kind);
        if (Json const* const fields = detail::engine::member(node, "fields")) {
            if (!fields->is_array()) {
                return std::unexpected(SchemaError{path, "\"fields\" must be an array of strings"});
            }
            for (Json const& name : fields->get_array()) {
                auto const* const text = name.get_if<std::string>();
                if (text == nullptr) {
                    return std::unexpected(SchemaError{path, "\"fields\" must be an array of strings"});
                }
                expr._fields.push_back(*text);
            }
        }
        auto child = [&](std::string_view key) -> std::expected<void, SchemaError> {
            Json const* const nested = detail::engine::member(node, key);
            if (nested == nullptr) {
                return std::unexpected(SchemaError{path, expr._kindName + " needs \"" + std::string{key} + "\""});
            }
            auto parsed = parse(*nested, path + "." + std::string{key});
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            expr._children.push_back(*std::move(parsed));
            return {};
        };
        auto const needFields = [&](std::size_t count, bool exact) -> std::expected<void, SchemaError> {
            if (exact ? expr._fields.size() != count : expr._fields.size() < count) {
                return std::unexpected(SchemaError{path, "\"" + expr._kindName + "\" needs " +
                                                             (exact ? "exactly " : "at least ") +
                                                             std::to_string(count) + " field(s)"});
            }
            return {};
        };
        std::expected<void, SchemaError> shape{};
        switch (expr._kind) {
            case Kind::RequiredWhen:
            case Kind::VisibleWhen:
            case Kind::ReadonlyWhen:
                shape = needFields(1, false);
                if (shape) {
                    shape = child("when");
                }
                break;
            case Kind::Not:
                shape = child("condition");
                break;
            case Kind::And:
            case Kind::Or: {
                Json const* const list = detail::engine::member(node, "conditions");
                if (list == nullptr || !list->is_array()) {
                    shape = std::unexpected(SchemaError{path, "\"" + expr._kindName + "\" needs a \"conditions\" array"});
                    break;
                }
                std::size_t index = 0;
                for (Json const& nested : list->get_array()) {
                    auto parsed = parse(nested, path + ".conditions[" + std::to_string(index++) + "]");
                    if (!parsed) {
                        return std::unexpected(parsed.error());
                    }
                    expr._children.push_back(*std::move(parsed));
                }
                break;
            }
            case Kind::Greater:
            case Kind::GreaterOrEqual:
            case Kind::Less:
            case Kind::LessOrEqual:
                shape = needFields(2, true);
                break;
            case Kind::Equals:
                shape = needFields(1, false);
                expr._literal = literalOf(node);
                break;
            case Kind::Engaged:
            case Kind::NotEngaged:
            case Kind::ExactlyOneOf:
            case Kind::AtLeastOneOf:
            case Kind::MutuallyExclusive:
                shape = needFields(1, false);
                break;
            default:
                break;
        }
        if (!shape) {
            return std::unexpected(shape.error());
        }
        return expr;
    }

    /// @brief The node's kind.
    /// @return The kind; `Unknown` for a `"kind"` this engine does not know.
    [[nodiscard]] Kind kind() const noexcept { return _kind; }

    /// @brief The `"kind"` string as written.
    /// @return The name.
    [[nodiscard]] std::string_view kindName() const noexcept { return _kindName; }

    /// @brief The wire names the node ranges over.
    /// @return The `fields` list; empty for the compound kinds.
    [[nodiscard]] std::span<std::string const> fields() const noexcept { return _fields; }

    /// @brief The `when` clause of `requiredWhen`, `visibleWhen` or `readonlyWhen`.
    /// @return The clause, or null for any other kind.
    [[nodiscard]] RuleExpr const* when() const noexcept {
        bool const hasWhen = _kind == Kind::RequiredWhen || _kind == Kind::VisibleWhen || _kind == Kind::ReadonlyWhen;
        return hasWhen && !_children.empty() ? &_children.front() : nullptr;
    }

    /// @brief The negated condition of `not`.
    /// @return The condition, or null for any other kind.
    [[nodiscard]] RuleExpr const* condition() const noexcept {
        return _kind == Kind::Not && !_children.empty() ? &_children.front() : nullptr;
    }

    /// @brief The operands of `and` / `or`.
    /// @return The conditions; empty for any other kind.
    [[nodiscard]] std::span<RuleExpr const> conditions() const noexcept {
        return (_kind == Kind::And || _kind == Kind::Or) ? std::span<RuleExpr const>{_children}
                                                          : std::span<RuleExpr const>{};
    }

    /// @brief The literal `equals` compares against.
    /// @return The literal; `std::monostate` for any other kind.
    [[nodiscard]] Scalar const& literal() const noexcept { return _literal; }

    /// @brief Whether this node, as a top-level rule, can block submission.
    /// @return `false` for `visibleWhen` / `readonlyWhen`, which only present.
    [[nodiscard]] bool gates() const noexcept { return _kind != Kind::VisibleWhen && _kind != Kind::ReadonlyWhen; }

    /// @brief Evaluates the node as a condition (inside `when`, `and`, `or`, `not`).
    /// @param lookup The form's field values.
    /// @return The verdict; `Unknown` for the rule-only kinds nested as a condition.
    // NOLINTNEXTLINE(misc-no-recursion) -- and/or/not evaluate their operands with the same function
    [[nodiscard]] Tri holds(RuleLookup const& lookup) const {
        switch (_kind) {
            case Kind::Engaged:
                return engaged(lookup(_fields.front()));
            case Kind::NotEngaged:
                return triNot(engaged(lookup(_fields.front())));
            case Kind::Equals:
                return equals(lookup(_fields.front()));
            case Kind::Greater:
            case Kind::GreaterOrEqual:
            case Kind::Less:
            case Kind::LessOrEqual:
                return compare(lookup(_fields.front()), lookup(_fields.back()));
            case Kind::ExactlyOneOf:
            case Kind::AtLeastOneOf:
            case Kind::MutuallyExclusive:
                return membership(lookup);
            case Kind::And: {
                Tri verdict = Tri::True;
                for (RuleExpr const& operand : _children) {
                    Tri const part = operand.holds(lookup);
                    if (part == Tri::False) {
                        return Tri::False;
                    }
                    if (part == Tri::Unknown) {
                        verdict = Tri::Unknown;
                    }
                }
                return verdict;
            }
            case Kind::Or: {
                Tri verdict = Tri::False;
                for (RuleExpr const& operand : _children) {
                    Tri const part = operand.holds(lookup);
                    if (part == Tri::True) {
                        return Tri::True;
                    }
                    if (part == Tri::Unknown) {
                        verdict = Tri::Unknown;
                    }
                }
                return verdict;
            }
            case Kind::Not:
                return triNot(_children.front().holds(lookup));
            default:
                return Tri::Unknown;
        }
    }

    /// @brief Evaluates the node as a top-level rule: whether it lets the form submit.
    /// @param lookup The form's field values.
    /// @return The gating verdict; `True` for the presentation kinds.
    [[nodiscard]] Tri test(RuleLookup const& lookup) const {
        switch (_kind) {
            case Kind::RequiredWhen:
                if (_children.front().holds(lookup) != Tri::True) {
                    return Tri::True;
                }
                return engaged(lookup(_fields.front()));
            case Kind::VisibleWhen:
            case Kind::ReadonlyWhen:
                return Tri::True;
            default:
                return holds(lookup);
        }
    }

private:
    [[nodiscard]] static Kind kindOf(std::string_view name) noexcept {
        constexpr std::array<std::pair<std::string_view, Kind>, 16> kNames{{
            {"engaged", Kind::Engaged},
            {"notEngaged", Kind::NotEngaged},
            {"equals", Kind::Equals},
            {"greater", Kind::Greater},
            {"greaterOrEqual", Kind::GreaterOrEqual},
            {"less", Kind::Less},
            {"lessOrEqual", Kind::LessOrEqual},
            {"requiredWhen", Kind::RequiredWhen},
            {"exactlyOneOf", Kind::ExactlyOneOf},
            {"atLeastOneOf", Kind::AtLeastOneOf},
            {"mutuallyExclusive", Kind::MutuallyExclusive},
            {"visibleWhen", Kind::VisibleWhen},
            {"readonlyWhen", Kind::ReadonlyWhen},
            {"and", Kind::And},
            {"or", Kind::Or},
            {"not", Kind::Not},
        }};
        auto const found = std::ranges::find(kNames, name, &std::pair<std::string_view, Kind>::first);
        return found == kNames.end() ? Kind::Unknown : found->second;
    }

    [[nodiscard]] static Scalar literalOf(detail::engine::Json const& node) {
        using detail::engine::Json;
        if (auto const* const digits = detail::engine::stringAt(node, "valueText");
            digits != nullptr && detail::engine::isCanonicalInteger(*digits)) {
            return DecimalText{*digits};
        }
        Json const* const value = detail::engine::member(node, "value");
        if (value == nullptr) {
            return std::monostate{};
        }
        if (auto const* const flag = value->get_if<bool>()) {
            return *flag;
        }
        if (auto const* const text = value->get_if<std::string>()) {
            return *text;
        }
        if (auto number = detail::engine::numberText(*value)) {
            return DecimalText{*std::move(number)};
        }
        auto const integer = [](Json const* part) -> std::optional<std::int64_t> {
            if (part == nullptr) {
                return std::nullopt;
            }
            if (auto const* const asSigned = part->get_if<std::int64_t>()) {
                return *asSigned;
            }
            if (auto const* const asUnsigned = part->get_if<std::uint64_t>();
                asUnsigned != nullptr && std::in_range<std::int64_t>(*asUnsigned)) {
                return static_cast<std::int64_t>(*asUnsigned);
            }
            return std::nullopt;
        };
        auto const num = integer(detail::engine::member(*value, "num"));
        auto const den = integer(detail::engine::member(*value, "den"));
        if (num && den && *den != 0) {
            return math::Rational{math::Numerator{*num}, math::Denominator{*den}, math::DecimalPlaces{0}};
        }
        return std::monostate{};
    }

    [[nodiscard]] static Tri engaged(RuleValue const& value) noexcept {
        switch (value.state) {
            case RuleValue::State::Blank:
                return Tri::False;
            case RuleValue::State::Valid:
                return Tri::True;
            default:
                return Tri::Unknown;
        }
    }

    [[nodiscard]] Tri equals(RuleValue const& value) const {
        if (value.state == RuleValue::State::Blank) {
            return Tri::False;
        }
        if (value.state == RuleValue::State::Invalid) {
            return Tri::Unknown;
        }
        auto const order = compareScalars(value.scalar, _literal);
        return triFromBool(order.has_value() && *order == std::strong_ordering::equal);
    }

    [[nodiscard]] Tri compare(RuleValue const& lhs, RuleValue const& rhs) const {
        if (lhs.state == RuleValue::State::Blank || rhs.state == RuleValue::State::Blank) {
            return Tri::True;
        }
        if (lhs.state == RuleValue::State::Invalid || rhs.state == RuleValue::State::Invalid) {
            return Tri::Unknown;
        }
        auto const order = compareScalars(lhs.scalar, rhs.scalar);
        if (!order) {
            return Tri::Unknown;
        }
        switch (_kind) {
            case Kind::Greater:
                return triFromBool(*order == std::strong_ordering::greater);
            case Kind::GreaterOrEqual:
                return triFromBool(*order != std::strong_ordering::less);
            case Kind::Less:
                return triFromBool(*order == std::strong_ordering::less);
            default:
                return triFromBool(*order != std::strong_ordering::greater);
        }
    }

    [[nodiscard]] Tri membership(RuleLookup const& lookup) const {
        std::size_t definite = 0;
        std::size_t open = 0;
        for (std::string const& name : _fields) {
            Tri const one = engaged(lookup(name));
            definite += one == Tri::True ? 1U : 0U;
            open += one == Tri::Unknown ? 1U : 0U;
        }
        switch (_kind) {
            case Kind::ExactlyOneOf:
                if (definite >= 2) {
                    return Tri::False;
                }
                if (open == 0) {
                    return triFromBool(definite == 1);
                }
                return Tri::Unknown;
            case Kind::AtLeastOneOf:
                if (definite >= 1) {
                    return Tri::True;
                }
                return open == 0 ? Tri::False : Tri::Unknown;
            default:
                if (definite >= 2) {
                    return Tri::False;
                }
                return definite + open <= 1 ? Tri::True : Tri::Unknown;
        }
    }

    Kind _kind = Kind::Unknown;
    std::string _kindName{};
    std::vector<std::string> _fields{};
    std::vector<RuleExpr> _children{};
    Scalar _literal{};
};

/// @brief Whether a rule list lets the form submit.
/// @param rules  The parsed `x-rules`.
/// @param lookup The form's field values.
/// @return `False` when any rule fails, else `Unknown` when any is undecidable, else `True`.
[[nodiscard]] inline Tri gatingVerdict(std::span<RuleExpr const> rules, RuleLookup const& lookup) {
    Tri verdict = Tri::True;
    for (RuleExpr const& entry : rules) {
        Tri const one = entry.test(lookup);
        if (one == Tri::False) {
            return Tri::False;
        }
        if (one == Tri::Unknown) {
            verdict = Tri::Unknown;
        }
    }
    return verdict;
}

namespace detail::engine {

/// @brief Combines the `when` verdicts of every rule of @p kind naming @p field.
/// @param rules    The parsed `x-rules`.
/// @param kind     The presentation kind.
/// @param field    The wire name.
/// @param lookup   The form's field values.
/// @param decisive The verdict that settles the answer as soon as one rule gives it.
/// @param absent   The answer when no rule names the field.
/// @return @p decisive if any rule gives it, else `Unknown` if any is undecidable, else the other value
///         (or @p absent when no rule names the field).
[[nodiscard]] inline Tri presentation(std::span<RuleExpr const> rules, RuleExpr::Kind kind, std::string_view field,
                                      RuleLookup const& lookup, Tri decisive, Tri absent) {
    bool named = false;
    bool unknown = false;
    for (RuleExpr const& entry : rules) {
        if (entry.kind() != kind || entry.fields().empty() || entry.fields().front() != field) {
            continue;
        }
        named = true;
        Tri const when = entry.when()->holds(lookup);
        if (when == decisive) {
            return decisive;
        }
        unknown = unknown || when == Tri::Unknown;
    }
    if (!named) {
        return absent;
    }
    return unknown ? Tri::Unknown : triNot(decisive);
}

}  // namespace detail::engine

/// @brief Whether `visibleWhen` shows @p field. Present `Unknown` as visible.
/// @param rules  The parsed `x-rules`.
/// @param field  The wire name.
/// @param lookup The form's field values.
/// @return `False` when any rule's condition is false; `True` when none names the field.
[[nodiscard]] inline Tri visibleVerdict(std::span<RuleExpr const> rules, std::string_view field,
                                        RuleLookup const& lookup) {
    return detail::engine::presentation(rules, RuleExpr::Kind::VisibleWhen, field, lookup, Tri::False, Tri::True);
}

/// @brief Whether `readonlyWhen` freezes @p field. Present `Unknown` as editable.
/// @param rules  The parsed `x-rules`.
/// @param field  The wire name.
/// @param lookup The form's field values.
/// @return `True` when any rule's condition is true; `False` when none names the field.
[[nodiscard]] inline Tri readonlyVerdict(std::span<RuleExpr const> rules, std::string_view field,
                                         RuleLookup const& lookup) {
    return detail::engine::presentation(rules, RuleExpr::Kind::ReadonlyWhen, field, lookup, Tri::True, Tri::False);
}

/// @brief Whether `requiredWhen` currently requires @p field. Present `Unknown` as not required.
/// @param rules  The parsed `x-rules`.
/// @param field  The wire name.
/// @param lookup The form's field values.
/// @return `True` when any rule's condition is true; `False` when none names the field.
[[nodiscard]] inline Tri requiredVerdict(std::span<RuleExpr const> rules, std::string_view field,
                                         RuleLookup const& lookup) {
    return detail::engine::presentation(rules, RuleExpr::Kind::RequiredWhen, field, lookup, Tri::True, Tri::False);
}

}  // namespace morph::forms
```

Register the header and the test as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][rules]"`
Expected: PASS, 8 test cases.

Mutation check: in `RuleExpr::compare`, change the blank branch's `return Tri::True;` to `return Tri::False;`.
Expected: FAIL in "comparisons are vacuous on a blank operand and exact on valid ones". Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/rules.hpp tests/test_forms_engine_rules.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): x-rules as RuleExpr, three-valued and exact

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: `FormModel::fromSchema` — the field model

**Files:**
- Create: `include/morph/forms/engine/field_model.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/rules.hpp`, add
  `include/morph/forms/engine/field_model.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_model.cpp` after `test_forms_engine_rules.cpp`
- Test: `tests/test_forms_engine_model.cpp`

**Interfaces:**
- Consumes: Task 2's text helpers; Task 3's `SchemaError`, `RuleExpr`; `forms::BlankAs`
  (`include/morph/forms/forms.hpp:197`); `forms::GroupKind` (`include/morph/forms/layout.hpp`);
  `forms::schemaJson<A>()` (`include/morph/forms/forms.hpp:3594`); `model::ActionTraits<A>::typeId()`
  (`include/morph/core/registry.hpp`).
- Produces (`morph::forms`), exactly as later tasks use them:
  - `FieldKind{Text, Multiline, Integer, Number, Decimal, Quantity, Boolean, Enum, Choice, DateTime, Date, Slider,
    Array, Object, ObjectArray}` (the contract's enumerators) and `fieldKindName(FieldKind) -> std::string_view`.
  - `SubmitMode{Automatic, Explicit}`.
  - `kMaxNestingDepth` (= 4): how many object levels below the action a schema is described.
  - `UnitOption{id, display, decimals, num, den}`, `ChoiceOption{valueJson, label}`, `ChoiceSpec{optionsAction,
    valueField, labelField, dependsOn}`.
  - `FieldSpec` with the members listed in Step 3; `FieldGroupSpec{title, kind, fields, implicit}`.
  - `FormModel`: `fromSchema(std::string_view actionType, std::string_view schemaJson) -> std::expected<FormModel,
    SchemaError>`, the contract's `template <class A> static FormModel forAction()` (throws `std::logic_error`
    naming the action id, the error's path and message when the engine cannot read the action's own schema),
    `actionType()`, `title()`, `fields()`, `groups()`, `rules()`, `submitMode()`, and the addition
    `find(std::string_view name) const -> FieldSpec const*`.
  - In `detail::engine`: `contains(range, value)` and `elementAt(std::span<T>, std::size_t) -> T&` (bounds-checked;
    a span has no `at()` before C++26) — every later engine header indexes spans through it.

A field's kind is decided in the order the QML renderer's encoder asked, so a schema keeps the control it had:
array (of objects → `ObjectArray`, else `Array`); a closed set → `Enum`; `x-optionsAction` → `Choice`;
`format: date-time` → `DateTime`; `format: date` → `Date`; `x-decimalPlaces` → `Quantity` with `ExtUnits`, else
`Decimal`; `x-widget: slider` with `x-min`/`x-max` on a numeric type → `Slider`; `integer`; `boolean`; `number`;
`object` → `Object`; `x-widget: textarea` → `Multiline`; else `Text`. Object members are described to
`kMaxNestingDepth` levels and stop at a `$ref` already on the path — a self-referential type is described once,
and the member where it would repeat is `truncated` (Task 7 says how a truncated member encodes).

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_model.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/forms.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// File-scope, not anonymous-namespaced: glaze's reflection needs linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
struct FmBooking {
    std::int64_t guests = 0;
    std::optional<std::string> note{};
};
struct FmBooked {
    bool confirmed = false;
};
struct FmModel {
    FmBooked execute(FmBooking const&) { return FmBooked{.confirmed = true}; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(FmModel, "Test_FormsEngineModel_Model")
BRIDGE_REGISTER_ACTION(FmModel, FmBooking, "Test_FormsEngineModel_Booking")

namespace {

using morph::forms::FieldKind;
using morph::forms::FieldSpec;
using morph::forms::FormModel;
using morph::forms::GroupKind;

[[nodiscard]] FormModel model(std::string_view action, std::string_view schema) {
    auto parsed = FormModel::fromSchema(action, schema);
    INFO((parsed ? std::string{} : parsed.error().path + ": " + parsed.error().message));
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

[[nodiscard]] std::string names(std::span<FieldSpec const> fields) {
    std::string out{};
    for (auto const& field : fields) {
        out += (out.empty() ? "" : ",") + field.name;
    }
    return out;
}

constexpr std::string_view kProbe = R"({"type":"object","properties":{
  "slot":{"type":["integer","null"],"x-order":0,"title":"Slot","x-optionsAction":"ListSlots","x-optionValue":"id","x-optionLabel":"name"},
  "mass":{"$ref":"#/$defs/q","x-order":1,"x-decimalPlaces":3,"title":"Mass","x-placeholder":"e.g. 1050",
          "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000},{"id":"t","display":"t","decimals":4,"num":1000,"den":1}]},
  "when":{"type":["string","null"],"format":"date-time","x-order":2,"title":"When","x-readonly":true},
  "note":{"type":["string","null"],"x-order":3,"title":"Notes","x-hidden":true}},
  "$defs":{"q":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}},
  "required":["slot","mass","when"]})";

}  // namespace

TEST_CASE("FormModel: kinds, units, the Choice descriptor and flags come from the schema", "[forms-engine][model]") {
    FormModel const probe = model("Probe", kProbe);
    CHECK(names(probe.fields()) == "slot,mass,when,note");
    FieldSpec const& slot = probe.fields()[0];
    CHECK(slot.kind == FieldKind::Choice);
    REQUIRE(slot.choice.has_value());
    CHECK(slot.choice->optionsAction == "ListSlots");
    CHECK(slot.choice->valueField == "id");
    CHECK(slot.choice->labelField == "name");
    CHECK(slot.required);
    CHECK(slot.title == "Slot");
    FieldSpec const& mass = probe.fields()[1];
    CHECK(mass.kind == FieldKind::Quantity);
    CHECK(mass.decimalPlaces == 3U);
    CHECK(mass.unit == "kg");
    REQUIRE(mass.units.size() == 3);
    CHECK(mass.units[1].display == "g");
    CHECK(mass.units[2].num == 1000);
    CHECK(mass.placeholder == "e.g. 1050");
    FieldSpec const& when = probe.fields()[2];
    CHECK(when.kind == FieldKind::DateTime);
    CHECK(when.readOnly);
    FieldSpec const& note = probe.fields()[3];
    CHECK(note.kind == FieldKind::Text);
    CHECK_FALSE(note.required);
    CHECK(note.hidden);
    CHECK(probe.submitMode() == morph::forms::SubmitMode::Automatic);
    CHECK(probe.actionType() == "Probe");
}

TEST_CASE("FormModel: widget hints become Multiline, Slider and a radio Choice", "[forms-engine][model]") {
    FormModel const hints = model("WidgetHintsProbe", R"({"properties":{
      "summary":{"type":["string","null"],"x-order":0,"x-widget":"textarea"},
      "level":{"type":["integer","null"],"x-order":1,"x-widget":"slider","x-min":0,"x-max":100,"x-step":5},
      "mode":{"type":["integer","null"],"x-order":2,"x-widget":"radio","x-optionsAction":"ListModes"},
      "plain":{"type":["string","null"],"x-order":3}},"required":["summary","level","mode"]})");
    CHECK(hints.fields()[0].kind == FieldKind::Multiline);
    FieldSpec const& level = hints.fields()[1];
    CHECK(level.kind == FieldKind::Slider);
    CHECK(level.sliderMin == 0);
    CHECK(level.sliderMax == 100);
    CHECK(level.sliderStep == 5);
    CHECK(hints.fields()[2].kind == FieldKind::Choice);
    CHECK(hints.fields()[2].widget == "radio");
    CHECK(hints.fields()[3].kind == FieldKind::Text);
}

TEST_CASE("FormModel: every renderer kind is named", "[forms-engine][model]") {
    FormModel const kinds = model("T_Kind", R"({"$defs":{"double":{"type":"number"},
      "Row":{"type":"object","properties":{"a":{"type":"integer","x-order":0}},"required":["a"]},
      "Sub":{"type":"object","properties":{"b":{"type":"string","x-order":0}}}},
      "properties":{
      "mass":{"type":["object","null"],"properties":{"num":{"type":"integer"},"den":{"type":"integer"},"dp":{"type":"integer"}},"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},"x-decimalPlaces":2,"x-order":0},
      "sample":{"type":"integer","x-optionsAction":"ListSamples","x-order":1},
      "role":{"type":"string","oneOf":[{"title":"A","const":"A"},{"title":"B","const":"B"}],"x-order":2},
      "takenAt":{"type":"string","format":"date-time","x-order":3},
      "day":{"type":"string","format":"date","x-order":4},
      "done":{"type":"boolean","x-order":5},
      "count":{"type":"integer","x-order":6},
      "ratio":{"$ref":"#/$defs/double","x-order":7},
      "note":{"type":"string","x-order":8},
      "tags":{"type":"array","items":{"type":"string"},"x-order":9},
      "rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":10},
      "sub":{"$ref":"#/$defs/Sub","x-order":11},
      "limit":{"type":"object","properties":{"num":{"type":"integer"},"den":{"type":"integer"},"dp":{"type":"integer"}},"x-decimalPlaces":2,"x-order":12}},
      "required":[]})");
    std::vector<std::string> kindsByField{};
    for (auto const& field : kinds.fields()) {
        kindsByField.push_back(field.name + ":" + std::string{morph::forms::fieldKindName(field.kind)});
    }
    CHECK(kindsByField == std::vector<std::string>{"mass:quantity", "sample:choice", "role:enum", "takenAt:datetime",
                                                   "day:date", "done:boolean", "count:integer", "ratio:number",
                                                   "note:text", "tags:array", "rows:objectArray", "sub:object",
                                                   "limit:decimal"});
}

TEST_CASE("FormModel: a closed set is an Enum in every spelling the emitter or a hand writes",
          "[forms-engine][model]") {
    FormModel const sets = model("Sets", R"({"$defs":{"int64_t":{"type":"integer"},
      "Grade":{"type":"string","oneOf":[{"title":"Low","const":"Low"},{"title":"High","const":"High"}]}},
      "properties":{
      "role":{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},{"title":"Member","const":"Member"},{"title":"Manager","const":"Manager"}],"x-order":0,"title":"Role"},
      "size":{"enum":[1,2,3],"x-order":1},
      "nullable":{"oneOf":[{"title":"On","const":"On"},{"title":"Off","const":"Off"},{"type":"null"}],"x-order":2},
      "grade":{"anyOf":[{"$ref":"#/$defs/Grade"},{"type":"null"}],"x-order":3},
      "inlined":{"anyOf":[{"oneOf":[{"title":"Low","const":"Low"},{"title":"High","const":"High"}]},{"type":"null"}],"x-order":4},
      "deeper":{"anyOf":[{"anyOf":[{"oneOf":[{"title":"Low","const":"Low"},{"title":"High","const":"High"}]}]},{"type":"null"}],"x-order":5},
      "partial":{"type":"string","oneOf":[{"title":"A","const":"A"},{"type":"string"}],"x-order":6},
      "optI64":{"anyOf":[{"$ref":"#/$defs/int64_t"},{"type":"null"}],"x-order":7}},"required":[]})");
    auto const options = [&](std::size_t index) {
        std::vector<std::string> out;
        for (auto const& option : sets.fields()[index].enumOptions) {
            out.push_back(option.label + "=" + option.valueJson);
        }
        return out;
    };
    CHECK(sets.fields()[0].kind == FieldKind::Enum);
    CHECK(sets.fields()[0].title == "Role");
    CHECK(options(0) == std::vector<std::string>{R"(Viewer="Viewer")", R"(Member="Member")", R"(Manager="Manager")"});
    CHECK(options(1) == std::vector<std::string>{"1=1", "2=2", "3=3"});
    CHECK(options(2) == std::vector<std::string>{R"(On="On")", R"(Off="Off")"});
    CHECK(sets.fields()[3].kind == FieldKind::Enum);
    CHECK(options(3) == std::vector<std::string>{R"(Low="Low")", R"(High="High")"});
    CHECK(sets.fields()[4].kind == FieldKind::Enum);
    CHECK(sets.fields()[5].kind != FieldKind::Enum);
    CHECK(sets.fields()[5].enumOptions.empty());
    CHECK(sets.fields()[6].kind == FieldKind::Text);
    CHECK(sets.fields()[7].kind == FieldKind::Integer);
}

TEST_CASE("FormModel: key order is irrelevant, x-order is the layout, required and layout read either way",
          "[forms-engine][model]") {
    FormModel const role = model("SetMemberRole", R"({"type":"object","properties":{
      "role":{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"}],"x-order":2,"title":"Role"},
      "principal":{"type":"string","x-order":1,"title":"Principal"},
      "projectId":{"$ref":"#/$defs/ProjectId","x-order":0,"title":"Project Id"}},
      "$defs":{"ProjectId":{"type":["integer","null"],"minimum":-9223372036854775808,"maximum":9223372036854775807,
               "x-exactMinimum":"-9223372036854775808","x-exactMaximum":"9223372036854775807"}},
      "required":["projectId","principal","role"]})");
    CHECK(names(role.fields()) == "projectId,principal,role");
    CHECK(role.fields()[0].kind == FieldKind::Integer);
    CHECK(role.fields()[0].jsonType == "integer");
    CHECK(role.fields()[0].exactMaximum == "9223372036854775807");
    CHECK(role.fields()[0].minimum == "-9223372036854775808");

    FormModel const reordered = model("X", R"({"properties":{"label":{"type":"string","x-order":1},
      "count":{"type":"integer","x-order":0}},"required":["count","label"]})");
    CHECK(names(reordered.fields()) == "count,label");
    CHECK(reordered.fields()[0].required);
    CHECK(reordered.fields()[1].required);

    FormModel const unbounded =
        model("Unbounded", R"({"properties":{"size":{"type":"integer","minimum":null,"maximum":null}},"required":["size"]})");
    CHECK_FALSE(unbounded.fields()[0].minimum.has_value());
    CHECK_FALSE(unbounded.fields()[0].maximum.has_value());
}

TEST_CASE("FormModel: x-layout buckets fields by x-section in x-order, with a trailing implicit group",
          "[forms-engine][model]") {
    FormModel const layout = model("LayoutProbe", R"({"properties":{
      "sampleId":{"type":["integer","null"],"x-order":0,"x-section":0},
      "density":{"type":["number","null"],"x-order":1,"x-section":1},
      "moisture":{"type":["number","null"],"x-order":2,"x-section":1},
      "notes":{"type":["string","null"],"x-order":3,"x-section":2,"x-colspan":2},
      "remarks":{"type":["string","null"],"x-order":4}},"required":["sampleId"],
      "x-layout":{"groups":[{"title":"Identity","kind":"section","fields":["sampleId"]},
        {"title":"Measurement","kind":"section","fields":["moisture","density"]},
        {"title":"Notes","kind":"accordion","fields":["notes"]}]}})");
    auto const groups = layout.groups();
    REQUIRE(groups.size() == 4);
    CHECK(groups[0].title == "Identity");
    CHECK(groups[0].kind == GroupKind::Section);
    CHECK(groups[0].fields == std::vector<std::string>{"sampleId"});
    CHECK(groups[1].fields == std::vector<std::string>{"density", "moisture"});
    CHECK(groups[2].kind == GroupKind::Accordion);
    CHECK(groups[3].implicit);
    CHECK(groups[3].fields == std::vector<std::string>{"remarks"});
    CHECK(layout.find("notes")->colspan == 2);
    CHECK(layout.find("sampleId")->colspan == 1);

    FormModel const flat = model("Probe", kProbe);
    REQUIRE(flat.groups().size() == 1);
    CHECK(flat.groups()[0].implicit);
    CHECK(flat.groups()[0].fields.size() == 4);
}

TEST_CASE("FormModel: a dependent Choice names its parents", "[forms-engine][model]") {
    FormModel const shipTo = model("ShipTo", R"({"properties":{
      "country":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListCountries","x-optionValue":"id","x-optionLabel":"name"},
      "city":{"type":["integer","null"],"x-order":1,"x-optionsAction":"ListCities","x-optionValue":"id","x-optionLabel":"name","x-optionsDependsOn":["country"]}},
      "required":["country","city"]})");
    CHECK(shipTo.fields()[0].choice->dependsOn.empty());
    CHECK(shipTo.fields()[1].choice->dependsOn == std::vector<std::string>{"country"});
}

TEST_CASE("FormModel: a plain number reads its display unit and decimals; a declared precision wins",
          "[forms-engine][model]") {
    FormModel const reading = model("T_Reading", R"({"$defs":{"double":{"type":"number","minimum":-1.7976931348623157e+308,"maximum":1.7976931348623157e+308}},
      "properties":{"density":{"$ref":"#/$defs/double","x-order":0,"title":"Density","ExtUnits":{"unitAscii":"kg/m³","unitUnicode":"kg/m³"},"x-displayDecimals":3},
      "temperature":{"$ref":"#/$defs/double","x-order":1,"title":"Temperature","ExtUnits":{"unitAscii":"°C","unitUnicode":"°C"}}},"required":["density"]})");
    FieldSpec const& density = reading.fields()[0];
    CHECK(density.kind == FieldKind::Number);
    CHECK(density.unit == "kg/m³");
    CHECK(density.unitAscii == "kg/m³");
    CHECK(density.displayDecimals == 3U);
    CHECK_FALSE(reading.fields()[1].displayDecimals.has_value());
    FormModel const precision =
        model("T_Precision", R"({"properties":{"reading":{"type":"number","x-decimalPlaces":1,"x-displayDecimals":3,"x-order":0}},"required":["reading"]})");
    CHECK(precision.fields()[0].kind == FieldKind::Decimal);
    CHECK(precision.fields()[0].decimalPlaces == 1U);
    CHECK_FALSE(precision.fields()[0].displayDecimals.has_value());
}

TEST_CASE("FormModel: nested objects and collections are described recursively, and a cycle stops",
          "[forms-engine][model]") {
    FormModel const ignition = model("T_Ignition", R"({"$defs":{"double":{"type":"number"},"int32_t":{"type":"integer"}},
      "properties":{"specimen":{"type":"object","x-order":0,"title":"Specimen","required":["massOfContainer","testTemperature"],"properties":{
        "massOfContainer":{"$ref":"#/$defs/double","x-order":0,"title":"Container","ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":1},
        "readoutBinderContent":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":2,"title":"Readout binder content","ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-displayDecimals":2},
        "testTemperature":{"$ref":"#/$defs/int32_t","x-order":3,"title":"Test Temperature"}}}},"required":["specimen"]})");
    FieldSpec const& specimen = ignition.fields()[0];
    CHECK(specimen.kind == FieldKind::Object);
    CHECK_FALSE(specimen.truncated);
    CHECK(names(specimen.children) == "massOfContainer,readoutBinderContent,testTemperature");
    CHECK(specimen.children[0].title == "Container");
    CHECK(specimen.children[0].unit == "g");
    CHECK(specimen.children[0].displayDecimals == 1U);
    CHECK(specimen.children[0].required);
    CHECK(specimen.children[0].path == "specimen.massOfContainer");
    CHECK_FALSE(specimen.children[1].required);
    CHECK(specimen.children[2].kind == FieldKind::Integer);

    FormModel const grading = model("T_Grading", R"({"$defs":{"double":{"type":"number"},
      "Row":{"type":"object","properties":{"note":{"type":["string","null"],"x-order":2},
        "sieve":{"$ref":"#/$defs/double","x-order":0,"title":"Sieve","ExtUnits":{"unitAscii":"mm","unitUnicode":"mm"}},
        "passing":{"type":["object","null"],"properties":{"num":{"type":"integer"},"den":{"type":"integer"},"dp":{"type":"integer"}},"ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-decimalPlaces":1,"x-order":1},
        "order":{"type":"integer","x-order":3,"x-readonly":true}},"required":["sieve","passing"]}},
      "properties":{"rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":0},
        "tags":{"type":"array","items":{"type":"string"},"x-order":1}},"required":["rows"]})");
    FieldSpec const& rows = grading.fields()[0];
    CHECK(rows.kind == FieldKind::ObjectArray);
    CHECK(names(rows.children) == "sieve,passing,note,order");
    CHECK(rows.children[0].path == "rows[].sieve");
    CHECK(rows.children[1].kind == FieldKind::Quantity);
    CHECK(rows.children[3].readOnly);
    CHECK(grading.fields()[1].kind == FieldKind::Array);
    CHECK(grading.fields()[1].children.empty());

    FormModel const tree = model("TreeNodeAction", R"({"$defs":{"TreeNode":{"type":"object","properties":{
        "name":{"type":"string","x-order":0},"children":{"type":"array","items":{"$ref":"#/$defs/TreeNode"},"x-order":1}},
        "required":["name","children"]}},
      "properties":{"name":{"type":"string","x-order":0},"children":{"type":"array","items":{"$ref":"#/$defs/TreeNode"},"x-order":1}},
      "required":["name","children"]})");
    FieldSpec const& children = tree.fields()[1];
    CHECK(children.kind == FieldKind::ObjectArray);
    CHECK_FALSE(children.truncated);
    CHECK(names(children.children) == "name,children");
    CHECK(children.children[1].truncated);  // TreeNode is already on the path
    CHECK(children.children[1].children.empty());
}

TEST_CASE("FormModel: an instance-decorated schema reads its precision and exact range", "[forms-engine][model]") {
    FormModel const decorated = model("ICCapture", R"({"properties":{"value":{"type":["object","null"],
      "ExtUnits":{"unitAscii":"mg_per_L","unitUnicode":"mg/L"},"x-order":1,"x-decimalPlaces":1,
      "x-minimum":{"num":0,"den":1,"dp":1},"x-maximum":{"num":20,"den":1,"dp":1}}},"required":["value"]})");
    FieldSpec const& value = decorated.fields()[0];
    CHECK(value.decimalPlaces == 1U);
    REQUIRE(value.instanceMaximum.has_value());
    CHECK(*value.instanceMaximum == morph::math::Rational{morph::math::Numerator{20}, morph::math::Denominator{1},
                                                          morph::math::DecimalPlaces{1}});
}

TEST_CASE("FormModel: a malformed document is a SchemaError", "[forms-engine][model]") {
    CHECK(FormModel::fromSchema("A", "not json").error().path.empty());
    CHECK_FALSE(FormModel::fromSchema("A", "[1,2]").has_value());
    CHECK(FormModel::fromSchema("A", R"({"properties":[]})").error().path == "properties");
    CHECK(FormModel::fromSchema("A", R"({"properties":{"a":3}})").error().path == "properties.a");
    CHECK(FormModel::fromSchema("A", R"({"x-rules":[{"fields":["a"]}]})").error().path == "x-rules[0]");
    CHECK(FormModel::fromSchema("A", R"({"x-layout":{"groups":{}}})").error().path == "x-layout.groups");
    CHECK(FormModel::fromSchema("A", "{}").has_value());
    CHECK(FormModel::fromSchema("A", R"({"x-submitMode":"explicit"})")->submitMode() ==
          morph::forms::SubmitMode::Explicit);
}

// Only the success path: every document `schemaJson<A>()` emits for a reflectable action is one the
// reader accepts (the conformance and rule corpora pin that), and the reader's failures — not JSON, not
// an object, a non-object property, a malformed `x-rules` or `x-layout` — have no spelling in a C++
// action type, so no registered action can reach the throw. Its message is reviewed with the code.
TEST_CASE("FormModel: forAction reads a registered action's own schema, as fromSchema does",
          "[forms-engine][model]") {
    FormModel const typed = FormModel::forAction<FmBooking>();
    auto const runtime =
        FormModel::fromSchema("Test_FormsEngineModel_Booking", morph::forms::schemaJson<FmBooking>());
    REQUIRE(runtime.has_value());
    CHECK(typed.actionType() == "Test_FormsEngineModel_Booking");
    CHECK(names(typed.fields()) == "guests,note");
    REQUIRE(typed.fields().size() == runtime->fields().size());
    for (std::size_t i = 0; i < typed.fields().size(); ++i) {
        CHECK(typed.fields()[i].name == runtime->fields()[i].name);
        CHECK(typed.fields()[i].kind == runtime->fields()[i].kind);
        CHECK(typed.fields()[i].required == runtime->fields()[i].required);
    }
    CHECK(typed.find("guests")->kind == FieldKind::Integer);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/field_model.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/field_model.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/field_model.hpp
/// @brief `FormModel`: the immutable runtime model of one action's form, read from the schema
///        JSON `schemaJson<A>()` emits (or any document in that vocabulary).
///
/// Specified in `docs/spec/forms/engine.md`, "The field model".

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../core/registry.hpp"
#include "../../util/rational.hpp"
#include "../forms.hpp"
#include "../layout.hpp"
#include "detail/text.hpp"
#include "rules.hpp"

namespace morph::forms {

/// @brief The control a field gets, which the JSON type alone does not say.
enum class FieldKind : std::uint8_t {
    Text,        ///< A string.
    Multiline,   ///< A string edited over several lines (`x-widget: textarea`).
    Integer,     ///< An integer of any width; every digit is kept.
    Number,      ///< A plain JSON number (a `double`/`float` member).
    Decimal,     ///< An exact `{num, den, dp}` with a declared precision and no unit.
    Quantity,    ///< An exact `{num, den, dp}` in a unit, with optional convertible alternatives.
    Boolean,     ///< `true` / `false`.
    Enum,        ///< A closed set the schema states.
    Choice,      ///< A set fetched by executing an options action.
    DateTime,    ///< An instant, entered in the display zone, sent as ISO-8601 UTC.
    Date,        ///< A calendar date, `YYYY-MM-DD`.
    Slider,      ///< A number on a track (`x-widget: slider` with `x-min`/`x-max`).
    Array,       ///< A list of scalars, entered comma-separated.
    Object,      ///< A nested object.
    ObjectArray  ///< A list of nested objects.
};

/// @brief The stable name of a field kind (what `Overrides::byKind` documents and tests print).
/// @param kind The kind.
/// @return `"text"`, `"multiline"`, `"integer"`, `"number"`, `"decimal"`, `"quantity"`, `"boolean"`,
///         `"enum"`, `"choice"`, `"datetime"`, `"date"`, `"slider"`, `"array"`, `"object"` or `"objectArray"`.
[[nodiscard]] constexpr std::string_view fieldKindName(FieldKind kind) noexcept {
    switch (kind) {
        case FieldKind::Text:
            return "text";
        case FieldKind::Multiline:
            return "multiline";
        case FieldKind::Integer:
            return "integer";
        case FieldKind::Number:
            return "number";
        case FieldKind::Decimal:
            return "decimal";
        case FieldKind::Quantity:
            return "quantity";
        case FieldKind::Boolean:
            return "boolean";
        case FieldKind::Enum:
            return "enum";
        case FieldKind::Choice:
            return "choice";
        case FieldKind::DateTime:
            return "datetime";
        case FieldKind::Date:
            return "date";
        case FieldKind::Slider:
            return "slider";
        case FieldKind::Array:
            return "array";
        case FieldKind::Object:
            return "object";
        case FieldKind::ObjectArray:
            return "objectArray";
        default:
            return "text";
    }
}

/// @brief When a form submits.
enum class SubmitMode : std::uint8_t {
    Automatic,  ///< Whenever it becomes ready with a changed body (no `x-submitMode`).
    Explicit    ///< Only on `FormSession::submit()` (`x-submitMode: explicit`).
};

/// @brief How many object levels below the action a schema is described; a deeper member is
///        `FieldSpec::truncated`.
inline constexpr std::size_t kMaxNestingDepth = 4;

/// @brief One unit a Quantity can be entered in; the first of a field's units is canonical.
struct UnitOption {
    /// @brief The unit id (`ExtUnits.unitAscii`, or the alternative's `id`).
    std::string id{};
    /// @brief What a selector shows.
    std::string display{};
    /// @brief Fraction digits accepted in this unit.
    std::uint32_t decimals = 0;
    /// @brief Numerator of the exact unit-to-canonical ratio.
    std::int64_t num = 1;
    /// @brief Denominator of the exact unit-to-canonical ratio.
    std::int64_t den = 1;
    /// @brief Member-wise equality.
    /// @param other The other unit.
    /// @return `true` when every member matches.
    bool operator==(UnitOption const& other) const = default;
};

/// @brief One selectable value of an Enum or a Choice.
struct ChoiceOption {
    /// @brief The value as the JSON literal the body carries (`"\"Viewer\""`, `"9007199254740993"`).
    std::string valueJson{};
    /// @brief What the user sees.
    std::string label{};
    /// @brief Member-wise equality.
    /// @param other The other option.
    /// @return `true` when value and label match.
    bool operator==(ChoiceOption const& other) const = default;
};

/// @brief Where a Choice's options come from (`x-optionsAction` and its companions).
struct ChoiceSpec {
    /// @brief The action whose result rows are the options.
    std::string optionsAction{};
    /// @brief The row member submitted as the value (`x-optionValue`).
    std::string valueField = "id";
    /// @brief The row member shown (`x-optionLabel`).
    std::string labelField = "name";
    /// @brief Sibling wire names whose values parameterise the options request (`x-optionsDependsOn`).
    std::vector<std::string> dependsOn{};
    /// @brief Member-wise equality.
    /// @param other The other descriptor.
    /// @return `true` when every member matches.
    bool operator==(ChoiceSpec const& other) const = default;
};

/// @brief Everything the engine knows about one field, from its schema property.
struct FieldSpec {
    /// @brief The wire name.
    std::string name{};
    /// @brief The path from the action: `name`, `parent.name`, or `rows[].name` for a collection's member.
    std::string path{};
    /// @brief The control and encoder.
    FieldKind kind = FieldKind::Text;
    /// @brief The first declared JSON type (`"integer"`, …), empty when the property declares none.
    std::string jsonType{};
    /// @brief The schema's title, or the wire name.
    std::string title{};
    /// @brief The schema's `description`.
    std::string help{};
    /// @brief `x-placeholder`.
    std::string placeholder{};
    /// @brief `x-i18nKey`: the explicit message-key stem, empty when none is declared.
    std::string i18nKey{};
    /// @brief Named in the containing object's `required`.
    bool required = false;
    /// @brief `x-blankAs` (Text and Multiline only): whether a cleared field submits `""`.
    BlankAs blankAs = BlankAs::Omit;
    /// @brief `x-readonly`.
    bool readOnly = false;
    /// @brief `x-hidden`.
    bool hidden = false;
    /// @brief `x-computed.inputs`: the members a computed field derives from.
    std::vector<std::string> computedInputs{};
    /// @brief `minimum`, as canonical decimal text.
    std::optional<std::string> minimum{};
    /// @brief `maximum`, as canonical decimal text.
    std::optional<std::string> maximum{};
    /// @brief A positive `multipleOf`, as canonical decimal text.
    std::optional<std::string> multipleOf{};
    /// @brief `x-exactMinimum`: an integer bound's exact digits.
    std::optional<std::string> exactMinimum{};
    /// @brief `x-exactMaximum`: an integer bound's exact digits.
    std::optional<std::string> exactMaximum{};
    /// @brief `x-minimum`: a per-instance lower bound (`InstanceConstraints::decorate`).
    std::optional<math::Rational> instanceMinimum{};
    /// @brief `x-maximum`: a per-instance upper bound.
    std::optional<math::Rational> instanceMaximum{};
    /// @brief `x-decimalPlaces`: the declared precision of a Decimal or Quantity.
    std::optional<std::uint32_t> decimalPlaces{};
    /// @brief `x-displayDecimals`: a plain Number's entry and display precision.
    std::optional<std::uint32_t> displayDecimals{};
    /// @brief The display unit (`ExtUnits.unitUnicode`, else `unitAscii`), empty when none.
    std::string unit{};
    /// @brief `ExtUnits.unitAscii`.
    std::string unitAscii{};
    /// @brief A Decimal's or Quantity's entry units: the canonical one first, then `x-unitAlternatives`.
    std::vector<UnitOption> units{};
    /// @brief An Enum's options, in schema order.
    std::vector<ChoiceOption> enumOptions{};
    /// @brief A Choice's options source.
    std::optional<ChoiceSpec> choice{};
    /// @brief `x-section`: the index of the `x-layout` group the field sits in.
    std::optional<std::size_t> group{};
    /// @brief `x-colspan`: grid columns the field spans (at least 1).
    int colspan = 1;
    /// @brief `x-widget`, empty when none.
    std::string widget{};
    /// @brief A Slider's track minimum (`x-min`).
    std::int64_t sliderMin = 0;
    /// @brief A Slider's track maximum (`x-max`).
    std::int64_t sliderMax = 100;
    /// @brief A Slider's increment (`x-step`, at least 1).
    std::int64_t sliderStep = 1;
    /// @brief An Array's item JSON type (`"string"` when undeclared).
    std::string itemType{};
    /// @brief An Object's members, or an ObjectArray's element members, in `x-order`.
    std::vector<FieldSpec> children{};
    /// @brief An Object or ObjectArray whose members are not described: its `$ref` is already on
    ///        the path, it is deeper than `kMaxNestingDepth`, or its schema states no members.
    bool truncated = false;
};

/// @brief One `x-layout` group, or the implicit group of the fields no group names.
struct FieldGroupSpec {
    /// @brief The group's title (empty for the implicit group).
    std::string title{};
    /// @brief Section, tab or accordion; the implicit group is a `Section` with `implicit` set.
    GroupKind kind = GroupKind::Section;
    /// @brief The wire names of the fields in the group, in `x-order`.
    std::vector<std::string> fields{};
    /// @brief The trailing group of fields no declared group names (or every field, without `x-layout`).
    bool implicit = false;
};

namespace detail::engine {

/// @brief Whether @p range holds @p value.
/// @param range A range.
/// @param value The value to look for.
/// @return `true` when an element equals @p value.
[[nodiscard]] inline bool contains(auto const& range, auto const& value) {
    return std::ranges::find(range, value) != std::ranges::end(range);
}

/// @brief Bounds-checked element access for a span, which has no `at()` before C++26.
/// @tparam T The element type.
/// @param items The span.
/// @param index The position.
/// @return The element at @p index.
/// @throws std::out_of_range when @p index is not below `items.size()`.
template <class T>
[[nodiscard]] T& elementAt(std::span<T> items, std::size_t index) {
    if (index >= items.size()) {
        throw std::out_of_range{"morph::forms: index " + std::to_string(index) + " past a span of " +
                                std::to_string(items.size())};
    }
    return *std::next(items.begin(), static_cast<std::ptrdiff_t>(index));
}

/// @brief Reads property schemas: resolves `$ref` into `$defs`, collapses nullable
///        `anyOf`/`oneOf`, and describes an object's properties as `FieldSpec`s.
class SchemaReader {
public:
    /// @param root The whole schema document. Borrowed: it must outlive the reader.
    explicit SchemaReader(Json const& root) : _defs{member(root, "$defs")} {}

    /// @brief Follows a `$ref` into `$defs`; the property's own keys win over the definition's.
    /// @param prop A property schema.
    /// @return The merged object (`{}` for a non-object).
    [[nodiscard]] Json resolveRef(Json const& prop) const {
        if (!prop.is_object()) {
            return emptyObject();
        }
        auto const* const ref = stringAt(prop, "$ref");
        if (ref == nullptr) {
            return prop;
        }
        std::string_view name{*ref};
        name = name.substr(name.rfind('/') + 1);
        Json merged = emptyObject();
        if (Json const* const def = _defs == nullptr ? nullptr : member(*_defs, name); def != nullptr && def->is_object()) {
            merged = *def;
        }
        for (auto const& [key, value] : prop.get_object()) {
            merged.get_object().insert_or_assign(key, value);
        }
        return merged;
    }

    /// @brief Resolves `$ref`, then collapses a nullable `anyOf`/`oneOf` onto its first non-null
    ///        branch; the outer keys win, and the branches' `anyOf`/`oneOf`/`const` are dropped.
    /// @param prop A property schema.
    /// @return The resolved property.
    [[nodiscard]] Json resolveProp(Json const& prop) const {
        Json const resolved = resolveRef(prop);
        Json const* branches = member(resolved, "anyOf");
        if (branches == nullptr || !branches->is_array()) {
            branches = member(resolved, "oneOf");
        }
        if (branches == nullptr || !branches->is_array()) {
            return resolved;
        }
        auto const dropped = [](std::string_view key) { return key == "anyOf" || key == "oneOf" || key == "const"; };
        for (Json const& raw : branches->get_array()) {
            Json const branch = resolveRef(raw);
            if (auto const* const type = stringAt(branch, "type"); type != nullptr && *type == "null") {
                continue;
            }
            Json merged = emptyObject();
            for (auto const& [key, value] : branch.get_object()) {
                if (member(resolved, key) == nullptr && !dropped(key)) {
                    merged.get_object().insert_or_assign(key, value);
                }
            }
            for (auto const& [key, value] : resolved.get_object()) {
                if (!dropped(key)) {
                    merged.get_object().insert_or_assign(key, value);
                }
            }
            return merged;
        }
        return resolved;
    }

    /// @brief Describes an object schema's properties, in `x-order` (stable for ties).
    /// @param objectSchema The object schema.
    /// @param depth        0 for the action, one more per nested level.
    /// @param pathPrefix   Prefix of every member's path (`""`, `"parent."`, `"rows[]."`).
    /// @param refChain     The `$defs` references on the path here.
    /// @return The fields, or the error naming the malformed property.
    // NOLINTNEXTLINE(misc-no-recursion) -- an object's members are described by the same walk, to kMaxNestingDepth
    [[nodiscard]] std::expected<std::vector<FieldSpec>, SchemaError> describe(
        Json const& objectSchema, std::size_t depth, std::string const& pathPrefix,
        std::vector<std::string> const& refChain) const {
        std::vector<FieldSpec> fields;
        Json const* const props = member(objectSchema, "properties");
        if (props == nullptr) {
            return fields;
        }
        if (!props->is_object()) {
            return std::unexpected(SchemaError{pathPrefix + "properties", "\"properties\" must be an object"});
        }
        std::vector<std::string> required;
        if (Json const* const list = member(objectSchema, "required"); list != nullptr && list->is_array()) {
            for (Json const& entry : list->get_array()) {
                if (auto const* const text = entry.get_if<std::string>()) {
                    required.push_back(*text);
                }
            }
        }
        std::vector<std::pair<std::string, std::uint64_t>> order;
        for (auto const& [name, prop] : props->get_object()) {
            order.emplace_back(name, unsignedOf(member(prop, "x-order")).value_or(0));
        }
        std::ranges::stable_sort(order, {}, &std::pair<std::string, std::uint64_t>::second);
        for (auto const& [name, ignoredOrder] : order) {
            static_cast<void>(ignoredOrder);
            Json const& raw = *member(*props, name);
            if (!raw.is_object()) {
                return std::unexpected(
                    SchemaError{"properties." + pathPrefix + name, "a property schema must be a JSON object"});
            }
            auto field = describeOne(raw, name, depth, pathPrefix, refChain, contains(required, name));
            if (!field) {
                return std::unexpected(field.error());
            }
            fields.push_back(*std::move(field));
        }
        return fields;
    }

    /// @brief A non-negative integer member, also from an integral double.
    /// @param value The member, or null.
    /// @return The value, or `std::nullopt`.
    [[nodiscard]] static std::optional<std::uint64_t> unsignedOf(Json const* value) {
        if (value == nullptr) {
            return std::nullopt;
        }
        if (auto const* const asUnsigned = value->get_if<std::uint64_t>()) {
            return *asUnsigned;
        }
        if (auto const* const asSigned = value->get_if<std::int64_t>(); asSigned != nullptr && *asSigned >= 0) {
            return static_cast<std::uint64_t>(*asSigned);
        }
        if (auto const* const asDouble = value->get_if<double>();
            asDouble != nullptr && *asDouble >= 0 && *asDouble == std::floor(*asDouble) && *asDouble < 1.8e19) {
            return static_cast<std::uint64_t>(*asDouble);
        }
        return std::nullopt;
    }

    /// @brief A signed integer member; a double is truncated toward zero.
    /// @param value The member, or null.
    /// @return The value, or `std::nullopt`.
    [[nodiscard]] static std::optional<std::int64_t> signedOf(Json const* value) {
        if (value == nullptr) {
            return std::nullopt;
        }
        if (auto const* const asSigned = value->get_if<std::int64_t>()) {
            return *asSigned;
        }
        if (auto const* const asUnsigned = value->get_if<std::uint64_t>();
            asUnsigned != nullptr && std::in_range<std::int64_t>(*asUnsigned)) {
            return static_cast<std::int64_t>(*asUnsigned);
        }
        if (auto const* const asDouble = value->get_if<double>();
            asDouble != nullptr && std::isfinite(*asDouble) && std::abs(*asDouble) < 9.2e18) {
            return static_cast<std::int64_t>(*asDouble);
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] static std::vector<std::string> jsonTypes(Json const& prop) {
        std::vector<std::string> types;
        Json const* const type = member(prop, "type");
        if (type == nullptr) {
            return types;
        }
        if (auto const* const one = type->get_if<std::string>()) {
            types.push_back(*one);
        } else if (type->is_array()) {
            for (Json const& entry : type->get_array()) {
                if (auto const* const text = entry.get_if<std::string>()) {
                    types.push_back(*text);
                }
            }
        }
        return types;
    }

    /// The key from the raw property first, then from the resolved one (the QML reader's order).
    [[nodiscard]] static Json const* either(Json const& raw, Json const& resolved, std::string_view key) {
        Json const* const own = member(raw, key);
        return own != nullptr ? own : member(resolved, key);
    }

    [[nodiscard]] static std::string textOf(Json const& raw, Json const& resolved, std::string_view key) {
        Json const* const value = either(raw, resolved, key);
        auto const* const text = value == nullptr ? nullptr : value->get_if<std::string>();
        return text == nullptr ? std::string{} : *text;
    }

    [[nodiscard]] static bool flagOf(Json const& raw, Json const& resolved, std::string_view key) {
        Json const* const value = either(raw, resolved, key);
        auto const* const flag = value == nullptr ? nullptr : value->get_if<bool>();
        return flag != nullptr && *flag;
    }

    [[nodiscard]] static std::string refNameOf(Json const& raw) {
        if (auto const* const ref = stringAt(raw, "$ref")) {
            return *ref;
        }
        for (std::string_view const key : {"anyOf", "oneOf"}) {
            if (Json const* const branches = member(raw, key); branches != nullptr && branches->is_array()) {
                for (Json const& branch : branches->get_array()) {
                    if (auto const* const ref = stringAt(branch, "$ref")) {
                        return *ref;
                    }
                }
            }
        }
        return {};
    }

    // NOLINTNEXTLINE(misc-no-recursion) -- one nested level of branches, bounded by `nested`
    [[nodiscard]] std::optional<std::vector<ChoiceOption>> constBranches(Json const& prop, bool nested) const {
        Json const* branches = member(prop, "anyOf");
        if (branches == nullptr || !branches->is_array()) {
            branches = member(prop, "oneOf");
        }
        if (branches == nullptr || !branches->is_array()) {
            return std::nullopt;
        }
        std::vector<ChoiceOption> rows;
        for (Json const& raw : branches->get_array()) {
            Json const branch = resolveRef(raw);
            if (auto const* const type = stringAt(branch, "type"); type != nullptr && *type == "null") {
                continue;
            }
            if (Json const* const literal = member(branch, "const")) {
                auto const* const title = stringAt(branch, "title");
                rows.push_back(ChoiceOption{toJson(*literal), title != nullptr ? *title : displayText(*literal)});
                continue;
            }
            auto inner = nested ? constBranches(branch, false) : std::nullopt;
            if (!inner || inner->empty()) {
                return std::nullopt;
            }
            rows.insert(rows.end(), inner->begin(), inner->end());
        }
        return rows;
    }

    [[nodiscard]] std::vector<ChoiceOption> enumChoices(Json const& raw) const {
        Json const prop = resolveRef(raw);
        if (Json const* const literals = member(prop, "enum"); literals != nullptr && literals->is_array()) {
            std::vector<ChoiceOption> rows;
            for (Json const& literal : literals->get_array()) {
                if (!literal.is_null()) {
                    rows.push_back(ChoiceOption{toJson(literal), displayText(literal)});
                }
            }
            return rows;
        }
        return constBranches(prop, true).value_or(std::vector<ChoiceOption>{});
    }

    [[nodiscard]] static std::optional<math::Rational> rationalOf(Json const* node) {
        if (node == nullptr) {
            return std::nullopt;
        }
        auto const num = signedOf(member(*node, "num"));
        auto const den = signedOf(member(*node, "den"));
        if (!num || !den || *den == 0) {
            return std::nullopt;
        }
        auto const places = unsignedOf(member(*node, "dp")).value_or(0);
        return math::Rational{math::Numerator{*num}, math::Denominator{*den},
                              math::DecimalPlaces{static_cast<std::uint32_t>(std::min<std::uint64_t>(places, math::kMaxDecimalPlaces))}};
    }

    // NOLINTNEXTLINE(misc-no-recursion) -- the other half of describe's walk
    [[nodiscard]] std::expected<FieldSpec, SchemaError> describeOne(Json const& raw, std::string const& name,
                                                                    std::size_t depth, std::string const& pathPrefix,
                                                                    std::vector<std::string> const& refChain,
                                                                    bool required) const {
        Json const prop = resolveProp(raw);
        std::vector<std::string> const types = jsonTypes(prop);
        auto const hasType = [&](std::string_view type) { return contains(types, type); };
        FieldSpec spec{};
        spec.name = name;
        spec.path = pathPrefix + name;
        spec.jsonType = types.empty() ? std::string{} : types.front();
        spec.title = textOf(raw, prop, "title");
        if (spec.title.empty()) {
            spec.title = name;
        }
        spec.help = textOf(prop, prop, "description");
        spec.placeholder = textOf(raw, prop, "x-placeholder");
        spec.i18nKey = textOf(raw, prop, "x-i18nKey");
        spec.required = required;
        spec.readOnly = flagOf(raw, prop, "x-readonly");
        spec.hidden = flagOf(raw, prop, "x-hidden");
        spec.widget = textOf(raw, prop, "x-widget");
        if (Json const* const computed = either(raw, prop, "x-computed")) {
            if (Json const* const inputs = member(*computed, "inputs"); inputs != nullptr && inputs->is_array()) {
                for (Json const& input : inputs->get_array()) {
                    if (auto const* const text = input.get_if<std::string>()) {
                        spec.computedInputs.push_back(*text);
                    }
                }
            }
        }
        if (Json const* const minimum = member(prop, "minimum")) {
            spec.minimum = numberText(*minimum);
        }
        if (Json const* const maximum = member(prop, "maximum")) {
            spec.maximum = numberText(*maximum);
        }
        if (Json const* const step = member(prop, "multipleOf")) {
            auto text = numberText(*step);
            if (text && compareDecimal(*text, "0") == std::strong_ordering::greater) {
                spec.multipleOf = std::move(text);
            }
        }
        if (auto const* const exact = stringAt(prop, "x-exactMinimum"); exact != nullptr && isCanonicalInteger(*exact)) {
            spec.exactMinimum = *exact;
        }
        if (auto const* const exact = stringAt(prop, "x-exactMaximum"); exact != nullptr && isCanonicalInteger(*exact)) {
            spec.exactMaximum = *exact;
        }
        spec.instanceMinimum = rationalOf(either(raw, prop, "x-minimum"));
        spec.instanceMaximum = rationalOf(either(raw, prop, "x-maximum"));
        if (auto const places = unsignedOf(either(raw, prop, "x-decimalPlaces"))) {
            spec.decimalPlaces = static_cast<std::uint32_t>(std::min<std::uint64_t>(*places, math::kMaxDecimalPlaces));
        }
        if (Json const* const units = member(prop, "ExtUnits")) {
            auto const* const ascii = stringAt(*units, "unitAscii");
            auto const* const unicode = stringAt(*units, "unitUnicode");
            spec.unitAscii = ascii != nullptr ? *ascii : std::string{};
            spec.unit = unicode != nullptr && !unicode->empty() ? *unicode : spec.unitAscii;
        }
        if (auto const section = unsignedOf(either(raw, prop, "x-section"))) {
            spec.group = static_cast<std::size_t>(*section);
        }
        spec.colspan = static_cast<int>(std::clamp<std::int64_t>(signedOf(either(raw, prop, "x-colspan")).value_or(1), 1, 1024));

        auto const* const optionsAction = [&]() -> std::string const* {
            Json const* const value = either(raw, prop, "x-optionsAction");
            return value == nullptr ? nullptr : value->get_if<std::string>();
        }();
        std::vector<ChoiceOption> enumRows = optionsAction != nullptr ? std::vector<ChoiceOption>{} : enumChoices(raw);
        auto const format = textOf(prop, prop, "format");
        auto const sliderMin = signedOf(either(raw, prop, "x-min"));
        auto const sliderMax = signedOf(either(raw, prop, "x-max"));
        std::string const refName = refNameOf(raw);
        bool const describesNested = depth < kMaxNestingDepth && (refName.empty() || !contains(refChain, refName));
        std::vector<std::string> nestedChain = refChain;
        if (!refName.empty()) {
            nestedChain.push_back(refName);
        }

        if (hasType("array")) {
            Json const* const rawItems = member(prop, "items");
            Json const items = rawItems == nullptr ? emptyObject() : resolveProp(*rawItems);
            std::vector<std::string> const itemTypes = jsonTypes(items);
            if (contains(itemTypes, "object") || contains(itemTypes, "array")) {
                spec.kind = FieldKind::ObjectArray;
                std::string const itemRef = rawItems == nullptr ? std::string{} : refNameOf(*rawItems);
                bool const itemsDescribed = depth < kMaxNestingDepth && member(items, "properties") != nullptr &&
                                            (itemRef.empty() || !contains(refChain, itemRef));
                if (itemsDescribed) {
                    std::vector<std::string> itemChain = refChain;
                    if (!itemRef.empty()) {
                        itemChain.push_back(itemRef);
                    }
                    auto children = describe(items, depth + 1, spec.path + "[].", itemChain);
                    if (!children) {
                        return std::unexpected(children.error());
                    }
                    spec.children = *std::move(children);
                } else {
                    spec.truncated = true;
                }
            } else {
                spec.kind = FieldKind::Array;
                spec.itemType = itemTypes.empty() ? std::string{"string"} : itemTypes.front();
            }
        } else if (!enumRows.empty()) {
            spec.kind = FieldKind::Enum;
            spec.enumOptions = std::move(enumRows);
        } else if (optionsAction != nullptr) {
            spec.kind = FieldKind::Choice;
            ChoiceSpec choice{};
            choice.optionsAction = *optionsAction;
            if (auto value = textOf(raw, prop, "x-optionValue"); !value.empty()) {
                choice.valueField = std::move(value);
            }
            if (auto label = textOf(raw, prop, "x-optionLabel"); !label.empty()) {
                choice.labelField = std::move(label);
            }
            if (Json const* const parents = either(raw, prop, "x-optionsDependsOn"); parents != nullptr && parents->is_array()) {
                for (Json const& parent : parents->get_array()) {
                    if (auto const* const text = parent.get_if<std::string>()) {
                        choice.dependsOn.push_back(*text);
                    }
                }
            }
            spec.choice = std::move(choice);
        } else if (format == "date-time") {
            spec.kind = FieldKind::DateTime;
        } else if (format == "date") {
            spec.kind = FieldKind::Date;
        } else if (spec.decimalPlaces.has_value()) {
            spec.kind = member(prop, "ExtUnits") != nullptr ? FieldKind::Quantity : FieldKind::Decimal;
            spec.units.push_back(UnitOption{spec.unitAscii, spec.unit, *spec.decimalPlaces, 1, 1});
            if (Json const* const alternatives = either(raw, prop, "x-unitAlternatives");
                spec.kind == FieldKind::Quantity && alternatives != nullptr && alternatives->is_array()) {
                for (Json const& alternative : alternatives->get_array()) {
                    UnitOption option{};
                    option.id = textOf(alternative, alternative, "id");
                    option.display = textOf(alternative, alternative, "display");
                    if (option.display.empty()) {
                        option.display = option.id;
                    }
                    option.decimals = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        unsignedOf(member(alternative, "decimals")).value_or(0), math::kMaxDecimalPlaces));
                    option.num = signedOf(member(alternative, "num")).value_or(1);
                    option.den = signedOf(member(alternative, "den")).value_or(1);
                    if (option.num > 0 && option.den > 0) {
                        spec.units.push_back(std::move(option));
                    }
                }
            }
        } else if ((hasType("integer") || hasType("number")) && spec.widget == "slider" && sliderMin && sliderMax) {
            spec.kind = FieldKind::Slider;
            spec.sliderMin = *sliderMin;
            spec.sliderMax = *sliderMax;
            spec.sliderStep = std::max<std::int64_t>(1, signedOf(either(raw, prop, "x-step")).value_or(1));
        } else if (hasType("integer")) {
            spec.kind = FieldKind::Integer;
        } else if (hasType("boolean")) {
            spec.kind = FieldKind::Boolean;
        } else if (hasType("number")) {
            spec.kind = FieldKind::Number;
            if (auto const decimals = unsignedOf(either(raw, prop, "x-displayDecimals"))) {
                spec.displayDecimals =
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(*decimals, math::kMaxDecimalPlaces));
            }
        } else if (hasType("object")) {
            spec.kind = FieldKind::Object;
            if (describesNested && member(prop, "properties") != nullptr) {
                auto children = describe(prop, depth + 1, spec.path + ".", nestedChain);
                if (!children) {
                    return std::unexpected(children.error());
                }
                spec.children = *std::move(children);
            } else {
                spec.truncated = true;
            }
        } else if (spec.widget == "textarea") {
            spec.kind = FieldKind::Multiline;
        } else {
            spec.kind = FieldKind::Text;
        }
        if ((spec.kind == FieldKind::Text || spec.kind == FieldKind::Multiline) &&
            textOf(raw, prop, "x-blankAs") == "empty") {
            spec.blankAs = BlankAs::Empty;
        }
        return spec;
    }

    Json const* _defs;
};

}  // namespace detail::engine

/// @brief The immutable runtime model of one action's form.
///
/// Built from schema JSON by `fromSchema`; `forAction<A>()` (which the typed facade `Form<A>`
/// uses) reads `schemaJson<A>()` through the same reader, so a typed and a runtime form of one
/// action are the same model.
class FormModel {
public:
    /// @brief Reads a schema document.
    /// @param actionType The action type the form submits (the registered type id).
    /// @param schemaJson The schema, in the vocabulary `schemaJson<A>()` emits.
    /// @return The model, or the error naming where the document is malformed.
    [[nodiscard]] static std::expected<FormModel, SchemaError> fromSchema(std::string_view actionType,
                                                                          std::string_view schemaJson) {
        using detail::engine::Json;
        auto const dom = detail::engine::parseJson(schemaJson);
        if (!dom) {
            return std::unexpected(SchemaError{"", "the schema is not valid JSON"});
        }
        if (!dom->is_object()) {
            return std::unexpected(SchemaError{"", "the schema is not a JSON object"});
        }
        FormModel model{};
        model._actionType = actionType;
        auto const* const title = detail::engine::stringAt(*dom, "title");
        model._title = title != nullptr ? *title : std::string{actionType};
        detail::engine::SchemaReader const reader{*dom};
        auto fields = reader.describe(*dom, 0, "", {});
        if (!fields) {
            return std::unexpected(fields.error());
        }
        model._fields = *std::move(fields);
        if (auto groups = readGroups(*dom, model._fields); groups) {
            model._groups = *std::move(groups);
        } else {
            return std::unexpected(groups.error());
        }
        if (Json const* const rules = detail::engine::member(*dom, "x-rules")) {
            if (!rules->is_array()) {
                return std::unexpected(SchemaError{"x-rules", "\"x-rules\" must be an array"});
            }
            std::size_t index = 0;
            for (Json const& node : rules->get_array()) {
                auto parsed = RuleExpr::parse(node, "x-rules[" + std::to_string(index++) + "]");
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                model._rules.push_back(*std::move(parsed));
            }
        }
        auto const* const mode = detail::engine::stringAt(*dom, "x-submitMode");
        model._submitMode = (mode != nullptr && *mode == "explicit") ? SubmitMode::Explicit : SubmitMode::Automatic;
        return model;
    }

    /// @brief The model of a registered action's own form: `fromSchema` over
    ///        `ActionTraits<A>::typeId()` and `schemaJson<A>()`.
    ///
    /// The schema is compiled from `A`, so a reader error here is a defect in the emitter or the
    /// engine, never bad input: it throws rather than returning an `expected` every caller would
    /// have to unwrap.
    /// @tparam A The action type (registered with `BRIDGE_REGISTER_ACTION` or declared with
    ///           `BRIDGE_DECLARE_ACTION`).
    /// @return The model.
    /// @throws std::logic_error naming the action id and the error's path and message when the
    ///         engine cannot read the action's schema.
    template <class A>
    [[nodiscard]] static FormModel forAction() {
        std::string_view const typeId = model::ActionTraits<A>::typeId();
        auto parsed = fromSchema(typeId, schemaJson<A>());
        if (!parsed) {
            throw std::logic_error{"FormModel::forAction: the schema of '" + std::string{typeId} +
                                   "' does not read as a form (" + parsed.error().path + ": " +
                                   parsed.error().message + ")"};
        }
        return *std::move(parsed);
    }

    /// @brief The action type the form submits.
    /// @return The type id passed to `fromSchema`.
    [[nodiscard]] std::string_view actionType() const noexcept { return _actionType; }

    /// @brief The schema's top-level `title`, or the action type.
    /// @return The title.
    [[nodiscard]] std::string_view title() const noexcept { return _title; }

    /// @brief The top-level fields, in `x-order`.
    /// @return The fields.
    [[nodiscard]] std::span<FieldSpec const> fields() const noexcept { return _fields; }

    /// @brief The layout groups: the declared `x-layout` groups, then an implicit group of the
    ///        fields none names; without `x-layout`, one implicit group of every field.
    /// @return The groups.
    [[nodiscard]] std::span<FieldGroupSpec const> groups() const noexcept { return _groups; }

    /// @brief The parsed `x-rules`, in declaration order.
    /// @return The rules.
    [[nodiscard]] std::span<RuleExpr const> rules() const noexcept { return _rules; }

    /// @brief When the form submits.
    /// @return `Explicit` for `x-submitMode: explicit`, else `Automatic`.
    [[nodiscard]] SubmitMode submitMode() const noexcept { return _submitMode; }

    /// @brief A top-level field by wire name.
    /// @param name The wire name.
    /// @return The field, or null.
    [[nodiscard]] FieldSpec const* find(std::string_view name) const noexcept {
        auto const found = std::ranges::find(_fields, name, &FieldSpec::name);
        return found == _fields.end() ? nullptr : &*found;
    }

private:
    FormModel() = default;

    [[nodiscard]] static std::expected<std::vector<FieldGroupSpec>, SchemaError> readGroups(
        detail::engine::Json const& dom, std::vector<FieldSpec> const& fields) {
        using detail::engine::Json;
        std::vector<FieldGroupSpec> groups;
        Json const* const layout = detail::engine::member(dom, "x-layout");
        Json const* const declared = layout == nullptr ? nullptr : detail::engine::member(*layout, "groups");
        if (declared != nullptr && !declared->is_array()) {
            return std::unexpected(SchemaError{"x-layout.groups", "\"groups\" must be an array"});
        }
        if (declared != nullptr) {
            for (Json const& node : declared->get_array()) {
                FieldGroupSpec group{};
                if (auto const* const title = detail::engine::stringAt(node, "title")) {
                    group.title = *title;
                }
                auto const* const kind = detail::engine::stringAt(node, "kind");
                group.kind = kind == nullptr       ? GroupKind::Section
                             : *kind == "tab"       ? GroupKind::Tab
                             : *kind == "accordion" ? GroupKind::Accordion
                                                    : GroupKind::Section;
                groups.push_back(std::move(group));
            }
        }
        FieldGroupSpec trailing{};
        trailing.implicit = true;
        for (FieldSpec const& field : fields) {
            if (field.group.has_value() && *field.group < groups.size()) {
                groups.at(*field.group).fields.push_back(field.name);
            } else {
                trailing.fields.push_back(field.name);
            }
        }
        if (!trailing.fields.empty()) {
            groups.push_back(std::move(trailing));
        }
        return groups;
    }

    std::string _actionType{};
    std::string _title{};
    std::vector<FieldSpec> _fields{};
    std::vector<FieldGroupSpec> _groups{};
    std::vector<RuleExpr> _rules{};
    SubmitMode _submitMode = SubmitMode::Automatic;
};

}  // namespace morph::forms
```

`contains` is the helper defined at the top of `detail::engine` in this header rather than
`std::ranges::contains`, which the libc++ emscripten ships does not have (see the comment at
`include/morph/forms/forms.hpp:2788`).

Register the header and the test file as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][model]"`
Expected: PASS, 12 test cases.

Mutation check: in `SchemaReader::describe`, replace `std::ranges::stable_sort(order, …)` with nothing (keep the
document's key order). Expected: FAIL in "key order is irrelevant, x-order is the layout…" (`"count,label"` becomes
`"label,count"`). Restore it. Then make `forAction` pass `"X"` instead of `typeId` to `fromSchema`. Expected: FAIL in
"forAction reads a registered action's own schema…" (`actionType()` is `"X"`). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/field_model.hpp tests/test_forms_engine_model.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): FormModel, the field model read from schema JSON

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: Encoders and decoders (`forms/engine/values.hpp`)

Pure functions over a `FieldSpec`: a draft in, an `Encoding` out, byte-exact; a wire value in, a draft out. The
session (Task 6) composes them; nothing here is reactive.

**Files:**
- Create: `include/morph/forms/engine/values.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/field_model.hpp`, add
  `include/morph/forms/engine/values.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_values.cpp` after `test_forms_engine_model.cpp`
- Test: `tests/test_forms_engine_values.cpp`

**Interfaces:**
- Consumes: Task 1's `math::parseDecimal`; Task 2's text helpers; Task 3's `Scalar`, `DecimalText`; Task 4's
  `FieldSpec`, `FieldKind`, `UnitOption`, `ChoiceSpec`, `ChoiceOption`; `render::NumericLocale`,
  `render::normalizeLocaleNumber`, `render::formatCanonicalNumber` (`include/morph/render/locale_format.hpp`);
  `render::TranslationProvider`, `render::resolveText` (`include/morph/render/i18n.hpp`); `math::checkedMul`,
  `math::checkedDiv`, `math::detail::mulOverflows`, `math::detail::powerOfTen` (`include/morph/util/rational.hpp`).
- Produces (`morph::forms`):
  - `FieldError{code, message}`; `WireValue{json, scalar}`; `Encoding =
    std::optional<std::expected<WireValue, FieldError>>` (`nullopt` = blank).
  - `ValueContext{locale, displayOffsetMinutes, translations, bcp47}` — the session's options a value
    conversion needs.
  - `encodeScalar(FieldSpec const&, std::string_view draft, std::size_t unit, ValueContext const&) -> Encoding`
    for every kind but Object and ObjectArray.
  - `decodeScalar(FieldSpec const&, detail::engine::Json const& value, ValueContext const&) -> std::string`.
  - `convertDraft(std::string_view draft, UnitOption const& from, UnitOption const& to, render::NumericLocale
    const&) -> std::optional<std::string>`.
  - `parseOptions(std::string_view reply, ChoiceSpec const&) -> std::vector<ChoiceOption>`.
  - `defaultPlaceholder(FieldSpec const&) -> std::string`; `scalarOf(detail::engine::Json const&) -> Scalar`;
    `fieldError(std::string_view code, ValueContext const&) -> FieldError`.
  - Error codes: `malformed`, `too-precise`, `below-minimum`, `above-maximum`, `not-a-multiple`, `not-an-option`,
    `out-of-range`, `incomplete`, `unrepresentable`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_values.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/values.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using morph::forms::Encoding;
using morph::forms::FieldSpec;
using morph::forms::FormModel;
using morph::forms::ValueContext;
namespace engine = morph::forms::detail::engine;

/// One field described from a one-property schema.
[[nodiscard]] FieldSpec field(std::string_view propertyJson, std::string_view defs = "{}") {
    std::string const schema = R"({"$defs":)" + std::string{defs} + R"(,"properties":{"f":)" + std::string{propertyJson} +
                               R"(},"required":["f"]})";
    auto model = FormModel::fromSchema("T", schema);
    REQUIRE(model.has_value());
    return model->fields().front();
}

[[nodiscard]] std::string json(Encoding const& encoding) {
    REQUIRE(encoding.has_value());
    INFO((encoding->has_value() ? std::string{} : encoding->error().code));
    REQUIRE(encoding->has_value());
    return (*encoding)->json;
}

[[nodiscard]] std::string error(Encoding const& encoding) {
    REQUIRE(encoding.has_value());
    REQUIRE_FALSE(encoding->has_value());
    return encoding->error().code;
}

ValueContext const kContext{};
ValueContext const kGerman{.locale = {.decimalSeparator = ",", .groupSeparator = "."}};

constexpr std::string_view kMass = R"({"$ref":"#/$defs/q","x-decimalPlaces":3,"x-unitAlternatives":[
  {"id":"g","display":"g","decimals":1,"num":1,"den":1000},{"id":"t","display":"t","decimals":4,"num":1000,"den":1}]})";
constexpr std::string_view kMassDefs = R"({"q":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}})";
constexpr std::string_view kInt64Defs = R"({"int64_t":{"type":"integer","minimum":-9223372036854775808,
  "maximum":9223372036854775807,"x-exactMinimum":"-9223372036854775808","x-exactMaximum":"9223372036854775807"}})";

}  // namespace

TEST_CASE("values: a Quantity keeps the unreduced scaled digits the QML renderer sent", "[forms-engine][values]") {
    FieldSpec const mass = field(kMass, kMassDefs);
    CHECK(json(morph::forms::encodeScalar(mass, "2650.5", 0, kContext)) == R"({"num":2650500,"den":1000,"dp":3})");
    CHECK(json(morph::forms::encodeScalar(mass, "2650500.0", 1, kContext)) == R"({"num":26505000,"den":10000,"dp":3})");
    CHECK(json(morph::forms::encodeScalar(mass, "1.050,25", 0, kGerman)) == R"({"num":1050250,"den":1000,"dp":3})");
    CHECK(error(morph::forms::encodeScalar(mass, "2650.5001", 0, kContext)) == "too-precise");
    CHECK(error(morph::forms::encodeScalar(mass, "1.5", 0, kGerman)) == "malformed");
    FieldSpec const tenth = field(R"({"type":"object","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"x"}})");
    CHECK(json(morph::forms::encodeScalar(tenth, "-0.5", 0, kContext)) == R"({"num":-5,"den":10,"dp":1})");
    CHECK(json(morph::forms::encodeScalar(tenth, "123456789012345678.9", 0, kContext)) ==
          R"({"num":1234567890123456789,"den":10,"dp":1})");
    CHECK(error(morph::forms::encodeScalar(tenth, "1234567890123456789.9", 0, kContext)) == "out-of-range");
    FieldSpec const decimal = field(R"({"type":"object","x-decimalPlaces":2})");
    CHECK(json(morph::forms::encodeScalar(decimal, "3.5", 0, kContext)) == R"({"num":350,"den":100,"dp":2})");
}

TEST_CASE("values: Quantity bounds, instance bounds and multipleOf are checked exactly in the canonical unit",
          "[forms-engine][values]") {
    FieldSpec const budget = field(R"({"type":"object","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"count"},"minimum":1,"multipleOf":1})");
    CHECK(error(morph::forms::encodeScalar(budget, "0", 0, kContext)) == "below-minimum");
    CHECK(error(morph::forms::encodeScalar(budget, "2.5", 0, kContext)) == "not-a-multiple");
    CHECK(json(morph::forms::encodeScalar(budget, "2", 0, kContext)) == R"({"num":20,"den":10,"dp":1})");
    FieldSpec const step = field(R"({"type":"object","x-decimalPlaces":2,"multipleOf":0.5,"ExtUnits":{"unitAscii":"u"}})");
    CHECK((*morph::forms::encodeScalar(step, "1.5", 0, kContext)).has_value());
    CHECK(error(morph::forms::encodeScalar(step, "0.75", 0, kContext)) == "not-a-multiple");
    FieldSpec const decorated = field(R"({"type":["object","null"],"ExtUnits":{"unitAscii":"mg_per_L"},"x-decimalPlaces":1,
      "x-minimum":{"num":0,"den":1,"dp":1},"x-maximum":{"num":20,"den":1,"dp":1}})");
    CHECK((*morph::forms::encodeScalar(decorated, "12.5", 0, kContext)).has_value());
    CHECK(error(morph::forms::encodeScalar(decorated, "25", 0, kContext)) == "above-maximum");
    CHECK(error(morph::forms::encodeScalar(decorated, "-1", 0, kContext)) == "below-minimum");
    CHECK(error(morph::forms::encodeScalar(decorated, "12.55", 0, kContext)) == "too-precise");
    FieldSpec const capped = field(R"({"$ref":"#/$defs/q","x-decimalPlaces":3,"maximum":2000,
      "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}]})", kMassDefs);
    CHECK(error(morph::forms::encodeScalar(capped, "2000000.1", 1, kContext)) == "above-maximum");  // checked in kg
}

TEST_CASE("values: unit conversion is exact and rounds half away from zero", "[forms-engine][values]") {
    using morph::forms::convertDraft;
    using morph::forms::UnitOption;
    UnitOption const kilograms{"kg", "kg", 3, 1, 1};
    UnitOption const gram{"g", "g", 1, 1, 1000};
    UnitOption const tonne{"t", "t", 4, 1000, 1};
    CHECK(convertDraft("2650.5", kilograms, gram, {}) == "2650500.0");
    CHECK(convertDraft("2650500.0", gram, kilograms, {}) == "2650.500");
    CHECK(convertDraft("2650.5", kilograms, tonne, {}) == "2.6505");
    CHECK(convertDraft("2.6505", tonne, gram, {}) == "2650500.0");
    CHECK(convertDraft("-0.001", kilograms, gram, {}) == "-1.0");
    CHECK(convertDraft("0.05", UnitOption{"a", "a", 2, 1, 1}, UnitOption{"b", "b", 1, 1, 1}, {}) == "0.1");
    CHECK(convertDraft("junk", kilograms, gram, {}) == std::nullopt);
    CHECK(convertDraft("2650,5", kilograms, gram, {.decimalSeparator = ",", .groupSeparator = "."}) == "2.650.500,0");
}

TEST_CASE("values: an integer keeps every digit and prefers the exact bound", "[forms-engine][values]") {
    FieldSpec const wideId = field(R"({"$ref":"#/$defs/int64_t"})", kInt64Defs);
    CHECK(json(morph::forms::encodeScalar(wideId, "9223372036854775807", 0, kContext)) == "9223372036854775807");
    CHECK(json(morph::forms::encodeScalar(wideId, "-9223372036854775808", 0, kContext)) == "-9223372036854775808");
    CHECK(error(morph::forms::encodeScalar(wideId, "9223372036854775808", 0, kContext)) == "above-maximum");
    CHECK(error(morph::forms::encodeScalar(wideId, "-9223372036854775809", 0, kContext)) == "below-minimum");
    CHECK(error(morph::forms::encodeScalar(wideId, "99999999999999999999", 0, kContext)) == "above-maximum");
    CHECK(json(morph::forms::encodeScalar(wideId, "0009223372036854775807", 0, kContext)) == "9223372036854775807");
    CHECK(json(morph::forms::encodeScalar(wideId, "-42", 0, kContext)) == "-42");
    CHECK(error(morph::forms::encodeScalar(wideId, "banana", 0, kContext)) == "malformed");
    FieldSpec const small = field(R"({"type":"integer","minimum":-10,"maximum":10})");
    CHECK((*morph::forms::encodeScalar(small, "10", 0, kContext)).has_value());
    CHECK(error(morph::forms::encodeScalar(small, "11", 0, kContext)) == "above-maximum");
    CHECK(error(morph::forms::encodeScalar(small, "-11", 0, kContext)) == "below-minimum");
    FieldSpec const unbounded = field(R"({"type":"integer"})");
    CHECK(json(morph::forms::encodeScalar(unbounded, "18446744073709551615", 0, kContext)) == "18446744073709551615");
    auto const scalar = (*morph::forms::encodeScalar(wideId, "9007199254740993", 0, kContext))->scalar;
    CHECK(std::get<morph::forms::DecimalText>(scalar).canonical == "9007199254740993");
}

TEST_CASE("values: a plain number is a JSON number of the digits typed, gated by its bounds",
          "[forms-engine][values]") {
    constexpr std::string_view kDefs = R"({"double":{"type":"number","minimum":-1.7976931348623157e+308,"maximum":1.7976931348623157e+308},
      "float":{"type":"number","minimum":-3.4028234663852886e+38,"maximum":3.4028234663852886e+38}})";
    FieldSpec const ratio = field(R"({"$ref":"#/$defs/double"})", kDefs);
    FieldSpec const score = field(R"({"$ref":"#/$defs/float"})", kDefs);
    CHECK(json(morph::forms::encodeScalar(ratio, "3.5", 0, kContext)) == "3.5");
    CHECK(json(morph::forms::encodeScalar(ratio, "4", 0, kContext)) == "4");
    CHECK(json(morph::forms::encodeScalar(ratio, "-0.25", 0, kContext)) == "-0.25");
    CHECK(json(morph::forms::encodeScalar(ratio, "007.50", 0, kContext)) == "7.50");
    for (std::string_view const text : {"abc", "3.", ".5", "1e5", "--1", "1 2", "0x10", "3,5"}) {
        INFO("accepted " << text);
        CHECK(error(morph::forms::encodeScalar(ratio, text, 0, kContext)) == "malformed");
    }
    ValueContext const grouped{.locale = {.groupSeparator = ","}};
    CHECK(json(morph::forms::encodeScalar(ratio, "1,000.5", 0, grouped)) == "1000.5");
    std::string const huge = "100000000000000000000000000000000000000000";
    CHECK(error(morph::forms::encodeScalar(score, huge, 0, kContext)) == "above-maximum");
    CHECK(json(morph::forms::encodeScalar(ratio, huge, 0, kContext)) == huge);
    FieldSpec const temperature = field(R"({"type":"number","minimum":-40.5,"maximum":120,"multipleOf":0.5})");
    CHECK((*morph::forms::encodeScalar(temperature, "-40.5", 0, kContext)).has_value());
    CHECK(error(morph::forms::encodeScalar(temperature, "-41", 0, kContext)) == "below-minimum");
    CHECK(error(morph::forms::encodeScalar(temperature, "120.5", 0, kContext)) == "above-maximum");
    CHECK(error(morph::forms::encodeScalar(temperature, "0.25", 0, kContext)) == "not-a-multiple");
    CHECK(json(morph::forms::encodeScalar(temperature, "0.5", 0, kContext)) == "0.5");
    FieldSpec const density = field(R"({"type":"number","x-displayDecimals":3})");
    CHECK(json(morph::forms::encodeScalar(density, "2.505", 0, kContext)) == "2.505");
    CHECK(error(morph::forms::encodeScalar(density, "2.5051", 0, kContext)) == "too-precise");
    FieldSpec const whole = field(R"({"type":"number","x-displayDecimals":0})");
    CHECK(json(morph::forms::encodeScalar(whole, "12", 0, kContext)) == "12");
    CHECK(error(morph::forms::encodeScalar(whole, "12.5", 0, kContext)) == "too-precise");
}

TEST_CASE("values: booleans, enums, choices, dates and arrays encode to their wire shapes", "[forms-engine][values]") {
    FieldSpec const flag = field(R"({"type":"boolean"})");
    CHECK(json(morph::forms::encodeScalar(flag, "true", 0, kContext)) == "true");
    CHECK(json(morph::forms::encodeScalar(flag, "", 0, kContext)) == "false");  // required: an unchecked box answers
    FieldSpec optionalFlag = flag;
    optionalFlag.required = false;
    CHECK_FALSE(morph::forms::encodeScalar(optionalFlag, "", 0, kContext).has_value());
    CHECK(error(morph::forms::encodeScalar(flag, "banana", 0, kContext)) == "malformed");

    FieldSpec const role = field(R"({"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},{"title":"Manager","const":"Manager"}]})");
    CHECK(json(morph::forms::encodeScalar(role, R"("Manager")", 0, kContext)) == R"("Manager")");
    CHECK(error(morph::forms::encodeScalar(role, R"("Emperor")", 0, kContext)) == "not-an-option");
    CHECK(error(morph::forms::encodeScalar(role, "Manager", 0, kContext)) == "not-an-option");

    FieldSpec const slot = field(R"({"type":"integer","x-optionsAction":"ListSlots"})");
    CHECK(json(morph::forms::encodeScalar(slot, "9007199254740993", 0, kContext)) == "9007199254740993");
    CHECK(error(morph::forms::encodeScalar(slot, "{not json", 0, kContext)) == "malformed");

    FieldSpec const when = field(R"({"type":"string","format":"date-time"})");
    CHECK(json(morph::forms::encodeScalar(when, "2026-07-05T14:30", 0, kContext)) == R"("2026-07-05T14:30:00Z")");
    CHECK(json(morph::forms::encodeScalar(when, "2026-07-05T16:30:00", 0, ValueContext{.displayOffsetMinutes = 120})) ==
          R"("2026-07-05T14:30:00Z")");
    CHECK(error(morph::forms::encodeScalar(when, "not-a-date", 0, kContext)) == "malformed");
    FieldSpec const day = field(R"({"type":"string","format":"date"})");
    CHECK(json(morph::forms::encodeScalar(day, "2026-07-20", 0, kContext)) == R"("2026-07-20")");
    CHECK(error(morph::forms::encodeScalar(day, "2026-07-32", 0, kContext)) == "malformed");

    FieldSpec const tags = field(R"({"type":"array","items":{"type":"string"}})");
    CHECK(json(morph::forms::encodeScalar(tags, "red, green, blue", 0, kContext)) == R"(["red","green","blue"])");
    CHECK(json(morph::forms::encodeScalar(tags, "  red ,, green ,   ", 0, kContext)) == R"(["red","green"])");
    CHECK(json(morph::forms::encodeScalar(tags, " , , ", 0, kContext)) == "[]");
    FieldSpec const counts = field(R"({"type":"array","items":{"type":"integer"}})");
    CHECK(json(morph::forms::encodeScalar(counts, "1, 2, 30", 0, kContext)) == "[1,2,30]");
    CHECK(error(morph::forms::encodeScalar(counts, "1, x", 0, kContext)) == "malformed");

    FieldSpec const text = field(R"({"type":"string"})");
    CHECK(json(morph::forms::encodeScalar(text, "  say \"hi\"  ", 0, kContext)) == R"("say \"hi\"")");
}

TEST_CASE("values: decoding is the inverse of encoding, per kind", "[forms-engine][values]") {
    auto const decode = [](FieldSpec const& spec, std::string_view value, ValueContext const& context = kContext) {
        auto const dom = engine::parseJson(value);
        REQUIRE(dom.has_value());
        return morph::forms::decodeScalar(spec, *dom, context);
    };
    FieldSpec const density = field(R"({"type":["object","null"],"ExtUnits":{"unitAscii":"kg_per_m3"},"x-decimalPlaces":2})");
    CHECK(decode(density, R"({"num":5,"den":4,"dp":2})") == "1.25");
    CHECK(decode(density, R"({"num":-1,"den":3,"dp":2})") == "-0.33");
    CHECK(decode(density, R"({"num":245050,"den":100,"dp":2})") == "2450.50");
    CHECK(decode(density, R"({"num":245050,"den":100,"dp":2})", kGerman) == "2450,50");
    CHECK(decode(density, R"("oops")").empty());
    FieldSpec const temperature = field(R"({"type":"number"})");
    CHECK(decode(temperature, "1e-7") == "0.0000001");
    CHECK(decode(temperature, "3") == "3");
    CHECK(decode(temperature, "21.5", kGerman) == "21,5");
    FieldSpec const padded = field(R"({"type":"number","x-displayDecimals":2})");
    CHECK(decode(padded, "3") == "3.00");
    CHECK(decode(padded, "21.5") == "21.50");
    CHECK(decode(padded, "1.23456") == "1.23456");
    CHECK(decode(field(R"({"type":"boolean"})"), "false") == "false");
    CHECK(decode(field(R"({"type":"string","oneOf":[{"const":"A"},{"const":"B"}]})"), R"("A")") == R"("A")");
    FieldSpec const takenAt = field(R"({"type":"string","format":"date-time"})");
    CHECK(decode(takenAt, R"("2026-01-01T00:30:00+01:00")") == "2025-12-31T23:30:00");
    CHECK(decode(takenAt, R"("2026-07-20T09:00:00Z")", ValueContext{.displayOffsetMinutes = 120}) ==
          "2026-07-20T11:00:00");
    CHECK(decode(field(R"({"type":"integer"})"), "9007199254740993") == "9007199254740993");
    CHECK(decode(field(R"({"type":"array","items":{"type":"string"}})"), R"(["a","b"])") == "a, b");
    CHECK(decode(field(R"({"type":"string"})"), "null").empty());
}

TEST_CASE("values: option rows are the reply array or its first array member, ids exact", "[forms-engine][values]") {
    morph::forms::ChoiceSpec const spec{.optionsAction = "ListRows"};
    CHECK(morph::forms::parseOptions(R"([{"id":1,"name":"One"}])", spec).size() == 1);
    CHECK(morph::forms::parseOptions(R"({"rows":[{"id":1},{"id":2}]})", spec).size() == 2);
    CHECK(morph::forms::parseOptions(R"({"nothing":1})", spec).empty());
    CHECK(morph::forms::parseOptions("not json", spec).empty());
    auto const big = morph::forms::parseOptions(
        R"({"rows":[{"id":9007199254740993,"name":"Alpha"},{"id":9007199254740992,"name":"Beta"}]})", spec);
    REQUIRE(big.size() == 2);
    CHECK(big[0].valueJson == "9007199254740993");
    CHECK(big[1].valueJson == "9007199254740992");
    CHECK(big[0].label == "Alpha");
    morph::forms::ChoiceSpec const byId{.optionsAction = "ListRows", .labelField = "id"};
    CHECK(morph::forms::parseOptions(R"([{"id":9007199254740993}])", byId).front().label == "9007199254740993");
}

TEST_CASE("values: placeholders follow the kind and the declared precision", "[forms-engine][values]") {
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"number","x-displayDecimals":3})")) == "0.000");
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"number","x-displayDecimals":0})")) == "0");
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"number"})")).empty());
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"integer"})")) == "0");
    CHECK(morph::forms::defaultPlaceholder(field(kMass, kMassDefs)) == "0.000");
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"string","x-placeholder":"e.g. 1050"})")) == "e.g. 1050");
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"array","items":{"type":"string"}})")) ==
          "comma-separated (e.g. red, green, blue)");
}

TEST_CASE("values: an error message resolves through the translation provider", "[forms-engine][values]") {
    ValueContext const translated{.translations =
                                      [](std::string_view key, std::string_view locale) -> std::optional<std::string> {
                                          if (key == "morph.forms.error.malformed" && locale == "de") {
                                              return "Ungültiger Wert";
                                          }
                                          return std::nullopt;
                                      },
                                  .bcp47 = "de"};
    auto const encoding = morph::forms::encodeScalar(field(R"({"type":"integer"})"), "x", 0, translated);
    REQUIRE(encoding.has_value());
    CHECK(encoding->error().message == "Ungültiger Wert");
    CHECK(morph::forms::fieldError("too-precise", kContext).message == "Too many decimal places");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/values.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/values.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/values.hpp
/// @brief The forms engine's encoders and decoders: a field's draft text to its exact wire
///        literal, a wire value back to the draft a control shows, exact unit conversion, and
///        Choice option rows.
///
/// Specified in `docs/spec/forms/engine.md`, "Values and exactness".

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../render/i18n.hpp"
#include "../../render/locale_format.hpp"
#include "../../util/rational.hpp"
#include "detail/text.hpp"
#include "field_model.hpp"
#include "rules.hpp"

namespace morph::forms {

/// @brief Why a draft does not encode: a stable code and the message shown beside the field.
struct FieldError {
    /// @brief One of `malformed`, `too-precise`, `below-minimum`, `above-maximum`, `not-a-multiple`,
    ///        `not-an-option`, `out-of-range`, `incomplete`, `unrepresentable`.
    std::string code{};
    /// @brief The message, resolved through the session's translations
    ///        (key `morph.forms.error.<code>`), else the English text.
    std::string message{};
    /// @brief Member-wise equality.
    /// @param other The other error.
    /// @return `true` when code and message match.
    bool operator==(FieldError const& other) const = default;
};

/// @brief One field's encoded value.
struct WireValue {
    /// @brief The JSON literal the body carries, byte for byte.
    std::string json{};
    /// @brief The exact value rules compare (`std::monostate` for arrays and objects).
    Scalar scalar{};
    /// @brief Member-wise equality.
    /// @param other The other value.
    /// @return `true` when literal and scalar match.
    bool operator==(WireValue const& other) const = default;
};

/// @brief A draft's encoding: `std::nullopt` when blank, else the value or why it does not encode.
using Encoding = std::optional<std::expected<WireValue, FieldError>>;

/// @brief The session options a value conversion reads.
struct ValueContext {
    /// @brief How numbers are typed and shown. The views must outlive the context.
    render::NumericLocale locale{};
    /// @brief The display zone for date-times, in minutes east of UTC.
    int displayOffsetMinutes = 0;
    /// @brief The host's catalog, for error messages; empty means none.
    render::TranslationProvider translations{};
    /// @brief The locale to resolve messages in.
    std::string bcp47 = "en";
};

/// @brief The error for @p code, its message resolved through @p context's translations.
/// @param code    A `FieldError::code`.
/// @param context The session's value context.
/// @return The error.
[[nodiscard]] inline FieldError fieldError(std::string_view code, ValueContext const& context) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kEnglish{{
        {"malformed", "Not a valid value"},
        {"too-precise", "Too many decimal places"},
        {"below-minimum", "Below the minimum"},
        {"above-maximum", "Above the maximum"},
        {"not-a-multiple", "Not an allowed step"},
        {"not-an-option", "Not one of the options"},
        {"out-of-range", "Too large to send exactly"},
        {"incomplete", "A required part is missing"},
        {"unrepresentable", "Nested too deeply to edit here"},
    }};
    auto const found = std::ranges::find(kEnglish, code, &std::pair<std::string_view, std::string_view>::first);
    std::string_view const english = found == kEnglish.end() ? std::string_view{"Not a valid value"} : found->second;
    std::string const key = "morph.forms.error." + std::string{code};
    return FieldError{std::string{code}, render::resolveText(context.translations, context.bcp47, std::nullopt, key, english)};
}

/// @brief The scalar a JSON literal denotes, for rules: a boolean, a string, or a decimal.
/// @param value The literal.
/// @return The scalar; `std::monostate` for null, arrays and objects.
[[nodiscard]] inline Scalar scalarOf(detail::engine::Json const& value) {
    if (auto const* const flag = value.get_if<bool>()) {
        return *flag;
    }
    if (auto const* const text = value.get_if<std::string>()) {
        return *text;
    }
    if (auto number = detail::engine::numberText(value)) {
        return DecimalText{*std::move(number)};
    }
    return std::monostate{};
}

namespace detail::engine {

/// @brief A canonical decimal in @p locale's spelling, without grouping — what a control is seeded with.
/// @param canonical Canonical decimal text.
/// @param locale    The display locale.
/// @return The draft text.
[[nodiscard]] inline std::string draftNumber(std::string_view canonical, render::NumericLocale const& locale) {
    render::NumericLocale ungrouped = locale;
    ungrouped.groupSeparator = {};
    return render::formatCanonicalNumber(canonical, ungrouped);
}

/// @brief Checks @p canonical against a field's `minimum`, `maximum` and `multipleOf`.
/// @param spec      The field.
/// @param canonical The value as canonical decimal text.
/// @param context   For the message.
/// @return The violation, or `std::nullopt`.
[[nodiscard]] inline std::optional<FieldError> decimalBounds(FieldSpec const& spec, std::string_view canonical,
                                                             ValueContext const& context) {
    if (spec.minimum && compareDecimal(canonical, *spec.minimum) == std::strong_ordering::less) {
        return fieldError("below-minimum", context);
    }
    if (spec.maximum && compareDecimal(canonical, *spec.maximum) == std::strong_ordering::greater) {
        return fieldError("above-maximum", context);
    }
    if (spec.multipleOf) {
        auto const value = math::parseDecimal(canonical, render::NumericLocale{});
        auto const step = math::parseDecimal(*spec.multipleOf, render::NumericLocale{});
        auto const quotient = (value && step) ? math::checkedDiv(*value, *step) : std::nullopt;
        if (!quotient || !quotient->isInteger()) {
            return fieldError("not-a-multiple", context);
        }
    }
    return std::nullopt;
}

/// @brief Encodes an integer draft.
/// @param spec    The field.
/// @param text    The trimmed draft.
/// @param context For messages.
/// @return The encoding.
[[nodiscard]] inline Encoding encodeInteger(FieldSpec const& spec, std::string_view text, ValueContext const& context) {
    if (!isCanonicalInteger(text)) {
        return std::unexpected(fieldError("malformed", context));
    }
    std::string const normalised = stripLeadingZeros(text);
    auto const lower = spec.exactMinimum ? spec.exactMinimum : spec.minimum;
    auto const upper = spec.exactMaximum ? spec.exactMaximum : spec.maximum;
    if (lower && compareDecimal(normalised, *lower) == std::strong_ordering::less) {
        return std::unexpected(fieldError("below-minimum", context));
    }
    if (upper && compareDecimal(normalised, *upper) == std::strong_ordering::greater) {
        return std::unexpected(fieldError("above-maximum", context));
    }
    if (spec.multipleOf) {
        if (auto const violation = decimalBounds(FieldSpec{.multipleOf = spec.multipleOf}, normalised, context)) {
            return std::unexpected(*violation);
        }
    }
    return WireValue{normalised, DecimalText{normalised}};
}

/// @brief Encodes a plain JSON number draft.
/// @param spec    The field.
/// @param text    The trimmed draft.
/// @param context The locale and messages.
/// @return The encoding.
[[nodiscard]] inline Encoding encodeNumber(FieldSpec const& spec, std::string_view text, ValueContext const& context) {
    auto const canonical = render::normalizeLocaleNumber(text, context.locale);
    if (!canonical || !isCanonicalDecimal(*canonical)) {
        return std::unexpected(fieldError("malformed", context));
    }
    auto const dot = canonical->find('.');
    std::size_t const places = dot == std::string::npos ? 0 : canonical->size() - dot - 1;
    if (spec.displayDecimals && places > *spec.displayDecimals) {
        return std::unexpected(fieldError("too-precise", context));
    }
    if (auto const violation = decimalBounds(spec, *canonical, context)) {
        return std::unexpected(*violation);
    }
    std::string const literal = stripLeadingZeros(*canonical);
    return WireValue{literal, DecimalText{literal}};
}

/// @brief Encodes a Decimal or Quantity draft typed in `spec.units[unit]` as the exact
///        `{num, den, dp}` in the canonical unit, unreduced: the scaled digits times the unit's
///        numerator over `10^decimals` times its denominator.
/// @param spec    The field.
/// @param text    The trimmed draft.
/// @param unit    Index into `spec.units`.
/// @param context The locale and messages.
/// @return The encoding.
[[nodiscard]] inline Encoding encodeExact(FieldSpec const& spec, std::string_view text, std::size_t unit,
                                          ValueContext const& context) {
    UnitOption const fallback{spec.unitAscii, spec.unit, spec.decimalPlaces.value_or(0), 1, 1};
    UnitOption const& entry = unit < spec.units.size() ? spec.units.at(unit) : fallback;
    auto const canonical = render::normalizeLocaleNumber(text, context.locale);
    if (!canonical || !isCanonicalDecimal(*canonical)) {
        return std::unexpected(fieldError("malformed", context));
    }
    auto const dot = canonical->find('.');
    std::size_t const places = dot == std::string::npos ? 0 : canonical->size() - dot - 1;
    if (places > entry.decimals) {
        return std::unexpected(fieldError("too-precise", context));
    }
    auto const typed = math::parseDecimal(*canonical, render::NumericLocale{}, entry.decimals);
    std::int64_t const scale = math::detail::powerOfTen(entry.decimals);
    if (!typed || scale == 0) {
        return std::unexpected(fieldError("out-of-range", context));
    }
    std::int64_t const factor = scale / typed->denominator;
    if (math::detail::mulOverflows(typed->numerator, factor)) {
        return std::unexpected(fieldError("out-of-range", context));
    }
    std::int64_t const scaled = typed->numerator * factor;
    if (math::detail::mulOverflows(scaled, entry.num) || math::detail::mulOverflows(scale, entry.den)) {
        return std::unexpected(fieldError("out-of-range", context));
    }
    std::int64_t const num = scaled * entry.num;
    std::int64_t const den = scale * entry.den;
    std::uint32_t const canonDp = spec.decimalPlaces.value_or(0);
    math::Rational const value{math::Numerator{num}, math::Denominator{den}, math::DecimalPlaces{canonDp}};
    if (spec.minimum && compareRationalToDecimal(value, *spec.minimum) == std::strong_ordering::less) {
        return std::unexpected(fieldError("below-minimum", context));
    }
    if (spec.maximum && compareRationalToDecimal(value, *spec.maximum) == std::strong_ordering::greater) {
        return std::unexpected(fieldError("above-maximum", context));
    }
    if (spec.instanceMinimum && value < *spec.instanceMinimum) {
        return std::unexpected(fieldError("below-minimum", context));
    }
    if (spec.instanceMaximum && value > *spec.instanceMaximum) {
        return std::unexpected(fieldError("above-maximum", context));
    }
    if (spec.multipleOf) {
        auto const step = math::parseDecimal(*spec.multipleOf, render::NumericLocale{});
        auto const quotient = step ? math::checkedDiv(value, *step) : std::nullopt;
        if (!quotient || !quotient->isInteger()) {
            return std::unexpected(fieldError("not-a-multiple", context));
        }
    }
    std::string const json = R"({"num":)" + std::to_string(num) + R"(,"den":)" + std::to_string(den) + R"(,"dp":)" +
                             std::to_string(canonDp) + "}";
    return WireValue{json, value};
}

/// @brief Encodes a comma-separated array draft by the item type.
/// @param spec    The field.
/// @param text    The trimmed draft.
/// @param context For messages.
/// @return The encoding; blank items are dropped, so `" , "` is `[]`.
[[nodiscard]] inline Encoding encodeArray(FieldSpec const& spec, std::string_view text, ValueContext const& context) {
    std::string json = "[";
    bool first = true;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto const comma = text.find(',', start);
        std::string_view const item = trim(text.substr(start, comma == std::string_view::npos ? text.npos : comma - start));
        start = comma == std::string_view::npos ? text.size() + 1 : comma + 1;
        if (item.empty()) {
            continue;
        }
        std::string literal;
        if (spec.itemType == "integer") {
            if (!isCanonicalInteger(item)) {
                return std::unexpected(fieldError("malformed", context));
            }
            literal = stripLeadingZeros(item);
        } else if (spec.itemType == "number") {
            if (!isCanonicalDecimal(item)) {
                return std::unexpected(fieldError("malformed", context));
            }
            literal = stripLeadingZeros(item);
        } else if (spec.itemType == "boolean") {
            if (item != "true" && item != "false") {
                return std::unexpected(fieldError("malformed", context));
            }
            literal = std::string{item};
        } else {
            literal = quote(item);
        }
        json += (first ? "" : ",") + literal;
        first = false;
    }
    json += "]";
    return WireValue{json, std::monostate{}};
}

}  // namespace detail::engine

/// @brief Encodes one scalar field's draft (every kind but `Object` and `ObjectArray`).
///
/// The draft is trimmed first. Blank is `std::nullopt` — except a required Boolean, which encodes
/// `false`: an unchecked box is an answer, and a required box always shows one.
/// @param spec    The field.
/// @param draft   What the user typed, or what a control holds (an option's `valueJson`, `true`/`false`).
/// @param unit    For a Decimal or Quantity, the index into `spec.units` the draft is typed in.
/// @param context The locale, display zone and translations.
/// @return The encoding.
[[nodiscard]] inline Encoding encodeScalar(FieldSpec const& spec, std::string_view draft, std::size_t unit,
                                           ValueContext const& context) {
    using namespace detail::engine;
    std::string_view const text = trim(draft);
    if (text.empty()) {
        if (spec.kind == FieldKind::Boolean && spec.required) {
            return WireValue{"false", false};
        }
        return std::nullopt;
    }
    switch (spec.kind) {
        case FieldKind::Text:
        case FieldKind::Multiline:
            return WireValue{quote(text), std::string{text}};
        case FieldKind::Date:
            if (!isIsoDate(text)) {
                return std::unexpected(fieldError("malformed", context));
            }
            return WireValue{quote(text), std::string{text}};
        case FieldKind::DateTime: {
            auto const wall = parseWallClock(text);
            if (!wall) {
                return std::unexpected(fieldError("malformed", context));
            }
            std::string const utc = formatUtc(*wall - std::chrono::minutes{context.displayOffsetMinutes});
            return WireValue{quote(utc), utc};
        }
        case FieldKind::Boolean:
            if (text == "true" || text == "false") {
                return WireValue{std::string{text}, text == "true"};
            }
            return std::unexpected(fieldError("malformed", context));
        case FieldKind::Enum: {
            auto const found = std::ranges::find(spec.enumOptions, text, &ChoiceOption::valueJson);
            if (found == spec.enumOptions.end()) {
                return std::unexpected(fieldError("not-an-option", context));
            }
            auto const literal = parseJson(text);
            return WireValue{std::string{text}, literal ? scalarOf(*literal) : Scalar{}};
        }
        case FieldKind::Choice: {
            auto const literal = parseJson(text);
            if (!literal || literal->is_object() || literal->is_array() || literal->is_null()) {
                return std::unexpected(fieldError("malformed", context));
            }
            return WireValue{std::string{text}, scalarOf(*literal)};
        }
        case FieldKind::Integer:
            return encodeInteger(spec, text, context);
        case FieldKind::Slider:
            return spec.jsonType == "number" ? encodeNumber(spec, text, context) : encodeInteger(spec, text, context);
        case FieldKind::Number:
            return encodeNumber(spec, text, context);
        case FieldKind::Decimal:
        case FieldKind::Quantity:
            return encodeExact(spec, text, unit, context);
        case FieldKind::Array:
            return encodeArray(spec, text, context);
        default:
            return std::unexpected(fieldError("unrepresentable", context));
    }
}

/// @brief The draft a control shows for the wire value @p value — the inverse of `encodeScalar`,
///        so encoding the result re-encodes @p value (numbers in the display locale, without
///        grouping; instants in the display zone).
/// @param spec    The field.
/// @param value   The wire value (JSON null and a mismatched shape decode to blank).
/// @param context The locale and display zone.
/// @return The draft text.
[[nodiscard]] inline std::string decodeScalar(FieldSpec const& spec, detail::engine::Json const& value,
                                              ValueContext const& context) {
    using namespace detail::engine;
    if (value.is_null()) {
        return {};
    }
    switch (spec.kind) {
        case FieldKind::Text:
        case FieldKind::Multiline:
        case FieldKind::Date: {
            auto const* const text = value.get_if<std::string>();
            return text == nullptr ? std::string{} : *text;
        }
        case FieldKind::DateTime: {
            auto const* const text = value.get_if<std::string>();
            auto const instant = text == nullptr ? std::nullopt : parseInstant(*text);
            return instant ? formatWallClock(*instant + std::chrono::minutes{context.displayOffsetMinutes})
                           : std::string{};
        }
        case FieldKind::Boolean: {
            auto const* const flag = value.get_if<bool>();
            return flag == nullptr ? std::string{} : (*flag ? "true" : "false");
        }
        case FieldKind::Enum:
        case FieldKind::Choice:
            return toJson(value);
        case FieldKind::Integer:
        case FieldKind::Slider:
            return numberText(value).value_or(std::string{});
        case FieldKind::Number: {
            auto canonical = numberText(value);
            if (!canonical) {
                return {};
            }
            if (spec.displayDecimals) {
                auto const dot = canonical->find('.');
                std::size_t const places = dot == std::string::npos ? 0 : canonical->size() - dot - 1;
                if (places < *spec.displayDecimals) {
                    if (dot == std::string::npos) {
                        *canonical += '.';
                    }
                    canonical->append(*spec.displayDecimals - places, '0');
                }
            }
            return draftNumber(*canonical, context.locale);
        }
        case FieldKind::Decimal:
        case FieldKind::Quantity: {
            auto const num = SchemaReader::signedOf(member(value, "num"));
            auto const den = SchemaReader::signedOf(member(value, "den"));
            if (!num || !den || *den == 0) {
                return {};
            }
            math::Rational const exact{math::Numerator{*num}, math::Denominator{*den}, math::DecimalPlaces{0}};
            return draftNumber(decimalText(exact, spec.decimalPlaces.value_or(0)), context.locale);
        }
        case FieldKind::Array: {
            if (!value.is_array()) {
                return {};
            }
            std::string out;
            for (Json const& item : value.get_array()) {
                out += (out.empty() ? "" : ", ") + displayText(item);
            }
            return out;
        }
        default:
            return {};
    }
}

/// @brief Converts a draft from one unit to another exactly, rounding half away from zero to
///        the target unit's decimals.
/// @param draft  The draft, in @p locale's spelling.
/// @param from   The unit it is typed in.
/// @param target The unit to convert to.
/// @param locale The display locale (the result is grouped in it).
/// @return The converted draft, or `std::nullopt` when the draft is not a number or the
///         conversion leaves the exact range.
[[nodiscard]] inline std::optional<std::string> convertDraft(std::string_view draft, UnitOption const& from,
                                                             UnitOption const& target, render::NumericLocale const& locale) {
    auto const typed = math::parseDecimal(detail::engine::trim(draft), locale);
    if (!typed || from.den == 0 || target.num == 0) {
        return std::nullopt;
    }
    math::Rational const toCanonical{math::Numerator{from.num}, math::Denominator{from.den}, math::DecimalPlaces{0}};
    math::Rational const fromCanonical{math::Numerator{target.den}, math::Denominator{target.num}, math::DecimalPlaces{0}};
    auto const canonical = math::checkedMul(*typed, toCanonical);
    auto const converted = canonical ? math::checkedMul(*canonical, fromCanonical) : canonical;
    if (!converted) {
        return std::nullopt;
    }
    return render::formatCanonicalNumber(detail::engine::decimalText(*converted, target.decimals), locale);
}

/// @brief The options an options action's reply carries: the reply itself when it is an array,
///        else its first array-valued member; rows without the value member are skipped.
/// @param reply The reply JSON.
/// @param spec  Which row members are value and label.
/// @return The options; empty for a reply that is not JSON or has no rows.
[[nodiscard]] inline std::vector<ChoiceOption> parseOptions(std::string_view reply, ChoiceSpec const& spec) {
    using namespace detail::engine;
    std::vector<ChoiceOption> options{};
    auto const dom = parseJson(reply);
    if (!dom) {
        return options;
    }
    Json const* rows = dom->is_array() ? &*dom : nullptr;
    if (rows == nullptr && dom->is_object()) {
        for (auto const& [key, candidate] : dom->get_object()) {
            if (candidate.is_array()) {
                rows = &candidate;
                break;
            }
        }
    }
    if (rows == nullptr) {
        return options;
    }
    for (Json const& row : rows->get_array()) {
        Json const* const value = member(row, spec.valueField);
        if (value == nullptr) {
            continue;
        }
        Json const* const label = member(row, spec.labelField);
        options.push_back(ChoiceOption{toJson(*value), displayText(label != nullptr ? *label : *value)});
    }
    return options;
}

/// @brief The placeholder a control shows when the schema declares none.
/// @param spec The field.
/// @return `x-placeholder` when declared; else `0.000…` for a Decimal or Quantity (at least one
///         place), `0` for an integer, the declared precision for a Number, a hint for an Array,
///         and empty otherwise.
[[nodiscard]] inline std::string defaultPlaceholder(FieldSpec const& spec) {
    if (!spec.placeholder.empty()) {
        return spec.placeholder;
    }
    switch (spec.kind) {
        case FieldKind::Decimal:
        case FieldKind::Quantity:
            return "0." + std::string(std::max<std::uint32_t>(1, spec.decimalPlaces.value_or(0)), '0');
        case FieldKind::Integer:
        case FieldKind::Slider:
            return "0";
        case FieldKind::Number:
            if (!spec.displayDecimals) {
                return {};
            }
            return *spec.displayDecimals == 0 ? std::string{"0"} : "0." + std::string(*spec.displayDecimals, '0');
        case FieldKind::Array:
            return "comma-separated (e.g. red, green, blue)";
        default:
            return {};
    }
}

}  // namespace morph::forms
```

Add `#include <algorithm>`, `#include <chrono>` and `#include <compare>` to the include list. Register the header
and the test as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][values]"`
Expected: PASS, 10 test cases.

Mutation check: in `encodeExact`, replace `std::int64_t const den = scale * entry.den;` with
`std::int64_t const den = scale;`. Expected: FAIL in "a Quantity keeps the unreduced scaled digits…" (the gram
literal's `"den"` becomes `10`). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/values.hpp tests/test_forms_engine_values.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): exact encoders and decoders for every scalar field kind

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: `FormSession` and `FieldState` — drafts, readiness, body, submission, prefill and reset

**Files:**
- Create: `include/morph/forms/engine/form_session.hpp`
- Create: `tests/forms_engine_support.hpp` (the fake server and harness every later engine test uses)
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/values.hpp`, add
  `include/morph/forms/engine/form_session.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_session.cpp` after `test_forms_engine_values.cpp`
- Test: `tests/test_forms_engine_session.cpp`

**Interfaces:**
- Consumes: Part 1 — `reactive::Runtime` (`batch`, `untracked`, `owner()`), `reactive::Signal<T>` (`get`, `peek`,
  `set`, `mutate`), `reactive::Computed<T>` (`get`, `peek`), `reactive::Effect`, `reactive::Query<A, R>(Runtime&,
  Fetch, Key)` (`pending`, `value`, `error`), `reactive::Mutation<A, R>(Runtime&, Run)` (`run`, `pending`, `error`,
  `lastResult`); `async::Completion<T>`; Tasks 2–5; `forms::i18n::fieldKey`, `explicitFieldKey`, `groupKey`,
  `FieldSlot` (`include/morph/forms/i18n.hpp`); `render::resolveText`.
- Produces (`morph::forms`), exactly:
  - `Submitter`, `ChoiceFetcher` (the contract's aliases).
  - `FormSessionOptions{locale, bcp47, translations, displayOffsetMinutes}` plus the addition
    `std::function<bool(std::string_view bodyJson)> accepts` — an extra readiness gate over the assembled body
    (`Form<A>` puts `ActionValidator<A>` there).
  - `FieldState` (non-copyable, non-movable): `spec()`, `path()`, `label()`, `help()`, `placeholder()`,
    `text() -> reactive::Signal<std::string>&`, `unit() -> reactive::Signal<std::size_t>&`,
    `engaged() -> reactive::Signal<bool>&`, `encoded() -> reactive::Computed<Encoding> const&`,
    `error() -> reactive::Computed<std::optional<FieldError>> const&`, `visible()`, `readonly()`, `required()`
    (each `-> reactive::Computed<bool> const&`), `blank() -> bool` (tracked), `ruleValue() -> RuleValue` (tracked),
    `options() -> std::vector<ChoiceOption> const&` (tracked), `optionsPending() -> bool` (tracked),
    `switchUnit(std::size_t)`, `load(detail::engine::Json const*)`, `clear()`, `start()`. Task 7 adds the nested
    members and rows, Task 8 the Choice options.
  - `FormSession`: the contract's members — `FormSession(reactive::Runtime&, FormModel, Submitter, ChoiceFetcher,
    FormSessionOptions = {})`, `model()`, `field(std::string_view path) -> FieldState&`, `ready()`, `body()`,
    `submit()`, `prefill(std::string_view)`, `reset()`, `pending()`, `lastReply()`, `lastError()` — plus the
    additions `runtime()`, `fields()`, `assign(std::string_view path, std::string_view jsonValue)`,
    `groupCollapsed(std::size_t) -> reactive::Signal<bool>&`, `tabSelection(std::size_t) ->
    reactive::Signal<std::size_t>&`, `groupTitle(std::size_t) -> std::string`, `submitMode()`.
- Paths accepted by `field()`: a top-level wire name (Task 7 adds `parent.member` and `rows[i].member`).

How the pieces fit:

- **A field's draft** is `text` (what the control holds), `unit` (a Quantity's entry unit) and `engaged` (a
  blank-capable field was filled once: an `x-blankAs: empty` field, or a collection that had rows). `encoded` is the
  pure encoder over those (Task 5), so a field's error, a rule's operand and the body all read one computed.
- **Readiness** is: every required field (static or by `requiredWhen`) encodes, no field has an error, no gating
  rule is False (Task 3), and `accepts(body)` when set. `body()` is the assembled JSON while ready, else `nullopt`.
- **Submission** is a `reactive::Mutation<std::string, std::string>` over the `Submitter`. In automatic mode an
  Effect submits whenever the form is ready with a body different from the last one submitted or seen. A
  programmatic change — `prefill`, `assign`, `reset`, clearing a stale Choice — ends by recording the body it
  produced as seen (`body` is a pull-based computed, so it is current inside the batch), and the Effect, finding
  that body when it runs, does not submit it; a user edit after it, even in the same flush, produces a different
  body and does. A form that is ready as constructed submits once, as the QML renderer's `Component.onCompleted` did.
- **Two-phase construction**: every `FieldState` is built first, then `start()`ed, because a Choice's options key
  reads its parents, which may come later in `x-order`.

- [ ] **Step 1: Write the failing test**

Create `tests/forms_engine_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms_engine_support.hpp
/// @brief A fake server for the forms engine's Submitter and ChoiceFetcher, and the harness
///        (owner, runtime, server) every engine test builds sessions in.

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/runtime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace morph::test::formsengine {

/// @brief One call the engine made.
struct Call {
    std::string action{};
    std::string body{};
};

/// @brief Records submits and options fetches and answers them on the owner.
///
/// A responder returning a value resolves the call at once (delivered on the next drain); one
/// returning `std::nullopt` leaves it pending until `resolve…`/`reject…` settles it by index.
class FakeServer {
public:
    using Responder = std::function<std::optional<std::string>(Call const&)>;

    explicit FakeServer(exec::IExecutor& owner) : _owner{&owner} {}

    void replyWith(Responder responder) { _submitResponder = std::move(responder); }
    void optionsWith(Responder responder) { _fetchResponder = std::move(responder); }

    [[nodiscard]] forms::Submitter submitter() {
        return [this](std::string_view action, std::string body) {
            return record(_submits, _submitPromises, _submitResponder, action, std::move(body));
        };
    }

    [[nodiscard]] forms::ChoiceFetcher fetcher() {
        return [this](std::string_view action, std::string body) {
            return record(_fetches, _fetchPromises, _fetchResponder, action, std::move(body));
        };
    }

    [[nodiscard]] std::vector<Call> const& submits() const noexcept { return _submits; }
    [[nodiscard]] std::vector<Call> const& fetches() const noexcept { return _fetches; }
    [[nodiscard]] std::string const& lastBody() const { return _submits.back().body; }

    void resolveSubmit(std::size_t index, std::string reply) { _submitPromises.at(index)->resolve(std::move(reply)); }
    void rejectSubmit(std::size_t index, std::string const& message) {
        _submitPromises.at(index)->reject(std::make_exception_ptr(std::runtime_error{message}));
    }
    void resolveFetch(std::size_t index, std::string reply) { _fetchPromises.at(index)->resolve(std::move(reply)); }
    void rejectFetch(std::size_t index, std::string const& message) {
        _fetchPromises.at(index)->reject(std::make_exception_ptr(std::runtime_error{message}));
    }

private:
    using Promise = async::Completion<std::string>::Promise;

    async::Completion<std::string> record(std::vector<Call>& calls, std::vector<std::shared_ptr<Promise>>& promises,
                                          Responder const& responder, std::string_view action, std::string body) {
        calls.push_back(Call{std::string{action}, std::move(body)});
        auto [completion, promise] = async::Completion<std::string>::makeSettleable(_owner);
        auto const shared = std::make_shared<Promise>(std::move(promise));
        promises.push_back(shared);
        if (responder) {
            if (auto reply = responder(calls.back())) {
                shared->resolve(*std::move(reply));
            }
        }
        return std::move(completion);
    }

    exec::IExecutor* _owner;
    Responder _submitResponder = [](Call const&) { return std::optional<std::string>{R"({"ok":true})"}; };
    Responder _fetchResponder = [](Call const&) { return std::optional<std::string>{"[]"}; };
    std::vector<Call> _submits{};
    std::vector<Call> _fetches{};
    std::vector<std::shared_ptr<Promise>> _submitPromises{};
    std::vector<std::shared_ptr<Promise>> _fetchPromises{};
};

/// @brief Reads a schema that must be well-formed.
[[nodiscard]] inline forms::FormModel modelOf(std::string_view action, std::string_view schema) {
    auto model = forms::FormModel::fromSchema(action, schema);
    INFO((model ? std::string{} : model.error().path + ": " + model.error().message));
    REQUIRE(model.has_value());
    return *std::move(model);
}

/// @brief The owner, runtime and server a test builds sessions over. Declare sessions after it.
struct Harness {
    testing::StepExecutor owner{};
    reactive::Runtime rt{owner};
    FakeServer server{owner};

    [[nodiscard]] std::unique_ptr<forms::FormSession> session(std::string_view action, std::string_view schema,
                                                              forms::FormSessionOptions options = {}) {
        return std::make_unique<forms::FormSession>(rt, modelOf(action, schema), server.submitter(), server.fetcher(),
                                                    std::move(options));
    }

    /// @brief Runs every posted flush and delivery.
    void settle() { owner.runAll(); }

    /// @brief A user edit: sets a field's draft, then settles.
    void type(forms::FormSession& session, std::string_view path, std::string_view text) {
        session.field(path).text().set(std::string{text});
        settle();
    }
};

}  // namespace morph::test::formsengine
```

Create `tests/test_forms_engine_session.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::FormSessionOptions;
using morph::test::formsengine::Harness;

constexpr std::string_view kProbe = R"({"type":"object","properties":{
  "slot":{"type":["integer","null"],"x-order":0},
  "mass":{"$ref":"#/$defs/q","x-order":1,"x-decimalPlaces":3,
          "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}]},
  "when":{"type":["string","null"],"format":"date-time","x-order":2}},
  "$defs":{"q":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}},
  "required":["slot","mass","when"]})";

constexpr std::string_view kTwoInts =
    R"({"properties":{"a":{"type":"integer","x-order":0},"b":{"type":"integer","x-order":1}},"required":["a","b"]})";

}  // namespace

TEST_CASE("FormSession: readiness gates on required fields, precision and date syntax; the body is byte-exact",
          "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("Probe", kProbe);
    CHECK_FALSE(form->ready());
    harness.type(*form, "slot", "4");
    harness.type(*form, "when", "2026-07-05T14:30");
    CHECK_FALSE(form->ready());
    harness.type(*form, "mass", "2650.5");
    REQUIRE(form->ready());
    CHECK(form->body() == R"({"slot":4,"mass":{"num":2650500,"den":1000,"dp":3},"when":"2026-07-05T14:30:00Z"})");
    harness.type(*form, "mass", "2650.5001");
    CHECK_FALSE(form->ready());
    CHECK_FALSE(form->body().has_value());
    CHECK(form->field("mass").error().get()->code == "too-precise");
    harness.type(*form, "mass", "2650.5");
    harness.type(*form, "when", "not-a-date");
    CHECK_FALSE(form->ready());
    harness.type(*form, "when", "2026-07-05T14:30:15");
    CHECK(form->ready());
    form->field("mass").switchUnit(1);
    harness.settle();
    CHECK(form->field("mass").text().peek() == "2650500.0");
    CHECK(form->body() == R"({"slot":4,"mass":{"num":26505000,"den":10000,"dp":3},"when":"2026-07-05T14:30:15Z"})");
}

TEST_CASE("FormSession: automatic mode submits once when the body becomes valid, and again only when it changes",
          "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("TestAction", kTwoInts);
    harness.type(*form, "a", "3");
    CHECK(harness.server.submits().empty());
    harness.type(*form, "b", "4");
    REQUIRE(harness.server.submits().size() == 1);
    CHECK(harness.server.submits()[0].action == "TestAction");
    CHECK(harness.server.lastBody() == R"({"a":3,"b":4})");
    CHECK(form->lastReply() == R"({"ok":true})");
    harness.type(*form, "b", "4");
    CHECK(harness.server.submits().size() == 1);
    harness.type(*form, "b", "5");
    CHECK(harness.server.submits().size() == 2);
}

TEST_CASE("FormSession: explicit mode submits only on submit(), and only while ready", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session(
        "CFR_BookRoom",
        R"({"properties":{"name":{"type":"string","x-order":0}},"required":["name"],"x-submitMode":"explicit"})");
    CHECK(form->model().submitMode() == morph::forms::SubmitMode::Explicit);
    form->submit();
    harness.settle();
    CHECK(harness.server.submits().empty());
    harness.type(*form, "name", "Alice");
    CHECK(form->ready());
    CHECK(harness.server.submits().empty());
    form->submit();
    harness.settle();
    REQUIRE(harness.server.submits().size() == 1);
    CHECK(harness.server.lastBody() == *form->body());
}

TEST_CASE("FormSession: a ready form with nothing to type submits as constructed", "[forms-engine][session]") {
    Harness harness;
    auto const form =
        harness.session("Test_UnknownRuleKind", R"({"properties":{"email":{"type":["string","null"],"x-order":0}},"required":[],
          "x-rules":[{"kind":"quantumEntangled","fields":["email"]}]})");
    harness.settle();
    CHECK(form->ready());
    CHECK(harness.server.submits().size() == 1);
}

// Review Focus 1.
TEST_CASE("FormSession: prefilling a complete payload never submits; the next user edit does", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("TestAction", kTwoInts);
    form->prefill(R"({"a":1,"b":2})");
    harness.settle();
    CHECK(form->ready());
    CHECK(harness.server.submits().empty());
    form->assign("b", "7");
    harness.settle();
    CHECK(form->body() == R"({"a":1,"b":7})");
    CHECK(harness.server.submits().empty());
    harness.type(*form, "a", "9");
    REQUIRE(harness.server.submits().size() == 1);
    CHECK(harness.server.lastBody() == R"({"a":9,"b":7})");
}

TEST_CASE("FormSession: reset clears every draft and does not submit a form that stays ready",
          "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("ResetResubmit", R"({"properties":{"note":{"type":["string","null"],"x-order":0}},"required":[]})");
    harness.type(*form, "note", "hello");
    auto const before = harness.server.submits().size();
    form->reset();
    harness.settle();
    CHECK(form->ready());
    CHECK(form->field("note").text().peek().empty());
    CHECK(harness.server.submits().size() == before);
}

TEST_CASE("FormSession: a stored payload round-trips, absent members start blank, the unit resets",
          "[forms-engine][session]") {
    Harness harness;
    constexpr std::string_view kSample = R"({"$defs":{"double":{"type":"number"},"int64_t":{"type":"integer"}},"properties":{
      "id":{"$ref":"#/$defs/int64_t","x-order":0},
      "density":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg_per_m3","unitUnicode":"kg/m³"},"x-decimalPlaces":2,"x-order":1,
                 "x-unitAlternatives":[{"id":"g_per_cm3","display":"g/cm³","decimals":5,"num":1000,"den":1}]},
      "temperature":{"$ref":"#/$defs/double","x-order":2},
      "takenAt":{"type":"string","format":"date-time","x-order":3},
      "done":{"type":"boolean","x-order":4},
      "role":{"type":"string","oneOf":[{"title":"A","const":"A"},{"title":"B","const":"B"}],"x-order":5},
      "note":{"type":["string","null"],"x-order":6},
      "tags":{"type":"array","items":{"type":"string"},"x-order":7}},
      "required":["id","density","temperature","takenAt","done","role"]})";
    constexpr std::string_view kStored = R"({"id":9007199254740993,"density":{"num":245050,"den":100,"dp":2},)"
                                         R"("temperature":21.5,"takenAt":"2026-07-20T09:00:00Z","done":true,"role":"B",)"
                                         R"("note":"retest","tags":["a","b"]})";
    auto const form = harness.session("T_Sample", kSample);
    form->field("density").switchUnit(1);
    form->prefill(kStored);
    harness.settle();
    CHECK(form->body() == kStored);
    CHECK(harness.server.submits().empty());
    CHECK(form->field("id").text().peek() == "9007199254740993");
    CHECK(form->field("density").text().peek() == "2450.50");
    CHECK(form->field("density").unit().peek() == 0);
    CHECK(form->field("takenAt").text().peek() == "2026-07-20T09:00:00");
    CHECK(form->field("role").text().peek() == R"("B")");
    CHECK(form->field("tags").text().peek() == "a, b");
    harness.type(*form, "temperature", "22.00");
    CHECK(form->body()->find(R"("temperature":22.00)") != std::string::npos);

    form->prefill(R"({"id":5})");
    harness.settle();
    CHECK(form->field("temperature").text().peek().empty());
    CHECK_FALSE(form->ready());
    std::string const before = form->field("id").text().peek();
    form->prefill("not json");
    form->prefill("[1,2]");
    harness.settle();
    CHECK(form->field("id").text().peek() == before);

    Harness german;
    auto const germanForm = german.session("T_Sample", kSample, FormSessionOptions{.locale = {.decimalSeparator = ",", .groupSeparator = "."}});
    germanForm->prefill(kStored);
    german.settle();
    CHECK(germanForm->field("density").text().peek() == "2450,50");
    CHECK(germanForm->field("temperature").text().peek() == "21,5");
    CHECK(germanForm->body() == kStored);

    Harness zoned;
    auto const shifted = zoned.session("T_Sample", kSample, FormSessionOptions{.displayOffsetMinutes = 120});
    shifted->prefill(kStored);
    zoned.settle();
    CHECK(shifted->field("takenAt").text().peek() == "2026-07-20T11:00:00");
    CHECK(shifted->body()->find(R"("takenAt":"2026-07-20T09:00:00Z")") != std::string::npos);
}

TEST_CASE("FormSession: x-blankAs empty submits \"\" once the field was filled, and only then",
          "[forms-engine][session]") {
    constexpr std::string_view kEdit = R"({"type":"object","properties":{
      "id":{"type":"integer","x-order":0},
      "remark":{"type":["string","null"],"x-order":1,"x-blankAs":"empty"},
      "operatorName":{"anyOf":[{"type":"string"},{"type":"null"}],"x-order":2,"x-blankAs":"empty"},
      "plain":{"type":["string","null"],"x-order":3},
      "weight":{"type":["number","null"],"x-order":4,"x-blankAs":"empty"},
      "title":{"type":"string","x-order":5,"x-blankAs":"empty"}},"required":["id","title"]})";
    constexpr std::string_view kStored = R"({"id":7,"remark":"abc","operatorName":"Ann","plain":"keep","weight":1.5,"title":"T"})";
    Harness harness;
    auto const form = harness.session("T_EditSample", kEdit);
    CHECK(form->field("remark").spec().blankAs == morph::forms::BlankAs::Empty);
    CHECK(form->field("weight").spec().blankAs == morph::forms::BlankAs::Omit);  // not a string field
    form->prefill(kStored);
    harness.settle();
    CHECK(harness.server.submits().empty());
    harness.type(*form, "remark", "");
    CHECK(harness.server.lastBody() == R"({"id":7,"remark":"","operatorName":"Ann","plain":"keep","weight":1.5,"title":"T"})");
    form->prefill(kStored);
    harness.settle();
    harness.type(*form, "plain", "");
    CHECK(harness.server.lastBody() == R"({"id":7,"remark":"abc","operatorName":"Ann","weight":1.5,"title":"T"})");
    form->prefill(kStored);
    harness.settle();
    harness.type(*form, "weight", "");
    CHECK(harness.server.lastBody() == R"({"id":7,"remark":"abc","operatorName":"Ann","plain":"keep","title":"T"})");
    auto const before = harness.server.submits().size();
    harness.type(*form, "title", "");
    CHECK_FALSE(form->ready());
    CHECK(harness.server.submits().size() == before);

    form->reset();
    harness.settle();
    harness.type(*form, "id", "8");
    harness.type(*form, "title", "U");
    CHECK(harness.server.lastBody() == R"({"id":8,"title":"U"})");
    harness.type(*form, "operatorName", "Bo");
    harness.type(*form, "operatorName", "");
    CHECK(harness.server.lastBody() == R"({"id":8,"operatorName":"","title":"U"})");

    form->prefill(R"({"id":7,"remark":"","title":"T"})");
    harness.settle();
    CHECK(form->body() == R"({"id":7,"remark":"","title":"T"})");
    form->prefill(R"({"id":7,"remark":null,"title":"T"})");
    harness.settle();
    CHECK(form->body() == R"({"id":7,"title":"T"})");
}

TEST_CASE("FormSession: booleans are bare literals; a required one answers false, an optional one is omitted",
          "[forms-engine][session]") {
    Harness harness;
    auto const required = harness.session(
        "T_Bool", R"({"properties":{"note":{"type":"string","x-order":0},"flag":{"type":"boolean","x-order":1}},"required":["note","flag"]})");
    harness.type(*required, "note", "n");
    CHECK(required->body() == R"({"note":"n","flag":false})");
    harness.type(*required, "flag", "true");
    CHECK(required->body() == R"({"note":"n","flag":true})");
    auto const optional = harness.session("T_NullableBool", R"({"properties":{"flag":{"type":["boolean","null"],"x-order":0}},"required":[]})");
    harness.settle();
    CHECK(optional->body() == "{}");
    harness.type(*optional, "flag", "true");
    CHECK(optional->body() == R"({"flag":true})");
}

TEST_CASE("FormSession: an integer through anyOf keeps every digit and refuses text", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("T_AnyOf", R"({"$defs":{"int64_t":{"type":"integer"},"TagId":{"type":"integer"}},"properties":{
      "optI64":{"anyOf":[{"$ref":"#/$defs/int64_t"},{"type":"null"}],"x-order":0},
      "refI64":{"$ref":"#/$defs/TagId","x-order":1}},"required":[]})");
    harness.type(*form, "optI64", "9007199254740993");
    CHECK(form->body() == R"({"optI64":9007199254740993})");
    harness.type(*form, "optI64", "9223372036854775807");
    harness.type(*form, "refI64", "18446744073709551615");
    CHECK(form->body() == R"({"optI64":9223372036854775807,"refI64":18446744073709551615})");
    harness.type(*form, "optI64", "banana");
    CHECK_FALSE(form->ready());
}

// Review Focus 3.
TEST_CASE("FormSession: an id beyond 2^53 is exact through encode, body, a rule and prefill", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("RcEqualsBigInt", R"({"$defs":{"int64_t":{"type":"integer"}},"properties":{
      "id":{"anyOf":[{"$ref":"#/$defs/int64_t"},{"type":"null"}],"x-order":0},
      "note":{"type":["string","null"],"x-order":1}},"required":[],
      "x-rules":[{"kind":"requiredWhen","fields":["note"],"when":{"kind":"equals","fields":["id"],"value":9007199254740993,"valueText":"9007199254740993"}}]})");
    harness.type(*form, "id", "9007199254740992");
    CHECK(form->ready());
    harness.type(*form, "id", "9007199254740993");
    CHECK_FALSE(form->ready());
    CHECK(form->field("note").required().get());
    form->prefill(R"({"id":9007199254740993,"note":"why"})");
    harness.settle();
    CHECK(form->body() == R"({"id":9007199254740993,"note":"why"})");
}

TEST_CASE("FormSession: x-rules gate and present live", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("CFR_BookRoom", R"({"properties":{
      "name":{"type":"string","x-order":0},"checkInN":{"type":"integer","x-order":1},"checkOutN":{"type":"integer","x-order":2},
      "email":{"type":"string","x-order":3},"phone":{"type":"string","x-order":4},
      "promo":{"type":"integer","x-order":5},"discount":{"type":"integer","x-order":6}},"required":["name"],
      "x-rules":[{"kind":"greater","fields":["checkOutN","checkInN"]},{"kind":"exactlyOneOf","fields":["email","phone"]},
        {"kind":"requiredWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}},
        {"kind":"visibleWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}}]})");
    CHECK_FALSE(form->field("discount").visible().get());
    harness.type(*form, "name", "Alice");
    CHECK_FALSE(form->ready());
    harness.type(*form, "email", "a@b.com");
    CHECK(form->ready());
    CHECK(harness.server.submits().size() == 1);
    harness.type(*form, "promo", "5");
    CHECK(form->field("discount").visible().get());
    CHECK(form->field("discount").required().get());
    CHECK_FALSE(form->ready());
    CHECK(harness.server.submits().size() == 1);
    harness.type(*form, "discount", "2");
    CHECK(form->ready());
    CHECK(harness.server.submits().size() == 2);
    harness.type(*form, "checkInN", "10");
    harness.type(*form, "checkOutN", "3");
    CHECK_FALSE(form->ready());
    harness.type(*form, "checkOutN", "20");
    CHECK(form->ready());
    harness.type(*form, "phone", "555");
    CHECK_FALSE(form->ready());
}

TEST_CASE("FormSession: compound when-clauses require, release and negate", "[forms-engine][session]") {
    auto const schema = [](std::string_view when) {
        return std::string{R"({"properties":{"name":{"type":"string","x-order":0},"promo":{"type":"integer","x-order":1},
          "loyaltyCode":{"type":"integer","x-order":2},"discount":{"type":"integer","x-order":3}},"required":["name"],
          "x-rules":[{"kind":"requiredWhen","fields":["discount"],"when":)"} +
               std::string{when} + "}]}";
    };
    Harness harness;
    auto const both = harness.session("A", schema(R"({"kind":"and","conditions":[{"kind":"engaged","fields":["promo"]},{"kind":"engaged","fields":["loyaltyCode"]}]})"));
    harness.type(*both, "name", "Alice");
    harness.type(*both, "promo", "5");
    CHECK(both->ready());
    harness.type(*both, "loyaltyCode", "9");
    CHECK_FALSE(both->ready());
    harness.type(*both, "discount", "2");
    CHECK(both->ready());
    auto const either = harness.session("O", schema(R"({"kind":"or","conditions":[{"kind":"engaged","fields":["promo"]},{"kind":"engaged","fields":["loyaltyCode"]}]})"));
    harness.type(*either, "name", "Alice");
    CHECK(either->ready());
    harness.type(*either, "promo", "5");
    CHECK_FALSE(either->ready());
    auto const negated = harness.session("N", schema(R"({"kind":"not","condition":{"kind":"engaged","fields":["promo"]}})"));
    harness.type(*negated, "name", "Alice");
    CHECK_FALSE(negated->ready());
    harness.type(*negated, "promo", "5");
    CHECK(negated->ready());
}

TEST_CASE("FormSession: an enum starts unselected, encodes the chosen literal and refuses anything else",
          "[forms-engine][session]") {
    Harness harness;
    harness.server.replyWith([](auto const&) { return std::nullopt; });  // record only
    auto const form = harness.session("SetMemberRole", R"({"properties":{
      "principal":{"type":"string","x-order":1},"projectId":{"type":["integer","null"],"x-order":0},
      "role":{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},{"title":"Member","const":"Member"},{"title":"Manager","const":"Manager"}],"x-order":2}},
      "required":["projectId","principal","role"]})");
    harness.type(*form, "projectId", "1");
    harness.type(*form, "principal", "bob");
    CHECK_FALSE(form->ready());
    CHECK(form->field("role").options().size() == 3);
    harness.type(*form, "role", R"("Emperor")");
    CHECK_FALSE(form->ready());
    CHECK(harness.server.submits().empty());
    harness.type(*form, "role", "Manager");
    CHECK_FALSE(form->ready());
    harness.type(*form, "role", R"("Manager")");
    CHECK(form->body() == R"({"projectId":1,"principal":"bob","role":"Manager"})");
    form->reset();
    harness.settle();
    CHECK(form->field("role").text().peek().empty());
    CHECK(form->field("principal").text().peek().empty());
}

TEST_CASE("FormSession: an array field encodes a JSON array; blank optional is omitted, a required one gates",
          "[forms-engine][session]") {
    Harness harness;
    constexpr std::string_view kProps = R"("name":{"type":"string","x-order":0},"tags":{"type":"array","items":{"type":"string"},"x-order":1})";
    auto const optional = harness.session("CFR_TagRoom", std::string{R"({"properties":{)"} + std::string{kProps} + R"(},"required":["name"]})");
    harness.type(*optional, "name", "Alice");
    CHECK(optional->body() == R"({"name":"Alice"})");
    harness.type(*optional, "tags", "solo");
    CHECK(optional->body() == R"({"name":"Alice","tags":["solo"]})");
    auto const required =
        harness.session("CFR_TagRoom", std::string{R"({"properties":{)"} + std::string{kProps} + R"(},"required":["name","tags"]})");
    harness.type(*required, "name", "Alice");
    CHECK_FALSE(required->ready());
    harness.type(*required, "tags", " , , ");
    CHECK(required->body() == R"({"name":"Alice","tags":[]})");
}

TEST_CASE("FormSession: labels resolve through the translations; numbers read in the session locale",
          "[forms-engine][session]") {
    morph::render::TranslationProvider const catalog = [](std::string_view key, std::string_view locale) -> std::optional<std::string> {
        if (locale != "de") {
            return std::nullopt;
        }
        if (key == "Probe.slot.label") {
            return "Steckplatz";
        }
        if (key == "custom.stem.label") {
            return "Übersteuert";
        }
        return std::nullopt;
    };
    constexpr std::string_view kSchema = R"({"properties":{"slot":{"type":["integer","null"],"x-order":0},
      "mass":{"$ref":"#/$defs/q","x-order":1,"x-decimalPlaces":3,"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}},
      "other":{"type":"integer","x-order":2,"x-i18nKey":"custom.stem"}},"$defs":{"q":{"type":["object","null"]}},"required":["mass"]})";
    morph::render::NumericLocale const german{.decimalSeparator = ",", .groupSeparator = "."};
    Harness harness;
    auto const form = harness.session("Probe", kSchema, FormSessionOptions{.locale = german, .bcp47 = "de", .translations = catalog});
    CHECK(form->field("slot").label() == "Steckplatz");
    CHECK(form->field("mass").label() == "mass");
    CHECK(form->field("other").label() == "Übersteuert");
    auto const plain = harness.session("Probe", kSchema);
    CHECK(plain->field("slot").label() == "slot");

    harness.type(*form, "mass", "1.050,25");
    CHECK(form->body() == R"({"mass":{"num":1050250,"den":1000,"dp":3}})");
    for (std::string_view const foreign : {"1.5", "1.50", "1234.050,25"}) {
        harness.type(*form, "mass", foreign);
        CHECK_FALSE(form->ready());
    }
    harness.type(*form, "mass", "+1.050,25");
    CHECK(form->body() == R"({"mass":{"num":1050250,"den":1000,"dp":3}})");
    constexpr std::string_view kMinus = "\xE2\x88\x92";  // U+2212
    morph::render::NumericLocale const basque{.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kMinus};
    auto const signForm = harness.session("Probe", kSchema, FormSessionOptions{.locale = basque});
    harness.type(*signForm, "mass", std::string{kMinus} + "5");
    CHECK(signForm->body() == R"({"mass":{"num":-5000,"den":1000,"dp":3}})");
    harness.type(*signForm, "mass", "-5");
    CHECK(signForm->body() == R"({"mass":{"num":-5000,"den":1000,"dp":3}})");
}

TEST_CASE("FormSession: a failed submission is the last error; a later success clears it", "[forms-engine][session]") {
    Harness harness;
    harness.server.replyWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session("TestAction", kTwoInts);
    harness.type(*form, "a", "1");
    harness.type(*form, "b", "2");
    CHECK(form->pending());
    harness.server.rejectSubmit(0, "denied");
    harness.settle();
    CHECK_FALSE(form->pending());
    CHECK(morph::reactive::errorMessage(form->lastError()) == "denied");
    harness.type(*form, "b", "3");
    harness.server.resolveSubmit(1, R"({"sum":4})");
    harness.settle();
    CHECK(form->lastError() == nullptr);
    CHECK(form->lastReply() == R"({"sum":4})");
}

TEST_CASE("FormSession: field() resolves paths and refuses unknown ones", "[forms-engine][session]") {
    Harness harness;
    auto const form = harness.session("TestAction", kTwoInts);
    CHECK(form->field("a").path() == "a");
    CHECK_THROWS_AS(form->field("nope"), std::out_of_range);
    CHECK_THROWS_AS(form->field("a.b"), std::out_of_range);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/form_session.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/form_session.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/form_session.hpp
/// @brief `FormSession`: the reactive state of one form over a `FormModel` — drafts, encoded
///        values, readiness, presentation rules, Choice options and submission. A controller in
///        the sense of `docs/spec/reactive/control.md`: no toolkit, testable headless.
///
/// Specified in `docs/spec/forms/engine.md`, "FormSession".

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "../../attributes.hpp"
#include "../../core/completion.hpp"
#include "../../reactive/control.hpp"
#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../../render/i18n.hpp"
#include "../../render/locale_format.hpp"
#include "../i18n.hpp"
#include "detail/text.hpp"
#include "field_model.hpp"
#include "rules.hpp"
#include "values.hpp"

namespace morph::forms {

/// @brief Executes a form's action: the action type and its JSON body in, the JSON reply out.
using Submitter = std::function<async::Completion<std::string>(std::string_view actionType, std::string bodyJson)>;

/// @brief Executes a Choice's options action: the action and its JSON request in, the reply out.
using ChoiceFetcher = std::function<async::Completion<std::string>(std::string_view optionsAction, std::string bodyJson)>;

/// @brief How a session reads numbers, dates and text.
struct FormSessionOptions {
    /// @brief How numbers are typed and shown. The views must outlive the session.
    render::NumericLocale locale{};
    /// @brief The locale translations are resolved in.
    std::string bcp47 = "en";
    /// @brief The host's catalog; empty means every label is the schema's literal.
    render::TranslationProvider translations{};
    /// @brief The display zone for date-times, in minutes east of UTC.
    int displayOffsetMinutes = 0;
    /// @brief An extra readiness gate over the assembled body; empty means none. `Form<A>` puts
    ///        the action's own `validate()` here.
    std::function<bool(std::string_view bodyJson)> accepts{};
};

class FieldState;

namespace detail::engine {

/// @brief What every field of one session shares: the runtime, the model, the options, the
///        Choice fetcher, and the hook a programmatic change ends with.
class SessionCore {
public:
    /// @param runtime The runtime. Borrowed.
    /// @param model   The form model.
    /// @param choices The Choice fetcher (may be empty when no field is a Choice).
    /// @param options The session options.
    SessionCore(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, FormModel model, ChoiceFetcher choices,
                FormSessionOptions options)
        : _rt{&runtime},
          _model{std::move(model)},
          _choices{std::move(choices)},
          _options{std::move(options)},
          _values{_options.locale, _options.displayOffsetMinutes, _options.translations, _options.bcp47} {}

    /// @brief The runtime.
    /// @return The runtime passed at construction.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return *_rt; }

    /// @brief The form model.
    /// @return The model.
    [[nodiscard]] FormModel const& model() const noexcept { return _model; }

    /// @brief The session options.
    /// @return The options.
    [[nodiscard]] FormSessionOptions const& options() const noexcept { return _options; }

    /// @brief The options a value conversion reads.
    /// @return The value context.
    [[nodiscard]] ValueContext const& values() const noexcept { return _values; }

    /// @brief The Choice fetcher.
    /// @return The fetcher; may be empty.
    [[nodiscard]] ChoiceFetcher const& choices() const noexcept { return _choices; }

    /// @brief Installs what `markProgrammatic` runs (the session records its current body as seen).
    /// @param hook The hook.
    void onProgrammatic(std::function<void()> hook) { _programmatic = std::move(hook); }

    /// @brief Ends a programmatic change: automatic submission does not submit the body it produced.
    void markProgrammatic() const {
        if (_programmatic) {
            _programmatic();
        }
    }

    /// @brief Installs the top-level fields rules look up.
    /// @param fields The session's fields. Borrowed: they must outlive this core's use.
    void adopt(std::vector<std::unique_ptr<FieldState>> const* fields) noexcept { _fields = fields; }

    /// @brief A top-level field by wire name.
    /// @param name The wire name.
    /// @return The field, or null.
    [[nodiscard]] FieldState* topLevel(std::string_view name) const;

    /// @brief The tracked lookup rules evaluate over.
    /// @return A lookup reading each named field's encoding.
    [[nodiscard]] RuleLookup lookup() const;

private:
    reactive::Runtime* _rt;
    FormModel _model;
    ChoiceFetcher _choices;
    FormSessionOptions _options;
    ValueContext _values;
    std::function<void()> _programmatic;
    std::vector<std::unique_ptr<FieldState>> const* _fields = nullptr;
};

}  // namespace detail::engine
/// @brief The reactive state of one field: its draft and everything derived from it.
class FieldState {
public:
    /// @param core   The session core. Borrowed: it must outlive the field.
    /// @param spec   The field's spec. Borrowed from the core's model (or a parent's children).
    /// @param path   The instance path (`name`, `parent.member`, `rows#key.member`).
    /// @param parent The containing Object or ObjectArray field, or null at the top level.
    FieldState(detail::engine::SessionCore& core MORPH_LIFETIMEBOUND, FieldSpec const& spec MORPH_LIFETIMEBOUND,
               std::string path, FieldState const* parent)
        : _core{&core},
          _spec{&spec},
          _path{std::move(path)},
          _parent{parent},
          _text{core.runtime(), std::string{}},
          _unit{core.runtime(), std::size_t{0}},
          _engaged{core.runtime(), false},
          _encoded{core.runtime(), [this] { return computeEncoding(); }},
          _error{core.runtime(),
                 [this]() -> std::optional<FieldError> {
                     Encoding const& encoding = _encoded.get();
                     if (encoding.has_value() && !encoding->has_value()) {
                         return encoding->error();
                     }
                     return std::nullopt;
                 }},
          _visible{core.runtime(),
                   [this] {
                       if (_spec->hidden) {
                           return false;
                       }
                       return _parent != nullptr ||
                              visibleVerdict(_core->model().rules(), _spec->name, _core->lookup()) != Tri::False;
                   }},
          _readonly{core.runtime(),
                    [this] {
                        if (_spec->readOnly || (_parent != nullptr && _parent->readonly().get())) {
                            return true;
                        }
                        return _parent == nullptr &&
                               readonlyVerdict(_core->model().rules(), _spec->name, _core->lookup()) == Tri::True;
                    }},
          _required{core.runtime(), [this] {
                        return _spec->required ||
                               (_parent == nullptr &&
                                requiredVerdict(_core->model().rules(), _spec->name, _core->lookup()) == Tri::True);
                    }} {
        _label = resolveSlot(i18n::FieldSlot::Label, _spec->title);
        _help = resolveSlot(i18n::FieldSlot::Help, _spec->help);
        _placeholder = _spec->placeholder.empty() ? defaultPlaceholder(*_spec)
                                                  : resolveSlot(i18n::FieldSlot::Placeholder, _spec->placeholder);
    }

    ~FieldState() = default;
    FieldState(FieldState const&) = delete;
    FieldState& operator=(FieldState const&) = delete;
    FieldState(FieldState&&) = delete;
    FieldState& operator=(FieldState&&) = delete;

    /// @brief The field's spec.
    /// @return The spec.
    [[nodiscard]] FieldSpec const& spec() const noexcept { return *_spec; }

    /// @brief The instance path.
    /// @return `name`, `parent.member`, or `rows#key.member` inside a collection row.
    [[nodiscard]] std::string const& path() const noexcept { return _path; }

    /// @brief The label: the explicit key's translation, else the derived key's (top level
    ///        only), else the schema's title.
    /// @return The label.
    [[nodiscard]] std::string const& label() const noexcept { return _label; }

    /// @brief The help text, resolved like the label.
    /// @return The help text; empty when the schema states none.
    [[nodiscard]] std::string const& help() const noexcept { return _help; }

    /// @brief The placeholder: `x-placeholder` resolved like the label, else `defaultPlaceholder`.
    /// @return The placeholder.
    [[nodiscard]] std::string const& placeholder() const noexcept { return _placeholder; }

    /// @brief The draft: what the control holds. Setting it is a user edit.
    /// @return The draft signal.
    [[nodiscard]] reactive::Signal<std::string>& text() noexcept { return _text; }

    /// @brief A Decimal's or Quantity's entry unit, an index into `spec().units`.
    /// @return The unit signal.
    [[nodiscard]] reactive::Signal<std::size_t>& unit() noexcept { return _unit; }

    /// @brief Whether a blank-capable field was filled once (an `x-blankAs: empty` text, or a
    ///        collection that had rows).
    /// @return The engagement signal.
    [[nodiscard]] reactive::Signal<bool>& engaged() noexcept { return _engaged; }

    /// @brief The draft's encoding. Tracked.
    /// @return The encoding computed.
    [[nodiscard]] reactive::Computed<Encoding> const& encoded() const noexcept { return _encoded; }

    /// @brief Why the draft does not encode, if it does not. Tracked.
    /// @return The error computed.
    [[nodiscard]] reactive::Computed<std::optional<FieldError>> const& error() const noexcept { return _error; }

    /// @brief Whether the field is shown: not `x-hidden`, and no `visibleWhen` is False.
    /// @return The visibility computed.
    [[nodiscard]] reactive::Computed<bool> const& visible() const noexcept { return _visible; }

    /// @brief Whether the field is frozen: `x-readonly`, a frozen parent, or a `readonlyWhen` that is True.
    /// @return The read-only computed.
    [[nodiscard]] reactive::Computed<bool> const& readonly() const noexcept { return _readonly; }

    /// @brief Whether the field must be filled: `required`, or a `requiredWhen` that is True.
    /// @return The required computed.
    [[nodiscard]] reactive::Computed<bool> const& required() const noexcept { return _required; }

    /// @brief Whether nothing is entered. Tracked.
    /// @return `true` when blank.
    [[nodiscard]] bool blank() const { return detail::engine::trim(_text.get()).empty(); }

    /// @brief What a rule sees of this field. Tracked.
    /// @return Blank, Invalid, or Valid with the exact scalar.
    [[nodiscard]] RuleValue ruleValue() const {
        Encoding const& encoding = _encoded.get();
        if (!encoding.has_value()) {
            return RuleValue{};
        }
        if (!encoding->has_value()) {
            return RuleValue{RuleValue::State::Invalid, {}};
        }
        return RuleValue{RuleValue::State::Valid, (*encoding)->scalar};
    }

    /// @brief The selectable options: an Enum's, from the schema. Tracked.
    /// @return The options; empty for every other kind.
    [[nodiscard]] std::vector<ChoiceOption> const& options() const { return _spec->enumOptions; }

    /// @brief Whether a Choice's options request is in flight. Tracked.
    /// @return `true` while fetching.
    [[nodiscard]] bool optionsPending() const { return false; }

    /// @brief Switches a Quantity's entry unit, converting the draft exactly (half away from
    ///        zero to the new unit's decimals). A user action.
    /// @param index The new unit, an index into `spec().units`.
    void switchUnit(std::size_t index) {
        std::size_t const current = _unit.peek();
        if (index >= _spec->units.size() || index == current || current >= _spec->units.size()) {
            return;
        }
        auto converted = convertDraft(_text.peek(), _spec->units.at(current), _spec->units.at(index),
                                      _core->values().locale);
        _core->runtime().batch([&] {
            _unit.set(index);
            if (!detail::engine::trim(_text.peek()).empty()) {
                _text.set(converted.value_or(std::string{}));
            }
        });
    }

    /// @brief Replaces the draft with the decoded wire value; null or absent is blank. Does not
    ///        mark the change programmatic — the caller does, in the same batch.
    /// @param value The wire value, or null.
    void load(detail::engine::Json const* value) {
        bool const present = value != nullptr && !value->is_null();
        _text.set(present ? decodeScalar(*_spec, *value, _core->values()) : std::string{});
        _unit.set(0);
        _engaged.set(_spec->blankAs == BlankAs::Empty && present && value->is_string());
    }

    /// @brief Blanks the draft, the unit and the engagement.
    void clear() {
        _text.set(std::string{});
        _unit.set(0);
        _engaged.set(false);
    }

    /// @brief Starts what needs every sibling to exist: an `x-blankAs` field's engagement.
    ///        Called once, after the session built all its fields.
    void start() {
        if (_spec->blankAs == BlankAs::Empty) {
            _engage = std::make_unique<reactive::Effect>(_core->runtime(), [this] {
                if (!detail::engine::trim(_text.get()).empty() && !_engaged.peek()) {
                    _engaged.set(true);
                }
            });
        }
    }

private:
    [[nodiscard]] std::string resolveSlot(i18n::FieldSlot slot, std::string const& literal) const {
        auto const& translations = _core->options().translations;
        std::string_view const locale = _core->options().bcp47;
        auto const explicitKey = i18n::explicitFieldKey(_spec->i18nKey, slot);
        if (_parent == nullptr) {
            return render::resolveText(translations, locale, explicitKey,
                                       i18n::fieldKey(_core->model().actionType(), _spec->name, slot), literal);
        }
        if (translations && explicitKey) {
            if (auto hit = translations(*explicitKey, locale)) {
                return *std::move(hit);
            }
        }
        return literal;
    }

    [[nodiscard]] Encoding computeEncoding() const {
        return encodeScalar(*_spec, _text.get(), _unit.get(), _core->values());
    }

    // Declaration order is destruction order reversed: the signals precede the computeds that
    // read them, and the effect is last, so nothing is torn down while something reads it.
    detail::engine::SessionCore* _core;
    FieldSpec const* _spec;
    std::string _path;
    FieldState const* _parent;
    std::string _label;
    std::string _help;
    std::string _placeholder;
    reactive::Signal<std::string> _text;
    reactive::Signal<std::size_t> _unit;
    reactive::Signal<bool> _engaged;
    reactive::Computed<Encoding> _encoded;
    reactive::Computed<std::optional<FieldError>> _error;
    reactive::Computed<bool> _visible;
    reactive::Computed<bool> _readonly;
    reactive::Computed<bool> _required;
    std::unique_ptr<reactive::Effect> _engage;
};

namespace detail::engine {

inline FieldState* SessionCore::topLevel(std::string_view name) const {
    if (_fields == nullptr) {
        return nullptr;
    }
    auto const found = std::ranges::find_if(*_fields, [&](auto const& entry) { return entry->spec().name == name; });
    return found == _fields->end() ? nullptr : found->get();
}

inline RuleLookup SessionCore::lookup() const {
    return [this](std::string_view name) {
        FieldState const* const field = topLevel(name);
        return field == nullptr ? RuleValue{} : field->ruleValue();
    };
}

}  // namespace detail::engine

/// @brief The reactive state of one form: a controller over a `FormModel`.
///
/// Non-copyable and non-movable: fields, bindings and in-flight replies point into it. Destroy a
/// mounted view of it before the session.
class FormSession {
public:
    /// @param runtime The runtime. Borrowed: it must outlive the session.
    /// @param model   The form model.
    /// @param submit  Executes the form's action. Required.
    /// @param choices Fetches Choice options; may be empty when no field is a Choice.
    /// @param options Locale, translations, display zone, extra readiness gate.
    /// @throws std::invalid_argument when @p submit is empty.
    FormSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, FormModel model, Submitter submit, ChoiceFetcher choices,
                FormSessionOptions options = {})
        : _core{std::make_unique<detail::engine::SessionCore>(runtime, std::move(model), std::move(choices), std::move(options))},
          _submit{std::move(submit)},
          _assembled{runtime, [this] { return assemble(); }},
          _ready{runtime,
                 [this] {
                     std::optional<std::string> const& assembled = _assembled.get();
                     if (!assembled.has_value()) {
                         return false;
                     }
                     auto const& accepts = _core->options().accepts;
                     return !accepts || accepts(*assembled);
                 }},
          _body{runtime, [this] { return _ready.get() ? _assembled.get() : std::optional<std::string>{}; }},
          _mutation{runtime, [this](std::string body) { return _submit(_core->model().actionType(), std::move(body)); }} {
        if (!_submit) {
            throw std::invalid_argument{"FormSession: a Submitter is required"};
        }
        for (FieldSpec const& spec : _core->model().fields()) {
            _fields.push_back(std::make_unique<FieldState>(*_core, spec, spec.name, nullptr));
        }
        _core->adopt(&_fields);
        _core->onProgrammatic([this] { _lastSubmitted = _body.peek(); });
        for (std::size_t i = 0; i < _core->model().groups().size(); ++i) {
            _collapsed.push_back(std::make_unique<reactive::Signal<bool>>(runtime, false));
            _tabs.push_back(std::make_unique<reactive::Signal<std::size_t>>(runtime, std::size_t{0}));
        }
        for (auto const& field : _fields) {
            field->start();
        }
        _autoSubmit = std::make_unique<reactive::Effect>(runtime, [this] { autoSubmit(); });
    }

    ~FormSession() = default;
    FormSession(FormSession const&) = delete;
    FormSession& operator=(FormSession const&) = delete;
    FormSession(FormSession&&) = delete;
    FormSession& operator=(FormSession&&) = delete;

    /// @brief The form model.
    /// @return The model.
    [[nodiscard]] FormModel const& model() const noexcept { return _core->model(); }

    /// @brief The runtime.
    /// @return The runtime passed at construction.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return _core->runtime(); }

    /// @brief When the form submits.
    /// @return The model's submit mode.
    [[nodiscard]] SubmitMode submitMode() const noexcept { return _core->model().submitMode(); }

    /// @brief The session options.
    /// @return The options.
    [[nodiscard]] FormSessionOptions const& options() const noexcept { return _core->options(); }

    /// @brief The top-level fields, in `x-order`.
    /// @return The fields.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> fields() const noexcept { return _fields; }

    /// @brief A top-level field by wire name.
    /// @param path The field's wire name.
    /// @return The field.
    /// @throws std::out_of_range when no field has that path.
    [[nodiscard]] FieldState& field(std::string_view path) const {
        auto const found = std::ranges::find_if(_fields, [&](auto const& entry) { return entry->spec().name == path; });
        if (found == _fields.end()) {
            throw std::out_of_range{"FormSession::field: no field '" + std::string{path} + "'"};
        }
        return **found;
    }

    /// @brief Whether the form can submit. Tracked.
    /// @return `true` when every required field encodes, no field has an error, no gating rule
    ///         is False, and `options().accepts` (when set) accepts the body.
    [[nodiscard]] bool ready() const { return _ready.get(); }

    /// @brief The body the form would submit. Tracked.
    /// @return The JSON body, fields in `x-order`, while ready; `std::nullopt` otherwise.
    [[nodiscard]] std::optional<std::string> body() const { return _body.get(); }

    /// @brief Submits the current body; does nothing while not ready. The explicit-mode trigger;
    ///        in automatic mode it forces a resubmission of an unchanged body.
    void submit() {
        std::optional<std::string> const body = _body.peek();
        if (!body.has_value()) {
            return;
        }
        _lastSubmitted = body;
        _mutation.run(*body);
    }

    /// @brief Loads a stored payload for editing. Every field is replaced (an absent member starts
    ///        blank), every unit returns to the canonical one, and nothing is submitted. Text that
    ///        is not a JSON object changes nothing.
    /// @param bodyJson The payload.
    void prefill(std::string_view bodyJson) {
        auto const dom = detail::engine::parseJson(bodyJson);
        if (!dom || !dom->is_object()) {
            return;
        }
        runtime().batch([&] {
            for (auto const& field : _fields) {
                field->load(detail::engine::member(*dom, field->spec().name));
            }
            _core->markProgrammatic();
        });
    }

    /// @brief Sets one field from a JSON value, programmatically (nothing is submitted).
    /// @param path      The field's path (see `field`).
    /// @param jsonValue The value as JSON; text that is not JSON changes nothing.
    /// @throws std::out_of_range when no field has that path.
    void assign(std::string_view path, std::string_view jsonValue) {
        FieldState& target = field(path);
        auto const dom = detail::engine::parseJson(jsonValue);
        if (!dom) {
            return;
        }
        runtime().batch([&] {
            target.load(&*dom);
            _core->markProgrammatic();
        });
    }

    /// @brief Blanks every field, programmatically (nothing is submitted).
    void reset() {
        runtime().batch([&] {
            for (auto const& field : _fields) {
                field->clear();
            }
            _core->markProgrammatic();
        });
    }

    /// @brief Whether a submission is in flight. Tracked.
    /// @return `true` while at least one is.
    [[nodiscard]] bool pending() const { return _mutation.pending(); }

    /// @brief The last successful reply. Tracked.
    /// @return The reply JSON, or `std::nullopt` before the first success.
    [[nodiscard]] std::optional<std::string> const& lastReply() const { return _mutation.lastResult(); }

    /// @brief The last failure. Tracked.
    /// @return The exception, or null; cleared by the next success.
    [[nodiscard]] std::exception_ptr lastError() const { return _mutation.error(); }

    /// @brief An accordion group's collapsed state.
    /// @param group Index into `model().groups()`.
    /// @return The signal.
    /// @throws std::out_of_range for an index past the groups.
    [[nodiscard]] reactive::Signal<bool>& groupCollapsed(std::size_t group) const { return *_collapsed.at(group); }

    /// @brief A tab run's selected tab, keyed by the run's first group index.
    /// @param group Index into `model().groups()` of the run's first tab group.
    /// @return The signal holding the selected tab's position in the run.
    /// @throws std::out_of_range for an index past the groups.
    [[nodiscard]] reactive::Signal<std::size_t>& tabSelection(std::size_t group) const { return *_tabs.at(group); }

    /// @brief A group's title, resolved through the translations (key `<action>.group.<index>`).
    /// @param group Index into `model().groups()`.
    /// @return The title.
    [[nodiscard]] std::string groupTitle(std::size_t group) const {
        FieldGroupSpec const& spec = detail::engine::elementAt(_core->model().groups(), group);
        if (spec.implicit) {
            return {};
        }
        return render::resolveText(_core->options().translations, _core->options().bcp47, std::nullopt,
                                   i18n::groupKey(_core->model().actionType(), group), spec.title);
    }

private:
    [[nodiscard]] std::optional<std::string> assemble() const {
        std::string json = "{";
        bool first = true;
        for (auto const& field : _fields) {
            Encoding const& encoding = field->encoded().get();
            bool const required = field->required().get();
            std::string literal;
            if (!encoding.has_value()) {
                if (field->spec().blankAs == BlankAs::Empty && field->engaged().get() && !required) {
                    literal = R"("")";
                } else if (required) {
                    return std::nullopt;
                } else {
                    continue;
                }
            } else if (!encoding->has_value()) {
                return std::nullopt;
            } else {
                literal = (*encoding)->json;
            }
            json += (first ? "" : ",") + detail::engine::quote(field->spec().name) + ":" + literal;
            first = false;
        }
        if (gatingVerdict(_core->model().rules(), _core->lookup()) == Tri::False) {
            return std::nullopt;
        }
        return json + "}";
    }

    void autoSubmit() {
        std::optional<std::string> const& body = _body.get();
        if (_core->model().submitMode() != SubmitMode::Automatic || !body.has_value() || body == _lastSubmitted) {
            return;
        }
        _lastSubmitted = body;
        std::string const toSend = *body;
        runtime().untracked([&] { _mutation.run(toSend); });
    }

    std::unique_ptr<detail::engine::SessionCore> _core;
    Submitter _submit;
    std::vector<std::unique_ptr<FieldState>> _fields;
    std::vector<std::unique_ptr<reactive::Signal<bool>>> _collapsed;
    std::vector<std::unique_ptr<reactive::Signal<std::size_t>>> _tabs;
    reactive::Computed<std::optional<std::string>> _assembled;
    reactive::Computed<bool> _ready;
    reactive::Computed<std::optional<std::string>> _body;
    reactive::Mutation<std::string, std::string> _mutation;
    std::optional<std::string> _lastSubmitted;
    std::unique_ptr<reactive::Effect> _autoSubmit;
};

}  // namespace morph::forms
```


The member order is load-bearing and commented in place where it is: in `FieldState`, the signals and `_members`
come before the computeds that read them and the effects come last, so an effect is destroyed before anything it
reads; in `FormSession`, `_fields` precede the computeds over them, and `_autoSubmit` is last.

Register the header and the test as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][session]"`
Expected: PASS, 18 test cases.

Mutation check: in the `FormSession` constructor, make the `onProgrammatic` hook's body empty (`[this] {}` — keep
the capture so nothing else changes). Expected: FAIL in "prefilling a complete payload never submits; the next user
edit does" (a submission appears after `prefill`). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/form_session.hpp tests/forms_engine_support.hpp \
        tests/test_forms_engine_session.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): FormSession — drafts, readiness, body, submission, prefill and reset

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: Nested objects and collections of objects

"Nothing is unrepresentable": an `Object` field owns a `FieldState` per member, an `ObjectArray` owns `RowState`s
of member fields, and both encode by asking their members — the same encoders a top-level field uses, in the
display locale. A blank optional member is left out; a blank required one makes the container `incomplete`. A
collection encodes `[]` once engaged (a row was added or a payload loaded it) and, when required, also while it has
no rows: an empty list is a value the action accepts. A `truncated` member (Task 4) cannot be edited: blank it is
omitted, holding a loaded value it is `unrepresentable`, and a required truncated collection is `[]`.

**Files:**
- Modify: `include/morph/forms/engine/form_session.hpp` — `RowState`, the `FieldState` members and rows, and the
  path syntax of `FormSession::field` (each change below names the code it replaces)
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_nested.cpp` after `test_forms_engine_session.cpp`
- Test: `tests/test_forms_engine_nested.cpp`

**Interfaces:**
- Consumes: Task 6's `FieldState`, `FormSession`, `detail::engine::SessionCore`; Task 5's `fieldError`.
- Produces: `RowState` (`key()`, `fields()`, `field(std::string_view)`, `blank()`, `start()`); row keys start at
  1, so `addRow()`'s 0 means "not an editable collection";
  `FieldState::members() -> std::span<std::unique_ptr<FieldState> const>`, `member(std::string_view) ->
  FieldState*`, `rows() -> std::vector<std::shared_ptr<RowState>> const&` (tracked), `addRow() -> std::uint64_t`,
  `removeRow(std::uint64_t)`; `FormSession::field` accepting `parent.member` and `rows[i].member`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_nested.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::FormSessionOptions;
using morph::test::formsengine::Harness;

constexpr std::string_view kIgnition = R"({"type":"object","$defs":{"double":{"type":"number"},"int32_t":{"type":"integer","minimum":-2147483648,"maximum":2147483647}},
  "properties":{
  "method":{"type":"string","oneOf":[{"title":"Furnace","const":"Furnace"},{"title":"Infrared","const":"Infrared"}],"x-order":0},
  "specimen":{"type":"object","x-order":1,"required":["massOfContainer","massOfContainerAndSampleBeforeIgnition","testTemperature"],"properties":{
    "massOfContainer":{"$ref":"#/$defs/double","x-order":0,"ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":1},
    "massOfContainerAndSampleBeforeIgnition":{"$ref":"#/$defs/double","x-order":1,"ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":1},
    "readoutBinderContent":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":2,"ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-displayDecimals":2},
    "testTemperature":{"$ref":"#/$defs/int32_t","x-order":3}}},
  "calibrationFactor":{"$ref":"#/$defs/double","x-order":2},
  "driedSample":{"type":"boolean","x-order":3},
  "testedByNr":{"anyOf":[{"$ref":"#/$defs/int32_t"},{"type":"null"}],"x-order":4}},
  "required":["method","specimen","calibrationFactor","driedSample"]})";

constexpr std::string_view kDensity = R"({"type":"object","$defs":{"double":{"type":"number"},
  "lab::PycnometerDetermination":{"type":"object","required":["massPycnometerAndSample","excluded"],"properties":{
    "massPycnometerEmpty":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":0,"ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":2},
    "massPycnometerAndSample":{"$ref":"#/$defs/double","x-order":1,"ExtUnits":{"unitAscii":"g","unitUnicode":"g"},"x-displayDecimals":2},
    "excluded":{"type":"boolean","x-order":2},
    "computedDensity":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":3,"x-readonly":true}}}},
  "properties":{"data":{"type":"object","required":["useSpecificGravity"],"x-order":0,"properties":{
    "useSpecificGravity":{"type":"boolean","x-order":0},
    "testLiquidTemperature":{"anyOf":[{"$ref":"#/$defs/double"},{"type":"null"}],"x-order":1,"x-displayDecimals":1},
    "testLiquidName":{"type":["string","null"],"x-order":2},
    "determination1":{"anyOf":[{"$ref":"#/$defs/lab::PycnometerDetermination"},{"type":"null"}],"x-order":3},
    "determination2":{"anyOf":[{"$ref":"#/$defs/lab::PycnometerDetermination"},{"type":"null"}],"x-order":4}}}},
  "required":["data"]})";

constexpr std::string_view kGrading = R"({"$defs":{"double":{"type":"number"},"int64_t":{"type":"integer"},
  "Row":{"type":"object","required":["sieve","passing"],"properties":{
    "note":{"type":["string","null"],"x-order":2},
    "sieve":{"$ref":"#/$defs/double","x-order":0,"ExtUnits":{"unitAscii":"mm","unitUnicode":"mm"}},
    "passing":{"type":["object","null"],"properties":{"num":{"type":"integer"},"den":{"type":"integer"},"dp":{"type":"integer"}},"ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-decimalPlaces":1,"x-order":1},
    "order":{"$ref":"#/$defs/int64_t","x-order":3,"x-readonly":true}}}},
  "properties":{"rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":0},
    "tags":{"type":"array","items":{"type":"string"},"x-order":1}},"required":["rows"]})";

void fillIgnitionScalars(Harness& harness, morph::forms::FormSession& form) {
    harness.type(form, "method", R"("Furnace")");
    harness.type(form, "calibrationFactor", "0.25");
    harness.type(form, "driedSample", "true");
}

}  // namespace

TEST_CASE("nested: an object encodes member by member with each member's own encoder", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Ignition", kIgnition);
    fillIgnitionScalars(harness, *form);
    CHECK_FALSE(form->ready());
    harness.type(*form, "specimen.massOfContainer", "512.3");
    harness.type(*form, "specimen.massOfContainerAndSampleBeforeIgnition", "2012.8");
    harness.type(*form, "specimen.readoutBinderContent", "5.25");
    harness.type(*form, "specimen.testTemperature", "538");
    REQUIRE(form->ready());
    CHECK(harness.server.lastBody() ==
          R"({"method":"Furnace","specimen":{"massOfContainer":512.3,"massOfContainerAndSampleBeforeIgnition":2012.8,)"
          R"("readoutBinderContent":5.25,"testTemperature":538},"calibrationFactor":0.25,"driedSample":true})");
    CHECK(form->field("specimen.massOfContainer").path() == "specimen.massOfContainer");
}

TEST_CASE("nested: a member that does not encode, or a blank required member, keeps the form unready",
          "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Ignition", kIgnition);
    fillIgnitionScalars(harness, *form);
    harness.type(*form, "specimen.massOfContainerAndSampleBeforeIgnition", "2.0");
    harness.type(*form, "specimen.testTemperature", "538");
    CHECK_FALSE(form->ready());  // massOfContainer is required and blank
    CHECK(form->field("specimen").error().get()->code == "incomplete");
    harness.type(*form, "specimen.massOfContainer", "1.25");
    CHECK_FALSE(form->ready());  // finer than x-displayDecimals
    harness.type(*form, "specimen.massOfContainer", "abc");
    CHECK_FALSE(form->ready());
    harness.type(*form, "specimen.massOfContainer", "1.2");
    harness.type(*form, "specimen.testTemperature", "1.5");
    CHECK_FALSE(form->ready());
    harness.type(*form, "specimen.testTemperature", "538");
    CHECK(form->ready());
    CHECK(harness.server.submits().size() == 1);
}

TEST_CASE("nested: members read in the display locale", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Ignition", kIgnition,
                                FormSessionOptions{.locale = {.decimalSeparator = ",", .groupSeparator = "."}});
    harness.type(*form, "method", R"("Furnace")");
    harness.type(*form, "calibrationFactor", "0,25");
    harness.type(*form, "driedSample", "true");
    harness.type(*form, "specimen.massOfContainer", "1.512,3");
    harness.type(*form, "specimen.massOfContainerAndSampleBeforeIgnition", "2012,8");
    harness.type(*form, "specimen.readoutBinderContent", "5,25");
    harness.type(*form, "specimen.testTemperature", "538");
    CHECK(form->body() ==
          R"({"method":"Furnace","specimen":{"massOfContainer":1512.3,"massOfContainerAndSampleBeforeIgnition":2012.8,)"
          R"("readoutBinderContent":5.25,"testTemperature":538},"calibrationFactor":0.25,"driedSample":true})");
}

TEST_CASE("nested: two levels deep, blank optional leaves and objects are left out", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Density", kDensity);
    CHECK_FALSE(form->ready());  // data is required and blank
    harness.type(*form, "data.useSpecificGravity", "false");
    harness.type(*form, "data.testLiquidTemperature", "25.0");
    harness.type(*form, "data.testLiquidName", "water");
    harness.type(*form, "data.determination1.massPycnometerEmpty", "1450.10");
    harness.type(*form, "data.determination1.massPycnometerAndSample", "3450.25");
    harness.type(*form, "data.determination1.excluded", "false");
    CHECK(form->body() ==
          R"({"data":{"useSpecificGravity":false,"testLiquidTemperature":25.0,"testLiquidName":"water",)"
          R"("determination1":{"massPycnometerEmpty":1450.10,"massPycnometerAndSample":3450.25,"excluded":false}}})");
    form->reset();
    harness.settle();
    harness.type(*form, "data.useSpecificGravity", "true");
    harness.type(*form, "data.testLiquidName", "  ");
    CHECK(form->body() == R"({"data":{"useSpecificGravity":true}})");
    harness.type(*form, "data.determination2.massPycnometerEmpty", "10.00");
    CHECK_FALSE(form->ready());  // a partly filled optional object needs its required members
    harness.type(*form, "data.determination2.massPycnometerAndSample", "20.00");
    harness.type(*form, "data.determination2.excluded", "true");
    CHECK(form->body() == R"({"data":{"useSpecificGravity":true,"determination2":{"massPycnometerEmpty":10.00,)"
                          R"("massPycnometerAndSample":20.00,"excluded":true}}})");
}

TEST_CASE("nested: a required boolean member of a filled object answers false", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Density", kDensity);
    harness.type(*form, "data.testLiquidName", "water");
    CHECK(form->body() == R"({"data":{"useSpecificGravity":false,"testLiquidName":"water"}})");
}

TEST_CASE("nested: a prefill decodes into the members and round-trips", "[forms-engine][nested]") {
    constexpr std::string_view kStored =
        R"({"data":{"useSpecificGravity":false,"testLiquidTemperature":25.5,"determination1":{"massPycnometerEmpty":1450.1,)"
        R"("massPycnometerAndSample":3450.25,"excluded":true,"computedDensity":2.412}}})";
    Harness harness;
    auto const form = harness.session("T_Density", kDensity);
    form->prefill(kStored);
    harness.settle();
    CHECK(harness.server.submits().empty());
    CHECK(form->field("data.useSpecificGravity").text().peek() == "false");
    CHECK(form->field("data.testLiquidTemperature").text().peek() == "25.5");
    CHECK(form->field("data.determination1.massPycnometerEmpty").text().peek() == "1450.10");
    CHECK(form->field("data.determination1.excluded").text().peek() == "true");
    CHECK(form->field("data.determination2").blank());
    CHECK(form->body() ==
          R"({"data":{"useSpecificGravity":false,"testLiquidTemperature":25.5,"determination1":{"massPycnometerEmpty":1450.10,)"
          R"("massPycnometerAndSample":3450.25,"excluded":true,"computedDensity":2.412}}})");
    form->reset();
    harness.settle();
    CHECK(form->field("data").blank());
    CHECK_FALSE(form->ready());

    Harness german;
    auto const germanForm = german.session("T_Ignition", kIgnition,
                                   FormSessionOptions{.locale = {.decimalSeparator = ",", .groupSeparator = "."}});
    germanForm->prefill(R"({"method":"Infrared","calibrationFactor":0.5,"driedSample":false,"specimen":{"massOfContainer":512.3,)"
                R"("massOfContainerAndSampleBeforeIgnition":2012.8,"testTemperature":538}})");
    german.settle();
    CHECK(germanForm->field("specimen.massOfContainer").text().peek() == "512,3");
    CHECK(germanForm->field("specimen.readoutBinderContent").blank());
    CHECK(germanForm->body() == R"({"method":"Infrared","specimen":{"massOfContainer":512.3,)"
                        R"("massOfContainerAndSampleBeforeIgnition":2012.8,"testTemperature":538},"calibrationFactor":0.5,)"
                        R"("driedSample":false})");
}

TEST_CASE("nested: collection rows encode with each member's encoder; an empty required collection is []",
          "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Grading", kGrading);
    CHECK(form->body() == R"({"rows":[]})");
    auto& rows = form->field("rows");
    rows.addRow();
    harness.type(*form, "rows[0].sieve", "31.5");
    CHECK_FALSE(form->ready());  // passing is required and blank
    harness.type(*form, "rows[0].passing", "100.0");
    auto const second = rows.addRow();
    harness.type(*form, "rows[1].sieve", "0.063");
    harness.type(*form, "rows[1].passing", "4.2");
    harness.type(*form, "rows[1].note", "fines");
    CHECK(form->body() == R"({"rows":[{"sieve":31.5,"passing":{"num":1000,"den":10,"dp":1}},)"
                          R"({"sieve":0.063,"passing":{"num":42,"den":10,"dp":1},"note":"fines"}]})");
    harness.type(*form, "rows[1].passing", "1.25");
    CHECK_FALSE(form->ready());  // an over-precise Quantity cell is refused, not rounded
    harness.type(*form, "rows[1].passing", "1.2");
    CHECK(form->ready());
    rows.removeRow(rows.rows().front()->key());
    harness.settle();
    REQUIRE(rows.rows().size() == 1);
    CHECK(rows.rows().front()->key() == second);
    CHECK(form->field("rows[0].note").text().peek() == "fines");
    CHECK(form->field("rows[0].sieve").path() == "rows#" + std::to_string(second) + ".sieve");
}

TEST_CASE("nested: loaded cells read as their text; a reset empties the rows", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Grading", kGrading);
    form->prefill(R"({"rows":[{"sieve":2.5,"passing":{"num":30,"den":10,"dp":1},"order":7}]})");
    harness.settle();
    CHECK(form->body() == R"({"rows":[{"sieve":2.5,"passing":{"num":30,"den":10,"dp":1},"order":7}]})");
    CHECK(form->field("rows[0].passing").text().peek() == "3.0");
    form->reset();
    harness.settle();
    CHECK(form->field("rows").rows().empty());
    CHECK(form->body() == R"({"rows":[]})");
}

TEST_CASE("nested: rows inside an object reuse the row encoder", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Grading", R"({"$defs":{"Row":{"type":"object","required":["sieve","passing"],"properties":{
        "sieve":{"type":"number","x-order":0},
        "passing":{"type":["object","null"],"ExtUnits":{"unitAscii":"pct","unitUnicode":"%"},"x-decimalPlaces":1,"x-order":1}}}},
      "properties":{"grading":{"type":"object","x-order":0,"required":["label","rows"],"properties":{
        "label":{"type":"string","x-order":0},"rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":1}}}},
      "required":["grading"]})");
    harness.type(*form, "grading.label", "A");
    CHECK(form->body() == R"({"grading":{"label":"A","rows":[]}})");
    form->field("grading.rows").addRow();
    harness.type(*form, "grading.rows[0].sieve", "8");
    CHECK_FALSE(form->ready());
    harness.type(*form, "grading.rows[0].passing", "55.0");
    CHECK(form->body() == R"({"grading":{"label":"A","rows":[{"sieve":8,"passing":{"num":550,"den":10,"dp":1}}]}})");
}

TEST_CASE("nested: an acyclic aggregate is drawn and encoded; an optional one may be left out",
          "[forms-engine][nested]") {
    Harness harness;
    auto const acyclic = harness.session("AcyclicAction", R"({"properties":{"id":{"type":"integer","x-order":0},
      "address":{"type":"object","x-order":1,"required":["street","city"],"properties":{
        "street":{"type":"string","x-order":0},"city":{"type":"string","x-order":1}}}},"required":["id","address"]})");
    harness.type(*acyclic, "id", "7");
    harness.type(*acyclic, "address.street", "Main");
    CHECK_FALSE(acyclic->ready());
    harness.type(*acyclic, "address.city", "Springfield");
    CHECK(acyclic->body() == R"({"id":7,"address":{"street":"Main","city":"Springfield"}})");
    auto const optional = harness.session("OptionalNestedAction", R"({"properties":{"id":{"type":"integer","x-order":0},
      "note":{"type":"string","x-order":1},"address":{"type":"object","x-order":2,"required":["street"],
      "properties":{"street":{"type":"string","x-order":0}}}},"required":["id"]})");
    harness.type(*optional, "id", "7");
    CHECK(optional->body() == R"({"id":7})");
}

TEST_CASE("nested: a self-referential type stops where it repeats, and a value loaded there is unrepresentable",
          "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Tree", R"({"$defs":{"TreeNode":{"type":"object","required":["name"],"properties":{
        "name":{"type":"string","x-order":0},"child":{"anyOf":[{"$ref":"#/$defs/TreeNode"},{"type":"null"}],"x-order":1}}}},
      "properties":{"root":{"$ref":"#/$defs/TreeNode","x-order":0}},"required":["root"]})");
    REQUIRE(form->field("root").members().size() == 2);
    CHECK(form->field("root.child").spec().truncated);
    harness.type(*form, "root.name", "top");
    CHECK(harness.server.lastBody() == R"({"root":{"name":"top"}})");
    form->prefill(R"({"root":{"name":"top","child":{"name":"leaf"}}})");
    harness.settle();
    CHECK_FALSE(form->ready());
    CHECK(form->field("root.child").error().get()->code == "unrepresentable");

    auto const tree = harness.session("SelfAction", R"({"$defs":{"probe::TreeNode":{"type":"object","required":["name","children"],"properties":{
        "children":{"type":"array","items":{"$ref":"#/$defs/probe::TreeNode"},"x-order":1},"name":{"type":"string","x-order":0}}}},
      "properties":{"id":{"type":"integer","x-order":0},"root":{"$ref":"#/$defs/probe::TreeNode","x-order":1}},"required":["id","root"]})");
    harness.type(*tree, "id", "7");
    harness.type(*tree, "root.name", "top");
    CHECK(tree->field("root.children").spec().truncated);  // probe::TreeNode is already on the path
    CHECK(tree->body() == R"({"id":7,"root":{"name":"top","children":[]}})");
    CHECK(tree->field("root.children").addRow() == 0);      // a truncated collection takes no rows
    CHECK(tree->field("root.children").rows().empty());
}

TEST_CASE("nested: field() walks members and rows and refuses what is not there", "[forms-engine][nested]") {
    Harness harness;
    auto const form = harness.session("T_Grading", kGrading);
    CHECK_THROWS_AS(form->field("rows[0].sieve"), std::out_of_range);
    form->field("rows").addRow();
    CHECK(form->field("rows[0].sieve").spec().name == "sieve");
    CHECK_THROWS_AS(form->field("rows[0]"), std::out_of_range);
    CHECK_THROWS_AS(form->field("rows[x].sieve"), std::out_of_range);
    CHECK_THROWS_AS(form->field("rows[0].nope"), std::out_of_range);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: compile errors `no member named 'members' in 'morph::forms::FieldState'` and `no member named 'addRow'`.

- [ ] **Step 3: Implement**

In `include/morph/forms/engine/form_session.hpp`:

1. After `class FieldState;` add `class RowState;`, and directly before `/// @brief The reactive state of one field: its
   draft and everything derived from it.` insert:

```cpp
/// @brief One collection row: the element's member fields.
class RowState {
public:
    /// @param core      The session core. Borrowed.
    /// @param arraySpec The collection field's spec (its `children` are the members).
    /// @param arrayPath The collection field's path.
    /// @param key       The row's stable key.
    /// @param parent    The collection field.
    RowState(detail::engine::SessionCore& core, FieldSpec const& arraySpec, std::string const& arrayPath,
             std::uint64_t key, FieldState const* parent);
    ~RowState();
    RowState(RowState const&) = delete;
    RowState& operator=(RowState const&) = delete;
    RowState(RowState&&) = delete;
    RowState& operator=(RowState&&) = delete;

    /// @brief The row's stable key (not its position).
    /// @return The key.
    [[nodiscard]] std::uint64_t key() const noexcept { return _key; }

    /// @brief The member fields, in `x-order`.
    /// @return The fields.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> fields() const noexcept { return _fields; }

    /// @brief A member by wire name.
    /// @param name The wire name.
    /// @return The member, or null.
    [[nodiscard]] FieldState* field(std::string_view name) const noexcept;

    /// @brief Whether every member is blank. Tracked.
    /// @return `true` when nothing is entered in the row.
    [[nodiscard]] bool blank() const;

    /// @brief Starts every member; called once the row is built and loaded.
    void start();

private:
    std::uint64_t _key;
    std::vector<std::unique_ptr<FieldState>> _fields;
};
```

2. In `FieldState`'s constructor initializer list, after `_engaged{core.runtime(), false},` add
   `_rows{core.runtime(), std::vector<std::shared_ptr<RowState>>{}},`; at the start of the constructor body add:

```cpp
        if (_spec->kind == FieldKind::Object) {
            for (FieldSpec const& child : _spec->children) {
                _members.push_back(std::make_unique<FieldState>(core, child, _path + "." + child.name, this));
            }
        }
```

3. Replace `FieldState::blank()` (the one-line body) with:

```cpp
    /// @brief Whether nothing is entered: a blank draft, an Object whose members are all blank, a
    ///        collection with no rows that was never engaged. Tracked.
    /// @return `true` when blank.
    // NOLINTNEXTLINE(misc-no-recursion) -- an Object asks its members
    [[nodiscard]] bool blank() const {
        switch (_spec->kind) {
            case FieldKind::Object:
                if (_spec->truncated) {
                    return detail::engine::trim(_text.get()).empty();
                }
                return std::ranges::all_of(_members, [](auto const& entry) { return entry->blank(); });
            case FieldKind::ObjectArray:
                if (_spec->truncated) {
                    return detail::engine::trim(_text.get()).empty() && !_engaged.get();
                }
                return _rows.get().empty() && !_engaged.get();
            default:
                return detail::engine::trim(_text.get()).empty();
        }
    }
```

4. After `switchUnit`, add:

```cpp
    /// @brief An Object's members, in `x-order`.
    /// @return The members; empty for every other kind.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> members() const noexcept { return _members; }

    /// @brief An Object's member by wire name.
    /// @param name The wire name.
    /// @return The member, or null.
    [[nodiscard]] FieldState* member(std::string_view name) const noexcept {
        auto const found = std::ranges::find_if(_members, [&](auto const& entry) { return entry->spec().name == name; });
        return found == _members.end() ? nullptr : found->get();
    }

    /// @brief A collection's rows, in order. Tracked.
    /// @return The rows; empty for every other kind.
    [[nodiscard]] std::vector<std::shared_ptr<RowState>> const& rows() const { return _rows.get(); }

    /// @brief Appends a blank row to a collection and engages it. A user action.
    /// @return The new row's key; 0 when the field is not a collection, or a truncated one.
    std::uint64_t addRow() {
        if (_spec->kind != FieldKind::ObjectArray || _spec->truncated) {
            return 0;
        }
        auto row = std::make_shared<RowState>(*_core, *_spec, _path, _nextRowKey++, this);
        row->start();
        std::uint64_t const key = row->key();
        _core->runtime().batch([&] {
            _rows.mutate([&](auto& rows) { rows.push_back(std::move(row)); });
            _engaged.set(true);
        });
        return key;
    }

    /// @brief Removes a collection row by key. A user action. A mounted view that still holds the
    ///        row keeps its state alive until it unmounts it.
    /// @param key The row's key.
    void removeRow(std::uint64_t key) {
        _rows.mutate([&](auto& rows) { std::erase_if(rows, [&](auto const& row) { return row->key() == key; }); });
    }
```

5. Replace `FieldState::load`, `clear` and `start` with:

```cpp
    /// @brief Replaces the draft with the decoded wire value; null or absent is blank. Does not
    ///        mark the change programmatic — the caller does, in the same batch.
    /// @param value The wire value, or null.
    // NOLINTNEXTLINE(misc-no-recursion) -- an Object loads its members
    void load(detail::engine::Json const* value) {
        using detail::engine::Json;
        bool const present = value != nullptr && !value->is_null();
        if (_spec->truncated) {
            _text.set(present ? detail::engine::toJson(*value) : std::string{});
            _engaged.set(present && value->is_array());
            return;
        }
        switch (_spec->kind) {
            case FieldKind::Object:
                for (auto const& entry : _members) {
                    entry->load(present ? detail::engine::member(*value, entry->spec().name) : nullptr);
                }
                return;
            case FieldKind::ObjectArray: {
                std::vector<std::shared_ptr<RowState>> loaded;
                if (present && value->is_array()) {
                    for (Json const& element : value->get_array()) {
                        auto row = std::make_shared<RowState>(*_core, *_spec, _path, _nextRowKey++, this);
                        for (auto const& cell : row->fields()) {
                            cell->load(detail::engine::member(element, cell->spec().name));
                        }
                        row->start();
                        loaded.push_back(std::move(row));
                    }
                }
                _rows.set(std::move(loaded));
                _engaged.set(present && value->is_array());
                return;
            }
            default:
                _text.set(present ? decodeScalar(*_spec, *value, _core->values()) : std::string{});
                _unit.set(0);
                _engaged.set(_spec->blankAs == BlankAs::Empty && present && value->is_string());
                return;
        }
    }

    /// @brief Blanks the draft, the unit, the engagement, the members and the rows.
    // NOLINTNEXTLINE(misc-no-recursion) -- an Object clears its members
    void clear() {
        _text.set(std::string{});
        _unit.set(0);
        _engaged.set(false);
        for (auto const& entry : _members) {
            entry->clear();
        }
        _rows.set({});
    }

    /// @brief Starts what needs every sibling to exist: an `x-blankAs` field's engagement, and
    ///        every member's own. Called once, after the owner built all its fields.
    // NOLINTNEXTLINE(misc-no-recursion) -- an Object starts its members
    void start() {
        for (auto const& entry : _members) {
            entry->start();
        }
        if (_spec->blankAs == BlankAs::Empty) {
            _engage = std::make_unique<reactive::Effect>(_core->runtime(), [this] {
                if (!detail::engine::trim(_text.get()).empty() && !_engaged.peek()) {
                    _engaged.set(true);
                }
            });
        }
    }
```

6. Replace `FieldState::computeEncoding` with (and add `encodeMembers` above it):

```cpp
    // NOLINTNEXTLINE(misc-no-recursion) -- an Object or a row encodes its members
    [[nodiscard]] std::expected<std::string, FieldError> encodeMembers(
        std::span<std::unique_ptr<FieldState> const> members) const {
        std::string json = "{";
        bool first = true;
        for (auto const& entry : members) {
            Encoding const& encoding = entry->encoded().get();
            if (!encoding.has_value()) {
                if (entry->spec().required) {
                    return std::unexpected(fieldError("incomplete", _core->values()));
                }
                continue;
            }
            if (!encoding->has_value()) {
                return std::unexpected(encoding->error());
            }
            json += (first ? "" : ",") + detail::engine::quote(entry->spec().name) + ":" + (*encoding)->json;
            first = false;
        }
        return json + "}";
    }

    // NOLINTNEXTLINE(misc-no-recursion) -- an Object or a row encodes its members
    [[nodiscard]] Encoding computeEncoding() const {
        if (_spec->truncated) {
            std::string_view const draft = detail::engine::trim(_text.get());
            bool const array = _spec->kind == FieldKind::ObjectArray;
            if (draft.empty() && !_engaged.get()) {
                return (array && _spec->required) ? Encoding{WireValue{"[]", std::monostate{}}} : Encoding{};
            }
            if (array && (draft == "[]" || draft.empty())) {
                return WireValue{"[]", std::monostate{}};
            }
            return std::unexpected(fieldError("unrepresentable", _core->values()));
        }
        switch (_spec->kind) {
            case FieldKind::Object: {
                if (blank()) {
                    return std::nullopt;
                }
                auto json = encodeMembers(_members);
                if (!json) {
                    return std::unexpected(json.error());
                }
                return WireValue{*std::move(json), std::monostate{}};
            }
            case FieldKind::ObjectArray: {
                auto const& rows = _rows.get();
                if (rows.empty() && !_engaged.get() && !_spec->required) {
                    return std::nullopt;
                }
                std::string json = "[";
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    auto row = encodeMembers(rows.at(i)->fields());
                    if (!row) {
                        return std::unexpected(row.error());
                    }
                    json += (i == 0 ? "" : ",") + *row;
                }
                return WireValue{json + "]", std::monostate{}};
            }
            default:
                return encodeScalar(*_spec, _text.get(), _unit.get(), _core->values());
        }
    }
```

7. In `FieldState`'s members, add `std::uint64_t _nextRowKey = 1;` after `_placeholder`, and after
   `reactive::Signal<bool> _engaged;` add:

```cpp
    reactive::Signal<std::vector<std::shared_ptr<RowState>>> _rows;
    std::vector<std::unique_ptr<FieldState>> _members;
```

8. After the closing `};` of `FieldState`, add the `RowState` definitions:

```cpp
inline RowState::RowState(detail::engine::SessionCore& core, FieldSpec const& arraySpec, std::string const& arrayPath,
                          std::uint64_t key, FieldState const* parent)
    : _key{key} {
    for (FieldSpec const& member : arraySpec.children) {
        _fields.push_back(
            std::make_unique<FieldState>(core, member, arrayPath + "#" + std::to_string(key) + "." + member.name, parent));
    }
}

inline RowState::~RowState() = default;

inline FieldState* RowState::field(std::string_view name) const noexcept {
    auto const found = std::ranges::find_if(_fields, [&](auto const& entry) { return entry->spec().name == name; });
    return found == _fields.end() ? nullptr : found->get();
}

inline bool RowState::blank() const {
    return std::ranges::all_of(_fields, [](auto const& entry) { return entry->blank(); });
}

inline void RowState::start() {
    for (auto const& entry : _fields) {
        entry->start();
    }
}
```

9. Replace `FormSession::field` with:

```cpp
    /// @brief A field by path: `name`, `parent.member`, or `rows[i].member` (a row by position).
    /// @param path The path.
    /// @return The field.
    /// @throws std::out_of_range when no field has that path.
    [[nodiscard]] FieldState& field(std::string_view path) const {
        FieldState* current = nullptr;
        std::span<std::unique_ptr<FieldState> const> scope = _fields;
        std::string_view rest = path;
        auto const missing = [&] { return std::out_of_range{"FormSession::field: no field '" + std::string{path} + "'"}; };
        while (!rest.empty()) {
            auto const dot = rest.find('.');
            std::string_view segment = rest.substr(0, dot);
            rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
            std::optional<std::size_t> index;
            if (auto const bracket = segment.find('['); bracket != std::string_view::npos) {
                if (!segment.ends_with(']')) {
                    throw missing();
                }
                std::string_view const digits = segment.substr(bracket + 1, segment.size() - bracket - 2);
                std::size_t parsed = 0;
                auto const result = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
                if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
                    throw missing();
                }
                index = parsed;
                segment = segment.substr(0, bracket);
            }
            auto const found =
                std::ranges::find_if(scope, [&](auto const& entry) { return entry->spec().name == segment; });
            if (found == scope.end()) {
                throw missing();
            }
            current = found->get();
            if (index.has_value()) {
                auto const& rows = current->rows();
                if (*index >= rows.size() || rest.empty()) {
                    throw missing();
                }
                scope = rows.at(*index)->fields();
            } else {
                scope = current->members();
                if (!rest.empty() && scope.empty()) {
                    throw missing();
                }
            }
        }
        if (current == nullptr) {
            throw missing();
        }
        return *current;
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine]"`
Expected: PASS — the 11 new `[nested]` cases and every earlier `[forms-engine]` case.

Mutation check: in `computeEncoding`'s `ObjectArray` case, drop `&& !_spec->required` from the blank test. Expected:
FAIL in "collection rows encode … an empty required collection is []" (the first body is `nullopt`). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/form_session.hpp tests/test_forms_engine_nested.cpp tests/CMakeLists.txt
git commit -m "wip(forms): nested objects and collections of objects in FormSession

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Choice options — a Query keyed on the parents, latest wins, stale selections cleared

A Choice's options are a `reactive::Query<std::string, std::string>` whose key is the options request body:
`{}` for an independent Choice, `{"parent":<literal>,…}` once every `x-optionsDependsOn` parent encodes, idle
(`nullopt`) while one does not. So a parent change refetches, a reply for a superseded parent value is dropped (the
query's latest-wins), and an idle query has no options. A selection no longer among the options is cleared — a
programmatic change, so it never submits — but only once the query has answered for the current parents: a
selection prefilled before its options arrive survives the fetch.

**Files:**
- Modify: `include/morph/forms/engine/form_session.hpp` — `FieldState` (constructor, `options`, `optionsPending`,
  `start`, three private helpers, four members) and the three places a `FieldState` is constructed
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_choice.cpp` after `test_forms_engine_nested.cpp`
- Test: `tests/test_forms_engine_choice.cpp`

**Interfaces:**
- Consumes: Part 1's `reactive::Query<A, R>(Runtime&, Fetch, Key)` with `pending()`, `value()`, `error()`;
  Task 5's `parseOptions`; Task 6/7's `FieldState`.
- Produces: `FieldState(SessionCore&, FieldSpec const&, std::string path, std::vector<std::unique_ptr<FieldState>>
  const* siblings, FieldState const* parent)` (the constructor gains `siblings`); `FieldState::options()` returning
  a Choice's fetched options; `FieldState::optionsPending()`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_choice.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"

namespace {

using morph::test::formsengine::Harness;
using morph::test::formsengine::modelOf;

constexpr std::string_view kShipTo = R"({"properties":{
  "country":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListCountries","x-optionValue":"id","x-optionLabel":"name"},
  "city":{"type":["integer","null"],"x-order":1,"x-optionsAction":"ListCities","x-optionValue":"id","x-optionLabel":"name","x-optionsDependsOn":["country"]}},
  "required":["country","city"]})";

/// Countries 1 and 2; cities 10 in country 1 and nothing else.
[[nodiscard]] std::optional<std::string> geography(morph::test::formsengine::Call const& call) {
    if (call.action == "ListCountries") {
        return R"([{"id":1,"name":"One"},{"id":2,"name":"Two"}])";
    }
    if (call.body == R"({"country":1})") {
        return R"([{"id":10,"name":"Ten"}])";
    }
    return "[]";
}

[[nodiscard]] std::vector<std::string> labels(morph::forms::FieldState const& field) {
    std::vector<std::string> out;
    for (auto const& option : field.options()) {
        out.push_back(option.label);
    }
    return out;
}

}  // namespace

TEST_CASE("choice: only an independent Choice fetches on construction, with an empty body", "[forms-engine][choice]") {
    Harness harness;
    auto const form = harness.session("ShipTo", kShipTo);
    REQUIRE(harness.server.fetches().size() == 1);
    CHECK(harness.server.fetches()[0].action == "ListCountries");
    CHECK(harness.server.fetches()[0].body == "{}");
}

TEST_CASE("choice: a dependent Choice refetches with its parents' literals when a parent changes",
          "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith(geography);
    auto const form = harness.session("ShipTo", kShipTo);
    harness.type(*form, "country", "1");
    REQUIRE(harness.server.fetches().size() == 2);
    CHECK(harness.server.fetches()[1].action == "ListCities");
    CHECK(harness.server.fetches()[1].body == R"({"country":1})");
    harness.type(*form, "country", "");
    CHECK(harness.server.fetches().size() == 2);  // a blank parent is no request
}

TEST_CASE("choice: a selection the new options no longer back is cleared, without submitting",
          "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith(geography);
    auto const form = harness.session("ShipTo", kShipTo);
    harness.type(*form, "country", "1");
    harness.type(*form, "city", "10");
    CHECK(form->field("city").text().peek() == "10");
    auto const submitted = harness.server.submits().size();
    harness.type(*form, "country", "2");
    CHECK(form->field("city").text().peek().empty());
    CHECK(harness.server.submits().size() == submitted);
}

// Review Focus 2.
TEST_CASE("choice: a reply for a superseded parent value is dropped, and a prefilled selection survives its refetch",
          "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) { return std::nullopt; });  // settle by hand
    auto const form = harness.session("ShipTo", kShipTo);
    harness.server.resolveFetch(0, R"([{"id":1,"name":"A"},{"id":2,"name":"B"}])");
    harness.settle();
    CHECK(labels(form->field("country")) == std::vector<std::string>{"A", "B"});
    harness.type(*form, "country", "1");
    harness.type(*form, "country", "2");
    REQUIRE(harness.server.fetches().size() == 3);
    CHECK(form->field("city").optionsPending());
    harness.server.resolveFetch(1, R"([{"id":10,"name":"Old"}])");
    harness.settle();
    CHECK(form->field("city").options().empty());  // the reply for country 1 is stale
    harness.server.resolveFetch(2, R"([{"id":20,"name":"New"}])");
    harness.settle();
    CHECK(labels(form->field("city")) == std::vector<std::string>{"New"});

    form->prefill(R"({"country":1,"city":10})");
    harness.settle();
    REQUIRE(harness.server.fetches().size() == 4);
    CHECK(form->field("city").text().peek() == "10");  // kept while its options are in flight
    harness.server.resolveFetch(3, R"([{"id":10,"name":"Old"}])");
    harness.settle();
    CHECK(form->field("city").text().peek() == "10");
    CHECK(form->body() == R"({"country":1,"city":10})");
}

TEST_CASE("choice: a failed fetch keeps the selection; an answered one without it clears it",
          "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session("Probe", R"({"properties":{"slot":{"type":"integer","x-optionsAction":"ListSlots","x-order":0}},"required":["slot"]})");
    form->prefill(R"({"slot":4})");
    harness.settle();
    harness.server.rejectFetch(0, "options unavailable");
    harness.settle();
    CHECK(form->field("slot").text().peek() == "4");
    CHECK(form->body() == R"({"slot":4})");

    Harness answered;
    answered.server.optionsWith([](auto const&) { return std::optional<std::string>{R"([{"id":5,"name":"Five"}])"}; });
    auto const other = answered.session("Probe", R"({"properties":{"slot":{"type":"integer","x-optionsAction":"ListSlots","x-order":0}},"required":["slot"]})");
    answered.settle();
    other->prefill(R"({"slot":4})");
    answered.settle();
    CHECK(other->field("slot").text().peek().empty());
}

TEST_CASE("choice: option values and labels keep ids beyond 2^53 distinct", "[forms-engine][choice]") {
    Harness harness;
    constexpr std::string_view kRows = R"({"rows":[{"id":9007199254740993,"name":"Alpha"},{"id":9007199254740992,"name":"Beta"}]})";
    harness.server.optionsWith([&](auto const&) { return std::optional<std::string>{kRows}; });
    auto const byName = harness.session("Probe", R"({"properties":{"slot":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListRows","x-optionValue":"id","x-optionLabel":"name"}},"required":["slot"]})");
    auto const byId = harness.session("Probe", R"({"properties":{"slot":{"type":["integer","null"],"x-order":0,"x-optionsAction":"ListRows","x-optionValue":"id","x-optionLabel":"id"}},"required":["slot"]})");
    harness.settle();
    auto const& options = byName->field("slot").options();
    REQUIRE(options.size() == 2);
    CHECK(options[0].valueJson == "9007199254740993");
    CHECK(options[1].valueJson == "9007199254740992");
    CHECK(labels(byId->field("slot")) == std::vector<std::string>{"9007199254740993", "9007199254740992"});
    harness.type(*byName, "slot", options[0].valueJson);
    CHECK(byName->body() == R"({"slot":9007199254740993})");
}

TEST_CASE("choice: a prefilled selection is shown once its options arrive", "[forms-engine][choice]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) {
        return std::optional<std::string>{R"([{"id":9007199254740993,"name":"Big"},{"id":2,"name":"Two"}])"};
    });
    auto const form = harness.session("T_Choice", R"({"properties":{"sample":{"type":"integer","x-optionsAction":"ListSamples","x-order":0}},"required":["sample"]})");
    form->prefill(R"({"sample":9007199254740993})");
    harness.settle();
    CHECK(form->body() == R"({"sample":9007199254740993})");
    auto const& options = form->field("sample").options();
    REQUIRE(options.size() == 2);
    CHECK(options[0].valueJson == form->field("sample").text().peek());
    CHECK(options[0].label == "Big");
}

TEST_CASE("choice: a form with no Choice needs no fetcher; one with a Choice gets its options",
          "[forms-engine][choice]") {
    Harness harness;
    morph::forms::FormSession const plain{
        harness.rt,
        modelOf("CreateNote",
                R"({"properties":{"title":{"type":"string","x-order":0},"count":{"type":"integer","x-order":1}},"required":["title"]})"),
        harness.server.submitter(), morph::forms::ChoiceFetcher{}};
    CHECK(plain.fields().size() == 2);
    CHECK(harness.server.fetches().empty());

    harness.server.optionsWith([](auto const&) { return std::optional<std::string>{R"([{"id": 4, "name": "Bravo"}])"}; });
    auto const choice = harness.session(
        "MoveCard",
        R"({"properties":{"columnId":{"type":"integer","x-order":0,"x-optionsAction":"ListColumns","x-optionValue":"id","x-optionLabel":"name"}},"required":["columnId"]})");
    harness.settle();
    CHECK(labels(choice->field("columnId")) == std::vector<std::string>{"Bravo"});
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][choice]"`
Expected: FAIL in "only an independent Choice fetches on construction…" (`harness.server.fetches().size() == 1` is `0 == 1`);
every other `[choice]` case fails on its first fetch or options assertion.

- [ ] **Step 3: Implement**

In `include/morph/forms/engine/form_session.hpp`:

1. `FieldState`'s constructor takes the siblings a Choice's `dependsOn` names. Change its parameter list and
   documentation to:

```cpp
    /// @param core     The session core. Borrowed: it must outlive the field.
    /// @param spec     The field's spec. Borrowed from the core's model (or a parent's children).
    /// @param path     The instance path (`name`, `parent.member`, `rows#key.member`).
    /// @param siblings The fields `x-optionsDependsOn` names are looked up among. Borrowed.
    /// @param parent   The containing Object or ObjectArray field, or null at the top level.
    FieldState(detail::engine::SessionCore& core MORPH_LIFETIMEBOUND, FieldSpec const& spec MORPH_LIFETIMEBOUND,
               std::string path, std::vector<std::unique_ptr<FieldState>> const* siblings, FieldState const* parent)
```

   add `_siblings{siblings},` to the initializer list after `_path{std::move(path)},`, and update the three
   constructions: in the `FieldState` constructor body
   `std::make_unique<FieldState>(core, child, _path + "." + child.name, &_members, this)`; in `RowState`'s
   constructor `std::make_unique<FieldState>(core, member, arrayPath + "#" + std::to_string(key) + "." + member.name,
   &_fields, parent)`; in `FormSession`'s constructor `std::make_unique<FieldState>(*_core, spec, spec.name, &_fields,
   nullptr)`.

2. Replace `options()` and `optionsPending()` with:

```cpp
    /// @brief The selectable options: an Enum's from the schema, a Choice's as last fetched for
    ///        its current parents. Tracked.
    /// @return The options; empty for every other kind, and for a Choice with nothing fetched.
    [[nodiscard]] std::vector<ChoiceOption> const& options() const {
        if (_fetched) {
            return _fetched->get();
        }
        return _spec->enumOptions;
    }

    /// @brief Whether a Choice's options request is in flight. Tracked.
    /// @return `true` while fetching.
    [[nodiscard]] bool optionsPending() const { return _optionsQuery != nullptr && _optionsQuery->pending(); }
```

3. In `start()`, after the `_engage` block, add:

```cpp
        if (_spec->kind == FieldKind::Choice && _spec->choice.has_value() && _core->choices()) {
            reactive::Runtime& runtime = _core->runtime();
            _optionsQuery = std::make_unique<reactive::Query<std::string, std::string>>(
                runtime,
                [this](std::string const& request) {
                    _issued = request;
                    return _core->choices()(_spec->choice->optionsAction, request);
                },
                [this] { return optionsRequest(); });
            _fetched = std::make_unique<reactive::Computed<std::vector<ChoiceOption>>>(runtime, [this] {
                std::optional<std::string> const& reply = _optionsQuery->value();
                return reply.has_value() ? parseOptions(*reply, *_spec->choice) : std::vector<ChoiceOption>{};
            });
            _clearStale = std::make_unique<reactive::Effect>(runtime, [this] { clearStaleSelection(); });
        }
```

4. Add to `FieldState`'s private section, after `resolveSlot`:

```cpp
    [[nodiscard]] FieldState* sibling(std::string_view name) const {
        if (_siblings == nullptr) {
            return nullptr;
        }
        auto const found =
            std::ranges::find_if(*_siblings, [&](auto const& entry) { return entry->spec().name == name; });
        return found == _siblings->end() ? nullptr : found->get();
    }

    /// The options request for the current parents, or nullopt while one does not encode.
    [[nodiscard]] std::optional<std::string> optionsRequest() const {
        std::string body = "{";
        bool first = true;
        for (std::string const& parent : _spec->choice->dependsOn) {
            FieldState const* const source = sibling(parent);
            if (source == nullptr) {
                return std::nullopt;
            }
            Encoding const& encoding = source->encoded().get();
            if (!encoding.has_value() || !encoding->has_value()) {
                return std::nullopt;
            }
            body += (first ? "" : ",") + detail::engine::quote(parent) + ":" + (*encoding)->json;
            first = false;
        }
        return body + "}";
    }

    /// Clears a selection the options no longer back — once the query has answered for the
    /// current parents, so a value set before its options arrive survives the fetch.
    void clearStaleSelection() {
        if (_optionsQuery->pending() || _optionsQuery->error() != nullptr) {
            return;
        }
        std::optional<std::string> const request = optionsRequest();
        if (request.has_value() && request != _issued) {
            return;
        }
        std::vector<ChoiceOption> const& current = _fetched->get();
        std::string_view const selected = detail::engine::trim(_text.get());
        if (selected.empty() ||
            std::ranges::any_of(current, [&](ChoiceOption const& option) { return option.valueJson == selected; })) {
            return;
        }
        _core->runtime().batch([&] {
            _text.set(std::string{});
            _core->markProgrammatic();
        });
    }
```

5. In `FieldState`'s members: add `std::vector<std::unique_ptr<FieldState>> const* _siblings;` after `_path`,
   `std::optional<std::string> _issued;` after `_nextRowKey`, and directly before `_engage`:

```cpp
    std::unique_ptr<reactive::Query<std::string, std::string>> _optionsQuery;
    std::unique_ptr<reactive::Computed<std::vector<ChoiceOption>>> _fetched;
    std::unique_ptr<reactive::Effect> _clearStale;
```

`_clearStale` reads `_fetched` and `_optionsQuery`, which are declared before it, so it is destroyed first.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine]"`
Expected: PASS — the 8 new `[choice]` cases and every earlier `[forms-engine]` case.

Mutation check: in `clearStaleSelection`, change the first guard to `if (_optionsQuery->error() != nullptr)` (drop
the `pending()` test). Expected: FAIL in "a reply for a superseded parent value is dropped, and a prefilled selection
survives its refetch" — while the prefill's refetch is in flight the query still holds the previous options
(`[New]`), which do not contain `10`, so the selection is cleared. Restore it. The `request != _issued` guard
covers the other order — a flush that reaches the clearing Effect before the query's own has issued for new parents;
`Query`'s Effect subscribes first, so no test can force that order today, and the guard is kept for the case where
a later change reorders the subscriptions.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/form_session.hpp tests/test_forms_engine_choice.cpp tests/CMakeLists.txt
git commit -m "wip(forms): Choice options as a Query keyed on the parents, stale selections cleared

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: The shared corpora through the engine

The rule corpus, the instance-bounds corpus and the conformance fixtures already pin the schema contract against
the compiled evaluators. Here each one also drives a real `FormSession`, so the engine is held to the same rows. The
two data files move to `tests/data/`, where the engine's reader and the QML suite both read them; the QML copies
stay in place, unread, until the QML renderer is retired.

**Files:**
- Create: `tests/data/rule_corpus.json` (a copy of `src/qt/forms/tests/data/rule_corpus.json`, `readme` updated)
- Create: `tests/data/instance_bounds.json` (a copy of `src/qt/forms/tests/data/instance_bounds.json`, `readme`
  updated)
- Modify: `tests/CMakeLists.txt` — the two `MORPH_FORMS_*` compile definitions and the comment above them
- Modify: `src/qt/forms/CMakeLists.txt` — the same two definitions on `morph_forms_qml_tests` (lines 80–82)
- Modify: `tests/test_forms_rule_corpus.cpp`, `tests/forms_rule_corpus.hpp`, `tests/test_forms_instance_constraints.cpp`,
  `src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml`, `src/qt/forms/tests/tst_DynamicFormInstanceBounds.qml`,
  `src/qt/forms/tests/tst_main.cpp` — the comments naming the data files' location and readers
- Modify: `tests/test_forms_engine_rules.cpp` — append the corpus cases
- Modify: `tests/test_forms_rule_agreement.cpp`, `tests/test_forms_instance_constraints.cpp`,
  `tests/test_forms_conformance_corpus.cpp` — append the engine cases
- Test: the four files above

**Interfaces:**
- Consumes: `morph::test::rulecorpus::{corpusActions, findAction, Draft}` (`tests/forms_rule_corpus.hpp`);
  `boundsCorpus()` (anonymous namespace in `tests/test_forms_instance_constraints.cpp:503`); the `CF*` fixtures
  (`tests/test_forms_conformance_corpus.cpp`); `RuleAgreementAction` (`tests/test_forms_rule_agreement.cpp:39`);
  Tasks 6–8.
- Produces: nothing new in the library.

- [ ] **Step 1: Write the failing test**

Move the data first, so the new cases and the old readers point at one place:

```bash
mkdir -p tests/data
cp src/qt/forms/tests/data/rule_corpus.json tests/data/rule_corpus.json
cp src/qt/forms/tests/data/instance_bounds.json tests/data/instance_bounds.json
```

In `tests/data/rule_corpus.json`, replace the first six `readme` lines (through `"different case lists. See …"`)
with:

```json
    "The shared x-rules corpus. ONE file, THREE readers:",
    "  tests/test_forms_rule_corpus.cpp          -- morph::forms::allRulesSatisfied",
    "  tests/test_forms_engine_rules.cpp         -- the forms engine's FormSession",
    "  src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml -- a real DynamicForm",
    "No reader owns the cases, so the evaluators cannot be pinned to different",
    "case lists. See docs/spec/forms/forms.md, 'Two evaluators, one corpus'.",
```

In `tests/data/instance_bounds.json`, replace the first three `readme` lines with:

```json
    "The per-instance constraints corpus. ONE file, THREE readers:",
    "  tests/test_forms_instance_constraints.cpp -- InstanceConstraints::checkValue, and the forms engine",
    "  src/qt/forms/tests/tst_DynamicFormInstanceBounds.qml -- a real DynamicForm",
```

In `tests/CMakeLists.txt`, replace the comment and definitions block that starts `# The shared x-rules corpus. One
file, two readers:` with:

```cmake
# The shared x-rules and instance-bounds corpora. Each is one file with several
# readers -- this suite (compiled evaluators and the forms engine) and the QML
# suite -- all pointed at it from CMake so none can quietly grow its own copy.
target_compile_definitions(morph_tests PRIVATE
    MORPH_FORMS_RULE_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/data/rule_corpus.json"
    MORPH_FORMS_INSTANCE_BOUNDS="${CMAKE_CURRENT_SOURCE_DIR}/data/instance_bounds.json")
```

In `src/qt/forms/CMakeLists.txt`, change the two definitions on `morph_forms_qml_tests` to
`"${PROJECT_SOURCE_DIR}/tests/data/rule_corpus.json"` and `"${PROJECT_SOURCE_DIR}/tests/data/instance_bounds.json"`,
and in the comment above them replace `this suite and tests/test_forms_rule_corpus.cpp` with
`this suite, tests/test_forms_rule_corpus.cpp and tests/test_forms_engine_rules.cpp`.

In the comments of `tests/test_forms_rule_corpus.cpp` (lines 13–16), `tests/forms_rule_corpus.hpp` (lines 18–21),
`tests/test_forms_instance_constraints.cpp` (line 438), `src/qt/forms/tests/tst_main.cpp` (lines 24–28),
`src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml` (line 12) and `src/qt/forms/tests/tst_DynamicFormInstanceBounds.qml`
(line 18), replace `src/qt/forms/tests/data/` and `data/` (as the location of the two files) with `tests/data/`, and
where a comment says "one file, two readers" name the engine's reader as the third.

Append to `tests/test_forms_engine_rules.cpp` (add `#include <fstream>`, `#include <sstream>`, `#include <set>`,
`#include "forms_engine_support.hpp"` and `#include "forms_rule_corpus.hpp"` to its includes):

```cpp
// ──── The shared corpus, through the forms engine ──────────────────────────────

namespace {

using morph::test::formsengine::Harness;
using morph::test::rulecorpus::Draft;

struct EngineRow {
    std::string id{};
    std::string action{};
    Draft state{};
    bool ready = false;
    std::map<std::string, bool, std::less<>> visible{};
    std::map<std::string, bool, std::less<>> readonly{};
};

struct EngineCorpus {
    std::map<std::string, std::string, std::less<>> schemas{};
    std::vector<EngineRow> rows{};
};

[[nodiscard]] EngineCorpus const& engineCorpus() {
    static EngineCorpus const parsed = [] {
        std::ifstream file{MORPH_FORMS_RULE_CORPUS};
        REQUIRE(file.is_open());
        std::ostringstream buffer;
        buffer << file.rdbuf();
        auto const dom = engine::parseJson(buffer.str());
        REQUIRE(dom.has_value());
        EngineCorpus corpus{};
        for (auto const& [name, schema] : engine::member(*dom, "schemas")->get_object()) {
            corpus.schemas.emplace(name, schema.get<std::string>());
        }
        for (auto const& entry : engine::member(*dom, "cases")->get_array()) {
            EngineRow row{};
            row.id = *engine::stringAt(entry, "id");
            row.action = *engine::stringAt(entry, "action");
            row.ready = engine::member(entry, "ready")->get<bool>();
            for (auto const& [field, text] : engine::member(entry, "state")->get_object()) {
                row.state.emplace(field, text.get<std::string>());
            }
            for (std::string_view const key : {"visible", "readonly"}) {
                if (auto const* const flags = engine::member(entry, key)) {
                    for (auto const& [field, flag] : flags->get_object()) {
                        (key == "visible" ? row.visible : row.readonly).emplace(field, flag.get<bool>());
                    }
                }
            }
            corpus.rows.push_back(std::move(row));
        }
        REQUIRE(corpus.rows.size() == 46);
        return corpus;
    }();
    return parsed;
}

struct EngineVerdict {
    bool ready = false;
    std::map<std::string, bool, std::less<>> visible{};
    std::map<std::string, bool, std::less<>> readonly{};
};

[[nodiscard]] EngineVerdict runRow(EngineRow const& row) {
    Harness harness;
    auto const form = harness.session("Test_" + row.action, engineCorpus().schemas.at(row.action));
    for (auto const& [name, text] : row.state) {
        form->field(name).text().set(text);
    }
    harness.settle();
    EngineVerdict verdict{};
    verdict.ready = form->ready();
    for (auto const& [name, ignored] : row.visible) {
        verdict.visible.emplace(name, form->field(name).visible().get());
    }
    for (auto const& [name, ignored] : row.readonly) {
        verdict.readonly.emplace(name, form->field(name).readonly().get());
    }
    return verdict;
}

}  // namespace

TEST_CASE("rule corpus: the engine reads every corpus schema", "[forms-engine][rules][corpus]") {
    for (auto const& [name, schema] : engineCorpus().schemas) {
        INFO(name);
        CHECK(morph::forms::FormModel::fromSchema(name, schema).has_value());
    }
}

TEST_CASE("rule corpus: every row's engine verdicts are the ones the corpus records", "[forms-engine][rules][corpus]") {
    std::size_t presentation = 0;
    for (auto const& row : engineCorpus().rows) {
        EngineVerdict const verdict = runRow(row);
        INFO("row " << row.id);
        CHECK(verdict.ready == row.ready);
        CHECK(verdict.visible == row.visible);
        CHECK(verdict.readonly == row.readonly);
        presentation += row.visible.size() + row.readonly.size();
    }
    CHECK(presentation == 4);  // the corpus's visibleWhen and readonlyWhen rows were all asserted
}

TEST_CASE("rule corpus: the engine and allRulesSatisfied agree on every row", "[forms-engine][rules][corpus]") {
    for (auto const& row : engineCorpus().rows) {
        auto const* const typed = morph::test::rulecorpus::findAction(row.action);
        REQUIRE(typed != nullptr);
        INFO("row " << row.id);
        CHECK(runRow(row).ready == typed->satisfied(row.state));
    }
}
```

Append to `tests/test_forms_rule_agreement.cpp` (add `#include "forms_engine_support.hpp"`):

```cpp
TEST_CASE("the forms engine reaches the compiled verdicts on the emitted schema", "[forms][rules][agreement][forms-engine]") {
    using morph::test::formsengine::Harness;
    auto const& schema = morph::forms::schemaJson<RuleAgreementAction>();
    {
        Harness harness;
        auto const form = harness.session("Test_RuleAgreement", schema);
        harness.type(*form, "flag", "true");
        CHECK_FALSE(form->ready());
        harness.type(*form, "reason", "because");
        CHECK(form->ready());
    }
    {
        Harness harness;
        auto const form = harness.session("Test_RuleAgreement", schema);
        harness.type(*form, "flag", "false");
        CHECK(form->ready());
    }
    {
        Harness harness;
        auto const form = harness.session("Test_RuleAgreement", schema);
        harness.type(*form, "id", "9007199254740992");
        CHECK(form->ready());
        harness.type(*form, "id", "9007199254740993");
        CHECK_FALSE(form->ready());
        harness.type(*form, "reason", "because");
        CHECK(form->ready());
    }
}
```

Append to `tests/test_forms_instance_constraints.cpp` (add `#include "forms_engine_support.hpp"`):

```cpp
TEST_CASE("Every corpus row's engine verdict is the one the corpus records", "[forms][instance-constraints][forms-engine]") {
    using morph::test::formsengine::Harness;
    for (auto const& row : boundsCorpus().cases) {
        Harness harness;
        auto const form = harness.session("Test_ICCapture", boundsCorpus().schemas.at(row.schema));
        harness.type(*form, "analysisVersionId", "7");  // every row's identity field
        harness.type(*form, "value", row.value);
        INFO("row " << row.id);
        CHECK(form->ready() == row.ready);
    }
}

TEST_CASE("The decorated schema drives the engine's entry precision", "[forms][instance-constraints][forms-engine]") {
    using morph::test::formsengine::modelOf;
    CHECK(modelOf("ICCapture", boundsCorpus().schemas.at("decorated")).find("value")->decimalPlaces == 1U);
    CHECK(modelOf("ICCapture", boundsCorpus().schemas.at("compiled")).find("value")->decimalPlaces == 3U);
}
```

Append to `tests/test_forms_conformance_corpus.cpp` (add `#include "forms_engine_support.hpp"`):

```cpp
// The same fixtures, through the forms engine: the renderer contract the QML
// renderer's tst_conformance.qml mirrored by hand, now read from the emitted
// schemas themselves.

TEST_CASE("Conformance corpus through the engine: x-order and the required gate", "[conformance][forms-engine]") {
    morph::test::formsengine::Harness harness;
    auto const form = harness.session("CFScalarsAndRequired", morph::forms::schemaJson<CFScalarsAndRequired>());
    REQUIRE(form->fields().size() == 3);
    CHECK(form->fields()[0]->spec().name == "count");
    CHECK(form->fields()[1]->spec().name == "label");
    CHECK(form->fields()[2]->spec().name == "note");
    CHECK_FALSE(form->ready());
    harness.type(*form, "count", "3");
    CHECK_FALSE(form->ready());
    harness.type(*form, "label", "widget-7");
    CHECK(form->body() == R"({"count":3,"label":"widget-7"})");
}

TEST_CASE("Conformance corpus through the engine: exact Quantity payloads and unit switching", "[conformance][forms-engine]") {
    morph::test::formsengine::Harness harness;
    auto const form = harness.session("CFQuantityAlternatives", morph::forms::schemaJson<CFQuantityAlternatives>());
    harness.type(*form, "mass", "2650.5");
    CHECK(form->body() == R"({"mass":{"num":2650500,"den":1000,"dp":3}})");
    form->field("mass").switchUnit(1);
    harness.settle();
    CHECK(form->field("mass").text().peek() == "2650500.0");
    CHECK(form->body() == R"({"mass":{"num":26505000,"den":10000,"dp":3}})");
    harness.type(*form, "mass", "2650500.01");
    CHECK_FALSE(form->ready());  // more places than grams take
}

TEST_CASE("Conformance corpus through the engine: Choice, Timestamp and the shared $def", "[conformance][forms-engine]") {
    morph::test::formsengine::Harness harness;
    auto const choice = harness.session("CFChoiceField", morph::forms::schemaJson<CFChoiceField>());
    auto const& spec = choice->field("widgetId").spec();
    REQUIRE(spec.choice.has_value());
    CHECK(spec.choice->optionsAction == "CFListWidgets");
    CHECK(spec.choice->valueField == "id");
    CHECK(spec.choice->labelField == "name");
    CHECK(harness.server.fetches().size() == 1);
    CHECK(harness.server.fetches()[0].body == "{}");

    auto const stamp = harness.session("CFTimestampField", morph::forms::schemaJson<CFTimestampField>());
    CHECK(stamp->field("when").spec().kind == morph::forms::FieldKind::DateTime);
    CHECK_FALSE(stamp->ready());
    harness.type(*stamp, "when", "2026-07-20T09:00");
    CHECK(stamp->body() == R"({"when":"2026-07-20T09:00:00Z"})");

    auto const shared = harness.session("CFSharedDefFields", morph::forms::schemaJson<CFSharedDefFields>());
    CHECK(shared->field("massA").spec().unit == "kg");
    CHECK(shared->field("massB").spec().unit == "kg");
    CHECK(shared->field("massA").spec().decimalPlaces == 3U);
    CHECK(shared->field("massB").spec().decimalPlaces == 3U);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake -S . -B build/reactive && cmake --build build/reactive --target morph_tests &&
./build/reactive/tests/morph_tests "[forms-engine],[corpus],[instance-constraints],[conformance],[agreement]"`
Expected: every pre-existing case passes against the moved files; among the new cases, the first failure is the one
the engine gets wrong — if all pass at once, that is the expected outcome for a correct Tasks 3–8, and Step 4's
mutation check is the evidence these cases measure something.

- [ ] **Step 3: Implement**

Nothing in the library: the cases exercise Tasks 3–8. Fix any engine defect the corpus rows expose in the header
it belongs to, and say which row exposed it in the commit body.

- [ ] **Step 4: Run the tests to verify they pass**

Run the Step 2 command. Expected: PASS.

Mutation check: in `rules.hpp`'s `RuleExpr::membership`, change the `AtLeastOneOf` arm's `if (definite >= 1)` to
`if (definite >= 2)`. Expected: FAIL in "rule corpus: every row's engine verdicts…" for `atLeastOneOf/one`, and in
"the engine and allRulesSatisfied agree on every row" for the same row. Restore it.

Also confirm the QML suite still reads the moved files (on a `build/all` with `MORPH_BUILD_FORMS_QML=ON`):
`ctest --test-dir build/all -R forms_qml_logic --output-on-failure` — expected PASS, and its log names no
`unreadable or empty` failure.

- [ ] **Step 5: Commit**

```bash
git add tests/data tests/CMakeLists.txt src/qt/forms/CMakeLists.txt tests/test_forms_rule_corpus.cpp \
        tests/forms_rule_corpus.hpp tests/test_forms_instance_constraints.cpp src/qt/forms/tests/tst_main.cpp \
        src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml src/qt/forms/tests/tst_DynamicFormInstanceBounds.qml \
        tests/test_forms_engine_rules.cpp tests/test_forms_rule_agreement.cpp tests/test_forms_conformance_corpus.cpp
git commit -m "wip(forms): the rule, instance-bounds and conformance corpora through the engine

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: `bridgeSubmitter` and `bridgeChoiceFetcher`

The spec has these execute "with `Bridge::execute` (the bridge's JSON entry point)". `Bridge` has no JSON entry
point: the type-erased path is `BridgeHandler<Model, Sharing>::executeJson`
(`include/morph/core/bridge.hpp:2312`), and `ActionExecuteRegistry` keys its executors by
`(modelId, actionId, sharing)` with no way to ask which model serves an action. So the registry learns two things
at registration — the models serving each action id, and how to make a handler of each model — and the router
behind both factories keeps one `BridgeHandler<Model, NoSharing>` per model it has routed to.

**Files:**
- Modify: `include/morph/core/bridge.hpp` — `class Bridge;` forward declaration above `ActionExecuteRegistry`;
  `#include <map>`; `ActionExecuteRegistry::HandlerFactory`, `modelsServing`, `makeHandler`, two private maps; and
  the end of the out-of-line `ActionExecuteRegistry::registerAction` (after the two `_executors[…]` assignments)
- Modify: `docs/spec/core/bridge.md` — the `ActionExecuteRegistry` section and its API table
- Create: `include/morph/forms/engine/bridge_submitter.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/form_session.hpp`, add
  `include/morph/forms/engine/bridge_submitter.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_bridge.cpp` after `test_forms_engine_choice.cpp`
- Test: `tests/test_forms_engine_bridge.cpp`

**Interfaces:**
- Consumes: `ActionExecuteRegistry::instance()`, `execute<Sharing>(modelId, actionId, void*, bodyJson)`,
  `BridgeHandler<Model, NoSharing>(Bridge&, IExecutor*)` (`include/morph/core/bridge.hpp:61`, `:100`, `:2137`);
  `async::Completion<T>::makeSettleable`; Task 6's `Submitter`, `ChoiceFetcher`, `FormSession`.
- Produces: the contract's `forms::bridgeSubmitter(bridge::Bridge&, exec::IExecutor&) -> Submitter` and
  `forms::bridgeChoiceFetcher(bridge::Bridge&, exec::IExecutor&) -> ChoiceFetcher`; in `morph::bridge`, the
  additions `ActionExecuteRegistry::HandlerFactory`, `modelsServing(std::string_view actionId) const ->
  std::vector<std::string>` and `makeHandler(std::string_view modelId, Bridge&, exec::IExecutor* guiExec) const ->
  std::shared_ptr<void>`; `forms::detail::engine::BridgeRouter`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_bridge.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <exception>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/engine/bridge_submitter.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <optional>
#include <string>
#include <vector>

#include "forms_engine_support.hpp"
#include "test_support.hpp"

// File-scope, not anonymous-namespaced: glaze's reflection needs linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
struct FeSum {
    int a = 0;
    int b = 0;
};
struct FeSumResult {
    int sum = 0;
};
struct FeColour {
    std::int64_t id = 0;
    std::string name{};
};
struct FeListColours {};
struct FeColours {
    std::vector<FeColour> rows{};
};
// Two action types registered under one id by two models: the ambiguity the router refuses.
struct FeTwinA {};
struct FeTwinB {};
struct FeTwinResult {
    int from = 0;
};
struct FeMathModel {
    FeSumResult execute(FeSum const& action) { return FeSumResult{.sum = action.a + action.b}; }
    FeColours execute(FeListColours const&) {
        return FeColours{.rows = {FeColour{.id = 9007199254740993, .name = "Ochre"}, FeColour{.id = 2, .name = "Teal"}}};
    }
    FeTwinResult execute(FeTwinA const&) { return FeTwinResult{.from = 1}; }
};
struct FeOtherModel {
    FeTwinResult execute(FeTwinB const&) { return FeTwinResult{.from = 2}; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(FeMathModel, "Test_FormsEngine_Math")
BRIDGE_REGISTER_ACTION(FeMathModel, FeSum, "Test_FormsEngine_Sum")
BRIDGE_REGISTER_ACTION(FeMathModel, FeListColours, "Test_FormsEngine_ListColours")
BRIDGE_REGISTER_ACTION(FeMathModel, FeTwinA, "Test_FormsEngine_Twin")
BRIDGE_REGISTER_MODEL(FeOtherModel, "Test_FormsEngine_Other")
BRIDGE_REGISTER_ACTION(FeOtherModel, FeTwinB, "Test_FormsEngine_Twin")

namespace {

/// A LocalBackend bridge whose owner drives a reactive runtime.
struct LocalStack {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner{};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::reactive::Runtime rt{owner};
};

[[nodiscard]] std::optional<std::string> await(LocalStack& stack, morph::async::Completion<std::string> completion,
                                               std::exception_ptr* error = nullptr) {
    std::optional<std::string> reply;
    bool done = false;
    completion
        .then([&](std::string const& json) {
            reply = json;
            done = true;
        })
        .onError([&](std::exception_ptr failure) {
            if (error != nullptr) {
                *error = std::move(failure);
            }
            done = true;
        });
    REQUIRE(morph::testing::pumpOwnerUntil(stack.owner, [&] { return done; }));
    return reply;
}

}  // namespace

TEST_CASE("bridgeSubmitter routes an action type to the model that registers it", "[forms-engine][bridge]") {
    LocalStack stack;
    auto const submit = morph::forms::bridgeSubmitter(stack.bridge, stack.owner);
    CHECK(await(stack, submit("Test_FormsEngine_Sum", R"({"a":3,"b":4})")) == R"({"sum":7})");
    CHECK(await(stack, submit("Test_FormsEngine_Sum", R"({"a":1,"b":1})")) == R"({"sum":2})");
}

TEST_CASE("bridgeSubmitter fails an action no model serves, and one two models serve", "[forms-engine][bridge]") {
    LocalStack stack;
    auto const submit = morph::forms::bridgeSubmitter(stack.bridge, stack.owner);
    std::exception_ptr unknown;
    CHECK_FALSE(await(stack, submit("Test_FormsEngine_Nope", "{}"), &unknown).has_value());
    CHECK(morph::reactive::errorMessage(unknown) == "no registered model serves action 'Test_FormsEngine_Nope'");
    std::exception_ptr ambiguous;
    CHECK_FALSE(await(stack, submit("Test_FormsEngine_Twin", "{}"), &ambiguous).has_value());
    CHECK(morph::reactive::errorMessage(ambiguous) ==
          "action 'Test_FormsEngine_Twin' is served by more than one model: Test_FormsEngine_Math, Test_FormsEngine_Other");
    auto const& registry = morph::bridge::ActionExecuteRegistry::instance();
    CHECK(registry.modelsServing("Test_FormsEngine_Sum") == std::vector<std::string>{"Test_FormsEngine_Math"});
    CHECK(registry.modelsServing("Test_FormsEngine_Nope").empty());
}

TEST_CASE("bridgeChoiceFetcher returns the options action's reply, ids exact", "[forms-engine][bridge]") {
    LocalStack stack;
    auto const fetch = morph::forms::bridgeChoiceFetcher(stack.bridge, stack.owner);
    CHECK(await(stack, fetch("Test_FormsEngine_ListColours", "{}")) ==
          R"({"rows":[{"id":9007199254740993,"name":"Ochre"},{"id":2,"name":"Teal"}]})");
}

TEST_CASE("a FormSession over a real bridge fetches its options and submits its body", "[forms-engine][bridge]") {
    LocalStack stack;
    morph::forms::FormSession form{
        stack.rt,
        morph::test::formsengine::modelOf(
            "Test_FormsEngine_Sum",
            R"({"properties":{"a":{"type":"integer","x-order":0},"b":{"type":"integer","x-order":1},
              "colour":{"type":["integer","null"],"x-order":2,"x-optionsAction":"Test_FormsEngine_ListColours"}},"required":["a","b"]})"),
        morph::forms::bridgeSubmitter(stack.bridge, stack.owner), morph::forms::bridgeChoiceFetcher(stack.bridge, stack.owner)};
    REQUIRE(morph::testing::pumpOwnerUntil(stack.owner, [&] { return form.field("colour").options().size() == 2; }));
    CHECK(form.field("colour").options().front().valueJson == "9007199254740993");
    form.field("a").text().set("3");
    form.field("b").text().set("4");
    REQUIRE(morph::testing::pumpOwnerUntil(stack.owner, [&] { return form.lastReply().has_value(); }));
    CHECK(form.lastReply() == R"({"sum":7})");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/bridge_submitter.hpp' file not found`.

- [ ] **Step 3: Implement**

In `include/morph/core/bridge.hpp`, add `#include <map>` to the standard includes, and directly above
`class ActionExecuteRegistry {` add:

```cpp
class Bridge;
```

In `ActionExecuteRegistry`'s public section, after `contains`, add:

```cpp
    /// @brief Makes a handler of one registered model on a bridge, type-erased so that
    ///        `execute<NoSharing>` accepts it.
    using HandlerFactory = std::function<std::shared_ptr<void>(Bridge&, ::morph::exec::IExecutor*)>;

    /// @brief The model ids that register an executor for @p actionId, in registration order.
    ///
    /// The routing question a schema-driven client asks: it knows an action type from a schema
    /// and needs the model whose handler executes it. Two models may register one id; the caller
    /// decides what that means.
    /// @param actionId The action type id.
    /// @return The model ids; empty when no model registers @p actionId.
    [[nodiscard]] std::vector<std::string> modelsServing(std::string_view actionId) const {
        ::morph::model::detail::noteRegistryRead(this == &instance());
        auto const found = _modelsByAction.find(actionId);
        return found == _modelsByAction.end() ? std::vector<std::string>{} : found->second;
    }

    /// @brief Makes a `BridgeHandler<Model, NoSharing>` of the model registered as @p modelId.
    /// @param modelId The model id.
    /// @param bridge  The bridge the handler registers on. Borrowed: it must outlive the handler.
    /// @param guiExec The executor the handler delivers completions on (see `BridgeHandler`).
    /// @return The handler, owning; pass `.get()` to `execute<NoSharing>`.
    /// @throws std::runtime_error when no model is registered as @p modelId.
    [[nodiscard]] std::shared_ptr<void> makeHandler(std::string_view modelId, Bridge& bridge,
                                                    ::morph::exec::IExecutor* guiExec) const {
        ::morph::model::detail::noteRegistryRead(this == &instance());
        auto const found = _handlerFactories.find(modelId);
        if (found == _handlerFactories.end()) {
            throw std::runtime_error("unknown model for makeHandler: " + std::string{modelId});
        }
        return found->second(bridge, guiExec);
    }
```

and in its private section, after `_executors`:

```cpp
    // Action id -> the model ids registering it, in registration order.
    std::map<std::string, std::vector<std::string>, std::less<>> _modelsByAction;
    // Model id -> how to make a NoSharing handler of it.
    std::map<std::string, HandlerFactory, std::less<>> _handlerFactories;
```

At the end of the out-of-line `ActionExecuteRegistry::registerAction` (after the `AllowShared` executor is filed),
add:

```cpp
    auto& models = _modelsByAction.try_emplace(std::string{actionId}).first->second;
    if (std::ranges::find(models, modelId) == models.end()) {
        models.emplace_back(modelId);
    }
    _handlerFactories.try_emplace(std::string{modelId}, [](Bridge& bridge, ::morph::exec::IExecutor* guiExec) {
        return std::shared_ptr<void>{std::make_shared<BridgeHandler<Model, NoSharing>>(bridge, guiExec)};
    });
```

Create `include/morph/forms/engine/bridge_submitter.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/bridge_submitter.hpp
/// @brief `bridgeSubmitter` and `bridgeChoiceFetcher`: the forms engine's production
///        `Submitter` and `ChoiceFetcher`, routing an action type to the model that registers it.
///
/// Specified in `docs/spec/forms/engine.md`, "Submitters".

#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "../../attributes.hpp"
#include "../../core/bridge.hpp"
#include "../../core/completion.hpp"
#include "../../core/executor.hpp"
#include "form_session.hpp"

namespace morph::forms {

namespace detail::engine {

/// @brief Executes JSON bodies by action type: finds the model registering the type and keeps one
///        handler per model it has routed to.
class BridgeRouter {
public:
    /// @param bridge    The bridge. Borrowed: it must outlive the router.
    /// @param callbacks The executor completions deliver on — the reactive runtime's owner. Borrowed.
    BridgeRouter(bridge::Bridge& bridge MORPH_LIFETIMEBOUND, exec::IExecutor& callbacks MORPH_LIFETIMEBOUND)
        : _bridge{&bridge}, _callbacks{&callbacks} {}

    /// @brief Executes @p bodyJson as @p actionType.
    /// @param actionType The action type id.
    /// @param bodyJson   The JSON body.
    /// @return The reply; a failed completion when no model, or more than one, registers the type.
    [[nodiscard]] async::Completion<std::string> execute(std::string_view actionType, std::string const& bodyJson) {
        auto& registry = bridge::ActionExecuteRegistry::instance();
        std::vector<std::string> const models = registry.modelsServing(actionType);
        if (models.empty()) {
            return failed("no registered model serves action '" + std::string{actionType} + "'");
        }
        if (models.size() > 1) {
            std::string names;
            for (std::string const& model : models) {
                names += (names.empty() ? "" : ", ") + model;
            }
            return failed("action '" + std::string{actionType} + "' is served by more than one model: " + names);
        }
        std::shared_ptr<void>& handler = _handlers.try_emplace(models.front()).first->second;
        if (!handler) {
            handler = registry.makeHandler(models.front(), *_bridge, _callbacks);
        }
        return registry.execute<bridge::NoSharing>(models.front(), actionType, handler.get(), bodyJson);
    }

private:
    [[nodiscard]] async::Completion<std::string> failed(std::string const& message) const {
        auto [completion, promise] = async::Completion<std::string>::makeSettleable(_callbacks);
        promise.reject(std::make_exception_ptr(std::runtime_error{message}));
        return std::move(completion);
    }

    bridge::Bridge* _bridge;
    exec::IExecutor* _callbacks;
    std::map<std::string, std::shared_ptr<void>, std::less<>> _handlers;
};

}  // namespace detail::engine

/// @brief The production `Submitter`: executes a form's action through @p bridge.
///
/// The action type selects the model registering it (`ActionExecuteRegistry::modelsServing`);
/// the body goes through that model's handler, so validation, journaling and backends behave as
/// for any typed call. Handlers are made on first use and kept while any copy of the returned
/// function lives; destroy those copies (the sessions holding them) on the owner, before the bridge.
/// @param bridge    The bridge. Borrowed: it must outlive every copy of the result.
/// @param callbacks The executor replies deliver on — the reactive runtime's owner. Borrowed.
/// @return The submitter.
[[nodiscard]] inline Submitter bridgeSubmitter(bridge::Bridge& bridge, exec::IExecutor& callbacks) {
    auto router = std::make_shared<detail::engine::BridgeRouter>(bridge, callbacks);
    return [router](std::string_view actionType, std::string bodyJson) { return router->execute(actionType, bodyJson); };
}

/// @brief The production `ChoiceFetcher`: executes an options action through @p bridge, routed
///        like `bridgeSubmitter`.
/// @param bridge    The bridge. Borrowed: it must outlive every copy of the result.
/// @param callbacks The executor replies deliver on — the reactive runtime's owner. Borrowed.
/// @return The fetcher.
[[nodiscard]] inline ChoiceFetcher bridgeChoiceFetcher(bridge::Bridge& bridge, exec::IExecutor& callbacks) {
    auto router = std::make_shared<detail::engine::BridgeRouter>(bridge, callbacks);
    return [router](std::string_view optionsAction, std::string bodyJson) {
        return router->execute(optionsAction, bodyJson);
    };
}

}  // namespace morph::forms
```

In `docs/spec/core/bridge.md`, at the end of the `## ActionExecuteRegistry` section (before `### Why the key carries
the sharing policy`), add:

```markdown
**Routing by action id.** `registerAction` also records, per action id, the model ids that register it
(`modelsServing`), and per model id a factory for a `BridgeHandler<Model, NoSharing>` (`makeHandler`). Together
they answer what a schema-driven client asks — it has an action type from a schema and needs the handler that
executes it — without the client naming a model type. `morph::forms::bridgeSubmitter` is that client: it refuses
an id no model serves, and one two models serve, rather than guessing.
```

and to the `### ActionExecuteRegistry` API table add:

```markdown
| `modelsServing` | `std::vector<std::string> modelsServing(string_view actionId) const` | Model ids registering `actionId`, in registration order; empty when none. |
| `makeHandler` | `std::shared_ptr<void> makeHandler(string_view modelId, Bridge&, exec::IExecutor* guiExec) const` | A `BridgeHandler<Model, NoSharing>` of the model registered as `modelId`, type-erased for `execute<NoSharing>`. Throws `runtime_error` for an unknown id. |
```

Register the header and the test as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][bridge],[execute-json]"`
Expected: PASS — 4 new cases and the existing `[execute-json]` cases.

Mutation check: in `BridgeRouter::execute`, delete the `if (models.size() > 1) { … }` block. Expected: FAIL in
"bridgeSubmitter fails an action no model serves, and one two models serve" (the Twin call is routed to the first
model and succeeds). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/core/bridge.hpp docs/spec/core/bridge.md include/morph/forms/engine/bridge_submitter.hpp \
        tests/test_forms_engine_bridge.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): bridgeSubmitter and bridgeChoiceFetcher, routed through the action registry

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10b: `handlerSubmitter` and `handlerChoiceFetcher` — forms on the app's own handlers

`bridgeSubmitter` keeps a handler of its own per model, which is right for models whose state lives in a database.
But under `LocalBackend` every `BridgeHandler` of a non-shared model is its **own model instance**, so a form whose
submissions must land in the instance the rest of the screen reads (the forms demo's in-memory `LabModel`), or on a
shared instance another handler attached (polls' `AllowShared` `PollModel`), must submit through **that** handler.
These two factories route each action type to the first of the given handlers that serves it, through
`BridgeHandler::executeJson` (`include/morph/core/bridge.hpp:2312`) and `servesAction` (`:2330`).

**Files:**
- Create: `include/morph/forms/engine/handler_submitter.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/bridge_submitter.hpp`, add
  `include/morph/forms/engine/handler_submitter.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_handler_submitter.cpp` after `test_forms_engine_bridge.cpp`
- Test: `tests/test_forms_engine_handler_submitter.cpp`

**Interfaces:**
- Consumes: `BridgeHandler<M, S>::executeJson(std::string_view, std::string_view)`, `servesAction(std::string_view)`;
  `async::Completion<T>::makeSettleable`; Task 6's `Submitter`, `ChoiceFetcher`.
- Produces (added to the interface contract's Part 5):
  `template <class... Handlers> Submitter forms::handlerSubmitter(exec::IExecutor& callbacks, Handlers&... handlers)`
  and `template <class... Handlers> ChoiceFetcher forms::handlerChoiceFetcher(exec::IExecutor& callbacks,
  Handlers&... handlers)` — each `Handlers` a `bridge::BridgeHandler<M, S>`; the handlers must outlive the returned
  function; an action no handler serves resolves the completion with `std::invalid_argument`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_handler_submitter.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/engine/handler_submitter.hpp>
#include <optional>
#include <stdexcept>
#include <string>

// File-scope, not anonymous-namespaced: glaze's reflection needs linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
struct HsBump {
    int by = 0;
};
struct HsRead {};
struct HsCount {
    int value = 0;
};
struct HsEcho {
    std::string text;
};

// In-memory state: each BridgeHandler<HsCounterModel> is its own instance under LocalBackend.
struct HsCounterModel {
    int count = 0;
    HsCount execute(HsBump action) {
        count += action.by;
        return HsCount{count};
    }
    HsCount execute(HsRead) const { return HsCount{count}; }
};

struct HsEchoModel {
    HsEcho execute(HsEcho action) { return action; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(HsCounterModel, "Test_HsCounterModel")
BRIDGE_REGISTER_ACTION(HsCounterModel, HsBump, "Test_HsBump")
BRIDGE_REGISTER_ACTION(HsCounterModel, HsRead, "Test_HsRead")
BRIDGE_REGISTER_MODEL(HsEchoModel, "Test_HsEchoModel")
BRIDGE_REGISTER_ACTION(HsEchoModel, HsEcho, "Test_HsEcho")

namespace {

struct Wiring {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<HsCounterModel> counter{bridge, &owner};
    morph::bridge::BridgeHandler<HsEchoModel> echo{bridge, &owner};
};

struct Outcome {
    std::optional<std::string> reply;
    std::exception_ptr error;
};

Outcome settle(morph::exec::MainThreadExecutor& owner, morph::async::Completion<std::string> completion) {
    auto outcome = std::make_shared<Outcome>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    completion
        .then([outcome, done](std::string const& reply) {
            outcome->reply = reply;
            done->store(true);
        })
        .onError([outcome, done](std::exception_ptr const& error) {
            outcome->error = error;
            done->store(true);
        });
    for (int step = 0; step < 400 && !done->load(); ++step) {
        owner.runFor(std::chrono::milliseconds{5});
    }
    REQUIRE(done->load());
    return *outcome;
}

}  // namespace

TEST_CASE("forms::handlerSubmitter: submissions land in the given handler's instance", "[forms-engine][handler]") {
    Wiring wiring;
    auto const submit = morph::forms::handlerSubmitter(wiring.owner, wiring.counter, wiring.echo);
    CHECK(settle(wiring.owner, submit("Test_HsBump", R"({"by":2})")).reply == std::optional<std::string>{R"({"value":2})"});
    CHECK(settle(wiring.owner, submit("Test_HsBump", R"({"by":3})")).reply == std::optional<std::string>{R"({"value":5})"});
    // The typed path on the same handler reads the same instance.
    CHECK(settle(wiring.owner, wiring.counter.executeJson("Test_HsRead", "{}")).reply ==
          std::optional<std::string>{R"({"value":5})"});
}

TEST_CASE("forms::handlerSubmitter: each action routes to the handler that serves it", "[forms-engine][handler]") {
    Wiring wiring;
    auto const submit = morph::forms::handlerSubmitter(wiring.owner, wiring.counter, wiring.echo);
    CHECK(settle(wiring.owner, submit("Test_HsEcho", R"({"text":"hi"})")).reply ==
          std::optional<std::string>{R"({"text":"hi"})"});
}

TEST_CASE("forms::handlerSubmitter: an action no handler serves is a failed completion", "[forms-engine][handler]") {
    Wiring wiring;
    auto const submit = morph::forms::handlerSubmitter(wiring.owner, wiring.echo);
    auto const outcome = settle(wiring.owner, submit("Test_HsBump", R"({"by":1})"));
    REQUIRE(outcome.error != nullptr);
    CHECK_THROWS_AS(std::rethrow_exception(outcome.error), std::invalid_argument);
}

TEST_CASE("forms::handlerChoiceFetcher: fetches options through the given handler", "[forms-engine][handler]") {
    Wiring wiring;
    auto const fetch = morph::forms::handlerChoiceFetcher(wiring.owner, wiring.counter);
    CHECK(settle(wiring.owner, fetch("Test_HsRead", "{}")).reply == std::optional<std::string>{R"({"value":0})"});
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: FAIL — `'morph/forms/engine/handler_submitter.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/handler_submitter.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../core/bridge.hpp"
#include "../../core/completion.hpp"
#include "../../core/executor.hpp"
#include "form_session.hpp"

/// @file
/// @brief Submitters and choice fetchers bound to an application's own `BridgeHandler`s.
///
/// Specified in `docs/spec/forms/engine.md`, "Submitters".

namespace morph::forms {

namespace detail::engine {

/// @brief Executes an action through one handler, or declines when the handler does not serve it.
using JsonRoute =
    std::function<std::optional<async::Completion<std::string>>(std::string_view actionType, std::string const& body)>;

/// @brief The route through @p handler.
/// @tparam Model The handler's model.
/// @tparam Sharing The handler's sharing policy.
/// @param handler The handler; it must outlive the route.
/// @return A route that serves exactly the actions @p handler's model registers.
template <class Model, class Sharing>
[[nodiscard]] JsonRoute routeThrough(bridge::BridgeHandler<Model, Sharing>& handler) {
    return [&handler](std::string_view actionType,
                      std::string const& body) -> std::optional<async::Completion<std::string>> {
        if (!handler.servesAction(actionType)) {
            return std::nullopt;
        }
        return handler.executeJson(actionType, body);
    };
}

/// @brief Runs @p body through the first route that serves @p actionType.
/// @param routes The routes, in priority order.
/// @param callbacks Where a refusal is delivered.
/// @param actionType The registered action id.
/// @param body The JSON body.
/// @return The route's completion, or one failed with `std::invalid_argument` when no route serves the action.
[[nodiscard]] inline async::Completion<std::string> routeFirst(std::vector<JsonRoute> const& routes,
                                                              exec::IExecutor& callbacks, std::string_view actionType,
                                                              std::string const& body) {
    for (JsonRoute const& route : routes) {
        if (std::optional<async::Completion<std::string>> completion = route(actionType, body)) {
            return std::move(*completion);
        }
    }
    auto [failed, promise] = async::Completion<std::string>::makeSettleable(&callbacks);
    promise.reject(std::make_exception_ptr(
        std::invalid_argument{"no handler serves the action \"" + std::string{actionType} + "\""}));
    return std::move(failed);
}

}  // namespace detail::engine

/// @brief A `Submitter` that sends each action through the first of @p handlers whose model registers it.
///
/// Use it when a form's submissions must reach the model instance the rest of the screen uses — an in-memory
/// model, or a shared instance another handler attached. `bridgeSubmitter` keeps handlers of its own instead.
/// @tparam Handlers `bridge::BridgeHandler<M, S>` types.
/// @param callbacks The executor a refusal is delivered on: the handlers' GUI executor.
/// @param handlers The handlers, in priority order; they must outlive the returned submitter.
/// @return The submitter.
template <class... Handlers>
    requires(sizeof...(Handlers) > 0)
[[nodiscard]] Submitter handlerSubmitter(exec::IExecutor& callbacks, Handlers&... handlers) {
    std::vector<detail::engine::JsonRoute> routes{detail::engine::routeThrough(handlers)...};
    return [routes = std::move(routes), &callbacks](std::string_view actionType, std::string bodyJson) {
        return detail::engine::routeFirst(routes, callbacks, actionType, bodyJson);
    };
}

/// @brief A `ChoiceFetcher` that runs each options action through the first of @p handlers that serves it.
/// @tparam Handlers `bridge::BridgeHandler<M, S>` types.
/// @param callbacks The executor a refusal is delivered on: the handlers' GUI executor.
/// @param handlers The handlers, in priority order; they must outlive the returned fetcher.
/// @return The fetcher.
template <class... Handlers>
    requires(sizeof...(Handlers) > 0)
[[nodiscard]] ChoiceFetcher handlerChoiceFetcher(exec::IExecutor& callbacks, Handlers&... handlers) {
    std::vector<detail::engine::JsonRoute> routes{detail::engine::routeThrough(handlers)...};
    return [routes = std::move(routes), &callbacks](std::string_view optionsAction, std::string bodyJson) {
        return detail::engine::routeFirst(routes, callbacks, optionsAction, bodyJson);
    };
}

}  // namespace morph::forms
```

Task 16 writes the "Submitting through the application's handlers" paragraph of `docs/spec/forms/engine.md`
("Submitters"): when to choose `handlerSubmitter` over `bridgeSubmitter`, the priority order, and the
`std::invalid_argument` refusal.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][handler]"`
Expected: PASS. Mutation check: in `handlerSubmitter`, build the routes from a fresh
`bridge::BridgeHandler<HsCounterModel>` instead of the given one (or, generally, drop `servesAction`'s check so the
first handler takes every action) — expected FAIL in "submissions land in the given handler's instance" (a second
instance reports `{"value":2}` then `{"value":3}`) or in "each action routes to the handler that serves it".
Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/handler_submitter.hpp tests/test_forms_engine_handler_submitter.cpp \
        tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): handlerSubmitter and handlerChoiceFetcher, bound to the app's own handlers

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: `Form<A>` — the typed facade

`Form<A>` builds its model with `FormModel::forAction<A>()` (Task 4) — `fromSchema` over `schemaJson<A>()`, the
reader a runtime form uses — so a typed and a runtime form of one action are the same model with the same rules. What it adds is typing at the edges: `set<>`
takes the member's own type, `value()` decodes the body into `A`, `ActionValidator<A>` (the action's `validate()`)
joins readiness, and a submission goes through `BridgeHandler::execute<A>` so `lastResult()` is the action's typed
result.

**Files:**
- Create: `include/morph/forms/engine/typed_form.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/handler_submitter.hpp`, add
  `include/morph/forms/engine/typed_form.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_typed.cpp` after `test_forms_engine_handler_submitter.cpp`
- Test: `tests/test_forms_engine_typed.cpp`

**Interfaces:**
- Consumes: Task 4's `FormModel::forAction<A>()`; `detail::memberWireName<MemberPtr>()`
  (`:629`); `bridge::detail::MemberPointerTraits` (`include/morph/core/bridge.hpp:258`); `model::ActionTraits<A>`
  (`typeId`, `fromJson`, `resultToJson`, `Result`), `model::ActionValidator<A>::ready`
  (`include/morph/core/registry.hpp:255`); `BridgeHandler<M, S>::execute<A>`; `async::CallbackScope`; Task 6's
  `FormSession`, `FormSessionOptions::accepts`, `FormSession::assign`.
- Produces: the contract's `forms::Form<A, M, S = bridge::NoSharing>` — `Form(reactive::Runtime&,
  bridge::BridgeHandler<M, S>&, ChoiceFetcher, FormSessionOptions = {})`, `session()`,
  `template <auto Member> void set(MemberValue<Member>)`, `value() -> std::optional<A>`, `ready()`,
  `lastResult() -> std::optional<Result> const&` — plus `pending()`, `error()` and the alias
  `Form::MemberValue<Member>`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_typed.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/engine/typed_form.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/runtime.hpp>
#include <optional>
#include <string>

#include "test_support.hpp"

// NOLINTBEGIN(misc-use-internal-linkage)
struct FtSum {
    int a = 0;
    int b = 0;
    std::optional<std::string> note{};

    // The action's own rule, which the schema cannot state: the form must not submit past it.
    [[nodiscard]] bool validate() const { return a < 100; }
};
struct FtSumResult {
    int sum = 0;
};
struct FtModel {
    FtSumResult execute(FtSum const& action) { return FtSumResult{.sum = action.a + action.b}; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(FtModel, "Test_FormsEngineTyped_Model")
BRIDGE_REGISTER_ACTION(FtModel, FtSum, "Test_FormsEngineTyped_Sum")

namespace {

struct TypedStack {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner{};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::reactive::Runtime rt{owner};
    morph::bridge::BridgeHandler<FtModel> handler{bridge, &owner};
};

}  // namespace

TEST_CASE("Form<A>: the typed model is the runtime model of the same schema", "[forms-engine][typed]") {
    TypedStack stack;
    morph::forms::Form<FtSum, FtModel> form{stack.rt, stack.handler, morph::forms::ChoiceFetcher{}};
    auto const runtime = morph::forms::FormModel::fromSchema("Test_FormsEngineTyped_Sum", morph::forms::schemaJson<FtSum>());
    REQUIRE(runtime.has_value());
    auto const& typed = form.session().model();
    CHECK(typed.actionType() == "Test_FormsEngineTyped_Sum");
    REQUIRE(typed.fields().size() == runtime->fields().size());
    for (std::size_t i = 0; i < typed.fields().size(); ++i) {
        CHECK(typed.fields()[i].name == runtime->fields()[i].name);
        CHECK(typed.fields()[i].kind == runtime->fields()[i].kind);
        CHECK(typed.fields()[i].required == runtime->fields()[i].required);
    }
}

TEST_CASE("Form<A>: set<> is a typed, programmatic change; the next edit submits through the handler",
          "[forms-engine][typed]") {
    TypedStack stack;
    morph::forms::Form<FtSum, FtModel> form{stack.rt, stack.handler, morph::forms::ChoiceFetcher{}};
    form.set<&FtSum::a>(3);
    form.set<&FtSum::b>(4);
    REQUIRE(form.ready());
    CHECK(form.session().body() == R"({"a":3,"b":4})");
    REQUIRE(form.value().has_value());
    CHECK(form.value()->a == 3);
    CHECK_FALSE(form.lastResult().has_value());  // set<> never submits

    form.session().field("b").text().set("5");
    REQUIRE(morph::testing::pumpOwnerUntil(stack.owner, [&] { return form.lastResult().has_value(); }));
    CHECK(form.lastResult()->sum == 8);
    CHECK(form.session().lastReply() == R"({"sum":8})");
    CHECK(form.error() == nullptr);
}

TEST_CASE("Form<A>: the action's validate() joins readiness", "[forms-engine][typed]") {
    TypedStack stack;
    morph::forms::Form<FtSum, FtModel> form{stack.rt, stack.handler, morph::forms::ChoiceFetcher{}};
    form.set<&FtSum::a>(150);
    form.set<&FtSum::b>(1);
    CHECK(form.session().field("a").error().get() == std::nullopt);  // every field encodes...
    CHECK_FALSE(form.ready());                                       // ...but the action refuses
    CHECK_FALSE(form.session().body().has_value());
    form.set<&FtSum::a>(99);
    CHECK(form.ready());
    form.set<&FtSum::note>(std::string{"why"});
    CHECK(form.session().body() == R"({"a":99,"b":1,"note":"why"})");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/typed_form.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/typed_form.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/typed_form.hpp
/// @brief `Form<A>`: a `FormSession` over `schemaJson<A>()`, with typed access at the edges.
///
/// Specified in `docs/spec/forms/engine.md`, "Typed forms".

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../../attributes.hpp"
#include "../../core/bridge.hpp"
#include "../../core/callback_scope.hpp"
#include "../../core/completion.hpp"
#include "../../core/registry.hpp"
#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../forms.hpp"
#include "field_model.hpp"
#include "form_session.hpp"

namespace morph::forms {

/// @brief A form over action `A`: a `FormSession` built from `schemaJson<A>()`, submitting
///        through a `BridgeHandler`, with typed `set<>`, `value()` and `lastResult()`.
///
/// Non-copyable and non-movable: its session and in-flight callbacks point into it.
/// @tparam A The action type (registered with `BRIDGE_REGISTER_ACTION`).
/// @tparam M The model type the handler talks to.
/// @tparam S The handler's sharing policy.
template <class A, class M, class S = bridge::NoSharing>
class Form {
public:
    /// @brief The action's registered result type.
    using Result = typename model::ActionTraits<A>::Result;

    /// @brief The type of the member @p Member points to.
    /// @tparam Member A pointer to a data member of `A`.
    template <auto Member>
    using MemberValue = typename bridge::detail::MemberPointerTraits<decltype(Member)>::ValueType;

    /// @param runtime The runtime; its owner must be @p handler's callback executor. Borrowed.
    /// @param handler Executes the action. Borrowed: it must outlive the form.
    /// @param choices Fetches Choice options; may be empty when `A` has no Choice member.
    /// @param options Locale, translations, display zone; an `accepts` gate is kept and joined
    ///                with `ActionValidator<A>`.
    /// @throws std::logic_error from `FormModel::forAction<A>()` when `schemaJson<A>()` does not
    ///         read as a form model.
    Form(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND,
         ChoiceFetcher choices, FormSessionOptions options = {})
        : _rt{&runtime},
          _handler{&handler},
          _lastResult{runtime, std::nullopt},
          _session{runtime, FormModel::forAction<A>(), [this](std::string_view, std::string body) { return execute(std::move(body)); },
                   std::move(choices), withValidator(std::move(options))},
          _value{runtime, [this]() -> std::optional<A> {
                     std::optional<std::string> const body = _session.body();
                     return body.has_value() ? decode(*body) : std::nullopt;
                 }} {}

    ~Form() = default;
    Form(Form const&) = delete;
    Form& operator=(Form const&) = delete;
    Form(Form&&) = delete;
    Form& operator=(Form&&) = delete;

    /// @brief The session: fields, readiness, body, submission.
    /// @return The session.
    [[nodiscard]] FormSession& session() noexcept { return _session; }

    /// @brief Sets one member, encoded with its field's encoder; a programmatic change, so
    ///        nothing is submitted.
    /// @tparam Member A pointer to a data member of `A`, e.g. `&Deposit::amount`.
    /// @param value The member's value.
    template <auto Member>
    void set(MemberValue<Member> value) {
        static_assert(std::is_same_v<typename bridge::detail::MemberPointerTraits<decltype(Member)>::ClassType, A>,
                      "Form<A>::set<>: the member must belong to A");
        _session.assign(detail::memberWireName<Member>(), glz::write_json(value).value_or(std::string{"null"}));
    }

    /// @brief The body decoded into `A`. Tracked.
    /// @return The action while the form is ready, else `std::nullopt`.
    [[nodiscard]] std::optional<A> value() const { return _value.get(); }

    /// @brief Whether the form can submit: the session is ready, which includes `A`'s own
    ///        `validate()`. Tracked.
    /// @return `true` when ready.
    [[nodiscard]] bool ready() const { return _session.ready(); }

    /// @brief The last successful result. Tracked.
    /// @return The typed result, or `std::nullopt` before the first success.
    [[nodiscard]] std::optional<Result> const& lastResult() const { return _lastResult.get(); }

    /// @brief Whether a submission is in flight. Tracked.
    /// @return `true` while one is.
    [[nodiscard]] bool pending() const { return _session.pending(); }

    /// @brief The last failure. Tracked.
    /// @return The exception, or null.
    [[nodiscard]] std::exception_ptr error() const { return _session.lastError(); }

private:
    [[nodiscard]] static std::optional<A> decode(std::string const& body) {
        try {
            return model::ActionTraits<A>::fromJson(body);
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] static FormSessionOptions withValidator(FormSessionOptions options) {
        options.accepts = [own = std::move(options.accepts)](std::string_view body) {
            if (own && !own(body)) {
                return false;
            }
            std::optional<A> const action = decode(std::string{body});
            return action.has_value() && model::ActionValidator<A>::ready(*action);
        };
        return options;
    }

    [[nodiscard]] async::Completion<std::string> execute(std::string const& body) {
        auto [completion, promise] = async::Completion<std::string>::makeSettleable(&_rt->owner());
        auto relay = std::make_shared<async::Completion<std::string>::Promise>(std::move(promise));
        std::optional<A> action = decode(body);
        if (!action) {
            relay->reject(std::make_exception_ptr(std::invalid_argument{"Form<A>: the body does not decode"}));
            return std::move(completion);
        }
        _handler->execute(*std::move(action))
            .then(_relay,
                  [this, relay](Result const& result) {
                      try {
                          std::string reply = model::ActionTraits<A>::resultToJson(result);
                          _lastResult.set(result);
                          relay->resolve(std::move(reply));
                      } catch (...) {
                          relay->reject(std::current_exception());
                      }
                  })
            .onError(_relay, [relay](std::exception_ptr error) { relay->reject(std::move(error)); });
        return std::move(completion);
    }

    reactive::Runtime* _rt;
    bridge::BridgeHandler<M, S>* _handler;
    reactive::Signal<std::optional<Result>> _lastResult;
    // Before the session: a session ready as constructed submits from its constructor, and the
    // handler callbacks that submission attaches are gated by this scope.
    async::CallbackScope _relay;
    FormSession _session;
    reactive::Computed<std::optional<A>> _value;
};

}  // namespace morph::forms
```

Register the header and the test as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][typed]"`
Expected: PASS, 3 test cases.

Mutation check: in `withValidator`, replace `return action.has_value() && model::ActionValidator<A>::ready(*action);`
with `return action.has_value();`. Expected: FAIL in "the action's validate() joins readiness". Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/typed_form.hpp tests/test_forms_engine_typed.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): Form<A>, the typed facade over FormSession

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 12: `formView`, `Overrides` and `FieldView` — the form rendered through the view tree

**Files:**
- Create: `include/morph/forms/engine/overrides.hpp`, `include/morph/forms/engine/form_view.hpp`
- Create: `tests/forms_engine_view_support.hpp` (the node outline and the RecordingBackend vocabulary engine view
  tests share)
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/typed_form.hpp`, add
  `include/morph/forms/engine/overrides.hpp` and `include/morph/forms/engine/form_view.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_view.cpp` after `test_forms_engine_typed.cpp`
- Test: `tests/test_forms_engine_view.cpp`

**Interfaces:**
- Consumes: Part 2 — `ui::Node`, the node aggregates and builders (`ui::text`, `button`, `textInput`, `checkbox`,
  `select`, `column`, `row`, `grid`, `panel`, `tabs`, `busy`, `dateTimeInput`, `slider`, `forEach<RowT>`),
  `ui::Prop<T>` (`evaluate()`), `ui::Common`, `ui::Key`, `ui::SelectOption`, `ui::GridCell`, `ui::Tab`,
  `ui::TextRole`, `ui::TextInputMode`, `ui::SelectStyle`, `ui::DateMode`; `ui::Mounted(reactive::Runtime&,
  IViewBackend&, Node)`; `ui::testing::RecordingBackend` (`all`, `find`, `prop`, `exists`, `edit`, `click`,
  `toggle`, `choose`, `setDateTime`, `slide`). Part 1 — `reactive::errorMessage`. Tasks 4–8.
- Produces (`morph::forms`), exactly:
  - `Overrides` — the contract's `byField(std::string path, Render)`, `byWidget(std::string hint, Render)`,
    `byUnit(std::string unit, Render)`, `byKind(FieldKind, Render)`, `Render = std::function<ui::Node(FieldView&)>`,
    plus `resolve(FieldSpec const&) const -> Render const*` (field path → widget hint → unit (`unitAscii`) → kind).
  - `FieldView` — `spec()`, `state()`, `session()`, `defaultControl()`; copyable, safe to keep inside a node.
  - `FormViewOptions{overrides, gridColumns = 2, submitLabel = "Submit"}` plus the addition
    `std::optional<int> flatGridColumns` (the implicit group's columns: 1 at the default of 2, else `gridColumns`).
  - `formView(FormSession&, FormViewOptions = {}) -> ui::Node`.
  - In `detail::engine`: `statusText(FormSession const&)`, `resultText(FormSession const&)`, `fieldCell`,
    `fieldControl` (Tasks 13–15 reuse them).

What each kind draws, and the frame around it:

| Kind | Control |
|---|---|
| Text, Array | `TextInput` (SingleLine) |
| Multiline | `TextInput` (Multiline) |
| Integer, Number, Decimal | `TextInput`, then `Text(unit)` when the field has one |
| Quantity | `Row{TextInput, Select(units)}`; `Text(unit)` instead of the `Select` without alternatives |
| Boolean | `Checkbox` |
| Enum | `Select` (Radio for `x-widget: radio`), options from the schema, nothing selected until chosen |
| Choice | `Row{Select, Busy}` — the options query's rows, `Busy` while it fetches |
| DateTime / Date | `DateTimeInput` (DateTime in the display zone / Date) |
| Slider | `Slider` over `x-min`/`x-max`/`x-step` |
| Object | `Panel(label)` around the member cells; a truncated one is a muted `Text` |
| ObjectArray | `Column{ForEach rows → Panel(label n){member cells, Button "Remove"}, Button "Add"}` |

A **cell** is `Column{Text(label, " *" while required), control, [Text(help, Muted)], Text(error, Error)}`, its
`visible` bound to the field's and its `enabled` to `!readonly`. The **form** is `Column{Text(action type,
Heading), group runs, Text(status, Muted), [Button(submitLabel) bound to ready], Text(last reply or error)}`; group
runs are a `Grid` per implicit group, a `Panel` per section, a collapsible `Panel` per accordion, and one `Tabs` per
run of consecutive tab groups. Every binding captures the session and its fields by pointer — mount the view after
the session and destroy it before — and a collection row's nodes hold that row's state, so a removed row lives until
its view unmounts.

- [ ] **Step 1: Write the failing test**

Create `tests/forms_engine_view_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms_engine_view_support.hpp
/// @brief A deterministic outline of a `ui::Node` tree (the golden form views compare against),
///        and the RecordingBackend kind names engine view tests look widgets up by.

#include <cstddef>
#include <cstdint>
#include <morph/ui/view.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace morph::test::formsview {

/// The RecordingBackend kind names (one per widget interface) these tests look up.
inline constexpr std::string_view kTextInput = "TextInput";
inline constexpr std::string_view kButton = "Button";
inline constexpr std::string_view kCheckbox = "Checkbox";
inline constexpr std::string_view kSelect = "Select";
inline constexpr std::string_view kDateTimeInput = "DateTimeInput";
inline constexpr std::string_view kSlider = "Slider";
inline constexpr std::string_view kPanel = "Panel";

namespace detail {

[[nodiscard]] inline std::string roleName(ui::TextRole role) {
    switch (role) {
        case ui::TextRole::Muted:
            return " (muted)";
        case ui::TextRole::Heading:
            return " (heading)";
        case ui::TextRole::Error:
            return " (error)";
        case ui::TextRole::Success:
            return " (success)";
        default:
            return "";
    }
}

[[nodiscard]] inline std::string flags(ui::Common const& common) {
    std::string out;
    if (!common.visible.evaluate()) {
        out += " hidden";
    }
    if (!common.enabled.evaluate()) {
        out += " disabled";
    }
    return out;
}

// NOLINTNEXTLINE(misc-no-recursion) -- a tree outline recurses into children
inline void outline(ui::Node const& node, std::size_t depth, std::string const& prefix, std::string& out) {
    std::string const indent(depth * 2, ' ');
    auto line = [&](std::string const& text, ui::Common const& common) {
        out += indent + prefix + text + flags(common) + "\n";
    };
    std::visit(
        [&](auto const& data) {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, ui::Text>) {
                line("Text \"" + data.text.evaluate() + "\"" + roleName(data.role.evaluate()), data.common);
            } else if constexpr (std::is_same_v<T, ui::Button>) {
                line("Button \"" + data.label.evaluate() + "\"", data.common);
            } else if constexpr (std::is_same_v<T, ui::TextInput>) {
                std::string const placeholder = data.placeholder.evaluate();
                line(std::string{"TextInput"} + (placeholder.empty() ? "" : " \"" + placeholder + "\"") +
                         (data.mode == ui::TextInputMode::Multiline ? " multiline" : ""),
                     data.common);
            } else if constexpr (std::is_same_v<T, ui::Checkbox>) {
                line("Checkbox", data.common);
            } else if constexpr (std::is_same_v<T, ui::Select>) {
                std::string labels;
                for (auto const& option : data.options.evaluate()) {
                    labels += (labels.empty() ? "" : "|") + option.label;
                }
                line("Select [" + labels + "]" + (data.style == ui::SelectStyle::Radio ? " radio" : ""), data.common);
            } else if constexpr (std::is_same_v<T, ui::Column> || std::is_same_v<T, ui::Row>) {
                line(std::is_same_v<T, ui::Column> ? "Column" : "Row", data.common);
                for (auto const& child : data.children) {
                    outline(child, depth + 1, "", out);
                }
            } else if constexpr (std::is_same_v<T, ui::Grid>) {
                line("Grid " + std::to_string(data.columns), data.common);
                for (auto const& cell : data.cells) {
                    outline(cell.node, depth + 1, "[" + std::to_string(cell.span) + "] ", out);
                }
            } else if constexpr (std::is_same_v<T, ui::Panel>) {
                line("Panel \"" + data.title.evaluate() + "\"" + (data.collapsible ? " collapsible" : ""), data.common);
                outline(data.child, depth + 1, "", out);
            } else if constexpr (std::is_same_v<T, ui::Tabs>) {
                line("Tabs", data.common);
                for (auto const& tab : data.tabs) {
                    out += std::string((depth + 1) * 2, ' ') + "Tab \"" + tab.label + "\"\n";
                    outline(tab.node, depth + 2, "", out);
                }
            } else if constexpr (std::is_same_v<T, ui::Dialog>) {
                line("Dialog \"" + data.title.evaluate() + "\"", data.common);
                outline(data.child, depth + 1, "", out);
            } else if constexpr (std::is_same_v<T, ui::Busy>) {
                line("Busy", data.common);
            } else if constexpr (std::is_same_v<T, ui::DateTimeInput>) {
                line(data.mode == ui::DateMode::Date ? "DateTimeInput date" : "DateTimeInput datetime", data.common);
            } else if constexpr (std::is_same_v<T, ui::Slider>) {
                line("Slider " + std::to_string(data.minimum) + ".." + std::to_string(data.maximum) + "/" +
                         std::to_string(data.step),
                     data.common);
            } else if constexpr (std::is_same_v<T, ui::ForEach>) {
                line("ForEach", data.common);
            } else if constexpr (std::is_same_v<T, ui::Table>) {
                std::string labels;
                for (auto const& column : data.columns) {
                    labels += (labels.empty() ? "" : "|") + column.label;
                }
                line("Table [" + labels + "]", data.common);
            } else if constexpr (std::is_same_v<T, ui::Menu>) {
                std::string labels;
                for (auto const& item : data.items) {
                    labels += (labels.empty() ? "" : "|") + item.label.evaluate();
                }
                line("Menu [" + labels + "]", data.common);
            } else if constexpr (std::is_same_v<T, ui::Switch>) {
                line("Switch", data.common);
            } else {
                line("Other", data.common);
            }
        },
        node->kind);
}

}  // namespace detail

/// @brief The tree under @p node, one line per node, two spaces per level: the kind, its
///        identifying text, ` hidden`/` disabled` from its current bindings. A ForEach's rows
///        and a Switch's cases are mounted lazily and are not expanded.
/// @param node The root.
/// @return The outline.
[[nodiscard]] inline std::string outline(ui::Node const& node) {
    std::string out;
    detail::outline(node, 0, "", out);
    return out;
}

}  // namespace morph::test::formsview
```

Create `tests/test_forms_engine_view.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <morph/forms/engine/form_view.hpp>
#include <morph/forms/engine/overrides.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"
#include "forms_engine_view_support.hpp"

namespace {

using morph::forms::FieldKind;
using morph::forms::FieldView;
using morph::forms::FormViewOptions;
using morph::test::formsengine::Harness;
using morph::test::formsview::outline;
namespace ui = morph::ui;
namespace view = morph::test::formsview;

constexpr std::string_view kLayout = R"({"properties":{
  "a":{"type":["integer","null"],"x-order":0,"title":"A","x-section":0,"x-colspan":4},
  "b":{"type":["integer","null"],"x-order":1,"title":"B","x-section":0,"x-colspan":8},
  "c":{"type":["integer","null"],"x-order":2,"title":"C","x-section":1,"x-colspan":3},
  "d":{"type":["integer","null"],"x-order":3,"title":"D","x-section":2,"x-colspan":2},
  "n":{"type":["integer","null"],"x-order":4,"title":"N","x-section":3},
  "e":{"type":["integer","null"],"x-order":5,"title":"E"}},"required":[],
  "x-layout":{"groups":[{"title":"Identity","kind":"section","fields":["a","b"]},{"title":"One","kind":"tab","fields":["c"]},
    {"title":"Two","kind":"tab","fields":["d"]},{"title":"More","kind":"accordion","fields":["n"]}]}})";

constexpr std::string_view kExplicit = R"({"properties":{
  "count":{"type":"integer","x-order":0,"title":"Count","description":"How many"},
  "note":{"type":["string","null"],"x-order":1,"title":"Note"}},"required":["count"],"x-submitMode":"explicit"})";

/// A field cell as the golden outlines print it.
[[nodiscard]] std::string cell(std::string_view prefix, std::string_view indent, std::string_view label,
                               std::string_view control) {
    std::string const deeper = std::string{indent} + "  ";
    return std::string{indent} + std::string{prefix} + "Column\n" + deeper + "Text \"" + std::string{label} + "\"\n" + deeper +
           std::string{control} + "\n" + deeper + "Text \"\" (error) hidden\n";
}

}  // namespace

TEST_CASE("formView: sections, tab runs, accordions and the implicit group, each on its grid", "[forms-engine][view]") {
    Harness harness;
    harness.server.replyWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session("T_Layout", kLayout);
    std::string const expected = std::string{"Column\n"} + "  Text \"T_Layout\" (heading)\n" + "  Panel \"Identity\"\n" +
                                 "    Grid 2\n" + cell("[2] ", "      ", "A", "TextInput \"0\"") +
                                 cell("[2] ", "      ", "B", "TextInput \"0\"") + "  Tabs\n" + "    Tab \"One\"\n" +
                                 "      Grid 2\n" + cell("[2] ", "        ", "C", "TextInput \"0\"") + "    Tab \"Two\"\n" +
                                 "      Grid 2\n" + cell("[2] ", "        ", "D", "TextInput \"0\"") +
                                 "  Panel \"More\" collapsible\n" + "    Grid 2\n" +
                                 cell("[1] ", "      ", "N", "TextInput \"0\"") + "  Grid 1\n" +
                                 cell("[1] ", "    ", "E", "TextInput \"0\"") +
                                 "  Text \"✓ executes automatically as you type\" (muted)\n" +
                                 "  Text \"\" (success) hidden\n";
    CHECK(outline(morph::forms::formView(*form)) == expected);
}

TEST_CASE("formView: a host grid widens every group's grid; spans clamp to it", "[forms-engine][view]") {
    Harness harness;
    auto const form = harness.session("T_Layout", kLayout);
    std::string const twelve = outline(morph::forms::formView(*form, FormViewOptions{.gridColumns = 12}));
    CHECK(twelve.find("    Grid 12\n      [4] Column\n") != std::string::npos);
    CHECK(twelve.find("      [8] Column\n") != std::string::npos);
    CHECK(twelve.find("        [3] Column\n") != std::string::npos);
    CHECK(twelve.find("  Grid 12\n    [1] Column\n") != std::string::npos);  // the implicit group follows the host grid
    std::string const flatTwo =
        outline(morph::forms::formView(*form, FormViewOptions{.gridColumns = 2, .flatGridColumns = 2}));
    CHECK(flatTwo.find("  Grid 2\n    [1] Column\n") != std::string::npos);
    auto const wide = harness.session("T_Flat", R"({"properties":{"a":{"type":"integer","x-order":0,"title":"A","x-colspan":20}},"required":[]})");
    CHECK(outline(morph::forms::formView(*wide, FormViewOptions{.gridColumns = 12})).find("[12] Column") != std::string::npos);
    CHECK(outline(morph::forms::formView(*wide)).find("[1] Column") != std::string::npos);
}

TEST_CASE("formView: explicit mode adds a Submit button bound to readiness; the cell shows help and a live *",
          "[forms-engine][view]") {
    Harness harness;
    harness.server.replyWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session("T_Chrome", kExplicit);
    std::string const expected = std::string{"Column\n"} + "  Text \"T_Chrome\" (heading)\n" + "  Grid 1\n" +
                                 "    [1] Column\n" + "      Text \"Count *\"\n" + "      TextInput \"0\"\n" +
                                 "      Text \"How many\" (muted)\n" + "      Text \"\" (error) hidden\n" +
                                 cell("[1] ", "    ", "Note", "TextInput") +
                                 "  Text \"fill the required (*) fields\" (muted)\n" + "  Button \"Submit\" disabled\n" +
                                 "  Text \"\" (success) hidden\n";
    CHECK(outline(morph::forms::formView(*form)) == expected);
    harness.type(*form, "count", "abc");
    CHECK(outline(morph::forms::formView(*form)).find("Text \"Not a valid value\" (error)\n") != std::string::npos);
}

TEST_CASE("formView: the mounted Submit button follows readiness and submits on click", "[forms-engine][view]") {
    Harness harness;
    auto const form = harness.session("CFR_BookRoom", R"({"properties":{"name":{"type":"string","x-order":0}},"required":["name"],"x-submitMode":"explicit"})");
    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::formView(*form)};
    harness.settle();
    auto const submit = backend.find(view::kButton, "label", "Submit");
    REQUIRE(submit.has_value());
    CHECK(backend.prop(*submit, "enabled") == "false");
    backend.click(*submit);
    harness.settle();
    CHECK(harness.server.submits().empty());
    backend.edit(backend.all(view::kTextInput).front(), "Alice");
    harness.settle();
    CHECK(backend.prop(*submit, "enabled") == "true");
    CHECK(harness.server.submits().empty());
    backend.click(*submit);
    harness.settle();
    REQUIRE(harness.server.submits().size() == 1);
    CHECK(harness.server.lastBody() == R"({"name":"Alice"})");
    CHECK(backend.find("Text", "text", R"(ok: {"ok":true})").has_value());

    auto const automatic = harness.session("CFR_BookRoom", R"({"properties":{"name":{"type":"string","x-order":0}},"required":["name"]})");
    CHECK(outline(morph::forms::formView(*automatic)).find("Button") == std::string::npos);
}

TEST_CASE("formView: each kind draws its control", "[forms-engine][view]") {
    Harness harness;
    harness.server.optionsWith([](auto const&) { return std::nullopt; });
    auto const form = harness.session("T_Kinds", R"({"$defs":{"q":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}},"properties":{
      "mass":{"$ref":"#/$defs/q","x-order":0,"title":"Mass","x-decimalPlaces":3,"x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}]},
      "density":{"type":"number","x-order":1,"title":"Density","ExtUnits":{"unitAscii":"kg/m³","unitUnicode":"kg/m³"},"x-displayDecimals":3},
      "done":{"type":"boolean","x-order":2,"title":"Done"},
      "role":{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},{"title":"Manager","const":"Manager"}],"x-order":3,"title":"Role"},
      "mode":{"type":"integer","x-order":4,"title":"Mode","x-widget":"radio","x-optionsAction":"ListModes"},
      "when":{"type":"string","format":"date-time","x-order":5,"title":"When"},
      "day":{"type":"string","format":"date","x-order":6,"title":"Day"},
      "level":{"type":"integer","x-order":7,"title":"Level","x-widget":"slider","x-min":0,"x-max":100,"x-step":5},
      "tags":{"type":"array","items":{"type":"string"},"x-order":8,"title":"Tags"},
      "notes":{"type":"string","x-order":9,"title":"Notes","x-widget":"textarea"}},"required":[]})");
    std::string const drawn = outline(morph::forms::formView(*form));
    CHECK(drawn.find("      Row\n        TextInput \"0.000\"\n        Select [kg|g]\n") != std::string::npos);
    CHECK(drawn.find("      Row\n        TextInput \"0.000\"\n        Text \"kg/m³\" (muted)\n") != std::string::npos);
    CHECK(drawn.find("      Checkbox\n") != std::string::npos);
    CHECK(drawn.find("      Select [Viewer|Manager]\n") != std::string::npos);
    CHECK(drawn.find("        Select [] radio\n        Busy\n") != std::string::npos);
    CHECK(drawn.find("      DateTimeInput datetime\n") != std::string::npos);
    CHECK(drawn.find("      DateTimeInput date\n") != std::string::npos);
    CHECK(drawn.find("      Slider 0..100/5\n") != std::string::npos);
    CHECK(drawn.find("      TextInput \"comma-separated (e.g. red, green, blue)\"\n") != std::string::npos);
    CHECK(drawn.find("      TextInput multiline\n") != std::string::npos);
}

TEST_CASE("formView: mounted controls drive the session", "[forms-engine][view]") {
    Harness harness;
    auto const form = harness.session("T_Mounted", R"({"$defs":{"q":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}},"properties":{
      "mass":{"$ref":"#/$defs/q","x-order":0,"x-decimalPlaces":3,"x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}]},
      "done":{"type":"boolean","x-order":1},
      "role":{"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},{"title":"Manager","const":"Manager"}],"x-order":2},
      "when":{"type":"string","format":"date-time","x-order":3},
      "level":{"type":"integer","x-order":4,"x-widget":"slider","x-min":0,"x-max":100,"x-step":5}},"required":["mass","done","role","when","level"]})");
    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::formView(*form)};
    harness.settle();
    backend.edit(backend.all(view::kTextInput).front(), "2650.5");
    // The unit selector keys its options by position; RecordingBackend::chooseIndex drives menus and tabs only.
    backend.choose(backend.all(view::kSelect).at(0), ui::Key{std::int64_t{1}});  // grams
    harness.settle();
    CHECK(form->field("mass").text().peek() == "2650500.0");
    CHECK(backend.prop(backend.all(view::kTextInput).front(), "text") == "2650500.0");
    backend.toggle(backend.all(view::kCheckbox).front());
    backend.choose(backend.all(view::kSelect).at(1), ui::Key{std::string{R"("Manager")"}});
    backend.setDateTime(backend.all(view::kDateTimeInput).front(),
                        morph::time::Timestamp{morph::time::DateTime{std::chrono::sys_days{std::chrono::July / 5 / 2026} +
                                                                     std::chrono::hours{14} + std::chrono::minutes{30}}});
    backend.slide(backend.all(view::kSlider).front(), 45);
    harness.settle();
    CHECK(form->body() == R"({"mass":{"num":26505000,"den":10000,"dp":3},"done":true,"role":"Manager",)"
                          R"("when":"2026-07-05T14:30:00Z","level":45})");
}

TEST_CASE("formView: visibility and read-only follow the rules on the mounted cells", "[forms-engine][view]") {
    Harness harness;
    auto const form = harness.session("T_Rules", R"({"properties":{
      "promo":{"type":"integer","x-order":0,"title":"Promo"},"discount":{"type":"integer","x-order":1,"title":"Discount"},
      "total":{"type":"integer","x-order":2,"title":"Total"}},"required":[],
      "x-rules":[{"kind":"visibleWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}},
        {"kind":"readonlyWhen","fields":["total"],"when":{"kind":"engaged","fields":["promo"]}},
        {"kind":"requiredWhen","fields":["total"],"when":{"kind":"engaged","fields":["promo"]}}]})");
    std::string const before = outline(morph::forms::formView(*form));
    CHECK(before.find("    [1] Column hidden\n      Text \"Discount\"\n") != std::string::npos);
    CHECK(before.find("      Text \"Total\"\n") != std::string::npos);
    harness.type(*form, "promo", "5");
    std::string const after = outline(morph::forms::formView(*form));
    CHECK(after.find("    [1] Column\n      Text \"Discount\"\n") != std::string::npos);
    CHECK(after.find("    [1] Column disabled\n      Text \"Total *\"\n") != std::string::npos);
}

TEST_CASE("formView: switching tabs keeps what was typed and submits nothing", "[forms-engine][view]") {
    Harness harness;
    auto const form = harness.session("TabResubmit", R"({"properties":{"a":{"type":"integer","x-order":0,"x-section":0},
      "b":{"type":["integer","null"],"x-order":1,"x-section":1}},"required":["a"],
      "x-layout":{"groups":[{"title":"One","kind":"tab","fields":["a"]},{"title":"Two","kind":"tab","fields":["b"]}]}})");
    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::formView(*form)};
    harness.settle();
    backend.edit(backend.all(view::kTextInput).front(), "1");
    harness.settle();
    REQUIRE(harness.server.submits().size() == 1);
    form->tabSelection(0).set(1);
    harness.settle();
    form->tabSelection(0).set(0);
    harness.settle();
    CHECK(backend.prop(backend.all(view::kTextInput).front(), "text") == "1");
    CHECK(form->field("a").text().peek() == "1");
    CHECK(harness.server.submits().size() == 1);
}

TEST_CASE("formView: labels fall back to the wire key, follow x-order and come from translations",
          "[forms-engine][view]") {
    morph::render::TranslationProvider const catalog = [](std::string_view key, std::string_view) -> std::optional<std::string> {
        if (key == "AccessibilityProbe.group.0") {
            return "Erste";
        }
        return std::nullopt;
    };
    Harness harness;
    auto const form = harness.session("AccessibilityProbe", R"({"properties":{"second":{"type":"string","x-order":1,"x-section":0},
      "first":{"type":"integer","x-order":0,"x-section":0}},"required":["first","second"],
      "x-layout":{"groups":[{"title":"First","kind":"section","fields":["first","second"]}]}})",
                                morph::forms::FormSessionOptions{.translations = catalog});
    std::string const drawn = outline(morph::forms::formView(*form));
    CHECK(drawn.find("Panel \"Erste\"") != std::string::npos);
    auto const first = drawn.find("Text \"first *\"");
    auto const second = drawn.find("Text \"second *\"");
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    CHECK(first < second);
}

TEST_CASE("Overrides: field beats widget beats unit beats kind; a miss draws the built-in control",
          "[forms-engine][view][overrides]") {
    auto const marker = [](std::string text) {
        return [text](FieldView&) { return ui::text(ui::Text{.text = text}); };
    };
    morph::forms::Overrides overrides;
    overrides.byKind(FieldKind::Integer, marker("kind")).byUnit("kg", marker("unit"));
    overrides.byWidget("slider", marker("widget")).byField("mass", marker("field"));
    Harness harness;
    auto const form = harness.session("Probe", R"({"properties":{
      "mass":{"type":"integer","x-order":0,"x-widget":"slider","x-min":0,"x-max":9,"ExtUnits":{"unitAscii":"kg"}},
      "level":{"type":"integer","x-order":1,"x-widget":"slider","x-min":0,"x-max":9,"ExtUnits":{"unitAscii":"kg"}},
      "weight":{"type":"integer","x-order":2,"ExtUnits":{"unitAscii":"kg"}},
      "count":{"type":"integer","x-order":3},"note":{"type":"string","x-order":4}},"required":[]})");
    std::string const drawn = outline(morph::forms::formView(*form, FormViewOptions{.overrides = overrides}));
    auto const order = [&](std::string_view needle) { return drawn.find(needle); };
    CHECK(order("Text \"field\"") < order("Text \"widget\""));
    CHECK(order("Text \"widget\"") < order("Text \"unit\""));
    CHECK(order("Text \"unit\"") < order("Text \"kind\""));
    CHECK(order("Text \"kind\"") < order("Text \"note\""));
    CHECK(drawn.find("Slider") == std::string::npos);
    CHECK(drawn.find("TextInput\n") != std::string::npos);  // note: no override, the built-in control
    morph::forms::Overrides other;
    other.byField("elsewhere", marker("field"));
    CHECK(other.resolve(form->field("mass").spec()) == nullptr);
}

TEST_CASE("Overrides: an override receives the field and drives the session like the built-in control",
          "[forms-engine][view][overrides]") {
    Harness harness;
    auto const form = harness.session("T_Reading", R"({"properties":{"density":{"type":"number","x-order":0,"title":"Density",
      "ExtUnits":{"unitAscii":"kg/m³","unitUnicode":"kg/m³"},"x-displayDecimals":3}},"required":["density"]})");
    std::string seenUnit;
    std::optional<std::uint32_t> seenDecimals;
    morph::forms::Overrides overrides;
    overrides.byUnit("kg/m³", [&](FieldView& field) {
        seenUnit = field.spec().unit;
        seenDecimals = field.spec().displayDecimals;
        morph::forms::FieldState* state = &field.state();
        return ui::column(ui::Column{.children = {ui::text(ui::Text{.text = "custom"}), field.defaultControl()},
                                     .common = {.visible = [state] { return state->visible().get(); }}});
    });
    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::formView(*form, FormViewOptions{.overrides = overrides})};
    harness.settle();
    CHECK(seenUnit == "kg/m³");
    CHECK(seenDecimals == 3U);
    CHECK(backend.find("Text", "text", "custom").has_value());
    backend.edit(backend.all(view::kTextInput).front(), "1.25");
    harness.settle();
    CHECK(form->body() == R"({"density":1.25})");
}

TEST_CASE("formView: an object is a panel of member cells; a collection adds and removes rows",
          "[forms-engine][view][nested]") {
    Harness harness;
    auto const form = harness.session("T_Grading", R"({"$defs":{"Row":{"type":"object","required":["sieve"],"properties":{
        "sieve":{"type":"number","x-order":0,"title":"Sieve"}}}},"properties":{
      "address":{"type":"object","x-order":0,"title":"Address","required":["street"],"properties":{"street":{"type":"string","x-order":0,"title":"Street"}}},
      "rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":1,"title":"Rows"}},"required":["rows"]})");
    std::string const drawn = outline(morph::forms::formView(*form));
    CHECK(drawn.find("      Panel \"Address\"\n        Column\n          Column\n            Text \"Street *\"\n") !=
          std::string::npos);
    CHECK(drawn.find("      Column\n        ForEach\n        Button \"Add\"\n") != std::string::npos);

    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::formView(*form)};
    harness.settle();
    auto const add = backend.find(view::kButton, "label", "Add");
    REQUIRE(add.has_value());
    backend.click(*add);
    backend.click(*add);
    harness.settle();
    auto inputs = backend.all(view::kTextInput);
    REQUIRE(inputs.size() == 3);  // street, two sieves
    backend.edit(inputs.at(1), "31.5");
    backend.edit(inputs.at(2), "0.063");
    harness.settle();
    CHECK(form->body() == R"({"rows":[{"sieve":31.5},{"sieve":0.063}]})");
    auto const firstRemove = backend.find(view::kButton, "label", "Remove");
    REQUIRE(firstRemove.has_value());
    backend.click(*firstRemove);
    harness.settle();
    CHECK(form->body() == R"({"rows":[{"sieve":0.063}]})");
    CHECK(backend.all(view::kTextInput).size() == 2);
}

// Review Focus 5.
TEST_CASE("formView: a row removed while its view is mounted is torn down after its bindings",
          "[forms-engine][view][nested]") {
    Harness harness;
    auto const form = harness.session("T_Rows", R"({"$defs":{"Row":{"type":"object","properties":{"v":{"type":"integer","x-order":0}}}},
      "properties":{"rows":{"type":"array","items":{"$ref":"#/$defs/Row"},"x-order":0}},"required":[]})");
    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::formView(*form)};
    harness.settle();
    auto& rows = form->field("rows");
    auto const key = rows.addRow();
    harness.settle();
    auto const input = backend.all(view::kTextInput).front();
    // Remove the row and edit its (still mounted) input in the same turn: the row's state must
    // outlive every binding that reads it. ASan is the observer here.
    rows.removeRow(key);
    backend.edit(input, "7");
    harness.settle();
    CHECK_FALSE(backend.exists(input));
    CHECK(form->body() == R"({"rows":[]})");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/form_view.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/overrides.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/overrides.hpp
/// @brief `Overrides`: an application's replacements for the control `formView` draws for a
///        field, chosen by field path, widget hint, unit or kind.
///
/// Specified in `docs/spec/forms/engine.md`, "Overrides".

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "../../ui/view.hpp"
#include "field_model.hpp"

namespace morph::forms {

class FieldView;

/// @brief Replacement controls, resolved per field in the order field path → widget hint →
///        unit → kind, else the built-in control.
///
/// An override replaces the control only: the cell around it — label, help, error, visibility —
/// is still drawn. `FieldView::defaultControl()` gives the built-in control to wrap.
class Overrides {
public:
    /// @brief Draws a field's control.
    using Render = std::function<ui::Node(FieldView&)>;

    /// @brief Overrides one field.
    /// @param path   The field's spec path (`FieldSpec::path`: `name`, `parent.member`, `rows[].member`).
    /// @param render The control.
    /// @return `*this`, for chaining.
    Overrides& byField(std::string path, Render render) {
        _byField.insert_or_assign(std::move(path), std::move(render));
        return *this;
    }

    /// @brief Overrides every field with an `x-widget` hint.
    /// @param hint   The hint, e.g. `"slider"`.
    /// @param render The control.
    /// @return `*this`, for chaining.
    Overrides& byWidget(std::string hint, Render render) {
        _byWidget.insert_or_assign(std::move(hint), std::move(render));
        return *this;
    }

    /// @brief Overrides every field in a unit.
    /// @param unit   The unit's `ExtUnits.unitAscii`.
    /// @param render The control.
    /// @return `*this`, for chaining.
    Overrides& byUnit(std::string unit, Render render) {
        _byUnit.insert_or_assign(std::move(unit), std::move(render));
        return *this;
    }

    /// @brief Overrides every field of a kind.
    /// @param kind   The kind.
    /// @param render The control.
    /// @return `*this`, for chaining.
    Overrides& byKind(FieldKind kind, Render render) {
        _byKind.insert_or_assign(kind, std::move(render));
        return *this;
    }

    /// @brief The override for @p spec, if any.
    /// @param spec The field.
    /// @return The first match in field → widget → unit → kind order, or null.
    [[nodiscard]] Render const* resolve(FieldSpec const& spec) const {
        if (auto const found = _byField.find(spec.path); found != _byField.end()) {
            return &found->second;
        }
        if (!spec.widget.empty()) {
            if (auto const found = _byWidget.find(spec.widget); found != _byWidget.end()) {
                return &found->second;
            }
        }
        if (!spec.unitAscii.empty()) {
            if (auto const found = _byUnit.find(spec.unitAscii); found != _byUnit.end()) {
                return &found->second;
            }
        }
        if (auto const found = _byKind.find(spec.kind); found != _byKind.end()) {
            return &found->second;
        }
        return nullptr;
    }

private:
    std::map<std::string, Render, std::less<>> _byField;
    std::map<std::string, Render, std::less<>> _byWidget;
    std::map<std::string, Render, std::less<>> _byUnit;
    std::map<FieldKind, Render> _byKind;
};

}  // namespace morph::forms
```

Create `include/morph/forms/engine/form_view.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/form_view.hpp
/// @brief `formView`: a `FormSession` rendered as a `ui::Node` tree, so one form draws on every
///        frontend; `FieldView`, what an override is handed.
///
/// Specified in `docs/spec/forms/engine.md`, "Rendering".

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "../../reactive/control.hpp"
#include "../../ui/view.hpp"
#include "../../util/datetime.hpp"
#include "../layout.hpp"
#include "detail/text.hpp"
#include "field_model.hpp"
#include "form_session.hpp"
#include "overrides.hpp"

namespace morph::forms {

/// @brief How `formView` lays a form out.
struct FormViewOptions {
    /// @brief Replacement controls.
    Overrides overrides{};
    /// @brief Columns of every section's, accordion's and tab's grid; `x-colspan` clamps to it.
    int gridColumns = 2;
    /// @brief The explicit-mode button's label.
    std::string submitLabel = "Submit";
    /// @brief Columns of the implicit group's grid; unset is 1 while `gridColumns` is 2, else `gridColumns`.
    std::optional<int> flatGridColumns{};
};

/// @brief A field's spec and state, handed to an override. Copyable; safe to keep inside a node
///        for as long as the session lives.
class FieldView {
public:
    /// @param session The session. Borrowed.
    /// @param state   The field. Borrowed.
    /// @param options The view options the form is drawn with.
    FieldView(FormSession& session, FieldState& state, std::shared_ptr<FormViewOptions const> options) noexcept
        : _session{&session}, _state{&state}, _options{std::move(options)} {}

    /// @brief The field's spec.
    /// @return The spec.
    [[nodiscard]] FieldSpec const& spec() const noexcept { return _state->spec(); }

    /// @brief The field's state: draft, encoding, error, visibility, options.
    /// @return The state.
    [[nodiscard]] FieldState& state() const noexcept { return *_state; }

    /// @brief The session the field belongs to.
    /// @return The session.
    [[nodiscard]] FormSession& session() const noexcept { return *_session; }

    /// @brief The control `formView` draws for this field without overrides.
    /// @return The control.
    [[nodiscard]] ui::Node defaultControl() const;

private:
    FormSession* _session;
    FieldState* _state;
    std::shared_ptr<FormViewOptions const> _options;
};

namespace detail::engine {

/// @brief The shared view options a form's nodes hold.
using ViewOptions = std::shared_ptr<FormViewOptions const>;

/// @brief The status line under a form.
/// @param session The session.
/// @return `fill the required (*) fields` while not ready; else a line saying how it submits.
[[nodiscard]] inline std::string statusText(FormSession const& session) {
    if (!session.ready()) {
        return "fill the required (*) fields";
    }
    return session.submitMode() == SubmitMode::Explicit ? "✓ ready -- press Submit" : "✓ executes automatically as you type";
}

/// @brief The last reply or failure.
/// @param session The session.
/// @return `error: <message>`, `ok: <reply>`, or empty before the first answer.
[[nodiscard]] inline std::string resultText(FormSession const& session) {
    if (std::exception_ptr const error = session.lastError()) {
        return "error: " + reactive::errorMessage(error);
    }
    std::optional<std::string> const& reply = session.lastReply();
    return reply.has_value() ? "ok: " + *reply : std::string{};
}

[[nodiscard]] ui::Node fieldCell(FormSession& session, FieldState& state, ViewOptions const& options);

/// @brief A draft wall clock (`YYYY-MM-DDTHH:MM[:SS]`, or a `YYYY-MM-DD` date) as the instant it denotes.
/// @param text          The draft.
/// @param date          Whether the field is a Date.
/// @param offsetMinutes The display zone.
/// @return The instant, or `std::nullopt` while the draft is not a complete one.
[[nodiscard]] inline std::optional<time::Timestamp> draftInstant(std::string_view text, bool date, int offsetMinutes) {
    std::optional<std::chrono::sys_seconds> local;
    if (date) {
        if (isIsoDate(text)) {
            local = parseWallClock(std::string{text} + "T00:00");
        }
    } else {
        local = parseWallClock(text);
    }
    if (!local) {
        return std::nullopt;
    }
    auto const utc = *local - std::chrono::minutes{date ? 0 : offsetMinutes};
    return time::Timestamp{time::DateTime{std::chrono::time_point_cast<std::chrono::milliseconds>(utc)}};
}

/// @brief The draft a date-time control writes for an instant (blank for none).
/// @param value         The instant.
/// @param date          Whether the field is a Date.
/// @param offsetMinutes The display zone.
/// @return `YYYY-MM-DDTHH:MM:SS` in the zone, or `YYYY-MM-DD`.
[[nodiscard]] inline std::string instantDraft(std::optional<time::Timestamp> const& value, bool date, int offsetMinutes) {
    if (!value || !value->hasValue()) {
        return {};
    }
    auto const instant = std::chrono::floor<std::chrono::seconds>((**value).value) +
                         std::chrono::minutes{date ? 0 : offsetMinutes};
    std::string text = formatWallClock(instant);
    return date ? text.substr(0, 10) : text;
}

/// @brief The built-in control for one field (see the table in `docs/spec/forms/engine.md`).
/// @param session The session.
/// @param state   The field.
/// @param options The view options (members and rows draw their own cells with them).
/// @return The control.
// NOLINTNEXTLINE(misc-no-recursion) -- an Object or ObjectArray draws its members' cells
[[nodiscard]] inline ui::Node fieldControl(FormSession& session, FieldState& state, ViewOptions const& options) {
    FieldState* const field = &state;
    FieldSpec const& spec = state.spec();
    auto const textInput = [field](ui::TextInputMode mode) {
        return ui::textInput(ui::TextInput{.value = [field] { return field->text().get(); },
                                           .onChange = [field](std::string text) { field->text().set(std::move(text)); },
                                           .placeholder = field->placeholder(),
                                           .mode = mode});
    };
    auto const unitSuffix = [&](ui::Node input) {
        if (spec.unit.empty()) {
            return input;
        }
        return ui::row(ui::Row{.children = {std::move(input), ui::text(ui::Text{.text = spec.unit, .role = ui::TextRole::Muted})},
                               .gap = 1});
    };
    auto const optionSelect = [field](ui::SelectStyle style) {
        return ui::select(ui::Select{
            .options =
                [field] {
                    std::vector<ui::SelectOption> options;
                    for (ChoiceOption const& option : field->options()) {
                        options.push_back(ui::SelectOption{ui::Key{option.valueJson}, option.label});
                    }
                    return options;
                },
            .selected = [field]() -> std::optional<ui::Key> {
                std::string_view const selected = trim(field->text().get());
                return selected.empty() ? std::nullopt : std::optional<ui::Key>{ui::Key{std::string{selected}}};
            },
            .onSelect =
                [field](ui::Key key) {
                    if (auto const* const value = std::get_if<std::string>(&key)) {
                        field->text().set(*value);
                    }
                },
            .style = style});
    };
    switch (spec.kind) {
        case FieldKind::Multiline:
            return textInput(ui::TextInputMode::Multiline);
        case FieldKind::Integer:
        case FieldKind::Number:
        case FieldKind::Decimal:
            return unitSuffix(textInput(ui::TextInputMode::SingleLine));
        case FieldKind::Quantity: {
            if (spec.units.size() <= 1) {
                return unitSuffix(textInput(ui::TextInputMode::SingleLine));
            }
            std::vector<ui::SelectOption> units;
            for (std::size_t i = 0; i < spec.units.size(); ++i) {
                units.push_back(ui::SelectOption{ui::Key{static_cast<std::int64_t>(i)}, spec.units.at(i).display});
            }
            return ui::row(ui::Row{
                .children = {textInput(ui::TextInputMode::SingleLine),
                             ui::select(ui::Select{
                                 .options = std::move(units),
                                 .selected = [field]() -> std::optional<ui::Key> {
                                     return ui::Key{static_cast<std::int64_t>(field->unit().get())};
                                 },
                                 .onSelect =
                                     [field](ui::Key key) {
                                         if (auto const* const index = std::get_if<std::int64_t>(&key); index != nullptr && *index >= 0) {
                                             field->switchUnit(static_cast<std::size_t>(*index));
                                         }
                                     }})},
                .gap = 1});
        }
        case FieldKind::Boolean:
            return ui::checkbox(ui::Checkbox{.label = std::string{},
                                             .checked = [field] { return trim(field->text().get()) == "true"; },
                                             .onToggle = [field](bool checked) { field->text().set(checked ? "true" : "false"); }});
        case FieldKind::Enum:
            return optionSelect(spec.widget == "radio" ? ui::SelectStyle::Radio : ui::SelectStyle::Dropdown);
        case FieldKind::Choice:
            return ui::row(ui::Row{
                .children = {optionSelect(spec.widget == "radio" ? ui::SelectStyle::Radio : ui::SelectStyle::Dropdown),
                             ui::busy(ui::Busy{.active = [field] { return field->optionsPending(); }, .label = std::string{}})},
                .gap = 1});
        case FieldKind::DateTime:
        case FieldKind::Date: {
            bool const date = spec.kind == FieldKind::Date;
            int const offset = date ? 0 : session.options().displayOffsetMinutes;
            return ui::dateTimeInput(ui::DateTimeInput{
                .value = [field, date, offset] { return draftInstant(trim(field->text().get()), date, offset); },
                .onChange = [field, date, offset](std::optional<time::Timestamp> value) {
                    field->text().set(instantDraft(value, date, offset));
                },
                .mode = date ? ui::DateMode::Date : ui::DateMode::DateTime,
                .offsetMinutes = offset});
        }
        case FieldKind::Slider:
            return ui::slider(ui::Slider{.value =
                                             [field] {
                                                 std::string_view const text = trim(field->text().get());
                                                 std::int64_t value = field->spec().sliderMin;
                                                 std::from_chars(text.data(), text.data() + text.size(), value);
                                                 return value;
                                             },
                                         .minimum = spec.sliderMin,
                                         .maximum = spec.sliderMax,
                                         .step = spec.sliderStep,
                                         .onChange = [field](std::int64_t value) { field->text().set(std::to_string(value)); }});
        case FieldKind::Object: {
            if (spec.truncated) {
                return ui::text(ui::Text{.text = "nested too deeply to edit here", .role = ui::TextRole::Muted});
            }
            std::vector<ui::Node> cells;
            for (auto const& member : state.members()) {
                cells.push_back(fieldCell(session, *member, options));
            }
            return ui::panel(ui::Panel{.title = state.label(), .padding = 1, .child = ui::column(ui::Column{.children = std::move(cells)})});
        }
        case FieldKind::ObjectArray: {
            if (spec.truncated) {
                return ui::text(ui::Text{.text = "nested too deeply to edit here", .role = ui::TextRole::Muted});
            }
            FormSession* const owner = &session;
            ViewOptions const shared = options;
            auto rows = ui::forEach<std::shared_ptr<RowState>>(
                [field] { return field->rows(); },
                [](std::shared_ptr<RowState> const& row) { return ui::Key{static_cast<std::int64_t>(row->key())}; },
                [field, owner, shared](reactive::Signal<std::shared_ptr<RowState>> const& slot) {
                    std::shared_ptr<RowState> const row = slot.peek();
                    std::vector<ui::Node> cells;
                    for (auto const& member : row->fields()) {
                        cells.push_back(fieldCell(*owner, *member, shared));
                    }
                    std::uint64_t const key = row->key();
                    cells.push_back(ui::button(ui::Button{.label = "Remove", .onClick = [field, key] { field->removeRow(key); }}));
                    // The row is held by the ForEach's slot for this key and, explicitly, by the title
                    // binding — the row view's first binding, destroyed after every binding inside the
                    // panel — so the row's state outlives everything that reads it after the session
                    // has let it go.
                    return ui::panel(ui::Panel{
                        .title = [field, row] {
                            auto const& rows = field->rows();
                            auto const found = std::ranges::find(rows, row);
                            return field->label() + " " + std::to_string(found - rows.begin() + 1);
                        },
                        .padding = 1,
                        .child = ui::column(ui::Column{.children = std::move(cells)})});
                });
            return ui::column(ui::Column{
                .children = {std::move(rows), ui::button(ui::Button{.label = "Add", .onClick = [field] { field->addRow(); }})}});
        }
        default:
            return textInput(ui::TextInputMode::SingleLine);
    }
}

/// @brief One field's cell: label (with ` *` while required), control (an override's, else the
///        built-in), help, error; visible and enabled as the field is.
/// @param session The session.
/// @param state   The field.
/// @param options The view options.
/// @return The cell.
// NOLINTNEXTLINE(misc-no-recursion) -- nested controls draw cells
inline ui::Node fieldCell(FormSession& session, FieldState& state, ViewOptions const& options) {
    FieldState* const field = &state;
    ui::Node control;
    if (Overrides::Render const* const render = options->overrides.resolve(state.spec())) {
        FieldView view{session, state, options};
        control = (*render)(view);
    } else {
        control = fieldControl(session, state, options);
    }
    std::vector<ui::Node> children;
    children.push_back(ui::text(ui::Text{.text = [field] { return field->label() + (field->required().get() ? " *" : ""); }}));
    children.push_back(std::move(control));
    if (!state.help().empty()) {
        children.push_back(ui::text(ui::Text{.text = state.help(), .role = ui::TextRole::Muted}));
    }
    children.push_back(ui::text(ui::Text{.text = [field] {
                                             auto const& error = field->error().get();
                                             return error.has_value() ? error->message : std::string{};
                                         },
                                         .role = ui::TextRole::Error,
                                         .common = {.visible = [field] { return field->error().get().has_value(); }}}));
    return ui::column(ui::Column{.children = std::move(children),
                                 .common = {.visible = [field] { return field->visible().get(); },
                                            .enabled = [field] { return !field->readonly().get(); }}});
}

/// @brief The fields of one group on a grid of @p columns, each spanning `x-colspan` clamped to it.
/// @param session The session.
/// @param group   The group.
/// @param columns The grid's columns.
/// @param options The view options.
/// @return The grid.
[[nodiscard]] inline ui::Node groupGrid(FormSession& session, FieldGroupSpec const& group, int columns,
                                        ViewOptions const& options) {
    std::vector<ui::GridCell> cells;
    for (std::string const& name : group.fields) {
        FieldState& state = session.field(name);
        cells.push_back(ui::GridCell{.node = fieldCell(session, state, options),
                                     .span = std::clamp(state.spec().colspan, 1, std::max(1, columns))});
    }
    return ui::grid(ui::Grid{.columns = std::max(1, columns), .cells = std::move(cells), .gap = 1});
}

}  // namespace detail::engine

inline ui::Node FieldView::defaultControl() const { return detail::engine::fieldControl(*_session, *_state, _options); }

/// @brief Renders @p session: its fields laid out by group, the status line, the Submit button in
///        explicit mode, and the last reply or error.
///
/// The nodes bind to the session and its fields by pointer: mount the result after the session
/// and destroy the mount before it.
/// @param session The session.
/// @param options Overrides, grid columns, the Submit label.
/// @return The view.
[[nodiscard]] inline ui::Node formView(FormSession& session, FormViewOptions options = {}) {
    using detail::engine::groupGrid;
    auto const shared = std::make_shared<FormViewOptions const>(std::move(options));
    int const columns = std::max(1, shared->gridColumns);
    int const flatColumns = shared->flatGridColumns.value_or(columns == 2 ? 1 : columns);
    FormSession* const form = &session;
    std::vector<ui::Node> children;
    children.push_back(ui::text(ui::Text{.text = std::string{session.model().actionType()}, .role = ui::TextRole::Heading}));
    auto const groups = session.model().groups();
    for (std::size_t i = 0; i < groups.size();) {
        FieldGroupSpec const& group = detail::engine::elementAt(groups, i);
        if (group.implicit) {
            children.push_back(groupGrid(session, group, flatColumns, shared));
            ++i;
        } else if (group.kind == GroupKind::Tab) {
            std::size_t const first = i;
            std::vector<ui::Tab> tabs;
            while (i < groups.size() && detail::engine::elementAt(groups, i).kind == GroupKind::Tab &&
                   !detail::engine::elementAt(groups, i).implicit) {
                tabs.push_back(ui::Tab{.label = session.groupTitle(i),
                                       .node = groupGrid(session, detail::engine::elementAt(groups, i), columns, shared)});
                ++i;
            }
            reactive::Signal<std::size_t>* const selected = &session.tabSelection(first);
            children.push_back(ui::tabs(ui::Tabs{.tabs = std::move(tabs),
                                                 .selected = [selected] { return selected->get(); },
                                                 .onSelect = [selected](std::size_t index) { selected->set(index); }}));
        } else if (group.kind == GroupKind::Accordion) {
            reactive::Signal<bool>* const collapsed = &session.groupCollapsed(i);
            children.push_back(ui::panel(ui::Panel{.title = session.groupTitle(i),
                                                   .padding = 1,
                                                   .child = groupGrid(session, group, columns, shared),
                                                   .collapsible = true,
                                                   .collapsed = [collapsed] { return collapsed->get(); },
                                                   .onToggle = [collapsed](bool value) { collapsed->set(value); }}));
            ++i;
        } else {
            children.push_back(ui::panel(ui::Panel{.title = session.groupTitle(i), .padding = 1, .child = groupGrid(session, group, columns, shared)}));
            ++i;
        }
    }
    children.push_back(ui::text(ui::Text{.text = [form] { return detail::engine::statusText(*form); }, .role = ui::TextRole::Muted}));
    if (session.submitMode() == SubmitMode::Explicit) {
        children.push_back(ui::button(ui::Button{.label = shared->submitLabel,
                                                 .onClick = [form] { form->submit(); },
                                                 .common = {.enabled = [form] { return form->ready(); }}}));
    }
    children.push_back(ui::text(ui::Text{
        .text = [form] { return detail::engine::resultText(*form); },
        .role = [form] { return form->lastError() != nullptr ? ui::TextRole::Error : ui::TextRole::Success; },
        .common = {.visible = [form] { return form->lastError() != nullptr || form->lastReply().has_value(); }}}));
    return ui::column(ui::Column{.children = std::move(children), .gap = 1});
}

}  // namespace morph::forms
```

Register the headers and the test as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][view]"`
Expected: PASS, 13 test cases. Then once under ASan (Task 17's configure) for Review Focus 5:
`./build/clang-asan/tests/morph_tests "a row removed while its view is mounted*"` — expected PASS with no report.

Mutation check: in "a row removed while its view is mounted…", delete the `rows.removeRow(key);` line. Expected:
FAIL at `CHECK_FALSE(backend.exists(input))` — the case really removes a mounted row. Restore it. The lifetime
itself has ASan as its observer: the row is held by the ForEach slot (Part 2's per-key row signal) and by the panel
title binding, and a clean ASan run of this case is the evidence neither lets go too early.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/overrides.hpp include/morph/forms/engine/form_view.hpp \
        tests/forms_engine_view_support.hpp tests/test_forms_engine_view.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): formView, Overrides and FieldView — forms rendered through the view tree

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: Collections — `CollectionModel`, `CollectionSession`, `collectionView`

A view document (`v-*`, what `views::viewSchemaJson<V>()` emits) becomes a list `Query` on `v-query`, a
`Mutation` for every row and collection action that refetches the list on success, a confirm step for `confirm`
actions, and one editor `FormSession` for `v-rowAction` — reset and prefilled from the row's `bind` entries when a
row is opened, closed and the list refetched when its submission succeeds. Bodies are built from the row's JSON
values (`JsonExact.literal`'s job in QML), so an id past 2^53 is sent digit for digit; cells are formatted from the
exact `{num, den, dp}` with `decimalText`, never through a double.

**Files:**
- Create: `include/morph/forms/engine/collection.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/form_view.hpp`, add
  `include/morph/forms/engine/collection.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_collection.cpp` after `test_forms_engine_view.cpp`
- Test: `tests/test_forms_engine_collection.cpp`

**Interfaces:**
- Consumes: Part 1's `reactive::Query`, `Mutation`, `MutationOptions{invalidates}`; Part 2's `ui::table<RowT>`,
  `ui::TableColumn`, `ui::TableOptions`, `ui::dialog`, `ui::Dialog`; Tasks 2, 6, 12 (`formView`,
  `detail::engine::resultText`).
- Produces (`morph::forms`): `CollectionKind{Collection, MasterDetail}`, `CollectionActionScope{Row, Collection}`,
  `CollectionColumn{field, label, hidden, decimalPlaces, unit}`, `CollectionAction{action, label, scope, bind,
  confirm}`, `CollectionModel` (`fromSchema(std::string_view viewJson) -> std::expected<CollectionModel,
  SchemaError>`, `kind()`, `title()`, `query()`, `rowKey()`, `columns()`, `rowAction()`, `actions()`),
  `CollectionRow{key, json, cells}`, `PendingConfirm{action, rowKey}`, `SchemaLookup =
  std::function<std::optional<std::string>(std::string_view actionType)>`, `CollectionSession` (below),
  `CollectionViewOptions{editor, openLabel, confirmTitle, confirmMessage, confirmLabel, cancelLabel}`, and the
  contract's `collectionView(CollectionSession&, CollectionViewOptions = {}) -> ui::Node`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_collection.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/collection.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"
#include "forms_engine_view_support.hpp"

namespace {

using morph::forms::CollectionModel;
using morph::forms::CollectionSession;
using morph::test::formsengine::Call;
using morph::test::formsengine::Harness;
namespace ui = morph::ui;
namespace view = morph::test::formsview;

constexpr std::string_view kView = R"({"v-kind":"collection","v-title":"Rows","v-query":"ListRows","v-rowKey":"id",
  "v-columns":[{"field":"id","label":"ID","v-hidden":true},{"field":"name","label":"Name"},
    {"field":"amount","label":"Amount","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}],
  "v-rowAction":{"action":"EditRow","bind":{"id":"id"}},
  "v-actions":[{"action":"DeleteRow","label":"Delete","scope":"row","bind":{"id":"id"},"confirm":true},
    {"action":"CreateRow","label":"New","scope":"collection"}]})";

constexpr std::string_view kRows =
    R"({"rows":[{"id":1,"name":"First","amount":{"num":15,"den":10,"dp":1}},{"id":2,"name":"Second","amount":{"num":30,"den":10,"dp":1}}]})";

[[nodiscard]] std::optional<std::string> schemas(std::string_view action) {
    if (action == "EditRow") {
        return R"({"properties":{"id":{"type":"integer","x-order":0},"name":{"type":"string","x-order":1}},"required":["id","name"]})";
    }
    return std::nullopt;
}

/// A collection over the fake server: ListRows answers @p rows, everything else `{"ok":true}`.
struct CollectionHarness : Harness {
    explicit CollectionHarness(std::string rows = std::string{kRows}) {
        server.replyWith([rows = std::move(rows)](Call const& call) -> std::optional<std::string> {
            return call.action == "ListRows" ? rows : std::string{R"({"ok":true})"};
        });
    }

    [[nodiscard]] std::unique_ptr<CollectionSession> collection(std::string_view json = kView) {
        auto model = CollectionModel::fromSchema(json);
        REQUIRE(model.has_value());
        return std::make_unique<CollectionSession>(rt, *std::move(model), schemas, server.submitter(), server.fetcher());
    }

    [[nodiscard]] std::size_t count(std::string_view action) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(server.submits(), [&](Call const& call) { return call.action == action; }));
    }

    [[nodiscard]] std::optional<std::string> lastBodyOf(std::string_view action) const {
        for (auto iter = server.submits().rbegin(); iter != server.submits().rend(); ++iter) {
            if (iter->action == action) {
                return iter->body;
            }
        }
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("CollectionModel: the v-* document reads, and a malformed one is a SchemaError", "[forms-engine][collection]") {
    auto const model = CollectionModel::fromSchema(kView);
    REQUIRE(model.has_value());
    CHECK(model->kind() == morph::forms::CollectionKind::Collection);
    CHECK(model->title() == "Rows");
    CHECK(model->query() == "ListRows");
    CHECK(model->rowKey() == "id");
    REQUIRE(model->columns().size() == 3);
    CHECK(model->columns()[0].hidden);
    CHECK(model->columns()[2].decimalPlaces == 1U);
    CHECK(model->columns()[2].unit == "kg");
    REQUIRE(model->rowAction().has_value());
    CHECK(model->rowAction()->bind == std::vector<std::pair<std::string, std::string>>{{"id", "id"}});
    REQUIRE(model->actions().size() == 2);
    CHECK(model->actions()[0].confirm);
    CHECK(model->actions()[1].scope == morph::forms::CollectionActionScope::Collection);
    CHECK_FALSE(CollectionModel::fromSchema("[]").has_value());
    CHECK(CollectionModel::fromSchema(R"({"v-kind":"collection"})").error().path == "v-query");
    CHECK(CollectionModel::fromSchema(R"({"v-kind":"grid","v-query":"Q"})").error().path == "v-kind");
}

TEST_CASE("CollectionSession: the list loads on construction; cells are exact", "[forms-engine][collection]") {
    CollectionHarness harness;
    auto const list = harness.collection();
    harness.settle();
    CHECK(harness.count("ListRows") == 1);
    REQUIRE(list->rows().size() == 2);
    CHECK(list->rows()[0].key == "1");
    CHECK(list->rows()[0].cells == std::vector<std::string>{"First", "1.5 kg"});
    CHECK(list->rows()[1].cells == std::vector<std::string>{"Second", "3.0 kg"});
}

TEST_CASE("CollectionSession: opening a row prefills only its bound fields and fires nothing",
          "[forms-engine][collection]") {
    CollectionHarness harness;
    auto const list = harness.collection();
    harness.settle();
    REQUIRE(list->editor() != nullptr);
    list->open("1");
    harness.settle();
    CHECK(list->openRow() == "1");
    CHECK(list->editor()->field("id").text().peek() == "1");
    CHECK(list->editor()->field("name").text().peek().empty());
    CHECK(harness.count("EditRow") == 0);
    CHECK(harness.count("ListRows") == 1);
}

TEST_CASE("CollectionSession: an edit after opening fires, closes the editor and reloads the list",
          "[forms-engine][collection]") {
    CollectionHarness harness;
    auto const list = harness.collection();
    harness.settle();
    list->open("1");
    harness.settle();
    harness.type(*list->editor(), "name", "Renamed");
    CHECK(harness.lastBodyOf("EditRow") == R"({"id":1,"name":"Renamed"})");
    CHECK_FALSE(list->openRow().has_value());
    CHECK(harness.count("ListRows") == 2);
}

TEST_CASE("CollectionSession: a confirmed row action fires its bound body and reloads; a cancelled one fires nothing",
          "[forms-engine][collection]") {
    CollectionHarness harness;
    auto const list = harness.collection();
    harness.settle();
    list->fire(0, "2");
    harness.settle();
    REQUIRE(list->pendingConfirm().has_value());
    CHECK(list->pendingConfirm()->rowKey == "2");
    CHECK(harness.count("DeleteRow") == 0);
    list->cancelConfirm();
    harness.settle();
    CHECK_FALSE(list->pendingConfirm().has_value());
    CHECK(harness.count("DeleteRow") == 0);
    list->fire(0, "1");
    list->confirm();
    harness.settle();
    CHECK(harness.lastBodyOf("DeleteRow") == R"({"id":1})");
    CHECK_FALSE(list->pendingConfirm().has_value());
    CHECK(harness.count("ListRows") == 2);
}

TEST_CASE("CollectionSession: a collection action fires with an empty body", "[forms-engine][collection]") {
    CollectionHarness harness;
    auto const list = harness.collection();
    harness.settle();
    list->fire(1);
    harness.settle();
    CHECK(harness.lastBodyOf("CreateRow") == "{}");
}

TEST_CASE("CollectionSession: nothing typed into one row's editor carries over to the next opening",
          "[forms-engine][collection]") {
    CollectionHarness harness;
    harness.server.replyWith([](Call const& call) -> std::optional<std::string> {
        if (call.action == "ListRows") {
            return std::string{kRows};
        }
        return call.action == "EditRow" ? std::nullopt : std::optional<std::string>{R"({"ok":true})"};
    });
    auto const list = harness.collection();
    harness.settle();
    list->open("1");
    harness.settle();
    harness.type(*list->editor(), "name", "Typed into row one");
    CHECK(harness.lastBodyOf("EditRow") == R"({"id":1,"name":"Typed into row one"})");
    auto const edits = harness.count("EditRow");
    list->open("2");
    harness.settle();
    CHECK(list->editor()->field("id").text().peek() == "2");
    CHECK(list->editor()->field("name").text().peek().empty());
    CHECK(harness.count("EditRow") == edits);
    list->close();
    list->open("2");
    harness.settle();
    CHECK(list->editor()->field("name").text().peek().empty());
}

TEST_CASE("CollectionSession: ids beyond 2^53 stay distinct in keys, cells and bodies", "[forms-engine][collection]") {
    CollectionHarness harness{R"({"rows":[{"id":9007199254740993,"name":"Alpha"},{"id":9007199254740992,"name":"Beta"}]})"};
    auto const list = harness.collection(R"({"v-kind":"collection","v-title":"Rows","v-query":"ListRows","v-rowKey":"id",
      "v-columns":[{"field":"id","label":"ID"},{"field":"name","label":"Name"}],"v-rowAction":{"action":"EditRow","bind":{"id":"id"}},
      "v-actions":[{"action":"DeleteRow","label":"Delete","scope":"row","bind":{"id":"id"},"confirm":true}]})");
    harness.settle();
    REQUIRE(list->rows().size() == 2);
    CHECK(list->rows()[0].key == "9007199254740993");
    CHECK(list->rows()[1].key == "9007199254740992");
    CHECK(list->rows()[0].cells.front() == "9007199254740993");
    list->fire(0, "9007199254740992");
    list->confirm();
    harness.settle();
    CHECK(harness.lastBodyOf("DeleteRow") == R"({"id":9007199254740992})");
    list->fire(0, "9007199254740993");
    list->confirm();
    harness.settle();
    CHECK(harness.lastBodyOf("DeleteRow") == R"({"id":9007199254740993})");

    CollectionHarness small{R"({"rows":[{"id":7,"name":"Small"}]})"};
    auto const plain = small.collection(R"({"v-kind":"collection","v-query":"ListRows",
      "v-actions":[{"action":"DeleteRow","label":"Delete","scope":"row","bind":{"id":"id"}}]})");
    small.settle();
    plain->fire(0, "7");
    small.settle();
    CHECK(small.lastBodyOf("DeleteRow") == R"({"id":7})");
}

TEST_CASE("collectionView: a table of the visible columns, the actions, the editor and confirm dialogs",
          "[forms-engine][collection][view]") {
    CollectionHarness harness;
    auto const list = harness.collection();
    harness.settle();
    std::string const drawn = view::outline(morph::forms::collectionView(*list));
    CHECK(drawn.find("Text \"Rows\" (heading)") != std::string::npos);
    CHECK(drawn.find("Button \"New\"") != std::string::npos);
    CHECK(drawn.find("Table [Name|Amount|]") != std::string::npos);
    CHECK(drawn.find("Dialog \"EditRow\"") != std::string::npos);
    CHECK(drawn.find("Dialog \"Confirm\"") != std::string::npos);

    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::collectionView(*list)};
    harness.settle();
    CHECK(backend.find("Text", "text", "1.5 kg").has_value());
    auto const firstDelete = backend.find(view::kButton, "label", "Delete");
    REQUIRE(firstDelete.has_value());
    backend.click(*firstDelete);
    harness.settle();
    auto const yes = backend.find(view::kButton, "label", "Yes");
    REQUIRE(yes.has_value());
    backend.click(*yes);
    harness.settle();
    CHECK(harness.lastBodyOf("DeleteRow") == R"({"id":1})");
    auto const open = backend.find(view::kButton, "label", "Open");
    REQUIRE(open.has_value());
    backend.click(*open);
    harness.settle();
    CHECK(list->openRow() == "1");
}

TEST_CASE("collectionView: master-detail puts the editor beside the table", "[forms-engine][collection][view]") {
    CollectionHarness harness;
    std::string json{kView};
    json.replace(json.find("\"collection\""), std::string_view{"\"collection\""}.size(), "\"master-detail\"");
    auto const list = harness.collection(json);
    harness.settle();
    std::string const drawn = view::outline(morph::forms::collectionView(*list));
    CHECK(drawn.find("Dialog \"EditRow\"") == std::string::npos);
    CHECK(drawn.find("  Row\n    Table [Name|Amount|]\n    Column hidden\n") != std::string::npos);
    list->open("2");
    harness.settle();
    CHECK(view::outline(morph::forms::collectionView(*list)).find("    Column\n      Column\n        Text \"EditRow\" (heading)") !=
          std::string::npos);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/collection.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/collection.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/collection.hpp
/// @brief Collections: a `v-*` view document as a reactive list with row and collection actions
///        and a row editor, rendered as a table.
///
/// Specified in `docs/spec/forms/engine.md`, "Collections".

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../../reactive/control.hpp"
#include "../../reactive/signal.hpp"
#include "../../ui/view.hpp"
#include "detail/text.hpp"
#include "field_model.hpp"
#include "form_session.hpp"
#include "form_view.hpp"
#include "rules.hpp"

namespace morph::forms {

/// @brief How a collection presents its row editor.
enum class CollectionKind : std::uint8_t {
    Collection,   ///< `v-kind: collection` — the editor is a dialog.
    MasterDetail  ///< `v-kind: master-detail` — the editor sits beside the table.
};

/// @brief Where an action's button sits.
enum class CollectionActionScope : std::uint8_t {
    Row,        ///< On every row; the body is built from the row's `bind` entries.
    Collection  ///< Once, beside the title; the body is `{}`.
};

/// @brief One `v-columns` entry.
struct CollectionColumn {
    /// @brief The row member shown.
    std::string field{};
    /// @brief The header.
    std::string label{};
    /// @brief `v-hidden`: carried in the rows, not shown.
    bool hidden = false;
    /// @brief `x-decimalPlaces`: the places a `{num, den}` value is shown with when it carries no `dp`.
    std::optional<std::uint32_t> decimalPlaces{};
    /// @brief The unit appended to a `{num, den}` value (`ExtUnits.unitUnicode`, else `unitAscii`).
    std::string unit{};
};

/// @brief One `v-rowAction` or `v-actions` entry.
struct CollectionAction {
    /// @brief The action type executed.
    std::string action{};
    /// @brief The button label (the action type when the schema gives none).
    std::string label{};
    /// @brief Row or collection.
    CollectionActionScope scope = CollectionActionScope::Row;
    /// @brief `{actionField: rowField}` pairs, in document order.
    std::vector<std::pair<std::string, std::string>> bind{};
    /// @brief Whether the action waits for a confirmation.
    bool confirm = false;
};

/// @brief The immutable model of a `v-*` view document.
class CollectionModel {
public:
    /// @brief Reads a view document.
    /// @param viewJson The document, in the vocabulary `views::viewSchemaJson<V>()` emits.
    /// @return The model, or the error naming the malformed key.
    [[nodiscard]] static std::expected<CollectionModel, SchemaError> fromSchema(std::string_view viewJson) {
        using detail::engine::Json;
        auto const dom = detail::engine::parseJson(viewJson);
        if (!dom || !dom->is_object()) {
            return std::unexpected(SchemaError{"", "a view document must be a JSON object"});
        }
        CollectionModel model{};
        auto const* const kind = detail::engine::stringAt(*dom, "v-kind");
        if (kind != nullptr && *kind != "collection" && *kind != "master-detail") {
            return std::unexpected(SchemaError{"v-kind", "\"v-kind\" must be \"collection\" or \"master-detail\""});
        }
        model._kind = (kind != nullptr && *kind == "master-detail") ? CollectionKind::MasterDetail : CollectionKind::Collection;
        auto const* const query = detail::engine::stringAt(*dom, "v-query");
        if (query == nullptr) {
            return std::unexpected(SchemaError{"v-query", "a view document needs a \"v-query\""});
        }
        model._query = *query;
        auto const* const title = detail::engine::stringAt(*dom, "v-title");
        model._title = title != nullptr ? *title : model._query;
        auto const* const rowKey = detail::engine::stringAt(*dom, "v-rowKey");
        model._rowKey = rowKey != nullptr ? *rowKey : std::string{"id"};
        if (Json const* const columns = detail::engine::member(*dom, "v-columns"); columns != nullptr && columns->is_array()) {
            for (Json const& entry : columns->get_array()) {
                CollectionColumn column{};
                auto const* const field = detail::engine::stringAt(entry, "field");
                if (field == nullptr) {
                    return std::unexpected(SchemaError{"v-columns", "a column needs a \"field\""});
                }
                column.field = *field;
                auto const* const label = detail::engine::stringAt(entry, "label");
                column.label = label != nullptr ? *label : column.field;
                Json const* const hidden = detail::engine::member(entry, "v-hidden");
                column.hidden = hidden != nullptr && hidden->is_boolean() && hidden->get<bool>();
                if (auto const places = detail::engine::SchemaReader::unsignedOf(detail::engine::member(entry, "x-decimalPlaces"))) {
                    column.decimalPlaces = static_cast<std::uint32_t>(std::min<std::uint64_t>(*places, math::kMaxDecimalPlaces));
                }
                if (Json const* const units = detail::engine::member(entry, "ExtUnits")) {
                    auto const* const unicode = detail::engine::stringAt(*units, "unitUnicode");
                    auto const* const ascii = detail::engine::stringAt(*units, "unitAscii");
                    column.unit = unicode != nullptr && !unicode->empty() ? *unicode : (ascii != nullptr ? *ascii : std::string{});
                }
                model._columns.push_back(std::move(column));
            }
        }
        if (Json const* const rowAction = detail::engine::member(*dom, "v-rowAction")) {
            auto action = actionOf(*rowAction, "v-rowAction");
            if (!action) {
                return std::unexpected(action.error());
            }
            model._rowAction = *std::move(action);
        }
        if (Json const* const actions = detail::engine::member(*dom, "v-actions"); actions != nullptr && actions->is_array()) {
            for (Json const& entry : actions->get_array()) {
                auto action = actionOf(entry, "v-actions");
                if (!action) {
                    return std::unexpected(action.error());
                }
                model._actions.push_back(*std::move(action));
            }
        }
        return model;
    }

    /// @brief How the row editor is presented.
    /// @return The kind.
    [[nodiscard]] CollectionKind kind() const noexcept { return _kind; }

    /// @brief `v-title`, or the query's type.
    /// @return The title.
    [[nodiscard]] std::string_view title() const noexcept { return _title; }

    /// @brief `v-query`: the action whose result rows are listed.
    /// @return The action type.
    [[nodiscard]] std::string_view query() const noexcept { return _query; }

    /// @brief `v-rowKey`: the row member that identifies a row (`id` by default).
    /// @return The member name.
    [[nodiscard]] std::string_view rowKey() const noexcept { return _rowKey; }

    /// @brief `v-columns`.
    /// @return The columns.
    [[nodiscard]] std::span<CollectionColumn const> columns() const noexcept { return _columns; }

    /// @brief `v-rowAction`: the editor's action and its bind entries.
    /// @return The row action, or `std::nullopt`.
    [[nodiscard]] std::optional<CollectionAction> const& rowAction() const noexcept { return _rowAction; }

    /// @brief `v-actions`.
    /// @return The actions.
    [[nodiscard]] std::span<CollectionAction const> actions() const noexcept { return _actions; }

private:
    CollectionModel() = default;

    [[nodiscard]] static std::expected<CollectionAction, SchemaError> actionOf(detail::engine::Json const& node,
                                                                               std::string const& path) {
        auto const* const type = detail::engine::stringAt(node, "action");
        if (type == nullptr) {
            return std::unexpected(SchemaError{path, "an action needs an \"action\""});
        }
        CollectionAction action{};
        action.action = *type;
        auto const* const label = detail::engine::stringAt(node, "label");
        action.label = label != nullptr ? *label : action.action;
        auto const* const scope = detail::engine::stringAt(node, "scope");
        action.scope = (scope != nullptr && *scope == "collection") ? CollectionActionScope::Collection : CollectionActionScope::Row;
        auto const* const confirm = detail::engine::member(node, "confirm");
        action.confirm = confirm != nullptr && confirm->is_boolean() && confirm->get<bool>();
        if (auto const* const bind = detail::engine::member(node, "bind"); bind != nullptr && bind->is_object()) {
            for (auto const& [field, rowField] : bind->get_object()) {
                if (auto const* const text = rowField.get_if<std::string>()) {
                    action.bind.emplace_back(field, *text);
                }
            }
        }
        return action;
    }

    CollectionKind _kind = CollectionKind::Collection;
    std::string _title;
    std::string _query;
    std::string _rowKey;
    std::vector<CollectionColumn> _columns;
    std::optional<CollectionAction> _rowAction;
    std::vector<CollectionAction> _actions;
};

/// @brief One listed row.
struct CollectionRow {
    /// @brief The row key's exact text.
    std::string key{};
    /// @brief The row as JSON, every integer digit kept.
    std::string json{};
    /// @brief The visible columns' formatted cells.
    std::vector<std::string> cells{};
    /// @brief Member-wise equality.
    /// @param other The other row.
    /// @return `true` when key, JSON and cells match.
    bool operator==(CollectionRow const& other) const = default;
};

/// @brief A confirm-guarded action waiting for an answer.
struct PendingConfirm {
    /// @brief Index into `CollectionModel::actions()`.
    std::size_t action = 0;
    /// @brief The row it was fired on.
    std::string rowKey{};
    /// @brief Member-wise equality.
    /// @param other The other pending action.
    /// @return `true` when action and row match.
    bool operator==(PendingConfirm const& other) const = default;
};

/// @brief Finds an action's schema JSON by its type id (a registry, a map, a remote descriptor).
using SchemaLookup = std::function<std::optional<std::string>(std::string_view actionType)>;

namespace detail::engine {

/// @brief A cell's text: a `{num, den}` value exactly at its `dp` (else the column's places) with
///        the column's unit, anything else as `displayText`, absent as empty.
/// @param row    The row.
/// @param column The column.
/// @return The text.
[[nodiscard]] inline std::string cellText(Json const& row, CollectionColumn const& column) {
    Json const* const value = member(row, column.field);
    if (value == nullptr || value->is_null()) {
        return {};
    }
    if (value->is_object()) {
        auto const num = SchemaReader::signedOf(member(*value, "num"));
        auto const den = SchemaReader::signedOf(member(*value, "den"));
        if (num && den && *den != 0) {
            auto const places = SchemaReader::unsignedOf(member(*value, "dp"));
            std::uint32_t const decimals = places ? static_cast<std::uint32_t>(*places) : column.decimalPlaces.value_or(0);
            std::string text = decimalText(math::Rational{math::Numerator{*num}, math::Denominator{*den}, math::DecimalPlaces{0}}, decimals);
            return column.unit.empty() ? text : text + " " + column.unit;
        }
    }
    return displayText(*value);
}

}  // namespace detail::engine

/// @brief The reactive state of one collection: the list, the actions, the confirm step and the
///        row editor.
///
/// Non-copyable and non-movable. Destroy a mounted view of it before the session.
class CollectionSession {
public:
    /// @param runtime The runtime. Borrowed.
    /// @param model   The view model.
    /// @param schemas Finds the row action's schema; an unknown or malformed one means no editor.
    /// @param submit  Executes the query and every action.
    /// @param choices Fetches the editor's Choice options.
    /// @param options The editor's session options.
    CollectionSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, CollectionModel model, SchemaLookup const& schemas,
                      Submitter submit, ChoiceFetcher choices, FormSessionOptions options = {})
        : _rt{&runtime},
          _model{std::move(model)},
          _submit{std::move(submit)},
          _openRow{runtime, std::nullopt},
          _pending{runtime, std::nullopt},
          _list{runtime, [this](std::string const& body) { return _submit(_model.query(), body); },
                [] { return std::optional<std::string>{"{}"}; }},
          _rows{runtime, [this] { return parseRows(); }},
          _actions{runtime, [this](Call call) { return _submit(call.action, std::move(call.body)); },
                   reactive::MutationOptions{.invalidates = {&_list}}} {
        if (_model.rowAction().has_value() && schemas) {
            if (auto const schema = schemas(_model.rowAction()->action)) {
                if (auto editorModel = FormModel::fromSchema(_model.rowAction()->action, *schema)) {
                    _editor = std::make_unique<FormSession>(runtime, *std::move(editorModel), _submit, std::move(choices),
                                                            std::move(options));
                    _editorWatch = std::make_unique<reactive::Effect>(runtime, [this] { watchEditor(); });
                }
            }
        }
    }

    ~CollectionSession() = default;
    CollectionSession(CollectionSession const&) = delete;
    CollectionSession& operator=(CollectionSession const&) = delete;
    CollectionSession(CollectionSession&&) = delete;
    CollectionSession& operator=(CollectionSession&&) = delete;

    /// @brief The view model.
    /// @return The model.
    [[nodiscard]] CollectionModel const& model() const noexcept { return _model; }

    /// @brief The listed rows, from the last successful query. Tracked.
    /// @return The rows.
    [[nodiscard]] std::vector<CollectionRow> const& rows() const { return _rows.get(); }

    /// @brief Whether the list is loading. Tracked.
    /// @return `true` while the query is in flight.
    [[nodiscard]] bool loading() const { return _list.pending(); }

    /// @brief The list's last failure. Tracked.
    /// @return The exception, or null.
    [[nodiscard]] std::exception_ptr listError() const { return _list.error(); }

    /// @brief Reloads the list.
    void refresh() { _list.refetch(); }

    /// @brief Fires `model().actions()[action]` — on @p rowKey for a row action; a confirm-guarded
    ///        one waits in `pendingConfirm()`.
    /// @param action Index into `model().actions()`.
    /// @param rowKey The row, for a row action.
    void fire(std::size_t action, std::string_view rowKey = {}) {
        CollectionAction const& entry = detail::engine::elementAt(_model.actions(), action);
        if (entry.confirm) {
            _pending.set(PendingConfirm{action, std::string{rowKey}});
            return;
        }
        run(entry, rowKey);
    }

    /// @brief The action waiting for confirmation. Tracked.
    /// @return It, or `std::nullopt`.
    [[nodiscard]] std::optional<PendingConfirm> const& pendingConfirm() const { return _pending.get(); }

    /// @brief Runs the pending action.
    void confirm() {
        std::optional<PendingConfirm> const pending = _pending.peek();
        _pending.set(std::nullopt);
        if (pending.has_value()) {
            run(detail::engine::elementAt(_model.actions(), pending->action), pending->rowKey);
        }
    }

    /// @brief Drops the pending action.
    void cancelConfirm() { _pending.set(std::nullopt); }

    /// @brief Opens the editor on a row: reset, then prefilled from the row action's bind entries.
    ///        Programmatic: nothing is submitted.
    /// @param rowKey The row.
    void open(std::string_view rowKey) {
        if (!_editor) {
            return;
        }
        std::string const body = bindBody(*_model.rowAction(), rowKey);
        _rt->batch([&] {
            _openRow.set(std::string{rowKey});
            _editor->reset();
            _editor->prefill(body);
        });
    }

    /// @brief Closes the editor.
    void close() { _openRow.set(std::nullopt); }

    /// @brief The row the editor is open on. Tracked.
    /// @return Its key, or `std::nullopt`.
    [[nodiscard]] std::optional<std::string> const& openRow() const { return _openRow.get(); }

    /// @brief The row editor.
    /// @return The editor, or null when the view has no row action or its schema is unknown.
    [[nodiscard]] FormSession* editor() const noexcept { return _editor.get(); }

    /// @brief Whether an action is in flight. Tracked.
    /// @return `true` while one is.
    [[nodiscard]] bool actionPending() const { return _actions.pending(); }

    /// @brief The last action failure. Tracked.
    /// @return The exception, or null.
    [[nodiscard]] std::exception_ptr actionError() const { return _actions.error(); }

private:
    struct Call {
        std::string action;
        std::string body;
    };

    [[nodiscard]] std::vector<CollectionRow> parseRows() const {
        using detail::engine::Json;
        std::vector<CollectionRow> rows;
        std::optional<std::string> const& reply = _list.value();
        auto const dom = reply.has_value() ? detail::engine::parseJson(*reply) : std::nullopt;
        if (!dom) {
            return rows;
        }
        Json const* list = dom->is_array() ? &*dom : nullptr;
        if (list == nullptr && dom->is_object()) {
            for (auto const& [ignored, candidate] : dom->get_object()) {
                if (candidate.is_array()) {
                    list = &candidate;
                    break;
                }
            }
        }
        if (list == nullptr) {
            return rows;
        }
        for (Json const& row : list->get_array()) {
            CollectionRow entry{};
            Json const* const key = detail::engine::member(row, _model.rowKey());
            entry.key = key == nullptr ? std::to_string(rows.size()) : detail::engine::displayText(*key);
            entry.json = detail::engine::toJson(row);
            for (CollectionColumn const& column : _model.columns()) {
                if (!column.hidden) {
                    entry.cells.push_back(detail::engine::cellText(row, column));
                }
            }
            rows.push_back(std::move(entry));
        }
        return rows;
    }

    [[nodiscard]] std::string bindBody(CollectionAction const& action, std::string_view rowKey) const {
        std::vector<CollectionRow> const& rows = _rows.peek();
        auto const found = std::ranges::find(rows, rowKey, &CollectionRow::key);
        auto const row = found == rows.end() ? std::nullopt : detail::engine::parseJson(found->json);
        std::string body = "{";
        bool first = true;
        for (auto const& [field, rowField] : action.bind) {
            detail::engine::Json const* const value = row ? detail::engine::member(*row, rowField) : nullptr;
            body += (first ? "" : ",") + detail::engine::quote(field) + ":" +
                    (value == nullptr ? std::string{"null"} : detail::engine::toJson(*value));
            first = false;
        }
        return body + "}";
    }

    void run(CollectionAction const& action, std::string_view rowKey) {
        std::string body = action.scope == CollectionActionScope::Collection ? std::string{"{}"} : bindBody(action, rowKey);
        _actions.run(Call{action.action, std::move(body)});
    }

    void watchEditor() {
        bool const pending = _editor->pending();
        bool const succeeded = _editorWasPending && !pending && _editor->lastError() == nullptr &&
                               _editor->lastReply().has_value();
        _editorWasPending = pending;
        if (succeeded) {
            _rt->batch([&] {
                _openRow.set(std::nullopt);
                _list.refetch();
            });
        }
    }

    reactive::Runtime* _rt;
    CollectionModel _model;
    Submitter _submit;
    reactive::Signal<std::optional<std::string>> _openRow;
    reactive::Signal<std::optional<PendingConfirm>> _pending;
    reactive::Query<std::string, std::string> _list;
    reactive::Computed<std::vector<CollectionRow>> _rows;
    reactive::Mutation<Call, std::string> _actions;
    std::unique_ptr<FormSession> _editor;
    bool _editorWasPending = false;
    std::unique_ptr<reactive::Effect> _editorWatch;
};

/// @brief What `collectionView` draws around the table.
struct CollectionViewOptions {
    /// @brief The editor form's view options.
    FormViewOptions editor{};
    /// @brief The row button that opens the editor.
    std::string openLabel = "Open";
    /// @brief The confirm dialog's title.
    std::string confirmTitle = "Confirm";
    /// @brief The confirm dialog's question.
    std::string confirmMessage = "Are you sure?";
    /// @brief The confirm dialog's accepting button.
    std::string confirmLabel = "Yes";
    /// @brief The confirm dialog's declining button.
    std::string cancelLabel = "No";
};

/// @brief Renders a collection: the title with the collection actions and a busy marker, a table
///        of the visible columns with each row's Open and row-action buttons, the last action
///        error, the editor (a dialog, or beside the table for master-detail) and the confirm dialog.
/// @param session The collection session; mount the result after it and destroy the mount before it.
/// @param options Labels and the editor's view options.
/// @return The view.
[[nodiscard]] inline ui::Node collectionView(CollectionSession& session, CollectionViewOptions options = {}) {
    CollectionSession* const list = &session;
    CollectionModel const& model = session.model();
    std::vector<ui::Node> header{ui::text(ui::Text{.text = std::string{model.title()}, .role = ui::TextRole::Heading}),
                                 ui::busy(ui::Busy{.active = [list] { return list->loading(); }, .label = std::string{}})};
    for (std::size_t i = 0; i < model.actions().size(); ++i) {
        CollectionAction const& action = detail::engine::elementAt(model.actions(), i);
        if (action.scope == CollectionActionScope::Collection) {
            header.push_back(ui::button(ui::Button{.label = action.label, .onClick = [list, i] { list->fire(i); }}));
        }
    }
    std::vector<ui::TableColumn> columns;
    for (CollectionColumn const& column : model.columns()) {
        if (!column.hidden) {
            columns.push_back(ui::TableColumn{.label = column.label});
        }
    }
    bool const rowButtons = session.editor() != nullptr ||
                            std::ranges::any_of(model.actions(), [](auto const& action) { return action.scope == CollectionActionScope::Row; });
    if (rowButtons) {
        columns.push_back(ui::TableColumn{.label = std::string{}});
    }
    std::size_t const visible = rowButtons ? columns.size() - 1 : columns.size();
    std::string const openLabel = options.openLabel;
    ui::Node table = ui::table<CollectionRow>(
        std::move(columns), [list] { return list->rows(); }, [](CollectionRow const& row) { return ui::Key{row.key}; },
        [list, visible, rowButtons, openLabel](reactive::Signal<CollectionRow> const& slot) {
            reactive::Signal<CollectionRow> const* const row = &slot;
            std::vector<ui::Node> cells;
            for (std::size_t i = 0; i < visible; ++i) {
                cells.push_back(ui::text(ui::Text{.text = [row, i] { return row->get().cells.at(i); }}));
            }
            if (rowButtons) {
                std::vector<ui::Node> buttons;
                if (list->editor() != nullptr) {
                    buttons.push_back(ui::button(ui::Button{.label = openLabel, .onClick = [list, row] { list->open(row->peek().key); }}));
                }
                for (std::size_t actionIndex = 0; actionIndex < list->model().actions().size(); ++actionIndex) {
                    CollectionAction const& action = detail::engine::elementAt(list->model().actions(), actionIndex);
                    if (action.scope == CollectionActionScope::Row) {
                        buttons.push_back(ui::button(ui::Button{.label = action.label,
                                                                .onClick = [list, row, actionIndex] { list->fire(actionIndex, row->peek().key); }}));
                    }
                }
                cells.push_back(ui::row(ui::Row{.children = std::move(buttons), .gap = 1}));
            }
            return cells;
        },
        ui::TableOptions{.onActivate = [list](ui::Key key) {
            if (auto const* const text = std::get_if<std::string>(&key)) {
                list->open(*text);
            }
        }});
    std::vector<ui::Node> children{ui::row(ui::Row{.children = std::move(header), .gap = 1})};
    if (session.editor() != nullptr && model.kind() == CollectionKind::MasterDetail) {
        children.push_back(ui::row(ui::Row{
            .children = {std::move(table),
                         ui::column(ui::Column{.children = {formView(*session.editor(), options.editor)},
                                               .common = {.visible = [list] { return list->openRow().has_value(); }}})},
            .gap = 2}));
    } else {
        children.push_back(std::move(table));
    }
    children.push_back(ui::text(ui::Text{.text = [list] { return reactive::errorMessage(list->actionError()); },
                                         .role = ui::TextRole::Error,
                                         .common = {.visible = [list] { return list->actionError() != nullptr; }}}));
    if (session.editor() != nullptr && model.kind() == CollectionKind::Collection) {
        children.push_back(ui::dialog(ui::Dialog{.open = [list] { return list->openRow().has_value(); },
                                                 .title = model.rowAction()->action,
                                                 .child = formView(*session.editor(), options.editor),
                                                 .onDismiss = [list] { list->close(); }}));
    }
    children.push_back(ui::dialog(ui::Dialog{
        .open = [list] { return list->pendingConfirm().has_value(); },
        .title = options.confirmTitle,
        .child = ui::column(ui::Column{
            .children = {ui::text(ui::Text{.text = options.confirmMessage}),
                         ui::row(ui::Row{.children = {ui::button(ui::Button{.label = options.confirmLabel, .onClick = [list] { list->confirm(); }}),
                                                      ui::button(ui::Button{.label = options.cancelLabel, .onClick = [list] { list->cancelConfirm(); }})},
                                         .gap = 1})},
            .gap = 1}),
        .onDismiss = [list] { list->cancelConfirm(); }}));
    return ui::column(ui::Column{.children = std::move(children), .gap = 1});
}

}  // namespace morph::forms
```

Register the header and the test as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][collection]"`
Expected: PASS, 10 test cases.

Mutation check: remove `reactive::MutationOptions{.invalidates = {&_list}}` from `_actions`' construction. Expected:
FAIL in "a confirmed row action fires its bound body and reloads…" (`harness.count("ListRows") == 2` is 1). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/collection.hpp tests/test_forms_engine_collection.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): collections — a v-* view as a reactive list, actions and an editor

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 14: Wizards — `WizardModel`, `WizardSession`, `wizardView`

A wizard document (`w-*`, `flows::wizardSchemaJson<W>()`) becomes one `FormSession` per step, all kept alive, so
Back and Next lose nothing. A step is **done** when its last submission succeeded. Every successful submission
records resolved values the way `FlowSession` does (`include/morph/forms/flows.hpp`, `captureResult`): each member
of the submitted body, then each member of the reply — the reply winning on a name collision — under
`<ActionType>.<member>`, as JSON. Entering a step by Next applies its `prefill` (`{field: "Action.member"}`) from
those values with `FormSession::assign`, as JSON values (the QML wizard re-typed them as text, which put quotes in a
string); a step the prefill leaves ready submits in automatic mode, as entering it in the QML wizard did.

**Files:**
- Create: `include/morph/forms/engine/wizard.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/collection.hpp`, add
  `include/morph/forms/engine/wizard.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_wizard.cpp` after `test_forms_engine_collection.cpp`
- Test: `tests/test_forms_engine_wizard.cpp`

**Interfaces:**
- Consumes: Tasks 6, 12, 13 (`SchemaLookup`); `forms::i18n::wizardTitleKey`, `wizardStepTitleKey`
  (`include/morph/forms/i18n.hpp`); `ui::switchOf`, `ui::Switch`, `ui::SwitchCase`.
- Produces (`morph::forms`): `WizardStepSpec{action, title, prefill}`; `WizardModel` (`fromSchema(std::string_view
  wizardId, std::string_view wizardJson) -> std::expected<WizardModel, SchemaError>`, `id()`, `title()`,
  `steps()`); `WizardSession` (`WizardSession(reactive::Runtime&, WizardModel, SchemaLookup const&, Submitter,
  ChoiceFetcher, FormSessionOptions = {})`, `model()`, `current()`, `stepCount()`, `step(std::size_t) ->
  FormSession&`, `stepDone(std::size_t)`, `canBack()`, `canNext()`, `onLastStep()`, `next()`, `back()`,
  `resolved(std::string_view path) -> std::optional<std::string>`, `title()`, `stepTitle(std::size_t)`); the
  contract's `wizardView(WizardSession&) -> ui::Node`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_wizard.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/wizard.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "forms_engine_support.hpp"
#include "forms_engine_view_support.hpp"

namespace {

using morph::forms::WizardModel;
using morph::forms::WizardSession;
using morph::test::formsengine::Call;
using morph::test::formsengine::Harness;
namespace ui = morph::ui;
namespace view = morph::test::formsview;

constexpr std::string_view kWizard = R"({"w-title":"Test flow","w-steps":[{"action":"WizStepOne","title":"One"},
  {"action":"WizStepTwo","title":"Two","prefill":{"refId":"WizStepOne.id","note":"WizStepOne.label"}}]})";

[[nodiscard]] std::optional<std::string> stepSchemas(std::string_view action) {
    if (action == "WizStepOne") {
        return R"({"properties":{"label":{"type":"string","x-order":0}},"required":["label"]})";
    }
    if (action == "WizStepTwo") {
        return R"({"properties":{"refId":{"type":"integer","x-order":0},"note":{"type":["string","null"],"x-order":1}},"required":["refId"]})";
    }
    return std::nullopt;
}

struct WizardHarness : Harness {
    WizardHarness() {
        server.replyWith([](Call const& call) -> std::optional<std::string> {
            if (call.action == "WizStepOne") {
                return R"({"id":1,"label":"x"})";
            }
            return R"({"summary":"ok"})";
        });
    }

    [[nodiscard]] std::unique_ptr<WizardSession> wizard() {
        auto model = WizardModel::fromSchema("TestWizard", kWizard);
        REQUIRE(model.has_value());
        return std::make_unique<WizardSession>(rt, *std::move(model), stepSchemas, server.submitter(), server.fetcher());
    }
};

}  // namespace

TEST_CASE("WizardModel: the w-* document reads, and a malformed one is a SchemaError", "[forms-engine][wizard]") {
    auto const model = WizardModel::fromSchema("TestWizard", kWizard);
    REQUIRE(model.has_value());
    CHECK(model->id() == "TestWizard");
    CHECK(model->title() == "Test flow");
    REQUIRE(model->steps().size() == 2);
    CHECK(model->steps()[1].action == "WizStepTwo");
    CHECK(model->steps()[1].prefill ==
          std::vector<std::pair<std::string, std::string>>{{"refId", "WizStepOne.id"}, {"note", "WizStepOne.label"}});
    CHECK(WizardModel::fromSchema("W", R"({"w-title":"x"})").error().path == "w-steps");
    CHECK(WizardModel::fromSchema("W", R"({"w-steps":[{"title":"no action"}]})").error().path == "w-steps[0]");
}

TEST_CASE("WizardSession: Next waits for the step's successful submission; Back always returns",
          "[forms-engine][wizard]") {
    WizardHarness harness;
    auto const wizard = harness.wizard();
    CHECK(wizard->current() == 0);
    CHECK(wizard->stepCount() == 2);
    CHECK_FALSE(wizard->canBack());
    CHECK_FALSE(wizard->canNext());
    wizard->next();
    CHECK(wizard->current() == 0);
    harness.type(wizard->step(0), "label", "sample");
    CHECK(wizard->stepDone(0));
    CHECK(wizard->canNext());
    wizard->next();
    harness.settle();
    CHECK(wizard->current() == 1);
    CHECK(wizard->onLastStep());
    CHECK(wizard->canBack());
    CHECK_FALSE(wizard->canNext());
    wizard->back();
    CHECK(wizard->current() == 0);
}

TEST_CASE("WizardSession: a failed submission leaves the step not done", "[forms-engine][wizard]") {
    WizardHarness harness;
    harness.server.replyWith([](Call const&) { return std::nullopt; });
    auto const wizard = harness.wizard();
    harness.type(wizard->step(0), "label", "sample");
    harness.server.rejectSubmit(0, "refused");
    harness.settle();
    CHECK_FALSE(wizard->stepDone(0));
    CHECK_FALSE(wizard->canNext());
}

TEST_CASE("WizardSession: entering a step applies its prefill as JSON values, the reply winning",
          "[forms-engine][wizard]") {
    WizardHarness harness;
    auto const wizard = harness.wizard();
    harness.type(wizard->step(0), "label", "sample");
    CHECK(wizard->resolved("WizStepOne.id") == "1");
    CHECK(wizard->resolved("WizStepOne.label") == R"("x")");  // the reply's, not the draft's
    CHECK_FALSE(wizard->resolved("WizStepOne.nope").has_value());
    wizard->next();
    harness.settle();
    CHECK(wizard->step(1).field("refId").text().peek() == "1");
    CHECK(wizard->step(1).field("note").text().peek() == "x");  // a string arrives unquoted
    REQUIRE(harness.server.submits().size() == 2);                    // the filled step submits on entry
    CHECK(harness.server.submits()[1].action == "WizStepTwo");
    CHECK(harness.server.submits()[1].body == R"({"refId":1,"note":"x"})");
    CHECK(wizard->stepDone(1));
}

TEST_CASE("WizardSession: an unknown step schema is refused at construction", "[forms-engine][wizard]") {
    Harness harness;
    auto model = WizardModel::fromSchema("W", R"({"w-steps":[{"action":"Missing","title":"?"}]})");
    REQUIRE(model.has_value());
    CHECK_THROWS_AS(WizardSession(harness.rt, *std::move(model), stepSchemas, harness.server.submitter(), harness.server.fetcher()),
                    std::invalid_argument);
}

TEST_CASE("wizardView: the title with the position, the step's form, and Back/Next bound to the session",
          "[forms-engine][wizard][view]") {
    WizardHarness harness;
    auto const wizard = harness.wizard();
    std::string const drawn = view::outline(morph::forms::wizardView(*wizard));
    CHECK(drawn.find("  Text \"Test flow  (1 / 2)\" (heading)\n  Text \"One\" (muted)\n  Switch\n") != std::string::npos);
    CHECK(drawn.find("    Button \"Back\" disabled\n    Button \"Next\" disabled\n") != std::string::npos);
    CHECK(drawn.find("    Text \"Last step — fill it in to finish\" (muted) hidden\n") != std::string::npos);

    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::wizardView(*wizard)};
    harness.settle();
    backend.edit(backend.all(view::kTextInput).front(), "sample");
    harness.settle();
    auto const next = backend.find(view::kButton, "label", "Next");
    REQUIRE(next.has_value());
    CHECK(backend.prop(*next, "enabled") == "true");
    backend.click(*next);
    harness.settle();
    CHECK(wizard->current() == 1);
    CHECK(backend.find("Text", "text", "Test flow  (2 / 2)").has_value());
    CHECK(backend.prop(backend.all(view::kTextInput).front(), "text") == "1");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/wizard.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `include/morph/forms/engine/wizard.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/wizard.hpp
/// @brief Wizards: a `w-*` document as one `FormSession` per step, sequenced by successful
///        submissions, with prefill from earlier steps' resolved values.
///
/// Specified in `docs/spec/forms/engine.md`, "Wizards".

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../../core/callback_scope.hpp"
#include "../../core/completion.hpp"
#include "../../reactive/signal.hpp"
#include "../../render/i18n.hpp"
#include "../../ui/view.hpp"
#include "../i18n.hpp"
#include "collection.hpp"
#include "detail/text.hpp"
#include "form_session.hpp"
#include "form_view.hpp"

namespace morph::forms {

/// @brief One `w-steps` entry.
struct WizardStepSpec {
    /// @brief The step's action type.
    std::string action{};
    /// @brief The step's title.
    std::string title{};
    /// @brief `{field: "Action.member"}`: what entering the step fills in, in document order.
    std::vector<std::pair<std::string, std::string>> prefill{};
};

/// @brief The immutable model of a `w-*` wizard document.
class WizardModel {
public:
    /// @brief Reads a wizard document.
    /// @param wizardId   The wizard's registered id (`BRIDGE_REGISTER_WIZARD`), the i18n key stem.
    /// @param wizardJson The document, in the vocabulary `flows::wizardSchemaJson<W>()` emits.
    /// @return The model, or the error naming the malformed key.
    [[nodiscard]] static std::expected<WizardModel, SchemaError> fromSchema(std::string_view wizardId,
                                                                            std::string_view wizardJson) {
        using detail::engine::Json;
        auto const dom = detail::engine::parseJson(wizardJson);
        if (!dom || !dom->is_object()) {
            return std::unexpected(SchemaError{"", "a wizard document must be a JSON object"});
        }
        WizardModel model{};
        model._id = wizardId;
        auto const* const title = detail::engine::stringAt(*dom, "w-title");
        model._title = title != nullptr ? *title : std::string{wizardId};
        Json const* const steps = detail::engine::member(*dom, "w-steps");
        if (steps == nullptr || !steps->is_array()) {
            return std::unexpected(SchemaError{"w-steps", "a wizard document needs a \"w-steps\" array"});
        }
        std::size_t index = 0;
        for (Json const& entry : steps->get_array()) {
            auto const* const action = detail::engine::stringAt(entry, "action");
            if (action == nullptr) {
                return std::unexpected(SchemaError{"w-steps[" + std::to_string(index) + "]", "a step needs an \"action\""});
            }
            WizardStepSpec step{};
            step.action = *action;
            auto const* const stepTitle = detail::engine::stringAt(entry, "title");
            step.title = stepTitle != nullptr ? *stepTitle : step.action;
            if (Json const* const prefill = detail::engine::member(entry, "prefill"); prefill != nullptr && prefill->is_object()) {
                for (auto const& [field, path] : prefill->get_object()) {
                    if (auto const* const text = path.get_if<std::string>()) {
                        step.prefill.emplace_back(field, *text);
                    }
                }
            }
            model._steps.push_back(std::move(step));
            ++index;
        }
        return model;
    }

    /// @brief The wizard's id.
    /// @return The id passed to `fromSchema`.
    [[nodiscard]] std::string_view id() const noexcept { return _id; }

    /// @brief `w-title`, or the id.
    /// @return The title.
    [[nodiscard]] std::string_view title() const noexcept { return _title; }

    /// @brief The steps, in order.
    /// @return The steps.
    [[nodiscard]] std::span<WizardStepSpec const> steps() const noexcept { return _steps; }

private:
    WizardModel() = default;

    std::string _id;
    std::string _title;
    std::vector<WizardStepSpec> _steps;
};

/// @brief The reactive state of one wizard: its steps' sessions, the position, what each step resolved.
///
/// Non-copyable and non-movable. Destroy a mounted view of it before the session.
class WizardSession {
public:
    /// @param runtime The runtime. Borrowed.
    /// @param model   The wizard model.
    /// @param schemas Finds each step's schema.
    /// @param submit  Executes every step's action.
    /// @param choices Fetches the steps' Choice options.
    /// @param options Every step's session options.
    /// @throws std::invalid_argument when a step's schema is unknown or malformed.
    WizardSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, WizardModel model, SchemaLookup const& schemas,
                  Submitter submit, ChoiceFetcher choices, FormSessionOptions options = {})
        : _rt{&runtime}, _model{std::move(model)}, _submit{std::move(submit)}, _current{runtime, std::size_t{0}} {
        for (std::size_t i = 0; i < _model.steps().size(); ++i) {
            WizardStepSpec const& spec = detail::engine::elementAt(_model.steps(), i);
            auto const schema = schemas ? schemas(spec.action) : std::nullopt;
            auto stepModel = schema ? FormModel::fromSchema(spec.action, *schema)
                                    : std::expected<FormModel, SchemaError>{std::unexpected(SchemaError{"", "unknown"})};
            if (!stepModel) {
                throw std::invalid_argument{"WizardSession: no usable schema for step '" + spec.action + "'"};
            }
            _done.push_back(std::make_unique<reactive::Signal<bool>>(runtime, false));
            _steps.push_back(std::make_unique<FormSession>(
                runtime, *std::move(stepModel),
                [this, i](std::string_view action, std::string body) { return relay(i, action, std::move(body)); },
                choices, options));
        }
    }

    ~WizardSession() = default;
    WizardSession(WizardSession const&) = delete;
    WizardSession& operator=(WizardSession const&) = delete;
    WizardSession(WizardSession&&) = delete;
    WizardSession& operator=(WizardSession&&) = delete;

    /// @brief The wizard model.
    /// @return The model.
    [[nodiscard]] WizardModel const& model() const noexcept { return _model; }

    /// @brief The current step. Tracked.
    /// @return Its index.
    [[nodiscard]] std::size_t current() const { return _current.get(); }

    /// @brief How many steps there are.
    /// @return The count.
    [[nodiscard]] std::size_t stepCount() const noexcept { return _steps.size(); }

    /// @brief A step's form session.
    /// @param index The step.
    /// @return Its session.
    [[nodiscard]] FormSession& step(std::size_t index) const { return *_steps.at(index); }

    /// @brief Whether a step's last submission succeeded. Tracked.
    /// @param index The step.
    /// @return `true` when done.
    [[nodiscard]] bool stepDone(std::size_t index) const { return _done.at(index)->get(); }

    /// @brief Whether Back is possible. Tracked.
    /// @return `true` past the first step.
    [[nodiscard]] bool canBack() const { return current() > 0; }

    /// @brief Whether Next is possible. Tracked.
    /// @return `true` when the current step is done and is not the last.
    [[nodiscard]] bool canNext() const { return current() + 1 < _steps.size() && stepDone(current()); }

    /// @brief Whether the current step is the last. Tracked.
    /// @return `true` on the last step.
    [[nodiscard]] bool onLastStep() const { return current() + 1 >= _steps.size(); }

    /// @brief Advances when `canNext()`, applying the next step's prefill.
    void next() {
        if (!_rt->untracked([&] { return canNext(); })) {
            return;
        }
        std::size_t const target = _current.peek() + 1;
        _current.set(target);
        FormSession& session = *_steps.at(target);
        for (auto const& [field, path] : detail::engine::elementAt(_model.steps(), target).prefill) {
            if (auto const value = resolved(path)) {
                session.assign(field, *value);
            }
        }
        if (session.submitMode() == SubmitMode::Automatic && _rt->untracked([&] { return session.ready(); })) {
            session.submit();
        }
    }

    /// @brief Returns to the previous step, its values as they were.
    void back() {
        if (_current.peek() > 0) {
            _current.set(_current.peek() - 1);
        }
    }

    /// @brief A value an earlier submission resolved.
    /// @param path `<ActionType>.<member>`.
    /// @return The member's JSON, or `std::nullopt`.
    [[nodiscard]] std::optional<std::string> resolved(std::string_view path) const {
        auto const found = _resolved.find(path);
        return found == _resolved.end() ? std::nullopt : std::optional<std::string>{found->second};
    }

    /// @brief The wizard's title, resolved through the translations (`<id>.title`).
    /// @return The title.
    [[nodiscard]] std::string title() const {
        auto const& options = _steps.empty() ? FormSessionOptions{} : _steps.front()->options();
        return render::resolveText(options.translations, options.bcp47, std::nullopt, i18n::wizardTitleKey(_model.id()),
                                   _model.title());
    }

    /// @brief A step's title, resolved through the translations (`<id>.step.<index>.title`).
    /// @param index The step.
    /// @return The title.
    [[nodiscard]] std::string stepTitle(std::size_t index) const {
        auto const& options = _steps.at(index)->options();
        return render::resolveText(options.translations, options.bcp47, std::nullopt,
                                   i18n::wizardStepTitleKey(_model.id(), index),
                                   detail::engine::elementAt(_model.steps(), index).title);
    }

private:
    [[nodiscard]] async::Completion<std::string> relay(std::size_t index, std::string_view action, std::string body) {
        auto [completion, promise] = async::Completion<std::string>::makeSettleable(&_rt->owner());
        auto settle = std::make_shared<async::Completion<std::string>::Promise>(std::move(promise));
        std::string const submitted = body;
        std::string const type{action};
        _submit(action, std::move(body))
            .then(_callbacks,
                  [this, index, settle, submitted, type](std::string const& reply) {
                      _rt->batch([&] {
                          record(type, submitted);
                          record(type, reply);
                          _done.at(index)->set(true);
                      });
                      settle->resolve(reply);
                  })
            .onError(_callbacks, [this, index, settle](std::exception_ptr error) {
                _done.at(index)->set(false);
                settle->reject(std::move(error));
            });
        return std::move(completion);
    }

    void record(std::string const& action, std::string const& json) {
        auto const dom = detail::engine::parseJson(json);
        if (!dom || !dom->is_object()) {
            return;
        }
        for (auto const& [name, value] : dom->get_object()) {
            _resolved.insert_or_assign(action + "." + name, detail::engine::toJson(value));
        }
    }

    reactive::Runtime* _rt;
    WizardModel _model;
    Submitter _submit;
    reactive::Signal<std::size_t> _current;
    std::vector<std::unique_ptr<reactive::Signal<bool>>> _done;
    std::map<std::string, std::string, std::less<>> _resolved;
    // Before the steps: a step ready as constructed submits from its constructor through `relay`.
    async::CallbackScope _callbacks;
    std::vector<std::unique_ptr<FormSession>> _steps;
};

/// @brief Renders a wizard: the title with the position, the current step's title and form, and
///        Back / Next bound to the session.
/// @param session The wizard session; mount the result after it and destroy the mount before it.
/// @return The view.
[[nodiscard]] inline ui::Node wizardView(WizardSession& session) {
    WizardSession* const wizard = &session;
    std::vector<ui::SwitchCase> cases;
    for (std::size_t i = 0; i < session.stepCount(); ++i) {
        cases.push_back(ui::SwitchCase{.key = ui::Key{static_cast<std::int64_t>(i)}, .node = formView(session.step(i))});
    }
    return ui::column(ui::Column{
        .children = {ui::text(ui::Text{.text = [wizard] {
                                           return wizard->title() + "  (" + std::to_string(wizard->current() + 1) + " / " +
                                                  std::to_string(wizard->stepCount()) + ")";
                                       },
                                       .role = ui::TextRole::Heading}),
                     ui::text(ui::Text{.text = [wizard] { return wizard->stepTitle(wizard->current()); }, .role = ui::TextRole::Muted}),
                     ui::switchOf(ui::Switch{.selector = [wizard] { return ui::Key{static_cast<std::int64_t>(wizard->current())}; },
                                             .cases = std::move(cases)}),
                     ui::row(ui::Row{.children = {ui::button(ui::Button{.label = "Back",
                                                                        .onClick = [wizard] { wizard->back(); },
                                                                        .common = {.enabled = [wizard] { return wizard->canBack(); }}}),
                                                  ui::button(ui::Button{.label = "Next",
                                                                        .onClick = [wizard] { wizard->next(); },
                                                                        .common = {.enabled = [wizard] { return wizard->canNext(); }}}),
                                                  ui::text(ui::Text{.text = "Last step — fill it in to finish",
                                                                    .role = ui::TextRole::Muted,
                                                                    .common = {.visible = [wizard] { return wizard->onLastStep(); }}})},
                                     .gap = 1})},
        .gap = 1});
}

}  // namespace morph::forms
```

Register the header and the test as listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][wizard]"`
Expected: PASS, 6 test cases.

Mutation check: in `relay`'s success handler, swap the two `record` calls (reply first, then the draft). Expected:
FAIL in "entering a step applies its prefill as JSON values, the reply winning" (`WizStepOne.label` is
`"sample"`). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/wizard.hpp tests/test_forms_engine_wizard.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): wizards — one FormSession per step, sequenced by successful submissions

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 14b: Entering a wizard step re-fetches its Choice options

A `WizardSession` builds every step's `FormSession` up front (Task 14), so a later step's `Choice` options are
fetched — and answered — before the earlier steps run. When the earlier step creates the row a later step's prefill
names (the forms demo's intake wizard: `RegisterSample` creates sample 100, step two's `sampleId` is prefilled with
it), Task 8's stale-selection rule clears the prefill at once, because the options answered for the current parents
lack it. Entering a step therefore re-fetches its options before the prefill is applied: the selection is kept while
the request is in flight and judged against a reply that includes the new row.

**Files:**
- Modify: `include/morph/forms/engine/form_session.hpp` — `FieldState::refreshOptions()`,
  `FormSession::refreshOptions()`
- Modify: `include/morph/forms/engine/wizard.hpp` — `WizardSession::next()`
- Test: `tests/test_forms_engine_wizard.cpp` (one case)

**Interfaces:**
- Consumes: Task 6/8's `FieldState` (`_optionsQuery`, `_members`), `FormSession` (`_fields`), `WizardSession::next()`,
  Part 1's `Query::refetch()`; the engine tests' `morph::test::formsengine::Harness`/`Call` and `WizardModel`,
  `WizardSession` (already named in `tests/test_forms_engine_wizard.cpp`).
- Produces: `FieldState::refreshOptions()`, `FormSession::refreshOptions()` (listed by Task 16 in `engine.md`).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_forms_engine_wizard.cpp`:

```cpp
TEST_CASE("WizardSession: entering a step refetches its Choice options, so a prefill naming a row the previous "
          "step created survives",
          "[forms-engine][wizard]") {
    auto const registered = std::make_shared<bool>(false);
    Harness harness;
    harness.server.replyWith([registered](Call const& call) -> std::optional<std::string> {
        if (call.action == "WizRegister") {
            *registered = true;
            return R"({"id":100})";
        }
        return R"({"ok":true})";
    });
    harness.server.optionsWith([registered](Call const&) -> std::optional<std::string> {
        return *registered ? R"([{"id":1,"name":"One"},{"id":100,"name":"New"}])" : R"([{"id":1,"name":"One"}])";
    });
    auto model = WizardModel::fromSchema("RegisterThenMeasure", R"({"w-steps":[{"action":"WizRegister","title":"Register"},
      {"action":"WizMeasure","title":"Measure","prefill":{"sampleId":"WizRegister.id"}}]})");
    REQUIRE(model.has_value());
    morph::forms::SchemaLookup const schemas = [](std::string_view action) -> std::optional<std::string> {
        if (action == "WizRegister") {
            return R"({"properties":{"name":{"type":"string","x-order":0}},"required":["name"]})";
        }
        if (action == "WizMeasure") {
            return R"({"properties":{"sampleId":{"type":"integer","x-order":0,"x-optionsAction":"ListSamples","x-optionValue":"id","x-optionLabel":"name"},"value":{"type":"integer","x-order":1}},"required":["sampleId","value"]})";
        }
        return std::nullopt;
    };
    WizardSession wizard{harness.rt, *std::move(model), schemas, harness.server.submitter(), harness.server.fetcher()};
    harness.settle();
    REQUIRE(harness.server.fetches().size() == 1);  // step two's options, answered before step one ran

    harness.type(wizard.step(0), "name", "Core 9");
    REQUIRE(wizard.canNext());
    wizard.next();
    harness.settle();
    CHECK(harness.server.fetches().size() == 2);
    CHECK(wizard.step(1).field("sampleId").text().peek() == "100");
    CHECK(wizard.step(1).field("sampleId").options().size() == 2);
}
```

Add `#include <memory>` to the file's includes if it is not there.

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][wizard]"
```

Expected: FAIL in the new case — `harness.server.fetches().size() == 2` is `1 == 2`, and
`wizard.step(1).field("sampleId").text().peek() == "100"` is `"" == "100"`; every other `[wizard]` case passes.

- [ ] **Step 3: Implement**

In `include/morph/forms/engine/form_session.hpp`, in `FieldState`'s public section after `optionsPending()`:

```cpp
    /// @brief Re-fetches a Choice's options for its current parents, and every nested member's; nothing for
    ///        another kind. The selection is kept while the request is in flight and judged against its reply.
    void refreshOptions() {
        if (_optionsQuery != nullptr) {
            _optionsQuery->refetch();
        }
        for (auto const& member : _members) {
            member->refreshOptions();
        }
    }
```

and in `FormSession`'s public section after `reset()`:

```cpp
    /// @brief Re-fetches every Choice's options (`FieldState::refreshOptions`).
    void refreshOptions() {
        for (auto const& field : _fields) {
            field->refreshOptions();
        }
    }
```

In `include/morph/forms/engine/wizard.hpp`, in `WizardSession::next()`, directly after
`FormSession& session = *_steps.at(target);`:

```cpp
        // The step's Choice options were fetched when the wizard was built, before the earlier steps ran, and a
        // prefill naming a row one of them created would be cleared against that answer. Re-fetching first keeps
        // the selection while the request is in flight and judges it against the reply.
        session.refreshOptions();
```

Task 16 states this in `engine.md`'s "Wizards" section and lists both `refreshOptions()` among the names beyond
the contract.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine]"
```

Expected: every `[forms-engine]` case passes, the new one included.

Mutation check: delete the `session.refreshOptions();` line from `WizardSession::next()`. Expected: FAIL in the new
case (`"" == "100"`). Restore.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/engine/form_session.hpp include/morph/forms/engine/wizard.hpp \
        tests/test_forms_engine_wizard.cpp
git commit -m "wip(forms): entering a wizard step re-fetches its Choice options before the prefill

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 15: The app shell — `ViewScreen`, `AppShellModel`, `AppShellSession`, `appShellView`

`forms/app.hpp` gains `ViewScreen<Id, View>` (kind `"view"`, ref `ViewTraits<View>::typeId()`), which the
comment above `App` has been leaving room for. An `app-*` document becomes a menu beside the current screen; each
screen's session — a `FormSession`, `WizardSession` or `CollectionSession` — is made the first time it is shown and
kept, so switching screens loses nothing.

**Files:**
- Modify: `include/morph/forms/app.hpp` — `#include "views.hpp"`; replace the comment block that begins
  `// A ViewScreen<Id, View> counterpart (kind: "view") could be added here:` with `ViewScreen`; in the file comment
  and `App`'s `@tparam Screens`, name `ViewScreen` beside `FormScreen` and `WizardScreen`
- Create: `include/morph/forms/engine/app_shell.hpp`
- Modify: `CMakeLists.txt` — `FILE_SET HEADERS`, after `include/morph/forms/engine/wizard.hpp`, add
  `include/morph/forms/engine/app_shell.hpp`
- Modify: `tests/CMakeLists.txt` — add `test_forms_engine_app_shell.cpp` after `test_forms_engine_wizard.cpp`
- Test: `tests/test_forms_engine_app_shell.cpp`

**Interfaces:**
- Consumes: `app::App`, `app::MenuEntry`, `app::appSchemaJson<AppT>()` (`include/morph/forms/app.hpp`);
  `views::ViewTraits<V>` (`include/morph/forms/views.hpp`); `forms::i18n::appTitleKey`, `appMenuLabelKey`;
  Tasks 12–14; Part 2's `ui::menu`, `ui::Menu`, `ui::MenuItem`, `ui::forEach<RowT>`.
- Produces: the contract's `morph::app::ViewScreen<Id, View>` (`id()`, `kind()` = `"view"`, `ref()`); in
  `morph::forms`: `ScreenKind{Form, Wizard, View}`, `AppMenuEntry{label, screen}`, `AppScreen{id, kind, ref}`,
  `AppShellModel` (`fromSchema(std::string_view appId, std::string_view appJson) -> std::expected<AppShellModel,
  SchemaError>`, `id()`, `title()`, `menu()`, `screens()`, `find(std::string_view) -> AppScreen const*`),
  `AppShellSources{actions, wizards, views}` (three `SchemaLookup`s), `AppShellSession`
  (`AppShellSession(reactive::Runtime&, AppShellModel, AppShellSources, Submitter, ChoiceFetcher,
  FormSessionOptions = {})`, `model()`, `current()`, `select(std::string_view)`, `form()`, `wizard()`,
  `collection()`, `title()`, `menuLabel(std::size_t)`), and the contract's `appShellView(AppShellSession&) ->
  ui::Node`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_forms_engine_app_shell.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/app.hpp>
#include <morph/forms/engine/app_shell.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "forms_engine_support.hpp"
#include "forms_engine_view_support.hpp"

// NOLINTBEGIN(misc-use-internal-linkage)
struct AsSamplesView {};
// NOLINTEND(misc-use-internal-linkage)

template <>
struct morph::views::ViewTraits<AsSamplesView> {
    static constexpr std::string_view typeId() noexcept { return "Test_AppShell_Samples"; }
};

namespace {

using morph::forms::AppShellModel;
using morph::forms::AppShellSession;
using morph::forms::ScreenKind;
using morph::test::formsengine::Call;
using morph::test::formsengine::Harness;
namespace ui = morph::ui;
namespace view = morph::test::formsview;

constexpr std::string_view kApp = R"({"app-title":"Lab","app-menu":[{"label":"Density","screen":"density"},
  {"label":"Samples","screen":"samples"},{"label":"Intake","screen":"intake"}],
  "app-screens":{"density":{"kind":"form","ref":"ComputeDensity"},"samples":{"kind":"view","ref":"SamplesView"},
    "intake":{"kind":"wizard","ref":"IntakeWizard"}}})";

[[nodiscard]] std::optional<std::string> actions(std::string_view action) {
    if (action == "ComputeDensity") {
        return R"({"properties":{"mass":{"type":"integer","x-order":0}},"required":["mass"]})";
    }
    if (action == "RegisterSample") {
        return R"({"properties":{"label":{"type":"string","x-order":0}},"required":["label"]})";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> wizards(std::string_view wizardId) {
    if (wizardId == "IntakeWizard") {
        return R"({"w-title":"Intake","w-steps":[{"action":"RegisterSample","title":"Register"}]})";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> views(std::string_view viewId) {
    if (viewId == "SamplesView") {
        return R"({"v-kind":"collection","v-title":"Samples","v-query":"ListSamples","v-columns":[{"field":"name","label":"Name"}]})";
    }
    return std::nullopt;
}

struct ShellHarness : Harness {
    ShellHarness() {
        server.replyWith([](Call const& call) -> std::optional<std::string> {
            return call.action == "ListSamples" ? R"([{"id":1,"name":"S-1"}])" : R"({"ok":true})";
        });
    }

    [[nodiscard]] std::unique_ptr<AppShellSession> shell() {
        auto model = AppShellModel::fromSchema("Lab", kApp);
        REQUIRE(model.has_value());
        return std::make_unique<AppShellSession>(rt, *std::move(model),
                                                 morph::forms::AppShellSources{actions, wizards, views},
                                                 server.submitter(), server.fetcher());
    }
};

}  // namespace

TEST_CASE("ViewScreen: a view screen emits kind view and the view's registered id", "[forms-engine][app-shell]") {
    using Shell = morph::app::App<"Lab", std::tuple<morph::app::MenuEntry<"Samples", "samples">>,
                                  std::tuple<morph::app::ViewScreen<"samples", AsSamplesView>>>;
    CHECK(morph::app::ViewScreen<"samples", AsSamplesView>::kind() == "view");
    CHECK(morph::app::appSchemaJson<Shell>().find(R"("samples":{"kind":"view","ref":"Test_AppShell_Samples"})") !=
          std::string::npos);
}

TEST_CASE("AppShellModel: the app-* document reads, and a malformed one is a SchemaError", "[forms-engine][app-shell]") {
    auto const model = AppShellModel::fromSchema("Lab", kApp);
    REQUIRE(model.has_value());
    CHECK(model->title() == "Lab");
    REQUIRE(model->menu().size() == 3);
    CHECK(model->menu()[1].screen == "samples");
    REQUIRE(model->find("intake") != nullptr);
    CHECK(model->find("intake")->kind == ScreenKind::Wizard);
    CHECK(model->find("samples")->kind == ScreenKind::View);
    CHECK(model->find("nope") == nullptr);
    CHECK(AppShellModel::fromSchema("X", R"({"app-screens":{"a":{"kind":"page","ref":"r"}}})").error().path ==
          "app-screens.a");
    CHECK_FALSE(AppShellModel::fromSchema("X", "[]").has_value());
}

TEST_CASE("AppShellSession: the first menu entry is shown; a screen's session is made when first asked for and kept",
          "[forms-engine][app-shell]") {
    ShellHarness harness;
    auto const shell = harness.shell();
    CHECK(shell->current() == "density");
    CHECK(harness.server.submits().empty());
    REQUIRE(shell->form("density") != nullptr);
    CHECK(shell->form("density") == shell->form("density"));
    CHECK(shell->collection("density") == nullptr);  // a form screen is no collection
    CHECK(harness.server.submits().empty());               // no list loads until its screen is made
    REQUIRE(shell->collection("samples") != nullptr);
    harness.settle();
    CHECK(shell->collection("samples")->rows().size() == 1);
    REQUIRE(shell->wizard("intake") != nullptr);
    CHECK(shell->wizard("intake")->stepCount() == 1);
    shell->select("samples");
    CHECK(shell->current() == "samples");
    shell->select("nope");
    CHECK(shell->current() == "samples");
}

TEST_CASE("appShellView: the menu beside the current screen; selecting swaps the screen and keeps its state",
          "[forms-engine][app-shell][view]") {
    ShellHarness harness;
    auto const shell = harness.shell();
    std::string const drawn = view::outline(morph::forms::appShellView(*shell));
    CHECK(drawn.find("  Row\n    Menu [Density|Samples|Intake]\n    ForEach\n") != std::string::npos);

    ui::testing::RecordingBackend backend;
    ui::Mounted const mounted{harness.rt, backend, morph::forms::appShellView(*shell)};
    harness.settle();
    backend.edit(backend.all(view::kTextInput).front(), "12");
    harness.settle();
    CHECK(shell->form("density")->body() == R"({"mass":12})");
    shell->select("samples");
    harness.settle();
    CHECK(backend.find("Text", "text", "S-1").has_value());
    CHECK(backend.all(view::kTextInput).empty());
    shell->select("density");
    harness.settle();
    CHECK(backend.prop(backend.all(view::kTextInput).front(), "text") == "12");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build/reactive --target morph_tests`
Expected: `fatal error: 'morph/forms/engine/app_shell.hpp' file not found`.

- [ ] **Step 3: Implement**

In `include/morph/forms/app.hpp`, add `#include "views.hpp"` after `#include "flows.hpp"`, and replace the comment
block beginning `// A ViewScreen<Id, View> counterpart (kind: "view") could be added here:` with:

```cpp
/// @brief A screen backed by a registered view (`BRIDGE_REGISTER_VIEW`): a collection or
///        master-detail screen.
/// @tparam Id   Screen-id, referenced from an `App::menu`'s `MenuEntry::screen()`.
/// @tparam View Registered view type this screen renders.
template <morph::forms::FixedString Id, typename View>
struct ViewScreen {
    /// @brief The screen's id.
    /// @return The declared id.
    [[nodiscard]] static constexpr std::string_view id() noexcept { return Id.view(); }

    /// @brief The screen's kind, for the `app-screens[id].kind` key.
    /// @return The literal `"view"`.
    [[nodiscard]] static constexpr std::string_view kind() noexcept { return "view"; }

    /// @brief The referenced view's registered type-id.
    /// @return `views::ViewTraits<View>::typeId()`.
    [[nodiscard]] static constexpr std::string_view ref() noexcept {
        return ::morph::views::ViewTraits<View>::typeId();
    }
};
```

In the file comment, change "a reference to an already-registered action form (`FormScreen`) or wizard
(`WizardScreen`)" to "a reference to an already-registered action form (`FormScreen`), wizard (`WizardScreen`) or
view (`ViewScreen`)", and "the shell only routes to existing action-forms and wizards" to "the shell only routes to
existing action-forms, wizards and views"; in `App`'s documentation change
`std::tuple<FormScreen<...> | WizardScreen<...>...>` to `std::tuple<FormScreen<...> | WizardScreen<...> |
ViewScreen<...>...>`.

Create `include/morph/forms/engine/app_shell.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/app_shell.hpp
/// @brief The app shell: an `app-*` document as a menu beside the current screen, each screen's
///        session made when first shown and kept.
///
/// Specified in `docs/spec/forms/engine.md`, "App shell".

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../../reactive/signal.hpp"
#include "../../render/i18n.hpp"
#include "../../ui/view.hpp"
#include "../i18n.hpp"
#include "collection.hpp"
#include "detail/text.hpp"
#include "form_session.hpp"
#include "form_view.hpp"
#include "wizard.hpp"

namespace morph::forms {

/// @brief What an `app-screens` entry routes to.
enum class ScreenKind : std::uint8_t {
    Form,    ///< `kind: form` — an action form (`ref` is the action type).
    Wizard,  ///< `kind: wizard` — a wizard (`ref` is the wizard id).
    View     ///< `kind: view` — a collection view (`ref` is the view id).
};

/// @brief One `app-menu` entry.
struct AppMenuEntry {
    /// @brief The menu label.
    std::string label{};
    /// @brief The screen id it routes to.
    std::string screen{};
};

/// @brief One `app-screens` entry.
struct AppScreen {
    /// @brief The screen id.
    std::string id{};
    /// @brief What it routes to.
    ScreenKind kind = ScreenKind::Form;
    /// @brief The action type, wizard id or view id.
    std::string ref{};
};

/// @brief The immutable model of an `app-*` document.
class AppShellModel {
public:
    /// @brief Reads an app document.
    /// @param appId   The app's registered id (`BRIDGE_REGISTER_APP`), the i18n key stem.
    /// @param appJson The document, in the vocabulary `app::appSchemaJson<AppT>()` emits.
    /// @return The model, or the error naming the malformed key.
    [[nodiscard]] static std::expected<AppShellModel, SchemaError> fromSchema(std::string_view appId,
                                                                              std::string_view appJson) {
        using detail::engine::Json;
        auto const dom = detail::engine::parseJson(appJson);
        if (!dom || !dom->is_object()) {
            return std::unexpected(SchemaError{"", "an app document must be a JSON object"});
        }
        AppShellModel model{};
        model._id = appId;
        auto const* const title = detail::engine::stringAt(*dom, "app-title");
        model._title = title != nullptr ? *title : std::string{appId};
        if (Json const* const menu = detail::engine::member(*dom, "app-menu"); menu != nullptr && menu->is_array()) {
            for (Json const& entry : menu->get_array()) {
                auto const* const label = detail::engine::stringAt(entry, "label");
                auto const* const screen = detail::engine::stringAt(entry, "screen");
                if (label == nullptr || screen == nullptr) {
                    return std::unexpected(SchemaError{"app-menu", "a menu entry needs a \"label\" and a \"screen\""});
                }
                model._menu.push_back(AppMenuEntry{*label, *screen});
            }
        }
        if (Json const* const screens = detail::engine::member(*dom, "app-screens"); screens != nullptr && screens->is_object()) {
            for (auto const& [screenId, node] : screens->get_object()) {
                auto const* const kind = detail::engine::stringAt(node, "kind");
                auto const* const ref = detail::engine::stringAt(node, "ref");
                if (kind == nullptr || ref == nullptr || (*kind != "form" && *kind != "wizard" && *kind != "view")) {
                    return std::unexpected(SchemaError{"app-screens." + screenId,
                                                       "a screen needs a \"ref\" and a \"kind\" of form, wizard or view"});
                }
                ScreenKind const screenKind = *kind == "form" ? ScreenKind::Form : (*kind == "wizard" ? ScreenKind::Wizard : ScreenKind::View);
                model._screens.push_back(AppScreen{screenId, screenKind, *ref});
            }
        }
        return model;
    }

    /// @brief The app's id.
    /// @return The id passed to `fromSchema`.
    [[nodiscard]] std::string_view id() const noexcept { return _id; }

    /// @brief `app-title`, or the id.
    /// @return The title.
    [[nodiscard]] std::string_view title() const noexcept { return _title; }

    /// @brief `app-menu`, in order.
    /// @return The entries.
    [[nodiscard]] std::span<AppMenuEntry const> menu() const noexcept { return _menu; }

    /// @brief `app-screens`, in document order.
    /// @return The screens.
    [[nodiscard]] std::span<AppScreen const> screens() const noexcept { return _screens; }

    /// @brief A screen by id.
    /// @param screenId The screen id.
    /// @return The screen, or null.
    [[nodiscard]] AppScreen const* find(std::string_view screenId) const noexcept {
        auto const found = std::ranges::find(_screens, screenId, &AppScreen::id);
        return found == _screens.end() ? nullptr : &*found;
    }

private:
    AppShellModel() = default;

    std::string _id;
    std::string _title;
    std::vector<AppMenuEntry> _menu;
    std::vector<AppScreen> _screens;
};

/// @brief Where an app shell finds the documents its screens name.
struct AppShellSources {
    /// @brief Action schemas, by action type (form screens, wizard steps, collection editors).
    SchemaLookup actions{};
    /// @brief Wizard documents, by wizard id.
    SchemaLookup wizards{};
    /// @brief View documents, by view id.
    SchemaLookup views{};
};

/// @brief The reactive state of one app shell: the current screen and every screen shown so far.
///
/// Non-copyable and non-movable. Destroy a mounted view of it before the session.
class AppShellSession {
public:
    /// @param runtime The runtime. Borrowed.
    /// @param model   The app model.
    /// @param sources Where the screens' documents come from.
    /// @param submit  Executes every screen's actions.
    /// @param choices Fetches Choice options for every screen.
    /// @param options Every screen's session options.
    AppShellSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, AppShellModel model, AppShellSources sources,
                    Submitter submit, ChoiceFetcher choices, FormSessionOptions options = {})
        : _rt{&runtime},
          _model{std::move(model)},
          _sources{std::move(sources)},
          _submit{std::move(submit)},
          _choices{std::move(choices)},
          _options{std::move(options)},
          _current{runtime, initialScreen(_model)} {}

    ~AppShellSession() = default;
    AppShellSession(AppShellSession const&) = delete;
    AppShellSession& operator=(AppShellSession const&) = delete;
    AppShellSession(AppShellSession&&) = delete;
    AppShellSession& operator=(AppShellSession&&) = delete;

    /// @brief The app model.
    /// @return The model.
    [[nodiscard]] AppShellModel const& model() const noexcept { return _model; }

    /// @brief The current screen. Tracked.
    /// @return Its id: the first menu entry's screen at first.
    [[nodiscard]] std::string const& current() const { return _current.get(); }

    /// @brief Shows a screen; an id the document does not declare changes nothing.
    /// @param screenId The screen id.
    void select(std::string_view screenId) {
        if (_model.find(screenId) != nullptr) {
            _current.set(std::string{screenId});
        }
    }

    /// @brief A form screen's session, made on first call.
    /// @param screenId The screen id.
    /// @return The session, or null for another kind or an unknown action schema.
    [[nodiscard]] FormSession* form(std::string_view screenId) {
        Screen& screen = screenFor(screenId);
        return screen.form.get();
    }

    /// @brief A wizard screen's session, made on first call.
    /// @param screenId The screen id.
    /// @return The session, or null for another kind or an unusable wizard document.
    [[nodiscard]] WizardSession* wizard(std::string_view screenId) {
        Screen& screen = screenFor(screenId);
        return screen.wizard.get();
    }

    /// @brief A view screen's session, made on first call.
    /// @param screenId The screen id.
    /// @return The session, or null for another kind or an unknown view document.
    [[nodiscard]] CollectionSession* collection(std::string_view screenId) {
        Screen& screen = screenFor(screenId);
        return screen.collection.get();
    }

    /// @brief The app's title, resolved through the translations (`<id>.title`).
    /// @return The title.
    [[nodiscard]] std::string title() const {
        return render::resolveText(_options.translations, _options.bcp47, std::nullopt, i18n::appTitleKey(_model.id()),
                                   _model.title());
    }

    /// @brief A menu entry's label, resolved through the translations (`<id>.menu.<index>.label`).
    /// @param index The entry.
    /// @return The label.
    [[nodiscard]] std::string menuLabel(std::size_t index) const {
        return render::resolveText(_options.translations, _options.bcp47, std::nullopt,
                                   i18n::appMenuLabelKey(_model.id(), index),
                                   detail::engine::elementAt(_model.menu(), index).label);
    }

private:
    struct Screen {
        std::unique_ptr<FormSession> form;
        std::unique_ptr<WizardSession> wizard;
        std::unique_ptr<CollectionSession> collection;
        bool made = false;
    };

    [[nodiscard]] static std::string initialScreen(AppShellModel const& model) {
        if (!model.menu().empty()) {
            return model.menu().front().screen;
        }
        return model.screens().empty() ? std::string{} : model.screens().front().id;
    }

    [[nodiscard]] Screen& screenFor(std::string_view screenId) {
        Screen& screen = _screens.try_emplace(std::string{screenId}).first->second;
        if (screen.made) {
            return screen;
        }
        screen.made = true;
        AppScreen const* const spec = _model.find(screenId);
        if (spec == nullptr) {
            return screen;
        }
        switch (spec->kind) {
            case ScreenKind::Form:
                if (auto const schema = _sources.actions ? _sources.actions(spec->ref) : std::nullopt) {
                    if (auto formModel = FormModel::fromSchema(spec->ref, *schema)) {
                        screen.form = std::make_unique<FormSession>(*_rt, *std::move(formModel), _submit, _choices, _options);
                    }
                }
                break;
            case ScreenKind::Wizard:
                if (auto const json = _sources.wizards ? _sources.wizards(spec->ref) : std::nullopt) {
                    if (auto wizardModel = WizardModel::fromSchema(spec->ref, *json)) {
                        try {
                            screen.wizard = std::make_unique<WizardSession>(*_rt, *std::move(wizardModel), _sources.actions,
                                                                            _submit, _choices, _options);
                        } catch (std::invalid_argument const&) {
                            screen.wizard.reset();  // a step's schema is missing: the screen shows nothing to edit
                        }
                    }
                }
                break;
            case ScreenKind::View:
                if (auto const json = _sources.views ? _sources.views(spec->ref) : std::nullopt) {
                    if (auto viewModel = CollectionModel::fromSchema(*json)) {
                        screen.collection = std::make_unique<CollectionSession>(*_rt, *std::move(viewModel), _sources.actions,
                                                                                _submit, _choices, _options);
                    }
                }
                break;
        }
        return screen;
    }

    reactive::Runtime* _rt;
    AppShellModel _model;
    AppShellSources _sources;
    Submitter _submit;
    ChoiceFetcher _choices;
    FormSessionOptions _options;
    reactive::Signal<std::string> _current;
    std::map<std::string, Screen, std::less<>> _screens;
};

/// @brief Renders an app shell: the title, and the menu beside the current screen's view (made
///        when first shown; a screen that cannot be made says so).
/// @param session The shell session; mount the result after it and destroy the mount before it.
/// @return The view.
[[nodiscard]] inline ui::Node appShellView(AppShellSession& session) {
    AppShellSession* const shell = &session;
    std::vector<ui::MenuItem> items;
    for (std::size_t i = 0; i < session.model().menu().size(); ++i) {
        std::string const screen = detail::engine::elementAt(session.model().menu(), i).screen;
        items.push_back(ui::MenuItem{.label = session.menuLabel(i), .onSelect = [shell, screen] { shell->select(screen); }});
    }
    auto content = ui::forEach<std::string>(
        [shell] { return std::vector<std::string>{shell->current()}; }, [](std::string const& screenId) { return ui::Key{screenId}; },
        [shell](reactive::Signal<std::string> const& slot) -> ui::Node {
            std::string const screenId = slot.peek();
            if (FormSession* const form = shell->form(screenId)) {
                return formView(*form);
            }
            if (WizardSession* const wizard = shell->wizard(screenId)) {
                return wizardView(*wizard);
            }
            if (CollectionSession* const collection = shell->collection(screenId)) {
                return collectionView(*collection);
            }
            return ui::text(ui::Text{.text = "this screen's document is not available: " + screenId, .role = ui::TextRole::Error});
        });
    return ui::column(ui::Column{
        .children = {ui::text(ui::Text{.text = session.title(), .role = ui::TextRole::Heading}),
                     ui::row(ui::Row{.children = {ui::menu(ui::Menu{.items = std::move(items)}), std::move(content)}, .gap = 2})},
        .gap = 1});
}

}  // namespace morph::forms
```

Add `#include <algorithm>` and `#include <stdexcept>` to the include list. Register the header and the test as
listed under **Files**.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[forms-engine][app-shell],[app]"`
Expected: PASS — 4 new cases and the existing `[app]` cases in `test_flows_apps.cpp`.

Mutation check: in `screenFor`, delete `screen.made = true;`. Expected: FAIL in "the first menu entry is shown; a
screen's session is made when first asked for and kept" (`form("density") == form("density")` compares two
different sessions — and the first is destroyed while the second is made). Restore it.

- [ ] **Step 5: Commit**

```bash
git add include/morph/forms/app.hpp include/morph/forms/engine/app_shell.hpp tests/test_forms_engine_app_shell.cpp \
        tests/CMakeLists.txt CMakeLists.txt
git commit -m "wip(forms): ViewScreen and the app shell — a menu beside lazily made screens

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 16: Specs, maps, changelog and the pinned depth

**Files:**
- Create: `docs/spec/forms/engine.md`
- Modify: `docs/spec/forms/forms.md` — a new `## The forms engine (C++)` section directly before
  `## Shipped Qt/QML reference renderer`, and a line in its `## Contents`
- Modify: `docs/spec/forms/views.md` — a new `## The forms engine's collections` section directly before
  `## The Qt/QML reference renderer`
- Modify: `docs/spec/forms/workflows_navigation.md` — `ViewScreen` in `## C++ descriptors` and in the `morph::app`
  API table; a new `## The forms engine's wizards and app shell` section before `## The Qt/QML reference renderer`;
  the `kind: "view"` bullet of `## Limitations` rewritten
- Modify: `docs/spec/forms/widget_hints.md` — a new `## In the forms engine` section before `## API reference`
- Modify: `docs/spec/README.md` — the "Schema-driven UI" group
- Modify: `docs/ARCHITECTURE.md` — the `morph::forms` row of "Namespace map", the `forms/` and `util/` tables of
  "Header map"
- Modify: `CHANGELOG.md` — `## [Unreleased]` → `### Added`
- Modify: `docs/spec/pinned_facts.toml`, `tests/test_pinned_facts.cpp`

**Interfaces:**
- Consumes: the API exactly as Tasks 1–15 ship it; where this text and the code disagree, fix the text.
- Produces: the authoritative spec Parts 8–10 read before touching the engine.

- [ ] **Step 1: Write `docs/spec/forms/engine.md`**

```markdown
# The forms engine — design

Design spec for the C++ forms engine (`include/morph/forms/engine/`): a form described by schema JSON —
or by an action type through `schemaJson<A>()` — as a toolkit-free reactive session (`FormSession`) and a
`ui::Node` view of it (`formView`), so one form renders on every frontend. Collections, wizards and the app
shell are built from the same parts.

## Contents

- [Shape](#shape)
- [The field model](#the-field-model)
- [Rules](#rules)
- [Values and exactness](#values-and-exactness)
- [FormSession](#formsession)
- [Submitters](#submitters)
- [Typed forms](#typed-forms)
- [Rendering](#rendering)
- [Overrides](#overrides)
- [Collections](#collections)
- [Wizards](#wizards)
- [App shell](#app-shell)
- [Headers](#headers)
- [Differences from the QML renderer](#differences-from-the-qml-renderer)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)

## Shape

    schema JSON ──FormModel::fromSchema─────┐
                                            ├─► FormModel ─► FormSession ─► formView() ─► ui::Node
    action A ──FormModel::forAction<A>()────┘                   │
                                                                └─ Submitter / ChoiceFetcher

- `FormModel` — immutable: fields, kinds, constraints, layout groups, parsed rules, submit mode.
- `FormSession` — reactive state over a model: drafts, encodings, readiness, presentation, Choice
  options, submission. No toolkit; tested headless.
- `formView(session, options)` — `ui` nodes bound to the session.
- `FormModel::forAction<A>()` — the model of a registered action's own schema (`schemaJson<A>()`).
- `Form<A>` — a `FormSession` over that model with typed access.

The schema JSON is the contract between the two entrances, already pinned by the conformance and rule
corpora; nothing about it changes for the engine.

## The field model

`FormModel::fromSchema(actionType, schemaJson) -> std::expected<FormModel, SchemaError>` reads the
emitted vocabulary with glaze's u64 DOM (integers keep every digit). Properties are described in
`x-order` (stable for ties). A property is resolved through `$ref` into `$defs` (its own keys win), then a
nullable `anyOf`/`oneOf` collapses onto its first non-null branch (outer keys win; the branches'
`anyOf`/`oneOf`/`const` are dropped).

The kind is decided in this order, which is the order the QML renderer's encoder asked in:

| `FieldKind` | From |
|---|---|
| `ObjectArray` / `Array` | `type: array`, items of object (or array) type / of scalars |
| `Enum` | a bare `enum`, or a `oneOf`/`anyOf` of `const` branches (one nested level, as glaze spells `std::optional<E>`), unless `x-optionsAction` |
| `Choice` | `x-optionsAction` (`x-optionValue`, `x-optionLabel`, `x-optionsDependsOn`) |
| `DateTime` / `Date` | `format: date-time` / `format: date` |
| `Quantity` / `Decimal` | `x-decimalPlaces`, with / without `ExtUnits` |
| `Slider` | `x-widget: slider` with `x-min` and `x-max` on an integer or number |
| `Integer`, `Boolean`, `Number` | the JSON type |
| `Object` | `type: object` |
| `Multiline` / `Text` | `x-widget: textarea` / anything else |

A `FieldSpec` carries the wire name and path, title, help, placeholder, `x-i18nKey`, `required`,
`x-blankAs` (text kinds), `x-readonly`, `x-hidden`, `x-computed.inputs`, `minimum`/`maximum`/`multipleOf`
as canonical decimal text, `x-exactMinimum`/`x-exactMaximum`, the instance bounds `x-minimum`/`x-maximum`
as `Rational`, `x-decimalPlaces`, `x-displayDecimals` (plain numbers only), the unit and its exact
alternatives (canonical first), enum options as `{valueJson, label}`, the Choice descriptor, `x-section`,
`x-colspan`, `x-widget`, the slider track, an Array's item type, and an Object's or ObjectArray's members.

**Nesting.** Members are described to `kMaxNestingDepth` (4) levels below the action and stop at a `$ref`
already on the path, so a self-referential type is described once; the member where it would repeat is
`truncated`. **Groups:** `groups()` is the declared `x-layout` groups, each holding the fields whose
`x-section` names it in `x-order`, then one implicit group of the rest; without `x-layout`, one implicit
group of every field. A malformed document — not JSON, not an object, `properties` not an object, a
property not an object, a rule without a kind, `x-layout.groups` not an array — is a `SchemaError` naming
the path.

**From an action type.** `FormModel::forAction<A>()` is `fromSchema(ActionTraits<A>::typeId(),
schemaJson<A>())`. The schema is compiled from `A`, so a reader error there is a defect in the emitter or the
engine, not bad input: `forAction` throws `std::logic_error` naming the action id and the error's path and
message instead of returning an `expected` every caller would unwrap.

## Rules

`x-rules` parses into `RuleExpr` trees; a `"kind"` the engine does not know is kept as `Unknown`.
Evaluation is three-valued over each field's encoding — Blank, Invalid (typed, does not encode), Valid
with an exact `Scalar` (boolean, string, canonical decimal, `Rational`):

| Node | Blank operand | Invalid operand | Valid |
|---|---|---|---|
| `engaged` / `notEngaged` | False / True | Unknown | True / False |
| `equals` | False | Unknown | exact equality; mismatched types are False |
| comparisons | True (vacuous) | Unknown | exact order; incomparable types are Unknown |
| membership | not engaged | Unknown unless the definite count decides | counted |
| `requiredWhen` (rule) | | | True unless `when` is True, then `engaged(field)` |
| `visibleWhen`, `readonlyWhen` (rule) | | | True |
| unknown kind | | | Unknown |

`and`/`or`/`not` are Kleene. **A gating rule blocks readiness only when it is False.** An Invalid operand
already blocks through its own field error, so the only Unknown that reaches readiness is a kind the engine
does not know — the server, which knows it, judges it. Presentation: `visible` unless a `visibleWhen`
naming the field is False; `readonly` when a `readonlyWhen` is True; dynamically `required` when a
`requiredWhen` is True — Unknown presents as visible, editable, not required. Comparisons never pass through
a double: integers compare as digits, decimals and quantities as `Rational`s or by digit
cross-multiplication. This is the only rule evaluator on the client; `allRulesSatisfied<A>` stays the
server's, and the rule corpus holds both to the same 46 rows.

## Values and exactness

`encodeScalar(spec, draft, unit, context)` trims the draft and returns `std::nullopt` (blank),
a `WireValue{json, scalar}`, or a `FieldError{code, message}` — codes `malformed`, `too-precise`,
`below-minimum`, `above-maximum`, `not-a-multiple`, `not-an-option`, `out-of-range`, `incomplete`,
`unrepresentable`; messages resolve through the session's translations under `morph.forms.error.<code>`.

| Kind | Literal |
|---|---|
| Text, Multiline, Date | the JSON string (`JSON.stringify` quoting); a Date must be a real `YYYY-MM-DD` |
| DateTime | `YYYY-MM-DDTHH:MM[:SS]` in the display zone → `"…Z"` UTC |
| Integer | the digits, leading zeros dropped, bounds by digits (`x-exact*` preferred) |
| Number | the typed digits after `render::normalizeLocaleNumber`; no exponent; `x-displayDecimals` is an entry limit |
| Decimal, Quantity | `{"num":N,"den":D,"dp":P}`: `parseDecimal` in the entry unit, `N` = scaled digits × unit num, `D` = 10^unit decimals × unit den — unreduced, as the QML renderer sent it |
| Boolean | `true`/`false`; a blank **required** box is `false` |
| Enum | the option's literal; anything outside the set is `not-an-option` |
| Choice | the selected option's literal |
| Array | `[…]` of the comma-separated items, by item type; blanks dropped |

Quantity bounds (`minimum`, `maximum`, `multipleOf`, instance bounds) are checked exactly in the canonical
unit, whatever unit the value was typed in. `math::parseDecimal` (`util/rational.hpp`) is the decimal
parser the engine and applications share. `decodeScalar` is the inverse: numbers in the display locale
without grouping, a Number padded to `x-displayDecimals`, a Quantity rounded half away from zero to its
places, instants in the display zone. `convertDraft` switches a Quantity's unit exactly.

## FormSession

A `FieldState` per field: `text`, `unit`, `engaged` signals; `encoded`, `error`, `visible`, `readonly`,
`required` computeds; Object members and ObjectArray rows (`RowState`, held by `shared_ptr` so a mounted
row outlives its removal); a Choice's options. **Readiness:** every required field encodes, no field has an
error, no gating rule is False, `FormSessionOptions::accepts(body)` when set. `body()` is the JSON in
`x-order` while ready. An `x-blankAs: empty` field filled once and cleared sends `""`. An Object is blank
when every member is; a blank required member makes it `incomplete`. A collection encodes `[]` once engaged
or, when required, while empty; a truncated member is omitted while blank and `unrepresentable` holding a
value.

**Submission** is a `reactive::Mutation` over the `Submitter`. In automatic mode an Effect submits whenever
the form is ready with a body different from the last one submitted or seen; a form ready as constructed
submits once. A programmatic change — `prefill`, `assign`, `reset`, clearing a stale Choice — records the
body it produced as seen, so it never submits; the next user edit does. Explicit mode submits only on
`submit()`, and only while ready. `prefill` replaces every draft (absent members blank, units canonical);
non-object text changes nothing.

**Choice options** are a `reactive::Query` keyed on the request body: `{}` for an independent Choice,
`{"parent":<literal>,…}` once every parent encodes, idle otherwise. Latest wins; an idle Choice has no
options. A selection the answered options do not contain is cleared — only once the query answered for
the current parents and did not fail, so a value prefilled before its options arrive survives the fetch.

## Submitters

`bridgeSubmitter(bridge, callbacks)` and `bridgeChoiceFetcher` route an action type to the model registering
it — `ActionExecuteRegistry::modelsServing` — and execute the body through a `BridgeHandler<Model,
NoSharing>` made once per model (`makeHandler`), so validation, journaling and backends behave as for any
typed call. An id no model serves, or more than one does, fails the completion with a message naming it.

**Submitting through the application's handlers.** Under `LocalBackend` every `BridgeHandler` of a
non-shared model is its own model instance, so the handler `bridgeSubmitter` makes is not the one the rest of
the screen reads. When a form's submissions must land in the application's instance — an in-memory model, or
a shared instance another handler attached — use `handlerSubmitter(callbacks, handlers...)` and
`handlerChoiceFetcher(callbacks, handlers...)` (`handler_submitter.hpp`) instead. Each action type goes to the
first of the given handlers whose model serves it (`BridgeHandler::servesAction`), in the order they are
passed, through `executeJson`; an action none of them serves fails the completion with
`std::invalid_argument`, delivered on `callbacks`. The handlers are borrowed and must outlive the returned
function. Use `bridgeSubmitter` when the model's state lives outside the instance (a database), so any
handler of it sees the same data.

## Typed forms

`Form<A, M, S>` builds its model with `FormModel::forAction<A>()`.
`set<&A::member>(value)` writes the member's JSON as a programmatic change; `value()` decodes the body into
`A`; `ActionValidator<A>` (the action's `validate()`) joins readiness through `accepts`; a submission goes
through `BridgeHandler::execute<A>`, so `lastResult()` is typed.

## Rendering

`formView(session, options)`:

| Kind | Control |
|---|---|
| Text, Array | `TextInput` |
| Multiline | `TextInput` (Multiline) |
| Integer, Number, Decimal | `TextInput`, the unit as muted `Text` beside it |
| Quantity | `Row{TextInput, Select(units)}`, or the unit `Text` without alternatives |
| Boolean | `Checkbox` |
| Enum | `Select` (Radio for `x-widget: radio`), nothing selected until chosen |
| Choice | `Row{Select, Busy}` |
| DateTime / Date | `DateTimeInput` |
| Slider | `Slider` |
| Object | `Panel(label)` of member cells |
| ObjectArray | `ForEach` of `Panel(label n){cells, Remove}`, then `Add` |

A cell is label (` *` while required), control, help (muted, only when present), error (Error role,
visible while there is one); its `visible` and `enabled` follow the field. Groups: the implicit group is a
`Grid` of `flatGridColumns` (1 while `gridColumns` is 2), a section a `Panel`, an accordion a collapsible
`Panel` (`FormSession::groupCollapsed`), consecutive tab groups one `Tabs` (`tabSelection`); `x-colspan`
clamps to the grid. Then the status line, the Submit button in explicit mode (enabled while ready), and
`ok: <reply>` / `error: <message>`.

## Overrides

`Overrides` maps field path, widget hint, unit (`unitAscii`) and kind — resolved in that order, then the
built-in — to `std::function<ui::Node(FieldView&)>`. An override replaces the control; the cell around it
stays. `FieldView` (spec, state, session, `defaultControl()`) is copyable and safe to keep in a node.
Chrome is not a registry: a host wanting its own frame builds it from the same `FieldState` and
`FormSession` signals `formView` binds to.

## Collections

`CollectionModel::fromSchema(viewJson)` reads `v-*`. `CollectionSession`: the list is a `Query` on
`v-query` (rows are the reply array or its first array member; cells formatted exactly; keys by
`v-rowKey`); actions are one `Mutation` that refetches the list on success; a `confirm` action waits in
`pendingConfirm()`; the row editor is one `FormSession` for `v-rowAction`, reset and prefilled from `bind`
on `open()`, closed and the list refetched when its submission succeeds. `collectionView` draws a `Table` of
the visible columns with Open and row-action buttons, collection actions beside the title, the editor in a
`Dialog` (beside the table for master-detail), and the confirm `Dialog`.

## Wizards

`WizardModel::fromSchema(id, wizardJson)` reads `w-*`. `WizardSession` keeps one `FormSession` per step; a
step is done when its last submission succeeded. Each success records `<Action>.<member>` from the
submitted body, then from the reply — the reply wins, as `FlowSession` documents. Next applies the step's
`prefill` with `assign` (JSON values), and a step the prefill leaves ready submits in automatic mode. Entering
a step first re-fetches its Choice options (`FormSession::refreshOptions`, `FieldState::refreshOptions`), so a
prefill naming a row an earlier step created is judged against options that include it.
`wizardView` draws the title with the position, the step title, the step's form, Back and Next.

## App shell

`AppShellModel::fromSchema(id, appJson)` reads `app-*`, screen kinds `form`, `wizard` and `view`
(`app::ViewScreen<Id, View>`). `AppShellSession` shows the first menu entry's screen and makes each screen's
session the first time it is shown, from `AppShellSources`. `appShellView` draws a `Menu` beside the current
screen.

## Headers

Header-only, base `morph` target. `engine/field_model.hpp` (model), `rules.hpp`, `values.hpp`,
`form_session.hpp` (no rendering), `bridge_submitter.hpp`, `handler_submitter.hpp`, `typed_form.hpp`,
`overrides.hpp`, `form_view.hpp`, `collection.hpp`, `wizard.hpp`, `app_shell.hpp`; `engine/detail/text.hpp`.
A controller that needs only `FormSession` includes `form_session.hpp` and nothing from `morph::ui`.

## Differences from the QML renderer

| Behaviour | QML renderer | Engine |
|---|---|---|
| Nested objects, collections of objects | unrepresentable unless a slot claims them | drawn and encoded |
| A required collection with no rows | blocks until a slot writes `[]` | `[]` |
| Array items | always strings | by item type |
| Quantity bounds | checked in the canonical unit only | checked whatever the entry unit |
| A required checkbox | seeded `"false"` into the draft | blank draft that encodes `false` |
| Clearing a stale Choice | on options arrival | whenever the answered options do not contain it |
| Several `visibleWhen`/`requiredWhen` for one field | the first one | all of them |
| Chrome | a registry of QML components | composed from the session's signals |

## Design decisions

| Decision | Why |
|---|---|
| Unknown blocks nothing | A newer server's rule kinds must not lock an older client out; the server re-checks. |
| Programmatic changes record their body | Prefill and reset are not user actions; recording the body (not a flag) also lets a user edit in the same flush through. |
| Unreduced Quantity literals | Byte parity with what the QML renderer sent and the corpora pin. |
| Rows by `shared_ptr` | A removed row's view still binds to it until it unmounts. |
| Readiness through `accepts` | `Form<A>` adds `validate()` without a second readiness path. |

## Limitations

- A self-referential type is editable to the first repetition only; deeper is `truncated`.
- Choice fetching depends on a `ChoiceFetcher`; without one a Choice has no options.
- Display text of the engine's own chrome (status, Add, Remove, Back, Next) is English.
```

- [ ] **Step 2: Add the engine sections to the existing specs**

In `docs/spec/forms/forms.md`, directly before `## Shipped Qt/QML reference renderer`:

```markdown
## The forms engine (C++)

The renderer contract above has a C++ implementation: `include/morph/forms/engine/`. `FormModel` reads a
schema in this vocabulary; `FormSession` is the reactive state (drafts, exact encodings, readiness, `x-rules`,
Choice options, submission); `formView` renders it as a `ui::Node` tree on every frontend; `Form<A>` is the
typed facade over `schemaJson<A>()`. It runs the rule, instance-bounds and conformance corpora
(`tests/data/`) alongside the compiled evaluators. Everything it does, and where it differs from the QML
renderer below, is specified in [engine.md](engine.md).
```

and add `- [The forms engine (C++)](#the-forms-engine-c)` to its `## Contents` before the QML renderer's entry.

In `docs/spec/forms/views.md`, before `## The Qt/QML reference renderer`:

```markdown
## The forms engine's collections

`morph::forms::CollectionModel` reads this vocabulary and `CollectionSession` runs it: the list is a query on
`v-query`, actions are mutations that refetch it on success, `confirm` actions wait for an answer, and the
`v-rowAction` editor is a `FormSession` prefilled from `bind`. Bodies take the row's JSON values, so an id past
2^53 is sent exactly; cells are formatted from the exact `{num, den, dp}`. `collectionView` renders it as a
table. See [engine.md, "Collections"](engine.md#collections).
```

In `docs/spec/forms/workflows_navigation.md`: in `## C++ descriptors`, after the `WizardScreen` description, add
`` `ViewScreen<Id, View>` — a screen backed by a registered view: `kind()` is `"view"`, `ref()` is
`views::ViewTraits<View>::typeId()`. ``; in the `morph::app` API table, after the `WizardScreen` row, add
`` | `ViewScreen<Id, View>` | struct | `id()`, `kind()=="view"`, `ref()==ViewTraits<View>::typeId()`. | ``;
before `## The Qt/QML reference renderer` add:

```markdown
## The forms engine's wizards and app shell

`morph::forms::WizardSession` runs a `w-*` document with one `FormSession` per step; a step is done when its
last submission succeeded, and Next applies the next step's `prefill` from the resolved values — the submitted
body, then the reply, the reply winning — as JSON values. `AppShellSession` runs an `app-*` document, making
each screen's session (form, wizard or view) the first time it is shown. See
[engine.md, "Wizards" and "App shell"](engine.md#wizards).
```

and replace the `## Limitations` bullet that begins `**`kind: "view"` is not implemented.**` with:

```markdown
- **`kind: "view"` routes to a registered view** through `app::ViewScreen<Id, View>`; the forms engine's app
  shell renders it. The QML reference demo's `AppShell.qml` renders a placeholder for it.
```

In `docs/spec/forms/widget_hints.md`, before `## API reference`:

```markdown
## In the forms engine

`formView` draws `x-widget: textarea` (`Multiline`) as a multiline `TextInput`, `x-widget: slider` with
`x-min`/`x-max`/`x-step` (`Ranged`) as a `Slider` — the encoding follows the member's JSON type — and
`x-widget: radio` on an Enum or Choice as a radio `Select`. `Overrides::byWidget(hint, …)` replaces the
control for every field with a hint. See [engine.md, "Rendering"](engine.md#rendering).
```

- [ ] **Step 3: Maps and changelog**

In `docs/spec/README.md`, in the "**Schema-driven UI**" group, add after the `forms/forms.md` link line:
`` [`forms/engine.md`](forms/engine.md) · ``.

In `docs/ARCHITECTURE.md`, "Namespace map", extend the `morph::forms` row's public symbols with: `FormModel`,
`FieldSpec`, `FieldKind`, `RuleExpr`, `Tri`, `FormSession`, `FieldState`, `FormSessionOptions`, `Submitter`,
`ChoiceFetcher`, `bridgeSubmitter`, `bridgeChoiceFetcher`, `handlerSubmitter`, `handlerChoiceFetcher`, `Form<A>`, `formView`, `Overrides`, `FieldView`,
`CollectionSession`, `collectionView`, `WizardSession`, `wizardView`, `AppShellSession`, `appShellView`; and to
the `forms/` header table add:

```markdown
| `forms/engine/field_model.hpp` | `FormModel`, `FieldSpec`, `FieldKind`, `FieldGroupSpec` — the runtime form model read from schema JSON or an action type (`forAction<A>`) |
| `forms/engine/rules.hpp` | `RuleExpr`, `Tri`, `Scalar`, `SchemaError` — three-valued, exact `x-rules` evaluation and presentation verdicts |
| `forms/engine/values.hpp` | `encodeScalar`, `decodeScalar`, `convertDraft`, `parseOptions`, `FieldError`, `WireValue` |
| `forms/engine/form_session.hpp` | `FormSession`, `FieldState`, `RowState`, `Submitter`, `ChoiceFetcher` — a form's reactive state, no rendering |
| `forms/engine/bridge_submitter.hpp` | `bridgeSubmitter`, `bridgeChoiceFetcher` — routed through `ActionExecuteRegistry` |
| `forms/engine/handler_submitter.hpp` | `handlerSubmitter`, `handlerChoiceFetcher` — routed through the application's own `BridgeHandler`s |
| `forms/engine/typed_form.hpp` | `Form<A, M, S>` — the typed facade |
| `forms/engine/overrides.hpp`, `form_view.hpp` | `Overrides`, `FieldView`, `formView` — the form as `ui` nodes |
| `forms/engine/collection.hpp`, `wizard.hpp`, `app_shell.hpp` | collections, wizards and the app shell over `FormSession` |
| `forms/engine/detail/text.hpp` | exact text helpers (detail) |
```

and in the `util/` table extend the `util/rational.hpp` row's text with `; `parseDecimal`, the exact decimal
entry parser`.

In `CHANGELOG.md`, under `## [Unreleased]` → `### Added` (after the entries earlier parts of this branch
added):

```markdown
- **The C++ forms engine** (`include/morph/forms/engine/`), header-only in the base `morph` target: a form
  described by schema JSON or by an action type (`Form<A>`) as a reactive `FormSession` — exact encodings
  (integers by digits, `{num, den, dp}` quantities with exact unit conversion), three-valued `x-rules`,
  readiness, automatic or explicit submission, prefill, Choice options as queries — and `formView`, which
  renders it as a `morph::ui` tree on every frontend, with `Overrides` per field, widget hint, unit or kind.
  Collections (`v-*`), wizards (`w-*`) and the app shell (`app-*`) are built on it. `bridgeSubmitter` /
  `bridgeChoiceFetcher` route an action type to its model through `ActionExecuteRegistry`, which gains
  `modelsServing` and `makeHandler`; `handlerSubmitter` / `handlerChoiceFetcher` route it through the
  application's own handlers instead, so an in-memory or shared model instance receives the submissions.
  `FormModel::forAction<A>()` builds a form model from a registered action. Specified in
  `docs/spec/forms/engine.md`.
- **`morph::math::parseDecimal`**: locale entry text to an exact `Rational`, refusing more places than
  declared.
- **`morph::app::ViewScreen<Id, View>`**: an app-shell screen of kind `"view"`.
```

- [ ] **Step 4: Pin the nesting depth**

Append to `docs/spec/pinned_facts.toml`:

```toml

# ── The forms engine ────────────────────────────────────────────────────────
FORMS_ENGINE_MAX_NESTING_DEPTH = 4   # morph::forms::kMaxNestingDepth; include/morph/forms/engine/field_model.hpp
```

Append to `tests/test_pinned_facts.cpp` (add `#include <morph/forms/engine/field_model.hpp>`):

```cpp
// ── The forms engine ─────────────────────────────────────────────────────────

static_assert(morph::forms::kMaxNestingDepth ==
              static_cast<std::size_t>(morph::pinned_facts::kExpected_FORMS_ENGINE_MAX_NESTING_DEPTH));
```

Run: `cmake build/reactive && cmake --build build/reactive --target morph_tests` — expected to build. Change the
toml value to 5 and reconfigure: expected a `static_assert` failure. Restore it.

- [ ] **Step 5: Build the docs with warnings as errors**

```bash
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
```

Expected: exits 0. A warning naming a `morph/forms/engine` symbol is a missing `@param`/`@tparam`/`@return`: fix
the header. Then `pre-commit run --files docs/spec/forms/*.md docs/spec/README.md docs/ARCHITECTURE.md CHANGELOG.md`
— expected clean (whitespace, JSON validity, codespell).

- [ ] **Step 6: Commit**

```bash
git add docs/spec/forms docs/spec/README.md docs/ARCHITECTURE.md CHANGELOG.md docs/spec/pinned_facts.toml \
        tests/test_pinned_facts.cpp
git commit -m "wip(forms): specify the forms engine

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 17: Whole-part verification

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Strict build and the full suite**

```bash
cmake --build build/reactive && ctest --test-dir build/reactive --output-on-failure
```

Expected: every test passes; `VERIFY_INTERFACE_HEADER_SETS` compiles each new public header standalone — the
twelve headers this part adds to `FILE_SET HEADERS` are, in order, `forms/engine/rules.hpp`, `field_model.hpp`,
`values.hpp`, `form_session.hpp`, `bridge_submitter.hpp`, `handler_submitter.hpp` (Task 10b), `typed_form.hpp`,
`overrides.hpp`, `form_view.hpp`, `collection.hpp`, `wizard.hpp` and `app_shell.hpp`, with
`forms/engine/detail/text.hpp` in `morph_detail_headers` (`forms/app.hpp` and `core/bridge.hpp` change in place).
Check the list against `CMakeLists.txt`: `grep -c 'include/morph/forms/engine/' CMakeLists.txt` prints 13. Then
`cmake --build build/all && ctest --test-dir build/all -R forms --output-on-failure` (`build/all` still configures
`MORPH_BUILD_FORMS_QML=ON`): the QML suite still reads the corpora from `tests/data/`.

- [ ] **Step 2: The QuickTest parity map is real**

Every TEST_CASE the plan's "QuickTest parity map" names exists (the map's backticked tokens are the test names,
the `.cpp` files and the `tst_*.qml` files; only the names are checked):

```bash
./build/reactive/tests/morph_tests --list-tests --verbosity quiet > /tmp/morph_names.txt
sed -n '/^## QuickTest parity map/,/^## What the parity map does not carry/p' \
    docs/superpowers/plans/2026-10-04-declarative-ui-tui-5-forms-engine.md \
  | grep -o '`[^`]*`' | tr -d '`' | grep -v -e '\.cpp$' -e '\.qml$' | sort -u \
  | while IFS= read -r name; do grep -Fxq -- "$name" /tmp/morph_names.txt || echo "MISSING: $name"; done
```

Expected: no `MISSING:` line. A name with an escaped quote in the source (`\"\"`) is listed by Catch2 with plain
quotes; the map spells it that way.

- [ ] **Step 3: Sanitizers** (Linux; on macOS an ASan configure of the same tree)

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-asan/tests/morph_tests asan
./build/clang-asan/tests/morph_tests "[forms-engine]"
cmake --preset clang-tsan && cmake --build --preset clang-tsan
bash scripts/check_sanitizer_instrumentation.sh --binary build/clang-tsan/tests/morph_tests tsan
./build/clang-tsan/tests/morph_tests "[forms-engine]"
```

Expected: clean. The removed-row and destroyed-session cases rely on ASan as their observer; the bridge cases run
a real thread pool under TSan.

- [ ] **Step 4: clang-tidy over the changed lines** — the recipe in CONTRIBUTING, "Running the `clang-tidy-diff`
  gate locally", with `origin/master...HEAD` and the file count asserted non-zero.

Expected: no findings.

- [ ] **Step 5: Install/export**

```bash
bash scripts/check_install_export.sh
```

Expected: passes — `forms/engine/detail/text.hpp` is in the detail file set, so the installed public engine
headers compile.

- [ ] **Step 6: Commit any fixes**

```bash
git add -A include/morph tests docs CMakeLists.txt src/qt/forms
git commit -m "wip(forms): fixes from the sanitizer, tidy and install gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip the commit if there was nothing to fix, and say so in the hand-off.

---

### Task 18: Squash this part into its one commit

- [ ] **Step 1: Squash**

Follow the master plan's "Squashing a part" procedure with key `forms` and this message:

```text
forms: the C++ forms engine, rendered through the view tree

The schema-driven forms runtime moves from QML/JS into C++: FormModel reads
the schema vocabulary schemaJson<A>() emits; FormSession keeps drafts,
exact encodings, three-valued x-rules, readiness, Choice options and
automatic or explicit submission as morph::reactive state; formView renders
it as morph::ui nodes, so one form draws on every frontend. Form<A>,
collections, wizards and the app shell (with ViewScreen) build on it, and
math::parseDecimal is the decimal entry parser they share. The rule,
instance-bounds and conformance corpora run through the engine.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

If a deviation from this plan was made during execution, add one paragraph per deviation to the body before the
sign-off — what the plan said, what was done instead, and why (master plan, "The docs commit").

- [ ] **Step 2: Check the history**

`git log --oneline master..HEAD` ends with `forms: the C++ forms engine, rendered through the view tree`, directly
after the `qt_quick:` commit.

