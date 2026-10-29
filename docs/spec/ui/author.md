# Authoring UI documents on the server — design

Design spec for the server-side builders of the UI document and their registration
(`include/morph/ui/author.hpp`). A server writes its screens in C++ (spec 5 §11,
`docs/superpowers/specs/2026-10-07-ui-document-design.md`); the builders produce the JSON document of
[`document.md`](document.md), and registration validates it once, with the load-time rules of
`ui::loadDocument` and then the accessibility rules, so a screen that breaks a rule is a failing
server test and never reaches a client.

Documents have no `invalidates`, `patch` or `refreshOn`, because an action declares what it changes beside the
action (its effects) and every query holding a changed record refreshes, so the builders offer no option for them
and setting one does not compile. A screen that must refetch for a reason no action
declares runs `refetch(query)`; data that changes with time rather than through actions polls with `.refreshEvery`.

Read this before changing what a builder writes. What the JSON means is [`document.md`](document.md)
and [`expressions.md`](expressions.md); the builders add no meaning of their own.

## Contents

- [The namespace](#the-namespace)
- [Expression handles](#expression-handles)
- [Scopes and declarations](#scopes-and-declarations)
- [Commands](#commands)
- [View nodes](#view-nodes)
- [Registration](#registration)
- [The catalog](#the-catalog)
- [Accessibility rules](#accessibility-rules)
- [Goldens](#goldens)
- [Design decisions](#design-decisions)
- [Out of scope](#out-of-scope)

## The namespace

The builders live in `morph::ui::author`. `morph::ui` already holds the view-tree builders of
`view.hpp` under the same names (`text`, `button`, `column`, …) with other argument types, so a
screen's source writes

```cpp
namespace ui = morph::ui::author;
```

and reads as spec 5 §11 writes it. The header includes `document.hpp`, `core/registry.hpp` (for
`ModelTraits` and `ActionTraits`) and `core/model_key.hpp`; it is a server header, and nothing on a
client includes it.

## Expression handles

`Handle<T>` is an expression: a JSON tree, typed by the C++ value it evaluates to. `T` is `Any`
(`AnyHandle`) where a screen cannot know the type — an element value, an event payload, a field read
by name.

| Builder | Writes |
|---|---|
| A C++ `bool`, integer, string, `math::Rational`, `ui::Decimal`, `ui::Quantity`, `time::DateTime`, `nullptr`, `ui::Value` or `std::vector` of scalars | The constant: a scalar as itself, anything else as `{"lit": v}` |
| `a + b`, `a - b`, `a * b`, `a / b`, `-a` | `add`, `sub`, `mul`, `div`, `neg` |
| `a < b`, `a <= b`, `a > b`, `a >= b`, `eq(a, b)`, `ne(a, b)` | `lt`, `le`, `gt`, `ge`, `eq`, `ne` |
| `a && b`, `a \|\| b`, `!a` | `and`, `or`, `not` (both operands are written: a document has no short circuit) |
| `x.orElse(y)` | `coalesce(x, y)` |
| `x.at(&T::member)`, `x.at("name")` | A field read: `{"ref": "x.member"}`, `{"row": "member"}`, and below a computed value `{"get": e, "path": "member"}` |
| `ref<T>("name.path")`, `row(&T::member)`, `row("path")`, `row<T>()` | `{"ref": ...}`, `{"row": ...}` |
| `cond(c, a, b)`, `obj({{"k", e}})`, `list({...})` | `{"if": [...]}`, `{"obj": ...}`, `{"list": ...}` |
| `map(list, "x", fn)`, `filter(list, "x", fn)`, `filter(list, &T::flag)` | The comprehensions; `fn` receives the element's handle |
| `sorted(list, "x", by, order?)` | The `sort` form: `by` receives the element's handle and gives the value to order by; `order` is absent for ascending |
| `runs(list, "x", by)` | The `runs` form: `by` receives the element's handle and gives the value a run shares |
| `sortKey(column, SortOrder)` | One key of a table's `sort`, `{"column", "direction"}`; a `list` of them is a sort |
| `call<R>(name, {...})` and `engaged`, `coalesce`, `concat`, `concatLists`, `slice(list, start, count?)`, `format`, `count`, `sum`, `includes`, `isEmpty`, `round(x, dp, Rounding)`, `decimal(x, dp, Rounding)`, `abs`, `sqrt(x, dp, Rounding)`, `stdev(list, dp, Rounding)`, `rsd(list, dp, Rounding)`, `parseDecimal`, `quantity`, `t(key, fallback, {{"n", e}})` | Library calls, and `{"t": ...}`; a `Rounding` of `HalfUp` writes the mode `"halfUp"`, and the default `HalfEven` writes none, so documents written before the mode are unchanged |
| `env(name)`, `session(name)`, `hostValue(name)`, `prop(name)`, `event()` | `{"env"}`, `{"session"}`, `{"host"}`, `{"ref": "props.name"}`, `{"ref": "event"}` |
| `element("id")` and its `value()`, `draft()`, `checked()`, `selected()`, `selection()`, `open()`, `valid()`, `ready()`, `get(path)` | An element's values (spec 5 §6) |
| A query handle's `value()`, `at(&Result::m)`, `pending()`, `error()`, `errorKind()`, `can()`; a mutation handle's `result()`, `pending()`, `error()`, `errorKind()`, `can()` | `{"ref": q}`, `{"pending": q}`, `{"result": m}`, … |
| `Point::pref(path)` | `{"pref": "point.path"}` |

- **Types.** A constant converts to a handle when it can stand for the handle's type, and a handle
  converts to another when its value can: the same type, `Any` on either side, a `std::optional`
  of a type for the type (an expression propagates `null`), a C++ conversion, or a list of such
  elements. A wrong body type is therefore a compile error, as spec 5 §11 asks.
- **Field reads.** `at` reads on any handle. Below a name it extends the `ref` or `row` path; below
  a `get` it extends the `get`'s path, so `first(xs).at("lot").at("stock")` is one node; below any
  other expression (a call, a mutation's `result()`, a constant) it writes `{"get": e, "path": ...}`.
  A member's JSON name is glaze's, found by its address in a value-initialised object of its class:
  a member a `glz::meta` leaves out has no name and throws `AuthoringError`. A renamed member is a
  compile error.
- **Absent handles.** A default-constructed handle is absent: an option left unset writes no key.
  Using an absent handle as an operand throws `AuthoringError`.
- **`concatLists`** takes a list of `AnyHandle`s, or of `Handle<std::vector<T>>` for one element
  type, which answers a `Handle<std::vector<T>>`.
- **No `==`.** Equality is `eq` and `ne`. A `==` returning an expression rather than a `bool` breaks
  the comparisons C++20 rewrites from it.

## Scopes and declarations

`DeclScope` holds one scope's declarations; `Scope`, the screen's, adds params, the lifecycle keys,
components and customization points. A node that creates a scope — a dialog, a drawer, a `switch`
case, the rows of a `forEach` or `table` — takes a builder function that receives its `DeclScope`
(`RowScope` is the same type) and returns its nodes; what it declares is written under the node's
`"scope"`.

| Builder | Writes |
|---|---|
| `param<T>(name)` | `params.name`: the document type of `T` (`int`, `int?`, `list<string>`, `decimal`, `quantity`, `timestamp`, `record`, …) |
| `model<M>(alias, {.instance = ...})` | `models.alias`: `type` is `ModelTraits<M>::typeId()`; `instance` is `Instance::Private`, `Instance::Shared`, a key handle, `Borrow{"alias"}`, or left out |
| `state<T>(name)`, `state(name, constant)`, `state(name, handle)` | `state.name` with its type and `init`; `state<Any>` leaves the type to the init |
| `let(name, handle)` | `let.name` |
| `query<A>(name, model, body, {.when, .debounce, .refreshEvery, .staleTime})` | `queries.name` with `action` |
| `evaluate<A>(name, model, body, options)` | `queries.name` with `evaluate` (spec 5 §7); its handle's value is typed as `A`, since the reply is the body with its computed fields filled |
| `mutation<A>(name, model, {.body, .concurrency, .serialKey, .optimistic, .onSuccess, .onError, .offlineQueue})` | `mutations.name`; `.optimistic` is an `Optimistic{.query, .at, .key, .with}`, read against the run's body bound as `body` |
| `watch(on, commands, Source::User)`, `check({.name, .assertion, .message, .targets, .blocking})` | `watch`, `checks` |
| `title`, `identity`, `dirty`, `onMount`, `onUnmount`, `onClose`, `onSave`, `onBackendChange`, `key(chord, commands)`, `showErrors`, `prefetch(query)` | The screen's own keys |
| `subtitle(expr)`, `status(HeadBadgeOptions)`, `action(ActionButtonOptions)`, `menu(MenuItem)`, `menuLabel(expr)`, `tool(HeadSearchOptions)`, `tool(HeadSelectOptions)` | The screen's head: its subtitle, and a status badge, an action, an entry of its own menu or a tool appended in order; the menu trigger's accessible name. `HeadSearchOptions{id, label, value, placeholder, onChange, onSubmit, width, visible, enabled}` and `HeadSelectOptions{id, label, options, selected, onChoose, width, visible, enabled}` write a `headTool` of their kind; a `width` of 0 writes none |
| `component(name, {.qml \| .host + .version, .props, .events, .fallback})` | `components.name`, returning the `ComponentRef` `custom` names |
| `columnsPoint`, `arrangePoint`, `choicePoint`, `valuePoint<T>`, `point(name, "layout" \| "saved")` | `customize.name`, returning the `Point` `customize`, `resetPrefs` and `pref` name |

- **Actions are types.** A query, evaluation, mutation, `form`, `dataTable`, `collection` or
  `wizard` names its action as a template argument and its model through a `ModelHandle<M>`; the
  model must `execute` the action (`Runs<M, A>`), or the screen does not compile.
- **Bodies** are the action's own members: `{{&ListResults::sampleId, sampleId}, ...}`, a list of
  member pointer and value pairs (`Body<A>`). The member pointer must be the action's, and the
  value's type must be able to stand for the member's.
- **Names are given.** A query or mutation is named by its first argument; the builders invent no
  names.

## Commands

Each builder returns a `Command`; `Commands` is a list, built from one command or a braced list.

| Builder | JSON |
|---|---|
| `set(target, value)`, `reset(target)` | `set`, `reset`; the target is a named read |
| `run(mutation, body, {.each, .as})` | `run` |
| `refetch(query)`, `open(element)`, `close(element)`, `close(self)`, `focus(element)` | `refetch`, `open`, `close`, `focus` |
| `navigate(screen, {.params, .as, .onResult, .onCancel})`, `navigateRoute(route, options)` | `navigate` |
| `returnWith(value)` | `return` |
| `host(name, args)`, `switchBackend(name)`, `resetPrefs(point)`, `resetAllPrefs()`, `setLocale(locale)`, `notify(message, Role)` | `host`, `switchBackend`, `resetPrefs`, `setLocale`, `notify` |
| `seq({...})`, `all({...})`, `ifThen(condition, then, otherwise)` | `seq`, `all`, `if` |
| `cancelClose()`, `retry()`, `signIn(mutation)`, `signOut()` | `cancelClose`, `retry`, `signIn`, `signOut` |
| `upload(file, mutation, {.as, .transfer})`, `downloadToFile(file, name)`, `downloadToPreview(file, state)` | `upload`, `download` |

`Command{json}` wraps a command no builder spells.

## View nodes

Every kind of the kind table has a builder whose options struct holds the kind's own keys:
`text`, `button`, `textInput`, `checkbox`, `select`, `menu`, `column`, `row`, `grid`, `spacer`,
`panel`, `scroll`, `switchOn` with `Case`, `tabs`, `dialog`, `drawer`, `busy`, `forEach`, `table`,
`dataTable`, `group`, `dateTimeInput`, `slider`, `filePicker`, `banner`, `badge`, `progress`, `steps`,
`keyValue`, `emptyState`, `chart`, `splitter`, `collapsible`, `dropZone`, `customize`, `boundary`, `form`,
`collection`, `wizard` and `custom`. `chart(ChartOptions{summary, series, lines, yMin, yMax, yPlaces, yLabel})`
takes its series as `ChartSeries{label, points, y, x, xLabel, tone, note, joined}`, where each per-point key is a
`PointExpression`, a function from the point's element (`row()`) to an expression, and its lines as
`ReferenceLine{value, label, tone, style}` with a `LineStyle`. Children come first, as a `Nodes` list (`{a, b}`, or one node),
options second. `tabs(tabs, TabsOptions{.selected, .onChoose})` takes `Tab{id, label, children}` pages.
`pinned(content, PinnedOptions{.top, .bottom})` writes the bar-that-stays-in-view idiom: a column sized to stretch
both ways holding `top`, a vertical `scroll` sized to stretch both ways around `content`, and `bottom`.
`stepper(StepperOptions{.step, .steps, .backLabel, .continueLabel, .finishLabel, .onFinish, .extra, .labelled,
.testIdPrefix})` writes a wizard over screen state, with no node of its own: a column of a `steps` indicator built from
the steps' labels and `step`, a `switch` on `step` with one case per step, and a footer row of Back, a spacer, `extra`
and Continue. Each `StepperStep{label, children, canContinue, blocking, onBlocked}` gives what Continue does there:
while `canContinue` holds (absent: always), it sets `step` one on, or runs `onFinish` on the last step; otherwise it
runs `onBlocked` and focuses `blocking`, which may be declared in the step's own case. On a step that cannot
continue and names no `blocking` element, Continue is disabled, since a click would do nothing. Continue's commands are one
`if` chain on `step`, so a step's own `set` never satisfies the next step's test. Back sets `step` one back and is
disabled on the first. The three labels are plain text and default to English (`Back`, `Continue`, `Finish`). A given
`testIdPrefix`, empty included, names the indicator, Back and Continue `<prefix>steps`, `<prefix>back` and
`<prefix>continue`.
`DialogOptions::actions` (a `dialog`'s or `drawer`'s) lists its footer buttons as
`ActionButtonOptions`, run in its scope, and `DialogOptions::description` writes its head's description line.
`DialogOptions::sheet` and `dismissLabel` are a dialog's: the condition for drawing it as a sheet, and its dismiss control's label.

The keys every node takes are member functions of `Node`, each returning a copy with the key set:
`id(element)`, `visible`, `enabled`, `tooltip`, `sizing`, `span`, `autofocus`, `testId`, `surface` (a name, or an
expression of one),
`a11y(name, role)`, `drag(payload)`, `drop(accepts, commands)`, `key(chord, commands)`, the bounds `minWidth(n)`,
`maxWidth(n)`, `minHeight(n)`, `maxHeight(n)`, `alignSelf(Align)`, and the input decorations `readonly`, `required`, `errors`, `stale`.
`sizing(width, height)` writes both dimensions as a record, a literal one when both are literal. The free helpers
`content()`, `fixed(n)` and `stretch(w = 1)` answer literal sizings (`"content"`, `{"lit": {"fixed": n}}`,
`{"lit": {"stretch": w}}`), which the loader checks; a record built with `obj` is evaluated at mount. `with(key, handle)` and
`withText(key, text)` set any other key, and `Node{json}` wraps a node no builder spells.

- **`badge`** takes its text, tone and icon, or `BadgeOptions{text, tone, icon, onRemove, removeLabel}` for one the
  user can remove.
- **`menu`** takes `MenuItem{id, label, icon, keys, checked, enabled, onClick, items, separator, description,
  visible}` entries, each key written only when given (`id` names an entry of a screen head's own menu, `Scope::menu`;
  a `menu` node's entries give none); `menuSeparator()` is an entry with `separator`, which writes nothing else.
  `menuButton(MenuButtonOptions{label, icon, emphasis}, items)` writes a `style: button` menu; an icon-only one names
  itself with `.a11y(...)`.
- **`text` and `banner`** take `spans` beside `text` (declared right after it, so a call names `.spans` before
  `.role`). The free helpers build the runs: `strong(text)`, `mono(text)` and `styled(text, SpanStyle{.strong,
  .mono})` each a record run, and `spans({…})` the list (`list` under a name that reads at the call):
  `text({.spans = spans({"Lot ", mono(lot)}), .role = "muted"})`. `styled` is not `span`, which a node's `span`
  already means.
- **`group`** takes its children and `GroupOptions{label, required, description, errors, gap}`.
- **`button`** takes `ButtonOptions{label, icon, emphasis, onClick, onActivate, style, selected, children}`: an absent `style`
  writes no key, and `selected`, a condition, is written as given: an item's current entry, a push button pressed in.
- **`column`, `row` and `grid`** take their children and `ContainerOptions{gap, columns, tracks, align, justify, wrap, divided}`; an
  absent `align` or `justify` is left out, and a present one is written by name (`"center"`). `columns` and `tracks`
  (`GridTrack{width, align}`, each key written only when given) are the grid's, and
  a grid's `justify` is written so the loader refuses it rather than dropped. `wrap` is a row's: the children
  continue on the next line when they do not fit; written on a column or a grid, the loader refuses it. `divided` is a column's: a rule is drawn between its children; the loader refuses it on a row. `forEach`
  takes the same `wrap`, for a horizontal `axis`, and `divided`, for a vertical one, in its `ForEachOptions`.
- **`table`** takes its rows, its options (`key`, `columns` (each `TableColumn{name, label, width, role, align,
  valign, cell, sortable, sortBy}`; an absent `align` or `valign` is left out and a present one is written by name,
  `sortable` only when `true`), `selectionMode` (a
  `Selection`, written as `selectionMode`), `selection` (the selected keys, controlled when given), `onSelectionChange`
  (with `event` the new keys), `sort` (a list of `sortKey`s, controlled when given), `onSortChange` (with `event` the
  new sort), `pageSize` and `page`, `group` (a `TableGroup{by, heading}`: a heading row before each run of rows
  sharing `by`, read in a row scope whose row is the run record), `filters` and `highlightKey` (which the loader takes on a `dataTable` only),
  `rowTone` (each row's tone, which the client draws), `selectColumn` (written only when `true`), `selectAllLabel`,
  `rowLabel` (each row's name, in the row scope), `onActivate`, `empty` (the nodes shown in
  its place while its rows are an empty list), `valign`), and optionally a row-scope builder that returns the
  columns; `cells({...})` makes columns from cells alone. **`dataTable<A>(model, body, options)`**
  is the table of spec 7 declared from a list action, with `mode` (`TableMode::Server` or
  `Client`).
- **`form<A>(model, {.prefill, .key, .submit, .showErrors, .fields, .onSubmit, .onSuccess, .onError,
  .presentation})`**: the `fields` overlay is keyed by member pointer, `{&A::member, {.hidden, .label, .unit,
  ...}}`. `FormPresentation{title, submitLabel, showStatus, columns, fieldLayout}` writes the presentation keys a form, a
  collection and a wizard share; an absent one is left out.
- **`collection<View>(model, presentation)` and `wizard<Wizard>(model, presentation)`** (`ui/author_forms.hpp`),
  like `collection<A>` and `wizard<A>`, take a `FormPresentation` (default none) and a view
  descriptor or a `flows::Wizard` instead of an action. The node names the view's `v-query` or the
  wizard's first step, and the node's facts carry `views::viewSchemaJson<View>()` or
  `flows::wizardSchemaJson<Wizard>()` under that action. The bundle serves the document with the
  `ui-catalog` reply, so a client builds the node without knowing the types. The header is separate
  so the other builders do not pull in the forms headers. `collection<A>` and `wizard<A>` over an
  action carry no document, and the client's catalog must supply one.
- **`custom(component, {.props = {{"name", e}}, .on = {{"event", commands}}})`** names a declared
  component; props and events are keyed by name, since a component's props are declared as data.

## Registration

`Screen` is an id and a builder function from `Scope&` to the view; `screen<"id">(fn)` and
`screen("id", fn)` make one. `Screen::write()` runs the builder and writes the document; a builder
that throws — `AuthoringError` or anything else — is a refusal at the document's root with the
exception's text.

`ScreenRegistry::add(screen)`:

1. refuses a second screen with the same id (`/id`);
2. emits the JSON once, as compact text — the text that is served;
3. loads that text with `loadDocumentText` and the registry's catalog (below), so the depth bound
   of 64 and every load-time rule of `document.md` apply to exactly what a client receives;
4. runs `checkAccessibility` on the loaded document;
5. keeps the text and the loaded `Document`, and adds the screen's catalog facts to the registry's.

The first failure is returned and nothing is kept. `enroll(screen)` does the same and keeps the
refusal in `refusals()`; `MORPH_REGISTER_SCREEN(screen)` enrols into the process's
`screenRegistry()` at static initialisation, where nothing could report a refusal, so a server's
test asserts that `refusals()` is empty and `lost()` is zero. An exception at static
initialisation cannot be caught, so `enroll` never throws: the macro's argument is an expression
that `enroll` evaluates inside its own `try`, through a lambda, so making the screen is covered
too, and anything thrown outside the builder (an allocation failure) is counted by `lost()`. The
registry is filled during static initialisation and read afterwards; it is not synchronised.
`catalog()` answers `ui::Catalog` from every registered screen's facts, for a local-mode client.

## The catalog

Registration answers `ui::Catalog` from the types the builders saw, not from the core registry,
which holds neither the UI facts nor which model an action belongs to:

| Fact | From |
|---|---|
| A model type id | `ModelTraits<M>::typeId()` of each `model<M>` |
| `keyed` | `model::KeyedModel<M>` |
| `stateless` | `ModelFacts<M>::stateless`, `false` unless the application specialises it |
| An action id and its model type | `ActionTraits<A>::typeId()`, and the model of the handle the action runs through (a query, mutation, form or table) |
| `calculatedFields`, `secretFields`, `credential` | `ActionFacts<A>`, empty unless the application specialises it |
| `listFields` | The members glaze writes as a JSON array, when glaze writes the result type of the model's `execute(A)` as a record; absent for any other result |

Facts gathered by a screen take precedence over the registry's, and the first fact recorded for an
id is kept. Because the builders check model and action types at compile time, the catalog's
unknown-model, unknown-action and wrong-model rules cannot fire on builder output;
the keyed-instance, optimistic, optimistic `at` and offline rules can, and registration refuses them with
their paths. An `Optimistic`'s `at` names the list member of a record result, and is written only when set; one
without a query is refused at registration.

## Accessibility rules

`checkAccessibility(document)` runs the rules of spec 5 §11 on a loaded document, after the
load-time validation:

| Node | Must have |
|---|---|
| `textInput`, `dateTimeInput`, `slider`, `select`, `checkbox`, `filePicker` | A `label` or an `a11y.name` |
| `button` | A `label` or an `a11y.name`; an `icon` never names it |
| `dialog`, `drawer` | A `title` or an `a11y.name` |
| `group` | A `label` or an `a11y.name` |
| `table` (including one declared from an action), `custom` | An `a11y.name` |

- A label, title or name written as the constant `""` or `null` does not count; any other
  expression does, since its value is not known until the client evaluates it.
- An item with content is named by its label, not by its content: a screen reader reads the label, so whatever a
  listener needs from the content (an out-of-specification tag) belongs in the label too.
- A `form` is not checked: each field takes its label from the schema or the `fields` overlay.
- The view is walked through every node list and every part's nodes (tab pages, `switch` cases,
  table cells, a boundary's `loading` and `error`), and so is each component's fallback.
- The refusal names the document, the node's JSON Pointer and the rule.
- The client does not run these rules: they guard authoring, and a client must not refuse a
  document an older server registered.

## Goldens

`tests/ui/golden/*.json` hold builder output, pretty-printed, for the reference screen of spec 5
§11 (`results`), every declaration, lifecycle key, component and customization point
(`declarations`), every node kind and common property (`palette`), and every command (`commands`).
`tests/test_ui_author.cpp` compares each screen's output with its file and registers it, so each
golden is also a document that passes the load-time and accessibility rules. With
`MORPH_UPDATE_GOLDENS` set in the environment, the test rewrites the files instead; a changed golden
is reviewed like code.

## Design decisions

- **A namespace of its own.** `morph::ui::author`, because `morph::ui` already names the view-tree
  builders; overloading them on option types would make every call site depend on which header
  was included.
- **Bodies are member and value pairs.** C++23 cannot synthesise the action's struct with its
  members retyped as handles, which `{.sampleId = id}` would need. A member pointer keeps both
  guarantees of spec 5 §11 — a renamed or foreign field and a value of the wrong type are compile
  errors — at the cost of naming the action in each pair.
- **No refresh options.** The refresh keys restate, per document, what an action's effects already say once for
  every client, and a restatement can drift from the action.
- **Facts from the types.** `keyed` is a property of the C++ type, and the model of an action is
  the model it runs through. The facts a type cannot show are traits an application specialises
  beside the action, which keeps them next to the code they describe.
- **The rules on the loaded document.** The accessibility rules read `ui::Document`, not the
  builders' state, so a hand-written document is checked the same way and the rules see exactly
  what a client renders.
- **Text once.** Registration loads the text it serves rather than the JSON tree, so the depth
  bound and the parser's own refusals apply to the served bytes.
- **Spelling.** `if_` and `return_` break the project's naming rule and `switch` is a keyword: the
  expression `if` is `cond`, the command is `ifThen`, `return` is `returnWith`, the node is
  `switchOn`; the `host` value is `hostValue`, beside the `host` command.

## Out of scope

- **The app document** (spec 6 §5): `Scope` writes a screen with a view; a builder for the app
  document, which has none, follows the app shell's work.
- **Bundles**: the `restyle` map, the manifest and message catalogues are the UI service's.
- **Spec 6 §11's reference screen** (`lims.sample`): the lims example does not carry it or the
  model additions it needs, so its golden waits for that example work. The `results` golden is
  spec 5 §11's screen.
