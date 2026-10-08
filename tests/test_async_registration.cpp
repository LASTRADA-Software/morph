// SPDX-License-Identifier: Apache-2.0
//
// The bind rule: a bind is a Completion delivered on the bridge's owner; a
// backend that can settles it before returning; a call made through a binding
// whose bind is in flight is held and dispatched when it settles. The doubles
// below settle their binds and promotes only when the test says so, the way a
// socket backend's reply arrives later on its own thread; the test is the
// bridge's owner and pumps it to apply what arrived.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <mutex>
#include <new>
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

// --- Keyed/shared coverage: the same deferred-reply idea applied to the
// --- register-or-attach and re-point shapes of the same bindModel request.

/// Names the instance it wants in the action payload -> payload-keyed, so
/// executing it attaches the handler first.
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
/// and promotes it once the reply names the key.
struct ARKeyedCreate {
    int initial = 0;
};

/// Payload-keyed, like ARTouch, but its ActionKeyTraits::key() below throws --
/// exercises execute()'s "key extraction is user code" guard (the try/catch
/// around ActionKeyTraits<Action>::key(action), ahead of the attach dispatch).
struct ARThrowingKeyTouch {
    int amount = 0;
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
    int execute(const ARThrowingKeyTouch& act) {
        value += act.amount;
        return value;
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

template <>
struct morph::model::ActionTraits<ARThrowingKeyTouch> {
    using Result = int;
    static constexpr std::string_view typeId() { return "AR_ThrowingKeyTouch"; }
    static std::string toJson(const ARThrowingKeyTouch& act) {
        return R"({"amount":)" + std::to_string(act.amount) + "}";
    }
    static ARThrowingKeyTouch fromJson(std::string_view /*json*/) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view text) { return std::stoi(std::string{text}); }
};

// Written directly rather than via BRIDGE_KEY_FROM: that macro's generated
// key() body cannot be made to throw, which is the entire point of this type.
template <>
struct morph::model::ActionKeyTraits<ARThrowingKeyTouch> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ARThrowingKeyTouch&) { throw std::runtime_error("key extraction failed"); }
};

BRIDGE_MODEL_KEY(ARKeyedModel, ARTouch, &ARTouch::id);
BRIDGE_KEY_FROM_RESULT(ARKeyedCreate, &ARKeyedCreated::id);

// ── assignHandlerPrimary goes through IBackend::promoteModel ──────────────
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

using ModelCompletion = morph::async::Completion<morph::exec::detail::ModelId>;

// Offers a non-blocking bind that does not complete until the test calls
// completeNext()/failNext() -- simulating a backend whose registration reply
// arrives later, asynchronously, instead of blocking the caller.
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
        state->setValue(call.localOp(*iter->second, call.action.get()));
        return comp;
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr&) override {}
    void setReconnectHandler(std::function<void()> /*handler*/, morph::exec::IExecutor* /*exec*/) override {}

    // One verb for all three acquire shapes (private, register-or-attach,
    // re-point): each is deferred the same way, so the reply lands in the same
    // queue completeNext()/failNext() drain and a keyed attach is observably
    // non-blocking for the same reason a plain registration is.
    ModelCompletion bindModel(morph::backend::detail::BindRequest request, morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = ModelCompletion::makeSettleable(&cbExec);
        queue(request.typeId, std::move(request.factory), std::move(promise));
        return std::move(completion);
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

protected:
    /// @brief Parks one bind request until completeNext()/failNext() settles it.
    void queue(std::string typeId, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
               ModelCompletion::Promise promise) {
        auto kept = std::make_shared<ModelCompletion::Promise>(std::move(promise));
        std::scoped_lock const lock{_pendingMtx};
        _pending.push_back(Pending{
            .typeId = std::move(typeId),
            .factory = std::move(factory),
            .onRegistered = [kept](morph::exec::detail::ModelId mid) { kept->resolve(mid); },
            .onError =
                [kept](const std::string& message) {
                    kept->reject(std::make_exception_ptr(std::runtime_error(message)));
                },
        });
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

// Settles its bind before `bindModel` returns -- the default's shape, and what
// `QtWebSocketBackend` does on its disconnected branch -- so the bridge applies
// the outcome in the same call.
class InlineCompletingBackend : public AsyncRegisterBackend {
public:
    /// @param failInline When set, the bind is rejected with this message
    ///        inline instead of succeeding.
    explicit InlineCompletingBackend(std::optional<std::string> failInline = std::nullopt)
        : _failInline{std::move(failInline)} {}

    ModelCompletion bindModel(morph::backend::detail::BindRequest request, morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = ModelCompletion::makeSettleable(&cbExec);
        if (_failInline) {
            promise.reject(std::make_exception_ptr(std::runtime_error(*_failInline)));
        } else {
            promise.resolve(registerModel(request.typeId, std::move(request.factory)));
        }
        return std::move(completion);
    }

private:
    std::optional<std::string> _failInline;
};

// A backend whose `bindModel` throws out of the call instead of rejecting --
// an out-of-tree override. The call it was made for is rejected with it.
class ThrowingDispatchBackend : public AsyncRegisterBackend {
public:
    ModelCompletion bindModel(morph::backend::detail::BindRequest request,
                              morph::exec::IExecutor& /*cbExec*/) override {
        if (!request.primary.empty()) {
            throw std::runtime_error("bindModel keyed dispatch failed");
        }
        throw std::runtime_error("bindModel anonymous dispatch failed");
    }
};

// Tries to settle the same bind twice. A Completion settles once, so the
// bridge sees one outcome.
class DoubleFiringBackend : public AsyncRegisterBackend {
public:
    ModelCompletion bindModel(morph::backend::detail::BindRequest request, morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = ModelCompletion::makeSettleable(&cbExec);
        auto mid = registerModel(request.typeId, std::move(request.factory));
        promise.resolve(mid);
        promise.resolve(mid);  // Contract violation: settles a second time inline.
        return std::move(completion);
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
    void setReconnectHandler(std::function<void()> handler, morph::exec::IExecutor* exec) override {
        _target->setReconnectHandler(std::move(handler), exec);
    }
    ModelCompletion bindModel(morph::backend::detail::BindRequest request, morph::exec::IExecutor& cbExec) override {
        return _target->bindModel(std::move(request), cbExec);
    }

private:
    std::shared_ptr<AsyncRegisterBackend> _target;
};

// Offers a non-blocking promote that does not complete until the test calls
// completeNext()/failNext() -- the assignHandlerPrimary counterpart of
// AsyncRegisterBackend above, simulating a backend (QtWebSocketBackend is the
// one real example) whose promote-in-place reply arrives later, on its own
// thread, instead of settling inside the promoteModel call. Everything else
// (registration, execute) is delegated to a real LocalBackend so a
// result-keyed action's ensureBound() step behaves normally; only the
// promotion step is deferred.
class AsyncAssignPrimaryBackend : public morph::backend::LocalBackend {
public:
    explicit AsyncAssignPrimaryBackend(morph::exec::IExecutor& pool) : LocalBackend{pool} {}

    ModelCompletion promoteModel(morph::backend::detail::PromoteRequest request,
                                 morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = ModelCompletion::makeSettleable(&cbExec);
        auto kept = std::make_shared<ModelCompletion::Promise>(std::move(promise));
        std::scoped_lock const lock{_pendingMtx};
        _pending.push_back(Pending{
            .mid = request.mid,
            .typeId = request.typeId,
            .primary = request.primary,
            .onRegistered = [kept](morph::exec::detail::ModelId mid) { kept->resolve(mid); },
            .onError =
                [kept](const std::string& message) {
                    kept->reject(std::make_exception_ptr(std::runtime_error(message)));
                },
        });
        return std::move(completion);
    }

    // Test hooks: settle the oldest still-pending promotion. Unlike
    // AsyncRegisterBackend::completeNext(), this does not also call the real
    // (synchronous) assignPrimary from the reply path by accident:
    // promoteModel's contract is that the backend performs the promotion
    // itself and merely reports back, so the test double's completion is the
    // promotion.
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

// A backend with no `bindModel` override, so `IBackend`'s default runs its
// synchronous verb -- which throws. The default turns that into a rejection.
class ThrowingSyncRegisterBackend : public morph::backend::detail::IBackend {
public:
    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>) override {
        return morph::exec::detail::ModelId{1};
    }
    void deregisterModel(morph::exec::detail::ModelId) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId,
                                                            morph::backend::detail::ActionCall,
                                                            morph::exec::IExecutor*) override {
        return {};
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr&) override {}

    morph::exec::detail::ModelId registerModelWithContext(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>,
        std::string_view) override {
        throw std::runtime_error("register failed synchronously");
    }
};

// A backend whose promoteModel defers, and which settles that promotion from
// inside its own destructor -- a backend torn down by switchBackend() while a
// promotion reply is in flight.
class SelfFiringAssignPrimaryBackend : public morph::backend::LocalBackend {
public:
    /// @p escapeSink must outlive the Bridge that owns this backend: the
    /// destructor writes to it, and the destructor runs from inside
    /// `switchBackend()`/`~Bridge`.
    SelfFiringAssignPrimaryBackend(morph::exec::IExecutor& pool, std::exception_ptr& escapeSink)
        : LocalBackend{pool}, _escapeSink{&escapeSink} {}

    SelfFiringAssignPrimaryBackend(const SelfFiringAssignPrimaryBackend&) = delete;
    SelfFiringAssignPrimaryBackend& operator=(const SelfFiringAssignPrimaryBackend&) = delete;
    SelfFiringAssignPrimaryBackend(SelfFiringAssignPrimaryBackend&&) = delete;
    SelfFiringAssignPrimaryBackend& operator=(SelfFiringAssignPrimaryBackend&&) = delete;

    // A destructor is implicitly noexcept, so anything the settle throws is
    // recorded for the test to check instead of terminating the binary.
    ~SelfFiringAssignPrimaryBackend() override {
        try {
            if (_pending) {
                auto pending = std::move(*_pending);
                _pending.reset();
                pending.promise->resolve(pending.mid);
            }
        } catch (...) {
            *_escapeSink = std::current_exception();
        }
    }

    ModelCompletion promoteModel(morph::backend::detail::PromoteRequest request,
                                 morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = ModelCompletion::makeSettleable(&cbExec);
        _pending =
            Pending{.mid = request.mid, .promise = std::make_shared<ModelCompletion::Promise>(std::move(promise))};
        return std::move(completion);
    }

private:
    struct Pending {
        morph::exec::detail::ModelId mid;
        std::shared_ptr<ModelCompletion::Promise> promise;
    };
    std::optional<Pending> _pending;
    std::exception_ptr* _escapeSink;
};

}  // namespace

namespace {

using Owner = morph::exec::MainThreadExecutor;
using morph::testing::pumpOwnerUntil;

/// Records the outcome of one call.
template <typename T>
struct Outcome {
    std::optional<T> value;
    std::optional<std::string> error;
    int settles = 0;

    void attach(morph::async::Completion<T>& completion) {
        completion.then([this](const T& got) {
            value = got;
            ++settles;
        });
        completion.onError([this](const std::exception_ptr& err) {
            ++settles;
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& exc) {
                error = exc.what();
            } catch (...) {
                error = "unknown";
            }
        });
    }
    [[nodiscard]] bool done() const { return settles > 0; }
};

template <typename T>
void track(Outcome<T>& outcome, morph::async::Completion<T> completion) {
    outcome.attach(completion);
}

}  // namespace

// ── Registration ─────────────────────────────────────────────────────────────

TEST_CASE("Bridge::registerHandler: a bind that settles later leaves the handler unbound, and a call is held",
          "[bridge][registration]") {
    Owner owner;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<ARModel> handler{bridge, &owner};
    REQUIRE_FALSE(handler.isBound());
    REQUIRE(async->pendingCount() == 1);

    Outcome<int> outcome;
    track(outcome, handler.execute(ARCount{.x = 7}));
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE_FALSE(outcome.done());

    async->completeNext();
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));
    REQUIRE(outcome.value == 7);
    REQUIRE(handler.isBound());
}

TEST_CASE("Bridge::registerHandler: a failed bind rejects the held call with the bind's error",
          "[bridge][registration]") {
    Owner owner;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<ARModel> handler{bridge, &owner};

    Outcome<int> outcome;
    track(outcome, handler.execute(ARCount{.x = 1}));
    async->failNext("server said no");
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));
    REQUIRE(outcome.error == "server said no");
    REQUIRE_FALSE(handler.isBound());
    REQUIRE(bridge.pendingCalls() == 0);

    // A later call through the same handler is rejected with the same error:
    // nothing is in flight to wait for.
    Outcome<int> again;
    track(again, handler.execute(ARCount{.x = 2}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return again.done(); }));
    REQUIRE(again.error == "server said no");
}

TEST_CASE("Bridge::registerHandler: a backend that settles before returning binds at once", "[bridge][registration]") {
    Owner owner;
    morph::bridge::Bridge bridge{std::make_unique<InlineCompletingBackend>(), owner};
    morph::bridge::BridgeHandler<ARModel> const handler{bridge, &owner};
    REQUIRE(handler.isBound());
}

TEST_CASE("Bridge::registerHandler: a bind reply superseded by switchBackend() is not applied",
          "[bridge][registration][switch]") {
    Owner owner;
    auto shared = std::make_shared<AsyncRegisterBackend>();
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<AsyncBackendShim>(shared), owner};
    morph::bridge::BridgeHandler<ARModel> const handler{bridge, &owner};
    REQUIRE(shared->pendingCount() == 1);

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool));
    REQUIRE(handler.isBound());
    auto const idOnLocal = handler.binding()->currentId.load();

    // The first backend's reply lands afterwards: the id the switch installed
    // stays.
    shared->completeNext();
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(handler.binding()->currentId.load() == idOnLocal);
}

TEST_CASE("Bridge::registerHandler: a bind reply arriving after ~Bridge() touches nothing",
          "[bridge][registration][lifetime]") {
    Owner owner;
    auto shared = std::make_shared<AsyncRegisterBackend>();
    {
        morph::bridge::Bridge bridge{std::make_unique<AsyncBackendShim>(shared), owner};
        auto binding = bridge.registerHandler<ARModel>();
        REQUIRE(shared->pendingCount() == 1);
    }
    shared->completeNext();
    owner.runFor(std::chrono::milliseconds{5});
    SUCCEED("a reply delivered on the owner after the bridge is gone is a no-op");
}

TEST_CASE("Bridge::registerHandler: a reply for a handler destroyed mid-bind is released, and a failure is dropped",
          "[bridge][registration][lifetime]") {
    Owner owner;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};

    Outcome<int> held;
    {
        morph::bridge::BridgeHandler<ARModel> first{bridge, &owner};
        morph::bridge::BridgeHandler<ARModel> const second{bridge, &owner};
        track(held, first.execute(ARCount{.x = 3}));
    }
    // The held call is rejected as the handler goes.
    REQUIRE(pumpOwnerUntil(owner, [&] { return held.done(); }));
    REQUIRE(held.error == std::string{morph::backend::HandlerDestroyedError{}.what()});

    async->completeNext();            // first's reply: its instance is released again
    async->failNext("late failure");  // second's: nobody left to tell
    owner.runFor(std::chrono::milliseconds{5});
    Outcome<std::shared_ptr<void>> probe;
    track(probe, async->execute(morph::exec::detail::ModelId{100}, morph::backend::detail::ActionCall{}, &owner));
    REQUIRE(pumpOwnerUntil(owner, [&] { return probe.done(); }));
    REQUIRE(probe.error == "no such model");
}

TEST_CASE("Bridge::registerHandler: the bind reply is applied inside an owner task", "[bridge][registration][owner]") {
    Owner owner;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<ARModel> handler{bridge, &owner};

    morph::testing::OwnerProbeRecorder const recorder{owner.coreExecutor()};
    std::thread settler{[async] { async->completeNext(); }};
    settler.join();
    REQUIRE(pumpOwnerUntil(owner, [&] { return handler.isBound(); }));
    REQUIRE(recorder.allPosted("Bridge::applyBind"));
}

// ── Keyed attach and result-keyed creation ───────────────────────────────────

TEST_CASE("BridgeHandler::execute: a payload-keyed action waits for its attach and then dispatches",
          "[bridge][registration][shared-instances]") {
    Owner owner;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};
    REQUIRE(async->pendingCount() == 0);  // a shared handler binds nothing up front

    Outcome<int> first;
    Outcome<int> second;
    track(first, handler.execute(ARTouch{.id = 5, .amount = 2}));
    track(second, handler.execute(ARTouch{.id = 5, .amount = 3}));
    // One attach for the key: the second call waits behind the first's bind.
    REQUIRE(async->pendingCount() == 1);

    async->completeNext();
    REQUIRE(pumpOwnerUntil(owner, [&] { return first.done() && second.done(); }));
    REQUIRE(first.value == 2);
    REQUIRE(second.value == 5);
    REQUIRE(handler.primary() == 5);
}

TEST_CASE("BridgeHandler::execute: a refused attach rejects the call through onError",
          "[bridge][registration][shared-instances]") {
    Owner owner;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<int> outcome;
    REQUIRE_NOTHROW(track(outcome, handler.execute(ARTouch{.id = 5, .amount = 2})));
    async->failNext("attach refused");
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));
    REQUIRE(outcome.error == "attach refused");
    REQUIRE_FALSE(handler.primary().has_value());
}

TEST_CASE("BridgeHandler::execute: a result-keyed action binds an anonymous instance, then promotes it",
          "[bridge][registration][shared-instances]") {
    Owner owner;
    auto backend = std::make_unique<AsyncRegisterBackend>();
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<ARKeyedCreated> outcome;
    track(outcome, handler.execute(ARKeyedCreate{.initial = 9}));
    REQUIRE(async->pendingCount() == 1);
    async->completeNext();
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));
    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->id == 4242);
    // The promotion settled inside promoteModel's default, before the caller's
    // `.then` ran.
    REQUIRE(handler.primary() == 4242);
    REQUIRE(async->assignments() ==
            std::vector<std::pair<uint64_t, std::string>>{{handler.binding()->currentId.load(), "4242"}});
}

TEST_CASE("BridgeHandler::execute: keyed actions over a backend that settles before returning",
          "[bridge][registration][shared-instances]") {
    Owner owner;
    morph::bridge::Bridge bridge{std::make_unique<InlineCompletingBackend>(), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<int> touched;
    track(touched, handler.execute(ARTouch{.id = 3, .amount = 4}));
    Outcome<ARKeyedCreated> created;
    track(created, handler.execute(ARKeyedCreate{.initial = 1}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return touched.done() && created.done(); }));
    REQUIRE(touched.value == 4);
    REQUIRE(created.value.has_value());
}

TEST_CASE("BridgeHandler::execute: an attach refused before returning is reported exactly once",
          "[bridge][registration][shared-instances]") {
    Owner owner;
    morph::bridge::Bridge bridge{std::make_unique<InlineCompletingBackend>("refused inline"), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<int> outcome;
    track(outcome, handler.execute(ARTouch{.id = 3, .amount = 4}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(outcome.settles == 1);
    REQUIRE(outcome.error == "refused inline");
}

TEST_CASE("BridgeHandler::execute: a bindModel that throws out of the call rejects the call",
          "[bridge][registration][shared-instances]") {
    Owner owner;
    morph::bridge::Bridge bridge{std::make_unique<ThrowingDispatchBackend>(), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<int> attached;
    track(attached, handler.execute(ARTouch{.id = 3, .amount = 4}));
    Outcome<ARKeyedCreated> created;
    track(created, handler.execute(ARKeyedCreate{.initial = 1}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return attached.done() && created.done(); }));
    REQUIRE(attached.error == "bindModel keyed dispatch failed");
    REQUIRE(created.error == "bindModel anonymous dispatch failed");
}

TEST_CASE("BridgeHandler::execute: a throwing key extraction is reported through onError",
          "[bridge][registration][shared-instances]") {
    Owner owner;
    morph::bridge::Bridge bridge{std::make_unique<InlineCompletingBackend>(), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<int> outcome;
    REQUIRE_NOTHROW(track(outcome, handler.execute(ARThrowingKeyTouch{.amount = 1})));
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));
    REQUIRE(outcome.error == "key extraction failed");
}

TEST_CASE("BridgeHandler::execute: a backend settling a bind twice is seen once", "[bridge][registration]") {
    Owner owner;
    morph::bridge::Bridge bridge{std::make_unique<DoubleFiringBackend>(), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<int> outcome;
    track(outcome, handler.execute(ARTouch{.id = 1, .amount = 1}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(outcome.settles == 1);
}

TEST_CASE("BridgeHandler::execute: IBackend's default bind turns a throwing verb into a rejection",
          "[bridge][registration]") {
    Owner owner;
    morph::bridge::Bridge bridge{std::make_unique<ThrowingSyncRegisterBackend>(), owner};
    morph::bridge::BridgeHandler<ARKeyedModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<ARKeyedCreated> outcome;
    track(outcome, handler.execute(ARKeyedCreate{.initial = 1}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));
    REQUIRE(outcome.error == "register failed synchronously");
}

// ── Promotion (assignHandlerPrimary) ─────────────────────────────────────────

TEST_CASE("Bridge::assignHandlerPrimary: a promotion that settles later is published when it does",
          "[bridge][registration][promote]") {
    Owner owner;
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<AsyncAssignPrimaryBackend>(pool);
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<ARCreated> outcome;
    track(outcome, handler.execute(ARCreate{.initial = 3}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return async->pendingCount() == 1; }));
    REQUIRE_FALSE(handler.primary().has_value());

    morph::testing::OwnerProbeRecorder const recorder{owner.coreExecutor()};
    std::thread settler{[async] { async->completeNext(); }};
    settler.join();
    REQUIRE(pumpOwnerUntil(owner, [&] { return handler.primary().has_value(); }));
    REQUIRE(recorder.allPosted("Bridge::assignHandlerPrimary"));
}

TEST_CASE("Bridge::assignHandlerPrimary: a failed promotion leaves the binding unpromoted",
          "[bridge][registration][promote]") {
    Owner owner;
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<AsyncAssignPrimaryBackend>(pool);
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<ARCreated> outcome;
    track(outcome, handler.execute(ARCreate{.initial = 3}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return async->pendingCount() == 1; }));
    async->failNext("promotion refused");
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE_FALSE(handler.primary().has_value());
}

TEST_CASE("Bridge::assignHandlerPrimary: a promotion superseded by switchBackend() is ignored",
          "[bridge][registration][promote][switch]") {
    Owner owner;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    auto async = std::make_shared<AsyncAssignPrimaryBackend>(pool);
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool2), owner};
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(async));
    morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<ARCreated> outcome;
    track(outcome, handler.execute(ARCreate{.initial = 3}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return async->pendingCount() == 1; }));

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));
    async->completeNext();
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE_FALSE(handler.primary().has_value());
}

TEST_CASE("Bridge::assignHandlerPrimary: a promotion reply after the handler or the bridge is gone is a no-op",
          "[bridge][registration][promote][lifetime]") {
    Owner owner;
    morph::exec::ThreadPoolExecutor pool{2};
    auto async = std::make_shared<AsyncAssignPrimaryBackend>(pool);
    // The outcomes outlive every pump below: their handlers are not
    // scope-gated, so a reply settling inside a later runFor() is delivered
    // into them after the handler or the bridge that made the call is gone.
    Outcome<ARCreated> afterHandler;
    Outcome<ARCreated> afterBridge;
    {
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
        bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(async));
        {
            morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &owner};
            track(afterHandler, handler.execute(ARCreate{.initial = 3}));
            REQUIRE(pumpOwnerUntil(owner, [&] { return async->pendingCount() == 1; }));
        }
        async->completeNext();  // the handler is gone
        owner.runFor(std::chrono::milliseconds{5});
        morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &owner};
        track(afterBridge, handler.execute(ARCreate{.initial = 4}));
        REQUIRE(pumpOwnerUntil(owner, [&] { return async->pendingCount() == 1; }));
    }
    async->completeNext();  // the bridge is gone
    owner.runFor(std::chrono::milliseconds{5});
    SUCCEED("late promotion replies touched nothing");
}

TEST_CASE("Bridge::assignHandlerPrimary: an unbound binding, an empty key, or a keyed binding is left alone",
          "[bridge][registration][promote]") {
    Owner owner;
    morph::exec::ThreadPoolExecutor pool{2};
    auto backend = std::make_unique<AsyncAssignPrimaryBackend>(pool);
    auto* async = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};

    auto unbound = bridge.registerSharedHandler<ARCreateModel>();
    bridge.assignHandlerPrimary<ARCreateModel>(unbound, "1");
    REQUIRE(async->pendingCount() == 0);

    auto keyed = bridge.registerSharedHandler<ARCreateModel>();
    bridge.attachHandler<ARCreateModel>(keyed, "7");
    REQUIRE(keyed->primary == "7");
    bridge.assignHandlerPrimary<ARCreateModel>(keyed, "");
    bridge.assignHandlerPrimary<ARCreateModel>(keyed, "8");
    REQUIRE(async->pendingCount() == 0);
    REQUIRE(keyed->primary == "7");
}

TEST_CASE("Bridge::switchBackend: a backend that settles its promotion from its own destructor does not escape",
          "[bridge][registration][promote][switch]") {
    Owner owner;
    morph::exec::ThreadPoolExecutor pool{2};
    std::exception_ptr escaped;
    morph::bridge::Bridge bridge{std::make_unique<SelfFiringAssignPrimaryBackend>(pool, escaped), owner};
    morph::bridge::BridgeHandler<ARCreateModel, morph::bridge::AllowShared> handler{bridge, &owner};

    Outcome<ARCreated> outcome;
    track(outcome, handler.execute(ARCreate{.initial = 3}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return outcome.done(); }));

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool));
    owner.runFor(std::chrono::milliseconds{5});
    CHECK_FALSE(escaped);
    CHECK_FALSE(handler.primary().has_value());
}
