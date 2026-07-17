# GUI collections & views — list / table / master-detail (planned)

> **Status: planned — not yet implemented.** This spec introduces the first
> **view-schema** document — a descriptor *above* the per-action form schema of
> [forms.md](../spec/forms.md) that composes a *set* of related actions (a query,
> an edit, a delete) into a list/table or master-detail **screen**. It sits in
> Tier 2 of [gui_overview.md](gui_overview.md) and builds on
> [choice.md](../spec/choice.md) (a query action serving rows) and
> [bridge.md](../spec/bridge.md) (per-action dispatch). It extends nothing in the
> action schema — it is a new, additive top-level document. See
> [todo.md](../todo.md).

## The gap

The current surface is **one action → one flat form**
([forms.md](../spec/forms.md)). A renderer builds a form from `schemaJson<A>()`
and fires that one action. There is no concept of a *collection*: nothing says
"run query action `ListSamples`, show its rows as a table, let a row open the
edit form for action `EditSample`, and let a button run `DeleteSample`." Every
real CRUD screen — a list you can drill into and edit — must be hand-wired today,
even though all three actions are already registered
([registry.md](../spec/registry.md)) and each already generates its own form.

`Choice` ([choice.md](../spec/choice.md)) already proves the smaller version of
this: a query action, executed with an empty body, serves rows the renderer maps
to `{value, label}`. A view generalises that one pattern — *a query action
serves rows* — from a combo box to a full table with per-row actions.

## Goal

A **view schema**: a small, declared descriptor that names a set of registered
actions and says how to compose their generated forms into a **list/table** or
**master-detail** screen. It obeys [gui_overview.md](gui_overview.md)'s guiding
principle — *infer by default, declare to override*: columns and row shape are
derived from the query action's result-row type where possible; declaration only
supplies what inference cannot (a column label, a hidden column, which action a
row opens).

Each screen is still **composed of existing action-forms** — a row's editor is
exactly the `EditSample` form a Tier-1 renderer already builds. The view schema
choreographs those forms; it does not replace them.

## Design

### A new top-level document, additive to the action schema

`schemaJson<A>()` describes **one action**. A view is described by a **separate,
NEW document** — proposed `morph::views::viewSchemaJson<V>()` — emitted alongside
the action schemas, never merged into them. An action schema a Tier-1 renderer
consumes is byte-for-byte unchanged; a renderer that knows nothing about views
still renders every referenced action as a plain form. The view document only
*references* action type-ids (the same string ids `ActionTraits<A>::typeId()`
and `x-optionsAction` already use) and lets the renderer fetch each action's
schema the usual way.

The view document is JSON with a small, **NEW** key vocabulary (mnemonic `v-*`,
parallel to the action schema's `x-*`):

```json
{
  "v-kind": "collection",
  "v-title": "Samples",
  "v-query": "ListSamples",
  "v-rowKey": "id",
  "v-columns": [
    { "field": "id",   "label": "ID",   "v-hidden": true },
    { "field": "name", "label": "Name" },
    { "field": "density", "label": "Density" }
  ],
  "v-rowAction": { "action": "EditSample", "bind": { "id": "id" } },
  "v-actions": [
    { "action": "DeleteSample", "label": "Delete", "scope": "row",
      "bind": { "id": "id" }, "confirm": true },
    { "action": "CreateSample", "label": "New", "scope": "collection" }
  ]
}
```

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `v-kind` | top-level | string | `"collection"` (list/table) or `"master-detail"` (list + inline/side editor). The one required discriminator. |
| `v-title` | top-level | string | Human title for the screen. Optional; defaults to `v-query`. |
| `v-query` | top-level | string | Type-id of the registered **query action** whose result rows populate the list. Executed with an empty body — the same contract `Choice`'s options action obeys ([choice.md](../spec/choice.md)). |
| `v-rowKey` | top-level | string | Wire field name that uniquely identifies a row (default `"id"`). Used to correlate a row with the edit/delete it opens. |
| `v-columns` | top-level | array | Ordered column descriptors. **Optional** — omitted means "derive every column from the row shape" (below). |
| ↳ `field` | column | string | Wire field name in the query result row. |
| ↳ `label` | column | string | Header text. Defaults to `field`. |
| ↳ `v-hidden` | column | bool | Column present in the row model but not displayed (e.g. the key). Default `false`. |
| `v-rowAction` | top-level | object | The action a row **opens** when activated — its form is the master-detail editor. `{ "action": <typeId>, "bind": { ... } }`. Optional. |
| `v-actions` | top-level | array | Buttons that run an action. Each has `action` (typeId), `label`, `scope` (`"row"` or `"collection"`), optional `bind`, optional `confirm`. |
| ↳ `bind` | row/collection action | object | Maps the target action's field names → row field names, so a row's `id` prefills the editor/deleter. `{ "id": "id" }` = "set the action's `id` from the row's `id`." |
| ↳ `confirm` | action | bool | Renderer should confirm before firing (delete guard). Default `false`. |

All of these are **NEW proposed keys** in a **NEW proposed document**; none are
verified against existing code. A renderer that does not understand `v-*` simply
never builds a collection screen — it loses the screen, not correctness, exactly
the fallback [gui_overview.md](gui_overview.md) requires of every additive key.

### Columns derived from the result-row type (infer by default)

The query action's result is an array of rows, or a struct whose first
array-valued member holds them — the same two shapes `Choice` reads (see
`optionRows` in `DynamicForm.qml`). Its
**row element type** is a plain aggregate `{ id, name, ... }`. Where the row
type is reflectable, `viewSchemaJson<V>()` derives one column per row field, in
declaration order (`x-order` reused), typed from the field's JSON type — no
`v-columns` needed. A `Quantity` column carries the same `ExtUnits` /
`x-decimalPlaces` the field's form schema carries, so the table formats a value
the same way the form does.

`v-columns` is the **declare-to-override** escape hatch: supply it to reorder,
relabel, hide, or subset the derived columns. This mirrors `optionalFields` in
[forms.md](../spec/forms.md) — inference is the default, a small typed
declaration overrides it.

### How a view is declared in C++ (proposed)

A view is a compile-time descriptor over already-registered action types, so its
references are checked the way `Choice`'s are (i.e. names are opaque NTTPs,
validated at runtime — see [choice.md](../spec/choice.md)'s author obligations):

```cpp
// namespace morph::views — NEW.
struct SamplesView {
    using kind      = CollectionView;                 // or MasterDetailView
    using query     = ListSamples;                    // registered query action
    using rowAction = EditSample;                     // opened on row activate
    using actions   = ActionList<DeleteSample, CreateSample>;

    static constexpr std::string_view rowKey = "id";
    // optional column overrides; absent => derive from ListSamples' row type
};
BRIDGE_REGISTER_VIEW(SamplesView, "SamplesView");     // NEW macro, parallels
                                                      // BRIDGE_REGISTER_ACTION
```

The **NEW** `BRIDGE_REGISTER_VIEW` macro (parallel to `BRIDGE_REGISTER_ACTION`,
[registry.md](../spec/registry.md)) specialises a `ViewTraits<V>` with a
`typeId()` and the `viewSchemaJson<V>()` body, and registers the view id so a
controller can enumerate views the way `Main.qml` enumerates schemas today. No
new dispatch path is introduced — the view descriptor is metadata only.

### Dispatch: the screen is still just action calls

A collection screen performs three ordinary dispatches, each already fully
specified:

1. **Populate** — execute `v-query` with an empty body via the normal execute
   path ([bridge.md](../spec/bridge.md)); read rows from the result exactly as
   `Choice` does. The query action is a pure read, so it declares
   `Loggable::No` ([registry.md](../spec/registry.md)) — a table refresh is not
   an audit event.
2. **Edit** — activating a row builds the `v-rowAction` action's form
   ([forms.md](../spec/forms.md)) with fields prefilled from `bind`, then fires
   it through the same `execute<Action>` / reactive `set<>` path as any form.
3. **Delete** — a `scope: "row"` action fires its bound action (e.g.
   `DeleteSample{ id }`), guarded by `confirm`.

After an edit or delete resolves, the renderer re-runs step 1 to refresh — there
is no new "list changed" push channel (see Non-goals). `bind` is the only new
mechanic, and it is a pure client-side prefill: it copies row field values into
the target action's draft before the form fires; the wire payload is the
ordinary action body.

### Master-detail is a layout of the same pieces

`v-kind: "master-detail"` is the same descriptor rendered as a split screen: the
list on one side, the `v-rowAction` form live-bound to the selected row on the
other. It introduces **no** new keys over the collection kind — only a rendering
choice. A renderer that cannot do split layout may fall back to rendering
master-detail exactly as a collection (list + modal editor); the contract does
not mandate the split.

## Non-goals

- **Not an app framework.** A view is one screen composed of existing
  action-forms. Multi-screen navigation, menus, and cross-screen flow live in
  [gui_workflows_navigation.md](gui_workflows_navigation.md), not here.
- **No new dispatch path or wire change.** Populate/edit/delete are ordinary
  action executes over the existing path ([bridge.md](../spec/bridge.md),
  [backend.md](../spec/backend.md)); the view document is metadata a renderer
  consumes, never a payload.
- **No server-side "query language."** `v-query` names a registered action that
  returns whatever rows it returns. There is no filtering/sorting/paging protocol
  invented here — a query that needs parameters is just an action with fields,
  and paging, if wanted, is a field on that action, not a view-schema concept.
- **No live/push list updates.** A list refreshes by re-running its query after a
  mutating action resolves. Reactive server-push of collection changes is out of
  scope (it would need a subscription channel morph does not have).
- **No nested/joined views.** A view references flat action row shapes, matching
  the flat-actions-only scope of schema generation
  ([forms.md](../spec/forms.md)). A row that is itself a collection is not a
  documented path.
- **Membership/staleness not enforced.** Exactly as with `Choice`
  ([choice.md](../spec/choice.md)), a row key bound into an edit/delete may be
  stale by the time the action fires; the handler must re-check, since the wire
  carries only the bound value.

## Testing (planned)

- `viewSchemaJson<SamplesView>()` emits `v-kind`, `v-query`, `v-rowKey`, a
  derived `v-columns` (one per `ListSamples` row field, in `x-order`), and the
  declared `v-rowAction` / `v-actions`; a `Quantity` column carries the row
  field's `ExtUnits` / `x-decimalPlaces`.
- Column override: declaring `v-columns` reorders/relabels/hides columns and
  suppresses derivation for the omitted fields.
- A renderer populates a table from `v-query` rows (array result and
  single-array-member result both handled, as `optionRows` does), opens the
  `v-rowAction` form with `bind`-prefilled fields, and re-populates after the
  edit resolves.
- A `confirm` row action fires its bound delete only after confirmation; the
  wire body is the ordinary action payload with the bound key.
- A renderer ignorant of `v-*` still renders every referenced action as a plain
  form and never builds a screen (additive-key fallback / regression guard).
- `master-detail` renders the same descriptor as a split screen and degrades to
  list-plus-modal where split layout is unavailable.

## Cross-references

- [gui_overview.md](gui_overview.md) — Tier 2; the "view/app schema layer above
  the action schema" this document opens, and the infer-by-default/declare-to-
  override principle the column derivation follows.
- [forms.md](../spec/forms.md) — the per-action schema each screen composes; the
  `x-order` / `ExtUnits` / `x-decimalPlaces` reused for derived columns and the
  flat-actions-only scope inherited here.
- [choice.md](../spec/choice.md) — the "a query action serves rows" pattern
  generalised from a combo box to a table, including the empty-body query
  contract and the stale-value caveat.
- [bridge.md](../spec/bridge.md) — the execute / reactive `set<>` path the
  populate/edit/delete dispatches use unchanged.
- [registry.md](../spec/registry.md) — `ActionTraits::typeId()` (the ids the view
  references), `Loggable::No` for the query action, and the
  `BRIDGE_REGISTER_ACTION` pattern the proposed `BRIDGE_REGISTER_VIEW` mirrors.
- [gui_workflows_navigation.md](gui_workflows_navigation.md) — the next tier up:
  sequencing screens/views into wizards and a navigable app shell.
- [gui_renderer_toolkit.md](gui_renderer_toolkit.md) — the reference renderer and
  conformance kit that would implement and verify the `v-*` contract.
