---
id: 031
title: DynamicForm has no control for JSON `array`-typed fields; it silently renders a text box that can never produce a valid submission
subsystem: forms
severity: major
source: rung 2 (bookmarks) task 18 — GUI shell, review-recommended
disposition: open
test: none
issue: https://github.com/LASTRADA-Software/morph/issues/66
---

Found while reviewing rung 2 (bookmarks)'s schema-driven GUI shell. A
`std::vector<std::string>` DTO field (`CreateBookmark::tags`,
`MergeTags`'s tag-name lists, etc.) is an unremarkable member type — it
compiles, `morph::forms::schemaJson<T>()` happily emits a JSON Schema
`"type": "array"` entry for it, and nothing in the framework rejects binding
such a DTO to a schema-driven form. But `DynamicForm.qml` has no rendering
path for it at all.

## The actual bug

`DynamicForm.qml`'s only JSON-type dispatch is a sequence of
`types.indexOf("...")` checks (e.g. `types.indexOf("integer") !== -1` at
line 194) selecting between numeric/boolean/string/enum controls. There is
no `types.indexOf("array")` branch anywhere in the file. An array-typed
field falls through every check and reaches the generic text-control path,
and `fieldJsonLiteral` (line 575-618) — the function that turns whatever the
user typed into the JSON literal sent to the server — has no array handling
either: its final fallback is `return JSON.stringify(text)` (line 617),
which wraps the raw text content in a JSON *string* literal, not a JSON
array.

This is not a missing feature that degrades gracefully (an omitted field, a
disabled control, a form that refuses to reach `ready`). It is a **normal,
enabled, apparently-functional text input** that a user can type into,
believing it does something, and submit — producing a body the server's own
schema validation is guaranteed to reject, every time, for every
array-typed field, with no indication in the UI of why.

## Impact on rung 2

This cost the bookmarks rung two workarounds and one disclosed,
unaddressed capability gap:

- `BulkEdit` (whose `addTags`/`removeTags` fields are array-typed) is
  excluded from the schema-driven form document entirely
  (`examples/bookmarks/gui_lib/bookmark_schemas.hpp`'s own comment records
  this) and is instead driven from ad hoc checkbox selection in QML,
  bypassing the schema-driven path `IMPLEMENTATION.md` rule 2 otherwise
  requires.
- Tagging a bookmark — a headline feature of a bookmarks manager — is not
  reachable from the GUI at all. `CreateBookmark::tags` and any
  tag-mutation path are only exercisable through direct model calls (tests,
  import) because no schema-driven form can safely expose them.

Every future rung with a list-valued input (multi-select, tag editors,
bulk-id pickers) will hit this the moment it tries to bind such a field to
`DynamicForm`.

## What morph would need

`DynamicForm.qml` needs an actual `"array"` branch: at minimum, for an
`array` of `string` items, a simple add/remove chip-list or
comma-separated-with-validation control that emits a genuine JSON array
literal from `fieldJsonLiteral`, not a stringified blob. The entry point
for a fix is the `fields` descriptor construction around
`DynamicForm.qml:160-213` (where the per-field control type is currently
selected) plus the corresponding literal-encoding arm in
`fieldJsonLiteral` (`:575-618`). Scoped to
`src/qt/forms/qml/DynamicForm.qml`; out of scope for the ladder task that
found it (rung 2 GUI shell, not `src/qt/forms/`).
