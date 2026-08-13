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
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

struct ARCount {
    int x = 0;
};

struct ARModel {
    int execute(const ARCount& a) { return a.x; }
};

// --- Keyed/shared coverage: the same deferred-reply idea applied to
// --- registerModelSharedAsync/attachModelAsync (the register-or-attach and
// --- attach counterparts of registerModelAsync).

/// Names the instance it wants in the action payload -> payload-keyed, so
/// executing it attaches the handler first (Bridge::attachHandlerAsync).
struct ARTouch {
    std::int64_t id = 0;
    int amount = 0;
};

/// Result of the creating action below; its `id` establishes the key.
struct ARKeyedCreated {
    std::int64_t id = 0;
    int value = 0;
};

/// Creates the entity, so its key can only come back in the reply ->
/// result-keyed, and executing it binds the handler first
/// (Bridge::ensureBoundAsync) and promotes it once the reply names the key.
struct ARKeyedCreate {
    int initial = 0;
};

struct ARKeyedModel {
    int value = 0;
    int execute(const ARTouch& act) {
        value += act.amount;
        return value;
    }
    ARKeyedCreated execute(const ARKeyedCreate& act) {
        value = act.initial;
        return {.id = 4242, .value = value};
    }
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

template <>
struct morph::model::ActionTraits<ARTouch> {
    using Result = int;
    static constexpr std::string_view typeId() { return "AR_Touch"; }
    static std::string toJson(const ARTouch& act) {
        return R"({"id":)" + std::to_string(act.id) + R"(,"amount":)" + std::to_string(act.amount) + "}";
    }
    static ARTouch fromJson(std::string_view /*json*/) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view text) { return std::stoi(std::string{text}); }
};
template <>
struct morph::model::ActionTraits<ARKeyedCreate> {
    using Result = ARKeyedCreated;
    static constexpr std::string_view typeId() { return "AR_KeyedCreate"; }
    static std::string toJson(const ARKeyedCreate& act) {
        return R"({"initial":)" + std::to_string(act.initial) + "}";
    }
    static ARKeyedCreate fromJson(std::string_view /*json*/) { return {}; }
    static std::string resultToJson(const ARKeyedCreated& res) {
        return R"({"id":)" + std::to_string(res.id) + R"(,"value":)" + std::to_string(res.value) + "}";
    }
    static ARKeyedCreated resultFromJson(std::string_view /*json*/) { return {}; }
};
template <>
struct morph::model::ModelTraits<ARKeyedModel> {
    static constexpr std::string_view typeId() { return "AR_KeyedModel"; }
};

BRIDGE_MODEL_KEY(ARKeyedModel, ARTouch, &ARTouch::id);
BRIDGE_KEY_FROM_RESULT(ARKeyedCreate, &ARKeyedCreated::id);

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

    // The shared/keyed counterparts, deferred exactly the same way: the reply
    // lands in the same queue completeNext()/failNext() drain, so a keyed
    // attach is observably non-blocking for the same reason a plain
    // registration is.
    bool registerModelSharedAsync(const std::string& typeId,
                                  std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                                  morph::backend::detail::InstanceIdentity /*identity*/,
                                  std::function<void(morph::exec::detail::ModelId)> onRegistered,
                                  std::function<void(const std::string&)> onError) override {
        std::scoped_lock const lock{_pendingMtx};
        _pending.push_back(Pending{.typeId = typeId,
                                   .factory = std::move(factory),
                                   .onRegistered = std::move(onRegistered),
                                   .onError = std::move(onError)});
        return true;
    }

    bool attachModelAsync(const std::string& typeId,
                          std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                          morph::backend::detail::InstanceIdentity /*identity*/,
                          morph::exec::detail::ModelId /*current*/,
                          std::function<void(morph::exec::detail::ModelId)> onRegistered,
                          std::function<void(const std::string&)> onError) override {
        std::scoped_lock const lock{_pendingMtx};
        _pending.push_back(Pending{.typeId = typeId,
                                   .factory = std::move(factory),
                                   .onRegistered = std::move(onRegistered),
                                   .onError = std::move(onError)});
        return true;
    }

    void assignPrimary(morph::exec::detail::ModelId mid, const std::string& /*typeId*/,
                       std::string_view primary) override {
        std::scoped_lock const lock{_regMtx};
        _assigned.emplace_back(mid.v, std::string{primary});
    }

    /// The (modelId, primary) pairs assignPrimary was asked to file, in order.
    [[nodiscard]] std::vector<std::pair<uint64_t, std::string>> assignments() const {
        std::scoped_lock const lock{_regMtx};
        return _assigned;
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
    std::vector<std::pair<uint64_t, std::string>> _assigned;
    uint64_t _nextId{100};
};

// Completes its async attach/bind callbacks *inline* -- synchronously, from
// inside attachModelAsync/registerModelSharedAsync itself, before the dispatch
// call returns. This is legal (nothing in IBackend forbids it) and it is what
// QtWebSocketBackend already does on its !_connected error branch, so
// Bridge::attachHandlerAsync/ensureBoundAsync must survive it: at that moment
// the Bridge is still holding _attachMtx around the dispatch, and anything the
// callback does that re-enters the Bridge under that lock -- publishing the
// binding's primary, or a result-keyed dispatch's assignHandlerPrimary --
// self-deadlocks unless the outcome is deferred out of the dispatch frame.
class InlineCompletingBackend : public AsyncRegisterBackend {
public:
    /// @param failInline When set, both methods report this message via onError
    ///        inline instead of succeeding.
    explicit InlineCompletingBackend(std::optional<std::string> failInline = std::nullopt)
        : _failInline{std::move(failInline)} {}

    bool registerModelSharedAsync(const std::string& typeId,
                                  std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                                  morph::backend::detail::InstanceIdentity /*identity*/,
                                  std::function<void(morph::exec::detail::ModelId)> onRegistered,
                                  std::function<void(const std::string&)> onError) override {
        completeInline(typeId, std::move(factory), onRegistered, onError);
        return true;
    }

    bool attachModelAsync(const std::string& typeId,
                          std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                          morph::backend::detail::InstanceIdentity /*identity*/,
                          morph::exec::detail::ModelId /*current*/,
                          std::function<void(morph::exec::detail::ModelId)> onRegistered,
                          std::function<void(const std::string&)> onError) override {
        completeInline(typeId, std::move(factory), onRegistered, onError);
        return true;
    }

private:
    void completeInline(const std::string& typeId,
                        std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                        const std::function<void(morph::exec::detail::ModelId)>& onRegistered,
                        const std::function<void(const std::string&)>& onError) {
        if (_failInline) {
            onError(*_failInline);
            return;
        }
        onRegistered(registerModel(typeId, std::move(factory)));
    }

    std::optional<std::string> _failInline;
};

// A backend whose async dispatch call itself throws synchronously, before
// returning -- e.g. QtWebSocketBackend::attachModelAsync's wire::encode()
// failing before send. Bridge::attachHandlerAsync/ensureBoundAsync must
// report this through onDone (matching execute()'s documented never-throws
// contract) rather than letting it escape.
class ThrowingDispatchBackend : public AsyncRegisterBackend {
public:
    bool attachModelAsync(const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>,
                          morph::backend::detail::InstanceIdentity, morph::exec::detail::ModelId,
                          std::function<void(morph::exec::detail::ModelId)>,
                          std::function<void(const std::string&)>) override {
        throw std::runtime_error("attachModelAsync dispatch failed");
    }

    bool registerModelSharedAsync(const std::string&,
                                  std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>,
                                  morph::backend::detail::InstanceIdentity,
                                  std::function<void(morph::exec::detail::ModelId)>,
                                  std::function<void(const std::string&)>) override {
        throw std::runtime_error("registerModelSharedAsync dispatch failed");
    }
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
    bool registerModelSharedAsync(const std::string& typeId,
                                  std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                                  morph::backend::detail::InstanceIdentity identity,
                                  std::function<void(morph::exec::detail::ModelId)> onRegistered,
                                  std::function<void(const std::string&)> onError) override {
        return _target->registerModelSharedAsync(typeId, std::move(factory), identity, std::move(onRegistered),
                                                  std::move(onError));
    }
    bool attachModelAsync(const std::string& typeId,
                          std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                          morph::backend::detail::InstanceIdentity identity, morph::exec::detail::ModelId current,
                          std::function<void(morph::exec::detail::ModelId)> onRegistered,
                          std::function<void(const std::string&)> onError) override {
        return _target->attachModelAsync(typeId, std::move(factory), identity, current, std::move(onRegistered),
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "Bridge::attachHandlerAsync: a stale async attach reply after switchBackend() does not clobber the new "
    "binding, and still resolves the caller's Completion",
    "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    auto asyncBackendA = std::make_shared<AsyncRegisterBackend>();
    morph::bridge::Bridge bridge{std::make_unique<AsyncBackendShim>(asyncBackendA)};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &cbExec};

    std::atomic<int> result{-1};
    std::atomic<bool> failed{false};
    auto pending = handler.execute(ARTouch{.id = 42, .amount = 5});
    pending.then([&](int val) { result.store(val); }).onError([&](const std::exception_ptr&) { failed.store(true); });

    // The attach was dispatched but has not replied yet.
    REQUIRE(asyncBackendA->pendingCount() == 1);
    CHECK(result.load() == -1);
    CHECK_FALSE(failed.load());

    // Switch away WHILE the attach on asyncBackendA is still outstanding. The
    // handler never attached (its primary is still empty), so switchBackend's
    // re-registration loop leaves it live-but-unbound on the new backend --
    // matching the `binding->shared && binding->primary.empty()` carry-over
    // path.
    auto secondBackend = std::make_unique<AsyncRegisterBackend>();
    auto* rawSecond = secondBackend.get();
    bridge.switchBackend(std::move(secondBackend));

    // The original (now-stale) attach reply from asyncBackendA finally
    // arrives. It must not be published into the binding as if it were a
    // valid id on the now-active backend -- and, unlike a fire-and-forget
    // re-registration, this caller's Completion is genuinely waiting on
    // `onDone`, so the stale reply must still resolve it (with an error)
    // rather than leaving it hanging forever.
    asyncBackendA->completeNext();
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1 || failed.load(); }));
    CHECK(result.load() == -1);
    CHECK(failed.load());
    CHECK_FALSE(handler.primary().has_value());

    // The handler is still usable against the now-active backend: a fresh
    // attach succeeds normally, proving the stale reply left no corruption
    // behind.
    std::atomic<int> secondResult{-1};
    handler.execute(ARTouch{.id = 42, .amount = 9})
        .then([&](int val) { secondResult.store(val); })
        .onError([&](const std::exception_ptr&) { failed.store(true); });
    REQUIRE(rawSecond->pendingCount() == 1);
    rawSecond->completeNext();
    REQUIRE(morph::testing::waitUntil([&] { return secondResult.load() != -1; }));
    CHECK(secondResult.load() == 9);
    CHECK(handler.primary().value_or(-1) == 42);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "Bridge::ensureBoundAsync: a stale async bind reply after switchBackend() does not clobber the new binding, "
    "and still resolves the caller's Completion",
    "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    auto asyncBackendA = std::make_shared<AsyncRegisterBackend>();
    morph::bridge::Bridge bridge{std::make_unique<AsyncBackendShim>(asyncBackendA)};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &cbExec};

    std::atomic<int> value{-1};
    std::atomic<bool> failed{false};
    auto pending = handler.execute(ARKeyedCreate{.initial = 11});
    pending.then([&](ARKeyedCreated res) { value.store(res.value); }).onError([&](const std::exception_ptr&) {
        failed.store(true);
    });

    REQUIRE(asyncBackendA->pendingCount() == 1);
    CHECK(value.load() == -1);
    CHECK_FALSE(failed.load());

    auto secondBackend = std::make_unique<AsyncRegisterBackend>();
    bridge.switchBackend(std::move(secondBackend));

    // The stale bind reply must not publish `currentId` from a backend
    // nothing uses any more, and must still resolve the waiting Completion.
    asyncBackendA->completeNext();
    REQUIRE(morph::testing::waitUntil([&] { return value.load() != -1 || failed.load(); }));
    CHECK(value.load() == -1);
    CHECK(failed.load());
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
    constexpr int kTrials = 200;
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

// ---------------------------------------------------------------------------
// Shared/keyed registration: registerModelSharedAsync + attachModelAsync.
//
// Same opt-in/fallback contract as registerModelAsync above, reached through
// Bridge::attachHandlerAsync (payload-keyed actions) and
// Bridge::ensureBoundAsync (result-keyed ones), both of which BridgeHandler's
// execute() now routes its keyed dispatches through. execute()'s own contract
// is unchanged: the attach/promote step never throws out of the call, it
// resolves the returned Completion.
// ---------------------------------------------------------------------------

using morph::bridge::AllowShared;

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Bridge prefers attachModelAsync over the synchronous attachModel when the backend offers it",
          "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};

    // An AllowShared handler registers nothing at construction -- it acquires
    // an instance only when a keyed action names one.
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};
    REQUIRE(rawBackend->pendingCount() == 0);

    std::atomic<int> result{-1};
    std::atomic<bool> failed{false};
    auto pending = handler.execute(ARTouch{.id = 42, .amount = 5});
    pending.then([&](int val) { result.store(val); }).onError([&](const std::exception_ptr&) { failed.store(true); });

    // The attach was dispatched but has not replied: execute() returned a
    // still-pending Completion rather than blocking in a nested wait, which is
    // the entire point on a WASM main thread.
    REQUIRE(rawBackend->pendingCount() == 1);
    CHECK(result.load() == -1);
    CHECK_FALSE(failed.load());

    rawBackend->completeNext();
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1; }));
    CHECK(result.load() == 5);
    CHECK_FALSE(failed.load());
    CHECK(handler.primary().value_or(-1) == 42);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("A backend with no async attach path falls back to the synchronous attachModel unchanged",
          "[bridge][registration][shared-instances][issue26]") {
    // LocalBackend overrides neither attachModelAsync nor
    // registerModelSharedAsync, so IBackend's defaults (returning false) apply
    // and the keyed execute() runs the identical synchronous attach it always
    // has -- bound before the dispatch, on this thread.
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

    std::atomic<int> result{-1};
    std::atomic<bool> failed{false};
    handler.execute(ARTouch{.id = 7, .amount = 3})
        .then([&](int val) { result.store(val); })
        .onError([&](const std::exception_ptr&) { failed.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1 || failed.load(); }));
    CHECK_FALSE(failed.load());
    CHECK(result.load() == 3);
    CHECK(handler.primary().value_or(-1) == 7);

    // A second keyed action on the same key is the idempotent-attach path, and
    // lands on the same instance (3 + 4), proving the fallback kept the
    // binding, not just the first reply.
    std::atomic<int> second{-1};
    handler.execute(ARTouch{.id = 7, .amount = 4})
        .then([&](int val) { second.store(val); })
        .onError([&](const std::exception_ptr&) { failed.store(true); });
    REQUIRE(morph::testing::waitUntil([&] { return second.load() != -1 || failed.load(); }));
    CHECK_FALSE(failed.load());
    CHECK(second.load() == 7);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "attachModelAsync's onError path surfaces through the returned Completion's onError, matching the synchronous "
    "path's documented contract",
    "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

    // execute() itself must not throw, whatever the attach does -- the failure
    // is a Completion outcome, not a synchronous exception.
    std::optional<morph::async::Completion<int>> pending;
    REQUIRE_NOTHROW(pending.emplace(handler.execute(ARTouch{.id = 99, .amount = 1})));

    std::string message;
    std::atomic<bool> succeeded{false};
    pending->then([&](int) { succeeded.store(true); }).onError([&](const std::exception_ptr& err) {
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            message = exc.what();
        }
    });

    REQUIRE(rawBackend->pendingCount() == 1);
    REQUIRE_NOTHROW(rawBackend->failNext("attach refused"));

    REQUIRE(morph::testing::waitUntil([&] { return !message.empty(); }));
    CHECK(message == "attach refused");
    CHECK_FALSE(succeeded.load());
    // The failed attach left the handler unattached, exactly as the
    // synchronous path's throwing attach does.
    CHECK_FALSE(handler.primary().has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("A backend that completes attachModelAsync inline does not deadlock and resolves normally",
          "[bridge][registration][shared-instances][issue26]") {
    // Regression guard for the inline-completion hole: attachHandlerAsync
    // dispatches under _attachMtx, and its success callback re-acquires that
    // lock to publish contextKey/primary. A callback that fires inline would
    // therefore re-enter a mutex this very frame holds. The dispatch frame must
    // park such an outcome and apply it after the lock is released instead.
    SyncExec cbExec;
    auto backend = std::make_unique<InlineCompletingBackend>();
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

    std::atomic<int> result{-1};
    std::atomic<bool> failed{false};
    // If the frame deadlocked, execute() never returns and this test hangs.
    handler.execute(ARTouch{.id = 8, .amount = 6})
        .then([&](int val) { result.store(val); })
        .onError([&](const std::exception_ptr&) { failed.store(true); });

    CHECK_FALSE(failed.load());
    CHECK(result.load() == 6);
    // The inline outcome was published exactly as an out-of-frame one would be:
    // primary() reads binding->primary under _attachMtx, which is also proof
    // the lock was released rather than left held by the dispatch frame.
    CHECK(handler.primary().value_or(-1) == 8);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("A backend that completes registerModelSharedAsync inline still promotes a result-keyed action",
          "[bridge][registration][shared-instances][issue26]") {
    // The sharpest form of the same hole: an inline bind runs onDone -- i.e.
    // the whole dispatch -- inside ensureBoundAsync's frame, and a result-keyed
    // dispatch's onResult calls assignHandlerPrimary, which takes _attachMtx.
    SyncExec cbExec;
    auto backend = std::make_unique<InlineCompletingBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

    std::atomic<int> value{-1};
    std::atomic<bool> failed{false};
    handler.execute(ARKeyedCreate{.initial = 17})
        .then([&](ARKeyedCreated res) { value.store(res.value); })
        .onError([&](const std::exception_ptr&) { failed.store(true); });

    CHECK_FALSE(failed.load());
    CHECK(value.load() == 17);
    CHECK(handler.primary().value_or(-1) == 4242);
    auto const assigned = rawBackend->assignments();
    REQUIRE(assigned.size() == 1);
    CHECK(assigned.front().second == "4242");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("A backend that reports its async attach failure inline surfaces it through onError, exactly once",
          "[bridge][registration][shared-instances][issue26]") {
    // QtWebSocketBackend's !_connected branch, in miniature: onError invoked
    // synchronously from inside attachModelAsync, which then returns true.
    SyncExec cbExec;
    auto backend = std::make_unique<InlineCompletingBackend>(std::optional<std::string>{"disconnected"});
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

    std::optional<morph::async::Completion<int>> pending;
    REQUIRE_NOTHROW(pending.emplace(handler.execute(ARTouch{.id = 3, .amount = 1})));

    std::string message;
    int errorCount = 0;
    std::atomic<bool> succeeded{false};
    pending->then([&](int) { succeeded.store(true); }).onError([&](const std::exception_ptr& err) {
        ++errorCount;
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            message = exc.what();
        }
    });

    CHECK(message == "disconnected");
    CHECK(errorCount == 1);
    CHECK_FALSE(succeeded.load());
    CHECK_FALSE(handler.primary().has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("ensureBoundAsync mirrors the same three cases for a result-keyed (creating) action",
          "[bridge][registration][shared-instances][issue26]") {
    SECTION("prefers registerModelSharedAsync when the backend offers it") {
        SyncExec cbExec;
        auto backend = std::make_unique<AsyncRegisterBackend>();
        auto* rawBackend = backend.get();
        morph::bridge::Bridge bridge{std::move(backend)};
        morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};
        REQUIRE(rawBackend->pendingCount() == 0);

        std::atomic<int> value{-1};
        std::atomic<bool> failed{false};
        auto pending = handler.execute(ARKeyedCreate{.initial = 11});
        pending.then([&](ARKeyedCreated res) { value.store(res.value); }).onError([&](const std::exception_ptr&) {
            failed.store(true);
        });

        // Bound asynchronously: still nothing resolved, nothing blocked.
        REQUIRE(rawBackend->pendingCount() == 1);
        CHECK(value.load() == -1);

        rawBackend->completeNext();
        REQUIRE(morph::testing::waitUntil([&] { return value.load() != -1; }));
        CHECK(value.load() == 11);
        CHECK_FALSE(failed.load());
        // The result-sourced key was adopted in place before the caller's
        // .then() saw the result.
        CHECK(handler.primary().value_or(-1) == 4242);
        auto const assigned = rawBackend->assignments();
        REQUIRE(assigned.size() == 1);
        CHECK(assigned.front().second == "4242");
    }

    SECTION("falls back to the synchronous registerModelShared when it does not") {
        morph::exec::ThreadPoolExecutor pool{2};
        SyncExec cbExec;
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
        morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

        std::atomic<int> value{-1};
        std::atomic<bool> failed{false};
        handler.execute(ARKeyedCreate{.initial = 23})
            .then([&](ARKeyedCreated res) { value.store(res.value); })
            .onError([&](const std::exception_ptr&) { failed.store(true); });

        REQUIRE(morph::testing::waitUntil([&] { return value.load() != -1 || failed.load(); }));
        CHECK_FALSE(failed.load());
        CHECK(value.load() == 23);
        CHECK(handler.primary().value_or(-1) == 4242);
    }

    SECTION("surfaces registerModelSharedAsync's onError through the returned Completion") {
        SyncExec cbExec;
        auto backend = std::make_unique<AsyncRegisterBackend>();
        auto* rawBackend = backend.get();
        morph::bridge::Bridge bridge{std::move(backend)};
        morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

        std::optional<morph::async::Completion<ARKeyedCreated>> pending;
        REQUIRE_NOTHROW(pending.emplace(handler.execute(ARKeyedCreate{.initial = 5})));

        std::string message;
        std::atomic<bool> succeeded{false};
        pending->then([&](ARKeyedCreated) { succeeded.store(true); }).onError([&](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& exc) {
                message = exc.what();
            }
        });

        REQUIRE(rawBackend->pendingCount() == 1);
        REQUIRE_NOTHROW(rawBackend->failNext("no capacity"));

        REQUIRE(morph::testing::waitUntil([&] { return !message.empty(); }));
        CHECK(message == "no capacity");
        CHECK_FALSE(succeeded.load());
        CHECK_FALSE(handler.primary().has_value());
    }
}

TEST_CASE("attachHandlerAsync reports a synchronously-throwing dispatch call through onDone",
          "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<ThrowingDispatchBackend>()};
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

    std::string message;
    std::atomic<bool> succeeded{false};
    REQUIRE_NOTHROW(handler.execute(ARTouch{.id = 9, .amount = 1})
                        .then([&](int) { succeeded.store(true); })
                        .onError([&](const std::exception_ptr& err) {
                            try {
                                std::rethrow_exception(err);
                            } catch (const std::exception& exc) {
                                message = exc.what();
                            }
                        }));

    CHECK(message == "attachModelAsync dispatch failed");
    CHECK_FALSE(succeeded.load());
    CHECK_FALSE(handler.primary().has_value());
}

TEST_CASE("ensureBoundAsync reports a synchronously-throwing dispatch call through onDone",
          "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<ThrowingDispatchBackend>()};
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

    std::string message;
    std::atomic<bool> succeeded{false};
    REQUIRE_NOTHROW(handler.execute(ARKeyedCreate{.initial = 3})
                        .then([&](ARKeyedCreated) { succeeded.store(true); })
                        .onError([&](const std::exception_ptr& err) {
                            try {
                                std::rethrow_exception(err);
                            } catch (const std::exception& exc) {
                                message = exc.what();
                            }
                        }));

    CHECK(message == "registerModelSharedAsync dispatch failed");
    CHECK_FALSE(succeeded.load());
    CHECK_FALSE(handler.primary().has_value());
}

TEST_CASE("attachHandlerAsync's out-of-frame success callback is a no-op once the Bridge is gone",
          "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    // The backend must outlive the Bridge for this test to complete the reply
    // after destroying it, so it is co-owned via AsyncBackendShim (the same
    // pattern test_bridge_lifetime.cpp uses) rather than owned solely by the
    // Bridge's unique_ptr.
    auto sharedBackend = std::make_shared<AsyncRegisterBackend>();
    auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<AsyncBackendShim>(sharedBackend));
    auto handler = std::make_unique<morph::bridge::BridgeHandler<ARKeyedModel, AllowShared>>(*bridge, &cbExec);

    REQUIRE_NOTHROW(handler->execute(ARTouch{.id = 11, .amount = 4}));
    REQUIRE(sharedBackend->pendingCount() == 1);

    // Destroy the handler and the Bridge itself before the deferred reply
    // lands: attachHandlerAsync's success callback holds only weak references
    // to both, so completing it now must be a quiet no-op rather than
    // dereferencing freed memory.
    handler.reset();
    bridge.reset();

    REQUIRE_NOTHROW(sharedBackend->completeNext());
    SUCCEED("completing an attach reply after the Bridge and handler are both gone did not crash");
}

TEST_CASE("attachHandlerAsync's out-of-frame success callback tolerates the BridgeHandler being gone",
          "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    auto handler = std::make_unique<morph::bridge::BridgeHandler<ARKeyedModel, AllowShared>>(bridge, &cbExec);

    REQUIRE_NOTHROW(handler->execute(ARTouch{.id = 12, .amount = 4}));
    REQUIRE(rawBackend->pendingCount() == 1);

    // Destroy only the handler; the Bridge itself stays alive. Note that the
    // binding itself does *not* actually go away here: execute()'s dispatch
    // copies `_binding` into a local held by this very completion's own
    // onDone closure (see BridgeHandler::execute), so the pending dispatch
    // keeps it alive independent of the BridgeHandler. This still exercises a
    // real case worth having a test for -- a caller that drops its handler
    // while a keyed attach is in flight must not crash when the reply lands.
    handler.reset();

    REQUIRE_NOTHROW(rawBackend->completeNext());
    SUCCEED("completing an attach reply after the BridgeHandler is gone did not crash");
}

TEST_CASE("ensureBoundAsync's out-of-frame success callback is a no-op once the Bridge is gone",
          "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    // See the identical attachHandlerAsync test above: the backend must
    // outlive the Bridge, so it is co-owned via AsyncBackendShim.
    auto sharedBackend = std::make_shared<AsyncRegisterBackend>();
    auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<AsyncBackendShim>(sharedBackend));
    auto handler = std::make_unique<morph::bridge::BridgeHandler<ARKeyedModel, AllowShared>>(*bridge, &cbExec);

    REQUIRE_NOTHROW(handler->execute(ARKeyedCreate{.initial = 6}));
    REQUIRE(sharedBackend->pendingCount() == 1);

    handler.reset();
    bridge.reset();

    REQUIRE_NOTHROW(sharedBackend->completeNext());
    SUCCEED("completing a registerModelSharedAsync reply after the Bridge and handler are both gone did not crash");
}

TEST_CASE("ensureBoundAsync's out-of-frame success callback tolerates the BridgeHandler being gone",
          "[bridge][registration][shared-instances][issue26]") {
    SyncExec cbExec;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* rawBackend = backend.get();
    morph::bridge::Bridge bridge{std::move(backend)};
    auto handler = std::make_unique<morph::bridge::BridgeHandler<ARKeyedModel, AllowShared>>(bridge, &cbExec);

    REQUIRE_NOTHROW(handler->execute(ARKeyedCreate{.initial = 7}));
    REQUIRE(rawBackend->pendingCount() == 1);

    // See attachHandlerAsync's identical test above: the binding itself stays
    // alive here (pinned by the pending dispatch's own onDone closure), but
    // dropping the handler while the reply is still in flight is still a real
    // case worth covering.
    handler.reset();

    REQUIRE_NOTHROW(rawBackend->completeNext());
    SUCCEED("completing a registerModelSharedAsync reply after the BridgeHandler is gone did not crash");
}

TEST_CASE("ensureBound is a no-op when the binding already has an instance",
          "[bridge][registration][shared-instances][issue26]") {
    // Bridge::ensureBound is the synchronous counterpart to ensureBoundAsync,
    // used directly (not through BridgeHandler::execute) when a caller wants
    // to force-bind an anonymous instance ahead of time. Calling it twice on
    // the same binding exercises its already-bound early-return: the second
    // call must not register a second instance.
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AR_KeyedModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ARKeyedModel>(); };

    REQUIRE_NOTHROW(bridge.ensureBound(binding));
    auto const firstId = binding->currentId.load();
    REQUIRE(firstId != 0U);

    REQUIRE_NOTHROW(bridge.ensureBound(binding));
    CHECK(binding->currentId.load() == firstId);
}

TEST_CASE("ensureBoundAsync's onError path is a no-op once the dispatching frame already claimed the outcome",
          "[bridge][registration][shared-instances][issue26]") {
    // Mirrors attachModelAsync's identical inline-failure test above, for
    // registerModelSharedAsync: onError invoked synchronously from inside the
    // dispatch call (which then returns true) exercises the parkIfInFrame
    // no-op inside ensureBoundAsync's error callback, not just its success one.
    SyncExec cbExec;
    auto backend = std::make_unique<InlineCompletingBackend>(std::optional<std::string>{"disconnected"});
    morph::bridge::Bridge bridge{std::move(backend)};
    morph::bridge::BridgeHandler<ARKeyedModel, AllowShared> handler{bridge, &cbExec};

    std::optional<morph::async::Completion<ARKeyedCreated>> pending;
    REQUIRE_NOTHROW(pending.emplace(handler.execute(ARKeyedCreate{.initial = 8})));

    std::string message;
    int errorCount = 0;
    std::atomic<bool> succeeded{false};
    pending->then([&](ARKeyedCreated) { succeeded.store(true); }).onError([&](const std::exception_ptr& err) {
        ++errorCount;
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            message = exc.what();
        }
    });

    CHECK(message == "disconnected");
    CHECK(errorCount == 1);
    CHECK_FALSE(succeeded.load());
    CHECK_FALSE(handler.primary().has_value());
}
