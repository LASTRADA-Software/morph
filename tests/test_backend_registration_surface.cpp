// SPDX-License-Identifier: Apache-2.0
//
// Coverage for issue #567: IBackend's structural registration surface
// (`bindModel`/`promoteModel`, `BindRequest`/`PromoteRequest`) and
// `SynchronousBackendAdapter`.
//
// The claim under test is not "registration works" -- the legacy verbs already
// covered that. It is that the two things the four `*Async` twins could only
// state in prose are now properties of the signature:
//
//   1. The continuation is not optional, so no call site carries a fallback.
//   2. The continuation is delivered on the executor the *caller* named, never
//      on whichever thread the backend happened to settle on. That is the
//      threading contract `backend.hpp`'s `@note` block could only assert and
//      nothing could check.
//
// Every delivery assertion below is written so it would fail if delivery went
// inline on the settling thread: the caller's executor is a
// `MainThreadExecutor` that runs nothing until this thread drains it, and the
// backend settles from a different thread, asserted to be a different thread.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/strand.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

using morph::backend::SynchronousBackendAdapter;
using morph::backend::detail::BindRequest;
using morph::backend::detail::IBackend;
using morph::backend::detail::InstanceIdentity;
using morph::backend::detail::PromoteRequest;
using morph::exec::detail::ModelId;
using ModelCompletion = morph::async::Completion<ModelId>;

constexpr std::string_view kTypeId = "RS_Model";

struct RegistrationSurfaceModel {
    int value = 0;
};

template <>
struct morph::model::ModelTraits<RegistrationSurfaceModel> {
    static constexpr std::string_view typeId() { return kTypeId; }
};

namespace {

/// @brief Records which legacy verb the default `bindModel` reached for.
///
/// Every verb returns a distinct `ModelId`, so the resolved value alone
/// identifies the branch taken even without reading `calls`.
struct RecordingBackend : IBackend {
    std::vector<std::string> calls;

    ModelId registerModel(const std::string& /*typeId*/,
                          std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/) override {
        calls.emplace_back("registerModel");
        return ModelId{1};
    }

    ModelId registerModelWithContext(const std::string& /*typeId*/,
                                     std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                                     std::string_view contextKey) override {
        calls.emplace_back("registerModelWithContext:" + std::string{contextKey});
        return ModelId{2};
    }

    ModelId registerModelShared(const std::string& /*typeId*/,
                                std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                                InstanceIdentity identity) override {
        calls.emplace_back("registerModelShared:" + std::string{identity.primary});
        return ModelId{3};
    }

    ModelId attachModel(const std::string& /*typeId*/,
                        std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                        InstanceIdentity identity, ModelId current) override {
        calls.emplace_back("attachModel:" + std::string{identity.primary} + ":" + std::to_string(current.v));
        return ModelId{4};
    }

    void assignPrimary(ModelId mid, const std::string& /*typeId*/, std::string_view primary) override {
        calls.emplace_back("assignPrimary:" + std::to_string(mid.v) + ":" + std::string{primary});
    }

    // The four legacy `*Async` twins return `true` here — the opposite of
    // `IBackend`'s default — so that a forwarding assertion cannot pass
    // vacuously: if `SynchronousBackendAdapter` stopped forwarding one, the
    // inherited default would answer `false` and the test would fail.
    bool registerModelAsync(const std::string& /*typeId*/,
                            std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                            std::string_view contextKey, std::function<void(ModelId)> /*onRegistered*/,
                            std::function<void(const std::string&)> /*onError*/) override {
        calls.emplace_back("registerModelAsync:" + std::string{contextKey});
        return true;
    }

    // NOLINTBEGIN(performance-unnecessary-value-param) — the overridden signatures take these by value.
    bool registerModelSharedAsync(const std::string& /*typeId*/,
                                  std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                                  InstanceIdentity identity, std::function<void(ModelId)> /*onRegistered*/,
                                  std::function<void(const std::string&)> /*onError*/) override {
        calls.emplace_back("registerModelSharedAsync:" + std::string{identity.primary});
        return true;
    }

    bool attachModelAsync(const std::string& /*typeId*/,
                          std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                          InstanceIdentity identity, ModelId current, std::function<void(ModelId)> /*onRegistered*/,
                          std::function<void(const std::string&)> /*onError*/) override {
        calls.emplace_back("attachModelAsync:" + std::string{identity.primary} + ":" + std::to_string(current.v));
        return true;
    }
    // NOLINTEND(performance-unnecessary-value-param)

    bool assignPrimaryAsync(ModelId mid, const std::string& /*typeId*/, std::string_view primary,
                            std::function<void(ModelId)> /*onRegistered*/,
                            std::function<void(const std::string&)> /*onError*/) override {
        calls.emplace_back("assignPrimaryAsync:" + std::to_string(mid.v) + ":" + std::string{primary});
        return true;
    }

    std::vector<std::string> listInstances(const std::string& /*typeId*/) override {
        calls.emplace_back("listInstances");
        return {"listed"};
    }

    void deregisterModel(ModelId mid) override { calls.emplace_back("deregisterModel:" + std::to_string(mid.v)); }

    morph::async::Completion<std::shared_ptr<void>> execute(ModelId mid, morph::backend::detail::ActionCall /*call*/,
                                                            morph::exec::IExecutor* /*cbExec*/) override {
        calls.emplace_back("execute:" + std::to_string(mid.v));
        return {};
    }

    void notifyBackendChanged() override { calls.emplace_back("notifyBackendChanged"); }

    void cancelPending(const std::exception_ptr& /*exc*/) override { calls.emplace_back("cancelPending"); }

    void setReconnectHandler(const std::function<void()>& /*handler*/) override {
        calls.emplace_back("setReconnectHandler");
    }

    void setConnectHandler(const std::function<void()>& /*handler*/) override {
        calls.emplace_back("setConnectHandler");
    }

    void setDisconnectHandler(const std::function<void()>& /*handler*/) override {
        calls.emplace_back("setDisconnectHandler");
    }

    void setSession(morph::session::Context session) override {
        calls.emplace_back("setSession:" + session.principal);
    }
};

/// @brief A backend whose registration genuinely never blocks — the shape
///        morph#568 moves `QtWebSocketBackend` onto.
///
/// `bindModel` stores the promise and returns; the reply is delivered later,
/// from whatever thread the transport happens to use.
struct DeferredBackend : RecordingBackend {
    std::shared_ptr<ModelCompletion::Promise> pending;

    ModelCompletion bindModel(BindRequest /*request*/, morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = ModelCompletion::makeSettleable(&cbExec);
        pending = std::make_shared<ModelCompletion::Promise>(std::move(promise));
        return std::move(completion);
    }
};

/// @brief A backend whose synchronous control calls fail.
struct ThrowingBackend : RecordingBackend {
    ModelId registerModelWithContext(const std::string& /*typeId*/,
                                     std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                                     std::string_view /*contextKey*/) override {
        throw std::runtime_error{"register refused"};
    }

    void assignPrimary(ModelId /*mid*/, const std::string& /*typeId*/, std::string_view /*primary*/) override {
        throw std::runtime_error{"promote refused"};
    }
};

/// @brief Collects the message a rejected `Completion` carries.
/// @return A handler suitable for `onErrorDetached`.
auto captureError(std::string& message, std::atomic<bool>& done) {
    return [&message, &done](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& err) {
            message = err.what();
        }
        done.store(true);
    };
}

std::unique_ptr<morph::model::detail::IModelHolder> makeHolder() {
    return morph::model::detail::ModelFactory::create<RegistrationSurfaceModel>();
}

/// @brief Drains @p exec on the calling thread until @p done, or gives up.
/// @return `true` if @p done became true within the polling budget.
bool drainUntil(morph::exec::MainThreadExecutor& exec, const std::atomic<bool>& done) {
    return morph::testing::waitUntil([&] {
        exec.runOnce();
        return done.load();
    });
}

}  // namespace

// ── The surface's shape: one verb where there were three ─────────────────────

TEST_CASE("morph::backend::IBackend: bindModel's default routes each request shape to the verb it replaces",
          "[backend][registration-surface]") {
    RecordingBackend backend;
    morph::exec::MainThreadExecutor callerExec;

    ModelId bound{};
    std::atomic<bool> done{false};
    auto observe = [&](ModelId mid) {
        bound = mid;
        done.store(true);
    };

    SECTION("empty primary, no current instance -> registerModelWithContext") {
        auto completion = backend.bindModel(
            BindRequest{.typeId = std::string{kTypeId}, .factory = makeHolder, .contextKey = "acct-7", .primary = {}},
            callerExec);
        completion.thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(bound == ModelId{2});
        REQUIRE(backend.calls == std::vector<std::string>{"registerModelWithContext:acct-7"});
    }

    SECTION("non-empty primary, no current instance -> registerModelShared") {
        auto completion = backend.bindModel(
            BindRequest{.typeId = std::string{kTypeId}, .factory = makeHolder, .contextKey = "k-1", .primary = "k-1"},
            callerExec);
        completion.thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(bound == ModelId{3});
        REQUIRE(backend.calls == std::vector<std::string>{"registerModelShared:k-1"});
    }

    SECTION("non-empty primary plus a current instance -> attachModel") {
        auto completion = backend.bindModel(BindRequest{.typeId = std::string{kTypeId},
                                                        .factory = makeHolder,
                                                        .contextKey = "k-2",
                                                        .primary = "k-2",
                                                        .current = ModelId{9}},
                                            callerExec);
        completion.thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(bound == ModelId{4});
        REQUIRE(backend.calls == std::vector<std::string>{"attachModel:k-2:9"});
    }

    SECTION("promoteModel reaches assignPrimary and echoes the id back") {
        auto completion = backend.promoteModel(
            PromoteRequest{.mid = ModelId{11}, .typeId = std::string{kTypeId}, .primary = "k-3"}, callerExec);
        completion.thenDetached(observe);
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(bound == ModelId{11});
        REQUIRE(backend.calls == std::vector<std::string>{"assignPrimary:11:k-3"});
    }
}

TEST_CASE(
    "morph::backend::IBackend: a failing control call rejects the completion instead of throwing at the call "
    "site",
    "[backend][registration-surface]") {
    ThrowingBackend backend;
    morph::exec::MainThreadExecutor callerExec;

    std::string message;
    std::atomic<bool> done{false};

    SECTION("bind") {
        auto completion = backend.bindModel(
            BindRequest{.typeId = std::string{kTypeId}, .factory = makeHolder, .contextKey = {}, .primary = {}},
            callerExec);
        completion.onErrorDetached(captureError(message, done));
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(message == "register refused");
    }

    SECTION("promote") {
        auto completion = backend.promoteModel(
            PromoteRequest{.mid = ModelId{3}, .typeId = std::string{kTypeId}, .primary = "k"}, callerExec);
        completion.onErrorDetached(captureError(message, done));
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(message == "promote refused");
    }
}

TEST_CASE(
    "morph::backend::SynchronousBackendAdapter: a failure inside the wrapped backend reaches the caller's "
    "executor as a rejection",
    "[backend][registration-surface]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor callerExec;
    auto throwing = std::make_shared<ThrowingBackend>();
    SynchronousBackendAdapter adapter{throwing, pool};

    std::string message;
    std::atomic<bool> done{false};

    SECTION("bind") {
        auto completion = adapter.bindModel(
            BindRequest{.typeId = std::string{kTypeId}, .factory = makeHolder, .contextKey = {}, .primary = {}},
            callerExec);
        completion.onErrorDetached(captureError(message, done));
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(message == "register refused");
    }

    SECTION("promote") {
        auto completion = adapter.promoteModel(
            PromoteRequest{.mid = ModelId{3}, .typeId = std::string{kTypeId}, .primary = "k"}, callerExec);
        completion.onErrorDetached(captureError(message, done));
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(message == "promote refused");
    }
}

// ── The threading contract, as a property of the signature ───────────────────

TEST_CASE(
    "morph::backend::SynchronousBackendAdapter: the continuation is delivered on the caller's executor, never the "
    "thread the backend settled on",
    "[backend][registration-surface][threading]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor callerExec;
    auto local = std::make_shared<morph::backend::LocalBackend>(pool);
    SynchronousBackendAdapter adapter{local, pool};

    std::atomic<bool> factoryRan{false};
    std::thread::id factoryThread{};
    std::atomic<bool> delivered{false};
    std::thread::id deliveryThread{};
    ModelId bound{};

    auto factory = [&]() -> std::unique_ptr<morph::model::detail::IModelHolder> {
        factoryThread = std::this_thread::get_id();
        factoryRan.store(true);
        return makeHolder();
    };

    auto completion = adapter.bindModel(
        BindRequest{.typeId = std::string{kTypeId}, .factory = factory, .contextKey = {}, .primary = {}}, callerExec);
    completion.thenDetached([&](ModelId mid) {
        deliveryThread = std::this_thread::get_id();
        bound = mid;
        delivered.store(true);
    });

    // The registration itself has completed, on the pool thread.
    REQUIRE(morph::testing::waitUntil([&] { return factoryRan.load(); }));
    REQUIRE(factoryThread != std::this_thread::get_id());

    // ...and yet nothing has been delivered, because the only thing that runs
    // this completion's handler is this thread draining `callerExec`. An
    // implementation that invoked the handler on the settling thread would have
    // set `delivered` by now.
    REQUIRE_FALSE(delivered.load());

    REQUIRE(drainUntil(callerExec, delivered));
    REQUIRE(deliveryThread == std::this_thread::get_id());
    REQUIRE(bound.v != 0U);
}

TEST_CASE(
    "morph::backend::IBackend: a natively non-blocking backend's reply is delivered on the caller's executor too",
    "[backend][registration-surface][threading]") {
    DeferredBackend backend;
    morph::exec::MainThreadExecutor callerExec;

    std::atomic<bool> delivered{false};
    std::thread::id deliveryThread{};
    ModelId bound{};

    auto completion = backend.bindModel(
        BindRequest{.typeId = std::string{kTypeId}, .factory = makeHolder, .contextKey = {}, .primary = {}},
        callerExec);
    completion.thenDetached([&](ModelId mid) {
        deliveryThread = std::this_thread::get_id();
        bound = mid;
        delivered.store(true);
    });

    // No blocking call ever ran: the backend simply kept the promise.
    REQUIRE(backend.calls.empty());
    REQUIRE_FALSE(delivered.load());

    // The "reply" arrives on a transport thread that is not the caller's.
    std::thread replier{[&] { backend.pending->resolve(ModelId{77}); }};
    replier.join();
    REQUIRE_FALSE(delivered.load());

    REQUIRE(drainUntil(callerExec, delivered));
    REQUIRE(deliveryThread == std::this_thread::get_id());
    REQUIRE(bound == ModelId{77});
}

// ── The adapter, against a real blocking backend ─────────────────────────────

TEST_CASE(
    "morph::backend::SynchronousBackendAdapter: bindModel returns before the wrapped backend's blocking call "
    "finishes",
    "[backend][registration-surface][threading]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor callerExec;
    auto local = std::make_shared<morph::backend::LocalBackend>(pool);
    SynchronousBackendAdapter adapter{local, pool};

    std::mutex mtx;
    std::condition_variable gate;
    bool dispatchReturned = false;
    std::atomic<bool> callerWasFreeWhileBlocked{false};
    std::atomic<bool> delivered{false};

    auto factory = [&]() -> std::unique_ptr<morph::model::detail::IModelHolder> {
        std::unique_lock lock{mtx};
        // Hold the wrapped backend's registration open until the caller has
        // returned from `bindModel`. If the adapter ran the blocking call
        // inline, this wait can only time out -- nothing can set the flag.
        callerWasFreeWhileBlocked.store(
            gate.wait_for(lock, morph::testing::kDefaultWaitBudget, [&] { return dispatchReturned; }));
        return makeHolder();
    };

    auto completion = adapter.bindModel(
        BindRequest{.typeId = std::string{kTypeId}, .factory = factory, .contextKey = {}, .primary = {}}, callerExec);
    {
        std::scoped_lock const lock{mtx};
        dispatchReturned = true;
    }
    gate.notify_all();

    completion.thenDetached([&](ModelId /*mid*/) { delivered.store(true); });
    REQUIRE(drainUntil(callerExec, delivered));
    REQUIRE(callerWasFreeWhileBlocked.load());
}

TEST_CASE(
    "morph::backend::SynchronousBackendAdapter: wraps LocalBackend unmodified and carries shared, re-point and "
    "promote binds",
    "[backend][registration-surface]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor callerExec;
    auto local = std::make_shared<morph::backend::LocalBackend>(pool);
    SynchronousBackendAdapter adapter{local, pool};

    auto bind = [&](const std::string& primary, ModelId current) {
        ModelId out{};
        std::atomic<bool> done{false};
        auto completion = adapter.bindModel(BindRequest{.typeId = std::string{kTypeId},
                                                        .factory = makeHolder,
                                                        .contextKey = primary,
                                                        .primary = primary,
                                                        .current = current},
                                            callerExec);
        completion.thenDetached([&](ModelId mid) {
            out = mid;
            done.store(true);
        });
        REQUIRE(drainUntil(callerExec, done));
        return out;
    };

    SECTION("two binds on the same key share one instance") {
        auto const first = bind("alice", ModelId{});
        auto const second = bind("alice", ModelId{});
        REQUIRE(first.v != 0U);
        REQUIRE(first == second);
        REQUIRE(local->listInstances(std::string{kTypeId}) == std::vector<std::string>{"alice"});
    }

    SECTION("a re-point releases the key it came from") {
        auto const first = bind("alice", ModelId{});
        auto const second = bind("bob", first);
        REQUIRE(second.v != 0U);
        REQUIRE(second != first);
        REQUIRE(local->listInstances(std::string{kTypeId}) == std::vector<std::string>{"bob"});
    }

    SECTION("promoteModel files a private instance under a key") {
        auto const priv = bind("", ModelId{});
        REQUIRE(local->listInstances(std::string{kTypeId}).empty());

        ModelId echoed{};
        std::atomic<bool> done{false};
        auto completion = adapter.promoteModel(
            PromoteRequest{.mid = priv, .typeId = std::string{kTypeId}, .primary = "carol"}, callerExec);
        completion.thenDetached([&](ModelId mid) {
            echoed = mid;
            done.store(true);
        });
        REQUIRE(drainUntil(callerExec, done));
        REQUIRE(echoed == priv);
        REQUIRE(local->listInstances(std::string{kTypeId}) == std::vector<std::string>{"carol"});
    }
}

TEST_CASE(
    "morph::backend::SynchronousBackendAdapter: rejects a null backend and forwards the legacy verbs it does "
    "not reshape",
    "[backend][registration-surface]") {
    morph::exec::ThreadPoolExecutor pool{1};
    auto recording = std::make_shared<RecordingBackend>();
    SynchronousBackendAdapter adapter{recording, pool};

    REQUIRE_THROWS_AS((SynchronousBackendAdapter{nullptr, pool}), std::invalid_argument);

    REQUIRE(&adapter.wrapped() == recording.get());
    REQUIRE(adapter.registerModel(std::string{kTypeId}, makeHolder) == ModelId{1});
    REQUIRE(adapter.registerModelWithContext(std::string{kTypeId}, makeHolder, "ck") == ModelId{2});
    REQUIRE(adapter.registerModelShared(std::string{kTypeId}, makeHolder, {.contextKey = "ck", .primary = "pk"}) ==
            ModelId{3});
    REQUIRE(adapter.attachModel(std::string{kTypeId}, makeHolder, {.contextKey = "ck", .primary = "pk"}, ModelId{5}) ==
            ModelId{4});

    // The legacy `*Async` twins are forwarded, not swallowed: a wrapped backend
    // that has a non-blocking path keeps it. `RecordingBackend` answers `true`
    // where `IBackend`'s default answers `false`, so each of these would fail if
    // the adapter stopped overriding the verb and inherited that default.
    REQUIRE(adapter.registerModelAsync(std::string{kTypeId}, makeHolder, "ck", nullptr, nullptr));
    REQUIRE(adapter.registerModelSharedAsync(std::string{kTypeId}, makeHolder, {.contextKey = "ck", .primary = "pk"},
                                             nullptr, nullptr));
    REQUIRE(adapter.attachModelAsync(std::string{kTypeId}, makeHolder, {.contextKey = "ck", .primary = "pk"},
                                     ModelId{5}, nullptr, nullptr));
    REQUIRE(adapter.assignPrimaryAsync(ModelId{1}, std::string{kTypeId}, "pk", nullptr, nullptr));

    adapter.assignPrimary(ModelId{6}, std::string{kTypeId}, "pk");
    REQUIRE(adapter.listInstances(std::string{kTypeId}) == std::vector<std::string>{"listed"});
    adapter.deregisterModel(ModelId{7});
    (void)adapter.execute(ModelId{8}, morph::backend::detail::ActionCall{}, nullptr);
    adapter.notifyBackendChanged();
    adapter.cancelPending(std::make_exception_ptr(std::runtime_error{"cancelled"}));
    adapter.setReconnectHandler(nullptr);
    adapter.setConnectHandler(nullptr);
    adapter.setDisconnectHandler(nullptr);
    adapter.setSession(morph::session::Context{.principal = "pal"});

    REQUIRE(recording->calls ==
            std::vector<std::string>{"registerModel", "registerModelWithContext:ck", "registerModelShared:pk",
                                     "attachModel:pk:5", "registerModelAsync:ck", "registerModelSharedAsync:pk",
                                     "attachModelAsync:pk:5", "assignPrimaryAsync:1:pk", "assignPrimary:6:pk",
                                     "listInstances", "deregisterModel:7", "execute:8", "notifyBackendChanged",
                                     "cancelPending", "setReconnectHandler", "setConnectHandler",
                                     "setDisconnectHandler", "setSession:pal"});
}

// ── `bindWaitPolicy`: the one bit `Completion` cannot carry (morph#593) ──────
//
// Two backends both return an unsettled `Completion` from `bindModel`, and
// `Bridge::registerHandler` — a synchronous entry point whose caller uses the
// handler on the next line — must wait for one and must not wait for the other.
// The pair of cases below pins both answers at the one call site that asks.
//
// The 200 ms reply delay is deliberate and is what makes each case *fail* under
// the opposite policy: it is far longer than the microseconds a non-waiting
// `registerHandler` takes to return, so "bound on return" cannot happen by luck
// and "unbound on return" cannot be a lost race. Neither assertion's passing
// direction depends on the exact value — the waiting case waits however long the
// reply takes, and the non-waiting case polls until it arrives.

namespace {

constexpr auto kBindReplyDelay = std::chrono::milliseconds{200};

/// @brief A backend whose bind reply arrives, later, from a thread the caller
///        does not own — `morph::net::SocketBackend`'s shape.
///
/// Derives from `RecordingBackend` for the verbs `Bridge` needs it to have, but
/// answers `false` to `registerModelAsync` (`RecordingBackend` answers `true`)
/// so that `Bridge::registerHandlerImpl` reaches the structural surface rather
/// than the legacy async verb morph#571 removes.
struct TransportThreadBackend : RecordingBackend {
    /// @brief Policy this double reports; the thing under test.
    morph::backend::detail::BindWait policy = morph::backend::detail::BindWait::kCallerMayBlock;
    /// @brief The "transport" that settles the bind, asserted to be another thread.
    std::thread transport;
    /// @brief Thread `bindModel` was called on.
    std::thread::id callerThread;
    /// @brief `true` if the completion settled somewhere other than `callerThread`.
    std::atomic<bool> settledOffCallerThread{false};

    TransportThreadBackend() = default;
    TransportThreadBackend(const TransportThreadBackend&) = delete;
    TransportThreadBackend& operator=(const TransportThreadBackend&) = delete;
    TransportThreadBackend(TransportThreadBackend&&) = delete;
    TransportThreadBackend& operator=(TransportThreadBackend&&) = delete;

    ~TransportThreadBackend() override {
        if (transport.joinable()) {
            transport.join();
        }
    }

    bool registerModelAsync(const std::string& /*typeId*/,
                            std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                            std::string_view /*contextKey*/, std::function<void(ModelId)> /*onRegistered*/,
                            std::function<void(const std::string&)> /*onError*/) override {
        return false;
    }

    ModelCompletion bindModel(BindRequest /*request*/, morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = ModelCompletion::makeSettleable(&cbExec);
        callerThread = std::this_thread::get_id();
        transport = std::thread{[this, kept = std::make_shared<ModelCompletion::Promise>(std::move(promise))] {
            std::this_thread::sleep_for(kBindReplyDelay);
            settledOffCallerThread.store(std::this_thread::get_id() != callerThread);
            kept->resolve(ModelId{99});
        }};
        return std::move(completion);
    }

    [[nodiscard]] morph::backend::detail::BindWait bindWaitPolicy() const noexcept override { return policy; }
};

}  // namespace

TEST_CASE("morph::bridge::Bridge: registerHandler waits out a kCallerMayBlock backend's bind and returns bound",
          "[backend][registration-surface][bridge]") {
    auto owned = std::make_unique<TransportThreadBackend>();
    auto* backend = owned.get();
    backend->policy = morph::backend::detail::BindWait::kCallerMayBlock;

    morph::bridge::Bridge bridge{std::move(owned)};
    auto binding = bridge.registerHandler<RegistrationSurfaceModel>();

    // No polling, no drain: the constructor did not return until the reply
    // landed. This is the contract every non-Qt embedder had before morph#568
    // and that morph#586 took away from `SocketBackend`.
    REQUIRE(morph::bridge::Bridge::isBound(binding));
    REQUIRE(binding->currentId.load() == 99U);
    // ...and it was a *wait*, not a synchronous backend: the value was produced
    // on a thread `registerHandler` does not own.
    REQUIRE(backend->settledOffCallerThread.load());
}

TEST_CASE("morph::bridge::Bridge: registerHandler does not wait for a kCallerMustNotBlock backend",
          "[backend][registration-surface][bridge]") {
    auto owned = std::make_unique<TransportThreadBackend>();
    auto* backend = owned.get();
    backend->policy = morph::backend::detail::BindWait::kCallerMustNotBlock;

    morph::bridge::Bridge bridge{std::move(owned)};
    auto binding = bridge.registerHandler<RegistrationSurfaceModel>();

    // Returned while the reply is still 200 ms away. For `QtWebSocketBackend`
    // under `asyncRegistrationEnabled` this is not a preference: the reply is
    // delivered by the Qt event loop of this very thread, so a `registerHandler`
    // that waited here would never return (morph#568's WASM page abort).
    REQUIRE_FALSE(morph::bridge::Bridge::isBound(binding));

    REQUIRE(morph::testing::waitUntil([&] { return morph::bridge::Bridge::isBound(binding); }));
    REQUIRE(binding->currentId.load() == 99U);
    REQUIRE(backend->settledOffCallerThread.load());
}

// ── cancelPending and the completions the adapter itself produced (#619) ─────
//
// `SynchronousBackendAdapter::cancelPending` used to be a one-line forward to
// the wrapped backend. The two verbs the adapter *reshapes* settle from a task
// on its own private control strand, so the forward reached none of them: a
// bind cancelled mid-flight went on to resolve **successfully**, which is the
// opposite of `IBackend::cancelPending`'s contract. The case below holds the
// wrapped call open so the completion is provably still pending when
// `cancelPending` runs, and fails if the adapter goes back to forwarding only
// -- the success continuation runs instead of the error one.

namespace {

/// @brief A wrapped backend whose blocking control calls do not return until
///        the test lets them, so a cancellation can land while one is in
///        flight.
///
/// Every field the test thread reads is atomic: `RecordingBackend::calls` is a
/// plain vector appended from the strand thread, so this double counts through
/// atomics instead and leaves that vector to the strand alone.
struct GatedBackend : RecordingBackend {
    std::mutex mtx;
    std::condition_variable gate;
    bool released = false;
    /// @brief Control calls that have entered the blocking region.
    std::atomic<int> entered{0};
    /// @brief Control calls that have come back out of it.
    std::atomic<int> finished{0};
    /// @brief Times the adapter forwarded a cancellation here.
    std::atomic<int> cancels{0};

    /// @brief Blocks the calling (strand) thread until `letGo()`.
    void hold() {
        entered.fetch_add(1);
        std::unique_lock lock{mtx};
        (void)gate.wait_for(lock, morph::testing::kDefaultWaitBudget, [this] { return released; });
    }

    /// @brief Releases every held control call.
    void letGo() {
        {
            std::scoped_lock const lock{mtx};
            released = true;
        }
        gate.notify_all();
    }

    ModelId registerModelWithContext(const std::string& /*typeId*/,
                                     std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                                     std::string_view /*contextKey*/) override {
        hold();
        finished.fetch_add(1);
        return ModelId{2};
    }

    void assignPrimary(ModelId /*mid*/, const std::string& /*typeId*/, std::string_view /*primary*/) override {
        hold();
        finished.fetch_add(1);
    }

    void cancelPending(const std::exception_ptr& /*exc*/) override { cancels.fetch_add(1); }
};

}  // namespace

TEST_CASE("morph::backend::SynchronousBackendAdapter: cancelPending rejects the completions it produced itself",
          "[backend][registration-surface][threading]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor callerExec;
    auto inner = std::make_shared<GatedBackend>();
    SynchronousBackendAdapter adapter{inner, pool};

    std::atomic<int> okRan{0};
    std::atomic<int> errRan{0};
    std::string message;

    auto attach = [&](ModelCompletion completion) {
        completion.then([&](ModelId /*mid*/) { okRan.fetch_add(1); }).onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::exception& err) {
                message = err.what();
            }
            errRan.fetch_add(1);
        });
    };

    SECTION("bind") {
        attach(adapter.bindModel(
            BindRequest{.typeId = std::string{kTypeId}, .factory = makeHolder, .contextKey = "ctx", .primary = {}},
            callerExec));
    }
    SECTION("promote") {
        attach(adapter.promoteModel(
            PromoteRequest{.mid = ModelId{7}, .typeId = std::string{kTypeId}, .primary = "key"}, callerExec));
    }

    // The wrapped control call is provably inside its blocking region, so the
    // completion is genuinely still pending -- not merely unobserved.
    REQUIRE(morph::testing::waitUntil([&] { return inner->entered.load() == 1; }));
    REQUIRE(inner->finished.load() == 0);
    REQUIRE(okRan.load() == 0);
    REQUIRE(errRan.load() == 0);

    adapter.cancelPending(std::make_exception_ptr(morph::backend::BridgeDestroyedError{}));

    // Delivered on the caller's executor, like every other continuation from
    // this adapter, and delivered *before* the wrapped call has returned.
    REQUIRE(morph::testing::waitUntil([&] {
        callerExec.runOnce();
        return errRan.load() == 1;
    }));
    REQUIRE(inner->finished.load() == 0);
    REQUIRE(okRan.load() == 0);
    REQUIRE(message == std::string{morph::backend::BridgeDestroyedError{}.what()});
    // ...and the wrapped backend still saw the cancellation it always saw.
    REQUIRE(inner->cancels.load() == 1);

    // Letting the wrapped call finish must not resurrect the cancelled
    // completion: its `resolve` finds the state already ready.
    inner->letGo();
    REQUIRE(morph::testing::waitUntil([&] {
        callerExec.runOnce();
        return inner->finished.load() == 1;
    }));
    callerExec.runOnce();
    REQUIRE(okRan.load() == 0);
    REQUIRE(errRan.load() == 1);
}
