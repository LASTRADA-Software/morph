# `morph::views` — view-schema generation for list/table & master-detail screens

`morph::views::viewSchemaJson<V>()` composes a registered **query** action's
result rows with an optional row-opener action and row/collection action
buttons into a **view-schema** document a renderer builds a list/table or
master-detail **screen** from. Where `morph::forms::schemaJson<A>()`
describes one action, a view describes a *set* of related actions — a query,
an edit, a delete — and how to choreograph their already-generated forms.

> **Terminology note.** "Screen" here is the informal, UI sense — whatever a
> view renders into. It is a different, not-yet-connected concept from the
> app-shell layer's formal `Screen` family (`FormScreen`/`WizardScreen`/`kind`,
> [workflows_navigation.md](workflows_navigation.md)): a view is not yet one
> of that layer's declarable screen kinds (see [Non-goals](#non-goals)).

## Contents

- [The gap this closes](#the-gap-this-closes)
- [A new, separate top-level document](#a-new-separate-top-level-document)
- [`ActionDescriptor` and `describeAction<Action>()`](#actiondescriptor-and-describeactionaction)
- [Declaring a view in C++](#declaring-a-view-in-c)
- [Column derivation](#column-derivation)
- [The JSON key vocabulary](#the-json-key-vocabulary)
- [`ViewTraits<V>` / `BRIDGE_REGISTER_VIEW` / `ViewRegistry`](#viewtraitsv--bridge_register_view--viewregistry)
- [Dispatch: the screen is still just action calls](#dispatch-the-screen-is-still-just-action-calls)
- [The Qt/QML reference renderer](#the-qtqml-reference-renderer)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Non-goals](#non-goals)
- [Cross-references](#cross-references)

## The gap this closes

The forms layer is **one action → one flat form**
([forms.md](forms.md)). Nothing says "run query action `ListSamples`, show
its rows as a table, let a row open the edit form for action `EditSample`,
and let a button run `DeleteSample`" — every create/read/update/delete
(CRUD) screen had to be hand-wired,
even though all three actions are already registered
([registry.md](../core/registry.md)) and each already generates its own form.
`Choice` ([choice.md](choice.md)) already proves the smaller version of this
pattern — a query action, executed with an empty body, serves rows a
renderer maps to `{value, label}`. A view generalises "a query action serves
rows" from a combo box to a full table with per-row actions.

## A new, separate top-level document

`schemaJson<A>()` describes one action; `viewSchemaJson<V>()` describes a
*view* — a **separate JSON document**, never merged into any action schema.
An action schema a [Tier-1](forms.md#renderer-conformance-kit) (per-action
`x-*` schema) renderer consumes is byte-for-byte unchanged by this
layer; a renderer that knows nothing about views still renders every
referenced action as a plain form. The view document only *references* action
type-ids (the same string ids `ActionTraits<A>::typeId()` and
`x-optionsAction` already use).

## `ActionDescriptor` and `describeAction<Action>()`

Every action a view references — its query, its row-opener, each button —
is a `morph::views::ActionDescriptor`, always built by
`describeAction<Action>(...)`, never constructed by hand:

```cpp
struct BindEntry {
    std::string_view actionField;   // wire field on the target action
    std::string_view rowField;      // wire field on the row to read the prefill from
};

struct ActionDescriptor {
    std::string_view actionTypeId;   // resolved from ActionTraits<Action>::typeId()
    std::string_view label{};        // "" = use the action type id as-is
    ActionScope scope{ActionScope::Row};   // Row or Collection
    std::span<const BindEntry> bind{};     // target-field <- row-field prefill map
    bool confirm{false};
};

template <typename Action>
consteval ActionDescriptor describeAction(std::string_view label = {},
                                           ActionScope scope = ActionScope::Row,
                                           std::span<const BindEntry> bind = {},
                                           bool confirm = false);
```

`describeAction<Action>()` requires `ActionTraits<Action>` to already be a
complete specialisation (i.e. `BRIDGE_REGISTER_ACTION` for `Action` must
already have run earlier in the translation unit) — a typo'd or unregistered
action is a **compile error** here, not the runtime-only failure
`Choice`'s unchecked `OptionsAction` [`FixedString`](forms.md#fixedstring--nttp-compile-time-string) NTTP allows (see
[choice.md](choice.md), "Limitations"). `BindEntry{actionField, rowField}`
maps one target-action wire field to one row wire field; `bind` is a pure
client-side prefill — the wire payload the action eventually fires is the
ordinary action body. `bind` *can* name more than one field, but see
"Limitations" below before binding every required field of a row-opener
action: combined with a no-submit-button/auto-fire-on-ready renderer (the
Qt/QML reference renderer), that fires the action the instant the row opens.

## Declaring a view in C++

```cpp
struct SamplesView {
    using kind  = morph::views::CollectionView;     // or MasterDetailView
    using query = ListSamples;                       // registered query action

    static constexpr std::string_view title = "Samples";   // optional; default: v-query's typeId
    static constexpr std::string_view rowKey = "id";        // optional; default "id"

    static constexpr std::array<morph::views::BindEntry, 1> kEditBind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"},
    };
    static constexpr auto rowAction =                        // optional
        morph::views::describeAction<EditSample>({}, morph::views::ActionScope::Row, kEditBind);

    static constexpr std::array<morph::views::BindEntry, 1> kDeleteBind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"},
    };
    static constexpr std::array<morph::views::ActionDescriptor, 2> actions{   // optional
        morph::views::describeAction<DeleteSample>("Delete", morph::views::ActionScope::Row, kDeleteBind, true),
        morph::views::describeAction<CreateSample>("New", morph::views::ActionScope::Collection),
    };

    // optional: static constexpr std::array<morph::views::ColumnOverride, N> columns;
};

using lab::SamplesView;
BRIDGE_REGISTER_VIEW(SamplesView, "SamplesView")
```

`kind` and `query` are the only required members. `title`, `rowKey`,
`columns`, `rowAction`, and `actions` are each detected structurally (via a
`requires`-expression, not inheritance or a marker base) and are individually
optional — absence falls back to the defaults below. This exact descriptor is
`examples/forms/lab_schemas.hpp`'s `SamplesView`, composed from
`examples/forms/lab_model.hpp`'s `ListSamples`/`EditSample`/`DeleteSample`/
`CreateSample`.

## Column derivation

`viewSchemaJson<V>()` finds the query action's **row element type** — the
result itself when it is a `std::vector<Row>`, otherwise its first
`std::vector<Row>`-typed member in declaration order (the same two shapes
`Choice`'s renderer-side `optionRows` reads) — and derives one column per
`Row` field by calling **`morph::forms::schemaJson<Row>()` directly**: `Row`
need not be a registered action, only a reflectable, default-constructible
aggregate, which is exactly what `schemaJson<A>()` already requires of any
type it is instantiated on. Each derived column carries:

- `field` / `label` — the wire key; `label` defaults to `field`.
- Declaration order — the row schema's `x-order`, ascending.
- `x-decimalPlaces` / `ExtUnits` — copied off the row schema's property node
  for a `Quantity` field, so a column formats a value exactly as that
  field's own form would. A `Quantity` type used only once on `Row` carries
  `ExtUnits` directly on its property node (glaze inlines a single-use
  struct schema in place); glaze only promotes the nested schema to a
  `$defs` entry (referenced back via the property's `$ref`) when the *same*
  `Quantity` type occurs on more than one property of `Row` (see
  [forms.md's renderer conformance kit](forms.md#renderer-conformance-kit),
  whose `CFSharedDefFields` fixture covers exactly this case) — column
  derivation reads whichever shape `schemaJson<Row>()` actually produced,
  trying the inlined property first and falling back to the `$ref`/`$defs`
  indirection.

`V::columns` (a `static constexpr std::array<ColumnOverride, N>`) is the
declare-to-override escape hatch: supplying it emits **exactly** the declared
entries, in declared order — reordering, relabeling, hiding
(`ColumnOverride::hidden`), or subsetting the derived set. A hidden column is
still emitted (with `"v-hidden": true`) — a renderer keeps the field in its
row model without displaying it. A declared `field` the row type does not
have is emitted as a bare `{field, label}` column with no `x-decimalPlaces` /
`ExtUnits` and no crash (schema generation never throws, matching
[forms.md](forms.md)'s failure-mode discipline).

## The JSON key vocabulary

| Key | Where | JSON type | Meaning |
|---|---|---|---|
| `v-kind` | top-level | string | `"collection"` or `"master-detail"`. The one required discriminator beyond `v-query`. |
| `v-title` | top-level | string | Screen title. Defaults to `v-query`'s type id. |
| `v-query` | top-level | string | Registered query action's type id; executed with an empty body. |
| `v-rowKey` | top-level | string | Wire field uniquely identifying a row. Defaults to `"id"`. |
| `v-columns` | top-level | array | Ordered column descriptors: `{field, label, "v-hidden"?, "x-decimalPlaces"?, ExtUnits?}`. Derived from the row type unless `V::columns` overrides it. |
| `v-rowAction` | top-level | object | `{action, bind?}` — the action a row activation opens. Omitted when `V` declares no `rowAction`. Carries only `action`/`bind`, never `label`/`scope`/`confirm`. |
| `v-actions` | top-level | array | `{action, label, scope, bind?, confirm?}` per entry — buttons that run an action. `scope` is `"row"` or `"collection"`; `confirm` is omitted when `false`; `bind` is omitted when empty. Omitted entirely when `V` declares no `actions`. |

A concrete `viewSchemaJson<SamplesView>()` output, for the `SamplesView`
declared above (row type carrying `id`/`name`):

```json
{
  "v-kind": "collection",
  "v-title": "Samples",
  "v-query": "ListSamples",
  "v-rowKey": "id",
  "v-columns": [
    { "field": "id", "label": "Id" },
    { "field": "name", "label": "Name" }
  ],
  "v-rowAction": {
    "action": "EditSample",
    "bind": { "id": "id" }
  },
  "v-actions": [
    { "action": "DeleteSample", "label": "Delete", "scope": "row", "bind": { "id": "id" }, "confirm": true },
    { "action": "CreateSample", "label": "New", "scope": "collection" }
  ]
}
```

`bind`, wherever it appears (`v-rowAction.bind` or a `v-actions` entry's
`bind`), is a plain JSON **object** mapping each target-action wire field to
a row wire field — `{"id": "id"}`, not an array of `{actionField, rowField}`
entries. This is `ActionDescriptor::bind`'s `std::span<const BindEntry>`
serialised key-by-key (`buildActionNode`), and it is what a renderer must
iterate with a `for...in` over its own keys, not by numeric index.

All keys are **additive**: a renderer that does not recognise `v-*` never
builds a screen from this document, but every action it references still
renders as an ordinary standalone form from its own `schemaJson<A>()` — the
view document changes nothing about that action's own schema (verified: no
`v-*` key is ever present in an action schema).

## `ViewTraits<V>` / `BRIDGE_REGISTER_VIEW` / `ViewRegistry`

`BRIDGE_REGISTER_VIEW(V, NAME)` specialises `ViewTraits<V>` (a `typeId()`
accessor, parallel to `ActionTraits<A>`) and registers `V`'s
`viewSchemaJson<V>()` provider with the process-level `ViewRegistry` at
static-init time — the same static-initialiser pattern
`BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION` use
([registry.md](../core/registry.md)), including its token-pasting constraint
(`V` must be an unqualified name in scope at the call site). Unlike
`BRIDGE_REGISTER_ACTION`, this macro needs only `<morph/core/registry.hpp>` —
a view registers **no executor**, only a schema provider, so there is no
`<morph/core/bridge.hpp>` dependency and no `registry.hpp` → `bridge.hpp`
include-cycle concern. `ViewRegistry::instance().schemaJson(viewId)` looks up
a registered view's document by string id (throws `std::runtime_error` for an
unknown id, mirroring `ActionExecuteRegistry::execute`); `viewIds()` lists
every registered id, so a controller can enumerate views the way it already
enumerates action schemas.

## Dispatch: the screen is still just action calls

A screen performs three ordinary dispatches, each already fully specified
elsewhere:

1. **Populate** — execute `v-query` with an empty body via the normal
   execute path ([bridge.md](../core/bridge.md)); read rows from the result
   exactly as `Choice` does.
2. **Edit** — activating a row builds the `v-rowAction` action's form
   ([forms.md](forms.md)) with `bind`-listed fields prefilled, then fires it
   through the same `executeJson` path as any form.
3. **Delete/other actions** — a `v-actions` entry fires its bound action,
   guarded by `confirm` when set.

After an edit/delete/create resolves, the renderer re-runs step 1 to refresh
— there is no push channel for collection changes (see Non-goals).

## The Qt/QML reference renderer

`src/qt/forms/qml/CollectionView.qml` is the reference implementation,
shipped as part of the `MorphForms` QML module (alongside `DynamicForm.qml`,
which it reuses **unmodified** as the row editor — see
[forms.md](forms.md), "Shipped Qt/QML reference renderer", for why the
renderer lives there and not in `examples/forms/gui_qml`, the demo
*consumer*). It parses `FormsController::viewsJson` (a `{viewType:
viewSchema}` object, mirroring `schemasJson`), renders `v-columns` as a
table, and instantiates `DynamicForm` as a modal `Dialog` for `v-kind:
"collection"` or a live side pane for `"master-detail"`. Populate/edit/
delete/create are issued purely through the existing
`controller.submitIfValid(actionType, bodyJson)` / `replyReceived` surface —
no new controller method was needed.

Row-open prefill locates the actual `field_<name>` `TextField` instance
DynamicForm.qml draws for a plain scalar field and sets its `text` — the same
path a user's own typing takes (via that control's own `onTextChanged`),
rather than reaching into `DynamicForm`'s internal `fieldValues` state
directly, since `DynamicForm.qml` has no external "set a field's displayed
value" API (every other control already owns writing its own text/selection
from the user's input, and none of the [Tier-1](forms.md#renderer-conformance-kit) features preceding this one
ever needed to prefill a field programmatically). See "Limitations" for what
this does not reach.

`examples/forms/lab_schemas.hpp`'s `SamplesView` (composed from
`ListSamples`/`EditSample`/`DeleteSample`/`CreateSample` in
`examples/forms/lab_model.hpp`) is the worked example.
`examples/forms/gui_qml/qml/Main.qml` renders every registered view alongside
the per-action forms, excluding from the standalone-forms list any action a
view already owns (its query, row-opener, or a `v-actions` target), so
nothing renders twice.

## API reference

### `morph::views::viewSchemaJson<V>()`

| Signature | Returns |
|---|---|
| `template <typename V> std::string viewSchemaJson()` | The view-schema JSON. Cached per type. Never throws — internal DOM (parsed JSON document tree) failure yields an empty string, matching `schemaJson<A>()`. |

### `ActionDescriptor` / `describeAction<Action>()` / `ColumnOverride`

| Symbol | Kind | Notes |
|---|---|---|
| `ActionScope` | enum class | `{Row, Collection}`. |
| `BindEntry{actionField, rowField}` | struct | One prefill mapping. |
| `ActionDescriptor{actionTypeId, label, scope, bind, confirm}` | struct | Always built via `describeAction`. |
| `describeAction<Action>(label, scope, bind, confirm)` | `consteval` function template | Resolves `actionTypeId` from `ActionTraits<Action>::typeId()`. |
| `ColumnOverride{field, label, hidden}` | struct | One `V::columns` entry. |
| `CollectionView` / `MasterDetailView` | empty tag structs | `V::kind`. |

### `ViewTraits<V>` / `BRIDGE_REGISTER_VIEW` / `ViewRegistry`

| Symbol | Kind | Notes |
|---|---|---|
| `ViewTraits<V>` | class template | **Customisation point.** `static constexpr std::string_view typeId()`. Specialise via `BRIDGE_REGISTER_VIEW`. |
| `BRIDGE_REGISTER_VIEW(V, NAME)` | macro | Specialises `ViewTraits<V>` and registers `V` with `ViewRegistry` at static-init time. |
| `ViewRegistry::registerView<V>(viewId)` | method template | Registers `V`'s schema provider (last-write-wins on a repeated `viewId`). |
| `ViewRegistry::schemaJson(viewId) const` | method | Returns the cached `viewSchemaJson<V>()` for `viewId`; throws `std::runtime_error` if unknown. |
| `ViewRegistry::viewIds() const` | method | Every registered view id. |
| `ViewRegistry::instance()` | static method | Process-level singleton. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Action references | **`ActionDescriptor` built by `describeAction<Action>()`**, not a bare `ActionList<...>` type-list | A homogeneous struct built by a `consteval` factory keeps `V::actions` a plain `std::array` (needed for iteration) while still resolving `actionTypeId` from the real, registered `ActionTraits<Action>`, compile-time-checked rather than a hand-typed string; it also carries per-action `label`/`scope`/`bind`/`confirm`, which a bare type-list cannot. |
| Column source of truth | **Reuse `morph::forms::schemaJson<Row>()`**, not a second reflection pass | One source for `x-order`/`x-decimalPlaces`/`ExtUnits` — a column formats a value exactly as that field's own form does, by construction, never by a second, potentially drifting implementation. |
| View registration | **Own `ViewRegistry`, no `bridge.hpp` dependency** | A view has no dispatch path (no executor to register), unlike an action — so, unlike `BRIDGE_REGISTER_ACTION`, `BRIDGE_REGISTER_VIEW` needs only `registry.hpp`'s `ActionTraits`. |
| Master-detail vs. collection | **Same document, `v-kind` only** | No new keys — a rendering choice, not a schema difference. |
| Hidden columns | **Still emitted, `v-hidden: true`** | A renderer keeps the field in its row model (e.g. for a later feature) without displaying it — "hidden" is presentational, not an omission. |
| `bind`'s wire shape | **A JSON object** (`{actionField: rowField}`), not an array of entries | Matches `ActionDescriptor::bind`'s natural key → value mapping and keeps lookups by field name O(1) for a renderer; `buildActionNode` serialises `std::span<const BindEntry>` this way. |

## Failure modes

Schema generation never throws: `viewSchemaJson<V>()` yields the same
best-effort/empty-string fallback discipline `schemaJson<A>()` documents (see
[forms.md](forms.md), "Total schema failure yields an empty string"). A
declared `ColumnOverride::field` or `BindEntry` naming a wire key the row/
target-action type does not have is silently accepted (a bare column, or a
`bind` entry the target action ignores on decode) rather than rejected.
`ViewRegistry::schemaJson`/`ActionExecuteRegistry::execute`-style lookups
throw `std::runtime_error` only for a wholly unregistered view/action id, not
for a malformed reference within an otherwise-valid one.

## Limitations

Every limitation `choice.md` documents for its "a query action serves rows"
pattern applies unchanged here, since a view's populate step is exactly that
pattern:

- **Membership/staleness not enforced.** A row's bound field values may be
  stale by the time a prefilled action fires; the handler must re-check.
- **Unchecked wire vocabulary.** `bind`'s field names are wire keys, resolved
  at runtime by the renderer, not checked against the target action's schema
  at declaration time (though `describeAction<Action>()` **does**
  compile-time-check that `Action` itself is a real, registered action — a
  stronger guarantee than `Choice`'s `OptionsAction` NTTP has).
- **The query action's result type must be default-constructible** when it is
  not itself a `std::vector<...>` — `viewSchemaJson<V>()` builds a probe
  instance purely to find the row-bearing member via reflection, the same
  constraint [forms.md](forms.md) already places on every action type for
  `mergeSchemaExtras`.
- **No i18n.** Like every other cached-per-type schema in this layer, labels
  (`title`, column labels, action labels) are fixed at first call. The Qt/QML
  reference renderer's `CollectionView.qml` does not (yet) accept an
  `I18nCatalog`/`displayLocale` the way `DynamicForm.qml` does.
- **Binding every required field of a row-opener, on a no-submit-button
  renderer, fires the action the instant the row opens.** A view schema
  itself has no notion of "submit" — a row-opener form fires exactly when
  the renderer decides it is ready, and the Qt/QML reference renderer
  (`DynamicForm.qml`) decides that the moment every required field is
  engaged, with no submit button to hold it back. If `bind` prefills *every*
  required field (not just the row key), the freshly-opened editor is
  already "ready" and auto-fires before the user reviews or changes
  anything. `examples/forms/lab_schemas.hpp`'s `SamplesView` avoids this by
  binding only `id` — `name` starts blank, so the edit fires only once the
  user actually types a new name. A renderer with an explicit submit step
  would not have this hazard; a no-submit-button renderer's authors must bind
  deliberately.
- **`CollectionView.qml`'s row-open prefill only reaches a plain scalar
  field.** It locates the bound field by DynamicForm's `objectName: "field_"
  + name` convention (the default `TextField` a plain integer/string
  property renders as) and sets its `text`. A `bind` entry targeting a field
  DynamicForm renders as a `Choice` combo box, a `Timestamp`/date-time
  picker, a slider, a multiline text area, a radio group, or a
  `SlotRegistry`-overridden control is not currently prefilled — the control
  is simply left at its own default, exactly as an unbound field would be
  (no crash, no partial state). Every worked example (`SamplesView`) only
  ever binds the integer row key, which always renders as a plain
  `TextField`, so this gap does not affect the shipped demo.

## Non-goals

- **Not an app framework.** A view is one screen composed of existing
  action-forms. Multi-screen navigation, menus, and cross-screen flow are a
  separate concern, implemented in
  [workflows_navigation.md](workflows_navigation.md) — a view is not itself
  one of that layer's screen kinds yet (`ViewScreen`/`kind: "view"` is
  reserved but not declared there).
- **No new dispatch path or wire change.** Populate/edit/delete/create are
  ordinary action executes over the existing path
  ([bridge.md](../core/bridge.md)); the view document is metadata a renderer
  consumes, never a payload.
- **No server-side "query language."** `v-query` names a registered action
  that returns whatever rows it returns; paging/filtering, if wanted, is a
  field on that action, not a view-schema concept.
- **No live/push list updates.** A list refreshes by re-running its query
  after a mutating action resolves — no subscription channel.
- **No nested/joined views.** A view references flat action row shapes,
  matching the flat-actions-only scope of schema generation
  ([forms.md](forms.md)).

## Cross-references

- [forms.md](forms.md) — `schemaJson<A>()`, the `x-order`/`ExtUnits`/
  `x-decimalPlaces` reused verbatim for derived columns, the shipped Qt/QML
  renderer toolkit `CollectionView.qml` ships alongside, and the
  flat-actions-only scope inherited here.
- [choice.md](choice.md) — the "a query action serves rows" pattern
  generalised from a combo box to a table, including the empty-body query
  contract and the stale-value caveat.
- [../core/bridge.md](../core/bridge.md) — the execute / `executeJson` path
  populate/edit/delete/create dispatch uses unchanged.
- [../core/registry.md](../core/registry.md) — `ActionTraits::typeId()` (the
  ids `describeAction` resolves), and the `BRIDGE_REGISTER_ACTION` pattern
  `BRIDGE_REGISTER_VIEW` mirrors.
- [workflows_navigation.md](workflows_navigation.md) — the wizard/app-shell
  layer this view's Non-goals defers menus and cross-screen flow to; that
  layer's `app-*` document reserves but does not yet declare a `kind: "view"`
  screen referencing a view like this one.
