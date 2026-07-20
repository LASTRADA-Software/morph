// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "test_support.hpp"

using morph::testing::WaitReply;
using morph::testing::waitUntil;

// ── morph::backend::RemoteServer::openConnection / closeConnection ──────────
// (Foundational bookkeeping only — no models are registered in this task; the
// scoped `register`/`deregister` wiring and its tests are Task 2.)

TEST_CASE("morph::backend::RemoteServer::openConnection: returns fresh non-zero ids", "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto cidA = server->openConnection();
    auto cidB = server->openConnection();
    REQUIRE(cidA != 0U);
    REQUIRE(cidB != 0U);
    REQUIRE(cidA != cidB);
}

TEST_CASE("morph::backend::RemoteServer::closeConnection: cid 0 is a no-op", "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->closeConnection(0);  // must not crash
}

TEST_CASE("morph::backend::RemoteServer::closeConnection: unknown cid is a no-op", "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->closeConnection(12345);  // never opened — must not crash
}

TEST_CASE("morph::backend::RemoteServer::closeConnection: closing a freshly opened, empty scope twice is idempotent",
          "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto cid = server->openConnection();
    server->closeConnection(cid);
    server->closeConnection(cid);  // second call: already closed, still a no-op
}
