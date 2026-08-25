// SPDX-License-Identifier: Apache-2.0

// The `"schemas"` envelope kind (morph#234) and the action-evolution policy
// gate it feeds (morph#207).
//
// morph#234: a client that is not linked against a model's C++ had no way to
// ask what an action's inputs are -- `morph::forms::schemaJson<A>()` is a
// compile-time function and no envelope kind served its output.
//
// morph#207: the action codec's lenient decode matches fields by *name* and
// default-constructs an absent one, so a client/server field rename
// (`amountCents` -> `amount`) decodes to a zero-valued action that
// `validate()` cannot distinguish from a legitimate zero. The two belong
// together because the fix for the second reads the document served by the
// first: `PayloadCompleteness::RequireDeclaredFields` enforces exactly the
// `required` array a `"schemas"` reply publishes.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/payload_schema.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "test_support.hpp"

using morph::backend::PayloadCompleteness;
using morph::backend::RemoteServer;
using morph::testing::WaitReply;
using morph::wire::encode;
using morph::wire::Envelope;
using morph::wire::makeRegister;
using morph::wire::makeSchemas;

// Model/action types need external linkage for glaze reflection (see
// tests/test_policy_hardening.cpp).

/// The action morph#207 was reported against, in miniature: one mandatory
/// field carrying the whole meaning of the request, and one field the author
/// declared optional.
struct WireSchemasDeposit {
    std::int64_t amountCents = 0;
    std::string memo;

    static constexpr std::array optionalFields{std::string_view{"memo"}};
};

struct WireSchemasDepositResult {
    std::int64_t balanceCents = 0;
};

struct WireSchemasWithdraw {
    std::int64_t amountCents = 0;
};

struct WireSchemasLedgerModel {
    std::int64_t balanceCents = 0;

    WireSchemasDepositResult execute(const WireSchemasDeposit& action) {
        balanceCents += action.amountCents;
        return WireSchemasDepositResult{.balanceCents = balanceCents};
    }
    WireSchemasDepositResult execute(const WireSchemasWithdraw& action) {
        balanceCents -= action.amountCents;
        return WireSchemasDepositResult{.balanceCents = balanceCents};
    }
};

BRIDGE_REGISTER_MODEL(WireSchemasLedgerModel, "WireSchemas_Ledger")
BRIDGE_REGISTER_ACTION(WireSchemasLedgerModel, WireSchemasDeposit, "WireSchemas_Deposit")
BRIDGE_REGISTER_ACTION(WireSchemasLedgerModel, WireSchemasWithdraw, "WireSchemas_Withdraw")

namespace {

/// Denies `authorize` (the type-level read gate `"schemas"` and `"instances"`
/// share) for one model type, allowing everything else.
struct WireSchemasDescribeGatingAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view modelType,
                                 std::string_view) const override {
        return modelType != "WireSchemas_Ledger";
    }
    [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
        if (ctx.principal.empty()) {
            return std::nullopt;
        }
        return ctx.principal;
    }
};

std::uint64_t registerLedger(RemoteServer& server) {
    WaitReply reply;
    server.handle(encode(makeRegister("WireSchemas_Ledger")), std::ref(reply));
    REQUIRE(reply.await());
    REQUIRE(reply.env.kind == "ok");
    return reply.env.modelId;
}

Envelope depositEnv(std::uint64_t modelId, std::string body) {
    Envelope env;
    env.kind = "execute";
    env.callId = 1;
    env.modelId = modelId;
    env.modelType = "WireSchemas_Ledger";
    env.actionType = "WireSchemas_Deposit";
    env.body = std::move(body);
    return env;
}

/// Runs one `execute` to completion and hands back the decoded reply.
Envelope runExecute(RemoteServer& server, std::uint64_t modelId, const std::string& body) {
    WaitReply reply;
    server.handle(encode(depositEnv(modelId, body)), std::ref(reply));
    REQUIRE(reply.await());
    return reply.env;
}

std::string fetchSchemas(RemoteServer& server, const std::string& typeId) {
    WaitReply reply;
    server.handle(encode(makeSchemas(typeId)), std::ref(reply));
    REQUIRE(reply.await());
    REQUIRE(reply.env.kind == "ok");
    return reply.env.body;
}

}  // namespace

// ── wire layer: the "schemas" kind and its factory ───────────────────────────

TEST_CASE("makeSchemas builds a schemas envelope carrying the model type id", "[wire][schemas]") {
    auto env = makeSchemas("WireSchemas_Ledger");
    REQUIRE(env.kind == "schemas");
    REQUIRE(env.typeId == "WireSchemas_Ledger");
    // Nothing else is populated: `schemas` is a pure query over a type.
    REQUIRE(env.modelId == 0U);
    REQUIRE(env.body.empty());
}

TEST_CASE("a schemas envelope round-trips through encode/decode", "[wire][schemas]") {
    auto decoded = morph::wire::decode(encode(makeSchemas("WireSchemas_Ledger")));
    REQUIRE(decoded.kind == "schemas");
    REQUIRE(decoded.typeId == "WireSchemas_Ledger");
}

// ── RemoteServer: answering "schemas" ────────────────────────────────────────

TEST_CASE("RemoteServer serves a {actionType: schema} document for a registered model type",
          "[remote][schemas][issue234]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    const auto document = fetchSchemas(*server, "WireSchemas_Ledger");
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, document));
    REQUIRE(dom.is_object());

    // Every action registered for the type, each a schema a renderer can use.
    REQUIRE(dom.contains("WireSchemas_Deposit"));
    REQUIRE(dom.contains("WireSchemas_Withdraw"));
    REQUIRE(dom["WireSchemas_Deposit"].contains("properties"));
    REQUIRE(dom["WireSchemas_Deposit"]["properties"].contains("amountCents"));
    REQUIRE(dom["WireSchemas_Deposit"]["properties"].contains("memo"));
}

TEST_CASE("a served schema's required array names the mandatory fields and omits declared-optional ones",
          "[remote][schemas][issue234]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, fetchSchemas(*server, "WireSchemas_Ledger")));
    REQUIRE(dom["WireSchemas_Deposit"].contains("required"));
    const auto& required = dom["WireSchemas_Deposit"]["required"].get_array();
    REQUIRE(required.size() == 1);
    REQUIRE(required[0].get<std::string>() == "amountCents");
}

TEST_CASE("a served schema carries the action's payload fingerprint and shape", "[remote][schemas][issue207]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, fetchSchemas(*server, "WireSchemas_Ledger")));
    const auto& entry = dom["WireSchemas_Deposit"];
    REQUIRE(entry.contains("x-payloadFingerprint"));
    REQUIRE(entry.contains("x-payloadShape"));
    // The same values the journal stamps on an entry, so a client linked
    // against the action can compare its own build's shape against the
    // server's without a second vocabulary.
    REQUIRE(entry["x-payloadFingerprint"].get<std::string>() ==
            morph::model::payloadFingerprint<WireSchemasDeposit>());
    REQUIRE(entry["x-payloadShape"].get<std::string>() == morph::model::payloadShapeString<WireSchemasDeposit>());
}

TEST_CASE("schemas without a typeId is refused", "[remote][schemas]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    WaitReply reply;
    server->handle(encode(makeSchemas("")), std::ref(reply));
    REQUIRE(reply.await());
    REQUIRE(reply.env.kind == "err");
    REQUIRE(reply.env.message == "schemas requires a typeId");
}

TEST_CASE("schemas for a type with no registered actions is an empty document, not an error", "[remote][schemas]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    // Answering "unknown type" differently would tell an unauthenticated
    // prober which type ids are real.
    REQUIRE(fetchSchemas(*server, "WireSchemas_NoSuchType") == "{}");
}

TEST_CASE("schemas is gated by the same authorize() hook as instances", "[remote][schemas][auth]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto authz = std::make_shared<WireSchemasDescribeGatingAuthorizer>();
    auto server = std::make_shared<RemoteServer>(pool, authz);

    Envelope env = makeSchemas("WireSchemas_Ledger");
    env.session.principal = "alice";
    WaitReply reply;
    server->handle(encode(env), std::ref(reply));
    REQUIRE(reply.await());
    REQUIRE(reply.env.kind == "err");
    REQUIRE(reply.env.message == "unauthorized");
}

// ── ActionDispatcher: the description store behind the wire kind ─────────────

TEST_CASE("ActionDispatcher::schemasJson is an empty object for an unknown model type", "[registry][schemas]") {
    REQUIRE(morph::model::detail::ActionDispatcher::instance().schemasJson("WireSchemas_NoSuchType") == "{}");
}

TEST_CASE("ActionDispatcher::requiredFieldsFor reports the schema's required fields", "[registry][schemas]") {
    const auto* required = morph::model::detail::ActionDispatcher::instance().requiredFieldsFor("WireSchemas_Ledger",
                                                                                                "WireSchemas_Deposit");
    REQUIRE(required != nullptr);
    REQUIRE(required->size() == 1);
    REQUIRE(required->front() == "amountCents");
}

TEST_CASE("ActionDispatcher::requiredFieldsFor returns nullptr for an unregistered pair", "[registry][schemas]") {
    // nullptr means "nothing to check", never "nothing is required": an
    // unregistered action never published a requirement.
    REQUIRE(morph::model::detail::ActionDispatcher::instance().requiredFieldsFor(
                "WireSchemas_Ledger", "WireSchemas_NoSuchAction") == nullptr);
}

// ── PayloadCompleteness: enforcing the published action-evolution policy ─────

TEST_CASE("RemoteServer defaults to PayloadCompleteness::Lenient", "[remote][completeness][issue207]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    REQUIRE(server->payloadCompleteness() == PayloadCompleteness::Lenient);
}

TEST_CASE("REGRESSION GUARD: leniently, a renamed field is accepted and applies nothing",
          "[remote][completeness][issue207]") {
    // morph#207's measured defect, pinned as it stands today so the default
    // path cannot change without this failing. The client sends `amount`; the
    // server's action declares `amountCents`. The unknown key is dropped, the
    // absent one is default-constructed, and the deposit silently applies 0.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    const auto modelId = registerLedger(*server);

    auto reply = runExecute(*server, modelId, R"({"amount":500})");
    REQUIRE(reply.kind == "ok");
    auto result = morph::model::ActionTraits<WireSchemasDeposit>::resultFromJson(reply.body);
    REQUIRE(result.balanceCents == 0);
}

TEST_CASE("RequireDeclaredFields rejects a renamed field by name", "[remote][completeness][issue207]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    server->setPayloadCompleteness(PayloadCompleteness::RequireDeclaredFields);
    const auto modelId = registerLedger(*server);

    auto reply = runExecute(*server, modelId, R"({"amount":500})");
    REQUIRE(reply.kind == "err");
    REQUIRE(reply.message == "payload missing required field(s): amountCents");
}

TEST_CASE("RequireDeclaredFields rejects an empty payload", "[remote][completeness][issue207]") {
    // The case `error_on_unknown_keys = true` cannot catch at all: `{}` has no
    // unknown key to trip over, only an absent required one.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    server->setPayloadCompleteness(PayloadCompleteness::RequireDeclaredFields);
    const auto modelId = registerLedger(*server);

    auto reply = runExecute(*server, modelId, "{}");
    REQUIRE(reply.kind == "err");
    REQUIRE(reply.message == "payload missing required field(s): amountCents");
}

TEST_CASE("RequireDeclaredFields dispatches a complete payload unchanged", "[remote][completeness][issue207]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    server->setPayloadCompleteness(PayloadCompleteness::RequireDeclaredFields);
    const auto modelId = registerLedger(*server);

    auto reply = runExecute(*server, modelId, R"({"amountCents":500,"memo":"rent"})");
    REQUIRE(reply.kind == "ok");
    auto result = morph::model::ActionTraits<WireSchemasDeposit>::resultFromJson(reply.body);
    REQUIRE(result.balanceCents == 500);
}

TEST_CASE("POLICY: RequireDeclaredFields still accepts a newer client's additive field",
          "[remote][completeness][issue207]") {
    // "Additive-only within a major version" is the first bullet of the very
    // policy this gate enforces, so the gate must not be a strict decode:
    // `error_on_unknown_keys = true` turns this legal payload into a parse
    // error, which is why morph#207 rules it out as a partial measure.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    server->setPayloadCompleteness(PayloadCompleteness::RequireDeclaredFields);
    const auto modelId = registerLedger(*server);

    auto reply = runExecute(*server, modelId, R"({"amountCents":500,"memoId":7})");
    REQUIRE(reply.kind == "ok");
    auto result = morph::model::ActionTraits<WireSchemasDeposit>::resultFromJson(reply.body);
    REQUIRE(result.balanceCents == 500);
}

TEST_CASE("POLICY: RequireDeclaredFields does not require a declared-optional field",
          "[remote][completeness][issue207]") {
    // The gate enforces the served `required` array and nothing else, so a
    // field the author put in `optionalFields` stays omissible.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    server->setPayloadCompleteness(PayloadCompleteness::RequireDeclaredFields);
    const auto modelId = registerLedger(*server);

    auto reply = runExecute(*server, modelId, R"({"amountCents":500})");
    REQUIRE(reply.kind == "ok");
}

TEST_CASE("RequireDeclaredFields leaves a non-object body to the action codec", "[remote][completeness][issue207]") {
    // A body that is not a JSON object has no keys to test. Reporting a
    // missing field there would replace `fromJson`'s precise parse diagnostic
    // with a misleading one.
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    server->setPayloadCompleteness(PayloadCompleteness::RequireDeclaredFields);
    const auto modelId = registerLedger(*server);

    auto reply = runExecute(*server, modelId, "\"not-an-object\"");
    REQUIRE(reply.kind == "err");
    REQUIRE(reply.message.find("payload missing required field") == std::string::npos);
}

// ── SimulatedRemoteBackend: the client-side accessor ─────────────────────────

TEST_CASE("SimulatedRemoteBackend::fetchActionSchemas returns the server's document", "[remote][schemas][issue234]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    morph::backend::SimulatedRemoteBackend backend{*server};

    REQUIRE(backend.fetchActionSchemas("WireSchemas_Ledger") == fetchSchemas(*server, "WireSchemas_Ledger"));
}

TEST_CASE("SimulatedRemoteBackend::fetchActionSchemas throws when the server refuses", "[remote][schemas][auth]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto authz = std::make_shared<WireSchemasDescribeGatingAuthorizer>();
    auto server = std::make_shared<RemoteServer>(pool, authz);
    morph::backend::SimulatedRemoteBackend backend{*server};

    REQUIRE_THROWS_AS(backend.fetchActionSchemas("WireSchemas_Ledger"), std::runtime_error);
}
