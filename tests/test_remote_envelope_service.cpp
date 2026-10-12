// SPDX-License-Identifier: Apache-2.0

// RemoteServer's envelope services: a kind the server does not know itself is
// answered by the first configured service that serves it, with the verified
// principal stamped first; a built-in kind is never handed to a service; a kind
// nobody serves is still `unknown envelope kind`.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using morph::wire::decode;
using morph::wire::encode;
using morph::wire::Envelope;

/// Answers `echo-*` kinds with the principal and kind it saw; throws on `echo-throw`.
class EchoService final : public morph::backend::IEnvelopeService {
public:
    [[nodiscard]] bool serves(std::string_view kind) const override {
        return kind.starts_with("echo-") || kind == "execute" || kind == "hello";
    }

    [[nodiscard]] std::string handle(Envelope const& request, morph::session::IAuthorizer const& authorizer) override {
        seen.push_back(request.kind);
        if (request.kind == "echo-throw") {
            throw std::runtime_error{"echo refused"};
        }
        if (!authorizer.authorize(request.session, "echo", request.kind)) {
            return encode(morph::wire::makeErr("unauthorized", request.callId));
        }
        return encode(morph::wire::makeOk(request.callId, request.session.principal + "|" + request.kind));
    }

    std::vector<std::string> seen;
};

/// Verifies a token by turning it into the principal, and refuses `echo-denied`.
struct TokenAuthorizer final : morph::session::IAuthorizer {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): IAuthorizer's signature.
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view modelType,
                                 std::string_view actionType) const override {
        static_cast<void>(ctx);
        return !(modelType == "echo" && actionType == "echo-denied");
    }

    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        if (ctx.token.empty()) {
            return std::nullopt;
        }
        return "verified:" + ctx.token;
    }
};

Envelope request(std::string kind, std::uint64_t callId) {
    Envelope env;
    env.kind = std::move(kind);
    env.callId = callId;
    return env;
}

}  // namespace

TEST_CASE("RemoteServer: a service answers the kinds it serves", "[remote][service]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto service = std::make_shared<EchoService>();
    morph::backend::ServerConfig config;
    config.services.push_back(service);
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<TokenAuthorizer>(), config);

    SECTION("with the verified principal, never the claim") {
        Envelope env = request("echo-ping", 7);
        env.session.principal = "mallory";
        env.session.token = "alice";
        Envelope const reply = decode(server->handleInline(encode(env)));
        REQUIRE(reply.kind == "ok");
        REQUIRE(reply.callId == 7U);
        REQUIRE(reply.body == "verified:alice|echo-ping");
    }

    SECTION("an unverified claim is cleared") {
        Envelope env = request("echo-ping", 8);
        env.session.principal = "mallory";
        Envelope const reply = decode(server->handleInline(encode(env)));
        REQUIRE(reply.body == "|echo-ping");
    }

    SECTION("the service authorises through the server's authoriser") {
        Envelope const reply = decode(server->handleInline(encode(request("echo-denied", 9))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message == "unauthorized");
    }

    SECTION("a throw becomes an err reply") {
        Envelope const reply = decode(server->handleInline(encode(request("echo-throw", 10))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.callId == 10U);
        REQUIRE(reply.message == "echo refused");
    }

    SECTION("a built-in kind is never handed to a service") {
        Envelope const reply = decode(server->handleInline(encode(morph::wire::makeHello())));
        REQUIRE(reply.kind == "ok");
        REQUIRE(service->seen.empty());
    }

    SECTION("a kind no service serves is still unknown") {
        Envelope const reply = decode(server->handleInline(encode(request("ui-hello", 11))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message == "unknown envelope kind: ui-hello");
    }
}

TEST_CASE("RemoteServer: services are consulted in order", "[remote][service]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto first = std::make_shared<EchoService>();
    auto second = std::make_shared<EchoService>();
    morph::backend::ServerConfig config;
    config.services = {first, second};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, config);
    Envelope const reply = decode(server->handleInline(encode(request("echo-ping", 1))));
    REQUIRE(reply.kind == "ok");
    REQUIRE(first->seen.size() == 1U);
    REQUIRE(second->seen.empty());
}

TEST_CASE("RemoteServer: a null service is refused at construction", "[remote][service]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::ServerConfig config;
    config.services.push_back(nullptr);
    REQUIRE_THROWS_AS(std::make_shared<morph::backend::RemoteServer>(pool, config), std::invalid_argument);
}
