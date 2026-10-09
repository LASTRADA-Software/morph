# Model-free dispatch seams Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A client compiled with no model or action type can bind a model by its type id, dispatch an action by
its id with a JSON body, and attach a shared instance by a string key — with exactly the session, deadline,
cancellation, in-flight accounting and `switchBackend` behaviour a typed `BridgeHandler` gets.

**Architecture:** The seams reuse `Bridge`'s typed machinery instead of copying it. `executeRaw` is `executeVia`
with `R = std::string` and a different `ActionCall`. The call's action object owns the ids and the JSON body. On a
remote backend, `serializeAction` returns the body and `deserializeResult` keeps the reply text. On `LocalBackend`,
`localOpAsync` runs the server's own JSON runner, `ActionDispatcher::dispatchAsync`, on the holder; that runner
already recomputes, validates and journals. `bindByType` builds a `HandlerBinding` whose factory resolves the type
id in `ModelRegistryFactory`, and adopts it through the same `adoptHandler` path a typed handler uses. A
`RawHandler` RAII class wraps the three, as `BridgeHandler` wraps the typed calls.

**Tech Stack:** C++23, header-only `include/morph/core/bridge.hpp`, Catch2 (`tests/`), Doxygen with
`WARN_AS_ERROR`.

**Spec:** `docs/superpowers/specs/2026-10-07-ui-document-design.md` §2 "Bridge seams this design adds", and its §14
"Bridge seams" test line. Issue: #886. Read `docs/spec/core/bridge.md` (the current bridge design) before Task 1.

## Global Constraints

- The seams go **through** `Bridge`'s existing paths (`makeSink`, `armDeadline`, `dispatchNow`'s hold/abandon
  logic, `startBind`, `deregisterHandler`, `switchBackend`). A second copy of any of them is a review failure.
- A raw call does **not** run the client-side `recomputeAll` or `ActionValidator`: they need the action type
  (spec 5 §2). On `LocalBackend` the dispatcher's runner runs them, as the server does for a remote call.
- The ids a raw call dispatches with are **owned by the call** (`ActionCall::modelTypeId`/`actionTypeId` are
  `string_view`s that must outlive the dispatch).
- A raw call's result is **not** published to typed subscriptions: it is JSON text, not the typed result a
  subscriber to `std::string` expects.
- AGENTS.md: comments state what the code does now and why — no history, no issue numbers. Every public symbol has
  complete `@param`/`@tparam`/`@return` (Doxygen runs with `WARN_AS_ERROR = FAIL_ON_WARNINGS`).
- CONTRIBUTING: strict warnings, clang-tidy-diff clean, a CHANGELOG `[Unreleased]` entry for a library change.
- One local build at a time; `ninja` already saturates the cores.

## Review Focus

1. **A typed subscriber on an instance a raw handler also drives.** A `BridgeHandler<M>::subscribe<std::string>`
   on the same shared instance must not receive a raw call's JSON text. Pinned in Task 2.
2. **An unknown type id or action id.** Local: the factory throws `unknown model type: X`, so the bind fails and
   the held call is rejected with that error; an unknown action rejects with `unknown action: M/A`. Remote: the
   server's `register failed: unknown model type` and `unknown action` replies. Neither throws out of `execute`.
   Pinned in Task 1.
3. **A shared raw binding that was never attached.** `execute` before `attach` rejects with the not-bound error;
   it does not dispatch to an anonymous instance. Pinned in Task 3.
4. **A raw handler destroyed while its bind is in flight.** Held calls reject with `HandlerDestroyedError`, and a
   bridge destroyed first leaves the handler's destructor a no-op. Pinned in Task 4.
5. **Malformed body JSON.** Rejected through the completion with the parser's message, never thrown. Pinned in
   Task 1.

Known limit, stated in the doc comment rather than checked: a raw handler cannot know whether its model is keyed, so
`BindSharing::Shared` over an unkeyed model is not refused by the bridge. Spec 5 §2 makes the interpreter refuse such
a document at load from the catalog's `keyed` flag. The raw path also does not auto-attach a payload-keyed action
or promote a result-keyed one; keys come from explicit `attach` calls (spec 5 §5, `instance`).

## Files

- Modify: `include/morph/core/bridge.hpp`: `BindSharing`, `detail::RawAction`, `Bridge::bindByType`,
  `Bridge::executeRaw` (two overloads), `Bridge::attach`, `BridgeSink`'s publish opt-out, `RawHandler`.
- Create: `tests/raw_dispatch_models.cpp`, the **only** translation unit that names the fixture model types.
- Create: `tests/raw_dispatch_probe.hpp`, the counters and scheduler scope for the Task-handler fixture. It names
  no model type.
- Create: `tests/test_bridge_raw.cpp`, every raw-seam test. It includes no model header, which is the point of
  the test.
- Modify: `tests/CMakeLists.txt`: add both `.cpp` files to `morph_tests`.
- Modify: `docs/spec/core/bridge.md`: a "Model-free dispatch" section and API-reference rows.
- Modify: `CHANGELOG.md`: an `### Added` entry.
- Modify: `docs/superpowers/specs/2026-10-07-ui-document-design.md` §2: name `RawHandler`.

---

### Task 0: Branch and baseline

- [ ] **Step 1: Confirm the branch and configure**

```bash
git switch feature/ui-bridge-seams
cmake -S . -B build/reactive -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF
```

Expected: the configure output contains `morph: warnings: ... strict=ON`. If it does not, stop and say so.

- [ ] **Step 2: Baseline the bridge tests**

```bash
cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[bridge]"
```

Expected: all pass. Record the test count; Task 5 compares against it.

---

### Task 1: Private raw handler — bindByType, executeRaw, local and remote

**Files:**
- Modify: `include/morph/core/bridge.hpp`
- Create: `tests/raw_dispatch_models.cpp`, `tests/raw_dispatch_probe.hpp`, `tests/test_bridge_raw.cpp`
- Modify: `tests/CMakeLists.txt` (the `morph_tests` source list, next to `test_bridge_execute_json.cpp`)

**Interfaces:**
- Produces (namespace `morph::bridge`):
  - `enum class BindSharing : std::uint8_t { Private, Shared };`
  - `std::shared_ptr<detail::HandlerBinding> Bridge::bindByType(std::string typeId, BindSharing sharing, std::string instanceKey = {});`
  - `::morph::async::Completion<std::string> Bridge::executeRaw(const std::shared_ptr<detail::HandlerBinding>& binding, std::string actionId, std::string bodyJson, ::morph::exec::IExecutor* cbExec);`
  - `class RawHandler { RawHandler(Bridge&, IExecutor* cbExec, std::string typeId, BindSharing = BindSharing::Private, std::string instanceKey = {}); Completion<std::string> execute(std::string actionId, std::string bodyJson); bool isBound() const noexcept; const std::string& typeId() const noexcept; };`
  - `namespace detail { struct RawAction { std::string modelTypeId; std::string actionTypeId; std::string body; }; }`

- [ ] **Step 1: Write the fixture models (the only TU naming them)**

`tests/raw_dispatch_models.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The models the raw-dispatch tests drive. This is the only translation unit
// that names them: test_bridge_raw.cpp reaches them by type id alone, which is
// what a client compiled without them does.

#include <chrono>
#include <cstdint>
#include <morph/async/delay.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>

#include "raw_dispatch_probe.hpp"

// NOLINTBEGIN(misc-use-internal-linkage)
struct RawAdd {
    std::int64_t by = 0;
};
struct RawRename {
    std::string name;
};
struct RawFail {};
struct RawLoad {
    std::int64_t id = 0;
};
struct RawRequestId {};
struct RawSleep {
    int ms = 0;
};

struct RawCounter {
    std::int64_t total = 0;

    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    std::int64_t execute(const RawAdd& action) {
        total += action.by;
        return total;
    }
    std::string execute(const RawRename& action) { return "renamed to " + action.name; }
    std::int64_t execute(const RawFail& /*action*/) { throw std::runtime_error{"counter refused"}; }
    std::int64_t execute(const RawLoad& action) {
        total = action.id;
        return total;
    }
    std::string execute(const RawRequestId& /*action*/) {
        auto const* session = morph::session::current();
        return session != nullptr ? session->requestId : std::string{"<none>"};
    }
    core::async::Task<int> execute(RawSleep action) {
        rawprobe::sleeper().started.fetch_add(1);
        try {
            co_await morph::async::delay(*rawprobe::sleeper().scheduler, std::chrono::milliseconds{action.ms});
        } catch (const core::async::OperationCancelled&) {
            rawprobe::sleeper().cancelled.fetch_add(1);
            throw;
        }
        rawprobe::sleeper().finished.fetch_add(1);
        co_return action.ms;
    }
    // NOLINTEND(readability-convert-member-functions-to-static)
};

BRIDGE_REGISTER_MODEL(RawCounter, "Raw_Counter")
BRIDGE_REGISTER_ACTION(RawCounter, RawAdd, "Raw_Add")
BRIDGE_REGISTER_ACTION(RawCounter, RawRename, "Raw_Rename")
BRIDGE_REGISTER_ACTION(RawCounter, RawFail, "Raw_Fail")
BRIDGE_REGISTER_ACTION(RawCounter, RawLoad, "Raw_Load")
BRIDGE_REGISTER_ACTION(RawCounter, RawRequestId, "Raw_RequestId")
BRIDGE_REGISTER_ACTION(RawCounter, RawSleep, "Raw_Sleep")
BRIDGE_MODEL_KEY(RawCounter, RawLoad, &RawLoad::id);

namespace rawprobe {
Sleeper& sleeper() {
    static Sleeper instance;
    return instance;
}
}  // namespace rawprobe
// NOLINTEND(misc-use-internal-linkage)
```

Check the include for `morph::async::delay` against `tests/test_cancellation_policy.cpp`'s includes and use the
same header it does.

`tests/raw_dispatch_probe.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// What test_bridge_raw.cpp observes of the Task-handler fixture, without
// naming any model type.
#pragma once

#include <atomic>
#include <morph/core/completion.hpp>

namespace rawprobe {

struct Sleeper {
    std::atomic<int> started{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> finished{0};
    morph::async::detail::TimeoutScheduler* scheduler = nullptr;
};

Sleeper& sleeper();

/// Owns the scheduler `Raw_Sleep` waits on for one test.
class SleeperScope {
public:
    SleeperScope() {
        sleeper().started = 0;
        sleeper().cancelled = 0;
        sleeper().finished = 0;
        sleeper().scheduler = &_scheduler;
    }
    SleeperScope(const SleeperScope&) = delete;
    SleeperScope& operator=(const SleeperScope&) = delete;
    SleeperScope(SleeperScope&&) = delete;
    SleeperScope& operator=(SleeperScope&&) = delete;
    ~SleeperScope() { sleeper().scheduler = nullptr; }

private:
    morph::async::detail::TimeoutScheduler _scheduler;
};

}  // namespace rawprobe
```

Take the `TimeoutScheduler` include from wherever `test_cancellation_policy.cpp` gets it.

- [ ] **Step 2: Write the failing tests**

`tests/test_bridge_raw.cpp`. Its includes are the guarantee under test: no fixture model header, no
`BRIDGE_REGISTER_*`.

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The model-free dispatch seams, driven by type id and action id only. This
// translation unit names no model or action type; the models live in
// raw_dispatch_models.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <exception>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "raw_dispatch_probe.hpp"
#include "test_support.hpp"

using morph::bridge::BindSharing;
using morph::bridge::RawHandler;

namespace {

enum class Mode : std::uint8_t { Local, Remote };

/// A bridge over LocalBackend or over SimulatedRemoteBackend and its server.
struct RawRig {
    explicit RawRig(Mode mode) {
        if (mode == Mode::Local) {
            bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(pool),
                                                             owner);
        } else {
            server = std::make_shared<morph::backend::RemoteServer>(serverPool);
            bridge = std::make_unique<morph::bridge::Bridge>(
                std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), owner);
        }
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor serverPool{2};
    morph::exec::MainThreadExecutor owner;
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::unique_ptr<morph::bridge::Bridge> bridge;
};

struct Reply {
    std::optional<std::string> value;
    std::exception_ptr error;
    [[nodiscard]] bool settled() const { return value.has_value() || error != nullptr; }
};

Reply await(RawRig& rig, morph::async::Completion<std::string> completion) {
    Reply reply;
    completion.then([&](std::string json) { reply.value = std::move(json); })
        .onError([&](const std::exception_ptr& err) { reply.error = err; });
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return reply.settled(); }));
    return reply;
}

std::string message(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& exc) {
        return exc.what();
    }
}

}  // namespace

TEST_CASE("RawHandler: dispatches by id and keeps the instance's state", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    CHECK(await(rig, handler.execute("Raw_Add", R"({"by":5})")).value == "5");
    CHECK(await(rig, handler.execute("Raw_Add", R"({"by":2})")).value == "7");
    CHECK(await(rig, handler.execute("Raw_Rename", R"({"name":"lab"})")).value == R"("renamed to lab")");
}

TEST_CASE("RawHandler: a model's refusal, an unknown action and a malformed body reject the call",
          "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    auto refused = await(rig, handler.execute("Raw_Fail", "{}"));
    REQUIRE(refused.error);
    CHECK(message(refused.error).find("counter refused") != std::string::npos);

    auto unknown = await(rig, handler.execute("Raw_Nope", "{}"));
    REQUIRE(unknown.error);
    CHECK(message(unknown.error).find("Raw_Counter/Raw_Nope") != std::string::npos);

    auto malformed = await(rig, handler.execute("Raw_Add", R"({"by":"x"})"));
    REQUIRE(malformed.error);
}

TEST_CASE("RawHandler: an unknown model type fails the bind and rejects the call", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Missing"};

    auto reply = await(rig, handler.execute("Raw_Add", R"({"by":1})"));
    REQUIRE(reply.error);
    CHECK(message(reply.error).find("Raw_Missing") != std::string::npos);
    CHECK_FALSE(handler.isBound());
}

TEST_CASE("RawHandler: the ids it dispatches with are its own", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, std::string{"Raw_"} + "Counter"};
    morph::async::Completion<std::string> pending;
    {
        std::string action = std::string{"Raw_"} + "Add";
        std::string body = R"({"by":3})";
        pending = handler.execute(action, body);
        action.assign(action.size(), '#');  // the caller's buffers die before the dispatch settles
        body.assign(body.size(), '#');
    }
    CHECK(await(rig, std::move(pending)).value == "3");
}
```

Register both files in `tests/CMakeLists.txt` after `test_bridge_execute_json.cpp`:

```cmake
    test_bridge_raw.cpp
    raw_dispatch_models.cpp
```

- [ ] **Step 3: Run to confirm the failure**

```bash
cmake --build build/reactive --target morph_tests
```

Expected: compile error, `no member named 'RawHandler' in namespace 'morph::bridge'`.

- [ ] **Step 4: Implement**

In `include/morph/core/bridge.hpp`:

(a) Next to `NoSharing`/`AllowShared`, declare the sharing choice. It is an enum, not a bool, so a call site reads
as what it means:

```cpp
/// @brief Whether a binding made by type id joins the shared instance directory.
enum class BindSharing : std::uint8_t {
    /// A private instance, created when the binding is bound.
    Private,
    /// A shared instance, created or joined when the binding is attached to a key.
    Shared,
};
```

(b) In `namespace detail`, the raw call's action. `ActionCall`'s id views point into it, and `ActionCall::action`
owns it, so the ids live exactly as long as the call:

```cpp
/// @brief The action of a call dispatched by id: both ids and the JSON body.
///
/// Owned by the call's `ActionCall::action`; the call's `modelTypeId` and
/// `actionTypeId` views point into it, so they outlive the dispatch however
/// short-lived the caller's strings were.
struct RawAction {
    /// @brief The model's registered type id.
    std::string modelTypeId;
    /// @brief The action's registered type id.
    std::string actionTypeId;
    /// @brief The action's JSON body.
    std::string body;
};
```

(c) `BridgeSink` gains a publish policy. Add a constructor parameter and gate the subscription fan-out on it. Use an
enum, not a bool:

```cpp
/// @brief Whether a settled result is offered to the typed subscriptions.
enum class Publish : std::uint8_t {
    /// A typed call: the result is `R` and subscribers to `R` receive it.
    Typed,
    /// A raw call: the result is JSON text, which no typed subscriber expects.
    No,
};
```

In `BridgeSink`: add `Publish publish` as the last constructor parameter, defaulting to `Publish::Typed`, and store
it as `_publish`. In `settleValue`, change `bridgeWork = bridgeWork || _subscriptions->hasSubscribers();` to
`bridgeWork = bridgeWork || (_publish == Publish::Typed && _subscriptions->hasSubscribers());`. In `forward`, change
`if (_subscriptions->hasSubscribers())` to `if (_publish == Publish::Typed && _subscriptions->hasSubscribers())`.
Document the parameter.

(d) In `Bridge`, private: the binding builder and the call builder.

```cpp
    /// @brief Builds a binding for the model registered as @p typeId, not yet
    ///        tracked or bound. Its factory resolves @p typeId in the process
    ///        registry when a local backend binds it; a remote backend sends
    ///        the id and ignores the factory.
    /// @param typeId The model's registered type id.
    /// @param shared Whether the binding joins the shared directory.
    /// @return The new binding.
    static std::shared_ptr<detail::HandlerBinding> makeBindingByType(std::string typeId, bool shared) {
        auto binding = std::make_shared<detail::HandlerBinding>();
        binding->modelFactory = [typeId] { return ::morph::model::detail::defaultRegistry().create(typeId); };
        binding->typeId = std::move(typeId);
        binding->shared = shared;
        return binding;
    }

    /// @brief Builds the `ActionCall` for one raw dispatch.
    /// @param action     The ids and body, owned by the call.
    /// @param stopSource The call's stop source; a Task handler on a local
    ///                   backend observes it.
    /// @return The call, without its session.
    static ::morph::backend::detail::ActionCall makeRawCall(std::shared_ptr<detail::RawAction> action,
                                                            std::shared_ptr<::core::async::StopSource> stopSource) {
        ::morph::backend::detail::ActionCall call;
        call.modelTypeId = action->modelTypeId;
        call.actionTypeId = action->actionTypeId;
        call.serializeAction = [](const void* actionPtr) {
            return static_cast<const detail::RawAction*>(actionPtr)->body;
        };
        call.deserializeResult = [](std::string_view json) -> std::shared_ptr<void> {
            return std::make_shared<std::string>(json);
        };
        // `dispatchAsync` runs an ordinary handler at once and a Task handler
        // to completion, so one path serves both; the dispatcher's runner does
        // the recompute, validation and journaling the server does.
        call.localOpAsync = [](::morph::model::detail::IModelHolder& holder, std::shared_ptr<void> actionPtr,
                               const std::shared_ptr<::morph::exec::detail::TaskResumer>& resumer,
                               ::core::async::StopToken token,
                               ::morph::backend::detail::ActionCall::LocalDone done) {
            auto const& raw = *static_cast<const detail::RawAction*>(actionPtr.get());
            ::morph::model::detail::defaultDispatcher().dispatchAsync(
                raw.modelTypeId, raw.actionTypeId, holder, raw.body, resumer, std::move(token),
                [done = std::move(done), keep = std::move(actionPtr)](std::string result, std::exception_ptr err) {
                    if (err) {
                        done(nullptr, err);
                        return;
                    }
                    done(std::make_shared<std::string>(std::move(result)), nullptr);
                });
        };
        call.stopSource = std::move(stopSource);
        call.action = std::move(action);
        return call;
    }
```

Check the exact namespaces of `defaultRegistry`/`defaultDispatcher` in `registry.hpp` (`morph::model::detail` or
`morph::model`) and qualify accordingly. Check how `LocalBackend` passes the action to `localOpAsync` (it takes
`ActionCall::action` by value) and keep `keep` only if the backend does not already hold the action for the
handler's lifetime. Under `MORPH_CLIENT_ONLY` the dispatcher holds no runners, so a local raw call rejects with
`unknown action`; a client-only build must not install `LocalBackend` anyway (registry.md, "MORPH_CLIENT_ONLY").

(e) Generalise `dispatchNow` so the raw path reuses it. Today it is
`template <typename Model, typename Action> void dispatchNow(HandlerBinding&, const shared_ptr<BridgeSink<R>>&, Action, IExecutor*, bool held)`,
and it builds its call with `makeActionCall<Model, Action>`. Split it: a non-template-on-action core

```cpp
    template <typename R, typename MakeCall>
    void dispatchWith(detail::HandlerBinding& binding, const std::shared_ptr<detail::BridgeSink<R>>& sink,
                      MakeCall&& makeCall, ::morph::exec::IExecutor* cbExec, bool held);
```

holding today's body verbatim. `makeCall()` replaces the `makeActionCall<Model, Action>(std::move(action), sink->stopSource)`
line, and `dispatchNow<Model, Action>` becomes a one-line forward to it. Behaviour is unchanged; the existing
`[bridge]` tests prove it in Step 5.

(f) In `Bridge`, public:

```cpp
    /// @brief Binds the model registered as @p typeId, for a caller compiled
    ///        without the model's type.
    ///
    /// A private binding is bound at once, as `registerHandler<Model>()` binds
    /// one. A shared binding is bound when attached: here when @p instanceKey
    /// is not empty, otherwise by a later `attach`. The binding is tracked
    /// for `switchBackend()`, deregistered by `deregisterHandler`, and its
    /// instance is owned by the session's principal on a server exactly as a
    /// typed handler's is. On the owner only.
    ///
    /// The bridge cannot tell whether the model is keyed: a shared binding of
    /// an unkeyed model is refused, if at all, by the backend when attached.
    ///
    /// @param typeId      The model's registered type id.
    /// @param sharing     Private or shared.
    /// @param instanceKey For a shared binding, the key to attach to now, or
    ///                    empty to stay unattached. Must be empty for a
    ///                    private binding.
    /// @return The binding.
    /// @throws std::invalid_argument for a private binding with a key.
    std::shared_ptr<detail::HandlerBinding> bindByType(std::string typeId, BindSharing sharing,
                                                       std::string instanceKey = {}) {
        note("Bridge::bindByType");
        if (sharing == BindSharing::Private && !instanceKey.empty()) {
            throw std::invalid_argument{"Bridge::bindByType: a private binding takes no instance key"};
        }
        auto binding = makeBindingByType(std::move(typeId), sharing == BindSharing::Shared);
        adoptOnOwner(binding);
        if (!instanceKey.empty()) {
            attach(binding, std::move(instanceKey));
        }
        return binding;
    }

    /// @brief Dispatches the action registered as @p actionId with a JSON
    ///        body through @p binding, and resolves with the JSON reply.
    ///
    /// The call is stamped with the default session, raced against the
    /// execute deadline, counted in `pendingCalls()`, held while a bind is in
    /// flight, and settled by `cancelPending` exactly as a typed call. Neither
    /// the client-side recompute nor the action's validator runs here: they
    /// need the action's type, and whoever runs the model runs both.
    ///
    /// @param binding  A binding from `bindByType`.
    /// @param actionId The action's registered type id.
    /// @param bodyJson The action's JSON body.
    /// @param cbExec   Executor the `Completion` callbacks are posted on.
    /// @return Completion resolving with the reply's JSON text.
    ::morph::async::Completion<std::string> executeRaw(const std::shared_ptr<detail::HandlerBinding>& binding,
                                                       std::string actionId, std::string bodyJson,
                                                       ::morph::exec::IExecutor* cbExec) {
        MORPH_ZONE("Bridge::executeRaw");
        note("Bridge::executeVia");
        auto sink = std::make_shared<detail::BridgeSink<std::string>>(
            std::function<void(const std::string&)>{}, _pendingCalls, _subscriptions, _callbacks.token(), _affinity,
            detail::Publish::No);
        _pendingCalls->fetch_add(1, std::memory_order_relaxed);
        sink->stopSource = std::make_shared<::core::async::StopSource>();
        ::morph::async::Completion<std::string> typed{sink, cbExec};
        armDeadline(sink);
        auto action = std::make_shared<detail::RawAction>(
            detail::RawAction{.modelTypeId = binding->typeId, .actionTypeId = std::move(actionId),
                              .body = std::move(bodyJson)});
        auto makeCall = [action, stop = sink->stopSource] { return makeRawCall(action, stop); };
        if (!binding->bindInFlight) {
            dispatchWith(*binding, sink, makeCall, cbExec, /*held=*/false);
            return typed;
        }
        std::weak_ptr<detail::HandlerBinding> const weak{binding};
        binding->waiting.push_back([this, weak, sink, cbExec, makeCall](const std::exception_ptr& failure) {
            auto strong = weak.lock();
            if (failure || !strong) {
                sink->settleException(failure ? failure
                                              : std::make_exception_ptr(::morph::backend::HandlerDestroyedError{}));
                return;
            }
            dispatchWith(*strong, sink, makeCall, cbExec, /*held=*/true);
        });
        return typed;
    }
```

Before writing it, compare this body line by line with `executeVia` and `makeSink`. The only differences allowed
are the sink's `Publish::No`, the stop source being unconditional (the bridge cannot know at compile time whether
the handler is a Task), and `dispatchWith` over `makeRawCall`. If `makeSink` can take the publish policy and an
"always" stop-source flag as parameters, do that instead of the inline construction, so the two paths cannot drift.

(g) `RawHandler`, after `BridgeHandler`. It mirrors its constructor, destructor and owner checks:

```cpp
/// @brief A handler for a model known only by its registered type id.
///
/// What a client compiled without the model's C++ type holds: binds through
/// `Bridge::bindByType` on construction, dispatches through
/// `Bridge::executeRaw`, and deregisters on destruction — on the owner, with
/// every guarantee a `BridgeHandler` has. Non-copyable and non-movable.
class RawHandler {
public:
    /// @brief Binds the model registered as @p typeId.
    /// @param bridge      The bridge to bind on. Borrowed: it must outlive
    ///                    every call made on this handler.
    /// @param cbExec      Executor the `Completion` callbacks are posted on.
    ///                    Borrowed: it must outlive this handler.
    /// @param typeId      The model's registered type id.
    /// @param sharing     Private or shared.
    /// @param instanceKey For a shared handler, the key to attach to now, or empty.
    RawHandler(Bridge& bridge MORPH_LIFETIMEBOUND, ::morph::exec::IExecutor* cbExec MORPH_LIFETIMEBOUND,
               std::string typeId, BindSharing sharing = BindSharing::Private, std::string instanceKey = {})
        : _bridge{bridge},
          _liveness{bridge.liveness()},
          _affinity{bridge.affinity()},
          _cbExec{cbExec},
          _binding{bridge.bindByType(std::move(typeId), sharing, std::move(instanceKey))} {}

    /// @brief Deregisters the binding, on the owner. Calls still held for a
    ///        bind in flight are rejected with `HandlerDestroyedError`. After
    ///        the bridge is gone, does nothing.
    ~RawHandler() {
        _affinity.note("RawHandler::~RawHandler");
        if (_liveness.active()) {
            _bridge.deregisterHandler(_binding);
        }
    }

    RawHandler(const RawHandler&) = delete;
    RawHandler& operator=(const RawHandler&) = delete;
    RawHandler(RawHandler&&) = delete;
    RawHandler& operator=(RawHandler&&) = delete;

    /// @brief Dispatches the action registered as @p actionId with @p bodyJson.
    /// @param actionId The action's registered type id.
    /// @param bodyJson The action's JSON body.
    /// @return Completion resolving with the reply's JSON text.
    [[nodiscard]] ::morph::async::Completion<std::string> execute(std::string actionId, std::string bodyJson) {
        return _bridge.executeRaw(_binding, std::move(actionId), std::move(bodyJson), _cbExec);
    }

    /// @brief Whether the handler holds a live instance now.
    /// @return `true` when bound.
    [[nodiscard]] bool isBound() const noexcept { return Bridge::isBound(_binding); }

    /// @brief The model's registered type id.
    /// @return The id given at construction.
    [[nodiscard]] const std::string& typeId() const noexcept { return _binding->typeId; }

private:
    Bridge& _bridge;
    ::morph::async::CallbackToken _liveness;
    ::morph::exec::detail::OwnerAffinity _affinity;
    ::morph::exec::IExecutor* _cbExec;
    std::shared_ptr<detail::HandlerBinding> _binding;
};
```

`liveness()`, `affinity()` and `deregisterHandler` are private or friend-gated for `BridgeHandler`; add
`friend class RawHandler;` next to `friend class BridgeHandler;` in `Bridge`.

- [ ] **Step 5: Build and run**

```bash
cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[bridge]"
```

Expected: the four new cases pass in both modes, and every pre-existing `[bridge]` test still passes (the
`dispatchWith` split and the sink parameter changed no behaviour).

- [ ] **Step 6: Prove the tests measure something**

Mutate, run `"[bridge][raw]"`, confirm a failure, revert, each in turn:
1. In `makeRawCall`, make `localOpAsync` return `std::make_shared<std::string>("0")` without dispatching. Expected:
   the `5`/`7` case fails in `Local` mode.
2. In `makeRawCall`, set `call.modelTypeId` from a local `std::string` copy that dies at return. Expected: the
   "ids are its own" case fails or crashes under ASan (run it in the ASan preset if it passes in Debug).

- [ ] **Step 7: Commit**

```bash
git add include/morph/core/bridge.hpp tests/raw_dispatch_models.cpp tests/raw_dispatch_probe.hpp \
        tests/test_bridge_raw.cpp tests/CMakeLists.txt
git commit -m "core: dispatch a model by type id and an action by id with a JSON body"
```

---

### Task 2: Parity with typed calls: session, deadline, accounting, cancellation, subscriptions

**Files:**
- Modify: `include/morph/core/bridge.hpp` (`Bridge::executeRaw` stop-token overload, `RawHandler::execute(…, StopToken)`)
- Modify: `tests/test_bridge_raw.cpp`

**Interfaces:**
- Consumes: `RawHandler`, `Bridge::executeRaw` (Task 1).
- Produces: `::morph::async::Completion<std::string> RawHandler::execute(std::string actionId, std::string bodyJson, ::core::async::StopToken stop);`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_bridge_raw.cpp`. Before writing each one, open its typed counterpart (named in the comment)
and keep the same assertions; only the handler changes.

```cpp
// Typed counterpart: test_principal.cpp / test_bridge_local.cpp session cases.
TEST_CASE("RawHandler: the default session reaches the model", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    morph::session::Context session;
    session.requestId = "req-raw-1";
    rig.bridge->setDefaultSession(session);
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    CHECK(await(rig, handler.execute("Raw_RequestId", "{}")).value == R"("req-raw-1")");
}

// Typed counterpart: test_client_execute_deadline.cpp, "fires ClientTimeoutError when no reply arrives in time".
TEST_CASE("RawHandler: the execute deadline rejects a call with no reply", "[bridge][raw]") {
    rawprobe::SleeperScope const sleeping;
    RawRig rig{Mode::Local};
    rig.bridge->setExecuteDeadline(std::chrono::milliseconds{50});
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    auto reply = await(rig, handler.execute("Raw_Sleep", R"({"ms":60000})"));
    REQUIRE(reply.error);
    CHECK_THROWS_AS(std::rethrow_exception(reply.error), morph::backend::ClientTimeoutError);
    // The deadline asks the Task handler to stop, as for a typed call.
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [] { return rawprobe::sleeper().cancelled.load() == 1; }));
}

// Typed counterpart: test_bridge_pending_calls.cpp.
TEST_CASE("RawHandler: pendingCalls counts a raw call until it settles", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    REQUIRE(rig.bridge->pendingCalls() == 0);

    auto completion = handler.execute("Raw_Add", R"({"by":1})");
    CHECK(rig.bridge->pendingCalls() == 1);
    auto ok = await(rig, std::move(completion));
    CHECK(ok.value == "1");
    CHECK(rig.bridge->pendingCalls() == 0);

    auto failed = await(rig, handler.execute("Raw_Fail", "{}"));
    CHECK(failed.error);
    CHECK(rig.bridge->pendingCalls() == 0);
}

// Typed counterpart: test_cancellation_policy.cpp, "execute with a stop token is G2 when stopped...".
TEST_CASE("RawHandler: a stop token cancels the call and stops its Task handler", "[bridge][raw]") {
    rawprobe::SleeperScope const sleeping;
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    core::async::StopSource stop;  // NOLINT(misc-const-correctness): request_stop() is non-const
    Reply reply;
    handler.execute("Raw_Sleep", R"({"ms":60000})", stop.get_token())
        .then([&](std::string json) { reply.value = std::move(json); })
        .onError([&](const std::exception_ptr& err) { reply.error = err; });
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [] { return rawprobe::sleeper().started.load() == 1; }));

    static_cast<void>(stop.request_stop());
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return reply.settled(); }));
    CHECK_THROWS_AS(std::rethrow_exception(reply.error), core::async::OperationCancelled);
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [] { return rawprobe::sleeper().cancelled.load() == 1; }));
    CHECK(rawprobe::sleeper().finished.load() == 0);
}

TEST_CASE("RawHandler: a token already stopped rejects without dispatching", "[bridge][raw]") {
    rawprobe::SleeperScope const sleeping;
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    core::async::StopSource stop;  // NOLINT(misc-const-correctness)
    static_cast<void>(stop.request_stop());

    auto reply = await(rig, handler.execute("Raw_Sleep", R"({"ms":10})", stop.get_token()));
    CHECK_THROWS_AS(std::rethrow_exception(reply.error), core::async::OperationCancelled);
    CHECK(rawprobe::sleeper().started.load() == 0);
    CHECK(rig.bridge->pendingCalls() == 0);
}

// Typed counterpart: test_cancellation_policy.cpp, "Bridge::switchBackend is G2 for the outgoing backend's calls".
TEST_CASE("RawHandler: cancelPending on a switch settles an in-flight raw call", "[bridge][raw]") {
    rawprobe::SleeperScope const sleeping;
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    Reply reply;
    handler.execute("Raw_Sleep", R"({"ms":60000})")
        .then([&](std::string json) { reply.value = std::move(json); })
        .onError([&](const std::exception_ptr& err) { reply.error = err; });
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [] { return rawprobe::sleeper().started.load() == 1; }));

    rig.bridge->switchBackend(std::make_unique<morph::backend::LocalBackend>(rig.pool));
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return reply.settled(); }));
    CHECK(reply.error);
}
```

The subscription test needs a typed subscriber on the instance a raw handler drives, and this file must not name
the model type. Put the typed half in the fixture TU behind a probe. Add to `raw_dispatch_probe.hpp`, inside
`namespace rawprobe` (add `#include <memory>`, `<string>`, `<morph/core/bridge.hpp>`, `<morph/core/executor.hpp>`):

```cpp
/// A typed shared handler on one instance, subscribed to `std::string`
/// results; counts what it receives. Opaque here: the handler's type names
/// the model.
class TypedRenameWatch;

/// Attaches a typed shared handler to the instance for @p key and subscribes it.
std::shared_ptr<TypedRenameWatch> watchRenames(morph::bridge::Bridge& bridge, morph::exec::IExecutor& owner,
                                               const std::string& key);

/// How many `std::string` results @p watch has received.
int renamesSeen(const TypedRenameWatch& watch);

/// Dispatches a typed `Raw_Rename` through @p watch's own handler.
void renameTyped(TypedRenameWatch& watch);
```

and to `raw_dispatch_models.cpp`, after the registrations:

```cpp
namespace rawprobe {
class TypedRenameWatch {
public:
    TypedRenameWatch(morph::bridge::Bridge& bridge, morph::exec::IExecutor& owner, const std::string& key)
        : handler{bridge, &owner} {
        handler.attach(std::stoll(key));
        handler.subscribe<std::string>([this](const std::string&) { ++seen; });
    }
    morph::bridge::BridgeHandler<RawCounter, morph::bridge::AllowShared> handler;
    int seen = 0;
};

std::shared_ptr<TypedRenameWatch> watchRenames(morph::bridge::Bridge& bridge, morph::exec::IExecutor& owner,
                                               const std::string& key) {
    return std::make_shared<TypedRenameWatch>(bridge, owner, key);
}

int renamesSeen(const TypedRenameWatch& watch) { return watch.seen; }

void renameTyped(TypedRenameWatch& watch) { static_cast<void>(watch.handler.execute(RawRename{.name = "typed"})); }
}  // namespace rawprobe
```

Check the subscribe callback's parameter type against `BridgeHandler::subscribe<R>(std::function<void(R)>)`. The
control half of the test is a typed `Raw_Rename` through the watch's own handler, which must raise `seen` to 1:
without it, a zero proves nothing. It is `renameTyped` above. Then the test:

```cpp
TEST_CASE("RawHandler: a raw result is not published to typed subscribers", "[bridge][raw]") {
    RawRig rig{Mode::Local};
    auto watch = rawprobe::watchRenames(*rig.bridge, rig.owner, "41");
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared, "41"};

    CHECK(await(rig, handler.execute("Raw_Rename", R"({"name":"x"})")).value == R"("renamed to x")");
    rig.owner.runFor(std::chrono::milliseconds{20});
    CHECK(rawprobe::renamesSeen(*watch) == 0);

    // Control: a typed result on the same instance does reach the subscriber.
    rawprobe::renameTyped(*watch);
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return rawprobe::renamesSeen(*watch) == 1; }));
}
```

This test also needs Task 3's `attach`; write it now, and expect it to compile only after Task 3. Mark it
`[!mayfail]` until then, or move it into Task 3's commit if you prefer one passing commit per task. Either is
fine; say which in the commit body.

- [ ] **Step 2: Run to confirm the failure**

Expected: compile error on `execute(…, StopToken)`.

- [ ] **Step 3: Implement the stop-token overload**

On `Bridge`, mirroring `BridgeHandler::execute(Action, StopToken)`:

```cpp
    /// @brief `executeRaw`, cancellable through @p stop.
    ///
    /// A stop before the call settles rejects it with
    /// `core::async::OperationCancelled` and asks its stop source to stop; a
    /// Task handler on a local backend sees that at its next stop-aware
    /// `co_await`. A token already stopped rejects without dispatching.
    /// @param binding  A binding from `bindByType`.
    /// @param actionId The action's registered type id.
    /// @param bodyJson The action's JSON body.
    /// @param cbExec   Executor the `Completion` callbacks are posted on.
    /// @param stop     The caller's cancel.
    /// @return Completion resolving with the reply's JSON text, or rejected
    ///         with `core::async::OperationCancelled`.
    ::morph::async::Completion<std::string> executeRaw(const std::shared_ptr<detail::HandlerBinding>& binding,
                                                       std::string actionId, std::string bodyJson,
                                                       ::morph::exec::IExecutor* cbExec,
                                                       ::core::async::StopToken stop) {
        if (stop.stop_requested()) {
            auto [cancelled, promise] = ::morph::async::Completion<std::string>::makeSettleable(cbExec);
            promise.reject(std::make_exception_ptr(::core::async::OperationCancelled{}));
            return std::move(cancelled);
        }
        auto completion = executeRaw(binding, std::move(actionId), std::move(bodyJson), cbExec);
        completion.state()->linkCancel(std::move(stop));
        return completion;
    }
```

and on `RawHandler`:

```cpp
    /// @brief `execute`, cancellable through @p stop.
    /// @param actionId The action's registered type id.
    /// @param bodyJson The action's JSON body.
    /// @param stop     The caller's cancel.
    /// @return Completion resolving with the reply's JSON text, or rejected
    ///         with `core::async::OperationCancelled`.
    [[nodiscard]] ::morph::async::Completion<std::string> execute(std::string actionId, std::string bodyJson,
                                                                  ::core::async::StopToken stop) {
        return _bridge.executeRaw(_binding, std::move(actionId), std::move(bodyJson), _cbExec, std::move(stop));
    }
```

- [ ] **Step 4: Run**

```bash
cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[bridge][raw]"
```

Expected: all pass (the subscription case per Step 1's choice).

- [ ] **Step 5: Mutations**

1. In `executeRaw`, drop `armDeadline(sink);`. Expected: the deadline case fails.
2. In `executeRaw`, drop `_pendingCalls->fetch_add`. Expected: the pending-calls case fails.
3. Pass `detail::Publish::Typed` instead of `Publish::No`. Expected: the subscription case fails, with
   `renamesSeen == 1`.
4. In `dispatchWith`, drop `call.session = _defaultSession;`. Expected: the session case fails in both modes and
   typed session tests fail too.

Revert each.

- [ ] **Step 6: Commit**

```bash
git add include/morph/core/bridge.hpp tests/test_bridge_raw.cpp tests/raw_dispatch_probe.hpp tests/raw_dispatch_models.cpp
git commit -m "core: raw dispatch carries the session, deadline, accounting and cancellation of a typed call"
```

---

### Task 3: Shared raw handlers — string-keyed attach and backend switches

**Files:**
- Modify: `include/morph/core/bridge.hpp` (`Bridge::attach`, `RawHandler::attach`, `RawHandler::primary`)
- Modify: `tests/test_bridge_raw.cpp`

**Interfaces:**
- Consumes: `Bridge::bindByType`, `RawHandler` (Task 1).
- Produces:
  - `void Bridge::attach(const std::shared_ptr<detail::HandlerBinding>& binding, std::string instanceKey);`
  - `void RawHandler::attach(std::string instanceKey);` (throws `std::logic_error` on a private handler)
  - `[[nodiscard]] std::string RawHandler::primary() const;` (empty when unattached)

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("RawHandler: two shared handlers on one key share one instance", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler first{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared, "7"};
    RawHandler second{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared};
    second.attach("7");

    CHECK(await(rig, first.execute("Raw_Add", R"({"by":2})")).value == "2");
    CHECK(await(rig, second.execute("Raw_Add", R"({"by":3})")).value == "5");
    CHECK(second.primary() == "7");
}

TEST_CASE("RawHandler: a shared handler that was never attached rejects its calls", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared};

    auto reply = await(rig, handler.execute("Raw_Add", R"({"by":1})"));
    CHECK(reply.error);
    CHECK(handler.primary().empty());
}

TEST_CASE("RawHandler: re-attaching moves the handler to the other instance", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared, "1"};
    CHECK(await(rig, handler.execute("Raw_Add", R"({"by":10})")).value == "10");

    handler.attach("2");
    CHECK(await(rig, handler.execute("Raw_Add", R"({"by":1})")).value == "1");
    CHECK(handler.primary() == "2");
}

TEST_CASE("RawHandler: a private handler refuses attach, and bindByType refuses a private key", "[bridge][raw]") {
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    CHECK_THROWS_AS(handler.attach("1"), std::logic_error);
    CHECK_THROWS_AS(rig.bridge->bindByType("Raw_Counter", BindSharing::Private, "1"), std::invalid_argument);
}

// Typed counterpart: test_bridge_local.cpp / test_bridge_bind_paths.cpp switchBackend re-binding.
TEST_CASE("RawHandler: switchBackend re-binds private and shared raw handlers", "[bridge][raw]") {
    RawRig rig{Mode::Local};
    RawHandler priv{*rig.bridge, &rig.owner, "Raw_Counter"};
    RawHandler shared{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared, "9"};
    REQUIRE(await(rig, priv.execute("Raw_Add", R"({"by":1})")).value == "1");

    rig.bridge->switchBackend(std::make_unique<morph::backend::LocalBackend>(rig.pool));
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return priv.isBound() && shared.isBound(); }));
    // A fresh backend: fresh instances, still reachable by the same handlers.
    CHECK(await(rig, priv.execute("Raw_Add", R"({"by":4})")).value == "4");
    CHECK(await(rig, shared.execute("Raw_Add", R"({"by":6})")).value == "6");
    CHECK(shared.primary() == "9");
}
```

If the first test's `Remote` run shows `RemoteServer` refusing an attach without an authorizer, configure the rig's
server the way `test_bridge_remote.cpp`'s shared-instance cases do, and note it in the commit body.

- [ ] **Step 2: Run to confirm the failure**

Expected: compile error on `attach`/`primary`.

- [ ] **Step 3: Implement**

`Bridge::attachHandler<Model>` does not use `Model` in its body. Move the body into a non-template `attach` and make
the template forward to it, so the typed and raw attaches are one function:

```cpp
    /// @brief Attaches (or re-points) a shared @p binding to the instance for
    ///        @p instanceKey, creating it if no live instance holds the key.
    ///
    /// Idempotent for the key the binding already holds. Issued after any
    /// bind in flight; never waits — a call made meanwhile is held until it
    /// settles. A refused attach is logged and leaves the binding on the
    /// instance it held.
    /// @param binding     A shared binding.
    /// @param instanceKey Canonical string encoding of the primary key.
    void attach(const std::shared_ptr<detail::HandlerBinding>& binding, std::string instanceKey) {
        note("Bridge::attachHandler");
        std::weak_ptr<detail::HandlerBinding> const weak{binding};
        whenIdle(*binding, [this, weak, primary = std::move(instanceKey)](const std::exception_ptr& failure) {
            auto strong = weak.lock();
            if (failure || !strong || (strong->primary == primary && strong->currentId.load() != 0U)) {
                return;
            }
            runSuperseded(startBind(strong, attachRequest(*strong, primary), primary, {}));
        });
    }

    template <typename Model>
    void attachHandler(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary) {
        attach(binding, std::move(primary));
    }
```

Keep `attachHandler`'s existing doc comment (with its `@tparam`), and say in it that it is `attach` for a typed
caller.

On `RawHandler`:

```cpp
    /// @brief Attaches (or re-points) this shared handler to @p instanceKey.
    /// @param instanceKey Canonical string encoding of the primary key.
    /// @throws std::logic_error on a private handler.
    void attach(std::string instanceKey) {
        if (!_binding->shared) {
            throw std::logic_error{"RawHandler::attach: the handler is private"};
        }
        _bridge.attach(_binding, std::move(instanceKey));
    }

    /// @brief This handler's current key.
    /// @return The attached key, or empty before the first attach settles.
    [[nodiscard]] std::string primary() const { return _bridge.bindingPrimary(_binding); }
```

- [ ] **Step 4: Run**

```bash
cmake --build build/reactive --target morph_tests && ./build/reactive/tests/morph_tests "[bridge]"
```

Expected: every `[bridge]` test passes, including the subscription case from Task 2.

- [ ] **Step 5: Mutation**

In `bindByType`, drop the `attach(binding, …)` call. Expected: "two shared handlers on one key" fails. Revert.

- [ ] **Step 6: Commit**

```bash
git add include/morph/core/bridge.hpp tests/test_bridge_raw.cpp
git commit -m "core: attach a shared handler by a string key, typed or raw"
```

---

### Task 4: Lifetime — destroyed handler, destroyed bridge, held calls

**Files:**
- Modify: `tests/test_bridge_raw.cpp`
- Modify: `tests/bind_support.hpp`, `tests/test_bridge_bind_paths.cpp` (share `GateBackend`)
- Modify: `include/morph/core/bridge.hpp` only if a test fails

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Share the gate backend**

`GateBackend` (with its `Reply` enum) is file-local to `tests/test_bridge_bind_paths.cpp` (the `Reply` enum and
the class, from `/// What a hand-settled backend does…` to the end of the class). Move both, unchanged, into
`tests/bind_support.hpp` inside `namespace morph::testing`. That header already exists for bind tests. Add its
missing includes (`<morph/core/backend.hpp>`, `<optional>`, `<vector>`). In `test_bridge_bind_paths.cpp`, replace
the moved code with `using morph::testing::GateBackend;` and `using morph::testing::Reply;`. Build and run
`"[bridge][bind]"`: they must pass unchanged before going on.

- [ ] **Step 2: Write the tests**

Append to `tests/test_bridge_raw.cpp`, and add `#include "bind_support.hpp"` to its includes:

```cpp
// Typed counterpart: test_bridge_lifetime.cpp, a handler destroyed with a call held.
TEST_CASE("RawHandler: destroying the handler rejects calls held for its bind", "[bridge][raw]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto backend = std::make_unique<morph::testing::GateBackend>(pool);
    auto* gate = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};

    Reply reply;
    {
        RawHandler handler{bridge, &owner, "Raw_Counter"};
        REQUIRE(gate->heldBinds() == 1);
        handler.execute("Raw_Add", R"({"by":1})")
            .then([&](std::string json) { reply.value = std::move(json); })
            .onError([&](const std::exception_ptr& err) { reply.error = err; });
        CHECK(bridge.pendingCalls() == 1);
    }
    REQUIRE(morph::testing::pumpOwnerUntil(owner, [&] { return reply.settled(); }));
    CHECK_THROWS_AS(std::rethrow_exception(reply.error), morph::backend::HandlerDestroyedError);

    gate->resolveBind();  // the late reply finds no binding and settles nothing
    owner.runFor(std::chrono::milliseconds{20});
    CHECK_FALSE(reply.value.has_value());
    CHECK(bridge.pendingCalls() == 0);
}

// Typed counterpart: test_bridge_lifetime.cpp, a handler that outlives its bridge.
TEST_CASE("RawHandler: a handler that outlives its bridge does nothing on destruction", "[bridge][raw]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto backend = std::make_unique<morph::testing::GateBackend>(pool);
    auto* gate = backend.get();
    auto released = gate->releasedCounter();
    auto bridge = std::make_unique<morph::bridge::Bridge>(std::move(backend), owner);
    auto handler = std::make_unique<RawHandler>(*bridge, &owner, "Raw_Counter");

    Reply reply;
    handler->execute("Raw_Add", R"({"by":1})")
        .then([&](std::string json) { reply.value = std::move(json); })
        .onError([&](const std::exception_ptr& err) { reply.error = err; });

    bridge.reset();
    REQUIRE(morph::testing::pumpOwnerUntil(owner, [&] { return reply.settled(); }));
    CHECK_THROWS_AS(std::rethrow_exception(reply.error), morph::backend::BridgeDestroyedError);

    int const releasedBefore = *released;
    handler.reset();  // the bridge's token has expired: deregisters nothing
    CHECK(*released == releasedBefore);
}
```

If the second case's `releasedBefore` is not what the typed counterpart asserts for the same sequence, follow the
typed test's assertion and say so in the commit body. The raw handler must behave like the typed one, not like
this plan's guess.

- [ ] **Step 3: Run under the address sanitizer**

```bash
cmake --preset clang-asan && cmake --build build/clang-asan --target morph_tests
./build/clang-asan/tests/morph_tests "[bridge][raw]"
bash scripts/check_sanitizer_instrumentation.sh build/clang-asan asan
```

Expected: pass, and the instrumentation script confirms the binary is instrumented. A sanitizer pass on an
uninstrumented binary is not evidence (AGENTS.md, "Verify rather than assert").

- [ ] **Step 4: Commit**

```bash
git add tests/test_bridge_raw.cpp tests/bind_support.hpp tests/test_bridge_bind_paths.cpp
git commit -m "core: raw handlers keep the typed handler's lifetime guarantees"
```

---

### Task 5: Docs, CHANGELOG, spec, and the whole-branch gates

**Files:**
- Modify: `docs/spec/core/bridge.md`, `CHANGELOG.md`, `docs/superpowers/specs/2026-10-07-ui-document-design.md`

- [ ] **Step 1: `docs/spec/core/bridge.md`**

Add a section **"Model-free dispatch"** after the `BridgeHandler<Model>` section. In present tense, with no
history and no issue numbers, cover:
- What `bindByType`, `executeRaw`, `attach` and `RawHandler` are for: a client compiled without the model's
  type.
- The call shape: `RawAction` owns the ids and the body. Remote backends send the body and keep the reply text.
  `LocalBackend` runs `ActionDispatcher::dispatchAsync`, so the local path recomputes, validates and journals
  exactly as a server does, and the client does neither.
- What is shared with typed calls: the session, deadline, `pendingCalls`, cancel and stop, held-while-binding,
  `switchBackend`, deregistration. Name the functions.
- What differs, and why: no subscription publish (JSON text is not `R`); an unconditional stop source (the bridge
  cannot know whether the handler is a Task); no payload-keyed auto-attach or result-keyed promotion (no key
  extraction without the type); keyedness is not checked by the bridge.

Add API-reference rows for `BindSharing`, `Bridge::bindByType`, `Bridge::executeRaw` (both overloads),
`Bridge::attach`, and `RawHandler`, in the tables under "API reference".

- [ ] **Step 2: `CHANGELOG.md`**

Under `## [Unreleased]`, `### Added` (create the heading if missing, in Keep a Changelog order):

```markdown
- **Model-free dispatch.** `RawHandler` binds a model by its registered type
  id and dispatches actions by id with a JSON body, for a client compiled
  without the model's type. Calls carry the default session, the execute
  deadline, `pendingCalls()` accounting, cancellation and `switchBackend`
  re-binding exactly as typed calls do; a shared handler attaches by a string
  key. The underlying `Bridge::bindByType`, `Bridge::executeRaw` and
  `Bridge::attach` are public for callers that manage bindings themselves.
```

- [ ] **Step 3: Spec 5 §2**

In "Bridge seams this design adds", add one sentence after the three bullets: the seams are wrapped by
`RawHandler`, which binds on construction and deregisters on destruction as `BridgeHandler` does. Change nothing
else in the spec.

- [ ] **Step 4: Whole-branch gates**

Run each and paste the summary line into the PR body:

```bash
cmake --build build/reactive && ctest --test-dir build/reactive --output-on-failure
cmake -S . -B build/docs -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
cmake --build build/docs --target doc
git diff --name-only origin/master...HEAD -- '*.hpp' '*.cpp'   # count the files clang-tidy-diff must see
```

Run clang-tidy-diff over `origin/master...HEAD` the way `.github/workflows/ci.yml`'s clang-tidy job does, and
assert that it reports on the same number of files the `git diff` above lists. Run the `clang-tsan` preset (`build/clang-tsan`) on
`"[bridge][raw]"`, and confirm it with `bash scripts/check_sanitizer_instrumentation.sh build/clang-tsan tsan`.

Compare the `[bridge]` test count with Task 0's baseline: it must be the baseline plus the new cases, with no
pre-existing case missing.

- [ ] **Step 5: Commit**

```bash
git add docs/spec/core/bridge.md CHANGELOG.md docs/superpowers/specs/2026-10-07-ui-document-design.md
git commit -m "docs: model-free dispatch in the bridge spec, CHANGELOG, and spec 5"
```

- [ ] **Step 6: Open the PR**

Title `core: model-free dispatch — RawHandler, bindByType, executeRaw, string-keyed attach`. The body says
`Closes #886`, lists what was measured on which configuration (Debug, ASan, TSan, Docs), and quotes the mutation
results from Tasks 1–3.
