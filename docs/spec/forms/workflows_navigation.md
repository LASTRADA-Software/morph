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
- [The Qt/QML reference renderer](#the-qtqml-reference-renderer)
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

The typed C++ sequencer, built entirely on `BridgeHandler`'s existing
reactive draft ([bridge.md](../core/bridge.md)):

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
  flight; nothing does now — not here, and not on the standalone-form path,
  where `DynamicForm` calls `submitIfValid` on every change that leaves the
  form ready with no in-flight suppression either. A caller that wants one
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
- **A late step callback cannot touch a destroyed session.** Every step's
  `.then` / `.onError` is attached through a `morph::async::CallbackScope`
  member ([callback_scope.md](../core/callback_scope.md)) declared last, so a
  completion resolving after the session is gone is refused rather than
  dereferencing freed memory. `~FlowSession` calls `requestStop()` as its first
  statement rather than relying on member destruction alone: members are
  destroyed only *after* the destructor body, and that body can pump an event
  loop (a blocking, `sendSync`-style call) and deliver into a half-dead
  session. This is the "teardown that pumps" escape hatch callback_scope.md
  documents, and `FlowSession` is its worked example.

## The Qt/QML reference renderer

`src/qt/forms/qml/WizardView.qml` is the reference implementation of a
stepper, shipped as part of the `MorphForms` QML module (CMake target
`morph_forms_module`) alongside `DynamicForm.qml` (reused **unmodified** per
step) and `CollectionView.qml`. It lives in `src/qt/forms`, not
`examples/forms/gui_qml`, for the same reason [views.md](views.md) gives for
`CollectionView.qml`: it is fully generic over its `wizardSchema`/`schemas`/
`controller` properties — nothing in it names `lab::` anything — so it ships
as a reusable component rather than example code. `WizardView` renders one
`w-steps` entry per `DynamicForm` inside a `Repeater` (not a `Loader`), kept
alive for the wizard's lifetime in a `StackLayout` so a step's entered values
survive Back/Next navigation; `applyPrefill(index)` reads
`controller.resolvedValue(path)` for each of a step's declared `prefill`
entries and calls that `DynamicForm`'s `setFieldValue(field, value)`.

`examples/forms/gui_qml/qml/AppShell.qml` is the demo-specific consumer —
like `Main.qml`, it instantiates the app's own `FormsController` QML type by
name (Qt cannot register a class template for QML, so each app writes its
own controller subclass; see [forms.md](forms.md), "Shipped Qt/QML reference
renderer"). It renders `app-menu` as a sidebar `ListView` and a `Loader`
whose `sourceComponent` is chosen from the selected screen's `kind`
(`DynamicForm` for `"form"`, `WizardView` for `"wizard"`, an explicit
placeholder `Label` for anything else, including the reserved but
unimplemented `"view"`). `AppShell.qml` is registered as the demo's new
default entry point (`examples/forms/gui_qml/main.cpp` now loads
`"AppShell"` instead of `"Main"`); `Main.qml` is untouched and still
independently buildable/loadable.

`FormsController` (the demo's `QObject`/`QML_ELEMENT` wrapper around
`morph::qt::forms::FormsControllerCore<lab::LabModel>`) exposes
`wizardSchemasJson`/`appSchemaJson` the same way E-G7 added `viewsJson` —
a `Q_PROPERTY` on the demo-specific subclass calling a `lab::` free function,
not a change to the model-agnostic `FormsControllerCore<Model>`. Resolved-value
tracking (`Q_INVOKABLE QString resolvedValue(path) const`) is populated by a
free function in `FormsController.cpp` that flattens the top-level keys of
both the submitted body and the reply into a `"<ActionType>.<field>"` map on
every successful `submitIfValid` — result keys are written after (and so win
over) draft keys on a name collision, mirroring `FlowSession::captureResult`'s
precedence exactly, just implemented as plain JSON manipulation instead of
the typed template API (see [Design decisions](#design-decisions)).

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
| QML component placement | `WizardView.qml` ships in the shared `MorphForms` module (`src/qt/forms/qml`); `AppShell.qml` stays in the demo (`examples/forms/gui_qml/qml`) | `WizardView` is fully generic (duck-typed `controller`, schema-driven), exactly like `CollectionView.qml` — it ships alongside it. `AppShell.qml` instantiates the demo's own concrete `FormsController` QML type by name, exactly like `Main.qml` already does, so it stays with the demo that defines that type. |
| No runtime wizard/app registry | `BRIDGE_REGISTER_WIZARD`/`BRIDGE_REGISTER_APP` only specialise traits | Neither a wizard nor an app is ever executed or looked up by string id at runtime; the consuming code always names the concrete type. Avoids a second registry to keep in lockstep with the schema-emission call sites, mirroring how the example's own `schemasJson()` already hand-assembles its schema set rather than walking a generic registry. |
| Prefill resolution lives with the caller/renderer | `FlowSession::resolved(path)` is read-only; nothing calls `set<>` automatically | Matches the wizard document's own wording that the renderer resolves and issues the `set<>` calls; keeps `FlowSession` a pure sequencer with no opinion on *when* a step should be pre-populated. |
| Result fields win over draft fields on name collision | `FlowSession::captureResult` records the draft first, then the result, so identical field names are overwritten by the result; `FormsController`'s QML-facing resolved-value map applies the same order | A deterministic, testable rule for the one genuinely ambiguous point in the source design (prefill can come from "an earlier step's result *or* submitted draft"), applied identically on both the typed C++ path and the QML reference renderer's JSON path. |
| Steps must be pairwise distinct types | `static_assert(detail::AllDistinct<Steps...>::value, ...)` | `BridgeHandler` keeps exactly one draft per `(handler, action type)`; reusing a type twice in one flow would silently collide. Also required for `std::get<A>(_drafts)` (`std::tuple::get<T>` needs a unique `T`). |
| QML reference renderer does not use `FlowSession` | `WizardView.qml`/`AppShell.qml`/`FormsController` sequence wizards via plain `executeJson` + a JSON resolved-value map, not the typed template API | QML/MOC cannot name a C++ action type generically at compile time, and `FormsController.hpp` must stay free of template-heavy morph headers (its existing `Q_MOC_RUN` guard). `FlowSession` remains available for non-QML/typed embeddings and is exercised directly by `tests/test_flows_apps.cpp`. |

## Limitations

- **`kind: "view"` is not implemented.** The `app-*` vocabulary reserves the
  value but no `ViewScreen` type exists yet — it would be added once a need
  arises to route an app-shell menu entry straight at a
  [views.md](views.md) `morph::views` view, requiring no change to
  `appSchemaJson` itself. The reference demo's `AppShell.qml` renders an
  explicit placeholder for any screen `kind` it does not recognise, rather
  than silently doing nothing. `examples/forms/lab_schemas.hpp`'s
  `SamplesView` (a `morph::views::CollectionView`, rendered standalone by
  `Main.qml`) is therefore not one of `LabApp`'s menu entries — integrating
  it would require `ViewScreen`, which this plan deliberately does not add.
- **No cross-screen state store in the reference renderer.** `AppShell.qml`
  destroys and recreates a wizard's `WizardView` instance when the menu
  routes away and back (a `Loader.sourceComponent` change), resetting its
  step to 0. This is consistent with the source spec's Non-goals ("Not a
  general application framework. No client-side state store...").
- **Prefill does not resync a widget's displayed value.** `WizardView`'s
  `applyPrefill` updates the target step's `fieldValues` (and therefore the
  assembled submission) via `DynamicForm.setFieldValue`, but does not push
  the value back into the visible `TextField`/`ComboBox`, since
  `DynamicForm.qml`'s fields are write-only from the widget's side (a
  pre-existing characteristic, not introduced here). A user who edits the
  prefilled field after arriving overwrites it normally; only the *visual*
  echo of the prefilled value is missing.
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
- The QML reference renderer's stepper (`WizardView.qml`) is covered by
  `src/qt/forms/tests/tst_wizardview.qml` (run as part of the
  `forms_qml_logic` ctest, alongside `tst_collectionview.qml` and the rest
  of the `MorphForms` module's own test corpus): Next stays disabled until
  the step replies ok, Next/Back move the step index, and prefill lands in
  the next step's `fieldValues`.

## Cross-references

- [forms.md](forms.md) — the per-action schema each wizard step and each
  `kind: "form"` screen renders; `FixedString`, the schema key vocabulary,
  and the Qt/QML reference renderer's package layout (`src/qt/forms` vs.
  `examples/forms/gui_qml`) this spec's own renderer section follows.
- [choice.md](choice.md) — `FixedString`'s other consumer; the "declare a
  compile-time name, validate at runtime" pattern `Bind`/`MenuEntry`/screen
  `Id`s reuse.
- [views.md](views.md) — the `v-*` view-schema layer; the `kind: "view"`
  screen this document reserves but does not yet implement, and the
  precedent (`CollectionView.qml` shipping in `src/qt/forms`) this spec's
  `WizardView.qml` placement follows.
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
