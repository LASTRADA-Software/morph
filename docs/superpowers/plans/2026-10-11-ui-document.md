# `morph::ui` — the UI document, `expr/1`, the interpreter and authoring — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship part 5 of the declarative UI program: the UI document of spec 5 as a C++ model with its load-time
validation, the `expr/1` evaluator, the interpreter that turns a document into reactive nodes and mounts it, and the
server-side builders. This plan covers the whole part. It is delivered as **three pull requests**: A (this branch),
B and C, described below.

**Spec:** `docs/superpowers/specs/2026-10-07-ui-document-design.md` (spec 5) §3–§11, §12b, §14;
`2026-10-08-app-hosting-design.md` (spec 6) §3, §5, §10, §14, §16–§20 for the lifecycle keys and the node, command
and expression additions spec 5 §3 puts in `ui/1`; `2026-10-04-declarative-ui-tui-design.md` (spec 1) §1, §2 and
§7 "ui". Issue: #890. Program: #885.

**Starting point.** No earlier plan exists for this part; it is written fresh from the specs on `master`
(`1d1a70a9`). The part depends on the bridge seams (`bindByType`, `executeRaw`, string-keyed `attach`), the
reactive core, the view tree with `Mounted` and `RecordingBackend`, and the table engine, all on `master`.

## The split

The issue suggests two pull requests, document and expressions first, then the interpreter. Planning against the
specs gives a third, independent piece, so the part lands as three:

| PR | Contents | Needs | Closes |
|---|---|---|---|
| **A** (this branch) | `value.hpp`, `expression.hpp`, `document.hpp`; the validation, expression and message corpora | `master` | `Part of #890` |
| **B** | `interpreter.hpp`; the control, relations, server-calculated values, several-models, permissions and commands corpora on `RecordingBackend` | A | `Part of #890` |
| **C** | `author.hpp`; the accessibility rules; the authoring goldens | A | `Part of #890`; the last of B and C closes it |

**Why `author.hpp` is not in A, and not in B.** The builders of spec 5 §11 produce a document and run the
load-time validation of §3 on it. They need the document model and nothing of the interpreter, so they can follow
A directly and run beside B in a second lane. They are kept out of A because A already carries the whole
language — values, types, the expression forms, the function library, MessageFormat, the document grammar and every
load-time rule — and a reviewer of A should be judging the language, not a C++ builder API on top of it. They are
kept out of B because B is the largest piece of the part (the control, relations and calculation semantics of §5–§8
over the reactive core) and gains nothing from them. Spec 5 §11 says the reference screen of spec 6 §11 fixes the
builder API; that screen uses `form` and `dataTable` nodes, so C is also the natural place to settle how the
builders name forms-engine and table-engine nodes.

**Why `value.hpp` is a header of its own.** The document's typed value (`Value`, spec 5 §3) is what `expression.hpp`
evaluates to and what `host.hpp` (spec 6 §4, hosting part) puts in `ParamMap`. Hosting and the Qt Quick renderer need
the value without the expression language or the document grammar. **`message.hpp`** is separate for the same
reason: whoever loads a message catalogue validates its patterns with `parseMessage`, without the expression
language; `expression.hpp`'s `t` uses it.

## The `ui-*` envelope decision (open point of #885)

Spec 5 §2 says the bridge seams and the `ui` envelope kinds "come before [the interpreter] in the delivery order";
spec 1 §1 delivers them with the generic client, after hosting. **Decision: the envelope kinds land with the UI
service part** (the `ui-*` kinds, the catalog and bundles), which follows this part and does not block it:

- The bridge seams have landed; they are what the interpreter dispatches through.
- The interpreter's load-time checks need two facts per model type — `stateless` and `keyed` — and the action facts
  of a few rules (which reply fields the server calculates, which are `secret`, which actions carry a credential).
  `document.hpp` reads them through `ui::Catalog`, a pair of lookups the caller supplies. In local mode the caller
  answers them in-process from the model registry; on the server, registration answers them from the same registry;
  in remote mode the UI service will answer them from the `ui-catalog` reply. Nothing in A or B needs a wire kind.
- A lookup the caller leaves empty skips the rules that need it, and the documentation says which. The server, which
  always has its registry, runs every rule at registration (spec 5 §11), so a client without the facts still never
  mounts a document the server could have refused.

Spec 5 §2 and spec 1 §1 are amended in A to say this, so the two agree.

## Global Constraints

- C++23, header-only, in the base `morph` target; every new public header added to the `FILE_SET HEADERS` of the
  top-level `CMakeLists.txt`. No new CMake option, no new dependency: JSON is glaze's `glz::generic_u64`, as
  `morph::forms` uses it, so an integer is never read through a double.
- `morph::ui`'s document layer depends on `morph/util/{rational,datetime}.hpp`, `morph/render/{i18n,
  locale_format}.hpp` and glaze; it includes no toolkit, nothing under `morph/net`, and not `morph/core/bridge.hpp`
  (the interpreter in B is the first `ui` header that dispatches).
- Every file starts with `// SPDX-License-Identifier: Apache-2.0`, then `#pragma once` for headers.
- Naming (`.clang-tidy`): types `CamelCase`, functions and variables `camelBack`, private members `_camelBack`,
  constants `kName`; parameter names of at least three characters. Headers index containers with `.at()` or
  iterators, never `operator[]`.
- AGENTS.md: comments and docs state what the code does now and why; no history, no issue numbers.
- Doxygen `WARN_AS_ERROR = FAIL_ON_WARNINGS`: every public symbol, `detail` included, has a brief and complete
  `@param`/`@tparam`/`@return`. A concept takes no `@tparam`.
- GCC 15 at `-O2` in CI: no braced temporaries inside a conditional expression, no `std::ranges::next_permutation`;
  a syntax check with `/opt/homebrew/bin/g++-16` and the project's GCC flags before pushing.
- Tests are tagged `[ui]` and `[ui][document]`, `[ui][expr]`, `[ui][messages]`; the corpora are JSON files under
  `tests/ui/corpus/`, read at run time through compile definitions, so another implementation of `expr/1` can run
  the same cases.
- One local build at a time, `-j 4`, examples off (`-DMORPH_BUILD_EXAMPLES=OFF`).

## Design (A)

**Values (`value.hpp`).** `Value` is null, `bool`, `int` (int64), `string`, `Decimal` (an exact `math::Rational`
whose `DecimalPlaces` is the declared places), `Quantity` (a decimal and a unit id), `timestamp`
(`time::DateTime`), a list, or a record. JSON decoding is structural: an object of exactly `num`, `den`, `dp` is a
decimal, with `unit` a quantity, which is the wire form of `Rational`; a non-integral JSON number is refused, since
an exact value is never written as a double. `Type` is `bool`, `int`, `string`, `decimal`, `quantity`,
`timestamp`, `key`, `record`, `list<T>`, each optionally `T?`, and `any` for what a document cannot know (a query
result's type comes from the action schema, which the document never restates). `Refusal` is the one error type
of the layer: the document id, a JSON Pointer into the document, and the reason.

**Expressions (`expression.hpp`).** `parseExpression` turns a JSON tree into an immutable `Expr` tree, each node
carrying its JSON Pointer; an unknown form, a wrong companion key or more than 256 nodes is a `Refusal`.
`checkExpression` walks the tree once with a `NameResolver` the document supplies, resolves every name, checks
function arity and the statically known argument types, and infers a coarse result type (`any` where it cannot
know). `evaluate` is total: it never throws, a runtime type mismatch, overflow or division by zero yields `null` and
is reported through the environment's diagnostic channel, once per expression path. The function library is spec 5
§4's, exact over `Rational` through the checked operations, with three-valued `and`/`or`/`not`, absence
propagation, unit conversion through a `UnitTable` (the exact `x-unitAlternatives` factors), and half-to-even
rounding for `round`, `mean` and the display functions. `t` implements the MessageFormat 2 subset of spec 5 §4
with the CLDR plural rules for the locales the corpus names and a small set of others; a pattern outside the subset
is refused by `parseMessage`, and `t` then shows the fallback. `Environment` is the interface the interpreter (B)
implements over reactive reads; `StaticEnvironment` is a value-holding implementation for tests and for evaluating
outside a mounted screen.

**The document (`document.hpp`).** `Document` holds a screen's (or the app document's) declarations, lifecycle
keys, components, customization points and view. Declarations are typed structs (`ModelDecl`, `QueryDecl`,
`MutationDecl`, `StateDecl`, `LetDecl`, `WatchDecl`, `CheckDecl`) grouped in a `ScopeDecl`; commands are `Command`
values of a closed `CommandKind`; a view node is a `DocNode` whose properties are parsed by a per-kind shape table
(expression, command list, node list, or a record or list of those), so the interpreter reads a kind's properties
by name and every node kind is described in one place. A scope-creating node (dialog, drawer, switch case,
`forEach` and `table` rows) carries its declarations under `"scope"`. `loadDocument` parses and validates in one
pass, and returns the first refusal.

**Load-time rules** (each with a corpus case): vocabulary `ui/1`; node kinds, functions, commands and expression
forms in the vocabulary; a `!`-prefixed key the client does not know; names well-formed and unique per scope;
every reference resolves, innermost scope first, including element values per element kind (§6), query and
mutation names, dialogs for `open`/`close`, targets of `set`, `reset`, `focus`, `refetch`, `run`, `signIn`,
`upload`; `row` only inside a row scope; `event` only inside a command list; `result` and `body` only in
`patch`/`optimistic`; `boundary` and `retry` only in a boundary's `error`; `cancelClose` only in `onClose`;
`props` only in a component's fallback; types (declared state and params against their `init`, `if`/`when`/
`dirty`/`visible`/`enabled`/`assert` conditions, function arguments); the `let` and `watch` graphs acyclic; the
size bound; a row scope's model alias names an `instance`; queries and mutations name a model alias and exactly one
action; `prefetch` bodies depend only on params; custom nodes name a declared component, its props and events;
`columns` customization points name a table; reserved chords; and, with a `Catalog`, unknown model types and
actions, `instance` on a model that is not keyed, actions of the wrong model type, `refreshOn` across model types,
an `optimistic` write to a calculated field, and `"offline": "queue"` on a credential-bearing action.

## Revisions against the spec (A)

- **S1 — envelope kinds.** Spec 5 §2 and spec 1 §1, as above.
- **S2 — scope declarations.** Spec 5 §3 says any scope may declare `models` … `checks` but not where a nested scope
  writes them. The §7 example uses `"scope"` on a table; A makes that the rule for every scope-creating node.
- **S3 — `run`'s `as`.** With `each`, `as` names the element inside `body`; without it, the reply for the following
  steps of the enclosing `seq`. The §5 example uses both; the §8 table now says which is which.
- **S4 — MessageFormat literals in spec 6 §11.** The reference screen passes `t("lims.outside", "{0} outside
  limits")` to `format`. In MessageFormat 2, `{0}` is a placeholder of the literal `0`, so the text would read "0
  outside limits" before `format` saw it, and a literal placeholder is outside the `ui/1` subset anyway. The example
  passes the count as a named argument instead.
- **S5 — `format`.** Spec 5 §4 names `format(template, args...)` without its placeholder syntax; it is `{0}`, `{1}`,
  … with `{{` and `}}` for literal braces, as spec 6 §11 uses it.
- **S6 — the node kinds' JSON names.** Spec 5 §9 lists the kinds in prose; A fixes their `kind` spellings
  (`dateTimeInput`, `filePicker`, `keyValue`, …) in `docs/spec/ui/document.md`.

- **S7 — what implementing settled.** Spec 5 now says: element and transfer ids share a scope's
  namespace, and `event`, `app`, `boundary`, `props`, `result`, `body` are reserved; drawers are
  scopes like dialogs; a refusal's path is a JSON Pointer; a number literal with a fraction is
  refused; rounding is half to even throughout; how quantities combine; exact `.match` keys are
  written `0` (MessageFormat 2's spelling) or `=0`, and a literal placeholder is outside the subset;
  `serialKey` is valid only with `serial`. Spec 1 §2 lists `value.hpp` and `message.hpp`.

Any further disagreement found while implementing is fixed in the spec in the same commit and listed in the PR.

## Files (A)

| File | Responsibility |
|---|---|
| `include/morph/ui/value.hpp` | `Json`, `Decimal`, `Quantity`, `Value`, `ValueKind`, `Type`, `parseType`, `valueFromJson`, `valueToJson`, `Refusal`, `pointerAppend`, `parseJson` |
| `include/morph/ui/message.hpp` | `PluralCategory`, `pluralCategory`, `MessagePattern`, `parseMessage`, `formatMessage`, `formatNumber` |
| `include/morph/ui/expression.hpp` | `Expr`, `ExprKind`, `parseExpression`, `NameResolver`, `checkExpression`, `UnitTable`, `FormatContext`, `Environment`, `StaticEnvironment`, `evaluate`, `functionNames` |
| `include/morph/ui/document.hpp` | `Catalog`, `ModelInfo`, `ActionInfo`, `LoadOptions`, the declaration structs, `Command`, `CommandKind`, `DocNode`, `Field`, `Component`, `CustomizePoint`, `Document`, `loadDocument`, `nodeKinds` |
| `tests/test_ui_value.cpp`, `test_ui_expression.cpp`, `test_ui_messages.cpp`, `test_ui_document.cpp` | The tests |
| `tests/ui/corpus/expressions.json`, `messages.json`, `documents.json` | The shared corpora |
| `docs/spec/ui/document.md`, `docs/spec/ui/expressions.md` | Authoritative specs |
| `CMakeLists.txt`, `tests/CMakeLists.txt`, `docs/spec/README.md`, `docs/ARCHITECTURE.md`, `CHANGELOG.md`, `scripts/check_branch_coverage.py` | Registration, maps, changelog, the coverage floor's measured value |
| `docs/superpowers/specs/2026-10-07-ui-document-design.md`, `2026-10-08-app-hosting-design.md`, `2026-10-04-declarative-ui-tui-design.md` | S1–S6 |

## Build and test commands (used by every task)

```bash
cmake -S . -B build/ui -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF   # once
cmake --build build/ui --target morph_tests -j 4
./build/ui/tests/morph_tests "[ui]"
```

A mutation is applied by hand, rebuilt with the same command, run against the named test, and reverted (`git diff`
checked) before the commit.

---

### Task 1: Values and types

**Files:** `include/morph/ui/value.hpp`, `tests/test_ui_value.cpp`, registration.

- [ ] `Value` with structural equality (a decimal's places are part of it, so a reactive equality skip never hides
  a change of display); `ValueKind`; accessors.
- [ ] `valueFromJson` / `valueToJson` (timestamps travel as ISO-8601 text, as `time::DateTime` does on the wire;
  `valueFromJson` given a `Type` reads them back); refusals for a non-integral number, an integer beyond int64, a
  zero denominator.
- [ ] `Type`, `parseType` (`int?`, `list<decimal?>`, refusals for an unknown name and unbalanced brackets),
  `Type::name()`.
- [ ] Tests: round trips per kind; each refusal; equality across places.

Commit `ui: the document's values and types`.

### Task 2: Messages

**Files:** `include/morph/ui/message.hpp`, `tests/test_ui_messages.cpp`, `tests/ui/corpus/messages.json`.

- [ ] `parseMessage` for the subset (text, `{$x}`, `{$x :number}`, `{$x :string}`, `.input`, `.match` with exact
  and category keys and `*`); refusal outside it; `formatMessage`; `pluralCategory` for `en`, `de`, `nl`, `sv`,
  `it`, `es`, `pt`, `fr`, `pl`, `ru`, `uk`, `cs`, `ar`, `ja`, `zh`, `ko`.
- [ ] Corpus: English, Polish and Arabic categories; `=0`/`0` exact keys; `.match` on a string; a refused pattern.

Commit `ui: MessageFormat 2 subset and CLDR plural rules for t()`.

### Task 3: Expressions

**Files:** `include/morph/ui/expression.hpp`, `tests/test_ui_expression.cpp`, `tests/ui/corpus/expressions.json`.

- [ ] `parseExpression` for every form of spec 5 §4 and spec 6 §5, §10, §14; the size bound; refusals with the
  path.
- [ ] `checkExpression` with arity, argument-type and condition checks, and the scoped names it binds itself
  (`map`/`filter`'s `as`).
- [ ] `evaluate` and the function library; `StaticEnvironment` with once-per-path diagnostics.
- [ ] `t` through the environment's `TranslationProvider`: the catalogue's pattern, else the fallback; a refused
  catalogue pattern shows the fallback and reports.
- [ ] The corpus: one case per function at least, exact arithmetic across units, overflow and division by zero as
  `null`, null propagation, Kleene tables, comprehensions, every reading form, the size bound, `mean` exact at
  the operands' places.
- [ ] Mutations run: `and` without Kleene's `false` dominance; `add` without the overflow check.

Commit `ui: the expr/1 evaluator`.

### Task 4: The document and its load-time validation

**Files:** `include/morph/ui/document.hpp`, `tests/test_ui_document.cpp`, `tests/ui/corpus/documents.json`.

- [ ] The model, the per-kind shape table, `loadDocument`.
- [ ] Every rule listed under "Load-time rules", each with a valid and an invalid corpus case naming the path and
  the reason.
- [ ] The `let` and `watch` cycle checks, reporting the cycle's names in order.
- [ ] The `Catalog` rules, and the same documents passing when the lookups are absent.
- [ ] Mutations run: the `let` cycle check disabled; the row-scope `instance` rule disabled.

Commit `ui: the UI document and its load-time validation`.

### Task 5: Specs, maps and changelog

- [ ] `docs/spec/ui/document.md` and `expressions.md`; `docs/spec/README.md`; `docs/ARCHITECTURE.md` (namespace
  row and header map); `CHANGELOG.md` `[Unreleased]` → `### Added`.
- [ ] S1–S6 in the design specs.

Commit `ui: specify the UI document and expr/1`.

### Task 6: Gates

- [ ] Full `morph_tests` on the Debug build.
- [ ] Coverage: a clang 22 coverage build of the `[ui]` tests through `scripts/aggregate_lcov_branches.py`;
  `include/morph/ui` at or above its 92% floor, and the floor's measured value updated. Never a lowered floor.
- [ ] GCC 16 syntax check of the new headers and tests with the project's GCC flags.
- [ ] Docs build (`MORPH_BUILD_DOCUMENTATION=ON`, target `doc`).
- [ ] clang-format on every touched file.
- [ ] Push, open the PR (`Part of #890`), comment on #890 with the split and the envelope decision.

---

## PR B: the interpreter (planned here, implemented later)

**Files:** `include/morph/ui/interpreter.hpp`; `tests/test_ui_interpreter_{control,relations,calculation,models,
permissions,commands}.cpp`; `docs/spec/ui/interpreter.md`.

- **Construction.** `Interpreter(reactive::Runtime&, Dispatcher&, InterpreterOptions)` mounts a `Document` with
  params through `ui::Mounted` on any `IViewBackend`. `Dispatcher` is the interpreter's view of the bridge seams:
  `bindByType`, `executeRaw` and the string-keyed `attach`, behind an interface so tests run on a fake and local
  mode on `LocalBackend`. The `Catalog` of A answers `stateless` and `keyed`.
- **Scopes.** One `reactive::Scope` per document scope; state as `Signal<Value>`, `let` and every bound property as
  `Computed<Value>` over `evaluate` with an `Environment` that reads the scope chain; element values as signals the
  mount's widget callbacks write.
- **Lowering.** Each `DocNode` kind to its `view.hpp` node (`Prop` slots over the computeds, command lists to
  `Action`s); `boundary` and `customize` lowered to view kinds, as spec 1 §5 says; `custom` to its fallback.
- **Control.** Queries through a per-session query cache keyed by `(model type, instance, action, canonical body)`,
  with `debounce`, `when`, latest-wins, `refreshOn`, `refreshEvery`, `staleTime`, prefetch; mutations with the four
  concurrency modes, `invalidates` in one batch, `patch`, `optimistic` with rollback, `offline: queue`.
- **Relations.** `checks`, `ready`, `showErrors`, `watch` with `source: "user"`, `onStale`.
- **Commands.** The closed set of `CommandKind`, one batch per event; `navigate`, `host`, `switchBackend`,
  `resetPrefs`, `setLocale` and `focus` through an `InterpreterHost` interface the hosting part implements.
- **Corpora:** control, relations, server-calculated values, several models, permissions and commands of spec 5 §14
  on `RecordingBackend` with a scripted dispatcher.
- **Open for B:** `instance: {"from": ...}` (spec 6 §16) is checked when a screen is opened, because only the opener
  knows whether it is a dialog; what a `null` command `if` condition runs.

### PR B refined (implementation detail)

- **Own control machinery, not `reactive::Query`/`Mutation`.** The query cache must be the single source of a
  query's value, because `patch` and `optimistic` write into it and two screens share one entry; `reactive::Query`
  keeps its value privately. The interpreter therefore builds queries and mutations from `Signal`, `Computed`,
  `Effect` and the `Scheduler` directly, with the same rules (latest wins by generation, value kept while
  refetching, debounce, a tick skipped while in flight, the four concurrency modes).
- **Seams.** `ModelChannel`/`Dispatcher` (one bound alias; `BridgeDispatcher` over `RawHandler`), `InterpreterHost`
  (every command the hosting part answers; each default reports "not supported"), `Session` (the query cache,
  `refreshOn` bookkeeping, permitted actions, formatting and client values), `Interpreter::mount` → `Screen`.
- **Lazy scopes.** Dialog, drawer and switch-case content is lowered behind a one-row `forEach`, so its scope is
  built when the content mounts and released when it unmounts; row scopes are built by the row view. A scope lives
  while the view nodes that read it do (they hold it by `shared_ptr`); replies and timers hold it weakly.
- **`source: "user"`.** Every source records the write sequence number and whether a user event wrote it; a watch
  runs when the newest write among the sources its `on` read was the user's.
- **Deferred to later parts, reported when met:** `form`, `collection`, `wizard` (forms engine), a model-backed
  `table` (table engine), component loading (the fallback renders), `prefetch` on hover (the host starts it through
  `Session::prefetch`), the offline queue itself (a dispatcher that has one honours `CallOptions::queueOffline`).

Tasks, one commit each: (B1) dispatch, session and query cache, scopes, lowering and the control corpus; (B2) the
relations corpus; (B3) server-calculated values; (B4) several models; (B5) permissions; (B6) commands; (B7) spec,
maps, changelog; (B8) gates — coverage, GCC 16, Doxygen, clang-format, mutations.

## PR C: authoring

**Files:** `include/morph/ui/author.hpp`; `tests/test_ui_author.cpp`; `tests/ui/golden/*.json`;
`docs/spec/ui/author.md`.

- Expression handles with operators (`a + b`, `a < b`, `a && b`, `!a`, `x.orElse(y)`), member-pointer field access,
  action-typed queries and mutations whose bodies are the action struct with handles in place of values, every
  node kind and command, the lifecycle keys, `refreshOn` over action types, `form` with its `fields` overlay,
  `dataTable`, `custom` and host components.
- Registration emits the JSON once and runs `loadDocument` with the registry as its `Catalog`, then the
  accessibility rules of spec 5 §11, each refusing with its path.
- Goldens: builder output against checked-in JSON. Spec 6 §11's reference screen is deferred: the lims example
  does not carry `lims.sample` or the model additions it needs, and building them is the example's work.

**Refinements found while planning C against the code:**

- **C1: namespace.** `morph::ui` already has `text`, `button`, `column` and the other view-tree builders of
  `view.hpp`, so the authoring builders live in `morph::ui::author`. A screen's source writes
  `namespace ui = morph::ui::author;` and reads as spec 5 §11 does.
- **C2: bodies.** C++23 cannot synthesise a struct whose members are an action's members retyped as handles, so
  `{.sampleId = id}` is not expressible. A body is a list of `{&Action::member, handle}` pairs. The member
  pointer keeps both guarantees of spec 5 §11: a renamed field and a field of another action are compile errors,
  and a handle whose value type cannot become the member's type is one too.
- **C3: `refreshOn`.** No trait says which model an action belongs to, so a free `actions<A, B>()` cannot check
  that the actions are of the query's model type. They are named through the model handle,
  `sample.actions<A, B>()`, which requires the model to `execute` each one.
- **C4: the catalog.** The core registry has no `stateless`, `calculatedFields`, `secretFields` or `credential`
  facts. Registration answers `ui::Catalog` from the types the builders saw: `keyed` from
  `model::KeyedModel<M>`, the model type of an action from the model handle it ran through, and the rest from two
  traits an application specialises, `ModelFacts<M>` and `ActionFacts<A>`.
- **C5: names.** `if_` and `return_` break the project's identifier rule, and `switch` is a keyword: the
  expression `if` is `cond`, the command `if` is `ifThen`, `return` is `returnWith`, the `switch` node is
  `switchOn`, and the `host` value is `hostValue` beside the `host` command.
- **C6: the accessibility rules** run on the loaded `Document`, as `author::checkAccessibility`, so a hand-written
  document can be checked the same way, and a label or name written as the literal `""` or `null` does not count.

**Tasks (C):** (1) this section; (2) `author.hpp` with its tests and goldens; (3) `docs/spec/ui/author.md`, the
maps, the changelog and the spec 5 and spec 6 corrections; (4) gates: the full `morph_tests`, a clang 22 coverage
measurement, a GCC 16 syntax check, the docs build, clang-format, and two mutations (one accessibility rule
disabled, one builder's output changed), each confirmed to fail a test before it is reverted.
