// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>

#include <QUrl>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

/// @file
/// Backend-parameterized app context (examples/TESTING.md, "Presenter
/// architecture" rule 2). Replaces bank's hard-wired LocalBackend
/// (gui/BankClient.cpp) with one type presenters can be built against
/// regardless of deployment mode.
///
/// This header lives in its own link target, `morph_ladder_app`
/// (`morph::ladder_app`), rather than in `morph_ladder_gui`: `Remote` mode
/// needs `morph::qt`/`morph_qt_impl` and transitively `Qt6::WebSockets`,
/// while `morph_ladder_gui` (presenters) is `Qt6::Core`-only by rule
/// (examples/TESTING.md, "Presenter architecture" rule 1 — presenters must
/// instantiate under a plain `QCoreApplication`). A rung's `gui_lib` links
/// `morph::ladder_gui`; its `gui`/`gui_wasm`/`tests` shells, which are the
/// things that actually choose a deployment mode, additionally link
/// `morph::ladder_app`.

namespace morph::ladder::gui {

/// @brief In-process backend, @p workers threads.
struct Local {
    std::size_t workers = 4;
};

/// @brief Remote backend over `QtWebSocketBackend` at @p url.
///
/// @warning Asynchronously connected. A `Remote` context is **not** usable
///          the line after its constructor returns — see `AppContext`'s
///          readiness contract (`ready()`/`onReady()`) below.
struct Remote {
    QUrl url;
};

/// @brief Owns, in destruction-safe order (worker pool -> executor -> bridge,
///        declared in reverse), everything a presenter set needs and nothing
///        a presenter should construct itself.
///
/// @par Readiness contract (why `Remote` mode still defers registration)
/// `Local` mode has no network dependency: `ready()` is `true` the moment the
/// constructor returns and `onReady()` invokes its callback synchronously.
///
/// `Remote` mode builds its `QtWebSocketBackend` with
/// `Config{.asyncRegistrationEnabled = true}` (the plain synchronous
/// `registerModel` nests a `QEventLoop` and aborts a WASM page —
/// examples/TESTING.md, "WASM reality"). `QtWebSocketBackend::
/// registerModelAsync()` queues a registration issued before the socket
/// has finished connecting and retries it once the connection comes up
/// (`docs/spec/core/backend.md`, "Asynchronous registration"), so
/// building a `BridgeHandler` immediately after this constructor returns is
/// no longer the correctness hazard it once was.
///
/// This class still detects readiness with `setConnectHandler` — not
/// `waitForConnected()`, which nests an event loop and hangs a WASM page —
/// and callers build their presenters (and therefore their `BridgeHandler`s)
/// from inside `onReady()`:
///
/// ```cpp
/// AppContext ctx{Remote{url}};
/// ctx.onReady([&] { presenters.emplace(ctx.bridge(), ctx.executor()); });
/// ```
///
/// This is the same ordering `examples/common/wasm_spike/main_wasm.cpp`
/// demonstrates end-to-end — deferring to `onReady()` is simpler to reason
/// about than relying on the pre-connect queue, not a requirement for
/// correctness.
class AppContext {
  public:
    using Mode = std::variant<Local, Remote>;

    /// @brief Builds the backend/bridge/executor set for @p mode.
    /// @param mode Deployment shape: `Local{workers}` or `Remote{url}`.
    explicit AppContext(Mode mode);

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
    AppContext(AppContext&&) = delete;
    AppContext& operator=(AppContext&&) = delete;
    ~AppContext() = default;

    /// @brief The bridge every handler in this context is built against.
    /// @return Reference to the owned `Bridge`.
    [[nodiscard]] ::morph::bridge::Bridge& bridge() { return *_bridge; }

    /// @brief The Qt-thread executor every handler delivers callbacks on.
    /// @return Non-owning pointer to the owned `QtExecutor`.
    [[nodiscard]] ::morph::exec::IExecutor* executor() { return _qtExecutor.get(); }

    /// @brief Whether the transport is up and handlers may now be built.
    ///
    /// Always `true` for `Local` (no transport to wait for). For `Remote`,
    /// `false` until the WebSocket's first successful connect — see the
    /// class doc comment's readiness contract.
    /// @return `true` once `BridgeHandler` construction against `bridge()`
    ///         is safe.
    [[nodiscard]] bool ready() const noexcept { return _ready; }

    /// @brief Runs @p callback once the context is ready.
    ///
    /// Invoked immediately (synchronously, before returning) if `ready()` is
    /// already `true` — which is always the case in `Local` mode. Otherwise
    /// queued and invoked exactly once, from the backend's connect handler,
    /// on the Qt event-loop thread. Registering several callbacks runs them
    /// in registration order.
    ///
    /// @param callback Work to run once handlers may be built — typically
    ///        the construction of this context's presenters.
    void onReady(std::function<void()> callback);

    /// @brief Sets the default session principal every handler built against
    ///        this context's bridge dispatches under.
    /// @param principal Auth principal (user id) — becomes
    ///        `session::Context::principal` in the bridge's default session
    ///        (see `include/morph/session/session.hpp`).
    void login(const std::string& principal);

  private:
    /// @brief Flips `ready()` and drains the queued `onReady()` callbacks.
    void markReady();

    std::unique_ptr<::morph::exec::ThreadPoolExecutor> _workerPool;  // Local only
    std::unique_ptr<::morph::qt::QtExecutor> _qtExecutor;
    std::unique_ptr<::morph::bridge::Bridge> _bridge;
    bool _ready{false};
    std::vector<std::function<void()>> _pendingReadyCallbacks;
};

}  // namespace morph::ladder::gui
