# Reactive Forms over Bridge/BridgeHandler

Date: 2026-07-06

Combined superpowers documentation: the design spec followed by the implementation plan (previously `specs/2026-07-06-reactive-forms-bridge-design.md` and `plans/2026-07-06-reactive-forms-bridge.md`).

---

## Reactive forms demo over BridgeHandler

### Problem

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

### Goals

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

### Non-goals

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

### Design

#### 1. `ActionExecuteRegistry` (new, in `include/morph/bridge.hpp`)

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

#### 2. `BridgeHandler<Model>::executeJson` (new method, `bridge.hpp`)

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

#### 3. `FormsController` rewrite (`examples/forms/gui_qml/FormsController.{hpp,cpp}`)

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

#### 4. `DynamicForm.qml`

`revalidate()` already computes `ready` and assembles `previewLine` (the
full JSON body) on every edit. Add: when `ready` becomes (or stays) `true`
after an edit, call `controller.submitIfValid(actionType, previewLine)`
directly from `revalidate()` — no separate per-field call, since the
execute path takes the whole body already assembled exactly as it is
today. The "execute" `Button` is removed from the reactive path; the
built line (`previewLine`) stays visible as a preview of what was last
sent, and `resultText` updates live as replies stream in via the
unchanged `onReplyReceived` handler.

#### Error handling

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

#### Testing

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

### Open questions / risks

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

---

## Reactive Forms Over BridgeHandler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the QML forms demo (`examples/forms/gui_qml`) talk to `LabModel` through `morph::bridge::Bridge`/`BridgeHandler` instead of hand-built `wire::Envelope`s over `RemoteServer`, and make forms fire automatically as the user types (no submit button) — with zero new C++ declarations required per action.

**Architecture:** A new `ActionExecuteRegistry` (in `include/morph/bridge.hpp`, alongside `Bridge`/`BridgeHandler`) maps `(modelId, actionId)` strings to type-erased closures that deserialize a JSON body, call the real compile-time `BridgeHandler<Model>::execute<Action>()`, and re-serialize the result. It is populated automatically inside the existing `BRIDGE_REGISTER_ACTION_4` macro (`include/morph/registry.hpp`) — every already-registered action gains a generic-execute entry for free. `BridgeHandler<Model>` gains one new method, `executeJson(actionType, bodyJson) -> Completion<std::string>`, that looks up and invokes the registered closure. `FormsController` is rewritten to own a `Bridge` + `BridgeHandler<LabModel>` and exposes a single generic `Q_INVOKABLE submitIfValid(actionType, bodyJson)` entry point; `DynamicForm.qml` calls it from `revalidate()` on every edit once the assembled body is valid, replacing the explicit "execute" button.

**Tech Stack:** C++23, glaze (JSON + reflection), Catch2, Qt 6 (Quick/Qml/QuickTest), CMake/Ninja, MSVC (primary) via vcpkg.

### Global Constraints

- `set<&Action::field>`/`subscribe<Action>` (BridgeHandler's compile-time fielded-draft API) is **out of scope** — confirmed unusable generically because glaze's plain-aggregate reflection (`reflectable<T>`, no `glz::meta`) exposes only field names, not compile-time member pointers. Do not attempt to resurrect it inside this plan.
- Adding a new action to `lab_model.hpp` (or any `BRIDGE_REGISTER_ACTION` call site) must require **zero new lines** beyond what it needs today. Every task that touches `BRIDGE_REGISTER_ACTION` must preserve this.
- `examples/forms/main.cpp` (schema dump / `--emit-html` / REPL) is untouched — no GUI, does not use `FormsController`.
- No change to `morph::forms::schemaJson` output or its `x-*` extension keys.
- Type erasure in new registries follows the existing `ActionDispatcher::Runner` idiom (`std::function` over a `void*`/`IModelHolder&`), not `std::any` — match `include/morph/registry.hpp`'s existing style.
- All new C++ files use `// SPDX-License-Identifier: Apache-2.0` as the first line, matching every existing header/source in this repo.

---

### Task 1: `ActionExecuteRegistry` in `bridge.hpp` with a direct unit test

**Files:**
- Modify: `include/morph/bridge.hpp` (add `ActionExecuteRegistry` class and `defaultActionExecuteRegistry()` free function, near the top of `namespace morph::bridge`, before `class Bridge`)
- Modify: `include/morph/registry.hpp` (add `registerActionExecutorOnce` helper in `namespace morph::model::detail`, and one call to it inside `BRIDGE_REGISTER_ACTION_4`)
- Test: `tests/test_bridge_execute_json.cpp` (new)
- Modify: `tests/CMakeLists.txt` (add the new test file to `add_executable(morph_tests ...)`)

**Interfaces:**
- Consumes: `morph::bridge::Bridge`, `morph::bridge::BridgeHandler<Model>::execute<Action>()` (existing, `include/morph/bridge.hpp:412-415`), `morph::model::ActionTraits<Action>::fromJson`/`resultToJson` (existing, generated by `BRIDGE_REGISTER_ACTION_4`), `morph::async::Completion<T>` (existing, `include/morph/completion.hpp`).
- Produces: `morph::bridge::ActionExecuteRegistry` singleton with `execute(modelId, actionId, handlerVoid, bodyJson) -> Completion<std::string>` (throws `std::runtime_error` if `(modelId, actionId)` is unregistered). `morph::model::detail::registerActionExecutorOnce<Model, Action>(modelId, actionId)` — called automatically from `BRIDGE_REGISTER_ACTION_4`, not meant to be called by hand.

Note on layering: `registry.hpp` is included by `bridge.hpp` today (`bridge.hpp:19` includes `"registry.hpp"`), so `bridge.hpp` can freely reference `morph::model::detail` types. `registry.hpp` must NOT include `bridge.hpp` back (would cycle) — `registerActionExecutorOnce` in `registry.hpp` calls into `morph::bridge::ActionExecuteRegistry::instance()` via a forward declaration plus an out-of-line definition placed in `bridge.hpp` after both classes are visible. See Step 3 for the exact split.

- [ ] **Step 1: Write the failing test**

Create `tests/test_bridge_execute_json.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <string>

#include "test_support.hpp"

namespace {

struct AddNumbers {
    int a = 0;
    int b = 0;
};

struct AddResult {
    int sum = 0;
};

struct MathModel {
    AddResult execute(const AddNumbers& action) { return AddResult{.sum = action.a + action.b}; }
};

}  // namespace

BRIDGE_REGISTER_MODEL(MathModel, "Test_ExecJson_MathModel")
BRIDGE_REGISTER_ACTION(MathModel, AddNumbers, "Test_ExecJson_AddNumbers")

using SyncExecutor = morph::testing::InlineExecutor;

TEST_CASE("ActionExecuteRegistry: executeJson deserialises, executes, and re-serialises", "[bridge][execute-json]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MathModel> handler{bridge, &cbExec};

    std::optional<std::string> resultJson;
    std::atomic<bool> done{false};
    handler.executeJson("Test_ExecJson_AddNumbers", R"({"a":3,"b":4})")
        .then([&](std::string json) {
            resultJson = std::move(json);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(resultJson.has_value());
    REQUIRE(*resultJson == R"({"sum":7})");
}

TEST_CASE("ActionExecuteRegistry: executeJson reports parse errors via onError", "[bridge][execute-json]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MathModel> handler{bridge, &cbExec};

    bool sawError = false;
    std::atomic<bool> done{false};
    handler.executeJson("Test_ExecJson_AddNumbers", R"({"a":"not a number"})")
        .then([&](std::string) { done.store(true); })
        .onError([&](const std::exception_ptr&) {
            sawError = true;
            done.store(true);
        });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(sawError);
}

TEST_CASE("ActionExecuteRegistry: unknown action type throws", "[bridge][execute-json]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MathModel> handler{bridge, &cbExec};

    REQUIRE_THROWS_AS(handler.executeJson("NoSuchAction", "{}"), std::runtime_error);
}
```

Check `tests/test_support.hpp` exposes `waitUntil` (confirmed present at `tests/test_support.hpp:40` as `template <typename Pred> void waitUntil(Pred, budget = kDefaultWaitBudget)` — if the exact name differs, use whatever polling helper is already defined there instead of adding a new one).

- [ ] **Step 2: Add the new test file to the test target**

In `tests/CMakeLists.txt`, add `test_bridge_execute_json.cpp` to the `add_executable(morph_tests ...)` list (alphabetical-ish position doesn't matter — follow the existing loose grouping, e.g. right after `test_bridge_remote.cpp`):

```cmake
    test_bridge_local.cpp
    test_bridge_remote.cpp
    test_bridge_execute_json.cpp
    test_remote_extra.cpp
```

- [ ] **Step 3: Run the test to verify it fails to compile**

Run: `cmake --build build/cl-debug --target morph_tests`
Expected: FAIL — `'executeJson': is not a member of 'morph::bridge::BridgeHandler<MathModel>'` (or similar "no member" compiler error). If `build/cl-debug` does not exist yet, configure it first: `cmake --preset cl-debug` (requires `VCPKG_ROOT` set to the vcpkg install, e.g. `C:/Program Files/Microsoft Visual Studio/18/Professional/VC/vcpkg`).

- [ ] **Step 4: Implement `ActionExecuteRegistry` in `bridge.hpp`**

In `include/morph/bridge.hpp`, add right after the `namespace morph::bridge {` line and before `namespace detail {` (i.e. before line 24's `namespace detail {` block):

```cpp
namespace morph::bridge {

/// @brief Type-erased, JSON-in/JSON-out execute path for actions whose
///        concrete C++ type is only known by its registered string id at
///        the call site (e.g. a schema-driven GUI that reads action names
///        out of a JSON Schema at runtime).
///
/// Populated automatically by `BRIDGE_REGISTER_ACTION` — no action-specific
/// code is required at any call site. Every entry calls through the real
/// `BridgeHandler<Model>::execute<Action>()` (so sessions, backend
/// switches, and completions all behave exactly as they do for hand-written
/// call sites), unlike `morph::model::detail::ActionDispatcher`, which
/// calls `Model::execute` directly against an already-owned model holder
/// and is only ever used server-side.
class ActionExecuteRegistry {
public:
    /// @brief Deserialises `bodyJson`, dispatches through the handler's
    ///        `Bridge`, and resolves with the JSON-encoded result.
    using Executor = std::function<::morph::async::Completion<std::string>(void*, std::string_view)>;

    /// @brief Registers the executor for `(Model, Action)` under the given string ids.
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId) {
        Key key{std::string{modelId}, std::string{actionId}};
        _executors[key] = [](void* handlerVoid, std::string_view bodyJson) -> ::morph::async::Completion<std::string> {
            auto* handler = static_cast<BridgeHandler<Model>*>(handlerVoid);
            auto resultState = std::make_shared<::morph::async::detail::CompletionState<std::string>>();
            Action action = ::morph::model::ActionTraits<Action>::fromJson(bodyJson);
            handler->template execute<Action>(std::move(action))
                .then([resultState](typename ::morph::model::ActionTraits<Action>::Result result) {
                    resultState->setValue(::morph::model::ActionTraits<Action>::resultToJson(result));
                })
                .onError([resultState](const std::exception_ptr& err) { resultState->setException(err); });
            return {resultState, handler->guiExecutor()};
        };
    }

    /// @brief Looks up and invokes the executor for `(modelId, actionId)`.
    /// @throws std::runtime_error if no executor was registered for that pair.
    [[nodiscard]] ::morph::async::Completion<std::string> execute(std::string_view modelId, std::string_view actionId,
                                                                    void* handler, std::string_view bodyJson) const {
        auto iter = _executors.find(Key{std::string{modelId}, std::string{actionId}});
        if (iter == _executors.end()) {
            throw std::runtime_error("unknown action for executeJson: " + std::string{modelId} + "/" +
                                     std::string{actionId});
        }
        return iter->second(handler, bodyJson);
    }

    /// @brief Returns the process-level singleton registry.
    static ActionExecuteRegistry& instance();

private:
    using Key = std::pair<std::string, std::string>;
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            std::size_t seed = std::hash<std::string>{}(key.first);
            seed ^= std::hash<std::string>{}(key.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
    std::unordered_map<Key, Executor, KeyHash> _executors;
};

inline ActionExecuteRegistry& ActionExecuteRegistry::instance() {
    static ActionExecuteRegistry inst;
    return inst;
}

namespace detail {
```

This inserts the new class inside the existing `namespace morph::bridge { ... namespace detail { ... } }` structure — `ActionExecuteRegistry` sits in `morph::bridge` directly (sibling of `Bridge`, `BridgeHandler`), and the pre-existing `namespace detail { ... }` block (starting at what was line 24) continues unchanged right after it.

`bridge.hpp` already includes `<functional>`, `<memory>`, `<string>`, `<string_view>`, `<unordered_map>`, `<utility>` (see its existing include list, lines 4-15) and `"registry.hpp"` (line 19) — no new includes needed for this step. `<stdexcept>` is also already included (line 10), covering `std::runtime_error`.

- [ ] **Step 5: Add `BridgeHandler<Model>::executeJson` and `guiExecutor()`**

In `include/morph/bridge.hpp`, inside `class BridgeHandler` (the existing class, currently ending around line 486-598), add two public methods right after the existing `execute` method (which ends at what is currently line 415, just before `subscribe`):

```cpp
    /// @brief Type-erased execute: looks up the action by its registered
    ///        string id and dispatches it through `ActionExecuteRegistry`.
    ///
    /// Use this only when the concrete `Action` type is not known at the
    /// call site (e.g. a schema-driven UI reading action names out of a
    /// JSON Schema at runtime). Prefer the templated `execute<Action>()`
    /// whenever the type is known at compile time.
    ///
    /// @param actionType Registered action type-id (the `NAME` passed to `BRIDGE_REGISTER_ACTION`).
    /// @param bodyJson   JSON-encoded action body.
    /// @return Completion resolving with the JSON-encoded result.
    /// @throws std::runtime_error if `actionType` was never registered for `Model`.
    [[nodiscard]] ::morph::async::Completion<std::string> executeJson(std::string_view actionType,
                                                                        std::string_view bodyJson) {
        return ActionExecuteRegistry::instance().execute(
            std::string{::morph::model::ModelTraits<Model>::typeId()}, actionType, this, bodyJson);
    }

    /// @brief The executor used to deliver this handler's `Completion` callbacks.
    /// @return The GUI/callback executor passed at construction.
    [[nodiscard]] ::morph::exec::IExecutor* guiExecutor() const noexcept { return _guiExec; }
```

- [ ] **Step 6: Register the executor automatically inside `BRIDGE_REGISTER_ACTION_4`**

In `include/morph/registry.hpp`, add a new detail helper right after `registerActionOnce` (currently lines 286-291):

```cpp
/// @brief Static-init helper for `BRIDGE_REGISTER_ACTION`'s generic-execute registration.
///
/// Defined out-of-line in `bridge.hpp` (after `ActionExecuteRegistry` is
/// visible) to avoid a `registry.hpp` -> `bridge.hpp` include cycle;
/// `bridge.hpp` already includes `registry.hpp`, not the other way round.
template <typename Model, typename Action>
inline bool registerActionExecutorOnce(std::string_view modelId, std::string_view actionId) noexcept;
```

Then in `BRIDGE_REGISTER_ACTION_4` (currently lines 344-382), add one line to the generated `namespace { ... }` block, right after the existing `bridge_action_reg_##M##_##A` line:

```cpp
    namespace {                                                                                          \
    [[maybe_unused]] const bool bridge_action_reg_##M##_##A =                                            \
        morph::model::detail::registerActionOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME);    \
    [[maybe_unused]] const bool bridge_action_exec_reg_##M##_##A =                                       \
        morph::model::detail::registerActionExecutorOnce<M, A>(morph::model::ModelTraits<M>::typeId(), NAME); \
    }
```

Now, in `include/morph/bridge.hpp`, define the out-of-line body — add this right after the `ActionExecuteRegistry` class and its `instance()` definition (from Step 4), still before the pre-existing `namespace detail {` line:

```cpp
}  // namespace morph::bridge

namespace morph::model::detail {

template <typename Model, typename Action>
inline bool registerActionExecutorOnce(std::string_view modelId, std::string_view actionId) noexcept {
    ::morph::bridge::ActionExecuteRegistry::instance().registerAction<Model, Action>(modelId, actionId);
    return true;
}

}  // namespace morph::model::detail

namespace morph::bridge {

namespace detail {
```

(This briefly closes and reopens `namespace morph::bridge` around the `morph::model::detail` block — matching the file's existing style of nested `namespace X { ... }` blocks rather than fully-qualified names for a multi-line definition.)

**Important ordering constraint:** `registry.hpp`'s declaration of `registerActionExecutorOnce` (a template) only needs to be *declared* before `BRIDGE_REGISTER_ACTION_4` expands (macro text is checked at the point of use, i.e. wherever `BRIDGE_REGISTER_ACTION` is called in user code, which always includes `bridge.hpp` transitively — confirm this holds for `lab_model.hpp`: it includes `<morph/registry.hpp>` directly per its current include list, and any translation unit calling `BRIDGE_REGISTER_ACTION` that also wants `Bridge`/`BridgeHandler` already includes `bridge.hpp`). Since `registry.hpp` only needs the *declaration* (function bodies are resolved at link time / are `inline`), and `bridge.hpp` includes `registry.hpp`, the definition in `bridge.hpp` is visible to any TU that includes `bridge.hpp` — which every `BRIDGE_REGISTER_ACTION` call site in this plan's scope (`lab_model.hpp`, the new test file) already does or will do. If a future action file calls `BRIDGE_REGISTER_ACTION` without including `bridge.hpp`, it will fail to link (undefined reference to `registerActionExecutorOnce`) — note this constraint in the doc comment on `registerActionExecutorOnce`'s declaration in `registry.hpp` (already drafted above) and additionally confirm in Step 7 that `lab_model.hpp` needs no include changes because `FormsController.cpp` (Task 3) will include `bridge.hpp` in the same binary.

- [ ] **Step 7: Build and run the test**

Run: `cmake --build build/cl-debug --target morph_tests`
Expected: builds cleanly.

Run: `ctest --test-dir build/cl-debug -R "execute-json" --output-on-failure`
Expected: all 3 new test cases PASS.

- [ ] **Step 8: Run the full existing test suite to check for regressions**

Run: `ctest --test-dir build/cl-debug --output-on-failure`
Expected: all tests pass (same pass count as before this change, plus the 3 new ones). This exercises every existing `BRIDGE_REGISTER_ACTION` call site in the test suite, confirming the macro change is backward compatible.

- [ ] **Step 9: Commit**

```bash
git add include/morph/bridge.hpp include/morph/registry.hpp tests/test_bridge_execute_json.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: add ActionExecuteRegistry for JSON-in/JSON-out action dispatch

BridgeHandler::executeJson(actionType, bodyJson) resolves a runtime
action-type string to the real compile-time execute<Action>() call,
registered automatically inside BRIDGE_REGISTER_ACTION. Lets a
schema-driven UI dispatch through Bridge/BridgeHandler without knowing
concrete C++ action types at compile time, with zero new declarations
per action.
EOF
)"
```

---

### Task 2: Rewrite `FormsController` to use `Bridge`/`BridgeHandler`

**Files:**
- Modify: `examples/forms/gui_qml/FormsController.hpp`
- Modify: `examples/forms/gui_qml/FormsController.cpp`

**Interfaces:**
- Consumes: `morph::bridge::Bridge`, `morph::bridge::BridgeHandler<lab::LabModel>::executeJson` (Task 1), `morph::bridge::BridgeHandler<lab::LabModel>::execute<lab::ListSamples>()` (existing), `morph::backend::LocalBackend` (existing, `include/morph/backend.hpp:148-152`), `morph::qt::QtExecutor` (existing, `include/morph/qt/qt_executor.hpp`).
- Produces: `FormsController` with the same public signal surface as before (`replyReceived(actionType, ok, payload)`, `optionsReceived(optionsAction, ok, payload)`, `schemasJson` property) plus a renamed invokable `submitIfValid(actionType, bodyJson)` replacing `submit(actionType, bodyJson)`. `fetchOptions(optionsAction)` keeps its existing signature.

- [ ] **Step 1: Update `FormsController.hpp`**

Replace the full contents of `examples/forms/gui_qml/FormsController.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// QML-facing controller for the schema-driven forms demo.
///
/// The QML layer renders forms from the schemas exposed here and submits
/// fully-assembled action bodies as JSON strings via `submitIfValid`,
/// called on every edit once the body validates client-side (no submit
/// button). This controller dispatches through a real `morph::bridge::Bridge`
/// + `BridgeHandler<LabModel>` — the same client API `examples/bank`'s GUI
/// uses — via the generic `BridgeHandler::executeJson` path, so it never
/// touches `morph::wire::Envelope` or `morph::backend::RemoteServer`
/// directly.

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

// Guarded like examples/bank/gui/controllers/AccountController.hpp: MOC
// only needs the Q_OBJECT/QML_ELEMENT macros above and the Q_INVOKABLE/
// Q_PROPERTY declarations below; it must not be pointed at morph's
// template-heavy headers (bridge.hpp, glaze) or the Qt executor.
#ifndef Q_MOC_RUN
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/qt/qt_executor.hpp>

#include "lab_model.hpp"
#endif

class FormsController : public QObject {
    Q_OBJECT
    QML_ELEMENT

    /// @brief `{actionType: schema}` JSON — everything the QML renderer needs.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

public:
    explicit FormsController(QObject* parent = nullptr);

    [[nodiscard]] QString schemasJson() const;

    /// @brief Dispatches @p bodyJson as the body of @p actionType if the
    ///        body is complete. Called by QML on every field edit once the
    ///        assembled body passes client-side validation — there is no
    ///        separate submit step. The reply arrives via `replyReceived`
    ///        on the GUI thread.
    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson);

    /// @brief Executes @p optionsAction with an empty body to fetch combo-box
    ///        options (a `Choice` field's declared provider). The reply
    ///        arrives via `optionsReceived` on the GUI thread.
    Q_INVOKABLE void fetchOptions(const QString& optionsAction);

signals:
    /// @brief Emitted once per `submitIfValid` call. @p payload is the
    ///        result JSON when @p ok, otherwise the error message.
    void replyReceived(const QString& actionType, bool ok, const QString& payload);

    /// @brief Emitted once per `fetchOptions`. @p payload is the options
    ///        action's result JSON when @p ok, otherwise the error message.
    void optionsReceived(const QString& optionsAction, bool ok, const QString& payload);

private:
    // Declaration order matters for destruction: `_handler` and `_bridge`
    // must be torn down before `_pool`/`_gui`, and `_pool` must outlive the
    // `LocalBackend` owned inside `_bridge` (constructed from it). Declared
    // in construction order so default destruction (reverse order) is safe.
    morph::exec::ThreadPoolExecutor _pool{2};
    morph::qt::QtExecutor _gui;
    morph::bridge::Bridge _bridge;
    morph::bridge::BridgeHandler<lab::LabModel> _handler;
};
```

- [ ] **Step 2: Update `FormsController.cpp`**

Replace the full contents of `examples/forms/gui_qml/FormsController.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#include "FormsController.hpp"

#include <exception>
#include <memory>
#include <string>

#include <morph/backend.hpp>

#include "lab_schemas.hpp"

namespace {

QString errorText(const std::exception_ptr& err) {
    try {
        if (err) {
            std::rethrow_exception(err);
        }
    } catch (const std::exception& exc) {
        return QString::fromUtf8(exc.what());
    } catch (...) {
        return QStringLiteral("unknown error");
    }
    return {};
}

}  // namespace

FormsController::FormsController(QObject* parent)
    : QObject{parent},
      _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)},
      _handler{_bridge, &_gui} {}

QString FormsController::schemasJson() const {
    return QString::fromStdString(lab::schemasJson());
}

void FormsController::submitIfValid(const QString& actionType, const QString& bodyJson) {
    auto const actionTypeStd = actionType.toStdString();
    _handler.executeJson(actionTypeStd, bodyJson.toStdString())
        .then([this, actionType](std::string resultJson) {
            emit replyReceived(actionType, true, QString::fromStdString(resultJson));
        })
        .onError([this, actionType](const std::exception_ptr& err) {
            emit replyReceived(actionType, false, errorText(err));
        });
}

void FormsController::fetchOptions(const QString& optionsAction) {
    _handler.execute(lab::ListSamples{})
        .then([this, optionsAction](lab::SampleList list) {
            std::string json = lab::ActionTraits_SampleList_toJson(list);
            emit optionsReceived(optionsAction, true, QString::fromStdString(json));
        })
        .onError([this, optionsAction](const std::exception_ptr& err) {
            emit optionsReceived(optionsAction, false, errorText(err));
        });
}
```

The `fetchOptions` body above has a placeholder call (`lab::ActionTraits_SampleList_toJson`) that does not exist — fix it in the next step by using the registered `ActionTraits` codec directly, since `SampleList` (the result of `ListSamples`) is not itself a registered action type and has no `BRIDGE_REGISTER_ACTION`-generated `toJson`. Use `glz::write_json` directly instead:

Replace the `fetchOptions` body with:

```cpp
void FormsController::fetchOptions(const QString& optionsAction) {
    _handler.execute(lab::ListSamples{})
        .then([this, optionsAction](lab::SampleList list) {
            std::string json;
            if (glz::write_json(list, json)) {
                emit optionsReceived(optionsAction, false, QStringLiteral("failed to encode options"));
                return;
            }
            emit optionsReceived(optionsAction, true, QString::fromStdString(json));
        })
        .onError([this, optionsAction](const std::exception_ptr& err) {
            emit optionsReceived(optionsAction, false, errorText(err));
        });
}
```

This needs `#include <glaze/glaze.hpp>` added to the top of `FormsController.cpp`'s include list (alongside the others).

**Note on `optionsJson()`'s shape:** the previous `fetchOptions` (via `RemoteServer`) returned the result of executing `ListSamples` wrapped as `{"samples": [...]}`  (matching `lab::SampleList`'s own field name) — `glz::write_json(list, json)` produces exactly the same shape (`{"samples":[{"id":1,"name":"Proctor A"}, ...]}`), so `DynamicForm.qml`'s `optionRows()` helper (which already handles "the result itself when it is an array, otherwise its first array-valued member" per `qml/DynamicForm.qml:236-244`) needs no change.

- [ ] **Step 3: Build**

Run: `cmake --build build/qml --target morph_forms_qml` (reuse the `build/qml` configure directory from the earlier `MORPH_BUILD_FORMS_QML=ON` work — if it no longer exists, reconfigure: `cmake -S . -B build/qml -G Ninja -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DMORPH_BUILD_FORMS_QML=ON -DCMAKE_BUILD_TYPE=Debug`)
Expected: builds cleanly. If it fails on `_handler{_bridge, &_gui}` member-initializer-list ordering (declaration order in the header must match: `_pool`, `_gui`, `_bridge`, `_handler` — already specified that way in Step 1), fix the header's member order to match and rebuild.

- [ ] **Step 4: Manual smoke test**

Run: `./build/qml/examples/forms/gui_qml/morph_forms_qml.exe` (add `$env:PATH` prefix with the Qt bin dir, or rely on the `windeployqt` post-build step from the earlier CMake work if already applied) and confirm the window opens with no crash and the forms render (they will not yet be reactive — the QML side is updated in Task 3). Close the window (or `Ctrl+C` the process) once confirmed.

- [ ] **Step 5: Commit**

```bash
git add examples/forms/gui_qml/FormsController.hpp examples/forms/gui_qml/FormsController.cpp
git commit -m "$(cat <<'EOF'
refactor: FormsController dispatches through Bridge/BridgeHandler

Replaces hand-built wire::Envelope + RemoteServer plumbing with the
framework's real client API, matching how examples/bank's GUI talks to
its models. submit() is renamed submitIfValid() and now goes through
BridgeHandler::executeJson (added in the previous commit) instead of a
manually assembled envelope.
EOF
)"
```

---

### Task 3: Make `DynamicForm.qml` reactive (remove the submit button)

**Files:**
- Modify: `examples/forms/gui_qml/qml/DynamicForm.qml`

**Interfaces:**
- Consumes: `FormsController::submitIfValid(actionType, bodyJson)` (Task 2), existing `replyReceived`/`optionsReceived` signals (unchanged shape).
- Produces: no new public interface — this is a leaf QML component change.

- [ ] **Step 1: Change `revalidate()` to call `submitIfValid` when the form is ready**

In `examples/forms/gui_qml/qml/DynamicForm.qml`, find the `revalidate()` function (currently lines 180-227). At the end, where it currently sets:

```qml
        ready = ok
        previewLine = ok ? "{" + parts.join(",") + "}" : ""
```

replace with:

```qml
        ready = ok
        previewLine = ok ? "{" + parts.join(",") + "}" : ""
        if (ready && form.controller)
            form.controller.submitIfValid(form.actionType, form.previewLine)
```

- [ ] **Step 2: Remove the submit button from the reactive path**

Find the `Button` element (currently lines 384-389):

```qml
        Button {
            Layout.topMargin: 8
            text: "execute"
            enabled: form.ready
            onClicked: form.controller.submit(form.actionType, form.previewLine)
        }
```

Replace it with a plain status label (keeps the layout stable, communicates the new reactive behavior instead of a dead control):

```qml
        Label {
            Layout.topMargin: 8
            text: form.ready ? "✓ executes automatically as you type" : "fill the required (*) fields"
            opacity: 0.6
            font.italic: true
        }
```

- [ ] **Step 3: Build**

Run: `cmake --build build/qml --target morph_forms_qml`
Expected: builds cleanly (QML changes do not require a C++ recompile, but re-running the build re-runs `qt_add_qml_module`'s QML resource packaging so the updated `.qml` file is picked up).

- [ ] **Step 4: Manual smoke test — verify reactivity**

Run: `./build/qml/examples/forms/gui_qml/morph_forms_qml.exe`. In the `ComputeDryDensity` form, type a value into `massDry`, then `volume`. Confirm:
- No "execute" button is present; the italic status label toggles from "fill the required fields" to "executes automatically as you type" once both fields have valid values.
- `resultText` updates automatically (no click needed) once both required fields are filled.
- Editing `volume` again after a result already appeared re-fires and updates `resultText` again without any click.

Close the app once confirmed.

- [ ] **Step 5: Commit**

```bash
git add examples/forms/gui_qml/qml/DynamicForm.qml
git commit -m "$(cat <<'EOF'
feat: fire forms automatically on valid edit, remove submit button

DynamicForm.qml now calls controller.submitIfValid() directly from
revalidate() whenever the assembled body passes client-side checks,
instead of waiting for a button click. Matches the live-recompute UX
the framework's fielded-action API models, without requiring a
compile-time set<&Action::field> hook per field.
EOF
)"
```

---

### Task 4: Qt Quick Test coverage for the reactive path

**Files:**
- Modify: `examples/forms/gui_qml/tests/tst_main.cpp` (check its current structure first — it likely just registers the test directory; see Step 1)
- Create: `examples/forms/gui_qml/tests/tst_DynamicFormReactive.qml`

**Interfaces:**
- Consumes: `DynamicForm.qml` (Task 3), a mock controller object exposing `submitIfValid`/`replyReceived`/`fetchOptions`/`optionsReceived` with the same signatures as `FormsController` (Qt Quick Test convention: a small QML mock, not the real C++ `FormsController`, so the test does not need a real `Bridge`/`LabModel` wired up).

- [ ] **Step 1: Read the existing Quick Test setup**

Run: `cat examples/forms/gui_qml/tests/tst_main.cpp` and list `examples/forms/gui_qml/tests/` to see what `.qml` test files already exist and what naming convention they use (Qt Quick Test auto-discovers `tst_*.qml` files in the directory passed via `-input` — confirmed by `gui_qml/CMakeLists.txt:42-45`: `add_test(... COMMAND morph_forms_qml_tests -input ${CMAKE_CURRENT_SOURCE_DIR}/tests)`).

- [ ] **Step 2: Write the failing QML test**

Create `examples/forms/gui_qml/tests/tst_DynamicFormReactive.qml` (adjust the mock controller's property/signal shape to match whatever pattern the existing test files in this directory already use, once Step 1's inspection is done — if this is the first QML test file in the suite, use the shape below):

```qml
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormReactive"

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property int submitCount: 0
        property string lastActionType: ""
        property string lastBodyJson: ""

        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            lastActionType = actionType
            lastBodyJson = bodyJson
            replyReceived(actionType, true, JSON.stringify({sum: 42}))
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    property var testSchema: ({
        properties: {
            a: { type: "integer", "x-order": 0 },
            b: { type: "integer", "x-order": 1 }
        },
        required: ["a", "b"]
    })

    Component {
        id: formComponent
        DynamicForm {
            actionType: "TestAction"
            schema: testCase.testSchema
            controller: mockController
        }
    }

    function test_fires_automatically_without_button() {
        mockController.submitCount = 0
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        // Locate the two TextField inputs in declaration order and type
        // into them — DynamicForm has no button in the reactive path.
        var fields = []
        function collect(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i]
                if (child.hasOwnProperty("text") && child.toString().indexOf("TextField") !== -1)
                    fields.push(child)
                collect(child)
            }
        }
        collect(form)
        compare(fields.length, 2)

        fields[0].text = "3"
        fields[1].text = "4"

        compare(mockController.submitCount, 1)
        compare(mockController.lastActionType, "TestAction")
        compare(form.resultText !== "", true)
    }

    function test_does_not_fire_when_incomplete() {
        mockController.submitCount = 0
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        var fields = []
        function collect(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i]
                if (child.hasOwnProperty("text") && child.toString().indexOf("TextField") !== -1)
                    fields.push(child)
                collect(child)
            }
        }
        collect(form)

        fields[0].text = "3"
        compare(mockController.submitCount, 0)
    }
}
```

If the DOM-walk-based field lookup (`collect()`) proves brittle against `DynamicForm`'s actual visual tree (Qt Quick Test's `findChild`-by-objectName is more standard), add `objectName: "field_" + fieldColumn.modelData.name` to the `TextField` in `qml/DynamicForm.qml` (inside the `Repeater`'s delegate, on the `TextField id: entry` element, currently around line 341-351) and use `findChild(form, "field_a")`/`findChild(form, "field_b")` in the test instead — this is the more idiomatic Qt Quick Test approach and should be preferred; only fall back to the manual `collect()` walk if `findChild` is unavailable in the project's Qt version. Prefer adding the `objectName` and using `findChild`.

Revised field-lookup approach (use this instead of `collect()`):

In `qml/DynamicForm.qml`, on the `TextField` (currently starting at line 341):

```qml
                    TextField {
                        id: entry
                        objectName: "field_" + fieldColumn.modelData.name
                        visible: !fieldColumn.modelData.isChoice && !fieldColumn.modelData.isDateTime
                        ...
```

And in the test, replace the `collect()`-based lookup with:

```qml
    function test_fires_automatically_without_button() {
        mockController.submitCount = 0
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        var fieldA = findChild(form, "field_a")
        var fieldB = findChild(form, "field_b")
        verify(fieldA !== null)
        verify(fieldB !== null)

        fieldA.text = "3"
        fieldB.text = "4"

        compare(mockController.submitCount, 1)
        compare(mockController.lastActionType, "TestAction")
        compare(form.resultText !== "", true)
    }

    function test_does_not_fire_when_incomplete() {
        mockController.submitCount = 0
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        var fieldA = findChild(form, "field_a")
        verify(fieldA !== null)
        fieldA.text = "3"
        compare(mockController.submitCount, 0)
    }
```

- [ ] **Step 3: Add the `objectName` to `DynamicForm.qml`**

Apply the `objectName: "field_" + fieldColumn.modelData.name` addition to the `TextField` shown in Step 2 above.

- [ ] **Step 4: Run the test to verify it fails first (TDD check), then passes**

Run: `cmake --build build/qml --target morph_forms_qml_tests`
Then: `ctest --test-dir build/qml -R forms_qml_logic --output-on-failure`

Expected on first run (before Step 3's `objectName` addition is in place, if done out of order): `findChild` returns `null`, test fails on `verify(fieldA !== null)`. After Step 3 is applied: both tests PASS.

- [ ] **Step 5: Run the full forms QML test suite for regressions**

Run: `ctest --test-dir build/qml -R forms --output-on-failure`
Expected: `forms_qml_logic` and `forms_html_math` (if `node` is on `PATH`) both PASS — confirms the `objectName` addition and `revalidate()` change did not break any existing QML logic test.

- [ ] **Step 6: Commit**

```bash
git add examples/forms/gui_qml/qml/DynamicForm.qml examples/forms/gui_qml/tests/tst_DynamicFormReactive.qml
git commit -m "$(cat <<'EOF'
test: cover DynamicForm's auto-fire-on-valid behavior

Adds a Qt Quick Test driving DynamicForm through simulated field edits
against a mock controller, asserting submitIfValid fires exactly once
the form becomes valid and not before — without any button click.
EOF
)"
```

---

### Task 5: Update the forms demo README

**Files:**
- Modify: `examples/forms/README.md`

**Interfaces:** None — documentation only.

- [ ] **Step 1: Read the current README**

Run: `cat examples/forms/README.md` to see its current description of the GUI's architecture (it likely still describes the `RemoteServer`/wire-envelope approach, matching the old `FormsController.hpp` file-header comment that said "wraps them in `morph::wire::Envelope`s and feeds them to an in-process `RemoteServer`").

- [ ] **Step 2: Update the architecture description**

Find and replace any prose describing `FormsController` as wrapping actions in `wire::Envelope`s / talking to `RemoteServer` with a description matching the new design: `FormsController` owns a `Bridge` + `BridgeHandler<LabModel>` and dispatches via the generic `executeJson` path, exactly like `examples/bank`'s GUI controllers. Also add a short note that forms execute automatically once valid (no submit button), replacing any mention of a submit/execute button in the QML section.

Since the exact current wording is only knowable by reading the file first (Step 1), the edit itself is: locate the paragraph(s) describing the GUI's transport and button-driven flow, and rewrite them to state:

> The Qt Quick renderer (`gui_qml/`) dispatches actions through `morph::bridge::Bridge` + `BridgeHandler<LabModel>` — the same client API `examples/bank`'s GUI uses — via a generic, JSON-in/JSON-out `executeJson` path (see `BridgeHandler::executeJson` in `morph/bridge.hpp`). Forms have no submit button: each form fires automatically the moment its assembled body passes client-side validation, and re-fires on every subsequent edit. Because each keystroke can trigger a fresh dispatch with no coalescing, rapid edits may produce out-of-order replies for the same action (last-arrival wins on the displayed result) — acceptable for this demo's read-mostly compute actions, but worth knowing before reusing this pattern for actions with side effects.

- [ ] **Step 3: Commit**

```bash
git add examples/forms/README.md
git commit -m "$(cat <<'EOF'
docs: describe the Bridge/BridgeHandler-based reactive forms flow

Updates the forms demo README to match FormsController's new transport
(Bridge/BridgeHandler via executeJson) and UX (auto-fire on valid edit,
no submit button), replacing the outdated RemoteServer/wire::Envelope
description.
EOF
)"
```

---

### Task 6: Full-suite regression pass and manual end-to-end verification

**Files:** None modified — verification only.

**Interfaces:** None — this task consumes everything built in Tasks 1-5.

- [ ] **Step 1: Full native test suite**

Run: `ctest --test-dir build/cl-debug --output-on-failure`
Expected: 100% pass (same as the baseline before this plan, plus the 3 new cases from Task 1).

- [ ] **Step 2: Full Qt-enabled test suite (WebSocket backend, unrelated to this change but shares the `Bridge`/`bridge.hpp` header)**

Run: `ctest --test-dir build/cl-qt-debug --output-on-failure`
Expected: same pass rate as the pre-existing baseline (365/366, with the one known pre-existing failure being an unrelated shell/em-dash encoding issue in a test filter name — see prior session notes). This confirms the `bridge.hpp` changes from Task 1 do not regress the Qt WebSocket backend, which also instantiates `BridgeHandler`.

- [ ] **Step 3: Full QML forms test suite**

Run: `ctest --test-dir build/qml -R forms --output-on-failure`
Expected: 100% pass, including the new `test_fires_automatically_without_button`/`test_does_not_fire_when_incomplete` cases from Task 4.

- [ ] **Step 4: Manual end-to-end smoke test of the real app**

Run: `./build/qml/examples/forms/gui_qml/morph_forms_qml.exe`. Walk through:
1. `ComputeDryDensity`: type `massDry` and `volume` values; confirm density computes and displays automatically, no click.
2. `RecordMeasurement`: confirm the `sampleId` combo box populates from `ListSamples` (via `fetchOptions`, unchanged path); fill `sampleId`, `measuredAt` (use the "now" button), and `density`; confirm it fires automatically once all required fields are set and `moisture`/`note` remain optional.
3. Trigger a validation error deliberately (e.g. if possible, clear a required field after a successful fire) and confirm the app does not crash and `resultText`/error display behaves sensibly.

Close the app once all three are confirmed.

- [ ] **Step 5: Report completion**

No commit for this task (verification only) — if all steps pass, the branch is ready for the `superpowers:requesting-code-review` or `superpowers:finishing-a-development-branch` skill, at the user's discretion.

---

### Self-Review Notes

- **Spec coverage:** Goals — "Bridge/BridgeHandler only" (Tasks 2-3), "fires automatically, no submit button" (Task 3), "zero new boilerplate per action" (Task 1's registration lives entirely inside `BRIDGE_REGISTER_ACTION_4`; `lab_model.hpp` is never touched in this plan), "`main.cpp` untouched" (no task modifies it) — all covered. Non-goals (`schemaJson` shape, `execute()`'s existing API, options-fetch reactivity, `--emit-html`) — none touched by any task. Testing section's three items (unit test for the registry, QML Quick Test, existing CTest wiring) map to Tasks 1 and 4 respectively.
- **Placeholder scan:** The one placeholder-like moment (Task 2 Step 2's first draft calling a nonexistent `lab::ActionTraits_SampleList_toJson`) is deliberately shown and then corrected within the same step with real code (`glz::write_json`), not left as a TBD — kept it in to document *why* the naive approach doesn't work (no `BRIDGE_REGISTER_ACTION` for `SampleList` itself), matching how the spec's own investigation-and-rejection of `set<>` was written up.
- **Type consistency:** `executeJson` signature (`std::string_view actionType, std::string_view bodyJson) -> Completion<std::string>`) is identical across Task 1 Step 5 (definition) and Task 2 Step 2 (call site). `submitIfValid` signature matches between Task 2 Step 1 (`.hpp` declaration) and Step 2 (`.cpp` definition) and Task 3 Step 1 (QML call site) and Task 4's mock (test double). `ActionExecuteRegistry::instance()`/`execute()`/`registerAction<Model,Action>()` names match between Task 1 Step 4 (class body) and Step 5/6 (call sites).
