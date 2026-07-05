// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// QML-facing controller for the schema-driven forms demo.
///
/// The QML layer is deliberately a *JSON-speaking client*: it renders forms
/// from the schemas exposed here and submits action bodies as JSON strings.
/// This controller wraps them in `morph::wire::Envelope`s and feeds them to
/// an in-process `RemoteServer` — the exact protocol a networked deployment
/// would run over a WebSocket, minus the socket. Swapping to
/// `QtWebSocketBackend`/`QtWebSocketServer` changes this file, not the QML.

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include <morph/executor.hpp>
#include <morph/remote.hpp>

class FormsController : public QObject {
    Q_OBJECT
    QML_ELEMENT

    /// @brief `{actionType: schema}` JSON — everything the QML renderer needs.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

public:
    explicit FormsController(QObject* parent = nullptr);

    [[nodiscard]] QString schemasJson() const;

    /// @brief Sends one `execute` envelope with @p bodyJson as the action payload.
    ///        The reply arrives via `replyReceived` on the GUI thread.
    Q_INVOKABLE void submit(const QString& actionType, const QString& bodyJson);

signals:
    /// @brief Emitted once per `submit`. @p payload is the result JSON when
    ///        @p ok, otherwise the server's error message.
    void replyReceived(const QString& actionType, bool ok, const QString& payload);

private:
    std::shared_ptr<morph::backend::RemoteServer> _server;
    std::atomic<std::uint64_t> _modelId{0};
    std::atomic<std::uint64_t> _nextCallId{1};

    /// @brief Declared last, so it is destroyed first: the pool joins (running
    ///        any still-queued reply callbacks, which touch the members above)
    ///        while those members are all still alive. GUI-thread hops queued
    ///        on a destroyed controller are dropped by Qt's context tracking.
    morph::exec::ThreadPoolExecutor _pool{2};
};
