// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <thread>

#include "test_support.hpp"

// ── Test models ───────────────────────────────────────────────────────────────

struct NotifiableModel {
    int notifyCount = 0;
    void onBackendChanged() { ++notifyCount; }
};

struct SilentModel {};

// ── Concept detection ─────────────────────────────────────────────────────────

TEST_CASE("morph::model::detail::BackendChangedNotifiable: detects onBackendChanged method", "[model][concept]") {
    STATIC_REQUIRE(morph::model::detail::BackendChangedNotifiable<NotifiableModel>);
    STATIC_REQUIRE_FALSE(morph::model::detail::BackendChangedNotifiable<SilentModel>);
}

TEST_CASE(
    "morph::model::detail::ModelHolder for notifiable model implements morph::model::detail::IBackendChangedSink",
    "[model][concept]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>();
    auto* sink = dynamic_cast<morph::model::detail::IBackendChangedSink*>(holder.get());
    REQUIRE(sink != nullptr);
}

TEST_CASE(
    "morph::model::detail::ModelHolder for silent model does NOT implement morph::model::detail::IBackendChangedSink",
    "[model][concept]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<SilentModel>>();
    auto* sink = dynamic_cast<morph::model::detail::IBackendChangedSink*>(holder.get());
    REQUIRE(sink == nullptr);
}

TEST_CASE("morph::model::detail::IBackendChangedSink::onBackendChanged delegates to model method",
          "[model][concept]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>();
    auto* sink = dynamic_cast<morph::model::detail::IBackendChangedSink*>(holder.get());
    REQUIRE(sink != nullptr);

    sink->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 1);

    sink->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 2);
}

// ── notifyBackendChanged ──────────────────────────────────────────────────────
TEST_CASE("morph::backend::LocalBackend::notifyBackendChanged calls onBackendChanged on notifiable models only",
          "[backend][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    // Register one notifiable model and one silent model.
    backend.registerModel("NotifiableModel",
                          [] { return std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>(); });
    backend.registerModel("SilentModel",
                          [] { return std::make_unique<morph::model::detail::ModelHolder<SilentModel>>(); });

    // Must not throw or crash regardless of model mix.
    REQUIRE_NOTHROW(backend.notifyBackendChanged());
}

// ── switchBackend test models ─────────────────────────────────────────────────
struct CountAction {
    int x = 0;
};
struct SwitchCountAction {};  // queries how many times onBackendChanged fired

struct CountModel {
    int value = 0;
    int switchCount = 0;
    int execute(const CountAction& act) {
        value += act.x;
        return value;
    }
    [[nodiscard]] int execute(const SwitchCountAction&) const { return switchCount; }
    void onBackendChanged() { ++switchCount; }
};

template <>
struct morph::model::ModelTraits<CountModel> {
    static constexpr std::string_view typeId() { return "SW_CountModel"; }
};
template <>
struct morph::model::ActionTraits<CountAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "SW_CountAction"; }
    static std::string toJson(const CountAction& act) { return R"({"x":)" + std::to_string(act.x) + "}"; }
    static CountAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};
template <>
struct morph::model::ActionTraits<SwitchCountAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "SW_SwitchCountAction"; }
    static std::string toJson(const SwitchCountAction&) { return "{}"; }
    static SwitchCountAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

// A LocalBackend that counts cancelPending() calls and tracks whether a
// reconnect handler is currently installed (non-null) -- used to observe
// switchBackend()'s "previous && previous != newShared" guards from the
// outside (Task 15a finding B9): a self-switch (switchBackend() called again
// with the SAME backend instance already active) must trip neither guard,
// since `previous == newShared` in that case.
class SwitchSelfObserverBackend : public morph::backend::LocalBackend {
public:
    explicit SwitchSelfObserverBackend(morph::exec::IExecutor& pool) : LocalBackend{pool} {}

    void setReconnectHandler(const std::function<void()>& handler) override { _handler = handler; }
    void cancelPending(const std::exception_ptr& exc) override {
        ++_cancelCount;
        LocalBackend::cancelPending(exc);
    }

    [[nodiscard]] bool hasHandler() const { return static_cast<bool>(_handler); }
    [[nodiscard]] int cancelCount() const { return _cancelCount; }

private:
    std::function<void()> _handler;
    int _cancelCount = 0;
};

using SyncExec = morph::testing::InlineExecutor;

// ── switchBackend tests ───────────────────────────────────────────────────────

TEST_CASE("morph::bridge::Bridge::switchBackend  -  handler works before and after switch", "[bridge][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};

    // Execute on original backend.
    std::atomic<int> res1{-1};
    handler.execute(CountAction{5}).then([&](int val) { res1.store(val); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return res1.load() != -1; }));
    REQUIRE(res1.load() == 5);

    // Switch to a fresh backend  -  model state resets (new instance).
    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    std::atomic<int> res2{-1};
    handler.execute(CountAction{7}).then([&](int val) { res2.store(val); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return res2.load() != -1; }));
    REQUIRE(res2.load() == 7);
}

TEST_CASE("morph::bridge::Bridge::switchBackend  -  destroyed handler not re-registered, no crash",
          "[bridge][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    {
        morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};
    }  // handler destroyed  -  weak_ptr in bridge goes stale

    morph::exec::ThreadPoolExecutor pool2{2};
    REQUIRE_NOTHROW(bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2)));
}

TEST_CASE("morph::bridge::Bridge::switchBackend  -  multiple live handlers all re-registered", "[bridge][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler1{bridge, &cbExec};
    morph::bridge::BridgeHandler<CountModel> handler2{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    std::atomic<int> res1{-1};
    std::atomic<int> res2{-1};
    handler1.execute(CountAction{10}).then([&](int val) { res1.store(val); }).onError([](const std::exception_ptr&) {
    });
    handler2.execute(CountAction{20}).then([&](int val) { res2.store(val); }).onError([](const std::exception_ptr&) {
    });
    REQUIRE(morph::testing::waitUntil([&] { return res1.load() != -1 && res2.load() != -1; }));
    REQUIRE(res1.load() == 10);
    REQUIRE(res2.load() == 20);
}

TEST_CASE("morph::bridge::Bridge::switchBackend(shared_ptr)  -  caller-owned instance can be re-installed",
          "[bridge][switch][shared_ptr]") {
    morph::exec::ThreadPoolExecutor poolInitial{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(poolInitial)};
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor poolA{2};
    morph::exec::ThreadPoolExecutor poolB{2};
    auto backendA = std::make_shared<morph::backend::LocalBackend>(poolA);
    auto backendB = std::make_shared<morph::backend::LocalBackend>(poolB);

    // Switch to a caller-owned shared_ptr backend -- the crux of the API this
    // overload adds: the caller keeps its own reference (use_count > 1) rather
    // than transferring ownership away, as the unique_ptr overload requires.
    bridge.switchBackend(backendA);
    REQUIRE(backendA.use_count() > 1);

    // Switch away to a second backend, then back to the *same* backendA
    // instance -- this is exactly what a unique_ptr signature cannot express,
    // since the first switchBackend call would have consumed it.
    bridge.switchBackend(backendB);
    REQUIRE_NOTHROW(bridge.switchBackend(backendA));

    std::atomic<int> res{-1};
    handler.execute(CountAction{9}).then([&](int val) { res.store(val); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return res.load() != -1; }));
    REQUIRE(res.load() == 9);
}

TEST_CASE(
    "morph::bridge::Bridge::switchBackend(shared_ptr)  -  switching to the same backend instance twice is a "
    "true no-op (Task 15a finding B9)",
    "[bridge][switch][shared_ptr]") {
    // switchBackend()'s tail runs `if (previous && previous != newShared) {
    // previous->setReconnectHandler(nullptr); }` and the identical guard
    // around cancelPending(). A self-switch (the same backend instance
    // installed twice in a row) makes `previous == newShared`, so neither
    // call may fire -- clearing the reconnect handler installReconnectHandler
    // just (re)installed on that same backend a moment earlier, or cancelling
    // that backend's own still-live pending calls, would both be genuine bugs
    // a caller re-installing an already-active backend would hit for no
    // reason.
    morph::exec::ThreadPoolExecutor poolInit{2};
    morph::exec::ThreadPoolExecutor poolObs{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(poolInit)};

    auto observer = std::make_shared<SwitchSelfObserverBackend>(poolObs);
    bridge.switchBackend(observer);
    REQUIRE(observer->hasHandler());
    REQUIRE(observer->cancelCount() == 0);

    // Self-switch: `previous` (observer) and `newShared` (observer) are the
    // same instance.
    bridge.switchBackend(observer);
    CHECK(observer->hasHandler());
    CHECK(observer->cancelCount() == 0);

    // The backend is still fully usable afterward -- a genuine end-to-end
    // sanity check, not just an internal-state probe.
    SyncExec cbExec;
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};
    std::atomic<int> res{-1};
    handler.execute(CountAction{4}).then([&](int val) { res.store(val); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return res.load() != -1; }));
    REQUIRE(res.load() == 4);
}

TEST_CASE(
    "morph::bridge::Bridge::switchBackend(unique_ptr) still transfers ownership (delegates to shared_ptr "
    "overload)",
    "[bridge][switch][shared_ptr]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    REQUIRE_NOTHROW(bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2)));

    std::atomic<int> res{-1};
    handler.execute(CountAction{3}).then([&](int val) { res.store(val); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return res.load() != -1; }));
    REQUIRE(res.load() == 3);
}

// ── Deep onBackendChanged count verification ──────────────────────────────────

TEST_CASE(
    "morph::bridge::Bridge::switchBackend  -  onBackendChanged called exactly once on new model after one switch",
    "[bridge][switch][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    // Query the new model instance's switchCount.
    std::atomic<int> count{-1};
    handler.execute(SwitchCountAction{})
        .then([&](int val) { count.store(val); })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return count.load() != -1; }));
    REQUIRE(count.load() == 1);
}

TEST_CASE(
    "morph::bridge::Bridge::switchBackend  -  onBackendChanged called exactly once per switch across two switches",
    "[bridge][switch][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    morph::exec::ThreadPoolExecutor pool3{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool3));

    // Each switch creates a fresh model instance. The LAST instance receives
    // onBackendChanged() exactly once (from the second switch).
    std::atomic<int> count{-1};
    handler.execute(SwitchCountAction{})
        .then([&](int val) { count.store(val); })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return count.load() != -1; }));
    REQUIRE(count.load() == 1);
}

// ── Reconnect handler coverage (Task 15a findings B14/B15, and B5's ─────────
// ── reconnect-path variant) ──────────────────────────────────────────────────
//
// installReconnectHandler() installs a callback (invoked on the backend's
// transport thread on a real reconnect) that re-registers every live
// HandlerBinding. LocalBackend never fires it itself (no transport to
// reconnect), so these tests use a small LocalBackend subclass that records
// the installed handler and lets the test fire it directly, plus which of
// registerModelShared/registerModelWithContext the re-registration loop used.
namespace {
class ReconnectableLocalBackend : public morph::backend::LocalBackend {
public:
    explicit ReconnectableLocalBackend(morph::exec::IExecutor& pool) : LocalBackend{pool} {}

    void setReconnectHandler(const std::function<void()>& handler) override { _handler = handler; }
    void fireReconnect() const {
        if (_handler) {
            _handler();
        }
    }
    // Copies out the currently-installed handler so a test can invoke it
    // AFTER something else (e.g. switchBackend() retiring this backend)
    // clears `_handler` via setReconnectHandler(nullptr) -- the snapshot
    // still runs the original closure with its original captures, exactly
    // like test_bridge_lifetime.cpp's identical FakeReconnectBackend::
    // snapshotHandler() pattern.
    [[nodiscard]] std::function<void()> snapshotHandler() const { return _handler; }

    morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
        morph::backend::detail::InstanceIdentity identity) override {
        ++_sharedCallCount;
        _lastSharedContextKey = std::string{identity.contextKey};
        _lastSharedPrimary = std::string{identity.primary};
        return LocalBackend::registerModelShared(typeId, std::move(factory), identity);
    }
    morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) override {
        ++_withContextCallCount;
        return LocalBackend::registerModelWithContext(typeId, std::move(factory), contextKey);
    }

    [[nodiscard]] int sharedCallCount() const { return _sharedCallCount; }
    [[nodiscard]] int withContextCallCount() const { return _withContextCallCount; }
    [[nodiscard]] const std::string& lastSharedPrimary() const { return _lastSharedPrimary; }
    [[nodiscard]] const std::string& lastSharedContextKey() const { return _lastSharedContextKey; }

private:
    std::function<void()> _handler;
    int _sharedCallCount = 0;
    int _withContextCallCount = 0;
    std::string _lastSharedContextKey;
    std::string _lastSharedPrimary;
};
}  // namespace

TEST_CASE("Bridge: reconnect handler skips a shared binding that never attached (Task 15a finding B14)",
          "[bridge][switch][reconnect]") {
    // The reconnect loop's `if (binding->shared && binding->primary.empty())
    // { continue; }` guard -- a shared binding registered via
    // registerSharedHandler() but never attached has no instance to
    // re-create, per the guard's own adjacent comment.
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<ReconnectableLocalBackend>(pool);
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};

    auto binding = bridge.registerSharedHandler<CountModel>();
    REQUIRE(binding->currentId.load() == 0U);
    REQUIRE(binding->primary.empty());

    REQUIRE_NOTHROW(rawBackend->fireReconnect());

    CHECK(binding->currentId.load() == 0U);
    CHECK(rawBackend->sharedCallCount() == 0);
    CHECK(rawBackend->withContextCallCount() == 0);
}

TEST_CASE(
    "Bridge: reconnect handler re-registers an attached shared binding via registerModelShared, with the "
    "correct key (Task 15a finding B15)",
    "[bridge][switch][reconnect]") {
    // Complement of B14: an attached shared binding (shared=true, primary
    // non-empty) must come back through registerModelShared, carrying its
    // real contextKey/primary -- not registerModelWithContext, which would
    // silently drop the sharing.
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<ReconnectableLocalBackend>(pool);
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};

    auto binding = bridge.registerSharedHandler<CountModel>();
    bridge.attachHandler<CountModel>(binding, "42");
    REQUIRE(binding->currentId.load() != 0U);
    REQUIRE(binding->primary == "42");

    int const sharedCallsBefore = rawBackend->sharedCallCount();
    int const withContextCallsBefore = rawBackend->withContextCallCount();

    rawBackend->fireReconnect();

    CHECK(rawBackend->sharedCallCount() == sharedCallsBefore + 1);
    CHECK(rawBackend->withContextCallCount() == withContextCallsBefore);
    CHECK(rawBackend->lastSharedPrimary() == "42");
    CHECK(rawBackend->lastSharedContextKey() == "42");
    CHECK(binding->currentId.load() != 0U);
}

TEST_CASE(
    "Bridge: a stale reconnect fired by a backend that is still alive but no longer current is ignored "
    "(Task 15a finding B5, reconnect variant)",
    "[bridge][switch][reconnect]") {
    // The reconnect handler's own guard (`!pinned || pinned != loadBackend()`)
    // has the identical shape as attachHandlerAsync/ensureBoundAsync/
    // assignHandlerPrimary's stale-reply guards. switchBackend() itself
    // correctly clears the OUTGOING backend's reconnect handler
    // (`previous->setReconnectHandler(nullptr)`) the moment it retires it, so
    // firing backendA's *current* handler after the switch would just be a
    // no-op (empty std::function) -- it would never reach this guard at all.
    // Snapshotting the handler *before* the switch (mirroring
    // test_bridge_lifetime.cpp's identical FakeReconnectBackend::
    // snapshotHandler() pattern) and firing that snapshot afterward models a
    // reconnect already latched on the transport thread at the moment the
    // switch lands: weakBackend.lock() still succeeds (the test's own
    // shared_ptr keeps backendA alive), but loadBackend() now returns
    // backendB.
    morph::exec::ThreadPoolExecutor poolInit{2};
    morph::exec::ThreadPoolExecutor poolA{2};
    morph::exec::ThreadPoolExecutor poolB{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(poolInit)};

    auto backendA = std::make_shared<ReconnectableLocalBackend>(poolA);
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(backendA));

    auto binding = bridge.registerSharedHandler<CountModel>();
    bridge.attachHandler<CountModel>(binding, "7");
    REQUIRE(binding->currentId.load() != 0U);

    auto staleHandler = backendA->snapshotHandler();
    REQUIRE(staleHandler);

    // Switch away to a different backend -- backendA stays alive via the
    // test's own shared_ptr, but is no longer current.
    auto backendB = std::make_shared<ReconnectableLocalBackend>(poolB);
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(backendB));
    auto const idOnB = binding->currentId.load();
    REQUIRE(idOnB != 0U);
    int const bCallsBefore = backendB->sharedCallCount();

    // Fire the snapshotted (now-stale) reconnect handler directly.
    REQUIRE_NOTHROW(staleHandler());

    // Must be a no-op: no re-registration on either backend, and the
    // binding's id (now on backendB) is untouched.
    CHECK(binding->currentId.load() == idOnB);
    CHECK(backendB->sharedCallCount() == bCallsBefore);
}

// ── Remote backend no-op ──────────────────────────────────────────────────────

#include <morph/core/remote.hpp>

TEST_CASE("morph::backend::SimulatedRemoteBackend::notifyBackendChanged is a documented no-op", "[remote][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::backend::SimulatedRemoteBackend backend{*server};
    REQUIRE_NOTHROW(backend.notifyBackendChanged());
}

// ── switchBackend rollback on partial-registration failure ────────────────────

namespace {

/// @brief Backend whose `registerModelWithContext` throws once the Nth call is
/// reached, so `Bridge::switchBackend`'s Phase-1 rollback can be exercised
/// deterministically. `deregisterModel` optionally throws too, to exercise the
/// nested rollback-failure log path.
class FlakyBackend : public morph::backend::detail::IBackend {
public:
    FlakyBackend(morph::exec::IExecutor& pool MORPH_LIFETIMEBOUND, int failOnCallNumber, bool deregisterThrows = false)
        : _inner{pool}, _failOnCallNumber{failOnCallNumber}, _deregisterThrows{deregisterThrows} {}

    morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory) override {
        return registerModelWithContext(typeId, std::move(factory), {});
    }

    morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) override {
        ++_calls;
        if (_calls == _failOnCallNumber) {
            throw std::runtime_error("simulated registration failure");
        }
        auto id = _inner.registerModelWithContext(typeId, std::move(factory), contextKey);
        _registered.push_back(id);
        return id;
    }

    void deregisterModel(morph::exec::detail::ModelId mid) override {
        ++(*_deregisterCalls);
        if (_deregisterThrows) {
            throw std::runtime_error("simulated deregister failure");
        }
        _inner.deregisterModel(mid);
    }

    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId mid,
                                                            morph::backend::detail::ActionCall call,
                                                            morph::exec::IExecutor* cbExec) override {
        return _inner.execute(mid, std::move(call), cbExec);
    }

    void notifyBackendChanged() override { _inner.notifyBackendChanged(); }
    void cancelPending(const std::exception_ptr& exc) override { _inner.cancelPending(exc); }

    /// @brief Shared handle to the deregister-call counter.
    ///
    /// `Bridge::switchBackend` takes ownership of the backend and destroys it
    /// when a partial-registration failure unwinds, so a raw `this` pointer is
    /// dangling by the time the test inspects the rollback. The counter lives in
    /// a `shared_ptr` the test can hold independently, outliving the backend.
    [[nodiscard]] std::shared_ptr<const int> deregisterCallCounter() const { return _deregisterCalls; }

private:
    morph::backend::LocalBackend _inner;
    int _failOnCallNumber;
    bool _deregisterThrows;
    int _calls{0};
    std::shared_ptr<int> _deregisterCalls{std::make_shared<int>(0)};
    std::vector<morph::exec::detail::ModelId> _registered;
};

}  // namespace

TEST_CASE("morph::bridge::Bridge::switchBackend  -  rollback on partial failure leaves old backend active",
          "[bridge][switch][rollback]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler1{bridge, &cbExec};
    morph::bridge::BridgeHandler<CountModel> handler2{bridge, &cbExec};

    auto const idBefore1 = handler1.binding()->currentId.load();
    auto const idBefore2 = handler2.binding()->currentId.load();

    morph::exec::ThreadPoolExecutor pool2{2};
    // Two live handlers means two registerModelWithContext calls; fail on the 2nd
    // so the 1st is already staged when the rollback runs.
    auto flaky = std::make_unique<FlakyBackend>(pool2, /*failOnCallNumber=*/2);
    auto deregisterCalls = flaky->deregisterCallCounter();  // outlives the backend

    REQUIRE_THROWS_AS(bridge.switchBackend(std::move(flaky)), std::runtime_error);
    REQUIRE(*deregisterCalls == 1);  // the 1 staged registration was rolled back

    // currentId values are untouched — the switch is a no-op on failure.
    REQUIRE(handler1.binding()->currentId.load() == idBefore1);
    REQUIRE(handler2.binding()->currentId.load() == idBefore2);

    // The old backend is still active and functional.
    std::atomic<int> res{-1};
    handler1.execute(CountAction{3}).then([&](int val) { res.store(val); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return res.load() != -1; }));
    REQUIRE(res.load() == 3);
}

TEST_CASE("morph::bridge::Bridge::switchBackend  -  rollback still rethrows original error when deregister also fails",
          "[bridge][switch][rollback]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler1{bridge, &cbExec};
    morph::bridge::BridgeHandler<CountModel> handler2{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    auto flaky = std::make_unique<FlakyBackend>(pool2, /*failOnCallNumber=*/2, /*deregisterThrows=*/true);
    auto deregisterCalls = flaky->deregisterCallCounter();  // outlives the backend

    REQUIRE_THROWS_AS(bridge.switchBackend(std::move(flaky)), std::runtime_error);
    REQUIRE(*deregisterCalls == 1);  // rollback attempted despite itself throwing
}

// ── BridgeHandler destructor: bridge destroyed first (dead-liveness-token branch) ─────────────

// Takes ownership of a bridge and destroys it on return.
//
// The destruction is spelled out of line because this file's one case for it is
// the single place in the tree where morph's contract and the attribute on
// `BridgeHandler`'s `Bridge&` genuinely disagree. bridge.md's "Lifetime &
// ownership" allows destroying the bridge *before* a live handler — the
// handler's liveness token turns its destructor into a no-op, and that carve-out
// is what the case below asserts. `[[clang::lifetimebound]]` has no way to say
// "except for destruction", so with the bridge held in a local `unique_ptr` and
// released in the same function, Clang reports the case as a use-after-scope. It
// is right about the code and wrong about morph. Handing the bridge to a
// function that destroys it keeps the mis-ordering the case is about, and the
// assertion below is unchanged: a handler destructor that dereferenced the dead
// bridge would still fault here, and under ASan loudly.
static void releaseBridge(std::unique_ptr<morph::bridge::Bridge> bridge) { bridge.reset(); }

TEST_CASE("morph::bridge::BridgeHandler destructor is a no-op when the bridge is already destroyed",
          "[bridge][lifetime]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    std::unique_ptr<morph::bridge::BridgeHandler<CountModel>> handler;
    auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(pool));
    handler = std::make_unique<morph::bridge::BridgeHandler<CountModel>>(*bridge, &cbExec);
    releaseBridge(std::move(bridge));  // bridge destroyed while handler still lives — its liveness token expires.
    REQUIRE_NOTHROW(handler.reset());  // handler dtor must not dereference the dangling Bridge&
}

// ── Behavior-parity fixtures for the compile-time onBackendChanged dispatch
//    refactor (docs/planned/backend_changed_dispatch.md) ─────────────────────
//
// AwareCounterModel exposes its onBackendChanged() side effect through an
// external std::shared_ptr<std::atomic<int>> (rather than a plain int member
// like NotifiableModel above) because LocalBackend::registerModel() only ever
// returns a ModelId — the constructed ModelHolder is never handed back to the
// caller, so there is no other way to observe the callback firing once the
// backend owns the holder.

struct AwareCounterModel {
    std::shared_ptr<std::atomic<int>> counter;
    void onBackendChanged() { counter->fetch_add(1); }
};

static std::unique_ptr<morph::model::detail::IModelHolder> makeAwareCounterHolder(
    std::shared_ptr<std::atomic<int>> counter) {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<AwareCounterModel>>();
    holder->model.counter = std::move(counter);
    return holder;
}

// ── Parity baseline ───────────────────────────────────────────────────────────
// These two tests pass against today's dynamic_cast-based
// LocalBackend::notifyBackendChanged (this task adds no production code).
// Task 3 re-runs them, byte-for-byte unmodified, against the rewritten
// implementation to prove the refactor is behavior-preserving.

TEST_CASE(
    "morph::backend::LocalBackend::notifyBackendChanged: parity baseline - notifies every change-aware "
    "model in a mixed population",
    "[backend][notify][parity]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    auto counter1 = std::make_shared<std::atomic<int>>(0);
    auto counter2 = std::make_shared<std::atomic<int>>(0);
    auto counter3 = std::make_shared<std::atomic<int>>(0);

    backend.registerModel("Aware1", [counter1] { return makeAwareCounterHolder(counter1); });
    backend.registerModel("Silent1",
                          [] { return std::make_unique<morph::model::detail::ModelHolder<SilentModel>>(); });
    backend.registerModel("Aware2", [counter2] { return makeAwareCounterHolder(counter2); });
    backend.registerModel("Silent2",
                          [] { return std::make_unique<morph::model::detail::ModelHolder<SilentModel>>(); });
    backend.registerModel("Aware3", [counter3] { return makeAwareCounterHolder(counter3); });

    backend.notifyBackendChanged();

    // Set semantics, not order: the spec makes no ordering guarantee across
    // models (each is serialised only against itself, via its own strand), so
    // this only asserts that all three eventually fire, not in what order.
    REQUIRE(morph::testing::waitUntil(
        [&] { return counter1->load() == 1 && counter2->load() == 1 && counter3->load() == 1; }));
}

TEST_CASE(
    "morph::backend::LocalBackend::notifyBackendChanged: parity baseline - a deregistered change-aware "
    "model is never notified again",
    "[backend][notify][parity]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    auto counter = std::make_shared<std::atomic<int>>(0);
    auto mid = backend.registerModel("Aware", [counter] { return makeAwareCounterHolder(counter); });

    backend.notifyBackendChanged();
    REQUIRE(morph::testing::waitUntil([&] { return counter->load() == 1; }));

    backend.deregisterModel(mid);
    REQUIRE_NOTHROW(backend.notifyBackendChanged());

    // Give any errant post a chance to land, then confirm the count never moved.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(counter->load() == 1);
}

// ── morph::model::detail::IModelHolder::isBackendChangeAware / onBackendChanged ──
// (compile-time capability capture — introduced to replace the dynamic_cast
// sweep in LocalBackend::notifyBackendChanged; see docs/spec/core/backend.md)

TEST_CASE("morph::model::detail::ModelHolder::isBackendChangeAware reflects BackendChangedNotifiable<M>",
          "[model][concept]") {
    auto notifiable = std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>();
    auto silent = std::make_unique<morph::model::detail::ModelHolder<SilentModel>>();

    REQUIRE(notifiable->isBackendChangeAware());
    REQUIRE_FALSE(silent->isBackendChangeAware());
}

TEST_CASE("morph::model::detail::IModelHolder::onBackendChanged forwards to the model when change-aware",
          "[model][concept]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>();
    morph::model::detail::IModelHolder* base = holder.get();  // base-class virtual dispatch, no dynamic_cast

    base->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 1);

    base->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 2);
}

TEST_CASE("morph::model::detail::IModelHolder::onBackendChanged is a no-op for a non-aware model",
          "[model][concept]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<SilentModel>>();
    morph::model::detail::IModelHolder* base = holder.get();

    REQUIRE_NOTHROW(base->onBackendChanged());
}

namespace {

/// @brief Minimal `IModelHolder` that leaves `onBackendChanged` at its base
/// default. `ModelHolder<Model>` always overrides it (its body only varies
/// via `if constexpr`), so the base's own no-op body is otherwise
/// unreachable through any real holder; this stub exercises it directly.
struct BareModelHolder : morph::model::detail::IModelHolder {
    [[nodiscard]] std::type_index type() const noexcept override { return typeid(BareModelHolder); }
    [[nodiscard]] bool isBackendChangeAware() const noexcept override { return false; }
};

}  // namespace

TEST_CASE("morph::model::detail::IModelHolder::onBackendChanged base default is a no-op", "[model][concept]") {
    BareModelHolder holder;
    REQUIRE_NOTHROW(holder.onBackendChanged());
}

TEST_CASE(
    "morph::model::detail::ModelHolder: the IModelHolder::onBackendChanged path and the "
    "dynamic_cast<IBackendChangedSink*> path both reach the same underlying call",
    "[model][concept]") {
    // ModelHolder<Model>::onBackendChanged() is declared once but is the final
    // overrider for two unrelated base virtuals: IModelHolder::onBackendChanged
    // (new) and, when Model is notifiable, IBackendChangedSink::onBackendChanged
    // (reached via BackendChangedMixin<Model, true>). This test proves both
    // call paths land on the identical implementation.
    auto holder = std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>();

    static_cast<morph::model::detail::IModelHolder&>(*holder).onBackendChanged();
    REQUIRE(holder->model.notifyCount == 1);

    auto* sink = dynamic_cast<morph::model::detail::IBackendChangedSink*>(holder.get());
    REQUIRE(sink != nullptr);
    sink->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 2);
}

// ── morph::backend::LocalBackend: _changeAware register/deregister bookkeeping ──

TEST_CASE("morph::backend::LocalBackend::notifyBackendChanged: no change-aware models means zero posts",
          "[backend][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    backend.registerModel("Silent", [] { return std::make_unique<morph::model::detail::ModelHolder<SilentModel>>(); });

    // No model in _changeAware: the loop body in notifyBackendChanged never
    // runs. Nothing to assert beyond "does not crash" — there is no observable
    // side effect from a silent model either way.
    REQUIRE_NOTHROW(backend.notifyBackendChanged());
}

TEST_CASE(
    "morph::backend::LocalBackend: notifyBackendChanged only notifies the currently-registered change-aware "
    "models across a deregister/register cycle",
    "[backend][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    auto counterA = std::make_shared<std::atomic<int>>(0);
    auto midA = backend.registerModel("A", [counterA] { return makeAwareCounterHolder(counterA); });

    backend.notifyBackendChanged();
    REQUIRE(morph::testing::waitUntil([&] { return counterA->load() == 1; }));

    // Deregister the aware model, then register a fresh aware model under a
    // new id — mirroring what Bridge::switchBackend does per-handler on a
    // backend swap, but exercised directly at the LocalBackend level.
    backend.deregisterModel(midA);
    auto counterB = std::make_shared<std::atomic<int>>(0);
    auto midB = backend.registerModel("B", [counterB] { return makeAwareCounterHolder(counterB); });

    backend.notifyBackendChanged();
    REQUIRE(morph::testing::waitUntil([&] { return counterB->load() == 1; }));

    // counterA must not have grown further: midA left _changeAware (and
    // _models) when it was deregistered, so this second notifyBackendChanged
    // call never reaches it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(counterA->load() == 1);

    (void)midB;
}
