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

    void deregisterModel(ModelId /*mid*/) override {}

    morph::async::Completion<std::shared_ptr<void>> execute(ModelId /*mid*/,
                                                            morph::backend::detail::ActionCall /*call*/,
                                                            morph::exec::IExecutor* /*cbExec*/) override {
        return {};
    }

    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr& /*exc*/) override {}
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

/// @brief A backend whose synchronous registration fails.
struct ThrowingBackend : RecordingBackend {
    ModelId registerModelWithContext(const std::string& /*typeId*/,
                                     std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> /*factory*/,
                                     std::string_view /*contextKey*/) override {
        throw std::runtime_error{"register refused"};
    }
};

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

TEST_CASE("morph::backend::IBackend: a failing bind rejects the completion instead of throwing at the call site",
          "[backend][registration-surface]") {
    ThrowingBackend backend;
    morph::exec::MainThreadExecutor callerExec;

    std::string message;
    std::atomic<bool> done{false};
    auto completion = backend.bindModel(
        BindRequest{.typeId = std::string{kTypeId}, .factory = makeHolder, .contextKey = {}, .primary = {}},
        callerExec);
    completion.onErrorDetached([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& err) {
            message = err.what();
        }
        done.store(true);
    });

    REQUIRE(drainUntil(callerExec, done));
    REQUIRE(message == "register refused");
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
    // that has a non-blocking path keeps it.
    REQUIRE_FALSE(adapter.registerModelAsync(std::string{kTypeId}, makeHolder, "ck", nullptr, nullptr));
    REQUIRE_FALSE(adapter.registerModelSharedAsync(std::string{kTypeId}, makeHolder, {}, nullptr, nullptr));
    REQUIRE_FALSE(adapter.attachModelAsync(std::string{kTypeId}, makeHolder, {}, ModelId{}, nullptr, nullptr));
    REQUIRE_FALSE(adapter.assignPrimaryAsync(ModelId{1}, std::string{kTypeId}, "pk", nullptr, nullptr));
    adapter.assignPrimary(ModelId{6}, std::string{kTypeId}, "pk");

    REQUIRE(recording->calls == std::vector<std::string>{"registerModel", "registerModelWithContext:ck",
                                                         "registerModelShared:pk", "attachModel:pk:5",
                                                         "assignPrimary:6:pk"});
}
