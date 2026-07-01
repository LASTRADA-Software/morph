// SPDX-License-Identifier: Apache-2.0
//
// Demonstrates morph's headline property: the *same* models, DTOs, and call
// sites work unchanged over a remote backend. Here we drive AccountModel
// through a SimulatedRemoteBackend (actions are serialised to JSON, dispatched
// on a server, and the typed result comes back), and install a real
// IAuthorizer that rejects one action type.

#include <catch2/catch_test_macros.hpp>

#include <morph/backend.hpp>
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/remote.hpp>
#include <morph/session.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank_test_support.hpp"

using bank::testing::await;

namespace {

std::string testConnection() {
    bank::testing::ensureDatabase();
    return "DRIVER=SQLite3;Database=" +
           (std::filesystem::temp_directory_path() / "morph_bank_tests.db").string();
}

/// Authorizer that forbids closing accounts but allows everything else.
struct NoCloseAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context& /*ctx*/, std::string_view /*model*/,
                                 std::string_view actionType) const override {
        return actionType != "CloseAccount";
    }
};

}  // namespace

TEST_CASE("AccountModel runs unchanged over a remote backend", "[remote]") {
    bank::testing::ensureDatabase();
    (void)testConnection();  // ensure the shared DB is configured

    morph::exec::ThreadPoolExecutor serverPool{2};
    morph::exec::MainThreadExecutor gui;

    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool,
                                                                 std::make_shared<NoCloseAuthorizer>());
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    morph::session::Context ctx;
    ctx.principal = "olivia-remote";
    bridge.setDefaultSession(ctx);
    // No App here to provision the principal, so ensure its users row exists.
    bank::testing::ensurePrincipal("olivia-remote");

    morph::bridge::BridgeHandler<bank::AccountModel> accounts{bridge, &gui};

    SECTION("opening an account round-trips through JSON serialisation") {
        auto info = await(accounts.execute(bank::dto::OpenAccount{
                              .kind = static_cast<int>(bank::AccountKind::Savings),
                              .currency = static_cast<int>(bank::Currency::GBP),
                          }),
                          gui);
        REQUIRE(info.id > 0);
        REQUIRE(info.owner == "olivia-remote");
        REQUIRE(info.currency == static_cast<int>(bank::Currency::GBP));
    }

    SECTION("the authorizer rejects the forbidden action") {
        auto info =
            await(accounts.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0}), gui);
        REQUIRE_THROWS(await(accounts.execute(bank::dto::CloseAccount{.id = info.id}), gui));
    }
}
