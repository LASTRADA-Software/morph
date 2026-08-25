// SPDX-License-Identifier: Apache-2.0
#include <QUrl>
#include <QWebSocket>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <stdexcept>

#include "testkit/fault_proxy.hpp"
#include "testkit/pump.hpp"

// Deliberately at namespace scope, not inside an anonymous namespace: glz's
// reflection (which BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION rely on to
// serialize these types across the wire) needs external linkage on the type —
// see glaze/reflection/get_name.hpp's `extern const T external`.
struct FaultProbeAdd {
    int by = 0;
};

// A running total, not a pure function of the action: only an accumulator can
// distinguish "the reply was dropped on the way back" from "the request never
// reached the server at all" — a later call's total still carries the effect
// of the call whose reply went missing.
struct FaultProbeCounter {
    int value = 0;
    int execute(FaultProbeAdd action) {
        value += action.by;
        return value;
    }
};

BRIDGE_REGISTER_MODEL(FaultProbeCounter, "FaultProbeCounter")
BRIDGE_REGISTER_ACTION(FaultProbeCounter, FaultProbeAdd, "FaultProbeAdd")

namespace {

using namespace std::chrono_literals;

/// @brief `RemoteServer` -> `QtWebSocketServer` -> `FaultProxy` ->
///        `QtWebSocketBackend` -> `Bridge`, wired in that order and torn down
///        in reverse.
///
/// Reconnect is disabled on the client: it isolates every assertion below from
/// an automatic re-dial racing them (the `killAfter` case especially, which
/// asserts on the disconnect the client observes).
struct ProxyRig {
    ::morph::exec::ThreadPoolExecutor serverPool{2};
    std::shared_ptr<::morph::backend::RemoteServer> server;
    std::unique_ptr<::morph::qt::QtWebSocketServer> wsServer;
    std::unique_ptr<::morph::ladder::testkit::FaultProxy> proxy;
    ::morph::qt::QtExecutor qtExec;
    ::morph::qt::QtWebSocketBackend* backend{nullptr};
    std::unique_ptr<::morph::bridge::Bridge> bridge;

    ProxyRig() {
        server = std::make_shared<::morph::backend::RemoteServer>(serverPool);
        wsServer = std::make_unique<::morph::qt::QtWebSocketServer>(*server, quint16{0});
        if (!wsServer->listen()) {
            throw std::runtime_error("ProxyRig: QtWebSocketServer failed to listen");
        }

        proxy = std::make_unique<::morph::ladder::testkit::FaultProxy>(
            QUrl{QString("ws://127.0.0.1:%1").arg(wsServer->port())});
        const QUrl proxyUrl = proxy->start();

        auto backendPtr = std::make_unique<::morph::qt::QtWebSocketBackend>(
            proxyUrl, std::nullopt, ::morph::qt::QtWebSocketBackend::Config{.reconnectEnabled = false});
        backend = backendPtr.get();
        if (!backendPtr->waitForConnected()) {
            throw std::runtime_error("ProxyRig: client failed to connect through the proxy");
        }
        bridge = std::make_unique<::morph::bridge::Bridge>(std::move(backendPtr));
    }

    ProxyRig(const ProxyRig&) = delete;
    ProxyRig& operator=(const ProxyRig&) = delete;
    ProxyRig(ProxyRig&&) = delete;
    ProxyRig& operator=(ProxyRig&&) = delete;

    ~ProxyRig() {
        bridge.reset();
        backend = nullptr;
        proxy.reset();
        if (wsServer) {
            wsServer->closeGracefully(2000ms);
        }
    }
};

}  // namespace

TEST_CASE("FaultProxy relays an unfaulted call unchanged", "[ladder][testkit][fault-proxy]") {
    ProxyRig rig;
    ::morph::bridge::BridgeHandler<FaultProbeCounter> handler{*rig.bridge, &rig.qtExec};

    CHECK(::morph::ladder::testkit::awaitQt(handler.execute(FaultProbeAdd{2})) == 2);
    CHECK(::morph::ladder::testkit::awaitQt(handler.execute(FaultProbeAdd{3})) == 5);
}

TEST_CASE("FaultProxy::dropReply loses exactly the reply frame of the targeted call",
          "[ladder][testkit][fault-proxy]") {
    // Declared above the rig deliberately, and it matters here more than
    // anywhere else in this file: this test leaves call 2's `Completion`
    // unsettled at scope exit *on purpose*. `~ProxyRig` then tears the backend
    // down, which calls `cancelPending(DisconnectedError)`; that posts the
    // `.onError` below through `QtExecutor`, and `~QtWebSocketBackend`'s own
    // `processEvents()` dispatches it a few lines later. Locals declared after
    // the rig are destroyed *before* it (reverse declaration order), so the
    // callback would write into dead stack slots. Anything a lambda outliving
    // the rig captures by reference therefore lives up here — the request
    // observer's counters included, since the proxy owns that lambda until
    // `~ProxyRig` destroys it.
    int requestsSeen = 0;
    std::uint64_t targetedCallId = 0;
    bool secondResolved = false;
    bool secondFailed = false;

    ProxyRig rig;
    ::morph::bridge::BridgeHandler<FaultProbeCounter> handler{*rig.bridge, &rig.qtExec};

    // The callId of an upcoming execute() is not knowable from here —
    // BridgeHandler::execute() hands back a bare Completion and never names the
    // id the backend assigned it. setRequestObserver supplies it at the one
    // moment where arming a rule for it is still race-free: the request is
    // sitting in the proxy, not yet forwarded upstream.
    rig.proxy->setRequestObserver([&](std::uint64_t callId, ::morph::ladder::testkit::FaultProxy& self) {
        if (++requestsSeen == 2) {
            targetedCallId = callId;
            self.dropReply(callId);  // exactly call k = 2, nothing else
        }
    });

    // Call 1 — unfaulted, must resolve.
    CHECK(::morph::ladder::testkit::awaitQt(handler.execute(FaultProbeAdd{1})) == 1);

    // Call 2 — its reply is the one dropped.
    const std::uint64_t forwardedBefore = rig.proxy->repliesForwarded();
    handler.execute(FaultProbeAdd{10})
        .then([&](int) { secondResolved = true; })
        .onError([&](const std::exception_ptr&) { secondFailed = true; });

    // Call 3 — unfaulted, must resolve. Its running total is the load-bearing
    // assertion: 1 + 10 + 100 only comes out if call 2 genuinely reached the
    // server and committed its effect there, so this distinguishes "the reply
    // was dropped" from "the request was never sent". It equally rules out a
    // proxy that drops everything — a blanket drop would hang this await.
    CHECK(::morph::ladder::testkit::awaitQt(handler.execute(FaultProbeAdd{100})) == 111);

    CHECK(requestsSeen == 3);
    CHECK(targetedCallId != 0);
    // Exactly one reply frame crossed to the client over those two calls —
    // call 3's. Call 2's was swallowed, and nothing else was.
    CHECK(rig.proxy->repliesForwarded() - forwardedBefore == 1);

    // And call 2 stays unsettled: neither resolved nor failed. (Pumping here
    // has already happened for call 3's round trip, so this is a second,
    // explicit budget on top of that.)
    CHECK_FALSE(::morph::ladder::testkit::pumpUntil([&] { return secondResolved || secondFailed; }, 500ms));
    CHECK_FALSE(secondResolved);
    CHECK_FALSE(secondFailed);
}

TEST_CASE("FaultProxy::delayReply holds exactly the targeted call's reply, which still arrives",
          "[ladder][testkit][fault-proxy]") {
    // Above the rig, for the reason spelled out in the dropReply case: every
    // one of these is captured by reference into a lambda the rig outlives.
    // Both completions do settle before this test returns — but only if its
    // REQUIREs hold, and a failing REQUIRE unwinds the scope with a completion
    // still pending, which is exactly the case that must not become UB.
    constexpr auto kDelay = 600ms;
    int requestsSeen = 0;
    bool delayedResolved = false;
    bool promptResolved = false;

    ProxyRig rig;
    ::morph::bridge::BridgeHandler<FaultProbeCounter> handler{*rig.bridge, &rig.qtExec};

    rig.proxy->setRequestObserver([&](std::uint64_t callId, ::morph::ladder::testkit::FaultProxy& self) {
        if (++requestsSeen == 1) {
            self.delayReply(callId, kDelay);
        }
    });

    const auto issuedAt = std::chrono::steady_clock::now();
    handler.execute(FaultProbeAdd{1}).then([&](int) { delayedResolved = true; });
    handler.execute(FaultProbeAdd{2}).then([&](int) { promptResolved = true; });

    // The *second* call is untouched and comes back on its own schedule, while
    // the first is still parked in the proxy — that ordering is what makes this
    // "exactly call k is delayed" rather than "the link is slow".
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return promptResolved; }));
    CHECK_FALSE(delayedResolved);

    // The held reply is delayed, not lost: it does arrive, and only after the
    // scripted delay has elapsed.
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return delayedResolved; }));
    const auto elapsed = std::chrono::steady_clock::now() - issuedAt;
    CHECK(elapsed >= kDelay - 50ms);

    // Both calls' effects are on the server exactly once.
    CHECK(::morph::ladder::testkit::awaitQt(handler.execute(FaultProbeAdd{0})) == 3);
}

TEST_CASE("FaultProxy::duplicateReply delivers the reply twice on the wire but resolves the Completion once",
          "[ladder][testkit][fault-proxy]") {
    // Above the rig — see the dropReply case.
    int requestsSeen = 0;
    int thenCount = 0;
    int observedValue = 0;

    ProxyRig rig;
    ::morph::bridge::BridgeHandler<FaultProbeCounter> handler{*rig.bridge, &rig.qtExec};

    rig.proxy->setRequestObserver([&](std::uint64_t callId, ::morph::ladder::testkit::FaultProxy& self) {
        if (++requestsSeen == 1) {
            self.duplicateReply(callId);
        }
    });

    const std::uint64_t forwardedBefore = rig.proxy->repliesForwarded();
    handler.execute(FaultProbeAdd{5}).then([&](int value) {
        ++thenCount;
        observedValue = value;
    });

    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return thenCount >= 1; }));
    CHECK(observedValue == 5);

    // The duplicate really did go out on the wire — without this the
    // single-invocation assertion below would pass just as happily against a
    // proxy that quietly forwarded one copy.
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return rig.proxy->repliesForwarded() - forwardedBefore >= 2; }));
    CHECK(rig.proxy->repliesForwarded() - forwardedBefore == 2);

    // The second copy of the reply must not re-fire the callback:
    // QtWebSocketBackend erases the pending entry when the first copy lands, so
    // the duplicate finds no match and is dropped. A `thenCount` of 2 here
    // would be a framework finding, not a test bug.
    CHECK_FALSE(::morph::ladder::testkit::pumpUntil([&] { return thenCount >= 2; }, 400ms));
    CHECK(thenCount == 1);

    // A duplicated *reply* is not a duplicated *execution*: the server ran the
    // action once, so the running total is still 5.
    CHECK(::morph::ladder::testkit::awaitQt(handler.execute(FaultProbeAdd{0})) == 5);
}

TEST_CASE("FaultProxy: a second client connection replaces the first, still working end-to-end",
          "[ladder][testkit][fault-proxy]") {
    ProxyRig rig;
    ::morph::bridge::BridgeHandler<FaultProbeCounter> handler{*rig.bridge, &rig.qtExec};
    CHECK(::morph::ladder::testkit::awaitQt(handler.execute(FaultProbeAdd{1})) == 1);

    // A second backend connects to the same proxy URL while the first client
    // socket is still live from the proxy's perspective — onClientConnection()
    // must tear down the old leg and adopt the new one instead of crashing or
    // silently keeping both. This is the shape a real reconnect after
    // killAfter takes (a fresh connection replacing an aborted one); this test
    // doesn't need killAfter to reach it, just two connections in sequence.
    auto secondBackend = std::make_unique<::morph::qt::QtWebSocketBackend>(
        rig.proxy->url(), std::nullopt, ::morph::qt::QtWebSocketBackend::Config{.reconnectEnabled = false});
    REQUIRE(secondBackend->waitForConnected());
    ::morph::bridge::Bridge secondBridge{std::move(secondBackend)};
    ::morph::bridge::BridgeHandler<FaultProbeCounter> secondHandler{secondBridge, &rig.qtExec};

    // The replacement leg genuinely relays end-to-end through the proxy. A
    // fresh connection registers its own model instance server-side (models
    // here are per-registration, not shared across connections unless
    // registered that way), so this is 1 (0+1 on the new instance), not 2 —
    // the point of this assertion is that the call resolves through the
    // *new* leg at all, not that state carried over from the old one.
    CHECK(::morph::ladder::testkit::awaitQt(secondHandler.execute(FaultProbeAdd{1})) == 1);
}

TEST_CASE("FaultProxy: an undecodable client frame is forwarded unreported, not dropped or crashed on",
          "[ladder][testkit][fault-proxy]") {
    ProxyRig rig;

    bool observerCalled = false;
    rig.proxy->setRequestObserver(
        [&](std::uint64_t, ::morph::ladder::testkit::FaultProxy&) { observerCalled = true; });

    // A raw socket, not a QtWebSocketBackend: the backend only ever emits
    // well-formed wire::Envelopes, so reaching onClientTextMessage's
    // undecodable-frame branch needs a client that can send genuine garbage.
    QWebSocket raw;
    QString reply;
    bool gotReply = false;
    QObject::connect(&raw, &QWebSocket::textMessageReceived, [&](const QString& msg) {
        reply = msg;
        gotReply = true;
    });
    raw.open(rig.proxy->url());
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return raw.state() == QAbstractSocket::ConnectedState; }));

    raw.sendTextMessage(QStringLiteral("not-json-and-not-a-wire-envelope"));

    // The garbage frame is still forwarded upstream (onClientTextMessage's
    // undecodable branch only skips reporting it to the observer, per its own
    // comment) — the real server replies with its own protocol-level error,
    // proving the frame reached it rather than being silently swallowed here.
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return gotReply; }));
    CHECK_FALSE(observerCalled);

    raw.close();
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return raw.state() == QAbstractSocket::UnconnectedState; }));
}

TEST_CASE("FaultProxy::killAfter drops the connection instead of the targeted reply, and the client sees it",
          "[ladder][testkit][fault-proxy]") {
    // Above the rig — see the dropReply case. `disconnected` especially: the
    // backend owns the handler that writes it, and the backend is destroyed
    // inside `~ProxyRig`.
    std::atomic<bool> disconnected{false};
    int requestsSeen = 0;
    bool resolved = false;
    bool failed = false;

    ProxyRig rig;
    ::morph::bridge::BridgeHandler<FaultProbeCounter> handler{*rig.bridge, &rig.qtExec};

    // Observed on the *client*, through QtWebSocketBackend's own disconnect
    // notification (the [issue29] pattern in tests/qt/test_qt_websocket.cpp) —
    // not by inspecting the proxy's or the server's side of the socket.
    rig.backend->setDisconnectHandler([&] { disconnected.store(true); });

    rig.proxy->setRequestObserver([&](std::uint64_t callId, ::morph::ladder::testkit::FaultProxy& self) {
        if (++requestsSeen == 1) {
            self.killAfter(callId);
        }
    });

    const std::uint64_t forwardedBefore = rig.proxy->repliesForwarded();
    handler.execute(FaultProbeAdd{1}).then([&](int) { resolved = true; }).onError([&](const std::exception_ptr&) {
        failed = true;
    });

    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return disconnected.load(); }));
    CHECK(requestsSeen == 1);
    // The connection died *instead of* the reply being forwarded.
    CHECK(rig.proxy->repliesForwarded() == forwardedBefore);

    // The reply died with the connection: the call fails rather than resolving.
    REQUIRE(::morph::ladder::testkit::pumpUntil([&] { return failed; }));
    CHECK_FALSE(resolved);
}

// Forcing a real listen() failure or a genuinely-null nextPendingConnection()
// deterministically isn't practically achievable without flakiness or a
// test-only seam on Qt's own socket classes — the decision logic that would
// run in either case is factored into these two plain functions instead, so
// it's what gets tested. See their doc comments in fault_proxy.hpp.
TEST_CASE("FaultProxy's throwIfFaultProxyListenFailed throws exactly when its argument is false",
          "[ladder][testkit][fault-proxy]") {
    REQUIRE_THROWS_AS(::morph::ladder::testkit::detail::throwIfFaultProxyListenFailed(false), std::runtime_error);
    REQUIRE_NOTHROW(::morph::ladder::testkit::detail::throwIfFaultProxyListenFailed(true));
}

TEST_CASE("isValidIncomingConnection rejects null, accepts non-null", "[ladder][testkit][fault-proxy]") {
    REQUIRE_FALSE(::morph::ladder::testkit::detail::isValidIncomingConnection(nullptr));

    QWebSocket socket;
    REQUIRE(::morph::ladder::testkit::detail::isValidIncomingConnection(&socket));
}

// A real trusted upstream server never emits an undecodable reply, so
// onUpstreamTextMessage's catch branch is otherwise unreachable from an
// integration test — decodeCallIdOrZero is what's tested directly instead.
// See its doc comment in fault_proxy.hpp.
TEST_CASE("decodeCallIdOrZero round-trips a valid envelope's callId, and is 0 for garbage",
          "[ladder][testkit][fault-proxy]") {
    const QString validReply = QString::fromStdString(::morph::wire::encode(::morph::wire::makeOk(/*callId=*/7)));
    CHECK(::morph::ladder::testkit::detail::decodeCallIdOrZero(validReply) == 7);

    CHECK(::morph::ladder::testkit::detail::decodeCallIdOrZero(QStringLiteral("not-json-and-not-a-wire-envelope")) ==
          0);
}
