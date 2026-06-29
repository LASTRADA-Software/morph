// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/qt/qt_executor.hpp>

#include <QString>

#include <cstddef>
#include <string>

/// @file
/// Shared client context for the GUI: a worker pool, a `QtExecutor` that
/// delivers completion callbacks on the Qt GUI thread, the process-wide
/// `Bridge` (over a local backend), database setup, and the session principal.
/// Each view constructs its own `BridgeHandler`s against `bridge()` / `gui()`.

namespace bankgui {

/// @brief Owns the morph wiring for the GUI and the current session.
class BankClient {
public:
    explicit BankClient(const std::string& connectionString, std::size_t workers = 4);

    BankClient(const BankClient&) = delete;
    BankClient& operator=(const BankClient&) = delete;

    [[nodiscard]] morph::bridge::Bridge& bridge() noexcept { return _bridge; }
    [[nodiscard]] morph::exec::IExecutor* gui() noexcept { return &_gui; }

    /// @brief Installs @p principal / @p displayName as the session for every call.
    void login(const QString& principal, const QString& displayName);
    void logout();

    [[nodiscard]] const QString& principal() const noexcept { return _principal; }
    [[nodiscard]] const QString& displayName() const noexcept { return _displayName; }

private:
    morph::exec::ThreadPoolExecutor _pool;
    morph::qt::QtExecutor _gui;
    morph::bridge::Bridge _bridge;
    QString _principal;
    QString _displayName;
};

}  // namespace bankgui
