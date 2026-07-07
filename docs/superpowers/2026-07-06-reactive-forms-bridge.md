# Reactive Forms over Bridge/BridgeHandler

The QML forms demo (`examples/forms/gui_qml`) talks to `lab::LabModel`
exclusively through `morph::bridge::Bridge`/`BridgeHandler<LabModel>` — the
framework's real client API. Forms are reactive: an action fires
automatically as soon as its required fields are valid, on every edit; there
is no submit button. Adding a new action to `lab_model.hpp` (or any
`BRIDGE_REGISTER_ACTION` call site) requires zero lines beyond the
registration itself — no per-action GUI code exists anywhere.

## Architecture

```
DynamicForm.qml ──revalidate()──▶ FormsController::submitIfValid(actionType, bodyJson)
                                        │
                                        ▼
                        BridgeHandler<LabModel>::executeJson(actionType, bodyJson)
                                        │  (string → closure lookup)
                                        ▼
                        ActionExecuteRegistry::execute(modelId, actionId, handler, body)
                                        │  (type-erased closure, registered by
                                        │   BRIDGE_REGISTER_ACTION at static init)
                                        ▼
                        BridgeHandler<LabModel>::execute<Action>(action)
                                        │  (real compile-time path: sessions,
                                        │   backend switches, completions)
                                        ▼
                        Bridge ─▶ LocalBackend ─▶ LabModel::execute
```

### `ActionExecuteRegistry` (`include/morph/bridge.hpp`)

A process-wide singleton mapping `(modelId, actionId)` strings to
type-erased executors. It is what lets QML dispatch actions it only knows
by name:

```cpp
class ActionExecuteRegistry {
public:
    using Executor = std::function<::morph::async::Completion<std::string>(void* /* BridgeHandler<Model>* */,
                                                                            std::string_view bodyJson)>;
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId);

    // Throws std::runtime_error if no executor is registered for the pair.
    ::morph::async::Completion<std::string> execute(std::string_view modelId, std::string_view actionId,
                                                      void* handler, std::string_view bodyJson) const;

    static ActionExecuteRegistry& instance();
};
```

Each registered closure deserialises the JSON body via
`ActionTraits<Action>::fromJson`, calls the real
`BridgeHandler<Model>::execute<Action>()` (so sessions, backend switches,
and completions behave exactly as at hand-written call sites), and
re-serialises the result via `ActionTraits<Action>::resultToJson`. It
resolves on the handler's GUI executor (`BridgeHandler::guiExecutor()`).

Registration happens inside `BRIDGE_REGISTER_ACTION_4`
(`include/morph/registry.hpp`) via
`morph::model::detail::registerActionExecutorOnce<Model, Action>`, next to
the existing server-side `registerActionOnce` call. Every action registered
with `BRIDGE_REGISTER_ACTION` therefore has a generic-execute entry
automatically.

**Hard requirement:** `registerActionExecutorOnce` is only defined in
`bridge.hpp`, so every translation unit that calls `BRIDGE_REGISTER_ACTION`
must include `bridge.hpp` (directly or transitively), or its static
initializer fails to link.

This registry is the client-side counterpart of
`morph::model::detail::ActionDispatcher` (`registry.hpp`), which calls
`Model::execute` directly against an owned model holder and is only used
server-side. Both use the same `void*`/`std::function` type-erasure idiom.

### `BridgeHandler<Model>::executeJson` (`bridge.hpp`)

```cpp
::morph::async::Completion<std::string> executeJson(std::string_view actionType, std::string_view bodyJson);
```

Thin wrapper: looks up the executor under
`(ModelTraits<Model>::typeId(), actionType)` and invokes it with `this`.

### `FormsController` (`examples/forms/gui_qml/FormsController.{hpp,cpp}`)

Owns the client stack:

- `morph::exec::ThreadPoolExecutor _pool` (worker) and
  `morph::qt::QtExecutor _gui` (GUI-thread delivery).
- `morph::bridge::Bridge _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)}`.
- `morph::bridge::BridgeHandler<lab::LabModel> _handler{_bridge, &_gui}`.

QML-facing API:

- `Q_INVOKABLE void submitIfValid(QString actionType, QString bodyJson)` —
  fully generic, no per-action branches. Calls
  `_handler.executeJson(...)`; `.then()`/`.onError()` emit
  `replyReceived(actionType, ok, payload)`.
- `fetchOptions` — backed by `_handler.execute(ListSamples{})`. This is the
  one fixed, hand-known query (combo-box options); it does not grow with
  the number of form actions.

### `DynamicForm.qml`

`revalidate()` computes `ready` and assembles the full JSON body
(`previewLine`) on every edit. When `ready` is `true` after an edit, it
calls `controller.submitIfValid(actionType, previewLine)` directly.
`previewLine` stays visible as a preview of what was last sent;
`resultText` updates live from the `onReplyReceived` handler.

## Error handling

- Unknown `actionType`: `executeJson` throws; `submitIfValid` catches and
  emits `replyReceived` with `ok=false` (never lets an exception cross
  Qt's slot-invocation boundary).
- Malformed JSON body: `ActionTraits<Action>::fromJson` throws
  `ParseError`; same catch-and-report path. Defence-in-depth only — QML
  gates on `revalidate()`'s required-field/regex checks before calling
  `submitIfValid`.
- `Model::execute` throwing (e.g. `RecordMeasurement`'s `validate()`
  guard): propagates through `Completion::onError` to
  `replyReceived(actionType, false, message)`.

## Known limitations

- No coalescing: a burst of edits while a call is in flight queues
  multiple concurrent `Bridge` calls; each resolves `replyReceived`
  independently, last-writer-wins on `resultText`. If this proves janky, a
  ~150 ms debounce timer in `DynamicForm.qml` is a self-contained
  follow-up.
- `BridgeHandler`'s compile-time fielded-draft API
  (`set<&Action::field>`/`subscribe<Action>`) is not usable generically:
  glaze plain-aggregate reflection exposes only field names, not
  compile-time member pointers. The generic path goes through
  `executeJson` instead.
- The options combo-box fetch is not reactive; it runs once per form from
  `Component.onCompleted`.
- `examples/forms/main.cpp` (schema dump / `--emit-html` / REPL) has no GUI
  and does not use `FormsController`; the static `--emit-html` page is pure
  JS with its own test (`test_html_math.mjs`).

## Testing

- `tests/test_bridge_execute_json.cpp` (Catch2): registers a test action
  via `BRIDGE_REGISTER_ACTION`, asserts `executeJson` resolves with the
  JSON-encoded result, and that a malformed body surfaces a parse error
  through `onError`.
- `examples/forms/gui_qml/tests/tst_main.cpp` (Qt Quick Test): drives
  `DynamicForm` through simulated field edits and asserts `resultText`
  updates without any button click. Runs offscreen; the test target
  deploys the Qt runtime + offscreen plugin.
