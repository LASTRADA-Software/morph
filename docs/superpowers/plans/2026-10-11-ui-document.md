# `morph::ui` document and `expr/1` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The first of three pull requests for part 5 of the declarative UI program: the UI document as data and the
expression language it is written in. `expression.hpp` (values, types, the `expr/1` tree and its evaluator, the
function library), `message.hpp` (the MessageFormat 2 subset and CLDR plural rules behind `t`) and `document.hpp`
(the screen document, its parser and its load-time validation), header-only in the base `morph` target, tested by
the two corpora spec 5 §14 names first: document validation and expressions.

**Not in this pull request.** The interpreter that turns a document into reactive nodes over `Mounted` (queries,
mutations, the query cache, relations, commands, the lifecycle keys) is the second pull request; `author.hpp`, the
server-side builders and their goldens, is the third. Spec 5 §14's control, relations, server-calculated,
several-models, permissions, commands and authoring corpora are theirs. Where the `ui-*` envelope kinds land
(the open point on the issue) is settled by the interpreter's pull request, which is the first to send one. `UnitTable` lands here; generating it from `UnitTraits` into the bundle lands with the bundle (spec 6), which is what first serves one.

**Architecture:** A `ui::Value` is null, a bool, an `int64`, a string, an exact decimal (`math::Rational` with its
declared places), an exact quantity (a decimal and a unit id), a timestamp, a list or a record (field names in
order). A `ui::Type` is one of the spec 5 §3 types; decoding JSON into a `Value` is type-directed, because a
timestamp and a string, or a decimal and a record with three fields, have the same JSON. An expression is parsed
once into an immutable tree that records its JSON path; the evaluator walks it against an `ExpressionContext`
(names, rows, queries, mutations, permissions, preferences, environment, session, host values, the running event)
and returns a `Value`, never throwing: a runtime error is `null`, reported once per expression path to a
diagnostics sink. Logic is Kleene's over `true`, `false` and `null`. A document parses into plain structs —
declarations, scopes, nodes and commands, each holding parsed expressions — and `validate` checks it against a
`Catalog` (model types, action ids, request and reply schemas, units, vocabularies) and returns every refusal with
the document id, the JSON path and the reason.

**Tech Stack:** C++23, header-only; glaze's generic JSON (`glz::generic_u64`, so no int64 is rounded through a
double); `morph/util/rational.hpp`, `quantity.hpp`, `datetime.hpp`; `morph/render/locale_format.hpp` and
`i18n.hpp`; Catch2 v3; Doxygen with `WARN_AS_ERROR`.

**Spec:** `docs/superpowers/specs/2026-10-07-ui-document-design.md` (spec 5): §3 (structure, scopes, types,
load-time validation), §4 (expressions, semantics, `expr/1`), §5–§10 for the declarations, nodes, commands and
components a document holds, §12 (vocabularies, `!` keys), §12b (`can`, `pref`, `customize`), §13 (what a
document can never do), §14 (the two corpora). Spec 6 (`2026-10-08-app-hosting-design.md`) §3 for the lifecycle
keys and the node and expression additions spec 5 §3 puts in `ui/1`. Spec 1 §2 (headers). Issue: #890. Program:
#885.

**Delivery.** Branch `feature/ui-document` from `origin/master`; this plan is its first commit; then one commit per
task; one pull request, `Part of #890`.

## Global Constraints

- C++23, header-only, every new public header in the `morph` target's `FILE_SET HEADERS`; no new CMake option, no
  new dependency.
- `expression.hpp`, `message.hpp` and `document.hpp` depend on `morph/util`, `morph/render` and glaze only: no
  `morph::reactive`, no bridge, no toolkit. The interpreter adds the reactive side.
- Every file starts with `// SPDX-License-Identifier: Apache-2.0`, then `#pragma once` for headers.
- Naming (`.clang-tidy`): types `CamelCase`, functions and variables `camelBack`, private members `_camelBack`,
  constants `kName`; parameter names of at least three characters.
- AGENTS.md: comments and docs state what the code does now and why; no history, no issue numbers. Every test
  names the mutation that makes it fail, and that mutation is run.
- Doxygen `WARN_AS_ERROR = FAIL_ON_WARNINGS`; strict warnings; clang-tidy-diff and clang-format 22 clean; GCC 15
  release builds clean (no conditional between two braced lists, no `ranges::next_permutation`).

## Spec amendments

| # | Where | Today | Change | Why |
|---|---|---|---|---|
| S1 | spec 5 §3 "Types"; `core/registry.hpp` `ActionDescription`; the `schemas` envelope | Says result types come from "the action schemas the server already emits"; the server emits request schemas only | `ActionDescription::schema` gains `x-reply`, the JSON Schema of `ActionTraits<A>::Result`, so the `schemas` envelope serves it with no new field (an added key: older clients ignore it). Spec 5 §3 says so | A document's `{"query": "q"}` and `{"result": "m"}` cannot be type-checked, nor a timestamp told from a string, without the reply's schema |
| S2 | spec 5 §3, §4 ("converting quantity units by their declared exact factors") | No part of a bundle declares the factors; a generic client has no unit enum | The bundle carries a unit table: per unit id, its display text, default places, and exact ratios to the other units of its dimension, generated from `UnitTraits` the way `x-unitAlternatives` already is. `Catalog::units()` exposes it; a quantity in a unit the table cannot convert compares and adds as `null` | The client compiles in no unit enum |
| S3 | spec 5 §4 "display" | `date(x, style)` and `dateTime(x, style)` "using the client's locale and display zone through `render/locale_format.hpp`", which formats numbers only | Numbers keep `locale_format.hpp`. Dates and times go through a `DisplayFormatter` the context supplies; the default writes ISO-8601 in the display zone (`short`: date; `medium`: date and minutes; `long`: date and seconds), and a renderer with its toolkit's locale data supplies its own | morph carries no CLDR date patterns; a toolkit does |
| S4 | spec 1 §2 headers | `document.hpp`, `expression.hpp`, `interpreter.hpp`, `author.hpp` | Adds `message.hpp`: the MessageFormat 2 subset, its catalogue check and the CLDR plural rules | It is used by `t` and by the catalogue loader, not only by expressions |
| S5 | spec 5 §3 "Types" | Silent on a JSON number with a fraction | A fractional JSON number is refused in a document; an exact fraction is written `{num, den, dp}` | glaze has rounded it through a double before the document is read, so it cannot be exact |

## Tasks

### Task 1: Reply schemas (S1)

- [x] `ActionDescription::reply`: `forms::schemaJson<typename ActionTraits<A>::Result>()` when the result is a
  reflectable struct or a scalar the schema writer knows; empty for `void`. `buildActionDescription` fills it.
- [x] The `schemas` envelope writes `reply` beside `schema`; `docs/spec/core/wire.md` (or the wire document that
  defines `schemas`) states the field and that it is additive.
- [x] Tests: an action with a struct reply, one with a scalar reply, one with none; the envelope carries each.
  Mutations: leave `reply` empty; drop it from the envelope.

### Task 2: Values and types (`expression.hpp`)

- [x] `ui::Value` with `null`, `bool`, `int64`, `string`, `Decimal`, `Quantity`, `Timestamp`, list, record;
  equality and a total order where the spec compares (exact across `int`/`decimal`; quantities only within a
  unit or through the unit table).
- [x] `ui::Type` and `parseType("list<decimal>?")`; records from a JSON Schema (`properties`, `required`,
  `format: date-time`, `x-decimalPlaces`, `x-unit`), so a reply schema becomes a `Type`.
- [x] `decode(json, Type)` and `encode(Value)`; `canonicalJson(Value)` with sorted keys (the query cache's key).
- [x] Tests: round trips for every type, a timestamp vs. a string, a decimal vs. a three-field record, an int64
  beyond 2^53, a malformed decimal, canonical key order. Mutations: decode untyped; compare decimals as doubles;
  leave keys unsorted.

### Task 3: The expression tree

- [x] `ui::Expr` parsed from JSON with every form of spec 5 §4 and spec 6's additions, each node recording its
  JSON path; the 256-node bound; `{"ref": "event"}` flagged so the validator can refuse it outside commands.
- [x] Parse errors carry a path and a reason; an unknown form is refused, an unknown function is refused against
  the vocabulary.
- [x] Tests: every form, the bound at 256 and 257, a malformed form of each kind. Mutations: off-by-one in the
  bound; accept an unknown form.

### Task 4: The evaluator

- [x] `ExpressionContext` (an interface): names by path, the innermost row, query and mutation state, `can`,
  `pref`, `env`, `errorKind`, `session`, `host`, the event, the unit table, the locale, the display zone, the
  translation provider and the display formatter. `evaluate(expr, context, diagnostics)`.
- [x] Kleene `and`/`or`/`not`; absence propagates; `if`, `obj`, `list`, `map`, `filter`; a runtime error yields
  `null` and is reported once per expression path.
- [x] Tests over a fake context: the truth tables, propagation through every form, one report per path across
  repeated evaluations. Mutations: two-valued logic; report on every evaluation; throw on a type mismatch.

### Task 5: The function library

- [x] Comparison and arithmetic over `int`, `decimal`, `quantity`, exact, overflow and division by zero as `null`;
  `round(x, dp)` half to even; unit conversion through the unit table.
- [x] `engaged`, `coalesce`; `concat`, `format`, `trim`, `length` (code points), `isEmpty`, `contains`; `count`,
  `sum`, `mean` (exact sum over the count at the operands' places, half to even), `min`, `max`, `any`, `all`,
  `first`, `includes`, `indexOf`.
- [x] Tests: each function with exact, null and mismatched inputs; overflow at the int64 edges; `mean` exact where
  a double is not. Mutations: half away from zero; `mean` through `double`; ignore units.

### Task 6: Messages and display (`message.hpp`, S3)

- [x] The MessageFormat 2 subset: placeholders, `:number` and `:string`, `.input`, `.match` with exact keys and
  CLDR plural categories; anything else refused when a catalogue loads, and the fallback used.
- [x] CLDR plural rules for the locales the tests name (English, Polish, Arabic) plus German and French, selected
  by language; an unknown language uses `other`.
- [x] `t(key, fallback, args)` through `render::TranslationProvider`; `number`, `decimal(x, dp)`,
  `quantity(x, unit?)`, `money(x, currency)` through `locale_format.hpp`; `date`, `dateTime` through the
  `DisplayFormatter`.
- [x] Tests: spec 5 §14 "Messages" (plural categories for English, Polish and Arabic; a string `.match`; a pattern
  outside the subset falls back); each display function. Mutations: plural `one` for every `1`-ending number in
  Polish; accept a pattern outside the subset.

### Task 7: The document model and parser (`document.hpp`)

- [x] Structs for a screen: `vocab`, `id`, the lifecycle keys, `params`, and per scope `models`, `state`, `let`,
  `queries`, `mutations`, `watch`, `checks`; `view` as nodes of the palette (spec 5 §9, the further kinds, `form`,
  `collection`, `wizard`, `custom`, `table` with its row scope, `boundary`, `customize`); commands (spec 5 §8);
  `components`, `customize`, `prefetch`.
- [x] Unknown optional keys are kept out of the model and ignored; a `!`-prefixed key the parser does not know
  refuses the node (or the document, at the top level).
- [x] Tests: a full document round-trips through the parser to the same structure; each `!` case. Mutations:
  ignore `!` keys; accept an unknown node kind.

### Task 8: Load-time validation

- [x] `validate(document, catalog) -> std::vector<Refusal>`, each `{document, path, reason}`: names and
  uniqueness; references resolved innermost scope first; types checked where both sides are known; `let` and
  `watch` cycles; the expression bound; vocabularies; `instance` only on keyed models and required in a row scope;
  `optimistic.with` naming a server-calculated field; `"offline": "queue"` on a credential mutation;
  `cancelClose` outside `onClose`, `retry` outside a boundary's `error`, `{"ref": "event"}` outside a command
  list.
- [x] Tests: the document-validation corpus (valid and invalid documents, each invalid one with its path and
  reason). Mutations: resolve outermost first; skip the `watch` cycle; skip the row-scope instance rule.

### Task 9: Corpora, docs and gates

- [x] `tests/data/ui/expressions.json` and `tests/data/ui/documents.json`, run by table tests, so the interpreter
  and the server's registration reuse them.
- [x] `docs/spec/ui/document.md` and `docs/spec/ui/expressions.md`; spec 5 and spec 1 amended (S1–S4);
  ARCHITECTURE, CHANGELOG; the coverage floor for `include/morph/ui` re-measured.
- [x] Full `ctest`; `[ui]` under ASan+UBSan and TSan; Doxygen; clang-format; clang-tidy-diff; GCC 15 release where
  available. The pull request states what was measured, on which configuration, and what was not.
