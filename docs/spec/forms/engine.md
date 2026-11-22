# The forms engine — design

Design spec for `include/morph/forms/engine/`: the runtime model of a schema-driven form, its rules, its exact
values and its reactive session, in C++ and without a toolkit. The emitters (`forms/forms.hpp`) describe an
action as schema JSON; the engine reads that JSON back into a form a user can fill in, and assembles the exact
body the action takes.

Read this before changing a header under `forms/engine/`. The headers here are the model, rules, values,
`FormSession`, the submitters, `Form<A>`, the collection and wizard sessions, and the rendering (`formView`,
overrides, the field overlay, `collectionView`, `wizardView`) that a document's `form`, `collection` and `wizard`
nodes reach through the interpreter. The app shell is not part of the engine: it mounts screens by document id,
which is the host's contract ([../ui/host.md](../ui/host.md)).

## Contents

- [Packaging](#packaging)
- [The field model](#the-field-model)
- [Rules](#rules)
- [Values and exactness](#values-and-exactness)
- [FormSession](#formsession)
- [Submitters and typed forms](#submitters-and-typed-forms)
- [The field overlay](#the-field-overlay)
- [Rendering](#rendering)
- [Derived values](#derived-values)
- [Collections](#collections)
- [Wizards](#wizards)
- [Rendering collections and wizards](#rendering-collections-and-wizards)
- [Corpora](#corpora)
- [Design decisions](#design-decisions)

## Packaging

Header-only, in the base `morph` target:

| Header | Holds |
|---|---|
| `field_model.hpp` | `FieldKind`, `SubmitMode`, `FieldSpec`, `FieldGroupSpec`, `FormModel` |
| `rules.hpp` | `SchemaError`, `Tri`, `RuleOperand`, `RuleExpr`, the gating and presentation verdicts |
| `values.hpp` | `FieldError`, `WireValue`, `Encoding`, `ValueContext`, `encodeScalar`, `decodeScalar`, `convertDraft`, `parseOptions` |
| `form_session.hpp` | `Submitter`, `ChoiceFetcher`, `FormSessionOptions`, `FieldState`, `RowState`, `FormSession`; its rounds of derived values use `forms/derive.hpp`'s `DeriveOptions`, `DeriveRound`, `DeriveLoad` and `DeriveState` |
| `handler_submitter.hpp` | `handlerSubmitter`, `handlerChoiceFetcher` |
| `typed_form.hpp` | `Form<A, M, S>`, and the definition of `FormModel::forAction<A>()` |
| `overrides.hpp` | `FieldView`, `Overrides` |
| `field_layout.hpp` | `FieldLayout` (shared with the authoring API, which names it without including the engine) |
| `form_view.hpp` | `FormViewOptions`, `formView` |
| `collection.hpp` | `CollectionModel`, `CollectionColumn`, `CollectionAction`, `CollectionRow`, `PendingConfirm`, `SchemaLookup`, `CollectionSession` |
| `wizard.hpp` | `WizardModel`, `WizardStepSpec`, `WizardSession` |
| `collection_view.hpp` | `CollectionChrome`, `CollectionViewOptions`, `collectionView` |
| `wizard_view.hpp` | `WizardChrome`, `WizardViewOptions`, `wizardView` |
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
`x-hidden`, `x-computed` and its inputs, `x-derived`; exact bounds (`minimum`, `maximum`, `multipleOf`, `x-exactMinimum`,
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
(`expr/1`, [../ui/expressions.md](../ui/expressions.md)) and evaluated with `ui::evaluate`. The client therefore has one
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
  unreduced `{num: digits·10^(d−places)·n, den: 10^d·m, dp: canonical places}` — the bytes the body corpus pins.
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
sense of [../reactive/control.md](../reactive/control.md). Non-copyable and non-movable: fields and in-flight replies point into
it. A view mounted on it is destroyed first.

- **Fields.** `FieldState`: the `text`, `unit` and `engaged` signals (a write to `text` is a user edit, as is
  `edit(json)`, a load of a wire value the session does not mark programmatic, refused for an object field given
  anything but an object and a list field given anything but an array); the
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
- **Programmatic changes** — `prefill(bodyJson)`, `refill(bodyJson)`, `assign(path, json)`, `reset()`, and
  clearing a stale Choice selection — record the body they produce as already seen, so they never submit; the
  next user edit does.
- **Edits since the last load.** The session keeps each top-level field's snapshot (its draft, unit and
  engagement, and its members' and rows') as `prefill`, `reset` or `assign` last left it, and as it was built.
  `refill(bodyJson)` loads like `prefill`, except that a top-level field whose snapshot differs (the user changed
  it since) keeps what was entered. It is for a later value of the record already being edited; `prefill` is for
  another record. A successful submission, automatic or explicit, ends the edit of every field still holding what it
  sent, so the record's later values reach those; a field changed after the submission was sent, or one whose
  submission failed, stays an edit. An automatic form submits while the person may still be typing, which two rules
  make safe. First, the record's form of the value just sent does not load: a saved scalar takes the record's value
  only when it would encode differently from the draft, so a typed `1` stays `1` when the record answers `1.000`
  (the comparison is the encoded body entry, not the text). Second, the snapshot a success is judged against is the
  one taken when that submission went out, so text typed while it was in flight is never mistaken for it, even when a
  following submission starts in the same turn as the reply. The engine has no focus, so "still typing" is "changed
  since the last send", which is exactly what the snapshots record. An automatic form's object and collection do not
  end their edit on a save: their members and rows cannot be compared with the record without rebuilding them under
  the cursor, so they own what was entered until the form is rebuilt or loaded with `prefill`. A member the record leaves out loads blank into a field that is no longer an edit, as it does in `prefill`.
- **Formatting follows the screen.** `setFormat(locale, datePattern, dateTimePattern)` on a `FormSession` (and so on a
  `WizardSession`'s steps and a `CollectionSession`'s editor) replaces the number locale and the date patterns of a
  built form. The number locale is read by every field's encoding, so each re-encodes its draft at once and the body
  follows; the drafts keep the text as typed, so `1,5` typed under a comma locale reads as an error once the locale
  switches to a point. The date inputs' `pattern` is read tracked and follows. The screen's interpreter keeps an effect per
  form, collection and wizard that applies the session's current formatting, and holds the facts it applied, because the
  number locale is a set of views into them. The display zone is read once, at build.
- **Texts follow the screen.** `setTranslations(provider, bcp47)` on a `FormSession` (and so on a `WizardSession`'s
  steps and a `CollectionSession`'s editor) replaces the catalogue and the locale of a built form. A field's `label()`,
  `help()` and `placeholder()`, `groupTitle(i)`, a wizard's `title()` and `stepTitle(i)`, and the error messages
  (`values()`) resolve through them on each read and are tracked, so a view that binds them shows the new text without
  a remount; `formView` binds every one of them, the control's accessible name and description included. Whether a
  field has a help line at all (`hasHelp()`) is decided untracked when the view is built. The same effect that applies
  the formatting calls `setTranslations` with the session's translations and locale, so a `setLocale` or
  `setTranslations` on the `Session` reaches every mounted form.
- **Choice options** are a `reactive::Query<std::string, std::string>` per Choice, keyed on the options request:
  `{}` for an independent Choice, `{"parent": <literal>, …}` once every parent encodes, idle while one does not.
  A parent change refetches, and a reply for a superseded request is dropped. A selection the answered options no
  longer contain is cleared (`clear`, the default of a `select` node's `onStale`,
  [../ui/document.md](../ui/document.md)), but only once the query has answered for
  the current parents, so a selection prefilled before its options arrive survives. An idle query, and a failed
  fetch, keep the selection. A failed fetch is not repeated on its own: `retryOptions()` fetches again, and opening
  the field's dropdown in `formView` runs it, so options that failed when the form was built (an action that needs
  the screen's own setup first) are fetched once the user asks for them.
- **A Choice member of a collection row** looks up each `x-optionsDependsOn` name among its row's members first, then
  among the top-level fields, so a grade can depend on its row's material and the form's site. Its options are not
  fetched until an editor of it opens (`FieldState::openOptions()`, which a grid's cell editor calls as it opens), so
  a column of 300 rows fetches nothing until a user opens a cell. Every row member's fetch goes through one cache of
  the session (`SessionCore::sharedChoices`): identical `(optionsAction, request)` pairs share one fetch for the
  session's life, a second ask joining the fetch in flight or taking its reply, and a failed fetch is not kept, so
  the next ask fetches again. A column of rows over five materials makes five requests. A top-level Choice keeps its
  own query, unshared.
- **Options narrowed per row.** `FieldState::narrowOptions(values)` keeps only the options whose JSON literal is
  listed (a grid column's `options` overlay); `options()` offers the narrowed list, and nothing restores every option.
  `overridePlaceholder(text)` replaces the placeholder the same way (a column's `placeholder` overlay).
- **Automatic submission waits for options in flight.** While any top-level Choice is fetching, its selection is
  unjudged — a parent just changed — so the automatic Effect does not submit; the settled query wakes it, after
  a stale selection has been cleared.
- **Automatic submission waits for the submission in flight.** The submission is `Exclusive`, so a body ready while
  another is in flight would be refused; the Effect waits instead and sends the latest body when the one in flight
  settles, so typing against a slow server loses nothing.
- **Groups.** `groupCollapsed(i)` and `tabSelection(i)` are per-group signals a view binds; `groupTitle(i)`
  resolves `<action>.group.<i>` through the translations, tracked.

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

## The field overlay

`FieldOverlay` is what a `form` node's `fields` ([../ui/document.md](../ui/document.md)), or an application, changes about a top-level field
before the session is built; `FormModel::fromSchema(action, schema, overlays)` applies it.

- `omit` removes the property from the schema, so the member is out of the form and of the body; a `required`
  entry naming it then marks nothing. `hidden` sets `x-hidden`: in the body, out of the view.
- `label`, `widget` and `blankAs` set `title`, `x-widget` and `x-blankAs` before the schema is read, so a widget
  overlay chooses the kind as the schema's own keyword would (`textarea`, `radio`, `slider`, `epochDays`, …).
- `unit` picks a Quantity's entry unit (`FieldSpec::entryUnit`) among its units, or replaces the unit shown on a
  field without units. A draft starts, and a prefill and a reset return, in the entry unit; the wire value stays in
  the canonical unit.
- `decimals` is `x-decimalPlaces` given by the screen: the canonical unit's entry places and the wire `dp` (another unit of a Quantity keeps its own); a Number's display decimals.
- An overlay naming a field the schema does not have, or a unit that is not one of the field's, is a
  `SchemaError`.

`FormModel::withSubmitMode` is a document's `submit` choosing over `x-submitMode`.

## Rendering

`formView(session, options)` renders a session as a view tree; mount it after the session and destroy the mount
before it.

| Kind | Control |
|---|---|
| Text, Multiline | `TextInput` (single-line, multiline) |
| Integer, Number, Decimal | `TextInput` showing its unit (`unit`), with its entry places (`decimals`: 0 for an Integer, else the chosen unit's, the display places or the declared places) |
| Quantity | `TextInput` showing its unit, as a Decimal's (one unit); with alternatives, a `TextInput` with no unit and a unit `Select` beside it; switching converts the draft exactly |
| Boolean | `Checkbox` labelled with the field |
| Enum, Choice | `Select` (dropdown, or radio for `x-widget: radio`) keyed by each option's JSON literal; a Choice adds a `Busy` while its options load |
| DateTime, EpochDateTime; Date, EpochDate | `DateTimeInput` (date-time in the display zone; date); the input shows and reads `FormSessionOptions::datePattern` or `dateTimePattern` while they are set (`ScreenCore::engineOptions` fills them from the session's formatting), and the draft text stays ISO; the input follows `FormSession::setFormat`, which the screen calls whenever its formatting changes |
| Slider | `Slider` over `x-min`..`x-max` by `x-step` |
| Array | `TextInput`, comma-separated |
| Object | `Panel` of its members' cells |
| ObjectArray | a `forEach` of row panels, each with Remove, and an Add button; with `x-widget: grid` (or an overlay's `widget: grid`), a grid of its rows (`ui::formGrid`, [../table/editing.md](../table/editing.md#a-grid-of-a-form)), one column per member in schema order |

- **A cell** is the label (a `ui::TextRole::Label` text, so the theme's `field.caption.case` shapes it) and the control (which marks a required field, live, and describes itself with the help),
  visible while the field is. The cell owns the caption, and the control owns the required mark and the unit: the mark is the
  control's alone, so a required field shows it once on every backend, and the control is told the field's label
  with `showLabel` false, so it draws no second caption. Each control carries the view tree's field state: read-only, required, the field's error as its
  one message, and the help as its `description`, which the control draws under itself and assistive technology
  reads as its description, so the help is tied to the control on every kit. A control an `Overrides::Render` draws
  need not read the field state, and an Object's panel and an ObjectArray's rows are not one control, so for those
  the cell draws the help as a muted text under the control. Test ids: `field_<path>` on the control, `label_<path>`, `cell_<path>`, `unit_<path>`,
  `add_<path>`, `remove_<path>#<key>`, `form_title`, `form_submit`, `form_status`, `form_result`.
- **Where the caption goes** is `FormViewOptions::fieldLayout` (`forms::FieldLayout`, in
  `forms/engine/field_layout.hpp` so the authoring API names it without the engine). `Stacked`, the default, is the
  column above: label, control. `Inline` makes the cell a row centred across (`align: center`, gap step 3): the
  label at `fixed(inlineLabelWidth)` (default 16 units) and the control in a column that stretches, so the control
  fills the rest of the line; a Boolean, whose checkbox carries its label, gets a spacer of the label's width in the
  label's place, so its box lines up with the other controls. The control's column already starts at the label's width, so its
  description lines up under it; a field whose help the cell draws (an override's) is a column of that row and a
  second row, the help indented by a spacer of the label's width, with `cell_<path>` on the column. An Object's panel and
  an ObjectArray's rows stay stacked, and their members follow the form's layout. A document sets `inlineLabelWidth` with `captionWidth`, a literal from 1 to 64 that needs `fieldLayout: inline`. `formView` throws
  `std::invalid_argument` for an `inlineLabelWidth` below 1. While a field shows an error, an inline label centres on
  the control and its message together: centring on the control alone needs a baseline the view tree does not carry.
- **Layout.** A section is a `Panel`, consecutive tab groups one `Tabs`, an accordion a collapsible `Panel`, the
  implicit group a flat grid; each group is a `Grid` of `gridColumns` columns (the flat one 1 while `gridColumns`
  is 2), every cell spanning its `x-colspan` clamped to the grid. Tab selection and collapse are the session's
  signals, so switching tabs keeps what was typed.
- **Chrome.** A heading over the fields: `FormViewOptions::title` when it is set (bound, or a constant that is not
  empty, shown whatever `showTitle` says, since a title given is a title wanted; test id `form_title`), else the
  schema's title while `showTitle`. The status line while `showStatus`; a button in explicit mode labelled
  `submitLabel` (default `Submit`; a `ui::Prop`, so a host may bind it), enabled while ready and not in flight;
  the last error, and while `showStatus` the last reply: a quiet form still reports a failure, since nothing else on
  it would say the save did not happen. The status line names the label: `ready: press Deposit` for a form whose
  button says *Deposit*.
- **Overrides** replace a field's control, resolved by field path, widget hint, unit, then kind. The replacement
  is handed a `FieldView` (the spec, the state, the session) and may wrap `defaultControl()`; it writes the draft
  as the built-in control does.
- **Through a document.** The interpreter lowers a `form` node to `formView` over a session it builds from
  `Catalog::schema`, the node's overlay and its model alias ([../ui/interpreter.md](../ui/interpreter.md#lowering)).

- **A host component per field.** A document's field overlay may name a `component` that draws the field
  ([../ui/interpreter.md](../ui/interpreter.md#lowering)): the interpreter adds it as a field-path override, and the cell
  around it (caption, help, visibility) is drawn as for every override.

The scalar controls (every kind but an Object and an ObjectArray) are `detail::engine::scalarControl(session,
field)` in `forms/engine/field_control.hpp`, which a grid's cell editor draws too; `form_view.hpp` includes it and
`ui/grid.hpp`, and neither of those includes `form_view.hpp`.

**Not supported:** a document's subtree overrides.

## What changed, and edits not yet committed

- **The load record.** For every scalar field, the members of an Object and of each row of an `ObjectArray`
  included, the session keeps the draft and the unit it held when the form was last loaded, by
  `FieldState::path()` (`specimens#3.mass`), and each row of a collection then. Construction, `prefill`, `reset`,
  a `refill` that loads a field and a successful submission that ends a field's edit write it, for the fields they
  load.
- **`modified(path)`** (tracked) is true when the field's draft or unit differs from the record, or the field is a
  member of a row added since the load. A computed member (`FieldSpec::computed`, from `x-computed`) and a derived
  one (`FieldSpec::derived`, from `x-derived`) are never modified: only a round writes them. Any other member stays modified whatever freezes it after the user typed in it.
  **`rowModified(arrayPath, key)`** is true when a member of the row is modified or the row was added since.
- **`loadGeneration()`** (tracked) is a count that moves with each load: each write through `FieldState::load`,
  which sets the fields it writes afresh, their units included. Construction, `prefill`, `reset`, a `refill` that
  loads a field, a successful submission that ends a field's edit and a round of derived values (`loadDerived`)
  that writes a member are loads. A switch of units as loaded alone (`switchUnitsAsLoaded`) is not, and nor is a
  `revert`. What follows loads, such as a fixed grid's `unit` overlay giving a field its unit again after a load,
  reads it; since a switch as loaded moves it not, one grid's switch is never another's load.
- **`revert(path)`** restores the record's draft and unit, programmatically; a row's path (`specimens#3`) reverts
  each member, and reverting a row added since the load removes it.
- **`field(path)`** takes a row by key too: `rows#<key>.member`, as `FieldState::path()` writes it, beside
  `rows[i].member` by position.
- **Before-submit hooks.** `addBeforeSubmit(hook)` returns an id for `removeBeforeSubmit`; `submit()` runs every
  hook, in the order added, before it reads the body, so an edit not committed yet (a grid's open cell editor)
  is sent with the rest.
- **A refused submission's fields.** A reply's `errors` member names them: an object from a field's path
  (`specimens[1].mass`, read as `field(path)` reads it) to a message, or to a list of messages of which the first is
  shown. Each is that field's refusal: `refusal(path)` (tracked) gives it while the field keeps the draft it had then;
  a path the form has no field for is not shown. Such a reply did not save the record, so what it sent stays an
  edit. The next reply replaces them, and a failure clears them. A failure names no field: a
  `model::ActionRefusal`'s arguments fill its sentence, and an argument whose name happens to be a field's path is
  not that field's error. A reply member named `errors` is reserved for field errors: an action whose reply carries
  a domain member of that name has it read as field errors, and the record taken as not saved.
- A field is frozen (`readonly()`) by `x-readonly`, `x-computed`, `x-derived`, a frozen parent or a `readonlyWhen`
  naming it.

## Derived values

A form asks for the values it cannot compute from what it holds in **rounds**
([../table/editing.md](../table/editing.md#derived-values)): an evaluation of the action, whose recomputed body gives
the computed members (`x-computed`, on the action and on a row type), and a call of the form's derive action, whose
`forms::Derived` reply gives the derived members (`x-derived`, `FieldSpec::derived`). Neither is a member the user can
edit, so a round never replaces a draft the user typed.

- **`deriveWith(DeriveOptions)`** turns rounds on: `evaluate` sends the body to the action's evaluation (the UI
  service's `ui-evaluate`), `derive` sends `{"body": <body>}` to the derive action, `scheduler` runs the debounce,
  `debounce` is how long a commit waits for a further one (**150 ms** by default), and `refused` is told the paths a
  derive reply named that are no derived member. Until it is called a session makes no round.
- **`noteCommit()`** is what a committed edit runs (a grid's `FormGridSource::committed`). With a round in flight
  it marks the round stale; otherwise it restarts the debounce timer, whose expiry starts a round; with no scheduler
  a round starts at once.
- **A round** captures itself (`captureRound()`: the next sequence, `partialBody()`, and the row keys of every
  collection by `FieldState::path()`), sets `deriveState()` to `Running`, and sends the evaluation and the derive call,
  each when set, with that body. At most one round is in flight.
- **`partialBody()`** is the body as far as it encodes: every top-level field that encodes; an Object or a collection
  member by member, a blank member or one that does not encode left out; every row written, in order, `{}` for a row
  with nothing. A left-out member decodes to its default on the server.
- **Loading.** `loadDerived(round, Derived)` writes the derived members a reply names, and
  `loadDerived(round, evaluatedBody)` the computed members of the recomputed body (a computed member the body leaves
  out of an object it holds is blank; its `errors` and `ready` are not read). Both resolve `name[i]` through
  `round.rowKeys[name][i]` and the row's current position by key, so a value lands on the row it was computed for
  after rows were inserted or removed, and a value for a row removed since is dropped (not refused). A path that
  names no member of the reply's kind is refused and returned in `DeriveLoad::refused`. A load is programmatic and no
  edit, as `assign` is: nothing is submitted, what the user typed stays an edit, and a member it writes that held its
  loaded value holds the new one as loaded. A field's snapshot (`FieldState::snapshot`, what `refill` and a save's
  settle compare) leaves computed and derived members out, so a round never makes a top-level field look edited or
  different from what a submission sent: a save the round overlaps still ends the edit of what it sent, and a later
  `refill` still reaches the collection. A computed or derived member is never `modified`.
- **`switchUnitsAsLoaded(switches)`** switches units the same way, converting each draft: what the first unit a grid's
  per-row `unit` overlay gives each loaded row is, so the cell is not modified by it and a revert returns to it. It
  takes unit switches only, so nothing beyond the cells it names is recorded as loaded.
- **Order.** Each kind of reply keeps the sequence it last loaded; a reply of an older round loads nothing, so derived
  values never go back to an older result. When both replies of a round settle, `deriveState()` is `Failed` if
  either failed (the values stay as they were), else `Current`; a stale round then starts one more at once, with the
  newer body. `recalculate()` starts a round unless one is in flight. `deriveFailing()` is true from a round that
  failed until a round succeeds, a retry in flight included: a grid's stale marks and its standing "Derived values could
  not be recalculated" with "Recalculate" follow it, so they stay while the retry runs.
- **`insertRow(position)`** inserts a blank row before `position` (past the end appends), a user action like
  `addRow`.
- **`removeRows(keys)`** removes rows by key in one change and returns them as `RemovedRow{position, row}`, each with
  the position it stood at, ascending; **`restoreRows(rows)`** puts them back, each at its position, in one change, the
  rows whole (their keys, drafts and units), so a grid's "Undo" of a deletion restores what was typed. An array's
  `minItems` and `maxItems` are `FieldSpec::minItems` and `maxItems`, the bounds a grid's row operations keep.

## Collections

`CollectionModel::fromSchema(viewJson)` reads the `v-*` view document ([views.md](views.md)): the kind
(`collection`, `master-detail`), title, query, row key, the columns (field, label, the table `kind` by
`table::ColumnKind`'s JSON names, `v-hidden`, places, unit), the row action and the actions with their `bind`,
`scope` and `confirm`. An unknown `v-kind` or column `kind`, a missing `v-query`, `field` or `action` is a
`SchemaError`.

`CollectionSession(runtime, model, schemas, submit, choices, options)`:

- **The list** is a `reactive::Query` executing `v-query` with `{}`. `rows()` reads the reply (an array, or its
  first array member): each row's exact key text, its JSON with every digit, and the visible columns' cells — an
  exact `{num, den}` at its `dp` (else the column's places) with the column's unit, anything else as its text.
- **The table source.** `source()` is a `table::VectorSource` with one column per `v-columns` entry (hidden ones
  included), kept in step with `rows()`: integers as `int64` (row keys too, so ids past 2^53 stay distinct), exact
  values as `Rational`, text, bools. Sorting, filtering and selection are the table engine's over it
  ([../table/engine.md](../table/engine.md)); the session keeps no order of its own.
- **Actions** are one `Exclusive` `reactive::Mutation`: a row action's body is its `bind` entries with the row's
  exact JSON values, a collection action's is `{}`. A `confirm` action waits in `pendingConfirm()` until
  `confirm()` runs it or `cancelConfirm()` drops it. A success refetches the list.
- **The editor** is a `FormSession` over the row action's schema (from `schemas`; none when it is unknown).
  `open(rowKey)` prefills it from the row's bound values; a prefill replaces every field, so nothing typed for
  another row carries over, and it submits nothing. Each success of the editor (its `successCount`) closes it and
  refetches; a failure keeps it open.

## Wizards

`WizardModel::fromSchema(wizardId, wizardJson)` reads `w-title` and `w-steps` (each step's `action`, `title` and
`prefill` of `"<Action>.<member>"` paths); a wizard without steps is refused.

`WizardSession(runtime, model, schemas, submit, choices, options)` keeps one `FormSession` per step, alive for the
wizard's lifetime, and throws `std::invalid_argument` when a step's schema is unknown or does not read.

- **Done.** A step is done while its current body is the one whose submission last succeeded; an edit makes it not
  done until the new body succeeds, and a failed submission leaves it not done.
- **Next** moves on at once from a done step; from a step that is ready and not in flight it submits and moves on
  when that submission succeeds. `canNext()` says which. **Back** always returns, every step keeping its values.
- **Resolved values.** Each success records its body's members, then its reply's, as JSON under
  `"<Action>.<member>"`, so the reply wins on a name collision. Entering a step assigns its prefill from them as
  JSON values, programmatically; a step the prefill leaves ready and not done submits once in automatic mode, and
  returning to a done step submits nothing.
- `title()` and `stepTitle(i)` resolve `<id>.title` and `<id>.step.<i>.title` through the translations, tracked;
  `setTranslations` forwards to every step.

## Rendering collections and wizards

`collectionView(session, options)` and `wizardView(session, options)` render the sessions as view trees; as with
`formView`, mount the result after the session and destroy the mount before it. Their `options.form` is handed
to every form they draw, so `Overrides` reach the editor's and every step's fields.

- **A collection** is a header (the title, a `Busy` while the list loads, a button per collection action), an
  error line (the list's failure, else the last action's), a `Table` of the visible columns' exact cells with a
  last column of Open and the row actions, the confirm `Dialog` (open while `pendingConfirm()` is engaged; accept
  runs the action, cancel or dismissal drops it), and the editor's `formView`: in a `Dialog` open while
  `openRow()` is engaged for a `collection` view, beside the table and shown while a row is open for a
  `master-detail` one, whose table selects the open row and opens the row selected. Activating a row opens it when the view has a row action.
  Every action button is disabled while an action is in flight. Test ids: `collection_title`,
  `collectionAction_<Action>`, `collection_error`, `collection_rows`, `cell_<field>_<rowKey>`,
  `rowOpen_<rowKey>`, `rowAction_<Action>_<rowKey>`, `confirmDialog`, `confirm_message`, `confirm_accept`,
  `confirm_cancel`, `editorDialog`, `detailForm`; a row key past 2^53 keeps every digit in them.
- **A wizard** is a header (`<title>  (<step> / <steps>)` and a `Steps` indicator: done, current, pending), a
  `Switch` over the steps' `formView`s that mounts the current one (the values live in the step sessions, so
  leaving a step loses nothing), and Back and Next, enabled by `canBack()` and `canNext()`, with a note on the
  last step. Test ids: `wizard_title`, `wizard_steps`, `wizard_step_<index>`, `wizardBack`, `wizardNext`,
  `wizard_last`.
- **Titles.** `CollectionViewOptions::title` and `WizardViewOptions::title` replace the schema's title in the
  built-in header (`collection_title`; the wizard's before the step's position); empty shows the schema's. A
  header chrome draws its own.
- **Chrome.** `CollectionChrome` and `WizardChrome` replace the view's own parts with a host's: the collection's
  header, each row (the rows are then a list of the chrome's nodes rather than a table), the rows table whole
  (`rows`, handed the session and what draws a row's Open and row-action buttons for its key text, empty when the
  view has neither; unused while `row` is set), the confirm (mounted only while an action waits) and the editor
  dialog (handed the editor's form view to place); the wizard's header and navigation. A chrome is handed the
  session and drives it through the same calls the built-in parts make, so its gates are the session's: a chrome's
  Next does nothing until `canNext()`. A master-detail view has no editor dialog to replace.
- **Through a document.** The interpreter lowers a `collection` or `wizard` node to these views over sessions it
  builds from `Catalog::view` or `Catalog::wizard` and its model alias
  ([../ui/interpreter.md](../ui/interpreter.md#lowering)). It sets the collection's `rows` chrome: the rows are a
  table on the engine over `source()`, sorted, filtered, arranged, copied and persisted as every other table of
  the screen ([../table/table.md](../table/table.md#collections-and-master-detail)).

## Corpora

Measured, not asserted, by `tests/test_forms_engine_corpora.cpp` and the corpus cases of the other suites:

- **Rule corpus** (`tests/data/rule_corpus.json`, 46 rows): every row's `ready`, `visible` and `readonly`, and the
  engine's `ready` against `allRulesSatisfied` on the same row, so the client and the server cannot disagree.
- **Body corpus** (`tests/data/body_corpus.json`): each case is a schema, a locale, a display offset, an optional
  prefill and the text typed per field, and its body is the exact bytes the engine sends — or `null` when the form
  is not ready.
- **Instance bounds and conformance**: every row of `tests/data/instance_bounds.json` and every conformance fixture
  through a session.
- **Rendering**: controls per kind, labels from translations and in `x-order`, the live required mark, help,
  explicit Submit, the reply and error line, rule-driven visibility and read-only, sections, tab runs, accordions
  and grids, object panels and collection rows, override precedence and an override driving the session, on
  `RecordingBackend` (`tests/test_forms_engine_view.cpp`); a `form` document through the interpreter
  (`tests/test_ui_interpreter_forms.cpp`), on Qt Quick (`tests/qt_quick/test_qt_quick_interpreter.cpp`) and on the
  terminal (`tests/tui/test_tui_forms.cpp`).
- **Collections and wizards**: `CollectionSession` and `WizardSession` (`tests/test_forms_engine_collection.cpp`,
  `tests/test_forms_engine_wizard.cpp`); drawn widgets — cells and buttons by test id, the confirm and editor
  dialogs' visibility, the master-detail layout, the header, row, confirm, editor and navigation chrome — on
  `RecordingBackend` (`tests/test_forms_engine_collection_view.cpp`, `tests/test_forms_engine_wizard_view.cpp`); a
  collection and a wizard document through the interpreter, on Qt Quick and on the terminal. The chrome of the
  embedded forms is `formView`'s options and `Overrides`, which reach them.

### Choices a renderer could make otherwise

Each is a named case of the body corpus, or a test of the engine's own suite:

| Behaviour | Engine | Why |
|---|---|---|
| Nested objects, collections of objects | fields with their own encoders | every member kind the emitters write has an encoder, so nothing within the nesting limit is unrepresentable |
| A blank parent of a dependent Choice | keeps the child's selection (the query is idle) | no options were fetched to judge it against |
| An operand whose draft does not encode, for `engaged` | `Unknown` | a value the server would refuse does not satisfy a condition |
| A presentation rule naming several fields | applies to every field it names | membership |
| Automatic submission while Choice options are in flight | waits for the options | the body could carry a selection the new parent does not offer |

## Design decisions

| Decision | Why |
|---|---|
| Rules through `expr/1`, not an evaluator of their own | One evaluator on the client for rules, relations and checks; its three-valued logic and exact comparisons are what the rules need |
| Two names per operand (`engaged`, `value`) | A blank and an invalid operand are both `null` as values, and the rules distinguish them |
| Gates block only on `False` | `Unknown` comes from a field that already blocks, or from a rule kind an older client does not know |
| The body as JSON text assembled in `x-order` | The bytes are the contract the parity corpus compares; a DOM round trip would reorder keys and could round numbers |
| `forAction` declared in the model, defined with the emitters | A runtime form reads served JSON and needs no action type; only typed code pays for the emitters |
| Programmatic changes mark their body as seen | A prefill, a reset or a cleared stale selection is not a user's request to submit |
