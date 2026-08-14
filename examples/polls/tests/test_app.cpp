// SPDX-License-Identifier: Apache-2.0
//
// App's own suite: booting the server side (RemoteServer + PollsAuthorizer +
// FileActionLog) and confirming a real client -- not a direct
// `PollModel::execute()` call -- can dispatch `CreatePoll`/`OpenPoll` through
// it end to end. Mirrors `bookmarks::app::App`'s own `[bookmarks][app]` suite
// in spirit (one App-boot smoke test dispatched over the real
// RemoteServer/SimulatedRemoteBackend path), scaled down to this rung's
// single model and its lack of a background worker: there is no
// fetchMetadataOnce()/relayOutboxOnce() equivalent to test here, so this
// file has exactly the one case the brief calls for.

#include "polls/app/app.hpp"

#include "polls/dto/poll_dto.hpp"
#include "polls/models/poll_model.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_executor.hpp>

#include <filesystem>
#include <memory>
#include <string>

using morph::bridge::AllowShared;
using morph::bridge::Bridge;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief A fresh, empty action-log path per test. Same convention as
///        `bookmarks::app`'s own `test_app.cpp` -- `FileActionLog` rebuilds
///        its idempotency-dedup set from whatever is already on disk, so a
///        leftover file from an earlier run would silently suppress a
///        re-logged entry.
[[nodiscard]] std::filesystem::path freshLogPath(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("polls_" + name + ".jsonl");
    std::filesystem::remove(path);
    return path;
}

}  // namespace

TEST_CASE("App boots, registers PollModel, and a real client can CreatePoll/OpenPoll over it", "[polls][app]") {
    DbFixture fixture;
    const auto logPath = freshLogPath("app_boot");
    {
        polls::app::App app{logPath};

        // A real client of app.server(): SimulatedRemoteBackend routes
        // through RemoteServer::handle() -- the identical dispatch path a
        // real socket client's QtWebSocketBackend would use -- so this
        // proves the server genuinely registered "PollModel" (via
        // poll_model.hpp's BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION
        // static-init registrars, pulled into this binary through app.cpp's
        // own include) and that auth::PollsAuthorizer's permissive
        // authorizeRegister/authorizeInstance hooks genuinely admit an
        // unauthenticated caller's register and keyed attach, exactly as
        // the rung's own design intends (polls_authorizer.hpp's @file
        // comment).
        morph::qt::QtExecutor exec;
        Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*app.server())};

        // Plain (NoSharing) handler for CreatePoll -- CreatePoll carries no
        // key, so nothing about it is shared/keyed. Mirrors
        // test_poll_model.cpp's own instance-rebirth test's "creator" handler.
        BridgeHandler<polls::PollModel> creator{bridge, &exec};
        const auto created = awaitQt(
            creator.execute(polls::CreatePoll{.title = "Team offsite", .options = {{"2026-09-01"}, {"2026-09-02"}}}));
        CHECK_FALSE(created.pollId.empty());
        REQUIRE(created.adminToken.hasValue());
        REQUIRE(created.participantToken.hasValue());
        CHECK_FALSE((*created.adminToken).empty());
        CHECK_FALSE((*created.participantToken).empty());
        CHECK(*created.adminToken != *created.participantToken);

        // AllowShared handler for OpenPoll -- the keyed attach path
        // (BRIDGE_MODEL_KEY(PollModel, OpenPoll, &OpenPoll::pollId)) a real
        // participant screen uses to join the poll `creator` just made.
        BridgeHandler<polls::PollModel, AllowShared> viewer{bridge, &exec};
        const auto state = awaitQt(viewer.execute(polls::OpenPoll{.pollId = created.pollId}));
        CHECK(state.pollId == created.pollId);
        CHECK(state.title == "Team offsite");
        REQUIRE(state.options.size() == 2);
        CHECK(state.options[0].label == "2026-09-01");
        CHECK(state.options[1].label == "2026-09-02");
        CHECK(state.finalized == polls::Finalized::No);
        CHECK(state.votes.empty());
        CHECK(state.comments.empty());
    }
    std::filesystem::remove(logPath);
}
