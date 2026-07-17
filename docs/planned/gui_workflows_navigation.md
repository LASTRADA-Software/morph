# GUI workflows & navigation — wizards and the app shell (planned)

> **Status: planned — not yet implemented.** This is the highest-level
> view-schema document. It adds two **NEW** top-level descriptors above the
> per-action form schema of [forms.md](../spec/forms.md): a **wizard** that
> sequences several actions into one flow with a draft spanning them, and an
> **app-shell / route** descriptor (menu → screens → actions/views) so a renderer
> builds navigation. It sits in Tier 2 of [gui_overview.md](gui_overview.md),
> extends the reactive draft of [bridge.md](../spec/bridge.md) to span a
> sequence, and composes the screens of
> [gui_collections_views.md](gui_collections_views.md). See [todo.md](../todo.md).

## The gap

Two ceilings remain once single screens exist:

- **No multi-step flow.** [bridge.md](../spec/bridge.md)'s reactive draft
  (`subscribe`/`set<>`/`reset`) spans **one action**: a draft is created lazily
  on the first `set<>` for its action type and fires when that action is ready.
  A real onboarding or checkout is several actions in sequence sharing
  accumulated state (a `customerId` chosen in step 1 must prefill step 3). There
  is no descriptor that says "these actions, in this order, share this draft."
- **No app.** A renderer today enumerates schemas and stacks every form on one
  scroll (`Main.qml`). There is no top-level structure — menu, screens, routes —
  so a collection of forms never becomes *an app* with navigation. "A form" is as
  far as the contract reaches.

Neither gap is a wire or dispatch problem; both are missing **descriptors** a
renderer consumes, exactly like the per-action schema.

## Goal

Two additive, independent descriptors:

1. A **wizard schema** — a `NEW` document naming an ordered list of actions and a
   **shared draft** that spans them, so a value entered in one step is available
   to later steps. It extends the per-action draft in
   [bridge.md](../spec/bridge.md) to a *sequence* draft, reusing the existing
   `set<>` / validator machinery per step.
2. An **app-shell / route schema** — a `NEW` lightweight top-level document
   (menu → screens → actions/views) a renderer turns into navigation, so the same
   action-forms and collection-views assemble into a navigable application.

Both obey [gui_overview.md](gui_overview.md): additive, unversioned, ignorable by
an older renderer (which loses navigation/sequencing, not correctness), and each
step/screen is still a Tier-1 action-form or a
[gui_collections_views.md](gui_collections_views.md) view.

## Design

### (a) Wizard: a draft that spans a sequence of actions

A wizard is described by a **NEW** document — proposed
`morph::flows::wizardSchemaJson<W>()` — emitted alongside the action and view
schemas, referencing action type-ids only. Its **NEW** key vocabulary (`w-*`):

```json
{
  "w-title": "Onboard customer",
  "w-steps": [
    { "action": "CreateCustomer", "title": "Details" },
    { "action": "ChoosePlan",     "title": "Plan",
      "prefill": { "customerId": "CreateCustomer.id" } },
    { "action": "ConfirmSignup",  "title": "Confirm",
      "prefill": { "customerId": "CreateCustomer.id",
                   "planId":     "ChoosePlan.planId" } }
  ]
}
```

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `w-title` | top-level | string | Human title for the whole flow. |
| `w-steps` | top-level | array | Ordered steps. Each step is one registered action rendered as its ordinary [forms.md](../spec/forms.md) form. |
| ↳ `action` | step | string | Type-id of the step's action. |
| ↳ `title` | step | string | Step label (breadcrumb / header). |
| ↳ `prefill` | step | object | Maps this action's field → a `"<PriorAction>.<field>"` path into an earlier step's *result or submitted draft*. This is how the shared draft threads a value forward. |

All `w-*` keys are **NEW proposed** keys in a **NEW proposed** document; none are
verified against existing code.

**The shared draft — extending [bridge.md](../spec/bridge.md).** Today each
action type owns one `SubscriberEntry` draft keyed by
`ActionTraits<A>::typeId()`, created on the first `set<>` and persisting across
fires. A wizard groups several such drafts into one **flow draft** that outlives
each step: proposed `morph::flows::FlowSession`, a thin owner over a set of
per-action drafts plus a small resolved-values map keyed by
`"<action>.<field>"`. The proposed API mirrors the handler's existing surface so
no new dispatch is invented:

```cpp
// namespace morph::flows — NEW. Built on BridgeHandler's per-action draft.
template <typename... Steps>
class FlowSession {
public:
    explicit FlowSession(morph::bridge::BridgeHandler<Model>& handler);

    template <auto FieldPtr> void set(/* value */);   // set<> on the current step
    void advance();   // fire current step, capture its result, move to next
    void back();      // return to the previous step; its draft is retained
    bool finished() const noexcept;
};
```

- **A step is an ordinary action fire.** `advance()` fires the current step
  through the same `execute<Action>` / reactive path as a standalone form
  ([bridge.md](../spec/bridge.md)); `ActionValidator<Action>::ready` gates it
  exactly as it gates a lone form. The wizard adds *sequencing*, not a new
  execution mode.
- **`prefill` threads state forward.** On entering a step, the renderer resolves
  each `prefill` path against the flow's captured values and issues the
  corresponding `set<>` on that step's draft before showing the form — the same
  prefill mechanic `bind` uses in
  [gui_collections_views.md](gui_collections_views.md), but the source is an
  earlier step rather than a table row.
- **Drafts persist across steps and `back()`.** A per-step draft persists exactly
  as [bridge.md](../spec/bridge.md) already guarantees (a draft "persists across
  fires ... destroyed only when the handler is destroyed or `reset<A>()`"). The
  flow session simply keeps every step's draft alive for the flow's lifetime, so
  navigating `back()` and forward preserves entered values without re-fetching.
- **No cross-action atomicity.** Each step commits independently as its own
  action; a wizard is a *UX sequence*, not a transaction. A flow that must be
  all-or-nothing composes a single final action from the collected values (or the
  transactional outbox in [outbox.md](outbox.md) at the model layer) — the wizard
  does not add distributed-commit semantics.

### (b) App shell: menu → screens → actions/views

The app shell is a **NEW** top-level document — proposed
`morph::app::appSchemaJson<App>()` — the *root* a renderer loads to build
navigation, replacing the "enumerate every schema onto one scroll" of
`Main.qml`. Its **NEW** key vocabulary (`app-*`):

```json
{
  "app-title": "Lab console",
  "app-menu": [
    { "label": "Samples",  "screen": "samples" },
    { "label": "Intake",   "screen": "intake" }
  ],
  "app-screens": {
    "samples": { "kind": "view",   "ref": "SamplesView" },
    "intake":  { "kind": "wizard", "ref": "IntakeWizard" },
    "quick":   { "kind": "form",   "ref": "RecordMeasurement" }
  }
}
```

| Key | Where | JSON type | Meaning / renderer obligation |
|---|---|---|---|
| `app-title` | top-level | string | Application title (window/header). |
| `app-menu` | top-level | array | Ordered navigation entries `{ label, screen }`. A renderer builds a menu/sidebar/tabs from these; the target `screen` keys into `app-screens`. |
| `app-screens` | top-level | object | Map of screen-id → screen descriptor. |
| ↳ `kind` | screen | string | `"form"` (one action, [forms.md](../spec/forms.md)), `"view"` (a collection/master-detail, [gui_collections_views.md](gui_collections_views.md)), or `"wizard"` (a flow, above). |
| ↳ `ref` | screen | string | The type-id of the referenced action / view / wizard. The renderer fetches that thing's own schema and renders it into the routed area. |

A screen is therefore **just a reference** to a Tier-1 form, a
[gui_collections_views.md](gui_collections_views.md) view, or a wizard — the
shell contributes only the menu and the routing, never new field-level rendering.
An `app-*`-ignorant renderer falls back to today's behavior: it can still load
each referenced action schema directly and render a form. The shell is declared
in C++ as a descriptor over registered ids and registered with a **NEW**
`BRIDGE_REGISTER_APP` macro (parallel to `BRIDGE_REGISTER_ACTION`,
[registry.md](../spec/registry.md)); like the view registration it is metadata
only — no dispatch path is added.

### One consistent layering

```
app schema      (app-*)   : menu -> screens                    [this doc]
 └─ screen = form | view | wizard
      view schema (v-*)    : query + edit + delete              [gui_collections_views.md]
      wizard schema (w-*)  : ordered actions + shared draft     [this doc]
      form schema (x-*)    : one action                         [forms.md]
```

Each layer references the layer below by type-id and adds one concern (routing;
collection composition; sequencing; field rendering). None mutates the layer
below, so every layer is independently ignorable — the additive-key contract of
[gui_overview.md](gui_overview.md) applied top to bottom.

## Non-goals

- **Not a general application framework.** No client-side state store, no routing
  guards, no data-binding engine beyond the `prefill`/`bind` value threading, no
  business logic. The shell routes to screens; the screens are existing
  action-forms and views. Anything richer is the app's own code consuming the
  schemas directly ([gui_overview.md](gui_overview.md)'s escape hatch).
- **No cross-action transaction.** A wizard sequences independent action commits;
  it is not a distributed transaction. All-or-nothing is a model-layer concern
  ([outbox.md](outbox.md)), not a wizard-schema one.
- **No new dispatch path or wire change.** Each step and each screen fires
  ordinary actions over the existing path ([bridge.md](../spec/bridge.md)); the
  wizard and app documents are metadata a renderer consumes.
- **No server-driven navigation.** The menu/screens are a static declared
  descriptor, not a server-pushed, permission-filtered navigation tree. Hiding a
  screen by role is the app's concern; the schema does not encode authorization
  ([security.md](../spec/security.md) governs the wire, not the menu).
- **No conditional branching in wizards (initially).** `w-steps` is a linear
  ordered list. Data-dependent branching ("skip step 3 if free plan") is a
  possible later extension, not part of this document; a linear flow with a
  prefilled shared draft is the committed scope.
- **Draft is client-side only.** The flow draft lives in the handler's
  `SubscriberState`, exactly as the per-action draft does
  ([bridge.md](../spec/bridge.md)); it is not persisted or resumable across a
  process restart.

## Testing (planned)

- `wizardSchemaJson<IntakeWizard>()` emits `w-title` and ordered `w-steps` with
  per-step `action`, `title`, and `prefill`; each referenced action still emits
  its ordinary [forms.md](../spec/forms.md) schema unchanged.
- A `FlowSession` fires step 1, captures its result, and `advance()`s to step 2
  with the `prefill` paths issued as `set<>` on step 2's draft; `back()` returns
  to step 1 with its entered values intact (draft persistence).
- Each step is gated by its own `ActionValidator::ready` exactly as a standalone
  form; a not-ready step does not `advance()`.
- `appSchemaJson<App>()` emits `app-title`, an ordered `app-menu`, and an
  `app-screens` map whose `ref`s resolve to a registered form / view / wizard id;
  a renderer builds a menu and routes each entry to the referenced screen.
- An `app-*`/`w-*`-ignorant renderer still renders each referenced action as a
  plain form (additive-key fallback / regression guard).
- Backend switch mid-flow: an in-flight step fire surfaces `BackendChangedError`
  on the step's `errSink` while the flow draft survives, matching
  [bridge.md](../spec/bridge.md)'s draft-survives-switch guarantee, so the step
  re-fires cleanly against the new backend.

## Cross-references

- [gui_overview.md](gui_overview.md) — Tier 2; the highest layer of the
  view/app schema stack and the additive-key contract each `*-` vocabulary obeys.
- [forms.md](../spec/forms.md) — the per-action form each wizard step and each
  `kind: "form"` screen renders.
- [gui_collections_views.md](gui_collections_views.md) — the `kind: "view"`
  screens the shell routes to; the `bind` prefill mechanic wizards reuse as
  `prefill`.
- [bridge.md](../spec/bridge.md) — the per-action reactive draft
  (`subscribe`/`set<>`/`reset`, draft persistence across fires and backend
  switches) the wizard's shared flow draft extends to span a sequence.
- [registry.md](../spec/registry.md) — `ActionTraits::typeId()` (the ids the
  wizard/app reference) and the `BRIDGE_REGISTER_ACTION` pattern the proposed
  `BRIDGE_REGISTER_APP` mirrors.
- [outbox.md](outbox.md) — where cross-action atomicity belongs (the wizard
  deliberately does not provide it).
- [gui_renderer_toolkit.md](gui_renderer_toolkit.md) — the reference renderer and
  conformance kit that would implement and verify the `w-*` / `app-*` contract.
