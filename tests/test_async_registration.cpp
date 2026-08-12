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
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

// ── Issue #67: assignHandlerPrimary prefers IBackend::assignPrimaryAsync ────
//
// A model whose result-keyed action (BRIDGE_KEY_FROM_RESULT) drives
// Bridge::assignHandlerPrimary. Needs **external** linkage (not an anonymous
// namespace) for the same reason as test_shared_instances.cpp's ShiCreate:
// glaze's plain-aggregate reflection cannot see into an anonymous namespace,
// and BRIDGE_REGISTER_* specialises templates at global scope.
// NOLINTBEGIN(misc-use-internal-linkage)
struct ARCreate {
    std::int64_t initial = 0;
};

struct ARCreated {
    std::int64_t id = 0;
    std::int64_t value = 0;
};

struct ARCreateModel {
    std::int64_t value = 0;
    ARCreated execute(const ARCreate& act) {
        static std::atomic<std::int64_t> nextId{5000};
        value = act.initial;
        return {.id = nextId.fetch_add(1), .value = value};
    }
};

BRIDGE_REGISTER_MODEL(ARCreateModel, "AR_CreateModel")
BRIDGE_REGISTER_ACTION(ARCreateModel, ARCreate, "AR_Create")
BRIDGE_MODEL_KEY_FROM_RESULT(ARCreateModel, ARCreate, &ARCreated::id);
// NOLINTEND(misc-use-internal-linkage)

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

// Offers an async assignPrimary path that does not complete until the test
// calls completeNext()/failNext() -- the assignHandlerPrimary counterpart of
// AsyncRegisterBackend above, simulating a backend (QtWebSocketBackend is the
// one real example) whose promote-in-place reply arrives later, on its own
// thread, instead of Bridge::assignHandlerPrimary falling back to the
// synchronous assignPrimary. Everything else (registration, execute) is
// delegated to a real LocalBackend so a result-keyed action's ensureBound()
// step behaves normally; only the promotion step is deferred.
class AsyncAssignPrimaryBackend : public morph::backend::LocalBackend {
public:
    explicit AsyncAssignPrimaryBackend(morph::exec::IExecutor& pool) : LocalBackend{pool} {}

    bool assignPrimaryAsync(morph::exec::detail::ModelId mid, const std::string& typeId, std::string_view primary,
                            std::function<void(morph::exec::detail::ModelId)> onRegistered,
                            std::function<void(const std::string&)> onError) override {
        std::scoped_lock const lock{_pendingMtx};
        _pending.push_back(Pending{mid, typeId, std::string{primary}, std::move(onRegistered), std::move(onError)});
        return true;
    }

    // Test hooks: settle the oldest still-pending async promotion. Unlike
    // AsyncRegisterBackend::completeNext(), this does not also call the real
    // (synchronous) assignPrimary -- assignPrimaryAsync's contract is that the
    // backend performs the promotion itself and merely reports back, so the
    // test double's completion is the promotion.
    void completeNext() {
        Pending pending;
        {
            std::scoped_lock const lock{_pendingMtx};
            REQUIRE_FALSE(_pending.empty());
            pending = std::move(_pending.front());
            _pending.erase(_pending.begin());
        }
        LocalBackend::assignPrimary(pending.mid, pending.typeId, pending.primary);
        pending.onRegistered(pending.mid);
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
        morph::exec::detail::ModelId mid;
        std::string typeId;
        std::string primary;
        std::function<void(morph::exec::detail::ModelId)> onRegistered;
        std::function<void(const std::string&)> onError;
    };
    mutable std::mutex _pendingMtx;
    std::vector<Pending> _pending;
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

// ── Issue #67: assignHandlerPrimary prefers IBackend::assignPrimaryAsync ────
//
// A result-keyed action's execute() calls ensureBound() then, once the reply
// names the key, assignHandlerPrimary(). When the backend offers
// assignPrimaryAsync, Bridge::assignHandlerPrimary must send the request and
// return without blocking, publish binding->primary/contextKey only once the
// (possibly deferred) reply confirms it, and guard a stale reply the same way
// registerHandlerImpl's async callback does (Bridge/binding gone, or a
// switchBackend()/concurrent promotion already moved past it).

TEST_CASE("Bridge::assignHandlerPrimary: uses the async path when the backend offers one; publishes on completion",
          "[bridge][registration][issue67]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<AsyncAssignPrimaryBackend>(pool);
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    SyncExec cbExec;
    morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &cbExec};

    std::atomic<bool> done{false};
    std::optional<ARCreated> created;
    handler.execute(ARCreate{.initial = 7})
        .then([&](ARCreated result) {
            created = result;
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    // The promotion reply has not arrived yet: the handler already has an
    // anonymous instance (ensureBound ran synchronously against LocalBackend),
    // so the action itself has already executed and resolved -- but the
    // promotion is what assignPrimaryAsync defers, not the execute() call.
    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(created.has_value());
    REQUIRE(rawBackend->pendingCount() == 1);
    // Not yet promoted: the async reply confirming the promotion is still
    // outstanding, so the handler must not report a primary key it has not
    // actually been filed under yet.
    CHECK_FALSE(handler.primary().has_value());

    rawBackend->completeNext();
    REQUIRE(handler.primary().has_value());
    CHECK(handler.primary().value_or(-1) == created->id);
}

TEST_CASE("Bridge::assignHandlerPrimary: async promotion failure leaves the binding unpromoted (no crash, logged)",
          "[bridge][registration][issue67]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<AsyncAssignPrimaryBackend>(pool);
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    SyncExec cbExec;
    morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &cbExec};

    std::atomic<bool> done{false};
    handler.execute(ARCreate{.initial = 3}).then([&](ARCreated) { done.store(true); }).onError([&](
                                                                                                    const std::exception_ptr&) {
        done.store(true);
    });
    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(rawBackend->pendingCount() == 1);

    REQUIRE_NOTHROW(rawBackend->failNext("simulated promotion failure"));
    CHECK_FALSE(handler.primary().has_value());
}

TEST_CASE(
    "Bridge::assignHandlerPrimary: a stale async reply from a backend that is no longer current is ignored",
    "[bridge][registration][issue67]") {
    // Mirrors registerHandlerImpl's own "stale reply after switchBackend()"
    // test above: the async promotion is still pending on the *old* backend
    // when switchBackend() moves the bridge to a new one; the eventual reply
    // must not overwrite whatever state the new backend's own handling of
    // the binding already established.
    morph::exec::ThreadPoolExecutor pool{2};
    auto asyncBackend = std::make_shared<AsyncAssignPrimaryBackend>(pool);
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(asyncBackend));
    SyncExec cbExec;
    morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &cbExec};

    std::atomic<bool> done{false};
    handler.execute(ARCreate{.initial = 4}).then([&](ARCreated) { done.store(true); }).onError([&](
                                                                                                    const std::exception_ptr&) {
        done.store(true);
    });
    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(asyncBackend->pendingCount() == 1);
    CHECK_FALSE(handler.primary().has_value());

    // Switch away WHILE the promotion on asyncBackend is still pending.
    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    // The stale reply from the now-superseded backend finally arrives. It
    // must be ignored (the `pinned != loadBackend()` guard), not promote the
    // binding using a backend nothing points at any more.
    asyncBackend->completeNext();
    CHECK_FALSE(handler.primary().has_value());
}

TEST_CASE(
    "Bridge::assignHandlerPrimary: an async reply for an already-promoted binding does not overwrite it",
    "[bridge][registration][issue67]") {
    // A binding whose primary is already set (by a concurrent
    // attach/assign that raced ahead of this async reply, or simply already
    // promoted) must not be overwritten -- the "if (!strongBinding->primary.
    // empty())" guard inside the async callback. Drives this directly via
    // Bridge::ensureBound/assignHandlerPrimary rather than through
    // BridgeHandler::execute, so the binding's primary can be forced to a
    // specific value between starting the async promotion and completing it.
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<AsyncAssignPrimaryBackend>(pool);
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_CreateModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARCreateModel>(); };
    bridge.ensureBound(binding);
    REQUIRE(binding->currentId.load() != 0U);

    bridge.assignHandlerPrimary<ARCreateModel>(binding, "100");
    REQUIRE(rawBackend->pendingCount() == 1);
    CHECK(binding->primary.empty());

    // Something else promotes this binding first -- e.g. a second, faster
    // assignHandlerPrimary call for the same binding (assignHandlerPrimary's
    // own early-return guard prevents a second concurrent async request once
    // primary is non-empty, so simulate the race's *outcome* directly, the
    // same way the existing already-bound tests in this file drive
    // currentId directly rather than orchestrating true concurrency).
    binding->primary = "999";
    binding->contextKey = "999";

    // The original async reply for "100" now arrives. It must not clobber
    // the "999" that (in this scenario) got there first.
    rawBackend->completeNext();
    CHECK(binding->primary == "999");
    CHECK(binding->contextKey == "999");
}

TEST_CASE(
    "Bridge::assignHandlerPrimary: a stale async reply after the binding itself is dropped is a safe no-op",
    "[bridge][registration][issue67]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<AsyncAssignPrimaryBackend>(pool);
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    SyncExec cbExec;

    {
        morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &cbExec};
        std::atomic<bool> done{false};
        handler.execute(ARCreate{.initial = 1}).then([&](ARCreated) { done.store(true); }).onError([&](
                                                                                                        const std::exception_ptr&) {
            done.store(true);
        });
        REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
        REQUIRE(rawBackend->pendingCount() == 1);
    }  // handler (and its binding, held only weakly by the bridge) drops out of scope here.

    // The deferred reply's weakBinding.lock() must fail cleanly rather than
    // touch a binding that is now gone.
    REQUIRE_NOTHROW(rawBackend->completeNext());
}

TEST_CASE(
    "Bridge::assignHandlerPrimary: an async reply arriving after ~Bridge() is a safe no-op",
    "[bridge][registration][issue67]") {
    morph::exec::ThreadPoolExecutor pool{2};
    // Co-owned via shared_ptr (installed through the switchBackend(shared_ptr)
    // overload, before any handler exists to be re-registered by it) so the
    // backend -- and therefore the deferred promotion callbacks it is holding
    // -- outlives the Bridge below, exactly like AsyncBackendShim does for the
    // registerHandler ~Bridge() test above.
    auto rawBackend = std::make_shared<AsyncAssignPrimaryBackend>(pool);
    auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(pool));
    bridge->switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(rawBackend));
    SyncExec cbExec;

    auto handler = std::make_unique<morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared>>(*bridge,
                                                                                                              &cbExec);
    std::atomic<bool> done{false};
    handler->execute(ARCreate{.initial = 9}).then([&](ARCreated) { done.store(true); }).onError([&](
                                                                                                     const std::exception_ptr&) {
        done.store(true);
    });
    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(rawBackend->pendingCount() == 1);

    // ~Bridge() runs; the handler's binding (and the handler itself, which
    // holds a reference back to the bridge) must not be touched by the reply
    // that arrives afterward.
    bridge.reset();

    REQUIRE_NOTHROW(rawBackend->completeNext());
}

// ── Bridge::whenBound: double-checked-lock re-check under registrationMtx ───
//
// whenBound() checks isBound() lock-free first (the common case: already
// bound, resolve immediately without ever touching registrationMtx). Only if
// that sees "not yet" does it acquire registrationMtx and check again, because
// registerHandlerImpl's callback can bind the id (under _mtx, via
// currentId.store) and only afterward acquire registrationMtx to resolve
// waiters -- so a whenBound() call can land in the narrow window between
// those two steps. Without the second check, such a call would queue a
// waiter that resolveRegistrationWaiters has *already* iterated past,
// hanging forever. Racing many concurrent whenBound() callers against one
// completeNext() many times gives the scheduler repeated chances to land a
// call inside that window.
TEST_CASE("Bridge::whenBound: concurrent callers racing the exact moment registration settles all resolve",
          "[bridge][registration][issue60][concurrency]") {
    // 50 trials x 8 real OS threads each (400 total thread spawns) still gives
    // the scheduler repeated chances to land a call inside the narrow window
    // under test.
    constexpr int kTrials = 50;
    constexpr int kWaitersPerTrial = 8;

    for (int trial = 0; trial < kTrials; ++trial) {
        SyncExec cbExec;
        auto backend = std::make_unique<AsyncRegisterBackend>();
        auto* rawBackend = backend.get();
        morph::bridge::Bridge bridge{std::move(backend)};
        morph::bridge::BridgeHandler<ARModel> handler{bridge, &cbExec};

        std::atomic<int> resolvedTrue{0};
        std::atomic<int> resolvedOther{0};
        int readyWaiters = 0;
        bool go = false;
        std::mutex goMtx;
        std::condition_variable goCv;
        std::condition_variable readyCv;

        std::vector<std::thread> waiters;
        waiters.reserve(kWaitersPerTrial);
        // Releases every still-waiting waiter and joins all of them
        // unconditionally, including on the exception-unwind path a failed
        // REQUIRE below takes -- without this, a waiter thread still parked
        // on goCv when the destructor for `waiters` runs would either hang
        // forever (joining a thread that never wakes) or crash via
        // std::terminate (a joinable std::thread destroyed without being
        // joined/detached), masking the real assertion failure with an
        // unrelated crash.
        struct ReleaseAndJoin {
            std::mutex& goMtx;
            std::condition_variable& goCv;
            bool& go;
            std::vector<std::thread>& waiters;
            ~ReleaseAndJoin() {
                {
                    std::scoped_lock const lock{goMtx};
                    go = true;
                }
                goCv.notify_all();
                for (auto& thr : waiters) {
                    if (thr.joinable()) {
                        thr.join();
                    }
                }
            }
        } releaseAndJoin{goMtx, goCv, go, waiters};
        // Both waits below are genuinely blocking (condition_variable),
        // never a busy-spin: Valgrind serialises threads onto one real core
        // with no scheduling-fairness guarantee against a tight spin loop --
        // reproduced hanging/crashing under real Valgrind with this test's
        // original `while (!go.load()) {}`, and again with a polling
        // `waitUntil` re-acquiring `goMtx` in a loop (still a spin, just on a
        // mutex instead of an atomic). Only a wait that actually parks the
        // thread (no polling of any kind, on any variable) is safe under
        // Valgrind's cooperative scheduler here.
        for (int w = 0; w < kWaitersPerTrial; ++w) {
            waiters.emplace_back([&] {
                {
                    std::scoped_lock const lock{goMtx};
                    ++readyWaiters;
                }
                readyCv.notify_one();
                std::unique_lock<std::mutex> lock{goMtx};
                goCv.wait(lock, [&] { return go; });
                lock.unlock();
                handler.whenBound()
                    .then([&](bool ok) {
                        if (ok) {
                            resolvedTrue.fetch_add(1);
                        } else {
                            resolvedOther.fetch_add(1);
                        }
                    })
                    .onError([&](const std::exception_ptr&) { resolvedOther.fetch_add(1); });
            });
        }
        {
            std::unique_lock<std::mutex> lock{goMtx};
            REQUIRE(readyCv.wait_for(lock, std::chrono::milliseconds{30'000},
                                     [&] { return readyWaiters == kWaitersPerTrial; }));
            go = true;
        }
        goCv.notify_all();
        rawBackend->completeNext();

        // Join before asserting, not just in ReleaseAndJoin's destructor:
        // the destructor only runs once this scope exits, which is *after*
        // the REQUIREs below -- without an explicit join here, those
        // REQUIREs would race the very threads whose resolved/other counters
        // they check, reading a partial count before every .then()/.onError()
        // callback has actually run. ReleaseAndJoin::~ReleaseAndJoin() still
        // exists purely as an exception-safety net (see its own comment);
        // joining a thread twice is a no-op via the joinable() guard both
        // places use.
        for (auto& thr : waiters) {
            thr.join();
        }

        // Every single waiter's Completion must have settled -- a lost
        // wakeup (the bug the second isBound() check under the lock
        // prevents) would leave one hanging with neither counter
        // incremented, which SyncExec's synchronous callback delivery makes
        // observable immediately, no polling required.
        REQUIRE(resolvedTrue.load() + resolvedOther.load() == kWaitersPerTrial);
        // Once registration has actually completed (rawBackend->completeNext()
        // returned above), every waiter -- whichever check caught it -- must
        // see success: nothing here ever fails registration.
        REQUIRE(resolvedTrue.load() == kWaitersPerTrial);
    }
}
