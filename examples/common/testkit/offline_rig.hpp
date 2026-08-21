// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <morph/qt/qt_websocket_server.hpp>

/// @file
/// `OfflineRig` -- scripted connectivity drop/revive for offline-stack
/// tests: closes the in-test `QtWebSocketServer`, then reopens it on the
/// same port, driving a real `ReconnectCoordinator`/`NetworkMonitor`
/// through a genuine connect -> disconnect -> reconnect cycle rather than a
/// hand-cranked signal (`examples/TESTING.md`'s own design for this file).

namespace morph::ladder::testkit {

/// @brief Scripts a connectivity drop and revive against a real, in-test
///        `QtWebSocketServer`.
///
/// `QtWebSocketServer::listen()` takes no arguments: the port it binds is
/// fixed once, at construction (`QtWebSocketServer`'s own `port` constructor
/// argument), and every subsequent `listen()` call re-binds to that same
/// fixed port. "Revive on the same port" therefore falls out of the
/// server's own re-listen behavior for free — `OfflineRig` only needs to
/// sequence `closeGracefully()`/`listen()`, never a port value of its own.
/// A server built with the default port `0` (let the OS pick one) does
/// *not* revive on the same port with this class -- the caller must
/// construct the rigged `QtWebSocketServer` with an explicit, nonzero port
/// for `reviveConnection()`'s "same port" guarantee to hold.
class OfflineRig {
public:
    /// @brief Wraps @p server for scripted drop/revive. `server` must outlive
    ///        this `OfflineRig`.
    /// @param server The in-test server to script connectivity against.
    explicit OfflineRig(::morph::qt::QtWebSocketServer& server) : _server{server} {}

    /// @brief Closes the server, simulating a network drop. Any client
    ///        connected to it observes a real disconnect.
    ///
    /// Uses `closeGracefully()` with a zero deadline rather than `close()`:
    /// zero deadline skips straight to `closeGracefully()`'s final hard-stop
    /// step, so the effect is the same immediate close, but the graceful
    /// path's `RemoteServer::beginShutdown()` call runs first, closing this
    /// connection's server-side session state exactly once instead of
    /// leaking it across a later `close()` call from someone else (e.g. the
    /// server's own destructor).
    void dropConnection() { _server.closeGracefully(std::chrono::milliseconds{0}); }

    /// @brief Reopens the server on the port it was constructed with -- the
    ///        same port a prior `dropConnection()` was listening on, so a
    ///        reconnecting client's cached URL is still valid.
    /// @return `true` if the server successfully re-bound to that port.
    bool reviveConnection() { return _server.listen(); }

private:
    ::morph::qt::QtWebSocketServer& _server;
};

}  // namespace morph::ladder::testkit
