// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <string>

/// @file
/// `App` wires the pieces every screen shares: the worker pool that runs the
/// models, the GUI executor that callbacks land on, and the single process-wide
/// `Bridge`. It also owns the database lifecycle (connection + migrations) and
/// the login/logout flow that sets the bridge's default session.
///
/// A real GUI would construct one `App`, then build a `BridgeHandler<Model>`
/// per screen against `app.bridge()` / `app.gui()`.

namespace bank::app {

/// @brief Shared application context: pool, GUI executor, bridge, and session.
class App {
public:
    /// @brief Builds the app over a local backend and sets up the database.
    ///
    /// @param connectionString ODBC string for the SQLite database.
    /// @param workers          Size of the model worker pool.
    explicit App(const std::string& connectionString, std::size_t workers = 4);

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;
    ~App() = default;

    /// @brief The shared bridge every handler registers on.
    [[nodiscard]] morph::bridge::Bridge& bridge() noexcept { return _bridge; }

    /// @brief Executor that `Completion` callbacks are delivered on.
    [[nodiscard]] morph::exec::IExecutor* gui() noexcept { return &_gui; }

    /// @brief The pumpable GUI loop (call `runFor` from a driver / event loop).
    [[nodiscard]] morph::exec::MainThreadExecutor& guiLoop() noexcept { return _gui; }

    /// @brief Sets the session principal applied to every subsequent call.
    void login(const std::string& principal);

    /// @brief Clears the session principal.
    void logout();

private:
    morph::exec::ThreadPoolExecutor _pool;
    morph::exec::MainThreadExecutor _gui;
    morph::bridge::Bridge _bridge;
};

}  // namespace bank::app
