// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "testkit/offline_rig.hpp"

#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_websocket_server.hpp>

#include <QTcpServer>

#include <chrono>

// No local QCoreApplication here: ladder_common_tests' own main()
// (testkit_main.cpp) already constructs the one QCoreApplication this whole
// binary is allowed to have -- Qt aborts ("there should be only one
// application object") if a second is constructed within the same process,
// which a TEST_CASE-local QCoreApplication would be.
TEST_CASE("OfflineRig closes and reopens the server on the same port", "[testkit][offline_rig]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::RemoteServer server{pool};

    // QtWebSocketServer::listen() takes no arguments -- the port it binds is
    // fixed once, at construction, and never updated to reflect an
    // OS-assigned value. So reviveConnection()'s "same port" guarantee only
    // holds if the port passed to the constructor is already a real,
    // concrete port -- not the "let the OS pick one" sentinel `0`. Reserve
    // one deterministically with a throwaway QTcpServer, then release it
    // immediately before QtWebSocketServer binds the real one.
    quint16 port = 0;
    {
        QTcpServer reservation;
        REQUIRE(reservation.listen(QHostAddress::LocalHost));
        port = reservation.serverPort();
    }

    morph::qt::QtWebSocketServer wsServer{server, port};
    REQUIRE(wsServer.listen());
    REQUIRE(wsServer.port() == port);

    morph::ladder::testkit::OfflineRig rig{wsServer};
    rig.dropConnection();
    CHECK(wsServer.port() == 0);

    REQUIRE(rig.reviveConnection());
    CHECK(wsServer.port() == port);

    wsServer.closeGracefully(std::chrono::milliseconds{0});
}
