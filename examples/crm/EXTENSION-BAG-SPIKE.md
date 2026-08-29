# Extension-bag spike — findings

Status: **complete**. This is the no-app spike named in
[`../LADDER.md`](../LADDER.md) ("Extension-bag spike (7b)") and referenced
from [`README.md`](README.md) steps 9–10 as "the endgame." Its code lives at
[`tests/test_extension_bag_spike.cpp`](../../tests/test_extension_bag_spike.cpp)
(part of `morph_tests`, tagged `[spike][extension-bag]`) and is throwaway —
nothing here ships as part of any rung, and no application was built to reach
this answer, per the spike's own charter.

## The question

`crm/README.md` step 9 names the framework question rung 7 exists to ask:

> Compiled C++ action structs cannot grow members, so this decides the
> framework question this rung exists to ask: can a morph model carry an open
> extension bag (`map<string, Value>` alongside typed members) whose fields
> appear in schemas, forms, validation, and the journal like first-class ones?

## Headline answer

**Yes, reachable today** — using primitives the framework already ships but
morph itself never plumbs together, plus one small piece of new app-level
code per opted-in action. No morph framework change was required to make the
probe work end to end. This is a materially better answer than the round-5
ground truth in `crm/README.md` implied when it wrote "neither has a
framework hook \[framework gap\]" for per-caller schema shaping: the missing
piece was a known glaze feature (`glz::meta<T>::unknown_read`/`unknown_write`)
that nothing in morph had tried, not a genuine framework gap.

The four layers, in the order the probe checked them:

| Layer | Verdict | What was needed |
|---|---|---|
| **Journal** (storage, replay, fingerprint) | No change | Nothing — `LogEntry::payload` is opaque JSON text; replay decodes it through the same `ActionTraits<A>::fromJson` used everywhere else. |
| **Forms client renderer** (`DynamicForm.qml`) | No change | Nothing — it already iterates `schema.properties` generically (verified by re-reading `DynamicForm.qml:227-330`, the round-5-cited "already schema-generic" renderer half; no QML test added by this spike, since nothing in it changes). |
| **Schema growth** (`schemaJson<A>()`-adjacent) | New app-level function, no framework change | A post-hoc DOM rewrite (`ebSchemaJsonWithCustomFields()`) that injects new `properties` nodes into the already-served schema, mirroring `morph::forms::InstanceConstraints::decorate()`'s exact idiom. |
| **Decode/validate** (dispatch) | New per-action declaration, no framework change | `glz::meta<A>::unknown_read`/`unknown_write` pointed at a `std::map<std::string, Value>` member, **plus** an explicit `value = glz::object(...)` listing the action's compiled members (see "The catch" below). |

## What the probe built

One throwaway action/model pair (`EB_UpdateContact` / `EB_ContactModel`,
`tests/test_extension_bag_spike.cpp`):

- Two compiled fields (`name`, `email`) plus `std::map<std::string, EB_Value>
  extra` — a hidden sink, not a normal reflected member (see "The catch"
  below for why it has to be hidden).
- `EB_Value = glz::generic_u64` — glaze's own JSON-DOM variant, already a
  morph dependency and already used internally the same way inside
  `forms.hpp` (`mergeSchemaExtras`, `InstanceConstraints::decorate`) for
  schema-DOM manipulation. Reused directly here rather than inventing a
  parallel `morph::forms::Value`. Deliberately narrow — string, number, bool
  — matching the probe's scope; see "What a real 7b would still need" below
  for what this doesn't cover.
- `EB_CustomFieldRegistry` — a process-wide stand-in for a real
  `AddCustomField` persisted to a metadata table. `add()`/`clear()`/`fields()`
  under a mutex; enough to answer the schema/decode/journal question without
  building actual persistence.
- `ebSchemaJsonWithCustomFields()` — the schema-injection function (see
  table above).
- Eight Catch2 test cases (`[spike][extension-bag]`), covering: absence when
  unregistered, presence + `x-custom` marker once registered, `required`
  array growth, the framework's default silent-drop behavior on a plain type
  with no bag, bag-preserving decode, `validate()` seeing bag contents,
  `toJson`/`fromJson` round-trip, and full `journal::replay()` reconstructing
  a bag value through a fresh registry/dispatcher pair.

All eight pass; the full `morph_tests` suite is unaffected (21200 → 21229
assertions, 1246 → 1254 test cases; the delta is exactly this file).

## The catch: `glz::meta::value` is not optional alongside `unknown_read`/`unknown_write`

This is the spike's sharpest finding, because it wasn't anticipated going in.
Every real usage of `unknown_read`/`unknown_write` in glaze's own test suite
and docs (`docs/unknown-keys.md`, `tests/json_test/json_test.cpp`'s
`unknown_fields_member`/`unknown_fields_2`/`unknown_write` fixtures)
**always** pairs them with an explicit
`static constexpr auto value = glz::object("field", &T::field, ...)`
declaration. Omitting `value` and declaring only `unknown_read`/`unknown_write`
compiles for read, but fails to link the write path:
`write.hpp`'s `static_assert(false_v<T>, "unknown_write type not handled")`
fires, because the unknown-field hooks are wired up for the
`glz::meta::value`-declared object path — not for morph's usual pure-reflection
path (`glz::reflectable<T>`, no `glz::meta<T>` specialization at all) that
every other action in this codebase relies on.

Consequence: **an action opting into an extension bag gives up morph's
pure-reflection convenience and must hand-list its own compiled members** in
a `glz::meta<A>::value` declaration. This is a real, if modest, per-action
cost a genuine 7b would have to pay — either every author writes this by
hand, or the framework grows a new opt-in macro (something like
`BRIDGE_REGISTER_ACTION_WITH_EXTENSIONS(Model, Action, "Name", extraMember)`
that expands to the `glz::meta` specialization the same way
`BRIDGE_REGISTER_ACTION` expands `ActionTraits<A>`) that generates it. Given
`forEachNamedMember`'s reliance on `glz::reflect<A>::keys`/`::size` (which
works over `glz::meta::value`-declared objects the same as over pure
reflection — the probe's `ebSchemaJsonWithCustomFields()` calls
`morph::forms::schemaJson<EB_UpdateContact>()` unmodified and it renders
`name`/`email` correctly), this looks like a small, mechanical framework
addition rather than new machinery — worth scoping precisely if 7a/7b is
ever green-lit.

## What a real 7b would still need beyond this probe

The probe deliberately stayed narrow; a real build has to additionally
answer:

- **Per-field authz interaction.** `crm/README.md`'s round-5 ground truth
  about per-caller schema shaping ("app-side JSON post-processing plus
  independent server-side per-field enforcement; neither has a framework
  hook") still holds for *compiled* fields. For *custom* fields the same
  gap exists but is now compounded: `ebSchemaJsonWithCustomFields()` has no
  concept of "this caller cannot see this custom field" — that would be a
  second post-hoc DOM pass, keyed on `session::Principal`, layered on top of
  this one.
- **Custom-field lifecycle races** (`crm/README.md`'s 7b section: delete a
  field while a client holds an open form / an offline client has queued
  edits / journal replay carries it). This spike's registry has no delete
  path at all — `clear()` is a full reset for test isolation, not a
  targeted single-field removal with a reject/drop/preserve-as-orphan policy.
- **Unit-bearing and Choice-backed custom values.** `EB_Value` covers
  string/number/bool only. A `Quantity`-in-bag needs decimal-places/unit
  metadata traveling with the value, not just the value; a `Choice`-in-bag
  needs the same referential re-check `crm/README.md` already mandates for
  compiled lookup fields (review D6: "Choice membership is never
  validated"). Neither was in this probe's scope.
- **Persistence.** The registry here is in-memory and the model's `extra`
  map is never written to SQLite. A real build stores custom-field
  *definitions* (name, type, required) in a metadata table and custom-field
  *values* most likely as a JSON blob column per record (the Lightweight ORM
  has no native JSON column type — see the framework-surface note the
  earlier investigation for this spike produced), losing per-key
  queryability unless a further indexing strategy is added.
- **A real `AddCustomField` action**, journaled and authorized like any
  other mutation — this spike's registry mutation (`ebRegistry().add(...)`)
  is test setup, not a dispatched, audited action.

None of these change the headline answer — they're the difference between
"the mechanism works" (what this spike answers) and "a production-shaped
7a/7b" (a separate, larger scoping and build decision, still gated on the
program's post-rung-4 go/no-go per `../LADDER.md`).
