// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_support.hpp"

// Regression test for the same-model execute reordering `RemoteServer` used
// to allow: `handle()` posts every envelope to the shared worker pool, so two
// `execute` envelopes for the *same* model, sent back-to-back on one
// connection, could reach the model's own strand out of send order the moment
// more than one pool worker was free to race the pre-strand work
// (decode/authorize/authenticate/registry-lookup) ahead of the other. The
// per-model execute-ordering gate that closes it (`_executeGates`,
// `awaitExecuteTurn`, `releaseExecuteTicket`) lives in
// `include/morph/core/remote.hpp`, whose comments carry its full design
// history — including the reverted first attempt.
//
// examples/common/testkit/test_fault_proxy.cpp's `FaultProxy::dropReply` test
// caught this incidentally (it happens to send two calls close together) but
// relies on real OS thread scheduling to hit the race, so it passed on every
// quiet/fast run and only failed, intermittently, under CI load — not a
// reliable reproduction on its own.
//
// This test forces the exact interleaving instead of hoping for it: a real
// ThreadPoolExecutor{2} (so the two calls' pre-strand work can genuinely run
// concurrently, on separate threads, exactly as in production), paired with a
// custom IAuthorizer whose `authorize()` deliberately sleeps for call A's
// invocation only. That guarantees call B's pre-strand work (which never
// sleeps) finishes first on every run, deterministically — call B's own
// pool thread reaches the point where it would call `_strand.post(mid, ...)`
// while call A's thread is still sleeping inside `authorize()`, on every
// single run of this test, not just probabilistically. A
// DeterministicExecutor-based version (single-threaded, step-driven) was
// tried first and does not work for this: it cannot model "B's pool thread
// blocks waiting for A to make progress" without a second real thread to
// make that progress — DeterministicExecutor only runs one task to
// completion at a time, so a fix that makes B legitimately wait for A
// deadlocks it. Real threads are required to exercise the actual blocking
// wait the ordering gate introduces.

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
///        first call it sees carrying `EroAddAction::by == kSlowByValue` —
///        every other call (including a second `by == kSlowByValue` call,
///        should a future edit to this test ever add one) returns
///        immediately. This is what turns "the race might happen" into "the
///        race always happens": call A's pre-strand work is held up right
///        here, in `dispatchExecute`'s own authorize() step, for long enough
///        that call B's identical pre-strand work — running concurrently on
///        the pool's other thread — reliably finishes first and reaches the
///        ticket-wait point before A ever does.
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

/// @brief Allow-all authorizer whose `authorize()` sleeps once, for the
///        first call it sees carrying `EroAddAction::by == kSlowByValue` --
///        every call carrying `EroAddAction::by == kRejectByValue` returns
///        `false` immediately, every run, with no sleep at all.
///
/// Drives `dispatchExecute`'s per-model execute-ordering gate (remote.hpp's
/// `_executeGates`) past full drain while an earlier-numbered ticket for the
/// same model is still outstanding: the slow call takes ticket 0 and is held
/// up in `authorize()`; two `kRejectByValue` calls for the *same* model, sent
/// after it, take tickets 1 and 2 and are rejected in `dispatchExecute`
/// before ever reaching `awaitExecuteTurn` (the "unauthorized" branch
/// releases its ticket immediately, without waiting). Both reject calls
/// therefore release and drain the gate (`releaseExecuteTicket`'s
/// `nextToRun == nextTicket` check erases the map entry once ticket 2
/// releases) while ticket 0 has not yet even called `awaitExecuteTurn` --
/// exactly the interleaving `awaitExecuteTurn`'s and `releaseExecuteTicket`'s
/// own "gate already gone" branches exist for.
class SlowFirstThenRejectAuthorizer : public morph::session::IAuthorizer {
public:
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view,
                                 std::string_view actionType) const override {
        if (actionType == kRejectMarker) {
            return false;
        }
        if (!_slowCallTaken.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        return true;
    }

    static constexpr std::string_view kRejectMarker = "ERO_RejectAction";

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

    // SlowFirstAuthorizer guarantees B's authorize() call (and everything
    // after it in B's pre-strand work) finishes before A's does -- A is the
    // first call reaching authorize() program-order, so it is the one held
    // up. Without the ordering gate, this is precisely the interleaving that
    // lets B's execute reach the model's strand before A's, even though the
    // client sent A first.
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
    // whichever pool thread happened to finish its pre-strand work first.
    CHECK(replyA.env.body == "10");
    CHECK(replyB.env.body == "110");
}

TEST_CASE(
    "RemoteServer::dispatchExecute: awaitExecuteTurn and releaseExecuteTicket both cope when their "
    "model's execute gate has already fully drained and been erased",
    "[remote][execute-ordering]") {
    // Three same-model executes, same send order every run:
    //   A (ticket 0, EroAddAction by=kSlowByValue) -- held up in authorize()
    //     for 200ms by SlowFirstThenRejectAuthorizer, so it is reliably still
    //     there when B and C's own pre-strand work runs.
    //   B (ticket 1, ERO_RejectAction) -- authorize() returns false
    //     immediately; dispatchExecute's rejectAndRelease releases ticket 1
    //     right away, never calling awaitExecuteTurn at all.
    //   C (ticket 2, ERO_RejectAction) -- same as B, ticket 2.
    // releaseExecuteTicket unconditionally sets nextToRun = ticket + 1, so
    // C's release (nextToRun 2 -> 3) meets nextTicket (3, since all three
    // tickets were already taken by the time C releases) and erases the
    // gate's map entry -- while A (ticket 0) has not yet reached
    // awaitExecuteTurn at all. When A's authorize() finally returns:
    //   - awaitExecuteTurn(mid, 0) finds no map entry -- "gate already gone"
    //     (line ~1467) -- and returns immediately instead of waiting.
    //   - dispatchExecute posts to the strand, runs, and then calls
    //     releaseExecuteTicket(mid, 0), which *also* finds no map entry --
    //     the defensive "should not happen" branch (line ~1489) -- since C's
    //     release already erased it.
    // Both branches are exercised by this one interleaving, in one test.
    morph::exec::ThreadPoolExecutor pool{3};
    auto authorizer = std::make_shared<SlowFirstThenRejectAuthorizer>();
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
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    reqB.actionType = std::string{SlowFirstThenRejectAuthorizer::kRejectMarker};
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));

    morph::wire::Envelope reqC = reqB;
    reqC.callId = 3;
    WaitReply replyC;
    server->handle(morph::wire::encode(reqC), std::ref(replyC));

    // B and C settle fast (rejected in authorize(), no wait); A settles after
    // its 200ms sleep. If awaitExecuteTurn/releaseExecuteTicket's "gate
    // already gone" branches did not handle a missing map entry gracefully
    // (e.g. by dereferencing iter->second unconditionally), this would crash
    // or hang instead of completing within the budget below.
    REQUIRE(replyB.await(std::chrono::milliseconds{2000}));
    REQUIRE(replyC.await(std::chrono::milliseconds{2000}));
    REQUIRE(replyA.await(std::chrono::milliseconds{5000}));

    REQUIRE(replyB.env.kind == "err");
    REQUIRE(replyB.env.message == "unauthorized");
    REQUIRE(replyC.env.kind == "err");
    REQUIRE(replyC.env.message == "unauthorized");
    REQUIRE(replyA.env.kind == "ok");
    CHECK(replyA.env.body == "5");
}

namespace {

/// @brief Wraps a real executor and holds back exactly one task -- the first
///        posted after `holdNextPost()` -- until `releaseHeld()` forwards it.
///
/// `RemoteServer` posts each `handle()` call's dispatch work to the executor it
/// was constructed with (and routes `_strand` through the same one), so
/// intercepting a single post is enough to decide *which* of two concurrent
/// requests reaches `dispatchMessage` first. That is what makes the shutdown
/// interleaving below deterministic instead of a nanoseconds-wide race between
/// two pool threads reading `_shuttingDown`: without it the window is real but
/// far too narrow to hit on demand (400 jittered attempts did not).
class HoldOnePostExecutor : public morph::exec::IExecutor {
public:
    explicit HoldOnePostExecutor(morph::exec::IExecutor& inner) : _inner{inner} {}

    /// @brief Forwards @p task, unless it is the one post `holdNextPost()` armed.
    /// @param task Callable to execute.
    void post(std::function<void()> task) override {
        {
            std::scoped_lock const lock{_mtx};
            if (_armed) {
                _armed = false;
                _held = std::move(task);
                return;
            }
        }
        _inner.post(std::move(task));
    }

    /// @brief Arms the interception: the next `post()` is captured, not forwarded.
    void holdNextPost() {
        std::scoped_lock const lock{_mtx};
        _armed = true;
    }

    /// @brief Forwards the captured task. No-op if nothing was captured.
    void releaseHeld() {
        std::function<void()> task;
        {
            std::scoped_lock const lock{_mtx};
            task = std::move(_held);
            _held = nullptr;
        }
        if (task) {
            _inner.post(std::move(task));
        }
    }

private:
    morph::exec::IExecutor& _inner;
    std::mutex _mtx;
    bool _armed = false;
    std::function<void()> _held;
};

}  // namespace

TEST_CASE(
    "an execute refused by the shutdown gate releases the execute-ordering "
    "ticket it took, so a later ticket already waiting on it is not stranded",
    "[remote][execute-ordering][shutdown]") {
    // Regression test for #348. `handleImpl` takes the ordering ticket on the
    // transport thread, in send order; `dispatchMessage`'s shutdown gate then
    // returns *before* `dispatchExecute`, which is the only place a ticket is
    // released. Because the pool may run the two posted tasks in either order,
    // the later ticket can pass the gate while the earlier one is still
    // upstream of it -- and if the earlier one is then refused and drops its
    // ticket, the later one waits in `awaitExecuteTurn` on a `cv.wait` with no
    // deadline, forever.
    //
    // The interleaving, forced rather than raced:
    //   A  handle() -> ticket 0; its pool task is intercepted before it runs.
    //   B  handle() -> ticket 1; runs, passes the gate, blocks in
    //      awaitExecuteTurn(mid, 1) waiting for ticket 0.
    //   .. beginShutdown()
    //   A  released; reaches the gate, now closed, and is refused.
    // A must release ticket 0 on that path or B never completes.
    //
    // Heap-allocated because a regression strands a pool worker in a wait with
    // no deadline: ~ThreadPoolExecutor would then block forever in join(),
    // turning a clean assertion failure into a whole-binary hang. On the
    // failing path the fixture is deliberately leaked instead (see below) --
    // the run is already reporting a failure, and a leak is a far more useful
    // outcome than a hang.
    auto pool = std::make_unique<morph::exec::ThreadPoolExecutor>(2);
    auto gated = std::make_unique<HoldOnePostExecutor>(*pool);
    auto server = std::make_shared<morph::backend::RemoteServer>(*gated, eroDispatcher(), eroRegistry());

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

    // A takes ticket 0 but does not run: its dispatch task is held.
    gated->holdNextPost();
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));

    // B takes ticket 1 and runs all the way to awaitExecuteTurn(mid, 1), where
    // it waits for ticket 0. `_inFlightExecutes` is incremented immediately
    // before that wait, so `health().inFlight == 1` is a deterministic signal
    // that B is parked there rather than a sleep hoping that it is.
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));
    REQUIRE(morph::testing::waitUntil([&] { return server->health().inFlight == 1; }));
    REQUIRE_FALSE(replyB.ready.load());

    server->beginShutdown();
    gated->releaseHeld();

    // A is refused by the now-closed gate...
    REQUIRE(replyA.await());
    CHECK(replyA.env.kind == "err");
    CHECK(replyA.env.message == "server shutting down");

    // ...and must have released ticket 0 on the way out. B passed the gate
    // before shutdown, so it is still owed its dispatch.
    bool const bCompleted = replyB.await(std::chrono::milliseconds{5000});
    if (!bCompleted) {
        // See the note above: B's pool thread is stuck in a deadline-less wait
        // and can never be joined.
        (void)server.get();
        (void)gated.release();
        (void)pool.release();
    }
    REQUIRE(bCompleted);
    CHECK(replyB.env.kind == "ok");
    CHECK(replyB.env.body == "11");

    // The same stranded wait also holds `_inFlightExecutes` above zero for
    // good, because it is incremented before the wait -- so a regression here
    // breaks graceful shutdown as well as this one caller.
    CHECK(server->drainedWithin(std::chrono::milliseconds{2000}));
}

namespace {

/// @brief Which of `dispatchExecute`'s user-supplied hooks the armed
///        authorizer below throws from.
///
/// The three are consulted in this order inside `dispatchExecute`
/// (`remote.hpp`): `authorize` (type-level gate), `authenticate` (principal
/// stamping), then -- after the registry lookup -- `authorizeInstance` (the
/// row-level gate). All three are non-`noexcept` virtuals on the public
/// `morph::session::IAuthorizer` extension point, and each sits *between*
/// `handleImpl`'s `takeExecuteTicket` and the `releaseExecuteTicket` that
/// follows `_strand.post`, so a throw from any of them unwinds across the
/// ticketed region.
enum class ThrowingHook : std::uint8_t { Authorize, Authenticate, AuthorizeInstance };

/// @brief Allow-all authorizer that throws from one chosen hook, but only
///        once `arm()` has been called.
///
/// Arming is what keeps the interleaving decided rather than raced: the
/// `register` envelope and request B both run through this authorizer while
/// it is still a plain allow-all, and only the *held* request A -- released
/// after B is parked in `awaitExecuteTurn` -- ever sees the throw.
class ArmedThrowingAuthorizer : public morph::session::IAuthorizer {
public:
    /// @brief Constructs an authorizer that will throw from @p hook once armed.
    /// @param hook The hook to throw from.
    explicit ArmedThrowingAuthorizer(ThrowingHook hook) : _hook{hook} {}

    /// @brief Arms the throw: every call after this point throws from the
    ///        configured hook.
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
        if (from == _hook && _armed.load()) {
            throw std::runtime_error{std::string{kThrowMessage}};
        }
    }

    ThrowingHook _hook;
    std::atomic<bool> _armed{false};
};

/// @brief Runs the "A throws while B is parked on A's ticket" scenario once,
///        with the throw coming from @p hook.
/// @param hook Which `IAuthorizer` hook request A's dispatch throws from.
// The Catch2 assertion macros, not branching logic, are what push this over
// the cognitive-complexity threshold -- exactly as they do in the sibling
// TEST_CASEs above, which the checker exempts only because it does not see
// through TEST_CASE's own generated function.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void runThrowingHookStrandsNothing(ThrowingHook hook) {
    // Same fixture shape, and the same leak-on-failure reasoning, as the
    // shutdown-gate case above: a regression parks a pool worker in a wait
    // with no deadline, so ~ThreadPoolExecutor would hang the whole binary in
    // join() instead of reporting a failed assertion.
    auto pool = std::make_unique<morph::exec::ThreadPoolExecutor>(2);
    auto gated = std::make_unique<HoldOnePostExecutor>(*pool);
    auto authorizer = std::make_shared<ArmedThrowingAuthorizer>(hook);
    auto server = std::make_shared<morph::backend::RemoteServer>(*gated, authorizer, eroDispatcher(), eroRegistry());

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

    // A takes ticket 0 but does not run: its dispatch task is held.
    gated->holdNextPost();
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));

    // B takes ticket 1 and runs, unimpeded (the authorizer is not armed yet),
    // all the way to awaitExecuteTurn(mid, 1). `_inFlightExecutes` is
    // incremented immediately before that wait, so `health().inFlight == 1` is
    // a deterministic signal that B is parked there -- no sleep involved.
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));
    REQUIRE(morph::testing::waitUntil([&] { return server->health().inFlight == 1; }));
    REQUIRE_FALSE(replyB.ready.load());

    // Only A's dispatch can reach the authorizer from here on.
    authorizer->arm();
    gated->releaseHeld();

    // A's hook throws; dispatchMessage's outer catch turns it into an `err`...
    REQUIRE(replyA.await());
    CHECK(replyA.env.kind == "err");
    CHECK(replyA.env.message == std::string{ArmedThrowingAuthorizer::kThrowMessage});

    // ...and ticket 0 must have been released as that throw unwound out of
    // dispatchExecute, or B waits on it forever.
    bool const bCompleted = replyB.await(std::chrono::milliseconds{5000});
    bool const drained = bCompleted && server->drainedWithin(std::chrono::milliseconds{2000});
    if (!bCompleted) {
        // B's pool thread is stuck in a deadline-less wait and can never be
        // joined; leak the fixture rather than hang the run (see above).
        (void)server.get();
        (void)gated.release();
        (void)pool.release();
    }
    REQUIRE(bCompleted);
    CHECK(replyB.env.kind == "ok");
    CHECK(replyB.env.body == "11");

    // The stranded wait also holds `_inFlightExecutes` above zero for good,
    // because it is incremented before the wait -- so a regression here breaks
    // graceful shutdown as well as this one caller.
    CHECK(drained);
}

}  // namespace

TEST_CASE(
    "a throw out of dispatchExecute releases the execute-ordering ticket it took, "
    "so a later ticket already waiting on it is not stranded",
    "[remote][execute-ordering][exceptions]") {
    // Regression test for #351, the sibling of the shutdown-gate case above
    // (#348): the same stranded ticket, reached by a different route.
    // `dispatchExecute` has no try/catch of its own, and its rejectAndRelease
    // helper only covers the *explicit* early returns; an exception unwinds
    // past all of them into `dispatchMessage`'s outer catch, which replies but
    // released nothing. The fix is structural -- the ticket is owned by an
    // RAII holder for the whole span between `takeExecuteTicket` and the
    // release that follows `_strand.post`, so every exit path releases it,
    // including ones nobody has thought of yet.
    //
    // The interleaving, forced rather than raced (identical to #348's case):
    //   A  handle() -> ticket 0; its pool task is intercepted before it runs.
    //   B  handle() -> ticket 1; runs, passes every gate, parks in
    //      awaitExecuteTurn(mid, 1) waiting for ticket 0.
    //   .. the authorizer is armed (so only A can see the throw)
    //   A  released; its hook throws.
    //
    // The three sections below are the three `IAuthorizer` hooks
    // `dispatchExecute` calls inside the ticketed region. The fourth reachable
    // throw site named in #351 -- `missingRequiredFields`, under
    // `PayloadCompleteness::RequireDeclaredFields` -- is *not* separately
    // exercised here: it is not a user-supplied virtual, so forcing a throw
    // out of it would mean faulting the dispatcher's own parse rather than
    // driving a documented extension point. It sits between the same two
    // points as these three (`remote.hpp`: after `authorizeInstance`, before
    // the in-flight reservation), so the holder covers it by construction --
    // as it does any future throw added anywhere in that span, which is the
    // whole reason the fix is a holder rather than a fourth hand-written
    // release.
    SECTION("authorize() throws") { runThrowingHookStrandsNothing(ThrowingHook::Authorize); }
    SECTION("authenticate() throws") { runThrowingHookStrandsNothing(ThrowingHook::Authenticate); }
    SECTION("authorizeInstance() throws") { runThrowingHookStrandsNothing(ThrowingHook::AuthorizeInstance); }
}

namespace {

/// @brief Allow-all authorizer that refuses exactly the marker action type,
///        with no sleeps anywhere.
///
/// The interleaving in the test below is forced by holding pool posts, not by
/// timing, so nothing here needs to be slow -- this authorizer's only job is
/// to make one of three same-model executes take `dispatchExecute`'s
/// `rejectAndRelease` path (which releases its ticket immediately, without
/// ever waiting for its turn) while the others take the normal path.
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

/// @brief Wraps a real executor and holds back the next `n` posts, handing
///        each one on individually, by the order it was posted in.
///
/// The single-post `HoldOnePostExecutor` above is not enough for the scenario
/// below, which needs *three* `handle()` calls to have taken their tickets --
/// tickets are taken synchronously on the calling thread, but the work is
/// posted -- before any of the three dispatch tasks runs. Holding only the
/// first would let the second's dispatch (and its ticket release) race the
/// third's `handle()` call, and the interleaving under test depends on the
/// third ticket already existing.
///
/// Only the *arming window* is intercepted: once `n` posts have been captured
/// every later post (notably `StrandExecutor`'s, which `RemoteServer` routes
/// through this same executor) passes straight through.
class HoldNextPostsExecutor : public morph::exec::IExecutor {
public:
    /// @brief Wraps @p inner, forwarding to it once the arming window closes.
    /// @param inner Executor that actually runs the tasks.
    explicit HoldNextPostsExecutor(morph::exec::IExecutor& inner) : _inner{inner} {}

    /// @brief Captures @p task while posts remain armed, otherwise forwards it.
    /// @param task Callable to execute.
    void post(std::function<void()> task) override {
        {
            std::scoped_lock const lock{_mtx};
            if (_remainingToHold > 0) {
                --_remainingToHold;
                _held.push_back(std::move(task));
                return;
            }
        }
        _inner.post(std::move(task));
    }

    /// @brief Arms interception of the next @p count posts.
    /// @param count Number of posts to capture instead of forwarding.
    void holdNextPosts(std::size_t count) {
        std::scoped_lock const lock{_mtx};
        _remainingToHold = count;
    }

    /// @brief Forwards the @p index-th captured post. No-op if already forwarded.
    /// @param index Position in capture (i.e. post) order.
    void forward(std::size_t index) {
        std::function<void()> task;
        {
            std::scoped_lock const lock{_mtx};
            if (index < _held.size()) {
                task = std::move(_held.at(index));
                _held.at(index) = nullptr;
            }
        }
        if (task) {
            _inner.post(std::move(task));
        }
    }

    /// @brief Forwards every captured post not already forwarded.
    void forwardRest() {
        std::vector<std::function<void()>> pending;
        {
            std::scoped_lock const lock{_mtx};
            pending.swap(_held);
            _remainingToHold = 0;
        }
        for (auto& task : pending) {
            if (task) {
                _inner.post(std::move(task));
            }
        }
    }

private:
    morph::exec::IExecutor& _inner;
    std::mutex _mtx;
    std::size_t _remainingToHold = 0;
    std::vector<std::function<void()>> _held;
};

}  // namespace

// The Catch2 assertion macros, not branching logic, are what push this over
// the cognitive-complexity threshold -- as in the sibling TEST_CASEs above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "an execute rejected out of ticket order does not strand an earlier ticket "
    "that has not reached awaitExecuteTurn yet",
    "[remote][execute-ordering]") {
    // Regression test for #449 -- the third occurrence of the stranded-ticket
    // bug class #348 and #351 each closed by making the *release* structural.
    // Making release unmissable was necessary and is not sufficient: the
    // remaining hole is in `releaseExecuteTicket` itself.
    //
    // `releaseExecuteTicket(mid, ticket)` used to assign
    //
    //     gate.nextToRun = ticket + 1;
    //
    // which is only correct if tickets are released in ticket order. They are
    // not, and by design: every early return in `dispatchExecute`
    // (`rejectAndRelease` -- model not found, unauthorized, over limit) and
    // `dispatchMessage`'s shutdown gate release *immediately*, deliberately
    // never calling `awaitExecuteTurn`, so that a rejection cannot hold up the
    // live executes queued behind it. So a later ticket routinely releases
    // first -- and when it did, that assignment pushed `nextToRun` straight
    // *past* an earlier ticket's number. `awaitExecuteTurn` waits on
    // `nextToRun == ticket` with no deadline, so the earlier ticket's waiter
    // could never be satisfied: a pool worker blocked for the rest of the
    // process's life, `_inFlightExecutes` stuck above zero (so
    // `drainedWithin()` can never succeed), and `~ThreadPoolExecutor` hanging
    // forever in join(). That is exactly the reported symptom, and it explains
    // why #449 reproduced under `morph::net` in particular: a dropped
    // connection reclaims that connection's models, so the executes still in
    // flight for one model split into some that find the model and some that
    // reject with "model not found" -- manufacturing precisely this
    // out-of-order release.
    //
    // The fix keeps the fast-reject path fast and instead makes `nextToRun`
    // advance only over a *contiguous* run of released tickets, holding an
    // early release aside as pending until every ticket before it has released
    // too.
    //
    // Forced, not raced. Three same-model executes take tickets 0, 1 and 2
    // synchronously in `handle()`, with all three dispatch tasks held, so the
    // gate is fully populated before any of them runs:
    //   A  ticket 0, ERO_AddAction    -- released second; must run first.
    //   B  ticket 1, ERO_RejectAction -- released first; rejected in
    //      authorize(), so it releases ticket 1 without ever waiting.
    //   C  ticket 2, ERO_AddAction    -- not released until the end, purely so
    //      `nextToRun` cannot reach `nextTicket` and erase the gate (that
    //      erase is what makes the *two*-ticket version of this interleaving
    //      harmless, and it is already covered by the "gate has already fully
    //      drained" case above).
    auto pool = std::make_unique<morph::exec::ThreadPoolExecutor>(3);
    auto gated = std::make_unique<HoldNextPostsExecutor>(*pool);
    auto authorizer = std::make_shared<RejectMarkerAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(*gated, authorizer, eroDispatcher(), eroRegistry());

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

    gated->holdNextPosts(3);
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));
    WaitReply replyC;
    server->handle(morph::wire::encode(reqC), std::ref(replyC));
    REQUIRE_FALSE(replyA.ready.load());
    REQUIRE_FALSE(replyB.ready.load());
    REQUIRE_FALSE(replyC.ready.load());

    // B first: `rejectAndRelease` releases ticket 1 and only then writes the
    // reply, so a settled replyB is a deterministic signal that ticket 1 is
    // already released -- no sleep involved.
    gated->forward(1);
    REQUIRE(replyB.await());
    CHECK(replyB.env.kind == "err");
    CHECK(replyB.env.message == "unauthorized");

    // Now A, whose ticket 0 that release skipped over.
    gated->forward(0);
    bool const aCompleted = replyA.await(std::chrono::milliseconds{5000});
    if (!aCompleted) {
        // A's pool thread is parked in a wait with no deadline and can never
        // be joined; leak the fixture rather than hang the whole binary in
        // ~ThreadPoolExecutor, exactly as the #348/#351 cases above do.
        (void)server.get();
        // NOLINTBEGIN(bugprone-unused-return-value) -- leaking is the point.
        (void)gated.release();
        (void)pool.release();
        // NOLINTEND(bugprone-unused-return-value)
    }
    REQUIRE(aCompleted);
    CHECK(replyA.env.kind == "ok");
    CHECK(replyA.env.body == "5");

    // And ordering still holds for the ticket behind the skipped one: C ran
    // after A, so the counter reads 5 + 7 rather than 7.
    gated->forwardRest();
    REQUIRE(replyC.await());
    CHECK(replyC.env.kind == "ok");
    CHECK(replyC.env.body == "12");

    // The stranded wait would also hold `_inFlightExecutes` above zero for
    // good, so a regression breaks graceful shutdown as well as this caller.
    CHECK(server->drainedWithin(std::chrono::milliseconds{2000}));
}
