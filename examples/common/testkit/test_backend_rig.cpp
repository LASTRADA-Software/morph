// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstddef>
#include <morph/core/bridge.hpp>
#include <morph/core/wire.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>

#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

namespace {

/// @brief Denies every registration — proves BackendRig{Mode::Socket, N,
///        authorizer} genuinely threads the authorizer through to the
///        RemoteServer it builds, rather than silently ignoring it.
class DenyAllAuthorizer : public morph::session::IAuthorizer {
public:
    // authorize() is IAuthorizer's one pure-virtual hook (dispatch-time
    // gating); this test only exercises the registration-time hook below, so
    // this stays permissive, matching AllowAllAuthorizer's own default.
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        return true;
    }

    [[nodiscard]] bool authorizeRegister(const morph::session::Context&, std::string_view) const override {
        return false;
    }
};

}  // namespace

// Deliberately at namespace scope, not inside an anonymous namespace: glz's
// reflection (which BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION rely on to
// serialize these types across the wire, exercised by Mode::Socket) needs
// external linkage on the type — see glaze/reflection/get_name.hpp's
// `extern const T external` — so an anonymous-namespace type fails to link.
struct RigProbeAction {
    int value = 0;
};
struct RigProbeModel {
    int execute(RigProbeAction action) { return action.value * 2; }
};

BRIDGE_REGISTER_MODEL(RigProbeModel, "RigProbeModel")
BRIDGE_REGISTER_ACTION(RigProbeModel, RigProbeAction, "RigProbeAction")

// Stateful accumulator, mirroring tests/qt/test_qt_websocket.cpp's
// WsCounterModel/WsAddAction. RigProbeModel above is a pure function of its
// action (execute() reads no member state), so a test built on it cannot tell
// genuine per-client instance isolation apart from every client accidentally
// sharing one instance — the two are indistinguishable when nothing
// accumulates. This model's running total only comes out right, per client,
// if each client truly owns its own instance.
struct RigAddAction {
    int by = 0;
};
struct RigCounterModel {
    int value = 0;
    int execute(RigAddAction action) {
        value += action.by;
        return value;
    }
};

BRIDGE_REGISTER_MODEL(RigCounterModel, "RigCounterModel")
BRIDGE_REGISTER_ACTION(RigCounterModel, RigAddAction, "RigAddAction")

// Carries an arbitrarily large payload, so a test can push one action frame
// past a configured QtWebSocketServerConfig::maxMessageBytes. RigProbeAction's
// lone int cannot: no value of it produces a frame big enough to trip any
// cap a server would plausibly be configured with.
struct RigBlobAction {
    std::string blob;
};
struct RigBlobModel {
    std::size_t execute(RigBlobAction action) { return action.blob.size(); }
};

BRIDGE_REGISTER_MODEL(RigBlobModel, "RigBlobModel")
BRIDGE_REGISTER_ACTION(RigBlobModel, RigBlobAction, "RigBlobAction")

TEST_CASE("BackendRig: one action round-trips in every mode", "[ladder][testkit][rig]") {
    auto mode = GENERATE(morph::ladder::testkit::Mode::Local, morph::ladder::testkit::Mode::LocalSingleThread,
                         morph::ladder::testkit::Mode::Socket);

    morph::ladder::testkit::BackendRig rig{mode, /*nClients=*/1};
    auto handler = rig.client<RigProbeModel>(0);

    auto result = morph::ladder::testkit::awaitQt(handler.execute(RigProbeAction{21}));
    REQUIRE(result == 42);
}

TEST_CASE("BackendRig exposes bridge/executor/url so presenters compose over it", "[ladder][testkit][rig]") {
    auto mode = GENERATE(morph::ladder::testkit::Mode::Local, morph::ladder::testkit::Mode::LocalSingleThread,
                         morph::ladder::testkit::Mode::Socket);

    morph::ladder::testkit::BackendRig rig{mode, /*nClients=*/1};

    // The pair a Presenter subclass is constructed from — client<Model>()
    // hands out a pre-bound handler, which a presenter that builds its own
    // handlers cannot use.
    morph::bridge::BridgeHandler<RigProbeModel> handler{rig.bridge(0), rig.executor()};
    REQUIRE(morph::ladder::testkit::awaitQt(handler.execute(RigProbeAction{21})) == 42);

    if (mode == morph::ladder::testkit::Mode::Socket) {
        REQUIRE(rig.url().scheme() == "ws");
        REQUIRE(rig.url().port() > 0);
    } else {
        // No server, so no URL to hand out — a caller asking for one has a
        // mode confusion, not a missing value.
        REQUIRE_THROWS_AS(rig.url(), std::logic_error);
    }
}

TEST_CASE("BackendRig::Socket: N clients each get an isolated model instance", "[ladder][testkit][rig][socket-only]") {
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, /*nClients=*/3};

    // One handler per client, held for the whole test: each call to
    // rig.client<Model>(index) registers a fresh model instance, so getting
    // a handler once per client and driving several actions through it (as
    // opposed to re-fetching the handler for every action) is what actually
    // exercises one running total per client rather than one per call.
    auto handler0 = rig.client<RigCounterModel>(0);
    auto handler1 = rig.client<RigCounterModel>(1);
    auto handler2 = rig.client<RigCounterModel>(2);

    // Client 0 increments by 10 three times -> running total 10, 20, 30.
    int last0 = 0;
    for (int i = 0; i < 3; ++i) {
        last0 = morph::ladder::testkit::awaitQt(handler0.execute(RigAddAction{10}));
    }
    // Client 1 increments by 1 twice -> running total 1, 2.
    int last1 = 0;
    for (int i = 0; i < 2; ++i) {
        last1 = morph::ladder::testkit::awaitQt(handler1.execute(RigAddAction{1}));
    }
    // Client 2 increments by 5 four times -> running total 5, 10, 15, 20.
    int last2 = 0;
    for (int i = 0; i < 4; ++i) {
        last2 = morph::ladder::testkit::awaitQt(handler2.execute(RigAddAction{5}));
    }

    // Only genuine per-client isolation produces exactly these three totals:
    // if clients accidentally shared one server-side instance, each client's
    // total would be contaminated by the others' increments (e.g. client 1's
    // final value would include client 0's +10s), and these REQUIREs would
    // fail.
    REQUIRE(last0 == 30);
    REQUIRE(last1 == 2);
    REQUIRE(last2 == 20);
}

TEST_CASE("BackendRig::mode() reports the mode it was constructed with", "[ladder][testkit][rig]") {
    auto mode = GENERATE(morph::ladder::testkit::Mode::Local, morph::ladder::testkit::Mode::LocalSingleThread,
                         morph::ladder::testkit::Mode::Socket);
    morph::ladder::testkit::BackendRig rig{mode, /*nClients=*/1};
    REQUIRE(rig.mode() == mode);
}

TEST_CASE("BackendRig::Socket threads a custom authorizer through to the RemoteServer it builds",
          "[ladder][testkit][rig][socket-only]") {
    auto authorizer = std::make_shared<DenyAllAuthorizer>();
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, /*nClients=*/1, authorizer};

    // Registration itself is denied and throws synchronously from
    // BridgeHandler's constructor — if the authorizer were silently ignored
    // (the pre-fix default-allow behavior), this would construct cleanly
    // instead.
    REQUIRE_THROWS_WITH(rig.client<RigProbeModel>(0), Catch::Matchers::ContainsSubstring("unauthorized"));
}

TEST_CASE("BackendRig::Socket threads a custom QtWebSocketServerConfig through to the server it builds",
          "[ladder][testkit][rig][socket-only]") {
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.maxMessageBytes = 1024;  // far below the 8 MiB wire cap the default carries
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, /*nClients=*/1,
                                           /*authorizer=*/nullptr, cfg};

    // Registration frames stay well under the cap, so the handler itself
    // constructs normally — only the oversized action frame below is refused,
    // by the transport, before it ever reaches the model.
    auto handler = rig.client<RigBlobModel>(0);

    REQUIRE(morph::ladder::testkit::awaitQt(handler.execute(RigBlobAction{std::string(16, 'x')})) == 16);

    // If the config were silently dropped (the pre-extension behavior), this
    // 64 KiB frame would sail through the default 8 MiB cap and resolve with
    // its own size instead of rejecting.
    REQUIRE_THROWS_WITH(morph::ladder::testkit::awaitQt(handler.execute(RigBlobAction{std::string(64 * 1024, 'x')})),
                        Catch::Matchers::ContainsSubstring("maxMessageBytes"));
}

TEST_CASE("BackendRig::socketBackend() hands out the live backend, usable for hello negotiation",
          "[ladder][testkit][rig][socket-only]") {
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, /*nClients=*/2};

    // negotiateProtocolVersion() is transport-level and has no Bridge-level
    // equivalent — reaching it at all is the reason this accessor exists.
    REQUIRE(rig.socketBackend(0).negotiateProtocolVersion() == morph::wire::ProtocolNegotiationResult::Negotiated);
    REQUIRE(rig.socketBackend(1).negotiateProtocolVersion() == morph::wire::ProtocolNegotiationResult::Negotiated);

    // Still a working backend afterwards: negotiation is not a one-way door.
    auto handler = rig.client<RigProbeModel>(0);
    REQUIRE(morph::ladder::testkit::awaitQt(handler.execute(RigProbeAction{21})) == 42);
}

TEST_CASE("BackendRig::socketBackend() throws out_of_range past nClients, and logic_error off Socket mode",
          "[ladder][testkit][rig]") {
    {
        morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, /*nClients=*/1};
        REQUIRE_THROWS_AS(rig.socketBackend(1), std::out_of_range);
    }
    auto localMode = GENERATE(morph::ladder::testkit::Mode::Local, morph::ladder::testkit::Mode::LocalSingleThread);
    morph::ladder::testkit::BackendRig localRig{localMode, /*nClients=*/1};
    REQUIRE_THROWS_AS(localRig.socketBackend(0), std::logic_error);
}

TEST_CASE("BackendRig::client() throws out_of_range past nClients in Socket mode",
          "[ladder][testkit][rig][socket-only]") {
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, /*nClients=*/1};
    REQUIRE_THROWS_AS(rig.client<RigProbeModel>(1), std::out_of_range);
}

TEST_CASE("BackendRig::bridge() throws out_of_range past nClients in Socket mode",
          "[ladder][testkit][rig][socket-only]") {
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, /*nClients=*/1};
    REQUIRE_THROWS_AS(rig.bridge(1), std::out_of_range);
}

// Forcing a real listen()/waitForConnected() failure deterministically isn't
// practically achievable without flakiness or a test-only seam on
// QtWebSocketServer/QtWebSocketBackend themselves — the throw logic that
// would run on failure is factored into these two plain-bool functions
// instead, so it's what gets tested. See their doc comments in
// backend_rig.hpp for the full rationale.
TEST_CASE("throwIfListenFailed throws exactly when its argument is false", "[ladder][testkit][rig]") {
    REQUIRE_THROWS_AS(morph::ladder::testkit::detail::throwIfListenFailed(false), std::runtime_error);
    REQUIRE_NOTHROW(morph::ladder::testkit::detail::throwIfListenFailed(true));
}

TEST_CASE("throwIfConnectFailed throws exactly when its argument is false", "[ladder][testkit][rig]") {
    REQUIRE_THROWS_AS(morph::ladder::testkit::detail::throwIfConnectFailed(false), std::runtime_error);
    REQUIRE_NOTHROW(morph::ladder::testkit::detail::throwIfConnectFailed(true));
}

// A `QtDrivenMainThreadExecutor` destroyed with its zero-delay drain timer
// still pending must not touch its own storage when that timer fires. This is
// the exact shape that aborted the process before `_liveness` was added: a
// `Mode::LocalSingleThread` rig is routinely destroyed one event-loop turn
// after its last `post()`, and the *next* thing to spin the Qt loop —
// `BackendRig{Mode::Socket, ...}`'s `waitForConnected()`, or
// `~QtWebSocketBackend`'s own `processEvents()` — delivered the stale event
// into freed memory, threw `std::system_error{"mutex lock failed"}` out of a
// Qt event handler, and Qt turned that into `abort()`. Reverting the guard in
// `post()` makes this case abort rather than fail.
TEST_CASE("QtDrivenMainThreadExecutor's pending drain is inert after the executor is destroyed",
          "[ladder][testkit][rig]") {
    bool taskRan = false;
    {
        morph::ladder::testkit::detail::QtDrivenMainThreadExecutor executor;
        executor.post([&taskRan] { taskRan = true; });
        // Deliberately no pump here: the drain timer is left in flight, which
        // is precisely the state the crash needed.
    }
    // Spinning the loop now delivers the orphaned timer event. It must be a
    // no-op, not a use-after-free.
    REQUIRE_FALSE(morph::ladder::testkit::pumpUntil([] { return false; }, std::chrono::milliseconds{50}));
    CHECK_FALSE(taskRan);
}
