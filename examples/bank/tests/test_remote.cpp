// SPDX-License-Identifier: Apache-2.0
//
// Demonstrates morph's headline property: the *same* models, DTOs, and call
// sites work unchanged over a remote backend. Here we drive AccountModel
// through a SimulatedRemoteBackend (actions are serialised to JSON, dispatched
// on a server, and the typed result comes back), and install a real
// IAuthorizer that rejects one action type.

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <string_view>

#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/customer_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" + (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

/// Authorizer that forbids closing accounts but allows everything else.
struct NoCloseAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context& /*ctx*/, std::string_view /*model*/,
                                 std::string_view actionType) const override {
        return actionType != "CloseAccount";
    }

    // This test-only authorizer does no real token verification, but it must
    // still vouch for the caller: the base IAuthorizer::authenticate()
    // returns nullopt, and RemoteServer clears Context::principal to empty
    // for any authorizer that does not authenticate (see
    // docs/spec/session/session.md, "The `authorizeRegister` hook"), so the
    // model would otherwise never see "olivia-remote" as the owner. A real
    // deployment would verify a token here (see SigningAuthorizer,
    // session_auth.hpp) instead of trusting the client's claim outright.
    [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
        if (ctx.principal.empty()) {
            return std::nullopt;
        }
        return ctx.principal;
    }
};

}  // namespace

TEST_CASE("AccountModel runs unchanged over a remote backend", "[remote]") {
    bank::testing::ensureDatabase();
    (void)testConnection();  // ensure the shared DB is configured

    morph::exec::ThreadPoolExecutor serverPool{2};
    morph::exec::MainThreadExecutor gui;

    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, std::make_shared<NoCloseAuthorizer>());
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    morph::session::Context ctx;
    ctx.principal = "olivia-remote";
    bridge.setDefaultSession(ctx);
    // No App here to provision the principal, so ensure its users row exists.
    bank::testing::ensurePrincipal("olivia-remote");

    morph::bridge::BridgeHandler<bank::AccountModel> accounts{bridge, &gui};
    morph::bridge::BridgeHandler<bank::CustomerModel> accountsOwner{bridge, &gui};

    SECTION("opening an account round-trips through JSON serialisation") {
        auto info = await(accountsOwner.execute(bank::dto::OpenAccount{
                              .kind = static_cast<int>(bank::AccountKind::Savings),
                              .currency = static_cast<int>(bank::Currency::GBP),
                          }),
                          gui);
        REQUIRE(info.id > 0);
        REQUIRE(info.owner == "olivia-remote");
        REQUIRE(info.currency == static_cast<int>(bank::Currency::GBP));
    }

    SECTION("the authorizer rejects the forbidden action") {
        auto info = await(accountsOwner.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), gui);
        REQUIRE_THROWS(await(accounts.execute(bank::dto::CloseAccount{.id = info.id}), gui));
    }
}
