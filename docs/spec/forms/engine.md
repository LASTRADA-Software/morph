# The forms engine — design

Design spec for `include/morph/forms/engine/`: the runtime model of a schema-driven form, its rules, its exact
values and its reactive session, in C++ and without a toolkit. The emitters (`forms/forms.hpp`) describe an
action as schema JSON; the engine reads that JSON back into a form a user can fill in, and assembles the exact
body the action takes. The program design is spec 2 (`docs/superpowers/specs/2026-10-04-forms-engine-design.md`).

Read this before changing a header under `forms/engine/`. The headers here are the headless half of spec 2:
the model, rules, values, `FormSession`, the submitters and `Form<A>`. Rendering a session through the view tree
(`formView`, overrides), collections, wizards and the app shell are not in the tree yet.

## Contents

- [Packaging](#packaging)
- [The field model](#the-field-model)
- [Rules](#rules)
- [Values and exactness](#values-and-exactness)
- [FormSession](#formsession)
- [Submitters and typed forms](#submitters-and-typed-forms)
- [Parity with the QML renderer](#parity-with-the-qml-renderer)
- [Design decisions](#design-decisions)

## Packaging

Header-only, in the base `morph` target:

| Header | Holds |
|---|---|
| `field_model.hpp` | `FieldKind`, `SubmitMode`, `FieldSpec`, `FieldGroupSpec`, `FormModel` |
| `rules.hpp` | `SchemaError`, `Tri`, `RuleOperand`, `RuleExpr`, the gating and presentation verdicts |
| `values.hpp` | `FieldError`, `WireValue`, `Encoding`, `ValueContext`, `encodeScalar`, `decodeScalar`, `convertDraft`, `parseOptions` |
| `form_session.hpp` | `Submitter`, `ChoiceFetcher`, `FormSessionOptions`, `FieldState`, `RowState`, `FormSession` |
| `handler_submitter.hpp` | `handlerSubmitter`, `handlerChoiceFetcher` |
| `typed_form.hpp` | `Form<A, M, S>`, and the definition of `FormModel::forAction<A>()` |
| `detail/text.hpp` | JSON reading and quoting, exact decimal text, ISO-8601 wall-clock text |

The engine depends on `morph::reactive` and on the UI document's values and expressions
(`ui/value.hpp`, `ui/expression.hpp`), never on a renderer. Only `typed_form.hpp` and `handler_submitter.hpp`
include the schema emitters and the bridge; a controller that needs a `FormSession` over served schema JSON
compiles in no action type. `BlankAs` lives in `forms/blank_as.hpp` so both sides can name it.

`render::parseDecimal` (`render/locale_format.hpp`) is the one decimal entry parser: locale text through
`normalizeLocaleNumber`, then the digits to an exact `math::Rational`, refusing more places than declared
rather than rounding, and refusing what does not fit `Rational`.

## The field model

`FormModel::fromSchema(actionType, schemaJson) -> std::expected<FormModel, SchemaError>` parses the text with
`ui::parseJson` (integers keep every digit; nesting deeper than 64 levels is refused), resolves `$ref` and
nullable `anyOf`/`oneOf`, and produces one `FieldSpec` per property in `x-order` (a stable sort, so key order in
the text does not matter).

| `FieldKind` | Read from | Wire encoding |
|---|---|---|
| `Text`, `Multiline` | `string`; `x-widget: textarea` | string |
| `Integer` | `integer` | the digits typed, leading zeros dropped |
| `Number` | `number` (`x-displayDecimals` caps places) | a JSON number of the digits typed |
| `Decimal` | `x-decimalPlaces` without `ExtUnits` | `{num, den, dp}` |
| `Quantity` | `x-decimalPlaces` with `ExtUnits` (+ `x-unitAlternatives`) | `{num, den, dp}` in the canonical unit |
| `Boolean` | `boolean` | `true` / `false` |
| `Enum` | `enum`, or `const` branches of `anyOf`/`oneOf` | the option's literal |
| `Choice` | `x-optionsAction`, `x-optionValue`, `x-optionLabel`, `x-optionsDependsOn` | the option's literal |
| `DateTime`, `Date` | `format: date-time` / `date` | ISO 8601 UTC / `YYYY-MM-DD` |
| `EpochDateTime`, `EpochDate` | `x-widget: epochSeconds` / `epochDays` on an integer | int64 seconds / days since the epoch, UTC |
| `Slider` | `x-widget: slider` with `x-min` and `x-max` | integer (number for a `number` type) |
| `Array` | an array of scalars, entered comma-separated | a JSON array of the item type |
| `Object` | a nested object | a nested object |
| `ObjectArray` | an array of objects | an array of nested objects |

A `FieldSpec` also carries the label, help, placeholder and i18n key; `required` and `x-blankAs`; `x-readonly`,
`x-hidden`, `x-computed`'s inputs; exact bounds (`minimum`, `maximum`, `multipleOf`, `x-exactMinimum`,
`x-exactMaximum`) and instance bounds (`x-minimum`, `x-maximum`); places; the units with their exact factors;
the enum options; the Choice descriptor; the section and column span; the slider range; and the children of an
`Object` or `ObjectArray`. Nesting is described to `kMaxNestingDepth` (4) levels and stops where a `$ref` chain
repeats, so a self-referential type terminates; a member past either limit is `truncated`. The model carries the
groups (`x-layout.groups`, then an implicit trailing group for fields no section claims), the parsed rules and
the submit mode (`x-submitMode: explicit`, else automatic).

`FormModel::forAction<A>()` is `fromSchema(ActionTraits<A>::typeId(), schemaJson<A>())`, the same reader, so a
typed and a runtime form of one action are one model. It is declared in `field_model.hpp` and defined in
`typed_form.hpp`; it throws `std::logic_error` when the emitted schema does not read, which is a defect, never
bad input.

## Rules

`x-rules` is read into `RuleExpr` trees: the 16 kinds the emitters write, and any other kind as `Unknown`, so an
older client never refuses a newer server's form.

**One evaluator.** Each rule is translated, when the model is read, into the UI document's expression language
(`expr/1`, `docs/spec/ui/expressions.md`) and evaluated with `ui::evaluate`. The client therefore has one
evaluator for rules, relations and checks, and the rules inherit its exact arithmetic and its Kleene logic.

**Operands.** A rule's fields are numbered `f0`, `f1`, … (so a wire name need not be a document name), and each
reads as two names:

| Name | Blank draft | Draft that does not encode | Valid draft |
|---|---|---|---|
| `fN.engaged` | `false` | `null` | `true` |
| `fN.value` | `null` | `null` | the exact value: `int`, `decimal`, `bool`, `string` or `timestamp` |

Both are needed because a blank operand and an invalid one are both "no value" to the expression language, and
the rules treat them differently.

**Translations.**

| Kind | Holds when |
|---|---|
| `engaged`, `notEngaged` | `fN.engaged`, `not(fN.engaged)` |
| `equals` | `if(fN.engaged, eq(fN.value, literal), false)`; the literal is a bool, a string, an `int` (from `valueText` when the emitter wrote the digits) or a decimal |
| `greater`, `greaterOrEqual`, `less`, `lessOrEqual` | `if(and(fa.engaged, fb.engaged), gt(fa.value, fb.value), true)`: vacuous while either is blank, `null` while either is invalid or the kinds do not order |
| `exactlyOneOf`, `atLeastOneOf`, `mutuallyExclusive` | counts of the definite (`filter` on `engaged`) and the open (`not(engaged(x))`) operands; `null` only while the open count could change the answer |
| `and`, `or`, `not` | the functions of the same names, three-valued |
| `requiredWhen` (gate) | `if(coalesce(when, false), and(f.engaged…), true)`: every named field must be engaged while the condition is definitely true |
| `visibleWhen`, `readonlyWhen`, `requiredWhen` (presentation) | the `when` clause |
| unknown kind, or a presentation kind used as a condition | `null` |

**Verdicts.** `null` is `Tri::Unknown`. `gatingVerdict` blocks readiness only on `False`: an `Unknown` gate waits
on a field that is required or does not encode, which blocks through readiness itself, and a kind this engine does
not know must not lock a form. `visibleVerdict`, `readonlyVerdict` and `requiredVerdict` combine every
presentation rule that **names** the field (membership, not only the first field): hidden as soon as one
`visibleWhen` is false, frozen and required as soon as one `readonlyWhen` / `requiredWhen` is true; `Unknown`
presents as visible, editable and not required.

**Limits.** An integer beyond int64 reaches the body with every digit but is `null` to a rule (the expression
language has no wider int), so a comparison with it is `Unknown`. A translation larger than `expr/1`'s 256 nodes
is a `SchemaError`.

## Values and exactness

`encodeScalar(spec, draft, unit, context) -> Encoding` trims the draft; blank is `std::nullopt` (a required
`Boolean` encodes `false`: an unchecked box is an answer); otherwise the `WireValue` (the literal, byte for byte,
and the `ui::Value` rules compare) or a `FieldError` with a stable code — `malformed`, `too-precise`,
`below-minimum`, `above-maximum`, `not-a-multiple`, `not-an-option`, `out-of-range`, `incomplete`,
`unrepresentable` — and its message resolved through the translations (`morph.forms.error.<code>`).

- Integers compare as digit strings against the exact bounds (`x-exactMinimum` wins over `minimum`), so nothing
  passes through a double.
- A `Decimal` or `Quantity` typed in unit *u* (decimals *d*, factor *n/m* to the canonical unit) encodes the
  unreduced `{num: digits·10^(d−places)·n, den: 10^d·m, dp: canonical places}` — the bytes the QML renderer sends.
  Bounds, instance bounds and `multipleOf` are checked exactly in the canonical unit.
- A `Number` is the canonical text typed, gated by its bounds and `x-displayDecimals`.
- A date-time is the wall clock typed in the display zone, sent as UTC.
- `x-blankAs: empty` sends `""` for a blank field once it was filled (or prefilled with a string); `omit` leaves it
  out.

`decodeScalar` is the inverse, for prefill: numbers in the display locale without grouping, instants in the
display zone. `convertDraft` converts a draft between units exactly, rounding half away from zero to the target's
decimals. `parseOptions` reads option rows: the reply if it is an array, else its first array member; values keep
every digit.

## FormSession

`FormSession(runtime, model, submit, choices, options)` is the reactive state of one form, a controller in the
sense of `docs/spec/reactive/control.md`. Non-copyable and non-movable: fields and in-flight replies point into
it. A view mounted on it is destroyed first.

- **Fields.** `FieldState`: the `text`, `unit` and `engaged` signals (a write to `text` is a user edit); the
  `encoded`, `error`, `visible`, `readonly` and `required` computeds; the resolved label, help and placeholder.
  `field(path)` takes `name`, `parent.member` and `rows[i].member`.
- **Nesting.** An `Object` owns a `FieldState` per member and encodes them with their own encoders; a blank
  optional object is left out, a blank required member makes it `incomplete`. An `ObjectArray` owns `RowState`s
  with stable keys (`addRow`, `removeRow`), encodes `[]` once engaged or while required, and a truncated member
  holding a value is `unrepresentable`.
- **Readiness.** Every required field encodes, no field has an error, no gating rule is `False`, and
  `options.accepts` (when set) accepts the body. `body()` is the JSON text in `x-order` while ready.
- **Submission** is an `Exclusive` `reactive::Mutation` over the `Submitter`: `pending()`, `lastReply()`,
  `lastError()` and `successCount()`, which ticks once per success. In automatic mode an Effect submits whenever
  the form becomes ready with a body it has not sent; in explicit mode only `submit()` does. A form already ready
  when it is built submits once in automatic mode.
- **Programmatic changes** — `prefill(bodyJson)`, `assign(path, json)`, `reset()`, and clearing a stale Choice
  selection — record the body they produce as already seen, so they never submit; the next user edit does.
- **Choice options** are a `reactive::Query<std::string, std::string>` per Choice, keyed on the options request:
  `{}` for an independent Choice, `{"parent": <literal>, …}` once every parent encodes, idle while one does not.
  A parent change refetches, and a reply for a superseded request is dropped. A selection the answered options no
  longer contain is cleared (`clear`, the default of spec 5's `onStale`), but only once the query has answered for
  the current parents, so a selection prefilled before its options arrive survives. An idle query, and a failed
  fetch, keep the selection.
- **Automatic submission waits for options in flight.** While any top-level Choice is fetching, its selection is
  unjudged — a parent just changed — so the automatic Effect does not submit; the settled query wakes it, after
  a stale selection has been cleared.
- **Groups.** `groupCollapsed(i)` and `tabSelection(i)` are per-group signals a view binds; `groupTitle(i)`
  resolves `<action>.group.<i>` through the translations.

Labels resolve the explicit `x-i18nKey` first, then (top level only) the derived `<action>.<field>.<slot>`, then
the schema's literal (`render::resolveText`).

## Submitters and typed forms

- `handlerSubmitter(callbacks, handlers…)` and `handlerChoiceFetcher(callbacks, handlers…)` route an action type
  to the first `BridgeHandler` whose model registers it (`servesAction`) and run it through `executeJson`; an
  action none serves fails with `std::invalid_argument` on `callbacks`. A form then reaches the model instance the
  rest of the screen uses.
- `Form<A, M, S>(runtime, handler, choices, options)` owns a `FormSession` over `FormModel::forAction<A>()`: the
  body is decoded into `A` and executed through the handler, so the result is typed. `ActionValidator<A>` joins
  readiness through `accepts`; `set<&A::member>(v)` is a programmatic change; `value()` is the body decoded;
  `lastResult()` is the session's last reply decoded, so the typed result and the JSON reply change together.

## Parity with the QML renderer

Measured, not asserted, by `tests/test_forms_engine_corpora.cpp` and the corpus cases of the existing suites:

- **Rule corpus** (`tests/data/rule_corpus.json`, 46 rows): every row's `ready`, `visible` and `readonly`, and the
  engine's `ready` against `allRulesSatisfied` on the same row. The QML suite reads the same file.
- **Body corpus** (`tests/data/body_corpus.json`): each case is a schema, a locale, a display offset, an optional
  prefill and the text typed per field; its body is what `DynamicForm` sent, written by
  `src/qt/forms/tests/tst_BodyCorpus.qml` (`MORPH_FORMS_WRITE_BODY_CORPUS=1` regenerates; otherwise the suite
  requires the committed bodies). The engine must send the same bytes, case by case.
- **Instance bounds and conformance**: every row of `tests/data/instance_bounds.json` and every conformance fixture
  through a session.

### Differences from the QML renderer

Each is a named case of the body corpus, or a test of the engine's own suite:

| Behaviour | QML renderer | Engine | Why |
|---|---|---|---|
| Integer array items | quoted strings | integers | the action's decoder refuses `["1"]` for `std::vector<int>` |
| Nested objects, collections of objects | unrepresentable unless a slot claims them | fields with their own encoders | spec 2: nothing is unrepresentable |
| A blank parent of a dependent Choice | clears the child's selection | keeps it (the query is idle) | no options were fetched to judge it against |
| An operand whose draft does not encode, for `engaged` | engaged (text present) | `Unknown` | a value the server would refuse does not satisfy a condition |
| A presentation rule naming several fields | the first field | every field it names | membership |
| Automatic submission while Choice options are in flight | submits the body with the unjudged selection | waits for the options | the body could carry a selection the new parent does not offer |

## Design decisions

| Decision | Why |
|---|---|
| Rules through `expr/1`, not an evaluator of their own | One evaluator on the client for rules, relations and checks; its three-valued logic and exact comparisons are what the rules need |
| Two names per operand (`engaged`, `value`) | A blank and an invalid operand are both `null` as values, and the rules distinguish them |
| Gates block only on `False` | `Unknown` comes from a field that already blocks, or from a rule kind an older client does not know |
| The body as JSON text assembled in `x-order` | The bytes are the contract the parity corpus compares; a DOM round trip would reorder keys and could round numbers |
| `forAction` declared in the model, defined with the emitters | A runtime form reads served JSON and needs no action type; only typed code pays for the emitters |
| Programmatic changes mark their body as seen | A prefill, a reset or a cleared stale selection is not a user's request to submit |
