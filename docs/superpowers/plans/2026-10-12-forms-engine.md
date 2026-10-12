# Declarative UI, part 7 — the C++ forms engine: implementation plan

**Issue:** #892 (part 7 of #885). **Spec:** spec 2, `docs/superpowers/specs/2026-10-04-forms-engine-design.md` (all
of it; packaging §9, parity and tests §10), read against what `master` holds: the reactive core
(`include/morph/reactive/`), the UI document and its expression language (`include/morph/ui/expression.hpp`,
`value.hpp`, spec 5 §4 and §6), the view tree and `RecordingBackend` (`ui/view.hpp`, `ui/testing/`), the Qt Quick
backend (`include/morph/qt_quick/`), and the QML renderer the engine replaces (`src/qt/forms/`). Spec 6 §7 for the
workspace shell.

## What planning found

1. **The part is three reviewable pull requests, not one.** Spec 2 re-implements about 4,400 lines of QML/JS. The
   old plan for it (`2026-10-04-declarative-ui-tui-5-forms-engine.md` at `29ca9b1f`) carries roughly 7,000 lines of
   header and test code for the field model, rules, values and sessions alone, before rendering, collections,
   wizards and the app shell. Split by what each layer depends on:
   - **(a) this PR — the headless engine.** `parseDecimal`, the field model, rules through `expr/1`, values,
     `FormSession` (nested objects, collections, Choice options), `handlerSubmitter`, `Form<A>`, and the parity
     that needs no renderer: the 46-row rule corpus, the conformance, instance-bounds and locale corpora, and the
     body corpus written by the QML renderer itself. `Part of #892`.
   - **(b) rendering.** `formView`, `Overrides`, the field overlay and the grid widget, lowering `form` nodes in the
     interpreter (with the `{"ref": "<form>.<field>.value"}` element values), every QuickTest view claim on
     `RecordingBackend`, and a form document mounted on the terminal and the Qt Quick backends.
   - **(c) composites.** `collection.hpp`, `wizard.hpp`, `app_shell.hpp`, `workspace.hpp` and the interpreter's
     `collection` and `wizard` nodes; that pull request is the one that completes the issue.
2. **(b) cannot start on this branch.** Lowering a `form` node is a change to `include/morph/ui/interpreter.hpp`,
   which an open pull request also edits; (b) is planned after that lands. Nothing in (a) touches the interpreter.
3. **Rules go through the one evaluator.** The old plan evaluates `x-rules` with its own three-valued evaluator.
   Spec 2 §3 says each rule is translated into `expr/1`, so a client has one evaluator for rules, relations and
   checks. `expr/1` has what the rules need — Kleene `and`/`or`/`not`, exact `int`/`decimal` comparison, `if`,
   `count` and `filter` — so (a) translates each rule into an `expr/1` tree at model load and evaluates it with
   `ui::evaluate`. Two facts the translation has to carry that a plain value cannot: a field that is *blank* and
   one whose draft *does not encode* are both "no value", and the rules treat them differently (a blank operand
   makes a comparison hold vacuously; an invalid one makes it `Unknown`). Each operand therefore reads as two
   names: `fN.engaged` (`true`, `false`, or `null` for an invalid draft) and `fN.value` (the encoded value, `null`
   when not valid).
4. **`field_model.hpp` must not include the schema emitters (§9).** The old plan's `FormModel::forAction<A>()` and
   its `detail/text.hpp` include `forms/forms.hpp`. `forAction` moves to `typed_form.hpp`; the engine's JSON helpers
   read the DOM themselves; `BlankAs` moves from `forms.hpp` into its own small header, which both include.
5. **`parseDecimal` lives in `render/locale_format.hpp`** (spec 2 §4), not in `util/rational.hpp` as the old plan
   put it, so `util/` keeps no dependency on `render/`.
6. **Spec 2 keyword names.** The emitter writes `x-optionValue` / `x-optionLabel`, which is what spec 2 §2 says; no
   change.
7. **`EpochDateTime` and `EpochDate`** (spec 2 §2) are newer than the QML renderer, which has no such kinds. The
   engine reads them (an integer with `x-widget: epochSeconds` / `epochDays`); there is no QML parity to measure.

## Split

| | Holds | Parity it measures |
|---|---|---|
| **(a)** | `field_model.hpp`, `rules.hpp`, `values.hpp`, `form_session.hpp`, `handler_submitter.hpp`, `typed_form.hpp`, `detail/text.hpp`; `parseDecimal` | rule corpus (46 rows: `ready`, `visible`, `readonly`, typed-vs-runtime agreement); body corpus byte-identical to the QML renderer; conformance, instance-bounds and locale corpora; the QuickTest claims about values, sessions, Choice options, nesting and rules |
| **(b)** | `form_view.hpp`, `overrides.hpp`; interpreter lowering of `form` | the QuickTest claims about rendering and overrides on `RecordingBackend`; a form document on the terminal and Qt Quick backends |
| **(c)** | `collection.hpp`, `wizard.hpp`, `app_shell.hpp`, `workspace.hpp` | the QuickTest claims about collections, wizards and view chrome |

## Design (a)

### Packaging

Header-only in the base `morph` target, under `include/morph/forms/engine/`; the detail header in
`morph_detail_headers`. `rules.hpp`, `field_model.hpp`, `values.hpp` and `form_session.hpp` include `morph::reactive`
and `ui/expression.hpp`/`ui/value.hpp` (the evaluator and its values), never a renderer and never the emitters;
`typed_form.hpp` and `handler_submitter.hpp` include the emitters and the bridge.

### The field model (`field_model.hpp`)

`FormModel::fromSchema(actionType, schemaJson) -> std::expected<FormModel, SchemaError>` reads glaze's
integer-preserving DOM, resolves `$ref` and nullable `anyOf`/`oneOf`, and produces one `FieldSpec` per property in
`x-order`: the kinds of spec 2 §2 (including the epoch kinds), required, `x-blankAs`, read-only, hidden, computed,
exact bounds, instance bounds, places, units with exact factors, enum options, the Choice descriptor, layout, slider
range, item type, and children for `Object`/`ObjectArray` (recursion bounded by `kMaxNestingDepth` and by the `$ref`
chain, so a cycle stops). The model carries the groups (`x-layout`, with the trailing implicit group), the parsed
rules and the submit mode.

### Rules (`rules.hpp`)

- `RuleExpr::parse` reads the 16 kinds; an unknown kind parses and is `Unknown`, so an older client never refuses a
  newer server's form.
- Each rule is translated at parse time into an `expr/1` tree (`RuleExpr::expression()`), with its field names
  mapped to `f0`, `f1`, … (so a wire name need not be a document name). Gating rules translate to their test
  (`requiredWhen` to "the condition is not true, or every named field is engaged"); presentation rules to their
  `when` clause. Comparison: `if(and(fa.engaged, fb.engaged), gt(fa.value, fb.value), true)`. Membership:
  `count(filter(list(e…), x))` for the definite and `count(filter(list(e…), not(engaged(x))))` for the open
  operands, combined as the kind says.
- Evaluation: `RuleExpr::evaluate(lookup) -> Tri` runs `ui::evaluate` over an environment that answers `fN.*` from
  a `RuleLookup` (the session's tracked field reads); `null` is `Unknown`. `gatingVerdict` blocks only on `False`;
  `visibleVerdict`, `readonlyVerdict` and `requiredVerdict` combine every presentation rule that *names* the field
  (membership, not only the first field).

### Values (`values.hpp`)

`encodeScalar(spec, draft, unit, context) -> Encoding` (`nullopt` blank, else `expected<WireValue, FieldError>`),
`decodeScalar` (the inverse, for prefill), `convertDraft` (exact unit switching), `parseOptions` (a reply array or its
first array member), `defaultPlaceholder`. A `WireValue` holds the exact JSON literal the body carries and the
`ui::Value` rules compare. Integers keep every digit; decimals and quantities are `{num, den, dp}` in the canonical
unit, unreduced as the QML renderer sent them; date-times convert from the display offset to UTC. Errors carry
stable codes and i18n'd messages.

### `FormSession` (`form_session.hpp`)

As spec 2 §5: per-field `text`, `unit`, `engaged` signals; `encoded`, `error`, `visible`, `readonly`, `required`
computeds; readiness; the body in `x-order`; submission as an `Exclusive` `reactive::Mutation` with
`successCount()`; automatic mode submits on a ready, changed body except while a programmatic change applies;
`prefill`, `assign`, `reset`; nested objects and collections with stable row keys; a Choice's options as a
`reactive::Query` keyed on its parents' encodings, a superseded reply dropped, a stale selection cleared once the
query has answered for the current parents (`onStale` defaults to `clear`; the overlay that sets it is (b)'s).

### Submitters and typed forms

- `handlerSubmitter(handlers…)` / `handlerChoiceFetcher(handlers…)`: route an action type to the application's own
  `BridgeHandler` that serves it, decoding the body into the action and encoding the reply, so tests and local
  tools run a form without a remote.
- `Form<A, M, S>`: builds `FormModel::forAction<A>()`, owns a `FormSession` over the handler, adds
  `ActionValidator<A>` to readiness, `set<&A::member>(v)`, a typed `value()` and `lastResult()`.

## Parity (a)

- **Rule corpus.** `rule_corpus.json` moves to `tests/data/`; the Catch2 corpus test and the QML suite both read it
  there. A new case drives every row through a `FormSession` and requires the row's `ready`, `visible` and
  `readonly`, and a second requires the runtime verdict to equal `allRulesSatisfied` on the same row.
- **Body corpus.** `tests/data/body_corpus.json` holds rows `{schema, typed texts (and unit switches, prefill),
  locale, display offset, body}`. A QuickTest drives a real `DynamicForm` through every row and writes the body it
  produces when regenerating (`MORPH_FORMS_WRITE_BODY_CORPUS`), and otherwise requires the committed body — so the
  expected bodies are the QML renderer's output, and a QML change that alters one fails there. The engine's test
  requires byte-identical bodies for every row. Rows: exact `{num, den, dp}`, int64 beyond 2^53, unit conversion,
  `x-blankAs`, enums, booleans, date-time zones, arrays, nested objects and collections, locale entry.
- **Conformance, instance-bounds and locale corpora**: every fixture and row through a session, read from the shared
  data files.
- **QuickTest claims.** The plan's parity map (below) names, per QuickTest file, the engine test that carries each
  claim (a), or the part that will ((b), (c)).

### QuickTest parity map

| QuickTest file | Carried by |
|---|---|
| `tst_conformance.qml`, `tst_conformance_negative.qml` | (a) `test_forms_conformance_corpus.cpp`, session readiness cases |
| `tst_conformance_accessibility.qml` | (b) |
| `tst_dynamicform.qml` | (a) model, text, values, session, choice cases; (b) layout runs and tab switching |
| `tst_DynamicFormArrayField.qml`, `…BlankAs`, `…BooleanAndAnyOf`, `…EnumChoice`, `…ExactBounds`, `…FieldBounds`, `…PlainNumber`, `…SchemaAsVariant`, `…InstanceBounds` | (a) values and session cases, body corpus |
| `tst_DynamicFormChoicelessController.qml`, `…Reactive.qml`, `…Prefill.qml`, `tst_LargeIdPrecision.qml` (form and Choice parts) | (a) session and choice cases |
| `tst_DynamicFormNestedAggregate.qml`, `…ObjectSlot.qml`, `…ObjectArraySlot.qml` | (a) nested cases (encoding, prefill, rows); (b) the panels and row views |
| `tst_DynamicFormRuleAgreement.qml`, `…RuleCorpus.qml`, `…Rules.qml`, `…UnknownRuleKind.qml` | (a) rules cases, rule corpus; (b) visibility and read-only on mounted cells |
| `tst_DynamicFormSubmitMode.qml`, `…ReseedSubmit.qml` | (a) automatic and explicit submission, reset; (b) the Submit button |
| `tst_i18n.qml` | (a) labels, error messages, `parseDecimal` over the locale corpus; (b) rendered labels |
| `tst_DynamicFormChrome.qml`, `…DisplayUnit.qml`, `…GridColumns.qml`, `tst_slot_registry.qml`, `tst_SlotRegistryByKind.qml` | (a) the model facts (kinds, units, decimals); (b) overrides and grid |
| `tst_collectionview.qml`, `tst_wizardview.qml`, `tst_ViewChrome.qml`, `tst_LargeIdPrecision.qml` (collection part) | (c) |

What no engine test carries, and why: Qt's own locale facts (`tst_i18n.qml`'s `qt…` cases measure `Qt.locale`; the
locale corpus they produced is what `parseDecimal` is run over); QML-only mechanics (`Connections` warnings, a
schema assigned as a JS object, the renderer's internal indexes).

## Tasks (a)

1. This plan.
2. `render::parseDecimal`, with its tests over the multi-unit and digit-set locale corpora.
3. `BlankAs` in its own header; `detail/text.hpp`; `field_model.hpp`; `rules.hpp` with the `expr/1` translation;
   their tests.
4. `values.hpp` and its tests.
5. `form_session.hpp` (scalar fields, readiness, submission, prefill, reset), then nested objects and collections,
   then Choice options; the test support (a fake server, a harness) and the session, nested and choice tests.
6. The corpora: move the rule and instance-bounds corpora to `tests/data/`, drive them, the conformance fixtures and
   the agreement test through the engine.
7. The body corpus: the QuickTest that writes and checks it, the committed corpus, and the engine test.
8. `handler_submitter.hpp`, `typed_form.hpp` and their tests.
9. Docs: `docs/spec/forms/engine.md` (new), the engine pointers in `forms.md`, the spec index, ARCHITECTURE,
   CHANGELOG.

## Verification

- GCC Debug (`cmake --preset gcc-debug`, target `morph_tests`, `-j 8`), run `[forms-engine]` and the existing
  `[forms]` cases; the Qt forms QuickTest suite for the body corpus (`MORPH_BUILD_FORMS_QML=ON`,
  `QT_QPA_PLATFORM=offscreen`).
- Mutation checks, each confirmed to fail a named test and then restored: a comparison rule's vacuous-blank branch;
  the membership threshold; the presentation match by membership; the Choice "answered for the current parents"
  guard; the required-empty-collection encoding; a digit of the Quantity scaling; the automatic-submission
  programmatic guard.
- `clang-format --dry-run --Werror` on every touched file; Doxygen with `WARN_AS_ERROR`.
- Not verified locally: clang-tidy, MSVC and the WebAssembly build (CI).

## Slice (c), split again

Slice (c) lands as two pull requests, both after (a):

- **(c1) the headless composites:** `collection.hpp` and `wizard.hpp` as models and sessions, no view and no
  interpreter node. `CollectionModel` reads the `v-*` document (with each column's table `kind`);
  `CollectionSession` lists rows through a `Query` on `v-query`, keeps a `table::VectorSource` of them in sync so
  sorting, filtering and selection are the table engine's, runs row and collection actions as one `Mutation`
  whose bodies are built from `bind` with the row's exact JSON values, holds a `confirm` action until it is
  answered, and opens the row editor as a `FormSession` reset and prefilled from the row; an editor success
  (`successCount`) closes it and refetches. `WizardModel` reads `w-*`; `WizardSession` keeps one `FormSession` per
  step, records each success's body and reply into the resolved values (draft first, the reply winning), applies
  a step's prefill as JSON on entry, and counts a step done only while its body is the one that succeeded, so
  returning to a done step submits nothing and a changed body is resubmitted by Next.
- **(c2) the rest:** `collectionView` and `wizardView` (they need (b)'s `formView`), the interpreter's
  `collection` and `wizard` nodes, and `app_shell.hpp`. `app_shell` mounts screens by document id, which is the
  host contract #933 introduces; it waits for that.

`workspace.hpp` (spec 6 §7) hosts screens of every kind, not forms, and its contract is `ui/host.hpp`; it
belongs with the hosting part (#893), not here. Spec 2 §9 lists it under the forms engine and is corrected in
the PR that builds it.

Parity of (c1): every QuickTest claim about collections and wizards that needs no view is re-expressed
headlessly (`tst_collectionview.qml`, the collection half of `tst_LargeIdPrecision.qml`, `tst_wizardview.qml`, and
the confirm-gate cases of `tst_ViewChrome.qml`). The chrome, dialog-visibility and master-detail layout claims
move to (c2) with the views.
