// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_support.hpp"

// Per-model execute ordering at the `RemoteServer` level: for one model, the
// order `handle()` is called in is the order the model runs the actions.
// `handle()` posts every envelope to the server strand, which admits an
// `execute` and posts it to the model's strand; both run in post order.
//
// Each case forces its interleaving with real threads rather than hoping for
// it: a ThreadPoolExecutor with more than one worker, so pre-dispatch work that
// the design ran concurrently would genuinely race, and an authorizer or an
// executor wrapper that holds one request back on purpose.

namespace {

// Deliberately at namespace scope, not inside an anonymous namespace: glz's
// reflection (which the model/action registration below relies on to
// serialize these types across the wire) needs external linkage on the type
// -- see glaze/reflection/get_name.hpp's `extern const T external`, and this
// file's own sibling examples/common/testkit/test_fault_proxy.cpp's identical
// note on FaultProbeAdd/FaultProbeCounter. (This anonymous namespace wraps
// only the authorizer and helper functions below, none of which need
// external linkage; EroAddAction/EroCounterModel are defined just outside
// it, further down, for exactly that reason.)

/// @brief Allow-all authorizer whose `authorize()` sleeps once, for the
///        first call it sees; every other call returns immediately. Call A's
///        admission is held up right here, in `dispatchExecute`'s authorize()
///        step, for long enough that call B — with the pool's other thread
///        free — would reach its model's strand first if anything but the
///        server strand's order decided it.
class SlowFirstAuthorizer : public morph::session::IAuthorizer {
public:
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        if (!_slowCallTaken.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        return true;
    }

private:
    mutable std::atomic<bool> _slowCallTaken{false};
};

}  // namespace

struct EroAddAction {
    int by = 0;
};

// A running total, not a pure function of the action -- mirrors
// FaultProbeCounter in test_fault_proxy.cpp: only an accumulator can
// distinguish "processed out of order" from "processed in order", since the
// wrong order still produces *a* plausible-looking total, just the wrong one.
struct EroCounterModel {
    int value = 0;
    int execute(EroAddAction action) {
        value += action.by;
        return value;
    }
};

template <>
struct morph::model::ModelTraits<EroCounterModel> {
    static constexpr std::string_view typeId() { return "ERO_CounterModel"; }
};
template <>
struct morph::model::ActionTraits<EroAddAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "ERO_AddAction"; }
    static std::string toJson(const EroAddAction& action) { return "{\"by\":" + std::to_string(action.by) + "}"; }
    static EroAddAction fromJson(std::string_view json) {
        EroAddAction action;
        // Minimal hand-rolled parse -- the fixed shape ({"by":N}) doesn't
        // justify pulling in glaze here; every sibling RemoteServer test in
        // this directory (test_remote_connection_scope.cpp's CsSquareAction,
        // etc.) round-trips through the real ActionDispatcher via glaze
        // instead, but this model only needs `execute()` reached directly
        // from RemoteServer's own decode path, which calls fromJson() itself.
        auto pos = json.find(':');
        if (pos != std::string_view::npos) {
            action.by = std::stoi(std::string{json.substr(pos + 1, json.find('}') - pos - 1)});
        }
        return action;
    }
    static std::string resultToJson(const int& result) { return std::to_string(result); }
    static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); }
};

namespace {

using morph::testing::WaitReply;

morph::model::detail::ActionDispatcher& eroDispatcher() {
    static morph::model::detail::ActionDispatcher dispatcher = [] {
        morph::model::detail::ActionDispatcher d;
        d.registerAction<EroCounterModel, EroAddAction>("ERO_CounterModel", "ERO_AddAction");
        return d;
    }();
    return dispatcher;
}

morph::model::detail::ModelRegistryFactory& eroRegistry() {
    static morph::model::detail::ModelRegistryFactory registry = [] {
        morph::model::detail::ModelRegistryFactory r;
        r.registerModel<EroCounterModel>("ERO_CounterModel");
        return r;
    }();
    return registry;
}

}  // namespace

TEST_CASE(
    "RemoteServer::handle() preserves send order for two same-model executes "
    "even when the second one's pre-strand work finishes first",
    "[remote][execute-ordering]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto authorizer = std::make_shared<SlowFirstAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, eroDispatcher(), eroRegistry());

    WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ERO_CounterModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "ok");
    const auto modelId = regReply.env.modelId;
    REQUIRE(modelId != 0U);

    // Two execute envelopes for the SAME model, sent back-to-back on the
    // same (simulated) connection -- call A (by=10) first, call B (by=100)
    // second, exactly like two requests arriving close together. handle()
    // returns immediately in both cases (it only posts to the pool), so
    // these two calls are made in strict program order here, mirroring two
    // messages arriving in that order over one WebSocket connection.
    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = modelId;
    reqA.modelType = "ERO_CounterModel";
    reqA.actionType = "ERO_AddAction";
    reqA.body = R"({"by":10})";
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    reqB.body = R"({"by":100})";
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));

    // SlowFirstAuthorizer holds A's admission for 200ms while the pool's
    // second thread is free. Were admission run concurrently, B would reach
    // the model's strand first; on the server strand B waits its turn.
    REQUIRE(replyA.await(std::chrono::milliseconds{5000}));
    REQUIRE(replyB.await(std::chrono::milliseconds{5000}));
    REQUIRE(replyA.env.kind == "ok");
    REQUIRE(replyB.env.kind == "ok");

    // The load-bearing assertion: A (by=10) must be applied before B
    // (by=100) resolves, because the client sent A first. If B's effect was
    // applied first (the bug), replyA.env.body is "110" and replyB.env.body
    // is "100" -- still internally consistent, still both "ok", but
    // backwards relative to send order. Correct behaviour is A settles at
    // 10, B settles at 110, in THAT order -- matching send order, not
    // whichever pool thread happened to finish its admission first.
    CHECK(replyA.env.body == "10");
    CHECK(replyB.env.body == "110");
}

namespace {

/// @brief Allow-all authorizer whose `authorize()` blocks, for the first call
///        it sees, until `release()`; every other call returns at once.
///
/// Holds one request's admission on the server strand, so what is sent after
/// it queues behind it — the way the shutdown case below orders "admitted
/// before shutdown" against "refused after it" without a sleep.
class GateFirstAuthorizer : public morph::session::IAuthorizer {
public:
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        if (!_firstTaken.exchange(true)) {
            _arrived.store(true);
            (void)morph::testing::waitUntil([this] { return _released.load(); },
                                            morph::testing::WaitBudget{std::chrono::milliseconds{10000}});
        }
        return true;
    }

    /// @return Whether the held call has reached `authorize()`.
    [[nodiscard]] bool arrived() const { return _arrived.load(); }

    /// @brief Lets the held call return.
    void release() { _released.store(true); }

private:
    mutable std::atomic<bool> _firstTaken{false};
    mutable std::atomic<bool> _arrived{false};
    std::atomic<bool> _released{false};
};

}  // namespace

TEST_CASE(
    "an execute refused by the shutdown gate does not strand a same-model "
    "execute admitted before it",
    "[remote][execute-ordering][shutdown]") {
    // B is admitted before beginShutdown() takes effect and A after it, on the
    // same model. A is refused; B must still run and reply, and the drain must
    // still complete. B's admission is held on the server strand (its
    // authorize() blocks), so beginShutdown() and A queue behind it: the order
    // is decided, not raced.
    morph::exec::ThreadPoolExecutor pool{2};
    auto authorizer = std::make_shared<GateFirstAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, eroDispatcher(), eroRegistry());

    WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ERO_CounterModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "ok");
    const auto modelId = regReply.env.modelId;
    REQUIRE(modelId != 0U);

    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = modelId;
    reqA.modelType = "ERO_CounterModel";
    reqA.actionType = "ERO_AddAction";
    reqA.body = R"({"by":7})";

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    reqB.body = R"({"by":11})";

    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));
    REQUIRE(morph::testing::waitUntil([&] { return authorizer->arrived(); }));

    server->beginShutdown();
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));
    REQUIRE_FALSE(replyA.ready.load());
    authorizer->release();

    // A is refused by the now-closed gate...
    REQUIRE(replyA.await());
    CHECK(replyA.env.kind == "err");
    CHECK(replyA.env.message == "server shutting down");

    // ...and B, admitted before shutdown, is still owed its dispatch.
    REQUIRE(replyB.await(std::chrono::milliseconds{5000}));
    CHECK(replyB.env.kind == "ok");
    CHECK(replyB.env.body == "11");

    CHECK(morph::testing::awaitValue(server->drainedWithin(std::chrono::milliseconds{2000})));
}

namespace {

/// @brief Which of `dispatchExecute`'s user-supplied hooks the armed
///        authorizer below throws from.
///
/// The three are consulted in this order inside `dispatchExecute`
/// (`remote.hpp`): `authorize` (type-level gate), `authenticate` (principal
/// stamping), then -- after the registry lookup -- `authorizeInstance` (the
/// row-level gate). All three are non-`noexcept` virtuals on the public
/// `morph::session::IAuthorizer` extension point.
enum class ThrowingHook : std::uint8_t { Authorize, Authenticate, AuthorizeInstance };

/// @brief Allow-all authorizer that throws from one chosen hook, once, the
///        first time that hook is called after `arm()`.
///
/// Arming after the `register` reply keeps registration (which also
/// authenticates) out of it, and throwing once means only the first execute
/// sent afterwards sees the throw.
class ArmedThrowingAuthorizer : public morph::session::IAuthorizer {
public:
    /// @brief Constructs an authorizer that will throw from @p hook once armed.
    /// @param hook The hook to throw from.
    explicit ArmedThrowingAuthorizer(ThrowingHook hook) : _hook{hook} {}

    /// @brief Arms the throw: the next call of the configured hook throws.
    void arm() { _armed.store(true); }

    /// @brief Type-level gate; throws when armed and configured to.
    /// @return `true` (allow) whenever it does not throw.
    [[nodiscard]] bool authorize([[maybe_unused]] const morph::session::Context& ctx,
                                 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                 [[maybe_unused]] std::string_view modelType,
                                 [[maybe_unused]] std::string_view actionType) const override {
        maybeThrow(ThrowingHook::Authorize);
        return true;
    }

    /// @brief Principal-stamping hook; throws when armed and configured to.
    /// @return `std::nullopt` whenever it does not throw.
    [[nodiscard]] std::optional<std::string> authenticate(
        [[maybe_unused]] const morph::session::Context& ctx) const override {
        maybeThrow(ThrowingHook::Authenticate);
        return std::nullopt;
    }

    /// @brief Row-level gate; throws when armed and configured to.
    /// @return `true` (allow) whenever it does not throw.
    [[nodiscard]] bool authorizeInstance([[maybe_unused]] const morph::session::Context& ctx,
                                         // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                         [[maybe_unused]] std::string_view modelType,
                                         [[maybe_unused]] std::string_view actionType,
                                         [[maybe_unused]] std::uint64_t modelId,
                                         [[maybe_unused]] std::string_view ownerPrincipal) const override {
        maybeThrow(ThrowingHook::AuthorizeInstance);
        return true;
    }

    /// @brief The message the armed hook throws, which the server must turn
    ///        into this call's `err` reply.
    static constexpr std::string_view kThrowMessage = "ero-351: extension point threw";

private:
    void maybeThrow(ThrowingHook from) const {
        if (from == _hook && _armed.exchange(false)) {
            throw std::runtime_error{std::string{kThrowMessage}};
        }
    }

    ThrowingHook _hook;
    mutable std::atomic<bool> _armed{false};
};

/// @brief Runs the "A throws, B sent after it on the same model" scenario
///        once, with the throw coming from @p hook.
/// @param hook Which `IAuthorizer` hook request A's admission throws from.
// The Catch2 assertion macros, not branching logic, are what push this over
// the cognitive-complexity threshold.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void runThrowingHookStrandsNothing(ThrowingHook hook) {
    morph::exec::ThreadPoolExecutor pool{2};
    auto authorizer = std::make_shared<ArmedThrowingAuthorizer>(hook);
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, eroDispatcher(), eroRegistry());

    WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ERO_CounterModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "ok");
    const auto modelId = regReply.env.modelId;
    REQUIRE(modelId != 0U);

    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = modelId;
    reqA.modelType = "ERO_CounterModel";
    reqA.actionType = "ERO_AddAction";
    reqA.body = R"({"by":7})";

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    reqB.body = R"({"by":11})";

    // Only A, the first execute after arming, sees the throw.
    authorizer->arm();
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));

    // A's hook throws; dispatchEnvelope's catch turns it into an `err`...
    REQUIRE(replyA.await());
    CHECK(replyA.env.kind == "err");
    CHECK(replyA.env.message == std::string{ArmedThrowingAuthorizer::kThrowMessage});

    // ...and B, behind it on the same model, still runs, on a counter A never
    // touched.
    REQUIRE(replyB.await(std::chrono::milliseconds{5000}));
    CHECK(replyB.env.kind == "ok");
    CHECK(replyB.env.body == "11");

    // A throw during admission leaves no in-flight slot behind.
    CHECK(morph::testing::awaitValue(server->drainedWithin(std::chrono::milliseconds{2000})));
}

}  // namespace

TEST_CASE(
    "a throw out of dispatchExecute's admission does not strand a same-model "
    "execute sent after it",
    "[remote][execute-ordering][exceptions]") {
    // The three sections are the three `IAuthorizer` hooks `dispatchExecute`
    // calls during admission. The fourth throw site there --
    // `missingRequiredFields`, under `PayloadCompleteness::RequireDeclaredFields`
    // -- is not a user-supplied virtual, so forcing a throw out of it would mean
    // faulting the dispatcher's own parse rather than driving a documented
    // extension point; it unwinds the same way.
    SECTION("authorize() throws") { runThrowingHookStrandsNothing(ThrowingHook::Authorize); }
    SECTION("authenticate() throws") { runThrowingHookStrandsNothing(ThrowingHook::Authenticate); }
    SECTION("authorizeInstance() throws") { runThrowingHookStrandsNothing(ThrowingHook::AuthorizeInstance); }
}

namespace {

/// @brief Allow-all authorizer that refuses exactly the marker action type.
class RejectMarkerAuthorizer : public morph::session::IAuthorizer {
public:
    /// @brief Refuses `kRejectMarker`, allows everything else.
    /// @param actionType Action type id being authorized.
    /// @return `false` for the marker action, `true` otherwise.
    [[nodiscard]] bool authorize(const morph::session::Context& /*session*/, std::string_view /*modelType*/,
                                 std::string_view actionType) const override {
        return actionType != kRejectMarker;
    }

    /// @brief Action type id this authorizer refuses.
    static constexpr std::string_view kRejectMarker = "ERO_RejectAction";
};

/// @brief Wraps a real executor and, once armed, blocks the *next* `post()`
///        call until `releaseFirst()` is called, forwarding it then; every
///        other call forwards immediately.
///
/// Not armed by default: a caller opts in via `armNextPost()`, so setup
/// traffic (e.g. a `register` envelope) that also goes through `post()` passes
/// straight through.
///
/// Holds one caller inside `handle()` — inside the server strand's hand-off
/// to the pool, or the model strand's — so a second, concurrent caller gets
/// every chance to run in between.
class StallFirstPostExecutor : public morph::exec::IExecutor {
public:
    explicit StallFirstPostExecutor(morph::exec::IExecutor& inner) : _inner{inner} {}

    /// @brief Forwards @p task, unless interception is armed and this is the
    ///        next call, which blocks until `releaseFirst()` before forwarding.
    /// @param task Callable to execute.
    void post(std::function<void()> task) override {
        bool shouldStall = false;
        {
            std::scoped_lock const lock{_mtx};
            if (_armed) {
                _armed = false;
                shouldStall = true;
            }
        }
        if (shouldStall) {
            {
                std::scoped_lock const lock{_mtx};
                _arrived = true;
            }
            _cv.notify_all();
            std::unique_lock lock{_mtx};
            _cv.wait(lock, [this] { return _released; });
        }
        _inner.post(std::move(task));
    }

    /// @brief Arms interception of the next `post()` call.
    void armNextPost() {
        std::scoped_lock const lock{_mtx};
        _armed = true;
    }

    /// @brief Blocks until the armed `post()` call has arrived and is stalling.
    void waitForArrival() {
        std::unique_lock lock{_mtx};
        _cv.wait(lock, [this] { return _arrived; });
    }

    /// @brief Releases the stalled call, letting it forward.
    void releaseFirst() {
        {
            std::scoped_lock const lock{_mtx};
            _released = true;
        }
        _cv.notify_all();
    }

private:
    morph::exec::IExecutor& _inner;
    std::mutex _mtx;
    std::condition_variable _cv;
    bool _armed = false;
    bool _arrived = false;
    bool _released = false;
};

}  // namespace

TEST_CASE("two concurrent handle() callers on one modelId with a pool of one do not deadlock",
          "[remote][execute-ordering][morph-519]") {
    // morph::net::SocketServer's shape: more than one thread calling handle()
    // for the same model, against a pool no larger than the number of
    // callers. StallFirstPostExecutor blocks thread A inside a post() it makes
    // on the way to the pool, so thread B's handle() call gets every chance to
    // run first. B *winning* that window is best-effort and cannot be made
    // deterministic; what is asserted holds however the two interleave: both
    // replies arrive, and A — whose handle() call came first — runs first.
    auto pool = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
    StallFirstPostExecutor gated{*pool};
    auto server = std::make_shared<morph::backend::RemoteServer>(gated, eroDispatcher(), eroRegistry());

    WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ERO_CounterModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "ok");
    const auto modelId = regReply.env.modelId;
    REQUIRE(modelId != 0U);

    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = modelId;
    reqA.modelType = "ERO_CounterModel";
    reqA.actionType = "ERO_AddAction";
    reqA.body = R"({"by":10})";

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    reqB.body = R"({"by":100})";

    // Armed only now -- the registration above also goes through post(), and
    // must not be the one intercepted.
    gated.armNextPost();
    WaitReply replyA;
    std::thread threadA([&] { server->handle(morph::wire::encode(reqA), std::ref(replyA)); });

    // Proceed only once a post on A's path has reached the executor and is
    // stalled there.
    gated.waitForArrival();

    WaitReply replyB;
    std::thread threadB([&] { server->handle(morph::wire::encode(reqB), std::ref(replyB)); });
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    gated.releaseFirst();

    threadA.join();
    threadB.join();

    bool const aCompleted = replyA.await(std::chrono::milliseconds{5000});
    bool const bCompleted = aCompleted && replyB.await(std::chrono::milliseconds{5000});
    REQUIRE(aCompleted);
    REQUIRE(bCompleted);
    CHECK(replyA.env.kind == "ok");
    CHECK(replyB.env.kind == "ok");
    // Load-bearing: A was sent first, so it must settle first -- 10, then 110 -- not
    // whichever thread happened to win the race to enqueue.
    CHECK(replyA.env.body == "10");
    CHECK(replyB.env.body == "110");
}

// The Catch2 assertion macros, not branching logic, are what push this over
// the cognitive-complexity threshold -- as in the sibling TEST_CASEs above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "an execute rejected between two same-model executes does not break the "
    "order of the two it sits between",
    "[remote][execute-ordering]") {
    // Three same-model executes, sent in order: A adds 5, B is refused by the
    // authorizer, C adds 7. A rejection answers in its own turn on the server
    // strand and holds nothing for anyone; A and C still run in send order, so
    // C reads 5 + 7.
    morph::exec::ThreadPoolExecutor pool{3};
    auto authorizer = std::make_shared<RejectMarkerAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, eroDispatcher(), eroRegistry());

    WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ERO_CounterModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "ok");
    const auto modelId = regReply.env.modelId;
    REQUIRE(modelId != 0U);

    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = modelId;
    reqA.modelType = "ERO_CounterModel";
    reqA.actionType = "ERO_AddAction";
    reqA.body = R"({"by":5})";

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    reqB.actionType = std::string{RejectMarkerAuthorizer::kRejectMarker};

    morph::wire::Envelope reqC = reqA;
    reqC.callId = 3;
    reqC.body = R"({"by":7})";

    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));
    WaitReply replyC;
    server->handle(morph::wire::encode(reqC), std::ref(replyC));

    REQUIRE(replyB.await());
    CHECK(replyB.env.kind == "err");
    CHECK(replyB.env.message == "unauthorized");

    REQUIRE(replyA.await(std::chrono::milliseconds{5000}));
    CHECK(replyA.env.kind == "ok");
    CHECK(replyA.env.body == "5");

    REQUIRE(replyC.await());
    CHECK(replyC.env.kind == "ok");
    CHECK(replyC.env.body == "12");

    CHECK(morph::testing::awaitValue(server->drainedWithin(std::chrono::milliseconds{2000})));
}
