# `morph::flows` and `morph::app` — wizards and the app shell

`morph::flows` sequences an ordered list of already-registered actions into
one multi-step flow sharing captured values across steps (`Wizard`,
`WizardStep`, `Bind`, `wizardSchemaJson<W>()`, `FlowSession<Model, Steps...>`).
`morph::app` composes registered action-forms and wizards into a navigable
menu (`App`, `MenuEntry`, `FormScreen`, `WizardScreen`, `appSchemaJson<AppT>()`).
Both are additive metadata and client-side sequencing over the existing
dispatch path in [bridge.md](../core/bridge.md) — no new wire format, no new
execution mode.

## Contents

- [The gap this closes](#the-gap-this-closes)
- [The `w-*` wizard document](#the-w--wizard-document)
- [The `app-*` app-shell document](#the-app--app-shell-document)
- [C++ descriptors](#c-descriptors)
- [`FlowSession<Model, Steps...>`](#flowsessionmodel-steps)
- [Rendering](#rendering)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Testing](#testing)
- [Cross-references](#cross-references)

## The gap this closes

A single form ([forms.md](forms.md)) gates on its own fields only — nothing
sequences one action's result into the next action's prefill, or groups a set
of already-registered actions into one navigable menu. Before this layer, a
multi-step flow ("register a sample, then record its first measurement,
carrying the new sample's id forward as a prefill") had to be hand-wired: a
bespoke controller tracking a step index, manually reading one action's reply
to seed the next request body, with no shared, introspectable vocabulary a
renderer could build a generic stepper from. `morph::flows` and `morph::app`
name that sequencing and menu structure declaratively — the same move
[views.md](views.md) makes for "a query action plus its row actions" instead
of a hand-wired list screen.

## The `w-*` wizard document

`wizardSchemaJson<W>()` emits a small JSON document alongside each step's
ordinary action schema ([forms.md](forms.md)):

```json
{
  "w-title": "Register & measure a sample",
  "w-steps": [
    { "action": "RegisterSample", "title": "New sample" },
    { "action": "RecordMeasurement", "title": "First measurement",
      "prefill": { "sampleId": "RegisterSample.id" } }
  ]
}
```

| Key | Where | JSON type | Meaning |
|---|---|---|---|
| `w-title` | top-level | string | Human title for the whole flow. |
| `w-steps` | top-level | array | Ordered steps. |
| ↳ `action` | step | string | The step's registered action type-id (`ActionTraits<A>::typeId()`). |
| ↳ `title` | step | string | Step label. |
| ↳ `prefill` | step | object | Present only when the step declares at least one `Bind`. Maps the step's field name to a `"<PriorAction>.<field>"` path into an earlier step's captured values. |

Each step still renders as an ordinary [forms.md](forms.md) action form; the
wizard document only adds sequencing. A renderer ignorant of `w-*` loses
nothing but the stepper — every step is independently a valid, standalone
action form.

## The `app-*` app-shell document

`appSchemaJson<AppT>()` emits the navigation root a renderer loads instead of
enumerating every action schema onto one scroll:

```json
{
  "app-title": "Lab console",
  "app-menu": [
    { "label": "Density", "screen": "density" },
    { "label": "Intake",  "screen": "intake" }
  ],
  "app-screens": {
    "density": { "kind": "form",   "ref": "ComputeDryDensity" },
    "intake":  { "kind": "wizard", "ref": "IntakeWizard" }
  }
}
```

| Key | Where | JSON type | Meaning |
|---|---|---|---|
| `app-title` | top-level | string | Application title. |
| `app-menu` | top-level | array | Ordered `{label, screen}` entries; `screen` keys into `app-screens`. |
| `app-screens` | top-level | object | Map of screen-id → `{kind, ref}`. |
| ↳ `kind` | screen | string | `"form"` or `"wizard"` in the current implementation (`"view"` is a reserved, not-yet-implemented value — see [Limitations](#limitations)). |
| ↳ `ref` | screen | string | The referenced action's or wizard's registered type-id. |

A screen is only a reference — the shell contributes menu and routing, never
field-level rendering. An `app-*`-ignorant renderer can still load each
referenced action schema directly and render a plain form.

## C++ descriptors

Declared in `include/morph/forms/flows.hpp` (namespace `morph::flows`) and
`include/morph/forms/app.hpp` (namespace `morph::app`), alongside
`forms.hpp`/`choice.hpp`/`views.hpp` in the same directory — mirroring how
`include/morph/util/` already hosts three distinct namespaces
(`morph::time`, `morph::math`, `morph::units`) under one directory, and how
`views.hpp` (E-G7) landed the same way for the same reasons.

Both descriptor sets are built entirely from
[`FixedString`](forms.md#fixedstring--nttp-compile-time-string) NTTPs (the
same compile-time-string vehicle `Choice` uses), so a step's action, a bind's
field/path, a screen's id, and a menu's label are all part of the type itself
— compile-time-checked shape, though (like `Choice`'s `OptionsAction`) the
*names* they carry are still resolved at runtime.

```cpp
// namespace morph::flows
template <morph::forms::FixedString Field, morph::forms::FixedString Path>
struct Bind { /* field(), path() */ };

template <typename Action, morph::forms::FixedString Title, typename... Binds>
struct WizardStep { using action = Action; using binds = std::tuple<Binds...>; /* title() */ };

template <morph::forms::FixedString Title, typename... Steps>
struct Wizard { using steps = std::tuple<Steps...>; /* title() */ };

template <typename W> struct WizardTraits;  // specialise via BRIDGE_REGISTER_WIZARD

template <typename W> std::string wizardSchemaJson();
```

```cpp
// namespace morph::app
template <morph::forms::FixedString Label, morph::forms::FixedString ScreenId>
struct MenuEntry { /* label(), screen() */ };

template <morph::forms::FixedString Id, typename Action>
struct FormScreen { /* id(), kind()=="form", ref()==ActionTraits<Action>::typeId() */ };

template <morph::forms::FixedString Id, typename Wizard>
struct WizardScreen { /* id(), kind()=="wizard", ref()==WizardTraits<Wizard>::typeId() */ };

template <morph::forms::FixedString Title, typename Menu, typename Screens>
struct App { using menu = Menu; using screens = Screens; /* title() */ };

template <typename A> struct AppTraits;  // specialise via BRIDGE_REGISTER_APP

template <typename AppT> std::string appSchemaJson();
```

A wizard/app descriptor is registered with `BRIDGE_REGISTER_WIZARD(W, NAME)` /
`BRIDGE_REGISTER_APP(A, NAME)`, each specialising `WizardTraits<W>` /
`AppTraits<A>` with a `typeId()`. Unlike `BRIDGE_REGISTER_ACTION`
([registry.md](../core/registry.md)), neither macro performs static-init
registration into any dispatch or enumeration registry — a wizard or app is
never itself executed or looked up by string id at runtime in this
implementation; only the actions/wizards its steps/screens reference are
(via the already-registered `ActionTraits`/`WizardTraits`). A consumer that
needs the schema for a specific wizard/app calls `wizardSchemaJson<W>()` /
`appSchemaJson<AppT>()` directly, naming the type — exactly how
`examples/forms/lab_schemas.hpp`'s `schemasJson()` already hand-assembles a
fixed `{actionType: schema}` map today, rather than through a generic
type-erased registry walk.

`ViewScreen<Id, View>` (a `kind: "view"` screen referencing a
[views.md](views.md) view) is deliberately not yet declared: `appSchemaJson`
only requires each screen type to expose `id()`/`kind()`/`ref()`, so adding
it later needs no change to `appSchemaJson` itself.

## `FlowSession<Model, Steps...>`

The typed C++ sequencer ([bridge.md](../core/bridge.md)):

```cpp
template <typename Model, typename... Steps>
class FlowSession {
public:
    explicit FlowSession(morph::bridge::BridgeHandler<Model>& handler,
                          std::function<void(std::exception_ptr)> onError = nullptr);

    template <auto FieldPtr> void set(/* ValueType */ value);
    bool advance();
    bool back();
    bool finished() const noexcept;
    bool ready() const noexcept;
    std::size_t currentIndex() const noexcept;
    static constexpr std::size_t stepCount() noexcept;
    std::string_view currentActionType() const noexcept;
    std::optional<std::string> resolved(std::string_view path) const;
};
```

- **A step is an ordinary action fire.** `set<FieldPtr>` writes the field into
  `FlowSession`'s own per-step draft, checks `ActionValidator<A>::ready` on it,
  and dispatches a ready draft with `BridgeHandler::execute<A>()`. The gate and
  the auto-fire-on-ready behaviour are the standalone form's, but the *draft*
  is the flow's: nothing is forwarded to handler-side draft machinery, and
  `FlowSession` still adds no new execution mode.
- **A flow step does not coalesce in flight.** Because the dispatch decision is
  made in `FlowSession::set<>`, **every** `set<>` that leaves the draft ready
  fires, including one made while an earlier fire is still outstanding.
  Keystroke-rate `set<>` calls on an already-complete draft therefore produce
  one request each, and their replies land in whatever order the backend
  answers — the last reply to arrive wins, which need not be the last call
  made. An earlier handler-side draft did collapse patches arriving during a
  flight; nothing does now, here or on the standalone-form path. A caller that wants one
  request per pause throttles or debounces on its own side; `FlowSession`
  deliberately does not, since it cannot know the host's input cadence.
- **`advance()`/`back()` are pure sequencing.** `advance()` moves to the next
  step only if the current step has already produced a captured, successful
  result (`ready() == true`); a not-ready step does not advance. `back()`
  returns to the previous step; `FlowSession`'s own per-step
  `std::tuple<Steps...>` draft — the only draft in play, since the handler
  keeps none — is never reset, so entered values survive navigation.
- **`resolved(path)` is the prefill source.** On every successful step
  result, `FlowSession` flattens both the step's submitted draft (via
  `morph::forms::detail::forEachNamedMember`) and its result into a
  `"<ActionTypeId>.<field>"` → JSON-value map — result fields overwrite
  draft fields of the same name on collision. Applying a `w-steps[].prefill`
  binding (reading `resolved(path)` and calling the target step's `set<>`)
  is the **caller's** responsibility, matching the wizard document's own
  wording ("the renderer resolves each prefill path ... and issues the
  corresponding `set<>`") — `FlowSession` does not push prefill itself.
- **`Steps...` must be pairwise distinct.** Each step type occupies one slot
  of `FlowSession`'s own `std::get<A>(_drafts)` tuple lookup, which requires
  a unique type. Reusing the same action type as two steps of one wizard is
  not supported (`static_assert`-enforced).
- **Backend-switch behaviour is inherited, not reimplemented.** A step is
  dispatched with the ordinary `BridgeHandler::execute`, so an in-flight step
  cancelled by `Bridge::switchBackend` surfaces `BackendChangedError` on
  `FlowSession`'s `onError` callback exactly as bridge.md documents for any
  other caller. The draft survives the switch because `FlowSession` owns it.
- **One owner.** A `FlowSession` belongs to its handler's owner — the
  bridge's — and is built, used and destroyed there; its draft, position and
  captured values are touched only there, without a lock, and each member
  checks it in a debug build. The handler's `guiExec` delivers every step's
  continuation on the owner, so capture and destruction run on one thread.
- **A late step callback cannot touch a destroyed session.** Every step's
  `.then` / `.onError` is attached through a `morph::async::CallbackScope`
  member ([callback_scope.md](../core/callback_scope.md)) declared last, so a
  completion resolving after the session is gone is refused rather than
  dereferencing freed memory. `~FlowSession` calls `requestStop()` as its first
  statement rather than relying on member destruction alone: members are
  destroyed only *after* the destructor body, and a body that pumped an event
  loop could deliver into a half-dead session. This is the "teardown that pumps" escape hatch callback_scope.md
  documents, and `FlowSession` is its worked example.

## Rendering

The forms engine renders a wizard: `WizardModel::fromSchema` reads the `w-*`
document, `WizardSession` keeps one `FormSession` per step alive for the
wizard's lifetime — so a step's values survive Back and Next — resolves each
step's `prefill` from earlier steps' bodies and replies, and `wizardView` draws
the header, the current step's form and Back/Next with their gates
([engine.md](engine.md), "Wizards" and "Rendering collections and wizards").
Result fields win over draft fields on a name collision, the same order as
`FlowSession::captureResult`. A document's `wizard` node reaches it through the
interpreter; the app shell's menu and screens are the host's
(`docs/spec/ui/host.md`).

## API reference

### `morph::flows`

| Member | Signature | Notes |
|---|---|---|
| `Bind<Field, Path>` | struct | `field()`, `path()` — one prefill binding. |
| `WizardStep<Action, Title, Binds...>` | struct | `action`, `binds`, `title()`. |
| `Wizard<Title, Steps...>` | struct | `steps`, `title()`. |
| `WizardTraits<W>` | class template | **Customisation point.** `typeId()`. Specialise via `BRIDGE_REGISTER_WIZARD`. |
| `wizardSchemaJson<W>()` | function template | Returns the `w-*` document. Never throws; empty string only on total glaze JSON-writer failure. |
| `FlowSession<Model, Steps...>` | class template | See above. Non-copyable, non-movable. |

### `morph::app`

| Member | Signature | Notes |
|---|---|---|
| `MenuEntry<Label, ScreenId>` | struct | `label()`, `screen()`. |
| `FormScreen<Id, Action>` | struct | `id()`, `kind()=="form"`, `ref()==ActionTraits<Action>::typeId()`. |
| `WizardScreen<Id, Wizard>` | struct | `id()`, `kind()=="wizard"`, `ref()==WizardTraits<Wizard>::typeId()`. |
| `App<Title, Menu, Screens>` | struct | `menu`, `screens`, `title()`. |
| `AppTraits<A>` | class template | **Customisation point.** `typeId()`. Specialise via `BRIDGE_REGISTER_APP`. |
| `appSchemaJson<AppT>()` | function template | Returns the `app-*` document. Never throws; empty string only on total glaze JSON-writer failure. |

### Macros

| Macro | Arguments | Generates |
|---|---|---|
| `BRIDGE_REGISTER_WIZARD` | `(W, NAME)` | `WizardTraits<W>` specialisation. No static-init registry side effect. |
| `BRIDGE_REGISTER_APP` | `(A, NAME)` | `AppTraits<A>` specialisation. No static-init registry side effect. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Header location | `include/morph/forms/{flows,app}.hpp`, not a new top-level directory | Both modules are small, tightly coupled to `forms.hpp`'s `FixedString`/schema machinery, and `include/morph/util/` already establishes the precedent that one directory can host several distinct namespaces — `views.hpp` (E-G7) independently converged on the same answer. |
| No runtime wizard/app registry | `BRIDGE_REGISTER_WIZARD`/`BRIDGE_REGISTER_APP` only specialise traits | Neither a wizard nor an app is ever executed or looked up by string id at runtime; the consuming code always names the concrete type. Avoids a second registry to keep in lockstep with the schema-emission call sites, mirroring how the example's own `schemasJson()` already hand-assembles its schema set rather than walking a generic registry. |
| Prefill resolution lives with the caller/renderer | `FlowSession::resolved(path)` is read-only; nothing calls `set<>` automatically | Matches the wizard document's own wording that the renderer resolves and issues the `set<>` calls; keeps `FlowSession` a pure sequencer with no opinion on *when* a step should be pre-populated. |
| Result fields win over draft fields on name collision | `FlowSession::captureResult` records the draft first, then the result, so identical field names are overwritten by the result; `WizardSession`'s resolved values apply the same order | A deterministic, testable rule for the one genuinely ambiguous point in the source design (prefill can come from "an earlier step's result *or* submitted draft"), applied identically on the typed C++ path and the runtime wizard. |
| Steps must be pairwise distinct types | `static_assert(detail::AllDistinct<Steps...>::value, ...)` | `std::get<A>(_drafts)` needs a unique `T`, and reusing a step type would silently collide the two steps' drafts. |
| The rendered wizard does not use `FlowSession` | `WizardSession` sequences a served `w-*` document over JSON bodies, not the typed template API | A client renders a wizard it was served and cannot name its C++ action types. `FlowSession` remains available for typed embeddings and is exercised directly by `tests/test_flows_apps.cpp`. |

## Limitations

- **`kind: "view"` is not implemented.** The `app-*` vocabulary reserves the
  value but no `ViewScreen` type exists yet — it would be added once a need
  arises to route an app-shell menu entry straight at a
  [views.md](views.md) `morph::views` view, requiring no change to
  `appSchemaJson` itself. `examples/forms/lab_schemas.hpp`'s `SamplesView`
  (a `morph::views::CollectionView`) is therefore not one of `LabApp`'s menu
  entries — integrating it would require `ViewScreen`.
- **`Steps...` uniqueness is a hard requirement, not a documented escape
  hatch.** A wizard that needs the same action type twice (e.g. two
  identically-shaped approval steps) is not expressible with one
  `FlowSession`; it would need two distinct action types (even if
  structurally identical) to get two independent draft slots.
- **No conditional branching, no cross-action transaction, no server-driven
  navigation.** `w-steps` is a linear list; each step commits independently;
  the menu/screens are a static compiled-in descriptor. See the planning
  source's Non-goals for the full rationale (unchanged by this
  implementation).

## Testing

- `wizardSchemaJson<W>()` emits `w-title` and ordered `w-steps` with
  per-step `action`/`title`, and `prefill` only for steps declaring a
  `Bind` (`tests/test_flows_apps.cpp`, `Flows::WizardSchemaJson*`).
- `appSchemaJson<AppT>()` emits `app-title`, `app-menu`, and `app-screens`
  with `kind`/`ref` resolving through `ActionTraits`/`WizardTraits`
  (`tests/test_flows_apps.cpp`, `App::*`).
- `FlowSession` fires step one, advances, captures step two's prefill
  source, gates `advance()` on readiness, supports `back()` with draft
  persistence, throws on a `set<>` targeting the wrong step, and surfaces
  `BackendChangedError` on the current step's error callback when a backend
  switch lands mid-flight (`tests/test_flows_apps.cpp`, `FlowSession:*`).
- The rendered wizard is covered by `tests/test_forms_engine_wizard.cpp` and
  `tests/test_forms_engine_wizard_view.cpp`: Next stays disabled until the
  step replies ok, Next/Back move the step index, and prefill lands in the
  next step's draft.

## Cross-references

- [forms.md](forms.md) — the per-action schema each wizard step and each
  `kind: "form"` screen renders; `FixedString` and the schema key vocabulary.
- [engine.md](engine.md) — the wizard session and view that render this
  document.
- [choice.md](choice.md) — `FixedString`'s other consumer; the "declare a
  compile-time name, validate at runtime" pattern `Bind`/`MenuEntry`/screen
  `Id`s reuse.
- [views.md](views.md) — the `v-*` view-schema layer; the `kind: "view"`
  screen this document reserves but does not yet implement.
- [../core/bridge.md](../core/bridge.md) — `BridgeHandler::execute`,
  `ActionValidator::ready`, and `BackendChangedError` — the mechanism
  `FlowSession` extends to span a sequence without adding a new dispatch path.
- [../core/callback_scope.md](../core/callback_scope.md) —
  `morph::async::CallbackScope`, the gate `FlowSession` uses so a step
  completion resolving after the session is destroyed is refused.
- [../core/registry.md](../core/registry.md) — `ActionTraits::typeId()` and the
  `BRIDGE_REGISTER_ACTION` pattern `BRIDGE_REGISTER_WIZARD`/`BRIDGE_REGISTER_APP`
  mirror (metadata-only, no dispatch registration).
- [../journal/journal.md](../journal/journal.md) — the outbox/durability layer
  where cross-action atomicity belongs; a wizard deliberately does not
  provide it (each step still commits independently).
