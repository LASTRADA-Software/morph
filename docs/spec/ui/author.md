# Authoring UI documents on the server — design

Design spec for the server-side builders of the UI document and their registration
(`include/morph/ui/author.hpp`). A server writes its screens in C++ (spec 5 §11,
`docs/superpowers/specs/2026-10-07-ui-document-design.md`); the builders produce the JSON document of
[`document.md`](document.md), and registration validates it once, with the load-time rules of
`ui::loadDocument` and then the accessibility rules, so a screen that breaks a rule is a failing
server test and never reaches a client.

The builders write `ui/2` documents. `ui/2` has no `invalidates`, `patch` or `refreshOn`, because an action
declares what it changes beside the action (its effects) and every query holding a changed record refreshes, so the
builders offer no option for them and setting one does not compile. A screen that must refetch for a reason no action
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
| `x.at(&T::member)`, `x.at("name")` | A field read: `{"ref": "x.member"}`, `{"row": "member"}` |
| `ref<T>("name.path")`, `row(&T::member)`, `row("path")`, `row<T>()` | `{"ref": ...}`, `{"row": ...}` |
| `cond(c, a, b)`, `obj({{"k", e}})`, `list({...})` | `{"if": [...]}`, `{"obj": ...}`, `{"list": ...}` |
| `map(list, "x", fn)`, `filter(list, "x", fn)`, `filter(list, &T::flag)` | The comprehensions; `fn` receives the element's handle |
| `call<R>(name, {...})` and `engaged`, `coalesce`, `concat`, `format`, `count`, `sum`, `includes`, `isEmpty`, `round`, `parseDecimal`, `quantity`, `t(key, fallback, {{"n", e}})` | Library calls, and `{"t": ...}` |
| `env(name)`, `session(name)`, `hostValue(name)`, `prop(name)`, `event()` | `{"env"}`, `{"session"}`, `{"host"}`, `{"ref": "props.name"}`, `{"ref": "event"}` |
| `element("id")` and its `value()`, `draft()`, `checked()`, `selected()`, `selection()`, `open()`, `valid()`, `ready()`, `get(path)` | An element's values (spec 5 §6) |
| A query handle's `value()`, `at(&Result::m)`, `pending()`, `error()`, `errorKind()`, `can()`; a mutation handle's `result()`, `pending()`, `error()`, `errorKind()`, `can()` | `{"ref": q}`, `{"pending": q}`, `{"result": m}`, … |
| `Point::pref(path)` | `{"pref": "point.path"}` |

- **Types.** A constant converts to a handle when it can stand for the handle's type, and a handle
  converts to another when its value can: the same type, `Any` on either side, a `std::optional`
  of a type for the type (an expression propagates `null`), a C++ conversion, or a list of such
  elements. A wrong body type is therefore a compile error, as spec 5 §11 asks.
- **Field reads.** `expr/1` reads a field only below a name, so `at` extends a `ref` or `row` path
  and throws `AuthoringError` on any other expression (a call, a `result`). A member's JSON name is
  glaze's, found by its address in a value-initialised object of its class: a member a `glz::meta`
  leaves out has no name and throws as well. A renamed member is a compile error.
- **Absent handles.** A default-constructed handle is absent: an option left unset writes no key.
  Using an absent handle as an operand throws `AuthoringError`.
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
`dataTable`, `dateTimeInput`, `slider`, `filePicker`, `banner`, `badge`, `progress`, `steps`,
`keyValue`, `emptyState`, `splitter`, `collapsible`, `dropZone`, `customize`, `boundary`, `form`,
`collection`, `wizard` and `custom`. Children come first, as a `Nodes` list (`{a, b}`, or one node),
options second.

The keys every node takes are member functions of `Node`, each returning a copy with the key set:
`id(element)`, `visible`, `enabled`, `tooltip`, `sizing`, `span`, `autofocus`, `testId`, `surface`,
`a11y(name, role)`, `drag(payload)`, `drop(accepts, commands)`, `key(chord, commands)`, and the
input decorations `readonly`, `required`, `errors`, `stale`. `with(key, handle)` and
`withText(key, text)` set any other key, and `Node{json}` wraps a node no builder spells.

- **`table`** takes its rows, its options (`key`, `columns`, `selection`, `sort`, `filters`,
  `rowTone`, `highlightKey`, `onActivate`), and optionally a row-scope builder that returns the
  columns; `cells({...})` makes columns from cells alone. **`dataTable<A>(model, body, options)`**
  is the table of spec 7 declared from a list action, with `mode` (`TableMode::Server` or
  `Client`).
- **`form<A>(model, {.prefill, .key, .submit, .showErrors, .fields, .onSubmit, .onSuccess, .onError})`**:
  the `fields` overlay is keyed by member pointer, `{&A::member, {.hidden, .label, .unit, ...}}`.
- **`collection<View>(model)` and `wizard<Wizard>(model)`** (`ui/author_forms.hpp`) take a view
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
| `table` (including one declared from an action), `custom` | An `a11y.name` |

- A label, title or name written as the constant `""` or `null` does not count; any other
  expression does, since its value is not known until the client evaluates it.
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
  every client, and a restatement can drift from the action. The builders emit only `ui/2` rather than offering both
  vocabularies: a server that must keep `ui/1` documents for older clients keeps them as hand-written JSON, which
  the UI service still serves.
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
