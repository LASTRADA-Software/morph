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
"execute" button; nothing recomputes until the user clicks it.

**Investigated and rejected:** the framework's field-by-field, auto-fire
API — `BridgeHandler::set<&Action::field>(value)` / `subscribe<Action>(cb)`
(used today only in `tests/test_subscription.cpp`) — looked like the
natural fit, since it fires automatically once a draft becomes valid.
It does not work here: `set<auto FieldPtr>` requires a real compile-time
pointer-to-member (`double Action::*`) as a template argument, but
`DynamicForm.qml` only knows field names as runtime strings from the JSON
schema. Generating that pointer generically from glaze reflection was
checked directly against glaze's `reflect<T>` implementation
(`glaze/core/reflect.hpp`): for a plain aggregate with no `glz::meta`
specialisation (every action in this codebase, e.g. `RecordMeasurement`),
glaze's `reflectable<T>` reflection path exposes only field **names**
(`keys`) and a `to_tie()`-based tuple of runtime references — no
compile-time member-pointer artifact exists to extract. Producing one
per field would need either C++26 static reflection (not in use here) or
a hand-written `glz::meta<Action>` per action declaring each member
pointer explicitly, which reintroduces exactly the per-action boilerplate
this design exists to avoid. `set<>`/`subscribe<>` is therefore left as
what it is today — a compile-time API for hand-written C++ call sites —
and out of scope for the schema-driven QML forms.

The chosen alternative keeps the *outcome* (fires automatically as the
user types, no button) without needing a field-level compile-time hook:
the controller assembles the full action body as JSON (the QML side
already does this for the current submit button) and calls a new
generic, JSON-in/JSON-out execute path through `Bridge`/`BridgeHandler`
whenever the assembled body is schema-valid. This still requires
resolving a runtime action-type string to a concrete C++ `Action` type to
call `BridgeHandler::execute<Action>()` — the same shape of problem, one
level up. It is solved the same way `ActionDispatcher` already solves it
for `RemoteServer` (`registry.hpp`): a static-init-populated map from
action-type string to a type-erased closure, built automatically inside
`BRIDGE_REGISTER_ACTION` (no new macro, no new per-action declaration).

## Goals

- `FormsController` talks to `LabModel` exclusively through
  `Bridge`/`BridgeHandler<LabModel>` — no `wire::Envelope`, no
  `RemoteServer`, no `ActionDispatcher`/`ModelRegistryFactory` singletons
  touched from the GUI layer.
- Forms fire automatically as soon as their required fields are valid
  (live recompute) — no submit button in the reactive path.
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

### 1. `ActionExecuteRegistry` (new, in `include/morph/bridge.hpp`)

A process-wide singleton with the same shape as the existing
`ActionDispatcher` (`registry.hpp`): a static-init-populated map, keyed by
`(modelId, actionId)`, of type-erased closures — except this closure calls
through `Bridge::executeVia<Model, Action>` (routing, sessions, backend
switches, completions) instead of `ActionDispatcher::Runner`, which calls
`Model::execute` directly against an already-owned `IModelHolder` and is
only ever invoked server-side (inside `RemoteServer`/`ActionDispatcher`
dispatch, never from a client).

```cpp
class ActionExecuteRegistry {
public:
    // Deserialises `bodyJson` into the concrete Action, dispatches it
    // through the handler's Bridge, and resolves with the JSON-encoded
    // result (or the exception, unchanged) on the handler's gui executor.
    using Executor = std::function<::morph::async::Completion<std::string>(void* /* BridgeHandler<Model>* */,
                                                                            std::string_view bodyJson)>;

    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId);

    ::morph::async::Completion<std::string> execute(std::string_view modelId, std::string_view actionId,
                                                      void* handler, std::string_view bodyJson) const;  // throws if unknown

    static ActionExecuteRegistry& instance();
};
```

The registered closure:

```cpp
[](void* handlerVoid, std::string_view bodyJson) -> ::morph::async::Completion<std::string> {
    auto* handler = static_cast<BridgeHandler<Model>*>(handlerVoid);
    Action action = ActionTraits<Action>::fromJson(bodyJson);  // already exists
    auto resultState = std::make_shared<::morph::async::detail::CompletionState<std::string>>();
    handler->template execute<Action>(std::move(action))
        .then([resultState](typename ActionTraits<Action>::Result result) {
            resultState->setValue(ActionTraits<Action>::resultToJson(result));  // already exists
        })
        .onError([resultState](std::exception_ptr err) { resultState->setException(err); });
    return {resultState, handler->guiExecutor()};  // BridgeHandler exposes its _guiExec
}
```

`ActionTraits<A>::fromJson`/`resultToJson` already exist (generated by
`BRIDGE_REGISTER_ACTION_4` for every action) — this closure only chains
them around the real `execute<Action>()` call. No new JSON codec, no new
reflection.

**Registration point:** `BRIDGE_REGISTER_ACTION_4` (in `registry.hpp`)
gains one extra line calling
`morph::model::detail::registerActionExecutorOnce<M, A>(...)`, mirroring
the existing `registerActionOnce` call right next to it. This is the only
change to the macro; nothing a user writes changes. Existing calls to
`BRIDGE_REGISTER_ACTION` in `lab_model.hpp`, the bank example, and every
test keep compiling unchanged and gain generic-execute registration for
free — `lab_model.hpp` needs zero new lines.

### 2. `BridgeHandler<Model>::executeJson` (new method, `bridge.hpp`)

```cpp
::morph::async::Completion<std::string> executeJson(std::string_view actionType, std::string_view bodyJson) {
    return ActionExecuteRegistry::instance().execute(
        std::string{ModelTraits<Model>::typeId()}, actionType, this, bodyJson);
}
```

Thin: looks up the closure, invokes it with `this`. `ModelTraits<Model>`
is already required by every `BridgeHandler<Model>` instantiation, so no
new constraint is added. Needs a `guiExecutor()` accessor added to
`BridgeHandler` (it already stores `_guiExec`; today nothing outside the
class reads it back).

### 3. `FormsController` rewrite (`examples/forms/gui_qml/FormsController.{hpp,cpp}`)

Drops: `morph::backend::RemoteServer`, `morph::wire::*`, `_modelId`,
`_nextCallId`, `_pool` sized for a manual server.

Adds:
- `morph::exec::ThreadPoolExecutor _pool` (worker executor, same role as
  `bankgui::BankClient::_pool`) and `morph::qt::QtExecutor _gui`.
- `morph::bridge::Bridge _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)}`.
- `morph::bridge::BridgeHandler<lab::LabModel> _handler{_bridge, &_gui}`.
- `Q_INVOKABLE void submitIfValid(QString actionType, QString bodyJson)`
  (called by QML whenever the assembled body is schema-valid, on every
  edit) → `_handler.executeJson(actionType.toStdString(), bodyJson.toStdString())`,
  `.then()`/`.onError()` emit the existing `replyReceived(actionType, ok, payload)`
  signal. Fully generic: no per-action branch, no per-action subscription
  to register in the constructor.
- `fetchOptions` stays, backed by `_handler.execute(ListSamples{})`
  instead of a hand-built envelope (this one call site *is* per-action —
  `ListSamples` is the one fixed, hand-known query the controller always
  needs — but it already existed conceptually in the current code and
  does not grow with the number of form actions).

`submit(actionType, bodyJson)` is renamed `submitIfValid` and re-fires on
every call rather than only on a button click; the signal surface to QML
(`replyReceived`, `optionsReceived`) is unchanged.

### 4. `DynamicForm.qml`

`revalidate()` already computes `ready` and assembles `previewLine` (the
full JSON body) on every edit. Add: when `ready` becomes (or stays) `true`
after an edit, call `controller.submitIfValid(actionType, previewLine)`
directly from `revalidate()` — no separate per-field call, since the
execute path takes the whole body already assembled exactly as it is
today. The "execute" `Button` is removed from the reactive path; the
built line (`previewLine`) stays visible as a preview of what was last
sent, and `resultText` updates live as replies stream in via the
unchanged `onReplyReceived` handler.

### Error handling

- Unknown `actionType` (typo, stale schema): `executeJson` throws;
  `FormsController::submitIfValid` catches and emits `replyReceived` with
  `ok=false` rather than propagating into Qt's slot-invocation exception
  boundary (undefined behaviour today if left uncaught).
- Bad/incomplete JSON body: `ActionTraits<Action>::fromJson` already
  throws `ParseError` on malformed input (existing behaviour, exercised
  today via `ActionDispatcher::dispatch`); same catch-and-report path.
  The QML side already gates on `revalidate()`'s own regex/required-field
  checks before calling `submitIfValid`, so this is a defence-in-depth
  path, not the primary validation gate.
- `Model::execute` throwing (e.g. `RecordMeasurement`'s own
  `validate()`-based guard): propagates through `Completion::onError` to
  `replyReceived(actionType, false, message)` exactly as the current
  `sendExecute` error path already does.

### Testing

- New unit test (`tests/test_bridge_execute_json.cpp` or added to
  `tests/test_bridge_local.cpp`): registers a test action via
  `BRIDGE_REGISTER_ACTION`, calls `BridgeHandler::executeJson` with a JSON
  body, asserts the `Completion<std::string>` resolves with the correctly
  JSON-encoded result — and a second case with a malformed body asserting
  the completion's `onError` fires with a parse error, matching
  `ActionTraits<A>::fromJson`'s existing throw behaviour.
- `examples/forms/gui_qml/tests/tst_main.cpp` (Qt Quick Test) gains a case
  driving `DynamicForm` through simulated field edits and asserting
  `resultText` updates without any button click.
- Existing `forms_qml_logic` test target and CTest wiring are unchanged.

## Open questions / risks

- `void*` type erasure in `ActionExecuteRegistry::Executor` mirrors the
  existing untyped-`Runner` pattern in `ActionDispatcher`
  (`std::function<std::string(IModelHolder&, std::string_view)>`) rather
  than introducing `std::any`; this keeps the new code consistent with
  the codebase's existing type-erasure idiom.
- `executeJson` re-fires the whole action on every keystroke once the
  form is valid, with no coalescing: a burst of edits while a call is
  in flight will queue multiple concurrent `Bridge::executeVia` calls
  (each independently resolving `replyReceived`, last-writer-wins on
  `resultText`). This is a real behavioural difference from `set<>`'s
  built-in coalescing (which this design deliberately does not use) and
  should be noted in the demo's README; if it proves visibly janky in
  practice, a debounce timer in `DynamicForm.qml` (fire ~150ms after the
  last edit) is a self-contained follow-up, not a blocker for this plan.
