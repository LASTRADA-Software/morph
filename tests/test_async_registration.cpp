// SPDX-License-Identifier: Apache-2.0
//
// Coverage for issue #26: Bridge::registerHandler() prefers
// IBackend::registerModelAsync when a backend offers one, falling back to the
// synchronous registerModelWithContext otherwise. AsyncRegisterBackend below
// is a minimal test double whose registerModelAsync defers its reply until
// the test explicitly completes it -- simulating a socket backend whose reply
// arrives later on its own thread (what QtWebSocketBackend's async path does
// against a real server), instead of blocking the calling thread via a nested
// event loop (what registerModel does today -- the pattern this issue is
// about, since Qt refuses to spin a nested loop on a WASM main thread at all).

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "test_support.hpp"

namespace {

struct ARCount {
    int x = 0;
};

struct ARModel {
    int execute(const ARCount& a) { return a.x; }
};

}  // namespace

template <>
struct morph::model::ActionTraits<ARCount> {
    using Result = int;
    static constexpr std::string_view typeId() { return "AR_Count"; }
    static std::string toJson(const ARCount& a) { return R"({"x":)" + std::to_string(a.x) + "}"; }
    static ARCount fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& r) { return std::to_string(r); }
    static int resultFromJson(std::string_view s) { return std::stoi(std::string{s}); }
};
template <>
struct morph::model::ModelTraits<ARModel> {
    static constexpr std::string_view typeId() { return "AR_Model"; }
};

namespace {

// Offers an async registration path that does not complete until the test
// calls completeNext()/failNext() -- simulating a backend whose registration
// reply arrives later, asynchronously, instead of blocking the caller.
class AsyncRegisterBackend : public morph::backend::detail::IBackend {
public:
    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory) override {
        std::scoped_lock const lock{_regMtx};
        auto holder = factory();
        auto const mid = morph::exec::detail::ModelId{_nextId++};
        _models[mid.v] = std::move(holder);
        return mid;
    }
    void deregisterModel(morph::exec::detail::ModelId mid) override {
        std::scoped_lock const lock{_regMtx};
        _models.erase(mid.v);
    }
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId mid,
                                                             morph::backend::detail::ActionCall call,
                                                             morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        morph::async::Completion<std::shared_ptr<void>> comp{state, cbExec};
        std::scoped_lock const lock{_regMtx};
        auto iter = _models.find(mid.v);
        if (iter == _models.end()) {
            state->setException(std::make_exception_ptr(std::runtime_error("no such model")));
            return comp;
        }
        state->setValue(call.localOp(*iter->second));
        return comp;
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr&) override {}
    void setReconnectHandler(const std::function<void()>&) override {}

    bool registerModelAsync(const std::string& typeId,
                            std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                            std::string_view /*contextKey*/,
                            std::function<void(morph::exec::detail::ModelId)> onRegistered,
                            std::function<void(const std::string&)> onError) override {
        std::scoped_lock const lock{_pendingMtx};
        _pending.push_back(Pending{typeId, std::move(factory), std::move(onRegistered), std::move(onError)});
        return true;
    }

    // Test hooks: settle the oldest still-pending async registration.
    void completeNext() {
        Pending pending;
        {
            std::scoped_lock const lock{_pendingMtx};
            REQUIRE_FALSE(_pending.empty());
            pending = std::move(_pending.front());
            _pending.erase(_pending.begin());
        }
        auto mid = registerModel(pending.typeId, pending.factory);
        pending.onRegistered(mid);
    }
    void failNext(const std::string& message) {
        Pending pending;
        {
            std::scoped_lock const lock{_pendingMtx};
            REQUIRE_FALSE(_pending.empty());
            pending = std::move(_pending.front());
            _pending.erase(_pending.begin());
        }
        pending.onError(message);
    }
    [[nodiscard]] std::size_t pendingCount() const {
        std::scoped_lock const lock{_pendingMtx};
        return _pending.size();
    }

private:
    struct Pending {
        std::string typeId;
        std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory;
        std::function<void(morph::exec::detail::ModelId)> onRegistered;
        std::function<void(const std::string&)> onError;
    };
    mutable std::mutex _pendingMtx;
    std::vector<Pending> _pending;

    mutable std::mutex _regMtx;
    std::unordered_map<uint64_t, std::unique_ptr<morph::model::detail::IModelHolder>> _models;
    uint64_t _nextId{100};
};

// Shim so a Bridge (which takes ownership of a unique_ptr) can hold a backend
// the test also keeps a shared_ptr to -- making it co-owned / able to outlive
// the Bridge (see test_bridge_lifetime.cpp's identical BackendShim). Also lets
// a still-async-capable backend be installed via the unique_ptr-only
// switchBackend() overload the codebase currently has.
class AsyncBackendShim : public morph::backend::detail::IBackend {
public:
    explicit AsyncBackendShim(std::shared_ptr<AsyncRegisterBackend> target) : _target{std::move(target)} {}
    morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory) override {
        return _target->registerModel(typeId, std::move(factory));
    }
    void deregisterModel(morph::exec::detail::ModelId mid) override { _target->deregisterModel(mid); }
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId mid,
                                                             morph::backend::detail::ActionCall call,
                                                             morph::exec::IExecutor* cbExec) override {
        return _target->execute(mid, std::move(call), cbExec);
    }
    void notifyBackendChanged() override { _target->notifyBackendChanged(); }
    void cancelPending(const std::exception_ptr& exc) override { _target->cancelPending(exc); }
    void setReconnectHandler(const std::function<void()>& handler) override { _target->setReconnectHandler(handler); }
    bool registerModelAsync(const std::string& typeId,
                            std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                            std::string_view contextKey, std::function<void(morph::exec::detail::ModelId)> onRegistered,
                            std::function<void(const std::string&)> onError) override {
        return _target->registerModelAsync(typeId, std::move(factory), contextKey, std::move(onRegistered),
                                           std::move(onError));
    }

private:
    std::shared_ptr<AsyncRegisterBackend> _target;
};

}  // namespace

using SyncExec = morph::testing::InlineExecutor;

TEST_CASE("Bridge::registerHandler: uses the async path when the backend offers one; binding starts unbound",
         "[bridge][registration][issue26]") {
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_Model";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARModel>(); };
    bridge.registerHandler(binding);

    // Still unbound: the async reply has not arrived yet -- proves registerHandler
    // did not block waiting for it (the whole point of this feature).
    CHECK(binding->currentId.load() == 0U);
    REQUIRE(rawBackend->pendingCount() == 1);

    rawBackend->completeNext();
    CHECK(binding->currentId.load() != 0U);
}

TEST_CASE("Bridge::registerHandler: async execute works once the deferred registration completes",
         "[bridge][registration][issue26]") {
    SyncExec cbExec;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARModel> handler{bridge, &cbExec};

    REQUIRE(rawBackend->pendingCount() == 1);
    rawBackend->completeNext();

    std::atomic<int> result{-1};
    handler.execute(ARCount{.x = 7}).then([&](int v) { result.store(v); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1; }));
    CHECK(result.load() == 7);
}

TEST_CASE("Bridge::registerHandler: onError leaves the binding unbound (no crash, logged)",
         "[bridge][registration][issue26]") {
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_Model";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARModel>(); };
    bridge.registerHandler(binding);

    REQUIRE(rawBackend->pendingCount() == 1);
    rawBackend->failNext("simulated registration failure");
    CHECK(binding->currentId.load() == 0U);
}

TEST_CASE("Bridge::registerHandler: a stale async reply after switchBackend() does not clobber the new id",
         "[bridge][registration][issue26]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto asyncBackendA = std::make_shared<AsyncRegisterBackend>();
    bridge.switchBackend(std::make_unique<AsyncBackendShim>(asyncBackendA));

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_Model";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARModel>(); };
    bridge.registerHandler(binding);
    REQUIRE(asyncBackendA->pendingCount() == 1);
    CHECK(binding->currentId.load() == 0U);

    // Switch to a second backend WHILE the registration on asyncBackendA is
    // still pending. switchBackend's re-registration loop doesn't consult
    // currentId -- it re-registers every tracked binding unconditionally --
    // so this binding gets a real id on the new backend synchronously, right
    // here, with the original async registration still outstanding.
    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));
    auto const idAfterSwitch = binding->currentId.load();
    CHECK(idAfterSwitch != 0U);

    // The original (now-stale) async reply from asyncBackendA finally
    // arrives. It must be ignored, not overwrite the id switchBackend already
    // assigned on the new, active backend.
    asyncBackendA->completeNext();
    CHECK(binding->currentId.load() == idAfterSwitch);
}

TEST_CASE("Bridge::registerHandler: an async reply arriving after ~Bridge() is a safe no-op",
         "[bridge][registration][issue26]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(pool));

    auto asyncBackend = std::make_shared<AsyncRegisterBackend>();
    bridge->switchBackend(std::make_unique<AsyncBackendShim>(asyncBackend));  // co-owned: outlives the Bridge below

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_Model";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARModel>(); };
    bridge->registerHandler(binding);
    REQUIRE(asyncBackend->pendingCount() == 1);

    bridge.reset();  // ~Bridge() runs; asyncBackend and binding both outlive it.

    // Must not crash or touch the dangling Bridge; the liveness guard makes
    // this a no-op, so the binding -- which the test still holds -- stays
    // exactly as it was at destruction (unbound).
    REQUIRE_NOTHROW(asyncBackend->completeNext());
    CHECK(binding->currentId.load() == 0U);
}

TEST_CASE("Bridge::registerHandler: an async reply arriving after the binding itself is dropped is a safe no-op",
         "[bridge][registration][issue26]") {
    // Distinct from the ~Bridge() case above: here the Bridge and backend are
    // both still alive, but the caller's own shared_ptr<HandlerBinding> --
    // the only strong owner, since Bridge tracks handlers via weak_ptr -- is
    // gone by the time the async reply arrives.
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};

    {
        auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
        binding->typeId = "AR_Model";
        binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARModel>(); };
        bridge.registerHandler(binding);
        REQUIRE(rawBackend->pendingCount() == 1);
    }  // binding drops out of scope; Bridge held only a weak_ptr to it.

    REQUIRE_NOTHROW(rawBackend->completeNext());
}

TEST_CASE(
    "Bridge::registerHandler: a stale async reply from a backend that is still alive but no longer current is "
    "ignored",
    "[bridge][registration][issue26]") {
    // Distinct from the switchBackend() test above: there, the old backend is
    // destroyed by the time the stale reply arrives (weakBackend.lock() fails
    // outright). Here the caller keeps the old backend alive via the
    // shared_ptr switchBackend() overload, so weakBackend.lock() still
    // succeeds -- the guard must instead catch it via the `!= loadBackend()`
    // comparison.
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto asyncBackendA = std::make_shared<AsyncRegisterBackend>();
    auto shimA = std::make_shared<AsyncBackendShim>(asyncBackendA);
    bridge.switchBackend(shimA);  // shared_ptr overload: the test keeps shimA alive below.

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_Model";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARModel>(); };
    bridge.registerHandler(binding);
    REQUIRE(asyncBackendA->pendingCount() == 1);

    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));
    auto const idAfterSwitch = binding->currentId.load();
    CHECK(idAfterSwitch != 0U);

    // shimA (and asyncBackendA) are still alive here via the test's own
    // shared_ptrs, unlike the destroyed-backend test above.
    asyncBackendA->completeNext();
    CHECK(binding->currentId.load() == idAfterSwitch);
}

TEST_CASE("Bridge::registerHandler: falls back to the synchronous path for a backend with no async support",
         "[bridge][registration][issue26]") {
    // LocalBackend does not override registerModelAsync, so the default
    // (returns false) applies and registerHandler falls back to
    // registerModelWithContext -- binding is bound immediately, exactly as
    // before this feature existed.
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_Model";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARModel>(); };
    bridge.registerHandler(binding);

    CHECK(binding->currentId.load() != 0U);
}

// ── Issue #60: registration-settled seam (whenBound/isBound) ────────────────
//
// executeVia() fails fast with "handler not bound" for a binding whose async
// registration hasn't round-tripped yet. Bridge::whenBound() gives a caller a
// seam to await that settlement instead of failing fast or polling by hand.

TEST_CASE("Bridge::whenBound: resolves immediately true for an already-bound handler",
         "[bridge][registration][issue60]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    SyncExec cbExec;
    morph::bridge::BridgeHandler<ARModel> handler{bridge, &cbExec};

    REQUIRE(handler.isBound());

    bool resolvedTrue = false;
    handler.whenBound().then([&](bool ok) { resolvedTrue = ok; }).onError([](const std::exception_ptr&) {});
    REQUIRE(resolvedTrue);
}

TEST_CASE("Bridge::whenBound: resolves false immediately when nothing is in flight and the handler is unbound",
         "[bridge][registration][issue60]") {
    // A binding that was never handed to registerHandler at all -- nothing is
    // registering, so there is nothing to wait for.
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_Model";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARModel>(); };
    CHECK_FALSE(bridge.isBound(binding));

    morph::testing::InlineExecutor exec;
    bool sawFalse = false;
    bool errored = false;
    bridge.whenBound(binding, &exec)
        .then([&](bool ok) { sawFalse = !ok; })
        .onError([&](const std::exception_ptr&) { errored = true; });
    REQUIRE(sawFalse);
    REQUIRE_FALSE(errored);
}

TEST_CASE("BridgeHandler::whenBound: fires once the deferred async registration completes",
         "[bridge][registration][issue60]") {
    SyncExec cbExec;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARModel> handler{bridge, &cbExec};

    // Not bound yet -- the async reply hasn't arrived.
    CHECK_FALSE(handler.isBound());

    bool resolvedTrue = false;
    bool errored = false;
    handler.whenBound().then([&](bool ok) { resolvedTrue = ok; }).onError([&](const std::exception_ptr&) {
        errored = true;
    });
    // whenBound() must not have resolved synchronously -- registration is
    // still in flight.
    CHECK_FALSE(resolvedTrue);
    CHECK_FALSE(errored);

    rawBackend->completeNext();
    CHECK(resolvedTrue);
    CHECK_FALSE(errored);
    CHECK(handler.isBound());

    // Once bound, dispatching immediately (the exact scenario issue #60
    // describes -- a dispatch issued right after connect) must succeed rather
    // than fail fast with "handler not bound".
    std::atomic<int> result{-1};
    handler.execute(ARCount{.x = 3}).then([&](int v) { result.store(v); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1; }));
    CHECK(result.load() == 3);
}

TEST_CASE("BridgeHandler::whenBound: delivers the registration failure via onError",
         "[bridge][registration][issue60]") {
    SyncExec cbExec;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARModel> handler{bridge, &cbExec};

    bool resolvedTrue = false;
    bool errored = false;
    handler.whenBound().then([&](bool ok) { resolvedTrue = ok; }).onError([&](const std::exception_ptr&) {
        errored = true;
    });

    rawBackend->failNext("simulated registration failure");
    CHECK_FALSE(resolvedTrue);
    CHECK(errored);
    CHECK_FALSE(handler.isBound());
}

TEST_CASE("Bridge::whenBound: multiple waiters on the same in-flight registration all resolve",
         "[bridge][registration][issue60]") {
    SyncExec cbExec;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARModel> handler{bridge, &cbExec};

    int resolvedCount = 0;
    handler.whenBound().then([&](bool) { ++resolvedCount; }).onError([](const std::exception_ptr&) {});
    handler.whenBound().then([&](bool) { ++resolvedCount; }).onError([](const std::exception_ptr&) {});
    handler.whenBound().then([&](bool) { ++resolvedCount; }).onError([](const std::exception_ptr&) {});
    CHECK(resolvedCount == 0);

    rawBackend->completeNext();
    CHECK(resolvedCount == 3);
}
