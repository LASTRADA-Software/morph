# Reactive forms demo over BridgeHandler

Date: 2026-07-06

## Problem

`examples/forms/gui_qml/FormsController` does not use the framework's real
client API. It builds `morph::wire::Envelope`s by hand and feeds them
directly to an in-process `morph::backend::RemoteServer`, bypassing
`Bridge`/`BridgeHandler` entirely. That is internal server-side wire
protocol plumbing — no real client is expected to touch `wire::Envelope` or
`RemoteServer` directly. Every other GUI in this codebase (`examples/bank`)
talks to models exclusively through `BridgeHandler<Model>`.

Separately, the forms demo is not reactive: each form has an explicit
"execute" button; nothing recomputes until the user clicks it. The
framework already has a live, field-by-field, auto-fire API for exactly
this shape of UI — `BridgeHandler::set<&Action::field>(value)` /
`subscribe<Action>(cb)` — used today only in tests
(`tests/test_subscription.cpp`) and not demonstrated in any GUI example.

The obstacle to adopting `set<>` directly from QML: it is a compile-time
API (a non-type template parameter per field), while `DynamicForm.qml`
discovers field names generically at runtime from the action's JSON
schema. Bridging that gap must not require new C++ per action — adding an
action to `lab_model.hpp` today only requires the struct plus one
`BRIDGE_REGISTER_ACTION` line, and that must remain true.

## Goals

- `FormsController` talks to `LabModel` exclusively through
  `Bridge`/`BridgeHandler<LabModel>` — no `wire::Envelope`, no
  `RemoteServer`, no `ActionDispatcher`/`ModelRegistryFactory` singletons
  touched from the GUI layer.
- Forms fire automatically as soon as their required fields are valid
  (live recompute), using `BridgeHandler::set<>` / `subscribe<>` under the
  hood. No submit button in the reactive path.
- Zero new boilerplate per action: `lab_model.hpp` (and any future action
  file) needs no changes beyond what `BRIDGE_REGISTER_ACTION` already
  requires today.
- `examples/forms/main.cpp` (schema dump / `--emit-html` / REPL) is
  untouched — it has no GUI and does not use `FormsController`.

## Non-goals

- No change to the JSON Schema shape (`morph::forms::schemaJson`) or to
  `x-order`/`x-decimalPlaces`/`x-optionsAction` metadata.
- No change to `BridgeHandler`'s existing `execute()`/one-shot API — it
  stays as-is for `fetchOptions` (`ListSamples` is a plain query, not a
  fielded draft).
- No attempt to make the *options* combo-box fetch reactive; it remains
  fetched once per form (as today), triggered from `Component.onCompleted`.
- The static `--emit-html` page (pure JS, no C++ backend at all) is out of
  scope; it already works standalone and has its own test
  (`test_html_math.mjs`).

## Design

### 1. `FieldSetterRegistry` (new, in `include/morph/bridge.hpp`)

A process-wide singleton with the same shape as the existing
`ActionDispatcher` (`registry.hpp`): a static-init-populated map, keyed by
`(modelId, actionId, fieldName)`, of type-erased closures.

```cpp
class FieldSetterRegistry {
public:
    // Reads `jsonValue` into the field's real type and calls the
    // compile-time handler.set<&Action::field>(value) on it.
    using Setter = std::function<void(void* /* BridgeHandler<Model>* */, std::string_view jsonValue)>;

    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId);

    void set(std::string_view modelId, std::string_view actionId, std::string_view fieldName,
              void* handler, std::string_view jsonValue) const;  // throws if unknown

    static FieldSetterRegistry& instance();
};
```

`registerAction<Model, Action>` iterates `Action`'s reflected members with
`morph::forms::detail::forEachNamedMember` (already exists, already used
by `schemaJson`/`allRequiredEngaged` — no new reflection mechanism). For
each member it captures a closure:

```cpp
[](void* handlerVoid, std::string_view jsonValue) {
    auto* handler = static_cast<BridgeHandler<Model>*>(handlerVoid);
    using FieldType = /* member's real type */;
    FieldType value{};
    if (glz::read_json(value, jsonValue)) {
        throw std::runtime_error("invalid field value");
    }
    handler->template set<FieldPtr>(std::move(value));
}
```

`Quantity`, `Choice`, `morph::time::Timestamp`, `std::optional<std::string>`,
and plain scalars all already round-trip through `glz::read_json` (`Choice`
and `Quantity` each have a `glz::meta` specialisation), so one generic
closure body covers every field kind with no per-type branching.

**Registration point:** `BRIDGE_REGISTER_ACTION_4` (in `registry.hpp`)
gains one extra line calling
`morph::model::detail::registerFieldSettersOnce<M, A>(...)`, mirroring the
existing `registerActionOnce` call right next to it. This is the only
change to the macro; nothing a user writes changes. Existing calls to
`BRIDGE_REGISTER_ACTION` in `lab_model.hpp`, the bank example, and every
test keep compiling unchanged and gain field-setter registration for free.

### 2. `BridgeHandler<Model>::setFieldJson` (new method, `bridge.hpp`)

```cpp
void setFieldJson(std::string_view actionType, std::string_view fieldName, std::string_view jsonValue) {
    FieldSetterRegistry::instance().set(
        std::string{ModelTraits<Model>::typeId()}, actionType, fieldName, this, jsonValue);
}
```

Thin: looks up the closure, invokes it with `this`. Errors (unknown
action/field, bad JSON) throw `std::runtime_error`; `FormsController`
catches and reports via the existing error-signal path rather than
crashing the GUI.

### 3. `FormsController` rewrite (`examples/forms/gui_qml/FormsController.{hpp,cpp}`)

Drops: `morph::backend::RemoteServer`, `morph::wire::*`, `_modelId`,
`_nextCallId`, `_pool` sized for a manual server.

Adds:
- `morph::exec::ThreadPoolExecutor _pool` (worker executor, same role as
  `bankgui::BankClient::_pool`) and `morph::qt::QtExecutor _gui`.
- `morph::bridge::Bridge _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)}`.
- `morph::bridge::BridgeHandler<lab::LabModel> _handler{_bridge, &_gui}`.
- One `subscribe<Action>(...)` call per known action type
  (`ComputeDryDensity`, `RecordMeasurement`) registered in the
  constructor — this is the one place per-action code still exists,
  matching `AccountController`'s pattern of one `.then()`/`.onError()`
  pair per action. It is unavoidable without type erasure on the *result*
  side too, and the design deliberately does not add that: results are
  few, fixed, and already need type-specific formatting
  (`humanize`/`toMap`-style) on the QML side eventually. `ListSamples`
  keeps using plain `_handler.execute()` (a one-shot query, not a draft).
- `Q_INVOKABLE void setField(QString actionType, QString fieldName, QString jsonValue)`
  → `_handler.setFieldJson(...)`, catches, emits `fieldError` on failure.
- Results surface via the same `replyReceived(actionType, ok, payload)` /
  `optionsReceived(...)` signals QML already listens for — the *signal
  surface* to QML does not change shape, only what feeds it.

`submit(actionType, bodyJson)` is removed. `fetchOptions` stays, backed by
`_handler.execute(ListSamples{})` instead of a hand-built envelope.

### 4. `DynamicForm.qml`

`revalidate()` already computes `ready` and assembles field values; add:
when a field changes and the *whole form* is `ready`, call
`controller.setField(actionType, name, jsonValue)` for the field that
changed (not the whole form — `setFieldJson` sets one field at a time,
matching `set<>`'s per-field contract). The "execute" `Button` is removed
from the reactive path; the built line (`previewLine`) stays visible as a
read-only preview of what was last sent, and `resultText` updates live as
replies stream in via the unchanged `onReplyReceived` handler.

### Error handling

- Unknown `(actionType, fieldName)` (typo, stale schema): `setFieldJson`
  throws; `FormsController::setField` catches and emits the existing
  `error`-style signal rather than propagating into Qt's slot-invocation
  exception boundary (undefined behaviour today if left uncaught).
- Bad JSON for a field's real type (e.g. non-numeric text for a
  `Quantity`): same path — `glz::read_json` failure is surfaced as a
  catchable exception, not a crash. The QML side already gates on
  `revalidate()`'s own regex checks before calling `setField`, so this is
  a defence-in-depth path, not the primary validation gate.
- `ActionValidator<Action>::ready()` (unchanged, existing mechanism) still
  decides whether a call actually fires after each `set<>` — untouched by
  this design.

### Testing

- New unit test (`tests/test_bridge_field_setters.cpp` or added to
  `tests/test_subscription.cpp`): registers a test action via
  `BRIDGE_REGISTER_ACTION`, calls `BridgeHandler::setFieldJson` for each
  field with JSON literals, asserts the draft fires and the subscribed
  callback receives the right result — covering scalars, `Quantity`, and
  `Choice` fields to prove the generic closure works across field kinds.
- `examples/forms/gui_qml/tests/tst_main.cpp` (Qt Quick Test) gains a case
  driving `DynamicForm` through `setField`-style edits and asserting
  `resultText` updates without any button click.
- Existing `forms_qml_logic` test target and CTest wiring are unchanged.

## Open questions / risks

- `void*` type erasure in `FieldSetterRegistry::Setter` mirrors the
  existing untyped-`Runner` pattern in `ActionDispatcher`
  (`std::function<std::string(IModelHolder&, std::string_view)>`) rather
  than introducing `std::any`; this keeps the new code consistent with
  the codebase's existing type-erasure idiom.
- `setFieldJson` calling `set<>` inherits `set<>`'s existing coalescing
  behaviour (bursts of `set<>` calls while a call is in flight collapse to
  the latest draft, then refire) — no new concurrency behaviour is
  introduced.
