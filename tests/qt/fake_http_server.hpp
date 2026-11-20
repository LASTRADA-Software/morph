// SPDX-License-Identifier: Apache-2.0
#pragma once

// A local server that speaks enough HTTP for one request and one canned answer, shared by the Qt provider tests. No
// network.

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace qt_test {

inline bool pumpUntil(std::function<bool()> const& done, std::chrono::milliseconds limit = std::chrono::seconds{10}) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

// Answers the first whole request with a canned response, and keeps what it was sent.
class FakeHttpServer {
public:
    explicit FakeHttpServer(std::string response) : _response{std::move(response)} {
        REQUIRE(_server.listen(QHostAddress::LocalHost));
        QObject::connect(&_server, &QTcpServer::newConnection, &_server, [this] {
            while (QTcpSocket* const socket = _server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { read(*socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    disconnected = true;
                    socket->deleteLater();
                });
            }
        });
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(static_cast<unsigned>(_server.serverPort()));
    }

    // Whether the whole request arrived and the canned response went out.
    [[nodiscard]] bool answered() const { return _answered; }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): what a test sets and reads directly.
    // Hold the response's end, so the test can act during the call.
    bool holdOpen = false;
    bool disconnected = false;
    std::string received;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    void read(QTcpSocket& socket) {
        received += socket.readAll().toStdString();
        auto const headersEnd = received.find("\r\n\r\n");
        if (headersEnd == std::string::npos) {
            return;
        }
        auto const at = received.find("content-length: ");
        auto const atCapital = received.find("Content-Length: ");
        auto const start = at != std::string::npos ? at : atCapital;
        REQUIRE(start != std::string::npos);
        std::size_t const length = std::stoul(received.substr(start + 16));
        if (received.size() < headersEnd + 4 + length || _answered) {
            return;
        }
        _answered = true;
        socket.write(_response.data(), static_cast<qint64>(_response.size()));
        socket.flush();
        if (!holdOpen) {
            socket.disconnectFromHost();
        }
    }

    QTcpServer _server;
    std::string _response;
    bool _answered = false;
};

}  // namespace qt_test
