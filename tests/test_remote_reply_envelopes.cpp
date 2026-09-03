// SPDX-License-Identifier: Apache-2.0
//
// What `RemoteServer` actually puts on the wire, read per envelope kind.
//
// Every control envelope `dispatchMessage` switches on answers with exactly one
// reply, and that reply is the whole of the request's observable behaviour for
// an out-of-process client: `kind` says accepted or refused, `callId` is the
// only thing correlating an answer to its question, and one further field
// carries the answer itself -- `modelId` for the register/attach/assign family,
// `body` for the two read channels, `message` for a refusal.
//
// The suite reaches all of these branches already, through the bridge, the
// shared-instance directory and the limit policy. What it does not do at most
// of them is read the reply: it takes the one field it needs and lets the rest
// go unexamined. morph#405's mutation run measured what that costs -- the
// `reply(...)` call itself could be deleted at twenty sites in `remote.hpp`
// and `morph_tests` stayed green, because no case at those sites asserts on
// the envelope the deleted call would have produced.
//
// So this file reads the reply, once per kind, on both the accepting and the
// refusing side. It is deliberately one file rather than assertions scattered
// into the twenty tests that happen to drive these paths: the property is
// per-kind and belongs somewhere a newly added kind is obviously missing from.
//
// `handleInline` is used throughout because every kind here is a control
// envelope, which it answers synchronously on the calling thread -- the reply
// is its return value, so there is no waiting and nothing to race.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>
#include <vector>

// Model, action and result types need external linkage for glaze's reflection
// and for the BRIDGE_REGISTER_* macros, so they sit at namespace scope.
// NOLINTBEGIN(misc-use-internal-linkage)

/// @brief The keyed counter these cases register, attach to and enumerate.
struct RreCounterState {
    std::int64_t value = 0;
};

/// @brief The keyed action; its `id` is the instance's primary key.
struct RreAddTo {
    std::int64_t id = 0;
    std::int64_t amount = 0;
};

struct RreCounterModel {
    std::int64_t value = 0;

    RreCounterState execute(const RreAddTo& act) {
        value += act.amount;
        return {.value = value};
    }
};

BRIDGE_REGISTER_MODEL(RreCounterModel, "RRE_CounterModel")
// The generated fromJson body declares a non-const `WireClampScope`, which
// lives in the macro in registry.hpp rather than here.
// NOLINTNEXTLINE(misc-const-correctness)
BRIDGE_REGISTER_ACTION(RreCounterModel, RreAddTo, "RRE_AddTo")
BRIDGE_MODEL_KEY(RreCounterModel, RreAddTo, &RreAddTo::id);

// NOLINTEND(misc-use-internal-linkage)

namespace {

/// @brief Refuses everything, so every kind's own gate answers `unauthorized`.
struct RreDenyAll : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return false;
    }
    [[nodiscard]] bool authorizeRegister(const morph::session::Context& /*ctx*/,
                                         std::string_view /*modelType*/) const override {
        return false;
    }
};

/// @brief Admits registration but refuses per-instance access, which is the
///        only combination that reaches `deregister`'s own gate: a server that
///        refuses `authorizeRegister` never lets a model exist to deregister.
struct RreDenyInstance : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }
    [[nodiscard]] bool authorizeRegister(const morph::session::Context& /*ctx*/,
                                         std::string_view /*modelType*/) const override {
        return true;
    }
    [[nodiscard]] bool authorizeInstance(const morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                         std::string_view /*actionType*/, std::uint64_t /*modelId*/,
                                         std::string_view /*ownerPrincipal*/) const override {
        return false;
    }
};

/// @brief Sends @p env with @p callId stamped on it and decodes the reply.
/// @param server Server under test.
/// @param env    Request envelope; its `callId` is overwritten.
/// @param callId Correlation id to stamp and expect back.
/// @param cid    Connection scope; `0` is unscoped.
/// @return The decoded reply envelope.
// The two trailing ids are both plain integers and so genuinely swappable; they
// are kept in this order because every call site passes a callId and most omit
// the scope entirely.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] morph::wire::Envelope ask(morph::backend::RemoteServer& server, morph::wire::Envelope env,
                                        std::uint64_t callId, morph::backend::ConnectionId cid = 0) {
    env.callId = callId;
    return morph::wire::decode(server.handleInline(morph::wire::encode(env), cid));
}
// NOLINTEND(bugprone-easily-swappable-parameters)

/// @brief A connection id that was never opened, so every scope lookup misses.
///
/// `openConnection` hands out ids from a counter that starts at 1, so a value
/// this large cannot collide with a live scope in a test process.
constexpr morph::backend::ConnectionId kNeverOpened = 1'000'000;

}  // namespace

// ── register ─────────────────────────────────────────────────────────────────

TEST_CASE("RemoteServer: register answers ok, correlated, carrying the new model id", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto const reply = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 11);

    CHECK(reply.kind == "ok");
    CHECK(reply.callId == 11U);    // the client's only way to route this answer
    CHECK(reply.modelId != 0U);    // and the only thing the answer says
    CHECK(reply.message.empty());  // an ok never carries a diagnostic
    CHECK(reply.body.empty());     // nor a body: register has nothing to return
}

TEST_CASE("RemoteServer: a refused register answers err unauthorized, correlated", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<RreDenyAll>());

    auto const reply = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 12);

    CHECK(reply.kind == "err");
    CHECK(reply.callId == 12U);
    CHECK(reply.message == "unauthorized");
    CHECK(reply.modelId == 0U);  // nothing was created, so nothing is named
}

TEST_CASE("RemoteServer: a register over the live-model cap answers err too many models", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->setLimitPolicy({.maxLiveModels = 1});

    auto const first = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 20);
    REQUIRE(first.kind == "ok");

    auto const second = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 21);
    CHECK(second.kind == "err");
    CHECK(second.callId == 21U);
    CHECK(second.message == "too many models");
    CHECK(second.modelId == 0U);
}

TEST_CASE("RemoteServer: a register on a scope that is already gone answers err connection closed",
          "[remote][reply]") {
    // The instance is not created and then leaked: the caller is told, on a
    // connection that is already dead, which is what makes the reply worth
    // producing at all -- a transport with a queued write still drains it.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto const reply = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 22, kNeverOpened);

    CHECK(reply.kind == "err");
    CHECK(reply.callId == 22U);
    CHECK(reply.message == "connection closed");
    CHECK(reply.modelId == 0U);
}

TEST_CASE("RemoteServer: register during shutdown answers err server shutting down", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->beginShutdown();

    auto const reply = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 23);

    CHECK(reply.kind == "err");
    CHECK(reply.callId == 23U);
    CHECK(reply.message == "server shutting down");
}

// ── attach / shared register ─────────────────────────────────────────────────

TEST_CASE("RemoteServer: attach answers ok with a fresh id, then with the same id", "[remote][reply]") {
    // The two replies are produced by two different call sites -- the
    // directory-miss path that creates the instance, and the directory-hit path
    // that only takes another reference -- and the property that makes sharing
    // sharing is that a client cannot tell them apart except by the id being
    // the same one.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto const created = ask(*server, morph::wire::makeAttach("RRE_CounterModel", "7"), 30);
    CHECK(created.kind == "ok");
    CHECK(created.callId == 30U);
    CHECK(created.modelId != 0U);
    CHECK(created.message.empty());

    auto const rejoined = ask(*server, morph::wire::makeAttach("RRE_CounterModel", "7"), 31);
    CHECK(rejoined.kind == "ok");
    CHECK(rejoined.callId == 31U);
    CHECK(rejoined.modelId == created.modelId);
    CHECK(rejoined.message.empty());
}

TEST_CASE("RemoteServer: a refused attach answers err unauthorized, correlated", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<RreDenyAll>());

    auto const reply = ask(*server, morph::wire::makeAttach("RRE_CounterModel", "7"), 32);

    CHECK(reply.kind == "err");
    CHECK(reply.callId == 32U);
    CHECK(reply.message == "unauthorized");
    CHECK(reply.modelId == 0U);
}

TEST_CASE("RemoteServer: a first attach over the cap answers err too many models", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->setLimitPolicy({.maxLiveModels = 1});

    REQUIRE(ask(*server, morph::wire::makeAttach("RRE_CounterModel", "A"), 33).kind == "ok");

    // A different key needs a second instance, and the cap is the admission
    // check inside the directory-miss path -- not the early one register uses,
    // which a shared request skips precisely because it may create nothing.
    auto const refused = ask(*server, morph::wire::makeAttach("RRE_CounterModel", "B"), 34);
    CHECK(refused.kind == "err");
    CHECK(refused.callId == 34U);
    CHECK(refused.message == "too many models");
    CHECK(refused.modelId == 0U);
}

TEST_CASE("RemoteServer: attach on a scope that is already gone answers err connection closed", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    // Directory miss: the instance is built, then refused admission because the
    // scope it would be attributed to no longer exists.
    auto const onMiss = ask(*server, morph::wire::makeAttach("RRE_CounterModel", "C"), 35, kNeverOpened);
    CHECK(onMiss.kind == "err");
    CHECK(onMiss.callId == 35U);
    CHECK(onMiss.message == "connection closed");

    // Directory hit: the same refusal from the other call site. The instance
    // exists (created unscoped just above), so this is the attach-to-existing
    // path rather than the creating one.
    REQUIRE(ask(*server, morph::wire::makeAttach("RRE_CounterModel", "D"), 36).kind == "ok");
    auto const onHit = ask(*server, morph::wire::makeAttach("RRE_CounterModel", "D"), 37, kNeverOpened);
    CHECK(onHit.kind == "err");
    CHECK(onHit.callId == 37U);
    CHECK(onHit.message == "connection closed");
}

// ── assign ───────────────────────────────────────────────────────────────────

TEST_CASE("RemoteServer: assign answers ok echoing the id it filed", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto const reg = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 40);
    REQUIRE(reg.kind == "ok");

    auto const filed = ask(*server, morph::wire::makeAssign("RRE_CounterModel", "42", reg.modelId), 41);
    CHECK(filed.kind == "ok");
    CHECK(filed.callId == 41U);
    // assign never creates: the id in the reply is the one the request named,
    // which is what tells the client the promotion applied to *its* instance.
    CHECK(filed.modelId == reg.modelId);

    // And the key now resolves to it.
    auto const rejoined = ask(*server, morph::wire::makeAttach("RRE_CounterModel", "42"), 42);
    CHECK(rejoined.kind == "ok");
    CHECK(rejoined.modelId == reg.modelId);
}

TEST_CASE("RemoteServer: a refused assign answers err unauthorized, correlated", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<RreDenyAll>());

    auto const reply = ask(*server, morph::wire::makeAssign("RRE_CounterModel", "42", 1), 43);

    CHECK(reply.kind == "err");
    CHECK(reply.callId == 43U);
    CHECK(reply.message == "unauthorized");
}

// ── instances ────────────────────────────────────────────────────────────────

TEST_CASE("RemoteServer: instances answers ok whose body is the live key set", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto const empty = ask(*server, morph::wire::makeInstances("RRE_CounterModel"), 50);
    CHECK(empty.kind == "ok");
    CHECK(empty.callId == 50U);
    CHECK(empty.body == "[]");  // an empty directory is an empty list, not an absent body

    REQUIRE(ask(*server, morph::wire::makeAttach("RRE_CounterModel", "k1"), 51).kind == "ok");

    auto const listed = ask(*server, morph::wire::makeInstances("RRE_CounterModel"), 52);
    CHECK(listed.kind == "ok");
    CHECK(listed.callId == 52U);
    std::vector<std::string> keys{};
    REQUIRE_FALSE(glz::read_json(keys, listed.body));
    CHECK(keys == std::vector<std::string>{"k1"});
}

TEST_CASE("RemoteServer: a refused instances answers err unauthorized, correlated", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<RreDenyAll>());

    auto const reply = ask(*server, morph::wire::makeInstances("RRE_CounterModel"), 53);

    CHECK(reply.kind == "err");
    CHECK(reply.callId == 53U);
    CHECK(reply.message == "unauthorized");
    CHECK(reply.body.empty());  // a refusal discloses no part of the key set
}

// ── schemas ──────────────────────────────────────────────────────────────────

TEST_CASE("RemoteServer: schemas answers ok whose body is the type's schema document", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto const reply = ask(*server, morph::wire::makeSchemas("RRE_CounterModel"), 60);

    CHECK(reply.kind == "ok");
    CHECK(reply.callId == 60U);
    REQUIRE_FALSE(reply.body.empty());
    // The body is the served description of this type's actions -- the thing a
    // renderer builds a form from, so its emptiness would be silent breakage.
    CHECK(reply.body.contains("RRE_AddTo"));
    glz::generic_u64 dom{};
    CHECK_FALSE(glz::read_json(dom, reply.body));
}

TEST_CASE("RemoteServer: a refused schemas answers err unauthorized and discloses nothing", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<RreDenyAll>());

    auto const reply = ask(*server, morph::wire::makeSchemas("RRE_CounterModel"), 61);

    CHECK(reply.kind == "err");
    CHECK(reply.callId == 61U);
    CHECK(reply.message == "unauthorized");
    // The schema is a real disclosure -- field names, bounds and payload
    // fingerprints -- so a refusal must not carry a partial one.
    CHECK(reply.body.empty());
}

// ── deregister ───────────────────────────────────────────────────────────────

TEST_CASE("RemoteServer: deregister answers a bare correlated ok", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto const reg = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 70);
    REQUIRE(reg.kind == "ok");

    auto const gone = ask(*server, morph::wire::makeDeregister(reg.modelId), 71);
    CHECK(gone.kind == "ok");
    CHECK(gone.callId == 71U);
    // Nothing else: deregister names no model in its answer and returns no
    // body. This is the acknowledgement a client waits on before considering
    // the instance released, so its arrival is the whole content.
    CHECK(gone.modelId == 0U);
    CHECK(gone.body.empty());
    CHECK(gone.message.empty());
}

TEST_CASE("RemoteServer: a deregister the owner gate refuses answers err unauthorized", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<RreDenyInstance>());

    auto const reg = ask(*server, morph::wire::makeRegister("RRE_CounterModel"), 72);
    REQUIRE(reg.kind == "ok");

    auto const refused = ask(*server, morph::wire::makeDeregister(reg.modelId), 73);
    CHECK(refused.kind == "err");
    CHECK(refused.callId == 73U);
    CHECK(refused.message == "unauthorized");

    // And the refusal is real: the instance is still there to be attached to
    // by id, rather than destroyed and then reported as refused.
    auto const stillThere = ask(*server, morph::wire::makeDeregister(reg.modelId), 74);
    CHECK(stillThere.kind == "err");
    CHECK(stillThere.message == "unauthorized");
}

// ── hello, and the kind the server does not know ─────────────────────────────

TEST_CASE("RemoteServer: hello answers ok whose body is the supported version range", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->setSupportedVersionRange(2, 5);

    auto const reply = ask(*server, morph::wire::makeHello(3), 80);

    CHECK(reply.kind == "ok");
    CHECK(reply.callId == 80U);
    morph::wire::ProtocolRange range{};
    REQUIRE_FALSE(glz::read_json(range, reply.body));
    // The range is what the client negotiates against; announcing the wrong one
    // (or none) makes a peer downgrade or refuse for no reason.
    CHECK(range.min == 2U);
    CHECK(range.max == 5U);
}

TEST_CASE("RemoteServer: hello outside the range answers err protocol version unsupported", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->setSupportedVersionRange(2, 5);

    auto const tooOld = ask(*server, morph::wire::makeHello(1), 81);
    CHECK(tooOld.kind == "err");
    CHECK(tooOld.callId == 81U);
    CHECK(tooOld.message == "protocol version unsupported");
    CHECK(tooOld.body.empty());

    auto const tooNew = ask(*server, morph::wire::makeHello(6), 82);
    CHECK(tooNew.kind == "err");
    CHECK(tooNew.callId == 82U);
    CHECK(tooNew.message == "protocol version unsupported");
}

TEST_CASE("RemoteServer: an unknown kind answers err naming the kind, correlated", "[remote][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::wire::Envelope odd{};
    odd.kind = "subscribe";
    auto const reply = ask(*server, odd, 90);

    CHECK(reply.kind == "err");
    CHECK(reply.callId == 90U);
    // Naming the kind is what lets `interpretHelloReply` tell a legacy peer
    // apart from a real rejection, so the message is contract, not prose.
    CHECK(reply.message == "unknown envelope kind: subscribe");
}
